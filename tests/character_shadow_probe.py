#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Controlled licensed humanoid shadow experiment; native Windows Python only.

Example (root-owned, gated native execution, after other graphics owners exit):
  python character_shadow_probe.py --binary <native-poima.exe> \
    --manifest <retained-performance-yard/world/manifest.json> \
    --output <new-directory> --gpu 1

Exactly 33 primary color captures. Fixed floor/camera/material/lighting direction,
real committed frozen animation poses, fresh runtime for each authored variant.
This diagnoses existing shadows; 2048px is not ground truth and this is neither
a performance benchmark nor a universal visual-quality gate. No renderer edits,
imaginary shadow debug views, gameplay module, source imports or asset generation.
The supervisor owns the native child tree; this client owns one stdio world.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
sys.path.insert(0, str(ROOT / 'tests'))
from poima_client import WorldClient
from scene_capture import pixels

FORMAT = 'poima.character-shadow-probe.v1'
WIDTH, HEIGHT = 1280, 720
CAMERA, LIGHT, ENV, FLOOR, RIG = [f'{n:032x}' for n in (1, 2, 3, 4, 400)]
CAMERA_POSITION = [6, 5, 9]
CAMERA_TARGET = [0, 1.8, 0]
FOV, NEAR, FAR = 50, .05, 50
RESOLUTIONS = (256, 512, 1024, 2048)
BIAS = (0, .0001, .0005)
NORMAL_BIAS = (0, .001, .005)


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def regular(path):
    need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(), 'Linked input/output rejected.')
    need(stat.S_ISREG(path.lstat().st_mode), 'Input is not a regular file.')


def read_json(path):
    regular(path)
    need(path.stat().st_size <= 64 * 1024 * 1024, 'JSON exceeds 64 MiB.')
    def unique(pairs):
        value = {}
        for name, item in pairs:
            need(name not in value, 'Duplicate JSON key.')
            value[name] = item
        return value
    def floating(value):
        result = float(value)
        need(math.isfinite(result), 'Nonfinite JSON number.')
        return result
    def invalid(value):
        raise ValueError('Nonfinite JSON literal: ' + value)
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_float=floating, parse_constant=invalid)


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')


def inventory(directory):
    need(directory.is_dir() and not directory.is_symlink() and not getattr(directory, 'is_junction', lambda: False)(),
         'Cooked store must be a real directory.')
    rows, seen, total = [], set(), 0
    for path in sorted(directory.rglob('*')):
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(), 'Linked content rejected.')
        if path.is_dir():
            continue
        regular(path)
        name = path.relative_to(directory).as_posix()
        need(name.casefold() not in seen, 'Case-aliased cooked content.')
        seen.add(name.casefold())
        size = path.stat().st_size
        total += size
        need(size <= 256 * 1024 * 1024 and total <= 1024 * 1024 * 1024 and len(rows) < 10000,
             'Cooked input budget exceeded.')
        rows.append(dict(path=name, bytes=size, sha256=sha(path)))
    return rows


def normalize(vector):
    length = math.sqrt(sum(value * value for value in vector))
    need(length > 1e-12, 'Degenerate direction.')
    return [value / length for value in vector]


def aim(direction):
    # Same -Z-facing convention used by tests/shadow_capture.py.
    d = normalize(direction)
    q = [d[1], -d[0], 0, 1-d[2]]
    return normalize(q) if sum(value*value for value in q) > 1e-12 else [0, 1, 0, 0]


CAMERA_ROTATION = aim([target-origin for target, origin in zip(CAMERA_TARGET, CAMERA_POSITION)])


def rotate(q, vector):
    x, y, z, w = q
    a, b, c = vector
    uv = [y*c-z*b, z*a-x*c, x*b-y*a]
    uuv = [y*uv[2]-z*uv[1], z*uv[0]-x*uv[2], x*uv[1]-y*uv[0]]
    return [v+2*(w*u+t) for v, u, t in zip(vector, uv, uuv)]


