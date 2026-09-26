#!/usr/bin/env python3
"""Native Vulkan texture channels, color space, UV/sampler/mip and persistence checks."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import struct
import uuid
from texture_fixture import quad,png
from gltf_fixture import glb
from scene_capture import pixels

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True)
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'gpu_index':args.gpu,'runs':[],'samples':[]}
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
world=run/'texture.world.json';rev=0

def batch(requests):
    rows=[{'jsonrpc':'2.0','id':i+1,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    process=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=120)
    replies=[json.loads(s) for s in process.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':process.returncode,'stderr':process.stderr})
    assert process.returncode==0 and len(replies)==len(rows),process.stdout+process.stderr
    for reply in replies:
        assert 'result' in reply,reply
        result=reply['result']
        if 'capture_written' in result:assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,result
    return [r['result'] for r in replies]
def transaction(ops):return ('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':rev,'ops':ops})
def capture(name):return ('world.capture',{'revision':rev,'camera':uid(1),'path':native(run/(name+'.bmp')),'gpu':args.gpu,'samples':4})
def component(id,kind,value):return {'op':'component.set','id':id,'type':kind,'value':value}
def transform(position):return {'position':position,'rotation':[0,0,0,1],'scale':[1,1,1]}
def lin(v):
    x=v/255;return x/12.92 if x<=.04045 else ((x+.055)/1.055)**2.4
def display(x):
    x=x/(1+x);return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
def emission(rgb):return [display(lin(v)) for v in rgb]
def check(name,expected,xy=(480,270),tolerance=3):
    actual=pixels(run/(name+'.bmp'))[xy[1]][xy[0]];record['samples'].append({'name':name,'pixel':xy,'actual':actual,'expected':expected,'tolerance':tolerance})
    assert max(abs(a-b) for a,b in zip(actual,expected))<=tolerance,(name,actual,expected)
def emissive_material():return {'pbrMetallicRoughness':{'baseColorFactor':[0,0,0,1],'metallicFactor':1,'roughnessFactor':1},'emissiveTexture':{'index':0},'emissiveFactor':[1,1,1]}
case_index=0

def render(name,images,material=None,sampler=None,uvs=None,normal=None):
    global rev,case_index
    doc,blob=quad(images,material,sampler,uvs)
    if normal is not None:
        blob=bytearray(blob)
        for i in range(4):struct.pack_into('<3f',blob,i*32+12,*normal)
    path=run/(name+'.glb');path.write_bytes(glb(doc,blob))
    asset=batch([('asset.import',{'source':native(path)})])[0]['asset'];ops=[]
    if case_index:ops.append({'op':'entity.delete','id':uid(100+case_index),'recursive':True})
    case_index+=1;ops.append({'op':'asset.instantiate','id':uid(100+case_index),'name':name,'asset':asset});batch([transaction(ops)]);rev+=1
    batch([capture(name)]);return asset,path

try:
    batch([transaction([{'op':'entity.create','id':uid(1),'name':'Camera'},component(uid(1),'Transform',transform([0,0,4])),component(uid(1),'Camera',{'vertical_fov':60,'near':.1,'far':100})])]);rev=1
    # A scalar reference, independent of the shader and image loader, checks all
    # four map semantics at a frontal sample: base/emission sRGB, MR G/B, AO R.
    rgb=[128,64,192];mr=[233,102,51];emit=[64,128,32];ao=[20,240,180]
    mat={'pbrMetallicRoughness':{'baseColorFactor':[.8,.6,.4,1],'metallicFactor':.7,'roughnessFactor':.8,'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':1}},
         'emissiveTexture':{'index':2},'emissiveFactor':[.2,.3,.4],'occlusionTexture':{'index':3,'strength':.65}}
    render('channels',[png(1,1,c+[255]) for c in [rgb,mr,emit,ao]],mat)
    base=[lin(c)*f for c,f in zip(rgb,[.8,.6,.4])];metal=.7*mr[2]/255;rough=.8*mr[1]/255
    nl=.6/math.sqrt(.16+.64+.36);nh=math.sqrt((1+nl)/2);a2=max(rough,.045)**4;D=a2/(math.pi*(nh*nh*(a2-1)+1)**2)
    vis=.5/(nl+math.sqrt(nl*nl*(1-a2)+a2));occlusion=1-.65+.65*ao[0]/255;expected=[]
    for i,b in enumerate(base):
        f0=.04*(1-metal)+b*metal;F=f0+(1-f0)*(1-nh)**5
        light=((1-F)*(1-metal)*b/math.pi+D*vis*F)*nl*math.pi+b*(1-metal)*.035*occlusion+lin(emit[i])*[.2,.3,.4][i]
        expected.append(display(light))
    check('channels',expected)
    # Point the shading normal away from the direct light to isolate ambient AO.
    ambient={'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'metallicFactor':0,'roughnessFactor':1},'occlusionTexture':{'index':0,'strength':.5}}
    render('ambient-occlusion',[png(1,1,[0,255,255,255])],ambient,normal=(0,-1,0));check('ambient-occlusion',[display(.035*.5)]*3)
    corners=[[255,0,0,255],[0,255,0,255],[0,0,255,255],[128,128,128,255]]
    render('quadrants',[png(2,2,sum(corners,[]))],emissive_material())
    for xy,rgb in zip([(430,220),(530,220),(430,320),(530,320)],corners):check('quadrants',emission(rgb[:3]),xy)
    red_green=png(2,1,[255,0,0,255,0,255,0,255])
    for wrap,u,expected in [(10497,1.25,[255,0,0]),(33071,1.25,[0,255,0]),(33648,1.25,[0,255,0]),(10497,-.25,[0,255,0]),(33071,-.25,[255,0,0]),(33648,-.25,[255,0,0])]:
        name=f'wrap-{wrap}-{u}';render(name,[red_green],emissive_material(),{'magFilter':9728,'minFilter':9728,'wrapS':wrap},[(u,.5)]*4);check(name,emission(expected))
    render('linear-filter',[red_green],emissive_material(),{'magFilter':9729,'minFilter':9729},[(.5,.5)]*4);check('linear-filter',[display(.5),display(.5),0])
    checker=[]
    for y in range(128):
        for x in range(128):checker.extend([255*((x+y)%2)]*3+[255])
    render('mip-checker',[png(128,128,checker)],emissive_material(),{'magFilter':9729,'minFilter':9987},[(0,80),(80,80),(80,0),(0,0)])
    check('mip-checker',[display(lin(188))]*3)
    for mode in [9984,9985,9986]:
        name=f'mip-filter-{mode}';render(name,[png(128,128,checker)],emissive_material(),{'magFilter':9729,'minFilter':mode},[(0,80),(80,80),(80,0),(0,0)]);check(name,[display(lin(188))]*3)
    # Single-level glTF minification must not silently use the generated chain.
    render('no-mip-checker',[png(128,128,checker)],emissive_material(),{'magFilter':9728,'minFilter':9728},[(0,80),(80,80),(80,0),(0,0)])
    region=pixels(run/'no-mip-checker.bmp');values={tuple(region[y][x]) for y in range(240,300) for x in range(450,510)};assert len(values)>=2
    jpeg=Path(__file__).parent/'fixtures/solid.jpg';render('jpeg',[jpeg.read_bytes()],emissive_material());check('jpeg',emission([124,76,212]),tolerance=3)
    # Freeze textured geometry in a runtime, remove its authored instance and
    # original glTF, then read back its pixels from the immutable runtime snapshot.
    name='persistent';asset,source=render(name,[png(2,2,sum(corners,[]))],emissive_material())
    source.unlink();batch([capture('reopened')]);assert pixels(run/'persistent.bmp')==pixels(run/'reopened.bmp')
    controller_ops=[{'op':'entity.create','id':uid(10),'name':'Replay controller'},component(uid(10),'Transform',transform([0,1,4])),
                    component(uid(10),'CharacterController',{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(11)}),
                    {'op':'entity.create','id':uid(11),'name':'Attached camera','parent':uid(10)},component(uid(11),'Transform',transform([0,1.6,0])),component(uid(11),'Camera',{'vertical_fov':60,'near':.1,'far':100})]
    batch([transaction(controller_ops)]);rev+=1
    session=uid(800)
    rows=batch([('runtime.start',{'session_id':session,'revision':rev}),transaction([{'op':'entity.delete','id':uid(100+case_index),'recursive':True}]),
                ('runtime.capture',{'session_id':session,'tick':0,'camera':uid(1),'path':native(run/'frozen.bmp'),'gpu':args.gpu,'samples':4}),
                ('runtime.play',{'session_id':session,'request_id':uid(900),'expected_tick':0,'controller':uid(10),'camera':uid(1),'mode':'replay','sequence':[{'ticks':120}],
                                 'path':native(run/'player.bmp'),'gpu':args.gpu,'samples':4})]);rev+=1
    assert pixels(run/'persistent.bmp')==pixels(run/'frozen.bmp')
    assert rows[-1]['success'] and rows[-1]['tick']==120 and pixels(run/'player.bmp')==pixels(run/'persistent.bmp')
    record['checks']={'ambient_occlusion_isolated':True,'continuous_textured_replay':True,'four_map_channels_scalar_reference':True,'uv_top_left_orientation':True,'repeat_clamp_mirror_positive_negative':True,'srgb_linear_filter':True,'srgb_minified_mips':True,'non_mip_sampler':True,'jpeg':True,'source_independent_reopen':True,'runtime_snapshot_owns_textures':True}
    record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')};record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'samples':record['samples']}))
