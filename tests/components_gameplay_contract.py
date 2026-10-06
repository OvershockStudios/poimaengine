#!/usr/bin/env python3
"""Actual compiled C# components through native authoring/runtime/reload/save APIs."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import queue
from pathlib import Path
import subprocess
import threading
import uuid

ROOT=Path(__file__).resolve().parents[1]
HEALTH='1'*32
INTERACTION='2'*32
TYPE='Poima.Tests.ComponentGame'
def uid(value):return f'{value:032x}'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

class Session:
    def __init__(self,args,world,evidence):
        self.args,self.world,self.evidence=args,world,evidence
        self.stderr=world.with_suffix('.stderr').open('a',encoding='utf-8')
        self.process=subprocess.Popen([str(args.binary.resolve()),'world',args.native(world)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
            stderr=self.stderr,text=True,encoding='utf-8',bufsize=1)
        self.output=queue.Queue()
        def reader():
            for line in self.process.stdout:self.output.put(line)
            self.output.put(None)
        threading.Thread(target=reader,daemon=True).start()
        self.session=None;self.tick=0
    def rpc(self,method,params=None,error=False):
        request=dict(jsonrpc='2.0',id=len(self.evidence['calls'])+1,method=method,params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n');self.process.stdin.flush()
        line=self.output.get(timeout=90)
        assert line is not None,self.world.with_suffix('.stderr').read_text()
        reply=json.loads(line);self.evidence['calls'].append(dict(request=request,response=reply));assert reply['id']==request['id']
        if error:
            assert 'error' in reply,reply
            if type(error) is int:assert reply['error']['code']==error,reply
            return reply['error']
        assert 'result' in reply,reply
        return reply['result']
    def start(self,revision):
        self.session=uuid.uuid4().hex;self.tick=0
        self.rpc('runtime.start',dict(session_id=self.session,revision=revision))
    def game(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
    def load(self,config,error=False):
        return self.rpc('runtime.gameplay.load_native' if 'descriptor' in config else 'runtime.gameplay.load',
            dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_revision=self.game()['revision'],**config),error)
    def edit(self,**values):
        return self.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_revision=self.game()['revision'],values=values))
    def components(self):return self.rpc('runtime.components',dict(session_id=self.session))
    def get(self,kind=HEALTH,entity=1):return self.rpc('runtime.component.get',dict(session_id=self.session,tick=self.tick,type=kind,id=uid(entity)))
    def state(self):return dict(tick=self.tick,game=self.game(),components=self.components(),health=self.get(),interaction=self.get(INTERACTION))
    def step(self,ticks=1,error=False):
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,ticks=ticks),error)
        if not error:self.tick=result['current_tick'];self.session=result['current_session_id']
        return result
    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=20);assert self.process.returncode==0
        finally:
            if self.process.poll() is None:self.process.kill();self.process.wait(timeout=10)
            self.stderr.close()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    for name in ('manifest','hostfxr','bridge','assembly','reorder','labels','incompatible','native-descriptor'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    if not args.manifest:parser.error('--manifest required')
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):parser.error('Supply CoreCLR paths or native descriptor')
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    args.native=native
    config=(dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else
        dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type=TYPE))
    manifest=json.loads(args.manifest.read_text())
    run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    files={name:getattr(args,name) for name in ('binary','manifest','hostfxr','bridge','assembly','reorder','labels','incompatible','native_descriptor') if getattr(args,name)}
    if args.native_descriptor:
        descriptor=json.loads(args.native_descriptor.read_text());files['library']=args.native_descriptor.parent/descriptor['library']
    evidence=dict(passed=False,backend='native_aot' if args.native_descriptor else 'coreclr',hashes={k:sha(v) for k,v in files.items()},
        harness_sha256=sha(__file__),checks=[],calls=[],limitations=['Actual native-owned components and compiled gameplay; no graphics, GUI or physical input.'])
    process=None
    try:
        world=run/'world.json';process=Session(args,world,evidence)
        fixture=json.loads((ROOT/'examples/interaction-room.jsonl').read_text());process.rpc(fixture['method'],fixture['params'])
        imported=process.rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=1,manifest=manifest))
        registered=process.rpc('component.schemas')['schemas']
        assert {s['id']:s['fingerprint'] for s in registered}=={s['id']:s['fingerprint'] for s in manifest['schemas']}
        values={uid(1):100,uid(2):100,uid(3):str(-(2**63))}
        ops=[dict(op='component.set',id=uid(i),type='game:'+HEALTH,value=values) for i in (1,2,3)]
        ops.append(dict(op='component.set',id=uid(1),type='game:'+INTERACTION,value={uid(1):uid(2),uid(2):.125}))
        authored=process.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=imported['revision'],ops=ops))
        revision=authored['revision'];authored_bytes=world.read_bytes();process.start(revision)
        before_load=process.game()
        rejected=process.load(dict(config,values=dict(Selected=uid(999999))),error=True)
        assert all(text in rejected['message'] for text in ('reference','Selected',uid(999999))) and process.game()==before_load
        loaded=process.load(config)
        module=loaded['module'];assert module['backend']==evidence['backend']
        assert {s['id']:s['fingerprint'] for s in module['schema']['components']}=={s['id']:s['fingerprint'] for s in registered}
        if args.native_descriptor:assert module['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False)
        process.step();observed=process.game()['module']['values']
        assert observed['Ticks']==1 and observed['Queried']==3 and observed['Selected']==uid(1) and observed['Alive']==1 and observed['Missing']==0
        assert observed['LastHealth']==100 and observed['LastScore']==str(-(2**63)) and observed['LastWeight']==.125
        assert process.components()['component_revision']==0 and process.get()['values']==values
        before_edit=process.state()
        rejected=process.rpc('runtime.gameplay.edit',dict(session_id=process.session,request_id=uuid.uuid4().hex,
            expected_tick=process.tick,expected_revision=process.game()['revision'],
            values=dict(Selected=uid(999999),Mode=10)),error=True)
        assert all(text in rejected['message'] for text in ('reference','Selected',uid(999999))) and process.state()==before_edit
        evidence['checks'].append('Unresolved global entity references reject module activation and paused field edits without changing state or revisions; null defaults remain valid')
        process.edit(Selected=uid(3));process.step();assert process.game()['module']['values']['Missing']==1
        process.edit(Selected=uid(1),Mode=1);process.step(3)
        assert process.get()['values'][uid(1)]==97 and process.get()['values'][uid(3)]==str(-(2**63)+3)
        assert process.components()['component_revision']==3 and process.game()['module']['values']['ReadBeforeWrite']==1
        evidence['checks'].append('Generated schemas match native fingerprints; sorted paged query, all typed reads, optional absence, liveness and staged writes run through actual C# Tick')
        for mode in (2,3,4,5,6,7,10,11):
            if mode in (4,11) and process.tick%2:
                process.edit(Mode=1);process.step()  # The failure must occur after one successful tick in the same batch.
            process.edit(Mode=mode);before=process.state();process.step(2 if mode in (4,11) else 1,error=True)
            assert process.state()==before and world.read_bytes()==authored_bytes
        previous_health=process.get()['values'][uid(1)]
        process.edit(Mode=1);process.step();assert process.get()['values'][uid(1)]==previous_health-1
        evidence['checks'].append('Duplicate, explicit exception, later-tick exception, NaN, dangling component/global references and unknown entity all roll back component bytes/revision/global state/tick; later valid write recovers')
        # Paused native edits are observed by the next generated C# read.
        change=copy.deepcopy(process.get()['values']);change[uid(1)]=25
        edited=process.rpc('runtime.component.edit',dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=process.tick,
            expected_revision=process.components()['component_revision'],id=uid(1),type=HEALTH,values=change))
        process.step();assert process.game()['module']['values']['LastHealth']==25 and process.get()['values'][uid(1)]==24
        if not args.native_descriptor:
            for name in ('reorder','labels'):
                path=getattr(args,name)
                if path:
                    before=process.state();process.load(dict(config,assembly=native(path)))
                    assert process.get()['values']==before['health']['values'] and process.game()['module']['values']==before['game']['module']['values']
                    process.step();assert process.game()['module']['values']['ReadBeforeWrite']==1
            if args.incompatible:
                before=process.state();process.load(dict(config,assembly=native(args.incompatible)),error=True);assert process.state()==before
            # Restore exact original image before save binding; exercise real collectible reloads.
            for iteration in range(100):process.load(config)
            collected=process.rpc('runtime.gameplay.collect',dict(session_id=process.session))
            assert collected['active_modules']==1 and collected['retired_alive']==0,collected
            evidence['checks'].append('100 real CoreCLR reloads collect all retired modules')
            if args.reorder and args.labels:evidence['checks'].append('Compatible CLR reorder/labels preserve native values')
            if args.incompatible:evidence['checks'].append('Changed component defaults reject without swapping the live module')
        storage=run/'saves';storage.mkdir()
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(storage)))
        saved=process.state();crev=process.components()['component_revision']
        write=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='components',expected_generation=0,session_id=process.session,
            expected_tick=process.tick,expected_gameplay_revision=process.game()['revision'],expected_component_revision=crev)
        process.rpc('save.write',dict(write,expected_component_revision=crev-1),error=-32009)
        result=process.rpc('save.write',write);assert result['generation']==1
        process.step(5)
        load=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='components',expected_generation=1,revision=revision,
            expected_session_id=process.session,expected_tick=process.tick,expected_gameplay_revision=process.game()['revision'],
            expected_component_revision=process.components()['component_revision'],new_session_id=uuid.uuid4().hex)
        before=process.state();process.rpc('save.load',dict(load,expected_component_revision=load['expected_component_revision']-1),error=-32009);assert process.state()==before
        restored=process.rpc('save.load',load);process.session=load['new_session_id'];process.tick=restored['tick']
        def no_session(value):
            if isinstance(value,dict):return {k:no_session(v) for k,v in value.items() if k not in ('session_id','migration')}
            if isinstance(value,list):return [no_session(v) for v in value]
            return value
        assert no_session(process.state())==no_session(saved)
        process.step();assert process.get()['values'][uid(1)]==saved['health']['values'][uid(1)]-1
        assert process.rpc('save.write',write)==dict(result,replayed=True)
        assert process.rpc('save.load',load)==dict(restored,replayed=True)
        assert world.read_bytes()==authored_bytes
        evidence['checks'].append('Exact component membership/state/revision and C# values restore transactionally; stale component-only save/load guards reject and old receipts never replay mutation')
        process.close();process=None
        # A genuinely new owner restores from durable bytes and explicit trusted module selection.
        process=Session(args,world,evidence)
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(storage)))
        fresh=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='components',expected_generation=1,revision=revision,
            expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_component_revision=None,
            new_session_id=uuid.uuid4().hex,gameplay=config)
        restored=process.rpc('save.load',fresh);process.session=fresh['new_session_id'];process.tick=restored['tick']
        assert no_session(process.state())==no_session(saved);process.step()
        assert process.get()['values'][uid(1)]==saved['health']['values'][uid(1)]-1
        evidence['checks'].append('Fresh process restores native-owned custom components with trusted CoreCLR/NativeAOT configuration and continues generated C# writes')
        process.edit(Mode=8,SaveHigh='0',SaveLow='0',SaveSequence='0',SaveState=0)
        saving=process.step();assert saving['save_serviced'] and not saving['runtime_replaced']
        from_game=process.state();assert from_game['game']['module']['values']['SaveState']==1
        process.step(3);assert process.game()['module']['values']['SaveState']==3
        process.edit(Mode=9,SaveHigh='0',SaveLow='0',SaveSequence='0',SaveState=0)
        request=dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=process.tick,ticks=1)
        replacement=process.rpc('runtime.step',request)
        assert replacement['runtime_replaced'] and replacement['save_serviced'] and replacement['session_id']==request['session_id']
        process.session=replacement['current_session_id'];process.tick=replacement['current_tick']
        assert process.session!=request['session_id'] and no_session(process.state())==no_session(from_game)
        process.step();assert process.game()['module']['values']['SaveState']==3  # Same owner retains the completed save receipt.
        assert process.get()['values'][uid(1)]==from_game['health']['values'][uid(1)]-1
        before=process.state();assert process.rpc('runtime.step',request)==dict(replacement,replayed=True);assert process.state()==before
        assert world.read_bytes()==authored_bytes
        process.close();process=None
        process=Session(args,world,evidence)
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(storage)))
        old_ticket_load=dict(fresh,request_id=uuid.uuid4().hex,new_session_id=uuid.uuid4().hex,slot='components-game')
        restored=process.rpc('save.load',old_ticket_load);process.session=old_ticket_load['new_session_id'];process.tick=restored['tick']
        assert no_session(process.state())==no_session(from_game)
        process.step();assert process.game()['module']['values']['SaveState']==0
        assert process.get()['values'][uid(1)]==from_game['health']['values'][uid(1)]-1
        evidence['checks'].append('Actual generated C# Tick saves/restores native components through owner coordination; same-owner ticket stays terminal, fresh-owner old ticket expires, and source-step retry cannot replay the load')
        evidence['passed']=True
    except BaseException as error:
        evidence['error']=repr(error)
        raise
    finally:
        if process:
            try:process.close()
            except BaseException as close_error:
                evidence['close_error']=repr(close_error)
                evidence['passed']=False
        (run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print(run/'evidence.json',flush=True)
        if 'close_error' in evidence and 'error' not in evidence:raise RuntimeError(evidence['close_error'])

if __name__=='__main__':main()
