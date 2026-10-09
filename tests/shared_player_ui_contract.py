#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual retained NativeAOT UI callbacks in one shared Vulkan player lifetime.

Requires the already published Windows managed-UI fixture. No builds, downloads,
DLL changes, physical input, or assumption that the window has keyboard focus.
"""
import argparse
import json
from pathlib import Path
import sys
import traceback
import uuid

import player_service_contract as shared
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
check, sha, uid = shared.check, shared.sha, shared.uid


def qualify(client, observer, args, record, world):
    shared.common_checks(client, observer)
    check(client.call('player.inspect')['available'], 'Native frame-driven player is unavailable')
    descriptor = json.loads(args.native_descriptor.read_text(encoding='utf-8'))
    check(descriptor['identity'] == 'poima.test.managed-ui' and
          descriptor['type'] == 'Poima.Tests.ManagedUiGame' and
          descriptor['target_os'] == 'Windows' and descriptor['services_version'] == 7 and
          descriptor['minimum_services_bytes'] == 176, 'Supply the retained managed-UI ABI7 fixture')
    ops = [{'op': 'entity.create', 'id': uid(101), 'name': 'Compiled UI camera'},
           {'op': 'component.set', 'id': uid(101), 'type': 'Transform', 'value': {
               'position': [0,0,6], 'rotation': [0,0,0,1], 'scale': [1,1,1]}},
           {'op': 'component.set', 'id': uid(101), 'type': 'Camera',
            'value': {'vertical_fov': 60, 'near': .1, 'far': 100}}]
    # IDs 2/3 are part of the unchanged compiled fixture's public contract.
    for number, kind, text, action in [(1,'panel','',''), (2,'label','Original',''),
            (3,'button','Edit','edit'), (4,'button','Save','save'),
            (5,'button','Resume','resume'), (6,'button','Pause','pause'),
            (13,'button','Load','load')]:
        ops.append({'op': 'ui.element.set', 'id': uid(number), 'element': {
            'parent': None if number == 1 else uid(1), 'name': 'Control '+str(number),
            'kind': kind, 'text': text, 'action': action or None, 'visible': True, 'enabled': True}})
    client.call('world.transact', {'base_revision': 0, 'request_id': uuid.uuid4().hex, 'ops': ops})
    authored = world.read_bytes()
    saves = args.output/'saves'; saves.mkdir()
    client.call('save.configure', {'request_id': uuid.uuid4().hex,
        'expected_generation': 0, 'root': shared.native(saves)})
    session = uuid.uuid4().hex
    client.call('runtime.start', {'session_id': session, 'revision': 1})
    loaded = client.call('runtime.gameplay.load_native', {'session_id': session,
        'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'expected_revision': 0,
        'descriptor': shared.native(args.native_descriptor),
        'expected_descriptor_sha256': record['inputs']['descriptor']['sha256']})
    check(loaded['module']['schema']['identity'] == descriptor['identity'], loaded)
    params = shared.start_parameters(session, gpu=args.gpu)
    params.pop('controller')  # The GUI is actual gameplay; no controller or camera hack is needed.
    ack = client.call('player.start', params)
    check(ack['state'] == 'initializing' and ack['active'] and not ack['ready'], ack)
    initial = shared.await_state(observer, lambda item: item['ready'], 'ui-initial-ready')
    check(initial['paused'] and initial['tick'] == 0, initial)
    player_id = initial['player_id']

    def state():
        owner = client.call('player.inspect')
        check(owner['paused'], 'State observations require an explicit paused owner boundary')
        sid = owner['current_session_id']
        runtime = client.call('runtime.inspect', {'session_id': sid})
        return {'owner': owner,
                'runtime': runtime,
                'game': client.call('runtime.gameplay.inspect', {'session_id': sid}),
                'ui': client.call('runtime.ui.inspect', {'session_id': sid, 'tick': runtime['tick']})}

    def values(boundary):
        return boundary['game']['module']['values']

    def row(boundary, number):
        return next(item for item in boundary['ui']['elements'] if item['id'] == uid(number))

    def stable_fields(boundary):
        # Presentation counters may advance at a paused boundary; authored and
        # simulation state, UI and command revisions must not.
        return {key: boundary['owner'][key] for key in ('player_id', 'generation',
            'control_revision', 'current_session_id', 'tick', 'paused',
            'control_sequence', 'ui_revision', 'structure_revision')}

    def activate(number):
        before = state()
        check(before['owner']['paused'], 'Compiled control requires owner pause')
        request = {'session_id': before['owner']['current_session_id'],
            'request_id': uuid.uuid4().hex, 'expected_tick': before['runtime']['tick'],
            'expected_ui_revision': before['ui']['ui_revision'],
            'expected_control_sequence': before['runtime']['control_sequence'],
            'expected_gameplay_revision': before['game']['revision'], 'id': uid(number)}
        return request, client.call('runtime.ui.activate', request)

    def force_pause(label):
        current = client.call('player.inspect')
        client.call('player.control', shared.control_parameters(current, 'pause'))
        result = shared.await_state(observer, lambda item: item['ready'] and item['paused'], label)
        check(result['player_id'] == player_id, 'Owner pause replaced the player window')
        return result

    def capture(label):
        boundary = state()
        owner = boundary['owner']
        check(owner['paused'] and owner['ready'], 'Capture requires a stable paused presentation')
        path = args.output/(label+'.bmp')
        result = client.call('player.capture', {'player_id': player_id,
            'request_id': uuid.uuid4().hex, 'expected_control_revision': owner['control_revision'],
            'session_id': owner['current_session_id'], 'tick': owner['tick'],
            'expected_ui_revision': owner['ui_revision'], 'path': shared.native(path)})
        frame = result['capture']
        check(result['tick'] == owner['tick'] and result['session_id'] == owner['current_session_id']
              and frame['ui_revision'] == owner['ui_revision'] and frame['camera'] == uid(101)
              and frame['hardware'] and frame['capture_written'] and frame['nvrhi_errors'] == 0,
              result)
        camera = client.call('runtime.entity', {'session_id': owner['current_session_id'], 'id': uid(101)})
        check(frame['camera_world'] == camera['world_matrix'], 'Capture used a stale camera')
        after = state()
        check(stable_fields(after) == stable_fields(boundary) and
              after['runtime'] == boundary['runtime'] and after['game'] == boundary['game'] and
              after['ui'] == boundary['ui'], 'UI capture advanced or edited native state')
        record.setdefault('captures', {})[label] = {'result': result,
            'image': shared.frame_signature(path)}
        return pixels(path)

    original = capture('original')
    edit_request, edited = activate(3)
    shared.await_state(observer, lambda item: item['ready'], 'ui-edited-ready')
    edited_state = state(); current = values(edited_state)
    check(edited['intent'] == 0 and current['ControlCalls'] == 1 and current['LastAction'] == 1
          and current['ReadBefore'] == 1 and current['ReadAfter'] == 1 and
          current['EnabledAfter'] == 1 and int(current['SeenSequence']) == 1 and
          int(current['SeenTick']) == 0 and row(edited_state,2)['text'] == 'Control 1' and
          not row(edited_state,3)['enabled'], edited_state)
    edited_pixels = capture('edited')
    changed = [(x,y) for y,(left,right) in enumerate(zip(original,edited_pixels))
               for x,(a,b) in enumerate(zip(left,right)) if a != b]
    check(len(changed) > 20, 'Real compiled UI edit had no substantive visible result')
    record['edit_pixels'] = {'changed_pixels': len(changed),
        'bounds': [min(x for x,y in changed), min(y for x,y in changed),
                   max(x for x,y in changed), max(y for x,y in changed)]}
    check(client.call('runtime.ui.activate', edit_request) == {**edited,'replayed': True},
          'Edit retry was not the retained receipt')
    check(values(state()) == current, 'Edit retry reran compiled Control')

    # Resume may or may not tick under real focus. A newer explicit owner Pause
    # creates the decisive boundary, without pretending to synthesize focus.
    resume_request, resumed = activate(5)
    check(resumed['intent'] == 1, resumed)
    force_pause('newer-owner-pause')
    paused = state()
    check(values(paused)['ControlCalls'] == 2 and values(paused)['LastAction'] == 3, paused)
    check(client.call('runtime.ui.activate', resume_request) == {**resumed,'replayed': True},
          'Historical Resume answer changed')
    after_retry = state()
    check(stable_fields(after_retry) == stable_fields(paused) and
          after_retry['runtime'] == paused['runtime'] and values(after_retry) == values(paused)
          and after_retry['ui'] == paused['ui'], 'Historical Resume resurrected a newer owner pause')
    record['historical_resume'] = {'receipt': resumed, 'before': paused, 'after': after_retry}
    _, native_pause = activate(6)
    check(native_pause['intent'] == 2 and values(state())['LastAction'] == 4, native_pause)
    force_pause('compiled-pause-ready')

    save_request, saved = activate(4)
    check(saved['save_serviced'] and not saved['runtime_replaced'], saved)
    shared.await_state(observer, lambda item: item['ready'] and item['paused'], 'compiled-save-ready')
    saved_state = state()
    check(values(saved_state)['ControlCalls'] == 4 and values(saved_state)['LastAction'] == 2 and
          int(values(saved_state)['SaveSequence']) > 0 and
          row(saved_state,2)['text'] == 'Saved control 4', saved_state)
    slot = client.call('save.inspect', {'slot': 'ui-control'})
    check(slot['generation'] == 1 and slot['selected']['verified'], slot)
    save_files = {str(path.relative_to(saves)): sha(path) for path in saves.rglob('*') if path.is_file()}
    saved_pixels = capture('saved')
    old_epoch = saved_state['owner']['current_session_id']
    _, replaced = activate(13)
    check(replaced['runtime_replaced'] and replaced['save_serviced'] and
          replaced['session_id'] == old_epoch and replaced['current_session_id'] != old_epoch and
          replaced['control_sequence'] == saved_state['runtime']['control_sequence']+1 and
          replaced['current_control_sequence'] == saved_state['runtime']['control_sequence'], replaced)
    restored_owner = shared.await_state(observer,
        lambda item: item['ready'] and item['paused'], 'compiled-load-ready')
    restored = state(); fresh = restored_owner['current_session_id']
    check(restored_owner['player_id'] == player_id and restored_owner['generation'] == 1 and
          restored_owner['control_revision'] > saved_state['owner']['control_revision'] and
          restored_owner['tick'] == saved_state['owner']['tick'] and fresh != old_epoch,
          restored_owner)
    check(values(restored) == values(saved_state) and
          restored['ui'] == {**saved_state['ui'], 'session_id': fresh},
          'Compiled load did not restore exact saved logical UI/gameplay state')
    report = restored_owner['report']
    check(report['initial_session_id'] == session and report['current_session_id'] == fresh and
          report['runtime_replacements'] >= 1 and report['gamepad']['mode'] == 'disabled' and
          not report['audio']['enabled'], 'Replacement left stale input/device report metadata')
    check(capture('restored') == saved_pixels, 'Same-context restored UI pixels differ from saved UI')
    restored = state()
    check(client.call('runtime.ui.activate', resume_request) == {**resumed,'replayed': True},
          'Old-epoch Resume receipt was not retained')
    after_epoch_retry = state()
    check(stable_fields(after_epoch_retry) == stable_fields(restored) and
          after_epoch_retry['runtime'] == restored['runtime'] and
          values(after_epoch_retry) == values(restored) and after_epoch_retry['ui'] == restored['ui'],
          'Old-epoch Resume affected the fresh paused player')
    check(client.call('runtime.ui.activate', save_request) == {**saved,'replayed': True},
          'Old Save receipt was not retained')
    check({str(path.relative_to(saves)): sha(path) for path in saves.rglob('*') if path.is_file()} == save_files,
          'Save retry modified durable generation')
    record['replacement'] = {'result': replaced, 'before': saved_state, 'after': after_epoch_retry,
                             'verified_slot': slot}
    client.call('player.control', shared.control_parameters(client.call('player.inspect'), 'stop'))
    terminal = shared.await_state(observer, lambda item: not item['active'], 'ui-player-stopped')
    check(terminal['state'] == 'finished' and terminal['report']['stop_reason'] == 'requested_stop', terminal)
    client.call('runtime.stop', {'session_id': fresh})
    check(world.read_bytes() == authored, 'Compiled UI player changed authored world bytes')
    record['checks'] = {'compiled_native_controls': True, 'visible_same_tick_ui_edit': True,
        'resume_receipt_cannot_override_newer_pause': True, 'actual_compiled_save_load': True,
        'replacement_paused_same_player': True, 'old_epoch_resume_not_reapplied': True,
        'exact_saved_ui_pixels': True, 'authored_world_unchanged': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--native-descriptor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    check(args.binary.is_file() and args.native_descriptor.is_file() and
          30 <= args.timeout <= 600 and args.gpu >= 0, 'Supply existing inputs and timeout30..600')
    args.binary = args.binary.resolve(); args.native_descriptor = args.native_descriptor.resolve()
    args.output = args.output.resolve(); args.output.mkdir(parents=True, exist_ok=False)
    descriptor = json.loads(args.native_descriptor.read_text(encoding='utf-8'))
    inputs = {'binary': args.binary, 'descriptor': args.native_descriptor,
              'verifier': Path(__file__).resolve(), 'shared_helper': Path(shared.__file__).resolve(),
              'fixture_source': ROOT/'tests/managed_ui_gameplay/ManagedUiGame.cs',
              'pixel_helper': ROOT/'tests/scene_capture.py'}
    for index,item in enumerate(descriptor['files']):
        relative = Path(item['path'])
        check(not relative.is_absolute() and '..' not in relative.parts, 'Artifact file escapes its parent')
        path = args.native_descriptor.parent/relative
        check(path.is_file() and path.stat().st_size == item['size'] and sha(path) == item['sha256'],
              'Retained artifact file does not match descriptor: '+item['path'])
        inputs['artifact_'+str(index)] = path
    record = {'passed': False, 'inputs': {name: {'path': str(path), 'sha256': sha(path)}
        for name,path in inputs.items()}, 'owners': [], 'calls': [],
        'limitations': ['No physical input, keyboard-focus, performance, or fresh SDK build qualification',
                        'Retained baseline ABI7 Windows NativeAOT fixture; no artifact modification']}
    shared.ARGS = args; shared.RECORD = record
    endpoint = 'compiled-ui-'+uuid.uuid4().hex
    host = client = observer = None
    try:
        world = args.output/'world.json'
        host = shared.ProcessOwner([str(args.binary), 'serve', shared.native(world),
            '--endpoint', endpoint], 'compiled-ui-host', endpoint, 'serve')
        client = shared.Client(endpoint, 'compiled-ui-client')
        observer = shared.Client(endpoint, 'compiled-ui-observer')
        qualify(client, observer, args, record, world)
        observer.close(); observer = None
        check(host.process.poll() is None, 'Observer detach killed shared owner')
        client.call('host.shutdown'); client.close(); client = None
        host.close(); host = None
        for owner in record['owners']:
            expected_stderr = ('Poima shared world ready: '+endpoint+'\n'
                               if owner['label'] == 'compiled-ui-host' else '')
            check(owner['exit_code'] == 0 and owner['stderr'] == expected_stderr and
                  not owner['stderr_truncated'], 'Owner exit has unexpected output: '+str(owner))
        check(all(sha(path) == record['inputs'][name]['sha256'] for name,path in inputs.items()),
              'Input, retained artifact, or verifier changed during qualification')
        record['rpc_count'] = len(record['calls']); record['passed'] = True
    except BaseException as error:
        record['failure'] = repr(error); record['traceback'] = traceback.format_exc()
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_errors = []
        for owner in (observer,client,host):
            if owner is not None:
                try:
                    owner.close(expected=False)
                except BaseException as error:
                    cleanup_errors.append(repr(error))
        if cleanup_errors:
            record['passed'] = False; record['cleanup_errors'] = cleanup_errors
        record['rpc_count'] = len(record['calls'])
        (args.output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        if cleanup_errors and not active_failure:
            raise AssertionError(cleanup_errors)
    print(json.dumps({'passed': record['passed'], 'rpc_count': record['rpc_count']}))


if __name__ == '__main__':
    main()
