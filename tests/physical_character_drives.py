#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded original-character compiled pose-drive workflow; no build or renderer.

An external process-tree supervisor must own this test. Uses original licensed
content, a separately prepared world and actual compiled gameplay. Saves and
literal requests/replies remain complete, including failed attempts. This is
not active balance, recovery, rendered quality or a performance benchmark.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time
import traceback

sys.dont_write_bytecode = True
import physical_character_gameplay_contract as base
import physical_drive_gameplay as drive_contract

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
DATA = ROOT/'examples/physical-character-yard/drive-poses.json'
f, need, exact = base.f, base.need, base.exact
RIG, TARGET, PLAYER = base.RIG, base.TARGET, base.PLAYER
JOINTS = 'spine neck shoulder_l elbow_l wrist_l shoulder_r elbow_r wrist_r hip_l knee_l ankle_l hip_r knee_r ankle_r'.split()
STRENGTHS = ('StrengthSpine StrengthNeck StrengthShoulderL StrengthElbowL StrengthWristL '
             'StrengthShoulderR StrengthElbowR StrengthWristR StrengthHipL StrengthKneeL '
             'StrengthAnkleL StrengthHipR StrengthKneeR StrengthAnkleR').split()
ADDED = {
    **dict.fromkeys('HoldRequested AppliedHold PosePreset AppliedPreset SelectedJoint DrivePending DriveWrites DriveChanges ObservedDriveCount ObservedJoint'.split(), 'int32'),
    'LastDriveTick':'int64', 'ObservedSelectedStrength':'float64',
    **dict.fromkeys(STRENGTHS, 'float64'),
}
FIELDS = dict(base.FIELDS, **ADDED)
ACTIONS = dict(base.ACTIONS, **{'pose-hold':514,'pose-release':515,'pose-preset':516,'pose-joint':517,'pose-strength':518})
SECONDS, CALLS, MIB = 420, 1800, 1024**2
clone, read, identity = copy.deepcopy, base.read, base.identity


def typed(values, schema):
    need(len(base.FIELDS)==61 and len(ADDED)==26 and len(FIELDS)==87 and
         type(values) is dict and set(values)==set(FIELDS) and
         schema['identity']==base.IDENTITY and len(schema['fields'])==87,
         'Complete original61 plus appended26 typed state fields')
    offset, layout = 0, {}
    for name,kind in FIELDS.items():
        size=16 if kind=='entity' else 4 if kind=='int32' else 8
        alignment=min(size,8); offset=(offset+alignment-1)//alignment*alignment
        layout[name]=dict(name=name,kind=kind,offset=offset,bytes=size); offset+=size
        value=values[name]
        valid=(type(value) is str and re.fullmatch('[0-9a-f]{32}',value) is not None) if kind=='entity' else (
            type(value) is int and -(2**31)<=value<2**31) if kind=='int32' else (
            type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value) is not None and
            -(2**63)<=int(value)<2**63) if kind=='int64' else type(value) is float and math.isfinite(value)
        need(valid,'Uncoerced complete scalar value: '+name)
    need((offset+7)//8*8==560 and type(schema['bytes']) is int and schema['bytes']==560 and
         [x['name'] for x in schema['fields']]==sorted(FIELDS) and
         all(exact(row,layout[row['name']]) for row in schema['fields']), 'Independent complete87-field560-byte layout')
    return values


def product(a,b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]


def target_limits(target, limits):
    base.vector(target,4); norm=math.hypot(*target)
    need(abs(norm-1)<1e-12 and target[3]>0,'Authored finite canonical unit target, without clamping')
    twist_norm=math.hypot(target[0],target[3]); need(twist_norm>1e-6,'Nonsingular authored twist')
    twist=[target[0]/twist_norm,0.,0.,target[3]/twist_norm]
    swing=product(target,[-twist[0],0.,0.,twist[3]])
    angle=2*math.atan2(twist[0],twist[3])
    if limits['kind']=='hinge':
        need(abs(target[1])<1e-12 and abs(target[2])<1e-12 and limits['minimum']<=angle<=limits['maximum'],
             'Authored hinge target fits original X limits')
    else:
        need(limits['kind']=='swing_twist' and
             (swing[1]/math.sin(limits['swing_y']/2))**2+(swing[2]/math.sin(limits['swing_z']/2))**2<=1+1e-12 and
             limits['twist_min']<=angle<=limits['twist_max'],'Authored target fits original swing ellipse and twist interval')


