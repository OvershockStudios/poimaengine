#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Raw transport tracing with in-memory peers; no subprocess is launched."""
import builtins
import collections
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/python'))
from poima_client import JsonRpcProcess, OutcomeUnknown, RpcError, TransportError
from poima_client import transport as module


class Pipe:
    def __init__(self):
        self.condition = threading.Condition()
        self.chunks = collections.deque()
        self.eof = False
        self.closed = False
        self.gate = None
        self.entered = threading.Event()

    def feed(self, data):
        with self.condition:
            self.chunks.append(data)
            self.condition.notify_all()

    def finish(self):
        with self.condition:
            self.eof = True
            self.condition.notify_all()

    def read(self, size):
        self.entered.set()
        if self.gate is not None:
            self.gate.wait()
        with self.condition:
            while not self.chunks and not self.eof:
                self.condition.wait()
            if not self.chunks:
                return b''
            chunk = self.chunks.popleft()
            if len(chunk) > size:
                self.chunks.appendleft(chunk[size:])
            return chunk[:size]

    def close(self):
        self.closed = True
        self.finish()


class Input:
    def __init__(self, peer, width):
        self.peer = peer
        self.width = width
        self.accepted = bytearray()
        self.pending = bytearray()
        self.closed = False

    def write(self, data):
        chunk = bytes(data[:self.width])
        self.accepted.extend(chunk)
        self.pending.extend(chunk)
        while b'\n' in self.pending:
            line, _, rest = self.pending.partition(b'\n')
            self.pending = bytearray(rest)
            # Feed the response before write returns: acceptance must not outrun
            # recording the final accepted stdin bytes.
            self.peer.reply(json.loads(line))
        return len(chunk)

    def close(self):
        self.closed = True
        self.peer.finish()


class Peer:
    def __init__(self, width=1 << 20, reply=None):
        self.pid = 123
        self.stdout, self.stderr = Pipe(), Pipe()
        self.stdin = Input(self, width)
        self.done = threading.Event()
        self.returncode = None
        self.response = reply
        self.replies = []
        self.on_finish = None

    def reply(self, request):
        raw = (self.response(request) if self.response else
               json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                           'result': request['params']}).encode() + b'\n')
        self.replies.append(raw)
        self.stdout.feed(raw)

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        if not self.done.wait(timeout):
            raise subprocess.TimeoutExpired(['fake'], timeout)
        return self.returncode

    def finish(self, code=0):
        if self.on_finish is not None:
            callback, self.on_finish = self.on_finish, None
            callback()
        self.returncode = code
        self.stdout.finish()
        self.stderr.finish()
        self.done.set()

    def terminate(self):
        self.finish(-15)

    def kill(self):
        self.finish(-9)


