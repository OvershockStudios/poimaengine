#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual Vulkan depth/world-shading-normal debug products against analytic geometry.
BMP checks qualify displayed products, not exact raw floating-point attachments.
"""
import argparse,hashlib,json,math,struct,subprocess,sys,traceback,uuid
from pathlib import Path
from texture_fixture import quad,png
from animation_fixture import ribbon
from gltf_fixture import glb
from scene_capture import pixels

def check(ok,why):
    if not ok:raise RuntimeError(str(why))
def uid(n):return f'{n:032x}'
def transform(position=(0,0,0),rotation=(0,0,0,1)):
    return {'position':list(position),'rotation':list(rotation),'scale':[1,1,1]}
def component(n,kind,value):return {'op':'component.set','id':uid(n) if isinstance(n,int) else n,'type':kind,'value':value}
def normal_rgb(normal):return tuple(round((x*.5+.5)*255) for x in normal)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reference-binary',type=Path);p.add_argument('--gpu',type=int,default=0);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    check(not sys.flags.optimize,'Shared BMP reader requires assertions')
    run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'world.json';revision=0
    report={'passed':False,'runs':[],'checks':{},'images':{},'gpu_index':a.gpu,
            'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'fixture_sources':{name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() for name in ('texture_fixture.py','animation_fixture.py','gltf_fixture.py','scene_capture.py')},
            'limits':'Quantized displayed depth/world shading normals only; no raw attachment readback or temporal history claim.'}
    if a.reference_binary:report['reference_binary_sha256']=hashlib.sha256(a.reference_binary.read_bytes()).hexdigest()
    def native(path):return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if a.windows_interop else str(path.resolve())
    def batch(requests):
        rows=[{'jsonrpc':'2.0','id':i,'method':m,'params':params} for i,(m,params) in enumerate(requests)]
        entry={'requests':rows};report['runs'].append(entry)
        result=subprocess.run([str(a.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,encoding='utf-8',timeout=180)
        entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
        check(result.returncode==0,'Engine process failed');answers=[json.loads(line) for line in result.stdout.splitlines()]
        check(len(answers)==len(rows),'Response count differs')
        for expected,answer in zip(rows,answers):check(answer.get('id')==expected['id'] and 'result' in answer,answer)
        return [r['result'] for r in answers]
    def edit(ops):
        nonlocal revision
        batch([('world.transact',{'base_revision':revision,'request_id':uuid.uuid4().hex,'ops':ops})]);revision+=1
    def imported(name,doc,blob):
        path=run/(name+'.glb');path.write_bytes(glb(doc,blob));return batch([('asset.import',{'source':native(path)})])[0]['asset']
    def request(name,view,method='world.capture',extra=None):
        params={'revision':revision,'camera':uid(1),'path':native(run/(name+'.bmp')),'width':320,'height':240,'gpu':a.gpu,'samples':1,'scene_debug_view':view}
        if method=='runtime.capture':params.pop('revision')
        params.update(extra or {});return method,params
    def image(name,result,view):
        check(result.get('capture_written') and result.get('hardware') and result.get('nvrhi_errors')==0,result)
        check(result['samples']==1,'Debug capture not single-sampled')
        product=result['render_diagnostics']['scene_products']
        check(product['available'] is True and product['view']==view,product)
        check(product['normal_buffer_bytes']==320*240*8,product)
        check(product['normal_format']=='RGBA16_FLOAT' and product['normal_space']=='world' and product['normal_alpha']=='surface validity',product)
        check(product['depth_convention']=='device depth [0,1], near 0, far/clear 1',product)
        check(product['depth_visualization']=='positive view distance divided by far; invalid surface black',product)
        gpu=result['gpu'];check(isinstance(gpu,str) and gpu.strip(),'Missing actual GPU identity')
        check(report.setdefault('gpu_name',gpu)==gpu,'GPU changed during capture cohort')
        path=run/(name+'.bmp');data=pixels(path);check(len(data)==240 and len(data[0])==320,'Unexpected extent')
        report['images'][name]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'report':result};return data
    def capture(name,view):return image(name,batch([request(name,view)])[0],view)
    def probe(name,data,expected,position=(160,120)):
        x,y=position;actual=data[y][x];check(max(abs(a-b) for a,b in zip(actual,expected))<=2,(name,actual,expected))
        report['checks'][name]={'actual':actual,'expected':expected,'pixel':position}
    def environment(exposure):return {'ambient':[.2,.2,.2],'exposure':exposure,'sky':{'enabled':True,'zenith':[.2,.4,.8],'horizon':[.4,.5,.6],'ground':[.1,.1,.1],'horizon_falloff':.35,'sun':None,'sun_size_degrees':1,'sun_intensity':0}}
    try:
        material={'pbrMetallicRoughness':{'baseColorFactor':[.5,.5,.5,1],'metallicFactor':0,'roughnessFactor':.6},'doubleSided':True}
        doc,blob=quad([],material);plain=imported('plain',doc,blob)
        reversed_blob=bytearray(blob);struct.pack_into('<6I',reversed_blob,128,0,2,1,0,3,2);back=imported('backface',doc,bytes(reversed_blob))
        ndoc,nblob=quad([png(1,1,[204,76,230,255])],{**material,'normalTexture':{'index':0}});mapped=imported('normalmapped',ndoc,nblob)
        rigdoc,rigblob=ribbon();rig=imported('ribbon',rigdoc,rigblob)
        mesh=hashlib.sha256(('poima.instance.v1/'+uid(10)+'/node/0/primitive/0').encode()).hexdigest()[:32]
        edit([{'op':'entity.create','id':uid(1),'name':'Observer'},component(1,'Transform',transform((0,0,4))),
              component(1,'Camera',{'vertical_fov':60,'near':.1,'far':100}),{'op':'entity.create','id':uid(2),'name':'Environment'},component(2,'LightingEnvironment',environment(1)),
              {'op':'asset.instantiate','id':uid(10),'name':'Analytic plane','asset':plain}])
        normal=capture('front-normal','shading_normal');depth=capture('front-depth','depth')
        probe('front_normal',normal,normal_rgb((0,0,1)));probe('front_depth',depth,(10,10,10))
        probe('normal_sky_invalid',normal,(0,0,0),(5,5));probe('depth_sky_invalid',depth,(0,0,0),(5,5))
        color=capture('front-color','color')
        if a.reference_binary:
            _,params=request('front-color-reference','color');params.pop('scene_debug_view')
            call={'jsonrpc':'2.0','id':0,'method':'world.capture','params':params}
            result=subprocess.run([str(a.reference_binary.resolve()),'world',native(world)],input=json.dumps(call)+'\n',capture_output=True,text=True,encoding='utf-8',timeout=180)
            report['reference_run']={'request':call,'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr}
            check(result.returncode==0,'Preserved reference engine failed')
            answers=[json.loads(line) for line in result.stdout.splitlines()]
            check(len(answers)==1 and answers[0].get('id')==0 and 'result' in answers[0],answers)
            reference=answers[0]['result']
            check(reference.get('capture_written') and reference.get('hardware') and reference.get('nvrhi_errors')==0 and reference.get('samples')==1,reference)
            check(reference.get('gpu')==report['gpu_name'],'Preserved reference selected another GPU')
            path=run/'front-color-reference.bmp';reference_pixels=pixels(path)
            check(len(reference_pixels)==240 and all(len(row)==320 for row in reference_pixels),'Reference extent differs')
            changed=sum(x!=y for row,other in zip(color,reference_pixels) for x,y in zip(row,other))
            maximum=max(abs(x-y) for row,other in zip(color,reference_pixels) for rgb,other_rgb in zip(row,other) for x,y in zip(rgb,other_rgb))
            report['checks']['preserved_single_sample_color']={'changed_pixels':changed,'maximum_channel_difference':maximum,'reference_image_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
            check(changed==0,'Single-sample color changed from preserved pre-MRT renderer')

        edit([component(2,'LightingEnvironment',environment(0))]);nzero=capture('zero-exposure-normal','shading_normal');dzero=capture('zero-exposure-depth','depth')
        check(nzero==normal and dzero==depth,'Zero exposure changed debug products');check(capture('zero-exposure-color','color')!=color,'Color control did not respond to exposure')
        edit([component(2,'LightingEnvironment',environment(64))]);check(capture('high-exposure-normal','shading_normal')==normal,'High exposure changed normal')
        check(capture('high-exposure-depth','depth')==depth,'High exposure changed depth')
        edit([component(1,'Camera',{'vertical_fov':60,'near':.5,'far':20})]);probe('far_normalization',capture('far20-depth','depth'),(51,51,51))
        angle=math.radians(30)/2
        edit([component(10,'Transform',transform(rotation=(0,math.sin(angle),0,math.cos(angle))))])
        probe('world_rotated_normal',capture('rotated-normal','shading_normal'),normal_rgb((.5,0,math.sqrt(.75))))
        edit([component(10,'Transform',transform()),component(mesh,'StaticMesh',{'asset':back,'primitive':0,'visible':True})])
        probe('backface_normal',capture('backface-normal','shading_normal'),normal_rgb((0,0,-1)))
        edit([component(mesh,'StaticMesh',{'asset':mapped,'primitive':0,'visible':True})])
        tangent=[204/255*2-1,-(76/255*2-1),230/255*2-1];length=math.sqrt(sum(x*x for x in tangent))
        probe('normalmap_basis',capture('mapped-normal','shading_normal'),normal_rgb([x/length for x in tangent]))
        edit([component(mesh,'StaticMesh',{'asset':plain,'primitive':0,'visible':True})])
        session=uid(900);requests=[('runtime.start',{'session_id':session,'revision':revision}),('runtime.step',{'session_id':session,'request_id':uuid.uuid4().hex,'expected_tick':0,'ticks':3}),
            request('runtime-normal','shading_normal','runtime.capture',{'session_id':session,'tick':3}),request('runtime-depth','depth','runtime.capture',{'session_id':session,'tick':3})]
        rows=batch(requests);probe('runtime_normal',image('runtime-normal',rows[2],'shading_normal'),normal_rgb((0,0,1)));probe('runtime_depth',image('runtime-depth',rows[3],'depth'),(51,51,51))
        edit([{'op':'entity.create','id':uid(20),'name':'Offscreen replay controller'},component(20,'Transform',transform((50,0,0))),
              {'op':'entity.create','id':uid(21),'name':'Controller camera'},component(21,'Transform',transform((0,1.6,0))),
              {'op':'entity.reparent','id':uid(21),'parent':uid(20),'mode':'keep_local'},component(21,'Camera',{'vertical_fov':60,'near':.1,'far':100}),
              component(20,'CharacterController',{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(21)})])
        for view in ('depth','shading_normal'):
            name='play-'+view;session=uid(910 if view=='depth' else 911)
            _,params=request(name,view,'runtime.capture',{'session_id':session})
            params.update(request_id=uuid.uuid4().hex,expected_tick=0,controller=uid(20),mode='replay',sequence=[{'ticks':3}],audio=False)
            rows=batch([('runtime.start',{'session_id':session,'revision':revision}),('runtime.play',params),
                        request(name+'-reference',view,'runtime.capture',{'session_id':session,'tick':3})])
            check(rows[1]['tick']==3 and rows[1]['frames_presented']==4,'Diagnostic player did not replay3 ticks')
            played=image(name,rows[1],view);reference=image(name+'-reference',rows[2],view)
            check(played==reference,'Continuous diagnostic player differs from isolated same-state capture')
            probe(name+'_analytic',played,(51,51,51) if view=='depth' else normal_rgb((0,0,1)))
        edit([component(1,'Transform',transform((.5,1,5)))])
        prior=None
        for time in (0,1):
            for view in ('depth','shading_normal'):
                captures=[]
                for skinning in ('gpu','cpu'):
                    name=f'skin-{time}-{view}-{skinning}'
                    captures.append(image(name,batch([request(name,view,'asset.animation.capture',{'asset':rig,'clip':0,'time':time,'loop':False,'skinning':skinning})])[0],view))
                check(captures[0]==captures[1],f'GPU/CPU skinned {view} differs at {time}')
                valid=[rgb for row in captures[0] for rgb in row if rgb!=(0,0,0)]
                check(len(valid)>500,'Skinned product has insufficient covered pixels')
                expected=(64,64,64) if view=='depth' else normal_rgb((0,0,1))
                check(all(max(abs(x-y) for x,y in zip(rgb,expected))<=2 for rgb in valid),'Skinned analytic plane product differs')
                if view=='shading_normal':
                    if prior is not None:check(captures[0]!=prior,'Skinned pose did not change coverage')
                    prior=captures[0]
        report['checks']['exposure_invariance']=True;report['checks']['skinning_analytic_and_cpu_reference']=True;report['passed']=True
    except BaseException:report['error']=traceback.format_exc()
    finally:
        unchanged=(report['binary_sha256']==hashlib.sha256(a.binary.read_bytes()).hexdigest()
            and report['test_sha256']==hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
            and all(digest==hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() for name,digest in report['fixture_sources'].items()))
        if a.reference_binary:unchanged=unchanged and report['reference_binary_sha256']==hashlib.sha256(a.reference_binary.read_bytes()).hexdigest()
        report['inputs_unchanged']=unchanged
        if not unchanged:report['passed']=False;report['input_error']='Binary or fixture source changed during qualification'
        (run/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
    check(report['passed'],'Scene products qualification failed: '+str(run/'evidence.json'))
    print(json.dumps({'passed':True,'evidence':str(run/'evidence.json')}))
if __name__=='__main__':main()
