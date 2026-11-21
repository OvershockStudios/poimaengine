#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the compiled GuardGame with native sight and a fresh-owner save continuation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

TESTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS))
from components_gameplay_contract import sha, uid
from fps_feedback_gameplay_contract import RecordedSession
from input_actions_gameplay_contract import fresh, need


def box(entity, name, position, half_extents, motion):
    return [dict(op='entity.create', id=uid(entity), name=name),
            dict(op='component.set', id=uid(entity), type='Transform', value=dict(
                position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=uid(entity), type='BoxCollider', value=dict(
                motion=motion, half_extents=half_extents, mass=1, friction=.5, restitution=0))]


def inspect(owner):
    return owner.rpc('runtime.inspect', dict(session_id=owner.session))


def step(owner, motions=None):
    request = dict(session_id=owner.session, request_id=fresh(), expected_tick=owner.tick,
                   expected_structure_revision=inspect(owner)['structure_revision'], ticks=1)
    if motions is not None:
        request['motions'] = motions
    result = owner.rpc('runtime.step', request)
    need(result['current_tick'] == owner.tick + 1, 'The guard must evaluate once per native Tick')
    owner.session, owner.tick = result['current_session_id'], result['current_tick']


def save(owner, saves, slot):
    state = inspect(owner)
    receipt = owner.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
        expected_generation=0, session_id=owner.session, expected_tick=owner.tick,
        expected_gameplay_revision=owner.game()['revision'], expected_structure_revision=state['structure_revision'],
        expected_material_revision=state['material_revision'], expected_ui_revision=state['ui_revision'],
        expected_control_sequence=state['control_sequence']))
    directory = saves / ('slot-' + slot)
    current = json.loads((directory / 'current.json').read_bytes())['payload']['current']
    need(current['generation'] == receipt['generation'] == 1, 'Save generation differs')
    need(re.fullmatch(r'p-[0-9a-f]{32}\.bin', current['file']), 'Invalid durable payload filename')
    payload = (directory / current['file']).read_bytes()
    body = json.loads(payload)
    need(body['format'] == 'poima.world-save' and body['version'] == 1
         and body['snapshot']['version'] == 1, 'Unexpected snapshot format for the plain box fixture')
    need(hashlib.sha256(payload).hexdigest() == current['sha256'] == receipt['sha256'], 'Save payload digest differs')
    need(len(payload) == current['bytes'] == receipt['bytes'], 'Save payload length differs')
    return payload


def first_sight(owner):
    # The callback samples the committed pose. The blocker moves during this Tick.
    step(owner, [dict(entity=uid(8), position=[1, 1.6, -2], rotation=[0, 0, 0, 1], duration_ticks=1)])
    middle = owner.game()['module']['values']
    need([middle[key] for key in ('SightStatus', 'LastDecision', 'ActionProgress', 'ActionRunning')]
         == [2, 2, 2, 1], 'Perception while the blocker moves must resume patrol')
    step(owner)
    after = owner.game()['module']['values']
    need([after[key] for key in ('SightStatus', 'LastDecision', 'ActionProgress', 'ActionRunning',
                                 'Interruptions', 'NextNode', 'LayoutId')]
         == [1, 1, 0, 0, 1, 0, '0'], 'Native sight must interrupt patrol and select Observe')
    return after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.deadline = time.monotonic() + 90
    args.native = lambda path: str(path.resolve())
    run = args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    world, saves = run / 'world.json', run / 'saves'
    saves.mkdir()
    config = {name: args.native(getattr(args, name)) for name in ('hostfxr', 'bridge', 'assembly')}
    config['type'] = 'Poima.BehaviorTreeExample.GuardGame'
    paths = [args.binary, args.hostfxr, args.bridge, args.assembly, args.assembly.parent / 'Poima.Ai.dll']
    evidence = dict(passed=False, owners=[], calls=[], artifact_sha256={path.name: sha(path) for path in paths})
    owner = None
    try:
        owner = RecordedSession(args, world, evidence)
        revision = owner.rpc('world.transact', dict(request_id=fresh(), base_revision=0,
            ops=box(7, 'Target', [0, 1.6, -4], [.5, .5, .5], 'static')
            + box(8, 'Occluder', [0, 1.6, -2], [.3, .7, .3], 'kinematic')))['revision']
        original = world.read_bytes()
        owner.start(revision)
        module = owner.load(config)['module']
        need(module['backend'] == 'coreclr' and module['assembly_sha256'] == sha(args.assembly), 'Wrong compiled game module')
        owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=args.native(saves)))
        step(owner)
        patrol = owner.game()['module']['values']
        need([patrol[key] for key in ('SightStatus', 'LastDecision', 'ActionProgress', 'ActionRunning', 'NextNode')]
             == [2, 2, 1, 1, 5], 'Occluded guard must start patrol')
        need(1 <= int(patrol['LayoutId']) <= 9007199254740991, 'Invalid saved cursor signature')
        save(owner, saves, 'patrol')
        observed = first_sight(owner)
        completed = save(owner, saves, 'completed')
        owner.close()
        owner = RecordedSession(args, world, evidence)
        owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=args.native(saves)))
        loaded = owner.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot='patrol',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, expected_structure_revision=None, expected_material_revision=None,
            expected_ui_revision=None, expected_control_sequence=None, new_session_id=fresh(), gameplay=config))
        owner.session = loaded.get('session_id', loaded.get('new_session_id'))
        owner.tick = loaded['tick']
        need(owner.tick == 1 and owner.game()['module']['values'] == patrol, 'Fresh owner must restore all saved guard fields')
        need(first_sight(owner) == observed, 'Fresh owner must select the same action and record identical diagnostics')
        replayed = save(owner, saves, 'replayed')
        need(completed == replayed, 'Complete post-action checkpoint bytes differ on replay')
        owner.close()
        owner = None
        need(world.read_bytes() == original, 'Runtime changes must preserve authored scene bytes')
        evidence.update(passed=True, world_unchanged=True, initial_patrol=patrol, resumed_observation=observed,
                        completed_save=hashlib.sha256(completed).hexdigest(), replayed_save=hashlib.sha256(replayed).hexdigest())
        print(f'{len(evidence["calls"])} native requests across two owners. Full checkpoint bytes agree.')
    finally:
        try:
            if owner is not None:
                owner.close()
        finally:
            (run / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()
