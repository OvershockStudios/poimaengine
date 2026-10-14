# SPDX-License-Identifier: Apache-2.0
"""Owned, read-only exported-game service for the independent Switchback gate."""
from pathlib import Path
import copy
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import queue
import re
import subprocess
import threading
import time
import traceback
import uuid
from switchback_oracle import Owner, require as need, strict_json, file_row
LOG_LIMIT = 32 * 1024 * 1024
RPC_LIMIT = 16 * 1024 * 1024

def require_terminal_eof(frames):
    """After both stream threads join, only the single EOF marker may remain."""
    try:
        frame = frames.get_nowait()
    except queue.Empty:
        raise AssertionError('Native connect did not retain its terminal EOF')
    need(frame is None and frames.empty(), 'Unconsumed native response after final RPC')

def pipe_name(endpoint):
    """Match src/local_session.cpp: SHA256 of the ASCII current-user SID."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel.GetCurrentProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes, kernel.CloseHandle.restype = [w.HANDLE], w.BOOL
    kernel.LocalFree.argtypes, kernel.LocalFree.restype = [ctypes.c_void_p], ctypes.c_void_p
    advapi.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
    advapi.OpenProcessToken.restype = w.BOOL
    advapi.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)]
    advapi.GetTokenInformation.restype = w.BOOL
    advapi.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(w.LPWSTR)]
    advapi.ConvertSidToStringSidW.restype = w.BOOL
    token, count, text = w.HANDLE(), w.DWORD(), w.LPWSTR()
    need(advapi.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)), 'Cannot inspect user token')
    try:
        advapi.GetTokenInformation(token, 1, None, 0, ctypes.byref(count))
        need(0 < count.value <= 65536, 'Unexpected user-token extent')
        storage = ctypes.create_string_buffer(count.value)
        need(advapi.GetTokenInformation(token, 1, storage, count, ctypes.byref(count)), 'Cannot read user SID')
        sid = ctypes.cast(storage, ctypes.POINTER(ctypes.c_void_p))[0]
        need(advapi.ConvertSidToStringSidW(sid, ctypes.byref(text)), 'Cannot describe user SID')
        sid_hash = hashlib.sha256(text.value.encode('ascii')).hexdigest()
    finally:
        if text:
            kernel.LocalFree(ctypes.cast(text, ctypes.c_void_p))
        kernel.CloseHandle(token)
    return '\\\\.\\pipe\\poima-' + sid_hash + '-' + endpoint


class Budget:
    def __init__(self, deadline, paths):
        self.deadline, self.paths = deadline, paths
        self.faults = []

    def remaining(self, maximum=12):
        remaining = self.deadline - time.monotonic()
        need(remaining > 0, 'Bounded worker deadline expired')
        need(not self.faults, 'Native retained stream failed or exceeded its bound')
        for path in self.paths:
            need(not path.exists() or path.stat().st_size <= LOG_LIMIT, 'Stream exceeded 32 MiB: ' + str(path))
        return min(maximum, remaining)


def retain_host_stream(process, role, path, budget):
    total = 0
    try:
        with path.open('wb') as retained:
            while True:
                chunk = getattr(process, role).read(65536)
                if not chunk:
                    return
                allowed = min(len(chunk), LOG_LIMIT - total)
                retained.write(chunk[:allowed])
                retained.flush()
                total += allowed
                need(allowed == len(chunk), 'Host ' + role + ' exceeded 32 MiB; retained prefix is explicit')
    except BaseException:
        budget.faults.append(traceback.format_exc())


def wait_host(host, endpoint, stdout, stderr, budget):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.WaitNamedPipeW.argtypes, kernel.WaitNamedPipeW.restype = [w.LPCWSTR, w.DWORD], w.BOOL
    address = pipe_name(endpoint)
    marker = ('Poima shared world ready: ' + endpoint).encode('utf-8')
    deadline = time.monotonic() + budget.remaining(45)
    while time.monotonic() < deadline:
        budget.remaining()
        need(host.poll() is None, 'Native game serve exited before readiness')
        # Endpoint allocation precedes bundle verification; require the actual
        # post-NativeAOT owner marker as well as an available correct named pipe.
        if marker in stderr.read_bytes() and kernel.WaitNamedPipeW(address, 50):
            need(stdout.stat().st_size == 0, 'Unexpected unsolicited host stdout')
            return {'pipe': address, 'wait_named_pipe': True, 'owner_ready_marker': marker.decode()}
        time.sleep(.02)
    raise TimeoutError('Native game service never became ready')


class RetainedTransport:
    """Documented WorldClient(request/close) embedding, with full wire retention.

    Only one synchronous request can be in flight. No retries, reconnect, or
    implicit process termination. Parent Job cleanup owns all unknown outcomes.
    """
    def __init__(self, argv, output, budget, accepted):
        self.budget, self.closed, self.counter = budget, False, 0
        self.accepted=accepted
        self._condition=threading.Lock(); self._next_id=1
        self.broken = False
        self.argv = argv
        self.frames = queue.Queue(maxsize=4)
        self.faults = []
        self.paths = {name: output / ('connect-' + name + '.ndjson' if name != 'stderr' else 'connect-stderr.txt')
                      for name in ('stdin', 'stdout', 'stderr')}
        budget.paths.extend(self.paths.values())
        self.writer = self.paths['stdin'].open('wb')
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0, shell=False)
        self._process=self.process
        self.threads = [threading.Thread(target=self.pump, args=(name,), daemon=True)
                        for name in ('stdout', 'stderr')]
        for thread in self.threads:
            thread.start()

    def pump(self, role):
        buffer, total = bytearray(), 0
        try:
            with self.paths[role].open('wb') as retained:
                stream = getattr(self.process, role)
                while True:
                    chunk = stream.read(65536)
                    if not chunk:
                        if role == 'stdout':
                            need(not buffer, 'Unterminated native RPC response')
                            self.frames.put_nowait(None)
                        return
                    allowed = min(len(chunk), LOG_LIMIT - total)
                    retained.write(chunk[:allowed])
                    retained.flush()
                    total += allowed
                    need(allowed == len(chunk), role + ' exceeds 32 MiB; retained prefix is explicit')
                    if role != 'stdout':
                        continue
                    buffer.extend(chunk)
                    while b'\n' in buffer:
                        end = buffer.index(b'\n')
                        need(end < RPC_LIMIT, 'Native RPC response exceeds 16 MiB')
                        self.frames.put_nowait(bytes(buffer[:end]))
                        del buffer[:end + 1]
                    need(len(buffer) < RPC_LIMIT, 'Native RPC response exceeds 16 MiB')
        except BaseException:
            self.faults.append(traceback.format_exc())
            try:
                self.frames.put_nowait(None)
            except queue.Full:
                pass

    def request(self, method, params=None, timeout=30):
        from poima_client.errors import RpcError, OutcomeUnknown
        need(not self.closed and not self.broken and not self.faults, 'Native transport unavailable')
        self.counter += 1
        self._next_id=self.counter+1
        payload = json.dumps({'jsonrpc': '2.0', 'id': self.counter, 'method': method,
                              'params': {} if params is None else params}, allow_nan=False,
                             separators=(',', ':')).encode('utf-8') + b'\n'
        need(len(payload) <= 1024 * 1024, 'RPC request exceeds 1 MiB')
        deadline = time.monotonic() + min(timeout, self.budget.remaining())
        self.writer.write(payload)
        self.writer.flush()
        # Payloads are tiny (<4 KiB) and there is only one in flight. The outer
        # preassigned Job still supplies a hard bound on an OS blocked write.
        try:
            self.process.stdin.write(payload)
            self.process.stdin.flush()
            while True:
                need(time.monotonic() < deadline, 'Native RPC deadline exceeded')
                need(not self.faults, 'Native wire pump failed')
                self.budget.remaining()
                try:
                    frame = self.frames.get(timeout=min(.05, deadline - time.monotonic()))
                    break
                except queue.Empty:
                    continue
            need(frame is not None, 'Native connect stdout EOF')
            response = strict_json(frame)
            need(isinstance(response, dict) and response.get('jsonrpc') == '2.0' and
                 type(response.get('id')) is int and response['id'] == self.counter and
                 set(response) in ({'jsonrpc', 'id', 'result'}, {'jsonrpc', 'id', 'error'}), 'Invalid RPC envelope')
            if 'error' in response:
                error=response['error']
                need(isinstance(error,dict) and type(error.get('code')) is int and isinstance(error.get('message'),str), 'Invalid RPC error')
        except Exception as cause:
            self.broken = True
            error = OutcomeUnknown(str(cause) + '; sent RPC outcome is unknown')
            error.rpc_id, error.method = self.counter, method
            error.params_json = json.dumps({} if params is None else params, allow_nan=False)
            raise error from cause
        self.accepted(response)
        if 'error' in response:
            error = response['error']
            need(isinstance(error, dict) and type(error.get('code')) is int and isinstance(error.get('message'), str),
                 'Invalid RPC error')
            raise RpcError(error['code'], error['message'], error.get('data'))
        return response['result']

    @property
    def returncode(self): return self.process.poll()

    @property
    def stderr_tail(self):
        path=self.paths['stderr']
        return path.read_bytes().decode('utf-8',errors='replace') if path.exists() else ''

    @property
    def stderr_truncated(self): return bool(self.faults)

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.process.stdin.close()
        self.process.wait(timeout=self.budget.remaining(10))
        for thread in self.threads:
            thread.join(timeout=self.budget.remaining(5))
        need(all(not thread.is_alive() for thread in self.threads), 'Client streams did not reach EOF')
        self.writer.close()
        need(not self.faults, 'Retained stream pump failed')
        require_terminal_eof(self.frames)
        need(self.process.returncode == 0, 'Native connect did not exit naturally with code zero')


class GameOwner(Owner):
    """One real game.serve host + connect client, both inherited from the Job.

    Construction and closure retain partial evidence. No transport path kills,
    terminates, reconnects, retries or fabricates successful shutdown. Unknown
    outcomes leave the exact assigned supervisor Job as the cleanup boundary.
    """
    def __init__(self, binary, manifest, handoff, admission, deadline, output,
                 save_root, *, endpoint, command_admission):
        need(callable(admission) and callable(command_admission), 'Owned command admission required')
        admission()
        self.deadline,self.raw_limit=deadline,50000
        self.handoff=handoff
        self.calls,self.raw,self.raw_bytes,self.forced=[],[],0,[]
        self.lock=threading.Lock()
        self.output=Path(output);self.output.mkdir(exist_ok=False)
        self.budget=Budget(deadline,[])
        self.host=self.transport=self.client=None
        self.host_threads=[];self.session=None;self.shutdown=False;self.close_errors=[]
        need(type(endpoint) is str and re.fullmatch(r'switchback-[0-9a-f]{32}',endpoint),'Literal unique endpoint required')
        self.endpoint=endpoint
        self.host_argv=(str(binary),'game','serve',str(manifest),'--endpoint',self.endpoint,'--save-root',str(save_root))
        self.argv=(str(binary),'connect',self.endpoint,'--timeout-ms','12000')
        need(command_admission(self.host_argv) is not False and command_admission(self.argv) is not False, 'Game command rejected')
        self.host_paths={role:self.output/('host-'+role+'.txt') for role in ('stdout','stderr')}
        self.budget.paths.extend(self.host_paths.values())
        self.readiness=None;self.closed=False
        self.persist()
        try:
            for path in self.host_paths.values():path.touch(exist_ok=False)
            self.host=subprocess.Popen(list(self.host_argv),stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE,bufsize=0,shell=False)
            self.persist()
            for role,path in self.host_paths.items():
                thread=threading.Thread(target=retain_host_stream,args=(self.host,role,path,self.budget),daemon=True)
                self.host_threads.append(thread);thread.start()
            self.readiness=wait_host(self.host,self.endpoint,self.host_paths['stdout'],self.host_paths['stderr'],self.budget)
            def accepted(response):
                encoded=json.dumps(response,allow_nan=False).encode('utf-8')
                with self.lock:
                    need(len(self.raw)<self.raw_limit and self.raw_bytes+len(encoded)<=64*1048576,'Raw response budget exceeded')
                    self.raw.append(copy.deepcopy(response));self.raw_bytes+=len(encoded)
            self.transport=RetainedTransport(list(self.argv),self.output,self.budget,accepted)
            from poima_client import WorldClient
            self.client=WorldClient(self.transport)
            self.persist()
        except BaseException:
            self.close_errors.append(traceback.format_exc());self.persist();raise

    def evidence(self):
        paths=list(self.host_paths.values())+(list(self.transport.paths.values()) if self.transport else [])
        return {'host_argv':list(self.host_argv),'host_pid':self.host.pid if self.host else None,
                'host_exit_code':self.host.poll() if self.host else None,
                'connect_argv':list(self.argv),'connect_pid':self.transport.process.pid if self.transport else None,
                'connect_exit_code':self.transport.returncode if self.transport else None,
                'readiness':self.readiness,'shutdown_acknowledged':self.shutdown,'closed':self.closed,
                'close_errors':self.close_errors,'host_stream_faults':self.budget.faults,
                'connect_stream_faults':self.transport.faults if self.transport else [],
                'streams':{path.name:file_row(path,LOG_LIMIT) for path in paths if path.is_file()}}

    def persist(self):
        (self.output/'owner.json').write_text(json.dumps(self.evidence(),indent=2,allow_nan=False)+'\n',encoding='utf-8')

    def close(self):
        if self.closed:
            need(not self.close_errors,'Prior game owner cleanup failed')
            return
        try:
            need(self.client is not None and self.transport is not None,'No usable game client for natural shutdown')
            if not self.shutdown:
                status=self.rpc('runtime.status')
                if status['active']:self.rpc('runtime.stop',session_id=status['session_id'])
                reply=self.rpc('host.shutdown')
                need(reply['closed'] is True,'Native host did not acknowledge shutdown')
                self.shutdown=True
            self.client.close()
            self.host.wait(timeout=self.budget.remaining(10))
            for thread in self.host_threads:thread.join(timeout=self.budget.remaining(5))
            need(all(not thread.is_alive() for thread in self.host_threads),'Host stream threads did not reach EOF')
            need(self.host.returncode==0 and self.host_paths['stdout'].stat().st_size==0,'Host did not naturally close with zero/empty stdout')
            need(not self.budget.faults and not self.transport.faults and not self.transport.stderr_tail,'Native stream faults or unexpected connect diagnostics')
            need(self.transport.returncode==0 and self.transport.closed and len(self.calls)==len(self.raw),'Connect/raw population did not close naturally')
            self.closed=True
        except BaseException:
            self.close_errors.append(traceback.format_exc());raise
        finally:self.persist()
