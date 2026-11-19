#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify physical occlusion, release-proxy identity and compiled aimed pickup."""

import argparse
import copy
import json
from pathlib import Path
import time

from item_attachment_gameplay import Run, sha
from item_attachment_pickup import APPLY, apply_params
from item_attachment_release import body_state, release_params
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ACTOR, CAMERA, FAR, FEATURES, HAND, ITEM, PROXY, WALL, advance, configure, settings
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME = 'Poima.Examples.AimedReachPickupGame'
FEATURE = 'item_attachment_aim_v1'
METHOD = 'runtime.attachment.aim'
EMPTY = '0'*32


def aim(game, origin, direction, ignored=None, error=None, **updates):
    params = dict(tick=game.tick, structure_revision=game.structure, origin=origin,
                  direction=direction, distance=2, ignore=[ACTOR, *(ignored or [])])
    params.update(updates)
    return game.call(METHOD,error=error,**params)


def load(game, error=None):
    return game.call('runtime.gameplay.load_native' if game.native_aot else 'runtime.gameplay.load',error=error,
        request_id=id_request(),expected_tick=game.tick,expected_structure_revision=game.structure,
        expected_revision=0,values=dict(settings(),Item=EMPTY),**game.config)


def capture(game,args,stage):
    if args.gpu is None:return None
    path=args.output/(stage+'.bmp')
    result=game.call('runtime.capture',tick=game.tick,structure_revision=game.structure,camera=CAMERA,
        path=native.native(path),width=640,height=360,samples=1,gpu=args.gpu)
    assert result['hardware'] and result['capture_written'] and result['nvrhi_errors']==0
    return dict(tick=result['tick'],gpu=result['gpu'],sha256=sha(path))


def shift_wall(game,x):
    result=game.call('runtime.step',request_id=id_request(),expected_tick=game.tick,expected_structure_revision=4,
        ticks=6,motions=[dict(entity=WALL,position=[x,1.05,-.36],rotation=[0,0,0,1],duration_ticks=6)])
    game.tick+=6
    assert result['tick']==game.tick


