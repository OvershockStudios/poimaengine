#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise physical arm blockers outside the player's eye ray."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import time

from carry_props import CarryRun, INITIAL, REQUIRED, author, capture
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ACTOR, CAMERA, ITEM, RIG, WALL
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME = 'Poima.Examples.ClearanceCarryPropsGame'
FEATURE = 'animation_ik_clearance_v1'
NODES = [hashlib.sha256(f'poima.instance.v1/{RIG}/node/{index}'.encode()).hexdigest()[:32]
         for index in (2,1,3)]


def probe(game, goal, ignore, error=None, **changes):
    params = dict(tick=game.tick, structure_revision=game.structure, goal=goal,
                  radii=[.02,.02], ignore=ignore)
    params.update(changes)
    return game.call('runtime.ik.clearance',error,**params)


def solve(game, goal):
    result = game.call('runtime.step',request_id=id_request(),expected_tick=game.tick,
                       expected_structure_revision=game.structure,ticks=1,ik=[goal])
    game.tick += 1
    assert result['tick'] == game.tick


def load(game, error=None):
    receipt = game.call('runtime.gameplay.load_native' if game.native_aot else 'runtime.gameplay.load',
        error,request_id=id_request(),expected_tick=0,expected_structure_revision=0,expected_revision=0,
        values=INITIAL,**game.config)
    if error is None:
        game.game_revision = receipt['revision']
    return receipt


