#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real imported MASK world, live adoption and source-removed Vulkan captures.

Native Windows Python under an external owned Job. No build, player host,
compiled gameplay, receipt normalization or save/asset patching is performed.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools/python'),str(ROOT/'tests')]
from poima_client import WorldClient,RpcError
from material_alpha_contract import need,uid,encoded,same,pin,inventory,component,transform,differences,windows_job
from switchback_oracle import strict_json
from texture_fixture import quad,png
from gltf_fixture import glb
from scene_capture import pixels

WIDTH,HEIGHT=320,240
CAM,BACK,ENV,ROOT_ID=(f'{n:032x}'for n in(1,2,3,100))
SUBJECT=hashlib.sha256(('poima.instance.v1/'+ROOT_ID+'/node/0/primitive/0').encode()).hexdigest()[:32]
RGBA=bytes([255,255,255,0,255,255,255,64,255,255,255,128,255,255,255,255])
ALPHAS=(0,64,128,255)
MATERIAL=dict(base_color=[1.,1.,1.],emissive=[.5,0.,0.],metallic=0.,roughness=1.,double_sided=True)
MASK={**MATERIAL,'alpha_mode':'mask','opacity':1.,'alpha_cutoff':.5}
OPAQUE={**MATERIAL,'alpha_mode':'opaque','opacity':.25,'alpha_cutoff':.75}


def compare(a,b):
    need(len(a)==len(b)==HEIGHT and all(len(r)==WIDTH for r in a+b),'Original image extent differs')
    maximum=strong=0
    for ar,br in zip(a,b):
        for x,y in zip(ar,br):
            d=max(abs(i-j)for i,j in zip(x,y));maximum=max(maximum,d);strong+=d>=2
    return dict(max_rgb_delta=maximum,strong_changed_pixels=strong)


