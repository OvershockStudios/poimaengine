#!/usr/bin/env python3
"""Finite native Windows self-process probe; no engine, window or GPU work.

The phase names are test labels. They do not represent actual engine gameplay.
Run explicitly with native Windows Python and a new --output JSON file.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/performance'))
import memory_phases
import windows_process_memory as memory


def run():
    if os.name != 'nt':
        raise RuntimeError('Native Windows Python required')
    sources = [Path(__file__), ROOT/'tools/performance/memory_phases.py',
               ROOT/'tools/performance/windows_process_memory.py']
    def pins():
        return {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sources}
    before = pins()
    origin = time.monotonic_ns()
    phases = memory_phases.PhaseLog(origin)
    pid = os.getpid()

    class Provider:
        def __init__(self, options):
            self.actual = memory.WindowsProvider(options)
            self.creation_filetime = self.actual.creation_filetime
            self.count = 0
            try:
                self.mark('startup')
            except BaseException:
                self.actual.close()
                raise

        def mark(self, phase):
            phases.mark(phase, owner='probe', pid=pid,
                creation_filetime_100ns=str(self.creation_filetime))

        def sample(self):
            # Actual timestamps inside the measured provider call deliberately
            # straddle two phase boundaries; all counters come from Windows.
            if self.count in (0, 4):
                self.mark('play' if self.count == 0 else 'pause')
            self.count += 1
            return self.actual.sample()

        def close(self):
            self.actual.close()

    report = memory.collect(memory.Options(pid, duration=2, interval=.2,
        max_samples=7, system_ram=True, query_timing=True),
        provider_factory=Provider, clock_origin_ns=origin)
    phases.mark('cleanup', owner='probe', pid=pid,
        creation_filetime_100ns=report['process']['creation_filetime_100ns'])
    phases.close()
    trace = phases.snapshot()
    association = memory_phases.classify_samples(report, trace, owner='probe')
    reasons = [row['reason'] for row in association['assignments']]
    expected = ['crosses_phase_boundary']+['within_phase']*3+['crosses_phase_boundary']+['within_phase']*2
    passed = (before == pins() and report['completed'] and report['stop_reason'] == 'sample_limit'
        and report['sample_count'] == 7 and 'error' not in report and 'close_error' not in report
        and trace['completed'] and association['supported'] and reasons == expected)
    return dict(format='poima.windows-memory-timeline-smoke', version=1, passed=passed,
        scope='Actual Windows self-process counters and query/phase alignment; test phase labels only. '
              'No engine, GPU, gameplay, memory-growth or performance qualification.',
        source_sha256=before, source_unchanged=before == pins(),
        memory_report=report, phase_report=trace, association=association)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink() or not args.output.parent.is_dir():
        parser.error('Output must be a new file in an existing directory')
    report = run()
    text = json.dumps(report, indent=2, allow_nan=False)+'\n'
    if len(text.encode('utf-8')) > 65536:
        raise RuntimeError('Probe report exceeds 64 KiB')
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    print(json.dumps(dict(passed=report['passed'], samples=report['memory_report']['sample_count'])))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
