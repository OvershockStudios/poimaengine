#!/usr/bin/env python3
"""Real separate-FBX rotation retarget Vulkan consumer and normalized-source oracle.

Supply original Kenney Animated Characters Protagonists files; no download,
engine/C# build, controller movement or authored pose modification is performed.
Expected motion uses ORIGINAL normalized source/reference poses, independent
quaternion-chain/FK math, and immutable ORIGINAL target geometry/inverse binds.
This is not an independent FBX parser, contact solver or seamless-loop proof.
All imported owned source copies are removed before a fresh capture owner opens.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid

from animation_rotation_retarget import (column_major, conjugate, from_columns,
    inverse, matrix, multiply, paths, point, product, qchains, rotated, unit, worlds)
from gltf_fixture import glb
from scene_capture import pixels

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

WIDTH,HEIGHT,TIME,TICKS=640,480,.25,15
POLICY='reference-rotation-v1'
ORIGINAL_SHA256=dict(
    base='18835fef534eede635b081ee7fe647d01a885550a591d2e6bf071010906167d8',
    run='e635461fc8dace85ec67a7f7941e949a7c3f108b51ae4d2da1557e6e01749df8',
    idle='c8a24e0294376ee5a195c56752a13310e1c0b5f8588a4db50e094120e3e4cc74')
LICENSE_SHA256='68280323c6dca1f532c71fb248a6f344abed39a574278a2edfa27801aea3d0cd'


def need(value,message):
    if not value:raise AssertionError(message)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def normal_transform(m,n):
    inv=inverse(m)
    return unit([sum(inv[c][r]*n[c] for c in range(3)) for r in range(3)])
def transpose_transform(m,n):return unit([sum(m[c][r]*n[c] for c in range(3)) for r in range(3)])
def weighted(palette,joints,weights):
    return [[sum(weights[k]*palette[joints[k]][r][c] for k in range(4)) for c in range(4)] for r in range(4)]

def package_geometry(path,expected_asset):
    raw=path.read_bytes();need(hashlib.sha256(raw).hexdigest()==expected_asset,'Original cooked target hash differs')
    need(raw[:8]==b'POIMAM04' and 16<=len(raw)<=64*1024*1024,'Unsupported original target package')
    json_size,binary_size=struct.unpack_from('<II',raw,8)
    need(json_size<=8*1024*1024 and 16+json_size+binary_size==len(raw),'Invalid original package bounds')
    meta=json.loads(raw[16:16+json_size]);blob=raw[16+json_size:];geometry_size=meta['geometry_bytes']
    need(type(geometry_size) is int and 0<=geometry_size<=len(blob),'Invalid geometry extent')
    offset=0;primitives=[]
    for description in meta['primitives']:
        nv,ni=description['vertices'],description['indices'];need(type(nv)is int and type(ni)is int and 0<nv<=1000000 and 0<ni<=3000000 and ni%3==0,'Invalid original primitive budget')
        need(offset+nv*48+(nv*32 if description.get('skinned') else 0)+ni*4<=geometry_size,'Truncated original geometry')
        vertices=[struct.unpack_from('<12f',blob,offset+i*48) for i in range(nv)];offset+=nv*48
        influences=[]
        if description.get('skinned'):
            influences=[struct.unpack_from('<4I4f',blob,offset+i*32) for i in range(nv)];offset+=nv*32
        indices=list(struct.unpack_from('<'+'I'*ni,blob,offset));offset+=ni*4;need(all(i<nv for i in indices),'Original index exceeds vertex count')
        primitives.append(dict(description=description,vertices=vertices,influences=influences,indices=indices))
    need(offset==geometry_size,'Geometry parse did not consume exact original section')
    return dict(metadata=meta,primitives=primitives,geometry_sha256=hashlib.sha256(blob[:geometry_size]).hexdigest(),geometry_bytes=blob[:geometry_size])


def oracle_glb(meshes,materials,source_hashes):
    blob=bytearray();views=[];accessors=[];output=[]
    for mesh,material in zip(meshes,materials):
        start=len(blob);positions=mesh['positions'];normals=mesh['normals'];indices=mesh['indices']
        for p,n in zip(positions,normals):blob.extend(struct.pack('<6f',*p,*n))
        view=len(views);views.append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start,byteStride=24))
        position_accessor=len(accessors);accessors.append(dict(bufferView=view,componentType=5126,count=len(positions),type='VEC3',
            min=[min(p[k] for p in positions) for k in range(3)],max=[max(p[k] for p in positions) for k in range(3)]))
        normal_accessor=len(accessors);accessors.append(dict(bufferView=view,byteOffset=12,componentType=5126,count=len(normals),type='VEC3'))
        start=len(blob);blob.extend(struct.pack('<'+'I'*len(indices),*indices));view=len(views);views.append(dict(buffer=0,byteOffset=start,byteLength=len(indices)*4))
        index_accessor=len(accessors);accessors.append(dict(bufferView=view,componentType=5125,count=len(indices),type='SCALAR'))
        output.append(dict(attributes=dict(POSITION=position_accessor,NORMAL=normal_accessor),indices=index_accessor,material=len(output)))
    doc=dict(asset=dict(version='2.0',generator='Poima independent normalized-original-source rotation/skin oracle',
        extras=dict(source_hashes=source_hashes,source='Kenney Animated Characters Protagonists',license='CC0-1.0',
            changes='Original normalized geometry posed with independent quaternion-chain/FK/weighted skin math; no generative model')),
        buffers=[dict(byteLength=len(blob))],bufferViews=views,accessors=accessors,
        materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[*m['base_color'],1],metallicFactor=m['metallic'],roughnessFactor=m['roughness']),
                        emissiveFactor=m['emissive'],doubleSided=m['double_sided']) for m in materials],
        meshes=[dict(primitives=output)],nodes=[dict(name='Independent original target posed skin',mesh=0)],scenes=[dict(nodes=[0])],scene=0)
    return glb(doc,bytes(blob))


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--source-directory',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--samples',type=int,choices=(1,4),default=4);parser.add_argument('--timeout',type=int,default=1200);args=parser.parse_args()
    if args.gpu<0 or not 60<=args.timeout<=1800:parser.error('--gpu must be nonnegative; --timeout must be60..1800')
    args.binary=args.binary.resolve(strict=True);args.source_directory=args.source_directory.resolve(strict=True);run=args.output.resolve()
    if not args.binary.is_file() or run.exists():parser.error('--binary must be a file and --output must be new')
    originals=dict(base=args.source_directory/'Model/characterMedium.fbx',run=args.source_directory/'Animations/run.fbx',idle=args.source_directory/'Animations/idle.fbx')
    if not all(p.is_file() and sha(p)==ORIGINAL_SHA256[name] for name,p in originals.items()):
        parser.error('Supply the unmodified pinned Kenney Protagonists1.1 Model/characterMedium.fbx and Animations/{run,idle}.fbx')
    original_license=args.source_directory/'License.txt'
    if not original_license.is_file() or sha(original_license)!=LICENSE_SHA256:
        parser.error('Supply the original Kenney Protagonists1.1 CC0 License.txt')
    run.mkdir(parents=True);world=run/'rotation-retarget.world.json';sources=run/'owned-sources';sources.mkdir();deadline=time.monotonic()+args.timeout;owner=None
    record=dict(passed=False,binary_sha256=sha(args.binary),source_sha256=sha(Path(__file__)),
        dependencies_sha256={name:sha(Path(__file__).with_name(name)) for name in ('animation_rotation_retarget.py','rotation_retarget_fixture.py','frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')},
        source_files={name:dict(path=str(p),sha256=sha(p)) for name,p in originals.items()},calls=[],owners=[],checks=[],captures={},comparisons={},
        time=TIME,runtime_ticks=TICKS,gpu_index=args.gpu,samples=args.samples,
        oracle='Native normalized ORIGINAL source/reference poses plus independent quaternion chains/FK; immutable original target geometry/inverse binds; recovered baseline normals. Not an independent FBX parser.',
        limits=['No controller/compiled C# locomotion, root-motion extraction, IK, contact, seamless-loop, editor, OS screenshot, physical-input or performance qualification.'])

    def bounded(maximum=150):
        remaining=deadline-time.monotonic();need(remaining>0,'Capture deadline exhausted');return min(maximum,remaining)
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True,timeout=bounded(10)).strip() if args.windows_interop else value
    def call(method,params=None):
        row=dict(method=method,params=params or {});record['calls'].append(row)
        try:result=owner.call(method,row['params'],timeout=bounded())
        except RpcError as error:row['error']=dict(code=error.code,message=error.message,data=error.data);raise
        row['result']=result;return result
    def page(method,params):
        rows=[];offset=0
        for _ in range(1024):
            r=call(method,dict(params,offset=offset,limit=64));rows.extend(r['items']);nxt=r.get('next_offset')
            if nxt is None:return rows
            need(type(nxt)is int and nxt>offset,'Pagination did not advance');offset=nxt
        raise AssertionError('Bounded pagination exceeded')
    def close():
        nonlocal owner
        previous,owner=owner,None
        if previous is None:return
        row=dict(exit_code=None);record['owners'].append(row)
        try:previous.close()
        except BaseException as error:row['cleanup_error']=repr(error);raise
        finally:row.update(process_id=previous.transport.process_id,exit_code=previous.transport.returncode,stderr=previous.transport.stderr_tail,stderr_truncated=previous.transport.stderr_truncated)
        need(row['exit_code']==0 and not row['stderr'] and not row['stderr_truncated'],'Native owner did not exit cleanly')
    def state():
        store=Path(str(world)+'.assets')
        return (call('world.inspect'),call('world.history'),world.read_bytes(),{p.relative_to(store).as_posix():sha(p) for p in sorted(store.rglob('*')) if p.is_file()})
    def uid(value):return f'{value:032x}'
    def capture(name,method,params,mode=None,runtime=False):
        target=run/(name+'.bmp');need(not target.exists(),'Capture would overwrite file');before=world.read_bytes()
        r=call(method,dict(camera=uid(1),path=native(target),width=WIDTH,height=HEIGHT,gpu=args.gpu,samples=args.samples,profile=True,**params))
        need(world.read_bytes()==before,'Capture changed authored bytes');need(r['capture_written'] and r['hardware'] and r['nvrhi_errors']==0,'Vulkan hardware capture failed')
        need(r['object_count']==expected_objects and r['samples']==args.samples,'Wrong primitive/AA count')
        if mode is not None:need(r['animation']['skinning']==('gpu_compute' if mode=='gpu' else 'cpu_reference'),'Requested skinning mode not exercised')
        if mode is not None or runtime:
            gpu=mode=='gpu' or runtime;draws=r['render_diagnostics']['last_draws'];need(draws['skinned_instances']==(expected_objects if gpu else 0),'Wrong skinning instance count')
            if gpu:
                need(draws['skinned_vertices']==expected_vertices,'Wrong actual GPU skinned vertex count');need(r['render_diagnostics']['gpu']['skinning']['samples']>=1,'No actual GPU skinning timestamp')
        record['captures'][name]=dict(report=r,sha256=sha(target))
    def compare(name,left,right,limit=128):
        a,b=pixels(run/(left+'.bmp')),pixels(run/(right+'.bmp'));need(len(a)==len(b)==HEIGHT and all(len(r)==WIDTH for r in a+b),'Wrong capture extent')
        errors=[max(abs(x-y) for x,y in zip(p,q)) for ar,br in zip(a,b) for p,q in zip(ar,br)]
        r=dict(maximum_channel_error=max(errors),pixels_over_2=sum(e>2 for e in errors),changed_pixels=sum(e!=0 for e in errors),mean_max_channel_error=sum(errors)/len(errors));record['comparisons'][name]=r
        if limit is not None:need(r['pixels_over_2']<=limit and r['mean_max_channel_error']<=.1,'Raster comparison failed: '+name+' '+str(r))
        return r

    try:
        copies={}
        for name,path in originals.items():
            relative=path.relative_to(args.source_directory);copied=sources/relative;copied.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,copied);need(sha(copied)==record['source_files'][name]['sha256'],'Owned exact FBX copy differs');copies[name]=copied
        license_path=original_license
        if license_path.is_file():
            text=license_path.read_text(encoding='utf-8-sig');need('CC0' in text or 'Creative Commons Zero' in text,'Unexpected original asset license')
            shutil.copyfile(license_path,sources/'License.txt');record['license_sha256']=sha(license_path)
        owner=WorldClient.open(str(args.binary),native(world),close_timeout=10)
        infos={};nodes={};clips={}
        for name,path in copies.items():
            info=call('asset.source.inspect',dict(source=native(path)));need(info['published'] is False,'Source inspection published content');infos[name]=info
            nodes[name]=page('asset.source.inspect',dict(source=native(path),section='nodes',expected_model_sha256=info['model_sha256']))
            clips[name]=page('asset.source.inspect',dict(source=native(path),section='animations',expected_model_sha256=info['model_sha256']))
            originalinfo=call('asset.source.inspect',dict(source=native(originals[name]),expected_model_sha256=info['model_sha256']))
            need(originalinfo['model_sha256']==info['model_sha256'],'Owned source layout altered normalized original model')
        target=nodes['base'];targetindex={n['index']:n for n in target};targetpaths=paths(target);by_path={p:i for i,p in targetpaths.items()};targetchains=qchains(target);targetworld=worlds(target)
        baseline=call('asset.import',dict(source=native(copies['base'])));baseline_asset=baseline['asset'];need(baseline['animations']==0 and baseline['images']==0,'Unexpected original target animations/images')
        baselinepackage=package_geometry(Path(str(world)+'.assets')/(baseline_asset+'.pmodel'),baseline_asset)
        materials=page('asset.inspect',dict(asset=baseline_asset,section='primitives'))
        need(all(all(v is None for v in m['textures'].values()) for m in materials),'Textured target requires an explicit matching reference texture closure')
        selections=[];profiles={}
        for name in ('run','idle'):
            originalclips=clips[name];need(len(originalclips)==2 and 'Targeting Pose' in originalclips[0]['name'] and 'Targeting Pose' not in originalclips[1]['name'],'Original FBX take layout changed')
            need(originalclips[1]['duration']>=TIME,'Original take shorter than15fixed ticks')
            hips=[n['index'] for n in nodes[name] if n['name']=='Hips'];need(len(hips)==1,'Original Hips ID is ambiguous')
            policy=dict(policy=POLICY,source_pose=dict(kind='sample',clip=originalclips[0]['index'],time=0),target_pose=dict(kind='rest'),
                expected_source_model_sha256=infos[name]['model_sha256'],expected_target_model_sha256=infos['base']['model_sha256'],positions=dict(kind='reference_delta',nodes=hips,scale=1),scales='target_reference')
            selections.append(dict(source=native(copies[name]),clip=originalclips[1]['index'],name='Original-'+name,retarget=policy))
            profiles[name]=dict(reference=originalclips[0],motion=originalclips[1],hips=hips[0],policy=policy)
        converted=call('asset.import',dict(source=native(copies['base']),animations=selections));asset=converted['asset'];need(converted['animations']==2,'Selected separate FBX take count changed')
        convertedpackage=package_geometry(Path(str(world)+'.assets')/(asset+'.pmodel'),asset)
        need(convertedpackage['geometry_bytes']==baselinepackage['geometry_bytes'],'Retarget modified original target geometry/influences/indices')
        for key in ('nodes','skins','roots','primitives'):
            need(convertedpackage['metadata'][key]==baselinepackage['metadata'][key],'Retarget modified original target '+key)
        record['original_target_geometry']=dict(asset=baseline_asset,geometry_sha256=baselinepackage['geometry_sha256'],vertices=baseline['vertices'],triangles=baseline['triangles'])
        source=nodes['run'];sourcepaths=paths(source);sourceindex={n['index']:n for n in source};mapping={s:by_path[p] for s,p in sourcepaths.items() if p in by_path}
        ref=page('asset.source.inspect',dict(source=native(copies['run']),section='nodes',pose=profiles['run']['policy']['source_pose'],expected_model_sha256=infos['run']['model_sha256']))
        refindex={n['index']:n for n in ref};refchains=qchains(ref)
        motion=page('asset.source.inspect',dict(source=native(copies['run']),section='nodes',pose=dict(kind='sample',clip=profiles['run']['motion']['index'],time=TIME),expected_model_sha256=infos['run']['model_sha256']))
        motionindex={n['index']:n for n in motion};corrections={s:unit(product(conjugate(refchains[s]),targetchains[t])) for s,t in mapping.items()}
        wanted=copy.deepcopy(target);wantedindex={n['index']:n for n in wanted}
        for source_id,target_id in mapping.items():
            parent=sourceindex[source_id]['parent'];left=[0,0,0,1] if parent<0 else conjugate(corrections[parent])
            wantedindex[target_id]['rotation']=unit(product(product(left,motionindex[source_id]['rotation']),corrections[source_id]))
            if source_id==profiles['run']['hips']:
                delta=rotated(left,[x-y for x,y in zip(motionindex[source_id]['position'],refindex[source_id]['position'])]);wantedindex[target_id]['position']=[x+y for x,y in zip(targetindex[target_id]['position'],delta)]
        wantedworld=worlds(wanted);skins=page('asset.inspect',dict(asset=baseline_asset,section='skins'));joint_records={s['index']:page('asset.animation.skin',dict(asset=baseline_asset,skin=s['index'])) for s in skins}
        reference_meshes=[];reference_materials=[];expected_by_primitive={};recovery_position_error=0;recovery_normal_error=0
        for mesh in (n for n in target if n['primitives']):
            need(mesh['skin']>=0,'Unexpected unskinned original target primitive');records=joint_records[mesh['skin']]
            rest_palette=[multiply(multiply(inverse(targetworld[mesh['index']]),targetworld[j['node']]),from_columns(j['inverse_bind'])) for j in records]
            motion_palette=[multiply(multiply(inverse(wantedworld[mesh['index']]),wantedworld[j['node']]),from_columns(j['inverse_bind'])) for j in records]
            for primitive in mesh['primitives']:
                basevertices=page('asset.animation.sample',dict(asset=baseline_asset,time=0,section='vertices',node=mesh['index'],primitive=primitive))
                raw=baselinepackage['primitives'][primitive];need(len(basevertices)==len(raw['vertices']),'Original baseline vertex records differ')
                positions=[];normals=[]
                for index,(v,original) in enumerate(zip(basevertices,raw['vertices'])):
                    raw_influence=raw['influences'][index]
                    need(v['index']==index and v['joints']==list(raw_influence[:4]) and v['weights']==list(raw_influence[4:]) and v['uv']==list(original[6:8]),'Original baseline sample changed immutable source vertex/influence records')
                    weights,joints=v['weights'],v['joints'];restblend=weighted(rest_palette,joints,weights);restblend[3]=[0,0,0,1]
                    recovered=point(inverse(restblend),v['position']);recovery_position_error=max(recovery_position_error,max(abs(x-y) for x,y in zip(recovered,original[:3])))
                    raw_normal=transpose_transform(restblend,v['normal']);recovery_normal_error=max(recovery_normal_error,max(abs(x-y) for x,y in zip(raw_normal,unit(original[3:6]))))
                    # Original target package positions retain exact normalized
                    # input floats. Recovery above cross-checks the independent
                    # inverse-affine calculation against immutable baseline data.
                    blend=weighted(motion_palette,joints,weights)
                    positions.append(point(wantedworld[mesh['index']],point(blend,original[:3])))
                    normals.append(normal_transform(wantedworld[mesh['index']],normal_transform(blend,unit(original[3:6]))))
                reference_meshes.append(dict(positions=positions,normals=normals,indices=raw['indices']));reference_materials.append(materials[primitive]['material'])
                expected_by_primitive[(mesh['index'],primitive)]=dict(positions=positions,normals=normals,baseline=basevertices)
        need(recovery_position_error<=2e-5 and recovery_normal_error<=3e-5,'Original baseline inverse-affine/normal recovery differs')
        record['checks'].append(dict(kind='original_target_input_recovery',maximum_position_error=recovery_position_error,maximum_normal_error=recovery_normal_error,expected_normal_source='Immutable original target package normal; independently cross-checked by inverse-transpose baseline recovery'))
        expected_vertices=sum(len(m['positions']) for m in reference_meshes);expected_objects=len(reference_meshes)
        need(expected_vertices==baseline['vertices'] and expected_objects==baseline['primitives'],'Mesh binding/primitive accounting differs')
        for (node,primitive),expected in expected_by_primitive.items():
            actual=page('asset.animation.sample',dict(asset=asset,clip=0,time=TIME,loop=False,section='vertices',node=node,primitive=primitive));need(len(actual)==len(expected['positions']),'Retarget sample count differs')
            p_error=n_error=0
            for v,p,n,b in zip(actual,expected['positions'],expected['normals'],expected['baseline']):
                need(v['index']==b['index'] and v['joints']==b['joints'] and v['weights']==b['weights'] and v['uv']==b['uv'],'Original geometry/influences changed')
                p_error=max(p_error,max(abs(x-y) for x,y in zip(v['world_position'],p)))
                observed_normal=normal_transform(wantedworld[node],v['normal']);n_error=max(n_error,max(abs(x-y) for x,y in zip(observed_normal,n)))
            need(p_error<=8e-5 and n_error<=3e-5,'Retargeted CPU sample differs from original-source oracle')
            record['checks'].append(dict(kind='normalized_original_source_weighted_position_normal',node=node,primitive=primitive,maximum_position_error=p_error,maximum_normal_error=n_error))
        source_hashes={name:info['model_sha256'] for name,info in infos.items()};reference=sources/'original-source-oracle.glb';reference.write_bytes(oracle_glb(reference_meshes,reference_materials,source_hashes));reference_asset=call('asset.import',dict(source=native(reference)))['asset']
        all_positions=[p for m in reference_meshes for p in m['positions']];bounds=dict(min=[min(p[k] for p in all_positions) for k in range(3)],max=[max(p[k] for p in all_positions) for k in range(3)])
        center=[(a+b)/2 for a,b in zip(bounds['min'],bounds['max'])];halfheight=(bounds['max'][1]-bounds['min'][1])/2;halfwidth=(bounds['max'][0]-bounds['min'][0])/2
        distance=1.35*max(halfheight,halfwidth*HEIGHT/WIDTH)/math.tan(math.radians(20))+(bounds['max'][2]-center[2]);need(distance>0,'Degenerate original character bounds')
        camera=[center[0],center[1],center[2]+distance];far=max(100,distance*10)
        call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=[
            dict(op='entity.create',id=uid(1),name='Original character camera'),
            dict(op='component.set',id=uid(1),type='Transform',value=dict(position=camera,rotation=[0,0,0,1],scale=[1,1,1])),
            dict(op='component.set',id=uid(1),type='Camera',value=dict(vertical_fov=40,near=.01,far=far)),
            dict(op='asset.instantiate',id=uid(100),name='Independent original-source skin oracle',asset=reference_asset)]))
        record['oracle_bounds']=bounds;record['original_source_profiles']={name:dict(model_sha256=infos[name]['model_sha256'],**profiles[name]) for name in profiles}
        record['owned_source_files']={p.relative_to(sources).as_posix():sha(p) for p in sorted(sources.rglob('*')) if p.is_file()};committed=state();close()
        need(all(sha(originals[name])==row['sha256'] for name,row in record['source_files'].items()),'Caller original source changed')
        need(record['owned_source_files']=={p.relative_to(sources).as_posix():sha(p) for p in sorted(sources.rglob('*')) if p.is_file()},'Owned source changed before removal')
        shutil.rmtree(sources);need(not sources.exists(),'Owned imported source removal failed');owner=WorldClient.open(str(args.binary),native(world),close_timeout=10);reopened=state()
        need((reopened[0],reopened[2],reopened[3])==(committed[0],committed[2],committed[3]),'Fresh owner changed authored/cooked data')
        need(reopened[1]['session_local'] and reopened[1]['undo_count']==0 and reopened[1]['redo_count']==0,'Fresh owner did not reset only session-local history')
        preview=dict(revision=1,asset=asset,clip=0,loop=False)
        for moment,label in ((0,'initial'),(TIME,'posed')):
            for mode in ('gpu','cpu'):capture(label+'-'+mode,'asset.animation.capture',dict(preview,time=moment,skinning=mode),mode)
            compare(label+'_gpu_vs_cpu',label+'-gpu',label+'-cpu')
        capture('posed-unculled','asset.animation.capture',dict(preview,time=TIME,skinning='gpu',culling=False),'gpu')
        for mode in ('gpu','cpu'):capture('idle-'+mode,'asset.animation.capture',dict(preview,clip=1,time=TIME,skinning=mode),mode)
        compare('idle_gpu_vs_cpu','idle-gpu','idle-cpu')
        capture('independent','world.capture',dict(revision=1));capture('independent-unculled','world.capture',dict(revision=1,culling=False))
        compare('gpu_vs_normalized_original_source','posed-gpu','independent');compare('cpu_vs_normalized_original_source','posed-cpu','independent')
        compare('converted_culling','posed-gpu','posed-unculled',0);compare('reference_culling','independent','independent-unculled',0)
        motion_difference=compare('original_visible_motion','initial-gpu','posed-gpu',None);need(motion_difference['pixels_over_2']>100,'Original source motion is not visibly exercised')
        need(state()==reopened,'Read-only captures changed authored state/history/assets')
        call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[dict(op='entity.delete',id=uid(100),recursive=True),dict(op='asset.instantiate',id=uid(200),name='Original separate-FBX runtime rig',asset=asset)]));before_runtime=state();session=uid(900)
        call('runtime.start',dict(session_id=session,revision=2));stepped=call('runtime.step',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,ticks=TICKS,animations=[dict(entity=uid(200),clip=0,time=0,speed=1,loop=False,playing=True)]));need(stepped['tick']==TICKS,'Runtime did not advance15ordinary ticks')
        required=set()
        for records in joint_records.values():
            for j in records:
                n=j['node']
                while n>=0:required.add(n);n=targetindex[n]['parent']
        for node in sorted(required):
            identity=hashlib.sha256(f'poima.instance.v1/{uid(200)}/node/{node}'.encode()).hexdigest()[:32]
            entity=call('runtime.entity',dict(session_id=session,tick=TICKS,id=identity));error=max(abs(x-y) for x,y in zip(entity['world_matrix'],column_major(wantedworld[node])));need(error<=5e-5,'Ordinary runtime pose differs from normalized original source')
            record['checks'].append(dict(kind='ordinary_runtime_original_source_global_matrix',node=node,maximum_error=error))
        capture('runtime','runtime.capture',dict(session_id=session,tick=TICKS),runtime=True);capture('runtime-unculled','runtime.capture',dict(session_id=session,tick=TICKS,culling=False),runtime=True)
        compare('runtime_vs_original_source','runtime','independent');compare('runtime_vs_preview','runtime','posed-gpu');compare('runtime_culling','runtime','runtime-unculled',0)
        call('runtime.stop',dict(session_id=session));need(state()==before_runtime,'Runtime changed authored state/history/assets')
        need(not sources.exists(),'Owned removed sources reappeared');need(sha(args.binary)==record['binary_sha256'],'Native binary changed')
        need(all(sha(originals[name])==row['sha256'] for name,row in record['source_files'].items()),'Caller original source changed')
        need(sha(original_license)==LICENSE_SHA256,'Caller original license changed')
        record.update(passed=True,source_independent_cooked_reopen=True,source_pins_verified_before_owned_removal=True,original_files_preserved=True,
            cpu_reference=True,normalized_original_source_static_geometry_and_normals=True,ordinary_runtime_ticks=True,actual_gpu_vertices=expected_vertices)
    except BaseException as error:record['error']=repr(error)
    finally:
        try:close()
        except BaseException as error:record['passed']=False;record['close_error']=repr(error)
        (run/'evidence.json').write_text(json.dumps(record,indent=2,allow_nan=False)+'\n',encoding='utf-8');print(run/'evidence.json')
    return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
