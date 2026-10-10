#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded shared-player owner protocol; optional actual Vulkan frame lifetime.

No downloads, builds or physical-device input. Concurrent clients are queued
without assuming that OS delivery places them in one particular host poll.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
MAX_LINE = 32*1024*1024
ARGS = None
RECORD = None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def uid(value):
    return f'{value:032x}'


def native(path):
    text = str(path.resolve())
    return subprocess.check_output(['wslpath', '-w', text], text=True,
                                   timeout=10).strip() if ARGS.windows_interop else text


def check(value, message):
    if not value:
        raise AssertionError(message)


def diagnostics(client, owner):
    value = client.call('player.diagnostics.inspect', {
        'player_id': owner['player_id'], 'generation': owner['generation']})
    check(value['format'] == 'poima.player-diagnostics.v1' and value['player_id'] == owner['player_id'] and
          value['generation'] == owner['generation'] and value['active'] == owner['active'] and
          value['terminal'] == (not owner['active']), 'Diagnostic retained-owner identity differs')
    check(value['capacity'] == 128 and value['slow_threshold_ns'] == 50_000_000 and
          0 <= value['count'] == len(value['rows']) <= 128, 'Unbounded diagnostic response')
    if not value['counters_saturated']:
        check(value['slow_polls'] == value['count']+value['overwritten'] <= value['polls'] and
              value['invalid_samples'] <= value['slow_polls'], 'Diagnostic retention accounting differs')
        check(all(b['poll_sequence'] > a['poll_sequence'] for a,b in zip(value['rows'],value['rows'][1:])),
              'Diagnostic ring is not chronological')
    for row in value['rows']:
        check(row['valid'] == (row['issues'] == 0) and row['issues'] & ~31 == 0, 'Diagnostic issue flags differ')
        for sample in (row['current'], row['previous']):
            if sample is None:
                continue
            check(all(type(sample[name]) is int and sample[name] >= 0 for name in
                      ('begin_ns','end_ns','tick_before','tick_after','flags','width','height',
                       'resize_ns','runtime_replacements_before','runtime_replacements_after')), 'Malformed CPU observation')
            check(set(sample['cpu_wall_ns']) == {'owner_prepare','events','simulation_owner','scene_prepare','render',
                      'retire','acquire','record','submit','present','gpu_wait','pacing_wait','report'} and
                  set(sample['audio_wall_ns']) == {'queue_wait','enqueue','queue_observe','device_resume','device_pause',
                      'clear','dsp','reset','gain','open','flush','drain_wait'} and
                  all(type(v) is int and v >= 0 for v in sample['cpu_wall_ns'].values()) and
                  all(type(v) is int and v >= 0 for v in sample['audio_wall_ns'].values()), 'Malformed operation wall timings')
            clock = sample['clock']
            for name in ('elapsed_seconds','accepted_seconds','dropped_seconds','accumulator_seconds'):
                measured = clock[name]
                check(measured is None if not clock['observed'] or name in clock['nonfinite_fields'] else
                      type(measured) in (int,float) and math.isfinite(measured), 'Unavailable clock value presented as measured zero')
        if row['inter_poll_gap_ns'] is not None:
            check(row['previous'] is not None and row['inter_poll_gap_ns'] ==
                  row['current']['begin_ns']-row['previous']['end_ns'], 'Host gap lacks its actual previous poll')
    return value


def cleanup_process(process, marker, role):
    if ARGS.windows_interop:
        literal = marker.replace("'", "''")
        script = ("$m='"+literal+"'; Get-CimInstance Win32_Process | Where-Object { "
                  "$_.Name -eq 'poima.exe' -and $_.CommandLine -and "
                  "$_.CommandLine.Contains($m) -and $_.CommandLine.Contains(' "+role+" ') "
                  "} | ForEach-Object { & taskkill.exe /PID $_.ProcessId /T /F | Out-Null }")
        encoded = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                       capture_output=True, timeout=30)
    if process.poll() is None:
        process.kill()
    process.wait(timeout=20)


