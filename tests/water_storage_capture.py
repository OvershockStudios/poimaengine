#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flood and drain an authored basin, then continue from a relocated save."""

import copy
import json
import math

from particle_collision_capture import main as run_fixture
from retained_capture import CAMERA, Fixture, component, expect_error, new_id, transform, tree_hashes, uid

TERRAIN = uid(980)
EYE = [-.5, .355, -5]
RELEASE, DRAIN = 'sluice.release', 'sluice.drain'
CELL = dict(cell=[0, 0])
HEADER, SUMP = dict(reservoir='header'), dict(reservoir='sump')


def reservoir(name, capacity, initial):
    return dict(id=name, floor_height_m=0, area_m2=1, capacity_m3=capacity, initial_volume_m3=initial)


def port(name, source, receiver, action, maximum=20):
    return dict(id=name, **{'from': source, 'to': receiver}, sill_height_m=0,
                conductance_m2_s=12, max_flow_m3_s=maximum, openness=0, action=action)


PROFILE = dict(reservoirs=[reservoir('sump', .15, .1), reservoir('header', 2, .85)], ports=[
    port('release', HEADER, CELL, RELEASE),
    port('capture', SUMP, CELL, DRAIN),
    port('bypass', HEADER, SUMP, DRAIN, maximum=3.24),
])


def near(value, expected, tolerance=2e-11):
    assert math.isclose(value, expected, rel_tol=0, abs_tol=tolerance), (value, expected)


