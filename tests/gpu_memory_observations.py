#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure recorder tests. No Windows calls, threads, child processes or GPU."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('poima_heap_observations', ROOT/'tools/performance/gpu_memory_observations.py')
heap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(heap)
# Soak imports the public helper by its normal module name. unittest discovery
# can itself import this test with that basename; bind the intended helper.
sys.modules['gpu_memory_observations'] = heap
sys.path.insert(0, str(ROOT/'tests'))
import performance_yard_soak as soak


def owner(**changes):
    return dict(dict(label='main', pid=123, player_id='a'*32, generation=1), **changes)


def budget(sequence=1, stamp=0, usage=0):
    return dict(source='VK_EXT_memory_budget', scope='current_process_on_selected_physical_device',
        status='sampled', issue='none', extension_supported=True, available=True, sampling_active=True,
        device=dict(vendor_id=1, device_id=2, api_version=3, device_type=2, uuid='b'*32),
        minimum_interval_ns=500_000_000, sample_sequence=sequence, sampled_at_ns=stamp,
        reported_at_ns=stamp+10, age_at_report_ns=10, query_cpu_ns=2,
        heaps=[dict(index=0, flags=1, device_local=True, size_bytes=1024,
                    allocated_estimate_bytes=usage, budget_bytes=512)],
        peak_qualified=False, residency_qualified=False)


def state(value, **changes):
    return dict(dict(active=True, ready=True, player_id='a'*32, generation=1,
        session_id='c'*32, tick=1, report=dict(hardware=True, nvrhi_errors=0,
        runtime_replacements=0, render_diagnostics=dict(memory_budget=value))), **changes)


