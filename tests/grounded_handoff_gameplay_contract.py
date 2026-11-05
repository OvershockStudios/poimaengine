#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual prebuilt CoreCLR grounded/planted controller-to-physics consumer.

Builds nothing and opens no window. Run beneath an external process-tree owner.
Three sequential CLI processes retain literal wire RPCs, complete typed state,
whole saves and native diagnostics. No imported-character/AOT/render/perf claim.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import time

import world_physical_contract as f
import controller_handoff_gameplay_contract as prior
import procedural_handoff_gameplay_contract as manual
from gltf_fixture import glb
from managed_physical_gameplay_contract import sha, strict_json

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT/'tests/managed_grounded_handoff_gameplay'
GAME, IDENTITY = 'Poima.Tests.GroundedHandoffGame', 'poima.test.grounded-handoff'
ACTOR, CAMERA, RIG, OTHER, FLOOR, RECIPE = (f'{n:032x}' for n in (100,109,101,201,300,900))
NAMES = ('pelvis','left_hip','left_knee','left_foot','right_hip','right_knee','right_foot')
PARENTS = (-1,0,1,2,0,4,5)
POSITIONS = ((0,1,0),(-.3,.8,0),(.5,-.7,0),(-.5,-.7,0),(.3,.8,0),(.5,-.7,0),(-.5,-.7,0))
MASSES = (4.,2.,1.,.5,2.,1.,.5)
FIELDS = {
    **dict.fromkeys((
        'Actor', 'Camera', 'Rig', 'LeftRoot', 'LeftMiddle', 'LeftEnd',
        'RightRoot', 'RightMiddle', 'RightEnd', 'OtherRig', 'OtherEnd', 'Support',
    ), 'entity'),
    **dict.fromkeys((
        'Mode', 'Ticks', 'Dead', 'PoseChecks', 'StagedChecks', 'OwnWrites',
        'OtherChecks', 'HistoricalReads', 'HistoricalInitialized', 'MalformedChecks', 'ObservedMode', 'LeftEnabled',
        'RightEnabled', 'LeftRequestedEnabled', 'RightRequestedEnabled', 'LeftHistoricalEnabled', 'LeftHistoricalSpace', 'LeftHistoricalClamped',
        'LeftHistoricalFallback', 'LeftHistoricalHasOrientation', 'LeftHistoricalRequestedEnabled', 'RightHistoricalEnabled', 'RightHistoricalSpace', 'RightHistoricalClamped',
        'RightHistoricalFallback', 'RightHistoricalHasOrientation', 'RightHistoricalRequestedEnabled',
    ), 'int32'),
    **dict.fromkeys((
        'FailAt', 'ObservedTick', 'DeathTick', 'PelvisBody', 'LeftHipBody', 'LeftKneeBody',
        'LeftFootBody', 'RightHipBody', 'RightKneeBody', 'RightFootBody',
    ), 'int64'),
    **dict.fromkeys((
        'CommittedYaw', 'CommittedPitch', 'ObservedYaw', 'ObservedPitch', 'ActorX', 'ActorY',
        'ActorZ', 'CameraX', 'CameraY', 'CameraZ', 'LeftRequestedWeight', 'RightRequestedWeight',
        'LeftWeight', 'RightWeight', 'LeftTargetError', 'RightTargetError', 'LeftCallbackGap', 'RightCallbackGap',
        'LeftAngularError', 'RightAngularError', 'OtherEndX', 'OtherEndY', 'OtherEndZ', 'LeftHistoricalWeight',
        'LeftHistoricalTargetX', 'LeftHistoricalTargetY', 'LeftHistoricalTargetZ', 'LeftHistoricalPoleX', 'LeftHistoricalPoleY', 'LeftHistoricalPoleZ',
        'LeftHistoricalEndX', 'LeftHistoricalEndY', 'LeftHistoricalEndZ', 'LeftHistoricalError', 'LeftHistoricalGoalX', 'LeftHistoricalGoalY',
        'LeftHistoricalGoalZ', 'LeftHistoricalGoalW', 'LeftHistoricalOrientationWeight', 'LeftHistoricalActualX', 'LeftHistoricalActualY', 'LeftHistoricalActualZ',
        'LeftHistoricalActualW', 'LeftHistoricalAngularError', 'LeftHistoricalRequestedWeight', 'RightHistoricalWeight', 'RightHistoricalTargetX', 'RightHistoricalTargetY',
        'RightHistoricalTargetZ', 'RightHistoricalPoleX', 'RightHistoricalPoleY', 'RightHistoricalPoleZ', 'RightHistoricalEndX', 'RightHistoricalEndY',
        'RightHistoricalEndZ', 'RightHistoricalError', 'RightHistoricalGoalX', 'RightHistoricalGoalY', 'RightHistoricalGoalZ', 'RightHistoricalGoalW',
        'RightHistoricalOrientationWeight', 'RightHistoricalActualX', 'RightHistoricalActualY', 'RightHistoricalActualZ', 'RightHistoricalActualW', 'RightHistoricalAngularError',
        'RightHistoricalRequestedWeight',
    ), 'float64'),
}
need, exact, fresh, near, vector, node = f.need, f.exact, prior.fresh, manual.near, manual.vector, manual.node


