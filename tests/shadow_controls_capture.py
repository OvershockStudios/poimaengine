#!/usr/bin/env python3
"""Bounded real capture proposal: legacy controls first, then renderer shadow flags.

Native Windows Python, under a root-owned kill-on-close Job. Baseline makes eight
omitted-flag captures on the old binary. Candidate repeats them plus nineteen
controls; identical fixture and path/filter/light recipes are required. Neither
phase is a timing benchmark. Candidate publication requires actual native review.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1] if Path(__file__).parent.name=='tests' else Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/python'))
sys.path.insert(0, str(ROOT / 'tests'))
from poima_client import WorldClient
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels

WIDTH, HEIGHT = 960, 540
BASE = [.6, .2, .05]
EMISSION = [.01, .02, .03]
CAMERA, LIGHT, ENV, BOX, RECEIVER, STATIC, SECONDARY, RIG = [f'{n:032x}' for n in (1, 2, 3, 4, 100, 200, 300, 400)]


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            digest.update(chunk)
    return digest.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def primitive(root):
    return hashlib.sha256(('poima.instance.v1/' + root + '/node/0/primitive/0').encode()).hexdigest()[:32]


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def transform(entity, position=(0, 0, 0), scale=(1, 1, 1), rotation=(0, 0, 0, 1)):
    return component(entity, 'Transform', dict(position=list(position), rotation=list(rotation), scale=list(scale)))


def aim(direction):
    length = math.sqrt(sum(x*x for x in direction))
    d = [x / length for x in direction]
    q = [d[1], -d[0], 0, 1-d[2]]
    length = math.sqrt(sum(x*x for x in q))
    return [x / length for x in q] if length > 1e-12 else [0, 1, 0, 0]


def skinned_quad():
    """One-joint weighted quad, with an actual .1 m committed translation at t=1."""
    doc, raw = quad([], {'pbrMetallicRoughness': {'baseColorFactor': BASE+[1], 'metallicFactor': 0, 'roughnessFactor': .6}, 'doubleSided': True, 'emissiveFactor': EMISSION})
    data = bytearray(raw)
    def accessor(rows, kind, fmt='f'):
        while len(data) % 4:
            data.append(0)
        start = len(data)
        flat = [x for row in rows for x in row]
        data.extend(struct.pack('<' + fmt*len(flat), *flat))
        doc['bufferViews'].append(dict(buffer=0, byteOffset=start, byteLength=len(data)-start))
        doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1, componentType=5126 if fmt=='f' else 5121, count=len(rows), type=kind))
        return len(doc['accessors'])-1
    attrs = doc['meshes'][0]['primitives'][0]['attributes']
    attrs['JOINTS_0'] = accessor([(0, 0, 0, 0)]*4, 'VEC4', 'B')
    attrs['WEIGHTS_0'] = accessor([(1, 0, 0, 0)]*4, 'VEC4')
    matrix = accessor([(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)], 'MAT4')
    times = accessor([(0,), (2,)], 'SCALAR')
    doc['accessors'][times].update(min=[0], max=[2])
    values = accessor([(0, 0, 0), (.2, 0, 0)], 'VEC3')
    doc['nodes'] = [dict(name='Weighted quad', mesh=0, skin=0), dict(name='Moving joint')]
    doc['scenes'] = [dict(nodes=[0, 1])]
    doc['skins'] = [dict(name='One-joint fixture', skeleton=1, joints=[1], inverseBindMatrices=matrix)]
    doc['animations'] = [dict(name='Translate', samplers=[dict(input=times, output=values, interpolation='LINEAR')], channels=[dict(sampler=0, target=dict(node=1, path='translation'))])]
    doc['buffers'][0]['byteLength'] = len(data)
    return doc, bytes(data)


class Owner:
    def __init__(self, args, record):
        self.args, self.record = args, record
        self.world = args.output / 'world.json'
        self.client = WorldClient.open(args.binary.resolve(), self.world, cwd=ROOT)
        self.deadline = time.monotonic() + 300
        self.revision, self.session, self.rpc_bytes = 0, None, 0
        self.calls = record['calls']

    def rpc(self, method, params):
        remaining = self.deadline-time.monotonic()
        need(remaining > 0 and len(self.calls) < 500, 'Deadline/RPC bound exceeded.')
        entry = dict(method=method, params=copy.deepcopy(params))
        self.calls.append(entry)
        try:
            entry['result'] = self.client.call(method, params, timeout=min(60, remaining))
            self.rpc_bytes += len(json.dumps(entry, allow_nan=False).encode())
            need(self.rpc_bytes <= 16*1048576 and time.monotonic() <= self.deadline, 'RPC byte/deadline bound exceeded.')
            return entry['result']
        except BaseException as error:
            entry['error'] = repr(error)
            raise

    def edit(self, ops):
        self.revision = self.rpc('world.transact', dict(base_revision=self.revision, request_id=uuid.uuid4().hex, ops=ops))['revision']

    def stop(self):
        if self.session:
            need(self.rpc('runtime.stop', dict(session_id=self.session))['stopped'], 'Runtime stop failed.')
            self.session = None

    def close(self):
        try:
            self.stop()
        finally:
            self.client.close()
            self.record['native_owner'] = dict(exit_code=self.client.transport.returncode, stderr=self.client.transport.stderr_tail,
                stderr_truncated=self.client.transport.stderr_truncated, rpc_bytes=self.rpc_bytes)
        row = self.record['native_owner']
        need(row['exit_code'] == 0 and row['stderr'] == '' and row['stderr_truncated'] is False, 'Native owner did not close normally.')


def value(asset, visible=True, **flags):
    return dict(asset=asset, primitive=0, visible=visible, **flags)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', choices=('baseline', 'candidate'), required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), required=True)
    parser.add_argument('--shadow-filter', choices=('pcf3_point', 'pcf3_fractional'), required=True)
    parser.add_argument('--gpu', type=int, default=1)
    args = parser.parse_args()
    started_at = time.monotonic()
    need(os.name == 'nt' and not sys.flags.optimize, 'Native Windows Python under the root Job is required.')
    need((args.phase == 'candidate') == (args.baseline is not None), 'Candidate requires one retained baseline evidence file.')
    need(not args.output.exists(), 'Output must be new.')
    args.output.mkdir(parents=True)
    (args.output/'captures').mkdir()
    record = dict(passed=False, phase=args.phase, lighting_path=args.lighting_path, shadow_filter=args.shadow_filter,
        gpu_index=args.gpu, fixture_sha256=sha(__file__), binary_sha256=sha(args.binary), calls=[], captures=[], comparisons=[],
        scope='Bounded renderer flag controls, no performance or exhaustive hardware claim.', native_owner=None)
    owner, failure = None, None
    try:
        baseline = None
        if args.baseline:
            need(args.baseline.stat().st_size <= 16*1048576, 'Baseline receipt exceeds limit.')
            baseline = json.loads(args.baseline.read_text())
            need(baseline['passed'] is True and baseline['phase']=='baseline' and baseline['fixture_sha256']==record['fixture_sha256']
                and baseline['lighting_path']==args.lighting_path and baseline['shadow_filter']==args.shadow_filter
                and baseline['gpu_index']==args.gpu and len(baseline['captures'])==8, 'Baseline recipe/source identity mismatch.')
            record['baseline_sha256'] = sha(args.baseline)
        owner = Owner(args, record)
        doc, blob = quad([], {'pbrMetallicRoughness': {'baseColorFactor': BASE+[1], 'metallicFactor': 0, 'roughnessFactor': .6}, 'doubleSided': True, 'emissiveFactor': EMISSION})
        assets = []
        for name, parts in [('quad', (doc, blob)), ('skin', skinned_quad())]:
            path = args.output/(name+'.glb');path.write_bytes(glb(*parts))
            assets.append(owner.rpc('asset.import', dict(source=str(path)))['asset'])
        flat, skin = assets
        binding=owner.rpc('asset.animation.skin',dict(asset=skin,skin=0,offset=0,limit=1))
        identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
        need(len(binding['items'])==1 and binding['items'][0]['node']==1 and binding['items'][0]['inverse_bind']==identity,'Native binding is not the original identity inverse bind.')
        record['native_skin_binding']=binding
        record['asset_inputs'] = {n: sha(args.output/(n+'.glb')) for n in ('quad', 'skin')}
        box = dict(primitive='box', visible=True, albedo=[.2, .2, .2])
        ops=[]
        for entity in (CAMERA, LIGHT, ENV, BOX):
            ops.append(dict(op='entity.create', id=entity, name='Control '+entity))
            ops.append(transform(entity))
        ops += [component(CAMERA, 'Camera', dict(vertical_fov=60, near=.1, far=100)),
            component(BOX, 'MeshRenderer', box), component(ENV, 'LightingEnvironment', dict(ambient=[.02]*3, exposure=1, shadow_resolution=1024))]
        for root, asset in ((RECEIVER,flat),(STATIC,flat),(SECONDARY,flat),(RIG,skin)):
            ops.append(dict(op='asset.instantiate', id=root, asset=asset, name='Shadow control'))
        owner.edit(ops)
        skins = dict(value(skin), rig=RIG, node=0)
        cases = [('box-'+kind, 'box', kind, 'default') for kind in ('directional','point','spot')]
        cases += [('static', 'static', 'directional', 'default'), ('off-static','off-static','directional','default'),
            ('skin','skin','directional','default'), ('off-skin','off-skin','directional','default'), ('skin-receiver','skin-receiver','directional','default')]
        if args.phase == 'candidate':
            cases += [('box-'+kind+'-'+mode,'box',kind,mode) for kind in ('directional','point','spot') for mode in ('cast-off','receive-off','both-off')]
            cases += [(scene+'-cast-off',scene,'directional','cast-off') for scene in ('static','off-static','skin','off-skin')]
            cases += [('skin-receiver-receive-off','skin-receiver','directional','receive-off'),
                ('box-hidden','box','directional','hidden'), ('skin-hidden','skin','directional','hidden')]
            cases += [(scene+'-explicit-true',scene,'directional','explicit-true') for scene in ('box','static','skin')]
        for label, scene, kind, mode in cases:
            owner.stop()
            # Full component replacement deliberately removes any earlier optional flags.
            caster_flags = {'cast_shadows':False} if mode in ('cast-off','both-off') else {}
            receiver_flags = {'receive_shadows':False} if mode in ('receive-off','both-off') else {}
            if mode=='explicit-true':
                caster_flags = receiver_flags = dict(cast_shadows=True, receive_shadows=True)
            skin_receiver = scene=='skin-receiver'
            use_box = scene in ('box','skin-receiver')
            use_static = scene in ('static','off-static')
            use_skin = scene in ('skin','off-skin','skin-receiver')
            primary_flags = receiver_flags
            skin_flags = receiver_flags if skin_receiver else caster_flags
            light = dict(kind=kind, color=[1]*3, intensity=math.pi if kind=='directional' else 20*math.pi, enabled=True,
                shadow=dict(enabled=True, distance=80, bias=.0001, normal_bias=.005))
            if kind=='spot':
                light.update(inner_angle=0, outer_angle=45)
            lens = 5 if scene.startswith('off-') else 60
            recipe = dict(scene=scene,kind=kind,lens=lens,light=light,pose=dict(clip=0,time=1,speed=1,loop=False,playing=False),ambient=[.02]*3,emission=EMISSION)
            ops = [transform(CAMERA,(0,0,6)), component(CAMERA,'Camera',dict(vertical_fov=lens,near=.1,far=100)),
                transform(LIGHT,(2,0,4),rotation=aim([-2,0,-4])),component(LIGHT,'Light',light),
                transform(BOX,(1,0,2),(.4,.4,.1) if skin_receiver else (.8,.8,.8)),component(BOX,'MeshRenderer',dict(box,visible=use_box and mode!='hidden',**caster_flags)),
                transform(RECEIVER,scale=(4,4,4)),component(primitive(RECEIVER),'StaticMesh',value(flat,not skin_receiver,**primary_flags)),
                transform(STATIC,(1,0,2),(.6,.6,.6)),component(primitive(STATIC),'StaticMesh',value(flat,use_static,**caster_flags)),
                transform(SECONDARY,(0,0,-2),(4,4,4)),component(primitive(SECONDARY),'StaticMesh',value(flat,skin_receiver)),
                transform(RIG,(0,0,0) if skin_receiver else (1,0,2),(.6,.6,.6)),
                component(primitive(RIG),'SkinnedMesh',dict(skins,visible=use_skin and mode!='hidden',**skin_flags))]
            owner.edit(ops)
            owner.session = uuid.uuid4().hex
            started = owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision))
            need(started['started'] and started['tick']==0,'Fresh runtime failed.')
            step = owner.rpc('runtime.step',dict(session_id=owner.session,request_id=uuid.uuid4().hex,expected_tick=0,ticks=1,
                animations=[dict(entity=RIG,**recipe['pose'])]))
            need(step['tick']==1,'Actual frozen pose tick failed.')
            joint=hashlib.sha256(('poima.instance.v1/'+RIG+'/node/1').encode()).hexdigest()[:32]
            ids=[joint,RIG,primitive(RIG),primitive(RECEIVER),primitive(STATIC),BOX,primitive(SECONDARY)]
            observe = dict(session_id=owner.session,tick=1,ids=ids,entity_fields=['world_matrix','animation'])
            before=owner.rpc('runtime.observe',observe)
            rig_state=next(row['state']['animation'] for row in before['entities'] if row['id']==RIG)
            joint_world=next(row['state']['world_matrix'] for row in before['entities'] if row['id']==joint)
            mesh_world=next(row['state']['world_matrix'] for row in before['entities'] if row['id']==primitive(RIG))
            palette=[0.0]*16;palette[15]=1.0
            for col in range(3):
                for row in range(3):
                    need(abs(mesh_world[col*4+row]-(.6 if col==row else 0))<1e-6,'Unexpected skin mesh affine basis.')
                    palette[col*4+row]=joint_world[col*4+row]/.6
            for axis in range(3):palette[12+axis]=(joint_world[12+axis]-mesh_world[12+axis])/.6
            need(abs(palette[12]-.1)<1e-6 and abs(palette[13])<1e-6 and abs(palette[14])<1e-6,'Actual native joint/mesh palette is not the committed nonidentity pose.')
            need(rig_state['clip']==0 and abs(rig_state['time']-1)<=1e-6 and rig_state['playing'] is False,'Uncommitted or substituted pose.')
            world_hash=sha(owner.world)
            path=args.output/'captures'/(label+'.bmp')
            result=owner.rpc('runtime.capture',dict(session_id=owner.session,tick=1,camera=CAMERA,path=str(path),gpu=args.gpu,
                width=WIDTH,height=HEIGHT,samples=4 if args.lighting_path=='forward' else 1,lighting_path=args.lighting_path,
                shadow_filter=args.shadow_filter,culling=not (scene=='off-static' and mode=='cast-off'),ambient_occlusion={'mode':'none'},reconstruction='none'))
            after=owner.rpc('runtime.observe',observe)
            need(before==after and world_hash==sha(owner.world),'Capture mutated frozen state or authored bytes.')
            need(result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,'Real hardware capture failed.')
            diag=result['render_diagnostics'];counts=diag['last_draws']
            need(diag['lighting_path']==args.lighting_path and diag['shadow_filter']==args.shadow_filter,'Capture mode substitution.')
            image=pixels(path);need(len(image)==HEIGHT and len(image[0])==WIDTH,'Capture extent mismatch.')
            center=image[HEIGHT//2][WIDTH//2]
            is_lit=mode in ('cast-off','receive-off','both-off','hidden')
            if is_lit:
                need(center[0]>60,'Shadow suppression did not illuminate receiver: '+label)
            else:
                expected=[round(255*(12.92*((x*.02+e)/(1+x*.02+e)) if (x*.02+e)/(1+x*.02+e)<=.0031308 else 1.055*((x*.02+e)/(1+x*.02+e))**(1/2.4)-.055)) for x,e in zip(BASE,EMISSION)]
                need(max(abs(a-b) for a,b in zip(center,expected))<=4,'Default shadow/ambient numeric control failed: '+label)
            if scene.startswith('off-') and mode!='hidden':
                if scene=='off-static' and mode=='cast-off':
                    need(counts['camera_culled']==0 and counts['camera_draws']==2,'Unculled static control lost camera work.')
                    need(counts['shadow_candidates']==counts['shadow_views'],'Disabled cast was restored when culling was disabled.')
                else:
                    need(counts['camera_culled']>=1 and counts['camera_draws']==1,'Shadow-only geometry entered camera pass.')
                    if mode=='default':
                        need(counts['shadow_draws']>counts['shadow_views'],'Off-camera caster produced no shadow draw beyond its visible receiver.')
                if use_skin:
                    need(counts['skinned_instances']==(0 if mode=='cast-off' else 1),'Shadow-only GPU skin work mismatch.')
            elif use_skin and mode!='hidden':
                need(counts['skinned_instances']==1,'Visible posed geometry did not use GPU skinning.')
            if mode=='hidden':
                need(counts['camera_draws']==1 and counts['skinned_instances']==0,'Legacy hidden renderer was resurrected.')
            secondary=None
            if skin_receiver:
                # The small weighted receiving quad also shadows the visible plane at z=-2.
                x=round(WIDTH/2+(-1.4/8)*(HEIGHT/2)/math.tan(math.radians(30)))
                secondary=image[HEIGHT//2][x]
                expected=[round(255*(12.92*((v*.02+e)/(1+v*.02+e)) if (v*.02+e)/(1+v*.02+e)<=.0031308 else 1.055*((v*.02+e)/(1+v*.02+e))**(1/2.4)-.055)) for v,e in zip(BASE,EMISSION)]
                need(max(abs(a-b) for a,b in zip(secondary,expected))<=4,'Receive-disabled renderer stopped casting onto second receiver.')
            row=dict(label=label,recipe=recipe,mode=mode,image='captures/'+path.name,image_sha256=sha(path),center=center,
                secondary=secondary,native_palette=palette,result=result,world_sha256=world_hash)
            record['captures'].append(row)
            if mode=='default' and baseline:
                old=next(item for item in baseline['captures'] if item['label']==label)
                oldpath=args.baseline.parent/old['image']
                need(old['recipe']==recipe and old['image_sha256']==sha(oldpath)==row['image_sha256'],'Legacy omitted-default image changed: '+label)
                need(old['result']['gpu']==result['gpu'],'Actual device changed from baseline.')
                record['comparisons'].append(dict(kind='legacy_byte_exact',label=label,sha256=row['image_sha256']))
            if mode=='explicit-true':
                default_label='box-directional' if scene=='box' else scene
                reference=next(item for item in record['captures'] if item['label']==default_label)
                need(reference['image_sha256']==row['image_sha256'],'Explicit true differs from omitted defaults.')
                record['comparisons'].append(dict(kind='explicit_true_byte_exact',label=label,sha256=row['image_sha256']))
            if mode=='cast-off' and not scene.startswith('off-'):
                need(counts['camera_draws']==2 and image[HEIGHT//2][597][0]>20,'Cast-disabled camera object disappeared.')
                reference_label='box-'+kind if scene=='box' else scene
                reference=next(item for item in record['captures'] if item['label']==reference_label)
                oldpixels=pixels(args.output/reference['image'])
                need(max(abs(a-b) for a,b in zip(image[HEIGHT//2][597],oldpixels[HEIGHT//2][597]))<=1,'Cast-disabled camera surface changed.')
            need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())<=112*1048576,'Capture output exceeded bounded budget.')
        need(len(record['captures'])==(8 if args.phase=='baseline' else 27),'Incomplete capture matrix.')
        owner.close();owner=None
        need(time.monotonic()-started_at<=300,'Whole fixture deadline exceeded during normal owner cleanup.')
        record['passed']=True
    except BaseException:
        failure=traceback.format_exc();record['error']=failure
    finally:
        if owner:
            try:owner.close()
            except BaseException:record['cleanup_error']=traceback.format_exc();record['passed']=False
        record['seconds']=time.monotonic()-started_at
        record['retained_output_cap_bytes']=128*1048576
        record['retained_output_bytes_before_evidence']=sum(path.stat().st_size for path in args.output.rglob('*') if path.is_file())
        encoded=json.dumps(record,indent=2,allow_nan=False)+'\n'
        if record['retained_output_bytes_before_evidence']+len(encoded.encode())>record['retained_output_cap_bytes']:
            record['passed']=False;record['retention_error']='Whole output cap exceeded.'
            encoded=json.dumps(record,indent=2,allow_nan=False)+'\n'
        (args.output/'evidence.json').write_text(encoded,encoding='utf-8')
        if time.monotonic()-started_at>300:
            record['passed']=False;record['seconds']=time.monotonic()-started_at
            record['publication_error']='Whole300s deadline exceeded during evidence serialization/publication.'
            save(args.output/'evidence.json',record)
    print(json.dumps(dict(passed=record['passed'],phase=args.phase,captures=len(record['captures']),error=failure)))
    return 0 if record['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
