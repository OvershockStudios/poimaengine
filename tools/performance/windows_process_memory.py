#!/usr/bin/env python3
"""Sample one Windows process's working set and private commit; no GPU counters.

Run with native Windows Python. The handle is opened once with query/synchronize
rights and never reopened by PID. JSON records contain no process names, command
lines, account information or environment values. --help works on other systems.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import statistics
import sys
import time

MAX_SAMPLES = 18001
MAX_DURATION = 3600.0
MIN_INTERVAL = .2


def native_windows():
    return os.name == 'nt'


class ProbeError(Exception):
    """A bounded operation/code diagnostic, without arbitrary OS text or paths."""
    def __init__(self, operation, code=None):
        self.operation, self.code = operation, code
        super().__init__(operation)


class ProcessEnded(Exception):
    pass


@dataclass(frozen=True)
class Options:
    pid: int
    duration: float = 60.0
    interval: float = .2
    max_samples: int = MAX_SAMPLES
    system_ram: bool = False
    expected_creation_filetime: int | None = None

    def validate(self):
        if type(self.pid) is not int or not 1 <= self.pid <= 0xffffffff:
            raise ValueError('PID must be an integer in 1..4294967295.')
        for name, value, low, high in (
                ('duration', self.duration, 0.0, MAX_DURATION),
                ('interval', self.interval, MIN_INTERVAL, MAX_DURATION)):
            if type(value) not in (int, float) or not math.isfinite(value) or value < low or value > high:
                raise ValueError(name+' is outside its finite supported range.')
        if self.duration <= 0:
            raise ValueError('Duration must be greater than zero.')
        if type(self.max_samples) is not int or not 1 <= self.max_samples <= MAX_SAMPLES:
            raise ValueError('Max samples must be an integer in 1..18001.')
        if type(self.system_ram) is not bool:
            raise ValueError('System RAM selection must be boolean.')
        value = self.expected_creation_filetime
        if value is not None and (type(value) is not int or not 1 <= value <= 0xffffffffffffffff):
            raise ValueError('Expected creation FILETIME must be a positive unsigned 64-bit integer.')
        return self


class FileTime(ctypes.Structure):
    _fields_ = [('low', ctypes.c_uint32), ('high', ctypes.c_uint32)]


class ProcessMemoryCountersEx(ctypes.Structure):
    _fields_ = [('cb', ctypes.c_uint32), ('PageFaultCount', ctypes.c_uint32)] + [
        (name, ctypes.c_size_t) for name in (
            'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
            'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
            'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class MemoryStatusEx(ctypes.Structure):
    _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [
        (name, ctypes.c_uint64) for name in (
            'total_physical', 'available_physical', 'total_page_file',
            'available_page_file', 'total_virtual', 'available_virtual', 'available_extended_virtual')]


class WindowsProvider:
    """One original process handle, including protection against PID reuse.

    API definitions: Microsoft Learn GetProcessMemoryInfo,
    PROCESS_MEMORY_COUNTERS_EX, GetProcessTimes, WaitForSingleObject and
    GlobalMemoryStatusEx. PrivateUsage is private commit, not resident RAM.
    """
    def __init__(self, options):
        if not native_windows():
            raise ProbeError('native_windows_required')
        self.handle = None
        self.system_ram = options.system_ram
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.psapi = ctypes.WinDLL('psapi', use_last_error=True)
        handle, u32, boolean = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int
        self.kernel.OpenProcess.argtypes = [u32, boolean, u32]
        self.kernel.OpenProcess.restype = handle
        self.kernel.CloseHandle.argtypes = [handle]
        self.kernel.CloseHandle.restype = boolean
        self.kernel.GetProcessTimes.argtypes = [handle] + [ctypes.POINTER(FileTime)]*4
        self.kernel.GetProcessTimes.restype = boolean
        self.kernel.WaitForSingleObject.argtypes = [handle, u32]
        self.kernel.WaitForSingleObject.restype = u32
        self.psapi.GetProcessMemoryInfo.argtypes = [handle, ctypes.POINTER(ProcessMemoryCountersEx), u32]
        self.psapi.GetProcessMemoryInfo.restype = boolean
        self.kernel.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MemoryStatusEx)]
        self.kernel.GlobalMemoryStatusEx.restype = boolean
        # PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE. No privilege changes,
        # VM access, executable lookup, injection, process creation or termination.
        self.handle = self.kernel.OpenProcess(0x1000 | 0x100000, False, options.pid)
        if not self.handle:
            raise ProbeError('OpenProcess', ctypes.get_last_error())
        try:
            self.creation_filetime = self.identity()
        except BaseException:
            self.close()
            raise

    def identity(self):
        creation, exit_time, kernel, user = FileTime(), FileTime(), FileTime(), FileTime()
        if not self.kernel.GetProcessTimes(self.handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                           ctypes.byref(kernel), ctypes.byref(user)):
            raise ProbeError('GetProcessTimes', ctypes.get_last_error())
        return (creation.high << 32) | creation.low

    def require_running(self):
        result = self.kernel.WaitForSingleObject(self.handle, 0)
        if result == 0:
            raise ProcessEnded()
        if result != 258:
            raise ProbeError('WaitForSingleObject', ctypes.get_last_error() if result == 0xffffffff else result)

    def sample(self):
        self.require_running()
        identity = self.identity()
        value = ProcessMemoryCountersEx()
        value.cb = ctypes.sizeof(value)
        if not self.psapi.GetProcessMemoryInfo(self.handle, ctypes.byref(value), value.cb):
            raise ProbeError('GetProcessMemoryInfo', ctypes.get_last_error())
        result = dict(creation_filetime=identity, working_set_bytes=int(value.WorkingSetSize),
                      private_commit_bytes=int(value.PrivateUsage))
        if self.system_ram:
            system = MemoryStatusEx()
            system.length = ctypes.sizeof(system)
            if not self.kernel.GlobalMemoryStatusEx(ctypes.byref(system)):
                raise ProbeError('GlobalMemoryStatusEx', ctypes.get_last_error())
            result.update(system_total_physical_bytes=int(system.total_physical),
                          system_available_physical_bytes=int(system.available_physical))
        # Discard a racing exit rather than report terminated-process counters.
        self.require_running()
        if self.identity() != identity:
            raise ProbeError('process_identity_changed')
        return result

    def close(self):
        if self.handle:
            handle, self.handle = self.handle, None
            if not self.kernel.CloseHandle(handle):
                raise ProbeError('CloseHandle', ctypes.get_last_error())


def summarize(samples):
    """Observed sample extrema, not process-lifetime peaks or time-weighted data."""
    names = ('working_set_bytes', 'private_commit_bytes', 'system_total_physical_bytes',
             'system_available_physical_bytes')
    result = {}
    for name in names:
        values = [sample[name] for sample in samples if name in sample]
        if any(type(value) is not int or not 0 <= value <= 0xffffffffffffffff for value in values):
            raise ValueError('Memory counters must be unsigned integer bytes.')
        if values:
            result[name] = dict(samples=len(values), minimum=min(values), maximum=max(values),
                                first=values[0], last=values[-1], median=statistics.median(values))
    return result


def diagnostic(error):
    if isinstance(error, ProbeError):
        return dict(operation=error.operation, win32_error=error.code)
    return dict(operation='sampler_exception', exception_type=type(error).__name__)


def collect(options, *, provider_factory=WindowsProvider, monotonic=time.monotonic, sleep=time.sleep):
    options.validate()
    report = dict(format='poima.windows-process-memory', version=1, completed=False,
        process=dict(pid=options.pid, creation_filetime_100ns=None),
        requested=dict(duration_seconds=options.duration, interval_seconds=options.interval,
                       maximum_samples=options.max_samples, system_ram=options.system_ram),
        units=dict(memory='bytes', elapsed='seconds', creation_filetime='100ns since 1601-01-01 UTC'),
        metrics=dict(working_set='Resident process working set; may include shared pages.',
                     private_commit='PROCESS_MEMORY_COUNTERS_EX.PrivateUsage; private committed memory, not resident bytes.'),
        gpu=dict(qualified=False, reason='WDDM residency and dedicated/shared GPU memory are not measured.'),
        samples=[], limitations=['Single process only; excludes child-process memory.',
            'Observed sample extrema are not lifetime peaks or time-weighted averages.'])
    provider = None
    try:
        started = monotonic()
        if not math.isfinite(started):
            raise ProbeError('invalid_monotonic_clock')
        previous = started
        provider = provider_factory(options)
        identity = provider.creation_filetime
        if type(identity) is not int or not 1 <= identity <= 0xffffffffffffffff:
            raise ProbeError('invalid_creation_filetime')
        report['process']['creation_filetime_100ns'] = str(identity)
        if options.expected_creation_filetime is not None and identity != options.expected_creation_filetime:
            raise ProbeError('process_identity_changed')
        due = started
        while len(report['samples']) < options.max_samples:
            now = monotonic()
            if not math.isfinite(now) or now < previous:
                raise ProbeError('invalid_monotonic_clock')
            previous = now
            if now-started > options.duration:
                report.update(completed=True, stop_reason='duration_complete')
                break
            if now < due:
                sleep(min(due-now, options.duration-(now-started)))
                continue
            row = provider.sample()
            if type(row.get('creation_filetime')) is not int or row['creation_filetime'] != identity:
                raise ProbeError('process_identity_changed')
            required = {'creation_filetime', 'working_set_bytes', 'private_commit_bytes'}
            if options.system_ram:
                required |= {'system_total_physical_bytes', 'system_available_physical_bytes'}
            if set(row) != required:
                raise ProbeError('invalid_sample_fields')
            sample = dict(elapsed_seconds=now-started, **{k: v for k, v in row.items() if k != 'creation_filetime'})
            summarize([sample])
            if options.system_ram and sample['system_available_physical_bytes'] > sample['system_total_physical_bytes']:
                raise ProbeError('invalid_system_ram')
            report['samples'].append(sample)
            # No catch-up burst after a slow probe/scheduler stall; at most 5 Hz.
            due = now+options.interval
            if due-started > options.duration or now-started >= options.duration:
                report.update(completed=True, stop_reason='duration_complete')
                break
        else:
            report.update(completed=True, stop_reason='sample_limit')
    except ProcessEnded:
        report['stop_reason'] = 'process_ended'
    except KeyboardInterrupt:
        report['stop_reason'] = 'interrupted'
    except Exception as error:
        report.update(stop_reason='error', error=diagnostic(error))
    finally:
        if provider is not None:
            try:
                provider.close()
            except Exception as error:
                report.update(completed=False, close_error=diagnostic(error))
        report['summary'] = summarize(report['samples'])
        report['sample_count'] = len(report['samples'])
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--duration', type=float, default=60)
    parser.add_argument('--interval', type=float, default=.2)
    parser.add_argument('--max-samples', type=int, default=MAX_SAMPLES)
    parser.add_argument('--system-ram', action='store_true')
    parser.add_argument('--expected-creation-filetime', type=int)
    parser.add_argument('--output', type=Path, required=True, help='New JSON file in an existing directory.')
    args = parser.parse_args(argv)
    options = Options(args.pid, args.duration, args.interval, args.max_samples,
                      args.system_ram, args.expected_creation_filetime)
    try:
        options.validate()
    except ValueError as error:
        parser.error(str(error))
    if args.output.exists() or args.output.is_symlink() or not args.output.parent.is_dir():
        parser.error('Output must be a new file in an existing directory.')
    report = collect(options)
    # Exclusive creation prevents overwriting a result that appeared during the
    # measurement. No process/account identifiers beyond PID/creation FILETIME.
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(dict(completed=report['completed'], stop_reason=report['stop_reason'],
                         sample_count=report['sample_count']), allow_nan=False))
    return 0 if report['completed'] else 1


if __name__ == '__main__':
    sys.exit(main())
