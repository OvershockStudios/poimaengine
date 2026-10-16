#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Owned Windows publication retry/exhaustion and exact conflict-guard protocol."""
from pathlib import Path
import argparse,copy,hashlib,json,os,queue,re,subprocess,sys,threading,time,traceback
ROOT=Path(__file__).resolve().parents[1]
CONFLICT={'code':-32009,'message':'World file changed outside this session; reopen before editing.'}
CASES=[('primary_publish','persistent'),('previous_publish','persistent'),('primary_publish','release'),('previous_publish','release'),('previous_publish','primary_guard'),('primary_publish','prepared_guard')]
PENDING=re.compile(rb'World publication retry pending: \[storage operation=MoveFileExW stage=(primary_publish|previous_publish) win32_code=([0-9]+) retry_attempts=1\]\r?\n')
RECOVERED=re.compile(rb'World publication recovered: \[storage operation=MoveFileExW stage=(primary_publish|previous_publish) retry_attempts=([0-9]+) first_win32_code=([0-9]+) last_win32_code=([0-9]+) scheduled_wait_ms=([0-9]+) retry_elapsed_us=([0-9]+)\]\r?\n')
EXHAUSTED=re.compile(rb'World operation failed: Cannot publish world snapshot\. \[storage operation=MoveFileExW stage=(primary_publish|previous_publish) win32_code=([0-9]+) retry_attempts=([0-9]+) first_win32_code=([0-9]+) last_win32_code=([0-9]+) scheduled_wait_ms=([0-9]+) retry_elapsed_us=([0-9]+)\]\r?\n')
WAIT_PREFIX=[0,2,6,14,30,62]
def need(v,m):
    if not v:raise AssertionError(m)
def direct_stderr_transport(parent):
    class DirectStderrTransport(parent):
        """Retain protocol stdin/stdout; inherit stderr directly into owned raw file.

        The CLI's synchronous unit-buffered stderr writes precede its JSON
        reply. Seed replies therefore fence the exact stderr offset; no delayed
        stderr pump can misattribute a seed recovery to a later controlled RPC.
        """
        def __init__(self,argv,output,budget,accepted):
            self.budget,self.closed,self.counter=budget,False,0;self.accepted=accepted;self._condition=threading.Lock();self._next_id=1;self.broken=False;self.argv=argv;self.frames=queue.Queue(maxsize=4);self.faults=[]
            self.paths={name:output/('connect-'+name+'.ndjson'if name!='stderr'else'connect-stderr.txt')for name in('stdin','stdout','stderr')};budget.paths.extend(self.paths.values());self.writer=self.paths['stdin'].open('wb');self.direct_stderr=self.paths['stderr'].open('xb',buffering=0)
            self.process=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.direct_stderr,bufsize=0,shell=False);self._process=self.process;self.threads=[threading.Thread(target=self.pump,args=('stdout',),daemon=True)]
            self.threads[0].start()
        def close(self):
            try:super().close()
            finally:self.direct_stderr.close()
    return DirectStderrTransport
