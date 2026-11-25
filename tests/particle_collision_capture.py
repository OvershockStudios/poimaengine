#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive opt-in collision, moving cover, terrain and rain in native Poima."""

import argparse
import json
import math
from pathlib import Path
import tempfile

from retained_capture import CAMERA, Fixture, component, expect_error, file_hash, new_id, transform, tree_hashes, uid
from scene_capture import pixels
from world_particles_contract import fixed

RED, BLUE, RAIN, SHELF, SLAB, TERRAIN, DYNAMIC = map(uid, range(801, 808))
SIZE = (240, 160)
ORIGIN = [-.72, 2, -4.2]
IDENTITY = [0, 0, 0, 1]


def emitter(rgb):
    return dict(spawn_space='world', simulation_space='world', lifetime_ticks=20, live_limit=16,
                particles_per_second=60, size_m=.22, linear_rgb=rgb, velocity_m_s=fixed([0, -96, 0]))


def box(motion, extents):
    return dict(motion=motion, half_extents=extents, mass=1, friction=.5, restitution=0)


class CollisionFixture(Fixture):
    def author(self, client):
        revision = client.call('world.inspect')['revision']
        raw = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
                              origin_m=[-3, .08, -7], spacing_m=[3, 2], heights_m=[0]*9))
        plan = client.call('world.terrain.inspect', dict(recipe=raw))
        assert client.call('world.terrain.cook', dict(recipe=raw, expected_recipe_sha256=plan['recipe_sha256'])) == plan
        tile, = plan['tiles']
        mesh = dict(asset=tile['asset'], primitive=tile['primitive'])
        red = emitter([1, .005, .005]) | dict(surface_collision='expire')
        blue = emitter([.005, .005, 1])
        rain = emitter([.01, .6, .06]) | dict(surface_collision='expire', particles_per_second=0,
            rain=dict(nm_per_particle=1000, max_particles_per_tick=1))
        operations = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
                      dict(op='component.remove', id=uid(1), type='BoxCollider'),
                      dict(op='component.remove', id=uid(5), type='Light'),
                      component(uid(4), 'LightingEnvironment', dict(ambient=[.10]*3, exposure=1))]
        for id_, name, position, components in (
            (CAMERA, 'Particle observation', [0, 1, 0], dict(Camera=dict(vertical_fov=50, near=.1, far=80))),
            (RED, 'World sparks', ORIGIN, dict(ParticleEmitter=red, BoxCollider=box('static', [.05]*3))),
            (BLUE, 'Comparison sparks', [.72, 2, -4.2], dict(ParticleEmitter=blue)),
            (RAIN, 'Rain over terrain', [.18, 2, -5.35], dict(ParticleEmitter=rain)),
            (SHELF, 'Sliding ledge', [-.72, 1.1, -4.2], dict(BoxCollider=box('kinematic', [.40, .05, .5]))),
            (DYNAMIC, 'Loose cover', [-.72, 1.65, -4.2], dict(BoxCollider=box('dynamic', [.15, .04, .15]))),
            (TERRAIN, 'Cooked terrain', [0, 0, 0], dict(StaticMesh=mesh | dict(visible=True),
                                                    MeshCollider=mesh | dict(friction=.5, restitution=0))),
        ):
            operations.extend([dict(op='entity.create', id=id_, name=name), component(id_, 'Transform', transform(position))])
            operations.extend(component(id_, key, config) for key,config in components.items())
        operations.extend([dict(op='entity.create', id=SLAB, name='Ledge mesh', parent=SHELF),
            component(SLAB, 'Transform', transform([0, 0, 0]) | dict(scale=[.8, .1, 1])),
            component(SLAB, 'MeshRenderer', dict(primitive='box', albedo=[.08, .08, .08], visible=True))])
        for wrong in (red | dict(simulation_space='local'), red | dict(live_limit=0), red | dict(live_limit=65),
                      red | dict(surface_collision=True), red | dict(surface_collision='bounce')):
            invalid = [op if op.get('id') != RED or op.get('type') != 'ParticleEmitter' else component(RED, 'ParticleEmitter', wrong)
                       for op in operations]
            expect_error(-32602, 'world.transact', client, dict(base_revision=revision, request_id=new_id(), ops=invalid))
            assert client.call('world.inspect')['revision'] == revision
        result = client.call('world.transact', dict(base_revision=revision, request_id=new_id(), ops=operations))
        assert result['committed']
        assert client.call('world.dependencies')['needs_particle_collisions']
        return result['revision']

    def observe(self, client, session, tick):
        result = client.call('runtime.particles.inspect', dict(session_id=session, tick=tick, limit=32))
        assert result['tick'] == tick and result['session_id'] == session and result['next_after'] is None
        for row in [result, *result['sources']]:
            counts = row['counts']
            assert counts['spawned'] == sum(counts.get(name, 0) for name in ('live','expired','removed','collided'))
        return {key:value for key,value in result.items() if key!='session_id'}

    def check_hit(self, snapshot, source, receiver, height, triangle):
        observed = next(row for row in snapshot['sources'] if row['entity']==source)
        step = observed['surface_collision']['last_step']
        hit = step['first']
        assert step['tick'] == snapshot['tick'] and step['tested'] >= step['hits'] == 1 and not step['skipped'], step
        assert hit['receiver'] == receiver and (hit['triangle'] is not None) is triangle, hit
        assert abs(hit['position_m'][1]-height) < .0001 and abs(hit['normal'][1]-1) < .0001, hit
        assert all(row['id'] != hit['particle'] for row in snapshot['particles'])
        return observed

    def step_once(self, client, session, tick, **options):
        response = client.call('runtime.step', dict(session_id=session, request_id=new_id(), expected_tick=tick, ticks=1, **options))
        assert response['current_tick'] == tick+1

    def picture(self, client, session, tick, name, visible_red):
        row = self.capture(client, session, tick, name, width=SIZE[0], height=SIZE[1], reuse=True)
        image = pixels(self.root/(name+'.bmp'))
        lower = [rgb for scan in image[SIZE[1]//2+8:] for rgb in scan]
        red = sum(r>120 and r>4*g and r>4*b for r,g,b in lower)
        blue = sum(b>120 and b>4*g and b>4*r for r,g,b in lower)
        assert blue > 10 and (red > 10 if visible_red else red == 0), (name, red, blue)
        self.data['pixel_counts'][name] = dict(red=red, blue=blue, image_sha256=row['image'])
        return row['image']

    def start_rain(self, client, session):
        result = client.call('runtime.wetness.step', dict(session_id=session, request_id=new_id(), expected_tick=0,
            ticks=1, wetness_dose=dict(first_quantum=0, rain_nm=[60_000])))
        assert result['current_tick'] == 1
        born = self.observe(client, session, 1)
        assert len(born['particles']) == 3 and all(row['age_ticks']==0 for row in born['particles'])
        self.step_once(client, session, 1)
        snapshot = self.observe(client, session, 2)
        self.check_hit(snapshot, RED, SHELF, 1.15, False)
        assert snapshot['counts']['collided'] == 1
        geometric = client.call('runtime.raycast', dict(session_id=session, tick=2, origin=ORIGIN,
            direction=[0,-1,0], distance=1.6, ignore=[RED]))['hit']
        assert geometric['entity'] == DYNAMIC
        self.data['initial'].append(snapshot)
        return snapshot

    def continue_after_opening(self, client, session, label):
        self.step_once(client, session, 2, motions=[dict(entity=SHELF, position=[-2.4,1.1,-4.2],
            rotation=IDENTITY, duration_ticks=2)])
        third = self.observe(client, session, 3)
        self.check_hit(third, RAIN, TERRAIN, .08, True)
        assert third['counts']['collided'] == 2
        if self.args.gpu is not None:
            self.picture(client, session, 3, label+'-open', True)
        self.step_once(client, session, 3)
        fourth = self.observe(client, session, 4)
        self.check_hit(fourth, RED, TERRAIN, .08, True)
        self.check_hit(fourth, RAIN, TERRAIN, .08, True)
        assert fourth['counts']['collided'] == 4
        self.data['continuations'][label] = [third, fourth]
        return [third, fourth]

    def record(self, client, session, saves, label):
        self.start_rain(client, session)
        if self.args.gpu is not None:
            self.picture(client, session, 2, label+'-closed', False)
        saved = self.save(client, session, 2, 'contacts')
        path, = (saves/'slot-contacts').glob('p-*.bin')
        assert file_hash(path) == saved['sha256']
        state = json.loads(path.read_bytes())['snapshot']['payload']['particles']['state']
        assert state['status']['particles']['collided'] == 1
        self.data['save_sha256'][label] = saved['sha256']
        return self.continue_after_opening(client, session, label)

    def replay(self, client, previous, tick, label):
        session = new_id()
        reply = client.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='contacts',
            expected_generation=1, revision=self.revision, expected_session_id=previous, expected_tick=tick,
            expected_gameplay_revision=0, expected_structure_revision=0, new_session_id=session))
        assert reply['tick'] == 2 and reply['session_id'] == session and not reply['source_stale']
        restored = self.observe(client, session, 2)
        assert restored['counts']['collided'] == 1
        for row in restored['sources']:
            if 'surface_collision' in row:
                assert row['surface_collision']['last_step'] is None
        if self.args.gpu is not None:
            self.picture(client, session, 2, label+'-closed', False)
        return self.continue_after_opening(client, session, label)

    def run(self):
        self.cli('project', 'create', self.native(self.project), '--name', 'Particle Contact Yard')
        source_saves = self.root/'Source saves'
        source_saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=self.native(source_saves)))
            session = self.mount(client)
            initial = self.record(client, session, source_saves, 'source')
            assert self.replay(client, session, 4, 'restored') == initial
        if self.args.runtime_root:
            self.export_fresh(initial)
        for stage in ('closed', 'open'):
            images = {row['image_sha256'] for name,row in self.data['pixel_counts'].items() if name.endswith('-'+stage)}
            assert len(images) == int(self.args.gpu is not None)

    def export_fresh(self, initial):
        stage, bundle, hidden = (self.root/name for name in ('Built', 'Relocated', 'Hidden source'))
        self.cli('project', 'build', self.native(self.manifest), '--output', self.native(stage), '--runtime', self.native(self.args.runtime_root))
        stage.rename(bundle)
        self.project.rename(hidden)
        game = bundle/'game.json'
        assert self.cli('game', 'inspect', self.native(game))['needs_particle_collisions']
        if self.args.windows_interop:
            binary = bundle/json.loads(game.read_bytes())['engine']['executable']
            binary.chmod(binary.stat().st_mode | 0o100)
        before = tree_hashes(bundle)
        saves = self.root/'Export saves'
        saves.mkdir()
        with self.owner('relocated', game=game, saves=saves) as client:
            state = client.call('runtime.status')
            assert state['active'] and state['tick'] == 0
            assert self.record(client, state['session_id'], saves, 'relocated') == initial
        with self.owner('fresh-save', game=game, saves=saves) as client:
            state = client.call('runtime.status')
            assert state['active'] and state['tick'] == 0
            assert self.replay(client, state['session_id'], 0, 'fresh-save') == initial
        assert not self.project.exists() and tree_hashes(bundle)==before
        self.data['immutable_files'] = len(before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--runtime-root', type=Path)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run the fixture with assertions enabled.')
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=args.output, prefix='particle-contact-'))
    data = dict(passed=False, binary_sha256=file_hash(args.binary), captures={}, owners=[], cli=[],
                initial=[], continuations={}, save_sha256={}, pixel_counts={})
    try:
        CollisionFixture(args, root, data).run()
        data['passed'] = True
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(data, indent=2, allow_nan=False)+'\n')
        print(path, flush=True)


if __name__ == '__main__':
    main()
