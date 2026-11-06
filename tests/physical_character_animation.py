#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded compiled imported-character animation-target workflow, without rendering.

An external owner must supervise the native process tree. Complete104-field
state, original licensed content, literal requests/replies and full native
save payloads remain intact. This is not balance, recovery, appearance,
publication, export, hardware input or representative performance evidence.
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

sys.dont_write_bytecode=True
import physical_character_drives as prior
import physical_character_gameplay_contract as base
import physical_drive_gameplay as drive_contract

ROOT=Path(__file__).resolve().parents[1];SELF=Path(__file__).resolve()
f,need,exact=base.f,base.need,base.exact
RIG,TARGET,PLAYER=base.RIG,base.TARGET,base.PLAYER
JOINTS,STRENGTHS=prior.JOINTS,prior.STRENGTHS
read,identity,clone=base.read,base.identity,copy.deepcopy
ADDED={**dict.fromkeys(('TargetTransitionMode TargetBlendTicks TargetRestart AppliedTargetRestart TargetPlaybackWrites TargetRateWrites '
    'ObservedTargetAvailable ObservedTargetClip ObservedTargetTransition ObservedAnimationSources ObservedSelectedSource').split(),'int32'),
    **dict.fromkeys('TargetSpeed ObservedTargetTime ObservedTargetSpeed ObservedTargetProgress ObservedSelectedCorrection'.split(),'float64'),
    'ObservedTargetTick':'int64'}
FIELDS=dict(prior.FIELDS,**ADDED)
ACTIONS=dict(prior.ACTIONS,**{'target-speed':520,'target-restart':521,'target-transition':522,'target-blend':523})
SECONDS,CALLS,MIB=420,1800,1024**2


def typed(values,schema):
    need(len(prior.FIELDS)==87 and len(ADDED)==17 and len(FIELDS)==104 and type(values)is dict and set(values)==set(FIELDS) and
         schema['identity']==base.IDENTITY and len(schema['fields'])==104,'Complete original87 plus17 target-control fields')
    offset,layout=0,{}
    for name,kind in FIELDS.items():
        size=16 if kind=='entity'else 4 if kind=='int32'else 8;alignment=min(size,8);offset=(offset+alignment-1)//alignment*alignment
        layout[name]=dict(name=name,kind=kind,offset=offset,bytes=size);offset+=size;value=values[name]
        valid=(type(value)is str and re.fullmatch('[0-9a-f]{32}',value)is not None)if kind=='entity'else(
            type(value)is int and -(2**31)<=value<2**31)if kind=='int32'else(
            type(value)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value)is not None and -(2**63)<=int(value)<2**63)if kind=='int64'else type(value)is float and math.isfinite(value)
        need(valid,'Uncoerced complete gameplay scalar: '+name)
    need((offset+7)//8*8==656 and type(schema['bytes'])is int and schema['bytes']==656 and
         [x['name']for x in schema['fields']]==sorted(FIELDS) and all(exact(row,layout[row['name']])for row in schema['fields']),
         'Independent complete104-field656-byte layout')
    return values


def source_contract(args,report):
    document,profile,regions,data=prior.source_contract(args,report)
    need(profile['animation_drive']==dict(limit_policy='fit'),'Explicit authored original-joint target fitting permission')
    for action,value in ACTIONS.items():
        ui=document['ui'][f'{value:032x}']
        need(ui['action']==action,'Actual original/new authored action binding: '+action)
    need(document['ui'][f'{524:032x}']['kind']=='label','Authored target status readout')
    return document,profile,regions,data


def admitted_target(q,limits):
    n=math.hypot(q[0],q[3]);need(n>0,'Resolved fitted quaternion chart')
    twist=[q[0]/n,0.,0.,q[3]/n];swing=prior.product(q,[-twist[0],0.,0.,twist[3]]);angle=2*math.atan2(twist[0],twist[3])
    if limits['kind']=='hinge':
        need(abs(q[1])<=1e-8 and abs(q[2])<=1e-8 and limits['minimum']-1e-10<=angle<=limits['maximum']+1e-10,
             'Fitted hinge satisfies original native admission')
    else:
        need((swing[1]/math.sin(limits['swing_y']/2))**2+(swing[2]/math.sin(limits['swing_z']/2))**2<=1+1e-10 and
             limits['twist_min']-1e-10<=angle<=limits['twist_max']+1e-10,'Fitted target satisfies original ellipse/twist admission')


