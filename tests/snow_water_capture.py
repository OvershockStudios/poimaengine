#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check coexisting streamed snow and water, shadows and relocated save replay."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from snow_compaction_contract import (
    CAMERA, CENTER, EAST, Lattice, REGULAR_DEPTH, REGULAR_RADIUS, TOP_DOWN, WEST, near, unit, vector_near,
)
from snow_shadow_contract import (
    LAMP, LAMP_POSITION, RECEIVER, RECEIVER_CENTER, RECEIVER_SCALE, RECEIVER_TOP, TARGETS, SnowShadowContract, lamp,
)
from world_particles_contract import component, transform
from world_terrain_contract import CAPTURE_SIZE, encode, error, file_hash, projection, rpc, success, tree_hashes, uid
from world_water_contract import INDEX_BYTES_PER_QUAD, VERTEX_BYTES_PER_QUAD


BED = uid(8590)
ORIGIN = (2, -.25, 0)
CELL_SIZE = .5
CELLS = tuple((x, z) for z in range(2) for x in range(2))
SUPPLY = .9
BED_EMISSION = [.72, .105, .039]
EYE = (1.45, 2, .5)
LENS = dict(vertical_fov=54, near=.1, far=10)
SITES = {
    'snow_west': ('snow', .947, .071),
    'snow_east': ('snow', 1.053, .071),
    'snow_bank': ('snow', 1.9375, .405),
    'water_near': ('water', 2.16, .31),
    'water_far': ('water', 2.78, .73),
} | {f'shadow_{name}': ('shadow', x, RECEIVER_CENTER[2]) for name, x, _ in TARGETS}
WIDTH, HEIGHT = CAPTURE_SIZE
FOCAL = HEIGHT/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
MATRIX = [1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, *EYE, 1]
SHADOWS = {f'shadow_{name}': tile for name, _, tile in TARGETS}


def water_height(tick):
    return ORIGIN[1]+tick*SUPPLY/(60*CELL_SIZE**2)


def xz_on_plane(pixel, depth):
    x, y = pixel
    return EYE[0]+(x+.5-WIDTH/2)*depth/FOCAL, EYE[2]+(y+.5-HEIGHT/2)*depth/FOCAL


def witness_pixel(name):
    kind, x, z = SITES[name]
    y = (Lattice().surface(x, z, WEST if x < 1 else EAST)[0] if kind == 'snow' else
         ORIGIN[1] if kind == 'water' else RECEIVER_TOP)
    return tuple(map(math.floor, projection(MATRIX, LENS, (x, y, z))))


def witness(lattice, tick, name):
    kind, _, _ = SITES[name]
    pixel = witness_pixel(name)
    if kind == 'snow':
        low, high = 1.90, 2.10

        def intersection(depth):
            x, z = xz_on_plane(pixel, depth)
            tile = WEST if x < 1 else EAST
            sample = lattice.surface(x, z, tile, smooth=True)
            return x, z, tile, sample

        for _ in range(24):
            mid = (low+high)/2
            _, _, _, (height, _, _) = intersection(mid)
            if EYE[1]-mid > height:
                low = mid
            else:
                high = mid
        depth = (low+high)/2
        x, z, entity, (height, normal, triangle) = intersection(depth)
    else:
        height = water_height(tick) if kind == 'water' else RECEIVER_TOP
        depth = EYE[1]-height
        x, z = xz_on_plane(pixel, depth)
        normal, triangle = (0, 1, 0), None
        entity = BED if kind == 'water' else RECEIVER
        if kind == 'water':
            assert 2.05 < x < 2.95 and .05 < z < .95, (name, tick, x, z)
            assert min(abs(x-2.5), abs(z-.5)) > .07, (name, tick, x, z)
        else:
            assert all(abs(p-RECEIVER_CENTER[i]) < RECEIVER_SCALE[i]/2-.012
                       for i, p in ((0, x), (2, z))), (name, pixel, x, z)
    n, f = LENS['near'], LENS['far']
    return dict(pixel=pixel, point=[x, height, z], normal=normal, camera_depth_m=depth,
                device_depth=f/(f-n)*(1-n/depth), entity=entity, triangle=triangle)


