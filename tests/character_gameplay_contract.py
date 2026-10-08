#!/usr/bin/env python3
"""Real compiled character intents through physics, rollback, reload and saves."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from components_gameplay_contract import Session, sha, uid


def fixture(actor=True):
    ops = []

    def entity(identity, name, position, scale=(1, 1, 1), parent=None):
        ops.extend([dict(op='entity.create', id=uid(identity), name=name, parent=parent),
            dict(op='component.set', id=uid(identity), type='Transform', value=dict(
                position=list(position), rotation=[0, 0, 0, 1], scale=list(scale)))])

    def component(identity, kind, value):
        ops.append(dict(op='component.set', id=uid(identity), type=kind, value=value))

    for identity, name, position, scale in [(1, 'Floor', (0, -.5, 0), (30, 1, 30)),
            (2, 'Wall', (0, 1.5, -4), (10, 3, .4))]:
        entity(identity, name, position, scale)
        component(identity, 'BoxCollider', dict(half_extents=[.5, .5, .5],
            motion='static', mass=1, friction=.5, restitution=0))
        component(identity, 'MeshRenderer', dict(primitive='box', visible=True,
            albedo=[.3, .35, .4]))
    entity(100, 'Player', (5, 1, 2))
    component(100, 'CharacterController', dict(radius=.3, height=1.8,
        speed=3, jump_speed=5, camera=uid(101)))
    entity(101, 'Player camera', (0, 1.6, 0), parent=uid(100))
    component(101, 'Camera', dict(vertical_fov=70, near=.1, far=100))
    if actor:
        entity(300, 'Compiled NPC', (0, 1, 2))
        component(300, 'CharacterController', dict(radius=.3, height=1.8,
            speed=3, jump_speed=5, camera=None))
        entity(301, 'NPC visual', (0, .9, 0), (.5, 1.8, .5), parent=uid(300))
        component(301, 'MeshRenderer', dict(primitive='box', visible=True,
            albedo=[.447, .184, .216]))
    return ops


def normalized(value):
    if isinstance(value, dict):
        return {k: normalized(v) for k, v in value.items() if k != 'session_id'}
    if isinstance(value, list):
        return [normalized(v) for v in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'incompatible', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not args.native_descriptor and not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('Supply CoreCLR paths or a native descriptor')

    def native(path):
        path = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if args.windows_interop else path

    args.native = native
    run = args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    files = {name: getattr(args, name) for name in ('binary', 'hostfxr', 'bridge',
        'assembly', 'incompatible', 'native_descriptor') if getattr(args, name)}
    if args.native_descriptor:
        descriptor = json.loads(args.native_descriptor.read_text())
        files['library'] = args.native_descriptor.parent/descriptor['library']
    evidence = dict(passed=False, backend='native_aot' if args.native_descriptor else 'coreclr',
        hashes={k: sha(v) for k, v in files.items()}, harness_sha256=sha(__file__), checks=[], calls=[],
        limits=['Small synthetic character fixture; no navigation, animation, GUI, physical input or performance claim.'])
    config = dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(
        hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(args.assembly),
        type='Poima.Tests.ManagedCharacterGame')
    process = None
    try:
        process = Session(args, run/'world.json', evidence)
        process.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=fixture()))
        authored = process.world.read_bytes()

        def restart():
            if process.session:
                process.rpc('runtime.stop', dict(session_id=process.session))
            process.start(1)
            process.step(60)
            process.load(config)

        def entity(identity=300):
            return process.rpc('runtime.entity', dict(session_id=process.session, tick=process.tick, id=uid(identity)))

        def position(identity=300):
            return entity(identity)['world_matrix'][12:15]

        def observed():
            return normalized(dict(tick=process.tick, game=process.game(),
                runtime=process.rpc('runtime.inspect', dict(session_id=process.session)),
                actor=entity(), player=entity(100), camera=entity(101)))

        def step(ticks=1, inputs=(), error=False):
            result = process.rpc('runtime.step', dict(session_id=process.session,
                request_id=uuid.uuid4().hex, expected_tick=process.tick, ticks=ticks, inputs=list(inputs)), error)
            if not error:
                process.tick += ticks
            return result

        def caller(identity=100, move=(0, 0), look=(0, 0), jump=False):
            return dict(entity=uid(identity), move=list(move), look=list(look), jump=jump)

        restart()
        player = position(100)
        initial = position()
        process.edit(Mode=1)
        step(30)
        moved = position()
        assert abs(moved[0]-initial[0]) < .001 and initial[2]-moved[2] > 1.4, (initial, moved)
        assert position(100) == player
        values = process.game()['module']['values']
        assert values['Ticks'] == values['StagedReads'] == 30 and values['PhysicalInputs'] == 0
        process.edit(Mode=0)
        paused = position()
        step(10)
        assert abs(position()[2]-paused[2]) < .001
        process.edit(Mode=1)
        step(180)
        stopped = position()
        assert -3.7 < stopped[2] < -3.2, stopped
        step(30)
        assert abs(position()[2]-stopped[2]) < .001
        evidence['checks'].append('Camera-free C# capsule moves through Jolt, stops at a wall, neutralizes omitted intents and leaves the player untouched; same-tick reads remain committed and Inputs contain no synthetic NPC command')

        restart()
        process.edit(Mode=2)
        before = entity()
        step()
        after = entity()
        assert after['velocity'][1] > 0 and after['world_matrix'] != before['world_matrix']
        assert after['yaw'] == 90 and after['pitch'] == 30, after
        process.edit(Mode=0)
        step(10)
        assert position()[1] > before['world_matrix'][13] + .2
        evidence['checks'].append('A compiled per-tick command applies yaw, pitch and a grounded jump without a camera')

        restart()
        for mode in (3, 4, 5, 8, 9, 10):
            process.edit(Mode=mode)
            before = observed()
            step(error=True)
            assert observed() == before and process.world.read_bytes() == authored, mode
        process.edit(Mode=6, FailAt=str(process.tick+1))
        before = observed()
        error = step(2, error=True)
        assert 'later-tick rollback' in error['message'] and observed() == before
        process.edit(Mode=1)
        step()
        evidence['checks'].append('Duplicate/dead/non-character targets, invalid float/bounds and a real second-tick C# exception restore complete observed state; the following valid command succeeds')

        restart()
        process.edit(Mode=1)
        for value in (caller(300), caller(300, (1, 0))):
            before = observed()
            step(2, [value], error=True)
            assert observed() == before
        process.edit(Mode=7, FailAt=str(process.tick+1))
        before = observed()
        step(2, [caller(100)], error=True)
        assert observed() == before
        process.edit(Mode=1)
        before_player = position(100)
        step(5, [caller(100, (1, 0))])
        assert position(100)[0] > before_player[0] + .2
        assert process.game()['module']['values']['PhysicalInputs'] == 1
        evidence['checks'].append('Explicit caller input owns a target throughout the whole batch, including neutral input and a second-tick conflict; distinct player and NPC controls coexist')

        if not args.native_descriptor:
            before = observed()
            process.load(config)
            after = observed()
            # A successful reload advances only the module's edit revision.
            before['game']['revision'] = after['game']['revision']
            migration = after['game']['module']['migration']
            assert migration['added'] == migration['removed'] == []
            assert migration['preserved'] == sorted(before['game']['module']['values'])
            before['game']['module']['migration'] = migration
            assert after == before
            if args.incompatible:
                before = observed()
                process.load(dict(config, assembly=native(args.incompatible)), error=True)
                assert observed() == before
            evidence['checks'].append('Compatible CoreCLR reload retains character and gameplay state; an incompatible schema rejects atomically' if args.incompatible else 'Compatible CoreCLR reload retains character and gameplay state')

        saves = run/'saves'
        saves.mkdir()
        process.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        saved_state = observed()
        saved = process.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
            slot='character', expected_generation=0, session_id=process.session,
            expected_tick=process.tick, expected_gameplay_revision=process.game()['revision']))
        payloads = list((saves/'slot-character').glob('p-*.bin'))
        assert len(payloads) == 1 and sha(payloads[0]) == saved['sha256']
        step(10)
        continuation = observed()
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.close()
        process = None
        process = Session(args, run/'world.json', evidence)
        process.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        fresh = uuid.uuid4().hex
        restored = process.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
            slot='character', expected_generation=1, revision=1, expected_session_id=None,
            expected_tick=None, expected_gameplay_revision=None, new_session_id=fresh, gameplay=config))
        process.session = fresh
        process.tick = restored['tick']
        restored_state = observed()
        # Migration is load diagnostics, not serialized simulation state. A
        # fresh game reports initial fields rather than the earlier reload.
        migration = restored_state['game']['module']['migration']
        assert migration['added'] == sorted(saved_state['game']['module']['values'])
        assert migration['preserved'] == migration['removed'] == []
        saved_state['game']['module']['migration'] = migration
        continuation['game']['module']['migration'] = migration
        assert restored_state == saved_state
        step(10)
        assert observed() == continuation
        assert process.world.read_bytes() == authored
        evidence['checks'].append('Original camera-free definition and compiled decision fields survive a durable fresh-process restore; continued movement matches the original runtime without persisting pending intents')
        evidence['passed'] = True
    finally:
        if process:
            process.close()
        evidence['rpc_count'] = len(evidence['calls'])
        (run/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
        print(run/'evidence.json')


if __name__ == '__main__':
    main()
