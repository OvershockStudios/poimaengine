#!/usr/bin/env python3
"""Compile real C# variants, drive the native engine, and verify reload/rollback."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid
from xml.sax.saxutils import escape

parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--dotnet',type=Path,required=True);parser.add_argument('--hostfxr',type=Path,required=True);parser.add_argument('--bridge',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--reloads',type=int,default=100);parser.add_argument('--reuse-run',type=Path)
args=parser.parse_args();root=Path(__file__).resolve().parents[1];run=args.output/uuid.uuid4().hex;run.mkdir(parents=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def path(p,windows=None):
    return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if (args.windows_interop if windows is None else windows) else str(p.resolve())
def uid(n):return f'{n:032x}'
record={'run':str(run.resolve()),'binary_sha256':sha(args.binary),'bridge_sha256':sha(args.bridge),'test_sha256':sha(Path(__file__)),'builds':[],'calls':[],'reloads':args.reloads}
source=(root/'examples/managed/DoorGame/DoorGame.cs').read_text();sdk=args.bridge.parent/'Poima.Gameplay.dll'
environment={**os.environ,'DOTNET_CLI_TELEMETRY_OPTOUT':'1','DOTNET_GENERATE_ASPNET_CERTIFICATE':'false'}
def build(name,text,fail=False):
    if args.reuse_run and not fail and (args.reuse_run/name/'DoorGame.cs').is_file() and (args.reuse_run/name/'DoorGame.cs').read_text()==text:
        directory=args.reuse_run/name;cs=directory/'DoorGame.cs';result=directory/'bin/Release/net10.0/Poima.VerifyGame.dll'
        assert cs.read_text()==text and result.is_file()
        record['builds'].append({'variant':name,'reused':True,'source_sha256':sha(cs),'assembly_sha256':sha(result),'assembly':str(result.resolve())})
        return result
    directory=run/name;directory.mkdir(exist_ok=True)
    cs=directory/'DoorGame.cs';cs.write_text(text)
    project=directory/'Poima.VerifyGame.csproj'
    hint=escape(path(sdk,args.dotnet.suffix.lower()=='.exe'),{'"':'&quot;'})
    project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic></PropertyGroup><ItemGroup><Reference Include="Poima.Gameplay"><HintPath>'+hint+'</HintPath></Reference></ItemGroup></Project>')
    started=time.monotonic();p=subprocess.run([str(args.dotnet.resolve()),'build',path(project,args.dotnet.suffix.lower()=='.exe'),'-c','Release','--nologo'],capture_output=True,text=True,env=environment,timeout=120)
    elapsed=(time.monotonic()-started)*1000;record['builds'].append({'variant':name,'source_sha256':sha(cs),'exit_code':p.returncode,'milliseconds':elapsed,'output':p.stdout+p.stderr})
    assert (p.returncode!=0)==fail,p.stdout+p.stderr
    result=directory/'bin/Release/net10.0/Poima.VerifyGame.dll'
    if not fail:record['builds'][-1].update({'assembly_sha256':sha(result),'assembly':str(result.resolve())})
    return result
base=build('base',source)
changed=source.replace('public int Open,Activations;','public int Open,Activations,ReloadMarker;public float FloatProbe;public long LongProbe;').replace('state.OpenX=2.2;','state.FloatProbe=.25f;state.LongProbe=long.MaxValue;state.ReloadMarker=7;state.OpenX=9;').replace('        if(!context.Pressed','        ++state.ReloadMarker;\n        if(!context.Pressed')
upgrade=build('upgrade',changed)
bad_layout=build('bad-layout',source.replace('public double OpenX,UseDistance;','public int OpenX;public double UseDistance;').replace('state.OpenX=2.2;','state.OpenX=2;'))
bad_initialize=build('bad-initialize',source.replace('state.OpenX=2.2;state.UseDistance=3;','state.OpenX=2.2;state.UseDistance=3;throw new InvalidOperationException("intentional initialization failure");'))
fault_source=source.split('    public override void Tick(')[0]+'''    public override void Tick(ref DoorState state,GameContext context)
    {
        ++state.Activations;
        context.MoveKinematic(state.Door,new(2.2,1.5,-3),Quaternion.Identity,120);
        if(context.Tick % 10 == 3)throw new InvalidOperationException("intentional tick failure after writes and motion");
    }
}
'''
fault=build('fault',fault_source)
leak_source=source.replace('public sealed class DoorGame : Game<DoorState>\n{','public sealed class DoorGame : Game<DoorState>\n{\n    public DoorGame() { AppDomain.CurrentDomain.ProcessExit += Retained; }\n    private static void Retained(object? sender,EventArgs args) { Console.Error.WriteLine(typeof(DoorGame).FullName); }')
leak=build('retained-context',leak_source)
world=run/'world.json';stderr_path=run/'engine.stderr.log';stderr=stderr_path.open('w')
p=subprocess.Popen([str(args.binary.resolve()),'world',path(world)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,text=True,encoding='utf-8',bufsize=1)
output=queue.Queue()
def reader():
    for line in p.stdout:output.put(line)
    output.put(None)
threading.Thread(target=reader,daemon=True).start()
def rpc(method,params=None,error=None):
    request={'jsonrpc':'2.0','id':len(record['calls'])+1,'method':method,'params':params or {}};started=time.monotonic()
    p.stdin.write(json.dumps(request)+'\n');p.stdin.flush();line=output.get(timeout=60);assert line is not None,stderr_path.read_text()
    reply=json.loads(line);record['calls'].append({'request':request,'response':reply,'milliseconds':(time.monotonic()-started)*1000});assert reply['id']==request['id'],reply
    if error is not None:assert reply['error']['code']==error,reply;return reply
    assert 'result' in reply,reply;return reply['result']
session=uid(900);tick=0;revision=0
load_config={'hostfxr':path(args.hostfxr),'bridge':path(args.bridge),'type':'Poima.Examples.DoorGame'}
def inspect():return rpc('runtime.gameplay.inspect',{'session_id':session,'tick':tick})
def load(dll,error=None,values=None):
    global revision
    params={'session_id':session,'request_id':uuid.uuid4().hex,'expected_tick':tick,'expected_revision':revision,**load_config,'assembly':path(dll)}
    if values is not None:params['values']=values
    result=rpc('runtime.gameplay.load',params,error)
    if error is None:
        revision+=1;assert result['revision']==revision and result['module']['assembly_sha256']==sha(dll)
    return result,params

def step(count,inputs=None,error=None):
    global tick
    result=rpc('runtime.step',{'session_id':session,'request_id':uuid.uuid4().hex,'expected_tick':tick,'ticks':count,'inputs':inputs or []},error)
    if error is None:tick+=count
    return result

def entity(n):return rpc('runtime.entity',{'session_id':session,'tick':tick,'id':uid(n)})
try:
    fixture=json.loads((root/'examples/interaction-room.jsonl').read_text());rpc(fixture['method'],fixture['params']);saved=world.read_bytes()
    rpc('runtime.start',{'session_id':session,'revision':1});assert inspect()['module'] is None
    first,params=load(base);assert first['module']['values']['Activations']==0
    compact=inspect();assert 'schema' not in compact['module']
    selected=rpc('runtime.gameplay.inspect',{'session_id':session,'fields':['Activations']});assert selected['module']['values']=={'Activations':0}
    described=rpc('runtime.gameplay.inspect',{'session_id':session,'include_schema':True});assert described['module']['schema']==first['module']['schema']
    rpc('runtime.gameplay.inspect',{'session_id':session,'fields':['Unknown']},-32602)
    retry=rpc('runtime.gameplay.load',params);assert retry=={**first,'replayed':True}
    rpc('runtime.gameplay.load',{**params,'type':'Wrong.Type'},-32010)
    step(120);step(150,[{'entity':uid(100),'move':[0,1]}]);blocked=entity(100);assert blocked['world_matrix'][14]>-2.7
    step(1,[{'entity':uid(100),'look':[90,0],'use':True}]);assert inspect()['module']['values']['Activations']==0
    step(1,[{'entity':uid(100),'look':[-90,0]}])
    step(120,[{'entity':uid(100),'use':True}]);info=inspect();assert info['module']['values']['Activations']==1 and info['module']['values']['Open']==1
    assert abs(entity(2)['world_matrix'][12]-2.2)<1e-5
    native_before=[entity(n) for n in [2,3,100,101]]
    updated,params=load(upgrade);assert updated['module']['values']['Activations']==1 and updated['module']['values']['OpenX']==2.2 and updated['module']['values']['ReloadMarker']==7
    assert updated['module']['migration']['added']==['FloatProbe','LongProbe','ReloadMarker'];assert updated['module']['values']['FloatProbe']==.25 and updated['module']['values']['LongProbe']==str(2**63-1);assert [entity(n) for n in [2,3,100,101]]==native_before
    step(10);assert inspect()['module']['values']['ReloadMarker']==17
    edit={'session_id':session,'request_id':uuid.uuid4().hex,'expected_tick':tick,'expected_revision':revision,'values':{'UseDistance':4,'OpenX':2.5,'FloatProbe':1.5,'LongProbe':str(-2**63)}}
    edited=rpc('runtime.gameplay.edit',edit);revision+=1;assert edited['revision']==revision
    assert rpc('runtime.gameplay.edit',edit)=={**edited,'replayed':True}
    rpc('runtime.gameplay.edit',{**edit,'request_id':uuid.uuid4().hex},-32009)
    before=inspect();assert before['module']['values']['FloatProbe']==1.5 and before['module']['values']['LongProbe']==str(-2**63)
    for invalid in [{'FloatProbe':1e39},{'LongProbe':str(2**63)},{'LongProbe':10},{'Open':True}]:
        rpc('runtime.gameplay.edit',{**edit,'request_id':uuid.uuid4().hex,'expected_revision':revision,'values':invalid},-32060);assert inspect()==before
    rpc('runtime.gameplay.edit',{**edit,'request_id':uuid.uuid4().hex,'expected_revision':revision,'values':{'OpenX':5,'Unknown':3}},-32060);assert inspect()==before
    for bad in [bad_layout,bad_initialize,run/'missing.dll']:
        load(bad,error=-32060);assert inspect()==before
    # The compiler actually fails while a valid game remains loaded.
    build('broken-build',source+'\nthis is intentionally invalid C#',fail=True)
    step(1);assert inspect()['module']['values']['ReloadMarker']==18
    reload_times=[]
    for _ in range(args.reloads):
        started=time.monotonic();result,_=load(upgrade);reload_times.append((time.monotonic()-started)*1000)
        assert result['module']['values']['ReloadMarker']==18 and result['module']['values']['OpenX']==2.5
        diagnostics=rpc('runtime.gameplay.collect',{'session_id':session});assert diagnostics=={'active_modules':1,'retired_alive':0},diagnostics
    record['reload_milliseconds']=reload_times
    rpc('runtime.stop',{'session_id':session});session=uid(901);tick=revision=0;rpc('runtime.start',{'session_id':session,'revision':1})
    assert rpc('runtime.gameplay.collect',{'session_id':session})=={'active_modules':0,'retired_alive':0}
    load(fault);state_before=inspect();bodies_before=[entity(n) for n in [2,3,100,101]]
    step(10,error=-32040);assert inspect()==state_before and [entity(n) for n in [2,3,100,101]]==bodies_before
    load(base);step(10);assert entity(2)['kinematic_target'] is None
    assert world.read_bytes()==saved
    rpc('runtime.stop',{'session_id':session});session=uid(902);tick=revision=0;rpc('runtime.start',{'session_id':session,'revision':1})
    assert rpc('runtime.gameplay.collect',{'session_id':session})=={'active_modules':0,'retired_alive':0}
    load(leak);load(base);retention=rpc('runtime.gameplay.collect',{'session_id':session});assert retention=={'active_modules':1,'retired_alive':1},retention
    record['intentional_retention_probe']=retention
    record['checks']={'native_CSharp_door_interaction':True,'use_edge_once_per_batch':True,'same_tick_look_visible_to_gameplay':True,'state_preserving_reload_and_added_field':True,'new_code_executes':True,'guards_and_retry_receipts':True,'atomic_field_edit':True,'float32_and_lossless_int64_fields':True,'failed_build_load_layout_and_initialize_preserve_game':True,'managed_tick_failure_rolls_back_physics_and_state':True,'retired_contexts_collected':True,'intentional_retention_detected':True,'native_runtime_stop_releases_module':True,'authored_world_unchanged':True};record['passed']=True
finally:
    if p.poll() is None:p.stdin.close();p.wait(timeout=20)
    stderr.close();record['stderr']=stderr_path.read_text();record['exit_code']=p.returncode
    args.output.mkdir(parents=True,exist_ok=True);(args.output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'reloads':args.reloads,'checks':record['checks']}))
