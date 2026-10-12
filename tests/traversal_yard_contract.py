#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the compiled Traversal Yard through its public native input/UI APIs.

Consumes supplied compiled artifacts and the pinned licensed source intake.
Builds nothing; run beneath an external process-tree owner. The walkthrough uses
ordinary caller movement, named crouch/aim and Use, never teleports or game edits.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback

from locomotion_yard_contract import OriginalOracle, observe

ROOT = Path(__file__).resolve().parents[1]
CAP = 96*1048576
DT = 1/60


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            digest.update(data)
    return digest.hexdigest()


def exact(a, b):
    if type(a) is not type(b):
        return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(exact(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(exact(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        return math.isfinite(a) and math.isfinite(b) and a.hex() == b.hex()
    return a == b


def load_launcher():
    path = ROOT/'examples/traversal-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_traversal_yard_sample', path)
    need(spec is not None and spec.loader is not None, 'Traversal Yard launcher is absent.')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def position(entity):
    return entity['world_matrix'][12:15]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'manifest', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    content = parser.add_mutually_exclusive_group(required=True)
    content.add_argument('--source-directory', type=Path)
    content.add_argument('--prepared-world', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=540)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if bool(args.descriptor) == bool(args.assembly) or (not args.descriptor and not (args.hostfxr and args.bridge)):
        parser.error('Supply --descriptor OR --assembly with --bridge and --hostfxr.')
    if args.gpu < 0 or not math.isfinite(args.timeout) or not 120 <= args.timeout <= 1200:
        parser.error('Use a nonnegative GPU and finite timeout within120..1200seconds.')
    if not 960 <= args.width <= 1920 or not 540 <= args.height <= 1080:
        parser.error('Readable bounded captures require width960..1920 and height540..1080.')
    args.output = args.output.resolve()
    if args.output.exists():
        parser.error('Output must be fresh; prior worlds, inputs and failures are retained.')
    args.output.mkdir(parents=True)
    started = time.monotonic();deadline = started+args.timeout
    if args.source_directory:
        args.source_directory = args.source_directory.resolve(strict=True)
    inputs = {}
    for name in ('binary', 'manifest', 'hostfxr', 'bridge', 'assembly', 'descriptor'):
        path = getattr(args, name)
        if path:
            path = path.resolve(strict=True);need(path.is_file(), 'Missing '+name);setattr(args, name, path);inputs[str(path)] = path
    if args.descriptor:
        metadata = json.loads(args.descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == 'Poima.Examples.TraversalYardGame' and metadata['identity'] == 'poima.examples.traversal-yard' and
             metadata['services_version'] == 7 and metadata['minimum_services_bytes'] == 336,
             'Wrong compiled traversal module/profile.')
        for member in metadata['files']:
            name = member['path'].replace('\\', '/')
            need(':' not in name and not name.startswith('/') and all(part not in ('', '.', '..') for part in name.split('/')), 'Unsafe Native AOT artifact member.')
            path = (args.descriptor.parent/name).resolve(strict=True)
            need(path.is_relative_to(args.descriptor.parent) and path.is_file() and path.stat().st_size == member['size'] and sha(path) == member['sha256'], 'Native artifact identity differs.')
            inputs[str(path)] = path
    else:
        for assembly in (args.assembly, args.bridge):
            sdk = assembly.parent/'Poima.Gameplay.dll';need(sdk.is_file(), 'Consumer/bridge SDK missing.');inputs[str(sdk)] = sdk
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                path = assembly.with_suffix(suffix)
                if path.is_file():
                    inputs[str(path)] = path
    source_names = ('tests/traversal_yard_contract.py', 'examples/traversal-yard/run.py', 'examples/traversal-yard/TraversalYardGame.cs',
        'examples/traversal-yard/Poima.TraversalYardGame.csproj', 'examples/locomotion-yard/run.py', 'tests/locomotion_yard_contract.py',
        'tests/animation_rotation_retarget.py', 'tests/animation_rotation_retarget_capture.py', 'tests/rotation_retarget_fixture.py',
        'tests/frame_transfer_fixture.py', 'tests/fbx_fixture.py', 'tests/texture_fixture.py', 'tests/gltf_fixture.py', 'tests/scene_capture.py')
    sources = [ROOT/name for name in source_names]
    sources += [p for p in (ROOT/'tools/python/poima_client').rglob('*') if p.is_file() and p.suffix in ('.py', '.json')]
    pins = {str(p.resolve()): sha(p) for p in (*sources, *inputs.values())}
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    need(manifest['format'] == 'poima.components' and {s['id'] for s in manifest['schemas']} == {'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb75', 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76'}, 'Actual compiled component schemas differ.')
    launcher = load_launcher()
    originals = {name: args.source_directory/name for name in launcher.SOURCE_SHA} if args.source_directory else {}
    need(all(p.is_file() and sha(p) == launcher.SOURCE_SHA[name] for name, p in originals.items()), 'Supply untouched pinned Kenney body,Run,Idle and license files.')
    owned_sources = args.output/'owned-sources'
    for name, path in originals.items():
        target = owned_sources/name;target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(path, target)
        need(sha(target) == launcher.SOURCE_SHA[name], 'Owned intake copy differs.')
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() if args.windows_interop and os.name != 'nt' else value
    evidence = dict(passed=False, calls=[], transport=[], owners=[], checks=[], snapshots=[], captures=[], stages={}, source_pins=pins,
        input_mode='source_intake' if args.source_directory else 'prepared_world',
        original_source_hashes=launcher.SOURCE_SHA if args.source_directory else {}, backend='native_aot' if args.descriptor else 'coreclr', limitations=[
            'Original pinned Kenney intake with the existing explicit reference-rotation policy; independent original-pose FK/weighted geometry checks do not independently decode FBX.',
            'The complete course is driven by public semantic caller input through the actual compiled game. No physical-input or human-usability qualification is claimed.',
            'Complete exact continuation compares cold-restored owners; uninterrupted warm-contact-cache trajectory equivalence is outside this test.',
            'Optional960x540 native captures do not establish frame rate or performance. Native AOT is process-pinned; compatible reload is checked only on CoreCLR.'])
    owners = [];game = None;sequence = 0;original_world = None;prepared_world = args.output/'prepared/world.json'
    def open_game(world, label):
        owner = launcher.OwnedGame(args, world, label, evidence, deadline, native, manifest);owners.append(owner);return owner
    def close_game(owner):
        if owner.closed:
            return
        owner.close();transport = owner.client.transport
        row = next(x for x in evidence['owners'] if x['label'] == owner.label)
        row.update(stderr=transport.stderr_tail, stderr_truncated=transport.stderr_truncated)
        need(row['exit_code'] == 0 and row['stderr'] == '' and row['stderr_truncated'] is False, 'Native owner did not close cleanly.')
    def raw_join():
        requests = {};responses = {};used = set()
        for row in evidence['transport']:
            message = row['message'];need(message['jsonrpc'] == '2.0' and type(message['id']) is int, 'Malformed native envelope.')
            key = (row['owner'], message['id']);table = requests if row['direction'] == 'request' else responses
            need(key not in table, 'Duplicate native envelope.');table[key] = message
        need(requests.keys() == responses.keys() and len(requests) == len(evidence['calls']), 'Lossless raw population differs.')
        for call in evidence['calls']:
            key = (call['owner'], call['rpc_id']);need(key not in used, 'Duplicate interpreted call.');used.add(key)
            need(requests[key]['method'] == call['method'] and exact(requests[key]['params'], call['params']), 'Native request differs.')
            if 'result' in call:
                need('error' not in responses[key] and exact(responses[key]['result'], call['result']), 'Native result differs.')
            else:
                failure = responses[key]['error']
                need('result' not in responses[key] and failure['code'] == call['error']['code'] and
                     failure['message'] == call['error']['message'] and exact(failure.get('data'), call['error']['data']), 'Native error differs.')
    def snapshot(owner, label):
        nonlocal sequence
        sequence += 1;slot = 'traversal-'+str(sequence)
        receipt = owner.write(slot);folder = args.output/'saves'/('slot-'+slot)
        current = json.loads((folder/'current.json').read_bytes())['payload'];entry = current['current'];name = entry['file']
        need(len(name) == 38 and name.startswith('p-') and name.endswith('.bin') and all(x in '0123456789abcdef' for x in name[2:-4]), 'Unsafe selected save member.')
        path = folder/name;raw = path.read_bytes();payload = json.loads(raw)
        need(len(raw) <= 16*1048576 and len(raw) == entry['bytes'] == receipt['bytes'] and sha(path) == entry['sha256'] == receipt['sha256'] and
             current['generation'] == entry['generation'] == receipt['generation'] == 1, 'Actual selected native payload differs.')
        need(payload['snapshot']['version'] == 9 and payload['document']['entities'][launcher.uid(100)]['components']['CharacterController']['locomotion'] == {}, 'Complete v9 frozen virtual save differs.')
        evidence['snapshots'].append(dict(label=label, slot=slot, receipt=receipt, payload_sha256=sha(path), payload_bytes=len(raw), snapshot=payload['snapshot']))
        return copy.deepcopy(payload['snapshot']), slot
    def input_row(move=(0, 0), crouch=False, aim=False, use=False):
        return dict(entity=launcher.uid(100), move=list(move), look=[0, 0], jump=False, use=use, actions=[
            dict(name='aim', held=aim, pressed=False, released=False), dict(name='crouch', held=crouch, pressed=False, released=False)])
    def step(owner, count=1, move=(0, 0), crouch=False, aim=False, use=False):
        before = owner.entity(100);old_tick = owner.tick
        owner.step(count, inputs=[input_row(move, crouch, aim, use)])
        after = owner.entity(100);a, b = position(before), position(after)
        need(owner.tick == old_tick+count and math.hypot(b[0]-a[0], b[2]-a[2]) <= 4*DT*count+.025,
             'Player moved without bounded native fixed-Tick input.')
        evidence.setdefault('movement', []).append(dict(tick=owner.tick, ticks=count, input=input_row(move, crouch, aim, use), before=a, after=b,
                                                       velocity=after['velocity'], ground=after['ground']))
        return after
    def move_to(owner, axis, target, crouch=False):
        for _ in range(120):
            current = position(owner.entity(100))[axis];delta = target-current
            if abs(delta) <= .07:
                return owner.entity(100)
            count = min(8, max(1, int(abs(delta)/(2 if crouch else 4)/DT)))
            sign = 1 if delta > 0 else -1
            step(owner, count, move=(0, sign) if axis == 0 else (sign, 0), crouch=crouch)
        raise AssertionError('Native player did not reach the authored course waypoint.')
    def stance(owner):
        return owner.rpc('runtime.character_stance', dict(session_id=owner.session, tick=owner.tick, entity=launcher.uid(100)))['state']
    def lens(owner):
        return owner.rpc('runtime.camera_lens', dict(session_id=owner.session, tick=owner.tick, entity=launcher.uid(101)))['state']
    def capture(owner, label, camera=101):
        if not args.capture:
            return
        path = args.output/(label+'.bmp');state = owner.inspect()
        result = owner.rpc('runtime.capture', dict(session_id=owner.session, tick=owner.tick, ui_revision=state['ui_revision'], camera=launcher.uid(camera),
            path=native(path), width=args.width, height=args.height, samples=4, shadow_filter='pcf3_fractional', gpu=args.gpu, capture_frames=2), timeout=90)
        need(result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0 and result['samples'] == 4 and
             result['render_diagnostics']['shadow_filter'] == 'pcf3_fractional' and path.is_file(), 'Actual sample capture failed or substituted its recipe.')
        evidence['captures'].append(dict(label=label, camera=launcher.uid(camera), sha256=sha(path), bytes=path.stat().st_size, result=result))
    def finish_outbound(owner):
        # This begins from the same durable mid-lift state in each cold owner.
        step(owner, 90)
        actor = owner.entity(100);lift = owner.entity(220)
        need(stance(owner)['ground'] == 'on_ground' and position(actor)[1] > 1.2 and
             abs(position(actor)[0]-position(lift)[0]) < .65 and abs(position(actor)[1]-1.4) < .045,
             'The player did not ride the physical lift to the elevated beacon.')
        step(owner, use=True)
        need(owner.values()['BeaconMask'] == 3, 'Elevated supported Use did not collect the lift beacon.')
        move_to(owner, 0, 5)
        step(owner, use=True)
        need(owner.values()['BeaconMask'] == 7 and owner.values()['Collected'] == 3 and owner.values()['Won'] == 0,
             'Native player did not collect the far beacon without an early win.')
    try:
        world = args.output/'world.json'
        if args.prepared_world:
            args.prepared_world = args.prepared_world.resolve(strict=True)
            evidence['prepared_world_sha256'] = sha(args.prepared_world)
            pins[str(args.prepared_world)] = evidence['prepared_world_sha256']
            for path in Path(str(args.prepared_world)+'.assets').rglob('*'):
                if path.is_file():
                    pins[str(path.resolve())] = sha(path)
            launcher.copy_prepared(args.prepared_world, world)
        game = open_game(world, 'author-and-play' if args.source_directory else 'prepared-and-play')
        imported = game.author(owned_sources) if args.source_directory else None
        if imported is not None:
            evidence['authored'] = imported
        original_world = world.read_bytes();(args.output/'course-original.json').write_bytes(original_world)
        doc = json.loads(original_world)
        need(doc['revision'] == 5 and doc['entities'][launcher.uid(400)]['parent'] == launcher.uid(300), 'Separate retained courier/visual graph or final navigation publication differs.')
        for actor in (100, 300):
            need(doc['entities'][launcher.uid(actor)]['components']['CharacterController']['locomotion'] == {}, 'Actor lacks explicit virtual locomotion.')
        for n, target in launcher.TARGETS.items():
            need(doc['entities'][launcher.uid(n)]['components']['Transform']['position'] == list(target), 'Authored objective target differs.')
        oracle = OriginalOracle(game, args.source_directory, world, imported, evidence, native) if imported is not None else None
        def animation_observation(owner):
            if oracle is not None:
                return observe(owner, oracle, doc, evidence)
            actor, rig, values = owner.entity(300), owner.entity(400), owner.values()
            animation = rig['animation']
            need(animation is not None and animation['loop'] and animation['playing'] and animation['clip'] in (0, 1) and
                 values['AnimationClip'] == animation['clip'] and abs(values['AnimationRate']-animation['speed']) < 1e-10,
                 'Cooked courier clock differs from its compiled committed-motion state.')
            row = dict(tick=owner.tick, actor=actor, rig=rig, values=values, source_pose_compared=False)
            evidence.setdefault('visual_observations', []).append(row)
            return row
        game.start();game.saves(args.output/'saves');step(game, 16)
        idle = animation_observation(game)
        need(idle['rig']['animation']['clip'] == 1 and idle['rig']['animation']['time'] > 0, 'Compiled courier did not advance genuine original Idle.')
        profilepath = args.output/'traversal.poima-input.json';game.input_profile(profilepath)
        evaluate = game.rpc('input.evaluate', dict(path=native(profilepath), events=[dict(control='key.c', down=True), dict(control='mouse.3', down=True), dict(consume=True)]))
        evidence['stages']['profile'] = evaluate
        ui = game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick))
        buttons = {row['id']: row for row in ui['elements']}
        for action, n in launcher.ACTIONS.items():
            need(buttons[launcher.uid(n)]['action'] == action and buttons[launcher.uid(n)]['eligible'], 'Actual native control is not eligible: '+action)
        capture(game, 'starting-first-person')
        player = move_to(game, 0, -3.65)
        need(abs(position(player)[1]-.6) < .05, 'Player did not climb the independently authored three .2m treads.')
        before = position(player);step(game, 12, move=(0, 1));blocked = game.entity(100)
        need(position(blocked)[0] < -3.45 and position(blocked)[0]-before[0] < .2, 'Standing capsule passed the low lintel.')
        start_fov = lens(game)['world']['vertical_fov']
        step(game, 6, aim=True);mid = lens(game)['world']['vertical_fov']
        expected_mid = math.degrees(2*math.atan(math.sqrt(math.tan(math.radians(start_fov)/2)*math.tan(math.radians(45)/2))))
        need(abs(mid-expected_mid) < 1e-10 and 45 < mid < start_fov, 'Compiled half-duration aim differs from independent perspective oracle.')
        step(game, 6, aim=True);need(lens(game)['world']['vertical_fov'] == 45, 'Held aim did not reach its exact endpoint.')
        initial_eye = game.entity(101)['local_transform']['position'][1]
        step(game, crouch=True);move_to(game, 0, -2.7, crouch=True)
        need(stance(game)['crouched'] and abs(initial_eye-game.entity(101)['local_transform']['position'][1]-.6) < 1e-6, 'Real crouch did not lower the native capsule and eye.')
        step(game, crouch=True, use=True)
        need(game.values()['BeaconMask'] == 1 and game.values()['CrouchedBeacon'] == 1, 'Use did not collect the committed crouched beacon.')
        step(game);ceiling = stance(game)
        need(ceiling['crouched'] and ceiling['stand_blocked'] and not ceiling['requested_crouch'], 'Released crouch did not preserve blocked standing intent.')
        move_to(game, 0, .1)
        need(not stance(game)['crouched'], 'Standing did not retry automatically after leaving the lintel.')
        move_to(game, 0, 1.5)
        need(abs(position(game.entity(100))[1]-.2) < .05, 'Player did not board the lowered physical lift.')
        step(game, use=True);moves = game.values()['LiftMoves'];step(game, 20)
        physical = game.entity(100);need(.35 < position(physical)[1] < 1.2 and game.entity(220)['motion_remaining_ticks'] > 0, 'Lift did not carry its supported player during a real motion interval.')
        step(game, use=True);need(game.values()['LiftMoves'] == moves and game.values()['LiftBusyNotice'] == 1, 'Busy lift Use was not bounded to the current motion.')
        run = animation_observation(game)
        need(run['rig']['animation']['clip'] == 0 and run['values']['LastHorizontalSpeed'] > 0, 'Controlled courier did not select genuine Run from committed movement.')
        checkpoint, checkpoint_slot = snapshot(game, 'mid-lift')
        if args.descriptor:
            game.load(error=-32060)
            unchanged, _ = snapshot(game, 'after-denied-aot-replacement');need(exact(unchanged, checkpoint), 'Denied AOT replacement mutated complete runtime state.')
        else:
            game.load();reloaded, _ = snapshot(game, 'after-compatible-reload')
            expected = copy.deepcopy(checkpoint);expected['payload']['gameplay_revision'] += 1;expected['sha256'] = reloaded['sha256']
            need(reloaded['sha256'] != checkpoint['sha256'] and exact(reloaded, expected), 'Compatible reload changed more than gameplay revision and its required digest.')
        game.restore(checkpoint_slot);cold_reference, _ = snapshot(game, 'cold-reference')
        need(exact(cold_reference, checkpoint), 'Immediate same-owner restoration changed complete v9 snapshot.')
        finish_outbound(game);continued, _ = snapshot(game, 'collected-reference')
        capture(game, 'collected-overview', 102)
        need(world.read_bytes() == original_world, 'Gameplay changed authored course bytes.')
        close_game(game)
        if args.source_directory:
            retained_sources = args.output/'retained-owned-sources';owned_sources.rename(retained_sources)
            need(not owned_sources.exists() and all(sha(retained_sources/name) == pin for name, pin in launcher.SOURCE_SHA.items()), 'Owned sources were not retained under an absent original path.')
        prepared_world.parent.mkdir();launcher.copy_prepared(world, prepared_world)
        game = open_game(prepared_world, 'cooked-source-free-reopen');game.saves(args.output/'saves');game.restore(checkpoint_slot)
        fresh_state, _ = snapshot(game, 'cold-fresh');need(exact(fresh_state, checkpoint), 'Source-free cooked reconstruction changed full native/managed state.')
        finish_outbound(game);fresh_continued, _ = snapshot(game, 'collected-fresh')
        need(exact(fresh_continued, continued), 'Two cold owners diverged in complete v9 physics/animation/typed/UI/game/lens state.')
        evidence['checks'].append('Caller-input steps, crouch passage, physical lift and semantic Use collect the course; all native/compiled state survives exact cold reconstructed continuation.')
        paused = game.control('pause');need(paused['intent'] == 2 and game.values()['Paused'] == 1, 'Native pause control failed.')
        saved = game.control('save');need(saved['save_serviced'] and saved['save_operation']['state'] == 3, 'Compiled named UI save failed.')
        ui_checkpoint, _ = snapshot(game, 'ui-save-checkpoint')
        game.control('status');game.control('resume');step(game, 6)
        loaded = game.control('load');need(loaded['save_serviced'] and loaded['runtime_replaced'] and loaded['save_operation']['state'] == 3, 'Compiled named UI load failed.')
        ui_loaded, _ = snapshot(game, 'ui-load-restored');need(exact(ui_loaded, ui_checkpoint), 'Named UI load changed complete immediate snapshot.')
        game.control('status');need(game.values()['LastSaveState'] == 3 and game.values()['LastSaveError'] == 0, 'Compiled status did not reconcile successful load.')
        game.control('resume')
        remaining = game.entity(220)['motion_remaining_ticks']
        if remaining:
            step(game, remaining)
        need(game.entity(220)['motion_remaining_ticks'] == 0, 'The prior physical lift trip did not settle before its next UI request.')
        prior_moves = game.values()['LiftMoves'];game.control('lift');step(game)
        need(game.values()['LiftMoves'] == prior_moves+1, 'Native Lift UI control did not publish a real kinematic request.')
        # Return by the connected shallow ramp, rather than bypassing movement
        # with a state edit or teleport. The initial facing maps strafe to +Z.
        move_to(game, 2, 8.2);move_to(game, 0, .1);ramp = move_to(game, 0, -1)
        p = position(ramp);normal = ramp['locomotion']['ground_normal'];angle = math.atan(.3)
        need(abs(p[1]-(-.3*p[0]+.3*(1/math.cos(angle)-1))) < .06 and
             abs(normal[0]-math.sin(angle)) < .04 and abs(normal[1]-math.cos(angle)) < .04,
             'Native return-ramp support differs from independent plane/capsule geometry.')
        move_to(game, 0, -2.7, crouch=True);move_to(game, 0, -4, crouch=True)
        move_to(game, 0, -7.5);need(game.values()['Won'] == 0, 'Return completed outside the actual gate radius.')
        move_to(game, 2, 6);step(game, 12)
        values = game.values();need(values['Won'] == 1 and values['Collected'] == 3 and values['BeaconMask'] == 7 and values['CrouchedBeacon'] == 1 and values['LiftMoves'] > 0,
                                   'Actual compiled player did not complete and return the three-beacon objective.')
        need(math.dist(position(game.entity(100)), launcher.TARGETS[213]) < 1.25, 'Completion has no native gate-position witness.')
        final, _ = snapshot(game, 'completed');evidence['stages'].update(completed=final, values=values, return_ramp=ramp,
            ui_save=saved, ui_load=loaded, source_free_reopen=dict(original_owned_path_absent=not owned_sources.exists(),
                retained_original_bytes=bool(args.source_directory), prepared_world_sha256=sha(prepared_world)))
        capture(game, 'returned-first-person')
        need(world.read_bytes() == original_world and prepared_world.read_bytes() == original_world, 'Live play changed either prepared authored document.')
        need(all(sha(path) == launcher.SOURCE_SHA[name] for name, path in originals.items()), 'Caller licensed sources changed.')
        need(all(sha(Path(name)) == pin for name, pin in pins.items()), 'Consumed sources or artifacts changed.')
        close_game(game);raw_join()
        evidence['checks'] += [('Original retained rig/controller graph and original-source joint/weighted geometry observations prove controlled Idle/Run playback.' if oracle is not None
                               else 'A prepared cooked world reopens without any FBX inspection/import; retained courier clocks match its compiled movement/animation state.'),
            'Named held aim matches an independent perspective midpoint and exact endpoint; native standing clearance retries after real crouched passage.',
            'Six real named UI controls operate; compiled save/load restores complete native/managed state and source-free cooked ownership reopens.',
            'A complete player course and return uses semantic inputs alone; native ramp/support geometry and final objective are independently witnessed.']
        evidence['passed'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        errors = []
        for owner in reversed(owners):
            try:
                close_game(owner)
            except BaseException:
                errors.append(traceback.format_exc())
        evidence['cleanup_errors'] = errors;evidence['passed'] = evidence['passed'] and not errors
        evidence['rpc_count'] = len(evidence['calls']);evidence['seconds'] = time.monotonic()-started
        evidence['source_unchanged'] = all(Path(name).is_file() and sha(Path(name)) == pin for name, pin in pins.items())
        evidence['passed'] = evidence['passed'] and evidence['source_unchanged'] and evidence['seconds'] <= args.timeout
        data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode()
        need(len(data) <= CAP and len(evidence['transport']) <= 12000, 'Whole evidence bound exceeded.')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic() > deadline:
            (args.output/'evidence.json').rename(args.output/'late-evidence.json');raise TimeoutError('Traversal final publication exceeded its deadline; evidence retained.')
    print(json.dumps(dict(passed=evidence['passed'], rpc_count=evidence['rpc_count'], owners=len(evidence['owners']), captures=len(evidence['captures']))))
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