def typed(values):
    need(type(values) is dict and set(values)==set(FIELDS), 'Complete grounded managed field membership differs')
    for name,kind in FIELDS.items():
        v=values[name]
        valid=(type(v) is str and re.fullmatch('[0-9a-f]{32}',v) is not None) if kind=='entity' else (
            type(v) is int and -(2**31)<=v<2**31) if kind=='int32' else (
            type(v) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',v) is not None and -(2**63)<=int(v)<2**63) if kind=='int64' else type(v) is float and math.isfinite(v)
        need(valid,'Grounded managed field type/value differs: '+name)
    return values


def transform(position,rotation=(0,0,0,1),scale=(1,1,1)):
    return dict(position=list(position),rotation=list(rotation),scale=list(scale))


def source_asset():
    """Original reviewable seven-node recipe; no downloaded/generated art."""
    blob,views,accessors=bytearray(),[],[]
    def accessor(rows,kind,fmt='f'):
        while len(blob)%4:blob.append(0)
        start=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+fmt*len(flat),*flat))
        views.append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start))
        accessors.append(dict(bufferView=len(views)-1,componentType=5126 if fmt=='f' else 5123,count=len(rows),type=kind))
        return len(accessors)-1
    positions=accessor([(0,0,0),(.0625,0,0),(0,.0625,0)],'VEC3');accessors[positions].update(min=[0,0,0],max=[.0625,.0625,0])
    normals=accessor([(0,0,1)]*3,'VEC3');indices=accessor([(0,),(1,),(2,)],'SCALAR','H')
    times=accessor([(0,),(2,)],'SCALAR');accessors[times].update(min=[0],max=[2])
    translations=accessor([(0,1,0),(.015,1,0)],'VEC3');scales=accessor([(1,1,1),(1.000004,)*3],'VEC3')
    layer=accessor([(0,1,0),(.01,1,0)],'VEC3')
    nodes=[dict(name=name,translation=list(POSITIONS[i])) for i,name in enumerate(NAMES)];nodes[0]['mesh']=0
    for i in range(7):
        children=[n for n in range(7) if PARENTS[n]==i]
        if children:nodes[i]['children']=children
    document=dict(asset=dict(version='2.0',generator='Poima original analytic two-leg grounded handoff recipe'),
        buffers=[dict(byteLength=len(blob))],bufferViews=views,accessors=accessors,nodes=nodes,
        meshes=[dict(primitives=[dict(attributes=dict(POSITION=positions,NORMAL=normals),indices=indices)])],scenes=[dict(nodes=[0])],scene=0,
        animations=[dict(name='Original bounded source',samplers=[dict(input=times,output=translations,interpolation='LINEAR'),dict(input=times,output=scales,interpolation='LINEAR')],
            channels=[dict(sampler=0,target=dict(node=0,path='translation')),dict(sampler=1,target=dict(node=0,path='scale'))]),
            dict(name='Distinct additive translation clock',samplers=[dict(input=times,output=layer,interpolation='LINEAR')],channels=[dict(sampler=0,target=dict(node=0,path='translation'))])])
    return glb(document,bytes(blob))


def profile():
    frame=lambda p:dict(position=list(p),rotation=[0,0,0,1])
    parts=[dict(name=name,node=i,body_in_bone=frame((0,0,0)),shape=dict(kind='sphere',radius=.04),mass=MASSES[i],friction=.5,restitution=0,linear_damping=0,angular_damping=0) for i,name in enumerate(NAMES)]
    joints=[dict(name='joint'+str(i),parent=NAMES[PARENTS[i]],child=NAMES[i],parent_frame=frame(POSITIONS[i]),child_frame=frame((0,0,0)),
        limits=dict(kind='swing_twist',swing_y=3,swing_z=3,twist_min=-3,twist_max=3),neutral_rotation=[0,0,0,1],rest_stiffness=0,rest_damping=0,
        soft_limit_stiffness=0,soft_limit_zone=.1,max_torque=1000,friction_torque=0) for i in range(1,7)]
    return dict(root='pelvis',initial_mode='animated',parts=parts,joints=joints,pose_frame='skeletal_bounded_scale',max_scale_deviation=1e-5,
        controller_handoff=dict(actor=ACTOR,follow='translate_keep_heading'),procedural_pose='grounded_animated_only')


def configure(io,report):
    source=io.directory/'original-two-leg.glb';source.write_bytes(source_asset());asset=io.rpc('asset.import',dict(source=f.native(source)))['asset']
    need(type(asset) is str and re.fullmatch('[0-9a-f]{64}',asset),'Cooked original analytic identity absent')
    need(f.transact(io,[dict(op='asset.instantiate',id=identity,name='Original analytic '+identity,asset=asset) for identity in (RIG,OTHER)],0)['revision']==1,'Asset instances did not commit revision1')
    document=strict_json(io.world.read_bytes());c=f.component
    create=lambda identity,name,parent=None:dict(op='entity.create',id=identity,name=name,parent=parent)
    collider=lambda half,motion:dict(half_extents=half,motion=motion,mass=1,friction=.5,restitution=0)
    operations=[create(ACTOR,'Grounded virtual actor'),c(ACTOR,'Transform',transform((.6,.1,.25))),
        c(ACTOR,'CharacterController',dict(radius=.3,height=1.8,speed=2,jump_speed=5,camera=CAMERA,locomotion={},stance=dict(crouched_height=1,crouched_speed=1,camera_drop=.5))),
        create(CAMERA,'Separate owned eye',ACTOR),c(CAMERA,'Transform',transform((.05,1.6,.02))),c(CAMERA,'Camera',dict(vertical_fov=70,near=.05,far=100)),
        create(FLOOR,'Independent real floor'),c(FLOOR,'Transform',transform((8,-.5,0))),c(FLOOR,'BoxCollider',collider([2,.5,2],'static')),
        dict(op='entity.reparent',id=RIG,parent=ACTOR,mode='keep_local'),c(RIG,'Transform',transform((0,0,0))),c(OTHER,'Transform',transform((8,0,0))),
        dict(op='template.set',id=RECIPE,name='Retirable native support',components=dict(Transform=transform((0,-.5,0),scale=(1.5,1,1.25)),BoxCollider=collider([3,.5,3],'kinematic')))]
    for identity in (RIG,OTHER):
        animation=copy.deepcopy(document['entities'][identity]['components']['AnimationRig']);chains=[]
        for slot in ((1,2) if identity==RIG else (1,)):
            base=1+3*(slot-1);chains.append(dict(slot=slot,root=base,middle=base+1,end=base+2,enabled=True,space='rig',weight=1,
                target=[-.3 if slot==1 else .3,.4,0],pole=[-1 if slot==1 else 1,1,0],metric='near-uniform-v1',
                grounding=dict(sole_offset=[0,-.2,0],sole_up=[0,1,0],sole_forward=[0,0,1],reference_height=.2,
                    plant_height=.03,lift_height=.15,probe_above=.3,probe_below=.6,max_correction=.6,max_slope_degrees=55,plant_lock=identity==RIG)))
        animation.update(clip=0,time=0,speed=1,loop=True,playing=True,ik=chains,
            layers=[dict(slot=1,mode='additive',clip=1,time=.15,speed=.5,loop=True,playing=True,weight=.2,mask=[dict(node=0,weight=1)],reference_clip=1,reference_time=0)])
        operations.append(c(identity,'AnimationRig',animation))
    operations.append(c(RIG,'PhysicalRig',profile()));need(f.transact(io,operations,1)['revision']==2,'Grounded authoring did not commit revision2')
    document=strict_json(io.world.read_bytes());mapping={}
    for identity in (RIG,OTHER):
        actual={e['components']['RigNode']['node']:key for key,e in document['entities'].items() if e['components'].get('RigNode',{}).get('rig')==identity}
        need(actual=={i:node(identity,i) for i in range(7)},'Complete cooked seven-node mapping differs');mapping[identity]=actual
    package=Path(str(io.world)+'.assets')/(asset+'.pmodel');need(package.is_file() and package.stat().st_size<=1024*1024,'Cooked analytic package absent/oversized')
    report['fixture']=dict(asset=asset,source=str(source),source_sha256=sha(source),package=str(package),package_sha256=sha(package),
        authored_world=str(io.world),authored_sha256=sha(io.world),revision=2,nodes=mapping,entities=sorted(document['entities']))
    expected={ACTOR,CAMERA,RIG,OTHER,FLOOR}
    expected.update(node(identity,i) for identity in (RIG,OTHER) for i in range(7))
    expected.update(hashlib.sha256(f'poima.instance.v1/{identity}/node/0/primitive/0'.encode()).hexdigest()[:32] for identity in (RIG,OTHER))
    need(len(expected)==21 and set(document['entities'])==expected,'Complete authored analytic inventory differs');return mapping


