#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prebuilt CoreCLR physical-animation consumer; no builds, windows or rendering.

An external process-tree owner must supervise the finite run. Retains literal
requests/replies, all54 gameplay fields and complete native saves, including
failures. Synthetic geometry tests callbacks and state, not character quality.
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

sys.dont_write_bytecode=True
import physical_animation_protocol as p
import physical_drive_gameplay as d
import world_physical_contract as f
from gltf_fixture import glb
from managed_physical_gameplay_contract import sha,strict_json

ROOT=Path(__file__).resolve().parents[1]
GAME,IDENTITY='Poima.Tests.PhysicalAnimationGame','poima.test.physical-rig-animation'
SECONDS,CALLS,MIB=240,800,1024**2
need,exact,uid=p.need,p.exact,p.uid
FIELDS={
    'Rig':'entity',
    **dict.fromkeys('Phase Joint Clip LayerClip TransitionMode BlendTicks WeightBlendTicks Source Ticks Writes CommittedReads RejectAfterWrite'.split(),'int32'),
    **dict.fromkeys('ObservedMode ObservedClip ObservedLayerClip ObservedSource SamplePresent HingeProjected TwistClamped SwingClamped ObservedTransition ObservedLayerTransition'.split(),'int32'),
    **dict.fromkeys('ObservedTick SampledTick FailAt'.split(),'int64'),
    **dict.fromkeys('Time Speed Weight Strength TargetAngle'.split(),'float64'),
    **dict.fromkeys('ObservedTime ObservedSpeed LayerTime LayerSpeed LayerWeight LayerTargetWeight Progress LayerProgress Correction ChartCondition'.split(),'float64'),
    **dict.fromkeys('RequestedX RequestedY RequestedZ RequestedW FittedX FittedY FittedZ FittedW EffectiveX EffectiveY EffectiveZ EffectiveW EffectiveStrength'.split(),'float64')}
CONTROLS=set('Phase Joint Clip LayerClip TransitionMode BlendTicks WeightBlendTicks Source RejectAfterWrite FailAt Time Speed Weight Strength TargetAngle'.split())
WRITES={0:0,1:4,2:1,3:1,4:1,5:1,6:1,7:1,8:2,9:2}


def initial():
    values={name:'0'*32 if kind=='entity' else '0' if kind=='int64' else 0. if kind=='float64' else 0 for name,kind in FIELDS.items()}
    values.update(Rig=p.RIG,Clip=1,LayerClip=2,Source=1,Time=.25,Speed=1.,Weight=1.,Strength=.6,TargetAngle=.2,FailAt='-1')
    return values


def validate(values,schema):
    need(type(values)is dict and set(values)==set(FIELDS) and len(FIELDS)==54,'Complete54-field scalar membership')
    offset,layout=0,{}
    for name,kind in FIELDS.items():
        size=16 if kind=='entity' else 4 if kind=='int32' else 8;align=min(size,8);offset=(offset+align-1)//align*align
        layout[name]=dict(name=name,kind=kind,offset=offset,bytes=size);offset+=size;value=values[name]
        valid=(type(value)is str and re.fullmatch('[0-9a-f]{32}',value)is not None) if kind=='entity' else (
            type(value)is int and -(2**31)<=value<2**31) if kind=='int32' else (
            type(value)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value)is not None and -(2**63)<=int(value)<2**63) if kind=='int64' else type(value)is float and math.isfinite(value)
        need(valid,'Typed complete game field: '+name)
    need(schema['identity']==IDENTITY and schema['bytes']==(offset+7)//8*8==352 and len(schema['fields'])==54 and
         [v['name'] for v in schema['fields']]==sorted(FIELDS) and all(exact(v,layout[v['name']]) for v in schema['fields']),'Independent complete CLR state layout')
    return values


