#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify that an over-budget frame falls back to base geometry on all draws."""

import argparse
import json
from pathlib import Path
import tempfile
import traceback

from snow_material_displacement_capture import HEIGHT, PROBES, WIDTH, SnowDisplacement
from snow_compaction_contract import CAMERA, LIGHT, TILES, unit
from world_particles_contract import component, transform
from world_terrain_contract import encode, file_hash, uid


class BudgetFixture(SnowDisplacement):
    def run(self):
        revision = self.author()
        cells = 128
        recipe = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=cells + 1,
                            samples_z=cells + 1, tile_cells=cells, origin_m=[-1, -.4, -1],
                            spacing_m=[1 / 32] * 2, heights_m=[0] * (cells + 1)**2))
        planned = self.world_call('world.terrain.inspect', recipe=recipe)
        assert self.world_call('world.terrain.cook', recipe=recipe,
                               expected_recipe_sha256=planned['recipe_sha256']) == planned
        tile, = planned['tiles']
        assert tile['triangles'] == 32768, tile
        dense = uid(766)
        mesh = dict(asset=tile['asset'], primitive=tile['primitive'], visible=True)
        snow = json.loads(self.world.read_bytes())['entities']
        profiles = {entity: dict(snow[entity]['components']['StaticMesh']) for entity in TILES}
        for profile in profiles.values():
            profile['displacement']['height_m'] = .32
        # Light the height ramp from the side so the active surface changes
        # output pixels as well as the higher-precision scene products.
        light_aim = unit([-.8, .6, 0, 1])
        off = [component(entity, 'StaticMesh', {key: value for key, value in profile.items()
                                                if key != 'displacement'}) for entity, profile in profiles.items()]
        on = [component(entity, 'StaticMesh', profile) for entity, profile in profiles.items()]
        cases = [
            ('base', [dict(op='entity.create', id=dense, name='Dense terrain budget witness'),
                      component(dense, 'Transform', transform([0, 0, 0])),
                      component(dense, 'StaticMesh', mesh),
                      component(LIGHT, 'Transform', transform([0, 0, 0], light_aim)), *off]),
            ('active', on),
            ('over-budget', [component(dense, 'StaticMesh', dict(mesh, displacement=profiles[TILES[0]]['displacement']))]),
            ('recovered', [component(dense, 'StaticMesh', mesh)]),
        ]
        self.measurements['dense_triangles'] = tile['triangles']
        frames = self.measurements['budget_captures'] = {}
        with self.owner('budget') as (client, _):
            for request, (label, operations) in enumerate(cases, 770):
                reply = client.call('world.transact', dict(request_id=uid(request), base_revision=revision,
                    ops=operations))
                assert reply['committed'] and reply['revision'] == revision + 1, reply
                revision = reply['revision']
                image = self.root / (label + '.bmp')
                receipt = client.call('world.capture', dict(revision=revision, camera=CAMERA,
                    path=self.native(image), width=WIDTH, height=HEIGHT, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
                    reconstruction='none', samples=1, capture_frames=2,
                    scene_product_probes=[dict(x=x, y=y) for x, y in PROBES]), timeout=90)
                assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] > 0, receipt
                assert receipt['nvrhi_errors'] == 0, receipt
                frames[label] = dict(sha256=file_hash(image), report=receipt)
        base, active, fallback, recovered = (frames[label] for label, _ in cases)
        assert base['sha256'] == fallback['sha256'] != active['sha256'] == recovered['sha256']
        renders = [frame['report']['render_diagnostics'] for frame in (base, active, fallback, recovered)]
        counts = [render['last_draws'] for render in renders]
        assert [row['displacement_requested'] for row in counts] == [0, 2, 3, 2], counts
        assert [row['displacement_budget_fallback'] for row in counts] == [False, False, True, False], counts
        assert all(row['shadow_draws'] > 0 for row in counts), counts
        assert counts[2]['displacement_draws'] == counts[2]['displacement_patches'] == counts[2]['displacement_segment_cap'] == 0
        assert counts[1]['displacement_patches'] == counts[3]['displacement_patches'] > 8192
        probes = [render['scene_products']['probes'] for render in renders]
        assert probes[0] == probes[2] and probes[1] == probes[3] and probes[0] != probes[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.set_defaults(headless=False)
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Use assertions and a GPU index from 0 to 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='displacement-budget-', dir=args.output.resolve()))
    fixture = BudgetFixture(args, root)
    result = dict(format='poima.material-displacement-budget.v1', passed=False, calls=fixture.calls,
                  measurements=fixture.measurements, executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)))
    try:
        fixture.run()
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
