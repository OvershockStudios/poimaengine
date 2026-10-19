#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original licensed Knight, actual ground contacts, full affine poses and saves.

Builds/downloads nothing. Run under an owned process-tree supervisor. This is
CPU geometry/state evidence, not a rendered gait or foot-locking assessment.
"""
import argparse,copy,hashlib,json,math,time,traceback
from pathlib import Path
import runtime_ik_metric_profile as metric
from runtime_ik_metric_profile import base,need,identity,typed_equal,CHAINS,RIG,fk,close,distance
from character_library_intake import LibraryOracle,decode_cooked,LICENSE_SHA
from imported_character_contract import point
from components_gameplay_contract import uid

ROOT=Path(__file__).resolve().parents[1]
FLOOR=uid(910);DYNAMIC=uid(911)
# Explicit sole calibration from the original rest geometry, not generated art.
CALIBRATION=[
 dict(sole_offset=[3.518225173502198e-9,.17351426574300935,.02932634479185336],sole_up=[6.943410503710568e-8,-.7198726005913565,-.6941062158760954],sole_forward=[1.3696781462479196e-7,.6941062158760974,-.719872600591345]),
 dict(sole_offset=[-4.364844294028667e-09, 0.17351426329625338, 0.0293263467672322],sole_up=[1.021392211036242e-07, -0.7198726005913618, -0.6941062158760859],sole_forward=[1.054333437946812e-07, 0.6941062158760934, -0.7198726005913542])]

def cross(a,b):return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def unit(v):
 n=math.hypot(*v);need(n>0 and math.isfinite(n),'Finite independent normal');return [x/n for x in v]
def vector(m,v):return [sum(m[4*c+r]*v[c]for c in range(3))for r in range(3)]
def normal(m,g):return unit(cross(vector(m,g['sole_forward']),vector(m,cross(g['sole_up'],g['sole_forward']))))

class Workflow(metric.Workflow):
 def prepare(self):
  original=self.args.source.read_bytes();self.oracle=LibraryOracle(original)
  need(hashlib.sha256(self.args.license.read_bytes()).hexdigest()==LICENSE_SHA,'Exact original CC0 license')
  self.source=self.args.output/'original-Knight.glb';self.source.write_bytes(original)
  info=self.p.rpc('asset.source.inspect',dict(source=self.args.native(self.source)))
  asset=self.p.rpc('asset.import',dict(source=self.args.native(self.source),expected_model_sha256=info['model_sha256']))['asset']
  self.package=Path(str(self.world)+'.assets')/(asset+'.pmodel');meta,meshes,mips=decode_cooked(self.package.read_bytes(),self.oracle);self.package_pin=identity(self.package);self.poses=metric.SourcePoses(self.oracle,meta)
  self.e['original_asset']=dict(source=identity(self.source),license=identity(self.args.license),asset=asset,cooked=self.package_pin,nodes=57,takes=76,channels=8712,keys=90748,triangles=6952,vertices=7024,skin_joints=41)
  self.tx([dict(op='asset.instantiate',id=RIG,asset=asset,name='Original Knight ground contacts')])
  self.rig=copy.deepcopy(json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig'])
  self.rig.update(clip=36,time=0,speed=1,loop=True,playing=False,ik=[dict(slot=s,root=r,middle=m,end=e,target=[0,0,0],pole=[0,1,1],space='rig',metric='near-uniform-v1',grounding=dict(CALIBRATION[i],reference_height=0))for i,(s,r,m,e)in enumerate(CHAINS)])
  ops=[dict(op='component.set',id=RIG,type='AnimationRig',value=self.rig)]
  for ident,name,position,half,motion in((FLOOR,'Kinematic support',[0,-.15,0],[5,.2,5],'kinematic'),(DYNAMIC,'Excluded dynamic cover',[0,.16,0],[1,.04,1],'dynamic')):
   ops.extend([dict(op='entity.create',id=ident,name=name),dict(op='component.set',id=ident,type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=ident,type='BoxCollider',value=dict(half_extents=half,motion=motion,mass=1,friction=.5,restitution=0))])
  self.tx(ops)
  original=self.world.read_bytes()
  for patch in(dict(plant_height=.2,lift_height=.1),dict(sole_up=[0,0,0]),dict(probe_above=True),dict(max_slope_degrees=81)):
   bad=copy.deepcopy(self.rig);bad['ik'][0]['grounding'].update(patch);self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=bad)],-32602);need(self.world.read_bytes()==original,'Malformed grounding changed authored bytes')
  self.document=json.loads(original);self.document_contract=base.frozen_document_contract(self.document);self.retain_authored('authored-before-start')
  self.p.start(self.revision)
  if self.game_config:
   loaded=self.p.load(self.game_config);need(loaded['module']['type']==metric.GAME,'Pinned compiled original-leg consumer')
  self.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=self.args.native(self.saves)))
 def states(self):
  return [self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=s,include_grounding=True,include_metric=True,include_end_orientation=True))['state']for s,*_ in CHAINS]
 def sample_contacts(self,clip,at,label,**extra):
  result,request=self.step(animations=[dict(entity=RIG,clip=clip,time=at,speed=1,loop=False,playing=False)],**extra)
  local,observed,envelope=self.observe();incoming=self.poses.pose(clip,at);computed,_=fk(local,self.poses.parents);source,_=fk(incoming,self.poses.parents)
  for i in range(57):
   close(observed[i],computed[i],2e-7,'Independent complete FK')
   for key in('position','scale'):close(local[i][key],incoming[i][key],2e-8,'Original local '+key)
  states=self.states();floor=self.p.rpc('runtime.entity',dict(session_id=self.p.session,id=FLOOR,tick=self.p.tick))['world_matrix'];up=unit(floor[4:7]);top=point(floor,[0,.2,0]);rows=[]
  for i,((slot,r,m,e),state)in enumerate(zip(CHAINS,states)):
   g=state['grounding'];definition=self.rig['ik'][i]['grounding'];expected_input=point(source[e],definition['sole_offset']);close(g['input_sole'],expected_input,3e-7,'Prepared original sole');need(g['decision_tick']==self.p.tick,'Current postphysics contact tick')
   actual=point(computed[e],definition['sole_offset']);actual_normal=normal(computed[e],definition)
   close(g['actual_sole'],actual,3e-7,'Independent full-affine sole');close(g['actual_normal'],actual_normal,3e-6,'Independent affine surface normal')
   if expected_input[1]>=.15:
    need(g['status']=='lifted'and not g['cast']and not state['enabled'],'Raised source foot must retain animation')
    for n in(r,m,e):
     for axis in([1,0,0],[0,1,0],[0,0,1]):close(base.rotated(local[n]['rotation'],axis),base.rotated(incoming[n]['rotation'],axis),2e-6,'Raised limb retained source rotation')
   if g['hit']:
    hit=g['hit'];need(hit['entity']==FLOOR,'Dynamic cover must not hide support')
    expected_y=top[1]-(up[0]*(expected_input[0]-top[0])+up[2]*(expected_input[2]-top[2]))/up[1]
    close(hit['position'],[expected_input[0],expected_y,expected_input[2]],3e-6,'Current physical support plane');close(hit['normal'],up,3e-6,'Current physical support normal')
    close([g['position_error']],[distance(actual,hit['position'])],3e-7,'Independent sole residual')
    angle=math.atan2(math.hypot(*cross(actual_normal,hit['normal'])),max(-1,min(1,dot(actual_normal,hit['normal']))));close([g['normal_error']],[angle],3e-6,'Independent normal residual')
    if g['status']=='contact'and g['weight']==1:
     need(g['position_error']<2e-4 and g['normal_error']<2e-4,'Full imported contact must actually reach support')
   rows.append(dict(slot=slot,state=state,independent_sole=actual,independent_normal=actual_normal))
  self.e['samples'].append(dict(label=label,clip=clip,time=at,tick=self.p.tick,pose_envelope=envelope,feet=rows));return result,request
 def cases(self):
  need(all(s['grounding']['status']=='uninitialized'and s['grounding']['casts']==0 for s in self.states()),'Initialization must not probe')
  default=self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=1))['state'];need('grounding'not in default,'Default shape preserved')
  self.sample_contacts(36,0,'flat-idle');need(all(s['grounding']['status']=='contact'for s in self.states()),'Both Idle feet contact real floor')
  before=self.capture('before-reads');original=self.states()
  for _ in range(3):need(typed_equal(self.states(),original),'Repeated inspection mutated contact state')
  self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=1,include_grounding=1),-32602)
  self.pair('all-state-read-purity',before,self.capture('after-reads'))
  command=dict(entity=RIG,slot=1,target=[0,0,0],pole=[0,0,1]);self.step(2,ik=[command],error=-32040);self.pair('owned-slot-full-rollback',before,self.capture('after-rejected-manual'))
  if self.game_config:
   self.p.edit(Mode=1);managed=self.capture('before-compiled-owned-slot');error,_=self.step(error=-32040);need('grounding'in error['message'].lower(),'Compiled setter must report automatic ownership');self.pair('compiled-owned-slot-full-rollback',managed,self.capture('after-compiled-owned-slot'));self.p.edit(Mode=0)
  target=dict(entity=FLOOR,position=[0,-.10,0],rotation=[0,0,math.sin(math.radians(10)/2),math.cos(math.radians(10)/2)],duration_ticks=4)
  for i in range(4):self.sample_contacts(36,.2,'moving-slope-'+str(i),**(dict(motions=[target])if i==0 else {}))
  need(all(s['grounding']['status']=='contact'for s in self.states()),'Both Idle feet conform to moved slope')
  warm=self.capture('warm-contact');self.cold_contact=warm;self.sample_contacts(48,0,'run-detour');self.restore(warm);self.pair('complete-warm-no-probe',warm,self.capture('warm-restored'))
  for fraction in(0,.125,.25,.375,.5,.625,.75,.875):self.sample_contacts(48,self.oracle.clips[48]['duration']*fraction,'running-'+str(fraction))
  need(sum(f['state']['grounding']['status']=='lifted'for s in self.e['samples']for f in s['feet'])>=2,'Actual running swing must survive')
  target=dict(entity=FLOOR,position=[0,-2,0],rotation=[0,0,0,1],duration_ticks=2)
  self.sample_contacts(36,0,'support-moving-away',motions=[target]);self.sample_contacts(36,0,'support-miss')
  need(all(s['grounding']['status']=='miss'and s['grounding']['hit']is None and not s['enabled']for s in self.states()),'Miss clears previous support and correction')
  self.e['checks'].extend(['Original geometry,8712channels/90748keys retained; all57node affine observations','Actual postphysics moving/sloped support; dynamic-body exclusion; raised-foot animation','Repeated read and warm/cold complete saves without new footprobes; owned-slot rollback'])

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in('binary','source','license','output'):ap.add_argument('--'+name,type=Path,required=True)
 for name in('hostfxr','bridge','assembly'):ap.add_argument('--'+name,type=Path)
 ap.add_argument('--timeout',type=float,default=180);args=ap.parse_args();need(20<=args.timeout<=180 and math.isfinite(args.timeout),'Finite deadline');core=any((args.hostfxr,args.bridge,args.assembly));need(not core or all((args.hostfxr,args.bridge,args.assembly)),'Supply all optional CoreCLR paths')
 args.native=lambda p:str(Path(p).resolve());args.output=args.output.resolve();need(not args.output.exists()and args.output.parent.is_dir(),'Fresh output');args.output.mkdir();args.deadline=time.monotonic()+args.timeout
 evidence=dict(format='poima.test.runtime-ground-contacts.v1',passed=False,calls=[],owners=[],checkpoints=[],whole_pairs=[],checks=[],samples=[],limitations=['CPU source-derived sole geometry, not rendered gait/footlocking/pelvis qualification.','Native leaders EOF0 require external descendant ownership.','Original project/source remain available; no relocated or access-denial claim.'])
 w=None;status=1
 try:
  evidence['input_pins']={str(p):identity(p)for p in(args.binary.resolve(),args.source.resolve(),args.license.resolve(),Path(__file__).resolve(),Path(metric.__file__).resolve(),Path(base.__file__).resolve())}
  if core:
   paths={args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve(),ROOT/'tests/managed_ik_metric_gameplay/ManagedIkMetricGame.cs'}
   for image in(args.bridge.resolve(),args.assembly.resolve()):
    paths.add(image.parent/'Poima.Gameplay.dll')
    for suffix in('.deps.json','.runtimeconfig.json'):
     if image.with_suffix(suffix).is_file():paths.add(image.with_suffix(suffix))
   evidence['input_pins'].update({str(p):identity(p)for p in paths})
  w=Workflow(args,evidence)
  if core:w.game_config=dict(hostfxr=args.native(args.hostfxr),bridge=args.native(args.bridge),assembly=args.native(args.assembly),type=metric.GAME)
  w.prepare();w.cases();w.restore(w.cold_contact);w.sample_contacts(36,0,'cold-source-pose');need(all(s['grounding']['status']=='contact'for s in w.states()),'Cold source must retain active contacts');source=w.capture('cold-source');w.step(2);future=w.capture('cold-expected');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.p=base.RawSession(args,w.world,evidence);w.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=args.native(w.saves)));w.restore(source,cold=True);w.pair('complete-cold-no-probe',source,w.capture('cold-restored'));w.step(2);w.pair('complete-cold-future',future,w.capture('cold-future'));w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.guard_authored();need(identity(w.package)==w.package_pin,'Original cooked content changed');need(all(identity(p)==v for p,v in evidence['input_pins'].items()),'Frozen inputs changed');base.census(args.output);need(time.monotonic()<args.deadline,'Whole original workflow deadline');evidence['passed']=True;status=0
 except BaseException:evidence['error']=traceback.format_exc()
 finally:
  if w and w.p:
   try:w.p.close()
   except BaseException:evidence['closure_error']=traceback.format_exc();status=1;evidence['passed']=False
  evidence['seconds']=time.monotonic()-(args.deadline-args.timeout);(args.output/'report.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n')
 print(json.dumps({k:v for k,v in evidence.items()if k in('passed','seconds','error','closure_error')}));return status
if __name__=='__main__':raise SystemExit(main())
