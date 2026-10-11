#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled two-bone IK controls, bone pose visibility, rollback and saves.

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

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedTwoBoneIkGame'
IDENTITY = 'poima.test.managed-two-bone-ik'
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
                 ROOT/'tests/managed_two_bone_ik_gameplay/ManagedTwoBoneIkGame.cs',
                 ROOT/'tests/managed_two_bone_ik_gameplay/Poima.ManagedTwoBoneIkGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 288 and metadata['required_features'] == ['baseline_v7', 'animation_ik_v1', 'hit_regions_v1'],
             'Wrong negotiated native IK consumer')
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
                    limitations=['Synthetic compiled query decisions; no GPU, physical input, audio, damage or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    try:
        world = run/'world.json';process = OwnedSession(args, world, evidence)
        source = run/'analytic-skin.glb'
        doc,blob=ribbon();doc['nodes'][1]['children']=[3];doc['nodes'].append(dict(name='Effector',translation=[0,1,0]))
        source.write_bytes(glb(doc,blob));evidence['analytic_source_sha256']=sha(source)
        asset=process.rpc('asset.import',dict(source=native(source)))['asset']
        operations=[dict(op='asset.instantiate',id=uid(400+i),asset=asset,name='Compiled chain '+str(i)) for i in range(65)]
        operations += [dict(op='entity.create',id=uid(900),name='Physics rollback witness'),
            dict(op='component.set',id=uid(900),type='Transform',value=dict(position=[250,10,0],rotation=[0,0,0,1],scale=[1,1,1])),
            dict(op='component.set',id=uid(900),type='BoxCollider',value=dict(half_extents=[.5]*3,motion='dynamic',mass=1,friction=.5,restitution=0))]
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=0,ops=operations))['revision']
        document=json.loads(world.read_text());operations=[]
        for i in range(65):
            rig=copy.deepcopy(document['entities'][uid(400+i)]['components']['AnimationRig'])
            rig.update(clip=None,playing=False,ik=[dict(slot=1,root=2,middle=1,end=3,target=[3*i+1,1,0],pole=[3*i,0,1])])
            operations += [dict(op='component.set',id=uid(400+i),type='Transform',value=dict(position=[3*i,0,0],rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=uid(400+i),type='AnimationRig',value=rig)]
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=revision,ops=operations))['revision']
        nodes=[];after=None
        for page in range(2):
            params=dict(component='RigNode',limit=256,revision=revision)
            if after is not None:params['after']=after
            result=process.rpc('entity.query',params);nodes.extend(result['entities']);after=result['next_after']
            if after is None:break
        need(after is None and len(nodes)==65*4 and len({row['id'] for row in nodes})==len(nodes),'Complete bounded RigNode inventory differs')
        joint=None;primary=0
        for row in nodes:
            if document['entities'][row['id']]['components']['RigNode']['rig']!=RIG:continue
            value=process.rpc('entity.get',dict(id=row['id'],revision=revision))['value']['components']['RigNode'];primary+=1
            if value['rig']==RIG and value['node']==3:
                need(joint is None,'Duplicate effector mapping');joint=row['id']
        need(primary==4,'Primary actual rig mapping differs')
        need(joint is not None,'Actual returned three-joint effector missing')
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=revision,ops=[
            dict(op='component.set',id=joint,type='HitRegion',value=dict(owner=RIG,name='effector',shape='sphere',radius=.1))]))['revision']
        evidence['animated_attachment']=dict(asset=asset,rig=RIG,joint=joint,rigs=65)
        source.unlink()  # All queries and fresh restores consume immutable cooked data.
        original = world.read_bytes();process.start(revision);loaded = process.load(config)
        need(loaded['module']['backend'] == evidence['backend'] and loaded['module']['assembly_sha256'] == sha(image), 'Wrong compiled consumer/backend')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False), 'Native consumer supports dynamic code')
        saves = run/'saves';saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        sequence = 0
        def values():
            return process.game()['module']['values']
        def snapshot(label):
            nonlocal sequence
            sequence += 1;slot = 'ik-'+str(sequence)
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision']))
            paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths) == 1 and paths[0].stat().st_size <= 16*1024*1024 and sha(paths[0]) == receipt['sha256'], 'Durable payload receipt differs')
            payload = json.loads(paths[0].read_text(encoding='utf-8'))
            evidence['snapshots'].append(dict(label=label, slot=slot, receipt=receipt, snapshot=payload['snapshot']))
            return copy.deepcopy(payload['snapshot']), slot, receipt
        def step(ticks=1, error=False, animations=None, ik=None, retry=False):
            request = dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick, ticks=ticks)
            if animations is not None:
                request['animations'] = animations
            if ik is not None:
                request['ik']=ik
            result = process.rpc('runtime.step', request, error)
            if not error:
                process.tick = result['current_tick'];process.session = result['current_session_id']
                if retry:
                    before, _, _ = snapshot('before-step-retry')
                    need(process.rpc('runtime.step', request) == dict(result, replayed=True), 'Step retry differs')
                    after, _, _ = snapshot('after-step-retry');need(exact(before, after), 'Step retry changed full state')
            return result
        def ik_state():
            return process.rpc('runtime.ik',dict(session_id=process.session,tick=process.tick,entity=RIG,slot=1))['state']
        def near(actual,wanted):
            import math
            need(len(actual)==len(wanted) and all(math.isfinite(a) and abs(a-b)<=1e-6 for a,b in zip(actual,wanted)), 'Independent solved endpoint differs')
        process.edit(Bone=joint)
        step(2,retry=True);need(values()['Ticks']==2 and values()['Reads']==2,'Committed compiled reads differ')
        near(ik_state()['end_position'],[1,1,0])
        process.edit(Mode=1);step();near(ik_state()['end_position'],[0,2,0])
        need(values()['Invisible']==1 and values()['CorrectedHits']==1,'Staged command/actual corrected bone proof absent')
        step();near([values()['LastEndX'],values()['LastEndY']],[0,2])
        process.edit(Mode=4);before=values();step(2)
        need(values()['Reads']==before['Reads']+2048,'Exact1024 reads or Tick reset failed')
        process.edit(Mode=7);before=values();step()
        need(values()['Writes']==before['Writes']+64,'Exact64 distinct writes failed');near(ik_state()['end_position'],[1,1,0])
        for mode,message in ((2,'slot'),(3,'1024'),(6,'64')):
            process.edit(Mode=mode);before,_,_=snapshot('before-callback-'+str(mode))
            failure=step(error=True);need(message.lower() in failure['message'].lower(),'Wrong IK callback rejection: '+failure['message'])
            after,_,_=snapshot('after-callback-'+str(mode));need(exact(after,before),'IK callback failure changed complete checkpoint')
        process.edit(Mode=9);before,_,_=snapshot('before-caller-game-overlap')
        failure=step(error=True,ik=[dict(entity=RIG,slot=1,target=[1,1,0],pole=[0,0,1])])
        need('same' in failure['message'].lower(),'Expected caller/game same slot rejection')
        after,_,_=snapshot('after-caller-game-overlap');need(exact(after,before),'Overlap changed complete checkpoint')
        process.edit(Mode=10);step();near(ik_state()['end_position'],[1,1,0])
        need(values()['Invisible']==2 and values()['CorrectedHits']==2,'Animation plus IK same-rig publication failed')
        process.edit(Mode=5,FailAt=str(process.tick+1));before,_,_=snapshot('before-later-tick')
        failure=step(2,error=True);need('later-tick rollback' in failure['message'],'Expected second Tick failure')
        after,_,_=snapshot('after-later-tick');need(exact(after,before) and not (saves/'slot-ik-intent').exists(),'Later Tick failed whole rollback including save intent')
        process.edit(Mode=5,FailAt='-1');step();state=values()
        epoch=f'{int(state["TicketHigh"]) & ((1<<64)-1):016x}{int(state["TicketLow"]) & ((1<<64)-1):016x}'
        receipt=process.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(state['TicketSequence'])))
        need(receipt['kind']==1 and receipt['state']==3 and receipt['generation']==1 and receipt['committed_tick']==process.tick,'IK compiled intent did not publish')
        evidence['save_intent']=receipt;process.edit(Mode=8);step();need(values()['SaveSucceeded']==1,'Compiled result read failed')
        intent_inventory={p.name:sha(p) for p in (saves/'slot-ik-intent').iterdir() if p.is_file()}
        evidence['checks'].append('Independent IK288 controls: actual frozen chain/committed solved RigNode+HitRegion; staged setter invisible then published; exact1024 read and64 distinct write budgets/reset; callback and caller/game overlap full rollback; animation+IK same rig; later-Tick rollback includes physics/gameplay/IK/save intent')
        if not args.native_descriptor:
            before, _, _ = snapshot('before-compatible-reload');prior_game = process.game();process.load(config)
            after, _, _ = snapshot('after-compatible-reload');expected = copy.deepcopy(before)
            expected['payload']['gameplay_revision'] += 1;expected['sha256'] = after['sha256']
            need(exact(after, expected) and after['sha256'] != before['sha256'] and process.game()['revision'] == prior_game['revision']+1, 'Compatible reload lost full checkpoint')
            migration = process.game()['module']['migration']
            need(migration['added'] == migration['removed'] == [] and migration['preserved'] == sorted(prior_game['module']['values']), 'Reload field preservation differs')
            evidence['checks'].append('Same-schema CoreCLR reload preserves complete native checkpoint and every IK control/decision field')
        saved, slot, receipt = snapshot('fresh-source');step(3)
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
        need(exact(actual, continuation) and world.read_bytes() == original and not source.exists(), 'Fresh compiled/cooked continuation differs')
        need(intent_inventory == {p.name: sha(p) for p in (saves/'slot-ik-intent').iterdir() if p.is_file()}, 'Original intent slot changed')
        paths = list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(paths) == 1 and sha(paths[0]) == receipt['sha256'], 'Original saved generation changed')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        evidence['checks'].append('Fresh native process restores full checkpoint without source GLB; three ordinary compiled IK Ticks match prior continuation and authored/save bytes remain immutable')
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
