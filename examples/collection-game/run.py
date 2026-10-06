#!/usr/bin/env python3
"""Run or headlessly verify the collection-room sample using the native service."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import queue
import threading
import traceback
from pathlib import Path
import subprocess
import sys
import uuid

ROOT=Path(__file__).resolve().parent
uid=lambda number:f'{number:032x}'


class Engine:
    def __init__(self,binary,project,timeout=None):
        self.project=project;self.calls=[];self.tick=0;self.session=uuid.uuid4().hex
        self.stderr=(project/'engine.stderr.log').open('a',encoding='utf-8')
        self.timeout=timeout;self.output=queue.Queue()
        try:
            self.process=subprocess.Popen([str(binary.resolve()),'world',str((project/'world.json').resolve())],stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,stderr=self.stderr,text=True,encoding='utf-8',bufsize=1)
        except BaseException:
            self.stderr.close();raise
        def read():
            try:
                for line in self.process.stdout:self.output.put(line)
            except BaseException as error:self.output.put(error)
            finally:self.output.put(None)
        threading.Thread(target=read,daemon=True).start()
    def rpc(self,method,params=None):
        request=dict(jsonrpc='2.0',id=len(self.calls)+1,method=method,params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n');self.process.stdin.flush()
        try:line=self.output.get(timeout=self.timeout)
        except queue.Empty:raise TimeoutError(f'Native service timed out while handling {method}.') from None
        if line is None:raise RuntimeError('Native service ended; see engine.stderr.log')
        if isinstance(line,BaseException):raise RuntimeError('Native output reader failed.') from line
        response=json.loads(line);self.calls.append(dict(request=request,response=response))
        if not isinstance(response,dict) or response.get('jsonrpc')!='2.0' or type(response.get('id')) is not int or response['id']!=request['id']:
            raise RuntimeError('Native response does not match the JSON-RPC request.')
        if 'error' in response:raise RuntimeError(str(response['error']))
        return response['result']
    def inspect(self):return self.rpc('runtime.inspect',dict(session_id=self.session))
    def values(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick))['module']['values']
    def step(self,ticks=1,move=None,use=False):
        inputs=[] if move is None and not use else [dict(entity=uid(100),move=move or [0,0],use=use)]
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,
            expected_structure_revision=self.inspect()['structure_revision'],ticks=ticks,inputs=inputs))
        self.tick=result['current_tick'];self.session=result['current_session_id'];return result
    def control(self,number):
        state=self.inspect();game=self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick))
        result=self.rpc('runtime.ui.activate',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=uid(number)))
        self.tick=result['current_tick'];self.session=result['current_session_id'];return result
    def entity(self,number):return self.rpc('runtime.entity',dict(session_id=self.session,tick=self.tick,id=uid(number)))
    def close(self):
        errors=[]
        try:
            try:self.process.stdin.close()
            except BaseException as error:errors.append(f'stdin close: {error}')
            try:self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                errors.append('Native service did not exit within 15 seconds.')
                self.process.kill();self.process.wait(timeout=10)
            if self.process.returncode:errors.append(f'Engine exit: {self.process.returncode}')
        finally:
            try:self.stderr.close()
            except BaseException as error:errors.append(f'stderr close: {error}')
        if errors:raise RuntimeError('; '.join(errors))


def restored_ui(engine):
    state=engine.values()
    ui=engine.rpc('runtime.ui.inspect',dict(session_id=engine.session,tick=engine.tick))
    text=next(row['text'] for row in ui['elements'] if row['id']==uid(3))
    assert state['SavePending']==0 and state['Status']==3 and text=='Checkpoint loaded.',(state,text)


def finish_room(engine):
    for expected in (2,3):
        # Walk around solid pedestals, then face the next cell down the corridor.
        engine.step(30,move=[1,0]);engine.step(60,move=[0,1]);engine.step(30,move=[-1,0])
        engine.step(1,use=True)
        current=engine.values();assert current['Collected']==expected,(current,engine.entity(100))
    ui=engine.rpc('runtime.ui.inspect',dict(session_id=engine.session,tick=engine.tick))
    progress=next(row['text'] for row in ui['elements'] if row['id']==uid(2))
    assert progress=='All 3 cells collected. Room complete!',progress

def verify(engine,capture=None):
    checks=[]
    engine.step(1)
    initial=engine.values();assert initial['Started']==1 and initial['Collected']==0
    assert all(initial[name] not in (None,'0'*32) for name in ('First','Second','Third'))
    if capture:capture(engine,'initial')
    engine.step(1,use=True)
    one=engine.values();assert one['Collected']==1 and one['First'] in (None,'0'*32),one
    engine.step(1,use=True);assert engine.values()['Collected']==1,'Out-of-range cell collected.'
    checks.append('Real raycast/Use removes exactly one spawned collectible; another Use cannot collect out of range.')
    paused=engine.control(11);assert paused['intent']==2
    saved=engine.control(13);assert saved['save_serviced'] and saved['save_operation']['state']==3,saved
    saved_tick=engine.tick;saved_position=engine.entity(100)['world_matrix']
    slot=engine.rpc('save.inspect',dict(slot='collection-room'));assert slot['selected']['verified'] and slot['generation']==1
    finish_room(engine);completed=engine.tick
    if capture:capture(engine,'completed')
    checks.append('Actual CharacterController movement and camera queries collect all three cells; native UI reports completion.')
    restored=engine.control(14)
    assert restored['runtime_replaced'] and engine.tick==saved_tick,restored
    state=engine.values();assert state['Collected']==1 and state['First'] in (None,'0'*32),state
    assert state['Second']==one['Second'] and state['Third']==one['Third']
    assert engine.entity(100)['world_matrix']==saved_position
    resumed=engine.control(12);assert resumed['intent']==1 and engine.tick==saved_tick
    restored_ui(engine)
    finish_room(engine)
    checks.append('Compiled Load restores count, live spawned handles and exact player transform; restored gameplay finishes again.')
    return dict(checks=checks,first_completion_tick=completed,restored_tick=saved_tick,saved_position=saved_position,final_tick=engine.tick,final_values=engine.values())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','project'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('hostfxr','bridge','assembly','native-descriptor'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--capture',action='store_true',help='With --verify, retain initial/completed renderer readbacks on --gpu.')
    parser.add_argument('--verify',action='store_true',help='Use a new project directory; script actual movement, collection and save/load without graphics.')
    parser.add_argument('--gpu',type=int,default=-1)
    args=parser.parse_args();project=args.project.resolve()
    if sys.flags.optimize:parser.error('Run this sample without -O/-OO/PYTHONOPTIMIZE; verification requires active assertions.')
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):parser.error('Supply --hostfxr/--bridge/--assembly or --native-descriptor.')
    if args.capture and not args.verify:parser.error('--capture requires --verify.')
    if args.verify and project.exists():parser.error('--verify requires a new project directory to preserve existing saves.')
    project.mkdir(parents=True,exist_ok=True);fresh=not (project/'world.json').exists()
    evidence=dict(passed=False,mode='headless_scripted' if args.verify else 'interactive',calls=[],physical_input_qualified=False,
        hashes={},captures=[],cleanup_errors=[],runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    engine=None;owned=[];work_complete=False
    def close_owned(instance):
        item=next(item for item in owned if item['instance'] is instance)
        if item['cleanup_attempted']:return
        item['cleanup_attempted']=True
        try:instance.close()
        except BaseException:
            evidence['cleanup_errors'].append(dict(process=item['label'],error=traceback.format_exc()))
            raise
    try:
        evidence['hashes']={name:hashlib.sha256(getattr(args,name).read_bytes()).hexdigest()
            for name in ('binary','bridge','assembly','native_descriptor') if getattr(args,name)}
        def start(fresh):
            instance=Engine(args.binary,project,timeout=90 if args.verify else None)
            owned.append(dict(instance=instance,label=len(owned)+1,cleanup_attempted=False))
            if fresh:
                authored=json.loads((ROOT/'world.jsonl').read_text(encoding='utf-8'))
                instance.rpc(authored['method'],authored['params'])
            revision=instance.rpc('world.inspect')['revision']
            instance.rpc('runtime.start',dict(session_id=instance.session,revision=revision))
            config=dict(descriptor=str(args.native_descriptor.resolve())) if args.native_descriptor else dict(
                hostfxr=str(args.hostfxr.resolve()),bridge=str(args.bridge.resolve()),assembly=str(args.assembly.resolve()),type='Poima.Examples.CollectionGame')
            instance.rpc('runtime.gameplay.load_native' if args.native_descriptor else 'runtime.gameplay.load',dict(session_id=instance.session,
                request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**config))
            saves=project/'saves';saves.mkdir(exist_ok=True)
            instance.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(saves)))
            return instance
        def capture(instance,name):
            path=project/(name+'.bmp');state=instance.inspect()
            report=instance.rpc('runtime.capture',dict(session_id=instance.session,tick=instance.tick,ui_revision=state['ui_revision'],
                camera=uid(101),path=str(path),width=1280,height=720,gpu=args.gpu,samples=1))
            assert report['capture_written'] and report['nvrhi_errors']==0,report
            evidence['captures'].append(dict(name=name,path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),report=report,kind='renderer_readback'))
        engine=start(fresh)
        if args.verify:
            evidence.update(verify(engine,capture if args.capture else None))
            evidence['first_process_calls']=engine.calls
            old=engine;engine=None;close_owned(old)
            engine=start(False)
            restored=engine.control(14)
            assert restored['runtime_replaced'] and engine.tick==evidence['restored_tick'],restored
            assert engine.values()['Collected']==1 and engine.entity(100)['world_matrix']==evidence['saved_position']
            resumed=engine.control(12);assert resumed['intent']==1
            restored_ui(engine)
            finish_room(engine)
            evidence['checks'].append('A fresh native process loads the same durable checkpoint and completes the room again.')
            evidence['reopened_final_values']=engine.values()
        else:
            report=engine.rpc('runtime.play',dict(session_id=engine.session,request_id=uuid.uuid4().hex,expected_tick=0,
                controller=uid(100),camera=uid(101),mode='interactive',gpu=args.gpu,width=1280,height=720))
            assert report['success'],report;evidence['player']=report
        work_complete=True
    except BaseException:
        evidence['error']=traceback.format_exc()
    finally:
        for item in reversed(owned):
            try:close_owned(item['instance'])
            except BaseException:pass  # The cleanup error is retained separately.
        if engine:evidence['calls']=engine.calls
        evidence['processes']=[dict(label=item['label'],pid=item['instance'].process.pid,
            exit_code=item['instance'].process.returncode,calls=item['instance'].calls) for item in owned]
        evidence['passed']=work_complete and 'error' not in evidence and not evidence['cleanup_errors']
        (project/'sample-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=evidence['passed'],evidence=str(project/'sample-evidence.json'),checks=evidence.get('checks',[]))))
    if not evidence['passed']:print(evidence.get('error') or evidence['cleanup_errors'],file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
