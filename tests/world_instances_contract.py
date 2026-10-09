#!/usr/bin/env python3
"""Original hierarchical recipes: authoring, references, publication and restore."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import unittest
import uuid

from animation_fixture import ribbon
from gltf_fixture import glb
from texture_fixture import png, quad

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--runtime', type=int, choices=(0, 1), default=1)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
BINARY = args.binary.resolve()
OUTPUT = args.output or Path(__file__).resolve().parents[1]/'build'/('world-instances-'+uuid.uuid4().hex)
if OUTPUT.exists():
    parser.error('--output must name a new directory')
OUTPUT.mkdir(parents=True)
CALLS, OWNERS, OBSERVATIONS = [], [], []


def uid(number):
    return f'{number:032x}'


TEMPLATE, ROOT, CAMERA, RIG, FLOOR = map(uid, (500, 100, 101, 200, 1000))
TYPE, TARGET, MEMBERS = map(uid, (3000, 3001, 3002))
TRANSFORM = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
SCHEMA = dict(id=TYPE, name='InstanceReferences', version=2, fields=[
    dict(id=TARGET, name='Target', kind='entity', default=uid(0)),
    dict(id=MEMBERS, name='Members', kind='array', element_kind='entity', capacity=4, default=[])])


def member(name, parent=None, **components):
    return dict(name=name, parent=parent, components=dict(Transform=copy.deepcopy(TRANSFORM), **components))


def recipe(entities=None):
    if entities is None:
        entities = {
            ROOT: member('Movement', CharacterController=dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=CAMERA)),
            CAMERA: member('Camera', ROOT, Camera=dict(vertical_fov=60, near=.1, far=100))}
        entities[CAMERA]['components']['Transform']['position'] = [0, 1.6, 0]
    return dict(op='template.set', id=TEMPLATE, name='Reusable hierarchy', root=ROOT, entities=entities)


class Contract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=OUTPUT)
        self.root = Path(self.temp.name)
        self.world = self.root/'world.json'
        self.process = None
        self.open()

    def native(self, path):
        path = Path(path).resolve()
        return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip() if args.windows_interop else str(path)

    def open(self):
        self.responses = queue.Queue()
        self.log = (self.root/('owner-'+uuid.uuid4().hex+'.log')).open('w', encoding='utf-8')
        self.process = subprocess.Popen([str(BINARY), 'world', self.native(self.world)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, encoding='utf-8')
        self.owner = dict(label=self.id(), pid=self.process.pid, first_call=len(CALLS), exit_code=None)
        OWNERS.append(self.owner)
        process, responses = self.process, self.responses

        def receive():
            try:
                for line in process.stdout:
                    responses.put(line)
            finally:
                responses.put(None)

        self.reader = threading.Thread(target=receive, daemon=True)
        self.reader.start()

    def close(self):
        if self.process is None:
            return
        process = self.process
        try:
            process.stdin.close()
            try:
                code = process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    code = process.wait(timeout=5)
                self.owner['forced_cleanup'] = True
            self.owner.update(exit_code=code, calls=len(CALLS)-self.owner['first_call'])
            self.reader.join(timeout=1)
            self.assertEqual(code, 0, 'Native owner did not exit cleanly; inspect retained log')
            self.assertNotIn('forced_cleanup', self.owner)
        finally:
            process.stdout.close()
            self.log.close()
            self.process = None

    def tearDown(self):
        try:
            self.close()
        finally:
            # Preserve logs, authored worlds and failures for qualification.
            retained = OUTPUT/('retained-'+uuid.uuid4().hex)
            shutil.copytree(self.root, retained)
            self.owner['retained'] = str(retained)
            self.temp.cleanup()

    def request(self, method, **params):
        identity = len(CALLS)+1
        self.process.stdin.write(json.dumps(dict(jsonrpc='2.0', id=identity, method=method, params=params))+'\n')
        self.process.stdin.flush()
        try:
            line = self.responses.get(timeout=60)
        except queue.Empty:
            self.fail('Bounded RPC response timeout')
        self.assertIsNotNone(line, 'Native owner exited before response')
        value = json.loads(line)
        self.assertEqual(value['id'], identity)
        CALLS.append(dict(method=method, passed='result' in value))
        return value

    def ok(self, method, **params):
        value = self.request(method, **params)
        self.assertIn('result', value, value)
        return value['result']

    def error(self, method, code, **params):
        value = self.request(method, **params)
        self.assertEqual(value.get('error', {}).get('code'), code, value)
        return value

    def tx(self, revision, ops, **extra):
        return self.ok('world.transact', request_id=extra.pop('request_id', uuid.uuid4().hex), base_revision=revision, ops=ops, **extra)

    def start(self, revision):
        self.session = uuid.uuid4().hex
        self.ok('runtime.start', session_id=self.session, revision=revision)

    def structure(self, revision, tick=0, **extra):
        return self.ok('runtime.structure.transact', session_id=self.session, request_id=uuid.uuid4().hex,
                       expected_tick=tick, expected_structure_revision=revision, **extra)

    def instance(self, root, tick=0, structure=None):
        guards = {} if structure is None else dict(expected_structure_revision=structure)
        return self.ok('runtime.instance', session_id=self.session, id=root, tick=tick, **guards)

    def ground(self):
        return [dict(op='entity.create', id=FLOOR, name='Ground'),
                dict(op='component.set', id=FLOOR, type='Transform', value=dict(TRANSFORM, position=[0, -.5, 0])),
                dict(op='component.set', id=FLOOR, type='BoxCollider', value=dict(
                    half_extents=[50, .5, 50], motion='static', mass=1, friction=.5, restitution=0))]

    def test_disjoint_shapes_preview_history_and_local_namespace(self):
        description = self.ok('world.describe')
        self.assertGreaterEqual(description['schema_revision'], 67)
        self.assertIn('runtime.instance', description['methods'])
        legacy = dict(op='template.set', id=uid(600), name='Legacy', components=dict(Transform=TRANSFORM))
        self.tx(0, [legacy])
        self.assertEqual(self.ok('template.get', id=uid(600))['template'], dict(name='Legacy', components=dict(Transform=TRANSFORM)))
        before = self.world.read_bytes()
        self.assertFalse(self.tx(1, [recipe()], preview=True)['committed'])
        self.assertEqual(self.world.read_bytes(), before)
        operation = recipe()
        receipt = dict(request_id=uuid.uuid4().hex, base_revision=1, ops=[operation])
        self.ok('world.transact', **receipt)
        self.assertTrue(self.ok('world.transact', **receipt)['replayed'])
        self.assertEqual(self.ok('world.inspect')['entity_count'], 0)
        self.assertEqual(self.ok('template.get', id=TEMPLATE)['template']['entities'], operation['entities'])
        rows = self.ok('template.query', revision=2)['templates']
        row = next(item for item in rows if item['id'] == TEMPLATE)
        self.assertEqual((row['root'], row['entity_count']), (ROOT, 2))
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=2)
        self.error('template.get', -32004, id=TEMPLATE)
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=3)
        # Local IDs are a separate namespace, even when ordinary IDs are retired.
        self.tx(4, [dict(op='entity.create', id=ROOT, name='Unrelated'), dict(op='entity.delete', id=ROOT, recursive=True)])
        self.assertEqual(self.ok('template.get', id=TEMPLATE)['template']['root'], ROOT)
        self.close();self.open()
        self.assertEqual(self.ok('template.get', id=TEMPLATE)['template']['entities'], operation['entities'])

    def test_invalid_graphs_are_atomic_and_external_custom_handles_are_deferred(self):
        operation = recipe()
        operation['entities'][ROOT]['components']['game:'+TYPE] = {TARGET: uid(9000), MEMBERS: [CAMERA]}
        self.tx(0, [dict(op='component.schema.set', schema=SCHEMA), operation])
        saved, history = self.world.read_bytes(), self.ok('world.history')
        variants = []
        mixed = copy.deepcopy(operation);mixed['components'] = dict(Transform=TRANSFORM);variants.append(mixed)
        incomplete = copy.deepcopy(operation);incomplete.pop('root');variants.append(incomplete)
        wrong = copy.deepcopy(operation);wrong['root'] = uid(999);variants.append(wrong)
        forest = copy.deepcopy(operation);forest['entities'][CAMERA]['parent'] = None;variants.append(forest)
        cyclic = copy.deepcopy(operation);cyclic['entities'][CAMERA]['parent'] = CAMERA;variants.append(cyclic)
        external = copy.deepcopy(operation);external['entities'][ROOT]['components']['CharacterController']['camera'] = uid(9000);variants.append(external)
        bad_native = copy.deepcopy(operation);bad_native['entities'][ROOT]['components']['CharacterController'].pop('camera');variants.append(bad_native)
        oversized = recipe({uid(i+1): member('Node', None if i == 0 else uid(1)) for i in range(1025)})
        oversized['root'] = uid(1);variants.append(oversized)
        for candidate in variants:
            self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=1,
                       ops=[dict(op='template.set', id=uid(700), name='Should rollback', components=dict(Transform=TRANSFORM)), candidate])
            self.assertEqual(self.world.read_bytes(), saved)
            self.assertEqual(self.ok('world.history'), history)
        if args.runtime:
            self.start(1)
            before = self.ok('runtime.inspect', session_id=self.session)
            self.error('runtime.structure.transact', -32602, session_id=self.session, request_id=uuid.uuid4().hex,
                       expected_tick=0, expected_structure_revision=0, spawns=[dict(template_id=TEMPLATE)])
            self.assertEqual(self.ok('runtime.inspect', session_id=self.session), before)

    def test_nested_dependency_reference_pagination_and_source_independent_reopen(self):
        doc, blob = quad([], material={'pbrMetallicRoughness': {'baseColorFactor': [.2, .5, .8, 1]}})
        source = self.root/'original.glb';source.write_bytes(glb(doc, blob))
        asset = self.ok('asset.import', source=self.native(source))['asset']
        image = self.root/'original.png';image.write_bytes(png(1, 1, [64, 128, 192, 255]))
        texture = self.ok('asset.image.import', source=self.native(image), color_space='srgb')['asset']
        nodes = {ROOT: member('Root'), CAMERA: member('Child', ROOT, StaticMesh=dict(asset=asset, primitive=0, visible=False),
                 PbrTextures=dict(base_color=dict(asset=texture)))}
        self.tx(0, [recipe(nodes)])
        dependencies = self.ok('world.dependencies')
        self.assertEqual({item['filename'] for item in dependencies['assets']}, {asset+'.pmodel', texture+'.pimage'})
        rows, cursor = [], None
        while True:
            query = dict(revision=1, owner=dict(kind='template', id=TEMPLATE), limit=1)
            if cursor is not None:
                query['after'] = cursor
            page = self.ok('world.asset.references', **query)
            rows.extend(page['edges']);cursor = page['next_after']
            if cursor is None:
                break
            self.assertLess(len(rows), 8)
        self.assertEqual([item['component'] for item in rows], ['PbrTextures', 'StaticMesh'])
        self.assertEqual(rows[0]['path'], '/entities/'+CAMERA+'/components/PbrTextures/base_color/asset')
        self.assertEqual(rows[1]['path'], '/entities/'+CAMERA+'/components/StaticMesh/asset')
        self.assertEqual(self.ok('world.asset.references', revision=1, asset=asset)['edges'], [rows[1]])
        self.close();source.unlink();image.unlink()
        relocated = self.root/'relocated'/'world.json';relocated.parent.mkdir()
        shutil.copy2(self.world, relocated)
        shutil.copytree(Path(str(self.world)+'.assets'), Path(str(relocated)+'.assets'))
        self.world = relocated;self.open()
        self.assertEqual(self.ok('world.dependencies')['assets'], dependencies['assets'])
        OBSERVATIONS.append(dict(group='nested_dependencies', assets=len(dependencies['assets']), edges=rows, sources_removed=True))

    @unittest.skipUnless(args.runtime, 'Simulation not built')
    def test_internal_scalar_array_references_and_whole_instance_removal(self):
        operation = recipe()
        operation['entities'][ROOT]['components']['game:'+TYPE] = {TARGET: CAMERA, MEMBERS: [ROOT, CAMERA, FLOOR]}
        self.tx(0, self.ground()+[dict(op='component.schema.set', schema=SCHEMA), operation])
        self.start(1)
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=0,
                      expected_structure_revision=0, spawns=[dict(template_id=TEMPLATE), dict(template_id=TEMPLATE)])
        born = self.ok('runtime.structure.transact', **params)
        self.assertEqual(self.ok('runtime.structure.transact', **params), dict(born, replayed=True))
        first, second = [self.instance(root, structure=1) for root in born['spawned']]
        self.assertEqual(set(first['nodes']), {ROOT, CAMERA})
        self.assertEqual(first['nodes'][ROOT], first['root'])
        self.assertFalse(set(first['nodes'].values()) & set(second['nodes'].values()))
        for instance in (first, second):
            values = self.ok('runtime.component.get', session_id=self.session, tick=0, id=instance['root'], type=TYPE)['values']
            self.assertEqual(values[TARGET], instance['nodes'][CAMERA])
            self.assertEqual(values[MEMBERS], [instance['root'], instance['nodes'][CAMERA], FLOOR])
        self.error('runtime.instance', -32009, session_id=self.session, id=first['root'], tick=0, expected_structure_revision=0)
        self.error('runtime.instance', -32004, session_id=self.session, id=first['nodes'][CAMERA], tick=0)
        self.error('runtime.structure.transact', -32602, session_id=self.session, request_id=uuid.uuid4().hex,
                   expected_tick=0, expected_structure_revision=1, despawns=[first['nodes'][CAMERA]])
        self.structure(1, despawns=[first['root']])
        for handle in first['nodes'].values():
            self.error('runtime.entity', -32004, session_id=self.session, id=handle)
        self.assertEqual(self.instance(second['root'], structure=2)['nodes'], second['nodes'])
        self.assertEqual(self.ok('runtime.inspect', session_id=self.session)['entities'], 3)
        OBSERVATIONS.append(dict(group='whole_instance', first=first, second=second, surviving=second['nodes']))

    @unittest.skipUnless(args.runtime, 'Simulation not built')
    def test_imported_animated_instance_new_clock_and_save_restore(self):
        doc, blob = ribbon();source = self.root/'original-ribbon.glb';source.write_bytes(glb(doc, blob))
        original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        asset = self.ok('asset.import', source=self.native(source))['asset']
        self.tx(0, [dict(op='asset.instantiate', id=RIG, name='Original hierarchy', asset=asset)])
        nodes = copy.deepcopy(json.loads(self.world.read_text())['entities'])
        nodes[RIG]['parent'] = ROOT
        nodes[RIG]['components']['AnimationRig'].update(clip=0, playing=True, loop=False)
        nodes.update(recipe()['entities'])
        self.tx(1, [dict(op='entity.delete', id=RIG, recursive=True)]+self.ground()+[recipe(nodes)])
        dependencies = self.ok('world.dependencies')
        self.assertEqual({item['filename'] for item in dependencies['assets']}, {asset+'.pmodel'})
        self.start(2)
        first_root = self.structure(0, spawns=[dict(template_id=TEMPLATE)])['spawned'][0]
        first = self.instance(first_root, structure=1)
        self.ok('runtime.step', session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=0, expected_structure_revision=1, ticks=30)
        second_root = self.structure(1, tick=30, spawns=[dict(template_id=TEMPLATE, transform=dict(TRANSFORM, position=[3, 0, 5]))])['spawned'][0]
        second = self.instance(second_root, tick=30, structure=2)
        for instance, expected_time in ((first, .5), (second, 0)):
            state = self.ok('runtime.entity', session_id=self.session, id=instance['nodes'][RIG], tick=30)
            self.assertAlmostEqual(state['animation']['time'], expected_time)
        # The original authored clip moves only node1.x at one metre/second.
        local_tip = next(identity for identity, node in nodes.items() if node['components'].get('RigNode', {}).get('node') == 1)
        self.assertAlmostEqual(self.ok('runtime.entity', session_id=self.session, id=first['nodes'][local_tip])['local_transform']['position'][0], .5)
        self.assertAlmostEqual(self.ok('runtime.entity', session_id=self.session, id=second['nodes'][local_tip])['local_transform']['position'][0], 0)
        (self.root/'saves').mkdir()
        self.ok('save.configure', request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(self.root/'saves'))
        self.ok('save.write', request_id=uuid.uuid4().hex, configuration_generation=1, slot='instances', expected_generation=0,
                session_id=self.session, expected_tick=30, expected_gameplay_revision=0, expected_structure_revision=2)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), original_hash)
        source.unlink()  # Only this test's owned original fixture is removed.
        self.close();self.open()
        self.ok('save.configure', request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(self.root/'saves'))
        restored = uuid.uuid4().hex
        self.ok('save.load', request_id=uuid.uuid4().hex, configuration_generation=1, slot='instances', expected_generation=1,
                revision=2, expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
                expected_structure_revision=None, new_session_id=restored)
        self.session = restored
        self.assertEqual(self.instance(first_root, tick=30, structure=2)['nodes'], first['nodes'])
        self.assertEqual(self.instance(second_root, tick=30, structure=2)['nodes'], second['nodes'])
        self.ok('runtime.step', session_id=restored, request_id=uuid.uuid4().hex, expected_tick=30, expected_structure_revision=2, ticks=15)
        for instance, expected_time in ((first, .75), (second, .25)):
            state = self.ok('runtime.entity', session_id=restored, id=instance['nodes'][RIG], tick=45)
            self.assertAlmostEqual(state['animation']['time'], expected_time)
        self.structure(2, tick=45, despawns=[first_root, second_root])
        self.assertEqual(self.ok('runtime.inspect', session_id=restored)['entities'], 1)
        self.assertFalse(source.exists())
        OBSERVATIONS.append(dict(group='animated_restore', initial_instances=[first, second], restored_session=restored,
                                 original_sha256=original_hash, continued_tick=45, owned_source_removed=True))


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Contract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    evidence = dict(passed=result.wasSuccessful(), tests=result.testsRun, skipped=len(result.skipped),
                    rpc_count=len(CALLS), owners=OWNERS, observations=OBSERVATIONS,
                    binary_sha256=hashlib.sha256(BINARY.read_bytes()).hexdigest(),
                    verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    calls=CALLS, scope='Original analytic content; CPU/headless authoring and simulation, no graphics/performance claim')
    (OUTPUT/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
