#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check collider names, surface-dependent VFX, and relocated gameplay."""

import math

from particle_collision_capture import RAIN, RED, SHELF, TERRAIN, CollisionFixture
from particle_impact_burst_capture import ImpactFixture, LOCAL_EFFECT, SKIN, SPLASH, run_fixture
from retained_capture import component, expect_error, new_id, transform, uid
from scene_capture import pixels

METAL = uid(950)
METAL_COLOR = [.95,.85,.005]


class SurfaceFixture(ImpactFixture):
    def template_impact(self):
        return dict(by_surface=dict(stone=dict(emitter=LOCAL_EFFECT,count=1)))

    def template_receiver(self, profile):
        assert 'emitter' not in profile
        return profile['by_surface']['stone']['emitter']

    def author(self, client):
        revision = super().author(client)

        def authored(entity, kind):
            return client.call('entity.get',dict(id=entity,component=kind,revision=revision))['value']

        colliders = [(SHELF,'BoxCollider','metal'),(TERRAIN,'MeshCollider','stone')]
        operations = []
        for entity,kind,surface in colliders:
            collider = authored(entity,kind)
            for invalid in ('', 'Uppercase', 'x'*33, True, None):
                expect_error(-32602,'world.transact',client,dict(base_revision=revision,request_id=new_id(),
                    ops=[component(entity,kind,collider | dict(surface=invalid))]))
                assert client.call('entity.get',dict(id=entity,component=kind))['value']==collider
            operations.append(component(entity,kind,collider | dict(surface=surface)))

        red = authored(RED,'ParticleEmitter')
        valid = dict(emitter=METAL,count=2)
        for profile in ({}, dict(emitter=SPLASH,by_surface=dict(metal=valid)),dict(by_surface={}),
                        dict(by_surface={'BAD TAG':valid}),
                        dict(by_surface=dict(metal=dict(emitter=uid(999),count=2))),
                        dict(by_surface={f'name{i}':None for i in range(9)}),
                        dict(by_surface=dict(metal=dict(emitter=SPLASH,count=False)))):
            expect_error(-32602,'world.transact',client,dict(base_revision=revision,request_id=new_id(),
                ops=[component(RED,'ParticleEmitter',red | dict(impact_burst=profile))]))
            assert client.call('world.inspect')['revision']==revision
        routed = red | dict(impact_burst=red['impact_burst'] | dict(by_surface=dict(metal=valid)))
        rain = authored(RAIN,'ParticleEmitter')
        rain['impact_burst']['by_surface'] = dict(stone=None)
        splash = authored(SPLASH,'ParticleEmitter')
        operations.extend((dict(op='entity.create',id=METAL,name='Warm metal sparks'),
            component(METAL,'Transform',transform([32,5,-20])),
            component(METAL,'ParticleEmitter',splash | dict(linear_rgb=METAL_COLOR)),
            component(RED,'ParticleEmitter',routed),component(RAIN,'ParticleEmitter',rain)))
        committed = self.transact(client,revision,operations)
        assert committed['committed'] and client.call('world.dependencies')['needs_collision_surfaces']
        return committed['revision']

    def start_rain(self, client, session):
        observed = super().start_rain(client,session)
        results = []
        families = [([-.72,1.4,-4.2],1,SHELF,'metal'),([.18,1,-5.35],1,TERRAIN,'stone'),
                    ([-.72,2.2,-4.2],.3,RED,None),([8,2,1],1,None,None)]
        for origin,distance,entity,surface in families:
            params = dict(session_id=session,tick=2,origin=origin,direction=[0,-1,0],distance=distance)
            tagged = client.call('runtime.surface.raycast',params)
            legacy = client.call('runtime.raycast',params)
            hit = tagged['hit']
            if entity is None:
                assert hit is None and tagged==legacy
            else:
                assert hit['entity']==entity and hit['surface']==surface
                assert 'surface' not in legacy['hit']
                assert {k:v for k,v in hit.items() if k!='surface'}==legacy['hit']
            results.append(hit)
        expect_error(-32009,'runtime.surface.raycast',client,params | dict(tick=1))
        self.data.setdefault('surface_rays',[]).append(results)
        return observed

    def observe(self, client, session, tick):
        observation = CollisionFixture.observe(self,client,session,tick)
        if tick not in (2,3,4):
            return observation
        sources = {row['entity']:row for row in observation['sources']}
        assert sources[METAL]['counts']['live']==2
        assert sources[SPLASH]['counts']['live']==(2 if tick==4 else 0)
        assert sources[RAIN]['impact_burst']['by_surface']=={'stone':None}
        if not sources[RED]['surface_collision']['last_step']:
            return observation
        if tick in (3,4):
            rain = sources[RAIN]
            first = rain['surface_collision']['last_step']['first']
            assert first['surface']=='stone' and first['impact_emitter'] is None
            assert rain['impact_burst']['requested_last_step']==0
        if tick in (2,4):
            tag,target = ('metal',METAL) if tick==2 else ('stone',SPLASH)
            red = sources[RED]
            hit = red['surface_collision']['last_step']['first']
            assert hit['surface']==tag and hit['impact_emitter']==target
            assert red['impact_burst']['requested_last_step']==2
            births = [row for row in observation['particles'] if row['emitter']==sources[target]['id'] and row['age_ticks']==0]
            assert len(births)==2
            for particle in births:
                center = [hit['position_m'][axis]+SKIN*hit['normal'][axis] for axis in range(3)]
                assert math.dist(particle['position_m'],center)<.00005
        return observation

    def picture(self,client,session,tick,name,visible_red):
        digest = CollisionFixture.picture(self,client,session,tick,name,visible_red)
        image = pixels(self.root/(name+'.bmp'))
        metal = sum(r>125 and g>125 and b*2<min(r,g) for row in image for r,g,b in row)
        stone = sum(r>125 and b>125 and g*2<min(r,b) for row in image for r,g,b in row)
        assert metal>=6 and (stone>=6 if tick==4 else stone==0), (name,metal,stone)
        self.data['pixel_counts'][name].update(metal=metal,stone=stone)
        return digest

    def continue_after_opening(self, client, session, label):
        result = super().continue_after_opening(client,session,label)
        if self.args.gpu is not None:
            self.picture(client,session,4,label+'-stone',True)
        return result

    def run(self):
        super().run()
        if self.args.gpu is not None:
            stone = {row['image_sha256'] for name,row in self.data['pixel_counts'].items() if name.endswith('-stone')}
            assert len(stone)==1

    def export_fresh(self, initial):
        super().export_fresh(initial)
        game = self.root/'Relocated'/'game.json'
        assert self.cli('game','inspect',self.native(game))['needs_collision_surfaces']


if __name__=='__main__':
    run_fixture(SurfaceFixture)
