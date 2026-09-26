#!/usr/bin/env python3
"""Qualify continuous Vulkan replay against the same headless stepping API."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--gpu',type=int,default=0)
parser.add_argument('--authored-lights',action='store_true')
parser.add_argument('--shadows',action='store_true')
parser.add_argument('--profile',action='store_true')
parser.add_argument('--kinematic',action='store_true')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
if args.shadows and not args.authored_lights:parser.error('--shadows requires --authored-lights')
run=args.output/uuid.uuid4().hex;run.mkdir()
def native(path):
    text=str(path.resolve())
    return subprocess.check_output(['wslpath','-w',text],text=True).strip() if args.windows_interop else text
def uid(n):return f'{n:032x}'
requests=[]
def request(method,params):
    requests.append({'jsonrpc':'2.0','id':len(requests)+1,'method':method,'params':params});return len(requests)
fixture=json.loads((Path(__file__).resolve().parents[1]/'examples/physics-room.jsonl').read_text())
if args.authored_lights:
    def component(n,kind,value):return {'op':'component.set','id':uid(n),'type':kind,'value':value}
    for n,parent,kind,position,color,intensity in [(201,3,'point',[0,2,0],[1,.3,.1],80),(202,101,'spot',[.3,-.2,0],[.2,.5,1],120)]:
        fixture['params']['ops'] += [
            {'op':'entity.create','id':uid(n),'parent':uid(parent),'name':'Moving light'},
            component(n,'Transform',{'position':position,'rotation':[0,0,0,1],'scale':[1,1,1]}),
            component(n,'Light',{'kind':kind,'color':color,'intensity':intensity,'range':20,'enabled':True})]
    fixture['params']['ops'].append(component(101,'LightingEnvironment',{'ambient':[.03,.03,.03],'exposure':1}))
    if args.shadows:
        for op in fixture['params']['ops']:
            if op.get('type')=='Light':op['value']['shadow']={'enabled':True,'distance':20}
            if op.get('type')=='LightingEnvironment':op['value']['shadow_resolution']=512
sequence=[{'ticks':120},{'ticks':60,'move':[0,1]},{'ticks':10,'jump':True},{'ticks':120},{'ticks':1,'look':[45,20]},{'ticks':60,'move':[.4,-.7]}]
if args.kinematic:
    for op in fixture['params']['ops']:
        if op.get('type')=='BoxCollider' and op['id']==uid(2):op['value']['motion']='kinematic'
    sequence[1]['motions']=[{'entity':uid(2),'position':[8,1.5,-3],'rotation':[0,0,0,1],'duration_ticks':120}]
total=sum(s['ticks'] for s in sequence)
request('world.transact',fixture['params']);request('runtime.start',{'session_id':uid(900),'revision':1})
if args.kinematic:
    query={'session_id':uid(900),'tick':0,'origin':[0,1.5,2],'direction':[0,0,-1],'distance':10,'ignore':[uid(100)]}
    initial_ray=request('runtime.raycast',query)
play={'session_id':uid(900),'request_id':uid(1000),'expected_tick':0,'controller':uid(100),'camera':uid(101),'mode':'replay',
      'sequence':sequence,'gpu':args.gpu,'path':native(run/'player.bmp'),'profile':args.profile}
played=request('runtime.play',play)
retry=request('runtime.play',play)
changed=request('runtime.play',{**play,'sequence':[{'ticks':1}]})
conflict=request('runtime.step',{'session_id':uid(900),'request_id':uid(1000),'expected_tick':total,'ticks':1})
stale=request('runtime.play',{**play,'request_id':uid(1001),'path':native(run/'stale.bmp')})
first_states=[request('runtime.entity',{'session_id':uid(900),'id':uid(n),'tick':total}) for n in ([100,101,3,2] if args.kinematic else [100,101,3])]
if args.kinematic:final_ray=request('runtime.raycast',{**query,'tick':total})
request('runtime.stop',{'session_id':uid(900)});request('runtime.start',{'session_id':uid(901),'revision':1})
tick=0
for segment in sequence:
    control={k:v for k,v in segment.items() if k not in ('ticks','motions')};control['entity']=uid(100)
    request('runtime.step',{'session_id':uid(901),'request_id':uuid.uuid4().hex,'expected_tick':tick,'ticks':segment['ticks'],'inputs':[control],'motions':segment.get('motions',[])});tick+=segment['ticks']
second_states=[request('runtime.entity',{'session_id':uid(901),'id':uid(n),'tick':total}) for n in ([100,101,3,2] if args.kinematic else [100,101,3])]
baseline=request('runtime.capture',{'session_id':uid(901),'tick':total,'camera':uid(101),'gpu':args.gpu,'path':native(run/'baseline.bmp'),'profile':args.profile,'culling':not args.profile})
# A graphics initialization failure must expose the current tick, be retryable
# without relaunching, and allow a new request to recover in the same runtime.
bad={**play,'session_id':uid(901),'request_id':uid(1002),'expected_tick':total,'sequence':[{'ticks':1}],'gpu':4095,'path':native(run/'bad.bmp')}
failed=request('runtime.play',bad);failed_retry=request('runtime.play',bad)
recovered=request('runtime.play',{**bad,'request_id':uid(1003),'gpu':args.gpu,'path':native(run/'recovered.bmp')})
inspected=request('runtime.inspect',{'session_id':uid(901)})
command=[str(args.binary.resolve()),'world',native(run/'world.json')]
p=subprocess.run(command,input=''.join(json.dumps(r)+'\n' for r in requests),text=True,encoding='utf-8',capture_output=True,timeout=120)
record={'command':command,'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'authored_lights':args.authored_lights,'shadows':args.shadows,'kinematic':args.kinematic,'exit_code':p.returncode,'stderr':p.stderr,'requests':requests}
try:
    assert p.returncode==0,p.stdout+p.stderr
    responses={r['id']:r for r in map(json.loads,p.stdout.splitlines())};record['responses']=responses
    assert len(responses)==len(requests)
    expected_errors={changed:-32010,conflict:-32010,stale:-32009}
    for n,response in responses.items():
        if n in expected_errors:assert response['error']['code']==expected_errors[n],response
        else:assert 'result' in response,response
    report=responses[played]['result'];assert report['success'] and report['stop_reason']=='replay_complete',report
    assert report['tick']==total and report['previous_tick']==0 and report['frames_presented']>=total
    assert report['hardware'] and report['nvrhi_errors']==0 and report['capture_written']
    assert responses[retry]['result']=={**report,'replayed':True}
    for first,second in zip(first_states,second_states):
        a=dict(responses[first]['result']);b=dict(responses[second]['result']);a.pop('session_id');b.pop('session_id');assert a==b,(a,b)
    assert report['camera_world']==responses[baseline]['result']['camera_world']
    assert report['lighting']==responses[baseline]['result']['lighting']
    if args.authored_lights:
        assert not report['lighting']['preview_fallback'] and len(report['lighting']['lights'])==2
        # The character has turned and the box has fallen: compare both attachments
        # to the independently stepped runtime, not to authored spawn transforms.
        lights={light['id']:light for light in report['lighting']['lights']}
        assert abs(lights[uid(201)]['position'][1]-5)>.5
        assert abs(lights[uid(202)]['direction'][0])>.1
        if args.shadows:assert report['lighting']['shadow_views']==7 and report['lighting']['shadow_bytes']==7*512*512*4
    if args.kinematic:
        assert responses[initial_ray]['result']['hit']['entity']==uid(2)
        assert responses[final_ray]['result']['hit'] is None
        door=responses[first_states[-1]]['result'];assert abs(door['world_matrix'][12]-8)<1e-5 and door['kinematic_target'] is None
    assert pixels(run/'player.bmp')==pixels(run/'baseline.bmp')
    diagnostics=report['render_diagnostics'];assert diagnostics['completed_submissions']==report['frames_presented']
    if args.profile:
        uncull=responses[baseline]['result']['render_diagnostics'];assert not uncull['culling'] and diagnostics['culling']
        assert diagnostics['gpu']['available'] and diagnostics['gpu']['total']['samples']==report['frames_presented']
        assert diagnostics['gpu']['samples_dropped']==0
        for key in ['camera_draws','shadow_draws']:assert diagnostics['last_draws'][key]<=uncull['last_draws'][key]
        record['profile_comparison']={'culled_player':diagnostics,'unculled_capture':uncull}
    assert not responses[failed]['result']['success'] and responses[failed]['result']['tick']==total
    assert responses[failed_retry]['result']=={**responses[failed]['result'],'replayed':True}
    assert responses[recovered]['result']['success'] and responses[recovered]['result']['tick']==total+1
    assert responses[inspected]['result']['tick']==total+1
    assert not (run/'stale.bmp').exists() and not (run/'bad.bmp').exists()
    saved=json.loads((run/'world.json').read_text());assert saved['revision']==1
    assert saved['entities'][uid(100)]['components']['Transform']['position']==[0,1,2]
    record['checks']={'continuous_replay_matches_headless_state':True,'final_pixels_match_independent_capture':True,'retry_does_not_advance':True,
        'stale_tick_guard':True,'request_id_cross_method_conflict':True,'device_failure_state_and_recovery':True,'authored_spawn_unchanged':True}
    record['images']={name:{'path':str((run/name).resolve()),'sha256':hashlib.sha256((run/name).read_bytes()).hexdigest()} for name in ['player.bmp','baseline.bmp','recovered.bmp']}
    record['passed']=True
finally:
    (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'gpu':args.gpu,'ticks':total,'checks':record['checks']}))
