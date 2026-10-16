#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows snapshot publication failures, exact recovery and cold continuation.

Requires an external owned Windows Job. Original wires and whole snapshot bytes
are retained. No retry follows an unknown outcome or an unexpected stress error.
"""
from pathlib import Path
from ctypes import wintypes as w
import argparse,copy,ctypes,hashlib,json,os,re,stat,sys,time,traceback
ROOT=Path(__file__).resolve().parents[1]
SUFFIXES=('', '.pending', '.previous', '.previous.pending')
ERROR={'code':-32000,'message':'World storage failure; inspect stderr.'}
LIMIT=64*1024**2;FILE_LIMIT=4096;MAX_CALLS=512
ENTITY='0'*31+'1';STRESS_ENTITY='0'*31+'2'

def need(value,message):
    if not value:raise AssertionError(message)
def exact(a,b):
    if type(a)is not type(b):return False
    if type(a)is dict:return a.keys()==b.keys()and all(exact(a[k],b[k])for k in a)
    if type(a)is list:return len(a)==len(b)and all(exact(a[i],b[i])for i in range(len(a)))
    return a==b
def parse(raw):
    def pairs(rows):
        result={}
        for k,v in rows:need(k not in result,'Duplicate JSON member');result[k]=v
        return result
    def invalid(_):raise AssertionError('Nonfinite JSON')
    return json.loads(raw,object_pairs_hook=pairs,parse_constant=invalid)
def digest(raw):return {'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
def encoded(value):return (json.dumps(value,indent=2,sort_keys=True,ensure_ascii=False,allow_nan=False)+'\n').encode('utf-8')
def ordinary(path):
    s=path.lstat();need(stat.S_ISREG(s.st_mode)and not stat.S_ISLNK(s.st_mode)and not getattr(s,'st_file_attributes',0)&0x400,'Ordinary file required');return s
def filebytes(path,limit=16*1024**2):
    s=ordinary(path);need(s.st_size<=limit,'Individual retained-file bound');raw=path.read_bytes();t=ordinary(path);need((s.st_ino,s.st_size,s.st_mtime_ns)==(t.st_ino,t.st_size,t.st_mtime_ns),'Input changed while reading');return raw
def census(root):
    count=total=0
    for p in root.rglob('*'):
        s=p.lstat();need(not stat.S_ISLNK(s.st_mode)and not getattr(s,'st_file_attributes',0)&0x400,'No output links')
        if stat.S_ISDIR(s.st_mode):continue
        need(stat.S_ISREG(s.st_mode),'No nonordinary output');count+=1;total+=s.st_size
    need(count<=FILE_LIMIT-1 and total<=LIMIT-8*1024**2,'Reserve evidence within64MiB4096-file output');return {'files':count,'bytes':total}
def write(path,value):
    raw=encoded(value);need(len(raw)<=8*1024**2,'Evidence8MiB bound')
    with path.open('xb')as f:f.write(raw)
def pin(path):return digest(filebytes(path,128*1024**2))
def document(world):return parse(filebytes(world))
def forecast(before,params):
    need(set(params)=={'request_id','base_revision','ops'}and type(params['base_revision'])is int and params['base_revision']==before['revision'],'Exact predeclared transaction')
    result=copy.deepcopy(before);changed=[]
    for op in params['ops']:
        entity=op['id'];changed.append(entity)
        if op['op']=='entity.create':
            need(set(op)=={'op','id','name','parent'}and entity not in result['entities'],'Closed create forecast')
            result['entities'][entity]={'name':op['name'],'parent':op['parent'],'components':{'Transform':{'position':[0,0,0],'rotation':[0,0,0,1],'scale':[1,1,1]}}}
        else:
            need(op['op']=='entity.rename'and set(op)=={'op','id','name'}and entity in result['entities'],'Closed rename forecast');result['entities'][entity]['name']=op['name']
    result['revision']+=1;receipt={'revision':result['revision'],'committed':True,'replayed':False,'changed_ids':sorted(set(changed)),'history_recorded':True}
    normalized=copy.deepcopy(params);normalized['preview']=False
    result['receipts']=(result['receipts']+[{'params':normalized,'result':copy.deepcopy(receipt)}])[-128:]
    return result,receipt

def transaction(revision,name,request,entity=ENTITY,create=False):return {'request_id':f'{request:032x}','base_revision':revision,'ops':[dict(op='entity.create',id=entity,name=name,parent=None)if create else dict(op='entity.rename',id=entity,name=name)]}

class Windows:
    def __init__(self):
        self.k=ctypes.WinDLL('kernel32',use_last_error=True);k=self.k
        k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,ctypes.c_void_p,w.DWORD,w.DWORD,w.HANDLE];k.CreateFileW.restype=w.HANDLE
        k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
        k.MoveFileExW.argtypes=[w.LPCWSTR,w.LPCWSTR,w.DWORD];k.MoveFileExW.restype=w.BOOL
        k.GetCurrentProcess.restype=w.HANDLE;k.IsProcessInJob.argtypes=[w.HANDLE,w.HANDLE,ctypes.POINTER(w.BOOL)];k.IsProcessInJob.restype=w.BOOL
        inside=w.BOOL();need(k.IsProcessInJob(k.GetCurrentProcess(),None,ctypes.byref(inside))and inside.value,'External preassigned Windows Job required')
    def lock(self,path):
        ctypes.set_last_error(0);h=self.k.CreateFileW(str(path),0x80000000,3,None,3,0x80,None);need(h!=w.HANDLE(-1).value,'Cannot acquire owned read/write-shared deny-delete handle');return h
    def release(self,h):need(self.k.CloseHandle(h),'Cannot release owned deny-delete handle')
    def oracle(self,root,stage):
        source=root/('oracle-source-'+stage+'.bin');target=root/('oracle-target-'+stage+'.bin');source.write_bytes(b'candidate\n');target.write_bytes(b'original\n');h=self.lock(target)
        try:
            ctypes.set_last_error(0);ok=self.k.MoveFileExW(str(source),str(target),1|8);code=ctypes.get_last_error();need(not ok and type(code)is int and 0<=code<=0xffffffff,'Independent owned MoveFileExW must fail with its actual unsigned32bit error code');need(source.read_bytes()==b'candidate\n'and target.read_bytes()==b'original\n','Independent failed rename changed complete files')
        finally:self.release(h)
        return {'operation':'MoveFileExW','flags':9,'succeeded':False,'win32_code':code,'scope':'Independent owned ordinary files; same-directory deny-delete failure, not interception of engine GetLastError.'}

class Capture:
    def __init__(self,root):self.root=root;self.blobs=root/'blobs';self.blobs.mkdir();self.snapshots=[]
    def snap(self,world,label):
        raw={};record={'label':label,'files':{}}
        for suffix in SUFFIXES:
            p=Path(str(world)+suffix)
            try:b=filebytes(p)
            except FileNotFoundError:raw[suffix]=None;record['files'][suffix]={'exists':False};continue
            raw[suffix]=b;identity=digest(b);blob=self.blobs/(identity['sha256']+'.bin')
            if blob.exists():need(blob.read_bytes()==b,'CAS byte collision')
            else:
                census(self.root)
                with blob.open('xb')as f:f.write(b)
            record['files'][suffix]={'exists':True,'identity':identity,'blob':blob.relative_to(self.root).as_posix()}
        self.snapshots.append(record);return raw,record
    def expect(self,world,label,expected):
        actual,record=self.snap(world,label);need(exact(actual,expected),'Whole four-sidecar byte forecast mismatch: '+label);return record

class Owner:
    def __init__(self,binary,path,world,role,report,budget,Transport,RpcError,capture):
        self.transport=None;self.close_attempted=False;self.responses=[];self.rpc_error=RpcError;self.capture=capture;self.world=world;self.report=report;self.role=role;self.budget=budget
        path.mkdir();self.row={'role':role,'calls':[],'closed':False,'exit_code':None};report['owners'].append(self.row)
        self.transport=Transport([str(binary),'world',str(world)],path,budget,self.responses.append);self.row.update(pid=self.transport.process.pid,raw={k:str(p)for k,p in self.transport.paths.items()})
    def call(self,method,params=None,error=None):
        need(sum(len(o['calls'])for o in self.report['owners'])<MAX_CALLS,'Whole512RPC cap');self.budget.remaining();params={}if params is None else params
        row={'method':method,'params':copy.deepcopy(params),'rpc_id':self.transport.counter+1,'outcome':'unsent'};self.row['calls'].append(row)
        try:value=self.transport.request(method,params,timeout=self.budget.remaining());row.update(outcome='result',result=value)
        except self.rpc_error:
            value=copy.deepcopy(self.responses[-1]['error']);row.update(outcome='error',error=value)
        except BaseException:row['outcome']='unknown';raise
        need((row['outcome']=='error')==(error is not None),'Unexpected native result/error; no automatic retry')
        if error is not None:need(exact(value,error),'Exact original public error contract')
        return value
    def observe(self,label):
        value={'inspect':self.call('world.inspect'),'history':self.call('world.history'),'entity':self.call('entity.get',{'id':ENTITY}),'query':self.call('entity.query')}
        self.row.setdefault('observations',[]).append({'label':label,'value':value});return value
    def commit(self,params,before,label):
        expected,result=forecast(before,params);old=self.capture.snap(self.world,label+'-before')[0]
        try:actual=self.call('world.transact',params)
        except BaseException:
            failure={'label':label,'snapshot':self.capture.snap(self.world,label+'-unexpected-failure')[1]};self.row.setdefault('unexpected_commit_failures',[]).append(failure)
            if self.row['calls'][-1]['outcome']in('error','result')and not self.transport.broken and not self.transport.faults:
                try:failure['native_observation']=self.observe(label+'-unexpected-failure')
                except BaseException:failure['observation_error']=traceback.format_exc()
            raise
        need(exact(actual,result),'Whole independently forecast transaction result')
        after={'':encoded(expected),'.pending':None,'.previous':old[''],'.previous.pending':None};self.capture.expect(self.world,label+'-after',after);need(exact(document(self.world),expected),'Whole typed authored-document forecast');return expected,result
    def close(self):
        if self.transport is None:return
        need(not self.close_attempted,'Unknown prior closure; no repeat');self.close_attempted=True;self.transport.close();self.row.update(closed=True,exit_code=self.transport.returncode,stderr=self.transport.stderr_tail,stream_threads_closed=all(not t.is_alive()for t in self.transport.threads),faults=list(self.transport.faults));need(self.row['exit_code']==0 and self.row['stream_threads_closed']and self.row['faults']==[],'Actual EOF/stream closure')

def exercise_failure(owner,win,stage,request,doc,oracle_code):
    before=owner.observe(stage+'-before');files,before_record=owner.capture.snap(owner.world,owner.role+'-'+stage+'-before');candidate,result=forecast(doc,request)
    need(files['']==encoded(doc),'Complete original primary serialization');target=owner.world if stage=='primary_publish'else Path(str(owner.world)+'.previous');need(target.exists(),'Existing destination required')
    h=win.lock(target);attempt={'owner':owner.role,'stage':stage,'request':copy.deepcopy(request),'before':before_record,'lock_released':False};owner.report['forced_failures'].append(attempt)
    try:
        problem=owner.call('world.transact',request,error=ERROR);attempt['native_error']=problem
        original_lines=owner.transport.paths['stdout'].read_bytes().splitlines(keepends=True);need(len(original_lines)==owner.transport.counter,'Exact retained error wire population')
        original=original_lines[-1];need(exact(parse(original),{'jsonrpc':'2.0','id':owner.transport.counter,'error':ERROR}),'Whole literal original public error response')
        attempt['raw_error_reply_hex']=original.hex();attempt['raw_error_reply']=digest(original)
        expected=copy.deepcopy(files);expected['.pending']=encoded(candidate)
        if stage=='primary_publish':expected['.previous']=files[''];expected['.previous.pending']=None
        else:expected['.previous.pending']=files['']
        attempt['after']=owner.capture.expect(owner.world,owner.role+'-'+stage+'-failed',expected)
        need(exact(document(owner.world),doc),'Failed publication changed complete original primary document/receipts');after=owner.observe(stage+'-failed');need(exact(before,after),'Failed publication changed complete native entity/query/inspect/history')
    finally:win.release(h);attempt['lock_released']=True
    # Only known -32000 plus all complete observations permit this exact retry.
    committed,receipt=owner.commit(request,doc,owner.role+'-'+stage+'-recovered');state=owner.observe(stage+'-recovered');stable=owner.capture.snap(owner.world,owner.role+'-'+stage+'-before-receipt')[0]
    replay=owner.call('world.transact',request);expected_replay=copy.deepcopy(receipt);expected_replay['replayed']=True;need(exact(replay,expected_replay),'Exact retry receipt; transaction applied only once');need(exact(owner.observe(stage+'-replayed'),state),'Receipt replay changed complete native state/history');owner.capture.expect(owner.world,owner.role+'-'+stage+'-after-receipt',stable)
    attempt.update(recovered_result=receipt,replayed_result=replay,recovered_observation=state,oracle_win32_code=oracle_code);return committed,request,receipt

def run(args):
    need(os.name=='nt'and not sys.flags.optimize,'Actual nonoptimized Windows Python required');root=args.repository.resolve();sys.dont_write_bytecode=True;sys.path[:0]=[str(root/'tests'),str(root/'tools/python')]
    import switchback_game_service as transport
    from poima_client.errors import RpcError
    transport.LOG_LIMIT=16*1024**2 # Per retained stream; original transport otherwise unchanged.
    win=Windows();need(not args.output.exists(),'Fresh output required');args.output.mkdir(parents=True);capture=Capture(args.output)
    sources=[Path(__file__).resolve(),root/'tests/switchback_game_service.py',root/'tests/switchback_oracle.py']+sorted(p for p in(root/'tools/python/poima_client').rglob('*')if p.suffix in('.py','.json')and'__pycache__'not in p.parts)
    pins={str(p):pin(p)for p in sources};binaries={'current':pin(args.binary)}
    if args.baseline:binaries['baseline']=pin(args.baseline)
    started=time.monotonic();budget=transport.Budget(started+180,[]);report={'format':'poima.world-storage-protocol.v1','passed':False,'owners':[],'forced_failures':[],'stress':{'passed':False,'commits':0,'scope':'New current-only131additional commits on a second entity after cold reopen; not an exact replay of the historical130-commit probe.'},'errors':[],'scope':'Real owned Windows deny-delete failures and exact snapshot/native recovery; no power-loss durability, external failure cause, broad filesystem atomicity or performance claim.'};owner=None
    try:
        oracles={stage:win.oracle(args.output,stage)for stage in('primary_publish','previous_publish')};report['independent_win32_oracles']=oracles
        for role,binary in ([('baseline',args.baseline)]if args.baseline else[])+[('current',args.binary)]:
            directory=args.output/role;directory.mkdir();world=directory/'世界 scene.json';owner=Owner.__new__(Owner);owner.transport=None;owner.close_attempted=False;Owner.__init__(owner,binary.resolve(),directory/'warm',world,role+'-warm',report,budget,transport.RetainedTransport,RpcError,capture)
            initial=owner.call('world.inspect');need(initial['revision']==0 and initial['persisted']is False,'Actual fresh world')
            doc={'format':'poima.authored-world','version':1,'world_id':initial['world_id'],'revision':0,'entities':{},'retired_ids':[],'receipts':[]}
            doc,_=owner.commit(transaction(0,'Entity 1',1,create=True),doc,role+'-seed1');doc,_=owner.commit(transaction(1,'Seed2',2),doc,role+'-seed2')
            for offset,stage in enumerate(('primary_publish','previous_publish'),3):doc,last,receipt=exercise_failure(owner,win,stage,transaction(doc['revision'],stage,offset),doc,oracles[stage]['win32_code'])
            warm=owner.observe('warm-final');before_eof=capture.snap(world,role+'-warm-before-eof')[0];owner.close();capture.expect(world,role+'-warm-after-eof',before_eof)
            stderr=filebytes(directory/'warm/connect-stderr.txt').decode('utf-8');lines=stderr.splitlines()
            if role=='current':
                expected=['World operation failed: Cannot publish world snapshot. [storage operation=MoveFileExW stage='+stage+' win32_code='+str(oracles[stage]['win32_code'])+']'for stage in('primary_publish','previous_publish')];need(lines==expected,'Exact current diagnostic operation/stage/independent actual Win32 code')
            else:need(lines==['World operation failed: Cannot publish world snapshot.']*2,'Exact original171 generic stderr')
            owner=None;owner=Owner.__new__(Owner);owner.transport=None;owner.close_attempted=False;Owner.__init__(owner,binary.resolve(),directory/'cold',world,role+'-cold',report,budget,transport.RetainedTransport,RpcError,capture);cold=owner.observe('cold-original');expected_cold=copy.deepcopy(warm);expected_cold['history']={'revision':doc['revision'],'undo_count':0,'redo_count':0,'bytes':0,'max_entries':32,'max_bytes':16*1024**2,'session_local':True,'skipped_large_edits':0};need(exact(cold,expected_cold),'Cold whole observation with explicit session-local history reset');capture.expect(world,role+'-cold-original',before_eof)
            replay=copy.deepcopy(receipt);replay['replayed']=True;need(exact(owner.call('world.transact',last),replay),'Cold persisted receipt survives reopen');capture.expect(world,role+'-cold-receipt',before_eof);need(exact(owner.observe('cold-receipt'),cold),'Cold receipt replay changes no complete observation')
            if role=='current':
                report['stress']['started']=True
                for index in range(131):
                    params=transaction(doc['revision'],'Stress '+str(index),1000+index,entity=STRESS_ENTITY,create=index==0);doc,result=owner.commit(params,doc,'stress-'+str(index));report['stress']['commits']+=1
                final=owner.observe('stress-final');need(final['inspect']['revision']==135 and len(doc['receipts'])==128,'Actual131commits and bounded receipt eviction');report['stress'].update(passed=True,final_revision=doc['revision'],receipt_count=len(doc['receipts']))
            terminal=capture.snap(world,role+'-cold-before-eof')[0];owner.close();capture.expect(world,role+'-cold-after-eof',terminal);need(filebytes(directory/'cold/connect-stderr.txt')==b'','Cold owner stderr must remain empty');owner=None
        if args.baseline:
            for stage in('primary_publish','previous_publish'):
                pair=[r for r in report['forced_failures']if r['stage']==stage];need(len(pair)==2 and pair[0]['raw_error_reply_hex']==pair[1]['raw_error_reply_hex'],'Complete original171/current public error wire byte parity')
        report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None and owner.transport is not None and not owner.close_attempted:
            try:owner.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        report['seconds']=time.monotonic()-started;report['snapshots']=capture.snapshots;report['source_inputs']=pins;report['source_inputs_unchanged']=exact(pins,{str(p):pin(p)for p in sources});report['binary_inputs']=binaries;report['binary_inputs_unchanged']=exact(binaries,{'current':pin(args.binary),**({'baseline':pin(args.baseline)}if args.baseline else{})})
        if report['seconds']>=180 or not report['source_inputs_unchanged']or not report['binary_inputs_unchanged']:report['passed']=False;report['errors'].append('Whole deadline or source/image fence failed')
        report['output_before_report']=census(args.output);write(args.output/'evidence.json',report)
    print(json.dumps({'passed':report['passed'],'owners':len(report['owners']),'calls':sum(len(o['calls'])for o in report['owners']),'stress':report['stress'],'errors':report['errors']}),flush=True);return 0 if report['passed']else 1
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--repository',type=Path,default=ROOT);p.add_argument('--binary',type=Path,required=True);p.add_argument('--baseline',type=Path);p.add_argument('--output',type=Path,required=True);raise SystemExit(run(p.parse_args()))
