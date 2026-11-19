#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate a contact-normal pickup fallback and exact retained save restoration."""

import argparse
from contextlib import closing
import copy
import json
import math
from pathlib import Path
import shutil
import time

from carry_props import CarryRun, author, capture
from final_pickup_clearance import close_in_one_tick, last_reach_sample, recover, reject_invalid_witness
from ik_clearance import finish, load
from item_attachment_release import payload
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ACTOR, ITEM, RIG, SKIN, WALL
from pickup_pose_blend import FEATURES as BLEND_FEATURES, GAME as BLEND_GAME
from pickup_pose_fallback import SIDE_CAMERA, add_pose_observation, arm, blocked, stationary_hits
from runtime_pose_frames import transform, uid
import world_physical_contract as native

GAME = 'Poima.Examples.SteeredFallbackCarryPropsGame'
STAGED_GAME = 'Poima.Examples.StagedRecoveryCarryPropsGame'
ARCING_GAME = 'Poima.Examples.ArcingApproachCarryPropsGame'
HAND_GAME = 'Poima.Examples.HandFallbackCarryPropsGame'
FEATURE = 'item_attachment_pickup_steering_v1'
FEATURES = BLEND_FEATURES | {FEATURE}
HAND_FEATURE = 'item_attachment_pickup_hand_fallback_v1'
PAD_BLOCKER = uid(112)


def wrist_target(game):
    return game.call('runtime.ik', tick=game.tick, entity=RIG, slot=1)['state']['target']


def insert_wall(game, offset=(.035,.012,0), *, arcing=False, pad_blocker=None):
    body, _ = last_reach_sample(game)
    if arcing:
        state = game.state()
        progress = state['TicksInPhase']/state['AimTicks']
        item = game.matrix(ITEM)
        grip_y = item[13] + sum(item[1+4*i]*state['Grip'+axis] for i,axis in enumerate('XYZ'))
        arch = min(progress/.30, (1-progress)/.70)
        lift = .12*arch*arch*(3-2*arch)
        assert lift > 0 and math.isclose(wrist_target(game)[1], grip_y+lift, abs_tol=1e-7)
    before = arm(game, body)
    elbow, wrist = before['points'][1:]
    wall = [a+.7*(b-a)+delta for a,b,delta in zip(elbow,wrist,offset)]
    if pad_blocker is None:
        close_in_one_tick(game, wall)
    else:
        game.run_ticks(1,motions=[
            dict(entity=entity,position=position,rotation=[0,0,0,1],duration_ticks=1)
            for entity,position in [(WALL,wall),(PAD_BLOCKER,pad_blocker)]])
        assert math.dist(game.matrix(WALL)[12:15],wall)<1e-6
        assert math.dist(game.matrix(PAD_BLOCKER)[12:15],pad_blocker)<1e-6
    return body, before, wall


def maximum_step(before, after):
    a, b = before['points'], after['points']
    return max(math.dist([x-o for x,o in zip(a[i],a[0])], [x-o for x,o in zip(b[i],b[0])])
               for i in (1,2))


def pad_points(game):
    pairs = [dict(source=dict(entity=SKIN,vertex=i),target=dict(entity=ITEM)) for i in range(4)]
    result = game.call('runtime.surface.measure',tick=game.tick,structure_revision=game.structure,pairs=pairs)
    return [row['source']['point_m'] for row in result['pairs']]


def pad_overlaps(game, body):
    return [game.call('runtime.capsule_cast',tick=game.tick,position=point,rotation=[0,0,0,1],
                radius=.001,half_height=0,direction=[0,0,-1],distance=0,ignore=[ACTOR,body])['hit']
            for point in pad_points(game)]


