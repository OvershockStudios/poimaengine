#!/usr/bin/env python3
"""Owned synthetic peers and real native processes qualify the automation SDK."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import concurrent.futures
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def peer(mode):
    if mode == 'blocked':
        time.sleep(30)
        return
    requests = []
    for line in sys.stdin:
        request = json.loads(line)
        if mode == 'reverse':
            requests.append(request)
            if len(requests) != 8:
                continue
            pending = reversed(requests)
        else:
            pending = [request]
        for item in pending:
            response = dict(jsonrpc='2.0', id=item['id'], result=item['params'])
            if mode == 'error' and item['method'] == 'fail':
                response = dict(jsonrpc='2.0', id=item['id'], error=dict(
                    code=-32009, message='Stale probe', data={'revision': 9}))
            if mode == 'delay':
                time.sleep(30)
            if mode == 'stderr':
                sys.stderr.buffer.write(b'x'*100000+b'\xffEND\n'); sys.stderr.flush()
            if mode == 'oversize':
                sys.stdout.buffer.write(b'x'*(16*1024*1024+1)); sys.stdout.flush()
                time.sleep(30)
                return
            malformed = {
                'bool-id': json.dumps(dict(response, id=True)),
                'both': json.dumps(dict(response, error={'code': -1, 'message': 'bad'})),
                'notification': '{"jsonrpc":"2.0","method":"unexpected"}',
                'nan': '{"jsonrpc":"2.0","id":'+str(item['id'])+',"result":NaN}',
                'duplicate-key': '{"jsonrpc":"2.0","id":'+str(item['id'])+',"result":1,"result":2}',
                'bad-error': json.dumps(dict(jsonrpc='2.0', id=item['id'], error=dict(code=True, message='bad'))),
                'unknown-id': json.dumps(dict(response, id=item['id']+100)),
                'truncated': '{"jsonrpc":"2.0","id":',
                'invalid-utf8': None,
            }
            if mode == 'invalid-utf8':
                sys.stdout.buffer.write(b'\xff\n'); sys.stdout.flush()
                time.sleep(30)
                return
            text = malformed.get(mode, json.dumps(response))
            sys.stdout.write(text + ('' if mode == 'truncated' else '\n')); sys.stdout.flush()
            if mode == 'truncated':
                return
        if mode == 'reverse':
            requests = []


if len(sys.argv) == 3 and sys.argv[1] == '--peer':
    peer(sys.argv[2])
    raise SystemExit(0)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine', type=Path)
parser.add_argument('--installed', action='store_true', help='Import installed package, not checkout source.')
args, unittest_args = parser.parse_known_args()
if not args.installed:
    sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import (JsonRpcProcess, WorldClient, RpcError, TransportError,
                         OutcomeUnknown, ResponseContractError, PaginationError, new_id)
from poima_client.responses import validate_core_result
from python_client_cleanup_diagnostics import CleanupDiagnostics

BINARY = None if args.engine is None else str(args.engine.resolve())
SCRATCH = ROOT/'build/python-client-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
A, B, C = '1'*32, '2'*32, '3'*32


class ScriptedTransport:
    def __init__(self, replies=(), error=None):
        self.replies = iter(replies)
        self.error = error
        self.calls = []
        self.closed = False
    def request(self, method, params=None, timeout=30.0):
        self.calls.append((method, copy.deepcopy(params)))
        if self.error:
            raise self.error
        return next(self.replies)
    def close(self):
        self.closed = True


class TransportContract(unittest.TestCase):
    def start(self, mode, **options):
        transport = JsonRpcProcess([sys.executable, str(Path(__file__).resolve()), '--peer', mode],
                                   close_timeout=1.5, **options)
        self.addCleanup(transport.close)
        return transport

    def test_concurrent_reordered_unicode_calls(self):
        transport = self.start('reverse')
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            futures = [executor.submit(transport.request, 'echo', {'text': '世界 '+str(i)}) for i in range(8)]
            self.assertEqual([f.result(timeout=8) for f in futures], [{'text': '世界 '+str(i)} for i in range(8)])
        transport.close()
        self.assertEqual(transport.returncode, 0)
        self.assertTrue(transport.closed)

    def test_structured_errors_keep_connection_usable(self):
        transport = self.start('error')
        params = {'request_id': B, 'nested': ['private-context', '世界']}
        with self.assertRaises(RpcError) as caught:
            transport.request('fail', params)
        error = caught.exception
        self.assertEqual((error.code, error.data), (-32009, {'revision': 9}))
        self.assertEqual((error.method, error.request_id), ('fail', B))
        self.assertIs(type(error.rpc_id), int)
        self.assertEqual(json.loads(error.params_json), params)
        params['nested'][0] = 'later caller edit'
        self.assertEqual(error.params['nested'][0], 'private-context')
        self.assertNotIn('private-context', repr(error))
        self.assertEqual(transport.request('echo', {'ok': True}), {'ok': True})

    def test_protocol_corruption_never_becomes_success(self):
        for mode in ('bool-id', 'both', 'notification', 'nan', 'duplicate-key', 'bad-error',
                     'unknown-id', 'truncated', 'invalid-utf8', 'oversize'):
            with self.subTest(mode=mode):
                transport = self.start(mode)
                with self.assertRaises(OutcomeUnknown):
                    transport.request('echo', timeout=8)
                self.assertTrue(transport.closed)
                transport.close()
                self.assertIsNotNone(transport.returncode)

    def test_blocked_write_deadline_disconnects_and_reaps(self):
        transport = self.start('blocked')
        started = time.monotonic()
        with self.assertRaises(OutcomeUnknown) as caught:
            transport.request('large', {'data': 'x'*900000}, timeout=.3)
        self.assertLess(time.monotonic()-started, 2)
        self.assertEqual(caught.exception.method, 'large')
        self.assertEqual(len(json.loads(caught.exception.params_json)['data']), 900000)
        transport.close()
        self.assertIsNotNone(transport.returncode)
        with self.assertRaises(TransportError):
            transport.request('echo')

    def test_interrupt_race_keeps_exact_recovery_context(self):
        import threading
        from unittest import mock
        import poima_client.transport as transport_module

        original_pending = transport_module._Pending
        for interrupt in (KeyboardInterrupt, SystemExit):
            with self.subTest(interrupt=interrupt.__name__):
                class InterruptedPending(original_pending):
                    def __init__(self, *values):
                        super().__init__(*values)

                        def interrupted_wait(timeout=None):
                            raise interrupt(2 if interrupt is SystemExit else 'Owned race probe')

                        self.event.wait = interrupted_wait

                class GatedProcess(JsonRpcProcess):
                    def __init__(self, *values, **options):
                        self.writer_gate = threading.Event()
                        super().__init__(*values, **options)

                    def _write_requests(self):
                        self.writer_gate.wait(3)
                        super()._write_requests()

                    def _disconnect(self, reason):
                        if reason.startswith('Request wait was interrupted'):
                            # Force the queued -> written transition after wait
                            # interruption but before atomic disconnection. A
                            # pre-disconnect state sample misses this send.
                            self.writer_gate.set()
                            deadline = time.monotonic() + 3
                            while not any(item.state == 'written' for item in self._pending.values()):
                                if time.monotonic() >= deadline:
                                    raise AssertionError('Owned writer did not send the race request')
                                time.sleep(.001)
                        super()._disconnect(reason)

                with mock.patch.object(transport_module, '_Pending', InterruptedPending):
                    transport = GatedProcess(
                        [sys.executable, str(Path(__file__).resolve()), '--peer', 'delay'],
                        close_timeout=1.5)
                    self.addCleanup(transport.close)
                    params = {'request_id': 'owned-race-receipt', 'text': '\u4e16\u754c'}
                    with self.assertRaises(interrupt) as caught:
                        transport.request('mutation.probe', params, timeout=5)
                    self.assertEqual(caught.exception.rpc_id, 1)
                    self.assertEqual(caught.exception.method, 'mutation.probe')
                    self.assertEqual(json.loads(caught.exception.params_json), params)
                    if interrupt is SystemExit:
                        self.assertEqual(caught.exception.code, 2)
                    self.assertTrue(transport.closed)
                    transport.close()
                    self.assertIsNotNone(transport.returncode)

    def test_close_cancels_pending_and_capacity_is_unsent(self):
        transport = self.start('delay', max_pending=1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(transport.request, 'first', {}, 10)
            deadline = time.monotonic()+3
            while not transport._pending:
                if time.monotonic() > deadline:
                    self.fail('Request was not submitted.')
                time.sleep(.005)
            with self.assertRaises(TransportError) as caught:
                transport.request('second')
            self.assertNotIsInstance(caught.exception, OutcomeUnknown)
            transport.close()
            with self.assertRaises(TransportError):
                future.result(timeout=3)
        self.assertIsNotNone(transport.returncode)

    def test_local_size_and_json_errors_do_not_disconnect(self):
        transport = self.start('echo')
        for value in ({'too_big': 'x'*(1024*1024)}, {'number': float('nan')}, {1: 'a', '1': 'b'}):
            with self.subTest(value_kind=list(value)[0]), self.assertRaises(ValueError):
                transport.request('echo', value)
        self.assertFalse(transport.closed)
        self.assertEqual(transport.request('echo', {'ok': 1}), {'ok': 1})

    def test_stderr_is_bounded_and_utf8_safe(self):
        transport = self.start('stderr', stderr_limit=1024)
        transport.request('echo')
        deadline = time.monotonic()+3
        while 'END' not in transport.stderr_tail and time.monotonic()<deadline:
            time.sleep(.005)
        self.assertTrue(transport.stderr_truncated)
        self.assertIn('\ufffdEND', transport.stderr_tail)
        self.assertLessEqual(len(transport.stderr_tail), 1024)


class WrapperContract(unittest.TestCase):
    def test_shutdown_host_sends_empty_params_and_preserves_native_result(self):
        result = {"closed": True, "future_metadata": {"native": "unchanged"}}
        class TimedTransport(ScriptedTransport):
            def request(self, method, params=None, timeout=30.0):
                self.timeout = timeout
                return super().request(method, params, timeout)
        transport = TimedTransport([result])
        client = WorldClient(transport)
        self.assertIs(client.shutdown_host(timeout=2.5), result)
        self.assertEqual(transport.timeout, 2.5)
        self.assertEqual(transport.calls, [("host.shutdown", {})])
        self.assertFalse(client.closed, "A shutdown request must not detach the client.")

    def test_shutdown_host_preserves_errors_without_retry_or_detach(self):
        for error in (RpcError(-32080, "Stop the active player first"),
                      OutcomeUnknown("Reply was not received")):
            with self.subTest(error=type(error).__name__):
                transport = ScriptedTransport(error=error)
                client = WorldClient(transport)
                with self.assertRaises(type(error)) as caught:
                    client.shutdown_host()
                self.assertIs(caught.exception, error)
                self.assertEqual((error.method, error.params_json, error.params),
                                 ("host.shutdown", "{}", {}))
                self.assertIsNone(error.request_id)
                self.assertEqual(transport.calls, [("host.shutdown", {})])
                self.assertFalse(client.closed)

    def test_shutdown_receipt_arguments_reject_locally_and_context_only_detaches(self):
        transport = ScriptedTransport()
        client = WorldClient(transport)
        with self.assertRaises(TypeError):
            client.shutdown_host(request_id=B)
        with self.assertRaises(TypeError):
            client.shutdown_host({"request_id": B})
        self.assertEqual(transport.calls, [])
        with client:
            pass
        self.assertTrue(client.closed)
        self.assertEqual(transport.calls, [], "Attached context exit must not shut down a host.")

    def test_mutation_discovery_helper_sends_exact_selectors(self):
        def result(operation, component=None):
            selection = {'operation': operation}
            properties = {'op': {'const': operation}}
            if component is not None:
                selection['type'] = component
                properties['type'] = {'const': component}
            envelope = {'type': 'object', 'required': ['request_id', 'base_revision', 'ops'],
                        'properties': {'request_id': {'type': 'string'}, 'base_revision': {'type': 'integer'},
                                       'ops': {'type': 'array', 'items': {'oneOf': [
                                           {'properties': properties}]}}}}
            return {'protocol_version': 1, 'schema_revision': 48, 'mode': 'authoring',
                    'read_only': False, 'runtime_available': True, 'partial': True,
                    'view': 'mutation', 'selection': selection, 'methods': {'world.transact': envelope}}
        values = [result('entity.create'), result('component.set', 'Transform')]
        class TimedTransport(ScriptedTransport):
            def request(self, method, params=None, timeout=30.0):
                self.timeout = timeout
                return super().request(method, params, timeout)
        transport = TimedTransport(values)
        client = WorldClient(transport)
        self.assertIs(client.discover_mutation('entity.create', timeout=2.5), values[0])
        self.assertEqual(transport.timeout, 2.5)
        self.assertIs(client.discover_mutation('component.set', 'Transform'), values[1])
        self.assertEqual(transport.calls, [
            ('world.describe', {'view': 'mutation', 'operation': 'entity.create'}),
            ('world.describe', {'view': 'mutation', 'operation': 'component.set', 'type': 'Transform'})])

    def test_discovery_helpers_reject_cross_view_selectors_without_sending(self):
        transport = ScriptedTransport()
        client = WorldClient(transport)
        invalid = [lambda: client.discover('mutation'), lambda: client.discover('future'),
                   lambda: client.discover('full', 'world.transact'),
                   lambda: client.discover('catalog', 'Transform'),
                   lambda: client.discover('component'), lambda: client.discover('method', False),
                   lambda: client.discover_mutation('entity.create', 'Camera')]
        for operation in (None, True, '', 'a'*129, 'é'*65, '\ud800'):
            invalid.append(lambda value=operation: client.discover_mutation(value))
        for component in (True, '', 'a'*129, 'é'*65, '\ud800'):
            invalid.append(lambda value=component: client.discover_mutation('component.set', value))
        for action in invalid:
            with self.assertRaises(ValueError):
                action()
        self.assertEqual(transport.calls, [])

    def test_mutation_discovery_bad_response_keeps_original_context(self):
        class MutatingTransport(ScriptedTransport):
            def request(self, method, params=None, timeout=30.0):
                params['operation'] = 'entity.create'
                return {'protocol_version': 1, 'schema_revision': 48, 'mode': 'authoring',
                        'read_only': False, 'runtime_available': True, 'partial': True,
                        'view': 'mutation', 'selection': {'operation': 'entity.create'},
                        'methods': {'world.transact': {}}}
        client = WorldClient(MutatingTransport())
        with self.assertRaises(ResponseContractError) as caught:
            client.discover_mutation('component.set', 'Transform')
        self.assertEqual(caught.exception.params,
                         {'view': 'mutation', 'operation': 'component.set', 'type': 'Transform'})
        self.assertFalse(client.closed)

    def test_invalid_wrapper_parameters_never_reach_transport(self):
        transport = ScriptedTransport()
        client = WorldClient(transport)
        for params in ({'unsupported': object()}, {'bad': float('nan')},
                       {1: 'first', '1': 'duplicate'}, {'bad': '\ud800'},
                       {'large': 'x'*(1024*1024+1)}):
            with self.subTest(kind=list(params)[0]), self.assertRaises(ValueError):
                client.call('world.inspect', params)
        self.assertEqual(transport.calls, [])

    def test_call_uses_original_snapshot_when_transport_mutates_input(self):
        class MutatingTransport(ScriptedTransport):
            def request(self, method, params=None, timeout=30.0):
                params['base_revision'] = 99
                params['request_id'] = C
                return {'revision': 8, 'committed': True, 'replayed': False,
                        'changed_ids': [], 'history_recorded': True}
        client = WorldClient(MutatingTransport())
        original = {'base_revision': 7, 'request_id': B, 'ops': []}
        self.assertEqual(client.call('world.transact', original)['revision'], 8)
        self.assertEqual(original['base_revision'], 7)
        self.assertEqual(original['request_id'], B)

    def test_malformed_successful_read_guard_keeps_channel_and_original_context(self):
        identity = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
        cases = [
            ('entity.get', dict(id=A, component='Transform'), dict(id=A, value=identity)),
            ('entity.query', dict(limit=1), dict(entities=[
                dict(id=A, name='Observed', parent=None, components=['Transform'])], next_after=None)),
            ('entity.world_transform', dict(id=A), dict(id=A, matrix=[
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], layout='column_major')),
        ]
        for method, selectors, value in cases:
            with self.subTest(method=method):
                reply = dict(value, revision=5)
                transport = ScriptedTransport([reply, reply])
                client = WorldClient(transport)
                request = dict(selectors, revision=5.0)
                with self.assertRaises(ResponseContractError) as caught:
                    client.call(method, request)
                request['revision'] = 999
                error = caught.exception
                self.assertEqual(error.method, method)
                self.assertIs(error.result, reply)
                self.assertEqual(json.loads(error.params_json)['revision'], 5.0)
                self.assertIs(type(error.params['revision']), float)
                self.assertEqual(len(transport.calls), 1)
                self.assertFalse(transport.closed)
                self.assertIs(client.call(method, dict(selectors, revision=5)), reply)
                self.assertEqual(len(transport.calls), 2)
                self.assertFalse(transport.closed)

    def test_generated_mutation_receipt_survives_unknown_outcome(self):
        for exception in (OutcomeUnknown('lost'), KeyboardInterrupt(), SystemExit(2)):
            ops = [{'op': 'entity.create', 'id': A, 'name': 'Before'}]
            transport = ScriptedTransport(error=exception)
            client = WorldClient(transport)
            with self.assertRaises(type(exception)) as caught:
                client.transact(ops, 7)
            error = caught.exception
            ops[0]['name'] = 'After'
            self.assertRegex(error.request_id, '^[0-9a-f]{32}$')
            self.assertEqual(json.loads(error.params_json)['ops'][0]['name'], 'Before')
            self.assertEqual(error.method, 'world.transact')
            self.assertEqual(len(transport.calls), 1, 'Mutation was retried automatically.')

    def test_pagination_pins_revision_and_rejects_truncation(self):
        def page(identifier, cursor, revision=7):
            return {'revision': revision, 'entities': [{'id': identifier}], 'next_after': cursor}
        transport = ScriptedTransport([page(A, A), page(B, None)])
        client = WorldClient(transport, validate_responses=False)
        self.assertEqual([r['id'] for r in client.iter_entities(limit=1)], [A, B])
        self.assertNotIn('revision', transport.calls[0][1])
        self.assertEqual(transport.calls[1][1]['revision'], 7)
        self.assertEqual(transport.calls[1][1]['after'], A)
        for replies in ([page(A, A), page(B, None, revision=8)], [page(A, A), page(A, None)]):
            with self.assertRaises(PaginationError):
                list(WorldClient(ScriptedTransport(replies), validate_responses=False).iter_entities(limit=1))
        with self.assertRaises(PaginationError):
            list(WorldClient(ScriptedTransport([page(A, A)]), validate_responses=False).iter_entities(max_pages=1))

    def test_cleanup_never_masks_original_error(self):
        transport = ScriptedTransport()
        def fail_close():
            raise TransportError('Cleanup probe')
        transport.close = fail_close
        with self.assertRaises(RpcError) as caught:
            with WorldClient(transport):
                raise RpcError(-32009, 'Original')
        self.assertEqual(caught.exception.message, 'Original')

    def test_invalid_core_result_preserves_mutation_context(self):
        transport = ScriptedTransport([{'revision': True}])
        client = WorldClient(transport)
        with self.assertRaises(ResponseContractError) as caught:
            client.transact([], 3, request_id=B)
        self.assertEqual(caught.exception.request_id, B)
        self.assertEqual(json.loads(caught.exception.params_json)['base_revision'], 3)
        self.assertFalse(client.closed)


class RpcDiagnosticsContract(unittest.TestCase):
    def pending_transport(self, requests):
        import threading
        from poima_client.transport import _Pending
        transport = object.__new__(JsonRpcProcess)
        transport._condition = threading.Condition()
        transport._closed = False
        transport._pending = {}
        for rpc_id, method, params in requests:
            serialized = json.dumps(params, ensure_ascii=False, separators=(",", ":"))
            pending = _Pending(rpc_id, b"", time.monotonic()+10, method, serialized)
            pending.state = "written"
            transport._pending[rpc_id] = pending
        return transport, dict(transport._pending)

    @staticmethod
    def failure(rpc_id, message="Invalid selectors", data=None):
        return dict(jsonrpc="2.0", id=rpc_id,
                    error=dict(code=-32602, message=message, data=data))

    def test_constructor_and_formatting_remain_compatible(self):
        error = RpcError(-32009, "Original", {"secret": "native-data"})
        self.assertEqual(error.args, ("Original",))
        self.assertEqual(str(error), "Original")
        self.assertEqual(repr(error), "RpcError('Original')")
        self.assertEqual(error.data, {"secret": "native-data"})
        for name in ("method", "rpc_id", "params_json", "params", "request_id"):
            self.assertIsNone(getattr(error, name))

    def test_terminal_error_carries_correlated_detached_context(self):
        params = {"request_id": B, "nested": ["private-token", "世界"], "revision": 5.0}
        transport, pending = self.pending_transport([(7, "world.transact", params)])
        data = {"required": ["tick"], "host_detail": "unchanged"}
        transport._accept_response(self.failure(7, data=data))
        error = pending[7].error
        self.assertIsInstance(error, RpcError)
        self.assertTrue(pending[7].event.is_set())
        self.assertEqual((error.method, error.rpc_id, error.request_id), ("world.transact", 7, B))
        self.assertIs(error.data, data)
        self.assertEqual((error.code, error.message), (-32602, "Invalid selectors"))
        params["nested"][0] = "later"
        self.assertEqual(error.params["nested"][0], "private-token")
        error.params["nested"][0] = "edited convenience copy"
        self.assertEqual(json.loads(error.params_json)["nested"][0], "private-token")
        self.assertIs(type(json.loads(error.params_json)["revision"]), float)
        self.assertNotIn("private-token", str(error))
        self.assertNotIn("private-token", repr(error))
        self.assertNotIn("host_detail", repr(error))
        self.assertFalse(transport.closed)

    def test_reordered_errors_keep_distinct_ids_and_success_remains_available(self):
        transport, pending = self.pending_transport([
            (11, "first", {"request_id": A}), (12, "second", [2]), (13, "third", {})])
        transport._accept_response(self.failure(12))
        self.assertFalse(pending[11].event.is_set())
        transport._accept_response(self.failure(11))
        transport._accept_response(dict(jsonrpc="2.0", id=13, result={"ok": True}))
        self.assertEqual((pending[11].error.method, pending[11].error.rpc_id), ("first", 11))
        self.assertEqual((pending[12].error.method, pending[12].error.rpc_id), ("second", 12))
        self.assertIsNone(pending[12].error.request_id)
        self.assertEqual(pending[12].error.params, [2])
        self.assertEqual(pending[13].result, {"ok": True})
        self.assertIsNone(pending[13].error)
        self.assertFalse(transport.closed)
        self.assertEqual(transport._pending, {})

    def test_invalid_errors_never_gain_terminal_authority(self):
        cases = [self.failure(True), self.failure(99),
                 dict(jsonrpc="2.0", id=1, error={"code": True, "message": "bad"}),
                 dict(jsonrpc="2.0", id=1, error={"code": -1, "message": "bad", "extra": 1}),
                 dict(jsonrpc="2.0", id=1, result={}, error={"code": -1, "message": "bad"})]
        for response in cases:
            with self.subTest(response=response):
                transport, pending = self.pending_transport([(1, "echo", {"secret": "retained"})])
                with self.assertRaises(ValueError):
                    transport._accept_response(response)
                self.assertIsNone(pending[1].error)
                self.assertFalse(pending[1].event.is_set())
                self.assertIn(1, transport._pending)
        # Reader-driven disconnection/unknown outcomes remain covered separately.

    def test_duplicate_response_cannot_overwrite_context(self):
        transport, pending = self.pending_transport([(1, "first", {"value": 1})])
        transport._accept_response(self.failure(1))
        original = pending[1].error
        with self.assertRaises(ValueError):
            transport._accept_response(self.failure(1, message="replacement"))
        self.assertIs(pending[1].error, original)
        self.assertEqual(original.message, "Invalid selectors")

    def test_embedding_mutation_keeps_original_context_and_native_correlation(self):
        native_error = RpcError(-32602, "Missing tick", {"required": ["tick"]})
        native_error.rpc_id = 42
        class MutatingTransport(ScriptedTransport):
            def request(self, method, params=None, timeout=30.0):
                self.calls.append((method, copy.deepcopy(params)))
                params["nested"][0] = "embedding edit"
                raise native_error
        transport = MutatingTransport()
        original = {"nested": ["private-token"], "request_id": B, "tick": 5.0}
        with self.assertRaises(RpcError) as caught:
            WorldClient(transport).call("runtime.ui.inspect", original)
        error = caught.exception
        self.assertIs(error, native_error)
        self.assertEqual((error.method, error.rpc_id, error.request_id), ("runtime.ui.inspect", 42, B))
        original["nested"][0] = "caller edit"
        self.assertEqual(error.params["nested"], ["private-token"])
        error.params["nested"][0] = "convenience edit"
        self.assertEqual(json.loads(error.params_json)["nested"], ["private-token"])
        self.assertIs(type(json.loads(error.params_json)["tick"]), float)
        self.assertEqual(len(transport.calls), 1)
        self.assertFalse(transport.closed)
        self.assertNotIn("private-token", repr(error))

    def test_generated_receipt_is_retained_without_retry_or_discovery(self):
        transport = ScriptedTransport(error=RpcError(-32009, "Stale revision"))
        ops = [{"op": "entity.create", "id": A, "name": "Original"}]
        with self.assertRaises(RpcError) as caught:
            WorldClient(transport).transact(ops, 7)
        error = caught.exception
        self.assertEqual(error.method, "world.transact")
        self.assertIsNone(error.rpc_id, "Embedding transport did not supply correlation.")
        self.assertRegex(error.request_id, "^[0-9a-f]{32}$")
        self.assertEqual(json.loads(error.params_json)["request_id"], error.request_id)
        ops[0]["name"] = "Later"
        self.assertEqual(error.params["ops"][0]["name"], "Original")
        self.assertEqual([x[0] for x in transport.calls], ["world.transact"])
        self.assertFalse(transport.closed)

    def test_array_and_empty_parameter_context_does_not_invent_receipts(self):
        for params in ([1, {"secret": "private-token"}], None):
            with self.subTest(params=params):
                transport = ScriptedTransport(error=RpcError(-32602, "Rejected"))
                with self.assertRaises(RpcError) as caught:
                    WorldClient(transport).call("custom.operation", params)
                error = caught.exception
                self.assertEqual(error.params, {} if params is None else params)
                self.assertIsNone(error.request_id)
                self.assertIsNone(error.rpc_id)
                self.assertEqual(len(transport.calls), 1)
                self.assertNotIn("private-token", repr(error))

    def test_oversized_parameters_still_fail_locally_without_error_context(self):
        transport = ScriptedTransport(error=RpcError(-32602, "Not reached"))
        with self.assertRaises(ValueError):
            WorldClient(transport).call("custom.operation", {"data": "x"*(1024*1024)})
        self.assertEqual(transport.calls, [])
        self.assertIsNone(transport.error.method)
        self.assertIsNone(transport.error.params_json)


@unittest.skipIf(BINARY is None, 'Supply --engine for real native qualification.')
class NativeContract(unittest.TestCase):
    def test_terminal_error_context_from_native_process(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            with WorldClient.open(BINARY, Path(tmp)/'context.json') as client:
                original = [{'op': 'entity.create', 'id': A, 'name': 'Private context'}]
                with self.assertRaises(RpcError) as caught:
                    client.transact(original, 1, request_id=B)
                error = caught.exception
                self.assertEqual((error.code, error.method, error.request_id),
                                 (-32009, 'world.transact', B))
                self.assertIs(type(error.rpc_id), int)
                params = json.loads(error.params_json)
                self.assertEqual(params['base_revision'], 1)
                self.assertEqual(params['ops'], original)
                self.assertEqual(params['request_id'], B)
                original[0]['name'] = 'Caller edit'
                error.params['ops'][0]['name'] = 'Convenience edit'
                self.assertEqual(json.loads(error.params_json)['ops'][0]['name'], 'Private context')
                self.assertNotIn('Private context', repr(error))
                self.assertFalse(client.closed)
                # Rejection did not commit the requested entity. The same
                # transport accepts a deliberately corrected fresh request.
                self.assertEqual(client.inspect()['revision'], 0)
                self.assertEqual(client.transact(params['ops'], 0)['revision'], 1)
                self.assertEqual(client.get(A)['value']['name'], 'Private context')

    def test_shutdown_host_rejects_standalone_without_closing_client(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            with WorldClient.open(BINARY, Path(tmp)/'standalone.json') as client:
                with self.assertRaises(RpcError) as caught:
                    client.shutdown_host()
                self.assertEqual(caught.exception.code, -32080)
                self.assertEqual(caught.exception.method, 'host.shutdown')
                self.assertEqual(caught.exception.params, {})
                self.assertIsNone(caught.exception.request_id)
                self.assertFalse(client.closed)
                self.assertEqual(client.inspect()['revision'], 0)

    def test_legacy_receipt_still_replays_without_invented_history(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            world = Path(tmp)/'legacy.json'
            request_id = new_id()
            ops = [{'op': 'entity.create', 'id': A, 'name': 'Legacy entity'}]
            with WorldClient.open(BINARY, world) as client:
                client.transact(ops, 0, request_id=request_id)
            document = json.loads(world.read_text(encoding='utf-8'))
            # Receipt shape from pre-history versions, still accepted natively.
            del document['receipts'][0]['result']['history_recorded']
            world.write_text(json.dumps(document), encoding='utf-8')
            with WorldClient.open(BINARY, world) as client:
                replay = client.transact(ops, 0, request_id=request_id)
                self.assertTrue(replay['replayed'])
                self.assertNotIn('history_recorded', replay)
                self.assertEqual(client.inspect()['revision'], 1)

    def test_shared_clients_conflict_retry_and_detach(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            world = Path(tmp)/'shared.json'
            endpoint = 'python-'+new_id()
            host = subprocess.Popen([BINARY, 'serve', str(world), '--endpoint', endpoint],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            client = None
            try:
                # Native connect waits for publication; no readiness polling loop.
                client = WorldClient.connect(BINARY, endpoint, timeout_ms=10000)
                self.assertEqual(client.discover()['session_scope'], 'shared_headless')
                self.assertEqual(client.inspect()['revision'], 0)
                with WorldClient.connect(BINARY, endpoint) as second:
                    request_id = new_id()
                    ops = [{'op': 'entity.create', 'id': A, 'name': 'Shared'}]
                    client.transact(ops, 0, request_id=request_id)
                    self.assertTrue(second.transact(ops, 0, request_id=request_id)['replayed'])
                    with self.assertRaises(RpcError) as caught:
                        second.transact([{'op': 'entity.rename', 'id': A, 'name': 'Stale'}], 0)
                    self.assertEqual(caught.exception.code, -32009)
                    client.close()
                    self.assertIsNone(host.poll(), 'Detachment stopped the shared host.')
                    self.assertEqual(second.get(A)['value']['name'], 'Shared')
                    self.assertEqual(second.history()['undo_count'], 1)
                    with self.assertRaises(RpcError) as rejected:
                        second.call('host.shutdown', {'request_id': new_id()})
                    self.assertEqual(rejected.exception.code, -32602)
                    self.assertIsNone(host.poll(), 'Rejected shutdown stopped the host.')
                    self.assertFalse(second.closed)
                    self.assertEqual(second.get(A)['value']['name'], 'Shared')
                    self.assertTrue(second.shutdown_host()['closed'])
                self.assertEqual(host.wait(timeout=10), 0)
            finally:
                if client is not None:
                    client.close()
                if host.poll() is None:
                    host.terminate()
                    try:
                        host.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        host.kill(); host.wait(timeout=5)
                host.communicate(timeout=5)

    def test_complete_authoring_and_persistent_retry(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            world = Path(tmp)/'世界 world.json'
            request_id = new_id()
            ops = [{'op': 'entity.create', 'id': A, 'name': 'Root'},
                   {'op': 'entity.create', 'id': B, 'name': 'Child', 'parent': A},
                   {'op': 'entity.create', 'id': C, 'name': 'Second root'}]
            with WorldClient.open(BINARY, world) as client:
                self.assertFalse(client.inspect()['persisted'])
                for view, name in (('full', None), ('catalog', None), ('method', 'world.transact'),
                                   ('component', 'Transform'), ('section', 'invariants')):
                    self.assertIn('schema_revision', client.discover(view, name))
                for operation, component in (('entity.create', None), ('component.set', 'Transform'),
                                              ('component.remove', 'Camera')):
                    focused = client.discover_mutation(operation, component)
                    self.assertEqual(list(focused['methods']), ['world.transact'])
                    self.assertEqual(focused['selection']['operation'], operation)
                    if component is not None:
                        self.assertEqual(focused['selection']['type'], component)
                preview = client.transact(ops, 0, request_id=request_id, preview=True)
                self.assertFalse(preview['committed'])
                self.assertEqual(client.inspect()['revision'], 0)
                committed = client.transact(ops, 0, request_id=request_id)
                self.assertEqual(committed['revision'], 1)
                self.assertTrue(client.transact(ops, 0, request_id=request_id)['replayed'])
                self.assertEqual(len(list(client.iter_entities(limit=1))), 3)
                self.assertEqual(len(client.query(parent=None)['entities']), 2)
                self.assertEqual(client.query(parent=A)['entities'][0]['id'], B)
                self.assertEqual(client.get(B)['value']['parent'], A)
                self.assertEqual(client.get(B, component='Transform')['value']['rotation'], [0, 0, 0, 1])
                self.assertEqual(len(client.world_transform(B)['matrix']), 16)
                self.assertEqual(client.history()['undo_count'], 1)
                client.undo(1); client.redo(2)
                with self.assertRaises(RpcError) as caught:
                    client.get(A, revision=1)
                self.assertEqual(caught.exception.code, -32009)
                with self.assertRaises(RpcError) as caught:
                    client.transact(ops, 0, request_id=request_id, preview=True)
                self.assertEqual(caught.exception.code, -32010)
                self.assertEqual(client.call('world.ui.list')['elements'], [])
            with WorldClient.open(BINARY, world) as reopened:
                receipt = reopened.transact(ops, 0, request_id=request_id)
                self.assertTrue(receipt['replayed'])
                self.assertEqual(receipt['revision'], 1, 'Historical receipt incorrectly replaced with current revision.')
                self.assertEqual(reopened.inspect()['revision'], 3)
                self.assertEqual(reopened.history()['undo_count'], 0)

    def test_stale_pagination_propagates_before_mixing_revisions(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            with WorldClient.open(BINARY, Path(tmp)/'world.json') as client:
                client.transact([{'op': 'entity.create', 'id': entity, 'name': entity} for entity in (A, B)], 0)
                pages = client.iter_query_pages(limit=1)
                self.assertEqual(next(pages)['revision'], 1)
                client.transact([{'op': 'entity.rename', 'id': B, 'name': 'Changed'}], 1)
                with self.assertRaises(RpcError) as caught:
                    next(pages)
                self.assertEqual(caught.exception.code, -32009)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *unittest_args], verbosity=2)
