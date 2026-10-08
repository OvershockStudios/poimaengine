#!/usr/bin/env python3
"""Exact old/new world discovery parity through native scopes and real CLI hosts.

Supply previously linked and preserved old/new world_schema_native probes. Their
complete transcripts must match byte for byte, without normalization or omitted
fields. CLI owners each receive a fresh copy of the same authored fixture and
repeat every standalone/shared-headless discovery case from those transcripts.
RPC comparisons canonicalize parsed object-key order and whitespace while
retaining all fields, array order, and boolean/integer/floating-point types.
No renderer, desktop process, provider, or gameplay tick is started.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def need(ok,message):
    if not ok:raise RuntimeError(message)
def decode(text):
    def unique(rows):
        out={}
        for key,value in rows:
            need(key not in out,'Duplicate JSON key');out[key]=value
        return out
    return json.loads(text,object_pairs_hook=unique,parse_constant=lambda value:(_ for _ in ()).throw(ValueError(value)))
def encode(value):return json.dumps(value,ensure_ascii=False,allow_nan=False,separators=(',',':'))
def same_json(left,right):
    # Python equality conflates True, 1 and 1.0. Canonical JSON retains their
    # distinct wire types while ignoring only object-key order/whitespace.
    def canonical(value):return json.dumps(value,sort_keys=True,ensure_ascii=False,allow_nan=False,separators=(',',':'))
    return canonical(left)==canonical(right)

def comparison_self_test():
    values=[True,1,1.0]
    for i,left in enumerate(values):
        for j,right in enumerate(values):
            need(same_json({'result':[left]}, {'result':[right]})==(i==j),'RPC equality conflates boolean/integer/float')
    need(same_json({'b':[1.0,True],'a':None},{'a':None,'b':[1.0,True]}),'Object-key order should not affect parsed RPC equality')
    need(not same_json([1,2],[2,1]),'Array order must affect RPC equality')

def files(root):return {p.relative_to(root).as_posix():sha(p) for p in sorted(root.rglob('*')) if p.is_file()}

def main():
    comparison_self_test()
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','legacy-binary','scoped-probe','legacy-scoped-probe','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--timeout-seconds',type=float,default=360)
    a=parser.parse_args()
    if not 30<=a.timeout_seconds<=900:parser.error('--timeout-seconds must be30..900')
    a.output=a.output.resolve()
    if a.output.exists():parser.error('--output must be new')
    inputs={name:getattr(a,name.replace('-','_')).resolve(strict=True) for name in ('binary','legacy-binary','scoped-probe','legacy-scoped-probe')}
    if not all(path.is_file() for path in inputs.values()):parser.error('All binaries/probes must be regular files')
    a.output.mkdir(parents=True);deadline=time.monotonic()+a.timeout_seconds
    record=dict(passed=False,normalization='Native: none; complete transcript bytes compared. RPC: parsed/canonical object-key order and whitespace ignored; every field, array order and boolean/integer/floating-point type retained.',
        source_sha256=sha(Path(__file__)),inputs={key:dict(path=str(path),sha256=sha(path)) for key,path in inputs.items()},commands=[],owners=[],checks=[])
    def timeout(maximum=90):
        remaining=deadline-time.monotonic();need(remaining>0,'Overall qualification deadline exhausted');return min(maximum,remaining)
    def native(path):
        value=str(path.resolve())
        if a.windows_interop:return subprocess.check_output(['wslpath','-w',value],text=True,timeout=timeout(5)).strip()
        return value
    def command(label,args,data=None):
        output=a.output/(label+'.stdout');error=a.output/(label+'.stderr');started=time.monotonic();entry=dict(label=label,args=args,forced=False)
        record['commands'].append(entry)
        with output.open('wb') as stdout,error.open('wb') as stderr:
            owner=subprocess.Popen(args,stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,stdout=stdout,stderr=stderr)
            try:owner.communicate(None if data is None else data.encode('utf-8'),timeout=timeout(150))
            except BaseException:
                entry['forced']=True;owner.kill();owner.wait(timeout=10);raise
            finally:entry.update(exit_code=owner.returncode,elapsed_seconds=time.monotonic()-started)
        need(owner.returncode==0,'Command failed: '+label+'; inspect retained stdout/stderr')
        need(error.stat().st_size==0,'Unexpected process stderr: '+label)
        return output.read_text(encoding='utf-8')
    def packets(cases):
        # Preserve JSON null/boolean/array params rather than a client SDK's
        # convenience defaults or local selector validation.
        probe=dict(jsonrpc='2.0',id=17)
        guard=[dict(probe,method='world.inspect',params={}),dict(probe,method='world.history',params={})]
        requests=guard+[dict(probe,method='world.describe',params=row['params']) for row in cases]+guard
        return requests,'\n'.join(encode(row) for row in requests)+'\n'
    def validate_replies(label,text,cases,requests):
        replies=[decode(line) for line in text.splitlines() if line.strip()]
        need(len(replies)==len(requests),'Missing/extra JSON-RPC replies: '+label)
        need(same_json(replies[:2],replies[-2:]),'Discovery mutated observable world/history: '+label)
        need(all(row.get('jsonrpc')=='2.0' and type(row.get('id')) is int and row['id']==17 for row in replies),'Invalid JSON-RPC envelope: '+label)
        need(same_json(replies[2:-2],[row['reply'] for row in cases]),'CLI discovery differs from scoped native oracle: '+label)
        return replies
    def cli_world(label,binary,fixture,cases,shared=False):
        root=a.output/label;root.mkdir();world=root/'world.json';world.write_text(encode(fixture)+'\n',encoding='utf-8');before=world.read_bytes()
        requests,data=packets(cases);host=None;stdout=stderr=None;cleanup_errors=[];entry=dict(label=label,shared=shared,forced=False)
        record['owners'].append(entry)
        try:
            if not shared:
                text=command(label,[str(binary),'world',native(world)],data)
            else:
                endpoint='schema-'+uuid.uuid4().hex;stdout=(root/'host.stdout').open('wb');stderr=(root/'host.stderr').open('wb')
                host=subprocess.Popen([str(binary),'serve',native(world),'--endpoint',endpoint],stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr)
                # Native connect bounds connection establishment; no readiness
                # polling/restarts and no extra world operations hidden here.
                text=command(label,[str(binary),'connect',endpoint,'--timeout-ms','10000'],data)
            replies=validate_replies(label,text,cases,requests)
            need(world.read_bytes()==before,'Discovery changed authored bytes: '+label)
            entry['discovery_calls']=len(cases)
        finally:
            if host is not None:
                try:
                    if host.poll() is None:
                        stop=command(label+'-shutdown',[str(binary),'connect',endpoint,'--timeout-ms','1000'],encode(dict(jsonrpc='2.0',id=17,method='host.shutdown',params={}))+'\n')
                        reply=decode(stop.strip());need('result' in reply,'Host shutdown rejected')
                    host.wait(timeout=timeout(8))
                except BaseException as error:
                    cleanup_errors.append(str(error))
                    if host.poll() is None:entry['forced']=True;host.kill();host.wait(timeout=10)
                finally:
                    stdout.close();stderr.close();entry['exit_code']=host.returncode
                    if host.returncode!=0:cleanup_errors.append('Shared host exit was nonzero')
                    diagnostic=(root/'host.stderr').read_bytes()
                    expected=('Poima shared world ready: '+endpoint+'\n').encode('ascii')
                    allowed=[expected]
                    if a.windows_interop:allowed.append(expected.replace(b'\n',b'\r\n'))
                    entry['expected_banner_verified']=diagnostic in allowed
                    entry['host_stderr_sha256']=hashlib.sha256(diagnostic).hexdigest()
                    if not entry['expected_banner_verified']:cleanup_errors.append('Shared host stderr differs from exact ready banner')
                    if (root/'host.stdout').stat().st_size:cleanup_errors.append('Shared host unexpectedly wrote stdout')
            else:
                entry['exit_code']=record['commands'][-1].get('exit_code')
            entry['cleanup_errors']=cleanup_errors
            need(not entry['forced'] and not cleanup_errors,'Owned process cleanup failed: '+label)
            need(world.read_bytes()==before,'Closing discovery owner changed authored bytes: '+label)
        # The cooperative lock's empty sidecar intentionally remains. Prove
        # released OS ownership by opening the same world in a fresh process,
        # rather than interpreting sidecar presence as a held lock.
        reopen_requests=requests[:2]
        reopened=command(label+'-reopen',[str(binary),'world',native(world)],'\n'.join(encode(row) for row in reopen_requests)+'\n')
        need(same_json([decode(line) for line in reopened.splitlines() if line.strip()],replies[:2]),'Fresh owner reopen changed world/history or failed: '+label)
        need(world.read_bytes()==before,'Fresh owner reopen changed authored bytes: '+label)
        entry['fresh_reopen_verified']=True
        return replies
    try:
        transcripts={}
        for name,key in [('legacy','legacy-scoped-probe'),('current','scoped-probe')]:
            dump=a.output/(name+'-scopes.json');root=a.output/(name+'-native-world')
            text=command(name+'-native',[str(inputs[key]),'--output',native(root),'--dump',native(dump)])
            need('World schema oracle passed' in text,'Native scoped assertions did not finish')
            need(dump.stat().st_size<=64*1024*1024,'Native transcript exceeds64MiB');d=decode(dump.read_text(encoding='utf-8'))
            need(d.get('passed') is True and len(d.get('transcript',[]))==6,'Incomplete native scope oracle')
            need(d['discovery_calls']==sum(len(view['cases']) for view in d['transcript']),'Native case count mismatch')
            transcripts[name]=d
        old,new=transcripts['legacy'],transcripts['current']
        need((a.output/'legacy-scopes.json').read_bytes()==(a.output/'current-scopes.json').read_bytes(),'Complete native discovery transcript differs; no normalization allowed')
        need(old==new,'Native JSON transcript differs')
        record['checks'].append('Exact complete native transcript across six mode/scope combinations')
        record['scoped_discovery_calls_per_host']=old['discovery_calls']
        record['native_transcript_sha256']=sha(a.output/'legacy-scopes.json')
        # The pure probe independently uses the identical authored fixture on
        # each host, then compares all scoped responses. CLI fixtures are fresh
        # copies, not a reused owner/cache or fields patched after observation.
        for scope in ('standalone','shared_headless'):
            rows=next(view['cases'] for view in old['transcript'] if view['mode']=='authoring' and view['scope']==scope)
            results=[]
            for name,key in [('legacy','legacy-binary'),('current','binary')]:results.append(cli_world(name+'-'+scope,inputs[key],old['fixture_document'],rows,shared=scope=='shared_headless'))
            need(same_json(results[0],results[1]),'Complete old/new CLI replies differ: '+scope)
            record['checks'].append('Exact fresh-copy CLI discovery/state/history replies: '+scope)
        need(all(sha(path)==record['inputs'][key]['sha256'] for key,path in inputs.items()),'Binary or probe changed during qualification')
        need(all(not owner['forced'] and owner['exit_code']==0 for owner in record['owners']),'Owned world exit was not clean')
        record['passed']=True
    except BaseException as error:
        record['error']=repr(error)
    finally:
        (a.output/'evidence.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(a.output/'evidence.json');return 0 if record['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
