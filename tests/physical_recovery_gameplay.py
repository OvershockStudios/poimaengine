#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prebuilt CoreCLR recovery consumer, real import/physics and complete saves.

Requires an external process-tree supervisor. Builds nothing and opens no GPU,
window or audio device. Fresh output preserves every original failure. This is
an analytic three-proxy test, not imported-character or get-up qualification.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
import traceback
import uuid
sys.dont_write_bytecode=True
SELF=Path(__file__).resolve()
ROOT=next(p for p in SELF.parents if (p/'tests/world_physical_contract.py').is_file() and (p/'CMakeLists.txt').is_file())
sys.path.insert(0,str(ROOT/'tests'))
import world_physical_contract as f
import physical_drive_gameplay as d
from managed_physical_gameplay_contract import sha,strict_json
from gltf_fixture import glb
need,exact=f.need,f.exact
SAMPLE=SELF.parent/'managed_physical_recovery_gameplay'
GAME='Poima.Tests.PhysicalRecoveryGame';UNMARKED='Poima.Tests.PhysicalRecoveryUnmarkedGame';IDENTITY='poima.test.physical-recovery'
RIG,ACTOR,FLOOR,WALL,RECIPE=(f'{n:032x}' for n in (0x10,0x20,0x30,0x31,0x40))
SECONDS,CALLS,MIB=360,1500,1024**2
STATUSES='locomotion history_unavailable not_standing not_upright moving wrapper_displaced missing_support moving_support invalid_support proxy_blocked motor_blocked query_budget invalid_native_result stabilizing ready returned sole_out_of_range'.split()
FIELDS={'Rig':'entity',**dict.fromkeys('Action Ticks Writes CommittedReads RejectAfterWrite Status Physical TransitionActive StableTicks RequiredStableTicks LastAttemptPresent LastAttemptResult ReturnedPresent SupportMask'.split(),'int32'),
    **dict.fromkeys('FailAt ObservedTick Generation AssessmentRevision LastAttemptTick ReturnedTick FirstSupportBody SecondSupportBody'.split(),'int64'),
    **dict.fromkeys('MaximumLinearSpeed MaximumAngularSpeed MinimumUprightDot WrapperDistance WrapperAngle'.split(),'float64')}
CONTROLS={'Action','RejectAfterWrite','FailAt'}
def fresh():return uuid.uuid4().hex

def initial():
    value={name:'0'*32 if kind=='entity' else '0' if kind=='int64' else 0. if kind=='float64' else 0 for name,kind in FIELDS.items()}
    value.update(Rig=RIG,FailAt='-1');return value

