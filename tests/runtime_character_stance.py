#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native stance authoring, clearance, rollback and cold save restore. External owned Job required."""
import argparse,copy,hashlib,json,math,struct,sys,time,traceback,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'))
from runtime_camera_lens import LensOwner
from live_material_capture import need,sha,encoded,same
from shadow_controls_capture import component,transform
from poima_client import RpcError

def uid(n):return f'{n:032x}'
def new():return uuid.uuid4().hex
def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
def controller(stance=True):
 value=dict(radius=.3,height=1.8,speed=4,jump_speed=5,camera=uid(101) if stance else None)
 if stance:value['stance']={}
 return value

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--timeout',type=float,default=180);a=p.parse_args()
 need(math.isfinite(a.timeout) and 0<a.timeout<=180,'Finite bounded deadline required');a.binary=a.binary.resolve();a.output=a.output.resolve();need(not a.output.exists(),'Fresh output required');a.output.mkdir(parents=True)
 sources=[Path(__file__),ROOT/'tests/runtime_camera_lens.py',ROOT/'tests/live_material_capture.py',ROOT/'tests/shadow_controls_capture.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py',ROOT/'tests/texture_fixture.py',a.binary,a.binary.parent/'phonon.dll']
 sources += [x for x in (ROOT/'tools/python/poima_client').rglob('*') if x.is_file() and x.suffix in ('.py','.json')]
 pins={str(x):sha(x) for x in sources};r=dict(passed=False,calls=[],transport=[],checks=[],owners=[],durable_payloads={},durable_receipts={});owner=None;tick=0;config=0;started=time.monotonic();deadline=started+a.timeout
 def call(method,params,error=None,message=None):
  try:
   result=owner.rpc(method,params);need(error is None,'Expected native rejection');return result
  except RpcError as e:
   need(e.code==error and (message is None or e.message==message),'Unexpected native failure: '+str(e));r['calls'][-1].update(expected_error_code=e.code,error_message=e.message,error_data=e.data);return None
 def read(n=100,error=None):return call('runtime.character_stance',dict(session_id=owner.session,tick=tick,entity=uid(n)),error)
 def entity(n):return call('runtime.entity',dict(session_id=owner.session,tick=tick,id=uid(n)))
 def step(commands=None,count=1,inputs=None,error=None,request=None):
  nonlocal tick
  args=dict(session_id=owner.session,request_id=request or new(),expected_tick=tick,ticks=count,character_stances=commands if commands is not None else [],inputs=inputs or [])
  result=call('runtime.step',args,error)
  if error is None:tick=result['tick']
  return args,result
 def save(slot):
  receipt=call('save.write',dict(request_id=new(),configuration_generation=config,slot=slot,expected_generation=0,session_id=owner.session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=0,expected_material_revision=0))
  folder=a.output/'saves'/('slot-'+slot);manifest=json.loads((folder/'current.json').read_bytes());member=manifest['payload']['current'];name=member['file']
  need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe selected save filename')
  data=(folder/name).read_bytes();need(len(data)==member['bytes'] and hashlib.sha256(data).hexdigest()==member['sha256'] and receipt['sha256']==member['sha256'],'Durable native payload digest differs');payload=json.loads(data);need(payload['format']=='poima.world-save' and payload['snapshot']['version']==8,'Stance save format differs')
  r['durable_payloads'][slot]=payload;r['durable_receipts'][slot]=receipt;return payload
 def close_owner():
  nonlocal owner
  owner.close();r['owners'].append(copy.deepcopy(r['native_owner']));owner=None
 try:
  owner=LensOwner(a,r,deadline,1);description=call('world.describe',{})
  need(description['schema_revision']==94 and 'runtime.character_stance' in description['methods'] and description['methods']['runtime.step']['properties']['character_stances']['maxItems']==32,'Stance discovery missing')
  ops=[]
  for n,pos,parent in [(1,[0,-.5,0],None),(2,[0,1.4,0],None),(100,[-2,.1,0],None),(101,[0,1.6,0],uid(100)),(300,[5,.1,0],None),(500,[0,0,4],None)]:
   ops += [dict(op='entity.create',id=uid(n),name='Stance '+str(n),parent=parent),transform(uid(n),position=pos)]
  ops += [component(uid(1),'BoxCollider',dict(half_extents=[8,.5,8],motion='static',mass=1,friction=.5,restitution=0)),component(uid(2),'BoxCollider',dict(half_extents=[.8,.1,1],motion='static',mass=1,friction=.5,restitution=0)),component(uid(100),'CharacterController',controller()),component(uid(300),'CharacterController',controller(False)),component(uid(101),'Camera',dict(vertical_fov=70,near=.1,far=100))]
  owner.edit(ops);original=sha(a.output/'world.json')
  for profile in [None,[],dict(extra=1),dict(crouched_height=True),dict(crouched_height=.6),dict(crouched_height=1.8),dict(crouched_speed=0),dict(crouched_speed=31),dict(camera_drop=-.01),dict(camera_drop=.81)]:
   bad=dict(controller(),stance=profile);call('world.transact',dict(request_id=new(),base_revision=owner.revision,ops=[component(uid(100),'CharacterController',bad)]),-32602);need(sha(a.output/'world.json')==original,'Malformed profile changed authored bytes')
  call('world.transact',dict(request_id=new(),base_revision=owner.revision,ops=[component(uid(500),'AnimationRig',dict(asset='a'*64,clip=None,time=0,speed=1,loop=True,playing=False)),component(uid(101),'RigNode',dict(rig=uid(500),node=0))]),-32602,'Stance controller camera cannot be an animation-owned RigNode.');need(sha(a.output/'world.json')==original,'Dual camera ownership changed authored bytes')
  owner.session=new();call('runtime.start',dict(session_id=owner.session,revision=owner.revision));step(count=60)
  initial=read()['state'];need(initial['configured'] and not initial['crouched'] and not initial['requested_crouch'] and not initial['stand_blocked'],'Initial stance differs')
  need(initial['radius']==f32(.3) and initial['standing_height']==f32(1.8) and initial['crouched_height']==1 and initial['height']==f32(1.8) and initial['speed']==4 and initial['camera_drop']==0,'Native default dimensions differ')
  need(read(300)['state']['configured'] is False and read(500)['state'] is None,'Legacy/noncontroller inspection differs');read(999,-32004);call('runtime.character_stance',dict(session_id=owner.session,tick=tick-1,entity=uid(100)),-32009)
  r['checks'].append('strict_optional_authoring_defaults_and_cached_guards')
  (a.output/'saves').mkdir();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(a.output/'saves')))['generation'];before=save('guards-before')
  malformed=[[dict(entity=uid(100))],[dict(entity=uid(100),crouch=1)],[dict(entity=uid(100),crouch=None)],[dict(entity=uid(100),crouch=True,extra=0)],[dict(entity=uid(100),crouch=True)]*33,[dict(entity=uid(100),crouch=True),dict(entity=uid(100),crouch=False)]]
  for rows in malformed:step(rows,error=-32602)
  for target in (300,500,999):step([dict(entity=uid(100),crouch=True),dict(entity=uid(target),crouch=True)],error=-32040)
  after=save('guards-after');need(same(before['snapshot'],after['snapshot']),'Rejected stance batch changed complete native state')
  r['checks'].append('strict_commands_and_complete_native_atomic_rejection')
  eye=entity(101)['local_transform']['position'][1];feet=entity(100)['world_matrix'][13];args,_=step([dict(entity=uid(100),crouch=True)],count=3)
  crouch=read()['state'];need(crouch['requested_crouch'] and crouch['crouched'] and not crouch['stand_blocked'] and crouch['height']==1 and crouch['speed']==2 and crouch['camera_drop']==f32(.8),'Crouch dimensions/speed did not commit')
  need(abs(entity(100)['world_matrix'][13]-feet)<.03 and abs((eye-entity(101)['local_transform']['position'][1])-f32(.8))<1e-6,'Crouching moved feet or failed eye drop')
  replay=call('runtime.step',args);need(replay['replayed'] is True and replay['tick']==tick,'Stance retry advanced twice')
  altered=copy.deepcopy(args);altered['character_stances'][0]['crouch']=False;call('runtime.step',altered,-32010)
  step(count=60,inputs=[dict(entity=uid(100),move=[1,0],look=[0,0],jump=False,use=False)])
  need(abs(entity(100)['world_matrix'][12])<.05,'Crouched speed failed to reach the low doorway')
  step([dict(entity=uid(100),crouch=False)],count=3);blocked=read()['state'];need(not blocked['requested_crouch'] and blocked['crouched'] and blocked['stand_blocked'],'Blocked standing did not retain crouch')
  step(count=2);need(same(blocked,read()['state']),'Desired stand state did not persist under obstruction')
  edited=controller();edited['stance']=dict(crouched_height=1.2,crouched_speed=3,camera_drop=.6);owner.edit([component(uid(100),'CharacterController',edited)])
  need(same(blocked,read()['state']),'Authored edits changed the frozen live profile')
  blocked_payload=save('blocked');need(blocked_payload['document']['entities'][uid(100)]['components']['CharacterController']['stance']=={},'Save did not retain original frozen stance profile')
  expected_actor=entity(100);expected_eye=entity(101);expected_tick=tick;r['checks'].append('foot_preserving_crouch_clearance_persistent_controls_retry_and_frozen_profile')
  close_owner();owner=LensOwner(a,r,deadline,2);owner.revision=call('world.inspect',{})['revision'];config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(a.output/'saves')))['generation']
  session=new();loaded=call('save.load',dict(request_id=new(),configuration_generation=config,slot='blocked',expected_generation=1,revision=owner.revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=session));owner.session=session;tick=loaded['tick']
  need(tick==expected_tick and same(read()['state'],blocked),'Fresh restore lost committed blocked stance')
  actual_actor=entity(100);actual_eye=entity(101)
  for old,actual in ((expected_actor,actual_actor),(expected_eye,actual_eye)):
   for key in ('local_transform','world_matrix','velocity','yaw','pitch'):need(same(old[key],actual[key]),'Fresh stance/body/camera restoration differs: '+key)
  fresh=save('restored');need(same(blocked_payload['snapshot'],fresh['snapshot']),'Fresh restore changed full durable native snapshot')
  step(count=60,inputs=[dict(entity=uid(100),move=[1,0],look=[0,0],jump=False,use=False)]);standing=read()['state'];need(not standing['requested_crouch'] and not standing['crouched'] and not standing['stand_blocked'] and standing['height']==f32(1.8) and standing['speed']==4 and standing['camera_drop']==0,'Persistent standing did not succeed after leaving clearance obstruction')
  r['checks'].append('fresh_process_full_snapshot_and_collision_clearance_continuation');r['passed']=True
 except BaseException:r['error']=traceback.format_exc()
 finally:
  if owner is not None:
   try:close_owner()
   except BaseException:r['cleanup_error']=traceback.format_exc();r['passed']=False
  r['source_pins']=pins;r['source_unchanged']=all(Path(n).is_file() and sha(n)==h for n,h in pins.items());r['seconds']=time.monotonic()-started;r['passed']=bool(r['passed'] and r['source_unchanged'] and r['seconds']<=a.timeout and len(r['transport'])<=1000)
  data=encoded(r)+b'\n';need(len(data)<=32*1048576,'Evidence budget');need(sum(x.stat().st_size for x in a.output.rglob('*') if x.is_file())+len(data)<=64*1048576,'Whole output budget');(a.output/'evidence.json').write_bytes(data)
  if time.monotonic()>deadline:r['passed']=False;r['deadline_exhausted']=True;(a.output/'evidence.json').rename(a.output/'late-evidence.json');(a.output/'failure.json').write_bytes(encoded(r)+b'\n')
 print(json.dumps(dict(passed=r['passed'],checks=len(r['checks']),error=r.get('error'))));return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
