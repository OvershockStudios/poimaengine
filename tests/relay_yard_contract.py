#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Play the genuine compiled Relay Yard sample through native semantic input.

No renderer, downloads, builds, teleports, gameplay patches, animation-clock
overrides or generated content. All movement/use goes through runtime.step.
Caller supplies the unchanged licensed Kenney sources and a Native AOT artifact.
Failures and every native operation remain in a new owned output directory.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
CONFIG = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb75'
ROUTE = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76'
PLAYER, CAMERA, ACTOR, RIG, TERMINAL = 100, 101, 300, 400, 600
CELL_FIELDS = ('CellOne', 'CellTwo', 'CellThree')
CELL_APPROACHES = ((-6, 7), (-2, 7.4), (4.5, 7))
ACTION_IDS = {'begin': 911, 'welcome.load': 913, 'welcome.refresh': 915,
    'menu': 901, 'close': 930,
    'save': 931, 'load': 932, 'refresh': 933, 'retry': 934,
    'fov.minus': 940, 'fov.plus': 941, 'ui.minus': 942, 'ui.plus': 943,
    'sensitivity.minus': 944, 'sensitivity.plus': 945,
    'gain.minus': 946, 'gain.plus': 947, 'preferences.reset': 948}
DT = 1 / 60


def need(value, message):
    if not value:
        raise AssertionError(message)


