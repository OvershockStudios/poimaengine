#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Consume prebuilt weapon sample through native ticks, full saves and cold restore.

CPU only. No build, renderer, sound device or historical fixture mutation.
External process-tree ownership is required. Literal RPC bytes are retained.
"""
import argparse,copy,importlib.util,json,math,re,subprocess,time
from pathlib import Path
from components_gameplay_contract import sha,uid
from input_actions_gameplay_contract import exact,fresh,need
from fps_feedback_gameplay_contract import RecordedSession,CAP
from switchback_oracle import read_checkpoint,exact_checkpoint
ROOT=Path(__file__).resolve().parents[1]
SAMPLE=ROOT/'examples/weapon-actions'
GAME='Poima.Examples.WeaponActionsGame'
IDENTITY='poima.example.weapon-actions'

class WeaponSession(RecordedSession):
    def rpc(self,method,params=None,error=False):
        need(len(self.evidence['calls'])<450,'RPC admission450 bound')
        return super().rpc(method,params,error)

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    for name in('hostfxr','bridge','assembly','native-descriptor'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--output',required=True,type=Path);parser.add_argument('--windows-interop',action='store_true');args=parser.parse_args()
    if bool(args.native_descriptor)==bool(args.hostfxr or args.bridge or args.assembly):parser.error('Supply CoreCLR paths or native descriptor.')
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):parser.error('CoreCLR requires hostfxr, bridge and assembly.')
    def native(path):
        value=str(Path(path).resolve());return subprocess.check_output(['wslpath','-w',value],text=True).strip()if args.windows_interop else value
    args.native=native;run=args.output.resolve();run.mkdir(parents=True,exist_ok=False);started=time.monotonic();args.deadline=started+180
    files=[Path(__file__).resolve(),args.binary.resolve(),ROOT/'tests/fps_feedback_gameplay_contract.py',ROOT/'tests/input_actions_gameplay_contract.py',ROOT/'tests/components_gameplay_contract.py',ROOT/'tests/character_gameplay_contract.py',ROOT/'tests/switchback_oracle.py',ROOT/'examples/fps-feedback/prepare.py',ROOT/'examples/fps-feedback/PoseMath.cs']
    files+=sorted(p for p in SAMPLE.iterdir()if p.suffix in('.cs','.csproj','.py'))
    files+=[ROOT/'packages/fps'/name for name in('Weapons.cs','Feedback.cs','Recoil.cs')]
    if args.native_descriptor:
        descriptor=args.native_descriptor.resolve();need(descriptor.stat().st_size<=1048576,'Descriptor1MiB bound');metadata=json.loads(descriptor.read_bytes())
        need(metadata['type']==GAME and metadata['identity']==IDENTITY and metadata['services_version']==7 and metadata['minimum_services_bytes']==392 and 'camera_pose_v1'in metadata['required_features'],'Wrong sample descriptor')
        files.append(descriptor)
        for row in metadata['files']:
            relative=Path(row['path']);path=(descriptor.parent/relative).resolve(strict=True)
            need(not relative.is_absolute()and'..'not in relative.parts and path.is_relative_to(descriptor.parent)and path.stat().st_size==row['size']and sha(path)==row['sha256'],'Native artifact closure differs');files.append(path)
        image=(descriptor.parent/metadata['library']).resolve(strict=True);need(image in files,'Native image absent from closure');config=dict(descriptor=native(descriptor),expected_descriptor_sha256=sha(descriptor))
    else:
        image=args.assembly.resolve();files +=[image,args.bridge.resolve(),args.hostfxr.resolve()]
        for assembly in(image,args.bridge.resolve()):
            sdk=assembly.parent/'Poima.Gameplay.dll';need(sdk.is_file(),'Consumed SDK missing');files.append(sdk)
            for suffix in('.deps.json','.runtimeconfig.json'):
                p=assembly.with_suffix(suffix)
                if p.is_file():files.append(p)
        config=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(image),type=GAME)
    pins={str(p):sha(p)for p in set(files)}
    evidence=dict(format='poima.weapon-actions-compiled-contract.v1',passed=False,calls=[],owners=[],checks=[],observations=[],checkpoints=[],whole_pairs=[],source_pins=pins,backend='native_aot'if args.native_descriptor else'coreclr',limits=dict(seconds=180,rpc=450,evidence_bytes=CAP),scope='Adapted compiled sample with synthetic native input, full native save/rollback/cold continuation. No GPU, physical input, sound device, performance, full weapon framework or fresh-agent claim.')
    process=None;sequence=0
    try:
        prepare=module('weapon_sample_prepare',SAMPLE/'prepare.py');prepared=prepare.prepare(run/'project');evidence['preparation']=prepared
        world=run/'project/world.json';process=WeaponSession(args,world,evidence);revision=process.rpc('world.inspect')['revision']
        configure=module('weapon_sample_controls',SAMPLE/'configure.py');authored=process.rpc('world.transact',dict(request_id=fresh(),base_revision=revision,ops=configure.operations()));revision=authored['revision'];original=world.read_bytes()
        process.start(revision);loaded=process.load(config);need(loaded['module']['backend']==evidence['backend']and loaded['module']['assembly_sha256']==sha(image),'Wrong compiled consumer')
        schema=loaded['module']['schema']['fields'];names={field['name']for field in schema};need(len(schema)==len(names)==33,'Expected all33 flat primitive sample fields')
        saves=run/'saves';saves.mkdir();process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        def inspect():return process.rpc('runtime.inspect',dict(session_id=process.session))
        def validate(v):
            need(type(v)is dict and set(v)==names,'Missing original primitive state fields')
            for field in schema:
                value=v[field['name']];kind=field['kind']
                if kind=='int32':need(type(value)is int and -(2**31)<=value<2**31,'Invalid raw int32')
                elif kind=='int64':need(type(value)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value)is not None and -(2**63)<=int(value)<2**63,'Invalid canonical raw int64')
                else:need(kind=='float64'and type(value)is float and math.isfinite(value),'Invalid raw float64')
            need(v['Magazine']+v['Chamber']+v['Reserve']+int(v['AcceptedShots'])==17,'Native consumer lost/duplicated inventory')
            need(int(v['RecoilImpulses'])==int(v['AcceptedShots']),'Feedback impulses differ from accepted shots')
            return v
        def values():return validate(process.game()['module']['values'])
        def observe(label):
            value=values();evidence['observations'].append(dict(label=label,tick=process.tick,values=copy.deepcopy(value)));return value
        def snapshot(label):
            nonlocal sequence
            sequence+=1;slot='weapon-proof-'+str(sequence);state=inspect();game=process.game()
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=process.session,expected_tick=process.tick,expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
            row=read_checkpoint(saves,slot,receipt);validate(row['snapshot']['payload']['gameplay']['values'])
            directory=saves/('slot-'+slot);current=json.loads((directory/'current.json').read_bytes())['payload']['current'];row['original_path']=str((directory/current['file']).relative_to(run))
            evidence['checkpoints'].append(dict(label=label,slot=slot,**row));return copy.deepcopy(row),slot
        def pair(label,left,right):
            need(exact_checkpoint(left,right)and(run/left['original_path']).read_bytes()==(run/right['original_path']).read_bytes(),'Complete original checkpoint bytes differ: '+label);evidence['whole_pairs'].append(dict(label=label,equal=True,bytes_equal=True,left=left['original_path'],right=right['original_path'],exclusions=[]))
        def control(n):
            state=inspect();game=process.game();r=process.rpc('runtime.ui.activate',dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=uid(n)));process.session=r['current_session_id'];process.tick=r['current_tick'];return r
        def step(ticks=1,fire=False,reload=False,aim=False,crouch=False,look=(0,0),move=(0,0),error=False,poses=None):
            need(len(evidence['calls'])<440,'RPC admission450 bound')
            actions=[dict(name=n,held=v,pressed=v,released=not v)for n,v in [('aim',aim),('crouch',crouch),('fire',fire),('reload',reload)]]
            request=dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,expected_structure_revision=inspect()['structure_revision'],ticks=ticks,inputs=[dict(entity=uid(2),move=list(move),look=list(look),jump=False,use=False,actions=actions)])
            if poses is not None:request['camera_poses']=poses
            result=process.rpc('runtime.step',request,error)
            if not error:process.session=result['current_session_id'];process.tick=result['current_tick']
            return result
        def restore(slot,existing=True):
            state=inspect()if existing else None;game=process.game()if existing else None
            result=process.rpc('save.load',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=1,revision=revision,expected_session_id=process.session if existing else None,expected_tick=process.tick if existing else None,expected_gameplay_revision=game['revision']if existing else None,expected_structure_revision=state['structure_revision']if existing else None,expected_material_revision=state['material_revision']if existing else None,expected_ui_revision=state['ui_revision']if existing else None,expected_control_sequence=state['control_sequence']if existing else None,new_session_id=fresh(),gameplay=config));process.session=result['session_id']if'session_id'in result else result['new_session_id'];process.tick=result['tick']
        def expect(label,**expected):
            v=observe(label);need(all(exact(v[k],value)for k,value in expected.items()),'Unexpected package consumer state: '+label+' '+str({k:v[k]for k in expected}));return v
        step(60);expect('initial',Magazine=4,Chamber=1,Reserve=12,AcceptedShots='0');control(503)
        # The original cover blocks the diagonal ray from spawn. Take the
        # scene's actual crouched lane and emerge beyond its lintel first.
        step(45,move=(-1,0));step(210,move=(0,1),crouch=True);step()
        actor_position=process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(2)))['world_matrix'][12:15]
        need(abs(actor_position[0]+3)<.2 and -2.3<actor_position[2]<-1.7,'Ordinary crouched lane did not clear the cover/lintel')
        # Aim at the actual scene target using native committed look, then
        # deliberately mislead presentation while paused before the first shot.
        pose=process.rpc('runtime.camera_pose',dict(session_id=process.session,tick=process.tick,entity=uid(3)))['state'];origin=pose['base_world_matrix'][12:15];target=[-3,1.3,-6];direction=[target[i]-origin[i]for i in range(3)];length=math.sqrt(sum(v*v for v in direction))
        actor=process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(2)));yaw=math.degrees(math.atan2(-direction[0],-direction[2]));pitch=math.degrees(math.asin(direction[1]/length));step(look=((yaw-actor['yaw']+180)%360-180,pitch-actor['pitch']))
        control(504);step(poses=[dict(entity=uid(3),world=dict(position=[0.0,0.0,0.0],rotation=[0.0,math.sin(.3),0.0,math.cos(.3)]))]);control(507)
        step(fire=True);first=expect('actual accepted shot',Magazine=3,Chamber=1,AcceptedShots='1',Hits=1,RecoilImpulses='1');need(first['AimPitch']>0 and first['VisualPitch']>0 and first['WeaponBack']>0,'Shot lacks independent recoil channels')
        step(6,fire=True);expect('semi held does not repeat',AcceptedShots='1',RecoilImpulses='1')
        step(reload=True);expect('reload eject begins',Phase=3,TicksRemaining=3);control(511);step(fire=True);expect('cancel eject suppresses shot',Phase=0,Magazine=3,Chamber=1,Reserve=12,AcceptedShots='1')
        step();step(reload=True);step(3);expect('ejected partial mag retained',Phase=4,TicksRemaining=4,Magazine=0,Chamber=1,Reserve=15);control(511);step();expect('cancel insert retains returned rounds',Phase=0,Magazine=0,Reserve=15)
        step(fire=True);expect('retained chamber fires',AcceptedShots='2',Magazine=0,Chamber=0);step();step(reload=True);step(3);step(4);expect('empty reload enters chamber stage',Phase=5,TicksRemaining=2,Magazine=4,Chamber=0,Reserve=11)
        control(511);step();expect('cancel chamber preserves unchambered mag',Phase=0,Magazine=4,Chamber=0,Reserve=11);step(fire=True);expect('dry fire never impulses',AcceptedShots='2',DryFires=1,RecoilImpulses='2')
        step(reload=True);expect('full mag starts chamber only',Phase=5,TicksRemaining=2);step(2);expect('one actual chamber transfer',Phase=0,Magazine=3,Chamber=1,Reserve=11)
        step(reload=True);step(3);step(4);expect('tactical reload retains chamber',Phase=0,Magazine=4,Chamber=1,Reserve=10);armed,armed_slot=snapshot('armed-full-plus-chamber')
        control(510);step(13,fire=True);expect('automatic cadence drains five rounds',FireMode=1,AcceptedShots='7',Magazine=0,Chamber=0);step(3,fire=True);expect('empty automatic produces no recoil',AcceptedShots='7',DryFires=2,RecoilImpulses='7')
        restore(armed_slot);control(510);control(510);step(fire=True);expect('burst begins',FireMode=2,AcceptedShots='3',BurstRemaining=2);step(6);expect('released burst finishes',AcceptedShots='5',BurstRemaining=0)
        step(3);step(fire=True);expect('second burst begins',AcceptedShots='6',BurstRemaining=2);control(512);step();expect('holster cancels burst',Phase=1,BurstRemaining=0,AcceptedShots='6');step(6,fire=True);expect('holstered held cannot shoot',AcceptedShots='6')
        control(513);step();expect('equip starts full duration',Phase=2,TicksRemaining=2);step(2);expect('equip completes',Phase=0,TicksRemaining=0,AcceptedShots='6')
        control(504);before=values();control(514);paused=values();need(paused['PendingCommands']==8 and paused['TickCount']==before['TickCount'],'Paused UI command advanced package timer');step(4);still=values();need(still['TickCount']==before['TickCount']and still['PendingCommands']==8,'Paused ticks consumed queued action');control(507);step();expect('queued reload applies on resumed tick',Phase=3,TicksRemaining=3,PendingCommands=0)
        step(3);step();expect('mid-insert durable state',Phase=4,TicksRemaining=3)
        count=values()['TickCount'];process.edit(TickCount=str(2**63-2));before_fault,_=snapshot('before-second-tick-fault');error=step(2,error=True);need('overflow'in error['message'].lower(),'Wrong intentional checked TickCount failure');after_fault,_=snapshot('after-second-tick-fault');pair('whole-batch-rollback',before_fault,after_fault);process.edit(TickCount=count)
        saved,slot=snapshot('cold-source-mid-reload');restore(slot);same,_=snapshot('same-owner-restored');pair('same-owner-complete',saved,same);step(5);continued,_=snapshot('same-owner-five-tick-continuation')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        process=WeaponSession(args,world,evidence);process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)));restore(slot,False);cold,_=snapshot('fresh-owner-restored');pair('cold-complete',saved,cold);step(5);cold_continued,_=snapshot('fresh-owner-five-tick-continuation');pair('cold-five-tick-complete',continued,cold_continued)
        need(world.read_bytes()==original and prepare.basis().inputs()==prepared['copied_inputs'],'Authored or historical scene changed');process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        need(len(evidence['calls'])<=450 and all(Path(p).is_file()and sha(p)==digest for p,digest in pins.items()),'Consumed source changed or RPC cap exceeded');evidence['checks']=['Ammo/chamber/reserve/accepted-shot conservation across every observed primitive state','Actual target hit uses BaseWorld plus logical recoil despite misleading presentation','Semi, automatic and released burst cadence; dry-fire and busy/holstered suppression','Interrupt eject/insert/chamber stages without duplicating rounds; explicit retained chamber','Logical pause queues without advancing timers; full-duration equip and burst cancellation','Full native rollback at second compiled Tick; same/fresh owner complete mid-reload continuation'];evidence['passed']=True
    except BaseException as error:evidence['error']=repr(error);raise
    finally:
        if process:
            try:process.close()
            except BaseException as error:evidence['close_error']=repr(error);evidence['passed']=False
        evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls'])
        if evidence['seconds']>180:evidence['passed']=False;evidence['deadline_error']='180s deadline exceeded'
        raw=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(raw)<=CAP,'32MiB evidence cap');(run/'evidence.json').write_bytes(raw)
        print(run/'evidence.json',flush=True)
        if evidence.get('close_error')and not evidence.get('error'):raise RuntimeError(evidence['close_error'])
        if evidence.get('deadline_error')and not evidence.get('error'):raise TimeoutError(evidence['deadline_error'])
if __name__=='__main__':main()
