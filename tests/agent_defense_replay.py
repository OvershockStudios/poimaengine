#!/usr/bin/env python3
"""Replay the recorded agent-authored Relay Defense fixture independently.

Uses reflected state/native snapshots and fresh controller inputs, never an
agent-authored test route. No provider, source repairs, live field patches,
teleports or rendering. Run Python on the engine's native operating system.
Raw local evidence contains paths and RPCs; public-summary.json is allowlisted.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError

ZERO = '0' * 32
MAX_CALLS = 8000


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def position(entity):
    return entity['world_matrix'][12:15]


def distance(a, b):
    return math.sqrt(sum((x-y)**2 for x, y in zip(a, b)))


class Game:
    def __init__(self, client, settings, manifest, evidence, name, deadline):
        self.client, self.settings, self.m, self.e, self.name = client, settings, manifest, evidence, name
        self.deadline = deadline
        self.session, self.tick = None, 0
        self.seen, self.wave_ids, self.last_shot = set(), {}, None

    def rpc(self, method, params=None):
        require(len(self.e['calls']) < MAX_CALLS, 'Independent RPC budget exceeded')
        remaining = self.deadline-time.monotonic()
        require(remaining > 0, 'Overall replay time budget exceeded')
        record = dict(process=self.name, method=method, params=copy.deepcopy(params or {}))
        self.e['calls'].append(record)
        try:
            result = self.client.call(method, params, timeout=min(30, remaining))
            record['result'] = copy.deepcopy(result)
            return result
        except BaseException as error:
            record['error'] = dict(kind=type(error).__name__, message=str(error), code=getattr(error, 'code', None))
            raise

    def fresh(self):
        current = self.rpc('runtime.status')
        if current['active']:
            self.rpc('runtime.stop', dict(session_id=current['session_id']))
        revision = self.rpc('world.inspect')['revision']
        self.session, self.tick = uuid.uuid4().hex, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=revision))
        loaded = self.rpc('runtime.gameplay.load', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=0, expected_revision=0, hostfxr=self.settings['hostfxr'], bridge=self.settings['bridge'],
            assembly=self.settings['assembly'], type=self.m['type']))
        require(loaded['module']['assembly_sha256'] == self.e['hashes']['assembly'], 'Loaded game assembly hash differs')
        initial = self.values()
        require(self.n(initial, 'kills') == self.n(initial, 'breaches') == self.n(initial, 'won') == self.n(initial, 'lost') == 0,
                'Fresh game progress is not initialized')
        require(self.n(initial, 'ammo') == 6 and self.n(initial, 'health') == 3, 'Fresh ammo/reactor health differs')
        self.step(1)

    def inspect(self):
        result = self.rpc('runtime.inspect', dict(session_id=self.session))
        require(result['tick'] == self.tick, 'Unexpected independent simulation advance')
        return result

    def values(self):
        result = self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))
        require(result['module']['assembly_sha256'] == self.e['hashes']['assembly'], 'Runtime gameplay artifact changed')
        return result['module']['values']

    def n(self, values, key):
        value = values[self.m['field_map'][key]]
        require(type(value) is int or (isinstance(value, str) and value.lstrip('-').isdigit()), 'Invalid scalar '+key)
        return int(value)

    def enemies(self, values=None):
        values = self.values() if values is None else values
        ids = [values[field] for field in self.m['enemy_fields'] if values[field] != ZERO]
        require(len(ids) == len(set(ids)) <= 3, 'Drone handles are duplicated or exceed wave budget')
        wave = self.n(values, 'wave')
        for identifier in ids:
            body = self.entity(identifier)
            require(body['motion'] == 'kinematic' and body['has_body'], 'Live drone is not a native kinematic body')
            if identifier not in self.seen:
                self.seen.add(identifier)
                self.wave_ids.setdefault(wave, set()).add(identifier)
        return ids

    def entity(self, identifier):
        state = self.inspect()
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick,
            structure_revision=state['structure_revision'], id=identifier))

    def gone(self, identifier):
        try:
            self.entity(identifier)
        except RpcError as error:
            require(error.code == -32004, 'Unexpected disappearance-query error')
            return True
        return False

    def step(self, ticks, **input):
        require(1 <= ticks <= 600, 'Unsafe verifier tick batch')
        state = self.inspect()
        if input:
            input['entity'] = self.m['player_id']
        previous = self.tick
        result = self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_structure_revision=state['structure_revision'], ticks=ticks,
            inputs=[input] if input else []))
        require(not result.get('runtime_replaced'), 'Unexpected replacement during ordinary input')
        self.session, self.tick = result['current_session_id'], result['current_tick']
        require(self.tick == previous+ticks, 'Step did not advance exact requested ticks')
        return result

    def ui(self, name):
        state = self.inspect()
        game = self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))
        result = self.rpc('runtime.ui.activate', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'], expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], id=self.m['ui'][name]))
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def hud(self):
        result = self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick))
        require(result['next_after'] is None, 'UI exceeds one bounded qualification page')
        hud = next((row for row in result['elements'] if row['id'] == self.m['ui']['hud']), None)
        require(hud is not None and isinstance(hud['text'], str) and hud['text'], 'Missing/empty native HUD')
        values = self.values()
        live = sum(values[field] != ZERO for field in self.m['enemy_fields'])
        status = ('VICTORY' if self.n(values, 'won') else 'REACTOR LOST' if self.n(values, 'lost')
                  else 'PAUSED' if self.n(values, 'paused') else 'DEFENDING')
        expected = (f"RELAY DEFENSE | Wave {self.n(values, 'wave')}/3 | Kills {self.n(values, 'kills')}/9 | "
                    f"Ammo {self.n(values, 'ammo')}/6 | Reactor {self.n(values, 'health')}/3 | Drones {live} | {status}")
        require(hud['text'] == expected, 'Logical HUD does not match native progress/ammo/health/status')
        return result

    def ray(self):
        camera = self.entity(self.m['camera_id'])
        matrix = camera['world_matrix']
        return self.rpc('runtime.raycast', dict(session_id=self.session, tick=self.tick, origin=position(camera),
            direction=[-matrix[8], -matrix[9], -matrix[10]], distance=120,
            ignore=[self.m['player_id'], self.m['camera_id']]))['hit']

    def look(self, yaw, pitch):
        player = self.entity(self.m['player_id'])
        delta_yaw = (yaw-player['yaw']+180) % 360-180
        self.step(1, look=[delta_yaw, max(-180, min(180, pitch-player['pitch']))])

    def aim(self, identifier):
        # Recompute after a one-tick correction; target motion is native, not a
        # precomputed author route. C# fires before the next physics advance.
        for _ in range(2):
            camera, drone = self.entity(self.m['camera_id']), self.entity(identifier)
            dx, dy, dz = [b-a for a, b in zip(position(camera), position(drone))]
            self.look(math.degrees(math.atan2(-dx, -dz)),
                      math.degrees(math.atan2(dy, math.hypot(dx, dz))))
        return self.ray()

    def ready(self):
        if self.last_shot is not None:
            remaining = 12-(self.tick-self.last_shot)
            if remaining > 0:
                self.step(remaining)

    def drone_health(self, values):
        result = {}
        for field in self.m['enemy_fields']:
            identifier = values[field]
            if identifier != ZERO:
                require(field+'Hp' in values, 'This Relay fixture must expose its named native drone health fields')
                result[identifier] = int(values[field+'Hp'])
        return result

    def shoot(self, accepted, expected_kill=0, expected_target=None):
        before = self.values()
        old_health = self.drone_health(before)
        self.step(1, use=True)
        after = self.values()
        new_health = self.drone_health(after)
        require(self.n(after, 'shots') == self.n(before, 'shots')+int(accepted), 'Shot acceptance differs')
        require(self.n(after, 'ammo') == self.n(before, 'ammo')-int(accepted), 'Ammo consumption differs')
        require(self.n(after, 'kills') == self.n(before, 'kills')+expected_kill, 'Kill count differs')
        require(self.n(after, 'breaches') == self.n(before, 'breaches'), 'Breach confounded shot gate')
        for identifier, hp in old_health.items():
            if identifier == expected_target:
                if expected_kill:
                    require(identifier not in new_health, 'Eliminated drone still has live health')
                else:
                    require(new_health.get(identifier) == hp-1, 'Camera hit did not reduce the target by exactly one HP')
            else:
                require(new_health.get(identifier) == hp, 'Miss/blocked shot changed drone health')
        if accepted:
            self.last_shot = self.tick
        return after

    def miss_view(self):
        player = self.entity(self.m['player_id'])
        for yaw, pitch in ((player['yaw'], 80), (player['yaw']+180, 80), (player['yaw']+90, 70),
                           (player['yaw']-90, 70)):
            self.look(yaw, pitch)
            if self.ray() is None:
                return
        raise AssertionError('No genuine camera ray miss found within bounded look search')

    def reveal(self, identifier):
        for side in (1, -1):
            for _ in range(14):
                hit = self.aim(identifier)
                if hit is not None and hit['entity'] == identifier:
                    return
                self.step(6, move=[side, 0])
                require(self.n(self.values(), 'lost') == 0, 'Game lost while revealing occluded drone')
        raise AssertionError('Controller sidestepping did not reveal live drone')

    def reload(self):
        before = self.tick
        self.ui('reload')
        require(self.tick == before and self.n(self.values(), 'ammo') == 6, 'Compiled reload does not refill at unchanged tick')
        self.hud()

    def kill(self, identifier, test_cooldown=False):
        for hit_number in range(2):
            if self.n(self.values(), 'ammo') == 0:
                self.reload()
            self.ready()
            self.reveal(identifier)
            hit = self.ray()
            require(hit is not None and hit['entity'] == identifier, 'Kill shot lacks a closest native drone ray hit')
            before = self.values()
            self.shoot(True, expected_kill=1 if hit_number == 1 else 0, expected_target=identifier)
            if hit_number == 0:
                require(not self.gone(identifier), 'Drone died after only one hit')
                if test_cooldown:
                    self.shoot(False)
            else:
                require(self.gone(identifier), 'Killed drone remains in native membership')
                require(identifier not in self.enemies(), 'Dead drone handle was not cleared')
                require(self.n(before, 'won') == 0, 'Win occurred before final hit')

    def finish(self):
        start_tick = self.tick
        for _ in range(24):
            values = self.values()
            require(self.n(values, 'lost') == 0, 'Independent playthrough lost')
            ids = self.enemies(values)
            if self.n(values, 'won'):
                require(self.n(values, 'kills') == 9 and self.n(values, 'breaches') == 0, 'Victory is not exactly nine clean kills')
                require(not ids and len(self.seen) == 9 and set(self.wave_ids) == {1, 2, 3}, 'Victory spawn/wave accounting differs')
                require(all(len(ids) == 3 for ids in self.wave_ids.values()), 'A wave does not contain exactly three unique drones')
                terminal = copy.deepcopy(values)
                terminal_tick = self.tick
                self.step(24)
                later = self.values()
                for key in ('kills', 'wave', 'health', 'won', 'lost', 'breaches', 'shots', 'ammo'):
                    require(self.n(later, key) == self.n(terminal, key), 'Win state kept progressing: '+key)
                return dict(tick=self.tick, observed_terminal_tick=terminal_tick, values=later, ui=self.hud(),
                            waves={str(k): sorted(v) for k, v in self.wave_ids.items()})
            if not ids:
                self.step(1)
            else:
                reactor = position(self.entity(self.m['reactor_id']))
                target = min(ids, key=lambda i: distance(position(self.entity(i)), reactor))
                self.kill(target)
            require(self.tick-start_tick <= 1500, 'Bounded win route exceeded 1500 ticks')
        raise AssertionError('Bounded win loop exhausted')

    def snapshot(self):
        state, values = self.inspect(), self.values()
        ids = self.enemies(values)
        entities = {i: self.entity(i) for i in [self.m['player_id'], self.m['camera_id'], *ids]}
        return dict(tick=self.tick, state=state, values=values, entities=entities, ui=self.hud())

    def restore_equals(self, saved):
        observed = self.snapshot()
        require(observed['tick'] == saved['tick'], 'Checkpoint tick differs')
        require(observed['values'] == saved['values'], 'Checkpoint reflected state differs before any resumed callback')
        require(set(observed['entities']) == set(saved['entities']), 'Checkpoint drone identities differ')
        for identifier, entity in saved['entities'].items():
            restored = observed['entities'][identifier]
            for key in ('world_matrix', 'motion', 'kinematic_target', 'motion_remaining_ticks', 'yaw', 'pitch'):
                require(restored[key] == entity[key], 'Checkpoint native '+key+' differs for '+identifier)
        for key in ('ui_revision', 'control_sequence', 'structure_revision'):
            require(observed['state'][key] == saved['state'][key], 'Checkpoint runtime counter differs: '+key)
        require(observed['ui']['elements'] == saved['ui']['elements'], 'Checkpoint complete logical UI differs')
        return observed


def unattended(game):
    game.fresh()
    values = game.values()
    ids = game.enemies(values)
    require(len(ids) == 3 and game.n(values, 'wave') == 1, 'Initial wave is not three drones')
    reactor = position(game.entity(game.m['reactor_id']))
    initial = {i: game.entity(i) for i in ids}
    game.step(30)
    for identifier, before in initial.items():
        after = game.entity(identifier)
        require(distance(position(after), reactor) < distance(position(before), reactor), 'Drone does not approach reactor')
        # A one-tick MoveKinematic target legitimately completes before this
        # observation; changed native poses, not a remaining timer, prove motion.
    history, previous = [], game.values()
    for _ in range(120):
        if game.n(previous, 'lost'):
            break
        before_ids = game.enemies(previous)
        game.step(min(15, 1800-game.tick))
        current = game.values()
        if game.n(current, 'breaches') > game.n(previous, 'breaches'):
            disappeared = [i for i in before_ids if game.gone(i)]
            require(len(disappeared) == game.n(current, 'breaches')-game.n(previous, 'breaches'),
                    'Breach count differs from removed native drones')
            history.append(dict(tick=game.tick, breaches=game.n(current, 'breaches'), removed=disappeared))
        require(game.n(current, 'health') == 3-game.n(current, 'breaches'), 'Reactor health does not match breaches')
        require(game.n(current, 'kills') == game.n(current, 'won') == 0, 'Unattended run fabricated kills/win')
        previous = current
        if game.tick >= 1800:
            break
    require(game.n(previous, 'lost') == 1 and game.n(previous, 'breaches') == 3 and game.n(previous, 'health') == 0,
            'Unattended three-breach loss not reached by 1800 ticks')
    require(game.n(previous, 'shots') == 0, 'Unattended run fired shots')
    terminal_tick = game.tick
    game.step(24)
    later = game.values()
    for key in ('kills', 'wave', 'health', 'won', 'lost', 'breaches', 'shots'):
        require(game.n(later, key) == game.n(previous, key), 'Loss state kept progressing')
    return dict(tick=game.tick, observed_terminal_tick=terminal_tick, values=later,
                breaches=history, ui=game.hud(), initial_motion=initial)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'world', 'manifest', 'assembly', 'hostfxr', 'bridge', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    parser.add_argument('--timeout-seconds', type=float, default=300,
                        help='Overall gameplay/RPC budget, 30–900 seconds (default: 300); cleanup is always attempted separately.')
    args = parser.parse_args()
    if not math.isfinite(args.timeout_seconds) or not 30 <= args.timeout_seconds <= 900:
        parser.error('--timeout-seconds must be finite and from 30 to 900')
    paths = {name:getattr(args,name).resolve() for name in
             ('engine','world','manifest','assembly','hostfxr','bridge','output')}
    for name in ('engine','world','manifest','assembly','hostfxr','bridge'):
        if not paths[name].is_file():
            parser.error('--'+name+' must name an existing file using native OS paths')
    try:
        manifest = json.loads(paths['manifest'].read_text(encoding='utf-8'))
        require(manifest['magazine_size'] == 6 and manifest['cooldown_ticks'] == 12
                and manifest['enemy_hit_points'] == 2 and manifest['save_slot'] == 'relay-checkpoint',
                'Manifest game constants differ')
        require(set(manifest['field_map']) == {'ammo','kills','wave','health','won','lost','paused','shots','breaches','cooldown'},
                'Manifest field mapping differs')
        require(len(manifest['enemy_fields']) == 3 and len(set(manifest['enemy_fields'])) == 3,
                'Manifest must define three distinct reflected drone handles')
        for key in ('player_id','camera_id','reactor_id','cover_id'):
            require(isinstance(manifest[key],str) and len(manifest[key]) == 32
                    and all(c in '0123456789abcdef' for c in manifest[key]), 'Invalid manifest '+key)
    except (KeyError, TypeError, ValueError, AssertionError) as error:
        parser.error('Invalid Relay Defense manifest: '+str(error))
    out = paths['output']
    try:
        out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('--output must be a new directory')
    settings = dict(binary=str(paths['engine']), hostfxr=str(paths['hostfxr']),
                    bridge=str(paths['bridge']), assembly=str(paths['assembly']), world=str(out/'world.json'))
    evidence = dict(passed=False, calls=[], checks=[], processes=[], cleanup_errors=[],
                    scope='Independent headless native controller gameplay; no agent route, field patches, teleports, graphics or physical-device qualification.',
                    recorded_agent_authoring=True, live_agent_session=False, gpu_qualified=False,
                    physical_input_qualified=False, started_unix=time.time(),
                    timeout_seconds=args.timeout_seconds,
                    hashes={name:digest(paths[name]) for name in ('engine','world','manifest','assembly','hostfxr','bridge')})
    evidence['hashes']['agent_defense_replay.py'] = digest(__file__)
    clients = []
    started = time.monotonic()
    deadline = started+args.timeout_seconds
    try:
        shutil.copyfile(paths['world'], settings['world'])
        assets = Path(str(paths['world'])+'.assets')
        if assets.exists():
            shutil.copytree(assets, Path(settings['world']+'.assets'))
        saves = out/'saves'
        saves.mkdir()
        require(time.monotonic() < deadline, 'Overall replay time budget exceeded before startup')
        client = WorldClient.open(settings['binary'], settings['world'], close_timeout=10)
        clients.append(client)
        game = Game(client, settings, manifest, evidence, 'standalone-native-process', deadline)
        evidence['loss'] = unattended(game)
        game.fresh()
        game.seen.clear(); game.wave_ids.clear(); game.last_shot = None
        initial_ids = game.enemies()
        initial_player = position(game.entity(manifest['player_id']))
        occluded = None
        for identifier in initial_ids:
            hit = game.aim(identifier)
            if hit is not None and hit['entity'] == manifest['cover_id']:
                occluded = identifier
                break
        require(occluded is not None, 'No initial drone is occluded by authored cover')
        game.ready()
        game.shoot(True)
        require(not game.gone(occluded), 'Cover-blocked shot removed drone')
        game.shoot(False)
        evidence['checks'].extend(['Native cover closest-hit blocks drone damage.', 'Immediate cooldown gate spends no ammunition.'])
        game.miss_view()
        while game.n(game.values(), 'ammo'):
            game.ready()
            require(game.ray() is None, 'Miss direction no longer misses')
            game.shoot(True)
        game.ready()
        game.reveal(occluded)
        require(distance(initial_player, position(game.entity(manifest['player_id']))) > .2,
                'Occluded drone was not revealed through actual controller movement')
        game.shoot(False)
        require(not game.gone(occluded), 'Empty magazine damaged drone')
        evidence['checks'].extend(['Accepted camera misses consume finite ammunition.', 'Controller movement reveals cover-occluded target.', 'Empty magazine refuses a visible drone shot.'])
        game.reload()
        game.kill(occluded, test_cooldown=True)
        require(game.n(game.values(), 'kills') == 1 and len(game.enemies()) == 2, 'Checkpoint is not one kill mid-wave')
        tick = game.tick
        paused = game.ui('pause')
        require(paused['intent'] == 2 and game.tick == tick and game.n(game.values(), 'paused') == 1,
                'Compiled pause does not return Pause intent/state at unchanged tick')
        resumed = game.ui('resume')
        require(resumed['intent'] == 1 and game.tick == tick and game.n(game.values(), 'paused') == 0,
                'Compiled resume does not return Resume intent/state at unchanged tick')
        generation = game.rpc('save.status')['generation']
        game.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=generation, root=str(saves)))
        saved = game.ui('save')
        require(saved.get('save_serviced') and saved['save_operation']['state'] == 3
                and saved['save_operation']['kind'] == 1, 'Compiled checkpoint Save failed')
        checkpoint = game.snapshot()
        require(len(game.enemies()) == 2, 'Saved checkpoint lacks two surviving native kinematic drones')
        slot = game.rpc('save.inspect', dict(slot=manifest['save_slot']))
        require(slot['generation'] == 1 and slot['selected'] is not None, 'New independent save generation not verified')
        evidence['checkpoint'] = checkpoint
        evidence['save'] = saved
        observed_ids, observed_waves = copy.deepcopy(game.seen), copy.deepcopy(game.wave_ids)
        evidence['win'] = game.finish()
        old_session = game.session
        loaded = game.ui('load')
        require(loaded.get('runtime_replaced') and loaded['save_operation']['state'] == 3
                and loaded['save_operation']['kind'] == 2 and game.session != old_session, 'Compiled Load did not replace runtime')
        evidence['compiled_restore'] = game.restore_equals(checkpoint)
        game.seen, game.wave_ids = copy.deepcopy(observed_ids), copy.deepcopy(observed_waves)
        game.last_shot = checkpoint['tick']
        evidence['win_after_compiled_load'] = game.finish()

        directory = out/'fresh-process'
        directory.mkdir()
        copy_world = directory/'world.json'
        shutil.copyfile(settings['world'], copy_world)
        original_assets = Path(settings['world']+'.assets')
        if original_assets.exists():
            shutil.copytree(original_assets, Path(str(copy_world)+'.assets'))
        require(time.monotonic() < deadline, 'Overall replay time budget exceeded before continuation startup')
        fresh_client = WorldClient.open(settings['binary'], copy_world, close_timeout=10)
        clients.append(fresh_client)
        fresh = Game(fresh_client, settings, manifest, evidence, 'fresh-native-process', deadline)
        require(not fresh.rpc('runtime.status')['active'], 'Fresh native process already has runtime')
        configuration = fresh.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(saves)))
        revision = fresh.rpc('world.inspect')['revision']
        selection = {key:settings[key] for key in ('hostfxr','bridge','assembly')}
        selection['type'] = manifest['type']
        restored = fresh.rpc('save.load', dict(request_id=uuid.uuid4().hex,
            configuration_generation=configuration['generation'], slot=manifest['save_slot'], expected_generation=1,
            revision=revision, expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
            new_session_id=uuid.uuid4().hex, gameplay=selection))
        fresh.session, fresh.tick = restored['session_id'], restored['tick']
        fresh.seen, fresh.wave_ids = copy.deepcopy(observed_ids), copy.deepcopy(observed_waves)
        fresh.last_shot = checkpoint['tick']
        evidence['fresh_process_restore'] = fresh.restore_equals(checkpoint)
        evidence['fresh_process_win'] = fresh.finish()
        require(digest(settings['world']) == evidence['hashes']['world'], 'Independent play changed authored world bytes')
        require(digest(settings['assembly']) == evidence['hashes']['assembly'], 'Compiled game artifact changed during verification')
        require(digest(copy_world) == evidence['hashes']['world'], 'Fresh-process play changed authored world bytes')
        require(all(digest(paths[name]) == evidence['hashes'][name] for name in ('world','manifest','assembly')),
                'Input fixture or assembly changed during verification')
        evidence['checks'].extend(['Three-breach unattended loss.', 'Exactly three waves of three two-hit native drones produce nine-kill win.',
            'Compiled Reload/Pause/Resume/Save/Load handlers exercised.',
            'Mid-wave checkpoint restores complete reflected state, logical UI, native identities/poses and motion.',
            'Fresh native process restores the same checkpoint and completes newly computed controller play.'])
        evidence['work_complete'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        for client in reversed(clients):
            transport = client.transport
            try:
                client.close()
            except BaseException:
                evidence['cleanup_errors'].append(traceback.format_exc())
            evidence['processes'].append(dict(pid=transport.process_id, closed=transport.closed, returncode=transport.returncode))
        evidence['finished_unix'] = time.time()
        evidence['elapsed_seconds'] = time.monotonic()-started
        evidence['passed'] = bool(evidence.get('work_complete') and not evidence.get('error')
            and not evidence['cleanup_errors'] and all(p['closed'] and p['returncode'] == 0 for p in evidence['processes']))
        (out/'raw-evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
        summary = dict(format='poima.agent-defense-replay.v1', passed=evidence['passed'],
            recorded_agent_authoring=True, live_agent_session=False, native_calls=len(evidence['calls']),
            elapsed_seconds=evidence['elapsed_seconds'], timeout_seconds=args.timeout_seconds,
            hashes=evidence['hashes'], checks=evidence['checks'],
            cleanup=dict(clean=not evidence['cleanup_errors'] and len(evidence['processes']) == 2
                         and all(p['closed'] and p['returncode'] == 0 for p in evidence['processes']),
                         owned_processes=len(evidence['processes']),
                         exit_codes=[p['returncode'] for p in evidence['processes']]),
            limits=['Recorded agent-authored fixture replay, not fresh provider authoring.',
                    'Logical HUD and synthetic controller input; no graphics or physical input qualification.',
                    'One bounded primitive hitscan game; no production, scale, universal determinism or full API stability claim.',
                    'Qualified platforms are described by separately recorded evidence; runner availability is not qualification.',
                    'Supplied DLL source provenance is not independently established by this runner.',
                    'Raw local evidence contains RPCs and filesystem paths; public summary excludes them.'])
        field_keys = ('ammo','kills','wave','health','won','lost','paused','shots','breaches','cooldown')
        summary['outcomes'] = {}
        for name in ('loss','checkpoint','win','compiled_restore','win_after_compiled_load',
                     'fresh_process_restore','fresh_process_win'):
            if name in evidence:
                stage = evidence[name]
                values = stage['values']
                summary['outcomes'][name] = dict(tick=stage['tick'],
                    values={key:int(values[manifest['field_map'][key]]) for key in field_keys})
                if 'observed_terminal_tick' in stage:
                    summary['outcomes'][name]['observed_terminal_tick'] = stage['observed_terminal_tick']
                if 'waves' in stage:
                    summary['outcomes'][name]['drones_per_wave'] = {
                        key:len(ids) for key,ids in stage['waves'].items()}
        (out/'public-summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(passed=evidence['passed'], calls=len(evidence['calls']),
                             summary=str(out/'public-summary.json'))))
        if not evidence['passed']:
            print(evidence.get('error') or evidence['cleanup_errors'], file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