def uid(value):
    return f'{value:032x}'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def normalized(value):
    if isinstance(value, dict):
        return {key: normalized(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [normalized(item) for item in value]
    return value


def position(value):
    return value['world_matrix'][12:15]


def present_id(value):
    return isinstance(value, str) and value != uid(0)


def node_id(index):
    return hashlib.sha256(f'poima.instance.v1/{uid(RIG)}/node/{index}'.encode()).hexdigest()[:32]


def wrap(degrees):
    return (degrees + 180) % 360 - 180


def inventory(directory):
    return {path.relative_to(directory).as_posix(): dict(bytes=path.stat().st_size, sha256=sha(path))
            for path in sorted(directory.rglob('*')) if path.is_file()}


def load_launcher():
    source = ROOT / 'examples/relay-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_relay_yard_contract_sample', source)
    need(spec is not None and spec.loader is not None, 'Relay Yard source launcher is absent')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RelayDriver:
    """Semantic-input helpers shared by the separate exported-player verifier.

    game provides guarded rpc/control/entity/module/inspect, mutable session/tick
    and the existing route/config readers. It owns authoritative native state.
    The helpers do not manufacture poses, routes, gameplay values or animation.
    An attached player must be genuinely paused around semantic stepping.
    """
    def __init__(self, game, record):
        self.game, self.record = game, record
        self.last_control_parameters = None

    def values(self):
        return self.game.values()

    def snapshot(self):
        game = self.game
        module, state, route = game.module(), game.inspect(), game.route()
        values = module['module']['values']
        entities = {uid(identifier): game.entity(identifier)
                    for identifier in (PLAYER, CAMERA, ACTOR, RIG, TERMINAL)}
        cells = {field: game.entity(values[field]) for field in CELL_FIELDS if present_id(values[field])}
        nodes = [game.entity(node_id(index)) for index in range(61)]
        return normalized(dict(tick=game.tick, entities=entities, cells=cells, nodes=nodes,
            values=values, backend=module['module']['backend'], gameplay_revision=module['revision'],
            route=route, config=game.visual_config(),
            revisions=dict(component_revision=route['component_revision'],
                **{key: state[key] for key in ('structure_revision', 'ui_revision', 'control_sequence')}),
            ui=game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick, limit=256))))

    def control(self, action):
        # Delegate so a graphical owner's adapter can reconcile/pause its
        # actual player after Begin/Close resume intents before driving ticks.
        start = len(self.game.record['calls'])
        result = self.game.control(action)
        requests = [row for row in self.game.record['calls'][start:]
                    if row.get('method') == 'runtime.ui.activate']
        need(len(requests) == 1, 'Control adapter must issue one compiled action')
        self.last_control_parameters = copy.deepcopy(requests[0]['params'])
        self.record.setdefault('semantic_controls', []).append(dict(
            action=action, parameters=self.last_control_parameters, result=result))
        return result

    def step(self, ticks=1, move=(0, 0), look=(0, 0), use=False):
        need(type(ticks) is int and 1 <= ticks <= 600, 'Semantic step must be 1..600 ticks')
        # One-tick observations make the courier's complete collision path,
        # prior-position rate policy and finite transition evolution reviewable.
        # Waiting/pickup travel before dispatch can use bounded native batches.
        values = self.values()
        if ticks > 1 and (values['Phase'] == 1 or values['Moving'] != 0):
            result = None
            for index in range(ticks):
                result = self.step(1, move=move, look=look if index == 0 else (0, 0),
                                   use=use if index == 0 else False)
            return result
        before_actor, before_rig = self.game.entity(ACTOR), self.game.entity(RIG)
        before_tick = self.game.tick
        state = self.game.inspect()
        parameters = dict(session_id=self.game.session, request_id=uuid.uuid4().hex,
            expected_tick=before_tick, expected_structure_revision=state['structure_revision'],
            ticks=ticks, inputs=[dict(entity=uid(PLAYER), move=list(move), look=list(look), use=bool(use))])
        result = self.game.rpc('runtime.step', parameters)
        self.game.session, self.game.tick = result['current_session_id'], result['current_tick']
        need(not result['runtime_replaced'] and self.game.tick == before_tick + ticks,
             'Movement step unexpectedly replaced runtime or advanced a different tick count')
        actor, rig, current = self.game.entity(ACTOR), self.game.entity(RIG), self.values()
        p = position(actor)
        # Original unrotated cover: x half-extent .5, z half-extent 3;
        # courier radius .42. Exact rectangle closest-distance avoids a false
        # square expansion at corners. Capsule foot/root y is checked separately.
        signed_gap = math.hypot(max(abs(p[0]) - .5, 0), max(abs(p[2]) - 3, 0)) - .42
        need(signed_gap >= -2e-5, 'Native courier penetrated route-blocking cover')
        need(abs(p[1]) <= .025, 'Courier no longer stands on the actual flat floor')
        row = dict(tick=self.game.tick, ticks=ticks, actor_position=p,
                   phase=current['Phase'], collected=current['Collected'], signed_cover_gap=signed_gap,
                   clip=rig['animation']['clip'], clock=rig['animation']['time'],
                   rate=rig['animation']['speed'], transition=rig['animation'].get('transition'),
                   requested_rate=current['RequestedRunRate'], sampled_speed=current['LastHorizontalSpeed'])
        if ticks == 1 and current['Started'] != 0:
            before_position = position(before_actor)
            valid = (values['PreviousPositionValid'] != 0 and
                     int(values['PreviousSampleTick']) + 1 == before_tick)
            displacement = math.hypot(before_position[0] - values['PreviousX'],
                                      before_position[2] - values['PreviousZ']) if valid else 0
            need(abs(current['LastDisplacement'] - displacement) < 2e-7 and
                 abs(current['LastHorizontalSpeed'] - displacement / DT) < 2e-5,
                 'Compiled animation did not observe prior committed controller displacement')
            config = self.game.visual_config()['values']
            nominal = config[uid(10)]
            need(abs(current['RequestedRunRate'] - min(8, displacement / DT / nominal)) < 8e-6,
                 'Run playback request is not derived from committed meters per second')
            new, old = rig['animation'], before_rig['animation']
            wanted_clip = config[uid(3)] if current['Moving'] else config[uid(2)]
            need(new['clip'] == wanted_clip and new['loop'] and new['playing'] and
                 current['AnimationClip'] == wanted_clip and
                 abs(current['AnimationRate'] - new['speed']) < 1e-10,
                 'Movement mode does not select/observe genuine advancing Idle/Run')
            changed = old['clip'] != new['clip']
            expected_clock = math.fmod((0 if changed else old['time']) + new['speed'] * DT,
                                       new['duration'])
            need(abs(new['time'] - expected_clock) < 3e-8,
                 'Native animation clock restarted or advanced with the wrong rate')
            if not changed and old.get('transition') is not None:
                need(new['speed'] == old['speed'] and current['RateChanges'] == values['RateChanges'],
                     'A rate-only correction restarted/replaced an active transition')
            need(math.dist(p, before_position) <= 4 * DT + 2e-5,
                 'Courier moved faster than its authored capsule; possible teleport')
            row.update(prior_committed_position=before_position, expected_displacement=displacement,
                       expected_clock=expected_clock, clock_error=abs(new['time'] - expected_clock))
        self.record.setdefault('courier_observations', []).append(row)
        return result

    def aim_at(self, identifier):
        target = self.game.entity(identifier)
        point = position(target)
        camera, player = self.game.entity(CAMERA), self.game.entity(PLAYER)
        origin = position(camera)
        dx, dy, dz = [point[k] - origin[k] for k in range(3)]
        distance = math.sqrt(dx * dx + dy * dy + dz * dz)
        need(distance > .001, 'Target coincides with the native camera')
        yaw = math.degrees(math.atan2(-dx, -dz))
        pitch = math.degrees(math.asin(dy / distance))
        need(abs(pitch) < 85, 'Target cannot be reached by the authored camera pitch range')
        self.step(look=(wrap(yaw - player['yaw']), pitch - player['pitch']))
        camera = self.game.entity(CAMERA)
        matrix = camera['world_matrix']
        ray = self.game.rpc('runtime.raycast', dict(session_id=self.game.session,
            tick=self.game.tick, origin=position(camera), direction=[-matrix[8], -matrix[9], -matrix[10]],
            distance=3, ignore=[uid(PLAYER)]))
        self.record.setdefault('aim_observations', []).append(dict(
            tick=self.game.tick, target=target['id'], target_position=point, camera=position(camera),
            center_distance=distance, ray=ray))
        return ray

    def drive_to(self, x, z, label, max_iterations=180):
        start = position(self.game.entity(PLAYER))
        observations = []
        for index in range(max_iterations):
            current = self.game.entity(PLAYER)
            p = position(current)
            dx, dz = x - p[0], z - p[2]
            distance = math.hypot(dx, dz)
            if distance <= .1:
                self.record.setdefault('player_paths', []).append(dict(
                    label=label, goal=[x, z], start=start, final=p, observations=observations))
                return p
            direction = (dx / distance, dz / distance)
            matrix = current['world_matrix']
            right, forward = (matrix[0], matrix[2]), (-matrix[8], -matrix[10])
            ticks = max(1, min(12, int(distance / (4 * .8) / DT)))
            strength = min(.8, distance / (4 * ticks * DT))
            move = (strength * sum(a * b for a, b in zip(direction, right)),
                    strength * sum(a * b for a, b in zip(direction, forward)))
            self.step(ticks, move=move)
            after = position(self.game.entity(PLAYER))
            need(math.hypot(after[0] - p[0], after[2] - p[2]) <= 4 * ticks * DT + 2e-5,
                 'Player movement exceeded actual authored capsule speed')
            observations.append(dict(tick=self.game.tick, position=after, ticks=ticks, move=move))
            if index > 8:
                need(math.hypot(after[0] - observations[-9]['position'][0],
                                after[2] - observations[-9]['position'][2]) > .005,
                     'Player semantic movement stalled against collision; inspect path')
        raise AssertionError(('Player waypoint budget exhausted', label, observations[-5:]))

    def follow_waypoints(self, points, label):
        for index, (x, z) in enumerate(points):
            self.drive_to(x, z, label + '-' + str(index))

    def use_target(self, identifier):
        ray = self.aim_at(identifier)
        wanted = uid(identifier) if isinstance(identifier, int) else identifier
        need(ray['hit'] is not None and ray['hit']['entity'] == wanted,
             'Actual native camera ray does not reach the intended target within 3 m')
        before = self.values()
        self.step(use=True)
        after = self.values()
        self.record.setdefault('use_observations', []).append(dict(
            target=wanted, ray=ray, before=before, after=after, tick=self.game.tick))
        return after

    def wait_for_delivery(self, max_ticks=2400):
        for _ in range(max_ticks):
            values = self.values()
            need(values['Phase'] != 3, 'Native courier has no complete path')
            if values['Phase'] == 2:
                self.step(20)  # actual finite Run->Idle transition must finish
                final = self.values()
                need(final['Arrivals'] == 1 and final['Won'] == 0,
                     'Arrival must be unique and cannot win without terminal Use')
                need(self.game.entity(RIG)['animation']['clip'] ==
                     self.game.visual_config()['values'][uid(2)], 'Arrived courier did not return to Idle')
                return final
            self.step()
        raise AssertionError('Courier did not arrive within bounded native ticks')


