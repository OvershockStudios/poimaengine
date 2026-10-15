# SPDX-License-Identifier: Apache-2.0
"""Bounded observations of existing player reports; no clocks, I/O or queries.

Heap usage is a driver allocation estimate for one process/device. These
sampled extrema are neither residency nor lifetime peaks. Separate graphics
owners and observed cache discontinuities are never summed or joined.
"""
import copy
import json
import math

MAX_OBSERVATIONS = 4096
MAX_BYTES = 16 * 1024 * 1024
_MOVING = {'reported_at_ns', 'age_at_report_ns', 'sampling_active'}


def _size(value):
    return len(json.dumps(value, ensure_ascii=True, allow_nan=False,
                          separators=(',', ':')).encode('utf-8'))


def _same(left, right):
    # Python equality equates True/1 and 1/1.0; native JSON observations don't.
    return json.dumps(left, sort_keys=True, allow_nan=False) == json.dumps(right, sort_keys=True, allow_nan=False)


def _uint(value, bits=64):
    return type(value) is int and 0 <= value < 2**bits


def _validate(value):
    """Validate the small observed telemetry shape, retaining additive fields."""
    if not isinstance(value, dict):
        return 'not_an_object'
    if not {'source', 'scope', 'status', 'issue', 'extension_supported', 'available',
            'sampling_active', 'device', 'minimum_interval_ns', 'sample_sequence',
            'sampled_at_ns', 'reported_at_ns', 'age_at_report_ns', 'query_cpu_ns',
            'heaps', 'peak_qualified', 'residency_qualified'} <= value.keys():
        return 'missing_field'
    if value.get('source') != 'VK_EXT_memory_budget' or value.get('scope') != 'current_process_on_selected_physical_device':
        return 'source_or_scope'
    status = value.get('status')
    if status not in ('uninitialized', 'awaiting_sample', 'unsupported', 'sampled', 'invalid'):
        return 'status'
    if value.get('issue') not in ('none', 'heap_count', 'heap_identity', 'budget_zero', 'budget_above_size'):
        return 'issue'
    if value.get('extension_supported') is not None and type(value.get('extension_supported')) is not bool:
        return 'extension_type'
    if type(value.get('available')) is not bool or value['available'] != (status == 'sampled'):
        return 'availability'
    if type(value.get('sampling_active')) is not bool or value.get('peak_qualified') is not False or value.get('residency_qualified') is not False:
        return 'qualification_or_activity'
    if value.get('minimum_interval_ns') != 500_000_000 or not _uint(value.get('minimum_interval_ns')) or not _uint(value.get('sample_sequence')):
        return 'cadence_or_sequence'
    for name in ('sampled_at_ns', 'reported_at_ns', 'age_at_report_ns', 'query_cpu_ns'):
        if name not in value or value[name] is not None and not _uint(value[name]):
            return 'time_type'
    if value['age_at_report_ns'] is not None:
        if value['sampled_at_ns'] is None or value['reported_at_ns'] is None or value['reported_at_ns'] - value['sampled_at_ns'] != value['age_at_report_ns']:
            return 'sample_age'
    device = value.get('device')
    if device is not None:
        if not isinstance(device, dict) or any(not _uint(device.get(k), 32) for k in ('vendor_id', 'device_id', 'api_version', 'device_type')):
            return 'device'
        uuid = device.get('uuid')
        if not isinstance(uuid, str) or len(uuid) != 32 or any(c not in '0123456789abcdef' for c in uuid):
            return 'device_uuid'
    heaps = value.get('heaps')
    if not isinstance(heaps, list) or len(heaps) > 16:
        return 'heap_count'
    for index, heap in enumerate(heaps):
        if not isinstance(heap, dict) or not _uint(heap.get('index')) or heap['index'] != index or not _uint(heap.get('flags'), 32) or not _uint(heap.get('size_bytes')):
            return 'heap_identity'
        if type(heap.get('device_local')) is not bool or heap['device_local'] != bool(heap['flags'] & 1):
            return 'heap_flags'
        for name in ('allocated_estimate_bytes', 'budget_bytes'):
            if name not in heap or (not _uint(heap[name]) if status == 'sampled' else heap[name] is not None):
                return 'heap_estimate'
        if status == 'sampled' and not 0 < heap['budget_bytes'] <= heap['size_bytes']:
            return 'heap_budget'
    if status == 'sampled' and (device is None or not heaps or value['extension_supported'] is not True or value['sampled_at_ns'] is None or value['query_cpu_ns'] is None or value['sample_sequence'] == 0 or value['issue'] != 'none'):
        return 'sampled_identity'
    if status == 'unsupported' and value['extension_supported'] is not False:
        return 'unsupported_capability'
    if status == 'invalid' and value['issue'] == 'none':
        return 'invalid_issue'
    if status != 'invalid' and value['issue'] != 'none':
        return 'unexpected_issue'
    return None


