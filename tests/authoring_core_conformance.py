#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Focused behavioral conformance for the authored-world core, without graphics."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest
import uuid


def uid(value):
    return f'{value:032x}'


def create(value, parent=None):
    return {'op': 'entity.create', 'id': uid(value), 'name': f'Entity {value}', 'parent': parent}


def transform(position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return {'position': list(position), 'rotation': list(rotation), 'scale': list(scale)}


class Client:
    def __init__(self, binary, world, windows_interop):
        path = str(world.resolve())
        if windows_interop:
            path = subprocess.check_output(['wslpath', '-w', path], text=True, timeout=10).strip()
        self.process = subprocess.Popen([str(binary), 'world', path], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding='utf-8', bufsize=1)
        self.lines, self.errors = queue.Queue(), []

        def output():
            try:
                for line in self.process.stdout:
                    self.lines.put(line)
            finally:
                self.lines.put(None)

        def errors():
            self.errors.extend(self.process.stderr)

        self.readers = [threading.Thread(target=output, daemon=True),
                        threading.Thread(target=errors, daemon=True)]
        for reader in self.readers:
            reader.start()

    def send(self, request):
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        self.process.stdin.flush()

    def request(self, method, params=None, rpc_id=1):
        self.send({'jsonrpc': '2.0', 'id': rpc_id, 'method': method,
                   'params': {} if params is None else params})
        line = self.lines.get(timeout=20)
        if line is None:
            raise AssertionError(f'Native process exited: {self.process.poll()}, {self.errors}')
        response = json.loads(line)
        if response.get('jsonrpc') != '2.0' or response.get('id') != rpc_id or type(response.get('id')) is not type(rpc_id):
            raise AssertionError(f'Incorrect response envelope: {response}')
        if ('result' in response) == ('error' in response):
            raise AssertionError(f'Response must have exactly one terminal outcome: {response}')
        return response

    def result(self, method, params=None):
        reply = self.request(method, params)
        if 'error' in reply:
            raise AssertionError(reply)
        return reply['result']

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                self.process.wait(timeout=10)
            if self.process.returncode != 0:
                raise AssertionError(f'Native exit {self.process.returncode}: {self.errors}')
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=10)
            for reader in self.readers:
                reader.join(timeout=2)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                if not stream.closed:
                    stream.close()


