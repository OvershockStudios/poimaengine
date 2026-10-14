# SPDX-License-Identifier: Apache-2.0
"""Windows owned-process gate. The worker is released only after Job assignment."""
import ctypes
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import time
import traceback
import hashlib
import json
LOG_LIMIT = 8 * 1024 * 1024

def need(condition, message):
    if not condition: raise RuntimeError(message)

def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False)+'\n', encoding='utf-8')

class Job:
    class Basic(ctypes.Structure):
        _fields_ = [('process_time', ctypes.c_longlong), ('job_time', ctypes.c_longlong),
                    ('flags', w.DWORD), ('min_ws', ctypes.c_size_t), ('max_ws', ctypes.c_size_t),
                    ('active', w.DWORD), ('affinity', ctypes.c_size_t), ('priority', w.DWORD),
                    ('scheduling', w.DWORD)]

    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ('readops', 'writeops', 'otherops', 'readbytes', 'writebytes', 'otherbytes')]

    class Accounting(ctypes.Structure):
        _fields_ = [('user_time', ctypes.c_longlong), ('kernel_time', ctypes.c_longlong),
                    ('period_user_time', ctypes.c_longlong), ('period_kernel_time', ctypes.c_longlong),
                    ('page_faults', w.DWORD), ('total_processes', w.DWORD),
                    ('active_processes', w.DWORD), ('terminated_processes', w.DWORD)]

    def __init__(self):
        class Extended(ctypes.Structure):
            _fields_ = [('basic', Job.Basic), ('io', Job.IO), ('process_limit', ctypes.c_size_t),
                        ('job_limit', ctypes.c_size_t), ('peak_process', ctypes.c_size_t),
                        ('peak_job', ctypes.c_size_t)]
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
        for name, args in {
            'SetInformationJobObject': [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD],
            'AssignProcessToJobObject': [w.HANDLE, w.HANDLE],
            'TerminateJobObject': [w.HANDLE, w.UINT],
            'QueryInformationJobObject': [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p],
            'CloseHandle': [w.HANDLE],
        }.items():
            call = getattr(self.kernel, name)
            call.argtypes, call.restype = args, w.BOOL
        self.handle = self.kernel.CreateJobObjectW(None, None)
        need(self.handle, 'CreateJobObjectW failed')
        limits = Extended()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.kernel.CloseHandle(self.handle)
            self.handle = None
            raise RuntimeError('Could not enable Job kill-on-close')

    def assign(self, process):
        need(self.kernel.AssignProcessToJobObject(self.handle, w.HANDLE(int(process._handle))),
             'Could not assign gated child to Job; no gate will be released')

    def accounting(self):
        value = self.Accounting()
        need(self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(value), ctypes.sizeof(value), None),
             'Could not inspect owned Job accounting')
        return {name: getattr(value, name) for name, _ in value._fields_}

    def terminate(self):
        need(self.kernel.TerminateJobObject(self.handle, 1), 'Could not terminate owned Job')

    def close(self):
        if self.handle:
            need(self.kernel.CloseHandle(self.handle), 'Could not close owned Job')
            self.handle = None


def gated(command, directory, timeout):
    """Return retained ownership even on failure; admission never races launch."""
    directory.mkdir(exist_ok=False)
    gate, trampoline = directory / 'release', directory / 'gated.py'
    trampoline.write_text(
        'import pathlib,runpy,sys,time\n'
        'p=pathlib.Path(sys.argv[1]);until=time.monotonic()+30\n'
        'while not p.exists():\n'
        " if time.monotonic()>until:raise TimeoutError('Supervisor gate not released')\n"
        ' time.sleep(.01)\n'
        'sys.argv=sys.argv[2:]\n'
        'sys.path.insert(0,str(pathlib.Path(sys.argv[0]).resolve().parent))\n'
        "runpy.run_path(sys.argv[0],run_name='__main__')\n", encoding='utf-8')
    record = dict(passed=False, forced_cleanup=False, job_assigned=False, cleanup_errors=[], command=command)
    job = child = None
    started = time.monotonic()
    try:
        job = Job()
        with (directory / 'stdout.txt').open('wb') as stdout, (directory / 'stderr.txt').open('wb') as stderr:
            child = subprocess.Popen([sys.executable, str(trampoline), str(gate), *command[1:]],
                stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            record['process_id'] = child.pid
            job.assign(child)
            record['job_assigned'] = True
            gate.write_text('released', encoding='utf-8')
            until = started + timeout
            while child.poll() is None:
                need(time.monotonic() < until, 'Owned child deadline exceeded')
                need(sum((directory / name).stat().st_size for name in ('stdout.txt', 'stderr.txt')) <= LOG_LIMIT,
                     'Owned child diagnostic output exceeded 8 MiB')
                time.sleep(.025)
            record['exit_code'] = child.returncode
        need(sum((directory / name).stat().st_size for name in ('stdout.txt', 'stderr.txt')) <= LOG_LIMIT,
             'Terminal diagnostic output exceeded 8 MiB')
        record['job_accounting_at_child_exit'] = job.accounting()
        need(record['job_accounting_at_child_exit']['active_processes'] == 0,
             'Owned child exited with surviving descendants')
        need(record['job_accounting_at_child_exit']['terminated_processes'] == 0, 'Owned descendants were forcibly terminated')
        need(child.returncode == 0, 'Owned child exited nonzero')
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        try:
            if job and record['job_assigned'] and job.accounting()['active_processes']:
                record['forced_cleanup'] = True
                job.terminate()
            elif child and child.poll() is None:
                # The unreleased gated child cannot yet import/launch a fixture.
                record['forced_cleanup'] = True
                child.kill()
            if child:
                child.wait(timeout=10)
                record['exit_code'] = child.returncode
            if job:
                until = time.monotonic() + 5
                while job.accounting()['active_processes'] and time.monotonic() < until:
                    time.sleep(.025)
                record['job_accounting_after_cleanup'] = job.accounting()
                need(record['job_accounting_after_cleanup']['active_processes'] == 0,
                     'Owned Job did not become empty after cleanup')
        except BaseException:
            record['cleanup_errors'].append(traceback.format_exc())
        finally:
            if job:
                try:
                    job.close()
                except BaseException:
                    record['cleanup_errors'].append(traceback.format_exc())
            # Closing this exact Job is also the last cleanup boundary if an
            # earlier accounting/termination API failed. Reap its direct child.
            if child and child.poll() is None:
                try:
                    child.wait(timeout=10)
                except BaseException:
                    record['cleanup_errors'].append(traceback.format_exc())
        record['seconds'] = time.monotonic() - started
        record['outputs'] = {name: dict(bytes=(directory / name).stat().st_size, sha256=sha(directory / name))
                             for name in ('stdout.txt', 'stderr.txt', 'gated.py') if (directory / name).exists()}
        record['passed'] = bool(record['passed'] and not record['forced_cleanup'] and not record['cleanup_errors']
                               and record.get('job_accounting_after_cleanup',{}).get('terminated_processes') == 0)
        write(directory / 'owner.json', record)
    return record