def model():
    # Extend the same public target fixture with a late, explicit chart failure.
    # It is finite/unit authored content; no verifier edits an archive or native
    # result to cause the rejection. Step interpolation reaches the singular
    # half-turn only on the second native tick after this clip is selected.
    raw=p.model();size,_=struct.unpack_from('<II',raw,12);document=strict_json(raw[20:20+size])
    binary_size,_=struct.unpack_from('<II',raw,20+size);binary=bytearray(raw[28+size:28+size+binary_size])
    def accessor(rows,kind):
        while len(binary)%4:binary.append(0)
        start=len(binary);flat=[x for row in rows for x in row];binary.extend(struct.pack('<'+'f'*len(flat),*flat))
        document['bufferViews'].append(dict(buffer=0,byteOffset=start,byteLength=len(binary)-start))
        document['accessors'].append(dict(bufferView=len(document['bufferViews'])-1,componentType=5126,count=len(rows),type=kind));return len(document['accessors'])-1
    times=accessor([(0.,),(1/64,),(1/32,)],'SCALAR');document['accessors'][times].update(min=[0.],max=[1/32])
    rotations=accessor([(0.,0.,0.,1.),(0.,0.,0.,1.),(1.,0.,0.,0.)],'VEC4')
    document['animations'].append(dict(name='bounded late half-turn rejection',samplers=[dict(input=times,output=rotations,interpolation='STEP')],
        channels=[dict(sampler=0,target=dict(node=1,path='rotation'))]))
    document['buffers'][0]['byteLength']=len(binary);return glb(document,bytes(binary))


def prepare(io):
    source=io.directory/'original-managed-targets.glb';source.write_bytes(model())
    imported=io.rpc('asset.import',dict(source=f.native(source)));asset=imported['asset']
    need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives'])==(4,4,1,1),'Complete managed target original import')
    profile=d.profile();profile['animation_drive']=dict(limit_policy='fit')
    layer=dict(slot=1,mode='override',clip=0,time=0.,speed=1.,loop=False,playing=False,weight=0.,mask=[dict(node=2,weight=1.)])
    ops=[dict(op='asset.instantiate',id=p.RIG,name='Compiled physical target consumer',asset=asset),
        f.component(p.RIG,'Transform',dict(**f.pose((0,200,0)),scale=[1,1,1])),
        f.component(p.RIG,'AnimationRig',dict(asset=asset,clip=0,time=0.,speed=1.,loop=False,playing=True,layers=[layer])),
        dict(op='entity.create',id=p.FLOOR,name='Independent static floor'),f.component(p.FLOOR,'Transform',dict(**f.pose((0,-.5,0)),scale=[1,1,1])),
        f.component(p.FLOOR,'BoxCollider',dict(half_extents=[20,.5,20],motion='static',mass=1,friction=.5,restitution=0))]
    need(f.transact(io,ops,0)['revision']==1 and f.transact(io,[f.component(p.RIG,'PhysicalRig',profile)],1)['revision']==2,'Complete compiled consumer authoring')
    package=Path(str(io.world)+'.assets')/(asset+'.pmodel');need(package.is_file() and sha(package)==asset,'Exact cooked target input')
    return dict(source=str(source),source_sha256=sha(source),package=str(package),package_sha256=asset,profile=profile)


