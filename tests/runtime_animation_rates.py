"""Opt-in original Knight animation-rate contract; explicit compiled inputs required.

Runs two headless native owners and preserves complete warm/cold save operands.
Builds nothing. An external process owner must bound descendant lifetime.
"""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import argparse,copy,hashlib,json,math,sys,time,traceback
ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True;sys.path.insert(0,str(ROOT/'tests'))
import runtime_ik_metric_profile as metric
import runtime_ground_contacts as contacts
from animation_fixture import ribbon
from gltf_fixture import glb
from character_library_intake import LibraryOracle,decode_cooked,SOURCE_SHA,LICENSE_SHA
from imported_character_contract import multiply
base=metric.base;need=base.need;identity=base.identity;typed=base.typed_equal
RIG=base.RIG;ACTOR=base.uid(920);FLOOR=base.uid(910);CAMERA=base.uid(930)
CAP=256*1024**2
PAYLOAD_KEYS={'content_sha256','world_id','authored_revision','tick','gameplay_revision','entities','animation','sound','gameplay','components','structure','ui','control_sequence','cameras'}
GAME='Poima.Tests.ManagedAnimationRateGame';MODULE='poima.test.managed-animation-rate'
RIGS=[base.uid(400+i)for i in range(13)]
HELPERS=('runtime_ik_metric_profile.py','runtime_ground_contacts.py','runtime_two_bone_ik_orientation.py','character_library_intake.py','imported_character_contract.py','input_actions_gameplay_contract.py','components_gameplay_contract.py','character_gameplay_contract.py','animation_fixture.py','gltf_fixture.py','scene_capture.py','switchback_game_service.py','switchback_oracle.py')

def rate(speed=1.,entity=RIG,layer=None):
 row=dict(entity=entity,speed=speed)
 if layer is not None:row['layer']=layer
 return row

def full(clip=36,entity=RIG,layer=None,blend=0,**extra):
 row=dict(entity=entity,clip=clip,time=0,speed=1,loop=True,playing=True,blend_ticks=blend,transition_mode='crossfade')
 if layer is not None:row.update(layer=layer,weight=.25,weight_blend_ticks=blend)
 row.update(extra);return row

def clock(capture,entity=RIG,layer=None):
 animation=capture['value']['snapshot']['payload']['animation'];rows=[r for r in animation['rigs']if r['entity']==entity];need(len(rows)==1,'Unique full saved rig')
 if layer is None:return rows[0]
 layers=[r for r in rows[0]['layers']if r['slot']==layer];need(len(layers)==1,'Unique full saved layer');return layers[0]

def cohort_profile(oracle,helper):
 original_channels=sum(len(c['channels'])for c in oracle.clips);original_keys=sum(len(v['times'])for c in oracle.clips for v in c['channels'])
 helper_channels=sum(len(c['channels'])for c in helper['animations']);helper_keys=sum(helper['accessors'][c['samplers'][v['sampler']]['input']]['count']for c in helper['animations']for v in c['channels'])
 need((original_channels,original_keys,helper_channels,helper_keys)==(8712,90748,1,2),'Complete original and unchanged standard source curve counts')
 profile=dict(original_rigs=1,standard_rigs=12,original_nodes=len(oracle.nodes),standard_nodes=len(helper['nodes']),instanced_channels=original_channels+12*helper_channels,unique_compiled_keys=original_keys+helper_keys,instance_summed_keys=original_keys+12*helper_keys,mapped_nodes=len(oracle.nodes)+12*len(helper['nodes']))
 need(profile['instanced_channels']<=65536 and profile['unique_compiled_keys']<=2000000 and profile['mapped_nodes']<=10000 and 4*profile['mapped_nodes']<=20000,'Exact existing native aggregate animation bounds before world admission')
 return profile

