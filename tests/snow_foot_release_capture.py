#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check visible snow compaction from actual foot releases in a relocated Vulkan game."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from runtime_ground_contacts import RIG
from snow_compaction_contract import CAMERA, TILES, Lattice, near, unit, vector_near
from snow_foot_release_contract import CHECKPOINT_HELD, CHECKPOINT_READY, ReleasedSoleContract, intents
from world_particles_contract import component, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid


EYE = [2.65, 2.65, 1.65]
FOCUS = [1.02, .43, .53]
LENS = dict(vertical_fov=49, near=.1, far=10)
# These sites remain clear of the original armored figure in both source poses.
# Each rim lies inside one foot's affected disc and outside the other one.
RIMS = [('east rim', 1.48, .48), ('west rim', .64, .78), ('untouched', 1.82, .48)]
EQUIPMENT_VARIANTS = {0, 1, 2, 3, 4, 10, 11}
TORSO = [1.03, .55, .54]
DEPTH_TOLERANCE = 2e-6


def camera_rotation():
    dx, dy, dz = unit(tuple(b-a for a, b in zip(EYE, FOCUS, strict=True)))
    pitch = math.asin(dy)/2
    yaw = math.atan2(-dx, -dz)/2
    return [math.cos(yaw)*math.sin(pitch), math.sin(yaw)*math.cos(pitch),
            -math.sin(yaw)*math.sin(pitch), math.cos(yaw)*math.cos(pitch)]


def pixel_ray(camera, pixel):
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    u, v = pixel
    screen_x = (u+.5-width/2)/focal
    screen_y = (height/2-v-.5)/focal
    return [camera[k]*screen_x+camera[4+k]*screen_y-camera[8+k] for k in range(3)]


def floor_intersection(origin, direction, lattice):
    """Intersect the pixel ray with the independently interpolated lattice."""

    def sample(distance, smooth=False):
        position = [p+distance*d for p, d in zip(origin, direction, strict=True)]
        x, _, z = position
        tile = TILES[int(x >= 1)]
        y, normal, triangle = lattice.surface(x, z, tile, smooth=smooth)
        return position, y, normal, triangle, tile

    assert direction[1] < -.1, direction
    low = (.0625-origin[1])/direction[1]
    high = (-.0625-origin[1])/direction[1]
    for _ in range(27):
        distance = (low+high)/2
        position, height, _, _, _ = sample(distance)
        if position[1] > height:
            low = distance
        else:
            high = distance
    distance = (low+high)/2
    point, height, smooth_normal, triangle, tile = sample(distance, True)
    _, _, geometric_normal, _, _ = sample(distance)
    near(point[1], height, 1e-8)
    return distance, point, smooth_normal, geometric_normal, triangle, tile


def device_depth(view_distance):
    near_plane, far_plane = LENS['near'], LENS['far']
    return far_plane/(far_plane-near_plane)*(1-near_plane/view_distance)


def view_distance(depth):
    near_plane, far_plane = LENS['near'], LENS['far']
    return near_plane/(1-depth*(far_plane-near_plane)/far_plane)


