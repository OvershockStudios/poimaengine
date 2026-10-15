"""Bounded phase/query association in one collector's monotonic-nanosecond clock.

This describes observation intervals, not allocation causes or memory growth.
Legacy unanchored samples are never assigned retrospective timestamps.
"""
# SPDX-License-Identifier: Apache-2.0
import bisect
import copy
import json
import os
import re
import time

DOMAIN = 'python.time.monotonic_ns'
MAX_EVENTS = 4096
MAX_BYTES = 1048576
U64 = (1 << 64)-1
PHASES = frozenset(('startup','warmup','play','pause','snapshot','save','save_retry',
    'discarded_play','load','load_retry','restore_check','frame_paging','capture','cleanup'))
TAGS = ('owner','pid','creation_filetime_100ns','player_id','generation','session_id',
        'tick','runtime_replacements','cycle')


def require(ok, operation):
    if not ok:
        raise ValueError(operation)


def uint(value, maximum=U64):
    return type(value) is int and 0 <= value <= maximum


def encoded(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode('utf-8')


def creation(value):
    return (type(value) is str and re.fullmatch(r'[1-9][0-9]{0,19}',value) is not None
            and int(value) <= U64)


def valid_clock(value):
    return (type(value) is dict and set(value) == {'domain','collector_pid','origin_ns'}
            and value['domain'] == DOMAIN and uint(value['collector_pid'],0xffffffff)
            and value['collector_pid'] > 0 and uint(value['origin_ns']))


def validate_event(event, sequence, previous, origin):
    require(type(event) is dict and set(event) == {'sequence','at_ns','phase',*TAGS}, 'invalid_phase_fields')
    require(type(event['sequence']) is int and event['sequence'] == sequence, 'invalid_phase_sequence')
    require(uint(event['at_ns']) and event['at_ns'] >= max(previous,origin), 'invalid_phase_clock')
    require(type(event['phase']) is str and event['phase'] in PHASES, 'invalid_phase')
    require(type(event['owner']) is str and re.fullmatch(r'[a-z][a-z0-9_-]{0,63}',event['owner']) is not None, 'invalid_owner')
    require(event['pid'] is None or (uint(event['pid'],0xffffffff) and event['pid'] > 0), 'invalid_pid')
    require(event['creation_filetime_100ns'] is None or creation(event['creation_filetime_100ns']), 'invalid_creation_filetime')
    for key in ('player_id','session_id'):
        value = event[key]
        require(value is None or (type(value) is str and re.fullmatch('[0-9a-f]{32}',value) is not None), 'invalid_'+key)
    for key in ('generation','tick','runtime_replacements','cycle'):
        require(event[key] is None or uint(event[key]), 'invalid_'+key)


class PhaseLog:
    """One writer; errors retain valid events and make the trace incomplete.

    Byte bounds use canonical compact JSON, including a reserved metadata envelope.
    Repeated timestamps are retained; associations touching a tied boundary are
    deliberately ambiguous. Nothing advances or repairs a caller's clock.
    """
    def __init__(self, origin_ns, *, clock_ns=time.monotonic_ns,
                 max_events=MAX_EVENTS, max_bytes=MAX_BYTES):
        require(uint(origin_ns), 'invalid_clock_origin')
        require(type(max_events) is int and 1 <= max_events <= MAX_EVENTS, 'invalid_event_limit')
        require(type(max_bytes) is int and 1024 <= max_bytes <= MAX_BYTES, 'invalid_byte_limit')
        require(callable(clock_ns), 'invalid_clock_provider')
        self._clock = dict(domain=DOMAIN,collector_pid=os.getpid(),origin_ns=origin_ns)
        self._now, self._events = clock_ns, []
        self._max_events, self._max_bytes = max_events, max_bytes
        self._event_bytes, self._closed, self._error = 0, None, None
        self._rejected = 0

    @property
    def clock(self):
        return dict(self._clock)

    def _fail(self, operation):
        if self._error is None:
            self._error = dict(operation=operation)
            self._rejected = 1
        raise ValueError(operation)

    def _read_clock(self):
        try:
            value = self._now()
        except Exception:
            self._fail('clock_read_failed')
        previous = self._events[-1]['at_ns'] if self._events else self._clock['origin_ns']
        if not uint(value) or value < previous:
            self._fail('invalid_phase_clock')
        return value

    def mark(self, phase, *, owner, pid=None, creation_filetime_100ns=None,
             player_id=None, generation=None, session_id=None, tick=None,
             runtime_replacements=None, cycle=None):
        if self._closed is not None:
            self._fail('phase_log_closed')
        if self._error is not None:
            raise ValueError('phase_log_failed')
        at = self._read_clock()
        event = dict(sequence=len(self._events)+1,at_ns=at,phase=phase,owner=owner,
            pid=pid,creation_filetime_100ns=creation_filetime_100ns,player_id=player_id,
            generation=generation,session_id=session_id,tick=tick,
            runtime_replacements=runtime_replacements,cycle=cycle)
        try:
            validate_event(event,len(self._events)+1,
                self._events[-1]['at_ns'] if self._events else self._clock['origin_ns'],self._clock['origin_ns'])
        except ValueError as error:
            self._fail(str(error))
        size = len(encoded(event))+1
        if len(self._events) >= self._max_events:
            self._fail('event_limit')
        # The remaining fixed metadata/error/clock/limits envelope is <768B.
        if self._event_bytes+size+768 > self._max_bytes:
            self._fail('byte_limit')
        self._events.append(event)
        self._event_bytes += size
        return dict(event)

    def close(self):
        if self._closed is None:
            self._closed = self._read_clock()

    def snapshot(self):
        report = dict(format='poima.memory-phases',version=1,clock=self.clock,
            completed=self._closed is not None and self._error is None,
            closed_ns=self._closed,event_count=len(self._events),events=copy.deepcopy(self._events),
            limits=dict(maximum_events=self._max_events,maximum_bytes=self._max_bytes,
                byte_encoding='canonical compact UTF-8 JSON'),
            rejected_events=self._rejected,error=copy.deepcopy(self._error))
        require(len(encoded(report)) <= self._max_bytes, 'phase_metadata_budget')
        return report


def classify_samples(memory_report, phase_report, *, owner):
    """Only fully contained closed queries with observed process identity align.

    Boundary overlaps remain unassigned. This routine does not mutate records,
    estimate growth, guess missing epochs or use legacy elapsed-seconds values.
    """
    require(type(memory_report) is dict and type(memory_report.get('samples')) is list, 'invalid_memory_report')
    require(type(owner) is str and re.fullmatch(r'[a-z][a-z0-9_-]{0,63}',owner) is not None, 'invalid_owner')
    samples = memory_report['samples']
    require(len(samples) <= 18001, 'memory_sample_limit')
    result = dict(format='poima.memory-phase-association',version=1,supported=False,
        sample_count=len(samples),assigned_count=0,unassigned_count=len(samples),assignments=[])
    def unassigned(reason):
        result['assignments'] = [dict(sample=i,phase_sequence=None,phase=None,reason=reason) for i in range(len(samples))]
        return result
    require(memory_report.get('format') == 'poima.windows-process-memory' and type(memory_report.get('version')) is int, 'invalid_memory_format')
    if memory_report['version'] == 1:
        return unassigned('legacy_without_query_clock')
    require(memory_report['version'] == 2, 'unsupported_memory_version')
    require(type(phase_report) is dict and phase_report.get('format') == 'poima.memory-phases'
        and type(phase_report.get('version')) is int and phase_report['version'] == 1, 'invalid_phase_report')
    phase_clock = phase_report.get('clock')
    require(valid_clock(phase_clock), 'invalid_shared_clock')
    require(type(phase_report.get('completed')) is bool, 'invalid_phase_completion')
    rejected = phase_report.get('rejected_events')
    require(type(rejected) is int and 0 <= rejected <= 1, 'invalid_phase_rejections')
    error = phase_report.get('error')
    require(error is None or (type(error) is dict and set(error) == {'operation'}
        and type(error['operation']) is str
        and re.fullmatch('[a-z][a-z0-9_]{0,63}',error['operation']) is not None), 'invalid_phase_error')
    if phase_report['completed']:
        require(error is None and rejected == 0, 'invalid_complete_trace')
    events = phase_report.get('events')
    limits = phase_report.get('limits')
    require(type(limits) is dict and set(limits) == {'maximum_events','maximum_bytes','byte_encoding'}
        and type(limits['maximum_events']) is int and 1 <= limits['maximum_events'] <= MAX_EVENTS
        and type(limits['maximum_bytes']) is int and 1024 <= limits['maximum_bytes'] <= MAX_BYTES
        and limits['byte_encoding'] == 'canonical compact UTF-8 JSON', 'invalid_phase_limits')
    require(type(events) is list and len(events) <= limits['maximum_events'] and type(phase_report.get('event_count')) is int
        and phase_report['event_count'] == len(events), 'invalid_phase_count')
    require(len(encoded(phase_report)) <= limits['maximum_bytes'], 'phase_byte_limit')
    previous = phase_clock['origin_ns']
    for index,event in enumerate(events,1):
        validate_event(event,index,previous,phase_clock['origin_ns'])
        previous = event['at_ns']
    horizon = phase_report.get('closed_ns')
    require((horizon is None and not phase_report['completed'])
        or (uint(horizon) and horizon >= previous), 'invalid_closed_horizon')
    process = memory_report.get('process')
    require(type(process) is dict and uint(process.get('pid'),0xffffffff) and process['pid'] > 0
        and (creation(process.get('creation_filetime_100ns'))
            or (not samples and process.get('creation_filetime_100ns') is None
                and memory_report.get('stop_reason') == 'error')), 'invalid_memory_identity')
    clock = memory_report.get('clock')
    if (type(clock) is dict and set(clock) == {'domain','collector_pid','origin_ns'}
            and clock['domain'] == DOMAIN and uint(clock['collector_pid'],0xffffffff)
            and clock['collector_pid'] > 0 and clock['origin_ns'] is None
            and not samples and memory_report.get('stop_reason') == 'error'
            and memory_report.get('sampler_started_ns') is None):
        return unassigned('query_clock_unavailable')
    require(valid_clock(clock), 'invalid_shared_clock')
    origin = clock['origin_ns']
    started = memory_report.get('sampler_started_ns')
    require((uint(started) and started >= origin)
        or (started is None and not samples and memory_report.get('stop_reason') == 'error'),
        'invalid_sampler_start')
    previous = origin if started is None else started
    for sample in samples:
        require(type(sample) is dict, 'invalid_memory_sample')
        begin,end = sample.get('query_begin_ns'),sample.get('query_end_ns')
        require(uint(begin) and uint(end) and origin <= begin <= end and begin >= previous, 'invalid_query_interval')
        previous = end
        for name in ('working_set_bytes','private_commit_bytes'):
            require(uint(sample.get(name)), 'invalid_memory_counter')
    if clock != phase_clock:
        return unassigned('clock_mismatch')
    if not phase_report['completed']:
        return unassigned('phase_trace_incomplete')
    if process.get('creation_filetime_100ns') is None:
        return unassigned('process_identity_unavailable')
    times = [event['at_ns'] for event in events]
    ties = {value for i,value in enumerate(times) if i and times[i-1] == value}
    result['supported'] = True
    for index,sample in enumerate(samples):
        begin,end = sample.get('query_begin_ns'),sample.get('query_end_ns')
        row = dict(sample=index,phase_sequence=None,phase=None,reason=None)
        position = bisect.bisect_right(times,begin)-1
        if begin in ties or end in ties:
            row['reason'] = 'ambiguous_boundary'
        elif not events or position < 0:
            row['reason'] = 'before_first_phase'
        elif begin >= horizon or end >= horizon:
            row['reason'] = 'after_trace_horizon'
        elif position+1 < len(times) and end >= times[position+1]:
            row['reason'] = 'crosses_phase_boundary'
        else:
            event = events[position]
            if event['owner'] != owner:
                row['reason'] = 'owner_mismatch'
            elif event['pid'] is None or event['creation_filetime_100ns'] is None:
                row['reason'] = 'process_identity_unavailable'
            elif event['pid'] != process['pid'] or event['creation_filetime_100ns'] != process['creation_filetime_100ns']:
                row['reason'] = 'process_identity_mismatch'
            else:
                row.update(phase_sequence=event['sequence'],phase=event['phase'],reason='within_phase')
                result['assigned_count'] += 1
        result['assignments'].append(row)
    result['unassigned_count'] -= result['assigned_count']
    return result
