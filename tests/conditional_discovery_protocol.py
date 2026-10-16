#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded CLI/MCP conditional-discovery regression; no commands on import.

Four serial original native owners, retained wires and complete descriptor
oracles derived only from full original schemas. Root supplies closed sealed
binaries and owns the outer Job/process deadline. No compiler, GPU or C#.
"""
from pathlib import Path
import argparse,copy,ctypes,hashlib,json,stat,sys,time,traceback

ROOT=Path(__file__).resolve().parents[1]

METADATA={'protocol_version','schema_revision','mode','read_only','runtime_available','session_scope','editor_discovery','unavailable_methods','unavailable_mutations','authoring_contract','runtime_game_session_contract'}
CUSTOM='0'*31+'2';FIELD='0'*31+'3';ENTITY='0'*31+'1'
SELECTORS=[('component.set','Transform'),('component.set','AnimationRig'),('component.set','game:'+CUSTOM),('component.remove','Camera'),('entity.create',None),('entity.rename',None),('entity.reparent',None),('component.set',None)]
INVALID=[dict(view='mutation',operation='missing.operation'),dict(view='mutation',operation='component.remove',type='Transform'),dict(view='mutation',operation='entity.rename',type='Camera'),dict(view='mutation',operation='component.set',type='Missing'),dict(view='section',name='runtime_game_session_contract')]


def need(v,m):
    if not v:raise AssertionError(m)


def exact(a,b):
    if type(a)is not type(b):return False
    if type(a)is dict:return a.keys()==b.keys()and all(exact(a[k],b[k])for k in a)
    if type(a)is list:return len(a)==len(b)and all(exact(a[i],b[i])for i in range(len(a)))
    return a==b


def digest(raw):return dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())


def pin(path):return digest(Path(path).read_bytes())


def write(path,value):
    raw=(json.dumps(value,indent=2,ensure_ascii=False,allow_nan=False)+'\n').encode();need(len(raw)<=8*1024**2,'Bounded retained JSON')
    with path.open('xb')as stream:stream.write(raw)
    return digest(raw)


def branches(transaction):return transaction['properties']['ops']['items']['oneOf']


def expected_selection(transaction,operation,component=None):
    # Fixed admitted native source profile: original op constants, exact type
    # constants/enums/custom-pattern only. Unknown descriptors fail honestly.
    result=[]
    for branch in branches(transaction):
        need(type(branch)is dict and branch.get('type')=='object'and not({'oneOf','anyOf','allOf','not','patternProperties','if','then','else'}&set(branch)),'Unexpected top-level branch semantics in admitted full descriptor')
        props=branch['properties'];op=props['op'];need(set(op)=={'const'}and type(op['const'])is str,'Independent plain operation witness')
        if op['const']!=operation:continue
        if component is not None:
            discriminator=props['type'];need(type(discriminator)is dict,'Independent component discriminator')
            allowed={'const','enum','pattern','type','description','title','default'};need(set(discriminator)<=allowed,'Unexpected discriminator constraint')
            if'const'in discriminator and discriminator['const']!=component:continue
            if'enum'in discriminator and component not in discriminator['enum']:continue
            if'pattern'in discriminator:
                need(discriminator['pattern']=='^game:[0-9a-f]{32}$','Known original custom pattern only')
                if not(component.startswith('game:')and len(component)==37 and all(c in'0123456789abcdef'for c in component[5:])):continue
        result.append(copy.deepcopy(branch))
    need(result,'Independent selected branch must exist');return result


def notification(transport):
    # Retain the exact notification bytes through the same exclusive stdin
    # writer. The following numbered ping proves it emitted no stray response.
    raw=b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
    transport.writer.write(raw);transport.writer.flush();transport.process.stdin.write(raw);transport.process.stdin.flush()


class Owner:
    def __init__(self,binary,mode,role,path,budget,record,Transport,strict_json):
        self.path=path;path.mkdir();self.world=path/'world.json';self.mode=mode;self.role=role;self.record=record;self.strict_json=strict_json;self.responses=[];self.last_bytes=None
        argv=[str(binary),'world',str(self.world)]if mode=='direct'else[str(binary),'mcp','--world',str(self.world)]
        self.transport=Transport(argv,path,budget,self.responses.append);record.update(role=role,mode=mode,pid=self.transport.process.pid,argv=argv,raw={key:str(value)for key,value in self.transport.paths.items()},rpc_calls=[],logical_calls=[])
        if mode=='mcp':
            result=self.rpc('initialize',dict(protocolVersion='2025-11-25',capabilities={},clientInfo=dict(name='poima-discovery-consumer',version='1')))
            need(result['protocolVersion']=='2025-11-25'and result['serverInfo']['version']==('0.0.170'if role=='baseline'else'0.0.171'),'Actual MCP build/version handshake')
            notification(self.transport);self.rpc('ping')
        self.call('world.transact',dict(request_id='a'*32,base_revision=0,ops=[dict(op='entity.create',id=ENTITY,name='Discovery invariant'),dict(op='component.schema.set',schema=dict(id=CUSTOM,name='DiscoveryCounter',version=1,fields=[dict(id=FIELD,name='Value',kind='int32',default=0)]))]))
        self.original_bytes=self.world.read_bytes();self.original_state=self.call('world.inspect');self.original_history=self.call('world.history')
        (path/'authored-original.json').write_bytes(self.original_bytes);write(path/'observed-original.json',dict(state=self.original_state,history=self.original_history))
    def rpc(self,method,params=None):
        need(len(self.record['rpc_calls'])<160,'Per-owner160request bound')
        result=self.transport.request(method,{}if params is None else params,timeout=self.transport.budget.remaining())
        raw=self.transport.paths['stdout'].read_bytes().splitlines(keepends=True)[-1];need(exact(self.strict_json(raw),self.responses[-1]),'Retained whole original response')
        self.last_bytes=len(raw);self.record['rpc_calls'].append(dict(method=method,params=copy.deepcopy({}if params is None else params),id=self.transport.counter,reply=digest(raw)))
        return result
    def call(self,method,params=None,error=None):
        params={}if params is None else params
        if self.mode=='direct':
            try:value=self.rpc(method,params);need(error is None,'Expected direct native rejection')
            except self.RpcError as failure:
                need(failure.code==error,'Expected exact native error code')
                raw=self.transport.paths['stdout'].read_bytes().splitlines(keepends=True)[-1];original=self.strict_json(raw)
                need(exact(original,self.responses[-1]),'Original direct error bytes');self.last_bytes=len(raw)
                self.record['rpc_calls'].append(dict(method=method,params=copy.deepcopy(params),id=self.transport.counter,reply=digest(raw)))
                value=original['error']
        else:
            name='poima_discover'if method=='world.describe'else'poima_call';arguments=params if name=='poima_discover'else dict(method=method,params=params)
            wrapped=self.rpc('tools/call',dict(name=name,arguments=arguments));need(set(wrapped)=={'content','structuredContent','isError'}and len(wrapped['content'])==1 and wrapped['content'][0]['type']=='text','Actual closed MCP tool envelope')
            structured=wrapped['structuredContent'];need(exact(self.strict_json(wrapped['content'][0]['text']),structured),'Complete MCP text/structured equality')
            expected='error'if error is not None else'result';need(set(structured)=={expected}and wrapped['isError']is(error is not None),'Actual MCP native result/error variant');value=structured[expected]
            if error is not None:need(type(value['code'])is int and value['code']==error,'Exact MCP native error code')
        self.record['logical_calls'].append(dict(method=method,params=copy.deepcopy(params),rpc_id=self.transport.counter,error=error is not None,reply_bytes=self.last_bytes))
        return value
    def observe(self):
        need(exact(self.call('world.inspect'),self.original_state)and exact(self.call('world.history'),self.original_history)and self.world.read_bytes()==self.original_bytes,'Whole per-owner state/history/authored-file equality')
    def close(self):
        self.observe();self.transport.close();need(self.transport.returncode==0 and not self.transport.stderr_tail and not self.transport.faults,'Actual natural owner/stream closure')
        need(self.world.read_bytes()==self.original_bytes,'EOF persistence bytes unchanged');self.record.update(closed=True,exit_code=self.transport.returncode,stderr=self.transport.stderr_tail,authored_file=pin(self.world))


def exercise(owner):
    full=owner.call('world.describe',dict(view='full'));need(full['schema_revision']==(103 if owner.role=='baseline'else 104),'Explicit original schema revisions')
    original=copy.deepcopy(full);meta={key:copy.deepcopy(value)for key,value in full.items()if key in METADATA};transaction=full['methods']['world.transact'];need(len(branches(transaction))==35,'Original admitted35-branch full union')
    write(owner.path/'full-original.json',full);owner.observe()
    catalog=owner.call('world.describe',dict(view='catalog'));sections=sorted(set(full)-METADATA-{'methods','components'})
    need(exact(catalog,dict(meta,partial=True,view='catalog',methods=sorted(full['methods']),components=sorted(full['components']),sections=sections)),'Exact independent catalog including metadata separation');owner.observe()
    for view,key,name in[('method','methods','world.transact'),('component','components','Transform'),('section','sections','invariants')]:
        expected=full[name]if view=='section'else full[key][name];actual=owner.call('world.describe',dict(view=view,name=name))
        need(exact(actual,dict(meta,partial=True,view=view,**{key:{name:expected}})),'Exact original subtree focused discovery');owner.observe()
    measurements=[]
    for operation,component in SELECTORS:
        params=dict(view='mutation',operation=operation);selection=dict(operation=operation)
        if component is not None:params['type']=selection['type']=component
        chosen=expected_selection(transaction,operation,component);expected_transaction=copy.deepcopy(transaction)
        if owner.role=='current':expected_transaction['properties']['ops']['items']['oneOf']=chosen
        unavailable=owner.role=='baseline'and operation=='component.set'and component=='AnimationRig'
        if unavailable:
            # Actual sealed170 treats this conditional value as opaque and
            # cannot establish a plain positive selector witness. Preserve that
            # exact original rejection; an error is not a35-branch schema.
            actual=owner.call('world.describe',params,error=-32602);reply_bytes=owner.last_bytes
            need(exact(actual,dict(code=-32602,message='Unknown or unavailable mutation operation or component type.')),'Exact original170 conditional-selector rejection')
            actual_count=None
        else:
            actual=owner.call('world.describe',params);reply_bytes=owner.last_bytes
            need(exact(actual,dict(meta,partial=True,view='mutation',selection=selection,methods={'world.transact':expected_transaction})),'Complete independent focused transaction/envelope oracle')
            restored=copy.deepcopy(actual['methods']['world.transact']);restored['properties']['ops']['items']['oneOf']=copy.deepcopy(branches(transaction));need(exact(restored,transaction),'Envelope and all guards preserved outside exact original selection')
            actual_count=len(branches(actual['methods']['world.transact']))
        measurements.append(dict(operation=operation,type=component,full_union_branches=35,independent_selected_branches=len(chosen),supported=not unavailable,native_error=actual if unavailable else None,actual_branches=actual_count,original_reply_bytes=reply_bytes));owner.observe()
    errors=[]
    for params in INVALID:
        problem=owner.call('world.describe',params,error=-32602);errors.append(dict(params=params,error=problem));owner.observe()
    need(exact(owner.call('world.describe',dict(view='full')),original),'Discovery source unchanged after all queries');owner.observe()
    return dict(full=full,measurements=measurements,errors=errors)


def run(args):
    root=args.repository.resolve();sys.dont_write_bytecode=True;sys.path[:0]=[str(root/'tests'),str(root/'tools/python')]
    from switchback_game_service import Budget,RetainedTransport
    from switchback_oracle import strict_json
    from poima_client.errors import RpcError
    Owner.RpcError=RpcError
    sources=[Path(__file__).resolve(),root/'tests/switchback_game_service.py',root/'tests/switchback_oracle.py']
    sources+=sorted(path for path in(root/'tools/python/poima_client').rglob('*')if path.suffix in('.py','.json')and'__pycache__'not in path.parts)
    source_before={str(path):pin(path)for path in sources}
    need(not args.output.exists(),'Fresh output only');args.output.mkdir(parents=True)
    if sys.platform=='win32':
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.GetCurrentProcess.restype=ctypes.c_void_p;kernel.IsProcessInJob.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)];inside=ctypes.c_int()
        need(kernel.IsProcessInJob(kernel.GetCurrentProcess(),None,ctypes.byref(inside))and inside.value,'External owned Windows Job required')
    started=time.monotonic();budget=Budget(started+180,[]);report=dict(passed=False,owners=[],comparisons=[],errors=[],scope='Bounded original CLI/MCP focused discovery:14reply-byte reductions and2restored conditional-selector support cases; no token pricing, latency, benchmark, renderer or Alpha claim. Original170 failed gate not rerun or regraded.');owner=None;results={}
    try:
        for mode in('direct','mcp'):
            for role,binary in(('baseline',args.baseline),('current',args.binary)):
                row={};report['owners'].append(row);owner=Owner(binary.resolve(),mode,role,args.output/(mode+'-'+role),budget,row,RetainedTransport,strict_json);results[(mode,role)]=exercise(owner);owner.close();owner=None
            old,new=results[(mode,'baseline')],results[(mode,'current')];forecast=copy.deepcopy(old['full']);forecast['schema_revision']=104
            need(exact(forecast,new['full']),'Whole old/new descriptor forecast allows only explicit schema_revision103to104 replacement')
            need(exact(old['errors'],new['errors']),'Whole unknown-selector native errors preserved')
            need(len(old['measurements'])==len(new['measurements'])==len(SELECTORS),'Exact measurement cohort')
            for index in range(len(SELECTORS)):
                a,b=old['measurements'][index],new['measurements'][index]
                need(b['supported']is True and b['native_error']is None and b['actual_branches']==b['independent_selected_branches'],'Whole independent current selector support')
                if b['operation']=='component.set'and b['type']=='AnimationRig':
                    need(a['supported']is False and a['actual_branches']is None and exact(a['native_error'],dict(code=-32602,message='Unknown or unavailable mutation operation or component type.')),'Exact original unsupported-to-supported boundary')
                    kind='support_restored' # Old error bytes are not a schema-size baseline.
                else:
                    need(a['supported']is True and a['native_error']is None and a['actual_branches']==35 and b['original_reply_bytes']<a['original_reply_bytes'],'Actual bounded same-transport response reduction')
                    kind='response_reduced'
                report['comparisons'].append(dict(mode=mode,kind=kind,operation=b['operation'],type=b['type'],baseline=a,current=b))
        for role in('baseline','current'):need(exact(results[('direct',role)]['full'],results[('mcp',role)]['full'])and exact(results[('direct',role)]['errors'],results[('mcp',role)]['errors']),'Complete same-build direct/MCP descriptor/error parity')
        report['response_reductions']=sum(row['kind']=='response_reduced'for row in report['comparisons']);report['support_restored']=sum(row['kind']=='support_restored'for row in report['comparisons'])
        need(report['response_reductions']==14 and report['support_restored']==2,'Exact14size comparisons and2restored-support comparisons')
        report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        source_after={str(path):pin(path)for path in sources};report['source_inputs']=source_before;report['source_inputs_unchanged']=exact(source_before,source_after)
        if not report['source_inputs_unchanged']:report['passed']=False;report['errors'].append('Selected fixture/helper source bytes changed during consumer')
        report['seconds']=time.monotonic()-started
        if report['seconds']>=180:report['passed']=False;report['errors'].append('Whole180-second consumer budget exceeded')
        report['binary_inputs']=dict(current=pin(args.binary),baseline=pin(args.baseline))
        count=total=0
        for path in args.output.rglob('*'):
            row=path.lstat();need(not stat.S_ISLNK(row.st_mode)and not getattr(row,'st_file_attributes',0)&0x400,'No retained output links')
            if stat.S_ISDIR(row.st_mode):continue
            need(stat.S_ISREG(row.st_mode),'No nonordinary output');count+=1;total+=row.st_size
        need(count<=4095 and total<=120*1024**2,'Reserve8MiB/onefile within128MiB4096 final cap');report['output_before_report']=dict(files=count,bytes=total)
        write(args.output/'evidence.json',report)
    print(json.dumps(dict(passed=report['passed'],owners=len(report['owners']),comparisons=len(report['comparisons']),errors=report['errors'])),flush=True)
    return 0 if report['passed']else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--repository',type=Path,default=ROOT);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--baseline',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    raise SystemExit(run(parser.parse_args()))