def validate_reference():
    lattice = Lattice()
    early = {tick: {name: witness(lattice, tick, name) for name in SITES} for tick in (0, 1)}
    assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
    late = {tick: {name: witness(lattice, tick, name) for name in SITES} for tick in (2, 3)}
    for group in (*early.values(), *late.values()):
        assert len({row['pixel'] for row in group.values()}) == len(SITES)
        assert all(5 < row['pixel'][0] < WIDTH-5 and 5 < row['pixel'][1] < HEIGHT-5 for row in group.values())
    for name in ('snow_west', 'snow_east'):
        assert late[2][name]['point'][1] < early[1][name]['point'][1]-.016
    for name in ('water_near', 'water_far'):
        assert all(b[name]['point'][1] > a[name]['point'][1]+.05 for a,b in
                   ((early[0],early[1]), (early[1],late[2]), (late[2],late[3])))
    return early | late


class SnowWaterCapture(SnowShadowContract):
    def commit(self, revision, request, *ops):
        result = self.world_call('world.transact', request_id=uid(request), base_revision=revision, ops=list(ops))
        assert result['committed'] and result['revision'] == revision+1, result
        return result['revision']

    def prepare(self):
        revision = self.add_test_light(self.author())
        # Exact contact along x=2 with the east snow tile. The bed itself is a
        # different mesh and sits below the snow surface.
        recipe = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3,
            tile_cells=2, origin_m=list(ORIGIN), spacing_m=[CELL_SIZE]*2, heights_m=[0]*9))
        terrain = self.world_call('world.terrain.inspect', recipe=recipe)
        assert self.world_call('world.terrain.cook', recipe=recipe,
                               expected_recipe_sha256=terrain['recipe_sha256']) == terrain
        tile, = terrain['tiles']
        assert (tile['x'], tile['z'], tile['vertices'], tile['triangles']) == (0, 0, 9, 8), tile
        part = dict(asset=tile['asset'], primitive=tile['primitive'])
        revision = self.commit(revision, 8570,
            component(CAMERA, 'Transform', transform(list(EYE), TOP_DOWN)),
            component(CAMERA, 'Camera', LENS),
            component(LAMP, 'Light', lamp(True)),
            component(uid(4), 'LightingEnvironment', dict(ambient=[.075, .075, .085], exposure=1,
                shadow_resolution=2048, shadow_filter='pcf3_point')),
            dict(op='entity.create', id=BED, name='Touching flat water bed'),
            component(BED, 'Transform', transform([0, 0, 0])),
            component(BED, 'StaticMesh', part | dict(visible=True, cast_shadows=False, receive_shadows=False)),
            component(BED, 'MeshCollider', part | dict(friction=.5, restitution=0)),
            component(BED, 'PbrMaterial', dict(base_color=[0]*3, emissive=BED_EMISSION,
                roughness=.8, metallic=1, double_sided=False)))
        parameters = dict(revision=revision, terrain=dict(recipe=recipe), tile_entities=[BED],
            expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            grid=dict(cells_x=2, cells_z=2, cell_size_m=CELL_SIZE, origin_x_m=ORIGIN[0],
                      origin_z_m=ORIGIN[2], relaxation_per_second=6), initial_depths_m=[0]*4,
            exchanges=[dict(cell=cell, source_m3_per_second=SUPPLY, drain_m3_per_second=0) for cell in CELLS])
        water = self.world_call('world.water.plan', **parameters)
        assert self.world_call('world.water.cook', **parameters, expected_plan_sha256=water['plan_sha256']) == water
        assert water['tiles'] == [dict(entity=BED, asset=tile['asset'])] and water['cells'] == 4, water
        pin = water['plan_sha256']
        assert water['asset_bytes'] == (self.assets/(pin+'.pwater')).stat().st_size
        assert file_hash(self.assets/(pin+'.pwater')) == pin
        self.measurements['water'] = water
        self.measurements['bed'] = terrain
        return revision, dict(op='water.set', asset=pin)

    def relocated(self, revision):
        profile = self.world_call('world.describe.select', sections=['snow'])
        assert profile['sections']['snow']['water_coexistence'] is True, profile
        dependencies = self.world_call('world.dependencies')
        assert dependencies['revision'] == revision and dependencies['needs_snow_water'] is True, dependencies
        project = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert project['needs_snow_water'] is True, project
        output, relocated = self.root/'Original game export', self.root/'Relocated standalone game'
        runtime = self.args.runtime_root
        assert json.loads((runtime/'runtime.json').read_bytes())['features']['snow_water_regions'] is True
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(runtime))))
        output.rename(relocated)
        source = self.root/'Detached authoring project'
        self.project.rename(source)
        assert not output.exists() and not self.project.exists()
        game = relocated/'game.json'
        executable = relocated/json.loads(game.read_bytes())['engine']['executable']
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert inspected['needs_snow_water'] is True, inspected
        expected = {tile['asset']+'.pmodel' for tile in self.measurements['terrain']['tiles']+self.measurements['bed']['tiles']}
        expected.add(self.measurements['water']['plan_sha256']+'.pwater')
        assets = [path for path in relocated.rglob('*') if path.suffix in ('.pmodel', '.pwater')]
        assert {path.name for path in assets} == expected and len(assets) == len(expected), assets
        assert all(file_hash(path) == path.stem for path in assets)
        self.measurements['immutable_bundle'] = tree_hashes(relocated)
        self.measurements['detached_source'] = tree_hashes(source)
        return game, source

    def water_observe(self, client, session, tick):
        report = client.call('runtime.water.inspect', dict(session_id=session, tick=tick, cells=[list(cell) for cell in CELLS]))
        assert report['active'] is True and report['tick'] == tick and report['session_id'] == session, report
        assert report['plan_sha256'] == self.measurements['water']['plan_sha256'], report
        for cell, row in zip(CELLS, report['cells'], strict=True):
            assert row['cell'] == list(cell), row
            near(row['floor_height_m'], ORIGIN[1], 1e-12)
            near(row['surface_height_m'], water_height(tick), 1e-10)
            near(row['depth_m'], water_height(tick)-ORIGIN[1], 1e-10)
        near(report['volume_m3'], 4*SUPPLY*tick/60, 1e-10)
        if tick:
            ledger = report['last_step']
            assert ledger['tick'] == tick
            near(ledger['supplied_m3'], 4*SUPPLY/60, 1e-10)
            near(ledger['drained_m3'], 0, 1e-12)
        return {key: value for key, value in report.items() if key != 'session_id'}

    def physical_rays(self, client, session, tick, lattice):
        hits = {}
        for name in SITES:
            expected = witness(lattice, tick, name)
            x, y, z = expected['point']
            if name in SHADOWS:
                direction = tuple(b-a for a,b in zip(LAMP_POSITION, (x,y,z), strict=True))
                ray = dict(origin=LAMP_POSITION, direction=unit(direction), distance=math.hypot(*direction)+.02)
                entity = SHADOWS[name] if not lattice.generation or name == 'shadow_quiet' else RECEIVER
            else:
                ray = dict(origin=[x, 2, z], direction=[0, -1, 0], distance=4)
                entity = expected['entity']
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick) | ray)['hit']
            assert hit is not None and hit['entity'] == entity, (name, tick, expected, hit)
            if name.startswith('water_'):
                # Local surface rendering does not move the fixed bed collider.
                vector_near(hit['position'], (x, ORIGIN[1], z), 3e-5)
                vector_near(hit['normal'], (0, 1, 0), .002)
            elif name.startswith('snow_'):
                vector_near(hit['position'], expected['point'], 3e-5)
                assert hit['triangle'] == expected['triangle'], hit
            elif entity == RECEIVER:
                vector_near(hit['position'], expected['point'], 3e-5)
            hits[name] = hit
        return hits

    def collect_frame(self, client, session, tick, lattice, label):
        water = self.water_observe(client, session, tick)
        snow = self.snow(client, session, tick, lattice)
        del snow['session_id']
        hits = self.physical_rays(client, session, tick, lattice)
        expected = {name: witness(lattice, tick, name) for name in SITES}
        path = self.root/(label+'.bmp')
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=WIDTH, height=HEIGHT, gpu=self.args.gpu, samples=1,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2, profile=True,
            scene_product_probes=[dict(x=x, y=y) for x, y in (row['pixel'] for row in expected.values())]))
        assert receipt['hardware'] is True and receipt['capture_written'] is True, receipt
        assert receipt['frames_presented'] == 2 and receipt['nvrhi_errors'] == 0 and receipt['tick'] == tick, receipt
        assert receipt['lens'] == LENS, receipt
        vector_near(receipt['camera_world'], MATRIX, 1e-8)
        diag = receipt['render_diagnostics']
        assert diag['lighting_path'] == self.args.lighting_path, diag
        draws = diag['last_draws']
        wet_count = len(CELLS) if tick else 0
        assert draws['streamed_draws'] == 2+int(bool(wet_count)), draws
        assert draws['streamed_vertex_bytes'] == 2*33**2*48+wet_count*VERTEX_BYTES_PER_QUAD, draws
        assert draws['streamed_index_bytes'] == 2*32**2*6*4+wet_count*INDEX_BYTES_PER_QUAD, draws
        assert draws['shadow_views'] == 1 and draws['shadow_draws'] > 0, draws
        products = diag['scene_products']
        assert products['available'] is True and products['view'] == 'color', products
        image = pixels(path)
        assert len(image) == HEIGHT and all(len(row) == WIDTH for row in image)
        frame = dict(sha256=file_hash(path), gpu=receipt['gpu'], snow=snow, water=water, draws=draws, probes={})
        self.measurements['captures'][label] = dict(receipt=receipt, frame=frame)
        for (name, desired), actual in zip(expected.items(), products['probes'], strict=True):
            x, y = desired['pixel']
            row = frame['probes'][name] = dict(expected=desired, raster=actual, rgb=image[y][x], physics_hit=hits[name])
            assert (actual['x'], actual['y'], actual['resolved_x'], actual['resolved_y']) == (x,y,x,y), row
            assert actual['surface_valid'] is True, row
            near(actual['depth'], desired['device_depth'], 2e-6)
            vector_near(actual['shading_normal'][:3], desired['normal'], .004)
            assert exact(actual['raw_hdr'], actual['resolved_hdr']), row
            if name.startswith('water_'):
                red, green, blue = actual['raw_hdr']
                if not tick:
                    vector_near(actual['raw_hdr'], BED_EMISSION, .003)
                else:
                    assert blue > .01 and green > .006 and red < min(green, blue)*.45, row
                    assert actual['motion_valid'] is False, row
        return frame

    def save_both(self, client, saves, session, tick, request, slot, lattice):
        raw = self.snow_archive(client, saves, session, tick, request, slot, lattice)
        archived = json.loads(raw)['snapshot']['payload']['water']
        assert archived['tick'] == tick and archived['plan_sha256'] == self.measurements['water']['plan_sha256'], archived
        self.measurements['archives'][slot]['water'] = archived
        return raw

    def exercise(self, game, revision):
        saves = self.root/'Game saves'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('warm combined scene', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(8572), saves, game)
            dry = self.collect_frame(client, session, 0, lattice, 'dry-pristine')
            step = client.call('runtime.step', self.guard(session, 0, 8573))
            assert step['current_tick'] == 1, step
            wet = self.collect_frame(client, session, 1, lattice, 'wet-pristine')
            stamp = self.stamp(client, session, 1, 0, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 1, 8574, stamp, retry=True)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            compacted = self.collect_frame(client, session, 2, lattice, 'wet-compacted')
            checkpoint = self.save_both(client, saves, session, 2, 8575, 'checkpoint', lattice)
            step = client.call('runtime.step', self.guard(session, 2, 8576))
            assert step['current_tick'] == 3, step
            final = self.collect_frame(client, session, 3, lattice, 'warm-continued')
            final_archive = self.save_both(client, saves, session, 3, 8577, 'warm-continued', lattice)
        with self.owner('cold combined scene', game=game, saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(8578), saves, game)
            session = uid(8579)
            assert self.load(client, revision, previous, session, 'checkpoint', 2, 8580) == 0
            assert checkpoint == self.save_both(client, saves, session, 2, 8581, 'cold-checkpoint', lattice)
            assert exact(compacted, self.collect_frame(client, session, 2, lattice, 'cold-checkpoint'))
            step = client.call('runtime.step', self.guard(session, 2, 8582))
            assert step['current_tick'] == 3, step
            assert final_archive == self.save_both(client, saves, session, 3, 8583, 'cold-continued', lattice)
            assert exact(final, self.collect_frame(client, session, 3, lattice, 'cold-continued'))
        assert warm['pid'] != cold['pid'], (warm, cold)
        self.compare(dry, wet, compacted, final)

    def compare(self, dry, wet, compacted, final):
        assert len({frame['sha256'] for frame in (dry, wet, compacted, final)}) == 4
        assert len({frame['gpu'] for frame in (dry, wet, compacted, final)}) == 1
        contrasts = self.measurements['contrasts'] = {}
        for name in SITES:
            original, wet_only, stamped = (frame['probes'][name] for frame in (dry, wet, compacted))
            if not name.startswith('water_'):
                keys = ('depth', 'shading_normal', 'raw_hdr', 'resolved_hdr')
                assert exact({k: original['raster'][k] for k in keys}, {k: wet_only['raster'][k] for k in keys}), name
                assert exact(original['rgb'], wet_only['rgb']), name
            if name in ('snow_west', 'snow_east'):
                before, after = wet_only['raster']['depth'], stamped['raster']['depth']
                assert after-before > .00035, (name, before, after)
                contrasts[name] = dict(device_depth_delta=after-before)
            elif name == 'snow_bank' or name == 'shadow_quiet':
                assert exact(wet_only['raster']['raw_hdr'], stamped['raster']['raw_hdr']), name
                assert exact(wet_only['raster']['depth'], stamped['raster']['depth']), name
            elif name.startswith('shadow_'):
                before, after = (row['raster']['raw_hdr'] for row in (wet_only, stamped))
                assert min(after) > .15 and all(a < .24*b for a,b in zip(before, after, strict=True)), (name, before, after)
                contrasts[name] = dict(shadowed=before, cleared=after)
            elif name.startswith('water_'):
                before, wet_depth, after = (row['raster']['depth'] for row in (original, wet_only, stamped))
                assert before > wet_depth > after and math.dist(original['rgb'], wet_only['rgb']) > 35, name
                contrasts[name] = dict(dry_depth=before, newly_wet_depth=wet_depth, stamped_tick_depth=after)
        self.measurements['cold_scene_and_archives_equal'] = True

    def run(self):
        self.measurements.update(reference=validate_reference(), captures={})
        revision, bind = self.prepare()
        if self.args.preflight_only:
            self.baseline_after_prepare(revision, bind)
            return
        revision = self.commit(revision, 8571, bind)
        game, source = self.relocated(revision)
        self.exercise(game, revision)
        assert not self.project.exists() and tree_hashes(source) == self.measurements['detached_source']
        assert tree_hashes(game.parent) == self.measurements['immutable_bundle']
        for call in self.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')), stderr

    def baseline_after_prepare(self, revision, bind):
        before = self.world.read_bytes()
        request = rpc('world.transact', request_id=uid(8571), base_revision=revision, ops=[bind])
        failure = error(self.world_calls(request)[0], -32602)
        assert 'water' in failure['message'].lower() and 'snow' in failure['message'].lower(), failure
        assert self.world.read_bytes() == before
        self.measurements['baseline_mixed_bind_rejection'] = failure
        saves = self.root/'Control saves'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('headless baseline') as (client, _):
            session = self.initialize(client, revision, uid(8584), saves, None)
            # The bed stays at its dry height and the two snow/receiver rays
            # still obey the same geometry before any renderer is started.
            self.physical_rays(client, session, 0, lattice)
            self.update(client, session, 0, 8585, self.stamp(client, session, 0, 0, REGULAR_RADIUS, REGULAR_DEPTH))
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            self.snow(client, session, 1, lattice)
            self.physical_rays(client, session, 1, lattice)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed Windows runtime.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--preflight-only', action='store_true', help='Check an old binary and its mixed-binding rejection without GPU work.')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Run with assertions enabled and a GPU index from 0 through 4095.')
    if not args.preflight_only and args.runtime_root is None:
        parser.error('The GPU cohort requires --runtime-root.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-water-', dir=args.output.resolve()))
    test = SnowWaterCapture(args, root)
    record = dict(format='poima.snow-water-capture.v1', passed=False, preflight_only=args.preflight_only,
                  lighting_path=args.lighting_path, gpu=args.gpu, binary_sha256=file_hash(args.binary),
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
