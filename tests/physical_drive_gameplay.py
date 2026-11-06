#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded native CLI/CoreCLR joint-drive contract; builds and renders nothing.

An external process-tree supervisor must own this run. Uses real import,
authoring, compiled callbacks and complete saves. Retains literal protocol
records and original failures. Synthetic geometry is a small analytic fixture,
not a character-quality, balance, performance or standalone-shipping claim.
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

sys.dont_write_bytecode = True
import world_physical_contract as f
from gltf_fixture import glb
from managed_physical_gameplay_contract import sha, strict_json

ROOT = Path(__file__).resolve().parents[1]
GAME, IDENTITY = 'Poima.Tests.PhysicalDriveGame', 'poima.test.physical-rig-drive'
RIG, FLOOR = '0'*30+'10', '0'*31+'2'
SECONDS, CALLS, MIB = 240, 700, 1024**2
FIELDS = {
    **dict.fromkeys('Rig SelectedNode'.split(), 'entity'),
    **dict.fromkeys('Phase Joint Part Ticks ObservedMode BodyCount JointCount DrivePresent PartActive PartSleeping Writes CommittedReadChecks RejectAfterWrite'.split(), 'int32'),
    **dict.fromkeys('ObservedTick SelectedBody FailAt'.split(), 'int64'),
    **dict.fromkeys('TargetAngle Strength ObservedTargetX ObservedTargetY ObservedTargetZ ObservedTargetW ObservedStrength Stiffness Damping MaxTorque PartX PartY PartZ PartQx PartQy PartQz PartQw PartVx PartVy PartVz PartWx PartWy PartWz PartMass'.split(), 'float64'),
}
need, exact = f.need, f.exact


def uid():
    return uuid.uuid4().hex


def clone(value):
    return copy.deepcopy(value)


def original_member(raw, selected):
    """Locate original JSON member text without reformatting numeric spellings."""
    text = raw.decode('utf-8') if isinstance(raw,bytes) else raw
    decoder = json.JSONDecoder(); cursor = 0
    def white(at):
        while at < len(text) and text[at].isspace(): at += 1
        return at
    cursor = white(cursor); need(text[cursor]=='{','Original member needs an object'); cursor += 1
    while True:
        cursor = white(cursor)
        if text[cursor]=='}': break
        key,end = decoder.raw_decode(text,cursor); cursor = white(end)
        need(type(key) is str and text[cursor]==':','Original JSON member syntax'); cursor=white(cursor+1)
        start=cursor; _,cursor=decoder.raw_decode(text,cursor)
        if key==selected: return text[start:cursor]
        cursor=white(cursor)
        if text[cursor]=='}': break
        need(text[cursor]==',','Original JSON member separator'); cursor += 1
    raise AssertionError('Original JSON member missing: '+selected)


def node(index):
    return hashlib.sha256(f'poima.instance.v1/{RIG}/node/{index}'.encode()).hexdigest()[:32]


def initial_values():
    value = {key: '0'*32 if kind == 'entity' else '0' if kind == 'int64' else 0.0 if kind == 'float64' else 0 for key, kind in FIELDS.items()}
    value.update(Rig=RIG, Part=1, FailAt='-1', TargetAngle=.3, Strength=.5)
    return value


def validate(values, schema):
    need(type(values) is dict and set(values) == set(FIELDS) and len(FIELDS) == 42, 'Complete42-field state membership differs')
    offset, layout = 0, {}
    for name, kind in FIELDS.items():
        size = 16 if kind == 'entity' else 4 if kind == 'int32' else 8
        align = min(size, 8); offset = (offset+align-1)//align*align
        layout[name] = dict(name=name, kind=kind, offset=offset, bytes=size); offset += size
        value = values[name]
        valid = (type(value) is str and re.fullmatch('[0-9a-f]{32}', value) is not None) if kind == 'entity' else (
            type(value) is int and -(2**31) <= value < 2**31) if kind == 'int32' else (
            type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None and
            -(2**63) <= int(value) < 2**63) if kind == 'int64' else type(value) is float and math.isfinite(value)
        need(valid, 'Original typed value invalid: '+name)
    need(schema['identity'] == IDENTITY and schema['bytes'] == (offset+7)//8*8 and
         type(schema['fields']) is list and len(schema['fields']) == 42 and
         [row['name'] for row in schema['fields']] == sorted(FIELDS) and
         all(exact(row, layout[row['name']]) for row in schema['fields']), 'Complete native scalar layout differs')
    return values