def input_paths(args):
 paths={Path(__file__).resolve(),args.binary,args.source,args.license,args.hostfxr,args.bridge,args.assembly,ROOT/'tests/managed_animation_rate_gameplay/ManagedAnimationRateGame.cs',ROOT/'tests/managed_animation_rate_gameplay/Poima.ManagedAnimationRateGame.csproj',ROOT/'managed/Directory.Build.props'}
 paths.update(ROOT/'tests'/name for name in HELPERS)
 paths.update(p for p in(ROOT/'tools/python/poima_client').iterdir()if p.is_file()and p.suffix in('.py','.json'))
 for image in(args.bridge,args.assembly):
  paths.add(image.parent/'Poima.Gameplay.dll')
  for suffix in('.deps.json','.runtimeconfig.json'):
   if image.with_suffix(suffix).is_file():paths.add(image.with_suffix(suffix))
 return paths

class Workflow(metric.Workflow):
 def prepare(self):
  raw=self.args.source.read_bytes();self.oracle=LibraryOracle(raw);need(hashlib.sha256(self.args.license.read_bytes()).hexdigest()==LICENSE_SHA,'Original CC0 license')
  need(self.oracle.clips[36]['name']=='Idle'and self.oracle.clips[48]['name']=='Running_A','Exact original take selection')
  self.source=self.args.output/'original-Knight.glb';self.source.write_bytes(raw);(self.args.output/'original-LICENSE.txt').write_bytes(self.args.license.read_bytes())
  inspected=self.p.rpc('asset.source.inspect',dict(source=self.args.native(self.source)))
  asset=self.p.rpc('asset.import',dict(source=self.args.native(self.source),expected_model_sha256=inspected['model_sha256']))['asset']
  self.package=Path(str(self.world)+'.assets')/(asset+'.pmodel');meta,meshes,mips=decode_cooked(self.package.read_bytes(),self.oracle);self.package_pin=identity(self.package);self.poses=metric.SourcePoses(self.oracle,meta)
  self.e['original_asset']=dict(source=identity(self.source),license=identity(self.args.license),asset=asset,cooked=self.package_pin,nodes=57,takes=76,channels=8712,keys=90748,triangles=6952,vertices=7024,skin_joints=41)
  # Published original analytic ribbon supplies quota clocks without duplicating
  # the complete Knight's8,712 animation channels13times beyond the world cap.
  helper_doc,helper_blob=ribbon();need(len(helper_doc['nodes'])==3 and len(helper_doc['animations'])==1 and len(helper_doc['animations'][0]['channels'])==1,'Unchanged standard ribbon source shape')
  self.e['source_cohort']=cohort_profile(self.oracle,helper_doc)
  helper_source=self.args.output/'standard-ribbon.glb';helper_source.write_bytes(glb(helper_doc,helper_blob))
  helper_inspected=self.p.rpc('asset.source.inspect',dict(source=self.args.native(helper_source)))
  helper_asset=self.p.rpc('asset.import',dict(source=self.args.native(helper_source),expected_model_sha256=helper_inspected['model_sha256']))['asset']
  self.helper_package=Path(str(self.world)+'.assets')/(helper_asset+'.pmodel');self.helper_package_pin=identity(self.helper_package);self.helper_source=helper_source;self.helper_source_pin=identity(helper_source)
  self.e['quota_asset']=dict(source=self.helper_source_pin,cooked=self.helper_package_pin,asset=helper_asset,fixture='tests/animation_fixture.py:ribbon via tests/gltf_fixture.py:glb',nodes=3,takes=1,channels=1,keys=2,instances=12)
  self.tx([dict(op='asset.instantiate',id=r,asset=asset if i==0 else helper_asset,name='Original Knight rate clock' if i==0 else 'Standard ribbon quota clock '+str(i))for i,r in enumerate(RIGS)])
  doc=json.loads(self.world.read_bytes());ops=[]
  for r in RIGS:
   knight=r==RIG
   rig=copy.deepcopy(doc['entities'][r]['components']['AnimationRig']);rig.update(clip=36 if knight else 0,time=0,speed=1,loop=True,playing=True)
   rig['layers']=[dict(slot=s,mode='override',clip=48 if knight else 0,time=0,speed=1,loop=True,playing=True,weight=0,mask=[dict(node=10 if knight else 1,weight=1)])for s in range(1,5)]
   # Disabled authored automatic slots keep a genuine same-binding v7 baseline;
   # they do not turn this playback-rate fixture into a grounding qualification.
   if knight:rig['ik']=[dict(slot=s,root=a,middle=b,end=c,target=[0,0,0],pole=[0,1,1],space='rig',enabled=False,metric='near-uniform-v1',grounding=dict(contacts.CALIBRATION[i],reference_height=0))for i,(s,a,b,c)in enumerate(metric.CHAINS)]
   ops.append(dict(op='component.set',id=r,type='AnimationRig',value=rig))
  ops += [dict(op='entity.create',id=ACTOR,name='Actual moving rate controller'),dict(op='component.set',id=ACTOR,type='Transform',value=dict(position=[0,.02,0],rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=ACTOR,type='CharacterController',value=dict(radius=.25,height=1.8,speed=4,jump_speed=5,camera=None)),dict(op='entity.reparent',id=RIG,parent=ACTOR,mode='keep_local'),dict(op='entity.create',id=FLOOR,name='Static rate controller floor'),dict(op='component.set',id=FLOOR,type='Transform',value=dict(position=[0,-.2,0],rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=FLOOR,type='BoxCollider',value=dict(half_extents=[50,.2,50],motion='static',mass=1,friction=.5,restitution=0))]
  ops += [dict(op='entity.create',id=CAMERA,name='Frozen camera save coverage'),dict(op='component.set',id=CAMERA,type='Transform',value=dict(position=[0,2,4],rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=CAMERA,type='Camera',value=dict(vertical_fov=70,near=.1,far=100))]
  self.tx(ops);self.document=json.loads(self.world.read_bytes());self.document_contract=base.frozen_document_contract(self.document)
  for r in RIGS:need(all(base.node_id(r,n)in self.document['entities']for n in range(57 if r==RIG else 3)),'Exact Knight57/standard ribbon3-node membership')
  self.e['rig_membership']=[dict(entity=r,asset=asset if r==RIG else helper_asset,node_ids=[base.node_id(r,n)for n in range(57 if r==RIG else 3)])for r in RIGS]
  need(len(self.document['entities'])==136 and sum('RigNode'in v['components']for v in self.document['entities'].values())==93,'Complete mixed authored136 entities/93 mapped nodes including primitive children')
  self.retain_authored('authored-before-start');self.p.start(self.revision)
  loaded=self.p.load(self.game_config);need(loaded['module']['backend']=='coreclr'and loaded['module']['type']==GAME and loaded['module']['schema']['identity']==MODULE and loaded['module']['assembly_sha256']==identity(self.args.assembly)['sha256'],'Exact actual CoreCLR consumer')
  self.module_schema=loaded['module']['schema']
  need(len(self.module_schema['fields'])==26 and {'PreviousPositionValid','PreviousSampleTick','PreviousX','PreviousZ'}<={f['name']for f in self.module_schema['fields']},'Actual corrected consumer26 fields including all saved committed-position history')
  self.p.edit(IdleClip=36,RunClip=48,Mode=0,Drive=0,AutoRate=0,BlendTicks=120,TransitionMode=1,Move=.375,NominalSpeed=3.,RequestedSpeed=1.)
  self.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=self.args.native(self.saves)))
 def capture(self,label):
  result=super().capture(label);snapshot=result['value']['snapshot'];payload=snapshot['payload']
  need(snapshot['version']==7 and set(payload)==PAYLOAD_KEYS,'Complete current camera-bearing snapshot14 members')
  entities=payload['entities'];need(len(entities)==len(self.document['entities'])and {e['id']for e in entities}==set(self.document['entities']),'Complete exact authored/native entity membership')
  rows=payload['animation']['rigs'];need(len(rows)==13 and {r['entity']for r in rows}==set(RIGS)and all(len(r['layers'])==4 and {l['slot']for l in r['layers']}=={1,2,3,4}for r in rows),'Complete13rig/65clock saved membership')
  need(typed(payload['gameplay']['schema'],self.module_schema)and set(payload['gameplay']['values'])=={f['name']for f in self.module_schema['fields']},'Every compiled schema field/value preserved')
  return result
 def restore(self,capture,cold=False):
  result=super().restore(capture,cold);need(result['source_stale']is False and result['recovered']is False,'Exact original source restore, no recovery branch');return result
 def animation(self,entity=RIG):return self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=entity))['animation']
 def actor(self):return self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=ACTOR))
 def observe_pose(self,label,pure_idle=False):
  local,world,envelope=self.observe();matrices,_=metric.fk(local,self.poses.parents);wrapper=self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=RIG))['world_matrix']
  for n in range(57):metric.close(world[n],multiply(wrapper,matrices[n]),3e-7,'Independent complete57-node FK')
  if pure_idle:
   state=self.animation();expected=self.poses.pose(36,state['time'])
   for a,b in zip(local,expected):
    for k in('position','scale'):metric.close(a[k],b[k],3e-7,'Original playing Idle '+k)
    for axis in([1,0,0],[0,1,0],[0,0,1]):metric.close(base.rotated(a['rotation'],axis),base.rotated(b['rotation'],axis),3e-6,'Original playing Idle rotation')
  self.e['samples'].append(dict(label=label,tick=self.p.tick,envelope=envelope,wrapper=wrapper,local=local,world=world,animation=self.animation(),actor=self.actor()))
 def unchanged_fade(self,label,before,after,layer=None):
  a,b=clock(before,layer=layer),clock(after,layer=layer)
  need(a['transition']is not None and typed(a['control'],b['control'])and a['anchor_tick']==b['anchor_tick']and typed(a['transition'],b['transition']),'Retiming replaced immutable original fade/control')
  if layer is not None:need(typed(a['weight_transition'],b['weight_transition'])and a['target_weight']==b['target_weight'],'Layer rate restarted independent weight ramp')
  self.e['fade_witnesses'].append(dict(label=label,before=before['slot'],after=after['slot'],layer=layer))
 def fail(self,label,mode=None,**extra):
  if mode is not None:self.p.edit(Mode=mode,RequestedSpeed=1.75)
  before=self.capture('before-'+label);error,_=self.step(error=-32040,**extra);after=self.capture('after-'+label);self.pair(label,before,after);self.e['rejections'].append(dict(label=label,error=error))
  if mode is not None:self.p.edit(Mode=0)
 def cases(self):
  self.step(60);need(self.actor()['ground']=='on_ground','Actual initial controller landing');self.observe_pose('original-playing-idle',True)
  baseline=self.capture('same-binding-v7');need(baseline['value']['snapshot']['payload']['animation']['version']==7,'Genuine untouched-rate v7 profile')
  for row in (dict(entity=RIG),dict(entity=RIG,speed=True),rate(-1),rate(8.1),rate(1,layer=0),rate(1,layer=5),dict(rate(1),layer=None),dict(rate(1),layer=1.0),dict(rate(1),extra=1)):
   self.step(animation_rates=[row],error=-32602)
  self.step(animation_rates=[rate(2),rate(.5)],error=-32602);self.step(animations=[full()],animation_rates=[rate(2)],error=-32602)
  self.pair('malformed-and-duplicate-whole-rollback',baseline,self.capture('after-json-rejections'))
  result,request=self.step(animation_rates=[rate(.5),rate(.75,layer=1)]);retry=self.capture('json-rate-applied')
  need(typed(self.p.rpc('runtime.step',request),dict(result,replayed=True)),'Exact original step retry');self.pair('exact-retry-no-second-advance',retry,self.capture('after-json-retry'))
  bad=dict(request,request_id=base.fresh(),expected_tick=self.p.tick-1);self.p.rpc('runtime.step',bad,-32009);self.pair('stale-whole-rollback',retry,self.capture('after-stale'))
  targets=[(r,s)for r in RIGS for s in(None,1,2,3,4)];rows=[rate(3,r,s)for r,s in targets[1:]];need(len(rows)==64,'Exactly65 available configured clocks')
  self.step(animations=[full()],animation_rates=rows,error=-32602);self.pair('combined65-valid-clocks-rejected',retry,self.capture('after-union65'))
  self.step(animations=[full()],animation_rates=rows[:-1]);observed={}
  for r in RIGS:
   a=self.animation(r);observed[(r,None)]=a['speed']
   for l in a['layers']:observed[(r,l['slot'])]=l['playback']['speed']
  need(observed[targets[0]]==1 and all(observed[t]==3 for t in targets[1:-1])and observed[targets[-1]]==1,'Actual full+63rate exact64 union')
  self.e['quota']=dict(accepted_full=1,accepted_rates=63,rejected_full=1,rejected_rates=64,configured_clocks=65,actual_speeds=[dict(entity=r,layer=s,speed=observed[(r,s)])for r,s in targets])
  self.restore(baseline);self.pair('warm-original-v7',baseline,self.capture('v7-restored'))
  start=self.actor();self.p.edit(Drive=1,AutoRate=1,Mode=2,Move=.375,NominalSpeed=3.,TransitionMode=1,BlendTicks=120);self.step();fade_start=self.capture('compiled-run-fade-start');self.step(8);auto=self.capture('compiled-velocity-rate')
  values=self.p.game()['module']['values'];actual=self.animation();need(values['ObservedMoving']>0 and values['BaseWrites']>0 and values['DuringFadeWrites']>0 and values['StagedReads']==values['BaseWrites']+values['LayerWrites'],'Actual committed/staged compiled rate callbacks')
  need(self.actor()['world_matrix'][12]>start['world_matrix'][12]+.01 and values['ObservedVelocity']>.01,'Real controller displacement/velocity, not clip-only motion');need(abs(actual['speed']-max(0,min(8,values['ObservedVelocity']/values['NominalSpeed'])))<.011,'Actual velocity-derived rate')
  self.unchanged_fade('actual-velocity-during-original-fade',fade_start,auto);self.observe_pose('actual-moving-run-retimed')
  self.p.edit(AutoRate=0,Mode=3,RequestedSpeed=2.);before=self.capture('before-managed-base');self.step();after=self.capture('after-managed-base');self.unchanged_fade('compiled-base-speed',before,after);need(self.animation()['speed']==2.,'Compiled base rate')
  self.step(animations=[full(48,layer=1,blend=120,time=.1,weight=.6)]);self.p.edit(Mode=4,RequestedSpeed=.25);before=self.capture('before-managed-layer');self.step();after=self.capture('after-managed-layer');self.unchanged_fade('compiled-layer-speed',before,after,1);need(self.animation()['layers'][0]['playback']['speed']==.25,'Compiled configured layer rate')
  self.p.edit(Mode=3,RequestedSpeed=0.);phase=self.animation()['time'];self.step(2);need(self.animation()['time']==phase and self.animation()['playing']is True,'Zero rate freezes phase, not Playing');self.step(2);need(self.animation()['time']==phase,'Zero rate phase advanced')
  self.p.edit(Mode=3,RequestedSpeed=1.25);self.step(2);need(self.animation()['speed']==1.25 and self.animation()['time']!=phase,'Positive rate resumes an already playing clock');self.observe_pose('compiled-base-layer-live')
  source=self.capture('active-v9-cold-source');need(source['value']['snapshot']['payload']['animation']['version']==9 and source['value']['snapshot']['payload']['animation']['legacy_version']==7,'Activev9 over same frozen v7 bindings');need(clock(source)['transition']is not None,'Cold source must retain actual activefade')
  for mode,label in((5,'compiled-duplicate'),(6,'compiled-full-rate-conflict')):self.fail(label,mode=mode)
  self.fail('caller-compiled-same-clock',mode=3,animation_rates=[rate(.75)])
  self.p.edit(Mode=3,RequestedSpeed=.8,FailAt=str(self.p.tick+1));before=self.capture('before-later-tick');self.step();witness=self.capture('first-tick-accepted-rate');need(self.animation()['speed']==.8,'First Tick rate witness');self.restore(before);self.pair('first-tick-witness-restored',before,self.capture('first-tick-restored'));error,_=self.step(2,error=-32040);need('Animation rate later-Tick rollback fixture'in error['message'],'Exact actual later-Tick failure');self.pair('later-tick-complete-rollback',before,self.capture('after-later-tick'));self.p.edit(Mode=0,FailAt='-1')
  self.restore(baseline);self.pair('load-v7-clears-all-segments',baseline,self.capture('legacy-v7-again'));self.step(2);old_future=self.capture('legacy-v7-future');self.restore(baseline);self.step(2);self.pair('legacy-v7-whole-continuation',old_future,self.capture('legacy-v7-repeated-future'))
  self.restore(source);self.pair('warm-active-v9-full',source,self.capture('warm-active-v9-restored'));self.step(2);future=self.capture('active-v9-expected-future');self.e['prospective_operands']=dict(source=source['slot'],source_identity=source['identity'],complete_future=future['slot'],future_identity=future['identity'],declared_before_cold_owner_ns=time.monotonic_ns(),scope='Full original warm future bytes exist before cold counterpart starts; no epoch/field rewrite or type coercion.');return source,future

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in('binary','source','license','hostfxr','bridge','assembly','output'):ap.add_argument('--'+name,type=Path,required=True)
 ap.add_argument('--timeout',type=float,default=180);args=ap.parse_args();need(math.isfinite(args.timeout)and 30<=args.timeout<=180,'Finite protocol timeout')
 for name in('binary','source','license','hostfxr','bridge','assembly'):setattr(args,name,getattr(args,name).resolve(strict=True))
 base.CAP=CAP # New201 prospective cap only; imported helper source and all old cohort limits remain unchanged.
 args.native=lambda p:str(Path(p).resolve());args.output=args.output.resolve();need(not args.output.exists()and args.output.parent.is_dir(),'Fresh protocol output');args.output.mkdir();started=time.monotonic();args.deadline=started+args.timeout-10
 e=dict(format='poima.animation-rate-contract.v1',passed=False,calls=[],owners=[],checkpoints=[],whole_pairs=[],checks=[],samples=[],fade_witnesses=[],rejections=[],limits=dict(seconds=args.timeout,rpc=700,bytes=CAP,files=512),scope='Actual CPU one complete original Knight base/layer playback rate during live fades plus moving controller; twelve unchanged standard ribbon rigs add quota clocks; complete saves; no renderer, gait, contacts, velocity-continuity, NativeAOT, Windows or performance qualification.')
 w=None;status=1
 try:
  e['input_pins']={str(p):identity(p)for p in sorted(input_paths(args))};need(e['input_pins'][str(args.source)]['sha256']==SOURCE_SHA,'Exact original licensed source')
  w=Workflow(args,e);w.game_config=dict(hostfxr=str(args.hostfxr),bridge=str(args.bridge),assembly=str(args.assembly),type=GAME);w.prepare();source,future=w.cases();w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.p=base.RawSession(args,w.world,e);w.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=str(w.saves)));w.restore(source,cold=True);w.pair('cold-active-v9-all-state',source,w.capture('cold-active-v9-restored'));w.step(2);w.pair('cold-active-v9-complete-future',future,w.capture('cold-active-v9-future'));w.observe_pose('cold-original-knight');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.guard_authored();need(identity(w.package)==w.package_pin and identity(w.helper_package)==w.helper_package_pin and identity(w.helper_source)==w.helper_source_pin and all(identity(p)==i for p,i in e['input_pins'].items()),'Immutable original/standard source/cooked/closure inputs changed');need(len(e['owners'])==2 and all(o['exit_code']==0 and not o['forced_cleanup']and o['surplus_stdout_lines']==0 for o in e['owners']),'Two actual native leaders EOF0; external owner must establish whole descendant closure');need(time.monotonic()<args.deadline,'Whole protocol work deadline');e['passed']=True;status=0
 except BaseException:e['error']=traceback.format_exc()
 finally:
  if w and w.p:
   try:w.p.close()
   except BaseException:e['closure_error']=traceback.format_exc();e['passed']=False;status=1
  e['seconds']=time.monotonic()-started;e['rpc_count']=len(e['calls']);e['output_before_report']=base.census(args.output)
  if e['seconds']>=args.timeout:e['passed']=False;e['deadline_exhausted']=True;status=1
  data=(json.dumps(e,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=8*1024**2,'Report8MiB reserve');pending=args.output/'report.pending.json';pending.write_bytes(data);base.census(args.output);pending.rename(args.output/'report.json')
 print(json.dumps({k:v for k,v in e.items()if k in('passed','rpc_count','seconds','error','closure_error')}));return status
if __name__=='__main__':raise SystemExit(main())
