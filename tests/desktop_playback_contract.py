#!/usr/bin/env python3
"""Windows native editor playback clock, Game cameras and optional Vulkan captures."""
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
from animation_fixture import ribbon
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


def transform(position, scale=None):
    return {'position': position, 'rotation': [0, 0, 0, 1], 'scale': scale or [1, 1, 1]}


def component(identity, kind, value):
    return {'op': 'component.set', 'id': identity, 'type': kind, 'value': value}


def transact(ops):
    global revision
    revision = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex, 'ops': ops})['revision']


def playback():
    return rpc('desktop.play.inspect')


def play(method, params=None, **kwargs):
    return rpc('desktop.play.'+method, params, **kwargs)


def start(paused=False):
    identity = uuid.uuid4().hex
    state = play('start', {'revision': revision, 'session_id': identity, 'paused': paused})
    check(state['state'] == ('paused' if paused else 'playing') and state['session_id'] == identity and state['tick'] == 0, state)
    return identity


def step_params(identity, ticks=1, animations=None):
    value = {'session_id': identity, 'expected_tick': playback()['tick'], 'ticks': ticks,
             'request_id': uuid.uuid4().hex, 'inputs': []}
    if animations is not None:
        value['animations'] = animations
    return value


def unchanged(data, history):
    check(world.read_bytes() == data, 'Playback/view operation changed authored world bytes.')
    check(rpc('world.history') == history, 'Playback/view operation changed authored history.')


def queue_capture(name):
    path = run/(name+'.bmp')
    job = rpc('desktop.capture', {'revision': revision, 'path': str(path)})
    return path, job['capture_id']


def complete_capture(path, identity):
    deadline = time.monotonic()+15
    while True:
        poll(draw=True)
        completed = rpc('desktop.capture.status', {'capture_id': identity})
        if completed['state'] != 'queued' or time.monotonic() >= deadline:
            break
        time.sleep(.01)
    check(completed['state'] == 'complete', completed)
    result = completed['result']
    check(result['render']['nvrhi_errors'] == 0, result)
    record.setdefault('captures', {})[path.stem] = {'image': bmp(path, result['width'], result['height']), 'result': result}
    return result


