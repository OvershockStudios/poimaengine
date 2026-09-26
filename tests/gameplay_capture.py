#!/usr/bin/env python3
"""Run the C# door through Vulkan replay and compare independent headless ticks."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels
parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--hostfxr',type=Path,required=True);parser.add_argument('--bridge',type=Path,required=True);parser.add_argument('--game',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();root=Path(__file__).resolve().parents[1];run=args.output/uuid.uuid4().hex;run.mkdir(parents=True)
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
requests=[]
def request(method,params):
    requests.append({'jsonrpc':'2.0','id':len(requests)+1,'method':method,'params':params});return len(requests)
fixture=json.loads((root/'examples/interaction-room.jsonl').read_text());request(fixture['method'],fixture['params'])
config={'hostfxr':native(args.hostfxr),'bridge':native(args.bridge),'assembly':native(args.game),'type':'Poima.Examples.DoorGame'}
sequence=[{'ticks':120},{'ticks':150,'move':[0,1]},{'ticks':120,'use':True},{'ticks':90,'move':[0,1]}]
request('runtime.start',{'session_id':uid(900),'revision':1})
request('runtime.gameplay.load',{'session_id':uid(900),'request_id':uid(1000),'expected_tick':0,'expected_revision':0,**config})
play={'session_id':uid(900),'request_id':uid(1001),'expected_tick':0,'controller':uid(100),'camera':uid(101),'mode':'replay','sequence':sequence,'gpu':args.gpu,'profile':True,'path':native(run/'player.bmp')}
played=request('runtime.play',play);retry=request('runtime.play',play)
first=[request('runtime.entity',{'session_id':uid(900),'tick':480,'id':uid(n)}) for n in [100,101,2,3,6]]
first_game=request('runtime.gameplay.inspect',{'session_id':uid(900),'tick':480})
request('runtime.stop',{'session_id':uid(900)});request('runtime.start',{'session_id':uid(901),'revision':1})
request('runtime.gameplay.load',{'session_id':uid(901),'request_id':uid(1002),'expected_tick':0,'expected_revision':0,**config})
tick=0
for s in sequence:
    control={k:v for k,v in s.items() if k!='ticks'};control['entity']=uid(100)
    request('runtime.step',{'session_id':uid(901),'request_id':uuid.uuid4().hex,'expected_tick':tick,'ticks':s['ticks'],'inputs':[control]});tick+=s['ticks']
second=[request('runtime.entity',{'session_id':uid(901),'tick':480,'id':uid(n)}) for n in [100,101,2,3,6]]
second_game=request('runtime.gameplay.inspect',{'session_id':uid(901),'tick':480})
capture=request('runtime.capture',{'session_id':uid(901),'tick':480,'camera':uid(101),'gpu':args.gpu,'culling':False,'profile':True,'path':native(run/'baseline.bmp')})
# Rewind only by starting a fresh runtime, then observe the stationary camera
# before and after C# handles use. A longer test interaction distance is a field edit.
request('runtime.stop',{'session_id':uid(901)});request('runtime.start',{'session_id':uid(902),'revision':1})
request('runtime.gameplay.load',{'session_id':uid(902),'request_id':uid(1003),'expected_tick':0,'expected_revision':0,**config,'values':{'UseDistance':10}})
request('runtime.step',{'session_id':uid(902),'request_id':uid(1004),'expected_tick':0,'ticks':120})
closed=request('runtime.capture',{'session_id':uid(902),'tick':120,'camera':uid(101),'gpu':args.gpu,'path':native(run/'closed.bmp')})
request('runtime.step',{'session_id':uid(902),'request_id':uid(1005),'expected_tick':120,'ticks':120,'inputs':[{'entity':uid(100),'use':True}]})
opened=request('runtime.capture',{'session_id':uid(902),'tick':240,'camera':uid(101),'gpu':args.gpu,'path':native(run/'open.bmp')})
request('runtime.stop',{'session_id':uid(902)});request('runtime.start',{'session_id':uid(903),'revision':1})
collected=request('runtime.gameplay.collect',{'session_id':uid(903)})
p=subprocess.run([str(args.binary.resolve()),'world',native(run/'room.world.json')],input=''.join(json.dumps(r)+'\n' for r in requests),text=True,capture_output=True,timeout=120)
record={'binary_sha256':sha(args.binary),'bridge_sha256':sha(args.bridge),'game_sha256':sha(args.game),'test_sha256':sha(Path(__file__)),'gpu_index':args.gpu,'requests':requests,'exit_code':p.returncode,'stderr':p.stderr}
try:
    assert p.returncode==0,p.stdout+p.stderr
    replies={r['id']:r for r in map(json.loads,p.stdout.splitlines())};record['responses']=replies;assert len(replies)==len(requests)
    for r in replies.values():assert 'result' in r,r
    result=lambda n:replies[n]['result']
    report=result(played);assert report['success'] and report['tick']==480 and report['stop_reason']=='replay_complete' and report['frames_presented']==481
    assert report['nvrhi_errors']==0 and report['hardware'] and report['capture_written']
    assert result(retry)=={**report,'replayed':True}
    for a,b in zip(first,second):
        left=dict(result(a));right=dict(result(b));left.pop('session_id');right.pop('session_id');assert left==right,(left,right)
    assert result(first_game)['module']==result(second_game)['module']
    assert result(first_game)['module']['values']['Activations']==1 and result(first_game)['module']['values']['Open']==1
    assert result(first[0])['world_matrix'][14]<-7 and abs(result(first[2])['world_matrix'][12]-2.2)<1e-5
    assert pixels(run/'player.bmp')==pixels(run/'baseline.bmp')
    for n in [capture,closed,opened]:assert result(n)['capture_written'] and result(n)['hardware'] and result(n)['nvrhi_errors']==0
    before=pixels(run/'closed.bmp');after=pixels(run/'open.bmp');assert max(abs(a-b) for a,b in zip(before[270][480],after[270][480]))>40
    assert result(collected)=={'active_modules':0,'retired_alive':0}
    record['checks']={'CSharp_use_opens_door_and_character_passes':True,'continuous_replay_matches_headless_state_and_CSharp_fields':True,'final_pixels_match_unculled_headless_capture':True,'retry_does_not_repeat_CSharp':True,'stationary_camera_observes_scripted_motion':True,'all_contexts_released':True}
    record['images']={p.name:{'path':str(p.resolve()),'sha256':sha(p)} for p in run.glob('*.bmp')};record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'ticks':480,'checks':record['checks']}))
