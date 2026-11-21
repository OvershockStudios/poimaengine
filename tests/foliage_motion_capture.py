#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify authored blade bending, shadow geometry and save continuation on Vulkan."""

import argparse
import json
import math
from pathlib import Path
import struct
import tempfile
import traceback

from gltf_fixture import glb
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from world_particles_contract import ParticleContract, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, polygon_edges, projection, success, tree_hashes, uid


WIND, TOUCH, ACTOR, FLOOR, PRIMARY, SPECTATOR = map(uid, range(8300, 8306))
ORIGINS = {WIND: (-2.2, 0, 0), TOUCH: (1.9, 0, 0)}
EYES = {PRIMARY: (0, 3.15, 7), SPECTATOR: (-1.05, 2.8, 6.4)}
ACTOR_START = (-.4, .02, -.22)
ACTOR_HEIGHT = 1.8
PITCH = math.radians(-20)
LENS = dict(vertical_fov=55, near=.35, far=40)
LIGHT_TRAVEL = (.88, -1, -.72)
ROOT, TIP = .40, 1.9
RINGS = ((.06, .26), (.24, .26), (.80, .235), (1.35, .20), (TIP, .17))
WIND_STRENGTH, WIND_PERIOD, WIND_LENGTH = .72, 48, 8.8
TOUCH_STRENGTH, TOUCH_RADIUS = .88, 2
EMISSION = [.018, .84, .032]
CHECKPOINT, FINAL = 24, 72
PROFILES = {
    WIND: dict(root_y=ROOT, tip_y=TIP, wind=dict(direction=[1, 0], tip_m=WIND_STRENGTH,
                                              period_ticks=WIND_PERIOD, wavelength_m=WIND_LENGTH)),
    TOUCH: dict(root_y=ROOT, tip_y=TIP, touch=dict(actor=ACTOR, radius_m=TOUCH_RADIUS, tip_m=TOUCH_STRENGTH)),
}


def dot(a, b):
    return sum(x*y for x, y in zip(a, b, strict=True))


def difference(a, b):
    return tuple(x-y for x, y in zip(a, b, strict=True))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def camera_matrix(camera):
    c, s = math.cos(PITCH), math.sin(PITCH)
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, *EYES[camera], 1]


def camera_ray(matrix, pixel):
    width, height = CAPTURE_SIZE
    x, y = pixel
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    local = ((x+.5-width/2)/focal, (height/2-y-.5)/focal, -1)
    return tuple(sum(matrix[4*axis+i]*local[axis] for axis in range(3)) for i in range(3))


def analytic_tip(plant, tick, actor, enabled):
    if not enabled:
        return (0, 0), 0
    if plant == WIND:
        angle = math.tau*(tick/WIND_PERIOD-ORIGINS[WIND][0]/WIND_LENGTH)
        return (WIND_STRENGTH*math.sin(angle), 0), 0
    dx, _, dz = difference(ORIGINS[plant], actor)
    distance = math.hypot(dx, dz)
    if distance >= TOUCH_RADIUS or actor[1] > TIP or actor[1]+ACTOR_HEIGHT < ROOT:
        return (0, 0), 0
    u = 1-distance/TOUCH_RADIUS
    weight = (3-2*u)*u**2
    magnitude = TOUCH_STRENGTH*weight/math.sqrt(distance**2+(TOUCH_RADIUS/8)**2)
    return (dx*magnitude, dz*magnitude), weight


