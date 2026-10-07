#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Explicit Vulkan HDR composition regression; --self-test needs no GPU.

A dim constant emissive silhouette identifies discrete MSAA coverage independently
of rasterizer sample locations. Bright captures must match tone mapping the mean
radiance, and must disagree with averaging already tone-mapped samples.
UI composition remains covered by ui_capture.py, not this scene-only fixture.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import traceback
import uuid
from scene_capture import pixels

W, H = 320, 240

def check(ok, message):
    if not ok:
        raise RuntimeError(message)

def encode(value):
    return round(255 * (12.92*value if value <= .0031308 else 1.055*value**(1/2.4)-.055))

def display(radiance, exposure=1):
    x = radiance*exposure
    return encode(x/(1+x))

def linear(byte):
    x = byte/255
    return x/12.92 if x <= .04045 else ((x+.055)/1.055)**2.4

def uid(n):
    return f'{n:032x}'

def component(n, kind, value):
    return {'op':'component.set','id':uid(n),'type':kind,'value':value}

def material(value):
    return {'base_color':[0,0,0],'emissive':[value]*3,'metallic':0,'roughness':1,'double_sided':False}

def environment(exposure, sky=0, enabled=True):
    return {'ambient':[0,0,0],'exposure':exposure,'sky':{'enabled':enabled,
        'zenith':[sky]*3,'horizon':[sky]*3,'ground':[sky]*3,'horizon_falloff':.35,
        'sun':None,'sun_size_degrees':1,'sun_intensity':0}}

def edge_checks(dim, bright):
    check(len(dim)==H and len(bright)==H and all(len(r)==W for r in dim+bright), 'Capture dimensions differ')
    coverage_counts = {1:0,2:0,3:0}
    wrong_distances = []
    maximum_error = 0
    for y in range(H):
        for x in range(W):
            p = dim[y][x]
            candidates = [k for k in (1,2,3) if max(abs(c-display(k/4)) for c in p)<=2]
            if len(candidates)!=1:
                continue
            k = candidates[0]; coverage_counts[k]+=1
            expected = display(16*k/4)
            wrong = encode((k/4)*(16/17))
            actual = bright[y][x]
            error = max(abs(c-expected) for c in actual)
            maximum_error = max(maximum_error,error)
            check(error<=3, f'Bright edge ({x},{y}) coverage {k}/4: {actual}, expected {expected}, pre-tonemapped resolve {wrong}')
            wrong_distances.append(min(abs(c-wrong) for c in actual))
    check(sum(coverage_counts.values())>=40, 'Too few actual partially covered pixels')
    check(sum(n>0 for n in coverage_counts.values())>=2, 'Insufficient MSAA coverage diversity')
    check(min(wrong_distances)>=12, 'Fixture does not discriminate resolve order')
    return {'coverage_counts':coverage_counts,'maximum_error':maximum_error,
            'minimum_distance_from_wrong_resolve':min(wrong_distances)}

