#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare direct and compiled guarded handoffs at a live animated boundary."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import time

import item_attachment_gameplay as compiled
from character_depth_oracle import inverse
from character_library_intake import multiply
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME='Poima.Tests.AttachmentContactGame'
ITEM,FIRST,SECOND=compiled.ITEM,compiled.FIRST,compiled.SECOND
ORIENTATION=[0,0,0,1]


def rpc_parameters(run, command):
    return dict(session_id=run.id, request_id=id_request(), expected_tick=run.tick,
                expected_structure_revision=run.structure, commands=[command])


def verify(run, commands):
    frame=run.call('runtime.pose.compare', tick=run.tick, structure_revision=run.structure,
        pairs=[dict(source=dict(entity=ITEM), target=dict(entity=FIRST,offset=commands['contact']['anchor_offset']))])['pairs'][0]
    error=run.owner.rpc('runtime.attachment.apply',rpc_parameters(run,commands),error=-32040)
    expected=commands['contact']
    assert error['data']==dict(entity=ITEM,anchor=FIRST,position_error_m=frame['position_error_m'],
        rotation_error_deg=frame['rotation_error_deg'],position_limit_m=expected['position_limit_m'],
        rotation_limit_deg=expected['rotation_limit_deg']), error
    return error['data']


def run(args):
    output=args.output.resolve()
    output.mkdir(parents=True)
    native.ARGS,native.DEADLINE=args,time.monotonic()+180
    saves=output/'saves';saves.mkdir()
    config=(dict(descriptor=native.native(args.descriptor),expected_descriptor_sha256=compiled.sha(args.descriptor))
            if args.descriptor else dict(hostfxr=native.native(args.hostfxr),bridge=native.native(args.bridge),
                                          assembly=native.native(args.assembly),type=GAME))
    report=dict(passed=False,engine=compiled.sha(args.binary),checks={},captures={})
    owner=native.Session(output/'direct')
    try:
        revision=compiled.fixture.configure(owner)
        world=owner.world
        authored=world.read_bytes()
        discovery=owner.rpc('world.describe')
        sdk=discovery['gameplay_item_attachments']
        assert discovery['schema_revision']>=143 and sdk['contact_feature']=='item_attachment_contact_v1'
        assert sdk['services_bytes']==528 and sdk['contact_command_bytes']==256
        direct=compiled.Run(owner,revision,saves,config,bool(args.descriptor))
        direct.call('runtime.start',revision=revision)
        original=direct.save('original')
        at_start=direct.matrix(FIRST)
        direct.step()
        item=direct.matrix()
        anchor=direct.matrix(FIRST)
        offset=multiply(inverse(anchor),item)
        compiled.fixture.close_matrix(offset[:12]+[0,0,0,1],
            [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])
        offset_xyz=offset[12:15]
        before=direct.save('before')
        capture=direct.capture(output,'reference',args.gpu)
        guarded=dict(entity=ITEM,anchor=FIRST,mode='keep_world',contact=dict(
            anchor_offset=dict(position=offset_xyz,rotation=ORIENTATION),position_limit_m=.002,rotation_limit_deg=.1))
        position=dict(guarded,contact=dict(guarded['contact'],anchor_offset=dict(position=[offset_xyz[0]+.05,*offset_xyz[1:]],rotation=ORIENTATION)))
        position_miss=verify(direct,position)
        assert math.isclose(position_miss['position_error_m'],.05) and position_miss['rotation_error_deg']<.1
        q=math.radians(15)
        rotation=dict(guarded,contact=dict(guarded['contact'],anchor_offset=dict(position=offset_xyz,
            rotation=[0,0,math.sin(q/2),math.cos(q/2)])))
        rotation_miss=verify(direct,rotation)
        assert rotation_miss['position_error_m']<.002 and math.isclose(rotation_miss['rotation_error_deg'],15)
        direct.owner.rpc('runtime.attachment.apply',rpc_parameters(direct,dict(guarded,anchor=None)),error=-32602)
        direct.owner.rpc('runtime.attachment.apply',rpc_parameters(direct,dict(guarded,mode='reset')),error=-32602)
        assert direct.save('after-misses')==before and direct.bindings()==[]
        accepted=rpc_parameters(direct,guarded)
        receipt=direct.owner.rpc('runtime.attachment.apply',accepted)
        direct.structure=1
        assert direct.owner.rpc('runtime.attachment.apply',accepted)==dict(receipt,replayed=True)
        reference_binding=direct.bindings()[0]
        compiled.fixture.close_matrix(direct.matrix(),item)
        assert direct.capture(output,'direct',args.gpu)==capture
        # Resolve the receiver at its candidate transform when another command
        # moves it in the same transaction, regardless of submission order.
        receiver=compiled.fixture.OTHER
        camera_space=multiply(inverse(direct.matrix(compiled.CAMERA)),item)
        moved=dict(entity=receiver,mode='snap',anchor=compiled.CAMERA,
                   offset=dict(position=camera_space[12:15],rotation=ORIENTATION,scale=[1,1,1]))
        paired=dict(entity=ITEM,mode='keep_world',anchor=receiver,
                    contact=dict(position_limit_m=.002,rotation_limit_deg=.1))
        params=rpc_parameters(direct,paired)
        params['commands'].append(dict(moved,offset=dict(moved['offset'],position=[0,0,-5])))
        prior=direct.save('before-atomic')
        miss=owner.rpc('runtime.attachment.apply',params,error=-32040)
        assert miss['data']['anchor']==receiver and miss['data']['position_error_m']>.002
        nonrigid=dict(paired,contact=dict(paired['contact'],rotation_limit_deg=180,
            anchor_offset=dict(position=[0,0,0],rotation=[0,0,math.sin(q/2),math.cos(q/2)])))
        sheared=rpc_parameters(direct,nonrigid)
        sheared['commands'].append(dict(moved,offset=dict(moved['offset'],scale=[2,1,1])))
        unavailable=owner.rpc('runtime.attachment.apply',sheared,error=-32040)['data']
        assert unavailable['position_error_m']<.002 and unavailable['rotation_error_deg'] is None
        assert direct.save('after-atomic')==prior
        report['checks']['unavailable_rotation']=unavailable
        params['commands'][-1]=moved
        assert owner.rpc('runtime.attachment.apply',params)['structure_revision']==2
        direct.structure=2
        compiled.fixture.close_matrix(direct.matrix(),item)
        compiled.fixture.close_matrix(direct.matrix(receiver),item)
        report['checks']['candidate_anchor']=miss['data']
        report['checks']['position_miss']=position_miss
        report['checks']['rotation_miss']=rotation_miss
        owner.close()

        start_error=math.dist(multiply(at_start,offset)[12:15],item[12:15])
        assert start_error>.002
        report['checks']['pre_tick_position_error_m']=start_error
        owner=native.Session(output/'compiled',world=world)
        live=compiled.Run(owner,revision,saves,config,bool(args.descriptor))
        live.start()
        values=dict(OffsetX=offset_xyz[0],OffsetY=offset_xyz[1],OffsetZ=offset_xyz[2])
        live.edit(Command=2,**values)
        failed=live.save('failed-before')
        error=live.step(2,error=-32040)
        assert error['data']['entity']==ITEM and error['data']['anchor']==SECOND and error['data']['position_error_m']>.002,error
        assert live.save('failed-after')==failed and live.bindings()==[] and not (saves/'slot-contact-auto').exists()
        report['checks']['late_tick_miss']=error['data']
        live.edit(Command=1)
        result=live.step(changes=1)
        assert result['save_serviced'] and result['save_operation']['generation']==1
        assert live.save('transferred')==live.bytes('contact-auto')
        assert live.bindings()==[reference_binding]
        assert live.game()['values']['Ticks']==1
        assert live.capture(output,'compiled',args.gpu)==capture
        report['captures']['same_tick_sha256']=capture
        handoff=live.game_revision
        live.step(10)
        continued=live.save('continuation')
        assert live.matrix()!=item
        owner.close()

        owner=native.Session(output/'cold',world=world)
        cold=compiled.Run(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('transferred',1,1,handoff)
        assert cold.save('restored')==cold.bytes('transferred')
        cold.step(10)
        assert cold.save('restored-continuation')==continued
        assert world.read_bytes()==authored and direct.bytes('original')==original
        report['passed']=True
    except BaseException as failure:
        report['failure']=repr(failure)
        raise
    finally:
        owner.close()
        report.update(owners=native.REPORT['owners'],rpc_count=len(native.REPORT['calls']))
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Contact gate verified in {report["rpc_count"]} native calls.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--hostfxr',type=Path)
    parser.add_argument('--bridge',type=Path)
    parser.add_argument('--assembly',type=Path)
    parser.add_argument('--descriptor',type=Path)
    args=parser.parse_args()
    if bool(args.descriptor)==bool(args.hostfxr and args.bridge and args.assembly):
        parser.error('supply either a Native AOT descriptor or a CoreCLR host, bridge and assembly')
    run(args)


if __name__=='__main__':
    main()
