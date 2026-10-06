#!/usr/bin/env python3
"""Qualify native profiler controls through real world-service processes."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import uuid

ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def uid(n):return f'{n:032x}'
class Client:
    def __init__(self,args,path,evidence):
        self.evidence=evidence;self.proc=subprocess.Popen([str(args.binary.resolve()),'world',args.native(path)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
        self.lines=queue.Queue();self.errors=[]
        def read():
            for line in self.proc.stdout:self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=read,daemon=True).start()
        threading.Thread(target=lambda:self.errors.extend(self.proc.stderr),daemon=True).start()
    def call(self,method,p=None,error=None):
        request=dict(jsonrpc='2.0',id=uuid.uuid4().hex,method=method,params=p or {})
        self.proc.stdin.write(json.dumps(request)+'\n');self.proc.stdin.flush()
        line=self.lines.get(timeout=60);assert line is not None,self.errors
        reply=json.loads(line);self.evidence['calls'].append(dict(request=request,response=reply))
        assert reply['id']==request['id']
        if error is not None:assert reply.get('error',{}).get('code')==error,reply;return reply['error']
        assert 'result' in reply,reply
        return reply['result']
    def close(self):
        self.proc.stdin.close()
        try:self.proc.wait(timeout=15);assert self.proc.returncode==0,self.errors
        finally:
            if self.proc.poll() is None:self.proc.kill();self.proc.wait(timeout=10)
            self.proc.stdout.close();self.proc.stderr.close()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('binary',type=Path);ap.add_argument('--windows-interop',action='store_true');ap.add_argument('--output',type=Path,default=ROOT/'build/profiler-contract');args=ap.parse_args()
    def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
    args.native=native;run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    evidence=dict(passed=False,binary_sha256=sha(args.binary),test_sha256=sha(__file__),calls=[],checks=[]);clients=[]
    try:
        first=Client(args,run/'first.json',evidence);clients.append(first)
        second=Client(args,run/'second.json',evidence);clients.append(second)
        descriptor=first.call('world.describe');assert descriptor['schema_revision']==38
        for name in ['start','stop','status','events','summary','export']:assert 'profiler.'+name in descriptor['methods']
        assert first.call('profiler.status')['state']=='empty'
        cap=uid(10);params=dict(capture_id=cap,expected_capture_id=None,capacity=256)
        for bad in [True,63,65537,64.0,'64']:first.call('profiler.start',{**params,'capacity':bad},-32602)
        first.call('profiler.start',{**params,'extra':1},-32602)
        accepted=first.call('profiler.start',params);assert accepted['recording'] and accepted['count']==0
        for _ in range(3):assert first.call('profiler.status')['count']==0
        assert first.call('profiler.start',params)['replayed']
        first.call('profiler.start',{**params,'capacity':128},-32009)
        first.call('profiler.events',dict(capture_id=cap),-32009)
        first.call('profiler.stop',dict(capture_id=uid(11)),-32009)
        first.call('world.inspect')
        for method in ['x'*90,'é'*60,'bad\x00method']:first.call(method,error=-32601)
        assert second.call('profiler.status')['state']=='empty'
        first.call('profiler.stop',dict(capture_id=cap))
        page=first.call('profiler.events',dict(capture_id=cap));events=page['events'];assert len(events)==4
        assert events[0]['name']=='world.inspect' and not events[0]['failed']
        assert all(e['failed'] for e in events[1:]);assert events[1]['name_truncated'];assert events[2]['name']==events[3]['name']=='request.invalid_name'
        assert not any(e['name'].startswith('profiler.') for e in events)
        trace=first.call('profiler.export',dict(capture_id=cap));assert len([e for e in trace['trace']['traceEvents'] if e['ph']=='X'])==4
        assert first.call('profiler.events',dict(capture_id=cap))==page
        evidence['checks'].append('Discovery/guards/retries, independent owners, failed-operation spans and safe Unicode/NUL diagnostic names; profiler observation does not record itself.')
        full=uid(12);first.call('profiler.start',dict(capture_id=full,expected_capture_id=cap,capacity=64))
        for _ in range(70):first.call('world.inspect')
        status=first.call('profiler.status');assert status['state']=='full' and not status['recording'] and status['open']==0 and status['count']==64 and status['dropped']==1,status
        joined=[];offset=0
        while True:
            page=first.call('profiler.events',dict(capture_id=full,offset=offset,limit=7));joined+=page['events']
            if page['next_offset'] is None:break
            offset=page['next_offset']
        assert len(joined)==64 and [e['id'] for e in joined]==list(range(1,65))
        first.call('profiler.events',dict(capture_id=full,offset=65),-32602)
        first.call('profiler.events',dict(capture_id=cap),-32009)
        first.call('profiler.start',dict(capture_id=cap,expected_capture_id=full,capacity=64),-32009)
        assert first.call('profiler.status')==status
        evidence['checks'].append('Capacity overflow seals a retained prefix with one rejected reservation; paging remains stable, stale/reused capture IDs reject.')
        # This process may be the authoring-only configuration.
        runtime=first.call('runtime.status')
        if runtime['available']:
            fixture=json.loads((ROOT/'examples/physics-room.jsonl').read_text())
            states=[]
            for client,capture in [(first,uid(13)),(second,None)]:
                client.call(fixture['method'],fixture['params']);client.call('runtime.start',dict(session_id=uid(1000),revision=1))
                if capture:client.call('profiler.start',dict(capture_id=capture,expected_capture_id=full,capacity=16384))
                for tick,count in [(0,30),(30,30)]:
                    client.call('runtime.step',dict(session_id=uid(1000),request_id=uuid.uuid4().hex,expected_tick=tick,ticks=count,inputs=[dict(entity=uid(100),move=[0,1],look=[0,0],jump=False)]))
                client.call('runtime.step',dict(session_id=uid(1000),request_id=uuid.uuid4().hex,expected_tick=60,ticks=2,inputs=[dict(entity=uid(9999),move=[0,0])]),-32040)
                states.append(client.call('runtime.entity',dict(session_id=uid(1000),tick=60,id=uid(100))))
                if capture:client.call('profiler.stop',dict(capture_id=capture))
            assert states[0]==states[1]
            summary=first.call('profiler.summary',dict(capture_id=uid(13)));names={r['name'] for r in summary['groups']}
            assert {'runtime.batch','runtime.tick','runtime.physics.update','runtime.checkpoint','owner.advance'}<=names,names
            all_events=[];offset=0
            while True:
                page=first.call('profiler.events',dict(capture_id=uid(13),offset=offset,limit=1024));all_events+=page['events']
                if page['next_offset'] is None:break
                offset=page['next_offset']
            ticks=[e for e in all_events if e['name']=='runtime.tick'];assert len(ticks)==60 and {e['tick'] for e in ticks}==set(range(60)),ticks[:2]
            assert all(e['session']==uid(1000) for e in ticks)
            for e in all_events:
                if e['parent']:
                    parent=all_events[e['parent']-1];assert parent['start_ns']<=e['start_ns'] and e['start_ns']+e['duration_ns']<=parent['start_ns']+parent['duration_ns']
            evidence['checks'].append('Actual 60-tick physics/controller replay is identical with profiling off/on; fixed-tick phases/session identity/nesting and failed input are recorded.')
        else:evidence['runtime_skipped']='Authoring-only engine reports simulation unavailable.'
        evidence['passed']=True
    finally:
        for client in clients:client.close()
        (run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps(dict(passed=True,checks=len(evidence['checks']),calls=len(evidence['calls']),evidence=str(run/'evidence.json'))))
if __name__=='__main__':main()