class ProcessOwner:
    def __init__(self, command, label, marker, role):
        self.marker, self.role = marker, role
        self.record = {'label': label, 'command': command, 'exit_code': None,
                       'stderr': '', 'stderr_truncated': False}
        RECORD['owners'].append(self.record)
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
        self.record['process_id'] = self.process.pid
        self.lines = queue.Queue()

        def stdout():
            try:
                while True:
                    line = self.process.stdout.readline(MAX_LINE+1)
                    if not line:
                        break
                    if len(line.encode('utf-8')) > MAX_LINE or not line.endswith('\n'):
                        self.lines.put(AssertionError('Response line exceeds32MiB or is unterminated'))
                        break
                    self.lines.put(line)
            except BaseException as exc:
                self.lines.put(exc)
            finally:
                self.lines.put(None)

        def stderr():
            while True:
                data = self.process.stderr.read(1024)
                if not data:
                    return
                text = self.record['stderr']+data
                self.record['stderr_truncated'] |= len(text)>16384
                self.record['stderr'] = text[-16384:]

        self.threads = [threading.Thread(target=stdout, daemon=True),
                        threading.Thread(target=stderr, daemon=True)]
        for thread in self.threads:
            thread.start()

    def close(self, expected=True):
        failed = False
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                try:
                    self.process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    failed = True
                    cleanup_process(self.process, self.marker, self.role)
            self.record['exit_code'] = self.process.returncode
            if expected:
                check(not failed and self.process.returncode == 0, self.record)
        finally:
            if self.process.poll() is None:
                cleanup_process(self.process, self.marker, self.role)
            self.record['exit_code'] = self.process.returncode
            for thread in self.threads:
                thread.join(timeout=3)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                if not stream.closed:
                    stream.close()


class Client(ProcessOwner):
    def __init__(self, endpoint, label):
        super().__init__([str(ARGS.binary.resolve()), 'connect', endpoint,
                         '--timeout-ms', str(ARGS.timeout*1000)], label, endpoint, 'connect')
        self.next_id = 0
        self.pending = None

    def send(self, method, params=None):
        check(self.pending is None, 'One outstanding request per client')
        self.next_id += 1
        request = {'jsonrpc': '2.0', 'id': self.next_id, 'method': method,
                   'params': {} if params is None else params}
        entry = {'owner': self.record['label'], 'request': request}
        RECORD['calls'].append(entry)
        self.pending = entry
        self.process.stdin.write(json.dumps(request, allow_nan=False)+'\n')
        self.process.stdin.flush()

    def receive(self):
        check(self.pending is not None, 'No pending client request')
        line = self.lines.get(timeout=ARGS.timeout)
        check(line is not None, self.record)
        if isinstance(line, BaseException):
            raise line
        reply = json.loads(line)
        request = self.pending['request']
        check(reply.get('jsonrpc') == '2.0' and type(reply.get('id')) is int and
              reply['id'] == request['id'], reply)
        check(('result' in reply) != ('error' in reply), reply)
        self.pending['response'] = reply
        self.pending = None
        return reply

    def reply(self, method, params=None):
        self.send(method, params)
        return self.receive()

    def call(self, method, params=None):
        reply = self.reply(method, params)
        check('result' in reply, reply)
        return reply['result']

    def reject(self, method, params, code):
        reply = self.reply(method, params)
        check(reply.get('error', {}).get('code') == code, reply)
        return reply


def frame_signature(path):
    raw = path.read_bytes()
    check(len(raw)>=54 and raw[:2] == b'BM', 'Missing actual BMP capture')
    width, height = struct.unpack_from('<ii', raw, 18)
    check(width == 640 and abs(height) == 480, (width, height))
    return {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw),
            'width': width, 'height': abs(height)}


def await_state(client, predicate, label):
    deadline = time.monotonic()+ARGS.timeout
    observations = []
    while time.monotonic()<deadline:
        observed = client.call('player.inspect')
        observations.append(observed)
        if predicate(observed):
            RECORD.setdefault('waits', {})[label] = observations
            return observed
        time.sleep(.01)
    raise AssertionError((label, observations[-3:]))


def common_checks(client, second):
    first = client.call('world.describe')
    check(first['session_scope'] == 'shared_headless', first)
    for name in ('player.start', 'player.inspect', 'player.control', 'player.capture'):
        check(name in first['methods'], name)
    # Queue both real clients before reading either response. Their arrival and
    # host poll grouping are deliberately unspecified.
    client.send('world.inspect'); second.send('player.inspect')
    check('result' in client.receive(), 'First concurrent client was stranded')
    check('result' in second.receive(), 'Second concurrent client was stranded')
    return first


