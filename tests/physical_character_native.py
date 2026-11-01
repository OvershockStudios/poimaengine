#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original full-body PhysicalRig qualification with owned native service children.

Requires an external process-tree owner. Builds and downloads nothing; preserves
literal requests, responses, complete archives and naturally closed owners.
This test definition alone establishes no native behavior or visual quality.
"""
import argparse,copy,hashlib,json,math,struct,sys,time,traceback,uuid
from pathlib import Path
STAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(STAGE/'examples/physical-character-yard'))
import knight_profile as k
sys.path.insert(0,str(k.REPO/'tests'))
import world_physical_contract as f
from imported_character_contract import transform_matrix,multiply,point

WRAPPER=dict(position=[1.,3.,-.5],rotation=[0.,math.sin(.23),0.,math.cos(.23)],scale=[1.,1.,1.])
def uid():return uuid.uuid4().hex
def clone(value):return copy.deepcopy(value)
def near(a,b,tol,why):return k.near(a,b,tol,why)

def prepare(s,source,oracle,profile,clip,terrain):
    imported=s.rpc('asset.import',dict(source=f.native(source)));asset=imported['asset']
    k.need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives'])==(57,76,1,15),'Original full library import membership')
    package=Path(str(s.world)+'.assets')/(asset+'.pmodel');raw=package.read_bytes()
    k.need(hashlib.sha256(raw).hexdigest()==asset,'Actual cooked content address')
    k.decode_cooked(raw,oracle) # Every original triangle corner, skin, inverse bind and all76 curve arrays.
    ops=[dict(op='asset.instantiate',id=f.RIG,name='Original Knight physical acceptance',asset=asset),
         f.component(f.RIG,'Transform',clone(WRAPPER)),
         f.component(f.RIG,'AnimationRig',dict(asset=asset,clip=clip,time=0.,speed=1.,loop=False,playing=True)),
         f.component(f.RIG,'PhysicalRig',clone(profile)),
         dict(op='entity.create',id=f.FLOOR,name='Static floor'),
         f.component(f.FLOOR,'Transform',dict(**f.pose((0,-.25,0)),scale=[1,1,1])),
         f.component(f.FLOOR,'BoxCollider',dict(half_extents=[12,.25,12],motion='static',mass=1,friction=.65,restitution=0))]
    if terrain=='stairs':
        for i in range(3):
            id=f'{20+i:032x}';height=.10*(i+1)
            ops += [dict(op='entity.create',id=id,name='Static stair '+str(i)),
                    f.component(id,'Transform',dict(**f.pose((.6+i*.3,height/2,-.5)),scale=[1,1,1])),
                    f.component(id,'BoxCollider',dict(half_extents=[.16,height/2,.8],motion='static',mass=1,friction=.65,restitution=0))]
    elif terrain=='slope':
        id=f'{25:032x}';a=.17
        ops += [dict(op='entity.create',id=id,name='Static slope'),
                f.component(id,'Transform',dict(position=[1,.17,-.5],rotation=[0,0,math.sin(a),math.cos(a)],scale=[1,1,1])),
                f.component(id,'BoxCollider',dict(half_extents=[1.5,.1,1.],motion='static',mass=1,friction=.65,restitution=0))]
    k.need(f.transact(s,ops,0)['revision']==1,'Original native authored revision')
    return dict(asset=asset,package=k.identity(raw),world=k.identity(s.world.read_bytes()),clip=clip,terrain=terrain)

def all_nodes(s,tick,session):
    return [s.rpc('runtime.entity',dict(session_id=session,tick=tick,id=f.node(i))) for i in range(57)]
def initialized_oracle(s,oracle,clip,session,tick=0):
    expected=oracle.pose(clip,tick/60.);wrapper=transform_matrix(**WRAPPER);rows=all_nodes(s,tick,session)
    for i,row in enumerate(rows):
        near(row['world_matrix'],multiply(wrapper,expected[2][i]),3e-5,'Original initialized FK node '+str(i))
        near(row['local_transform']['scale'],expected[0][i]['scale'],0,'Exact original initialized local scale '+str(i))
    return rows

def physical_seam(s,tick,session,oracle,profile,frozen_scales):
    state=f.query(s,tick,session,include_parts=True,include_joints=True);k.need(state['mode']=='passive' and state['body_count']==15 and state['joint_count']==14,'Actual fifteen-body/fourteen-joint native passive rig')
    k.need([p['name']for p in state['parts']]==[p['name']for p in profile['parts']],'Whole native named ordered physical parts')
    rows=all_nodes(s,tick,session)
    locals=[row['local_transform']for row in rows];worlds=[row['world_matrix']for row in rows]
    # The wrapper follows physics. Treat its current actual transform as the
    # external reference; independently derive every logical skeletal rotation.
    wrapper=s.rpc('runtime.entity',dict(session_id=session,tick=tick,id=f.RIG))
    wrapper_rotation=wrapper['local_transform']['rotation'];frames=k.logical_frames(oracle,(locals,[],worlds))
    for i,local in enumerate(locals):near(local['scale'],frozen_scales[i],0,'Unmodified frozen local source scale '+str(i))
    for actual,part in zip(state['parts'],profile['parts']):
        frame=clone(frames[part['node']]);frame['rotation']=k.normalized(k.quaternion(wrapper_rotation,frame['rotation']))
        expected=k.compose(frame,part['body_in_bone'])
        near(actual['position'],expected['position'],4e-4,'Native physics COM to actual affine skeleton origin '+part['name'])
        k.need(abs(abs(k.dot(actual['rotation'],expected['rotation']))-1)<=2e-5,'Native physics logical-frame orientation '+part['name'])
        k.need(actual['enrolled']is True and all(math.isfinite(v)for field in ['position','rotation','linear_velocity','angular_velocity']for v in actual[field]),'Actual admitted finite enrolled native bodies')
    return state

def archived(s,saves,tick,slot,session):
    before=len(f.REPORT['archives']);value=f.archive(s,saves,tick,slot,session)
    record=f.REPORT['archives'][before];return value,Path(record['path']).read_bytes(),record

# Source-space error bounds are fixed before native execution. The existing
# original FK comparison permits3e-5m matrix error;4e-5m COM error includes a
# metric offset. Differencing two such poses at60Hz yields<=.0048m/s. Aggregate
# momentum includes this conservative uncertainty plus explicit float roundoff.
SOURCE_COM_TOL=4e-5
SOURCE_LINEAR_VELOCITY_TOL=2*SOURCE_COM_TOL*60
SOURCE_ANGULAR_VELOCITY_TOL=.001
DT=1/60.
def angular_velocity(previous,current):
    # Independently use acos plus normalized imaginary-axis, with explicit
    # shortest-arc sign selection. Production uses atan2 of a quaternion sine.
    delta=k.normalized(k.quaternion(current,k.conjugate(previous)))
    if delta[3]<0:delta=[-v for v in delta]
    w=max(-1.,min(1.,delta[3]));axis=delta[:3];length=k.norm(axis)
    if length<1e-9:return k.mul(axis,2/DT)
    return k.mul(axis,2*math.acos(w)/(length*DT))
def source_bodies(oracle,profile,clip,tick):
    pose=oracle.pose(clip,tick/60.);frames=k.logical_frames(oracle,pose)
    wrapper=transform_matrix(**WRAPPER);result=[]
    for part in profile['parts']:
        frame=clone(frames[part['node']]);frame['position']=point(wrapper,frame['position'])
        frame['rotation']=k.normalized(k.quaternion(WRAPPER['rotation'],frame['rotation']))
        result.append(dict(part=part['name'],**k.compose(frame,part['body_in_bone'])))
    return result

def verify_playing_history(archive,oracle,profile,clip,tick):
    rigs=archive['snapshot']['payload']['physical']['rigs']
    k.need(len(rigs)==1 and rigs[0]['entity']==f.RIG,'Complete selected named physical history owner')
    rig=rigs[0];history=rig['pose'];k.need(rig['membership']['mode']=='animated' and rig['membership']['enrolled']is False,'Native allocated Animated membership before takeover')
    k.need(history['current']['tick']==tick and history['previous']['tick']==tick-1 and history['root_bone_to_wrapper']is None,'Two exact consecutive committed played source ticks before takeover')
    checks=[]
    for frame in (history['previous'],history['current']):
        ft=frame['tick'];expected=source_bodies(oracle,profile,clip,ft);before=source_bodies(oracle,profile,clip,ft-1)
        k.need([p['part']for p in frame['bodies']]==[p['name']for p in profile['parts']],'Complete15part final-history source ordering')
        for actual,current,old in zip(frame['bodies'],expected,before):
            near(actual['position'],current['position'],SOURCE_COM_TOL,'Original played source COM '+current['part'])
            k.need(abs(abs(k.dot(actual['rotation'],current['rotation']))-1)<=2e-10,'Original played logical orientation '+current['part'])
            linear=k.mul(k.sub(current['position'],old['position']),1/DT)
            angular=angular_velocity(old['rotation'],current['rotation'])
            near(actual['linear_velocity'],linear,SOURCE_LINEAR_VELOCITY_TOL,'Original played60Hz COM momentum '+current['part'])
            near(actual['angular_velocity'],angular,SOURCE_ANGULAR_VELOCITY_TOL,'Original played shortest quaternion-log momentum '+current['part'])
            checks.append(dict(tick=ft,part=current['part'],source_linear_velocity=linear,source_angular_velocity=angular))
        near(frame['wrapper']['position'],WRAPPER['position'],1e-12,'Constant actual wrapper source reference')
        near(frame['wrapper']['linear_velocity'],[0,0,0],1e-12,'Do not add nonexistent wrapper velocity twice')
        near(frame['wrapper']['angular_velocity'],[0,0,0],1e-12,'Constant wrapper angular reference')
    # Even Idle must contain actual original local movement at this selected
    # interval; no silently frozen clip can pass this source-history witness.
    k.need(any(k.norm(c['source_linear_velocity'])>.001 or k.norm(c['source_angular_velocity'])>.001 for c in checks),'Original selected interval supplies nonzero played motion')
    return checks

def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
def verify_takeover_momentum(state,archive,oracle,profile,clip):
    # First passive Jolt tick includes gravity/drag and internal constraints.
    # Per-body post-solver velocities are not unchanged animation velocities.
    # Sum of internal joint/self-contact impulses is zero linear momentum;
    # reject external contacts/clipping instead of masking them.
    ids={p['body_id']for p in state['parts']};physics=archive['snapshot']['payload']['physical']['physics']
    k.need(all(p['body_a']in ids and p['body_b']in ids for p in physics['pairs']),'First takeover tick must be airborne without external contact pairs')
    k.need(all(not p['at_linear_velocity_limit'] and not p['at_angular_velocity_limit'] for p in state['parts']),'First takeover must not clamp animation or solver momentum')
    current=source_bodies(oracle,profile,clip,6);old=source_bodies(oracle,profile,clip,5)
    velocities=[k.mul(k.sub(a['position'],b['position']),60)for a,b in zip(current,old)]
    mass=sum(p['mass']for p in profile['parts']);initial=[sum(p['mass']*v[i]for p,v in zip(profile['parts'],velocities))for i in range(3)]
    k.need(all(p['linear_damping']==.10 for p in profile['parts']),'Authored common drag is part of this aggregate oracle')
    dt=f32(1/60);gravity=f32(-9.81);drag=f32(1-f32(f32(.10)*dt))
    expected=k.mul(k.add(initial,[0,mass*gravity*dt,0]),drag)
    actual=[sum(p['mass']*p['linear_velocity'][i]for p in state['parts'])for i in range(3)]
    roundoff=128*(2**-23)*max(1,sum(abs(v)for v in initial),sum(abs(v)for v in expected))
    bound=mass*SOURCE_LINEAR_VELOCITY_TOL+roundoff
    near(actual,expected,bound,'Real native takeover aggregate momentum from independent played source')
    return dict(source_tick=6,native_tick=7,source_linear_momentum=initial,expected_after_gravity_drag=expected,actual_native_momentum=actual,
        absolute_per_axis_bound=bound,source_difference_bound=mass*SOURCE_LINEAR_VELOCITY_TOL,float_roundoff_bound=roundoff,
        scope='Aggregate native linear momentum only; individual pre-takeover linear/angular finite differences separately checked against full saved history. Solver may redistribute each part velocity.')

def restore(s,saves,slot,session,existing_session=None,existing_tick=None):
    state=game=None
    if existing_session is None:s.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=f.native(saves)))
    else:
        state=s.rpc('runtime.inspect',dict(session_id=existing_session))
        k.need(state['tick']==existing_tick,'Warm restore current native tick')
        game=s.rpc('runtime.gameplay.inspect',dict(session_id=existing_session,tick=existing_tick))
    receipt=s.rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot=slot,expected_generation=1,revision=1,
        expected_session_id=existing_session,expected_tick=existing_tick,expected_gameplay_revision=game['revision']if game else None,
        expected_structure_revision=state['structure_revision']if state else None,expected_material_revision=state['material_revision']if state else None,
        expected_ui_revision=state['ui_revision']if state else None,expected_control_sequence=state['control_sequence']if state else None,new_session_id=session))
    k.need(receipt.get('session_id',receipt.get('new_session_id'))==session,'Restored exact requested session identity')
    return receipt['tick']

def case(directory,source,oracle,profile,clip,terrain):
    s=f.Session(directory);fresh=None;record=dict(clip=clip,terrain=terrain,passed=False,whole_pairs=[])
    f.REPORT['cases'].append(record)
    try:
        record['source']=prepare(s,source,oracle,profile,clip,terrain);authored=s.world.read_bytes();session=uid()
        s.rpc('runtime.start',dict(session_id=session,revision=1));record['initialized_nodes']=initialized_oracle(s,oracle,clip,session)
        saves=f.save_root(s)
        f.step(s,0,ticks=5,session=session)
        record['played_tick5_nodes']=initialized_oracle(s,oracle,clip,session,5)
        pre5,_,_=archived(s,saves,5,'played-five',session)
        record['played_tick5_history']=verify_playing_history(pre5,oracle,profile,clip,5)
        f.step(s,5,session=session)
        record['played_tick6_nodes']=initialized_oracle(s,oracle,clip,session,6)
        pre6,_,_=archived(s,saves,6,'played-six',session)
        record['played_tick6_history']=verify_playing_history(pre6,oracle,profile,clip,6)
        # Transfer the LAST committed played pose (tick6), with its real
        # source-derived linear/angular momentum, before the tick7 solver.
        f.step(s,6,[dict(entity=f.RIG,activate=True)],session=session)
        takeover=f.query(s,7,session,include_parts=True,include_joints=True)
        post7,_,_=archived(s,saves,7,'takeover-seven',session)
        record['takeover_momentum']=verify_takeover_momentum(takeover,post7,oracle,profile,clip)
        f.step(s,7,ticks=11,session=session);tick=18
        state=physical_seam(s,tick,session,oracle,profile,[r['scale']for r in oracle.pose(clip,0)[0]])
        shoulder=next(p for p in state['parts']if p['name']=='upper_arm_l');point=k.add(shoulder['position'],[.10,.06,-.03])
        f.step(s,tick,[f.impulse('upper_arm_l',point,(2.,.5,-1.))],session=session);tick+=1
        f.step(s,tick,ticks=120,session=session);tick+=120
        fallen=physical_seam(s,tick,session,oracle,profile,[r['scale']for r in oracle.pose(clip,0)[0]])
        k.need(next(p for p in fallen['parts']if p['name']=='pelvis')['position'][1]<3.,'Actual multi-part fall from original elevated initialized character')
        base,base_bytes,_=archived(s,saves,tick,'checkpoint',session)
        f.step(s,tick,ticks=120,session=session);future_tick=tick+120
        future,future_bytes,_=archived(s,saves,future_tick,'future',session)
        warm_session=uid();warm_tick=restore(s,saves,'checkpoint',warm_session,session,future_tick)
        k.need(warm_tick==tick,'Same-owner warm restore native tick')
        warm,warm_bytes,_=archived(s,saves,tick,'warm-restored',warm_session)
        k.need(base_bytes==warm_bytes and f.exact(base,warm),'Complete same-owner warm restore bytes without exclusions')
        record['whole_pairs'].append(dict(kind='warm_restore',bytes=len(base_bytes),sha256=hashlib.sha256(base_bytes).hexdigest()))
        f.step(s,tick,ticks=120,session=warm_session)
        warm_future,warm_future_bytes,_=archived(s,saves,future_tick,'warm-future',warm_session)
        k.need(future_bytes==warm_future_bytes and f.exact(future,warm_future),'Complete same-owner warm future bytes without exclusions')
        record['whole_pairs'].append(dict(kind='warm_future',bytes=len(future_bytes),sha256=hashlib.sha256(future_bytes).hexdigest()))
        s.close()
        fresh=f.Session(directory/'cold-owner',s.world);new_session=uid();restored_tick=restore(fresh,saves,'checkpoint',new_session)
        k.need(restored_tick==tick,'Actual fresh-process native tick')
        restored,restored_bytes,_=archived(fresh,saves,tick,'cold-restored',new_session)
        k.need(base_bytes==restored_bytes and f.exact(base,restored),'Complete original warm/cold native save bytes with no exclusions')
        record['whole_pairs'].append(dict(kind='cold_restore',bytes=len(base_bytes),sha256=hashlib.sha256(base_bytes).hexdigest()))
        f.step(fresh,tick,ticks=120,session=new_session);current=future_tick
        continued,continued_bytes,_=archived(fresh,saves,current,'cold-future',new_session)
        k.need(future_bytes==continued_bytes and f.exact(future,continued),'Complete native future archive bytes after cold continuation')
        record['whole_pairs'].append(dict(kind='cold_future',bytes=len(future_bytes),sha256=hashlib.sha256(future_bytes).hexdigest()))
        # Settling is a real, bounded acceptance condition; a failed profile is
        # retained for tuning rather than relabeled acceptable jitter.
        state=f.query(fresh,current,new_session,include_parts=True,include_joints=True)
        sleeping=all(p['sleeping']is True for p in state['parts'])
        for _ in range(16):
            if sleeping:break
            f.step(fresh,current,ticks=60,session=new_session);current+=60
            state=f.query(fresh,current,new_session,include_parts=True,include_joints=True)
            sleeping=all(p['sleeping']is True for p in state['parts'])
        k.need(sleeping,'All15 actual passive bodies must settle within1200 continuation ticks, including final chunk observation')
        pelvis=next(p for p in state['parts']if p['name']=='pelvis')
        f.step(fresh,current,[f.impulse('pelvis',k.add(pelvis['position'],[0,.12,0]),(3.,1.,0))],session=new_session);current+=1
        awakened=f.query(fresh,current,new_session,include_parts=True,include_joints=True);k.need(any(p['active']is True for p in awakened['parts']),'Later actual hit must wake the settled physical graph')
        physical_seam(fresh,current,new_session,oracle,profile,[r['scale']for r in oracle.pose(clip,0)[0]])
        k.need(s.world.read_bytes()==authored,'Original authored world unchanged by runtime and cold restore')
        record.update(passed=True,final_tick=current)
    finally:
        s.close()
        if fresh is not None:fresh.close()
    return record

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--source',type=Path,required=True);p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    k.need(not a.output.exists(),'Fresh output');a.output.mkdir(parents=True);f.ARGS=a;f.DEADLINE=time.monotonic()+300;f.MAX_CALLS=1024
    f.REPORT=dict(format='poima.physical-character-native.v1',passed=False,calls=[],owners=[],archives=[],cases=[],errors=[],
        scope='Original licensed fifteen-part source/FK/played6tick momentum/native fall/contact/wake/warm+cold continuation. No rendered/humanoid-quality/active-control/controller/performance claim.')
    try:
        oracle,profile,calibration=k.inspect_inputs(a.source,a.license);f.REPORT['calibration']=calibration
        # Both original fixed-scale takes are initialized in independent worlds;
        # each has a different contact surface and retains every original take.
        for clip,terrain in [(36,'stairs'),(48,'slope')]:case(a.output/('clip-'+str(clip)),a.source,oracle,profile,clip,terrain)
        k.need(all(o['natural_exit']and not o['forced_cleanup']and o['exit_code']==0 for o in f.REPORT['owners']),'Every actual native owner must close naturally with EOF0')
        f.REPORT['passed']=True
    except BaseException:f.REPORT['errors'].append(traceback.format_exc())
    (a.output/'evidence.json').write_text(json.dumps(f.REPORT,indent=2,allow_nan=False)+'\n');return 0 if f.REPORT['passed']else 1
if __name__=='__main__':raise SystemExit(main())
