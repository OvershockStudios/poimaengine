#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled character stance controls, clearance, rollback and durable saves.

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
from character_gameplay_contract import fixture

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedCharacterStanceGame'
IDENTITY = 'poima.test.managed-character-stance'
CAP = 32*1024*1024
ACTOR = uid(100)




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
                 ROOT/'tests/components_gameplay_contract.py', ROOT/'managed/Directory.Build.props',
                 ROOT/'tests/managed_character_stance_gameplay/ManagedCharacterStanceGame.cs',
                 ROOT/'tests/managed_character_stance_gameplay/Poima.ManagedCharacterStanceGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 336 and metadata['required_features'] == ['baseline_v7', 'character_stance_v1'],
             'Wrong negotiated native stance consumer')
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
                    limitations=['Synthetic compiled crouch/stand and Jolt clearance; no GPU, window, physical input, audio, animation or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    try:
        # The native32-controller limit includes legacy controllers. Qualify
        # exact32 writes in a separate World, then close it before the sparse
        # doorway/legacy/birth course. The33rd write is refused before lookup.
        quota_world=run/'quota-world.json';process=OwnedSession(args,quota_world,evidence)
        quota_profile=dict(radius=.25,height=2,speed=4,jump_speed=5,camera=None,stance=dict(crouched_height=1,crouched_speed=2,camera_drop=.5))
        quota_ops=[]
        for identity in [100,*range(1000,1031)]:
            quota_ops += [dict(op='entity.create',id=uid(identity),name='Quota controller '+str(identity),parent=None),
                dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=[(identity-1000)*3 if identity!=100 else -10,10,0],rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=uid(identity),type='CharacterController',value=copy.deepcopy(quota_profile))]
        quota_revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=0,ops=quota_ops))['revision']
        quota_original=quota_world.read_bytes();process.start(quota_revision);quota_loaded=process.load(config)
        need(quota_loaded['module']['backend']==evidence['backend'] and quota_loaded['module']['assembly_sha256']==sha(image),'Wrong quota consumer/backend')
        quota_saves=run/'quota-saves';quota_saves.mkdir();process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(quota_saves)))
        quota_sequence=0
        def quota_snapshot(label):
            nonlocal quota_sequence
            quota_sequence+=1;slot='quota-'+str(quota_sequence)
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=process.session,expected_tick=process.tick,expected_gameplay_revision=process.game()['revision'],expected_structure_revision=0,expected_material_revision=0))
            folder=quota_saves/('slot-'+slot);manifest=json.loads((folder/'current.json').read_bytes());member=manifest['payload']['current'];name=member['file']
            need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe quota selected payload')
            path=folder/name;data=path.read_bytes();need(len(data)==member['bytes'] and len(data)<=16*1024*1024 and sha(path)==member['sha256']==receipt['sha256'],'Quota durable digest differs')
            state=json.loads(data)['snapshot'];need(state['version']==8,'Quota snapshot version differs')
            evidence['snapshots'].append(dict(label=label,slot=slot,storage='quota',receipt=receipt,snapshot=state));return copy.deepcopy(state)
        process.edit(Mode=5);prior=process.game()['module']['values'];process.step()
        current=process.game()['module']['values'];need(current['Writes']==prior['Writes']+32,'Exact32 unique writes failed')
        for identity in [100,*range(1000,1031)]:
            state=process.rpc('runtime.character_stance',dict(session_id=process.session,tick=process.tick,entity=uid(identity)))['state']
            need(state['configured'] and state['crouched'] and state['requested_crouch'],'Quota target did not receive staged crouch')
        process.edit(Mode=3);prior=process.game()['module']['values'];process.step()
        need(process.game()['module']['values']['Reads']==prior['Reads']+256,'Exact256 reads failed')
        for mode,message in [(4,'256'),(6,'32')]:
            process.edit(Mode=mode);before=quota_snapshot('before-quota-'+str(mode));failure=process.step(error=True)
            need(message in failure['message'].lower(),'Unexpected quota failure: '+failure['message']);after=quota_snapshot('after-quota-'+str(mode));need(exact(before,after),'Quota failure changed complete state')
        process.edit(Mode=5);before=quota_snapshot('before-combined-quota')
        failure=process.rpc('runtime.step',dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,ticks=1,expected_structure_revision=0,character_stances=[dict(entity=ACTOR,crouch=True)]),True)
        need('32' in failure['message'],'Caller/game33-write quota not rejected');after=quota_snapshot('after-combined-quota');need(exact(before,after),'Combined quota failure changed complete state')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        need(quota_world.read_bytes()==quota_original,'Quota authored bytes changed')
        evidence['checks'].append('Separate32-controller World proves256/257 reads,32/33 unique writes and combined caller/game33-write rejection with complete checkpoint rollback; no controller population limit bypass')
        world = run/'world.json';process = OwnedSession(args, world, evidence)
        ops=fixture(actor=True)
        for row in ops:
            if row.get('type')=='Transform' and row['id']==uid(100):row['value']['position']=[-2,.1,0]
            if row.get('type')=='Transform' and row['id']==uid(300):row['value']['position']=[5,.1,0]
            if row.get('type')=='Transform' and row['id']==uid(1):row['value']['scale']=[500,1,50]
            if row.get('type')=='Transform' and row['id']==uid(2):row['value']=dict(position=[0,1.4,0],rotation=[0,0,0,1],scale=[1.6,.2,2])
            if row.get('type')=='CharacterController' and row['id']==ACTOR:row['value'].update(height=2,speed=4,stance=dict(crouched_height=1,crouched_speed=2,camera_drop=.5))
        profile=dict(radius=.25,height=2,speed=4,jump_speed=5,camera=None,stance=dict(crouched_height=1,crouched_speed=2,camera_drop=.5))
        for identity in [500]:
            ops += [dict(op='entity.create',id=uid(identity),name='Stance witness '+str(identity),parent=None),
                dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=[100+(identity-1000)*3,10,10] if identity!=500 else [10,10,0],rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=uid(identity),type='CharacterController' if identity!=500 else 'BoxCollider',
                    value=copy.deepcopy(profile) if identity!=500 else dict(half_extents=[.5,.5,.5],motion='dynamic',mass=1,friction=.5,restitution=0))]
        recipe=dict(Transform=dict(position=[15,.1,0],rotation=[0,0,0,1],scale=[1,1,1]),CharacterController=profile)
        ops.append(dict(op='template.set',id=uid(700),name='Compiled stance recipe',root=uid(700),
            entities={uid(700):dict(name='Pending controller',parent=None,components=recipe)}))
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=0,ops=ops))['revision']
        original=world.read_bytes();process.start(revision)
        def inspect():return process.rpc('runtime.inspect',dict(session_id=process.session))
        def structure():return inspect()['structure_revision']
        def load():
            return process.rpc('runtime.gameplay.load_native' if args.native_descriptor else 'runtime.gameplay.load',
                dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,
                    expected_revision=process.game()['revision'],expected_structure_revision=structure(),**config))
        def edit(**values):
            return process.rpc('runtime.gameplay.edit',dict(session_id=process.session,request_id=fresh(),
                expected_tick=process.tick,expected_revision=process.game()['revision'],expected_structure_revision=structure(),values=values))
        loaded=load();need(loaded['module']['backend']==evidence['backend'] and loaded['module']['assembly_sha256']==sha(image),'Wrong compiled consumer/backend')
        if args.native_descriptor:need(loaded['module']['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Native consumer allows dynamic code')
        saves=run/'saves';saves.mkdir();process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        sequence=0
        def values():return process.game()['module']['values']
        def read(entity=ACTOR):
            return process.rpc('runtime.character_stance',dict(session_id=process.session,tick=process.tick,entity=entity))['state']
        def entity(identity):return process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(identity)))
        def snapshot(label):
            nonlocal sequence
            sequence+=1;slot='stance-'+str(sequence)
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,
                expected_generation=0,session_id=process.session,expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision'],expected_structure_revision=structure(),expected_material_revision=0))
            folder=saves/('slot-'+slot);manifest=json.loads((folder/'current.json').read_bytes());member=manifest['payload']['current']
            name=member['file'];need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe selected save payload')
            path=folder/name;data=path.read_bytes()
            need(len(data)==member['bytes'] and len(data)<=16*1024*1024 and sha(path)==member['sha256']==receipt['sha256'],'Durable receipt differs')
            payload=json.loads(data);state=payload['snapshot']
            need(state['version']==8 and payload['document']['entities'][ACTOR]['components']['CharacterController']['stance']==dict(crouched_height=1,crouched_speed=2,camera_drop=.5),'Durable stance rewrote frozen authored profile')
            evidence['snapshots'].append(dict(label=label,slot=slot,receipt=receipt,snapshot=state));return copy.deepcopy(state),slot,receipt
        def step(ticks=1,error=False,commands=None,inputs=None,retry=False):
            request=dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,
                         expected_structure_revision=structure(),ticks=ticks)
            if commands is not None:request['character_stances']=commands
            if inputs is not None:request['inputs']=inputs
            result=process.rpc('runtime.step',request,error)
            if not error:
                process.tick=result['current_tick'];process.session=result['current_session_id']
                if retry:
                    before,_,_=snapshot('before-step-retry');need(process.rpc('runtime.step',request)==dict(result,replayed=True),'Step retry differs')
                    after,_,_=snapshot('after-step-retry');need(exact(before,after),'Retry mutated full checkpoint')
            return result
        step(60);initial=read();eye=entity(101)['local_transform']['position'][1];feet=entity(100)['world_matrix'][13]
        need(initial['configured'] and not initial['requested_crouch'] and not initial['crouched'] and initial['height']==2 and initial['speed']==4 and initial['camera_drop']==0,'Initial stance differs')
        edit(Mode=1);step(retry=True);crouched=read()
        need(crouched['requested_crouch'] and crouched['crouched'] and not crouched['stand_blocked'] and crouched['height']==1 and crouched['speed']==2 and crouched['camera_drop']==.5,'Compiled crouch did not commit')
        need(values()['Invisible']==1 and values()['ObservedCrouched']==0 and values()['ObservedHeight']==2,'Compiled staged read exposed crouch')
        need(abs(entity(100)['world_matrix'][13]-feet)<.03 and abs(eye-entity(101)['local_transform']['position'][1]-.5)<1e-6,'Crouch moved feet or failed camera drop')
        step(60,inputs=[dict(entity=ACTOR,move=[1,0],look=[0,0],jump=False,use=False)])
        need(abs(entity(100)['world_matrix'][12])<.05,'Crouched speed failed actual doorway approach')
        edit(Mode=2);step();blocked=read()
        need(not blocked['requested_crouch'] and blocked['crouched'] and blocked['stand_blocked'] and blocked['height']==1,'Ceiling did not block compiled standing')
        step(2);need(exact(blocked,read()),'Standing desire did not remain cached under ceiling')
        edit(Mode=10);step();need(read(uid(500)) is None and not read(uid(300))['configured'],'Noncontroller/legacy transfer differs')
        for mode,message in [(7,'duplicate'),(11,'configured'),(12,'configured'),(13,'entity')]:
            edit(Mode=mode);before,_,_=snapshot('before-rejected-mode-'+str(mode));failure=step(error=True)
            need(message in failure['message'].lower(),'Unexpected callback rejection: '+failure['message'])
            after,_,_=snapshot('after-rejected-mode-'+str(mode));need(exact(before,after),'Rejected compiled stance command changed full state')
        for mode,commands in [(1,[dict(entity=ACTOR,crouch=False)])]:
            edit(Mode=mode);before,_,_=snapshot('before-caller-game-'+str(mode));failure=step(error=True,commands=commands)
            need(('same' if mode==1 else '32') in failure['message'].lower(),'Caller/game overlap or combined quota accepted')
            after,_,_=snapshot('after-caller-game-'+str(mode));need(exact(before,after),'Caller/game rejection changed full checkpoint')
        edit(Mode=8,FailAt=str(process.tick+1));before,_,_=snapshot('before-later-Tick');failure=step(2,error=True)
        need('later-Tick rollback' in failure['message'],'Later callback failure absent');after,_,_=snapshot('after-later-Tick')
        need(exact(before,after) and not (saves/'slot-stance-intent').exists(),'Stance/game/physics/save rollback incomplete')
        edit(Mode=8,FailAt='-1');step();v=values();epoch=f'{int(v["TicketHigh"]) & ((1<<64)-1):016x}{int(v["TicketLow"]) & ((1<<64)-1):016x}'
        receipt=process.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(v['TicketSequence'])))
        need(receipt['kind']==1 and receipt['state']==3 and receipt['generation']==1 and receipt['committed_tick']==process.tick,'Compiled stance intent not published')
        evidence['save_intent']=receipt;edit(Mode=9);step();need(values()['SaveSucceeded']==1,'Compiled durable result failed')
        intent_inventory={p.name:sha(p) for p in (saves/'slot-stance-intent').iterdir() if p.is_file()}
        # Standing intent retries without another setter once movement clears the doorway.
        edit(Mode=2);step();need(read()['stand_blocked'],'Reissued standing desire should be blocked')
        step(60,inputs=[dict(entity=ACTOR,move=[1,0],look=[0,0],jump=False,use=False)])
        standing=read();need(not standing['requested_crouch'] and not standing['crouched'] and not standing['stand_blocked'] and standing['height']==2 and standing['speed']==4 and standing['camera_drop']==0,'Persistent standing did not succeed after leaving doorway')
        evidence['checks'].append('Compiled capsule/camera crouch, physical ceiling clearance, persistent standing retry, staged reads, independent rights, exact256/32 quotas and whole callback/caller/later-Tick rollback')
        edit(Mode=14);step();born=values()['Born'];need(structure()==1 and read(born)['configured'] and read(born)['crouched'],'Pending configured controller did not publish crouched')
        edit(Mode=15);before,_,_=snapshot('before-removal-conflict');failure=step(error=True)
        need('remov' in failure['message'].lower(),'Stance/removal conflict accepted');after,_,_=snapshot('after-removal-conflict');need(exact(before,after),'Removal conflict changed full membership/stance snapshot')
        edit(Mode=0);evidence['checks'].append('Reserved authored controller getter and staged crouch publish once; removal conflict restores complete state')
        if not args.native_descriptor:
            before,_,_=snapshot('before-compatible-reload');old_game=process.game();load();after,_,_=snapshot('after-compatible-reload')
            expected=copy.deepcopy(before);expected['payload']['gameplay_revision']+=1;expected['sha256']=after['sha256']
            need(exact(after,expected) and after['sha256']!=before['sha256'],'Compatible reload lost stance/full native state')
            migration=process.game()['module']['migration'];need(migration['added']==migration['removed']==[] and migration['preserved']==sorted(old_game['module']['values']),'Reload field preservation differs')
            evidence['checks'].append('Same-schema CoreCLR reload preserves stance, membership, game state and frozen content')
        saved,slot,receipt=snapshot('fresh-source');saved_structure=structure()
        def restore(existing):
            session=fresh();params=dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=1,
                revision=revision,expected_session_id=process.session if existing else None,
                expected_tick=process.tick if existing else None,expected_gameplay_revision=process.game()['revision'] if existing else None,
                expected_structure_revision=structure() if existing else None,new_session_id=session,gameplay=config)
            result=process.rpc('save.load',params);process.session=session;process.tick=result['tick']
        restore(True);reference,_,_=snapshot('fresh-reference-restored');need(exact(reference,saved),'Same-owner cold reconstruction changed full checkpoint')
        step(3);continuation,_,_=snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        process=OwnedSession(args,world,evidence);process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        restore(False);actual,_,_=snapshot('fresh-restored');need(exact(actual,saved) and structure()==saved_structure,'Fresh stance/structure restore differs')
        step(3);actual,_,_=snapshot('fresh-actual-continuation');need(exact(actual,continuation),'Fresh three-Tick full cold continuation differs')
        need(world.read_bytes()==original and intent_inventory=={p.name:sha(p) for p in (saves/'slot-stance-intent').iterdir() if p.is_file()},'Authored bytes/original intent slot changed')
        original_payload=list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(original_payload)==1 and sha(original_payload[0])==receipt['sha256'],'Original saved generation changed')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        evidence['checks'].append('Exact complete cold reconstruction and three ordinary Ticks agree across native owners; authored and durable intent bytes unchanged; Jolt warm-cache trajectory equivalence is outside scope')
        need(all(sha(path)==pins[name] for name,path in files.items()),'Consumed source/image closure changed')
        need(time.monotonic()<args.deadline,'Whole fixture deadline expired');evidence['passed']=True
    except BaseException as error:
        evidence['error']=repr(error);raise
    finally:
        if process:
            try:process.close()
            except BaseException as error:evidence['close_error']=repr(error);evidence['passed']=False
        evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls'])
        data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=CAP and evidence['rpc_count']<=1000,'Final evidence/RPC budget exceeded')
        need(sum(path.stat().st_size for path in run.rglob('*') if path.is_file())+len(data)<=128*1024*1024,'Whole output exceeds128MiB')
        (run/'evidence.json').write_bytes(data)
        if time.monotonic()>args.deadline:
            evidence['passed']=False;evidence['deadline_error']='Final publication exceeded180s'
            data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=CAP,'Failure evidence exceeds32MiB')
            (run/'evidence.json').write_bytes(data);raise TimeoutError(evidence['deadline_error'])
        print(run/'evidence.json',flush=True)
        if 'close_error' in evidence and 'error' not in evidence:raise RuntimeError(evidence['close_error'])


if __name__=='__main__':main()