def source_contract(args, report):
    need(identity(args.source)['sha256']==base.SOURCE_SHA and identity(args.license)['sha256']==base.LICENSE_SHA,
         'Unchanged licensed original Knight inputs')
    oracle=base.LibraryOracle(args.source.read_bytes()); document=read(args.world); data=read(DATA)
    need(document['format']=='poima.authored-world' and type(document['revision']) is int,'Actual authored source world')
    need(data['format']=='poima.physical-character-drive-poses' and type(data['version']) is int and data['version']==1 and
         len(data['joints'])==14 and [x['name'] for x in data['joints']]==JOINTS and
         len(data['presets'])==2 and [(x['id'],x['name']) for x in data['presets']]==[(0,'Relaxed'),(1,'Guard')],
         'Complete ordered authored drive data and two named presets')
    original=read(ROOT/'examples/physical-character-yard/profiles/knight.json')
    entities=document['entities']; profile=entities[RIG]['components']['PhysicalRig']
    need(exact(profile['parts'],original['parts']) and profile['root']==original['root'] and
         profile['initial_mode']=='animated' and profile['pose_frame']=='skeletal_bounded_scale' and
         profile['controller_handoff']==dict(actor=TARGET,follow='translate_keep_heading') and
         profile['procedural_pose']=='grounded_animated_only','Original bodies, source frames and explicit physical owner')
    need(len(profile['joints'])==len(original['joints'])==14,'Original full joint graph')
    for actual,old,setting in zip(profile['joints'],original['joints'],data['joints']):
        expected=clone(old); expected['drive']=setting['drive']
        need(exact(actual,expected) and actual['name']==setting['name'],'Only exact authored drive permission extends original joint')
        gains=setting['drive']; need(set(gains)=={'stiffness','damping','max_torque'} and
            all(type(x) in (int,float) and math.isfinite(x) for x in gains.values()) and
            0<gains['stiffness']<=1e6 and 0<=gains['damping']<=1e4 and 0<gains['max_torque']<=1e5 and
            type(setting['default_strength']) is float and setting['default_strength']==.65,'Bounded explicit gains and default strength')
    for preset in data['presets']:
        need([x['joint'] for x in preset['targets']]==JOINTS,'Every preset covers original joints exactly once in order')
        for item,joint in zip(preset['targets'],profile['joints']):target_limits(item['target_rotation'],joint['limits'])
    rig=entities[RIG]['components']['AnimationRig']
    need(len(oracle.nodes)==57 and len(oracle.clips)==76 and oracle.clips[36]['name']=='Idle' and
         oracle.clips[48]['name']=='Running_A' and rig['clip']==36,'Original complete source library')
    mapping={e['components']['RigNode']['node']:key for key,e in entities.items() if e['components'].get('RigNode',{}).get('rig')==RIG}
    need(mapping=={i:base.node_id(i) for i in range(57)},'All original source-node identities')
    for index,key in mapping.items():
        parent=base.node_id(oracle.parents[index]) if index in oracle.parents else RIG
        need(entities[key]['parent']==parent,'Original source hierarchy unchanged')
    regions={}
    for index,part in enumerate(profile['parts']):
        key=f'{2000+index:032x}'; entity=entities[key]; region=entity['components']['HitRegion']
        need(entity['parent']==mapping[part['node']] and region['owner']==TARGET and region['name']==part['name'],
             'Original source bone to named physical hit-part')
        regions[part['name']]=dict(index=index,entity=key,node=part['node'],node_entity=mapping[part['node']])
    need(len(regions)==15 and {key for key,e in entities.items() if e['components'].get('HitRegion',{}).get('owner')==TARGET}==
         {x['entity'] for x in regions.values()},'Complete fifteen-part hit-region population')
    need(entities[PLAYER]['components']['CharacterController']['camera']==base.CAMERA and
         entities[TARGET]['components']['CharacterController']['camera']==base.TARGET_CAMERA and
         entities[base.CAMERA]['parent']==PLAYER and entities[RIG]['parent']==TARGET,'Independent player camera and target ownership')
    asset=rig['asset']; need(type(asset) is str and re.fullmatch('[0-9a-f]{64}',asset),'Original cooked content address')
    package=Path(str(args.world)+'.assets')/(asset+'.pmodel'); need(identity(package)['sha256']==asset,'Whole cooked image identity')
    base.decode_cooked(package.read_bytes(),oracle)
    report['source_contract']=dict(world=identity(args.world),source=identity(args.source),license=identity(args.license),
        package=dict(path=str(package),identity=identity(package)),drive_data=identity(DATA),profile=profile,regions=regions,
        original_nodes=57,original_clips=76,authored_data=data)
    return document,profile,regions,data


