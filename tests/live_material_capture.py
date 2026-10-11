#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paired scalar-material captures; native Windows execution requires an external owned Job.

One runtime per subject, unchanged throughout three material applications. RGB
max error <=1 is fixed before execution; each stage must change >=100 pixels by
>=2 levels. These captures qualify neither player-window continuity nor latency.
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

_SOURCE = Path(__file__).resolve()
ROOT = _SOURCE.parents[1] if (_SOURCE.parents[1]/'CMakeLists.txt').is_file() else _SOURCE.parents[4]
sys.path.insert(0, str(ROOT/'tools/python'))
sys.path.insert(0, str(ROOT/'tests'))
from poima_client import WorldClient
from poima_client.transport import JsonRpcProcess
from shadow_controls_capture import quad, skinned_quad, primitive, component, transform
from gltf_fixture import glb
from scene_capture import pixels

WIDTH, HEIGHT, CAP = 320, 240, 64*1048576
CAMERA, LIGHT, ENV, BOX, STATIC, RIG = [f'{n:032x}' for n in (1,2,3,4,200,400)]
BASE = dict(base_color=[.6,.2,.05], emissive=[0,0,0], metallic=.6, roughness=.08, double_sided=True)


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            digest.update(data)
    return digest.hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode()


def same(a, b):
    return encoded(a) == encoded(b)


def inventory(directory):
    return {str(p.relative_to(directory)):dict(bytes=p.stat().st_size,sha256=sha(p))
            for p in sorted(directory.rglob('*')) if p.is_file()}


class Owner:
    def __init__(self, args, record, deadline):
        self.args, self.record, self.deadline = args, record, deadline
        self.revision, self.session, self.raw_bytes = 0, None, 0
        owner = self
        class RecordedTransport(JsonRpcProcess):
            def _accept_response(self, response):
                owner.raw(dict(direction='response',message=copy.deepcopy(response)))
                super()._accept_response(response)
        self.transport = RecordedTransport([str(args.binary.resolve()),'world',str(args.output/'world.json')], cwd=ROOT)
        request = self.transport.request
        def logged(method, params=None, timeout=30.0):
            remaining = self.deadline-time.monotonic()
            need(remaining > 0, 'Fixture request deadline expired.')
            with self.transport._condition:
                identifier = self.transport._next_id
            self.raw(dict(direction='request',message=dict(jsonrpc='2.0',id=identifier,method=method,params=copy.deepcopy(params or {}))))
            return request(method, params, timeout=min(timeout, remaining))
        self.transport.request = logged
        self.client = WorldClient(self.transport)

    def raw(self, row):
        self.raw_bytes += len(encoded(row))
        need(self.raw_bytes<=16*1048576 and len(self.record['transport'])<1000, 'Raw RPC evidence bound exceeded.')
        self.record['transport'].append(row)

    def rpc(self, method, params):
        row = dict(method=method,params=copy.deepcopy(params))
        self.record['calls'].append(row)
        try:
            value=self.client.call(method, params, timeout=min(60,max(.001,self.deadline-time.monotonic())))
            row['result']=copy.deepcopy(value)
            need(time.monotonic()<=self.deadline, 'Late reply exceeded fixture deadline.')
            need(sum(p.stat().st_size for p in self.args.output.rglob('*') if p.is_file())+2*len(encoded(self.record))<=CAP,
                 'Aggregate output/evidence bound exceeded.')
            return value
        except BaseException as error:
            row['error']=repr(error)
            raise

    def edit(self, ops):
        self.revision=self.rpc('world.transact',dict(base_revision=self.revision,request_id=uuid.uuid4().hex,ops=ops))['revision']

    def stop(self):
        if self.session:
            need(self.rpc('runtime.stop',dict(session_id=self.session))['stopped'], 'Runtime stop failed.')
            self.session=None

    def close(self):
        try:
            self.stop()
        finally:
            self.client.close()
            self.record['native_owner']=dict(exit_code=self.transport.returncode,stderr=self.transport.stderr_tail,
                stderr_truncated=self.transport.stderr_truncated,rpc_bytes=self.raw_bytes)
        row=self.record['native_owner']
        need(type(row['exit_code']) is int and row['exit_code']==0 and row['stderr']=='' and row['stderr_truncated'] is False,
             'Native owner did not close normally.')


