#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the compiled reach example against an original skinned contact fixture."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import time

from gltf_fixture import glb
from item_attachment_gameplay import Run
from item_attachment_pickup import APPLY, apply_params
from item_attachment_release import body_state, release_params
from item_attachment_surface_gameplay import configuration, parameters
from runtime_pose_frames import id_request, transform, uid
from runtime_surface_measure import accessor
import world_physical_contract as native

CAMERA, ACTOR, RIG, ITEM, SUPPORT, FLOOR, PROXY = map(uid, (20, 21, 22, 23, 24, 25, 26))
HAND = hashlib.sha256(f'poima.instance.v1/{RIG}/node/3'.encode()).hexdigest()[:32]
SKIN = hashlib.sha256(f'poima.instance.v1/{RIG}/node/0/primitive/0'.encode()).hexdigest()[:32]
GAME = 'Poima.Examples.ReachPickupGame'
FEATURES = {'baseline_v7', 'animation_ik_end_orientation_v1', 'animation_ik_v1', 'animation_inertial_v1',
            'animation_layers_v1', 'input_actions_v1', 'item_attachment_contact_v1', 'item_attachment_pickup_v1',
            'item_attachment_recovery_v1', 'item_attachment_surface_v1', 'item_attachment_v1'}


def panel(owner, skinned):
    doc = dict(asset=dict(version='2.0'), buffers=[{}], bufferViews=[], accessors=[])
    blob = bytearray()

    def pack(values, shape, count, integer=False):
        code = 'H' if integer else 'f'
        return accessor(doc, blob, struct.pack('<' + code*len(values), *values), shape, 5123 if integer else 5126, count)

    width, height, y = (.028, .035, .4) if skinned else (.11, .08, 0.)
    p = pack((-width,y-height,0,width,y-height,0,width,y+height,0,-width,y+height,0), 'VEC3', 4)
    doc['accessors'][p].update(min=[-width,y-height,0], max=[width,y+height,0])
    attrs = dict(POSITION=p, NORMAL=pack((0,0,1)*4, 'VEC3', 4))
    doc['meshes'] = [dict(primitives=[dict(attributes=attrs, indices=pack((0,1,2,0,2,3), 'SCALAR', 6, True))])]
    doc['nodes'] = [dict(name='Contact panel', mesh=0)]
    roots = [0]
    if skinned:
        attrs.update(JOINTS_0=pack((0,0,0,0)*4, 'VEC4', 4, True), WEIGHTS_0=pack((1,0,0,0)*4, 'VEC4', 4))
        doc['nodes'][0]['skin'] = 0
        doc['nodes'] += [dict(name='Elbow', translation=[0,.2,0], children=[3]), dict(name='Shoulder', children=[1]),
                         dict(name='Wrist', translation=[0,.2,0], children=[4]), dict(name='Finger')]
        roots += [2]
        bind = pack((1,0,0,0,0,1,0,0,0,0,1,0,0,-.4,0,1), 'MAT4', 1)
        doc['skins'] = [dict(joints=[4], inverseBindMatrices=bind)]
        times = pack((0,.2), 'SCALAR', 2)
        doc['accessors'][times].update(min=[0],max=[.2])
        translation = pack((0,0,0,0,0,.06), 'VEC3', 2)
        doc['animations'] = [dict(name='Open contact points', samplers=[dict(input=times,output=translation,interpolation='LINEAR')],
                                  channels=[dict(sampler=0,target=dict(node=4,path='translation'))])]
    doc['scenes'] = [dict(nodes=roots)]
    doc['buffers'][0]['byteLength'] = len(blob)
    source = owner.directory / ('hand.glb' if skinned else 'item.glb')
    source.write_bytes(glb(doc, blob))
    return owner.rpc('asset.import',dict(source=native.native(source)))['asset']


def configure(owner):
    arm, prop = panel(owner, True), panel(owner, False)
    lens = dict(vertical_fov=60,near=.02,far=20)
    body = dict(motion='static',mass=.8,friction=.75,restitution=0)
    ops = []
    for key, name, parent, components in (
        (ACTOR,'Player',None,dict(Transform=transform((0,.001,0)),CharacterController=dict(
            camera=CAMERA,radius=.20,height=1.60,speed=1.2,jump_speed=4))),
        (CAMERA,'Eye',ACTOR,dict(Transform=transform((0,1.12,0)),Camera=dict(lens,viewmodel=lens,viewmodel_world_depth=True))),
        (ITEM,'Resting contact panel',CAMERA,dict(Transform=transform((.12,-.045,-.60)),
            CameraDraw=dict(camera=CAMERA,mode='viewmodel'),StaticMesh=dict(asset=prop,primitive=0,visible=True))),
        (SUPPORT,'Physical shelf',None,dict(Transform=transform((.12,.951,-.60)),
            BoxCollider=dict(body,half_extents=[.20,.05,.09]))),
        (FLOOR,'Physical floor',None,dict(Transform=transform((0,-.4,0)),BoxCollider=dict(body,half_extents=[10,.4,10]))),
    ):
        ops.append(dict(op='entity.create',id=key,name=name,parent=parent))
        ops.extend(native.component(key,kind,value) for kind,value in components.items())
    ops.extend([dict(op='asset.instantiate',id=RIG,name='Analytic reach rig',asset=arm,parent=CAMERA),
        native.component(RIG,'AnimationRig',dict(asset=arm,clip=None,time=0,speed=1,loop=False,playing=False,
            ik=[dict(slot=1,root=2,middle=1,end=3,target=[0,1,0],pole=[1,1,0],enabled=False,space='world',weight=0)],
            layers=[dict(slot=1,mode='override',mask=[dict(node=4,weight=1)],clip=0,time=0,
                         speed=1,weight=1,loop=False,playing=False)])),
        native.component(SKIN,'CameraDraw',dict(camera=CAMERA,mode='viewmodel')),
        dict(op='template.set',id=PROXY,name='Panel proxy',components=dict(Transform=transform(),
            BoxCollider=dict(body,motion='dynamic',half_extents=[.11,.024,.045])))])
    return native.transact(owner,ops,0)['revision']


