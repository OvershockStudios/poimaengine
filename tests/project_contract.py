#!/usr/bin/env python3
"""Portable project manifests, immutable inspection and optional bundle export."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
p.add_argument('--windows-interop', action='store_true')
p.add_argument('--runtime', type=Path, help='Optional CMake-installed native runtime root.')
p.add_argument('--evidence', type=Path)
args = p.parse_args()
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT/'build/project-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []

def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

def tree(path):
    return {str(p.relative_to(path)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(path.rglob('*')) if p.is_file()}

class ProjectContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.tmp.name)
        self.project = self.root/'Portable project'
        self.created = self.run_cli('project', 'create', native(self.project), '--name', 'Portable room')
        self.manifest = self.project/'project.json'
        self.document = json.loads(self.manifest.read_text(encoding='utf-8'))

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv, success=True):
        command = [str(args.binary.resolve()), *argv]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        try:
            envelope = json.loads(result.stdout)
        except ValueError:
            self.fail(f'Expected structured reply: {result.returncode} {result.stdout} {result.stderr}')
        CALLS.append({'command': command, 'exit_code': result.returncode, 'reply': envelope, 'stderr': result.stderr})
        self.assertEqual(result.returncode == 0, success, envelope)
        self.assertEqual(envelope['status'], 'ok' if success else 'error', envelope)
        if not success:
            self.assertTrue(envelope['diagnostics'], envelope)
        return envelope.get('result')

    def write(self, doc):
        self.manifest.write_text(json.dumps(doc, indent=2)+'\n', encoding='utf-8')

    def inspect(self, success=True):
        before = tree(self.project)
        result = self.run_cli('project', 'inspect', native(self.manifest), success=success)
        self.assertEqual(tree(self.project), before, 'Project inspection wrote or altered project files.')
        return result

    def world_rpc(self, method, **params):
        world = self.project/self.document['entry']['world']
        request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}
        result = subprocess.run([str(args.binary.resolve()), 'world', native(world)], input=json.dumps(request)+'\n',
            capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        reply = json.loads(result.stdout)
        self.assertIn('result', reply, reply)
        return reply['result']

    def test_starter_manifest_and_no_write_inspection(self):
        doc = self.document
        self.assertIn(self.created['publication'], ('atomic_directory', 'manifest_last'))
        self.assertFalse((self.project/'.poima-incomplete').exists(), 'Successful creation retained an incomplete marker.')
        self.assertEqual((doc['format'], doc['version'], doc['name']), ('poima.project', 1, 'Portable room'))
        self.assertRegex(doc['project_id'], '^[0-9a-f]{32}$')
        self.assertFalse(Path(doc['entry']['world']).is_absolute())
        world = json.loads((self.project/doc['entry']['world']).read_text(encoding='utf-8'))
        self.assertIn('CharacterController', world['entities'][doc['entry']['controller']]['components'])
        self.assertIn('Camera', world['entities'][doc['entry']['camera']]['components'])
        environments = [e['components']['LightingEnvironment'] for e in world['entities'].values()
                        if 'LightingEnvironment' in e['components']]
        self.assertEqual(len(environments), 1)
        sky = environments[0]['sky']
        self.assertTrue(sky['enabled'])
        sun = world['entities'][sky['sun']]['components']['Light']
        self.assertEqual(sun['kind'], 'directional')
        self.assertTrue(sun['enabled'])
        inspected = self.inspect()
        self.assertEqual(inspected['revision'], world['revision'])
        self.assertEqual(inspected['project_id'], doc['project_id'])
        self.assertFalse(list(self.project.rglob('*.lock')), 'Starter/inspection unexpectedly created a lock sidecar.')

    def test_incomplete_project_is_rejected_without_repair(self):
        marker = self.project/'.poima-incomplete'
        marker.write_text('Publication is incomplete.\n', encoding='utf-8')
        self.inspect(False)
        self.assertEqual(marker.read_text(encoding='utf-8'), 'Publication is incomplete.\n')
        marker.unlink()
        self.inspect()

    def test_existing_create_is_never_overwritten(self):
        before = tree(self.project)
        self.run_cli('project', 'create', native(self.project), '--name', 'Overwrite attempt', success=False)
        self.assertEqual(tree(self.project), before)
        existing = self.root/'existing-empty'
        existing.mkdir()
        self.run_cli('project', 'create', native(existing), '--name', 'Existing', success=False)
        self.assertEqual(list(existing.iterdir()), [])

    def test_duplicate_unknown_and_wrong_type_manifest_fields(self):
        raw = self.manifest.read_text(encoding='utf-8')
        self.manifest.write_text(raw.replace('{', '{"version":1,', 1), encoding='utf-8')
        self.inspect(False)
        for field, value in [('version', 2), ('name', 19), ('project_id', 'BAD'), ('audio', 'true'),
                             ('unknown', True), ('gameplay', {'assembly': 'unimplemented.dll'})]:
            with self.subTest(field=field):
                doc = copy.deepcopy(self.document)
                doc[field] = value
                self.write(doc)
                self.inspect(False)

    def test_world_path_must_stay_portable_and_inside_project(self):
        source = self.project/self.document['entry']['world']
        outside = self.root/'outside.json'
        shutil.copyfile(source, outside)
        for value in ['../outside.json', native(outside), 'C:/outside.json', 'world\x00.json']:
            with self.subTest(path=value):
                doc = copy.deepcopy(self.document)
                doc['entry']['world'] = value
                self.write(doc)
                self.inspect(False)

    def test_missing_world_and_invalid_entry_entities(self):
        for key, value in [('world', 'absent.json'), ('camera', 'f'*32), ('controller', self.document['entry']['camera'])]:
            with self.subTest(field=key):
                doc = copy.deepcopy(self.document)
                doc['entry'][key] = value
                self.write(doc)
                self.inspect(False)

    def test_missing_cooked_mesh_rejected_without_repair(self):
        world = self.project/self.document['entry']['world']
        doc = json.loads(world.read_text(encoding='utf-8'))
        entity = uuid.uuid4().hex
        self.world_rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=doc['revision'], ops=[
            {'op': 'entity.create', 'id': entity, 'name': 'Missing package'},
            {'op': 'component.set', 'id': entity, 'type': 'StaticMesh',
             'value': {'asset': 'f'*64, 'primitive': 0, 'visible': True}}])
        self.inspect(False)

    def test_valid_input_profile_readonly_and_malformed_profile_rejected(self):
        profile = self.project/'controls.poima-input.json'
        value = {'bindings': {'forward': ['key.up'], 'backward': ['key.down'], 'left': ['key.left'],
            'right': ['key.right'], 'jump': ['key.space'], 'use': ['key.e']},
            'sensitivity_x': .2, 'sensitivity_y': .15, 'invert_x': False, 'invert_y': False}
        self.world_rpc('input.transact', path=native(profile), request_id=uuid.uuid4().hex, expected_revision=0, profile=value)
        # Remove test-created writer locks before exercising immutable readers.
        for lock in self.project.rglob('*.lock'):
            lock.unlink()
        doc = copy.deepcopy(self.document)
        doc['input_profile'] = profile.name
        self.write(doc)
        self.inspect()
        self.assertFalse(list(self.project.rglob('*.lock')))
        profile.write_text('{"format":"poima.input.v99"}', encoding='utf-8')
        self.inspect(False)

    def test_missing_runtime_descriptor_rejects_export(self):
        runtime = self.root/'not-runtime'
        runtime.mkdir()
        destination = self.root/'bad-export'
        before = tree(self.project)
        self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(runtime), success=False)
        self.assertEqual(tree(self.project), before)
        self.assertFalse(destination.exists(), 'Failed preflight published a partial game bundle.')

    @unittest.skipUnless(args.runtime, 'Requires explicit installed runtime root.')
    def test_export_inventory_inspection_tamper_and_source_unchanged(self):
        before = tree(self.project)
        destination = self.root/'Exported game'
        result = self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(args.runtime))
        self.assertGreater(result['file_count'], 0)
        self.assertIn(result['publication'], ('atomic_directory', 'manifest_last'))
        self.assertFalse((destination/'.poima-incomplete').exists(), 'Successful export retained an incomplete marker.')
        self.assertEqual(tree(self.project), before)
        game = destination/'game.json'
        self.assertTrue(game.is_file())
        game_document = json.loads(game.read_text(encoding='utf-8'))
        self.assertEqual(game_document['version'], 1, 'Projects without gameplay retain v1 bundles.')
        self.assertNotIn('gameplay', game_document)
        bundle = tree(destination)
        inspected = self.run_cli('game', 'inspect', native(game))
        self.assertEqual(inspected['project_id'], self.document['project_id'])
        self.assertEqual(tree(destination), bundle)
        marker = destination/'.poima-incomplete'
        marker.write_text('Publication is incomplete.\n', encoding='utf-8')
        incomplete = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), incomplete, 'Inspection changed an incomplete bundle.')
        marker.unlink()
        self.run_cli('game', 'inspect', native(game))
        self.assertEqual(tree(destination), bundle, 'Inspection changed the restored complete bundle.')
        self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(args.runtime), success=False)
        self.assertEqual(tree(destination), bundle)
        packaged_world = next(p for p in destination.rglob('*.json') if p.name != 'game.json' and
            '"poima.authored-world"' in p.read_text(encoding='utf-8', errors='replace'))
        original = packaged_world.read_bytes()
        packaged_world.write_bytes(original+b' ')
        tampered = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), tampered)
        packaged_world.write_bytes(original)
        packaged_world.unlink()
        missing = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), missing)

    @unittest.skipUnless(args.runtime, 'Requires explicit installed runtime root.')
    def test_export_cannot_replace_source_project(self):
        before = tree(self.project)
        self.run_cli('project', 'build', native(self.manifest), '--output', native(self.project), '--runtime', native(args.runtime), success=False)
        self.assertEqual(tree(self.project), before)

if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProjectContract))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({'tests': result.testsRun, 'passed': result.wasSuccessful(),
            'skipped': len(result.skipped), 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'calls': CALLS}, indent=2)+'\n', encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
