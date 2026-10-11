#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Observe bounded diagnostics during the unchanged Performance Yard soak.

Use native Windows Python and performance_yard_soak.py's original arguments.
Both its full defaults and --rehearsal configurations remain available. No
resizing, injected input, forced stall, clock-policy change or new build occurs.
The original 512 MiB RPC bound and all game/clock/checkpoint guards remain.

Selected paused post-Load boundaries, first failure before cleanup and final
same-owner terminal data are retained in evidence.json.reliability_diagnostics.
reliability-diagnostics.json links that result and checks source integrity even
on failure. Diagnostics are observations, not a complete per-frame trace or a
causal identification of clock stalls. Run under an exact process-tree owner.
"""
import copy
import ctypes
from ctypes import wintypes as w
import json
import math
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import performance_yard_soak as soak

READ_LIMIT = 42
RESPONSE_LIMIT = 1024*1024
TOTAL_DIAGNOSTIC_BYTES = 16*1024*1024
REPORT_LIMIT = 128*1024
READ_RPC_RESERVATION = RESPONSE_LIMIT+2*REPORT_LIMIT+4096
AUDIO = {'queue_wait', 'enqueue', 'queue_observe', 'device_resume', 'device_pause',
         'clear', 'dsp', 'reset', 'gain', 'open', 'flush', 'drain_wait'}
CPU = {'owner_prepare','events','simulation_owner','scene_prepare','render','retire',
       'acquire','record','submit','present','gpu_wait','pacing_wait','report'}
SOURCE_NAME = 'tests/performance_yard_reliability_diagnostics.py'


class DiagnosticReliability(soak.Soak):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.record['source_sha256'][SOURCE_NAME] = soak.shared.sha(Path(__file__))
        n = self.args.cycles
        selected = (set(range(1,n+1)) if n <= 38 else
                    {1+(i*(n-1)+18)//37 for i in range(38)})
        self.selected = selected
        self.diagnostic_state = self.record.setdefault('reliability_diagnostics', dict(
            scope='Selected paused checkpoints, first failure and terminal native owners; not a full trace',
            read_limit=READ_LIMIT, response_limit_bytes=RESPONSE_LIMIT,
            diagnostic_byte_limit=TOTAL_DIAGNOSTIC_BYTES, original_rpc_limit=soak.MAX_RPC_BYTES,
            native_report_limit_bytes=REPORT_LIMIT, read_rpc_reservation_bytes=READ_RPC_RESERVATION,
            selected_soak_replacements=sorted(selected), observations=[], omitted=[],
            diagnostic_bytes=0, first_failure_attempted=False, errors=[]))
        self.observed_checkpoints = set()
        self.last_diagnostic = {}
        self.terminal_diagnostic = {}

    def focus(self):
        try:
            return super().focus()
        except AssertionError as error:
            if str(error) != 'Owned player failed to acquire actual foreground focus':
                raise
            original_error = str(error)
        # Activation is setup only. Do not unlock/switch desktops, inject input,
        # resize the owner or change global foreground-lock settings.
        attempt_record = dict(owner=self.label, original_error=original_error,
            setup_only=True, attempts=[], interactive_desktop_verified=False,
            tick_preserved=False, actual_foreground_acquired=False)
        self.record.setdefault('reliability_focus_acquisition', []).append(attempt_record)
        owner = super().rpc('player.inspect')
        soak.need(owner['active'] and owner['paused'] and owner['player_id'] == self.player_id and
            owner['generation'] == self.generation and owner['session_id'] == self.sid,
            'Foreground activation fallback requires the exact paused native owner')
        before_tick = owner['tick']
        u = self.u
        u.GetWindowThreadProcessId.restype = w.DWORD
        u.GetQueueStatus.argtypes = [w.UINT];u.GetQueueStatus.restype = w.DWORD
        u.AttachThreadInput.argtypes = [w.DWORD,w.DWORD,w.BOOL];u.AttachThreadInput.restype = w.BOOL
        u.ShowWindowAsync.argtypes = [w.HWND,ctypes.c_int];u.ShowWindowAsync.restype = w.BOOL
        u.OpenInputDesktop.argtypes = [w.DWORD,w.BOOL,w.DWORD];u.OpenInputDesktop.restype = w.HANDLE
        u.CloseDesktop.argtypes = [w.HANDLE];u.CloseDesktop.restype = w.BOOL
        u.GetThreadDesktop.argtypes = [w.DWORD];u.GetThreadDesktop.restype = w.HANDLE
        u.GetUserObjectInformationW.argtypes = [w.HANDLE,ctypes.c_int,ctypes.c_void_p,
            w.DWORD,ctypes.POINTER(w.DWORD)]
        u.GetUserObjectInformationW.restype = w.BOOL
        kernel = ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.GetCurrentThreadId.argtypes = [];kernel.GetCurrentThreadId.restype = w.DWORD
        current_thread = kernel.GetCurrentThreadId()
        u.GetQueueStatus(0x04ff)  # Create/query caller message queue; no event injection.

        def default_desktop(handle):
            text, needed = ctypes.create_unicode_buffer(256), w.DWORD()
            soak.need(handle and u.GetUserObjectInformationW(handle,2,text,ctypes.sizeof(text),ctypes.byref(needed)),
                'Cannot inspect interactive desktop for owned activation')
            soak.need(text.value.casefold() == 'default',
                'Interactive Default desktop unavailable; no activation attempted')

        def owned_thread():
            pid = w.DWORD()
            thread = u.GetWindowThreadProcessId(self.hwnd,ctypes.byref(pid))
            soak.need(self.host.process.poll() is None and thread and pid.value == self.host.process.pid,
                'Foreground activation HWND no longer belongs to the native owner')
            return thread

        target_thread = owned_thread()
        default_desktop(u.GetThreadDesktop(current_thread))  # Borrowed handles, not closed.
        default_desktop(u.GetThreadDesktop(target_thread))
        until = min(self.deadline,time.monotonic()+3)
        for _ in range(12):
            soak.need(time.monotonic() < until, 'Owned foreground activation setup deadline expired')
            desktop = u.OpenInputDesktop(0,False,1)  # DESKTOP_READOBJECTS only.
            soak.need(desktop, 'Input desktop inaccessible or locked; no activation attempted')
            try:
                default_desktop(desktop)
            finally:
                soak.need(u.CloseDesktop(desktop), 'Cannot close owned input-desktop query handle')
            attempt_record['interactive_desktop_verified'] = True
            owned_thread()
            item = dict(request_succeeded=False, owned_foreground=False,
                temporary_queue_attachment=False, queue_detached=True)
            attempt_record['attempts'].append(item)
            u.ShowWindowAsync(self.hwnd,9)  # Restore this owned window only.
            item['request_succeeded'] = bool(u.SetForegroundWindow(self.hwnd))
            if u.GetForegroundWindow() != self.hwnd:
                foreground_thread = u.GetWindowThreadProcessId(u.GetForegroundWindow(),None)
                if foreground_thread and foreground_thread != current_thread:
                    default_desktop(u.GetThreadDesktop(foreground_thread))
                    attached = bool(u.AttachThreadInput(current_thread,foreground_thread,True))
                    item['temporary_queue_attachment'] = attached
                    if attached:
                        item['queue_detached'] = False
                        try:
                            item['request_succeeded'] = bool(u.SetForegroundWindow(self.hwnd))
                        finally:
                            item['queue_detached'] = bool(u.AttachThreadInput(current_thread,foreground_thread,False))
                            soak.need(item['queue_detached'], 'Cannot detach temporary foreground input queue')
            time.sleep(min(.25,max(0,until-time.monotonic())))
            item['owned_foreground'] = u.GetForegroundWindow() == self.hwnd
            if item['owned_foreground']:
                break
        soak.need(u.GetForegroundWindow() == self.hwnd,
            'Owned player cannot acquire actual foreground focus; activation denied or desktop unavailable')
        attempt_record['actual_foreground_acquired'] = True
        after = super().rpc('player.inspect')
        soak.need(after['active'] and after['paused'] and after['tick'] == before_tick and
            all(after[k] == owner[k] for k in ('player_id','generation','session_id')),
            'Foreground activation changed native pause, tick or retained ownership')
        owned_thread()
        attempt_record['tick_preserved'] = True
        return super().focus()  # Original actual extent/foreground/machine validation.

    def validate(self, value, previous):
        soak.need(value['format'] == 'poima.player-diagnostics.v1' and
            value['count'] == len(value['rows']) <= 128 and value['capacity'] == 128 and
            value['slow_threshold_ns'] == 50_000_000, 'Native diagnostic bounds differ')
        soak.need(all(type(value[k]) is int and 0 <= value[k] <= 2**64-1 for k in
            ('polls','slow_polls','overwritten','invalid_samples','max_gap_ns','max_wall_ns','clock_drop_polls')),
            'Malformed diagnostic lifetime counters')
        if not value['counters_saturated']:
            soak.need(value['slow_polls'] == value['count']+value['overwritten'] <= value['polls'] and
                value['invalid_samples'] <= value['slow_polls'], 'Diagnostic lifetime conservation differs')
            soak.need(all(a['poll_sequence'] < b['poll_sequence'] for a,b in zip(value['rows'],value['rows'][1:])),
                'Native retained diagnostics are not chronological')
        for name in ('audio_cumulative','audio_since_last_poll'):
            audio = value[name]
            soak.need(set(audio['wall_ns']) == AUDIO and all(type(v) is int and v >= 0
                for v in audio['wall_ns'].values()), 'Audio timing counter declarations differ')
        cumulative, pending = value['audio_cumulative'], value['audio_since_last_poll']
        if not cumulative['counters_saturated'] and not pending['counters_saturated']:
            soak.need(all(pending['wall_ns'][k] <= cumulative['wall_ns'][k] for k in AUDIO),
                'Pending audio durations exceed cumulative observations')
        if previous is not None and not (previous['counters_saturated'] or value['counters_saturated']):
            soak.need(all(value[k] >= previous[k] for k in ('polls','slow_polls','overwritten',
                'invalid_samples','max_gap_ns','max_wall_ns','clock_drop_polls')),
                'Native lifetime counters reset at a checkpoint')
            soak.need(all(cumulative['wall_ns'][k] >= previous['audio_cumulative']['wall_ns'][k] for k in AUDIO),
                'Audio cumulative counters reset at a checkpoint')
            if previous['polls'] == value['polls']:
                soak.need(all(pending['wall_ns'][k]-previous['audio_since_last_poll']['wall_ns'][k] ==
                    cumulative['wall_ns'][k]-previous['audio_cumulative']['wall_ns'][k] >= 0 for k in AUDIO),
                    'Diagnostic read consumed pending audio durations')
        for row in value['rows']:
            current, prior, gap = row['current'], row['previous'], row['inter_poll_gap_ns']
            soak.need(type(row['issues']) is int and row['issues'] & ~31 == 0 and
                row['valid'] == (row['issues'] == 0) and type(row['poll_sequence']) is int and
                0 < row['poll_sequence'] <= value['polls'], 'Malformed diagnostic issue or sequence metadata')
            for sample in (current,prior):
                if sample is None:
                    continue
                soak.need(all(type(sample[k]) is int and 0 <= sample[k] <= 2**64-1 for k in
                    ('begin_ns','end_ns','tick_before','tick_after','flags','width','height','resize_ns',
                     'runtime_replacements_before','runtime_replacements_after')) and
                    set(sample['cpu_wall_ns']) == CPU and set(sample['audio_wall_ns']) == AUDIO and
                    all(type(v) is int and 0 <= v <= 2**64-1 for v in
                    list(sample['cpu_wall_ns'].values())+list(sample['audio_wall_ns'].values())),
                    'Malformed native current/prior timing fields')
                if not sample['clock']['observed']:
                    soak.need(all(sample['clock'][k] is None for k in ('elapsed_seconds','accepted_seconds',
                        'dropped_seconds','accumulator_seconds','planned_ticks','committed_ticks')),
                        'Unavailable clock observation was presented as measured zero')
            if row['valid']:
                soak.need(current['end_ns'] >= current['begin_ns'] and
                    current['poll_wall_ns'] == current['end_ns']-current['begin_ns'], 'Valid native wall duration differs')
                if gap is not None:
                    soak.need(prior is not None and gap == current['begin_ns']-prior['end_ns'] >= 0,
                        'Native host gap lacks its preceding poll')
                c = current['clock']
                drop = c['observed'] and c['dropped_seconds'] > 0
                soak.need(current['poll_wall_ns'] > 50_000_000 or
                    (gap is not None and gap > 50_000_000) or drop or current['flags'] & 128,
                    'Ordinary fast native poll entered slow diagnostics')
            # Preserve invalid/nonfinite raw evidence rather than repairing it.
            # Numerical identities apply only to samples whose owning row is valid.
            c = current['clock']
            if row['valid'] and c['observed']:
                soak.need(not c['nonfinite_fields'] and all(type(c[k]) in (int,float) and
                    math.isfinite(c[k]) and c[k] >= 0 for k in
                    ('elapsed_seconds','accepted_seconds','dropped_seconds','accumulator_seconds')) and
                    0 <= c['committed_ticks'] <= c['planned_ticks'] <= 8, 'Valid native clock observation differs')
                if c['active']:
                    soak.need(math.isclose(c['elapsed_seconds']-c['accepted_seconds'],c['dropped_seconds'],
                        rel_tol=0, abs_tol=64*sys.float_info.epsilon*max(1,c['elapsed_seconds'])), 'Clock observation identity differs')
                else:
                    soak.need(all(c[k] == 0 for k in ('accepted_seconds','dropped_seconds','planned_ticks','committed_ticks')),
                        'Paused clock diagnostic invented simulation work')

    def read_diagnostics(self, label, required=False, failure=None):
        state = self.diagnostic_state
        log = self.record['calls']
        # Flush only at paused/failure/terminal boundaries, never add a running
        # heartbeat or collect pages inside the measured native play window.
        log.flush_pending()
        reason = None
        if len(state['observations']) >= READ_LIMIT:
            reason = 'diagnostic read count exhausted'
        elif state['diagnostic_bytes']+(1 if required else 4)*RESPONSE_LIMIT > TOTAL_DIAGNOSTIC_BYTES:
            reason = 'diagnostic byte reservation exhausted'
        elif log.bytes+(1 if required else 4)*READ_RPC_RESERVATION > soak.MAX_RPC_BYTES:
            reason = 'original RPC cap reserves failure/terminal headroom'
        if reason:
            state['omitted'].append(dict(owner=self.label, label=label, reason=reason,
                rpc_bytes=log.bytes, required=required))
            soak.need(not required, 'Required diagnostic boundary could not fit original bounded RPC log: '+reason)
            return None
        owner = super().rpc('player.inspect')
        soak.need(owner['player_id'] == self.player_id and owner['generation'] == self.generation,
            'Diagnostic observation lost the original player owner')
        soak.need(len(soak.encoded(owner)) <= REPORT_LIMIT, 'Native player report exceeded diagnostic reservation')
        if not required:
            soak.need(owner['active'] and owner['paused'], 'Checkpoint diagnostic was not observed at a native paused boundary')
        value = super().rpc('player.diagnostics.inspect', dict(player_id=self.player_id,generation=self.generation))
        entry = dict(owner=self.label,label=label, observation=value,
            native_report=dict(tick=owner['tick'],paused=owner['paused'],active=owner['active'],
                session_id=owner['session_id'], report=owner['report']),
            report_observation_precedes_diagnostic=True,
            active_clock_may_progress_between_requests=bool(owner['active'] and not owner['paused']))
        if failure is not None:
            entry['failure'] = failure
        # Append actual response before asserting invariants so malformed data
        # survives the original failure cleanup path.
        state['observations'].append(entry)
        size = len(soak.encoded(value))
        state['diagnostic_bytes'] += size
        entry['response_bytes'] = size
        if not required:
            after = super().rpc('player.inspect')
            entry['after_query'] = {key: after[key] for key in
                ('player_id','generation','session_id','tick','paused','active')}
            soak.need(len(soak.encoded(after)) <= REPORT_LIMIT and after['active'] and after['paused'] and
                all(after[k] == owner[k] for k in ('player_id','generation','session_id','tick')),
                'Paused checkpoint diagnostic read advanced native simulation or replaced its owner')
        soak.need(size <= RESPONSE_LIMIT and state['diagnostic_bytes'] <= TOTAL_DIAGNOSTIC_BYTES,
            'Actual diagnostic response exceeded its byte budget')
        soak.need(value['player_id'] == self.player_id and value['generation'] == self.generation and
            value['active'] == owner['active'] and value['terminal'] == (not owner['active']),
            'Diagnostic retained owner/terminal identity differs')
        soak.need(value['invalid_samples'] == 0 and not value['counters_saturated'] and
            not value['audio_cumulative']['counters_saturated'] and
            not value['audio_since_last_poll']['counters_saturated'],
            'Native reliability diagnostics retained invalid or saturated observations')
        previous = self.last_diagnostic.get(self.label)
        self.validate(value, previous)
        if self.label in self.terminal_diagnostic:
            soak.need(value == self.terminal_diagnostic[self.label], 'Retained terminal diagnostics mutated')
        self.last_diagnostic[self.label] = copy.deepcopy(value)
        if value['terminal']:
            self.terminal_diagnostic[self.label] = copy.deepcopy(value)
        return value

    def first_failure(self, where):
        state = self.diagnostic_state
        if state['first_failure_attempted']:
            return
        state['first_failure_attempted'] = True
        failure = dict(where=where, window_observation=None)
        try:
            if self.hwnd:
                rect = w.RECT()
                accepted = bool(self.u.GetClientRect(self.hwnd,ctypes.byref(rect)))
                failure['window_observation'] = dict(client_extent=[rect.right-rect.left,rect.bottom-rect.top]
                    if accepted else None, foreground_owned=self.u.GetForegroundWindow() == self.hwnd,
                    query_succeeded=accepted)
            self.read_diagnostics('first-failure-before-cleanup', required=True, failure=failure)
        except BaseException as error:
            state['errors'].append(dict(where='first-failure-before-cleanup',error=str(error), failure=failure))
            # Never replace the original failure with an observation failure.

    def snapshot(self):
        value = super().snapshot()
        key = self.label,self.replacements
        selected = (self.label == 'fresh' and self.replacements == 1 or
                    self.label == 'soak' and self.replacements in self.selected)
        if selected and key not in self.observed_checkpoints:
            self.observed_checkpoints.add(key)
            self.read_diagnostics('paused-restored-checkpoint-'+str(self.replacements))
        return value

    def run_for(self, *args, **kwargs):
        try:
            return super().run_for(*args, **kwargs)
        except BaseException:
            self.first_failure('running-segment')
            raise

    def rpc(self, method, params=None):
        if method == 'host.shutdown' and self.player_id is not None:
            self.read_diagnostics('terminal-before-host-shutdown', required=True)
        return super().rpc(method,params)

    def close(self):
        if 'error' in self.record and self.client is not None:
            self.first_failure('original-exception-before-close')
        return super().close()


def main():
    original = soak.Soak
    soak.Soak = DiagnosticReliability
    try:
        result = soak.main()  # Original parser, bounds, clock and cleanup policy.
    finally:
        soak.Soak = original
    record = soak.shared.RECORD
    source = Path(__file__)
    expected = record.get('source_sha256', {})
    changed = [name for name,pin in expected.items() if not (ROOT/name).is_file() or soak.shared.sha(ROOT/name) != pin]
    diagnostics = record.get('reliability_diagnostics', {})
    terminal_owners = sorted({entry['owner'] for entry in diagnostics.get('observations',[])
                             if entry['observation']['terminal']})
    summary = dict(format='poima.performance-yard-reliability-diagnostics.v1',
        passed=bool(result == 0 and record['passed'] and not changed and
                    not diagnostics.get('errors') and terminal_owners == ['fresh','soak']),
        original_soak_passed=record['passed'], rehearsal=record['rehearsal'],
        functional_reliability_passed=record['functional_reliability_passed'], alpha_gate_closed=False,
        source_sha256=soak.shared.sha(source), immutable_sources=not changed, changed_sources=changed,
        observed_boundaries=len(diagnostics.get('observations',[])), terminal_owners=terminal_owners,
        diagnostic_bytes=diagnostics.get('diagnostic_bytes',0),
        first_failure_attempted=diagnostics.get('first_failure_attempted',False),
        diagnostic_errors=diagnostics.get('errors',[]),
        native_evidence_sha256=soak.shared.sha(soak.shared.ARGS.output/'evidence.json'),
        limitations=['Selected diagnostic snapshots are not a full per-frame trace.',
            'Paused startup/capture/serialization gaps are not active gameplay stalls.',
            'Resize work can affect a subsequent clock observation; current and prior polls are retained.',
            'Inter-poll gaps combine owner work, RPC handling, host wait and OS scheduling.',
            'Audio cumulative/pending counters include external controls, without new device queries.',
            'No resizing, injected controls, forced hitch, timing-budget or original RPC-cap relaxation.'])
    summary['functional_reliability_passed'] &= summary['passed']
    soak.json_write(soak.shared.ARGS.output/'reliability-diagnostics.json',summary)
    print(json.dumps(dict(passed=summary['passed'],original_soak_passed=record['passed'],
        observed_boundaries=summary['observed_boundaries'])),flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