def validate(value,schema):
    need(type(value)is dict and set(value)==set(FIELDS),'Every compiled field is present')
    offset,layout=0,{}
    for name,kind in FIELDS.items():
        size=16 if kind=='entity' else 4 if kind=='int32' else 8;align=min(size,8);offset=(offset+align-1)//align*align
        layout[name]=dict(name=name,kind=kind,offset=offset,bytes=size);offset+=size;x=value[name]
        valid=(type(x)is str and re.fullmatch('[0-9a-f]{32}',x)) if kind=='entity' else (type(x)is int and -(2**31)<=x<2**31) if kind=='int32' else (type(x)is str and re.fullmatch(r'0|-[1-9][0-9]*|[1-9][0-9]*',x) and -(2**63)<=int(x)<2**63) if kind=='int64' else type(x)is float and math.isfinite(x)
        need(valid,'Exact typed field '+name)
    need(schema['identity']==IDENTITY and schema['bytes']==(offset+7)//8*8 and [r['name'] for r in schema['fields']]==sorted(FIELDS) and all(exact(r,layout[r['name']]) for r in schema['fields']),'Independent complete scalar layout')
    return value

def model():
    # Same pelvis/sole/accessory geometry as the native recovery fixture,
    # expressed as an original deterministic glTF recipe through real import.
    blob,views,accessors=bytearray(),[],[]
    def acc(rows,kind,fmt='f'):
        while len(blob)%4:blob.append(0)
        start=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+fmt*len(flat),*flat))
        views.append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start));accessors.append(dict(bufferView=len(views)-1,componentType=5126 if fmt=='f' else 5123,count=len(rows),type=kind));return len(accessors)-1
    position=acc([(0,.9,0),(-.18,.08,0),(.18,.08,0)],'VEC3');accessors[position].update(min=[-.18,.08,0],max=[.18,.9,0])
    attributes=dict(POSITION=position,NORMAL=acc([(0,0,1)]*3,'VEC3'),TEXCOORD_0=acc([(0,1),(0,0),(1,0)],'VEC2'),JOINTS_0=acc([(0,0,0,0),(1,0,0,0),(2,0,0,0)],'VEC4','H'),WEIGHTS_0=acc([(1,0,0,0)]*3,'VEC4'))
    indices=acc([(0,),(1,),(2,)],'SCALAR','H');inverse=[]
    for x,y in ((0,.9),(-.18,.08),(.18,.08)):inverse.append([1,0,0,0,0,1,0,0,0,0,1,0,-x,-y,0,1])
    binds=acc(inverse,'MAT4');times=acc([(0,),(4,)],'SCALAR');accessors[times].update(min=[0],max=[4])
    values=acc([(.03,.2,.1),(.08,.2,.1)],'VEC3')
    doc=dict(asset=dict(version='2.0',generator='Poima deterministic recovery fixture'),buffers=[dict(byteLength=len(blob))],bufferViews=views,accessors=accessors,
        meshes=[dict(primitives=[dict(attributes=attributes,indices=indices)])],
        nodes=[dict(name='pelvis',translation=[0,.9,0],children=[1,2,3]),dict(name='left_foot',translation=[-.18,-.82,0]),dict(name='right_foot',translation=[.18,-.82,0]),dict(name='unmapped_accessory',translation=[.03,.2,.1],mesh=0,skin=0)],
        skins=[dict(skeleton=0,joints=[0,1,2],inverseBindMatrices=binds)],animations=[dict(name='idle and accessory',samplers=[dict(input=times,output=values,interpolation='LINEAR')],channels=[dict(sampler=0,target=dict(node=3,path='translation'))])],scenes=[dict(nodes=[0])],scene=0)
    return glb(doc,bytes(blob))

def profile():
    parts=[];names=['pelvis','left_foot','right_foot']
    for n,name in enumerate(names):parts.append(dict(name=name,node=n,body_in_bone=f.pose(),shape=dict(kind='box',half_extents=[.15,.1,.25] if n else [.15,.12,.12]),mass=2 if n else 4,friction=.6,restitution=0,linear_damping=.1,angular_damping=.1))
    joints=[]
    for n,x in ((1,-.18),(2,.18)):joints.append(dict(name='left_joint' if n==1 else 'right_joint',parent='pelvis',child=names[n],parent_frame=f.pose((x,-.82,0)),child_frame=f.pose(),limits=dict(kind='swing_twist',swing_y=.4,swing_z=.4,twist_min=-.4,twist_max=.4),neutral_rotation=[0,0,0,1],rest_stiffness=0,rest_damping=0,soft_limit_stiffness=0,soft_limit_zone=.1,max_torque=1000,friction_torque=1))
    return dict(root='pelvis',initial_mode='animated',parts=parts,joints=joints,controller_handoff=dict(actor=ACTOR,follow='translate_keep_heading'),
        recovery=dict(supports=[dict(part=name,point=[0,-.1,0]) for name in names[1:]],uprights=[dict(part='pelvis',up_axis=[0,1,0])],max_tilt_degrees=20,max_linear_speed=.6,max_angular_speed=2,stability_ticks=3,blend_ticks=12))