def finish(game, cleared):
    game.move_wall(cleared)
    game.run_ticks(10)
    game.expect_prompt('Pick up')
    game.run_ticks(1,action='pickup')
    for _ in range(35):
        game.run_ticks(1)
        state = game.state()
        if state['Phase'] == 3:
            break
    assert state['Phase'] == 3 and state['Cancellations'] == 1, state
    game.expect_prompt('Drop')
    return int(state['PickupTick'])


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 680
    native.DEADLINE = time.monotonic()+240
    config = configuration(args,GAME,REQUIRED|{FEATURE,'capsule_cast_v1'})
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    owner = native.Session(args.output/'warm')
    world = owner.world
    saves = args.output/'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        discovered = owner.rpc('world.describe',dict(view='method',name='runtime.ik.clearance'))
        schema = discovered['methods']['runtime.ik.clearance']
        assert set(schema['required']) == {'session_id','tick','structure_revision','goal','radii'}
        revision = author(owner,json.loads(manifest.read_text(encoding='utf-8-sig')),
            wall_scale=(.04,.06,.05),rig_degrees=-65)
        game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        game.call('runtime.start',revision=revision)
        load(game)
        game.run_ticks(138)
        game.run_ticks(2,look=-9)
        game.run_ticks(17,move=.77)
        camera = game.matrix(CAMERA)
        target = game.matrix(ITEM)[12:15]
        current_pitch = math.degrees(math.atan2(-camera[9],camera[10]))
        target_pitch = math.degrees(math.atan2(target[1]-camera[13],camera[14]-target[2]))
        game.run_ticks(2,look=target_pitch-current_pitch)
        assert game.visible_item() == ITEM
        body = game.binding()['anchor']
        exclusions = [ACTOR,body]
        rig = game.matrix(RIG)
        pole = [sum(rig[axis*4+k]*weight for axis,weight in enumerate((.4,.15,.02,1)))
                for k in range(3)]
        goal = dict(entity=RIG,slot=1,target=game.matrix(ITEM)[12:15],pole=pole,weight=.65)
        resting = dict(goal,enabled=False,weight=0)
        clear = probe(game,goal,exclusions)
        assert clear['blocker'] is None, clear
        assert clear['current'] == [game.matrix(entity)[12:15] for entity in NODES]
        assert clear['candidate'][1][0] > .06, clear
        solve(game,goal)
        worst = max(math.dist(expected,game.matrix(entity)[12:15])
                    for expected,entity in zip(clear['candidate'],NODES))
        assert worst < 1e-8, worst
        solve(game,resting)
        elbow = clear['candidate'][1]
        forearm = [a*.68+b*.32 for a,b in zip(elbow,clear['candidate'][2])]
        game.move_wall(forearm)
        assert game.visible_item() == ITEM
        lower = probe(game,goal,exclusions)['blocker']
        assert lower and lower['segment'] == 2 and lower['hit']['entity'] == WALL and lower['hit']['fraction'] > 0, lower
        game.move_wall(elbow)
        assert game.visible_item() == ITEM
        assert probe(game,resting,exclusions)['blocker'] is None
        before = game.save('before-probes')
        blocked = probe(game,goal,exclusions)
        witness = blocked['blocker']
        assert witness['hit']['entity'] == WALL and witness['segment'] == 1, blocked
        ignored = probe(game,goal,[*exclusions,WALL])
        assert ignored['blocker'] is None and blocked['candidate'] == ignored['candidate'], (blocked,ignored)
        probe(game,goal,exclusions,error=-32009,tick=game.tick-1)
        probe(game,goal,exclusions,error=-32009,structure_revision=game.structure-1)
        probe(game,goal,exclusions,error=-32602,radii=[0,.02])
        probe(game,goal,[*exclusions,ACTOR],error=-32602)
        probe(game,dict(goal,slot=2),exclusions,error=-32602)
        assert game.save('after-probes') == before
        game.run_ticks(1,action='pickup')
        for _ in range(20):
            if game.state()['Phase'] == 8:
                break
            game.run_ticks(1)
        state = game.state()
        assert state['Phase'] == 8 and state['Cancellations'] == 1 and state['Attempts'] == 0,state
        assert game.binding()['released'] and game.binding()['anchor'] == body
        published = game.call('runtime.ik',tick=game.tick,entity=RIG,slot=1)['state']
        published_goal = {key:published[key] for key in ('entity','slot','target','pole','enabled','space','weight')}
        held_pose = probe(game,published_goal,exclusions)
        assert held_pose['blocker'] is None and all(math.dist(a,b)<1e-8
            for a,b in zip(held_pose['current'],held_pose['candidate'])), held_pose
        game.expect_prompt('Reach blocked')
        blocked_image = capture(game,args,'reach-blocked')
        boundary = (game.tick,game.structure,game.game_revision)
        checkpoint = game.save('blocked-reach')
        cleared = [1.5,elbow[1],elbow[2]]
        pickup_tick = finish(game,cleared)
        recovered_image = capture(game,args,'recovered')
        final = game.save('final')
        owner.close()

        owner = native.Session(args.output/'cold',world=world)
        cold = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('blocked-reach',*boundary)
        assert cold.save('cold-reach') == checkpoint
        assert finish(cold,cleared) == pickup_tick
        assert cold.save('cold-final') == final
        owner.close()
        rejection = None
        if args.previous_binary:
            native.ARGS = copy.copy(args)
            native.ARGS.binary = args.previous_binary
            owner = native.Session(args.output/'previous',world=world)
            old_saves = args.output/'previous-saves'
            old_saves.mkdir()
            old = CarryRun(owner,revision,old_saves,config,bool(args.descriptor))
            old.call('runtime.start',revision=revision)
            rejection = load(old,error=-32060)
            assert FEATURE in rejection['message'],rejection
            owner.close()
        report.update(passed=True,calls=len(native.REPORT['calls']),exact_archive_pairs=3,
            upper_blocker=witness,lower_blocker=lower,
            world_check_residual_m=worst,block_outcome_tick=boundary[0],successful_pickup_tick=pickup_tick,
            previous_host_rejection=rejection,captures=dict(blocked=blocked_image,recovered=recovered_image),
            owners=native.REPORT['owners'])
    finally:
        owner.close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--gpu',type=int)
    options = parser.parse_args()
    if not options.descriptor and not options.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    verify(options)
