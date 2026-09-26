#!/usr/bin/env python3
"""Native Vulkan glTF geometry, material factors, culling and persistence checks."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import uuid
from gltf_fixture import write_fixture,glb
from scene_capture import pixels

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True);run=args.output/uuid.uuid4().hex;run.mkdir()
def native(path):
    path=str(path.resolve());return subprocess.check_output(['wslpath','-w',path],text=True).strip() if args.windows_interop else path
def uid(n):return f'{n:032x}'
def derived(root,suffix):return hashlib.sha256(('poima.instance.v1/'+uid(root)+'/'+suffix).encode()).hexdigest()[:32]
def transform(position,rotation=(0,0,0,1),scale=(1,1,1)):return {'position':list(position),'rotation':list(rotation),'scale':list(scale)}
def material(metal=0,rough=.5,color=(.6,.2,.05),double=False):return {'base_color':list(color),'emissive':[0,0,0],'metallic':metal,'roughness':rough,'double_sided':double}
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'gpu_index':args.gpu,'runs':[]}
world=run/'model.world.json'
def batch(requests):
    requests=[{'jsonrpc':'2.0','id':i+1,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    p=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,encoding='utf-8',timeout=120)
    rows=[json.loads(s) for s in p.stdout.splitlines()];record['runs'].append({'requests':requests,'responses':rows,'exit_code':p.returncode,'stderr':p.stderr})
    assert p.returncode==0 and len(rows)==len(requests),p.stdout+p.stderr
    return rows

def result(row):assert 'result' in row,row;return row['result']
def set_component(entity,kind,value):return {'op':'component.set','id':entity,'type':kind,'value':value}
def edit(revision,ops):return ('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':revision,'ops':ops})
def capture(name,revision=1):return ('world.capture',{'revision':revision,'camera':uid(1),'path':native(run/(name+'.bmp')),'gpu':args.gpu,'samples':4})

def cpu_color(metal,rough,color):
    # Independent scalar reference at the center of a +Z-facing plane.
    l=[-.4,.8,.6];length=math.sqrt(sum(x*x for x in l));l=[x/length for x in l];v=[0,0,1]
    h=[l[i]+v[i] for i in range(3)];length=math.sqrt(sum(x*x for x in h));h=[x/length for x in h]
    nl=l[2];nv=1;nh=vh=h[2];a=max(rough,.045)**2;a2=a*a;D=a2/(math.pi*(nh*nh*(a2-1)+1)**2)
    visibility=.5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2))
    values=[]
    for base in color:
        f0=.04*(1-metal)+base*metal;F=f0+(1-f0)*(1-vh)**5
        linear=((1-F)*(1-metal)*base/math.pi+D*visibility*F)*nl*math.pi+base*(1-metal)*.035
        linear=linear/(1+linear);srgb=12.92*linear if linear<=.0031308 else 1.055*linear**(1/2.4)-.055;values.append(round(255*srgb))
    return values

try:
    sphere_path,_=write_fixture(run/'sources')
    vertices=[-1,-1,0,0,0,1, 1,-1,0,0,0,1, 1,1,0,0,0,1, -1,1,0,0,0,1]
    blob=struct.pack('<24f6I',*vertices,0,1,2,0,2,3)
    doc={'asset':{'version':'2.0'},'buffers':[{'byteLength':len(blob)}],'bufferViews':[{'buffer':0,'byteLength':96,'byteStride':24},{'buffer':0,'byteOffset':96,'byteLength':24}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[-1,-1,0],'max':[1,1,0]},{'bufferView':0,'byteOffset':12,'componentType':5126,'count':4,'type':'VEC3'},{'bufferView':1,'componentType':5125,'count':6,'type':'SCALAR'}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2}]}],'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
    quad=run/'sources/quad.glb';quad.write_bytes(glb(doc,blob))
    imported=batch([('asset.import',{'source':native(p)}) for p in [sphere_path,quad]])
    sphere_asset=result(imported[0])['asset'];quad_asset=result(imported[1])['asset']
    root=uid(100);mesh=derived(100,'node/0/primitive/0')
    ops=[{'op':'entity.create','id':uid(1),'name':'Camera'},set_component(uid(1),'Transform',transform((0,0,4))),set_component(uid(1),'Camera',{'vertical_fov':60,'near':.1,'far':100}),
         {'op':'asset.instantiate','id':root,'name':'Reference plane','asset':quad_asset},set_component(mesh,'PbrMaterial',material())]
    requests=[edit(0,ops),capture('dielectric',1)]
    requests+=[edit(1,[set_component(mesh,'PbrMaterial',material(metal=1))]),capture('metal',2)]
    requests+=[edit(2,[set_component(mesh,'PbrMaterial',material(metal=1,rough=.9))]),capture('rough',3)]
    requests+=[edit(3,[set_component(root,'Transform',transform((0,0,0),(0,1,0,0)))]),capture('back-culled',4)]
    requests+=[edit(4,[set_component(mesh,'PbrMaterial',material(metal=1,rough=.9,double=True))]),capture('back-double',5)]
    # Freeze an imported scene in the runtime, then change its authored material.
    requests+=[('runtime.start',{'session_id':uid(900),'revision':5}),edit(5,[set_component(mesh,'PbrMaterial',material(color=(0,1,0),double=True))]),
              ('runtime.capture',{'session_id':uid(900),'tick':0,'camera':uid(1),'path':native(run/'frozen.bmp'),'gpu':args.gpu}),capture('edited',6)]
    responses=batch(requests)
    for row in responses:result(row)
    checks={};measurements=[]
    for name,metal,rough in [('dielectric',0,.5),('metal',1,.5),('rough',1,.9)]:
        image=pixels(run/(name+'.bmp'));actual=image[270][480];expected=cpu_color(metal,rough,(.6,.2,.05));measurements.append({'name':name,'actual':actual,'expected':expected})
        assert max(abs(a-b) for a,b in zip(actual,expected))<=4,(name,actual,expected)
    background=pixels(run/'back-culled.bmp')[270][480]
    assert pixels(run/'dielectric.bmp')[270][480]!=background
    assert pixels(run/'back-double.bmp')[270][480]!=background
    assert pixels(run/'frozen.bmp')==pixels(run/'back-double.bmp')
    assert pixels(run/'edited.bmp')!=pixels(run/'frozen.bmp')
    # Import source removal cannot change the immutable cooked asset. Reopening
    # reproduces pixels without any glTF parse/render-time source dependency.
    sphere_path.unlink();quad.unlink();reopened=batch([capture('reopened',6)]);result(reopened[0]);assert pixels(run/'edited.bmp')==pixels(run/'reopened.bmp')
    ops=[{'op':'entity.delete','id':root,'recursive':True},set_component(uid(1),'Transform',transform((0,0,11))),set_component(uid(1),'Camera',{'vertical_fov':34,'near':.1,'far':100})]
    for row in range(2):
        for col in range(4):
            n=200+row*4+col;ops+=[{'op':'asset.instantiate','id':uid(n),'name':f'Material {row} {col}','asset':sphere_asset},set_component(uid(n),'Transform',transform(((col-1.5)*2.8,(.5-row)*2.8,0))),
                set_component(derived(n,'node/1/primitive/0'),'PbrMaterial',material(metal=col/3,rough=.18 if row==0 else .65,color=(.75,.36,.12)))]
    rows=batch([edit(6,ops),capture('material-grid',7)]);result(rows[0]);grid=result(rows[1]);assert grid['object_count']==8 and grid['nvrhi_errors']==0
    for run_record in record['runs']:
        for response in run_record['responses']:
            r=response.get('result',{})
            if 'capture_written' in r:assert r['capture_written'] and r['hardware'] and r['nvrhi_errors']==0,r
    record['checks']={'gpu_brdf_matches_scalar_reference':True,'backface_culling_and_double_sided':True,'frozen_runtime_material':True,'authoring_edit_visible':True,'reopen_without_sources_pixel_identical':True,'instanced_indexed_geometry':True}
    record['brdf_samples']=measurements;record['assets']={'sphere':sphere_asset,'quad':quad_asset};record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')};record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'brdf_samples':record['brdf_samples']}))
