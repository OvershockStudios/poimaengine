#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paired live texture adoption on a fixed imported UV quad.

Requires native Windows and an external owned Job. Each slot changes texture
inputs while its scalar factors stay fixed. Authored/live RGB and actual HDR
probes are compared independently in forward or deferred rendering. This does
not qualify persistent player resource reuse, GPU completion, or latency.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import uuid
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from live_material_capture import (Owner, need, sha, encoded, same, inventory,
                                   compare, checkpoint, WIDTH, HEIGHT, CAP, CAMERA)
from texture_fixture import png, quad
from gltf_fixture import glb
from shadow_controls_capture import primitive, component, transform, skinned_quad
from scene_capture import pixels
SUBJECT_ROOT=f'{200:032x}'
SUBJECT=primitive(SUBJECT_ROOT)
LIGHT=f'{2:032x}'
ENV=f'{3:032x}'
SLOTS=('base_color','metallic_roughness','emissive','occlusion','normal')
PBR=dict(base_color=[.5,.5,.5],emissive=[0.,0.,0.],metallic=.75,roughness=.125,double_sided=True)


def capture(owner, label, live, epoch, source=None):
    path=owner.args.output/'captures'/(label+'.bmp')
    params=dict(camera=CAMERA,path=str(path),gpu=owner.args.gpu,width=WIDTH,height=HEIGHT,
        samples=1,lighting_path=owner.args.lighting_path,
        scene_product_probes=[dict(x=x,y=y) for y in (HEIGHT//2-20,HEIGHT//2,HEIGHT//2+20) for x in (WIDTH//2-20,WIDTH//2,WIDTH//2+20)],
        frames_in_flight=2,capture_frames=2,ambient_occlusion={'mode':'none'},reconstruction='none',shadow_filter='pcf3_point')
    if live:params.update(session_id=owner.session,tick=0,material_revision=epoch)
    else:params['revision']=owner.revision
    world_hash=sha(owner.args.output/'world.json')
    result=owner.rpc('runtime.capture' if live else 'world.capture',params)
    need(result['capture_written'] is True and result['hardware'] is True and result['nvrhi_errors']==0 and
         result['width']==WIDTH and result['height']==HEIGHT and result['material_revision']==epoch and
         result['object_count']==1 and result['build_version']=='0.0.114', 'Hardware capture identity/shape failed.')
    need(sha(owner.args.output/'world.json')==world_hash, 'Capture mutated authored bytes.')
    diagnostics=result['render_diagnostics'];receipts=diagnostics['scene_receipts']
    submitted,presented=receipts['submitted'],receipts['presented']
    need(isinstance(submitted,dict) and same(submitted,presented), 'Submitted/presented snapshot receipt mismatch.')
    need(presented['material_revision']==epoch and presented['authored_revision']==result['revision'] and
         presented['camera_id']==CAMERA and presented['world_id']==result['world_id'] and
         type(presented['frame_sequence']) is int and presented['frame_sequence']>0 and
         type(presented['submission_id']) is int and presented['submission_id']>0 and
         bool(presented['presentation_source_id']), 'Capture lacks exact frame/material receipt.')
    observed=copy.deepcopy(receipts['observed']);identity=copy.deepcopy(presented)
    for row in (observed,identity):row.pop('frame_sequence');row.pop('submission_id')
    need(same(observed,identity), 'Observed snapshot changed before presentation.')
    if live:
        need(result['session_id']==owner.session and result['tick']==0 and presented['presentation_generation']==epoch,
             'Live capture changed runtime identity or material reset epoch.')
        if source is not None:need(presented['presentation_source_id']==source, 'Live capture replaced its runtime source.')
    need(diagnostics['lighting_path']==owner.args.lighting_path and diagnostics['ambient_occlusion']['mode']=='none' and
         diagnostics['reconstruction']['mode']=='none', 'Rendering recipe substitution.')
    counts=diagnostics['last_draws']
    need(counts['objects']==1 and counts['camera_draws']==1 and counts['camera_triangles']>=2, 'Subject was not actually drawn.')
    weighted=label.startswith('weighted-')
    need(counts['skinned_instances']==(1 if weighted else 0) and (not weighted or counts['skinned_vertices']>=4),
         'Weighted subject did not execute the real skinned draw path.')
    products=diagnostics['scene_products']
    need(products['available'] is True and len(products['probes'])==9, 'Actual HDR probes unavailable.')
    for probe in products['probes']:
        need(all(type(x) in (int,float) and math.isfinite(x) for x in probe['raw_hdr']), 'Nonfinite actual HDR probe.')
    need(products['probes'][4]['surface_valid'] is True, 'Central highlight probe missed the subject.')
    image=pixels(path)
    owner.record['captures'].append(dict(label=label,image=str(path.relative_to(owner.args.output)),sha256=sha(path),
        bytes=path.stat().st_size,result=result))
    return image,presented['presentation_source_id'],copy.deepcopy(products['probes'])

def image(owner,name,color,space,width=1):
    path=owner.args.output/(name+'.png')
    path.write_bytes(png(width,1,color))
    result=owner.rpc('asset.image.import',dict(source=str(path),color_space=space))
    need(result['format']=='poima.image.v1' and result['color_space']==space,'Wrong imported texture color space.')
    owner.record['image_inputs'].append(dict(name=name,sha256=sha(path),bytes=path.stat().st_size,result=result))
    return result['asset']


def linear_channel(byte):
    value=byte/255.
    return value/12.92 if value<=.04045 else ((value+.055)/1.055)**2.4


def channel_reference(case,stage,probes):
    # These controls have no direct radiance. Their independently computed
    # constant-channel HDR targets qualify R-only AO, sRGB emission and actual
    # sampler behavior, rather than a change in scalar factors or exposure.
    expected=None
    if case=='emissive':expected=[.0025+.5*linear_channel(x) for x in (48,200,24)]
    elif case=='occlusion':expected=[.125*(24/255. if stage=='map' else 1.)]*3
    elif case=='sampler':expected={'repeat':[1.,0.,0.],'clamp':[0.,1.,0.],'linear':[.5,.5,0.]}[stage]
    elif case=='inherit':expected=[1.,1.,1.] if stage=='null' else [linear_channel(x) for x in (200,48,32)]
    if expected is not None:
        error=max(abs(actual-target) for actual,target in zip(probes[4]['raw_hdr'],expected))
        need(error<=.002,'Independent texture channel HDR reference failed.')
        return dict(kind='constant_channel_hdr',expected=expected,maximum_error=error,tolerance=.002)
    if case=='normal':
        normal=probes[4]['shading_normal']
        if stage=='map':need(abs(normal[1])>=.98 and abs(normal[2])<=.05,'Normal map did not tilt actual shading normal.')
        else:need(abs(normal[2])>=.99 and abs(normal[0])<=.01 and abs(normal[1])<=.01,'Zero normal scale did not restore geometric normal.')
        return dict(kind='shading_normal',normal=normal,stage=stage)
    return None


def inspect(owner,epoch):
    row=owner.rpc('runtime.texture.inspect',dict(session_id=owner.session,tick=0,material_revision=epoch,ids=[SUBJECT]))
    need(row['material_revision']==epoch and row['tick']==0 and len(row['textures'])==1 and row['textures'][0]['id']==SUBJECT,
         'Texture inspection identity mismatch.')
    maps=row['textures'][0]['effective']['maps']
    need(set(maps)==set(SLOTS),'Texture inspection lost a slot.')
    for slot,value in maps.items():
        if value is not None:
            need(value['color_space']==('srgb' if slot in ('base_color','emissive') else 'linear') and
                 value['width']>0 and value['height']>0 and value['mip_levels']>0 and len(value['image_sha256'])==64,
                 'Effective texture metadata is invalid.')
    return row


def main():
    global SUBJECT,SUBJECT_ROOT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),required=True)
    parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args();args.binary=args.binary.resolve();args.output=args.output.resolve()
    need(os.name=='nt' and not sys.flags.optimize,'Native Windows Python beneath an external owned Job is required.')
    need(not args.output.exists(),'Output must be new.')
    args.output.mkdir(parents=True);(args.output/'captures').mkdir();(args.output/'saves').mkdir()
    started=time.monotonic();deadline=started+300
    sources=[Path(__file__),ROOT/'tests/live_material_capture.py',ROOT/'tests/shadow_controls_capture.py',
             ROOT/'tests/texture_fixture.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py']
    sources+=sorted((ROOT/'tools/python/poima_client').glob('*.py'))+sorted((ROOT/'tools/python/poima_client').glob('*.json'))
    pins={str(p):sha(p) for p in sources}
    record=dict(passed=False,scope='Standalone authored/live texture parity; no player continuity, GPU completion, performance or Alpha claim.',
        recipe=dict(width=WIDTH,height=HEIGHT,lighting_path=args.lighting_path,gpu=args.gpu,samples=1,frames_in_flight=2,
            maximum_rgb_error=1,maximum_hdr_error=.002,minimum_strong_changed_pixels=100,minimum_max_delta=5,
            ambient_occlusion='none',reconstruction='none',runtime_tick=0,scalar_changes_during_adoption=False),
        source_pins=pins,binary_sha256=sha(args.binary),binary_dependencies={'phonon.dll':sha(args.binary.parent/'phonon.dll')},
        calls=[],transport=[],captures=[],comparisons=[],checkpoints=[],cases=[],image_inputs=[],native_owner=None)
    owner=None;failure=None
    try:
        owner=Owner(args,record,deadline)
        record['discovery']=owner.rpc('world.describe',dict(view='method',name='runtime.texture.apply'))
        owner.edit([dict(op='entity.create',id=CAMERA,name='Camera'),transform(CAMERA,(0,0,6)),
            component(CAMERA,'Camera',dict(vertical_fov=40,near=.1,far=100)),
            dict(op='entity.create',id=LIGHT,name='Light'),transform(LIGHT,(0,1.5,3)),
            dict(op='entity.create',id=ENV,name='Ambient'),transform(ENV)])
        owner.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(args.output/'saves')))
        assets={}
        colors={'base_color':[32,190,64,255],'metallic_roughness':[240,224,32,255],
                'emissive':[48,200,24,255],'occlusion':[24,240,240,255],'normal':[128,250,128,255]}
        for slot in SLOTS:assets[slot]=image(owner,slot,colors[slot],'srgb' if slot in ('base_color','emissive') else 'linear')
        assets['sampler']=image(owner,'sampler',[255,0,0,255,0,255,0,255],'srgb',2)
        cases=[]
        for slot in SLOTS:
            factors=copy.deepcopy(PBR);ambient=.005;intensity=12*math.pi
            if slot in ('emissive','occlusion'):
                factors.update(metallic=0.,roughness=.75)
                if slot=='emissive':factors['emissive']=[.5,.5,.5];ambient=.005;intensity=0.
                else:ambient=.25;intensity=0.
            initial=dict(asset=assets[slot],min_filter=9728,mag_filter=9728)
            first={slot:initial};stages=[('map',first,True)]
            if slot=='normal':stages.append(('strength-zero',dict(first,normal_scale=0.),True))
            if slot=='occlusion':stages.append(('strength-zero',dict(first,occlusion_strength=0.),True))
            cases.append((slot,factors,ambient,intensity,None,None,stages))
        emission=copy.deepcopy(PBR);emission.update(base_color=[0.,0.,0.],emissive=[1.,1.,1.],metallic=0.,roughness=.75)
        nearest=dict(asset=assets['sampler'],wrap_s=10497,wrap_t=10497,min_filter=9728,mag_filter=9728)
        cases.append(('sampler',emission,0.,0.,[(1.,.5)]*4,None,[
            ('repeat',dict(emissive=nearest),True),
            ('clamp',dict(emissive=dict(nearest,wrap_s=33071)),True),
            ('linear',dict(emissive=dict(nearest,min_filter=9729,mag_filter=9729)),True)]))
        inherited=png(1,1,[200,48,32,255])
        cases.append(('inherit',emission,0.,0.,None,inherited,[
            ('null',dict(emissive=None),True),('empty',{},True),('remove',None,False)]))
        cases.append(('weighted-base-color',copy.deepcopy(PBR),.005,12*math.pi,None,None,[
            ('map',dict(base_color=dict(asset=assets['base_color'],min_filter=9728,mag_filter=9728)),True)]))
        last_subject=None
        for number,(name,factors,ambient,intensity,uvs,builtin,stages) in enumerate(cases):
            owner.stop();SUBJECT_ROOT=f'{200+number:032x}';SUBJECT=primitive(SUBJECT_ROOT)
            mat=dict(pbrMetallicRoughness=dict(baseColorFactor=factors['base_color']+[1.],metallicFactor=factors['metallic'],roughnessFactor=factors['roughness']),
                     emissiveFactor=factors['emissive'],doubleSided=True)
            if builtin is not None:mat['emissiveTexture']=dict(index=0)
            weighted=name.startswith('weighted-')
            if weighted:
                doc,raw=skinned_quad();doc['materials']=[mat]
            else:doc,raw=quad([] if builtin is None else [builtin],mat,uvs=uvs)
            path=args.output/(name+'.glb');path.write_bytes(glb(doc,raw))
            model=owner.rpc('asset.import',dict(source=str(path)))['asset']
            mesh_kind='SkinnedMesh' if weighted else 'StaticMesh'
            mesh_value=dict(asset=model,primitive=0,visible=True,cast_shadows=False)
            if weighted:mesh_value.update(rig=SUBJECT_ROOT,node=0)
            ops=[dict(op='asset.instantiate',id=SUBJECT_ROOT,asset=model,name='Texture '+name),
                component(SUBJECT,mesh_kind,mesh_value),
                component(ENV,'LightingEnvironment',dict(ambient=[ambient]*3,exposure=1)),
                component(LIGHT,'Light',dict(kind='point',color=[1,1,1],intensity=intensity,enabled=True,
                    shadow=dict(enabled=False,distance=20,bias=.0001,normal_bias=.005)))]
            if last_subject is not None:
                hidden=copy.deepcopy(last_mesh_value);hidden['visible']=False
                ops.append(component(last_subject,last_mesh_kind,hidden))
            # inherited case deliberately has no authored PbrMaterial; the imported
            # model's immutable PBR and maps survive component insertion/removal.
            if builtin is None:ops.append(component(SUBJECT,'PbrMaterial',factors))
            else:ops.append(dict(op='component.remove',id=SUBJECT,type='PbrMaterial'))
            owner.edit(ops);last_subject,last_mesh_kind,last_mesh_value=SUBJECT,mesh_kind,copy.deepcopy(mesh_value)
            owner.session=uuid.uuid4().hex
            owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision))
            baseline_info=owner.rpc('runtime.inspect',dict(session_id=owner.session));original_info=baseline_info
            baseline,source,old_probes=capture(owner,name+'-baseline',True,0)
            original,old_dir,old_inventory=checkpoint(owner,name+'-original',0)
            frozen=inspect(owner,0)
            need(frozen['textures'][0]['authored'] is None,'Baseline unexpectedly has authored texture overrides.')
            for epoch,(stage,value,visible_effect) in enumerate(stages,1):
                op=component(SUBJECT,'PbrTextures',value) if value is not None else dict(op='component.remove',id=SUBJECT,type='PbrTextures')
                owner.edit([op]);need(inspect(owner,epoch-1)['textures'][0]['authored']==frozen['textures'][0]['authored'],
                                      'Authored edit silently changed live texture intent.')
                authored,_,authored_probes=capture(owner,name+'-'+stage+'-authored',False,0)
                params=dict(session_id=owner.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_structure_revision=0,
                    expected_material_revision=epoch-1,revision=owner.revision,expected_authored_revision=original_info['authored_revision'],
                    expected_content_sha256=original_info['content_sha256'],ids=[SUBJECT])
                accepted=owner.rpc('runtime.texture.apply',params)
                need(accepted['material_revision']==epoch and accepted['tick']==0 and accepted['replayed'] is False,'Texture adoption identity changed.')
                retry=owner.rpc('runtime.texture.apply',params);expected=copy.deepcopy(accepted);expected['replayed']=True
                need(same(retry,expected),'Texture retry changed its settled receipt.')
                live,_,live_probes=capture(owner,name+'-'+stage+'-live',True,epoch,source)
                parity=compare(authored,live);effect=compare(baseline,live)
                need(parity['max_rgb_delta']<=1,'Live image differs from independent authored texture target.')
                hdr=max(abs(a-b) for p,q in zip(authored_probes,live_probes) for a,b in zip(p['raw_hdr'],q['raw_hdr']))
                need(hdr<=.002,'Authored/live HDR texture mismatch.')
                if visible_effect:need(effect['strong_changed_pixels']>=100 and effect['max_rgb_delta']>=5,'Texture-only stage lacks meaningful channel effect.')
                else:need(effect['max_rgb_delta']==0,'Removing empty texture intent changed inherited image.')
                frozen=inspect(owner,epoch)
                need(same(frozen['textures'][0]['authored'],value) and frozen['source_stale'] is False,'Resolved texture intent/baseline mismatch.')
                if stage=='null':need(frozen['textures'][0]['effective']['maps']['emissive'] is None,'Explicit null did not clear inherited map.')
                if name=='sampler':
                    mapping=frozen['textures'][0]['effective']['maps']['emissive']
                    need(mapping['wrap_s']==value['emissive']['wrap_s'] and mapping['min_filter']==value['emissive']['min_filter'] and
                         mapping['mag_filter']==value['emissive']['mag_filter'],'Effective sampler differs from requested sampler.')
                if stage=='strength-zero':need(frozen['textures'][0]['effective']['normal_scale' if name=='normal' else 'occlusion_strength']==0.,'Strength was not adopted.')
                info=owner.rpc('runtime.inspect',dict(session_id=owner.session))
                a,b=copy.deepcopy(baseline_info),copy.deepcopy(info)
                for row in (a,b):
                    for key in ('authored_revision','current_authored_revision','source_stale','material_revision','content_sha256'):row.pop(key)
                need(same(a,b),'Texture adoption changed another runtime domain.')
                saved,_,_=checkpoint(owner,name+'-'+stage,epoch)
                a,b=copy.deepcopy(original['snapshot']['payload']),copy.deepcopy(saved['snapshot']['payload'])
                for row in (a,b):row.pop('authored_revision');row.pop('content_sha256')
                need(same(a,b),'Texture adoption changed complete saved runtime state.')
                comps=saved['document']['entities'][SUBJECT]['components']
                need(('PbrTextures' in comps)==(value is not None) and (value is None or same(comps['PbrTextures'],value)),
                     'Checkpoint did not retain exact texture intent.')
                need(inventory(old_dir)==old_inventory,'Old durable baseline changed.')
                reference=channel_reference(name,stage,live_probes)
                record['comparisons'].append(dict(case=name,stage=stage,parity=parity,effect=effect,max_raw_hdr_parity_error=hdr,
                    expected_visible_effect=visible_effect,channel_reference=reference))
                original_info=info;baseline=live;old_probes=live_probes
            record['cases'].append(dict(case=name,applications=len(stages),tick=0,imported_pbr_without_explicit_component=builtin is not None,
                scalar_factors_unchanged=True,weighted_draw=weighted,full_saved_state_preserved=True,original_slot_immutable=True))
        need(len(record['captures'])==36 and len(record['comparisons'])==14,'Incomplete texture capture matrix.')
    except BaseException as error:
        failure=repr(error);record['error']=failure;record['traceback']=traceback.format_exc()
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException as error:record['cleanup_error']=repr(error);failure=failure or repr(error)
        record['seconds']=time.monotonic()-started
        try:
            need(record['seconds']<=300,'Whole fixture deadline exceeded.')
            need(all(sha(Path(p))==h for p,h in pins.items()) and sha(args.binary)==record['binary_sha256'] and
                 sha(args.binary.parent/'phonon.dll')==record['binary_dependencies']['phonon.dll'],'Fixture/native inputs changed.')
            record['passed']=failure is None and record['native_owner'] is not None
            data=encoded(record)+b'\n';need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data)<=CAP,'Final aggregate cap exceeded.')
            with (args.output/'evidence.pending.json').open('xb') as stream:stream.write(data)
            need(time.monotonic()<=deadline,'Evidence write exceeded deadline.')
            (args.output/'evidence.pending.json').rename(args.output/'evidence.json')
            need(time.monotonic()<=deadline,'Evidence publication exceeded deadline.')
        except BaseException as error:
            record['passed']=False;failure=failure or repr(error)
            with (args.output/'failure.json').open('xb') as stream:
                stream.write(encoded(dict(passed=False,error=failure,publication_error=repr(error),seconds=time.monotonic()-started))+b'\n')
            if (args.output/'evidence.json').exists():(args.output/'evidence.json').rename(args.output/'late-evidence.json')
    print(json.dumps(dict(passed=record['passed'],captures=len(record['captures']),error=failure)))
    return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
