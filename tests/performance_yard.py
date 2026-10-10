#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure Performance Yard on the actual interactive Windows player clock.

Run after examples/performance-yard/prepare.py and native gameplay publication.
The compiled circuit owns movement; no replay or per-tick RPC drives simulation.
This preserves missed budgets as results, rather than a successful FPS claim.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import threading
import time
import traceback
import uuid

import player_service_contract as shared
from player_performance import verify_capture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/performance'))
import windows_process_memory as memory


def machine_observation(hwnd):
    """Observe the owning window's OS display mode and power before timing."""
    class Power(ctypes.Structure):
        _fields_ = [('ac', w.BYTE), ('flags', w.BYTE), ('percent', w.BYTE), ('reserved', w.BYTE),
                    ('lifetime', w.DWORD), ('full_lifetime', w.DWORD)]

    class Monitor(ctypes.Structure):
        _fields_ = [('size', w.DWORD), ('bounds', w.RECT), ('work', w.RECT),
                    ('flags', w.DWORD), ('device', w.WCHAR*32)]

    class Mode(ctypes.Structure):
        _fields_ = [('device', w.WCHAR*32), ('spec', w.WORD), ('driver', w.WORD),
            ('size', w.WORD), ('extra', w.WORD), ('fields', w.DWORD), ('position_union', w.BYTE*16),
            ('color', w.SHORT), ('duplex', w.SHORT), ('y_resolution', w.SHORT), ('tt', w.SHORT),
            ('collate', w.SHORT), ('form', w.WCHAR*32), ('log_pixels', w.WORD),
            ('bits', w.DWORD), ('width', w.DWORD), ('height', w.DWORD), ('display_flags', w.DWORD),
            ('frequency', w.DWORD), ('icm_method', w.DWORD), ('icm_intent', w.DWORD),
            ('media', w.DWORD), ('dither', w.DWORD), ('reserved1', w.DWORD), ('reserved2', w.DWORD),
            ('panning_width', w.DWORD), ('panning_height', w.DWORD)]

    assert ctypes.sizeof(Power) == 12 and ctypes.sizeof(Monitor) == 104 and ctypes.sizeof(Mode) == 220
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetSystemPowerStatus.argtypes = [ctypes.POINTER(Power)]
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.MonitorFromWindow.argtypes = [w.HWND, w.DWORD]
    user.MonitorFromWindow.restype = w.HANDLE
    user.GetMonitorInfoW.argtypes = [w.HANDLE, ctypes.POINTER(Monitor)]
    user.EnumDisplaySettingsW.argtypes = [w.LPCWSTR, w.DWORD, ctypes.POINTER(Mode)]
    power, monitor, mode = Power(), Monitor(), Mode()
    monitor.size, mode.size = ctypes.sizeof(monitor), ctypes.sizeof(mode)
    assert kernel.GetSystemPowerStatus(ctypes.byref(power)), ctypes.get_last_error()
    assert user.GetMonitorInfoW(user.MonitorFromWindow(hwnd, 2), ctypes.byref(monitor)), ctypes.get_last_error()
    assert user.EnumDisplaySettingsW(monitor.device, 0xffffffff, ctypes.byref(mode)), ctypes.get_last_error()
    return dict(os=platform.win32_ver(), ac_line_status=int(power.ac),
        power_source={0:'battery', 1:'AC', 255:'unknown'}.get(power.ac, 'unknown'),
        battery_percent=int(power.percent) if power.percent <= 100 else None,
        display=dict(device=monitor.device, mode_width=int(mode.width), mode_height=int(mode.height),
            raw_frequency=int(mode.frequency), nominal_hz=int(mode.frequency) if mode.frequency > 1 else None,
            variable_refresh_qualified=False,
            explanation='Current OS mode of the owning window monitor; not measured scanout cadence.'),
        cache_state='Existing engine shader/package caches; fresh native process and cold scene upload. No cache reset.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'world', 'descriptor', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--warmup', type=float, default=30)
    parser.add_argument('--duration', type=float, default=60)
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='deferred')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Run with native Windows Python for actual HWND/process identity.')
    if not 1 <= args.warmup <= 300 or not 30 <= args.duration <= 1800:
        parser.error('Warmup must be 1..300 seconds; measurement 30..1800 seconds.')
    if not 640 <= args.width <= 3840 or not 480 <= args.height <= 2160:
        parser.error('Use a bounded player extent.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    args.windows_interop = False
    args.timeout = 30
    world = args.output/'world.json'
    shutil.copy2(args.world, world)
    shutil.copytree(Path(str(args.world)+'.assets'), Path(str(world)+'.assets'))
    baseline = world.read_bytes()
    record = dict(passed=False, scope='Interactive compiled small-game workload; not AAA or release qualification',
                  calls=[], owners=[], observations=[], binary_sha256=shared.sha(args.binary),
                  world_sha256=shared.sha(world), descriptor_sha256=shared.sha(args.descriptor),
                  test_sha256=shared.sha(Path(__file__)), settings=vars(args).copy())
    record['settings'] = {k: str(v) if isinstance(v, Path) else v for k, v in record['settings'].items()}
    shared.ARGS, shared.RECORD = args, record
    endpoint = 'performance-yard-'+uuid.uuid4().hex
    sid, cap = uuid.uuid4().hex, uuid.uuid4().hex
    host = client = pool = sampling = None
    stop_sampling = threading.Event()

    def sample_sleep(seconds):
        if stop_sampling.wait(seconds):
            raise KeyboardInterrupt  # collect retains bounded partial observations.
    u = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback, w.LPARAM]
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.GetForegroundWindow.restype = w.HWND
    u.SetForegroundWindow.argtypes = [w.HWND]
    u.SetForegroundWindow.restype = w.BOOL
    u.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    u.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    u.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    assert u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))

    def observe():
        state = client.call('player.inspect')
        record['observations'].append(dict(elapsed=time.monotonic()-started, state=state))
        assert state['active'] and state['ready'], state
        return state

    def pause():
        state = client.call('player.inspect')
        if state['active'] and not state['paused']:
            client.call('player.control', shared.control_parameters(state, 'pause'))
        state = client.call('player.inspect')
        assert state['active'] and state['paused'], state
        return state

    def snapshot(label):
        state = pause()
        tick = state['tick']
        result = dict(player=state, runtime=client.call('runtime.inspect', dict(session_id=sid)),
                      gameplay=client.call('runtime.gameplay.inspect', dict(session_id=sid, tick=tick, include_schema=True)),
                      route=client.call('runtime.component.get', dict(session_id=sid, tick=tick, id=shared.uid(100),
                          type='c0940000000000000000000000000001')),
                      controller=client.call('runtime.entity', dict(session_id=sid, tick=tick, id=shared.uid(100))))
        image = args.output/(label+'.bmp')
        result['capture'] = client.call('player.capture', dict(player_id=state['player_id'],
            request_id=uuid.uuid4().hex, expected_control_revision=state['control_revision'],
            session_id=sid, tick=tick, expected_structure_revision=state['structure_revision'], path=str(image)))
        result['image_sha256'] = shared.sha(image)
        (args.output/(label+'.json')).write_text(json.dumps(result, indent=2))
        return result

    try:
        host = shared.ProcessOwner([str(args.binary.resolve()), 'serve', str(world), '--endpoint', endpoint],
                                   'yard-owner', endpoint, 'serve')
        client = shared.Client(endpoint, 'yard-client')
        discovery = client.call('world.describe')
        record['build'] = discovery.get('build')
        info = client.call('world.inspect')
        record['world'] = info
        client.call('runtime.start', dict(session_id=sid, revision=info['revision']))
        module = client.call('runtime.gameplay.inspect', dict(session_id=sid, tick=0))
        client.call('runtime.gameplay.load_native', dict(session_id=sid, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=module['revision'], descriptor=str(args.descriptor.resolve())))
        client.call('runtime.step', dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1))
        state = client.call('runtime.inspect', dict(session_id=sid))
        module = client.call('runtime.gameplay.inspect', dict(session_id=sid, tick=1))
        client.call('runtime.ui.activate', dict(session_id=sid, request_id=uuid.uuid4().hex,
            expected_tick=1, expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=module['revision'], expected_structure_revision=state['structure_revision'],
            id=shared.uid(911)))
        params = shared.start_parameters(sid, expected_tick=1, width=args.width, height=args.height,
                    gpu=args.gpu, frames_in_flight=2, audio=True, lighting_path=args.lighting_path,
                    expected_structure_revision=state['structure_revision'],
                    ambient_occlusion=dict(mode='gtao' if args.lighting_path == 'deferred' else 'none', quality='medium', radius=1))
        params.pop('controller')  # The compiled circuit is this controller's sole input authority.
        client.call('player.start', params)
        ready = shared.await_state(client, lambda s: s['ready'], 'ready')
        windows = []

        @callback
        def visit(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            title = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, title, 256)
            if pid.value == host.process.pid and title.value.startswith('Poima player'):
                windows.append(hwnd)
            return True

        u.EnumWindows(visit, 0)
        assert len(windows) == 1, windows
        hwnd = windows[0]
        # A real foreground request, scoped to this owner's window. Never fake focus flags.
        record['foreground_request_accepted'] = bool(u.SetForegroundWindow(hwnd))
        record['foreground_observed'] = u.GetForegroundWindow() == hwnd
        rect = w.RECT()
        assert u.GetClientRect(hwnd, ctypes.byref(rect))
        record['client_extent'] = [rect.right-rect.left, rect.bottom-rect.top]
        assert record['foreground_observed'], 'Owned player window did not acquire actual foreground focus.'
        record['machine'] = machine_observation(hwnd)
        started = time.monotonic()
        record['before'] = snapshot('before')
        ready = client.call('player.inspect')
        client.call('player.control', shared.control_parameters(ready, 'resume'))
        warm_deadline = time.monotonic()+args.warmup
        while time.monotonic() < warm_deadline:
            time.sleep(min(1, max(0, warm_deadline-time.monotonic())))
            assert not observe()['paused'], 'Workload paused during warmup.'
        record['warmup'] = snapshot('warmup')
        ready = client.call('player.inspect')
        client.call('player.control', shared.control_parameters(ready, 'resume'))
        # Include the resume transition in raw rows; the service flags it explicitly.
        client.call('performance.start', dict(capture_id=cap, expected_capture_id=None,
                    player_id=ready['player_id'], capacity=262144))
        provider = memory.WindowsProvider(memory.Options(host.process.pid))
        identity = provider.creation_filetime
        provider.close()
        pool = ThreadPoolExecutor(max_workers=1)
        sampling = pool.submit(memory.collect, memory.Options(host.process.pid, duration=args.duration,
            interval=.2, system_ram=True, expected_creation_filetime=identity), sleep=sample_sleep)
        measured_start = time.monotonic()
        deadline = measured_start+args.duration
        while time.monotonic() < deadline:
            time.sleep(min(5, max(0, deadline-time.monotonic())))
            current = observe()
            assert not current['paused'], 'Workload paused during measurement.'
        client.call('performance.stop', dict(capture_id=cap))
        record['measured_wall_seconds'] = time.monotonic()-measured_start
        record['after'] = snapshot('after')
        capture = verify_capture(client, cap)
        (args.output/'frames.json').write_text(json.dumps(capture, indent=2))
        record['capture_sha256'] = shared.sha(args.output/'frames.json')
        summary, rows = capture['summary'], capture['rows']
        assert not summary['full'] and summary['frames_dropped'] == 0 and summary['frames_rejected'] == 0
        assert all(row['session'] == sid and [row['width'], row['height']] == [args.width, args.height] for row in rows)
        assert all(row['flags'] & 1 for row in rows), 'Player lost actual foreground focus.'
        assert all(not row['flags'] & (2|4|8|16|32|64|128|256) for row in rows[2:]), 'Unexpected workload intervention.'
        assert summary['present_return_cadence_eligible']['samples'] >= 10, 'No eligible interactive cadence.'
        tick_delta = rows[-1]['tick_after']-rows[0]['tick_before']
        wall = (rows[-1]['end_ns']-rows[0]['begin_ns'])/1e9
        record['interactive_clock'] = dict(ticks=tick_delta, wall_seconds=wall, ticks_per_second=tick_delta/wall,
            max_ticks_per_poll=max(r['tick_after']-r['tick_before'] for r in rows))
        assert all(0 <= r['tick_after']-r['tick_before'] <= 8 for r in rows)
        assert all(b['tick_before'] == a['tick_after'] for a, b in zip(rows, rows[1:])), 'Unobserved simulation tick.'
        assert 58 <= record['interactive_clock']['ticks_per_second'] <= 62, 'Interactive clock did not sustain 60 Hz.'
        assert record['after']['player']['report']['dropped_wall_seconds'] == record['warmup']['player']['report']['dropped_wall_seconds'], 'Simulation dropped measured wall time.'
        initial_route, final_route = record['warmup']['route']['values'], record['after']['route']['values']
        field = shared.uid
        assert initial_route[field(1)] == final_route[field(1)] == 1
        assert final_route[field(10)]-initial_route[field(10)] > args.duration, 'Native circuit did not travel.'
        assert int(final_route[field(14)])-int(initial_route[field(14)]) > args.duration*20, 'Native circuit did not keep moving.'
        assert int(final_route[field(4)]) > int(initial_route[field(4)]) and int(final_route[field(12)]) > int(initial_route[field(12)])
        assert int(final_route[field(13)]) > int(initial_route[field(13)]), 'No new native navigation plan along route.'
        assert int(final_route[field(3)]) > int(initial_route[field(3)]), 'No complete circuit traversed.'
        assert final_route[field(15)] == 0 and final_route[field(18)] == 1, 'Native path did not reach its initial corner.'
        record['route_progress'] = dict(before=initial_route, after=final_route)
        completed = [r['state']['report']['render_diagnostics'] for r in record['observations']
                     if r['state'].get('report') and r['state']['report']['render_diagnostics']['completed_submissions'] > 0]
        assert completed
        assert all(d['last_draws']['skinned_instances'] >= 24 and d['last_draws']['skinned_vertices'] > 0 and
                   d['last_draws']['objects'] >= 768 and d['light_assignment']['light_count'] == 64 and
                   d['last_draws']['shadow_views'] > 0 for d in completed), 'Declared rendered workload was absent.'
        assert max(d['last_draws']['camera_draws'] for d in completed) >= 100, 'Route did not show sufficient geometry.'
        record['completed_workload_observations'] = [dict(completed_submissions=d['completed_submissions'],
             last_draws=d['last_draws'], light_assignment=d['light_assignment']) for d in completed]
        before = record['warmup']['controller']['world_matrix'][12:15]
        after = record['after']['controller']['world_matrix'][12:15]
        record['controller_positions'] = dict(before=before, after=after)
        cadence = summary['present_return_cadence_eligible']
        mem = sampling.result(timeout=15)
        assert mem['completed'] and mem['process']['creation_filetime_100ns'] == str(identity)
        (args.output/'memory.json').write_text(json.dumps(mem, indent=2))
        record['memory_sha256'] = shared.sha(args.output/'memory.json')
        record['summary'] = summary
        record['memory_summary'] = mem['summary']
        record['budgets'] = dict(p95_16_7ms=cadence['p95'] <= 16_700_000,
            p99_25ms=cadence['p99'] <= 25_000_000,
            working_set_4GiB=mem['summary']['working_set_bytes']['maximum'] <= 4*1024**3, vram_qualified=False,
            explanation='Application present-return cadence, not scanout FPS. No frame generation.')
        state = client.call('player.inspect')
        client.call('player.control', shared.control_parameters(state, 'stop'))
        final = client.call('player.inspect')
        record['final'] = final
        assert final['report']['success'] and final['report']['nvrhi_errors'] == 0
        assert world.read_bytes() == baseline, 'Measurement changed authored content.'
        client.call('runtime.stop', dict(session_id=sid))
        client.call('host.shutdown')
        client.close(); client = None
        host.close(); host = None
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        stop_sampling.set()
        if sampling and not (args.output/'memory.json').exists():
            try:
                mem = sampling.result(timeout=15)
                (args.output/'memory.json').write_text(json.dumps(mem, indent=2))
                record['memory_sha256'] = shared.sha(args.output/'memory.json')
            except BaseException:
                record['memory_error'] = traceback.format_exc()
        if pool:
            pool.shutdown(wait=True)
        if client:
            try:
                state = client.call('player.inspect')
                if state['active']:
                    client.call('player.control', shared.control_parameters(state, 'stop'))
                client.call('host.shutdown')
            except BaseException:
                record['cleanup_error'] = traceback.format_exc()
            try:
                client.close(expected=False)
            except BaseException:
                record['client_close_error'] = traceback.format_exc()
        if host:
            try:
                host.close(expected=record['passed'])
            except BaseException:
                record['host_close_error'] = traceback.format_exc()
        record['passed'] &= not any(k in record for k in ('cleanup_error','client_close_error','host_close_error','memory_error'))
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2))
        print(json.dumps(dict(passed=record['passed'], output=str(args.output), error=record.get('error'))), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
