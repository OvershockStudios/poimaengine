#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare guarded pickup pose policies at the same obstacle and tick."""

import argparse
from contextlib import closing
import copy
import json
import math
from pathlib import Path
import time

from carry_props import CarryRun, author, capture
from final_pickup_clearance import close_in_one_tick, last_reach_sample, recover, reject_invalid_witness
from ik_clearance import finish, load
from item_attachment_release import payload
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ITEM, WALL
from pickup_pose_fallback import (
    FEATURES as ENDPOINT_FEATURES, GAME as ENDPOINT_GAME, SIDE_CAMERA, add_pose_observation,
    arm, blocked, other_fallbacks, stationary_hits,
)
import world_physical_contract as native

GAME = 'Poima.Examples.BlendedFallbackCarryPropsGame'
FEATURE = 'item_attachment_pickup_blend_v1'
FEATURES = ENDPOINT_FEATURES | {FEATURE}


def endpoints(game):
    body, elbow = last_reach_sample(game)
    prior = arm(game, body)
    animation = arm(game, body, animation_pose=True)['evaluated']
    close_in_one_tick(game, elbow)
    return body, elbow, prior, animation


def compare(args, world, revision, tick, current_config):
    old = copy.copy(args)
    old.binary = args.previous_binary
    old.descriptor = args.comparison_descriptor
    if not old.descriptor:
        old.assembly, old.bridge = args.comparison_assembly, args.comparison_bridge
    native.ARGS = old
    settings = configuration(old, ENDPOINT_GAME, ENDPOINT_FEATURES)
    saves = args.output / 'endpoint-saves'
    saves.mkdir()
    with closing(native.Session(args.output / 'endpoint', world=world)) as owner:
        game = CarryRun(owner, revision, saves, settings, bool(old.descriptor))
        body, _, prior, _ = endpoints(game)
        outcome = blocked(game, body)
        assert outcome['pose'] == 'animation' and game.tick == tick
        published = arm(game, body)
        assert published['blocker'] is None and published['end_orientation'] is None
        boundary = (game.tick, game.structure, game.game_revision)
        archive = game.save('endpoint')
        assert payload(archive)['attachment_outcomes']['version'] == 3
        frame = capture(game, args, 'endpoint', SIDE_CAMERA)
    with closing(native.Session(args.output / 'previous-host', world=world)) as owner:
        game = CarryRun(owner, revision, saves, current_config, bool(args.descriptor))
        game.call('runtime.start', revision=revision)
        rejected = load(game, error=-32060)
        assert FEATURE in rejected['message'], rejected
    native.ARGS = args
    with closing(native.Session(args.output / 'endpoint-restored', world=world)) as owner:
        game = CarryRun(owner, revision, saves, settings, bool(old.descriptor))
        game.restore('endpoint', *boundary)
        assert game.save('endpoint-restored') == archive
        assert game.outcome() == outcome and arm(game, body) == published
    return dict(outcome=outcome, published=published, prior=prior, rejected=rejected, capture=frame)


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 1000
    native.DEADLINE = time.monotonic() + 280
    config = configuration(args, GAME, FEATURES)
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    saves = args.output / 'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        with closing(native.Session(args.output / 'warm')) as owner:
            schema = owner.rpc('world.describe')['gameplay_item_attachments']
            assert (schema['pickup_blend_feature'], schema['pickup_blend_command_version']) == (FEATURE, 9)
            revision = author(owner, json.loads(manifest.read_text(encoding='utf-8-sig')),
                              wall_scale=(.04,.06,.05), rig_degrees=-65)
            revision = add_pose_observation(owner, revision)
            world = owner.world
            game = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            body, elbow, prior, animation = endpoints(game)
            outcome = blocked(game, body)
            assert outcome['pose'] == 'blended', outcome
            published = arm(game, body)
            assert 0 < published['weight'] < prior['weight'] and published['blocker'] is None, published
            assert stationary_hits(game, published['points'], body) == [None, None]
            assert any(hit and hit['entity'] == WALL for hit in stationary_hits(game, prior['points'], body))
            wrist_keys = ('rotation', 'weight')
            for key in wrist_keys:
                assert published['end_orientation'][key] == prior['end_orientation'][key]
            old_step = math.dist(prior['points'][1], animation[1])
            blend_step = math.dist(prior['points'][1], published['points'][1])
            assert blend_step < old_step
            frame = capture(game, args, 'blended', SIDE_CAMERA)
            boundary = (game.tick, game.structure, game.game_revision)
            archive = game.save('blocked')
            assert payload(archive)['attachment_outcomes'] == dict(version=4, tick=game.tick, outcomes=[outcome])
            recover(game, outcome)
            assert game.state()['ReturnWeight'] == published['weight']
            after = arm(game, body)['end_orientation']
            assert 0 < after['weight'] < published['end_orientation']['weight'], after
            cleared = [1.5, elbow[1], elbow[2]]
            pickup_tick = finish(game, cleared)
            final = game.save('finished')
            invalid = reject_invalid_witness(game, final, field='pose', value='invented',
                                             message='Invalid saved arm fallback pose')
        with closing(native.Session(args.output / 'cold', world=world)) as owner:
            game = CarryRun(owner, revision, saves, config, bool(args.descriptor))
            game.restore('blocked', *boundary)
            assert game.save('cold-blocked') == archive and arm(game, body) == published
            recover(game, outcome)
            assert arm(game, body)['end_orientation'] == after
            assert finish(game, cleared) == pickup_tick
            assert game.save('cold-finished') == final
        retained = compare(args, world, revision, boundary[0], config) if args.previous_binary else None
        if retained:
            assert {k: v for k, v in retained['outcome'].items() if k != 'pose'} == {
                k: v for k, v in outcome.items() if k != 'pose'}
            assert retained['prior'] == prior
        alternatives = other_fallbacks(args, world, revision, config)
        report.update(passed=True, outcome=outcome, blocked_tick=boundary[0], pickup_tick=pickup_tick,
                      prior=prior, published=published, next_wrist=after,
                      endpoint_elbow_step_m=old_step, blend_elbow_step_m=blend_step,
                      retained=retained, invalid_pose=invalid, capture=frame, alternatives=alternatives,
                      exact_archive_pairs=5 + bool(retained))
    finally:
        report.update(calls=len(native.REPORT['calls']), owners=native.REPORT['owners'])
        (args.output / 'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f"Verified blended pickup in {report['calls']} native replies.")


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
        parser.error('Supply a retained endpoint-only artifact with --previous-binary.')
    verify(args)
