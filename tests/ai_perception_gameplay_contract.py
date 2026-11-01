#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prebuilt Perception Yard: native sight rays, last-seen memory, rollback and full cold saves.

Consumes an already compiled CoreCLR sample assembly plus the CLI, hostfxr and
bridge. Builds nothing, opens no window or audio device, and needs an external
process-tree owner. Expected sight values come from an independent axis-aligned
box oracle over the authored operations and analytic station positions; memory
expectations come from causal path analysis. Game telemetry is only compared.
Authored as source; it has no execution record until root runs it.
"""
import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import re
import subprocess
import time

from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import exact, fresh, need
from fps_feedback_gameplay_contract import RecordedSession, CAP
from switchback_oracle import read_checkpoint, exact_checkpoint

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT/'examples/perception-yard'
GAME = 'Poima.Examples.PerceptionYardGame'
IDENTITY = 'poima.example.perception-yard'
RPC_LIMIT, SECONDS = 600, 180
FIELDS = {
    **dict.fromkeys(('Station', 'PendingStation', 'RayBudget', 'Sampling', 'TargetMode', 'SelfMode', 'MotionTicks',
                     'SightStatus', 'RaysCast', 'FirstVisible', 'S0Status', 'S0Reason', 'S1Status', 'S1Reason',
                     'MemoryPhase', 'MemoryEvents', 'SearchStatus'), 'int32'),
    **dict.fromkeys(('Ticks', 'FailAt', 'LastTick', 'RetainTicks', 'TotalRays', 'FirstSeenTick', 'LastSeenTick',
                     'RetainUntilTick', 'MemoryUpdateTick', 'Reacquisitions'), 'int64'),
    **dict.fromkeys(('S0Hit', 'S1Hit', 'MemoryTarget', 'SearchHit'), 'entity'),
    **dict.fromkeys(('HorizontalHalfAngle', 'VerticalHalfAngle', 'MinRange', 'MaxRange', 'GazeX', 'GazeZ',
                     'TorsoX', 'TorsoY', 'TorsoZ', 'HeadX', 'HeadY', 'HeadZ',
                     'S0Distance', 'S0HitDistance', 'S0Azimuth', 'S0Elevation',
                     'S1Distance', 'S1HitDistance', 'S1Azimuth', 'S1Elevation',
                     'LastKnownX', 'LastKnownY', 'LastKnownZ', 'SearchHitDistance'), 'float64'),
}
CONFIG = dict(HorizontalHalfAngle=.7, VerticalHalfAngle=.45, MinRange=.25, MaxRange=12.0, RetainTicks='90',
              RayBudget=4, Sampling=0, TargetMode=0, SelfMode=0)
RETAIN = 90
EYE = (0.0, 1.6, 6.0)
REST = (0.0, -1.0)
SELF = {0: (10, 11), 1: (10,), 2: ()}
TARGET = {0: (20, 21), 1: (20,)}
# Package enum values: sight status, sample status, reject reason, memory phase/events.
VISIBLE, OCCLUDED, OUT_OF_VIEW, INCONCLUSIVE = 1, 2, 3, 4
REJECTED, EXHAUSTED, SKIPPED, CLEAR, TARGET_HIT, BLOCKED = 1, 2, 3, 4, 5, 6
TOO_CLOSE, TOO_FAR, OUTSIDE_HORIZONTAL, OUTSIDE_VERTICAL = 2, 3, 5, 6
EMPTY, SEEING, REMEMBERING, EXPIRED = 0, 1, 2, 3
REFRESHED, LOST, EXPIRED_EVENT, REACQUIRED = 2, 4, 8, 16


class PerceptionSession(RecordedSession):
    def rpc(self, method, params=None, error=False):
        need(len(self.evidence['calls']) < RPC_LIMIT, f'RPC admission {RPC_LIMIT} bound')
        return super().rpc(method, params, error)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def near(actual, expected, tolerance, message):
    need(type(actual) is float and math.isfinite(actual) and abs(actual-expected) <= tolerance,
         f'{message}: {actual!r}, expected {expected!r} within {tolerance}')


def heading(point):
    dx, dz = point[0]-EYE[0], point[2]-EYE[2]
    length = math.hypot(dx, dz)
    return (dx/length, dz/length)


class Yard:
    """Independent model of the authored colliders: solid axis-aligned boxes, closest entry, ties by ID."""

    def __init__(self, ops, stations, torso_y, head_y):
        self.stations, self.torso_y, self.head_y = stations, torso_y, head_y
        entities = {}
        for op in ops:
            if op['op'] == 'entity.create':
                entities[int(op['id'], 16)] = {}
            elif op['op'] == 'component.set':
                entities[int(op['id'], 16)][op['type']] = op['value']
        self.colliders = []
        for n, value in entities.items():
            pose = value.get('Transform')
            if 'BoxCollider' in value:
                need(value['BoxCollider']['half_extents'] == [.5, .5, .5] and pose['rotation'] == [0, 0, 0, 1],
                     'Oracle requires axis-aligned unit box colliders')
                self.colliders.append((n, pose['position'], [s/2 for s in pose['scale']]))
            elif 'CharacterController' in value:
                # Conservative enclosing box for the spectator capsule; it lies far from every sight line.
                p, c = pose['position'], value['CharacterController']
                self.colliders.append((n, [p[0], p[1]+c['height']/2, p[2]], [c['radius'], c['height']/2, c['radius']]))
        need({n for n, _, _ in self.colliders} == {1, 2, 10, 11, 20, 21, 30, 31}, 'Unexpected authored collider set')

    def points(self, xz):
        return [(xz[0], self.torso_y, xz[1]), (xz[0], self.head_y, xz[1])]

    def boxes(self, xz):
        result = []
        for n, centre, extents in self.colliders:
            if n in (20, 21):
                centre = [xz[0], self.torso_y if n == 20 else self.head_y, xz[1]]
            result.append((n, [centre[k]-extents[k] for k in range(3)], [centre[k]+extents[k] for k in range(3)]))
        return result

    @staticmethod
    def ray(origin, point, ignore, boxes):
        d = [point[k]-origin[k] for k in range(3)]
        length = math.sqrt(sum(x*x for x in d))
        u = [x/length for x in d]
        best = None
        for n, lo, hi in boxes:
            if n in ignore:
                continue
            first, last = 0.0, length
            for k in range(3):
                if u[k] == 0:
                    if not lo[k] <= origin[k] <= hi[k]:
                        break
                else:
                    a, b = sorted(((lo[k]-origin[k])/u[k], (hi[k]-origin[k])/u[k]))
                    first, last = max(first, a), min(last, b)
                    if first > last:
                        break
            else:
                if best is None or (first, n) < best:
                    best = (first, n)
        return best

    def sight(self, eye, gaze, points, targets, ignore, budget, boxes):
        """Restates the documented package contract with yaw/pitch limits (cone disabled) and FirstVisible."""
        f, r = (gaze[0], 0.0, gaze[1]), (-gaze[1], 0.0, gaze[0])
        rows, first, rays, remaining, unchecked, rejected = [], -1, 0, budget, 0, 0
        for i, p in enumerate(points):
            d = [p[k]-eye[k] for k in range(3)]
            distance = math.sqrt(sum(x*x for x in d))
            x, z = sum(d[k]*r[k] for k in range(3)), sum(d[k]*f[k] for k in range(3))
            azimuth, elevation = math.atan2(x, z), math.atan2(d[1], math.hypot(x, z))
            reason = (TOO_CLOSE if distance < CONFIG['MinRange'] else TOO_FAR if distance > CONFIG['MaxRange'] else
                      OUTSIDE_HORIZONTAL if abs(azimuth) > CONFIG['HorizontalHalfAngle'] else
                      OUTSIDE_VERTICAL if abs(elevation) > CONFIG['VerticalHalfAngle'] else 0)
            hit = None
            if reason:
                status, rejected = REJECTED, rejected+1
            elif first >= 0:
                status = SKIPPED
            elif remaining == 0:
                status, unchecked = EXHAUSTED, unchecked+1
            else:
                remaining, rays = remaining-1, rays+1
                hit = self.ray(eye, p, ignore, boxes)
                status = CLEAR if hit is None else TARGET_HIT if hit[1] in targets else BLOCKED
                if status in (CLEAR, TARGET_HIT):
                    first = i
            rows.append(dict(status=status, reason=reason, hit=0 if hit is None else hit[1],
                             hit_distance=0.0 if hit is None else hit[0], distance=distance, azimuth=azimuth, elevation=elevation))
        status = VISIBLE if first >= 0 else OUT_OF_VIEW if rejected == len(points) else INCONCLUSIVE if unchecked else OCCLUDED
        return dict(status=status, rays=rays, first=first, rows=rows)

    def robust(self, gaze, points, targets, ignore, budget, boxes):
        """The expectation must not change when the eye moves 2 mm; otherwise the scenario itself is too marginal."""
        expected = self.sight(EYE, gaze, points, targets, ignore, budget, boxes)
        key = lambda e: (e['status'], e['rays'], e['first'], [(r['status'], r['reason'], r['hit']) for r in e['rows']])
        for k in range(3):
            for sign in (-2e-3, 2e-3):
                eye = list(EYE)
                eye[k] += sign
                need(key(self.sight(tuple(eye), gaze, points, targets, ignore, budget, boxes)) == key(expected),
                     'Analytic expectation is within 2 mm of a classification boundary')
        return expected

    def lattice(self, start, end):
        (ax, az), (bx, bz) = self.stations[start], self.stations[end]
        n = max(1, math.ceil(math.hypot(bx-ax, bz-az)/.5-1e-6))
        return [(ax+(bx-ax)*k/n, az+(bz-az)*k/n) for k in range(n+1)]

    def visible(self, xz, gaze):
        return self.robust(gaze, self.points(xz), TARGET[0], SELF[0], 4, self.boxes(xz))['status'] == VISIBLE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly'):
        parser.add_argument('--'+name, required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    args.native = native
    run = args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    args.deadline = started+SECONDS
    image = args.assembly.resolve()
    files = [Path(__file__).resolve(), args.binary.resolve(), ROOT/'packages/ai/Perception.cs',
             ROOT/'tests/fps_feedback_gameplay_contract.py', ROOT/'tests/input_actions_gameplay_contract.py',
             ROOT/'tests/components_gameplay_contract.py', ROOT/'tests/character_gameplay_contract.py',
             ROOT/'tests/switchback_oracle.py', image, args.bridge.resolve(), args.hostfxr.resolve()]
    files += sorted(p for p in SAMPLE.iterdir() if p.suffix in ('.cs', '.csproj', '.py'))
    for assembly in (image, args.bridge.resolve()):
        files.append(assembly.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json', '.runtimeconfig.json'):
            member = assembly.with_suffix(suffix)
            if member.is_file():
                files.append(member)
    pins = {str(p): sha(p) for p in set(files)}
    config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    evidence = dict(format='poima.perception-yard-compiled-contract.v1', passed=False, calls=[], owners=[], checks=[],
                    observations=[], expectations=[], checkpoints=[], whole_pairs=[], source_pins=pins, backend='coreclr',
                    limits=dict(seconds=SECONDS, rpc=RPC_LIMIT, evidence_bytes=CAP),
                    scope='Compiled sample, native GameContext.Raycast/IsAlive, kinematic primitive boxes, synthetic UI controls, '
                          'typed edits, rollback and complete native save/same-owner/fresh-owner continuation. No GPU, physical '
                          'input, hearing/navigation/tactics, AOT, performance or fresh-agent claim.')
    process = None
    sequence = 0
    expected_first_seen = expected_last_seen = 0
    try:
        prepare = module('perception_yard_prepare', SAMPLE/'prepare.py')
        evidence['preparation'] = prepare.prepare(run/'project')
        ops = prepare.operations()
        yard = Yard(ops, prepare.STATIONS, prepare.TORSO_Y, prepare.HEAD_Y)
        world = run/'project/world.json'
        process = PerceptionSession(args, world, evidence)
        revision = process.rpc('world.inspect')['revision']
        need(revision == 0, 'Fresh native world required')
        revision = process.rpc('world.transact', dict(request_id=fresh(), base_revision=revision, ops=ops))['revision']
        original = world.read_bytes()
        process.start(revision)
        loaded = process.load(config)
        need(loaded['module']['backend'] == 'coreclr' and loaded['module']['assembly_sha256'] == sha(image), 'Wrong compiled consumer')
        schema = loaded['module']['schema']
        need(schema['identity'] == IDENTITY and len(schema['fields']) == len(FIELDS) and
             {f['name']: f['kind'] for f in schema['fields']} == FIELDS, 'Complete original typed field contract differs')
        saves = run/'saves'
        saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))

        def inspect():
            return process.rpc('runtime.inspect', dict(session_id=process.session))

        def validate(v):
            need(type(v) is dict and set(v) == set(FIELDS), 'Complete original state field set differs')
            for name, kind in FIELDS.items():
                value = v[name]
                if kind == 'int32':
                    valid = type(value) is int and -(2**31) <= value < 2**31
                elif kind == 'int64':
                    valid = type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None
                elif kind == 'entity':
                    valid = type(value) is str and re.fullmatch(r'[0-9a-f]{32}', value) is not None
                else:
                    valid = type(value) is float and math.isfinite(value)
                need(valid, 'Original typed field invalid: '+name)
                if kind == 'int64':
                    need(-(2**63) <= int(value) < 2**63, 'Original Int64 field outside signed64 range: '+name)
            return v

        def observe(label):
            v = validate(process.game()['module']['values'])
            evidence['observations'].append(dict(label=label, tick=process.tick, values=copy.deepcopy(v)))
            return v

        def snapshot(label):
            nonlocal sequence
            sequence += 1
            slot = 'perception-proof-'+str(sequence)
            state = inspect()
            game = process.game()
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=game['revision'], expected_structure_revision=state['structure_revision'],
                expected_material_revision=state['material_revision'], expected_ui_revision=state['ui_revision'],
                expected_control_sequence=state['control_sequence']))
            row = read_checkpoint(saves, slot, receipt)
            validate(row['snapshot']['payload']['gameplay']['values'])
            directory = saves/('slot-'+slot)
            current = json.loads((directory/'current.json').read_bytes())['payload']['current']
            row['original_path'] = str((directory/current['file']).relative_to(run))
            evidence['checkpoints'].append(dict(label=label, slot=slot, **row))
            return copy.deepcopy(row), slot

        def pair(label, left, right):
            need(exact_checkpoint(left, right) and
                 (run/left['original_path']).read_bytes() == (run/right['original_path']).read_bytes(),
                 'Complete original checkpoint bytes differ: '+label)
            evidence['whole_pairs'].append(dict(label=label, equal=True, bytes_equal=True,
                                                left=left['original_path'], right=right['original_path'], exclusions=[]))

        def control(n):
            state = inspect()
            game = process.game()
            result = process.rpc('runtime.ui.activate', dict(session_id=process.session, request_id=fresh(),
                expected_tick=process.tick, expected_ui_revision=state['ui_revision'],
                expected_control_sequence=state['control_sequence'], expected_gameplay_revision=game['revision'],
                expected_structure_revision=state['structure_revision'], id=uid(n)))
            process.session = result['current_session_id']
            process.tick = result['current_tick']

        def step(ticks=1, error=False):
            result = process.rpc('runtime.step', dict(session_id=process.session, request_id=fresh(),
                expected_tick=process.tick, expected_structure_revision=inspect()['structure_revision'], ticks=ticks), error)
            if not error:
                process.session = result['current_session_id']
                process.tick = result['current_tick']
            return result

        def advance(ticks, label):
            before = observe(label+' / before')
            step(ticks)
            after = observe(label)
            need(int(after['Ticks']) == int(before['Ticks'])+ticks and int(after['LastTick']) == int(before['LastTick'])+ticks and
                 int(after['LastTick']) == process.tick-1,
                 'Committed Tick count/context tick did not advance exactly: '+label)
            return before, after

        def restore(slot, existing=True):
            state = inspect() if existing else None
            game = process.game() if existing else None
            result = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=1, revision=revision,
                expected_session_id=process.session if existing else None, expected_tick=process.tick if existing else None,
                expected_gameplay_revision=game['revision'] if existing else None,
                expected_structure_revision=state['structure_revision'] if existing else None,
                expected_material_revision=state['material_revision'] if existing else None,
                expected_ui_revision=state['ui_revision'] if existing else None,
                expected_control_sequence=state['control_sequence'] if existing else None,
                new_session_id=fresh(), gameplay=config))
            process.session = result['session_id'] if 'session_id' in result else result['new_session_id']
            process.tick = result['tick']

        def check(label, v, station, gaze, next_gaze=None, budget=4, target=0, self_mode=0):
            """Compare one Tick's sight telemetry with the independent oracle at the analytic station."""
            need(int(v['LastTick']) == process.tick-1, label+': callback tick differs from committed native tick minus one')
            xz = yard.stations[station]
            e = yard.robust(gaze, yard.points(xz), TARGET[target], SELF[self_mode], budget, yard.boxes(xz))
            for key, value in zip(('TorsoX', 'TorsoY', 'TorsoZ', 'HeadX', 'HeadY', 'HeadZ'),
                                  (*yard.points(xz)[0], *yard.points(xz)[1])):
                near(v[key], value, 1e-4, label+' committed '+key)
            g = next_gaze or gaze
            near(v['GazeX'], g[0], 1e-6, label+' gaze x')
            near(v['GazeZ'], g[1], 1e-6, label+' gaze z')
            need(v['Station'] == station and v['PendingStation'] == 0 and v['MotionTicks'] == 0,
                 label+': intruder has not settled at the expected station')
            need((v['SightStatus'], v['RaysCast'], v['FirstVisible']) == (e['status'], e['rays'], e['first']),
                 f"{label}: aggregate {(v['SightStatus'], v['RaysCast'], v['FirstVisible'])} != oracle {(e['status'], e['rays'], e['first'])}")
            for prefix, row in zip(('S0', 'S1'), e['rows']):
                need((v[prefix+'Status'], v[prefix+'Reason'], v[prefix+'Hit']) == (row['status'], row['reason'], uid(row['hit'])),
                     f"{label}: {prefix} {(v[prefix+'Status'], v[prefix+'Reason'], v[prefix+'Hit'])} != oracle {row}")
                for name, key, tolerance in (('Distance', 'distance', 1e-4), ('HitDistance', 'hit_distance', 2e-4),
                                             ('Azimuth', 'azimuth', 1e-4), ('Elevation', 'elevation', 1e-4)):
                    near(v[prefix+name], row[key], tolerance, f'{label} {prefix}{name}')
            evidence['expectations'].append(dict(label=label, station=station, gaze=list(gaze), next_gaze=list(g),
                                                 budget=budget, target_mode=target, self_mode=self_mode, oracle=e))
            return e['rays']

        def search(label, v, spot, station, budget=4, self_mode=0):
            """Remembered-spot probe: empty target list, gaze toward the analytic last known position."""
            if spot is None:
                need(v['SearchStatus'] == 0 and v['SearchHit'] == uid(0) and v['SearchHitDistance'] == 0.0,
                     label+': search probe ran without remembered target')
                return 0
            xz = yard.stations[station]
            e = yard.robust(heading(spot), [spot], (), SELF[self_mode], budget, yard.boxes(xz))
            row = e['rows'][0]
            need((v['SearchStatus'], v['SearchHit']) == (row['status'], uid(row['hit'])),
                 f"{label}: search {(v['SearchStatus'], v['SearchHit'])} != oracle {row}")
            near(v['SearchHitDistance'], row['hit_distance'], 2e-4, label+' search hit distance')
            evidence['expectations'].append(dict(label=label+' / search', spot=list(spot), oracle=e))
            return e['rays']

        def memory(label, v, phase, reacquired=0, spot=None, events=None):
            tick = process.tick-1  # Native receipt counter, independent of game memory/LastTick telemetry.
            ticks = [int(v[k]) for k in ('FirstSeenTick', 'LastSeenTick', 'RetainUntilTick', 'MemoryUpdateTick')]
            if phase == EMPTY:
                need(v['MemoryPhase'] == EMPTY and v['MemoryTarget'] == uid(0) and ticks == [0, 0, 0, 0] and
                     v['Reacquisitions'] == '0' and (v['LastKnownX'], v['LastKnownY'], v['LastKnownZ']) == (0.0, 0.0, 0.0),
                     label+': memory is not canonically empty')
            else:
                first, last, until, update = ticks
                need(v['MemoryPhase'] == phase and v['MemoryTarget'] == uid(20) and int(v['Reacquisitions']) == reacquired and
                     (first, last, until, update) ==
                     (expected_first_seen, expected_last_seen, expected_last_seen+RETAIN, tick),
                     f'{label}: memory phase/target/reacquisitions/ticks differ: {v["MemoryPhase"]}, {v["Reacquisitions"]}, {ticks}, {tick}')
                need((phase == SEEING and last == tick) or (phase == REMEMBERING and last < tick <= until) or
                     (phase == EXPIRED and tick > until), label+': memory phase disagrees with ticks')
                for key, value in zip(('LastKnownX', 'LastKnownY', 'LastKnownZ'), spot):
                    near(v[key], value, 1e-4, label+' remembered '+key)
            if events is not None:
                need(v['MemoryEvents'] == events, f'{label}: memory events {v["MemoryEvents"]} != {events}')

        def rays(label, before, after, expected):
            need(int(after['TotalRays'])-int(before['TotalRays']) == expected,
                 f'{label}: native ray count {int(after["TotalRays"])-int(before["TotalRays"])} != analytic {expected}')

        def single(label, station, gaze, next_gaze=None, budget=4, target=0, self_mode=0, spot=None, **remembered):
            before, v = advance(1, label)
            used = check(label, v, station, gaze, next_gaze, budget, target, self_mode)
            used += search(label, v, spot, station, budget, self_mode)
            rays(label, before, v, used)
            # While remembering, the searched spot is the remembered position.
            memory(label, v, spot=spot, **remembered)
            return v

        def torso(xz):
            return (xz[0], prepare.TORSO_Y, xz[1])

        # A. Authored start behind the tall wall: both samples cast and blocked; nothing remembered.
        initial = observe('initial')
        need(all(exact(initial[k], value) for k, value in CONFIG.items()) and
             exact({k: initial[k] for k in ('Station', 'FailAt', 'FirstVisible', 'GazeX', 'GazeZ', 'Ticks')},
                   dict(Station=2, FailAt='-1', FirstVisible=-1, GazeX=0.0, GazeZ=-1.0, Ticks='0')), 'Initialized configuration differs')
        memory('initial', initial, EMPTY)
        step()
        v = observe('hidden at authored start')
        check('hidden at authored start', v, 2, REST)
        search('hidden at authored start', v, None, 2)
        memory('hidden at authored start', v, EMPTY, events=0)

        # B. Emerge into the open: acquisition is causal from the lattice, then continuous sight.
        path = yard.lattice(2, 1)
        seen = [yard.visible(p, REST) for p in path]
        need(not seen[0] and True in seen and all(seen[seen.index(True):]), 'Emergence path is not one hidden-to-visible run')
        control(510)
        move_start = process.tick
        expected_first_seen = move_start+seen.index(True)
        _, v = advance(len(path)+1, 'open ground')
        expected_last_seen = move_start+len(path)
        check('open ground', v, 1, REST)
        memory('open ground', v, SEEING, 0, torso(yard.stations[1]), REFRESHED)

        # C. Head over the low wall: torso blocked by 31, the second ordered sample sees head 21.
        path = yard.lattice(1, 3)
        need(all(yard.visible(p, REST) for p in path), 'Low-wall approach should stay visible through the head sample')
        control(512)
        move_start = process.tick
        _, v = advance(len(path)+1, 'head over low wall')
        expected_last_seen = move_start+len(path)
        check('head over low wall', v, 3, REST)
        memory('head over low wall', v, SEEING, 0, torso(yard.stations[3]), REFRESHED)
        s3 = torso(yard.stations[3])
        g3 = heading(s3)

        # D. Undeclared compound part fails closed: head 21 now occludes; memory loses and searches.
        process.edit(TargetMode=1)
        single('torso-only target', 3, REST, g3, target=1, spot=s3, phase=REMEMBERING, reacquired=0, events=LOST)
        process.edit(TargetMode=0)
        expected_first_seen = expected_last_seen = process.tick
        before, v = advance(1, 'compound target restored')
        rays('compound target restored', before, v, check('compound target restored', v, 3, g3) + search('compound target restored', v, None, 3))
        memory('compound target restored', v, SEEING, 1, s3, REACQUIRED)

        # E. One-ray budget: the head stays unchecked, so the verdict is Inconclusive rather than Occluded.
        process.edit(RayBudget=1)
        v = single('one-ray budget', 3, g3, budget=1, spot=s3, phase=REMEMBERING, reacquired=1, events=LOST)
        need(v['SightStatus'] == INCONCLUSIVE and v['S1Status'] == EXHAUSTED and v['S1Hit'] == uid(0), 'Budget did not leave the head unchecked')
        process.edit(RayBudget=4)
        expected_first_seen = expected_last_seen = process.tick
        before, v = advance(1, 'full budget restored')
        rays('full budget restored', before, v, check('full budget restored', v, 3, g3) + search('full budget restored', v, None, 3))
        memory('full budget restored', v, SEEING, 2, s3, REACQUIRED)

        # F. Explicit self colliders: omitting the visor or the post blocks every ray at the sentry.
        process.edit(SelfMode=1)
        single('visor not ignored', 3, g3, self_mode=1, spot=s3, phase=REMEMBERING, reacquired=2, events=LOST)
        process.edit(SelfMode=2)
        v = single('no self ignore', 3, g3, self_mode=2, spot=s3, phase=REMEMBERING, reacquired=2, events=0)
        need(v['S0Hit'] == uid(10) and v['S0HitDistance'] == 0.0, 'Eye inside the post did not block at distance zero')
        process.edit(SelfMode=0)
        expected_first_seen = expected_last_seen = process.tick
        before, v = advance(1, 'self ignore restored')
        rays('self ignore restored', before, v, check('self ignore restored', v, 3, g3) + search('self ignore restored', v, None, 3))
        memory('self ignore restored', v, SEEING, 3, s3, REACQUIRED)

        # G1. Malformed settings fail the Tick before any query or memory change.
        process.edit(MaxRange=20000.0)
        bad_before, _ = snapshot('invalid-range-before')
        error = step(error=True)
        need('Sight range must satisfy' in error['message'], 'Wrong invalid-settings failure')
        bad_after, _ = snapshot('invalid-range-after')
        pair('invalid-settings-rollback', bad_before, bad_after)
        process.edit(MaxRange=12.0)
        # G2. A second-Tick fault rolls back motion, rays, memory, gaze and UI from the first Tick too.
        control(511)
        control(515)
        fault_before, _ = snapshot('fault-before')
        error = step(2, error=True)
        need('Perception second-Tick rollback fixture' in error['message'], 'Wrong intentional callback failure')
        fault_after, _ = snapshot('fault-after')
        pair('second-tick-fault-rollback', fault_before, fault_after)
        control(516)

        # H. Retreat behind the tall wall: the remembered spot is the last analytically visible lattice point.
        path = yard.lattice(3, 2)
        lost = next(k for k, p in enumerate(path) if not yard.visible(p, g3))
        need(lost > 0 and all(yard.visible(p, g3) for p in path[:lost]), 'Retreat is not one visible-to-hidden run')
        spot = torso(path[lost-1])
        gl = heading(spot)
        need(not any(yard.visible(p, gl) for p in path[lost:]), 'Search gaze would reacquire during the retreat')
        move_start = process.tick
        # Motion stages on the first callback; lattice[k] is observed at move_start+k.
        expected_last_seen = move_start+lost-1
        _, v = advance(len(path)+1, 'behind tall wall')
        check('behind tall wall', v, 2, gl)
        search('behind tall wall', v, spot, 2)
        memory('behind tall wall', v, REMEMBERING, 3, spot, 0)
        wait = expected_last_seen+RETAIN-2-(process.tick-1)
        need(wait >= 0, 'Retreat consumed the retention window')
        if wait:
            advance(wait, 'remembering before save')

        # I. Complete warm save/restore and fresh-owner continuation across expiry.
        def continuation(owner):
            _, v = advance(3, owner+' expiry')
            check(owner+' expiry', v, 2, gl, REST)
            search(owner+' expiry', v, None, 2)
            memory(owner+' expiry', v, EXPIRED, 3, spot, EXPIRED_EVENT)
            _, v = advance(2, owner+' expired')
            check(owner+' expired', v, 2, REST)
            memory(owner+' expired', v, EXPIRED, 3, spot, 0)
            return snapshot(owner+'-five-tick-continuation')[0]

        saved, slot = snapshot('cold-source-remembering')
        restore(slot)
        same, _ = snapshot('same-owner-restored')
        pair('same-owner-complete', saved, same)
        continued = continuation('same-owner')
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.close()
        process = None
        process = PerceptionSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        restore(slot, False)
        cold, _ = snapshot('fresh-owner-restored')
        pair('cold-complete', saved, cold)
        pair('cold-five-tick-complete', continued, continuation('fresh-owner'))

        # J. Reacquire after expiry in the fresh owner.
        path = yard.lattice(2, 1)
        seen = [yard.visible(p, REST) for p in path]
        need(not seen[0] and all(seen[seen.index(True):]), 'Return path is not one hidden-to-visible run')
        control(510)
        move_start = process.tick
        expected_first_seen = move_start+seen.index(True)
        _, v = advance(len(path)+1, 'reacquired after expiry')
        expected_last_seen = move_start+len(path)
        check('reacquired after expiry', v, 1, REST)
        memory('reacquired after expiry', v, SEEING, 4, torso(yard.stations[1]), REFRESHED)

        # K. Field of view and range: after explicit forgetting, rejected samples cast no rays.
        for button, station, reason in ((513, 4, OUTSIDE_HORIZONTAL), (514, 5, TOO_FAR)):
            control(button)
            advance(len(yard.lattice(1 if station == 4 else 4, station))+1, f'move to station {station}')
            control(517)
            v = single(f'reset at station {station}', station, REST, phase=EMPTY, events=0)
            need(v['SightStatus'] == OUT_OF_VIEW and v['RaysCast'] == 0 and v['S0Reason'] == reason and v['S1Reason'] == reason,
                 f'Station {station} was not rejected by geometry alone')

        need(world.read_bytes() == original, 'Gameplay changed the authored world document')
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.close()
        process = None
        need(all(Path(p).is_file() and sha(p) == digest for p, digest in pins.items()), 'Consumed source changed')
        evidence['checks'] = [
            'Authored tall/low walls occlude exactly as an independent box oracle predicts, with analytic hit distances',
            'Compound target needs both colliders; an undeclared head part occludes (fail closed)',
            'Self post/visor must be ignored explicitly; the eye inside the post blocks at distance zero',
            'One-ray budget leaves the head unchecked and reports Inconclusive, distinct from Occluded',
            'Flank and far stations are rejected by horizontal limit and range with zero native rays',
            'Memory first/last-seen, retention and update ticks equal native receipt counters plus analytic lattice visibility; expiry wait is independent of memory telemetry',
            'Invalid settings and a second-Tick fault roll back complete native checkpoints byte-for-byte',
            'Same/fresh native owners restore identical archives and continue byte-identically across expiry',
        ]
        evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        if process:
            try:
                process.close()
            except BaseException as error:
                evidence['close_error'] = repr(error)
                evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-started
        evidence['rpc_count'] = len(evidence['calls'])
        if evidence['seconds'] > SECONDS:
            evidence['passed'] = False
            evidence['deadline_error'] = f'{SECONDS}s deadline exceeded'
        raw = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode()
        need(len(raw) <= CAP, '32MiB evidence cap')
        (run/'evidence.json').write_bytes(raw)
        print(run/'evidence.json', flush=True)
        if evidence.get('close_error') and not evidence.get('error'):
            raise RuntimeError(evidence['close_error'])
        if evidence.get('deadline_error') and not evidence.get('error'):
            raise TimeoutError(evidence['deadline_error'])


if __name__ == '__main__':
    main()
