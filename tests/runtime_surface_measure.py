#!/usr/bin/env python3
"""Verify selected skinning and nearest-surface queries against analytic geometry."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import time

from animation_fixture import ribbon
from gltf_fixture import glb
from runtime_ik_attachment import save
from runtime_pose_frames import id_request, transform, uid
import world_physical_contract as native

METHOD = 'runtime.surface.measure'
TARGET, REVERSED, MANY, HIDDEN = (uid(i) for i in (51, 52, 53, 54))
SKIN = hashlib.sha256(f'poima.instance.v1/{native.RIG}/node/0/primitive/0'.encode()).hexdigest()[:32]


def accessor(document, blob, payload, shape, component_type, count):
    blob.extend(bytes((-len(blob)) % 4))
    view = len(document['bufferViews'])
    index = len(document['accessors'])
    document['bufferViews'].append(dict(buffer=0, byteOffset=len(blob), byteLength=len(payload)))
    document['accessors'].append(dict(bufferView=view, componentType=component_type, count=count, type=shape))
    blob.extend(payload)
    document['buffers'][0]['byteLength'] = len(blob)
    return index


def geometry(session):
    def import_model(label, content, binary):
        path = session.directory/f'{label}.glb'
        path.write_bytes(glb(content, bytes(binary)))
        return session.rpc('asset.import', dict(source=native.native(path)))['asset']

    document, blob = ribbon()
    blob = bytearray(blob)
    output = accessor(document, blob, struct.pack('<6f', 0,1,0,0,-1,0), 'VEC3', 5126, 2)
    collapse = copy.deepcopy(document['animations'][0])
    collapse.update(name='Collapse to an edge')
    collapse['samplers'][0]['output'] = output
    document['animations'].append(collapse)
    skin = import_model('skinned', document, blob)

    panel, blob = ribbon()
    blob = bytearray(blob)
    del panel['skins'], panel['animations']
    panel['nodes'] = [dict(name='Panel', mesh=0)]
    panel['scenes'] = [dict(nodes=[0])]
    primitive = panel['meshes'][0]['primitives'][0]
    del primitive['attributes']['JOINTS_0'], primitive['attributes']['WEIGHTS_0']
    solid = import_model('rigid', panel, blob)
    topology = session.rpc('asset.mesh.triangles', dict(asset=solid,primitive=0,offset=2,limit=1))
    assert topology['total']==4 and topology['items']==[dict(index=2,vertices=[2,3,4])]
    assert topology['next_offset']==3
    last=session.rpc('asset.mesh.triangles', dict(asset=solid,primitive=0,offset=3))
    assert last['items']==[dict(index=3,vertices=[3,5,4])] and last['next_offset'] is None
    session.rpc('asset.mesh.triangles',dict(asset=solid,primitive=0,limit=True),error=-32602)
    repeated = 32769
    primitive['indices'] = accessor(panel, blob, struct.pack('<3H',0,1,2)*repeated, 'SCALAR', 5123, repeated*3)
    dense = import_model('many-triangles', panel, blob)

    ops = [
        dict(op='asset.instantiate', id=native.RIG, name='Moving weighted ribbon', asset=skin),
        native.component(native.RIG,'Transform',transform((0,0,.125))),
        native.component(native.RIG,'AnimationRig',dict(asset=skin,clip=0,time=0,speed=1,loop=False,playing=True)),
        dict(op='template.set',id=uid(101),name='Extra root',components=dict(Transform=transform((10,0,0)))),
    ]
    for identity, asset, visible, oriented in (
            (TARGET,solid,True,transform()),(REVERSED,solid,True,transform(rotation=(0,1,0,0))),
            (MANY,dense,True,transform((20,0,0))),(HIDDEN,solid,False,transform())):
        ops.extend((dict(op='entity.create',id=identity,name='Selected panel'),
                    native.component(identity,'Transform',oriented),
                    native.component(identity,'StaticMesh',dict(asset=asset,primitive=0,visible=visible))))
    return native.transact(session,ops,0)['revision']


def close_point(actual, expected):
    assert len(actual)==len(expected) and all(math.isclose(a,b,rel_tol=0,abs_tol=2e-8)
                                             for a,b in zip(actual,expected)), (actual,expected)


def probe(source=SKIN,target=TARGET,triangle=0,weights=(.25,.5,.25),**rest):
    return dict(source=dict(entity=source,triangle=triangle,barycentric=list(weights)),target=dict(entity=target),**rest)


def verify(session, revision):
    owner=id_request()
    start=dict(session_id=owner,revision=revision)
    session.rpc('runtime.start',start)
    tick=0

    def measure(*pairs,error=None,**guards):
        params=dict(session_id=owner,tick=tick,structure_revision=0,pairs=pairs)
        params.update(guards)
        return session.rpc(METHOD,params,error=error)

    saves=native.save_root(session)
    baseline=save(session,saves,'before',owner,tick)
    row=measure(probe(distance_limit_m=.125),probe(target=REVERSED),
                probe(source=TARGET,target=SKIN),probe(distance_limit_m=.124999))
    assert row['triangle_checks']==16 and row['authored_revision']==revision and row['tick']==0
    first, reverse, swapped, beyond=row['pairs']
    close_point(first['source']['point_m'],[0,.25,.125])
    close_point(first['target']['point_m'],[0,.25,0])
    close_point(first['world_delta_m'],[0,0,.125])
    assert first['target']['triangle']==0
    close_point(first['target']['barycentric'],[.25,.5,.25])
    close_point(first['target']['normal'],[0,0,1])
    assert first['normal_offset_m']==.125 and first['distance_m']==.125 and first['within_distance_limit'] is True
    assert first['normal_alignment']==1 and reverse['normal_alignment']==-1
    assert reverse['normal_offset_m']==-.125 and swapped['normal_offset_m']==-.125
    assert beyond['within_distance_limit'] is False and swapped['within_distance_limit'] is None

    tie=dict(source=dict(entity=TARGET,vertex=3),target=dict(entity=TARGET,triangles=[2,1]))
    selected=measure(tie)['pairs'][0]
    assert selected['target']['triangle']==1 and selected['distance_m']==0
    assert selected['source']['normal'] is None and selected['normal_alignment'] is None
    assert measure(probe(target=MANY),error=-32602)
    final_tri=probe(target=MANY)
    final_tri['target']['triangles']=[32768]
    assert measure(final_tri)['pairs'][0]['target']['triangle']==32768
    duplicated=copy.deepcopy(tie);duplicated['target']['triangles']=[1,1]
    far=probe();far['target']['triangles']=[4]
    negative=probe(weights=(.5,.8,-.3))
    for invalid in (duplicated,far,negative,probe(weights=(0,0,0)),probe(triangle=0.0),
                    probe(target=HIDDEN),probe(target=native.RIG),dict(source=dict(entity=SKIN,vertex=99),target=dict(entity=TARGET))):
        measure(invalid,error=-32602)
    measure(probe(target=uid(999)),error=-32004)
    measure(probe(),error=-32009,tick=1)
    measure(probe(),error=-32009,structure_revision=1)
    measure(probe(),error=-32030,session_id=id_request())
    assert len(measure(*([probe()]*32))['pairs'])==32
    measure(*([probe()]*33),error=-32602)
    assert save(session,saves,'after',owner,tick)==baseline

    tick=session.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick,ticks=30))['tick']
    moved=measure(probe(triangle=3,weights=(.3,.6,.1)))['pairs'][0]
    close_point(moved['source']['point_m'],[.825,1.7,.125])
    close_point(moved['target']['point_m'],[.5,1.7,0])
    close_point(moved['world_delta_m'],[.325,0,.125])
    assert math.isclose(moved['distance_m'],math.hypot(.325,.125),abs_tol=2e-8)
    tick=session.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick,ticks=1,
        animations=[dict(entity=native.RIG,clip=1,time=2,speed=1,loop=False,playing=False)]))['tick']
    collapsed=probe()
    against_collapse=dict(source=dict(entity=TARGET,vertex=0),target=dict(entity=SKIN,triangles=[0]))
    degenerate, segment=measure(collapsed,against_collapse)['pairs']
    assert degenerate['source']['normal'] is None
    close_point(degenerate['source']['point_m'],[0,0,.125])
    assert segment['target']['normal'] is None and segment['normal_offset_m'] is None
    close_point(segment['target']['point_m'],[-.5,0,.125])
    assert segment['distance_m']==.125

    session.rpc('runtime.structure.transact',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
        expected_structure_revision=0,spawns=[dict(template_id=uid(101))]))
    measure(probe(),error=-32009)
    assert measure(probe(),structure_revision=1)['structure_revision']==1
    session.rpc('runtime.stop',dict(session_id=owner))
    return dict(moving_sample=moved,collapsed_sample=segment,tie_keeps_lowest=True,
                complete_observation_save_equality=True,observed_structure_guard=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    args.output=args.output.resolve()
    args.output.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+180
    report=dict(passed=False,engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session=native.Session(args.output/'session')
    try:
        info=session.rpc('world.describe',dict(view='method',name=METHOD))
        assert info['schema_revision']>=139
        pair=info['methods'][METHOD]['properties']['pairs']
        assert pair['maxItems']==32 and len(pair['items']['properties']['source']['oneOf'])==2
        revision=geometry(session)
        authored=session.world.read_bytes()
        report.update(verify(session,revision))
        assert session.world.read_bytes()==authored
        session.close()
        report['passed']=True
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        session.close()
        report['owners']=native.REPORT['owners']
        report['rpc_count']=len(native.REPORT['calls'])
        (args.output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Verified committed surfaces in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    main()
