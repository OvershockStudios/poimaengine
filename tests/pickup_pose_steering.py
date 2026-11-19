#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate a contact-normal pickup fallback and exact retained save restoration."""

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
from item_reach_pickup import RIG, WALL
from pickup_pose_blend import FEATURES as BLEND_FEATURES, GAME as BLEND_GAME
from pickup_pose_fallback import SIDE_CAMERA, add_pose_observation, arm, blocked, stationary_hits
import world_physical_contract as native

GAME = 'Poima.Examples.SteeredFallbackCarryPropsGame'
STAGED_GAME = 'Poima.Examples.StagedRecoveryCarryPropsGame'
FEATURE = 'item_attachment_pickup_steering_v1'
FEATURES = BLEND_FEATURES | {FEATURE}


def wrist_target(game):
    return game.call('runtime.ik', tick=game.tick, entity=RIG, slot=1)['state']['target']


def insert_wall(game, offset=(.035,.012,0)):
    body, _ = last_reach_sample(game)
    before = arm(game, body)
    elbow, wrist = before['points'][1:]
    wall = [a+.7*(b-a)+delta for a,b,delta in zip(elbow,wrist,offset)]
    close_in_one_tick(game, wall)
    return body, before, wall


def maximum_step(before, after):
    a, b = before['points'], after['points']
    return max(math.dist([x-o for x,o in zip(a[i],a[0])], [x-o for x,o in zip(b[i],b[0])])
               for i in (1,2))


def verify_staged_return(game, clear, wrist_start):
    game.move_wall(clear)
    duration = game.state()['ReturnTicks']
    saw_wrist_only = False
    for _ in range(math.ceil(duration*1.5)+2):
        state = game.state()
        pose = game.call('runtime.ik', tick=game.tick, entity=RIG, slot=1,
                         include_end_orientation=True)['state']
        if state['TicksInPhase']==duration:
            assert state['Phase']==8 and pose['enabled'] and pose['weight']==0, (state, pose)
            assert math.isclose(pose['end_orientation']['weight'], wrist_start*.5, abs_tol=1e-12), pose
            saw_wrist_only = True
        if state['Phase']==0:
            assert saw_wrist_only and state['TicksInPhase']==math.ceil(duration*1.5), state
            assert not pose['enabled'] and pose['end_orientation'] is None, pose
            return game.tick
        game.run_ticks(1)
    raise AssertionError('Staged return did not finish after the obstacle cleared.')


