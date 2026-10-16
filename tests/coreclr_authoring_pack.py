#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CoreCLR pack boundary/build-policy checks; no compiler or engine is executed."""
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('poima_legacy_pack_test_helpers', ROOT / 'tests/authoring_pack.py')
common = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = common
spec.loader.exec_module(common)
pack, builder = common.pack, common.builder
publisher_support = common.module('poima_coreclr_publisher_test_helpers', ROOT / 'tests/publisher_authoring_pack.py')


def manifest(root, version=2):
    roles = pack.CORECLR_PAYLOAD_ROLES if version == 2 else pack.PAYLOAD_ROLES
    value = dict(format='poima.authoring-tools', version=version, engine_version='0.0.164',
                 framework='net10.0', language='14.0', files=[])
    for name, role in sorted(roles.items()):
        data = (root / name).read_bytes()
        value['files'].append(dict(path=name, role=role, size=len(data), sha256=common.digest(data)))
    common.save(root, value)
    return value


def fixture(root):
    for name in pack.CORECLR_PAYLOAD_ROLES:
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(b'one exact SDK' if name.endswith('Poima.Gameplay.dll') else ('original ' + name).encode())
    return manifest(root)


class CoreClrPackContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='poima-coreclr-pack-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / 'pack'
        self.value = fixture(self.root)
        self.raw = (self.root / 'authoring.json').read_bytes()

    def test_exact_v2_inventory_resources_and_relocation(self):
        admitted = pack.load_pack(self.root, expected_sha256=common.digest(self.raw))
        self.assertEqual(len(admitted._entries), 17)
        self.assertEqual(len(pack.PAYLOAD_ROLES), 13)
        self.assertEqual(len(pack.CORECLR_PAYLOAD_ROLES), 17)
        self.assertEqual(set(pack.CORECLR_PAYLOAD_ROLES) - set(pack.PAYLOAD_ROLES), {
            'bridge/Poima.ManagedBridge.dll', 'bridge/Poima.ManagedBridge.deps.json',
            'bridge/Poima.ManagedBridge.runtimeconfig.json', 'bridge/Poima.Gameplay.dll'})
        for name in pack.CORECLR_PAYLOAD_ROLES:
            self.assertEqual(admitted.resource(Path(name)), self.root / name)
        for name in ('authoring.json', 'bridge/../sdk/Poima.Gameplay.dll', 'bridge/Unknown.dll', '../bridge/Poima.ManagedBridge.dll'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                admitted.resource(name)
        relocated = self.base / 'installed'
        shutil.copytree(self.root, relocated)
        shutil.rmtree(self.root)
        installed = pack.load_pack(relocated)
        self.assertEqual(installed.manifest_sha256, admitted.manifest_sha256)
        self.assertIsNone(installed.verify())

    def test_v1_cannot_access_uninventoried_bridge(self):
        legacy = self.base / 'legacy'
        common.fixture(legacy)
        admitted = pack.load_pack(legacy)
        self.assertEqual(len(admitted._entries), 13)
        for name in set(pack.CORECLR_PAYLOAD_ROLES) - set(pack.PAYLOAD_ROLES):
            with self.subTest(name=name), self.assertRaises(ValueError):
                admitted.resource(name)
        admitted.verify()

    def test_manually_constructed_pack_cannot_extend_known_resource_paths(self):
        for name in ('../outside.dll', '/outside.dll', 'bridge/Unknown.dll', 'bridge/../sdk/Poima.Gameplay.dll'):
            forged = pack.Pack(self.root, '0.0.164', '0' * 64, ((name, 1, '0' * 64, 'bridge'),))
            with self.subTest(name=name), self.assertRaises(ValueError):
                forged.resource(name)

    def test_version_inventory_and_closed_types_cannot_be_mixed(self):
        mutations = [lambda value: value.update(version=1), lambda value: value.update(version=3),
                     lambda value: value.update(version=True), lambda value: value.update(version=2.0),
                     lambda value: value.update(extra=True), lambda value: value['files'].pop(),
                     lambda value: value['files'].reverse(),
                     lambda value: value['files'].__setitem__(-1, value['files'][0].copy()),
                     lambda value: value['files'][0].update(role='bridge'),
                     lambda value: value['files'][1].update(role='generator'),
                     lambda value: value['files'][0].update(path='bridge/Unlisted.dll'),
                     lambda value: value['files'][0].update(size=True),
                     lambda value: value['files'][0].update(size=pack.FILE_LIMIT + 1),
                     lambda value: value['files'][0].update(sha256='A' * 64)]
        for index, mutate in enumerate(mutations):
            value = json.loads(self.raw)
            mutate(value)
            common.save(self.root, value)
            with self.subTest(index=index), self.assertRaises(ValueError):
                pack.load_pack(self.root)
        common.save(self.root, self.value)
        for raw in (self.raw.replace(b'"version": 2', b'"version": 2, "version": 2'),
                    b'\xef\xbb\xbf' + self.raw, self.raw.replace(b'"version": 2', b'"version": NaN')):
            (self.root / 'authoring.json').write_bytes(raw)
            with self.subTest(raw=raw[:48]), self.assertRaises(ValueError):
                pack.load_pack(self.root)

    def test_all_three_sdk_copies_are_bound_to_exact_bytes(self):
        for name in ('sdk/Poima.Gameplay.dll', 'tools/native-generator/Poima.Gameplay.dll', 'bridge/Poima.Gameplay.dll'):
            target = self.root / name
            original = target.read_bytes()
            target.write_bytes(b'one incorrect SDK')
            manifest(self.root)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'SDK copies'):
                pack.load_pack(self.root)
            target.write_bytes(original)
        manifest(self.root)
        pack.load_pack(self.root).verify()

    def test_bridge_missing_extra_and_mutated_payloads_fail(self):
        target = self.root / 'bridge/Poima.ManagedBridge.dll'
        original = target.read_bytes()
        admitted = pack.load_pack(self.root)
        for action in ('missing', 'mutated', 'extra_file', 'extra_directory', 'manifest'):
            with self.subTest(action=action):
                if action == 'missing': target.unlink()
                elif action == 'mutated': target.write_bytes(b'changed bridge')
                elif action == 'extra_file': (self.root / 'bridge/extra.dll').write_bytes(b'unknown')
                elif action == 'extra_directory': (self.root / 'bridge/unlisted').mkdir()
                else: (self.root / 'authoring.json').write_bytes(self.raw + b'\n')
                with self.assertRaises(ValueError): admitted.verify()
                if action in ('missing', 'mutated'): target.write_bytes(original)
                elif action == 'extra_file': (self.root / 'bridge/extra.dll').unlink()
                elif action == 'extra_directory': (self.root / 'bridge/unlisted').rmdir()
                else: (self.root / 'authoring.json').write_bytes(self.raw)
        admitted.verify()

    def test_v2_aggregate_bound_and_manifest_hash_still_apply(self):
        value = json.loads(self.raw)
        for row in value['files']: row['size'] = pack.FILE_LIMIT
        common.save(self.root, value)
        with self.assertRaisesRegex(ValueError, 'total'): pack.load_pack(self.root)
        (self.root / 'authoring.json').write_bytes(self.raw)
        with self.assertRaisesRegex(ValueError, 'identity differs'):
            pack.load_pack(self.root, expected_sha256='0' * 64)

    def test_validator_cli_reports_v2_actual_payload_count(self):
        with patch.object(sys, 'argv', ['authoring_pack.py', str(self.root)]), patch('sys.stdout', new_callable=io.StringIO) as output:
            pack.main()
        self.assertEqual(json.loads(output.getvalue()), dict(engine_version='0.0.164',
                         manifest_sha256=common.digest(self.raw), files=17))


