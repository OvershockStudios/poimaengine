#!/usr/bin/env python3
"""Standalone template catalog protocol, history, runtime snapshots, and dependencies."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid
from gltf_fixture import glb
from texture_fixture import png, quad

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--runtime', type=int, choices=(0, 1), default=1)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--evidence', type=Path)
args = parser.parse_args()
BINARY = str(args.binary.resolve())
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT/'build/world-templates-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []


def uid(n): return f'{n:032x}'
A, B, C, TYPE = uid(1), uid(2), uid(3), uid(100)
FIELD = uid(1000)
SCHEMA = {'id': TYPE, 'name': 'Target', 'version': 1, 'fields': [
    {'id': FIELD, 'name': 'Entity', 'kind': 'entity', 'default': uid(0)}]}
TRANSFORM = {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}


def recipe(identity=A, name='Crate', components=None):
    return {'op': 'template.set', 'id': identity, 'name': name,
            'components': copy.deepcopy({'Transform': TRANSFORM} if components is None else components)}


class Contract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.temp.name)
        self.world = self.root/'world.json'
        self.process = None
        self.open()

    def native(self, path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    def open(self):
        self.process = subprocess.Popen([BINARY, 'world', self.native(self.world)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')

    def close(self):
        if self.process:
            self.process.stdin.close()
            self.process.wait(timeout=20)
            self.assertEqual(self.process.returncode, 0, self.process.stderr.read())
            self.process.stdout.close(); self.process.stderr.close(); self.process = None

    def tearDown(self):
        self.close(); self.temp.cleanup()

    def request(self, method, **params):
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': len(CALLS)+1, 'method': method, 'params': params})+'\n')
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        self.assertTrue(line, 'World service exited unexpectedly')
        result = json.loads(line)
        CALLS.append({'method': method, 'ok': 'result' in result})
        return result

    def ok(self, method, **params):
        result = self.request(method, **params)
        self.assertIn('result', result, result)
        return result['result']

    def error(self, method, code, **params):
        result = self.request(method, **params)
        self.assertEqual(result.get('error', {}).get('code'), code, result)

    def tx(self, rev, ops, **extra):
        return self.ok('world.transact', request_id=extra.pop('request_id', uuid.uuid4().hex), base_revision=rev, ops=ops, **extra)

    def test_discovery_empty_catalog_and_explicit_upgrade(self):
        description = self.ok('world.describe')
        self.assertEqual(description['schema_revision'], 34)
        for method in ('template.get', 'template.query'):
            self.assertIn(method, description['methods'])
        self.assertEqual(self.ok('template.query')['templates'], [])
        self.tx(0, [{'op': 'entity.create', 'id': C, 'name': 'Legacy'}])
        self.assertEqual(json.loads(self.world.read_text())['version'], 1)
        original = self.world.read_bytes()
        self.assertFalse(self.tx(1, [recipe()], preview=True)['committed'])
        self.assertEqual(self.world.read_bytes(), original)
        self.assertEqual(self.ok('template.query')['templates'], [])
        self.tx(1, [recipe()])
        document = json.loads(self.world.read_text())
        self.assertEqual(document['version'], 3)
        for field in ('templates', 'retired_template_ids', 'component_schemas', 'retired_component_schemas'):
            self.assertIn(field, document)
        self.assertEqual(self.ok('world.inspect')['entity_count'], 1)

    def test_zero_instances_pagination_and_complete_replacement(self):
        components = {'Transform': TRANSFORM, 'MeshRenderer': {
            'primitive': 'box', 'visible': True, 'albedo': [.2, .4, .6]}}
        self.tx(0, [recipe(B, 'Second'), recipe(A, components=components)])
        self.assertEqual(self.ok('world.inspect')['entity_count'], 0)
        first = self.ok('template.query', revision=1, limit=1)
        self.assertEqual(first['revision'], 1)
        self.assertEqual(first['templates'], [{'id': A, 'name': 'Crate', 'components': sorted(components)}])
        self.assertEqual(first['next_after'], A)
        second = self.ok('template.query', revision=1, after=A, limit=1)
        self.assertEqual([t['id'] for t in second['templates']], [B])
        self.assertIsNone(second['next_after'])
        self.error('template.query', -32602, after=A)
        self.error('template.query', -32602, limit=0)
        self.error('template.query', -32009, revision=0)
        self.assertEqual(self.ok('template.get', id=A, revision=1)['template']['components'], components)
        self.tx(1, [recipe(A, 'Replacement')])
        result = self.ok('template.get', id=A)
        self.assertEqual(result['revision'], 2)
        self.assertEqual(result['id'], A)
        self.assertEqual(result['template'], {'name': 'Replacement', 'components': {'Transform': TRANSFORM}})
        self.error('template.get', -32009, id=A, revision=1)
        self.error('template.get', -32004, id=C)
        self.close(); self.open()
        self.assertEqual(self.ok('template.get', id=A)['template'], result['template'])

    def test_retry_undo_redo_retirement_and_durable_upgrade(self):
        request = {'request_id': uuid.uuid4().hex, 'base_revision': 0, 'ops': [recipe()]}
        self.ok('world.transact', **request)
        self.assertTrue(self.ok('world.transact', **request)['replayed'])
        self.error('world.transact', -32010, **dict(request, ops=[recipe(name='Conflict')]))
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=1)
        self.assertEqual(self.ok('template.query')['templates'], [])
        self.assertEqual(json.loads(self.world.read_text())['version'], 3)
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=2)
        self.assertEqual(self.ok('template.get', id=A)['template']['name'], 'Crate')
        self.tx(3, [{'op': 'template.remove', 'id': A}])
        self.assertIn(A, json.loads(self.world.read_text())['retired_template_ids'])
        original = self.world.read_bytes()
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=4, ops=[recipe()])
        self.assertEqual(self.world.read_bytes(), original)
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=4)
        self.assertEqual(self.ok('template.get', id=A)['template']['name'], 'Crate')
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=5)
        self.assertEqual(self.ok('template.query')['templates'], [])
        self.close(); self.open()
        self.assertTrue(self.ok('world.transact', **request)['replayed'])
        self.assertEqual(self.ok('world.inspect')['revision'], 6)
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=6, ops=[recipe()])

    def test_invalid_recipes_preserve_world_and_history(self):
        self.tx(0, [recipe()])
        original = self.world.read_bytes(); history = self.ok('world.history')
        invalid = [recipe(components={}), recipe(components={'Transform': {}}),
                   recipe(components={'Transform': TRANSFORM, 'Camera': {'vertical_fov': 60, 'near': .1, 'far': 1000}}),
                   recipe(components={'Transform': TRANSFORM, 'game:'+TYPE: {FIELD: B}}),
                   recipe(components={'Transform': TRANSFORM, 'Unknown': {}})]
        for operation in invalid:
            self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=1,
                       ops=[recipe(B, 'Must roll back'), operation])
            self.assertEqual(self.world.read_bytes(), original)
            self.assertEqual(self.ok('world.history'), history)

    def test_custom_schema_usage_and_literal_entity_reference(self):
        components = {'Transform': TRANSFORM, 'game:'+TYPE: {FIELD: B}}
        self.tx(0, [{'op': 'component.schema.set', 'schema': SCHEMA}, recipe(components=components)])
        self.assertEqual(self.ok('world.inspect')['entity_count'], 0)
        self.assertEqual(self.ok('template.get', id=A)['template']['components'], components)
        original = self.world.read_bytes()
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=1,
                   ops=[{'op': 'component.schema.remove', 'id': TYPE}])
        self.assertEqual(self.world.read_bytes(), original)
        self.tx(1, [{'op': 'template.remove', 'id': A}, {'op': 'component.schema.remove', 'id': TYPE}])
        self.assertEqual(self.ok('component.schemas')['schemas'], [])
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=2)
        self.assertEqual(self.ok('template.get', id=A)['template']['components'], components)

    @unittest.skipUnless(args.runtime, 'Simulation not built')
    def test_runtime_structural_transactions_and_receipts(self):
        self.tx(0, [recipe(), {'op': 'entity.create', 'id': C, 'name': 'Authored'}])
        sid = uuid.uuid4().hex
        self.ok('runtime.start', session_id=sid, revision=1)
        original = self.world.read_bytes()
        def request(rev, **extra):
            return dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0,
                        expected_structure_revision=rev, **extra)
        first = request(0, spawns=[{'template_id': A, 'transform': dict(TRANSFORM, position=[3, 4, 5])}])
        created = self.ok('runtime.structure.transact', **first)
        identity = created['spawned'][0]
        self.assertNotEqual(identity, C)
        self.assertEqual(created['structure_revision'], 1)
        self.assertEqual(created['tick'], 0)
        self.assertEqual(created['despawned'], [])
        self.assertEqual(self.ok('runtime.inspect', session_id=sid)['entities'], 2)
        self.error('runtime.entity', -32009, session_id=sid, id=identity, structure_revision=0)
        self.ok('runtime.entity', session_id=sid, id=identity, structure_revision=1)
        self.assertEqual(self.ok('runtime.entity', session_id=sid, id=identity)['local_transform']['position'], [3, 4, 5])
        retry = self.ok('runtime.structure.transact', **dict(first, despawns=[]))
        self.assertEqual(retry, dict(created, replayed=True))
        self.error('runtime.structure.transact', -32010, **dict(first, spawns=[{'template_id': A}]))
        self.error('runtime.step', -32010, session_id=sid, request_id=first['request_id'], expected_tick=0, ticks=1)
        self.error('runtime.structure.transact', -32009, **request(0, despawns=[identity]))
        self.error('runtime.structure.transact', -32009, **dict(request(1, despawns=[identity]), expected_tick=1))
        before = self.ok('runtime.inspect', session_id=sid)
        failed = request(1, spawns=[{'template_id': B}])
        self.error('runtime.structure.transact', -32602, **failed)
        self.error('runtime.structure.transact', -32602, **request(1, despawns=[C]))
        self.error('runtime.structure.transact', -32602, **request(1, despawns=[identity, identity]))
        self.assertEqual(self.ok('runtime.inspect', session_id=sid), before)
        # A failed request consumes neither a receipt nor an entity identity.
        second = self.ok('runtime.structure.transact', **dict(failed, spawns=[{'template_id': A}]))
        self.assertEqual(int(second['spawned'][0], 16), int(identity, 16)+1)
        removed = self.ok('runtime.structure.transact', **request(2, despawns=[identity, second['spawned'][0]]))
        self.assertEqual(removed['structure_revision'], 3)
        self.assertEqual(self.ok('runtime.inspect', session_id=sid)['entities'], 1)
        self.assertEqual(self.ok('runtime.structure.transact', **first), dict(created, replayed=True))
        self.error('runtime.entity', -32004, session_id=sid, id=identity)
        self.assertEqual(self.world.read_bytes(), original)
        step = dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0, expected_structure_revision=3, ticks=1)
        self.ok('runtime.step', **step)
        self.error('runtime.structure.transact', -32010,
                   **dict(request(3, spawns=[{'template_id': A}]), request_id=step['request_id'], expected_tick=1))

    @unittest.skipUnless(args.runtime, 'Simulation not built')
    def test_structural_guards_cover_tick_based_mutations(self):
        self.tx(0, [recipe(), {'op': 'component.schema.set', 'schema': SCHEMA},
                    {'op': 'entity.create', 'id': C, 'name': 'Survivor'},
                    {'op': 'component.set', 'id': C, 'type': 'game:'+TYPE, 'value': {FIELD: uid(0)}}])
        sid = uuid.uuid4().hex
        self.ok('runtime.start', session_id=sid, revision=1)
        component = self.ok('runtime.component.get', session_id=sid, tick=0, id=C, type=TYPE)
        self.assertEqual(self.ok('runtime.gameplay.inspect', session_id=sid)['structure_revision'], 0)
        birth = self.ok('runtime.structure.transact', session_id=sid, request_id=uuid.uuid4().hex,
                        expected_tick=0, expected_structure_revision=0, spawns=[{'template_id': A}])
        self.assertEqual(birth['component_revision'], component['component_revision'])
        common = dict(session_id=sid, expected_tick=0)
        candidates = {
            'runtime.step': dict(ticks=1),
            'runtime.component.edit': dict(expected_revision=component['component_revision'], id=C, type=TYPE, values={FIELD: uid(0)}),
            'runtime.gameplay.edit': dict(expected_revision=0, values={}),
            'runtime.gameplay.load': dict(expected_revision=0, hostfxr='unopened-host', bridge='unopened-bridge', assembly='unopened-assembly', type='Unused'),
            'runtime.gameplay.load_native': dict(expected_revision=0, descriptor='unopened-descriptor'),
            'runtime.audio.replay': dict(listener=C, path=str(self.root/'never-created.wav'), sequence=[{'ticks': 1}]),
            'runtime.play': dict(controller=C, camera=C, mode='replay', sequence=[{'ticks': 1}])}
        baseline = self.ok('runtime.inspect', session_id=sid)
        for method, extra in candidates.items():
            request = dict(common, request_id=uuid.uuid4().hex, **extra)
            self.error(method, -32602, **request)
            self.error(method, -32009, **dict(request, expected_structure_revision=0))
            self.error(method, -32602, **dict(request, expected_structure_revision=1.0))
        self.assertEqual(self.ok('runtime.inspect', session_id=sid), baseline)
        self.assertFalse((self.root/'never-created.wav').exists())
        edit = dict(common, request_id=uuid.uuid4().hex, expected_structure_revision=1, **candidates['runtime.component.edit'])
        result = self.ok('runtime.component.edit', **edit)
        self.ok('runtime.structure.transact', session_id=sid, request_id=uuid.uuid4().hex,
                expected_tick=0, expected_structure_revision=1, despawns=birth['spawned'])
        # Successful receipt stays recoverable after the structure guard becomes stale.
        self.assertEqual(self.ok('runtime.component.edit', **edit), dict(result, replayed=True))
        self.error('runtime.component.edit', -32602, **dict(edit, expected_structure_revision=1.0))
        step = dict(common, request_id=uuid.uuid4().hex, expected_structure_revision=2, ticks=1)
        advanced = self.ok('runtime.step', **step)
        self.assertEqual(self.ok('runtime.step', **step), dict(advanced, replayed=True))
        self.error('runtime.step', -32602, **dict(step, expected_structure_revision=True))

    @unittest.skipUnless(args.runtime, 'Simulation not built')
    def test_spawned_props_save_restore_and_retired_ids(self):
        self.tx(0, [recipe()])
        sid = uuid.uuid4().hex
        self.ok('runtime.start', session_id=sid, revision=1)
        spawned = self.ok('runtime.structure.transact', session_id=sid, request_id=uuid.uuid4().hex,
                          expected_tick=0, expected_structure_revision=0, spawns=[{'template_id': A}])['spawned'][0]
        (self.root/'saves').mkdir()
        self.ok('save.configure', request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(self.root/'saves'))
        self.error('save.write', -32602, request_id=uuid.uuid4().hex, configuration_generation=1, slot='props',
                   expected_generation=0, session_id=sid, expected_tick=0, expected_gameplay_revision=0)
        self.error('save.write', -32009, request_id=uuid.uuid4().hex, configuration_generation=1, slot='props',
                   expected_generation=0, session_id=sid, expected_tick=0, expected_gameplay_revision=0, expected_structure_revision=0)
        self.ok('save.write', request_id=uuid.uuid4().hex, configuration_generation=1, slot='props',
                expected_generation=0, session_id=sid, expected_tick=0, expected_gameplay_revision=0, expected_structure_revision=1)
        self.ok('runtime.structure.transact', session_id=sid, request_id=uuid.uuid4().hex,
                expected_tick=0, expected_structure_revision=1, despawns=[spawned])
        restored = uuid.uuid4().hex
        self.error('save.load', -32009, request_id=uuid.uuid4().hex, configuration_generation=1, slot='props',
                   expected_generation=1, revision=1, expected_session_id=sid, expected_tick=0,
                   expected_gameplay_revision=0, expected_structure_revision=1, new_session_id=restored)
        self.ok('save.load', request_id=uuid.uuid4().hex, configuration_generation=1, slot='props',
                expected_generation=1, revision=1, expected_session_id=sid, expected_tick=0,
                expected_gameplay_revision=0, expected_structure_revision=2, new_session_id=restored)
        self.assertEqual(self.ok('runtime.inspect', session_id=restored)['structure_revision'], 1)
        self.ok('runtime.entity', session_id=restored, id=spawned)
        later = self.ok('runtime.structure.transact', session_id=restored, request_id=uuid.uuid4().hex,
                        expected_tick=0, expected_structure_revision=1, spawns=[{'template_id': A}])
        self.assertGreater(int(later['spawned'][0], 16), int(spawned, 16))

    def test_template_only_asset_dependency_closure(self):
        model = self.root/'source.glb'
        document, blob = quad([], material={'pbrMetallicRoughness': {'baseColorFactor': [.2, .5, .8, 1]}})
        model.write_bytes(glb(document, blob))
        model_id = self.ok('asset.import', source=self.native(model))['asset']
        image = self.root/'source.png'; image.write_bytes(png(1, 1, [64, 128, 192, 255]))
        image_id = self.ok('asset.image.import', source=self.native(image), color_space='srgb')['asset']
        components = {'Transform': TRANSFORM,
                      'StaticMesh': {'asset': model_id, 'primitive': 0, 'visible': False},
                      'PbrTextures': {'base_color': {'asset': image_id}}}
        self.tx(0, [recipe(components=components), recipe(B, components=components)])
        self.assertEqual(self.ok('world.inspect')['entity_count'], 0)
        original = self.world.read_bytes()
        dependencies = self.ok('world.dependencies')
        self.assertEqual({a['filename'] for a in dependencies['assets']},
                         {model_id+'.pmodel', image_id+'.pimage'})
        self.assertEqual(len(dependencies['assets']), 2)
        self.assertEqual(self.world.read_bytes(), original)

        # Relocate cooked content without the original import sources.
        model.unlink(); image.unlink()
        self.close()
        relocated = self.root/'relocated'/'world.json'
        relocated.parent.mkdir()
        shutil.copy2(self.world, relocated)
        shutil.copytree(Path(str(self.world)+'.assets'), Path(str(relocated)+'.assets'))
        self.world = relocated
        self.open()
        self.assertEqual(self.ok('world.dependencies')['assets'], dependencies['assets'])
        if args.runtime:
            sid = uuid.uuid4().hex
            self.ok('runtime.start', session_id=sid, revision=1)
            frozen = self.ok('runtime.template.get', session_id=sid, tick=0, id=A)['template']
            (self.root/'saves').mkdir()
            self.ok('save.configure', request_id=uuid.uuid4().hex, expected_generation=0,
                    root=self.native(self.root/'saves'))
            self.ok('save.write', request_id=uuid.uuid4().hex, configuration_generation=1,
                    slot='catalog', expected_generation=0, session_id=sid,
                    expected_tick=0, expected_gameplay_revision=0)
            self.tx(1, [recipe(name='Replacement')])
            restored = uuid.uuid4().hex
            self.ok('save.load', request_id=uuid.uuid4().hex, configuration_generation=1,
                    slot='catalog', expected_generation=1, revision=2,
                    expected_session_id=sid, expected_tick=0, expected_gameplay_revision=0,
                    new_session_id=restored)
            self.assertEqual(self.ok('runtime.template.get', session_id=restored, tick=0, id=A)['template'], frozen)
            self.assertEqual(self.ok('template.get', id=A)['template']['name'], 'Replacement')

    @unittest.skipUnless(args.runtime, 'Authoring-only build')
    def test_runtime_freezes_catalog_and_checks_session_tick(self):
        self.tx(0, [recipe(), recipe(B, 'Second')])
        sid = uuid.uuid4().hex
        self.ok('runtime.start', session_id=sid, revision=1)
        frozen = self.ok('runtime.template.get', session_id=sid, tick=0, id=A)
        self.assertEqual((frozen['session_id'], frozen['tick'], frozen['revision'], frozen['id']), (sid, 0, 1, A))
        page = self.ok('runtime.template.query', session_id=sid, tick=0, limit=1)
        self.assertEqual((page['revision'], page['next_after']), (1, A))
        self.assertEqual([t['id'] for t in page['templates']], [A])
        self.tx(1, [recipe(name='Changed'), {'op': 'template.remove', 'id': B}, recipe(C, 'New')])
        self.assertEqual(self.ok('runtime.template.get', session_id=sid, tick=0, id=A), frozen)
        tail = self.ok('runtime.template.query', session_id=sid, tick=0, revision=1, after=A)
        self.assertEqual([t['id'] for t in tail['templates']], [B])
        self.assertEqual(self.ok('template.get', id=A)['template']['name'], 'Changed')
        self.error('runtime.template.get', -32009, session_id=sid, tick=1, id=A)
        self.error('runtime.template.query', -32009, session_id=sid, tick=0, revision=2)
        self.ok('runtime.step', session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1)
        self.error('runtime.template.get', -32009, session_id=sid, tick=0, id=A)
        advanced = self.ok('runtime.template.get', session_id=sid, tick=1, id=A)
        self.assertEqual(advanced['template'], frozen['template'])
        self.assertEqual(advanced['revision'], 1)


suite = unittest.defaultTestLoader.loadTestsFromTestCase(Contract)
result = unittest.TextTestRunner(verbosity=2).run(suite)
if args.evidence:
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps({'tests': result.testsRun, 'skipped': len(result.skipped), 'passed': result.wasSuccessful(),
        'rpc_count': len(CALLS), 'binary_sha256': hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(), 'calls': CALLS}, indent=2)+'\n')
raise SystemExit(0 if result.wasSuccessful() else 1)
