#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check calibrated socket arrival and native renders on the pinned CC0 Knight."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import time

from character_depth_oracle import inverse
from character_library_intake import LibraryOracle, multiply, near, transform_matrix
from runtime_pose_frames import id_request, setup, source_shear, uid
from viewmodel_clearance_player import quaternion
import world_physical_contract as native

PINNED='60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168'
HOLD_CLIP, ACTION_CLIP, ITEM_NODE, RECEIVER_NODE=13,8,11,5


def run(args):
    out=args.output.resolve()
    out.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+180
    source=args.source.read_bytes()
    assert hashlib.sha256(source).hexdigest()==PINNED
    oracle=LibraryOracle(source)
    ready=oracle.pose(HOLD_CLIP,0)[2]
    item_contact=multiply(inverse(ready[ITEM_NODE]),ready[RECEIVER_NODE])
    assert source_shear(item_contact)<1e-6
    contact=dict(position=item_contact[12:15],rotation=quaternion(item_contact))
    contact_local=transform_matrix(**contact,scale=[1,1,1])
    receiver=native.node(RECEIVER_NODE)
    report=dict(passed=False,source_sha256=PINNED,contact=contact,captures={})
    io=native.Session(out/'session')
    try:
        setup(io,args.source,args.gpu)
        authored=json.loads(io.world.read_text())
        identity=native.node(ITEM_NODE)
        item,=[entity for entity,row in authored['entities'].items()
               if row['parent']==identity and 'StaticMesh' in row['components']]
        rig=authored['entities'][native.RIG]['components']['AnimationRig']
        rig.update(clip=ACTION_CLIP,time=0,loop=False,playing=True)
        edits=[native.component(native.RIG,'AnimationRig',rig)]
        hidden={native.node(i) for i in (0,1,2,3,4,10)}
        for entity,row in authored['entities'].items():
            if row['parent'] in hidden and 'StaticMesh' in row['components']:
                edits.append(native.component(entity,'StaticMesh',dict(row['components']['StaticMesh'],visible=False)))
        revision=native.transact(io,edits,authored['revision'])['revision']
        authored=io.world.read_bytes()
        owner=id_request()
        io.rpc('runtime.start',dict(session_id=owner,revision=revision))
        tick,structure=0,0

        def step(ticks,**extra):
            nonlocal tick
            reply=io.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
                expected_structure_revision=structure,ticks=ticks,**extra))
            tick+=ticks
            assert reply['tick']==tick

        def matrix(entity):
            return io.rpc('runtime.entity',dict(session_id=owner,tick=tick,structure_revision=structure,id=entity))['world_matrix']

        def observe():
            return io.rpc('runtime.pose.compare',dict(session_id=owner,tick=tick,structure_revision=structure,pairs=[
                dict(source=dict(entity=item,offset=contact),target=dict(entity=receiver),
                     position_limit_m=.015,rotation_limit_deg=5)]))['pairs'][0]

        def transfer(error=None):
            return io.rpc('runtime.attachment.apply',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
                expected_structure_revision=structure,commands=[dict(entity=item,mode='keep_world',anchor=receiver,
                    contact=dict(item_offset=contact,position_limit_m=.015,rotation_limit_deg=5))]),error)

        def capture(label):
            if args.gpu is None:return
            images={}
            for number,view in ((31,'front'),(32,'overhead')):
                path=out/f'{label}-{view}.bmp'
                result=io.rpc('runtime.capture',dict(session_id=owner,tick=tick,structure_revision=structure,camera=uid(number),
                    path=native.native(path),width=720,height=405,gpu=args.gpu,samples=4))
                assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0
                images[view]=hashlib.sha256(path.read_bytes()).hexdigest()
            report['captures'][label]=images

        step(12)
        late=oracle.pose(ACTION_CLIP,12/60)[2]
        measured=observe()
        expected=math.dist(multiply(late[ITEM_NODE],contact_local)[12:15],late[RECEIVER_NODE][12:15])
        assert expected>.18 and math.isclose(measured['position_error_m'],expected,abs_tol=3e-6)
        assert not measured['within_position_limit']
        error=transfer(-32040)['data']
        assert error['position_error_m']==measured['position_error_m'] and error['rotation_error_deg']==measured['rotation_error_deg']
        report['deferred_contact']=error
        assert io.rpc('runtime.status')['structure_revision']==0
        step(1,animations=[dict(entity=native.RIG,clip=ACTION_CLIP,time=0,speed=1,loop=False,playing=False)])
        aligned=observe()
        assert aligned['within_position_limit'] and aligned['within_rotation_limit']
        report['accepted_contact']=dict(tick=tick,position_error_m=aligned['position_error_m'],rotation_error_deg=aligned['rotation_error_deg'])
        before=matrix(item)
        capture('aligned')
        assert transfer()['structure_revision']==1
        structure=1
        near(matrix(item),before,2e-8,'Keep-world item after the accepted handoff')
        capture('handoff')
        assert report['captures'].get('handoff')==report['captures'].get('aligned')
        step(10,animations=[dict(entity=native.RIG,clip=ACTION_CLIP,time=0,speed=1,loop=False,playing=True)])
        motion=oracle.pose(ACTION_CLIP,10/60)[2]
        predicted=multiply(motion[RECEIVER_NODE],multiply(inverse(ready[RECEIVER_NODE]),ready[ITEM_NODE]))
        near(matrix(item),predicted,4e-6,'Continued following after the receive')
        assert math.dist(matrix(item)[12:15],before[12:15])>.05
        settled=observe()
        assert settled['within_position_limit'] and settled['within_rotation_limit']
        report['following_contact']=dict(tick=tick,position_error_m=settled['position_error_m'],rotation_error_deg=settled['rotation_error_deg'])
        capture('following')
        if args.gpu is not None:assert report['captures']['following']!=report['captures']['handoff']
        assert io.world.read_bytes()==authored
        io.rpc('runtime.stop',dict(session_id=owner))
        report['passed']=True
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        io.close()
        report.update(rpc_count=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Imported handoff contact verified in {report["rpc_count"]} native calls.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    run(parser.parse_args())


if __name__=='__main__':
    main()