class Consumer(prior.Consumer):
    def game(self):
        result=self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True));m=result['module']
        need(m['backend']=='coreclr' and m['assembly_sha256']==self.report['assembly_sha256'] and m['schema']['identity']==IDENTITY,'Actual CoreCLR image/schema differs')
        fields=m['schema']['fields'];need(len(fields)==len(FIELDS) and {x['name']:x['kind'] for x in fields}==FIELDS,'Complete generated field membership differs')
        offset,layout=0,{}
        for name,kind in FIELDS.items():
            size=16 if kind=='entity' else 4 if kind=='int32' else 8;align=min(size,8);offset=(offset+align-1)//align*align
            layout[name]=dict(name=name,kind=kind,offset=offset,bytes=size);offset+=size
        need(type(m['schema']['bytes']) is int and m['schema']['bytes']==(offset+7)//8*8==920,'Complete generated 8-byte layout differs')
        for row in fields:need(exact(row,layout[row['name']]),'Exact generated field layout differs: '+row['name'])
        typed(m['values']);return result

    def edit(self,**changes):
        need(set(changes)<=set(('Mode','FailAt','LeftRoot','LeftMiddle','LeftEnd','RightRoot','RightMiddle','RightEnd','OtherEnd','Support')),'Verifier edited noncontrol/nonbinding fields')
        before=self.game();self.io.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,
            expected_revision=before['revision'],expected_structure_revision=self.inspect()['structure_revision'],values=changes));after=self.values()
        for key,value in changes.items():need(type(after[key]) is type(value) and exact(after[key],value),'Typed control edit coerced: '+key)

    def step(self,ticks=1,inputs=(),error=None,**extra):
        result=self.io.rpc('runtime.step',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,ticks=ticks,inputs=list(inputs),
            expected_structure_revision=self.inspect()['structure_revision'],**extra),error)
        if error is None:
            need(type(result['tick']) is int and result['tick']==self.tick+ticks and result['replayed'] is False and result['current_session_id']==self.session and result['current_tick']==result['tick'],'Accepted step receipt differs')
            self.tick=result['tick']
        return result

    def query(self,method,**params):
        result=self.io.rpc(method,dict(session_id=self.session,tick=self.tick,**params))
        need(set(result)=={'session_id','tick','source','state'} and result['session_id']==self.session and type(result['tick']) is int and result['tick']==self.tick and result['source']=='runtime','Native query envelope differs: '+method)
        return result['state']

    def handoff(self):
        s=self.query('runtime.controller_handoff',entity=RIG)
        need(type(s) is dict and set(s)=={'entity','actor','phase','capsule_body_id','capsule_enrolled','takeover_tick','support'} and s['entity']==RIG and s['actor']==ACTOR,'Complete handoff membership differs')
        need(type(s['capsule_body_id']) is int and 0<=s['capsule_body_id']<=0xffffffff and type(s['capsule_enrolled']) is bool,'Handoff native ID/flag differs')
        if s['phase']=='locomotion':need(s['capsule_enrolled'] is True and s['takeover_tick'] is None and s['support'] is None,'Animated handoff differs')
        else:
            need(s['phase']=='physical' and s['capsule_enrolled'] is False and type(s['takeover_tick']) is int,'Passive handoff differs');g=s['support']
            need(type(g) is dict and set(g)=={'semantics','ground','body_id','normal','velocity'} and g['semantics']=='cached_at_takeover' and type(g['body_id']) is int and 0<=g['body_id']<=0xffffffff and g['ground'] in ('on_ground','steep_ground','not_supported','in_air'),'Complete cached controller support differs')
            vector(g['normal'],3);vector(g['velocity'],3)
        return s

    def grounding(self,identity=RIG,slot=1):
        s=self.query('runtime.grounding',entity=identity,slot=slot)
        need(type(s) is dict and set(s)=={'entity','slot','enabled','weight'} and s['entity']==identity and type(s['slot']) is int and s['slot']==slot and type(s['enabled']) is bool and type(s['weight']) is float and 0<=s['weight']<=1,'Requested-only grounding DTO differs')
        return s

    def ik(self,identity=RIG,slot=1):
        s=self.query('runtime.ik',entity=identity,slot=slot,include_end_orientation=True,include_metric=True,include_grounding=True,include_planting=True)
        keys={'entity','slot','root','middle','end','enabled','space','target','pole','weight','end_position','target_error','clamped','pole_fallback','end_orientation','metric','grounding','planting'}
        need(type(s) is dict and set(s)==keys and s['entity']==identity and [s['slot'],s['root'],s['middle'],s['end']]==[slot,1+3*(slot-1),2+3*(slot-1),3+3*(slot-1)] and all(type(s[k]) is int for k in ('slot','root','middle','end')),'Complete IK chain membership/types differ')
        for k in ('enabled','clamped','pole_fallback'):need(type(s[k]) is bool,'IK flag coerced')
        need(s['space']=='world','Generated grounded controls are not world-space')
        for k in ('target','pole','end_position'):vector(s[k],3)
        for k in ('weight','target_error'):need(type(s[k]) is float and math.isfinite(s[k]),'IK finite scalar differs')
        o=s['end_orientation']
        if o is not None:
            need(type(o) is dict and set(o)=={'rotation','weight','actual_rotation','angular_error'},'Oriented state membership differs');vector(o['rotation'],4);vector(o['actual_rotation'],4)
            for k in ('weight','angular_error'):need(type(o[k]) is float and math.isfinite(o[k]),'Orientation scalar differs')
        m=s['metric'];need(type(m) is dict and set(m)=={'profile','evaluated','relative_limit','local_scale_error','frame_error','gram_error','unweighted_position_error','unweighted_position_limit'} and m['profile']=='near-uniform-v1' and type(m['evaluated']) is bool,'Complete metric DTO differs')
        for k in set(m)-{'profile','evaluated'}:need(m[k] is None or type(m[k]) is float and math.isfinite(m[k]),'Metric scalar differs')
        g=s['grounding'];need(type(g) is dict and set(g)=={'decision_tick','casts','status','cast','weight','input_sole','ray_origin','hit','actual_sole','actual_normal','position_error','normal_error'},'Complete native contact DTO differs')
        for k in ('decision_tick','casts'):need(type(g[k]) is int and g[k]>=0,'Contact counter coerced')
        need(type(g['status']) is str and type(g['cast']) is bool and type(g['weight']) is float,'Contact scalar types differ')
        for k in ('input_sole','ray_origin','actual_sole','actual_normal'):vector(g[k],3)
        for k in ('position_error','normal_error'):need(g[k] is None or type(g[k]) is float and math.isfinite(g[k]),'Contact residual differs')
        h=g['hit']
        if h is not None:
            need(type(h) is dict and set(h)=={'entity','position','normal','triangle'} and type(h['entity']) is str and re.fullmatch('[0-9a-f]{32}',h['entity']) and (h['triangle'] is None or type(h['triangle']) is int),'Complete native hit DTO differs');vector(h['position'],3);vector(h['normal'],3)
        p=s['planting']
        if p is not None:
            need(type(p) is dict and set(p)=={'armed','status','anchor','target_sole','target_normal','target_forward','position_error','normal_error'} and type(p['armed']) is bool and type(p['status']) is str,'Complete plant DTO differs')
            for k in ('target_sole','target_normal','target_forward'):
                if p[k] is not None:vector(p[k],3)
            for k in ('position_error','normal_error'):need(p[k] is None or type(p[k]) is float and math.isfinite(p[k]),'Plant residual type differs')
            anchor=p['anchor']
            if anchor is not None:
                need(type(anchor) is dict and set(anchor)=={'acquired_tick','support','triangle','local_point','local_normal','local_forward'} and type(anchor['acquired_tick']) is int and type(anchor['support']) is str and re.fullmatch('[0-9a-f]{32}',anchor['support']) and (anchor['triangle'] is None or type(anchor['triangle']) is int),'Complete plant anchor differs')
                for k in ('local_point','local_normal','local_forward'):vector(anchor[k],3)
        return s

    def procedural(self):
        s=self.query('runtime.procedural_pose',entity=RIG)
        need(type(s) is dict and set(s)=={'entity','phase','diagnostics','sampled_tick','slots'} and s['entity']==RIG and type(s['sampled_tick']) is int and exact(s['slots'],[1,2]),'Complete procedural ownership DTO differs')
        need((s['phase']=='animated' and s['diagnostics']=='current' and s['sampled_tick']==self.tick) or (s['phase']=='physical' and s['diagnostics']=='last_animated_pose' and s['sampled_tick']<self.tick),'Procedural diagnostics age/phase differs');return s

    def physical(self):
        s=self.query('runtime.physical_rig',entity=RIG,include_parts=True,include_joints=True)
        need(type(s) is dict and set(s)=={'entity','mode','body_count','joint_count','parts','joints'} and s['entity']==RIG and s['mode'] in ('animated','passive') and type(s['body_count']) is int and s['body_count']==7 and type(s['joint_count']) is int and s['joint_count']==6 and len(s['parts'])==7 and len(s['joints'])==6,'Complete physical summary differs')
        for i,p in enumerate(s['parts']):
            need(set(p)==f.PART_FIELDS and p['name']==NAMES[i] and type(p['node']) is int and p['node']==i and p['node_entity']==node(RIG,i) and type(p['body_id']) is int and 0<=p['body_id']<=0xffffffff,'Complete physical part identity differs')
            for k in ('enrolled','active','sleeping','at_linear_velocity_limit','at_angular_velocity_limit'):need(type(p[k]) is bool,'Physical flag coerced')
            need(p['enrolled'] is (s['mode']=='passive'),'Physical enrollment differs')
            for k,n in (('position',3),('rotation',4),('linear_velocity',3),('angular_velocity',3),('inertia',9)):vector(p[k],n)
            for k in ('mass','max_linear_velocity','max_angular_velocity','linear_damping','angular_damping','friction','restitution'):need(type(p[k]) is float and math.isfinite(p[k]),'Physical part scalar differs')
            need(p['mass']==MASSES[i],'Frozen physical mass differs')
        for i,j in enumerate(s['joints'],1):
            need(set(j)==f.JOINT_FIELDS and (j['name'],j['parent'],j['child'])==('joint'+str(i),NAMES[PARENTS[i]],NAMES[i]) and type(j['hinge']) is bool and j['hinge'] is False,'Complete physical joint identity differs')
            for k,n in (('relative_rotation',4),('relative_angular_velocity',3),('evaluated_passive_child_torque',3)):vector(j[k],n)
            for k in f.JOINT_FIELDS-{'name','parent','child','hinge','relative_rotation','relative_angular_velocity','evaluated_passive_child_torque'}:need(type(j[k]) is float and math.isfinite(j[k]),'Physical joint scalar differs')
        return s

    def structure(self,spawns=(),despawns=()):
        return self.io.rpc('runtime.structure.transact',dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,
            expected_structure_revision=self.inspect()['structure_revision'],spawns=list(spawns),despawns=list(despawns)))

    def snapshot(self,saves,label):
        inspect,game=self.inspect(),self.game();slot='grounded-'+str(len(self.report['archives'])+1)
        receipt=self.io.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=self.session,
            expected_tick=self.tick,expected_gameplay_revision=game['revision'],expected_structure_revision=inspect['structure_revision'],expected_material_revision=inspect['material_revision'],expected_ui_revision=inspect['ui_revision'],expected_control_sequence=inspect['control_sequence']))
        manifest_path=saves/('slot-'+slot)/'current.json';need(manifest_path.stat().st_size<=1024*1024,'Save manifest exceeds1MiB');manifest=strict_json(manifest_path.read_bytes());member=manifest['payload']['current']
        need(type(member['file']) is str and re.fullmatch(r'p-[0-9a-f]{32}\.bin',member['file']),'Unsafe complete payload name');path=manifest_path.parent/member['file'];raw=path.read_bytes();archive=strict_json(raw);p=archive['snapshot']['payload']
        need(len(raw)<=32*1024*1024 and type(receipt['bytes']) is int and type(member['bytes']) is int and len(raw)==receipt['bytes']==member['bytes'] and sha(path)==receipt['sha256']==member['sha256'],'Complete archive/manifest/receipt identities differ')
        need(archive['format']=='poima.world-save' and type(archive['version']) is int and archive['version']==1 and type(p['tick']) is int and p['tick']==self.tick and p['gameplay']['backend']=='coreclr' and p['gameplay']['assembly_sha256']==self.report['assembly_sha256'] and p['gameplay']['type']==GAME and exact(typed(p['gameplay']['values']),game['module']['values']),'Complete typed saved image/state differs')
        need(p['physical']['version']==4 and len(p['physical']['rigs'])==1,'Grounded physical extension4 differs')
        row=p['physical']['rigs'][0];capture=row['procedural_capture'];need(row['entity']==RIG and capture['version']==2 and capture['grounding']['prepared'] is True,'Actual complete prepared capture absent')
        observation=self.observe(label,p)
        need(type(capture['states']) is list and len(capture['states'])==2 and [s['slot'] for s in capture['states']]==[1,2] and all(type(s['slot']) is int and s['entity']==RIG for s in capture['states']),'Complete saved two-slot owner/order membership differs')
        for saved,current in zip(capture['states'],observation['feet']):
            need(set(saved)=={'entity','slot','root','middle','end','enabled','space','target','pole','weight','end_position','target_error','clamped','pole_fallback','end_orientation','metric','grounding'},'Complete saved cached state differs')
            for key in set(saved)-{'grounding','end_orientation','metric'}:need(exact(saved[key],current[key]),'Saved cache/public native point differs: '+key)
            need(exact(saved['grounding']['state'],current['grounding']) and exact(saved['grounding']['planting'],current['planting']),'Saved complete grounding/planting differs from literal native inspection')
            o=saved['end_orientation'];q=current['end_orientation'];need((o is None)==(q is None),'Saved/public orientation membership differs')
            if o is not None:need(exact(o['goal'],dict(rotation=q['rotation'],weight=q['weight'])) and exact(o['actual_rotation'],q['actual_rotation']) and exact(o['angular_error'],q['angular_error']),'Saved/public complete orientation differs')
            for key,value in saved['metric'].items():need(exact(value,current['metric'][key]),'Saved/public metric differs: '+key)
        if observation['procedural']['phase']=='physical':need(exact(capture,row['procedural_terminal']),'Passive capture is not exactly the frozen terminal')
        else:need(row['procedural_terminal'] is None and capture['captured_tick']==self.tick,'Animated capture is not current')
        record=dict(label=label,slot=slot,tick=self.tick,path=str(path),bytes=len(raw),sha256=sha(path),archive=archive,receipt=receipt,manifest_path=str(manifest_path),manifest=manifest,observation=observation)
        self.report['archives'].append(copy.deepcopy(record));return record

    def observe(self,label,payload):
        ids=[e['id'] for e in payload['entities']];need(len(ids)==len(set(ids))==self.inspect()['entities'],'Complete saved/live entity population differs')
        entities={identity:self.entity(identity) for identity in ids};physical=self.physical()
        state=dict(label=label,session_id=self.session,tick=self.tick,game=self.game(),feet=[self.ik(slot=s) for s in (1,2)],requested=[self.grounding(slot=s) for s in (1,2)],other_ik=self.ik(OTHER),other_requested=self.grounding(OTHER),procedural=self.procedural(),handoff=self.handoff(),physical=physical,entities=entities)
        for e in payload['entities']:need(exact(e['local'],entities[e['id']]['local_transform']),'Complete saved/native local entity transform differs')
        if physical['mode']=='passive':
            for part in physical['parts']:
                for a,b in zip(part['position'],entities[part['node_entity']]['world_matrix'][12:15]):near(a,b,2e-6)
        need(self.tick<120,'Original bounded-scale oracle exceeded its declared source interval')
        scalar=struct.unpack('<f',struct.pack('<f',1.000004))[0]
        for identity in (RIG,OTHER):
            for i in range(7):
                for value in entities[node(identity,i)]['local_transform']['scale']:near(value,1+(scalar-1)*self.tick/120 if i==0 else 1.,3e-15)
            a=entities[identity]['animation'];near(a['time'],self.tick/60,2e-14);need(a['clip']==0 and a['speed']==1.0 and len(a['layers'])==1,'Complete distinct base/layer playback absent')
            layer=a['layers'][0];need(layer['slot']==1 and layer['mode']=='additive' and layer['playback']['clip']==1 and layer['playback']['speed']==.5,'Distinct layer configuration lost');near(layer['playback']['time'],.15+self.tick/120,2e-14)
        self.report['observations'].append(copy.deepcopy(state));return state