def check_hand_obstruction(args, world, revision, saves, config):
    control = copy.copy(args)
    control.descriptor = args.hand_control_descriptor
    old_config = configuration(control,ARCING_GAME,FEATURES)
    with closing(native.Session(args.output/'locate-pad',world=world)) as owner:
        game = CarryRun(owner,revision,saves,old_config,bool(control.descriptor))
        body,_,_ = insert_wall(game,arcing=True)
        assert blocked(game,body)['pose']=='steered'
        blocker = pad_points(game)[3]
    comparison_world = args.output/'pad-obstruction-world.json'
    shutil.copy2(world,comparison_world)
    shutil.copytree(world.with_suffix('.json.assets'),comparison_world.with_suffix('.json.assets'))
    with closing(native.Session(args.output/'pad-obstruction-author',world=comparison_world)) as owner:
        authored = native.transact(owner,[
            dict(op='entity.create',id=PAD_BLOCKER,name='Fingertip test blocker',parent=None),
            native.component(PAD_BLOCKER,'Transform',transform((1.5,1,-.6))),
            native.component(PAD_BLOCKER,'BoxCollider',dict(motion='kinematic',
                half_extents=[.006,.006,.006],mass=.8,friction=.75,restitution=0))],revision)
    results = {}
    for name, module, aot in [('arm',old_config,bool(control.descriptor)),('hand',config,bool(args.descriptor))]:
        with closing(native.Session(args.output/f'pad-obstruction-{name}',world=comparison_world)) as owner:
            game = CarryRun(owner,authored['revision'],saves,module,aot)
            body, before, wall = insert_wall(game,arcing=True,pad_blocker=blocker)
            outcome = blocked(game,body)
            published = arm(game,body)
            assert stationary_hits(game,published['points'],body)==[None,None]
            overlaps = pad_overlaps(game,body)
            results[name] = dict(outcome=outcome,tick=game.tick,start=before,wall=wall,
                overlaps=overlaps,maximum_step_m=maximum_step(before,published))
    arm_only,hand = results['arm'],results['hand']
    assert (arm_only['tick'],arm_only['start'],arm_only['wall'])==(hand['tick'],hand['start'],hand['wall'])
    hit = arm_only['overlaps'][3]
    assert hit and hit['entity']==PAD_BLOCKER and hit['penetration_depth']>.001,arm_only
    assert hand['overlaps']==[None]*4 and hand['outcome']['pose']!='unresolved',hand
    return results


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
    game_type = GAME
    if args.hand_fallback:
        game_type = HAND_GAME
    elif args.arcing_approach:
        game_type = ARCING_GAME
    elif args.staged_recovery:
        game_type = STAGED_GAME
    arcing = args.arcing_approach or args.hand_fallback
    staged = args.staged_recovery or arcing
    features = FEATURES | {HAND_FEATURE} if args.hand_fallback else FEATURES
    config = configuration(args,game_type,features)
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    saves = args.output/'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        with closing(native.Session(args.output/'warm')) as owner:
            schema = owner.rpc('world.describe')['gameplay_item_attachments']
            assert (schema['pickup_steering_feature'],schema['pickup_steering_command_version'])==(FEATURE,10)
            if args.hand_fallback:
                assert (schema['pickup_hand_fallback_feature'],schema['pickup_hand_fallback_command_version'],
                        schema['pickup_hand_radius_m'])==(HAND_FEATURE,11,.001)
            revision = author(owner,json.loads(manifest.read_text(encoding='utf-8-sig')),
                              wall_scale=(.04,.06,.05),rig_degrees=-65)
            revision = add_pose_observation(owner,revision)
            world = owner.world
            game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
            body, before, wall = insert_wall(game,arcing=arcing)
            outcome = blocked(game,body)
            expected_pose = 'blended' if args.hand_fallback else 'steered'
            assert outcome['pose']==expected_pose,outcome
            published = arm(game,body)
            assert published['blocker'] is None,published
            if args.hand_fallback:
                assert 0<published['weight']<before['weight'] and pad_overlaps(game,body)==[None]*4
            else:
                assert published['weight']==before['weight'],published
            assert stationary_hits(game,published['points'],body)==[None,None]
            for key in ('rotation','weight'):
                assert published['end_orientation'][key]==before['end_orientation'][key]
            target = wrist_target(game)
            frame = capture(game,args,'steered',SIDE_CAMERA)
            boundary = (game.tick,game.structure,game.game_revision)
            archive = game.save('blocked')
            outcome_version = 4 if args.hand_fallback else 5
            assert payload(archive)['attachment_outcomes']==dict(version=outcome_version,tick=game.tick,outcomes=[outcome])
            recover(game,outcome)
            assert [game.state()['Target'+axis] for axis in 'XYZ']==target
            clear = [1.5,wall[1],wall[2]]
            wrist_start = published['end_orientation']['weight']
            rest_tick = verify_staged_return(game,clear,wrist_start) if staged else None
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
            if staged:
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
            alternate_body,_,_=insert_wall(game,offset=(.004,.04,0),arcing=arcing)
            alternate=blocked(game,alternate_body)
            assert alternate['pose']=='blended',alternate
            assert stationary_hits(game,arm(game,alternate_body)['points'],alternate_body)==[None,None]
        if args.hand_fallback:
            report['hand_obstruction']=check_hand_obstruction(args,world,revision,saves,config)
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
    parser.add_argument('--hand-control-descriptor',type=Path)
    parser.add_argument('--gpu',type=int)
    approach = parser.add_mutually_exclusive_group()
    approach.add_argument('--staged-recovery',action='store_true')
    approach.add_argument('--arcing-approach',action='store_true')
    approach.add_argument('--hand-fallback',action='store_true')
    args=parser.parse_args()
    if not args.descriptor and not args.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    if args.previous_binary and not (args.comparison_descriptor if args.descriptor
                                     else args.comparison_assembly and args.comparison_bridge):
        parser.error('Supply a retained blended pickup artifact with --previous-binary.')
    if args.hand_fallback and args.descriptor and not args.hand_control_descriptor:
        parser.error('Supply the arcing approach descriptor with --hand-control-descriptor.')
    verify(args)
