#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure one owned idle headless host's CPU time; not power or game FPS.

Run Python on the engine's operating system. Public output omits local paths,
process IDs and diagnostics. CPU counters have OS accounting granularity.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import benchmark_shared_session as probe
from poima_client import WorldClient


class CpuSampler:
    def __init__(self, pid):
        self.pid, self.handle = pid, None
        if os.name == 'nt':
            from ctypes import wintypes
            self.filetime = wintypes.FILETIME
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
            self.kernel.GetProcessTimes.restype = wintypes.BOOL
            self.handle = self.kernel.OpenProcess(0x1000, False, pid)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
        elif not sys.platform.startswith('linux'):
            raise ValueError('CPU sampling supports native Windows and Linux only')

    def read(self):
        if self.handle is not None:
            values = [self.filetime() for unused in range(4)]
            if not self.kernel.GetProcessTimes(self.handle, *(ctypes.byref(value) for value in values)):
                raise ctypes.WinError(ctypes.get_last_error())
            ticks = [(value.dwHighDateTime << 32) | value.dwLowDateTime for value in values[2:]]
            return sum(ticks) / 10000000
        fields = Path('/proc/{}/stat'.format(self.pid)).read_text().rsplit(')', 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')

    def close(self):
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def seconds(text):
    value = float(text)
    if not math.isfinite(value) or not 1 <= value <= 30:
        raise argparse.ArgumentTypeError('Idle interval must be finite and between 1 and 30 seconds')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--world', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=seconds, default=5.0)
    args = parser.parse_args()
    args.engine, args.world, args.output = (p.resolve() for p in (args.engine, args.world, args.output))
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(passed=False, errors=[], engine_path=str(args.engine), world_path=str(args.world))
    client = host = sampler = None
    graceful = False
    try:
        deadline = time.monotonic() + 90
        engine_hash = probe.digest(args.engine, deadline)
        world_hash = probe.digest(args.world, deadline)
        expected = probe.source_identity(args.world)
        world = args.output / 'idle-world.json'
        shutil.copyfile(args.world, world)
        endpoint = 'idle-' + uuid.uuid4().hex
        host = probe.OwnedHost(args.engine, world, endpoint)
        report['host_pid'] = host.process.pid
        client = WorldClient.connect(args.engine, endpoint, timeout_ms=10000, close_timeout=5)
        probe.check_inspection(client.inspect(timeout=10), expected)
        sampler = CpuSampler(host.process.pid)
        probe.require(host.process.poll() is None, 'Owned host exited before measurement')
        before = sampler.read()
        started = time.perf_counter()
        time.sleep(args.seconds)
        wall = time.perf_counter() - started
        after = sampler.read()
        probe.require(host.process.poll() is None, 'Owned host exited during measurement')
        probe.require(after >= before, 'CPU accounting moved backwards')
        probe.check_inspection(client.inspect(timeout=10), expected)
        report['measurement'] = dict(requested_idle_seconds=args.seconds, elapsed_seconds=wall,
            host_cpu_seconds=after-before, host_cpu_percent_of_one_core=100*(after-before)/wall,
            method='GetProcessTimes kernel+user' if os.name == 'nt' else '/proc/pid/stat utime+stime',
            accounting_units_per_second=10000000 if os.name == 'nt' else os.sysconf('SC_CLK_TCK'))
        report['hashes'] = dict(engine=engine_hash, world_input=world_hash,
                                probe=probe.digest(Path(__file__), deadline))
        probe.require(probe.digest(args.engine, deadline)==engine_hash and
                      probe.digest(args.world, deadline)==world_hash and
                      probe.digest(world, deadline)==world_hash, 'Inputs changed')
        probe.require(client.call('host.shutdown', timeout=10)['closed'] is True, 'Shutdown not acknowledged')
        graceful = True
    except BaseException as error:
        report['errors'].append(type(error).__name__ + ': ' + str(error)[:2048])
    finally:
        if sampler is not None:
            sampler.close()
        if client is not None:
            try:
                client.close()
                report['client_exit_code'] = client.transport.returncode
                probe.require(client.transport.returncode == 0, 'Client exit not zero')
            except BaseException as error:
                report['errors'].append('Client cleanup: ' + str(error)[:2048])
        if host is not None:
            try:
                report['host_cleanup'] = host.close(graceful, 5)
                probe.require(report['host_cleanup']['exit_code']==0 and
                              not report['host_cleanup']['forced'] and
                              report['host_cleanup']['diagnostic_error'] is None, 'Host cleanup failed')
            except BaseException as error:
                report['errors'].append('Host cleanup: ' + str(error)[:2048])
    report['passed'] = not report['errors'] and graceful
    (args.output/'local-report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    public = {key:report[key] for key in ('passed','measurement','hashes','client_exit_code') if key in report}
    public['format'] = 'poima.shared-idle-cpu.v1'
    public['platform'] = sys.platform
    public['error_count'] = len(report['errors'])
    if 'host_cleanup' in report:
        public['host_exit_code'] = report['host_cleanup']['exit_code']
        public['forced_cleanup'] = report['host_cleanup']['forced']
    public['limits'] = ['Single idle headless host with one connected idle client; startup, shutdown and queries excluded.',
        'CPU accounting has finite granularity; zero does not prove zero CPU work or energy use.',
        'No power, battery, GUI, rendering or gameplay performance measurement.']
    (args.output/'public-summary.json').write_text(json.dumps(public, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'passed':report['passed'], 'error_count':len(report['errors'])}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
