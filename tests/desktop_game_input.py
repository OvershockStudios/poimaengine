#!/usr/bin/env python3
"""Windows native desktop gameplay-input delivery and optional Vulkan capture holds."""
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
args.output = args.output.resolve()
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
ZERO = {'move': [0, 0], 'look': [0, 0], 'jump': False, 'use': False}
timeline = []
sid = None


def input_state():
    return rpc('desktop.input.inspect')


def input_call(method, params=None, **kwargs):
    return rpc('desktop.input.'+method, params, **kwargs)


def events(batch, *, identity=None, receipt=None, **kwargs):
    return input_call('events', {'session_id': identity or sid, 'request_id': receipt or uuid.uuid4().hex, 'events': batch}, **kwargs)


def control(name, down=True):
    return {'control': name, 'down': down}


def focus(value):
    return input_call('focus', {'session_id': sid, 'focused': value})


def entity(identity=None):
    return rpc('runtime.entity', {'session_id': sid, 'id': identity or controller})


def position(value):
    return value['world_matrix'][12:15]


def check_zero():
    state = input_state()
    check(not state['focused'] and state['pending'] == ZERO, state)
    return state


def pump(delay=.04, draw=False):
    before = rpc('desktop.play.inspect')['tick']
    time.sleep(delay)
    poll()
    after = rpc('desktop.play.inspect')['tick']
    state = input_state()
    if before is not None and after is not None and after > before:
        applied = state['last_applied']
        if applied is not None and applied['first_tick'] == before+1:
            check(applied['ticks'] == after-before, applied)
            frames = applied['frames']
            check(len(frames) == applied['ticks'] and 1 <= len(frames) <= 8, applied)
            check(frames[0] == applied['input'], 'Legacy first-frame metadata changed.')
            for frame in frames:
                timeline.append({'ticks': 1, 'inputs': [frame]})
        else:
            timeline.append({'ticks': after-before, 'inputs': []})
    if draw:
        check(library.poima_desktop_draw(host) >= 0, error(host))
    return state


def clear_lifecycle(action, recover):
    events([control('key.w'), control('key.space'), control('key.e'), {'motion': [10, 20]}])
    check(input_state()['pending'] != ZERO, 'Clear fixture has no pending input.')
    action()
    cleared = check_zero()
    check(cleared['configured'], 'Ordinary input release discarded the selected profile/controller.')
    events([control('key.w')], expected_error=-32009)
    recover()
    focus(True)
    check(input_state()['pending'] == ZERO, 'Reacquiring input restored stale held controls.')
    before = position(entity())
    pump()
    after = position(entity())
    check(math.dist([before[0], before[2]], [after[0], after[2]]) < 1e-6, 'Cleared controls moved the character after reacquisition.')