def pair(report,label,a,b):
    manual.pair(report,label,a,b)


def body_id(archive,identity):
    rows=[x for x in archive['snapshot']['payload']['physical']['bootstrap']['bodies'] if x['entity']==identity]
    need(len(rows)==1 and type(rows[0]['id']) is int,'Actual saved native support binding absent');return rows[0]['id']


def contact(c,support):
    feet=[c.ik(slot=s) for s in (1,2)]
    for s in feet:
        g,p=s['grounding'],s['planting'];need(g['status']=='contact' and g['cast'] is True and g['hit'] is not None and g['hit']['entity']==support and g['decision_tick']==c.tick and g['casts']>0 and g['input_sole'][1]>.15,'Actual complete current ground contact/correction precondition absent')
        need(p is not None and p['status']=='locked' and p['armed'] is False and p['anchor'] is not None and p['anchor']['support']==support and all(p[k] is not None for k in ('target_sole','target_normal','target_forward')),'Actual support-relative plant lock absent')
    other=c.ik(OTHER);need(other['grounding']['status']=='contact' and other['grounding']['cast'] is True and other['grounding']['decision_tick']==c.tick and other['grounding']['hit']['entity']==FLOOR and other['planting'] is None,'Independent live grounding precondition absent')
    return feet


def historical(values,feet,requested):
    need(values['HistoricalInitialized']==1 and values['HistoricalReads']>0,'Actual C# historical reads/capture did not execute')
    for side,s,r in zip(('Left','Right'),feet,requested):
        p=side+'Historical'
        for key,expected in (('Enabled',int(s['enabled'])),('Space',0),('Clamped',int(s['clamped'])),('Fallback',int(s['pole_fallback'])),('RequestedEnabled',int(r['enabled']))):
            need(type(values[p+key]) is int and values[p+key]==expected,'C# historical enum/flag differs: '+p+key)
        for prefix,member in (('Target','target'),('Pole','pole'),('End','end_position')):need(exact([values[p+prefix+axis] for axis in 'XYZ'],s[member]),'Complete C# historical vector differs')
        for key,expected in (('Weight',s['weight']),('Error',s['target_error']),('RequestedWeight',r['weight'])):need(exact(values[p+key],expected),'C# historical scalar differs')
        o=s['end_orientation'];need(o is not None and values[p+'HasOrientation']==1,'Generated historical sole frame absent')
        need(exact([values[p+'Goal'+a] for a in 'XYZW'],o['rotation']) and exact([values[p+'Actual'+a] for a in 'XYZW'],o['actual_rotation']) and exact(values[p+'OrientationWeight'],o['weight']) and exact(values[p+'AngularError'],o['angular_error']),'Complete C# historical orientation differs')