class Driver(base.Driver):
    def __init__(self,*args,data,backend='coreclr',**kwargs):
        super().__init__(*args,**kwargs); self.data,self.backend=data,backend

    def game(self):
        value=self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
        module=value['module']; need(value['session_id']==self.session and value['tick']==self.tick and
            module['backend']==self.backend and module['assembly_sha256']==self.report['game_image']['sha256'],
            'Actual supplied compiled game/backend and committed clock')
        typed(module['values'],module['schema'])
        if self.backend=='native_aot':
            need(exact(module['schema'],self.report['native_artifact']['descriptor']['schema']),
                 'Native runtime schema equals complete supplied artifact schema')
        return value

    def start(self):
        value=self.io.rpc('runtime.start',dict(session_id=self.session,revision=self.revision))
        need(value['session_id']==self.session and value['tick']==0 and value['started'] is True,'Actual fresh runtime start')
        method='runtime.gameplay.load' if self.backend=='coreclr' else 'runtime.gameplay.load_native'
        self.io.rpc(method,dict(session_id=self.session,request_id=base.uid(),expected_tick=0,expected_revision=0,**self.config))
        return self.game()

    def physical(self):
        state=super().physical(); need(state['mode'] in ('animated','passive','driven'),'Explicit native physical mode')
        strengths=[]
        for joint,authored in zip(state['joints'],self.profile['joints']):
            need(set(joint)==f.JOINT_FIELDS|{'drive'} and joint['parent']==authored['parent'] and joint['child']==authored['child'] and
                 type(joint['hinge']) is bool and joint['hinge']==(authored['limits']['kind']=='hinge'),'Whole mapped native joint and opted-in drive')
            drive=joint['drive']; need(set(drive)=={'target_rotation','strength','stiffness','damping','max_torque','evaluated_child_torque','angular_error','saturation_scale'},'Complete native drive observation')
            for name,n in [('target_rotation',4),('evaluated_child_torque',3)]:base.vector(drive[name],n)
            for name in ('strength','stiffness','damping','max_torque','angular_error','saturation_scale'):
                need(type(drive[name]) is float and math.isfinite(drive[name]),'Uncoerced native drive scalar: '+name)
            need(all(drive[k]==authored['drive'][k] for k in ('stiffness','damping','max_torque')) and
                 0<=drive['strength']<=1 and 0<=drive['saturation_scale']<=1 and
                 math.hypot(*drive['evaluated_child_torque'])<=drive['strength']*drive['max_torque']+1e-8,
                 'Immutable authored gains and bounded actual drive torque')
            if drive['strength']==0:need(exact(drive['evaluated_child_torque'],[0.,0.,0.]),'Disabled joint drive contributes zero active torque')
            strengths.append(drive['strength'])
        if state['mode']!='animated':need((state['mode']=='driven')==any(x>0 for x in strengths),'Driven iff a physical drive strength is positive')
        return state

    def control(self,action,error=None):
        if action in base.ACTIONS and error is None:return super().control(action)
        state,game=self.state(),self.game(); key=f'{ACTIONS[action]:032x}'
        ui=self.io.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256))
        selected=[x for x in ui['elements'] if x['id']==key]
        need(len(selected)==1 and selected[0]['action']==action and selected[0]['eligible'] is True,'Actual authored eligible pose control')
        request=dict(session_id=self.session,request_id=base.uid(),expected_tick=self.tick,expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'],id=key)
        result=self.io.rpc('runtime.ui.activate',request,error)
        if error is None:
            need(result['replayed'] is False,'New ordinary pose control')
            self.session,self.tick=result['current_session_id'],result['current_tick']
        self.report['controls'].append(dict(action=action,request=request,result=result,expected_error=error)); return request,result

    def edit_strengths(self,value):
        request=dict(session_id=self.session,request_id=base.uid(),expected_tick=self.tick,expected_revision=self.game()['revision'],
                     values={name:value for name in STRENGTHS})
        result=self.io.rpc('runtime.gameplay.edit',request); need(result['replayed'] is False,'Ordinary typed strength configuration edit')
        self.report['edits'].append(dict(request=request,result=result))

    def archive(self,slot,label,receipt=None,current_game=None):
        manifest_path=self.saves/('slot-'+slot)/'current.json'; manifest=read(manifest_path,MIB); member=manifest['payload']['current']
        need(type(member['file']) is str and re.fullmatch('p-[0-9a-f]{32}[.]bin',member['file']),'Safe original complete save member')
        path=manifest_path.parent/member['file']; pin=identity(path,16*MIB)
        need(pin=={key:member[key] for key in ('bytes','sha256')},'Whole save/manifest identity')
        if receipt is not None:need(pin=={key:receipt[key] for key in ('bytes','sha256')},'Whole save/write receipt identity')
        raw=path.read_bytes(); value=read(path,16*MIB); payload=value['snapshot']['payload']; game=payload['gameplay']
        need(value['format']=='poima.world-save' and type(value['version']) is int and value['version']==1 and
             game['backend']==self.backend and game['type']==base.GAME and game['assembly_sha256']==self.report['game_image']['sha256'],
             'Whole saved selected compiled backend/image')
        if current_game is not None:need(payload['tick']==self.tick and exact(game['values'],current_game['module']['values']),
                                        'All saved gameplay fields match committed values')
        saved=dict(label=label,slot=slot,generation=member['generation'],tick=payload['tick'],path=str(path),identity=pin,
                   manifest_path=str(manifest_path),manifest_identity=identity(manifest_path),receipt=receipt)
        self.report['archives'].append(saved); base.census(self.report['output'])
        typed(payload['gameplay']['values'],payload['gameplay']['schema'])
        original=drive_contract.original_member(drive_contract.original_member(raw,'snapshot'),'payload')
        need(hashlib.sha256(original.encode()).hexdigest()==value['snapshot']['sha256'],'Original unreformatted whole snapshot payload checksum')
        native=self.physical() if payload['tick']==self.tick else None
        physical=payload['physical']; need(physical['version']==5 and len(physical['rigs'])==1 and physical['rigs'][0]['entity']==RIG,
                                          'Complete saved original-character drive ownership extension5')
        controls=physical['rigs'][0]['membership']['drives']; need([x['joint'] for x in controls]==JOINTS,'All fourteen saved controls in source order')
        if native is not None:
            need(exact(controls,[dict(joint=j['name'],target_rotation=j['drive']['target_rotation'],strength=j['drive']['strength']) for j in native['joints']]),
                 'Whole saved committed target/strength population')
        return saved


def applied(driver,preset,strengths,label):
    native=driver.physical(); values=driver.values(); targets=driver.data['presets'][preset]['targets']
    need(values['HoldRequested']==1 and values['AppliedHold']==1 and values['DrivePending']==0 and
         values['AppliedPreset']==preset,'Actual compiled hold request settled through native writes')
    for joint,target,strength in zip(native['joints'],targets,strengths):
        need(joint['drive']['strength']==strength and all(abs(a-b)<2e-14 for a,b in zip(joint['drive']['target_rotation'],target['target_rotation'])),
             'Committed authored target/strength, allowing only native unit canonicalization')
    need(native['mode']==('driven' if any(x>0 for x in strengths) else 'passive'),'Requested hold and actual physical mode stay distinct')
    driver.report['drive_observations'].append(dict(label=label,tick=driver.tick,values=values,native=native,preset=preset,requested_strengths=strengths))
    return native


def advance(driver,count):
    for _ in range(count):driver.step()


def fired_driven(driver):
    driver.aim('chest'); before=driver.observe('driven-before-real-shot'); ray=driver.camera_hit()
    old=before['game']['module']['values']; need(old['Cooldown']<=1,'Actual firing cooldown has elapsed')
    request,result=driver.step(fire=True); after=driver.observe('driven-after-real-shot')
    values=after['game']['module']['values']; hit=ray['result']['hit']; part=ray['expected_part']
    need(values['Shots']==old['Shots']+1 and values['Hits']==old['Hits']+1 and values['Misses']==old['Misses'] and
         values['Takeovers']==old['Takeovers'] and values['LastShotKind']==4 and values['LastPhysicalCommand']==2 and
         values['LastHitPart']==part['index'] and values['LastHitRegion']==part['entity'] and values['LastHitEntity']==TARGET and
         values['LastShotTick']==str(before['tick']) and values['Health']==max(0.,old['Health']-old['Damage']),
         'Actual camera hit on Driven character applies localized impulse without a second activation')
    for prefix,vector in (('LastOrigin',ray['origin']),('LastAim',ray['direction']),('LastHit',hit['position']),
                          ('LastImpulse',[x*old['ShotImpulse'] for x in ray['direction']])):
        need(all(abs(values[prefix+axis]-expected)<1e-8 for axis,expected in zip('XYZ',vector)),
             'Compiled Driven-hit camera/part/point/impulse join: '+prefix)
    need(after['physical']['mode']=='driven' and after['handoff']['phase']=='physical' and
         after['handoff']['capsule_enrolled'] is False and
         after['handoff']['takeover_tick']==before['handoff']['takeover_tick'] and
         values['DriveWrites']==old['DriveWrites'] and values['DriveChanges']==old['DriveChanges'] and
         exact([(j['name'],j['drive']['target_rotation'],j['drive']['strength']) for j in before['physical']['joints']],
               [(j['name'],j['drive']['target_rotation'],j['drive']['strength']) for j in after['physical']['joints']]),
         'Driven hit preserves original physical owner and every committed joint target/strength')
    saved=driver.snapshot('driven-after-real-shot')
    driver.report['shots'].append(dict(label='driven-native-camera-hit',request=request,result=result,ray=ray,before=before,after=after,archive=saved))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','source','license','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--backend',choices=('coreclr','native_aot'),default='coreclr')
    for name in ('hostfxr','bridge','assembly','descriptor'):parser.add_argument('--'+name,type=Path)
    args=parser.parse_args(); need(not sys.flags.optimize,'Nonoptimized verifier required')
    for name,value in vars(args).items():
        if isinstance(value,Path):setattr(args,name,value.resolve())
    need((args.backend=='coreclr' and all((args.hostfxr,args.bridge,args.assembly)) and args.descriptor is None) or
         (args.backend=='native_aot' and args.descriptor is not None and all(x is None for x in (args.hostfxr,args.bridge,args.assembly))),
         'Select CoreCLR hostfxr/bridge/assembly OR native_aot descriptor, never mixed selectors')
    need(not args.output.exists(),'Fresh output; preserve every prior attempt'); args.output.mkdir(parents=True); args.windows_interop=False
    f.ARGS,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED=args,time.monotonic()+SECONDS,CALLS,48*MIB
    report=dict(format='poima.physical-character-drives.v1',passed=False,errors=[],calls=[],owners=[],archives=[],whole_pairs=[],
        observations=[],controls=[],steps=[],shots=[],camera_hits=[],inputs={},copies={},checks=[],edits=[],drive_observations=[],
        output=str(args.output),backend=args.backend,limits=dict(seconds=SECONDS,rpc=CALLS,output_bytes=base.MAX_OUTPUT,owners=3),
        scope='Original imported character, supplied compiled87-field gameplay and ordinary native controls. Same-host complete save continuation and bounded drive response; no active balance, recovery, visual quality, GPU, artifact publication, export or performance claim.')
    f.REPORT=report; io=None
    try:
        document,profile,regions,data=source_contract(args,report)
        files={SELF,DATA,args.binary,args.source,args.license,
            ROOT/'examples/physical-character-yard/PhysicalCharacterGame.cs',ROOT/'examples/physical-character-yard/Poima.PhysicalCharacterGame.csproj',
            ROOT/'examples/physical-character-yard/profiles/knight.json',ROOT/'managed/Directory.Build.props'}
        for module in tuple(sys.modules.values()):
            path=getattr(module,'__file__',None)
            if path and Path(path).suffix=='.py' and Path(path).resolve().is_relative_to(ROOT):files.add(Path(path).resolve())
        if args.backend=='coreclr':
            files.update((args.hostfxr,args.bridge,args.assembly))
            for image in (args.bridge,args.assembly):
                files.add(image.parent/'Poima.Gameplay.dll')
                for suffix in ('.deps.json','.runtimeconfig.json'):
                    if image.with_suffix(suffix).is_file():files.add(image.with_suffix(suffix))
            need(identity(args.bridge.parent/'Poima.Gameplay.dll')==identity(args.assembly.parent/'Poima.Gameplay.dll'),'Same actual compiled SDK')
            report['game_image']=identity(args.assembly)
            config=dict(hostfxr=str(args.hostfxr),bridge=str(args.bridge),assembly=str(args.assembly),type=base.GAME)
        else:
            import game_stdio_contract as bundle
            descriptor=read(args.descriptor); library=bundle.relative(args.descriptor.parent,descriptor['library'])
            expected_features={'baseline_v7','animation_rate_v1','animation_grounding_control_v1','animation_ik_end_orientation_v1',
                'animation_ik_v1','animation_inertial_v1','character_input_v1','hit_regions_v1','input_actions_v1','physical_rig_v1','physical_rig_drive_v1'}
            need(descriptor['format']=='poima.native-gameplay' and descriptor['version']==2 and descriptor['identity']==base.IDENTITY and
                 descriptor['type']==base.GAME and descriptor['minimum_services_bytes']==448 and descriptor['services_version']==7 and
                 len(descriptor['required_features'])==len(expected_features) and set(descriptor['required_features'])==expected_features,
                 'Explicit actual448-drive NativeAOT artifact requirement, without rebuilding')
            files.update((args.descriptor,library))
            need(1<=len(descriptor['files'])<=64,'Bounded complete published payload population')
            for item in descriptor['files']:
                path=bundle.relative(args.descriptor.parent,item['path'])
                need(identity(path)==dict(bytes=item['size'],sha256=item['sha256']),'Complete supplied native payload identity')
                files.add(path)
            report['game_image']=identity(library); report['native_artifact']=dict(path=str(args.descriptor),identity=identity(args.descriptor),descriptor=descriptor)
            config=dict(descriptor=str(args.descriptor),expected_descriptor_sha256=identity(args.descriptor)['sha256'])
        for module in tuple(sys.modules.values()):
            path=getattr(module,'__file__',None)
            if path and Path(path).suffix=='.py' and Path(path).resolve().is_relative_to(ROOT):files.add(Path(path).resolve())
        report['inputs'].update({str(path):identity(path) for path in sorted(files)}); base.copied_inputs(args,report)
        saves=args.output/'saves'; saves.mkdir()
        def open_owner(name):
            nonlocal io
            need(io is None and len(report['owners'])<3,'One bounded actual owner at a time')
            io=f.Session(args.output/name,args.output/'world.json')
            need(io.rpc('save.configure',dict(request_id=base.uid(),expected_generation=0,root=str(saves)))['generation']==1,'Actual save routing')
            return Driver(io,report,config,document['revision'],saves,profile,regions,data=data,backend=args.backend)
        def close_owner():
            nonlocal io
            io.close(); need(io.process.poll() is not None,'Actual native owner closed before replacement'); io=None

        d=open_owner('primary'); d.start(); values=d.values()
        need(values['Active']==0 and values['HoldRequested']==values['AppliedHold']==0 and values['ObservedJoint']==-1 and
             all(values[name]==.65 for name in STRENGTHS),'Unmodified initial game and disabled drive defaults')
        d.control('begin'); base.settled(d); advance(d,12); base.grounded(d.observe('begun-grounded'))
        animated=d.snapshot('animated-origin'); before=d.observe('animated-before-hold',all_nodes=True)
        old=before['game']['module']['values']; d.control('pose-hold'); d.step()
        held=applied(d,0,[.65]*14,'hold-directly-from-animated'); values=d.values()
        need(values['Takeovers']==old['Takeovers']+1 and values['DriveWrites']==old['DriveWrites']+14 and
             values['Shots']==old['Shots'] and values['Hits']==old['Hits'] and values['Health']==old['Health'],
             'Hold itself stages exactly one genuine activation plus fourteen drive writes, without firing')
        direct=d.snapshot('animated-hold-takeover'); terminal=read(direct['path'])['snapshot']['payload']['physical']['rigs'][0]['procedural_terminal']
        need(terminal['captured_tick']==before['tick'] and exact(terminal['poses'],[dict(entity=row['id'],**row['local_transform']) for row in before['all_nodes']]),
             'Hold takeover preserves every original committed source-node local')
        d.restore(animated); base.whole(report,'complete animated origin after direct Hold branch',animated,d.snapshot('animated-restored-after-hold'))
        menu_request,menu_reply=d.control('menu'); menu=d.snapshot('menu-before-retry')
        need(d.values()['Active']==2 and exact(io.rpc('runtime.ui.activate',menu_request),dict(menu_reply,replayed=True)),'Ordinary menu and retry receipt')
        base.whole(report,'menu retry preserves full state',menu,d.snapshot('menu-after-retry')); d.control('resume')
        d.control('target-motion'); start=d.entity(TARGET)['world_matrix'][12:15]; d.step(40)
        moving=d.observe('patrolling'); need(moving['rig']['animation']['clip']==48 and math.dist(start,moving['target']['world_matrix'][12:15])>.2,'Original Running_A and native patrol remain functional')
        d.control('target-motion'); d.step(20); base.grounded(d.observe('stopped-grounded'))
        d.aim('chest'); before=d.observe('aimed-before-real-shot',all_nodes=True)
        _,passive=base.fired(d,before,d.camera_hit(),d.values()['ShotImpulse'],'ordinary-native-camera-hit')

        d.edit_strengths(-.1); invalid=d.snapshot('invalid-authored-strength-before-control')
        _,rejected=d.control('pose-hold',error=-32040)
        need('Every requested joint strength' in rejected['message'],'Actual compiled finite-strength admission rejected UI control')
        base.whole(report,'rejected compiled control preserves complete state',invalid,d.snapshot('invalid-authored-strength-after-control'))

        # Both variants begin from this identical original physical checkpoint.
        d.restore(passive); base.whole(report,'exact passive counterfactual origin',passive,d.snapshot('counterfactual-origin'))
        d.edit_strengths(0.); zero_input=d.snapshot('zero-strength-explicit-input')
        a,b=read(passive['path']),read(zero_input['path']); expected=clone(a)
        for name in STRENGTHS:expected['snapshot']['payload']['gameplay']['values'][name]=0.
        need(b['snapshot']['payload']['gameplay_revision']==a['snapshot']['payload']['gameplay_revision']+1,'Exactly one explicit typed configuration revision')
        expected['snapshot']['payload']['gameplay_revision']=b['snapshot']['payload']['gameplay_revision']; expected['snapshot']['sha256']=b['snapshot']['sha256']
        need(exact(expected,b),'Zero variant changes only14 authored strengths, gameplay revision and snapshot checksum')
        d.control('pose-preset'); d.control('pose-hold'); advance(d,5); zero=applied(d,1,[0.]*14,'zero-strength-hold')
        zero_future=d.snapshot('zero-strength-response')

        d.restore(passive); base.whole(report,'positive variant original full input',passive,d.snapshot('positive-origin'))
        d.control('pose-preset'); native_before=d.physical(); writes=d.values()['DriveWrites']; d.control('pose-hold')
        need(exact(native_before,d.physical()) and d.values()['DrivePending']==1,'Logical Hold only queues native work')
        queued=d.snapshot('queued-hold-before-conflict')
        target=data['presets'][1]['targets'][0]['target_rotation']
        _,conflict=d.step(error=-32040,physical_rigs=[dict(entity=RIG,drive=dict(joint='spine',target_rotation=target,strength=.65))])
        need('Caller and gameplay requested duplicate joint drives' in conflict['message'],'Actual duplicate caller/compiled joint-drive conflict')
        base.whole(report,'caller/compiled duplicate drive rolls back complete state',queued,d.snapshot('queued-hold-after-conflict'))
        d.step(); need(d.values()['DriveWrites']==writes+14,'Exactly fourteen actual staged joint calls')
        advance(d,4); positive=applied(d,1,[.65]*14,'positive-strength-hold'); driven=d.snapshot('driven-checkpoint')
        need(d.values()['DriveWrites']==writes+14,'Steady holding does not repeat unchanged native writes')
        response=[dict(part=p['name'],angular_velocity_difference=math.dist(p['angular_velocity'],z['angular_velocity']),
                       position_difference=math.dist(p['position'],z['position'])) for p,z in zip(positive['parts'],zero['parts'])]
        need(any(x['angular_velocity_difference']>1e-7 or x['position_difference']>1e-7 for x in response) and
             any(math.hypot(*j['drive']['evaluated_child_torque'])>1e-7 for j in positive['joints']), 'Positive native joint drives change real solver response')
        report['counterfactual']=dict(original=passive,zero_input=zero_input,zero_response=zero_future,positive_response=driven,
            changed_paths=['/snapshot/payload/gameplay/values/'+name for name in STRENGTHS]+['/snapshot/payload/gameplay_revision','/snapshot/sha256'],
            native_response=response,scope='Exact shared physical origin and explicit strength tuning; no exact force/momentum or convergence claim.')
        fired_driven(d); d.restore(driven)
        base.whole(report,'complete driven origin after actual impulse branch',driven,d.snapshot('driven-restored-after-shot'))
        advance(d,4); driven_future=d.snapshot('driven-future-reference')
        previous=d.values(); native_before=d.physical(); d.control('pose-joint'); selected=d.values()
        need(selected['SelectedJoint']==(previous['SelectedJoint']+1)%14 and selected['ObservedJoint']==previous['ObservedJoint'],
             'Changing selection does not relabel an earlier native observation')
        d.control('pose-strength'); desired=d.values(); chosen=desired['SelectedJoint']; strengths=[desired[name] for name in STRENGTHS]
        need(strengths[chosen]==.75 and all(x==.65 for i,x in enumerate(strengths) if i!=chosen) and exact(native_before,d.physical()),
             'Ordinary strength UI queues only selected-joint change')
        d.step(); need(d.values()['DriveWrites']==previous['DriveWrites']+1,'Exactly one changed-strength joint write')
        d.step(); applied(d,1,strengths,'selected-joint-strength')
        selected=d.values(); need(selected['ObservedJoint']==chosen and selected['ObservedSelectedStrength']==.75 and
             selected['ObservedDriveCount']==14,'Actual subsequent native read is tied to selected joint')
        d.control('pose-preset'); d.step(); applied(d,0,strengths,'ordinary-relaxed-preset')
        d.snapshot('selected-strength-and-relaxed-preset')
        d.restore(driven); base.whole(report,'warm driven restore',driven,d.snapshot('warm-driven'))
        advance(d,4); base.whole(report,'warm driven future',driven_future,d.snapshot('warm-driven-future'))
        d.restore(driven); held=d.physical(); old_writes=d.values()['DriveWrites']; d.control('pose-release'); d.step()
        released_native=d.physical(); need(released_native['mode']=='passive' and d.values()['HoldRequested']==0 and
            d.values()['AppliedHold']==0 and d.values()['DrivePending']==0 and d.values()['DriveWrites']==old_writes+14 and
            all(j['drive']['strength']==0 and exact(j['drive']['target_rotation'],prior['drive']['target_rotation']) for j,prior in zip(released_native['joints'],held['joints'])),
            'Release disables all drives, retains targets and physical ownership')
        released=d.snapshot('released-checkpoint'); advance(d,4); released_future=d.snapshot('released-future-reference')
        d.restore(released); base.whole(report,'warm released restore',released,d.snapshot('warm-released'))
        advance(d,4); base.whole(report,'warm released future',released_future,d.snapshot('warm-released-future')); close_owner()

        d=open_owner('cold-driven'); d.restore(driven,cold=True); base.whole(report,'cold driven restore',driven,d.snapshot('cold-driven'))
        advance(d,4); base.whole(report,'cold driven future',driven_future,d.snapshot('cold-driven-future')); close_owner()
        d=open_owner('cold-released'); d.restore(released,cold=True); base.whole(report,'cold released restore',released,d.snapshot('cold-released'))
        advance(d,4); base.whole(report,'cold released future',released_future,d.snapshot('cold-released-future'))
        d.control('save'); user_save=d.archive('physical-character','ordinary-user-save'); base.settled(d); d.step(2)
        previous=d.session; _,loaded=d.control('load')
        need(loaded['runtime_replaced'] is True and d.session!=previous and d.tick==user_save['tick'],'Ordinary game Load replaces actual runtime')
        base.whole(report,'ordinary game Load restores every saved field',user_save,d.snapshot('ordinary-load'))
        base.settled(d); d.control('resume'); d.step(); need(d.values()['Active']==1 and d.physical()['mode']=='passive','Ordinary Resume preserves released physical state')
        close_owner(); base.closed_records(report)
        need(len(report['owners'])==3 and all(identity(Path(path))==pin for path,pin in report['inputs'].items()) and
             all(identity(Path(path))==pin for path,pin in report['copies'].items()) and time.monotonic()<f.DEADLINE,'All original inputs and bounded natural owners')
        report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if io is not None:
            try:io.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        report['passed']=report['passed'] and not report['errors']; report['rpc_count']=len(report['calls'])
        report['natural_cli_owners']=sum(o.get('natural_exit') is True and o.get('forced_cleanup') is False and o.get('exit_code')==0 for o in report['owners'])
        report['output_census_before_report']=base.census(args.output)
        raw=(json.dumps(report,indent=2,allow_nan=False)+'\n').encode()
        need(len(raw)<=32*MIB and report['output_census_before_report']['bytes']+len(raw)<=base.MAX_OUTPUT,'Complete bounded evidence, never truncate fields')
        with (args.output/'evidence.json').open('xb')as stream:stream.write(raw)
        print(json.dumps(dict(passed=report['passed'],evidence=str(args.output/'evidence.json'))))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
