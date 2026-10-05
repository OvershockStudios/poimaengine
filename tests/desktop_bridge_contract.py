#!/usr/bin/env python3
"""Windows C ABI + optional real-HWND Vulkan qualification for the desktop host."""
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
from animation_fixture import ribbon

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
        check(reply.get('error', {}).get('code') == expected_error, f'{method}: {reply}')
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

try:
    project = run/'Source project'
    created = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Desktop bridge room'],
                             capture_output=True, text=True, encoding='utf-8', timeout=30)
    check(created.returncode == 0, created.stdout+created.stderr)
    manifest = json.loads((project/'project.json').read_text(encoding='utf-8'))
    world = project/manifest['entry']['world']
    endpoint = 'desktop-test-'+uuid.uuid4().hex
    host = library.poima_desktop_create(str(world).encode('utf-8'), endpoint.encode('ascii'),
                                       args.gpu if args.gpu is not None else -1, 4)
    check(host, f'Create failed: {error()}')
    initial = poll()
    check(initial['world_changed'] and initial['runtime_changed'] and not initial['attached'], initial)
    check(not poll()['world_changed'], 'Unchanged world triggers a refresh every poll.')
    check(library.poima_desktop_draw(host) == 0, 'Unattached draw did not return transient/unavailable zero.')
    duplicate = library.poima_desktop_create(str(world).encode('utf-8'), b'another-endpoint', -1, 4)
    check(not duplicate and 'one desktop host' in error().lower(), 'Duplicate host was not explicitly rejected.')
    check(not library.poima_desktop_call(VP(1), b'{}'), 'Invalid opaque handle was accepted.')
    checks.append('No graphics at create; unchanged polling and unattached draw; duplicate/invalid handle rejection.')
    schema = rpc('desktop.describe')
    check('desktop.capture.status' in schema['methods'], 'Capture jobs are not discoverable.')
    check(rpc('world.describe')['editor_discovery'] == 'desktop.describe', 'World discovery points at the wrong frontend.')
    check(rpc('desktop.inspect')['revision'] == initial['revision'], 'Discovery changed the world.')
    malformed = library.poima_desktop_call(host, b'{')
    check(json.loads(decode(malformed))['error']['code'] == -32700, 'Malformed JSON did not return a parse error.')
    rpc('desktop.inspect', notification=True)
    rpc('session.close', expected_error=-32080)
    rpc('session.close', expected_error=None, notification=True)
    rpc('runtime.play', expected_error=-32080)
    check(rpc('world.inspect')['revision'] == initial['revision'], 'Restricted commands changed session state.')
    checks.append('Discoverable JSON-RPC methods, malformed request/notification behavior, shared-owner/graphics restrictions.')
    before = rpc('desktop.inspect')
    with ThreadPoolExecutor(max_workers=1) as workers:
        def wrong_thread():
            pointer = library.poima_desktop_call(host, b'{"jsonrpc":"2.0","id":1,"method":"world.inspect"}')
            return bool(pointer), error(host)
        accepted, detail = workers.submit(wrong_thread).result(timeout=3)
    check(not accepted and 'thread' in detail.lower(), f'Wrong-thread call did not reject: {detail}')
    check(rpc('desktop.inspect')['revision'] == before['revision'], 'Wrong-thread rejection mutated world state.')
    checks.append('Off-thread calls reject without transferring WorldSession ownership or mutating state.')
    entity = uuid.uuid4().hex
    requests = [{'jsonrpc': '2.0', 'id': 1, 'method': 'world.transact', 'params': {
        'base_revision': initial['revision'], 'request_id': uuid.uuid4().hex,
        'ops': [{'op': 'entity.create', 'id': entity, 'name': 'CLI shared edit'}]}},
        {'jsonrpc': '2.0', 'id': 2, 'method': 'desktop.inspect'}]
    with ThreadPoolExecutor(max_workers=1) as workers:
        future = workers.submit(client, requests)
        until(lambda _: future.done())
        replies = future.result()
    check(all('result' in row for row in replies), replies)
    revision = initial['revision']+1
    check(replies[-1]['result']['revision'] == revision and rpc('entity.get', {'id': entity})['value']['name'] == 'CLI shared edit', replies)
    rpc('desktop.select', {'id': entity})
    check(rpc('desktop.inspect')['selected'] == entity, 'Native selection did not retain the selected ID.')
    rpc('world.transact', {'base_revision': initial['revision'], 'request_id': uuid.uuid4().hex,
                         'ops': [{'op': 'entity.rename', 'id': entity, 'name': 'Stale edit'}]}, expected_error=-32009)
    check(rpc('world.inspect')['revision'] == revision, 'Stale revision changed the world.')
    checks.append('Independent CLI process shares authoritative revision/entities and stale edits remain atomic.')
    camera_before = rpc('desktop.inspect')['camera']
    rpc('desktop.camera', {'pitch': True}, expected_error=-32602)
    check(rpc('desktop.inspect')['camera'] == camera_before, 'Invalid camera patch changed the pose.')
    rpc('desktop.camera', {'position': [6, 4, 8], 'yaw': 37, 'pitch': -22})
    rpc('desktop.capture', {'revision': revision, 'path': str(run/'Unattached.bmp')}, expected_error=-32003)
    check(not (run/'Unattached.bmp').exists(), 'Capture without a viewport wrote output.')
    check(library.poima_desktop_attach(host, None) == 0, 'Null HWND was accepted.')
    check(rpc('world.inspect')['revision'] == revision, 'Invalid attach closed the world.')
    checks.append('Camera patches validate before mutation; unavailable captures/invalid HWND preserve authoring.')

    # These geometry/navigation contracts deliberately run without an HWND or GPU.
    check({'desktop.pick', 'desktop.frame'} <= schema['methods'].keys(), 'Picking/framing are not discoverable.')
    group, front, rear, hidden, rotated, triangle = [uuid.uuid4().hex for _ in range(6)]
    def transform(position, rotation=None, scale=None):
        return {'position': position, 'rotation': rotation or [0, 0, 0, 1], 'scale': scale or [1, 1, 1]}
    def set_component(identity, kind, value):
        return {'op': 'component.set', 'id': identity, 'type': kind, 'value': value}
    def transact(ops):
        global revision
        revision = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex, 'ops': ops})['revision']
    def create(identity, name, position, parent=None, visible=None, rotation=None):
        result = [{'op': 'entity.create', 'id': identity, 'name': name, 'parent': parent},
                  set_component(identity, 'Transform', transform(position, rotation))]
        if visible is not None:
            result.append(set_component(identity, 'MeshRenderer', {'primitive': 'box', 'albedo': [.5, .5, .5], 'visible': visible}))
        return result
    triangle_bytes = struct.pack('<9f', -1, -1, 0, 1, -1, 0, -1, 1, 0)
    triangle_doc = {'asset': {'version': '2.0'},
                    'buffers': [{'byteLength': len(triangle_bytes), 'uri': 'data:application/octet-stream;base64,'+base64.b64encode(triangle_bytes).decode()}],
                    'bufferViews': [{'buffer': 0, 'byteLength': len(triangle_bytes)}],
                    'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [-1, -1, 0], 'max': [1, 1, 0]}],
                    'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
                    'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    triangle_path = run/'Pick triangle.gltf'
    triangle_path.write_text(json.dumps(triangle_doc), encoding='utf-8')
    imported = rpc('asset.import', {'source': str(triangle_path)})['asset']
    transact(create(group, 'Framed group', [1000, 0, 0]) + create(front, 'Front', [0, 0, 0], group, True)
             + create(rear, 'Rear', [0, 0, -3], group, True) + create(hidden, 'Hidden', [0, 0, 2], group, False)
             + create(rotated, 'Rotated box', [1040, 0, 0], visible=True, rotation=[0, 0, math.sin(math.pi/8), math.cos(math.pi/8)])
             + create(triangle, 'Actual triangle', [1020, 0, 0])
             + [set_component(triangle, 'StaticMesh', {'asset': imported, 'primitive': 0, 'visible': True})])
    rpc('desktop.camera', {'position': [1000, 0, 5], 'yaw': 0, 'pitch': 0, 'near': .1, 'far': 1000, 'vertical_fov': 60})
    def pick(x=.5, y=.5, **overrides):
        return rpc('desktop.pick', {'revision': revision, 'x': x, 'y': y, 'aspect': 1, **overrides})
    unchanged = world.read_bytes()
    before = rpc('desktop.inspect')
    hit = pick()
    check(hit['id'] == front and abs(hit['distance']-4.5) < 1e-9 and hit['source'] == 'authored' and hit['tick'] is None, hit)
    check(pick(0, 0)['id'] is None, 'Empty ray selected an object.')
    for method, params in [('desktop.pick', {'revision': revision-1, 'x': .5, 'y': .5, 'aspect': 1}),
                           ('desktop.frame', {'revision': revision-1, 'id': group, 'aspect': 1})]:
        rpc(method, params, expected_error=-32009)
    for key, value in [('x', -1), ('y', 1.1), ('x', True), ('aspect', 0), ('aspect', 101), ('aspect', '1'), ('extra', 1)]:
        rpc('desktop.pick', {'revision': revision, 'x': .5, 'y': .5, 'aspect': 1, key: value}, expected_error=-32602)
    rpc('desktop.frame', {'revision': revision, 'id': uuid.uuid4().hex, 'aspect': 1}, expected_error=-32004)
    rpc('desktop.frame', {'revision': revision, 'id': group, 'aspect': False}, expected_error=-32602)
    check(rpc('desktop.inspect')['camera'] == before['camera'] and rpc('desktop.inspect')['selected'] == before['selected'], 'Query/error changed navigation state.')
    check(world.read_bytes() == unchanged, 'Queries/errors rewrote the authored world.')
    rpc('desktop.camera', {'near': 6})
    check(pick()['id'] == rear, 'Near clipping did not exclude the front box.')
    rpc('desktop.camera', {'near': .1, 'far': 4})
    check(pick()['id'] is None, 'Far clipping selected an invisible box.')
    rpc('desktop.camera', {'far': 1000, 'position': [1020, 0, 5]})
    def projected(x, y):
        return .5+x/(10*math.tan(math.pi/6)), .5-y/(10*math.tan(math.pi/6))
    check(pick(*projected(.6, .6))['id'] is None, 'Triangle AABB empty corner was falsely selected.')
    check(pick(*projected(-.5, -.5))['id'] == triangle, 'Triangle interior did not hit.')
    rpc('desktop.camera', {'position': [1020, 0, -5], 'yaw': 180})
    check(pick(*projected(.5, -.5))['id'] is None, 'Backface-only triangle was selected.')
    rpc('desktop.camera', {'position': [1040, 0, 5], 'yaw': 0})
    check(pick(*projected(.6, .6))['id'] is None and pick()['id'] == rotated, 'Rotated box used a world AABB instead of its actual shape.')
    check(world.read_bytes() == unchanged, 'Picking rewrote world storage.')
    checks.append('GPU-independent picking: nearest visible geometry, no-hit, top-left coordinates, true triangle/rotated-box intersections, clipping and strict atomic guards.')

    rpc('desktop.select', {'id': group})
    framed = rpc('desktop.frame', {'revision': revision, 'aspect': .5})
    check(math.dist(framed['target'], [1000, 0, -1.5]) < 1e-4 and framed['distance'] > 0, framed)
    check(framed['camera'] == rpc('desktop.inspect')['camera'] and framed['camera']['yaw'] == 0 and framed['camera']['pitch'] == 0, framed)
    # Independently project all combined corners; padding keeps both axes inside.
    for x in [999.5, 1000.5]:
        for y in [-.5, .5]:
            for z in [-3.5, .5]:
                camera = framed['camera']; depth = camera['position'][2]-z
                check(camera['near'] < depth < camera['far'], 'Framing clipped a corner in depth.')
                check(abs(x-camera['position'][0]) < depth*math.tan(math.pi/6)*.5 and abs(y-camera['position'][1]) < depth*math.tan(math.pi/6), 'Framing does not fit the supplied aspect.')
    empty_frame = rpc('desktop.frame', {'revision': revision, 'id': entity, 'aspect': 1})
    check(empty_frame['target'] == [0, 0, 0], 'Non-rendered entity did not frame its world origin.')
    rpc('desktop.select', {'id': None})
    rpc('desktop.frame', {'revision': revision, 'aspect': 1}, expected_error=-32602)
    check(world.read_bytes() == unchanged, 'Framing mutated authored bytes.')
    checks.append('Framing combines visible descendants, respects aspect/clipping, preserves orientation, supports non-rendered entities and never edits world bytes.')

    if rpc('desktop.inspect')['runtime']['available']:
        navigation_session = uuid.uuid4().hex
        rpc('runtime.start', {'session_id': navigation_session, 'revision': revision})
        frozen_revision = revision
        transact([{'op': 'entity.delete', 'id': front, 'recursive': True},
                  {'op': 'entity.reparent', 'id': rear, 'parent': None, 'mode': 'keep_local'}])
        rpc('desktop.camera', {'position': [1000, 0, 5], 'yaw': 0, 'pitch': 0})
        live_hit = pick()
        check(live_hit['id'] == front and live_hit['source'] == 'runtime' and live_hit['tick'] == 0 and live_hit['scene_revision'] == frozen_revision, live_hit)
        live_frame = rpc('desktop.frame', {'revision': revision, 'id': group, 'aspect': 1})
        check(math.dist(live_frame['target'], [1000, 0, -1.5]) < 1e-4, 'Runtime framing used the changed authored hierarchy.')
        deleted_frame = rpc('desktop.frame', {'revision': revision, 'id': front, 'aspect': 1})
        check(deleted_frame['target'] == [1000, 0, 0], 'Runtime-only entity could not be framed.')
        check(rpc('runtime.inspect', {'session_id': navigation_session})['tick'] == 0, 'Navigation advanced simulation.')
        rpc('runtime.stop', {'session_id': navigation_session})
        revision = rpc('world.undo', {'base_revision': revision, 'request_id': uuid.uuid4().hex})['revision']
        checks.append('Runtime picking/framing uses frozen geometry and hierarchy after authored deletion/reparenting without advancing ticks.')

    # Analytic skin: editing the tip joint translates the top row by two metres;
    # pick must follow the posed triangles, not the original vertices or bounds.
    rig = uuid.uuid4().hex
    skin_doc, skin_bytes = ribbon()
    skin_doc['buffers'][0]['uri'] = 'data:application/octet-stream;base64,'+base64.b64encode(skin_bytes).decode()
    skin_path = run/'Pick skin.gltf'
    skin_path.write_text(json.dumps(skin_doc), encoding='utf-8')
    skin_asset = rpc('asset.import', {'source': str(skin_path)})['asset']
    transact([{'op': 'asset.instantiate', 'id': rig, 'asset': skin_asset, 'name': 'Pick skin'},
              set_component(rig, 'Transform', transform([1060, 0, 0]))])
    skin_entities = rpc('entity.query', {'revision': revision, 'component': 'SkinnedMesh', 'limit': 256})['entities']
    skin_entity = next(e['id'] for e in skin_entities if rpc('entity.get', {'id': e['id']})['value']['components']['SkinnedMesh']['rig'] == rig)
    rig_nodes = rpc('entity.query', {'revision': revision, 'component': 'RigNode', 'limit': 256})['entities']
    tip = next(e['id'] for e in rig_nodes if rpc('entity.get', {'id': e['id']})['value']['components']['RigNode'] == {'rig': rig, 'node': 1})
    rpc('desktop.camera', {'position': [1060, 1.8, 5], 'yaw': 0, 'pitch': 0})
    check(pick()['id'] == skin_entity, 'Rest skin geometry did not hit.')
    transact([set_component(tip, 'Transform', transform([2, 1, 0]))])
    check(pick()['id'] is None, 'Posed skin picked its obsolete rest shape.')
    rpc('desktop.camera', {'position': [1061.8, 1.8, 5]})
    check(pick()['id'] == skin_entity, 'Posed skin triangle was not pickable at its deformed position.')
    checks.append('GPU-independent skin picking follows edited joint poses and excludes obsolete rest geometry.')
    transact([{'op': 'entity.delete', 'id': identity, 'recursive': True} for identity in [group, rotated, triangle, rig]])
    rpc('desktop.select', {'id': entity})
    rpc('desktop.camera', camera_before)

    if args.gpu is not None:
        parent, child = make_window()
        original_proc = user32.GetWindowLongPtrW(child, -4)
        check(library.poima_desktop_attach(host, child) == 1, f'Attach failed: {error(host)}')
        check(library.poima_desktop_attach(host, child) == 0, 'A second viewport was accepted without detach.')
        until(lambda status: status['frames_presented'] >= 2, draw=True, timeout=20)
        current = rpc('desktop.inspect')
        check(current['render']['hardware'] and current['render']['nvrhi_errors'] == 0, current)
        framing_path = run/'Framing changed.bmp'
        framing_job = rpc('desktop.capture', {'revision': revision, 'path': str(framing_path)})
        pick()
        check(rpc('desktop.capture.status', {'capture_id': framing_job['capture_id']})['state'] == 'queued', 'Pick invalidated a pending capture.')
        rpc('desktop.frame', {'revision': revision, 'id': entity, 'aspect': 760/520})
        framing_status = rpc('desktop.capture.status', {'capture_id': framing_job['capture_id']})
        check(framing_status['state'] == 'error' and framing_status['error']['code'] == -32009 and not framing_path.exists(), framing_status)
        rpc('desktop.camera', camera_before)
        checks.append('Picking preserves pending capture; framing changes camera generation and rejects stale capture without a file.')
        rpc('desktop.capture', {'revision': revision-1, 'path': str(run/'Stale.bmp')}, expected_error=-32009)
        sentinel = run/'Existing.bmp'
        sentinel.write_bytes(b'preserve')
        rpc('desktop.capture', {'revision': revision, 'path': str(sentinel)}, expected_error=-32602)
        check(sentinel.read_bytes() == b'preserve', 'Existing capture destination was changed.')
        record['capture_preflight_failures'] = []
        for case in ('destination appeared', 'parent disappeared'):
            folder = run/('Capture '+case)
            folder.mkdir()
            destination = folder/'Capture.bmp'
            before = rpc('desktop.inspect')['frames_presented']
            queued = rpc('desktop.capture', {'revision': revision, 'path': str(destination)})
            if case == 'destination appeared':
                destination.write_bytes(b'preserve late destination')
            else:
                folder.rmdir()
            check(library.poima_desktop_draw(host) >= 0, f'Capture preflight poisoned rendering: {error(host)}')
            failed = rpc('desktop.capture.status', {'capture_id': queued['capture_id']})
            check(failed['state'] == 'error' and failed['error']['code'] == -32602, failed)
            status = until(lambda value: value['frames_presented'] > before, draw=True)
            check(status['graphics_error'] is None and status['attached'], status)
            if case == 'destination appeared':
                check(destination.read_bytes() == b'preserve late destination', 'Late destination was overwritten.')
            else:
                check(not destination.exists(), 'Capture recreated a missing destination parent.')
            record['capture_preflight_failures'].append({'case': case, 'job': failed,
                                                         'frames_before': before,
                                                         'frames_after': status['frames_presented']})
        checks.append('Capture preflight failures preserve late files and continue drawing without viewport reattachment.')
        capture = run/'Authored.bmp'
        job = rpc('desktop.capture', {'revision': revision, 'path': str(capture)})
        check(job['state'] == 'queued' and not capture.exists(), 'Capture reported completion before drawing.')
        check(rpc('desktop.capture.status', {'capture_id': job['capture_id']})['state'] == 'queued', 'Status secretly rendered a capture.')
        rpc('desktop.capture', {'revision': revision, 'path': str(run/'Extra.bmp')}, expected_error=-32009)
        until(lambda status: status['capture']['state'] != 'queued', draw=True, timeout=20)
        completed = rpc('desktop.capture.status', {'capture_id': job['capture_id']})
        check(completed['state'] == 'complete', completed)
        record['authored_capture'] = bmp(capture, completed['result']['width'], completed['result']['height'])
        checks.append('Real child-HWND Vulkan draws; capture remains queued until presentation; exclusive destinations and bounded pending job.')

        session = uuid.uuid4().hex
        rpc('runtime.start', {'session_id': session, 'revision': revision})
        for _ in range(5):
            poll(draw=True)
        check(rpc('runtime.inspect', {'session_id': session})['tick'] == 0, 'Drawing advanced simulation automatically.')
        stale_path = run/'Tick changed.bmp'
        stale_job = rpc('desktop.capture', {'revision': revision, 'path': str(stale_path)})
        rpc('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'ticks': 1})
        stale_status = rpc('desktop.capture.status', {'capture_id': stale_job['capture_id']})
        check(stale_status['state'] == 'error' and stale_status['error']['code'] == -32009 and not stale_path.exists(), stale_status)
        runtime_capture = run/'Runtime.bmp'
        runtime_job = rpc('desktop.capture', {'revision': revision, 'path': str(runtime_capture)})
        until(lambda status: status['capture']['state'] != 'queued', draw=True, timeout=20)
        result = rpc('desktop.capture.status', {'capture_id': runtime_job['capture_id']})
        check(result['state'] == 'complete' and result['result']['source'] == 'runtime' and result['result']['tick'] == 1, result)
        check(rpc('runtime.inspect', {'session_id': session})['tick'] == 1, 'Capture advanced simulation.')
        record['runtime_capture'] = bmp(runtime_capture, result['result']['width'], result['result']['height'])
        checks.append('Runtime snapshots are captured at explicit ticks; unchanged draws do not advance physics; stale-tick capture produces no file.')

        user32.ShowWindow(child, 0)
        hidden_path = run/'Hidden.bmp'
        hidden = rpc('desktop.capture', {'revision': revision, 'path': str(hidden_path)})
        until(lambda status: status['capture']['state'] != 'queued', draw=True, timeout=5)
        hidden_status = rpc('desktop.capture.status', {'capture_id': hidden['capture_id']})
        check(hidden_status['state'] == 'error' and hidden_status['error']['code'] == -32003 and not hidden_path.exists(), hidden_status)
        user32.ShowWindow(child, 8)
        check(user32.SetWindowPos(child, None, 0, 0, 640, 360, 0x0014), 'Child resize failed.')
        until(lambda status: status['render']['width'] == 640 and status['render']['height'] == 360, draw=True)
        queued = rpc('desktop.capture', {'revision': revision, 'path': str(run/'Detached.bmp')})
        before_detach = rpc('desktop.inspect')
        library.poima_desktop_detach(host)
        check(not error(host), f'Detach failed: {error(host)}')
        detached = rpc('desktop.inspect')
        check(not detached['attached'] and detached['render'] == before_detach['render'], 'Detach lost the last render evidence.')
        check(detached['revision'] == revision and detached['runtime']['tick'] == 1, 'Detach changed authoritative state.')
        check(rpc('desktop.capture.status', {'capture_id': queued['capture_id']})['state'] == 'error', 'Detach left a pending capture hanging.')
        check(user32.IsWindow(child) and user32.GetWindowLongPtrW(child, -4) == original_proc, 'Detach destroyed the frontend HWND or did not restore its WndProc.')
        check(library.poima_desktop_attach(host, child) == 1, f'Reattach failed: {error(host)}')
        frames = detached['frames_presented']
        until(lambda status: status['frames_presented'] > frames, draw=True, timeout=20)
        check(rpc('desktop.inspect')['runtime']['tick'] == 1, 'Reattach reset runtime state.')
        checks.append('Hidden viewport capture times out; resize observes actual client extent; detach/recreate preserves world/runtime and frontend HWND.')
        record['final_render'] = rpc('desktop.inspect')['render']
        check(record['final_render']['nvrhi_errors'] == 0, record['final_render'])
    else:
        record['limitations'].append('No --gpu supplied: real HWND/Vulkan draw/capture/lifecycle checks were not run.')

    record['final_state'] = rpc('desktop.inspect')
    if args.gpu is not None:
        record['child_hwnd_survived_before_destroy'] = bool(user32.IsWindow(child))
    library.poima_desktop_destroy(host)
    check(not error(), f'Destroy failed: {error()}')
    host = None
    if args.gpu is not None:
        check(user32.IsWindow(child) and user32.GetWindowLongPtrW(child, -4) == original_proc, 'Host destruction invalidated frontend-owned HWND.')
    reopened = library.poima_desktop_create(str(world).encode('utf-8'), endpoint.encode('ascii'), -1, 4)
    check(reopened, f'Native host could not reopen released world/endpoint: {error()}')
    host = reopened
    check(rpc('world.inspect')['revision'] == revision, 'Reopening lost committed CLI edits.')
    library.poima_desktop_destroy(host)
    host = None
    checks.append('Host destruction releases writer ownership and endpoint; frontend HWND survives; committed edits survive reopen.')
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
    print(record.get('error', 'Desktop bridge test failed.'), file=sys.stderr)
sys.exit(0 if record['success'] else 1)
