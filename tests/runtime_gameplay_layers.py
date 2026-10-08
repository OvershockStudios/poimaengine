#!/usr/bin/env python3
"""Qualify real compiled C# masked layers, owned processes and exact saves.

CoreCLR compiles this small original fixture against a supplied SDK; Native AOT
uses the separate runner and an already published artifact. Nothing rewrites
runtime poses, retries uncertain mutations, or changes supplied binaries.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid
import wave
from xml.sax.saxutils import escape
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from verify_gameplay_inertial import (Game as BaseGame,WorldClient,digest,require,
    without_session,blend_fixture,command,component,node,transform,uid,correction,glb)
FIXTURE=ROOT/'tests/fixtures/gameplay_layers/LayerGame.cs'
TYPE='Poima.Verification.LayerGame'


def authored(slot,mode,clip,weight,**extra):
    return dict(slot=slot,mode=mode,clip=clip,time=0,speed=1,loop=False,playing=False,
                weight=weight,mask=[dict(node=1,weight=1)],**extra)


class LayerGame(BaseGame):
    def __init__(self,*args,native_aot=False,**kwargs):
        super().__init__(*args,**kwargs);self.native_aot=native_aot
    def load(self,artifact,error=None):
        params=dict(session_id=self.session,request_id=uuid.uuid4().hex,
                    expected_tick=self.tick,expected_revision=self.revision)
        if self.native_aot:
            method='runtime.gameplay.load_native'
            params.update(descriptor=self.native(artifact),expected_descriptor_sha256=digest(artifact))
        else:
            method='runtime.gameplay.load'
            params.update(hostfxr=self.native(self.hostfxr),bridge=self.native(self.bridge),
                          assembly=self.native(artifact),type=TYPE)
        result=self.rpc(method,params,error)
        if error is None:
            self.revision+=1;require(result['revision']==self.revision,'Load revision differs')
            module=result['module']
            require(module['backend']==('native_aot' if self.native_aot else 'coreclr'),'Wrong actual compiled backend')
            require('requirements' not in module['schema'],'Requirements altered saved state schema')
            if self.native_aot:
                require(module['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Artifact is not actual Native AOT')
                require(module['schema']==json.loads(artifact.read_text())['schema'],'Actual artifact schema differs')
            require(self.rpc(method,params)==dict(result,replayed=True),'Load receipt did not replay exactly')
        return result
    def layer(self,slot=1):
        return self.layer_for(self.rig,slot)
    def layer_for(self,identity,slot):
        return next(row for row in self.animation(identity)['layers'] if row['slot']==slot)
    def query_matches(self,expected,observed,tick,base):
        playback=expected['playback'];transition=playback['transition'];fade=expected['weight_transition']
        mapping=dict(QueryTick=tick,QSlot=expected['slot'],QMode=int(expected['mode']=='additive'),
            QMaskNodes=expected['mask_nodes'],QWeight=expected['weight'],QTargetWeight=expected['target_weight'],
            QClip=-1 if playback['clip'] is None else playback['clip'],QTime=playback['time'],QSpeed=playback['speed'],
            QDuration=playback['duration'],QLoop=int(playback['loop']),QPlaying=int(playback['playing']),
            QHasTransition=int(transition is not None),QTransitionMode=-1 if transition is None else int(transition.get('mode')=='inertial'),
            QProgress=-1 if transition is None else transition['weight'],QHasWeightTransition=int(fade is not None),QBaseMatches=1,
            QStartTick=0,QDurationTicks=0,QElapsedTicks=0,QSourceFrozen=0,
            QWeightStartTick=0,QWeightDurationTicks=0,QWeightElapsedTicks=0,QWeightSource=0,QWeightTarget=0,
            QBaseClip=-1 if base['clip'] is None else base['clip'],QBaseTime=base['time'],
            QBaseMode=-1 if base['transition'] is None else int(base['transition'].get('mode')=='inertial'),
            QBaseProgress=-1 if base['transition'] is None else base['transition']['weight'])
        if transition:
            mapping.update(QStartTick=transition['start_tick'],QDurationTicks=transition['duration_ticks'],
                QElapsedTicks=transition['elapsed_ticks'],QSourceFrozen=int(transition['source_frozen']))
        if fade:
            mapping.update(QWeightStartTick=fade['start_tick'],QWeightDurationTicks=fade['duration_ticks'],
                QWeightElapsedTicks=fade['elapsed_ticks'],QWeightSource=fade['source'],QWeightTarget=fade['target'])
        for key,value in mapping.items():
            require(abs(float(observed[key])-value)<1e-10,'Committed layer getter differs for '+key)
    def step(self,count,animations=(),error=None,marked=True):
        base=self.animation() if error is None and count==1 and not animations else None
        expected=next(row for row in base['layers'] if row['slot']==int(self.values()['QuerySlot'])) if base is not None else None
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,ticks=count,animations=list(animations)),error)
        if error is None:
            previous=self.tick;self.tick+=count;require(result['tick']==self.tick,'Step tick differs')
            if expected is not None:self.query_matches(expected,self.values(),previous,base)
        return result


def exercise(game,artifact,save,record,stamp,reloads=None):
    game.start();game.load(artifact);game.configure(3);game.step(1)
    require(game.values()['NullProbe']==1,'Missing slot/non-rig getter did not return null')
    require({'constructor','initialize','tick'}<=set(stamp.read_text().splitlines()),'Positive constructor/Initialize/Tick sentinels not observed')
    game.configure(8,Clip=0,Time=.5,Speed=1,Playing=1,Loop=0);game.step(29)
    # Base starts .5 at tick1, advances29 ticks: .983333...; upper initial weight0.
    game.configure(Clip=1,Time=0,Weight=1,Playing=0,BlendTicks=0,WeightBlendTicks=60);game.step(30)
    t=(.5+29/60)+.5;game.position((t*.5+1,2,0));require(game.layer()['weight']==.5,'Managed weight fade not independent')
    game.configure(Weight=0,WeightBlendTicks=30);game.step(15)
    game.position(((t+.25)*.75+.5,1.5,0));require(game.layer()['weight']==.25,'Managed interrupted weight fade differs')
    game.step(15);require(game.layer()['weight']==0 and game.layer()['weight_transition'] is None,'Weight fade failed to finish')
    game.configure(Slot=2,QuerySlot=2,Clip=1,Weight=.5,WeightBlendTicks=0);game.step(1)
    # Frozen additive reference A(.5), layer B(0): delta(1.5,2).
    base_x=min(2,t+.25+.25+1/60);game.position((base_x+.75,2,0))
    require(game.values()['QMode']==1 and game.values()['QMaskNodes']==1,'Compiled additive metadata differs')
    require(game.values()['StagingChecks']>0,'No compiled staging equality checks executed')
    record['checks']['compiled_committed_queries_override_additive_weight_interruption_and_staging']=True

    game.start();game.load(artifact)
    game.configure(8,Clip=0,Time=0,Playing=1,Loop=0);game.step(30)
    game.configure(Clip=1,Playing=0,Weight=1);game.step(1)
    # Enable B then hold long enough to establish independent stationary layer history.
    game.step(2)
    game.configure(Clip=0,Time=0,Weight=.8,TransitionMode=1,BlendTicks=60,WeightBlendTicks=30);game.step(15)
    elapsed=.25;weight=.9;base_x=48/60
    layer=(correction(2,0,elapsed,1),1+correction(2,0,elapsed,1),0)
    game.position(((1-weight)*base_x+weight*layer[0],(1-weight)+weight*layer[1],0))
    require(game.layer()['playback']['transition']['mode']=='inertial','Managed command did not start native layer inertia')
    game.configure(8,Clip=1,Time=0,Playing=0,TransitionMode=1,BlendTicks=60);game.step(1)
    require(game.animation()['transition']['mode']=='inertial','Layer marker failed inherited base inertia')
    if reloads:
        upgrade,failed=reloads;before=game.animation();pose=game.entity()['local_transform']
        game.load(upgrade);require(game.animation()==before and game.entity()['local_transform']==pose,'Compatible reload changed native clocks/history')
        require(game.values()['ReloadMarker']==17,'Compatible added field default differs')
        stable=game.snapshot();game.load(failed,error=-32060);require(game.snapshot()==stable,'Failed constructor reload changed layered state')
        game.step(1);require(game.values()['ReloadMarker']>17,'Reloaded compiled Tick did not execute')
    else:
        stable=game.snapshot();sentinel=stamp.read_bytes();game.load(artifact,error=-32060)
        require(game.snapshot()==stable and stamp.read_bytes()==sentinel,'Unsupported Native AOT replacement executed game or changed state')
    record['checks']['independent_layer_inertia_base_inertia_and_'+('reload' if reloads else 'native_replacement_rejection')]=True

    # Caller and compiled writes share target keys and a single64-command bound.
    for mode,slot,caller in [(1,1,dict(layer=2,weight=0)),(8,1,dict(layer=1,weight=0))]:
        game.configure(mode,Slot=slot,Clip=0,Time=0,Playing=0,TransitionMode=0,BlendTicks=0,Weight=0,WeightBlendTicks=0)
        game.step(1,[command(game.rig,clip=0,playing=False,loop=False,**caller)])
    game.configure(9,Slot=1,Clip=1,Time=.25,Playing=0,Weight=.25);game.step(1)
    require(game.animation()['clip']==1 and game.layer()['playback']['clip']==1,'Compiled base plus layer did not coexist')
    game.configure(10,Clip=0,Time=.75,Weight=.5);game.step(1)
    require(all(game.layer(slot)['playback']['clip']==0 and game.layer(slot)['weight']==.5 for slot in (1,2)),
            'Distinct compiled layer targets did not coexist')
    for mode,caller in [(1,dict(layer=1,weight=0)),(8,{})]:
        game.configure(mode,Slot=1);before=game.snapshot()
        game.step(1,[command(game.rig,clip=0,playing=False,loop=False,**caller)],error=-32040)
        require(game.snapshot()==before,'Same caller/compiled target conflict was not atomic')
    for mode,fields in [(2,{}),(4,{}),(1,dict(Slot=0)),(1,dict(Slot=4)),(1,dict(Weight=-.1)),
            (1,dict(Weight=1.1)),(1,dict(TransitionMode=2)),(1,dict(WeightBlendTicks=3601)),(1,dict(Clip=99))]:
        values=dict(Slot=1,QuerySlot=1,Weight=0,WeightBlendTicks=0,Clip=0,TransitionMode=0)
        values.update(fields);game.configure(mode,**values)
        before=game.snapshot();game.step(1,error=-32040);require(game.snapshot()==before,'Invalid compiled layer command changed state')
    game.configure(3,QuerySlot=0);before=game.snapshot();game.step(1,error=-32040)
    require(game.snapshot()==before,'Invalid compiled getter slot changed state')
    game.configure(0,QuerySlot=1)
    game.configure(5,Count=64,Slot=1,Clip=0,Time=0,Weight=0,TransitionMode=0,WeightBlendTicks=0,BlendTicks=0);game.step(1)
    require(all(game.layer_for(uid(1000+i),1)['playback']['clip']==0 for i in range(64)) and
            game.layer_for(uid(1064),1)['playback']['clip']==1,'Exact64 compiled targets or untouched65th differ')
    game.configure(5,Count=65);before=game.snapshot();game.step(1,error=-32040);require(game.snapshot()==before,'65 compiled writes changed state')
    caller=command(game.rig,clip=0,playing=False,layer=2,weight=0)
    game.configure(5,Count=63);game.step(1,[caller]);game.configure(5,Count=64)
    before=game.snapshot();game.step(1,[caller],error=-32040);require(game.snapshot()==before,'Separate caller allowance bypassed combined64 budget')
    record['checks']['distinct_and_same_target_conflicts_invalid_commands_and_shared_64_limit']=True

    for name in ('invalid-sample','later-throw'):
        game.start();game.load(artifact);game.configure(8,Clip=0,Playing=1,Loop=0);game.step(30)
        game.configure(Clip=1,Playing=0,Weight=.8,TransitionMode=1,BlendTicks=120,WeightBlendTicks=90);game.step(10)
        game.configure(7,Clip=2 if name=='invalid-sample' else 0,Time=0,Playing=1,
            TransitionMode=0,BlendTicks=0,Weight=.75,WeightBlendTicks=30,ThrowAt=str(game.tick+2) if name=='later-throw' else '-1')
        before=game.snapshot();saved=save(game,name+'-before');game.step(60 if name=='invalid-sample' else 5,error=-32040)
        require(game.snapshot()==before,'Late failed batch changed selected native/gameplay state')
        after=save(game,name+'-after');require((after['sha256'],after['bytes'])==(saved['sha256'],saved['bytes']),'Late failed batch changed complete serialized layer/history/physics/audio state')
        game.configure(0,ThrowAt='-1');elapsed=game.layer()['playback']['transition']['elapsed_ticks'];game.step(1)
        require(game.layer()['playback']['transition']['elapsed_ticks']==elapsed+1,'Rollback lost prior layer correction')
        require(game.entity(uid(2))['kinematic_target'] is None and game.rpc('runtime.audio.voices',dict(session_id=game.session,tick=game.tick))['next_voice']==1,'Rollback retained motion/voice allocation')
    game.configure(7,Clip=1,Playing=0,TransitionMode=0,BlendTicks=0,Weight=.5,WeightBlendTicks=0);game.step(3)
    require(game.entity(uid(2))['kinematic_target'] is not None and game.rpc('runtime.audio.voices',dict(session_id=game.session,tick=game.tick))['next_voice']==2,'Same compiled services failed to commit after rollback')
    record['checks']['late_sample_and_managed_failures_restore_full_save_with_motion_audio_and_gameplay']=True


def main(native_aot=False):
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('binary','output','legacy-binary'):parser.add_argument('--'+key,type=Path,required=True)
    if native_aot:parser.add_argument('--descriptor',type=Path,required=True)
    else:
        for key in ('dotnet','bridge','hostfxr','legacy-bridge'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--timeout',type=float,default=600)
    args=parser.parse_args();require(60<=args.timeout<=1200,'Timeout must be60..1200seconds')
    output=args.output.resolve();require(not output.exists(),'Output must be new; preserve earlier evidence');output.mkdir(parents=True)
    inputs={name:getattr(args,name).resolve() for name in ('binary','legacy_binary')}
    inputs.update(fixture=FIXTURE,runner=Path(__file__).resolve(),project=FIXTURE.parent/'Poima.LayerGameplay.csproj')
    if native_aot:inputs['descriptor']=args.descriptor.resolve()
    else:
        inputs.update(bridge=args.bridge.resolve(),hostfxr=args.hostfxr.resolve(),sdk=args.bridge.resolve().parent/'Poima.Gameplay.dll',
                      legacy_bridge=args.legacy_bridge.resolve(),legacy_sdk=args.legacy_bridge.resolve().parent/'Poima.Gameplay.dll')
    require(all(p.is_file() for p in inputs.values()),'A required input is missing')
    require(digest(inputs['binary'])!=digest(inputs['legacy_binary']),'Supply distinct real current and historical engine images')
    record=dict(passed=False,calls=[],builds=[],processes=[],checks={},hashes={k:digest(p) for k,p in inputs.items()},
        limits=['Bounded original compiled fixture, not character quality or game-performance qualification.',
                'No GUI, GPU, physical-input, clean-machine, console or browser qualification.',
                'Sentinels prove no game constructor/Initialize/Tick for supplied rejected cohorts; arbitrary module/assembly/DLL initialization lies outside that guarantee.',
                'Old176/192 compiled binary and shipping baseline qualification is a separate preserved-artifact cohort.',
                'Native AOT libraries are pinned until process exit; replacement rejection is qualified, compatible native reload is not.',
                'Request timeout excludes potentially longer bounded owner cleanup; no automatic reconnect or retry.'])
    deadline=time.monotonic()+args.timeout;clients=[];payloads={}
    old_sentinel=os.environ.get('POIMA_LAYER_SENTINEL');old_wslenv=os.environ.get('WSLENV')
    if args.windows_interop and os.name!='nt':
        entries=[x for x in (old_wslenv or '').split(':') if x and x.split('/',1)[0]!='POIMA_LAYER_SENTINEL']
        os.environ['WSLENV']=':'.join(entries+['POIMA_LAYER_SENTINEL'])
    def native(path):
        if args.windows_interop and os.name!='nt':
            return subprocess.check_output(['wslpath','-w',str(Path(path).resolve())],text=True,timeout=10).strip()
        return str(Path(path).resolve())
    def open_game(label,binary,world,bridge=None):
        require(time.monotonic()<deadline,'Deadline exceeded before spawning owned host')
        stamp=output/(label+'.sentinel');require(not stamp.exists(),'Sentinel already exists')
        os.environ['POIMA_LAYER_SENTINEL']=native(stamp)
        client=WorldClient.open(str(binary.resolve()),native(world),close_timeout=10);clients.append((label,client))
        return LayerGame(client,record,label,native,bridge,args.hostfxr if not native_aot else None,deadline,native_aot=native_aot),stamp
    def copy_world(source,target):
        shutil.copyfile(source,target);shutil.copytree(Path(str(source)+'.assets'),Path(str(target)+'.assets'))
    def build(name,text):
        directory=output/name;directory.mkdir();source=directory/'Game.cs';source.write_text(text,encoding='utf-8')
        project=directory/'Poima.LayerVerification.csproj'
        hint=native(inputs['sdk']) if args.dotnet.suffix.lower()=='.exe' else str(inputs['sdk'])
        project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic></PropertyGroup><ItemGroup><Reference Include="Poima.Gameplay"><HintPath>'+escape(hint)+'</HintPath></Reference></ItemGroup></Project>')
        remaining=deadline-time.monotonic();require(remaining>0,'Build deadline exceeded')
        command_line=[str(args.dotnet.resolve()),'build',native(project) if args.dotnet.suffix.lower()=='.exe' else str(project),'-c','Release','--nologo','--disable-build-servers']
        proc=subprocess.run(command_line,capture_output=True,text=True,timeout=min(120,remaining),env=dict(os.environ,DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_GENERATE_ASPNET_CERTIFICATE='false'))
        row=dict(name=name,exit_code=proc.returncode,source_sha256=digest(source),sdk_sha256=digest(inputs['sdk']),log=proc.stdout+proc.stderr);record['builds'].append(row)
        require(proc.returncode==0,'Compiled layer fixture build failed:'+name)
        image=directory/'bin/Release/net10.0/Poima.LayerVerification.dll';row['assembly_sha256']=digest(image);return image
    saves=output/'saves';saves.mkdir()
    def configure_saves(game):
        return game.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))['generation']
    def save(game,slot):
        result=game.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=0,
            session_id=game.session,expected_tick=game.tick,expected_gameplay_revision=game.revision))
        files=list((saves/('slot-'+slot)).glob('p-*.bin'));require(len(files)==1,'Save slot payload count differs')
        payload=files[0];require(payload.stat().st_size==result['bytes'] and digest(payload)==result['sha256'],'Reported full save bytes/hash differ from actual payload')
        stored=json.loads(payload.read_bytes());require(stored['snapshot']['payload']['animation']['version']==3,'Layered compiled game did not retain nested v3')
        require('requirements' not in stored['snapshot']['payload']['gameplay']['schema'],'Requirements leaked into saved state schema')
        return result
    try:
        if native_aot:
            artifact=inputs['descriptor'];spec=json.loads(artifact.read_text())
            require(spec['type']==TYPE and spec['format']=='poima.native-gameplay' and spec['version']==2 and spec['minimum_services_bytes']==208,'Supply actual208-byte LayerGame artifact')
            require(set(spec['required_features'])=={'baseline_v7','animation_inertial_v1','animation_layers_v1'},'Marked artifact feature declaration differs')
            for row in spec['files']:
                path=artifact.parent/row['path'];require(not path.is_symlink() and path.resolve().is_relative_to(artifact.parent),'Artifact path escapes')
                require(path.is_file() and path.stat().st_size==row['size'] and digest(path)==row['sha256'],'Artifact payload differs')
                payloads[row['path']]=dict(path=path,sha256=row['sha256'])
            record['artifact_payload_sha256']={k:v['sha256'] for k,v in payloads.items()};reloads=None
        else:
            text=FIXTURE.read_text();artifact=build('base',text)
            upgraded=text.replace('public int Mode,','public int ReloadMarker; public int Mode,').replace('state.ThrowAt=-1;','state.ThrowAt=-1;state.ReloadMarker=17;').replace('++state.Ticks;','++state.Ticks;++state.ReloadMarker;')
            upgrade=build('upgrade',upgraded);failed=build('failed-reload',upgraded.replace('Stamp("constructor");','Stamp("constructor");throw new InvalidOperationException("Intentional layer constructor failure.");'))
            reloads=(upgrade,failed)
        world=output/'world.json';game,stamp=open_game('marked',inputs['binary'],world,None if native_aot else inputs['bridge'])
        doc,blob=blend_fixture();model=output/'original.glb';model.write_bytes(glb(doc,blob));asset=game.rpc('asset.import',dict(source=native(model)))['asset'];model.unlink()
        wav=output/'original.wav'
        with wave.open(str(wav),'wb') as stream:
            stream.setparams((1,2,48000,4800,'NONE','not compressed'));stream.writeframes(struct.pack('<'+'h'*4800,*([1000,-1000]*2400)))
        sound=game.rpc('asset.audio.import',dict(source=native(wav)))['asset'];wav.unlink()
        ops=[]
        for i in range(65):
            rig=uid(1000+i);ops.extend([dict(op='asset.instantiate',id=rig,name='Layer rig '+str(i),asset=asset),
                component(rig,'AnimationRig',dict(asset=asset,clip=0,time=0,speed=1,loop=False,playing=True,
                    layers=[authored(1,'override',1,0),authored(2,'additive',1,0,reference_clip=0,reference_time=.5)]))])
        for identity,position,motion,half in [(1,(0,-.5,0),'static',(20,.5,20)),(2,(5,1,0),'kinematic',(.5,.5,.5)),(4,(5,30,0),'dynamic',(.25,.25,.25))]:
            ops.extend([dict(op='entity.create',id=uid(identity),name='Body '+str(identity)),component(uid(identity),'Transform',transform(position)),
                component(uid(identity),'BoxCollider',dict(half_extents=list(half),motion=motion,mass=1,friction=.5,restitution=0))])
        ops.extend([dict(op='entity.create',id=uid(3),name='Plain'),dict(op='entity.create',id=uid(20),name='Sound'),
            component(uid(20),'AudioEmitter',dict(asset=sound,gain=1,loop=True,enabled=True))])
        game.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=ops));authored_bytes=world.read_bytes()
        require(configure_saves(game)==1,'Fresh save configuration differs')
        exercise(game,artifact,save,record,stamp,reloads)

        game.start();game.load(artifact);game.configure(8,Clip=0,Playing=1,Loop=0);game.step(30)
        game.configure(Clip=1,Playing=0,Weight=1);game.step(3)
        game.configure(Clip=0,Time=0,TransitionMode=1,BlendTicks=120,Weight=.3,WeightBlendTicks=90);game.step(17)
        game.configure(8,Clip=1,Time=.25,Speed=.5,Playing=1,TransitionMode=1,BlendTicks=120);game.step(7)
        require(game.animation()['transition']['mode']=='inertial' and
                game.layer()['playback']['transition']['mode']=='inertial' and game.layer()['weight_transition'] is not None,
                'Checkpoint must retain active independent base/layer corrections and weight ramp')
        checkpoint=without_session(game.snapshot());saved=save(game,'layer-checkpoint')
        freshworld=output/'fresh-world.json';copy_world(world,freshworld)
        fresh,freshstamp=open_game('fresh-restore',inputs['binary'],freshworld,None if native_aot else inputs['bridge']);generation=configure_saves(fresh)
        config=dict(descriptor=native(artifact),expected_descriptor_sha256=digest(artifact)) if native_aot else dict(hostfxr=native(args.hostfxr),bridge=native(inputs['bridge']),assembly=native(artifact),type=TYPE)
        session=uuid.uuid4().hex
        loaded=fresh.rpc('save.load',dict(request_id=uuid.uuid4().hex,configuration_generation=generation,slot='layer-checkpoint',expected_generation=1,
            revision=1,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=session,gameplay=config))
        fresh.session,fresh.tick=session,loaded['tick'];fresh.revision=fresh.inspect()['revision'];fresh.serial=int(fresh.values()['Serial'])
        require(freshstamp.read_text().splitlines()==['constructor'],'Stopped restore ran Initialize/Tick')
        require(without_session(fresh.snapshot())==checkpoint,'Fresh process selected complete state differs')
        repeated=save(fresh,'fresh-restored');require((repeated['sha256'],repeated['bytes'])==(saved['sha256'],saved['bytes']),'Fresh process changed complete saved layer/history bytes')
        for value in (game,fresh):value.configure(Clip=1,TransitionMode=1,BlendTicks=60,Weight=.7,WeightBlendTicks=30);value.step(1)
        require(without_session(game.snapshot())==without_session(fresh.snapshot()),'Immediate compiled re-interruption lost restored independent history')
        game.step(19);fresh.step(3);fresh.step(16)
        require(without_session(game.snapshot())==without_session(fresh.snapshot()),'Fresh continuation depends on tick partition')
        game.step(120);fresh.step(120);require(without_session(game.snapshot())==without_session(fresh.snapshot()),'Restored clocks diverged after completion')
        require(game.animation()['transition'] is None and game.layer()['playback']['transition'] is None and
                game.layer()['weight_transition'] is None,'Base/layer did not finish clip/weight transitions')
        record['checks']['fresh_complete_payload_restore_and_immediate_compiled_reinterruption']=True

        cohorts=[]
        if native_aot:
            copyroot=output/'omitted-layer-artifact';copyroot.mkdir()
            for name,row in payloads.items():
                target=copyroot/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(row['path'],target)
            omitted=copyroot/artifact.name
            omitted.write_text(json.dumps(dict(spec,minimum_services_bytes=192,required_features=['baseline_v7','animation_inertial_v1']),indent=2)+'\n')
            cohorts=[('descriptor-omission',inputs['binary'],None,omitted),('old-host-marked',inputs['legacy_binary'],None,artifact)]
        else:
            cohorts=[('matched-old-bridge-host-marked',inputs['legacy_binary'],inputs['legacy_bridge'],artifact),
                     ('new-bridge-old-host-marked',inputs['legacy_binary'],inputs['bridge'],artifact)]
        for label,binary,bridge,selected in cohorts:
            ownedworld=output/(label+'.world.json');copy_world(world,ownedworld)
            cohort,cohortstamp=open_game(label,binary,ownedworld,bridge);cohort.start();before=cohort.snapshot()
            failure=cohort.load(selected,error=-32060)
            require(not cohortstamp.exists(),'Rejected cohort ran constructor/Initialize/Tick')
            require(cohort.snapshot()==before,'Rejected marked module changed native state')
            counters=cohort.rpc('runtime.gameplay.collect',dict(session_id=cohort.session))
            require(counters['active_modules']==0 and counters['retired_alive']==0,'Rejected game leaked a handle')
            if native_aot and label=='descriptor-omission':
                require('native' in counters and counters['native']['active_modules']==0,'Omitted declaration did not reach compiled native validation cleanly')
                require('animation_layers_v1' in failure['message'] or '208' in failure['message'],'Omitted declaration failed for unrelated reason')
            if native_aot and label=='old-host-marked':require('native' not in counters,'Old host initialized unsupported native library')
            record['checks'][label.replace('-','_')+'_rejects_before_game_callbacks']=True
        require(world.read_bytes()==authored_bytes,'Original authored world changed')
        require(all(digest(path)==record['hashes'][name] for name,path in inputs.items()),'Preserved input/source changed')
        require(all(digest(row['path'])==row['sha256'] for row in payloads.values()),'Original artifact payload changed')
        record['checks']['authored_fixture_and_original_input_artifacts_immutable']=True;record['passed']=True
    except BaseException as failure:
        record['failure']=dict(type=type(failure).__name__,message=str(failure));raise
    finally:
        for label,client in reversed(clients):
            error=None
            try:client.close()
            except BaseException as failure:error=str(failure)
            transport=client.transport
            record['processes'].append(dict(host=label,pid=transport.process_id,exit_code=transport.returncode,
                closed=client.closed,cleanup_error=error,stderr=transport.stderr_tail))
            if error or transport.returncode!=0 or not client.closed:record['passed']=False
        for name,old in [('POIMA_LAYER_SENTINEL',old_sentinel),('WSLENV',old_wslenv)]:
            if old is None:os.environ.pop(name,None)
            else:os.environ[name]=old
        path=output/'evidence.json';path.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(path)
    require(record['passed'],'Owned process cleanup failed')
    print(json.dumps(dict(passed=True,checks=record['checks'],rpc_calls=len(record['calls']),clean_owned_processes=len(record['processes']))))


if __name__=='__main__':main()
