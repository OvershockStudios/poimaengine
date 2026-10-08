#!/usr/bin/env python3
"""Real authored/runtime component protocol, history, and portable-save guards."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--runtime', type=int, choices=(0, 1), default=1)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--evidence', type=Path)
args = parser.parse_args()
BINARY = str(args.binary.resolve())
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT/'build/world-components-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []


def uid(n): return f'{n:032x}'
A, B, C, TYPE = uid(1), uid(2), uid(3), uid(100)
F = [uid(1000+i) for i in range(5)]
SCHEMA = {'id': TYPE, 'name': 'Stats', 'version': 1, 'fields': [
    {'id': F[0], 'name': 'Health', 'kind': 'int32', 'default': 100},
    {'id': F[1], 'name': 'Score', 'kind': 'int64', 'default': '0'},
    {'id': F[2], 'name': 'Speed', 'kind': 'float32', 'default': 1.25},
    {'id': F[3], 'name': 'Distance', 'kind': 'float64', 'default': 2.5},
    {'id': F[4], 'name': 'Target', 'kind': 'entity', 'default': uid(0)}]}


def values(target=B, health=100):
    return dict(zip(F, [health, '9223372036854775807', 1.25, 2.5, target]))


def create(entity): return {'op': 'entity.create', 'id': entity, 'name': 'Fixture '+entity[-2:]}
def set_value(entity=A, value=None): return {'op': 'component.set', 'id': entity, 'type': 'game:'+TYPE, 'value': values() if value is None else value}
def manifest(schema=SCHEMA): return {'format': 'poima.components', 'version': 1, 'schemas': [schema]}


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

    def seed(self):
        self.tx(0, [{'op': 'component.schema.set', 'schema': SCHEMA}, create(A), create(B), create(C), set_value(), set_value(B, values(A, 70))])

    def start(self):
        sid = uuid.uuid4().hex
        self.ok('runtime.start', session_id=sid, revision=1)
        return sid

    def test_discovery_and_explicit_upgrade_only(self):
        d = self.ok('world.describe')
        self.assertEqual(d['schema_revision'], 49)
        for name in ('component.schemas', 'component.schema.import', 'runtime.components', 'runtime.component.get', 'runtime.component.query', 'runtime.component.edit'):
            self.assertIn(name, d['methods'])
        declared = d['methods']['component.schema.import']['properties']['manifest']['properties']['schemas']['items']['oneOf']
        self.assertEqual([entry['properties']['version']['const'] for entry in declared], [1, 2])
        array = declared[1]['properties']['fields']['contains']
        self.assertEqual(array['properties']['kind']['const'], 'array')
        self.assertEqual(array['properties']['capacity']['maximum'], 31)
        self.assertEqual(self.ok('component.schemas')['schemas'], [])
        self.tx(0, [create(A)])
        self.assertEqual(json.loads(self.world.read_text())['version'], 1)
        before = self.world.read_bytes()
        self.tx(1, [{'op': 'component.schema.set', 'schema': SCHEMA}], preview=True)
        self.assertEqual(self.world.read_bytes(), before)
        request = {'request_id': uuid.uuid4().hex, 'base_revision': 1, 'manifest': manifest()}
        first = self.ok('component.schema.import', **request)
        self.assertEqual(first['revision'], 2)
        doc = json.loads(self.world.read_text())
        self.assertEqual(doc['version'], 2)
        self.assertEqual(len(doc['component_schemas']), 1)
        schema = self.ok('component.schemas', id=TYPE)['schemas'][0]
        self.assertEqual(len(schema['fingerprint']), 64)
        self.assertTrue(all('unit' in f for f in schema['fields']))
        self.close(); self.open()
        self.assertTrue(self.ok('component.schema.import', **request)['replayed'])
        changed = copy.deepcopy(request); changed['manifest']['schemas'][0]['name'] = 'Changed retry'
        self.error('component.schema.import', -32010, **changed)
        self.error('world.undo', -32010, request_id=request['request_id'], base_revision=2)

    def test_reference_validation_is_final_candidate_and_atomic(self):
        self.seed(); before = self.world.read_bytes()
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=1,
                   ops=[{'op': 'entity.delete', 'id': B, 'recursive': False}])
        self.assertEqual(self.world.read_bytes(), before)
        self.tx(1, [{'op': 'entity.delete', 'id': B, 'recursive': False}, set_value(A, values(uid(0)))])
        self.assertEqual(self.ok('entity.get', id=A, component='game:'+TYPE)['value'], values(uid(0)))
        self.assertEqual([e['id'] for e in self.ok('entity.query', component='game:'+TYPE)['entities']], [A])
        before = self.world.read_bytes()
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=2, ops=[set_value(A, values(uid(900)))])
        self.assertEqual(self.world.read_bytes(), before)
        self.tx(2, [create(uid(900)), set_value(A, values(uid(900)))])

    def test_zero_entity_upgrade_rejects_without_mutating_legacy(self):
        self.tx(0, [create(uid(0))])
        original = self.world.read_bytes()
        self.error('component.schema.import', -32602, request_id=uuid.uuid4().hex, base_revision=1, manifest=manifest())
        self.assertEqual(self.world.read_bytes(), original)
        self.assertEqual(json.loads(original)['version'], 1)
        self.tx(1, [{'op': 'component.schema.set', 'schema': SCHEMA}, {'op': 'entity.delete', 'id': uid(0), 'recursive': False}])
        self.assertEqual(json.loads(self.world.read_text())['version'], 2)
        self.assertEqual(self.ok('world.inspect')['entity_count'], 0)

    def test_typed_value_failures_preserve_world_and_history(self):
        self.seed(); before = self.world.read_bytes(); history = self.ok('world.history')
        invalid = []
        for key, value in [(F[0], 1.0), (F[0], 2147483648), (F[1], 123), (F[1], '+1'), (F[1], '01'), (F[1], '-0'), (F[2], 1e100), (F[4], 'x'*32)]:
            v = values(); v[key] = value; invalid.append(v)
        invalid.append({F[0]: 1})
        for v in invalid:
            self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=1, ops=[set_value(A, v)])
        self.assertEqual(self.world.read_bytes(), before)
        self.assertEqual(self.ok('world.history'), history)

    def test_metadata_compatible_shape_changes_and_retirement(self):
        self.seed(); old = self.ok('component.schemas')['schemas'][0]
        rename = copy.deepcopy(SCHEMA); rename['name'] = 'Vitals'; rename['fields'][0]['name'] = 'Current'; rename['fields'][0]['unit'] = 'points'
        self.tx(1, [{'op': 'component.schema.set', 'schema': rename}])
        self.assertEqual(self.ok('component.schemas')['schemas'][0]['fingerprint'], old['fingerprint'])
        incompatible = copy.deepcopy(rename); incompatible['fields'][0]['default'] = 101
        before = self.world.read_bytes()
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=2, ops=[{'op': 'component.schema.set', 'schema': incompatible}])
        self.error('world.transact', -32602, request_id=uuid.uuid4().hex, base_revision=2, ops=[{'op': 'component.schema.remove', 'id': TYPE}])
        self.assertEqual(self.world.read_bytes(), before)
        self.tx(2, [{'op': 'component.remove', 'id': i, 'type': 'game:'+TYPE} for i in (A, B)]+[{'op': 'component.schema.remove', 'id': TYPE}])
        self.error('component.schema.import', -32602, request_id=uuid.uuid4().hex, base_revision=3, manifest=manifest())
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=3)
        self.assertEqual(self.ok('entity.get', id=A, component='game:'+TYPE)['value'], values())
        self.assertEqual(self.ok('component.schemas')['schemas'][0]['name'], 'Vitals')
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=4)
        self.assertEqual(self.ok('component.schemas')['schemas'], [])
        self.assertIn(TYPE, json.loads(self.world.read_text())['retired_component_schemas'])

    def test_schema_only_history_stays_v2_and_failed_persist_keeps_history(self):
        self.tx(0, [create(A)])
        self.ok('component.schema.import', request_id=uuid.uuid4().hex, base_revision=1, manifest=manifest())
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=2)
        self.assertEqual(json.loads(self.world.read_text())['version'], 2)
        self.assertEqual(self.ok('component.schemas')['schemas'], [])
        self.ok('world.undo', request_id=uuid.uuid4().hex, base_revision=3)
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=4)
        self.ok('world.redo', request_id=uuid.uuid4().hex, base_revision=5)
        history = self.ok('world.history'); self.world.write_bytes(self.world.read_bytes()+b' ')
        changed = self.world.read_bytes()
        self.error('world.undo', -32009, request_id=uuid.uuid4().hex, base_revision=6)
        self.assertEqual(self.ok('world.history'), history)
        self.assertEqual(self.world.read_bytes(), changed)

    @unittest.skipUnless(args.runtime, 'Authoring-only build')
    def test_runtime_frozen_query_edit_receipts_and_save_guards(self):
        self.seed(); sid = self.start(); original = self.world.read_bytes()
        schema = self.ok('runtime.components', session_id=sid)
        self.assertEqual(schema['component_revision'], 0)
        page = self.ok('runtime.component.query', session_id=sid, tick=0, type=TYPE, limit=1)
        self.assertEqual(page['entities'], [A]); self.assertEqual(page['next_after'], A)
        self.assertEqual(self.ok('runtime.component.query', session_id=sid, tick=0, type=TYPE, after=A)['entities'], [B])
        self.assertIsNone(self.ok('runtime.component.get', session_id=sid, tick=0, id=C, type=TYPE)['values'])
        request = {'session_id': sid, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'expected_revision': 0, 'id': A, 'type': TYPE, 'values': values(B, 65)}
        self.assertEqual(self.ok('runtime.component.edit', **request)['component_revision'], 1)
        self.assertTrue(self.ok('runtime.component.edit', **request)['replayed'])
        bad = copy.deepcopy(request); bad['values'][F[0]] = 65.0
        self.error('runtime.component.edit', -32602, **bad)
        stale = dict(request, request_id=uuid.uuid4().hex)
        self.error('runtime.component.edit', -32009, **stale)
        self.error('runtime.step', -32010, session_id=sid, request_id=request['request_id'], expected_tick=0, ticks=1)
        self.assertEqual(self.world.read_bytes(), original)
        self.tx(1, [set_value(A, values(B, 10))])
        self.assertEqual(self.ok('runtime.component.get', session_id=sid, tick=0, id=A, type=TYPE)['values'], values(B, 65))
        savedir = self.root/'saves'; savedir.mkdir()
        self.ok('save.configure', request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(savedir))
        write = {'request_id': uuid.uuid4().hex, 'configuration_generation': 1, 'slot': 'quick', 'expected_generation': 0,
                 'session_id': sid, 'expected_tick': 0, 'expected_gameplay_revision': 0}
        self.error('save.write', -32602, **write)
        self.error('save.write', -32009, **dict(write, expected_component_revision=0))
        write['expected_component_revision'] = 1
        self.assertEqual(self.ok('save.write', **write)['generation'], 1)
        self.assertTrue(self.ok('save.write', **write)['replayed'])
        self.ok('runtime.component.edit', **dict(request, request_id=uuid.uuid4().hex, expected_revision=1, values=values(B, 55)))
        load = {'request_id': uuid.uuid4().hex, 'configuration_generation': 1, 'slot': 'quick', 'expected_generation': 1, 'revision': 2,
                'expected_session_id': sid, 'expected_tick': 0, 'expected_gameplay_revision': 0, 'new_session_id': uuid.uuid4().hex}
        self.error('save.load', -32602, **load)
        self.error('save.load', -32009, **dict(load, expected_component_revision=1))
        result = self.ok('save.load', **dict(load, expected_component_revision=2))
        self.assertTrue(result['source_stale'])
        restored = self.ok('runtime.component.get', session_id=result['session_id'], tick=0, id=A, type=TYPE)
        self.assertEqual(restored['values'], values(B, 65)); self.assertEqual(restored['component_revision'], 1)
        self.assertEqual(self.ok('entity.get', id=A, component='game:'+TYPE)['value'], values(B, 10))
        self.assertTrue(self.ok('save.load', **dict(load, expected_component_revision=2))['replayed'])

    @unittest.skipUnless(args.runtime, 'Authoring-only build')
    def test_step_receipt_conflict_and_query_guards(self):
        self.seed(); sid = self.start(); req = uuid.uuid4().hex
        self.ok('runtime.step', session_id=sid, request_id=req, expected_tick=0, ticks=1)
        self.error('runtime.component.edit', -32010, session_id=sid, request_id=req, expected_tick=1, expected_revision=0, id=A, type=TYPE, values=values())
        self.error('runtime.component.get', -32009, session_id=sid, tick=0, id=A, type=TYPE)
        self.error('runtime.component.query', -32602, session_id=sid, tick=1, type=TYPE, limit=257)
        self.error('runtime.component.get', -32602, session_id=sid, tick=1, id=uid(900), type=TYPE)
        self.assertEqual(self.ok('runtime.components', session_id=sid)['component_revision'], 0)


suite = unittest.defaultTestLoader.loadTestsFromTestCase(Contract)
result = unittest.TextTestRunner(verbosity=2).run(suite)
if args.evidence:
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps({'tests': result.testsRun, 'skipped': len(result.skipped), 'passed': result.wasSuccessful(),
        'rpc_count': len(CALLS), 'binary_sha256': hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(), 'calls': CALLS}, indent=2)+'\n')
raise SystemExit(0 if result.wasSuccessful() else 1)
