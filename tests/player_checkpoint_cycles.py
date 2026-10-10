#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Repeated real checkpoint replacement inside one native Windows player.

Starts one actual Vulkan window, moves its controller before every restore,
and checks the same player survives beyond the former 32-load exit. No builds,
downloads, physical input, compiled fixture, or performance qualification.
Every invocation requires a new output directory and retains failed evidence.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import traceback
import uuid

import player_live_settings_contract as live
import player_service_contract as service

ROOT = Path(__file__).resolve().parents[1]
uid, check = service.uid, service.check
ARGS = RECORD = None


def request_id():
    return uuid.uuid4().hex


def inventory(root):
    return {str(path.relative_to(root)).replace('\\', '/'): {
        'bytes': path.stat().st_size, 'sha256': service.sha(path)}
        for path in sorted(root.rglob('*')) if path.is_file()}


def normalized_entity(value):
    result = copy.deepcopy(value)
    result.pop('session_id', None)
    return result


def entity(client, session, identifier):
    return client.call('runtime.entity', {'session_id': session, 'id': uid(identifier)})


def boundary_projection(value):
    # Presentation can retire another frame between requests. Compare the
    # authoritative owner boundary, rather than incidental frame counters.
    return {key: value[key] for key in ('generation', 'player_id', 'control_revision',
        'state', 'active', 'ready', 'paused', 'session_id', 'current_session_id',
        'tick', 'structure_revision', 'ui_revision', 'control_sequence')}


def ready(client, session, replacements, settings_revision, label):
    def predicate(value):
        check(value['active'], {'reason': 'Native player ended during checkpoint replacement',
                                'cycle': replacements, 'observed': value})
        if not value['ready'] or value['report'] is None:
            return False
        report = value['report']
        application = report.get('live_settings', {}).get('application', {})
        return (value['session_id'] == session and
                report['runtime_replacements'] == replacements and
                application.get('presented_revision') == settings_revision)
    value = service.await_state(client, predicate, label)
    check(value['paused'] and value['report']['hardware'] and
          value['report']['nvrhi_errors'] == 0, value)
    return value


