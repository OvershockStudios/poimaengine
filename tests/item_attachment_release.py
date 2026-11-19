#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the held-to-world release boundary with native motion and cold replay."""
import argparse
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import time

from character_library_intake import multiply
from item_attachment_gameplay import Run, sha
from item_attachment_recovery_gameplay import hold
from runtime_attachment import CAMERA, ITEM, RIG_A, SPAWN, close_matrix, configure, node
from runtime_pose_frames import id_request, transform, uid
import world_physical_contract as native

METHOD='runtime.attachment.release'
COLLIDER,FLOOR,OBSERVER=uid(89),uid(90),uid(91)
HOLDER=node(RIG_A,1)


def add_release_world(owner):
    revision=configure(owner)
    material=dict(half_extents=[.5,.5,.5],motion='dynamic',mass=.7,friction=.7,restitution=.03)
    ops=[dict(op='template.set',id=COLLIDER,name='Release collision proxy',
              components=dict(Transform=transform(),BoxCollider=material))]
    for identity, label, local, kind, value in (
            (FLOOR,'Floor',transform((0,-1.75,0)),'BoxCollider',
             dict(material,half_extents=[20,.25,20],motion='static')),
            (OBSERVER,'World view',transform((0,0,5)),'Camera',
             dict(vertical_fov=58,near=.01,far=40))):
        ops.extend([dict(op='entity.create',id=identity,name=label),
                    native.component(identity,'Transform',local),native.component(identity,kind,value)])
    return native.transact(owner,ops,revision)['revision']


def release_params(run, item=ITEM, template=COLLIDER, linear=(2.5,.75,0), angular=(0,0,3), offset=None):
    request=dict(session_id=run.id,request_id=id_request(),expected_tick=run.tick,expected_structure_revision=run.structure,
                 item=item,body_template=template,linear_velocity_m_s=list(linear),angular_velocity_rad_s=list(angular))
    if offset is not None:request['body_in_item']=offset
    return request


def capture(run,args,output,label,camera):
    if args.gpu is None:return None
    path=output/(label+'.bmp')
    frame=run.call('runtime.capture',tick=run.tick,structure_revision=run.structure,camera=camera,
                   path=native.native(path),width=720,height=405,samples=4,gpu=args.gpu)
    assert frame['capture_written'] and frame['hardware'] and frame['nvrhi_errors']==0,frame
    viewmodel=frame['render_diagnostics'].get('viewmodel')
    return dict(sha256=sha(path),objects=frame['object_count'],
                foreground=viewmodel['draws']['camera_draws'] if viewmodel else 0)


def payload(archive):
    return json.loads(archive)['snapshot']['payload']

def body_state(archive,identity):
    return next(row['body'] for row in payload(archive)['entities'] if row['id']==identity)


def land(run,body,slot):
    run.step(96)
    end=run.save(slot)
    landed=body_state(end,body)
    assert -1.02<landed['position'][1]<-.78 and not landed['active'],landed
    assert landed['linear_velocity']==landed['angular_velocity']==[0,0,0],landed
    return end,landed


def resume(run,body,args,output,phase):
    initial=run.matrix(body)
    run.step(12)
    sample=run.matrix(body)
    followed=run.bindings()[0]
    close_matrix(run.matrix(ITEM),multiply(sample,followed['offset_matrix']))
    assert sample[12]-initial[12]>.3 and abs(sample[1]-initial[1])>.1,(initial,sample)
    mid=run.save(phase+'-in-motion')
    state=body_state(mid,body)
    assert state['active'] and state['linear_velocity'][1]<-.5 and state['angular_velocity'][2]>1
    visible=capture(run,args,output,phase+'-falling',OBSERVER)
    run.step(24)
    contact=run.save(phase+'-contact')
    assert body_state(contact,body)['active'] and payload(contact)['physical']['physics']['pairs']
    end,landed=land(run,body,phase+'-landed')
    return mid,contact,end,visible,state,landed


def restore(owner,revision,saves,slot,tick):
    game=Run(owner,revision,saves,{})
    restored=owner.rpc('save.load',dict(request_id=id_request(),configuration_generation=1,slot=slot,
        expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,
        expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=game.id))
    assert restored['tick']==tick
    game.tick=tick;game.structure=2
    return game


