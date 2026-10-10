#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Short native Windows player-diagnostics qualification, not a reliability pass.

Run with native Windows Python and the same autonomous world/artifact inputs
as performance_yard_soak.py; --rehearsal is required. Example:
python tests/player_diagnostics_contract.py --binary build/windows-runtime/poima.exe \
  --world build/performance-world/world.json --descriptor build/performance-artifact/native-gameplay.json \
  --workload-manifest build/performance-world/manifest.json --output build/diagnostics --gpu 1 --rehearsal

This wraps the unchanged two-cycle soak rehearsal. It preserves its genuine
native-clock, save/load, preference, audio, fresh-process and cleanup guards.
An owned paused HWND is resized programmatically and restored before any
running segment. This does not qualify physical dragging or window controls.
Fast resize polls need not enter the slow-poll ring: that sub-observation is
reported explicitly. No forced hitch, injected audio or per-tick input occurs.
"""
import argparse
import copy
import ctypes
from ctypes import wintypes as w
import json
import math
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import performance_yard_soak as soak

OPERATIONS = {'queue_wait', 'enqueue', 'queue_observe', 'device_resume',
    'device_pause', 'clear', 'dsp', 'reset', 'gain', 'open', 'flush', 'drain_wait'}


class DiagnosticSoak(soak.Soak):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.previous_diagnostics = None
        self.diagnostic_reads = 0
        self.guard_busy = False
        self.terminal_diagnostics = {}

    def observe(self, state, label):
        value = soak.shared.diagnostics(self.client, state)
        soak.need(self.diagnostic_reads < 2048, 'Diagnostic request budget exceeded')
        self.diagnostic_reads += 1
        soak.need(value['invalid_samples'] == 0 and not value['counters_saturated'],
                  'Actual native diagnostics reported invalid or saturated counters')
        for name in ('audio_cumulative', 'audio_since_last_poll'):
            audio = value[name]
            soak.need(set(audio['wall_ns']) == OPERATIONS and
                not audio['counters_saturated'] and all(type(v) is int and v >= 0
                for v in audio['wall_ns'].values()), 'Actual audio timing declarations differ')
        cumulative = value['audio_cumulative']['wall_ns']
        pending = value['audio_since_last_poll']['wall_ns']
        soak.need(all(pending[k] <= cumulative[k] for k in OPERATIONS),
                  'Pending audio duration exceeds its actual cumulative duration')
        if self.previous_diagnostics is not None:
            old = self.previous_diagnostics
            soak.need((value['player_id'], value['generation']) ==
                      (old['player_id'], old['generation']), 'Diagnostic owner changed within one native window')
            soak.need(all(value[k] >= old[k] for k in
                ('polls', 'slow_polls', 'overwritten', 'invalid_samples', 'max_gap_ns',
                 'max_wall_ns', 'clock_drop_polls')), 'Diagnostic lifetime counters regressed')
            soak.need(all(cumulative[k] >= old['audio_cumulative']['wall_ns'][k]
                          for k in OPERATIONS), 'Audio cumulative counters reset on save/load or controls')
            if value['polls'] == old['polls']:
                old_pending = old['audio_since_last_poll']['wall_ns']
                old_cumulative = old['audio_cumulative']['wall_ns']
                soak.need(all(pending[k] >= old_pending[k] and
                    pending[k]-old_pending[k] == cumulative[k]-old_cumulative[k] for k in OPERATIONS),
                    'Reading pending audio counters consumed or changed the uncompleted interval')
        for row in value['rows']:
            current, previous = row['current'], row['previous']
            wall, gap = current['poll_wall_ns'], row['inter_poll_gap_ns']
            clock = current['clock']
            dropped = clock['observed'] and clock['dropped_seconds'] > 0
            soak.need(not row['valid'] or wall > 50_000_000 or
                      (gap is not None and gap > 50_000_000) or dropped or current['flags'] & 128,
                      'Ordinary fast poll was retained as a slow observation')
            for sample in (current, previous):
                if sample is None:
                    continue
                c = sample['clock']
                if c['observed']:
                    soak.need(not c['nonfinite_fields'] and 0 <= c['committed_ticks'] <= c['planned_ticks'] <= 8 and
                              all(math.isfinite(c[k]) and c[k] >= 0 for k in
                              ('elapsed_seconds', 'accepted_seconds', 'dropped_seconds', 'accumulator_seconds')),
                              'Native interactive clock observation is malformed')
                    if c['active']:
                        soak.need(math.isclose(c['elapsed_seconds']-c['accepted_seconds'],
                            c['dropped_seconds'], abs_tol=64*sys.float_info.epsilon*max(1,c['elapsed_seconds'])),
                            'Clock accepted/dropped observation differs from actual policy')
                    else:
                        soak.need(c['accepted_seconds'] == c['dropped_seconds'] ==
                                  c['planned_ticks'] == c['committed_ticks'] == 0,
                                  'Paused diagnostic observation invented simulation work')
        self.previous_diagnostics = copy.deepcopy(value)
        self.record.setdefault('diagnostic_observations', []).append(
            dict(owner=self.label, label=label, observation=value))
        return value

    def player(self):
        state = super().player()
        if not self.guard_busy:
            self.observe(state, 'existing-player-boundary')
        return state

    def logical_boundary(self):
        state = super().player()
        soak.need(state['paused'], 'Read-only diagnostic proof requires native pause')
        tick = state['tick']
        return dict(player_id=state['player_id'], generation=state['generation'],
            session_id=state['session_id'], tick=tick, control_revision=state['control_revision'],
            world=self.rpc('world.inspect'), runtime=self.runtime(), module=self.module(tick),
            ui=self.ui(tick), preferences=self.preferences())

    def read_only_guards(self):
        self.guard_busy = True
        try:
            before = self.logical_boundary()
            state = super().player()
            self.observe(state, 'guarded-read-only')
            for params, code in (
                (dict(player_id=soak.shared.uid(999999), generation=self.generation), -32004),
                (dict(player_id=self.player_id, generation=self.generation+1), -32009),
                (dict(player_id=self.player_id, generation=self.generation, unknown=True), -32602),
                (dict(player_id=self.player_id, generation=float(self.generation)), -32602)):
                self.client.reject('player.diagnostics.inspect', params, code)
            soak.need(self.logical_boundary() == before,
                      'Successful/rejected diagnostic reads changed logical runtime or preferences')
            self.record.setdefault('diagnostic_readonly', []).append(dict(owner=self.label,
                tick=before['tick'], identity_preserved=True, rejected_requests=4,
                native_report_counters_may_advance=True))
        finally:
            self.guard_busy = False

    def rpc(self, method, params=None):
        # Observe external controls through their cumulative timing counters.
        # The next normal host poll may consume pending durations between RPCs;
        # that is recorded, never treated as a query-caused state mutation.
        track = method == 'player.control' and self.player_id is not None and not self.guard_busy
        before = self.observe(super().rpc('player.inspect'), 'before-external-control') if track else None
        if method == 'host.shutdown' and self.label in self.terminal_diagnostics:
            # The original close() has already stopped Runtime at this point.
            terminal = super().rpc('player.inspect')
            soak.need(not terminal['active'], 'Terminal player unexpectedly reactivated')
            value = self.observe(terminal, 'terminal-after-runtime-stop')
            soak.need(value == self.terminal_diagnostics[self.label],
                      'Runtime stop changed retained terminal diagnostic data')
        result = super().rpc(method, params)
        if track:
            after_state = super().rpc('player.inspect')
            after = self.observe(after_state, 'after-external-control')
            self.record.setdefault('diagnostic_external_controls', []).append(dict(owner=self.label,
                action=params['action'], before=before['audio_cumulative'], after=after['audio_cumulative'],
                pending=after['audio_since_last_poll'], owner_preserved=True))
        if method == 'player.inspect' and self.player_id is not None and not result['active'] and result.get('report'):
            first = self.observe(result, 'terminal')
            second = self.observe(result, 'terminal-repeat')
            soak.need(first == second, 'Cached terminal diagnostic data is mutable')
            retained = self.terminal_diagnostics.setdefault(self.label, copy.deepcopy(first))
            soak.need(retained == first, 'Terminal diagnostic data changed after Runtime stop')
            self.record.setdefault('diagnostic_terminal', {})[self.label] = first
        return result

    def resize_paused(self):
        initial = super().player()
        soak.need(initial['paused'] and self.hwnd, 'Owned resize requires ready paused native window')
        sid, tick = initial['session_id'], initial['tick']
        changes = []
        resize_record = dict(owner=self.label, changes=changes, restored_extent=None,
            tick_preserved=False, session_preserved=False, pixel_equivalence_claimed=False,
            phase='before-capture')
        self.record.setdefault('diagnostic_resizes', []).append(resize_record)
        self.capture('diagnostic-before-resize')
        self.u.GetWindowLongW.argtypes = [w.HWND, ctypes.c_int]
        self.u.GetWindowLongW.restype = ctypes.c_long
        self.u.GetDpiForWindow.argtypes = [w.HWND];self.u.GetDpiForWindow.restype = w.UINT
        self.u.AdjustWindowRectExForDpi.argtypes = [ctypes.POINTER(w.RECT), w.DWORD, w.BOOL, w.DWORD, w.UINT]
        self.u.AdjustWindowRectExForDpi.restype = w.BOOL
        self.u.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
        self.u.SetWindowPos.restype = w.BOOL
        baseline = self.observe(initial, 'before-owned-resize')
        for width, height in ((960,540), (1920,1080)):
            resize_record['phase'] = 'request-'+str(width)+'x'+str(height)
            observations = []
            change = dict(requested_extent=[width,height], observations=observations,
                request_accepted=False, settled=False, retained_resize_observed=None,
                retained_resize_rows=[],
                retention_note='Only slow/drop/error/invalid polls enter the ring; fast resize need not be retained.')
            changes.append(change)
            rectangle = w.RECT(0,0,width,height)
            dpi = self.u.GetDpiForWindow(self.hwnd)
            style = self.u.GetWindowLongW(self.hwnd, -16) & 0xffffffff
            extended = self.u.GetWindowLongW(self.hwnd, -20) & 0xffffffff
            soak.need(dpi and self.u.AdjustWindowRectExForDpi(ctypes.byref(rectangle), style, False, extended, dpi),
                      'Cannot compute owned window client extent')
            # Suppress the pre-change sizing clamp for the programmatic owned
            # resize. WM_WINDOWPOSCHANGED and SDL's native resize events still
            # run; client/native extents remain exact qualification conditions.
            flags = 0x0002|0x0004|0x0010|0x0400  # NOMOVE, NOZORDER, NOACTIVATE, NOSENDCHANGING
            change['resize_flags'] = flags
            soak.need(self.u.SetWindowPos(self.hwnd, None, 0, 0, rectangle.right-rectangle.left,
                rectangle.bottom-rectangle.top, flags), 'Owned window resize failed')
            change['request_accepted'] = True
            change['requested_outer_extent'] = [rectangle.right-rectangle.left, rectangle.bottom-rectangle.top]
            until = min(self.deadline, time.monotonic()+15)
            while True:
                state = super().rpc('player.inspect')
                rect = w.RECT()
                soak.need(self.u.GetClientRect(self.hwnd, ctypes.byref(rect)), 'Cannot observe resized owned client')
                client = [rect.right-rect.left, rect.bottom-rect.top]
                observations.append(dict(client_extent=client, native_extent=[state['report']['width'],state['report']['height']],
                    tick=state['tick'], session_id=state['session_id'], paused=state['paused']))
                soak.need(state['active'] and state['paused'] and state['tick'] == tick and state['session_id'] == sid and
                    state['player_id'] == self.player_id and state['generation'] == self.generation,
                    'Paused OS resize changed native simulation or ownership')
                soak.need(state['report']['nvrhi_errors'] == 0 and
                    state['report']['runtime_replacements'] == self.replacements,
                    'Resize failed the retained native owner')
                if state['ready'] and client == [width,height] and observations[-1]['native_extent'] == [width,height]:
                    change['settled'] = True
                    break
                soak.need(time.monotonic() < until, 'Native resized extent did not settle within15 seconds')
                time.sleep(.05)
            diagnostic = self.observe(state, 'owned-resize-'+str(width))
            rows = [row for row in diagnostic['rows'] if row['poll_sequence'] > baseline['polls'] and
                row['current']['flags'] & 8 and row['current']['resize_ns'] > 0 and
                [row['current']['width'],row['current']['height']] == [width,height]]
            change['retained_resize_observed'] = bool(rows)
            change['retained_resize_rows'] = rows
        resize_record['phase'] = 'after-capture'
        self.capture('diagnostic-after-resize')
        soak.need(super().player()['tick'] == tick and self.sid == sid,
                  'Resize capture advanced native simulation')
        resize_record.update(restored_extent=[1920,1080], tick_preserved=True,
            session_preserved=True, phase='completed')

    def start(self, label):
        self.previous_diagnostics = None
        super().start(label)
        self.record['source_sha256']['tests/player_diagnostics_contract.py'] = soak.shared.sha(Path(__file__))
        state = super().player()
        render = state['report']['render_diagnostics']
        soak.need(not render['profile_requested'] and not render['gpu']['available'] and
            render['gpu']['total']['samples'] == render['ambient_occlusion_gpu']['samples'] ==
            render['ambient_occlusion_filter_gpu']['samples'] == render['deferred_lighting_gpu']['samples'] == 0,
            'Always-on CPU diagnostics enabled unrequested GPU profiling')
        self.record.setdefault('diagnostic_gpu_disabled', []).append(dict(owner=label,
            present_count=state['report']['frames_presented'], profile_requested=False, gpu_samples=0))
        soak.need(state['report']['frames_presented'] > 0, 'No genuine presentation preceded CPU diagnostic observation')
        self.read_only_guards()
        if label == 'soak':
            self.resize_paused()
            self.focus()  # Original1920x1080 foreground/display check again.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','descriptor','workload-manifest','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--rehearsal', action='store_true', required=True)
    args = parser.parse_args()
    soak.need(os.name == 'nt' and not sys.flags.optimize and 0 <= args.gpu <= 4095,
              'Use native Windows Python without optimization and a valid GPU index')
    previous_class, previous_argv = soak.Soak, sys.argv
    fixed = ['--rehearsal','--cycles','2','--warmup','10','--before-save-seconds','10',
             '--after-save-seconds','5','--continuation-seconds','30','--timeout','600']
    sys.argv = [str(Path(soak.__file__))]
    for name in ('binary','world','descriptor','workload_manifest','output','gpu'):
        sys.argv += ['--'+name.replace('_','-'), str(getattr(args,name))]
    sys.argv += fixed
    soak.Soak = DiagnosticSoak
    try:
        result = soak.main()
    finally:
        soak.Soak, sys.argv = previous_class, previous_argv
    record = soak.shared.RECORD
    terminals = record.get('diagnostic_terminal', {})
    summary = dict(format='poima.player-diagnostics-qualification.v1', passed=False,
        rehearsal_only=True, functional_reliability_qualified=False, alpha_gate_closed=False,
        native_evidence_sha256=soak.shared.sha(args.output.resolve()/'evidence.json'),
        source_sha256=soak.shared.sha(Path(__file__)), observations=len(record.get('diagnostic_observations',[])),
        cycles_completed=len(record['cycles']), resize=record.get('diagnostic_resizes',[]),
        terminal_owners=sorted(terminals), limitations=[
            'Short two-cycle functional rehearsal, not the thirty-minute reliability gate.',
            'Diagnostic reads observe completed native CPU polls; host polls may run between requests.',
            'Fast resize polls may be absent from the slow ring; no forced stall is introduced.',
            'No audible-output, physical-input, GPU-fault-cause or VRAM qualification.'])
    try:
        soak.need(result == 0 and record['passed'] and record['harness_rehearsal_passed'] and
            not record['functional_reliability_passed'] and len(record['cycles']) == 2 and
            set(terminals) == {'soak','fresh'} and len(record.get('diagnostic_readonly',[])) == 2 and
            record['fresh_continuation']['exact_state_restored'], 'Original rehearsal/diagnostic scope did not finish')
        audio = terminals['soak']['audio_cumulative']['wall_ns']
        soak.need(all(audio[k] > 0 for k in ('open','enqueue','dsp','device_resume','device_pause',
            'clear','reset','gain','flush','drain_wait')),
            'Real native audio operations did not produce expected cumulative observations')
        summary['passed'] = True
    except BaseException as error:
        summary['error'] = str(error)
    soak.json_write(args.output.resolve()/'diagnostics-evidence.json', summary)
    print(json.dumps(dict(diagnostics_passed=summary['passed'], rehearsal_passed=record['passed'])),flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