class Consumer(p.Consumer):
    def __init__(self,io,report,saves,config):
        super().__init__(io,report,saves);self.config=config;self.model=initial()

    def game(self):
        result=self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
        need(set(result)=={'session_id','tick','revision','structure_revision','module'} and
             result['session_id']==self.session and type(result['tick']) is int and result['tick']==self.tick and
             all(type(result[k]) is int and result[k]>=0 for k in ('revision','structure_revision')),
             'Complete committed gameplay inspection envelope')
        return result

    def values(self,label):
        game=self.game();module=game['module']
        need(type(module) is dict and module['backend']=='coreclr' and module['type']==GAME and
             module['assembly_sha256']==self.report['assembly_sha256'],'Actual selected compiled gameplay module')
        values=validate(module['values'],module['schema']);need(exact(values,self.model),'Complete independently predicted54 fields: '+label)
        self.report['game_observations'].append(dict(label=label,tick=self.tick,values=copy.deepcopy(values),expected=copy.deepcopy(self.model)))
        return game

    def edit(self,**values):
        need(set(values)<=CONTROLS,'Only explicit ordinary fixture tuning may be edited')
        self.io.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=uid(),expected_tick=self.tick,expected_revision=self.game()['revision'],values=values))
        self.model.update(values);self.values('typed control edit')

    def single(self,label,caller=()):
        state=self.query();native=self.physical();expected=copy.deepcopy(self.model);joint=expected['Joint']
        need(joint in (0,1) and expected['Phase'] in WRITES,'Successful fixture controls')
        base,layer=state['playback'],state['playback']['layers'][0];source=state['joints'][joint];effective=native['joints'][joint]['drive']
        expected.update(Ticks=expected['Ticks']+1,Writes=expected['Writes']+WRITES[expected['Phase']],CommittedReads=expected['CommittedReads']+1,
            Phase=0,ObservedTick=str(self.tick),SampledTick=str(state['sampled_tick']),ObservedMode={'animated':0,'passive':1,'driven':2}[native['mode']],
            ObservedClip=base['clip'] if base['clip'] is not None else -1,ObservedLayerClip=layer['playback']['clip'] if layer['playback']['clip'] is not None else -1,
            ObservedSource={'manual':0,'animation':1}[source['source']],ObservedTime=base['time'],ObservedSpeed=base['speed'],
            LayerTime=layer['playback']['time'],LayerSpeed=layer['playback']['speed'],LayerWeight=layer['weight'],LayerTargetWeight=layer['target_weight'],
            EffectiveStrength=effective['strength'])
        for value,mode,progress in [(base,'ObservedTransition','Progress'),(layer['playback'],'ObservedLayerTransition','LayerProgress')]:
            transition=value['transition'];expected[mode]=-1 if transition is None else 1 if transition.get('mode','crossfade')=='inertial' else 0
            expected[progress]=0. if transition is None else transition['weight']
        sample=source['sample'];expected['SamplePresent']=int(sample is not None)
        expected['Correction']=0. if sample is None else sample['correction_radians'];expected['ChartCondition']=0. if sample is None else sample['chart_condition']
        for field,key in [('HingeProjected','hinge_projected'),('TwistClamped','twist_clamped'),('SwingClamped','swing_clamped')]:expected[field]=0 if sample is None else int(sample[key])
        for prefix,values in [('Requested',[0.]*4 if sample is None else sample['requested_rotation']),('Fitted',[0.]*4 if sample is None else sample['target_rotation']),('Effective',effective['target_rotation'])]:
            for suffix,value in zip('XYZW',values):expected[prefix+suffix]=value
        self.step(caller);self.model=expected;self.values(label)
        self.report['native_observations'].append(dict(label=label,tick=self.tick,before=copy.deepcopy(state),before_native=copy.deepcopy(native),after=self.query(),after_native=self.physical()))

    def archive(self,label):
        game=self.values(label);state=self.io.rpc('runtime.inspect',dict(session_id=self.session));slot='managed-target-'+str(len(self.report['archives'])+1)
        receipt=self.io.rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=slot,expected_generation=0,
            session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],
            expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        manifest_path=self.saves/('slot-'+slot)/'current.json';manifest=strict_json(manifest_path.read_bytes());current=manifest['payload']['current']
        need(type(current['file'])is str and re.fullmatch(r'p-[0-9a-f]{32}\.bin',current['file']),'Exact archive basename')
        path=manifest_path.parent/current['file'];need(path.stat().st_size<=16*MIB,'Bounded full archive');raw=path.read_bytes();value=strict_json(raw)
        need(len(raw)==receipt['bytes']==current['bytes'] and hashlib.sha256(raw).hexdigest()==receipt['sha256']==current['sha256'],'Complete archive receipt/manifest identity')
        payload=value['snapshot']['payload'];original=d.original_member(d.original_member(raw,'snapshot'),'payload')
        need(hashlib.sha256(original.encode()).hexdigest()==value['snapshot']['sha256'],'Original native snapshot checksum')
        need(payload['tick']==self.tick and payload['gameplay']['backend']=='coreclr' and payload['gameplay']['type']==GAME and
             payload['gameplay']['assembly_sha256']==self.report['assembly_sha256'] and exact(validate(payload['gameplay']['values'],payload['gameplay']['schema']),self.model),'All54 saved fields and exact compiled image')
        need(payload['physical']['version']==6 and len(payload['physical']['rigs'])==1,'Saved native animation extension6')
        row=dict(label=label,slot=slot,tick=self.tick,path=str(path),sha256=receipt['sha256'],bytes=len(raw),archive=value,receipt=receipt,manifest=manifest,manifest_path=str(manifest_path))
        self.report['archives'].append(copy.deepcopy(row));return row

    def load(self,row,existing=True):
        state=self.io.rpc('runtime.inspect',dict(session_id=self.session)) if existing else None;game=self.game() if existing else None;session=uid()
        result=self.io.rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot=row['slot'],expected_generation=1,revision=2,
            expected_session_id=self.session if existing else None,expected_tick=self.tick if existing else None,expected_gameplay_revision=game['revision'] if existing else None,
            expected_structure_revision=state['structure_revision'] if existing else None,expected_material_revision=state['material_revision'] if existing else None,
            expected_ui_revision=state['ui_revision'] if existing else None,expected_control_sequence=state['control_sequence'] if existing else None,new_session_id=session,gameplay=self.config))
        need(result.get('session_id',result.get('new_session_id'))==session and result['tick']==row['tick'],'Exact restored compiled owner')
        self.session,self.tick,self.model=session,result['tick'],copy.deepcopy(row['archive']['snapshot']['payload']['gameplay']['values']);self.values('restored complete game')