class Collector:
    """Trusted in-memory JSON input only. Diagnostics never change game grading.

    Each retained record has first/last observation tags and a report count.
    Session transitions/terminal observations retain separate records even if
    the native cache sample is the same. Errors and omissions are explicit.
    max_bytes bounds compact ASCII JSON for snapshot(), not caller indentation
    or transient Python RAM. snapshot() copies retained data; call it outside
    measured play. pid/time may explicitly be None for offline inputs, which
    cannot establish complete live process/time identity.
    """
    def __init__(self, max_observations=MAX_OBSERVATIONS, max_bytes=MAX_BYTES):
        if type(max_observations) is not int or not 1 <= max_observations <= MAX_OBSERVATIONS:
            raise ValueError('Observation capacity outside 1..4096')
        if type(max_bytes) is not int or not 4096 <= max_bytes <= MAX_BYTES:
            raise ValueError('Byte capacity outside 4096..16777216')
        self.max_observations, self.max_bytes = max_observations, max_bytes
        self.records, self.contexts = [], []
        self.observed = self.omitted = self.errors = self.unavailable = 0
        self.omission_reasons = dict(record_capacity=0, byte_capacity=0, invalid_input=0)
        self.error_counts = {}
        self._bytes = 0
        self._last = {}

    def observe(self, owner, state, observed_seconds):
        # No callbacks, OS calls or native requests occur here. Input is a
        # trusted decoded JSON reply; bad telemetry is retained, never raised.
        self.observed += 1
        try:
            self._observe(owner, state, observed_seconds)
        except (ValueError, TypeError, KeyError, OverflowError, RecursionError):
            # JSON/nonfinite/shape failure cannot replace an existing workload
            # exception. Bounded counters retain failure even without a row.
            self._error('invalid_input')
            self.omitted += 1
            self.omission_reasons['invalid_input'] += 1

    def _error(self, issue):
        self.errors += 1
        self.error_counts[issue] = self.error_counts.get(issue, 0) + 1

    def _observe(self, owner, state, seconds):
        if (not isinstance(owner, dict) or
                any(not isinstance(owner.get(k), str) or not 1 <= len(owner[k]) <= 256
                    for k in ('label', 'player_id')) or 'pid' not in owner or
                (owner['pid'] is not None and not _uint(owner['pid'], 32)) or
                not _uint(owner.get('generation'))):
            raise ValueError('Owner identity')
        if seconds is not None and (type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds < 0):
            raise ValueError('Observation time')
        if not isinstance(state, dict) or state.get('player_id') != owner['player_id'] or not _uint(state.get('generation')) or state['generation'] != owner['generation']:
            raise ValueError('Player owner mismatch')
        report = state.get('report')
        render = report.get('render_diagnostics') if isinstance(report, dict) else None
        present = isinstance(render, dict) and 'memory_budget' in render
        value = copy.deepcopy(render['memory_budget']) if present else None
        issue = _validate(value) if present else None
        if issue:
            self._error(issue)
        if not present:
            self.unavailable += 1
        tag = dict(observed_seconds=seconds, process_identity_complete=owner['pid'] is not None,
                   observation_time_available=seconds is not None,
                   session_id=state.get('session_id'), tick=state.get('tick'),
                   active=state.get('active'), runtime_replacements=report.get('runtime_replacements') if isinstance(report, dict) else None)
        if isinstance(value, dict):
            tag.update({k: value.get(k) for k in _MOVING})
        owner_key = tuple(owner[k] for k in ('label', 'pid', 'player_id', 'generation'))
        prior = self._last.get(owner_key)
        cache = {k: v for k, v in value.items() if k not in _MOVING} if isinstance(value, dict) and issue is None else None
        same = prior is not None and cache is not None and prior['cache'] is not None and all(_same(cache.get(k), prior['cache'].get(k)) for k in ('device', 'sample_sequence', 'sampled_at_ns'))
        if same and not _same(cache, prior['cache']):
            issue = 'cached_sample_changed'
            self._error(issue)
        transition = None
        context = prior['context'] if prior is not None else None
        if cache is not None and issue is None:
            old = prior['cache'] if prior else None
            if prior is not None and old is None:
                transition = 'telemetry_became_available'
            elif old is not None and old['device'] != cache['device']:
                transition = 'device_changed'
            elif old is not None and not same and (cache['sample_sequence'] <= old['sample_sequence'] or
                    cache['sampled_at_ns'] is not None and old['sampled_at_ns'] is not None and cache['sampled_at_ns'] < old['sampled_at_ns']):
                transition = 'cache_discontinuity'
            elif old is not None and not same and not _same(
                    [[h['index'], h['flags'], h['size_bytes']] for h in old['heaps']],
                    [[h['index'], h['flags'], h['size_bytes']] for h in cache['heaps']]):
                transition = 'heap_identity_changed'
            if prior is None or prior['cache'] is None or transition:
                context = len(self.contexts)
        old_record = self.records[prior['record']] if prior else None
        duplicate = same and issue is None and old_record is not None and all(tag.get(k) == old_record['last_observation'].get(k) for k in ('session_id', 'active', 'sampling_active'))
        record = (copy.deepcopy(old_record) if duplicate else
                  dict(owner=copy.deepcopy(owner), context=context, memory_budget_present=present,
                       memory_budget=value, issue=issue, discontinuity=transition,
                       new_cache_sample=bool(cache is not None and not same and issue is None),
                       first_observation=tag, reports=0))
        record['last_observation'] = tag
        record['reports'] += 1
        summary = None
        new_context = context == len(self.contexts) and cache is not None and issue is None
        if cache is not None and issue is None:
            summary = (dict(owner=copy.deepcopy(owner), device=copy.deepcopy(cache['device']),
                            usable_samples=0, heaps=[]) if new_context else copy.deepcopy(self.contexts[context]))
            if not same and cache['status'] == 'sampled':
                summary['usable_samples'] += 1
                for heap in cache['heaps']:
                    if len(summary['heaps']) <= heap['index']:
                        summary['heaps'].append(dict(index=heap['index'], flags=heap['flags'], size_bytes=heap['size_bytes'], samples=0,
                            observed_min_allocated_estimate_bytes=heap['allocated_estimate_bytes'],
                            observed_max_allocated_estimate_bytes=heap['allocated_estimate_bytes'],
                            observed_min_budget_bytes=heap['budget_bytes'], observed_max_budget_bytes=heap['budget_bytes']))
                    row = summary['heaps'][heap['index']]
                    if (row['flags'], row['size_bytes']) != (heap['flags'], heap['size_bytes']):
                        raise ValueError('Heap identity changed without context discontinuity')
                    row['samples'] += 1
                    for metric, key in (('allocated_estimate', 'allocated_estimate_bytes'), ('budget', 'budget_bytes')):
                        row['observed_min_'+metric+'_bytes'] = min(row['observed_min_'+metric+'_bytes'], heap[key])
                        row['observed_max_'+metric+'_bytes'] = max(row['observed_max_'+metric+'_bytes'], heap[key])
        delta = _size(record) - (_size(old_record) if duplicate else -1)
        if summary is not None:
            delta += _size(summary) - (_size(self.contexts[context]) if not new_context else -1)
        # Reserve fixed report/counter metadata, including growing counters.
        if (not duplicate and len(self.records) >= self.max_observations) or self._bytes + delta > self.max_bytes - 2048:
            self.omitted += 1
            reason = 'record_capacity' if not duplicate and len(self.records) >= self.max_observations else 'byte_capacity'
            self.omission_reasons[reason] += 1
            return
        self._bytes += delta
        if duplicate:
            self.records[prior['record']] = record
            index = prior['record']
        else:
            index = len(self.records)
            self.records.append(record)
        if summary is not None:
            if new_context:
                self.contexts.append(summary)
            else:
                self.contexts[context] = summary
        # Invalid data never replaces the preceding trusted cache authority.
        if issue is None:
            self._last[owner_key] = dict(cache=cache, context=context, record=index)

    def snapshot(self):
        return copy.deepcopy(dict(format='poima.gpu-memory-observations.v1',
            complete=self.omitted == 0, contract_ok=self.errors == 0,
            observations=self.observed, omitted=self.omitted, errors=self.errors,
            omission_reasons=self.omission_reasons, error_counts=self.error_counts,
            legacy_unavailable=self.unavailable, maximum_records=self.max_observations,
            maximum_bytes=self.max_bytes, records=self.records, contexts=self.contexts,
            peak_qualified=False, residency_qualified=False,
            scope='Observed per-process/device heap allocation estimates; separate contexts are not summed.'))