try:
    project = run/'Game input project'
    created = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Game input contract'],
                             capture_output=True, text=True, encoding='utf-8', timeout=30)
    check(created.returncode == 0, created.stdout+created.stderr)
    manifest = json.loads((project/'project.json').read_text())
    world = project/manifest['entry']['world']
    controller, camera = manifest['entry']['controller'], manifest['entry']['camera']
    endpoint = 'game-input-'+uuid.uuid4().hex
    host = library.poima_desktop_create(str(world).encode(), endpoint.encode(), args.gpu if args.gpu is not None else -1, 4)
    check(host, error())
    revision = rpc('world.inspect')['revision']
    storage, history = world.read_bytes(), rpc('world.history')
    methods = rpc('desktop.describe')['methods']
    for name in ['configure', 'focus', 'events', 'inspect']:
        check('desktop.input.'+name in methods, 'Input method is not discoverable: '+name)
    check('desktop.controllers' in methods, 'Controller discovery is absent.')
    check_zero()
    if args.gpu is not None:
        parent_hwnd, child_hwnd = make_window()
        check(library.poima_desktop_attach(host, child_hwnd) == 1, error(host))
        until(lambda state: state['frames_presented'] >= 2, draw=True, timeout=20)

    sid = uuid.uuid4().hex
    rpc('desktop.play.start', {'session_id': sid, 'revision': revision, 'paused': True})
    rpc('desktop.play.step', {'session_id': sid, 'expected_tick': 0, 'request_id': uuid.uuid4().hex, 'ticks': 120})
    timeline.append({'ticks': 120, 'inputs': []})
    available = rpc('desktop.controllers')
    check(available['source'] == 'runtime' and {'id': controller, 'camera': camera} in available['controllers'], available)
    configured = input_call('configure', {'session_id': sid, 'controller': controller})
    check(configured['configured'] and not configured['focused'] and configured['profile']['source'] == 'defaults', configured)
    input_call('focus', {'session_id': sid, 'focused': True}, expected_error=-32009)
    rpc('desktop.play.resume', {'session_id': sid})
    input_call('focus', {'session_id': sid, 'focused': True}, expected_error=-32009)
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    focus(True)
    checks.append('Frozen controller discovery and strict playing/Game-camera focus gates work without requiring graphics.')

    baseline = entity()
    check(baseline['ground'] == 'on_ground', baseline)
    receipt = uuid.uuid4().hex
    first_batch = [control('key.w'), control('key.space'), control('key.e'), {'motion': [30, -20]}]
    accepted = events(first_batch, receipt=receipt)
    pending = input_state()
    check(pending['pending'] == {'move': [0, 1], 'look': [-3, 2], 'jump': True, 'use': True}, pending)
    check(events(first_batch, receipt=receipt)['replayed'] is True, 'Exact input retry was not recognized.')
    check(input_state() == pending, 'Exact input retry reapplied relative motion or edges.')
    events([{'motion': [1, 1]}], receipt=receipt, expected_error=-32010)
    focus(True)
    check(input_state() == pending, 'Repeated focus acquisition cleared input.')
    bad_batches = [[control('key.a'), control('unknown.control')], [control('key.escape')], [control('gamepad.south')],
                   [{'control': 'key.a', 'down': 1}], [{'motion': [1, True]}],
                   [{'motion': [1, 2], 'control': 'key.a', 'down': True}], [control('key.a')]*257]
    for batch in bad_batches:
        events(batch, expected_error=-32602)
        check(input_state() == pending, 'Invalid event batch partially changed evaluator state or receipts.')
    failed_receipt = uuid.uuid4().hex
    events([control('bad.control')], receipt=failed_receipt, expected_error=-32602)
    events([], receipt=failed_receipt)
    state = pump()
    applied = state['last_applied']['input']
    check(applied == {'entity': controller, **pending['pending']}, applied)
    moved = entity()
    check(position(moved)[2] < position(baseline)[2]-.03 and position(moved)[1] > position(baseline)[1]+.03,
          'Accepted input did not move and jump the actual CharacterController.')
    check(abs(moved['yaw']+3) < 1e-6 and abs(moved['pitch']-2) < 1e-6, moved)
    check(state['pending'] == {'move': [0, 1], 'look': [0, 0], 'jump': False, 'use': False}, state)
    events([control('key.space'), control('key.e')])
    state = pump()
    check(state['last_applied']['input'] == {'entity': controller, 'move': [0, 1], 'look': [0, 0], 'jump': False, 'use': False}, state)
    check(abs(entity()['yaw']+3) < 1e-6, 'Mouse motion was applied again on a later batch.')
    events([control('key.w', False)])
    # Keep Space/E held through landing: neither must synthesize another edge.
    for _ in range(12):
        state = pump(.14)
        check(not state['last_applied']['input']['jump'] and not state['last_applied']['input']['use'], state)
    grounded = entity()
    check(grounded['ground'] == 'on_ground' and abs(position(grounded)[1]-position(baseline)[1]) < .02, grounded)
    check(math.dist(position(grounded)[::2], position(moved)[::2]) < 1, 'Released movement did not stop.')
    focus(False)
    check(events(first_batch, receipt=receipt)['replayed'] is True, 'Paused/focus-lost retry must remain harmless.')
    check_zero(); focus(True)
    checks.append('Real controller movement/jump/yaw matches evaluated controls; held movement persists, while look/jump/use consume once and repeat events/receipts cannot retrigger them.')
    checks.append('Invalid batches validate atomically; reserved/gamepad/type/shape/count errors preserve pending input and failed receipts are reusable.')

    # A mouse displacement can span several fixed ticks. Treating one pump as
    # a batch with first-tick look loses the remaining 250 degrees here.
    before_backlog = entity()['yaw']
    events([control('key.w'), control('key.space'), control('key.e'), {'motion': [4300, 0]}])
    burst = pump(.15)
    frames = burst['last_applied']['frames']
    check(len(frames) == 8, 'Catch-up fixture did not exercise the eight-tick budget.')
    check([frame['look'] for frame in frames] == [[-180, 0], [-180, 0], [-70, 0]]+[[0, 0]]*5, frames)
    check(all(frame['move'] == [0, 1] for frame in frames), frames)
    check(frames[0]['jump'] and frames[0]['use'] and all(not frame['jump'] and not frame['use'] for frame in frames[1:]), frames)
    check(abs(entity()['yaw']-math.remainder(before_backlog-430, 360)) < 1e-6, 'Committed catch-up lost mouse backlog.')
    check(burst['pending'] == {'move': [0, 1], 'look': [0, 0], 'jump': False, 'use': False}, burst)
    events([control('key.w', False), control('key.space', False), control('key.e', False)])
    checks.append('Eight-tick catch-up drains independent mouse frames -180/-180/-70 then zero, preserves held movement, and consumes jump/use once; exact frames feed independent replay.')

    clear_lifecycle(lambda: focus(False), lambda: None)
    clear_lifecycle(lambda: rpc('desktop.play.pause', {'session_id': sid}),
                    lambda: rpc('desktop.play.resume', {'session_id': sid}))
    clear_lifecycle(lambda: rpc('desktop.view', {'mode': 'scene'}),
                    lambda: rpc('desktop.view', {'mode': 'game', 'camera': camera}))
    if args.gpu is not None:
        def reattach():
            check(library.poima_desktop_attach(host, child_hwnd) == 1, error(host))
        clear_lifecycle(lambda: library.poima_desktop_detach(host), reattach)
    checks.append('Focus loss, Pause and Scene switch clear held controls and pending edges; explicit reacquisition does not restore stale movement.')

    profile_path = run/'rebound.poima-input.json'
    profile = {'bindings': {'forward': ['key.up'], 'backward': ['key.down'], 'left': ['key.left'], 'right': ['key.right'],
                            'jump': ['mouse.3'], 'use': ['key.f1']},
               'sensitivity_x': .5, 'sensitivity_y': .25, 'invert_x': True, 'invert_y': False}
    saved = rpc('input.transact', {'path': str(profile_path), 'expected_revision': 0, 'request_id': uuid.uuid4().hex, 'profile': profile})
    profile_bytes = profile_path.read_bytes()
    before_config = input_state()
    for params in [{'session_id': sid, 'controller': camera},
                   {'session_id': sid, 'controller': controller, 'input_profile': str(profile_path), 'input_revision': 0}]:
        input_call('configure', params, expected_error='any')
        check(input_state() == before_config, 'Failed controller/profile configuration changed input state.')
    configured = input_call('configure', {'session_id': sid, 'controller': controller, 'input_profile': str(profile_path), 'input_revision': 1})
    check_zero()
    check(configured['profile']['source'] == 'profile' and configured['profile']['content_hash'] == saved['content_hash'], configured)
    focus(True)
    events([control('key.w')])
    check(input_state()['pending']['move'] == [0, 0], 'Old W binding survived profile replacement.')
    before_rebound = entity()
    events([control('key.up'), {'motion': [4, 8]}])
    check(input_state()['pending']['move'] == [0, 1] and input_state()['pending']['look'] == [2, -2], input_state())
    pump()
    rebound = entity()
    check(position(rebound)[2] < position(before_rebound)[2]-.03 and abs(rebound['yaw']-before_rebound['yaw']-2) < 1e-6
          and abs(rebound['pitch']-before_rebound['pitch']+2) < 1e-6, rebound)
    focus(False); focus(True)
    checks.append('Persisted rebound profile changes actual movement and yaw/pitch; stale profile revisions reject without disturbing the old evaluator.')

    if args.gpu is not None:
        captured_path = run/'held-input-capture.bmp'
        job = rpc('desktop.capture', {'revision': revision, 'path': str(captured_path)})
        events([control('key.up'), control('key.f1'), {'motion': [6, 0]}])
        held = input_state(); held_tick = rpc('desktop.play.inspect')['tick']
        for _ in range(3):
            pump(.05)
            check(input_state() == held and rpc('desktop.play.inspect')['tick'] == held_tick,
                  'Capture hold consumed pending input or advanced the runtime.')
        deadline = time.monotonic()+10
        while True:
            pump(0, draw=True)
            done = rpc('desktop.capture.status', {'capture_id': job['capture_id']})
            if done['state'] != 'queued' or time.monotonic() >= deadline: break
        check(done['state'] == 'complete' and done['result']['tick'] == held_tick, done)
        record['capture'] = {'result': done['result'], 'image': bmp(captured_path, WIDTH, HEIGHT)}
        check(input_state() == held, 'Presentation consumed gameplay input.')
        pump(.03)  # First post-capture poll discards the held interval.
        check(input_state() == held, 'First post-capture hold release consumed gameplay input.')
        applied = pump(.04)['last_applied']
        check(applied['input']['look'] == [3, 0] and applied['input']['use'] and applied['input']['move'] == [0, 1], applied)
        again = pump()['last_applied']['input']
        check(again['look'] == [0, 0] and not again['use'] and again['move'] == [0, 1], again)
        check(rpc('desktop.inspect')['render']['nvrhi_errors'] == 0, rpc('desktop.inspect'))
        checks.append('Real Vulkan capture holds input and clock without consumption; the next simulated batch receives retained movement/look/use exactly once.')
    else:
        record['limitations'].append('No GPU supplied: capture hold and native viewport detach checks were not run.')

    final_controller, final_camera = entity(), entity(camera)
    original_session = sid
    rpc('desktop.play.stop', {'session_id': sid})
    cleared = check_zero()
    check(not cleared['configured'] and cleared['last_applied'] is None, cleared)
    sid = uuid.uuid4().hex
    rpc('desktop.play.start', {'session_id': sid, 'revision': revision, 'paused': True})
    events([control('key.up')], identity=original_session, expected_error=-32009)
    check(not input_state()['configured'], 'A new runtime inherited prior-session input configuration.')
    tick = 0
    for segment in timeline:
        rpc('desktop.play.step', {'session_id': sid, 'expected_tick': tick, 'request_id': uuid.uuid4().hex, **segment})
        tick += segment['ticks']
    for observed, independent in [(final_controller, entity()), (final_camera, entity(camera))]:
        left, right = dict(observed), dict(independent)
        left.pop('session_id'); right.pop('session_id')
        check(left == right, {'delivered': left, 'independent_replay': right})
    record['semantic_replay_ticks'] = tick
    record['delivered_batches'] = timeline
    record['final_controller'] = final_controller
    rpc('desktop.play.stop', {'session_id': sid})
    check(world.read_bytes() == storage and rpc('world.history') == history, 'Gameplay input mutated the authored world or undo history.')
    check(profile_path.read_bytes() == profile_bytes, 'Read-only profile selection changed saved bytes.')
    checks.append('Stop/new session drops configuration and rejects stale events; independent semantic replay exactly matches final controller/camera state with unchanged authored bytes/history.')
    # Separate mutation phase: preserve the unchanged-authoring proof above,
    # then require controller discovery/configuration to use frozen definitions.
    timeline = []
    sid = uuid.uuid4().hex
    rpc('desktop.play.start', {'session_id': sid, 'revision': revision, 'paused': True})
    rpc('desktop.play.step', {'session_id': sid, 'expected_tick': 0, 'request_id': uuid.uuid4().hex, 'ticks': 120})
    deleted = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex,
                  'ops': [{'op': 'entity.delete', 'id': controller, 'recursive': True}]})
    revision = deleted['revision']
    authored_entities = json.loads(world.read_text())['entities']
    check(controller not in authored_entities and camera not in authored_entities, 'Deletion fixture did not remove authored controller/camera.')
    frozen = rpc('desktop.controllers')
    check(frozen['source'] == 'runtime' and {'id': controller, 'camera': camera} in frozen['controllers'], frozen)
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    input_call('configure', {'session_id': sid, 'controller': controller})
    rpc('desktop.play.resume', {'session_id': sid})
    focus(True)
    before_frozen_move = entity()
    events([control('key.w')])
    pump()
    after_frozen_move = entity()
    check(position(after_frozen_move)[2] < position(before_frozen_move)[2]-.03,
          'Authoring deletion prevented controlling the frozen runtime character.')
    record['frozen_after_authored_deletion'] = {'controllers': frozen, 'before': before_frozen_move, 'after': after_frozen_move}
    rpc('desktop.play.stop', {'session_id': sid})
    authored_controllers = rpc('desktop.controllers')
    check(authored_controllers['source'] == 'authored' and not authored_controllers['controllers'], authored_controllers)
    check(not input_state()['configured'], 'Stop retained deleted runtime controller configuration.')
    rpc('desktop.view', {'mode': 'scene'})
    checks.append('After actual recursive authored deletion, frozen runtime controller/camera discovery, configuration and movement remain valid; Stop restores the now-empty authored controller list.')
    record['final_state'] = rpc('desktop.inspect')
    record['success'] = True
except BaseException:
    record['error'] = traceback.format_exc()
finally:
    if host: library.poima_desktop_destroy(host)
    for window in reversed(windows):
        if user32.IsWindow(window): user32.DestroyWindow(window)
    search.close()
    (run/'result.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
print(json.dumps({'success': record['success'], 'evidence': str(run/'result.json'), 'checks': len(checks)}))
if not record['success']: print(record.get('error', 'Desktop game input contract failed.'), file=sys.stderr)
sys.exit(0 if record['success'] else 1)
