#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original Knight shared-world crossfades, bounded scales and complete cold saves.

Builds/downloads nothing. Requires external owned process-tree supervision.
An independent raw-source clock/TRS oracle supplies expected values; no engine
animation sampler supplies expected poses. Source alone is not passing evidence.
"""
import argparse,copy,functools,hashlib,json,math,os,stat,struct,sys,time,traceback,uuid
from pathlib import Path
SELF=Path(__file__).resolve()
parents=list(SELF.parents)
REPO=next((p for p in parents if (p/'.git').exists() and (p/'tests/physical_character_native.py').is_file()),None)
if REPO is None:
    REPO=next(p for p in parents if (p/'CMakeLists.txt').is_file() and (p/'tests/physical_character_native.py').is_file() and (p/'examples/physical-character-yard/knight_profile.py').is_file())
sys.path.insert(0,str(REPO/'tests'))
import physical_character_native as original
from imported_character_contract import transform_matrix,multiply,point,interpolate_rotation
f,k=original.f,original.k
DT=1/60.;SECONDS=240;RPC_LIMIT=2000;MIB=1024*1024;OUTPUT_LIMIT=128*MIB;EVIDENCE_RESERVE=16*MIB
BOUND=1e-5;SCALE_ULPS=4
# Fixed independent source-oracle tolerances selected before native execution.
# Scale arithmetic uses an ULP bound, not the physical admission envelope. All
# native node/saved-scale joins and complete archive pairs below remain EXACT.
MATRIX_TOL=3e-5;COM_TOL=4e-5;VELOCITY_TOL=2*COM_TOL/DT;ANGULAR_TOL=.001
WRAPPER=copy.deepcopy(original.WRAPPER)
PUBLIC_INPUTS=('tests/physical_character_native.py','tests/world_physical_contract.py','tests/character_library_intake.py',
    'tests/imported_character_contract.py','tests/animation_fixture.py','tests/gltf_fixture.py',
    'examples/physical-character-yard/knight_profile.py','examples/physical-character-yard/profiles/knight.json',
    'examples/physical-character-yard/source-manifest.json')


def uid():return uuid.uuid4().hex

def identity(path):
    path=Path(path);before=path.lstat();k.need(stat.S_ISREG(before.st_mode),'Ordinary selected input/output')
    with path.open('rb')as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    after=path.lstat();k.need((before.st_size,before.st_mtime_ns,before.st_ino)==(after.st_size,after.st_mtime_ns,after.st_ino),'File identity changed during read')
    return dict(bytes=after.st_size,sha256=digest)


def output_bound(root,reserve=EVIDENCE_RESERVE):
    total=members=0
    for base,dirs,names in os.walk(root,followlinks=False,onerror=lambda e:(_ for _ in()).throw(e)):
        for name in dirs+names:
            path=Path(base)/name;s=path.lstat();members+=1
            k.need(members<=8192 and not stat.S_ISLNK(s.st_mode) and not getattr(s,'st_file_attributes',0)&0x400,'Ordinary bounded output membership')
            if stat.S_ISDIR(s.st_mode):continue
            k.need(stat.S_ISREG(s.st_mode),'Ordinary output file');total+=s.st_size
            k.need(total<=OUTPUT_LIMIT-reserve,'Observed output bytes exceed reserved128MiB budget')
    return dict(bytes=total,members=members,reserve=reserve,scope='Observed census, not an OS storage quota')


class Session(f.Session):
    def rpc(self,*args,**kwargs):
        result=super().rpc(*args,**kwargs)
        if len(f.REPORT['calls'])%64==0:output_bound(f.ARGS.output)
        return result


def ulps(a,b):
    k.need(type(a)in(int,float) and type(b)in(int,float) and math.isfinite(a) and math.isfinite(b) and a>0 and b>0,'Positive numeric source/native scale components')
    return abs(struct.unpack('<Q',struct.pack('<d',a))[0]-struct.unpack('<Q',struct.pack('<d',b))[0])


def pose_from_locals(oracle,locals):
    mats=[transform_matrix(**value)for value in locals];worlds={}
    def at(index):
        if index not in worlds:worlds[index]=multiply(at(oracle.parents[index]),mats[index])if index in oracle.parents else mats[index]
        return worlds[index]
    return locals,mats,[at(i)for i in range(57)]


class SourceSchedule:
    """Literal command schedule: Idle0; Run@6/12ticks; Death_B@18/12ticks.

    No interrupted fade, rate, layer or inertial inference is hidden here. The
    second fade begins only when the first has finished. Clocks are independent
    source clocks, each speed1,nonlooping,time0 at its command anchor.
    """
    def __init__(self,oracle):
        self.oracle=oracle
        k.need([(oracle.clips[i]['name'])for i in (36,48,25)]==['Idle','Running_A','Death_B'],'Literal original take identities')
        self.initial=copy.deepcopy(oracle.pose(36,0)[0])
    def time(self,clip,ticks):return min(max(0.,ticks*DT),self.oracle.clips[clip]['duration'])
    def sample(self,clip,ticks):return self.oracle.pose(clip,self.time(clip,ticks))
    def mix(self,left,right,weight):
        if weight==0:return left
        if weight==1:return right
        values=[]
        for a,b in zip(left[0],right[0]):
            values.append(dict(position=[math.fsum((x*(1-weight),y*weight))for x,y in zip(a['position'],b['position'])],
                rotation=interpolate_rotation(a['rotation'],b['rotation'],weight),
                scale=[math.fsum((x*(1-weight),y*weight))for x,y in zip(a['scale'],b['scale'])]))
        return pose_from_locals(self.oracle,values)
    @functools.lru_cache(maxsize=64)
    def pose(self,tick):
        k.need(type(tick)is int and tick>=0,'Independent source Tick domain')
        if tick<=6:return self.sample(36,tick)
        if tick<18:return self.mix(self.sample(36,tick),self.sample(48,tick-6),(tick-6)/12)
        if tick==18:return self.sample(48,12)
        if tick<30:return self.mix(self.sample(48,tick-6),self.sample(25,tick-18),(tick-18)/12)
        return self.sample(25,tick-18)
    def clock(self,tick,phase):
        if phase=='idle':clip,anchor,source,source_anchor=36,0,None,0
        elif phase=='run':clip,anchor,source,source_anchor=48,6,36,0
        else:k.need(phase=='death','Known source phase');clip,anchor,source,source_anchor=25,18,48,6
        elapsed=tick-anchor;duration=self.oracle.clips[clip]['duration'];time=self.time(clip,elapsed)
        expected=dict(clip=clip,time=time,speed=1.,loop=False,playing=time<duration,duration=duration)
        if source is not None and elapsed<12:
            source_time=self.time(source,tick-source_anchor)
            expected['transition']=dict(start_tick=anchor,duration_ticks=12,elapsed_ticks=elapsed,weight=elapsed/12,
                source_frozen=False,source_clip=source,source_time=source_time,source_speed=1.,source_loop=False,
                source_playing=source_time<self.oracle.clips[source]['duration'])
        else:expected['transition']=None
        return expected
    def bodies(self,profile,tick):
        frames=k.logical_frames(self.oracle,self.pose(tick));wrapper=transform_matrix(**WRAPPER);result=[]
        for part in profile['parts']:
            frame=copy.deepcopy(frames[part['node']]);frame['position']=point(wrapper,frame['position'])
            frame['rotation']=k.normalized(k.quaternion(WRAPPER['rotation'],frame['rotation']))
            result.append(dict(part=part['name'],**k.compose(frame,part['body_in_bone'])))
        return result
    def later_changed_scale_tick(self):
        end=18+math.ceil(self.oracle.clips[25]['duration']*60)
        k.need(30<=end<=318,'Bounded original Death_B duration')
        base=self.pose(23)[0]
        candidates=[(max(abs(p['scale'][a]-base[i]['scale'][a])for i,p in enumerate(self.pose(t)[0])for a in range(3)),t)for t in range(30,end+1)]
        delta,tick=max(candidates)
        k.need(delta>1e-12,'Selected original varying-scale take must supply measurable post-takeover scale movement')
        return max(60,tick),dict(source_scale_delta=delta,selected_source_tick=tick,comparison_source_tick=23)


def step(s,tick,session,ticks=1,animations=(),physical=()):
    result=s.rpc('runtime.step',dict(session_id=session,request_id=uid(),expected_tick=tick,ticks=ticks,
        animations=list(animations),physical_rigs=list(physical)))
    k.need(type(result['tick'])is int and result['tick']==tick+ticks,'Actual committed Tick receipt differs from literal source schedule')
    return tick+ticks


def command(clip):
    return dict(entity=f.RIG,clip=clip,time=0.,speed=1.,loop=False,playing=True,blend_ticks=12,transition_mode='crossfade')


def scalar_near(actual,expected,tolerance,why):
    k.need(type(actual)in(int,float) and type(expected)in(int,float) and math.isfinite(actual) and math.isfinite(expected),
        why+': strict finite numeric scalar values required')
    k.need(abs(actual-expected)<=tolerance,why)


def compare_clock(actual,expected):
    k.need(type(actual)is dict and set(actual)==set(expected),'Exact complete playback/transition field membership')
    for name in ('clip','speed','loop','playing','duration'):k.need(f.exact(actual[name],expected[name]),'Independent target clock field '+name)
    scalar_near(actual['time'],expected['time'],2e-15,'Independent source clock phase')
    if expected['transition']is None:k.need(actual['transition']is None,'Expired/nonexistent fade must not remain active')
    else:
        a,b=actual['transition'],expected['transition'];k.need(type(a)is dict and set(a)==set(b),'Exact ordinary crossfade diagnostic membership')
        for name in b:
            if name in ('weight','source_time'):scalar_near(a[name],b[name],2e-15,'Independent transition clock/weight')
            else:k.need(f.exact(a[name],b[name]),'Independent outgoing source clock field '+name)


def observe(s,tick,session,schedule,profile,phase,passive,label):
    expected=schedule.pose(tick);rows=original.all_nodes(s,tick,session)
    wrapper=s.rpc('runtime.entity',dict(session_id=session,tick=tick,id=f.RIG))
    compare_clock(wrapper['animation'],schedule.clock(tick,phase))
    state=f.query(s,tick,session,include_parts=True,include_joints=True)
    k.need(state['mode']==('passive'if passive else'animated') and state['body_count']==15 and state['joint_count']==14 and len(state['parts'])==15 and len(state['joints'])==14,'Actual rich complete physical graph')
    k.need([p['name']for p in state['parts']]==[p['name']for p in profile['parts']] and len({p['body_id']for p in state['parts']})==15,'Complete ordered unique original physical membership')
    mapped={p['node']for p in profile['parts']};scale_rows=[]
    for index,(row,wanted)in enumerate(zip(rows,expected[0])):
        actual=row['local_transform']['scale'];distances=[ulps(a,b)for a,b in zip(actual,wanted['scale'])]
        k.need(len(actual)==3 and all(d<=SCALE_ULPS for d in distances),'Actual composed source scale differs beyond fixed independent4ULP arithmetic bound')
        initial=schedule.initial[index]['scale']
        k.need(all(abs(a-b)<=BOUND*b for a,b in zip(actual,initial)),'Actual full-node value escaped authored bounded-scale envelope')
        scale_rows.append(dict(node=index,actual=copy.deepcopy(actual),source_expected=copy.deepcopy(wanted['scale']),ulp_distance=distances))
        if not passive:
            k.near(row['world_matrix'],multiply(transform_matrix(**WRAPPER),expected[2][index]),MATRIX_TOL,'Independent composed original source FK node '+str(index))
        elif index not in mapped:
            k.near(row['local_transform']['position'],wanted['position'],MATRIX_TOL,'Unmapped actual source local position '+str(index))
            k.need(abs(abs(k.dot(row['local_transform']['rotation'],wanted['rotation']))-1)<=2e-10,'Unmapped actual source local rotation')
    if passive:
        frames=k.logical_frames(schedule.oracle,([r['local_transform']for r in rows],[],[r['world_matrix']for r in rows]))
        for body,part in zip(state['parts'],profile['parts']):
            frame=copy.deepcopy(frames[part['node']]);frame['rotation']=k.normalized(k.quaternion(wrapper['local_transform']['rotation'],frame['rotation']))
            target=k.compose(frame,part['body_in_bone']);k.near(body['position'],target['position'],4e-4,'Actual source-scale affine skeleton/native COM join')
            k.need(abs(abs(k.dot(body['rotation'],target['rotation']))-1)<=2e-5 and body['enrolled']is True,'Actual logical body axes and native enrollment')
    record=dict(label=label,tick=tick,phase=phase,passive=passive,all_nodes=rows,wrapper=wrapper,physical=state,scales=scale_rows,
        source_clock=schedule.clock(tick,phase))
    f.REPORT['observations'].append(record)
    return record


def archive(s,saves,tick,slot,session,observation):
    value,raw,record=original.archived(s,saves,tick,slot,session)
    entities=value['snapshot']['payload']['entities'];by_id={e['id']:e for e in entities}
    k.need(len(by_id)==len(entities),'No duplicate saved entity identities')
    for index,row in enumerate(observation['all_nodes']):
        k.need(f.exact(by_id[f.node(index)]['local']['scale'],row['local_transform']['scale']),
            'EXACT actual native/saved all57 local-scale witness')
    output_bound(f.ARGS.output)
    return value,raw,record


def whole(left,right,label):
    k.need(left[1]==right[1] and f.exact(left[0],right[0]),'Complete original archive bytes/document differ: '+label)
    row=dict(kind=label,left=left[2]['path'],right=right[2]['path'],bytes=len(left[1]),sha256=hashlib.sha256(left[1]).hexdigest())
    f.REPORT['whole_pairs'].append(row)


def verify_history(value,schedule,profile,tick):
    rigs=value['snapshot']['payload']['physical']['rigs']
    k.need(len(rigs)==1 and rigs[0]['entity']==f.RIG,'Complete named original history owner')
    rig=rigs[0];history=rig['pose']
    k.need(rig['membership']['mode']=='animated' and rig['membership']['enrolled']is False,'Still Animated before explicit takeover')
    k.need(history['current']['tick']==tick and history['previous']['tick']==tick-1 and history['root_bone_to_wrapper']is None,'Exact two committed crossfade history ticks')
    checks=[]
    for frame in (history['previous'],history['current']):
        ft=frame['tick'];now=schedule.bodies(profile,ft);before=schedule.bodies(profile,ft-1)
        k.need([p['part']for p in frame['bodies']]==[p['name']for p in profile['parts']],'Complete ordered15part history')
        for actual,current,old in zip(frame['bodies'],now,before):
            k.near(actual['position'],current['position'],COM_TOL,'Independent crossfade historical metric COM')
            k.need(abs(abs(k.dot(actual['rotation'],current['rotation']))-1)<=2e-10,'Independent crossfade history logical rotation')
            linear=k.mul(k.sub(current['position'],old['position']),1/DT)
            angular=original.angular_velocity(old['rotation'],current['rotation'])
            k.near(actual['linear_velocity'],linear,VELOCITY_TOL,'Independent consecutive composed-pose COM velocity')
            k.near(actual['angular_velocity'],angular,ANGULAR_TOL,'Independent composed-pose angular velocity')
            checks.append(dict(tick=ft,part=current['part'],source_position=current['position'],source_rotation=current['rotation'],
                source_linear_velocity=linear,source_angular_velocity=angular))
        k.near(frame['wrapper']['position'],WRAPPER['position'],1e-12,'Constant authored wrapper')
        k.near(frame['wrapper']['linear_velocity'],[0,0,0],1e-12,'No invented wrapper linear momentum')
        k.near(frame['wrapper']['angular_velocity'],[0,0,0],1e-12,'No invented wrapper angular momentum')
    k.need(any(k.norm(c['source_linear_velocity'])>.001 or k.norm(c['source_angular_velocity'])>.001 for c in checks),'Actual original composed source supplies nonzero motion')
    return checks


def takeover_momentum(state,value,schedule,profile):
    ids={p['body_id']for p in state['parts']};pairs=value['snapshot']['payload']['physical']['physics']['pairs']
    k.need(all(p['body_a']in ids and p['body_b']in ids for p in pairs),'First takeover must remain airborne without external contacts')
    k.need(all(not p['at_linear_velocity_limit'] and not p['at_angular_velocity_limit']for p in state['parts']),'Do not clip played or solver velocity')
    now,old=schedule.bodies(profile,22),schedule.bodies(profile,21)
    velocities=[k.mul(k.sub(a['position'],b['position']),1/DT)for a,b in zip(now,old)]
    mass=sum(p['mass']for p in profile['parts'])
    initial=[sum(p['mass']*v[i]for p,v in zip(profile['parts'],velocities))for i in range(3)]
    k.need(all(p['linear_damping']==.10 for p in profile['parts']),'Unchanged original common damping')
    f32=original.f32;dt=f32(DT);gravity=f32(-9.81);drag=f32(1-f32(f32(.10)*dt))
    expected=k.mul(k.add(initial,[0,mass*gravity*dt,0]),drag)
    actual=[sum(p['mass']*p['linear_velocity'][i]for p in state['parts'])for i in range(3)]
    roundoff=128*2**-23*max(1,sum(abs(v)for v in initial),sum(abs(v)for v in expected))
    bound=mass*VELOCITY_TOL+roundoff
    k.near(actual,expected,bound,'First actual native passive aggregate momentum from source blend history')
    return dict(source_tick=22,native_tick=23,source_linear_momentum=initial,expected_after_gravity_drag=expected,
        actual_native_momentum=actual,absolute_per_axis_bound=bound,source_difference_bound=mass*VELOCITY_TOL,
        float_roundoff_bound=roundoff,scope='Aggregate linear momentum; solver may redistribute individual velocities. No energy-conservation or active-control claim.')


def world_witness(s,label):
    expected=f.REPORT['authored_identity'];package=Path(str(s.world)+'.assets')/(f.REPORT['prepared']['asset']+'.pmodel')
    world_actual=identity(s.world);package_actual=identity(package)
    k.need(world_actual==expected['world'] and package_actual==expected['package'],'Original authored world and entire original cooked asset bytes changed between owners')
    f.REPORT['world_identity_joins'].append(dict(label=label,owner=str(s.directory),world=str(s.world),world_identity=world_actual,
        package=str(package),package_identity=package_actual,nodes=57,takes=76,parts=15))


def continuation(s,saves,base,future,base_observation,future_observation,session,tick,future_tick,prefix):
    warm=uid();k.need(original.restore(s,saves,base[2]['slot'],warm,session,future_tick)==tick,'Warm restore exact source clock tick')
    restored=archive(s,saves,tick,prefix+'-warm-restored',warm,base_observation);whole(base,restored,prefix+'-warm-restore')
    step(s,tick,warm,future_tick-tick)
    continued=archive(s,saves,future_tick,prefix+'-warm-future',warm,future_observation);whole(future,continued,prefix+'-warm-future')
    # Exclusive authoritative writer: retire this actual owner before opening
    # the same world in a genuinely fresh process. The new owner continues the
    # next phase, preserving one world rather than cloning authored content.
    world_witness(s,prefix+'-before-owner-close')
    s.close()
    world_witness(s,prefix+'-after-natural-owner-close')
    cold=Session(s.directory/('cold-'+prefix),s.world)
    try:
        world_witness(cold,prefix+'-fresh-owner-original-world-and-package')
        cold_session=uid();k.need(original.restore(cold,saves,base[2]['slot'],cold_session)==tick,'Fresh process reconstructs exact source clock tick')
        restored=archive(cold,saves,tick,prefix+'-cold-restored',cold_session,base_observation);whole(base,restored,prefix+'-cold-restore')
        step(cold,tick,cold_session,future_tick-tick)
        continued=archive(cold,saves,future_tick,prefix+'-cold-future',cold_session,future_observation);whole(future,continued,prefix+'-cold-future')
    except BaseException:
        cold.close();raise
    return cold,cold_session


def run(a,oracle,profile,schedule):
    s=Session(a.output/'shared-world')
    try:
        f.REPORT['prepared']=original.prepare(s,a.source,oracle,profile,36,'flat')
        package=Path(str(s.world)+'.assets')/(f.REPORT['prepared']['asset']+'.pmodel')
        f.REPORT['authored_identity']=dict(world=identity(s.world),package=identity(package))
        world_witness(s,'first-owner-original-world-and-package')
        authored=s.world.read_bytes();session=uid();s.rpc('runtime.start',dict(session_id=session,revision=1))
        saves=f.save_root(s)
        observe(s,0,session,schedule,profile,'idle',False,'idle-initial')
        step(s,0,session,6);observe(s,6,session,schedule,profile,'idle',False,'idle-committed')
        observations={}
        for tick in range(7,19):
            step(s,tick-1,session,animations=[command(48)]if tick==7 else())
            observations[tick]=observe(s,tick,session,schedule,profile,'run',False,'run-crossfade-'+str(tick))
            if tick==12:run_mid=archive(s,saves,tick,'run-mid',session,observations[tick])
        run_future=archive(s,saves,18,'run-future',session,observations[18])
        f.REPORT['run_mid_history']=verify_history(run_mid[0],schedule,profile,12)
        s,session=continuation(s,saves,run_mid,run_future,observations[12],observations[18],session,12,18,'run')
        for tick in range(19,23):
            step(s,tick-1,session,animations=[command(25)]if tick==19 else())
            observations[tick]=observe(s,tick,session,schedule,profile,'death',False,'death-crossfade-'+str(tick))
            if tick==21:archive(s,saves,tick,'death-prev',session,observations[tick])
        death_mid=archive(s,saves,22,'death-mid',session,observations[22])
        f.REPORT['takeover_history']=verify_history(death_mid[0],schedule,profile,22)
        step(s,22,session,physical=[dict(entity=f.RIG,activate=True)])
        first=observe(s,23,session,schedule,profile,'death',True,'takeover-after-distinct-committed-tick')
        first_save=archive(s,saves,23,'takeover',session,first)
        f.REPORT['takeover_momentum']=takeover_momentum(first['physical'],first_save[0],schedule,profile)
        step(s,23,session,4);mid=observe(s,27,session,schedule,profile,'death',True,'passive-mid-crossfade')
        base=archive(s,saves,27,'passive-mid',session,mid)
        future_tick,selection=schedule.later_changed_scale_tick();f.REPORT['later_scale_selection']=selection
        step(s,27,session,future_tick-27);future_observation=observe(s,future_tick,session,schedule,profile,'death',True,'passive-varying-source-future')
        k.need(any(ulps(a,b)>SCALE_ULPS for left,right in zip(first['all_nodes'],future_observation['all_nodes'])for a,b in zip(left['local_transform']['scale'],right['local_transform']['scale'])),'Actual scale movement must survive passive ownership; no takeover freeze')
        future=archive(s,saves,future_tick,'passive-future',session,future_observation)
        s,session=continuation(s,saves,base,future,mid,future_observation,session,27,future_tick,'passive')
        k.need(s.world.read_bytes()==authored,'Runtime/reloads/saves must not modify authored original world')
        k.need(len(f.REPORT['archives'])==15 and len(f.REPORT['whole_pairs'])==8,'Complete planned archive/pair population')
        f.REPORT.update(final_tick=future_tick,authored_world_unchanged=True)
    finally:
        s.close()
        if 'authored_identity'in f.REPORT:world_witness(s,'final-owner-after-natural-close')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--source',type=Path,required=True)
    p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    a.binary=a.binary.resolve();a.source=a.source.resolve();a.license=a.license.resolve();a.output=a.output.resolve()
    k.need(not a.output.exists(),'Fresh output required');a.output.mkdir(parents=True)
    f.ARGS=a;f.DEADLINE=time.monotonic()+SECONDS;f.MAX_CALLS=RPC_LIMIT
    f.REPORT=dict(format='poima.physical-character-transitions.v1',passed=False,calls=[],owners=[],archives=[],whole_pairs=[],observations=[],world_identity_joins=[],errors=[],
        bounds=dict(seconds=SECONDS,rpc=RPC_LIMIT,observed_output_bytes=OUTPUT_LIMIT,scale_ulp_distance=SCALE_ULPS,authored_relative_scale_bound=BOUND),
        scope='One original57node/76take world, ordinary Idle/Run/Death_B crossfades, actual source scales, midblend takeover and complete warm/cold saves. No controller/active-control/render/performance/cross-platform equality claim.')
    try:
        inputs={str(path):identity(path)for path in [SELF,a.binary,a.source,a.license,*[REPO/path for path in PUBLIC_INPUTS]]}
        f.REPORT['inputs']=inputs
        oracle,original_profile,calibration=k.inspect_inputs(a.source,a.license);profile=copy.deepcopy(original_profile)
        profile['pose_frame']='skeletal_bounded_scale';profile['max_scale_deviation']=BOUND
        old=copy.deepcopy(profile);old.pop('max_scale_deviation');old['pose_frame']='skeletal_fixed_scale'
        k.need(f.exact(old,original_profile),'Original15part proxy/joint profile unchanged except explicit policy and bound')
        schedule=SourceSchedule(oracle);schedule.later_changed_scale_tick()
        f.REPORT.update(calibration=calibration,authored_profile=profile)
        run(a,oracle,profile,schedule)
        k.need(len(f.REPORT['owners'])==3 and all(o['natural_exit']and not o['forced_cleanup']and o['exit_code']==0 for o in f.REPORT['owners']),'All3 native owners must naturally close EOF0')
        for path,witness in inputs.items():k.need(identity(path)==witness,'Selected source/native/helper bytes changed during workflow')
        f.REPORT['passed']=True
    except BaseException:f.REPORT['errors'].append(traceback.format_exc())
    f.REPORT['output_census']=output_bound(a.output)
    encoded=(json.dumps(f.REPORT,indent=2,allow_nan=False)+'\n').encode()
    k.need(len(encoded)<=EVIDENCE_RESERVE and f.REPORT['output_census']['bytes']+len(encoded)<=OUTPUT_LIMIT,'Complete final evidence must fit reserved bound')
    (a.output/'evidence.json').write_bytes(encoded)
    return 0 if f.REPORT['passed']else 1
if __name__=='__main__':raise SystemExit(main())