def reject(c,label,caller=(),ticks=1,message=None):
    before=c.archive(label+' before');error=c.step(caller,ticks=ticks,error=-32040)
    if message:need(message in error['message'],'Wrong original rejection: '+label)
    c.values(label+' unchanged');p.pair(c.report,label,before,c.archive(label+' after'))


def future(c,label):
    c.edit(Phase=2,Clip=1,Time=.5,TransitionMode=1,BlendTicks=8,Speed=1.25);c.single(label+' inertial')
    for n in range(4):c.single(label+' advance '+str(n))
    return c.archive(label+' complete')


def exercise(c):
    c.io.rpc('runtime.start',dict(session_id=c.session,revision=2))
    loaded=c.io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=uid(),expected_tick=0,expected_revision=c.game()['revision'],**c.config))
    c.report['loaded_module']=copy.deepcopy(loaded['module']);c.values('initial all54 fields');c.single('initial committed reads')
    c.edit(Phase=1);c.single('compiled activation and source selection');c.single('compiled committed streamed targets')
    c.edit(Phase=3,Joint=1,Time=0.,WeightBlendTicks=3);c.single('compiled layer replacement')
    for n in range(3):c.single('compiled fitted layer advance '+str(n))
    need(c.model['HingeProjected']==1 and c.model['TwistClamped']==1 and c.model['SamplePresent']==1,'Compiled fit flags were not observed')
    c.edit(Phase=4,Speed=.5);c.single('compiled base rate');c.edit(Phase=5,Speed=2.);c.single('compiled layer rate')
    old=c.physical()['joints'][1]['drive']['target_rotation'];c.edit(Phase=6,Source=0,Strength=.2);c.single('compiled manual hold')
    need(exact(old,c.physical()['joints'][1]['drive']['target_rotation']),'Compiled manual selection changed effective target')
    c.single('compiled manual sample null');need(c.model['SamplePresent']==0,'Compiled manual source kept a sample')
    c.edit(Phase=7,Strength=0.);c.single('compiled explicit manual target')
    c.edit(Phase=8,Joint=0,Source=1,Strength=.6,Speed=1.);c.single('compiled independent source and rate')
    for label,values,caller,message,ticks in [
        ('post-stage exception',dict(Phase=4,RejectAfterWrite=1),(),'Physical animation fixture rejected after staging.',1),
        ('native missing clip',dict(Phase=2,RejectAfterWrite=0,Clip=255,Time=0.,BlendTicks=0),(),None,1),
        ('SDK invalid source',dict(Phase=6,Source=3),(),None,1),
        ('caller duplicate target',dict(Phase=4,Source=1,Speed=1.),(dict(entity=p.RIG,animation_rate=dict(speed=.5)),),None,1),
        ('late native chart rejection',dict(Phase=2,Clip=3,Time=0.,Speed=1.,TransitionMode=0,BlendTicks=0),(),None,2)]:
        c.edit(**values);reject(c,label,caller,ticks,message)
    c.edit(Phase=4,FailAt=str(c.tick+1));reject(c,'late compiled exception',ticks=2,message='Physical animation fixture later-Tick failure.')
    c.edit(Phase=0,FailAt='-1',Clip=1)
    before=c.archive('before same-image reload');revision=c.game()['revision']
    c.io.rpc('runtime.gameplay.load',dict(session_id=c.session,request_id=uid(),expected_tick=c.tick,expected_revision=revision,**c.config))
    c.values('same-image reload');after=c.archive('after same-image reload');expected=copy.deepcopy(before['archive'])
    need(after['archive']['snapshot']['payload']['gameplay_revision']==before['archive']['snapshot']['payload']['gameplay_revision']+1,'Reload revision exact delta')
    expected['snapshot']['payload']['gameplay_revision']=after['archive']['snapshot']['payload']['gameplay_revision'];expected['snapshot']['sha256']=after['archive']['snapshot']['sha256']
    need(exact(expected,after['archive']) and before['archive']['snapshot']['sha256']!=after['archive']['snapshot']['sha256'],'Reload changed an unapproved complete-state leaf')
    c.report['reload']=dict(before=before['path'],after=after['path'],changed_paths=['/snapshot/payload/gameplay_revision','/snapshot/sha256'],whole_byte_equality_claimed=False)
    c.single('post-reload callback');origin=c.archive('driven origin');reference=future(c,'driven future reference');c.load(origin)
    p.pair(c.report,'warm whole restore',origin,c.archive('warm driven restored'));p.pair(c.report,'warm whole future',reference,future(c,'warm future'))
    return origin,reference


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    parser.add_argument('--hostfxr',type=Path,required=True);parser.add_argument('--bridge',type=Path,required=True);parser.add_argument('--assembly',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');args=parser.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    report=dict(format='poima.physical-animation-gameplay.v1',passed=False,backend='coreclr',calls=[],owners=[],archives=[],whole_pairs=[],observations=[],game_observations=[],native_observations=[],
        limits=dict(seconds=SECONDS,rpc=CALLS,recorded_bytes=32*MIB,evidence_bytes=64*MIB),
        limitations=['Analytic three-part actual CoreCLR consumer; no character motion quality, GPU, audio, NativeAOT, standalone export or performance claim.',
                    'Same-image reload preserves all values with explicit revision/checksum changes; it is not changed-code latency. Full continuation is same-platform/native image.'])
    f.ARGS,f.REPORT,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED=args,report,time.monotonic()+SECONDS,CALLS,32*MIB;io=None;pins={}
    try:
        files=[Path(__file__).resolve(),Path(p.__file__).resolve(),Path(d.__file__).resolve(),Path(f.__file__).resolve(),ROOT/'tests/managed_physical_gameplay_contract.py',
            ROOT/'tests/gltf_fixture.py',ROOT/'tests/animation_fixture.py',ROOT/'tests/managed_physical_animation_gameplay/Game.cs',ROOT/'tests/managed_physical_animation_gameplay/Poima.ManagedPhysicalAnimationGame.csproj',
            ROOT/'managed/Directory.Build.props',args.binary.resolve(),args.hostfxr.resolve(),args.bridge.resolve(),args.assembly.resolve()]
        for image in (args.bridge.resolve(),args.assembly.resolve()):
            files.append(image.parent/'Poima.Gameplay.dll')
            for suffix in ('.deps.json','.runtimeconfig.json'):
                member=image.with_suffix(suffix)
                if member.is_file():files.append(member)
        for path in set(files):need(path.is_file(),'Missing original input: '+str(path));pins[str(path)]=sha(path)
        report['assembly_sha256']=sha(args.assembly);report['input_pins']=copy.deepcopy(pins)
        config=dict(hostfxr=f.native(args.hostfxr),bridge=f.native(args.bridge),assembly=f.native(args.assembly),type=GAME)
        io=f.Session(output/'original-owner');report['authoring']=prepare(io);world,original=io.world,io.world.read_bytes();saves=f.save_root(io)
        for name in ('source','package'):path=Path(report['authoring'][name]);pins[str(path)]=sha(path)
        report['input_pins']=copy.deepcopy(pins);c=Consumer(io,report,saves,config);origin,reference=exercise(c);io.close();io=None
        io=f.Session(output/'cold-driven-owner',world);io.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=f.native(saves)))
        c=Consumer(io,report,saves,config);c.load(origin,False);p.pair(report,'fresh driven whole restore',origin,c.archive('fresh driven restored'))
        p.pair(report,'fresh driven whole future',reference,future(c,'fresh driven future'))
        c.edit(Phase=9);c.single('compiled release both');need(c.physical()['mode']=='passive','Compiled release must retain passive ownership')
        released=c.archive('released origin')
        for n in range(3):c.single('released future '+str(n))
        released_future=c.archive('released reference');io.close();io=None
        io=f.Session(output/'cold-released-owner',world);io.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=f.native(saves)))
        c=Consumer(io,report,saves,config);c.load(released,False);p.pair(report,'fresh released whole restore',released,c.archive('fresh released restored'))
        for n in range(3):c.single('released cold future '+str(n))
        p.pair(report,'fresh released whole future',released_future,c.archive('released cold complete'))
        need(world.read_bytes()==original,'Managed verification changed authoring');io.close();io=None
        need(len(report['owners'])==3 and all(o['natural_exit'] and not o['forced_cleanup'] and o['exit_code']==0 for o in report['owners']),'Three actual CLI owners must close naturally')
        need(all(sha(path)==digest for path,digest in pins.items()),'Original consumer inputs changed');report['passed']=True
    except Exception as error:report['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
    finally:
        if io is not None:
            try:io.close()
            except Exception as error:report['closure_failure']=dict(type=type(error).__name__,message=str(error));report['passed']=False
        report['elapsed_seconds']=time.monotonic()-(f.DEADLINE-SECONDS);report['recordings']=[]
        for owner in report['owners']:
            for name in ('requests.jsonl','responses.jsonl','stderr.log'):
                path=Path(owner['directory'])/name
                if path.is_file():report['recordings'].append(dict(path=str(path),bytes=path.stat().st_size,sha256=sha(path)))
        encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        if len(encoded.encode())>64*MIB:report['passed']=False;report['failure']=dict(type='EvidenceBound',message='Full evidence exceeds64MiB; no fields removed');encoded=json.dumps(report,indent=2,allow_nan=False)+'\n'
        (output/'evidence.json').write_text(encoded,encoding='utf-8')
        print(json.dumps(dict(passed=report['passed'],output=str(output),**{key:len(report[key]) for key in ('calls','owners','archives','whole_pairs','game_observations')})))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
