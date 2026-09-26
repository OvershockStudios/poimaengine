#!/usr/bin/env python3
"""A native sliding-door room: queries, solid collision, shadows and live images."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels
parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();root=Path(__file__).resolve().parents[1];run=args.output/uuid.uuid4().hex;run.mkdir(parents=True)
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
requests=[]
def request(method,params):
    requests.append({'jsonrpc':'2.0','id':len(requests)+1,'method':method,'params':params});return len(requests)
fixture=json.loads((root/'examples/interaction-room.jsonl').read_text());request(fixture['method'],fixture['params'])
request('runtime.start',{'session_id':uid(900),'revision':1})
def step(tick,ticks,motions=None,walk=False):
    return request('runtime.step',{'session_id':uid(900),'request_id':uuid.uuid4().hex,'expected_tick':tick,'ticks':ticks,'motions':motions or [],'inputs':[{'entity':uid(100),'move':[0,1]}] if walk else []})
def capture(name,tick):return request('runtime.capture',{'session_id':uid(900),'tick':tick,'camera':uid(101),'gpu':args.gpu,'profile':True,'path':native(run/(name+'.bmp'))})
def state(n,tick):return request('runtime.entity',{'session_id':uid(900),'id':uid(n),'tick':tick})
def ray(tick):return request('runtime.raycast',{'session_id':uid(900),'tick':tick,'origin':[0,1.5,2],'direction':[0,0,-1],'distance':10,'ignore':[uid(100)]})
step(0,120);closed=capture('closed',120);initial=ray(120)
step(120,120,[{'entity':uid(2),'position':[2.2,1.5,-3],'rotation':[0,0,0,1],'duration_ticks':120}]);opened=capture('open',240);clear=ray(240);door=state(2,240);handle=state(6,240)
step(240,150,walk=True);passed=state(100,390)
request('entity.get',{'id':uid(2),'component':'Transform'})
command=[str(args.binary.resolve()),'world',native(run/'room.world.json')]
p=subprocess.run(command,input=''.join(json.dumps(r)+'\n' for r in requests),text=True,capture_output=True,timeout=120)
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256((root/'examples/interaction-room.jsonl').read_bytes()).hexdigest(),'gpu_index':args.gpu,'requests':requests,'stderr':p.stderr,'exit_code':p.returncode}
try:
    assert p.returncode==0,p.stdout+p.stderr
    replies={r['id']:r for r in map(json.loads,p.stdout.splitlines())};record['responses']=replies;assert len(replies)==len(requests)
    for r in replies.values():assert 'result' in r,r
    def result(n):return replies[n]['result']
    for n in [closed,opened]:
        r=result(n);assert r['capture_written'] and r['hardware'] and r['nvrhi_errors']==0 and r['render_diagnostics']['gpu']['available'],r
    assert result(initial)['hit']['entity']==uid(2) and result(clear)['hit'] is None
    assert abs(result(door)['world_matrix'][12]-2.2)<1e-5 and result(door)['kinematic_target'] is None
    assert abs(result(handle)['world_matrix'][12]-2.8)<1e-5
    assert result(passed)['world_matrix'][14]<-7 and result(len(requests))['value']['position']==[0,1.5,-3]
    before=pixels(run/'closed.bmp');after=pixels(run/'open.bmp');center0=before[270][480];center1=after[270][480]
    assert max(abs(a-b) for a,b in zip(center0,center1))>40,(center0,center1)
    changed=sum(a!=b for ra,rb in zip(before,after) for a,b in zip(ra,rb));assert changed>10000
    record['pixels']={'closed_center':center0,'open_center':center1,'changed_pixels':changed}
    record['checks']={'collider_query_changes_with_door':True,'child_follows_door':True,'character_passes_open_door':True,'authored_spawn_unchanged':True,'fixed_camera_images_change':True}
    record['images']={p.name:{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in run.glob('*.bmp')};record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'checks':record['checks'],'pixels':record['pixels']}))
