#!/usr/bin/env python3
"""Qualify live fixed-tick rig animation against isolated CPU reference pixels."""
# SPDX-License-Identifier: Apache-2.0
import argparse, hashlib, json, subprocess, uuid
from pathlib import Path
from animation_fixture import ribbon
from gltf_fixture import glb
from scene_capture import pixels
p=argparse.ArgumentParser();p.add_argument('binary',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,default=0);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
run=a.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'world.json';record={'passed':False,'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'runs':[]}
def native(path):return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if a.windows_interop else str(path.resolve())
def uid(n):return f'{n:032x}'
def batch(reqs):
 rows=[{'jsonrpc':'2.0','id':i,'method':m,'params':v} for i,(m,v) in enumerate(reqs)]
 result=subprocess.run([str(a.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=150)
 replies=[json.loads(s) for s in result.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':result.returncode,'stderr':result.stderr})
 assert result.returncode==0 and len(replies)==len(rows),result.stdout+result.stderr
 for r in replies:assert 'result' in r,r
 return [r['result'] for r in replies]
def comp(n,t,v):return {'op':'component.set','id':uid(n),'type':t,'value':v}
def create(n,name,pos):return [{'op':'entity.create','id':uid(n),'name':name},comp(n,'Transform',{'position':pos,'rotation':[0,0,0,1],'scale':[1,1,1]})]
def capture(name):return {'camera':uid(1),'path':native(run/(name+'.bmp')),'width':640,'height':480,'gpu':a.gpu,'samples':4,'profile':True}
try:
 doc,blob=ribbon();source=run/'ribbon.glb';source.write_bytes(glb(doc,blob));asset=batch([('asset.import',{'source':native(source)})])[0]['asset']
 ops=create(1,'Observer',[.5,1,5])+[comp(1,'Camera',{'vertical_fov':50,'near':.1,'far':100})]
 ops+=create(10,'Offscreen controller',[50,0,0])+create(11,'Controller camera',[0,1.6,0])
 ops += [{'op':'entity.reparent','id':uid(11),'parent':uid(10),'mode':'keep_local'} ,comp(11,'Camera',{'vertical_fov':60,'near':.1,'far':100}),comp(10,'CharacterController',{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(11)})]
 ops += [{'op':'asset.instantiate','id':uid(100),'name':'Live rig','asset':asset},comp(100,'AnimationRig',{'asset':asset,'clip':0,'time':0,'speed':1,'loop':True,'playing':True})]
 batch([('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':0,'ops':ops})]);before=world.read_bytes();source.unlink()
 play={'session_id':uid(900),'request_id':uid(901),'expected_tick':0,'controller':uid(10),'mode':'replay','sequence':[{'ticks':60}],**capture('play')}
 results=batch([('world.capture',{'revision':1,**capture('authored-rest')}),('runtime.start',{'session_id':uid(900),'revision':1}),('runtime.play',play),('runtime.capture',{'session_id':uid(900),'tick':60,**capture('live')}),('runtime.entity',{'session_id':uid(900),'id':uid(100)}),('asset.animation.capture',{'revision':1,'asset':asset,'clip':0,'time':1,'skinning':'cpu',**capture('reference')}),('world.capture',{'revision':1,**capture('authored-after')})])
 for report in [results[i] for i in [0,2,3,5,6]]:assert report['capture_written'] and report['hardware'] and report['nvrhi_errors']==0,report
 assert results[2]['tick']==60 and results[2]['frames_presented']==61,results[2]
 assert abs(results[4]['animation']['time']-1)<1e-10,results[4]
 images={n:pixels(run/(n+'.bmp')) for n in ['play','live','reference','authored-rest','authored-after']}
 def diff(x,y):
  values=[max(abs(u-v) for u,v in zip(i,j)) for row,other in zip(images[x],images[y]) for i,j in zip(row,other)]
  return {'maximum':max(values),'changed':sum(v!=0 for v in values),'over_two':sum(v>2 for v in values)}
 comparisons={'continuous_vs_capture':diff('play','live'),'live_vs_cpu_reference':diff('live','reference'),'authored_unchanged':diff('authored-rest','authored-after'),'motion':diff('authored-rest','live')}
 assert comparisons['continuous_vs_capture']['changed']==0,comparisons
 assert comparisons['live_vs_cpu_reference']['over_two']<=64,comparisons
 assert comparisons['authored_unchanged']['changed']==0 and comparisons['motion']['over_two']>1000,comparisons
 assert before==world.read_bytes()
 assert results[2]['render_diagnostics']['last_draws']['skinned_instances']==1,results[2]
 record.update(passed=True,comparisons=comparisons,play=results[2],rig=results[4],authored_bytes_unchanged=True,images={n:hashlib.sha256((run/(n+'.bmp')).read_bytes()).hexdigest() for n in images})
finally:
 out=run/'evidence.json';out.write_text(json.dumps(record,indent=2)+'\n');print(out)
