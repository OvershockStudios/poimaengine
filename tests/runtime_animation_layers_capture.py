#!/usr/bin/env python3
"""Compare Vulkan layer captures with independently authored ribbon poses.

Only bounded pose correctness is qualified; these captures measure neither
character quality nor game performance. References never call a layer evaluator.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import uuid
from gltf_fixture import glb
from scene_capture import pixels
from runtime_animation_blend_contract import blend_fixture, command, component, node, transform, uid
from runtime_animation_inertial_contract import correction


def require(value, message):
    if not value:
        raise AssertionError(message)


def authored(slot=1, clip=1, weight=.5, mode='override', mask_weight=1, **extra):
    return dict(slot=slot, mode=mode, clip=clip, time=0, speed=1, loop=False,
                playing=False, weight=weight, mask=[dict(node=1, weight=mask_weight)], **extra)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    run = args.output.resolve()/uuid.uuid4().hex; run.mkdir(parents=True)
    world = run/'world.json'
    report = dict(passed=False, gpu=args.gpu, runs=[], comparisons=[],
        binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256((Path(__file__).parent/'runtime_animation_blend_contract.py').read_bytes()).hexdigest(),
        limits=['Bounded640x480 original ribbon, not a game-performance or production-character benchmark.',
                'Static authored references cover masked composition and skinning; no physical input or installed GUI.',
                'These scenes cover translation and one fractional rotation; broader rotation/scale, saves and rejection use independent native/protocol suites.'])

    def native(path):
        if args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True,timeout=10).strip()
        return str(path.resolve())

    def batch(requests):
        rows = [dict(jsonrpc='2.0', id=i, method=method, params=params)
                for i,(method,params) in enumerate(requests)]
        proc = subprocess.run([str(args.binary.resolve()),'world',native(world)],
            input=''.join(json.dumps(row)+'\n' for row in rows), text=True, encoding='utf-8',
            capture_output=True, timeout=240)
        replies = [json.loads(line) for line in proc.stdout.splitlines()]
        report['runs'].append(dict(requests=rows,responses=replies,exit_code=proc.returncode,stderr=proc.stderr))
        require(proc.returncode==0 and len(replies)==len(rows), 'Owned capture process or response count failed.')
        for i,reply in enumerate(replies):
            require(reply.get('id')==i and 'result' in reply, 'Capture RPC failed or returned a mismatched correlation: '+str(reply))
        return [reply['result'] for reply in replies]

    rig, tip, camera = uid(100), node(uid(100),1), uid(1)
    def lc(clip=1, weight=1, **extra):
        values = dict(layer=1,weight=weight,playing=False,loop=False); values.update(extra)
        return command(rig,clip=clip,**values)
    def base(clip=1, **extra):
        values=dict(playing=False,loop=False);values.update(extra)
        return command(rig,clip=clip,**values)

    # Every reference is computed from written clip endpoint facts, elementary
    # weighted arithmetic, or the independent six-boundary-condition solve.
    cases = [
        dict(name='sparse-fraction', layers=[authored(mask_weight=.5)], steps=[(30,[])],
             position=(.875,1.5,0), weight=.5),
        dict(name='additive-frozen-reference', layers=[authored(mode='additive',reference_clip=0,reference_time=.5)],
             steps=[(30,[])],position=(1.25,2,0),weight=.5),
        dict(name='additive-reference-later', layers=[authored(mode='additive',reference_clip=0,reference_time=.5)],
             steps=[(60,[])],position=(1.75,2,0),weight=.5),
        dict(name='sorted-overlap', layers=[authored(slot=4,clip=0),authored(slot=1,clip=1)],
             steps=[(30,[])],position=(.625,1.5,0),weight=.5),
        dict(name='weight-midpoint', layers=[authored(weight=0)],
             steps=[(30,[lc(weight=1,weight_blend_ticks=60)])],position=(1.25,2,0),weight=.5),
        dict(name='weight-interrupted', layers=[authored(weight=0)],
             steps=[(30,[lc(weight=1,weight_blend_ticks=60)]),(15,[lc(weight=0,weight_blend_ticks=30)])],
             position=(1.0625,1.5,0),weight=.25),
        dict(name='layer-inertial', layers=[authored(weight=1)],
             steps=[(30,[]),(15,[lc(clip=0,blend_ticks=60,transition_mode='inertial')])],
             position=(correction(2,0,.25,1),1+correction(2,0,.25,1),0),weight=1),
        dict(name='base-history-isolated', layers=[authored(weight=1)],
             steps=[(30,[]),(15,[base(blend_ticks=60,transition_mode='inertial'),lc(weight=0)])],
             position=(2+correction(-1.5,1,.25,1),3+correction(-2,0,.25,1),0),weight=0),
        dict(name='fractional-rotation', layers=[dict(authored(clip=3),time=1)],steps=[(30,[])],
             position=(.25,1,0),weight=.5,rotation=(0,0,math.sin(math.pi/8),math.cos(math.pi/8))),
    ]
    try:
        doc,blob=blend_fixture();source=run/'original.glb';source.write_bytes(glb(doc,blob))
        asset=batch([('asset.import',dict(source=native(source)))])[0]['asset'];source.unlink()
        batch([('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=[
            dict(op='asset.instantiate',id=rig,name='Independent layer ribbon',asset=asset),
            dict(op='entity.create',id=camera,name='Observer'),
            component(camera,'Transform',transform((.75,1.25,5))),
            component(camera,'Camera',dict(vertical_fov=50,near=.1,far=100))]))])
        revision=1
        for samples in [1,4]:
            requests=[];indices=[]
            def capture(name):
                return dict(camera=camera,path=native(run/f'{samples}x-{name}.bmp'),
                            width=640,height=480,gpu=args.gpu,samples=samples,profile=True)
            for case in cases:
                name=case['name'];session=uuid.uuid4().hex;tick=0
                rig_value=dict(asset=asset,clip=0,time=0,speed=1,loop=False,playing=True,layers=case['layers'])
                requests.append(('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,
                    ops=[component(tip,'Transform',transform((0,1,0))),component(rig,'AnimationRig',rig_value)])))
                revision+=1
                requests.append(('runtime.start',dict(session_id=session,revision=revision)))
                for count,commands in case['steps']:
                    requests.append(('runtime.step',dict(session_id=session,request_id=uuid.uuid4().hex,
                        expected_tick=tick,ticks=count,animations=commands)))
                    tick+=count
                live_index=len(requests);requests.append(('runtime.capture',dict(session_id=session,tick=tick,**capture(name))))
                pose_index=len(requests);requests.append(('runtime.entity',dict(session_id=session,id=tip)))
                state_index=len(requests);requests.append(('runtime.entity',dict(session_id=session,id=rig)))
                requests.append(('runtime.stop',dict(session_id=session)))
                reference=transform(case['position']);reference['rotation']=list(case.get('rotation',(0,0,0,1)))
                requests.append(('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,
                    ops=[component(tip,'Transform',reference)])))
                revision+=1
                reference_index=len(requests);requests.append(('world.capture',dict(revision=revision,**capture(name+'-authored'))))
                indices.append((case,live_index,pose_index,state_index,reference_index))
            results=batch(requests)
            images={}
            for case,live_index,pose_index,state_index,reference_index in indices:
                for index in [live_index,reference_index]:
                    value=results[index]
                    require(value['capture_written'] and value['hardware'] and value['nvrhi_errors']==0,
                            'Vulkan capture failed or reported NVRHI errors: '+str(value))
                actual=results[pose_index]['local_transform']
                require(all(abs(a-b)<1e-8 for a,b in zip(actual['position'],case['position'])),
                        'Layer pose differs from independent position reference: '+case['name'])
                expected_rotation=case.get('rotation',(0,0,0,1))
                require(abs(abs(sum(a*b for a,b in zip(actual['rotation'],expected_rotation)))-1)<1e-8,
                        'Layer quaternion differs from independently authored orientation.')
                layers=results[state_index]['animation']['layers']
                require(layers[0]['weight']==case['weight'], 'Effective layer weight differs.')
                require([row['slot'] for row in layers]==sorted(row['slot'] for row in layers), 'Layer order differs.')
                if case['name']=='layer-inertial':
                    require(layers[0]['playback']['transition']['mode']=='inertial', 'Layer did not use actual inertial clock.')
                if case['name']=='base-history-isolated':
                    require(results[state_index]['animation']['transition']['mode']=='inertial', 'Base did not use actual inertial clock.')
                live_path=run/f"{samples}x-{case['name']}.bmp"
                ref_path=run/f"{samples}x-{case['name']}-authored.bmp"
                live,reference=pixels(live_path),pixels(ref_path)
                require(len(live)==len(reference) and all(len(a)==len(b) for a,b in zip(live,reference)), 'Capture dimensions differ.')
                differences=[max(abs(a-b) for a,b in zip(p,q)) for row,other in zip(live,reference) for p,q in zip(row,other)]
                stats=dict(samples=samples,case=case['name'],position=list(case['position']),
                    rotation=list(expected_rotation),changed_pixels=sum(x!=0 for x in differences),
                    maximum_channel_difference=max(differences),
                    live_capture_report_sha256=hashlib.sha256(json.dumps(results[live_index],sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest(),
                    authored_capture_report_sha256=hashlib.sha256(json.dumps(results[reference_index],sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest(),
                    live_sha256=hashlib.sha256(live_path.read_bytes()).hexdigest(),
                    authored_sha256=hashlib.sha256(ref_path.read_bytes()).hexdigest())
                report['comparisons'].append(stats);require(stats['changed_pixels']==0,'Layer pixels differ: '+str(stats))
                images[case['name']]=live
            changed=sum(p!=q for row,other in zip(images['fractional-rotation'],images['additive-reference-later']) for p,q in zip(row,other))
            require(changed>1000,'Reference fixture did not produce visibly distinct ribbon poses.')
            report.setdefault('motion_pixels',{})[str(samples)]=changed
        report['capture_count']=len(cases)*4;report['passed']=True
    except BaseException as error:
        report['failure']=dict(type=type(error).__name__,message=str(error));raise
    finally:
        evidence=run/'evidence.json';evidence.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(evidence)


if __name__=='__main__':
    main()
