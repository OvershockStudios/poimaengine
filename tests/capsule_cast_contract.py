#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual native capsule queries and independently negotiated CoreCLR consumer.

Run inside an owned process tree. This fixture creates a fresh world, uses only
compiled gameplay, retains original SDK wire traces and complete native saves,
and never renders or compiles. Every output must be new.
"""
import argparse
import copy
import hashlib
import json
import math
import re
import sys
import time
import traceback
import uuid
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'tests'))
from poima_client import WorldClient, RpcError
from switchback_oracle import read_checkpoint


def need(value,message):
    if not value:raise AssertionError(message)


def uid():return uuid.uuid4().hex

def entity(n):return f'{n:032x}'

def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()

def same(a,b):return encoded(a)==encoded(b)

def pin(path):
    p=Path(path);need(p.is_file()and not p.is_symlink(),'Ordinary selected file required')
    with p.open('rb')as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(bytes=p.stat().st_size,sha256=digest)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','assembly','bridge','hostfxr','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    for name in ('binary','assembly','bridge','hostfxr'):setattr(a,name,getattr(a,name).resolve(strict=True))
    a.output=a.output.absolute();need(not a.output.exists()and a.output.parent.is_dir(),'Fresh output/existing parent required')
    a.output.mkdir();saves=a.output/'saves';saves.mkdir();started=time.monotonic();deadline=started+180
    r=dict(format='poima.capsule-cast-contract.v1',passed=False,calls=[],checkpoints=[],whole_pairs=[],errors=[],scope='Actual native capsule queries and capsule-only compiled CoreCLR permission/quota/rollback. Full v9 save bytes and all22 typed fields. No GPU, physical input, performance, Native AOT or broad weapon-geometry claim.')
    files=[Path(__file__),ROOT/'tests/switchback_oracle.py',a.binary,a.assembly,a.bridge,a.hostfxr]
    files += list((ROOT/'tests/managed_capsule_cast_gameplay').glob('*.cs'))
    for image in (a.bridge,a.assembly):
        files.append(image.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json','.runtimeconfig.json'):
            path=image.with_suffix(suffix)
            if path.is_file():files.append(path)
    r['source_pins']={str(f):pin(f)for f in files};client=None;cleanup=[];session=uid();fields=None
    def bound():
        paths=[x for x in a.output.rglob('*')if x.is_file()]
        need(len(paths)<=512 and sum(x.stat().st_size for x in paths)+len(encoded(r))<=64*1048576,'Observed64MiB/512file cap')
        need(time.monotonic()<deadline,'Original180s deadline')
    def rpc(method,params=None,error=None):
        need(len(r['calls'])<220,'Original220RPC cap');bound()
        row=dict(method=method,params=copy.deepcopy(params or {}));r['calls'].append(row)
        try:value=client.call(method,params or {},timeout=min(30,max(.001,deadline-time.monotonic())))
        except RpcError as e:
            row['error']=dict(code=e.code,message=e.message,data=e.data)
            need(error is True or type(error)is int and e.code==error,'Unexpected native rejection: '+str(row['error']));return row['error']
        row['result']=value;bound();need(error is None,'Expected native rejection');return value
    def inspect():return rpc('runtime.inspect',dict(session_id=session))
    def game():return rpc('runtime.gameplay.inspect',dict(session_id=session))
    def checkpoint(label):
        state=inspect();g=game();values=g['module']['values']
        need(set(values)==set(fields),'Complete original component field set differs')
        for name,kind in fields.items():
            v=values[name]
            if kind=='int32':need(type(v)is int,'Original int32 kind differs')
            elif kind=='float64':need(type(v)is float and math.isfinite(v),'Original float64 kind differs')
            elif kind=='int64':need(type(v)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',v),'Original int64 kind differs')
            else:need(kind=='entity'and type(v)is str and re.fullmatch(r'[0-9a-f]{32}',v),'Original entity kind differs')
        receipt=rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,session_id=session,expected_tick=state['tick'],expected_gameplay_revision=g['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        full=read_checkpoint(saves,label,receipt);current=json.loads((saves/('slot-'+label)/'current.json').read_bytes())['payload']['current']
        path=saves/('slot-'+label)/current['file'];raw=path.read_bytes();r['checkpoints'].append(dict(label=label,path=str(path.relative_to(a.output)),identity=pin(path),complete=full));return raw
    def edit(values):
        s=inspect();g=game();return rpc('runtime.gameplay.edit',dict(session_id=session,request_id=uid(),expected_tick=s['tick'],expected_revision=g['revision'],expected_structure_revision=s['structure_revision'],values=values))
    def step(ticks=1,error=None):
        s=inspect();return rpc('runtime.step',dict(session_id=session,request_id=uid(),expected_tick=s['tick'],ticks=ticks),error)
    try:
        client=WorldClient.open(str(a.binary),str(a.output/'world.json'),trace_directory=str(a.output/'stdio'),trace_limit_bytes=16*1048576,close_timeout=5)
        process=client.transport._process
        for action in ('terminate','kill'):
            original=getattr(process,action)
            def observed(*args,_action=action,_original=original,**kw):cleanup.append(_action);return _original(*args,**kw)
            setattr(process,action,observed)
        ops=[]
        def create(n,name,position):
            ops.extend([dict(op='entity.create',id=entity(n),name=name),dict(op='component.set',id=entity(n),type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1]))])
        def box(n,extents):ops.append(dict(op='component.set',id=entity(n),type='BoxCollider',value=dict(half_extents=extents,motion='static',mass=1,friction=.5,restitution=0)))
        create(1,'Floor',[0,-1.1,0]);box(1,[10,.1,10])
        create(2,'Saved virtual actor',[5,-1,0]);ops.append(dict(op='component.set',id=entity(2),type='CharacterController',value=dict(radius=.35,height=1.8,speed=4,jump_speed=6,camera=None,locomotion={})))
        create(10,'Analytic query wall',[0,0,-3]);box(10,[1,1,.1])
        rev=rpc('world.transact',dict(request_id=uid(),base_revision=0,ops=ops))['revision']
        schema=rpc('world.describe',dict(view='method',name='runtime.capsule_cast'));r['query_schema']=schema
        rpc('runtime.start',dict(session_id=session,revision=rev))
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)))
        loaded=rpc('runtime.gameplay.load',dict(session_id=session,request_id=uid(),expected_tick=0,expected_revision=0,assembly=str(a.assembly),bridge=str(a.bridge),hostfxr=str(a.hostfxr),type='Poima.Tests.ManagedCapsuleCastGame'))
        module=loaded['module'];need(module['backend']=='coreclr'and module['schema']['identity']=='poima.test.managed-capsule-cast','Actual compiled capsule fixture required')
        fields={f['name']:f['kind']for f in module['schema']['fields']};need(len(fields)==len(module['schema']['fields'])==22,'All22 distinct native fields required')
        need(module['assembly_sha256']==pin(a.assembly)['sha256'],'Native loader did not select the supplied compiled consumer')
        base=dict(session_id=session,tick=0,position=[0,0,0],rotation=[0,0,0,1],radius=.25,half_height=.5,direction=[0,0,-1],distance=5)
        before=checkpoint('queries-before')
        hit=rpc('runtime.capsule_cast',base)['hit'];need(hit['entity']==entity(10)and abs(hit['distance']-2.65)<2e-4 and abs(hit['fraction']-.53)<4e-5 and hit['initial_overlap']is False and hit['triangle']is None,'Analytic capsule contact differs')
        need(abs(hit['point_on_obstacle'][2]+2.9)<2e-4 and hit['normal'][2]>.999,'Contact point/obstacle normal differs')
        overlap=rpc('runtime.capsule_cast',{**base,'position':[0,0,-2.9],'distance':0})['hit'];need(overlap['initial_overlap']is True and overlap['distance']==0 and overlap['penetration_depth']>0,'Initial overlap was not reported')
        need(rpc('runtime.capsule_cast',{**base,'distance':0})['hit']is None,'Clear overlap query returned a hit')
        need(rpc('runtime.capsule_cast',{**base,'ignore':[entity(10)]})['hit']is None,'Explicit ignored wall still hit')
        sphere=rpc('runtime.capsule_cast',{**base,'half_height':0})['hit'];need(abs(sphere['distance']-2.65)<2e-4,'Zero-height sphere differs')
        rotated=rpc('runtime.capsule_cast',{**base,'rotation':[math.sqrt(.5),0,0,math.sqrt(.5)]})['hit'];need(abs(rotated['distance']-2.15)<2e-4,'Capsule rotation did not change axial extent')
        for patch in ({'direction':[0,0,0]},{'rotation':[0,0,0,0]},{'radius':True},{'radius':0},{'half_height':-1},{'distance':-1},{'distance':True},{'ignore':[entity(10),entity(10)]},{'ignore':[entity(999)]},{'extra':True}):rpc('runtime.capsule_cast',{**base,**patch},-32602)
        rpc('runtime.capsule_cast',{**base,'tick':1},-32009)
        after=checkpoint('queries-after');need(before==after,'Read queries/guards changed complete original native archive');r['whole_pairs'].append(dict(before='queries-before',after='queries-after',bytes_equal=True))
        step();v=game()['module']['values'];need(v['Ticks']==1 and v['Queries']==1 and v['Hit']==1 and v['Entity']==entity(10)and abs(v['Distance']-2.65)<2e-4,'Actual independently negotiated compiled capsule callback differs')
        edit(dict(Mode=32));step();v=game()['module']['values'];need(v['Ticks']==2 and v['Queries']==33,'Exactly32 queries did not complete')
        for mode,label in ((33,'quota'),(2,'invalid')):
            edit(dict(Mode=mode));before=checkpoint(label+'-before');rejected=step(error=True);need('capsule'in rejected['message'].lower(),'Missing capsule failure diagnostic');after=checkpoint(label+'-after');need(before==after,'Failed callback changed complete native archive');r['whole_pairs'].append(dict(before=label+'-before',after=label+'-after',bytes_equal=True))
        # Tick callbacks receive the entry tick: the second call in a two-tick batch is tick+1.
        tick=inspect()['tick'];edit(dict(Mode=1,FailAt=str(tick+1)));before=checkpoint('batch-before');rejected=step(2,error=True);need('Capsule later-Tick rollback fixture'in rejected['message'],'Did not reach intended later-Tick failure');after=checkpoint('batch-after');need(before==after,'Later-Tick failure did not roll back complete batch');r['whole_pairs'].append(dict(before='batch-before',after='batch-after',bytes_equal=True))
        rpc('runtime.stop',dict(session_id=session));r['passed']=True
    except BaseException:r['errors'].append(traceback.format_exc());r['passed']=False
    finally:
        if client is not None:
            try:client.close()
            except BaseException:r['errors'].append('Native close failed:\n'+traceback.format_exc());r['passed']=False
            r['native_owner']=dict(pid=client.transport.process_id,closed=client.closed,exit_code=client.transport.returncode,cleanup_actions=cleanup,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),stderr=client.transport.stderr_tail,stderr_truncated=client.transport.stderr_truncated)
            if not(client.closed and client.transport.returncode==0 and not cleanup and client.cleanup_error is None and not client.transport.stderr_truncated):r['passed']=False;r['errors'].append('Native owner did not close naturally')
        r['seconds']=time.monotonic()-started
        try:
            need(all(pin(Path(f))==value for f,value in r['source_pins'].items()),'Original selected source/images changed');bound()
        except BaseException:r['errors'].append('Final identity/bound failure:\n'+traceback.format_exc());r['passed']=False
        (a.output/'evidence.json').write_bytes(encoded(r)+b'\n')
    print(json.dumps(dict(passed=r['passed'],rpc=len(r['calls']),whole_pairs=len(r['whole_pairs']),seconds=r['seconds'],evidence=str(a.output/'evidence.json'))))
    return 0 if r['passed']else 1


if __name__=='__main__':raise SystemExit(main())
