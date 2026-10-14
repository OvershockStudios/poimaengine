#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Observe genuinely advancing native replay UI without requiring window focus.

Windows-native Python, an existing simulation/Vulkan/game-UI engine, and the
retained ABI7/min176 managed-UI Native AOT fixture are required. No builds,
downloads, artifact changes, physical input, focus synthesis or external Tick
steps. Replay consumes its declared 600 neutral inputs through the native host.

Run inside the normal owned native qualification gate:
python tests/player_ui_replay_observation.py --binary POIMA_EXE \
  --native-descriptor MANAGED_UI_NATIVE_GAMEPLAY_JSON --output NEW_DIRECTORY --gpu 1

This qualifies unattended replay observation, not focused interactive input,
image quality, frame time, or the separate compiled control/save/load workflow.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import traceback
import uuid

import player_service_contract as shared
from player_ui_observation_contract import verify_live_reads, verify_readiness

ROOT = Path(__file__).resolve().parents[1]
check, sha, uid = shared.check, shared.sha, shared.uid
REPLAY_TICKS = 600


def entity(number, position, parent=None, **components):
    ops = [{'op': 'entity.create', 'id': uid(number), 'name': 'Replay '+str(number), 'parent': parent},
           {'op': 'component.set', 'id': uid(number), 'type': 'Transform', 'value': {
               'position': position, 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}}]
    ops += [{'op': 'component.set', 'id': uid(number), 'type': kind, 'value': value}
            for kind, value in components.items()]
    return ops