class FootReleaseCapture(ReleasedSoleContract):
    def author_views(self, revision):
        authored = json.loads(self.world.read_bytes())['entities']
        asset = self.measurements['knight']['asset']
        provenance_record = dict(format='poima.asset-provenance', version=1, asset=asset, kind='model',
                                 title='KayKit Adventurers 1.0 Knight', creator='Kay Lousberg',
                                 license=dict(identifier='CC0-1.0', notice=self.args.license.read_text(encoding='utf-8')))
        provenance = self.world_call('asset.provenance.create', record=provenance_record)['record']
        operations = [component(CAMERA, 'Transform', transform(EYE, camera_rotation())),
                      component(CAMERA, 'Camera', LENS),
                      dict(op='asset.provenance.set', asset=asset, records=[provenance])]
        hidden = []
        for entity, item in authored.items():
            drawable = item['components'].get('StaticMesh')
            parent = authored.get(item['parent'])
            node = parent and parent['components'].get('RigNode')
            if drawable and node and node['rig'] == RIG and node['node'] in EQUIPMENT_VARIANTS:
                # Keep the original body, cape, helmet and rig. The alternate
                # shields and weapons would overlap the exposed rim witnesses.
                operations.append(component(entity, 'StaticMesh', drawable | dict(visible=False)))
                hidden.append(node['node'])
        assert set(hidden) == EQUIPMENT_VARIANTS, hidden
        result = self.world_call('world.transact', request_id=uid(800), base_revision=revision, ops=operations)
        assert result['committed'] and result['revision'] == revision+1, result
        self.measurements['view'] = dict(eye_m=EYE, focus_m=FOCUS, lens=LENS, hidden_equipment_nodes=sorted(hidden))
        self.measurements['knight']['provenance_sha256'] = provenance
        return result['revision']

    def capture_release(self, client, session, tick, lattice, label):
        camera = client.call('runtime.entity', dict(session_id=session, tick=tick, id=CAMERA))['world_matrix']
        origin = camera[12:15]
        vector_near(origin, EYE, 1e-8)
        positions = {}
        base = Lattice()
        for name, x, z in RIMS:
            y, _, _ = base.surface(x, z, TILES[int(x >= 1)])
            positions[name] = [x, y, z]
        positions['visible knight'] = TORSO
        pixels = {name: tuple(map(math.floor, projection(camera, LENS, position)))
                  for name, position in positions.items()}
        assert len(set(pixels.values())) == len(positions), pixels
        width, height = CAPTURE_SIZE
        assert all(0 <= x < width and 0 <= y < height for x, y in pixels.values()), pixels
        path = self.root/(label+'.bmp')
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in pixels.values()]))
        assert receipt['hardware'] is True and receipt['capture_written'] is True and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera and receipt['nvrhi_errors'] == 0, receipt
        diagnostics = receipt['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        draws = diagnostics['last_draws']
        assert draws['skinned_instances'] > 0 and draws['skinned_vertices'] > 0, draws
        assert draws['streamed_draws'] == 2, draws
        probes = {}
        for (name, pixel), row in zip(pixels.items(), diagnostics['scene_products']['probes'], strict=True):
            assert (row['x'], row['y']) == pixel and row['surface_valid'] is True, (name, row, pixel)
            direction = pixel_ray(camera, pixel)
            distance, point, smooth, geometric, triangle, tile = floor_intersection(origin, direction, lattice)
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick, origin=origin,
                direction=unit(direction), distance=LENS['far']*math.hypot(*direction)))['hit']
            assert hit['entity'] == tile and hit['triangle'] == triangle, (name, hit, tile, triangle)
            vector_near(hit['position'], point, 3e-5)
            vector_near(hit['normal'], geometric, .002)
            actual_distance = view_distance(row['depth'])
            if name == 'visible knight':
                # Character meshes are presentation-only in this fixture. A
                # nearer rendered surface with a snow collider behind it
                # establishes that the actor is visible in the same image.
                assert actual_distance < distance-.18, (row, actual_distance, distance)
            else:
                near(row['depth'], device_depth(distance), DEPTH_TOLERANCE)
                vector_near(row['shading_normal'][:3], smooth, .004)
            probes[name] = dict(raster=row, collider=hit, expected_height_m=point[1],
                                expected_ground_camera_depth_m=distance, observed_camera_depth_m=actual_distance,
                                expected_ground_normal=smooth)
        capture = dict(sha256=file_hash(path), probes=probes, lighting_path=self.args.lighting_path,
                       gpu=receipt['gpu'], draws=draws)
        self.measurements.setdefault('captures', {})[label] = dict(path=str(path), receipt=receipt) | capture
        return capture

    @staticmethod
    def terrain_depth(capture, name):
        return capture['probes'][name]['raster']['depth']

    def verify_changes(self, held, ready, first, both):
        for name, _, _ in RIMS:
            near(self.terrain_depth(held, name), self.terrain_depth(ready, name), DEPTH_TOLERANCE)
        assert held['sha256'] != ready['sha256']
        for name, earlier, later in [('east rim', ready, first), ('west rim', first, both)]:
            a, b = self.terrain_depth(earlier, name), self.terrain_depth(later, name)
            assert b-a > 2e-5, (name, a, b)
            assert later['probes'][name]['collider']['position'][1] < earlier['probes'][name]['collider']['position'][1]
        for name, earlier, later in [('west rim', ready, first), ('east rim', first, both)]:
            near(self.terrain_depth(earlier, name), self.terrain_depth(later, name), DEPTH_TOLERANCE)
        untouched = self.terrain_depth(held, 'untouched')
        for capture in (ready, first, both):
            near(self.terrain_depth(capture, 'untouched'), untouched, DEPTH_TOLERANCE)

    def consume_ready(self, client, session, lattice, queued, label, lifted):
        captures = []
        for index, event in enumerate(queued):
            before = CHECKPOINT_READY+index
            self.plain_step(client, session, before, 823+index, retry=True)
            self.automatic_stamp(lattice, event)
            tick = before+1
            self.observe(client, session, tick, lattice, intents(ready=queued[index+1:], applied=index+1),
                         f'{label} automatic at tick {tick}')
            states = self.states(client, session, tick)
            for current, original in zip(states, lifted, strict=True):
                assert current['planting']['status'] == 'lifted', current
                assert exact(current['grounding']['input_sole'], original['grounding']['input_sole']), (current, original)
            captures.append(self.capture_release(client, session, tick, lattice, f'{label} automatic {index+1}'))
        return captures

    def confirm_relocated(self, revision, game):
        saves = self.root/'Game saves'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('warm Windows game', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(820), saves, game)
            self.plain_step(client, session, 0, 821)
            planted = self.states(client, session, 1)
            holding = self.acquire_oracle(planted, lattice, 1)
            self.plain_step(client, session, 1, 822)
            held = self.states(client, session, CHECKPOINT_HELD)
            for before, after in zip(planted, held, strict=True):
                assert after['planting']['status'] == 'locked', after
                assert exact(before['planting']['anchor'], after['planting']['anchor']), (before, after)
            self.observe(client, session, CHECKPOINT_HELD, lattice, intents(holding=holding), 'render held')
            held_frame = self.capture_release(client, session, CHECKPOINT_HELD, lattice, 'warm held')
            lifted, queued, ready_observation = self.lift(client, session, 'warm', holding, lattice)
            for before, after in zip(held, lifted, strict=True):
                assert after['grounding']['input_sole'][1]-before['planting']['target_sole'][1] > .15, (before, after)
            ready_frame = self.capture_release(client, session, CHECKPOINT_READY, lattice, 'warm ready')
            ready = self.archive_release(client, saves, session, CHECKPOINT_READY, 826, 'render-ready', lattice,
                                         intents(ready=queued))
            first, both = self.consume_ready(client, session, lattice, queued, 'warm', lifted)
            self.verify_changes(held_frame, ready_frame, first, both)
            final_tick = CHECKPOINT_READY+len(queued)
            final = self.archive_release(client, saves, session, final_tick, 827, 'render-final', lattice,
                                         intents(applied=2))

        lattice = Lattice()
        with self.owner('cold Windows game', game=game, saves=saves) as (client, cold):
            fresh = self.initialize(client, revision, uid(830), saves, game)
            session = uid(831)
            assert self.load(client, revision, fresh, session, 'render-ready', CHECKPOINT_READY, 832) == 0
            assert exact(lifted, self.states(client, session, CHECKPOINT_READY))
            observed = self.observe(client, session, CHECKPOINT_READY, lattice, intents(ready=queued), 'cold ready')
            assert exact(ready_observation, observed)
            assert ready == self.archive_release(client, saves, session, CHECKPOINT_READY, 833,
                                                  'restored-ready', lattice, intents(ready=queued))
            assert exact(ready_frame, self.capture_release(client, session, CHECKPOINT_READY, lattice, 'cold ready'))
            assert exact([first, both], self.consume_ready(client, session, lattice, queued, 'cold', lifted))
            assert final == self.archive_release(client, saves, session, final_tick, 834,
                                                  'restored-final', lattice, intents(applied=2))
        assert warm['pid'] != cold['pid']
        self.measurements['pose'] = dict(planted=planted, held=held, lifted=lifted)

    def run(self):
        revision = self.author_views(self.enable_foot_release(self.author_knight()))
        initial = self.root/'Original bundle'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(initial),
                                    '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated foot release game', self.root/'Unavailable authored project'
        initial.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not initial.exists()
        game = bundle/'game.json'
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        assets = [tile['asset'] for tile in self.measurements['terrain']['tiles']]
        assets.append(self.measurements['knight']['asset'])
        for asset in assets:
            assert any(Path(name).name == asset+'.pmodel' and digest == asset for name, digest in frozen.items())
        assert self.measurements['knight']['provenance_sha256'] in frozen.values()
        assert 'Kay Lousberg' in (bundle/'content'/'ASSET_CREDITS.txt').read_text(encoding='utf-8')
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=frozen, detached_source=authored)
        self.confirm_relocated(revision, game)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


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
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-foot-release-render-', dir=args.output.resolve()))
    contract = FootReleaseCapture(args, root)
    result = dict(format='poima.snow-foot-release-render-contract.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)),
                  headless_fixture_sha256=file_hash(Path(__file__).with_name('snow_foot_release_contract.py')),
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
