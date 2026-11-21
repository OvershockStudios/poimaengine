#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a shared snow lattice, live collision, atomic stamps and relocated save replay."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from character_library_intake import LICENSE_SHA, SOURCE_SHA
from input_actions_gameplay_contract import exact
from runtime_ground_contacts import CALIBRATION, CHAINS, RIG
from surface_marks_capture import SurfaceMarksCapture
from world_particles_contract import component, transform
from world_terrain_contract import CAPTURE_SIZE, encode, error, file_hash, projection, rpc, success, tree_hashes, uid


WEST, EAST, PLAIN, CAMERA = map(uid, range(410, 414))
LIGHT = uid(5)
TILES = [WEST, EAST]
CELLS = 32
WIDTH, HEIGHT = 2 * CELLS + 1, CELLS + 1
SPACING = 1 / CELLS
HEIGHT_UNIT = 1 / 4096
THICKNESS = 128
CENTER = (1 - 8 * HEIGHT_UNIT, .0625 + 5 * HEIGHT_UNIT)
REGULAR_RADIUS, REGULAR_DEPTH = 1024, 90
CAMERA_POSITION = [1, 2, .5]
TOP_DOWN = [-math.sqrt(.5), 0, 0, math.sqrt(.5)]
LENS = dict(vertical_fov=50, near=.1, far=10)
POSITIONS = [('west', .963, .038), ('seam', 1, .038), ('east', 1.036, .038),
             ('center', *CENTER), ('undisturbed', 1.379, .411)]
SAMPLES = [[31, 1], [32, 2], [33, 2], [32, 3], [26, 2], [38, 2], [44, 2], [32, 0],
           [0, 0], [64, 32]]
DRAWABLE = dict(primitive='box', albedo=[.55, .61, .67], visible=True)
MATERIAL = dict(base_color=DRAWABLE['albedo'], emissive=[0, 0, 0], metallic=0, roughness=.8,
                double_sided=False)
BOX = dict(motion='static', half_extents=[.5]*3, mass=1, friction=.5, restitution=0)


def base(x, z):
    return (2*x + 3*z) * HEIGHT_UNIT


def down(x, z):
    return dict(origin=[x, 2, z], direction=[0, -1, 0], distance=3)


def near(actual, expected, tolerance):
    assert math.isclose(actual, expected, rel_tol=0, abs_tol=tolerance), (actual, expected, tolerance)


def vector_near(actual, expected, tolerance):
    for a, b in zip(actual, expected, strict=True):
        near(a, b, tolerance)


def unit(vector):
    length = math.hypot(*vector)
    return tuple(value/length for value in vector)


def faces(x, z):
    """Two upward triangles in a 1/32 m cell."""
    return (((x, z), (x+1, z+1), (x+1, z)),
            ((x, z), (x, z+1), (x+1, z+1)))