class Driver(prior.Driver):
    def game(self):
        value=self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True));module=value['module']
        need(value['session_id']==self.session and value['tick']==self.tick and module['backend']==self.backend and
             module['assembly_sha256']==self.report['game_image']['sha256'],'Actual selected compiled104-field game and clock')
        typed(module['values'],module['schema'])
        if self.backend=='native_aot':need(exact(module['schema'],self.report['native_artifact']['descriptor']['schema']),'Complete native artifact schema')
        return value

    def control(self,action,error=None):
        state,game=self.state(),self.game();key=f'{ACTIONS[action]:032x}'
        ui=self.io.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256));selected=[x for x in ui['elements']if x['id']==key]
        need(len(selected)==1 and selected[0]['action']==action and selected[0]['eligible']is True,'Actual eligible target control: '+action)
        request=dict(session_id=self.session,request_id=base.uid(),expected_tick=self.tick,expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=key)
        result=self.io.rpc('runtime.ui.activate',request,error)
        if error is None:
            need(result['replayed']is False,'New ordinary control');self.session,self.tick=result['current_session_id'],result['current_tick']
        self.report['controls'].append(dict(action=action,request=request,result=result,expected_error=error));return request,result

    def edit(self,**values):
        need(set(values)<=set(STRENGTHS)|{'TargetSpeed','TargetBlendTicks','TargetTransitionMode','TargetRestart'},'Explicit sample controls only')
        request=dict(session_id=self.session,request_id=base.uid(),expected_tick=self.tick,expected_revision=self.game()['revision'],values=values)
        result=self.io.rpc('runtime.gameplay.edit',request);need(result['replayed']is False,'New ordinary typed control edit')
        self.report['edits'].append(dict(request=request,result=result))

    def target(self):
        state=self.query('runtime.physical_animation',entity=RIG)
        need(set(state)=={'entity','sampled_tick','limit_policy','playback','joints'} and state['entity']==RIG and
             type(state['sampled_tick'])is int and state['sampled_tick']==self.tick and state['limit_policy']=='fit','Complete committed target owner')
        need(set(state['playback'])=={'clip','time','speed','loop','playing','duration','transition'} and
             [j['name']for j in state['joints']]==JOINTS,'Original unlayered playback and complete14-joint membership')
        for joint,authored in zip(state['joints'],self.profile['joints']):
            need(set(joint)=={'name','source','sample'} and joint['source']in('manual','animation'),'Closed source attribution')
            sample=joint['sample']
            if joint['source']=='manual':need(sample is None,'No misleading manual-source sample');continue
            need(type(sample)is dict and set(sample)=={'requested_rotation','target_rotation','correction_radians','chart_condition','hinge_projected','twist_clamped','swing_clamped'},'Complete explicit fitting diagnostics')
            for field in ('requested_rotation','target_rotation'):
                q=sample[field];base.vector(q,4);need(abs(sum(x*x for x in q)-1)<=2e-12,'Finite unit sampled target')
            for field in ('correction_radians','chart_condition'):need(type(sample[field])is float and math.isfinite(sample[field]),'Typed finite fitting metric')
            need(0<=sample['correction_radians']<=math.pi and 4e-5<=sample['chart_condition']<=1.000001 and
                 all(type(sample[n])is bool for n in ('hinge_projected','twist_clamped','swing_clamped')),'Declared finite fitting domain and flags')
            admitted_target(sample['target_rotation'],authored['limits'])
        return state

    def target_observe(self,label):
        target,native,game=self.target(),self.physical(),self.game()
        for row,body in zip(target['joints'],native['joints']):
            if row['source']=='animation':
                dot=abs(sum(a*b for a,b in zip(row['sample']['target_rotation'],body['drive']['target_rotation'])))
                need(abs(dot-1)<=2e-12,'Fitted intent joins committed effective native target')
        row=dict(label=label,tick=self.tick,target=target,physical=native,game=game)
        self.report['target_observations'].append(row);return row

    def archive(self,slot,label,receipt=None,current_game=None):
        manifest_path=self.saves/('slot-'+slot)/'current.json';member=read(manifest_path,MIB)['payload']['current']
        need(type(member['file'])is str and re.fullmatch('p-[0-9a-f]{32}[.]bin',member['file']),'Safe original complete save member')
        path=manifest_path.parent/member['file'];pin=identity(path,16*MIB)
        need(pin=={k:member[k]for k in ('bytes','sha256')},'Whole original save/manifest identity')
        if receipt is not None:need(pin=={k:receipt[k]for k in ('bytes','sha256')},'Original write receipt joins complete payload')
        raw=path.read_bytes();doc=read(path,16*MIB);payload=doc['snapshot']['payload'];game=payload['gameplay']
        need(set(doc)=={'format','version','document','snapshot'} and doc['format']=='poima.world-save' and type(doc['version'])is int and doc['version']==1 and
             game['backend']==self.backend and game['type']==base.GAME and game['assembly_sha256']==self.report['game_image']['sha256'],'Complete original selected compiled game save')
        typed(game['values'],game['schema'])
        if current_game is not None:need(payload['tick']==self.tick and exact(game['values'],current_game['module']['values']),'Every saved gameplay field equals current typed state')
        original=drive_contract.original_member(drive_contract.original_member(raw,'snapshot'),'payload')
        need(hashlib.sha256(original.encode()).hexdigest()==doc['snapshot']['sha256'],'Original unreformatted inner payload checksum')
        physical=payload['physical'];need(set(physical)=={'version','bootstrap','rigs','constraint_order','owner','physics'} and physical['version']==6 and
            len(physical['rigs'])==1 and physical['rigs'][0]['entity']==RIG,'Complete target-enabled physical extension6')
        rig=physical['rigs'][0];controls=rig['membership']['drives'];need([x['joint']for x in controls]==JOINTS,'All14 saved effective controls')
        target=rig['animation'];player=target['player']
        need(set(target)=={'version','entity','tick','player','joints'} and target['version']==1 and target['entity']==RIG and target['tick']==payload['tick'] and
             set(player)=={'format','version','tick','entity','player','history','layer_history'} and player['format']=='poima.animation-target-state' and
             player['version']==1 and player['tick']==payload['tick'] and player['entity']==RIG and player['layer_history']==[],
             'Complete original independent target player envelope')
        history=player['history'];need(set(history)=={'current','previous'} and history['current']['tick']==payload['tick'],'Complete target current/previous histories')
        if history['previous']is not None:need(history['previous']['tick']==payload['tick']-1,'Target previous history is the preceding native tick')
        need([x['joint']for x in target['joints']]==JOINTS,'Complete saved source and sampled target membership')
        if payload['tick']==self.tick:
            native,current=self.physical(),self.target()
            need(exact(controls,[dict(joint=j['name'],target_rotation=j['drive']['target_rotation'],strength=j['drive']['strength'])for j in native['joints']]),'All saved effective targets/strengths equal committed native controls')
            expected=[]
            for j in current['joints']:
                sample=j['sample']
                # The saved codec has its own explicit requested/target names.
                # Compare that defined representation; retain both originals.
                saved_sample=None if sample is None else dict(requested=sample['requested_rotation'],target=sample['target_rotation'],
                    **{n:sample[n]for n in ('correction_radians','chart_condition','hinge_projected','twist_clamped','swing_clamped')})
                expected.append(dict(joint=j['name'],source=j['source'],sample=saved_sample))
            need(exact(target['joints'],expected),'All saved sources and fit samples equal the committed codec representation')
        saved=dict(label=label,slot=slot,generation=member['generation'],tick=payload['tick'],path=str(path),identity=pin,
            manifest_path=str(manifest_path),manifest_identity=identity(manifest_path),receipt=receipt)
        self.report['archives'].append(saved);base.census(self.report['output']);return saved