def retained_run(args, world, revision, current_config, tick):
    previous = copy.copy(args)
    previous.binary = args.previous_binary
    previous.descriptor = args.comparison_descriptor
    if not previous.descriptor:
        previous.assembly, previous.bridge = args.comparison_assembly, args.comparison_bridge
    native.ARGS = previous
    config = configuration(previous,BLEND_GAME,BLEND_FEATURES)
    saves = args.output / 'blended-saves'
    saves.mkdir()
    with closing(native.Session(args.output/'retained-blend',world=world)) as owner:
        game = CarryRun(owner,revision,saves,config,bool(previous.descriptor))
        body, before, wall = insert_wall(game)
        outcome = blocked(game,body)
        assert game.tick==tick and outcome['pose']=='blended',outcome
        published = arm(game,body)
        # Compare the committed endpoints. Re-solving a saved goal can differ
        # by a rounding unit and turn an endpoint query into an inflated sweep.
        assert stationary_hits(game,published['points'],body)==[None,None],published
        archive = game.save('retained')
        boundary = (game.tick,game.structure,game.game_revision)
        assert payload(archive)['attachment_outcomes']['version']==4
        frame = capture(game,args,'retained',SIDE_CAMERA)
    with closing(native.Session(args.output/'previous-host',world=world)) as owner:
        game = CarryRun(owner,revision,saves,current_config,bool(args.descriptor))
        game.call('runtime.start',revision=revision)
        error = load(game,error=-32060)
        assert FEATURE in error['message'],error

    native.ARGS = args
    with closing(native.Session(args.output/'retained-restored',world=world)) as owner:
        game = CarryRun(owner,revision,saves,config,bool(previous.descriptor))
        game.restore('retained',*boundary)
        assert game.save('retained-restored')==archive
        assert game.outcome()==outcome and arm(game,body)==published
    return dict(before=before,wall=wall,outcome=outcome,published=published,
                maximum_step_m=maximum_step(before,published),rejection=error,capture=frame)


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 750
    native.DEADLINE = time.monotonic()+280
    config = configuration(args,STAGED_GAME if args.staged_recovery else GAME,FEATURES)
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    saves = args.output/'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        with closing(native.Session(args.output/'warm')) as owner:
            schema = owner.rpc('world.describe')['gameplay_item_attachments']
            assert (schema['pickup_steering_feature'],schema['pickup_steering_command_version'])==(FEATURE,10)
            revision = author(owner,json.loads(manifest.read_text(encoding='utf-8-sig')),
                              wall_scale=(.04,.06,.05),rig_degrees=-65)
            revision = add_pose_observation(owner,revision)
            world = owner.world
            game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
            body, before, wall = insert_wall(game)
            outcome = blocked(game,body)
            assert outcome['pose']=='steered',outcome
            published = arm(game,body)
            assert published['blocker'] is None and published['weight']==before['weight'],published
            assert stationary_hits(game,published['points'],body)==[None,None]
            for key in ('rotation','weight'):
                assert published['end_orientation'][key]==before['end_orientation'][key]
            target = wrist_target(game)
            frame = capture(game,args,'steered',SIDE_CAMERA)
            boundary = (game.tick,game.structure,game.game_revision)
            archive = game.save('blocked')
            assert payload(archive)['attachment_outcomes']==dict(version=5,tick=game.tick,outcomes=[outcome])
            recover(game,outcome)
            assert [game.state()['Target'+axis] for axis in 'XYZ']==target
            clear = [1.5,wall[1],wall[2]]
            wrist_start = published['end_orientation']['weight']
            rest_tick = verify_staged_return(game,clear,wrist_start) if args.staged_recovery else None
            retry = finish(game,clear)
            game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=3,id=body)
            final = game.save('final')
            invalid = reject_invalid_witness(game,final,field='pose',value='invented',
                                             message='Invalid saved arm fallback pose')
        with closing(native.Session(args.output/'cold',world=world)) as owner:
            game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
            game.restore('blocked',*boundary)
            assert game.save('cold-blocked')==archive and arm(game,body)==published
            recover(game,outcome)
            if args.staged_recovery:
                assert verify_staged_return(game,clear,wrist_start)==rest_tick
            assert finish(game,clear)==retry and game.save('cold-final')==final
        retained = retained_run(args,world,revision,config,boundary[0]) if args.previous_binary else None
        selected_step = maximum_step(before,published)
        if retained:
            assert retained['before']==before and retained['wall']==wall
            assert selected_step<retained['maximum_step_m'],(selected_step,retained['maximum_step_m'])
        alternate_saves=args.output/'alternate-saves'
        alternate_saves.mkdir()
        with closing(native.Session(args.output/'alternate',world=world)) as owner:
            game=CarryRun(owner,revision,alternate_saves,config,bool(args.descriptor))
            alternate_body,_,_=insert_wall(game,offset=(.004,.04,0))
            alternate=blocked(game,alternate_body)
            assert alternate['pose']=='blended',alternate
            assert stationary_hits(game,arm(game,alternate_body)['points'],alternate_body)==[None,None]
        report.update(passed=True,outcome=outcome,published=published,blocked_tick=boundary[0],
                      retry_tick=retry,staged_rest_tick=rest_tick,selected_step_m=selected_step,retained=retained,
                      invalid_pose=invalid,capture=frame,alternate=alternate,exact_archive_pairs=3+bool(retained))
    finally:
        report.update(calls=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f"Verified pickup steering in {report['calls']} native replies.")


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--comparison-descriptor',type=Path)
    parser.add_argument('--comparison-assembly',type=Path)
    parser.add_argument('--comparison-bridge',type=Path)
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--staged-recovery',action='store_true')
    args=parser.parse_args()
    if not args.descriptor and not args.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    if args.previous_binary and not (args.comparison_descriptor if args.descriptor
                                     else args.comparison_assembly and args.comparison_bridge):
        parser.error('Supply a retained blended pickup artifact with --previous-binary.')
    verify(args)
