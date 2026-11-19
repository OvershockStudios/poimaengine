#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the committed arm pose on the exact tick of a late pickup obstruction."""

import argparse
from contextlib import closing
import copy
import json
import math
from pathlib import Path
import time

from carry_props import CarryRun, author, capture
from final_pickup_clearance import (
    FEATURE as CLEARANCE, GAME as LEGACY, PREVIEW_FEATURES,
    close_in_one_tick, last_reach_sample, recover, reject_invalid_witness,
)
from ik_clearance import NODES, finish, load, probe
from item_attachment_release import payload
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ACTOR, HAND, ITEM, RIG, WALL
from runtime_pose_frames import pose_camera, transform, uid
import world_physical_contract as native

GAME = 'Poima.Examples.PoseFallbackCarryPropsGame'
FEATURE = 'item_attachment_pickup_pose_v1'
FEATURES = PREVIEW_FEATURES | {CLEARANCE, FEATURE}
SIDE_CAMERA = uid(107)


def add_pose_observation(owner, revision):
    ops = [dict(op='entity.create', id=SIDE_CAMERA, name='Side pose witness', parent=None),
           native.component(SIDE_CAMERA, 'Transform', pose_camera((.77,1.32,-.29),(.035,1.11,-.46))),
           native.component(SIDE_CAMERA, 'Camera', dict(vertical_fov=42, near=.02, far=20))]
    for index, joint, name, position, scale, color in (
            (108,NODES[0],'Upper arm sleeve',(0,.1,0),(.035,.2,.035),(.32,.51,.65)),
            (109,NODES[1],'Forearm sleeve',(0,.1,0),(.035,.2,.035),(.43,.67,.74)),
            (110,NODES[1],'Elbow marker',(0,0,0),(.047,.047,.047),(.8,.68,.28)),
            (111,NODES[2],'Wrist marker',(0,0,0),(.047,.047,.047),(.8,.68,.28))):
        entity = uid(index)
        ops.extend([dict(op='entity.create', id=entity, name=name, parent=joint),
                    native.component(entity, 'Transform', transform(position, scale=scale)),
                    native.component(entity, 'MeshRenderer', dict(primitive='box', albedo=color, visible=True))])
    return native.transact(owner, ops, revision)['revision']


def arm(game, body, *, animation_pose=False):
    state = game.call('runtime.ik', tick=game.tick, entity=RIG, slot=1,
                      include_end_orientation=True)['state']
    goal = {key: state[key] for key in ('target', 'pole', 'enabled', 'space', 'weight')}
    if animation_pose:
        goal.update(enabled=False, weight=0)
    elif state['end_orientation']:
        goal['end_orientation'] = {key: state['end_orientation'][key] for key in ('rotation', 'weight')}
    result = probe(game, dict(entity=RIG, slot=1, **goal), [ACTOR, body])
    matrices = [game.matrix(joint) for joint in NODES]
    actual = [matrix[12:15] for matrix in matrices]
    assert max(math.dist(a, b) for a, b in zip(actual, result['current'])) < 1e-12
    if not animation_pose:
        assert max(math.dist(a, b) for a, b in zip(actual, result['candidate'])) < 1e-12
    return dict(points=actual, evaluated=result['candidate'], blocker=result['blocker'],
                enabled=state['enabled'], weight=state['weight'], end_orientation=state['end_orientation'],
                matrices=matrices)


def blocked(game, body):
    outcome = game.outcome()
    assert outcome['status'] == 'reach_blocked' and outcome['obstacle'] == WALL, outcome
    assert outcome['anchor'] == HAND and game.structure == 2
    assert game.binding()['released'] and game.binding()['anchor'] == body
    assert game.visible_item() == ITEM
    assert game.state()['PendingTick'] == str(game.tick)
    return outcome


def stationary_hits(game, joints, body):
    hits = []
    for first, second in zip(joints, joints[1:]):
        length = math.dist(first, second)
        x, y, z = [(b-a)/length for a, b in zip(first, second)]
        from_y = [z, 0, -x, 1+y]
        scale = math.hypot(*from_y)
        hit = game.call('runtime.capsule_cast', tick=game.tick,
                        position=[(a+b)/2 for a, b in zip(first, second)],
                        rotation=[value/scale for value in from_y],
                        radius=.02001, half_height=length/2,
                        direction=[0, 1, 0], distance=0, ignore=[ACTOR, body])['hit']
        hits.append(hit)
    return hits


