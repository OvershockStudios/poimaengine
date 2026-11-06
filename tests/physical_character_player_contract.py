#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the compiled character sample in a bounded Windows Vulkan player.

Run under an owned Windows Job. Uses actual background elapsed-time play and
native logical input, not physical keyboard/controller events. Capture does not
advance simulation; complete saves bracket each image. Builds nothing.
"""
import argparse
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import uuid

import physical_character_gameplay_contract as game
import player_service_contract as protocol
from fps_pose_player import require_job

ROOT = Path(__file__).resolve().parents[1]
need = game.need


class Connection:
    def __init__(self, client):
        self.client = client

    def rpc(self, method, params, error=None):
        need(error is None, 'This live-player check expects successful commands.')
        return self.client.call(method, params)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'world', 'source', 'license', 'hostfxr', 'bridge', 'assembly', 'input-profile', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Native Windows Python without optimization required.')
    require_job()
    for name in ('binary', 'world', 'source', 'license', 'hostfxr', 'bridge', 'assembly', 'input_profile', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    need(not args.output.exists() and args.output.parent.is_dir() and args.gpu >= 0,
         'Use a fresh output with an existing parent and explicit GPU.')
    args.output.mkdir()
    args.windows_interop, args.timeout = False, 30
    protocol.ARGS = args
    report = dict(format='poima.physical-character-player.v1', passed=False, errors=[],
        calls=[], owners=[], archives=[], whole_pairs=[], observations=[], controls=[],
        steps=[], shots=[], camera_hits=[], inputs={}, copies={}, captures=[], intervals=[],
        checks=[], output=str(args.output), assembly=game.identity(args.assembly),
        scope='Actual background elapsed-time Windows player, native logical inputs, Vulkan captures and complete capture-purity saves. No physical input, motion-quality, performance, AOT or export qualification.')
    protocol.RECORD = report
    host = client = driver = None
    deadline = time.monotonic()+180
    player_id = None
    try:
        document, profile, regions = game.source_contract(args, report)
        game.copied_inputs(args, report)
        sources = {Path(__file__).resolve(), args.binary, args.assembly, args.bridge, args.hostfxr,
            args.input_profile, args.source, args.license,
            ROOT/'examples/physical-character-yard/PhysicalCharacterGame.cs',
            ROOT/'examples/physical-character-yard/Poima.PhysicalCharacterGame.csproj',
            ROOT/'examples/physical-character-yard/profiles/knight.json',
            ROOT/'examples/physical-character-yard/source-manifest.json'}
        for module in tuple(sys.modules.values()):
            path = getattr(module, '__file__', None)
            if path and Path(path).suffix == '.py' and Path(path).resolve().is_relative_to(ROOT):
                sources.add(Path(path).resolve())
        for image in (args.assembly, args.bridge):
            sources.add(image.parent/'Poima.Gameplay.dll')
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                companion = image.with_suffix(suffix)
                if companion.is_file():
                    sources.add(companion)
        pins = {str(path):game.identity(path) for path in sources}
        report['inputs'].update(pins)
        saves = args.output/'saves'
        saves.mkdir()
        endpoint = 'physical-character-'+uuid.uuid4().hex
        host = protocol.ProcessOwner([str(args.binary), 'serve', str(args.output/'world.json'), '--endpoint', endpoint],
                                     'physical-character-host', endpoint, 'serve')
        client = protocol.Client(endpoint, 'physical-character-driver')
        client.call('save.configure', dict(request_id=game.uid(), expected_generation=0, root=str(saves)))
        config = dict(hostfxr=str(args.hostfxr), bridge=str(args.bridge), assembly=str(args.assembly), type=game.GAME)
        driver = game.Driver(Connection(client), report, config, document['revision'], saves, profile, regions)
        driver.start()
        initial = driver.state()
        client.call('runtime.ui.edit', dict(session_id=driver.session, request_id=game.uid(), expected_tick=0,
            expected_ui_revision=initial['ui_revision'], edits=[], modal=f'{500:032x}'))
        initial_ui = client.call('runtime.ui.inspect', dict(session_id=driver.session, tick=0))
        need(initial_ui['modal'] == f'{500:032x}', 'Welcome menu owns the initial input boundary.')
        input_info = client.call('input.inspect', dict(path=str(args.input_profile), read_only=True))
        ack = client.call('player.start', dict(session_id=driver.session, request_id=game.uid(), expected_tick=0,
            expected_generation=0, mode='background', initially_paused=True,
            controller=game.PLAYER, camera=game.CAMERA, input_profile=str(args.input_profile),
            input_revision=input_info['revision'], audio=False, gamepad=dict(mode='disabled'),
            width=1280, height=720, gpu=args.gpu, samples=4, shadow_filter='pcf3_fractional',
            pose_interpolation=True, lens_interpolation=True))
        player_id = ack['player_id']

        def inspect():
            need(time.monotonic() < deadline, 'Live-player deadline exceeded.')
            value = client.call('player.inspect', dict(player_id=player_id))
            need(value['active'] and value['player_id'] == player_id and value['mode'] == 'background',
                 'Owned player ended or changed identity/mode.')
            return value

        ready_deadline = min(deadline, time.monotonic()+40)
        while True:
            value = inspect()
            if value['ready']:
                break
            need(time.monotonic() < ready_deadline, 'No ready native presentation.')
            time.sleep(.02)
        need(value['paused'] and value['tick'] == 0, 'Initial menu must stay frozen.')

        def synchronize():
            value = inspect()
            need(value['paused'], 'Exact observation requires a paused player.')
            driver.session, driver.tick = value['session_id'], value['tick']
            return value

        def owner_control(action):
            value = inspect()
            return client.call('player.control', dict(player_id=player_id, request_id=game.uid(),
                expected_control_revision=value['control_revision'], action=action))

        def interval(label, seconds=1.0):
            before = inspect()
            if before['paused']:
                owner_control('resume')
            started = time.monotonic()
            time.sleep(seconds)
            owner_control('pause')
            after = synchronize()
            elapsed = time.monotonic()-started
            need(after['session_id'] == before['session_id'] and after['tick'] > before['tick'],
                 'Elapsed play did not advance this live game.')
            report['intervals'].append(dict(label=label, before=before, after=after, elapsed_seconds=elapsed))

        def capture(label):
            value = synchronize()
            before = driver.snapshot(label+' before capture')
            path = args.output/(label+'.bmp')
            result = client.call('player.capture', dict(player_id=player_id, request_id=game.uid(),
                expected_control_revision=value['control_revision'], session_id=driver.session, tick=driver.tick,
                expected_structure_revision=value['structure_revision'], expected_ui_revision=value['ui_revision'], path=str(path)))
            captured = result['capture']
            need(captured['hardware'] and captured['capture_written'] and captured['nvrhi_errors'] == 0 and
                 captured['width'] == 1280 and captured['height'] == 720 and captured['samples'] == 4,
                 'Actual 1280x720 four-sample hardware capture required.')
            draws = captured['render_diagnostics']['last_draws']
            need(draws['skinned_instances'] >= 1 and draws['skinned_vertices'] > 0,
                 'Original character must be submitted for skinning.')
            ui = client.call('player.ui.inspect', dict(player_id=player_id, generation=value['generation'],
                session_id=driver.session, tick=driver.tick, ui_revision=value['ui_revision']))
            after = driver.snapshot(label+' after capture')
            game.whole(report, label+' capture preserves complete state', before, after)
            report['captures'].append(dict(label=label, path=str(path), identity=game.identity(path),
                result=result, ui=ui, tick=driver.tick))

        capture('welcome')
        driver.control('begin')
        interval('begin-and-idle')
        idle = driver.observe('live-grounded-idle')
        game.grounded(idle)
        begun_ui = client.call('runtime.ui.inspect', dict(session_id=driver.session, tick=driver.tick))
        need(begun_ui['modal'] is None, 'Begin clears modal input ownership.')
        capture('idle')
        driver.control('menu')
        synchronize()
        menu = client.call('runtime.ui.inspect', dict(session_id=driver.session, tick=driver.tick))
        need(menu['modal'] == f'{500:032x}' and driver.values()['Active'] == 2,
             'Menu must pause play and own the UI input boundary.')
        driver.control('resume')
        owner_control('pause')
        synchronize()
        resumed = client.call('runtime.ui.inspect', dict(session_id=driver.session, tick=driver.tick))
        need(resumed['modal'] is None and driver.values()['Active'] == 1,
             'Resume must clear modal input ownership.')
        driver.control('target-motion')
        interval('original-run')
        running = driver.observe('live-original-run')
        need(running['rig']['animation']['clip'] == 48 and running['game']['module']['values']['GroundingEnabled'] == 0 and
             math.dist(idle['target']['world_matrix'][12:15], running['target']['world_matrix'][12:15]) > .1,
             'Original run must move the real target in elapsed play.')
        capture('running')
        driver.control('target-motion')
        interval('stop-and-ground')
        stopped = driver.observe('live-stopped-grounded')
        game.grounded(stopped)
        ray = driver.aim('chest')
        shot_tick = driver.tick
        driver.step(fire=True)
        shot = driver.values()
        need(shot['Shots'] == 1 and shot['Hits'] == 1 and shot['Takeovers'] == 1 and
             shot['LastShotTick'] == str(shot_tick) and
             shot['LastHitRegion'] == ray['result']['hit']['region_entity'] and driver.physical()['mode'] == 'passive',
             'Actual camera shot must cause named-part physical takeover.')
        capture('impact')
        interval('passive-fall', 1.5)
        fallen = driver.observe('live-passive-fall')
        need(fallen['physical']['mode'] == 'passive' and fallen['game']['module']['values']['Hits'] == 1,
             'Native passive simulation must continue after the shot.')
        capture('fallen')
        need(len(report['captures']) == 5 and len(report['whole_pairs']) == 5 and len(report['intervals']) == 4,
             'Complete declared visual/live cohort required.')
        need(all(game.identity(Path(path)) == pin for path, pin in report['inputs'].items()),
             'Consumed input changed during play.')
        report['checks'] = ['Actual background elapsed-time idle/run/stop/physical play',
            'Native camera hit and named physical part takeover',
            'Five complete capture-purity save pairs and actual rendered UI/character observations']
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            try:
                current = client.call('player.inspect')
                if current['active']:
                    report['terminal'] = client.call('player.control', dict(player_id=current['player_id'],
                        request_id=game.uid(), expected_control_revision=current['control_revision'], action='stop'))
                client.call('host.shutdown')
            except BaseException:
                report['errors'].append(traceback.format_exc())
            try:
                client.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
        if host is not None:
            try:
                host.process.wait(timeout=20)
                host.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
        report['passed'] = report['passed'] and not report['errors']
        (args.output/'evidence.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=report['passed'], captures=len(report['captures']),
                          report=str(args.output/'evidence.json'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
