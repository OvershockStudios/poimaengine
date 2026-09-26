#!/usr/bin/env python3
"""Check that Vulkan observations follow actual Jolt state, not authored poses."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    run=args.output/uuid.uuid4().hex;run.mkdir()
    def native(path):
        value=str(path.resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    def uid(n):return f'{n:032x}'
    requests=[];captures={}
    def request(method,params):
        requests.append({'jsonrpc':'2.0','id':len(requests)+1,'method':method,'params':params});return len(requests)
    def capture(name,tick,camera=200,method='runtime.capture',revision=None):
        path=run/(name+'.bmp');params={'camera':uid(camera),'path':native(path),'gpu':args.gpu,'width':960,'height':540,'samples':4}
        if method=='runtime.capture':params.update(session_id=uid(900),tick=tick)
        else:params['revision']=revision
        n=request(method,params);captures[name]=(n,path);return n
    fixture=json.loads((Path(__file__).resolve().parents[1]/'examples/physics-room.jsonl').read_text())
    request('world.transact',fixture['params']);request('runtime.start',{'session_id':uid(900),'revision':1})
    capture('spawn',0)
    request('runtime.step',{'session_id':uid(900),'request_id':uid(901),'expected_tick':0,'ticks':120})
    capture('settled',120)
    player=request('runtime.entity',{'session_id':uid(900),'id':uid(100),'tick':120})
    camera=capture('first-person',120,camera=101)
    # Move the authored crate after start; both observations must expose the
    # correct source rather than mixing authored/live state under one revision.
    request('world.transact',{'request_id':uid(902),'base_revision':1,'ops':[{'op':'component.set','id':uid(3),'type':'Transform',
        'value':{'position':[2,7,0],'rotation':[0,0,0,1],'scale':[1,.8,1]}}]})
    capture('runtime-after-author-edit',120)
    capture('authored-after-edit',0,method='world.capture',revision=2)
    inspected=request('runtime.inspect',{'session_id':uid(900)})
    command=[str(args.binary.resolve()),'world',native(run/'world.json')]
    p=subprocess.run(command,input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,encoding='utf-8',timeout=120)
    record={'command':command,'requests':requests,'exit_code':p.returncode,'stderr':p.stderr}
    try:
        assert p.returncode==0,p.stdout+p.stderr
        responses={r['id']:r for r in map(json.loads,p.stdout.splitlines())};record['responses']=responses
        assert len(responses)==len(requests)
        for response in responses.values():assert 'result' in response,response
        record['captures']={};images={}
        for name,(n,path) in captures.items():
            report=responses[n]['result'];assert report['capture_written'] and report['nvrhi_errors']==0
            assert report['hardware'] and report['samples']==4
            images[name]=pixels(path)
            record['captures'][name]={'report':report,'path':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        def red_centroid_y(image):
            points=[y for y,row in enumerate(image) for r,g,b in row if r>70 and r>g*1.7 and r>b*1.7]
            assert len(points)>500,len(points)
            return sum(points)/len(points)
        before=red_centroid_y(images['spawn']);after=red_centroid_y(images['settled'])
        assert after-before>40,(before,after)
        assert images['settled']==images['runtime-after-author-edit']
        assert images['settled']!=images['authored-after-edit']
        current=responses[inspected]['result'];assert current['source_stale'] and current['tick']==120
        observation=responses[camera]['result'];position=responses[player]['result']['world_matrix']
        assert observation['source']=='runtime' and observation['tick']==120 and observation['revision']==1
        assert abs(observation['camera_world'][13]-(position[13]+1.6))<1e-6
        assert record['captures']['authored-after-edit']['report']['revision']==2
        assert record['captures']['authored-after-edit']['report']['tick'] is None
        record['checks']={'falling_body_pixel_motion':True,'crate_pixel_y_before':before,'crate_pixel_y_after':after,
            'live_capture_unchanged_by_authored_edit':True,'authored_capture_reflects_edit':True,'first_person_camera_follows_controller':True,
            'source_revision_tick_metadata':True}
        record['passed']=True
    finally:
        (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'passed':True,'checks':record['checks'],'evidence':str(args.output/f'gpu-{args.gpu}.json')}))


if __name__=='__main__':main()