def floor_probes():
    """Freeze pixels from world-space floor targets, with per-pixel plane depth.

    Native depth+upward normal must also agree: a projected target obscured by
    the character cannot count as a known floor observation.
    """
    tangent = math.tan(math.radians(FOV)/2)
    inverse = [-v for v in CAMERA_ROTATION[:3]] + [CAMERA_ROTATION[3]]
    probes, seen = [], set()
    targets = [(x, z) for z in (-.3, -.6, -.9, -1.2, -1.6, -2.1)
               for x in (-1, -.5, 0, .5, 1)] + [(x, z) for x in (-3, 3) for z in (-1, 1)]
    for x, z in targets:
        local = rotate(inverse, [x-CAMERA_POSITION[0], -CAMERA_POSITION[1], z-CAMERA_POSITION[2]])
        need(local[2] < 0, 'Floor target is behind camera.')
        px = int((1+local[0]/(-local[2]*tangent*(WIDTH/HEIGHT)))*WIDTH/2)
        py = int((1-local[1]/(-local[2]*tangent))*HEIGHT/2)
        need(0 <= px < WIDTH and 0 <= py < HEIGHT, 'Floor target falls outside fixed image.')
        if (px, py) in seen:
            continue
        seen.add((px, py))
        ray = rotate(CAMERA_ROTATION, [(2*(px+.5)/WIDTH-1)*tangent*(WIDTH/HEIGHT),
                                      (1-2*(py+.5)/HEIGHT)*tangent, -1])
        need(ray[1] < 0, 'Floor ray does not point down.')
        distance = -CAMERA_POSITION[1]/ray[1]
        hit = [origin+distance*delta for origin, delta in zip(CAMERA_POSITION, ray)]
        need(abs(hit[0]) < 6 and abs(hit[2]) < 6, 'Floor probe misses finite receiver.')
        # Ray parameter equals positive view distance because local ray.z=-1.
        depth = FAR/(FAR-NEAR) - FAR*NEAR/((FAR-NEAR)*distance)
        probes.append(dict(x=px, y=py, expected_floor_depth=depth, floor_world=hit))
    need(2 <= len(probes) <= 64, 'Invalid probe budget.')
    return probes


def component(identifier, kind, value):
    return dict(op='component.set', id=identifier, type=kind, value=value)