class Lattice:
    """Independent triangle and displaced-volume oracle for this authored grid."""

    def __init__(self):
        self.generation = 0
        self.depth = [[0]*WIDTH for _ in range(HEIGHT)]

    def stamp(self, center, radius, depth):
        cx, cz = (round(axis/HEIGHT_UNIT) for axis in center)
        step = round(SPACING / HEIGHT_UNIT)
        square = radius**2
        changed = False
        for z, row in enumerate(self.depth):
            for x, old in enumerate(row):
                distance = (x*step-cx)**2 + (z*step-cz)**2
                if distance >= square:
                    continue
                increase = (2*depth*(square-distance) + square) // (2*square)
                row[x] = min(THICKNESS, old+increase)
                changed |= row[x] != old
        self.generation += int(changed)
        return changed

    def payload(self):
        return dict(version=1, generation=self.generation,
                    samples=[[z*WIDTH+x, depth] for z, row in enumerate(self.depth)
                             for x, depth in enumerate(row) if depth])

    def volume_units(self):
        # Sum offsets per incident face. An interior vertex contributes six
        # times, an edge fewer, and a shared seam has only one physical vertex.
        return sum(self.depth[z][x] for j in range(HEIGHT-1) for i in range(WIDTH-1)
                   for face in faces(i, j) for x, z in face)

    def vertices(self, face):
        return [[x*SPACING, base(x, z)-self.depth[z][x]*HEIGHT_UNIT, z*SPACING] for x, z in face]

    def cross(self, face):
        a, b, c = self.vertices(face)
        u = [s-t for s, t in zip(b, a, strict=True)]
        v = [s-t for s, t in zip(c, a, strict=True)]
        return (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])

    def vertex_normal(self, vertex):
        x, z = vertex
        contributions = [self.cross(face) for i in range(max(x-1, 0), min(x+1, WIDTH-1))
                         for j in range(max(z-1, 0), min(z+1, HEIGHT-1))
                         for face in faces(i, j) if vertex in face]
        return unit(tuple(map(sum, zip(*contributions, strict=True))))

    def cell(self, x, z, tile):
        grid_x, grid_z = x/SPACING, z/SPACING
        tile_x = TILES.index(tile)*CELLS
        i = max(tile_x, min(math.floor(grid_x), tile_x+CELLS-1))
        j = min(math.floor(grid_z), HEIGHT-2)
        u, v = grid_x-i, grid_z-j
        assert -1e-6 <= u <= 1+1e-6 and -1e-6 <= v <= 1+1e-6, (x, z, tile, u, v)
        if u >= v:
            return i, j, 0, (1-u, v, u-v)
        return i, j, 1, (1-v, v-u, u)

    def surface(self, x, z, tile, *, smooth=False):
        i, j, selector, weights = self.cell(x, z, tile)
        face = faces(i, j)[selector]
        y = sum(p[1]*w for p, w in zip(self.vertices(face), weights, strict=True))
        if smooth:
            normal = unit(tuple(sum(v*w for v, w in zip(axis, weights, strict=True))
                                for axis in zip(*(self.vertex_normal(p) for p in face), strict=True)))
        else:
            normal = unit(self.cross(face))
        triangle = 2*(CELLS*j+i-TILES.index(tile)*CELLS) + selector
        return y, normal, triangle


