#!/usr/bin/env python3
"""Verify committed frame comparisons on guarded fixtures and an original animated rig."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import time
import uuid

from character_library_intake import LibraryOracle, multiply, near, transform_matrix
import world_physical_contract as native


def uid(number):
    return f'{number:032x}'


def id_request():
    return uuid.uuid4().hex


def qz(degrees):
    angle = math.radians(degrees) / 2
    return [0, 0, math.sin(angle), math.cos(angle)]


def qy(degrees):
    angle = math.radians(degrees) / 2
    return [0, math.sin(angle), 0, math.cos(angle)]


def transform(position=(0,0,0), rotation=(0,0,0,1), scale=(1,1,1)):
    return dict(position=list(position), rotation=list(rotation), scale=list(scale))


def frame(entity, position=None, rotation=(0,0,0,1)):
    return dict(entity=entity) if position is None else dict(
        entity=entity, offset=dict(position=list(position), rotation=list(rotation)))


def pose_camera(position, target):
    x,y,z = (eye-look for eye,look in zip(position,target))
    yaw, pitch = math.atan2(x,z)/2, math.atan2(-y,math.hypot(x,z))/2
    return transform(position, (math.cos(yaw)*math.sin(pitch),math.sin(yaw)*math.cos(pitch),
                                -math.sin(yaw)*math.sin(pitch),math.cos(yaw)*math.cos(pitch)))

def source_shear(matrix):
    axes = [matrix[start:start+3] for start in (0,4,8)]
    return max(abs(sum(x*y for x,y in zip(axes[i],axes[j]))) /
               (math.hypot(*axes[i])*math.hypot(*axes[j])) for i,j in ((0,1),(1,2),(0,2)))


def setup(session, original, gpu):
    imported = session.rpc('asset.import', dict(source=native.native(original)))
    ops = [dict(op='asset.instantiate', id=native.RIG, name='Knight', asset=imported['asset'])]
    for identity, label, local in (
        (uid(21),'Orthogonal with unequal scales',transform((50,2,3),qy(90),(2,3,4))),
        (uid(22),'Rigid',transform((50,2,3),qy(90))),
    ):
        ops.extend((dict(op='entity.create',id=identity,name=label),
                    native.component(identity,'Transform',local)))
    ops.append(dict(op='template.set', id=uid(23), name='Spawn guard',
                    components=dict(Transform=transform((80,0,0)))))
    if gpu is not None:
        for index,eye in ((31,(-2.2,1.3,2.6)),(32,(-.1,3.5,.8))):
            identity = uid(index)
            ops.extend((dict(op='entity.create',id=identity,name='Pose review camera'),
                        native.component(identity,'Camera',dict(vertical_fov=40,near=.01,far=30)),
                        native.component(identity,'Transform',pose_camera(eye,(-.12,1,0)))))
    session.rpc('world.transact',dict(request_id=id_request(),base_revision=0,ops=ops))


def deterministic(session, owner):
    tick = 0
    def read(pairs, error=None, **guards):
        params = dict(session_id=owner,tick=tick,structure_revision=0,pairs=pairs)
        return session.rpc('runtime.pose.compare',{**params,**guards},error=error)
    def pair(source, target, **limits):
        return dict(source=source,target=target,**limits)

    aligned = frame(uid(22))
    scaled = frame(uid(21))
    before = session.rpc('runtime.status')
    test_pairs = [
        pair(frame(uid(21),(.05,.04,-.02)),scaled,position_limit_m=.17,rotation_limit_deg=.001),
        pair(frame(uid(21),(0,0,0),qz(20)),scaled,rotation_limit_deg=180),
        pair(frame(uid(22),(0,0,0),qz(20)),aligned,position_limit_m=0,rotation_limit_deg=10),
        pair(frame(uid(22),(0,0,0),qz(179.999)),aligned),
        pair(frame(uid(22),(0,0,0),qz(1e-7)),aligned),
        pair(aligned,aligned,position_limit_m=0,rotation_limit_deg=0),
    ]
    first, shear, rotation, half_turn, small, zero = read(test_pairs)['pairs']
    near(first['target_delta_m'],[.1,.12,-.08],1e-12,'Signed target-axis metre displacement')
    near(first['world_delta_m'],[-.08,.12,-.1],1e-12,'World-space displacement')
    assert math.isclose(first['position_error_m'],math.hypot(.1,.12,.08),abs_tol=1e-12)
    assert first['within_position_limit'] is False and first['within_rotation_limit'] is True
    assert shear['source']['basis']=='sheared' and shear['rotation_error_deg'] is None
    assert shear['within_rotation_limit'] is None and shear['position_error_m']==0
    assert math.isclose(rotation['rotation_error_deg'],20,abs_tol=1e-10)
    assert rotation['within_rotation_limit'] is False and rotation['within_position_limit'] is True
    assert math.isclose(half_turn['rotation_error_deg'],179.999,abs_tol=1e-9)
    assert math.isclose(small['rotation_error_deg'],1e-7,abs_tol=1e-12)
    assert zero['position_error_m']==zero['rotation_error_deg']==0
    assert zero['within_position_limit'] is True and zero['within_rotation_limit'] is True
    assert len(read([test_pairs[-1]]*32)['pairs'])==32

    read([test_pairs[0]],-32009,tick=1)
    read([test_pairs[0]],-32030,session_id=id_request())
    read([test_pairs[0]],-32009,structure_revision=1)
    read([test_pairs[0]]*33,-32602)
    read([test_pairs[0],pair(frame(uid(9999)),aligned)],-32004)
    read([pair(frame(uid(22),(0,0,0),(0,0,0,0)),aligned)],-32602)
    read([pair(aligned,aligned,rotation_limit_deg=True)],-32602)
    read([pair(dict(entity=uid(22),offset=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])),aligned)],-32602)
    assert session.rpc('runtime.status')==before

    edited = session.rpc('runtime.structure.transact',dict(session_id=owner,request_id=id_request(),
        expected_tick=0,expected_structure_revision=0,spawns=[dict(template_id=uid(23))],despawns=[]))
    assert edited['structure_revision']==1
    read([test_pairs[0]],-32009)
    after = read([test_pairs[0]],structure_revision=1)
    assert after['tick']==0 and after['structure_revision']==1
    return {'six_analytic_pairs':True,'positive_nonuniform_scale':True,'small_and_half_turn':True,
            'sheared_basis_removes_angle':True,'same_tick_structure_guard':True,'readonly_rejects':8}


def grips(session, owner, oracle, gpu):
    notes = []
    sword, slot, wrist = 10,12,14
    assert oracle.nodes[sword]['name']=='1H_Sword' and oracle.parents[sword]==slot
    assert oracle.nodes[slot]['name']=='handslot.r' and oracle.nodes[wrist]['name']=='wrist.r'
    for tick,(clip,phase) in enumerate(((36,.1),(0,0),(0,.2),(0,.45)),1):
        session.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick-1,
            expected_structure_revision=1,ticks=1,animations=[dict(entity=native.RIG,clip=clip,time=phase,
                speed=1,playing=False,loop=False)]))
        local,_,world = oracle.pose(clip,phase)
        target = frame(native.node(slot),local[sword]['position'],local[sword]['rotation'])
        source = frame(native.node(sword))
        authored_offset = dict(position=[.035,0,0],rotation=qz(20))
        caught = frame(native.node(sword),**authored_offset)
        comparisons = session.rpc('runtime.pose.compare',dict(session_id=owner,tick=tick,structure_revision=1,pairs=[
            dict(source=source,target=target,position_limit_m=1e-5,rotation_limit_deg=.01),
            dict(source=caught,target=target,position_limit_m=.01,rotation_limit_deg=2),
            dict(source=frame(native.node(wrist)),target=target),
        ]))
        assert comparisons['layout']=='column_major' and comparisons['tick']==tick
        good, bad, wrist_row = comparisons['pairs']
        expected_slot = multiply(world[slot],transform_matrix(position=local[sword]['position'],
            rotation=local[sword]['rotation'],scale=[1,1,1]))
        expected_bad = multiply(world[sword],transform_matrix(**authored_offset,scale=[1,1,1]))
        worst = max(near(good['source']['world_matrix'],world[sword],3e-6,'Sword frame from source GLB'),
                    near(good['target']['world_matrix'],expected_slot,3e-6,'Hand-slot composition from source GLB'),
                    near(bad['source']['world_matrix'],expected_bad,3e-6,'Offset frame from source GLB'),
                    near(wrist_row['source']['world_matrix'],world[wrist],3e-6,'Actual wrist against source GLB'))
        assert good['within_position_limit'] is True and bad['within_position_limit'] is False
        for result, independent_source in ((good,world[sword]),(bad,expected_bad)):
            source_basis = 'sheared' if source_shear(independent_source)>1e-6 else 'regular'
            target_basis = 'sheared' if source_shear(expected_slot)>1e-6 else 'regular'
            assert result['source']['basis']==source_basis and result['target']['basis']==target_basis
            if 'sheared' in (source_basis,target_basis):
                assert result['within_rotation_limit'] is None and result['rotation_error_deg'] is None
            else:
                is_aligned = result is good
                assert result['within_rotation_limit'] is is_aligned
                assert math.isclose(result['rotation_error_deg'],0 if is_aligned else 20,abs_tol=.001)
        assert math.isclose(bad['position_error_m'],.035,abs_tol=3e-6)
        row = dict(tick=tick,clip=oracle.clips[clip]['name'],seconds=phase,independent_matrix_error=worst,
                   nominal_basis=good['source']['basis'],independent_axis_dot=source_shear(world[sword]),
                   aligned_m=good['position_error_m'],aligned_degrees=good['rotation_error_deg'],
                   perturbed_m=bad['position_error_m'],perturbed_degrees=bad['rotation_error_deg'],captures=[])
        if gpu is not None and tick in (2,4):
            for camera,label in ((31,'front'),(32,'overhead')):
                path = session.directory/f'{label}-{tick}.bmp'
                result=session.rpc('runtime.capture',dict(session_id=owner,tick=tick,camera=uid(camera),
                    path=native.native(path),width=960,height=540,samples=4,gpu=gpu))
                assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0
                assert result['tick']==comparisons['tick']
                row['captures'].append(dict(filename=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        notes.append(row)
    return notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True,help='Original pinned Knight.glb')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()
    data = args.source.read_bytes()
    oracle = LibraryOracle(data)
    native.ARGS, native.DEADLINE = args, time.monotonic()+180
    report = dict(passed=False,source_sha256=hashlib.sha256(data).hexdigest(),
                  executable_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),gpu=args.gpu)
    session = native.Session(args.output/'session')
    try:
        method = session.rpc('world.describe',dict(view='method',name='runtime.pose.compare'))
        assert method['methods']['runtime.pose.compare']['properties']['pairs']['maxItems']==32
        setup(session,args.source,args.gpu)
        original = session.world.read_bytes()
        owner = id_request()
        session.rpc('runtime.start',dict(session_id=owner,revision=1))
        report['analytic'] = deterministic(session,owner)
        report['rig'] = grips(session,owner,oracle,args.gpu)
        assert session.world.read_bytes()==original
        session.rpc('runtime.stop',dict(session_id=owner))
        report['passed'] = True
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'],rpc_count=len(native.REPORT['calls']))
        (args.output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f"Verified six analytical pairs and four poses on the original Knight in {report['rpc_count']} native calls.")


if __name__ == '__main__':
    main()
