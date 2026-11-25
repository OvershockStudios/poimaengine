#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify contact-positioned splash sprites, budgets and restart continuations."""

import argparse
import math
from pathlib import Path

from particle_collision_capture import BLUE, RAIN, RED, SHELF, TERRAIN, CollisionFixture, emitter, main as run_fixture
from retained_capture import component, expect_error, new_id, transform, uid
from scene_capture import pixels
from texture_fixture import png
from world_particles_contract import fixed

SPLASH, RECIPE, LOCAL_SOURCE, LOCAL_EFFECT = map(uid, range(900, 904))
ANGLE = math.pi/6
SLOPE = [-math.sin(ANGLE), math.cos(ANGLE), 0]
ROTATION = [0, 0, math.sin(ANGLE/2), math.cos(ANGLE/2)]
SKIN = .075


class ImpactFixture(CollisionFixture):
    def template_impact(self):
        return dict(emitter=LOCAL_EFFECT,count=1)

    def template_receiver(self, profile):
        return profile['emitter']

    def transact(self, client, revision, operations):
        return client.call('world.transact', dict(base_revision=revision, request_id=new_id(), ops=operations))

    def sprite(self, client):
        side = 12
        texels = []
        for y in range(side):
            for x in range(side):
                radius = math.hypot(x+0.5-side/2, y+0.5-side/2)/(side/2)
                texels.extend((255, 255, 255, round(255*max(0, min(1, 3*(1-radius))))))
        path = self.project/'Impact mask.png'
        path.write_bytes(png(side, side, texels))
        image = client.call('asset.image.import', dict(source=path.name, color_space='srgb'))
        path.rename(self.root/'Original mask.png')
        return image['asset']

    def author(self, client):
        revision = super().author(client)
        image = self.sprite(client)
        splash = emitter([.95, .005, .88]) | dict(spawn_space='local', particles_per_second=0,
            lifetime_ticks=12, live_limit=5, size_m=.25, offset_m=fixed([0,SKIN,0]),
            velocity_m_s=fixed([0,4,0]), sprite=dict(texture=image, depth_fade_m=.025))
        source = emitter([1, .005, .005]) | dict(surface_collision='expire', impact_burst=dict(emitter=SPLASH, count=2))
        rain = emitter([.01, .6, .06]) | dict(surface_collision='expire', particles_per_second=0,
            rain=dict(nm_per_particle=1000, max_particles_per_tick=1), impact_burst=dict(emitter=SPLASH, count=1))
        invalid = [source | dict(impact_burst=dict(emitter=target, count=count))
                   for target,count in ((RED,2),(TERRAIN,2),(uid(999),2),(RAIN,2),(BLUE,9),(BLUE,True))]
        invalid.append({key:value for key,value in source.items() if key!='surface_collision'})
        for configuration in invalid:
            params = dict(base_revision=revision, request_id=new_id(), ops=[component(RED,'ParticleEmitter',configuration)])
            expect_error(-32602, 'world.transact', client, params)
            assert client.call('world.inspect')['revision']==revision
        source_child = source | dict(impact_burst=self.template_impact(),
                                     velocity_m_s=fixed([0,96,0]))
        template = dict(op='template.set', id=RECIPE, name='Paired splash emitters', root=LOCAL_SOURCE,
            entities={
                LOCAL_SOURCE:dict(name='Impact source', parent=None, components=dict(
                    Transform=transform([.18,-2,-6.3]), ParticleEmitter=source_child)),
                LOCAL_EFFECT:dict(name='Splash target', parent=LOCAL_SOURCE, components=dict(
                    Transform=transform([1,0,0]), ParticleEmitter=splash)),
            })
        result = self.transact(client, revision, [
            dict(op='entity.create', id=SPLASH, name='Detached impact sprite pool'),
            component(SPLASH, 'Transform', transform([30,5,-20])),
            component(SPLASH, 'ParticleEmitter', splash),
            component(RED, 'ParticleEmitter', source), component(RAIN, 'ParticleEmitter', rain),
            component(SHELF, 'Transform', transform([-.72,1.1,-4.2], ROTATION)), template])
        manifest = client.call('world.dependencies')
        assert manifest['needs_particle_impact_bursts'] and manifest['needs_sprite_particles']
        return result['revision']

    def check_hit(self, snapshot, source, receiver, height, triangle):
        if receiver != SHELF:
            return super().check_hit(snapshot, source, receiver, height, triangle)
        row = next(row for row in snapshot['sources'] if row['entity']==source)
        step = row['surface_collision']['last_step']
        hit = step['first']
        assert step['tick']==snapshot['tick'] and step['tested']==step['hits']==1
        assert hit['receiver']==SHELF and hit['triangle'] is None
        assert abs(hit['position_m'][1]-(1.1+.05/SLOPE[1])) < .0001
        assert math.dist(hit['normal'],SLOPE) < .0001
        assert all(particle['id']!=hit['particle'] for particle in snapshot['particles'])
        return row

    def observe(self, client, session, tick):
        observation = super().observe(client, session, tick)
        if tick not in (2,3,4):
            return observation
        sources = {source['entity']:source for source in observation['sources']}
        restored = sources[RED]['surface_collision']['last_step'] is None
        target = sources[SPLASH]
        assert target['position_m']==[30,5,-20]
        assert target['counts']['live']=={2:2,3:3,4:5}[tick]
        assert target['counts']['dropped']==int(tick==4)
        births = [row for row in observation['particles'] if row['age_ticks']==0 and row['emitter']==target['id']]
        assert len(births)=={2:2,3:1,4:2}[tick]
        if not restored:
            offered = RAIN if tick==3 else RED
            origin = sources[offered]['surface_collision']['last_step']['first']
            expected_outward = origin['normal']
            expected_center = [origin['position_m'][i]+SKIN*expected_outward[i] for i in range(3)]
            assert sources[offered]['impact_burst']['requested_last_step']==(1 if tick==3 else 2)
            if tick==4:
                assert sources[RAIN]['impact_burst']['requested_last_step']==1
            for particle in births:
                assert math.dist(particle['position_m'],expected_center)<.00005
                assert math.dist(particle['velocity_m_s'],[4*x for x in expected_outward])<.00005
                assert particle['presentation']['visible']
        return observation

    def picture(self, client, session, tick, name, visible_red):
        digest = super().picture(client, session, tick, name, visible_red)
        image = pixels(self.root/(name+'.bmp'))
        splash = sum(r>125 and b>125 and g*2<min(r,b) for row in image for r,g,b in row)
        report = self.data['captures'][name]['report']['render_diagnostics']['last_draws']
        assert splash >= 6 and report['sprite_instances']=={2:2,3:3}[tick], (name, splash, report)
        self.data['pixel_counts'][name]['splash'] = splash
        return digest

    def replay(self, client, previous, tick, label):
        restored = super().replay(client, previous, tick, label)
        if label=='restored':
            status = client.call('runtime.status')
            session = status['session_id']
            result = client.call('runtime.structure.transact', dict(session_id=session, request_id=new_id(), expected_tick=4,
                expected_structure_revision=0, spawns=[dict(template_id=RECIPE)]))
            root = result['spawned'][0]
            instance = client.call('runtime.instance', dict(session_id=session,tick=4,id=root))
            for current in (4,5,6):
                self.step_once(client,session,current,expected_structure_revision=1)
            observation = self.observe(client,session,7)
            sources = {source['entity']:source for source in observation['sources']}
            routed = sources[root]['impact_burst']
            target = sources[instance['nodes'][LOCAL_EFFECT]]
            assert self.template_receiver(routed)==target['entity'] and target['entity']!=LOCAL_EFFECT
            assert routed['requested_last_step']==1 and target['counts']['spawned']==1
            particle, = [row for row in observation['particles'] if row['emitter']==target['id']]
            assert particle['age_ticks']==0 and math.dist(particle['velocity_m_s'],[0,-4,0])<.00005
            assert abs(particle['position_m'][1]-(.08-SKIN))<.00005
            self.data['template_remap'] = True
            self.data['template_underside_velocity_m_s'] = particle['velocity_m_s']
        return restored

    def export_fresh(self, initial):
        super().export_fresh(initial)
        bundle = self.root/'Relocated'/'game.json'
        assert self.cli('game', 'inspect', self.native(bundle))['needs_particle_impact_bursts']


if __name__=='__main__':
    run_fixture(ImpactFixture)