def transform(identifier, position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return component(identifier, 'Transform', dict(position=list(position), rotation=list(rotation), scale=list(scale)))


class Owner:
    def __init__(self, binary, world, record):
        self.world, self.record = world, record
        self.client = WorldClient.open(binary, world, cwd=ROOT)
        self.entry = dict(calls=[], exit_code=None)
        record['native_owners'].append(self.entry)
        self.deadline = time.monotonic()+900
        self.revision, self.session = None, None
        self.rpc_bytes = 0

    def rpc(self, method, params):
        need(len(self.entry['calls']) < 2000, 'RPC count budget exceeded.')
        remaining = self.deadline-time.monotonic()
        need(remaining > 0, 'Native owner deadline expired.')
        entry = dict(method=method, params=copy.deepcopy(params))
        self.entry['calls'].append(entry)
        try:
            entry['result'] = self.client.call(method, params, timeout=min(120, remaining))
            self.rpc_bytes += len(json.dumps(entry, ensure_ascii=False, allow_nan=False).encode('utf-8'))
            need(self.rpc_bytes <= 64 * 1024 * 1024, 'Retained RPC byte budget exceeded.')
            return entry['result']
        except BaseException as error:
            entry['error'] = repr(error)
            raise

    def transact(self, ops):
        self.revision = self.rpc('world.transact', dict(base_revision=self.revision,
            request_id=uuid.uuid4().hex, ops=ops))['revision']

    def stop(self):
        if self.session is not None:
            result = self.rpc('runtime.stop', dict(session_id=self.session))
            need(result['stopped'], 'Runtime did not stop.')
            self.session = None

    def snapshot(self, ids):
        status = self.rpc('runtime.inspect', dict(session_id=self.session))
        need(status['tick'] == 1, 'Frozen diagnostic runtime changed tick.')
        rows = []
        for offset in range(0, len(ids), 32):
            response = self.rpc('runtime.observe', dict(session_id=self.session, tick=1,
                ids=ids[offset:offset+32], entity_fields=['world_matrix', 'local_transform', 'animation']))
            need(not response['source_stale'] and response['authored_revision'] == self.revision,
                 'Observation is not the freshly authored world.')
            rows.extend(response['entities'])
        return dict(status=status, entities=sorted(rows, key=lambda row: row['id']),
                    lighting=self.rpc('runtime.lighting', dict(session_id=self.session, tick=1)))

    def close(self):
        try:
            self.stop()
        finally:
            self.client.close()
            self.entry.update(exit_code=self.client.transport.returncode,
                              stderr=self.client.transport.stderr_tail,
                              stderr_truncated=self.client.transport.stderr_truncated, rpc_bytes=self.rpc_bytes)
        need(self.entry['exit_code'] == 0 and not self.entry['stderr'] and not self.entry['stderr_truncated'],
             'Native owner did not exit cleanly.')


def page(owner, asset, section):
    rows, offset = [], 0
    for _ in range(160):
        response = owner.rpc('asset.inspect', dict(asset=asset, section=section, offset=offset, limit=64))
        rows.extend(response['items'])
        next_offset = response['next_offset']
        if next_offset is None:
            return rows
        need(type(next_offset) is int and next_offset > offset, 'Asset pagination did not advance.')
        offset = next_offset
    raise AssertionError('Asset pagination budget exceeded.')


def light_value(enabled, bias, normal_bias):
    return dict(kind='directional', color=[1, 1, 1], intensity=2.2, enabled=True,
        shadow=dict(enabled=enabled, near=.05, distance=30, bias=bias, normal_bias=normal_bias))


def variant(owner, character, pose_name, clip, clip_time, resolution, shadow, bias, normal_bias, ids, directory):
    owner.stop()
    ops = [component(LIGHT, 'Light', light_value(shadow, bias, normal_bias)),
           component(ENV, 'LightingEnvironment', dict(ambient=[.03, .03, .03], exposure=1,
                                                      shadow_resolution=resolution)),
           component(RIG, 'AnimationRig', dict(asset=character['asset'], clip=clip, time=clip_time,
                                               speed=1, loop=False, playing=False))]
    owner.transact(ops)
    owner.session = uuid.uuid4().hex
    start = owner.rpc('runtime.start', dict(session_id=owner.session, revision=owner.revision))
    need(start['started'] and start['tick'] == 0, 'Fresh frozen runtime failed to start.')
    step = owner.rpc('runtime.step', dict(session_id=owner.session, request_id=uuid.uuid4().hex,
        expected_tick=0, ticks=1, animations=[dict(entity=RIG, clip=clip, time=clip_time,
                                                speed=1, loop=False, playing=False)]))
    need(step['tick'] == 1, 'Legitimate pose seek failed.')
    state = owner.snapshot(ids)
    rig = next(row['state'] for row in state['entities'] if row['id'] == RIG)
    need(rig['animation']['clip'] == clip and not rig['animation']['playing'] and
         abs(rig['animation']['time']-clip_time) <= 1e-6, 'Actual committed pose differs from the requested seek.')
    name = f'{pose_name}-r{resolution}-s{int(shadow)}-b{bias:g}-n{normal_bias:g}'
    save_json(directory/(name+'.json'), dict(authored_world=read_json(owner.world), runtime=state))
    return name, state


def capture(owner, name, state, ids, probes, ao, output, record):
    label = name+'-ao'+str(int(ao))
    path = output/'captures'/(label+'.bmp')
    need(not path.exists(), 'Capture path already exists.')
    world_sha = sha(owner.world)
    settings = dict(mode='gtao' if ao else 'none', quality='medium', radius=1)
    row = dict(label=label, pose_variant=name, ambient_occlusion=settings,
               shadow_filter=record['shadow_filter'],
               authored_revision=owner.revision, world_sha256=world_sha, image='captures/'+path.name)
    record['captures'].append(row)
    response = owner.rpc('runtime.capture', dict(session_id=owner.session, tick=1, camera=CAMERA,
        path=str(path), width=WIDTH, height=HEIGHT, gpu=record['gpu_index'], samples=1,
        lighting_path='deferred', reconstruction='none', capture_frames=2, profile=True,
        shadow_filter=record['shadow_filter'],
        scene_debug_view='color', scene_product_probes=[dict(x=p['x'], y=p['y']) for p in probes],
        ambient_occlusion=settings))
    row['result'] = response
    need(response['source'] == 'runtime' and response['session_id'] == owner.session and response['tick'] == 1 and
         response['hardware'] and response['capture_written'] and response['nvrhi_errors'] == 0 and
         response['width'] == WIDTH and response['height'] == HEIGHT and response['samples'] == 1,
         'Native hardware runtime capture failed.')
    need(all(abs(response['lens'][key]-expected) <= 1e-6 for key, expected in
             dict(vertical_fov=FOV, near=NEAR, far=FAR).items()), 'Camera lens changed.')
    frozen_camera = next(row['state']['world_matrix'] for row in state['entities'] if row['id'] == CAMERA)
    need(response['camera_world'] == frozen_camera, 'Actual capture camera differs from frozen runtime camera.')
    if 'device' not in record:
        record['device'] = response['gpu']
    need(isinstance(response['gpu'], str) and response['gpu'] == record['device'], 'Actual graphics device changed.')
    diagnostic = response['render_diagnostics']
    need(diagnostic['shadow_filter'] == record['shadow_filter'], 'Effective shadow filter differs.')
    need(diagnostic['lighting_path'] == 'deferred' and diagnostic['ambient_occlusion'] == settings and
         diagnostic['scene_products']['view'] == 'color', 'Effective rendering settings changed.')
    need(diagnostic['last_draws']['skinned_instances'] == 1 and
         diagnostic['last_draws']['skinned_vertices'] == 1029, 'Imported skin was not actually submitted.')
    expected_views = 4 if state['lighting']['lights'][0]['shadow']['enabled'] else 0
    need(response['lighting'] == {key: value for key, value in state['lighting'].items()
                                 if key not in ('revision', 'session_id', 'tick')} and
         response['lighting']['shadow_views'] == expected_views and
         response['lighting']['shadow_bytes'] == expected_views*response['lighting']['shadow_resolution']**2*4,
         'Actual captured light/shadow resource settings differ from frozen runtime.')
    need(diagnostic['ambient_occlusion_buffer_bytes'] == (WIDTH*HEIGHT*4 if ao else 0), 'AO resource admission differs.')
    for field in ('ambient_occlusion_gpu', 'ambient_occlusion_filter_gpu'):
        need(diagnostic[field]['samples'] > 0 if ao else diagnostic[field]['samples'] == 0, 'AO pass admission differs.')
    observed = diagnostic['scene_products']['probes']
    need(len(observed) == len(probes), 'Scene product probes missing.')
    for requested, sample in zip(probes, observed):
        need((sample['x'], sample['y']) == (requested['x'], requested['y']), 'Probe location differs.')
        need(all(math.isfinite(value) for value in [sample['depth'], *sample['shading_normal'], *sample['raw_hdr']]),
             'Nonfinite scene product.')
        if not ao:
            need(sample['raw_ambient_visibility'] == 1 and sample['ambient_visibility'] == 1, 'Disabled AO visibility differs.')
    image = pixels(path)
    need(len(image) == HEIGHT and all(len(line) == WIDTH for line in image), 'Bitmap size differs.')
    row['image_sha256'] = sha(path)
    need(owner.snapshot(ids) == state and sha(owner.world) == world_sha, 'Capture mutated frozen pose/state/world.')
    return row


def floor_difference(off, on, probes, output):
    left, right = pixels(output/off['image']), pixels(output/on['image'])
    a = off['result']['render_diagnostics']['scene_products']['probes']
    b = on['result']['render_diagnostics']['scene_products']['probes']
    observations = []
    for requested, unshadowed, shadowed in zip(probes, a, b):
        def floor(sample):
            return sample['surface_valid'] and abs(sample['depth']-requested['expected_floor_depth']) <= 5e-6 and \
                max(abs(value-target) for value, target in zip(sample['shading_normal'], [0, 1, 0])) <= .003
        if not (floor(unshadowed) and floor(shadowed)):
            continue
        x, y = requested['x'], requested['y']
        delta = max(abs(c-d) for c, d in zip(left[y][x], right[y][x]))
        darkening = max(c-d for c, d in zip(unshadowed['raw_hdr'], shadowed['raw_hdr']))
        observations.append(dict(x=x, y=y, floor_world=requested['floor_world'],
                                 maximum_byte_difference=delta, hdr_darkening=darkening))
    changed = [row for row in observations if row['maximum_byte_difference'] >= 2 and row['hdr_darkening'] > .001]
    need(len(observations) >= 8 and len(changed) >= 2,
         'Direct shadows did not change two independently depth/normal-verified floor locations.')
    return dict(off=off['label'], on=on['label'], verified_floor_samples=len(observations),
                changed_floor_samples=len(changed), samples=observations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--shadow-filter', choices=('pcf3_point', 'pcf3_fractional', 'pcss_directional'), default='pcf3_point')
    args = parser.parse_args()
    need(os.name == 'nt', 'Run under native Windows Python with the root-owned process-tree supervisor.')
    binary, manifest = args.binary.resolve(strict=True), args.manifest.resolve(strict=True)
    output = args.output.resolve()
    need(not output.exists() and args.gpu >= 0, 'Supply a new output and nonnegative GPU index.')
    regular(binary)
    source_world = manifest.parent/'world.json'
    source_store = Path(str(source_world)+'.assets')
    need(not output.is_relative_to(manifest.parent) and not manifest.parent.is_relative_to(output) and
         not binary.is_relative_to(output), 'Output overlaps immutable inputs.')
    workload = read_json(manifest)
    need(workload['format'] == 'poima.performance-yard' and workload['version'] == 1, 'Unexpected retained manifest.')
    character = workload['character']
    need(character['positive_weight_vertices_per_rig'] == character['vertices_per_rig'] == 1029 and
         character['triangles_per_rig'] == 1604 and character['idle_clip'] != character['run_clip'],
         'Licensed character profile differs; review a new recipe instead of silently changing it.')
    inputs = dict(manifest_sha256=sha(manifest), world_sha256=sha(source_world), assets=inventory(source_store))
    need(inputs['world_sha256'] == workload['prepared_world_sha256'] and inputs['assets'] == workload['assets'],
         'Retained manifest/content closure no longer matches.')
    sources = [Path(__file__), ROOT/'tests/scene_capture.py', ROOT/'tools/python/poima_client/core_responses.v1.json']
    sources += sorted((ROOT/'tools/python/poima_client').glob('*.py'))
    sources += sorted((ROOT/'shaders').glob('*.hlsl')) + sorted((ROOT/'shaders').glob('*.hlsli'))
    sources += [ROOT/name for name in ('src/world.cpp', 'src/world_observe.inc', 'src/runtime.cpp',
        'src/runtime_animation.cpp', 'src/runtime_geometry.cpp', 'src/render_smoke.cpp', 'src/shadows.cpp', 'src/scene.cpp')]
    pins = {path.relative_to(ROOT).as_posix(): sha(path) for path in sources}
    dependencies = {path.name: sha(path) for path in (binary.parent/'phonon.dll', binary.parent/'runtime.json')}
    probes = floor_probes()
    record = dict(format=FORMAT, passed=False, primary_capture_count=33, gpu_index=args.gpu,
        shadow_filter=args.shadow_filter,
        binary_sha256=sha(binary), binary_dependencies=dependencies, source_pins=pins, input_inventory=inputs,
        recipe=dict(width=WIDTH, height=HEIGHT, camera_position=CAMERA_POSITION, camera_rotation=CAMERA_ROTATION,
                    camera_target=CAMERA_TARGET, lens=dict(vertical_fov=FOV, near=NEAR, far=FAR),
                    floor=dict(position=[0, -.1, 0], scale=[12, .2, 12]), character=character,
                    floor_probes=probes, image_change_minimum=2, hdr_darkening_minimum=.001),
        native_owners=[], captures=[], variants=[], comparisons=[],
        scope='Controlled stylized licensed humanoid direct-shadow/AO diagnostic; no quality ranking, physical ground truth or performance claim.')
    output.mkdir(parents=True)
    (output/'captures').mkdir()
    fixture = output/'fixture'
    fixture.mkdir()
    world = fixture/'world.json'
    owner, failure = None, None
    try:
        shutil.copytree(source_store, Path(str(world)+'.assets'))
        need(inventory(Path(str(world)+'.assets')) == inputs['assets'], 'Copied content closure differs.')
        owner = Owner(binary, world, record)
        owner.revision = owner.rpc('world.inspect', {})['revision']
        need(owner.revision == 0, 'Fresh world did not begin at revision zero.')
        discovery = owner.rpc('world.describe', dict(view='method', name='runtime.capture'))
        props = discovery['methods']['runtime.capture']['properties']
        need('ambient_occlusion' in props and 'scene_product_probes' in props and
             'color' in props['scene_debug_view']['enum'], 'Required existing capture API unavailable.')
        asset = character['asset']
        summary = owner.rpc('asset.inspect', dict(asset=asset))
        need(summary['vertices'] == 1029 and summary['triangles'] == 1604 and summary['skins'] > 0,
             'Cooked model profile differs.')
        nodes, clips = page(owner, asset, 'nodes'), page(owner, asset, 'animations')
        record['asset_inspection'] = dict(summary=summary, nodes=nodes, clips=clips,
                                        materials=page(owner, asset, 'primitives'))
        need(0 < len(nodes) <= 128 and [row['index'] for row in nodes] == list(range(len(nodes))),
             'Unexpected bounded rig topology.')
        ids = [CAMERA, LIGHT, ENV, FLOOR, RIG] + [hashlib.sha256(
            f'poima.instance.v1/{RIG}/node/{row["index"]}'.encode()).hexdigest()[:32] for row in nodes]
        ids += [hashlib.sha256(f'poima.instance.v1/{RIG}/node/{row["index"]}/primitive/{slot}'.encode()).hexdigest()[:32]
                for row in nodes for slot, _ in enumerate(row['primitives'])]
        declaration = character['declarations'][asset]
        records = [entry['record'] for entry in declaration]
        source_bindings = read_json(source_world)['asset_provenance'][asset]
        need(records == source_bindings and len(records) > 0, 'Retained character provenance declarations differ.')
        for entry in declaration:
            observed = owner.rpc('asset.provenance.inspect', dict(record=entry['record']))
            need(observed['metadata'] == entry['metadata'] and observed['metadata']['license']['identifier'] == 'CC0-1.0',
                 'Retained original CC0 declaration differs.')
        ops = [dict(op='entity.create', id=identifier, name=name, parent=None)
               for identifier, name in ((CAMERA, 'Fixed observer'), (LIGHT, 'Fixed sun'), (ENV, 'Ambient fill'),
                                        (FLOOR, 'Matte receiver'))]
        ops += [transform(CAMERA, CAMERA_POSITION, CAMERA_ROTATION),
                component(CAMERA, 'Camera', dict(vertical_fov=FOV, near=NEAR, far=FAR)),
                transform(LIGHT, rotation=[-.5, 0, 0, math.sqrt(.75)]),
                component(LIGHT, 'Light', light_value(False, .0001, .005)),
                component(ENV, 'LightingEnvironment', dict(ambient=[.03, .03, .03], exposure=1, shadow_resolution=256)),
                transform(FLOOR, [0, -.1, 0], scale=[12, .2, 12]),
                component(FLOOR, 'MeshRenderer', dict(primitive='box', albedo=[.45, .45, .45], visible=True)),
                component(FLOOR, 'PbrMaterial', dict(base_color=[.45, .45, .45], emissive=[0, 0, 0],
                                                    metallic=0, roughness=.85, double_sided=False)),
                dict(op='asset.instantiate', id=RIG, name='Licensed Kenney character', asset=asset, parent=None),
                component(RIG, 'Transform', character['attachment']),
                dict(op='asset.provenance.set', asset=asset, records=records)]
        owner.transact(ops)
        record['authored_provenance'] = owner.rpc('world.asset.provenance', dict(revision=owner.revision))
        idle = character['idle_clip']
        run = character['run_clip']
        durations = {clip['index']: clip['duration'] for clip in clips}
        need(abs(durations[idle]-character['idle_duration']) <= 1e-9 and durations[run] > 0,
             'Retained animation durations differ.')
        baseline_pose = None
        off_images = {}
        def run_variant(pose_name, clip, clip_time, resolution, shadow, bias=.0001, normal_bias=.005, ao_modes=(False,)):
            name, state = variant(owner, character, pose_name, clip, clip_time, resolution,
                                  shadow, bias, normal_bias, ids, fixture)
            record['variants'].append(dict(name=name, pose=pose_name, clip=clip, time=clip_time,
                resolution=resolution, shadow=shadow, bias=bias, normal_bias=normal_bias, state=state))
            results = [capture(owner, name, state, ids, probes, ao, output, record) for ao in ao_modes]
            return state, results
        for resolution in RESOLUTIONS:
            pairs = {}
            for shadow in (False, True):
                state, rows = run_variant('idle-zero', idle, 0, resolution, shadow, ao_modes=(False, True))
                node_pose = {row['id']: row['state']['world_matrix'] for row in state['entities'] if row['id'] not in
                             (CAMERA, LIGHT, ENV, FLOOR, RIG)}
                if baseline_pose is None:
                    baseline_pose = node_pose
                need(node_pose == baseline_pose, 'Shadow/AO/resolution variants changed actual bone pose.')
                for ao, row in zip((False, True), rows):
                    pairs[(shadow, ao)] = row
                    if not shadow:
                        if ao in off_images:
                            need(pixels(output/row['image']) == pixels(output/off_images[ao]['image']),
                                 'Disabled shadow image changed with shadow resolution.')
                        else:
                            off_images[ao] = row
            for ao in (False, True):
                record['comparisons'].append(floor_difference(pairs[(False, ao)], pairs[(True, ao)], probes, output))
        for bias in BIAS:
            for normal_bias in NORMAL_BIAS:
                run_variant('bias', idle, 0, 1024, True, bias=bias, normal_bias=normal_bias)
        for pose_name, clip, clip_time in (('idle-half', idle, durations[idle]/2), ('run-quarter', run, durations[run]/4)):
            for resolution in (512, 1024):
                pair = []
                previous_pose = None
                for shadow in (False, True):
                    state, rows = run_variant(pose_name, clip, clip_time, resolution, shadow)
                    node_pose = {row['id']: row['state']['world_matrix'] for row in state['entities'] if row['id'] in baseline_pose}
                    delta = max(abs(a-b) for identifier in baseline_pose for a, b in
                                zip(node_pose[identifier], baseline_pose[identifier]))
                    need(delta > 1e-4, 'Additional selected clip pose did not produce actual changed bones.')
                    if previous_pose is not None:
                        need(node_pose == previous_pose, 'Paired enabled/disabled shadows changed actual alternate pose.')
                    previous_pose = node_pose
                    record['variants'][-1]['maximum_bone_matrix_change_from_idle_zero'] = delta
                    pair.append(rows[0])
                record['comparisons'].append(floor_difference(pair[0], pair[1], probes, output))
        need(len(record['captures']) == 33 and len(record['comparisons']) == 12, 'Declared primary capture count differs.')
        owner.close()
        owner = None
        record['passed'] = True
    except BaseException as error:
        failure = error
        record['error'] = repr(error)
        record['traceback'] = traceback.format_exc()
    finally:
        if owner is not None:
            try:
                owner.close()
            except BaseException as error:
                record['cleanup_error'] = repr(error)
                failure = failure or error
        try:
            need(inputs == dict(manifest_sha256=sha(manifest), world_sha256=sha(source_world), assets=inventory(source_store)),
                 'Original retained inputs changed.')
            need(pins == {name: sha(ROOT/name) for name in pins}, 'Probe/source inputs changed.')
            need(record['binary_sha256'] == sha(binary) and dependencies == {
                name: sha(binary.parent/name) for name in dependencies}, 'Native dependency bytes changed.')
            need(inventory(Path(str(world)+'.assets')) == inputs['assets'], 'Copied immutable cooked store changed.')
            record['inputs_unchanged'] = True
            record['final_world_sha256'] = sha(world) if world.exists() else None
        except BaseException as error:
            record['integrity_error'] = repr(error)
            failure = failure or error
        if failure is not None:
            record['passed'] = False
        save_json(output/'evidence.json', record)
    if failure is not None:
        raise failure
    print(json.dumps(dict(passed=True, primary_captures=33, floor_comparisons=12, device=record['device'])))


if __name__ == '__main__':
    main()
