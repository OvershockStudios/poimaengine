#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored height detail, motion and collision on a mutable snow seam."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from material_displacement_capture import linear_height
from snow_compaction_contract import (
    CAMERA, CAMERA_POSITION, CELLS, CENTER, EAST, LENS, REGULAR_DEPTH, REGULAR_RADIUS,
    SnowContract, TILES, WEST, Lattice, near,
)
from snow_shadow_contract import LAMP, RECEIVER, SnowShadowContract, lamp
from texture_fixture import png
from world_particles_contract import component
from world_terrain_contract import error, file_hash, rpc, uid


WIDTH, HEIGHT = 481, 321
FOCAL = HEIGHT / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
RELIEF_M = .07
PROBES = [(WIDTH // 2 + x, y) for y in (78, 83, 90) for x in (-7, -2, -1, 0, 1, 2, 7)]


def world_point(pixel, distance):
    x, y = pixel
    return (CAMERA_POSITION[0] + (x + .5 - WIDTH / 2) * distance / FOCAL,
            CAMERA_POSITION[2] + (y + .5 - HEIGHT / 2) * distance / FOCAL)


def visual_height(lattice, x, z):
    physical, _, _ = lattice.surface(x, z, TILES[int(x >= 1)])
    return physical + RELIEF_M * linear_height(x)


def expected_distance(lattice, pixel):
    low, high = 1.8, 2.1
    for _ in range(27):
        distance = (low + high) / 2
        surface = visual_height(lattice, *world_point(pixel, distance))
        if CAMERA_POSITION[1] - distance > surface:
            low = distance
        else:
            high = distance
    return (low + high) / 2


class SnowDisplacement(SnowContract):
    def author(self):
        revision = super().author()
        source = self.root / 'Two height texels.png'
        source.write_bytes(png(2, 1, [0, 0, 0, 255, 255, 255, 255, 255]))
        asset = self.world_call('asset.image.import', source=self.native(source), color_space='linear')['asset']
        source.unlink()
        self.height_asset = asset
        profile = dict(texture=asset, axis='y', tile_mm=2000, height_m=RELIEF_M)
        original = self.world.read_bytes()
        entities = json.loads(original)['entities']

        def tile(entity, displacement):
            mesh = entities[entity]['components']['StaticMesh']
            return component(entity, 'StaticMesh', dict(mesh, displacement=displacement))

        rejected = []
        for request, operations in enumerate((
            [tile(WEST, profile)],
            [tile(WEST, profile), tile(EAST, dict(profile, height_m=.05))],
            [tile(entity, dict(profile, axis='z')) for entity in TILES],
        ), 740):
            reply, = self.world_calls(rpc('world.transact', request_id=uid(request), base_revision=revision,
                                          ops=operations))
            rejected.append(error(reply, -32602))
            assert self.world.read_bytes() == original
        self.measurements['profile_refusals'] = rejected
        result = self.world_call('world.transact', request_id=uid(743), base_revision=revision,
                                 ops=[tile(entity, profile) for entity in TILES])
        assert result['committed'] and result['revision'] == revision + 1, result
        edges = self.world_call('world.asset.references', asset=asset)['edges']
        assert {(edge['owner']['id'], edge['component'], edge['path']) for edge in edges} == {
            (entity, 'StaticMesh', '/displacement/texture') for entity in TILES
        }, edges
        dependency = self.world_call('world.dependencies')
        assert dependency['needs_material_displacement'] and dependency['needs_snow_material_displacement'], dependency
        self.measurements['height_profile'] = profile
        self.measurements['height_references'] = edges
        if not self.args.headless:
            self.reject_old_runtime()
        return result['revision']

    def reject_old_runtime(self):
        root = self.root / 'Runtime without snow displacement'
        root.mkdir()
        descriptor = json.loads((self.args.runtime_root / 'runtime.json').read_bytes())
        assert descriptor['features'].pop('snow_material_displacement') and descriptor['features']['material_displacement']
        (root / 'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
        output = self.root / 'Refused export'
        process = subprocess.run([str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                                  '--runtime', self.native(root), '--output', self.native(output)],
                                 capture_output=True, text=True, encoding='utf-8', timeout=90)
        result = json.loads(process.stdout)
        assert process.returncode != 0 and not output.exists(), result
        assert any('snow height displacement' in row['message'].lower()
                   for row in result['diagnostics']), result
        self.measurements['required_feature_refusal'] = result

    def capture(self, client, session, tick, label, lattice, reuse):
        path = self.root / (label + '.bmp')
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=WIDTH, height=HEIGHT, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=1,
            reuse_renderer=reuse, scene_product_probes=[dict(x=x, y=y) for x, y in PROBES]), timeout=90)
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['nvrhi_errors'] == 0, receipt
        render = receipt['render_diagnostics']
        assert render['lighting_path'] == self.args.lighting_path and render['material_displacement_supported'], render
        draws = render['last_draws']
        assert draws['streamed_draws'] == draws['displacement_requested'] == 2, draws
        assert draws['displacement_draws'] > 2, draws
        patches = draws['displacement_patches']
        assert patches == draws['displacement_draws'] * 2 * CELLS**2, draws
        assert not draws['displacement_budget_fallback'], draws
        expected_cap = min(16, math.isqrt((8192 * 16**2) // patches)) & ~1
        assert draws['displacement_segment_cap'] == expected_cap, draws
        near_plane, far_plane = LENS['near'], LENS['far']
        observations = []
        for pixel, sample in zip(PROBES, render['scene_products']['probes'], strict=True):
            assert (sample['x'], sample['y']) == pixel and sample['surface_valid'], sample
            distance = expected_distance(lattice, pixel)
            depth = far_plane / (far_plane - near_plane) * (1 - near_plane / distance)
            near(sample['depth'], depth, 6e-6)
            position = world_point(pixel, distance)
            assert RELIEF_M * linear_height(position[0]) > .015, position
            observations.append(dict(device_depth=depth, distance=distance, world_xz=position, observed=sample))
        stable = dict(sha256=file_hash(path), gpu=receipt['gpu'], observations=observations, patches=patches,
                      segment_cap=expected_cap)
        self.measurements.setdefault('captures', {})[label] = dict(report=receipt) | stable
        return stable

    def capture_snow(self, client, session, tick, label, expected):
        return self.capture(client, session, tick, label, expected, reuse=False)

    def exercise_snow(self, revision, game=None):
        super().exercise_snow(revision, game)
        if game is None:
            return
        assert self.height_asset in self.measurements['bundle']['inventory'].values()
        saves = self.root / 'Motion save slots'
        saves.mkdir()
        pristine, compacted = Lattice(), Lattice()
        with self.owner('motion', game=game, saves=saves) as (client, _):
            session = self.initialize(client, revision, uid(746), saves, game)
            previous = self.capture(client, session, 0, 'motion-first', pristine, reuse=True)
            stamp = self.stamp(client, session, 0, 0, REGULAR_RADIUS, REGULAR_DEPTH)
            assert compacted.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 0, 747, stamp)
            current = self.capture(client, session, 1, 'motion-stamped', compacted, reuse=True)
            captures = self.measurements['captures']
            assert not captures['motion-first']['report']['renderer_reused']
            assert captures['motion-stamped']['report']['renderer_reused']
            errors = []
            for first, second in zip(previous['observations'], current['observations'], strict=True):
                assert not first['observed']['motion_valid'] and second['observed']['motion_valid'], (first, second)
                before = CAMERA_POSITION[1] - visual_height(pristine, *second['world_xz'])
                ratio = second['distance'] / before - 1
                x, y = second['observed']['x'], second['observed']['y']
                expected = ((x + .5 - WIDTH / 2) / WIDTH * ratio, (y + .5 - HEIGHT / 2) / HEIGHT * ratio)
                assert abs(expected[1]) > 1e-4, expected
                for actual, target in zip(second['observed']['motion'], expected, strict=True):
                    near(actual, target, 12e-6)
                    errors.append(abs(actual - target))
            assert client.call('runtime.capture.release', dict(session_id=session))['released']
            self.measurements['motion_max_error_uv'] = max(errors)


class DisplacedSnowShadows(SnowShadowContract):
    def run(self):
        revision = self.add_test_light(self.author())
        enabled = self.world_call('world.transact', request_id=uid(755), base_revision=revision,
                                  ops=[component(LAMP, 'Light', lamp(True))])
        assert enabled['committed'], enabled
        _, base = self.shadow_pass(enabled['revision'], True, 'Base snow')

        source = self.root / 'Uniform height.png'
        source.write_bytes(png(1, 1, [255, 255, 255, 255]))
        white = self.world_call('asset.image.import', source=self.native(source), color_space='linear')['asset']
        source.unlink()
        profile = dict(texture=white, axis='y', tile_mm=2000, height_m=RELIEF_M)
        entities = json.loads(self.world.read_bytes())['entities']
        patches = [component(tile, 'StaticMesh', dict(entities[tile]['components']['StaticMesh'], displacement=profile))
                   for tile in TILES]
        dressed = self.world_call('world.transact', request_id=uid(756), base_revision=enabled['revision'], ops=patches)
        assert dressed['committed'], dressed
        _, raised = self.shadow_pass(dressed['revision'], True, 'Raised snow')
        disabled = self.world_call('world.transact', request_id=uid(757), base_revision=dressed['revision'],
                                   ops=[component(LAMP, 'Light', lamp(False))])
        assert disabled['committed'], disabled
        _, reference = self.shadow_pass(disabled['revision'], False, 'Unshadowed raised snow')
        ratios = {}
        for position in ('west', 'east'):
            hits = [capture[position]['native_ray_hit'] for capture in (base, raised, reference)]
            assert hits[0] == hits[1] == hits[2] and hits[0]['entity'] == RECEIVER, hits
            base_hdr, raised_hdr, reference_hdr = (capture[position]['sample']['raw_hdr']
                                                  for capture in (base, raised, reference))
            clear, shadowed = ([value / unshadowed for value, unshadowed in zip(rgb, reference_hdr, strict=True)]
                               for rgb in (base_hdr, raised_hdr))
            assert all(.9 <= value <= 1.01 for value in clear), clear
            assert all(0 <= value <= .2 for value in shadowed), shadowed
            ratios[position] = dict(base_over_reference=clear, raised_over_reference=shadowed)
        self.measurements['receiver_ratios'] = ratios


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless', action='store_true')
    mode.add_argument('--runtime-root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Use assertions and a GPU index from 0 to 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-height-', dir=args.output.resolve()))
    contract = SnowDisplacement(args, root)
    result = dict(format='poima.snow-material-displacement.v1', passed=False, headless=args.headless,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
        if not args.headless:
            shadow_root = root / 'Displaced shadow witness'
            shadow_root.mkdir()
            shadows = DisplacedSnowShadows(args, shadow_root)
            result['shadow_fixture'] = dict(calls=shadows.calls, measurements=shadows.measurements)
            shadows.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