def blade_model():
    vertices = [value for y, half_width in RINGS for x in (-half_width, half_width)
                for value in (x, y, 0, 0, 0, 1)]
    indices = [index+2*band for band in range(len(RINGS)-1) for index in (0, 1, 2, 1, 3, 2)]
    points = struct.pack(f'<{len(vertices)}f', *vertices)
    faces = struct.pack(f'<{len(indices)}I', *indices)
    return glb(dict(asset=dict(version='2.0'), buffers=[dict(byteLength=len(points)+len(faces))],
        bufferViews=[dict(buffer=0, byteOffset=0, byteLength=len(points), byteStride=24, target=34962),
                     dict(buffer=0, byteOffset=len(points), byteLength=len(faces), target=34963)],
        accessors=[dict(bufferView=0, componentType=5126, count=len(vertices)//6, type='VEC3',
                        min=[-RINGS[0][1], RINGS[0][0], 0], max=[RINGS[0][1], TIP, 0]),
                   dict(bufferView=0, byteOffset=12, componentType=5126, count=len(vertices)//6, type='VEC3'),
                   dict(bufferView=1, componentType=5125, count=len(indices), type='SCALAR')],
        meshes=[dict(primitives=[dict(attributes=dict(POSITION=0, NORMAL=1), indices=2)])],
        nodes=[dict(mesh=0)], scenes=[dict(nodes=[0])], scene=0), points+faces)


def blade_bands(plant, tip):
    px, py, pz = ORIGINS[plant]
    rx, rz = tip
    rings = []
    for y, half_width in RINGS:
        t = max(0, min(1, (y-ROOT)/(TIP-ROOT)))
        w = t**2*(3-2*t)
        rings.append(tuple((px+x+w*rx, py+y, pz+w*rz) for x in (-half_width, half_width)))
    return [(a, b, d, c) for (a, b), (c, d) in zip(rings, rings[1:])]


def plane_depth(matrix, ray, points):
    a, b, _, d = points
    normal = cross(difference(b, a), difference(d, a))
    return dot(normal, difference(a, matrix[12:15]))/dot(normal, ray)


def raster_bands(matrix, bands):
    return [(tuple(polygon_edges([projection(matrix, LENS, p) for p in quad])), quad) for quad in bands]


def margin(edges, point):
    x, y = point
    return min(a*x+b*y+c for a, b, c in edges)


def band_center(matrix, points):
    center = [sum(p[i] for p in points)/4 for i in range(3)]
    return tuple(map(math.floor, projection(matrix, LENS, center)))


def shadow_edges(bands):
    dx, dy, dz = LIGHT_TRAVEL
    return [polygon_edges([(x-y*dx/dy, z-y*dz/dy) for x, y, z in quad]) for quad in bands]


def pixel_center(pixel):
    return pixel[0]+.5, pixel[1]+.5


def ground_point(matrix, pixel):
    eye = matrix[12:15]
    ray = camera_ray(matrix, pixel)
    depth = -eye[1]/ray[1]
    return depth, (eye[0]+depth*ray[0], eye[2]+depth*ray[2])


def near_shadow_witness(matrix, source, band, current, alternate, *, displacement=False, lit=False):
    """Find a receiver sample away from all raster and shadow boundaries."""
    caster = source[band]
    dx, dy, dz = LIGHT_TRAVEL
    shadow_quad = [(x-y*dx/dy, 0, z-y*dz/dy) for x, y, z in caster]
    current_edges = shadow_edges(current)
    alternate_edges = shadow_edges(alternate)
    covered = raster_bands(matrix, current+alternate)
    expected_edges = shadow_edges([caster])[0]
    center_x, center_y = band_center(matrix, shadow_quad)
    candidates = []
    for y in range(center_y-14, center_y+15):
        for x in range(center_x-33, center_x+34):
            pixel = (x, y)
            if not (3 < x < CAPTURE_SIZE[0]-3 and 3 < y < CAPTURE_SIZE[1]-3):
                continue
            if any(margin(edges, pixel_center(pixel)) > -2 for edges, _ in covered):
                continue
            depth, point = ground_point(matrix, pixel)
            if not (LENS['near'] < depth < LENS['far'] and all(abs(v) < 12 for v in point)):
                continue
            here = max(margin(edges, point) for edges in current_edges)
            rest = max(margin(edges, point) for edges in alternate_edges)
            inside = margin(expected_edges, point)
            if lit:
                score = min(-here, -rest)
            elif displacement:
                score = min(inside, -rest)
            else:
                score = inside
            if score >= .075:
                candidates.append((score, pixel, depth))
    assert candidates, (source, band, center_x, center_y, displacement, lit)
    score, pixel, depth = max(candidates)
    return dict(pixel=pixel, depth_m=depth, shadow=not lit, shadow_margin_m=score)


def scene_reference(camera, tick, actor, enabled):
    matrix = camera_matrix(camera)
    tips = {plant: analytic_tip(plant, tick, actor, enabled) for plant in ORIGINS}
    blades = {plant: blade_bands(plant, tip) for plant, (tip, _) in tips.items()}
    static = {plant: blade_bands(plant, (0, 0)) for plant in ORIGINS}
    all_bands = blades[WIND]+blades[TOUCH]
    rest_bands = static[WIND]+static[TOUCH]
    samples, outlines = {}, {}
    for name, plant in (('wind', WIND), ('touch', TOUCH)):
        bands = blades[plant]
        projected = outlines[name] = raster_bands(matrix, bands)
        for label, index in (('root', 0), ('tip', -1)):
            edges, band = projected[index]
            pixel = band_center(matrix, band)
            assert margin(edges, pixel_center(pixel)) > 2.7, (name, label, pixel, band)
            samples[f'{name}_{label}'] = dict(pixel=pixel, plant=True, depth_m=plane_depth(
                matrix, camera_ray(matrix, pixel), band))
        moved = math.hypot(*tips[plant][0]) > .24
        samples[f'{name}_shadow'] = near_shadow_witness(matrix, bands, -1, all_bands, rest_bands,
                                                       displacement=moved)
        samples[f'{name}_lit'] = near_shadow_witness(matrix, bands, -1, all_bands, rest_bands, lit=True)
        if moved:
            samples[f'{name}_vacated_shadow'] = near_shadow_witness(matrix, static[plant], -1,
                rest_bands, all_bands, displacement=True) | dict(shadow=False)
    assert len({tuple(p['pixel']) for p in samples.values()}) == len(samples), samples
    return tips, outlines, samples


def validate_reference():
    checkpoints = {0: ACTOR_START, CHECKPOINT: (1.2, 0, ACTOR_START[2]), FINAL: (4.4, 0, ACTOR_START[2])}
    witnesses = {}
    for tick, actor in checkpoints.items():
        for camera in EYES:
            for enabled in (False, True):
                tips, _, samples = scene_reference(camera, tick, actor, enabled)
                witnesses[f'{tick}/{camera}/{enabled}'] = dict(model=tips, probes=samples)
    assert analytic_tip(WIND, 0, ACTOR_START, True)[0][0] > .7
    assert analytic_tip(WIND, CHECKPOINT, ACTOR_START, True)[0][0] < -.7
    assert analytic_tip(TOUCH, CHECKPOINT, checkpoints[CHECKPOINT], True)[0][0] > .48
    assert analytic_tip(TOUCH, FINAL, checkpoints[FINAL], True) == ((0, 0), 0)
    return witnesses


def assert_silhouette(image, matrix, outlines):
    projected = [item for bands in outlines.values() for item in bands]
    width, height = CAPTURE_SIZE
    assert len(image) == height and all(len(row) == width for row in image)
    required, observed = 0, 0
    for y, row in enumerate(image):
        for x, (r, g, b) in enumerate(row):
            green = g > 105 and g > 3*max(r, b)
            if green:
                observed += 1
                assert max(margin(edges, (x+.5, y+.5)) for edges, _ in projected) > -1.15, (x, y, r, g, b)
    for edges, quad in projected:
        xs, ys = zip(*(projection(matrix, LENS, p) for p in quad), strict=True)
        for y in range(math.floor(min(ys)), math.ceil(max(ys))):
            for x in range(math.floor(min(xs)), math.ceil(max(xs))):
                if margin(edges, (x+.5, y+.5)) > 1.15:
                    required += 1
                    r, g, b = image[y][x]
                    assert g > 105 and g > 3*max(r, b), (x, y, r, g, b)
    assert observed >= required > 1150, (required, observed)
    return dict(required_green_pixels=required, green_pixels=observed)


class FoliageCapture(ParticleContract):
    def transact(self, revision, request, ops):
        reply = self.world_call('world.transact', request_id=uid(request), base_revision=revision, ops=ops)
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def mesh(self, foliage=None):
        value = dict(asset=self.asset, primitive=0, visible=True, cast_shadows=True, receive_shadows=False)
        if foliage is not None:
            value['foliage_motion'] = foliage
        return value

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Foliage Motion')))
        path = self.project/'analytic blade.glb'
        path.write_bytes(blade_model())
        source = self.world_call('asset.source.inspect', source=self.native(path))
        imported = self.world_call('asset.import', source=self.native(path), expected_model_sha256=source['model_sha256'])
        self.asset = imported['asset']
        assert file_hash(self.assets/(self.asset+'.pmodel')) == self.asset, imported
        self.measurements['blade'] = dict(glb=file_hash(path), asset=self.asset)
        aim = (LIGHT_TRAVEL[1], -LIGHT_TRAVEL[0], 0, math.hypot(*LIGHT_TRAVEL)-LIGHT_TRAVEL[2])
        rotation = [v/math.hypot(*aim) for v in aim]
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(1), type='BoxCollider'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[.02]*3, exposure=1,
                   shadow_resolution=2048, shadow_filter='pcf3_point')),
               component(uid(5), 'Transform', transform([0, 6, 0], rotation)),
               component(uid(5), 'Light', dict(kind='directional', color=[1]*3, intensity=3, enabled=True,
                   shadow=dict(enabled=True, near=.05, distance=24, bias=.0002, normal_bias=.001))),
               dict(op='entity.create', id=FLOOR, name='Fixed shadow receiver'),
               component(FLOOR, 'Transform', transform([0, -.15, 0]) | dict(scale=[25, .30, 25])),
               component(FLOOR, 'MeshRenderer', dict(primitive='box', albedo=[.65]*3, visible=True,
                   cast_shadows=False, receive_shadows=True)),
               component(FLOOR, 'PbrMaterial', dict(base_color=[.65]*3, roughness=.82, metallic=0,
                   emissive=[0]*3, double_sided=False)),
               component(FLOOR, 'BoxCollider', dict(motion='static', half_extents=[.5]*3,
                   mass=1, friction=.5, restitution=0)),
               dict(op='entity.create', id=ACTOR, name='Independent touch actor'),
               component(ACTOR, 'Transform', transform(list(ACTOR_START))),
               component(ACTOR, 'CharacterController', dict(radius=.25, height=ACTOR_HEIGHT, speed=4,
                   jump_speed=5, camera=None))]
        for camera, eye in EYES.items():
            ops.extend([dict(op='entity.create', id=camera, name=f'Foliage capture camera {camera[-1]}'),
                        component(camera, 'Transform', transform(list(eye),
                            [math.sin(PITCH/2), 0, 0, math.cos(PITCH/2)])),
                        component(camera, 'Camera', LENS)])
        for plant, origin in ORIGINS.items():
            ops.extend([dict(op='entity.create', id=plant, name='Wind blade' if plant == WIND else 'Touch blade'),
                        component(plant, 'Transform', transform(list(origin))),
                        component(plant, 'StaticMesh', self.mesh()),
                        component(plant, 'PbrMaterial', dict(base_color=[0]*3, roughness=.9, metallic=1,
                            emissive=EMISSION, double_sided=True))])
        return self.transact(self.world_call('world.inspect')['revision'], 8310, ops)

    def bundle(self, label, revision, enabled):
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info.get('needs_foliage_motion', False) is enabled, info
        original, relocated = self.root/(label+' export'), self.root/(label+' relocated')
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(original),
                                   '--runtime', self.native(self.args.runtime_root))))
        original.rename(relocated)
        assert not original.exists() and not list(relocated.rglob('*.glb'))
        model, = relocated.rglob('*.pmodel')
        assert model.stem == file_hash(model) == self.asset
        game = relocated/'game.json'
        executable = relocated/json.loads(game.read_bytes())['engine']['executable']
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert inspected.get('needs_foliage_motion', False) is enabled, inspected
        self.measurements['bundles'][label] = dict(revision=revision, inventory=tree_hashes(relocated))
        return game

    def capture(self, client, session, tick, camera, enabled, label):
        def entity(id_):
            return client.call('runtime.entity', dict(session_id=session, tick=tick, id=id_))

        actor = entity(ACTOR)['world_matrix'][12:15]
        expected_x = ACTOR_START[0]+tick*4/60
        assert abs(actor[0]-expected_x) < .12 and abs(actor[2]-ACTOR_START[2]) < .002, (tick, actor)
        matrix = entity(camera)['world_matrix']
        near_vector(matrix, camera_matrix(camera))
        tips, outlines, samples = scene_reference(camera, tick, actor, enabled)
        inspected = {}
        if enabled:
            for plant, ((dx, dz), weight) in tips.items():
                got = client.call('runtime.foliage.inspect', dict(session_id=session, tick=tick, entity=plant,
                                                                  structure_revision=0))
                assert (got['session_id'], got['entity'], got['tick'], got['structure_revision']) == (session, plant, tick, 0), got
                near_vector(got['tip_local_xz'], (dx, dz))
                near_vector(got['tip_world_offset_m'], (dx, 0, dz))
                assert abs(got['touch_weight']-weight) < 4e-5, got
                inspected[plant] = {key: value for key, value in got.items() if key != 'session_id'}
        output = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=camera, path=self.native(output), width=width, height=height, gpu=self.args.gpu,
            samples=1, lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in (row['pixel'] for row in samples.values())]))
        assert receipt['hardware'] is True and receipt['capture_written'] is True, receipt
        assert receipt['frames_presented'] > 0 and receipt['nvrhi_errors'] == 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == matrix, receipt
        diagnostics = receipt['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        draws = diagnostics['last_draws']
        assert draws['shadow_views'] > 0 and draws['shadow_draws'] >= 2, draws
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        image = pixels(output)
        result = dict(tick=tick, actor=actor, camera=camera, gpu=receipt['gpu'], inspected=inspected,
                      sha256=file_hash(output), probes={})
        result['silhouette'] = assert_silhouette(image, matrix, outlines)
        self.measurements['captures'][label] = result
        for (name, expected), sample in zip(samples.items(), products['probes'], strict=True):
            x, y = expected['pixel']
            result['probes'][name] = row = expected | dict(raster=sample, rgb=image[y][x])
            assert (sample['x'], sample['y']) == (x, y) and sample['surface_valid'] is True, row
            near, far = LENS['near'], LENS['far']
            depth = near/(1-sample['depth']*(far-near)/far)
            assert abs(depth-expected['depth_m']) < .003, row
            if expected.get('plant'):
                near_vector(sample['raw_hdr'], EMISSION, tolerance=.006)
                r, g, b = image[y][x]
                assert g > 105 and g > 3*max(r, b), row
            else:
                near_vector(sample['shading_normal'][:3], (0, 1, 0), tolerance=.002)
        for name, row in result['probes'].items():
            if 'shadow' not in row:
                continue
            lit = result['probes'][name.split('_')[0]+'_lit']['raster']['raw_hdr']
            actual = row['raster']['raw_hdr']
            assert min(lit) > .20, (name, lit)
            if row['shadow']:
                assert max(actual) < .3*min(lit), (name, row, lit)
            else:
                near_vector(actual, lit, tolerance=.045)
        return result

    def save_bytes(self, client, saves, session, tick, slot, request):
        receipt = client.call('save.write', dict(request_id=uid(request), configuration_generation=1,
            slot=slot, expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0,
            expected_structure_revision=0))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert json.loads(raw)['snapshot']['payload']['tick'] == tick, receipt
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        self.measurements['saves'][slot] = dict(tick=tick, receipt=receipt)
        return raw

    def exercise(self, game, revision, label, enabled):
        saves = self.root/(label+' saves')
        saves.mkdir()
        move = [dict(entity=ACTOR, move=[1, 0], look=[0, 0], jump=False, use=False)]
        with self.owner(label+' warm', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, None, saves, game)
            initial = self.capture(client, session, 0, PRIMARY, enabled, label+' initial')
            self.step(client, session, 0, 8320, ticks=CHECKPOINT, inputs=move)
            hit = self.capture(client, session, CHECKPOINT, PRIMARY, enabled, label+' checkpoint')
            other = self.capture(client, session, CHECKPOINT, SPECTATOR, enabled, label+' spectator')
            assert exact(other['inspected'], hit['inspected']) and exact(other['actor'], hit['actor'])
            assert hit['probes']['touch_tip']['pixel'] != other['probes']['touch_tip']['pixel']
            if enabled:
                checkpoint = self.save_bytes(client, saves, session, CHECKPOINT, 'checkpoint', 8321)
            self.step(client, session, CHECKPOINT, 8322, ticks=FINAL-CHECKPOINT, inputs=move)
            final = self.capture(client, session, FINAL, PRIMARY, enabled, label+' final')
            if enabled:
                continuation = self.save_bytes(client, saves, session, FINAL, 'warm-final', 8323)
        if enabled:
            with self.owner(label+' cold', game=game, saves=saves) as (client, cold):
                previous = self.initialize(client, revision, None, saves, game)
                session = uid(8324)
                assert self.load(client, revision, previous, session, 'checkpoint', CHECKPOINT, 8325) == 0
                assert checkpoint == self.save_bytes(client, saves, session, CHECKPOINT, 'cold-checkpoint', 8326)
                assert exact(hit, self.capture(client, session, CHECKPOINT, PRIMARY, enabled, label+' restored'))
                self.step(client, session, CHECKPOINT, 8327, ticks=FINAL-CHECKPOINT, inputs=move)
                assert exact(final, self.capture(client, session, FINAL, PRIMARY, enabled, label+' cold final'))
                assert continuation == self.save_bytes(client, saves, session, FINAL, 'cold-final', 8328)
            assert warm['pid'] != cold['pid'], (warm, cold)
        return initial, hit, final

    def compare(self, static, enabled):
        assert len({starting['sha256'] for starting in static}) == 1, static
        assert len({frame['sha256'] for frame in enabled}) == 3, enabled
        for idle, active in zip(static, enabled, strict=True):
            assert idle['sha256'] != active['sha256'] and exact(idle['actor'], active['actor'])
            for label in ('wind_root', 'touch_root'):
                original, bent = idle['probes'][label], active['probes'][label]
                for key in ('x', 'y', 'surface_valid', 'depth', 'raw_hdr', 'shading_normal'):
                    assert exact(original['raster'][key], bent['raster'][key]), (label, key, original, bent)
                assert exact(original['rgb'], bent['rgb']), (label, original, bent)
        start, hit, final = (result['inspected'] for result in enabled)
        assert start[WIND]['tip_local_xz'][0] > .7 and hit[WIND]['tip_local_xz'][0] < -.7
        assert start[TOUCH]['touch_weight'] == final[TOUCH]['touch_weight'] == 0
        assert hit[TOUCH]['touch_weight'] > .58 and hit[TOUCH]['tip_local_xz'][0] > .48, hit
        assert exact(hit[WIND]['tip_local_xz'], final[WIND]['tip_local_xz'])
        self.measurements['checks'] = dict(unchanged_static_bitmap=True, static_root_geometry=True,
            live_touch_outside_after_pass=True, cold_checkpoint_bitmap_and_save_bytes=True,
            cold_continuation_bitmap_and_save_bytes=True, stationary_spectator_independent_touch=True)

    def run(self):
        self.measurements.update(oracle=validate_reference(), bundles={}, captures={}, saves={})
        base = self.author()
        static = self.bundle('omitted', base, False)
        active = self.transact(base, 8311, [component(plant, 'StaticMesh', self.mesh(profile))
                                           for plant, profile in PROFILES.items()])
        game = self.bundle('enabled', active, True)
        source = self.root/'Detached Foliage Source'
        self.project.rename(source)
        fingerprint = tree_hashes(source)
        assert not self.project.exists()
        self.compare(self.exercise(static, base, 'omitted', False), self.exercise(game, active, 'enabled', True))
        for label, path in (('omitted', static), ('enabled', game)):
            assert tree_hashes(path.parent) == self.measurements['bundles'][label]['inventory']
        assert not self.project.exists() and tree_hashes(source) == fingerprint
        self.measurements['detached_source'] = fingerprint
        for call in self.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')), stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Run with assertions enabled and a GPU index between 0 and 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='foliage-motion-', dir=args.output.resolve()))
    test = FoliageCapture(args, root)
    record = dict(format='poima.foliage-motion-capture.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, binary_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
