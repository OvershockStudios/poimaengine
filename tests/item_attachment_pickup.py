#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify sampled pickup and removal of a real dynamic release body."""
import argparse
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
import time

from character_library_intake import multiply
from item_attachment_gameplay import Run, sha
from item_attachment_recovery_gameplay import hold, outcomes
from item_attachment_release import capture, payload, release_params
from item_attachment_surface_gameplay import FEATURES as SURFACE_FEATURES, check_previous, configuration, load, parameters
from runtime_attachment import CAMERA, close_matrix
from runtime_pose_frames import id_request, transform, uid
from runtime_surface_measure import geometry, MANY, probe, SKIN, TARGET
import world_physical_contract as native

ITEM, START, WITNESS, BOX, FLOOR, OBSERVER, ACTOR = map(uid, (61,62,63,89,90,91,92))
HOLDER=native.node(1)
GAME='Poima.Tests.AttachmentPickupGame'
FEATURE='item_attachment_pickup_v1'
FEATURES=SURFACE_FEATURES|{'input_actions_v1','item_attachment_recovery_v1','item_attachment_release_v1',FEATURE}
INITIAL=dict(Item=ITEM,Holder=HOLDER,Source=SKIN,Target=TARGET,Actor=ACTOR,TemplateLow=str(int(BOX,16)),
             BlockedLimit=.02,PickupLimit=.3,FrameLimit=.6,DropAt=1,MissAt=22,SaveMiss=1,SavePickup=1)
INPUT=[dict(entity=ACTOR,actions=[dict(name='pickup',held=False,pressed=True,released=True)])]
APPLY='runtime.attachment.apply'


def configure(owner):
    revision=geometry(owner)
    ops=[]
    for identity,label in ((ITEM,'Released prop'),(START,'Starting hand'),(WITNESS,'Proxy dependent')):
        ops.extend([dict(op='entity.create',id=identity,name=label),
                    native.component(identity,'Transform',transform())])
    lens=dict(vertical_fov=60,near=.01,far=40)
    for identity, label, components in (
            (CAMERA,'Player',dict(Transform=transform((0,0,4)),Camera=dict(lens,viewmodel=lens))),
            (OBSERVER,'Observer',dict(Transform=transform((0,0,4)),Camera=lens)),
            (FLOOR,'Drop test floor',dict(Transform=transform((0,-.60,0)),
                BoxCollider=dict(motion='static',half_extents=[20,.25,20],mass=1,friction=.75,restitution=0))),
            (ACTOR,'Input actor',dict(Transform=transform((15,0,0)),
                CharacterController=dict(radius=.25,height=1.7,speed=4,jump_speed=5,camera=None)))):
        ops.append(dict(op='entity.create',id=identity,name=label))
        ops.extend(native.component(identity,kind,value) for kind,value in components.items())
    ops.extend([dict(op='entity.reparent',id=ITEM,parent=CAMERA,mode='keep_local'),
                native.component(ITEM,'Transform',transform((0,0,-4))),
                dict(op='entity.reparent',id=TARGET,parent=ITEM,mode='keep_local'),
                native.component(TARGET,'CameraDraw',dict(camera=CAMERA,mode='viewmodel')),
                native.component(TARGET,'PbrMaterial',dict(base_color=[.8,.22,.05],metallic=0,roughness=.7,
                    emissive=[0,0,0],double_sided=True)),
                dict(op='entity.delete',id=MANY,recursive=False),
                dict(op='template.set',id=BOX,name='Temporary pickup collision',components=dict(
                    Transform=transform(),BoxCollider=dict(motion='dynamic',half_extents=[.25,.18,.1],
                        mass=.8,friction=.75,restitution=0)))])
    return native.transact(owner,ops,revision)['revision']


def selector(limit):
    sample=probe(triangle=3,weights=(.25,.5,.25),distance_limit_m=limit)
    sample['target']['triangles']=[2,3]
    return sample


def pickup(limit=.3,anchor=HOLDER):
    return dict(entity=ITEM,mode='pickup',anchor=anchor,
                contact=dict(item_offset=dict(position=[0,1,0],rotation=[0,0,0,1]),position_limit_m=.6,
                             rotation_limit_deg=5,surface=[selector(limit)]))


def apply_params(game,*commands):
    return dict(session_id=game.id,request_id=id_request(),expected_tick=game.tick,
        expected_structure_revision=game.structure,commands=list(commands))


def restore_unmanaged(game,slot,tick,structure):
    result=game.owner.rpc('save.load',dict(request_id=id_request(),configuration_generation=1,slot=slot,
        expected_generation=1,revision=game.revision,expected_session_id=None,expected_tick=None,
        expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=game.id))
    assert result['tick']==tick
    game.tick=tick;game.structure=structure


def cast_body(game,position):
    x,y,z=position
    return game.call('runtime.raycast',tick=game.tick,
        origin=[x,y,z+2],direction=[0,0,-1],distance=4)['hit']


