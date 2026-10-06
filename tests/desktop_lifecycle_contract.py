#!/usr/bin/env python3
"""Focused owned-HWND regression for paused runtime lifecycle and desktop cache guards."""
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
parser.add_argument('--gpu', type=int, required=True, help='Vulkan device index for real Scene/Game captures.')
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
    hwnd = u.CreateWindowExW(0, 'STATIC', 'Poima owned lifecycle qualification', style,
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


def poll_state():
    raw = lib.poima_desktop_poll(host)
    check(raw, error())
    result = json.loads(decode(raw))
    record.setdefault('polls', []).append(result)
    return result

def image_hash(label):
    return hashlib.sha256((run/(label+'.bmp')).read_bytes()).hexdigest()

try:
    project_dir = run/'project'
    process = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project_dir), '--name', 'Lifecycle cache qualification'], capture_output=True, text=True)
    check(process.returncode == 0, process.stdout+process.stderr)
    manifest = json.loads((project_dir/'project.json').read_text())
    world = project_dir/manifest['entry']['world']
    host = lib.poima_desktop_create(str(world).encode(), b'', args.gpu, args.samples)
    check(host, error())
    camera, template = uuid.uuid4().hex, uuid.uuid4().hex
    transform = {'position': [0, 2, 8], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}
    before_revision = state()['revision']
    rpc('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': before_revision, 'ops': [
        {'op': 'entity.create', 'id': camera, 'name': 'Lifecycle test camera'},
        {'op': 'component.set', 'id': camera, 'type': 'Transform', 'value': transform},
        {'op': 'component.set', 'id': camera, 'type': 'Camera', 'value': {'vertical_fov': 60, 'near': .1, 'far': 1000}},
        {'op': 'template.set', 'id': template, 'name': 'Plain visible prop', 'components': {
            'Transform': {'position': [0, 2, 4], 'rotation': [0, 0, 0, 1], 'scale': [2, 2, 2]},
            'MeshRenderer': {'primitive': 'box', 'visible': True, 'albedo': [.9, .15, .05]}}}]})
    revision = state()['revision']
    rpc('desktop.game.camera', {'camera': camera})
    rpc('desktop.camera', {'position': [0, 2, 8], 'yaw': 0, 'pitch': 0})
    parent = create_window(x=30, y=30, width=860, height=390)
    attach('scene', create_window(parent, 10, 10, 400, 300))
    attach('game', create_window(parent, 420, 10, 400, 300))
    session = uuid.uuid4().hex
    rpc('desktop.play.start', {'revision': revision, 'session_id': session, 'paused': True})
    baseline = state()
    check(baseline['runtime']['structure_revision'] == 0 and baseline['runtime']['tick'] == 0, baseline)
    base_component_revision = baseline['components']['revision']
    # Warm both snapshots before changing topology; changing the camera later
    # would mask the regression this test is intended to catch.
    capture('scene', 'scene-before')
    capture('game', 'game-before')
    poll_state()
    check(not poll_state()['runtime_changed'], 'Idle runtime reported a change')
    abandoned_scene = run/'stale-scene.bmp'
    queued = rpc('desktop.capture', {'revision': revision, 'path': str(abandoned_scene), 'view': 'scene'})
    spawn = rpc('runtime.structure.transact', {'session_id': session, 'request_id': uuid.uuid4().hex,
        'expected_tick': 0, 'expected_structure_revision': 0, 'spawns': [{'template_id': template}]})
    prop = spawn['spawned'][0]
    changed = poll_state()
    check(changed['runtime_changed'] and changed['runtime']['structure_revision'] == 1 and changed['runtime']['tick'] == 0, changed)
    check(changed['components']['revision'] == base_component_revision, 'Plain prop unexpectedly changed custom component revision')
    expired = rpc('desktop.capture.status', {'capture_id': queued['capture_id']})
    check(expired['state'] == 'error' and expired['error']['code'] == -32009 and not abandoned_scene.exists(), expired)
    record['checks'].append('same_tick_plain_birth_status_poll_and_queued_scene_capture_invalidation')
    pick = rpc('desktop.pick', {'revision': revision, 'x': .5, 'y': .5, 'aspect': 4/3})
    check(pick['id'] == prop, pick)
    capture('scene', 'scene-spawned')
    capture('game', 'game-spawned')
    check(image_hash('scene-before') != image_hash('scene-spawned'), 'Scene snapshot reused pre-spawn pixels')
    check(image_hash('game-before') != image_hash('game-spawned'), 'Game snapshot reused pre-spawn pixels')
    record['checks'].append('same_tick_birth_invalidates_both_viewport_pixels_and_scene_picking')
    abandoned_game = run/'stale-game.bmp'
    queued = rpc('desktop.capture', {'revision': revision, 'path': str(abandoned_game), 'view': 'game'})
    rpc('runtime.structure.transact', {'session_id': session, 'request_id': uuid.uuid4().hex,
        'expected_tick': 0, 'expected_structure_revision': 1, 'despawns': [prop]})
    changed = poll_state()
    check(changed['runtime_changed'] and changed['runtime']['structure_revision'] == 2 and changed['runtime']['tick'] == 0, changed)
    expired = rpc('desktop.capture.status', {'capture_id': queued['capture_id']})
    check(expired['state'] == 'error' and expired['error']['code'] == -32009 and not abandoned_game.exists(), expired)
    check(rpc('desktop.pick', {'revision': revision, 'x': .5, 'y': .5, 'aspect': 4/3})['id'] != prop, 'Scene picking retained retired prop')
    capture('scene', 'scene-removed')
    capture('game', 'game-removed')
    check(image_hash('scene-before') == image_hash('scene-removed'), 'Scene pixels did not return to pre-spawn state')
    check(image_hash('game-before') == image_hash('game-removed'), 'Game pixels did not return to pre-spawn state')
    record['checks'].append('same_tick_removal_restores_pixels_and_picking_and_invalidates_queued_game_capture')
    step = {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'ticks': 1}
    before_step = state()['runtime']
    rpc('desktop.play.step', step, -32602)
    check(state()['runtime'] == before_step, 'Missing topology guard changed the paused runtime')
    rpc('desktop.play.step', dict(step, expected_structure_revision=1), -32009)
    check(state()['runtime'] == before_step, 'Stale topology guard changed the paused runtime')
    step['expected_structure_revision'] = 2
    committed_step = rpc('desktop.play.step', step)
    after_step = state()['runtime']
    check(after_step['tick'] == 1 and after_step['structure_revision'] == 2, after_step)
    retried_step = rpc('desktop.play.step', step)
    check(retried_step == dict(committed_step, replayed=True), retried_step)
    check(state()['runtime'] == after_step, 'Retained paused-step retry advanced the runtime twice')
    record['checks'].append('paused_step_requires_current_topology_guard_and_replays_without_advancing')
    rpc('desktop.play.resume', {'session_id': session})
    rpc('runtime.structure.transact', {'session_id': session, 'request_id': uuid.uuid4().hex,
        'expected_tick': 1, 'expected_structure_revision': 2, 'spawns': [{'template_id': template}]}, -32009)
    rpc('desktop.play.pause', {'session_id': session})
    final = state()
    check(final['runtime']['structure_revision'] == 2, final)
    record['checks'].append('playing_desktop_rejects_manual_structure_transaction')
    record['pixel_sha256'] = {name: image_hash(name) for name in (
        'scene-before', 'game-before', 'scene-spawned', 'game-spawned', 'scene-removed', 'game-removed')}
    record['state'] = final
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
