#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual native end-orientation authoring, controls, saves and compiled consumers.

Builds nothing. Run under an external process-tree owner with a matching native
engine. Optional CoreCLR paths or one Native AOT descriptor enable the supplied
original compiled orientation game. Raw requests/replies and complete original
save payloads are retained. No renderer, foot-grounding or performance claim.
"""
import argparse,copy,hashlib,json,math,queue,struct,subprocess,time,traceback
from pathlib import Path
from input_actions_gameplay_contract import OwnedSession,exact,fresh,need
from components_gameplay_contract import uid,sha
from animation_fixture import ribbon
from gltf_fixture import glb
ROOT=Path(__file__).resolve().parents[1]
GAME='Poima.Tests.ManagedTwoBoneIkOrientationGame'
IDENTITY='poima.test.managed-two-bone-ik-orientation'
RIG=uid(400);TEMPLATE=uid(600)
QZ=[0,0,1,0];Q90=[0,0,math.sqrt(.5),math.sqrt(.5)];QC=[.5,.5,.5,.5]
MAX_RPC=700;CAP=128*1024**2;MAX_FILES=512

def identity(path):return dict(bytes=Path(path).stat().st_size,sha256=sha(path))
def near(a,b,epsilon=2e-6):
 need(len(a)==len(b)and all(type(v)in(int,float)and math.isfinite(v)and abs(v-w)<=epsilon for v,w in zip(a,b)),'Independent orientation/position oracle differs: '+repr((a,b)))
def rotated(q,v):
 n=math.hypot(*q);x,y,z,w=(a/n for a in q);u=[x,y,z];dot=sum(a*b for a,b in zip(u,v));uu=sum(a*a for a in u);cross=[y*v[2]-z*v[1],z*v[0]-x*v[2],x*v[1]-y*v[0]]
 return [2*dot*u[i]+(w*w-uu)*v[i]+2*w*cross[i]for i in range(3)]
def same_rotation(a,b):
 for axis in ([1,0,0],[0,1,0],[0,0,1]):near(rotated(a,axis),rotated(b,axis))
def control(**extra):
 value=dict(entity=RIG,slot=1,enabled=True,space='rig',target=[1,1,0],pole=[0,0,1],weight=1);value.update(extra);return value
def orientation(rotation=QZ,weight=1):return dict(rotation=list(rotation),weight=weight)
def rig_configuration(value,primary):
 result=copy.deepcopy(value)
 if not primary:result['ik'][0].pop('end_orientation')
 return result
def node_id(rig,node):return hashlib.sha256(f'poima.instance.v1/{rig}/node/{node}'.encode()).hexdigest()[:32]
CANONICAL_HISTORY=('receipts','retired_ids','retired_component_schemas','retired_template_ids','retired_ui_ids')
def typed_equal(a,b):
 # Bool/int/float and every nested member remain distinct; no coercion.
 if type(a)is not type(b):return False
 if type(a)is dict:return all(type(k)is str for k in a)and all(type(k)is str for k in b)and set(a)==set(b)and all(typed_equal(a[k],b[k])for k in a)
 if type(a)is list:return len(a)==len(b)and all(typed_equal(x,y)for x,y in zip(a,b))
 return a==b
def frozen_document_contract(authored):
 # This declaration is made from the original authored source BEFORE start,
 # never from an observed save. Optional history keys are not synthesized.
 need(type(authored)is dict and {'receipts','retired_ids'}<=set(authored),'Original authored canonical-history fields')
 present=tuple(name for name in CANONICAL_HISTORY if name in authored)
 need(all(type(authored[name])is list for name in present),'Original authored history ledgers must be arrays')
 return dict(keys=tuple(sorted(authored)),canonical_history=present)
def assert_frozen_document(saved,authored,contract):
 need(type(saved)is dict and set(saved)==set(authored)==set(contract['keys']),'Complete frozen authored-document key set')
 need(tuple(name for name in CANONICAL_HISTORY if name in authored)==contract['canonical_history'],'Original prospective history membership')
 for name in contract['keys']:
  if name in contract['canonical_history']:
   need(type(saved[name])is list and saved[name]==[],'Canonical frozen history must remain an explicit empty array: '+name)
  else:need(typed_equal(saved[name],authored[name]),'Complete typed frozen authored field differs: '+name)
def census(root):
 files=[p for p in root.rglob('*')if p.is_file()];need(len(files)<=MAX_FILES and sum(p.stat().st_size for p in files)<=CAP-8*1024**2,'Observed fixture output bound/reserve')
 return dict(files=len(files),bytes=sum(p.stat().st_size for p in files))

class RawSession(OwnedSession):
 def __init__(self,args,world,evidence):
  self.index=len(evidence['owners']);super().__init__(args,world,evidence)
  self.raw_in=(args.output/f'owner-{self.index}.stdin.jsonl').open('xb');self.raw_out=(args.output/f'owner-{self.index}.stdout.jsonl').open('xb');self.owner['raw_stdin']=str(self.raw_in.name);self.owner['raw_stdout']=str(self.raw_out.name)
 def rpc(self,method,params=None,error=False):
  need(len(self.evidence['calls'])<MAX_RPC and time.monotonic()<self.args.deadline,'RPC/deadline bound')
  request=dict(jsonrpc='2.0',id=len(self.evidence['calls'])+1,method=method,params=copy.deepcopy(params or {}));row=dict(owner=self.index,request=request,before_ns=time.monotonic_ns());self.evidence['calls'].append(row)
  data=(json.dumps(request,allow_nan=False,separators=(',',':'))+'\n').encode();self.raw_in.write(data);self.raw_in.flush();self.process.stdin.write(data.decode());self.process.stdin.flush()
  try:line=self.output.get(timeout=min(20,max(.001,self.args.deadline-time.monotonic())))
  except queue.Empty:raise TimeoutError('Owned native response timeout')from None
  need(line is not None,'Native owner ended before response');data=line.encode();self.raw_out.write(data);self.raw_out.flush();row['after_ns']=time.monotonic_ns();reply=json.loads(line);row['response']=reply
  need(reply.get('id')==request['id']and time.monotonic()<self.args.deadline,'Original response correlation/deadline')
  need(self.raw_in.tell()+self.raw_out.tell()<=32*1024**2,'Per-owner raw wire bound');census(self.args.output)
  if error:
   need('error'in reply and 'result'not in reply,'Expected original native rejection')
   if type(error)is int:need(reply['error']['code']==error,'Wrong original error code: '+str(reply['error']))
   return reply['error']
  need('result'in reply and 'error'not in reply,'Original native request failed: '+str(reply.get('error')));return reply['result']
 def close(self):
  try:
   super().close()
   if not getattr(self,'raw_closed',False):
    surplus=0
    while True:
     try:line=self.output.get(timeout=1)
     except queue.Empty:raise RuntimeError('Native stdout reader has no terminal EOF proof')from None
     if line is None:break
     self.raw_out.write(line.encode());surplus+=1
    self.owner['surplus_stdout_lines']=surplus;self.raw_closed=True;need(surplus==0,'Unexpected original stdout after final correlated response')
  finally:
   for stream in(self.raw_in,self.raw_out):
    if not stream.closed:stream.flush();stream.close()
   self.owner['raw_stdin_identity']=identity(self.raw_in.name);self.owner['raw_stdout_identity']=identity(self.raw_out.name)

class Workflow:
 def __init__(self,args,evidence):
  self.args=args;self.e=evidence;self.world=args.output/'world.json';self.p=RawSession(args,self.world,evidence);self.revision=0;self.structure=0;self.sequence=0;self.saves=args.output/'saves';self.saves.mkdir();self.game_config=None
 def tx(self,ops,error=False):
  if hasattr(self,'authored_bytes'):self.guard_authored()
  result=self.p.rpc('world.transact',dict(request_id=fresh(),base_revision=self.revision,ops=ops),error)
  if not error:
   self.revision=result['revision']
   if hasattr(self,'authored_bytes'):self.retain_authored('after-author-edit-'+str(self.revision))
  elif hasattr(self,'authored_bytes'):self.guard_authored()
  return result
 def retain_authored(self,label):
  raw=self.world.read_bytes();value=json.loads(raw);need(value['revision']==self.revision,'Actual authored boundary revision')
  path=self.args.output/(label+'.json')
  with path.open('xb')as stream:stream.write(raw)
  self.e.setdefault('authored_boundaries',[]).append(dict(label=label,path=str(path.relative_to(self.args.output)),identity=identity(path),revision=self.revision,history_counts={name:len(value[name])for name in CANONICAL_HISTORY if name in value}))
  self.authored_bytes=raw
 def guard_authored(self):
  need(self.world.read_bytes()==self.authored_bytes,'Runtime operation unexpectedly changed full authored bytes/history')
  for boundary in self.e['authored_boundaries']:need(identity(self.args.output/boundary['path'])==boundary['identity'],'Retained full authored boundary/history bytes changed')
 def state(self,rig=RIG,include=True):
  self.guard_authored()
  params=dict(session_id=self.p.session,tick=self.p.tick,entity=rig,slot=1)
  if include is not None:params['include_end_orientation']=include
  return self.p.rpc('runtime.ik',params)['state']
 def step(self,ticks=1,error=False,**extra):
  self.guard_authored()
  params=dict(session_id=self.p.session,request_id=fresh(),expected_tick=self.p.tick,ticks=ticks,expected_structure_revision=self.structure,**extra);result=self.p.rpc('runtime.step',params,error)
  if not error:self.p.tick=result['current_tick'];self.p.session=result['current_session_id'];self.structure=self.p.rpc('runtime.inspect',dict(session_id=self.p.session))['structure_revision']
  self.guard_authored()
  return result,params
 def capture(self,label):
  self.guard_authored()
  need(self.sequence<40,'Checkpoint count bound');self.sequence+=1;slot='orientation-'+str(self.sequence);game_revision=self.p.game()['revision']
  receipt=self.p.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=self.p.session,expected_tick=self.p.tick,expected_gameplay_revision=game_revision,expected_structure_revision=self.structure))
  members=list((self.saves/('slot-'+slot)).glob('p-*.bin'));need(len(members)==1,'One original current payload');path=members[0];raw=path.read_bytes();need(len(raw)<=8*1024**2 and sha(path)==receipt['sha256'],'Original complete payload receipt')
  value=json.loads(raw);need(set(value)=={'format','version','document','snapshot'},'Complete world-save envelope keys');assert_frozen_document(value['document'],self.document,self.document_contract);self.guard_authored()
  self.e['checkpoints'].append(dict(label=label,slot=slot,path=str(path.relative_to(self.args.output)),identity=identity(path),receipt=receipt,tick=self.p.tick,gameplay_revision=game_revision,before_return_ns=time.monotonic_ns()))
  return dict(value=value,raw=raw,slot=slot,tick=self.p.tick,game_revision=game_revision,identity=identity(path))
 def pair(self,label,a,b):
  need(a['raw']==b['raw']and exact(a['value'],b['value']),'Complete typed/lexical checkpoint pair differs: '+label);self.e['whole_pairs'].append(dict(label=label,left=a['slot'],right=b['slot']))
 def restore(self,capture,cold=False):
  self.guard_authored()
  params=dict(request_id=fresh(),configuration_generation=1,slot=capture['slot'],expected_generation=1,revision=self.revision,expected_session_id=None if cold else self.p.session,expected_tick=None if cold else self.p.tick,expected_gameplay_revision=None if cold else self.p.game()['revision'],new_session_id=fresh(),expected_structure_revision=None if cold else self.structure)
  if self.game_config and capture['value']['snapshot']['payload']['gameplay']is not None:params['gameplay']=self.game_config
  result=self.p.rpc('save.load',params);self.p.session=params['new_session_id'];self.p.tick=result['tick'];need(self.p.tick==capture['tick'],'Original restore boundary');self.structure=self.p.rpc('runtime.inspect',dict(session_id=self.p.session))['structure_revision'];self.guard_authored();return result
 def prepare(self):
  doc,blob=ribbon();doc['nodes'][1]['children']=[3];doc['nodes'].append(dict(name='Effector',translation=[0,1,0],children=[4]));doc['nodes'].append(dict(name='End attachment',translation=[.25,0,0]));blob=bytearray(blob)
  def accessor(rows,kind):
   while len(blob)%4:blob.append(0)
   start=len(blob);blob.extend(struct.pack('<'+'f'*sum(len(r)for r in rows),*(x for r in rows for x in r)));doc['bufferViews'].append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start));doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1,componentType=5126,count=len(rows),type=kind));return len(doc['accessors'])-1
  times=accessor([(0,),(1/64,),(1/32,)],'SCALAR');doc['accessors'][times].update(min=[0],max=[1/32]);scales=accessor([(1,1,1),(1,1,1),(2,1,1)],'VEC3');doc['animations'].append(dict(name='Invalid later metric',samplers=[dict(input=times,output=scales,interpolation='STEP')],channels=[dict(sampler=0,target=dict(node=2,path='scale'))]));doc['buffers'][0]['byteLength']=len(blob)
  self.source=self.args.output/'original-ribbon.glb';self.source.write_bytes(glb(doc,bytes(blob)));asset=self.p.rpc('asset.import',dict(source=self.args.native(self.source)))['asset'];self.e['original_asset']=dict(source=identity(self.source),asset=asset)
  count=65 if self.game_config else 1;self.tx([dict(op='asset.instantiate',id=uid(400+i),asset=asset,name='Orientation chain '+str(i))for i in range(count)]+[dict(op='entity.create',id=uid(900),name='Physical rollback witness'),dict(op='component.set',id=uid(900),type='Transform',value=dict(position=[5,10,0],rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=uid(900),type='BoxCollider',value=dict(half_extents=[.5]*3,motion='dynamic',mass=1,friction=.5,restitution=0))])
  authored=json.loads(self.world.read_bytes());self.rig=copy.deepcopy(authored['entities'][RIG]['components']['AnimationRig']);self.rig.update(clip=None,playing=False,ik=[dict(slot=1,root=2,middle=1,end=3,target=[1,1,0],pole=[0,0,1],space='rig',end_orientation=orientation([0,0,2,0]))])
  self.tx([dict(op='component.set',id=uid(400+i),type='AnimationRig',value=rig_configuration(self.rig,i==0))for i in range(count)])
  authored=json.loads(self.world.read_bytes());members={}
  for key,value in authored['entities'].items():
   ancestor=key
   while ancestor is not None and ancestor!=RIG:ancestor=authored['entities'][ancestor]['parent']
   if ancestor==RIG:members[key]=copy.deepcopy(value)
  members[RIG]['parent']=None;self.tx([dict(op='template.set',id=TEMPLATE,name='Oriented complete graph',root=RIG,entities=members)])
  original=self.world.read_bytes()
  for goal in malformed_goals():
   bad=copy.deepcopy(self.rig);bad['ik'][0]['end_orientation']=goal;self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=bad)],-32602);need(self.world.read_bytes()==original,'Rejected authored goal changed bytes/history')
  self.document=json.loads(original);self.document_contract=frozen_document_contract(self.document);self.retain_authored('authored-before-start');need(self.authored_bytes==original,'Original authored source unchanged before start');self.source.unlink();self.p.start(self.revision);self.guard_authored();self.p.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=self.args.native(self.saves)))
 def native_cases(self):
  default=self.state(include=None);need('end_orientation'not in default,'Default IK response ABI changed');explicit=self.state();need(set(explicit)==set(default)|{'end_orientation'}and all(exact(explicit[k],default[k])for k in default),'Opt-in state changed old fields');near(explicit['end_orientation']['rotation'],QZ);same_rotation(explicit['end_orientation']['actual_rotation'],QZ);near(explicit['end_position'],[1,1,0]);near([explicit['end_orientation']['angular_error']],[0])
  attachment=self.p.rpc('runtime.entity',dict(session_id=self.p.session,tick=self.p.tick,id=node_id(RIG,4)))['world_matrix'];near(attachment[12:15],[.75,1,0])
  for bad in (None,0,1,'true'):
   self.p.rpc('runtime.ik',dict(session_id=self.p.session,tick=self.p.tick,entity=RIG,slot=1,include_end_orientation=bad),-32602)
  baseline=self.capture('authored-active-v6');need(baseline['value']['snapshot']['payload']['animation']['version']==6,'Retained orientation not serialized asv6')
  for goal in malformed_goals():
   self.step(error=-32602,ik=[control(end_orientation=goal)]);need(exact(self.state(),explicit),'Malformed request changed cached state')
  self.step(error=-32602,ik=[control(),control(end_orientation=orientation())]);self.pair('malformed-whole-rollback',baseline,self.capture('after-malformed'))
  self.step(ik=[control()]);need(self.state()['end_orientation']is None,'Omission did not clear orientation');cleared=self.capture('point-cleared');need(cleared['value']['snapshot']['payload']['animation']['version']==4,'Point-only blob no longerlegacyv4')
  self.restore(baseline);self.pair('warm-active-v6',baseline,self.capture('warm-active-restored'));self.step(ik=[control(end_orientation=orientation([2,3,4,5],0))]);self.pair('zero-vs-omitted-complete-bytes',cleared,self.capture('zero-cleared'))
  self.step(ik=[control(end_orientation=orientation([-0.,-0.,-2.,-0.]))]);state=self.state();near(state['end_orientation']['rotation'],QZ)
  result,params=self.step(ik=[control(end_orientation=orientation())]);before=self.capture('before-exact-retry');need(self.p.rpc('runtime.step',params)==dict(result,replayed=True),'Exact retry result changed');self.pair('exact-retry',before,self.capture('after-exact-retry'))
  disabled=control(enabled=False,end_orientation=orientation());self.step(ik=[disabled]);same_rotation(self.state()['end_orientation']['actual_rotation'],[0,0,0,1]);near(self.state()['end_position'],[0,2,0])
  self.step(ik=[control(end_orientation=orientation(QC))]);same_rotation(self.state()['end_orientation']['actual_rotation'],QC);before=self.capture('before-late-tick')
  self.step(3,error=-32040,animations=[dict(entity=RIG,clip=1,time=0,speed=1,loop=False,playing=True)]);self.pair('later-tick-full-rollback',before,self.capture('after-late-tick'))
  before_state=self.state();bad=copy.deepcopy(self.rig);bad['ik'][0]['end_orientation']=orientation(Q90)
  self.tx([dict(op='component.set',id=RIG,type='AnimationRig',value=bad)]);need(exact(self.state(),before_state),'Live author edit changed frozen oriented pose');undone=self.p.rpc('world.undo',dict(request_id=fresh(),base_revision=self.revision));need(undone['revision']==self.revision+1,'Actual undo revision');self.revision=undone['revision'];self.retain_authored('after-author-undo-'+str(self.revision))
  # Undo is a real authored revision. The current frozen runtime/save document
  # remains the original document; do not erase its revision or authored field.
  self.e['checks'].append('JSON strict authoring/opt-in/default/clear/zero/disabled/geometry/retry/v6/warm restore and whole late-Tick rollback')

def malformed_goals():
 return [None,True,{},dict(rotation=[0,0,0,1]),dict(weight=1),orientation([0,0,0,0]),orientation([0,0,1]),orientation([0,0,0,True]),orientation(weight=True),orientation(weight=-.1),orientation(weight=1.1),dict(orientation(),extra=1),dict(orientation(),space='world'),orientation([1e308]*4),orientation([0,0,0,0],0)]

def compiled_cases(w):
 p=w.p;e=w.e;config=w.game_config;loaded=p.load(config);need(loaded['module']['backend']==e['backend']and loaded['module']['assembly_sha256']==e['image']['sha256']and loaded['module']['type']==GAME and loaded['module']['schema']['identity']==IDENTITY,'Actual supplied compiled consumer/backend identity')
 if e['backend']=='native_aot':need(loaded['module']['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Native AOT diagnostic contract')
 def values():return p.game()['module']['values']
 def mode(number,**fields):p.edit(Mode=number,**fields)
 def full_failure(number,label,**step):
  mode(number);before=w.capture('before-'+label);result,_=w.step(error=True,**step);w.pair(label,before,w.capture('after-'+label));return result
 def verify_goal(q,point_weight,weight):
  state=w.state();near([state['weight']],[point_weight]);need(state['end_orientation']is not None,'Compiled V2 goal absent');same_rotation(state['end_orientation']['rotation'],q);near([state['end_orientation']['weight']],[weight]);return state
 # The managed consumer decodes real 144/248-byte V2 records on every call;
 # actual goal/point fields and cached values are inspected independently here.
 before=values();mode(1);w.step();state=verify_goal(Q90,1,1);same_rotation(state['end_orientation']['actual_rotation'],Q90);need(values()['Invisible']==before['Invisible']+1,'Staged V2 write became visible or missing read')
 w.step();v=values();need(v['HasOrientation']==1,'Compiled V2 read omitted goal');same_rotation([v['GoalX'],v['GoalY'],v['GoalZ'],v['GoalW']],Q90);same_rotation([v['ActualX'],v['ActualY'],v['ActualZ'],v['ActualW']],Q90);near([v['AngularError']],[0]);near([v['EndX'],v['EndY'],v['EndZ']],state['end_position'])
 mode(2);w.step();same_rotation(verify_goal(QC,0,1)['end_orientation']['actual_rotation'],QC);near(w.state()['end_position'],[0,2,0])
 mode(3);w.step();partial=verify_goal(QC,.5,.25);need(partial['end_orientation']['angular_error']>0,'Partial orientation unexpectedly exact full target')
 for number in(4,5,6):
  mode(1);w.step();mode(number);w.step();need(w.state()['end_orientation']is None,'Complete V1/V2null/zero replacement failed to clear')
 for number,label in((7,'v2-v1-duplicate'),(8,'v1-v2-duplicate'),(10,'65-write-budget'),(12,'1025-read-budget'),(13,'zero-quaternion-callback')):full_failure(number,label)
 mode(9);before=values();w.step();need(values()['Writes']==before['Writes']+64,'Real V2 exact64 shared writes did not commit');verify_goal(Q90,0,1)
 mode(11);before=values();w.step();need(values()['Reads']==before['Reads']+1024,'Real mixed V1/V2 shared1024 reads/reset failed')
 full_failure(1,'caller-game-same-slot',ik=[control(end_orientation=orientation(QC))])
 # The failure happens on the second callback after a save intent+V2 write.
 mode(14,FailAt=str(p.tick+1));before=w.capture('before-later-compiled-tick');failure,_=w.step(2,error=True);need('later-tick rollback'in failure['message'],'Wrong actual later-Tick failure');w.pair('compiled-later-tick-save-rollback',before,w.capture('after-later-compiled-tick'));need(not(w.saves/'slot-ik-orientation-intent').exists(),'Rejected batch published durable intent')
 mode(14,FailAt='-1');w.step();v=values();epoch=f'{int(v["TicketHigh"])&((1<<64)-1):016x}{int(v["TicketLow"])&((1<<64)-1):016x}';receipt=p.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(v['TicketSequence'])));need(receipt['kind']==1 and receipt['state']==3 and receipt['generation']==1 and receipt['committed_tick']==p.tick,'Compiled orientation save intent not completed');e['compiled_save_intent']=receipt
 mode(15);w.step();need(values()['SaveSucceeded']==1,'Compiled actual result query did not observe success')
 # Pending birth is addressed by its actual reserved identity; one successful
 # and one rejected complete graph are compared with every saved field intact.
 mode(17,TemplateHigh='0',TemplateLow='600');before=w.capture('before-pending-birth-conflict');w.step(error=True);w.pair('pending-birth-conflict-whole-rollback',before,w.capture('after-pending-birth-conflict'))
 mode(16,TemplateHigh='0',TemplateLow='600');w.step();spawned=values()['Spawned'];need(type(spawned)is str and len(spawned)==32 and spawned!='0'*32,'Actual committed birth identity');born=w.state(spawned);need(born['entity']==spawned,'Committed birth has wrong chain identity');same_rotation(born['end_orientation']['actual_rotation'],Q90);near(born['end_position'],[0,2,0]);e['pending_birth']=dict(root=spawned,state=born)
 e['checks'].append('Actual compiled352-byte negotiated V2 consumer, V1/V2 complete clear, shared64/1024 budgets, callback/caller conflicts, late-Tick durable rollback and pending-birth publication')
 # A complete warm restore and cold fresh-process continuation are exact,
 # retaining every gameplay scalar, animation row, entity and structure member.
 source=w.capture('compiled-warm-source');w.step(2);w.restore(source);w.pair('compiled-warm-full-state',source,w.capture('compiled-warm-restored'))

def inputs(args,evidence):
 paths=[Path(__file__).resolve(),ROOT/'tests/input_actions_gameplay_contract.py',ROOT/'tests/character_gameplay_contract.py',ROOT/'tests/components_gameplay_contract.py',ROOT/'tests/animation_fixture.py',ROOT/'tests/gltf_fixture.py',args.binary.resolve()]
 paths += [p for p in(ROOT/'tools/python/poima_client').rglob('*')if p.is_file()and p.suffix in('.py','.json')]
 config=None
 if args.native_descriptor or args.assembly:
  source=ROOT/'tests/managed_two_bone_ik_orientation_gameplay';paths += [source/'ManagedTwoBoneIkOrientationGame.cs',source/'Poima.ManagedTwoBoneIkOrientationGame.csproj',ROOT/'managed/Directory.Build.props']
  if args.native_descriptor:
   descriptor=args.native_descriptor.resolve(strict=True);need(descriptor.stat().st_size<=1024**2,'Bounded native descriptor');metadata=json.loads(descriptor.read_bytes());need(metadata['type']==GAME and metadata['identity']==IDENTITY and type(metadata['minimum_services_bytes'])is int and metadata['minimum_services_bytes']==352 and metadata['services_version']==7 and set(metadata['required_features'])=={'baseline_v7','animation_ik_v1','animation_ik_end_orientation_v1'},'Supply exact negotiated352-byte oriented consumer')
   paths.append(descriptor)
   for item in metadata['files']:
    rel=Path(item['path']);path=(descriptor.parent/rel).resolve(strict=True);need(not rel.is_absolute()and '..'not in rel.parts and path.is_relative_to(descriptor.parent)and path.stat().st_size==item['size']and sha(path)==item['sha256'],'Exact native descriptor payload closure');paths.append(path)
   image=(descriptor.parent/metadata['library']).resolve(strict=True);need(image in paths,'Native image in complete payload closure');config=dict(descriptor=args.native(descriptor),expected_descriptor_sha256=sha(descriptor));evidence['backend']='native_aot';evidence['descriptor']=dict(identity=identity(descriptor),required_features=metadata['required_features'],minimum_services_bytes=352)
  else:
   image=args.assembly.resolve(strict=True);paths += [image,args.hostfxr.resolve(strict=True),args.bridge.resolve(strict=True)]
   for owner in(image,args.bridge.resolve(strict=True)):
    paths.append(owner.parent/'Poima.Gameplay.dll')
    for suffix in('.deps.json','.runtimeconfig.json'):
     path=owner.with_suffix(suffix)
     if path.is_file():paths.append(path)
   config=dict(hostfxr=args.native(args.hostfxr),bridge=args.native(args.bridge),assembly=args.native(image),type=GAME);evidence['backend']='coreclr'
  evidence['image']=identity(image)
 evidence['input_pins']={str(p):identity(p)for p in sorted(set(paths))};return config

def main(argv=None):
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--binary',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--timeout',type=float,default=180)
 for name in('hostfxr','bridge','assembly','native-descriptor'):ap.add_argument('--'+name,type=Path)
 ap.add_argument('--windows-interop',action='store_true');args=ap.parse_args(argv);need(math.isfinite(args.timeout)and 20<=args.timeout<=180,'Finite20..180second deadline');core=any((args.hostfxr,args.bridge,args.assembly));need(not(args.native_descriptor and core)and(not core or all((args.hostfxr,args.bridge,args.assembly))),'Optional allCoreCLR paths or one native descriptor')
 def native(path):
  absolute=str(Path(path).resolve());return subprocess.check_output(['wslpath','-w',absolute],text=True).strip()if args.windows_interop else absolute
 args.native=native;args.output=args.output.resolve();need(not args.output.exists()and args.output.parent.is_dir(),'Fresh output with existing parent');args.output.mkdir();started=time.monotonic();args.deadline=started+args.timeout
 evidence=dict(format='poima.test.runtime-two-bone-ik-orientation.v1',passed=False,calls=[],owners=[],checkpoints=[],whole_pairs=[],checks=[],backend=None,limitations=['Externally owned native subprocesses; local EOF0 is not a complete descendant-lifetime proof.','Original analytic asset, no renderer/automatic contacts/physical foot grounding or performance claim.','Optional supplied compiled CoreCLR/NativeAOT consumer is executed; no compilation or publication by this fixture.','Source GLB is removed only in owned stage; checkout/tool/assembly paths remain readable; no OS sandbox claim.'])
 w=None;status=1
 try:
  config=inputs(args,evidence);w=Workflow(args,evidence);w.game_config=config;w.prepare();w.native_cases();w.guard_authored()
  if config:compiled_cases(w)
  source=w.capture('cold-source');w.step(2);continuation=w.capture('cold-expected-continuation');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  w.p=RawSession(args,w.world,evidence);w.p.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=args.native(w.saves)));w.restore(source,cold=True);w.pair('cold-exact-full-state',source,w.capture('cold-actual-restored'));w.step(2);w.pair('cold-complete-continuation',continuation,w.capture('cold-actual-continuation'))
  need(w.world.read_bytes()==w.authored_bytes and not w.source.exists(),'Retained authored bytes or owned source absence differs');w.p.rpc('runtime.stop',dict(session_id=w.p.session));w.p.close();w.p=None
  need(all(identity(path)==value for path,value in evidence['input_pins'].items()),'Consumed immutable input bytes changed');need(time.monotonic()<args.deadline,'Whole fixture deadline');evidence['passed']=True;status=0
 except BaseException:evidence['error']=traceback.format_exc()
 finally:
  if w and w.p:
   try:w.p.close()
   except BaseException:evidence['cleanup_error']=traceback.format_exc();evidence['passed']=False;status=1
  evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls']);evidence['output_before_report']=census(args.output)
  if time.monotonic()>=args.deadline:evidence['passed']=False;evidence['deadline_exhausted']=True;status=1
  data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=8*1024**2,'Report reserve');pending=args.output/'evidence.pending.json';pending.write_bytes(data);census(args.output);need(time.monotonic()<args.deadline,'Before canonical evidence deadline');pending.rename(args.output/'evidence.json')
 print(json.dumps(dict(passed=evidence['passed'],checks=evidence['checks'],rpc=evidence['rpc_count'])));return status
if __name__=='__main__':raise SystemExit(main())
