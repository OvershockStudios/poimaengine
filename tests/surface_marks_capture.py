#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify receiver-local surface marks, atomic deposits and fresh-process saves."""
import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from scene_capture import pixels
from world_particles_contract import ParticleContract, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, encode, file_hash, projection, success, tree_hashes, uid
from poima_client import RpcError


MOVER, NEIGHBOR, WRONG_SHAPE, CAMERA = map(uid, (100, 101, 102, 103))
TERRAIN, BOX_TEMPLATE = uid(110), uid(111)
START = [-2, 1, -4.5]
STAMPED = [-1.25, 1, -4.5]
MOVED = [-2.375, 1, -4.5]
NEIGHBOR_CENTER = [0, 1, -4.5]
WRONG_SHAPE_CENTER = [2, 1, -4.5]
CAMERA_CENTER = [-1.25, 1, 0]
FRONT = -4
LENS = dict(vertical_fov=90, near=1, far=10)
LIMITS = dict(commands_per_step=16, marks_per_receiver=4, marks_per_scene=256)
BASE = (.125, .25, .5)
AMBIENT, INTENSITY, ROUGHNESS = .25, 1, .75
FIRST_OFFSET = (.3125, 0)
EXTRA_OFFSETS = ((-.3125, .25), (.25, -.3125))
MATERIAL = dict(base_color=list(BASE), emissive=[0, 0, 0], metallic=0, roughness=ROUGHNESS,
                double_sided=False)
DRAWABLE = dict(primitive='box', albedo=list(BASE), visible=True, cast_shadows=False, receive_shadows=False)


def ray(x, y):
    return dict(origin=[x, y, 0], direction=[0, 0, -1], distance=8)


def mark(position, offset, radius, color, opacity, roughness, edge=.5):
    x, y = (origin+local for origin, local in zip(position[:2], offset, strict=True))
    return dict(receiver=MOVER, ray=ray(x, y), half_extent_m=[radius]*2, half_depth_m=.125,
                minimum_normal_dot=.8, edge_fraction=edge, linear_rgb=list(color), opacity=opacity,
                roughness=roughness)


def first_marks():
    return [
        mark(STAMPED, FIRST_OFFSET, .75, (.875, .125, .125), .75, .375),
        mark(STAMPED, FIRST_OFFSET, .3125, (.125, .875, .125), .5, .25, edge=0),
    ]


def next_marks():
    return [
        mark(MOVED, EXTRA_OFFSETS[0], .09375, (.125, .125, .875), 1, .5),
        mark(MOVED, EXTRA_OFFSETS[1], .09375, (.875, .875, .125), 1, .375),
    ]


def motion(position):
    return dict(entity=MOVER, position=position, rotation=[0, 0, 0, 1], duration_ticks=1)


def world_position(x, y):
    """Intersect a raster pixel center with the front face shared by both boxes."""
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    view_depth = CAMERA_CENTER[2]-FRONT
    return (CAMERA_CENTER[0]+(x+.5-width/2)*view_depth/focal,
            CAMERA_CENTER[1]+(height/2-y-.5)*view_depth/focal, FRONT)


def normalized(vector):
    length = math.hypot(*vector)
    return tuple(value/length for value in vector)


def radiance(point, base, roughness):
    """Independent GGX reference for the front face, white normal-incidence light and no IBL."""
    view = normalized([camera-p for camera, p in zip(CAMERA_CENTER, point, strict=True)])
    halfway = normalized((view[0], view[1], view[2]+1))
    nv, nh = view[2], halfway[2]
    vh = sum(v*h for v, h in zip(view, halfway, strict=True))
    alpha_squared = roughness**4
    normal_distribution = alpha_squared/(math.pi*(nh*nh*(alpha_squared-1)+1)**2)
    visibility = .5/(math.sqrt(nv*nv*(1-alpha_squared)+alpha_squared)+nv)
    fresnel = .04+.96*(1-vh)**5
    return tuple(channel*AMBIENT+INTENSITY*((1-fresnel)*channel/math.pi+normal_distribution*visibility*fresnel)
                 for channel in base)


