#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paused native-player shadow-filter forwarding, not physical input or quality.

Native Windows Python, under a root-owned kill-on-close process-tree supervisor:
  python character_shadow_player_probe.py --binary <poima.exe> \
    --fixture-world <controlled-shadow-result/fixture/world.json> \
    --output <new-directory> --gpu 1

Uses the copied controlled licensed fixture; does not author another scene or
load a gameplay module. One shared host, driver and observer; two sequential
paused native windows, one BMP each. No public player.poll RPC exists: the real
shared host polls its window while clients use bounded player.inspect readiness.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import character_shadow_probe as fixture
import player_service_contract as protocol

MODES = ('pcf3_point', 'pcf3_fractional')
FORMAT = 'poima.character-shadow-player-probe.v1'


class RuntimeView(fixture.Owner):
    """Only reuse fixture's bounded committed-state projection; owns no child."""
    def __init__(self, client, world, revision, session, deadline):
        self.client, self.world = client, world
        self.revision, self.session, self.deadline = revision, session, deadline

    def rpc(self, method, params):
        fixture.need(time.monotonic() < self.deadline, 'Player probe deadline expired.')
        return self.client.call(method, params)


def same_boundary(before, after):
    fixture.need(all(before[key] == after[key] for key in ('player_id', 'generation', 'control_revision',
        'session_id', 'tick', 'structure_revision', 'ui_revision', 'control_sequence', 'paused')) and
        after['active'] and after['ready'] and after['paused'], 'Paused native player boundary changed.')


def ready(client, identity, session, generation, observations, deadline):
    local_deadline = min(deadline, time.monotonic()+30)
    for _ in range(300):
        fixture.need(time.monotonic() < local_deadline, 'Native player readiness deadline expired.')
        observed = client.call('player.inspect', dict(player_id=identity))
        observations.append(observed)
        fixture.need(time.monotonic() < local_deadline, 'Native player readiness response exceeded its deadline.')
        fixture.need(observed['player_id'] == identity and observed['generation'] == generation and
                     observed['session_id'] == session and observed['tick'] == 1,
                     'Native player switched owner/session or advanced frozen simulation.')
        fixture.need(observed['active'], 'Native player terminated before readiness; actual terminal report retained.')
        if observed['ready']:
            fixture.need(observed['paused'], 'Initially paused native player resumed unexpectedly.')
            return observed
        time.sleep(.02)
    raise AssertionError('Native player readiness observation count exceeded.')


def input_fixture(world):
    document = fixture.read_json(world)
    entities = document['entities']
    fixture.need(not document.get('ui') and not document.get('component_schemas') and not document.get('templates'),
                 'Supply the controlled shadow fixture, not the gameplay/UI-bearing full yard.')
    for entity in entities.values():
        fixture.need(not any(kind in entity['components'] for kind in ('CharacterController', 'BoxCollider',
            'MeshCollider', 'AudioEmitter')) and not any(kind.startswith('game:') for kind in entity['components']),
            'Controlled player forwarding fixture must have no physics/input/audio/gameplay components.')
    for identifier in (fixture.CAMERA, fixture.LIGHT, fixture.ENV, fixture.FLOOR, fixture.RIG):
        fixture.need(identifier in entities, 'Required original controlled fixture entity absent.')
    camera = entities[fixture.CAMERA]['components']
    fixture.need(camera['Transform']['position'] == fixture.CAMERA_POSITION and
        camera['Transform']['rotation'] == fixture.CAMERA_ROTATION and
        all(abs(camera['Camera'][name]-expected) <= 1e-6 for name, expected in
            dict(vertical_fov=fixture.FOV, near=fixture.NEAR, far=fixture.FAR).items()),
        'Controlled camera recipe differs.')
    floor = entities[fixture.FLOOR]['components']
    fixture.need(floor['Transform']['position'] == [0, -.1, 0] and floor['Transform']['scale'] == [12, .2, 12] and
                 floor['PbrMaterial']['metallic'] == 0 and floor['PbrMaterial']['roughness'] == .85,
                 'Controlled matte receiver recipe differs.')
    lights = [value['components']['Light'] for value in entities.values() if 'Light' in value['components']]
    fixture.need(len(lights) == 1 and lights[0]['kind'] == 'directional' and lights[0]['shadow']['enabled'],
                 'Player probe requires the single actually shadowed directional fixture.')
    animation = entities[fixture.RIG]['components']['AnimationRig']
    fixture.need(animation['clip'] is not None and not animation['playing'] and not animation['loop'],
                 'Selected actual clip pose is not frozen.')
    declarations = document['asset_provenance'][animation['asset']]
    fixture.need(declarations, 'Original asset provenance is missing.')
    return document, animation, list(entities), declarations


