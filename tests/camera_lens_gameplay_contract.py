#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled camera lens controls, staged reads, rollback, births and durable saves.

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
GAME = 'Poima.Tests.ManagedCameraLensGame'
IDENTITY = 'poima.test.managed-camera-lens'
CAP = 32*1024*1024
CAMERA = uid(101)




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
                 ROOT/'tests/managed_camera_lens_gameplay/ManagedCameraLensGame.cs',
                 ROOT/'tests/managed_camera_lens_gameplay/Poima.ManagedCameraLensGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 320 and metadata['required_features'] == ['baseline_v7', 'camera_lens_v1'],
             'Wrong negotiated native camera consumer')
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
                    limitations=['Synthetic compiled world/foreground lens controls; no GPU, physical input, audio, damage or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    def lens(fov, near=.1, far=100):
        return dict(vertical_fov=float(fov), near=float(near), far=float(far))
    try:
        world = run/'world.json';process = OwnedSession(args, world, evidence)
        ops=fixture(actor=False)
        for row in ops:
            if row.get('type')=='Camera':row['value']['viewmodel']=lens(75,.01,10)
        # A genuine falling rigid body makes a later-Tick rollback observable.
        for identity in [200,*range(1000,1065),900]:
            ops += [dict(op='entity.create',id=uid(identity),name='Lens witness '+str(identity),parent=None),
                dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=[10,10,0],rotation=[0,0,0,1],scale=[1,1,1]))]
            if identity==900:kind='BoxCollider';value=dict(half_extents=[.5,.5,.5],motion='dynamic',mass=1,friction=.5,restitution=0)
            else:kind='Camera';value=lens(60)
            ops.append(dict(op='component.set',id=uid(identity),type=kind,value=value))
        camera_recipe=dict(Transform=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1]),Camera=dict(lens(60),viewmodel=lens(75,.01,10)))
        ops.append(dict(op='template.set',id=uid(700),name='Compiled Camera recipe',root=uid(700),
            entities={uid(700):dict(name='Pending Camera',parent=None,components=camera_recipe)}))
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
        def read(entity=CAMERA):
            return process.rpc('runtime.camera_lens',dict(session_id=process.session,tick=process.tick,entity=entity))['state']
        def snapshot(label):
            nonlocal sequence
            sequence+=1;slot='lens-'+str(sequence)
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,
                expected_generation=0,session_id=process.session,expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision'],expected_structure_revision=structure(),expected_material_revision=0))
            paths=list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths)==1 and paths[0].stat().st_size<=16*1024*1024 and sha(paths[0])==receipt['sha256'],'Durable receipt differs')
            payload=json.loads(paths[0].read_text(encoding='utf-8'));state=payload['snapshot']
            need(state['version']==7 and payload['document']['entities'][CAMERA]['components']['Camera']==dict(vertical_fov=70,near=.1,far=100,viewmodel=lens(75,.01,10)),'Durable camera state rewrote authored baseline')
            evidence['snapshots'].append(dict(label=label,slot=slot,receipt=receipt,snapshot=state));return copy.deepcopy(state),slot,receipt
        def step(ticks=1,error=False,commands=None,retry=False):
            request=dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,
                         expected_structure_revision=structure(),ticks=ticks)
            if commands is not None:request['camera_lenses']=commands
            result=process.rpc('runtime.step',request,error)
            if not error:
                process.tick=result['current_tick'];process.session=result['current_session_id']
                if retry:
                    before,_,_=snapshot('before-step-retry')
                    need(process.rpc('runtime.step',request)==dict(result,replayed=True),'Step retry differs')
                    after,_,_=snapshot('after-step-retry');need(exact(before,after),'Retry mutated full checkpoint')
            return result
        before=read();need(before==dict(entity=CAMERA,world=lens(70),viewmodel=lens(75,.01,10),authored_world=lens(70)),'Initial camera baseline differs')
        edit(Mode=1);step(retry=True);after=read()
        need(after['world']==lens(35,.02,300) and after['viewmodel']==lens(55,.005,20) and after['authored_world']==before['world'],'Dual lens update or baseline differs')
        need(values()['Invisible']==1 and values()['ObservedWorld']==70 and values()['ObservedViewmodel']==75,'Compiled read-your-writes leaked')
        # Caller changes are committed at callback entry; game writes remain staged.
        edit(Mode=0);step(commands=[dict(entity=CAMERA,world=lens(80,.04,400),viewmodel=lens(90,.02,25))])
        need(values()['ObservedWorld']==80 and values()['ObservedViewmodel']==90,'Caller lenses were not visible at Tick entry')
        edit(Mode=2);step();need(read()['world']==lens(35,.02,300) and read()['viewmodel']==lens(90,.02,25),'World-only setter changed foreground')
        edit(Mode=3);step();need(read()['world']==lens(35,.02,300) and read()['viewmodel']==lens(55,.005,20),'Foreground-only setter changed world')
        edit(Mode=14);step();need(read(uid(100)) is None,'Live noncamera returned lens state')
        edit(Mode=5);prior=values();step(2);need(values()['Reads']==prior['Reads']+512,'Exact256 reads/Tick failed')
        edit(Mode=15);prior=values();step();need(values()['Writes']==prior['Writes']+64 and read(uid(1063))['world']==lens(45,.03,250),'Exact64 writes failed')
        for mode,message in [(4,'256'),(6,'64'),(7,'duplicate'),(12,'finite'),(13,'foreground')]:
            edit(Mode=mode);before,_,_=snapshot('before-rejected-mode-'+str(mode));failure=step(error=True)
            need(message in failure['message'].lower(),'Unexpected callback rejection: '+failure['message'])
            after,_,_=snapshot('after-rejected-mode-'+str(mode));need(exact(before,after),'Rejected compiled lens command changed full state')
        edit(Mode=1);before,_,_=snapshot('before-caller-game-overlap')
        failure=step(error=True,commands=[dict(entity=CAMERA,world=lens(40))]);need('same' in failure['message'].lower(),'Caller/game overlap accepted')
        after,_,_=snapshot('after-caller-game-overlap');need(exact(before,after),'Caller/game overlap changed checkpoint')
        edit(Mode=8,FailAt=str(process.tick+1));before,_,_=snapshot('before-later-Tick')
        failure=step(2,error=True);need('later-Tick rollback' in failure['message'],'Later callback failure absent')
        after,_,_=snapshot('after-later-Tick');need(exact(before,after) and not (saves/'slot-lens-intent').exists(),'Lens/game/physics/save rollback incomplete')
        edit(Mode=8,FailAt='-1');step();v=values()
        epoch=f'{int(v["TicketHigh"]) & ((1<<64)-1):016x}{int(v["TicketLow"]) & ((1<<64)-1):016x}'
        receipt=process.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(v['TicketSequence'])))
        need(receipt['kind']==1 and receipt['state']==3 and receipt['generation']==1 and receipt['committed_tick']==process.tick,'Compiled lens intent not published')
        evidence['save_intent']=receipt;edit(Mode=9);step();need(values()['SaveSucceeded']==1,'Compiled save result read failed')
        intent_inventory={p.name:sha(p) for p in (saves/'slot-lens-intent').iterdir() if p.is_file()}
        evidence['checks'].append('Compiled dual/partial lens controls, caller-entry reads, staged invisibility, strict256/64 quotas, canonical noncamera absence, duplicate/SDK/native/caller overlap and later-Tick complete rollback including physics and durable intent')
        edit(Mode=10);step();born=values()['Born'];need(structure()==1 and read(born)==dict(entity=born,world=lens(25,.02,200),viewmodel=lens(50,.005,15),authored_world=lens(60)),'Pending camera values failed publication')
        edit(Mode=11);before,_,_=snapshot('before-removal-conflict');failure=step(error=True)
        need('remov' in failure['message'].lower(),'Lens/removal conflict accepted')
        after,_,_=snapshot('after-removal-conflict');need(exact(before,after),'Removal conflict changed lens/membership/allocator')
        edit(Mode=0);evidence['checks'].append('Reserved camera getter uses authored lenses; staged birth writes publish once; removal conflict preserves complete snapshot')
        if not args.native_descriptor:
            before,_,_=snapshot('before-compatible-reload');old_game=process.game();load();after,_,_=snapshot('after-compatible-reload')
            expected=copy.deepcopy(before);expected['payload']['gameplay_revision']+=1;expected['sha256']=after['sha256']
            need(exact(after,expected) and after['sha256']!=before['sha256'],'Compatible reload lost camera/full native state')
            migration=process.game()['module']['migration'];need(migration['added']==migration['removed']==[] and migration['preserved']==sorted(old_game['module']['values']),'Reload field preservation differs')
            evidence['checks'].append('Same-schema CoreCLR reload preserves all camera lenses, born membership, state and frozen authored baseline')
        saved,slot,receipt=snapshot('fresh-source');saved_structure=structure()
        def restore(existing):
            session=fresh();params=dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=1,
                revision=revision,expected_session_id=process.session if existing else None,
                expected_tick=process.tick if existing else None,expected_gameplay_revision=process.game()['revision'] if existing else None,
                expected_structure_revision=structure() if existing else None,new_session_id=session,gameplay=config)
            result=process.rpc('save.load',params);process.session=session;process.tick=result['tick']
        restore(True);reference,_,_=snapshot('fresh-reference-restored');need(exact(reference,saved),'Same-owner reconstruction changed full checkpoint')
        step(3);continuation,_,_=snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        process=OwnedSession(args,world,evidence);process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        restore(False);actual,_,_=snapshot('fresh-restored');need(exact(actual,saved) and structure()==saved_structure,'Fresh camera/structure restore differs')
        step(3);actual,_,_=snapshot('fresh-actual-continuation');need(exact(actual,continuation),'Fresh three-Tick full continuation differs')
        need(world.read_bytes()==original and intent_inventory=={p.name:sha(p) for p in (saves/'slot-lens-intent').iterdir() if p.is_file()},'Authored bytes/original intent slot changed')
        original_payload=list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(original_payload)==1 and sha(original_payload[0])==receipt['sha256'],'Original saved generation changed')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        evidence['checks'].append('Fresh native process restores exact complete authored/runtime/game/born-camera state and matches separately reconstructed three-Tick continuation; original authored/slot bytes unchanged')
        need(all(sha(path)==pins[name] for name,path in files.items()),'Consumed source/image closure changed')
        need(time.monotonic()<args.deadline,'Whole fixture deadline expired');evidence['passed']=True
    except BaseException as error:
        evidence['error']=repr(error);raise
    finally:
        if process:
            try:process.close()
            except BaseException as error:evidence['close_error']=repr(error);evidence['passed']=False
        evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls'])
        data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=CAP,'Final evidence exceeds32MiB')
        (run/'evidence.json').write_bytes(data)
        if time.monotonic()>args.deadline:
            evidence['passed']=False;evidence['deadline_error']='Final publication exceeded180s'
            data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=CAP,'Failure evidence exceeds32MiB')
            (run/'evidence.json').write_bytes(data);raise TimeoutError(evidence['deadline_error'])
        print(run/'evidence.json',flush=True)
        if 'close_error' in evidence and 'error' not in evidence:raise RuntimeError(evidence['close_error'])


if __name__=='__main__':main()