def transform(position=(0,0,0)):return dict(**f.pose(position),scale=[1,1,1])
def box(half):return dict(half_extents=half,motion='static',mass=1,friction=.5,restitution=0)
def prepare(io):
    source=io.directory/'original-recovery.glb';source.write_bytes(model());imported=io.rpc('asset.import',dict(source=f.native(source)))
    need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives'])==(4,1,1,1),'Actual complete analytic import')
    asset=imported['asset'];ops=[]
    for entity,name,position in ((FLOOR,'Static floor',(0,-.5,0)),(WALL,'Standing collision wall',(2,1,0)),(ACTOR,'Linked movement owner',(0,.1,0))):ops.extend([dict(op='entity.create',id=entity,name=name),f.component(entity,'Transform',transform(position))])
    ops += [f.component(FLOOR,'BoxCollider',box([20,.5,20])),f.component(WALL,'BoxCollider',box([.1,1,2])),
        f.component(ACTOR,'CharacterController',dict(radius=.3,height=1.8,speed=2,jump_speed=5,camera=None,locomotion={})),
        dict(op='asset.instantiate',id=RIG,name='Recovery analytic rig',asset=asset,parent=ACTOR),
        f.component(RIG,'AnimationRig',dict(asset=asset,clip=0,time=0,speed=1,loop=True,playing=True)),
        dict(op='template.set',id=RECIPE,name='Capsule obstruction',components=dict(Transform=transform((0,1,0)),BoxCollider=box([.05,.6,.5])))]
    need(f.transact(io,ops,0)['revision']==1 and f.transact(io,[f.component(RIG,'PhysicalRig',profile())],1)['revision']==2,'Complete authored linked recovery world')
    cooked=Path(str(io.world)+'.assets')/(asset+'.pmodel');need(cooked.is_file() and sha(cooked)==asset,'Cooked source hash')
    return dict(source=str(source),source_sha256=sha(source),package=str(cooked),package_sha256=asset,profile=profile(),imported=imported)

def pair(report,label,left,right):
    equal=Path(left['path']).read_bytes()==Path(right['path']).read_bytes() and exact(left['archive'],right['archive'])
    report['whole_pairs'].append(dict(label=label,left=left['path'],right=right['path'],complete_bytes_and_typed_equal=equal,excluded_fields=[]));need(equal,'Whole save differs: '+label)