def surface(point, marks):
    base, roughness = BASE, ROUGHNESS
    for stamp in marks:
        cx, cy, _ = stamp['ray']['origin']
        u, v = stamp['half_extent_m']
        normalized_radius = math.hypot((point[0]-cx)/u, (point[1]-cy)/v)
        if normalized_radius > 1:
            continue
        edge = stamp['edge_fraction']
        opacity = stamp['opacity']
        if edge:
            t = max(0, min(1, (normalized_radius-(1-edge))/edge))
            opacity *= 1-t*t*(3-2*t)
        base = tuple(previous+(color-previous)*opacity
                     for previous, color in zip(base, stamp['linear_rgb'], strict=True))
        roughness += (stamp['roughness']-roughness)*opacity
    return base, roughness


class SurfaceMarksCapture(ParticleContract):
    def author(self):
        reply = self.cli('project', 'create', self.native(self.project), '--name', 'Persistent Surface Marks')
        success(json.loads(reply))
        base_revision = self.world_call('world.inspect')['revision']
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[AMBIENT]*3, exposure=1)),
               dict(op='entity.create', id=CAMERA, name='Fixed test camera'),
               component(CAMERA, 'Transform', transform(CAMERA_CENTER)),
               component(CAMERA, 'Camera', LENS),
               dict(op='entity.create', id=uid(104), name='Normal-incidence light'),
               component(uid(104), 'Transform', transform([0, 0, 0])),
               component(uid(104), 'Light', dict(kind='directional', color=[1, 1, 1], intensity=INTENSITY, enabled=True))]
        for entity, name, position, body, size in (
                (MOVER, 'Kinematic receiving crate', START, 'kinematic', .5),
                (NEIGHBOR, 'Adjacent unstamped crate', NEIGHBOR_CENTER, 'static', .5),
                (WRONG_SHAPE, 'Crate with mismatched collision', WRONG_SHAPE_CENTER, 'static', .375)):
            ops.extend([dict(op='entity.create', id=entity, name=name),
                        component(entity, 'Transform', transform(position)),
                        component(entity, 'MeshRenderer', DRAWABLE), component(entity, 'PbrMaterial', MATERIAL),
                        component(entity, 'BoxCollider', dict(motion=body, half_extents=[size]*3, mass=1,
                                                              friction=.5, restitution=0))])
        result = self.world_call('world.transact', request_id=uid(700), base_revision=base_revision, ops=ops)
        assert result['committed'] and result['revision'] == base_revision+1, result
        return result['revision']

    def inspect(self, client, session, tick, *, structure=0, **scope):
        result = client.call('runtime.surface_marks.inspect',
                             dict(session_id=session, tick=tick, structure_revision=structure) | scope)
        assert (result['session_id'], result['tick'], result['structure_revision']) == (session, tick, structure), result
        assert result['limits'] == LIMITS and isinstance(result['marks'], list), result
        return result['marks']

    def observe(self, client, session, tick, position):
        result = client.call('runtime.entity', dict(session_id=session, id=MOVER, tick=tick))
        near_vector(result['world_matrix'][12:15], position)
        return self.inspect(client, session, tick)

    def advance(self, client, session, tick, request, position, stamps, ticks=1, retry=False):
        params = dict(session_id=session, expected_tick=tick, request_id=uid(request), ticks=ticks,
                      expected_structure_revision=0, motions=[motion(position)], surface_marks=stamps)
        result = client.call('runtime.step', params)
        assert (result['current_session_id'], result['current_tick']) == (session, tick+ticks), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        rows = self.observe(client, session, tick+ticks, position)
        if retry:
            assert client.call('runtime.step', params) == (result | dict(replayed=True)), result
            assert self.inspect(client, session, tick+ticks) == rows
        return rows

    def expected_rows(self, rows):
        commands = first_marks() if len(rows) == 2 else first_marks()+next_marks()
        assert len(rows) == len(commands) and len(rows) in (2, 4), rows
        offsets = [FIRST_OFFSET, FIRST_OFFSET, *EXTRA_OFFSETS]
        for index, (row, cmd) in enumerate(zip(rows, commands, strict=True)):
            assert row['id'] == str(index+1) and row['receiver'] == MOVER and row['tick'] == 1+index//2, row
            near_vector(row['center_m'], [*offsets[index], .5])
            near_vector(row['normal'], [0, 0, 1])
            near_vector(row['tangent'], [1, 0, 0])
            near_vector(row['half_extent_m'], cmd['half_extent_m'])
            near_vector(row['linear_rgb'], cmd['linear_rgb'])
            for key in ('half_depth_m', 'minimum_normal_dot', 'edge_fraction', 'opacity', 'roughness'):
                assert type(row[key]) in (int, float), (key, row)
                assert math.isclose(row[key], cmd[key], rel_tol=0, abs_tol=1e-7), (key, row)

    def archive(self, client, saves, session, tick, request, slot, has_marks=True, *, structure=0):
        receipt = client.call('save.write', dict(request_id=uid(request), configuration_generation=1,
            slot=slot, expected_generation=0, session_id=session, expected_tick=tick,
            expected_gameplay_revision=0, expected_structure_revision=structure))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['tick'] == tick and ('surface_marks' in payload) is has_marks, payload
        self.measurements.setdefault('archives', {})[slot] = dict(tick=tick, bytes=len(raw), sha256=file_hash(path))
        return raw

    def reject(self, client, method, params, code):
        try:
            client.call(method, params)
        except RpcError as failure:
            assert failure.code == code, failure
            return dict(code=failure.code, message=str(failure))
        raise AssertionError(f'{method} accepted {params!r}')

    def refusal_checks(self, client, saves, session, checkpoint):
        valid, alternate = next_marks()
        stale = dict(session_id=session, tick=1, structure_revision=0)
        failures = self.measurements['refusals'] = {
            'stale_tick': self.reject(client, 'runtime.surface_marks.inspect', stale | dict(tick=0), -32009),
            'stale_structure': self.reject(client, 'runtime.surface_marks.inspect',
                                           stale | dict(structure_revision=1), -32009),
        }
        guard = dict(session_id=session, expected_tick=1, expected_structure_revision=0, ticks=2,
                     motions=[motion(MOVED)])
        cases = [
            ('ray_miss', [valid, alternate | dict(ray=ray(8, 1))], -32040),
            ('wrong_first_hit', [valid, alternate | dict(ray=ray(0, 1))], -32040),
            ('mismatched_geometry', [valid, alternate | dict(receiver=WRONG_SHAPE, ray=ray(2, 1))], -32040),
            ('receiver_capacity', [valid, alternate, valid], -32040),
            ('invalid_roughness', [valid | dict(roughness=.04)], -32602),
            ('request_capacity', [valid]*17, -32602),
        ]
        for index, (name, stamps, code) in enumerate(cases):
            params = guard | dict(request_id=uid(730+index), surface_marks=stamps)
            failure = failures[name] = self.reject(client, 'runtime.step', params, code)
            assert self.observe(client, session, 1, STAMPED) == checkpoint, failure
        for name in ('ray_miss', 'wrong_first_hit'):
            assert 'must first hit the expected receiver' in failures[name]['message'], failures[name]
        assert 'receiver capacity exceeded' in failures['receiver_capacity']['message'], failures['receiver_capacity']
        return self.archive(client, saves, session, 1, 737, 'after-refusals')

    def capture(self, client, session, tick, revision, label):
        position = {0: START, 1: STAMPED, 3: MOVED}[tick]
        center = [position[0]+FIRST_OFFSET[0], position[1], FRONT]
        samples = {
            'overlap': (center, True),
            'red_falloff': ([position[0]-.25, position[1], FRONT], True),
            'outside_ellipse': ([position[0]-.445, position[1]+.25, FRONT], True),
            'unmarked_neighbor': ([-.375, 1, FRONT], False),
        }
        if tick == 3:
            samples.update({
                'third_mark': ([position[0]+EXTRA_OFFSETS[0][0], position[1]+EXTRA_OFFSETS[0][1], FRONT], True),
                'fourth_mark': ([position[0]+EXTRA_OFFSETS[1][0], position[1]+EXTRA_OFFSETS[1][1], FRONT], True),
                'unmarked_old_center': ([STAMPED[0]+FIRST_OFFSET[0], position[1], FRONT], None),
            })
        camera_world = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *CAMERA_CENTER, 1]
        coordinates = {key: tuple(map(math.floor, projection(camera_world, LENS, point)))
                       for key, (point, _) in samples.items()}
        output = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(output), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in coordinates.values()]))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0, report
        assert report['nvrhi_errors'] == 0 and report['tick'] == tick and report['lens'] == LENS, report
        assert report['revision'] == revision and report['camera_world'] == camera_world, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        record = dict(tick=tick, gpu=report['gpu'], sha256=file_hash(output), probes={})
        self.measurements.setdefault('captures', {})[label] = record
        image = pixels(output)
        assert len(image) == height and all(len(row) == width for row in image)
        stamps = []
        if tick >= 1:
            stamps = first_marks()
        if tick == 3:
            dx = MOVED[0]-STAMPED[0]
            stamps = [stamp | dict(ray=ray(stamp['ray']['origin'][0]+dx, stamp['ray']['origin'][1]))
                      for stamp in stamps]+next_marks()
        for (name, (x, y)), probe in zip(coordinates.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            receiver = samples[name][1]
            point = world_position(x, y)
            item = record['probes'][name] = dict(hdr=probe['resolved_hdr'], bmp_rgb=image[y][x], pixel=[x, y],
                                               depth=probe['depth'], surface_valid=probe['surface_valid'])
            if receiver is None:
                assert probe['surface_valid'] is False, item
                continue
            assert probe['surface_valid'] and abs(probe['depth']-(1-1/4)*10/9) < 2e-6, item
            near_vector(probe['shading_normal'][:3], [0, 0, 1])
            base, roughness = surface(point, stamps if receiver else [])
            expected = radiance(point, base, roughness)
            item.update(expected_hdr=expected, expected_bmp=[display(n) for n in expected],
                        base=base, roughness=roughness)
            assert probe['raw_hdr'] == probe['resolved_hdr'], item
            near_vector(probe['resolved_hdr'], expected, tolerance=.002)
            assert max(abs(a-b) for a, b in zip(image[y][x], item['expected_bmp'], strict=True)) <= 2, item
            if tick == 1 and name == 'overlap':
                wrong_roughness = radiance(point, base, ROUGHNESS)
                wrong_order = radiance(point, *surface(point, stamps[::-1]))
                assert min(max(abs(a-b) for a, b in zip(expected, wrong, strict=True))
                           for wrong in (wrong_roughness, wrong_order)) > .02, item
            if tick == 1 and name == 'unmarked_neighbor':
                leaked = radiance(point, *surface(point, stamps))
                assert max(abs(a-b) for a, b in zip(expected, leaked, strict=True)) > .02, item
        return record

    def exercise(self, revision, game=None):
        saves = self.root/'External slots'
        saves.mkdir()
        snapshots = self.measurements['observations'] = {}
        with self.owner('warm marks', game=game, saves=saves) as (client, original):
            session = self.initialize(client, revision, uid(710), saves, game)
            assert self.inspect(client, session, 0) == []
            pre_move = client.call('runtime.raycast', dict(session_id=session, tick=0) | first_marks()[0]['ray'])
            assert pre_move['hit'] is None, pre_move
            self.measurements['pre_move_ray'] = pre_move
            self.archive(client, saves, session, 0, 711, 'unmarked', has_marks=False)
            if game:
                self.capture(client, session, 0, revision, 'unmarked')

            checkpoint = self.advance(client, session, 0, 712, STAMPED, first_marks(), retry=True)
            snapshots['checkpoint'] = checkpoint
            self.expected_rows(checkpoint)
            assert self.inspect(client, session, 1, entity=MOVER) == checkpoint
            assert self.inspect(client, session, 1, entity=NEIGHBOR) == []
            saved = self.archive(client, saves, session, 1, 713, 'checkpoint')
            assert self.refusal_checks(client, saves, session, checkpoint) == saved
            if game:
                checkpoint_capture = self.capture(client, session, 1, revision, 'checkpoint-warm')
            continued = self.advance(client, session, 1, 740, MOVED, next_marks(), ticks=2)
            snapshots['continued'] = continued
            assert continued[:2] == checkpoint
            self.expected_rows(continued)
            final = self.archive(client, saves, session, 3, 741, 'final')
            if game:
                final_capture = self.capture(client, session, 3, revision, 'final-warm')

        with self.owner('restored marks', game=game, saves=saves) as (client, restored):
            fresh = self.initialize(client, revision, uid(750), saves, game)
            session = uid(751)
            assert fresh != session and self.load(client, revision, fresh, session, 'checkpoint', 1, 752) == 0
            assert self.observe(client, session, 1, STAMPED) == checkpoint
            assert self.archive(client, saves, session, 1, 753, 'restored-checkpoint') == saved
            if game:
                assert self.capture(client, session, 1, revision, 'checkpoint-cold') == checkpoint_capture
            assert self.advance(client, session, 1, 754, MOVED, next_marks(), ticks=2) == continued
            assert self.archive(client, saves, session, 3, 755, 'restored-final') == final
            if game:
                assert self.capture(client, session, 3, revision, 'final-cold') == final_capture
        assert restored['pid'] != original['pid']

    def author_terrain_and_template(self, revision):
        recipe = dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
                      origin_m=[10, 1, -5], spacing_m=[1, 1],
                      heights_m=[.25*x+.5*z for z in range(3) for x in range(3)])
        raw = encode(recipe)
        plan = self.world_call('world.terrain.inspect', recipe=raw)
        tile, = plan['tiles']
        assert (tile['x'], tile['z'], tile['primitive'], tile['triangles']) == (0, 0, 0, 8), tile
        assert self.world_call('world.terrain.cook', recipe=raw, expected_recipe_sha256=plan['recipe_sha256']) == plan
        cooked = self.assets/(tile['asset']+'.pmodel')
        assert cooked.stat().st_size == tile['bytes'] and file_hash(cooked) == tile['asset'], tile
        part = dict(asset=tile['asset'], primitive=tile['primitive'])
        ops = [dict(op='entity.create', id=TERRAIN, name='Cooked triangle receiver'),
               component(TERRAIN, 'Transform', transform([0, 0, 0])),
               component(TERRAIN, 'StaticMesh', dict(part, visible=True)),
               component(TERRAIN, 'MeshCollider', dict(part, friction=.5, restitution=0)),
               dict(op='template.set', id=BOX_TEMPLATE, name='Temporary marked crate',
                    components=dict(Transform=transform([5, 1, -4.5]), MeshRenderer=DRAWABLE, PbrMaterial=MATERIAL,
                                    BoxCollider=dict(motion='kinematic', half_extents=[.5]*3, mass=1,
                                                     friction=.5, restitution=0)))]
        result = self.world_call('world.transact', request_id=uid(760), base_revision=revision, ops=ops)
        assert result['committed'] and result['revision'] == revision+1, result
        refs = self.world_call('world.asset.references', revision=revision+1)
        matching = {(edge['component'], edge['asset'], edge['subresource']['index'])
                    for edge in refs['edges'] if edge['owner']['id'] == TERRAIN}
        assert matching == {(name, tile['asset'], 0) for name in ('StaticMesh', 'MeshCollider')}, refs
        authored_tile = json.loads(self.world.read_bytes())['entities'][TERRAIN]['components']
        assert 'PbrMaterial' not in authored_tile, authored_tile
        return revision+1, tile

    def terrain_and_retirement(self, revision):
        revision, tile = self.author_terrain_and_template(revision)
        authored = tree_hashes(self.project)
        saves = self.root/'Terrain and retirement slots'
        saves.mkdir()
        metrics = self.measurements['terrain_and_retirement'] = dict(revision=revision, tile=tile)
        query = dict(origin=[10.25, 8, -4.375], direction=[0, -1, 0], distance=12)
        expected_point = [10.25, 1.375, -4.375]
        expected_normal = normalized((-.25, 1, -.5))
        normal_x = expected_normal[0]
        expected_tangent = normalized([1-normal_x**2, -normal_x*expected_normal[1], -normal_x*expected_normal[2]])
        terrain_stamp = dict(receiver=TERRAIN, ray=query, half_extent_m=[.25, .125], half_depth_m=.03125,
                             minimum_normal_dot=.75, edge_fraction=.5, linear_rgb=[.125, .875, .25],
                             opacity=.75, roughness=.5)
        with self.owner('cooked terrain mark', saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(761), saves, None)
            hit = client.call('runtime.raycast', dict(session_id=session, tick=0) | query)['hit']
            # In the first cell the point has z fraction .625 > x fraction .25.
            # It is strictly inside triangle 1, away from the shared diagonal.
            assert hit['entity'] == TERRAIN and hit['triangle'] == 1, hit
            near_vector(hit['position'], expected_point)
            near_vector(hit['normal'], expected_normal)
            metrics['triangle_hit'] = hit
            self.step(client, session, 0, 762, expected_structure_revision=0, surface_marks=[terrain_stamp])
            record, = self.inspect(client, session, 1, entity=TERRAIN)
            assert (record['id'], record['receiver'], record['tick']) == ('1', TERRAIN, 1), record
            near_vector(record['center_m'], expected_point)
            near_vector(record['normal'], expected_normal)
            near_vector(record['tangent'], expected_tangent)
            assert record['half_extent_m'] == [.25, .125] and record['roughness'] == .5, record
            metrics['terrain_mark'] = record
            checkpoint = self.archive(client, saves, session, 1, 763, 'terrain')

        with self.owner('restored terrain and crate retirement', saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(764), saves, None)
            session = uid(765)
            assert self.load(client, revision, previous, session, 'terrain', 1, 766) == 0
            assert self.inspect(client, session, 1) == [record]
            assert self.archive(client, saves, session, 1, 767, 'terrain-restored') == checkpoint

            spawned = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(768),
                expected_tick=1, expected_structure_revision=0, spawns=[dict(template_id=BOX_TEMPLATE)]))
            crate, = spawned['spawned']
            assert spawned['structure_revision'] == 1, spawned
            box_stamp = terrain_stamp | dict(receiver=crate, ray=ray(5, 1))
            self.step(client, session, 1, 769, expected_structure_revision=1, surface_marks=[box_stamp])
            retired, = self.inspect(client, session, 2, structure=1, entity=crate)
            assert (retired['id'], retired['receiver'], retired['tick']) == ('2', crate, 2), retired
            near_vector(retired['center_m'], [0, 0, .5])
            removed = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(770),
                expected_tick=2, expected_structure_revision=1, despawns=[crate]))
            assert removed['structure_revision'] == 2 and removed['despawned'] == [crate], removed
            assert self.inspect(client, session, 2, structure=2) == [record]
            assert self.inspect(client, session, 2, structure=2, entity=crate) == []
            self.step(client, session, 2, 771, expected_structure_revision=2, surface_marks=[terrain_stamp])
            before, after = self.inspect(client, session, 3, structure=2)
            assert before == record and after == (record | dict(id='3', tick=3)), (before, after)
            metrics['retirement'] = dict(removed_receiver=crate, removed_mark=retired['id'], next_mark=after['id'])
            final = self.archive(client, saves, session, 3, 772, 'after-retirement', structure=2)
            retained = json.loads(final)['snapshot']['payload']['surface_marks']
            assert retained['version'] == 1 and retained['next_id'] == 4, retained
            assert [row['id'] for row in retained['marks']] == [1, 3], retained
        assert warm['pid'] != cold['pid'] and tree_hashes(self.project) == authored

    def run(self):
        revision = self.measurements['authored_revision'] = self.author()
        catalog = self.world_call('world.describe', view='catalog')
        assert 'runtime.surface_marks.inspect' in catalog['methods'], catalog
        methods = self.world_call('world.describe', view='method', name='runtime.step')['methods']
        assert methods['runtime.step']['properties']['surface_marks']['maxItems'] == LIMITS['commands_per_step']
        if self.args.headless:
            initial = tree_hashes(self.project)
            self.exercise(revision)
            assert tree_hashes(self.project) == initial
            self.terrain_and_retirement(revision)
            return

        built = self.root/'Built Surface Marks'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(built),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Surface Marks', self.root/'Unavailable authoring project'
        built.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not built.exists()
        manifest = bundle/'game.json'
        result = success(json.loads(self.cli('game', 'inspect', self.native(manifest))))
        assert result['revision'] == revision, result
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(manifest), inventory=frozen,
                                           detached_source=authored)
        self.exercise(revision, game=manifest)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless', action='store_true', help='Check owner state and save continuation.')
    mode.add_argument('--runtime-root', type=Path, help='Installed Windows runtime for the relocated game.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='surface-marks-', dir=args.output.resolve()))
    test = SurfaceMarksCapture(args, root)
    result = dict(passed=False, headless=args.headless, gpu=None if args.headless else args.gpu,
                  lighting_path=None if args.headless else args.lighting_path,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
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
