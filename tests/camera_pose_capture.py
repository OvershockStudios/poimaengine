#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Six-frame Windows camera-pose observation; an external owned Job is required.

Three fresh 640x360 forward/MSAA4 captures: identity, foreground-only offset,
then world offset. RGB isolation is not depth/HDR scene-product qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'tests'))
from poima_client import WorldClient
from scene_capture import pixels
from texture_fixture import quad
from gltf_fixture import glb

W,H=640,360
CAP,FILE_CAP,RPC_CAP,SECONDS=128*1048576,512,100,180
CAM,FORE,MARK,WALL,ENV=(f'{n:032x}' for n in (1,21,20,22,30))
IDENTITY=dict(position=[0.0,0.0,0.0],rotation=[0.0,0.0,0.0,1.0])
VM=dict(position=[.12,0.0,0.0],rotation=[0.0,0.0,0.0,1.0])
WORLD=dict(position=[.35,0.0,0.0],rotation=[0.0,0.0,0.0,1.0])


def need(value,message):
    if not value:raise AssertionError(message)


def pin(path):
    path=Path(path);need(path.is_file() and not path.is_symlink(),'Ordinary retained file: '+str(path))
    with path.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    return dict(bytes=path.stat().st_size,sha256=digest)


def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()
def same(left,right):return encoded(left)==encoded(right)
def component(entity,kind,value):return dict(op='component.set',id=entity,type=kind,value=value)
def transform(entity,position,scale=(1,1,1)):
    return component(entity,'Transform',dict(position=list(position),rotation=[0,0,0,1],scale=list(scale)))


def color_mask(image,kind):
    if kind=='foreground':predicate=lambda r,g,b:r>70 and r>2*g and r>2*b
    else:predicate=lambda r,g,b:r>70 and g>70 and b<min(r,g)*.4
    return {(x,y)for y,row in enumerate(image)for x,(r,g,b)in enumerate(row)if predicate(r,g,b)}


def centroid(mask):
    need(len(mask)>=200,'Analytic colored silhouette is absent/small')
    return [sum(x for x,y in mask)/len(mask),sum(y for x,y in mask)/len(mask)]


def dilated(mask,radius=4):
    return {(x+dx,y+dy)for x,y in mask for dx in range(-radius,radius+1)
            for dy in range(-radius,radius+1)if 0<=x+dx<W and 0<=y+dy<H}