class CoreClrBuilderContract(unittest.TestCase):
    def setUp(self):
        common.BuilderContract.setUp(self)

    def fake_build(self, command, **options):
        self.commands.append((command, options))
        output = Path(command[command.index('-o') + 1])
        output.mkdir()
        if output.name == 'bridge-output':
            for name in ('Poima.ManagedBridge.dll', 'Poima.ManagedBridge.deps.json',
                         'Poima.ManagedBridge.runtimeconfig.json', 'Poima.Gameplay.dll',
                         'Poima.ManagedBridge.pdb', 'Poima.Gameplay.pdb', 'Poima.Gameplay.deps.json'):
                (output / name).write_bytes(('bridge built ' + name).encode())
        elif output.name == 'generator-output':
            for name in ('Poima.NativeGame.Generator.dll', 'Poima.NativeGame.Generator.deps.json',
                         'Poima.NativeGame.Generator.runtimeconfig.json'):
                (output / name).write_bytes(('generator built ' + name).encode())
            reference = [item.split('=', 1)[1] for item in command if item.startswith('-p:PoimaGameplayAssembly=')]
            (output / 'Poima.Gameplay.dll').write_bytes(Path(reference[0]).read_bytes() if reference else b'legacy SDK')
        else:
            (output / 'Poima.Components.Generator.dll').write_bytes(b'built analyzer')

    def build(self, coreclr=True, suffix=''):
        return builder.package_authoring(Path(sys.executable), self.base / ('pack' + suffix),
                                         self.base / ('work' + suffix), '0.0.164', coreclr=coreclr)

    def test_bridge_first_generator_binary_reference_and_three_identical_sdks(self):
        with patch.object(builder.subprocess, 'run', side_effect=self.fake_build):
            result = self.build()
        self.assertEqual([Path(command[command.index('-o') + 1]).name for command, _ in self.commands],
                         ['bridge-output', 'generator-output', 'analyzer-output'])
        for index, (command, options) in enumerate(self.commands):
            self.assertEqual(options['cwd'], self.base / 'work')
            self.assertTrue(options['check'])
            self.assertIn('--disable-build-servers', command)
            self.assertIn('-nodeReuse:false', command)
            self.assertIn('-p:UseSharedCompilation=false', command)
            references = [item for item in command if item.startswith('-p:PoimaGameplayAssembly=')]
            self.assertEqual(references, ['-p:PoimaGameplayAssembly=' + str(self.base / 'work/bridge-output/Poima.Gameplay.dll')]
                             if index == 1 else [])
        self.assertEqual(json.loads((result.root / 'authoring.json').read_text())['version'], 2)
        self.assertEqual(len(result._entries), 17)
        sdk = result.resource('sdk/Poima.Gameplay.dll').read_bytes()
        self.assertEqual(result.resource('bridge/Poima.Gameplay.dll').read_bytes(), sdk)
        self.assertEqual(result.resource('tools/native-generator/Poima.Gameplay.dll').read_bytes(), sdk)
        self.assertEqual(result.resource('build/Poima.Gameplay.props').read_text(), builder.PROPS)
        self.assertEqual(result.resource('build/Poima.Components.targets').read_text(), builder.TARGETS)
        moved = self.base / 'installed'
        shutil.copytree(result.root, moved)
        shutil.rmtree(self.checkout)
        shutil.rmtree(self.base / 'work')
        shutil.rmtree(result.root)
        pack.load_pack(moved).verify()

    def test_default_v1_build_retains_two_original_commands(self):
        with patch.object(builder.subprocess, 'run', side_effect=self.fake_build): result = self.build(coreclr=False)
        self.assertEqual(len(self.commands), 2)
        self.assertEqual([Path(command[command.index('-o') + 1]).name for command, _ in self.commands],
                         ['generator-output', 'analyzer-output'])
        self.assertFalse(any(item.startswith('-p:PoimaGameplayAssembly=') for command, _ in self.commands for item in command))
        self.assertEqual(json.loads((result.root / 'authoring.json').read_text())['version'], 1)
        self.assertEqual(len(result._entries), 13)
        self.assertFalse((result.root / 'bridge').exists())

    def test_generator_project_selects_source_or_exact_sdk_reference(self):
        project = ET.parse(ROOT / 'managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj').getroot()
        references = project.findall('./ItemGroup/ProjectReference')
        binaries = project.findall('./ItemGroup/Reference')
        self.assertEqual(len(references), 1)
        self.assertEqual(len(binaries), 1)
        source_group = next(group for group in project.findall('ItemGroup') if group.find('ProjectReference') is not None)
        binary_group = next(group for group in project.findall('ItemGroup') if group.find('Reference') is not None)
        self.assertEqual(source_group.attrib['Condition'], "'$(PoimaGameplayAssembly)' == ''")
        self.assertEqual(binary_group.attrib['Condition'], "'$(PoimaGameplayAssembly)' != ''")
        self.assertEqual(binaries[0].findtext('HintPath'), '$(PoimaGameplayAssembly)')
        self.assertEqual(binaries[0].findtext('Private'), 'true')

    def test_unknown_dependency_in_each_build_cannot_publish(self):
        for index, label in enumerate(('bridge-output', 'generator-output', 'analyzer-output')):
            self.commands.clear()
            def extra(command, **options):
                self.fake_build(command, **options)
                output = Path(command[command.index('-o') + 1])
                if output.name == label: (output / 'Unreviewed.dll').write_bytes(b'unknown dependency')
            with self.subTest(label=label), patch.object(builder.subprocess, 'run', side_effect=extra), \
                    self.assertRaisesRegex(ValueError, 'Additional build dependency'):
                self.build(suffix=str(index))
            self.assertFalse((self.base / ('pack' + str(index))).exists())
            self.assertTrue((self.base / ('work' + str(index))).exists())
            if label == 'bridge-output': self.assertEqual(len(self.commands), 1)

    def test_missing_bridge_output_stops_before_generator(self):
        def missing(command, **options):
            self.fake_build(command, **options)
            (self.base / 'work/bridge-output/Poima.ManagedBridge.runtimeconfig.json').unlink()
        with patch.object(builder.subprocess, 'run', side_effect=missing), self.assertRaisesRegex(ValueError, 'omits required'):
            self.build()
        self.assertEqual(len(self.commands), 1)
        self.assertFalse((self.base / 'pack').exists())

    def test_generator_with_wrong_sdk_cannot_publish(self):
        def different(command, **options):
            self.fake_build(command, **options)
            output = Path(command[command.index('-o') + 1])
            if output.name == 'generator-output': (output / 'Poima.Gameplay.dll').write_bytes(b'wrong second build SDK')
        with patch.object(builder.subprocess, 'run', side_effect=different), self.assertRaisesRegex(ValueError, 'SDK copies'):
            self.build()
        self.assertFalse((self.base / 'pack').exists())

    def test_bridge_mutation_during_copy_leaves_no_manifest(self):
        original = builder._copy
        def mutate(source, destination, expected):
            if destination.as_posix().endswith('bridge/Poima.ManagedBridge.dll'):
                source.write_bytes(b'changed after source admission')
            return original(source, destination, expected)
        with patch.object(builder.subprocess, 'run', side_effect=self.fake_build), \
                patch.object(builder, '_copy', side_effect=mutate), self.assertRaises(ValueError):
            self.build()
        self.assertTrue((self.base / 'pack').exists())
        self.assertFalse((self.base / 'pack/authoring.json').exists())

    def test_coreclr_option_requires_boolean_before_compile(self):
        for value in (1, 'true', None):
            with self.subTest(value=value), patch.object(builder.subprocess, 'run') as process, self.assertRaises(ValueError):
                self.build(coreclr=value)
            process.assert_not_called()
        self.assertFalse((self.base / 'work').exists())

    def test_builder_cli_opt_in_and_actual_payload_count(self):
        value = fixture(self.base / 'prepared')
        admitted = pack.load_pack(self.base / 'prepared')
        arguments = ['package_authoring.py', '--dotnet', sys.executable, '--output', str(self.base / 'pack'),
                     '--work', str(self.base / 'work'), '--coreclr', '--engine-version', '0.0.164']
        with patch.object(sys, 'argv', arguments), patch.object(builder, 'package_authoring', return_value=admitted) as compile_pack, \
                patch('sys.stdout', new_callable=io.StringIO) as output:
            builder.main()
        compile_pack.assert_called_once_with(Path(sys.executable), self.base / 'pack', self.base / 'work', '0.0.164', coreclr=True)
        self.assertEqual(json.loads(output.getvalue()), dict(authoring_pack=str(admitted.root), engine_version=value['engine_version'],
                         manifest_sha256=admitted.manifest_sha256, files=17))


