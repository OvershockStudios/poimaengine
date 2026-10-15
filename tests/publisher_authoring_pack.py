#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable publisher planning/publication tests with synthetic, non-executable tools.

These exercise the real publisher and pack validator while replacing every
command runner. They do not compile, load or execute .NET/native payloads.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import authoring_pack

SPEC = importlib.util.spec_from_file_location('publisher_pack_under_test', ROOT/'scripts/publish_native_gameplay.py')
publish = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class PublisherPack(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.pack = self.base/'relocated authoring & tools'
        self.project = self.base/'external game'/'Demo.csproj'
        self.project.parent.mkdir()
        self.project.write_text('<Project/>', encoding='utf-8')
        self.work, self.output = self.base/'owned work', self.base/'artifact'
        self.checkout = self.base/'unavailable-checkout'
        self.sdk = b'synthetic SDK identity; not an assembly'
        rows = []
        for name, role in sorted(authoring_pack.PAYLOAD_ROLES.items()):
            data = self.sdk if role == 'sdk' else ('synthetic resource '+name).encode()
            path = self.pack/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            rows.append(dict(path=name, size=len(data), sha256=digest(data), role=role))
        self.manifest = self.pack/'authoring.json'
        self.manifest.write_text(json.dumps(dict(format='poima.authoring-tools', version=1,
            engine_version='2.3.4', framework='net10.0', language='14.0', files=rows))+'\n', encoding='utf-8')
        self.rid = 'win-x64' if os.name == 'nt' else 'linux-x64'
        self.library_name = 'Poima.NativeGame.dll' if os.name == 'nt' else 'Poima.NativeGame.so'
        self.commands, self.copies = [], []
        self.after = None
        self.sdk_output = self.sdk
        self.extra_payload = False
        self.assets_override = None
        self.packages = self.base/'nuget'
        for family in ('Microsoft.NETCore.App.Runtime.', 'Microsoft.NETCore.App.Runtime.NativeAOT.'):
            directory = self.packages/(family+self.rid).lower()/'10.0.1'
            directory.mkdir(parents=True)
            for name in ('LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT'):
                (directory/name).write_text('synthetic runtime notice '+family+name, encoding='utf-8')

    def fake_run(self, command, cwd, *, capture=False):
        command = [str(value) for value in command]
        self.commands.append(dict(argv=command, cwd=Path(cwd), capture=capture))
        operation = command[1]
        result = None
        event = None
        if operation == 'build':
            output = Path(command[command.index('-o')+1]); output.mkdir(parents=True)
            if Path(command[2]) == self.project:
                (output/'Demo.dll').write_bytes(b'synthetic game assembly')
                (output/'Poima.Gameplay.dll').write_bytes(self.sdk_output)
                event = 'game_build'
            else:
                self.assertEqual(Path(command[2]), self.checkout/'managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj')
                (output/'Poima.NativeGame.Generator.dll').write_bytes(b'synthetic generator')
                event = 'generator_build'
        elif operation == 'msbuild':
            if '-getProperty:AssemblyName' in command:
                result = 'Demo'; event = 'assembly_name'
            else:
                self.assertIn('-getProperty:ProjectAssetsFile', command)
                result = str(self.assets_override or self.assets_path); event = 'assets_query'
        elif operation.endswith('Poima.NativeGame.Generator.dll'):
            directory = Path(command[-1]); directory.mkdir(parents=True)
            (directory/'schema.json').write_text(json.dumps(dict(identity='synthetic.pack-game',
                components=[dict(version=2, id='1'*32)])), encoding='utf-8')
            (directory/'requirements.json').write_text(json.dumps(dict(call_version=1, call_bytes=80,
                services_version=7, services_bytes=176, features=['baseline_v7'])), encoding='utf-8')
            (directory/'game.poima-components.json').write_text('{"synthetic":true}', encoding='utf-8')
            event = 'generate'
        elif operation == 'publish':
            directory = Path(command[command.index('-o')+1]); directory.mkdir(parents=True)
            (directory/self.library_name).write_bytes(b'synthetic native image; not executable')
            if self.extra_payload: (directory/'unlisted-runtime.dll').write_bytes(b'additional dependency')
            if '--artifacts-path' in command:
                self.assets_path = Path(command[command.index('--artifacts-path')+1])/'obj'/'NativeHost'/'project.assets.json'
            else:
                self.assets_path = Path(command[2]).parent/'obj'/'project.assets.json'
            self.assets_path.parent.mkdir(parents=True)
            self.assets_path.write_text(json.dumps(dict(packageFolders={str(self.packages): {}}, project=dict(
                frameworks={'net10.0': dict(downloadDependencies=[dict(name=family+self.rid,
                    version='[10.0.1, 10.0.1]') for family in
                    ('Microsoft.NETCore.App.Runtime.', 'Microsoft.NETCore.App.Runtime.NativeAOT.')])}))), encoding='utf-8')
            event = 'publish'
        else:
            self.fail('Unexpected command: '+repr(command))
        if self.after is not None: self.after(event)
        return result

    def invoke(self, *, installed=True, expected=None, engine_version=None, work=None, output=None):
        args = ['publisher', '--project', str(self.project), '--type', 'Synthetic.Game', '--output',
                str(output or self.output), '--work', str(work or self.work), '--dotnet', 'synthetic-dotnet', '--rid', self.rid]
        if installed: args += ['--authoring-pack', str(self.pack)]
        if expected is not None: args += ['--authoring-sha256', expected]
        if engine_version is not None: args += ['--engine-version', engine_version]
        with (patch.object(publish, 'ROOT', self.checkout), patch.object(publish, 'run', side_effect=self.fake_run),
              patch.object(publish.platform, 'machine', return_value='x86_64'), patch.object(sys, 'argv', args),
              contextlib.redirect_stdout(io.StringIO()),
              patch.object(publish.subprocess, 'run', side_effect=AssertionError('Real process forbidden'))):
            publish.main()

    def descriptor(self):
        return json.loads((self.output/'native-gameplay.json').read_text(encoding='utf-8'))

    def test_installed_commands_use_no_checkout_or_internal_generator_build(self):
        expected = digest(self.manifest.read_bytes())
        before = {p.relative_to(self.pack).as_posix():p.read_bytes() for p in self.pack.rglob('*') if p.is_file()}
        self.invoke(expected=expected)
        self.assertFalse(self.checkout.exists())
        self.assertEqual(before, {p.relative_to(self.pack).as_posix():p.read_bytes() for p in self.pack.rglob('*') if p.is_file()})
        self.assertEqual([c['argv'][1] for c in self.commands if c['argv'][1] in ('build','publish')], ['build','publish'])
        for row in self.commands:
            self.assertEqual(row['cwd'], self.project.parent)
            self.assertNotIn(str(self.checkout), '\n'.join(row['argv']))
        builds = [c['argv'] for c in self.commands if c['argv'][1] in ('build','publish')]
        for command in builds:
            artifact_root = Path(command[command.index('--artifacts-path')+1])
            self.assertTrue(artifact_root.is_relative_to(self.work))
            self.assertIn('--disable-build-servers', command)
            self.assertIn('-nodeReuse:false', command)
            self.assertIn('-p:UseSharedCompilation=false', command)
        self.assertNotEqual(builds[0][builds[0].index('--artifacts-path')+1], builds[1][builds[1].index('--artifacts-path')+1])
        queries = [c['argv'] for c in self.commands if c['argv'][1] == 'msbuild']
        for command in queries:
            self.assertIn('-p:Configuration=Release', command)
        self.assertIn('-p:RuntimeIdentifier='+self.rid, queries[-1])
        generator = next(c['argv'] for c in self.commands if c['argv'][1].endswith('Poima.NativeGame.Generator.dll'))
        self.assertEqual(Path(generator[1]), self.pack/'tools/native-generator/Poima.NativeGame.Generator.dll')
        document = ET.parse(self.work/'generated/Poima.NativeGame.csproj').getroot()
        self.assertEqual(document.find('.//ProjectReference').get('Include'), str(self.project))
        self.assertEqual(document.find('.//Compile').get('Include'), str(self.pack/'native/Entry.cs'))
        self.assertEqual(document.find('.//HintPath').text, str(self.work/'game/Poima.Gameplay.dll'))
        descriptor = self.descriptor()
        self.assertEqual(descriptor['engine_version'], '2.3.4')
        self.assertIn('component_collections_v1', descriptor['required_features'])
        self.assertFalse((self.output/'.poima-publishing').exists())
        actual = {p.relative_to(self.output).as_posix() for p in self.output.rglob('*') if p.is_file()}
        self.assertEqual(actual, {'native-gameplay.json', *(row['path'] for row in descriptor['files'])})
        for row in descriptor['files']:
            data = (self.output/row['path']).read_bytes()
            self.assertEqual((row['size'],row['sha256']), (len(data),digest(data)))
        self.assertEqual((self.output/'notices/POIMA-LICENSE.txt').read_bytes(), (self.pack/'notices/POIMA-LICENSE.txt').read_bytes())
        self.assertEqual(len([p for p in (self.output/'notices').iterdir() if p.name.startswith('DOTNET-')]), 4)

    def test_explicit_target_version_remains_selectable(self):
        self.invoke(engine_version='7.8.9')
        self.assertEqual(self.descriptor()['engine_version'], '7.8.9')

    def test_hash_without_pack_and_mismatched_hash_reject_before_build(self):
        for installed, expected in ((False,'0'*64),(True,'0'*64),(True,'bad')):
            with self.subTest(installed=installed,expected=expected), self.assertRaises(ValueError):
                self.invoke(installed=installed, expected=expected)
            self.assertEqual(self.commands, [])
            self.assertFalse(self.work.exists()); self.assertFalse(self.output.exists())

    def test_pack_project_and_work_output_overlap_reject_before_build(self):
        for output,work in ((self.pack/'output',self.work), (self.output,self.pack/'work'),
                            (self.project.parent/'output',self.work), (self.output,self.project.parent/'work'),
                            (self.work/'output',self.work), (self.output,self.output/'work')):
            with self.subTest(output=output,work=work), self.assertRaises(ValueError):
                self.invoke(output=output, work=work)
            self.assertEqual(self.commands, [])
            self.assertFalse(self.work.exists()); self.assertFalse(self.output.exists())

    def test_wrong_built_sdk_rejects_before_prebuilt_generator(self):
        self.sdk_output = bytes([self.sdk[0]^1])+self.sdk[1:]
        with self.assertRaisesRegex(ValueError, 'exactly match'):
            self.invoke()
        self.assertEqual([c['argv'][1] for c in self.commands], ['build','msbuild'])
        self.assertFalse(self.output.exists())

    def test_each_tool_boundary_reverifies_pack(self):
        for event in ('game_build','generate','publish','assets_query'):
            with self.subTest(event=event):
                # Each subcase has an independent pack/work/output.
                child = type(self)('runTest'); child.setUp()
                try:
                    def mutate(actual):
                        if actual == event:
                            path = child.pack/'native/Entry.cs'
                            path.write_bytes(path.read_bytes()+b'changed')
                    child.after = mutate
                    with self.assertRaises(ValueError): child.invoke()
                    self.assertFalse(child.output.exists())
                    if event == 'game_build':
                        self.assertFalse(any(c['argv'][1].endswith('Poima.NativeGame.Generator.dll') for c in child.commands))
                    if event == 'generate': self.assertFalse(any(c['argv'][1]=='publish' for c in child.commands))
                finally: child.doCleanups()

    def test_pack_mutation_during_final_copy_keeps_invalid_marker_and_manifest_last(self):
        original = shutil.copyfileobj
        def copy(incoming, outgoing, *args, **kwargs):
            destination = Path(outgoing.name)
            if destination.is_relative_to(self.output):
                self.copies.append(destination.relative_to(self.output).as_posix())
                path = self.pack/'analyzers/Poima.Components.Generator.dll'
                if len(self.copies) == 1: path.write_bytes(path.read_bytes()+b'changed')
            return original(incoming,outgoing,*args,**kwargs)
        with patch.object(publish.shutil,'copyfileobj',side_effect=copy), self.assertRaises(ValueError):
            self.invoke()
        self.assertTrue((self.output/'.poima-publishing').is_file())
        self.assertEqual(self.copies[-1], 'native-gameplay.json')

    def test_assets_metadata_must_resolve_inside_owned_work(self):
        self.assets_override = self.base/'external-assets.json'
        self.assets_override.write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'inside installed publication work'):
            self.invoke()
        self.assertFalse(self.output.exists())

    def test_extra_publish_dependency_and_missing_dotnet_notice_remain_rejected(self):
        self.extra_payload = True
        with self.assertRaisesRegex(ValueError, 'Additional publish payloads'):
            self.invoke()
        self.assertFalse(self.output.exists())
        shutil.rmtree(self.work); self.extra_payload = False
        next(self.packages.rglob('LICENSE.TXT')).unlink()
        with self.assertRaisesRegex(ValueError, 'runtime notice is missing'):
            self.invoke()
        self.assertFalse(self.output.exists())

    def test_existing_output_remains_untouched(self):
        self.output.mkdir(); (self.output/'sentinel').write_bytes(b'owned by caller')
        with self.assertRaisesRegex(ValueError, 'Output already exists'):
            self.invoke()
        self.assertEqual((self.output/'sentinel').read_bytes(), b'owned by caller')
        self.assertEqual(self.commands, [])

    def test_omitted_pack_preserves_checkout_build_cwd_entry_and_notices(self):
        for name,data in {'CMakeLists.txt':'project(poima VERSION 1.2.3)', 'LICENSE':'checkout license',
                          'THIRD_PARTY_NOTICES.md':'checkout notices', 'managed/Poima.NativeGame/Entry.cs':'checkout entry'}.items():
            path=self.checkout/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(data,encoding='utf-8')
        self.invoke(installed=False)
        self.assertEqual(self.descriptor()['engine_version'], '1.2.3')
        self.assertEqual([c['argv'][1] for c in self.commands], ['build','msbuild','build',str(self.work/'generator/Poima.NativeGame.Generator.dll'),'publish'])
        for row in self.commands:
            self.assertEqual(row['cwd'], self.checkout)
            self.assertNotIn('--artifacts-path', row['argv'])
        self.assertEqual(ET.parse(self.work/'generated/Poima.NativeGame.csproj').find('.//Compile').get('Include'),
                         str(self.checkout/'managed/Poima.NativeGame/Entry.cs'))
        self.assertEqual((self.output/'notices/POIMA-LICENSE.txt').read_text(), 'checkout license')


if __name__ == '__main__':
    unittest.main()