def start_parameters(session, generation=0, **extra):
    return {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0,
            'expected_generation': generation, 'camera': uid(101), 'controller': uid(100),
            'mode': 'interactive', 'initially_paused': True,
            'gamepad': {'mode': 'disabled'}, 'width': 640, 'height': 480,
            'samples': 1, 'frames_in_flight': 1, **extra}


def control_parameters(observed, action):
    return {'player_id': observed['player_id'], 'request_id': uuid.uuid4().hex,
            'expected_control_revision': observed['control_revision'], 'action': action}


def standalone_scope_checks():
    world = ARGS.output/'standalone.json'
    owner = ProcessOwner([str(ARGS.binary.resolve()), 'world', native(world)],
                         'standalone-scope-owner', native(world), 'world')
    calls = [('player.start', start_parameters(uuid.uuid4().hex)),
             ('player.control', {'player_id': uid(5), 'request_id': uuid.uuid4().hex,
                                 'expected_control_revision': 0, 'action': 'stop'}),
             ('player.capture', {'player_id': uid(5), 'request_id': uuid.uuid4().hex,
                                  'expected_control_revision': 0, 'session_id': uid(6),
                                  'tick': 0, 'path': native(ARGS.output/'unscoped.bmp')})]
    try:
        for index, (method, params) in enumerate(calls):
            request = {'jsonrpc': '2.0', 'id': index+1, 'method': method, 'params': params}
            owner.process.stdin.write(json.dumps(request)+'\n'); owner.process.stdin.flush()
            line = owner.lines.get(timeout=ARGS.timeout)
            check(isinstance(line, str), owner.record)
            reply = json.loads(line)
            RECORD['calls'].append({'owner': owner.record['label'], 'request': request, 'response': reply})
            check(reply.get('error', {}).get('code') == -32080, reply)
        owner.close()
    finally:
        if owner.process.poll() is None:
            owner.close(expected=False)
    check(not world.exists() and not (ARGS.output/'unscoped.bmp').exists(),
          'Wrong-scope player operation wrote an artifact')


def unavailable_checks(client, discovery, world):
    standalone_scope_checks()
    initial = client.call('player.inspect')
    check(initial['generation'] == 0 and initial['player_id'] is None and
          initial['control_revision'] == 0 and initial['state'] == 'absent' and
          not initial['active'] and not initial['ready'], initial)
    client.reject('player.inspect', {'unknown': True}, -32602)
    client.reject('player.diagnostics.inspect', {'player_id': uid(999), 'generation': 0}, -32004)
    client.reject('player.diagnostics.inspect', {'player_id': uid(999), 'generation': 0.0}, -32602)
    check(client.call('player.inspect') == initial, 'Absent diagnostics read changed owner')
    if not initial['available']:
        params = start_parameters(uuid.uuid4().hex)
        client.reject('player.start', params, -32003)
        client.reject('player.start', params, -32003)
        check(client.call('player.inspect') == initial, 'Unavailable start changed generation/state')
        check(not world.exists(), 'Unavailable start wrote authored state')
        RECORD['driver'] = {'available': False, 'unavailable_start_atomic': True}
    else:
        RECORD['driver'] = {'available': True, 'graphics_skipped': True,
                            'reason': '--capture not requested'}
    RECORD['checks'] = {'shared_discovery': True, 'two_queued_clients_answered': True,
                        'wrong_scope_rejected': True, 'no_graphics_initialized': True}


