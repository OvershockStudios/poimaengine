#!/usr/bin/env python3
"""Native static triangle collision through real imported glTF packages and RPC.

Original fence, open stair and floor fixtures contain deliberately empty space.
The Python harness authors and measures; native code owns all collision/simulation.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid
from animation_fixture import ribbon
from gltf_fixture import glb

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
parser.add_argument('--runtime-root', type=Path, help='Optional installed runtime for an actual bundle export.')
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--evidence', type=Path)
args = parser.parse_args()
BINARY = str(args.binary.resolve())
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT/'build/mesh-collision-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS, MEASUREMENTS = [], []


def uid(n):
    return f'{n:032x}'


def rpc(method, **params):
    return {'jsonrpc': '2.0', 'method': method, 'params': params}


def create(n, parent=None):
    return {'op': 'entity.create', 'id': uid(n), 'name': f'Collision fixture {n}',
            'parent': uid(parent) if parent is not None else None}


def component(n, kind, value):
    return {'op': 'component.set', 'id': uid(n) if isinstance(n, int) else n, 'type': kind, 'value': value}


def transform(n, position=(0, 0, 0), scale=(1, 1, 1), yaw=0):
    angle = math.radians(yaw)/2
    return component(n, 'Transform', {'position': list(position), 'scale': list(scale),
                                     'rotation': [0, math.sin(angle), 0, math.cos(angle)]})


def box(n, motion='static', half=(.5, .5, .5)):
    return component(n, 'BoxCollider', {'half_extents': list(half), 'motion': motion,
                                       'mass': 1, 'friction': .5, 'restitution': 0})


def controller(n, x=0, z=2):
    return [create(n), transform(n, (x, 1, z)),
            component(n, 'CharacterController', {'radius': .2, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': uid(n+1)}),
            create(n+1, n), transform(n+1, (0, 1.6, 0)),
            component(n+1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100})]


def fixture(degenerate=False):
    """Three independent local-space primitives, preserving triangle ordinals."""
    blob, views, accessors, primitives = bytearray(), [], [], []

    def accessor(rows, kind, fmt='f'):
        while len(blob) % 4:
            blob.append(0)
        start = len(blob)
        flat = [value for row in rows for value in row]
        blob.extend(struct.pack('<'+fmt*len(flat), *flat))
        views.append({'buffer': 0, 'byteOffset': start, 'byteLength': len(blob)-start})
        accessors.append({'bufferView': len(views)-1, 'componentType': 5126 if fmt == 'f' else 5125,
                          'count': len(rows), 'type': kind})
        return len(accessors)-1

    def primitive(quads, normal):
        positions, normals, indices = [], [], []
        for points in quads:
            start = len(positions)
            positions.extend(points); normals.extend([normal]*4)
            indices.extend(start+i for i in [0, 1, 2, 0, 2, 3])
        if degenerate and not primitives:
            indices[:3] = [0, 0, 1]
        p = accessor(positions, 'VEC3')
        accessors[p].update(min=[min(row[c] for row in positions) for c in range(3)],
                            max=[max(row[c] for row in positions) for c in range(3)])
        attributes = {'POSITION': p, 'NORMAL': accessor(normals, 'VEC3')}
        index = accessor([(i,) for i in indices], 'SCALAR', 'I')
        primitives.append({'attributes': attributes, 'indices': index})

    def vertical(x0, x1, y0, y1):
        return [(x0, y0, 0), (x1, y0, 0), (x1, y1, 0), (x0, y1, 0)]

    def horizontal(y, z0, z1, width=1):
        return [(-width, y, z1), (width, y, z1), (width, y, z0), (-width, y, z0)]

    # Rail triangles0..3; overhead bar4..5 leaves a character-sized opening.
    primitive([vertical(-1, -.7, 0, 2.8), vertical(.7, 1, 0, 2.8), vertical(-.7, .7, 2.5, 2.7)], (0, 0, 1))
    # No riser faces: a horizontal ray between tread heights must pass through.
    primitive([horizontal(.3, -1, 0), horizontal(.6, -2, -1), horizontal(.9, -3, -2)], (0, 1, 0))
    primitive([horizontal(0, -5, 5, width=5)], (0, 1, 0))
    doc = {'asset': {'version': '2.0', 'generator': 'Poima original collision gaps'},
           'buffers': [{'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors,
           'meshes': [{'primitives': primitives}], 'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    return doc, bytes(blob)


class MeshCollision(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.directory.name)
        self.world = self.root/'triangle world.json'
        self.asset = self.import_model(*fixture())

    def tearDown(self):
        self.directory.cleanup()

    def native(self, path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    def requests(self, *rows):
        requests = [{**row, 'id': i+1} for i, row in enumerate(rows)]
        p = subprocess.run([BINARY, 'world', self.native(self.world)],
                           input=''.join(json.dumps(row)+'\n' for row in requests),
                           capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.assertEqual(p.returncode, 0, p.stdout+p.stderr)
        responses = [json.loads(line) for line in p.stdout.splitlines()]
        CALLS.append({'test': self.id(), 'requests': requests, 'responses': responses, 'stderr': p.stderr})
        self.assertEqual(len(responses), len(requests), p.stdout+p.stderr)
        self.assertEqual([r['id'] for r in responses], list(range(1, len(rows)+1)))
        return responses

    def result(self, reply):
        self.assertIn('result', reply, reply)
        return reply['result']

    def error(self, reply, code):
        self.assertEqual(reply.get('error', {}).get('code'), code, reply)

    def import_model(self, doc, blob):
        path = self.root/(uuid.uuid4().hex+'.glb');path.write_bytes(glb(doc, blob))
        asset = self.result(self.requests(rpc('asset.import', source=self.native(path)))[0])['asset']
        path.unlink()  # Every subsequent load must use its immutable package.
        return asset

    def collider(self, n, primitive=0, asset=None):
        return component(n, 'MeshCollider', {'asset': asset or self.asset, 'primitive': primitive, 'friction': .5, 'restitution': 0})

    def transaction(self, ops, revision=0, **extra):
        return rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=revision, ops=ops, **extra)

    def start(self, revision=1, session=900):
        return rpc('runtime.start', session_id=uid(session), revision=revision)

    def step(self, tick, ticks, inputs=(), session=900, **extra):
        params = {'session_id': uid(session), 'request_id': uuid.uuid4().hex, 'expected_tick': tick,
                  'ticks': ticks, 'inputs': list(inputs), **extra}
        return rpc('runtime.step', **params)

    def entity(self, n, tick=None):
        return rpc('runtime.entity', session_id=uid(900), id=uid(n), **({'tick': tick} if tick is not None else {}))

    def ray(self, origin, direction=(0, 0, -1), distance=10, tick=0, ignore=()):
        return rpc('runtime.raycast', session_id=uid(900), tick=tick, origin=list(origin),
                   direction=list(direction), distance=distance, ignore=[uid(n) for n in ignore])

    def test_schema_numeric_validation_component_conflicts_and_history(self):
        self.result(self.requests(self.transaction([create(1), self.collider(1)]))[0])
        before = self.world.read_bytes()
        schema = self.result(self.requests(rpc('world.describe'))[0])['components']['MeshCollider']
        self.assertEqual(set(schema['required']), {'asset', 'primitive', 'friction', 'restitution'})
        value = self.collider(1)['value']
        bad = [{k: v for k, v in value.items() if k != field} for field in value]
        bad += [{**value, key: val} for key, val in [('asset', 'x'*64), ('primitive', -1), ('primitive', 10000),
                ('primitive', True), ('friction', -.01), ('friction', 2.01), ('friction', True),
                ('restitution', -1), ('restitution', 1.01), ('restitution', '0'), ('motion', 'dynamic')]]
        invalid = [self.transaction([{'op': 'entity.rename', 'id': uid(1), 'name': 'Must roll back'},
                                     component(1, 'MeshCollider', v)], revision=1) for v in bad]
        invalid += [self.transaction([box(1)], revision=1),
                    self.transaction([component(1, 'CharacterController', {'radius': .2, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': uid(99)})], revision=1)]
        for reply in self.requests(*invalid):
            self.error(reply, -32602)
        self.assertEqual(self.world.read_bytes(), before)
        rows = self.requests(rpc('entity.query', component='MeshCollider'),
             self.transaction([{'op': 'component.remove', 'id': uid(1), 'type': 'MeshCollider'}], revision=1),
             rpc('world.undo', request_id=uuid.uuid4().hex, base_revision=2), rpc('entity.get', id=uid(1), component='MeshCollider'))
        self.assertEqual([e['id'] for e in self.result(rows[0])['entities']], [uid(1)])
        self.assertEqual(self.result(rows[-1])['value'], value)
        self.assertEqual(self.result(rows[2])['revision'], 3)

    @unittest.skipUnless(args.runtime, 'Native simulation disabled in this build.')
    def test_fence_holes_two_sided_hits_and_source_triangle_ordinals(self):
        self.result(self.requests(self.transaction([create(1), self.collider(1),
             component(1, 'StaticMesh', {'asset': self.asset, 'primitive': 0, 'visible': False}),
             create(2), transform(2, (0, 1.5, -2)), box(2, half=(1.2, 1.5, .2))]))[0])
        saved = self.world.read_bytes()
        points = [(-.9, .2, 0), (-.95, 2, 1), (.9, .2, 2), (.8, 2, 3), (0, 2.55, 4)]
        requests = [self.start()]
        for x, y, _ in points:
            requests += [self.ray((x, y, 3)), self.ray((x, y, -1), (0, 0, 1))]
        requests += [self.ray((0, 1, 3)), self.ray((0, 1, 3), ignore=(2,)), self.ray((2, 1, 3))]
        rows = self.requests(*requests)
        for i, (_, _, triangle) in enumerate(points):
            for side, distance in [(0, 3), (1, 1)]:
                hit = self.result(rows[1+i*2+side])['hit']
                self.assertEqual((hit['entity'], hit['triangle']), (uid(1), triangle))
                self.assertAlmostEqual(hit['distance'], distance, delta=1e-5)
                self.assertGreater(hit['normal'][2], .999)
                MEASUREMENTS.append({'case': 'fence', 'backface': bool(side), 'hit': hit})
        behind = self.result(rows[-3])['hit']
        self.assertEqual(behind['entity'], uid(2));self.assertIsNone(behind['triangle'])
        self.assertAlmostEqual(behind['distance'], 4.8, delta=1e-5)
        self.assertIsNone(self.result(rows[-2])['hit']);self.assertIsNone(self.result(rows[-1])['hit'])
        self.assertEqual(self.world.read_bytes(), saved)

    @unittest.skipUnless(args.runtime, 'Native simulation disabled in this build.')
    def test_open_stair_treads_and_empty_risers(self):
        self.result(self.requests(self.transaction([create(1), self.collider(1, primitive=1),
               create(2), transform(2, (0, 1, -4)), box(2, half=(1, 1, .1))]))[0])
        rows = self.requests(self.start(), *[self.ray((.3, 2, -i-.2), (0, -1, 0)) for i in range(3)],
                self.ray((0, .45, 1)), self.ray((0, .45, 1), ignore=(2,)))
        for i, reply in enumerate(rows[1:4]):
            hit = self.result(reply)['hit']
            self.assertEqual((hit['entity'], hit['triangle']), (uid(1), 2*i))
            self.assertAlmostEqual(hit['distance'], 2-.3*(i+1), delta=1e-5)
            self.assertGreater(hit['normal'][1], .999)
        self.assertEqual(self.result(rows[4])['hit']['entity'], uid(2))
        self.assertIsNone(self.result(rows[5])['hit'])

    @unittest.skipUnless(args.runtime, 'Native simulation disabled in this build.')
    def test_parent_translation_rotation_and_nonuniform_static_scale(self):
        ops = [create(5), transform(5, (5, 2, -1), scale=(2, 1, 3), yaw=90),
               create(1, 5), transform(1, (1, 0, 0), scale=(1, 2, 1)), self.collider(1)]
        self.result(self.requests(self.transaction(ops))[0])
        rows = self.requests(self.start(), self.ray((9, 2.4, -1.2), (-1, 0, 0)),
                             self.ray((1, 2.4, -1.2), (1, 0, 0)))
        for row in rows[1:]:
            hit = self.result(row)['hit']
            self.assertEqual((hit['entity'], hit['triangle']), (uid(1), 0))
            self.assertAlmostEqual(hit['distance'], 4, delta=1e-4)
            for actual, expected in zip(hit['position'], (5, 2.4, -1.2)):
                self.assertAlmostEqual(actual, expected, delta=1e-4)
            self.assertGreater(hit['normal'][0], .999)

    @unittest.skipUnless(args.runtime, 'Native simulation disabled in this build.')
    def test_character_gap_and_blocked_rail_with_dynamic_body_contacts(self):
        ops = [create(1), self.collider(1), create(2), self.collider(2, primitive=2),
               create(30), transform(30, (2, 2, 1)), box(30, motion='dynamic', half=(.25, .25, .25))]
        ops += controller(10) + controller(20, x=-.85)
        self.result(self.requests(self.transaction(ops))[0]);before = self.world.read_bytes()
        movement = [{'entity': uid(n), 'move': [0, 1]} for n in (10, 20)]
        rows = self.requests(self.start(), self.step(0, 120), self.entity(10), self.entity(20), self.entity(30),
                             self.step(120, 90, movement), self.entity(10), self.entity(20), self.entity(30), self.entity(2))
        for i in (2, 3):
            self.assertEqual(self.result(rows[i])['ground'], 'on_ground')
        body = self.result(rows[4]);self.assertAlmostEqual(body['world_matrix'][13], .25, delta=.04)
        passed, blocked = self.result(rows[6]), self.result(rows[7])
        self.assertLess(passed['world_matrix'][14], -2)
        self.assertGreater(blocked['world_matrix'][14], .15);self.assertLess(blocked['world_matrix'][14], .5)
        self.assertEqual(passed['ground'], 'on_ground');self.assertEqual(blocked['ground'], 'on_ground')
        self.assertAlmostEqual(self.result(rows[8])['world_matrix'][13], .25, delta=.04)
        floor = self.result(rows[9]);self.assertTrue(floor['has_body']);self.assertEqual(floor['motion'], 'static')
        self.assertEqual(self.world.read_bytes(), before)
        MEASUREMENTS.append({'case': 'contacts', 'gap_character': passed, 'rail_character': blocked, 'resting_box': body})

    def test_deferred_missing_primitive_weighted_and_corrupt_package_preserve_authoring(self):
        weighted = self.import_model(*ribbon())
        self.result(self.requests(self.transaction([create(1), self.collider(1)]))[0])
        rev = 1
        for asset, primitive in [('f'*64, 0), (self.asset, 9999), (weighted, 0)]:
            self.result(self.requests(self.transaction([self.collider(1, primitive, asset)], revision=rev))[0]);rev += 1
            before = self.world.read_bytes()
            queries = [rpc('world.dependencies')]+([self.start(revision=rev)] if args.runtime else [])
            for reply in self.requests(*queries):
                self.error(reply, -32050)
            self.assertEqual(self.world.read_bytes(), before)
        self.result(self.requests(self.transaction([self.collider(1)], revision=rev))[0]);rev += 1
        package = Path(str(self.world)+'.assets')/(self.asset+'.pmodel')
        original, before = package.read_bytes(), self.world.read_bytes()
        package.write_bytes(original[:-1]+bytes([original[-1]^1]))
        for reply in self.requests(rpc('world.dependencies'), *([self.start(revision=rev)] if args.runtime else [])):
            self.error(reply, -32050)
        self.assertEqual(self.world.read_bytes(), before)
        package.write_bytes(original)
        self.result(self.requests(rpc('world.dependencies'))[0])
        if args.runtime:
            self.result(self.requests(self.start(revision=rev))[0])
        self.assertEqual(self.world.read_bytes(), before)

    def test_moving_sheared_degenerate_and_animation_owned_colliders_reject(self):
        self.result(self.requests(self.transaction([create(5), box(5, motion='kinematic'), create(1, 5), self.collider(1)]))[0])
        before = self.world.read_bytes()
        for reply in self.requests(rpc('world.dependencies'), *([self.start()] if args.runtime else [])):
            self.error(reply, -32602)
        self.assertEqual(self.world.read_bytes(), before)
        # Nonuniform parent scale plus a rotated child introduces shear.
        self.result(self.requests(self.transaction([{'op': 'component.remove', 'id': uid(5), 'type': 'BoxCollider'},
                      transform(5, scale=(2, 1, 1)), transform(1, yaw=45)], revision=1))[0])
        for reply in self.requests(rpc('world.dependencies'), *([self.start(revision=2)] if args.runtime else [])):
            self.error(reply, -32602)
        bad = self.import_model(*fixture(degenerate=True))
        self.result(self.requests(self.transaction([transform(5), transform(1), self.collider(1, asset=bad)], revision=2))[0])
        for reply in self.requests(rpc('world.dependencies'), *([self.start(revision=3)] if args.runtime else [])):
            self.error(reply, -32602)
        rig = uid(100);animated = self.import_model(*ribbon())
        self.result(self.requests(self.transaction([{'op': 'component.remove', 'id': uid(1), 'type': 'MeshCollider'},
            {'op': 'asset.instantiate', 'id': rig, 'asset': animated, 'name': 'Animation ownership fixture'}], revision=3))[0])
        bone = hashlib.sha256(f'poima.instance.v1/{rig}/node/1'.encode()).hexdigest()[:32]
        before = self.world.read_bytes()
        self.error(self.requests(self.transaction([self.collider(bone)], revision=4))[0], -32602)
        self.assertEqual(self.world.read_bytes(), before)

    @unittest.skipUnless(args.runtime, 'Native simulation disabled in this build.')
    def test_failed_mid_batch_animation_restores_mesh_contact_runtime(self):
        doc, blob = ribbon(cubic=True);doc['animations'][0]['channels'][0]['target']['path'] = 'scale'
        accessor = doc['accessors'][doc['animations'][0]['samplers'][0]['output']]
        offset = doc['bufferViews'][accessor['bufferView']]['byteOffset']
        data = bytearray(blob)
        values = [(0, 0, 0), (1, 1, 1), (-4, 0, 0), (4, 0, 0), (1, 1, 1), (0, 0, 0)]
        struct.pack_into('<18f', data, offset, *[v for row in values for v in row])
        animated = self.import_model(doc, bytes(data));rig = uid(100)
        self.result(self.requests(self.transaction([create(2), self.collider(2, primitive=2),
             create(30), transform(30, (2, 2, 1)), box(30, motion='dynamic', half=(.25, .25, .25)),
             {'op': 'asset.instantiate', 'id': rig, 'asset': animated, 'name': 'Rollback curve'}]+controller(10)))[0])
        before = self.world.read_bytes()
        command = {'entity': rig, 'clip': 0, 'time': 0, 'speed': 1, 'loop': False, 'playing': True}
        failed_receipt = uuid.uuid4().hex
        rows = self.requests(self.start(), self.step(0, 10, animations=[command]), self.entity(10), self.entity(30),
             self.ray((4, 2, 0), (0, -1, 0), tick=10),
             self.step(10, 50, request_id=failed_receipt), self.entity(10), self.entity(30),
             self.ray((4, 2, 0), (0, -1, 0), tick=10), rpc('runtime.inspect', session_id=uid(900)),
             self.step(10, 1, request_id=failed_receipt, animations=[{**command, 'clip': None, 'playing': False}]))
        self.result(rows[1]);self.error(rows[5], -32040)
        for a, b in [(2, 6), (3, 7), (4, 8)]:
            self.assertEqual(self.result(rows[a]), self.result(rows[b]))
        self.assertEqual(self.result(rows[9])['tick'], 10);self.assertEqual(self.result(rows[10])['tick'], 11)
        self.assertEqual(self.world.read_bytes(), before)

    def test_collision_only_export_dependency_closure(self):
        # Create a real project so inspection exercises the same closure used by export.
        project = self.root/'Collision-only project'
        def cli(*arguments):
            p = subprocess.run([BINARY, *arguments], capture_output=True, text=True, encoding='utf-8', timeout=90)
            reply = json.loads(p.stdout);CALLS.append({'test': self.id(), 'argv': arguments, 'reply': reply, 'stderr': p.stderr})
            self.assertEqual(p.returncode, 0, reply);self.assertEqual(reply['status'], 'ok');return reply['result']
        cli('project', 'create', self.native(project), '--name', 'Triangle fixture')
        manifest = project/'project.json';spec = json.loads(manifest.read_text())
        self.world = project/spec['entry']['world']
        asset = self.import_model(*fixture())
        revision = json.loads(self.world.read_text())['revision']
        self.result(self.requests(self.transaction([create(1000), self.collider(1000, asset=asset)], revision=revision))[0])
        before = self.world.read_bytes()
        closure = self.result(self.requests(rpc('world.dependencies'))[0])
        expected = asset+'.pmodel'
        self.assertEqual([v['filename'] for v in closure['assets']], [expected])
        package = Path(str(self.world)+'.assets')/expected
        self.assertEqual(closure['assets'][0]['bytes'], package.stat().st_size)
        self.assertEqual(closure['assets'][0]['sha256'], hashlib.sha256(package.read_bytes()).hexdigest())
        inspected = cli('project', 'inspect', self.native(manifest))
        self.assertEqual(inspected['assets'], closure['assets'])
        self.assertEqual(self.world.read_bytes(), before)
        saved = json.loads(before)['entities'][uid(1000)]['components']
        self.assertIn('MeshCollider', saved);self.assertNotIn('StaticMesh', saved)
        if args.runtime_root:
            exported = self.root/'Exported collision game'
            cli('project', 'build', self.native(manifest), '--output', self.native(exported), '--runtime', self.native(args.runtime_root))
            copied = list(exported.rglob(expected));self.assertEqual(len(copied), 1)
            self.assertEqual(copied[0].read_bytes(), package.read_bytes())
            cli('game', 'inspect', self.native(exported/'game.json'))
        MEASUREMENTS.append({'case': 'collision_only_dependency', 'assets': closure['assets'], 'actual_export': bool(args.runtime_root)})


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(MeshCollision))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({'passed': result.wasSuccessful(), 'tests': result.testsRun, 'skipped': len(result.skipped),
            'runtime': bool(args.runtime), 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'calls': CALLS,
            'measurements': MEASUREMENTS}, indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
