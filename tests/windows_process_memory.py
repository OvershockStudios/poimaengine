#!/usr/bin/env python3
"""Provider-free sampler contracts; no child processes or Windows API calls."""
# SPDX-License-Identifier: Apache-2.0
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1]/'tools/performance/windows_process_memory.py'
SPEC = importlib.util.spec_from_file_location('poima_windows_process_memory', PATH)
sampler = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sampler
SPEC.loader.exec_module(sampler)


class Clock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        if duration <= 0:
            raise AssertionError('Busy wait or invalid sleep.')
        self.sleeps.append(duration)
        self.now += duration


class Provider:
    creation_filetime = 134000000000000001

    def __init__(self, rows, close_error=None):
        self.rows = iter(rows)
        self.calls = 0
        self.closed = 0
        self.close_error = close_error

    def sample(self):
        self.calls += 1
        value = next(self.rows)
        if isinstance(value, BaseException):
            raise value
        return dict(creation_filetime=self.creation_filetime, **value)

    def close(self):
        self.closed += 1
        if self.close_error:
            raise self.close_error


def counters(working=100, private=200, **extra):
    return dict(working_set_bytes=working, private_commit_bytes=private, **extra)


class Contracts(unittest.TestCase):
    def collect(self, rows, *, options=None, close_error=None, clock=None):
        provider = Provider(rows, close_error)
        clock = clock or Clock()
        report = sampler.collect(options or sampler.Options(42, duration=.4, interval=.2),
            provider_factory=lambda _: provider, monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(provider.closed, 1)
        return report, provider, clock

    def test_pure_summary_empty_and_even_median(self):
        self.assertEqual(sampler.summarize([]), {})
        value = sampler.summarize([counters(10, 200), counters(13, 400)])
        self.assertEqual(value['working_set_bytes']['median'], 11.5)
        self.assertEqual(value['private_commit_bytes']['median'], 300)
        with self.assertRaises(ValueError):
            sampler.summarize([counters(private=-1)])

    def test_summary_distinguishes_resident_and_commit(self):
        report, provider, clock = self.collect([counters(100, 500), counters(300, 100), counters(200, 900)])
        self.assertTrue(report['completed'])
        self.assertEqual(report['stop_reason'], 'duration_complete')
        self.assertEqual(provider.calls, 3)
        self.assertEqual([row['elapsed_seconds'] for row in report['samples']], [0, .2, .4])
        self.assertEqual(report['process'], dict(pid=42, creation_filetime_100ns=str(provider.creation_filetime)))
        self.assertEqual(report['summary']['working_set_bytes'],
                         dict(samples=3, minimum=100, maximum=300, first=100, last=200, median=200))
        self.assertEqual(report['summary']['private_commit_bytes']['maximum'], 900)
        self.assertFalse(report['gpu']['qualified'])
        self.assertEqual(clock.sleeps, [.2, .2])

    def test_count_bounds_and_short_duration(self):
        report, provider, _ = self.collect([counters(0, 0)]*2,
            options=sampler.Options(42, duration=100, max_samples=2))
        self.assertEqual(report['stop_reason'], 'sample_limit')
        self.assertEqual(report['sample_count'], 2)
        self.assertTrue(report['completed'])
        self.assertEqual(report['summary']['working_set_bytes']['maximum'], 0)
        report, provider, clock = self.collect([counters()], options=sampler.Options(42, duration=.01))
        self.assertEqual(provider.calls, 1)
        self.assertEqual(clock.sleeps, [])
        self.assertTrue(report['completed'])

    def test_process_exit_retains_only_successful_samples(self):
        report, _, _ = self.collect([counters(11, 22), sampler.ProcessEnded()])
        self.assertFalse(report['completed'])
        self.assertEqual(report['stop_reason'], 'process_ended')
        self.assertEqual(report['sample_count'], 1)
        self.assertNotIn('error', report)
        report, _, _ = self.collect([sampler.ProcessEnded()])
        self.assertEqual(report['samples'], [])
        self.assertEqual(report['summary'], {})

    def test_api_failure_retains_partial_and_numeric_error(self):
        report, _, _ = self.collect([counters(11, 22), sampler.ProbeError('GetProcessMemoryInfo', 5)])
        self.assertFalse(report['completed'])
        self.assertEqual(report['sample_count'], 1)
        self.assertEqual(report['error'], dict(operation='GetProcessMemoryInfo', win32_error=5))

    def test_identity_reuse_never_admits_replacement_sample(self):
        class Reused(Provider):
            def sample(self):
                value = super().sample()
                if self.calls > 1:
                    value['creation_filetime'] += 1
                return value
        provider, clock = Reused([counters(11, 22), counters(999, 999)]), Clock()
        report = sampler.collect(sampler.Options(42, duration=.4), provider_factory=lambda _: provider,
                                  monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(provider.closed, 1)
        self.assertEqual(report['sample_count'], 1)
        self.assertEqual(report['error']['operation'], 'process_identity_changed')
        self.assertEqual(report['summary']['working_set_bytes']['maximum'], 11)

    def test_expected_identity_mismatch_closes_without_sampling(self):
        report, provider, _ = self.collect([], options=sampler.Options(42, expected_creation_filetime=7))
        self.assertEqual(provider.calls, 0)
        self.assertEqual(report['error']['operation'], 'process_identity_changed')

    def test_close_failure_preserves_data_and_prevents_completed(self):
        report, _, _ = self.collect([counters()], options=sampler.Options(42, max_samples=1),
                                    close_error=sampler.ProbeError('CloseHandle', 6))
        self.assertFalse(report['completed'])
        self.assertEqual(report['close_error'], dict(operation='CloseHandle', win32_error=6))
        self.assertEqual(report['sample_count'], 1)

    def test_system_ram_is_optional_and_separate(self):
        report, _, _ = self.collect([counters(system_total_physical_bytes=10000,
                                             system_available_physical_bytes=3000)],
                                    options=sampler.Options(42, max_samples=1, system_ram=True))
        self.assertTrue(report['completed'])
        self.assertEqual(report['summary']['system_available_physical_bytes']['first'], 3000)
        report, _, _ = self.collect([counters(system_total_physical_bytes=1,
                                             system_available_physical_bytes=2)],
                                    options=sampler.Options(42, system_ram=True))
        self.assertEqual(report['error']['operation'], 'invalid_system_ram')
        self.assertEqual(report['sample_count'], 0)

    def test_invalid_counter_or_extra_field_never_enters_report(self):
        for value in (counters(-1), counters(True), counters(1.5), counters(2**64),
                      dict(private_commit_bytes=5), counters(process_name='private-user-path')):
            with self.subTest(value=value):
                report, _, _ = self.collect([counters(), value])
                self.assertFalse(report['completed'])
                self.assertEqual(report['sample_count'], 1)
                self.assertNotIn('private-user-path', json.dumps(report))

    def test_unexpected_exception_does_not_copy_private_text(self):
        report, _, _ = self.collect([ValueError('private-user-path')])
        self.assertEqual(report['error'], dict(operation='sampler_exception', exception_type='ValueError'))
        self.assertNotIn('private-user-path', json.dumps(report))

    def test_interrupt_retains_data_and_closes(self):
        report, _, _ = self.collect([counters(), KeyboardInterrupt()])
        self.assertEqual(report['stop_reason'], 'interrupted')
        self.assertEqual(report['sample_count'], 1)
        self.assertFalse(report['completed'])

    def test_slow_probe_does_not_catch_up_in_bursts(self):
        clock = Clock()
        class Slow(Provider):
            def sample(self):
                value = super().sample()
                clock.now += .35
                return value
        provider = Slow([counters()]*3)
        report = sampler.collect(sampler.Options(42, duration=1, max_samples=3),
            provider_factory=lambda _: provider, monotonic=clock.monotonic, sleep=clock.sleep)
        times = [x['elapsed_seconds'] for x in report['samples']]
        self.assertEqual(len(times), 3)
        self.assertTrue(all(b-a >= .2 for a, b in zip(times, times[1:])))
        self.assertEqual(clock.sleeps, [])
        self.assertEqual(provider.closed, 1)

    def test_invalid_clock_retains_data_and_closes(self):
        values = iter([0.0, 0.0, -.1])
        provider = Provider([counters()])
        report = sampler.collect(sampler.Options(42), provider_factory=lambda _: provider,
                                  monotonic=lambda: next(values), sleep=lambda _: None)
        self.assertEqual(report['sample_count'], 1)
        self.assertEqual(report['error']['operation'], 'invalid_monotonic_clock')
        self.assertEqual(provider.closed, 1)
        opened = []
        report = sampler.collect(sampler.Options(42), provider_factory=lambda _: opened.append(True),
                                  monotonic=lambda: float('nan'))
        self.assertEqual(opened, [])
        self.assertEqual(report['error']['operation'], 'invalid_monotonic_clock')

    def test_validation_before_provider_open(self):
        for kwargs in (
            dict(pid=0), dict(pid=-1), dict(pid=True), dict(pid=2**32),
            dict(duration=0), dict(duration=-1), dict(duration=3601), dict(duration=float('nan')),
            dict(duration=float('inf')), dict(interval=0), dict(interval=.199), dict(interval=3601),
            dict(interval=float('nan')), dict(max_samples=0), dict(max_samples=18002), dict(max_samples=True),
            dict(expected_creation_filetime=0), dict(expected_creation_filetime=2**64), dict(system_ram=1)):
            with self.subTest(kwargs=kwargs):
                opened = []
                args = dict(pid=42); args.update(kwargs)
                with self.assertRaises(ValueError):
                    sampler.collect(sampler.Options(**args), provider_factory=lambda _: opened.append(True))
                self.assertEqual(opened, [])
        sampler.Options(0xffffffff, duration=3600, interval=.2, max_samples=18001).validate()

    def test_open_error_has_no_handle_or_samples(self):
        def denied(_):
            raise sampler.ProbeError('OpenProcess', 5)
        report = sampler.collect(sampler.Options(42), provider_factory=denied)
        self.assertEqual(report['error'], dict(operation='OpenProcess', win32_error=5))
        self.assertEqual(report['sample_count'], 0)
        self.assertIsNone(report['process']['creation_filetime_100ns'])

    def test_non_windows_rejection_without_loading_windows_dll(self):
        with patch.object(sampler, 'native_windows', return_value=False):
            with self.assertRaises(sampler.ProbeError) as caught:
                sampler.WindowsProvider(sampler.Options(42))
            self.assertEqual(caught.exception.operation, 'native_windows_required')
            report = sampler.collect(sampler.Options(42))
        self.assertEqual(report['error']['operation'], 'native_windows_required')
        self.assertEqual(report['samples'], [])

    def test_portable_help_and_cli_failure_record(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                sampler.main(['--help'])
            self.assertEqual(caught.exception.code, 0)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'memory.json'
            with patch.object(sampler, 'native_windows', return_value=False), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(sampler.main(['--pid', '42', '--duration', '1', '--output', str(output)]), 1)
            report = json.loads(output.read_text())
            self.assertEqual(report['sample_count'], 0)
            self.assertEqual(report['error']['operation'], 'native_windows_required')
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    sampler.main(['--pid', '42', '--output', str(output)])
            self.assertEqual(caught.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
