#!/usr/bin/env python3
"""Public fixed-tick crossfades checked against original analytic glTF curves."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import unittest
import uuid
from animation_fixture import ribbon
from gltf_fixture import glb

ROOT = Path(__file__).resolve().parents[1]
REPORTS = []
ARGS = None


def uid(value):
    return f'{value:032x}'


def node(root, index):
    return hashlib.sha256(f'poima.instance.v1/{root}/node/{index}'.encode()).hexdigest()[:32]


def transform(position=(0, 0, 0), scale=(1, 1, 1)):
    return {'position': list(position), 'rotation': [0, 0, 0, 1], 'scale': list(scale)}


def component(entity, kind, value):
    return {'op': 'component.set', 'id': entity, 'type': kind, 'value': value}


def command(entity, clip=0, time=0, speed=1, loop=True, playing=True, **extra):
    return dict(entity=entity, clip=clip, time=time, speed=speed, loop=loop, playing=playing, **extra)


def blend_fixture():
    """A(t)=(t,1,0), B(t)=(2-t,3,0); C has invalid interior cubic scale.

    An additional rotation-only clip tests authored missing-channel defaults.
    Clip endpoints are deliberately valid; C must fail while being sampled.
    """
    doc, data = ribbon()
    blob = bytearray(data)

    def accessor(rows, kind):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        values = [x for row in rows for x in row]
        blob.extend(struct.pack('<' + 'f' * len(values), *values))
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(blob) - offset})
        doc['accessors'].append({'bufferView': len(doc['bufferViews']) - 1, 'componentType': 5126,
                                 'count': len(rows), 'type': kind})
        return len(doc['accessors']) - 1

    times = doc['animations'][0]['samplers'][0]['input']
    for name, rows, kind, path, interpolation in [
        ('Return higher', [(2, 3, 0), (0, 3, 0)], 'VEC3', 'translation', 'LINEAR'),
        ('Invalid interior scale', [(0, 0, 0), (1, 1, 1), (-4, 0, 0),
                                    (4, 0, 0), (1, 1, 1), (0, 0, 0)], 'VEC3', 'scale', 'CUBICSPLINE'),
        ('Quarter turn', [(0, 0, 0, 1), (0, 0, 1, 0)], 'VEC4', 'rotation', 'LINEAR'),
    ]:
        output = accessor(rows, kind)
        doc['animations'].append({'name': name, 'samplers': [{'input': times, 'output': output,
                                  'interpolation': interpolation}],
                                  'channels': [{'sampler': 0, 'target': {'node': 1, 'path': path}}]})
    doc['buffers'][0]['byteLength'] = len(blob)
    return doc, bytes(blob)


class BlendContract(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build/runtime-animation-blend-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.root = Path(self.temp.name)
        self.world = self.root / 'world.json'
        self.rig, self.tip, self.session = uid(100), node(uid(100), 1), uid(900)
        self.tick = 0
        self.rows = []
        self.stderr = (self.root / 'stderr.log').open('w+', encoding='utf-8')
        self.process = subprocess.Popen([str(ARGS.binary.resolve()), 'world', self.native(self.world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding='utf-8')
        self.responses = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.responses.put(line)
            self.responses.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def tearDown(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=1)
        self.process.stdout.close()
        self.stderr.seek(0)
        REPORTS.append({'test': self.id(), 'calls': self.rows, 'exit_code': self.process.returncode,
                        'stderr': self.stderr.read()})
        self.stderr.close()
        self.temp.cleanup()

    def native(self, path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if ARGS.windows_interop else str(path.resolve())

    def call(self, method, **params):
        request = {'jsonrpc': '2.0', 'id': len(self.rows), 'method': method, 'params': params}
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        try:
            line = self.responses.get(timeout=45)
        except queue.Empty:
            self.fail(f'No reply to {method} within 45 seconds.')
        self.assertIsNotNone(line, f'World process exited during {method}.')
        response = json.loads(line)
        self.rows.append({'request': request, 'response': response})
        return response

    def good(self, method, **params):
        result = self.call(method, **params)
        self.assertIn('result', result, result)
        return result['result']

    def error(self, response, code):
        self.assertEqual(response.get('error', {}).get('code'), code, response)

    def prepare(self, baseline=None, two=False):
        doc, blob = blend_fixture()
        source = self.root / 'original.glb'
        source.write_bytes(glb(doc, blob))
        asset = self.good('asset.import', source=self.native(source))['asset']
        source.unlink()
        ops = [{'op': 'asset.instantiate', 'id': self.rig, 'name': 'Analytic rig', 'asset': asset}]
        if baseline:
            ops.append(component(self.tip, 'Transform', baseline))
        if two:
            ops.append({'op': 'asset.instantiate', 'id': uid(200), 'name': 'Independent rig', 'asset': asset})
        self.good('world.transact', request_id=uuid.uuid4().hex, base_revision=0, ops=ops)
        self.before = self.world.read_bytes()
        return asset

    def start(self, session=None):
        if session:
            self.session = session
        self.tick = 0
        return self.good('runtime.start', session_id=self.session, revision=1)

    def params(self, ticks, animations=(), **extra):
        return dict(session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick,
                    ticks=ticks, animations=list(animations), **extra)

    def step(self, ticks, animations=()):
        result = self.good('runtime.step', **self.params(ticks, animations))
        self.tick += ticks
        self.assertEqual(result['tick'], self.tick)
        return result

    def entity(self, entity=None):
        return self.good('runtime.entity', session_id=self.session, id=entity or self.tip)

    def animation(self, entity=None):
        return self.entity(entity or self.rig)['animation']

    def pose(self, position, scale=None):
        local = self.entity()['local_transform']
        for actual, expected in zip(local['position'], position):
            self.assertAlmostEqual(actual, expected, places=6)
        if scale is not None:
            for actual, expected in zip(local['scale'], scale):
                self.assertAlmostEqual(actual, expected, places=6)
        return local

    def test_discovery_optional_field_preserves_existing_required_fields(self):
        schema = self.good('world.describe')
        self.assertGreaterEqual(schema['schema_revision'], 25)
        item = schema['methods']['runtime.step']['properties']['animations']['items']
        self.assertEqual(set(item['required']), {'entity', 'clip', 'time', 'speed', 'loop', 'playing'})
        blend = item['properties']['blend_ticks']
        self.assertEqual(blend['type'], 'integer')
        self.assertEqual((blend['minimum'], blend['maximum']), (0, 3600))

    def test_rest_fade_missing_channels_rotation_and_exact_completion(self):
        self.prepare(baseline=transform((.25, 4, 0), (1.5, 1, 1)))
        self.start()
        self.step(15, [command(self.rig, time=1, playing=False, blend_ticks=30)])
        self.pose((.625, 2.5, 0), (1.5, 1, 1))
        transition = self.animation()['transition']
        self.assertEqual(transition, dict(start_tick=0, duration_ticks=30, elapsed_ticks=15, weight=.5,
            source_frozen=False, source_clip=None, source_time=0, source_speed=1, source_loop=True, source_playing=False))
        self.step(15)
        self.pose((1, 1, 0), (1.5, 1, 1))
        self.assertIsNone(self.animation()['transition'])
        # A rotation-only destination must restore authored translation, not inherit A.
        self.step(15, [command(self.rig, clip=3, time=1, playing=False, blend_ticks=30)])
        local = self.pose((.625, 2.5, 0), (1.5, 1, 1))
        import math
        self.assertAlmostEqual(abs(local['rotation'][2]), math.sin(math.pi / 8), places=6)
        self.assertAlmostEqual(abs(local['rotation'][3]), math.cos(math.pi / 8), places=6)
        self.step(15)
        self.pose((.25, 4, 0), (1.5, 1, 1))
        self.step(15, [command(self.rig, clip=None, blend_ticks=30)])
        self.pose((.25, 4, 0), (1.5, 1, 1))
        self.assertIsNone(self.animation()['clip'])
        self.assertFalse(self.animation()['playing'])
        self.assertIsNotNone(self.animation()['transition'])
        self.step(15)
        self.assertIsNone(self.animation()['transition'])
        self.assertEqual(self.entity()['local_transform'], transform((.25, 4, 0), (1.5, 1, 1)))
        self.assertEqual(self.world.read_bytes(), self.before)

    def test_two_advancing_clocks_speed_independent_weight_and_end_clamp(self):
        self.prepare()
        self.start()
        self.step(30, [command(self.rig)])  # A=.5
        self.step(15, [command(self.rig, clip=1, time=.25, speed=2, blend_ticks=60)])
        self.pose((.875, 1.5, 0))  # .75*A(.75) + .25*B(.75)
        state = self.animation()
        self.assertAlmostEqual(state['time'], .75)
        self.assertEqual(state['transition']['weight'], .25)
        self.assertAlmostEqual(state['transition']['source_time'], .75)
        self.assertEqual(state['transition']['source_speed'], 1)
        self.step(45)
        self.pose((1.75, 3, 0))  # B wraps from 2.25 to .25.
        self.assertIsNone(self.animation()['transition'])
        self.step(1, [command(self.rig, time=1.9, loop=False, speed=8)])
        self.assertFalse(self.animation()['playing'])
        self.step(15, [command(self.rig, clip=1, time=0, speed=0, blend_ticks=30)])
        self.pose((2, 2, 0))
        transition = self.animation()['transition']
        self.assertEqual(transition['source_time'], 2)
        self.assertFalse(transition['source_playing'])
        self.assertEqual(transition['weight'], .5)

    def test_interruption_freezes_local_pose_and_immediate_cancels(self):
        self.prepare()
        self.start()
        self.step(30, [command(self.rig, clip=1, time=.5, playing=False, blend_ticks=60)])
        self.pose((.75, 2, 0))
        self.step(15, [command(self.rig, time=2, playing=False, loop=False, blend_ticks=60)])
        self.pose((1.0625, 1.75, 0))
        transition = self.animation()['transition']
        self.assertTrue(transition['source_frozen'])
        for field in ['source_clip', 'source_time', 'source_speed', 'source_loop', 'source_playing']:
            self.assertIsNone(transition[field])
        self.assertEqual(transition['start_tick'], 30)
        self.step(15)
        self.pose((1.375, 1.5, 0))
        self.step(1, [command(self.rig, clip=1, time=1, playing=False)])
        self.pose((1, 3, 0))
        self.assertIsNone(self.animation()['transition'])
        self.step(1, [command(self.rig, clip=None, blend_ticks=0)])
        self.pose((0, 1, 0))
        self.assertIsNone(self.animation()['transition'])

    def test_partition_equivalence_and_independent_rigs(self):
        self.prepare(two=True)
        self.start()
        commands = [command(self.rig, clip=1, speed=2, blend_ticks=120),
                    command(uid(200), time=1, playing=False, blend_ticks=30)]
        self.step(45, commands)
        first = [self.entity(), self.entity(node(uid(200), 1)), self.animation()]
        self.good('runtime.stop', session_id=self.session)
        self.start(uid(901))
        self.step(7, commands)
        self.step(11)
        self.step(27)
        second = [self.entity(), self.entity(node(uid(200), 1)), self.animation()]
        for left, right in zip(first, second):
            left.pop('session_id', None)
            right.pop('session_id', None)
            self.assertEqual(left, right)
        self.assertEqual(second[1]['local_transform']['position'], [1, 1, 0])
        self.assertEqual(self.world.read_bytes(), self.before)

    def test_receipts_normalization_invalid_values_duplicate_atomicity(self):
        self.prepare()
        self.start()
        params = self.params(10, [command(self.rig)])
        original = self.good('runtime.step', **params)
        self.tick = 10
        pose = self.entity()
        normalized = copy.deepcopy(params)
        normalized['animations'][0]['blend_ticks'] = 0
        self.assertEqual(self.good('runtime.step', **normalized), dict(original, replayed=True))
        normalized['animations'][0]['blend_ticks'] = 0.0
        self.error(self.call('runtime.step', **normalized), -32602)
        self.assertEqual(self.entity(), pose)
        normalized['animations'][0]['blend_ticks'] = 1
        self.error(self.call('runtime.step', **normalized), -32010)
        self.assertEqual(self.entity(), pose)
        for value in [-1, 3601, 0.0, .5, True, '30', None]:
            self.error(self.call('runtime.step', **self.params(1, [command(self.rig, blend_ticks=value)])), -32602)
            self.assertEqual(self.entity(), pose)
        duplicate = command(self.rig, blend_ticks=30)
        self.error(self.call('runtime.step', **self.params(1, [duplicate, duplicate])), -32602)
        self.assertEqual(self.entity(), pose)
        self.error(self.call('runtime.step', **dict(self.params(1, [duplicate]), expected_tick=0)), -32009)
        fade = self.params(10, [command(self.rig, clip=1, blend_ticks=3600)])
        first = self.good('runtime.step', **fade)
        during = self.entity()
        self.assertEqual(self.good('runtime.step', **fade), dict(first, replayed=True))
        self.assertEqual(self.entity(), during)
        self.assertEqual(self.animation()['transition']['duration_ticks'], 3600)
        self.assertEqual(self.world.read_bytes(), self.before)

    def test_sample_failure_rolls_back_both_clocks_transition_and_retry(self):
        self.prepare(two=True)
        self.start()
        self.step(10, [command(self.rig, clip=2), command(uid(200), clip=1)])
        before = [self.entity(), self.animation(), self.entity(node(uid(200), 1)), self.animation(uid(200))]
        # Invalid outgoing scale occurs inside this fade even though destination is valid.
        params = self.params(50, [command(self.rig, clip=1, blend_ticks=120)])
        self.error(self.call('runtime.step', **params), -32040)
        after = [self.entity(), self.animation(), self.entity(node(uid(200), 1)), self.animation(uid(200))]
        self.assertEqual(before, after)
        self.assertEqual(self.good('runtime.inspect', session_id=self.session)['tick'], 10)
        params['ticks'] = 1
        params['animations'] = [command(self.rig, clip=None)]
        self.assertEqual(self.good('runtime.step', **params)['tick'], 11)
        self.tick = 11
        # A failed destination sample must also restore an already-running transition.
        self.step(10, [command(self.rig, clip=2, blend_ticks=120)])
        before = [self.entity(), self.animation(), self.entity(node(uid(200), 1))]
        self.error(self.call('runtime.step', **self.params(50)), -32040)
        self.assertEqual(before, [self.entity(), self.animation(), self.entity(node(uid(200), 1))])
        self.assertEqual(self.good('runtime.inspect', session_id=self.session)['tick'], 21)
        self.assertEqual(self.world.read_bytes(), self.before)


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path)
    ARGS = parser.parse_args()
    if not ARGS.runtime:
        for name in unittest.defaultTestLoader.getTestCaseNames(BlendContract):
            if name != 'test_discovery_optional_field_preserves_existing_required_fields':
                setattr(BlendContract, name, unittest.skip('Native runtime not built.')(getattr(BlendContract, name)))
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BlendContract))
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True, exist_ok=True)
        ARGS.evidence.write_text(json.dumps({'passed': result.wasSuccessful(), 'tests': result.testsRun,
            'skipped': len(result.skipped), 'runtime': bool(ARGS.runtime),
            'binary_sha256': hashlib.sha256(ARGS.binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'reports': REPORTS}, indent=2) + '\n')
    return not result.wasSuccessful()


if __name__ == '__main__':
    raise SystemExit(main())