class CollectorContract(unittest.TestCase):
    def add(self, collector, value=None, **kwargs):
        collector.observe(owner(), state(budget() if value is None else value, **kwargs), 0)

    def test_valid_zero_and_lossless_estimate_above_heap_budget(self):
        c = heap.Collector(); self.add(c)
        self.add(c, budget(3, 1_000_000_000, 2**64-1))
        result = json.loads(json.dumps(c.snapshot()))
        row = result['contexts'][0]['heaps'][0]
        self.assertEqual(row['observed_min_allocated_estimate_bytes'], 0)
        self.assertEqual(row['observed_max_allocated_estimate_bytes'], 2**64-1)
        self.assertEqual(row['samples'], 2)
        self.assertTrue(result['contract_ok'])

    def test_missing_is_distinct_from_explicit_null(self):
        c = heap.Collector(); missing = state(None)
        del missing['report']['render_diagnostics']['memory_budget']
        c.observe(owner(), missing, 0); c.observe(owner(), state(None), 1)
        r = c.snapshot()
        self.assertEqual(r['legacy_unavailable'], 1)
        self.assertEqual([x['memory_budget_present'] for x in r['records']], [False, True])
        self.assertEqual(r['errors'], 1)
        self.assertEqual(r['records'][1]['issue'], 'not_an_object')

    def test_unavailable_then_initialized(self):
        c = heap.Collector(); s = state(None); del s['report']['render_diagnostics']['memory_budget']
        c.observe(owner(), s, 0); self.add(c)
        self.assertEqual(c.snapshot()['contexts'][0]['usable_samples'], 1)
        self.assertEqual(c.snapshot()['records'][1]['discontinuity'], 'telemetry_became_available')

    def test_unsupported_and_invalid_are_not_zero_samples(self):
        c = heap.Collector(); b = budget()
        b.update(status='unsupported', available=False, extension_supported=False, sampling_active=False,
                 sample_sequence=0, sampled_at_ns=None, age_at_report_ns=None, query_cpu_ns=None)
        for h in b['heaps']: h.update(allocated_estimate_bytes=None, budget_bytes=None)
        self.add(c, b)
        b = copy.deepcopy(b); b.update(status='invalid', issue='budget_zero', extension_supported=True,
            sample_sequence=1, sampled_at_ns=20, reported_at_ns=25, age_at_report_ns=5, query_cpu_ns=1)
        self.add(c, b)
        r = c.snapshot()
        self.assertTrue(r['contract_ok'])
        self.assertEqual(r['contexts'][0]['usable_samples'], 0)
        self.assertEqual(r['records'][1]['memory_budget']['status'], 'invalid')

    def test_uninitialized_shape(self):
        b = budget(); b.update(status='uninitialized', extension_supported=None, available=False,
            sampling_active=False, device=None, sample_sequence=0, sampled_at_ns=None,
            reported_at_ns=None, age_at_report_ns=None, query_cpu_ns=None, heaps=[])
        c = heap.Collector(); self.add(c, b); self.assertTrue(c.snapshot()['contract_ok'])

    def test_cache_dedup_preserves_first_last_age_and_count(self):
        c = heap.Collector(); self.add(c)
        b = budget(); b.update(reported_at_ns=30, age_at_report_ns=30)
        c.observe(owner(), state(b), 2)
        r = c.snapshot(); self.assertEqual(len(r['records']), 1)
        self.assertEqual(r['records'][0]['reports'], 2)
        self.assertEqual(r['records'][0]['memory_budget']['reported_at_ns'], 10)
        self.assertEqual(r['records'][0]['last_observation']['reported_at_ns'], 30)
        self.assertEqual(r['contexts'][0]['usable_samples'], 1)

    def test_same_cache_estimate_mutation_is_retained_failure(self):
        c = heap.Collector(); self.add(c); self.add(c, budget(usage=1))
        r = c.snapshot(); self.assertFalse(r['contract_ok'])
        self.assertEqual(r['records'][1]['issue'], 'cached_sample_changed')
        self.assertEqual(r['records'][1]['memory_budget']['heaps'][0]['allocated_estimate_bytes'], 1)
        self.assertEqual(r['contexts'][0]['usable_samples'], 1)

    def test_additive_bool_int_cached_mutation_is_not_equal(self):
        c = heap.Collector(); b = budget(); b['future'] = {'count': True}; self.add(c, b)
        b['future']['count'] = 1; self.add(c, b)
        self.assertEqual(c.snapshot()['errors'], 1)

    def test_session_change_annotated_without_new_context_or_sample(self):
        c = heap.Collector(); self.add(c); self.add(c, session_id='d'*32)
        r = c.snapshot(); self.assertEqual(len(r['records']), 2)
        self.assertEqual(len(r['contexts']), 1)
        self.assertEqual(r['contexts'][0]['usable_samples'], 1)
        self.assertFalse(r['records'][1]['new_cache_sample'])

    def test_terminal_is_retained_without_new_sample(self):
        c = heap.Collector(); self.add(c); b = budget(); b['sampling_active'] = False
        self.add(c, b, active=False)
        r = c.snapshot(); self.assertEqual(len(r['records']), 2)
        self.assertFalse(r['records'][-1]['last_observation']['active'])
        self.assertEqual(r['contexts'][0]['usable_samples'], 1)

    def test_separate_owner_or_device_are_not_summed(self):
        c = heap.Collector(); self.add(c)
        c.observe(owner(pid=124), state(budget(usage=100)), 1)
        b = budget(usage=200); b['device']['uuid'] = 'e'*32; self.add(c, b)
        r = c.snapshot(); self.assertEqual(len(r['contexts']), 3)
        self.assertEqual([x['heaps'][0]['observed_max_allocated_estimate_bytes'] for x in r['contexts']], [0,100,200])
        self.assertEqual(r['records'][-1]['discontinuity'], 'device_changed')

    def test_same_pid_new_generation_is_a_separate_owner(self):
        c = heap.Collector(); self.add(c)
        c.observe(owner(generation=2), state(budget(), generation=2), 1)
        self.assertEqual(len(c.snapshot()['contexts']), 2)
        self.assertEqual([x['usable_samples'] for x in c.snapshot()['contexts']], [1,1])

    def test_sequence_reset_and_backwards_sample_time_split_contexts(self):
        c = heap.Collector(); self.add(c, budget(9, 100)); self.add(c, budget(1, 101))
        self.add(c, budget(2, 90))
        r = c.snapshot(); self.assertEqual(len(r['contexts']), 3)
        self.assertEqual([x['discontinuity'] for x in r['records'][1:]], ['cache_discontinuity']*2)

    def test_same_sequence_new_timestamp_is_explicit_discontinuity(self):
        c = heap.Collector(); self.add(c); self.add(c, budget(1, 30))
        self.assertEqual(c.snapshot()['records'][1]['discontinuity'], 'cache_discontinuity')

    def test_heap_identity_change_retains_raw_discontinuity(self):
        c = heap.Collector(); self.add(c)
        b = budget(2, 10); b['heaps'][0]['size_bytes'] = 2048; self.add(c, b)
        r = c.snapshot(); self.assertEqual(r['omitted'], 0)
        self.assertEqual(r['records'][1]['discontinuity'], 'heap_identity_changed')
        self.assertEqual(r['contexts'][1]['heaps'][0]['size_bytes'], 2048)

    def test_invalid_known_fields_and_boolean_integer_are_retained(self):
        for mutate in [lambda b:b.update(sample_sequence=True), lambda b:b.update(age_at_report_ns=99),
                       lambda b:b['heaps'][0].update(allocated_estimate_bytes=None),
                       lambda b:b.pop('extension_supported')]:
            c = heap.Collector(); b = budget(); mutate(b); self.add(c, b)
            self.assertFalse(c.snapshot()['contract_ok']); self.assertEqual(len(c.snapshot()['records']), 1)

    def test_offline_unknown_pid_and_clock_are_explicit(self):
        c = heap.Collector(); c.observe(owner(pid=None), state(budget()), None)
        r = c.snapshot(); self.assertTrue(r['contract_ok'])
        self.assertFalse(r['records'][0]['first_observation']['process_identity_complete'])
        self.assertFalse(r['records'][0]['first_observation']['observation_time_available'])

    def test_input_and_snapshot_mutation_are_isolated(self):
        c = heap.Collector(); b = budget(); o = owner(); c.observe(o, state(b), 0)
        o['label']='changed'; b['heaps'][0]['budget_bytes']=9
        r = c.snapshot(); r['records'][0]['memory_budget']['heaps'].clear(); r['contexts'].clear()
        again = c.snapshot(); self.assertEqual(again['records'][0]['owner']['label'], 'main')
        self.assertEqual(again['records'][0]['memory_budget']['heaps'][0]['budget_bytes'], 512)
        self.assertEqual(len(again['contexts']), 1)

    def test_record_capacity_explicit_omission_and_no_summary_invention(self):
        c = heap.Collector(max_observations=1); self.add(c); self.add(c, budget(2, 10, 90))
        r = c.snapshot(); self.assertFalse(r['complete']); self.assertTrue(r['contract_ok'])
        self.assertEqual(r['omitted'], 1); self.assertEqual(r['contexts'][0]['usable_samples'], 1)
        self.assertEqual(r['omission_reasons']['record_capacity'], 1)

    def test_byte_capacity_bounds_serialized_output_including_summary(self):
        c = heap.Collector(max_bytes=4096)
        for i in range(50): self.add(c, budget(i+1, i*500_000_000))
        r = c.snapshot(); self.assertGreater(r['omitted'], 0)
        self.assertEqual(r['omission_reasons']['byte_capacity'], r['omitted'])
        self.assertLessEqual(len(json.dumps(r, separators=(',', ':'), ensure_ascii=True).encode()), 4096)

    def test_bad_json_never_raises_or_masks_primary(self):
        c = heap.Collector(); b = budget(); b['extra'] = float('nan')
        self.add(c, b); c.observe(owner(), state(budget()), float('inf'))
        self.assertFalse(c.snapshot()['contract_ok']); self.assertEqual(c.snapshot()['omitted'], 2)
        self.assertEqual(c.snapshot()['omission_reasons']['invalid_input'], 2)

    def test_failure_and_omission_counters_remain_separate(self):
        c = heap.Collector(max_observations=1); self.add(c)
        self.add(c, budget(usage=10))
        r = c.snapshot(); self.assertFalse(r['complete']); self.assertFalse(r['contract_ok'])
        self.assertEqual(r['error_counts'], {'cached_sample_changed':1})
        self.assertEqual(r['omission_reasons']['record_capacity'], 1)

    def test_malformed_owner_is_counted_without_new_context(self):
        c = heap.Collector(); c.observe(owner(generation=True), state(budget()), 0)
        self.assertEqual(c.snapshot()['error_counts'], {'invalid_input':1})
        self.assertEqual(c.snapshot()['contexts'], [])

    def test_invalid_constructor_limits(self):
        for kwargs in [dict(max_observations=True), dict(max_observations=4097), dict(max_bytes=4095)]:
            with self.assertRaises(ValueError): heap.Collector(**kwargs)


