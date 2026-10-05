#!/usr/bin/env python3
"""Read-only native gameplay artifact metadata and portable v2 bundle closure.

Synthetic ELF/PE headers exercise metadata validation only: they are never
loaded or represented as compiled games. --artifact additionally qualifies
export of an actual published native artifact; execution is a separate gate.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--runtime', type=Path)
parser.add_argument('--artifact', type=Path)
parser.add_argument('--execute-native', action='store_true', help='Qualify actual load preflight with --artifact and a simulation/native-gameplay binary.')
parser.add_argument('--evidence', type=Path)
ARGS = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / 'build/project-gameplay-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []


def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if ARGS.windows_interop else str(path.resolve())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree(path):
    return {p.relative_to(path).as_posix(): digest(p) for p in sorted(path.rglob('*')) if p.is_file()}


def image(target):
    data = bytearray(512)
    if target == 'Linux':
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<HH', data, 16, 3, 62)
    else:
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'
        struct.pack_into('<H', data, 132, 0x8664)
        struct.pack_into('<H', data, 150, 0x2000)
        struct.pack_into('<H', data, 152, 0x20b)
    return bytes(data)


class GameplayProjects(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.temp.name)
        self.project = self.root / 'Source project'
        self.cli('project', 'create', native(self.project), '--name', 'Native gameplay metadata')
        self.manifest = self.project / 'project.json'
        self.spec = json.loads(self.manifest.read_text())
        self.version = self.cli('version')['version']
        self.artifact = self.project / 'gameplay'
        self.artifact.mkdir()
        self.descriptor = self.artifact / 'native-gameplay.json'
        self.target = json.loads((ARGS.runtime / 'runtime.json').read_text())['target_os'] if ARGS.runtime else ('Windows' if ARGS.windows_interop or os.name == 'nt' else 'Linux')

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *argv, success=True):
        proc = subprocess.run([str(ARGS.binary.resolve()), *argv], capture_output=True, text=True, encoding='utf-8', timeout=120)
        reply = json.loads(proc.stdout)
        CALLS.append({'test': self.id(), 'arguments': argv, 'exit_code': proc.returncode, 'reply': reply, 'stderr': proc.stderr})
        self.assertEqual(proc.returncode == 0, success, reply)
        self.assertEqual(reply['status'], 'ok' if success else 'error', reply)
        return reply.get('result')

    def write(self):
        self.manifest.write_text(json.dumps(self.spec, indent=2) + '\n')

    def artifact_fixture(self, target=None):
        target = target or self.target
        name = 'Poima.NativeGame.dll' if target == 'Windows' else 'Poima.NativeGame.so'
        library = self.artifact / name
        library.write_bytes(image(target))
        notice = self.artifact / 'NOTICE.txt'
        notice.write_text('Original non-executable metadata fixture.\n')
        identity = 'poima.test.native-metadata'
        schema = {'identity': identity, 'bytes': 32, 'fields': [
            {'name': 'Counter', 'kind': 'int32', 'offset': 0, 'bytes': 4},
            {'name': 'Distance', 'kind': 'float64', 'offset': 8, 'bytes': 8},
            {'name': 'Rig', 'kind': 'entity', 'offset': 16, 'bytes': 16}]}
        descriptor = {'format': 'poima.native-gameplay', 'version': 1, 'engine_version': self.version,
            'target_os': target, 'target_arch': 'x86_64', 'call_version': 1, 'services_version': 5,
            'entry': 'poima_gameplay_entry', 'library': name, 'identity': identity, 'type': 'Poima.Test.NativeMetadata',
            'schema': schema, 'files': [{'path': p.name, 'size': p.stat().st_size, 'sha256': digest(p), 'role': role}
                                      for p, role in [(library, 'library'), (notice, 'notice')]]}
        self.descriptor.write_text(json.dumps(descriptor, indent=2) + '\n')
        self.spec.update(version=2, gameplay={'descriptor': 'gameplay/native-gameplay.json',
            'values': {'Counter': 7, 'Distance': .25, 'Rig': '0' * 31 + '2'}})
        self.write()
        return descriptor

    def inspect(self, success=True):
        before = tree(self.project)
        result = self.cli('project', 'inspect', native(self.manifest), success=success)
        self.assertEqual(tree(self.project), before, 'Inspection changed source files or created sidecars.')
        return result

    def test_v1_unchanged_v2_requires_gameplay_and_foreign_metadata_is_read_only(self):
        self.inspect()
        self.spec['version'] = 2
        self.write()
        self.inspect(False)
        self.artifact_fixture('Windows' if self.target == 'Linux' else 'Linux')
        result = self.inspect()
        self.assertEqual(result['gameplay']['backend'], 'native_aot')
        self.assertEqual(result['gameplay']['values'], self.spec['gameplay']['values'])
        self.assertEqual(result['gameplay']['identity'], 'poima.test.native-metadata')
        self.spec['version'] = 1
        self.write()
        self.inspect(False)

    def test_descriptor_versions_abi_target_type_and_schema_are_strict(self):
        good = self.artifact_fixture()
        variants = []
        for field, value in [('engine_version', '0.0.0'), ('version', 2), ('call_version', 2),
                             ('services_version', 3), ('call_version', 1.0), ('services_version', True),
                             ('entry', 'other_export'), ('target_arch', 'arm64'), ('target_os', 'Other'),
                             ('type', ''), ('identity', 'different')]:
            value_doc = copy.deepcopy(good)
            value_doc[field] = value
            variants.append(value_doc)
        for change in ['overlap', 'wrong_size', 'unknown_kind', 'unknown_field']:
            value_doc = copy.deepcopy(good)
            if change == 'overlap': value_doc['schema']['fields'][1]['offset'] = 0
            if change == 'wrong_size': value_doc['schema']['fields'][0]['bytes'] = 8
            if change == 'unknown_kind': value_doc['schema']['fields'][0]['kind'] = 'object'
            if change == 'unknown_field': value_doc['unexpected'] = True
            variants.append(value_doc)
        for candidate in variants:
            with self.subTest(candidate=candidate):
                self.descriptor.write_text(json.dumps(candidate))
                self.inspect(False)
        self.descriptor.write_text(json.dumps(good))
        self.inspect()

    def test_component_metadata_matches_descriptor_and_inventory(self):
        good = self.artifact_fixture()
        component = {'id': '1' * 32, 'name': 'Health', 'version': 1, 'fields': [
            {'id': '2' * 32, 'name': 'Value', 'kind': 'int32', 'default': 100}]}
        good['schema']['components'] = [component]
        world_path = self.project / self.spec['entry']['world']
        world = json.loads(world_path.read_text())
        world.update(version=2, component_schemas={component['id']: component}, retired_component_schemas=[])
        world['entities'][self.spec['entry']['controller']]['components']['game:'+component['id']] = {'2'*32: 100}
        world_path.write_text(json.dumps(world))
        metadata = self.artifact / 'game.poima-components.json'
        manifest = {'format': 'poima.components', 'version': 1, 'schemas': [component]}

        def install(doc, content):
            metadata.write_text(json.dumps(content))
            candidate = copy.deepcopy(doc)
            candidate['files'].append({'path': metadata.name, 'size': metadata.stat().st_size,
                                       'sha256': digest(metadata), 'role': 'metadata'})
            self.descriptor.write_text(json.dumps(candidate))
            return candidate

        valid = install(good, manifest)
        self.inspect()
        original_world = copy.deepcopy(world)
        changed_world = copy.deepcopy(world)
        changed_world['component_schemas'][component['id']]['fields'][0]['default'] = 101
        world_path.write_text(json.dumps(changed_world))
        self.inspect(False)
        world_path.write_text(json.dumps(original_world))
        wrong = copy.deepcopy(manifest)
        wrong['schemas'][0]['fields'][0]['default'] = 99
        install(good, wrong)
        self.inspect(False)
        valid = install(good, manifest)
        valid['files'][-1]['role'] = 'notice'
        self.descriptor.write_text(json.dumps(valid))
        self.inspect(False)
        valid = install(good, manifest)
        del valid['schema']['components']
        self.descriptor.write_text(json.dumps(valid))
        self.inspect(False)
        valid = install(good, manifest)
        other = self.artifact / 'other.poima-components.json'
        metadata.rename(other)
        valid['files'][-1]['path'] = other.name
        self.descriptor.write_text(json.dumps(valid))
        self.inspect(False)

    def test_typed_initial_values_and_portable_descriptor_paths(self):
        self.artifact_fixture()
        original = copy.deepcopy(self.spec)
        for value in [{'Unknown': 1}, {'Counter': True}, {'Counter': 2**40}, {'Distance': 'one'},
                      {'Rig': 'invalid'}, [], None]:
            self.spec = copy.deepcopy(original)
            self.spec['gameplay']['values'] = value
            self.write()
            self.inspect(False)
        for path in ['/absolute.json', '../outside.json', 'gameplay\\native-gameplay.json',
                     'gameplay/../gameplay/native-gameplay.json', 'gameplay/CON.json']:
            self.spec = copy.deepcopy(original)
            self.spec['gameplay']['descriptor'] = path
            self.write()
            self.inspect(False)
        self.spec = copy.deepcopy(original)
        del self.spec['gameplay']['values']
        self.write()
        self.assertEqual(self.inspect()['gameplay']['values'], {})

    def test_inventory_corruption_missing_extra_duplicates_and_native_image_metadata(self):
        good = self.artifact_fixture()
        library = self.artifact / good['library']
        original = library.read_bytes()
        library.write_bytes(original + b'changed')
        self.inspect(False)
        library.unlink()
        self.inspect(False)
        library.write_bytes(original)
        extra = self.artifact / 'unlisted.txt'
        extra.write_text('extra')
        self.inspect(False)
        extra.unlink()
        for change in ['duplicate', 'traversal', 'wrong_role', 'hash', 'size', 'descriptor_alias', 'wrong_image']:
            candidate = copy.deepcopy(good)
            if change == 'duplicate': candidate['files'].append(copy.deepcopy(candidate['files'][0]))
            if change == 'traversal': candidate['files'][0]['path'] = '../outside.so'
            if change == 'wrong_role': candidate['files'][0]['role'] = 'notice'
            if change == 'hash': candidate['files'][0]['sha256'] = '0' * 64
            if change == 'size': candidate['files'][0]['size'] += 1
            if change == 'descriptor_alias': candidate['files'][0]['path'] = 'native-gameplay.json'
            if change == 'wrong_image':
                library.write_bytes(b'Not a native library')
                candidate['files'][0].update(size=library.stat().st_size, sha256=digest(library))
            self.descriptor.write_text(json.dumps(candidate))
            self.inspect(False)
            library.write_bytes(original)
        self.descriptor.write_text(json.dumps(good))
        self.inspect()
        self.descriptor.write_text(json.dumps(good).replace('"call_version": 1', '"call_version": 1, "call_version": 1'))
        self.inspect(False)

    @unittest.skipIf(ARGS.windows_interop, 'Linux symlink setup; Windows junction behavior needs native fixture.')
    def test_payload_symlinks_preserve_original_target(self):
        descriptor = self.artifact_fixture()
        library = self.artifact / descriptor['library']
        target = self.root / 'outside.so'
        library.replace(target)
        original = target.read_bytes()
        try:
            library.symlink_to(target)
        except OSError as error:
            if getattr(error, 'winerror', None) == 1314:
                self.skipTest('Windows symlink creation privilege is unavailable (WinError 1314).')
            raise
        before = tree(self.project)
        self.cli('project', 'inspect', native(self.manifest), success=False)
        self.assertEqual(target.read_bytes(), original)
        self.assertEqual(tree(self.project), before)

    @unittest.skipUnless(ARGS.runtime, 'Requires an installed runtime for actual directory export.')
    def test_v2_export_relocation_inventory_roles_and_tamper(self):
        self.artifact_fixture()
        before = tree(self.project)
        destination = self.root / 'Exported'
        self.cli('project', 'build', native(self.manifest), '--runtime', native(ARGS.runtime), '--output', native(destination))
        self.assertEqual(tree(self.project), before)
        game = json.loads((destination / 'game.json').read_text())
        self.assertEqual(game['version'], 2)
        self.assertEqual(game['gameplay']['descriptor'], 'gameplay/native-gameplay.json')
        self.assertEqual(game['gameplay']['values'], self.spec['gameplay']['values'])
        roles = {f['path']: f['role'] for f in game['files']}
        self.assertEqual(roles['gameplay/native-gameplay.json'], 'gameplay_descriptor')
        self.assertIn('gameplay_library', roles.values())
        self.assertIn('gameplay_notice', roles.values())
        relocated = self.root / 'Relocated bundle'
        destination.rename(relocated)
        bundle = tree(relocated)
        self.cli('game', 'inspect', native(relocated / 'game.json'))
        self.assertEqual(tree(relocated), bundle)
        library = next(relocated / f['path'] for f in game['files'] if f['role'] == 'gameplay_library')
        library.write_bytes(library.read_bytes() + b'changed')
        changed = tree(relocated)
        self.cli('game', 'inspect', native(relocated / 'game.json'), success=False)
        self.assertEqual(tree(relocated), changed)

    @unittest.skipUnless(ARGS.runtime, 'Requires installed runtime for export validation.')
    def test_target_mismatch_and_missing_native_feature_reject_before_publication(self):
        self.artifact_fixture('Windows' if self.target == 'Linux' else 'Linux')
        output = self.root / 'Wrong target'
        self.cli('project', 'build', native(self.manifest), '--runtime', native(ARGS.runtime), '--output', native(output), success=False)
        self.assertFalse(output.exists())

        # Metadata-only runtime fixture: no executable is loaded during export preflight.
        metadata_runtime = self.root / 'No native feature'
        metadata_runtime.mkdir()
        spec = json.loads((ARGS.runtime / 'runtime.json').read_text())
        spec['features'].pop('native_gameplay', None)
        (metadata_runtime / 'runtime.json').write_text(json.dumps(spec))
        # Rebuild source fixture with matching target, removing previous target's library.
        for path in self.artifact.iterdir(): path.unlink()
        self.artifact_fixture()
        self.cli('project', 'build', native(self.manifest), '--runtime', native(metadata_runtime), '--output', native(output), success=False)
        self.assertFalse(output.exists())

    def test_native_runtime_services_abi_matches_at_export_and_bundle_inspection(self):
        self.artifact_fixture()
        runtime = self.root/'Metadata runtime'
        (runtime/'bin').mkdir(parents=True)
        notices = runtime/'share/poima'; notices.mkdir(parents=True)
        for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
            (notices/name).write_text('Original metadata-only test fixture.\n')
        executable = 'bin/poima.exe' if self.target == 'Windows' else 'bin/poima'
        (runtime/executable).write_bytes(b'Metadata-only runtime fixture; never executed.\n')
        (runtime/executable).chmod(0o755)
        spec = dict(format='poima.runtime', version=1, engine_version=self.version,
                    target_os=self.target, target_arch='x86_64', executable=executable,
                    gameplay_services_version=5,
                    features=dict(simulation=True, renderer=True, audio=False, managed=False, editor=False, native_gameplay=True))
        path = runtime/'runtime.json'
        path.write_text(json.dumps(spec))
        source = tree(self.project)
        bundle = self.root/'Matching ABI'
        self.cli('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(bundle))
        self.cli('game', 'inspect', native(bundle/'game.json'))
        game = json.loads((bundle/'game.json').read_text())
        for index, value in enumerate([None, 3, 4, 6, '5', True, 5.0, 0]):
            with self.subTest(services_version=value):
                changed = copy.deepcopy(spec)
                if value is None: changed.pop('gameplay_services_version')
                else: changed['gameplay_services_version'] = value
                path.write_text(json.dumps(changed))
                before = tree(runtime)
                destination = self.root/f'ABI mismatch {index}'
                self.cli('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(destination), success=False)
                self.assertFalse(destination.exists(), 'ABI rejection published a bundle.')
                self.assertEqual(tree(runtime), before)
                # Recompute inventory to prove this is the semantic ABI guard,
                # not ordinary tamper detection. No fixture binary executes.
                bundled = bundle/'runtime/runtime.json'; bundled.write_bytes(path.read_bytes())
                changed_game = copy.deepcopy(game)
                entry = next(item for item in changed_game['files'] if item['path'] == 'runtime/runtime.json')
                entry.update(size=bundled.stat().st_size, sha256=digest(bundled))
                (bundle/'game.json').write_text(json.dumps(changed_game))
                before_bundle = tree(bundle)
                self.cli('game', 'inspect', native(bundle/'game.json'), success=False)
                self.assertEqual(tree(bundle), before_bundle)
        self.assertEqual(tree(self.project), source)

    @unittest.skipUnless(ARGS.runtime and ARGS.artifact, 'Requires a real published Native AOT artifact and installed runtime.')
    def test_real_published_artifact_exports_without_development_paths(self):
        descriptor = json.loads(ARGS.artifact.read_text())
        for file in descriptor['files']:
            destination = self.artifact / file['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ARGS.artifact.parent / file['path'], destination)
        self.descriptor.write_bytes(ARGS.artifact.read_bytes())
        self.spec.update(version=2, gameplay={'descriptor': 'gameplay/native-gameplay.json', 'values': {}})
        self.write()
        self.inspect()
        source = tree(self.project)
        output = self.root / 'Actual AOT export'
        self.cli('project', 'build', native(self.manifest), '--runtime', native(ARGS.runtime), '--output', native(output))
        self.assertEqual(tree(self.project), source)
        inspection = self.cli('game', 'inspect', native(output / 'game.json'))
        self.assertIn('gameplay', inspection)
        copied = output / 'gameplay' / descriptor['library']
        self.assertEqual(digest(copied), digest(ARGS.artifact.parent / descriptor['library']))
        for manifest in [output / 'game.json', output / 'gameplay/native-gameplay.json']:
            self.assertNotIn(str(ROOT), manifest.read_text())
            self.assertNotIn(str(ARGS.artifact.parent), manifest.read_text())

    @unittest.skipUnless(ARGS.artifact and ARGS.execute_native, 'Requires explicit actual-native execution qualification.')
    def test_real_load_hash_and_typed_values_preflight_does_not_initialize_module(self):
        descriptor = json.loads(ARGS.artifact.read_text())
        for file in descriptor['files']:
            destination = self.artifact / file['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ARGS.artifact.parent / file['path'], destination)
        self.descriptor.write_bytes(ARGS.artifact.read_bytes())
        self.spec.update(version=2, gameplay={'descriptor': 'gameplay/native-gameplay.json', 'values': {}})
        self.write()
        original = tree(self.project)
        inspected = self.inspect()
        self.assertEqual(inspected['gameplay']['identity'], descriptor['identity'])
        self.assertEqual(inspected['gameplay']['type'], descriptor['type'])
        self.assertEqual(tree(self.project), original)
        session = '0' * 29 + '900'
        requests = []
        def request(method, **params):
            requests.append({'jsonrpc': '2.0', 'id': len(requests), 'method': method, 'params': params})
        def load(expected, values):
            request('runtime.gameplay.load_native', session_id=session, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=0, descriptor=native(self.descriptor),
                    expected_descriptor_sha256=expected, values=values)
        request('runtime.start', session_id=session, revision=1)
        request('runtime.gameplay.collect', session_id=session)
        load('0' * 64, {})
        request('runtime.gameplay.collect', session_id=session)
        field = descriptor['schema']['fields'][0]
        invalid = {'entity': 'not-an-entity', 'int32': True, 'int64': 7,
                   'float32': 'not-a-number', 'float64': 'not-a-number'}[field['kind']]
        load(digest(self.descriptor), {field['name']: invalid})
        request('runtime.gameplay.collect', session_id=session)
        request('runtime.gameplay.inspect', session_id=session, include_schema=True)
        load(digest(self.descriptor), {})
        request('runtime.gameplay.collect', session_id=session)
        request('runtime.gameplay.inspect', session_id=session, include_schema=True)
        world = self.project / self.spec['entry']['world']
        authored = world.read_bytes()
        proc = subprocess.run([str(ARGS.binary.resolve()), 'world', native(world)],
            input=''.join(json.dumps(r) + '\n' for r in requests), capture_output=True, text=True, encoding='utf-8', timeout=120)
        replies = [json.loads(line) for line in proc.stdout.splitlines()]
        CALLS.append({'test': self.id(), 'requests': requests, 'responses': replies, 'exit_code': proc.returncode, 'stderr': proc.stderr})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(replies), len(requests))
        for index, reply in enumerate(replies):
            if index in [2, 4]: self.assertEqual(reply.get('error', {}).get('code'), -32060, reply)
            else: self.assertIn('result', reply, reply)
        self.assertEqual(replies[1]['result'], {'active_modules': 0, 'retired_alive': 0})
        self.assertEqual(replies[1]['result'], replies[3]['result'])
        self.assertEqual(replies[1]['result'], replies[5]['result'])
        self.assertIsNone(replies[6]['result']['module'])
        self.assertEqual(replies[8]['result']['active_modules'], 1)
        module = replies[9]['result']['module']
        self.assertEqual(module['backend'], 'native_aot')
        self.assertEqual(module['schema'], descriptor['schema'])
        self.assertEqual(module['type'], descriptor['type'])
        library = next(f for f in descriptor['files'] if f['role'] == 'library')
        self.assertEqual(module['assembly_sha256'], library['sha256'])
        self.assertEqual(module['native_diagnostics'], {'dynamic_code_supported': False, 'dynamic_code_compiled': False})
        self.assertEqual(world.read_bytes(), authored)


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(GameplayProjects))
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True, exist_ok=True)
        ARGS.evidence.write_text(json.dumps({'passed': result.wasSuccessful(), 'tests': result.testsRun,
            'skipped': len(result.skipped), 'binary_sha256': digest(ARGS.binary), 'test_sha256': digest(Path(__file__)),
            'synthetic_artifacts_are_metadata_only': True, 'real_artifact_supplied': ARGS.artifact is not None,
            'calls': CALLS}, indent=2) + '\n')
    raise SystemExit(not result.wasSuccessful())
