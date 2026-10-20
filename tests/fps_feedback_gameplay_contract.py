#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compiled FPS Feedback Range: modes, native poses, rollback and full cold saves.

Consumes an already built CoreCLR sample or Native AOT descriptor. Builds nothing;
opens no renderer/audio device. An external process-tree owner is required. This
is an adapted sample workflow, not an original fresh-agent game qualification.
"""
import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import queue
import re
import subprocess
import sys
import time

from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import OwnedSession, exact, fresh, need
from switchback_oracle import read_checkpoint, exact_checkpoint

ROOT=Path(__file__).resolve().parents[1]
SAMPLE=ROOT/'examples/fps-feedback'
GAME='Poima.Examples.FpsFeedbackGame'
IDENTITY='poima.example.fps-feedback'
CAP=32*1024*1024

class RecordedSession(OwnedSession):
    """Same owned lifetime, retaining the literal emitted and received NDJSON."""
    def rpc(self,method,params=None,error=False):
        need(time.monotonic()<self.args.deadline,'Fixture deadline expired')
        request=dict(jsonrpc='2.0',id=len(self.evidence['calls'])+1,method=method,params=copy.deepcopy(params or {}))
        wire=json.dumps(request,allow_nan=False)+'\n'
        row=dict(owner_pid=self.process.pid,request=request,request_text=wire)
        self.evidence['calls'].append(row)
        self.process.stdin.write(wire);self.process.stdin.flush()
        try:line=self.output.get(timeout=min(20,max(.001,self.args.deadline-time.monotonic())))
        except queue.Empty:raise TimeoutError('Owned native RPC timed out') from None
        need(line is not None,'Native owner closed before response')
        row['response_text']=line;reply=json.loads(line);row['response']=reply
        need(reply.get('id')==request['id'],'Native RPC correlation differs')
        need(time.monotonic()<=self.args.deadline,'Native RPC exceeded deadline')
        need(len(json.dumps(self.evidence,allow_nan=False).encode())<=CAP,'Evidence exceeds32MiB')
        if error:
            need('error' in reply,'Expected native rejection')
            if type(error) is int:need(reply['error']['code']==error,'Unexpected rejection code')
            return reply['error']
        need('result' in reply and 'error' not in reply,'Native request failed: '+str(reply.get('error')))
        return reply['result']

def multiply(a,b):
    return [sum(a[k*4+r]*b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]

def local(value):
    p=value['position'];x,y,z,w=value['rotation'];norm=math.sqrt(x*x+y*y+z*z+w*w)
    x,y,z,w=x/norm,y/norm,z/norm,w/norm
    sx,sy,sz=value.get('scale',[1,1,1])
    return [(1-2*(y*y+z*z))*sx,2*(x*y+z*w)*sx,2*(x*z-y*w)*sx,0,
            2*(x*y-z*w)*sy,(1-2*(x*x+z*z))*sy,2*(y*z+x*w)*sy,0,
            2*(x*z+y*w)*sz,2*(y*z-x*w)*sz,(1-2*(x*x+y*y))*sz,0,*p,1]

def close_matrix(a,b,message):
    need(len(a)==len(b)==16 and all(type(v) in (int,float) and math.isfinite(v) for v in a+b),'Invalid matrix shape/number')
    need(max(abs(x-y) for x,y in zip(a,b))<=1e-10,message)

def motion_zero(values):
    names=('AimX','AimY','AimZ','AimVX','AimVY','AimVZ','WeaponX','WeaponY','WeaponZ','WeaponVX','WeaponVY','WeaponVZ','WeaponBack','WeaponBackVelocity','ShakeEnvelope','ShakeEnvelopeVelocity')
    return all(type(values[name]) in (int,float) and values[name]==0 for name in names)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    for name in ('hostfxr','bridge','assembly','native-descriptor'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--output',required=True,type=Path);parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    if bool(args.native_descriptor)==bool(args.hostfxr or args.bridge or args.assembly):parser.error('Supply either CoreCLR paths or one native descriptor')
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):parser.error('CoreCLR requires --hostfxr --bridge --assembly')
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    args.native=native;run=args.output.resolve();run.mkdir(parents=True,exist_ok=False)
    started=time.monotonic();args.deadline=started+180
    files=[Path(__file__).resolve(),ROOT/'tests/components_gameplay_contract.py',ROOT/'tests/input_actions_gameplay_contract.py',ROOT/'tests/character_gameplay_contract.py',ROOT/'tests/switchback_oracle.py',ROOT/'managed/Directory.Build.props',args.binary.resolve()]
    files += sorted(p for p in SAMPLE.iterdir() if p.suffix in ('.cs','.csproj','.py'))
    files += [ROOT/'packages/fps/Feedback.cs',ROOT/'packages/fps/Recoil.cs']
    if args.native_descriptor:
        descriptor=args.native_descriptor.resolve();need(descriptor.stat().st_size<=1024*1024,'Descriptor exceeds1MiB')
        metadata=json.loads(descriptor.read_bytes());need(metadata['type']==GAME and metadata['identity']==IDENTITY and metadata['services_version']==7 and metadata['minimum_services_bytes']==392 and 'camera_pose_v1' in metadata['required_features'],'Wrong native sample contract')
        files.append(descriptor)
        for row in metadata['files']:
            rel=Path(row['path']);path=(descriptor.parent/rel).resolve(strict=True)
            need(not rel.is_absolute() and '..' not in rel.parts and path.is_relative_to(descriptor.parent) and path.is_file() and path.stat().st_size==row['size'] and sha(path)==row['sha256'],'Native artifact closure differs');files.append(path)
        image=(descriptor.parent/metadata['library']).resolve(strict=True);need(image in files,'Native image missing from closure')
        config=dict(descriptor=native(descriptor),expected_descriptor_sha256=sha(descriptor))
    else:
        image=args.assembly.resolve();files += [image,args.bridge.resolve(),args.hostfxr.resolve()]
        for assembly in (image,args.bridge.resolve()):
            sdk=assembly.parent/'Poima.Gameplay.dll';need(sdk.is_file(),'Sample/bridge SDK missing');files.append(sdk)
            for suffix in ('.deps.json','.runtimeconfig.json'):
                member=assembly.with_suffix(suffix)
                if member.is_file():files.append(member)
        config=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(image),type=GAME)
    pins={str(p):sha(p) for p in set(files)}
    evidence=dict(format='poima.fps-feedback-compiled-contract.v1',passed=False,calls=[],owners=[],checks=[],checkpoints=[],source_pins=pins,
        backend='native_aot' if args.native_descriptor else 'coreclr',limitations=['CPU compiled sample with synthetic native inputs; no GPU, physical input, audio device, latency, frame performance or fresh-agent claim.','The only fault injection uses the public typed gameplay edit API to set FeedbackTicks near Int64 maximum; no source/save/cooked payload is patched.','External owner must independently prove process-tree closure.'])
    process=None;sequence=0
    try:
        spec=importlib.util.spec_from_file_location('poima_fps_feedback_prepare',SAMPLE/'prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
        prepared=prepare.prepare(run/'project');evidence['preparation']=prepared
        world=run/'project/world.json';process=RecordedSession(args,world,evidence)
        revision=process.rpc('world.inspect')['revision']
        element=dict(parent=uid(500),name='Feedback profile',kind='button',text='Feedback: Hybrid',action='feedback-mode',visible=True,enabled=True,
            layout=dict(position='absolute',left=dict(unit='dp',value=320),top=dict(unit='dp',value=145),width=dict(unit='dp',value=280),height=dict(unit='dp',value=42)),style=dict(background_color='#722f37',color='#f7e8eb',font_size=18,border_color='#722f37',border_width=1,focus=dict(border_color='#d46a7e')))
        authored=process.rpc('world.transact',dict(request_id=fresh(),base_revision=revision,ops=[dict(op='ui.element.set',id=uid(510),element=element)]))
        revision=authored['revision'];original=world.read_bytes();process.start(revision)
        loaded=process.load(config);need(loaded['module']['backend']==evidence['backend'] and loaded['module']['assembly_sha256']==sha(image),'Wrong compiled sample')
        values_schema=loaded['module']['schema']['fields'];field_names={row['name'] for row in values_schema}
        need(len(values_schema)==len(field_names)==59,'Sample must declare all59 distinct primitive fields')
        need(all(row['kind'] in ('int32','int64','float64') for row in values_schema),'Unsupported sample field kind')
        saves=run/'saves';saves.mkdir();process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)))
        def inspect():return process.rpc('runtime.inspect',dict(session_id=process.session))
        def validate_values(v):
            need(type(v) is dict and set(v)==field_names,'Incomplete sample gameplay state')
            for field in values_schema:
                name,kind=field['name'],field['kind'];value=v[name]
                if kind=='int32':need(type(value) is int and -(2**31)<=value<2**31,'Non-int32 raw field: '+name)
                elif kind=='int64':
                    need(type(value) is str and len(value)<=20 and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value) is not None,'Noncanonical int64 raw field: '+name)
                    need(-(2**63)<=int(value)<2**63,'Int64 raw field out of range: '+name)
                else:need(type(value) is float and math.isfinite(value),'Non-float64 raw field: '+name)
            return v  # Return the original values unchanged; validation never normalizes.
        def values():return validate_values(process.game()['module']['values'])
        def entity(n):return process.rpc('runtime.entity',dict(session_id=process.session,tick=process.tick,id=uid(n)))
        def pose():
            value=process.rpc('runtime.camera_pose',dict(session_id=process.session,tick=process.tick,entity=uid(3)))['state']
            need(type(value) is dict and set(value)=={'entity','world','viewmodel','base_world_matrix'} and value['entity']==uid(3),'Invalid native camera pose state shape/entity')
            for channel in ('world','viewmodel'):
                offset=value[channel];need(type(offset) is dict and set(offset)=={'position','rotation'},'Invalid native pose channel shape')
                position,rotation=offset['position'],offset['rotation']
                need(type(position) is list and len(position)==3 and all(type(v) is float and math.isfinite(v) and abs(v)<=10 for v in position),'Native pose position must contain three finite bounded doubles')
                need(type(rotation) is list and len(rotation)==4 and all(type(v) is float and math.isfinite(v) and abs(v)<=1.00001 for v in rotation) and abs(sum(v*v for v in rotation)-1)<=1e-5,'Native pose rotation must contain four finite normalized doubles')
            matrix=value['base_world_matrix']
            need(type(matrix) is list and len(matrix)==16 and all(type(v) is float and math.isfinite(v) and abs(v)<=1e12 for v in matrix),'Native BaseWorld must contain sixteen finite bounded doubles')
            need(matrix[3]==matrix[7]==matrix[11]==0.0 and matrix[15]==1.0,'Native BaseWorld must be affine')
            return value  # Preserve the original native result, including every numeric type.

        def snapshot(label):
            nonlocal sequence
            sequence+=1;slot='fps-proof-'+str(sequence);state=inspect();game=process.game()
            receipt=process.rpc('save.write',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=0,session_id=process.session,expected_tick=process.tick,expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
            row=read_checkpoint(saves,slot,receipt);validate_values(row['snapshot']['payload']['gameplay']['values'])
            evidence['checkpoints'].append(dict(label=label,slot=slot,**row));return copy.deepcopy(row),slot
        def control(n):
            state=inspect();game=process.game();result=process.rpc('runtime.ui.activate',dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=uid(n)))
            process.session=result['current_session_id'];process.tick=result['current_tick'];return result
        def step(ticks=1,move=(0,0),look=(0,0),use=False,fire=False,release=False,crouch=False,commands=None,error=False):
            records=[dict(name=name,held=held,pressed=pressed,released=released) for name,held,pressed,released in [('aim',False,False,False),('crouch',crouch,False,False),('fire',fire,fire,release),('reload',False,False,False)]]
            request=dict(session_id=process.session,request_id=fresh(),expected_tick=process.tick,expected_structure_revision=inspect()['structure_revision'],ticks=ticks,inputs=[dict(entity=uid(2),move=list(move),look=list(look),jump=False,use=use,actions=records)])
            if commands is not None:request['camera_poses']=commands
            result=process.rpc('runtime.step',request,error)
            if not error:process.session=result['current_session_id'];process.tick=result['current_tick']
            return result
        def restore(slot,existing=True):
            state=inspect() if existing else None;game=process.game() if existing else None
            result=process.rpc('save.load',dict(request_id=fresh(),configuration_generation=1,slot=slot,expected_generation=1,revision=revision,expected_session_id=process.session if existing else None,expected_tick=process.tick if existing else None,expected_gameplay_revision=game['revision'] if existing else None,expected_structure_revision=state['structure_revision'] if existing else None,expected_ui_revision=state['ui_revision'] if existing else None,expected_control_sequence=state['control_sequence'] if existing else None,expected_material_revision=state['material_revision'] if existing else None,new_session_id=fresh(),gameplay=config))
            process.session=result['session_id'] if 'session_id' in result else result['new_session_id'];process.tick=result['tick']
        def pose_join():
            p=pose();camera=entity(3);actor=entity(2)
            close_matrix(p['base_world_matrix'],multiply(actor['world_matrix'],local(camera['local_transform'])),'BaseWorld does not equal unmodified native look/stance frame')
            close_matrix(camera['world_matrix'],multiply(p['base_world_matrix'],local(p['world'])),'Presented camera does not compose committed world offset')
            return p
        step(60);need(values()['FeedbackMode']==2,'Default mode is not Hybrid')
        observed=[]
        for expected in (0,1,2):control(510);need(values()['FeedbackMode']==expected,'UI recoil mode cycle differs');observed.append(expected)
        evidence['mode_cycle']=observed;control(503);step();before=values();need(before['Phase']==1 and before['Ammo']==0 and motion_zero(before),'Initial range state differs')
        step(fire=True);after=values();need(after['Shots']==0 and after['Rejected']==before['Rejected']+1 and motion_zero(after),'Rejected empty shot emitted recoil impulse')
        step(release=True);evidence['checks'].append('Ordinary guarded mode UI cycles Authored/Seeded/Hybrid; Begin uses compiled control; empty-fire rejection produces no aim/weapon/shake impulse')
        step(45,move=(-1,0));step(150,move=(0,1),crouch=True);position=entity(2)['world_matrix'][12:15]
        need(math.hypot(position[0]+3,position[2])<.2 and abs(position[1])<.05,'Ordinary crouched route missed supply cache')
        step(crouch=True,use=True);need(values()['Supply']==1 and values()['Ammo']==4,'Ordinary crouch/use did not acquire supply')
        step(60,crouch=True);ready,ready_slot=snapshot('supplied-before-any-shot')
        modes={}
        for mode in (0,1,2):
            restore(ready_slot);control(504)
            for _ in range((mode-2)%3):control(510)
            need(values()['FeedbackMode']==mode,'Selected recoil mode differs');control(507)
            before=values();need(before['Shots']==0 and motion_zero(before),'Mode branch inherited recoil')
            origin=pose()['base_world_matrix'][12:15];target=[-3,1.3,-6];direction=[target[i]-origin[i] for i in range(3)];length=math.sqrt(sum(v*v for v in direction));player=entity(2)
            yaw=math.degrees(math.atan2(-direction[0],-direction[2]));pitch=math.degrees(math.asin(direction[1]/length));step(look=((yaw-player['yaw']+180)%360-180,pitch-player['pitch']),crouch=True)
            # Deliberately turn only the presented camera away while paused.
            # The next shot must still use the unmodified base frame plus logical
            # aim recoil, rather than visual shake/weapon presentation state.
            control(504);visual_only=dict(position=[.02,0.0,0.0],rotation=[0.0,math.sin(.25),0.0,math.cos(.25)])
            step(commands=[dict(entity=uid(3),world=visual_only)]);presentation_witness=pose_join()
            need(exact(presentation_witness['world'],visual_only),'Visual-only witness was not committed');control(507)
            pre=values();step(fire=True,crouch=True);v=values();need(v['Shots']==1 and v['Ammo']==3 and v['ClearedMask']&1,'Accepted first shot did not clear actual target A independently of visual-only camera rotation')
            need(v['Rejected']==pre['Rejected'] and v['WeaponBack']>0 and v['ShakeEnvelope']>0,'Accepted shot feedback/state differs')
            p=pose_join();need(any(abs(p['world']['rotation'][i])>1e-9 for i in range(3)) and any(abs(p['viewmodel']['rotation'][i])>1e-9 for i in range(3)),'Accepted shot has no independent world/foreground pose')
            modes[mode]=dict(values=copy.deepcopy(v),pose=p,presentation_before_shot=presentation_witness)
        gain=lambda frequency:2*math.pi*frequency*math.e/60*math.exp(-2*math.pi*frequency/60)
        for name,authored in [('AimX',.012),('AimY',-.002),('WeaponX',.040),('WeaponY',-.010),('WeaponZ',.004)]:
            frequency=6 if name.startswith('Aim') else 8;need(abs(modes[0]['values'][name]-authored*gain(frequency))<1e-12,'Authored per-shot analytic response differs: '+name)
            need(abs(modes[2]['values'][name]-modes[0]['values'][name]-modes[1]['values'][name])<1e-12,'Hybrid is not authored plus independent seeded kick: '+name)
        need(any(abs(modes[1]['values'][name])>1e-9 for name in ('AimX','AimY','WeaponX','WeaponY')),'Seeded recoil produced no variation')
        evidence['modes']=modes;evidence['checks'].append('Same saved accepted-shot ordinal/seed exercises all three modes against actual target A; authored analytic response and linear Hybrid=Authored+Seeded response verified; deliberately misleading presented camera does not redirect logical target hit; presented camera joins BaseWorld and foreground remains independent')
        step(release=True,crouch=True);evidence['bob_observations']=[]
        for movement in ((1,0),(0,0)):
            baseline=values();before_actor=entity(2)['world_matrix'][12:15]
            distance=math.hypot(before_actor[0]-baseline['PreviousPlayerX'],before_actor[2]-baseline['PreviousPlayerZ']) if baseline['HavePreviousPlayer'] and int(baseline['PreviousPlayerTick'])==process.tick-1 else 0
            expected_phase=(baseline['BobPhase']+(distance*1.6 if abs(before_actor[1])<=.08 else 0))%1
            step(move=movement,crouch=True);after_bob=values();need(abs(after_bob['BobPhase']-expected_phase)<1e-12,'Bob used requested motion instead of committed displacement')
            evidence['bob_observations'].append(dict(requested_move=list(movement),committed_distance=distance,before_phase=baseline['BobPhase'],expected_phase=expected_phase,actual_phase=after_bob['BobPhase']))
        need(evidence['bob_observations'][1]['committed_distance']>.02 and evidence['bob_observations'][1]['requested_move']==[0,0],'Bob witness lacks actual displacement with neutral current input')
        control(504);old_pose=pose();old_camera=entity(3);view=dict(position=[.02,-.01,.03],rotation=[0.0,0.0,0.0,1.0]);step(commands=[dict(entity=uid(3),viewmodel=view)])
        current_pose=pose();need(exact(current_pose['world'],old_pose['world']) and exact(current_pose['viewmodel'],view),'Foreground-only command altered world channel')
        close_matrix(entity(3)['world_matrix'],old_camera['world_matrix'],'Foreground-only effect changed logical/presented camera transform');pose_join();control(507)
        evidence['checks'].append('Bob phase forecast uses committed callback-entry displacement; paused compiled sample accepts independent foreground-only presentation update without changing camera world frame')
        actual_tick=values()['FeedbackTicks'];process.edit(FeedbackTicks=str(2**63-2));before_fault,_=snapshot('before-later-Tick-overflow')
        failed=step(2,crouch=True,error=True);need('overflow' in failed['message'].lower(),'Unexpected later-Tick failure')
        after_fault,_=snapshot('after-later-Tick-overflow');need(exact_checkpoint(before_fault,after_fault),'Later-Tick exception failed to restore complete authored/runtime/game/UI/sound/pose checkpoint')
        process.edit(FeedbackTicks=actual_tick);evidence['checks'].append('Explicit public typed Int64 boundary injection fails on second compiled Tick; complete snapshot/document state restores without omitted fields or payload repair')
        before_conflict,_=snapshot('before-caller-compiled-pose-conflict');failed=step(crouch=True,commands=[dict(entity=uid(3),world=dict(position=[0.0,0.0,0.0],rotation=[0.0,0.0,0.0,1.0]))],error=True)
        need('same camera pose' in failed['message'].lower(),'Unexpected caller/game pose conflict');after_conflict,_=snapshot('after-caller-compiled-pose-conflict');need(exact_checkpoint(before_conflict,after_conflict),'Caller/game pose conflict changed complete state')
        saved,slot=snapshot('cold-source');restore(slot);reference,_=snapshot('same-owner-restored');need(exact_checkpoint(saved,reference),'Same-owner reconstruction lost complete state')
        step(3,move=(-.25,0),crouch=True);continued,_=snapshot('same-owner-continuation');pose_join()
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        process=RecordedSession(args,world,evidence);process.rpc('save.configure',dict(request_id=fresh(),expected_generation=0,root=native(saves)));restore(slot,False)
        cold,_=snapshot('fresh-owner-restored');need(exact_checkpoint(saved,cold),'Fresh owner lost complete snapshot/document state');pose_join()
        step(3,move=(-.25,0),crouch=True);cold_continued,_=snapshot('fresh-owner-continuation');need(exact_checkpoint(continued,cold_continued),'Fresh three-Tick continuation differs in complete state');pose_join()
        need(world.read_bytes()==original,'Compiled feedback rewrote authored source');need(prepare.inputs()==prepared['copied_inputs'],'Historical source fixture changed')
        process.rpc('runtime.stop',dict(session_id=process.session));process.close();process=None
        need(all(Path(name).is_file() and sha(name)==pin for name,pin in pins.items()),'Consumed inputs changed')
        evidence['checks'].append('Caller/game duplicate pose writes roll back all domains; same-owner and fresh-process restore/three-Tick continuation compare every snapshot/document member, including all primitive feedback operands and optional native pose state')
        need(time.monotonic()<args.deadline,'Fixture deadline exceeded');evidence['passed']=True
    except BaseException as error:evidence['error']=repr(error);raise
    finally:
        if process:
            try:process.close()
            except BaseException as error:evidence['close_error']=repr(error);evidence['passed']=False
        evidence['seconds']=time.monotonic()-started;evidence['rpc_count']=len(evidence['calls'])
        if time.monotonic()>args.deadline:evidence['passed']=False;evidence['deadline_error']='Publication exceeded180s'
        data=(json.dumps(evidence,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=CAP,'Final evidence exceeds32MiB');(run/'evidence.json').write_bytes(data)
        print(run/'evidence.json',flush=True)
        if evidence.get('close_error') and not evidence.get('error'):raise RuntimeError(evidence['close_error'])
        if evidence.get('deadline_error') and not evidence.get('error'):raise TimeoutError(evidence['deadline_error'])

if __name__=='__main__':main()