def step_observed(d,label):
    before=d.target_observe(label+' before');old=before['game']['module']['values'];t=before['target'];physical=before['physical'];tick=d.tick
    request,result=d.step();values=d.values()
    if physical['mode']=='animated':
        expected=dict(ObservedTargetAvailable=0,ObservedTargetClip=-1,ObservedTargetTransition=-1,ObservedAnimationSources=0,
            ObservedSelectedSource=-1,ObservedTargetTick='-1',ObservedTargetTime=0.,ObservedTargetSpeed=0.,ObservedTargetProgress=0.,ObservedSelectedCorrection=0.)
    else:
        playback=t['playback'];transition=playback['transition'];chosen=t['joints'][old['SelectedJoint']];sample=chosen['sample']
        expected=dict(ObservedTargetAvailable=1,ObservedTargetClip=playback['clip']if playback['clip']is not None else -1,
            ObservedTargetTransition=-1 if transition is None else 1 if transition.get('mode','crossfade')=='inertial'else 0,
            ObservedAnimationSources=sum(x['source']=='animation'for x in t['joints']),ObservedSelectedSource=1 if chosen['source']=='animation'else 0,
            ObservedTargetTick=str(tick),ObservedTargetTime=playback['time'],ObservedTargetSpeed=playback['speed'],
            ObservedTargetProgress=0. if transition is None else transition['weight'],ObservedSelectedCorrection=0. if sample is None else sample['correction_radians'])
    need(exact({key:values[key]for key in expected},expected),'Compiled target readbacks describe the actual previous committed owner')
    after=d.target_observe(label+' after');d.report['target_steps'].append(dict(label=label,request=request,result=result,before=before,after=after,expected_observed=expected));return before,after


