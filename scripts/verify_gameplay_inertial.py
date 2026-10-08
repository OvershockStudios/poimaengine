#!/usr/bin/env python3
"""Compile and verify negotiated C# inertial animation on explicitly supplied hosts.

This owns disposable worlds and processes. It never repairs a failed game, retries
an uncertain mutation, or modifies the preserved legacy engine/bridge cohort.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError
from runtime_animation_blend_contract import blend_fixture, command, component, node, transform, uid
from runtime_animation_inertial_contract import correction
from gltf_fixture import glb
from verify_gameplay_animation import PROBE as LEGACY_SOURCE

FIXTURE = ROOT / 'tests/fixtures/gameplay_inertial/ProbeGame.cs'
TYPE = 'Poima.Verification.ProbeGame'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def without_session(value):
    if isinstance(value, dict):
        return {key: without_session(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [without_session(item) for item in value]
    return value


class Game:
    def __init__(self, client, record, label, native, bridge, hostfxr, deadline):
        self.client, self.record, self.label = client, record, label
        self.native, self.bridge, self.hostfxr, self.deadline = native, bridge, hostfxr, deadline
        self.session, self.tick, self.revision, self.serial = None, 0, 0, 0
        self.rig, self.tip = uid(1000), node(uid(1000), 1)

    def rpc(self, method, params=None, error=None):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, 'Qualification deadline exceeded')
        row = dict(host=self.label, method=method, params=params or {})
        self.record['calls'].append(row)
        try:
            result = self.client.call(method, params or {}, timeout=min(30, remaining))
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message, data=failure.data)
            require(error is not None and failure.code == error,
                    '{}: unexpected error {}: {}'.format(method, failure.code, failure.message))
            return row['error']
        row['result'] = result
        require(error is None, '{} unexpectedly succeeded'.format(method))
        return result

    def start(self):
        if self.session:
            self.rpc('runtime.stop', dict(session_id=self.session))
        self.session, self.tick, self.revision, self.serial = uuid.uuid4().hex, 0, 0, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=1))

    def inspect(self):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))

    def values(self):
        return self.inspect()['module']['values']

    def load(self, assembly, error=None):
        result = self.rpc('runtime.gameplay.load', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=self.revision, hostfxr=self.native(self.hostfxr),
            bridge=self.native(self.bridge), assembly=self.native(assembly), type=TYPE), error)
        if error is None:
            self.revision += 1
            require(result['revision'] == self.revision, 'Gameplay load revision differs')
        return result

    def edit(self, **fields):
        result = self.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=self.revision, values=fields))
        self.revision += 1
        require(result['revision'] == self.revision, 'Gameplay edit revision differs')

    def configure(self, mode=1, **fields):
        self.serial += 1
        self.edit(Mode=mode, Serial=self.serial, **fields)

    def entity(self, identity=None):
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=identity or self.tip))

    def animation(self, identity=None):
        return self.entity(identity or self.rig)['animation']

    def position(self, expected):
        actual = self.entity()['local_transform']['position']
        require(all(abs(a-b) < 1e-8 for a, b in zip(actual, expected)), 'Independent pose reference differs: {} != {}'.format(actual, expected))

    def snapshot(self):
        identities = [uid(1000+i) for i in range(65)] + [self.tip] + [uid(i) for i in (1, 2, 3, 4, 20)]
        rows = []
        for first in range(0, len(identities), 32):
            result = self.rpc('runtime.observe', dict(session_id=self.session, tick=self.tick,
                ids=identities[first:first+32], entity_fields=['local_transform', 'world_matrix', 'velocity', 'animation', 'kinematic_target']))
            rows.extend(result['entities'])
        return dict(tick=self.tick, entities=sorted(rows, key=lambda row: row['id']), gameplay=self.inspect(),
            voices=self.rpc('runtime.audio.voices', dict(session_id=self.session, tick=self.tick)))

    def query_matches(self, expected, observed, tick):
        require(int(observed['QueryTick']) == tick, 'Committed query tick differs')
        transition = expected['transition']
        mapping = dict(QClip=-1 if expected['clip'] is None else expected['clip'], QTime=expected['time'],
            QSpeed=expected['speed'], QLoop=int(expected['loop']), QPlaying=int(expected['playing']),
            QDuration=expected['duration'], QHasTransition=int(transition is not None), QExtendedMatches=1,
            QActiveMode=-1 if transition is None else int(transition.get('mode') == 'inertial'),
            QProgress=-1 if transition is None else transition['weight'])
        if transition is not None:
            mapping.update(QStartTick=transition['start_tick'], QDurationTicks=transition['duration_ticks'],
                QElapsedTicks=transition['elapsed_ticks'], QWeight=transition['weight'], QSourceFrozen=int(transition['source_frozen']))
        for key, value in mapping.items():
            require(abs(float(observed[key])-value) < 1e-10, 'Extended committed query differs for {}'.format(key))

    def step(self, count, animations=(), error=None, marked=True):
        before = self.animation() if error is None and count == 1 and not animations and marked else None
        result = self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, ticks=count, animations=list(animations)), error)
        if error is None:
            old = self.tick
            self.tick += count
            require(result['tick'] == self.tick, 'Runtime step tick differs')
            if before is not None:
                self.query_matches(before, self.values(), old)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'dotnet', 'hostfxr', 'bridge', 'output', 'legacy-binary', 'legacy-bridge'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--legacy-assembly', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    require(60 <= args.timeout <= 1200, 'Timeout must be 60..1200 seconds')
    args.output = args.output.resolve()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    deadline = time.monotonic()+args.timeout
    record = dict(passed=False, calls=[], builds=[], processes=[], checks={}, limits=[
        'CoreCLR qualification only; NativeAOT, installed GUI and physical input are separate cohorts.',
        'Measured outgoing secant motion is qualified; this does not promise foot locking, seam continuity or overshoot prevention.',
        'Legacy rejection covers the supplied real old engine/bridge/SDK cohort and game constructor/Initialize/Tick sentinels; arbitrary assembly initialization is outside that guarantee.',
        'Managed SDK checks do not qualify deliberately forged raw native ABI prefixes or headers.'])
    inputs = dict(binary=args.binary, bridge=args.bridge, sdk=args.bridge.parent/'Poima.Gameplay.dll',
        legacy_binary=args.legacy_binary, legacy_bridge=args.legacy_bridge,
        legacy_sdk=args.legacy_bridge.parent/'Poima.Gameplay.dll', fixture=FIXTURE)
    record['hashes'] = {name:digest(path) for name,path in inputs.items()}
    clients = []
    previous_sentinel = os.environ.get('POIMA_INERTIAL_SENTINEL')
    previous_wslenv = os.environ.get('WSLENV')
    if args.windows_interop and os.name != 'nt':
        # The sentinel is already a Windows path, so /p would corrupt it.
        # Preserve every unrelated entry and its flags; restore the complete
        # original environment below, including any task-variable entry.
        entries = [entry for entry in (previous_wslenv or '').split(':')
                   if entry and entry.split('/', 1)[0] != 'POIMA_INERTIAL_SENTINEL']
        os.environ['WSLENV'] = ':'.join(entries + ['POIMA_INERTIAL_SENTINEL'])

    def native(path, windows=None):
        windows = args.windows_interop if windows is None else windows
        if windows and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', str(Path(path).resolve())], text=True, timeout=10).strip()
        return str(Path(path).resolve())

    def build(name, source, sdk):
        directory = run/name
        directory.mkdir()
        path = directory/'Game.cs'
        path.write_text(source, encoding='utf-8')
        project = directory/'Poima.InertialVerification.csproj'
        project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework>'
            '<LangVersion>14.0</LangVersion><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>'
            '<TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic></PropertyGroup>'
            '<ItemGroup><Reference Include="Poima.Gameplay"><HintPath>'+escape(native(sdk,args.dotnet.suffix.lower()=='.exe'))+
            '</HintPath></Reference></ItemGroup></Project>', encoding='utf-8')
        remaining=deadline-time.monotonic();require(remaining>0,'Build deadline exceeded')
        result=subprocess.run([str(args.dotnet.resolve()),'build',native(project,args.dotnet.suffix.lower()=='.exe'),
            '-c','Release','--nologo','--disable-build-servers'],capture_output=True,text=True,timeout=min(120,remaining),
            env=dict(os.environ,DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_GENERATE_ASPNET_CERTIFICATE='false'))
        item=dict(name=name,source_sha256=digest(path),sdk_sha256=digest(sdk),exit_code=result.returncode,log=result.stdout+result.stderr)
        record['builds'].append(item);require(result.returncode==0,'C# fixture compilation failed: '+name)
        assembly=directory/'bin/Release/net10.0/Poima.InertialVerification.dll'
        item['assembly_sha256']=digest(assembly)
        return assembly

    def open_game(label, binary, bridge, world, sentinel=None):
        if sentinel is None:
            sentinel=run/(label+'.sentinel')
        require(not sentinel.exists(),'Sentinel path already exists')
        os.environ['POIMA_INERTIAL_SENTINEL']=native(sentinel)
        client=WorldClient.open(str(binary.resolve()),native(world),close_timeout=10)
        clients.append((label,client))
        return Game(client,record,label,native,bridge,args.hostfxr,deadline),sentinel

    def copy_world(source,destination):
        shutil.copyfile(source,destination)
        assets=Path(str(source)+'.assets')
        if assets.exists():shutil.copytree(assets,Path(str(destination)+'.assets'))

    try:
        text=FIXTURE.read_text(encoding='utf-8')
        base=build('base',text,inputs['sdk'])
        upgraded=text.replace('public int Mode,','public int ReloadMarker; public int Mode,').replace(
            'state.ThrowAt = -1;','state.ThrowAt = -1; state.ReloadMarker = 17;').replace('++state.Ticks;','++state.Ticks; ++state.ReloadMarker;')
        upgrade=build('upgrade',upgraded,inputs['sdk'])
        failure=build('failed-reload',upgraded.replace('Stamp("constructor");','Stamp("constructor"); throw new InvalidOperationException("Intentional constructor failure.");'),inputs['sdk'])
        legacy=args.legacy_assembly or build('legacy',LEGACY_SOURCE,inputs['legacy_sdk'])
        record['legacy_assembly_sha256']=digest(legacy)
        record['legacy_assembly_provenance']='preserved-input' if args.legacy_assembly else 'compiled-against-preserved-sdk'
        world=run/'world.json'
        game,sentinel=open_game('new-host-marked',args.binary,args.bridge,world)
        doc,blob=blend_fixture(); model=run/'original.glb';model.write_bytes(glb(doc,blob))
        asset=game.rpc('asset.import',dict(source=native(model)))['asset'];model.unlink()
        audio=run/'original.wav'
        with wave.open(str(audio),'wb') as stream:
            stream.setparams((1,2,48000,4800,'NONE','not compressed'));stream.writeframes(struct.pack('<'+'h'*4800,*([1000,-1000]*2400)))
        sound=game.rpc('asset.audio.import',dict(source=native(audio)))['asset'];audio.unlink()
        ops=[dict(op='asset.instantiate',id=uid(1000+i),name='Rig '+str(i),asset=asset) for i in range(65)]
        for identity,position,motion,half in [(1,(0,-.5,0),'static',(20,.5,20)),(2,(5,1,0),'kinematic',(.5,.5,.5)),(4,(5,30,0),'dynamic',(.25,.25,.25))]:
            ops += [dict(op='entity.create',id=uid(identity),name='Body '+str(identity)),component(uid(identity),'Transform',transform(position)),
                component(uid(identity),'BoxCollider',dict(half_extents=list(half),motion=motion,mass=1,friction=.5,restitution=0))]
        ops += [dict(op='entity.create',id=uid(3),name='Plain'),dict(op='entity.create',id=uid(20),name='Sound'),
            component(uid(20),'AudioEmitter',dict(asset=sound,gain=1,loop=True,enabled=True))]
        game.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=ops));authored=world.read_bytes()

        game.start();game.load(base)
        game.configure(3);game.step(1);require(game.values()['NullProbe']==1,'Extended non-rig query did not return null')
        require(sentinel.exists() and {'constructor','initialize','tick'} <= set(sentinel.read_text().splitlines()),
                'Positive sentinel mechanism did not reach the native gameplay process')
        record['checks']['marked_constructor_initialize_tick_sentinel_observed']=True
        game.configure(Clip=0,Time=.5,Speed=1,Loop=0,Playing=1,BlendTicks=0);game.step(30)
        game.position((1,1,0))
        game.configure(Clip=1,Time=.25,Speed=2,BlendTicks=60);game.step(15)
        def original(t):return (1.75-2*t+correction(-.75,3,t,1),3+correction(-2,0,t,1),0)
        game.position(original(.25));game.step(15);game.position(original(.5))
        current,previous=original(.5),original(29/60);velocity=[(a-b)*60 for a,b in zip(current,previous)]
        game.configure(Clip=0,Time=1.5,Playing=0,BlendTicks=30);game.step(15)
        def interrupted(t):return (1.5+correction(current[0]-1.5,velocity[0],t,.5),1+correction(current[1]-1,velocity[1],t,.5),0)
        game.position(interrupted(.25));require(game.animation()['transition']['mode']=='inertial','Callback did not commit actual inertial mode')
        before=[game.entity(),game.animation()];fields=game.values();game.load(upgrade)
        require([game.entity(),game.animation()]==before,'Compatible reload changed active native correction')
        require(all(game.values()[k]==v for k,v in fields.items()),'Compatible reload changed retained fields')
        require(game.values()['ReloadMarker']==17,'New reload field default not initialized')
        require(game.rpc('runtime.gameplay.collect',dict(session_id=game.session))['retired_alive']==0,'Compatible reload retained old module')
        before=game.snapshot();game.load(failure,error=-32060);require(game.snapshot()==before,'Failed reload changed committed native/game state')
        require(game.rpc('runtime.gameplay.collect',dict(session_id=game.session))['retired_alive']==0,'Failed reload leaked retired module')
        game.step(1);game.position(interrupted(16/60));require(game.values()['ReloadMarker']==18,'Replacement compiled code did not run')
        require(game.values()['StagingChecks']>0,'Real callback did not check staging')
        record['checks']['compiled_inertial_analytic_pose_extended_committed_staging_and_reload']=True

        game.start();game.load(base)
        cases=[(2,{}),(1,dict(Clip=999)),(1,dict(Time=-1)),(1,dict(Speed=9)),(1,dict(BlendTicks=3601)),(1,dict(InertialMode=2)),(1,dict(Rig=uid(3)))]
        for mode,update in cases:
            fields=dict(Clip=0,Time=0,Speed=1,BlendTicks=30,InertialMode=1,Rig=game.rig);fields.update(update)
            game.configure(mode,**fields);before=game.snapshot();game.step(3,error=-32040);require(game.snapshot()==before,'Rejected callback changed shared state')
            game.configure(0,Rig=game.rig);game.step(1);require(game.animation()['clip'] is None,'Failed callback left a stale staged command')
        game.configure(Clip=1,Time=0,Speed=1,BlendTicks=30);before=game.snapshot()
        game.step(3,[command(game.rig)],error=-32040);require(game.snapshot()==before,'Native/managed target conflict was not atomic')
        game.configure(0);game.step(1)
        game.configure(5,Count=64,Clip=1,Time=.5,Playing=0,BlendTicks=0);game.step(1)
        require(all(game.animation(uid(1000+i))['clip']==1 for i in range(64)) and game.animation(uid(1064))['clip'] is None,'Exact 64 staged commands did not commit')
        for count,caller in [(65,[]),(64,[command(uid(1064))])]:
            game.configure(5,Count=count);before=game.snapshot();game.step(1,caller,error=-32040);require(game.snapshot()==before,'Over-limit callback changed state')
        game.configure(5,Count=63,Clip=0,Time=.75)
        game.step(1,[command(uid(1064),time=.25,playing=False)])
        require(game.animation(uid(1062))['clip']==0 and game.animation(uid(1063))['clip']==1 and game.animation(uid(1064))['clip']==0,'Combined exact 64 limit failed')
        record['checks']['duplicate_invalid_mode_target_conflict_and_exact_64_limit']=True

        game.start();game.load(base)
        game.configure(Clip=0,Time=0,Speed=1,Playing=1,BlendTicks=0);game.step(30)
        game.configure(Clip=1,Time=.25,Speed=2,BlendTicks=120);game.step(15)
        for kind in ('invalid_sample','later_throw'):
            game.configure(7,Clip=2 if kind=='invalid_sample' else 1,Time=0,Speed=1,Playing=1,BlendTicks=120,
                ThrowAt=str(game.tick+2) if kind=='later_throw' else '-1')
            before=game.snapshot();game.step(50 if kind=='invalid_sample' else 5,error=-32040)
            require(game.snapshot()==before,'{} did not restore animation/history, physics, sound and managed state'.format(kind))
            old=game.animation()['transition']['elapsed_ticks'];game.configure(0,ThrowAt='-1');game.step(1)
            require(game.animation()['transition']['elapsed_ticks']==old+1,'Rollback lost prior active inertial transition')
            require(game.entity(uid(2))['kinematic_target'] is None,'Rollback retained queued motion')
            require(game.rpc('runtime.audio.voices',dict(session_id=game.session,tick=game.tick))['next_voice']==1,'Rollback consumed a voice handle')
        game.configure(7,Clip=1,Time=.5,Playing=0,BlendTicks=30);game.step(3)
        require(game.entity(uid(2))['kinematic_target'] is not None,'Same services could not commit after rollback')
        require(game.rpc('runtime.audio.voices',dict(session_id=game.session,tick=game.tick))['next_voice']==2,'Sound services could not commit after rollback')
        record['checks']['sample_and_managed_failure_restore_active_history_physics_audio_and_gameplay']=True

        game.start();game.load(base)
        game.configure(Clip=0,Time=0,Speed=1,Playing=1,Loop=0,BlendTicks=0);game.step(30)
        game.configure(Clip=1,Time=.25,Speed=2,BlendTicks=120);game.step(17)
        saves=run/'saves';saves.mkdir();configuration=game.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))['generation']
        checkpoint=without_session(game.snapshot())
        game.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=configuration,slot='inertial',expected_generation=0,
            session_id=game.session,expected_tick=game.tick,expected_gameplay_revision=game.revision))
        fresh_world=run/'fresh-world.json';copy_world(world,fresh_world)
        fresh,_=open_game('fresh-restore',args.binary,args.bridge,fresh_world)
        generation=fresh.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))['generation']
        session=uuid.uuid4().hex
        loaded=fresh.rpc('save.load',dict(request_id=uuid.uuid4().hex,configuration_generation=generation,slot='inertial',expected_generation=1,
            revision=1,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=session,
            gameplay=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(base),type=TYPE)))
        fresh.session,fresh.tick=session,loaded['tick'];fresh.revision=fresh.inspect()['revision'];fresh.serial=int(fresh.values()['Serial'])
        require(without_session(fresh.snapshot())==checkpoint,'Fresh process did not restore the complete selected animation/physics/game state')
        for value in (game,fresh):value.configure(Clip=0,Time=1.5,Playing=0,BlendTicks=60);value.step(1)
        require(without_session(game.snapshot())==without_session(fresh.snapshot()),'Immediate managed re-interruption lost restored output velocity')
        game.step(19);fresh.step(3);fresh.step(16)
        require(without_session(game.snapshot())==without_session(fresh.snapshot()),'Fresh restored continuation depends on request partition')
        game.step(41);fresh.step(41)
        require(without_session(game.snapshot())==without_session(fresh.snapshot()) and game.animation()['transition'] is None,'Fresh managed correction missed completion')
        record['checks']['fresh_durable_save_exact_selected_state_and_immediate_compiled_reinterruption']=True

        for label,binary,bridge,marked in [('old-compiled-new-host',args.binary,args.bridge,False),
            ('old-cohort-marked',args.legacy_binary,args.legacy_bridge,True),
            ('new-bridge-old-host-unmarked',args.legacy_binary,args.bridge,False),
            ('new-bridge-old-host-marked',args.legacy_binary,args.bridge,True)]:
            compatibility_world=run/(label+'.world.json');copy_world(world,compatibility_world)
            cohort,stamp=open_game(label,binary,bridge,compatibility_world);cohort.start()
            if marked:
                before=cohort.snapshot();cohort.load(base,error=-32060)
                require(not stamp.exists(),'Legacy cohort executed marked game constructor/Initialize/Tick')
                require(cohort.snapshot()==before,'Rejected marked module changed host state')
                collected=cohort.rpc('runtime.gameplay.collect',dict(session_id=cohort.session))
                require(collected['active_modules']==0 and collected['retired_alive']==0,'Rejected marked module leaked a managed handle')
            else:
                cohort.load(legacy);cohort.configure(Clip=0,Time=0,Speed=1,Playing=1,Loop=0,BlendTicks=0)
                cohort.step(1,marked=False);cohort.step(1,marked=False)
                require(cohort.animation()['clip']==0 and cohort.values()['QClip']==0,'Old compiled gameplay baseline calls failed')
                cohort.position((2/60,1,0))
            record['checks'][label.replace('-','_')]=True
        require(world.read_bytes()==authored,'Verification changed original authored world bytes')
        require(all(digest(path)==record['hashes'][name] for name,path in inputs.items()),'Qualification modified preserved input/source binaries')
        record['checks']['authored_bytes_and_preserved_cohort_unchanged']=True
        record['passed']=True
    except BaseException as error:
        record['failure']=dict(type=type(error).__name__,message=str(error))
        raise
    finally:
        for label,client in reversed(clients):
            cleanup=None
            try:client.close()
            except BaseException as error:cleanup=str(error);record['passed']=False
            record['processes'].append(dict(host=label,pid=client.transport.process_id,exit_code=client.transport.returncode,
                closed=client.closed,cleanup_error=cleanup,stderr=client.transport.stderr_tail))
            if client.transport.returncode != 0:record['passed']=False
        if previous_sentinel is None:os.environ.pop('POIMA_INERTIAL_SENTINEL',None)
        else:os.environ['POIMA_INERTIAL_SENTINEL']=previous_sentinel
        if previous_wslenv is None:os.environ.pop('WSLENV',None)
        else:os.environ['WSLENV']=previous_wslenv
        (args.output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print(args.output/'evidence.json')
    require(record['passed'],'Owned process cleanup did not complete successfully')
    print(json.dumps(dict(passed=True,checks=record['checks'],rpc_calls=len(record['calls']))))


if __name__ == '__main__':
    main()