class CoreClrPublisherContract(unittest.TestCase):
    # Reuse the real-publisher mocked runner and original complete assertions;
    # do not inherit its tests or launch an additional process of any kind.
    fake_run = publisher_support.PublisherPack.fake_run
    invoke = publisher_support.PublisherPack.invoke
    descriptor = publisher_support.PublisherPack.descriptor

    def setUp(self):
        publisher_support.PublisherPack.setUp(self)
        value = json.loads(self.manifest.read_text())
        value['version'] = 2
        for name in set(pack.CORECLR_PAYLOAD_ROLES) - set(pack.PAYLOAD_ROLES):
            role = pack.CORECLR_PAYLOAD_ROLES[name]
            data = self.sdk if role == 'sdk' else ('synthetic resource ' + name).encode()
            path = self.pack / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            value['files'].append(dict(path=name, size=len(data), sha256=common.digest(data), role=role))
        value['files'].sort(key=lambda row: row['path'])
        common.save(self.pack, value)

    def test_v2_pack_reuses_complete_installed_native_publisher_contract(self):
        publisher_support.PublisherPack.test_installed_commands_use_no_checkout_or_internal_generator_build(self)
        self.assertEqual(len(pack.load_pack(self.pack)._entries), 17)

    def test_unconsumed_bridge_mutation_still_invalidates_native_publish(self):
        def mutate(event):
            if event == 'game_build':
                target = self.pack / 'bridge/Poima.ManagedBridge.dll'
                target.write_bytes(target.read_bytes() + b'mutated after admission')
        self.after = mutate
        with self.assertRaises(ValueError): self.invoke()
        self.assertEqual([row['argv'][1] for row in self.commands], ['build', 'msbuild'])
        self.assertFalse(self.output.exists())
        self.assertFalse(self.checkout.exists())


if __name__ == '__main__':
    unittest.main()