class FileWrapper:
    def __init__(self, stream, *, write=None, close_error=False):
        self.stream = stream
        self.write_override = write
        self.close_error = close_error

    @property
    def closed(self):
        return self.stream.closed

    def write(self, data):
        return self.write_override(self.stream, data) if self.write_override else self.stream.write(data)

    def close(self):
        self.stream.close()
        if self.close_error:
            raise OSError('injected trace close failure')


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'trace'
        self.transports = []
        self.addCleanup(self.retire)

    def retire(self):
        for process in self.transports:
            try:
                process.close()
            except TransportError:
                pass
            if process._cleanup_thread is not None:
                process._cleanup_thread.join(2)
                self.assertFalse(process._cleanup_thread.is_alive())

    def start(self, peer=None, **kwargs):
        peer = peer or Peer()
        with patch.object(module.subprocess, 'Popen', return_value=peer) as launch:
            process = JsonRpcProcess(['memory-peer'], **kwargs)
        self.assertEqual(launch.call_count, 1)
        self.transports.append(process)
        return process, peer

    def traced(self, peer=None, **kwargs):
        return self.start(peer, trace_directory=self.path, **kwargs)

    def raw(self, name):
        return (self.path / name).read_bytes()

    def wait_for(self, predicate):
        deadline = time.monotonic() + 2
        while not predicate() and time.monotonic() < deadline:
            threading.Event().wait(0.001)
        self.assertTrue(predicate())

    def test_default_has_no_trace_filesystem_work_or_extra_thread(self):
        with patch.object(module.os, 'mkdir', side_effect=AssertionError('trace mkdir')), \
             patch.object(module, 'open', side_effect=AssertionError('trace open'), create=True):
            process, _ = self.start()
            self.assertEqual(process.request('echo', {'x': 1}, timeout=1), {'x': 1})
            process.close()
        self.assertEqual(len(process._threads), 3)
        self.assertIsNone(process._trace)

    def test_partial_stdin_and_unmodified_whitespace_unicode_response(self):
        def reply(request):
            return (' { "id": %d, "result": "rosé", "jsonrpc": "2.0" }\r\n' % request['id']).encode()
        process, peer = self.traced(Peer(width=3, reply=reply))
        self.assertEqual(process.request('echo', {'text': 'été'}, timeout=1), 'rosé')
        process.close()
        self.assertEqual(self.raw('stdin.ndjson'), bytes(peer.stdin.accepted))
        self.assertEqual(self.raw('stdout.ndjson'), b''.join(peer.replies))
        self.assertEqual(self.raw('stderr.log'), b'')
        self.assertEqual(json.loads(self.raw('stdin.ndjson'))['params'], {'text': 'été'})

    def test_native_error_data_and_context_are_not_changed(self):
        def reply(request):
            return json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'error':
                               {'code': -32009, 'message': 'unchanged', 'data': {'kind': [1, None]}}}).encode() + b'\n'
        process, peer = self.traced(Peer(reply=reply))
        with self.assertRaises(RpcError) as caught:
            process.request('mutate', {'request_id': 'receipt'}, timeout=1)
        self.assertEqual(caught.exception.data, {'kind': [1, None]})
        self.assertEqual(caught.exception.params, {'request_id': 'receipt'})
        self.assertEqual(caught.exception.rpc_id, 1)
        process.close()
        self.assertEqual(self.raw('stdout.ndjson'), b''.join(peer.replies))

    def test_invalid_utf8_and_unterminated_stdout_preserved(self):
        for index, raw in enumerate((b'\xff\n', b'{"jsonrpc":"2.0"')):
            with self.subTest(raw=raw):
                self.path = Path(self.temporary.name) / ('invalid-' + str(index))
                peer = Peer(reply=lambda request, raw=raw: raw)
                if not raw.endswith(b'\n'):
                    original = peer.reply
                    def reply(request):
                        original(request)
                        peer.stdout.finish()
                    peer.reply = reply
                process, _ = self.traced(peer)
                with self.assertRaises(OutcomeUnknown):
                    process.request('bad-output', timeout=1)
                process.close()
                self.assertEqual(self.raw('stdout.ndjson'), raw)

    def test_stderr_is_complete_before_tail_decoding_and_truncation(self):
        process, peer = self.traced(stderr_limit=16)
        raw = b'a' * 100000 + b'\xffTAIL'
        peer.stderr.feed(raw)
        self.wait_for(lambda: process.stderr_truncated and process.stderr_tail.endswith('TAIL'))
        process.close()
        self.assertEqual(self.raw('stderr.log'), raw)
        self.assertEqual(process.stderr_tail, raw[-16:].decode(errors='replace'))

    def test_stdin_limit_retains_only_accepted_prefix_and_unknown_outcome(self):
        process, peer = self.traced(Peer(width=7), trace_limit_bytes=8)
        with self.assertRaises(OutcomeUnknown) as caught:
            process.request('cap', timeout=1)
        self.assertEqual(caught.exception.rpc_id, 1)
        with self.assertRaises(TransportError):
            process.close()
        self.assertEqual(len(peer.stdin.accepted), 14)
        self.assertEqual(self.raw('stdin.ndjson'), bytes(peer.stdin.accepted[:8]))
        self.assertIsNotNone(process.cleanup_error)

    def test_stdout_limit_preserves_raw_prefix_and_never_returns_result(self):
        process, peer = self.traced(Peer(reply=lambda request: b'X' * 200 + b'\n'), trace_limit_bytes=128)
        with self.assertRaises(OutcomeUnknown):
            process.request('cap', timeout=1)
        with self.assertRaises(TransportError):
            process.close()
        self.assertEqual(self.raw('stdout.ndjson'), peer.replies[0][:128])

    def test_stderr_limit_before_send_keeps_unsent_classification(self):
        process, peer = self.traced(trace_limit_bytes=4)
        peer.stderr.feed(b'abcdef')
        self.wait_for(lambda: process.closed)
        with self.assertRaises(TransportError) as caught:
            process.request('not-sent', timeout=1)
        self.assertNotIsInstance(caught.exception, OutcomeUnknown)
        with self.assertRaises(TransportError):
            process.close()
        self.assertEqual(bytes(peer.stdin.accepted), b'')
        self.assertEqual(self.raw('stderr.log'), b'abcd')

    def test_completed_response_cannot_hide_failed_request_trace(self):
        stdout_recorded = threading.Event()
        writes = 0
        handles = []
        def opening(path, *args, **kwargs):
            nonlocal writes
            stream = builtins.open(path, *args, **kwargs)
            if Path(path).name == 'stdin.ndjson':
                def write(target, data):
                    nonlocal writes
                    writes += 1
                    if writes == 1:
                        return target.write(data[:5])
                    if not stdout_recorded.wait(1):
                        raise AssertionError('response was not retained')
                    raise OSError('injected request trace failure')
                wrapped = FileWrapper(stream, write=write)
            elif Path(path).name == 'stdout.ndjson':
                def write(target, data):
                    count = target.write(data)
                    stdout_recorded.set()
                    return count
                wrapped = FileWrapper(stream, write=write)
            else:
                wrapped = FileWrapper(stream)
            handles.append(wrapped)
            return wrapped
        with patch.object(module, 'open', side_effect=opening, create=True):
            process, peer = self.traced()
            with self.assertRaises(OutcomeUnknown):
                process.request('echo', {'sent': True}, timeout=2)
            with self.assertRaises(TransportError):
                process.close()
        self.assertEqual(self.raw('stdin.ndjson'), bytes(peer.stdin.accepted[:5]))
        self.assertEqual(self.raw('stdout.ndjson'), peer.replies[0])
        self.assertTrue(all(stream.closed for stream in handles))

    def test_late_stderr_failure_after_reply_and_closed_flag_surfaces_close(self):
        process, peer = self.traced(trace_limit_bytes=128)
        self.assertEqual(process.request('echo', {'ok': True}, timeout=1), {'ok': True})
        # Explicit close sets _closed before graceful stdin EOF. The raw error
        # then occurs during retirement, with no pending call to carry it.
        peer.on_finish = lambda: peer.stderr.feed(b'late' * 40)
        with self.assertRaises(TransportError):
            process.close()
        self.assertEqual(len(self.raw('stderr.log')), 128)
        self.assertIsNotNone(process.cleanup_error)

    def test_validation_existing_directory_and_partial_open_never_launch(self):
        invalid = ({'trace_limit_bytes': True}, {'trace_limit_bytes': 0},
                   {'trace_limit_bytes': 128 * 1024 * 1024 + 1},
                   {'trace_directory': b'bytes'}, {'trace_directory': ''},
                   {'trace_directory': 'bad\0path'})
        with patch.object(module.subprocess, 'Popen') as launch:
            for options in invalid:
                with self.subTest(options=options), self.assertRaises(ValueError):
                    JsonRpcProcess(['memory-peer'], **options)
            self.path.mkdir()
            with self.assertRaises(TransportError):
                JsonRpcProcess(['memory-peer'], trace_directory=self.path)
            launch.assert_not_called()
        self.path = Path(self.temporary.name) / 'partial'
        handles = []
        def opening(path, *args, **kwargs):
            if Path(path).name == 'stdout.ndjson':
                raise OSError('injected opening failure')
            stream = builtins.open(path, *args, **kwargs)
            handles.append(stream)
            return stream
        with patch.object(module, 'open', side_effect=opening, create=True), \
             patch.object(module.subprocess, 'Popen') as launch:
            with self.assertRaises(TransportError):
                JsonRpcProcess(['memory-peer'], trace_directory=self.path)
            launch.assert_not_called()
        self.assertEqual([item.name for item in self.path.iterdir()], ['stdin.ndjson'])
        self.assertTrue(all(stream.closed for stream in handles))

    def test_process_launch_failure_closes_trace_handles(self):
        handles = []
        def opening(path, *args, **kwargs):
            stream = builtins.open(path, *args, **kwargs)
            handles.append(stream)
            return stream
        with patch.object(module, 'open', side_effect=opening, create=True), \
             patch.object(module.subprocess, 'Popen', side_effect=OSError('launch failure')):
            with self.assertRaises(TransportError):
                JsonRpcProcess(['memory-peer'], trace_directory=self.path)
        self.assertEqual(len(handles), 3)
        self.assertTrue(all(stream.closed for stream in handles))

    def test_partial_thread_start_failure_retires_existing_threads_and_traces(self):
        original_start = threading.Thread.start
        peer = Peer()
        def start(thread):
            if thread.name == 'poima-stderr':
                raise RuntimeError('injected thread startup failure')
            return original_start(thread)
        handles = []
        def opening(path, *args, **kwargs):
            stream = builtins.open(path, *args, **kwargs)
            handles.append(stream)
            return stream
        with patch.object(module, 'open', side_effect=opening, create=True), \
             patch.object(module.subprocess, 'Popen', return_value=peer), \
             patch.object(module.threading.Thread, 'start', start):
            with self.assertRaisesRegex(RuntimeError, 'thread startup failure'):
                JsonRpcProcess(['memory-peer'], trace_directory=self.path, close_timeout=0.2)
        self.assertEqual(peer.poll(), 0)
        self.assertTrue(all(stream.closed for stream in handles))

    def test_trace_close_failure_is_not_masked_by_success_or_primary_error(self):
        handles = []
        def opening(path, *args, **kwargs):
            wrapped = FileWrapper(builtins.open(path, *args, **kwargs),
                                  close_error=Path(path).name == 'stdin.ndjson')
            handles.append(wrapped)
            return wrapped
        with patch.object(module, 'open', side_effect=opening, create=True):
            process, _ = self.traced()
            self.assertEqual(process.request('echo', timeout=1), {})
            primary = RpcError(-32009, 'body error')
            with self.assertRaises(RpcError) as caught:
                with process:
                    raise primary
            self.assertIs(caught.exception, primary)
            self.assertIsNotNone(process.cleanup_error)
            with self.assertRaisesRegex(TransportError, 'injected trace close failure'):
                process.close()
        self.assertTrue(all(stream.closed for stream in handles))

    def test_trace_files_stay_open_until_surviving_reader_retires(self):
        peer = Peer()
        gate = threading.Event()
        peer.stderr.gate = gate
        process, _ = self.traced(peer, close_timeout=0.06)
        self.assertTrue(peer.stderr.entered.wait(1))
        try:
            with self.assertRaisesRegex(TransportError, 'I/O thread did not stop'):
                process.close()
            self.assertTrue(peer.done.is_set())
            self.assertTrue(all(not stream.closed for stream in process._trace.files.values()))
        finally:
            gate.set()
            process._cleanup_thread.join(2)
        self.assertFalse(process._cleanup_thread.is_alive())
        self.assertTrue(all(stream.closed for stream in process._trace.files.values()))


if __name__ == '__main__':
    unittest.main()
