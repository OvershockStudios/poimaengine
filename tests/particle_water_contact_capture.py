#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sweep accepted rain and collision particles against committed water, then replay the export."""

import json
import math
import struct

from particle_collision_capture import IDENTITY, RAIN, RED, SHELF, TERRAIN, CollisionFixture, emitter
from particle_impact_burst_capture import ImpactFixture, SKIN, SPLASH, run_fixture
from retained_capture import component, expect_error, new_id, transform, uid
from scene_capture import pixels

POOL = uid(951)
TERRAIN_HEIGHT = .08
# The water cooker samples its bed from the terrain's float32 mesh vertices.
STAGE = struct.unpack('<f',struct.pack('<f',TERRAIN_HEIGHT))[0]+.20
WATER_COLOR = [.005,.9,.9]


def segment(start, end):
    return dict(from_world=start, to_world=end)


class WaterContactFixture(ImpactFixture):
    def author(self, client):
        revision = super().author(client)
        raw = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
                              origin_m=[-3, TERRAIN_HEIGHT, -7], spacing_m=[3, 2], heights_m=[0]*9))
        terrain = client.call('world.terrain.inspect', dict(recipe=raw))
        args = dict(revision=revision, terrain=dict(recipe=raw), expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            tile_entities=[TERRAIN], grid=dict(cells_x=3,cells_z=3,cell_size_m=1,origin_x_m=-1.5,origin_z_m=-6,
            relaxation_per_second=0), initial_depths_m=[.20,.20,0]*3)
        plan = client.call('world.water.plan', args)
        assert client.call('world.water.cook', args | dict(expected_plan_sha256=plan['plan_sha256'])) == plan
        pool = client.call('entity.get', dict(id=SPLASH,component='ParticleEmitter'))['value']
        for invalid in (False, 1, 'true', None):
            expect_error(-32602, 'world.transact', client, dict(base_revision=revision, request_id=new_id(),
                ops=[component(RED, 'ParticleEmitter', emitter([1,0,0]) | dict(surface_collision='expire', water_contact=invalid))]))
        expect_error(-32602, 'world.transact', client, dict(base_revision=revision, request_id=new_id(),
            ops=[component(RED, 'ParticleEmitter', emitter([1,0,0]) | dict(water_contact=True))]))
        routes = []
        for source in (RAIN,RED):
            value = client.call('entity.get', dict(id=source,component='ParticleEmitter'))['value']
            if source == RAIN:
                # Separate the foreground raindrop from the wider splash it can cover.
                value['size_m'] = .07
            routes.append(component(source,'ParticleEmitter',value | dict(water_contact=True,
                impact_burst=value['impact_burst'] | dict(by_surface=dict(water=dict(emitter=POOL,count=1))))))
        result = self.transact(client, revision, routes+[
            dict(op='water.set', asset=plan['plan_sha256']),
            dict(op='entity.create', id=POOL, name='Water splash sprite pool'),
            component(POOL,'Transform',transform([31,5,-20])),
            component(POOL,'ParticleEmitter',pool | dict(linear_rgb=WATER_COLOR,size_m=.32))])
        assert result['committed'] and client.call('world.dependencies')['needs_particle_water_contact']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_particle_water_contact']
        return result['revision']

    def observe(self, client, session, tick):
        return CollisionFixture.observe(self, client, session, tick)

    def start_rain(self, client, session):
        state = CollisionFixture.start_rain(self, client, session)
        y_above,y_below = STAGE+.125,STAGE-.125
        vertical = segment([.18,y_above,-5.35],[.18,y_below,-5.35])
        paths = [
            vertical,
            segment([.18,y_below,-5.35],[.18,y_above,-5.35]),
            segment([1.1,y_above,-5.35],[1.1,y_below,-5.35]),
            segment([-5,y_above,-5.35],[5,y_below,-5.35]),
            segment([5,y_above,-5.35],[-5,y_below,-5.35]),
            segment([.18,y_below,-5.35],[.18,y_below-.05,-5.35]),
            segment([.18,y_above,-5.35],[.18,STAGE,-5.35]),
        ]
        params = dict(session_id=session,tick=2,segments=paths)
        results = client.call('runtime.water.inspect',params)['segments']
        assert [r is not None for r in results] == [True,False,False,True,True,False,True], results
        for i in (0,3,4,6):
            entry = results[i]
            assert entry['cell']==[1,0] and entry['normal']==[0,1,0] and not entry['offshore']
            assert math.isclose(entry['position_m'][1],STAGE,abs_tol=1e-12)
            assert math.isclose(entry['fraction'],1 if i==6 else .5,abs_tol=1e-12)
        expect_error(-32009,'runtime.water.inspect',client,params | dict(tick=1))
        expect_error(-32602,'runtime.water.inspect',client,params | dict(segments=[vertical]*65))
        expect_error(-32602,'runtime.water.inspect',client,params | dict(segments=[vertical | dict(to_world=[0,1e6+1,0])]))
        solid = next(source for source in state['sources'] if source['entity']==RED)
        assert solid['surface_collision']['water_contact']
        assert solid['surface_collision']['last_step']['first']['impact_emitter']==SPLASH
        self.data.setdefault('water_segments', []).append(results)
        return state

    def continue_after_opening(self, client, session, label):
        self.step_once(client,session,2,motions=[dict(entity=SHELF,position=[-2.4,1.1,-4.2],
            rotation=IDENTITY,duration_ticks=2)])
        third = self.observe(client,session,3)
        self.check_splashes(third,[RAIN])
        if self.args.gpu is not None:
            self.picture(client,session,3,label+'-open',True)
        self.step_once(client,session,3)
        fourth = self.observe(client,session,4)
        self.check_splashes(fourth,[RED,RAIN])
        self.data['continuations'][label] = [third,fourth]
        return [third,fourth]

    def check_splashes(self, snapshot, contacts):
        sources = {source['entity']:source for source in snapshot['sources']}
        splashes = sources[POOL]
        births = [row for row in snapshot['particles'] if row['age_ticks']==0 and row['emitter']==splashes['id']]
        assert len(births)==len(contacts) and snapshot['counts']['collided']==(2 if len(contacts)==1 else 4)
        for source,birth in zip(contacts,births,strict=True):
            trace = sources[source]['surface_collision']
            hit = trace['last_step']['first']
            assert trace['water_contact'] and hit['receiver'] is None and hit['triangle'] is None
            assert hit['surface']=='water' and hit['normal']==[0,1,0] and hit['impact_emitter']==POOL
            assert math.isclose(hit['position_m'][1],STAGE,abs_tol=1e-12)
            assert math.dist(birth['position_m'],[hit['position_m'][0],STAGE+SKIN,hit['position_m'][2]])<1e-12
            assert birth['velocity_m_s']==[0,4,0] and birth['presentation']['visible']
            assert sources[source]['impact_burst']['requested_last_step']==1
            assert all(row['id']!=hit['particle'] for row in snapshot['particles'])

    def picture(self, client, session, tick, name, visible_red):
        digest = CollisionFixture.picture(self,client,session,tick,name,visible_red)
        image = pixels(self.root/(name+'.bmp'))
        water = sum(min(g,b)>125 and r*4<min(g,b) for scan in image for r,g,b in scan)
        assert (water >= 6 if tick==3 else water==0), (name,water)
        report = self.data['captures'][name]['report']['render_diagnostics']['last_draws']
        assert report['sprite_instances']=={2:2,3:3}[tick]
        self.data['pixel_counts'][name]['splash'] = water
        return digest

    def replay(self, client, previous, tick, label):
        return CollisionFixture.replay(self,client,previous,tick,label)

    def export_fresh(self, initial):
        super().export_fresh(initial)
        assert self.cli('game','inspect',self.native(self.root/'Relocated'/'game.json'))['needs_particle_water_contact']


if __name__=='__main__':
    run_fixture(WaterContactFixture)