def analytic_model():
    """Reviewable three-bone triangle recipe; original bind/curve values retained."""
    blob, views, accessors = bytearray(), [], []
    def accessor(rows, kind, fmt='f'):
        while len(blob) % 4: blob.append(0)
        start = len(blob); flat = [v for row in rows for v in row]
        blob.extend(struct.pack('<'+fmt*len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=start, byteLength=len(blob)-start))
        accessors.append(dict(bufferView=len(views)-1, componentType={'f':5126,'H':5123}[fmt], count=len(rows), type=kind))
        return len(accessors)-1
    position = accessor([(0,0,0),(.15,.5,0),(0,1,0)], 'VEC3')
    accessors[position].update(min=[0,0,0], max=[.15,1,0])
    attributes = dict(POSITION=position, NORMAL=accessor([(0,0,1)]*3,'VEC3'),
                      TEXCOORD_0=accessor([(0,0),(1,0),(0,1)],'VEC2'),
                      JOINTS_0=accessor([(0,0,0,0),(1,0,0,0),(2,0,0,0)],'VEC4','H'),
                      WEIGHTS_0=accessor([(1,0,0,0)]*3,'VEC4'))
    indices = accessor([(0,),(1,),(2,)],'SCALAR','H')
    inverse = []
    for y in (0,-.5,-1):
        inverse.append([1,0,0,0,0,1,0,0,0,0,1,0,0,y,0,1])
    binds = accessor(inverse,'MAT4'); times = accessor([(0,),(2,)],'SCALAR'); accessors[times].update(min=[0],max=[2])
    values = accessor([(0,0,0),(0,0,0)],'VEC3')
    document = dict(asset=dict(version='2.0', generator='Poima original analytic drive contract'),
        buffers=[dict(byteLength=len(blob))], bufferViews=views, accessors=accessors,
        meshes=[dict(primitives=[dict(attributes=attributes,indices=indices)])],
        nodes=[dict(name='pelvis',children=[1]),dict(name='arm',translation=[0,.5,0],children=[2]),
               dict(name='hand',translation=[0,.5,0]),dict(name='triangle',mesh=0,skin=0)],
        skins=[dict(name='analytic three-part bind',skeleton=0,joints=[0,1,2],inverseBindMatrices=binds)],
        animations=[dict(name='still',samplers=[dict(input=times,output=values,interpolation='LINEAR')],
                         channels=[dict(sampler=0,target=dict(node=0,path='translation'))])],
        scenes=[dict(nodes=[0,3])],scene=0)
    return glb(document,bytes(blob))


def profile():
    parts = [dict(name=name,node=index,body_in_bone=f.pose(),shape=dict(kind='sphere',radius=radius),mass=mass,
                  friction=0,restitution=0,linear_damping=0,angular_damping=0)
             for index,(name,radius,mass) in enumerate(zip(('pelvis','arm','hand'),(.12,.1,.08),(4,2,1)))]
    common = dict(parent_frame=f.pose((0,.25,0)),child_frame=f.pose((0,-.25,0)),neutral_rotation=[0,0,0,1],
                  rest_stiffness=0,rest_damping=0,soft_limit_stiffness=0,soft_limit_zone=.1,max_torque=1000,friction_torque=0,
                  drive=dict(stiffness=60,damping=6,max_torque=10))
    return dict(root='pelvis',initial_mode='animated',parts=parts,joints=[
        dict(clone(common),name='shoulder',parent='pelvis',child='arm',limits=dict(kind='swing_twist',swing_y=1.2,swing_z=1.2,twist_min=-.8,twist_max=.8)),
        dict(clone(common),name='elbow',parent='arm',child='hand',limits=dict(kind='hinge',minimum=-.8,maximum=1.2))])


