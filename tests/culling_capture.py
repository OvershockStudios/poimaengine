#!/usr/bin/env python3
"""Verify camera/shadow culling by pixel parity and inspect real GPU pass timings."""
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

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'visibility.world.json';rev=0
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'gpu_index':args.gpu,'runs':[],'samples':[]}
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
        if 'capture_written' in value:assert value['capture_written'] and value['hardware'] and value['nvrhi_errors']==0,value
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
    p={'camera':uid(1),'path':native(run/(name+'.bmp')),'gpu':args.gpu,'samples':4}
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
def observe(name,culling=True,profile=True,session=None,tick=0):
    method,p=capture(name,session,tick);return method,{**p,'culling':culling,'profile':profile}
def audit(result,culling,profile):
    d=result['render_diagnostics'];c=d['last_draws'];assert d['culling']==culling and d['profile_requested']==profile
    assert c['camera_draws']+c['camera_culled']==c['objects']
    assert c['shadow_draws']+c['shadow_culled']==c['shadow_candidates']==c['objects']*c['shadow_views']
    assert d['completed_submissions']==result['frames_presented']==2
    if profile:
        assert d['gpu']['available'] and d['gpu']['timestamp_valid_bits']>0 and d['gpu']['timestamp_period_ns']>0,d
        assert d['gpu']['samples_dropped']==0
        for kind in ['shadows','opaque','post','total']:
            t=d['gpu'][kind];assert t['samples']==2 and 0<=t['min_ms']<=t['mean_ms']<=t['max_ms'] and math.isfinite(t['last_ms']),t
        assert d['gpu']['total']['max_ms']>0
        for kind in ['prepare','record','render_call']:assert d['cpu'][kind]['samples']>0
    else:
        assert not d['gpu']['available'] and d['gpu']['total']['mean_ms'] is None
        assert d['cpu']['record']['samples']==0
    if not culling:assert c['camera_culled']==c['shadow_culled']==0
    return d
try:
    doc,blob=quad([],{'pbrMetallicRoughness':{'baseColorFactor':BASE+[1],'metallicFactor':0,'roughnessFactor':.6}})
    path=run/'receiver.glb';path.write_bytes(glb(doc,blob));asset=batch([('asset.import',{'source':native(path)})])[0]['asset']
    box={'primitive':'box','visible':True,'albedo':[.2,.3,.4]}
    edit([create(1),transform(1,(0,0,6)),component(1,'Camera',{'vertical_fov':60,'near':.1,'far':100}),create(2),transform(2,(7.5,0,6)),light(kind='point',intensity=100*math.pi,shadow={'enabled':True,'distance':30,'bias':.0001}),create(3),env((.02,.02,.02)),create(4),transform(4,(5,0,4),s=(1.2,1.2,1.2)),component(4,'MeshRenderer',box),create(5),transform(5,(-2,1,0)),component(5,'MeshRenderer',box),create(6),transform(6,(-1,0,5.95)),component(6,'MeshRenderer',box),create(7),transform(7,(2,1,-2),aim([-.4,0,-1]),(3,.6,1)),create(8,uid(7)),transform(8,(-.1,0,0),(0,0,math.sin(.3),math.cos(.3)),(1,2,.7)),component(8,'MeshRenderer',box),{'op':'asset.instantiate','id':uid(100),'name':'Receiver','asset':asset},transform(100,s=(4,4,4))])
    for base in range(1000,1240,80):
        edit([op for n in range(base,base+80) for op in [create(n),transform(n,((1 if n%2 else -1)*(200+n%80),n%17,-150-n%80)),component(n,'MeshRenderer',box)]])
    plain,culled,unprofiled=batch([observe('all-draws',False),observe('culled'),observe('unprofiled',True,False)])
    a=audit(plain,False,True);b=audit(culled,True,True);audit(unprofiled,True,False)
    assert pixels(run/'all-draws.bmp')==pixels(run/'culled.bmp')==pixels(run/'unprofiled.bmp')
    assert b['last_draws']['camera_culled']>=241 and b['last_draws']['shadow_culled']>=240*6,b
    dark('culled') # An offscreen box must still cast onto the visible receiver.
    record['comparison']={'unculled':a,'culled':b}
    # Disable only the offscreen caster; its shadow must disappear with culling on.
    edit([component(4,'MeshRenderer',{**box,'visible':False})]);rows=batch([observe('caster-hidden')]);audit(rows[0],True,True);lit('caster-hidden')
    edit([component(4,'MeshRenderer',box),component(4,'BoxCollider',{'half_extents':[.5,.5,.5],'motion':'dynamic','mass':1,'friction':.5,'restitution':0})])
    session=uid(900);rows=batch([('runtime.start',{'session_id':session,'revision':rev}),observe('runtime-all',False,True,session),observe('runtime-culled',True,True,session),('runtime.step',{'session_id':session,'request_id':uid(901),'expected_tick':0,'ticks':60}),observe('moved-all',False,True,session,60),observe('moved-culled',True,True,session,60)])
    for i,c in [(1,False),(2,True),(4,False),(5,True)]:audit(rows[i],c,True)
    assert pixels(run/'runtime-all.bmp')==pixels(run/'runtime-culled.bmp')
    assert pixels(run/'moved-all.bmp')==pixels(run/'moved-culled.bmp');lit('moved-culled')
    record['passed']=True;record['checks']={'profiled_and_unprofiled_pixel_parity':True,'offscreen_shadow_caster_retained':True,'240_distant_objects_rejected':True,'near_plane_and_sheared_hierarchy':True,'runtime_motion_parity':True,'actual_gpu_timestamp_intervals':True}
    record['images']={p.stem:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')}
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'comparison':record['comparison']}))