def graphics_checks(client, second, world):
    standalone_scope_checks()
    check(client.call('player.inspect')['available'], 'This build lacks frame-driven native player')
    def entity(n, position, parent=None, **components):
        out = [{'op': 'entity.create', 'id': uid(n), 'name': str(n), 'parent': parent},
               {'op': 'component.set', 'id': uid(n), 'type': 'Transform', 'value': {
                   'position': position, 'rotation': [0,0,0,1], 'scale': [1,1,1]}}]
        out += [{'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
                for kind,value in components.items()]
        return out
    ops = entity(1, [0,-.5,0], MeshRenderer={'primitive': 'box', 'visible': True,
                    'albedo': [.14,.2,.24]}, BoxCollider={'half_extents': [10,.5,10],
                    'motion': 'static', 'mass': 10, 'friction': .5, 'restitution': 0})
    ops += entity(20, [0,1.6,-5], MeshRenderer={'primitive': 'box', 'visible': True,
                                            'albedo': [.9,.01,.01]})
    ops += entity(100, [0,0,0], CharacterController={'radius': .3, 'height': 1.8,
                    'speed': 4, 'jump_speed': 5, 'camera': uid(101)})
    ops += entity(101, [0,1.6,0], parent=uid(100),
                  Camera={'vertical_fov': 60, 'near': .1, 'far': 100})
    client.call('world.transact', {'base_revision': 0, 'request_id': uuid.uuid4().hex, 'ops': ops})
    world_before = world.read_bytes()
    saves = ARGS.output/'saves'; saves.mkdir()
    configured = client.call('save.configure', {'request_id': uuid.uuid4().hex,
                            'expected_generation': 0, 'root': native(saves)})
    session = uuid.uuid4().hex
    client.call('runtime.start', {'session_id': session, 'revision': 1})
    params = start_parameters(session, gpu=ARGS.gpu,
                              settings_overrides={'camera.vertical_fov': 90})
    first = client.call('player.start', params)
    check(first['generation'] == 1 and first['active'] and not first['ready'] and
          first['state'] == 'initializing' and first['tick'] == 0, first)
    check(client.call('player.start', params) == {**first, 'replayed': True},
          'Start receipt was not replayed before current-generation guard')
    # A second client can inspect while initialization proceeds, without owning
    # another window or an independent Runtime.
    observed = await_state(second, lambda value: value['ready'], 'interactive-ready')
    check(observed['player_id'] == first['player_id'] and observed['paused'] and
          observed['session_id'] == session and observed['tick'] == 0, observed)
    check(observed['report']['hardware'] and observed['report']['nvrhi_errors'] == 0, observed)
    identity = observed['player_id']
    before_read = client.call('runtime.inspect', {'session_id': session})
    first_diagnostics = diagnostics(client, observed)
    check(first_diagnostics['polls'] > 0, 'Ready native player recorded no diagnostic polls')
    second_diagnostics = diagnostics(second, observed)
    check(second_diagnostics['polls'] >= first_diagnostics['polls'] and
          client.call('runtime.inspect', {'session_id': session}) == before_read,
          'Paused diagnostic observation changed runtime state')
    client.reject('player.diagnostics.inspect', {'player_id': identity, 'generation': observed['generation']+1}, -32009)
    client.reject('player.diagnostics.inspect', {'player_id': uid(999), 'generation': observed['generation']}, -32004)
    client.reject('player.diagnostics.inspect', {'player_id': identity, 'generation': observed['generation'], 'unknown': True}, -32602)
    after_read = client.call('player.inspect')
    check(all(after_read[key] == observed[key] for key in
              ('player_id','generation','control_revision','tick','structure_revision','ui_revision','control_sequence')) and
          'diagnostics' not in after_read and 'diagnostics' not in after_read['report'],
          'Read-only diagnostic request changed a guard or duplicated its ring in regular inspection')

    old_pause = control_parameters(observed, 'pause')
    paused = client.call('player.control', old_pause)
    check(paused['paused'] and paused['control_revision'] == observed['control_revision']+1, paused)
    frozen_tick = paused['tick']
    for _ in range(3):
        check(second.call('player.inspect')['tick'] == frozen_tick, 'Paused owner advanced simulation')
    capture_params = {'player_id': identity, 'request_id': uuid.uuid4().hex,
                      'expected_control_revision': paused['control_revision'],
                      'session_id': session, 'tick': frozen_tick,
                      'path': native(ARGS.output/'initial.bmp')}
    before_entity = client.call('runtime.entity', {'session_id': session, 'id': uid(100)})
    initial_capture = client.call('player.capture', capture_params)
    image = frame_signature(ARGS.output/'initial.bmp')
    captured = initial_capture['capture']
    check(captured['capture_written'] and captured['hardware'] and captured['nvrhi_errors'] == 0,
          initial_capture)
    check(captured['lens']['vertical_fov'] == 90 and captured['samples'] == 1, captured)
    camera = client.call('runtime.entity', {'session_id': session, 'id': uid(101)})
    check(captured['camera_world'] == camera['world_matrix'], 'Captured a stale or foreign camera')
    check(client.call('runtime.entity', {'session_id': session, 'id': uid(100)}) == before_entity,
          'Same-context capture advanced native controller')
    check(client.call('player.capture', capture_params) == {**initial_capture, 'replayed': True},
          'Capture retry recreated the exclusive image')
    check(frame_signature(ARGS.output/'initial.bmp') == image, 'Capture retry changed original artifact')
    repeated_path = {**capture_params, 'request_id': uuid.uuid4().hex}
    client.reject('player.capture', repeated_path, -32602)
    check(frame_signature(ARGS.output/'initial.bmp') == image, 'New request overwrote exclusive image')

    def observe_capture(name, boundary):
        path = ARGS.output/(name+'.bmp')
        parameters = {'player_id': boundary['player_id'], 'request_id': uuid.uuid4().hex,
                      'expected_control_revision': boundary['control_revision'],
                      'session_id': boundary['session_id'], 'tick': boundary['tick'],
                      'path': native(path)}
        response = client.call('player.capture', parameters)
        frame = response['capture']
        check(frame['capture_written'] and frame['hardware'] and frame['nvrhi_errors'] == 0, frame)
        current_camera = client.call('runtime.entity', {'session_id': boundary['session_id'], 'id': uid(101)})
        check(frame['camera_world'] == current_camera['world_matrix'], 'Observation used stale camera at '+name)
        check(client.call('runtime.inspect', {'session_id': boundary['session_id']})['tick'] == boundary['tick'],
              'Observation advanced tick at '+name)
        signature = frame_signature(path)
        RECORD.setdefault('iterative_captures', {})[name] = {'result': response, 'image': signature}
        return pixels(path)

    # Manual advancement belongs to the original native stepping API and is
    # admitted only under a paused interactive presentation.
    stepped = client.call('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                          'expected_tick': frozen_tick, 'ticks': 3,
                          'inputs': [{'entity': uid(100), 'move': [.2,.6], 'look': [12,-5]}]})
    check(stepped['tick'] == frozen_tick+3, stepped)
    after_step = await_state(client, lambda value: value['ready'], 'stepped-ready')
    tick = after_step['tick']; check(tick == 3 and after_step['paused'], after_step)
    saved_entity = client.call('runtime.entity', {'session_id': session, 'id': uid(100)})
    check(saved_entity != before_entity, 'Accepted manual step did not affect native actor')
    stepped_pixels = observe_capture('stepped', after_step)
    check(stepped_pixels != pixels(ARGS.output/'initial.bmp'), 'Accepted movement/look had no rendered effect')
    stale = {**capture_params, 'request_id': uuid.uuid4().hex,
             'expected_control_revision': after_step['control_revision'],
             'path': native(ARGS.output/'stale.bmp')}
    client.reject('player.capture', stale, -32009)
    client.reject('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                  'expected_tick': 0, 'ticks': 1}, -32009)
    check(not (ARGS.output/'stale.bmp').exists(), 'Stale tick created image')
    check(client.call('runtime.entity', {'session_id': session, 'id': uid(100)}) == saved_entity,
          'Stale request changed controller')

    resumed = client.call('player.control', control_parameters(after_step, 'resume'))
    before_retry = second.call('player.inspect')
    retry = client.call('player.control', old_pause)
    check(retry == {**paused, 'replayed': True}, 'Historical pause retry result changed')
    current = second.call('player.inspect')
    for key in ('player_id', 'control_revision', 'session_id', 'paused'):
        check(current[key] == before_retry[key], 'Historical pause changed '+key)
    RECORD['interactive_resume'] = {'ack': resumed, 'before_retry': before_retry,
                                   'after_retry': current,
                                   'effective_running': not current['paused']}
    if not current['paused']:
        client.reject('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                      'expected_tick': current['tick'], 'ticks': 1}, -32080)
    else:
        RECORD['interactive_resume']['running_write_rejection_skipped'] = \
            'Player remains inactive under real window focus; replay supplies focus-independent running proof'
    repaused = client.call('player.control', control_parameters(current, 'pause'))
    # Save a committed paused boundary, then restore it while the same native
    # window stays attached. No fabricated UI, pose or controller patch is used.
    saved_tick = repaused['tick']
    saved_entity = client.call('runtime.entity', {'session_id': session, 'id': uid(100)})
    written = client.call('save.write', {'request_id': uuid.uuid4().hex,
             'configuration_generation': configured['generation'], 'slot': 'paused',
             'expected_generation': 0, 'session_id': session, 'expected_tick': saved_tick,
             'expected_gameplay_revision': 0})
    saved_boundary = await_state(client, lambda value: value['ready'], 'saved-ready')
    saved_pixels = observe_capture('saved', saved_boundary)
    client.call('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                'expected_tick': saved_tick, 'ticks': 2,
                'inputs': [{'entity': uid(100), 'move': [-.5,.3]}]})
    changed = client.call('runtime.entity', {'session_id': session, 'id': uid(100)})
    check(changed != saved_entity, 'Pre-restore advancement had no observable native effect')
    before_restore = client.call('player.inspect')
    fresh = uuid.uuid4().hex
    client.call('save.load', {'request_id': uuid.uuid4().hex,
                'configuration_generation': configured['generation'], 'slot': 'paused',
                'expected_generation': written['generation'], 'revision': 1,
                'expected_session_id': session, 'expected_tick': saved_tick+2,
                'expected_gameplay_revision': 0, 'new_session_id': fresh})
    restored = await_state(second, lambda value: value['ready'], 'restored-ready')
    check(restored['player_id'] == identity and restored['generation'] == 1 and
          restored['session_id'] == fresh and restored['current_session_id'] == fresh and
          restored['tick'] == saved_tick and restored['paused'] and
          restored['control_revision'] > before_restore['control_revision'], restored)
    actual = client.call('runtime.entity', {'session_id': fresh, 'id': uid(100)})
    a,b = copy.deepcopy(actual),copy.deepcopy(saved_entity); a.pop('session_id'); b.pop('session_id')
    check(a == b, 'Save replacement did not restore exact native controller state')
    restored_pixels = observe_capture('restored', restored)
    check(restored_pixels == saved_pixels, 'Restored live context pixels differ from saved boundary')
    client.reject('player.control', control_parameters(before_restore, 'resume'), -32009)
    client.reject('player.capture', {**capture_params, 'request_id': uuid.uuid4().hex,
                  'expected_control_revision': restored['control_revision'],
                  'path': native(ARGS.output/'old-session.bmp')}, -32030)
    check(not (ARGS.output/'old-session.bmp').exists(), 'Old-session capture wrote an artifact')
    for method, parameters in [
        ('runtime.stop', {'session_id': fresh}),
        ('runtime.start', {'session_id': uuid.uuid4().hex, 'revision': 1}),
        ('runtime.capture', {'session_id': fresh, 'tick': saved_tick, 'camera': uid(101),
                            'path': native(ARGS.output/'nested.bmp')}),
        ('host.shutdown', {})]:
        client.reject(method, parameters, -32080)
    check(not (ARGS.output/'nested.bmp').exists(), 'Second graphics lifetime was created')

    # The bounded receipt cache may evict old answers, but cannot resurrect an
    # old command when its original guards are no longer current.
    current = restored
    for _ in range(34):
        current = client.call('player.control', control_parameters(current, 'pause'))
    before_evicted = client.call('player.inspect')
    client.reject('player.control', old_pause, -32009)
    client.reject('player.start', params, -32009)
    after_evicted = client.call('player.inspect')
    for key in ('generation', 'player_id', 'control_revision', 'session_id', 'tick', 'paused'):
        check(after_evicted[key] == before_evicted[key], 'Expired receipt changed '+key)
    stopped = client.call('player.control', control_parameters(after_evicted, 'stop'))
    terminal = await_state(client, lambda value: not value['active'], 'interactive-stopped')
    check(terminal['state'] == 'finished' and terminal['report']['stop_reason'] == 'requested_stop', terminal)
    terminal_diagnostics = diagnostics(client, terminal)
    check(terminal_diagnostics == diagnostics(second, terminal), 'Terminal diagnostic snapshot changed after window destruction')

    # Paused replay permits owner pause/resume/cancel, while every external
    # simulation/save mutation remains forbidden.
    replay_ticks = 48
    replay = start_parameters(fresh, generation=terminal['generation'], expected_tick=saved_tick,
                  mode='replay', gpu=ARGS.gpu, sequence=[{'ticks': replay_ticks, 'move': [.3,.4], 'look': [4,-2]}])
    replay_ack = client.call('player.start', replay)
    replay_ready = await_state(client, lambda value: value['ready'], 'replay-ready')
    client.reject('player.diagnostics.inspect', {'player_id': identity, 'generation': terminal['generation']}, -32004)
    client.reject('player.diagnostics.inspect', {'player_id': replay_ready['player_id'], 'generation': terminal['generation']}, -32009)
    check(replay_ready['mode'] == 'replay' and replay_ready['paused'] and
          replay_ready['tick'] == saved_tick, replay_ready)
    for method, parameters in [
        ('runtime.step', {'session_id': fresh, 'request_id': uuid.uuid4().hex,
                          'expected_tick': saved_tick, 'ticks': 1}),
        ('save.write', {'request_id': uuid.uuid4().hex,
                        'configuration_generation': configured['generation'], 'slot': 'replay',
                        'expected_generation': 0, 'session_id': fresh, 'expected_tick': saved_tick,
                        'expected_gameplay_revision': 0}),
        ('save.configure', {'request_id': uuid.uuid4().hex,
                            'expected_generation': configured['generation'], 'root': native(saves)})]:
        client.reject(method, parameters, -32080)
    canceled = client.call('player.control', control_parameters(replay_ready, 'stop'))
    canceled = await_state(client, lambda value: not value['active'], 'replay-canceled')
    check(canceled['tick'] == saved_tick and canceled['report']['stop_reason'] == 'requested_stop', canceled)
    replay['request_id'] = uuid.uuid4().hex; replay['expected_generation'] = canceled['generation']
    client.call('player.start', replay)
    ready = await_state(client, lambda value: value['ready'], 'second-replay-ready')
    resumed = client.call('player.control', control_parameters(ready, 'resume'))
    pause_request = control_parameters(resumed, 'pause')
    paused_again = client.call('player.control', pause_request)
    pause_tick = paused_again['tick']
    check(saved_tick <= pause_tick < saved_tick+replay_ticks, 'Replay pause lost its recorded offset')
    for _ in range(3):
        check(second.call('player.inspect')['tick'] == pause_tick, 'Paused replay advanced')
    resumed = client.call('player.control', control_parameters(paused_again, 'resume'))
    before_replay_retry = second.call('player.inspect')
    check(not before_replay_retry['paused'] and before_replay_retry['active'], 'Replay did not resume')
    check(client.call('player.control', pause_request) == {**paused_again, 'replayed': True},
          'Replay pause receipt changed')
    after_replay_retry = second.call('player.inspect')
    check(after_replay_retry['control_revision'] == before_replay_retry['control_revision'] and
          not after_replay_retry['paused'], 'Replay pause receipt re-applied pause after resume')
    client.reject('runtime.step', {'session_id': fresh, 'request_id': uuid.uuid4().hex,
                  'expected_tick': after_replay_retry['tick'], 'ticks': 1}, -32080)
    finished = await_state(client, lambda value: not value['active'], 'replay-complete')
    check(finished['tick'] == saved_tick+replay_ticks and finished['report']['stop_reason'] == 'replay_complete'
          and finished['report']['success'], finished)
    client.reject('player.control', control_parameters(resumed, 'pause'), -32009)
    client.call('runtime.stop', {'session_id': fresh})
    finished_diagnostics = diagnostics(client, finished)
    check(finished_diagnostics == diagnostics(second, finished), 'Finished diagnostics depended on destroyed Runtime/window')
    check(world.read_bytes() == world_before, 'Live player changed authored world bytes')
    RECORD['graphics'] = {'passed': True, 'gpu': ARGS.gpu, 'hardware_name': captured['gpu'],
                          'initial_capture': initial_capture, 'image': image,
                          'saved_entity': saved_entity, 'restored_entity': actual,
                          'replay_ack': replay_ack, 'finished': finished}
    RECORD['checks'] = {'shared_discovery': True, 'two_queued_clients_answered': True,
                        'deferred_start_ack': True, 'paused_tick_stable': True,
                        'same_context_capture_no_tick': True, 'capture_retry_preserves_bytes': True,
                        'manual_step_changes_rendered_pixels': True, 'save_restore_exact_context_pixels': True,
                        'paused_manual_step': True, 'stale_requests_atomic': True,
                        'historical_pause_not_reapplied': True, 'exact_save_replacement': True,
                        'replacement_pauses_and_invalidates_controls': True,
                        'one_graphics_lifetime': True, 'expired_receipts_do_not_reapply': True,
                        'replay_external_mutations_rejected': True,
                        'zero_tick_replay_cancel': True, 'replay_pause_preserves_offset': True,
                        'diagnostic_reads_preserve_runtime_and_guards': True,
                        'diagnostic_identity_generation_guards': True, 'terminal_diagnostics_immutable': True,
                        'diagnostic_ring_bounded_chronological': True,
                        'authored_world_unchanged': True}


def main():
    global ARGS, RECORD
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true', help='Actual hardware player integration')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=180)
    ARGS = parser.parse_args()
    check(ARGS.binary.is_file() and 30<=ARGS.timeout<=600, 'Supply binary and timeout30..600')
    if ARGS.capture and ARGS.output is None:
        parser.error('--capture requires explicit new --output')
    temporary = ARGS.output is None
    if temporary:
        scratch = ROOT/'build'/'player-service-contract'; scratch.mkdir(parents=True, exist_ok=True)
        ARGS.output = Path(tempfile.mkdtemp(prefix='protocol-', dir=scratch))
    else:
        ARGS.output = ARGS.output.resolve(); ARGS.output.mkdir(parents=True, exist_ok=False)
    world = ARGS.output/'world.json'; endpoint = 'player-'+uuid.uuid4().hex
    RECORD = {'passed': False, 'binary_sha256': sha(ARGS.binary),
              'verifier_sha256': sha(Path(__file__)), 'owners': [], 'calls': [],
              'limitations': ['No physical input or timing/performance qualification',
                              'Concurrent client delivery does not imply one host poll batch']}
    host = client = second = None
    try:
        host = ProcessOwner([str(ARGS.binary.resolve()), 'serve', native(world),
                            '--endpoint', endpoint], 'shared-player-host', endpoint, 'serve')
        client = Client(endpoint, 'primary-client')
        second = Client(endpoint, 'observer-client')
        discovery = common_checks(client, second)
        if ARGS.capture:
            graphics_checks(client, second, world)
        else:
            unavailable_checks(client, discovery, world)
        second.close(); second = None
        check(host.process.poll() is None, 'Detaching observer killed shared owner')
        check(client.call('world.inspect')['revision'] >= 0, 'Host did not survive detach')
        client.call('host.shutdown')
        client.close(); client = None
        host.process.wait(timeout=20); host.close(); host = None
        check(sha(ARGS.binary) == RECORD['binary_sha256'], 'Binary changed during qualification')
        check(sha(Path(__file__)) == RECORD['verifier_sha256'], 'Verifier changed during qualification')
        RECORD['rpc_count'] = len(RECORD['calls']); RECORD['passed'] = True
    except BaseException as exc:
        RECORD['failure'] = repr(exc); RECORD['traceback'] = traceback.format_exc()
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_errors = []
        for owner in (second, client, host):
            if owner is not None:
                try:
                    owner.close(expected=False)
                except BaseException as exc:
                    cleanup_errors.append(repr(exc))
        if cleanup_errors:
            RECORD['passed'] = False; RECORD['cleanup_errors'] = cleanup_errors
        (ARGS.output/'evidence.json').write_text(json.dumps(RECORD, indent=2)+'\n', encoding='utf-8')
        if not RECORD['passed']:
            print(f'Player service evidence retained: {ARGS.output}', file=sys.stderr)
        if cleanup_errors and not active_failure:
            raise AssertionError(cleanup_errors)
    if temporary:
        shutil.rmtree(ARGS.output)
    print(json.dumps({key: RECORD[key] for key in ('passed', 'rpc_count')}))


if __name__ == '__main__':
    main()
