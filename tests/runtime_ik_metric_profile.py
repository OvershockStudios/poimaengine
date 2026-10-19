#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original Knight, explicit near-uniform IK, complete saves and optional C#.

Builds/downloads nothing. Supply the unchanged KayKit Knight GLB and CC0 license.
All original geometry/curves are checked against the actual cooked package. Two
leg chains are then checked against independent full affine FK at one declared
time in every original take, with extra Idle/Running_A samples. This is an
explicit pose-control qualification, not foot contact, gait, rendering or timing.
Use an external process-tree owner; retained EOF0 is only native-leader evidence.
"""
import argparse,copy,hashlib,json,math,struct,subprocess,time,traceback
from pathlib import Path
import runtime_two_bone_ik_orientation as base
from character_library_intake import LibraryOracle,decode_cooked,SOURCE_SHA,LICENSE_SHA
from imported_character_contract import multiply,transform_matrix,interpolate_rotation
from components_gameplay_contract import uid

ROOT=Path(__file__).resolve().parents[1]
RIG=uid(400);GAME='Poima.Tests.ManagedIkMetricGame';IDENTITY='poima.test.managed-ik-metric'
CHAINS=((1,25,24,23),(2,29,28,27));LIMIT=1e-4
need=base.need;identity=base.identity;typed_equal=base.typed_equal

def unit(q):
 n=math.hypot(*q);need(n>0 and math.isfinite(n),'Independent finite quaternion');return [x/n for x in q]
def product(a,b):
 x,y,z,w=a;X,Y,Z,W=b
 return unit([w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z])
def fk(local,parents):
 matrices={};rotations={}
 def visit(i):
  if i not in matrices:
   own=transform_matrix(**local[i]);q=unit(local[i]['rotation'])
   if i in parents:
    visit(parents[i]);own=multiply(matrices[parents[i]],own);q=product(rotations[parents[i]],q)
   matrices[i]=own;rotations[i]=q
  return matrices[i]
 for i in range(len(local)):visit(i)
 return [matrices[i]for i in range(len(local))],[rotations[i]for i in range(len(local))]
def metrics(local,parents,middle):
 worlds,rotations=fk(local,parents);errors=[0.,0.,0.];i=middle
 while i is not None:
  scale=local[i]['scale'];need(all(math.isfinite(x)and x>0 for x in scale),'Independent positive original scales')
  errors[0]=max(errors[0],max(scale)/min(scale)-1)
  m=worlds[i];r=transform_matrix([0,0,0],rotations[i],[1,1,1]);a=[[m[c*4+k]for c in range(3)]for k in range(3)];rr=[[r[c*4+k]for c in range(3)]for k in range(3)]
  s=sum(a[k][c]*rr[k][c]for k in range(3)for c in range(3))/3;need(s>0,'Independent positive frame projection')
  d=[[x/s for x in row]for row in a]
  errors[1]=max(errors[1],max(abs(d[k][c]-rr[k][c])for k in range(3)for c in range(3)))
  errors[2]=max(errors[2],max(abs(sum(d[k][c]*d[k][j]for k in range(3))-(c==j))for c in range(3)for j in range(3)))
  i=parents.get(i)
 return errors
def distance(a,b):return math.hypot(*(x-y for x,y in zip(a,b)))
def close(a,b,tolerance,message):
 need(type(a)in(list,tuple)and len(a)==len(b)and all(type(x)in(int,float)and math.isfinite(x)for x in a),message+' shape/type')
 error=max((abs(x-y)for x,y in zip(a,b)),default=0);need(error<=tolerance,message+': '+repr((error,tolerance)));return error
def canonical(q):
 q=unit(q)
 for i in(3,0,1,2):
  if q[i]:
   if q[i]<0:q=[-x for x in q]
   break
 return [0. if x==0 else x for x in q]

class SourcePoses:
 def __init__(self,oracle,meta):
  self.oracle=oracle;self.parents=oracle.parents;self.base=[]
  for raw,cooked in zip(oracle.nodes,meta['nodes']):
   def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]
   expected=dict(position=[f32(x)for x in raw.get('translation',[0,0,0])],rotation=unit([f32(x)for x in raw.get('rotation',[0,0,0,1])]),scale=[f32(x)for x in raw.get('scale',[1,1,1])])
   for key in expected:close(cooked[key],expected[key],2e-15,'Original stored rest '+key)
   self.base.append({key:list(cooked[key])for key in expected})
 def pose(self,clip=None,at=0):
  # Original raw accessor interpolation; cooked curve equality was checked in
  # decode_cooked. Stored rest uses the importer's documented float32 defaults.
  local=copy.deepcopy(self.base)
  if clip is not None:
   import bisect
   for c in self.oracle.clips[clip]['channels']:
    times=c['times'];right=min(len(times)-1,bisect.bisect_right(times,at));left=max(0,bisect.bisect_right(times,at)-1);u=0 if right==left else(at-times[left])/(times[right]-times[left]);a,b=c['values'][left],c['values'][right]
    value=interpolate_rotation(a,b,u)if c['path']=='rotation'else[x+(y-x)*u for x,y in zip(a,b)]
    local[c['node']][{'translation':'position','rotation':'rotation','scale':'scale'}[c['path']]]=list(value)
  return local

class Workflow(base.Workflow):
 def prepare(self):
  original=self.args.source.read_bytes();license_=self.args.license.read_bytes();self.oracle=LibraryOracle(original)
  need(hashlib.sha256(license_).hexdigest()==LICENSE_SHA,'Exact original CC0 license required')
  self.source=self.args.output/'original-Knight.glb';self.source.write_bytes(original);(self.args.output/'original-LICENSE.txt').write_bytes(license_)
  info=self.p.rpc('asset.source.inspect',dict(source=self.args.native(self.source)))
  asset=self.p.rpc('asset.import',dict(source=self.args.native(self.source),expected_model_sha256=info['model_sha256']))['asset']
  package=Path(str(self.world)+'.assets')/(asset+'.pmodel');meta,meshes,mips=decode_cooked(package.read_bytes(),self.oracle);self.poses=SourcePoses(self.oracle,meta)
  self.e['original_asset']=dict(source=identity(self.source),license=identity(self.args.license),asset=asset,cooked=identity(package),nodes=57,takes=76,channels=8712,keys=90748,triangles=6952,vertices=7024,skin_joints=41,mips=mips)
  self.package=package;self.package_pin=identity(package)
  self.tx([dict(op='asset.instantiate',id=RIG,asset=asset,name='Original Knight explicit metric profile')])
  authored=json.loads(self.world.read_bytes());rig=copy.deepcopy(authored['entities'][RIG]['components']['AnimationRig']);self.rig=rig
  rig.update(clip=None,playing=False,ik=[dict(slot=s,root=r,middle=m,end=e,target=[0,0,0],pole=[0,0,1],space='rig',enabled=False)for s,r,m,e in CHAINS])
  before=self.world.read_bytes()
  for explicit in(False,True):
   strict=copy.deepcopy(rig)
   if explicit:
    for c in strict['ik']:c['metric']='uniform'
   error=self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=strict)],-32602)
   need('uniform'in error['message'].lower()and self.world.read_bytes()==before,'Original strict rest rejection/atomicity')
  for c in rig['ik']:c['metric']='near-uniform-v1'
  for bad in(None,True,1,{},'near-uniform','NEAR-UNIFORM-V1'):
   malformed=copy.deepcopy(rig);malformed['ik'][0]['metric']=bad
   self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=malformed)],-32602);need(self.world.read_bytes()==before,'Malformed metric changed authored bytes/history')
  retry_id=base.fresh();malformed=copy.deepcopy(rig);malformed['ik'][1]['metric']='invalid'
  ops=[dict(op='entity.create',id=uid(901),name='Atomic unused-ID retry witness'),dict(op='component.set',id=RIG,type='AnimationRig',value=malformed)]
  self.p.rpc('world.transact',dict(request_id=retry_id,base_revision=self.revision,ops=ops),-32602);need(self.world.read_bytes()==before,'Late metric rejection changed full authored namespace/history')
  accepted=self.p.rpc('world.transact',dict(request_id=retry_id,base_revision=self.revision,ops=ops[:1]));self.revision=accepted['revision']
  self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=rig)])
  self.document=json.loads(self.world.read_bytes());self.document_contract=base.frozen_document_contract(self.document);self.retain_authored('authored-before-start')
  self.p.start(self.revision);self.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=self.args.native(self.saves)));self.guard_authored()
 def states(self,include=True):
  results=[]
  for slot,*_ in CHAINS:
   params=dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=slot,include_end_orientation=True)
   if include is not None:params['include_metric']=include
   value=self.p.rpc('runtime.ik',params);need(value['session_id']==self.p.session and value['tick']==self.p.tick,'Exact IK observation boundary');results.append(value['state'])
  return results
 def observe(self):
  rows={};envelope=None;ids=[base.node_id(RIG,i)for i in range(57)]
  for start in(0,32):
   page=self.p.rpc('runtime.observe',dict(session_id=self.p.session,tick=self.p.tick,ids=ids[start:start+32],entity_fields=['world_matrix','local_transform']))
   header={k:v for k,v in page.items()if k not in('entities','query','component_types')}
   need(header['session_id']==self.p.session and header['tick']==self.p.tick,'Pose same-session/tick')
   if envelope is None:envelope=header
   else:need(typed_equal(envelope,header),'Both pose pages must have all exact revision/sequence fields')
   need({r['id']for r in page['entities']}==set(ids[start:start+32]),'Exact57 actual RigNode membership')
   for row in page['entities']:need(row['id']not in rows,'Unique observed node');rows[row['id']]=row['state']
  return [rows[i]['local_transform']for i in ids],[rows[i]['world_matrix']for i in ids],envelope
 def commands(self,local,oriented=True):
  worlds,quats=fk(local,self.poses.parents);commands=[]
  for slot,r,m,e in CHAINS:
   root=worlds[r][12:15];knee=worlds[m][12:15];end=worlds[e][12:15]
   target=[a+.98*(b-a)for a,b in zip(root,end)];pole=[knee[0],knee[1],knee[2]+.2]
   command=dict(entity=RIG,slot=slot,enabled=True,space='rig',target=target,pole=pole,weight=1)
   if oriented:command['end_orientation']=dict(rotation=canonical(quats[e]),weight=1)
   commands.append(command)
  return commands
 def sample(self,clip,at,label,oriented=True):
  incoming=self.poses.pose(clip,at);commands=self.commands(incoming,oriented)
  result,request=self.step(animations=[dict(entity=RIG,clip=clip,time=at,speed=0,loop=False,playing=False,blend_ticks=0)],ik=commands)
  actual,observed,envelope=self.observe();states=self.states();computed,_=fk(actual,self.poses.parents);original,_=fk(incoming,self.poses.parents);changed={r for _,r,m,e in CHAINS}|{m for _,r,m,e in CHAINS}|({e for _,r,m,e in CHAINS}if oriented else set())
  max_matrix=max_local=0.
  for i in range(57):
   max_matrix=max(max_matrix,close(observed[i],computed[i],2e-7,'Actual full affine FK node '+str(i)))
   for key in('position','scale'):max_local=max(max_local,close(actual[i][key],incoming[i][key],2e-8,'Original unsnapped local '+key+' node '+str(i)))
   if i not in changed:
    for axis in([1,0,0],[0,1,0],[0,0,1]):close(base.rotated(actual[i]['rotation'],axis),base.rotated(incoming[i]['rotation'],axis),2e-6,'Unchanged original animated rotation node '+str(i))
  diagnostics=[]
  for (slot,r,m,e),state,command in zip(CHAINS,states,commands):
   need((state['root'],state['middle'],state['end'])==(r,m,e),'Original deform leg correspondence')
   metric=state['metric'];need(type(metric)is dict and metric['profile']=='near-uniform-v1'and metric['evaluated']is True and type(metric['relative_limit'])is float and metric['relative_limit']==LIMIT,'Explicit evaluated metric profile/types')
   expected=[max(a,b)for a,b in zip(metrics(incoming,self.poses.parents,m),metrics(actual,self.poses.parents,m))]
   close([metric[k]for k in('local_scale_error','frame_error','gram_error')],expected,2e-10,'Independent input/published metric maxima')
   need(all(x<=LIMIT for x in expected),'Independent bounded actual affine frame')
   a=distance(original[r][12:15],original[m][12:15]);b=distance(original[m][12:15],original[e][12:15]);target=command['target'];root=original[r][12:15];length=distance(root,target);reach=min(a+b,max(abs(a-b),length));wanted=[root[k]+(target[k]-root[k])*reach/length for k in range(3)];residual=distance(computed[e][12:15],wanted);limit=max(1e-9,LIMIT*(a+b))
   close([metric['unweighted_position_error'],metric['unweighted_position_limit']],[residual,limit],2e-8,'Actual affine endpoint/residual budget');need(residual<=limit,'Actual target residual exceeds near-uniform budget');close(state['end_position'],computed[e][12:15],2e-7,'Actual published endpoint');close([state['target_error']],[distance(target,computed[e][12:15])],2e-7,'Actual target-error diagnostic')
   if oriented:
    goal=state['end_orientation'];need(goal is not None,'Retained logical foot orientation')
    for axis in([1,0,0],[0,1,0],[0,0,1]):close(base.rotated(goal['actual_rotation'],axis),base.rotated(command['end_orientation']['rotation'],axis),2e-6,'Actual rig-frame foot orientation')
   diagnostics.append(dict(slot=slot,metric=copy.deepcopy(metric),independent_maxima=expected,actual_target_error=state['target_error'],independent_residual=residual))
  # Real source skinning operands, not a native/GPU vertex readback. Preserve
  # finite foot-mesh witnesses alongside all57 actual affine transforms.
  witnesses=[]
  for node in(54,55):
   if 'mesh'in self.oracle.nodes[node]:
    for index in(0,len(self.oracle.meshes[self.oracle.nodes[node]['mesh']]['positions'])//2):
     position,normal=self.oracle.vertex((actual,[],computed),node,index);need(all(math.isfinite(x)for x in position+normal),'Finite original skinning witness');witnesses.append(dict(node=node,vertex=index,position=position,normal=normal))
  self.e['samples'].append(dict(label=label,clip=clip,name=self.oracle.clips[clip]['name'],time=at,tick=self.p.tick,request_id=request['request_id'],pose_envelope=envelope,oriented=oriented,max_matrix_error=max_matrix,max_unchanged_local_error=max_local,diagnostics=diagnostics,source_skinning_witnesses=witnesses));return result,request
 def cases(self):
  default=self.states(None);explicit=self.states();need(all('metric'not in s for s in default),'Default state shape changed')
  for a,b in zip(default,explicit):need(typed_equal(a,{k:v for k,v in b.items()if k!='metric'}),'Metric query changed old fields');need(b['metric']['evaluated']is False and all(b['metric'][k]is None for k in('local_scale_error','frame_error','gram_error','unweighted_position_error','unweighted_position_limit')),'Inactive metric nulls')
  initial=self.capture('original-disabled-v4');need(initial['value']['snapshot']['payload']['animation']['version']==4,'Metric must not change point-only animation blob version')
  for bad in(None,0,'true'):
   self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=1,include_metric=bad),-32602)
  self.pair('invalid-query-full-state',initial,self.capture('after-invalid-query'))
  for clip in self.oracle.clips:self.sample(clip['index'],clip['duration']*.5,'all-takes-'+str(clip['index']))
  for clip in(36,48):
   need(self.oracle.clips[clip]['name']in('Idle','Running_A'),'Pinned detailed original take')
   for fraction in(0.,.25,.75,1.):self.sample(clip,self.oracle.clips[clip]['duration']*fraction,'detailed-'+str(clip)+'-'+str(fraction))
  result,request=self.sample(48,self.oracle.clips[48]['duration']*.5,'exact-retry-source')
  active=self.capture('oriented-active-v6');need(active['value']['snapshot']['payload']['animation']['version']==6,'Actual retained orientation must remain nestedv6')
  need(typed_equal(self.p.rpc('runtime.step',request),dict(result,replayed=True)),'Exact native step replay');self.pair('exact-retry-full-state',active,self.capture('after-retry'))
  self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick-1,entity=RIG,slot=1,include_metric=True),-32009);self.pair('stale-query-full-state',active,self.capture('after-stale'))
  self.sample(36,0,'warm-detour');self.restore(active);self.pair('warm-original-full-state',active,self.capture('warm-restored'))
  self.e['checks'].append('Original strict rest rejection, explicit near-profile, all76+Idle/Run affine57-node observations, original geometry/curves, active/inactive metrics, full retry/stale/warm state')

def compiled(w):
 loaded=w.p.load(w.game_config);need(loaded['module']['type']==GAME and loaded['module']['schema']['identity']==IDENTITY,'Original supplied compiled metric consumer')
 before=w.p.game()['module']['values'];w.p.edit(Mode=1);w.step();after=w.p.game()['module']['values'];need(after['Reads']==before['Reads']+2 and after['Writes']==before['Writes']+2,'Both real compiled leg controls')
 states=w.states();need(all(s['metric']['evaluated']and s['end_orientation']is not None for s in states),'Compiled controls used frozen near profile')
 w.p.edit(Mode=2);a=w.capture('compiled-before-throw');error,_=w.step(error=True);need('metric rollback'in error['message'],'Actual compiled failure');w.pair('compiled-both-legs-full-rollback',a,w.capture('compiled-after-throw'));w.p.edit(Mode=0)
 a=w.capture('compiled-warm-source');w.step();w.restore(a);w.pair('compiled-warm-full-state',a,w.capture('compiled-warm-restored'))
 w.e['checks'].append('Supplied compiled352-byte consumer reads/writes both original deform legs and complete Tick-throw rollback')

def main(argv=None):
 ap=argparse.ArgumentParser(description=__doc__)
 for name in('binary','source','license','output'):ap.add_argument('--'+name,type=Path,required=True)
 for name in('hostfxr','bridge','assembly'):ap.add_argument('--'+name,type=Path)
 ap.add_argument('--timeout',type=float,default=180);ap.add_argument('--windows-interop',action='store_true');args=ap.parse_args(argv)
 need(20<=args.timeout<=180 and math.isfinite(args.timeout),'Finite bounded deadline');core=any((args.hostfxr,args.bridge,args.assembly));need(not core or all((args.hostfxr,args.bridge,args.assembly)),'All optional CoreCLR paths required')
 def native(path):
  absolute=str(Path(path).resolve());return subprocess.check_output(['wslpath','-w',absolute],text=True).strip()if args.windows_interop else absolute
 args.native=native;args.output=args.output.resolve();need(not args.output.exists()and args.output.parent.is_dir(),'Fresh bounded output');args.output.mkdir();started=time.monotonic();args.deadline=started+args.timeout
 e=dict(format='poima.test.runtime-ik-metric-profile.v1',passed=False,calls=[],owners=[],checkpoints=[],whole_pairs=[],checks=[],samples=[],limitations=['Original asset/curves unchanged; explicit controlled poses, not automatic contacts/gait/grounding.','CPU native affine57-node evidence and source-derived skinning witnesses; no rendered/GPU geometry observation.','Two EOF0 leaders require an external process-tree owner for descendant closure.','Original checkout and supplied source/assemblies remain readable; no filesystem sandbox or source-absence claim.','Optional compiled consumer uses existing352-byte SDK; no compilation by this fixture.'])
 w=None;status=1
 try:
  paths={Path(__file__).resolve(),Path(base.__file__).resolve(),ROOT/'tests/character_library_intake.py',ROOT/'tests/imported_character_contract.py',ROOT/'tests/input_actions_gameplay_contract.py',ROOT/'tests/components_gameplay_contract.py',ROOT/'tests/character_gameplay_contract.py',ROOT/'tests/animation_fixture.py',ROOT/'tests/gltf_fixture.py',args.binary.resolve(),args.source.resolve(),args.license.resolve()}
  paths.update(ROOT/'tests'/name for name in('scene_capture.py','switchback_game_service.py','switchback_oracle.py'))
  paths.update(p for p in(ROOT/'tools/python/poima_client').rglob('*')if p.is_file()and p.suffix in('.py','.json'))
  if core:
   paths.update((args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve(),ROOT/'tests/managed_ik_metric_gameplay/ManagedIkMetricGame.cs',ROOT/'tests/managed_ik_metric_gameplay/Poima.ManagedIkMetricGame.csproj',ROOT/'managed/Directory.Build.props'))
   for image in(args.assembly.resolve(),args.bridge.resolve()):
    paths.add(image.parent/'Poima.Gameplay.dll')
    for suffix in('.deps.json','.runtimeconfig.json'):
     if image.with_suffix(suffix).is_file():paths.add(image.with_suffix(suffix))
  e['input_pins']={str(p):identity(p)for p in sorted(paths)};w=Workflow(args,e)
  if core:w.game_config=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type=GAME)
  w.prepare();w.cases()
  if core:compiled(w)
  source=w.capture('cold-source');w.step(2);continuation=w.capture('cold-expected');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.p=base.RawSession(args,w.world,e);w.p.rpc('save.configure',dict(request_id=base.fresh(),expected_generation=0,root=native(w.saves)));w.restore(source,cold=True);w.pair('cold-complete-original',source,w.capture('cold-restored'));w.step(2);w.pair('cold-complete-continuation',continuation,w.capture('cold-continuation'));w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.guard_authored();need(identity(w.package)==w.package_pin and all(identity(p)==v for p,v in e['input_pins'].items()),'Original immutable source/package/helper closure changed');need(time.monotonic()<args.deadline,'Whole fixture deadline');e['passed']=True;status=0
 except BaseException:e['error']=traceback.format_exc()
 finally:
  if w and w.p:
   try:w.p.close()
   except BaseException:e['cleanup_error']=traceback.format_exc();e['passed']=False;status=1
  e['seconds']=time.monotonic()-started;e['rpc_count']=len(e['calls']);e['output_before_report']=base.census(args.output)
  if time.monotonic()>=args.deadline:e['passed']=False;e['deadline_exhausted']=True;status=1
  data=(json.dumps(e,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=8*1024**2,'Report8MiB reserve');pending=args.output/'evidence.pending.json';pending.write_bytes(data);base.census(args.output);pending.rename(args.output/'evidence.json')
 print(json.dumps(dict(passed=e['passed'],samples=len(e['samples']),rpc=e['rpc_count'])));return status
if __name__=='__main__':raise SystemExit(main())