def phase(d,preset,label):
    row=d.target_observe(label);values=row['game']['module']['values'];native=row['physical'];target=row['target']
    need(values['HoldRequested']==values['AppliedHold']==1 and values['PosePreset']==values['AppliedPreset']==preset and values['DrivePending']==0,'Compiled requested/applied preset settled')
    strengths=[values[name]for name in STRENGTHS]
    need(exact([j['drive']['strength']for j in native['joints']],strengths),'Every native joint has its exact requested strength')
    if preset>=2:
        need(target['playback']['clip']==(values['IdleClip']if preset==2 else values['RunClip']) and target['playback']['playing']and target['playback']['loop']and
             target['playback']['speed']==values['TargetSpeed']and all(j['source']=='animation'for j in target['joints']),'Live source follows the independently sampled original clip')
    else:
        need(all(j['source']=='manual'for j in target['joints']),'Static preset selects manual sources')
        for actual,wanted in zip(native['joints'],d.data['presets'][preset]['targets']):
            need(all(abs(a-b)<2e-14 for a,b in zip(actual['drive']['target_rotation'],wanted['target_rotation'])),'Actual static preset orientation with existing canonicalization resolution')
    need(native['mode']==('driven'if any(strengths)else'passive'),'Actual mode follows effective strength');return row


def continued_clock(before,after,speed,ticks=1):
    a,b=before['playback'],after['playback'];expected=(a['time']+speed*ticks/60)%a['duration']
    need(a['clip']==b['clip']and b['speed']==speed and abs(b['time']-expected)<=2e-12,'Rate-only/steady advancement never seeks or restarts')
    if a['transition']is not None and b['transition']is not None:
        need(a['transition']['start_tick']==b['transition']['start_tick']and a['transition']['duration_ticks']==b['transition']['duration_ticks'],
             'Existing transition origin/duration survive nonreplacement writes')


def shoot_live(d):
    d.aim('chest');before=d.observe('live-before-real-shot');target=d.target();ray=d.camera_hit();old=before['game']['module']['values']
    need(old['Cooldown']<=1,'Real firing cooldown elapsed');request,result=d.step(fire=True);after=d.observe('live-after-real-shot');new=after['game']['module']['values'];hit=ray['result']['hit'];part=ray['expected_part']
    need(new['Shots']==old['Shots']+1 and new['Hits']==old['Hits']+1 and new['Misses']==old['Misses']and new['Takeovers']==old['Takeovers']and
         new['LastShotKind']==4 and new['LastPhysicalCommand']==2 and new['LastHitPart']==part['index']and new['LastHitRegion']==part['entity']and
         new['LastHitEntity']==TARGET and new['LastShotTick']==str(before['tick'])and new['Health']==max(0.,old['Health']-old['Damage']),
         'Actual camera hit on live physical character applies one localized impulse without reactivation')
    for prefix,vector in (('LastOrigin',ray['origin']),('LastAim',ray['direction']),('LastHit',hit['position']),('LastImpulse',[x*old['ShotImpulse']for x in ray['direction']])):
        need(all(abs(new[prefix+axis]-value)<2e-12 for axis,value in zip('XYZ',vector)),'Actual named hit/impulse spatial record')
    current=d.target();continued_clock(target,current,old['TargetSpeed']);need(all(x['source']=='animation'for x in current['joints']),'Localized hit retains live source ownership')
    need(new['TargetPlaybackWrites']==old['TargetPlaybackWrites']and new['TargetRateWrites']==old['TargetRateWrites']and new['DriveWrites']==old['DriveWrites'],
         'Hit does not rewrite steady target controls')
    saved=d.snapshot('live-hit-complete');d.report['shots'].append(dict(label='live-native-camera-hit',request=request,result=result,ray=ray,before=before,after=after,archive=saved))


