#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real-clock Native AOT Performance Yard save/restore reliability check.

No build/download, caller-driven movement, pose editing, replay, or reload of a
different NativeAOT image occurs. Compatible CoreCLR reload/scene-edit gates
are separate and remain open. One initialization tick per actual fresh host
is outside measured play. This is a warm small-game test on one Windows GPU.
"""
import argparse
import copy
import ctypes
from ctypes import wintypes as w
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import sys
import threading
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools/performance'))
import player_service_contract as shared
import performance_yard as yard
from player_performance import verify_capture
import windows_process_memory as memory

PERFORMANCE = 'c0940000000000000000000000000001'
CADENCE = 'c0870000000000000000000000000001'
FIELDS = ['world_matrix', 'layout', 'local_transform', 'animation', 'motion',
          'kinematic_target', 'motion_remaining_ticks', 'velocity', 'has_body',
          'is_character', 'ground', 'yaw', 'pitch']
MAX_RPC_BYTES = 512 * 1024 * 1024


def need(condition, text):
    if not condition:
        raise AssertionError(text)


def fresh():
    return uuid.uuid4().hex


def normalize(value):
    # Only public opaque runtime session identity is normalized. No field,
    # tick, native revision, UI state, audio cursor or gameplay value is hidden.
    if isinstance(value, dict):
        return {key: normalize(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [normalize(item) for item in value]
    return value


def preference_identity(value):
    # Player control revision/presentation counters legitimately change on
    # replacement. The independent preference owner, values and configuration
    # revision must remain current; do not compare incidental renderer reports.
    return dict(player_id=value['player_id'], revision=value['settings']['revision'],
                values=copy.deepcopy(value['settings']['values']))


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False, allow_nan=False).encode('utf-8')


def json_write(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def gzip_write(path, value):
    with Path(path).open('xb') as stream:
        with gzip.GzipFile(filename='', mode='wb', fileobj=stream, mtime=0) as out:
            out.write(encoded(value))


def inventory(directory):
    result, folded, total = {}, set(), 0
    need(directory.is_dir(), 'Missing immutable input directory')
    for path in sorted(directory.rglob('*')):
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(),
             'Input closure contains a link or junction')
        if path.is_dir():
            continue
        need(path.is_file(), 'Input closure contains a special file')
        name = path.relative_to(directory).as_posix()
        need(name.casefold() not in folded, 'Input closure has a case collision')
        folded.add(name.casefold())
        size = path.stat().st_size
        total += size
        need(size <= 256*1024*1024 and total <= 1024*1024*1024 and len(result)<50000,
             'Input inventory exceeds file/count/byte bound')
        result[name] = dict(bytes=size, sha256=shared.sha(path))
    return result


class RpcLog:
    """shared.Client appends a mutable request before filling its response."""
    def __init__(self, path):
        self.file = path.open('xb')
        self.stream = gzip.GzipFile(filename='', mode='wb', fileobj=self.file, mtime=0)
        self.pending, self.count, self.bytes = None, 0, 0

    def append(self, row):
        self.flush_pending()
        self.pending = row
        self.count += 1

    def flush_pending(self):
        if self.pending is not None:
            payload = encoded(self.pending) + b'\n'
            self.bytes += len(payload)
            need(self.bytes <= MAX_RPC_BYTES, 'Retained RPC log exceeded 512 MiB uncompressed')
            self.stream.write(payload)
            self.pending = None

    def close(self):
        try:
            self.flush_pending()
        finally:
            self.stream.close()
            self.file.close()


class Sampler:
    def __init__(self, owner, path, deadline):
        probe = memory.WindowsProvider(memory.Options(owner.process.pid))
        self.identity = probe.creation_filetime
        probe.close()
        self.stop = threading.Event()
        self.result, self.path = None, path

        def sleep(seconds):
            if self.stop.wait(seconds):
                raise KeyboardInterrupt  # collect retains partial rows.

        def sample():
            self.result = memory.collect(memory.Options(owner.process.pid,
                duration=min(3599, max(1, deadline-time.monotonic())), interval=1,
                system_ram=True, expected_creation_filetime=self.identity), sleep=sleep)

        self.thread = threading.Thread(target=sample, daemon=True)
        self.thread.start()

    def finish(self):
        self.stop.set()
        self.thread.join(15)
        need(not self.thread.is_alive() and self.result is not None, 'Memory sampler did not stop')
        result = self.result
        json_write(self.path, result)
        need(result['process']['creation_filetime_100ns'] == str(self.identity) and
             result['stop_reason'] == 'interrupted' and 'error' not in result and
             'close_error' not in result and len(result['samples']) >= 2,
             'Memory sampling failed or original process identity changed')
        return result


class Soak:
    def __init__(self, args, record, world, descriptor, manifest, deadline, owned):
        self.args, self.record, self.world = args, record, world
        self.descriptor, self.manifest, self.deadline = descriptor, manifest, deadline
        self.owned, self.host, self.client, self.sampler = owned, None, None, None
        self.sid, self.player_id, self.generation = None, None, None
        self.replacements, self.previous_capture = 0, None
        self.label, self.hwnd = '', None
        self.u = ctypes.WinDLL('user32', use_last_error=True)
        self.callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.u.EnumWindows.argtypes = [self.callback, w.LPARAM]
        self.u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        self.u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        self.u.GetForegroundWindow.restype = w.HWND
        self.u.SetForegroundWindow.argtypes = [w.HWND]
        self.u.SetForegroundWindow.restype = w.BOOL
        self.u.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]

    def rpc(self, method, params=None):
        need(time.monotonic() < self.deadline, 'Whole-cohort deadline expired')
        need(self.host is None or self.host.process.poll() is None, 'Owned native host exited unexpectedly')
        return self.client.call(method, params)

    def player(self):
        state = self.rpc('player.inspect')
        if state['active'] and not state['ready']:
            state = shared.await_state(self.client, lambda row: row['active'] and row['ready'] and
                row['player_id'] == self.player_id and row['generation'] == self.generation and
                row['session_id'] == self.sid, self.label+'-presentation-ready')
        need(state['active'] and state['ready'] and state['player_id'] == self.player_id and
             state['generation'] == self.generation, 'Player/window ownership changed')
        report = state['report']
        need(report['hardware'] and report['nvrhi_errors'] == 0 and
             report['runtime_replacements'] == self.replacements, 'Native report failed/replacement count differs')
        self.sid = state['session_id']
        return state

    def pause(self):
        state = self.player()
        if not state['paused']:
            self.rpc('player.control', shared.control_parameters(state, 'pause'))
        state = self.player()
        need(state['paused'], 'Native pause did not freeze the actual player')
        return state

    def runtime(self):
        return self.rpc('runtime.inspect', dict(session_id=self.sid))

    def module(self, tick):
        result = self.rpc('runtime.gameplay.inspect', dict(session_id=self.sid, tick=tick, include_schema=True))
        actual = result['module']
        library = next(row for row in self.descriptor['files'] if row['role'] == 'library')
        need(actual['backend'] == 'native_aot' and actual['type'] == self.descriptor['type'] and
             actual['schema'] == self.descriptor['schema'] and actual['assembly_sha256'] == library['sha256'] and
             actual['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
             'Actual native gameplay image/schema/backend differs')
        return result

    def ui(self, tick):
        result = self.rpc('runtime.ui.inspect', dict(session_id=self.sid, tick=tick, limit=256))
        need(result['next_after'] is None, 'UI reflection unexpectedly truncated')
        return result

    def control(self, action):
        state = self.pause()
        runtime, module, ui = self.runtime(), self.module(state['tick']), self.ui(state['tick'])
        rows = [row for row in ui['elements'] if row.get('action') == action and row['eligible']]
        need(len(rows) == 1, 'Compiled action has no unique currently eligible stable UI binding: '+action)
        params = dict(session_id=self.sid, request_id=fresh(), expected_tick=state['tick'],
            expected_ui_revision=runtime['ui_revision'], expected_control_sequence=runtime['control_sequence'],
            expected_gameplay_revision=module['revision'], expected_structure_revision=runtime['structure_revision'],
            id=rows[0]['id'])
        result = self.rpc('runtime.ui.activate', params)
        self.sid = result['current_session_id']
        if result['runtime_replaced']:
            self.replacements += 1
        if result['runtime_replaced']:
            # A restored paused runtime needs one presentation poll to become
            # ready. Await the exact retained owner without advancing Tick.
            shared.await_state(self.client, lambda row: row['active'] and row['ready'] and
                row['paused'] and row['player_id'] == self.player_id and
                row['generation'] == self.generation and row['session_id'] == self.sid and
                row['report']['runtime_replacements'] == self.replacements,
                self.label+'-replacement-'+str(self.replacements))
        # Resume intents really resume. This helper never immediately pauses
        # them or attributes the old acknowledgment tick to subsequent reads.
        self.record.setdefault('controls', []).append(dict(owner=self.label, action=action,
            params=params, result=result, elapsed=time.monotonic()-self.record['started_monotonic']))
        if action in ('save', 'load'):
            need(result['save_serviced'] and result['save_operation']['state'] == 3,
                 'Compiled save/load did not commit the real configured store')
        return result, params

    def snapshot(self):
        state = self.pause()
        tick = state['tick']
        runtime, module, ui = self.runtime(), self.module(tick), self.ui(tick)
        need(runtime['tick'] == tick and not runtime['source_stale'], 'Snapshot runtime is not the frozen source')
        identifiers = set(self.record['authored_entity_ids'])
        for key in ('CellOne', 'CellTwo', 'CellThree'):
            value = module['module']['values'][key]
            if value != shared.uid(0):
                identifiers.add(value)
        need(runtime['entities'] == len(identifiers), 'Unexpected live entity membership outside exact snapshot oracle')
        component_types = sorted(schema['id'] for schema in self.descriptor['schema']['components'])
        need(len(component_types) <= 4, 'Snapshot exceeds native component selector bound')
        pages = []
        identifiers = sorted(identifiers)
        for offset in range(0, len(identifiers), 32):
            pages.append(self.rpc('runtime.observe', dict(session_id=self.sid, tick=tick,
                ids=identifiers[offset:offset+32], entity_fields=FIELDS,
                components=[dict(type=kind) for kind in component_types], include_schemas=offset == 0,
                structure_revision=runtime['structure_revision'], gameplay_revision=module['revision'],
                ui_revision=runtime['ui_revision'], control_sequence=runtime['control_sequence'])))
        voices = self.rpc('runtime.audio.voices', dict(session_id=self.sid, tick=tick, limit=256))
        need(not voices['has_more'] and len(voices['voices']) <= 256, 'Native sound history is not bounded/complete')
        need(self.player()['tick'] == tick, 'Paused snapshot advanced simulation')
        # Loader paths/migration diagnostics are not persisted simulation
        # state. Full unmodified module responses remain in the RPC log.
        actual_module = module['module']
        persisted_module = {key:actual_module[key] for key in
            ('backend','type','schema','assembly_sha256','native_diagnostics','values')}
        return normalize(dict(runtime=runtime, module=dict(revision=module['revision'],
            structure_revision=module['structure_revision'],tick=module['tick'],module=persisted_module),
            ui=ui, observations=pages, voices=voices))

    def route(self):
        state = self.pause()
        return self.rpc('runtime.component.get', dict(session_id=self.sid, tick=state['tick'],
            id=shared.uid(100), type=PERFORMANCE))['values']

    def preferences(self):
        return self.rpc('player.settings.inspect', dict(player_id=self.player_id))

    def focus(self):
        windows = []
        @self.callback
        def visit(hwnd, _):
            pid = w.DWORD()
            self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            title = ctypes.create_unicode_buffer(256)
            self.u.GetWindowTextW(hwnd, title, 256)
            if pid.value == self.host.process.pid and title.value.startswith('Poima player'):
                windows.append(hwnd)
            return True
        self.u.EnumWindows(visit, 0)
        need(len(windows) == 1, 'Expected exactly one actual owned native window')
        self.hwnd = windows[0]
        accepted = bool(self.u.SetForegroundWindow(self.hwnd))
        need(self.u.GetForegroundWindow() == self.hwnd, 'Owned player failed to acquire actual foreground focus')
        rect = w.RECT()
        need(self.u.GetClientRect(self.hwnd, ctypes.byref(rect)), 'Cannot observe client extent')
        extent = [rect.right-rect.left, rect.bottom-rect.top]
        need(extent == [self.args.width, self.args.height], 'Actual window client extent differs from requested render extent')
        return dict(request_accepted=accepted, client_extent=extent, machine=yard.machine_observation(self.hwnd))

    def start(self, label):
        self.label, self.replacements, self.previous_capture = label, 0, None
        endpoint = 'performance-soak-'+fresh()
        self.host = shared.ProcessOwner([str(self.args.binary), 'serve', str(self.world), '--endpoint', endpoint],
                                       label+'-host', endpoint, 'serve')
        self.owned.append(self.host)
        self.client = shared.Client(endpoint, label+'-client')
        self.owned.append(self.client)
        build = self.rpc('world.describe')
        self.record.setdefault('builds', []).append(build.get('build'))
        self.sid = fresh()
        self.rpc('runtime.start', dict(session_id=self.sid, revision=self.rpc('world.inspect')['revision']))
        current = self.rpc('runtime.gameplay.inspect', dict(session_id=self.sid, tick=0))
        self.rpc('runtime.gameplay.load_native', dict(session_id=self.sid, request_id=fresh(), expected_tick=0,
            expected_revision=current['revision'], descriptor=str(self.args.descriptor)))
        self.rpc('runtime.step', dict(session_id=self.sid, request_id=fresh(), expected_tick=0, ticks=1))
        self.record.setdefault('initialization_ticks', []).append(dict(owner=label, ticks=1, measured_play=False))
        self.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=str(self.args.output/'saves')))
        runtime = self.runtime()
        params = shared.start_parameters(self.sid, expected_tick=1, width=self.args.width, height=self.args.height,
            gpu=self.args.gpu, frames_in_flight=2, audio=True, expected_structure_revision=runtime['structure_revision'],
            lighting_path='deferred', ambient_occlusion=dict(mode='gtao', quality='medium', radius=1),
            settings_overrides={'camera.vertical_fov':60, 'ui.scale':1, 'audio.master_gain':1})
        params.pop('controller')
        response = self.rpc('player.start', params)
        self.player_id, self.generation = response['player_id'], response['generation']
        shared.await_state(self.client, lambda row: row['ready'] and row['paused'], label+'-ready')
        self.record.setdefault('window_observations', []).append(dict(owner=label, **self.focus()))
        self.module(1)
        self.sampler = Sampler(self.host, self.args.output/(label+'-memory.json'), self.deadline)

    def capture(self, label):
        state = self.pause()
        path = self.args.output/(label+'.bmp')
        result = self.rpc('player.capture', dict(player_id=self.player_id, request_id=fresh(),
            expected_control_revision=state['control_revision'], session_id=self.sid, tick=state['tick'],
            expected_structure_revision=state['structure_revision'], expected_ui_revision=state['ui_revision'], path=str(path)))
        actual = result['capture']
        draws = actual['render_diagnostics']['last_draws']
        need(actual['capture_written'] and actual['hardware'] and actual['nvrhi_errors'] == 0 and
             actual['width'] == self.args.width and actual['height'] == self.args.height and
             draws['skinned_instances'] >= 24 and draws['skinned_vertices'] > 0 and draws['objects'] >= 768,
             'Bounded paused capture did not submit the genuine imported workload')
        self.record.setdefault('images', []).append(dict(owner=self.label, label=label, sha256=shared.sha(path),
            bytes=path.stat().st_size, result=result,
            scope='Actual native readback and submission receipt; no full-image reference or physical input claim'))

    def collect_frames(self, capture_id):
        result = verify_capture(self.client, capture_id)
        summary, rows = result['summary'], result['rows']
        need(not summary['full'] and summary['frames_dropped'] == 0 and summary['frames_rejected'] == 0,
             'Short recorder window overflowed or rejected samples')
        need(all(row['session'] == self.sid and [row['width'], row['height']] == [self.args.width,self.args.height]
                 and row['flags'] & 1 for row in rows), 'Frame identity/extent/foreground differs')
        need(all(not row['flags'] & (2|4|8|16|32|64|128|256) for row in rows[2:]),
             'Unexpected pause/replay/capture/storage/resize/replacement in running window')
        need(all(0 <= row['tick_after']-row['tick_before'] <= 8 for row in rows) and
             all(b['tick_before'] == a['tick_after'] for a,b in zip(rows,rows[1:])), 'Native clock chronology differs')
        wall = (rows[-1]['end_ns']-rows[0]['begin_ns']) / 1e9
        clock = (rows[-1]['tick_after']-rows[0]['tick_before']) / wall
        need(58 <= clock <= 62, 'Captured interactive clock did not sustain fixed 60 Hz')
        cadence = summary['present_return_cadence_eligible']
        need(cadence['samples'] >= 10, 'No eligible actual present-return cadence')
        path = self.args.output/('frames-'+capture_id+'.json.gz')
        gzip_write(path, result)
        self.record.setdefault('frame_windows', []).append(dict(owner=self.label, capture_id=capture_id,
            file=path.name, sha256=shared.sha(path), count=len(rows), summary=summary, ticks_per_second=clock,
            timing_budgets=dict(p95_16_7ms=cadence['p95'] <= 16_700_000, p99_25ms=cadence['p99'] <= 25_000_000)))
        self.previous_capture = capture_id

    def run_for(self, seconds, label, measured=True, frame_window=False):
        before = self.player()
        need(not before['paused'], 'Real-clock segment must begin with an observed running owner')
        need(self.u.GetForegroundWindow() == self.hwnd, 'Actual focus lost before running segment')
        began = time.monotonic()
        until, capture_id, frozen_capture = began+seconds, None, None
        if frame_window:
            capture_id = fresh()
            self.rpc('performance.start', dict(capture_id=capture_id, expected_capture_id=self.previous_capture,
                player_id=self.player_id, capacity=65536))
        last, checkpoints = began, []
        initial_machine = yard.machine_observation(self.hwnd)
        while time.monotonic() < until:
            need(time.monotonic() < self.deadline, 'Whole-cohort deadline expired during real play')
            time.sleep(min(5, max(0, until-time.monotonic())))
            now = time.monotonic()
            need(now-last < 10, 'Large scheduling/suspend gap interrupts genuine play qualification')
            current = self.player()
            need(not current['paused'] and current['session_id'] == before['session_id'] and
                 self.u.GetForegroundWindow() == self.hwnd, 'Unexpected pause/session replacement/focus loss')
            report = current['report']
            need(report['dropped_wall_seconds'] == before['report']['dropped_wall_seconds'],
                 'Native clock discarded genuine running wall time')
            diagnostics = report['render_diagnostics']
            audio = report['audio']
            need(audio['enabled'] and audio['driver'] not in ('dummy','disk','unknown','') and
                 audio['submitted_frames'] >= before['report']['audio']['submitted_frames'] and
                 audio['max_queued_frames'] <= 5824, 'Actual bounded native audio sink failed or fell behind')
            draws = diagnostics['last_draws']
            need(draws['skinned_instances'] >= 24 and draws['skinned_vertices'] > 0 and draws['objects'] >= 768 and
                 diagnostics['light_assignment']['light_count'] == 64 and draws['shadow_views'] > 0,
                 'Declared imported geometry/light/shadow workload disappeared')
            checkpoints.append(dict(wall_seconds=now-began, tick=current['tick'],
                completed_submissions=diagnostics['completed_submissions'], last_draws=draws,
                audio=report['audio']))
            last = now
            if capture_id is not None and now-began >= 10:
                self.rpc('performance.stop', dict(capture_id=capture_id))
                frozen_capture, capture_id = capture_id, None
        if capture_id is not None:
            self.rpc('performance.stop', dict(capture_id=capture_id))
            frozen_capture = capture_id
        # Conservative credited interval ends BEFORE the pause request. The
        # reported final tick may include real ticks during that request.
        ended = time.monotonic()
        after = self.pause()
        need(after['report']['audio']['submitted_frames'] > before['report']['audio']['submitted_frames'],
             'Native audio sink did not advance during actual running play')
        if frozen_capture is not None:
            self.collect_frames(frozen_capture)
        final_machine = yard.machine_observation(self.hwnd)
        need(initial_machine['ac_line_status'] == final_machine['ac_line_status'] and
             initial_machine['display'] == final_machine['display'],
             'Power source or owning monitor mode changed within running segment')
        baseline_machine = self.record['window_observations'][0]['machine']
        need(initial_machine['ac_line_status'] == baseline_machine['ac_line_status'] and
             initial_machine['display'] == baseline_machine['display'],
             'Power source or owning monitor mode changed across the cohort')
        need(after['report']['dropped_wall_seconds'] == before['report']['dropped_wall_seconds'],
             'Native clock discarded running wall time before the final paused boundary')
        ticks, duration = after['tick']-before['tick'], ended-began
        need(58 <= ticks/duration <= 62, 'Running segment did not sustain real 60 Hz simulation')
        route = self.route()
        need(route[shared.uid(1)] == 1 and int(route[shared.uid(14)]) > 0 and route[shared.uid(10)] > 0,
             'Compiled autonomous route is disabled or did not observe actual controller movement')
        self.record.setdefault('play_segments', []).append(dict(owner=self.label,label=label,measured=measured,
            credited_unpaused_seconds=duration, tick_before=before['tick'], tick_after=after['tick'],
            ticks_per_second=ticks/duration, heartbeat=checkpoints, route=route,
            power_display_before=initial_machine,power_display_after=final_machine))
        return duration, route

    def close(self):
        if self.sampler:
            sampler, self.sampler = self.sampler, None
            result = sampler.finish()
            self.record.setdefault('memory', []).append(dict(owner=self.label,file=sampler.path.name,
                sha256=shared.sha(sampler.path),summary=result['summary'],sample_count=len(result['samples']),
                creation_filetime_100ns=str(sampler.identity)))
        if self.client:
            state = self.rpc('player.inspect')
            if state['active']:
                self.rpc('player.control', shared.control_parameters(state,'stop'))
            terminal = self.rpc('player.inspect')
            need(terminal['report']['success'] and terminal['report']['nvrhi_errors'] == 0,
                 'Actual player ended unsuccessfully')
            need(terminal['report']['audio']['enabled'] and terminal['report']['audio']['stream_drained'],
                 'Actual native audio stream did not drain on clean player stop')
            self.record.setdefault('terminal_players', []).append(dict(owner=self.label,state=terminal))
            runtime = self.rpc('runtime.status')
            if runtime['active']:
                self.rpc('runtime.stop',dict(session_id=runtime['session_id']))
            self.rpc('host.shutdown')
            self.client.close(); self.client = None
        if self.host:
            self.host.close(); self.host = None


def durable(storage, expected_generation):
    slot = storage/'slot-relay-yard'
    current = json.loads((slot/'current.json').read_text(encoding='utf-8'))['payload']
    item = current['current']
    filename = item['file']
    need(Path(filename).name == filename and filename not in ('.','..') and
         item['generation'] == current['generation'] == expected_generation, 'Durable slot/generation differs')
    path = slot/filename
    need(path.is_file() and path.stat().st_size == item['bytes'] and shared.sha(path) == item['sha256'],
         'Actual durable payload does not match manifest inventory')
    payload = json.loads(path.read_text(encoding='utf-8'))['snapshot']['payload']
    need(not any(key in payload for key in ('preferences','player_preferences','settings','live_settings')),
         'Durable save contains player preference owner/configuration')
    return dict(manifest_sha256=shared.sha(slot/'current.json'), payload_sha256=item['sha256'],
                generation=expected_generation, saved_tick=payload['tick']), inventory(storage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','descriptor','workload-manifest','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=1)
    parser.add_argument('--rehearsal', action='store_true', help='Short harness check; never a reliability qualification')
    parser.add_argument('--cycles',type=int,default=40)
    parser.add_argument('--warmup',type=float,default=60)
    parser.add_argument('--before-save-seconds',type=float,default=35)
    parser.add_argument('--after-save-seconds',type=float,default=10)
    parser.add_argument('--continuation-seconds',type=float,default=60)
    parser.add_argument('--timeout',type=float,default=3600)
    parser.add_argument('--width',type=int,default=1920)
    parser.add_argument('--height',type=int,default=1080)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without optimization')
    need(all(math.isfinite(value) for value in
         (args.warmup,args.before_save_seconds,args.after_save_seconds,args.continuation_seconds,args.timeout)) and
         0 <= args.gpu <= 4095 and (args.width,args.height) == (1920,1080), 'Invalid common bounds')
    if args.rehearsal:
        need(1 <= args.cycles <= 4 and 10 <= args.warmup <= 30 and
             10 <= args.before_save_seconds <= 30 and 5 <= args.after_save_seconds <= 15 and
             30 <= args.continuation_seconds <= 60 and 180 <= args.timeout <= 900,
             'Arguments exceed explicitly unqualified rehearsal bounds')
    else:
        need(40 <= args.cycles <= 64 and 30 <= args.warmup <= 120 and
             35 <= args.before_save_seconds <= 60 and 10 <= args.after_save_seconds <= 30 and
             60 <= args.continuation_seconds <= 120 and 2400 <= args.timeout <= 3600,
             'Arguments exceed qualification bounds')
    for key in ('binary','world','descriptor','workload_manifest'):
        setattr(args,key,getattr(args,key).resolve(strict=True))
    args.output = args.output.resolve()
    need(not args.output.exists() and not any(args.output.is_relative_to(path) for path in
         (args.descriptor.parent,Path(str(args.world)+'.assets'))), 'Output must be new and outside supplied closures')
    args.output.mkdir(parents=True)
    (args.output/'saves').mkdir()
    args.windows_interop, args.timeout_requested = False, args.timeout
    args.timeout = 30  # shared.Client per-request timeout, not whole-run deadline.
    record = dict(passed=False,scope='Native AOT Windows real-clock small-game reliability cohort',
        calls=None,owners=[],cycles=[],initialization_ticks=[],rehearsal=args.rehearsal,started_monotonic=time.monotonic(),
        limitations=['No compatible CoreCLR script reload or scene edit/application qualification.',
            'Warm supplied assets/caches; not source-free export, clean-machine, AAA, or Alpha acceptance.',
            'Native compiled controller commands, actual Vulkan/WASAPI reports; no physical input/listening proof.',
            'Present-return cadence is not scanout FPS or click-to-photon latency; no frame generation.',
            'Memory is one owning process working set/private commit; no WDDM VRAM or editor combined budget.',
            'Memory growth must be reviewed separately; this check does not equate bounded samples with absence of leaks.'])
    log = RpcLog(args.output/'rpc.jsonl.gz');record['calls'] = log
    shared.ARGS, shared.RECORD = args, record
    owned, game, original_pins = [], None, None
    expired, watchdog_stop = threading.Event(), threading.Event()
    deadline = record['started_monotonic']+args.timeout_requested

    def watchdog():
        while not watchdog_stop.wait(.5):
            if time.monotonic() >= deadline:
                expired.set()
                for owner in list(owned):
                    if owner.process.poll() is None:
                        owner.record['whole_deadline_forced_kill'] = True
                        owner.process.kill()  # Exact owned handles; never global process-name cleanup.
                return

    watcher = threading.Thread(target=watchdog,daemon=True);watcher.start()
    try:
        descriptor = json.loads(args.descriptor.read_text(encoding='utf-8'))
        manifest = json.loads(args.workload_manifest.read_text(encoding='utf-8'))
        document = json.loads(args.world.read_text(encoding='utf-8'))
        need(descriptor['format']=='poima.native-gameplay' and descriptor['version']==2 and
             descriptor['identity']=='poima.examples.performance-yard' and descriptor['target_os']=='Windows' and
             descriptor['target_arch']=='x86_64' and manifest['format']=='poima.performance-yard' and
             manifest['version']==1 and manifest['prepared_world_sha256']==shared.sha(args.world), 'Input identities differ')
        need(len(manifest['ids']['rigs'])==24 and len(manifest['ids']['props'])==768 and
             len(manifest['ids']['dynamics'])==32 and len(manifest['ids']['lights'])==64 and
             document['entities'][shared.uid(100)]['components']['game:'+PERFORMANCE][shared.uid(1)]==1,
             'Supply the complete enabled representative autonomous workload')
        artifact = inventory(args.descriptor.parent)
        names = {'native-gameplay.json'}
        for row in descriptor['files']:
            name = row['path']
            need(name in artifact and artifact[name]==dict(bytes=row['size'],sha256=row['sha256']),
                 'Actual NativeAOT artifact closure differs')
            names.add(name)
        need(set(artifact)==names, 'Artifact has an unlisted file')
        store = inventory(Path(str(args.world)+'.assets'))
        original_pins = dict(world=shared.sha(args.world),manifest=shared.sha(args.workload_manifest),
            descriptor=shared.sha(args.descriptor),artifact=artifact,store=store,binary=shared.sha(args.binary))
        selected_runtime = {name:shared.sha(args.binary.parent/name) for name in
                            ('phonon.dll','runtime.json','core_responses.v1.json') if (args.binary.parent/name).is_file()}
        need('phonon.dll' in selected_runtime, 'Staged native audio library is absent')
        original_pins['selected_runtime_files'] = selected_runtime
        record['inputs'] = original_pins
        record['authored_entity_ids'] = sorted(document['entities'])
        record['source_sha256'] = {str(path.relative_to(ROOT)).replace('\\','/'):shared.sha(path)
            for path in (Path(__file__),ROOT/'tests/performance_yard.py',ROOT/'tests/player_service_contract.py',
                         ROOT/'tests/player_performance.py',ROOT/'tools/performance/windows_process_memory.py')}
        world = args.output/'world.json'
        shutil.copy2(args.world,world)
        shutil.copytree(Path(str(args.world)+'.assets'),Path(str(world)+'.assets'))
        u = ctypes.WinDLL('user32',use_last_error=True)
        u.SetThreadDpiAwarenessContext.argtypes=[ctypes.c_void_p];u.SetThreadDpiAwarenessContext.restype=ctypes.c_void_p
        need(u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4)), 'Cannot establish per-monitor DPI context')
        game = Soak(args,record,world,descriptor,manifest,deadline,owned)
        game.start('soak')
        first_identity, first_pid = game.player_id,game.host.process.pid
        game.control('begin')
        game.run_for(args.warmup,'warmup',measured=False,frame_window=True)
        game.capture('warmup')
        initial_route = game.route()
        saved, save_pins, saved_pin = None,None,None
        for cycle in range(args.cycles):
            # Refresh reconciles the restored save ticket against the CURRENT
            # configured store before Close starts actual native-clock play.
            if cycle:
                game.control('refresh')
            state = game.player()
            if game.ui(state['tick'])['modal'] is None:
                game.rpc('player.control',shared.control_parameters(state,'resume'))
            else:
                game.control('close')
            _, route_before_save = game.run_for(args.before_save_seconds,f'cycle-{cycle+1}-before-save',
                                                frame_window=cycle%4==0)
            prior = initial_route if cycle==0 else record['cycles'][-1]['route_saved']
            need(route_before_save[shared.uid(10)]-prior[shared.uid(10)]>args.before_save_seconds and
                 int(route_before_save[shared.uid(14)])-int(prior[shared.uid(14)])>args.before_save_seconds*20 and
                 int(route_before_save[shared.uid(13)])>int(prior[shared.uid(13)]),
                 'Actual route movement/navigation stalled between checkpoints')
            game.control('menu')
            save_reply, save_params = game.control('save')
            saved = game.snapshot()
            saved_pin, save_pins = durable(args.output/'saves',cycle+1)
            need(saved['runtime']['tick']==saved_pin['saved_tick'], 'Saved native snapshot tick differs')
            snapshot_path = args.output/(f'checkpoint-{cycle+1:02d}.json.gz')
            gzip_write(snapshot_path,saved)
            # Retry only retained immediate receipts; never rely on ancient
            # history after >32 load/save turns.
            retry = game.rpc('runtime.ui.activate',copy.deepcopy(save_params))
            need(retry==dict(save_reply,replayed=True) and inventory(args.output/'saves')==save_pins and game.snapshot()==saved,
                 'Immediate Save retry invoked callback or changed durable bytes')
            if cycle==0:
                for action in ('fov.plus','ui.plus','gain.minus'):
                    game.control(action)
            current_preferences = game.preferences()
            game.control('close')
            _, discarded_route = game.run_for(args.after_save_seconds,f'cycle-{cycle+1}-discarded-play')
            need(discarded_route[shared.uid(10)]-route_before_save[shared.uid(10)]>args.after_save_seconds and
                 int(discarded_route[shared.uid(14)])-int(route_before_save[shared.uid(14)])>args.after_save_seconds*20,
                 'No sustained real controller progress existed to restore')
            game.control('menu')
            load_reply, load_params = game.control('load')
            # First post-load domain read: no Refresh, Control or Tick may
            # change the restored checkpoint before exact observation.
            actual = game.snapshot()
            need(actual==saved, 'Same-window complete exact state restoration differs')
            need(inventory(args.output/'saves')==save_pins and
                 preference_identity(game.preferences())==preference_identity(current_preferences),
                 'Load changed durable checkpoint or current independent preferences')
            need(game.player()['report']['runtime_replacements']==cycle+1 and game.player_id==first_identity,
                 'Same-window load did not retain owner through the replacement seam')
            replay = game.rpc('runtime.ui.activate',copy.deepcopy(load_params))
            need(replay==dict(load_reply,replayed=True) and game.replacements==cycle+1 and
                 game.snapshot()==saved and inventory(args.output/'saves')==save_pins,
                 'Immediate Load retry repeated replacement or changed committed state')
            record['cycles'].append(dict(cycle=cycle+1,checkpoint=snapshot_path.name,
                checkpoint_sha256=shared.sha(snapshot_path),saved=saved_pin,exact_restore=True,
                player_id=game.player_id,replacements=game.replacements,route_saved=route_before_save,
                discarded_route=discarded_route,preferences=current_preferences))
            if cycle in (0,31,args.cycles-1):
                game.capture(f'load-{cycle+1:02d}')
            log.flush_pending()
            progress=args.output/'progress.json';temporary=args.output/'progress.tmp'
            temporary.write_text(json.dumps(dict(cycles=cycle+1,total=args.cycles,
                credited_unpaused_seconds=sum(row['credited_unpaused_seconds'] for row in record['play_segments']
                                             if row['measured'] and row['owner']=='soak'),elapsed=time.monotonic()-record['started_monotonic'])))
            os.replace(temporary,progress)
        credited=sum(row['credited_unpaused_seconds'] for row in record['play_segments']
                     if row['measured'] and row['owner']=='soak')
        need(credited >= (args.cycles*(args.before_save_seconds+args.after_save_seconds) if args.rehearsal else 1800) and
             game.replacements >= (args.cycles if args.rehearsal else 40),
             'True unpaused duration/replacement gate was not reached')
        record['soak_credited_unpaused_seconds']=credited
        first_preferences=game.preferences()
        need(first_preferences['settings']['revision']>=3 and
             first_preferences['settings']['values']['camera.vertical_fov']==65 and
             first_preferences['settings']['values']['ui.scale']==1.25 and
             abs(first_preferences['settings']['values']['audio.master_gain']-.9)<1e-12,
             'Compiled menu did not retain the independently changed current preferences')
        game.close()
        need(inventory(args.output/'saves')==save_pins, 'Shutdown changed durable save bytes')
        game.start('fresh')
        fresh_preferences=game.preferences()
        need(game.host.process.pid!=first_pid and game.player_id!=first_identity and
             fresh_preferences['settings']['revision']==0, 'Fresh process reused public owner/preferences revision')
        for key,value in {'camera.vertical_fov':60,'ui.scale':1,'audio.master_gain':1}.items():
            need(fresh_preferences['settings']['values'][key]==value, 'Fresh owner defaults differ')
        game.control('load')  # Eligible Welcome Load reflects the same compiled action.
        need(game.snapshot()==saved and preference_identity(game.preferences())==preference_identity(fresh_preferences) and
             inventory(args.output/'saves')==save_pins,
             'Fresh actual process did not restore exact checkpoint/current preference separation')
        game.capture('fresh-exact-restore')
        record['fresh_continuation']=dict(exact_state_restored=True,first_player_id=first_identity,
            fresh_player_id=game.player_id,first_process_id=first_pid,fresh_process_id=game.host.process.pid,
            first_preferences=first_preferences,fresh_preferences=fresh_preferences,checkpoint=saved_pin,
            different_native_image_loaded=False,in_process_code_reload_qualified=False,hidden_preference_epochs_observed=False)
        fresh_route = game.route()
        game.control('refresh');game.control('close')
        _, continued_route = game.run_for(args.continuation_seconds,'fresh-continuation',measured=False,frame_window=True)
        need(continued_route[shared.uid(10)]-fresh_route[shared.uid(10)]>args.continuation_seconds and
             int(continued_route[shared.uid(14)])-int(fresh_route[shared.uid(14)])>args.continuation_seconds*20 and
             int(continued_route[shared.uid(13)])>int(fresh_route[shared.uid(13)]) and
             int(continued_route[shared.uid(3)])>int(fresh_route[shared.uid(3)]),
             'Fresh restored compiled controller did not genuinely navigate another full circuit')
        record['fresh_continuation']['route_before']=fresh_route
        record['fresh_continuation']['route_after']=continued_route
        game.capture('fresh-continued')
        game.close();game=None
        need(inventory(args.output/'saves')==save_pins, 'Continuation rewrote durable checkpoint')
        need(shared.sha(world)==original_pins['world'] and inventory(Path(str(world)+'.assets'))==original_pins['store'],
             'Soak mutated the authored working copy/cooked asset closure')
        need(shared.sha(args.world)==original_pins['world'] and shared.sha(args.workload_manifest)==original_pins['manifest'] and
             shared.sha(args.descriptor)==original_pins['descriptor'] and inventory(args.descriptor.parent)==original_pins['artifact'] and
             inventory(Path(str(args.world)+'.assets'))==original_pins['store'] and shared.sha(args.binary)==original_pins['binary'],
             'Immutable supplied source/artifact/runtime inputs changed')
        need(all(shared.sha(args.binary.parent/name)==pin for name,pin in selected_runtime.items()),
             'Selected runtime dependency changed')
        need(all(shared.sha(ROOT/name)==pin for name,pin in record['source_sha256'].items()),
             'Verifier helper source changed during the cohort')
        memory_data=json.loads((args.output/'soak-memory.json').read_text(encoding='utf-8'))
        samples=memory_data['samples']
        need(samples[-1]['elapsed_seconds']-samples[0]['elapsed_seconds'] >= (credited if args.rehearsal else 1800),
             'Memory observation did not span the measured play')
        record['memory_budget_screen']=dict(working_set_4GB=memory_data['summary']['working_set_bytes']['maximum']<=4_000_000_000,
            private_commit_4GB=memory_data['summary']['private_commit_bytes']['maximum']<=4_000_000_000,
            vram_qualified=False,continuous_growth_review_required=True)
        def medians(field):
            bins={}
            for row in samples:
                bins.setdefault(int(row['elapsed_seconds']//60),[]).append(row[field])
            series=[dict(minute=key,median_bytes=statistics.median(values)) for key,values in sorted(bins.items()) if len(values)>=30]
            early=[row[field] for row in samples if
                   (samples[0]['elapsed_seconds']<=row['elapsed_seconds']<=samples[-1]['elapsed_seconds']/2
                    if args.rehearsal else 120<=row['elapsed_seconds']<=420)]
            late=[row[field] for row in samples if row['elapsed_seconds']>=samples[-1]['elapsed_seconds']-300]
            return dict(per_minute=series,late_minus_early_median_bytes=statistics.median(late)-statistics.median(early))
        record['memory_growth_observations'] = (dict(status='insufficient_duration',qualified=False) if args.rehearsal else
            {key:medians(key) for key in ('working_set_bytes','private_commit_bytes')})
        record['reliability_duration_requirement_met'] = not args.rehearsal and credited >= 1800
        record['beyond_32_replacements_qualified'] = not args.rehearsal and args.cycles > 32
        record['functional_reliability_passed']=not args.rehearsal
        record['harness_rehearsal_passed']=args.rehearsal
        record['alpha_gate_closed']=False
        record['passed']=True
    except BaseException:
        record['error']=traceback.format_exc()
    finally:
        # Failure snapshots and actual child cleanup remain in this NEW output.
        # A failed shutdown never becomes a successful qualification.
        if game:
            try:game.close()
            except BaseException:record['cleanup_error']=traceback.format_exc()
        for owner in reversed(owned):
            try:owner.close(expected=False)
            except BaseException:record.setdefault('owner_cleanup_errors',[]).append(traceback.format_exc())
        watchdog_stop.set();watcher.join(2)
        record['whole_deadline_expired']=expired.is_set()
        record['elapsed_seconds']=time.monotonic()-record['started_monotonic']
        record['passed'] &= not expired.is_set() and 'cleanup_error' not in record and not record.get('owner_cleanup_errors')
        record['passed'] &= all(row['exit_code']==0 and not row.get('stderr_truncated') and
                                not row.get('whole_deadline_forced_kill') for row in record['owners'])
        record['passed'] &= all(owner.record['stderr']==
            ('Poima shared world ready: '+owner.marker+'\n' if owner.role=='serve' else '') for owner in owned)
        record['functional_reliability_passed'] = bool(record['passed'] and not args.rehearsal)
        record['harness_rehearsal_passed'] = bool(record['passed'] and args.rehearsal)
        try:log.close()
        except BaseException:record['rpc_log_error']=traceback.format_exc();record['passed']=False
        record['calls']=dict(file='rpc.jsonl.gz',count=log.count,uncompressed_bytes=log.bytes,
                             sha256=shared.sha(args.output/'rpc.jsonl.gz'))
        record['functional_reliability_passed'] &= record['passed']
        record['harness_rehearsal_passed'] &= record['passed']
        json_write(args.output/'evidence.json',record)
        print(json.dumps(dict(passed=record['passed'],output=str(args.output),error=record.get('error'))),flush=True)
    return 0 if record['passed'] else 1


if __name__=='__main__':
    sys.exit(main())
