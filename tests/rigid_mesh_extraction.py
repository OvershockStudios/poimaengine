#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a joint-local rigid extraction, including materials and weighted rejection."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import time

from animation_fixture import ribbon
from character_library_intake import multiply, point
from gltf_fixture import glb
from runtime_pose_frames import id_request, uid
from texture_fixture import png
import world_physical_contract as native

EXTRACT='asset.mesh.extract_rigid'
SOURCE_NODE, JOINT, MESH_ROOT, ITEM=0,1,uid(121),uid(122)


def source_glb():
    doc, payload=ribbon()
    blob=bytearray(payload)
    views,accessors=doc['bufferViews'],doc['accessors']
    attrs=doc['meshes'][0]['primitives'][0]['attributes']
    target=accessors[doc['skins'][0]['inverseBindMatrices']]
    bind=[0,0,-2,0, 0,3,0,0, .5,0,0,0, .25,-1,.5,1]
    struct.pack_into('<16f',blob,views[target['bufferView']]['byteOffset']+64,*bind)
    weights=len(accessors)
    accessors.append(dict(bufferView=len(views),componentType=5126,count=6,type='VEC4'))
    views.append(dict(buffer=0,byteOffset=len(blob),byteLength=96))
    blob.extend(struct.pack('<24f',*([0,1,0,0]*6)))
    rigid=dict(attrs,WEIGHTS_0=weights)
    mesh=doc['meshes'][0]['primitives']
    mesh.extend([dict(mesh[0],attributes=rigid,material=1),dict(mesh[0],attributes=rigid,material=2)])
    doc['images'],doc['textures']=[],[]
    doc['samplers']=[dict(wrapS=33071,wrapT=33648,minFilter=9728,magFilter=9728)]
    for rgba in ([9,205,91,255],[205,108,44,245],[128,128,255,255]):
        blob.extend(b'\0'*(-len(blob)%4))
        image=png(2,2,rgba*4)
        doc['images'].append(dict(bufferView=len(views),mimeType='image/png'))
        doc['textures'].append(dict(source=len(doc['images'])-1,sampler=0))
        views.append(dict(buffer=0,byteOffset=len(blob),byteLength=len(image)))
        blob.extend(image)
    doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']=dict(index=0)
    doc['materials'].extend([
        dict(alphaMode='MASK',alphaCutoff=.37,doubleSided=True,emissiveFactor=[.1,.2,.3],
             normalTexture=dict(index=2,scale=.8),occlusionTexture=dict(index=2,strength=.3),
             pbrMetallicRoughness=dict(baseColorTexture=dict(index=1),metallicFactor=.65,roughnessFactor=.35)),
        dict(normalTexture=dict(index=2,scale=.3),pbrMetallicRoughness=dict(
             baseColorTexture=dict(index=1),baseColorFactor=[.77,.31,.19,1],metallicFactor=.5,roughnessFactor=.8)),
    ])
    doc['buffers'][0]['byteLength']=len(blob)
    return glb(doc,bytes(blob)),bind


def cooked(path):
    data=path.read_bytes()
    assert hashlib.sha256(data).hexdigest()==path.stem
    assert data[:8] in (b'POIMAM04',b'POIMAM05',b'POIMAM03')
    header_length,bytes_length=struct.unpack_from('<II',data,8)
    assert len(data)==16+header_length+bytes_length
    metadata=json.loads(data[16:16+header_length])
    offset=16+header_length
    vertices,indices=[],[]
    for primitive in metadata['primitives']:
        rows=primitive['vertices']
        vertices.append([struct.unpack_from('<12f',data,offset+i*48) for i in range(rows)])
        offset+=rows*(80 if primitive.get('skinned',False) else 48)
        indices.append(struct.unpack_from(f'<{primitive["indices"]}I',data,offset))
        offset+=primitive['indices']*4
    assert offset==16+header_length+metadata['geometry_bytes']
    offset+=metadata.get('animation_bytes',0)
    images=[]
    for image in metadata['images']:
        begin=offset
        offset+=sum(level['bytes'] for level in image['mips'])
        images.append(data[begin:offset])
    assert offset==len(data)
    return metadata,vertices,indices,images


def near(left,right,tolerance=2e-6):
    assert len(left)==len(right)
    difference=max(abs(a-b) for a,b in zip(left,right))
    assert difference<=tolerance,(left,right,difference,tolerance)
    return difference


def normalized(vector):
    magnitude=math.hypot(*vector)
    return [v/magnitude for v in vector]


def verify_bake(source,extracted,bind):
    before,original_vertices,original_triangles,original_images=source
    after,vertices,triangles,images=extracted
    assert after['importer']=='poima-rigid-mesh-extract-1' and after['version']==5 and not after['animated']
    assert after['roots']==[0] and len(after['nodes'])==1 and after['nodes'][0]['primitives']==[0,1]
    assert after['nodes'][0]['position']==[0,0,0] and after['nodes'][0]['rotation']==[0,0,0,1]
    assert len(images)<len(original_images) and 'skins' not in after and 'animations' not in after
    maximum=0
    for output,original in ((0,1),(1,2)):
        old=before['primitives'][original]
        new=after['primitives'][output]
        for key in ('indices','vertices','material','has_uv','occlusion_strength','normal_scale'):
            assert new[key]==old[key],key
        assert not new.get('skinned',False) and triangles[output]==original_triangles[original]
        for a,b in zip(old['textures'],new['textures']):
            if a is None:
                assert b is None
                continue
            assert b is not None and {k:v for k,v in a.items() if k!='image'}=={k:v for k,v in b.items() if k!='image'}
            assert before['images'][a['image']]==after['images'][b['image']]
            assert original_images[a['image']]==images[b['image']]
        for a,b in zip(original_vertices[original],vertices[output]):
            normal=normalized([2*a[5],a[4]/3,-.5*a[3]])
            tangent=[.5*a[10],3*a[9],-2*a[8]]
            projection=sum(x*y for x,y in zip(normal,tangent))
            tangent=normalized([v-projection*n for v,n in zip(tangent,normal)])
            assert a[6:8]==b[6:8] and a[11]==b[11]
            maximum=max(maximum,near(b[:3],point(bind,a[:3])),near(b[3:6],normal),near(b[8:11],tangent))
    return maximum