class CoreConformance(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='core-', dir=self.scratch)
        self.world = Path(self.directory.name) / 'authoring 世界.json'
        self.clients = []
        self.client = self.open()

    def tearDown(self):
        try:
            for client in self.clients:
                client.close()
        finally:
            self.directory.cleanup()

    def open(self):
        client = Client(self.binary, self.world, self.windows_interop)
        self.clients.append(client)
        return client

    def snapshot(self):
        return (self.client.result('world.inspect'), self.client.result('world.history'),
                self.world.read_bytes() if self.world.exists() else None)

    def unchanged_error(self, method, params, code, rpc_id=1):
        before = self.snapshot()
        response = self.client.request(method, params, rpc_id)
        self.assertNotIn('result', response)
        self.assertEqual(response['error']['code'], code, response)
        self.assertIs(type(response['error']['code']), int)
        self.assertIsInstance(response['error']['message'], str)
        self.assertTrue(response['error']['message'])
        self.assertEqual(self.snapshot(), before)
        return response

    def transaction(self, revision, operations, request_id=None, **extra):
        params = {'base_revision': revision, 'request_id': request_id or uuid.uuid4().hex,
                  'ops': operations, **extra}
        return params, self.client.result('world.transact', params)

    def test_valid_rpc_ids_are_preserved_on_success_and_core_errors(self):
        original, _ = self.transaction(0, [create(1)])
        errors = [
            ('world.inspect', {'unknown': 1}, -32602),
            ('entity.get', {'id': uid(999)}, -32004),
            ('entity.get', {'id': uid(1), 'revision': 0}, -32009),
            ('world.transact', {**original, 'ops': [create(2)]}, -32010),
            ('world.transact', {'base_revision': 1, 'request_id': uid(77), 'ops': []}, -32602),
            ('world.redo', {'base_revision': 1, 'request_id': uid(78)}, -32004),
            ('unknown.method', {}, -32601),
        ]
        for rpc_id in (None, 0, -17, 2147483648, 9007199254740991,
                       -9223372036854775808, 18446744073709551615, '', '雪☃'):
            with self.subTest(rpc_id=rpc_id):
                before = self.snapshot()
                self.assertEqual(self.client.request('world.inspect', rpc_id=rpc_id)['result'], before[0])
                self.assertEqual(self.snapshot(), before)
                for method, params, code in errors:
                    self.unchanged_error(method, params, code, rpc_id)
        for invalid_id in (True, 1.5, [], {}, -9223372036854775809, 18446744073709551616):
            with self.subTest(invalid_id=invalid_id):
                before = self.snapshot()
                self.client.send({'jsonrpc': '2.0', 'id': invalid_id, 'method': 'world.inspect'})
                response = json.loads(self.client.lines.get(timeout=20))
                self.assertEqual(response['jsonrpc'], '2.0')
                self.assertIsNone(response['id'])
                self.assertNotIn('result', response)
                self.assertEqual(response['error']['code'], -32600)
                self.assertEqual(self.snapshot(), before)
        # A null ID is a request; absent ID is a notification. Successful and
        # rejected notifications must not add a response to the following read.
        self.client.send({'jsonrpc': '2.0', 'method': 'world.transact', 'params': {
            'base_revision': 1, 'request_id': uid(79),
            'ops': [{'op': 'entity.rename', 'id': uid(1), 'name': 'Notification'}]}})
        self.assertEqual(self.client.result('entity.get', {'id': uid(1)})['value']['name'], 'Notification')
        before = self.snapshot()
        self.client.send({'jsonrpc': '2.0', 'method': 'world.transact', 'params': {
            'base_revision': 2, 'request_id': uid(80),
            'ops': [{'op': 'entity.rename', 'id': uid(1), 'name': ''}]}})
        self.assertEqual(self.snapshot(), before)

    def test_old_transact_undo_redo_receipts_replay_before_stale_guard_and_after_restart(self):
        transact, first = self.transaction(0, [create(1)])
        undo = {'base_revision': 1, 'request_id': uid(81)}
        undo_receipt = self.client.result('world.undo', undo)
        redo = {'base_revision': 2, 'request_id': uid(82)}
        redo_receipt = self.client.result('world.redo', redo)
        self.transaction(3, [{'op': 'entity.rename', 'id': uid(1), 'name': 'Latest'}])
        before = self.snapshot()
        receipts = [('world.transact', transact, first), ('world.undo', undo, undo_receipt),
                    ('world.redo', redo, redo_receipt)]
        for method, params, receipt in receipts:
            self.assertEqual(self.client.result(method, params), {**receipt, 'replayed': True})
            self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.client.result('world.transact', {**transact, 'preview': False}),
                         {**first, 'replayed': True})
        for method, params in [
            ('world.undo', {'base_revision': 0, 'request_id': transact['request_id']}),
            ('world.redo', undo),
            ('world.transact', {'base_revision': 4, 'request_id': redo['request_id'], 'ops': [create(2)]}),
            ('world.redo', {**redo, 'base_revision': 3}),
        ]:
            self.unchanged_error(method, params, -32010)
        self.client.close()
        self.client = self.open()
        reopened = self.snapshot()
        self.assertEqual(reopened[0], before[0])
        self.assertEqual(reopened[2], before[2])
        self.assertEqual((reopened[1]['undo_count'], reopened[1]['redo_count']), (0, 0))
        for method, params, receipt in receipts:
            self.assertEqual(self.client.result(method, params), {**receipt, 'replayed': True})
            self.assertEqual(self.snapshot(), reopened)
        self.assertEqual(self.client.result('entity.get', {'id': uid(1)})['value']['name'], 'Latest')

    def assert_matrix(self, entity, revision, expected):
        result = self.client.result('entity.world_transform', {'id': uid(entity), 'revision': revision})
        self.assertEqual((result['id'], result['revision'], result['layout']), (uid(entity), revision, 'column_major'))
        self.assertEqual(len(result['matrix']), 16)
        for actual, wanted in zip(result['matrix'], expected):
            self.assertTrue(math.isfinite(actual))
            self.assertAlmostEqual(actual, wanted, places=10)

    def test_authored_true_shear_and_keep_local_reparent_survive_history(self):
        half, quarter = math.sqrt(.5), math.sin(math.pi / 8)
        parent = transform((10, -2, 3), (0, 0, half, half), (2, 3, 4))
        child = transform((1, 2, 3), (0, 0, quarter, math.cos(math.pi / 8)), (5, 7, 11))
        other = transform((-1, 4, -7))
        operations = [create(1), create(2, uid(1)), create(3)]
        operations += [{'op': 'component.set', 'id': uid(value), 'type': 'Transform', 'value': local}
                       for value, local in ((1, parent), (2, child), (3, other))]
        self.transaction(0, operations)
        # Closed-form matrices: parent rotates +90deg around Z; child +45deg.
        # Nonuniform parent scale makes the child basis nonorthogonal.
        sheared = [-15 * half, 10 * half, 0, 0, -21 * half, -14 * half, 0, 0,
                   0, 0, 44, 0, 4, 0, 15, 1]
        self.assertAlmostEqual(sum(sheared[i] * sheared[i + 4] for i in range(3)), 87.5)
        self.assert_matrix(2, 1, sheared)
        self.transaction(1, [{'op': 'entity.reparent', 'id': uid(2), 'parent': uid(3), 'mode': 'keep_local'}])
        self.assertEqual(self.client.result('entity.get', {'id': uid(2), 'component': 'Transform'})['value'], child)
        self.assertEqual(self.client.result('entity.get', {'id': uid(2)})['value']['parent'], uid(3))
        reparented = [5 * half, 5 * half, 0, 0, -7 * half, 7 * half, 0, 0,
                      0, 0, 11, 0, 0, 6, -4, 1]
        self.assert_matrix(2, 2, reparented)
        self.client.result('world.undo', {'base_revision': 2, 'request_id': uid(83)})
        self.assertEqual(self.client.result('entity.get', {'id': uid(2)})['value']['parent'], uid(1))
        self.assert_matrix(2, 3, sheared)
        self.client.result('world.redo', {'base_revision': 3, 'request_id': uid(84)})
        self.assert_matrix(2, 4, reparented)
        self.unchanged_error('world.transact', {'base_revision': 4, 'request_id': uid(85), 'ops': [
            {'op': 'component.set', 'id': uid(1), 'type': 'Transform', 'value': transform((99, 0, 0))},
            {'op': 'entity.reparent', 'id': uid(3), 'parent': uid(2), 'mode': 'keep_local'}]}, -32602)

    def test_query_filters_exclusive_nonmember_cursor_and_exact_terminal_page(self):
        self.transaction(0, [create(10), create(20, uid(10)), create(30, uid(10)),
                             create(40), create(50, uid(40)), create(60, uid(20))])

        def page(params, expected, cursor=None):
            result = self.client.result('entity.query', params)
            self.assertEqual(result['revision'], params.get('revision', 1))
            self.assertEqual([row['id'] for row in result['entities']], [uid(value) for value in expected])
            self.assertEqual(result['next_after'], None if cursor is None else uid(cursor))
            for row in result['entities']:
                self.assertEqual(row['components'], ['Transform'])
            return result

        page({'revision': 1, 'limit': 2}, [10, 20], 20)
        page({'revision': 1, 'after': uid(20), 'limit': 2}, [30, 40], 40)
        page({'revision': 1, 'after': uid(40), 'limit': 2}, [50, 60])
        page({'revision': 1, 'after': uid(25), 'limit': 3}, [30, 40, 50], 50)
        page({'revision': 1, 'after': uid(60), 'limit': 1}, [])
        page({'revision': 1, 'parent': None, 'limit': 2}, [10, 40])
        page({'revision': 1, 'parent': uid(10), 'limit': 2}, [20, 30])
        page({'revision': 1, 'parent': uid(999)}, [])
        page({'revision': 1, 'component': 'Transform', 'limit': 256}, [10, 20, 30, 40, 50, 60])
        page({'revision': 1, 'parent': uid(10), 'after': uid(25), 'component': 'Transform', 'limit': 1}, [30])
        self.transaction(1, [create(value) for value in range(100, 165)])
        all_ids = [10, 20, 30, 40, 50, 60] + list(range(100, 165))
        page({'revision': 2}, all_ids[:64], all_ids[63])
        page({'revision': 2, 'limit': 256}, all_ids)
        self.unchanged_error('entity.query', {'revision': 1, 'after': uid(40), 'limit': 2}, -32009)
        self.unchanged_error('entity.query', {'after': uid(25), 'limit': 1}, -32602)

    def test_failed_undo_redo_storage_does_not_consume_history_or_request_id(self):
        self.transaction(0, [create(1)])
        pending = Path(str(self.world) + '.pending')
        for method, revision in (('world.undo', 1), ('world.redo', 2)):
            params = {'base_revision': revision, 'request_id': uuid.uuid4().hex}
            pending.mkdir()
            try:
                self.unchanged_error(method, params, -32000)
            finally:
                pending.rmdir()
            result = self.client.result(method, params)
            self.assertEqual(result['revision'], revision + 1)
            self.assertFalse(result['replayed'])
            self.assertEqual(self.client.result('world.inspect')['entity_count'], int(method == 'world.redo'))
            before = self.snapshot()
            self.assertEqual(self.client.result(method, params), {**result, 'replayed': True})
            self.assertEqual(self.snapshot(), before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if not binary.is_file():
        parser.error('binary must be an existing native executable file')
    scratch = Path(__file__).resolve().parents[1] / 'build/authoring-core-conformance'
    scratch.mkdir(parents=True, exist_ok=True)
    CoreConformance.binary, CoreConformance.windows_interop, CoreConformance.scratch = binary, args.windows_interop, scratch
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(CoreConformance))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({
            'passed': result.wasSuccessful(), 'tests': result.testsRun, 'failures': len(result.failures),
            'errors': len(result.errors), 'skipped': len(result.skipped), 'windows_interop': args.windows_interop,
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'scope': 'Native authored-world wire IDs/errors, durable core receipts, affine hierarchy, pinned query pages and failed history publication; no graphics.'
        }, indent=2) + '\n', encoding='utf-8')
    return not result.wasSuccessful()


if __name__ == '__main__':
    raise SystemExit(main())
