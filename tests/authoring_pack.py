#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable fixed-pack boundary and builder tests; no .NET/native process runs."""
from dataclasses import FrozenInstanceError
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

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result

pack = module('poima_pack_contract_impl', ROOT/'scripts/authoring_pack.py')
with patch.dict(sys.modules, authoring_pack=pack):
    builder = module('poima_pack_builder_contract_impl', ROOT/'scripts/package_authoring.py')


def digest(data): return hashlib.sha256(data).hexdigest()


def fixture(root):
    for name in pack.PAYLOAD_ROLES:
        path = root/name
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = b'original sdk' if name.endswith('Poima.Gameplay.dll') else ('original '+name).encode()
        path.write_bytes(payload)
    return manifest(root)


def manifest(root):
    value = dict(format='poima.authoring-tools', version=1, engine_version='0.0.162',
                 framework='net10.0', language='14.0', files=[])
    for name, role in sorted(pack.PAYLOAD_ROLES.items()):
        data = (root/name).read_bytes()
        value['files'].append(dict(path=name, role=role, size=len(data), sha256=digest(data)))
    save(root, value)
    return value


def save(root, value):
    (root/'authoring.json').write_bytes((json.dumps(value, indent=2)+'\n').encode())


class PackContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='poima-pack-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base/'pack'
        self.value = fixture(self.root)
        self.raw = (self.root/'authoring.json').read_bytes()

    def test_exact_pack_frozen_api_and_relocation(self):
        original = pack.load_pack(self.root, expected_sha256=digest(self.raw))
        self.assertEqual(original.engine_version, '0.0.162')
        self.assertEqual(original.manifest_sha256, digest(self.raw))
        self.assertEqual(original.root, self.root)
        self.assertIsNone(original.verify())
        for name in pack.PAYLOAD_ROLES:
            self.assertEqual(original.resource(name), self.root/name)
        with self.assertRaises(FrozenInstanceError): original.engine_version = '1.0.0'
        for name in ('../authoring.json', 'authoring.json', 'sdk/../native/Entry.cs', '/native/Entry.cs', 'native\\Entry.cs'):
            with self.subTest(resource=name), self.assertRaises(ValueError): original.resource(name)
        moved = self.base/'relocated'
        shutil.copytree(self.root, moved)
        shutil.rmtree(self.root)
        installed = pack.load_pack(moved)
        self.assertEqual(installed.manifest_sha256, original.manifest_sha256)
        self.assertIsNone(installed.verify())

    def test_closed_typed_manifest_and_fixed_policy(self):
        mutations = [lambda v:v.update(extra=True), lambda v:v.update(version=True),
                     lambda v:v.update(version=2), lambda v:v.update(format='other'),
                     lambda v:v.update(framework='net9.0'), lambda v:v.update(language='preview'),
                     lambda v:v.update(engine_version='1.2'), lambda v:v.update(engine_version='١.2.3'),
                     lambda v:v.update(engine_version='1.2.3\n'), lambda v:v.update(engine_version='1'*129+'.2.3'),
                     lambda v:v.update(files={}), lambda v:v['files'].pop(),
                     lambda v:v['files'][0].update(extra='unknown'), lambda v:v['files'][0].update(size=True),
                     lambda v:v['files'][0].update(size=1.0), lambda v:v['files'][0].update(size=0),
                     lambda v:v['files'][0].update(size=pack.FILE_LIMIT+1),
                     lambda v:v['files'][0].update(role='sdk'), lambda v:v['files'][0].update(sha256='A'*64)]
        for index, mutate in enumerate(mutations):
            value = json.loads(self.raw); mutate(value); save(self.root,value)
            with self.subTest(case=index), self.assertRaises(ValueError): pack.load_pack(self.root)
        for invalid in [b'{"version":1,"version":1}', self.raw.replace(b'"version": 1', b'"version": NaN'),
                        self.raw.replace(b'"size": ', b'"size": 1, "size": ',1), b'\xff', b'[]']:
            (self.root/'authoring.json').write_bytes(invalid)
            with self.subTest(raw=invalid[:32]), self.assertRaises(ValueError): pack.load_pack(self.root)

    def test_exact_order_membership_and_path_aliases(self):
        names = ['../sdk/Poima.Gameplay.dll','/sdk/Poima.Gameplay.dll','sdk//Poima.Gameplay.dll',
                 'sdk/./Poima.Gameplay.dll','sdk/../sdk/Poima.Gameplay.dll','sdk\\Poima.Gameplay.dll',
                 'SDK/Poima.Gameplay.dll','C:/sdk/Poima.Gameplay.dll','sdk/Poima.Gameplay.dll.']
        for name in names:
            value=json.loads(self.raw);value['files'][0]['path']=name;save(self.root,value)
            with self.subTest(path=name), self.assertRaises(ValueError):pack.load_pack(self.root)
        for order in ('reversed','duplicate'):
            value=json.loads(self.raw)
            if order=='reversed':value['files'].reverse()
            else:value['files'][-1]=value['files'][0].copy()
            save(self.root,value)
            with self.subTest(order=order), self.assertRaises(ValueError):pack.load_pack(self.root)

    def test_manifest_file_and_total_bounds_before_hashing(self):
        (self.root/'authoring.json').write_bytes(self.raw+b' '*(pack.MANIFEST_LIMIT+1))
        with self.assertRaises(ValueError):pack.load_pack(self.root)
        save(self.root,self.value)
        path=self.root/'native/Entry.cs'
        with path.open('wb') as stream:stream.truncate(pack.FILE_LIMIT+1)
        with self.assertRaises(ValueError):pack.load_pack(self.root)
        path.write_bytes(b'')
        with self.assertRaises(ValueError):pack.load_pack(self.root)
        value=json.loads(self.raw)
        for entry in value['files']:entry['size']=pack.FILE_LIMIT
        save(self.root,value)
        with self.assertRaisesRegex(ValueError,'total'):pack.load_pack(self.root)

    def test_verify_binds_manifest_bytes_payload_and_membership(self):
        admitted=pack.load_pack(self.root)
        for action in ('manifest','payload','extra_file','extra_directory'):
            with self.subTest(action=action):
                if action=='manifest':(self.root/'authoring.json').write_bytes(self.raw+b'\n')
                elif action=='payload':(self.root/'native/Entry.cs').write_bytes(b'wrong')
                elif action=='extra_file':(self.root/'extra').write_bytes(b'x')
                else:(self.root/'extra').mkdir()
                with self.assertRaises(ValueError):admitted.verify()
                if action=='manifest':(self.root/'authoring.json').write_bytes(self.raw)
                elif action=='payload':(self.root/'native/Entry.cs').write_bytes(b'original native/Entry.cs')
                elif action=='extra_file':(self.root/'extra').unlink()
                else:(self.root/'extra').rmdir()
        admitted.verify()

    def test_sdk_duplicates_must_have_identical_bytes(self):
        (self.root/'tools/native-generator/Poima.Gameplay.dll').write_bytes(b'different sdk')
        manifest(self.root)
        with self.assertRaisesRegex(ValueError,'SDK copies'):pack.load_pack(self.root)

    def test_expected_hash_types_and_real_mismatch(self):
        for expected in [True, 123, '', 'A'*64, '0'*64]:
            with self.subTest(expected=expected), self.assertRaises(ValueError):pack.load_pack(self.root,expected_sha256=expected)

    def test_payload_hardlinks_are_rejected(self):
        target=self.root/'native/Entry.cs';external=self.base/'external'
        try:os.link(target,external)
        except (OSError,NotImplementedError) as error:self.skipTest(str(error))
        with self.assertRaises(ValueError):pack.load_pack(self.root)

    def test_manifest_hardlinks_are_rejected(self):
        try:os.link(self.root/'authoring.json',self.base/'manifest-copy')
        except (OSError,NotImplementedError) as error:self.skipTest(str(error))
        with self.assertRaises(ValueError):pack.load_pack(self.root)

    def test_symlink_payload_and_root_ancestor_are_rejected(self):
        target=self.root/'native/Entry.cs';retained=self.base/'retained-entry';target.rename(retained)
        try:target.symlink_to(retained)
        except (OSError,NotImplementedError) as error:self.skipTest(str(error))
        with self.assertRaises(ValueError):pack.load_pack(self.root)
        target.unlink();retained.rename(target)
        linked=self.base/'alias'
        linked.symlink_to(self.root,target_is_directory=True)
        with self.assertRaises(ValueError):pack.load_pack(linked)
        with self.assertRaises(ValueError):pack.load_pack(linked/'tools'/'..')

    def test_cli_only_validates(self):
        with patch.object(sys,'argv',['authoring_pack.py',str(self.root)]),patch('sys.stdout',new_callable=io.StringIO) as output:
            pack.main()
        result=json.loads(output.getvalue())
        self.assertEqual(result,dict(engine_version='0.0.162',manifest_sha256=digest(self.raw),files=13))