def self_test():
    dim=[[(0,0,0)]*W for _ in range(H)]
    bright=[[(0,0,0)]*W for _ in range(H)]
    wrong=[[(0,0,0)]*W for _ in range(H)]
    for x in range(90):
        k=x%3+1
        dim[0][x]=(display(k/4),)*3
        bright[0][x]=(display(16*k/4),)*3
        wrong[0][x]=(encode(k/4*16/17),)*3
    edge_checks(dim,bright)
    rejected=False
    try:edge_checks(dim,wrong)
    except RuntimeError:rejected=True
    check(rejected, 'Reference test accepted deliberately incorrect resolve order')
    check(display(65504,1e-5)+30<display(1e6,1e-5),'Finite saturation oracle lacks contrast')
    check(all(display(x,0)==0 for x in (0,.025,.75,65504)), 'Zero exposure oracle differs')
    print('PASS: analytic oracle accepts HDR resolve and rejects tone-map-before-resolve; no GPU executed.')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path,nargs='?');p.add_argument('--output',type=Path)
    p.add_argument('--gpu',type=int,default=0);p.add_argument('--windows-interop',action='store_true')
    p.add_argument('--self-test',action='store_true');a=p.parse_args()
    check(not sys.flags.optimize,'Run without Python optimization: shared BMP parser uses assertions')
    if a.self_test:self_test();return
    check(a.binary is not None and a.output is not None,'binary and --output required')
    run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    report={'passed':False,'gpu':a.gpu,'runs':[],'checks':{},'images':{},
        'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'parser_sha256':hashlib.sha256(Path(__file__).with_name('scene_capture.py').read_bytes()).hexdigest()}
    world=run/'world.json';revision=0
    def native(path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if a.windows_interop else str(path.resolve())
    def batch(method,params):
        request={'jsonrpc':'2.0','id':uuid.uuid4().hex,'method':method,'params':params}
        process=subprocess.run([str(a.binary.resolve()),'world',native(world)],input=json.dumps(request)+'\n',capture_output=True,text=True,encoding='utf-8',timeout=120)
        entry={'request':request,'stdout':process.stdout,'stderr':process.stderr,'exit_code':process.returncode};report['runs'].append(entry)
        check(process.returncode==0,'Engine process failed')
        replies=[json.loads(line) for line in process.stdout.splitlines()]
        check(len(replies)==1 and replies[0].get('id')==request['id'] and 'result' in replies[0],'Unexpected RPC response')
        return replies[0]['result']
    def edit(ops):
        nonlocal revision
        batch('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':revision,'ops':ops});revision+=1
    def capture(name,samples):
        path=run/(name+'.bmp');before=world.read_bytes()
        result=batch('world.capture',{'revision':revision,'camera':uid(1),'path':native(path),'width':W,'height':H,'gpu':a.gpu,'samples':samples})
        check(result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,'Capture hardware/validation gate failed')
        check(result['samples']==samples,'Requested MSAA unavailable')
        check(world.read_bytes()==before,'Capture changed authored world')
        report['images'][name]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'path':str(path),'result':result}
        return pixels(path)
    def probe(image,x,y,value):
        actual=image[y][x];expected=display(value)
        check(max(abs(c-expected) for c in actual)<=3,f'Probe ({x},{y}) {actual} != {expected}')
        return {'actual':actual,'expected':expected}
    try:
        angle=math.radians(23)/2
        edit([{'op':'entity.create','id':uid(n),'name':name} for n,name in [(1,'Observer'),(2,'Environment'),(3,'No direct light'),(4,'Constant emissive silhouette')]]+[
            component(1,'Transform',{'position':[0,0,4],'rotation':[0,0,0,1],'scale':[1,1,1]}),
            component(1,'Camera',{'vertical_fov':60,'near':.1,'far':100}),
            component(2,'LightingEnvironment',environment(1)),
            component(3,'Light',{'kind':'directional','color':[1,1,1],'intensity':0,'enabled':True}),
            component(4,'Transform',{'position':[.017,.013,0],'rotation':[0,0,math.sin(angle),math.cos(angle)],'scale':[1.7,1.3,.1]}),
            component(4,'MeshRenderer',{'primitive':'box','albedo':[0,0,0],'visible':True}),component(4,'PbrMaterial',material(1))])
        dim=capture('dim-msaa4',4)
        edit([component(2,'LightingEnvironment',environment(16))]);bright=capture('bright-msaa4',4)
        report['checks']['resolve_order']=edge_checks(dim,bright)
        single=capture('bright-single',1)
        report['checks']['bright_interior']=probe(single,W//2,H//2,16)
        check(sum(abs(c-display(16))<=3 for c in bright[H//2][W//2])==3,'MSAA interior differs')
        for exposure in (1,4):
            edit([component(2,'LightingEnvironment',environment(exposure,.25)),component(4,'PbrMaterial',material(.5))])
            image=capture('common-exposure-'+str(exposure),4)
            report['checks']['exposure-'+str(exposure)]={'material':probe(image,W//2,H//2,.5*exposure),'sky':probe(image,5,5,.25*exposure)}
        # An authored lighting environment selects material shading even for
        # a MeshRenderer without explicit PbrMaterial. Verify its default
        # diffuse material from base*ambient, and the separate clear background.
        edit([{'op':'component.remove','id':uid(4),'type':'PbrMaterial'},
              component(4,'MeshRenderer',{'primitive':'box','albedo':[.2,.4,.6],'visible':True})])
        for exposure in (1,4):
            fill=environment(exposure,enabled=False);fill['ambient']=[.25]*3
            edit([component(2,'LightingEnvironment',fill)])
            image=capture('default-box-exposure'+str(exposure),4)
            actual=image[H//2][W//2]
            expected=[display(base*.25,exposure) for base in (.2,.4,.6)]
            check(max(abs(a-b) for a,b in zip(actual,expected))<=3,
                  f'Default box ambient/exposure differs: {actual} != {expected}')
            background=[display(c,exposure) for c in (.025,.035,.055)]
            check(max(abs(a-b) for a,b in zip(image[5][5],background))<=3,
                  'Default background omitted shared exposure/tone mapping')
            report['checks']['default_box_exposure_'+str(exposure)]={
                'actual':actual,'expected':expected,'background_actual':image[5][5],
                'background_expected':background}
        # White diffuse with no direct light has scene radiance exactly equal
        # to authored ambient. 1e6 exceeds finite binary16 range; use low
        # exposure so finite saturation is distinguishable from an unclamped
        # FP32 path (or infinity becoming NaN/black in Reinhard).
        extreme=environment(1e-5,.25);extreme['ambient']=[1e6]*3
        white=material(0);white['base_color']=[1,1,1]
        edit([component(4,'PbrMaterial',white),component(2,'LightingEnvironment',extreme)])
        image=capture('finite-hdr-saturation',4)
        actual=image[H//2][W//2];expected=display(65504,1e-5)
        unbounded=display(1e6,1e-5)
        check(max(abs(c-expected) for c in actual)<=3,
              f'Finite HDR saturation differs: {actual}, expected {expected}')
        check(min(abs(c-unbounded) for c in actual)>=30,
              'Saturation fixture does not distinguish unclamped radiance')
        report['checks']['finite_hdr_saturation']={'actual':actual,'expected':expected,
            'unbounded_reference':unbounded,'ambient':1e6,'exposure':1e-5,'storage_maximum':65504}
        # Zero exposure must suppress both finite-saturated material and sky;
        # repeat with sky disabled to exercise the separate clear-background path.
        for enabled in (True,False):
            zero=environment(0,.75,enabled=enabled);zero['ambient']=[1e6]*3
            edit([component(2,'LightingEnvironment',zero)])
            image=capture('zero-exposure-sky-'+str(enabled).lower(),4)
            maximum=max(c for row in image for rgb in row for c in rgb)
            check(maximum==0,'Zero exposure left nonblack scene pixels')
            report['checks']['zero_exposure_sky_'+str(enabled).lower()]={'maximum_channel':maximum}
        # True legacy preview requires absence of authored light/environment
        # and material. Rotation about Z leaves the visible front normal +Z.
        edit([{'op':'component.remove','id':uid(n),'type':kind} for n,kind in
              ((2,'LightingEnvironment'),(3,'Light'),(4,'PbrMaterial'))])
        image=capture('legacy-preview-default-exposure',4)
        shade=.18+.82*.6/math.sqrt(.4**2+.8**2+.6**2)
        expected=[display(base*shade) for base in (.2,.4,.6)]
        old_unmapped=[encode(base*shade) for base in (.2,.4,.6)]
        actual=image[H//2][W//2]
        check(max(abs(a-b) for a,b in zip(actual,expected))<=3,
              f'Legacy preview shared tone mapping differs: {actual} != {expected}')
        check(max(abs(a-b) for a,b in zip(actual,old_unmapped))>=12,
              'Legacy fixture does not distinguish old unmapped output')
        report['checks']['legacy_preview']={'actual':actual,'expected':expected,
            'old_unmapped_reference':old_unmapped,'diffuse_shade':shade}
        report['passed']=True
    except BaseException:report['error']=traceback.format_exc()
    finally:(run/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
    check(report['passed'],'HDR qualification failed; see '+str(run/'evidence.json'))
    print(json.dumps({'passed':True,'evidence':str(run/'evidence.json')}))

if __name__=='__main__':main()
