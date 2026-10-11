#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent opaque foreground multisample coverage; external Windows Job required.

Five frozen captures: no foreground, explicit1, omitted1, explicit4, empty4.
Quarter-coverage radiance is composed BEFORE the Reinhard/sRGB display transform.
RGB error2, partial population100 and all three partial classes are fixed here;
no sample-position assumption, execution-time tuning or performance claim.
"""
import argparse
import copy
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
sys.path.insert(0,str(ROOT/'tests'))
from live_material_capture import Owner,need,sha,encoded,same,compare
from poima_client.errors import RpcError
from shadow_controls_capture import component,transform
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels

W,H,CAP=320,240,64*1048576
CAM,WALL,FG,ENV=(f'{n:032x}' for n in (1,20,21,30))
RGB_ERROR=2
BACKGROUND=(.03125,.03125,.03125)
FOREGROUND=(.5,0.,0.)
# Exact float32 dyadics at z=-1, FOV90: three nonsymmetric projected slopes.
VERTICES=((-15/16,3/4,0),(7/8,3/8,0),(-1/2,-3/4,0))
SCREEN=tuple((W/2+x*H/2,H/2-y*H/2) for x,y,_ in VERTICES)
GLTF_MATERIAL=dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0,roughnessFactor=1),doubleSided=True)
PROBE_FIELDS=('x','y','depth','raw_hdr','resolved_hdr','shading_normal','surface_valid','motion','motion_valid')


def pbr(color):
    return dict(base_color=[0,0,0],emissive=list(color),metallic=0,roughness=1,double_sided=True)


def triangle():
    doc,_=quad([],GLTF_MATERIAL)
    data=[]
    for p,uv in zip(VERTICES,((0,1),(1,1),(0,0))):data.extend((*p,0,0,1,*uv))
    raw=struct.pack('<24f3I',*data,0,1,2)
    doc['buffers'][0]['byteLength']=len(raw)
    doc['bufferViews'][0]['byteLength']=96
    doc['bufferViews'][1].update(byteOffset=96,byteLength=12)
    for accessor in doc['accessors']:accessor['count']=3
    doc['accessors'][0].update(min=[min(p[i] for p in VERTICES) for i in range(3)],max=[max(p[i] for p in VERTICES) for i in range(3)])
    return doc,raw


class BoundedOwner(Owner):
    def raw(self,row):
        need(len(encoded(row))<=1048576,'Individual RPC row exceeds1MiB')
        super().raw(row)


def raw_joins(record):
    requests={};responses={}
    for row in record['transport']:
        message=row['message'];need(message['jsonrpc']=='2.0' and type(message['id']) is int,'Malformed raw envelope')
        need(row['direction'] in ('request','response'),'Unknown raw direction')
        target=requests if row['direction']=='request' else responses
        need(message['id'] not in target,'Duplicate raw envelope');target[message['id']]=message
    need(set(requests)==set(responses) and len(requests)==len(record['calls']),'Unsettled/lost RPC evidence')
    for identifier,call in zip(requests,record['calls']):
        request,response=requests[identifier],responses[identifier]
        need(request['method']==call['method'] and same(request['params'],call['params']),'Native request differs from interpretation')
        if 'native_error' in call:
            need(same(response.get('error'),call['native_error']) and 'result' not in call,'Native error differs from interpretation')
        else:need('result' in response and same(response['result'],call['result']),'Native result differs from interpretation')
    record['raw_joined_calls']=len(requests)


def reject(owner,params):
    try:owner.rpc('runtime.capture',params)
    except RpcError as error:
        call=owner.record['calls'][-1]
        requests=[row['message'] for row in owner.record['transport'] if row['direction']=='request']
        identifier=requests[-1]['id']
        errors=[row['message']['error'] for row in owner.record['transport'] if row['direction']=='response' and row['message']['id']==identifier]
        need(len(errors)==1 and error.code==-32602 and errors[0]['code']==error.code and errors[0]['message']==error.message,'Malformed sample count did not produce actual invalid-params error')
        call['native_error']=copy.deepcopy(errors[0])
        need(not Path(params['path']).exists(),'Rejected capture wrote output')
        return copy.deepcopy(errors[0])
    raise AssertionError('Malformed sample count was accepted')


def case(args,label,requested,subject,lens,deadline):
    output=args.output/label;output.mkdir();(output/'captures').mkdir()
    local=argparse.Namespace(**vars(args));local.output=output
    record=dict(passed=False,label=label,calls=[],transport=[],captures=[],native_owner=None,recipe=dict(viewmodel_samples=requested,subject=subject,lens=lens))
    owner=None;failure=None
    try:
        wall=output/'wall.glb';wall.write_bytes(glb(*quad([],GLTF_MATERIAL)))
        source=output/'diagonal.glb';source.write_bytes(glb(*triangle()))
        record['input_images']={p.name:dict(bytes=p.stat().st_size,sha256=sha(p)) for p in (wall,source)}
        owner=BoundedOwner(local,record,deadline)
        description=owner.rpc('world.describe',dict(view='method',name='runtime.capture'))
        need(description['schema_revision']==90,'Current capture discovery missing')
        spec=description['methods']['runtime.capture']['properties']['viewmodel_samples']
        need(spec['type']=='integer' and spec['enum']==[1,2,4,8] and spec['default']==1,'Independent sample-count schema differs')
        wall_asset=owner.rpc('asset.import',dict(source=str(wall)))['asset']
        triangle_asset=owner.rpc('asset.import',dict(source=str(source)))['asset']
        identities=[(CAM,None),(WALL,None),(ENV,None)]+([(FG,CAM)] if subject else [])
        ops=[]
        for identity,parent in identities:
            ops.extend([dict(op='entity.create',id=identity,name='Coverage '+identity,parent=parent),transform(identity)])
        camera=dict(vertical_fov=90,near=.01,far=20)
        if lens:camera['viewmodel']=dict(vertical_fov=90,near=.01,far=10)
        ops.extend([component(CAM,'Camera',camera),component(ENV,'LightingEnvironment',dict(ambient=[0,0,0],exposure=1)),
            transform(WALL,(0,0,-.2),(20,20,1)),component(WALL,'StaticMesh',dict(asset=wall_asset,primitive=0,visible=True,cast_shadows=False)),component(WALL,'PbrMaterial',pbr(BACKGROUND))])
        if subject:ops.extend([transform(FG,(0,0,-1)),component(FG,'StaticMesh',dict(asset=triangle_asset,primitive=0,visible=True,cast_shadows=False)),component(FG,'PbrMaterial',pbr(FOREGROUND)),component(FG,'CameraDraw',dict(camera=CAM,mode='viewmodel'))])
        owner.edit(ops);record['world_sha256']=sha(output/'world.json');owner.session=uuid.uuid4().hex
        start=owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision));need(start['started'] is True and start['tick']==0,'Frozen runtime did not start')
        factor=1.5 if args.reconstruction=='fsr3_quality' else 1
        rw,rh=int(W/factor),int(H/factor)
        path=output/'captures/c.bmp'
        params=dict(session_id=owner.session,tick=0,material_revision=0,camera=CAM,path=str(path),gpu=args.gpu,width=W,height=H,samples=1,
            lighting_path=args.lighting_path,reconstruction=args.reconstruction,frames_in_flight=2,capture_frames=2,
            ambient_occlusion=dict(mode='none'),scene_product_probes=[dict(x=rw//2+dx,y=rh//2+dy) for dy in (-12,0,12) for dx in (-12,0,12)])
        if requested is not None:params['viewmodel_samples']=requested
        if label=='explicit-1':
            before=owner.rpc('runtime.inspect',dict(session_id=owner.session));record['invalid_options']=[]
            for index,value in enumerate((True,0,3,16)):
                invalid=copy.deepcopy(params);invalid.update(viewmodel_samples=value,path=str(output/'captures'/f'rejected-{index}.bmp'))
                record['invalid_options'].append(dict(value=value,native_error=reject(owner,invalid)))
            need(same(before,owner.rpc('runtime.inspect',dict(session_id=owner.session))),'Rejected options changed runtime')
        result=owner.rpc('runtime.capture',params)
        need(result['build_version']=='0.0.121' and result['hardware'] is True and result['capture_written'] is True and result['nvrhi_errors']==0,'Actual hardware capture absent')
        need(result['width']==W and result['height']==H and result['samples']==1 and result['session_id']==owner.session and result['tick']==0 and result['material_revision']==0,'Captured owner/extent differs')
        diagnostics=result['render_diagnostics'];need(diagnostics['lighting_path']==args.lighting_path,'World path differs')
        receipts=diagnostics['scene_receipts'];need(same(receipts['submitted'],receipts['presented']),'Frame not actually presented')
        observed=copy.deepcopy(receipts['observed']);presented=copy.deepcopy(receipts['presented'])
        for receipt in (observed,presented):receipt.pop('frame_sequence');receipt.pop('submission_id')
        need(same(observed,presented),'Observed/presented immutable snapshot differs')
        shown=receipts['presented'];need(shown['camera_id']==CAM and shown['authored_revision']==owner.revision and shown['material_revision']==0 and shown['frame_sequence']>0 and shown['submission_id']>0,'Capture identity missing')
        if lens:
            draw=shown['camera_draw'];need(draw['hidden']==[] and draw['viewmodels']==([dict(entity_id=FG,incarnation=1)] if subject else []) and draw['viewmodel_lens']==dict(vertical_fov=90.0,near=.01,far=10.0),'Foreground receipt differs')
            vm=diagnostics['viewmodel'];need(vm['rendered'] is True and vm['samples']==(requested or 1) and vm['output_width']==W and vm['output_height']==H,'Actual independent samples/extent differ')
            need(vm['draws']['camera_draws']==(1 if subject else 0),'Actual foreground draw count differs')
            record['viewmodel']=copy.deepcopy(vm)
        else:need('viewmodel' not in diagnostics and 'camera_draw' not in shown,'Unconfigured foreground appeared')
        record['probes']=copy.deepcopy(diagnostics['scene_products']['probes'])
        need(len(record['probes'])==9 and all(probe['surface_valid'] for probe in record['probes']),'World wall products absent')
        raw_error=max(abs(value-reference) for probe in record['probes'] for value,reference in zip(probe['raw_hdr'],BACKGROUND))
        resolved_deviation=max(abs(value-reference) for probe in record['probes'] for value,reference in zip(probe['resolved_hdr'],BACKGROUND))
        need(raw_error<=1e-6,'Independent raw pre-tone world-background radiance differs')
        # Reconstruction output is observed, not asserted to be the authored constant.
        # The complete raw/resolved products must still match across all five cases.
        record['background_hdr']=dict(expected_rgb=list(BACKGROUND),maximum_raw_error=raw_error,raw_error_limit=1e-6,
            maximum_resolved_deviation=resolved_deviation,resolved_deviation_is_observational=True,probes=9)
        record['captures'].append(dict(image='captures/c.bmp',bytes=path.stat().st_size,sha256=sha(path),result=result))
        need(sha(output/'world.json')==record['world_sha256'],'Capture changed authored source')
        need(all(p.stat().st_size==record['input_images'][p.name]['bytes'] and sha(p)==record['input_images'][p.name]['sha256'] for p in (wall,source)),'Original geometric inputs changed');record['passed']=True
    except BaseException as error:failure=repr(error);record['error']=failure;record['traceback']=traceback.format_exc()
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException as error:failure=failure or repr(error);record['cleanup_error']=repr(error)
        record['passed']=bool(record['passed'] and failure is None and record['native_owner'] is not None)
        if record['passed']:
            try:raw_joins(record)
            except BaseException:
                record['passed']=False;record['raw_join_error']=traceback.format_exc()
    return record


def display(radiance):
    exposed=radiance/(1+radiance)
    srgb=12.92*exposed if exposed<=.0031308 else 1.055*exposed**(1/2.4)-.055
    return round(255*srgb)


def palette():
    return [tuple(display((1-k/4)*b+(k/4)*f) for b,f in zip(BACKGROUND,FOREGROUND)) for k in range(5)]


def edge_distance(x,y):
    distances=[]
    for (ax,ay),(bx,by) in zip(SCREEN,SCREEN[1:]+SCREEN[:1]):
        dx,dy=bx-ax,by-ay
        t=max(0.,min(1.,((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy)))
        distances.append(math.hypot(x-(ax+t*dx),y-(ay+t*dy)))
    return min(distances)


def inside(x,y):
    signs=[(bx-ax)*(y-ay)-(by-ay)*(x-ax) for (ax,ay),(bx,by) in zip(SCREEN,SCREEN[1:]+SCREEN[:1])]
    return all(value>=0 for value in signs) or all(value<=0 for value in signs)


def coverage(single,multi,background):
    expected=palette();counts=[0]*5;single_counts=[0]*2;interior=exterior=partial=0;max_error=0
    for y in range(H):
        for x in range(W):
            b=background[y][x];need(max(abs(a-c) for a,c in zip(b,expected[0]))<=RGB_ERROR,'Independent uniform background differs from shader display equation')
            a=single[y][x];errors=[max(abs(p-q) for p,q in zip(a,color)) for color in (expected[0],expected[4])]
            selected=min(range(2),key=errors.__getitem__);need(errors[selected]<=RGB_ERROR,'Single sample produced fractional opaque coverage');single_counts[selected]+=1
            value=multi[y][x];errors=[max(abs(p-q) for p,q in zip(value,color)) for color in expected]
            k=min(range(5),key=errors.__getitem__);need(errors[k]<=RGB_ERROR,'Multisample edge is not a pre-tone quarter-radiance mixture');max_error=max(max_error,errors[k]);counts[k]+=1
            distance=edge_distance(x+.5,y+.5)
            if 0<k<4:
                need(distance<=1.5,'Fractional coverage appeared away from the independent analytic triangle edge');partial+=1
            if distance>1.5:
                expected_k=4 if inside(x+.5,y+.5) else 0
                need(k==expected_k and selected==(1 if expected_k else 0),'Interior/exterior geometry differs from independent projection');interior+=bool(expected_k);exterior+=not bool(expected_k)
    need(partial>=100 and all(counts[k]>=10 for k in (1,2,3)),'Insufficient three-class actual multisample edge coverage')
    need(min(single_counts)>=1000 and interior>=1000 and exterior>=1000,'Coverage image is empty or truncated')
    return dict(rgb_error_limit=RGB_ERROR,maximum_rgb_error=max_error,expected_display_rgb=[list(color) for color in expected],
        quarter_coverage_pixels=counts,partial_pixels=partial,minimum_partial_pixels=100,minimum_each_partial_class=10,
        analytic_interior_pixels=interior,analytic_exterior_pixels=exterior,maximum_edge_distance_pixels=1.5,
        single_sample_binary_pixels=single_counts,screen_vertices=[list(p) for p in SCREEN],tone_transform='Reinhard then sRGB; coverage composed in scene-linear HDR')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--gpu',required=True,type=int,choices=(0,1));parser.add_argument('--lighting-path',required=True,choices=('forward','deferred'))
    parser.add_argument('--reconstruction',default='none',choices=('none','fsr3_quality'));args=parser.parse_args()
    args.binary=args.binary.resolve();args.output=args.output.resolve()
    need(os.name=='nt' and not sys.flags.optimize and not args.output.exists(),'Fresh root-owned Windows fixture required');args.output.mkdir(parents=True)
    sources=[Path(__file__),args.binary,args.binary.parent/'phonon.dll',*[ROOT/'tests'/name for name in ('live_material_capture.py','shadow_controls_capture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')]]
    sources.extend(p for p in (ROOT/'tools/python/poima_client').rglob('*') if p.is_file() and p.suffix in ('.py','.json'))
    pins={str(p):sha(p) for p in sources};started=time.monotonic();deadline=started+300
    record=dict(passed=False,source_pins=pins,recipe=dict(gpu=args.gpu,lighting_path=args.lighting_path,reconstruction=args.reconstruction,width=W,height=H,world_samples=1,viewmodel_samples=4,rgb_error_limit=RGB_ERROR),cases=[],
        scope='Original opaque diagonal analytic triangle: independent projected edge-band and pre-tone quarter-radiance mixtures, omitted versus explicit1, transparent empty4 and exact world products. No assumed sample positions, player continuity, visual-art quality or performance claim.')
    try:
        recipes=[('no-viewmodel',None,False,False),('explicit-1',1,True,True),('default-1',None,True,True),('explicit-4',4,True,True),('configured-empty-4',4,False,True)]
        for label,requested,subject,lens in recipes:
            record['cases'].append(case(args,label,requested,subject,lens,deadline));need(record['cases'][-1]['passed'],'Actual case failed: '+label)
        images={c['label']:pixels(args.output/c['label']/'captures/c.bmp') for c in record['cases']}
        record['coverage']=coverage(images['explicit-1'],images['explicit-4'],images['no-viewmodel'])
        compatible=compare(images['explicit-1'],images['default-1']);need(compatible['max_rgb_delta']==0,'Omitted/explicit1 differs')
        empty=compare(images['no-viewmodel'],images['configured-empty-4']);need(empty['max_rgb_delta']==0,'Transparent empty4 changed world output')
        record['default_compatibility']=dict(explicit_omitted=compatible,empty_foreground=empty)
        reference=record['cases'][0]['probes']
        for actual in record['cases'][1:]:need(all(same({k:a[k] for k in PROBE_FIELDS},{k:b[k] for k in PROBE_FIELDS}) for a,b in zip(reference,actual['probes'])),'Foreground MSAA contaminated world products')
        record['world_product_isolation']=dict(exact_probes_per_case=9,compared_cases=4,exact_probe_pairs=36,fields=list(PROBE_FIELDS))
        need(sum(len(c['transport']) for c in record['cases'])<=1000,'Aggregate raw row cap exceeded')
        record['passed']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        record['seconds']=time.monotonic()-started
        record['changed_sources']=[name for name,digest in pins.items() if not Path(name).is_file() or sha(name)!=digest]
        record['passed']=bool(record['passed'] and not record['changed_sources'] and record['seconds']<=300)
        data=encoded(record)+b'\n';need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data)<=CAP,'Aggregate output/evidence cap exceeded')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic()>deadline:
            record['passed']=False;record['publication_error']='Whole300s exceeded during publication.';(args.output/'evidence.json').write_bytes(encoded(record)+b'\n')
    print(encoded({k:record.get(k) for k in ('passed','seconds','error')}).decode())
    return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
