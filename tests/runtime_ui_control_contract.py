#!/usr/bin/env python3
"""Real compiled UI control turns, receipts, owner-serviced saves and restores."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import subprocess
import uuid
from components_gameplay_contract import Session, sha, uid


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    for name in ('hostfxr','bridge','assembly','native-descriptor'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--capture-gpu',type=int,help='Capture compiled Control before/after on this GPU with MSAA4')
    args=parser.parse_args()
    if args.capture_gpu is not None and args.capture_gpu<0:parser.error('--capture-gpu must be nonnegative')
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):
        parser.error('Supply CoreCLR paths or --native-descriptor')
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    args.native=native
    config=dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(
        hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type='Poima.Tests.ManagedUiGame')
    run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    inputs={key:getattr(args,key) for key in ('binary','hostfxr','bridge','assembly','native_descriptor') if getattr(args,key)}
    if args.native_descriptor:
        artifact=json.loads(args.native_descriptor.read_text());inputs['library']=args.native_descriptor.parent/artifact['library']
    evidence=dict(passed=False,backend='native_aot' if args.native_descriptor else 'coreclr',checks=[],calls=[],
        hashes={k:sha(v) for k,v in inputs.items()},harness_sha256=sha(__file__),
        limitations=['No physical-input or desktop playback intent application qualification.',
            'Two explicit same-tick captures only; no performance qualification.' if args.capture_gpu is not None else 'Headless: rendering is not exercised.'])
    process=None
    try:
        world=run/'world.json';process=Session(args,world,evidence)
        body='a'*32
        ops=[dict(op='entity.create',id=body,name='Falling control witness'),
             dict(op='component.set',id=body,type='Transform',value=dict(position=[0,10,0],rotation=[0,0,0,1],scale=[1,1,1])),
             dict(op='component.set',id=body,type='BoxCollider',value=dict(motion='dynamic',half_extents=[.5,.5,.5],mass=1,friction=.5,restitution=0)),
             dict(op='template.set',id='b'*32,name='Control guard prop',components=dict(Transform=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])))]
        camera='c'*32
        if args.capture_gpu is not None:
            ops.extend([dict(op='entity.create',id=camera,name='Compiled UI capture camera'),
                dict(op='component.set',id=camera,type='Transform',value=dict(position=[0,0,6],rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=camera,type='Camera',value=dict(vertical_fov=60,near=.1,far=100))])
        definitions=[(1,None,'panel','','',True,True),(2,1,'label','Original','',True,True),
            (3,1,'button','Edit','edit',True,True),(4,1,'button','Save','save',True,True),
            (5,1,'button','Resume','resume',True,True),(6,1,'button','Pause','pause',True,True),
            (7,1,'button','Throw','throw',True,True),(8,1,'button','Open modal','modal',True,True),
            (9,1,'panel','','',False,True),(10,9,'button','Close modal','clear_modal',True,True),
            (11,1,'button','Invalid edit','invalid',True,True),(12,1,'button','Disabled','resume',True,False),
            (13,1,'button','Load','load',True,True)]
        for number,parent,kind,text,action,visible,enabled in definitions:
            ops.append(dict(op='ui.element.set',id=uid(number),element=dict(parent=uid(parent) if parent else None,
                name=f'Control {number}',kind=kind,text=text,action=action or None,visible=visible,enabled=enabled)))
        process.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=ops))
        authored=world.read_bytes();process.start(1)
        discovery=process.rpc('world.describe')['methods']
        assert 'runtime.ui.activate' in discovery
        for method in ('save.write','save.load'):
            assert 'expected_control_sequence' in discovery[method]['properties']
        def ui():return process.rpc('runtime.ui.inspect',dict(session_id=process.session,tick=process.tick))
        def runtime():return process.rpc('runtime.inspect',dict(session_id=process.session))
        def values():return process.game()['module']['values']
        def entity():return process.rpc('runtime.entity',dict(session_id=process.session,id=body,tick=process.tick))
        def state():return dict(runtime=runtime(),ui=ui(),game=process.game(),entity=entity(),save=process.rpc('runtime.save.status',dict(session_id=process.session)))
        def row(number):return next(e for e in ui()['elements'] if e['id']==uid(number))
        def params(number):
            return dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=process.tick,
                expected_ui_revision=ui()['ui_revision'],expected_control_sequence=runtime()['control_sequence'],
                expected_gameplay_revision=process.game()['revision'],id=uid(number))
        def activate(number):
            request=params(number);result=process.rpc('runtime.ui.activate',request)
            process.session=result['current_session_id'];process.tick=result['current_tick']
            return request,result
        def fresh():
            process.rpc('runtime.stop',dict(session_id=process.session));process.start(1);process.load(config)
        def capture_control(name,expected_ui):
            from scene_capture import pixels
            before=state();path=run/(name+'.bmp')
            report=process.rpc('runtime.capture',dict(session_id=process.session,tick=process.tick,ui_revision=expected_ui,
                camera=camera,path=native(path),gpu=args.capture_gpu,samples=4,width=640,height=480))
            assert state()==before, 'Capturing logical UI mutated runtime state.'
            assert report['source']=='runtime' and report['session_id']==process.session and report['tick']==3
            assert report['ui_revision']==expected_ui and report['samples']==4
            assert report['hardware'] and report['capture_written'] and report['nvrhi_errors']==0,report
            image=pixels(path);assert len(image)==480 and len(image[0])==640
            evidence.setdefault('captures',{})[name]=dict(path=str(path),sha256=sha(path),report=report)
            return image
        # Absence of a module is a real native rejection, not an inert success.
        absent=state();process.rpc('runtime.ui.activate',params(5),error=-32040);assert state()==absent
        process.load(config);process.step(3);before_body=entity();before_gameplay_revision=process.game()['revision']
        if args.capture_gpu is not None:before_pixels=capture_control('control-before',0)
        request,result=activate(3)
        current=values()
        assert result['tick']==3 and result['control_sequence']==1 and result['ui_revision']==1 and result['intent']==0
        assert int(current['SeenTick'])==3 and int(current['SeenSequence'])==1 and current['ControlCalls']==1
        assert current['ReadBefore']==1 and current['ReadAfter']==1 and current['EnabledAfter']==1
        assert row(2)['text']=='Control 1' and row(3)['enabled'] is False and entity()==before_body
        if args.capture_gpu is not None:
            after_pixels=capture_control('control-after',1)
            changed=[(x,y) for y,(a,b) in enumerate(zip(before_pixels,after_pixels)) for x,(c,d) in enumerate(zip(a,b)) if c!=d]
            assert len(changed)>20, 'Compiled Control did not change visible UI pixels.'
            assert all(x<480 for x,y in changed), 'Scene background changed outside the UI region at an unchanged tick.'
            evidence['capture_comparison']=dict(gpu=args.capture_gpu,samples=4,tick=3,before_ui_revision=0,after_ui_revision=1,
                changed_pixels=len(changed),bounds=[min(x for x,y in changed),min(y for x,y in changed),max(x for x,y in changed),max(y for x,y in changed)],
                max_channel_difference=max(abs(c-d) for a,b in zip(before_pixels,after_pixels) for aa,bb in zip(a,b) for c,d in zip(aa,bb)))
            evidence['checks'].append('Real compiled Control changes named-camera UI pixels at the same simulation tick, preserving capture state and reporting zero Vulkan errors at MSAA4')
        assert process.game()['revision']==before_gameplay_revision+1
        accepted=state()
        process.rpc('runtime.gameplay.edit',dict(session_id=process.session,request_id=uuid.uuid4().hex,
            expected_tick=process.tick,expected_revision=before_gameplay_revision,values={'Mode':1}),error=-32009)
        assert state()==accepted, 'A stale same-tick gameplay edit overwrote successful Control state.'
        assert process.rpc('runtime.ui.activate',request)==dict(result,replayed=True)
        assert state()==accepted
        for update in [dict(expected_tick=2),dict(expected_ui_revision=0),dict(expected_control_sequence=0),dict(expected_gameplay_revision=0)]:
            process.rpc('runtime.ui.activate',dict(params(5),**update),error=-32009)
            assert state()==accepted
        for number in (2,3,12,99):
            process.rpc('runtime.ui.activate',params(number),error=-32040);assert state()==accepted
        process.rpc('runtime.ui.activate',dict(request,id=uid(5)),error=-32010)
        process.rpc('runtime.ui.activate',dict(request,expected_control_sequence=0.0),error=-32602)
        _,resume=activate(5);_,pause=activate(6)
        assert resume['intent']==1 and pause['intent']==2 and pause['control_sequence']==3
        assert runtime()['tick']==3 and ui()['ui_revision']==1
        later=state();assert process.rpc('runtime.ui.activate',request)==dict(result,replayed=True);assert state()==later
        evidence['checks'].append('Compiled Control reads committed UI, merges queued writes once, preserves physics/tick, advances gameplay freshness, rejects stale global edits, returns ordered intents, enforces guards and retains exact non-reexecuting receipts')
        activate(8);assert ui()['modal']==uid(9) and row(10)['eligible'] and not row(5)['eligible']
        modal=state();process.rpc('runtime.ui.activate',params(5),error=-32040);assert state()==modal
        activate(10);assert ui()['modal'] is None and not row(9)['visible'] and row(5)['eligible']
        if not args.native_descriptor:
            previous=dict(values=values(),ui=ui(),sequence=runtime()['control_sequence'])
            process.load(config)
            assert dict(values=values(),ui=ui(),sequence=runtime()['control_sequence'])==previous
        evidence['checks'].append('Real handler modal selection gates activation and clear/hide commits atomically; compatible CoreCLR reload preserves logical state when available')
        # All runtime mutation families share request identity within a session.
        # Reusing an accepted control ID must reject before any mutation occurs.
        collision=state()
        for method,payload in (
            ('runtime.ui.edit',dict(expected_tick=process.tick,expected_ui_revision=ui()['ui_revision'],edits=[])),
            ('runtime.step',dict(expected_tick=process.tick,ticks=1)),
            ('runtime.structure.transact',dict(expected_tick=process.tick,expected_structure_revision=0,despawns=[body]))):
            process.rpc(method,dict(session_id=process.session,request_id=request['request_id'],**payload),error=-32010)
            assert state()==collision
        for method,payload in (
            ('runtime.ui.edit',dict(expected_tick=process.tick,expected_ui_revision=ui()['ui_revision'],edits=[dict(id=uid(2),text='Receipt check')])),
            ('runtime.step',dict(expected_tick=process.tick,ticks=1))):
            other=uuid.uuid4().hex
            result=process.rpc(method,dict(session_id=process.session,request_id=other,**payload))
            if method=='runtime.step':process.tick=result['tick']
            before=state();process.rpc('runtime.ui.activate',dict(params(5),request_id=other),error=-32010);assert state()==before
        # A full ring keeps the latest action replayable while an evicted old
        # request reaches ordinary stale-sequence guards instead of reexecuting.
        oldest,oldest_result=activate(5)
        for _ in range(32):latest,latest_result=activate(6)
        before=state();assert process.rpc('runtime.ui.activate',latest)==dict(latest_result,replayed=True)
        process.rpc('runtime.ui.activate',oldest,error=-32009);assert state()==before
        structural_id=uuid.uuid4().hex
        process.rpc('runtime.structure.transact',dict(session_id=process.session,request_id=structural_id,
            expected_tick=process.tick,expected_structure_revision=0,spawns=[dict(template_id='b'*32)]))
        before=dict(runtime=runtime(),ui=ui(),game=process.game())
        structural_control=params(5)
        process.rpc('runtime.ui.activate',dict(structural_control,request_id=structural_id),error=-32010)
        process.rpc('runtime.ui.activate',structural_control,error=-32602)
        process.rpc('runtime.ui.activate',dict(structural_control,expected_structure_revision=0),error=-32009)
        assert dict(runtime=runtime(),ui=ui(),game=process.game())==before
        process.rpc('runtime.ui.activate',dict(structural_control,expected_structure_revision=1))
        evidence['checks'].append('Control/step/UI/structure request-ID collisions reject without mutation; bounded control receipts retain newest exact retry and reject evicted stale actions')
        # Start a clean handler state; enable owner storage before throwing Save.
        fresh();root=run/'saves';root.mkdir()
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(root)))
        before=state();files={str(p.relative_to(root)):sha(p) for p in root.rglob('*') if p.is_file()}
        failure=process.rpc('runtime.ui.activate',params(7),error=-32040)
        assert 'Control UI/global/save rollback' in failure['message'] and state()==before
        assert {str(p.relative_to(root)):sha(p) for p in root.rglob('*') if p.is_file()}==files
        process.rpc('runtime.ui.activate',params(11),error=-32040);assert state()==before
        saved_request,saved_result=activate(4)
        assert saved_result['save_serviced'] and not saved_result['runtime_replaced'] and saved_result['tick']==0
        assert saved_result['control_sequence']==1 and int(values()['SaveSequence'])==1 and values()['ControlCalls']==1
        assert row(2)['text']=='Saved control 1'
        slot=process.rpc('save.inspect',dict(slot='ui-control'));assert slot['generation']==1 and slot['selected']['verified']
        saved_values=values();saved_ui=ui();saved_body=entity();saved_sequence=runtime()['control_sequence']
        save_files={str(p.relative_to(root)):sha(p) for p in root.rglob('*') if p.is_file()}
        activate(5);advanced=state()
        assert process.rpc('runtime.ui.activate',saved_request)==dict(saved_result,replayed=True)
        assert state()==advanced and {str(p.relative_to(root)):sha(p) for p in root.rglob('*') if p.is_file()}==save_files
        evidence['checks'].append('Throwing Control rolls back UI/globals/intent/save ticket; next same-tick Save commits sequence1 and writes a verified durable generation exactly once')
        replacement=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='ui-control',expected_generation=1,
            revision=1,expected_session_id=process.session,expected_tick=0,expected_gameplay_revision=process.game()['revision'],
            expected_ui_revision=ui()['ui_revision'],expected_control_sequence=runtime()['control_sequence'],new_session_id=uuid.uuid4().hex)
        missing=dict(replacement);missing.pop('expected_control_sequence')
        process.rpc('save.load',missing,error=-32602)
        process.rpc('save.load',dict(replacement,expected_control_sequence=1),error=-32009)
        assert state()==advanced
        loaded=process.rpc('save.load',replacement);old_session=process.session;process.session=loaded['session_id'];process.tick=loaded['tick']
        assert values()==saved_values and ui()==dict(saved_ui,session_id=process.session)
        assert runtime()['control_sequence']==saved_sequence and entity()==dict(saved_body,session_id=process.session)
        restored=state();assert process.rpc('runtime.ui.activate',saved_request)==dict(saved_result,replayed=True)
        assert state()==restored
        process.rpc('runtime.ui.activate',dict(saved_request,request_id=uuid.uuid4().hex),error=-32030)
        assert process.session!=old_session
        # The handler itself can request a load. Its accepted source sequence is
        # distinct from the restored owner's current sequence in the response.
        activate(5);source_sequence=runtime()['control_sequence'];load_request,load_result=activate(13)
        assert load_result['runtime_replaced'] and load_result['save_serviced']
        assert load_result['control_sequence']==source_sequence+1 and load_result['current_control_sequence']==saved_sequence
        assert values()==saved_values and row(2)['text']=='Saved control 1'
        restored=state()
        replay=process.rpc('runtime.ui.activate',load_request)
        assert replay==dict(load_result,replayed=True) and state()==restored
        process.rpc('runtime.ui.activate',dict(load_request,request_id=uuid.uuid4().hex),error=-32030)
        evidence['checks'].append('External control-sequence restore guards reject same-tick stale state; both external and compiled-handler loads restore v5 state and reject old-session actions')
        # Explicit Tick batch remains atomic even when an earlier callback has
        # queued both UI edits and a save request.
        process.edit(Mode=2);before=state();failed=process.step(2,error=-32040)
        assert 'Later UI Tick rollback' in failed['message'] and state()==before
        assert not process.rpc('save.inspect',dict(slot='ui-tick'))['exists']
        process.edit(Mode=1);process.step(2)
        assert row(2)['text']=='Tick 2' and runtime()['control_sequence']==saved_sequence
        assert world.read_bytes()==authored
        evidence['checks'].append('Later throwing Tick rolls back prior UI/global/save work for the whole explicit batch; subsequent valid Tick UI writes recover without changing control order')
        # A separate engine process must restore using explicitly trusted code
        # paths, not paths serialized in the checkpoint.
        process.close();process=None
        process=Session(args,world,evidence)
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(root)))
        fresh_load=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='ui-control',expected_generation=1,
            revision=1,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=uuid.uuid4().hex,
            gameplay=config)
        # `values` is absent from the trusted module selection, so saved typed
        # state is authoritative during staged restore.
        loaded=process.rpc('save.load',fresh_load);process.session=loaded['session_id'];process.tick=loaded['tick']
        assert values()==saved_values and ui()==dict(saved_ui,session_id=process.session)
        assert runtime()['control_sequence']==saved_sequence
        _,continued=activate(6);assert continued['control_sequence']==saved_sequence+1 and continued['intent']==2
        assert world.read_bytes()==authored
        evidence['checks'].append('Fresh-process trusted-module restore retains compiled globals, logical UI and action sequence; the next real handler continues ordering')
        evidence['passed']=True
    finally:
        if process:process.close()
        evidence['rpc_count']=len(evidence['calls']);(run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print(run/'evidence.json')


if __name__=='__main__':main()
