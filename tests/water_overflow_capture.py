#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fill a cistern above its crest and verify automatic overflow and saved replay."""

import json
import math

from particle_collision_capture import main
from retained_capture import CAMERA, expect_error, new_id
from water_reservoir_capture import ABOVE, ABOVE_EYE, AUDIO, EYE, FOOTPRINT, GATE, OPTICS, TERRAIN, TILT, ReservoirFixture
from water_storage_capture import near, port, reservoir

CREST = .7
WIDTH = 1.
COEFFICIENT = 1.7
SPILL_TICKS = 120
FINISH_TICK = SPILL_TICKS + 1
TOTAL_VOLUME = 2.33


def expected_spill(ticks):
    tank, basin, rate = .9, .25, 0.
    for _ in range(ticks):
        # For this fixture the receiving surface stays below the crest, its
        # capacity is ample, and the resulting step is always below the stability limit.
        rate = COEFFICIENT * WIDTH * math.pow(tank-CREST, 1.5)
        tank -= rate/60
        basin += rate/60
    return tank, basin, rate


class OverflowFixture(ReservoirFixture):
    def author(self, client):
        revision = super().author(client)
        original, = self.project.rglob('*.pwater')
        old = json.loads(original.read_bytes())
        recipe = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
            origin_m=[-2, 0, -7], spacing_m=[2, 2], heights_m=[0]*9))
        cistern, header = dict(reservoir='cistern'), dict(reservoir='header')
        cell = dict(cell=[0, 0])
        supply = port('supply', header, cistern, GATE) | dict(conductance_m2_s=18)
        spill = dict(id='spill', **{'from': cistern, 'to': cell}, sill_height_m=CREST,
            max_flow_m3_s=10, openness=1, overflow=dict(width_m=WIDTH, coefficient_sqrt_m_s=COEFFICIENT))
        tanks = [reservoir('header', 2, 1.39), reservoir('cistern', 2, .69) | dict(footprint=FOOTPRINT)]
        common = dict(revision=revision, terrain=dict(recipe=recipe), expected_terrain_recipe_sha256=old['terrain_recipe_sha256'],
            tile_entities=[TERRAIN], grid=old['grid'], initial_depths_m=[.25], optics=OPTICS, listener_audio=AUDIO)
        def args(outlet):
            return common | dict(storage=dict(reservoirs=tanks, ports=[supply, outlet]))
        for bad in (spill | dict(conductance_m2_s=1), spill | {'from': cell, 'to': cistern},
                    spill | dict(sill_height_m=0), spill | dict(sill_height_m=2),
                    spill | dict(overflow=dict(width_m=0, coefficient_sqrt_m_s=COEFFICIENT))):
            expect_error(-32602, 'world.water.plan', client, args(bad))
        selected = args(spill)
        plan = client.call('world.water.plan', selected)
        assert client.call('world.water.cook', selected | dict(expected_plan_sha256=plan['plan_sha256'])) == plan
        self.data['plan_sha256'] = pin = plan['plan_sha256']
        asset, = self.project.rglob(pin+'.pwater')
        assert json.loads(asset.read_bytes())['version'] == 21
        revision = self.transact(client, revision, dict(op='water.set', asset=pin))
        assert client.call('world.dependencies')['needs_water_overflow']
        assert self.cli('project', 'inspect', self.native(self.manifest))['needs_water_overflow']
        return revision

    def actual(self, client, session, tick, tank, terrain, rate, gate):
        report = client.call('runtime.water.inspect', dict(session_id=session, tick=tick, cells=[[0, 0]], points=[EYE],
            segments=[dict(from_world=[2, 1.5, -5], to_world=[2, 0, -5])]))
        report.pop('session_id')
        near(report['volume_m3'], TOTAL_VOLUME)
        near(report['cells'][0]['surface_height_m'], terrain)
        cistern, header = report['storage']['reservoirs']
        near(cistern['surface_height_m'], tank)
        near(header['volume_m3'], TOTAL_VOLUME-tank-terrain)
        spill, supply = report['storage']['ports']
        assert spill['id']=='spill' and 'overflow' in spill and spill['openness']==1
        assert supply['id']=='supply' and supply['openness']==gate
        near(spill['discharge_m3_s'], rate)
        point, = report['points']
        assert point['column']['reservoir']=='cistern'
        near(point['submersion_m'], max(0,tank-EYE[1]))
        near(report['segments'][0]['position_m'][1], tank)
        if tick:
            row = report['last_step']
            near(row['volume_before_m3'], TOTAL_VOLUME)
            assert row['supplied_m3']==row['drained_m3']==0
        return report

    def start(self, client, session, label):
        self.data['initial'].append(self.actual(client, session, 0, .69, .25, 0, 0))
        for camera, eye, tilt, suffix in ((CAMERA,EYE,0,''),(ABOVE,ABOVE_EYE,TILT,'-above')):
            self.picture(client, session, 0, label+'-sealed'+suffix, .69, camera, eye, tilt)
        self.step(client, session, 0, GATE)
        saved = client.call('save.write', dict(request_id=new_id(), configuration_generation=1,
            expected_generation=0, slot='water', session_id=session, expected_tick=1, expected_gameplay_revision=0))
        self.data['save_sha256'][label] = saved['sha256']
        return self.take(client, session, label)

    def take(self, client, session, label):
        full = self.actual(client, session, 1, .9, .25, 0, 1)
        self.picture(client, session, 1, label+'-above-crest', .9, CAMERA, EYE, 0)
        self.step(client, session, 1, GATE)
        first = self.actual(client, session, 2, *expected_spill(1), 0)
        stepped = client.call('runtime.step', dict(session_id=session, request_id=new_id(), expected_tick=2, ticks=SPILL_TICKS-1))
        assert stepped['current_tick']==FINISH_TICK
        tank, terrain, rate = expected_spill(SPILL_TICKS)
        last = self.actual(client, session, FINISH_TICK, tank, terrain, rate, 0)
        for camera, eye, tilt, suffix in ((CAMERA,EYE,0,''),(ABOVE,ABOVE_EYE,TILT,'-above')):
            self.picture(client, session, FINISH_TICK, label+'-released'+suffix, tank, camera, eye, tilt)
        self.data['continuations'][label] = result = [full, first, last]
        return result

    def load(self, client, old, tick, label):
        return super().load(client, old, FINISH_TICK if tick else 0, label)

    def run(self):
        super().run()
        if self.args.runtime_root:
            game = self.root/'Relocated'/'game.json'
            assert self.cli('game', 'inspect', self.native(game))['needs_water_overflow']
        if self.args.gpu is not None:
            frames = self.data['pixel_counts']
            full = frames['source-above-crest']['capture']
            assert full != frames['source-released']['capture']
            assert all(row['capture']==full for name,row in frames.items() if name.endswith('-above-crest'))


if __name__ == '__main__':
    main(OverflowFixture)