def coverage(masked,opaque,discarded):
    """Independent pinhole/UV and original PNG alpha; skip raster/texel edges."""
    kept=holes=maximum=0;tangent=math.tan(math.pi/6);aspect=WIDTH/HEIGHT
    for y in range(HEIGHT):
        for x in range(WIDTH):
            px=((x+.5)/WIDTH*2-1)*4*tangent*aspect;py=(1-(y+.5)/HEIGHT*2)*4*tangent
            if abs(px)>=.94 or abs(py)>=.94:continue
            u=(px+1)/2;v=(1-py)/2
            if abs(u-.5)<.04 or abs(v-.5)<.04:continue
            keep=ALPHAS[int(v*2)*2+int(u*2)]/255>=.5
            ref=opaque[y][x]if keep else discarded[y][x]
            maximum=max(maximum,max(abs(a-b)for a,b in zip(masked[y][x],ref)))
            if keep:kept+=1
            else:holes+=1
    need(kept>500 and holes>500 and maximum<=1,'Imported PNG alpha differs from independent projected opaque/discarded references')
    effect=compare(opaque,masked);need(effect['strong_changed_pixels']>=500 and effect['max_rgb_delta']>=20,'Masked holes have no meaningful visible effect')
    return dict(kept_pixels=kept,hole_pixels=holes,max_rgb_error=maximum,effect=effect)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in('binary','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--gpu',type=int,required=True);a=p.parse_args()
    need(os.name=='nt'and not sys.flags.optimize and a.gpu>=0,'Native nonoptimized Windows Python and explicit GPU required');windows_job()
    a.binary=a.binary.resolve(strict=True);a.output=a.output.absolute();need(not a.output.exists()and a.output.parent.is_dir(),'Fresh owned output/existing parent required')
    a.output.mkdir();(a.output/'captures').mkdir();saves=a.output/'saves';saves.mkdir();sources=a.output/'sources';sources.mkdir();world=a.output/'world.json';store=Path(str(world)+'.assets')
    started=time.monotonic();deadline=started+180;work_deadline=deadline-10;client=None;session=uid();revision=0;actions=[]
    r=dict(format='poima.material-alpha-live-capture.v1',passed=False,errors=[],calls=[],owners=[],captures=[],checkpoints=[],whole_pairs=[],adoptions=[],comparisons=[],limits=dict(seconds=180,rpc=120,bytes=64*1048576,files=512,owners=2,captures=9),scope='Actual synthetic imported M05/world/native-material adoption/Vulkan parity, full original native saves and source-removed cold reopen. No compiled gameplay, player continuity, performance, alpha-aware collision, AOT or imported-art claim.')
    files=[Path(__file__),ROOT/'tests/material_alpha_contract.py',ROOT/'tests/switchback_oracle.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/texture_fixture.py',ROOT/'tests/scene_capture.py',a.binary]
    files += [f for f in(ROOT/'tools/python/poima_client').iterdir()if f.is_file()and f.suffix in('.py','.json')]
    if(a.binary.parent/'phonon.dll').is_file():files.append(a.binary.parent/'phonon.dll')
    pins={str(f):pin(f)for f in files};r['source_pins']=pins
    def bound():
        rows=[q for q in a.output.rglob('*')if q.is_file()];need(len(rows)<=512 and sum(q.stat().st_size for q in rows)+len(encoded(r))+1<=64*1048576,'Observed64MiB/512file bound')
    def rpc(method,params=None,error=None):
        need(time.monotonic()<work_deadline and len(r['calls'])<120,'Original120RPC/180s cleanup-reserved bound');row=dict(method=method,params=copy.deepcopy(params or {}),owner=len(r['owners']));r['calls'].append(row)
        try:value=client.call(method,params or {},timeout=min(40,max(.001,work_deadline-time.monotonic())))
        except RpcError as e:
            row['error']=dict(code=e.code,message=e.message,data=e.data);need(type(error)is int and e.code==error,'Unexpected native rejection');return row['error']
        except BaseException as e:row['transport_error']=repr(e);raise
        row['result']=value;bound();need(error is None and time.monotonic()<work_deadline,'Missing rejection or late native outcome');return value
    def open_owner(label):
        nonlocal client,actions
        actions=[];client=WorldClient.open(str(a.binary),str(world),cwd=ROOT,trace_directory=str(a.output/label),trace_limit_bytes=16*1048576,close_timeout=5)
        r['owners'].append(dict(label=label,pid=client.transport.process_id,closed=False,cleanup_actions=actions))
        for action in('terminate','kill'):
            original=getattr(client.transport._process,action)
            def observe(*args,_action=action,_original=original,**kw):actions.append(_action);return _original(*args,**kw)
            setattr(client.transport._process,action,observe)
    def close_owner():
        nonlocal client
        if client is None:return
        original=client
        try:original.close()
        finally:
            r['owners'][-1].update(closed=original.closed,exit_code=original.transport.returncode,cleanup_error=None if original.cleanup_error is None else str(original.cleanup_error),stderr=original.transport.stderr_tail,stderr_truncated=original.transport.stderr_truncated)
            need(original.closed and original.transport.returncode==0 and original.cleanup_error is None and not actions and not original.transport.stderr_truncated,'Natural lossless native EOF0 closure required');client=None
    def tx(ops):
        nonlocal revision
        v=rpc('world.transact',dict(request_id=uid(),base_revision=revision,ops=ops));revision=v['revision'];return v
    def status():return rpc('runtime.inspect',dict(session_id=session))
    def checkpoint(label):
        s=status();receipt=rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,session_id=session,expected_tick=s['tick'],expected_gameplay_revision=0,expected_structure_revision=s['structure_revision'],expected_material_revision=s['material_revision'],expected_ui_revision=s['ui_revision'],expected_control_sequence=s['control_sequence']))
        current=strict_json((saves/('slot-'+label)/'current.json').read_bytes())['payload']['current'];path=saves/('slot-'+label)/current['file'];raw=path.read_bytes();identity=pin(path)
        need(identity=={k:receipt[k]for k in('bytes','sha256')}and identity=={k:current[k]for k in('bytes','sha256')}and current['generation']==receipt['generation']==1,'Complete original save/receipt/manifest join differs')
        full=strict_json(raw);need(set(full)=={'format','version','document','snapshot'}and full['format']=='poima.world-save'and full['version']==1,'Unexpected complete checkpoint envelope')
        r['checkpoints'].append(dict(label=label,path=str(path.relative_to(a.output)),identity=identity,full=full));return raw,full
    def capture(label,live,epoch):
        before,old=checkpoint(label+'-before');authored=pin(world);path=a.output/'captures'/(label+'.bmp')
        params=dict(camera=CAM,path=str(path),gpu=a.gpu,width=WIDTH,height=HEIGHT,samples=1,lighting_path='forward',frames_in_flight=2,capture_frames=2,ambient_occlusion=dict(mode='none'),reconstruction='none')
        if live:params.update(session_id=session,tick=0,material_revision=epoch)
        else:params['revision']=revision
        value=rpc('runtime.capture'if live else'world.capture',params)
        need(value['build_version']=='0.0.209'and value['hardware']is True and value['capture_written']is True and value['nvrhi_errors']==0 and value['width']==WIDTH and value['height']==HEIGHT and value['samples']==1,'Actual209 hardware capture/extent differs')
        need(value['source']==('runtime'if live else'authored')and value['material_revision']==epoch and value['object_count']==2,'Actual capture source/material/object identity differs')
        d=value['render_diagnostics'];receipts=d['scene_receipts'];submitted,presented=receipts['submitted'],receipts['presented']
        need(type(presented)is dict and same(submitted,presented)and type(presented['frame_sequence'])is int and presented['frame_sequence']>0 and type(presented['submission_id'])is int and presented['submission_id']>0,'Original submitted/presented immutable scene join differs')
        need(presented['camera_id']==CAM and presented['world_id']==value['world_id']and presented['material_revision']==epoch and presented['authored_revision']==value['revision'],'Actual submitted world/camera/material revision differs')
        need(d['lighting_path']=='forward'and d['ambient_occlusion']['mode']=='none'and d['reconstruction']['mode']=='none'and d['last_draws']['camera_draws']==2,'Actual controlled rendering recipe/draw count differs')
        if live:need(value['session_id']==session and value['tick']==0 and presented['presentation_generation']==epoch,'Capture changed live identity/clock/presentation epoch')
        need(pin(world)==authored,'GPU capture changed authored bytes');image=pixels(path);after,new=checkpoint(label+'-after');need(before==after,'Capture changed complete original native save bytes')
        r['whole_pairs'].append(dict(label=label,before=label+'-before',after=label+'-after',bytes_equal=True,exclusions=[]));r['captures'].append(dict(label=label,image=dict(path=str(path.relative_to(a.output)),identity=pin(path)),result=value))
        return dict(image=image,before=before,after=after,old=old,new=new,source=presented['presentation_source_id'])
    def parity(label,left,right):
        q=compare(left['image'],right['image']);need(q['max_rgb_delta']<=1,'Actual authored/live full-image parity differs: '+label);r['comparisons'].append(dict(label=label,**q))
    def adopt(value,original):
        s=status();params=dict(request_id=uid(),session_id=session,expected_tick=0,expected_structure_revision=s['structure_revision'],expected_material_revision=s['material_revision'],revision=revision,expected_authored_revision=s['authored_revision'],expected_content_sha256=s['content_sha256'],ids=[SUBJECT]);receipt=rpc('runtime.material.apply',params)
        need(receipt['tick']==0 and receipt['session_id']==session and receipt['replayed']is False and receipt['material_revision']==s['material_revision']+1,'Actual guarded material adoption receipt differs')
        need(same(rpc('runtime.material.apply',params),{**receipt,'replayed':True}),'Exact material retry failed')
        return dict(params=params,receipt=receipt,before=original,value=value)
    def adoption_pair(change,after):
        delta=differences(change['before']['new'],after['old']);allowed={('document','revision'),('snapshot','sha256'),('snapshot','payload','authored_revision'),('snapshot','payload','content_sha256')}
        allowed|={('document','entities',SUBJECT,'components','PbrMaterial',k)for k in('alpha_mode','opacity','alpha_cutoff')}
        need(delta and all(tuple(v['path'])in allowed for v in delta),'Alpha adoption changed another original full native save field')
        need(same(after['old']['document']['entities'][SUBJECT]['components']['PbrMaterial'],change['value']),'Adopted alpha fields absent from complete frozen document')
        r['adoptions'].append(dict(params=change['params'],receipt=change['receipt'],complete_field_differences=delta,allowed_exact_paths=[list(p)for p in sorted(allowed)]))
    try:
        open_owner('live-stdio');revision=rpc('world.inspect')['revision']
        m=dict(alphaMode='MASK',alphaCutoff=.5,doubleSided=True,emissiveFactor=[.5,0.,0.],pbrMetallicRoughness=dict(baseColorFactor=[1.,1.,1.,1.],baseColorTexture=dict(index=0),metallicFactor=0.,roughnessFactor=1.))
        doc,blob=quad([png(2,2,RGBA)],m);source=sources/'original-alpha.glb';source.write_bytes(glb(doc,blob));source_id=pin(source);asset=rpc('asset.import',dict(source=str(source)))['asset']
        package=store/(asset+'.pmodel');raw=package.read_bytes();h,n=struct.unpack_from('<II',raw,8);header=strict_json(raw[16:16+h]);need(raw[:8]==b'POIMAM05'and header['version']==5 and 16+h+n==len(raw),'Actual imported M05 required')
        need(raw[16+h+header['geometry_bytes']:16+h+header['geometry_bytes']+16]==RGBA,'Original cooked RGBA changed');r['imported']=dict(source=source_id,asset=asset,package=pin(package),header=header,original_rgba=list(RGBA))
        ops=[]
        for id,name in[(CAM,'Alpha camera'),(BACK,'Green emissive background'),(ENV,'Authored lighting')]:ops += [dict(op='entity.create',id=id,name=name),component(id,'Transform',transform())]
        camera=transform();camera['position']=[0.,0.,4.];background=transform();background['position']=[0.,0.,-2.];background['scale']=[8.,8.,.1]
        ops += [component(CAM,'Transform',camera),component(CAM,'Camera',dict(vertical_fov=60,near=.1,far=40)),component(BACK,'Transform',background),component(BACK,'MeshRenderer',dict(primitive='box',visible=True,albedo=[0.,1.,0.])),component(BACK,'PbrMaterial',dict(base_color=[0.,0.,0.],emissive=[0.,.5,0.],metallic=0.,roughness=1.,double_sided=False)),component(ENV,'LightingEnvironment',dict(ambient=[0.,0.,0.],exposure=1.)),dict(op='asset.instantiate',id=ROOT_ID,asset=asset,name='Original masked card')]
        tx(ops);need(same(rpc('entity.material',dict(id=SUBJECT))['material'],MASK),'Imported effective alpha intent differs')
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)));rpc('runtime.start',dict(session_id=session,revision=revision))
        masked=capture('authored-mask',False,0);initial=capture('runtime-mask',True,0);parity('initial-import-world-runtime',masked,initial)
        tx([component(SUBJECT,'PbrMaterial',{**MASK,'alpha_cutoff':1e6})]);discarded=capture('authored-discarded',False,0)
        tx([component(SUBJECT,'PbrMaterial',OPAQUE)]);opaque=capture('authored-opaque',False,0);stale=capture('runtime-stale-mask',True,0);parity('frozen-runtime-despite-authoring',initial,stale)
        need(status()['source_stale']is True,'Authored alpha edit did not mark stale source');r['coverage']=coverage(masked['image'],opaque['image'],discarded['image'])
        change=adopt(OPAQUE,stale);live_opaque=capture('runtime-adopted-opaque',True,1);adoption_pair(change,live_opaque);parity('adopted-opaque-vs-authored',opaque,live_opaque)
        need(live_opaque['source']==initial['source'],'Live alpha adoption replaced the runtime presentation source')
        tx([component(SUBJECT,'PbrMaterial',MASK)]);new_mask=capture('authored-mask-return',False,0);parity('authored-mask-return',masked,new_mask)
        change=adopt(MASK,new_mask);live_mask=capture('runtime-adopted-mask',True,2);adoption_pair(change,live_mask);parity('adopted-mask-vs-authored',new_mask,live_mask)
        need(live_mask['source']==initial['source']and status()['source_stale']is False,'Adoption source/epoch did not resolve')
        original_packages=inventory(store);rpc('session.close');close_owner();sources.rename(a.output/'removed-sources');need(pin(a.output/'removed-sources/original-alpha.glb')==source_id and not sources.exists(),'Original source removal did not preserve bytes')
        open_owner('cold-stdio');revision=rpc('world.inspect')['revision'];rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)));session=uid()
        rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot='runtime-adopted-mask-after',expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_component_revision=None,expected_structure_revision=None,expected_material_revision=None,expected_ui_revision=None,expected_control_sequence=None,new_session_id=session))
        cold=capture('cold-restored-mask',True,0);need(live_mask['after']==cold['before'],'Source-removed cold restore changed complete original save');r['whole_pairs'].append(dict(label='source-removed-cold',before='runtime-adopted-mask-after',after='cold-restored-mask-before',bytes_equal=True,exclusions=[]));parity('source-removed-cold-rendered-mask',new_mask,cold)
        need(inventory(store)==original_packages,'Cold renderer replaced original cooked assets');rpc('session.close');close_owner();need(len(r['captures'])==9 and len(r['whole_pairs'])==10 and len(r['checkpoints'])==18,'Complete finite actual capture/save cohort missing');r['passed']=True
    except BaseException:r['passed']=False;r['errors'].append(traceback.format_exc())
    finally:
        try:close_owner()
        except BaseException:r['passed']=False;r['errors'].append('Native owner closure failed:\n'+traceback.format_exc())
        try:
            r['seconds']=time.monotonic()-started;r['changed_inputs']=[f for f,v in pins.items()if not Path(f).is_file()or pin(f)!=v];need(not r['changed_inputs']and r['seconds']<=180,'Original input/whole deadline fence');bound()
        except BaseException:r['passed']=False;r['errors'].append('Terminal fence failed:\n'+traceback.format_exc())
        (a.output/'evidence.json').write_bytes(encoded(r)+b'\n')
    print(json.dumps(dict(passed=r['passed'],rpc=len(r['calls']),captures=len(r['captures']),checkpoints=len(r['checkpoints']),whole_pairs=len(r['whole_pairs']),owners=len(r['owners']))));return 0 if r['passed']else 1


if __name__=='__main__':raise SystemExit(main())