def legacy_comparison(args, world, revision, current_config, elbow, tick):
    previous = copy.copy(args)
    previous.binary = args.previous_binary
    previous.descriptor = args.comparison_descriptor
    if args.comparison_assembly:
        previous.assembly, previous.bridge = args.comparison_assembly, args.comparison_bridge

    saves = args.output / 'legacy-saves'
    saves.mkdir()
    native.ARGS = previous
    legacy_config = configuration(previous, LEGACY, PREVIEW_FEATURES | {CLEARANCE})
    with closing(native.Session(args.output / 'legacy', world=world)) as owner:
        game = CarryRun(owner, revision, saves, legacy_config, bool(previous.descriptor))
        body, prior_elbow = last_reach_sample(game)
        assert math.dist(elbow, prior_elbow) < 1e-12
        close_in_one_tick(game, prior_elbow)
        assert game.tick == tick
        outcome = blocked(game, body)
        assert 'pose' not in outcome
        sampled = arm(game, body)
        assert sampled['end_orientation'] is not None
        assert sampled['blocker']['hit']['entity'] == WALL and sampled['blocker']['hit']['penetration_depth'] > .01, sampled
        still_clipping = capture(game, args, 'legacy-blocked-tick', SIDE_CAMERA)
        archive = game.save('legacy-blocked')
        assert payload(archive)['attachment_outcomes']['version'] == 2
        boundary = (game.tick, game.structure, game.game_revision)

    with closing(native.Session(args.output / 'legacy-requirements', world=world)) as owner:
        old = CarryRun(owner, revision, saves, current_config, bool(args.descriptor))
        old.call('runtime.start', revision=revision)
        rejection = load(old, error=-32060)
        assert FEATURE in rejection['message'], rejection

    native.ARGS = args
    with closing(native.Session(args.output / 'legacy-restored', world=world)) as owner:
        restored = CarryRun(owner, revision, saves, legacy_config, bool(previous.descriptor))
        restored.restore('legacy-blocked', *boundary)
        assert restored.save('legacy-restored') == archive
        assert restored.outcome() == outcome and arm(restored, body) == sampled
    return dict(outcome=outcome, sampled=sampled, capture=still_clipping, rejection=rejection)