def qualify(client, observer, args, record, world):
    discovery = shared.common_checks(client, observer)
    check('player.ui.inspect' in discovery['methods'], 'Discover the current live UI observation method')
    check(client.call('player.inspect')['available'], 'Native player is unavailable')
    ops = entity(10, [0, -.5, 0], MeshRenderer={
        'primitive': 'box', 'visible': True, 'albedo': [.14, .2, .24]}, BoxCollider={
        'half_extents': [10, .5, 10], 'motion': 'static', 'mass': 10, 'friction': .5, 'restitution': 0})
    ops += entity(100, [0, 0, 0], CharacterController={
        'radius': .3, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': uid(101)})
    ops += entity(101, [0, 1.6, 0], parent=uid(100), Camera={
        'vertical_fov': 60, 'near': .1, 'far': 100})
    # The unchanged compiled fixture uses label2 and editbutton3. No callback
    # is invoked here: Mode1 writes the label from its ordinary native Tick.
    for number, kind, text, action in [(1, 'panel', '', None),
                                     (2, 'label', 'Original', None),
                                     (3, 'button', 'Edit', 'edit')]:
        ops.append({'op': 'ui.element.set', 'id': uid(number), 'element': {
            'parent': None if number == 1 else uid(1), 'name': 'Replay control '+str(number),
            'kind': kind, 'text': text, 'action': action, 'visible': True, 'enabled': True}})
    client.call('world.transact', {'base_revision': 0, 'request_id': uuid.uuid4().hex, 'ops': ops})
    authored_hash = sha(world)
    record['authored_world'] = {'sha256': authored_hash, 'bytes': world.stat().st_size}
    session = uuid.uuid4().hex
    client.call('runtime.start', {'session_id': session, 'revision': 1})
    loaded = client.call('runtime.gameplay.load_native', {'session_id': session,
        'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'expected_revision': 0,
        'descriptor': shared.native(args.native_descriptor),
        'expected_descriptor_sha256': record['inputs']['descriptor']['sha256']})
    check(loaded['module']['schema']['identity'] == 'poima.test.managed-ui' and
          loaded['module']['backend'] == 'native_aot' and
          not loaded['module']['native_diagnostics']['dynamic_code_supported'], loaded)
    game = client.call('runtime.gameplay.inspect', {'session_id': session})
    client.call('runtime.gameplay.edit', {'session_id': session, 'request_id': uuid.uuid4().hex,
        'expected_tick': 0, 'expected_revision': game['revision'], 'expected_structure_revision': 0,
        'values': {'Mode': 1}})
    initial_ui = client.call('runtime.ui.inspect', {'session_id': session, 'tick': 0, 'limit': 256})
    check(next(row for row in initial_ui['elements'] if row['id'] == uid(2))['text'] == 'Original', initial_ui)
    params = shared.start_parameters(session, gpu=args.gpu, mode='replay', initially_paused=False,
        sequence=[{'ticks': REPLAY_TICKS, 'move': [0, 0]}])
    ack = client.call('player.start', params)
    check(ack['active'] and ack['state'] == 'initializing' and ack['mode'] == 'replay', ack)
    initial = shared.await_state(observer, lambda owner: owner['ready'] or not owner['active'], 'replay-ready')
    check(initial['active'] and initial['ready'] and not initial['paused'] and initial['mode'] == 'replay', initial)
    check(initial['report']['hardware'] and initial['report']['nvrhi_errors'] == 0, initial)
    verify_readiness(initial)
    record['initial'] = initial

    # No runtime edit, explicit step, pause, focus or synthetic input occurs
    # inside this interval. The unchanged public helper requires true native
    # unpaused ownership and actual Tick progress, not just a Resume receipt.
    first_observation_call = len(record['calls'])
    live = verify_live_reads(client, require_unpaused=True)
    record['live_ui_observation'] = live
    interval = record['calls'][first_observation_call:]
    check(all(call['request']['method'] in ('player.inspect', 'player.ui.inspect') for call in interval),
          'Live observation issued a mutation or explicit simulation step')
    reads = live['reads']
    check(len(reads) == 32 and live['unpaused_advancing_observation_qualified'] and
          all(not row['before']['paused'] and not row['after']['paused'] for row in reads), live)
    check(reads[-1]['observation']['tick'] > reads[0]['observation']['tick'] and
          reads[-1]['observation']['ui_revision'] > reads[0]['observation']['ui_revision'],
          'Native replay Tick and C# UI revision did not both advance')
    check(all(row['before']['mode'] == row['after']['mode'] == 'replay' and
              row['before']['control_revision'] == row['after']['control_revision'] == initial['control_revision']
              for row in reads), 'Observation changed player mode or lifecycle control revision')
    request = {'player_id': initial['player_id'], 'generation': initial['generation'], 'session_id': session}
    client.reject('player.ui.inspect', dict(request, player_id=uid(999)), -32004)
    client.reject('player.ui.inspect', dict(request, generation=initial['generation']+1), -32009)
    client.reject('player.ui.inspect', dict(request, session_id=uuid.uuid4().hex), -32009)
    client.reject('player.ui.inspect', dict(request, tick=reads[0]['observation']['tick']), -32009)
    client.reject('player.ui.inspect', dict(request, ui_revision=reads[0]['observation']['ui_revision']), -32009)
    client.reject('player.ui.inspect', dict(request, tick=True), -32602)
    client.reject('player.ui.inspect', dict(request, unknown=True), -32602)

    current = client.call('player.inspect')
    if current['active']:
        response = client.reply('player.control', shared.control_parameters(current, 'stop'))
        if 'error' in response:
            # Completion can win this real race after the final observation.
            # Accept only a verified natural replay completion, never a live
            # owner with an unexplained rejected stop or a changed identity.
            check(response['error']['code'] == -32009, response)
            completed = client.call('player.inspect')
            check(not completed['active'] and completed['player_id'] == initial['player_id'] and
                  completed['generation'] == initial['generation'] and
                  completed['report']['stop_reason'] == 'replay_complete', completed)
    terminal = shared.await_state(observer, lambda owner: not owner['active'], 'replay-stopped')
    check(terminal['state'] == 'finished' and terminal['report']['success'] and
          terminal['report']['stop_reason'] in ('requested_stop', 'replay_complete') and
          terminal['player_id'] == initial['player_id'] and terminal['generation'] == initial['generation'], terminal)
    verify_readiness(terminal)
    final = client.call('runtime.inspect', {'session_id': session})
    game = client.call('runtime.gameplay.inspect', {'session_id': session})
    ui = client.call('runtime.ui.inspect', {'session_id': session, 'tick': final['tick'],
                                          'ui_revision': final['ui_revision'], 'limit': 256})
    label = next(row for row in ui['elements'] if row['id'] == uid(2))
    values = game['module']['values']
    check(values['Mode'] == 1 and values['ControlCalls'] == 0 and values['Ticks'] == final['tick'] and
          initial['tick'] < final['tick'] <= REPLAY_TICKS and
          label['text'] == 'Tick '+str(values['Ticks']) and label['text'] != 'Original' and
          ui['ui_revision'] > initial_ui['ui_revision'], 'Native Tick did not produce the observed logical UI: '+str((final, values, label)))
    client.reject('player.ui.inspect', request, -32003)
    record['terminal'] = terminal
    record['final'] = {'runtime': final, 'gameplay': game, 'ui': ui}
    client.call('runtime.stop', {'session_id': session})
    check(sha(world) == authored_hash, 'Read/replay workflow changed authored world bytes')
    record['checks'] = {'unattended_native_replay': True, 'bounded_neutral_replay_ticks': REPLAY_TICKS,
        'running_reads': len(reads), 'native_tick_and_ui_revision_progress': True,
        'compiled_tick_label_changed': True, 'no_control_callback': True,
        'exact_owner_and_optional_stale_guards': True, 'live_interval_reads_only': True,
        'authored_world_unchanged': True, 'terminal_ui_observation_rejected': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--native-descriptor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=180)
    parser.set_defaults(windows_interop=False)
    args = parser.parse_args()
    check(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without -O inside the owned gate')
    check(args.binary.is_file() and args.native_descriptor.is_file() and
          30 <= args.timeout <= 600 and args.gpu >= 0, 'Supply existing inputs and a timeout of 30..600 seconds')
    args.binary = args.binary.resolve(); args.native_descriptor = args.native_descriptor.resolve()
    args.output = args.output.resolve(); args.output.mkdir(parents=True, exist_ok=False)
    descriptor = json.loads(args.native_descriptor.read_text(encoding='utf-8'))
    check(descriptor['identity'] == 'poima.test.managed-ui' and descriptor['type'] == 'Poima.Tests.ManagedUiGame' and
          descriptor['target_os'] == 'Windows' and descriptor['target_arch'] == 'x86_64' and
          descriptor['call_version'] == 1 and descriptor['call_bytes'] == 80 and
          descriptor['services_version'] == 7 and descriptor['minimum_services_bytes'] == 176,
          'Supply the unchanged retained managed-UI ABI7/min176 Native AOT fixture')
    inputs = {'binary': args.binary, 'descriptor': args.native_descriptor, 'verifier': Path(__file__).resolve(),
        'shared_helper': Path(shared.__file__).resolve(),
        'ui_observation_helper': ROOT/'tests/player_ui_observation_contract.py',
        'pixel_import_helper': ROOT/'tests/scene_capture.py',
        'fixture_source': ROOT/'tests/managed_ui_gameplay/ManagedUiGame.cs'}
    for index, item in enumerate(descriptor['files']):
        relative = Path(item['path'])
        check(not relative.is_absolute() and not relative.drive and '..' not in relative.parts,
              'Artifact file escapes its parent')
        path = args.native_descriptor.parent/relative
        check(path.is_file() and path.stat().st_size == item['size'] and sha(path) == item['sha256'],
              'Retained artifact differs from descriptor: '+item['path'])
        inputs['artifact_'+str(index)] = path
    record = {'format': 'poima.player-ui-replay-observation.v1', 'passed': False,
        'inputs': {name: {'path': str(path), 'sha256': sha(path)} for name, path in inputs.items()},
        'owners': [], 'calls': [], 'limitations': [
            'Unattended recorded replay; no focused interactive or physical-input qualification.',
            'No fresh Native AOT publication, image-quality, audio, save/load or performance claim.',
            'Replay uses ordinary native owner pumping; no external stepping or synthetic focus.']}
    shared.ARGS = args; shared.RECORD = record
    endpoint = 'ui-replay-'+uuid.uuid4().hex
    host = client = observer = None
    try:
        world = args.output/'world.json'
        host = shared.ProcessOwner([str(args.binary), 'serve', shared.native(world), '--endpoint', endpoint],
                                   'ui-replay-host', endpoint, 'serve')
        client = shared.Client(endpoint, 'ui-replay-client')
        observer = shared.Client(endpoint, 'ui-replay-observer')
        qualify(client, observer, args, record, world)
        observer.close(); observer = None
        check(host.process.poll() is None, 'Observer detach killed shared owner')
        client.call('host.shutdown'); client.close(); client = None
        host.close(); host = None
        for owner in record['owners']:
            expected = 'Poima shared world ready: '+endpoint+'\n' if owner['label'] == 'ui-replay-host' else ''
            check(owner['exit_code'] == 0 and owner['stderr'] == expected and not owner['stderr_truncated'], owner)
        check(all(sha(path) == record['inputs'][name]['sha256'] for name, path in inputs.items()),
              'Engine, retained module or verifier inputs changed')
        record['passed'] = True
    except BaseException as error:
        record['failure'] = repr(error); record['traceback'] = traceback.format_exc()
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_errors = []
        for owner in (observer, client, host):
            if owner is not None:
                try:
                    owner.close(expected=False)
                except BaseException as error:
                    cleanup_errors.append(repr(error))
        if cleanup_errors:
            record['passed'] = False; record['cleanup_errors'] = cleanup_errors
        record['rpc_count'] = len(record['calls'])
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        if cleanup_errors and not active_failure:
            raise AssertionError(cleanup_errors)
    print(json.dumps({'passed': record['passed'], 'rpc_count': record['rpc_count'],
                      'scope': 'unattended native replay UI observation'}))


if __name__ == '__main__':
    main()
