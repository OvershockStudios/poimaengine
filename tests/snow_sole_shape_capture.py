#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare shaped snow pixels and collision in a relocated Vulkan game."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from character_library_intake import LibraryOracle
from input_actions_gameplay_contract import exact
from snow_compaction_contract import HEIGHT_UNIT, SPACING, TILES, TOP_DOWN, Lattice, near, unit, vector_near
from snow_foot_release_capture import FootReleaseCapture, floor_intersection
from snow_foot_release_contract import ACTOR_POSITION, CHECKPOINT_HELD, CHECKPOINT_READY, intents, released
from snow_sole_shape_contract import DIRECTIONS, SoleLattice, SoleShapeContract, validate_reference
from world_particles_contract import component, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, uid


OVERHEAD = uid(414)
GRAZING_FILL = uid(415)
EYE = [1, 2, .5]
# Cut away the lifted actor above the snow. The same accepted poses are also
# captured with the ordinary camera, where the whole actor remains visible.
LENS = dict(vertical_fov=35, near=1.94, far=4)
RASTER_DISTANCE_TOLERANCE_M = 1e-5
FILL_POSITION = [.38, .43, .15]
FILL_LIGHT = dict(kind='point', color=[1, .91, .80], intensity=6, range=2.5, enabled=True)


def pixel_ray(camera, pixel):
    width, height = CAPTURE_SIZE
    focal = height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    x, y = pixel
    u, v = (x + .5 - width / 2) / focal, (height / 2 - y - .5) / focal
    return [camera[i] * u + camera[4 + i] * v - camera[8 + i] for i in range(3)]


def device_depth(distance):
    near_plane, far_plane = LENS['near'], LENS['far']
    return far_plane / (far_plane - near_plane) * (1 - near_plane / distance)


def view_distance(depth):
    near_plane, far_plane = LENS['near'], LENS['far']
    return near_plane / (1 - depth * (far_plane - near_plane) / far_plane)


def clear_of_triangle_edges(point):
    u, v = ((p / SPACING) % 1 for p in (point[0], point[2]))
    return min(u, 1-u, v, 1-v, abs(u-v)) > .035


def exposed_witnesses(camera, queue):
    """Pick raster witnesses from the reference alone, before any capture."""
    baseline = Lattice()
    witnesses = {}
    for index, event in enumerate(queue):
        cx, cz = (value * HEIGHT_UNIT for value in event['center_units'])
        profiles = [SoleLattice() for _ in range(3)]
        for lattice, offset in zip(profiles, (0, 128, 64), strict=True):
            lattice.shaped_stamp(event | dict(heading_bin=(event['heading_bin'] + offset) % 256))
        center_on_snow = [cx, baseline.surface(cx, cz, TILES[int(cx >= 1)])[0], cz]
        px, py = map(math.floor, projection(camera, LENS, center_on_snow))
        fx, fz = DIRECTIONS[event['heading_bin']]
        scores = dict(toe=[], heel=[], cross_axis=[])
        for y in range(py-23, py+24):
            for x in range(px-23, px+24):
                pixel = x, y
                ray = pixel_ray(camera, pixel)
                before, point, *_ = floor_intersection(camera[12:15], ray, baseline)
                actual, contact, *_ = floor_intersection(camera[12:15], ray, profiles[0])
                if not (clear_of_triangle_edges(point) and clear_of_triangle_edges(contact)):
                    continue
                reverse, *_ = floor_intersection(camera[12:15], ray, profiles[1])
                quarter, *_ = floor_intersection(camera[12:15], ray, profiles[2])
                ahead = fx * (point[0] - cx) + fz * (point[2] - cz)
                if ahead > 0 and actual - before > .006:
                    scores['toe'].append((actual - reverse, pixel, reverse))
                if ahead < 0:
                    scores['heel'].append((reverse - actual, pixel, reverse))
                if actual - before > .006:
                    scores['cross_axis'].append((actual - quarter, pixel, quarter))
        for kind, candidates in scores.items():
            delta, pixel, alternative = max(row for row in candidates if all(
                math.dist(row[1], existing['pixel']) >= 3 for existing in witnesses.values()))
            assert delta > .004, (kind, event, delta, pixel)
            witnesses[f'print {index + 1} {kind}'] = dict(pixel=pixel, applied_index=index+1,
                alternative_m=alternative, nearest_wrong_delta_m=delta,
                alternative_heading_offset=64 if kind == 'cross_axis' else 128)
    witnesses['undisturbed'] = dict(pixel=tuple(map(math.floor,
        projection(camera, LENS, [1.81, baseline.surface(1.81, .82, TILES[1])[0], .82]))))
    return witnesses


