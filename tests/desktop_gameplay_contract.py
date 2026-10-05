#!/usr/bin/env python3
"""Headless Windows desktop launch configuration and actual CoreCLR Play contract."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in ('bridge', 'hostfxr', 'managed-bridge', 'assembly', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
if os.name != 'nt':
    parser.error('Use native Windows Python; this test loads the Windows desktop bridge, without GPU attachment.')
run = args.output.resolve() / uuid.uuid4().hex
run.mkdir(parents=True)
world = run / 'world.json'
record = dict(passed=False, checks=[], calls=[], hashes={name: hashlib.sha256(path.read_bytes()).hexdigest()
    for name, path in dict(bridge=args.bridge, hostfxr=args.hostfxr, managed_bridge=args.managed_bridge,
                          assembly=args.assembly, test=Path(__file__)).items()},
    limitations=['Headless native desktop ABI and actual CoreCLR module; no GUI, HWND or physical-input qualification.'])
search = os.add_dll_directory(str(args.bridge.resolve().parent))
lib = ctypes.CDLL(str(args.bridge.resolve()))
VP, STR = ctypes.c_void_p, ctypes.c_char_p
for name, params, result in [('create', [STR, STR, ctypes.c_int32, ctypes.c_uint32], VP),
                             ('call', [VP, STR], VP), ('poll', [VP], VP),
                             ('error', [VP], VP), ('destroy', [VP], None)]:
    function = getattr(lib, 'poima_desktop_' + name)
    function.argtypes, function.restype = params, result
host = None
sequence = 0

def decode(pointer):
    return ctypes.string_at(pointer).decode('utf-8') if pointer else ''

def rpc(method, params=None, error=None):
    global sequence
    sequence += 1
    request = dict(jsonrpc='2.0', id=sequence, method=method, params={} if params is None else params)
    pointer = lib.poima_desktop_call(host, json.dumps(request).encode())
    assert pointer, decode(lib.poima_desktop_error(host))
    response = json.loads(decode(pointer))
    record['calls'].append(dict(request=request, response=response))
    assert response.get('id') == sequence, response
    if error is not None:
        assert response.get('error', {}).get('code') == error, response
        return response['error']
    assert 'result' in response, response
    return response['result']

def poll():
    pointer = lib.poima_desktop_poll(host)
    assert pointer, decode(lib.poima_desktop_error(host))
    state = json.loads(decode(pointer))
    record.setdefault('polls', []).append(state)
    gameplay = state['gameplay']
    assert set(gameplay) == {'generation', 'configured', 'runtime'}, gameplay
    if gameplay['runtime'] is not None:
        runtime = gameplay['runtime']
        assert set(runtime) == {'session_id', 'tick', 'revision', 'module'}, runtime
        if runtime['module'] is not None:
            assert set(runtime['module']) == {'identity', 'assembly_sha256'}, runtime
    return state

def inspect():
    return rpc('desktop.gameplay.inspect')

def config(profile, generation=None, error=None):
    params = dict(request_id=uuid.uuid4().hex,
                  expected_generation=inspect()['generation'] if generation is None else generation,
                  profile=profile)
    return params, rpc('desktop.gameplay.configure', params, error)

def start(paused=True, sid=None, generation=None, error=None):
    params = dict(revision=1, session_id=sid or uuid.uuid4().hex, paused=paused)
    if generation is not None:
        params['expected_gameplay_generation'] = generation
    return params, rpc('desktop.play.start', params, error)

def stop(sid):
    return rpc('desktop.play.stop', dict(session_id=sid))

def game(sid):
    return rpc('runtime.gameplay.inspect', dict(session_id=sid, include_schema=True))

def step(sid, ticks, inputs=()):
    state = rpc('desktop.play.inspect')
    return rpc('desktop.play.step', dict(session_id=sid, request_id=uuid.uuid4().hex,
        expected_tick=state['tick'], ticks=ticks, inputs=list(inputs)))

def unchanged():
    assert world.read_bytes() == authored
    assert rpc('world.history') == history

try:
    host = lib.poima_desktop_create(str(world).encode(), b'', -1, 1)
    assert host, decode(lib.poima_desktop_error(None))
    fixture = json.loads((ROOT / 'examples/interaction-room.jsonl').read_text())
    rpc(fixture['method'], fixture['params'])
    authored, history = world.read_bytes(), rpc('world.history')
    described = rpc('desktop.describe')
    assert {'desktop.gameplay.configure', 'desktop.gameplay.inspect'} <= set(described['methods'])
    assert 'expected_gameplay_generation' in described['methods']['desktop.play.start']['properties']
    assert inspect() == dict(generation=0, profile=None, runtime=None)
    old_start, result = start()
    assert result['tick'] == 0 and result['state'] == 'paused'
    assert game(old_start['session_id'])['module'] is None
    stop(old_start['session_id'])
    unchanged()
    record['checks'].append('Existing no-profile Play remains valid and does not load gameplay.')

    # Configuring an existing file that is not an assembly proves the operation
    # validates filesystem metadata, without attempting module initialization.
    placeholder = run / 'uncompiled.dll'
    placeholder.write_bytes(b'Not managed code; configuration must not execute it.')
    profile = dict(hostfxr=str(args.hostfxr.resolve()), bridge=str(args.managed_bridge.resolve()),
                   assembly='uncompiled.dll', type='Poima.Examples.DoorGame')
    configured, result = config(profile)
    placeholder_receipt = result
    assert result['generation'] == 1 and result['replayed'] is False
    assert Path(result['profile']['assembly']) == placeholder
    assert result['profile']['values'] == {} and inspect()['runtime'] is None
    placeholder.unlink()
    retry = dict(configured, profile=dict(profile, values={}))
    assert rpc('desktop.gameplay.configure', retry) == dict(result, replayed=True)
    rpc('desktop.gameplay.configure', dict(retry, profile=None), -32010)
    before = inspect()
    config(profile, error=-32602)
    assert inspect() == before
    record['checks'].append('Configuration executes no module; paths canonicalize, omitted values normalize, receipt replay survives file removal.')

    valid = dict(hostfxr=str(args.hostfxr.resolve()), bridge=str(args.managed_bridge.resolve()),
                 assembly=str(args.assembly.resolve()), type='Poima.Examples.DoorGame',
                 values=dict(OpenX=2.8, UseDistance=4))
    invalid = [dict(valid, typo=True), dict(valid, type=''), dict(valid, type='x' * 513),
               dict(valid, assembly='bad\x00path'), dict(valid, values=[]),
               dict(valid, values={'OpenX': True}), dict(valid, values={'OpenX': [1]}),
               dict(valid, values={'x' * 65: 1}), dict(valid, values={'x': 'x' * 4097}),
               dict(valid, values={str(i): 1 for i in range(129)}),
               dict(valid, values={str(i): 'x' * 4096 for i in range(17)})]
    for candidate in invalid:
        config(candidate, error=-32602)
        assert inspect() == before
    # Relative paths fit the request budget but their canonical absolute forms
    # cross it. All files exist, so rejection specifically tests stored size.
    boundary = dict(hostfxr='f0', bridge='f1', assembly='f2', type='T',
                    values={f'f{i}': '' for i in range(16)})
    for name in ('f0', 'f1', 'f2'):
        (run / name).write_bytes(b'Configuration-only size fixture')
    compact_size = lambda value: len(json.dumps(value, separators=(',', ':'), ensure_ascii=False).encode())
    remaining = 65535 - compact_size(boundary)
    for name in boundary['values']:
        count = min(4096, remaining)
        boundary['values'][name] = 'x' * count
        remaining -= count
    assert remaining == 0 and compact_size(boundary) == 65535
    expanded = dict(boundary, **{name: str((run / boundary[name]).resolve()) for name in ('hostfxr', 'bridge', 'assembly')})
    assert compact_size(expanded) > 65536
    config(boundary, error=-32602)
    assert inspect() == before
    config(valid, generation=0, error=-32009)
    assert inspect() == before
    _, _ = config(dict(valid, type='Missing.Type'))
    generation = inspect()['generation']
    start(error=-32602)  # Configured profile requires explicit generation.
    start(generation=generation-1, error=-32009)
    assert inspect()['runtime'] is None
    record['checks'].append('Strict shape/size/value bounds and stale/missing launch generations reject atomically.')

    failed_start, _ = start(generation=generation, error=-32060)
    assert inspect()['runtime'] is None
    diagnostic = rpc('desktop.play.inspect')['last_error']
    assert diagnostic and rpc('desktop.play.inspect')['state'] == 'stopped'
    for _ in range(3):
        assert poll()['playback']['last_error'] == diagnostic
    rpc('desktop.play.start', failed_start, -32010)
    assert inspect()['generation'] == generation and inspect()['profile']['type'] == 'Missing.Type'
    unchanged()
    # Typed field-name checking necessarily occurs against the loaded schema.
    config(dict(valid, values={'MissingField': 1}))
    start(generation=inspect()['generation'], error=-32060)
    assert inspect()['runtime'] is None
    unchanged()
    record['checks'].append('Failed type/initial-value loads stop the unticked runtime, retain configuration/diagnostics, consume the failed session ID, and preserve authored bytes/history.')

    saved_config, loaded_config = config(valid)
    generation = loaded_config['generation']
    assert rpc('desktop.gameplay.configure', configured) == dict(placeholder_receipt, replayed=True)
    assert inspect()['generation'] == generation and inspect()['profile'] == loaded_config['profile']
    manual_sid = uuid.uuid4().hex
    rpc('runtime.start', dict(session_id=manual_sid, revision=1))
    assert game(manual_sid)['module'] is None
    stop(manual_sid)
    params, result = start(generation=generation)
    sid = params['session_id']
    initial = game(sid)
    assert result['tick'] == 0 and initial['revision'] == 1
    assert initial['module']['backend'] == 'coreclr'
    assert initial['module']['values']['OpenX'] == 2.8 and initial['module']['values']['UseDistance'] == 4
    assert initial['module']['values']['Activations'] == 0
    assert rpc('desktop.play.start', params) == result
    assert game(sid) == initial
    assert rpc('desktop.gameplay.configure', saved_config) == dict(loaded_config, replayed=True)
    config(None, error=-32009)
    assert game(sid) == initial
    assert poll()['gameplay']['runtime']['revision'] == 1
    record['checks'].append('Play loads actual DoorGame and typed overrides before tick zero; active retry neither reloads nor resumes; configuration remains stopped-only.')

    step(sid, 120)
    step(sid, 150, [dict(entity=f'{100:032x}', move=[0, 1])])
    step(sid, 120, [dict(entity=f'{100:032x}', use=True)])
    state = game(sid)
    assert state['module']['values']['Activations'] == 1 and state['module']['values']['Open'] == 1
    door = rpc('runtime.entity', dict(session_id=sid, tick=390, id=f'{2:032x}'))
    assert abs(door['world_matrix'][12] - 2.8) < 1e-5
    rpc('runtime.gameplay.edit', dict(session_id=sid, request_id=uuid.uuid4().hex,
        expected_tick=390, expected_revision=1, values={'OpenX': 3.5}))
    assert poll()['gameplay']['runtime']['revision'] == 2
    before_reload = game(sid)
    reload_params = dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=390,
        expected_revision=2, **{key: valid[key] for key in ('hostfxr', 'bridge', 'assembly', 'type')})
    reloaded = rpc('runtime.gameplay.load', reload_params)
    assert reloaded['module']['values'] == before_reload['module']['values']
    assert reloaded['module']['schema'] == before_reload['module']['schema']
    assert poll()['gameplay']['runtime']['revision'] == 3
    assert rpc('runtime.gameplay.load', reload_params) == dict(reloaded, replayed=True)
    before_bad_reload = game(sid)
    rpc('runtime.gameplay.load', dict(reload_params, request_id=uuid.uuid4().hex,
        expected_revision=3, type='Missing.Type'), -32060)
    assert game(sid) == before_bad_reload
    record['checks'].append('Actual Use/ray/kinematic gameplay runs; paused external edits and state-preserving reloads change compact revision, failed reload preserves the live module.')

    rpc('desktop.play.resume', dict(session_id=sid))
    for method in ('runtime.gameplay.load', 'runtime.gameplay.load_native', 'runtime.gameplay.edit'):
        rpc(method, {}, -32009)
    assert game(sid) == before_bad_reload  # Calls do not pump the clock.
    rpc('desktop.play.pause', dict(session_id=sid))
    stop(sid)
    assert inspect()['profile'] == loaded_config['profile'] and inspect()['runtime'] is None
    config(None)
    empty_start, _ = start(generation=inspect()['generation'])
    assert game(empty_start['session_id'])['module'] is None
    stop(empty_start['session_id'])
    unchanged()
    record['checks'].append('Automatic playback rejects both backend loads/edits; Stop retains configuration, explicit clear restores no-code Play.')

    first, first_result = config(None)
    latest = None
    for _ in range(32):
        latest = config(None)
    assert rpc('desktop.gameplay.configure', latest[0]) == dict(latest[1], replayed=True)
    before = inspect()
    rpc('desktop.gameplay.configure', first, -32009)
    assert inspect() == before
    unchanged()
    record['checks'].append('Receipt retention is bounded to 32: latest retry survives, evicted stale generation cannot reapply.')
    record['passed'] = True
except Exception:
    record['error'] = traceback.format_exc()
    raise
finally:
    if host:
        lib.poima_desktop_destroy(host)
    (run / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(dict(passed=True, checks=len(record['checks']), calls=len(record['calls']), evidence=str(run / 'evidence.json'))))
