#!/usr/bin/env python3
"""Headless Windows desktop bridge save guards, replacement and frozen hierarchy."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bridge', type=Path, required=True)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if os.name != 'nt':
    parser.error('Use native Windows Python. This harness does not create windows or initialize graphics.')
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
run = args.output / uuid.uuid4().hex
run.mkdir()
record = {'success': False, 'checks': [], 'calls': [],
          'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
          'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'limitations': ['Headless C ABI and local IPC; no GPU, HWND, physical input or GUI interaction.',
                          'Input events exercise the explicit service protocol; OS capture/cursor release and rendered captures are not qualified.']}
search = os.add_dll_directory(str(args.bridge.resolve().parent))
library = ctypes.CDLL(str(args.bridge.resolve()))
VP, STR = ctypes.c_void_p, ctypes.c_char_p
library.poima_desktop_create.argtypes = [STR, STR, ctypes.c_int32, ctypes.c_uint32]
library.poima_desktop_create.restype = VP
library.poima_desktop_call.argtypes = [VP, STR]
library.poima_desktop_call.restype = VP
library.poima_desktop_poll.argtypes = [VP]
library.poima_desktop_poll.restype = VP
library.poima_desktop_destroy.argtypes = [VP]
library.poima_desktop_destroy.restype = None
library.poima_desktop_error.argtypes = [VP]
library.poima_desktop_error.restype = VP
host = None
request_id = 0
endpoint = 'save-test-' + uuid.uuid4().hex


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def decode(pointer):
    return ctypes.string_at(pointer).decode('utf-8') if pointer else ''


def result(reply, expected_error=None):
    if expected_error is not None:
        check(reply.get('error', {}).get('code') == expected_error, reply)
        return reply['error']
    check('result' in reply, reply)
    return reply['result']


def rpc(method, params=None, expected_error=None):
    global request_id
    request_id += 1
    request = {'jsonrpc': '2.0', 'id': request_id, 'method': method, 'params': params or {}}
    pointer = library.poima_desktop_call(host, json.dumps(request).encode('utf-8'))
    check(pointer, decode(library.poima_desktop_error(host)))
    reply = json.loads(decode(pointer))
    check(reply.get('id') == request_id, reply)
    record['calls'].append({'transport': 'C ABI', 'request': request, 'reply': reply})
    return result(reply, expected_error)


def poll():
    pointer = library.poima_desktop_poll(host)
    check(pointer, decode(library.poima_desktop_error(host)))
    return json.loads(decode(pointer))


def ipc(requests):
    def communicate():
        process = subprocess.run([str(args.binary.resolve()), 'connect', endpoint, '--timeout-ms', '4000'],
                                 input=''.join(json.dumps(r)+'\n' for r in requests), text=True,
                                 encoding='utf-8', capture_output=True, timeout=10)
        check(process.returncode == 0, process.stdout + process.stderr)
        return [json.loads(line) for line in process.stdout.splitlines()]
    with ThreadPoolExecutor(max_workers=1) as executor:
        pending = executor.submit(communicate)
        deadline = time.monotonic() + 12
        while not pending.done() and time.monotonic() < deadline:
            poll()
            time.sleep(.005)
        check(pending.done(), 'IPC client timed out.')
        replies = pending.result()
    check(len(replies) == len(requests), replies)
    for request, reply in zip(requests, replies):
        check(request['id'] == reply.get('id'), reply)
        record['calls'].append({'transport': 'local IPC', 'request': request, 'reply': reply})
    return replies


def transform(position):
    return {'position': position, 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}


def component(entity, kind, value):
    return {'op': 'component.set', 'id': entity, 'type': kind, 'value': value}


def playback():
    return rpc('desktop.play.inspect')


def tree(directory):
    return {str(path.relative_to(directory)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(directory.rglob('*')) if path.is_file()}


try:
    project = run / 'Project'
    made = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Desktop save contract'],
                          capture_output=True, text=True, encoding='utf-8', timeout=30)
    check(made.returncode == 0, made.stdout + made.stderr)
    manifest = json.loads((project / 'project.json').read_text(encoding='utf-8'))
    world = project / manifest['entry']['world']
    controller, camera = manifest['entry']['controller'], manifest['entry']['camera']
    storage = run / 'External saves'
    storage.mkdir()
    host = library.poima_desktop_create(str(world).encode('utf-8'), endpoint.encode(), -1, 1)
    check(host, decode(library.poima_desktop_error(None)))
    revision = rpc('world.inspect')['revision']
    parent, child, other = [uuid.uuid4().hex for _ in range(3)]
    operations = [
        {'op': 'entity.create', 'id': parent, 'name': 'Frozen parent'},
        component(parent, 'Transform', transform([100, 0, 0])),
        {'op': 'entity.create', 'id': child, 'name': 'Frozen descendant', 'parent': parent},
        component(child, 'Transform', transform([10, 0, 0])),
        component(child, 'MeshRenderer', {'primitive': 'box', 'visible': True, 'albedo': [.4, .6, .3]}),
        {'op': 'entity.create', 'id': other, 'name': 'Edited parent'},
        component(other, 'Transform', transform([-100, 0, 0]))]
    revision = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex, 'ops': operations})['revision']
    configured = rpc('save.configure', {'request_id': uuid.uuid4().hex, 'expected_generation': 0, 'root': str(storage)})
    generation = configured['generation']
    check(generation == 1, configured)
    world_methods = rpc('world.describe')['methods']
    check(all(name in world_methods for name in ['save.configure', 'save.write', 'save.load', 'save.inspect', 'save.status']), 'Save discovery is incomplete.')
    sid = uuid.uuid4().hex
    rpc('desktop.play.start', {'session_id': sid, 'revision': revision})
    check(playback()['state'] == 'playing', 'Playback did not start.')
    for method in ['save.configure', 'save.write', 'save.load']:
        rpc(method, {}, expected_error=-32009)
    check(rpc('save.status') == {'generation': 1, 'root': str(storage)}, 'Rejected mutation altered save configuration.')
    check(not rpc('save.inspect', {'slot': 'main'})['exists'], 'Rejected save mutation created a generation.')
    requests = [{'jsonrpc': '2.0', 'id': i+1, 'method': name, 'params': {}}
                for i, name in enumerate(['save.configure', 'save.write', 'save.load', 'save.status'])]
    requests.append({'jsonrpc': '2.0', 'id': 5, 'method': 'save.inspect', 'params': {'slot': 'main'}})
    replies = ipc(requests)
    for reply in replies[:3]:
        result(reply, -32009)
    check(result(replies[3])['generation'] == 1 and not result(replies[4])['exists'], replies)
    check(not list(storage.iterdir()), 'Blocked save mutations created storage files.')
    record['checks'].append('Playing-clock mutation guards apply to C ABI and local IPC; save inspection/status remain available.')

    # Exercise native input ownership without HWND or OS input. Physical cursor
    # capture is deliberately outside this headless service qualification.
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    rpc('desktop.input.configure', {'session_id': sid, 'controller': controller})
    rpc('desktop.input.focus', {'session_id': sid, 'focused': True})
    input_receipt = uuid.uuid4().hex
    rpc('desktop.input.events', {'session_id': sid, 'request_id': input_receipt,
                                'events': [{'control': 'key.w', 'down': True}, {'motion': [3, -2]}]})
    check(rpc('desktop.input.inspect')['accepted_batches'] == 1, 'Protocol input was not recorded.')
    rpc('desktop.play.pause', {'session_id': sid})
    rpc('desktop.view', {'mode': 'scene'})
    saved_tick = playback()['tick']
    frame = rpc('desktop.frame', {'revision': revision, 'id': parent, 'aspect': 1.5})
    write = {'request_id': uuid.uuid4().hex, 'configuration_generation': generation, 'slot': 'main',
             'expected_generation': 0, 'session_id': sid, 'expected_tick': saved_tick, 'expected_gameplay_revision': 0}
    saved = rpc('save.write', write)
    check(saved['generation'] == 1 and saved['bytes'] > 0, saved)
    rpc('desktop.play.step', {'session_id': sid, 'request_id': uuid.uuid4().hex, 'expected_tick': saved_tick, 'ticks': 3})
    revision = rpc('world.transact', {'base_revision': revision, 'request_id': uuid.uuid4().hex,
                                    'ops': [{'op': 'entity.reparent', 'id': child, 'parent': other, 'mode': 'keep_local'}]})['revision']
    authored_bytes, history, storage_before = world.read_bytes(), rpc('world.history'), tree(storage)
    new_sid = uuid.uuid4().hex
    load = {'request_id': uuid.uuid4().hex, 'configuration_generation': generation, 'slot': 'main', 'expected_generation': 1,
            'revision': revision, 'expected_session_id': sid, 'expected_tick': saved_tick+3,
            'expected_gameplay_revision': 0, 'new_session_id': new_sid}
    before = rpc('desktop.inspect')
    rpc('save.load', dict(load, expected_generation=2), expected_error=-32009)
    failed = rpc('desktop.inspect')
    for key in ['runtime', 'playback', 'input', 'gameplay']:
        check(failed[key] == before[key], 'Rejected load altered '+key)
    restored = rpc('save.load', load)
    after = rpc('desktop.inspect')
    check(restored['session_id'] == new_sid and restored['tick'] == saved_tick and restored['source_stale'], restored)
    check(after['playback']['state'] == 'paused' and after['playback']['session_id'] == new_sid and after['playback']['tick'] == saved_tick, after['playback'])
    check(after['playback']['dropped_seconds'] == 0 and after['playback']['last_error'] is None, after['playback'])
    check(not after['input']['configured'] and not after['input']['focused'] and after['input']['accepted_batches'] == 0, after['input'])
    check(after['gameplay']['runtime']['session_id'] == new_sid, 'Gameplay observation cache retained the previous session.')
    restored_frame = rpc('desktop.frame', {'revision': revision, 'id': parent, 'aspect': 1.5})
    check(restored_frame['target'] == frame['target'] and restored_frame['distance'] == frame['distance'], 'Framing used current authored parents instead of the restored frozen hierarchy.')
    check(rpc('entity.get', {'id': child})['value']['parent'] == other, 'Restore modified authored parent.')
    time.sleep(.025)
    check(poll()['runtime']['tick'] == saved_tick, 'Restored paused session advanced on poll.')
    check(world.read_bytes() == authored_bytes and rpc('world.history') == history and tree(storage) == storage_before,
          'Load mutated authoring or save-generation files.')
    record['checks'].append('Paused save/load restores a fresh paused session, resets clock/input and module caches, and frames the saved hierarchy after authored reparenting.')
    record['checks'].append('Failed replacement preserves active runtime, playback, input and module metadata; load leaves authored state/history and slot bytes unchanged.')

    # A successful retry must return the retained result without resetting newly
    # configured input or replacing the session a second time.
    rpc('desktop.input.configure', {'session_id': new_sid, 'controller': controller})
    check(rpc('save.load', load)['replayed'], 'Load retry did not replay its receipt.')
    check(rpc('desktop.input.inspect')['configured'], 'Load retry cleared input configuration.')
    rpc('desktop.view', {'mode': 'game', 'camera': camera})
    rpc('desktop.play.resume', {'session_id': new_sid})
    rpc('desktop.input.focus', {'session_id': new_sid, 'focused': True})
    reused = rpc('desktop.input.events', {'session_id': new_sid, 'request_id': input_receipt,
                                        'events': [{'control': 'key.a', 'down': True}]})
    check(not reused['replayed'] and reused['accepted_batches'] == 1, 'Previous-session input receipt survived replacement.')
    rpc('desktop.input.events', {'session_id': sid, 'request_id': uuid.uuid4().hex, 'events': []}, expected_error=-32009)
    rpc('desktop.play.pause', {'session_id': new_sid})
    record['checks'].append('Exact load retry is inert; replacement discards old input receipts and rejects events for the previous session.')
    record['final_state'] = rpc('desktop.inspect')
    record['success'] = True
except BaseException:
    record['error'] = traceback.format_exc()
finally:
    if host:
        library.poima_desktop_destroy(host)
    search.close()
    evidence = run / 'evidence.json'
    evidence.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
print(json.dumps({'success': record['success'], 'checks': len(record['checks']), 'evidence': str(evidence)}))
if not record['success']:
    print(record.get('error', 'Desktop save contract failed.'), file=sys.stderr)
sys.exit(0 if record['success'] else 1)