class SoleShapeCapture(FootReleaseCapture, SoleShapeContract):
    def enable_foot_release(self, revision):
        return self.bind_shape(revision, 0, ACTOR_POSITION)

    def author_views(self, revision):
        revision = super().author_views(revision)
        result = self.world_call('world.transact', request_id=uid(801), base_revision=revision, ops=[
            dict(op='entity.create', id=OVERHEAD, name='Snow geometry qualification view'),
            component(OVERHEAD, 'Transform', transform(EYE, TOP_DOWN)),
            component(OVERHEAD, 'Camera', LENS),
            # The actor still casts a shadow when clipped by the overhead lens.
            # An unshadowed fill reveals the slope without changing that caster.
            dict(op='entity.create', id=GRAZING_FILL, name='Grazing snow fill'),
            component(GRAZING_FILL, 'Transform', transform(FILL_POSITION)),
            component(GRAZING_FILL, 'Light', FILL_LIGHT)])
        assert result['committed'] and result['revision'] == revision+1, result
        self.measurements['overhead_view'] = dict(eye_m=EYE, lens=LENS)
        self.measurements['grazing_fill'] = dict(position_m=FILL_POSITION, light=FILL_LIGHT)
        return result['revision']

    def capture_shape(self, client, session, tick, lattice, witnesses, label):
        camera = client.call('runtime.entity', dict(session_id=session, tick=tick, id=OVERHEAD))['world_matrix']
        origin = camera[12:15]
        vector_near(origin, EYE, 1e-8)
        path = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=OVERHEAD, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for (x, y) in (w['pixel'] for w in witnesses.values())]))
        assert receipt['hardware'] is True and receipt['capture_written'] is True and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera and receipt['nvrhi_errors'] == 0, receipt
        diagnostics = receipt['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        draws = diagnostics['last_draws']
        assert draws['streamed_draws'] == 2, draws
        probes = {}
        for (name, witness), row in zip(witnesses.items(), diagnostics['scene_products']['probes'], strict=True):
            pixel = witness['pixel']
            assert (row['x'], row['y']) == pixel and row['surface_valid'] is True, (name, row, pixel)
            direction = pixel_ray(camera, pixel)
            distance, point, smooth, geometric, triangle, tile = floor_intersection(origin, direction, lattice)
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick, origin=origin,
                              direction=unit(direction), distance=LENS['far']*math.hypot(*direction)))['hit']
            assert hit['entity'] == tile and hit['triangle'] == triangle, (name, hit, tile, triangle)
            vector_near(hit['position'], point, 3e-5)
            vector_near(hit['normal'], geometric, .002)
            # A near plane just above the snow gives depth a different scale
            # from the actor view. Compare the physical camera-depth error.
            actual_distance = view_distance(row['depth'])
            near(actual_distance, distance, RASTER_DISTANCE_TOLERANCE_M)
            vector_near(row['shading_normal'][:3], smooth, .004)
            if lattice.generation == witness.get('applied_index'):
                wrong_depth = device_depth(witness['alternative_m'])
                assert abs(row['depth'] - wrong_depth) > .001, (name, row, witness)
            probes[name] = dict(raster=row, collider=hit, expected_height_m=point[1],
                                expected_depth=device_depth(distance), expected_shading_normal=smooth,
                                expected_camera_depth_m=distance, observed_camera_depth_m=actual_distance)
        capture = dict(sha256=file_hash(path), probes=probes, lighting_path=self.args.lighting_path,
                       gpu=receipt['gpu'], draws=draws)
        self.measurements.setdefault('captures', {})[label] = dict(path=str(path), receipt=receipt) | capture
        return capture

    def settle(self, client, session, queue, witnesses, label, saves, lifted):
        lattice = SoleLattice()
        self.inspect_shape(client, session, CHECKPOINT_READY, lattice, intents(ready=queue), label+' ready')
        frames = [self.capture_shape(client, session, CHECKPOINT_READY, lattice, witnesses, label+' ready')]
        for index, event in enumerate(queue):
            before = CHECKPOINT_READY+index
            self.plain_step(client, session, before, 874+index, retry=True)
            assert lattice.shaped_stamp(event) > 0
            tick = before+1
            self.inspect_shape(client, session, tick, lattice, intents(ready=queue[index+1:], applied=index+1),
                               f'{label} impression {index+1}')
            states = self.states(client, session, tick)
            for current, original in zip(states, lifted, strict=True):
                assert current['planting']['status'] == 'lifted' and current['planting']['anchor'] is None, current
                assert exact(current['grounding']['input_sole'], original['grounding']['input_sole']), (current, original)
            frames.append(self.capture_shape(client, session, tick, lattice, witnesses, f'{label} impression {index+1}'))
        final_tick = CHECKPOINT_READY+len(queue)
        frames.append(self.capture_release(client, session, final_tick, lattice, label+' final actor'))
        archive = self.archive_shape(client, saves, session, final_tick, 878, label+'-final', lattice, intents(applied=2))
        return frames, archive

    def confirm_relocated(self, revision, game):
        saves = self.root/'Game saves'
        saves.mkdir()
        lattice = SoleLattice()
        source = LibraryOracle(self.args.source.read_bytes()).pose(36, 0)[2]
        with self.owner('warm Windows shaped game', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(870), saves, game)
            self.plain_step(client, session, 0, 871)
            planted = self.states(client, session, 1)
            holding = self.verify_anchors(planted, lattice, 0, ACTOR_POSITION, source)
            self.plain_step(client, session, 1, 872)
            held = self.states(client, session, CHECKPOINT_HELD)
            assert exact([r['planting']['anchor'] for r in planted], [r['planting']['anchor'] for r in held])
            self.inspect_shape(client, session, CHECKPOINT_HELD, lattice, intents(holding=holding), 'warm held')
            self.capture_release(client, session, CHECKPOINT_HELD, lattice, 'warm held actor')
            lifted = self.release_pose(client, session)
            queue = released(holding, CHECKPOINT_READY)
            camera = client.call('runtime.entity', dict(session_id=session, tick=CHECKPOINT_READY, id=OVERHEAD))['world_matrix']
            witnesses = exposed_witnesses(camera, queue)
            self.measurements['witnesses'] = witnesses
            ready = self.archive_shape(client, saves, session, CHECKPOINT_READY, 876, 'render-ready', lattice,
                                       intents(ready=queue))
            frames, final = self.settle(client, session, queue, witnesses, 'warm', saves, lifted)

        with self.owner('cold Windows shaped game', game=game, saves=saves) as (client, cold):
            fresh = self.initialize(client, revision, uid(880), saves, game)
            session = uid(881)
            assert self.load(client, revision, fresh, session, 'render-ready', CHECKPOINT_READY, 882) == 0
            assert exact(lifted, self.states(client, session, CHECKPOINT_READY))
            assert ready == self.archive_shape(client, saves, session, CHECKPOINT_READY, 883, 'restored-ready', lattice,
                                               intents(ready=queue))
            cold_frames, cold_final = self.settle(client, session, queue, witnesses, 'cold', saves, lifted)
            assert exact(frames, cold_frames) and final == cold_final
        assert warm['pid'] != cold['pid']
        image_hashes = [frame['sha256'] for frame in frames[:3]]
        assert len(set(image_hashes)) == 3, image_hashes
        for index in range(1, 3):
            before, after = frames[index-1:index+1]
            neighborhoods = [name for name, row in witnesses.items() if row.get('applied_index') == index]
            hdr_change = max(math.dist(before['probes'][name]['raster']['raw_hdr'],
                                       after['probes'][name]['raster']['raw_hdr'])
                             for name in neighborhoods)
            assert hdr_change > .005, (index, hdr_change)
            self.measurements.setdefault('print_hdr_changes', []).append(hdr_change)
        self.measurements['pose'] = dict(planted=planted, held=held, lifted=lifted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Windows authoring and runtime executable.')
    parser.add_argument('--runtime-root', type=Path, required=True, help='Installed Windows runtime for export.')
    parser.add_argument('--source', type=Path, required=True, help='Original Knight.glb.')
    parser.add_argument('--license', type=Path, required=True, help='Exact CC0 notice accompanying the Knight.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    validate_reference()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-sole-shape-render-', dir=args.output.resolve()))
    contract = SoleShapeCapture(args, root)
    result = dict(format='poima.snow-sole-shape-render-contract.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)), reference_sha256={name: file_hash(Path(__file__).with_name(name))
                      for name in ('snow_sole_shape_contract.py', 'snow_foot_release_capture.py')},
                  calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
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
