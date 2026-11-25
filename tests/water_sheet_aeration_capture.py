#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare stock aerated-water scattering with a clear sheet and a ray calculation."""

import json
import math

from particle_collision_capture import main
from retained_capture import expect_error
from water_flow_particles_capture import DRY_TICK
from water_reservoir_capture import AMBIENT, AUDIO, GATE, LENS, OPTICS, TERRAIN
from water_sheet_capture import (CAMERA, EYE, SHAPE, SITES, SIZE, SheetFixture,
                                 projected_path, reflected_mix)
from water_optics_capture import intersect
from water_overflow_capture import CREST, expected_spill
from water_storage_capture import near


AERATION = .86


def smooth(low, high, value):
    u = min(1., max(0., (value - low) / (high - low)))
    return u * u * (3 - 2 * u)


def bubble_extinction(ray, distance, speed, tick):
    x, _, z = [eye + direction * distance for eye, direction in zip(EYE, ray, strict=True)]
    along, across = 1.5 - x, -5 - z
    growth = smooth(.02, .72, along / 2)
    edge = 1 - smooth(.77, 1., 2 * abs(across))
    footprint = 2 * distance * math.tan(math.radians(LENS['vertical_fov']) / 2) / SIZE[1]
    phase = along / speed - (tick % 960) / 60
    broad = math.cos(math.tau * (3.7 * across + 4 * phase)) / 2
    detail = math.cos(math.tau * (9.2 * across - 7 * phase)) / 2
    bands = .5 + .65 * broad * (1 - smooth(.035, .14, footprint))
    bands += .35 * detail * (1 - smooth(.014, .065, footprint))
    return 32 * AERATION * growth * edge * (.5 + .5 * bands)


def transport(leaving, scatter, bubble_light, water_depth, bubble_depth):
    total = water_depth + bubble_depth
    if total <= 0:
        return leaving
    source = (scatter * water_depth + bubble_light * bubble_depth) / total
    return source + (leaving - source) * math.exp(-total)


