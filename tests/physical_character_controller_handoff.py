#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original Knight locomotion, source crossfades and one-way physical ownership.

Builds/downloads nothing. Run beneath an external process-tree owner. Literal
RPC files, full saves and original source data remain intact. No rendered,
production-humanoid, compiled-gameplay or representative-performance claim.
"""
import argparse,copy,hashlib,json,math,sys,time,traceback,uuid
from pathlib import Path
import physical_character_transitions as t

original,f,k=t.original,t.f,t.k
REPO=t.REPO
SELF=Path(__file__).resolve()
DT=1/60.;SECONDS=360;RPC_LIMIT=2500
ACTOR,CAMERA,OBSERVER,ENV,LIGHT,VISIBLE_FLOOR=(f'{n:032x}'for n in (1,2,700,701,702,750))
IDENTITY=dict(position=[0.,0.,0.],rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])
CONTROLLER=dict(radius=.3,height=1.8,speed=2.,jump_speed=5.,camera=CAMERA,
    stance=dict(crouched_height=1.,crouched_speed=1.,camera_drop=.5),
    locomotion=dict(step_height=.4,ground_snap=.5,max_slope_degrees=45.))
EYE=dict(position=[.05,1.6,.02],rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])
OBSERVER_LOCAL=dict(position=[0.,3.5,9.],rotation=[-.12477422427683947,0.,0.,.9921851606207951],scale=[1.,1.,1.])
PUBLIC_INPUTS=(*t.PUBLIC_INPUTS,'tests/physical_character_transitions.py')
Session=t.Session
uid=lambda:uuid.uuid4().hex


def entity(s,tick,session,identity):
    value=s.rpc('runtime.entity',dict(session_id=session,tick=tick,id=identity))
    k.need(value['id']==identity and value['session_id']==session and type(value['tick'])is int and value['tick']==tick,'Actual entity identity/clock')
    return value


def ownership(s,tick,session):
    value=s.rpc('runtime.controller_handoff',dict(session_id=session,tick=tick,entity=f.RIG))
    k.need(set(value)=={'session_id','tick','source','state'} and value['session_id']==session and value['tick']==tick and value['source']=='runtime','Complete handoff envelope')
    state=value['state']
    k.need(type(state)is dict and set(state)=={'entity','actor','phase','capsule_body_id','capsule_enrolled','takeover_tick','support'} and state['entity']==f.RIG and state['actor']==ACTOR,'Complete original-character ownership membership')
    return state


def prepare(s,a,oracle,profile):
    imported=s.rpc('asset.import',dict(source=f.native(a.source)));asset=imported['asset']
    k.need(type(asset)is str and len(asset)==64 and all(x in '0123456789abcdef'for x in asset),'Safe complete cooked content identity')
    k.need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives'])==(57,76,1,15),'Unchanged original full library import membership')
    package=Path(str(s.world)+'.assets')/(asset+'.pmodel');raw=package.read_bytes()
    k.need(hashlib.sha256(raw).hexdigest()==asset,'Actual entire cooked content address');k.decode_cooked(raw,oracle)
    def create(identity,name,parent=None):return dict(op='entity.create',id=identity,name=name,parent=parent)
    light_rotation=k.quaternion([0.,math.sin(.2),0.,math.cos(.2)],[math.sin(-.4),0.,0.,math.cos(.4)])
    ops=[create(ACTOR,'Original Knight virtual actor'),f.component(ACTOR,'Transform',copy.deepcopy(original.WRAPPER)),
        f.component(ACTOR,'CharacterController',copy.deepcopy(CONTROLLER)),
        dict(op='asset.instantiate',id=f.RIG,name='Original Knight physical library',asset=asset,parent=ACTOR),
        f.component(f.RIG,'Transform',copy.deepcopy(IDENTITY)),
        f.component(f.RIG,'AnimationRig',dict(asset=asset,clip=36,time=0.,speed=1.,loop=False,playing=True)),
        f.component(f.RIG,'PhysicalRig',copy.deepcopy(profile)),
        create(CAMERA,'Owned character eye',ACTOR),f.component(CAMERA,'Transform',copy.deepcopy(EYE)),
        f.component(CAMERA,'Camera',dict(vertical_fov=70,near=.05,far=100)),
        create(f.FLOOR,'Static original flat floor'),
        f.component(f.FLOOR,'Transform',dict(position=[0.,-.25,0.],rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])),
        f.component(f.FLOOR,'BoxCollider',dict(half_extents=[12,.25,12],motion='static',mass=1,friction=.65,restitution=0)),
        create(OBSERVER,'Original-character observer',ACTOR),f.component(OBSERVER,'Transform',copy.deepcopy(OBSERVER_LOCAL)),
        f.component(OBSERVER,'Camera',dict(vertical_fov=45,near=.1,far=100)),
        create(ENV,'Original-character environment'),f.component(ENV,'Transform',copy.deepcopy(IDENTITY)),
        f.component(ENV,'LightingEnvironment',dict(ambient=[.12,.12,.12],exposure=1)),
        create(LIGHT,'Original-character directional light'),
        f.component(LIGHT,'Transform',dict(position=[0.,4.,2.],rotation=light_rotation,scale=[1.,1.,1.])),
        f.component(LIGHT,'Light',dict(kind='directional',color=[1,.97,.92],intensity=2.5,enabled=True,
            shadow=dict(enabled=True,near=.05,distance=30,bias=.001,normal_bias=.01))),
        create(VISIBLE_FLOOR,'Visible matching flat floor'),
        f.component(VISIBLE_FLOOR,'Transform',dict(position=[0.,-.25,0.],rotation=[0.,0.,0.,1.],scale=[24.,.5,24.])),
        f.component(VISIBLE_FLOOR,'MeshRenderer',dict(primitive='box',albedo=[.25,.27,.29],visible=True))]
    k.need(f.transact(s,ops,0)['revision']==1,'Complete original actor/library/observer content committed revision1')
    return dict(asset=asset,package=t.identity(package),world=t.identity(s.world),clip=36,terrain='flat',actor=ACTOR,camera=CAMERA,observer=OBSERVER)


def input_for(held=False):
    return dict(entity=ACTOR,move=[-.75,.5]if held else[.35,.65],look=[37.,21.]if held else[.5,-.125],jump=held,use=False)


def step(s,tick,session,ticks=1,animations=(),activate=False,held=None,crouch=None):
    params=dict(session_id=session,request_id=uid(),expected_tick=tick,ticks=ticks,animations=list(animations))
    if held is not None:params['inputs']=[input_for(held)]
    if crouch is not None:params['character_stances']=[dict(entity=ACTOR,crouch=crouch)]
    if activate:params['physical_rigs']=[dict(entity=f.RIG,activate=True)]
    value=s.rpc('runtime.step',params)
    k.need(type(value['tick'])is int and value['tick']==tick+ticks,'Actual committed Tick differs from frozen source/input schedule')
    return tick+ticks


class ActorFrames:
    def __init__(self):self.frames={}
    def read(self,s,tick,session,label,animated):
        value=entity(s,tick,session,ACTOR)
        f.REPORT['actor_observations'].append(dict(label=label,tick=tick,actor=value))
        if animated:
            if tick not in self.frames:
                self.frames[tick]=copy.deepcopy(value);f.REPORT['actor_frames'][str(tick)]=copy.deepcopy(value)
            else:
                for field in ('world_matrix','local_transform','velocity','yaw','pitch','ground','locomotion'):
                    k.need(f.exact(value[field],self.frames[tick][field]),'Selected actual actor observation differs across original/restored motion: '+field)
            initial=self.frames[0]
            t.scalar_near(value['yaw'],math.remainder(initial['yaw']+.5*tick,360),1e-10,'Literal native actor yaw schedule')
            t.scalar_near(value['pitch'],-.125*tick,1e-12,'Literal native actor pitch schedule')
        return value


def advance_animated(s,start,end,session,frames):
    for tick in range(start+1,end+1):
        commands=[t.command(48)]if tick==7 else[t.command(25)]if tick==19 else[]
        step(s,tick-1,session,animations=commands,held=False,crouch=True if tick==13 else None)
        frames.read(s,tick,session,'committed-motion-'+str(tick),True)
    return end


def source_bodies(schedule,profile,tick,actor):
    # Source local frames are independent GLB samples. Only their real resolved
    # actor transform comes from the native controller, never the animation API.
    frames=k.logical_frames(schedule.oracle,schedule.pose(tick));world=actor['world_matrix'];q=actor['local_transform']['rotation']
    values=[]
    for part in profile['parts']:
        bone=copy.deepcopy(frames[part['node']]);bone['position']=t.point(world,bone['position'])
        bone['rotation']=k.normalized(k.quaternion(q,bone['rotation']))
        values.append(dict(part=part['name'],**k.compose(bone,part['body_in_bone'])))
    return values


def observe(s,tick,session,schedule,profile,frames,phase,passive,label,anchor=None):
    expected=schedule.pose(tick);rows=original.all_nodes(s,tick,session)
    actor=frames.read(s,tick,session,label,not passive);wrapper=entity(s,tick,session,f.RIG)
    camera=entity(s,tick,session,CAMERA);observer=entity(s,tick,session,OBSERVER)
    t.compare_clock(wrapper['animation'],schedule.clock(tick,phase));state=f.query(s,tick,session,include_parts=True,include_joints=True)
    handoff=ownership(s,tick,session)
    k.need(state['mode']==('passive'if passive else'animated') and state['body_count']==15 and state['joint_count']==14 and len(state['parts'])==15 and len(state['joints'])==14,'Actual complete original15part/14joint graph')
    k.need([p['name']for p in state['parts']]==[p['name']for p in profile['parts']] and len({p['body_id']for p in state['parts']})==15,'Ordered unique original native part identities')
    k.need([j['name']for j in state['joints']]==[j['name']for j in profile['joints']],'Ordered original native joint identities')
    k.need(all(set(p)==f.PART_FIELDS for p in state['parts'])and all(set(j)==f.JOINT_FIELDS for j in state['joints']),'Complete native part/joint result fields')
    k.need(handoff['phase']==('physical'if passive else'locomotion')and handoff['capsule_enrolled']is(not passive),'Actual motor ownership phase and capsule membership')
    mapped={p['node']for p in profile['parts']};scales=[]
    for index,(row,wanted)in enumerate(zip(rows,expected[0])):
        k.need(row['id']==f.node(index)and row['session_id']==session and row['tick']==tick,'Original model node query identity')
        actual=row['local_transform']['scale'];distances=[t.ulps(a,b)for a,b in zip(actual,wanted['scale'])]
        k.need(len(actual)==3 and all(d<=t.SCALE_ULPS for d in distances),'Independent raw-source scale exceeds fixed4ULP arithmetic bound')
        k.need(all(abs(a-b)<=t.BOUND*b for a,b in zip(actual,schedule.initial[index]['scale'])),'Original node scale left authored bounded envelope')
        scales.append(dict(node=index,actual=copy.deepcopy(actual),source_expected=copy.deepcopy(wanted['scale']),ulp_distance=distances))
        if not passive:k.near(row['world_matrix'],t.multiply(actor['world_matrix'],expected[2][index]),t.MATRIX_TOL,'Actual actor times independent source FK node '+str(index))
        elif index not in mapped:
            k.near(row['local_transform']['position'],wanted['position'],t.MATRIX_TOL,'Unmapped original local source position')
            k.need(abs(abs(k.dot(row['local_transform']['rotation'],wanted['rotation']))-1)<=2e-10,'Unmapped original local source rotation')
    k.need(len(rows)==57 and len(scales)==57,'Complete57-node composition')
    for actual,local in ((camera,EYE),(observer,OBSERVER_LOCAL)):
        selected=actual['local_transform']
        if actual is observer:k.need(f.exact(selected,local),'Frozen observer local transform')
        k.near(actual['world_matrix'],t.multiply(actor['world_matrix'],t.transform_matrix(**selected)),t.MATRIX_TOL,'Actual actor/child-camera world composition')
    if passive:
        k.need(anchor is not None and handoff['takeover_tick']==22 and handoff['capsule_body_id']==anchor['handoff']['capsule_body_id'],'Committed original takeover tick and retained capsule identity')
        for field in ('yaw','pitch','velocity','ground','locomotion'):
            k.need(f.exact(actor[field],anchor['actor'][field]),'Suppressed original controller observation changed: '+field)
        k.need(f.exact(camera['local_transform'],anchor['camera']['local_transform']),'Suppressed input changed original owned-camera local pose')
        heading=anchor['actor']['world_matrix'];actual=actor['world_matrix']
        k.near([actual[c*4+r]for c in range(3)for r in range(3)],[heading[c*4+r]for c in range(3)for r in range(3)],1e-6,'Captured actor world heading')
        offset=k.sub(anchor['actor']['world_matrix'][12:15],anchor['wrapper']['world_matrix'][12:15])
        k.near(actor['world_matrix'][12:15],k.add(wrapper['world_matrix'][12:15],offset),1e-7,'Actual actor follows solved original wrapper translation')
        logical=k.logical_frames(schedule.oracle,([r['local_transform']for r in rows],[],[r['world_matrix']for r in rows]))
        wrapper_q=k.normalized(k.quaternion(actor['local_transform']['rotation'],wrapper['local_transform']['rotation']))
        for body,part in zip(state['parts'],profile['parts']):
            bone=copy.deepcopy(logical[part['node']]);bone['rotation']=k.normalized(k.quaternion(wrapper_q,bone['rotation']))
            target=k.compose(bone,part['body_in_bone']);k.near(body['position'],target['position'],4e-4,'Actual physical COM/affine original skeleton join')
            k.need(abs(abs(k.dot(body['rotation'],target['rotation']))-1)<=2e-5 and body['enrolled']is True,'Actual physical logical axes/native membership')
    record=dict(label=label,tick=tick,phase=phase,passive=passive,all_nodes=rows,wrapper=wrapper,actor=actor,camera=camera,observer=observer,
        physical=state,handoff=handoff,scales=scales,source_clock=schedule.clock(tick,phase))
    f.REPORT['observations'].append(record);return record


def archive(s,saves,tick,slot,session,observation):
    value,raw,record=original.archived(s,saves,tick,slot,session)
    entities=value['snapshot']['payload']['entities'];by_id={e['id']:e for e in entities}
    k.need(len(by_id)==len(entities),'Unique complete saved entity membership')
    for index,row in enumerate(observation['all_nodes']):
        k.need(f.exact(by_id[f.node(index)]['local'],row['local_transform']),'Exact native/saved original57-node local TRS witness')
    for name in ('actor','camera','observer','wrapper'):
        row=observation[name];k.need(f.exact(by_id[row['id']]['local'],row['local_transform']),'Exact native/saved ownership/camera local witness')
    physical=value['snapshot']['payload']['physical'];k.need(physical['version']==2 and len(physical['rigs'])==1,'Original linked snapshot extension2')
    rig=physical['rigs'][0];k.need(rig['entity']==f.RIG and rig['handoff']['actor']==ACTOR,'Saved complete original ownership graph')
    handoff=observation['handoff'];passive=rig['handoff']['passive']
    k.need((passive is not None)is observation['passive'],'Saved original ownership phase')
    if passive is not None:
        k.need(passive['proxy']['body_id']==handoff['capsule_body_id']and passive['takeover_tick']==handoff['takeover_tick'],'Saved/native exact original capsule allocation')
        k.need(not any(b['id']==handoff['capsule_body_id']for b in physical['physics']['bodies']),'Suppressed original capsule remains in native enrolled-body state')
    call=f.REPORT['calls'][-1]
    k.need(call['request']['method']=='save.write'and call['request']['params']['slot']==slot,'Selected actual archive RPC receipt')
    manifest_path=saves/('slot-'+slot)/'current.json';manifest=strict_json(manifest_path.read_bytes())
    record.update(receipt=copy.deepcopy(call['response']['result']),manifest=manifest,manifest_path=str(manifest_path),
        manifest_identity=t.identity(manifest_path),observation=copy.deepcopy(observation))
    t.output_bound(f.ARGS.output);return value,raw,record


def verify_history(value,schedule,profile,frames,tick):
    rig=value['snapshot']['payload']['physical']['rigs'][0];history=rig['pose']
    k.need(rig['membership']['mode']=='animated'and rig['membership']['enrolled']is False and rig['handoff']['passive']is None,'Committed Animated locomotion ownership before handoff')
    k.need(history['current']['tick']==tick and history['previous']['tick']==tick-1 and history['root_bone_to_wrapper']is None,'Exact two committed original source/actor history ticks')
    checks=[]
    for frame in (history['previous'],history['current']):
        ft=frame['tick'];now=source_bodies(schedule,profile,ft,frames.frames[ft]);before=source_bodies(schedule,profile,ft-1,frames.frames[ft-1])
        k.need([p['part']for p in frame['bodies']]==[p['name']for p in profile['parts']],'Complete ordered original15part history')
        for actual,current,old in zip(frame['bodies'],now,before):
            linear=k.mul(k.sub(current['position'],old['position']),1/DT);angular=original.angular_velocity(old['rotation'],current['rotation'])
            k.near(actual['position'],current['position'],t.COM_TOL,'Independent actor/source historical COM')
            k.need(abs(abs(k.dot(actual['rotation'],current['rotation']))-1)<=2e-10,'Independent actor/source logical orientation')
            k.near(actual['linear_velocity'],linear,t.VELOCITY_TOL,'Independent actor/source consecutive COM velocity')
            k.near(actual['angular_velocity'],angular,t.ANGULAR_TOL,'Independent actor/source quaternion-log velocity')
            checks.append(dict(tick=ft,part=current['part'],source_position=current['position'],source_rotation=current['rotation'],source_linear_velocity=linear,source_angular_velocity=angular))
        actor=frames.frames[ft];old=frames.frames[ft-1]
        k.near(frame['wrapper']['position'],actor['world_matrix'][12:15],1e-12,'Identity rig wrapper inherits actual moving actor')
        k.near(frame['wrapper']['linear_velocity'],k.mul(k.sub(actor['world_matrix'][12:15],old['world_matrix'][12:15]),1/DT),1e-8,'Moving wrapper history already includes motor motion')
    return checks


def takeover_momentum(state,value,schedule,profile,frames):
    ids={p['body_id']for p in state['parts']};physics=value['snapshot']['payload']['physical']['physics']
    k.need(all(p['body_a']in ids and p['body_b']in ids for p in physics['pairs']),'Original first passive Tick must have no external contacts')
    k.need(all(not p['at_linear_velocity_limit']and not p['at_angular_velocity_limit']for p in state['parts']),'Original handoff must not clip native velocities')
    now=source_bodies(schedule,profile,22,frames.frames[22]);old=source_bodies(schedule,profile,21,frames.frames[21])
    velocities=[k.mul(k.sub(a['position'],b['position']),1/DT)for a,b in zip(now,old)]
    mass=sum(p['mass']for p in profile['parts']);initial=[sum(p['mass']*v[i]for p,v in zip(profile['parts'],velocities))for i in range(3)]
    k.need(all(p['linear_damping']==.10 for p in profile['parts']),'Unchanged original common drag')
    dt=original.f32(DT);gravity=original.f32(-9.81);drag=original.f32(1-original.f32(original.f32(.10)*dt))
    expected=k.mul(k.add(initial,[0,mass*gravity*dt,0]),drag)
    actual=[sum(p['mass']*p['linear_velocity'][i]for p in state['parts'])for i in range(3)]
    roundoff=128*2**-23*max(1,sum(abs(v)for v in initial),sum(abs(v)for v in expected));bound=mass*t.VELOCITY_TOL+roundoff
    k.near(actual,expected,bound,'Original moving/falling actor plus source blend aggregate momentum, without extra motor velocity')
    return dict(source_tick=22,native_tick=23,source_linear_momentum=initial,expected_after_gravity_drag=expected,actual_native_momentum=actual,
        absolute_per_axis_bound=bound,source_difference_bound=mass*t.VELOCITY_TOL,float_roundoff_bound=roundoff,
        last_resolved_motor_velocity=frames.frames[22]['velocity'],scope='Aggregate linear momentum from actor-composed source COM finite differences. Native motor velocity is recorded, never added again.')


def ray(s,tick,session,origin,direction,distance,ignore):
    return s.rpc('runtime.raycast',dict(session_id=session,tick=tick,origin=origin,direction=direction,distance=distance,ignore=list(ignore)))


def continuation(s,saves,base,future,session,start,end,schedule,profile,frames,phase,prefix,anchor=None):
    def inspect(owner,current,identity,label):return observe(owner,current,identity,schedule,profile,frames,phase,anchor is not None,label,anchor)
    warm=uid();k.need(original.restore(s,saves,base[2]['slot'],warm,session,end)==start,'Warm restore exact source/motor Tick')
    observed=inspect(s,start,warm,prefix+'-warm-restored');t.whole(base,archive(s,saves,start,prefix+'-warm-restored',warm,observed),prefix+'-warm-restore')
    if anchor is None:advance_animated(s,start,end,warm,frames)
    else:step(s,start,warm,end-start)
    observed=inspect(s,end,warm,prefix+'-warm-future');t.whole(future,archive(s,saves,end,prefix+'-warm-future',warm,observed),prefix+'-warm-future')
    t.world_witness(s,prefix+'-before-natural-close');s.close();t.world_witness(s,prefix+'-after-natural-close')
    cold=Session(s.directory/('cold-'+prefix),s.world)
    try:
        t.world_witness(cold,prefix+'-fresh-original-world');identity=uid()
        k.need(original.restore(cold,saves,base[2]['slot'],identity)==start,'Fresh-process exact source/motor Tick')
        observed=inspect(cold,start,identity,prefix+'-cold-restored');t.whole(base,archive(cold,saves,start,prefix+'-cold-restored',identity,observed),prefix+'-cold-restore')
        if anchor is None:advance_animated(cold,start,end,identity,frames)
        else:step(cold,start,identity,end-start,held=True,crouch=False)
        observed=inspect(cold,end,identity,prefix+'-cold-future');t.whole(future,archive(cold,saves,end,prefix+'-cold-future',identity,observed),prefix+'-cold-future')
    except BaseException:cold.close();raise
    return cold,identity


def run(a,oracle,profile,schedule):
    s=Session(a.output/'shared-world');frames=ActorFrames()
    try:
        f.REPORT['prepared']=prepare(s,a,oracle,profile)
        package=Path(str(s.world)+'.assets')/(f.REPORT['prepared']['asset']+'.pmodel')
        f.REPORT['authored_identity']=dict(world=t.identity(s.world),package=t.identity(package));authored=s.world.read_bytes()
        t.world_witness(s,'initial-original-world');session=uid();s.rpc('runtime.start',dict(session_id=session,revision=1));saves=f.save_root(s)
        f.REPORT['artifacts']=dict(world=str(s.world),asset_package=str(package),saves_root=str(saves),
            authored_revision=1,save_configuration_generation=1,render_observer=OBSERVER)
        observe(s,0,session,schedule,profile,frames,'idle',False,'initial-original-actor')
        advance_animated(s,0,6,session,frames);idle=observe(s,6,session,schedule,profile,frames,'idle',False,'idle-moving')
        archive(s,saves,6,'idle-moving',session,idle)
        advance_animated(s,6,12,session,frames);mid=observe(s,12,session,schedule,profile,frames,'run',False,'run-mid')
        base=archive(s,saves,12,'run-mid',session,mid);f.REPORT['run_mid_history']=verify_history(base[0],schedule,profile,frames,12)
        advance_animated(s,12,18,session,frames);future=observe(s,18,session,schedule,profile,frames,'run',False,'run-complete')
        future_save=archive(s,saves,18,'run-future',session,future)
        s,session=continuation(s,saves,base,future_save,session,12,18,schedule,profile,frames,'run','run')
        advance_animated(s,18,21,session,frames);observe(s,21,session,schedule,profile,frames,'death',False,'death-prev')
        advance_animated(s,21,22,session,frames)
        anchor=observe(s,22,session,schedule,profile,frames,'death',False,'death-mid');death_mid=archive(s,saves,22,'death-mid',session,anchor)
        f.REPORT['takeover_history']=verify_history(death_mid[0],schedule,profile,frames,22)
        first=frames.frames[0];last=anchor['actor'];delta=k.sub(last['world_matrix'][12:15],first['world_matrix'][12:15])
        k.need(math.hypot(delta[0],delta[2])>.2 and delta[1]<-.1 and last['ground']=='in_air'and last['velocity'][1]<-1,'Actual original actor must move, turn and remain airborne while falling at takeover')
        t.scalar_near(last['yaw']-first['yaw'],11.,1e-10,'Actual nonzero moderate native turn');t.scalar_near(last['pitch'],-2.75,1e-12,'Actual pre-death pitch')
        actor_saved={e['id']:e for e in death_mid[0]['snapshot']['payload']['entities']}[ACTOR]
        k.need(actor_saved['controller']['crouched']is True and actor_saved['controller']['requested_crouch']is True,'Actual pre-death native crouched shape')
        motor=actor_saved['controller']['motor']['position'];capsule_origin=k.add(motor,[0.,.5,2.])
        before=ray(s,22,session,capsule_origin,[0.,0.,-1.],4.,[f.RIG,f.FLOOR])
        k.need(before['hit']is not None and before['hit']['entity']==ACTOR,'Actual isolated capsule ray before ownership transfer')
        f.REPORT['capsule_ray']=dict(origin=capsule_origin,direction=[0.,0.,-1.],distance=4.,ignore=[f.RIG,f.FLOOR],before=before)
        step(s,22,session,activate=True);neutral=observe(s,23,session,schedule,profile,frames,'death',True,'neutral-takeover',anchor)
        neutral_save=archive(s,saves,23,'neutral-takeover',session,neutral)
        step(s,23,session,4);neutral_mid=observe(s,27,session,schedule,profile,frames,'death',True,'neutral-passive-mid',anchor)
        neutral_mid_save=archive(s,saves,27,'neutral-mid',session,neutral_mid)
        held_session=uid();k.need(original.restore(s,saves,'death-mid',held_session,session,27)==22,'Restore original exact pre-takeover branch')
        restored=observe(s,22,held_session,schedule,profile,frames,'death',False,'held-branch-origin')
        t.whole(death_mid,archive(s,saves,22,'held-branch-origin',held_session,restored),'held-origin-complete-restore');session=held_session
        step(s,22,session,activate=True,held=True,crouch=False)
        held=observe(s,23,session,schedule,profile,frames,'death',True,'held-takeover',anchor)
        held_save=archive(s,saves,23,'held-takeover',session,held);t.whole(neutral_save,held_save,'held-neutral-complete-takeover')
        f.REPORT['takeover_momentum']=takeover_momentum(held['physical'],held_save[0],schedule,profile,frames)
        after=ray(s,23,session,capsule_origin,[0.,0.,-1.],4.,[f.RIG,f.FLOOR]);k.need(after['hit']is None,'Actual removed capsule remains a query obstacle')
        f.REPORT['capsule_ray']['after']=after
        pelvis=next(p for p in held['physical']['parts']if p['name']==profile['root'])
        part_ray=ray(s,23,session,k.add(pelvis['position'],[2.,0.,0.]),[-1.,0.,0.],4.,[f.FLOOR])
        k.need(part_ray['hit']is not None and part_ray['hit']['entity']==f.RIG,'Actual articulated rig absent from native query geometry');f.REPORT['part_ray']=part_ray
        for tick in range(24,28):step(s,tick-1,session,held=True,crouch=False)
        mid=observe(s,27,session,schedule,profile,frames,'death',True,'held-passive-mid',anchor)
        base=archive(s,saves,27,'held-mid',session,mid);t.whole(neutral_mid_save,base,'held-neutral-complete-passive-future')
        end,selection=schedule.later_changed_scale_tick();f.REPORT['later_scale_selection']=selection
        step(s,27,session,end-27,held=True,crouch=False);future=observe(s,end,session,schedule,profile,frames,'death',True,'passive-varying-source-future',anchor)
        k.need(any(t.ulps(a,b)>t.SCALE_ULPS for old,new in zip(held['all_nodes'],future['all_nodes'])for a,b in zip(old['local_transform']['scale'],new['local_transform']['scale'])),'Original composed scales were frozen after takeover')
        future_save=archive(s,saves,end,'passive-future',session,future)
        s,session=continuation(s,saves,base,future_save,session,27,end,schedule,profile,frames,'death','passive',anchor)
        k.need(s.world.read_bytes()==authored,'Original authored content changed during controller/physical workflow')
        k.need(len(f.REPORT['archives'])==18 and len(f.REPORT['whole_pairs'])==11,'Complete predeclared archive/pair population')
        f.REPORT.update(final_tick=end,authored_world_unchanged=True,actor_displacement_before_takeover=delta)
    finally:
        s.close()
        if'authored_identity'in f.REPORT:t.world_witness(s,'final-original-world-after-natural-close')


def strict_json(raw):
    def pairs(values):
        result={}
        for key,value in values:k.need(key not in result,'Duplicate key in literal recorded JSON');result[key]=value
        return result
    def constant(value):raise AssertionError('Nonfinite literal JSON constant '+value)
    return json.loads(raw,object_pairs_hook=pairs,parse_constant=constant)


def audit_recordings():
    f.REPORT['recordings']=[];joined=0
    for owner in f.REPORT['owners']:
        k.need(owner.get('natural_exit')is True and owner.get('forced_cleanup')is False and owner.get('exit_code')==0 and owner.get('reader_failure')is None,'Owner must naturally close before literal transcript audit')
        directory=Path(owner['directory']);calls=[row for row in f.REPORT['calls']if Path(row['owner'])==directory]
        for name,field in (('requests','request'),('responses','response')):
            path=directory/(name+'.jsonl');raw=path.read_bytes();k.need(len(raw)<=f.MAX_RECORDED,'Bounded original transcript')
            rows=[strict_json(line)for line in raw.splitlines()if line.strip()]
            k.need(f.exact(rows,[row[field]for row in calls]),'Complete literal '+name+' differ from immutable recorded history')
            f.REPORT['recordings'].append(dict(owner=str(directory),kind=name,path=str(path),identity=t.identity(path),rows=len(rows)))
        joined+=len(calls)
    k.need(joined==len(f.REPORT['calls']),'Every retained RPC belongs to exactly one closed native owner')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--source',type=Path,required=True)
    p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    a.binary=a.binary.resolve();a.source=a.source.resolve();a.license=a.license.resolve();a.output=a.output.resolve()
    k.need(not a.output.exists(),'Fresh output required');a.output.mkdir(parents=True)
    f.ARGS=a;f.DEADLINE=time.monotonic()+SECONDS;f.MAX_CALLS=RPC_LIMIT
    f.REPORT=dict(format='poima.physical-character-controller-handoff.v1',passed=False,calls=[],owners=[],archives=[],whole_pairs=[],observations=[],actor_observations=[],actor_frames={},world_identity_joins=[],errors=[],
        bounds=dict(seconds=SECONDS,rpc=RPC_LIMIT,observed_output_bytes=t.OUTPUT_LIMIT,scale_ulp_distance=t.SCALE_ULPS,authored_relative_scale_bound=t.BOUND),
        scope='One unchanged original57node/76take library with actual moving/falling virtual actor, source-composed blend/COM history, passive transfer, native queries and full warm/cold saves. No rendered/production-humanoid/compiled-gameplay/performance or cross-platform equality claim.')
    inputs={}
    try:
        inputs={str(path):t.identity(path)for path in (SELF,a.binary,a.source,a.license,*[REPO/name for name in PUBLIC_INPUTS])};f.REPORT['inputs']=inputs
        oracle,original_profile,calibration=k.inspect_inputs(a.source,a.license);profile=copy.deepcopy(original_profile)
        profile.update(pose_frame='skeletal_bounded_scale',max_scale_deviation=t.BOUND,controller_handoff=dict(actor=ACTOR,follow='translate_keep_heading'))
        old=copy.deepcopy(profile);old.pop('max_scale_deviation');old.pop('controller_handoff');old['pose_frame']='skeletal_fixed_scale'
        k.need(f.exact(old,original_profile),'Original physical dimensions/masses/joints unchanged except explicit scale frame and controller link')
        f.REPORT.update(calibration=calibration,authored_profile=profile,actor_profile=copy.deepcopy(CONTROLLER),actor_initial_transform=copy.deepcopy(original.WRAPPER))
        schedule=t.SourceSchedule(oracle);schedule.later_changed_scale_tick();run(a,oracle,profile,schedule)
    except BaseException:f.REPORT['errors'].append(traceback.format_exc())
    try:
        audit_recordings()
        k.need(len(f.REPORT['owners'])==3,'Exactly three sequential native owners')
        for path,witness in inputs.items():k.need(t.identity(path)==witness,'Selected source/native/helper changed during workflow')
    except BaseException:f.REPORT['errors'].append(traceback.format_exc())
    f.REPORT['passed']=not f.REPORT['errors'];f.REPORT['rpc_count']=len(f.REPORT['calls'])
    f.REPORT['output_census']=t.output_bound(a.output)
    encoded=(json.dumps(f.REPORT,indent=2,allow_nan=False)+'\n').encode()
    k.need(len(encoded)<=t.EVIDENCE_RESERVE and f.REPORT['output_census']['bytes']+len(encoded)<=t.OUTPUT_LIMIT,'Complete final evidence exceeds reserved128MiB output')
    (a.output/'evidence.json').write_bytes(encoded);print(a.output/'evidence.json',flush=True)
    return 0 if f.REPORT['passed']else 1
if __name__=='__main__':raise SystemExit(main())