class Consumer:
    def __init__(self,io,report,saves,config):self.io,self.report,self.saves,self.config=io,report,saves,config;self.session,self.tick,self.values=fresh(),0,initial();self.structure=0
    def state(self):return self.io.rpc('runtime.inspect',dict(session_id=self.session))
    def game(self):return self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
    def compiled(self,label):
        result=self.game();module=result['module'];need(module['backend']=='coreclr' and module['type']==self.config['type'] and module['assembly_sha256']==self.report['assembly_sha256'],'Actual selected compiled consumer')
        values=validate(module['values'],module['schema']);need(exact(values,self.values),'Complete predicted state: '+label)
        self.report['game_observations'].append(dict(label=label,tick=self.tick,values=copy.deepcopy(values)));return result
    def query(self,method='runtime.physical_recovery',**extra):
        result=self.io.rpc(method,dict(session_id=self.session,tick=self.tick,entity=RIG,**extra));need(result['session_id']==self.session and result['tick']==self.tick and result['source']=='runtime','Exact committed native query');return result['state']
    def recovery(self):
        q=self.query();need(set(q)=={'entity','sampled_tick','generation','assessment_revision','phase','status','stable_ticks','required_stable_ticks','last_attempt_tick','last_attempt_result','returned_tick','maximum_linear_speed','maximum_angular_speed','minimum_upright_dot','wrapper_distance','wrapper_angle','support_body_ids'} and q['entity']==RIG and q['sampled_tick']==self.tick and q['status'] in STATUSES and q['last_attempt_result'] in STATUSES and q['phase'] in ('physical','returning','locomotion'),'Complete recovery observation')
        for k in ('sampled_tick','generation','assessment_revision','stable_ticks','required_stable_ticks'):need(type(q[k])is int and 0<=q[k]<=9007199254740991,'Recovery unsigned '+k)
        for k in ('last_attempt_tick','returned_tick'):need(q[k] is None or type(q[k])is int and 0<q[k]<=self.tick,'Recovery optional '+k)
        need(len(q['support_body_ids'])==2 and all(x is None or type(x)is int and 0<=x<=0xffffffff for x in q['support_body_ids']),'Exact support identity mask')
        for k in ('maximum_linear_speed','maximum_angular_speed','minimum_upright_dot','wrapper_distance','wrapper_angle'):need(type(q[k])is float and math.isfinite(q[k]),'Recovery typed metric '+k)
        return q
    def physical(self):
        q=self.query('runtime.physical_rig',include_parts=True,include_joints=True)
        need(q['body_count']==3 and q['joint_count']==2 and [x['name'] for x in q['parts']]==['pelvis','left_foot','right_foot'] and [x['name'] for x in q['joints']]==['left_joint','right_joint'],'Complete original physical membership')
        hand=self.query('runtime.controller_handoff');physical=q['mode']!='animated'
        need(hand['actor']==ACTOR and hand['capsule_enrolled']==(not physical) and hand['phase']==('physical' if physical else 'locomotion') and all(x['enrolled']==physical for x in q['parts']),'Exclusive capsule/articulation ownership')
        return q
    def entity(self,entity):
        value=self.io.rpc('runtime.entity',dict(session_id=self.session,tick=self.tick,id=entity))
        need(value['id']==entity and value['session_id']==self.session and type(value['tick'])is int and value['tick']==self.tick,'Exact entity identity and committed tick')
        return value
    def pose(self):
        identities=[ACTOR,RIG]+[hashlib.sha256(f'poima.instance.v1/{RIG}/node/{n}'.encode()).hexdigest()[:32] for n in range(4)]
        return [dict(id=x,local=(q:=self.entity(x))['local_transform'],world=q['world_matrix']) for x in identities]
    def edit(self,**values):
        need(set(values)<=CONTROLS,'Only explicit fixture controls are mutable');g=self.game();state=self.state()
        self.io.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_revision=g['revision'],expected_structure_revision=state['structure_revision'],values=values));self.values.update(values);self.compiled('typed control edit')
    def expected(self,q):
        v=copy.deepcopy(self.values);need(v['Action'] in (0,1,2),'Known successful action');v.update(Action=0,Ticks=v['Ticks']+1,Writes=v['Writes']+int(v['Action']!=0),CommittedReads=v['CommittedReads']+1,
            Status=STATUSES.index(q['status']),Physical=int(q['phase']=='physical'),TransitionActive=int(q['phase']=='returning'),StableTicks=q['stable_ticks'],RequiredStableTicks=q['required_stable_ticks'],
            LastAttemptPresent=int(q['last_attempt_tick'] is not None),LastAttemptResult=STATUSES.index(q['last_attempt_result']),ReturnedPresent=int(q['returned_tick'] is not None),SupportMask=sum(1<<i for i,x in enumerate(q['support_body_ids']) if x is not None))
        for field,key in [('ObservedTick','sampled_tick'),('Generation','generation'),('AssessmentRevision','assessment_revision'),('LastAttemptTick','last_attempt_tick'),('ReturnedTick','returned_tick')]:v[field]=str(q[key] or 0)
        v['FirstSupportBody'],v['SecondSupportBody']=[str(x or 0) for x in q['support_body_ids']]
        for field,key in [('MaximumLinearSpeed','maximum_linear_speed'),('MaximumAngularSpeed','maximum_angular_speed'),('MinimumUprightDot','minimum_upright_dot'),('WrapperDistance','wrapper_distance'),('WrapperAngle','wrapper_angle')]:v[field]=q[key]
        return v
    def step(self,label,inputs=(),caller=()):
        before=self.recovery();expected=self.expected(before)
        reply=self.io.rpc('runtime.step',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_structure_revision=self.structure,ticks=1,inputs=list(inputs),physical_rigs=list(caller)))
        need(reply['tick']==self.tick+1 and not reply['replayed'],'Exact one native Tick');self.tick+=1;self.values=expected;self.compiled(label)
        after=self.recovery();self.report['observations'].append(dict(label=label,before=before,after=after));return after
    def archive(self,label):
        game=self.compiled(label);state=self.state();slot='recovery-'+str(len(self.report['archives'])+1)
        receipt=self.io.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        manifest_path=self.saves/('slot-'+slot)/'current.json';manifest=strict_json(manifest_path.read_bytes());current=manifest['payload']['current'];need(re.fullmatch(r'p-[0-9a-f]{32}\.bin',current['file']) is not None,'Exact archive member')
        path=manifest_path.parent/current['file'];raw=path.read_bytes();value=strict_json(raw);need(len(raw)<=16*MIB and len(raw)==receipt['bytes']==current['bytes'] and hashlib.sha256(raw).hexdigest()==receipt['sha256']==current['sha256'],'Complete original archive identity')
        need(set(value)=={'format','version','document','snapshot'} and value['format']=='poima.world-save' and type(value['version'])is int and value['version']==1,'Exact complete world save envelope')
        original=d.original_member(d.original_member(raw,'snapshot'),'payload');payload=value['snapshot']['payload'];need(hashlib.sha256(original.encode()).hexdigest()==value['snapshot']['sha256'],'Original native checksum')
        game=payload['gameplay'];need(game['backend']=='coreclr' and game['type']==self.config['type'] and game['assembly_sha256']==self.report['assembly_sha256'] and exact(validate(game['values'],game['schema']),self.values),'Complete saved compiled state')
        need(payload['tick']==self.tick and payload['physical']['version']==8 and len(payload['physical']['rigs'])==1,'Recovery extension8 retained')
        native=payload['physical']['rigs'][0];need(native['entity']==RIG and set(native['recovery'])=={'state','source_player','animation'},'Complete physical/source/return recovery membership')
        source=native['recovery']['source_player'];need(source['format']=='poima.animation-target-state' and source['version']==1 and source['tick']==self.tick and source['entity']==RIG,'Complete source player at the same actual boundary')
        need((source['history']['previous'] is not None)==(native['recovery']['state']['previous'] is not None),'Source player retains the same previous-boundary membership as the physical frames')
        row=dict(label=label,slot=slot,tick=self.tick,path=str(path),bytes=len(raw),sha256=sha(path),archive=value,receipt=receipt,manifest=manifest,manifest_path=str(manifest_path));self.report['archives'].append(copy.deepcopy(row));return row
    def load(self,row,existing=True):
        state=self.state() if existing else None;game=self.game() if existing else None;session=fresh()
        result=self.io.rpc('save.load',dict(request_id=fresh(),configuration_generation=1,slot=row['slot'],expected_generation=1,revision=2,expected_session_id=self.session if existing else None,expected_tick=self.tick if existing else None,expected_gameplay_revision=game['revision'] if existing else None,expected_structure_revision=state['structure_revision'] if existing else None,expected_material_revision=state['material_revision'] if existing else None,expected_ui_revision=state['ui_revision'] if existing else None,expected_control_sequence=state['control_sequence'] if existing else None,new_session_id=session,gameplay=self.config))
        need(result.get('session_id',result.get('new_session_id'))==session and result['tick']==row['tick'],'Restored current owner identity')
        payload=row['archive']['snapshot']['payload'];structure=payload['structure']
        expected_structure=0 if structure is None else structure['revision']
        need(type(expected_structure)is int and type(result['structure_revision'])is int and result['structure_revision']==expected_structure,'Restored structure revision matches complete saved structure')
        self.session,self.tick,self.values=session,result['tick'],copy.deepcopy(payload['gameplay']['values']);self.structure=result['structure_revision'];self.compiled('restored all fields')
    def reject(self,label,caller=(),ticks=1,message=None):
        before=self.archive(label+' before');error=self.io.rpc('runtime.step',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_structure_revision=self.structure,ticks=ticks,physical_rigs=list(caller)),error=-32040)
        if message:need(message in error['message'],'Wrong literal failure: '+label)
        self.compiled(label+' unchanged');pair(self.report,label,before,self.archive(label+' after'))