class StorageFixture(Fixture):
    def transact(self, client, revision, *operations):
        result = client.call('world.transact', dict(request_id=new_id(), base_revision=revision, ops=operations))
        assert result['committed']
        return result['revision']

    def author(self, client):
        self.data['pixels'] = {}
        revision = client.call('world.inspect')['revision']
        recipe = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3,
            tile_cells=2, origin_m=[-2, 0, -7], spacing_m=[2, 2], heights_m=[0]*9))
        terrain = client.call('world.terrain.inspect', dict(recipe=recipe))
        assert client.call('world.terrain.cook', dict(recipe=recipe, expected_recipe_sha256=terrain['recipe_sha256'])) == terrain
        tile, = terrain['tiles']
        mesh = dict(asset=tile['asset'], primitive=tile['primitive'])
        revision = self.transact(client, revision,
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(5), type='Light'),
            component(uid(4), 'LightingEnvironment', dict(ambient=[.7]*3, exposure=1)),
            dict(op='physics_checkpoint.set', definition=dict(version=1)),
            dict(op='entity.create', id=TERRAIN, name='Storage basin'),
            component(TERRAIN, 'Transform', transform([0, 0, 0])),
            component(TERRAIN, 'StaticMesh', mesh | dict(visible=True)),
            component(TERRAIN, 'MeshCollider', mesh | dict(friction=.7, restitution=0)),
            dict(op='entity.create', id=CAMERA, name='Basin waterline'),
            component(CAMERA, 'Transform', transform(EYE)),
            component(CAMERA, 'Camera', dict(vertical_fov=50, near=.03, far=50)))

        params = dict(revision=revision, terrain=dict(recipe=recipe), tile_entities=[TERRAIN],
            expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            grid=dict(cells_x=1, cells_z=1, cell_size_m=1, origin_x_m=-1, origin_z_m=-5.5),
            initial_depths_m=[.25], optics=dict(follow_terrain=True, refraction=True,
                extinction_per_m=[.12, .4, .75], scattered_radiance=[.05, .1, .15]))
        plain = client.call('world.water.plan', params)
        self.reject_invalid_profiles(client, params)
        args = params | dict(storage=PROFILE)
        plan = client.call('world.water.plan', args)
        reordered = dict(reservoirs=PROFILE['reservoirs'][::-1], ports=PROFILE['ports'][::-1])
        assert plan['plan_sha256'] != plain['plan_sha256']
        assert client.call('world.water.plan', params | dict(storage=reordered))['plan_sha256'] == plan['plan_sha256']
        assert client.call('world.water.cook', args | dict(expected_plan_sha256=plan['plan_sha256'])) == plan
        self.data['plan_sha256'] = plan['plan_sha256']
        revision = self.transact(client, revision, dict(op='water.set', asset=plan['plan_sha256']))
        assert client.call('world.dependencies')['needs_water_storage']
        assert self.cli('project', 'inspect', self.native(self.manifest))['needs_water_storage']
        return revision

    def reject_invalid_profiles(self, client, params):
        for field, index, values in (
            ('reservoirs', 0, dict(initial_volume_m3=.2)),
            ('reservoirs', 0, dict(id='header')),
            ('ports', 0, dict(to=HEADER)),
            ('ports', 0, dict(to=dict(reservoir='unknown'))),
            ('ports', 0, dict(to=CELL | HEADER)),
            ('ports', 0, dict(to=dict(cell=[1, 0]))),
            ('ports', 0, dict(max_flow_m3_s=0)),
            ('ports', 0, dict(id='bypass')),
        ):
            profile = copy.deepcopy(PROFILE)
            profile[field][index].update(values)
            expect_error(-32602, 'world.water.plan', client, params | dict(storage=profile))

    def read(self, client, session, tick):
        result = client.call('runtime.water.inspect', dict(session_id=session, tick=tick,
            cells=[[0, 0]], points=[EYE], segments=[dict(from_world=[EYE[0], 1, EYE[2]], to_world=[EYE[0], .01, EYE[2]])]))
        result.pop('session_id')
        near(result['volume_m3'], 1.2)
        near(result['grid_volume_m3'] + result['storage']['volume_m3'], 1.2)
        if tick:
            receipt = result['last_step']
            near(receipt['volume_before_m3'], 1.2)
            near(receipt['volume_after_m3'], 1.2)
            assert receipt['supplied_m3'] == receipt['drained_m3'] == 0
        return result

    def check(self, observed, stage, header, sump, discharge, openings):
        near(observed['grid_volume_m3'], stage)
        near(observed['cells'][0]['surface_height_m'], stage)
        near(observed['segments'][0]['position_m'][1], stage)
        near(observed['points'][0]['submersion_m'], max(0, stage - EYE[1]))
        tanks, gates = observed['storage']['reservoirs'], observed['storage']['ports']
        assert [tank['id'] for tank in tanks] == ['header', 'sump']
        assert [gate['id'] for gate in gates] == ['bypass', 'capture', 'release']
        for tank, volume in zip(tanks, [header, sump]):
            near(tank['volume_m3'], volume)
            near(tank['surface_height_m'], volume)
        for gate, expected in zip(gates, discharge):
            near(gate['discharge_m3_s'], expected)
        assert [gate['openness'] for gate in gates] == openings

    def step(self, client, session, tick, *actions):
        params = dict(session_id=session, request_id=new_id(), expected_tick=tick, ticks=1)
        if actions:
            params['inputs'] = [dict(entity=uid(2), actions=[dict(name=action, held=True, pressed=True,
                released=False) for action in sorted(actions)])]
        reply = client.call('runtime.step', params)
        assert reply['current_tick'] == tick + 1

    def picture(self, client, session, tick, label, depth):
        if self.args.gpu is None:
            return
        capture = self.capture(client, session, tick, label, width=240, height=160, reuse=True)
        optics = capture['report']['render_diagnostics']['water_optics']
        assert optics['active'] and optics['refraction']
        near(optics['camera_submersion_m'], depth, 2e-6)
        self.data['pixels'][label] = dict(image=capture['image'], submersion_m=optics['camera_submersion_m'])

    def continue_draining(self, client, session, label):
        flooded = self.read(client, session, 2)
        self.check(flooded, .37, .73, .1, [0, 0, 7.2], [0, 0, 1])
        self.picture(client, session, 2, label+'-flooded', .015)
        self.step(client, session, 2, RELEASE, DRAIN)
        drained = self.read(client, session, 3)
        self.check(drained, .345, .705, .15, [1.5, -1.5, 0], [1, 1, 0])
        self.picture(client, session, 3, label+'-drained', 0)
        self.step(client, session, 3)
        full = self.read(client, session, 4)
        self.check(full, .345, .705, .15, [0, 0, 0], [1, 1, 0])
        result = [flooded, drained, full]
        self.data['continuations'][label] = result
        return result

    def begin(self, client, session, label):
        self.check(self.read(client, session, 0), .25, .85, .1, [0, 0, 0], [0, 0, 0])
        self.picture(client, session, 0, label+'-initial', 0)
        self.step(client, session, 0)
        self.check(self.read(client, session, 1), .25, .85, .1, [0, 0, 0], [0, 0, 0])
        self.step(client, session, 1, RELEASE)
        save = client.call('save.write', dict(request_id=new_id(), configuration_generation=1,
            expected_generation=0, slot='basin', session_id=session, expected_tick=2, expected_gameplay_revision=0))
        self.data['save_sha256'][label] = save['sha256']
        return self.continue_draining(client, session, label)

    def restore(self, client, old, tick, label):
        session = new_id()
        reply = client.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='basin',
            expected_generation=1, revision=self.revision, expected_session_id=old, expected_tick=tick,
            expected_gameplay_revision=0, new_session_id=session))
        assert reply['tick'] == 2 and not reply['source_stale']
        return self.continue_draining(client, session, label)

    def run(self):
        self.cli('project', 'create', self.native(self.project), '--name', 'Reservoir Basin')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=self.native(saves)))
            session = self.mount(client)
            expected = self.begin(client, session, 'source')
            assert self.restore(client, session, 4, 'restored') == expected
        if self.args.runtime_root:
            stage, bundle, hidden = (self.root/name for name in ('Built', 'Relocated', 'Hidden source'))
            self.cli('project', 'build', self.native(self.manifest), '--output', self.native(stage),
                '--runtime', self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(hidden)
            game = bundle/'game.json'
            assert self.cli('game', 'inspect', self.native(game))['needs_water_storage']
            if self.args.windows_interop:
                executable = bundle/json.loads(game.read_bytes())['engine']['executable']
                executable.chmod(executable.stat().st_mode | 0o100)
            before = tree_hashes(bundle)
            saves = self.root/'Export saves'
            saves.mkdir()
            with self.owner('relocated', game=game, saves=saves) as client:
                status = client.call('runtime.status')
                assert self.begin(client, status['session_id'], 'relocated') == expected
            with self.owner('fresh-save', game=game, saves=saves) as client:
                status = client.call('runtime.status')
                assert self.restore(client, status['session_id'], 0, 'fresh-save') == expected
            assert tree_hashes(bundle) == before and not self.project.exists()
            self.data['immutable_files'] = len(before)
        if self.args.gpu is not None:
            pixels = self.data['pixels']
            assert len({pixels['source-'+phase]['image'] for phase in ('initial', 'flooded', 'drained')}) == 3
            for phase in ('flooded', 'drained'):
                assert len({record['image'] for label, record in pixels.items() if label.endswith('-'+phase)}) == 1


if __name__ == '__main__':
    run_fixture(StorageFixture)
