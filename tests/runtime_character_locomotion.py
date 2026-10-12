#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Opt-in virtual locomotion authoring, traversal and cold reconstruction. External owned Job required."""
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
def controller(profile=None,legacy=False):
 value=dict(radius=.3,height=1.8,speed=2,jump_speed=5,camera=None)
 if not legacy:value['locomotion']={} if profile is None else profile
 return value
def input_row(n,move=(0,0),jump=False):return dict(entity=uid(n),move=list(move),look=[0,0],jump=jump,use=False)
def xyz(row):return [row['world_matrix'][i] for i in (12,13,14)]
def native_entity(row):return {key:value for key,value in row.items() if key!='session_id'}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--timeout',type=float,default=180);a=p.parse_args()
 need(math.isfinite(a.timeout) and 0<a.timeout<=180,'Finite bounded deadline required');a.binary=a.binary.resolve();a.output=a.output.resolve();need(not a.output.exists(),'Fresh output required');a.output.mkdir(parents=True)
 sources=[Path(__file__),ROOT/'tests/runtime_camera_lens.py',ROOT/'tests/live_material_capture.py',ROOT/'tests/shadow_controls_capture.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py',ROOT/'tests/texture_fixture.py',a.binary,a.binary.parent/'phonon.dll']
 sources += [x for x in (ROOT/'tools/python/poima_client').rglob('*') if x.is_file() and x.suffix in ('.py','.json')]
 pins={str(x):sha(x) for x in sources};r=dict(passed=False,calls=[],transport=[],checks=[],owners=[],durable_payloads={},durable_receipts={},stages={});owner=None;tick=0;config=0;started=time.monotonic();deadline=started+a.timeout
 def call(method,params,error=None):
  try:
   result=owner.rpc(method,params);need(error is None,'Expected native rejection');return result
  except RpcError as e:
   need(e.code==error,'Unexpected native failure: '+str(e));r['calls'][-1].update(expected_error_code=e.code,error_message=e.message,error_data=e.data);return None
 def entity(n,error=None,at=None):return call('runtime.entity',dict(session_id=owner.session,tick=tick if at is None else at,id=uid(n)),error)
 def step(count=1,inputs=None,motions=None,error=None,request=None):
  nonlocal tick
  args=dict(session_id=owner.session,request_id=request or new(),expected_tick=tick,ticks=count,inputs=inputs or [],motions=motions or [])
  result=call('runtime.step',args,error)
  if error is None:tick=result['tick']
  return args,result
 def save(slot):
  receipt=call('save.write',dict(request_id=new(),configuration_generation=config,slot=slot,expected_generation=0,session_id=owner.session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=0,expected_material_revision=0))
  folder=a.output/'saves'/('slot-'+slot);manifest=json.loads((folder/'current.json').read_bytes());member=manifest['payload']['current'];name=member['file']
  need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe selected save filename')
  path=folder/name;data=path.read_bytes();digest=hashlib.sha256(data).hexdigest();need(len(data)==member['bytes'] and digest==member['sha256']==receipt['sha256'] and receipt['bytes']==len(data) and receipt['generation']==member['generation']==manifest['payload']['generation'],'Durable native payload receipt differs')
  payload=json.loads(data);need(payload['format']=='poima.world-save' and payload['snapshot']['version']==9,'Virtual save format differs')
  r['durable_payloads'][slot]=payload;r['durable_receipts'][slot]=receipt;return payload
 def load(slot,active):
  nonlocal tick
  session=new();result=call('save.load',dict(request_id=new(),configuration_generation=config,slot=slot,expected_generation=1,revision=owner.revision,expected_session_id=owner.session if active else None,expected_tick=tick if active else None,expected_gameplay_revision=0 if active else None,expected_structure_revision=0 if active else None,new_session_id=session))
  owner.session=session;tick=result['tick'];return result
 def close_owner():
  nonlocal owner
  owner.close();r['owners'].append(copy.deepcopy(r['native_owner']));owner=None
 def box(n,pos,half,motion='static',rotation=(0,0,0,1)):
  return [dict(op='entity.create',id=uid(n),name='Locomotion '+str(n),parent=None),transform(uid(n),position=pos,rotation=rotation),component(uid(n),'BoxCollider',dict(half_extents=list(half),motion=motion,mass=1,friction=.5,restitution=0))]
 def cached(n):
  row=entity(n);state=row['locomotion'];need(state['mode']=='virtual-v1' and len(state['ground_normal'])==3 and len(state['ground_velocity'])==3 and all(math.isfinite(x) for x in state['ground_normal']+state['ground_velocity']),'Cached locomotion diagnostics invalid');return row
 try:
  owner=LensOwner(a,r,deadline,1);d=call('world.describe',{});need(d['schema_revision']==94,'Locomotion discovery revision differs')
  props=d['components']['CharacterController']['properties']['locomotion'];need(props['type']=='object' and props['additionalProperties'] is False and props['required']==[],'Locomotion discovery object differs')
  for field,limit,default_value in [('step_height',1,.4),('ground_snap',1,.5),('max_slope_degrees',80,45)]:
   need(props['properties'][field]==dict(type='number',minimum=0,maximum=limit,default=default_value),'Locomotion discovery bounds/default differ')
  ops=box(1,[0,-.5,14],[20,.5,24]);ops += box(20,[0,.5,20],[2,.5,2],'kinematic')
  for i in range(3):ops += box(10+i,[i+.5,(i+1)*.1,0],[.5,(i+1)*.1,1])
  ops += box(13,[.5,.5,4],[.5,.5,1])
  for n,degrees,z in ((30,35,8),(40,55,12)):
   angle=math.radians(degrees);ops += box(n,[3,3*math.tan(angle)-.1/math.cos(angle),z],[3/math.cos(angle),.1,1],rotation=[0,0,math.sin(angle/2),math.cos(angle/2)])
  for n,pos,profile,legacy in [(100,[-2,.1,0],None,False),(200,[-2,.1,4],None,False),(300,[-2,.1,8],None,False),(400,[-2,.1,12],None,False),(500,[0,1.1,20],None,False),(600,[8,.1,28],None,True),(700,[8,.1,24],dict(step_height=0,ground_snap=0,max_slope_degrees=80),False)]:
   ops += [dict(op='entity.create',id=uid(n),name='Controller '+str(n),parent=None),transform(uid(n),position=pos),component(uid(n),'CharacterController',controller(profile,legacy))]
  owner.edit(ops);original=(a.output/'world.json').read_bytes();(a.output/'course-original.json').write_bytes(original);r['original_world_sha256']=hashlib.sha256(original).hexdigest()
  bad_profiles=[None,[],True,0,dict(extra=1),dict(step_height=True),dict(ground_snap=False),dict(max_slope_degrees=True),dict(step_height='0.4'),dict(step_height=-.001),dict(step_height=1.001),dict(ground_snap=-.001),dict(ground_snap=1.001),dict(max_slope_degrees=-.001),dict(max_slope_degrees=80.001),dict(step_height=1e308)]
  for profile in bad_profiles:
   call('world.transact',dict(request_id=new(),base_revision=owner.revision,ops=[component(uid(100),'CharacterController',dict(controller(),locomotion=profile))]),-32602)
   need((a.output/'world.json').read_bytes()==original,'Invalid locomotion profile changed authored bytes')
  owner.session=new();call('runtime.start',dict(session_id=owner.session,revision=owner.revision));step(count=60)
  default=dict(step_height=f32(.4),ground_snap=.5,max_slope_degrees=45.0);need(same(cached(100)['locomotion']['profile'],default),'Default frozen locomotion profile differs')
  need(same(cached(700)['locomotion']['profile'],dict(step_height=0.0,ground_snap=0.0,max_slope_degrees=80.0)),'Boundary locomotion profile differs')
  need('locomotion' not in entity(600) and 'locomotion' not in entity(1),'Legacy/noncontroller acquired optional diagnostics');entity(999,-32004);entity(100,-32009,at=tick-1)
  before_read=call('runtime.inspect',dict(session_id=owner.session));first=cached(100);second=cached(100);need(same(first,second) and same(before_read,call('runtime.inspect',dict(session_id=owner.session))),'Cached inspection advanced or refreshed native state')
  r['checks'].append('strict_optional_authoring_defaults_legacy_omission_and_cached_guards')
  step(count=120,inputs=[input_row(n,(1,0)) for n in (100,200,300,400)])
  ramp=cached(300);steep=cached(400);blocked=cached(200);need(xyz(ramp)[0]>.7 and xyz(ramp)[1]>.4,'Walkable ramp did not ascend')
  # The foot point is below the capsule's contact on a tilted surface; its
  # analytic offset is radius*(sec(slope)-1), independently of solver output.
  ramp_plane=math.tan(math.radians(35))*xyz(ramp)[0];need(abs(xyz(ramp)[1]-(ramp_plane+f32(.3)*(1/math.cos(math.radians(35))-1)))<.06,'Ramp foot geometry differs from authored plane')
  normal=ramp['locomotion']['ground_normal'];need(abs(normal[0]+math.sin(math.radians(35)))<.03 and abs(normal[1]-math.cos(math.radians(35)))<.03 and abs(normal[2])<.03,'Ramp support normal differs')
  need(xyz(steep)[0]<.5 and xyz(steep)[1]<.7 and xyz(blocked)[0]<.05 and xyz(blocked)[1]<.15,'Steep ramp or over-height obstacle admitted traversal')
  step(count=24,inputs=[input_row(100,(1,0))]);stairs=cached(100);foot=xyz(stairs)
  # The third authored tread is x[2,3], y=.6, z[-1,1]. The complete
  # radius-.3 capsule must end strictly inside both X margins.
  need(2.3<foot[0]<2.7 and .55<foot[1]<.67 and stairs['ground']=='on_ground' and all(abs(x-y)<1e-6 for x,y in zip(stairs['locomotion']['ground_normal'],[0,1,0])),'Three bounded steps failed to reach the independently supported third tread')
  ray=call('runtime.raycast',dict(session_id=owner.session,tick=tick,origin=[foot[0],1.1,.5],direction=[0,-1,0],distance=2))
  hit=ray['hit'];top=.3+f32(.3)
  need(hit is not None and hit['entity']==uid(12) and hit['triangle'] is None and hit['normal'] is not None and len(hit['normal'])==len(hit['position'])==3 and
       all(abs(x-y)<1e-6 for x,y in zip(hit['normal'],[0,1,0])) and
       all(abs(x-y)<1e-6 for x,y in zip(hit['position'],[foot[0],top,.5])) and abs(hit['distance']-(1.1-top))<1e-6 and abs(hit['fraction']-(1.1-top)/2)<1e-6,'Independent downward ray did not witness the authored third-tread top')
  r['stages']['traversal']=dict(stairs=stairs,third_tread_ray=ray,walkable_ramp=ramp,steep_ramp=steep,over_height=blocked);r['checks'].append('fixed_tick_stairs_walkable_ramp_and_steep_or_tall_rejection')
  platform_before=cached(500);position_before=xyz(platform_before);motion=dict(entity=uid(20),position=[3,.5,20],rotation=[0,0,0,1],duration_ticks=120)
  args,_=step(count=60,motions=[motion]);platform_mid=cached(500);support=entity(20);need(abs(xyz(support)[0]-1.5)<1e-5 and abs(xyz(platform_mid)[0]-position_before[0]-1.5)<.08 and abs(xyz(platform_mid)[1]-position_before[1])<.03,'Neutral character failed translating-platform carry')
  need(abs(platform_mid['locomotion']['ground_velocity'][0]-1.5)<.03 and abs(platform_mid['locomotion']['ground_velocity'][1])<.03,'Cached support velocity differs from kinematic trajectory')
  replay=call('runtime.step',args);need(replay['replayed'] is True and replay['tick']==tick and same(platform_mid,cached(500)),'Retried platform interval advanced twice')
  conflict=copy.deepcopy(args);conflict['motions'][0]['position'][0]=4;call('runtime.step',conflict,-32010)
  owner.edit([component(uid(500),'CharacterController',controller(dict(step_height=.1,ground_snap=.2,max_slope_degrees=20)))])
  need(same(platform_mid,cached(500)),'Live authoring changed the frozen backend/profile')
  r['stages']['platform']=dict(before=platform_before,mid=platform_mid,support=support);r['checks'].append('kinematic_support_transport_retry_and_frozen_profile')
  (a.output/'saves').mkdir();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(a.output/'saves')))['generation'];mid=save('platform-mid')
  need(mid['document']['entities'][uid(500)]['components']['CharacterController']['locomotion']=={},'Durable baseline adopted live edited locomotion')
  malformed_before=save('reject-before');step(inputs=[input_row(500,(1,0)),input_row(999,(1,0))],error=-32040);malformed_after=save('reject-after');need(same(malformed_before['snapshot'],malformed_after['snapshot']),'Rejected movement batch changed full native state')
  # Ground contacts are reconstructed. Compare two cold-restored native owners,
  # keeping both immediate full payload equality and continued exact Ticks.
  load('platform-mid',True);cold=save('cold-reference');need(same(mid['snapshot'],cold['snapshot']),'Immediate same-owner restore changed full native snapshot')
  step(count=20);continued=save('continued-reference');expected_actor=cached(500);expected_support=entity(20);expected_tick=tick
  close_owner();owner=LensOwner(a,r,deadline,2);owner.revision=call('world.inspect',{})['revision'];config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(a.output/'saves')))['generation'];load('platform-mid',False)
  fresh=save('cold-fresh');need(same(mid['snapshot'],fresh['snapshot']),'Fresh owner lost full virtual/physical saved state')
  step(count=20);actual=save('continued-fresh');need(tick==expected_tick and same(continued['snapshot'],actual['snapshot']) and same(native_entity(expected_actor),native_entity(cached(500))) and same(native_entity(expected_support),native_entity(entity(20))),'Two cold owners diverged on moving-platform continuation')
  r['checks'].append('complete_v9_atomic_rejection_and_two_cold_owner_platform_continuation');r['passed']=True
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
