#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify one changing tank against the committed water, audio and GPU paths."""

import json
import math

from particle_collision_capture import main
from retained_capture import CAMERA, Fixture, component, expect_error, new_id, transform, tree_hashes, uid
from water_optics_capture import fresnel, intersect
from water_storage_capture import StorageFixture, near, port, reservoir

TERRAIN, ABOVE = uid(992), uid(993)
EYE = [2, .79, -5]
ABOVE_EYE = [2, 1.45, -3.6]
TILT = math.radians(-30)
GATE = 'sluice.release'
LENS = dict(vertical_fov=50, near=.03, far=30)
AMBIENT = [.28, .34, .39]
OPTICS = dict(extinction_per_m=[1.15, .42, .18], scattered_radiance=[.03, .12, .21])
AUDIO = dict(cutoff_hz=420, gain=.55, full_depth_m=.12)
FOOTPRINT = dict(min_xz_m=[1.5, -5.5], size_xz_m=[1, 1])
SIZE = (240, 160)
PIXELS = [(x, y) for y in (30, 54, 75, 86, 104, 130) for x in (25, 77, 119, 164, 209)]


def optical_reference(pixel, probe, height, eye, tilt):
    width, screen_height = SIZE
    focal = screen_height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    x, y = (pixel[0] + .5 - width/2)/focal, (screen_height/2 - pixel[1] - .5)/focal
    scale = math.hypot(x, y, 1)
    ray = (x/scale, (y*math.cos(tilt)+math.sin(tilt))/scale,
           (y*math.sin(tilt)-math.cos(tilt))/scale)
    depth = probe['depth']
    clip = scale * LENS['near'] / (1-depth + depth*LENS['near']/LENS['far'])
    lower, upper = (1.5, 0, -5.5), (2.5, height, -4.5)
    span = intersect(eye, ray, lower, upper, clip)
    distance = span[1]-span[0] if span else 0
    top = (height-eye[1])/ray[1] if ray[1] else -1
    hit = (0 <= top < clip and lower[0] <= eye[0]+top*ray[0] < upper[0] and
        lower[2] <= eye[2]+top*ray[2] < upper[2])
    mixing = fresnel(ray[1]) if hit else 0.
    prefix = max(0., min(top, span[1])-span[0]) if hit and span else 0.
    reference = []
    for channel, (extinction, source) in enumerate(zip(OPTICS['extinction_per_m'], OPTICS['scattered_radiance'])):
        reflection = AMBIENT[channel] if ray[1] < 0 else source
        reference.append(source + (1-mixing)*math.exp(-extinction*distance)*(probe['raw_hdr'][channel]-source) +
            mixing*math.exp(-extinction*prefix)*(reflection-source))
    return reference, distance, hit


