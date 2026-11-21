#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check separately authored snow and water in one native world and save."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from snow_compaction_contract import (
    BOX, CELLS, CENTER, DRAWABLE, HEIGHT, Lattice, MATERIAL, PLAIN, SPACING,
    THICKNESS, TILES, WIDTH, SnowContract, base, down,
)
from world_particles_contract import component, transform
from world_terrain_contract import encode, file_hash, read_inventory, success, uid
from world_water_contract import near


BED = [uid(501), uid(502)]
OVERLAP = [uid(503), uid(504)]
FLOOR = -.25
WATER_CELLS = [[0, 0], [1, 0]]
STAMP_RADIUS, STAMP_DEPTH = 768, 48


def snow_binding(tiles):
    return dict(op='snow.set', definition=dict(tiles=tiles, thickness_units=THICKNESS))


def without_session(report):
    return {key: value for key, value in report.items() if key != 'session_id'}


class SnowWaterContract(SnowContract):
    def terrain(self, client, origin, heights, entities, label):
        recipe = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=WIDTH, samples_z=HEIGHT,
                             tile_cells=CELLS, origin_m=origin, spacing_m=[SPACING, SPACING], heights_m=heights))
        plan = client.call('world.terrain.inspect', dict(recipe=recipe))
        assert client.call('world.terrain.cook', dict(recipe=recipe,
                           expected_recipe_sha256=plan['recipe_sha256'])) == plan
        ops = []
        for index, (entity, tile) in enumerate(zip(entities, plan['tiles'], strict=True)):
            assert (tile['x'], tile['z']) == (index, 0), plan
            part = dict(asset=tile['asset'], primitive=tile['primitive'])
            ops.extend([dict(op='entity.create', id=entity, name=f'{label} {index}'),
                        component(entity, 'StaticMesh', dict(part, visible=True)),
                        component(entity, 'MeshCollider', dict(part, friction=.5, restitution=0))])
        return recipe, plan, ops

    def transact(self, client, revision, request, *ops):
        receipt = client.call('world.transact', dict(request_id=uid(request), base_revision=revision, ops=ops))
        assert receipt['committed'] is True and receipt['revision'] == revision+1, receipt
        return receipt['revision']

    def cook_water(self, client, revision, bed_recipe, bed_plan, origin_x, cell_size):
        request = dict(revision=revision, terrain=dict(recipe=bed_recipe),
                       expected_terrain_recipe_sha256=bed_plan['recipe_sha256'], tile_entities=BED,
                       grid=dict(cells_x=2, cells_z=1, cell_size_m=cell_size,
                                 origin_x_m=origin_x, origin_z_m=0, relaxation_per_second=6),
                       initial_depths_m=[.5, 0], exchanges=[dict(
                           cell=WATER_CELLS[0], source_m3_per_second=.6, drain_m3_per_second=0)])
        plan = client.call('world.water.plan', request)
        pin = plan['plan_sha256']
        assert client.call('world.water.cook', request | dict(expected_plan_sha256=pin)) == plan
        package = self.assets/(pin+'.pwater')
        assert file_hash(package) == pin and package.stat().st_size == plan['asset_bytes']
        return plan

    def read_water(self, client, session, tick):
        return client.call('runtime.water.inspect', dict(session_id=session, tick=tick, cells=WATER_CELLS))

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Separate Snow and Water')))
        with self.owner('author') as (client, _):
            schema = client.call('world.describe')
            assert schema['schema_revision'] >= 198 and schema['snow']['water_coexistence'] is True, schema
            assert schema['snow']['available'] is bool(self.args.runtime), schema['snow']
            initial = client.call('world.inspect')['revision']
            elevations = [base(x, z) for z in range(HEIGHT) for x in range(WIDTH)]
            _, _, snow_ops = self.terrain(client, [0, 0, 0], elevations, TILES, 'Snow')
            bed_recipe, bed_plan, bed_ops = self.terrain(client, [2, FLOOR, 0], [0]*(WIDTH*HEIGHT), BED, 'Water bed')
            _, _, overlap_ops = self.terrain(client, [1.5, 1, 0], elevations, OVERLAP, 'Overlap witness')
            revision = self.transact(client, initial, 5010, *snow_ops, *bed_ops,
                dict(op='component.remove', id=uid(1), type='MeshRenderer'),
                dict(op='component.remove', id=uid(1), type='BoxCollider'),
                dict(op='entity.create', id=PLAIN, name='Outside both simulation regions'),
                component(PLAIN, 'Transform', transform([6, 0, .5])),
                component(PLAIN, 'MeshRenderer', DRAWABLE), component(PLAIN, 'PbrMaterial', MATERIAL),
                component(PLAIN, 'BoxCollider', BOX))

            disjoint = self.cook_water(client, revision, bed_recipe, bed_plan, 2.5, .5)
            edge = self.cook_water(client, revision, bed_recipe, bed_plan, 2, 1)
            self.pin = edge['plan_sha256']
            assert disjoint['plan_sha256'] != self.pin
            revision = self.transact(client, revision, 5011, dict(op='water.set', asset=disjoint['plan_sha256']))
            assert 'needs_snow_water' not in client.call('world.dependencies')
            revision = self.transact(client, revision, 5012, snow_binding(TILES))
            assert client.call('world.dependencies')['needs_snow_water'] is True
            revision = self.transact(client, revision, 5013, dict(op='water.set', asset=self.pin))
            joint = json.loads(self.world.read_bytes())
            assert joint['snow']['tiles'] == TILES and joint['water']['asset'] == self.pin, joint

            frame = uid(5014)
            if self.args.runtime:
                client.call('runtime.start', dict(session_id=frame, revision=revision))
                frozen = self.read_water(client, frame, 0)
                assert frozen['plan_sha256'] == self.pin, frozen
                self.snow(client, frame, 0, Lattice())
            undone = client.call('world.undo', dict(request_id=uid(5015), base_revision=revision))
            assert undone['revision'] == revision+1, undone
            document = json.loads(self.world.read_bytes())
            assert document['snow'] == joint['snow'] and document['water']['asset'] == disjoint['plan_sha256']
            if self.args.runtime:
                assert client.call('runtime.status')['authored_revision'] == revision
                assert exact(self.read_water(client, frame, 0), frozen)
            redone = client.call('world.redo', dict(request_id=uid(5016), base_revision=revision+1))
            assert redone['revision'] == revision+2, redone
            revision += 2
            document = json.loads(self.world.read_bytes())
            assert (document['snow'], document['water']) == (joint['snow'], joint['water'])

            before, assets, history = self.world.read_bytes(), read_inventory(self.assets), client.call('world.history')
            self.measurements['authored_refusals'] = {}
            for index, (label, operations, code, message) in enumerate((
                ('overlap', overlap_ops+[snow_binding(OVERLAP)], -32602,
                 'Snow and water must occupy separate terrain areas.'),
                ('reused-bed', [snow_binding(BED)], -32009,
                 'Water terrain tiles cannot also be bound to snow.'),
            )):
                rejected = self.reject(client, 'world.transact', dict(request_id=uid(5017+index),
                    base_revision=revision, ops=operations), code)
                assert message in rejected['message'], rejected
                assert self.world.read_bytes() == before and read_inventory(self.assets) == assets
                assert exact(client.call('world.history'), history)
                self.measurements['authored_refusals'][label] = rejected
            deps = client.call('world.dependencies')
            project = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
            assert deps['revision'] == project['revision'] == revision and deps['assets'] == project['assets']
            assert deps['needs_snow_water'] is project['needs_snow_water'] is True
            self.measurements.update(disjoint=disjoint, exact_edge=edge, dependencies=deps)
            return revision

    def observe(self, client, session, tick, lattice):
        snow = self.snow(client, session, tick, lattice)
        water = self.read_water(client, session, tick)
        assert water['active'] is True and water['tick'] == tick and water['plan_sha256'] == self.pin, water
        # The two flat one-metre cells receive 0.01 m³ per tick in the
        # first cell, then exchange one tenth of the depth difference.
        a, b = .5, 0.
        for _ in range(tick):
            a += .01
            transfer = (a-b)*.1
            a, b = a-transfer, b+transfer
        for cell, expected, row in zip(WATER_CELLS, (a, b), water['cells'], strict=True):
            assert row['cell'] == cell, row
            near(row['floor_height_m'], FLOOR)
            near(row['depth_m'], expected)
        near(water['volume_m3'], .5+.01*tick)
        if tick:
            ledger = water['last_step']
            assert ledger['tick'] == tick, ledger
            near(ledger['supplied_m3'], .01)
            near(ledger['volume_before_m3'], .5+.01*(tick-1))
            near(ledger['volume_after_m3'], water['volume_m3'])
        else:
            assert water['last_step'] is None, water
        guard = dict(session_id=session, tick=tick)
        snow_hit = client.call('runtime.raycast', guard | down(*CENTER))['hit']
        surface, _, triangle = lattice.surface(*CENTER, snow_hit['entity'])
        assert snow_hit['entity'] in TILES and snow_hit['triangle'] == triangle, snow_hit
        near(snow_hit['position'][1], surface, tolerance=3e-5)
        bed_hit = client.call('runtime.raycast', guard | down(2.513, .517))['hit']
        assert bed_hit['entity'] == BED[0], bed_hit
        near(bed_hit['position'][1], FLOOR, tolerance=3e-5)
        return dict(snow=without_session(snow), water=without_session(water), snow_hit=snow_hit, bed_hit=bed_hit)

    def checkpoint(self, client, saves, session, tick, request, slot, lattice):
        raw = self.snow_archive(client, saves, session, tick, request, slot, lattice)
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['water']['plan_sha256'] == self.pin and payload['water']['last_report']['tick'] == tick, payload
        return raw

    def runtime(self, revision):
        saves = self.root/'Save slots'
        saves.mkdir()
        lattice = Lattice()
        phases = self.measurements['phases'] = {}
        with self.owner('warm') as (client, warm):
            session = self.initialize(client, revision, uid(5020), saves, None)
            phases['initial'] = self.observe(client, session, 0, lattice)
            first = self.stamp(client, session, 0, 0, STAMP_RADIUS, STAMP_DEPTH)
            self.update(client, session, 0, 5021, first, retry=True)
            assert lattice.stamp(CENTER, STAMP_RADIUS, STAMP_DEPTH)
            phases['checkpoint'] = self.observe(client, session, 1, lattice)
            assert phases['checkpoint']['snow_hit']['position'][1] < phases['initial']['snow_hit']['position'][1]
            assert exact(phases['checkpoint']['bed_hit'], phases['initial']['bed_hit'])
            checkpoint = self.checkpoint(client, saves, session, 1, 5022, 'checkpoint', lattice)

            second = self.stamp(client, session, 1, 1, STAMP_RADIUS, STAMP_DEPTH)
            # Failed late, after the snow stamp, physics and water step.
            missing_mark = dict(receiver=PLAIN, ray=down(9, 9), half_extent_m=[.1, .1], half_depth_m=.03,
                minimum_normal_dot=.8, edge_fraction=.25, linear_rgb=[.1, .2, .3], opacity=1, roughness=.8)
            failed = self.reject(client, 'runtime.snow.step', self.guard(session, 1, 5023) | dict(
                snow_stamp=second, surface_marks=[missing_mark]), -32040)
            assert 'must first hit the expected receiver' in failed['message'], failed
            assert exact(self.observe(client, session, 1, lattice), phases['checkpoint'])
            assert checkpoint == self.checkpoint(client, saves, session, 1, 5024, 'rollback', lattice)
            self.measurements['late_rollback'] = failed

            self.step(client, session, 1, 5025)
            phases['water_step'] = self.observe(client, session, 2, lattice)
            assert phases['water_step']['snow']['generation'] == 1
            second = self.stamp(client, session, 2, 1, STAMP_RADIUS, STAMP_DEPTH)
            self.update(client, session, 2, 5026, second)
            assert lattice.stamp(CENTER, STAMP_RADIUS, STAMP_DEPTH)
            phases['final'] = self.observe(client, session, 3, lattice)
            final = self.checkpoint(client, saves, session, 3, 5027, 'final', lattice)

        lattice = Lattice()
        with self.owner('cold') as (client, cold):
            fresh = self.initialize(client, revision, uid(5028), saves, None)
            session = uid(5029)
            assert self.load(client, revision, fresh, session, 'checkpoint', 1, 5030) == 0
            assert lattice.stamp(CENTER, STAMP_RADIUS, STAMP_DEPTH)
            assert exact(self.observe(client, session, 1, lattice), phases['checkpoint'])
            assert self.checkpoint(client, saves, session, 1, 5031, 'restored', lattice) == checkpoint
            self.step(client, session, 1, 5032)
            assert exact(self.observe(client, session, 2, lattice), phases['water_step'])
            self.update(client, session, 2, 5033, second)
            assert lattice.stamp(CENTER, STAMP_RADIUS, STAMP_DEPTH)
            assert exact(self.observe(client, session, 3, lattice), phases['final'])
            assert self.checkpoint(client, saves, session, 3, 5034, 'restored-final', lattice) == final
        assert cold['pid'] != warm['pid']
        self.measurements['complete_saves'] = dict(checkpoint=checkpoint.decode('utf-8'), final=final.decode('utf-8'))

    def export(self, runtime, revision):
        output = self.root/'Snow and Water Game'
        descriptor = json.loads((runtime/'runtime.json').read_bytes())
        assert descriptor['features']['snow_water_regions'] is True, descriptor
        for mode in ('missing', 'false'):
            unsupported = self.root/('Unsupported '+mode)
            unsupported.mkdir()
            spec = descriptor | dict(features=descriptor['features'].copy())
            if mode == 'missing':
                del spec['features']['snow_water_regions']
            else:
                spec['features']['snow_water_regions'] = False
            (unsupported/'runtime.json').write_text(encode(spec), encoding='utf-8')
            command = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                       '--output', self.native(output), '--runtime', self.native(unsupported)]
            child = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
            self.calls.append(dict(argv=command, exit=child.returncode, stdout=child.stdout, stderr=child.stderr))
            assert child.returncode == 4 and not output.exists(), child
            assert json.loads(child.stdout)['diagnostics'] == [dict(
                code='project.failed', message='Coexisting snow and water require a compatible simulation runtime.')], child
        success(json.loads(self.cli('project', 'build', self.native(self.manifest),
                                   '--output', self.native(output), '--runtime', self.native(runtime))))
        expected = self.measurements['dependencies']['assets']
        assets = sorted(file for file in output.rglob('*') if file.suffix in ('.pmodel', '.pwater'))
        assert sorted(file.name for file in assets) == [entry['filename'] for entry in expected]
        assert all(file_hash(file) == file.stem for file in assets)
        relocated = self.root/'Relocated game'
        output.rename(relocated)
        self.project.rename(self.root/'Detached source')
        game = relocated/'game.json'
        executable = relocated/json.loads(game.read_bytes())['engine']['executable']
        info = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert info['revision'] == revision and info['needs_snow_water'] is True, info
        self.measurements['relocated_export'] = info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=(0, 1), default=1)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    scratch = Path(__file__).resolve().parents[1]/'build'/'snow-water-contract'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch) as directory:
        test = SnowWaterContract(args, Path(directory))
        passed, failure = False, None
        try:
            revision = test.author()
            if args.runtime:
                test.runtime(revision)
            if args.runtime_root:
                test.export(args.runtime_root, revision)
            passed = True
        except BaseException:
            failure = traceback.format_exc()
            raise
        finally:
            if args.evidence:
                result = dict(passed=passed, runtime=bool(args.runtime), executable_sha256=file_hash(args.binary),
                              calls=test.calls, measurements=test.measurements)
                if failure:
                    result['failure'] = failure
                args.evidence.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print('Separate snow and water authoring passed.' +
          (' Native collision, water volumes, rollback and cold saves matched.' if args.runtime else ''))


if __name__ == '__main__':
    main()
