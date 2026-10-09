#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual shared Windows player consumes a spawned controller/camera instance.

Original analytic hierarchy only. Captures use real Vulkan contexts; semantic
input is submitted through the paused runtime and is not physical-input proof.
No downloads, builds, scene patches, or source changes are performed.
"""
import argparse
import copy
import json
from pathlib import Path
import sys
import traceback
import uuid

import player_service_contract as service
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
uid = service.uid
check = service.check
TEMPLATE, LOCAL_ROOT, LOCAL_CAMERA, LOCAL_PROP = map(uid, (500, 100, 101, 102))
IDENTITY = {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}


def authored(number, position, **components):
    return [{'op': 'entity.create', 'id': uid(number), 'name': 'Original '+str(number)},
            {'op': 'component.set', 'id': uid(number), 'type': 'Transform',
             'value': {**IDENTITY, 'position': position}}]+[
                 {'op': 'component.set', 'id': uid(number), 'type': kind, 'value': value}
                 for kind, value in components.items()]


def member(name, parent=None, position=None, **components):
    transform = copy.deepcopy(IDENTITY)
    if position is not None:
        transform['position'] = position
    return {'name': name, 'parent': parent, 'components': {'Transform': transform, **components}}


def exercise(client, second, world, args, record):
    discovery = service.common_checks(client, second)
    check(discovery['schema_revision'] == 67 and 'runtime.instance' in discovery['methods'], discovery)
    initial = client.call('player.inspect')
    check(initial['available'] and initial['generation'] == 0 and initial['state'] == 'absent', initial)
    graph = {
        LOCAL_ROOT: member('Reusable controller', CharacterController={
            'radius': .3, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': LOCAL_CAMERA}),
        LOCAL_CAMERA: member('Reusable camera', LOCAL_ROOT, [0, 1.6, 0],
                            Camera={'vertical_fov': 60, 'near': .1, 'far': 100}),
        LOCAL_PROP: member('Reusable marker', LOCAL_ROOT, [1, 1.2, -3],
                          MeshRenderer={'primitive': 'box', 'visible': True, 'albedo': [.01, .7, .1]})}
    operations = authored(1, [0, -.5, 0], MeshRenderer={
        'primitive': 'box', 'visible': True, 'albedo': [.14, .2, .24]},
        BoxCollider={'half_extents': [10, .5, 10], 'motion': 'static', 'mass': 10,
                     'friction': .5, 'restitution': 0})
    operations += authored(20, [0, 1.6, -5], MeshRenderer={
        'primitive': 'box', 'visible': True, 'albedo': [.9, .01, .01]})
    operations.append({'op': 'template.set', 'id': TEMPLATE, 'name': 'Original player instance',
                       'root': LOCAL_ROOT, 'entities': graph})
    client.call('world.transact', {'base_revision': 0, 'request_id': uuid.uuid4().hex, 'ops': operations})
    authored_bytes = world.read_bytes()
    session = uuid.uuid4().hex
    client.call('runtime.start', {'session_id': session, 'revision': 1})
    born = client.call('runtime.structure.transact', {
        'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0,
        'expected_structure_revision': 0, 'spawns': [{'template_id': TEMPLATE}]})
    check(len(born['spawned']) == 1 and born['structure_revision'] == 1, born)
    root = born['spawned'][0]
    instance = client.call('runtime.instance', {'session_id': session, 'tick': 0, 'id': root,
                                              'expected_structure_revision': 1})
    check(instance['root'] == root and instance['template_id'] == TEMPLATE and
          set(instance['nodes']) == set(graph) and instance['nodes'][LOCAL_ROOT] == root, instance)
    camera = instance['nodes'][LOCAL_CAMERA]
    check(len(set(instance['nodes'].values())) == 3 and
          not set(instance['nodes'].values()) & {uid(1), uid(20)}, instance)
    parameters = service.start_parameters(session, camera=camera, controller=root,
                                          expected_structure_revision=1, gpu=args.gpu)
    started = client.call('player.start', parameters)
    check(started['generation'] == 1 and started['active'] and not started['ready'] and
          started['state'] == 'initializing' and started['tick'] == 0 and
          started['structure_revision'] == 1, started)
    check(client.call('player.start', parameters) == {**started, 'replayed': True}, 'Start retry changed outcome')
    ready = service.await_state(second, lambda value: value['ready'], 'spawned-player-ready')
    check(ready['generation'] == 1 and ready['player_id'] == started['player_id'] and
          ready['state'] == 'paused' and ready['paused'] and ready['tick'] == 0 and
          ready['session_id'] == session and ready['structure_revision'] == 1, ready)
    check(ready['report']['camera'] == camera and ready['report']['hardware'] and
          ready['report']['nvrhi_errors'] == 0, ready)

    def capture(name, boundary):
        path = args.output/(name+'.bmp')
        before = client.call('runtime.inspect', {'session_id': session})
        actor = client.call('runtime.entity', {'session_id': session, 'id': root})
        response = client.call('player.capture', {
            'player_id': boundary['player_id'], 'request_id': uuid.uuid4().hex,
            'expected_control_revision': boundary['control_revision'], 'session_id': session,
            'tick': boundary['tick'], 'expected_structure_revision': 1, 'path': service.native(path)})
        rendered = response['capture']
        runtime_camera = client.call('runtime.entity', {'session_id': session, 'id': camera})
        check(rendered['camera'] == camera and rendered['camera_world'] == runtime_camera['world_matrix'],
              'Capture did not use the generated child camera')
        check(rendered['hardware'] and rendered['capture_written'] and rendered['nvrhi_errors'] == 0 and
              rendered['samples'] == 1 and rendered['lens']['vertical_fov'] == 60, rendered)
        check(client.call('runtime.inspect', {'session_id': session}) == before and
              client.call('runtime.entity', {'session_id': session, 'id': root}) == actor,
              'Capture advanced or mutated the generated actor')
        record.setdefault('captures', {})[name] = {'response': response, 'image': service.frame_signature(path)}
        return pixels(path)

    initial_pixels = capture('initial', ready)
    for _ in range(3):
        frozen = second.call('player.inspect')
        check(frozen['tick'] == 0 and frozen['paused'] and frozen['generation'] == 1, frozen)
    original_actor = client.call('runtime.entity', {'session_id': session, 'id': root})
    stepped = client.call('runtime.step', {
        'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0,
        'expected_structure_revision': 1, 'ticks': 3,
        'inputs': [{'entity': root, 'move': [.2, .6], 'look': [12, -5]}]})
    check(stepped['tick'] == 3, stepped)
    boundary = service.await_state(second, lambda value: value['ready'], 'spawned-player-stepped')
    check(boundary['tick'] == 3 and boundary['paused'] and boundary['generation'] == 1 and
          boundary['player_id'] == ready['player_id'], boundary)
    check(client.call('runtime.entity', {'session_id': session, 'id': root}) != original_actor,
          'Semantic input did not affect generated controller')
    stepped_pixels = capture('stepped', boundary)
    check(stepped_pixels != initial_pixels, 'Generated movement/look had no visible effect')
    stopped = client.call('player.control', service.control_parameters(boundary, 'stop'))
    check(not stopped['active'] and not stopped['ready'] and stopped['state'] == 'finished' and
          stopped['generation'] == 1 and stopped['tick'] == 3, stopped)
    final = second.call('player.inspect')
    check(not final['active'] and final['generation'] == 1 and final['state'] == 'finished' and
          final['report']['success'] and final['report']['nvrhi_errors'] == 0, final)
    # A separate stopped-runtime capture is an independent presentation consumer
    # of precisely the same native scene, lens and generated camera boundary.
    reference = args.output/'independent.bmp'
    independent = client.call('runtime.capture', {'session_id': session, 'tick': 3,
        'camera': camera, 'path': service.native(reference), 'width': 640, 'height': 480,
        'gpu': args.gpu, 'samples': 1, 'frames_in_flight': 1})
    check(independent['capture_written'] and independent['hardware'] and independent['nvrhi_errors'] == 0,
          independent)
    check(pixels(reference) == stepped_pixels, 'Player and independent runtime capture pixels disagree')
    record['independent_capture'] = {'response': independent, 'image': service.frame_signature(reference)}
    removed = client.call('runtime.structure.transact', {
        'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 3,
        'expected_structure_revision': 1, 'despawns': [root]})
    check(removed['structure_revision'] == 2, removed)
    for identity in instance['nodes'].values():
        client.reject('runtime.entity', {'session_id': session, 'id': identity}, -32004)
    client.reject('runtime.instance', {'session_id': session, 'tick': 3, 'id': root,
                                     'expected_structure_revision': 2}, -32004)
    before_rejected = client.call('player.inspect')
    stale = service.start_parameters(session, generation=1, expected_tick=3,
        expected_structure_revision=2, camera=camera, controller=root, gpu=args.gpu)
    client.reject('player.start', stale, -32004)
    camera_only = {**stale, 'request_id': uuid.uuid4().hex}
    camera_only.pop('controller')
    client.reject('player.start', camera_only, -32602)
    check(client.call('player.inspect') == before_rejected, 'Rejected stale handles changed player generation/state')
    check(world.read_bytes() == authored_bytes, 'Runtime instance activity changed authored source bytes')
    check(client.call('runtime.inspect', {'session_id': session})['entities'] == 2,
          'Whole instance retirement removed authored entities or retained members')
    record['instance'] = instance
    record['checks'] = {'generated_controller_camera': True, 'paused_semantic_input': True,
                        'captures_do_not_advance': True, 'independent_pixels_equal': True,
                        'whole_instance_retired': True, 'stale_handles_rejected_without_generation': True,
                        'authored_bytes_unchanged': True}
    client.call('runtime.stop', {'session_id': session})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if not args.binary.is_file() or not 30 <= args.timeout <= 600 or not 0 <= args.gpu <= 4095:
        parser.error('Supply an existing Windows binary, gpu0..4095 and timeout30..600')
    if args.output.exists():
        parser.error('--output must be new')
    args.binary = args.binary.resolve();args.output = args.output.resolve();args.output.mkdir(parents=True)
    service.ARGS = args
    pins = {str(path.relative_to(ROOT)): service.sha(path) for path in
            (Path(__file__).resolve(), ROOT/'tests/player_service_contract.py', ROOT/'tests/scene_capture.py')}
    record = {'passed': False, 'binary_sha256': service.sha(args.binary), 'sources': pins,
              'owners': [], 'calls': [], 'limitations': [
                  'Synthetic semantic input only; no physical mouse/gamepad or performance claim',
                  'Original analytic unskinned hierarchy; imported weighted-character checks are separate']}
    service.RECORD = record
    host = client = second = None
    endpoint = 'instance-player-'+uuid.uuid4().hex
    world = args.output/'world.json'
    try:
        host = service.ProcessOwner([str(args.binary), 'serve', service.native(world), '--endpoint', endpoint],
                                    'instance-player-host', endpoint, 'serve')
        client = service.Client(endpoint, 'instance-author');second = service.Client(endpoint, 'instance-observer')
        exercise(client, second, world, args, record)
        second.close();second = None
        check(host.process.poll() is None, 'Detaching observer killed shared host')
        client.call('host.shutdown');client.close();client = None
        host.process.wait(timeout=20);host.close();host = None
        check(service.sha(args.binary) == record['binary_sha256'], 'Native binary changed')
        for name, expected in pins.items():
            check(service.sha(ROOT/name) == expected, 'Verifier dependency changed: '+name)
        check(all(row['exit_code'] == 0 and not row['stderr_truncated'] and
                  'NVRHI' not in row['stderr'] for row in record['owners']), record['owners'])
        record['rpc_count'] = len(record['calls']);record['passed'] = True
    except BaseException as exc:
        record.update(failure=repr(exc), traceback=traceback.format_exc())
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
            record.update(passed=False, cleanup_errors=cleanup_errors)
        record['rpc_count'] = len(record['calls'])
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        if cleanup_errors and not active_failure:
            raise AssertionError(cleanup_errors)
    print(json.dumps({'passed': record['passed'], 'rpc_count': record['rpc_count']}))


if __name__ == '__main__':
    main()