def compare(left, right):
    need(len(left)==HEIGHT and len(right)==HEIGHT and all(len(row)==WIDTH for row in left+right), 'Wrong image extent.')
    maximum, changed, strong, total = 0,0,0,0
    for arow,brow in zip(left,right):
        for a,b in zip(arow,brow):
            delta=max(abs(x-y) for x,y in zip(a,b))
            maximum=max(maximum,delta);changed+=delta>0;strong+=delta>=2
            total+=sum(abs(x-y) for x,y in zip(a,b))
    return dict(max_rgb_delta=maximum,changed_pixels=changed,strong_changed_pixels=strong,total_rgb_delta=total)


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
         result['object_count']==1 and result['build_version']=='0.0.113', 'Hardware capture identity/shape failed.')
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


def checkpoint(owner, slot, epoch):
    result=owner.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,
        expected_generation=0,session_id=owner.session,expected_tick=0,expected_gameplay_revision=0,
        expected_material_revision=epoch))
    need(result['generation']==1, 'Fresh save generation changed.')
    status=owner.rpc('save.inspect',dict(slot=slot))
    directory=owner.args.output/'saves'/('slot-'+slot)
    files=list(directory.glob('*.bin'))
    need(len(files)==1 and files[0].stat().st_size<=1048576 and sha(files[0])==status['selected']['sha256'] and
         status['selected']['verified'] is True, 'Checkpoint bytes do not match verified native slot.')
    save=json.loads(files[0].read_text(encoding='utf-8'))
    need(save['format']=='poima.world-save' and save['version']==1, 'Unexpected checkpoint envelope.')
    owner.record['checkpoints'].append(dict(slot=slot,status=status,files=inventory(directory),
        payload_sha256=hashlib.sha256(encoded(save['snapshot']['payload'])).hexdigest()))
    return save,directory,inventory(directory)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),required=True)
    parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args();args.binary=args.binary.resolve();args.output=args.output.resolve()
    need(os.name=='nt' and not sys.flags.optimize, 'Native Windows Python under an external owned Job is required.')
    need(not args.output.exists(), 'Output must be new.')
    args.output.mkdir(parents=True);(args.output/'captures').mkdir();(args.output/'saves').mkdir()
    started=time.monotonic();deadline=started+300
    source_files=[Path(__file__),ROOT/'tests/shadow_controls_capture.py',ROOT/'tests/texture_fixture.py',
        ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py']+sorted((ROOT/'tools/python/poima_client').glob('*.py'))
    pins={str(p):sha(p) for p in source_files}
    record=dict(passed=False,scope='Standalone paired scalar-material capture; no player-window, timing, AOT or Alpha completion claim.',
        recipe=dict(width=WIDTH,height=HEIGHT,lighting_path=args.lighting_path,gpu=args.gpu,maximum_rgb_error=1,
                    minimum_strong_changed_pixels=100,minimum_stage_max_delta=5,pose='bind, no tick/animation/transform mutation',
                    ambient_occlusion='none',reconstruction='none',frames_in_flight=2,samples=1,
                    minimum_roughness_hdr_difference=.02,minimum_old_hdr_spatial_contrast=.05),
        source_pins=pins,binary_sha256=sha(args.binary),binary_dependencies={'phonon.dll':sha(args.binary.parent/'phonon.dll')},
        calls=[],transport=[],captures=[],comparisons=[],checkpoints=[],cases=[],native_owner=None)
    owner=None;failure=None
    try:
        owner=Owner(args,record,deadline)
        record['discovery']=owner.rpc('world.describe',dict(view='method',name='runtime.material.apply'))
        need(record['discovery']['schema_revision']>=82, 'Material discovery revision is too old.')
        doc,raw=quad([],{'pbrMetallicRoughness':{'baseColorFactor':[.6,.2,.05,1], 'metallicFactor':.6,'roughnessFactor':.08},'doubleSided':True})
        assets=[]
        for name,parts in [('quad',(doc,raw)),('skin',skinned_quad())]:
            path=args.output/(name+'.glb');path.write_bytes(glb(*parts))
            assets.append(owner.rpc('asset.import',dict(source=str(path)))['asset'])
        record['asset_inputs']={name:sha(args.output/(name+'.glb')) for name in ('quad','skin')}
        flat,skin=assets;ops=[]
        for entity in (CAMERA,LIGHT,ENV,BOX):
            ops.extend([dict(op='entity.create',id=entity,name='Material '+entity),transform(entity)])
        ops.extend([transform(CAMERA,(0,0,6)),component(CAMERA,'Camera',dict(vertical_fov=40,near=.1,far=100)),
            transform(LIGHT,(0,0,3)),component(LIGHT,'Light',dict(kind='point',color=[1,1,1],intensity=12*math.pi,enabled=True,
                shadow=dict(enabled=False,distance=20,bias=.0001,normal_bias=.005))),
            component(ENV,'LightingEnvironment',dict(ambient=[.005]*3,exposure=1)),
            component(BOX,'MeshRenderer',dict(primitive='box',albedo=[.6,.2,.05],visible=False,cast_shadows=False)),
            component(BOX,'PbrMaterial',BASE)])
        for root,asset in ((STATIC,flat),(RIG,skin)):
            ops.append(dict(op='asset.instantiate',id=root,asset=asset,name='Material fixture'))
        owner.edit(ops)
        owner.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(args.output/'saves')))
        subjects=[('primitive',BOX),('static',primitive(STATIC)),('weighted',primitive(RIG))]
        for case,subject in subjects:
            owner.stop()
            owner.edit([component(BOX,'MeshRenderer',dict(primitive='box',albedo=[.6,.2,.05],visible=case=='primitive',cast_shadows=False)),
                component(primitive(STATIC),'StaticMesh',dict(asset=flat,primitive=0,visible=case=='static',cast_shadows=False)),
                component(primitive(RIG),'SkinnedMesh',dict(asset=skin,primitive=0,rig=RIG,node=0,visible=case=='weighted',cast_shadows=False)),
                component(subject,'PbrMaterial',BASE)])
            owner.session=uuid.uuid4().hex
            started_runtime=owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision))
            need(started_runtime['started'] is True and started_runtime['tick']==0, 'Fresh subject runtime failed.')
            original_info=owner.rpc('runtime.inspect',dict(session_id=owner.session))
            old_image,source,old_probes=capture(owner,case+'-baseline',True,0)
            original,old_directory,old_inventory=checkpoint(owner,case+'-original',0)
            value=copy.deepcopy(BASE)
            stages=[('color',dict(base_color=[.05,.6,.2])),('roughness',dict(roughness=.8)),
                    ('emissive-metallic',dict(emissive=[.12,.025,.005],metallic=.1))]
            for epoch,(stage,change) in enumerate(stages,1):
                value.update(change);owner.edit([component(subject,'PbrMaterial',copy.deepcopy(value))])
                authored,_,authored_probes=capture(owner,case+'-'+stage+'-authored',False,0)
                params=dict(session_id=owner.session,request_id=uuid.uuid4().hex,expected_tick=0,
                    expected_structure_revision=0,expected_material_revision=epoch-1,revision=owner.revision,
                    expected_authored_revision=original_info['authored_revision'],expected_content_sha256=original_info['content_sha256'],ids=[subject])
                applied=owner.rpc('runtime.material.apply',params)
                need(applied['material_revision']==epoch and applied['tick']==0 and applied['session_id']==owner.session,
                     'Apply changed native identity/time.')
                retry=owner.rpc('runtime.material.apply',params)
                expected=copy.deepcopy(applied);expected['replayed']=True
                need(same(retry,expected), 'Apply retry did not resolve original receipt.')
                live,_,live_probes=capture(owner,case+'-'+stage+'-live',True,epoch,source)
                parity=compare(authored,live);effect=compare(old_image,live)
                need(parity['max_rgb_delta']<=1, 'Live scalar image differs from authored target beyond declared allowance.')
                need(effect['strong_changed_pixels']>=100 and effect['max_rgb_delta']>=5, 'Scalar stage has no meaningful visible effect.')
                probe_error=max(abs(a-b) for left,right in zip(authored_probes,live_probes) for a,b in zip(left['raw_hdr'],right['raw_hdr']))
                need(probe_error<=.002 and all(a['surface_valid']==b['surface_valid'] for a,b in zip(authored_probes,live_probes)),
                     'Authored/live raw HDR probe mismatch.')
                highlight=None
                if stage=='roughness':
                    valid=[(a,b) for a,b in zip(old_probes,live_probes) if a['surface_valid'] and b['surface_valid']]
                    need(valid, 'Roughness probe domain is empty.')
                    old_brightness=[sum(a['raw_hdr'][:3])/3 for a,b in valid]
                    highlight=dict(raw_hdr_difference=max(abs(x-y) for a,b in valid for x,y in zip(a['raw_hdr'][:3],b['raw_hdr'][:3])),
                        old_spatial_contrast=max(old_brightness)-min(old_brightness))
                    need(highlight['raw_hdr_difference']>=.02 and highlight['old_spatial_contrast']>=.05,
                         'Roughness stage lacks a meaningful measured highlight change.')
                record['comparisons'].append(dict(case=case,stage=stage,parity=parity,effect=effect,
                    max_raw_hdr_parity_error=probe_error,roughness_highlight=highlight))
                info=owner.rpc('runtime.inspect',dict(session_id=owner.session))
                before=copy.deepcopy(original_info);after=copy.deepcopy(info)
                for row in (before,after):
                    for key in ('authored_revision','current_authored_revision','source_stale','material_revision','content_sha256'):row.pop(key)
                need(same(before,after) and info['source_stale'] is False, 'Material adoption changed unrelated runtime state.')
                saved,_,_=checkpoint(owner,case+'-'+stage,epoch)
                baseline=copy.deepcopy(original['snapshot']['payload']);current=copy.deepcopy(saved['snapshot']['payload'])
                for row in (baseline,current):row.pop('authored_revision');row.pop('content_sha256')
                need(same(baseline,current) and saved['document']['entities'][subject]['components']['PbrMaterial']==value,
                     'Apply changed another checkpoint domain or failed to save adopted factors.')
                need(inventory(old_directory)==old_inventory, 'Original durable slot changed after material adoption.')
                original_info=info;old_image=live;old_probes=live_probes
            record['cases'].append(dict(case=case,session_id=owner.session,tick=0,applications=3,original_slot_immutable=True))
        need(len(record['captures'])==21 and len(record['comparisons'])==9, 'Incomplete capture matrix.')
    except BaseException as error:
        failure=repr(error);record['error']=failure;record['traceback']=traceback.format_exc()
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException as error:
                record['cleanup_error']=repr(error);failure=failure or repr(error)
        record['seconds']=time.monotonic()-started
        try:
            need(record['seconds']<=300, 'Whole fixture deadline exceeded.')
            need(all(sha(Path(p))==value for p,value in pins.items()) and sha(args.binary)==record['binary_sha256'] and
                 sha(args.binary.parent/'phonon.dll')==record['binary_dependencies']['phonon.dll'], 'Source/native inputs changed.')
            record['passed']=failure is None and record['native_owner'] is not None
            data=encoded(record)+b'\n'
            need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data)<=CAP, 'Final output cap exceeded.')
            need(time.monotonic()<=deadline, 'Evidence publication exceeded deadline.')
            temporary=args.output/'evidence.pending.json'
            with temporary.open('xb') as stream:stream.write(data)
            need(time.monotonic()<=deadline, 'Evidence write exceeded whole fixture deadline.')
            temporary.rename(args.output/'evidence.json')
            need(time.monotonic()<=deadline, 'Evidence publication exceeded whole fixture deadline.')
        except BaseException as error:
            record['passed']=False;failure=failure or repr(error)
            record['publication_error']=repr(error);record['seconds']=time.monotonic()-started
            evidence=args.output/'evidence.json'
            if evidence.exists():evidence.rename(args.output/'late-evidence.json')
            with (args.output/'failure.json').open('xb') as stream:
                stream.write(encoded(record)+b'\n')
    print(json.dumps(dict(passed=record['passed'],captures=len(record['captures']),error=failure)))
    return 0 if record['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