def prepare(io):
    source = io.directory/'original-drive-triangle.glb'; source.write_bytes(analytic_model())
    imported = io.rpc('asset.import',dict(source=f.native(source)))
    need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives']) == (4,1,1,1), 'Complete original analytic import membership')
    asset = imported['asset']
    ops = [dict(op='asset.instantiate',id=RIG,name='Analytic joint-drive contract',asset=asset),
        f.component(RIG,'Transform',dict(**f.pose((0,200,0)),scale=[1,1,1])),
        f.component(RIG,'AnimationRig',dict(asset=asset,clip=0,time=0,speed=1,loop=False,playing=True)),
        dict(op='entity.create',id=FLOOR,name='Independent static floor'),
        f.component(FLOOR,'Transform',dict(**f.pose((0,-.5,0)),scale=[1,1,1])),
        f.component(FLOOR,'BoxCollider',dict(half_extents=[20,.5,20],motion='static',mass=1,friction=.5,restitution=0))]
    need(f.transact(io,ops,0)['revision'] == 1, 'Analytic imported graph authoring')
    need(f.transact(io,[f.component(RIG,'PhysicalRig',profile())],1)['revision'] == 2, 'Opt-in drive profile authoring')
    return dict(source=str(source),source_sha256=sha(source),asset=asset,profile=profile())


def projection(state):
    need(set(state) == {'entity','mode','body_count','joint_count','parts','joints'} and state['entity'] == RIG and
         state['mode'] in ('animated','passive','driven') and type(state['body_count']) is int and state['body_count'] == 3 and
         type(state['joint_count']) is int and state['joint_count'] == 2 and len(state['parts']) == 3 and len(state['joints']) == 2,
         'Complete physical projection mode/membership')
    for index,(p,name,mass) in enumerate(zip(state['parts'],('pelvis','arm','hand'),(4.,2.,1.))):
        need(set(p) == f.PART_FIELDS and p['name'] == name and type(p['node']) is int and p['node'] == index and
             p['node_entity'] == node(index) and type(p['body_id']) is int and 0 <= p['body_id'] <= 0x7fffffff,
             'Actual part/source-node/native-body join')
        for field,count in [('position',3),('rotation',4),('linear_velocity',3),('angular_velocity',3),('inertia',9)]:
            need(type(p[field]) is list and len(p[field]) == count and all(type(x) is float and math.isfinite(x) for x in p[field]), 'Part typed vector: '+field)
        for field in ('mass','max_linear_velocity','max_angular_velocity','linear_damping','angular_damping','friction','restitution'):
            need(type(p[field]) is float and math.isfinite(p[field]), 'Part typed number: '+field)
        need(p['mass'] == mass, 'Exact analytic powers-of-two mass')
        for field in ('enrolled','active','sleeping','at_linear_velocity_limit','at_angular_velocity_limit'):
            need(type(p[field]) is bool, 'Part flag coerced: '+field)
        need(p['enrolled'] == (state['mode'] != 'animated') and p['sleeping'] == (p['enrolled'] and not p['active']), 'Physical enrollment/sleep semantics')
    need(len({p['body_id'] for p in state['parts']}) == 3,'Physical parts alias native identities')
    for index,j in enumerate(state['joints']):
        need(set(j) == f.JOINT_FIELDS|{'drive'} and (j['name'],j['parent'],j['child'],j['hinge']) ==
             (('shoulder','pelvis','arm',False) if index == 0 else ('elbow','arm','hand',True)), 'Complete authored joint membership')
        need(type(j['hinge']) is bool,'Joint hinge flag coerced')
        vectors={'relative_rotation':4,'relative_angular_velocity':3,'evaluated_passive_child_torque':3}
        for key,count in vectors.items():
            need(type(j[key]) is list and len(j[key])==count and all(type(x) is float and math.isfinite(x) for x in j[key]),'Complete joint typed vector: '+key)
        for key in f.JOINT_FIELDS-{'name','parent','child','hinge'}-set(vectors):
            need(type(j[key]) is float and math.isfinite(j[key]),'Complete joint typed number: '+key)
        d = j['drive']
        need(set(d) == {'target_rotation','strength','stiffness','damping','max_torque','evaluated_child_torque','angular_error','saturation_scale'}, 'Complete drive diagnostics')
        for key,count in [('target_rotation',4),('evaluated_child_torque',3)]:
            need(type(d[key]) is list and len(d[key]) == count and all(type(x) is float and math.isfinite(x) for x in d[key]), 'Drive typed vector')
        for key in ('strength','stiffness','damping','max_torque','angular_error','saturation_scale'):
            need(type(d[key]) is float and math.isfinite(d[key]), 'Drive typed number')
        need((d['stiffness'],d['damping'],d['max_torque']) == (60.,6.,10.) and 0 <= d['strength'] <= 1 and
             0 <= d['saturation_scale'] <= 1 and math.hypot(*d['evaluated_child_torque']) <= d['strength']*10+1e-10, 'Authored gain/strength/torque bounds')
        if d['strength'] == 0: need(exact(d['evaluated_child_torque'],[0.0]*3), 'Zero drive produced active torque')
    return state