def advance(game,index,frames):
    key = {1:'pickup',4:'cancel_pickup',20:'pickup'}.get(index)
    action = [dict(name=key,held=False,pressed=True,released=True)] if key else []
    move = .77 if 20<=index<=40 else 0.
    step = game.call('runtime.step',request_id=id_request(),expected_tick=game.tick,
        expected_structure_revision=game.structure,ticks=1,inputs=[dict(entity=ACTOR,move=[0,move],actions=action)])
    game.tick+=1
    assert step['tick']==game.tick
    game.structure=game.owner.rpc('runtime.status')['structure_revision']
    if frames is not None:
        frames.append(game.game()['values'])


def verify(args):
    root=args.output.resolve()
    root.mkdir()
    native.ARGS=args;native.MAX_CALLS=600;native.DEADLINE=time.monotonic()+150
    config=configuration(args,GAME,FEATURES)
    owner=native.Session(root/'warm')
    world=owner.world
    saves=root/'saves';saves.mkdir()
    evidence=dict(passed=False)
    try:
        revision=configure(owner)
        game=Run(owner,revision,saves,config,bool(args.descriptor))
        game.call('runtime.start',revision=revision)
        game.structure=owner.rpc(APPLY,apply_params(game,dict(entity=ITEM,anchor=CAMERA,mode='keep_world')))['structure_revision']
        drop=owner.rpc('runtime.attachment.release',release_params(game,item=ITEM,template=PROXY,
            linear=(0,0,0),angular=(0,0,0)))
        game.structure=drop['structure_revision']
        game.step(140)
        resting=body_state(game.save('resting'),drop['body'])
        assert not resting['active'] and resting['linear_velocity']==[0,0,0] and 1.01<resting['position'][1]<1.04
        values=dict(Actor=ACTOR,Rig=RIG,Hand=HAND,Item=ITEM,FingerMesh=SKIN,Vertex1=0,Vertex2=1,Vertex3=2,Vertex4=3,
            AimTicks=12,RetryTicks=18,ReturnTicks=8,FingerLayer=1,OpenClip=0,OpenTime=.2,GripQw=1,
            PoleX=.4,PoleY=.15,PoleZ=.02,FrameLimit=.006,AngleLimit=5,SurfaceLimit=.005)
        loaded=game.call('runtime.gameplay.load_native' if args.descriptor else 'runtime.gameplay.load',
            request_id=id_request(),expected_tick=game.tick,expected_structure_revision=game.structure,
            expected_revision=0,values=values,**config)
        game.game_revision=loaded['revision']
        frames=[]
        for index in range(1,58):
            advance(game,index,frames)
            if index==3:
                assert frames[-1]['Phase']==1 and frames[-1]['Opening']>0 and frames[-1]['Attempts']==0
                initial_tick=game.tick
                initial=game.save('reach')
            if index==17:
                assert frames[-1]['Phase']==0 and frames[-1]['Cancellations']==1 and frames[-1]['Attempts']==0
                assert game.bindings()[0]['anchor']==drop['body'] and game.bindings()[0]['released']
        values=frames[-1]
        assert values['Cancellations']==1 and values['Phase']==3 and values['Misses']>=1
        assert values['Attempts']==values['Misses']+1 and game.structure==3
        game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=3,id=drop['body'])
        assert game.bindings()[0]['anchor']==HAND and 'released' not in game.bindings()[0]
        pairs=[dict(source=dict(entity=SKIN,vertex=i),target=dict(entity=ITEM)) for i in range(4)]
        grip=game.call('runtime.surface.measure',tick=game.tick,structure_revision=3,pairs=pairs)['pairs']
        assert max(p['distance_m'] for p in grip)<.005
        final=game.save('final')
        owner.close()
        owner=native.Session(root/'cold',world=world)
        cold=Run(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('reach',initial_tick,2,1)
        assert cold.save('cold-reach')==initial
        for index in range(4,58):advance(cold,index,None)
        assert cold.save('cold-final')==final
        owner.close()
        evidence.update(passed=True, exact_archives=2, pickup_tick=int(values['PickupTick']),
            attempts=values['Attempts'], misses=values['Misses'], canceled=values['Cancellations'],
            final_grip_m=[p['distance_m'] for p in grip], resting=resting)
    finally:
        owner.close()
        evidence.update(owners=native.REPORT['owners'],calls=len(native.REPORT['calls']))
        (root/'report.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    verify(parser.parse_args())
