#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify terrain-bound ripple normals and physical invariance in Windows Vulkan."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid
from world_water_contract import (
    CELLS, CHECKPOINT, FINAL, GATE, OPENED, SILL, TILES, WaterContract, bed, component, reference,
)


ORIGIN_XZ = (10, -4)
RIPPLES = dict(wavelength_m=.62, slope=.30, period_ticks=12, heading_degrees=37)
PROFILES = {'legacy': None, 'zero': RIPPLES | dict(slope=0), 'enabled': RIPPLES}
DRY = uid(8102)
DRY_POSITION, DRY_SCALE = (8.15, 6.15, -3), (.8, .35, .65)
NEAR = uid(8099)
FAR = uid(8100)
MOVED = uid(8101)
LENS = dict(vertical_fov=55, near=.5, far=100)
VIEWS = {
    NEAR: ('near', [12, 11.5, 1.6], -47),
    FAR: ('far', [12, 62, -3], -90),
    MOVED: ('moved', [12.6, 11.3, 1.2], -51),
}
WATER_SITES = (
    ('west_front', (0, 0), .28, .61), ('west_back', (0, 1), .73, .53),
    ('sluice', (1, 0), .61, .31), ('east', (2, 0), .72, .28),
)
NORMAL_TOLERANCE = .0025
VALIDATION_LAYER = 'Insert instance layer "VK_LAYER_KHRONOS_validation"'


def dot(a, b):
    return sum(x*y for x, y in zip(a, b, strict=True))


def unit(v):
    length = math.hypot(*v)
    return tuple(x/length for x in v)


def camera_matrix(camera):
    _, eye, degrees = VIEWS[camera]
    theta = math.radians(degrees)
    c, s = math.cos(theta), math.sin(theta)
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, *eye, 1]


def water_heights(physical):
    return {cell: (bed(10.5+cell[0], -3.5+cell[1])+depth)
            for cell, depth in zip(CELLS, physical['depths'], strict=True) if depth > 0}


def probes_for(camera, physical):
    matrix = camera_matrix(camera)
    heights = water_heights(physical)
    result = {}
    for label, cell, x, z in WATER_SITES:
        if cell in heights:
            target = (ORIGIN_XZ[0]+cell[0]+x, heights[cell], ORIGIN_XZ[1]+cell[1]+z)
            result[label] = (cell, tuple(map(math.floor, projection(matrix, LENS, target))))
    dry_top = (DRY_POSITION[0], DRY_POSITION[1]+DRY_SCALE[1]/2, DRY_POSITION[2])
    result['dry'] = (None, tuple(map(math.floor, projection(matrix, LENS, dry_top))))
    assert len({p for _, p in result.values()}) == len(result)
    assert all(1 <= x < CAPTURE_SIZE[0]-1 and 1 <= y < CAPTURE_SIZE[1]-1
               for _, (x, y) in result.values())
    return result


def plane_point(camera, p, plane_y, jitter=(0, 0)):
    matrix = camera_matrix(camera)
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    u = (p[0]+.5-jitter[0]-width/2)/focal
    v = (height/2-p[1]-.5+jitter[1])/focal
    ray = [u*matrix[i]+v*matrix[4+i]-matrix[8+i] for i in range(3)]
    eye = matrix[12:15]
    assert ray[1] < 0
    depth = (plane_y-eye[1])/ray[1]
    return tuple(a+b*depth for a, b in zip(eye, ray, strict=True)), depth


def inside_cell(point, cell):
    x, z = ORIGIN_XZ[0]+cell[0], ORIGIN_XZ[1]+cell[1]
    return x < point[0] < x+1 and z < point[2] < z+1


def smooth_fade(gradient):
    t = max(0, min(1, (gradient-.5)/2))
    return 1-t*t*(3-2*t)


