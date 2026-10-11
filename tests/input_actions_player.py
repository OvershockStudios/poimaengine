#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual Vulkan player named-action replay versus independent headless batches."""
import argparse,copy,json,sys,time,uuid
from pathlib import Path
from types import SimpleNamespace
from input_actions_gameplay_contract import OwnedSession,action,scene,exact,need
from components_gameplay_contract import sha,uid

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--hostfxr',type=Path,required=True);p.add_argument('--bridge',type=Path,required=True);p.add_argument('--assembly',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,default=0);a=p.parse_args()
 run=a.output.resolve();run.mkdir(parents=True,exist_ok=False);a.native=lambda v:str(Path(v).resolve());a.deadline=time.monotonic()+120
 config=dict(hostfxr=a.native(a.hostfxr),bridge=a.native(a.bridge),assembly=a.native(a.assembly),type='Poima.Tests.ManagedInputActionsGame')
 sequence=[dict(ticks=3,actions=[action('aim',True,True),action('fire',True,True)],use=True),dict(ticks=2,actions=[action('aim',True),action('fire',True)]),dict(ticks=1,actions=[action('aim',released=True),action('fire',released=True),action('reload',pressed=True,released=True)]),dict(ticks=1,actions=[action('fire',pressed=True,released=True)]),dict(ticks=1)]
 e=dict(passed=False,calls=[],owners=[],checks=[],captures=[],limitations=['Synthetic replay on one Windows GPU; no physical input, weapon assets, animation, latency or performance claim.']);s=None
 paths=[Path(__file__),Path(__file__).with_name('input_actions_gameplay_contract.py'),Path(__file__).with_name('components_gameplay_contract.py'),Path(__file__).with_name('character_gameplay_contract.py'),a.binary,a.hostfxr,a.bridge,a.assembly,a.bridge.parent/'Poima.Gameplay.dll',a.assembly.parent/'Poima.Gameplay.dll'];pins={str(x.resolve()):sha(x) for x in paths};e['source_pins']=pins
 try:
  s=OwnedSession(a,run/'world.json',e);s.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=scene()));original=s.world.read_bytes();saves=run/'saves';saves.mkdir();s.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(saves)))
  defaults=s.rpc('input.describe')['defaults'];profile=copy.deepcopy(defaults);profile['actions']={'aim':['mouse.3'],'fire':['mouse.1'],'reload':['key.r']};profilepath=run/'combat.poima-input.json';s.rpc('input.transact',dict(path=str(profilepath),request_id=uuid.uuid4().hex,expected_revision=0,profile=profile))
  def snapshot(slot):
   r=s.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=0,session_id=s.session,expected_tick=s.tick,expected_gameplay_revision=s.game()['revision']));files=list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(files)==1 and sha(files[0])==r['sha256'],'Save receipt differs');return json.loads(files[0].read_text())['snapshot']
  s.start(1);s.load(config)
  for segment in sequence:
   r=s.rpc('runtime.step',dict(session_id=s.session,request_id=uuid.uuid4().hex,expected_tick=s.tick,ticks=segment['ticks'],inputs=[dict(entity=uid(100),**{k:v for k,v in segment.items() if k!='ticks'})]));s.tick=r['current_tick']
  expected=snapshot('headless');s.rpc('runtime.stop',dict(session_id=s.session))
  for lighting in ('forward','deferred'):
   s.start(1);s.load(config);capture=run/(lighting+'.bmp')
   r=s.rpc('runtime.play',dict(session_id=s.session,request_id=uuid.uuid4().hex,expected_tick=0,controller=uid(100),camera=uid(101),mode='replay',sequence=sequence,path=str(capture),width=320,height=180,gpu=a.gpu,samples=1,lighting_path=lighting,input_profile=str(profilepath),input_revision=1))
   # Play retains the same session and its committed tick; inspect authoritative state.
   need(r['success'] and r['hardware'] and r['capture_written'] and r['nvrhi_errors']==0 and r['stop_reason']=='replay_complete' and r['frames_presented']>0 and r['initial_session_id']==r['current_session_id']==s.session and r['runtime_replacements']==0 and r['render_diagnostics']['lighting_path']==lighting,'Actual renderer report failed');s.tick=s.rpc('runtime.inspect',dict(session_id=s.session))['tick'];actual=snapshot(lighting)
   need(s.tick==8 and exact(actual,expected),'Player replay differs from full independently stepped checkpoint')
   need(r['input_profile']['format']=='poima.input.v3' and r['input_profile']['applied'] is False,'Replay reinterpreted bindings')
   need(capture.is_file() and capture.stat().st_size<1048576,'Bounded capture missing');e['captures'].append(dict(lighting_path=lighting,bytes=capture.stat().st_size,sha256=sha(capture),report=r,values=s.game()['module']['values']));s.rpc('runtime.stop',dict(session_id=s.session))
  need(s.world.read_bytes()==original,'Playback changed authored world');s.close();s=None
  need(all(sha(n)==h for n,h in pins.items()),'Source/image changed');e['checks'].append('Both native Vulkan render paths preserve full headless checkpoint across held actions, first-tick edges, quick taps, reload and old Use; explicit v3 bindings remain unapplied to semantic replay');e['passed']=True
 except BaseException as error:e['error']=repr(error);raise
 finally:
  if s:
   try:s.close()
   except BaseException as error:e['close_error']=repr(error);e['passed']=False
  data=(json.dumps(e,indent=2)+'\n').encode();need(len(data)<=32*1048576,'Evidence exceeds32MiB');(run/'evidence.json').write_bytes(data)
  if time.monotonic()>a.deadline:
   e['passed']=False;e['deadline_error']='Final publication exceeded120s';(run/'evidence.json').write_text(json.dumps(e,indent=2)+'\n');raise TimeoutError(e['deadline_error'])
  print(run/'evidence.json')
  if e.get('close_error'):raise RuntimeError(e['close_error'])
if __name__=='__main__':main()