def verify(args):
    out=args.output.resolve()
    out.mkdir(exist_ok=True)
    native.ARGS,native.DEADLINE=args,time.monotonic()+230
    saves=out/'saves';saves.mkdir()
    report=dict(passed=False,engine_sha256=sha(args.binary),captures={})
    owner=native.Session(out/'runtime')
    try:
        discovery=owner.rpc('world.describe',dict(view='method',name=METHOD))
        assert discovery['schema_revision']>=149 and METHOD in discovery['methods']
        revision=add_release_world(owner)
        world=owner.world; authored=world.read_bytes()
        game=Run(owner,revision,saves,{})
        game.call('runtime.start',revision=revision)
        owner.rpc(METHOD,release_params(game),error=-32602)
        hold(game,ITEM,HOLDER)
        game.step(12)
        before=game.save('held')
        assert 'physical' not in payload(before)
        item=game.matrix(ITEM)
        report['captures']['holder']=capture(game,args,out,'holder',CAMERA)
        report['captures']['world-before']=capture(game,args,out,'world-before',OBSERVER)
        owner.rpc(METHOD,release_params(game,template=SPAWN),error=-32602)
        owner.rpc(METHOD,release_params(game,linear=(80,80,0)),error=-32602)
        owner.rpc(METHOD,dict(release_params(game),expected_tick=11),error=-32009)
        request=release_params(game,offset=dict(position=[0,-1.35,0],rotation=[0,0,0,1]))
        obstructed=owner.rpc(METHOD,request,error=-32602)
        assert 'penetrates' in obstructed['message'],obstructed
        assert game.save('after-admission-failures')==before and game.bindings()[0]['anchor']==HOLDER

        # Repair the obstructed request in place. The failed Jolt birth cannot
        # consume either the entity identity or the retry receipt.
        request['body_in_item']['position']=[.12,0,0]
        expected_id=json.loads(before)['snapshot']['payload']['structure']['next_entity_id']
        receipt=owner.rpc(METHOD,request)
        game.structure=2
        body=receipt['body']
        assert receipt==dict(session_id=game.id,tick=12,structure_revision=2,item=ITEM,body=expected_id,replayed=False)
        assert owner.rpc(METHOD,request)==dict(receipt,replayed=True)
        owner.rpc(METHOD,dict(request,linear_velocity_m_s=[3,0,0]),error=-32010)
        close_matrix(game.matrix(ITEM),item)
        assert game.bindings()[0]['released'] is True and game.bindings()[0]['anchor']==body
        report['captures']['world-release']=capture(game,args,out,'world-release',OBSERVER)
        report['captures']['player-release']=capture(game,args,out,'player-release',CAMERA)
        if args.gpu is not None:
            assert report['captures']['world-release']['objects']==report['captures']['world-before']['objects']+2
            assert report['captures']['player-release']['foreground']+2==report['captures']['holder']['foreground']
            assert report['captures']['world-before']['sha256']!=report['captures']['world-release']['sha256']
        launched=game.save('launched')
        state=body_state(launched,body)
        assert state['linear_velocity']==[2.5,.75,0] and state['angular_velocity']==[0,0,3] and state['active']
        world_payload=json.loads(launched)['snapshot']['payload']
        assert world_payload['attachments']['bindings'][0]['released'] is True
        assert world_payload['physical']['version']==1 and world_payload['physical']['rigs']==[]
        spawn=next(row for row in world_payload['structure']['spawned'] if row['id']==body)
        assert spawn['template_id']==COLLIDER
        game.call('runtime.structure.transact',error=-32602,request_id=id_request(),expected_tick=12,
                  expected_structure_revision=2,despawns=[body])
        assert game.save('proxy-still-owned')==launched
        warm=resume(game,body,args,out,'warm')
        owner.close()

        owner=native.Session(out/'cold',world=world)
        game=restore(owner,revision,saves,'launched',12)
        assert game.save('restored')==launched
        assert capture(game,args,out,'cold-release',OBSERVER)==report['captures']['world-release']
        cold=resume(game,body,args,out,'cold')
        assert cold==warm
        owner.close()

        owner=native.Session(out/'contact',world=world)
        game=restore(owner,revision,saves,'warm-contact',48)
        assert game.save('contact-restored')==warm[1]
        assert land(game,body,'contact-restored-landed')==(warm[2],warm[5])
        landed_pose=game.matrix(ITEM)
        hold(game,ITEM,HOLDER)
        close_matrix(game.matrix(ITEM),landed_pose)
        assert 'released' not in game.bindings()[0]
        removed=game.call('runtime.structure.transact',request_id=id_request(),expected_tick=game.tick,
                          expected_structure_revision=game.structure,despawns=[body])
        game.structure=removed['structure_revision']
        game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=game.structure,id=body)
        assert 'physical' not in payload(game.save('reclaimed'))
        assert world.read_bytes()==authored
        owner.close()
        report.update(passed=True,body_at_release=state,body_in_flight=warm[4],body_landed=warm[5],
                      active_contact_pairs=len(payload(warm[1])['physical']['physics']['pairs']),
                      obstructed_failure=obstructed,complete_checkpoint_pairs=8)
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        owner.close()
        report.update(rpc_count=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (out/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Physical release verified in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    args=parser.parse_args()
    if args.output is not None:
        verify(args)
    else:
        with TemporaryDirectory(prefix='poima-item-release-') as output:
            args.output=Path(output)
            verify(args)
