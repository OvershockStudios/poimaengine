#!/usr/bin/env python3
"""Explicit Vulkan observation of baked recipes; not a frame-performance benchmark."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient
from gltf_fixture import glb
from texture_fixture import quad,png
from scene_capture import pixels


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'world.json'
    record=dict(passed=False,gpu=args.gpu,binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),calls=[],captures={},cleanup={})
    def uid(n):return '{:032x}'.format(n)
    def native(path):return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True,timeout=10).strip() if args.windows_interop else str(path.resolve())
    client=WorldClient.open(str(args.binary.resolve()),native(world),close_timeout=5)
    def call(method,params):
        result=client.call(method,params,timeout=30);record['calls'].append(dict(method=method,params=params,result=result));return result
    def generate(path):
        recipe=json.loads(path.read_text());job=call('asset.material.generate',dict(recipe=recipe));deadline=time.monotonic()+20
        while True:
            state=call('asset.material.job',dict(id=job['id']))
            if state['state']!='baking':break
            assert time.monotonic()<deadline,'Material bake deadline';time.sleep(.001)
        assert state['state']=='succeeded',state;return state['result']
    revision=0
    def transaction(ops):
        nonlocal revision
        result=call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops));revision=result['revision']
    def component(n,kind,value):return dict(op='component.set',id=uid(n),type=kind,value=value)
    def transform(position,scale=(1,1,1)):return dict(position=position,rotation=[0,0,0,1],scale=scale)
    probes=[dict(x=x,y=y) for y in range(195,346,25) for x in range(390,541,25)]
    def capture(name,view='color'):
        path=run/(name+'.bmp');result=call('world.capture',dict(revision=revision,camera=uid(1),path=native(path),gpu=args.gpu,samples=1,width=960,height=540,scene_debug_view=view,scene_product_probes=probes))
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,result
        record['captures'][name]=dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),report=result);return result,pixels(path)
    primary_failure=None
    try:
        brick=generate(ROOT/'examples/materials/wine-brick.recipe.json');plaster=generate(ROOT/'examples/materials/warm-plaster.recipe.json')
        doc,blob=quad([png(1,1,[255]*4)],uvs=[(0,2),(2,2),(2,0),(0,0)]);source=run/'repeat-quad.glb';source.write_bytes(glb(doc,blob))
        model=call('asset.import',dict(source=native(source)))['asset'];source.unlink()
        transaction([dict(op='entity.create',id=uid(1),name='Camera',parent=None),component(1,'Transform',transform([0,0,5])),component(1,'Camera',dict(vertical_fov=60,near=.1,far=100)),
            dict(op='entity.create',id=uid(2),name='Two-by-two physically matched repeat',parent=None),component(2,'Transform',transform([0,0,0],[2,1,1])),component(2,'StaticMesh',dict(asset=model,primitive=0,visible=True)),
            component(2,'PbrMaterial',brick['PbrMaterial']),component(2,'PbrTextures',dict(brick['PbrTextures'],normal_scale=0))])
        flat,flat_pixels=capture('brick-normal-disabled','shading_normal')
        transaction([component(2,'PbrTextures',brick['PbrTextures'])]);bumped,bumped_pixels=capture('brick-normal-enabled','shading_normal')
        before=flat['render_diagnostics']['scene_products']['probes'];after=bumped['render_diagnostics']['scene_products']['probes'];pairs=[(a,b) for a,b in zip(before,after) if a['surface_valid'] and b['surface_valid']]
        assert pairs and len(before)==len(after)==49
        assert all(max(abs(v-e) for v,e in zip(a['shading_normal'],[0,0,1]))<.003 for a,b in pairs),'Disabled normal must preserve independent +Z geometry oracle'
        assert any(max(abs(x-y) for x,y in zip(a['shading_normal'],b['shading_normal']))>.01 for a,b in pairs),'Generated height normals never reached scene products'
        assert flat_pixels!=bumped_pixels
        first,color=capture('wine-brick');repeated,same=capture('wine-brick-repeat');assert color==same,'Unchanged baked material/camera did not repeat pixels'
        transaction([component(1,'Transform',transform([0,0,9]))]);capture('wine-brick-distance')
        transaction([component(1,'Transform',transform([0,0,5])),component(2,'PbrMaterial',plaster['PbrMaterial']),component(2,'PbrTextures',plaster['PbrTextures'])]);_,plaster_pixels=capture('warm-plaster');assert plaster_pixels!=color
        # Existing original curved UV fixture exercises a separate imported tangent frame.
        curved=call('asset.import',dict(source=native(ROOT/'examples/assets/normal-sphere.glb')))['asset']
        transaction([component(2,'StaticMesh',dict(asset=curved,primitive=0,visible=True)),component(2,'Transform',transform([0,0,0]))]);capture('warm-plaster-curved')
        record['checks']=dict(actual_native_generated_maps_rendered=True,independent_flat_geometry_normal=True,generated_normals_change_products=True,unchanged_color_capture_equal=True,two_by_two_uv_repeat_fixture=True,two_camera_distances=True,plaster_color_changes=True,imported_curved_tangent_fixture=True)
        record['recipe_ids']=[brick['recipe_id'],plaster['recipe_id']];record['passed']=True
    except BaseException as error:
        primary_failure=error;record['failure']=dict(type=type(error).__name__,message=str(error));raise
    finally:
        try:client.close()
        except BaseException as error:
            record['cleanup_error']=str(error);record['passed']=False
            if primary_failure is None:raise
        finally:
            record['cleanup']=dict(exit_code=client.transport.returncode,closed=client.closed)
            if client.transport.returncode!=0 or not client.closed:record['passed']=False
            (run/'local-evidence.json').write_text(json.dumps(record,indent=2)+'\n')
    assert record['passed'],record['cleanup'];print(json.dumps(dict(passed=True,run=str(run),checks=record['checks'],cleanup=record['cleanup'])))
if __name__=='__main__':main()
