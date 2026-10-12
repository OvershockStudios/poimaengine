#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compiled virtual locomotion, staged controls, rollback and durable saves.

Consumes a supplied CoreCLR assembly or Native AOT descriptor; builds nothing.
Run beneath an external process-tree owner. No renderer, audio or timing claim.
"""
import argparse
import copy
import json
import math
import struct
import traceback
from pathlib import Path
import subprocess
import time

from input_actions_gameplay_contract import OwnedSession, exact, fresh, need
from components_gameplay_contract import sha, uid

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedCharacterLocomotionGame'
IDENTITY = 'poima.test.managed-character-locomotion'
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
                 ROOT/'tests/managed_character_locomotion_gameplay/ManagedCharacterLocomotionGame.cs',
                 ROOT/'tests/managed_character_locomotion_gameplay/Poima.ManagedCharacterLocomotionGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 336 and metadata['required_features'] == ['baseline_v7', 'character_input_v1', 'character_stance_v1'],
             'Wrong negotiated native locomotion consumer')
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
                    limitations=['Synthetic compiled virtual movement and Jolt traversal; no GPU, window, physical input, audio, animation, arbitrary navigation or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    evidence.update(stages={})
    process=None
    try:
        world=run/'world.json';process=OwnedSession(args,world,evidence)
        discovery=process.rpc('world.describe',{})
        need(discovery['schema_revision']==94,'Compiled locomotion discovery revision differs')
        def box(n,position,half,motion='static',rotation=(0,0,0,1)):
            return [dict(op='entity.create',id=uid(n),name='Compiled locomotion course '+str(n),parent=None),
                dict(op='component.set',id=uid(n),type='Transform',value=dict(position=position,rotation=list(rotation),scale=[1,1,1])),
                dict(op='component.set',id=uid(n),type='BoxCollider',value=dict(half_extents=half,motion=motion,mass=1,friction=.5,restitution=0))]
        ops=box(1,[0,-.5,14],[20,.5,24])+box(20,[0,.5,20],[2,.5,2],'kinematic')
        for i in range(3):ops+=box(10+i,[i+.5,(i+1)*.1,0],[.5,(i+1)*.1,1])
        ops+=box(13,[.5,.5,4],[.5,.5,1])
        for n,degrees,z in ((30,35,8),(40,55,12)):
            angle=math.radians(degrees)
            ops+=box(n,[3,3*math.tan(angle)-.1/math.cos(angle),z],[3/math.cos(angle),.1,1],rotation=[0,0,math.sin(angle/2),math.cos(angle/2)])
        profile=dict(radius=.3,height=1.8,speed=2,jump_speed=5,camera=None,locomotion={},
                     stance=dict(crouched_height=1,crouched_speed=1,camera_drop=.4))
        positions={100:[-2,.1,0],200:[-2,.1,4],300:[-2,.1,8],400:[-2,.1,12],500:[0,1.1,20]}
        for n,position in positions.items():
            ops += [dict(op='entity.create',id=uid(n),name='Virtual compiled actor '+str(n),parent=None),
                dict(op='component.set',id=uid(n),type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=uid(n),type='CharacterController',value=copy.deepcopy(profile))]
        # A falling rigid body makes later-Tick rollback include ordinary physics.
        ops+=box(600,[10,10,25],[.5,.5,.5],'dynamic')
        revision=process.rpc('world.transact',dict(request_id=fresh(),base_revision=0,ops=ops))['revision']
        original=world.read_bytes();process.start(revision)
        loaded=process.load(config)
        need(loaded['module']['backend']==evidence['backend'] and loaded['module']['assembly_sha256']==sha(image),'Compiled locomotion image/backend differs')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Native AOT permits dynamic code')
        saves=run/'saves';saves.mkdir();process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        sequence=0
        def values():return process.game()['module']['values']
        def edit(**fields):return process.edit(**fields)
        def entity(n):return process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(n)))
        def xyz(row):return [row['world_matrix'][i] for i in (12,13,14)]
        def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
        def cached(n):
            row=entity(n);v=row['locomotion']
            need(row['has_body'] and row['is_character'] and row['motion']=='character' and v['mode']=='virtual-v1' and
                 exact(v['profile'],dict(step_height=f32(.4),ground_snap=.5,max_slope_degrees=45.0)) and
                 all(math.isfinite(x) for x in row['velocity']+v['ground_normal']+v['ground_velocity']),'Native frozen virtual report differs')
            return row
        def snapshot(label):
            nonlocal sequence
            sequence+=1;slot='locomotion-'+str(sequence)
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,
                session_id=process.session,expected_tick=process.tick,expected_gameplay_revision=process.game()['revision'],
                expected_structure_revision=0,expected_material_revision=0))
            folder=saves/('slot-'+slot);manifest=json.loads((folder/'current.json').read_bytes());member=manifest['payload']['current'];name=member['file']
            need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe selected durable filename')
            path=folder/name;data=path.read_bytes()
            need(len(data)==member['bytes']==receipt['bytes'] and len(data)<=16*1024*1024 and
                 sha(path)==member['sha256']==receipt['sha256'] and receipt['generation']==member['generation']==manifest['payload']['generation']==1,'Selected durable receipt differs')
            payload=json.loads(data);state=payload['snapshot']
            need(state['version']==9 and payload['document']['entities'][ACTOR]['components']['CharacterController']==profile,'Saved frozen virtual profile differs')
            evidence['snapshots'].append(dict(label=label,slot=slot,receipt=receipt,snapshot=state))
            return copy.deepcopy(state),slot,receipt
        def step(count=1,error=False,inputs=None,request=None):
            params=dict(session_id=process.session,request_id=request or fresh(),expected_tick=process.tick,
                        expected_structure_revision=0,ticks=count)
            if inputs is not None:params['inputs']=inputs
            result=process.rpc('runtime.step',params,error)
            if not error:process.tick=result['current_tick'];process.session=result['current_session_id']
            return params,result
        step(60);initial=cached(100);initial_read=cached(100)
        need(exact(initial,initial_read),'Virtual cached read changed state')
        entry_tick=process.tick;step();observed=values()
        need(int(observed['LastTick'])==entry_tick and observed['ObservedX']==xyz(initial)[0] and
             observed['ObservedY']==xyz(initial)[1] and observed['ObservedVelocityX']==0 and
             math.isfinite(observed['ObservedVelocityY']) and abs(observed['ObservedVelocityY'])<1,
             'Compiled getter did not observe committed entry pose and neutral prepared velocity')
        # All traversal inputs originate in the compiled Tick, not caller inputs.
        edit(Mode=1);prior=values();step(120)
        ramp=cached(300);steep=cached(400);wall=cached(200)
        need(values()['Inputs']==prior['Inputs']+480,'Compiled four-lane movement count differs')
        p=xyz(ramp);angle=math.radians(35)
        need(p[0]>.7 and p[1]>.4 and abs(p[1]-(math.tan(angle)*p[0]+f32(.3)*(1/math.cos(angle)-1)))<.06,'Compiled walkable ramp differs from independent plane/capsule geometry')
        normal=ramp['locomotion']['ground_normal']
        need(abs(normal[0]+math.sin(angle))<.03 and abs(normal[1]-math.cos(angle))<.03 and abs(normal[2])<.03,'Walkable support normal differs')
        need(xyz(steep)[0]<.5 and xyz(steep)[1]<.7 and xyz(wall)[0]<.05 and xyz(wall)[1]<.15,'Steep or over-height lane passed the configured limits')
        edit(Mode=2);step(24);stairs=cached(100)
        # Third box spans x[2,3], top y.6. Keep the radius.3 capsule center
        # strictly inside its clear tread; an exact travel distance is not
        # prescribed while the solver negotiates each actual step boundary.
        need(2.3<xyz(stairs)[0]<2.7 and .55<xyz(stairs)[1]<.67 and stairs['ground']=='on_ground' and
             max(abs(a-b) for a,b in zip(stairs['locomotion']['ground_normal'],[0,1,0]))<1e-6,
             'Compiled movement did not reach the clear supported third tread')
        third_tread_hit=process.rpc('runtime.raycast',dict(session_id=process.session,tick=process.tick,
            origin=[xyz(stairs)[0],1.1,.5],direction=[0,-1,0],distance=2))['hit']
        need(third_tread_hit is not None and third_tread_hit['entity']==uid(12) and
             third_tread_hit['triangle'] is None and third_tread_hit['normal'] is not None and
             max(abs(a-b) for a,b in zip(third_tread_hit['normal'],[0,1,0]))<1e-6 and
             max(abs(a-b) for a,b in zip(third_tread_hit['position'],[xyz(stairs)[0],.6,.5]))<1e-6 and
             abs(third_tread_hit['distance']-.5)<1e-6 and abs(third_tread_hit['fraction']-.25)<1e-6,
             'Independent downray differs from the authored third tread')
        evidence['stages']['traversal']=dict(stairs=stairs,ramp=ramp,steep=steep,wall=wall,third_tread_hit=third_tread_hit)
        evidence['checks'].append('Code-driven fixed-Tick stairs, walkable35-degree slope, rejected55-degree slope and1m wall; independent foot/normal geometry')
        # Real stance controls change the active virtual shape and movement speed.
        edit(Mode=5,Crouch=1);step();stance=process.rpc('runtime.character_stance',dict(session_id=process.session,tick=process.tick,entity=ACTOR))['state']
        need(stance['configured'] and stance['crouched'] and stance['height']==1 and stance['speed']==1 and values()['Invisible']==1,'Virtual staged crouch/shape did not commit')
        before=cached(100);edit(Mode=6);step(6);slow=cached(100)
        need(abs(xyz(slow)[0]-xyz(before)[0]-.1)<.04 and abs(slow['velocity'][0]-1)<.05,'Crouched compiled relative speed differs')
        edit(Mode=5,Crouch=0);step();standing=process.rpc('runtime.character_stance',dict(session_id=process.session,tick=process.tick,entity=ACTOR))['state']
        need(not standing['crouched'] and standing['height']==f32(1.8) and standing['speed']==2,'Virtual standing did not restore authored shape/speed')
        jump_start=cached(100);edit(Mode=3);step();step(4);jump=cached(100)
        need(xyz(jump)[1]>xyz(jump_start)[1]+.2 and jump['velocity'][1]>2 and jump['ground']=='in_air','Compiled jump was suppressed by ground snap')
        evidence['stages']['stance_jump']=dict(crouched=slow,jump=jump,standing=standing)
        edit(Mode=13);step();need(values()['ProxyHits']==1,'Compiled virtual proxy ray absent')
        edit(Mode=4);platform_before=cached(500);params,result=step(60);platform=cached(500);support=entity(20)
        need(abs(xyz(support)[0]-1.5)<1e-5 and abs(xyz(platform)[0]-xyz(platform_before)[0]-1.5)<.08 and
             abs(xyz(platform)[1]-xyz(platform_before)[1])<.03 and abs(platform['locomotion']['ground_velocity'][0]-1.5)<.03,'Compiled kinematic target did not carry neutral virtual actor')
        before_retry,_,_=snapshot('before-step-retry');replayed=process.rpc('runtime.step',params)
        need(exact(replayed,dict(result,replayed=True)),'Exact step replay differs');after_retry,_,_=snapshot('after-step-retry')
        need(exact(before_retry,after_retry),'Retried platform interval changed complete snapshot')
        edit(Mode=13);step();need(values()['ProxyHits']==2,'Moved query proxy absent from compiled ray')
        foot=xyz(cached(500));ray=process.rpc('runtime.raycast',dict(session_id=process.session,tick=process.tick,origin=[foot[0]-2,foot[1]+.9,foot[2]],direction=[1,0,0],distance=4))['hit']
        need(ray is not None and ray['entity']==uid(500) and 1<ray['distance']<2,'Native proxy ray does not agree with compiled ray')
        evidence['stages']['platform']=dict(before=platform_before,mid=platform,support=support,proxy_hit=ray)
        evidence['checks'].append('Committed entry pose and prepared callback velocity; actual post-Tick movement, staged crouch/speed, upward jump and proxies tracking a compiled moving-support command')
        for label,mode,inputs,message in [('duplicate',11,None,'duplicate'),('caller-game',2,[dict(entity=ACTOR,move=[1,0],look=[0,0],jump=False,use=False)],'same')]:
            edit(Mode=mode);before,_,_=snapshot('before-'+label);_,error=step(error=-32040,inputs=inputs)
            need(message in error['message'].lower(),'Unexpected rejected compiled batch')
            after,_,_=snapshot('after-'+label);need(exact(before,after),'Rejected batch changed complete virtual/game/physics state')
        edit(Mode=9,FailAt=str(process.tick+1));before,_,_=snapshot('before-later-Tick');_,error=step(2,error=-32040)
        need('later-Tick rollback' in error['message'],'Later callback failure missing');after,_,_=snapshot('after-later-Tick')
        need(exact(before,after) and not (saves/'slot-locomotion-intent').exists(),'Later Tick left motor state or premature durable intent')
        edit(Mode=9,FailAt='-1');step();v=values();epoch=f'{int(v["TicketHigh"]) & ((1<<64)-1):016x}{int(v["TicketLow"]) & ((1<<64)-1):016x}'
        intent=process.rpc('runtime.save.result',dict(epoch=epoch,sequence=int(v['TicketSequence'])))
        need(intent['state']==3 and intent['kind']==1 and intent['generation']==1 and intent['committed_tick']==process.tick,'Actual gameplay save intent failed')
        evidence['save_intent']=intent;edit(Mode=10);step();need(values()['SaveSucceeded']==1,'Compiled durable result absent')
        intent_inventory={p.name:sha(p) for p in (saves/'slot-locomotion-intent').iterdir() if p.is_file()}
        edit(Mode=0,FailAt='-1');step(60)
        stopped=cached(500);step(15);need(abs(xyz(cached(500))[0]-xyz(stopped)[0])<.03,'Stopped platform left accumulating transport')
        evidence['checks'].append('Exact complete batch/later-Tick rollback and inert step replay; gameplay durable intent commits only after the accepted batch')
        if not args.native_descriptor:
            before,_,_=snapshot('before-compatible-reload');old=process.game();process.load(config);after,_,_=snapshot('after-compatible-reload')
            expected=copy.deepcopy(before);expected['payload']['gameplay_revision']+=1;expected['sha256']=after['sha256']
            need(exact(after,expected) and after['sha256']!=before['sha256'],'CoreCLR reload changed motor/game/full state')
            migration=process.game()['module']['migration'];need(migration['added']==migration['removed']==[] and migration['preserved']==sorted(old['module']['values']),'Compatible migration fields differ')
            evidence['checks'].append('Compatible CoreCLR reload preserves all authored profiles, native motor state and game fields')
        saved,slot,receipt=snapshot('fresh-source')
        def restore(active):
            session=fresh();result=process.rpc('save.load',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=1,
                revision=revision,expected_session_id=process.session if active else None,expected_tick=process.tick if active else None,
                expected_gameplay_revision=process.game()['revision'] if active else None,expected_structure_revision=0 if active else None,
                new_session_id=session,gameplay=config))
            process.session=session;process.tick=result['tick']
        restore(True);cold,_,_=snapshot('fresh-reference-restored');need(exact(cold,saved),'Same-owner cold v9 reconstruction differs')
        step(3);continued,_,_=snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        process=OwnedSession(args,world,evidence);process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        restore(False);cold,_,_=snapshot('fresh-restored');need(exact(cold,saved),'Fresh owner did not exactly reconstruct full v9 state')
        step(3);actual,_,_=snapshot('fresh-actual-continuation');need(exact(actual,continued),'Three cold-restored compiled Ticks disagree')
        need(world.read_bytes()==original and intent_inventory=={p.name:sha(p) for p in (saves/'slot-locomotion-intent').iterdir() if p.is_file()},'Authored/original intent inventory changed')
        original_payload=list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(original_payload)==1 and sha(original_payload[0])==receipt['sha256'],'Original saved generation changed')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        evidence['checks'].append('Complete v9 cold reconstruction plus3 ordinary compiled Ticks exactly agree across owners; warm-cache trajectories are outside scope')
        need(all(sha(path)==pins[name] for name,path in files.items()),'Consumed input closure changed')
        need(time.monotonic()<args.deadline,'Fixture deadline expired');evidence['passed']=True
    except BaseException:
        evidence['error']=traceback.format_exc();evidence['passed']=False
    finally:
        if process:
            try:process.close()
            except BaseException:evidence['close_error']=traceback.format_exc();evidence['passed']=False
        evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls'])
        try:
            need(all(sha(path)==pins[name] for name,path in files.items()),'Terminal source closure changed')
            data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode()
            need(len(data)<=CAP and evidence['rpc_count']<=1000,'Evidence/RPC budget exceeded')
            need(sum(p.stat().st_size for p in run.rglob('*') if p.is_file())+len(data)<=128*1024*1024,'Whole output budget exceeded')
            (run/'evidence.json').write_bytes(data)
            need(time.monotonic()<=args.deadline,'Final publication exceeded180s')
        except BaseException:
            first=evidence.get('error') or evidence.get('close_error') or traceback.format_exc();evidence['passed']=False
            if (run/'evidence.json').exists():
                need(not (run/'unqualified-evidence.json').exists(),'Failure archive destination occupied')
                (run/'evidence.json').rename(run/'unqualified-evidence.json')
            (run/'failure.json').write_text(json.dumps(dict(passed=False,error=first,publication_error=traceback.format_exc(),rpc_count=evidence['rpc_count']),indent=2)+'\n')
    print(json.dumps(dict(passed=evidence['passed'],rpc_count=evidence['rpc_count'],checks=len(evidence['checks']))),flush=True)
    return 0 if evidence['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
