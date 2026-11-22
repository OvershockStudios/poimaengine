#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check wet and dry terrain in immutable authored Vulkan captures."""

import argparse
import json
import math
from pathlib import Path
import traceback

from scene_capture import pixels
from world_terrain_contract import (CAPTURE_LENS, CAPTURE_SIZE, file_hash,
                                    projection, rpc, success, tree_hashes, uid)
from world_water_contract import (CELLS, TILES, INDEX_BYTES_PER_QUAD,
                                  VERTEX_BYTES_PER_QUAD, WaterContract, bed,
                                  cell_center, component, reference)


CAMERA = uid(9109)
CAMERA_POSITION = (12, 14, -3)
HALF_PITCH = -math.pi / 4
CAMERA_MATRIX = (1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, *CAMERA_POSITION, 1)
SURFACE_Y = 6.5
VALIDATION_LAYER = 'Insert instance layer "VK_LAYER_KHRONOS_validation"'
VALIDATION_ERRORS = ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD', 'Undefined-Value')


def water_blue(rgb):
    r, g, b = rgb
    return b > 70 and b > r + 20 and b > g + 4


def probe_sites():
    sites = []
    for cell in CELLS:
        x, z = cell_center(cell)
        # Keep the rays through the outer cells on the terrain below the water plane.
        x = min(13.375, max(10.625, x))
        pixel = tuple(map(math.floor, projection(CAMERA_MATRIX, CAPTURE_LENS, [x, SURFACE_Y, z])))
        sites.append((cell, pixel))
    return sites


def device_depth(pixel, submerged):
    # With the known top-down camera, forward distance is also the Y drop.
    # At distance t, x=camera_x+u*t and z=camera_z+v*t. Substitute into
    # the fixture's own bed plane to find the dry terrain intersection.
    x, y = pixel
    width, height = CAPTURE_SIZE
    focal = height / (2 * math.tan(math.radians(CAPTURE_LENS['vertical_fov']) / 2))
    u = (x + .5 - width/2) / focal
    v = (y + .5 - height/2) / focal
    camera_x, camera_y, camera_z = CAMERA_POSITION
    if submerged:
        distance = camera_y - SURFACE_Y
    else:
        distance = (camera_y - bed(camera_x, camera_z)) / (1 - .25*u + .125*v)
    assert 10 < camera_x + u*distance < 14 and -4 < camera_z + v*distance < -2, pixel
    near, far = CAPTURE_LENS['near'], CAPTURE_LENS['far']
    return far / (far - near) * (1 - near / distance)


def footprint(image, wet):
    width, height = CAPTURE_SIZE
    assert len(image) == height and all(len(row) == width for row in image)
    left, top = projection(CAMERA_MATRIX, CAPTURE_LENS, (10, SURFACE_Y, -4))
    right, bottom = projection(CAMERA_MATRIX, CAPTURE_LENS, (12, SURFACE_Y, -2))
    inside = colored = 0
    mismatches = []
    for y, row in enumerate(image):
        for x, color in enumerate(row):
            border = min(x + .5 - left, right - x - .5, y + .5 - top, bottom - y - .5)
            blue = water_blue(color)
            inside += border > 1
            colored += blue
            missing_water = wet and border > 1 and not blue
            stray_water = blue and (not wet or border < -1)
            if (missing_water or stray_water) and len(mismatches) < 12:
                mismatches.append({'pixel': [x, y], 'color': color, 'distance_to_edge_px': border})
    result = {'projected_bounds_px': [left, top, right, bottom], 'interior_pixels': inside,
              'water_pixels': colored, 'mismatches': mismatches}
    assert inside > 7_000 and not mismatches, result
    return result


