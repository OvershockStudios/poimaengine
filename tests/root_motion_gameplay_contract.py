#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled root-motion Character controls, collision and pose stripping, rollback and saves.

Consumes a supplied CoreCLR assembly or Native AOT descriptor; builds nothing.
Run beneath an external process-tree owner. No renderer, audio or timing claim.
"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import time

from input_actions_gameplay_contract import OwnedSession, exact, fresh, need
from components_gameplay_contract import sha, uid
from animation_fixture import ribbon
from gltf_fixture import glb
import math
import struct

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedRootMotionGame'
IDENTITY = 'poima.test.managed-root-motion'
CAP = 32*1024*1024
RIG = uid(400)




def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if bool(args.native_descriptor) == bool(args.hostfxr or args.bridge or args.assembly):
        parser.error('Supply either CoreCLR paths or one native descriptor')
    if not args.native_descriptor and not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('CoreCLR requires --hostfxr --bridge --assembly')
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value
    args.native = native
    run = args.output.resolve();run.mkdir(parents=True, exist_ok=False)
    args.deadline = time.monotonic()+180;started = time.monotonic()
    files = {str(Path(__file__).resolve()): Path(__file__).resolve()}
    for path in (ROOT/'tests/input_actions_gameplay_contract.py', ROOT/'tests/character_gameplay_contract.py',
                 ROOT/'tests/components_gameplay_contract.py', ROOT/'tests/animation_fixture.py',
                 ROOT/'tests/gltf_fixture.py', ROOT/'managed/Directory.Build.props',
                 ROOT/'tests/managed_root_motion_gameplay/ManagedRootMotionGame.cs',
                 ROOT/'tests/managed_root_motion_gameplay/Poima.ManagedRootMotionGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 304 and metadata['required_features'] == ['baseline_v7', 'animation_root_motion_v1', 'character_input_v1'],
             'Wrong negotiated native root consumer')
        for row in metadata['files']:
            rel = Path(row['path']);path = (descriptor.parent/rel).resolve(strict=True)
            need(not rel.is_absolute() and '..' not in rel.parts and path.is_relative_to(descriptor.parent) and
                 path.is_file() and path.stat().st_size == row['size'] and sha(path) == row['sha256'], 'Native artifact closure differs')
            files[str(path)] = path
        image = (descriptor.parent/metadata['library']).resolve(strict=True)
        need(str(image) in files, 'Native image absent from artifact closure')
        config = dict(descriptor=native(descriptor), expected_descriptor_sha256=sha(descriptor))
    else:
        image = args.assembly.resolve()
        for owner in (image, args.bridge.resolve()):
            sdk = owner.parent/'Poima.Gameplay.dll';need(sdk.is_file(), 'Consumer/bridge SDK missing');files[str(sdk)] = sdk
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                dependency = owner.with_suffix(suffix)
                if dependency.is_file():
                    files[str(dependency)] = dependency
        config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    for path in (ROOT/'tools/python/poima_client').rglob('*'):
        if path.is_file() and path.suffix in ('.py', '.json'):
            files[str(path.resolve())] = path.resolve()
    pins = {name: sha(path) for name, path in files.items()}
    evidence = dict(passed=False, calls=[], owners=[], checks=[], snapshots=[], source_pins=pins,
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    limitations=['Synthetic compiled Character root-motion controls; no GPU, physical input, audio, damage or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    try:
        world = run/'world.json';process = OwnedSession(args, world, evidence)
        source = run/'analytic-root.glb'
        doc,blob=ribbon();blob=bytearray(blob)
        def accessor(rows,kind):
            while len(blob)%4:blob.append(0)
            start=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+'f'*len(flat),*flat))
            doc['bufferViews'].append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start))
            doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1,componentType=5126,count=len(rows),type=kind))
            return len(doc['accessors'])-1
        times=accessor([(0,),(1,)],'SCALAR');doc['accessors'][times].update(min=[0],max=[1])
        motion=accessor([(0,0,0),(1,0,0)],'VEC3')
        rotation=accessor([(0,0,0,1),(0,math.sqrt(.5),0,math.sqrt(.5))],'VEC4')
        for name,yaw in [('Original one meter root',False),('Original translating yaw root',True)]:
            samplers=[dict(input=times,output=motion,interpolation='LINEAR')]
            channels=[dict(sampler=0,target=dict(node=2,path='translation'))]
            if yaw:
                samplers.append(dict(input=times,output=rotation,interpolation='LINEAR'))
                channels.append(dict(sampler=1,target=dict(node=2,path='rotation')))
            doc['animations'].append(dict(name=name,samplers=samplers,channels=channels))
        doc['buffers'][0]['byteLength']=len(blob);source.write_bytes(glb(doc,bytes(blob)));evidence['analytic_source_sha256']=sha(source)
        asset=process.rpc('asset.import',dict(source=native(source)))['asset']
        operations=[]
        for identity,position,collider in [(100,[0,0,0],'CharacterController'),(200,[0,-.2,0],'BoxCollider'),(201,[2,1,0],'BoxCollider'),(900,[250,10,0],'BoxCollider')]:
            operations += [dict(op='entity.create',id=uid(identity),name='Physical witness '+str(identity)),
                dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1]))]
            if identity==100:value=dict(radius=.2,height=1.6,speed=4,jump_speed=5,camera=None)
            else:value=dict(half_extents={200:[20,.2,20],201:[.1,1,3],900:[.5,.5,.5]}[identity],motion='dynamic' if identity==900 else 'static',mass=1,friction=.5,restitution=0)
            operations.append(dict(op='component.set',id=uid(identity),type=collider,value=value))
        operations += [dict(op='asset.instantiate',id=uid(400+i),asset=asset,name='Compiled root driver '+str(i)) for i in range(65)]
        operations.append(dict(op='entity.reparent',id=RIG,parent=uid(100),mode='keep_local'))
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=0,ops=operations))['revision']
        document=json.loads(world.read_text());operations=[]
        for i in range(65):
            rig=copy.deepcopy(document['entities'][uid(400+i)]['components']['AnimationRig'])
            rig.update(clip=1,time=0,speed=1,loop=True,playing=True,root_motion=dict(node=2,mode='character',actor=uid(100),yaw=True) if i==0 else dict(node=2))
            operations.append(dict(op='component.set',id=uid(400+i),type='AnimationRig',value=rig))
            if i:operations.append(dict(op='component.set',id=uid(400+i),type='Transform',value=dict(position=[-3*i,0,0],rotation=[0,0,0,1],scale=[1,1,1])))
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=revision,ops=operations))['revision']
        document=json.loads(world.read_text());nodes=[];after=None
        for page in range(2):
            params=dict(component='RigNode',limit=256,revision=revision)
            if after is not None:params['after']=after
            result=process.rpc('entity.query',params);nodes.extend(result['entities']);after=result['next_after']
            if after is None:break
        need(after is None and len(nodes)==65*3 and len({row['id'] for row in nodes})==len(nodes),'Complete bounded RigNode inventory differs')
        joint=[row['id'] for row in nodes if document['entities'][row['id']]['components']['RigNode']==dict(rig=RIG,node=2)]
        need(len(joint)==1,'Actual returned root mapping differs');joint=joint[0]
        evidence['root_binding']=dict(asset=asset,rig=RIG,actor=uid(100),joint=joint,drivers=65,character_drivers=1,extract_drivers=64)
        retained_source=source.with_name('analytic-root.retained-source.glb');source.rename(retained_source)
        original=world.read_bytes();process.start(revision);loaded=process.load(config)
        need(loaded['module']['backend']==evidence['backend'] and loaded['module']['assembly_sha256']==sha(image),'Wrong compiled consumer/backend')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Native consumer supports dynamic code')
        saves = run/'saves';saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        sequence = 0
        def values():
            return process.game()['module']['values']
        def snapshot(label):
            nonlocal sequence
            sequence += 1;slot = 'root-'+str(sequence)
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision']))
            paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths) == 1 and paths[0].stat().st_size <= 16*1024*1024 and sha(paths[0]) == receipt['sha256'], 'Durable payload receipt differs')
            payload = json.loads(paths[0].read_text(encoding='utf-8'))
            evidence['snapshots'].append(dict(label=label, slot=slot, receipt=receipt, snapshot=payload['snapshot']))
            return copy.deepcopy(payload['snapshot']), slot, receipt
        def step(ticks=1,error=False,animations=None,root_motion=None,inputs=None,retry=False):
            request=dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,ticks=ticks)
            for key,value in [('animations',animations),('root_motion',root_motion),('inputs',inputs)]:
                if value is not None:request[key]=value
            result=process.rpc('runtime.step',request,error)
            if not error:
                process.tick=result['current_tick'];process.session=result['current_session_id']
                if retry:
                    before,_,_=snapshot('before-step-retry');need(process.rpc('runtime.step',request)==dict(result,replayed=True),'Step retry differs')
                    after,_,_=snapshot('after-step-retry');need(exact(before,after),'Retry changed full state')
            return result
        def root_state(rig=RIG):
            return process.rpc('runtime.root_motion',dict(session_id=process.session,tick=process.tick,entity=rig))['state']
        def actor():
            return process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(100)))
        def position():return actor()['world_matrix'][12:15]
        def neutral(value):
            need(value['local_delta']==value['world_requested']==value['world_applied']==[0,0,0] and
                 value['yaw_delta']==value['applied_yaw']==value['translation_error']==0,'Disabled root diagnostics not neutral')
        process.edit(Bone=joint);step(60,retry=True)
        need(.98<=position()[0]<=1.002 and values()['Ticks']==60 and values()['PhysicalInputs']==0,'Compiled Character did not follow original one-meter root clip')
        state=root_state();need(state['from_tick']==process.tick-1 and state['to_tick']==process.tick,'Committed interval differs')
        root=process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=joint))
        need(abs(root['world_matrix'][12]-position()[0])<1e-5,'Actual root bone translation was not stripped')
        extracted=root_state(uid(401));need(extracted['actor'] is None and extracted['world_requested'][0]>.016 and extracted['world_applied']==[0,0,0],'Independent Extract interval differs')
        step(60);blocked=position();state=root_state()
        need(1.65<blocked[0]<1.73 and state['world_requested'][0]>.016 and state['translation_error']>.005,'Real Jolt wall did not separate requested/applied motion')
        step(30);need(abs(position()[0]-blocked[0])<.005,'Blocked motion accumulated debt/penetrated wall')
        process.edit(Mode=1);step();neutral(root_state());need(values()['Invisible']==1,'Disable setter became visible during Tick')
        process.edit(Mode=12);before=position();step(5);neutral(root_state())
        need(position()[0]<before[0]-.2,'Disabled driver did not allow ordinary compiled Character input')
        process.edit(Mode=2);before=position();step();need(root_state()['enabled'] and position()[0]>before[0],'Re-enable/playback replacement failed')
        process.edit(Mode=14);before=position();step();need(actor()['velocity'][1]>0 and position()[1]>before[1],'Root locomotion lost grounded jump/gravity')
        process.edit(Mode=0);step(60);need(abs(position()[1])<.05,'Character did not return to floor under gravity')
        process.edit(Mode=13);before=actor()['yaw'];step(20)
        need(abs(actor()['yaw']-before)>10 and abs(root_state()['applied_yaw'])>.005,'Authored root yaw did not reach physical actor/controller')
        process.edit(Mode=4);before=values();step(2);need(values()['Reads']==before['Reads']+2048,'Exact1024 root read budget/Tick reset failed')
        process.edit(Mode=7);before=values();step();need(values()['Writes']==before['Writes']+64,'Exact64 distinct root writes failed')
        for mode,message in ((3,'1024'),(6,'64'),(10,'move'),(11,'yaw'),(15,'duplicate')):
            process.edit(Mode=mode);before,_,_=snapshot('before-callback-'+str(mode));failure=step(error=True)
            need(message in failure['message'].lower(),'Wrong root callback rejection: '+failure['message'])
            after,_,_=snapshot('after-callback-'+str(mode));need(exact(after,before),'Root callback rejection changed full checkpoint')
        process.edit(Mode=9);before,_,_=snapshot('before-caller-game-overlap')
        failure=step(error=True,root_motion=[dict(entity=RIG)])
        need('same' in failure['message'].lower(),'Caller/game root conflict absent')
        after,_,_=snapshot('after-caller-game-overlap');need(exact(after,before),'Root overlap changed whole checkpoint')
        process.edit(Mode=5,FailAt=str(process.tick+1));before,_,_=snapshot('before-later-tick')
        failure=step(2,error=True);need('later-tick rollback' in failure['message'],'Expected second Tick failure')
        after,_,_=snapshot('after-later-tick');need(exact(after,before) and not (saves/'slot-root-intent').exists(),'Later Tick failed physics/root/game/save rollback')
        process.edit(Mode=5,FailAt='-1');step();state=values()
        epoch=f'{int(state["TicketHigh"]) & ((1<<64)-1):016x}{int(state["TicketLow"]) & ((1<<64)-1):016x}'
        receipt=process.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(state['TicketSequence'])))
        need(receipt['kind']==1 and receipt['state']==3 and receipt['generation']==1 and receipt['committed_tick']==process.tick,'Compiled root intent did not publish')
        evidence['save_intent']=receipt;process.edit(Mode=8);step();need(values()['SaveSucceeded']==1,'Compiled result read failed')
        intent_inventory={p.name:sha(p) for p in (saves/'slot-root-intent').iterdir() if p.is_file()}
        evidence['checks'].append('Compiled Character root route: original cycle/real wall blocking, root stripping, independent Extract, disabled ordinary input, staged invisibility, physical yaw/jump/gravity,1024 reads64 writes, caller/game and callback rejection full rollback, later-Tick rollback includes save intent')
        if not args.native_descriptor:
            before, _, _ = snapshot('before-compatible-reload');prior_game = process.game();process.load(config)
            after, _, _ = snapshot('after-compatible-reload');expected = copy.deepcopy(before)
            expected['payload']['gameplay_revision'] += 1;expected['sha256'] = after['sha256']
            need(exact(after, expected) and after['sha256'] != before['sha256'] and process.game()['revision'] == prior_game['revision']+1, 'Compatible reload lost full checkpoint')
            migration = process.game()['module']['migration']
            need(migration['added'] == migration['removed'] == [] and migration['preserved'] == sorted(prior_game['module']['values']), 'Reload field preservation differs')
            evidence['checks'].append('Same-schema CoreCLR reload preserves complete native checkpoint and every root control/decision field')
        saved, slot, receipt = snapshot('fresh-source')
        # Durable state excludes Jolt contact-cache internals. Build both reference
        # and fresh-owner continuations from the same exact reconstructed state.
        reference_session = fresh()
        restored = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=1, revision=revision, expected_session_id=process.session, expected_tick=process.tick,
            expected_gameplay_revision=process.game()['revision'], new_session_id=reference_session, gameplay=config))
        process.session = reference_session;process.tick = restored['tick']
        reference, _, _ = snapshot('fresh-reference-restored')
        need(exact(reference, saved), 'Same-owner reconstruction changed full checkpoint')
        step(3)
        continuation, _, _ = snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        process = OwnedSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        session = fresh();restored = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        process.session = session;process.tick = restored['tick']
        actual, _, _ = snapshot('fresh-restored');need(exact(actual, saved), 'Fresh process failed exact durable restore')
        step(3);actual, _, _ = snapshot('fresh-actual-continuation')
        need(exact(actual, continuation) and world.read_bytes() == original and not source.exists() and sha(retained_source)==evidence['analytic_source_sha256'], 'Fresh compiled/cooked continuation differs')
        need(intent_inventory == {p.name: sha(p) for p in (saves/'slot-root-intent').iterdir() if p.is_file()}, 'Original intent slot changed')
        paths = list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(paths) == 1 and sha(paths[0]) == receipt['sha256'], 'Original saved generation changed')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        evidence['checks'].append('Fresh native process restores full checkpoint without source GLB; three ordinary grounded compiled root Ticks match the separately reconstructed reference continuation and authored/save bytes remain immutable')
        need(all(sha(path) == pins[name] for name, path in files.items()), 'Consumed source/image closure changed')
        need(time.monotonic() < args.deadline, 'Whole fixture deadline expired');evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error);raise
    finally:
        if process:
            try:
                process.close()
            except BaseException as error:
                evidence['close_error'] = repr(error);evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-started;evidence['rpc_count'] = len(evidence['calls'])
        data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode();need(len(data) <= CAP, 'Final evidence exceeds32MiB')
        (run/'evidence.json').write_bytes(data)
        if time.monotonic() > args.deadline:
            evidence['passed'] = False;evidence['deadline_error'] = 'Final publication exceeded180s'
            (run/'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            raise TimeoutError(evidence['deadline_error'])
        print(run/'evidence.json', flush=True)
        if 'close_error' in evidence and 'error' not in evidence:
            raise RuntimeError(evidence['close_error'])


if __name__ == '__main__':
    main()