class BuilderContract(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='poima-pack-builder-')
        self.addCleanup(self.temporary.cleanup)
        self.base=Path(self.temporary.name).resolve();self.checkout=self.base/'checkout';self.checkout.mkdir()
        for name in ['managed/Poima.NativeGame/Entry.cs','scripts/publish_native_gameplay.py',
                     'scripts/authoring_pack.py','LICENSE','THIRD_PARTY_NOTICES.md']:
            p=self.checkout/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(('original '+name).encode())
        (self.checkout/'CMakeLists.txt').write_bytes(b'project(Poima VERSION 0.0.162 LANGUAGES C CXX)\n')
        self.root_patch=patch.object(builder,'ROOT',self.checkout);self.root_patch.start();self.addCleanup(self.root_patch.stop)
        self.commands=[]

    def fake_build(self,command,**options):
        self.commands.append((command,options))
        output=Path(command[command.index('-o')+1]);output.mkdir()
        if 'generator-output'==output.name:
            for name in ['Poima.NativeGame.Generator.dll','Poima.NativeGame.Generator.deps.json',
                         'Poima.NativeGame.Generator.runtimeconfig.json','Poima.Gameplay.dll']:
                (output/name).write_bytes(('built '+name).encode())
        else:(output/'Poima.Components.Generator.dll').write_bytes(b'built analyzer')

    def build(self,version=None):
        return builder.package_authoring(Path(sys.executable),self.base/'pack',self.base/'work',version)

    def test_real_builder_assembly_commands_closure_and_relocatable_xml(self):
        before={p.relative_to(self.checkout).as_posix():p.read_bytes() for p in self.checkout.rglob('*') if p.is_file()}
        with patch.object(builder.subprocess,'run',side_effect=self.fake_build):result=self.build()
        self.assertEqual(len(self.commands),2);result.verify()
        self.assertEqual(result.engine_version,'0.0.162')
        paths=[]
        for command,options in self.commands:
            self.assertEqual(command[1],'build');self.assertTrue(options['check']);self.assertEqual(options['cwd'],self.base/'work')
            self.assertIn('--disable-build-servers',command);self.assertIn('-nodeReuse:false',command);self.assertIn('-p:UseSharedCompilation=false',command)
            artifacts=Path(command[command.index('--artifacts-path')+1]);paths.append(artifacts)
            self.assertEqual(artifacts.parent,self.base/'work')
            self.assertIn('-p:RestoreConfigFile='+str(self.base/'work/NuGet.Config'),command)
        self.assertNotEqual(*paths)
        self.assertNotIn('<add ',(self.base/'work/NuGet.Config').read_text())
        self.assertIn('<clear />',(self.base/'work/NuGet.Config').read_text())
        self.assertEqual(before,{p.relative_to(self.checkout).as_posix():p.read_bytes() for p in self.checkout.rglob('*') if p.is_file()})
        props=ET.parse(result.resource('build/Poima.Gameplay.props')).getroot()
        self.assertEqual(props.findtext('./PropertyGroup/TargetFramework'),'net10.0')
        self.assertEqual(props.findtext('./PropertyGroup/LangVersion'),'14.0')
        self.assertEqual(props.findtext('./ItemGroup/Reference/Private'),'true')
        self.assertEqual(props.findtext('./ItemGroup/Reference/HintPath'),'$(MSBuildThisFileDirectory)../sdk/Poima.Gameplay.dll')
        targets=ET.parse(result.resource('build/Poima.Components.targets')).getroot()
        self.assertEqual(targets.find('./ItemGroup/Analyzer').attrib['Include'],'$(MSBuildThisFileDirectory)../analyzers/Poima.Components.Generator.dll')
        self.assertIsNone(props.find('.//ProjectReference'));self.assertIsNone(targets.find('.//ProjectReference'))
        manifest_bytes=(result.root/'authoring.json').read_bytes()
        moved=self.base/'installed';shutil.copytree(result.root,moved);shutil.rmtree(self.checkout);shutil.rmtree(self.base/'work');shutil.rmtree(result.root)
        installed=pack.load_pack(moved);installed.verify();self.assertEqual((moved/'authoring.json').read_bytes(),manifest_bytes)
        self.assertEqual(installed.resource('sdk/Poima.Gameplay.dll').read_bytes(),installed.resource('tools/native-generator/Poima.Gameplay.dll').read_bytes())

    def test_unknown_build_dependency_fails_without_publishing_manifest(self):
        def extra(command,**options):
            self.fake_build(command,**options)
            if Path(command[command.index('-o')+1]).name=='generator-output':
                (self.base/'work/generator-output/Unexpected.dll').write_bytes(b'unpackaged dependency')
        with patch.object(builder.subprocess,'run',side_effect=extra),self.assertRaisesRegex(ValueError,'Additional build dependency'):self.build()
        self.assertFalse((self.base/'pack').exists());self.assertTrue((self.base/'work').is_dir())

    def test_missing_output_and_compiler_failure_preserve_work(self):
        with patch.object(builder.subprocess,'run',return_value=None),self.assertRaises(ValueError):self.build()
        self.assertTrue((self.base/'work').is_dir());self.assertFalse((self.base/'pack').exists())

    def test_compiler_error_preserves_work_without_publishing(self):
        failure = builder.subprocess.CalledProcessError(7, ['selected-dotnet', 'build'])
        with patch.object(builder.subprocess,'run',side_effect=failure) as process, self.assertRaises(builder.subprocess.CalledProcessError):
            self.build()
        self.assertEqual(process.call_count,1)
        self.assertTrue((self.base/'work/NuGet.Config').is_file())
        self.assertFalse((self.base/'pack').exists())

    def test_source_changed_after_admission_cannot_publish(self):
        original = builder._copy
        def change(source,destination,expected):
            if source.name=='Entry.cs':source.write_bytes(b'changed after admission')
            return original(source,destination,expected)
        with patch.object(builder.subprocess,'run',side_effect=self.fake_build),patch.object(builder,'_copy',side_effect=change),self.assertRaises(ValueError):
            self.build()
        self.assertTrue((self.base/'pack').exists())
        self.assertFalse((self.base/'pack/authoring.json').exists())

    def test_new_nonoverlapping_directories_required_before_compile(self):
        for output,work in [(self.base,self.base/'work'),(self.base/'same',self.base/'same'),
                            (self.base/'tree',self.base/'tree/work'),(self.base/'tree/output',self.base/'tree')]:
            with self.subTest(output=output,work=work),patch.object(builder.subprocess,'run') as process,self.assertRaises(ValueError):
                builder.package_authoring(Path(sys.executable),output,work)
            process.assert_not_called()

    def test_builder_rejects_invalid_version_before_compile(self):
        with patch.object(builder.subprocess,'run') as process,self.assertRaises(ValueError):self.build('١.2.3')
        process.assert_not_called();self.assertFalse((self.base/'work').exists())


if __name__=='__main__':unittest.main()