def assert_gone(game,body,position):
    game.call('runtime.entity',error=-32004,id=body,tick=game.tick,structure_revision=game.structure)
    assert cast_body(game,position) is None
    row=next(r for r in game.bindings() if r['entity']==ITEM)
    assert row['anchor']==HOLDER and 'released' not in row


def direct_pickup(game,args,out,body,expected,phase):
    item=game.matrix(ITEM)
    other=game.matrix(WITNESS)
    position=game.matrix(body)[12:15]
    assert cast_body(game,position)['entity']==body
    frames={'before':capture(game,args,out,phase+'-dropped',OBSERVER)}
    params=apply_params(game,pickup(),dict(entity=WITNESS,mode='keep_world',anchor=HOLDER))
    if expected is not None:
        params['request_id']=expected
    answer=game.owner.rpc(APPLY,params)
    game.structure+=1
    assert answer['structure_revision']==game.structure and answer['tick']==game.tick and not answer['replayed']
    assert game.owner.rpc(APPLY,params)==dict(answer,replayed=True)
    assert_gone(game,body,position)
    close_matrix(game.matrix(ITEM),item)
    close_matrix(game.matrix(WITNESS),other)
    frames['world']=capture(game,args,out,phase+'-picked-world',OBSERVER)
    frames['eye']=capture(game,args,out,phase+'-picked-eye',CAMERA)
    if args.gpu is not None:
        assert frames['before']['objects']==frames['world']['objects']+1 and frames['eye']['foreground']==1
        assert frames['before']['sha256']!=frames['world']['sha256']
    return game.save(phase+'-picked'),frames


def run_direct(args,out,world,revision):
    saves=out/'direct-saves';saves.mkdir()
    owner=native.Session(out/'direct',world=world)
    try:
        game=Run(owner,revision,saves,{})
        game.call('runtime.start',revision=revision)
        hold(game,ITEM,START)
        owner.rpc(APPLY,apply_params(game,pickup()),error=-32602)
        receipt=owner.rpc('runtime.attachment.release',release_params(game,item=ITEM,template=BOX,linear=(.3,0,0),angular=(0,0,0)))
        game.structure=receipt['structure_revision']
        body=receipt['body']
        game.step(22)
        hold(game,WITNESS,body)
        before=game.save('direct-before')
        shape=payload(before)['physical']['physics']
        assert shape['pairs'] or not next(row['body']['active'] for row in payload(before)['entities'] if row['id']==body)
        observed=game.call('runtime.surface.measure',tick=game.tick,structure_revision=game.structure,
                           pairs=[selector(.02)])['pairs'][0]
        assert .09<observed['distance_m']<.3,observed
        missed=apply_params(game,pickup(.02))
        row=owner.rpc(APPLY,missed,error=-32040)
        assert abs(row['data']['surface']['distance_m']-observed['distance_m'])<1e-8
        protected=owner.rpc(APPLY,apply_params(game,pickup()),error=-32602)
        assert 'anchor must remain live' in protected['message'],protected
        owner.rpc(APPLY,apply_params(game,pickup(anchor=body)),error=-32602)
        invalid=pickup()
        del invalid['contact']['surface']
        owner.rpc(APPLY,apply_params(game,invalid),error=-32602)
        assert game.save('direct-after-errors')==before
        picked,frames=direct_pickup(game,args,out,body,missed['request_id'],'direct')
        assert 'physical' not in payload(picked) and not payload(picked)['structure']['spawned']
        owner.close()

        owner=native.Session(out/'direct-cold',world=world)
        game=Run(owner,revision,saves,{})
        restore_unmanaged(game,'direct-before',22,3)
        assert game.save('direct-restored')==before
        assert direct_pickup(game,args,out,body,None,'direct-cold')==(picked,frames)
        owner.close()
        return dict(gap_m=observed['distance_m'],retired=body,exact_archives=3,frames=frames)
    finally:
        owner.close()


def pickup_input(game,ticks=1,error=None):
    result=game.call('runtime.step',error=error,request_id=id_request(),expected_tick=game.tick,
        expected_structure_revision=game.structure,ticks=ticks,inputs=INPUT)
    if error is None:
        game.tick+=ticks;game.structure+=1
        status=game.owner.rpc('runtime.status')
        assert result['tick']==game.tick and status['structure_revision']==game.structure,(result,status)
    return result


