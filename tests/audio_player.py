#!/usr/bin/env python3
"""Qualify native player audio queue and compare its mixer with headless replay."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid
p=argparse.ArgumentParser();p.add_argument('binary',type=Path);p.add_argument('--windows-interop',action='store_true');p.add_argument('--fail-device',action='store_true');p.add_argument('--gpu',type=int,default=0);p.add_argument('--evidence',type=Path,required=True)
args=p.parse_args();root=Path(__file__).resolve().parents[1];run=root/'build/audio-player'/uuid.uuid4().hex;run.mkdir(parents=True)
def uid(n):return f'{n:032x}'
def native(path):return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'calls':[],'run':str(run)}
environment=dict(os.environ)
if args.fail_device:environment['SDL_AUDIO_DRIVER']='poima-test-invalid'
process=subprocess.Popen([str(args.binary.resolve()),'world',native(run/'world.json')],env=environment,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def rpc(method,params):
    request={'jsonrpc':'2.0','id':len(record['calls'])+1,'method':method,'params':params};process.stdin.write(json.dumps(request)+'\n');process.stdin.flush();line=process.stdout.readline();assert line,'Player terminated before reply';reply=json.loads(line);record['calls'].append({'request':request,'response':reply});assert 'result' in reply,reply;return reply['result']
try:
    asset=rpc('asset.audio.import',{'source':native(root/'examples/assets/acoustic-probe.wav')})['asset']
    fixture=json.loads((root/'examples/interaction-room.jsonl').read_text());fixture['params']['ops'] += [
        {'op':'component.set','id':uid(2),'type':'AudioEmitter','value':{'asset':asset,'gain':.25,'loop':True,'enabled':True}},
        {'op':'component.set','id':uid(2),'type':'AcousticMaterial','value':{'absorption':[.1,.2,.3],'transmission':[.5,.15,.03],'scattering':.5,'enabled':True}}]
    rpc(fixture['method'],fixture['params']);rpc('runtime.start',{'session_id':uid(900),'revision':1})
    sequence=[{'ticks':30,'sounds':[{'op':'play','emitter':uid(2)}]}, {'ticks':60,'motions':[{'entity':uid(2),'position':[3,1.5,-3],'rotation':[0,0,0,1],'duration_ticks':60}]},{'ticks':30,'sounds':[{'op':'stop','voice':1}]}]
    play={'session_id':uid(900),'request_id':uid(1000),'expected_tick':0,'controller':uid(100),'camera':uid(101),'mode':'replay','sequence':sequence,'audio':True,'gpu':args.gpu,'width':512,'height':288}
    played=rpc('runtime.play',play)
    if args.fail_device:
        assert not played['success'] and played['tick']==0 and not played['audio']['enabled'] and 'poima-test-invalid' in played['detail'],played
    else:
        assert played['success'],played
        assert played['tick']==120 and played['audio']['enabled'] and played['audio']['stream_drained'],played
        assert played['audio']['submitted_frames']==120*800 and played['audio']['voices_started']==1,played
        assert played['audio']['max_queued_frames']<=5824 and played['audio']['peak']>0,played
        assert played['audio']['driver'] not in ['dummy','disk','unknown'],played
    retry=rpc('runtime.play',play);assert retry=={**played,'replayed':True}
    if not args.fail_device:
        rpc('runtime.stop',{'session_id':uid(900)});rpc('runtime.start',{'session_id':uid(901),'revision':1})
    capture_session=uid(900 if args.fail_device else 901)
    audio_sequence=[{**segment,'inputs':[{'entity':uid(100)}]} for segment in sequence]
    captured=rpc('runtime.audio.replay',{'session_id':capture_session,'request_id':uid(1001),'expected_tick':0,'listener':uid(101),'sequence':audio_sequence,'path':native(run/'comparison.wav')});assert captured['success'],captured
    if not args.fail_device:
        for key in ['peak','over_range_samples','voices_started']:
            assert captured['stream'][key]==played['audio'][key],(key,captured,played)
        assert captured['capture']['frames']==played['audio']['submitted_frames']
    record.update({'failed_device_test':args.fail_device,'passed':True,'driver':played['audio']['driver'],'device':played['audio'],'capture':captured['capture'],'scope':'Failed output initialization retains tick zero and a retry receipt; headless audio remains usable in the same runtime session.' if args.fail_device else 'Actual output stream submission/drain; same mixer peak/event count as headless replay. No loopback recording or subjective listening claim.'})
finally:
    process.stdin.close();process.wait(timeout=10);record['stderr']=process.stderr.read();record['exit_code']=process.returncode
    args.evidence.parent.mkdir(parents=True,exist_ok=True);args.evidence.write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({k:v for k,v in record.items() if k not in ['calls']},indent=2))