def compact(v):return json.dumps(v,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
def history_image(doc):return {'entities':doc['entities'],'component_schemas':{},'templates':{},'ui':{}}
def history_size(before,after):return len(compact(history_image(before)))+len(compact(history_image(after)))
def observe(owner,doc,undo,label,base):
    actual=owner.observe(label);eid=base.ENTITY
    expected={'inspect':{'world_id':doc['world_id'],'revision':doc['revision'],'entity_count':len(doc['entities']),'persisted':True,'read_only':False,'mode':'authoring','coordinate_system':'right-handed Y-up; meters; local XYZW quaternion transforms'},'entity':{'revision':doc['revision'],'id':eid,'value':doc['entities'][eid]},'query':{'revision':doc['revision'],'entities':[{'id':key,'name':v['name'],'parent':v['parent'],'components':sorted(v['components'])}for key,v in sorted(doc['entities'].items())],'next_after':None},'history':{'revision':doc['revision'],'undo_count':len(undo),'redo_count':0,'bytes':sum(undo),'max_entries':32,'max_bytes':16*1024**2,'session_local':True,'skipped_large_edits':0}}
    need(base.exact(actual,expected),'Complete independent native observation: '+label);return actual

def in_flight(owner,win,target,stage,mode,params,prepared,primary,record,base):
    path=owner.transport.paths['stderr'];offset=path.stat().st_size if path.exists()else 0
    record.update(stderr_start_offset=offset,started_ns=time.monotonic_ns(),handle_released=False);handle=win.lock(target);box={};done=threading.Event()
    expected=base.ERROR if mode=='persistent'else None if mode=='release'else CONFLICT
    def invoke():
        try:box['reply']=owner.call('world.transact',params,error=expected)
        except BaseException:box['error']=traceback.format_exc()
        finally:box['completed_ns']=time.monotonic_ns();done.set()
    worker=threading.Thread(target=invoke,daemon=True);worker.start();caught=None
    try:
        deadline=time.monotonic()+min(2,owner.budget.remaining())
        while True:
            data=path.read_bytes()[offset:]if path.exists()else b'';matches=[m for m in PENDING.finditer(data)if m.group(1).decode()==stage]
            if matches:
                first=matches[0];record.update(observed_pending_ns=time.monotonic_ns(),pending_raw_hex=first.group(0).hex(),pending_code=int(first.group(2)),pending_observed_before_reply=not done.is_set());need(record['pending_code']in(5,32,33),'Actual pending event must identify an eligible failed call')
                if mode!='persistent':need(not done.is_set(),'Required actual failed-call event was not observed while the same RPC remained in flight')
                break
            need(not done.is_set(),'RPC completed before its required pending event could be observed');need(time.monotonic()<deadline,'Bounded pending-event observation deadline');time.sleep(.0005)
        if mode=='release':
            record['action_started_ns']=time.monotonic_ns();win.release(handle);handle=None;record.update(handle_released=True,action_completed_ns=time.monotonic_ns())
        elif mode in('primary_guard','prepared_guard'):
            mutation=primary if mode=='primary_guard'else prepared;original=base.filebytes(mutation);expected_original=record['expected_mutation_original'];need(original.hex()==expected_original,'Entire actual guard input before declared external change')
            changed=original+b'\n';record.update(action_started_ns=time.monotonic_ns(),mutated_path_role='primary'if mode=='primary_guard'else'prepared',external_before=base.digest(original),external_after=base.digest(changed),external_bytes_hex=changed.hex())
            with mutation.open('wb')as stream:stream.write(changed)
            need(base.filebytes(mutation)==changed,'Complete declared external bytes');record['action_completed_ns']=time.monotonic_ns()
        need(done.wait(owner.budget.remaining()),'Original in-flight RPC did not finish');worker.join(timeout=owner.budget.remaining());need(not worker.is_alive(),'RPC observer thread did not retire')
        if mode in('release','persistent')and 'error'not in box:
            terminal=RECOVERED if mode=='release'else EXHAUSTED;drain_deadline=time.monotonic()+min(2,owner.budget.remaining())
            while True:
                retained=path.read_bytes()[offset:]if path.exists()else b''
                if any(m.group(1).decode()==stage for m in terminal.finditer(retained)):
                    record['terminal_stderr_observed_ns']=time.monotonic_ns();break
                need(time.monotonic()<drain_deadline,'Known RPC reply but required terminal stderr remains unobserved');time.sleep(.0005)
    except BaseException:caught=traceback.format_exc()
    finally:
        if handle is not None:win.release(handle);record['handle_released']=True
        remaining=max(0,min(12,owner.budget.deadline-time.monotonic()))
        if not done.is_set():done.wait(remaining)
        if done.is_set():worker.join(timeout=max(0,min(1,owner.budget.deadline-time.monotonic())))
        if worker.is_alive()or owner.transport.broken or(owner.row['calls']and owner.row['calls'][-1]['outcome']=='unknown'):owner.retry_outcome_unknown=True;record['outcome_unknown']=True
        record['rpc_thread']=box;record['stderr_end_offset']=path.stat().st_size if path.exists()else 0
        if path.exists():record['stderr_interval_hex']=path.read_bytes()[offset:record['stderr_end_offset']].hex()
    need(caught is None,'In-flight actor failed: '+str(caught));need('error'not in box,'Original RPC failed: '+str(box.get('error')));return box['reply']

def event_facts(record,stage,mode,complete_raw=None):
    raw=bytes.fromhex(record['stderr_interval_hex'])if complete_raw is None else complete_raw;pending=[m for m in PENDING.finditer(raw)if m.group(1).decode()==stage];recovered=[m for m in RECOVERED.finditer(raw)if m.group(1).decode()==stage];failed=[m for m in EXHAUSTED.finditer(raw)if m.group(1).decode()==stage]
    need(len(pending)==1,'Exactly one actual first-failure event for this rename');first=int(pending[0].group(2))
    if mode in('primary_guard','prepared_guard'):need(not recovered and not failed,'Conflict guard must not report recovery or exhaustion for that rename');return {'pending_first_win32_code':first,'guard_conflict':True}
    terminal=recovered if mode=='release'else failed;need(len(terminal)==1 and(not failed if mode=='release'else not recovered),'One exact target terminal event');m=terminal[0];numbers=[int(v)for v in m.groups()[1:]]
    if mode=='persistent':last,attempts,reported_first,reported_last,wait,elapsed=numbers;need(last==reported_last,'Literal terminal error and last failed-call code')
    else:attempts,reported_first,reported_last,wait,elapsed=numbers
    need(reported_first==first and 0<=reported_last<=0xffffffff and 1<=attempts<=6 and wait in WAIT_PREFIX and 0<=wait<=62 and elapsed>=0,'Exact bounded actual retry facts')
    if mode=='release':need(reported_last in(5,32,33)and attempts>=2 and wait==WAIT_PREFIX[attempts-1],'Successful retry preserves prior failed-call codes and scheduled sleeps')
    else:
        allowed={WAIT_PREFIX[attempts-1]}
        if attempts<6 and reported_last in(5,32,33):allowed.add(WAIT_PREFIX[attempts])
        need(wait in allowed,'Exhaustion includes only declared scheduled waits')
    return {'attempts':attempts,'first_win32_code':reported_first,'last_win32_code':reported_last,'scheduled_wait_ms':wait,'retry_elapsed_us':elapsed,'scope':'Elapsed includes actual observer/guard/wait overhead; scheduling and syscall duration are not a hard wall-clock promise.'}

def run(args):
    need(os.name=='nt'and not sys.flags.optimize,'Actual nonoptimized Windows Python required');root=args.repository.resolve();sys.dont_write_bytecode=True;sys.path[:0]=[str(root/'tests'),str(root/'tools/python')]
    import world_storage_protocol as base
    import switchback_game_service as transport
    from poima_client.errors import RpcError
    need(base.pin(root/'tests/world_storage_protocol.py')=={'bytes':20247,'sha256':'bcb377545fb7a8d387ca2568181bf725017a9fd312d108dc8843d5e7e0c3bbad'},'Immutable172reference helper required')
    base.LIMIT=128*1024**2;transport.LOG_LIMIT=16*1024**2;DirectTransport=direct_stderr_transport(transport.RetainedTransport);win=base.Windows();need(not args.output.exists(),'Fresh protocol output');args.output.mkdir(parents=True);capture=base.Capture(args.output)
    sources=[Path(__file__).resolve(),root/'tests/world_storage_protocol.py',root/'tests/switchback_game_service.py',root/'tests/switchback_oracle.py']+sorted(p for p in(root/'tools/python/poima_client').rglob('*')if p.suffix in('.py','.json')and'__pycache__'not in p.parts);pins={str(p):base.pin(p)for p in sources};binary=base.pin(args.binary)
    started=time.monotonic();budget=transport.Budget(started+180,[]);report={'format':'poima.world-storage-retry-protocol.v1','passed':False,'owners':[],'cases':[],'errors':[],'stderr_observation':'Native stderr inherited directly into the owned raw file, no stderr pump. Protocol stdin/stdout retain the existing framed transport. This changes observer I/O and is not a timing comparison.','scope':'Fixed six-case real Windows publication retry/exhaustion/exact-byte-guard protocol,12serial native owners. No broad atomicity, general reliability, original failure regrade, token, performance or external-cause claim.'};owner=None
    try:
        for index,(stage,mode)in enumerate(CASES):
            label=f'{index}-{stage}-{mode}';folder=args.output/label;folder.mkdir();world=folder/'世界 scene.json';owner=base.Owner.__new__(base.Owner);owner.transport=None;owner.close_attempted=False;owner.retry_outcome_unknown=False;base.Owner.__init__(owner,args.binary.resolve(),folder/'warm',world,label+'-warm',report,budget,DirectTransport,RpcError,capture)
            fresh=owner.call('world.inspect');need(fresh['revision']==0 and fresh['persisted']is False,'Fresh two-commit world');doc={'format':'poima.authored-world','version':1,'world_id':fresh['world_id'],'revision':0,'entities':{},'retired_ids':[],'receipts':[]};undo=[]
            for n in(1,2):
                old=doc;doc,_=owner.commit(base.transaction(doc['revision'],'Entity 1'if n==1 else'Seed2',n,create=n==1),doc,label+'-seed'+str(n));undo.append(history_size(old,doc))
            before=observe(owner,doc,undo,'before',base);files=capture.snap(world,label+'-before')[0];params=base.transaction(2,'Changed',3);candidate,receipt=base.forecast(doc,params)
            record={'index':index,'stage':stage,'mode':mode,'request':copy.deepcopy(params),'passed':False};report['cases'].append(record)
            target=world if stage=='primary_publish'else Path(str(world)+'.previous');prepared=Path(str(world)+('.pending'if stage=='primary_publish'else'.previous.pending'))
            if mode=='primary_guard':record['expected_mutation_original']=files[''].hex()
            elif mode=='prepared_guard':record['expected_mutation_original']=base.encoded(candidate).hex()
            reply=in_flight(owner,win,target,stage,mode,params,prepared,world,record,base);record['retry_facts']=event_facts(record,stage,mode)
            expected_files=copy.deepcopy(files)
            if mode=='release':
                need(base.exact(reply,receipt),'Whole same-RPC success receipt, not a pending/recovered event alone');expected_files={'':base.encoded(candidate),'.pending':None,'.previous':files[''],'.previous.pending':None};undo.append(history_size(doc,candidate));doc=candidate
            else:
                need(base.exact(reply,base.ERROR if mode=='persistent'else CONFLICT),'Exact unchanged public storage/conflict error');expected_files['.pending']=base.encoded(candidate)
                if stage=='primary_publish':expected_files['.previous']=files[''];expected_files['.previous.pending']=None
                else:expected_files['.previous.pending']=files['']
                if mode=='primary_guard':expected_files['']=files['']+b'\n'
                elif mode=='prepared_guard':expected_files['.pending']=base.encoded(candidate)+b'\n'
            capture.expect(world,label+'-after',expected_files);need(base.exact(base.document(world),doc),'Complete whole authored primary model');after=observe(owner,doc,undo,'after',base)
            if mode!='release':need(base.exact(before,after),'Known failed/conflicting RPC changed complete native state/history/receipts')
            if mode=='release':
                expected_replay=copy.deepcopy(receipt);expected_replay['replayed']=True;need(base.exact(owner.call('world.transact',params),expected_replay),'Same-owner exact once-only receipt');need(base.exact(observe(owner,doc,undo,'receipt',base),after),'Receipt replay changed complete native state');capture.expect(world,label+'-receipt',expected_files)
            owner.close();capture.expect(world,label+'-warm-eof',expected_files)
            full_stderr=base.filebytes(folder/'warm/connect-stderr.txt')[record['stderr_start_offset']:];record['complete_stderr_eof_interval_hex']=full_stderr.hex()
            need(all(PENDING.fullmatch(line)or RECOVERED.fullmatch(line)or EXHAUSTED.fullmatch(line)for line in full_stderr.splitlines(keepends=True)),'Complete retained post-attempt stderr has unrecognized event bytes')
            need(base.exact(event_facts(record,stage,mode,full_stderr),record['retry_facts']),'Complete EOF event facts differ from bounded immediate observation');owner=None
            owner=base.Owner.__new__(base.Owner);owner.transport=None;owner.close_attempted=False;owner.retry_outcome_unknown=False;base.Owner.__init__(owner,args.binary.resolve(),folder/'cold',world,label+'-cold',report,budget,DirectTransport,RpcError,capture);cold=observe(owner,doc,[],'cold',base);capture.expect(world,label+'-cold',expected_files)
            if mode=='release':
                need(base.exact(owner.call('world.transact',params),expected_replay),'Cold persisted exact once-only receipt');need(base.exact(observe(owner,doc,[],'cold-receipt',base),cold),'Cold replay changed complete native state');capture.expect(world,label+'-cold-receipt',expected_files)
            else:need(all(r['params']['request_id']!=params['request_id']for r in doc['receipts']),'Failed request acquired no durable receipt')
            owner.close();capture.expect(world,label+'-cold-eof',expected_files);owner=None;record['passed']=True
        report['passed']=True
    except BaseException:
        report['errors'].append(traceback.format_exc())
        if owner is not None and owner.transport is not None and not getattr(owner,'retry_outcome_unknown',False)and owner.row.get('calls')and owner.row['calls'][-1]['outcome']in('result','error')and not owner.transport.broken:
            try:report['failure_snapshot']=capture.snap(owner.world,'actual-failure')[1];report['failure_native']=owner.observe('actual-failure')
            except BaseException:report['failure_observation_error']=traceback.format_exc()
    finally:
        if owner is not None and owner.transport is not None and not owner.close_attempted and not getattr(owner,'retry_outcome_unknown',False):
            try:owner.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        report.update(snapshots=capture.snapshots,source_inputs=pins,source_inputs_unchanged=base.exact(pins,{str(p):base.pin(p)for p in sources}),binary_input=binary,binary_input_unchanged=base.exact(binary,base.pin(args.binary)),seconds=time.monotonic()-started)
        if not report['source_inputs_unchanged']or not report['binary_input_unchanged']or report['seconds']>=180:report['passed']=False;report['errors'].append('Source/image fence or180s worker budget failed')
        report['passed']=report['passed']and report['errors']==[]and len(report['cases'])==6 and all(c['passed']is True for c in report['cases'])and len(report['owners'])==12 and all(o['closed']is True and type(o['exit_code'])is int and o['exit_code']==0 and o.get('stream_threads_closed')is True and o.get('faults')==[]for o in report['owners'])
        report['output_before_report']=base.census(args.output);base.write(args.output/'evidence.json',report)
    print(json.dumps({'passed':report['passed'],'cases':len(report['cases']),'owners':len(report['owners']),'errors':report['errors']}),flush=True);return 0 if report['passed']else 1
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--repository',type=Path,default=ROOT);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);raise SystemExit(run(p.parse_args()))