class ReservoirFixture(Fixture):
    transact = StorageFixture.transact
    step = StorageFixture.step

    def author(self, client):
        revision = client.call('world.inspect')['revision']
        recipe = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
            origin_m=[-2, 0, -7], spacing_m=[2, 2], heights_m=[0]*9))
        terrain = client.call('world.terrain.inspect', dict(recipe=recipe))
        assert client.call('world.terrain.cook', dict(recipe=recipe, expected_recipe_sha256=terrain['recipe_sha256'])) == terrain
        tile, = terrain['tiles']
        mesh = dict(asset=tile['asset'], primitive=tile['primitive'])
        ops = [dict(op='component.remove', id=uid(1), type=kind) for kind in ('BoxCollider', 'MeshRenderer')]
        ops.extend([dict(op='component.remove', id=uid(5), type='Light'),
            component(uid(4), 'LightingEnvironment', dict(ambient=AMBIENT, exposure=1)),
            dict(op='physics_checkpoint.set', definition=dict(version=1)),
            dict(op='entity.create', id=TERRAIN, name='Downstream basin'), component(TERRAIN, 'Transform', transform([0,0,0])),
            component(TERRAIN, 'StaticMesh', mesh | dict(visible=True)),
            component(TERRAIN, 'MeshCollider', mesh | dict(friction=.7, restitution=0))])
        for e, eye, angle in ((CAMERA, EYE, 0), (ABOVE, ABOVE_EYE, TILT)):
            ops.extend([dict(op='entity.create', id=e, name='Reservoir camera'), component(e, 'Camera', LENS),
                component(e,'Transform',transform(eye) | dict(rotation=[math.sin(angle/2), 0, 0, math.cos(angle/2)]))])
        for i, (position, scale) in enumerate((([2,-.05,-5], [1.2,.1,1.2]), ([2,.5,-5.56], [1.2,1,.12]),
                                               ([1.44,.44,-5], [.12,.88,1]), ([2.56,.44,-5], [.12,.88,1]))):
            entity = uid(996+i)
            ops.extend([dict(op='entity.create', id=entity, name='Stone lining'),
                component(entity, 'Transform', transform(position) | dict(scale=scale)),
                component(entity, 'MeshRenderer', dict(primitive='box', albedo=[.65,.58,.45], visible=True))])
        revision = self.transact(client, revision, *ops)
        tank = reservoir('cistern', 2, .85)
        valves = [port('gate', dict(reservoir='cistern'), dict(cell=[0, 0]), GATE)]
        params = dict(revision=revision, terrain=dict(recipe=recipe), tile_entities=[TERRAIN],
            expected_terrain_recipe_sha256=terrain['recipe_sha256'], grid=dict(cells_x=1, cells_z=1, cell_size_m=1,
                origin_x_m=-1, origin_z_m=-5.5), initial_depths_m=[.25], optics=OPTICS, listener_audio=AUDIO)
        def args(tanks):
            return params | dict(storage=dict(reservoirs=tanks, ports=valves))
        control = client.call('world.water.plan', args([tank]))
        for bad in (FOOTPRINT | dict(size_xz_m=[1, 2]), FOOTPRINT | dict(min_xz_m=[-1, -5.5]),
                    FOOTPRINT | dict(min_xz_m=[1e6, -5.5])):
            expect_error(-32602, 'world.water.plan', client, args([tank | dict(footprint=bad)]))
        placed = tank | dict(footprint=FOOTPRINT)
        expect_error(-32602, 'world.water.plan', client, args([placed, placed | dict(id='overlap')]))
        configured = args([placed])
        inspected = client.call('world.water.plan', configured)
        assert inspected['plan_sha256'] != control['plan_sha256']
        self.data['plan_sha256'] = pin = inspected['plan_sha256']
        assert client.call('world.water.cook', configured | dict(expected_plan_sha256=pin)) == inspected
        asset, = self.project.rglob('*.pwater')
        assert json.loads(asset.read_bytes())['version'] == 20 and asset.stem == pin
        revision = self.transact(client, revision, dict(op='water.set', asset=pin))
        assert client.call('world.dependencies')['needs_water_reservoirs']
        assert self.cli('project', 'inspect', self.native(self.manifest))['needs_water_reservoirs']
        return revision

    def observe(self, client, session, tick, tank_height, stage):
        state = client.call('runtime.water.inspect', dict(session_id=session, tick=tick, cells=[[0,0]],
            points=[EYE, [2.5,.1,-5], [2,-.01,-5]], segments=[dict(from_world=[2,1.5,-5], to_world=[2,0,-5])]))
        state.pop('session_id')
        near(state['volume_m3'], 1.1)
        near(state['cells'][0]['surface_height_m'], stage)
        near(state['storage']['volume_m3'], tank_height)
        tank, edge, below = state['points']
        assert tank['column']['reservoir'] == 'cistern' and edge['column'] is None
        assert below['submersion_m'] == 0
        near(tank['submersion_m'], max(0, tank_height-EYE[1]))
        hit, = state['segments']
        assert hit['reservoir'] == 'cistern' and not hit['offshore'] and hit['cell'] is None
        near(hit['position_m'][1], tank_height)
        audio = client.call('runtime.audio.inspect', dict(session_id=session, tick=tick, listener=CAMERA))['listener_water']
        near(audio['submersion_m'], tank['submersion_m'])
        near(audio['target_blend'], .5 if tank_height > EYE[1] else 0.)
        return dict(water=state, listener=audio)

    def picture(self, client, session, tick, label, height, camera, eye, tilt):
        if self.args.gpu is None:
            return
        record = self.capture(client, session, tick, label, reuse=True, camera=camera,
            width=SIZE[0], height=SIZE[1], scene_product_probes=[dict(x=x, y=y) for x,y in PIXELS])
        render = record['report']['render_diagnostics']
        optics = render['water_optics']
        assert optics['active'] and optics['reservoir_volumes'] == 1, optics
        near(optics['camera_submersion_m'], max(0, height-eye[1]), 2e-6)
        probes = render['scene_products']['probes']
        errors, wet, interfaces = [], 0, 0
        for pixel, probe in zip(PIXELS, probes, strict=True):
            expected, length, interface = optical_reference(pixel, probe, height, eye, tilt)
            errors.extend(abs(expected[i]-probe['resolved_hdr'][i]) for i in range(3))
            wet += length > .01
            interfaces += interface
        assert max(errors) < .002 and wet > 0, (label, max(errors), wet, interfaces)
        self.data['pixel_counts'][label] = dict(capture=record['image'], wet_probes=wet, interfaces=interfaces,
            max_linear_error=max(errors))

    def take(self, client, session, label):
        state = self.observe(client, session, 1, .73, .37)
        self.picture(client, session, 1, label+'-released', .73, CAMERA, EYE, 0)
        self.picture(client, session, 1, label+'-released-above', .73, ABOVE, ABOVE_EYE, TILT)
        self.data['continuations'][label] = state
        return state

    def start(self, client, session, label):
        self.data['initial'].append(self.observe(client, session, 0, .85, .25))
        self.picture(client, session, 0, label+'-sealed', .85, CAMERA, EYE, 0)
        self.picture(client, session, 0, label+'-sealed-above', .85, ABOVE, ABOVE_EYE, TILT)
        self.step(client, session, 0, GATE)
        saved = client.call('save.write', dict(request_id=new_id(), configuration_generation=1,
            expected_generation=0, slot='water', session_id=session, expected_tick=1, expected_gameplay_revision=0))
        self.data['save_sha256'][label] = saved['sha256']
        return self.take(client, session, label)

    def load(self, client, old, tick, label):
        session = new_id()
        result = client.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='water',
            expected_generation=1, revision=self.revision, expected_session_id=old, expected_tick=tick,
            expected_gameplay_revision=0, new_session_id=session))
        assert result['tick'] == 1 and not result['source_stale']
        return self.take(client, session, label)

    def run(self):
        self.cli('project', 'create', self.native(self.project), '--name', 'Cistern and Basin')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=self.native(saves)))
            session = self.mount(client)
            expected = self.start(client, session, 'source')
            assert self.load(client, session, 1, 'warm') == expected
        if self.args.runtime_root:
            stage, bundle = self.root/'Built', self.root/'Relocated'
            self.cli('project','build', self.native(self.manifest), '--output', self.native(stage),
                '--runtime', self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(self.root/'Hidden source')
            game = bundle/'game.json'
            assert self.cli('game','inspect',self.native(game))['needs_water_reservoirs']
            if self.args.windows_interop:
                exe = bundle/json.loads(game.read_bytes())['engine']['executable']
                exe.chmod(exe.stat().st_mode | 0o100)
            original = tree_hashes(bundle)
            saves = self.root/'Export saves'
            saves.mkdir()
            with self.owner('export-write', game=game, saves=saves) as client:
                status = client.call('runtime.status')
                assert self.start(client, status['session_id'], 'relocated') == expected
            with self.owner('export-load', game=game, saves=saves) as client:
                status = client.call('runtime.status')
                assert self.load(client, status['session_id'], 0, 'fresh') == expected
            assert tree_hashes(bundle) == original and not self.project.exists()
            self.data['immutable_files'] = len(original)
        if self.args.gpu is not None:
            frames = self.data['pixel_counts']
            for variant in ('', '-above'):
                after = frames['source-released'+variant]['capture']
                assert after != frames['source-sealed'+variant]['capture']
                assert all(record['capture'] == after for name,record in frames.items() if name.endswith('-released'+variant))


if __name__ == '__main__':
    main(ReservoirFixture)
