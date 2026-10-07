#!/usr/bin/env python3
"""Qualify real Vulkan shadows with occluders, six point faces, cascades and live physics."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import struct
import uuid
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'shadows.world.json';rev=0
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
def aim(direction):
    d=normalize(direction);q=[d[1],-d[0],0,1-d[2]]
    return normalize(q) if sum(x*x for x in q)>1e-12 else [0,1,0,0]
def center(name):return pixels(run/(name+'.bmp'))[270][480]
def dark(name):check(name,[display(b*.02) for b in BASE],tolerance=4)
def lit(name):
    value=center(name);record['samples'].append({'name':name,'actual':value,'minimum_red':60});assert value[0]>60,(name,value)
def shadow_light(kind,enabled=True):return light(kind=kind,intensity=math.pi if kind=='directional' else 20*math.pi,shadow={'enabled':enabled,'distance':80,'bias':.0001,'normal_bias':.005})
try:
    doc,blob=quad([],{'pbrMetallicRoughness':{'baseColorFactor':BASE+[1],'metallicFactor':0,'roughnessFactor':.6}})
    path=run/'quad.glb';path.write_bytes(glb(doc,blob));asset=batch([('asset.import',{'source':native(path)})])[0]['asset']
    smooth_blob=bytearray(blob)
    for vertex in range(4):struct.pack_into('<3f',smooth_blob,vertex*32+12,.5,0,math.sqrt(.75))
    smooth_path=run/'smooth-quad.glb';smooth_path.write_bytes(glb(doc,bytes(smooth_blob)));smooth_asset=batch([('asset.import',{'source':native(smooth_path)})])[0]['asset']
    mesh=hashlib.sha256(('poima.instance.v1/'+uid(100)+'/node/0/primitive/0').encode()).hexdigest()[:32]
    blocker={'primitive':'box','visible':True,'albedo':[.2,.2,.2]}
    edit([create(1),transform(1,(0,0,6)),component(1,'Camera',{'vertical_fov':60,'near':.1,'far':100}),create(2),transform(2,(2,0,4),aim([-2,0,-4])),shadow_light('directional',False),create(3),env((.02,.02,.02)),create(4),transform(4,(1,0,2),s=(.8,.8,.8)),component(4,'MeshRenderer',blocker),{'op':'asset.instantiate','id':uid(100),'name':'Receiver','asset':asset},transform(100,s=(4,4,4))])
    batch([capture('unshadowed')]);lit('unshadowed')
    for kind in ['directional','spot','point']:
        edit([shadow_light(kind)]);batch([capture(kind)]);dark(kind)
        edit([shadow_light(kind,False)]);batch([capture(kind+'-off')]);lit(kind+'-off')
    # Every cascade sees the same receiver, including both sides of blend bands.
    edit([shadow_light('directional')])
    splits=[.6*.1*(80/.1)**(i/4)+.4*(.1+(80-.1)*i/4) for i in [1,2,3]]
    for i,d in enumerate([4,12,25,50,*[v+offset for v in splits for offset in [-.02,.02]]]):
        edit([transform(1,(0,0,d))]);batch([capture('cascade-'+str(i))]);dark('cascade-'+str(i))
    edit([transform(1,(0,0,85))]);batch([capture('outside-shadow-distance')]);lit('outside-shadow-distance')
    edit([transform(1,(0,0,6)),transform(4,(3,0,2))]);batch([capture('moved-occluder')]);lit('moved-occluder')
    edit([transform(4,(1,0,2)),component(4,'MeshRenderer',{**blocker,'visible':False})]);batch([capture('hidden-occluder')]);lit('hidden-occluder');check('hidden-occluder',center('unshadowed'),tolerance=1)
    # Authored smooth normals need not equal the actual triangle plane. They
    # must affect BRDF shading without introducing receiver self-shadow stripes.
    edit([component(mesh,'StaticMesh',{'asset':smooth_asset,'primitive':0,'visible':True})]);batch([capture('smooth-normal-shadow')])
    edit([shadow_light('directional',False)]);batch([capture('smooth-normal-unshadowed')])
    a=pixels(run/'smooth-normal-shadow.bmp');b=pixels(run/'smooth-normal-unshadowed.bmp')
    error=max(abs(a[y][x][c]-b[y][x][c]) for y in range(60,481,20) for x in range(250,711,20) for c in range(3))
    record['smooth_normal_max_error']=error;assert error<=1,('Smooth normals changed shadow visibility',error)
    edit([component(mesh,'StaticMesh',{'asset':asset,'primitive':0,'visible':True}),shadow_light('directional')])
    edit([transform(4,(1,0,2),s=(.6,.6,.6)),{'op':'component.remove','id':uid(4),'type':'MeshRenderer'},component(4,'StaticMesh',{'asset':asset,'primitive':0,'visible':True})]);batch([capture('indexed-occluder')]);dark('indexed-occluder')
    edit([{'op':'component.remove','id':uid(4),'type':'StaticMesh'},component(4,'MeshRenderer',blocker)])
    # Move the entire test rig around a point source; the oblique camera leaves
    # the occluder out of its center ray while observing the shadowed receiver.
    axes=[[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1],normalize([1,1,0])]
    for i,axis in enumerate(axes):
        tangent=normalize([axis[1],-axis[0],0]) if abs(axis[2])<.9 else [1,0,0]
        target=[4*x for x in axis];camera=[-a+3*b for a,b in zip(axis,tangent)]
        edit([transform(2),shadow_light('point'),transform(100,target,aim(axis),(4,4,4)),transform(4,[2*x for x in axis],s=(.8,.8,.8)),transform(1,camera,aim([a-b for a,b in zip(target,camera)]))])
        batch([capture('face-'+str(i))]);dark('face-'+str(i))
        edit([shadow_light('point',False)]);batch([capture('face-'+str(i)+'-off')]);lit('face-'+str(i)+'-off')
    # Exercise the final texture-array/constant-buffer slot with a -Z point face:
    # directional (4), dark point (6), active point (6) -> layer 15 is sampled.
    edit([transform(1,(0,0,6)),transform(100,s=(4,4,4)),transform(4,(1,0,2),s=(.8,.8,.8)),transform(2,(2,0,4),aim([-2,0,-4])),light(intensity=0,shadow={'enabled':True}),create(10),light(10,'point',intensity=0,shadow={'enabled':True}),create(11),transform(11,(2,0,4)),light(11,'point',intensity=20*math.pi,shadow={'enabled':True,'bias':.0001})])
    rows=batch([('world.lighting',{}),capture('last-shadow-layer')]);assert rows[0]['shadow_views']==16;dark('last-shadow-layer')
    edit([light(11,'point',intensity=20*math.pi,shadow={'enabled':False})]);batch([capture('last-shadow-layer-off')]);lit('last-shadow-layer-off')
    edit([{'op':'entity.delete','id':uid(n),'recursive':True} for n in [10,11]]+[shadow_light('spot')])
    for resolution in [256,512,2048]:
        edit([component(3,'LightingEnvironment',{'ambient':[.02,.02,.02],'exposure':1,'shadow_resolution':resolution})]);batch([capture('resolution-'+str(resolution))]);dark('resolution-'+str(resolution))
    edit([env((.02,.02,.02))])
    # A live dynamic occluder must clear its old shadow, including within play.
    edit([transform(1,(0,0,6)),transform(100,s=(4,4,4)),transform(2,(2,0,4),aim([-2,0,-4])),shadow_light('spot'),transform(4,(1,0,2),s=(.8,.8,.8)),component(4,'BoxCollider',{'half_extents':[.5,.5,.5],'motion':'dynamic','mass':1,'friction':.5,'restitution':0})])
    session=uid(900);rows=batch([('runtime.start',{'session_id':session,'revision':rev}),capture('runtime-initial',session),transaction([shadow_light('spot',False)]),capture('runtime-frozen',session),('runtime.step',{'session_id':session,'request_id':uid(901),'expected_tick':0,'ticks':30}),capture('runtime-moved',session,30)]);rev+=1
    dark('runtime-initial');assert pixels(run/'runtime-initial.bmp')==pixels(run/'runtime-frozen.bmp');lit('runtime-moved')
    record['passed']=True;record['checks']={'smooth_normals_do_not_change_receiver_plane':True,'all_resolutions_and_final_array_layer':True,'directional_spot_point_occlusion':True,'all_cascades_and_blends':True,'shadow_distance_bound':True,'moving_hidden_and_indexed_casters':True,'six_point_faces_and_seam':True,'frozen_runtime_settings_and_moving_occluder':True}
    record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')}
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'samples':record['samples']}))