def verify_closed(report):
    need(len(report['owners'])==3,'Exactly three fresh sequential actual CLI owners required');report['recordings']=[]
    for owner in report['owners']:
        need(owner['natural_exit'] is True and owner['forced_cleanup'] is False and owner['exit_code']==0 and owner['reader_failure'] is None,'Owner did not close naturally before verification')
        directory=Path(owner['directory']);calls=[x for x in report['calls'] if Path(x['owner'])==directory]
        for name,field in (('requests','request'),('responses','response')):
            path=directory/(name+'.jsonl');rows=[strict_json(line) for line in path.read_bytes().splitlines() if line.strip()]
            need(exact(rows,[x[field] for x in calls]),'Postclosure literal RPC stream differs: '+name);report['recordings'].append(dict(path=str(path),bytes=path.stat().st_size,sha256=sha(path)))
    for row in report['archives']:
        path,manifest=Path(row['path']),Path(row['manifest_path']);need(path.stat().st_size==row['bytes'] and sha(path)==row['sha256'] and exact(strict_json(path.read_bytes()),row['archive']) and exact(strict_json(manifest.read_bytes()),row['manifest']),'Postclosure complete archive/manifest differs')
        need(exact(typed(row['archive']['snapshot']['payload']['gameplay']['values']),row['observation']['game']['module']['values']),'Complete typed saved state lost after owner closure')
        need(any(x['request']['method']=='save.write' and x['request']['params'].get('slot')==row['slot'] and x['request']['params'].get('session_id')==row['observation']['session_id'] and x['request']['params'].get('expected_tick')==row['tick'] and 'result' in x['response'] and exact(x['response']['result'],row['receipt']) for x in report['calls']),'Archive receipt does not rejoin original literal reply')
    for row in report['whole_pairs']:need(Path(row['left']).read_bytes()==Path(row['right']).read_bytes() and sha(Path(row['left']))==row['sha256'] and row['exclusions']==[],'Postclosure complete byte pair differs')
    for o in report['observations']:
        bindings=[('runtime.gameplay.inspect',{},o['game'],False),('runtime.procedural_pose',dict(entity=RIG),o['procedural'],True),('runtime.controller_handoff',dict(entity=RIG),o['handoff'],True),('runtime.physical_rig',dict(entity=RIG),o['physical'],True),('runtime.ik',dict(entity=OTHER,slot=1),o['other_ik'],True),('runtime.grounding',dict(entity=OTHER,slot=1),o['other_requested'],True)]
        for slot in (1,2):bindings.extend([('runtime.ik',dict(entity=RIG,slot=slot),o['feet'][slot-1],True),('runtime.grounding',dict(entity=RIG,slot=slot),o['requested'][slot-1],True)])
        for method,params,state,nested in bindings:
            candidates=[x['response']['result'] for x in report['calls'] if x['request']['method']==method and x['request']['params'].get('session_id')==o['session_id'] and x['request']['params'].get('tick')==o['tick'] and all(exact(x['request']['params'].get(k),v) for k,v in params.items()) and 'result' in x['response']]
            need(any(exact(x['state'] if nested else x,state) for x in candidates),'Observation lacks full literal native reply: '+method)
        for identity,state in o['entities'].items():need(any(x['request']['method']=='runtime.entity' and x['request']['params']==dict(session_id=o['session_id'],tick=o['tick'],id=identity) and 'result' in x['response'] and exact(x['response']['result'],state) for x in report['calls']),'Complete entity observation lacks literal reply')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','hostfxr','bridge','assembly','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true');args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    f.ARGS,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED=args,time.monotonic()+420,3500,48*1024*1024
    report=dict(format='poima.grounded-handoff-coreclr.v1',passed=False,backend='coreclr',calls=[],owners=[],archives=[],observations=[],whole_pairs=[],source_pins={},assembly_sha256=sha(args.assembly),
        limits=dict(seconds=420,rpc=3500,per_owner_wire_bytes=48*1024*1024,output_bytes=192*1024*1024,archive_bytes=32*1024*1024),
        limitations=['Actual supplied CoreCLR assembly; deterministic seven-node/two-leg analytic native content and original procedural triangle.',
            'C# reads requested grounding controls and point/oriented IK; complete contact and planting caches are separately inspected through native CLI and whole saves.',
            'Three sequential fresh CLI owners beneath external process-tree ownership; no imported humanoid, Native AOT, graphics, physical input or performance qualification.'])
    f.REPORT=report;config=dict(hostfxr=f.native(args.hostfxr),bridge=f.native(args.bridge),assembly=f.native(args.assembly),type=GAME)
    files=[Path(__file__).resolve(),Path(f.__file__).resolve(),Path(prior.__file__).resolve(),Path(manual.__file__).resolve(),ROOT/'tests/managed_physical_gameplay_contract.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/animation_fixture.py',SAMPLE/'Game.cs',SAMPLE/'Poima.ManagedGroundedHandoffGame.csproj',ROOT/'managed/Directory.Build.props',args.binary.resolve(),args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve()]
    for image in (args.bridge.resolve(),args.assembly.resolve()):
        files.append(image.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json','.runtimeconfig.json'):
            if image.with_suffix(suffix).is_file():files.append(image.with_suffix(suffix))
    io=c=None
    try:
        for path in set(files):need(path.is_file(),'Pinned input absent: '+str(path));report['source_pins'][str(path)]=sha(path)
        need(sha(args.bridge.parent/'Poima.Gameplay.dll')==sha(args.assembly.parent/'Poima.Gameplay.dll'),'Bridge/consumer SDK images differ')
        io=f.Session(out/'warm');mapping=configure(io,report);world=io.world;saves=out/'saves';saves.mkdir()
        io.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=f.native(saves)));c=Consumer(io,report,config);io.rpc('runtime.start',dict(session_id=c.session,revision=2))
        support=c.structure(spawns=[dict(template_id=RECIPE)])['spawned'][0];need(support not in report['fixture']['entities'],'Spawned support reused authored identity');report['support']=support
        c.step(30);contact(c,support)
        io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=fresh(),expected_tick=c.tick,expected_revision=0,expected_structure_revision=c.inspect()['structure_revision'],**config))
        c.edit(LeftRoot=mapping[RIG][1],LeftMiddle=mapping[RIG][2],LeftEnd=mapping[RIG][3],RightRoot=mapping[RIG][4],RightMiddle=mapping[RIG][5],RightEnd=mapping[RIG][6],OtherEnd=mapping[OTHER][3],Support=support)
        c.step(inputs=[prior.input_for(ACTOR,(.025,.075),(2,-.25))]);contact(c,support);c.snapshot(saves,'initial-actual-contact')
        c.edit(Mode=1);c.step(inputs=[prior.input_for(ACTOR,(.025,.075),(3,-.25))]);c.snapshot(saves,'staged-distinct-requested')
        need(c.values()['LeftRequestedWeight']==1.0 and c.values()['RightRequestedWeight']==1.0 and c.values()['OwnWrites']==2,'Actual C# staged setters leaked or did not run')
        c.step();need([c.grounding(slot=s)['weight'] for s in (1,2)]==[.75,.65] and c.values()['LeftRequestedWeight']==.75 and c.values()['RightRequestedWeight']==.65,'Actual requested controls did not publish on regular boundary')
        motion=dict(entity=support,position=[.6,-.5,.2],rotation=[0,math.sin(.15),0,math.cos(.15)],duration_ticks=60)
        c.step(20,motions=[motion]);frozen=contact(c,support);requested=[c.grounding(slot=s) for s in (1,2)];alive=c.snapshot(saves,'alive-moving-planted-reference')
        actor_state=c.entity(ACTOR)
        need(actor_state['ground']=='on_ground' and c.values()['CommittedYaw']==5.0 and math.sqrt(sum(v*v for v in actor_state['locomotion']['ground_velocity']))>.1,'Actual supported turning/platform-velocity precondition absent')
        need([s['weight'] for s in frozen]==[.75,.65] and requested[0]['weight']==.75 and requested[1]['weight']==.65,'Generated weights and requested multipliers differ from staged policy')
        capture=alive['archive']['snapshot']['payload']['physical']['rigs'][0]['procedural_capture'];journal=capture['grounding']['journal'];raw_support=body_id(alive['archive'],support)
        need(len(journal)==4 and [x['kind'] for x in journal].count('ray')==2 and [x['kind'] for x in journal].count('support')==2,'Actual two-foot query history incomplete')
        for q in journal:
            b=q['binding'];need(b is not None and b['entity']==support and b['body_id']==raw_support and b['motion']=='kinematic','Actual native support binding absent')
            need(abs(alive['observation']['entities'][support]['world_matrix'][0]-b['frame'][0])>.2,'Nonuniform render scale did not differ from rigid native support frame')
        moving=[e for e in alive['archive']['snapshot']['payload']['entities'] if e['id']==support][0]['body'];need(math.sqrt(sum(v*v for v in moving['linear_velocity']))>.1 and abs(moving['angular_velocity'][1])>.1,'Support did not actually translate and rotate')
        held=[prior.input_for(ACTOR,(-.75,.5),(5,1),True)]
        for mode in (3,4,5,8):
            c.edit(Mode=mode);before=c.snapshot(saves,'reject-mode-'+str(mode)+'-before');c.step(inputs=held,error=-32040);pair(report,'complete callback rejection '+str(mode),before,c.snapshot(saves,'reject-mode-'+str(mode)+'-after'));c.restore(alive)
        c.edit(Mode=6);c.step();need(c.values()['MalformedChecks']==1,'Actual managed NaN multiplier was not caught');c.snapshot(saves,'malformed-managed-control-retained');c.restore(alive)
        c.edit(Mode=2,FailAt=str(c.tick+1));before=c.snapshot(saves,'late-death-before');c.step(2,held,error=-32040);pair(report,'whole late callback/ownership rollback',before,c.snapshot(saves,'late-death-after'));c.restore(alive)
        takeover_tick=alive['tick']

        def takeover(same_batch=False):
            c.edit(Mode=9 if same_batch else 2);before=c.tick;c.step(inputs=held,character_stances=[dict(entity=ACTOR,crouch=True)])
            v,h,p=c.values(),c.handoff(),c.procedural()
            need(v['Dead']==1 and v['DeathTick']==str(before) and v['ObservedMode']==0 and v['ObservedYaw']==10.0 and v['CommittedYaw']==5.0 and p['phase']=='physical' and p['sampled_tick']==before and h['takeover_tick']==before and h['capsule_enrolled'] is False,'Actual callback preview/committed takeover differs')
            need(exact([c.ik(slot=s) for s in (1,2)],frozen) and exact([c.grounding(slot=s) for s in (1,2)],requested),'Takeover replaced complete committed cached decisions/requests')
            need(v['Support']==('0'*32 if same_batch else support),'Compiled support binding did not follow live retirement')
            near(c.entity(ACTOR)['yaw'],5.,0);near(c.entity(ACTOR)['pitch'],-.5,0)
            if same_batch:io.rpc('runtime.entity',dict(session_id=c.session,tick=c.tick,id=support),-32004)
            return before

        def continuation(ticks):
            before=c.ik(OTHER)['grounding']['casts'];c.step(ticks,[prior.input_for(ACTOR,(1,-1),(37,-17),True)],character_stances=[dict(entity=ACTOR,crouch=True)])
            need(exact([c.ik(slot=s) for s in (1,2)],frozen) and exact([c.grounding(slot=s) for s in (1,2)],requested) and c.procedural()['sampled_tick']==takeover_tick,'Passive native caches or requests changed')
            need(c.ik(OTHER)['grounding']['casts']==before+ticks and c.ik(OTHER)['grounding']['decision_tick']==c.tick,'Independent live grounding stopped/duplicated')
            historical(c.values(),frozen,requested);near(c.entity(ACTOR)['yaw'],5.,0);near(c.entity(ACTOR)['pitch'],-.5,0)

        def replacement():
            born=c.structure(spawns=[dict(template_id=RECIPE,transform=transform((-8,-.5,0),scale=(1.5,1,1.25)))])['spawned'][0]
            need(born!=support,'Retired public support identity reused');row=c.snapshot(saves,'replacement-live-control')
            new=body_id(row['archive'],born);need((new&0x007fffff)==(raw_support&0x007fffff) and new!=raw_support,'Actual native slot was not reused with new generation')
            frozen_capture=row['archive']['snapshot']['payload']['physical']['rigs'][0]['procedural_capture'];need(exact(frozen_capture,capture),'Historical support capture rebound to replacement ID/frame')
            return row

        origin=c.entity(ACTOR)['world_matrix'][12:15];ray=dict(origin=[origin[0]+2,origin[1]+1.3,origin[2]],direction=[-1,0,0],distance=4)
        need(io.rpc('runtime.raycast',dict(session_id=c.session,tick=c.tick,**ray))['hit']['entity']==ACTOR,'Actual alive capsule ray absent')
        takeover();hit=io.rpc('runtime.raycast',dict(session_id=c.session,tick=c.tick,**ray))['hit'];need(hit is None or hit['entity']!=ACTOR,'Suppressed capsule remains query-visible')
        part=c.physical()['parts'][0];hit=io.rpc('runtime.raycast',dict(session_id=c.session,tick=c.tick,origin=[part['position'][0]+2,part['position'][1],part['position'][2]],direction=[-1,0,0],distance=4))['hit'];need(hit is not None and hit['entity']==RIG,'Actual enrolled physical part ray absent')
        passive=c.snapshot(saves,'passive-reference');c.edit(Mode=7);before=c.snapshot(saves,'passive-write-before');c.step(error=-32040);pair(report,'whole passive grounding rejection',before,c.snapshot(saves,'passive-write-after'));c.restore(passive)
        continuation(4);future=c.snapshot(saves,'warm-future');continuation(2);late=c.snapshot(saves,'warm-late')
        c.restore(future);c.edit(Mode=10);c.step();need(c.values()['Support']=='0'*32,'Compiled late retirement retained a live support binding');need(exact([c.ik(slot=s) for s in (1,2)],frozen),'Post-takeover support retirement changed historical cache');io.rpc('runtime.entity',dict(session_id=c.session,tick=c.tick,id=support),-32004)
        retired=c.snapshot(saves,'historical-support-retired');retired_replacement=replacement();continuation(2);retired_future=c.snapshot(saves,'retired-support-future')
        c.restore(passive);pair(report,'warm passive complete roundtrip',passive,c.snapshot(saves,'warm-passive-roundtrip'));continuation(4);pair(report,'warm passive complete future',future,c.snapshot(saves,'warm-passive-future'))
        c.restore(alive);takeover(True);same_batch=c.snapshot(saves,'same-batch-retired-passive');same_replacement=replacement();continuation(4);same_future=c.snapshot(saves,'same-batch-retired-future')
        io.close();io=None

        io=f.Session(out/'cold',world=world);c=Consumer(io,report,config);io.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=f.native(saves)))
        c.restore(alive,existing=False);pair(report,'cold active whole roundtrip',alive,c.snapshot(saves,'cold-active-roundtrip'));takeover();pair(report,'cold active immediate takeover',passive,c.snapshot(saves,'cold-active-takeover'));continuation(4);pair(report,'cold active complete future',future,c.snapshot(saves,'cold-active-future'));continuation(2);pair(report,'cold active complete late',late,c.snapshot(saves,'cold-active-late'))
        c.restore(passive);pair(report,'cold passive whole roundtrip',passive,c.snapshot(saves,'cold-passive-roundtrip'));continuation(4);pair(report,'cold passive complete future',future,c.snapshot(saves,'cold-passive-future'))
        c.restore(same_batch);pair(report,'cold samebatch whole roundtrip',same_batch,c.snapshot(saves,'cold-same-batch-roundtrip'));pair(report,'cold samebatch actual slotreuse',same_replacement,replacement());continuation(4);pair(report,'cold samebatch whole future',same_future,c.snapshot(saves,'cold-same-batch-future'))
        c.restore(retired_replacement);pair(report,'cold retired replacement whole roundtrip',retired_replacement,c.snapshot(saves,'cold-retired-roundtrip'));continuation(2);pair(report,'cold retired whole future',retired_future,c.snapshot(saves,'cold-retired-future'))
        io.close();io=None

        io=f.Session(out/'cold-historical',world=world);c=Consumer(io,report,config);io.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=f.native(saves)))
        c.restore(alive,existing=False);pair(report,'third active whole roundtrip',alive,c.snapshot(saves,'third-active-roundtrip'));takeover(True);pair(report,'third active samebatch takeover',same_batch,c.snapshot(saves,'third-same-batch-takeover'));pair(report,'third samebatch actual slotreuse',same_replacement,replacement());continuation(4);pair(report,'third samebatch complete future',same_future,c.snapshot(saves,'third-same-batch-future'))
        c.restore(future);pair(report,'third initialized history whole roundtrip',future,c.snapshot(saves,'third-historical-roundtrip'));continuation(2);pair(report,'third initialized history whole late',late,c.snapshot(saves,'third-historical-late'))
        c.restore(retired_replacement);pair(report,'third retired replacement whole roundtrip',retired_replacement,c.snapshot(saves,'third-retired-roundtrip'));continuation(2);pair(report,'third retired whole future',retired_future,c.snapshot(saves,'third-retired-future'))
        io.close();io=None;verify_closed(report)
        need(all(sha(Path(path))==digest for path,digest in report['source_pins'].items()) and sha(world)==report['fixture']['authored_sha256'] and sha(Path(report['fixture']['package']))==report['fixture']['package_sha256'] and sha(Path(report['fixture']['source']))==report['fixture']['source_sha256'],'Pinned source/image/authored/cooked content changed')
        report['checks']=['Actual116-field/920-byte CoreCLR consumer; requested grounding Get/Set, generated point/oriented IK, prepared yaw/FK/camera joins and staged getter visibility',
            'Actual two-foot contact/plant caches, translating/rotating rigid support versus nonuniform render scale; complete native DTO observations',
            'Activation conflicts, immediate/later callback failure, duplicate/malformed/passive/automatically-owned IK writes with complete rollback',
            'One-way committed-pose transfer; preserved native ground/plant/history and complete C# historical point/orientation/requested reads while unrelated grounding/source/layer clocks continue',
            'Both support retirement timings, actual body-index/new-generation reuse, complete warm/three-process alive/passive/historical saves and literal postclosure receipt/transcript joins']
        report['passed']=True
    except BaseException as error:
        report['error']=repr(error);raise
    finally:
        if io is not None:
            try:io.close()
            except BaseException as error:report['passed']=False;report['close_error']=repr(error)
        report['rpc_count']=len(report['calls']);report['natural_cli_owners']=sum(x.get('natural_exit') is True and x.get('exit_code')==0 and not x['forced_cleanup'] for x in report['owners'])
        raw=(json.dumps(report,indent=2,allow_nan=False)+'\n').encode();need(len(raw)<=64*1024*1024 and sum(p.stat().st_size for p in out.rglob('*') if p.is_file())+len(raw)<=192*1024*1024,'Bounded consumer output exceeded')
        (out/'evidence.json').write_bytes(raw);print(out/'evidence.json',flush=True)
        if not report['passed'] and 'error' not in report:raise RuntimeError(report.get('close_error','Consumer incomplete'))


if __name__=='__main__':main()