def check_render(report, mode, record):
    fixture.need(report['hardware'] and report['nvrhi_errors'] == 0 and report['samples'] == 1,
                 'Native player did not render on hardware without validation errors.')
    fixture.need(report['render_diagnostics']['shadow_filter'] == mode and
                 report['render_diagnostics']['lighting_path'] == 'deferred' and
                 report['render_diagnostics']['ambient_occlusion']['mode'] == 'none',
                 'Selected filter did not reach actual player rendering.')
    fixture.need(report['render_diagnostics']['last_draws']['skinned_instances'] == 1 and
                 report['render_diagnostics']['last_draws']['skinned_vertices'] == 1029,
                 'Original imported character was not submitted for skinning.')
    fixture.need(report['width'] == fixture.WIDTH and report['height'] == fixture.HEIGHT,
                 'Actual native extent differs from requested controlled capture.')
    if 'device' not in record:
        record['device'] = report['gpu']
    fixture.need(isinstance(report['gpu'], str) and report['gpu'] == record['device'], 'Native device identity changed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--fixture-world', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    args = parser.parse_args()
    fixture.need(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without optimization.')
    binary, source_world = args.binary.resolve(strict=True), args.fixture_world.resolve(strict=True)
    output = args.output.resolve()
    fixture.need(not output.exists() and args.gpu >= 0 and not output.is_relative_to(source_world.parent) and
                 not source_world.parent.is_relative_to(output) and not binary.is_relative_to(output),
                 'Supply a fresh output separate from immutable inputs and native runtime.')
    document, animation, ids, declarations = input_fixture(source_world)
    source_store = Path(str(source_world)+'.assets')
    original = dict(world_sha256=fixture.sha(source_world), assets=fixture.inventory(source_store))
    sources = [Path(__file__), ROOT/'tests/character_shadow_probe.py', ROOT/'tests/player_service_contract.py',
               ROOT/'tests/scene_capture.py', ROOT/'tools/python/poima_client/core_responses.v1.json']
    sources += sorted((ROOT/'tools/python/poima_client').glob('*.py'))
    pins = {path.relative_to(ROOT).as_posix(): fixture.sha(path) for path in sources}
    dependency_pins = {name: fixture.sha(binary.parent/name) for name in ('runtime.json', 'phonon.dll')}
    record = dict(format=FORMAT, passed=False, binary_sha256=fixture.sha(binary), binary_dependencies=dependency_pins,
        source_pins=pins, input_inventory=original, owners=[], calls=[], modes=[], primary_capture_count=2,
        scope='Actual paused player mode forwarding and immutable clip pose; no physical input, filter quality or performance claim.')
    protocol.ARGS = argparse.Namespace(binary=binary, output=output, gpu=args.gpu, windows_interop=False, timeout=45)
    protocol.RECORD = record
    output.mkdir(parents=True)
    world = output/'world.json'
    host = client = observer = None
    active_view = None
    deadline = time.monotonic()+240
    failure = None
    try:
        shutil.copyfile(source_world, world)
        shutil.copytree(source_store, Path(str(world)+'.assets'))
        endpoint = 'shadow-player-'+uuid.uuid4().hex
        record['shared_host_ready_stderr'] = 'Poima shared world ready: '+endpoint+'\n'
        host = protocol.ProcessOwner([str(binary), 'serve', str(world), '--endpoint', endpoint],
            'shadow-player-host', endpoint, 'serve')
        client = protocol.Client(endpoint, 'shadow-player-driver')
        observer = protocol.Client(endpoint, 'shadow-player-observer')
        discovery = protocol.common_checks(client, observer)
        property_ = discovery['methods']['player.start']['properties'].get('shadow_filter')
        fixture.need(property_ is not None and property_['enum'] == list(MODES), 'Player filter discovery is absent.')
        inspection = client.call('world.inspect')
        fixture.need(inspection['revision'] == document['revision'], 'Copied authoring revision differs.')
        for declaration in declarations:
            value = client.call('asset.provenance.inspect', dict(record=declaration))
            fixture.need(value['metadata']['asset'] == animation['asset'] and
                         value['metadata']['license']['identifier'] == 'CC0-1.0', 'Original CC0 declaration differs.')
        previous_pose = None
        for mode in MODES:
            fixture.need(time.monotonic() < deadline, 'Player probe overall deadline expired.')
            mode_record = dict(mode=mode, readiness=[])
            record['modes'].append(mode_record)
            previous_owner = client.call('player.inspect')
            fixture.need(not previous_owner['active'], 'Previous player owner is still active.')
            session = uuid.uuid4().hex
            active_view = RuntimeView(client, world, document['revision'], session, deadline)
            start_runtime = client.call('runtime.start', dict(session_id=session, revision=document['revision']))
            fixture.need(start_runtime['started'] and start_runtime['tick'] == 0, 'Fresh runtime failed.')
            step = client.call('runtime.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
                ticks=1, animations=[dict(entity=fixture.RIG, clip=animation['clip'], time=animation['time'],
                                         speed=animation['speed'], loop=False, playing=False)]))
            fixture.need(step['tick'] == 1, 'Legitimate frozen pose seek failed.')
            state = active_view.snapshot(ids)
            rig = next(row['state'] for row in state['entities'] if row['id'] == fixture.RIG)
            fixture.need(rig['animation']['clip'] == animation['clip'] and not rig['animation']['playing'] and
                         abs(rig['animation']['time']-animation['time']) <= 1e-6, 'Actual clip pose differs.')
            # Cross-mode equality excludes the fresh session owner guard only.
            if previous_pose is not None:
                fixture.need(state['entities'] == previous_pose, 'Player filter modes changed actual pose/transforms.')
            previous_pose = state['entities']
            mode_record['before'] = state
            params = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=1,
                expected_generation=previous_owner['generation'], camera=fixture.CAMERA, mode='interactive',
                initially_paused=True, audio=False, gamepad=dict(mode='disabled'),
                width=fixture.WIDTH, height=fixture.HEIGHT, gpu=args.gpu, samples=1, frames_in_flight=1,
                lighting_path='deferred', ambient_occlusion=dict(mode='none', quality='medium', radius=1),
                reconstruction='none', shadow_filter=mode, profile=True)
            ack = client.call('player.start', params)
            mode_record['start'] = ack
            fixture.need(ack['active'] and ack['paused'] and ack['generation'] == previous_owner['generation']+1,
                         'Native player acknowledgement differs.')
            observed = ready(observer, ack['player_id'], session, ack['generation'], mode_record['readiness'], deadline)
            check_render(observed['report'], mode, record)
            fixture.need(active_view.snapshot(ids) == state and fixture.sha(world) == original['world_sha256'],
                         'Native player initialization changed frozen pose or authored world.')
            path = output/(mode+'.bmp')
            capture_params = dict(player_id=ack['player_id'], request_id=uuid.uuid4().hex,
                expected_control_revision=observed['control_revision'], session_id=session, tick=1, path=str(path),
                expected_structure_revision=observed['structure_revision'], expected_ui_revision=observed['ui_revision'])
            result = client.call('player.capture', capture_params)
            mode_record['capture'] = result
            captured = result['capture']
            fixture.need(captured['capture_written'] and path.is_file(), 'Actual player BMP is missing.')
            check_render(captured, mode, record)
            fixture.need(captured['camera_world'] == next(row['state']['world_matrix'] for row in state['entities']
                                                        if row['id'] == fixture.CAMERA), 'Player captured a foreign camera.')
            image = fixture.pixels(path)
            fixture.need(len(image) == fixture.HEIGHT and all(len(row) == fixture.WIDTH for row in image),
                         'Actual player bitmap extent differs.')
            mode_record['image'] = path.name
            mode_record['image_sha256'] = fixture.sha(path)
            after = observer.call('player.inspect', dict(player_id=ack['player_id']))
            same_boundary(observed, after)
            mode_record['after'] = active_view.snapshot(ids)
            fixture.need(mode_record['after'] == state and fixture.sha(world) == original['world_sha256'],
                         'Player capture advanced simulation or changed full frozen state.')
            retry = client.call('player.capture', capture_params)
            fixture.need(retry == {**result, 'replayed': True} and fixture.sha(path) == mode_record['image_sha256'],
                         'Player capture receipt retry changed exclusive output.')
            stopped = client.call('player.control', protocol.control_parameters(after, 'stop'))
            fixture.need(not stopped['active'], 'Guarded player stop did not terminate native owner.')
            terminal = observer.call('player.inspect', dict(player_id=ack['player_id']))
            mode_record['terminal'] = terminal
            fixture.need(not terminal['active'] and terminal['generation'] == ack['generation'] and
                         terminal['tick'] == 1 and terminal['session_id'] == session and
                         terminal['report']['success'], 'Native terminal report is not successful same-owner state.')
            check_render(terminal['report'], mode, record)
            fixture.need(active_view.snapshot(ids) == state, 'Normal native stop changed frozen pose/state.')
            active_view.stop()
            active_view = None
        observer.close()
        observer = None
        client.call('host.shutdown')
        client.close()
        client = None
        host.process.wait(timeout=20)
        host.close()
        host = None
        fixture.need(len(record['modes']) == 2 and all(row['terminal']['report']['nvrhi_errors'] == 0 for row in record['modes']),
                     'Declared two-mode qualification is incomplete.')
        fixture.need(time.monotonic() < deadline, 'Completed player probe exceeded its overall deadline.')
        record['passed'] = True
    except BaseException as error:
        failure = error
        record['error'], record['traceback'] = repr(error), traceback.format_exc()
    finally:
        if client is not None:
            try:
                current = client.call('player.inspect')
                record['cleanup_observation'] = current
                if current['active']:
                    record['cleanup_stop'] = client.call('player.control', protocol.control_parameters(current, 'stop'))
                if active_view is not None:
                    active_view.stop()
                client.call('host.shutdown')
            except BaseException as error:
                record['cleanup_error'] = repr(error)
                failure = failure or error
        for owner in (observer, client, host):
            if owner is not None:
                try:
                    owner.close(expected=False)
                except BaseException as error:
                    record.setdefault('cleanup_errors', []).append(repr(error))
                    failure = failure or error
        try:
            fixture.need(original == dict(world_sha256=fixture.sha(source_world), assets=fixture.inventory(source_store)),
                         'Original controlled fixture/content changed.')
            fixture.need(fixture.sha(world) == original['world_sha256'] and
                         fixture.inventory(Path(str(world)+'.assets')) == original['assets'], 'Copied fixture/content changed.')
            fixture.need(pins == {name: fixture.sha(ROOT/name) for name in pins} and
                         record['binary_sha256'] == fixture.sha(binary) and dependency_pins == {
                             name: fixture.sha(binary.parent/name) for name in dependency_pins}, 'Measured source/native inputs changed.')
            fixture.need(all(owner['exit_code'] == 0 and owner['stderr'] == (
                             record['shared_host_ready_stderr'] if owner['label'] == 'shadow-player-host' else '') and
                             not owner['stderr_truncated']
                             for owner in record['owners']), 'Native child owner did not exit cleanly.')
            record['inputs_unchanged'] = True
        except BaseException as error:
            record['integrity_error'] = repr(error)
            failure = failure or error
        record['rpc_count'] = len(record['calls'])
        if failure is not None:
            record['passed'] = False
        fixture.save_json(output/'evidence.json', record)
    if failure is not None:
        raise failure
    print(json.dumps(dict(passed=True, captures=2, modes=list(MODES), device=record['device'])))


if __name__ == '__main__':
    main()
