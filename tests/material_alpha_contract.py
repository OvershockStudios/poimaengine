#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded native alpha-material intake, authoring, adoption and full saves.

Run with native platform Python under an external owned process/job. This test
does not compile, render, load a compiled game or patch native/save responses.
"""
import argparse
import copy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools/python'),str(ROOT/'tests')]
from poima_client import WorldClient,RpcError
from gltf_fixture import glb
from texture_fixture import quad,png
from switchback_oracle import strict_json

BASE=dict(base_color=[.25,.5,.75],emissive=[0.,.125,.25],metallic=.25,roughness=.75,double_sided=False)
RGBA=bytes([255,32,64,0,64,255,32,64,32,64,255,128,255,255,255,255])
BODY,TYPE,FIELD=f'{1:032x}',f'{20:032x}',f'{21:032x}'


def need(ok,message):
    if not ok:raise AssertionError(message)
def uid():return uuid.uuid4().hex
def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()
def same(a,b):return encoded(a)==encoded(b)
def pin(path):
    p=Path(path);need(p.is_file()and not p.is_symlink(),'Ordinary retained file required: '+str(p))
    data=p.read_bytes();return dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
def inventory(path):return {str(p.relative_to(path)):pin(p)for p in sorted(path.rglob('*'))if p.is_file()}if path.exists()else None
def component(entity,kind,value):return dict(op='component.set',id=entity,type=kind,value=value)
def transform(y=0):return dict(position=[0.,float(y),0.],rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])


def differences(a,b,path=()):
    """Retain each original unequal operand/type; never delete comparison fields."""
    if type(a)is not type(b):return [dict(path=list(path),before=a,after=b,kind='type')]
    if type(a)is dict:
        rows=[]
        for key in sorted(a.keys()|b.keys()):
            if key not in a or key not in b:rows.append(dict(path=list(path+(key,)),before_present=key in a,after_present=key in b,before=a.get(key),after=b.get(key),kind='member'))
            else:rows.extend(differences(a[key],b[key],path+(key,)))
        return rows
    if type(a)is list:
        if len(a)!=len(b):return [dict(path=list(path),before=a,after=b,kind='length')]
        return [row for i,(x,y)in enumerate(zip(a,b))for row in differences(x,y,path+(i,))]
    return []if same(a,b)else[dict(path=list(path),before=a,after=b,kind='value')]


def effective(value):
    alpha=dict(alpha_mode=value.get('alpha_mode','opaque'),opacity=value.get('opacity',1.),alpha_cutoff=value.get('alpha_cutoff',.5))
    return {**BASE,**alpha}if alpha!=dict(alpha_mode='opaque',opacity=1.,alpha_cutoff=.5)else copy.deepcopy(BASE)


def windows_job():
    if os.name!='nt':return
    from ctypes import wintypes as w
    k=ctypes.WinDLL('kernel32',use_last_error=True);k.GetCurrentProcess.restype=w.HANDLE
    k.IsProcessInJob.argtypes=[w.HANDLE,w.HANDLE,ctypes.POINTER(w.BOOL)];k.IsProcessInJob.restype=w.BOOL
    inside=w.BOOL();need(k.IsProcessInJob(k.GetCurrentProcess(),None,ctypes.byref(inside))and inside.value,'External Windows Job required before native startup')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--timeout',type=int,default=180)
    a=parser.parse_args();need(not sys.flags.optimize and 1<=a.timeout<=180,'Assertions and original180s ceiling required');windows_job()
    a.binary=a.binary.resolve(strict=True);a.output=a.output.absolute();need(not a.output.exists()and a.output.parent.is_dir(),'Fresh owned output with existing parent required')
    a.output.mkdir();sources=a.output/'sources';sources.mkdir();saves=a.output/'saves';saves.mkdir();world=a.output/'world.json';store=Path(str(world)+'.assets')
    started=time.monotonic();deadline=started+a.timeout;work_deadline=deadline-10
    record=dict(format='poima.material-alpha-contract.v1',passed=False,errors=[],calls=[],owners=[],checkpoints=[],whole_pairs=[],adoptions=[],limits=dict(seconds=a.timeout,rpc=260,bytes=64*1048576,files=512,owners=2),scope='Native CPU protocol, original synthetic textured quad, actual moving body and custom component. No compiled gameplay/GPU/alpha-aware collision/performance/AOT claim.')
    files=[Path(__file__),ROOT/'tests/gltf_fixture.py',ROOT/'tests/texture_fixture.py',ROOT/'tests/switchback_oracle.py',a.binary]
    files += [p for p in(ROOT/'tools/python/poima_client').iterdir()if p.is_file()and p.suffix in('.py','.json')]
    if(a.binary.parent/'phonon.dll').is_file():files.append(a.binary.parent/'phonon.dll')
    pins={str(p):pin(p)for p in files};record['source_pins']=pins
    client=None;session=uid();owner_actions=[];revision=0
    def bound():
        paths=[p for p in a.output.rglob('*')if p.is_file()]
        need(len(paths)<=512 and sum(p.stat().st_size for p in paths)+len(encoded(record))+1<=64*1048576,'Observed64MiB/512file retention limit')
    def rpc(method,params=None,error=None):
        need(time.monotonic()<work_deadline and len(record['calls'])<260,'Original work deadline/RPC limit')
        row=dict(method=method,params=copy.deepcopy(params or {}),owner=len(record['owners']));record['calls'].append(row)
        try:value=client.call(method,params or {},timeout=min(20,max(.001,work_deadline-time.monotonic())))
        except RpcError as e:
            row['error']=dict(code=e.code,message=e.message,data=e.data);need(type(error)is int and e.code==error,'Unexpected native error: '+str(row['error']));bound();return row['error']
        except BaseException as e:row['transport_error']=repr(e);raise
        row['result']=value;bound();need(error is None,'Native accepted expected rejection');return value
    def open_owner(label):
        nonlocal client,owner_actions
        owner_actions=[];client=WorldClient.open(str(a.binary),str(world),trace_directory=str(a.output/label),trace_limit_bytes=16*1048576,close_timeout=5)
        row=dict(label=label,pid=client.transport.process_id,closed=False,cleanup_actions=owner_actions);record['owners'].append(row)
        for action in('terminate','kill'):
            original=getattr(client.transport._process,action)
            def observed(*args,_action=action,_original=original,**kw):owner_actions.append(_action);return _original(*args,**kw)
            setattr(client.transport._process,action,observed)
    def close_owner():
        nonlocal client
        if client is None:return
        current=client
        try:current.close()
        finally:
            row=record['owners'][-1];row.update(closed=current.closed,exit_code=current.transport.returncode,cleanup_error=None if current.cleanup_error is None else str(current.cleanup_error),stderr=current.transport.stderr_tail,stderr_truncated=current.transport.stderr_truncated)
            need(current.closed and current.transport.returncode==0 and current.cleanup_error is None and not owner_actions and not current.transport.stderr_truncated,'Native owner did not close naturally/losslessly')
            client=None
    def tx(ops,preview=False,error=None,request=None):
        nonlocal revision
        params=dict(request_id=request or uid(),base_revision=revision,ops=ops,preview=preview);r=rpc('world.transact',params,error)
        if error is None and not preview:revision=r['revision']
        return r,params
    def status():return rpc('runtime.inspect',dict(session_id=session))
    def material():
        s=status();return rpc('runtime.material.inspect',dict(session_id=session,tick=s['tick'],material_revision=s['material_revision'],ids=[BODY]))
    def apply_params():
        s=status();return dict(request_id=uid(),session_id=session,expected_tick=s['tick'],expected_structure_revision=s['structure_revision'],expected_material_revision=s['material_revision'],revision=revision,expected_authored_revision=s['authored_revision'],expected_content_sha256=s['content_sha256'],ids=[BODY])
    def checkpoint(label):
        s=status();comp=rpc('runtime.components',dict(session_id=session));receipt=rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,session_id=session,expected_tick=s['tick'],expected_gameplay_revision=0,expected_component_revision=comp['component_revision'],expected_structure_revision=s['structure_revision'],expected_material_revision=s['material_revision'],expected_ui_revision=s['ui_revision'],expected_control_sequence=s['control_sequence']))
        manifest=strict_json((saves/('slot-'+label)/'current.json').read_bytes());entry=manifest['payload']['current'];p=saves/('slot-'+label)/entry['file'];raw=p.read_bytes();identity=pin(p)
        need(identity=={k:receipt[k]for k in('bytes','sha256')}and identity=={k:entry[k]for k in('bytes','sha256')}and entry['generation']==receipt['generation']==1,'Native receipt/manifest/full archive join differs')
        full=strict_json(raw);need(set(full)=={'format','version','document','snapshot'}and full['format']=='poima.world-save'and full['version']==1,'Original complete checkpoint shape differs')
        record['checkpoints'].append(dict(label=label,path=str(p.relative_to(a.output)),identity=identity,full=full));return raw,full
    def equal_pair(label,before,after):
        need(before==after,'Full original checkpoint changed: '+label);record['whole_pairs'].append(dict(label=label,bytes_equal=True,exclusions=[]))
    def frozen():return dict(document=pin(world),history=rpc('world.history'),inspection=rpc('world.inspect'),store=inventory(store))
    try:
        open_owner('author-stdio');revision=rpc('world.inspect')['revision']
        # Own original RGBA content. Embedded PNG bytes survive in the cooked
        # model; alpha comparisons below inspect original unmodified bytes.
        for label,mode,opacity,cutoff in [('opaque','OPAQUE',1.,None),('mask','MASK',.25,.75),('mask-default','MASK',.5,None),('blend','BLEND',.5,.5)]:
            m=dict(alphaMode=mode,pbrMetallicRoughness=dict(baseColorFactor=[.25,.5,.75,opacity],baseColorTexture=dict(index=0),metallicFactor=.25,roughnessFactor=.75))
            if cutoff is not None:m['alphaCutoff']=cutoff
            doc,blob=quad([png(2,2,RGBA)],m);(sources/(label+'.glb')).write_bytes(glb(doc,blob))
        record['original_sources']=inventory(sources);assets={};record['imports']={}
        for label in('opaque','mask','mask-default'):
            assets[label]=rpc('asset.import',dict(source=str(sources/(label+'.glb'))))['asset']
            page=rpc('asset.inspect',dict(asset=assets[label],section='primitives'));v=page['items'][0]['material']
            expected={**BASE,'emissive':[0.,0.,0.]}
            if label!='opaque':expected.update(alpha_mode='mask',opacity=.25 if label=='mask'else .5,alpha_cutoff=.75 if label=='mask'else .5)
            need(same(v,expected),'Imported alpha material differs from exact factor/defaults')
            package=store/(assets[label]+'.pmodel');raw=package.read_bytes();need(hashlib.sha256(raw).hexdigest()==assets[label],'Cooked content identity differs')
            h,n=struct.unpack_from('<II',raw,8);metadata=strict_json(raw[16:16+h]);need(16+h+n==len(raw),'Cooked package extent differs')
            need(raw[:8]==(b'POIMAM03'if label=='opaque'else b'POIMAM05')and metadata['version']==(3 if label=='opaque'else 5),'Opaque legacy/MASK5 encoding differs')
            image=metadata['images'][0];need(image['srgb']is True and image['mips'][0]==dict(width=2,height=2,bytes=16),'Original base-color image metadata differs')
            start=16+h+metadata['geometry_bytes'];need(raw[start:start+16]==RGBA,'Cooked base PNG RGBA/alpha bytes differ')
            record['imports'][label]=dict(inspection=page,package=pin(package),header=metadata,first_mip_rgba=list(raw[start:start+16]))
        ops=[dict(op='component.schema.set',schema=dict(id=TYPE,name='Alpha retained counter',version=1,fields=[dict(id=FIELD,name='Counter',kind='int32',default=0)]))]
        for i in(1,2):
            id=f'{i:032x}';ops += [dict(op='entity.create',id=id,name='Alpha native body'if i==1 else'Legacy explicit defaults'),component(id,'Transform',transform(10 if i==1 else 0)),component(id,'MeshRenderer',dict(primitive='box',visible=True,albedo=[.25,.5,.75])),component(id,'PbrMaterial',copy.deepcopy(BASE))]
        ops += [component(BODY,'BoxCollider',dict(half_extents=[.5,.5,.5],motion='dynamic',mass=1.,friction=.5,restitution=0.)),component(BODY,'game:'+TYPE,{FIELD:23}),dict(op='asset.instantiate',id=f'{100:032x}',name='Original alpha card',asset=assets['mask'])]
        tx(ops);legacy=rpc('entity.material',dict(id=BODY))['material'];need(same(legacy,BASE),'Default opaque effective five-key shape changed')
        # Imports publish cooked assets; only a committed transaction persists
        # the world file required by the complete BLEND rejection file oracle.
        before=frozen();rpc('asset.import',dict(source=str(sources/'blend.glb')),-32050);need(same(before,frozen()),'Rejected BLEND import changed authoring/store')
        explicit={**BASE,'alpha_mode':'opaque','opacity':1.,'alpha_cutoff':.5};accepted,parameters=tx([component(f'{2:032x}','PbrMaterial',explicit)])
        before=frozen();need(same(rpc('world.transact',parameters),{**accepted,'replayed':True})and same(before,frozen()),'Exact authored retry changed complete authoring/store')
        need(same(rpc('entity.material',dict(id=f'{2:032x}'))['material'],BASE),'Explicit default alpha changed effective legacy shape')
        # Valid preview boundaries and strict invalid operands preserve all
        # authoring/history/store bytes, not merely the selected material.
        for extra in [dict(alpha_mode='mask',opacity=0.,alpha_cutoff=0.),dict(alpha_mode='opaque',opacity=1.,alpha_cutoff=1e6)]:
            before=frozen();r,_=tx([component(BODY,'PbrMaterial',{**BASE,**extra})],preview=True);need(r['committed']is False and same(before,frozen()),'Valid preview persisted alpha edit')
        invalid=[dict(alpha_mode='blend'),dict(alpha_mode='MASK'),dict(alpha_mode=None),dict(opacity=-.01),dict(opacity=1.01),dict(opacity=True),dict(opacity='0.5'),dict(alpha_cutoff=-.01),dict(alpha_cutoff=1000001.),dict(alpha_cutoff=False),dict(alpha_cutoff='NaN'),dict(extra=1)]
        before=frozen()
        for extra in invalid:tx([component(BODY,'PbrMaterial',{**BASE,**extra})],error=-32602)
        for key in BASE:
            missing={name:value for name,value in BASE.items()if name!=key};tx([component(BODY,'PbrMaterial',missing)],error=-32602)
        need(same(before,frozen()),'Strict rejected alpha fields mutated complete authored state')
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)));rpc('runtime.start',dict(session_id=session,revision=revision));rpc('runtime.step',dict(session_id=session,request_id=uid(),expected_tick=0,ticks=7))
        comp=rpc('runtime.components',dict(session_id=session));rpc('runtime.component.edit',dict(session_id=session,request_id=uid(),expected_tick=7,expected_revision=comp['component_revision'],id=BODY,type=TYPE,values={FIELD:37}))
        need(rpc('runtime.entity',dict(session_id=session,id=BODY))['world_matrix'][13]<10,'Actual dynamic body did not move')
        before,_=checkpoint('before-queries');material();rpc('entity.material',dict(id=BODY));after,_=checkpoint('after-queries');equal_pair('pure-material-reads',before,after)
        # Three replacements exercise each alpha domain, active OPAQUE opacity,
        # and return to legacy semantics without clearing unrelated native state.
        for index,extra in enumerate([dict(alpha_mode='mask',opacity=.5,alpha_cutoff=.75),dict(alpha_mode='opaque',opacity=.25,alpha_cutoff=1.),dict(alpha_mode='opaque',opacity=1.,alpha_cutoff=.5)]):
            before_raw,old=checkpoint('adopt-'+str(index)+'-before');old_material=material();value={**BASE,**extra};tx([component(BODY,'PbrMaterial',value)])
            need(same(material()['materials'],old_material['materials'])and status()['source_stale']is True,'Authored alpha edit changed frozen runtime before adoption')
            params=apply_params();receipt=rpc('runtime.material.apply',params);need(receipt['replayed']is False and receipt['material_revision']==index+1 and receipt['tick']==7,'Alpha adoption receipt/clock differs')
            live=material();need(same(live['materials'],[dict(id=BODY,material=effective(value))])and live['source_stale']is False,'Adopted effective alpha profile differs')
            after_raw,new=checkpoint('adopt-'+str(index)+'-after');delta=differences(old,new)
            allowed={('document','revision'),('snapshot','sha256'),('snapshot','payload','authored_revision'),('snapshot','payload','content_sha256')}
            allowed |= {('document','entities',BODY,'components','PbrMaterial',key)for key in('alpha_mode','opacity','alpha_cutoff')}
            need(delta and all(tuple(x['path'])in allowed for x in delta),'Material adoption changed a non-whitelisted original native/save field')
            need(same(new['document']['entities'][BODY]['components']['PbrMaterial'],value)and new['snapshot']['payload']['content_sha256']!=old['snapshot']['payload']['content_sha256'],'Adopted document/content identity differs')
            record['adoptions'].append(dict(params=params,receipt=receipt,complete_field_differences=delta,allowed_exact_paths=[list(p)for p in sorted(allowed)]))
            replay=rpc('runtime.material.apply',params);need(same(replay,{**receipt,'replayed':True}),'Exact adoption retry did not return original receipt')
            rpc('runtime.material.apply',{**params,'expected_tick':8},-32010)
            for key in('expected_tick','expected_structure_revision','expected_material_revision','revision','expected_authored_revision'):
                stale=apply_params();stale[key]+=1;rpc('runtime.material.apply',stale,-32009)
            stale=apply_params();stale['expected_content_sha256']='0'*64;rpc('runtime.material.apply',stale,-32009)
            retried,_=checkpoint('adopt-'+str(index)+'-retry');equal_pair('exact-retry-and-rejections-'+str(index),after_raw,retried)
        # Even a valid selected alpha edit cannot smuggle an unrelated transform.
        tx([component(BODY,'PbrMaterial',{**BASE,'alpha_mode':'mask','opacity':.5,'alpha_cutoff':.75}),component(f'{2:032x}','Transform',transform(2))]);pre,_=checkpoint('before-unrelated');rpc('runtime.material.apply',apply_params(),-32602);post,_=checkpoint('after-unrelated');equal_pair('unrelated-edit-rejection',pre,post)
        tx([component(BODY,'PbrMaterial',explicit),component(f'{2:032x}','Transform',transform())]);final_raw,final=checkpoint('cold-source')
        runtime_before=material();authored_before=rpc('entity.material',dict(id=BODY));packages=inventory(store);rpc('session.close');close_owner()
        sources.rename(a.output/'removed-sources');need(inventory(a.output/'removed-sources')==record['original_sources']and not sources.exists(),'Original owned source-removal namespace differs')
        open_owner('cold-stdio');revision=rpc('world.inspect')['revision'];need(same(rpc('entity.material',dict(id=BODY)),authored_before),'Source-removed authored effective material differs')
        for label in assets:need(same(rpc('asset.inspect',dict(asset=assets[label],section='primitives')),record['imports'][label]['inspection']),'Source-removed complete cooked material inspection differs')
        need(inventory(store)==packages,'Source-removed cooked read changed packages')
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)))
        session=uid();rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot='cold-source',expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_component_revision=None,expected_structure_revision=None,expected_material_revision=None,expected_ui_revision=None,expected_control_sequence=None,new_session_id=session))
        cold,_=checkpoint('cold-restored');equal_pair('source-removed-cold-complete',final_raw,cold)
        need(same(material()['materials'],runtime_before['materials']),'Cold effective opaque material differs')
        need(rpc('runtime.component.get',dict(session_id=session,tick=7,id=BODY,type=TYPE))['values'][FIELD]==37,'Full native component value was lost')
        rpc('session.close');close_owner();need(inventory(store)==packages,'Cooked closure changed on cold restore');record['passed']=True
    except BaseException:record['passed']=False;record['errors'].append(traceback.format_exc())
    finally:
        try:close_owner()
        except BaseException:record['passed']=False;record['errors'].append('Owner closure failed:\n'+traceback.format_exc())
        try:
            record['seconds']=time.monotonic()-started;record['changed_inputs']=[p for p,v in pins.items()if not Path(p).is_file()or pin(p)!=v];need(not record['changed_inputs']and record['seconds']<=a.timeout,'Input/time terminal fence');bound()
        except BaseException:record['passed']=False;record['errors'].append('Terminal fence failed:\n'+traceback.format_exc())
        (a.output/'evidence.json').write_bytes(encoded(record)+b'\n')
    print(json.dumps(dict(passed=record['passed'],rpc=len(record['calls']),owners=len(record['owners']),checkpoints=len(record['checkpoints']),whole_pairs=len(record['whole_pairs']),adoptions=len(record['adoptions']))));return 0 if record['passed']else 1


if __name__=='__main__':raise SystemExit(main())