class AuthoredWaterCapture(WaterContract):
    def prepare(self):
        recipe, _, revision = self.arm_terrain()
        camera_transform = dict(position=CAMERA_POSITION, scale=[1, 1, 1],
                                rotation=[math.sin(HALF_PITCH), 0, 0, math.cos(HALF_PITCH)])
        ops = [dict(op='entity.create', id=CAMERA, name='Overhead water witness'),
               component(CAMERA, 'Transform', camera_transform), component(CAMERA, 'Camera', CAPTURE_LENS),
               component(uid(4), 'LightingEnvironment', dict(ambient=[1, 1, 1], exposure=1)),
               dict(op='component.remove', id=uid(5), type='Light'),
               dict(op='component.remove', id=uid(1), type='MeshRenderer')]
        for tile in TILES:
            ops.append(component(tile, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=[.12]*3,
                                 metallic=0, roughness=1, double_sided=False)))
        published = success(self.world_calls(self.transaction(revision, 9100, *ops))[0])
        assert published['committed'] and published['revision'] == revision + 1, published
        self.recipe = recipe
        return published['revision']

    def bind(self, revision, *, wet):
        depths = reference()[0]['depths'] if wet else [0] * len(CELLS)
        params = dict(revision=revision, terrain={'source': self.recipe.name},
                      expected_terrain_recipe_sha256=self.terrain['recipe_sha256'], tile_entities=TILES,
                      grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4,
                                relaxation_per_second=6), initial_depths_m=depths)
        plan = self.world_call('world.water.plan', **params)
        digest = plan['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=digest) == plan
        package = self.assets / (digest + '.pwater')
        assert file_hash(package) == digest
        payload = json.loads(package.read_bytes())
        assert payload['initial_depths_m'] == depths
        assert payload['floor_heights_m'] == [bed(*cell_center(cell)) for cell in CELLS]
        transaction = self.transaction(revision, 9101 + int(wet), dict(op='water.set', asset=digest))
        outcome = success(self.world_calls(transaction)[0])
        assert outcome['committed'] and outcome['revision'] == revision + 1, outcome
        return outcome['revision'], digest

    def request_capture(self, revision, destination):
        width, height = CAPTURE_SIZE
        return rpc('world.capture', revision=revision, camera=CAMERA, path=self.native(destination),
                   width=width, height=height, samples=1, capture_frames=2, gpu=self.args.gpu,
                   scene_product_probes=[{'x': x, 'y': y} for _, (x, y) in probe_sites()])

    def check_capture(self, revision, digest, *, wet):
        label = 'wet' if wet else 'dry'
        snapshot = tree_hashes(self.project)
        destination = self.root / f'{label}.bmp'
        receipt = self.cases(('capture', self.request_capture(revision, destination)), raw_world=True)['capture']
        stderr = self.calls[-1]['stderr']
        # Record raw outputs even if a later independent observation fails.
        item = self.measurements[label] = {'asset_sha256': digest, 'world_sha256': file_hash(self.world),
                                       'capture_sha256': file_hash(destination), 'receipt': receipt,
                                       'files_unchanged': tree_hashes(self.project) == snapshot,
                                       'validation_layer_loaded': VALIDATION_LAYER in stderr}
        assert item['files_unchanged'], label
        assert not any(marker in stderr for marker in VALIDATION_ERRORS), stderr
        if self.args.require_validation:
            assert item['validation_layer_loaded'], stderr
        assert receipt['source'] == 'authored' and receipt['tick'] is None and receipt['session_id'] is None, receipt
        assert receipt['camera'] == CAMERA and receipt['revision'] == revision and receipt['lens'] == CAPTURE_LENS
        assert all(abs(actual-expected) < 1e-12 for actual, expected in
                   zip(receipt['camera_world'], CAMERA_MATRIX, strict=True)), receipt
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] == 2
        assert receipt['nvrhi_errors'] == 0 and receipt['samples'] == 1
        assert (receipt['width'], receipt['height']) == CAPTURE_SIZE

        diagnostics = receipt['render_diagnostics']
        draws = diagnostics['last_draws']
        wet_cells = sum(cell[0] < 2 for cell in CELLS) if wet else 0
        assert receipt['object_count'] == draws['objects'] == draws['camera_draws'] == 2 + int(wet), draws
        assert draws['camera_triangles'] == 16 + 2 * wet_cells, draws
        assert draws['streamed_draws'] == int(wet), draws
        assert draws['streamed_vertex_bytes'] == wet_cells * VERTEX_BYTES_PER_QUAD, draws
        assert draws['streamed_index_bytes'] == wet_cells * INDEX_BYTES_PER_QUAD, draws
        scene_receipts = diagnostics['scene_receipts']
        assert scene_receipts['submitted'] == scene_receipts['presented'], scene_receipts
        assert scene_receipts['presented']['authored_revision'] == revision, scene_receipts
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products

        sample_depths = item['depth_probes'] = []
        for (cell, pixel), sample in zip(probe_sites(), products['probes'], strict=True):
            submerged = wet and cell[0] < 2
            expected = device_depth(pixel, submerged)
            row = dict(cell=cell, pixel=pixel, expected=expected, depth=sample['depth'], water=submerged)
            sample_depths.append(row)
            assert sample['surface_valid'] and (sample['x'], sample['y']) == pixel, sample
            assert abs(sample['depth'] - expected) < 3e-5, row
            if submerged:
                # The normal target stores half floats. Allow one unit-scale
                # quantum for normalization and readback of the flat plane.
                assert math.dist(sample['shading_normal'], (0, 1, 0)) <= 2**-10, sample
        item['footprint'] = footprint(pixels(destination), wet)
        return item

    def run(self):
        revision = self.prepare()
        if self.args.expect_legacy_error:
            revision, digest = self.bind(revision, wet=True)
            snapshot = tree_hashes(self.project)
            destination = self.root / 'legacy-wet.bmp'
            reply = self.cases(('legacy', self.request_capture(revision, destination), -32000), raw_world=True)['legacy']
            self.measurements['legacy'] = {'reply': reply, 'asset_sha256': digest, 'revision': revision}
            assert reply['message'] == 'World storage failure; inspect stderr.', reply
            assert 'map::at' in self.calls[-1]['stderr'], self.calls[-1]
            assert not destination.exists() and tree_hashes(self.project) == snapshot
            return

        revision, dry_asset = self.bind(revision, wet=False)
        dry_world = json.loads(self.world.read_bytes())
        dry = self.check_capture(revision, dry_asset, wet=False)
        revision, wet_asset = self.bind(revision, wet=True)
        wet_world = json.loads(self.world.read_bytes())
        assert dry_asset != wet_asset
        assert {k: v for k, v in dry_world.items() if k not in ('revision', 'receipts', 'water')} == {
                k: v for k, v in wet_world.items() if k not in ('revision', 'receipts', 'water')}
        wet = self.check_capture(revision, wet_asset, wet=True)
        assert dry['receipt']['gpu'] == wet['receipt']['gpu']
        for left, right in zip(dry['depth_probes'], wet['depth_probes'], strict=True):
            if right['water']:
                assert left['depth'] > right['depth'] + .001, (left, right)
            else:
                assert left['depth'] == right['depth'], (left, right)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory outside the world.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--gpu', type=int, help='Native Vulkan device index for both authored captures.')
    mode.add_argument('--expect-legacy-error', action='store_true', help='Replay only the pre-render 0.0.398 failure.')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if args.gpu is not None and not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    if args.require_validation and args.expect_legacy_error:
        parser.error('--require-validation requires --gpu.')
    if args.expect_legacy_error:
        args.gpu = 0

    output = args.output.resolve()
    output.mkdir()
    test = AuthoredWaterCapture(args, output)
    result = {'format': 'poima.water-authored-capture.v1', 'passed': False,
              'mode': 'legacy-error' if args.expect_legacy_error else 'vulkan',
              'binary_sha256': file_hash(args.binary), 'test_sha256': file_hash(Path(__file__)),
              'calls': test.calls, 'measurements': test.measurements}
    try:
        version = success(json.loads(test.cli('version')))
        result['binary'] = version
        if args.expect_legacy_error:
            assert version['version'] == '0.0.398', 'Legacy failure mode requires the previous released executable.'
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        (output / 'evidence.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(output / 'evidence.json')


if __name__ == '__main__':
    main()
