# SPDX-License-Identifier: Apache-2.0
"""Bounded, call-only JSON-RPC over an explicitly owned native subprocess.

There is no shell, automatic retry, reconnect, or notification support. A call
deadline includes queuing, writing and waiting for its response. A deadline or
protocol failure disconnects the whole channel: sent calls have unknown
outcomes, while calls still queued are known not to have been sent.
"""
import collections
import json
import math
import os
import subprocess
import threading
import time

from .errors import OutcomeUnknown, RpcError, TransportError

REQUEST_LIMIT = 1024 * 1024
RESPONSE_LIMIT = 16 * 1024 * 1024
STDERR_LIMIT = 65536
_MISSING = object()


class _Pending:
    def __init__(self, request_id, payload, deadline, method, params_json):
        self.request_id = request_id
        self.payload = payload
        self.deadline = deadline
        self.method = method
        self.params_json = params_json
        self.event = threading.Event()
        self.state = "queued"
        self.result = _MISSING
        self.error = None


def _finite_positive(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(name + " must be a positive finite number")
    try:
        number = float(value)
    except OverflowError as error:
        raise ValueError(name + " exceeds the supported timeout range") from error
    if not math.isfinite(number) or not 0 < number <= threading.TIMEOUT_MAX / 2:
        raise ValueError(name + " must be positive and within the platform timeout range")
    return number


def _strict_json(frame):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ValueError("Duplicate JSON object key")
            result[key] = value
        return result

    def floating(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("Non-finite JSON number")
        return number

    def invalid(value):
        raise ValueError("Non-finite JSON number")

    return json.loads(frame.decode("utf-8"), object_pairs_hook=pairs,
                      parse_float=floating, parse_constant=invalid)


def _encode(request):
    chunks = []
    size = 1  # The terminating newline is part of the request bound.
    encoder = json.JSONEncoder(ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    for chunk in encoder.iterencode(request):
        data = chunk.encode("utf-8")
        size += len(data)
        if size > REQUEST_LIMIT:
            raise ValueError("JSON-RPC request exceeds 1 MiB")
        chunks.append(data)
    return b"".join(chunks) + b"\n"


class JsonRpcProcess:
    """Own one explicit ``poima world`` or ``poima connect`` stdio process.

    ``params=None`` sends an empty object. A process has no generic startup
    handshake: launch errors are immediate, and native startup failures surface
    through the first request or the process/diagnostic properties. ``close``
    is bounded and idempotent and reaps the directly owned subprocess; there is
    no generic Windows descendant-tree cleanup guarantee.
    """

    def __init__(self, argv, *, cwd=None, max_pending=32, stderr_limit=STDERR_LIMIT,
                 close_timeout=10.0):
        if isinstance(argv, (str, bytes)) or not isinstance(argv, (list, tuple)) or not argv:
            raise ValueError("argv must be a nonempty explicit list or tuple")
        if any(not isinstance(value, str) or "\0" in value for value in argv) or not argv[0]:
            raise ValueError("argv entries must be strings without NUL, with a nonempty executable")
        if isinstance(max_pending, bool) or not isinstance(max_pending, int) or not 1 <= max_pending <= 128:
            raise ValueError("max_pending must be between 1 and 128")
        if isinstance(stderr_limit, bool) or not isinstance(stderr_limit, int) or not 1 <= stderr_limit <= STDERR_LIMIT:
            raise ValueError("stderr_limit must be between 1 and 65536")
        self._close_timeout = _finite_positive(close_timeout, "close_timeout")
        self._limit = max_pending
        self._stderr_limit = stderr_limit
        self._condition = threading.Condition()
        self._pending = {}
        self._queue = collections.deque()
        self._next_id = 1
        self._closed = False
        self._failure = None
        self._stderr = bytearray()
        self._stderr_dropped = 0
        self._cleanup_done = threading.Event()
        self._cleanup_thread = None
        self._cleanup_error = None
        self._threads = []
        self._thread_by_role = {}
        self._writer_active = False
        options = dict(stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       cwd=cwd, shell=False, bufsize=0)
        if os.name == "posix":
            options["start_new_session"] = True
        try:
            self._process = subprocess.Popen(list(argv), **options)
        except (OSError, ValueError) as error:
            raise TransportError("Cannot launch native process: " + str(error)) from error
        try:
            for name, target in (("stdout", self._read_stdout), ("stderr", self._read_stderr),
                                 ("writer", self._write_requests)):
                thread = threading.Thread(target=target, name="poima-" + name, daemon=True)
                self._threads.append(thread)
                self._thread_by_role[name] = thread
                thread.start()
        except BaseException:
            self._disconnect("Transport thread startup failed")
            self.close()
            raise

    @property
    def closed(self):
        with self._condition:
            return self._closed

    @property
    def process_id(self):
        return self._process.pid

    @property
    def returncode(self):
        return self._process.poll()

    @property
    def stderr_tail(self):
        with self._condition:
            return bytes(self._stderr).decode("utf-8", errors="replace")

    @property
    def stderr_truncated(self):
        with self._condition:
            return self._stderr_dropped != 0

    def request(self, method, params=None, timeout=30.0):
        deadline = time.monotonic() + _finite_positive(timeout, "timeout")
        if not isinstance(method, str) or not method or "\0" in method:
            raise ValueError("method must be a nonempty string without NUL")
        if params is None:
            params = {}
        if not isinstance(params, (dict, list)):
            raise ValueError("params must be an object, array, or None")
        with self._condition:
            if self._closed:
                raise TransportError(self._failure or "Transport is closed")
            if len(self._pending) >= self._limit:
                raise TransportError("Concurrent request capacity reached; request was not sent")
            if self._next_id > (1 << 53) - 1:
                raise TransportError("Request identifier space exhausted")
            request_id = self._next_id
            self._next_id += 1
            pending = _Pending(request_id, b"", deadline, method, "")
            pending.state = "encoding"
            self._pending[request_id] = pending
        try:
            payload = _encode(dict(jsonrpc="2.0", id=request_id, method=method, params=params))
            snapshot = _strict_json(payload[:-1])["params"]
            params_json = json.dumps(snapshot, ensure_ascii=False, allow_nan=False,
                                     separators=(",", ":"))
        except BaseException as error:
            with self._condition:
                self._pending.pop(request_id, None)
            if isinstance(error, (TypeError, ValueError, UnicodeError, RecursionError)):
                raise ValueError("Cannot encode request: " + str(error)) from error
            raise
        with self._condition:
            if self._closed:
                raise TransportError(self._failure or "Transport is closed")
            if time.monotonic() >= deadline:
                self._pending.pop(request_id, None)
                raise TransportError("Request deadline expired before enqueue; request was not sent")
            pending.payload = payload
            pending.params_json = params_json
            pending.state = "queued"
            self._queue.append(pending)
            self._condition.notify()
        remaining = max(0.0, deadline - time.monotonic())
        try:
            ready = pending.event.wait(remaining)
        except BaseException as error:
            self._disconnect("Request wait was interrupted; transport disconnected")
            # Classification must come from the atomic disconnect. Sampling
            # state first races a writer starting or another closer completing
            # the pending call before this interrupt acquires the lock.
            if isinstance(pending.error, OutcomeUnknown):
                error.rpc_id = pending.request_id
                error.method = pending.method
                error.params_json = pending.params_json
            raise
        if not ready:
            with self._condition:
                timed_out = not pending.event.is_set()
            if timed_out:
                self._disconnect("Request deadline expired; transport disconnected")
        if pending.error is not None:
            raise pending.error
        return pending.result

    def _write_requests(self):
        try:
            while True:
                with self._condition:
                    while not self._queue and not self._closed:
                        self._condition.wait()
                    if self._closed:
                        return
                    pending = self._queue.popleft()
                    if time.monotonic() >= pending.deadline:
                        expired = True
                    else:
                        expired = False
                        pending.state = "writing"
                        self._writer_active = True
                if expired:
                    self._disconnect("Request deadline expired before write")
                    return
                view = memoryview(pending.payload)
                while view:
                    written = self._process.stdin.write(view)
                    if written is None or written <= 0:
                        raise OSError("Native stdin did not accept request bytes")
                    view = view[written:]
                with self._condition:
                    self._writer_active = False
                    if pending.state != "completed":
                        pending.state = "written"
                pending.payload = b""
        except Exception as error:
            self._disconnect("Native request write failed: " + str(error))
        finally:
            with self._condition:
                self._writer_active = False

    def _read_stdout(self):
        buffer = bytearray()
        try:
            while True:
                chunk = self._process.stdout.read(65536)
                if not chunk:
                    reason = "Native process closed stdout"
                    if buffer:
                        reason = "Native process closed stdout with an unterminated response"
                    self._disconnect(reason)
                    return
                buffer.extend(chunk)
                while True:
                    newline = buffer.find(b"\n")
                    if newline < 0:
                        if len(buffer) >= RESPONSE_LIMIT:
                            raise ValueError("JSON-RPC response exceeds 16 MiB")
                        break
                    if newline + 1 > RESPONSE_LIMIT:
                        raise ValueError("JSON-RPC response exceeds 16 MiB")
                    frame = bytes(buffer[:newline])
                    del buffer[:newline + 1]
                    self._accept_response(_strict_json(frame))
        except Exception as error:
            self._disconnect("Invalid native response: " + str(error))

    def _accept_response(self, response):
        if not isinstance(response, dict) or response.get("jsonrpc") != "2.0":
            raise ValueError("Expected JSON-RPC 2.0 response object")
        keys = set(response)
        if keys not in ({"jsonrpc", "id", "result"}, {"jsonrpc", "id", "error"}):
            raise ValueError("Response must have id and exactly one result or error")
        request_id = response["id"]
        if type(request_id) is not int:
            raise ValueError("Response id must be an exact integer request id")
        rpc_error = None
        if "error" in response:
            error = response["error"]
            if (not isinstance(error, dict) or not {"code", "message"} <= set(error)
                    or set(error) - {"code", "message", "data"}
                    or type(error["code"]) is not int or not isinstance(error["message"], str)):
                raise ValueError("Invalid JSON-RPC error object")
            rpc_error = RpcError(error["code"], error["message"], error.get("data"))
        with self._condition:
            pending = self._pending.get(request_id)
            if pending is None or pending.state not in ("writing", "written"):
                raise ValueError("Response id is unknown, duplicated, or not yet sent")
            del self._pending[request_id]
            pending.state = "completed"
            pending.error = rpc_error
            pending.result = response.get("result", _MISSING)
            pending.event.set()

    def _read_stderr(self):
        try:
            while True:
                chunk = self._process.stderr.read(4096)
                if not chunk:
                    return
                with self._condition:
                    self._stderr.extend(chunk)
                    overflow = max(0, len(self._stderr) - self._stderr_limit)
                    if overflow:
                        del self._stderr[:overflow]
                        self._stderr_dropped += overflow
        except Exception as error:
            self._disconnect("Native stderr read failed: " + str(error))

    def _disconnect(self, reason):
        with self._condition:
            if self._closed:
                return
            self._closed = True
            self._failure = reason
            for pending in self._pending.values():
                uncertain = pending.state in ("writing", "written")
                error_type = OutcomeUnknown if uncertain else TransportError
                pending.error = error_type(reason)
                pending.error.rpc_id = pending.request_id
                pending.error.method = pending.method
                pending.error.params_json = pending.params_json
                pending.state = "completed"
                pending.payload = b""
                pending.event.set()
            self._pending.clear()
            self._queue.clear()
            self._condition.notify_all()
            self._cleanup_thread = threading.Thread(target=self._cleanup,
                                                     name="poima-cleanup", daemon=True)
            self._cleanup_thread.start()

    def _cleanup(self):
        deadline = time.monotonic() + self._close_timeout
        try:
            # Do not close a descriptor underneath an active writer. Even raw
            # FileIO can encounter platform fd locks around a blocked syscall.
            # Idle stdin gets graceful EOF; otherwise terminate the child first.
            with self._condition:
                writer_active = self._writer_active
            if not writer_active:
                self._process.stdin.close()
            phase = self._close_timeout / 3
            try:
                self._process.wait(timeout=phase)
            except subprocess.TimeoutExpired:
                self._process.terminate()
                try:
                    self._process.wait(timeout=min(phase, max(0, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait(timeout=max(0.001, deadline - time.monotonic()))
        except Exception as error:
            self._cleanup_error = error
        finally:
            for thread in self._threads:
                if thread.ident is not None:
                    thread.join(timeout=max(0, deadline - time.monotonic()))
            for role, stream in (("writer", self._process.stdin),
                                 ("stdout", self._process.stdout), ("stderr", self._process.stderr)):
                thread = self._thread_by_role.get(role)
                if thread is not None and thread.is_alive():
                    # A descendant outside the direct-child guarantee may keep
                    # a pipe open. Never make close unbounded by taking its fd
                    # lock; the daemon reader will release it when that ends.
                    if self._cleanup_error is None:
                        self._cleanup_error = TransportError("Owned process exited but an I/O thread did not stop")
                    continue
                try:
                    stream.close()
                except OSError:
                    pass
            self._cleanup_done.set()

    def close(self):
        self._disconnect("Transport was closed; interrupted calls have no trusted outcome")
        if not self._cleanup_done.wait(self._close_timeout + 0.1):
            raise TransportError("Owned process cleanup exceeded its bounded deadline")
        if self._cleanup_error is not None:
            raise TransportError("Owned process cleanup failed: " + str(self._cleanup_error))

    def __enter__(self):
        if self.closed:
            raise TransportError(self._failure or "Transport is closed")
        return self

    def __exit__(self, exception_type, exception, traceback):
        try:
            self.close()
        except TransportError:
            if exception_type is None:
                raise
        return False
