#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prebuilt projectile consumer: analytic motion, native hits and full cold saves.

CPU only; requires an external process-tree owner. This fixture builds nothing,
retains literal RPC bytes and never modifies existing evidence or imported art.
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
SAMPLE = ROOT / 'examples/projectile-range'
GAME = 'Poima.Examples.ProjectileRangeGame'
IDENTITY = 'poima.example.projectile-range'
FIELDS = {
    **dict.fromkeys(('Mode', 'PendingMode', 'Phase', 'Hit', 'InitialOverlap',
                    'NormalValid', 'TriangleValid', 'LastQueries', 'CenterRayHit', 'MaxSegments'), 'int32'),
    **dict.fromkeys(('Ticks', 'QueryCount', 'Launches', 'FailAt', 'Triangle'), 'int64'),
    **dict.fromkeys(('HitEntity', 'RayEntity'), 'entity'),
    **dict.fromkeys(('PositionX', 'PositionY', 'PositionZ', 'VelocityX', 'VelocityY', 'VelocityZ', 'Age',
                    'AccelerationX', 'AccelerationY', 'AccelerationZ', 'Radius', 'Lifetime', 'MaxChordError',
                    'LastConsumedSeconds', 'LastArcDeviation', 'Fraction', 'Distance',
                    'PointOnQueryX', 'PointOnQueryY', 'PointOnQueryZ',
                    'PointOnObstacleX', 'PointOnObstacleY', 'PointOnObstacleZ',
                    'NormalX', 'NormalY', 'NormalZ', 'PenetrationDepth',
                    'IncidentVelocityX', 'IncidentVelocityY', 'IncidentVelocityZ'), 'float64'),
}