def saved_inventory(root):
    return inventory(root / 'slot-relay-yard')


def saved_generation(root):
    document = json.loads((root / 'slot-relay-yard/current.json').read_text(encoding='utf-8'))
    payload = document['payload']
    current = payload['current']
    need(current['generation'] == payload['generation'], 'Durable save generation differs')
    path = root / 'slot-relay-yard' / current['file']
    need(path.is_file() and path.stat().st_size == current['bytes'] and sha(path) == current['sha256'],
         'Actual saved payload differs from its published identity')
    return payload['generation']


def qualify(game, driver, launcher, source_directory, output, record):
    authored = game.author(source_directory)
    world_bytes = game.world.read_bytes()
    assets = inventory(Path(str(game.world) + '.assets'))
    record['authored'] = authored
    document = json.loads(world_bytes)
    need(document['entities'][uid(RIG)]['parent'] == uid(ACTOR), 'Imported rig must follow native courier root')
    bindings = document['asset_provenance'][authored['asset']]
    provenance = [game.rpc('asset.provenance.inspect', dict(record=identifier)) for identifier in bindings]
    need(provenance and all(row['metadata']['creator'] == 'Kenney' and
         row['metadata']['license']['identifier'] == 'CC0-1.0' and
         row['metadata']['source'] == launcher.locomotion.SOURCE_URL for row in provenance),
         'Original licensed character provenance is missing')
    record['provenance'] = provenance
    record['immutable_world'] = dict(sha256=hashlib.sha256(world_bytes).hexdigest(), assets=assets)
    game.start();game.load();game.saves(output / 'saves')
    module = game.module()
    need(module['module']['backend'] == 'native_aot' and
         module['module']['schema']['identity'] == 'poima.examples.relay-yard',
         'Verifier must execute the actual selected Relay Yard Native AOT module')
    record['compiled_module'] = module

    # Gating must be real compiled behavior, not a fabricated paused clock.
    player_before = game.entity(PLAYER)
    driver.step(3)
    values = driver.values()
    need(values['Initialized'] == 1 and values['Started'] == 0 and values['Ticks'] == 0 and
         values['Collected'] == 0 and values['Phase'] == 0,
         'Welcome bootstrap advanced gameplay before Begin')
    initial_cells = {field: values[field] for field in CELL_FIELDS}
    need(len(set(initial_cells.values())) == 3 and all(present_id(value) for value in initial_cells.values()),
         'First compiled Tick did not spawn three distinct native cell roots')
    welcome = game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick, limit=256))
    need(welcome['modal'] == uid(910), 'Initial welcome did not own native UI input')
    missing = driver.control('welcome.load')
    need(missing['save_serviced'] and not missing['runtime_replaced'] and
         missing['save_operation']['state'] == 4 and missing['save_operation']['error_code'] != 0,
         'Missing checkpoint must be a failed save outcome, not silent continuation')
    need(not (output / 'saves/slot-relay-yard/current.json').exists(), 'Missing Load created a save')
    record['missing_checkpoint'] = missing
    begun = driver.control('begin')
    need(begun['intent'] == 1 and driver.values()['Started'] == 1 and
         driver.values()['LastSaveState'] == 4, 'Begin did not resume or poll missing-load diagnostic')
    driver.step(16)
    need(game.entity(RIG)['animation']['clip'] == authored['clips']['Idle']['index'] and
         game.entity(RIG)['animation']['playing'], 'Started stationary courier must play original Idle')
    record['checks'].append('Welcome gates game logic; missing Load surfaces a terminal outcome; Begin selects actual Idle')

    # Wrong direction: point above all known scene geometry and query native
    # ray explicitly before Use. No out-of-view gameplay patch is made.
    player = game.entity(PLAYER)
    driver.step(look=(0, 80 - player['pitch']))
    camera = game.entity(CAMERA); matrix = camera['world_matrix']
    wrong_ray = game.rpc('runtime.raycast', dict(session_id=game.session, tick=game.tick,
        origin=position(camera), direction=[-matrix[8], -matrix[9], -matrix[10]], distance=3, ignore=[uid(PLAYER)]))
    need(wrong_ray['hit'] is None, 'Wrong-direction probe accidentally hit geometry')
    driver.step(use=True)
    need(driver.values()['Collected'] == 0, 'Use without a native hit collected a cell')
    far_ray = driver.aim_at(initial_cells['CellThree'])
    need(far_ray['hit'] is None, 'Out-of-range target unexpectedly lies inside use distance')
    driver.step(use=True)
    need(driver.values()['Collected'] == 0, 'Out-of-range Use collected a cell')
    record['negative_use'] = dict(wrong_ray=wrong_ray, far_ray=far_ray, player_start=player_before)

    driver.follow_waypoints(((5.2, 5.2), (5.2, 1.4)), 'early-terminal')
    early = driver.use_target(TERMINAL)
    need(early['LockedUses'] == 1 and early['Won'] == 0 and early['Collected'] == 0,
         'Premature in-range relay Use bypassed cells or courier arrival')
    record['checks'].append('Wrong/no-hit/out-of-range Use cannot collect; real premature terminal Use remains locked')

    driver.follow_waypoints(((5.2, 5.2), CELL_APPROACHES[0]), 'first-cell')
    picked = driver.use_target(initial_cells['CellOne'])
    need(picked['Collected'] == 1 and not present_id(picked['CellOne']), 'First native ray failed to collect cell')
    game.rpc('runtime.entity', dict(session_id=game.session, id=initial_cells['CellOne']), error=-32004)
    opened = driver.control('menu')
    need(opened['intent'] == 2, 'Compiled menu must request native Pause')
    save_one = driver.control('save')
    need(save_one['save_operation']['state'] == 3 and saved_generation(output / 'saves') == 1,
         'First compiled checkpoint did not commit actual generation one')
    driver.control('refresh')
    save_two = driver.control('save')
    save_parameters = copy.deepcopy(driver.last_control_parameters)
    need(save_two['save_operation']['state'] == 3 and saved_generation(output / 'saves') == 2,
         'Repeated compiled Save with omitted expected generation did not advance to generation two')
    checkpoint = driver.snapshot()
    durable = saved_inventory(output / 'saves')
    record['durable_checkpoint_pin'] = durable
    repeated = game.rpc('runtime.ui.activate', save_parameters)
    need(repeated == dict(save_two, replayed=True) and driver.snapshot() == checkpoint and
         saved_inventory(output / 'saves') == durable,
         'Exact compiled Save retry repeated callback or durable write')
    game.rpc('runtime.ui.activate', dict(save_parameters, id=uid(ACTION_IDS['load'])), error=-32010)
    driver.control('refresh')
    stale = copy.deepcopy(driver.last_control_parameters);stale['request_id'] = uuid.uuid4().hex
    driver.control('refresh')
    before_stale = driver.snapshot()
    game.rpc('runtime.ui.activate', stale, error=-32009)
    need(driver.snapshot() == before_stale, 'Stale control guard changed native/compiled state')
    driver.control('close')
    driver.drive_to(*CELL_APPROACHES[1], 'second-before-load')
    second = driver.use_target(initial_cells['CellTwo'])
    need(second['Collected'] == 2 and not present_id(second['CellTwo']), 'Second pickup failed')
    driver.control('menu')
    loaded = driver.control('load')
    load_parameters = copy.deepcopy(driver.last_control_parameters)
    need(loaded['runtime_replaced'] and loaded['save_operation']['state'] == 3 and
         loaded['save_operation']['generation'] == 2 and driver.snapshot() == checkpoint,
         'Compiled Load did not restore exact partial player/courier/rig/UI/component/allocated-cell checkpoint')
    game.rpc('runtime.entity', dict(session_id=game.session, id=initial_cells['CellOne']), error=-32004)
    need(game.entity(initial_cells['CellTwo'])['id'] == initial_cells['CellTwo'],
         'Load did not restore the collected-after-save cell under its original generated ID')
    retry = game.rpc('runtime.ui.activate', load_parameters)
    need(retry == dict(loaded, replayed=True) and driver.snapshot() == checkpoint and
         saved_inventory(output / 'saves') == durable, 'Exact Load retry replaced runtime again')
    old_step = dict(session_id=loaded['session_id'], request_id=uuid.uuid4().hex,
                    expected_tick=loaded['tick'], expected_structure_revision=checkpoint['revisions']['structure_revision'], ticks=1)
    game.rpc('runtime.step', old_step, error=-32030)
    need(driver.snapshot() == checkpoint, 'Old-session input advanced restored native state')
    record['checkpoint'] = dict(saved=checkpoint, save_one=save_one, save_two=save_two,
        loaded=loaded, load_retry=retry, durable_inventory=durable)
    record['checks'].append('Repeated compiled saves, inert retries, stale guards and exact partial checkpoint restoration')

    driver.control('refresh')
    need(driver.values()['SavePending'] == 0 and driver.values()['SaveNotice'] == 6 and
         int(driver.values()['LastSaveGeneration']) == 2, 'Fresh restore epoch did not reconcile saved pending ticket')
    driver.control('close')
    driver.drive_to(*CELL_APPROACHES[1], 'second-after-load')
    driver.use_target(initial_cells['CellTwo'])
    driver.drive_to(*CELL_APPROACHES[2], 'third-cell')
    last = driver.use_target(initial_cells['CellThree'])
    need(last['Collected'] == 3 and all(not present_id(last[field]) for field in CELL_FIELDS) and
         last['Phase'] == 1 and last['Won'] == 0, 'Three true pickups did not dispatch courier or won prematurely')
    for identifier in initial_cells.values():
        game.rpc('runtime.entity', dict(session_id=game.session, id=identifier), error=-32004)
    travelling = driver.snapshot()
    route = travelling['route']['values'][uid(1)]
    need(len(route) >= 6 and travelling['values']['Plans'] == 1 and
         travelling['values']['LastCornerCount'] == len(route) // 3,
         'Courier did not query and persist a native path around cover')
    delivered = driver.wait_for_delivery()
    need(delivered['AnimationTransitions'] >= 3 and delivered['RateChanges'] > 0 and
         any(row['clip'] == authored['clips']['Run']['index'] and row['sampled_speed'] > .1
             for row in record['courier_observations']),
         'Courier lacks actual Idle/Run switches and displacement-derived playback corrections')
    driver.follow_waypoints(((5.2, 5.2), (5.2, 1.4)), 'final-terminal')
    won = driver.use_target(TERMINAL)
    need(won['Won'] == 1 and won['Collected'] == 3 and won['Arrivals'] == 1,
         'Final in-range relay ray did not complete after native courier arrival')
    record['completed'] = driver.snapshot()
    record['checks'].append('All three roots truly despawn; native collision/navigation/IdleRun courier arrives; terminal ray alone completes')
    need(game.world.read_bytes() == world_bytes and inventory(Path(str(game.world) + '.assets')) == assets and
         saved_inventory(output / 'saves') == durable,
         'Gameplay, checkpoints or negative probes rewrote authored/cooked/durable content')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'manifest', 'descriptor', 'source-directory', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=900)
    args = parser.parse_args()
    need(math.isfinite(args.timeout) and 60 <= args.timeout <= 1800, 'Timeout must be finite 60..1800 seconds')
    inputs = {name: getattr(args, name).resolve(strict=True) for name in ('binary', 'manifest', 'descriptor')}
    for name, path in inputs.items():
        need(path.is_file(), 'Supply a regular ' + name)
        setattr(args, name, path)
    args.source_directory = args.source_directory.resolve(strict=True)
    launcher = load_launcher()
    originals = {name: args.source_directory / name for name in launcher.locomotion.SOURCE_SHA}
    need(all(path.is_file() and sha(path) == launcher.locomotion.SOURCE_SHA[name]
             for name, path in originals.items()), 'Supply unchanged pinned original Kenney base/Idle/Run/license')
    descriptor = json.loads(args.descriptor.read_text(encoding='utf-8'))
    need(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2,
         'Genuine Native AOT descriptor must inventory actual artifact files')
    library = False
    for index, member in enumerate(descriptor['files']):
        relative = Path(member['path'])
        need(not relative.is_absolute() and '..' not in relative.parts, 'Unsafe native artifact member')
        path = args.descriptor.parent / relative
        need(path.is_file() and path.stat().st_size == member['size'] and sha(path) == member['sha256'],
             'Actual native artifact inventory differs')
        inputs['artifact_' + str(index)] = path
        library |= member['path'] == descriptor['library'] and member['role'] == 'library'
    need(library, 'Native artifact does not inventory its compiled library')
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    need(manifest['format'] == 'poima.components' and {row['id'] for row in manifest['schemas']} == {CONFIG, ROUTE},
         'Compiled Relay component schemas must retain original locomotion config/route IDs')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = [Path(__file__), ROOT / 'examples/relay-yard/run.py',
        ROOT / 'examples/relay-yard/RelayYardGame.cs', ROOT / 'examples/relay-yard/Poima.RelayYardGame.csproj',
        ROOT / 'examples/locomotion-yard/run.py', ROOT / 'examples/locomotion-yard/LocomotionYardGame.cs',
        *sorted((ROOT / 'tools/python/poima_client').glob('*.py'))]
    record = dict(passed=False, format='poima.relay-yard-contract', version=1,
        calls=[], owners=[], checks=[], cleanup_errors=[],
        inputs={name: sha(path) for name, path in inputs.items()},
        source_pins={path.relative_to(ROOT).as_posix(): sha(path) for path in sources},
        original_source_pins={name: sha(path) for name, path in originals.items()},
        limitations=['Compiled native semantic inputs; no renderer or physical input',
            'Original FBX decoding/retargeting uses native importer; not an independent FBX parser',
            'Controller-derived in-place animation; no stride fitting, IK or foot locking',
            'One small game; no clean-machine, performance, AA/AAA or full alpha claim'])
    game = None
    started = time.monotonic()
    try:
        def native(path):
            text = str(Path(path).resolve())
            return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip() \
                if args.windows_interop and os.name != 'nt' else text
        game = launcher.OwnedRelay(args, output / 'world.json', 'relay-contract', record,
                                   started + args.timeout, native, manifest)
        driver = RelayDriver(game, record)
        qualify(game, driver, launcher, args.source_directory, output, record)
        game.rpc('runtime.stop', dict(session_id=game.session))
        game.close()
        transport = game.client.transport
        record['owners'][-1].update(process_id=transport.process_id, stderr=transport.stderr_tail,
                                    stderr_truncated=transport.stderr_truncated)
        need(record['owners'][-1]['exit_code'] == 0 and not transport.stderr_tail and
             not transport.stderr_truncated, 'Native owner did not close cleanly')
        game = None
        record['passed'] = True
    except BaseException as exc:
        record['failure'] = repr(exc)
        record['traceback'] = traceback.format_exc()
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        if game is not None:
            try:
                game.close()
                transport = game.client.transport
                record['owners'][-1].update(process_id=transport.process_id,
                    stderr=transport.stderr_tail, stderr_truncated=transport.stderr_truncated)
            except BaseException as exc:
                record['cleanup_errors'].append(repr(exc))
        try:
            immutable = dict(inputs=all(sha(path) == record['inputs'][name] for name, path in inputs.items()),
                sources=all(sha(path) == record['source_pins'][path.relative_to(ROOT).as_posix()] for path in sources),
                originals=all(sha(path) == record['original_source_pins'][name] for name, path in originals.items()))
            if 'immutable_world' in record:
                pins = record['immutable_world']
                immutable['authored'] = sha(output / 'world.json') == pins['sha256']
                immutable['cooked'] = inventory(output / 'world.json.assets') == pins['assets']
            if 'durable_checkpoint_pin' in record:
                immutable['durable_checkpoint'] = saved_inventory(output / 'saves') == record['durable_checkpoint_pin']
            record['immutable_inputs_on_exit'] = immutable
            need(all(immutable.values()), 'Qualification input/content changed during run')
        except BaseException as exc:
            record['cleanup_errors'].append('Final immutable-input check: ' + repr(exc))
        if record['cleanup_errors']:
            record['passed'] = False
        record['elapsed_seconds'] = time.monotonic() - started
        record['rpc_count'] = len(record['calls'])
        (output / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        if not record['passed']:
            print('Relay contract evidence retained: ' + str(output), file=sys.stderr)
        if record['cleanup_errors'] and not active_failure:
            raise AssertionError(record['cleanup_errors'])
    print(json.dumps(dict(passed=record['passed'], checks=len(record['checks']), rpc_count=record['rpc_count'])))


if __name__ == '__main__':
    main()