class SoakIntegration(unittest.TestCase):
    def make(self, enabled):
        obj = object.__new__(soak.Soak)  # No ctypes/WinDLL constructor.
        obj.args=SimpleNamespace(record_gpu_memory=enabled)
        obj.record=dict(started_monotonic=0, passed=False, error='original failure')
        obj.host=SimpleNamespace(process=SimpleNamespace(pid=123, poll=lambda:None), close=Mock())
        obj.label='main'; obj.player_id='a'*32; obj.generation=1; obj.sid='c'*32
        obj.replacements=0; obj.sampler=None; obj.client=SimpleNamespace(close=Mock())
        obj.rpc=Mock(return_value=state(budget()))
        return obj

    def test_option_off_does_not_record_or_add_requests(self):
        obj=self.make(False)
        with patch.object(heap, 'Collector', side_effect=AssertionError('disabled collector')):
            value=obj.player()
        self.assertEqual(value['tick'], 1); obj.rpc.assert_called_once_with('player.inspect')
        self.assertFalse(hasattr(obj,'gpu_memory_collector'))

    def test_option_on_uses_same_response_without_added_requests_or_snapshots(self):
        obj=self.make(True)
        with patch.object(soak.time,'monotonic',return_value=1):
            with patch.object(heap.Collector,'snapshot',side_effect=AssertionError('no growing serialization')):
                obj.player(); obj.player()
        self.assertEqual(obj.rpc.call_count, 2)
        self.assertEqual(obj.gpu_memory_collector.snapshot()['records'][0]['reports'], 2)
        self.assertEqual(obj.record['error'],'original failure'); self.assertFalse(obj.record['passed'])

    def test_diagnostic_error_keeps_existing_functional_failure(self):
        obj=self.make(True); obj.rpc.return_value=state(None)
        obj.player()
        self.assertFalse(obj.gpu_memory_collector.snapshot()['contract_ok'])
        self.assertEqual(obj.record['error'],'original failure'); self.assertFalse(obj.record['passed'])

    def test_terminal_reuses_existing_close_request(self):
        obj=self.make(True); terminal=state(budget(),active=False)
        terminal['report'].update(success=True,audio=dict(enabled=True,stream_drained=True))
        terminal['report']['render_diagnostics']['memory_budget']['sampling_active']=False
        client=obj.client; host=obj.host
        obj.rpc.side_effect=[terminal,terminal,dict(active=False),dict()]
        obj.close()
        self.assertEqual([x.args[0] for x in obj.rpc.call_args_list],
                         ['player.inspect','player.inspect','runtime.status','host.shutdown'])
        self.assertEqual(len(obj.gpu_memory_collector.snapshot()['records']), 1)
        self.assertFalse(obj.gpu_memory_collector.snapshot()['records'][0]['last_observation']['active'])
        client.close.assert_called_once(); host.close.assert_called_once()

    def test_disabled_close_retains_original_requests_and_no_collector(self):
        obj=self.make(False); terminal=state(budget(),active=False)
        terminal['report'].update(success=True,audio=dict(enabled=True,stream_drained=True))
        obj.rpc.side_effect=[terminal,terminal,dict(active=False),dict()]
        with patch.object(heap,'Collector',side_effect=AssertionError('disabled collector')):
            obj.close()
        self.assertEqual([x.args[0] for x in obj.rpc.call_args_list],
                         ['player.inspect','player.inspect','runtime.status','host.shutdown'])
        self.assertFalse(hasattr(obj,'gpu_memory_collector'))


if __name__ == '__main__':
    unittest.main()