try:
    project = run/'Playback project'
    created = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Playback contract'],
                             capture_output=True, text=True, encoding='utf-8', timeout=30)
    check(created.returncode == 0, created.stdout+created.stderr)
    manifest = json.loads((project/'project.json').read_text(encoding='utf-8'))
    world = project/manifest['entry']['world']
    endpoint = 'playback-test-'+uuid.uuid4().hex
    host = library.poima_desktop_create(str(world).encode(), endpoint.encode(), args.gpu if args.gpu is not None else -1, 4)
    check(host, f'Host creation failed: {error()}')
    revision = rpc('world.inspect')['revision']
    methods = rpc('desktop.describe')['methods']
    for method in ['start', 'pause', 'resume', 'stop', 'inspect', 'step']:
        check('desktop.play.'+method in methods, 'Playback method not discoverable: '+method)
    check('desktop.view' in methods and 'desktop.cameras' in methods, 'Game view discovery is absent.')
    initial = playback()
    check(initial['state'] == 'stopped' and initial['session_id'] is None and initial['tick'] is None, initial)
    check(abs(initial['fixed_dt']-1/60) < 1e-12 and initial['last_error'] is None and initial['suspended'] is None, initial)
    cube, camera, bad_camera = [uuid.uuid4().hex for _ in range(3)]
    lens = {'vertical_fov': 47, 'near': .2, 'far': 450}
    ops = []
    for identity, name, position in [(cube, 'Playback subject', [1000, 0, 0]),
                                     (camera, 'Game camera', [1000, 0, 5]),
                                     (bad_camera, 'Nonrigid camera', [1000, 0, 5])]:
        ops += [{'op': 'entity.create', 'id': identity, 'name': name}, component(identity, 'Transform', transform(position))]
    ops += [component(cube, 'MeshRenderer', {'primitive': 'box', 'albedo': [.3, .55, .7], 'visible': True}),
            component(camera, 'Camera', lens), component(bad_camera, 'Camera', lens),
            component(bad_camera, 'Transform', transform([1000, 0, 5], [2, 1, 1]))]
    transact(ops)
    rpc('desktop.camera', {'position': [1003, 2, 6], 'yaw': 25, 'pitch': -12, 'vertical_fov': 60})
    rpc('desktop.select', {'id': cube})
    # The nonrigid camera is useful for authored view validation, but cannot be
    # present when starting a runtime, whose camera contract is deliberately rigid.
    original_view = rpc('desktop.inspect')['view']
    for params in [{'mode': 'scene', 'camera': camera}, {'mode': 'game'}, {'mode': 'game', 'camera': cube},
                   {'mode': 'game', 'camera': bad_camera}, {'mode': 'unknown'}]:
        rpc('desktop.view', params, expected_error='any')
        check(rpc('desktop.inspect')['view'] == original_view, 'Rejected Game view changed the active view.')
    cameras = rpc('desktop.cameras')
    check(cameras['source'] == 'authored' and any(c['id'] == camera and c['vertical_fov'] == 47 for c in cameras['cameras']), cameras)
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    game_handles = rpc('desktop.gizmo.inspect', {'width': WIDTH, 'height': HEIGHT})
    check(game_handles['overlay_vertices'] == 0 and not any(h['visible'] for h in game_handles['handles']), game_handles)
    for method, params in [('desktop.camera', {'yaw': 0}),
                           ('desktop.pick', {'revision': revision, 'x': .5, 'y': .5, 'aspect': WIDTH/HEIGHT}),
                           ('desktop.frame', {'revision': revision, 'id': cube, 'aspect': WIDTH/HEIGHT}),
                           ('desktop.gizmo.begin', {'revision': revision, 'width': WIDTH, 'height': HEIGHT, 'x': 0, 'y': 0})]:
        rpc(method, params, expected_error=-32009)
    rpc('desktop.view', {'mode': 'scene'})
    transact([{'op': 'entity.delete', 'id': bad_camera, 'recursive': True}])
    storage, history = world.read_bytes(), rpc('world.history')
    checks.append('Playback/view schemas are discoverable; invalid camera selection is atomic and Game excludes Scene interaction tools.')

    sid = start(paused=True)
    parameters = {'revision': revision, 'session_id': sid}
    check(play('start', parameters)['state'] == 'paused', 'Retrying start silently resumed a paused session.')
    for params in [dict(parameters, paused='false'), dict(parameters, revision=revision-1), dict(parameters, extra=True)]:
        play('start', params, expected_error='any')
    check(playback()['tick'] == 0 and playback()['state'] == 'paused', 'Invalid start changed playback.')
    time.sleep(.08)
    for _ in range(3):
        poll()
        check(library.poima_desktop_draw(host) == 0, 'Unattached draw unexpectedly presented.')
    check(playback()['tick'] == 0, 'Paused/detached poll or draw advanced simulation.')
    params = step_params(sid)
    result = play('step', params)
    check(playback()['tick'] == 1, result)
    retried = play('step', params)
    check(playback()['tick'] == 1 and retried['tick'] == result['tick'], 'Step receipt replay advanced the clock.')
    play('pause', {'session_id': uuid.uuid4().hex}, expected_error=-32009)
    check(playback()['state'] == 'paused', 'Wrong-session pause changed state.')
    unchanged(storage, history)
    checks.append('Paused clock and detached draw remain inert; Step advances exactly once with core receipt retry and strict session guards.')

    play('resume', {'session_id': sid})
    start_tick = playback()['tick']
    time.sleep(.23)
    for _ in range(4):
        check(playback()['tick'] == start_tick, 'Inspect advanced the owner clock.')
        rpc('desktop.inspect')
        check(library.poima_desktop_draw(host) == 0, 'Detached draw unexpectedly presented.')
    check(playback()['tick'] == start_tick, 'Draw advanced the owner clock.')
    poll()
    stalled = playback()
    check(stalled['tick']-start_tick == 8 and stalled['dropped_seconds'] > .05, stalled)
    record['stalled_clock'] = stalled
    tick = stalled['tick']
    for method in ['desktop.play.step', 'runtime.step']:
        rpc(method, step_params(sid), expected_error=-32009)
    # These valid method names must be guarded before their mutating handlers.
    for method in ['runtime.audio.replay', 'runtime.gameplay.load', 'runtime.gameplay.edit']:
        rpc(method, {}, expected_error=-32009)
    check(playback()['tick'] == tick, 'Rejected mutating command advanced the clock.')
    play('pause', {'session_id': sid})
    time.sleep(.18)
    poll()
    check(playback()['tick'] == tick, 'Paused time advanced simulation.')
    dropped = playback()['dropped_seconds']
    resume_started = time.monotonic()
    play('resume', {'session_id': sid})
    poll()
    resume_elapsed = time.monotonic()-resume_started
    resumed = playback()
    allowed_ticks = min(8, math.ceil(resume_elapsed*60)+1)
    allowed_drop = max(0, resume_elapsed-8/60)+.005
    check(resumed['tick']-tick <= allowed_ticks and resumed['dropped_seconds']-dropped <= allowed_drop, resumed)
    record['resume_elapsed_seconds'] = resume_elapsed
    play('pause', {'session_id': sid})
    play('stop', {'session_id': sid})
    stopped = playback()
    check(stopped['state'] == 'stopped' and stopped['session_id'] is None and stopped['tick'] is None, stopped)
    unchanged(storage, history)
    checks.append('Only owner poll advances real time; long stalls cap at eight ticks, competing runtime mutations reject, and paused wall time never catches up.')

    external = uuid.uuid4().hex
    rpc('runtime.start', {'revision': revision, 'session_id': external})
    check(playback()['state'] == 'paused' and playback()['session_id'] == external, playback())
    time.sleep(.05); poll()
    check(playback()['tick'] == 0, 'External runtime.start enabled the wall clock implicitly.')
    rpc('runtime.stop', {'session_id': external})
    check(playback()['state'] == 'stopped', playback())
    unchanged(storage, history)
    checks.append('Existing runtime.start/stop synchronize with the desktop driver, defaulting external starts to paused.')

    sid = start(paused=True)
    frozen_revision = revision
    scene_camera = rpc('desktop.inspect')['camera']
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    transact([component(camera, 'Camera', {'vertical_fov': 83, 'near': .5, 'far': 900}),
              component(camera, 'Transform', transform([1000, 0, 9]))])
    frozen = rpc('desktop.cameras')
    selected_camera = next(c for c in frozen['cameras'] if c['id'] == camera)
    check(frozen['source'] == 'runtime' and selected_camera['vertical_fov'] == 47 and selected_camera['near'] == .2 and selected_camera['far'] == 450, frozen)
    transact([{'op': 'entity.delete', 'id': camera, 'recursive': True}])
    check(any(c['id'] == camera for c in rpc('desktop.cameras')['cameras']), 'Authored deletion removed a frozen runtime camera.')
    check(rpc('desktop.inspect')['view'] == {'mode': 'game', 'camera': camera}, 'Frozen Game camera selection was lost.')
    storage, history = world.read_bytes(), rpc('world.history')
    checks.append('Game uses the same runtime with its frozen camera lens and hierarchy despite authored camera edits and deletion.')

    if args.gpu is not None:
        parent_hwnd, child_hwnd = make_window()
        check(library.poima_desktop_attach(host, child_hwnd) == 1, f'Attach failed: {error(host)}')
        until(lambda status: status['frames_presented'] >= 2, draw=True, timeout=20)
        path, job = queue_capture('frozen-game-camera')
        game_capture = complete_capture(path, job)
        check(game_capture['view'] == {'mode': 'game', 'camera': camera} and game_capture['source'] == 'runtime' and game_capture['scene_revision'] == frozen_revision and game_capture['tick'] == 0, game_capture)
        check(game_capture['camera_world'][12:15] == [1000, 0, 5], 'Game capture used an authored camera pose instead of the frozen runtime pose.')
        check(game_capture['lens']['vertical_fov'] == 47 and game_capture['lens']['near'] == .2 and game_capture['lens']['far'] == 450, game_capture)
        state = rpc('desktop.gizmo.inspect', {'width': WIDTH, 'height': HEIGHT})
        check(not any(h['visible'] for h in state['handles']) and state['overlay_vertices'] == 0, state)
        play('resume', {'session_id': sid})
        draw_tick = playback()['tick']
        time.sleep(.05)
        for _ in range(2):
            check(library.poima_desktop_draw(host) >= 0, f'Playing draw failed: {error(host)}')
        check(playback()['tick'] == draw_tick, 'Real GPU presentation advanced simulation outside owner poll.')
        poll()
        path, job = queue_capture('playing-clock-held')
        held_tick = playback()['tick']; held_dropped = playback()['dropped_seconds']
        check(playback()['suspended'] == 'capture' and playback()['state'] == 'playing', playback())
        for _ in range(3):
            time.sleep(.06); poll()
            check(playback()['tick'] == held_tick, 'Queued capture did not suspend auto-play.')
        held = complete_capture(path, job)
        check(held['tick'] == held_tick, held)
        check(playback()['state'] == 'playing' and playback()['suspended'] is None, playback())
        poll()
        check(playback()['tick'] == held_tick and abs(playback()['dropped_seconds']-held_dropped) < 1e-9,
              'Capture hold leaked into catch-up time.')
        time.sleep(.04); poll()
        check(playback()['tick'] > held_tick, 'Playback did not resume after capture.')
        play('pause', {'session_id': sid})
        path, job = queue_capture('invalidated-game-view')
        rpc('desktop.view', {'mode': 'scene'})
        rejected = rpc('desktop.capture.status', {'capture_id': job})
        check(rejected['state'] == 'error' and rejected['error']['code'] == -32009 and not path.exists(), rejected)
        check(rpc('desktop.inspect')['camera'] == scene_camera, 'Leaving Game changed the Scene navigation camera.')
        rpc('desktop.view', {'mode': 'game', 'camera': camera})
        play('stop', {'session_id': sid})
        missing_path, missing_job = queue_capture('missing-authored-camera')
        check(library.poima_desktop_draw(host) < 0, 'Missing authored Game camera did not report a recoverable snapshot error.')
        missing_capture = rpc('desktop.capture.status', {'capture_id': missing_job})
        check(missing_capture['state'] == 'error' and not missing_path.exists(),
              'Failed snapshot left capture queued until timeout or wrote a stale image: '+str(missing_capture))
        check('timed out' not in missing_capture['error']['message'].lower(), missing_capture)
        record['missing_camera_capture'] = missing_capture
        check(rpc('desktop.inspect')['graphics_error'] is None, 'Missing Game camera poisoned the renderer.')
        rpc('desktop.view', {'mode': 'scene'})
        before_recovery = rpc('desktop.inspect')['frames_presented']
        until(lambda state: state['frames_presented'] > before_recovery, draw=True)
        record['final_render'] = rpc('desktop.inspect')['render']
        check(record['final_render']['nvrhi_errors'] == 0, record['final_render'])
        checks.append('Actual Vulkan Game captures preserve frozen camera pose/lens; auto-play holds queued capture, resumes without catch-up, and view changes invalidate capture.')
        checks.append('Missing authored camera after Stop is a recoverable presentation error; Scene restores the same viewport with zero backend errors.')
    else:
        rpc('desktop.view', {'mode': 'scene'})
        play('stop', {'session_id': sid})
        record['limitations'].append('No GPU supplied: viewport captures, capture suspension and presentation recovery were not exercised.')
    unchanged(storage, history)

    # A valid authored clip has positive endpoint scale keys but crosses through
    # zero between them. This produces a real atomic Runtime::step failure.
    doc, blob = ribbon(cubic=True)
    doc['animations'][0]['channels'][0]['target']['path'] = 'scale'
    accessor = doc['accessors'][doc['animations'][0]['samplers'][0]['output']]
    view = doc['bufferViews'][accessor['bufferView']]
    data = bytearray(blob)
    values = [(0, 0, 0), (1, 1, 1), (-4, 0, 0), (4, 0, 0), (1, 1, 1), (0, 0, 0)]
    struct.pack_into('<18f', data, view['byteOffset'], *[v for row in values for v in row])
    doc['buffers'][0]['uri'] = 'data:application/octet-stream;base64,'+base64.b64encode(data).decode()
    asset_path = run/'Scale failure.gltf'; asset_path.write_text(json.dumps(doc), encoding='utf-8')
    asset = rpc('asset.import', {'source': str(asset_path)})['asset']
    rig = uuid.uuid4().hex
    transact([{'op': 'asset.instantiate', 'id': rig, 'asset': asset, 'name': 'Atomic failure rig'}])
    sid = start(paused=True)
    command = {'entity': rig, 'clip': 0, 'time': 0, 'speed': 1, 'loop': True, 'playing': True}
    play('step', step_params(sid, ticks=10, animations=[command]))
    storage, history = world.read_bytes(), rpc('world.history')
    play('resume', {'session_id': sid})
    previous = playback()['tick']
    deadline = time.monotonic()+3
    while time.monotonic() < deadline:
        time.sleep(.05); poll()
        state = playback()
        if state['state'] == 'paused':
            check(state['tick'] == previous and state['last_error'], 'Failed batch partly advanced or did not expose the error.')
            break
        previous = state['tick']
    else:
        raise AssertionError('Invalid sampled animation did not pause real-time playback.')
    record['failed_clock'] = state
    time.sleep(.07); poll()
    check(playback()['tick'] == previous, 'Playback retried a failing tick automatically.')
    play('stop', {'session_id': sid})
    unchanged(storage, history)
    checks.append('A genuine animation sampling failure rolls back the entire batch, pauses playback, exposes the error, and preserves authored bytes/history.')
    record['final_state'] = rpc('desktop.inspect')
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
    print(record.get('error', 'Desktop playback contract failed.'), file=sys.stderr)
sys.exit(0 if record['success'] else 1)
