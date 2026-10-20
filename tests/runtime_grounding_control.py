#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original Knight requested contact controls, real jump and complete saves.

Builds/downloads nothing. Supply unchanged licensed Knight plus a matching native
engine. Optional compiled368-byte consumer and retained352-byte consumer are
separate explicit inputs. Run under an external process-tree owner. This is CPU
state/geometry evidence, not rendered gait, locking or a performance measurement.
"""
import argparse,copy,hashlib,json,math,time,traceback
from pathlib import Path
import runtime_ground_contacts as original
from runtime_ground_contacts import metric,base,need,identity,typed_equal,CHAINS,RIG,fk,close,distance
from character_library_intake import LibraryOracle,decode_cooked,LICENSE_SHA
from imported_character_contract import point,multiply
from components_gameplay_contract import uid

ROOT=Path(__file__).resolve().parents[1]
ACTOR=uid(920);FLOOR=original.FLOOR
GAME='Poima.Tests.ManagedGroundingControlGame'
IDENTITY='poima.test.managed-grounding-control'

def commands(enabled=True,weight=1.):
 return [dict(entity=RIG,slot=s,enabled=enabled,weight=weight)for s,*_ in CHAINS]

class Workflow(original.Workflow):
 def prepare(self):
  raw=self.args.source.read_bytes();self.oracle=LibraryOracle(raw)
  need(hashlib.sha256(self.args.license.read_bytes()).hexdigest()==LICENSE_SHA,'Exact original CC0 license')
  self.source=self.args.output/'original-Knight.glb';self.source.write_bytes(raw);(self.args.output/'original-LICENSE.txt').write_bytes(self.args.license.read_bytes())
  info=self.p.rpc('asset.source.inspect',dict(source=self.args.native(self.source)))
  asset=self.p.rpc('asset.import',dict(source=self.args.native(self.source),expected_model_sha256=info['model_sha256']))['asset']
  self.package=Path(str(self.world)+'.assets')/(asset+'.pmodel');meta,meshes,mips=decode_cooked(self.package.read_bytes(),self.oracle)
  self.package_pin=identity(self.package);self.poses=metric.SourcePoses(self.oracle,meta)
  self.e['original_asset']=dict(source=identity(self.source),license=identity(self.args.license),asset=asset,cooked=self.package_pin,nodes=57,takes=76,channels=8712,keys=90748,triangles=6952,vertices=7024,skin_joints=41)
  self.tx([dict(op='asset.instantiate',id=RIG,asset=asset,name='Original Knight controlled contacts')])
  self.rig=copy.deepcopy(json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig'])
  self.rig.update(clip=36,time=0,speed=1,loop=True,playing=False,ik=[dict(slot=s,root=r,middle=m,end=e,target=[0,0,0],pole=[0,1,1],space='rig',metric='near-uniform-v1',grounding=dict(original.CALIBRATION[i],reference_height=0))for i,(s,r,m,e)in enumerate(CHAINS)])
  self.tx([dict(op='entity.create',id=ACTOR,name='Actual controller owning Knight wrapper'),
   dict(op='component.set',id=ACTOR,type='Transform',value=dict(position=[0,.02,0],rotation=[0,0,0,1],scale=[1,1,1])),
   dict(op='component.set',id=ACTOR,type='CharacterController',value=dict(radius=.25,height=1.8,speed=2,jump_speed=5,camera=None)),
   dict(op='entity.reparent',id=RIG,parent=ACTOR,mode='keep_local'),
   dict(op='component.set',id=RIG,type='AnimationRig',value=self.rig),
   dict(op='entity.create',id=FLOOR,name='Static jump and landing support'),
   dict(op='component.set',id=FLOOR,type='Transform',value=dict(position=[0,-.2,0],rotation=[0,0,0,1],scale=[1,1,1])),
   dict(op='component.set',id=FLOOR,type='BoxCollider',value=dict(half_extents=[5,.2,5],motion='static',mass=1,friction=.5,restitution=0))])
  self.document=json.loads(self.world.read_bytes());self.document_contract=base.frozen_document_contract(self.document);self.retain_authored('authored-before-start')
  self.p.start(self.revision)
  if self.args.legacy_assembly:
   loaded=self.p.load(dict(self.game_config,assembly=self.args.native(self.args.legacy_assembly),type=metric.GAME))
   need(loaded['module']['type']==metric.GAME,'Explicit retained352-byte consumer')
   self.step();need(self.p.game()['module']['values']['Reads']==2,'Actual old352-byte getters did not execute')
   self.e['legacy_352']=dict(assembly=identity(self.args.legacy_assembly),committed_values=self.p.game()['module']['values'])
   self.p.rpc('runtime.stop',dict(session_id=self.p.session));self.p.start(self.revision)
  if self.game_config:
   loaded=self.p.load(self.game_config);need(loaded['module']['type']==GAME and loaded['module']['schema']['identity']==IDENTITY,'Matching compiled368-byte consumer')
  self.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=self.args.native(self.saves)))
 def controls(self):
  rows=[]
  for slot,*_ in CHAINS:
   reply=self.p.rpc('runtime.grounding',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=slot))
   need(reply['session_id']==self.p.session and reply['tick']==self.p.tick,'Exact requested-control observation boundary')
   state=reply['state'];need(type(state)is dict and set(state)=={'entity','slot','enabled','weight'} and state['entity']==RIG and type(state['slot'])is int and state['slot']==slot and type(state['enabled'])is bool and type(state['weight'])in(int,float)and not isinstance(state['weight'],bool),'Complete typed requested-control state')
   rows.append(state)
  return rows
 def requested(self,enabled,weight):
  rows=self.controls();need(all(c['enabled']is enabled and c['weight']==weight for c in rows),'Requested multiplier differs from admitted command');return rows
 def actor(self):
  return self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=ACTOR))
 def sample(self,label,ticks=1,**extra):
  result,request=self.step(ticks,**extra);local,observed,envelope=self.observe();incoming=self.poses.pose(36,0);computed,_=fk(local,self.poses.parents);source,_=fk(incoming,self.poses.parents)
  wrapper=self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=RIG))['world_matrix'];states=self.states();controls=self.controls();changed={r for _,r,m,e in CHAINS}|{m for _,r,m,e in CHAINS}|{e for _,r,m,e in CHAINS}
  for i in range(57):
   close(observed[i],multiply(wrapper,computed[i]),3e-7,'Independent complete world FK')
   for key in('position','scale'):close(local[i][key],incoming[i][key],2e-8,'Original local '+key)
   if i not in changed:
    for axis in([1,0,0],[0,1,0],[0,0,1]):close(base.rotated(local[i]['rotation'],axis),base.rotated(incoming[i]['rotation'],axis),2e-6,'Unchanged original rotation')
  floor=self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=FLOOR))['world_matrix'];top=point(floor,[0,.2,0]);up=original.unit(floor[4:7]);feet=[]
  for i,((slot,r,m,e),state,control)in enumerate(zip(CHAINS,states,controls)):
   profile=self.rig['ik'][i]['grounding'];g=state['grounding'];actual=point(multiply(wrapper,computed[e]),profile['sole_offset']);actual_normal=original.normal(multiply(wrapper,computed[e]),profile)
   close(g['input_sole'],point(multiply(wrapper,source[e]),profile['sole_offset']),3e-7,'Actual original wrapper-relative incoming sole')
   close(g['actual_sole'],actual,3e-7,'Independent full-affine controlled sole');close(g['actual_normal'],actual_normal,3e-6,'Independent full-affine controlled normal');need(g['decision_tick']==self.p.tick,'Current final-phase decision tick')
   if not control['enabled']or control['weight']==0:
    need(g['status']=='disabled'and g['weight']==0 and not g['cast']and g['hit']is None,'Disabled requested control must not probe or retain a hit')
    for n in(r,m,e):
     for axis in([1,0,0],[0,1,0],[0,0,1]):close(base.rotated(local[n]['rotation'],axis),base.rotated(incoming[n]['rotation'],axis),2e-6,'Disabled foot retained original animation')
   if g['hit']:
    hit=g['hit'];need(hit['entity']==FLOOR,'Grounding hit unrelated owner');expected_y=top[1]-(up[0]*(g['input_sole'][0]-top[0])+up[2]*(g['input_sole'][2]-top[2]))/up[1]
    close(hit['position'],[g['input_sole'][0],expected_y,g['input_sole'][2]],3e-6,'Actual current physical support plane');close(hit['normal'],up,3e-6,'Actual current physical normal')
    close([g['position_error']],[distance(actual,hit['position'])],3e-7,'Independent achieved residual');angle=math.atan2(math.hypot(*original.cross(actual_normal,hit['normal'])),max(-1,min(1,original.dot(actual_normal,hit['normal']))));close([g['normal_error']],[angle],3e-6,'Independent achieved normal error')
    if g['status']=='contact':
     close([g['weight']],[control['weight']],2e-12,'Authored one × requested contact multiplier')
     if g['weight']==1:need(g['position_error']<2e-4 and g['normal_error']<2e-4,'Full original Knight contact did not reach actual support')
   feet.append(dict(slot=slot,requested=control,state=state,independent_sole=actual,independent_normal=actual_normal))
  self.e['samples'].append(dict(label=label,tick=self.p.tick,pose_envelope=envelope,wrapper=wrapper,actor=self.actor(),feet=feet));return result,request
 def cases(self):
  self.requested(True,1);need(all(s['grounding']['casts']==0 for s in self.states()),'Getters or Initialize queried supports')
  need(self.p.rpc('runtime.grounding',dict(session_id=self.p.session,tick=self.p.tick,entity=FLOOR,slot=1))['state']is None,'Nonautomatic owner must return null')
  self.sample('physical-landing-baseline',ticks=60);need(self.actor()['ground']=='on_ground','Real controller must land before contact control tests');need(all(s['grounding']['status']=='contact'for s in self.states()),'Both original Idle feet contact floor')
  old=self.capture('identity-old-v7');need(old['value']['snapshot']['payload']['animation']['version']==7,'Default requested controls changed v7')
  before=self.controls();casts=[s['grounding']['casts']for s in self.states()]
  for _ in range(3):need(typed_equal(self.controls(),before),'Requested getter changed state')
  need([s['grounding']['casts']for s in self.states()]==casts,'Getter probed support');self.pair('getter-complete-state',old,self.capture('after-getters'))
  for patch in(dict(enabled=1),dict(weight=True),dict(weight=-.1),dict(weight=1.01),dict(slot=0),dict(extra=1)):
   malformed=commands();malformed[0].update(patch);self.step(grounding=malformed,error=-32602)
  self.step(grounding=[commands()[0],commands()[0]],error=True);self.pair('invalid-command-complete-state',old,self.capture('after-invalid-controls'))
  self.sample('caller-disabled',grounding=commands(False,.75));self.requested(False,.75);disabled=self.capture('disabled-v8');need(disabled['value']['snapshot']['payload']['animation']['version']==8,'Live control must persist in v8')
  manual=[dict(entity=RIG,slot=1,target=[0,0,0],pole=[0,0,1])];self.step(ik=manual,error=-32040);self.pair('disabled-still-owned',disabled,self.capture('after-owned-rejection'))
  result,request=self.sample('caller-half',grounding=commands(True,.5));half=self.capture('caller-half-v8')
  need(typed_equal(self.p.rpc('runtime.step',request),dict(result,replayed=True)),'Exact grounding request replay');self.pair('exact-retry-complete-state',half,self.capture('after-exact-retry'))
  self.p.rpc('runtime.grounding',dict(session_id=self.p.session,tick=self.p.tick-1,entity=RIG,slot=1),-32009);self.pair('stale-read-complete-state',half,self.capture('after-stale'))
  self.restore(half);self.requested(True,.5);self.pair('warm-v8-complete',half,self.capture('warm-v8-restored'))
  self.restore(old);self.requested(True,1);self.pair('load-v7-identity',old,self.capture('old-v7-restored'))
  if self.game_config:self.compiled()
  else:
   self.sample('caller-real-jump',grounding=commands(False,1),inputs=[dict(entity=ACTOR,jump=True)])
  need(self.actor()['velocity'][1]>0,'Real controller Jump must create positive upward velocity')
  self.sample('actual-airborne',ticks=9);need(self.actor()['ground']!='on_ground'and self.actor()['world_matrix'][13]>.1,'Actual physics must carry the Knight airborne');self.requested(False,1)
  air=self.capture('airborne-v8');self.step(100);need(self.actor()['ground']=='on_ground','Actual controller must land again');landed=self.capture('landed-disabled')
  self.restore(air);self.pair('airborne-warm-restore',air,self.capture('airborne-restored'));self.step(100);self.pair('airborne-landing-continuation',landed,self.capture('landing-replayed'))
  self.sample('landing-quarter',grounding=commands(True,.25));self.sample('landing-reenabled',grounding=commands());need(all(s['grounding']['status']=='contact'and s['grounding']['weight']==1 for s in self.states()),'Landing must regain actual contact')
  self.sample('cold-half-source',grounding=commands(True,.5));self.cold_contact=self.capture('cold-live-v8')
  self.e['checks'].extend(['Unchanged original Knight geometry/curves; actual57-node full-affine observations','Requested controls remain distinct from achieved contact; disable/fade/re-enable and no read probes','Real CharacterController Jump/airborne/landing; complete warm/v7/v8/retry/stale/ownership state pairs'])
 def compiled(self):
  self.p.edit(Mode=2);self.sample('compiled-half');self.requested(True,.5)
  self.p.edit(Mode=1);self.sample('compiled-disabled');self.requested(False,.75)
  self.p.edit(Mode=6);before=self.capture('before-compiled-manual-conflict');self.step(error=-32040);self.pair('compiled-disabled-still-owned',before,self.capture('after-compiled-manual-conflict'));self.p.edit(Mode=0)
  self.p.edit(Mode=3);before=self.capture('before-caller-managed-conflict');self.step(grounding=commands(False,1),error=-32040);self.pair('caller-compiled-control-conflict',before,self.capture('after-caller-managed-conflict'));self.p.edit(Mode=0)
  self.p.edit(Mode=4,FailAt=str(self.p.tick+1));before=self.capture('before-later-compiled-failure');self.step();self.requested(False,.25);self.restore(before);self.pair('compiled-first-tick-witness-restored',before,self.capture('first-tick-witness-restored'));error,_=self.step(ticks=2,error=-32040);need('later-tick rollback'in error['message'],'Wrong actual compiled late failure');self.pair('compiled-later-tick-complete-rollback',before,self.capture('after-later-compiled-failure'));self.p.edit(Mode=0)
  for mode,label,cause in((7,'read-1025-budget','exceeded1024 grounding reads'),(8,'duplicate-staged-slot','Duplicate gameplay grounding slot')):
   self.p.edit(Mode=mode);before=self.capture('before-compiled-'+label);error,_=self.step(error=-32040);need(cause in error['message'],'Wrong original compiled budget/duplicate failure');self.pair('compiled-'+label+'-complete-rollback',before,self.capture('after-compiled-'+label));self.p.edit(Mode=0)
  self.p.edit(Mode=5);self.sample('compiled-real-jump');need(self.p.game()['module']['values']['JumpRequests']==1,'Actual compiled Jump callback did not execute');self.requested(False,1)
  self.e['checks'].append('Compiled368-byte consumer reads/writes both legs; committed same-Tick getters; caller conflict, disabled manual conflict, later-Tick complete rollback and actual character Jump')

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in('binary','source','license','output'):ap.add_argument('--'+name,type=Path,required=True)
 for name in('hostfxr','bridge','assembly','legacy-assembly'):ap.add_argument('--'+name,type=Path)
 ap.add_argument('--timeout',type=float,default=180);args=ap.parse_args();need(20<=args.timeout<=180 and math.isfinite(args.timeout),'Finite deadline');core=any((args.hostfxr,args.bridge,args.assembly));need(not core or all((args.hostfxr,args.bridge,args.assembly)),'Supply all CoreCLR paths');need(not args.legacy_assembly or core,'Legacy consumer requires CoreCLR paths')
 args.native=lambda p:str(Path(p).resolve());args.output=args.output.resolve();need(not args.output.exists()and args.output.parent.is_dir(),'Fresh output');args.output.mkdir();started=time.monotonic();args.deadline=started+args.timeout
 e=dict(format='poima.test.runtime-grounding-control.v1',passed=False,calls=[],owners=[],checkpoints=[],whole_pairs=[],checks=[],samples=[],limitations=['Original licensed geometry and curves preserved; rotations of the configured chains intentionally change.','Actual CPU physics/complete native saves; no rendered gait, locking, pelvis, physical input or performance claim.','Original checkout/source remain readable; EOF0 native leaders need external process-tree ownership.','Optional compiled game must be supplied already built against the matched368-byte SDK; this fixture compiles nothing.'])
 w=None;status=1
 try:
  paths={Path(__file__).resolve(),Path(original.__file__).resolve(),Path(metric.__file__).resolve(),Path(base.__file__).resolve(),args.binary.resolve(),args.source.resolve(),args.license.resolve()}
  paths.update(ROOT/'tests'/name for name in('character_library_intake.py','imported_character_contract.py','input_actions_gameplay_contract.py','components_gameplay_contract.py','character_gameplay_contract.py','animation_fixture.py','gltf_fixture.py','scene_capture.py','switchback_game_service.py','switchback_oracle.py'))
  paths.update(p for p in(ROOT/'tools/python/poima_client').rglob('*')if p.is_file()and p.suffix in('.py','.json'))
  if core:
   paths.update((args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve(),ROOT/'tests/managed_grounding_control_gameplay/ManagedGroundingControlGame.cs',ROOT/'tests/managed_grounding_control_gameplay/Poima.ManagedGroundingControlGame.csproj',ROOT/'managed/Directory.Build.props'))
   for image in filter(None,(args.bridge,args.assembly,args.legacy_assembly)):
    image=image.resolve();paths.update((image,image.parent/'Poima.Gameplay.dll'))
    for suffix in('.deps.json','.runtimeconfig.json'):
     if image.with_suffix(suffix).is_file():paths.add(image.with_suffix(suffix))
  e['input_pins']={str(p):identity(p)for p in sorted(paths)};w=Workflow(args,e)
  if core:w.game_config=dict(hostfxr=args.native(args.hostfxr),bridge=args.native(args.bridge),assembly=args.native(args.assembly),type=GAME)
  w.prepare();w.cases();source=w.cold_contact;w.step(2);future=w.capture('cold-expected');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.p=base.RawSession(args,w.world,e);w.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=args.native(w.saves)));w.restore(source,cold=True);w.requested(True,.5);w.pair('cold-live-v8-complete',source,w.capture('cold-restored'));w.step(2);w.pair('cold-live-v8-continuation',future,w.capture('cold-future'));w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.guard_authored();need(identity(w.package)==w.package_pin and all(identity(p)==pin for p,pin in e['input_pins'].items()),'Immutable source/package/helper inputs changed');base.census(args.output);need(time.monotonic()<args.deadline,'Whole deadline');e['passed']=True;status=0
 except BaseException:e['error']=traceback.format_exc()
 finally:
  if w and w.p:
   try:w.p.close()
   except BaseException:e['closure_error']=traceback.format_exc();e['passed']=False;status=1
  e['seconds']=time.monotonic()-started;e['rpc_count']=len(e['calls']);e['output_before_report']=base.census(args.output)
  if time.monotonic()>=args.deadline:e['passed']=False;e['deadline_exhausted']=True;status=1
  data=(json.dumps(e,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=8*1024**2,'Report8MiB reserve');pending=args.output/'report.pending.json';pending.write_bytes(data);base.census(args.output);pending.rename(args.output/'report.json')
 print(json.dumps({k:v for k,v in e.items()if k in('passed','rpc_count','seconds','error','closure_error')}));return status
if __name__=='__main__':raise SystemExit(main())