def ripple_normal(profile, camera, pixel, y, tick, jitter, *, filtered=True, local=True):
    if profile is None or profile['slope'] == 0:
        return (0., 1., 0.), [1., 1.]
    p, _ = plane_point(camera, pixel, y, jitter)
    xz = (p[0]-ORIGIN_XZ[0], p[2]-ORIGIN_XZ[1]) if local else (p[0], p[2])
    # Resolve derivatives from the same aligned 2x2 raster quad. The far
    # view looks straight down and the water plane is affine in raster XY.
    x, z = pixel
    x0, z0 = x-x%2, z-z%2
    a, _ = plane_point(camera, (x0, z), y, jitter)
    b, _ = plane_point(camera, (x0+1, z), y, jitter)
    c, _ = plane_point(camera, (x, z0), y, jitter)
    d, _ = plane_point(camera, (x, z0+1), y, jitter)
    dx, dy = (b[0]-a[0], b[2]-a[2]), (d[0]-c[0], d[2]-c[2])
    theta = math.radians(profile['heading_degrees'])
    along, across = (math.cos(theta), math.sin(theta)), (-math.sin(theta), math.cos(theta))
    k = math.tau/profile['wavelength_m']
    phase = math.tau*(tick % profile['period_ticks'])/profile['period_ticks']
    slope = [0., 0.]
    fades = []
    for axis, weight, frequency, phase_multiplier in ((along, .65, k, 1), (across, .35, 1.6*k, 2)):
        attenuation = smooth_fade(frequency*max(abs(dot(dx, axis)), abs(dot(dy, axis)))) if filtered else 1.
        fades.append(attenuation)
        amount = profile['slope']*weight*attenuation*math.cos(frequency*dot(xz, axis)-phase_multiplier*phase)
        for i in range(2):
            slope[i] += amount*axis[i]
    return unit((-slope[0], 1, -slope[1])), fades


def probe_reference(camera, physical, cell, pixel, jitter):
    heights = water_heights(physical)
    y = heights[cell] if cell is not None else DRY_POSITION[1]+DRY_SCALE[1]/2
    point, distance = plane_point(camera, pixel, y, jitter)
    if cell is not None:
        assert inside_cell(point, cell), (cell, point)
        # The shallow newly wet cell remains partly occluded by its sloping
        # bed. Qualify the exposed water, with margin for reconstruction jitter.
        assert point[1]-bed(point[0], point[2]) > .015, (cell, point)
    else:
        assert abs(point[0]-DRY_POSITION[0]) < DRY_SCALE[0]/2 and abs(point[2]-DRY_POSITION[2]) < DRY_SCALE[2]/2, point
    for other, height in heights.items():
        intersection, depth = plane_point(camera, pixel, height, jitter)
        assert other == cell or not inside_cell(intersection, other) or depth > distance, (camera, cell, other, point)
    return point, distance


def verify_reference():
    """Keep assertions about coverage and alternative wave models executable off-GPU."""
    states = reference()
    differences = []
    frozen_gaps = []
    for camera, tick in ((NEAR, 0), (NEAR, CHECKPOINT), (NEAR, FINAL), (FAR, CHECKPOINT), (MOVED, CHECKPOINT)):
        sites = probes_for(camera, states[tick])
        for jitter in ((0, 0), (-.5, -.5), (.5, -.5), (-.5, .5), (.5, .5)):
            for cell, pixel in sites.values():
                p, _ = probe_reference(camera, states[tick], cell, pixel, jitter)
                if cell is None:
                    continue
                n, fades = ripple_normal(RIPPLES, camera, pixel, p[1], tick, jitter)
                if camera == FAR:
                    assert all(.02 < f < .9 for f in fades), fades
                else:
                    assert fades == [1, 1], fades
                if jitter == (0, 0):
                    variant, _ = ripple_normal(RIPPLES, camera, pixel, p[1], tick, jitter, local=False)
                    unfiltered, _ = ripple_normal(RIPPLES, camera, pixel, p[1], tick, jitter, filtered=False)
                    differences.append((camera, math.dist(n, variant), math.dist(n, unfiltered)))
                    if camera == NEAR and tick == CHECKPOINT:
                        frozen, _ = ripple_normal(RIPPLES, camera, pixel, p[1], 0, jitter)
                        frozen_gaps.append(math.dist(n, frozen))
    assert max(local_gap for _, local_gap, _ in differences) > .12
    assert max(fade_gap for camera, _, fade_gap in differences if camera == FAR) > .08
    assert max(frozen_gaps) > .10