def native_input(jump=False):return dict(entity=ACTOR,move=[1,0],look=[0,0],jump=jump,use=False)
def advance(c,count,label,inputs=()):
    for n in range(count):c.step(label+' '+str(n),inputs)
def ready(c):
    for n in range(121):
        q=c.recovery()
        if q['status']=='ready':need(q['phase']=='physical' and q['stable_ticks']==3,'Ready ownership/stability');return
        need(n<120,'Frozen fixture did not become ready within120 ticks');c.step('bounded admission '+str(n))
def return_once(c,label):
    before=c.recovery();pose=c.pose();parts=c.physical()['parts'];need(before['status']=='ready','Eligible original required')
    c.edit(Action=2);q=c.step(label,inputs=[native_input(True)])
    need(q['phase']=='returning' and q['generation']==before['generation']+1 and q['last_attempt_tick']==c.tick and q['returned_tick']==c.tick and q['last_attempt_result']=='returned','Actual completed recovery receipt')
    need(exact(pose,c.pose()),'First returned boundary changed actor/wrapper/all original node poses')
    need([r['body_id'] for r in parts]==[r['body_id'] for r in c.physical()['parts']],'Recovery reallocated physical IDs')
    return c.archive(label+' complete')
def continue_case(c,kind,label):
    if kind=='ready':return_once(c,label+' return');advance(c,3,label+' seeded blend')
    elif kind=='mid':advance(c,4,label+' midblend')
    elif kind=='finished':advance(c,3,label+' recovered idle')
    else:raise AssertionError('Unknown continuation')
    return c.archive(label+' future')
