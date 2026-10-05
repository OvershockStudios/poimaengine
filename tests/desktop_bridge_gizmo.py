#!/usr/bin/env python3
"""Windows native gizmo transactions and optional real Vulkan overlay captures."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import traceback
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bridge', type=Path, required=True)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--gpu', type=int, help='Enable actual HWND/Vulkan attach, capture, resize and detach checks.')
args = parser.parse_args()
if os.name != 'nt':
    parser.error('Run this harness with native Windows Python; the bridge owns Windows HWNDs.')
args.output.mkdir(parents=True, exist_ok=True)
run = args.output/('run-'+uuid.uuid4().hex)
run.mkdir()
record = {'success': False, 'checks': [], 'calls': [], 'gpu': args.gpu,
          'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
          'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'limitations': ['Native ABI/HWND automation, not physical Avalonia widget qualification.']}
checks = record['checks']
# Keep the DLL search registration alive for the entire test. This also resolves
# the explicitly installed phonon.dll when this build enables CPU acoustics.
search = os.add_dll_directory(str(args.bridge.resolve().parent))
library = ctypes.CDLL(str(args.bridge.resolve()))
VP, STR = ctypes.c_void_p, ctypes.c_char_p
library.poima_desktop_create.argtypes = [STR, STR, ctypes.c_int32, ctypes.c_uint32]
library.poima_desktop_create.restype = VP
library.poima_desktop_call.argtypes = [VP, STR]
library.poima_desktop_call.restype = VP
library.poima_desktop_poll.argtypes = [VP]
library.poima_desktop_poll.restype = VP
library.poima_desktop_attach.argtypes = [VP, VP]
library.poima_desktop_attach.restype = ctypes.c_int
library.poima_desktop_draw.argtypes = [VP]
library.poima_desktop_draw.restype = ctypes.c_int
library.poima_desktop_detach.argtypes = [VP]
library.poima_desktop_detach.restype = None
library.poima_desktop_destroy.argtypes = [VP]
library.poima_desktop_destroy.restype = None
library.poima_desktop_error.argtypes = [VP]
library.poima_desktop_error.restype = VP

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                 wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, VP]
user32.CreateWindowExW.restype = wintypes.HWND
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.DestroyWindow.restype = wintypes.BOOL
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.ShowWindow.restype = wintypes.BOOL
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT]
user32.SetWindowPos.restype = wintypes.BOOL
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
user32.PeekMessageW.restype = wintypes.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t

host = None
windows = []
request_id = 0

def decode(pointer):
    return ctypes.string_at(pointer).decode('utf-8') if pointer else ''

def error(handle=None):
    return decode(library.poima_desktop_error(handle))

def check(value, message):
    if not value:
        raise AssertionError(message)

def rpc(method, params=None, expected_error=None, notification=False):
    global request_id
    request_id += 1
    request = {'jsonrpc': '2.0', 'method': method, 'params': {} if params is None else params}
    if not notification:
        request['id'] = request_id
    pointer = library.poima_desktop_call(host, json.dumps(request).encode('utf-8'))
    check(pointer, f'{method}: bridge failure: {error(host)}')
    raw = decode(pointer)
    if notification:
        check(raw == '', f'{method}: notification returned a response.')
        return None
    reply = json.loads(raw)
    record['calls'].append({'request': request, 'reply': reply})
    check(reply.get('id') == request_id, f'{method}: response ID differs.')
    if expected_error is not None:
        check('error' in reply and (expected_error == 'any' or reply['error']['code'] == expected_error), f'{method}: {reply}')
        return reply['error']
    check('result' in reply, f'{method}: {reply}')
    return reply['result']

def messages():
    message = wintypes.MSG()
    for _ in range(100):
        if not user32.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            break
        user32.TranslateMessage(ctypes.byref(message))
        user32.DispatchMessageW(ctypes.byref(message))

def poll(draw=False):
    messages()
    pointer = library.poima_desktop_poll(host)
    check(pointer, f'Poll failed: {error(host)}')
    status = json.loads(decode(pointer))
    if draw:
        check(library.poima_desktop_draw(host) >= 0, f'Draw failed: {error(host)}')
    return status

def until(predicate, draw=False, timeout=8):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        status = poll(draw)
        if predicate(status):
            return status
        time.sleep(.01)
    raise AssertionError('Desktop bridge test wait timed out.')

def client(requests):
    data=''.join(json.dumps(r)+'\n' for r in requests)
    process = subprocess.run([str(args.binary.resolve()), 'connect', endpoint, '--timeout-ms', '4000'],
                             input=data, text=True, encoding='utf-8', capture_output=True, timeout=10)
    check(process.returncode == 0, process.stdout+process.stderr)
    return [json.loads(line) for line in process.stdout.splitlines()]

def make_window():
    module = kernel32.GetModuleHandleW(None)
    parent = user32.CreateWindowExW(0, 'STATIC', 'Poima desktop bridge contract', 0x10CF0000,
                                  80, 80, 820, 650, None, None, module, None)
    check(parent, f'Parent HWND creation failed: {ctypes.get_last_error()}')
    windows.append(parent)
    child = user32.CreateWindowExW(0, 'STATIC', '', 0x56000000, 0, 0, 760, 520, parent, None, module, None)
    check(child, f'Child HWND creation failed: {ctypes.get_last_error()}')
    windows.append(child)
    messages()
    return parent, child

def bmp(path, width, height):
    raw = path.read_bytes()
    check(raw[:2] == b'BM' and len(raw) > 54, 'Capture is not a complete BMP.')
    actual_width, actual_height = struct.unpack_from('<ii', raw, 18)
    check((actual_width, abs(actual_height)) == (width, height), f'BMP extent differs: {actual_width}x{actual_height}.')
    return {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'width': width, 'height': height}

WIDTH, HEIGHT = 760, 520

def gizmo(method, params=None, **kwargs):
    return rpc('desktop.gizmo.'+method, params, **kwargs)

def inspect_gizmo():
    return gizmo('inspect', {'width': WIDTH, 'height': HEIGHT})

def gesture(axis='x', mode='move', space='world'):
    gizmo('configure', {'mode': mode, 'space': space})
    inspected = inspect_gizmo()
    handle = next(h for h in inspected['handles'] if h['axis'] == axis)
    check(handle['visible'] and len(handle['points']) >= 2, f'Handle unavailable: {inspected}')
    points = handle['points']
    if mode == 'rotate':
        candidates = [(points[i], points[(i+max(1, len(points)//8)) % len(points)]) for i in range(0, len(points), max(1, len(points)//12))]
    else:
        start, end = points[0], points[-1]
        dx, dy = end[0]-start[0], end[1]-start[1]
        length = math.hypot(dx, dy)
        check(length > 1, 'Handle segment has no usable extent.')
        candidates = [([start[0]+dx*t, start[1]+dy*t], [start[0]+dx*t+dx/length*35, start[1]+dy*t+dy/length*35]) for t in [.72, .86, .55]]
    for anchor, destination in candidates:
        begun = gizmo('begin', {'revision': revision, 'width': WIDTH, 'height': HEIGHT, 'x': anchor[0], 'y': anchor[1]})
        if begun.get('started') and begun.get('axis') == axis:
            return begun['drag_id'], destination
        if begun.get('started'):
            gizmo('cancel')
    raise AssertionError(f'Could not begin {mode}/{axis} at its reported handle: {inspected}')

def update(drag, destination, snap=False, **kwargs):
    return gizmo('update', {'drag_id': drag, 'x': destination[0], 'y': destination[1], 'snap': snap}, **kwargs)

def transform(position, rotation=None, scale=None):
    return {'position': position, 'rotation': rotation or [0, 0, 0, 1], 'scale': scale or [1, 1, 1]}

def component(identity, kind, value):
    return {'op': 'component.set', 'id': identity, 'type': kind, 'value': value}

def transact(ops):
    global revision
    revision = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex, 'ops': ops})['revision']

def authored(identity):
    return rpc('entity.get', {'id': identity})['value']['components']['Transform']

def select_frame(identity):
    rpc('desktop.select', {'id': identity})
    return rpc('desktop.frame', {'revision': revision, 'id': identity, 'aspect': WIDTH/HEIGHT})

def assert_unmodified(data, identity, baseline):
    check(world.read_bytes() == data and authored(identity) == baseline, 'Uncommitted gizmo changed authored storage.')

def capture(name):
    path = run/(name+'.bmp')
    job = rpc('desktop.capture', {'revision': revision, 'path': str(path)})
    until(lambda state: state['capture']['state'] != 'queued', draw=True, timeout=20)
    completed = rpc('desktop.capture.status', {'capture_id': job['capture_id']})
    check(completed['state'] == 'complete', completed)
    report = completed['result']['render']
    check(report['nvrhi_errors'] == 0, report)
    record.setdefault('captures', {})[name] = bmp(path, completed['result']['width'], completed['result']['height'])
    record['captures'][name]['result'] = completed['result']
    return path

def pixel_difference(a, b):
    left, right = a.read_bytes(), b.read_bytes()
    left_offset, right_offset = struct.unpack_from('<I', left, 10)[0], struct.unpack_from('<I', right, 10)[0]
    check(left[18:30] == right[18:30], 'Comparison captures have different dimensions/formats.')
    width, height = struct.unpack_from('<ii', left, 18)
    bits = struct.unpack_from('<H', left, 28)[0]
    check(bits in (24, 32), f'Unsupported BMP pixel format: {bits}')
    stride = ((width*bits+31)//32)*4
    size = bits//8
    changed = 0
    for row in range(abs(height)):
        for column in range(width):
            p = row*stride+column*size
            if left[left_offset+p:left_offset+p+3] != right[right_offset+p:right_offset+p+3]:
                changed += 1
    return changed

try:
    project = run/'Gizmo project'
    created = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Gizmo contract'],
                             capture_output=True, text=True, encoding='utf-8', timeout=30)
    check(created.returncode == 0, created.stdout+created.stderr)
    manifest = json.loads((project/'project.json').read_text(encoding='utf-8'))
    world = project/manifest['entry']['world']
    endpoint = 'gizmo-test-'+uuid.uuid4().hex
    host = library.poima_desktop_create(str(world).encode(), endpoint.encode(), args.gpu if args.gpu is not None else -1, 4)
    check(host, f'Host creation failed: {error()}')
    revision = rpc('world.inspect')['revision']
    methods = rpc('desktop.describe')['methods']
    for name in ['configure', 'inspect', 'begin', 'update', 'commit', 'cancel']:
        check('desktop.gizmo.'+name in methods, 'Gizmo method not discoverable: '+name)
    cube, other, parent, child = [uuid.uuid4().hex for _ in range(4)]
    ops = []
    for identity, name, position in [(cube, 'Gizmo cube', [1000, 0, 0]), (other, 'Other selection', [1005, 0, 0]), (parent, 'Affine parent', [1020, 0, 0])]:
        ops.extend([{'op': 'entity.create', 'id': identity, 'name': name}, component(identity, 'Transform', transform(position))])
    ops += [{'op': 'entity.create', 'id': child, 'name': 'Parented cube', 'parent': parent}]
    for identity in [cube, child]:
        ops.append(component(identity, 'MeshRenderer', {'primitive': 'box', 'albedo': [.35, .5, .65], 'visible': True}))
    ops.append(component(parent, 'Transform', transform([1020, 0, 0], [0, 0, math.sin(math.pi/4), math.cos(math.pi/4)], [2, 1, 1])))
    transact(ops)
    rpc('desktop.camera', {'yaw': 32, 'pitch': -23, 'vertical_fov': 60})
    select_frame(cube)
    baseline = authored(cube); storage = world.read_bytes()
    gizmo('configure', {'mode': 'invalid', 'space': 'world'}, expected_error=-32602)
    gizmo('inspect', {'width': 0, 'height': HEIGHT}, expected_error=-32602)
    gizmo('begin', {'revision': revision-1, 'width': WIDTH, 'height': HEIGHT, 'x': 1, 'y': 1}, expected_error=-32009)
    missed = gizmo('begin', {'revision': revision, 'width': WIDTH, 'height': HEIGHT, 'x': 0, 'y': 0})
    check(not missed['started'], 'Empty-space gesture unexpectedly started.')
    assert_unmodified(storage, cube, baseline)
    checks.append('Strict schemas, stale begin and handle misses preserve authored state without graphics initialization.')

    history_before = rpc('world.history')['undo_count']
    drag, _ = gesture()
    unchanged_commit = gizmo('commit', {'drag_id': drag, 'request_id': uuid.uuid4().hex})
    check(unchanged_commit['revision'] == revision and not unchanged_commit['changed'], unchanged_commit)
    check(rpc('world.history')['undo_count'] == history_before and inspect_gizmo()['active'] is None,
          'No-movement commit wrote history or left an active drag.')
    assert_unmodified(storage, cube, baseline)
    checks.append('No-movement commit creates no revision, history entry or storage write.')
    drag, destination = gesture()
    preview = update(drag, destination)
    check(preview['transform'] != baseline and preview['drag_id'] == drag, preview)
    check(inspect_gizmo()['active']['drag_id'] == drag, 'Drag state is not observable.')
    assert_unmodified(storage, cube, baseline)
    check(rpc('world.history')['undo_count'] == history_before, 'Preview polluted history.')
    gizmo('cancel')
    check(inspect_gizmo()['active'] is None, 'Cancel did not clear active drag.')
    update(drag, destination, expected_error=-32009)
    assert_unmodified(storage, cube, baseline)

    drag, destination = gesture()
    preview = update(drag, destination, snap=True)
    commit_params = {'drag_id': drag, 'request_id': uuid.uuid4().hex}
    committed = gizmo('commit', commit_params); revision = committed['revision']
    committed_transform = authored(cube)
    check(committed_transform == preview['transform'] and committed_transform != baseline, 'Commit differs from preview.')
    check(rpc('world.history')['undo_count'] == history_before+1, 'Gesture must create exactly one undo entry.')
    committed_bytes = world.read_bytes()
    repeated = gizmo('commit', commit_params)
    check(repeated == committed and repeated['revision'] == revision and world.read_bytes() == committed_bytes, 'Exact commit retry is not idempotent.')
    gizmo('commit', {'drag_id': drag, 'request_id': uuid.uuid4().hex}, expected_error='any')
    transact([{'op': 'entity.rename', 'id': other, 'name': 'After gizmo commit'}])
    after_external = world.read_bytes(); after_history = rpc('world.history')
    repeated_later = gizmo('commit', commit_params)
    check(repeated_later == committed and world.read_bytes() == after_external and rpc('world.history') == after_history,
          'Retained commit retry changed the world/history after a later authored revision.')
    revision = rpc('world.undo', {'request_id': uuid.uuid4().hex, 'base_revision': revision})['revision']
    revision = rpc('world.undo', {'request_id': uuid.uuid4().hex, 'base_revision': revision})['revision']
    check(authored(cube) == baseline, 'Undo did not restore pre-gesture transform.')
    revision = rpc('world.redo', {'request_id': uuid.uuid4().hex, 'base_revision': revision})['revision']
    check(authored(cube) == committed_transform, 'Redo did not restore committed transform.')
    checks.append('Preview/cancel leaves storage/history untouched; snapped commit produces one undo entry with idempotent retry, undo and redo.')

    select_frame(cube)
    rotation_baseline = authored(cube); rotation_bytes = world.read_bytes()
    drag, destination = gesture('z', 'rotate', 'world')
    rotated = update(drag, destination)['transform']
    check(rotated['rotation'] != rotation_baseline['rotation'] and abs(sum(v*v for v in rotated['rotation'])-1) < 1e-6,
          'Valid rotation did not yield a changed unit quaternion: '+str(rotated))
    check(math.dist(rotated['position'], rotation_baseline['position']) < 1e-8 and math.dist(rotated['scale'], rotation_baseline['scale']) < 1e-8, 'Pure rotation changed position or scale.')
    gizmo('cancel'); assert_unmodified(rotation_bytes, cube, rotation_baseline)
    checks.append('A valid unparented world rotation produces a normalized preview without changing position/scale or authored storage.')

    for reason in ['camera', 'selection', 'configuration', 'authored revision', 'runtime']:
        if reason == 'runtime' and not rpc('desktop.inspect')['runtime']['available']:
            continue
        select_frame(cube)
        drag, destination = gesture()
        update(drag, destination)
        if reason == 'camera':
            camera = rpc('desktop.inspect')['camera']; rpc('desktop.camera', {'yaw': camera['yaw']+1})
        elif reason == 'selection':
            rpc('desktop.select', {'id': other})
        elif reason == 'configuration':
            gizmo('configure', {'mode': 'scale', 'space': 'local'})
        elif reason == 'authored revision':
            transact([{'op': 'entity.rename', 'id': other, 'name': 'External revision'}])
        else:
            session = uuid.uuid4().hex; rpc('runtime.start', {'session_id': session, 'revision': revision})
        expected_bytes = world.read_bytes()
        update(drag, destination, expected_error=-32009)
        gizmo('commit', {'drag_id': drag, 'request_id': uuid.uuid4().hex}, expected_error=-32009)
        check(inspect_gizmo()['active'] is None and world.read_bytes() == expected_bytes, reason+' invalidation was not atomic.')
        if reason == 'runtime':
            rpc('runtime.stop', {'session_id': session})
    checks.append('Camera, selection, tool configuration, authored revision and runtime transitions invalidate pending gestures without committing previews.')

    select_frame(child)
    child_before = authored(child); storage = world.read_bytes()
    drag, destination = gesture('x', 'move', 'world')
    moved = update(drag, destination)['transform']
    check(abs(moved['position'][0]-child_before['position'][0]) < 1e-6 and abs(moved['position'][1]-child_before['position'][1]) > .001,
          'World move did not transform through rotated, nonuniform parent: '+str(moved))
    gizmo('cancel'); assert_unmodified(storage, child, child_before)
    drag, destination = gesture('z', 'rotate', 'world')
    before_rejected = inspect_gizmo()['active']['transform']
    update(drag, destination, expected_error=-32602)
    check(inspect_gizmo()['active']['transform'] == before_rejected, 'Rejected shear changed the last valid preview.')
    gizmo('cancel'); assert_unmodified(storage, child, child_before)
    drag, destination = gesture('x', 'scale', 'local')
    scaled = update(drag, destination)['transform']
    check(all(value > 0 for value in scaled['scale']) and scaled['scale'][0] != child_before['scale'][0] and scaled['scale'][1:] == child_before['scale'][1:], scaled)
    gizmo('cancel'); assert_unmodified(storage, child, child_before)
    checks.append('Parent-affine world movement converts to local TRS; sheared world rotation rejects atomically; local axis scale preserves positive other axes.')

    if args.gpu is not None:
        parent_hwnd, child_hwnd = make_window()
        check(library.poima_desktop_attach(host, child_hwnd) == 1, error(host))
        select_frame(cube)
        gizmo('configure', {'mode': 'none', 'space': 'world'})
        until(lambda state: state['frames_presented'] >= 2, draw=True, timeout=20)
        plain = capture('scene-no-overlay')
        gizmo('configure', {'mode': 'move', 'space': 'world'})
        idle = capture('scene-with-handles')
        overlay_pixels = pixel_difference(plain, idle)
        check(overlay_pixels > 20, 'Native overlay produced no meaningful visible pixel changes.')
        for change in ['update', 'configure', 'select']:
            if change == 'update':
                drag, destination = gesture()
            stale_path = run/('capture-invalidated-by-'+change+'.bmp')
            queued = rpc('desktop.capture', {'revision': revision, 'path': str(stale_path)})
            if change == 'update':
                update(drag, destination)
            elif change == 'configure':
                gizmo('configure', {'mode': 'rotate', 'space': 'world'})
            else:
                rpc('desktop.select', {'id': other})
            stale = rpc('desktop.capture.status', {'capture_id': queued['capture_id']})
            check(stale['state'] == 'error' and stale['error']['code'] == -32009 and not stale_path.exists(), stale)
            gizmo('cancel'); gizmo('configure', {'mode': 'move', 'space': 'world'}); rpc('desktop.select', {'id': cube})
        checks.append('Queued captures reject subsequent preview updates, tool configuration and selection changes without publishing stale pixels.')
        storage = world.read_bytes(); baseline = authored(cube)
        drag, destination = gesture()
        update(drag, destination)
        preview_capture = capture('scene-drag-preview')
        check(pixel_difference(idle, preview_capture) > 20, 'Gesture preview is absent from actual captured Vulkan scene.')
        assert_unmodified(storage, cube, baseline)
        gizmo('cancel')
        canceled = capture('scene-canceled')
        restored_pixels = pixel_difference(idle, canceled)
        check(restored_pixels == 0, f'Cancel did not restore the rendered scene/handles: {restored_pixels} changed pixels.')
        drag, destination = gesture()
        check(user32.SetWindowPos(child_hwnd, None, 0, 0, 640, 360, 0x0014), 'Child resize failed.')
        until(lambda state: state['render']['width'] == 640 and state['render']['height'] == 360, draw=True)
        update(drag, destination, expected_error=-32009)
        WIDTH, HEIGHT = 640, 360
        resized = capture('scene-resized')
        record['overlay_pixels'] = overlay_pixels; record['cancel_changed_pixels'] = restored_pixels
        record['final_render'] = rpc('desktop.inspect')['render']
        checks.append('Real Vulkan captures include handles and preview; cancel restores pixels exactly; resize invalidates drag and rebuilds overlay resources with zero backend errors.')
    else:
        record['limitations'].append('No GPU supplied: real overlay/capture/resize checks were not run.')

    record['final_state'] = rpc('desktop.inspect')
    record['final_gizmo'] = inspect_gizmo()
    record['success'] = True
except BaseException:
    record['error'] = traceback.format_exc()
finally:
    if host:
        library.poima_desktop_destroy(host)
    for window in reversed(windows):
        if user32.IsWindow(window):
            user32.DestroyWindow(window)
    search.close()
    (run/'result.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
print(json.dumps({'success': record['success'], 'evidence': str(run/'result.json'), 'checks': len(checks)}))
if not record['success']:
    print(record.get('error', 'Desktop gizmo contract failed.'), file=sys.stderr)
sys.exit(0 if record['success'] else 1)