def physical(report):
    return {key: value for key, value in report.items() if key not in ('session_id', 'plan_sha256', 'surface_ripples')}


class RippleCapture(WaterContract, ParticleContract):
    def author(self):
        recipe, _, revision = self.arm_terrain()
        self.recipe = recipe
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            component(uid(4), 'LightingEnvironment', dict(ambient=[.20, .22, .26], exposure=1)),
            component(uid(5), 'Transform', transform([0, 0, 0],
                [math.sin(math.radians(-62)/2), 0, 0, math.cos(math.radians(-62)/2)])),
            component(uid(5), 'Light', dict(kind='directional', color=[1, .96, .92], intensity=2, enabled=True)),
            dict(op='entity.create', id=DRY, name='Dry bank control'),
            component(DRY, 'Transform', transform(list(DRY_POSITION)) | dict(scale=list(DRY_SCALE))),
            component(DRY, 'MeshRenderer', dict(primitive='box', albedo=[.14, .24, .16],
                                                cast_shadows=False, receive_shadows=False, visible=True)),
            component(DRY, 'PbrMaterial', dict(base_color=[.14, .24, .16], emissive=[0, 0, 0],
                                                metallic=0, roughness=.8, double_sided=False)),
            component(DRY, 'BoxCollider', dict(motion='static', half_extents=[.5]*3, mass=1,
                                                friction=.5, restitution=0)),
        ]
        for camera, (label, eye, angle) in VIEWS.items():
            ops.append(dict(op='entity.create', id=camera, name=f'{label.title()} ripple camera'))
            pitch = math.radians(angle)/2
            ops.extend((component(camera, 'Transform', transform(eye, [math.sin(pitch), 0, 0, math.cos(pitch)])),
                        component(camera, 'Camera', LENS)))
        result = success(self.world_calls(self.transaction(revision, 8110, *ops))[0])
        assert result['committed'] and result['revision'] == revision+1, result
        return result['revision']

    def bundle(self, label, profile, revision, request):
        params = dict(revision=revision, terrain={'source': self.recipe.name},
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'], tile_entities=TILES,
            grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4, relaxation_per_second=6),
            initial_depths_m=self.oracle[0]['depths'],
            faces=[dict(cell=[1, 0], axis='x', openness=0, sill_height_m=SILL, action=GATE, action_openness=.5),
                   dict(cell=[1, 1], axis='x', openness=0, sill_height_m=SILL)],
            exchanges=[dict(cell=[0, 0], source_m3_per_second=.6, drain_m3_per_second=0),
                       dict(cell=[3, 0], source_m3_per_second=0, drain_m3_per_second=.24)])
        if profile is not None:
            params['surface_ripples'] = profile
        plan = self.world_call('world.water.plan', **params)
        pin = plan['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=pin) == plan
        artifact = self.assets/(pin+'.pwater')
        assert file_hash(artifact) == pin
        decoded = json.loads(artifact.read_bytes())
        assert decoded.get('surface_ripples') == profile and decoded['version'] == (1 if profile is None else 3), decoded
        updated = success(self.world_calls(self.transaction(revision, request, dict(op='water.set', asset=pin)))[0])
        assert updated['committed'] and updated['revision'] == revision+1, updated
        revision += 1
        output, relocated = self.root/(label+' original export'), self.root/(label+' relocated game')
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        relocated_assets = sorted(path.name for path in output.rglob('*') if path.suffix in ('.pmodel', '.pwater'))
        assert relocated_assets == sorted([pin+'.pwater', *(tile['asset']+'.pmodel' for tile in self.terrain['tiles'])])
        assert all(file_hash(path) == path.stem for path in output.rglob('*') if path.suffix in ('.pmodel', '.pwater'))
        assert not list(output.rglob(self.recipe.name))
        output.rename(relocated)
        assert not output.exists()
        self.measurements['bundles'][label] = dict(revision=revision, plan=plan, content=tree_hashes(relocated))
        return revision, relocated/'game.json'

    @staticmethod
    def call(client, rpc):
        return client.call(rpc['method'], rpc['params'])

    def initial(self, client, revision):
        status = client.call('runtime.status')
        assert status['active'] is True and status['tick'] == 0 and status['authored_revision'] == revision, status
        return status['session_id']

    def verify_scene_state(self, client, session, tick, profile):
        wet = self.call(client, self.state(session, tick))
        self.verify_state(wet, tick)
        assert wet.get('surface_ripples') == profile, wet
        rays = {}
        for cell in ((0, 0), (2, 0)):
            x, z = 10.41+cell[0], -3.66+cell[1]
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick,
                origin=[x, 20, z], direction=[0, -1, 0], distance=30))['hit']
            assert hit['entity'] == TILES[cell[0]//2] and type(hit['triangle']) is int, hit
            near_vector(hit['position'], [x, bed(x, z), z], tolerance=3e-5)
            near_vector(hit['normal'], unit((.25, 1, -.125)), tolerance=.002)
            rays[str(cell)] = hit
        return dict(water=physical(wet), bed=rays)

    def capture(self, client, session, tick, camera, variant, label):
        profile = PROFILES[variant]
        observations = self.verify_scene_state(client, session, tick, profile)
        where = self.oracle[tick]
        sites = probes_for(camera, where)
        matrix = client.call('runtime.entity', dict(session_id=session, tick=tick, id=camera))['world_matrix']
        near_vector(matrix, camera_matrix(camera), tolerance=1e-8)
        name = f'{variant}-{label}-{VIEWS[camera][0]}'
        output = self.root/(name+'.bmp')
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=camera, path=self.native(output), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
            reconstruction=self.args.reconstruction, capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for _, (x, y) in sites.values()]))
        self.measurements['reports'][name] = report
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] == 2, report
        assert report['tick'] == tick and report['lens'] == LENS and report['camera_world'] == matrix, report
        assert report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        receipts = diagnostics['scene_receipts']
        assert receipts['presented'] and receipts['submitted'] == receipts['presented'], receipts
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        reconstruction = diagnostics['reconstruction']
        temporal = self.args.reconstruction != 'none'
        assert reconstruction['active'] is temporal, reconstruction
        jitter = reconstruction['jitter_pixels'] if temporal else (0, 0)
        if temporal:
            assert (reconstruction['render_width'], reconstruction['render_height']) == CAPTURE_SIZE, reconstruction
            assert reconstruction['history_sequence'] == 2 and reconstruction['history_reset'] is False, reconstruction
        assert len(jitter) == 2 and all(abs(value) <= .5 for value in jitter), jitter
        image = pixels(output)
        assert len(image) == height and all(len(row) == width for row in image)
        record = dict(tick=tick, camera=camera, gpu=report['gpu'], sha256=file_hash(output),
                      jitter=jitter, observations=observations, probes={})
        self.measurements['captures'][name] = record
        near, far = LENS['near'], LENS['far']
        for (tag, (cell, pixel)), probe in zip(sites.items(), products['probes'], strict=True):
            x, y = pixel
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            point, camera_depth = probe_reference(camera, where, cell, pixel, jitter)
            sample, fades = ripple_normal(profile if cell is not None else None, camera, pixel, point[1], tick, jitter)
            depth_m = near/(1-probe['depth']*(far-near)/far)
            depth_error = abs(camera_depth-depth_m)
            item = record['probes'][tag] = dict(cell=cell, point=point, expected_normal=sample, fades=fades,
                                              depth_error_m=depth_error, rgb=image[y][x], raster=probe)
            assert probe['surface_valid'] is True and depth_error < .002, item
            near_vector(probe['shading_normal'][:3], sample, tolerance=NORMAL_TOLERANCE)
            if cell is not None:
                assert probe['motion_valid'] is False, item
            if not temporal:
                assert exact(probe['raw_hdr'], probe['resolved_hdr']), item
        return record

    def save_bytes(self, client, saves, session, tick, slot, request):
        receipt = self.call(client, self.write(session, tick, slot, request))
        path, = (saves/('slot-'+slot)).glob('p-*.bin')
        assert receipt['sha256'] == file_hash(path), receipt
        raw = path.read_bytes()
        archive = json.loads(raw)['snapshot']['payload']
        assert archive['tick'] == archive['water']['tick'] == tick, archive
        assert archive['water']['plan_sha256'] == self.plan['plan_sha256'], archive
        water = {key: value for key, value in archive['water'].items() if key != 'plan_sha256'}
        self.measurements['saves'][saves.name+'/'+slot] = dict(tick=tick, sha256=receipt['sha256'], water=water)
        return raw

    def finish(self, client, session, request):
        for start, end, pressed in ((CHECKPOINT, OPENED, False), (OPENED, FINAL, True)):
            stepped = self.call(client, self.step(session, start, end-start, request, pressed=pressed))
            assert stepped['current_tick'] == end and stepped['stepped'] == end-start, stepped
            request += 1

    def exercise(self, variant, revision, game):
        self.plan = self.measurements['bundles'][variant]['plan']
        saves = self.root/(variant+' saves')
        saves.mkdir()
        with self.owner(variant+' warm', game=game, saves=saves) as (client, warm):
            session = self.initial(client, revision)
            initial = self.capture(client, session, 0, NEAR, variant, 'initial')
            stepped = self.call(client, self.step(session, 0, CHECKPOINT-1, 8150, pressed=False))
            assert stepped['current_tick'] == CHECKPOINT-1, stepped
            stepped = self.call(client, self.step(session, CHECKPOINT-1, 1, 8151, pressed=True))
            assert stepped['current_tick'] == CHECKPOINT, stepped
            checkpoint = self.save_bytes(client, saves, session, CHECKPOINT, 'checkpoint', 8152)
            frames = [self.capture(client, session, CHECKPOINT, camera, variant, 'checkpoint') for camera in VIEWS]
            self.finish(client, session, 8153)
            final = self.capture(client, session, FINAL, NEAR, variant, 'final')
            warmed = self.save_bytes(client, saves, session, FINAL, 'final', 8155)
        if variant == 'enabled':
            with self.owner(variant+' cold', game=game, saves=saves) as (client, cold):
                active = self.initial(client, revision)
                session = uid(8160)
                loaded = self.call(client, self.load(session, revision, 8161, active=active, active_tick=0))
                assert loaded['tick'] == CHECKPOINT and loaded['session_id'] == session, loaded
                assert loaded['source_stale'] is False, loaded
                assert checkpoint == self.save_bytes(client, saves, session, CHECKPOINT, 'cold-checkpoint', 8162)
                assert exact(frames, [self.capture(client, session, CHECKPOINT, camera, variant, 'restored')
                                     for camera in VIEWS])
                self.finish(client, session, 8163)
                assert exact(final, self.capture(client, session, FINAL, NEAR, variant, 'cold-final'))
                assert warmed == self.save_bytes(client, saves, session, FINAL, 'cold-final', 8165)
            assert warm['pid'] != cold['pid'], (warm, cold)
        return [initial, *frames, final]

    def compare(self, captures):
        legacy, zero, enabled = (captures[key] for key in PROFILES)
        assert exact(legacy, zero), 'An explicit zero slope changed the legacy render or physical state.'
        altered_hdr, opposed_normals, fade_gap, origin_gap = [], [], [], []
        for dry, rippled in zip(legacy, enabled, strict=True):
            assert exact(dry['observations'], rippled['observations']), (dry['tick'], dry['camera'])
            assert rippled['sha256'] != dry['sha256'], (dry['tick'], dry['camera'])
            for name, previous in dry['probes'].items():
                active = rippled['probes'][name]
                assert previous['point'] == active['point'], (name, previous, active)
                if name == 'dry':
                    assert exact(previous, active), (name, previous, active)
                    continue
                base, observed = previous['raster'], active['raster']
                for key in ('depth', 'surface_valid', 'motion_valid', 'motion'):
                    assert exact(base[key], observed[key]), (name, key, base, observed)
                altered_hdr.append(math.dist(base['raw_hdr'], observed['raw_hdr']))
                x, y = observed['x'], observed['y']
                arguments = (RIPPLES, rippled['camera'], (x, y), active['point'][1], rippled['tick'], rippled['jitter'])
                wrong_origin, _ = ripple_normal(*arguments, local=False)
                origin_gap.append(math.dist(observed['shading_normal'][:3], wrong_origin))
                if rippled['camera'] == FAR:
                    unfiltered, _ = ripple_normal(*arguments, filtered=False)
                    fade_gap.append(math.dist(observed['shading_normal'][:3], unfiltered))
                if rippled['tick'] == CHECKPOINT and rippled['camera'] == NEAR:
                    static_phase = arguments[:4] + (0,) + arguments[5:]
                    frozen, _ = ripple_normal(*static_phase)
                    opposed_normals.append(math.dist(observed['shading_normal'][:3], frozen))
        assert max(altered_hdr) > .02, altered_hdr
        assert max(opposed_normals) > .10, opposed_normals
        assert max(origin_gap) > .12, origin_gap
        assert max(fade_gap) > .08, fade_gap
        for slot in ('checkpoint', 'final'):
            states = [self.measurements['saves'][variant+' saves/'+slot]['water'] for variant in PROFILES]
            assert exact(states[0], states[1]) and exact(states[0], states[2]), slot
        self.measurements['contrast'] = dict(max_hdr_delta=max(altered_hdr), frozen_phase_gap=max(opposed_normals),
            wrong_origin_gap=max(origin_gap), unfiltered_gap=max(fade_gap), zero_legacy_exact=True)

    def run(self):
        verify_reference()
        self.oracle = reference()
        self.measurements.update(bundles={}, captures={}, reports={}, saves={})
        revision = self.author()
        variants = {}
        for offset, (name, profile) in enumerate(PROFILES.items()):
            revision, game = self.bundle(name, profile, revision, 8120+offset)
            variants[name] = revision, game
        detached = self.root/'Hidden Water Authoring'
        self.project.rename(detached)
        source = tree_hashes(detached)
        assert not self.project.exists()
        captures = {}
        for variant, (revision, game) in variants.items():
            captures[variant] = self.exercise(variant, revision, game)
            assert not self.project.exists() and tree_hashes(detached) == source
            assert tree_hashes(game.parent) == self.measurements['bundles'][variant]['content']
        self.compare(captures)
        self.measurements['detached_source'] = source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True, help='Installed Windows runtime used for export.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--reconstruction', choices=('none', 'fsr3_native'), default='none')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-ripples-', dir=args.output.resolve()))
    test = RippleCapture(args, root)
    record = dict(format='poima.water-ripples-capture.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, reconstruction=args.reconstruction,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        errors = ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD', 'Undefined-Value')
        owner_logs = [call['wire']['stderr.log'] for call in test.calls if 'wire' in call]
        assert owner_logs and all(not any(error in log for error in errors) for log in owner_logs)
        validated = sum(VALIDATION_LAYER in log for log in owner_logs)
        if args.require_validation:
            assert validated == len(owner_logs), 'Vulkan validation did not load for every renderer owner.'
        record['validation_owners'] = validated
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
