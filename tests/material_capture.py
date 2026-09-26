#!/usr/bin/env python3
"""GPU qualification of tangent-space normals and transactional texture overrides."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import uuid
from texture_fixture import quad,png
from gltf_fixture import glb
from scene_capture import pixels

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'material.world.json';rev=0
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'gpu_index':args.gpu,'runs':[],'samples':[]}
def uid(n):return f'{n:032x}'
mesh=hashlib.sha256(('poima.instance.v1/'+uid(100)+'/node/0/primitive/0').encode()).hexdigest()[:32]
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def batch(requests,errors=None):
    rows=[{'jsonrpc':'2.0','id':i+1,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    process=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=120)
    replies=[json.loads(s) for s in process.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':process.returncode,'stderr':process.stderr})
    assert process.returncode==0 and len(replies)==len(rows),process.stdout+process.stderr
    for i,reply in enumerate(replies):
        if errors and i in errors:assert reply['error']['code']==errors[i],reply;continue
        assert 'result' in reply,reply
        result=reply['result']
        if 'capture_written' in result:assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,result
    return replies
def result(row):return row['result']
def transaction(ops,preview=False,request=None):return ('world.transact',{'request_id':request or uuid.uuid4().hex,'base_revision':rev,'ops':ops,'preview':preview})
def capture(name):return ('world.capture',{'revision':rev,'camera':uid(1),'path':native(run/(name+'.bmp')),'gpu':args.gpu,'samples':4})
def component(id,kind,value):return {'op':'component.set','id':id,'type':kind,'value':value}
def edit(ops):
    global rev
    batch([transaction(ops)]);rev+=1
def transform(position=(0,0,0),scale=(1,1,1)):return {'position':list(position),'rotation':[0,0,0,1],'scale':list(scale)}
def check(name,expected,tolerance=3):
    actual=pixels(run/(name+'.bmp'))[270][480];record['samples'].append({'name':name,'actual':actual,'expected':expected,'tolerance':tolerance});assert max(abs(a-b) for a,b in zip(actual,expected))<=tolerance,(name,actual,expected)
def normalize(v):
    length=math.sqrt(sum(x*x for x in v));return [x/length for x in v]
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def display(x):
    x=x/(1+x);return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
def reference(n):
    n=normalize(n);l=normalize([-.4,.8,.6]);v=[0,0,1];h=normalize([l[i]+v[i] for i in range(3)]);nl=max(dot(n,l),0);nv=max(dot(n,v),1e-5);nh=max(dot(n,h),0);vh=max(dot(v,h),0)
    a2=.6**4;D=a2/(math.pi*(nh*nh*(a2-1)+1)**2);vis=.5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2));F=.04+.96*(1-vh)**5
    return [display(((1-F)*base/math.pi+D*vis*F)*nl*math.pi+base*.035) for base in [.6,.2,.05]]
def image_asset(name,rgb,space):
    path=run/(name+'.png');path.write_bytes(png(1,1,rgb+[255]));return result(batch([('asset.image.import',{'source':native(path),'color_space':space})])[0])['asset']
try:
    normal=[204,76,230];sampled=[v/255*2-1 for v in normal];base={'pbrMetallicRoughness':{'baseColorFactor':[.6,.2,.05,1],'metallicFactor':0,'roughnessFactor':.6},'normalTexture':{'index':0},'emissiveTexture':{'index':1}}
    images=[png(1,1,normal+[255]),png(1,1,[0,0,255,255])];assets=[]
    for name,uvs,tangent in [('generated',None,None),('mirrored',[(1,1),(0,1),(0,0),(1,0)],None),('authored',None,[0,1,0,1]),('diagonal',None,[math.sqrt(.5),math.sqrt(.5),0,1])]:
        doc,blob=quad(images,base,uvs=uvs)
        if tangent:
            blob+=b'\0'*((-len(blob))%4);offset=len(blob);blob+=struct.pack('<16f',*(tangent*4));doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':64});doc['accessors'].append({'bufferView':len(doc['bufferViews'])-1,'componentType':5126,'count':4,'type':'VEC4'});doc['meshes'][0]['primitives'][0]['attributes']['TANGENT']=len(doc['accessors'])-1;doc['buffers'][0]['byteLength']=len(blob)
        path=run/(name+'.glb');path.write_bytes(glb(doc,blob));assets.append(result(batch([('asset.import',{'source':native(path)})])[0])['asset'])
    inherited_emissive=result(batch([('asset.inspect',{'asset':assets[0],'section':'primitives'})])[0])['items'][0]['textures']['emissive']
    edit([{'op':'entity.create','id':uid(1),'name':'Camera'},component(uid(1),'Transform',transform((0,0,4))),component(uid(1),'Camera',{'vertical_fov':60,'near':.1,'far':100}),{'op':'asset.instantiate','id':uid(100),'name':'Material test','asset':assets[0]}])
    batch([capture('generated')]);check('generated',reference([sampled[0],-sampled[1],sampled[2]]))
    edit([component(mesh,'PbrTextures',{'normal_scale':0})]);batch([capture('zero-scale')]);check('zero-scale',reference([0,0,1]))
    edit([{'op':'component.remove','id':mesh,'type':'PbrTextures'},component(mesh,'StaticMesh',{'asset':assets[1],'primitive':0,'visible':True})]);batch([capture('mirrored')]);check('mirrored',reference([-sampled[0],-sampled[1],sampled[2]]))
    edit([component(mesh,'StaticMesh',{'asset':assets[2],'primitive':0,'visible':True})]);batch([capture('authored')]);check('authored',reference([-sampled[1],sampled[0],sampled[2]]))
    edit([component(mesh,'StaticMesh',{'asset':assets[3],'primitive':0,'visible':True}),component(uid(100),'Transform',transform(scale=(2,1,.5)))]);batch([capture('nonuniform')]);t=normalize([2,1,0]);b=[-t[1],t[0],0];check('nonuniform',reference([t[i]*sampled[0]+b[i]*sampled[1]+([0,0,1][i])*sampled[2] for i in range(3)]))
    red=image_asset('red',[255,0,0],'srgb');green=image_asset('green',[0,255,0],'srgb');linear=image_asset('wrong-space',[255,0,0],'linear')
    material={'base_color':[0,0,0],'emissive':[1,1,1],'metallic':1,'roughness':1,'double_sided':False}
    edit([component(uid(100),'Transform',transform()),component(mesh,'StaticMesh',{'asset':assets[0],'primitive':0,'visible':True}),component(mesh,'PbrMaterial',material),component(mesh,'PbrTextures',{'normal':None,'emissive':{'asset':green}})])
    batch([capture('green')]);check('green',[0,188,0])
    preview=transaction([component(mesh,'PbrTextures',{'normal':None,'emissive':{'asset':red}})],preview=True);rows=batch([preview,('entity.material',{'id':mesh,'revision':rev}),capture('preview')]);assert not result(rows[0])['committed'] and result(rows[1])['textures']['emissive']['asset']==green;assert pixels(run/'preview.bmp')==pixels(run/'green.bmp')
    # Supply a valid controller for continuous replay through an independent camera.
    edit([{'op':'entity.create','id':uid(10),'name':'Controller'},component(uid(10),'Transform',transform((0,1,4))),component(uid(10),'CharacterController',{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(11)}),{'op':'entity.create','id':uid(11),'name':'Camera child','parent':uid(10)},component(uid(11),'Transform',transform((0,1.6,0))),component(uid(11),'Camera',{'vertical_fov':60,'near':.1,'far':100})])
    change=transaction([component(mesh,'PbrTextures',{'normal':None,'emissive':{'asset':red}})]);session=uid(700)
    rows=batch([('runtime.start',{'session_id':session,'revision':rev}),change,change,('runtime.play',{'session_id':session,'request_id':uid(800),'expected_tick':0,'controller':uid(10),'camera':uid(1),'mode':'replay','sequence':[{'ticks':120}],'path':native(run/'frozen-player.bmp'),'gpu':args.gpu,'samples':4})]);rev+=1
    assert result(rows[2])['replayed'] and result(rows[3])['success'] and result(rows[3])['tick']==120
    assert pixels(run/'frozen-player.bmp')==pixels(run/'green.bmp')
    batch([capture('red')]);check('red',[188,0,0])
    for path in list(run.glob('*.png'))+list(run.glob('*.glb')):path.unlink()
    batch([capture('reopened')]);assert pixels(run/'reopened.bmp')==pixels(run/'red.bmp')
    edit([component(mesh,'PbrTextures',{'normal':None,'emissive':None})]);batch([capture('explicit-null')]);check('explicit-null',[188,188,188])
    edit([{'op':'component.remove','id':mesh,'type':'PbrTextures'}]);rows=batch([('entity.material',{'id':mesh}),capture('inherited')]);assert result(rows[0])['textures']['emissive']=={'asset':assets[0],**inherited_emissive};check('inherited',[0,0,188])
    edit([component(mesh,'PbrTextures',{'emissive':{'asset':linear}})]);batch([('entity.material',{'id':mesh}),capture('bad-space')],{0:-32050,1:-32050});assert not (run/'bad-space.bmp').exists()
    # Load an actual 0.0.8 package (32-byte vertices, no tangent attribute).
    legacy=(Path(__file__).parent/'fixtures/legacy-textured-v2.pmodel').read_bytes();legacy_id=hashlib.sha256(legacy).hexdigest();(Path(str(world)+'.assets')/(legacy_id+'.pmodel')).write_bytes(legacy)
    edit([{'op':'component.remove','id':mesh,'type':'PbrTextures'},component(mesh,'StaticMesh',{'asset':legacy_id,'primitive':0,'visible':True})]);batch([capture('legacy-v2')]);check('legacy-v2',[188,0,0])
    normal_asset=image_asset('box-normal',normal,'linear')
    edit([component(mesh,'PbrTextures',{'normal':{'asset':normal_asset}})]);batch([('entity.material',{'id':mesh})],{0:-32050})
    edit([{'op':'component.remove','id':mesh,'type':'StaticMesh'},component(mesh,'MeshRenderer',{'primitive':'box','visible':True,'albedo':[.6,.2,.05]}),component(mesh,'PbrMaterial',{'base_color':[.6,.2,.05],'emissive':[0,0,0],'metallic':0,'roughness':.6,'double_sided':False})]);batch([capture('box-normal')]);check('box-normal',reference(sampled))
    record['checks']={'legacy_v2_render_and_normal_rejection':True,'builtin_box_tangent_frame':True,'generated_authored_mirrored_tangent_normals':True,'normal_scale_zero':True,'nonuniform_transform':True,'standalone_image_overrides':True,'preview_read_only':True,'retry_receipt':True,'effective_material_inspection':True,'null_disable_remove_inherit':True,'frozen_runtime_120_tick_replay':True,'source_independent_reopen':True,'wrong_color_space_rejected_before_capture':True}
    record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')};record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'samples':record['samples']}))
