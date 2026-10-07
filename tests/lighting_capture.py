#!/usr/bin/env python3
"""Compare actual Vulkan lighting readback with scalar radiometry/BRDF references."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import uuid
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'lights.world.json';rev=0
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'gpu_index':args.gpu,'lighting_path':args.lighting_path,'capture_samples':1 if args.lighting_path=='deferred' else 4,'runs':[],'samples':[]}
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def batch(requests):
    rows=[{'jsonrpc':'2.0','id':i+1,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    process=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=120)
    replies=[json.loads(s) for s in process.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':process.returncode,'stderr':process.stderr})
    assert process.returncode==0 and len(replies)==len(rows),process.stdout+process.stderr
    for reply in replies:
        assert 'result' in reply,reply
        value=reply['result']
        if 'capture_written' in value:
            assert value['capture_written'] and value['hardware'] and value['nvrhi_errors']==0,value
            assert value['render_diagnostics']['lighting_path']==args.lighting_path,value
            assert value['samples']==record['capture_samples'],value
    return [r['result'] for r in replies]
def transaction(ops):return ('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':rev,'ops':ops})
def create(n,parent=None):return {'op':'entity.create','id':uid(n),'name':f'Lighting {n}','parent':parent}
def component(n,kind,value):return {'op':'component.set','id':uid(n) if isinstance(n,int) else n,'type':kind,'value':value}
def transform(n,p=(0,0,0),q=(0,0,0,1),s=(1,1,1)):return component(n,'Transform',{'position':list(p),'rotation':list(q),'scale':list(s)})
def light(n=2,kind='directional',**kw):return component(n,'Light',{'kind':kind,'color':[1,1,1],'intensity':math.pi,'enabled':True,**kw})
def env(ambient=(0,0,0),exposure=1):return component(3,'LightingEnvironment',{'ambient':list(ambient),'exposure':exposure})
def edit(ops):
    global rev
    batch([transaction(ops)]);rev+=1
def capture(name,session=None,tick=0):
    p={'camera':uid(1),'path':native(run/(name+'.bmp')),'gpu':args.gpu,'samples':1 if args.lighting_path=='deferred' else 4,'lighting_path':args.lighting_path}
    return ('runtime.capture',{**p,'session_id':session,'tick':tick}) if session else ('world.capture',{**p,'revision':rev})
def check(name,expected,tolerance=3):
    actual=pixels(run/(name+'.bmp'))[270][480];record['samples'].append({'name':name,'actual':actual,'expected':expected,'tolerance':tolerance});assert max(abs(a-b) for a,b in zip(actual,expected))<=tolerance,(name,actual,expected)
def normalize(v):
    length=math.sqrt(sum(x*x for x in v));return [x/length for x in v]
def display(x):
    x=x/(1+x);return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
BASE=[.6,.2,.05]
def radiance(irradiance,l=(0,0,1),base=BASE,roughness=.6):
    l=normalize(l);nl=max(l[2],0);h=normalize([l[0],l[1],l[2]+1]);nh=h[2];a2=roughness**4
    D=a2/(math.pi*(nh*nh*(a2-1)+1)**2);vis=.5/max(nl+math.sqrt(nl*nl*(1-a2)+a2),1e-6);F=.04+.96*(1-nh)**5
    return [((1-F)*b/math.pi+D*vis*F)*nl*e for b,e in zip(base,irradiance)]
def expected(irradiance,l=(0,0,1),ambient=(0,0,0),emission=(0,0,0),exposure=1,base=BASE,roughness=.6):
    return [display((v+b*a+e)*exposure) for v,b,a,e in zip(radiance(irradiance,l,base,roughness),base,ambient,emission)]

try:
    doc,blob=quad([],{'pbrMetallicRoughness':{'baseColorFactor':BASE+[1],'metallicFactor':0,'roughnessFactor':.6}})
    path=run/'quad.glb';path.write_bytes(glb(doc,blob));asset=batch([('asset.import',{'source':native(path)})])[0]['asset']
    mesh=hashlib.sha256(('poima.instance.v1/'+uid(100)+'/node/0/primitive/0').encode()).hexdigest()[:32]
    edit([create(1),transform(1,(0,0,4)),component(1,'Camera',{'vertical_fov':60,'near':.1,'far':100}),create(2),light(),create(3),env(),{'op':'asset.instantiate','id':uid(100),'name':'Lighting target','asset':asset}])
    batch([capture('directional')]);check('directional',expected([math.pi]*3))
    edit([light(kind='point',intensity=4*math.pi),transform(2,(0,0,2))]);batch([capture('point-near')]);check('point-near',expected([math.pi]*3))
    edit([transform(2,(0,0,4))]);batch([capture('point-far')]);check('point-far',expected([math.pi/4]*3))
    edit([light(kind='point',intensity=4*math.pi,range=4),transform(2,(0,0,2))]);batch([capture('range-taper')]);check('range-taper',expected([math.pi*(1-(2/4)**4)]*3))
    edit([light(kind='point',range=2)]);batch([capture('range-boundary')]);check('range-boundary',[0,0,0])
    edit([light(kind='spot',intensity=4*math.pi,inner_angle=15,outer_angle=45)]);batch([capture('spot-axis')]);check('spot-axis',expected([math.pi]*3))
    edit([transform(2,(0,0,2),(0,math.sin(math.pi/12),0,math.cos(math.pi/12)))]);batch([capture('spot-cone')]);cone=(math.cos(math.pi/6)-math.cos(math.pi/4))/(math.cos(math.pi/12)-math.cos(math.pi/4));check('spot-cone',expected([math.pi*cone**2]*3))
    edit([transform(2,(0,0,2),(0,1,0,0))]);batch([capture('spot-away')]);check('spot-away',[0,0,0])
    edit([light(enabled=False)]);batch([capture('disabled')]);check('disabled',[0,0,0])
    edit([env((.1,.2,.3),2)]);batch([capture('ambient-exposure')]);check('ambient-exposure',expected([0]*3,ambient=[.1,.2,.3],exposure=2))
    edit([env(),transform(2),light(color=[1,0,0]),create(4),light(4,color=[0,1,0],intensity=2*math.pi)])
    batch([capture('colored-sum')]);check('colored-sum',expected([math.pi,2*math.pi,0]))
    # Retain the original 64-light regression, including slot 63.
    # clustered_lighting_capture.py additionally exercises slot 1023.
    edit([light(intensity=0),light(4,intensity=0)]+[op for n in range(10,72) for op in (create(n),light(n,intensity=math.pi if n==71 else 0))])
    batch([capture('last-slot')]);check('last-slot',expected([math.pi]*3))
    edit([{'op':'entity.delete','id':uid(n),'recursive':True} for n in [4,*range(10,72)]])
    # Native runtime owns the light settings and updates a light attached to a falling body.
    edit([create(5),transform(5,(0,2,0)),component(5,'BoxCollider',{'half_extents':[.1,.1,.1],'motion':'dynamic','mass':1,'friction':.5,'restitution':0}),{'op':'entity.reparent','id':uid(2),'parent':uid(5),'mode':'keep_local'},transform(2,(0,0,2)),light(kind='point',intensity=4*math.pi)])
    session=uid(900);rows=batch([('runtime.start',{'session_id':session,'revision':rev}),capture('runtime-initial',session),transaction([light(kind='point',intensity=0),env(exposure=0)]),capture('runtime-frozen',session),('runtime.step',{'session_id':session,'request_id':uid(901),'expected_tick':0,'ticks':30}),('runtime.lighting',{'session_id':session,'tick':30}),capture('runtime-moved',session,30)]);rev+=1
    assert pixels(run/'runtime-initial.bmp')==pixels(run/'runtime-frozen.bmp')
    check('runtime-initial',expected([math.pi/2]*3,l=[0,2,2]))
    pos=rows[5]['lights'][0]['position'];check('runtime-moved',expected([4*math.pi/sum(x*x for x in pos)]*3,l=pos))
    assert pixels(run/'runtime-moved.bmp')!=pixels(run/'runtime-initial.bmp')
    batch([capture('authored-dark')]);check('authored-dark',[0,0,0])
    # Authored lights switch legacy primitives to the same material lighting path.
    edit([env(),light(intensity=math.pi),{'op':'entity.reparent','id':uid(2),'parent':None,'mode':'keep_local'},transform(2),{'op':'component.remove','id':mesh,'type':'StaticMesh'},{'op':'component.remove','id':mesh,'type':'PbrMaterial'},component(mesh,'MeshRenderer',{'primitive':'box','visible':True,'albedo':BASE})]);batch([capture('builtin-box')]);check('builtin-box',expected([math.pi]*3,roughness=1))
    # Emission also obeys explicit exposure; no light or ambient energy is introduced.
    edit([light(enabled=False),env(exposure=.5),component(mesh,'PbrMaterial',{'base_color':BASE,'emissive':[.2,.4,.8],'metallic':0,'roughness':.6,'double_sided':False})]);batch([capture('emission')]);check('emission',[display(x*.5) for x in [.2,.4,.8]])
    record['passed']=True
    record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')}
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'samples':record['samples']}))
