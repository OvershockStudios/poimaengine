#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author the independent greybox range with one native guarded transaction.

Run only through an admitted native route. No downloads, compilation, art,
gameplay injection, renderer invocation or automatic retry occurs here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient


def entity(value):return f'{value:032x}'


def operations():
    ops=[]
    def create(n,name,position,scale=(1,1,1),parent=None,rotation=(0,0,0,1)):
        ops.extend([dict(op='entity.create',id=entity(n),name=name,parent=None if parent is None else entity(parent)),
            dict(op='component.set',id=entity(n),type='Transform',value=dict(position=list(position),rotation=list(rotation),scale=list(scale)))])
    def component(n,kind,value):ops.append(dict(op='component.set',id=entity(n),type=kind,value=value))
    def box(n,name,position,extents,color,motion='static'):
        # Unit collider half extents plus visual scale keep both geometries exact.
        create(n,name,position,tuple(2*x for x in extents))
        component(n,'MeshRenderer',dict(primitive='box',visible=True,albedo=color))
        component(n,'BoxCollider',dict(half_extents=[.5,.5,.5],motion=motion,mass=1,friction=.5,restitution=0))
    box(1,'Range floor',(0,-2,0),(30,.1,30),[.18,.2,.22])
    create(2,'Range observer',(4,-1.88,8))
    component(2,'CharacterController',dict(radius=.3,height=1.8,speed=4,jump_speed=5,camera=entity(3),locomotion={}))
    create(3,'Range camera',(0,5,0),parent=2,rotation=(-.17364817766693033,0,0,.984807753012208))
    component(3,'Camera',dict(vertical_fov=70,near=.1,far=100))
    box(10,'Ten millimetre collision wall',(0,1,-10),(.5,2,.005),[.45,.5,.55])
    box(11,'Sphere graze target',(8,1,-3),(.1,.3,.1),[.55,.4,.25])
    box(20,'Committed projectile marker',(20,5,0),(.01,.01,.01),[1,.65,.1],motion='kinematic')
    create(30,'Range sun',(0,0,0),rotation=(-.3826834323650898,0,0,.9238795325112867))
    component(30,'Light',dict(kind='directional',color=[1,.94,.86],intensity=3,enabled=True,shadow=dict(enabled=True,distance=60)))
    create(31,'Range ambient',(0,0,0));component(31,'LightingEnvironment',dict(ambient=[.07,.08,.1],exposure=1,shadow_resolution=512))
    def ui(n,kind,text,top,action=None):
        element=dict(parent=None if n==500 else entity(500),name=text,kind=kind,text='' if kind=='panel' else text,action=action,visible=True,enabled=True)
        element['layout']=dict(position='absolute',left=dict(unit='dp',value=0 if n==500 else 24),top=dict(unit='dp',value=0 if n==500 else top),width=dict(unit='dp',value=448 if n==500 else 400),height=dict(unit='dp',value=500 if n==500 else 62 if n==502 else 36))
        element['style']=dict(background_color='#182024dd'if n==500 else '#00000000'if kind=='label'else '#722f37',color='#f7e8eb',font_size=16)
        ops.append(dict(op='ui.element.set',id=entity(n),element=element))
    ui(500,'panel','Projectile range',0);ui(501,'label','FINITE-SPEED PROJECTILES',16)
    ui(502,'label','Choose a launch mode',58)
    for n,text,action in [(510,'Fast / 1200 m/s','fast'),(511,'Gravity / curved flight','gravity'),(512,'Sphere graze / center ray misses','graze'),(513,'Initial overlap','overlap'),(514,'Lifetime expiration','expire'),(515,'Fault on second future Tick','fault'),(516,'Clear pending fault','clear-fault')]:
        ui(n,'button',text,140+(n-510)*42,action)
    return ops


def identity(path):
    with path.open('rb')as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(bytes=path.stat().st_size,sha256=digest)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','project','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.binary=a.binary.resolve(strict=True);a.project=a.project.resolve(strict=True);a.output=a.output.absolute()
    if a.output.exists()or not a.output.parent.is_dir():p.error('Fresh receipt output with an existing parent required.')
    spec=json.loads(a.project.read_bytes());world=a.project.parent/'world.json'
    if spec['entry']!=dict(world='world.json',camera=entity(3),controller=entity(2))or world.exists():p.error('Only a fresh prepared projectile project with no world is accepted.')
    if a.output.resolve().is_relative_to(a.project.parent.resolve())or a.project.parent.resolve().is_relative_to(a.output.resolve()):p.error('Project and receipt output must be disjoint.')
    profile=a.project.parent/spec['input_profile'];pins={str(q):identity(q)for q in (a.binary,a.project,profile)}
    a.output.mkdir();r=dict(passed=False,calls=[],errors=[],source_pins=pins,scope='Fresh canonical range authoring only; not gameplay or physics qualification.')
    client=None;deadline=time.monotonic()+30
    try:
        client=WorldClient.open(str(a.binary),str(world),trace_directory=str(a.output/'stdio'),close_timeout=5)
        def call(method,params=None):
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('Thirty-second authoring deadline.')
            row=dict(method=method,params=params or {});r['calls'].append(row)
            row['result']=client.call(method,row['params'],timeout=min(remaining,10));return row['result']
        before=call('world.inspect')
        if before['revision']!=0 or before['entity_count']!=0:raise ValueError('Expected new empty native world.')
        changed=call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=operations()))
        after=call('world.inspect')
        if changed['committed']is not True or after['revision']!=changed['revision']or not world.is_file():raise RuntimeError('Guarded range was not persisted.')
        if any(identity(Path(name))!=expected for name,expected in pins.items()):raise RuntimeError('Selected original inputs changed.')
        r['world']=identity(world);r['passed']=True
    except BaseException:r['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            try:client.close()
            except BaseException:r['passed']=False;r['errors'].append(traceback.format_exc())
            r['close']=dict(closed=client.closed,pid=client.transport.process_id,exit_code=client.transport.returncode,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error))
            if not client.closed or client.transport.returncode!=0 or client.cleanup_error is not None:r['passed']=False
        (a.output/'calls.json').write_text(json.dumps(r,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=r['passed'],record=str(a.output/'calls.json'))));return 0 if r['passed']else 1


if __name__=='__main__':raise SystemExit(main())
