#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Committed lens controls, guards, retries and fresh-process saves; external Job owner required."""
import argparse,copy,hashlib,json,sys,time,traceback,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'))
from live_material_capture import Owner,need,sha,encoded,same
from shadow_controls_capture import component,transform
from poima_client import RpcError

class LensOwner(Owner):
 def __init__(self,args,record,deadline,ordinal):
  self.ordinal=ordinal;super().__init__(args,record,deadline)
 def raw(self,row):
  row['owner']=self.ordinal;super().raw(row)
 def rpc(self,method,params):
  with self.transport._condition:identifier=self.transport._next_id
  index=len(self.record['calls'])
  try:return super().rpc(method,params)
  finally:
   self.record['calls'][index]['owner']=self.ordinal;self.record['calls'][index]['rpc_id']=identifier

def uid(n):return f'{n:032x}'
def new():return uuid.uuid4().hex
def lens(fov=60,near=.1,far=1000):return dict(vertical_fov=fov,near=near,far=far)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--timeout',type=float,default=180);a=p.parse_args();need(0<a.timeout<=180,'Bounded deadline required')
 a.binary=a.binary.resolve();a.output=a.output.resolve();need(not a.output.exists(),'Fresh output required');a.output.mkdir(parents=True)
 sources=[Path(__file__),ROOT/'tests/live_material_capture.py',ROOT/'tests/shadow_controls_capture.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py',ROOT/'tests/texture_fixture.py',a.binary,a.binary.parent/'phonon.dll']
 sources += [x for x in (ROOT/'tools/python/poima_client').rglob('*') if x.is_file() and x.suffix in ('.py','.json')]
 pins={str(x):sha(x) for x in sources};r=dict(passed=False,calls=[],transport=[],native_owner=None,checks=[],fresh_owner=None)
 owner=None;tick=0;started=time.monotonic();deadline=started+a.timeout
 def read(entity=uid(1),error=None):return call('runtime.camera_lens',dict(session_id=owner.session,tick=tick,entity=entity),error)
 def call(method,params,error=None):
  try:
   result=owner.rpc(method,params);need(error is None,'Expected rejection');return result
  except RpcError as e:
   need(error==e.code,'Unexpected RPC failure: '+str(e));r['calls'][-1]['expected_error_code']=e.code;return None
 def step(commands,request=None,error=None,count=1):
  nonlocal tick
  args=dict(session_id=owner.session,request_id=request or new(),expected_tick=tick,ticks=count,camera_lenses=commands)
  result=call('runtime.step',args,error)
  if error is None:tick=result['tick']
  return args,result
 def save(slot):
  return call('save.write',dict(request_id=new(),configuration_generation=config,slot=slot,expected_generation=0,
   session_id=owner.session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=0,expected_material_revision=0))
 try:
  owner=LensOwner(a,r,deadline,1);d=call('world.describe',{})
  need(d['schema_revision']==91 and 'runtime.camera_lens' in d['methods'] and 'camera_lenses' in d['methods']['runtime.step']['properties'],'Lens discovery missing')
  ops=[]
  for n,cam in [(1,dict(lens(),viewmodel=lens(75,.01,10))),(2,lens(90)),(3,None)]:
   ops += [dict(op='entity.create',id=uid(n),name='Camera '+str(n),parent=None),transform(uid(n))]
   if cam is not None:ops += [component(uid(n),'Camera',cam)]
  owner.edit(ops);original=sha(a.output/'world.json');owner.session=new();call('runtime.start',dict(session_id=owner.session,revision=owner.revision))
  need(read()['state']==dict(entity=uid(1),world=lens(),viewmodel=lens(75,.01,10),authored_world=lens()),'Initial native lens differs')
  need(read(uid(3))['state'] is None,'Noncamera did not return null');read(uid(99),-32004)
  call('runtime.camera_lens',dict(session_id=owner.session,tick=1,entity=uid(1)),-32009)
  r['checks'].append('discovery_cached_reads_noncamera_missing_and_tick_guard')
  before=call('runtime.inspect',dict(session_id=owner.session));before_lens=read()
  malformed=[[],[dict(entity=uid(1))],[dict(entity=uid(1),world=None)],[dict(entity=uid(1),world=dict(lens(),near=True))],
   [dict(entity=uid(1),world=lens(4))],[dict(entity=uid(1),world=lens(far=.1))],[dict(entity=uid(1),world=dict(lens(),extra=1))],
   [dict(entity=uid(1),world=lens()),dict(entity=uid(1),viewmodel=lens())]]
  # Empty arrays are a legal neutral step, test only malformed commands here.
  for commands in malformed[1:]:
   step(commands,error=-32602)
   need(same(before,call('runtime.inspect',dict(session_id=owner.session))) and same(before_lens,read()),'Malformed command changed native state')
  for commands in [[dict(entity=uid(2),viewmodel=lens())],[dict(entity=uid(3),world=lens())],[dict(entity=uid(99),world=lens())],
                   [dict(entity=uid(1),world=lens(35)),dict(entity=uid(2),viewmodel=lens())]]:
   step(commands,error=-32040)
   need(same(before,call('runtime.inspect',dict(session_id=owner.session))) and same(before_lens,read()),'Rejected batch changed native state')
  need(sha(a.output/'world.json')==original,'Runtime rejection rewrote authored content');r['checks'].append('malformed_membership_duplicate_and_atomic_batch_rejection')
  command=dict(entity=uid(1),world=lens(35,.025,250),viewmodel=lens(50,.005,5));args,result=step([command],count=3)
  need(tick==3 and read()['state']==dict(entity=uid(1),world=command['world'],viewmodel=command['viewmodel'],authored_world=lens()),'Committed lenses/baseline differ')
  replay=call('runtime.step',args);need(replay['replayed'] is True and replay['tick']==3 and read()['tick']==3,'Retry applied twice')
  changed=copy.deepcopy(args);changed['camera_lenses'][0]['world']['vertical_fov']=40;call('runtime.step',changed,-32010)
  step([dict(entity=uid(1),world=lens(45))]);need(read()['state']['viewmodel']==command['viewmodel'],'World write replaced foreground')
  step([dict(entity=uid(1),viewmodel=lens(80,.01,10))]);need(read()['state']['world']==lens(45),'Foreground write replaced world')
  r['checks'].append('first_tick_complete_partial_controls_and_retry_receipts')
  saves=a.output/'saves';saves.mkdir();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves)))['generation'];save('lens')
  manifest=json.loads((saves/'slot-lens/current.json').read_bytes());member=manifest['payload']['current'];payload=(saves/'slot-lens'/member['file']).read_bytes();need(sha(saves/'slot-lens'/member['file'])==member['sha256'],'Save digest differs')
  r['durable_sha256']=hashlib.sha256(payload).hexdigest();world_save=json.loads(payload)
  need(world_save['document']['entities'][uid(1)]['components']['Camera']==dict(lens(),viewmodel=lens(75,.01,10)),'Save rewrote authored lens')
  expected=read()['state'];owner.close();r['first_owner']=r['native_owner'];owner=None
  # Reopen a separate owner and restore the exact complete camera state.
  owner=LensOwner(a,r,deadline,2);owner.revision=world_save['document']['revision'];config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves)))['generation']
  session=new();loaded=call('save.load',dict(request_id=new(),configuration_generation=config,slot='lens',expected_generation=1,revision=owner.revision,
   expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=session))
  owner.session=session;tick=loaded['tick'];need(read()['state']==expected and tick==5,'Fresh restore lost committed lens state')
  step([dict(entity=uid(1),world=lens(),viewmodel=lens(75,.01,10))]);need(read()['state']['world']==lens() and read()['state']['viewmodel']==lens(75,.01,10),'Neutral reset did not restore authored lenses')
  need(sha(a.output/'world.json')==original,'Lens controls mutated authored document');r['checks'].append('fresh_process_save_restore_and_exact_neutral_reset');r['passed']=True
 except BaseException:r['error']=traceback.format_exc()
 finally:
  if owner is not None:
   try:owner.close();r['fresh_owner']=r['native_owner']
   except BaseException:r['cleanup_error']=traceback.format_exc();r['passed']=False
  r['owners']=[x for x in (r.get('first_owner'),r.get('fresh_owner')) if x];r['binary_sha256']=sha(a.binary);r['seconds']=time.monotonic()-started;r['source_pins']=pins;r['source_unchanged']=all(x.is_file() and sha(x)==h for x,h in [(Path(n),h) for n,h in pins.items()]);r['passed']=bool(r['passed'] and r['source_unchanged'] and r['seconds']<=180)
  data=encoded(r)+b'\n';need(len(data)<=32*1048576,'Evidence budget');(a.output/'evidence.json').write_bytes(data)
  if time.monotonic()>deadline:
   r['passed']=False;r['deadline_exhausted']=True;(a.output/'evidence.json').write_bytes(encoded(r)+b'\n')
 print(json.dumps(dict(passed=r['passed'],checks=len(r['checks']),error=r.get('error'))));return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