def other_fallbacks(args, world, revision, config):
    results = {}
    for mode in ('previous', 'unresolved'):
        saves = args.output / f'{mode}-saves'
        saves.mkdir()
        with closing(native.Session(args.output / mode, world=world)) as owner:
            game = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            body, elbow = last_reach_sample(game)
            prior = arm(game, body)
            assert prior['blocker'] is None
            if mode == 'previous':
                # Place the near wall face between the two elbow capsule edges.
                # The authored wall has a 20 mm x half-extent and the arm radius is 20 mm.
                center = [(.5 * (elbow[0] + prior['points'][1][0])) + .04, elbow[1], elbow[2]]
            else:
                # The two upper arms converge in front of the shoulder.
                # This wall covers both the previous and incoming-animation poses.
                shoulder = prior['points'][0]
                center = [shoulder[0] + .03, shoulder[1], shoulder[2] - .12]
            close_in_one_tick(game, center)
            outcome = blocked(game, body)
            assert outcome['pose'] == mode, outcome
            sampled = arm(game, body)
            if mode == 'previous':
                assert sampled == prior, (sampled, prior)
            else:
                assert sampled['blocker']['hit']['entity'] == WALL and sampled['weight'] == 1, sampled
                incoming = arm(game, body, animation_pose=True)
                for candidate in (prior['points'], incoming['evaluated']):
                    assert any(hit and hit['entity'] == WALL for hit in stationary_hits(game, candidate, body))
            archive = game.save(mode)
            assert payload(archive)['attachment_outcomes']['outcomes'] == [outcome]
            boundary = (game.tick, game.structure, game.game_revision)
        with closing(native.Session(args.output / f'{mode}-cold', world=world)) as owner:
            cold = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            cold.restore(mode, *boundary)
            assert cold.save(f'{mode}-cold') == archive
            assert cold.outcome() == outcome and arm(cold, body) == sampled
        results[mode] = dict(outcome=outcome, sampled=sampled)
    return results


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 1100
    native.DEADLINE = time.monotonic() + 340
    native.REPORT['owners'].clear()
    native.REPORT['calls'].clear()
    config = configuration(args, GAME, FEATURES)
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    saves = args.output / 'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        with closing(native.Session(args.output / 'warm')) as owner:
            world = owner.world
            schema = owner.rpc('world.describe')['gameplay_item_attachments']
            assert (schema['pickup_pose_feature'], schema['pickup_pose_command_version']) == (FEATURE, 8)
            revision = author(owner, json.loads(manifest.read_text(encoding='utf-8-sig')),
                              wall_scale=(.04,.06,.05), rig_degrees=-65)
            revision = add_pose_observation(owner, revision)
            game = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            body, elbow = last_reach_sample(game)
            incoming = arm(game, body, animation_pose=True)['evaluated']
            close_in_one_tick(game, elbow)
            outcome = blocked(game, body)
            assert outcome['pose'] == 'animation', outcome
            sampled = arm(game, body)
            assert sampled['blocker'] is None and not sampled['enabled'] and sampled['weight'] == 0, sampled
            assert sampled['end_orientation'] is None
            assert max(math.dist(a, b) for a, b in zip(sampled['points'], incoming)) < 1e-12
            same_tick = capture(game, args, 'fallback-blocked-tick', SIDE_CAMERA)
            boundary = (game.tick, game.structure, game.game_revision)
            archive = game.save('blocked')
            assert payload(archive)['attachment_outcomes'] == dict(version=3, tick=game.tick, outcomes=[outcome])
            recover(game, outcome)
            assert game.state()['ReturnWeight'] == 0, game.state()
            cleared = [1.5, elbow[1], elbow[2]]
            pickup_tick = finish(game, cleared)
            game.call('runtime.entity', error=-32004, tick=game.tick, structure_revision=3, id=body)
            final = game.save('final')
            recovered = capture(game, args, 'recovered', SIDE_CAMERA)
            invalid_pose = reject_invalid_witness(game, final, field='pose', value='invented',
                                                  message='Invalid saved arm fallback pose')

        with closing(native.Session(args.output / 'cold', world=world)) as owner:
            cold = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            cold.restore('blocked', *boundary)
            assert cold.save('cold-blocked') == archive
            assert arm(cold, body) == sampled
            recover(cold, outcome)
            assert finish(cold, cleared) == pickup_tick
            assert cold.save('cold-final') == final

        legacy = legacy_comparison(args, world, revision, config, elbow, boundary[0]) if args.previous_binary else None
        if legacy:
            assert {k: v for k, v in outcome.items() if k != 'pose'} == legacy['outcome']
            assert math.dist(sampled['points'][1], legacy['sampled']['points'][1]) > .05
        other = other_fallbacks(args, world, revision, config)
        report.update(passed=True, blocked_tick=boundary[0], pickup_tick=pickup_tick, outcome=outcome,
                      sampled=sampled, other_fallbacks=other, legacy=legacy, invalid_pose=invalid_pose,
                      exact_archive_pairs=5 + bool(legacy), captures=dict(blocked=same_tick, recovered=recovered))
    finally:
        report.update(calls=len(native.REPORT['calls']), owners=native.REPORT['owners'])
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--previous-binary', type=Path)
    parser.add_argument('--comparison-descriptor', type=Path)
    parser.add_argument('--comparison-assembly', type=Path)
    parser.add_argument('--comparison-bridge', type=Path)
    parser.add_argument('--gpu', type=int)
    args = parser.parse_args()
    if not args.descriptor and not args.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    if args.previous_binary and not (args.comparison_descriptor if args.descriptor
                                     else args.comparison_assembly and args.comparison_bridge):
        parser.error('Supply a matching retained gameplay artifact with --previous-binary.')
    verify(args)