class SnowContract(SurfaceMarksCapture):
    def discovery(self):
        result = self.world_call('world.describe.select', methods=['runtime.snow.step'], sections=['snow'])
        assert result['sections']['snow']['stamp_method'] == 'runtime.snow.step', result['sections']
        schema = result['methods']['runtime.snow.step']
        assert 'snow_stamp' in schema['required'] and 'surface_marks' in schema['properties'], schema
        existing = self.world_call('world.describe.select', methods=['runtime.step'],
                                   sections=['surface_marks', 'gameplay_surface_marks'])
        assert 'snow_stamp' not in existing['methods']['runtime.step']['properties']

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Compacted Snow')))
        self.discovery()
        baseline = self.world_call('world.inspect')['revision']
        recipe = dict(format='poima.terrain.heightfield', version=1, samples_x=WIDTH, samples_z=HEIGHT,
                      tile_cells=CELLS, origin_m=[0, 0, 0], spacing_m=[SPACING]*2,
                      heights_m=[base(x, z) for z in range(HEIGHT) for x in range(WIDTH)])
        raw = encode(recipe)
        plan = self.world_call('world.terrain.inspect', recipe=raw)
        assert self.world_call('world.terrain.cook', recipe=raw, expected_recipe_sha256=plan['recipe_sha256']) == plan
        before = self.world.read_bytes()
        operations = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            dict(op='entity.create', id=CAMERA, name='Fixed snow geometry camera'),
            component(CAMERA, 'Transform', transform(CAMERA_POSITION, TOP_DOWN)),
            component(CAMERA, 'Camera', LENS),
            component(LIGHT, 'Transform', transform([0, 0, 0], TOP_DOWN)),
            component(LIGHT, 'Light', dict(kind='directional', color=[1, 1, 1], intensity=1, enabled=True,
                                           shadow=dict(enabled=True, distance=8, bias=.0001, normal_bias=.005))),
            dict(op='entity.create', id=PLAIN, name='Off-lattice comparison box'),
            component(PLAIN, 'Transform', transform([4, 0, .5])),
            component(PLAIN, 'MeshRenderer', DRAWABLE),
            component(PLAIN, 'PbrMaterial', MATERIAL),
            component(PLAIN, 'BoxCollider', BOX),
        ]
        for x, (entity, tile) in enumerate(zip(TILES, plan['tiles'], strict=True)):
            assert (tile['x'], tile['z'], tile['vertices'], tile['triangles']) == (
                x, 0, (CELLS+1)**2, 2*CELLS**2), tile
            part = dict(asset=tile['asset'], primitive=tile['primitive'])
            operations.extend([dict(op='entity.create', id=entity, name=f'Snow terrain tile {x}'),
                               component(entity, 'Transform', transform([0, 0, 0])),
                               component(entity, 'StaticMesh', dict(part, visible=True)),
                               component(entity, 'MeshCollider', dict(part, friction=.5, restitution=0))])
        for index, thickness in enumerate((True, 513)):
            malformed = dict(op='snow.set', definition=dict(tiles=TILES, thickness_units=thickness))
            reply, = self.world_calls(rpc('world.transact', request_id=uid(440+index), base_revision=baseline,
                                          ops=operations+[malformed]))
            error(reply, -32602)
            assert self.world.read_bytes() == before
        snow = dict(op='snow.set', definition=dict(tiles=TILES, thickness_units=THICKNESS))
        transaction = self.world_call('world.transact', request_id=uid(442), base_revision=baseline,
                                      ops=operations+[snow])
        assert transaction['committed'] and transaction['revision'] == baseline+1, transaction
        frozen = self.world.read_bytes()
        revision = transaction['revision']
        rejected = []
        for index, operation in enumerate((
                dict(op='snow.set', definition=dict(tiles=[EAST, WEST], thickness_units=THICKNESS)),
                component(WEST, 'Transform', transform([0, .125, 0])),
                dict(op='component.remove', id=EAST, type='MeshCollider'))):
            reply, = self.world_calls(rpc('world.transact', request_id=uid(445+index),
                                          base_revision=revision, ops=[operation]))
            rejected.append(error(reply, -32602))
            assert self.world.read_bytes() == frozen
        self.measurements['terrain'] = plan
        self.measurements['authoring_refusals'] = rejected
        return revision

    def snow(self, client, session, tick, expected):
        result = client.call('runtime.snow.inspect',
                             dict(session_id=session, tick=tick, structure_revision=0, samples=SAMPLES))
        assert (result['session_id'], result['tick'], result['structure_revision']) == (session, tick, 0), result
        assert result['active'] is True and exact(result['generation'], expected.generation), result
        assert exact(result['tiles'], TILES) and exact(result['thickness_units'], THICKNESS), result
        vector_near(result['origin_m'], [0, 0], 0)
        near(result['spacing_m'], SPACING, 0)
        assert exact(result['compacted_samples'], len(expected.payload()['samples'])), result
        volume = expected.volume_units()
        assert exact(result['lost_volume_units'], volume), result
        near(result['lost_volume_m3'], volume*SPACING**2*HEIGHT_UNIT/6, 1e-12)
        for (x, z), sample in zip(SAMPLES, result['samples'], strict=True):
            amount = expected.depth[z][x]
            assert all(exact(sample[key], value) for key, value in
                       dict(x=x, z=z, compaction_units=amount).items()), sample
            near(sample['base_height_m'], base(x, z), 0)
            near(sample['height_m'], base(x, z)-amount*HEIGHT_UNIT, 0)
        return result

    def collision(self, client, session, tick, expected):
        guard = dict(session_id=session, tick=tick)
        result = {}
        for name, x, z in POSITIONS:
            hit = client.call('runtime.raycast', guard | down(x, z))['hit']
            tile = hit['entity']
            owners = TILES if x == 1 else [TILES[int(x > 1)]]
            assert tile in owners, hit
            y, normal, triangle = expected.surface(x, z, tile)
            assert exact(hit['triangle'], triangle), (x, z, hit, triangle)
            vector_near(hit['position'], [x, y, z], 3e-5)
            vector_near(hit['normal'], normal, 2e-3)
            row = result[name] = dict(ray=hit, expected_height_m=y, expected_normal=normal)
            if name in ('west', 'east'):
                capsule = client.call('runtime.capsule_cast', guard | dict(position=[x, 2, z],
                    rotation=[0, 0, 0, 1], radius=.003, half_height=.01, direction=[0, -1, 0], distance=3))['hit']
                assert capsule['entity'] == tile and capsule['initial_overlap'] is False, capsule
                px, py, pz = capsule['point_on_obstacle']
                surface_height, _, capsule_triangle = expected.surface(px, pz, tile)
                assert exact(capsule['triangle'], capsule_triangle), (capsule, capsule_triangle)
                near(py, surface_height, 2e-4)
                row['capsule'] = capsule
        return result

    def stamp(self, client, session, tick, generation, radius, depth, center=CENTER):
        ray = down(*center)
        sensor = client.call('runtime.raycast', dict(session_id=session, tick=tick) | ray)['hit']
        return dict(expected_generation=generation, receiver=sensor['entity'], triangle=sensor['triangle'],
                    ray=ray, radius_units=radius, depth_units=depth)

    def guard(self, session, tick, request):
        return dict(session_id=session, expected_tick=tick, expected_structure_revision=0,
                    request_id=uid(request), ticks=1)

    def update(self, client, session, tick, request, stamp, *, retry=False):
        params = self.guard(session, tick, request) | dict(snow_stamp=stamp)
        result = client.call('runtime.snow.step', params)
        assert result['current_session_id'] == session and exact(result['current_tick'], tick+1), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        if retry:
            replay = client.call('runtime.snow.step', params)
            assert exact(replay, result | dict(replayed=True)), (result, replay)

    def snow_archive(self, client, saves, session, tick, request, slot, expected):
        raw = self.archive(client, saves, session, tick, request, slot, has_marks=False)
        archived = json.loads(raw)['snapshot']['payload']['snow']
        assert exact(archived, expected.payload()), (slot, archived, expected.payload())
        return raw

    def refused(self, client, session, tick, stamp):
        samples = dict(session_id=session, tick=tick, structure_revision=0, samples=SAMPLES)
        denied = self.measurements['refusals'] = {
            'stale_tick': self.reject(client, 'runtime.snow.inspect', samples | dict(tick=tick-1), -32009),
            'stale_structure': self.reject(client, 'runtime.snow.inspect', samples | dict(structure_revision=1), -32009),
            'too_many_samples': self.reject(client, 'runtime.snow.inspect',
                                            samples | dict(samples=[[32, 14]]*65), -32602),
            'ordinary_step_with_stamp': self.reject(client, 'runtime.step',
                                                   self.guard(session, tick, 457) | dict(snow_stamp=stamp), -32602),
            'snow_step_without_stamp': self.reject(client, 'runtime.snow.step',
                                                   self.guard(session, tick, 458), -32602),
        }
        cases = [
            ('stale_generation', stamp | dict(expected_generation=0), -32040),
            ('wrong_receiver', stamp | dict(receiver=EAST if stamp['receiver'] == WEST else WEST), -32040),
            ('wrong_triangle', stamp | dict(triangle=stamp['triangle'] ^ 1), -32040),
            ('out_of_range_radius', stamp | dict(radius_units=127), -32602),
            ('malformed_depth', stamp | dict(depth_units=True), -32602),
        ]
        for index, (name, command, code) in enumerate(cases):
            params = self.guard(session, tick, 460+index) | dict(snow_stamp=command)
            denied[name] = self.reject(client, 'runtime.snow.step', params, code)
        # The opaque, matching static box admits this surface mark before Tick.
        # Its ray misses at the end of Tick, after the snow geometry was staged.
        failed_mark = dict(receiver=PLAIN, ray=down(9, 9), half_extent_m=[.1, .1], half_depth_m=.03,
                           minimum_normal_dot=.8, edge_fraction=.25, linear_rgb=[.1, .2, .3],
                           opacity=1, roughness=.8)
        params = self.guard(session, tick, 469) | dict(snow_stamp=stamp, surface_marks=[failed_mark])
        late = denied['mark_after_staging'] = self.reject(client, 'runtime.snow.step', params, -32040)
        assert 'must first hit the expected receiver' in late['message'], late

    def capture_snow(self, client, session, tick, label, expected):
        path = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        camera = client.call('runtime.entity', dict(session_id=session, tick=tick, id=CAMERA))['world_matrix']
        pixels = []
        for _, x, z in POSITIONS:
            y, _, _ = expected.surface(x, z, TILES[int(x >= 1)])
            pixels.append(tuple(map(math.floor, projection(camera, LENS, [x, y, z]))))
        assert len(set(pixels)) == len(pixels), pixels
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in pixels]))
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera and receipt['nvrhi_errors'] == 0, receipt
        diagnostic = receipt['render_diagnostics']
        assert diagnostic['lighting_path'] == self.args.lighting_path, diagnostic
        draws = diagnostic['last_draws']
        assert draws['streamed_draws'] >= (2 if expected.generation else 0), draws
        assert draws['shadow_draws'] > 0 and draws['shadow_triangles'] >= 4*CELLS**2, draws
        focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
        observations = []
        for (px, py), sample in zip(pixels, diagnostic['scene_products']['probes'], strict=True):
            assert (sample['x'], sample['y']) == (px, py) and sample['surface_valid'] is True, sample

            def intersection(distance):
                x = CAMERA_POSITION[0]+(px+.5-width/2)*distance/focal
                z = CAMERA_POSITION[2]+(py+.5-height/2)*distance/focal
                return expected.surface(x, z, TILES[int(x >= 1)], smooth=True)

            low, high = 1.9, 2.1
            for _ in range(24):
                depth = (low+high)/2
                y, _, _ = intersection(depth)
                if CAMERA_POSITION[1]-depth > y:
                    low = depth
                else:
                    high = depth
            distance = (low+high)/2
            _, normal, _ = intersection(distance)
            near_plane, far_plane = LENS['near'], LENS['far']
            device_depth = far_plane/(far_plane-near_plane)*(1-near_plane/distance)
            near(sample['depth'], device_depth, 2e-6)
            vector_near(sample['shading_normal'][:3], normal, .004)
            observations.append(dict(camera_depth_m=distance, expected_device_depth=device_depth,
                                     expected_normal=normal, observed=sample))
        capture = dict(sha256=file_hash(path), gpu=receipt['gpu'], lighting_path=self.args.lighting_path,
                       probes=observations)
        self.measurements.setdefault('captures', {})[label] = dict(path=str(path), report=receipt) | capture
        return capture

    def exercise_snow(self, revision, game=None):
        saves = self.root/'Save slots'
        saves.mkdir()
        phases = self.measurements['phases'] = {}

        def observe(client, session, tick, expected, label):
            snow = self.snow(client, session, tick, expected)
            collisions = self.collision(client, session, tick, expected)
            phases[label] = dict(snow=snow, collision=collisions)
            return collisions

        lattice = Lattice()
        with self.owner('warm', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(470), saves, game)
            baseline = observe(client, session, 0, lattice, 'pristine')
            if game:
                pristine_capture = self.capture_snow(client, session, 0, 'pristine', lattice)
            first = self.stamp(client, session, 0, 0, REGULAR_RADIUS, REGULAR_DEPTH)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 0, 471, first, retry=True)
            initial = observe(client, session, 1, lattice, 'first_stamp')
            for name in ('west', 'seam', 'east'):
                assert initial[name]['ray']['position'][1] < baseline[name]['ray']['position'][1], name
            for name in ('west', 'east'):
                assert initial[name]['capsule']['distance'] > baseline[name]['capsule']['distance'], name
            quiet, pristine = (phase['undisturbed'] for phase in (initial, baseline))
            assert exact(quiet['expected_height_m'], pristine['expected_height_m'])
            assert exact(quiet['expected_normal'], pristine['expected_normal'])
            quiet_ray, pristine_ray = quiet['ray'], pristine['ray']
            assert exact({key: value for key, value in quiet_ray.items() if key != 'normal'},
                         {key: value for key, value in pristine_ray.items() if key != 'normal'})
            # Rebuilding the Jolt mesh can change its quantized vertex encoding
            # even where the source vertices and ray intersection stay fixed.
            vector_near(quiet_ray['normal'], pristine_ray['normal'], 2e-6)
            vector_near(quiet_ray['normal'], quiet['expected_normal'], 2e-6)
            if game:
                checkpoint_capture = self.capture_snow(client, session, 1, 'warm-checkpoint', lattice)
                assert checkpoint_capture['sha256'] != pristine_capture['sha256']
            checkpoint = self.snow_archive(client, saves, session, 1, 472, 'checkpoint', lattice)

            second = self.stamp(client, session, 1, 1, REGULAR_RADIUS, REGULAR_DEPTH)
            self.refused(client, session, 1, second)
            snow_after = observe(client, session, 1, lattice, 'after_refusals')
            assert exact(initial, snow_after)
            assert checkpoint == self.snow_archive(client, saves, session, 1, 473, 'after-refusals', lattice)
            self.update(client, session, 1, 474, second)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            saturated = observe(client, session, 2, lattice, 'saturation')
            assert lattice.depth[2][32] == THICKNESS
            no_op = self.stamp(client, session, 2, 2, 128, REGULAR_DEPTH)
            self.update(client, session, 2, 475, no_op)
            assert not lattice.stamp(CENTER, 128, REGULAR_DEPTH)
            assert exact(saturated, observe(client, session, 3, lattice, 'saturated_noop'))
            warm_final = self.snow_archive(client, saves, session, 3, 476, 'warm-final', lattice)
            if game:
                final_capture = self.capture_snow(client, session, 3, 'warm-final', lattice)
                assert final_capture['sha256'] != checkpoint_capture['sha256']

        lattice = Lattice()
        with self.owner('cold', game=game, saves=saves) as (client, cold):
            fresh = self.initialize(client, revision, uid(480), saves, game)
            session = uid(481)
            assert self.load(client, revision, fresh, session, 'checkpoint', 1, 482) == 0
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            restored = observe(client, session, 1, lattice, 'restored')
            assert exact(initial, restored)
            assert checkpoint == self.snow_archive(client, saves, session, 1, 483, 'cold-checkpoint', lattice)
            if game:
                assert exact(checkpoint_capture, self.capture_snow(client, session, 1, 'cold-checkpoint', lattice))
            self.update(client, session, 1, 484, second)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 2, 485, no_op)
            assert not lattice.stamp(CENTER, 128, REGULAR_DEPTH)
            assert exact(saturated, observe(client, session, 3, lattice, 'cold_saturated'))
            assert warm_final == self.snow_archive(client, saves, session, 3, 486, 'cold-final', lattice)
            if game:
                assert exact(final_capture, self.capture_snow(client, session, 3, 'cold-final', lattice))
        assert warm['pid'] != cold['pid']

    def held_contact(self):
        revision = self.author()
        source, notice = self.args.source, self.args.license
        assert file_hash(source) == SOURCE_SHA and file_hash(notice) == LICENSE_SHA
        inspected = self.world_call('asset.source.inspect', source=self.native(source))
        asset = self.world_call('asset.import', source=self.native(source),
                                expected_model_sha256=inspected['model_sha256'])['asset']
        assert file_hash(self.assets/(asset+'.pmodel')) == asset
        result = self.world_call('world.transact', request_id=uid(490), base_revision=revision,
                                 ops=[dict(op='asset.instantiate', id=RIG, asset=asset, name='Original Knight')])
        assert result['committed'] and result['revision'] == revision+1, result
        revision = result['revision']
        rig = json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig']
        rig.update(clip=36, time=0, speed=1, loop=False, playing=False,
                   ik=[dict(slot=slot, root=root, middle=middle, end=end, target=[0, 0, 0], pole=[0, 1, 1],
                            space='rig', metric='near-uniform-v1',
                            grounding=dict(calibration, reference_height=0, plant_lock=True, max_correction=.4))
                       for (slot, root, middle, end), calibration in zip(CHAINS, CALIBRATION, strict=True)])
        result = self.world_call('world.transact', request_id=uid(491), base_revision=revision,
                                 ops=[component(RIG, 'Transform', transform([1.03, .02, .5])),
                                      component(RIG, 'AnimationRig', rig)])
        assert result['committed'] and result['revision'] == revision+1, result
        revision = result['revision']
        saves = self.root/'Save slots'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('held foot') as (client, _):
            session = self.initialize(client, revision, uid(492), saves, None)

            def states(tick):
                return [client.call('runtime.ik', dict(session_id=session, tick=tick, entity=RIG, slot=slot,
                            include_grounding=True, include_planting=True))['state'] for slot, *_ in CHAINS]

            client.call('runtime.step', self.guard(session, 0, 493))
            locked = states(1)
            assert [row['planting']['anchor']['support'] for row in locked] == [EAST, WEST], locked
            assert all(row['planting']['status'] == 'locked' for row in locked), locked
            px, _, pz = locked[0]['planting']['target_sole']
            command = self.stamp(client, session, 1, 0, 256, 75, center=(px, pz))
            before = self.snow_archive(client, saves, session, 1, 494, 'locked-before', lattice)
            fallback = self.guard(session, 1, 495) | dict(snow_stamp=command)
            rejected = self.reject(client, 'runtime.snow.step', fallback, -32040)
            assert 'held foot anchor' in rejected['message'], rejected
            assert exact(locked, states(1))
            assert before == self.snow_archive(client, saves, session, 1, 496, 'locked-after', lattice)
            # Active feet on this pair should not prevent a separate depression
            # that leaves both selected support triangles and their neighbors intact.
            remote = (.125, .125)
            far_stamp = self.stamp(client, session, 1, 0, 256, 75, center=remote)
            self.update(client, session, 1, 497, far_stamp)
            assert lattice.stamp(remote, 256, 75)
            still_locked = states(2)
            assert all(row['planting']['status'] == 'locked' for row in still_locked), still_locked
            for before, after in zip(locked, still_locked, strict=True):
                assert exact(before['planting']['anchor'], after['planting']['anchor']), (before, after)
            self.snow(client, session, 2, lattice)

            lift = dict(entity=RIG, clip=48, time=0, speed=1, loop=False, playing=False)
            client.call('runtime.step', self.guard(session, 2, 498) | dict(animations=[lift]))
            lifted = states(3)
            assert all(row['planting']['status'] == 'lifted' and row['planting']['anchor'] is None
                       for row in lifted), lifted
            self.update(client, session, 3, 499, command | dict(expected_generation=1))
            assert lattice.stamp((px, pz), 256, 75)
            self.snow(client, session, 4, lattice)
            self.snow_archive(client, saves, session, 4, 500, 'released-and-stamped', lattice)
        self.measurements['held_contact'] = dict(model_sha256=asset, source_sha256=SOURCE_SHA,
            license_sha256=LICENSE_SHA, contact=locked, rejected=rejected, unrelated_stamp=still_locked,
            lifted=lifted, eligible_again=lattice.payload())

    def run(self):
        revision = self.author()
        if self.args.headless:
            self.exercise_snow(revision)
            return
        built = self.root/'Built Snow'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(built),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Snow', self.root/'Unavailable authored project'
        built.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not built.exists()
        game = bundle/'game.json'
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        for tile in self.measurements['terrain']['tiles']:
            asset = tile['asset']
            assert any(Path(name).name == asset+'.pmodel' and digest == asset for name, digest in frozen.items())
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=frozen, detached_source=authored)
        self.exercise_snow(revision, game)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored

    def settled_prop(self):
        revision = self.author()
        body = uid(507)
        x, z, half = .981, .134, .018
        lattice = Lattice()
        surface, _, _ = lattice.surface(x, z, WEST)
        operations = [dict(op='entity.create', id=body, name='Settled prop over snow'),
                      component(body, 'Transform', transform([x, surface+half+.004, z])),
                      component(body, 'BoxCollider', BOX | dict(motion='dynamic', half_extents=[half]*3,
                                                                 mass=.1, friction=.9))]
        result = self.world_call('world.transact', request_id=uid(508), base_revision=revision, ops=operations)
        assert result['committed'] and result['revision'] == revision+1, result
        saves, session = self.root/'Prop slots', uid(509)
        saves.mkdir()

        def sample(client, tick, request, label):
            saved = self.snow_archive(client, saves, session, tick, request, label, lattice)
            rows = json.loads(saved)['snapshot']['payload']['entities']
            row, = (entity for entity in rows if entity['id'] == body)
            return row['body']

        with self.owner('sleeping prop') as (client, _):
            self.initialize(client, result['revision'], session, saves, None)
            client.call('runtime.step', self.guard(session, 0, 510) | dict(ticks=180))
            resting = sample(client, 180, 511, 'resting')
            assert resting['active'] is False and resting['linear_velocity'] == [0, 0, 0], resting
            command = self.stamp(client, session, 180, 0, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 180, 512, command)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            awakened = sample(client, 181, 513, 'awakened')
            assert awakened['active'] is True and awakened['linear_velocity'][1] < 0, awakened
            client.call('runtime.step', self.guard(session, 181, 514) | dict(ticks=180))
            settled = sample(client, 361, 515, 'settled')
            assert settled['active'] is False and settled['position'][1] < resting['position'][1]-.008, settled
        self.measurements['body'] = dict(resting=resting, awakened=awakened, settled=settled)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless', action='store_true', help='Check live collision, rollback and save continuation.')
    mode.add_argument('--runtime-root', type=Path, help='Installed Windows runtime for the relocated game.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--source', type=Path, help='Original Knight.glb for the optional held-foot check.')
    parser.add_argument('--license', type=Path, help='Exact CC0 notice accompanying the Knight.')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    if bool(args.source) != bool(args.license):
        parser.error('--source and --license must be supplied together.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-compaction-', dir=args.output.resolve()))
    contract = SnowContract(args, root)
    result = dict(format='poima.snow-compaction-contract.v1', passed=False, headless=args.headless,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  gpu=None if args.headless else args.gpu, lighting_path=None if args.headless else args.lighting_path,
                  calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
        if args.headless:
            prop_root = root/'Settled prop'
            prop_root.mkdir()
            prop_check = SnowContract(args, prop_root)
            result['prop_fixture'] = dict(calls=prop_check.calls, measurements=prop_check.measurements)
            prop_check.settled_prop()
        if args.source:
            contact_root = root/'Held contact'
            contact_root.mkdir()
            contact_check = SnowContract(args, contact_root)
            result['held_foot_fixture'] = dict(calls=contact_check.calls, measurements=contact_check.measurements)
            contact_check.held_contact()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