class ProjectileSession(RecordedSession):
    def rpc(self, method, params=None, error=False):
        need(len(self.evidence['calls']) < 450, 'RPC admission450 bound')
        return super().rpc(method, params, error)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def near(actual, expected, tolerance=2e-11):
    need(type(actual) is float and math.isfinite(actual) and abs(actual-expected) <= tolerance,
         f'Analytic value differs: {actual!r}, expected {expected!r}, tolerance {tolerance}')


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
    args.deadline = started + 180
    image = args.assembly.resolve()
    files = [Path(__file__).resolve(), args.binary.resolve(), ROOT/'packages/fps/Projectiles.cs',
             ROOT/'tests/fps_feedback_gameplay_contract.py', ROOT/'tests/input_actions_gameplay_contract.py',
             ROOT/'tests/components_gameplay_contract.py', ROOT/'tests/character_gameplay_contract.py',
             ROOT/'tests/switchback_oracle.py',
             ROOT/'docs/evidence/fixtures/agent-switchback-fresh/settings/range.poima-input.json',
             image, args.bridge.resolve(), args.hostfxr.resolve()]
    files += sorted(p for p in SAMPLE.iterdir() if p.suffix in ('.cs', '.csproj', '.py'))
    for assembly in (image, args.bridge.resolve()):
        files.append(assembly.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json', '.runtimeconfig.json'):
            member = assembly.with_suffix(suffix)
            if member.is_file():
                files.append(member)
    pins = {str(p): sha(p) for p in set(files)}
    config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    evidence = dict(format='poima.projectile-compiled-contract.v1', passed=False, calls=[], owners=[],
                    checks=[], observations=[], checkpoints=[], whole_pairs=[], source_pins=pins,
                    backend='coreclr', limits=dict(seconds=180, rpc=450, evidence_bytes=CAP),
                    scope='Compiled sample, synthetic native controls, static colliders and complete native save/rollback/cold continuation. No GPU, physical input, moving-target CCD, damage, AOT, performance or fresh-agent claim.')
    process = None
    sequence = 0
    try:
        prepare = module('projectile_sample_prepare', SAMPLE/'prepare.py')
        prepared = prepare.prepare(run/'project')
        evidence['preparation'] = prepared
        world = run/'project/world.json'
        process = ProjectileSession(args, world, evidence)
        revision = process.rpc('world.inspect')['revision']
        configure = module('projectile_sample_configure', SAMPLE/'configure.py')
        authored = process.rpc('world.transact', dict(request_id=fresh(), base_revision=revision, ops=configure.operations()))
        revision = authored['revision']
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
                    valid = type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None and -(2**63) <= int(value) < 2**63
                elif kind == 'entity':
                    valid = type(value) is str and re.fullmatch(r'[0-9a-f]{32}', value) is not None
                else:
                    valid = type(value) is float and math.isfinite(value)
                need(valid, 'Original typed field invalid: '+name)
            return v

        def values():
            return validate(process.game()['module']['values'])

        def observe(label):
            v = values()
            evidence['observations'].append(dict(label=label, tick=process.tick, values=copy.deepcopy(v)))
            return v

        def expect(label, **expected):
            v = observe(label)
            need(all(exact(v[k], value) for k, value in expected.items()),
                 'Unexpected sample state: '+label+' '+str({k: v[k] for k in expected}))
            return v

        def snapshot(label):
            nonlocal sequence
            sequence += 1
            slot = 'projectile-proof-'+str(sequence)
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

        expect('initial', Mode=0, PendingMode=0, Phase=2, QueryCount='0')
        before = values()
        control(510)
        queued = expect('queued fast launch', Mode=0, PendingMode=1, QueryCount='0')
        need(queued['Ticks'] == before['Ticks'] and queued['Age'] == before['Age'], 'Paused control advanced projectile time')
        step()
        fast = expect('fast shot hits thin wall', Mode=1, Phase=1, Hit=1, HitEntity=uid(10), InitialOverlap=0, LastQueries=1)
        # Wall front is z=-9.995. Its .02m sphere centre stops at -9.975,
        # not at the native contact point on the wall.
        near(fast['PositionZ'], -9.975, 3e-4)
        near(fast['Age'], 9.975/1200, 3e-7)
        near(fast['PointOnObstacleZ'], -9.995, 3e-4)
        need(abs(fast['PositionZ']-fast['PointOnObstacleZ']) > .019, 'Contact point incorrectly used as projectile centre')
        near(fast['VelocityZ'], -1200.0)
        step(3)
        held = expect('terminal impact does not repeat', Phase=1, Hit=1, LastQueries=0)
        need(held['QueryCount'] == fast['QueryCount'] and held['Age'] == fast['Age'] and held['PositionZ'] == fast['PositionZ'], 'Terminal projectile advanced or queried again')

        control(512)
        step()
        graze = expect('volume graze despite centre ray miss', Mode=3, Phase=1, Hit=1, HitEntity=uid(11), CenterRayHit=0, RayEntity=uid(0))
        need(-3.12 < graze['PositionZ'] < -2.88 and graze['InitialOverlap'] == 0, 'Graze contact outside independent target bounds')
        near(graze['PositionX'], 8.115)

        control(513)
        step()
        overlap = expect('initial inflated-sphere overlap', Mode=4, Phase=1, Hit=1, HitEntity=uid(10), InitialOverlap=1)
        near(overlap['Age'], 0.0)
        near(overlap['LastConsumedSeconds'], 0.0)
        near(overlap['PositionZ'], -10.0)

        control(514)
        step()
        expiring = expect('partial lifetime first tick', Mode=5, Phase=0, Hit=0)
        near(expiring['Age'], 1/60)
        step()
        expired = expect('lifetime clamps partial second tick', Mode=5, Phase=2, Hit=0)
        near(expired['Age'], .03125, 0)
        near(expired['PositionZ'], -.03125)
        near(expired['LastConsumedSeconds'], .03125-1/60)
        step(2)
        inert = expect('expired projectile remains terminal', Phase=2, LastQueries=0)
        need(inert['QueryCount'] == expired['QueryCount'] and inert['Age'] == expired['Age'], 'Expired projectile advanced')

        control(511)
        step(2)
        flight = expect('gravity midflight', Mode=2, Phase=0, Hit=0)
        t = 2/60
        near(flight['Age'], t)
        near(flight['PositionX'], 4+t)
        near(flight['PositionY'], 3+2*t-.5*9.81*t*t)
        near(flight['PositionZ'], -5*t)
        near(flight['VelocityY'], 2-9.81*t)
        need(0 < flight['LastArcDeviation'] <= .0001 and flight['LastQueries'] > 1, 'Gravity step omitted bounded curvature subdivision')

        control(515)
        fault_before, _ = snapshot('before-second-tick-fault')
        error = step(2, error=True)
        need('Projectile second-Tick staged-marker rollback fixture' in error['message'], 'Wrong intentional callback failure')
        fault_after, _ = snapshot('after-second-tick-fault')
        pair('whole-batch-rollback', fault_before, fault_after)
        control(516)
        saved, slot = snapshot('cold-source-midflight')
        restore(slot)
        same, _ = snapshot('same-owner-restored')
        pair('same-owner-complete', saved, same)
        step(5)
        continued, _ = snapshot('same-owner-five-tick-continuation')
        later = observe('gravity analytic after continuation')
        t = 7/60
        near(later['PositionX'], 4+t)
        near(later['PositionY'], 3+2*t-.5*9.81*t*t)
        near(later['PositionZ'], -5*t)
        near(later['VelocityY'], 2-9.81*t)
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.close()
        process = None
        process = ProjectileSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        restore(slot, False)
        cold, _ = snapshot('fresh-owner-restored')
        pair('cold-complete', saved, cold)
        step(5)
        cold_continued, _ = snapshot('fresh-owner-five-tick-continuation')
        pair('cold-five-tick-complete', continued, cold_continued)
        observe('fresh-owner final state')
        need(world.read_bytes() == original and prepare.identity(prepare.PROFILE) == prepared['input_profile'], 'Original authored scene or public input changed')
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.close()
        process = None
        need(all(Path(p).is_file() and sha(p) == digest for p, digest in pins.items()), 'Consumed source changed')
        evidence['checks'] = [
            'Fast sphere sweep hits a ten-millimetre wall; retained centre differs from obstacle contact',
            'Sphere graze hits while native centre ray misses; initial overlap consumes no time',
            'Exact analytic free flight and partial lifetime; terminal states issue no further queries',
            'UI launch queues without ticking; gravity uses bounded curvature subdivisions',
            'Second-Tick callback fault rolls back complete native game, marker, physics and UI archive',
            'Same/fresh native owners retain all typed fields and complete byte-identical continued-flight archives',
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
        if evidence['seconds'] > 180:
            evidence['passed'] = False
            evidence['deadline_error'] = '180s deadline exceeded'
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