def exercise(c):
    c.io.rpc('runtime.start',dict(session_id=c.session,revision=2))
    # Settle through ordinary native steps before loading the real consumer.
    settled=c.io.rpc('runtime.step',dict(session_id=c.session,request_id=fresh(),expected_tick=0,ticks=90));need(settled['tick']==90,'Original controller settled');c.tick=90
    need(c.entity(ACTOR)['ground']=='on_ground','Real static support for the standing controller')
    loaded=c.io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=fresh(),expected_tick=c.tick,expected_revision=c.game()['revision'],**c.config));c.report['loaded_module']=copy.deepcopy(loaded['module'])
    # runtime.gameplay.inspect intentionally omits load-time requirements. Real
    # callback consumption plus the pinned marker is the proof here; do not
    # invent a requirement receipt that this API does not return.
    c.compiled('initialized complete small state');c.step('first actual compiled read');c.edit(Action=1);c.step('compiled activation');c.physical();ready(c)
    origin=c.archive('eligible original')
    c.edit(Action=2,RejectAfterWrite=1);c.reject('request then callback throw',message='Recovery fixture rejected after staging.')
    c.edit(RejectAfterWrite=0);c.reject('duplicate caller and compiled request',caller=[dict(entity=RIG,recover=True)])
    c.edit(FailAt=str(c.tick+1));c.reject('successful first step then late callback throw',ticks=2,message='Recovery fixture later-Tick failure.')
    c.edit(Action=0,FailAt='-1');c.load(origin);pair(c.report,'restored original after rollback cohorts',origin,c.archive('rollback origin restored'))
    # A real stale geometry admission: new wall is admitted at the same tick.
    before=c.recovery();state=c.state();born=c.io.rpc('runtime.structure.transact',dict(session_id=c.session,request_id=fresh(),expected_tick=c.tick,expected_structure_revision=state['structure_revision'],spawns=[dict(template_id=RECIPE)]))
    c.structure=born['structure_revision'];stale=c.recovery();need(stale['status']=='history_unavailable' and stale['assessment_revision']==before['assessment_revision'] and born['structure_revision']>stale['assessment_revision'],'Same-tick edit invalidated current eligibility')
    c.edit(Action=2);refused=c.step('actual freshly obstructed request');need(refused['generation']==before['generation'] and refused['phase']=='physical' and refused['last_attempt_tick']==c.tick and refused['last_attempt_result'] in ('proxy_blocked','motor_blocked'),'Stale Ready bypassed current full capsule obstruction')
    c.report['refusal']=dict(before=before,spawn_receipt=born,stale=stale,after=refused,archive=c.archive('actual obstruction refusal')['path'])
    c.load(origin);pair(c.report,'restore removes blocker and refusal state',origin,c.archive('post-refusal origin restored'))
    # Actual old permission check uses a real compiled class, not fabricated pointers.
    c.config=dict(c.config,type=UNMARKED);g=c.game();state=c.state()
    c.io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=fresh(),expected_tick=c.tick,expected_revision=g['revision'],expected_structure_revision=state['structure_revision'],**c.config))
    c.compiled('same schema unmarked module');c.reject('old physical permission denied',message='Declare IPhysicalRecoveryGame')
    c.config=dict(c.config,type=GAME);c.load(origin);pair(c.report,'original fully restored after unmarked consumer',origin,c.archive('marked origin restored'))
    references=[]
    ready_ref=continue_case(c,'ready','ready reference');references.append(('ready',origin,ready_ref))
    c.load(origin);pair(c.report,'warm eligible restore',origin,c.archive('warm eligible'))
    pair(c.report,'warm eligible continuation',ready_ref,continue_case(c,'ready','ready warm'))
    c.load(origin);mid=return_once(c,'return for midblend origin');mid_ref=continue_case(c,'mid','mid reference');references.append(('mid',mid,mid_ref))
    c.load(mid);pair(c.report,'warm midblend restore',mid,c.archive('warm midblend'))
    pair(c.report,'warm midblend continuation',mid_ref,continue_case(c,'mid','mid warm'))
    advance(c,12,'complete authored blend');q=c.recovery();need(q['phase']=='locomotion' and q['generation']==1,'Recovery completed in authored finite blend');c.physical()
    finished=c.archive('finished origin');finished_ref=continue_case(c,'finished','finished reference');references.append(('finished',finished,finished_ref))
    c.load(finished);pair(c.report,'warm finished restore',finished,c.archive('warm finished'))
    pair(c.report,'warm finished continuation',finished_ref,continue_case(c,'finished','finished warm'))
    start=c.entity(ACTOR)['world_matrix'][12:15];advance(c,15,'ordinary recovered walk',[native_input()]);walk=c.entity(ACTOR)
    need(walk['world_matrix'][12]>start[0]+.35 and walk['ground']=='on_ground','Recovered capsule did not walk on real floor')
    c.step('ordinary recovered jump',[native_input(True)]);advance(c,3,'jump ascent',[native_input()]);jump=c.entity(ACTOR)
    need(jump['world_matrix'][13]>walk['world_matrix'][13]+.15 and jump['ground']=='in_air','Recovered capsule did not jump')
    c.report['locomotion']=dict(start=start,walk=walk,jump=jump,archive=c.archive('walk and jump complete')['path'])
    return references

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);parser.add_argument('--hostfxr',type=Path,required=True);parser.add_argument('--bridge',type=Path,required=True);parser.add_argument('--assembly',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');args=parser.parse_args()
    need(not sys.flags.optimize,'Assertions must not be disabled');output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    report=dict(format='poima.physical-recovery-gameplay.v1',passed=False,backend='coreclr',calls=[],owners=[],archives=[],whole_pairs=[],observations=[],game_observations=[],limits=dict(seconds=SECONDS,rpc=CALLS,output_bytes=256*MIB,recorded_per_owner=48*MIB),scope='Actual analytic imported three-proxy compiled512 recovery and controller flow; no original-Knight, rendering, NativeAOT or performance claim.')
    f.ARGS,f.REPORT,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED=args,report,time.monotonic()+SECONDS,CALLS,48*MIB;io=None;pins={}
    try:
        files=[SELF,Path(f.__file__),Path(d.__file__),ROOT/'tests/managed_physical_gameplay_contract.py',ROOT/'tests/animation_fixture.py',ROOT/'tests/gltf_fixture.py',SAMPLE/'Game.cs',SAMPLE/'Poima.ManagedPhysicalRecoveryGame.csproj',ROOT/'managed/Directory.Build.props',args.binary.resolve(),args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve()]
        for image in (args.bridge.resolve(),args.assembly.resolve()):
            files.append(image.parent/'Poima.Gameplay.dll')
            for suffix in ('.deps.json','.runtimeconfig.json'):
                member=image.with_suffix(suffix)
                if member.is_file():files.append(member)
        for path in set(files):need(path.is_file(),'Missing exact input '+str(path));pins[str(path.resolve())]=sha(path)
        need(sha(args.bridge.parent/'Poima.Gameplay.dll')==sha(args.assembly.parent/'Poima.Gameplay.dll'),'Bridge and consumer must use the same exact SDK')
        report['assembly_sha256']=sha(args.assembly);report['input_pins']=copy.deepcopy(pins)
        config=dict(hostfxr=f.native(args.hostfxr),bridge=f.native(args.bridge),assembly=f.native(args.assembly),type=GAME)
        io=f.Session(output/'original-owner');report['authoring']=prepare(io);world=io.world;original=world.read_bytes();saves=f.save_root(io)
        for name in ('source','package'):path=Path(report['authoring'][name]);pins[str(path)]=sha(path)
        pins[str(world)]=sha(world);report['input_pins']=copy.deepcopy(pins)
        c=Consumer(io,report,saves,config);references=exercise(c);io.close();io=None
        io=f.Session(output/'cold-owner',world);io.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=f.native(saves)));c=Consumer(io,report,saves,config)
        for index,(kind,origin,reference) in enumerate(references):
            c.load(origin,existing=index!=0);pair(report,'fresh process '+kind+' restore',origin,c.archive('cold '+kind+' origin'))
            pair(report,'fresh process '+kind+' continuation',reference,continue_case(c,kind,'cold '+kind))
        need(world.read_bytes()==original,'Immutable authored world changed');io.close();io=None
        need(len(report['owners'])==2 and all(o['natural_exit'] and not o['forced_cleanup'] and o['exit_code']==0 for o in report['owners']),'Two natural owner closures')
        need(all(sha(path)==digest for path,digest in pins.items()),'An original consumed input changed');report['passed']=True
    except Exception as error:report['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
    finally:
        if io is not None:
            try:io.close()
            except Exception as error:report['closure_failure']=dict(type=type(error).__name__,message=str(error));report['passed']=False
        report['recordings']=[]
        for owner in report['owners']:
            for name,key in (('requests.jsonl','request'),('responses.jsonl','response'),('stderr.log',None)):
                path=Path(owner['directory'])/name
                try:
                    if path.is_file():
                        report['recordings'].append(dict(path=str(path),bytes=path.stat().st_size,sha256=sha(path)))
                        if key:
                            rows=[strict_json(line) for line in path.read_bytes().splitlines() if line.strip()]
                            need(exact(rows,[r[key] for r in report['calls'] if r['owner']==owner['directory']]),'Original wire population differs from decoded call records')
                except Exception as error:
                    # Preserve raw bytes and the original workflow failure; a
                    # secondary wire audit failure must not prevent evidence.
                    report['passed']=False;report.setdefault('wire_failures',[]).append(dict(path=str(path),type=type(error).__name__,message=str(error)))
        report['elapsed_seconds']=time.monotonic()-(f.DEADLINE-SECONDS)
        total=sum(p.stat().st_size for p in output.rglob('*') if p.is_file());encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        if len(encoded.encode())>64*MIB or total+len(encoded.encode())>256*MIB:report['passed']=False;report['evidence_bound_failure']=dict(existing_bytes=total,report_bytes=len(encoded.encode()));encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        (output/'evidence.json').write_text(encoded,encoding='utf-8');print(json.dumps(dict(passed=report['passed'],output=str(output),counts={k:len(report[k]) for k in ('calls','owners','archives','whole_pairs','game_observations')})))
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