def compare_pixels(left,right,excluded):
    count=maximum=changed=strong=0
    for y in range(H):
        for x in range(W):
            if(x,y)in excluded:continue
            delta=max(abs(a-b)for a,b in zip(left[y][x],right[y][x]))
            count+=1;maximum=max(maximum,delta);changed+=delta>0;strong+=delta>=2
    return dict(pixels=count,max_rgb_delta=maximum,changed_pixels=changed,strong_changed_pixels=strong)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--engine-sha256',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--gpu',type=int,required=True)
    args=parser.parse_args();args.binary=args.binary.resolve();args.output=args.output.absolute()
    need(os.name=='nt' and not sys.flags.optimize,'Native Windows Python in root-owned Job required')
    need(re.fullmatch('[0-9a-f]{64}',args.engine_sha256) and pin(args.binary)['sha256']==args.engine_sha256,'Exact root-selected206 engine image')
    need(args.gpu>=0 and not args.output.exists() and args.output.parent.is_dir(),'One explicit GPU/fresh output/existing parent')
    sources=[Path(__file__),args.binary,*[ROOT/'tests'/name for name in('scene_capture.py','texture_fixture.py','gltf_fixture.py')]]
    sources += [p for p in (ROOT/'tools/python/poima_client').iterdir()if p.is_file()and p.suffix in('.py','.json')]
    if(args.binary.parent/'phonon.dll').is_file():sources.append(args.binary.parent/'phonon.dll')
    pins={str(p):pin(p)for p in sources}
    args.output.mkdir();(args.output/'captures').mkdir();(args.output/'saves').mkdir()
    started=time.monotonic();deadline=started+SECONDS;client=None;tick=0;session=uuid.uuid4().hex
    record=dict(format='poima.camera-pose-capture.v1',passed=False,errors=[],calls=[],captures=[],checkpoints=[],whole_pairs=[],
        source_pins=pins,recipe=dict(width=W,height=H,samples=4,viewmodel_samples=4,capture_frames=2,captures=3,
        maximum_frames_presented=6,gpu=args.gpu,lighting_path='forward',background_max_rgb_delta=1,mask_dilation_pixels=4),
        scope='Analytic emissive quads and actual hardware frames; separate world/foreground pose controls. RGB products only; no depth/HDR, gameplay/C#, physical input, foreground window interaction, latency or performance qualification.')
    cleanup=[]
    def cap():
        files=[p for p in args.output.rglob('*')if p.is_file()]
        need(len(files)<=FILE_CAP and sum(p.stat().st_size for p in files)+len(encoded(record))<=CAP,'128MiB/512file observed output cap')
    def rpc(method,params=None):
        need(len(record['calls'])<RPC_CAP and time.monotonic()<deadline,'100RPC/180second bound')
        row=dict(method=method,params=params or {});record['calls'].append(row)
        try:value=client.call(method,params or {},timeout=min(60,max(.001,deadline-time.monotonic())))
        except BaseException as error:row['error']=repr(error);raise
        row['result']=value;need(time.monotonic()<deadline,'Late original reply exceeds deadline');cap();return value
    def entity(identity):return rpc('runtime.entity',dict(session_id=session,tick=tick,id=identity))
    def pose():return rpc('runtime.camera_pose',dict(session_id=session,tick=tick,entity=CAM))['state']
    def checkpoint(label):
        receipt=rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=label,
            expected_generation=0,session_id=session,expected_tick=tick,expected_gameplay_revision=0,
            expected_component_revision=0,expected_structure_revision=0,expected_material_revision=0,
            expected_ui_revision=0,expected_control_sequence=0))
        directory=args.output/'saves'/('slot-'+label);manifest=directory/'current.json'
        selected=json.loads(manifest.read_bytes())['payload']['current'];name=selected['file']
        need(re.fullmatch(r'p-[0-9a-f]{32}\.bin',name),'Original native payload name')
        path=directory/name;identity=pin(path)
        need(identity=={k:receipt[k]for k in('bytes','sha256')}=={k:selected[k]for k in('bytes','sha256')},'Full original save receipt/payload join')
        raw=path.read_bytes();saved=json.loads(raw)
        need(saved['format']=='poima.world-save'and saved['version']==1 and saved['snapshot']['version']==7,'Exact camera-only fixture save profile')
        record['checkpoints'].append(dict(label=label,path=str(path.relative_to(args.output)),identity=identity,
            receipt=receipt,manifest=dict(path=str(manifest.relative_to(args.output)),identity=pin(manifest)),
            snapshot_members=list(saved['snapshot']['payload']),document_members=list(saved['document'])))
        return raw,saved
    try:
        source=args.output/'analytic.glb'
        material=dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0,roughnessFactor=1),doubleSided=True)
        source.write_bytes(glb(*quad([],material)));record['analytic_source']=pin(source)
        client=WorldClient.open(str(args.binary),str(args.output/'world.json'),trace_directory=str(args.output/'stdio'),trace_limit_bytes=8*1048576)
        process=client.transport._process
        for action in('terminate','kill'):
            original=getattr(process,action)
            def observed(*a,_action=action,_original=original,**kw):
                cleanup.append(dict(action=_action,monotonic=time.monotonic()));return _original(*a,**kw)
            setattr(process,action,observed)
        asset=rpc('asset.import',dict(source=str(source)))['asset'];revision=rpc('world.inspect')['revision'];ops=[]
        for identity,parent in((CAM,None),(FORE,CAM),(MARK,None),(WALL,None),(ENV,None)):
            ops += [dict(op='entity.create',id=identity,name='Camera-pose analytic '+identity,parent=parent),transform(identity,(0,0,0))]
        ops += [component(CAM,'Camera',dict(vertical_fov=60,near=.01,far=50,viewmodel=dict(vertical_fov=75,near=.01,far=10))),
                component(ENV,'LightingEnvironment',dict(ambient=[0,0,0],exposure=1))]
        for identity,position,scale,color in[(WALL,(0,0,-6),(20,15,1),[.03125]*3),
            (MARK,(-1,0,-4),(.3,.65,1),[.5,.5,0]),(FORE,(.10,-.07,-.5),(.10,.075,1),[.5,0,0])]:
            ops += [transform(identity,position,scale),component(identity,'StaticMesh',dict(asset=asset,primitive=0,visible=True,cast_shadows=False)),
                    component(identity,'PbrMaterial',dict(base_color=[0,0,0],emissive=color,metallic=0,roughness=1,double_sided=True))]
        ops.append(component(FORE,'CameraDraw',dict(camera=CAM,mode='viewmodel')))
        revision=rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops))['revision']
        world_identity=pin(args.output/'world.json');record['authored_world']=world_identity
        need(rpc('runtime.start',dict(session_id=session,revision=revision))['tick']==0,'Fresh runtime at0')
        rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(args.output/'saves')))
        images={};states={};base=None
        for label,command in[('baseline',None),('viewmodel',dict(entity=CAM,viewmodel=VM)),('world',dict(entity=CAM,world=WORLD))]:
            if command is not None:
                tick=rpc('runtime.step',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=tick,ticks=1,camera_poses=[command]))['tick']
            state=dict(pose=pose(),camera=entity(CAM),foreground=entity(FORE),marker=entity(MARK));states[label]=state
            if base is None:base=state
            need(same(state['pose']['base_world_matrix'],base['pose']['base_world_matrix']),'BaseWorld changed under presentation offsets')
            if label=='viewmodel':
                need(same(state['pose']['world'],IDENTITY)and same(state['pose']['viewmodel'],VM),'Foreground-only controls differ')
                need(same(state['camera']['world_matrix'],base['camera']['world_matrix'])and same(state['foreground']['world_matrix'],base['foreground']['world_matrix']),'Foreground control changed camera/logical foreground transform')
            if label=='world':
                need(same(state['pose']['world'],WORLD)and same(state['pose']['viewmodel'],VM),'World channel failed to retain independent foreground channel')
                need(state['camera']['world_matrix'][12:15]==[.35,0.0,0.0],'World pose not applied to camera')
            before,saved=checkpoint(label+'-before');path=args.output/'captures'/(label+'.bmp')
            capture=rpc('runtime.capture',dict(session_id=session,tick=tick,material_revision=0,camera=CAM,path=str(path),
                gpu=args.gpu,width=W,height=H,samples=4,viewmodel_samples=4,lighting_path='forward',frames_in_flight=2,
                capture_frames=2,ambient_occlusion=dict(mode='none'),reconstruction='none',profile=False))
            need(capture['build_version']=='0.0.206'and capture['hardware']is True and capture['capture_written']is True and capture['nvrhi_errors']==0,'Strict actual206 hardware capture')
            need(capture['width']==W and capture['height']==H and capture['samples']==4 and capture['frames_presented']==2 and capture['tick']==tick and capture['session_id']==session,'Exact capture/frame/owner recipe')
            need(same(capture['camera_world'],state['camera']['world_matrix']),'Capture camera differs from committed pose')
            diagnostics=capture['render_diagnostics'];receipts=diagnostics['scene_receipts']
            need(same(receipts['submitted'],receipts['presented']),'Submitted/presented actual receipt mismatch')
            need(receipts['presented']['camera_id']==CAM and receipts['presented']['authored_revision']==revision,'Presented immutable camera/revision mismatch')
            need(diagnostics['lighting_path']=='forward'and diagnostics['viewmodel']['rendered']is True and diagnostics['viewmodel']['samples']==4,'Actual foreground rendering/MSAA missing')
            need(diagnostics['viewmodel']['draws']['camera_draws']==1 and receipts['presented']['camera_draw']['viewmodels']==[dict(entity_id=FORE,incarnation=1)],'Exact one foreground draw membership missing')
            need(diagnostics['scene_products']['available']is False,'MSAA4 must not pretend single-sample depth/HDR products')
            after,_=checkpoint(label+'-after');need(before==after,'Capture changed complete original native saved bytes')
            record['whole_pairs'].append(dict(before=label+'-before',after=label+'-after',bytes_equal=True,no_exclusions=True))
            need(pin(args.output/'world.json')==world_identity,'Capture/controls mutated authored world')
            image=pixels(path);need(len(image)==H and all(len(row)==W for row in image),'Exact BMP extent')
            images[label]=image;red=color_mask(image,'foreground');yellow=color_mask(image,'marker')
            record['captures'].append(dict(label=label,image=str(path.relative_to(args.output)),identity=pin(path),result=capture,
                foreground=dict(pixels=len(red),centroid=centroid(red)),world_marker=dict(pixels=len(yellow),centroid=centroid(yellow)),state=state))
        base_red=color_mask(images['baseline'],'foreground');moved_red=color_mask(images['viewmodel'],'foreground')
        red_motion=centroid(moved_red)[0]-centroid(base_red)[0]
        need(red_motion>15 and len(base_red^moved_red)>500,'Foreground offset did not move actual silhouette right')
        excluded=dilated(base_red|moved_red);isolation=compare_pixels(images['baseline'],images['viewmodel'],excluded)
        need(isolation['pixels']>W*H*.75 and isolation['max_rgb_delta']<=1,'World/background RGB changed outside foreground+AA coverage')
        yellow0=color_mask(images['viewmodel'],'marker');yellow1=color_mask(images['world'],'marker')
        world_motion=centroid(yellow1)[0]-centroid(yellow0)[0]
        need(world_motion< -10 and len(yellow0^yellow1)>500,'World offset did not move actual world silhouette left')
        record.update(rgb_isolation=isolation,foreground_centroid_dx=red_motion,world_centroid_dx=world_motion,
            frames_presented=sum(c['result']['frames_presented']for c in record['captures']),state_observations=states,passed=True)
        need(record['frames_presented']==6,'Exactly six actual presented frames required')
    except BaseException:record['errors'].append(traceback.format_exc());record['passed']=False
    finally:
        if client is not None:
            try:client.close()
            except BaseException:record['errors'].append(traceback.format_exc());record['passed']=False
            try:
                record['native_owner']=dict(pid=client.transport.process_id,closed=client.closed,exit_code=client.transport.returncode,
                    cleanup_actions=cleanup,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),
                    stderr=client.transport.stderr_tail,stderr_truncated=client.transport.stderr_truncated,
                    scope='Direct SDK lifetime only; whole Job ownership/closure supplied by caller.')
                if not(client.closed and client.transport.returncode==0 and not cleanup and client.cleanup_error is None and not client.transport.stderr_truncated):record['passed']=False
            except BaseException:
                record['errors'].append('Terminal owner metadata failed:\n'+traceback.format_exc());record['passed']=False
        try:record['changed_sources']=[name for name,identity in pins.items()if not Path(name).is_file()or pin(name)!=identity]
        except BaseException:
            record['changed_sources']=None;record['errors'].append('Terminal input fence unavailable:\n'+traceback.format_exc());record['passed']=False
        record['seconds']=time.monotonic()-started
        if record['changed_sources']is None or record['changed_sources']or record['seconds']>SECONDS:record['passed']=False
        try:cap()
        except BaseException:record['errors'].append(traceback.format_exc());record['passed']=False
        try:(args.output/'evidence.json').write_bytes(encoded(record)+b'\n')
        except BaseException:
            print(json.dumps(dict(evidence_publication_failed=True,original_errors=record['errors'],publication_error=traceback.format_exc())),file=sys.stderr)
            raise
    print(json.dumps(dict(passed=record['passed'],captures=len(record['captures']),frames_presented=record.get('frames_presented'),evidence=str(args.output/'evidence.json'))))
    return 0 if record['passed']else 1


if __name__=='__main__':raise SystemExit(main())
