#!/usr/bin/env python3
"""Owned Windows HWND qualification for independent native Scene/Game panes."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import traceback
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bridge', type=Path, required=True)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--gpu', type=int, help='Enable actual dual-device Vulkan presentation.')
parser.add_argument('--samples', type=int, choices=[1, 4], default=4)
args = parser.parse_args()
if os.name != 'nt':
    parser.error('Use native Windows Python; all HWNDs belong to this test process.')
run = args.output.resolve()/('run-'+uuid.uuid4().hex)
run.mkdir(parents=True)
record = {'success': False, 'checks': [], 'calls': [], 'gpu': args.gpu, 'samples': args.samples,
          'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
          'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'limitations': ['Owned-HWND native ABI test; no physical input or Avalonia docking claim.']}
search = os.add_dll_directory(str(args.bridge.resolve().parent))
lib = ctypes.CDLL(str(args.bridge.resolve()))
VP, STR = ctypes.c_void_p, ctypes.c_char_p
for name, parameters, result in [
    ('create', [STR, STR, ctypes.c_int32, ctypes.c_uint32], VP),
    ('call', [VP, STR], VP), ('poll', [VP], VP), ('error', [VP], VP),
    ('attach', [VP, VP], ctypes.c_int), ('draw', [VP], ctypes.c_int),
    ('attach_view', [VP, STR, VP], ctypes.c_int), ('draw_view', [VP, STR], ctypes.c_int),
    ('detach_view', [VP, STR], None), ('destroy', [VP], None)]:
    function = getattr(lib, 'poima_desktop_'+name)
    function.argtypes, function.restype = parameters, result
u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.GetModuleHandleW.argtypes, k.GetModuleHandleW.restype = [wintypes.LPCWSTR], wintypes.HMODULE
u.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                            wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, VP]
u.CreateWindowExW.restype = wintypes.HWND
u.DestroyWindow.argtypes, u.DestroyWindow.restype = [wintypes.HWND], wintypes.BOOL
u.IsWindow.argtypes, u.IsWindow.restype = [wintypes.HWND], wintypes.BOOL
u.ShowWindow.argtypes, u.ShowWindow.restype = [wintypes.HWND, ctypes.c_int], wintypes.BOOL
u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
u.SetWindowPos.restype = wintypes.BOOL
u.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
u.PeekMessageW.restype = wintypes.BOOL
u.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
u.DispatchMessageW.argtypes, u.DispatchMessageW.restype = [ctypes.POINTER(wintypes.MSG)], ctypes.c_ssize_t
host = None
windows = []
sequence = 0

def check(ok, detail):
    if not ok:
        raise AssertionError(detail)

def decode(pointer):
    return ctypes.string_at(pointer).decode() if pointer else ''

def error():
    return decode(lib.poima_desktop_error(host))

def rpc(method, params=None, expected=None):
    global sequence
    sequence += 1
    request = {'jsonrpc': '2.0', 'id': sequence, 'method': method, 'params': params or {}}
    raw = lib.poima_desktop_call(host, json.dumps(request).encode())
    check(raw, (method, error()))
    response = json.loads(decode(raw))
    record['calls'].append({'request': request, 'response': response})
    if expected is not None:
        check(response.get('error', {}).get('code') == expected, response)
        return response['error']
    check('result' in response, response)
    return response['result']

def state():
    return rpc('desktop.inspect')

def pump_windows():
    message = wintypes.MSG()
    for _ in range(256):
        if not u.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            break
        u.TranslateMessage(ctypes.byref(message))
        u.DispatchMessageW(ctypes.byref(message))

def create_window(parent=None, x=0, y=0, width=400, height=300):
    style = (0x40000000 | 0x04000000 | 0x02000000) if parent else (0x00CF0000 | 0x02000000)
    if args.gpu is not None:
        style |= 0x10000000
    hwnd = u.CreateWindowExW(0, 'STATIC', 'Poima owned dual-view qualification', style,
                           x, y, width, height, parent, None, k.GetModuleHandleW(None), None)
    check(hwnd, ctypes.get_last_error())
    windows.append(hwnd)
    return hwnd

def attach(view, hwnd):
    check(lib.poima_desktop_attach_view(host, view.encode(), hwnd) == 1, (view, error()))

def detach(view):
    lib.poima_desktop_detach_view(host, view.encode())
    check(not error(), error())

def draw(view, allow_zero=False):
    pump_windows()
    value = lib.poima_desktop_draw_view(host, view.encode())
    check(value >= 0, (view, error(), state()))
    if not allow_zero:
        check(value == 1, (view, value, state()))
    return value

def capture(view, label, mutate_other=None):
    path = run/(label+'.bmp')
    job = rpc('desktop.capture', {'revision': state()['revision'], 'path': str(path), 'view': view})
    if mutate_other:
        mutate_other()
    deadline = time.monotonic()+1.8
    while time.monotonic() < deadline:
        draw(view, True)
        result = rpc('desktop.capture.status', {'capture_id': job['capture_id']})
        if result['state'] != 'queued':
            break
        time.sleep(.01)
    check(result['state'] == 'complete', result)
    check(path.exists() and path.read_bytes()[:2] == b'BM', str(path))
    check(result['result']['view']['mode'] == view, result)
    record.setdefault('captures', {})[label] = result['result']
    return result['result']

try:
    project_dir = run/'project'
    process = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project_dir), '--name', 'Dual viewport qualification'], capture_output=True, text=True)
    check(process.returncode == 0, process.stdout+process.stderr)
    manifest = json.loads((project_dir/'project.json').read_text())
    world = project_dir/manifest['entry']['world']
    camera, controller = manifest['entry']['camera'], manifest['entry']['controller']
    original = world.read_bytes()
    host = lib.poima_desktop_create(str(world).encode(), b'', args.gpu if args.gpu is not None else -1, args.samples)
    check(host, error())
    check('desktop.game.camera' in rpc('desktop.describe')['methods'], 'Missing camera discovery')
    rpc('desktop.game.camera', {'camera': None})
    rpc('desktop.game.camera', {'camera': 'invalid'}, -32602)
    parent = create_window(x=30, y=30, width=860, height=390)
    scene_hwnd = create_window(parent, 10, 10, 400, 300)
    game_hwnd = create_window(parent, 420, 10, 400, 300)
    attach('scene', scene_hwnd)
    check(lib.poima_desktop_attach_view(host, b'game', scene_hwnd) == 0, 'Duplicate HWND accepted')
    check(not state()['views']['game']['attached'], 'Duplicate changed Game state')
    attach('game', game_hwnd)
    check(lib.poima_desktop_attach(host, scene_hwnd) == 0, 'Legacy/named mixing accepted')
    check(lib.poima_desktop_draw(host) == -1, 'Legacy draw accepted in named mode')
    rpc('desktop.view', {'mode': 'scene'}, -32009)
    check(draw('game', True) == 0, 'No-camera Game unexpectedly presented')
    check(state()['views']['game']['preparation_error'] and state()['views']['game']['graphics_error'] is None, state())
    rpc('desktop.game.camera', {'camera': camera})
    rpc('desktop.camera', {'position': [6, 4, 8], 'yaw': 25, 'pitch': -15})
    record['checks'].append('lazy_slots_distinct_hwnds_legacy_exclusion_and_missing_camera_recovery')

    if args.gpu is not None:
        for _ in range(3):
            draw('scene'); draw('game')
        initial = state()
        check(all(initial['views'][view]['render']['nvrhi_errors'] == 0 for view in ['scene', 'game']), initial)
        first_scene = capture('scene', 'scene-initial')
        first_game = capture('game', 'game-initial')
        check((run/'scene-initial.bmp').read_bytes() != (run/'game-initial.bmp').read_bytes(), 'Distinct cameras produced identical captures')
        check(first_scene['camera_world'] != first_game['camera_world'], 'Camera metadata coupled')
        record['checks'].append('simultaneous_alternating_device_presentation_distinct_camera_pixels')

        job = rpc('desktop.capture', {'revision': state()['revision'], 'path': str(run/'game-independent.bmp'), 'view': 'game'})
        rpc('desktop.capture', {'revision': state()['revision'], 'path': str(run/'capacity-rejected.bmp'), 'view': 'scene'}, -32009)
        rpc('desktop.camera', {'yaw': 35})
        rpc('desktop.select', {'id': controller})
        draw('scene')
        check(rpc('desktop.capture.status', {'capture_id': job['capture_id']})['state'] == 'queued', 'Scene invalidated/completed Game capture')
        draw('game')
        check(rpc('desktop.capture.status', {'capture_id': job['capture_id']})['state'] == 'complete', 'Target did not complete capture')
        capture('scene', 'scene-independent', lambda: rpc('desktop.game.camera', {'camera': None}))
        rpc('desktop.game.camera', {'camera': camera})
        record['checks'].append('capture_capacity_and_target_only_completion_camera_selection_guards')

    session = uuid.uuid4().hex
    rpc('desktop.play.start', {'revision': state()['revision'], 'session_id': session})
    rpc('desktop.input.configure', {'session_id': session, 'controller': controller})
    rpc('desktop.input.focus', {'session_id': session, 'focused': True})
    rpc('desktop.input.events', {'session_id': session, 'request_id': uuid.uuid4().hex,
                                'events': [{'control': 'key.w', 'down': True}, {'motion': [8, -3]}]})
    tick = state()['runtime']['tick']
    rpc('desktop.camera', {'yaw': 40})
    if args.gpu is not None:
        draw('scene'); draw('game'); draw('scene')
    check(state()['runtime']['tick'] == tick, 'Drawing advanced shared simulation')
    check(rpc('desktop.input.inspect')['focused'], 'Scene navigation cleared Game input')
    detach('scene')
    check(rpc('desktop.input.inspect')['focused'], 'Scene detach cleared Game input')
    attach('scene', scene_hwnd)
    time.sleep(.17)
    polled = json.loads(decode(lib.poima_desktop_poll(host)))
    check(0 < polled['runtime']['tick']-tick <= 8, polled)
    detach('game')
    unfocused = rpc('desktop.input.inspect')
    check(not unfocused['focused'] and all(v == 0 for v in unfocused['pending']['move']), unfocused)
    check(u.IsWindow(game_hwnd), 'Native detach destroyed frontend HWND')
    attach('game', game_hwnd)
    rpc('desktop.play.pause', {'session_id': session})
    record['checks'].append('one_poll_clock_draw_never_ticks_scene_lifecycle_preserves_game_input')

    if args.gpu is not None:
        for order in [('scene', 'game'), ('game', 'scene')]:
            draw('scene'); draw('game')
            detach(order[0]); draw(order[1]); detach(order[1])
            check(u.IsWindow(scene_hwnd) and u.IsWindow(game_hwnd), 'Viewport owned external HWND')
            attach(order[1], scene_hwnd if order[1] == 'scene' else game_hwnd)
            draw(order[1])
            attach(order[0], scene_hwnd if order[0] == 'scene' else game_hwnd)
            draw(order[0]); draw(order[1])
        record['checks'].append('device_and_shared_instance_teardown_recreation_in_both_orders')
        check(u.SetWindowPos(game_hwnd, None, 420, 10, 360, 260, 0x0004 | 0x0010), ctypes.get_last_error())
        resized = capture('game', 'game-resized')
        check((resized['width'], resized['height']) == (360, 260), resized)
        check(state()['views']['scene']['render']['width'] == 400, state())
        u.ShowWindow(game_hwnd, 0)
        check(draw('game', True) == 0, 'Hidden Game pane presented')
        draw('scene')
        u.ShowWindow(game_hwnd, 5)
        draw('game')
        record['checks'].append('independent_resize_hide_restore')

    rpc('desktop.play.stop', {'session_id': session})
    revision = state()['revision']
    rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex,
                          'ops': [{'op': 'component.remove', 'id': camera, 'type': 'Camera'},
                                  {'op': 'component.remove', 'id': controller, 'type': 'CharacterController'}]})
    check(lib.poima_desktop_draw_view(host, b'game') == -1, 'Deleted Game camera did not report recoverable error')
    failed = state()['views']['game']
    check(failed['preparation_error'] and failed['graphics_error'] is None, failed)
    if args.gpu is not None:
        draw('scene')
    rpc('desktop.game.camera', {'camera': None})
    check(draw('game', True) == 0, 'Cleared Game camera unexpectedly presented')
    record['checks'].append('deleted_game_camera_local_recoverable_scene_remains_available')
    record['state'] = state()
    # Rendering/input are read-only; the single deliberate final transaction is
    # accounted explicitly instead of claiming the complete test was immutable.
    check(record['state']['revision'] == revision+1, record['state'])
    record['world_before_sha256'] = hashlib.sha256(original).hexdigest()
    record['world_after_sha256'] = hashlib.sha256(world.read_bytes()).hexdigest()
    record['success'] = True
except Exception:
    record['error'] = traceback.format_exc()
finally:
    if host:
        lib.poima_desktop_destroy(host)
        host = None
    for hwnd in reversed(windows):
        if u.IsWindow(hwnd):
            u.DestroyWindow(hwnd)
    search.close()
    (run/'result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps({'success': record['success'], 'checks': record['checks'], 'evidence': str(run/'result.json'), 'error': record.get('error')}))
raise SystemExit(0 if record['success'] else 1)