class AerationFixture(SheetFixture):
    def picture(self, client, session, tick, label):
        return self.capture(client, session, tick, label, camera=CAMERA, width=SIZE[0], height=SIZE[1],
            reuse=True, scene_product_probes=[dict(x=x,y=y) for x,y in SITES])

    def same_tick(self, client, revision, label):
        self.revision=revision
        session=self.mount(client)
        self.step(client,session,0,GATE)
        self.step(client,session,1,GATE)
        shot=self.picture(client, session, 2, label)
        client.call('runtime.stop',dict(session_id=session))
        return shot

    def author(self, client):
        revision=super().author(client)
        if self.args.gpu is not None:
            self.clear=self.same_tick(client,revision,'unmodified-clear')
        old=json.loads(next(self.project.rglob(self.data['plan_sha256']+'.pwater')).read_bytes())
        recipe=json.dumps(dict(format='poima.terrain.heightfield',version=1,samples_x=3,samples_z=3,tile_cells=2,
            origin_m=[-2,0,-7],spacing_m=[2,2],heights_m=[0]*9))
        base=dict(terrain=dict(recipe=recipe),expected_terrain_recipe_sha256=old['terrain_recipe_sha256'],
            tile_entities=[TERRAIN],grid=old['grid'],initial_depths_m=[.25],optics=OPTICS,listener_audio=AUDIO,
            storage=old['storage'])
        for invalid in (-.01,1.01,True,None):
            old['storage']['ports'][0]['sheet']=SHAPE|dict(aeration=invalid)
            expect_error(-32602,'world.water.plan',client,base|dict(revision=revision))
        for gain in (0,AERATION):
            old['storage']['ports'][0]['sheet']=SHAPE|dict(aeration=gain)
            params=base|dict(revision=revision)
            checked=client.call('world.water.plan',params)
            assert client.call('world.water.cook',params|dict(expected_plan_sha256=checked['plan_sha256']))==checked
            pin=checked['plan_sha256']
            package=json.loads(next(self.project.rglob(pin+'.pwater')).read_bytes())
            assert package['version']==23 and package['storage']['ports'][0]['sheet']['aeration']==gain
            revision=self.transact(client,revision,dict(op='water.set',asset=pin))
            assert client.call('world.dependencies')['needs_water_sheet_aeration']
            if gain==0 and self.args.gpu is not None:
                zero=self.same_tick(client,revision,'explicit-zero')
                assert zero['image']==self.clear['image']
                assert 'aerated_sheets' not in zero['report']['render_diagnostics']['water_optics']
            self.data['plan_sha256']=pin
        return revision

    def capture_spray(self, client, session, tick, label, sprites):
        state=client.call('runtime.water.inspect',dict(session_id=session,tick=tick,cells=[[0,0]]))
        port=state['storage']['ports'][0]
        stage={2:1,DRY_TICK:0,DRY_TICK+1:2}[tick]
        tank,terrain,rate=expected_spill(stage)
        sample=port['active_sheet']
        assert port['sheet']['aeration']==AERATION
        if stage:
            assert sample['aeration']==AERATION
            curvature=(CREST-terrain)/4
            speed=2/math.sqrt(2*(CREST-terrain)/9.81)
            thickness=rate/speed
            near(sample['vertical_depth_m'],thickness)
            near(port['discharge_m3_s'],rate)
        else:
            assert sample is None and port['discharge_m3_s']==0
        self.data.setdefault('sheets',{})[label]=sample
        if self.args.gpu is None:
            return
        shot=self.picture(client,session,tick,label)
        render=shot['report']['render_diagnostics']
        assert render['water_optics'].get('aerated_sheets',0)==bool(stage)
        assert not render['water_optics'].get('aerated_sheets_sun',False)
        assert render['last_draws']['sprite_instances']==sprites
        self.data['pixel_counts'][label]=dict(image=shot['image'],sprite_instances=sprites)
        if label != 'source-spray':
            return
        baseline=self.clear['report']['render_diagnostics']['scene_products']['probes']
        errors=[]
        active=dry=0
        for pixel,before,after in zip(SITES,baseline,render['scene_products']['probes'],strict=True):
            assert before['raw_hdr']==after['raw_hdr'] and before['depth']==after['depth']
            ray,maximum,length,boundary=projected_path(pixel,after,curvature,thickness)
            if intersect(EYE,ray,(-1,0,-5.5),(0,terrain,-4.5),maximum) or \
               intersect(EYE,ray,(1.5,0,-5.5),(2.5,tank,-4.5),maximum):
                continue
            if length==0:
                assert after['resolved_hdr']==before['resolved_hdr']
                dry+=1
                continue
            if boundary is None or not boundary[4]:
                continue
            # The selected rays enter once and hit neither of the storage volumes.
            rate_at_midpoint=bubble_extinction(ray,boundary[0]+length/2,speed,tick)
            mix=reflected_mix(ray,boundary,curvature,speed,tick)
            assert boundary[5]==0
            for channel,(ext,scatter) in enumerate(zip(OPTICS['extinction_per_m'],OPTICS['scattered_radiance'],strict=True)):
                medium=transport(after['raw_hdr'][channel],scatter,.82*AMBIENT[channel],ext*length,rate_at_midpoint*length)
                expected=(1-mix)*medium+mix*AMBIENT[channel]
                errors.append(abs(expected-after['resolved_hdr'][channel]))
            if before['resolved_hdr']!=after['resolved_hdr']:
                active+=1
        assert active>=3 and dry>=8 and max(errors)<.002,(active,dry,errors)
        self.data['independent_aeration']=dict(changed_sheet_probes=active,unchanged_dry_probes=dry,
            max_hdr_error=max(errors),numerical_channels=len(errors),zero_matches_clear=True)

    def run(self):
        super().run()
        if self.args.runtime_root:
            assert self.cli('game','inspect',self.native(self.root/'Relocated'/'game.json'))['needs_water_sheet_aeration']


if __name__=='__main__':
    main(AerationFixture)