def runtime(io,source,extracted):
    joint=hashlib.sha256(f'poima.instance.v1/{MESH_ROOT}/node/{JOINT}'.encode()).hexdigest()[:32]
    ops=[dict(op='asset.instantiate',id=MESH_ROOT,name='Weighted prop source',asset=source),
         dict(op='asset.instantiate',id=ITEM,name='Independent rigid item',asset=extracted,parent=joint)]
    ops.append(native.component(MESH_ROOT,'AnimationRig',dict(asset=source,clip=0,time=0,playing=True,speed=1,loop=False)))
    revision=native.transact(io,ops,0)['revision']
    owner=id_request();io.rpc('runtime.start',dict(session_id=owner,revision=revision))
    tick,structure=0,0

    def matrix(entity):
        return io.rpc('runtime.entity',dict(session_id=owner,tick=tick,structure_revision=structure,id=entity))['world_matrix']

    def sample(entity,vertex):
        return io.rpc('runtime.surface.measure',dict(session_id=owner,tick=tick,structure_revision=structure,pairs=[
            dict(source=dict(entity=entity,vertex=vertex),target=dict(entity=entity,triangles=[0]))]))['pairs'][0]['source']['point_m']

    def primitive(root,node,index):
        return hashlib.sha256(f'poima.instance.v1/{root}/node/{node}/primitive/{index}'.encode()).hexdigest()[:32]

    worst=0
    for next_tick in (0,12,24):
        if next_tick>tick:
            io.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
                expected_structure_revision=structure,ticks=next_tick-tick))
            tick=next_tick
        for i in range(2):
            for vertex in (0,2,5):
                left=sample(primitive(MESH_ROOT,0,i+1),vertex)
                right=sample(primitive(ITEM,0,i),vertex)
                worst=max(worst,near(left,right))
    prior=matrix(ITEM)
    r=io.rpc('runtime.attachment.apply',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
        expected_structure_revision=structure,commands=[dict(entity=ITEM,mode='keep_world',anchor=None)]))
    structure=r['structure_revision']
    near(matrix(ITEM),prior,1e-10)
    io.rpc('runtime.step',dict(session_id=owner,request_id=id_request(),expected_tick=tick,
          expected_structure_revision=structure,ticks=12))
    tick+=12
    near(matrix(ITEM),prior,1e-10)
    assert math.dist(matrix(joint)[12:15],prior[12:15])>.1
    io.rpc('runtime.stop',dict(session_id=owner))
    return worst


def run(args):
    output=args.output.resolve()
    output.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+180
    report=dict(passed=False,engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    io=native.Session(output/'session')
    try:
        request_schema=io.rpc('world.describe',dict(view='method',name=EXTRACT))
        assert request_schema['schema_revision']>=144
        assert request_schema['methods'][EXTRACT]['properties']['primitives']['uniqueItems']
        original,bind=source_glb()
        path=output/'original.glb';path.write_bytes(original)
        source_asset=io.rpc('asset.import',dict(source=native.native(path)))['asset']
        path.unlink()
        store=io.world.with_suffix('.json.assets')
        base=store/(source_asset+'.pmodel')
        def authored_bytes():
            return io.world.read_bytes() if io.world.is_file() else None
        authored=authored_bytes()
        def extract(primitives, error=None, **overrides):
            return io.rpc(EXTRACT,dict(asset=source_asset,node=SOURCE_NODE,joint=JOINT,primitives=primitives)|overrides,error)
        extract([],error=-32602)
        extract([1,1],error=-32602)
        extract([1.0],error=-32602)
        extract([1],error=-32602,node=JOINT)
        extract([1],error=-32602,joint=SOURCE_NODE)
        extract([1,8],error=-32602)
        mixed=extract([0,1],error=-32602)['data']
        assert mixed==dict(primitive=0,vertex=0,joint=JOINT,reason='non_rigid_skin_weights'),mixed
        assert list(store.glob('*.pmodel'))==[base] and authored_bytes()==authored
        r=extract([2,1])
        assert r==extract([1,2]) and r['frame']=='joint_local'
        assert r['joint']==JOINT and r['source_asset']==source_asset and r['source_node']==SOURCE_NODE
        assert r['primitives']==[dict(source_primitive=1,primitive=0),dict(source_primitive=2,primitive=1)]
        assert authored_bytes()==authored and hashlib.sha256(base.read_bytes()).hexdigest()==source_asset
        extracted=store/(r['asset']+'.pmodel')
        assert r['bytes']==extracted.stat().st_size
        report['payload_maximum_error']=verify_bake(cooked(base),cooked(extracted),bind)
        report['live_vertex_maximum_error_m']=runtime(io,source_asset,r['asset'])
        report.update(source_asset=source_asset,extracted_asset=r['asset'],passed=True)
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        io.close()
        report.update(calls=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Rigid extraction verified in {report["calls"]} native calls.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    if args.output is not None:
        run(args)
    else:
        with tempfile.TemporaryDirectory(prefix='poima-rigid-extraction-',dir=args.binary.resolve().parent) as work:
            args.output=Path(work)/'run'
            run(args)


if __name__=='__main__':
    main()
