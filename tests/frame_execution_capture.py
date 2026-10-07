#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows native HWND frame-slot correctness; queue depth is not GPU overlap."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import traceback
import uuid
from scene_capture import pixels

def check(ok,why):
    if not ok:raise RuntimeError(str(why))
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,default=0)
    p.add_argument('--frames',type=int,default=60);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    check(not sys.flags.optimize,'Shared BMP parser requires assertions')
    run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True);prefix=run/'frame'
    argument=subprocess.check_output(['wslpath','-w',str(prefix)],text=True).strip() if a.windows_interop else str(prefix)
    evidence={'passed':False,'binary_sha256':sha(a.binary),'test_sha256':sha(__file__),
        'native_source_sha256':sha(Path(__file__).with_name('frame_execution_native.cpp')),'comparisons':[]}
    try:
        command=[str(a.binary.resolve()),argument,str(a.gpu),str(a.frames)];evidence['command']=command
        result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',timeout=600)
        evidence.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
        native=json.loads(result.stdout);evidence['native']=native
        check(result.returncode==0 and native.get('passed'),native)
        modes={mode['limit']:mode for mode in native['modes']};check(set(modes)=={1,2},'Missing execution mode')
        gpu_names=set();images={}
        for limit,mode in modes.items():
            for capture in mode['captures']:
                report=capture['report'];gpu_names.add(report['gpu'])
                check(report['hardware'] and report['success'] and report['errors']==0,'Capture hardware gate failed')
                check(report['submitted']==report['completed'] and report['outstanding']==0,'Capture retained pending submissions')
                path=Path(str(prefix)+f'-slots{limit}-'+capture['stage']+'.bmp')
                image=pixels(path);check(len(image)==report['height'] and len(image[0])==report['width'],'Capture extent differs')
                images[limit,capture['stage']]=image;capture['sha256']=sha(path)
        check(len(gpu_names)==1,'Selected GPU changed across devices/modes');evidence['gpu']=next(iter(gpu_names))
        check(modes[2]['two_slot_retirement_depth_exercised'],'No two-slot retention exercised')
        check(modes[2]['varying_warm']['submission_phase_peak_outstanding']==2,'No varying-phase two-slot submission observed')
        for mode in modes.values():
            check(mode['varying_warm']['verified_deferred_results']==49 and mode['varying_warm']['trace_dropped']==0,'Deferred attribution trace incomplete')
        def equal(left,right):
            a_image,b_image=images[left],images[right]
            check(len(a_image)==len(b_image) and all(len(a)==len(b) for a,b in zip(a_image,b_image)),'Comparison extent differs')
            changed=sum(a!=b for row,other in zip(a_image,b_image) for a,b in zip(row,other))
            maximum=max(abs(x-y) for row,other in zip(a_image,b_image) for a,b in zip(row,other) for x,y in zip(a,b))
            evidence['comparisons'].append({'left':left,'right':right,'changed_pixels':changed,'max_channel_difference':maximum})
            check(changed==0,'One/two-slot or fresh reference images differ')
        for stage in ('churn','secondary','overflow','varying','resized','recreated','final','fresh'):equal((1,stage),(2,stage))
        for limit in (1,2):
            equal((limit,'secondary'),(limit,'recreated'));equal((limit,'final'),(limit,'fresh'))
            check(images[limit,'churn']!=images[limit,'final'],'Changing scene produced unchanged image')
            # UI is deliberately opaque and away from geometry. The final texel
            # has an independent color oracle, not just cross-path equality.
            final=images[limit,'final'];expected=(111,149,80)
            check(max(abs(a-b) for a,b in zip(final[16][24],expected))<=1,'Final UI texture is stale or clobbered')
            check(sum(max(rgb)>65 for row in final[50:] for rgb in row)>300,'Skinned/light fixture lacks visible scene signal')
        evidence['passed']=True
    except BaseException:evidence['error']=traceback.format_exc()
    finally:(run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    check(evidence['passed'],'Frame execution qualification failed: '+str(run/'evidence.json'))
    print(json.dumps({'passed':True,'evidence':str(run/'evidence.json')}))
if __name__=='__main__':main()