class Consumer:
    def __init__(self,io,report,config):
        self.io,self.report,self.config = io,report,config
        self.session,self.tick,self.model = uid(),0,initial_values()

    def game(self):
        return self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))

    def inspect(self):
        result = self.io.rpc('runtime.inspect',dict(session_id=self.session))
        need(type(result['tick']) is int and result['tick'] == self.tick, 'Literal committed clock')
        return result

    def physical(self):
        value = self.io.rpc('runtime.physical_rig',dict(session_id=self.session,tick=self.tick,entity=RIG,include_parts=True,include_joints=True))
        need(set(value) == {'session_id','tick','source','state'} and value['session_id'] == self.session and
             type(value['tick']) is int and value['tick'] == self.tick and value['source'] == 'runtime', 'Physical projection receipt')
        return projection(value['state'])

    def observe(self,label):
        module = self.game()['module']
        need(module['backend'] == 'coreclr' and module['assembly_sha256'] == self.report['compiled_image_sha256'], 'Actual compiled backend/image differs')
        value = validate(module['values'],module['schema'])
        need(exact(value,self.model), 'Complete independently predicted42-field callback state: '+label)
        self.report['observations'].append(dict(label=label,tick=self.tick,values=clone(value),expected=clone(self.model)))
        return module

    def edit(self,**values):
        need(set(values) <= {'Phase','Joint','Part','TargetAngle','Strength','RejectAfterWrite','FailAt'}, 'Only ordinary fixture controls may be edited')
        result = self.io.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=uid(),expected_tick=self.tick,
                             expected_revision=self.game()['revision'],values=values))
        need(result['replayed'] is False, 'Control edit must commit once')
        self.model.update(values); self.observe('typed control edit')

    def single(self,label,caller=(),request=None):
        before = self.physical(); expected = clone(self.model)
        phase,joint,part = expected['Phase'],expected['Joint'],expected['Part']
        need(phase in (0,1,2,3) and joint in (0,1) and part in (0,1,2), 'Successful callback controls must be supported')
        b,d = before['parts'][part],before['joints'][joint]['drive']
        expected.update(Ticks=expected['Ticks']+1,ObservedTick=str(self.tick),ObservedMode={'animated':0,'passive':1,'driven':2}[before['mode']],
            BodyCount=3,JointCount=2,DrivePresent=1,PartActive=int(b['active']),PartSleeping=int(b['sleeping']),
            SelectedNode=b['node_entity'],SelectedBody=str(b['body_id']),PartMass=b['mass'],ObservedStrength=d['strength'],
            Stiffness=d['stiffness'],Damping=d['damping'],MaxTorque=d['max_torque'],Phase=0,
            Writes=expected['Writes']+(1 if phase in (1,3) else 0)+(1 if phase in (1,2) else 0),
            CommittedReadChecks=expected['CommittedReadChecks']+1)
        for prefix,names,source in [('Part','XYZ',b['position']),('PartQ',('x','y','z','w'),b['rotation']),
             ('PartV',('x','y','z'),b['linear_velocity']),('PartW',('x','y','z'),b['angular_velocity']),
             ('ObservedTarget','XYZW',d['target_rotation'])]:
            for name,value in zip(names,source): expected[prefix+name] = value
        receipt = f.step(self.io,self.tick,caller,session=self.session,request=request)
        need(type(receipt['tick']) is int and receipt['tick'] == self.tick+1 and receipt['replayed'] is False, 'Single committed native tick')
        self.tick,self.model = receipt['tick'],expected; self.observe(label)
        after = self.physical()
        need([p['body_id'] for p in after['parts']] == [p['body_id'] for p in before['parts']], 'Drive operation relabeled body ownership')
        self.report['native_observations'].append(dict(label=label,before=before,after=after,receipt=clone(receipt),caller=clone(list(caller))))
        return receipt

    def snapshot(self,saves,label):
        state,game = self.inspect(),self.game(); slot = 'physical-drive-'+str(len(self.report['archives'])+1)
        receipt = self.io.rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=slot,expected_generation=0,
            session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        manifest_path = saves/('slot-'+slot)/'current.json'; need(manifest_path.stat().st_size <= MIB, 'Bounded save manifest')
        manifest = strict_json(manifest_path.read_bytes()); current = manifest['payload']['current']
        need(type(current['file']) is str and re.fullmatch(r'p-[0-9a-f]{32}\.bin',current['file']), 'Exact save archive filename')
        path = manifest_path.parent/current['file']; need(path.stat().st_size <= 16*MIB, 'Bounded complete fixture archive')
        raw = path.read_bytes(); value = strict_json(raw)
        need(type(receipt['bytes']) is int and type(current['bytes']) is int and len(raw) == receipt['bytes'] == current['bytes'] and
             hashlib.sha256(raw).hexdigest() == receipt['sha256'] == current['sha256'] and
             type(receipt['generation']) is int and receipt['generation'] == current['generation'] == 1, 'Whole save identity/receipt/manifest')
        need(set(value) == {'format','version','document','snapshot'} and value['format'] == 'poima.world-save' and value['version'] == 1, 'Complete world-save envelope')
        original_payload=original_member(original_member(raw,'snapshot'),'payload')
        need(hashlib.sha256(original_payload.encode('utf-8')).hexdigest()==value['snapshot']['sha256'],
             'Native snapshot checksum differs from original unreformatted payload bytes')
        payload = value['snapshot']['payload']; physical = payload['physical']
        need(payload['tick'] == self.tick and payload['gameplay']['backend'] == 'coreclr' and
             payload['gameplay']['assembly_sha256'] == self.report['compiled_image_sha256'] and payload['gameplay']['type'] == GAME and
             exact(validate(payload['gameplay']['values'],payload['gameplay']['schema']),self.model), 'Complete saved compiled state')
        need(physical['version'] == 5 and len(physical['rigs']) == 1 and physical['rigs'][0]['entity'] == RIG, 'Drive ownership extension5')
        membership = physical['rigs'][0]['membership']; native = self.physical()
        need(membership['mode'] == native['mode'] and exact(membership['drives'],[
            dict(joint=j['name'],target_rotation=j['drive']['target_rotation'],strength=j['drive']['strength']) for j in native['joints']]), 'Saved original requested drive population/order/values')
        row = dict(label=label,slot=slot,tick=self.tick,path=str(path),bytes=len(raw),sha256=receipt['sha256'],archive=value,
                   receipt=receipt,manifest=manifest,manifest_path=str(manifest_path),manifest_sha256=sha(manifest_path))
        self.report['archives'].append(clone(row)); return row

    def restore(self,row,existing=True):
        state,game = (self.inspect(),self.game()) if existing else (None,None)
        requested = uid()
        result = self.io.rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot=row['slot'],expected_generation=1,
            revision=2,expected_session_id=self.session if existing else None,expected_tick=self.tick if existing else None,
            expected_gameplay_revision=game['revision'] if existing else None,expected_structure_revision=state['structure_revision'] if existing else None,
            expected_material_revision=state['material_revision'] if existing else None,expected_ui_revision=state['ui_revision'] if existing else None,
            expected_control_sequence=state['control_sequence'] if existing else None,new_session_id=requested,gameplay=self.config))
        need(result.get('session_id',result.get('new_session_id')) == requested and type(result['tick']) is int and result['tick'] == row['tick'], 'Exact restored session/clock')
        self.session,self.tick,self.model = requested,result['tick'],clone(row['archive']['snapshot']['payload']['gameplay']['values'])
        self.observe('restored complete source42 fields')