def compiled_resume(game,args,out,phase,blocked,body):
    assert game.tick==22 and game.structure==2 and outcomes(game)==[blocked]
    position=game.matrix(body)[12:15]
    before=capture(game,args,out,phase+'-dropped',OBSERVER)
    assert cast_body(game,position)['entity']==body

    game.edit(PinnedBody=body)
    strong=game.save(phase+'-before-strong')
    error=pickup_input(game,error=-32040)
    assert 'PinnedBody' in error['message'],error
    assert game.save(phase+'-after-strong')==strong and outcomes(game)==[blocked]
    game.edit(PinnedBody='0'*32,BadAt=24)
    prior=game.save(phase+'-before-batch')
    error=pickup_input(game,2,-32040)
    assert 'active released item' in error['message'],error
    assert game.save(phase+'-after-batch')==prior and outcomes(game)==[blocked]
    assert not (game.saves/'slot-pickup-complete').exists()
    assert cast_body(game,position)['entity']==body

    game.edit(BadAt=0)
    pickup_input(game)
    assert_gone(game,body,position)
    assert outcomes(game)==[dict(entity=ITEM,anchor=HOLDER,status='transferred')]
    values=game.game()['values']
    assert values['LastOutcome']==3 and values['LastGap']==blocked['distance_m'] and values['LastOutcomeTick']=='22'
    completed=game.save(phase+'-completed')
    assert completed==game.bytes('pickup-complete') and 'physical' not in payload(completed)
    after=capture(game,args,out,phase+'-picked',CAMERA)
    if args.gpu is not None:
        assert after['foreground']==1 and before['objects']==after['objects']+1
    return completed,after


def compiled_follow(game,phase):
    first=game.matrix(ITEM)
    game.step()
    assert game.game()['values']['LastOutcome']==1 and not outcomes(game)
    game.step(5)
    current=game.matrix(ITEM)
    assert current[12]-first[12]>.08,(first,current)
    row=next(row for row in game.bindings() if row['entity']==ITEM)
    close_matrix(current,multiply(game.matrix(HOLDER),row['offset_matrix']))
    return game.save(phase+'-follow')


def run_compiled(args,out,world,revision):
    settings=configuration(args,GAME,FEATURES)
    saves=out/'warm-saves';saves.mkdir()
    owner=native.Session(out/'warm',world=world)
    try:
        game=Run(owner,revision,saves,settings,bool(args.descriptor))
        load(game,INITIAL)
        hold(game,ITEM,START)
        game.step(1,changes=1)
        body=next(r for r in game.bindings() if r['entity']==ITEM)['anchor']
        assert outcomes(game)==[dict(entity=ITEM,anchor=body,status='released')]
        game.step(21)
        blocked,=outcomes(game)
        assert blocked['status']=='surface_miss' and blocked['anchor']==HOLDER and .09<blocked['distance_m']<.3,blocked
        missed=game.save('warm-missed')
        assert missed==game.bytes('pickup-miss') and 'physical' in payload(missed)
        completed,visible=compiled_resume(game,args,out,'warm',blocked,body)
        end=compiled_follow(game,'warm')
        owner.close()

        cold=out/'cold-saves';cold.mkdir()
        shutil.copytree(saves/'slot-pickup-miss',cold/'slot-pickup-miss')
        owner=native.Session(out/'cold-miss',world=world)
        game=Run(owner,revision,cold,settings,bool(args.descriptor))
        game.restore('pickup-miss',22,2,1)
        assert game.save('cold-restored-miss')==missed
        assert compiled_resume(game,args,out,'cold',blocked,body)==(completed,visible)
        assert compiled_follow(game,'cold')==end
        owner.close()

        owner=native.Session(out/'cold-completed',world=world)
        game=Run(owner,revision,cold,settings,bool(args.descriptor))
        game.restore('pickup-complete',23,3,4)
        assert game.save('cold-restored-completed')==completed
        assert compiled_follow(game,'cold-completed')==end
        owner.close()
        previous=check_previous(args,out,settings,world,revision,INITIAL,FEATURE) if args.previous_binary else None
        return dict(miss=blocked,exact_archives=12,retired=body,windows_capture=visible,previous=previous)
    finally:
        owner.close()


def verify(args):
    out=args.output.resolve();out.mkdir(exist_ok=True)
    native.ARGS,native.DEADLINE=args,time.monotonic()+230
    report=dict(passed=False,engine_sha256=sha(args.binary))
    author=native.Session(out/'authored')
    try:
        world=author.world
        description=author.rpc('world.describe')
        feature=description['gameplay_item_attachments']
        assert (feature['pickup_feature'],feature['pickup_command_version'],feature['pickup_command_bytes'])==(FEATURE,6,272)
        rules=description['methods'][APPLY]['properties']['commands']['items']['oneOf']
        rule=next(r for r in rules if r['properties']['mode'].get('const')=='pickup')
        assert 'surface' in rule['properties']['contact']['required']
        revision=configure(author)
        authored=world.read_bytes()
        author.close()
        report['direct']=run_direct(args,out,world,revision)
        if args.descriptor or args.hostfxr:
            report['compiled']=run_compiled(args,out,world,revision)
        assert world.read_bytes()==authored
        report['passed']=True
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        author.close()
        report.update(rpc_count=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (out/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Atomic item pickup verified in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser,output_required=False)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--gpu',type=int)
    args=parser.parse_args()
    if args.output is not None:
        verify(args)
    else:
        with TemporaryDirectory(prefix='poima-item-pickup-') as output:
            args.output=Path(output)
            verify(args)