def drive(game,args):
    game.call('runtime.start',revision=game.revision)
    proxies={}
    for item in (ITEM,FAR):
        held=game.owner.rpc(APPLY,apply_params(game,dict(entity=item,anchor=CAMERA,mode='keep_world')))
        game.structure=held['structure_revision']
        release=game.owner.rpc('runtime.attachment.release',release_params(game,item=item,template=PROXY,
            linear=(0,0,0),angular=(0,0,0)))
        game.structure=release['structure_revision']
        proxies[item]=release['body']
    result=game.call('runtime.step',request_id=id_request(),expected_tick=0,expected_structure_revision=4,
        ticks=140,inputs=[dict(entity=ACTOR,look=[0,-9])])
    game.tick=140
    assert result['tick']==game.tick
    before=game.save('resting')
    assert all(not body_state(before,x)['active'] for x in proxies.values())
    origin=game.matrix(CAMERA)[12:15]
    direction=[-x for x in game.matrix(CAMERA)[8:11]]
    blocked=aim(game,origin,direction)
    assert blocked['hit']['entity']==WALL and blocked['item'] is None,blocked
    assert blocked['hit']==game.call('runtime.raycast',tick=game.tick,origin=origin,direction=direction,
        distance=2,ignore=[ACTOR])['hit']
    near=aim(game,origin,direction,[WALL])
    far=aim(game,origin,direction,[WALL,proxies[ITEM]])
    assert near['item']==ITEM and near['hit']['entity']==proxies[ITEM],near
    assert far['item']==FAR and far['hit']['entity']==proxies[FAR] and far['hit']['distance']>near['hit']['distance'],far
    assert aim(game,origin,[0,1,0])['hit'] is None
    aim(game,origin,direction,error=-32009,structure_revision=3)
    aim(game,origin,direction,error=-32009,tick=139)
    aim(game,origin,[0,0,0],error=-32602)
    aim(game,origin,direction,error=-32602,ignore=[ACTOR,ACTOR])
    images=dict(blocked=capture(game,args,'blocked'))
    assert game.save('after-query')==before
    game.game_revision=load(game)['revision']
    frames=[]
    advance(game,1,frames)
    assert frames[-1]['Phase']==0 and frames[-1]['Item']==EMPTY and frames[-1]['Attempts']==0
    shift_wall(game,1)
    assert aim(game,origin,direction)['item']==ITEM
    images['clear']=capture(game,args,'clear')
    advance(game,1,frames)
    assert frames[-1]['Phase']==1 and frames[-1]['Item']==ITEM
    shift_wall(game,0)
    game.step(1)
    during=game.game()['values']
    assert during['Phase']==2 and during['Cancellations']==1 and during['Attempts']==0
    assert all(row['released'] for row in game.bindings())
    shift_wall(game,1)
    game.step(8)
    assert game.game()['values']['Phase']==0
    for index in range(1,58):
        advance(game,index,frames)
        if index==3:
            assert frames[-1]['Phase']==1 and frames[-1]['Item']==ITEM
            checkpoint_tick=game.tick
            checkpoint=game.save('reach')
        if index==17:
            assert frames[-1]['Phase']==0 and frames[-1]['Cancellations']==2 and frames[-1]['Attempts']==0
            assert all(row['released'] for row in game.bindings())
    tail=frames[-1]
    assert tail['Phase']==3 and tail['Item']==ITEM and tail['Attempts']==tail['Misses']+1
    assert game.structure==5
    game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=5,id=proxies[ITEM])
    after=aim(game,origin,direction)
    assert after['item']==FAR and after['hit']['entity']==proxies[FAR],after
    assert [(r['entity'],r['anchor']) for r in game.bindings()]==[(ITEM,HAND),(FAR,proxies[FAR])]
    images['held']=capture(game,args,'held')
    if args.gpu is not None:
        assert len({value['sha256'] for value in images.values()})==3
    result=dict(acquired=ITEM,picked_tick=int(tail['PickupTick']),misses=tail['Misses'],canceled_reaches=tail['Cancellations'],
        blocked_hit=blocked['hit'],nearest_hit=near['hit'],second_hit=far['hit'],after_retirement=after['hit'],
        captures=images)
    return checkpoint_tick,checkpoint,game.save('final'),result


def verify(args):
    root=args.output.resolve()
    args.output=root
    root.mkdir()
    native.ARGS=args;native.MAX_CALLS=650;native.DEADLINE=time.monotonic()+180
    config=configuration(args,GAME,FEATURES|{FEATURE})
    owner=native.Session(root/'warm')
    world=owner.world
    saves=root/'saves';saves.mkdir()
    evidence=dict(passed=False)
    try:
        revision=configure(owner, aimed=True)
        warm=Run(owner,revision,saves,config,bool(args.descriptor))
        tick,checkpoint,final,selection=drive(warm,args)
        owner.close()
        owner=native.Session(root/'cold',world=world)
        cold=Run(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('reach',tick,4,1)
        assert cold.save('cold-reach')==checkpoint
        for index in range(4,58):advance(cold,index,None)
        assert cold.save('cold-final')==final
        owner.close()
        if args.previous_binary:
            native.ARGS=copy.copy(args)
            native.ARGS.binary=args.previous_binary
            owner=native.Session(root/'previous',world=world)
            retired=root/'previous-saves';retired.mkdir()
            previous=Run(owner,revision,retired,config,bool(args.descriptor))
            previous.call('runtime.start',revision=revision)
            refusal=load(previous,error=-32060)
            assert FEATURE in refusal['message'],refusal
            evidence['previous_host_rejection']=refusal
            owner.close()
            native.ARGS=args
        evidence.update(passed=True,exact_archive_pairs=3,selection=selection)
    finally:
        owner.close()
        evidence.update(owners=native.REPORT['owners'],calls=len(native.REPORT['calls']))
        (root/'report.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--gpu',type=int)
    verify(parser.parse_args())