def future(d,label):
    d.control('target-speed');step_observed(d,label+' speed');d.control('pose-preset');step_observed(d,label+' clip')
    for i in range(3):step_observed(d,label+' advance '+str(i))
    return d.snapshot(label+' complete')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','source','license','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--backend',choices=('coreclr','native_aot'),default='coreclr')
    for name in ('hostfxr','bridge','assembly','descriptor'):parser.add_argument('--'+name,type=Path)
    args=parser.parse_args();need(not sys.flags.optimize,'Nonoptimized verifier required')
    for name,value in vars(args).items():
        if isinstance(value,Path):setattr(args,name,value.resolve())
    need((args.backend=='coreclr'and all((args.hostfxr,args.bridge,args.assembly))and args.descriptor is None)or
         (args.backend=='native_aot'and args.descriptor is not None and all(x is None for x in (args.hostfxr,args.bridge,args.assembly))),
         'Select exact CoreCLR inputs OR a NativeAOT descriptor')
    need(not args.output.exists(),'Fresh output preserves previous attempts');args.output.mkdir(parents=True);args.windows_interop=False
    f.ARGS,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED=args,time.monotonic()+SECONDS,CALLS,48*MIB
    report=dict(format='poima.physical-character-animation.v1',passed=False,errors=[],calls=[],owners=[],archives=[],whole_pairs=[],
        observations=[],controls=[],steps=[],shots=[],camera_hits=[],inputs={},copies={},checks=[],edits=[],drive_observations=[],target_observations=[],target_steps=[],
        output=str(args.output),backend=args.backend,limits=dict(seconds=SECONDS,rpc=CALLS,output_bytes=base.MAX_OUTPUT,owners=2),
        scope='Original imported character, supplied compiled104-field game, live/static target controls and complete same-host save continuation. No balance, recovery, rendering, hardware input, publication, export or performance claim.')
    f.REPORT=report;io=None
    try:
        document,profile,regions,data=source_contract(args,report)
        files={SELF,prior.DATA,args.binary,args.source,args.license,ROOT/'examples/physical-character-yard/source-manifest.json',
            ROOT/'examples/physical-character-yard/PhysicalCharacterGame.cs',ROOT/'examples/physical-character-yard/Poima.PhysicalCharacterGame.csproj',
            ROOT/'examples/physical-character-yard/profiles/knight.json',ROOT/'managed/Directory.Build.props'}
        if args.backend=='coreclr':
            files.update((args.hostfxr,args.bridge,args.assembly))
            for image in (args.bridge,args.assembly):
                files.add(image.parent/'Poima.Gameplay.dll')
                for suffix in ('.deps.json','.runtimeconfig.json'):
                    if image.with_suffix(suffix).is_file():files.add(image.with_suffix(suffix))
            need(identity(args.bridge.parent/'Poima.Gameplay.dll')==identity(args.assembly.parent/'Poima.Gameplay.dll'),'Identical actual compiled SDK')
            report['game_image']=identity(args.assembly);config=dict(hostfxr=str(args.hostfxr),bridge=str(args.bridge),assembly=str(args.assembly),type=base.GAME)
        else:
            import game_stdio_contract as bundle
            descriptor=read(args.descriptor);library=bundle.relative(args.descriptor.parent,descriptor['library'])
            features={'baseline_v7','animation_rate_v1','animation_grounding_control_v1','animation_ik_end_orientation_v1','animation_ik_v1',
                'animation_inertial_v1','character_input_v1','hit_regions_v1','input_actions_v1','physical_rig_v1','physical_rig_drive_v1','physical_rig_animation_v1'}
            need(descriptor['format']=='poima.native-gameplay'and descriptor['version']==2 and descriptor['identity']==base.IDENTITY and descriptor['type']==base.GAME and
                 descriptor['minimum_services_bytes']==496 and descriptor['services_version']==7 and len(descriptor['required_features'])==len(features)and set(descriptor['required_features'])==features,
                 'Actual496 animation-target NativeAOT artifact with exact feature requirements')
            need(1<=len(descriptor['files'])<=64,'Bounded complete native payload');files.update((args.descriptor,library))
            for item in descriptor['files']:
                path=bundle.relative(args.descriptor.parent,item['path']);need(identity(path)==dict(bytes=item['size'],sha256=item['sha256']),'Entire original native published payload identity');files.add(path)
            report['game_image']=identity(library);report['native_artifact']=dict(path=str(args.descriptor),identity=identity(args.descriptor),descriptor=descriptor)
            config=dict(descriptor=str(args.descriptor),expected_descriptor_sha256=identity(args.descriptor)['sha256'])
        for module in tuple(sys.modules.values()):
            path=getattr(module,'__file__',None)
            if path and Path(path).suffix=='.py'and Path(path).resolve().is_relative_to(ROOT):files.add(Path(path).resolve())
        report['inputs'].update({str(path):identity(path)for path in sorted(files)});base.copied_inputs(args,report);saves=args.output/'saves';saves.mkdir()
        def open_owner(name):
            nonlocal io
            need(io is None and len(report['owners'])<2,'One bounded native owner at a time');io=f.Session(args.output/name,args.output/'world.json')
            need(io.rpc('save.configure',dict(request_id=base.uid(),expected_generation=0,root=str(saves)))['generation']==1,'Actual independent save routing')
            return Driver(io,report,config,document['revision'],saves,profile,regions,data=data,backend=args.backend)
        def close_owner():
            nonlocal io
            io.close();need(io.process.poll()is not None,'Actual closure before next owner');io=None

        d=open_owner('primary');d.start();v=d.values()
        need(v['Active']==0 and v['HoldRequested']==v['AppliedHold']==0 and v['TargetTransitionMode']==1 and v['TargetBlendTicks']==12 and v['TargetSpeed']==1. and
             v['TargetPlaybackWrites']==v['TargetRateWrites']==v['TargetRestart']==v['AppliedTargetRestart']==0 and v['ObservedTargetTick']=='-1','Actual initial compiled target defaults')
        d.control('begin');base.settled(d);d.step(12);base.grounded(d.observe('begun-grounded'))
        animated=d.snapshot('animated-origin');before=d.observe('original-grounded-before-live-hold',all_nodes=True);target_before=d.target();physical_before=d.physical()
        d.control('pose-preset');d.control('pose-preset');d.control('pose-hold')
        need(d.values()['PosePreset']==2 and d.values()['DrivePending']==1 and exact(d.target(),target_before)and exact(d.physical(),physical_before),
             'Logical LiveIdle/Hold requests leave committed target and physics untouched')
        queued=d.snapshot('queued-live-hold-before-conflict')
        _,rejected=d.step(error=-32040,physical_rigs=[dict(entity=RIG,animation=dict(clip=36,time=0.,speed=1.,loop=True,playing=True))])
        need('Duplicate physical target playback channel' in rejected['message'],'Actual caller/compiled replacement conflict')
        base.whole(report,'complete live activation/playback rollback',queued,d.snapshot('queued-live-hold-after-conflict'))
        step_observed(d,'first-live-takeover');live=phase(d,2,'live-idle-committed');v=live['game']['module']['values'];old=before['game']['module']['values']
        need(v['Takeovers']==old['Takeovers']+1 and v['DriveWrites']==old['DriveWrites']+14 and v['TargetPlaybackWrites']==1 and v['TargetRateWrites']==0 and
             v['Shots']==old['Shots']and v['Hits']==old['Hits'],'One genuine activation,14 source writes and one independent playback replacement')
        direct=d.snapshot('live-takeover-complete');terminal=read(direct['path'])['snapshot']['payload']['physical']['rigs'][0]['procedural_terminal']
        need(terminal['captured_tick']==before['tick']and exact(terminal['poses'],[dict(entity=row['id'],**row['local_transform'])for row in before['all_nodes']]),
             'Live takeover keeps all57 original committed source-node locals')
        initial_live=d.snapshot('initial-live-checkpoint')
        d.control('pose-strength');v=d.values();chosen=v['SelectedJoint'];need(chosen==2 and v[STRENGTHS[chosen]]==.75,'Ordinary selected strength step')
        a,b=step_observed(d,'strength-only-live');continued_clock(a['target'],b['target'],1.)
        va,vb=a['game']['module']['values'],b['game']['module']['values'];need(vb['DriveWrites']==va['DriveWrites']+1 and vb['TargetPlaybackWrites']==va['TargetPlaybackWrites']and vb['TargetRateWrites']==va['TargetRateWrites'],
             'Strength-only writes exactly one source strength without seeking target playback')
        phase(d,2,'unequal-live-strengths')
        a,b=step_observed(d,'unchanged-live-controls');need(b['game']['module']['values']['DriveWrites']==a['game']['module']['values']['DriveWrites'],'No repeated unchanged source writes')
        d.control('target-speed');need(d.values()['TargetSpeed']==1.5,'Ordinary1to1.5 speed step');a,b=step_observed(d,'rate-only-live');continued_clock(a['target'],b['target'],1.5)
        va,vb=a['game']['module']['values'],b['game']['module']['values'];need(vb['TargetRateWrites']==va['TargetRateWrites']+1 and vb['TargetPlaybackWrites']==va['TargetPlaybackWrites']and vb['DriveWrites']==va['DriveWrites'],'One rate write leaves source strengths and transition origin intact')
        d.control('pose-preset');a,b=step_observed(d,'inertial-running');phase(d,3,'original-running-target')
        need(b['target']['playback']['transition']['mode']=='inertial'and b['target']['playback']['transition']['duration_ticks']==12,'Configured inertial original-clip transition')
        step_observed(d,'inertial-running-advance')
        for _ in range(3):d.control('pose-preset')
        a,b=step_observed(d,'interrupted-inertial-idle');phase(d,2,'interrupted-idle-target')
        need(a['target']['playback']['transition']is not None and b['target']['playback']['transition']['mode']=='inertial'and
             b['target']['playback']['transition']['start_tick']==a['tick'],'A real in-progress transition is replaced at its current boundary')
        native=d.target();writes=d.values()['TargetPlaybackWrites'];d.control('target-transition');d.control('target-blend')
        need(d.values()['TargetTransitionMode']==0 and d.values()['TargetBlendTicks']==24 and exact(d.target(),native),'Future transition settings do not silently replace playback')
        d.control('target-restart');requested=d.values()['TargetRestart'];a,b=step_observed(d,'explicit-crossfade-restart')
        transition=b['target']['playback']['transition'];need(transition is not None and transition.get('mode','crossfade')=='crossfade'and transition['duration_ticks']==24 and
             transition['start_tick']==a['tick']and b['game']['module']['values']['TargetPlaybackWrites']==writes+1 and b['game']['module']['values']['AppliedTargetRestart']==requested,
             'Explicit restart consumes one saved request with configured crossfade/duration')
        checkpoint=d.snapshot('live-before-paused-controls');d.control('menu');paused=d.values();native=d.target();d.control('target-speed');d.control('target-restart')
        need(d.values()['Active']==2 and d.values()['TargetSpeed']==2. and exact(d.target(),native),'Menu queues speed/restart without changing current native target')
        paused_request=d.snapshot('paused-target-request');a,b=step_observed(d,'explicit-tick-while-game-menu')
        va,vb=a['game']['module']['values'],b['game']['module']['values'];need(vb['TargetPlaybackWrites']==va['TargetPlaybackWrites']and vb['TargetRateWrites']==va['TargetRateWrites']and
             vb['AppliedTargetRestart']<vb['TargetRestart']and vb['TickCount']==va['TickCount'],'Explicit native tick in game menu does not consume queued gameplay controls')
        continued_clock(a['target'],b['target'],a['target']['playback']['speed'])
        d.control('resume');step_observed(d,'resume-consumes-pending-restart');phase(d,2,'resumed-live-target')
        need(d.values()['AppliedTargetRestart']==d.values()['TargetRestart'],'Pending restart consumed on next playing Tick')
        d.restore(paused_request);base.whole(report,'complete paused request restore',paused_request,d.snapshot('paused-request-restored'))
        d.control('resume');step_observed(d,'restored-paused-request-resume');phase(d,2,'restored-live-target')
        d.restore(checkpoint);base.whole(report,'complete live checkpoint restore',checkpoint,d.snapshot('live-restored-before-static'))
        for _ in range(2):d.control('pose-preset')
        a,b=step_observed(d,'live-to-static');phase(d,0,'static-relaxed')
        continued_clock(a['target'],b['target'],a['target']['playback']['speed'])
        need(b['game']['module']['values']['TargetPlaybackWrites']==a['game']['module']['values']['TargetPlaybackWrites'],'Static hold changes sources/targets without restarting independent player')
        for _ in range(2):d.control('pose-preset')
        step_observed(d,'static-to-live');phase(d,2,'live-again')
        d.edit_strengths(0.);a,b=step_observed(d,'zero-strength-live');phase(d,2,'zero-strength-source-still-live');continued_clock(a['target'],b['target'],a['target']['playback']['speed'])
        need(all(j['source']=='animation'for j in b['target']['joints'])and b['physical']['mode']=='passive','Zero active torque still samples all animation targets')
        d.restore(initial_live);base.whole(report,'whole live state before localized hit',initial_live,d.snapshot('live-restored-for-hit'));shoot_live(d)
        d.restore(checkpoint);base.whole(report,'whole live state before continuation',checkpoint,d.snapshot('live-restored-for-continuation'))
        reference=future(d,'live-reference');d.restore(checkpoint);base.whole(report,'warm live complete origin',checkpoint,d.snapshot('warm-live-origin'))
        base.whole(report,'warm live target/history continuation',reference,future(d,'live-warm'))
        d.restore(checkpoint);held=d.physical();writes=d.values()['DriveWrites'];d.control('pose-release');step_observed(d,'release-live-source')
        released=d.target_observe('released-passive');v=released['game']['module']['values']
        need(v['HoldRequested']==v['AppliedHold']==0 and v['DrivePending']==0 and v['DriveWrites']==writes+14 and released['physical']['mode']=='passive'and
             all(j['source']=='manual'for j in released['target']['joints'])and
             all(j['drive']['strength']==0 and exact(j['drive']['target_rotation'],old['drive']['target_rotation'])for j,old in zip(released['physical']['joints'],held['joints'])),
             'Release selects Manual0, retaining every effective target and physical ownership')
        released_checkpoint=d.snapshot('released-checkpoint');step_observed(d,'released-future');released_future=d.snapshot('released-future-complete')
        d.restore(released_checkpoint);base.whole(report,'warm released complete state',released_checkpoint,d.snapshot('warm-release'))
        step_observed(d,'released-warm-future');base.whole(report,'warm released continuation',released_future,d.snapshot('released-warm-future-complete'));close_owner()

        d=open_owner('cold');d.restore(checkpoint,cold=True);base.whole(report,'cold live complete state',checkpoint,d.snapshot('cold-live'))
        base.whole(report,'cold live target/history continuation',reference,future(d,'live-cold'))
        d.restore(released_checkpoint);base.whole(report,'cold owner complete released state',released_checkpoint,d.snapshot('cold-release'))
        step_observed(d,'released-cold-future');base.whole(report,'cold owner released continuation',released_future,d.snapshot('released-cold-future-complete'))
        d.control('save');user=d.archive('physical-character','ordinary-user-save');base.settled(d);d.step(2);previous=d.session;_,loaded=d.control('load')
        need(loaded['runtime_replaced']is True and d.session!=previous and d.tick==user['tick'],'Ordinary game load replaces the actual native owner')
        base.whole(report,'ordinary game load preserves every original saved field',user,d.snapshot('ordinary-game-load'));base.settled(d);d.control('resume');step_observed(d,'ordinary-loaded-resume')
        need(d.values()['Active']==1 and d.physical()['mode']=='passive','Resume preserves released physical ownership');close_owner();base.closed_records(report)
        need(len(report['owners'])==2 and all(identity(Path(p))==v for p,v in report['inputs'].items())and all(identity(Path(p))==v for p,v in report['copies'].items())and time.monotonic()<f.DEADLINE,'All original inputs and bounded naturally closed owners')
        report['checks']=['Complete104-field656-byte state and original57-node/15-part licensed character',
            'Live/static source selection, previous-commit C# reads and explicit fitting diagnostics',
            'Rate/strength edits preserve playback continuity; paused requests and interrupted transitions',
            'Duplicate target command rolls back the entire original saved state',
            'Actual localized camera hit and explicit release retain physical ownership',
            'Whole warm/cold target-player/history continuation and ordinary game save/load']
        report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if io is not None:
            try:io.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        report['passed']=report['passed']and not report['errors'];report['rpc_count']=len(report['calls'])
        report['natural_cli_owners']=sum(o.get('natural_exit')is True and o.get('forced_cleanup')is False and o.get('exit_code')==0 for o in report['owners'])
        report['output_census_before_report']=base.census(args.output);raw=(json.dumps(report,indent=2,allow_nan=False)+'\n').encode()
        need(len(raw)<=32*MIB and report['output_census_before_report']['bytes']+len(raw)<=base.MAX_OUTPUT,'Complete untruncated bounded evidence')
        with(args.output/'evidence.json').open('xb')as stream:stream.write(raw)
        print(json.dumps(dict(passed=report['passed'],evidence=str(args.output/'evidence.json'))))
    return 0 if report['passed']else 1


if __name__=='__main__':raise SystemExit(main())