def pair(report,label,left,right):
    same = Path(left['path']).read_bytes() == Path(right['path']).read_bytes() and exact(left['archive'],right['archive'])
    report['whole_pairs'].append(dict(label=label,left=left['path'],right=right['path'],complete_bytes_and_typed_equal=same,excluded_fields=[]))
    need(same,'Complete original save differs: '+label)


def reject(c,saves,label,ticks=1,caller=(),message=None):
    before = c.snapshot(saves,label+' before'); native = c.physical()
    error = f.step(c.io,c.tick,caller,ticks=ticks,session=c.session,error=-32040)
    if message: need(message in error['message'],'Unexpected compiled rejection: '+label)
    c.inspect(); c.observe(label+' unchanged42 fields')
    need(exact(native,c.physical()),'Rejected callback changed complete physical projection')
    pair(c.report,label,before,c.snapshot(saves,label+' after'))


def drive(joint,angle,strength):
    return dict(entity=RIG,drive=dict(joint=joint,target_rotation=[math.sin(angle/2),0.,0.,math.cos(angle/2)],strength=strength))


def future(c,saves,label):
    c.edit(Phase=2,Joint=1,TargetAngle=.45,Strength=.6); c.single(label+' elbow retarget')
    for n in range(3): c.single(label+' future '+str(n))
    return c.snapshot(saves,label+' future complete')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path); parser.add_argument('--hostfxr',type=Path,required=True)
    parser.add_argument('--bridge',type=Path,required=True); parser.add_argument('--assembly','--game',dest='game',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True); parser.add_argument('--windows-interop',action='store_true')
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    report = dict(format='poima.physical-drive-gameplay.v1',passed=False,backend='coreclr',calls=[],owners=[],archives=[],whole_pairs=[],
                  observations=[],native_observations=[],source_pins={},limits=dict(seconds=SECONDS,rpc=CALLS,recorded_bytes=32*MIB,evidence_bytes=64*MIB),
                  limitations=['Three tiny analytic parts, compiled CoreCLR callbacks and real native CLI; no imported-character quality, balance, GPU, audio, device input, NativeAOT or shipping claim.',
                               'Compatible same-image reload checks state retention; this is not an edit-to-verified-tick latency or changed-code benchmark.',
                               'Full save equality has no field exclusions. Exact fresh-process continuation is same-platform/build. External supervisor must establish complete process-tree ownership.'])
    f.ARGS=args; f.REPORT=report; f.DEADLINE=time.monotonic()+SECONDS; f.MAX_CALLS=CALLS; f.MAX_RECORDED=32*MIB
    io=None; pins={}
    try:
        files=[Path(__file__).resolve(),Path(f.__file__).resolve(),ROOT/'tests/gltf_fixture.py',ROOT/'tests/animation_fixture.py',
               ROOT/'tests/managed_physical_gameplay_contract.py',ROOT/'tests/managed_physical_drive_gameplay/Game.cs',
               ROOT/'tests/managed_physical_drive_gameplay/Poima.ManagedPhysicalDriveGame.csproj',ROOT/'managed/Directory.Build.props',
               ROOT/'managed/Poima.Gameplay/PhysicalRigDrive.cs',ROOT/'managed/Poima.Gameplay/PhysicalRig.cs',
               ROOT/'managed/Poima.Gameplay/GameplayRequirements.cs',ROOT/'managed/Poima.ManagedBridge/Entry.cs',
               args.binary.resolve(),args.hostfxr.resolve(),args.bridge.resolve(),args.game.resolve()]
        for assembly in (args.game.resolve(),args.bridge.resolve()):
            files.append(assembly.parent/'Poima.Gameplay.dll')
            for suffix in ('.deps.json','.runtimeconfig.json'):
                member=assembly.with_suffix(suffix)
                if member.is_file(): files.append(member)
        for path in sorted(set(files)):
            need(path.is_file(),'Required original source/artifact absent: '+str(path)); pins[str(path)]=sha(path)
        report['source_pins']=clone(pins); report['compiled_image_sha256']=sha(args.game)
        config=dict(hostfxr=f.native(args.hostfxr),bridge=f.native(args.bridge),assembly=f.native(args.game),type=GAME)
        io=f.Session(output/'original-owner'); report['authoring']=prepare(io); original=io.world.read_bytes(); world=io.world
        report['authored_world_sha256']=hashlib.sha256(original).hexdigest()
        c=Consumer(io,report,config); io.rpc('runtime.start',dict(session_id=c.session,revision=2))
        loaded=io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=uid(),expected_tick=0,expected_revision=c.game()['revision'],**config))
        report['loaded_module']=clone(loaded['module']); c.observe('initial complete42 fields'); saves=f.save_root(io)
        need([p['position'] for p in c.physical()['parts']] == [[0.,200.,0.],[0.,200.5,0.],[0.,201.,0.]],'Independent authored COM origins')
        c.single('ordinary committed animated reads'); animated=c.snapshot(saves,'animated checkpoint')
        c.edit(Phase=1); c.single('compiled activate then drive')
        need(c.physical()['mode']=='driven' and c.model['Writes']==2,'Compiled activation and active torque permission')
        c.single('next callback sees committed Driven mode'); need(c.model['ObservedMode']==2,'Managed summaryv2 failed to report Driven')
        response=c.physical()['joints'][0]
        need(abs(response['relative_rotation'][0])>1e-6,'Compiled nonzero target failed to produce actual native relative joint motion')
        report['active_response']=dict(tick=c.tick,joint=clone(response),minimum_actual_x_quaternion_component=1e-6)
        c.restore(animated); pair(report,'warm animated whole restore',animated,c.snapshot(saves,'warm animated restored'))
        c.edit(Phase=2); before_tick=c.tick; request=uid()
        accepted=c.single('caller activation precedes compiled drive',[dict(entity=RIG,activate=True)],request)
        before_retry=c.snapshot(saves,'before exact retry')
        retry=f.step(io,before_tick,[dict(entity=RIG,activate=True)],session=c.session,request=request)
        need(exact(retry,dict(accepted,replayed=True)),'Exact active-drive retry receipt')
        f.step(io,before_tick,[dict(entity=RIG,activate=True),drive('elbow',.1,.5)],session=c.session,request=request,error=-32010)
        c.observe('retry did not invoke gameplay again')
        pair(report,'successful retry and changed-request refusal preserve whole state',before_retry,c.snapshot(saves,'after exact retry and refusal'))
        c.edit(Phase=2,Joint=1,TargetAngle=.4,Strength=.75); c.single('second compiled joint enabled')
        c.edit(Phase=2,Joint=0,TargetAngle=-.2,Strength=0.0); c.single('one disabled joint retains Driven')
        need(c.physical()['mode']=='driven','Disabling one joint discarded another active drive')
        c.edit(Phase=2,Joint=1,Strength=0.0); c.single('all zero returns passive')
        need(c.physical()['mode']=='passive','Zeroing all drives did not retain passive physical ownership')
        c.edit(Phase=2,Joint=0,TargetAngle=.3,Strength=.5); c.single('ordinary drive re-enable')
        for label,changes,caller,message in [
            ('staged callback exception',dict(Phase=2,RejectAfterWrite=1),(),'Physical drive fixture rejected after callback staging'),
            ('native hard-limit rejection',dict(Phase=2,RejectAfterWrite=0,TargetAngle=1.5),(),None),
            ('native strength rejection',dict(Phase=2,TargetAngle=.3,Strength=-.1),(),None),
            ('caller and managed duplicate',dict(Phase=2,Strength=.5),(drive('shoulder',.2,.5),),None)]:
            c.edit(**changes); reject(c,saves,label,caller=caller,message=message)
        c.edit(Phase=2,FailAt=str(c.tick+1)); reject(c,saves,'later-Tick rollback',ticks=2,message='Physical drive fixture later-Tick failure')
        c.edit(Phase=0,FailAt='-1')
        before_reload=c.snapshot(saves,'before compatible reload'); revision=c.game()['revision']
        io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=uid(),expected_tick=c.tick,expected_revision=revision,**config))
        c.observe('compatible same-image reload keeps complete state'); after_reload=c.snapshot(saves,'after compatible reload')
        a,b=before_reload['archive'],after_reload['archive']; expected=clone(a)
        need(b['snapshot']['payload']['gameplay_revision']==a['snapshot']['payload']['gameplay_revision']+1,'Reload advances gameplay revision exactly once')
        # The reload contract has exactly two changed leaves; keep both complete
        # original archives and record the explicit delta rather than exclude it.
        expected['snapshot']['payload']['gameplay_revision']=b['snapshot']['payload']['gameplay_revision']
        expected['snapshot']['sha256']=b['snapshot']['sha256']
        need(exact(expected,b) and a['snapshot']['sha256'] != b['snapshot']['sha256'],'Reload changed a field outside revision/checksum')
        report['reload']=dict(before=before_reload['path'],after=after_reload['path'],changed_paths=['/snapshot/payload/gameplay_revision','/snapshot/sha256'],
                              values_preserved=True,whole_archive_byte_equality_claimed=False)
        c.single('post-reload compiled callback'); checkpoint=c.snapshot(saves,'driven continuation checkpoint')
        reference=future(c,saves,'original'); c.restore(checkpoint)
        pair(report,'warm driven whole restore',checkpoint,c.snapshot(saves,'warm driven restored'))
        pair(report,'warm driven whole future',reference,future(c,saves,'warm'))
        need(world.read_bytes()==original,'Runtime mutated original authored world')
        io.close(); io=None
        io=f.Session(output/'cold-driven-owner',world); io.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=f.native(saves)))
        c=Consumer(io,report,config); c.restore(checkpoint,False)
        pair(report,'fresh driven whole restore',checkpoint,c.snapshot(saves,'fresh driven restored'))
        pair(report,'fresh driven whole future',reference,future(c,saves,'cold'))
        for joint_index in (0,1): c.edit(Phase=2,Joint=joint_index,Strength=0.0); c.single('disable joint '+str(joint_index))
        need(c.physical()['mode']=='passive','Final disabled phase is not passive')
        disabled=c.snapshot(saves,'disabled checkpoint')
        for n in range(4): c.single('disabled original future '+str(n))
        disabled_future=c.snapshot(saves,'disabled future reference'); io.close(); io=None
        io=f.Session(output/'cold-disabled-owner',world); io.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=f.native(saves)))
        c=Consumer(io,report,config); c.restore(disabled,False)
        pair(report,'fresh disabled whole restore',disabled,c.snapshot(saves,'fresh disabled restored'))
        for n in range(4): c.single('disabled cold future '+str(n))
        pair(report,'fresh disabled whole future',disabled_future,c.snapshot(saves,'fresh disabled future'))
        need(world.read_bytes()==original,'Fresh continuation mutated authoring'); io.close(); io=None
        need(len(report['owners'])==3 and all(o['natural_exit'] and not o['forced_cleanup'] and o['exit_code']==0 for o in report['owners']),'All three native CLI owners must close naturally')
        need(all(sha(path)==digest for path,digest in pins.items()),'Original pinned source/artifact changed during qualification')
        report['passed']=True
    except Exception as error:
        report['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
    finally:
        if io is not None:
            try: io.close()
            except Exception as error: report['closure_failure']=dict(type=type(error).__name__,message=str(error)); report['passed']=False
        report['elapsed_seconds']=time.monotonic()-(f.DEADLINE-SECONDS); report['recordings']=[]
        for owner in report['owners']:
            for name in ('requests.jsonl','responses.jsonl','stderr.log'):
                path=Path(owner['directory'])/name
                if path.is_file(): report['recordings'].append(dict(path=str(path),bytes=path.stat().st_size,sha256=sha(path)))
        encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        if len(encoded.encode())>64*MIB:
            report['passed']=False; report['failure']=dict(type='EvidenceBound',message='Complete evidence exceeds64MiB; no fields dropped')
            encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        (output/'evidence.json').write_text(encoded,encoding='utf-8')
        print(json.dumps({key:len(report[key]) for key in ('calls','owners','archives','whole_pairs','observations','native_observations')}|dict(passed=report['passed'],output=str(output))))
    return 0 if report['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