def author(client):
    def make(n, name, position, parent=None, scale=(1, 1, 1), **components):
        result = [{'op': 'entity.create', 'id': uid(n), 'name': name, 'parent': parent},
                  {'op': 'component.set', 'id': uid(n), 'type': 'Transform', 'value': {
                      'position': position, 'rotation': [0, 0, 0, 1], 'scale': list(scale)}}]
        return result + [{'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
                         for kind, value in components.items()]
    operations = make(1, 'Checkpoint floor', [0, -.5, 0], scale=(20, 1, 20),
        MeshRenderer={'primitive': 'box', 'visible': True, 'albedo': [.14, .2, .24]},
        BoxCollider={'half_extents': [.5, .5, .5], 'motion': 'static', 'mass': 10,
                     'friction': .5, 'restitution': 0})
    operations += make(20, 'Projection subject', [0, 1.6, -5],
        MeshRenderer={'primitive': 'box', 'visible': True, 'albedo': [.9, .01, .01]})
    operations += make(100, 'Native controller', [0, 0, 0],
        CharacterController={'radius': .3, 'height': 1.8, 'speed': 4,
                             'jump_speed': 5, 'camera': uid(101)})
    operations += make(101, 'Bound camera', [0, 1.6, 0], parent=uid(100),
        Camera={'vertical_fov': 60, 'near': .1, 'far': 100})

    def dp(value):
        return {'unit': 'dp', 'value': value}

    def ui(n, parent, kind, text='', action=None, **properties):
        return {'op': 'ui.element.set', 'id': uid(n), 'element': {
            'parent': parent, 'name': f'Checkpoint observation {n}', 'kind': kind,
            'text': text, 'action': action, 'visible': True, 'enabled': True, **properties}}

    # Same independently derived rectangle/projection oracle as the live
    # settings fixture. No Control callback is installed or invoked.
    operations += [ui(500, None, 'panel', layout={
        'position': 'absolute', 'left': dp(16), 'top': dp(16), 'width': dp(160),
        'height': dp(88), 'direction': 'column', 'align': 'stretch', 'justify': 'start',
        'padding': [8, 8, 8, 8], 'gap': 8, 'hit_test': 'pass_through'},
        style={'background_color': '#14202a', 'border_width': 0, 'border_radius': 0}),
        ui(501, uid(500), 'label', 'Checkpoint cycles',
           layout={'width': dp(144), 'height': dp(20)},
           style={'font_size': 14, 'color': '#f7e8eb'}),
        ui(502, uid(500), 'button', 'Observation', 'checkpoint-observation',
           layout={'width': dp(144), 'height': dp(36)},
           style={'background_color': '#224466', 'color': '#f7e8eb', 'font_size': 14,
                  'border_width': 0, 'border_radius': 0,
                  'hover': {'background_color': '#224466'},
                  'focus': {'background_color': '#224466'},
                  'pressed': {'background_color': '#224466'}})]
    result = client.call('world.transact', {'base_revision': 0,
        'request_id': request_id(), 'ops': operations})
    RECORD['authoring'] = {'operations': operations, 'result': result}


def run_cycles(client, observer, world):
    check(client.call('player.inspect')['available'], 'Native player is not built')
    author(client)
    authored = world.read_bytes()
    saves = ARGS.output / 'saves'
    saves.mkdir()
    configured = client.call('save.configure', {'request_id': request_id(),
        'expected_generation': 0, 'root': service.native(saves)})
    profile = ARGS.output / 'preferences.poima-settings.json'
    created = client.call('settings.transact', {'path': service.native(profile),
        'request_id': request_id(), 'expected_revision': 0, 'set': {}})
    profile_before = profile.read_bytes()
    session = request_id()
    client.call('runtime.start', {'session_id': session, 'revision': 1})
    parameters = service.start_parameters(session, gpu=ARGS.gpu,
        width=live.WIDTH, height=live.HEIGHT, audio=False,
        settings_profile=service.native(profile), settings_revision=created['revision'])
    acknowledged = client.call('player.start', parameters)
    check(acknowledged['active'] and not acknowledged['ready'], acknowledged)
    initial = ready(observer, session, 0, 0, 'initial-presented')
    identity, generation = initial['player_id'], initial['generation']

    # Let the real capsule settle against the floor before the frozen save.
    client.call('runtime.step', {'session_id': session, 'request_id': request_id(),
        'expected_tick': 0, 'expected_structure_revision': 0, 'ticks': 6})
    saved_boundary = ready(observer, session, 0, 0, 'settled-presented')
    check(saved_boundary['tick'] == 6, saved_boundary)
    saved_controller = entity(client, session, 100)
    saved_camera = entity(client, session, 101)
    written = client.call('save.write', {'request_id': request_id(),
        'configuration_generation': configured['generation'], 'slot': 'checkpoint',
        'expected_generation': 0, 'session_id': session, 'expected_tick': 6,
        'expected_gameplay_revision': 0, 'expected_structure_revision': 0,
        'expected_ui_revision': saved_boundary['ui_revision'],
        'expected_control_sequence': saved_boundary['control_sequence']})
    saved_files = inventory(saves)
    check(saved_files, 'Save produced no durable files')
    payload = live.stored_payload(saves, 'checkpoint')
    RECORD['saved_boundary'] = {'player': saved_boundary, 'controller': saved_controller,
        'camera': saved_camera, 'write': written, 'inventory': saved_files, 'payload': payload}
    RECORD['content_pins'] = {'world_sha256': hashlib.sha256(authored).hexdigest(),
        'profile_sha256': hashlib.sha256(profile_before).hexdigest(), 'save_files': saved_files}

    # Apply preferences AFTER saving: every older gameplay restore must retain
    # this current owner authority, not bring preferences back from the save.
    settings = live.prefs(client)
    values = {'camera.vertical_fov': 75, 'ui.scale': 1.25, 'audio.master_gain': .6}
    accepted, preferences = live.commit(client, live.patch(settings, set=values),
                                        'preferences-presented')
    authority = preferences['player_id']
    settings_revision = preferences['settings']['revision']
    expected_values = copy.deepcopy(preferences['settings']['values'])
    baseline = live.capture(client, 'initial')
    live.image_oracle(baseline, 75, 1.25, 'initial')
    RECORD['preferences'] = {'accepted': accepted, 'current': preferences,
        'authority': authority, 'after_gameplay_save': True}

    RECORD['cycles'] = []
    for cycle in range(1, ARGS.cycles + 1):
        entry = {'cycle': cycle, 'completed': False}
        RECORD['cycles'].append(entry)
        before = client.call('player.inspect')
        check(before['active'] and before['ready'] and before['paused'] and
              before['player_id'] == identity and before['generation'] == generation and
              before['tick'] == 6 and before['session_id'] == session, before)
        stepped = client.call('runtime.step', {'session_id': session,
            'request_id': request_id(), 'expected_tick': 6,
            'expected_structure_revision': before['structure_revision'], 'ticks': 4,
            'inputs': [{'entity': uid(100), 'move': [.4, .6], 'look': [7, -3]}]})
        moved = ready(observer, session, cycle - 1, settings_revision,
                      f'cycle-{cycle}-moved')
        moved_controller = entity(client, session, 100)
        moved_camera = entity(client, session, 101)
        displacement = math.hypot(*(moved_controller['world_matrix'][i] -
                                    saved_controller['world_matrix'][i] for i in (12, 14)))
        check(stepped['tick'] == 10 and moved['tick'] == 10 and displacement > .01 and
              moved_controller['world_matrix'] != saved_controller['world_matrix'] and
              moved_camera['world_matrix'] != saved_camera['world_matrix'],
              'Pre-restore movement/look did not change both native entities')
        entry.update(before=before, moved=moved, moved_controller=moved_controller,
                     moved_camera=moved_camera, horizontal_displacement=displacement)
        if cycle == 1:
            check(live.capture(client, 'moved') != baseline,
                  'Actual native movement/look left presentation unchanged')
        stale_control = service.control_parameters(moved, 'resume')
        fresh = request_id()
        load_parameters = {'request_id': request_id(),
            'configuration_generation': configured['generation'], 'slot': 'checkpoint',
            'expected_generation': written['generation'], 'revision': 1,
            'expected_session_id': session, 'expected_tick': 10,
            'expected_gameplay_revision': 0,
            'expected_structure_revision': moved['structure_revision'],
            'expected_ui_revision': moved['ui_revision'],
            'expected_control_sequence': moved['control_sequence'], 'new_session_id': fresh}
        entry['load_parameters'] = load_parameters
        loaded = client.call('save.load', load_parameters)
        entry['loaded'] = loaded
        restored = ready(observer, fresh, cycle, settings_revision,
                         f'cycle-{cycle}-restored')
        check(restored['player_id'] == identity and restored['generation'] == generation and
              restored['current_session_id'] == fresh and restored['tick'] == 6 and
              restored['control_revision'] > moved['control_revision'] and
              restored['report']['initial_session_id'] == acknowledged['session_id'], restored)
        restored_controller = entity(client, fresh, 100)
        restored_camera = entity(client, fresh, 101)
        check(normalized_entity(restored_controller) == normalized_entity(saved_controller) and
              normalized_entity(restored_camera) == normalized_entity(saved_camera),
              'Checkpoint failed to restore exact controller/camera state')
        entry.update(restored=restored, restored_controller=restored_controller,
                     restored_camera=restored_camera)
        current_preferences = live.prefs(observer)
        check(current_preferences['player_id'] == identity and
              current_preferences['settings']['revision'] == settings_revision and
              current_preferences['settings']['values'] == expected_values,
              'Runtime restore replaced current player preference authority')

        # Historical same-request/same-destination retries return the original
        # receipt before checking now-stale source guards. They cannot replace
        # the runtime a second time or cause another window/renderer lifetime.
        entry['retry'] = client.call('save.load', load_parameters)
        check(entry['retry'] == {**loaded, 'replayed': True},
              'Exact Load retry changed its historical receipt')
        client.reject('save.load', {**load_parameters, 'new_session_id': request_id()}, -32010)
        client.reject('player.control', stale_control, -32009)
        stale_path = ARGS.output / 'stale.bmp'
        client.reject('player.capture', {'player_id': identity, 'request_id': request_id(),
            'expected_control_revision': restored['control_revision'],
            'session_id': session, 'tick': 10,
            'expected_structure_revision': moved['structure_revision'],
            'expected_ui_revision': moved['ui_revision'], 'path': service.native(stale_path)}, -32030)
        preserved = client.call('player.inspect')
        check(boundary_projection(preserved) == boundary_projection(restored) and
              preserved['report']['runtime_replacements'] == cycle and not stale_path.exists(),
              'Retry/stale operation mutated the current player or wrote a capture')
        check(entity(client, fresh, 100) == restored_controller and
              entity(client, fresh, 101) == restored_camera,
              'Retry/stale operation changed controller or camera')
        check(world.read_bytes() == authored and profile.read_bytes() == profile_before and
              inventory(saves) == saved_files, 'Cycle changed authored/profile/save bytes')
        if cycle in {31, 32, ARGS.cycles}:
            label = f'restored-{cycle}'
            image = live.capture(client, label)
            check(image == baseline,
                  f'Same-context checkpoint pixels changed at cycle {cycle}')
            live.image_oracle(image, 75, 1.25, label)
        entry['completed'] = True
        session = fresh

    current = client.call('player.inspect')
    check(current['report']['runtime_replacements'] == ARGS.cycles, current)
    stopped = client.call('player.control', service.control_parameters(current, 'stop'))
    terminal = service.await_state(observer, lambda value: not value['active'], 'requested-stop')
    check(stopped['state'] == 'finished' and terminal['report']['success'] and
          terminal['report']['stop_reason'] == 'requested_stop' and
          terminal['report']['runtime_replacements'] == ARGS.cycles, terminal)
    client.call('runtime.stop', {'session_id': session})
    check(world.read_bytes() == authored and profile.read_bytes() == profile_before and
          inventory(saves) == saved_files, 'Final shutdown changed pinned content')
    RECORD['terminal'] = terminal
    RECORD['checks'] = {name: True for name in (
        'one_live_player_beyond_32_restores', 'real_movement_before_each_load',
        'exact_controller_and_camera_restoration', 'fresh_session_each_load',
        'replacement_remains_paused', 'stale_controls_rejected_without_writes',
        'load_receipt_retry_inert', 'current_preferences_survive_older_gameplay_save',
        'same_context_exact_pixels_at_31_32_and_final', 'capture_does_not_advance_tick',
        'authored_profile_and_checkpoint_bytes_unchanged', 'owned_clean_shutdown')}


def main():
    global ARGS, RECORD
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--cycles', type=int, default=40)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--total-timeout', type=int, default=900)
    ARGS = parser.parse_args()
    check(ARGS.binary.is_file(), 'Supply an existing native player binary')
    check(33 <= ARGS.cycles <= 128, 'Cycles must be bounded 33..128')
    check(0 <= ARGS.gpu <= 4095 and 30 <= ARGS.timeout <= 90 and
          ARGS.timeout <= ARGS.total_timeout <= 1800, 'Invalid GPU/timeout bounds')
    ARGS.output = ARGS.output.resolve()
    ARGS.output.mkdir(parents=True, exist_ok=False)
    files = {'binary': ARGS.binary, 'verifier': Path(__file__),
             'service_helper': Path(service.__file__), 'live_settings_helper': Path(live.__file__),
             'pixel_decoder': ROOT / 'tests' / 'scene_capture.py'}
    RECORD = {'format': 'poima.player-checkpoint-cycles', 'version': 1, 'passed': False,
        'input_sha256': {key: service.sha(path) for key, path in files.items()},
        'requested_cycles': ARGS.cycles, 'gpu_index': ARGS.gpu,
        'rpc_timeout_seconds': ARGS.timeout, 'overall_timeout_seconds': ARGS.total_timeout,
        'owners': [], 'calls': [], 'limitations': [
            'Native graphical service with synthetic semantic movement; no physical input',
            'Primitive fixture; no C# menu or source-free bundle qualification',
            'No timing, throughput, GPU memory-growth or full stability claim',
            'No audio sink or device-audibility qualification',
            'Finite 33..128 replacement coverage is not proof of unlimited loads']}
    service.ARGS, service.RECORD = ARGS, RECORD
    live.ARGS, live.RECORD = ARGS, RECORD
    live.DEADLINE = time.monotonic() + ARGS.total_timeout
    world = ARGS.output / 'world.json'
    endpoint = 'checkpoint-cycles-' + request_id()
    host = client = observer = None
    started = time.monotonic()
    try:
        host = service.ProcessOwner([str(ARGS.binary.resolve()), 'serve', service.native(world),
            '--endpoint', endpoint], 'checkpoint-cycle-host', endpoint, 'serve')
        client = live.Client(endpoint, 'driver-client')
        observer = live.Client(endpoint, 'observer-client')
        service.common_checks(client, observer)
        run_cycles(client, observer, world)
        observer.close()
        observer = None
        check(host.process.poll() is None, 'Observer disconnect killed shared owner')
        client.call('host.shutdown')
        client.close()
        client = None
        host.process.wait(timeout=20)
        host.close()
        host = None
        for owner in RECORD['owners']:
            expected = ('Poima shared world ready: ' + endpoint + '\n'
                        if owner['label'] == 'checkpoint-cycle-host' else '')
            check(owner['exit_code'] == 0 and not owner['stderr_truncated'] and
                  owner['stderr'] == expected, owner)
        for key, path in files.items():
            check(service.sha(path) == RECORD['input_sha256'][key],
                  'Qualification input changed: ' + key)
        RECORD['passed'] = True
    except BaseException as exc:
        RECORD['failure'] = repr(exc)
        RECORD['traceback'] = traceback.format_exc()
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_errors = []
        # Failure evidence stays failed, but a live owned transport can still
        # stop the player/runtime and return a normal host shutdown receipt.
        if (host is not None and host.process.poll() is None and client is not None
                and client.process.poll() is None and client.pending is None):
            previous_timeout, previous_deadline = ARGS.timeout, live.DEADLINE
            ARGS.timeout, live.DEADLINE = 5, time.monotonic() + 30
            try:
                state = client.call('player.inspect')
                if state['active']:
                    client.call('player.control', service.control_parameters(state, 'stop'))
                state = client.call('runtime.status')
                if state['active']:
                    client.call('runtime.stop', {'session_id': state['session_id']})
                client.call('host.shutdown')
                RECORD['failure_cleanup_graceful'] = True
            except BaseException as exc:
                cleanup_errors.append('Graceful shutdown: ' + repr(exc))
            finally:
                ARGS.timeout, live.DEADLINE = previous_timeout, previous_deadline
        for owner in (observer, client, host):
            if owner is not None:
                try:
                    owner.close(expected=False)
                except BaseException as exc:
                    cleanup_errors.append(repr(exc))
        try:
            unchanged = {key: service.sha(path) == RECORD['input_sha256'][key]
                         for key, path in files.items()}
            RECORD['inputs_unchanged_on_exit'] = unchanged
            check(all(unchanged.values()), 'Qualification source/binary changed during run')
            if 'content_pins' in RECORD:
                pins = RECORD['content_pins']
                content = {'world': service.sha(world) == pins['world_sha256'],
                    'profile': service.sha(ARGS.output / 'preferences.poima-settings.json') == pins['profile_sha256'],
                    'save_files': inventory(ARGS.output / 'saves') == pins['save_files']}
                RECORD['content_unchanged_on_exit'] = content
                check(all(content.values()), 'Authored/profile/checkpoint content changed during run')
        except BaseException as exc:
            cleanup_errors.append('Final immutable-input check: ' + repr(exc))
        if cleanup_errors:
            RECORD['passed'] = False
            RECORD['cleanup_errors'] = cleanup_errors
        RECORD['rpc_count'] = len(RECORD['calls'])
        RECORD['elapsed_seconds'] = time.monotonic() - started
        RECORD['completed_cycles'] = sum(value.get('completed', False)
                                         for value in RECORD.get('cycles', []))
        (ARGS.output / 'evidence.json').write_text(json.dumps(RECORD, indent=2) + '\n',
                                                 encoding='utf-8')
        if not RECORD['passed']:
            print('Checkpoint-cycle evidence retained: ' + str(ARGS.output), file=sys.stderr)
        if cleanup_errors and not active_failure:
            raise AssertionError(cleanup_errors)
    print(json.dumps({key: RECORD[key] for key in ('passed', 'rpc_count', 'completed_cycles')}))


if __name__ == '__main__':
    main()
