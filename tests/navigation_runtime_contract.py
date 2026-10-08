#!/usr/bin/env python3
"""Actual compiled native navigation and camera-free character-route replay.

Takes already built artifacts; does not compile, repair game state, teleport,
render, invoke a provider or replay uncertain mutations.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
TYPE='Poima.Verification.NavigationGame'
COMPONENT='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa64'
COORDINATES='00000000000000000000000000000001'
CURSOR='00000000000000000000000000000002'
ACTIONS={name:10+index for index,name in enumerate(('quota','failed_attempt','fail','clear_failure','replan','pause','resume','save','load'))}

def uid(value):return f'{value:032x}'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def normalized(value):
    if isinstance(value,dict):return {k:normalized(v) for k,v in value.items() if k!='session_id'}
    if isinstance(value,list):return [normalized(v) for v in value]
    return value

def fixture():
    ops=[]
    for identity,name,position in [(1,'Static navigation floor',[0,-.5,0]),(2,'Route-blocking cover',[0,1.5,0]),(300,'Camera-free routed NPC',[-4,0,0])]:
        ops.extend([dict(op='entity.create',id=uid(identity),name=name,parent=None),dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1]))])
    for identity,half in [(1,[10,.5,10]),(2,[.5,1.5,3])]:
        ops.append(dict(op='component.set',id=uid(identity),type='BoxCollider',value=dict(half_extents=half,motion='static',mass=1,friction=.5,restitution=0)))
    ops.append(dict(op='component.set',id=uid(300),type='CharacterController',value=dict(radius=.3,height=1.8,speed=3,jump_speed=5,camera=None)))
    ops.append(dict(op='component.set',id=uid(300),type='game:'+COMPONENT,value={COORDINATES:[],CURSOR:0}))
    for action,identity in ACTIONS.items():
        ops.append(dict(op='ui.element.set',id=uid(identity),element=dict(parent=None,name=action,kind='button',text=action,action=action,visible=True,enabled=True)))
    return ops

class OwnedGame:
    def __init__(self,args,world,label,record,deadline,native,manifest,binary=None,forbid_initialize=False):
        self.args,self.world,self.label,self.record=args,world,label,record
        self.deadline,self.native,self.manifest=deadline,native,manifest
        self.session=None;self.tick=0;self.revision=0;self.closed=False;self.calls=0
        self.stamp=world.parent/(label+'.sentinel')
        self.stamp.parent.mkdir(parents=True,exist_ok=True)
        variables={'POIMA_NAVIGATION_SENTINEL':native(self.stamp),'POIMA_NAVIGATION_FORBID_INITIALIZE':'1' if forbid_initialize else '0'}
        prior={key:os.environ.get(key) for key in (*variables,'WSLENV')}
        try:
            os.environ.update(variables)
            if args.windows_interop and os.name!='nt':
                entries=[entry for entry in (prior['WSLENV'] or '').split(':') if entry and entry.split('/',1)[0] not in variables]
                os.environ['WSLENV']=':'.join(entries+list(variables))
            self.client=WorldClient.open(str((binary or args.binary).resolve()),native(world))
        finally:
            for key,value in prior.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
    def rpc(self,method,params=None,error=None):
        left=self.deadline-time.monotonic()
        if left<=0:raise TimeoutError('Navigation qualification deadline exhausted')
        row=dict(owner=self.label,method=method,params=params or {});self.record['calls'].append(row);self.calls+=1
        try:result=self.client.call(method,params or {},timeout=min(30,left))
        except RpcError as failure:
            row['error']=dict(code=failure.code,message=failure.message)
            assert error is not None and failure.code==error,(method,row['error'],error)
            return row['error']
        row['result']=result;assert error is None,(method,result,'Expected rejection');return result
    def author(self,bind=True):
        self.rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=0,manifest=self.manifest))
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=fixture()));revision=2
        if bind:
            asset=self.rpc('world.navigation.bake',dict(revision=revision,profile=dict(radius=.3,height=1.8,climb=0)))['asset']
            self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=[dict(op='navigation.set',asset=asset)]));revision+=1
        return revision
    def start(self):
        if self.session:self.rpc('runtime.stop',dict(session_id=self.session))
        self.session=uuid.uuid4().hex;self.tick=0;self.revision=0
        self.rpc('runtime.start',dict(session_id=self.session,revision=self.rpc('world.inspect')['revision']))
        self.step(60) # Settle native capsule before initializing any game state.
    def config(self):
        if self.args.descriptor:return dict(descriptor=self.native(self.args.descriptor))
        return dict(hostfxr=self.native(self.args.hostfxr),bridge=self.native(self.args.bridge),assembly=self.native(self.args.assembly),type=TYPE)
    def load(self,error=None):
        params=dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_revision=self.module()['revision'],**self.config())
        method='runtime.gameplay.load_native' if self.args.descriptor else 'runtime.gameplay.load'
        result=self.rpc(method,params,error)
        if error is None:
            self.revision=result['revision'];assert result['module']['backend']==('native_aot' if self.args.descriptor else 'coreclr')
        return result
    def inspect(self):return self.rpc('runtime.inspect',dict(session_id=self.session))
    def module(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
    def values(self):return self.module()['module']['values']
    def entity(self,identity=300):return self.rpc('runtime.entity',dict(session_id=self.session,tick=self.tick,id=uid(identity)))
    def route(self):return self.rpc('runtime.component.get',dict(session_id=self.session,tick=self.tick,id=uid(300),type=COMPONENT))
    def step(self,count=1,error=None):
        result=self.rpc('runtime.step',dict(request_id=uuid.uuid4().hex,session_id=self.session,expected_tick=self.tick,ticks=count),error)
        if error is None:
            assert result['tick']==self.tick+count
            self.session,self.tick=result['current_session_id'],result['current_tick']
        return result
    def control(self,action):
        state=self.inspect();module=self.module()
        result=self.rpc('runtime.ui.activate',dict(request_id=uuid.uuid4().hex,session_id=self.session,expected_tick=self.tick,
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=module['revision'],
            expected_structure_revision=state['structure_revision'],id=uid(ACTIONS[action])))
        self.session,self.tick=result['current_session_id'],result['current_tick'];return result
    def snapshot(self):
        state=self.inspect();module=self.module();route=self.route()
        return normalized(dict(tick=self.tick,entity=self.entity(),route=route,values=module['module']['values'],
            backend=module['module']['backend'],gameplay_revision=module['revision'],
            revisions=dict(component_revision=route['component_revision'],**{key:state[key] for key in ('structure_revision','ui_revision','control_sequence')}),
            ui=self.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick))))
    def saves(self,path):
        path.mkdir(exist_ok=True);self.save_root=path
        self.configuration=self.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=self.native(path)))['generation']
    def write(self,slot):
        state=self.inspect();game=self.module()
        return self.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=self.configuration,slot=slot,
            expected_generation=0,session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],
            expected_component_revision=self.route()['component_revision'],expected_structure_revision=state['structure_revision'],
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
    def restore(self,slot):
        state=self.inspect() if self.session else None;game=self.module() if self.session else None
        params=dict(request_id=uuid.uuid4().hex,configuration_generation=self.configuration,slot=slot,expected_generation=1,
            revision=self.rpc('world.inspect')['revision'],expected_session_id=self.session,expected_tick=self.tick if self.session else None,
            expected_gameplay_revision=game['revision'] if game else None,expected_component_revision=self.route()['component_revision'] if state else None,
            expected_structure_revision=state['structure_revision'] if state else None,expected_ui_revision=state['ui_revision'] if state else None,
            expected_control_sequence=state['control_sequence'] if state else None,new_session_id=uuid.uuid4().hex,gameplay=self.config())
        result=self.rpc('save.load',params);self.session=params['new_session_id'];self.tick=result['tick'];self.revision=self.module()['revision']
        return result
    def close(self):
        if self.closed:return
        self.closed=True;self.client.close();code=self.client.transport.returncode
        self.record['owners'].append(dict(label=self.label,calls=self.calls,exit_code=code))
        assert code==0,('Native owner cleanup failed',self.label,code)

def position(entity):return entity['world_matrix'][12:15]
def forbidden(p):return abs(p[0])<.74 and abs(p[2])<3.24

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','manifest','output'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('hostfxr','bridge','assembly','descriptor','legacy-binary','unavailable-binary'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--timeout',type=float,default=300)
    args=parser.parse_args()
    if sys.flags.optimize:parser.error('Assertions must be enabled')
    if not math.isfinite(args.timeout) or not 30<=args.timeout<=900:parser.error('Timeout must be finite30..900seconds')
    if bool(args.descriptor)==bool(args.assembly):parser.error('Supply exactly one assembly or Native AOT descriptor')
    if not args.descriptor and not(args.hostfxr and args.bridge):parser.error('CoreCLR requires hostfxr and bridge')
    files={key:getattr(args,key) for key in ('binary','manifest','hostfxr','bridge','assembly','descriptor','legacy_binary','unavailable_binary') if getattr(args,key)}
    for key,path in files.items():
        if not path.is_file():parser.error('Missing '+key+' file')
    args.output=args.output.resolve()
    if args.output.exists():parser.error('Output must be new')
    args.output.mkdir(parents=True)
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if args.windows_interop and os.name!='nt' else value
    manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
    assert manifest['format']=='poima.components' and len(manifest['schemas'])==1 and manifest['schemas'][0]['id']==COMPONENT
    schema=manifest['schemas'][0];fields={field['id']:field for field in schema['fields']}
    assert set(fields)=={COORDINATES,CURSOR} and fields[COORDINATES]['kind']=='array' and fields[COORDINATES]['element_kind']=='float32' and fields[COORDINATES]['capacity']==30 and fields[CURSOR]['kind']=='int32'
    record=dict(passed=False,backend='native_aot' if args.descriptor else 'coreclr',checks=[],calls=[],owners=[],cleanup_errors=[],
        hashes={key:sha(path) for key,path in files.items()},sources={str(path.relative_to(ROOT)):sha(path) for path in (Path(__file__),ROOT/'tests/managed_navigation_gameplay/NavigationGame.cs')},
        limits=['Flat authored floor and single static cover; no crowd, moving obstacle, step traversal, physical input, GUI, rendering or performance proof.',
            'Tick-only SDK exposes no navigation method to Initialize(ref State) or ControlContext; actual non-Tick raw callback rejection is a separate native qualification.',
            'Before-constructor guard excludes arbitrary assembly/DLL/module initialization.',
            'Supplied actual artifacts are exercised; this runner does not qualify publication, redistribution or clean-machine deployment.'])
    deadline=time.monotonic()+args.timeout;owners=[]
    def open_game(world,label,binary=None,forbid=False):
        game=OwnedGame(args,world,label,record,deadline,native,manifest,binary,forbid);owners.append(game);return game
    def check(name):record['checks'].append(name)
    try:
        world=args.output/'world.json';game=open_game(world,'route');game.author();game.start();game.load();authored=world.read_bytes()
        game.step();values=game.values();route=game.route()['values'];points=route[COORDINATES]
        assert values['Plans']==1 and values['LastStatus']==0 and values['LastCount']>=4 and len(points)==3*values['LastCount']
        assert any(abs(points[i+2])>3.25 for i in range(0,len(points),3)),points
        p=position(game.entity());assert abs(values['LastStartX']-p[0])<.1 and abs(values['LastStartZ']-p[2])<.1
        ray=game.rpc('runtime.raycast',dict(session_id=game.session,tick=game.tick,origin=[-4,.8,0],direction=[1,0,0],distance=8,ignore=[uid(300)]))
        assert ray['hit']['entity']==uid(2),ray
        check('Actual compiled query stores a complete multi-corner route and cursor in a generated native float buffer; native ray confirms the direct shortcut is blocked.')

        game.control('quota');game.step();assert game.values()['QuotaRejected']==1
        game.step();assert game.values()['QuotaReset']==1 and game.values()['Mode']==0
        game.control('failed_attempt');game.step();assert game.values()['FailedAttempts']==1
        game.step();assert game.values()['QuotaReset']==2
        check('Eight actual native query attempts succeed; caught ninth preserves span; a failed non-character attempt consumes quota; subsequent Tick resets allowance.')

        game.control('pause');before=game.snapshot();game.step(5);after=game.snapshot()
        assert after['values']==before['values'] and after['route']['values']==before['route']['values'] and after['revisions']==before['revisions'] and after['ui']['elements']==before['ui']['elements'] and math.dist(position(after['entity']),position(before['entity']))<.001
        game.control('resume');game.step(70);assert game.values()['Arrived']==0
        if not args.descriptor:
            before=game.snapshot();game.load();after=game.snapshot()
            assert after['values']==before['values'] and after['entity']==before['entity'] and after['route']==before['route'] and after['ui']==before['ui']
            check('Compatible CoreCLR reload preserves compiled goal/timers, ordered route/cursor, native character and logical UI.')
        else:
            before=game.snapshot();game.load(error=-32060);assert game.snapshot()==before
            check('Unsupported Native AOT replacement is rejected without changing routing state.')
        game.control('fail');before=game.snapshot();game.step(6,error=-32040);assert game.snapshot()==before
        assert game.values()['FailureSentinel']==0
        game.control('clear_failure');game.step()
        check('A late compiled failure after staged route, scalar state and character input rolls back the complete multi-tick batch.')

        game.saves(args.output/'saves')
        compiled_save=game.control('save');assert compiled_save['save_serviced'] and compiled_save['save_operation']['state']==3,compiled_save
        compiled_checkpoint=game.snapshot();compiled_payloads=list((args.output/'saves/slot-navigation-room').glob('p-*.bin'))
        assert len(compiled_payloads)==1
        compiled_storage=game.rpc('save.inspect',dict(slot='navigation-room'))
        assert sha(compiled_payloads[0])==compiled_storage['selected']['sha256']
        game.step(15);compiled_load=game.control('load')
        assert compiled_load['save_serviced'] and compiled_load['runtime_replaced'] and compiled_load['save_operation']['state']==3,compiled_load
        assert game.snapshot()==compiled_checkpoint
        check('Compiled Control save/load services a real disk checkpoint and replaces the runtime with the exact persisted routing/native/UI state.')
        checkpoint=game.snapshot();written=game.write('midroute')
        payloads=list((args.output/'saves/slot-midroute').glob('p-*.bin'));assert len(payloads)==1 and sha(payloads[0])==written['sha256']
        game.step(40);continued=game.snapshot();game.restore('midroute');assert game.snapshot()==checkpoint
        game.step(40);assert game.snapshot()==continued
        game.restore('midroute');game.control('replan');game.step(15);replanned=game.snapshot()
        assert replanned['values']['Replans']==1 and replanned['values']['Plans']==2
        game.close();fresh=open_game(world,'fresh',forbid=True);fresh.saves(args.output/'saves');fresh.restore('midroute');assert fresh.snapshot()==checkpoint
        assert 'initialize' not in fresh.stamp.read_text().splitlines(),'Exact restore reexecuted Initialize'
        fresh.step(40);assert fresh.snapshot()==continued
        fresh.restore('midroute')
        for _ in range(40):fresh.step()
        assert fresh.snapshot()==continued,'Single-tick and grouped navigation movement diverged'
        fresh.restore('midroute');fresh.control('replan');fresh.step(15);assert fresh.snapshot()==replanned
        record['checkpoint']=checkpoint;record['continuation']=continued;record['immediate_replan']=replanned
        check('Disk-verified mid-route checkpoint restores complete routing/native/UI state in same and fresh owners;40-tick continuation, grouped versus individual ticks and immediate compiled replan agree exactly.')

        # Single-tick observations give a route independent of the game's own
        # claimed status and prove native movement stays outside solid cover.
        fresh.restore('midroute');positions=[]
        for _ in range(1400):
            p=position(fresh.entity());assert not forbidden(p),('NPC crossed solid cover',p)
            positions.append(p)
            if fresh.values()['Arrived']:break
            fresh.step()
        assert fresh.values()['Arrived']==1 and math.hypot(positions[-1][0]-4,positions[-1][2])<.13,positions[-1]
        assert any(abs(p[2])>3.25 for p in positions),positions
        terminal=fresh.snapshot();fresh.step(10)
        assert fresh.values()==dict(terminal['values'],Ticks=terminal['values']['Ticks']+10)
        assert fresh.route()['values']==terminal['route']['values']
        assert math.dist(position(fresh.entity()),positions[-1])<.001
        record['route_positions']=positions;record['terminal']=terminal
        assert world.read_bytes()==authored
        check('Independently observed native positions go around cover and reach the destination; completed route keeps the capsule stationary.')
        fresh.close()

        # A no-binding runtime on the SAME capable host must reject the marker
        # before game construction, not merely fail its first query.
        for label,binary,backend_absent in [('unbound',args.binary,False),('oldhost',args.legacy_binary,False),('backend-disabled',args.unavailable_binary,True)]:
            if binary is None:continue
            directory=args.output/label;directory.mkdir();negative=open_game(directory/'world.json',label,binary)
            description=negative.rpc('world.describe');assert description['runtime_available'],label+' requires a simulation-enabled host'
            if backend_absent:assert not description['navigation']['available']
            negative.author(bind=False);negative.start();before=negative.inspect();failure=negative.load(error=-32060)
            assert negative.inspect()==before and negative.module()['module'] is None
            assert not negative.stamp.exists() or not negative.stamp.read_bytes(),label+' executed game code'
            diagnostic=failure['message'].lower()
            assert 'navigation' in diagnostic or 'required gameplay service prefix' in diagnostic,failure
            negative.close();check(label+' rejects requested navigation before constructor/Initialize/Tick and leaves native runtime unchanged.')
        record['passed']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        for game in reversed(owners):
            try:game.close()
            except BaseException:record['cleanup_errors'].append(traceback.format_exc())
        record['passed']=record['passed'] and not record['cleanup_errors']
        record['rpc_count']=len(record['calls']);record['method_counts']=dict(sorted(Counter(row['method'] for row in record['calls']).items()))
        (args.output/'evidence.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=record['passed'],checks=len(record['checks']),rpc_count=record['rpc_count'],owners=len(record['owners']))))
    if not record['passed']:print(record.get('error') or record['cleanup_errors'],file=sys.stderr)
    return 0 if record['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
