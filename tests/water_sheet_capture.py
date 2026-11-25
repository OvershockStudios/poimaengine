#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare a visible spill sheet to a same-tick control and restore its saved discharge."""

import json
import math

from particle_collision_capture import main
from retained_capture import component, expect_error, transform, uid
from water_flow_particles_capture import DRY_TICK, FlowFixture
from water_optics_capture import fresnel, intersect
from water_overflow_capture import CREST, expected_spill
from water_reservoir_capture import AMBIENT, AUDIO, GATE, LENS, OPTICS, TERRAIN
from water_storage_capture import near

CAMERA = uid(1018)
EYE = [.45, 1.43, -2.55]
TILT = math.radians(-20)
SIZE = (360, 240)
SITES = [(x, y) for y in (70, 85, 100, 115, 130) for x in (60, 110, 165, 200, 245, 295)]
SHAPE = dict(lip_xz_m=[1.5, -5], outlet_xz_m=[-.5, -5], outlet_floor_m=0)


def projected_path(pixel, probe, curvature, thickness):
    width,height=SIZE
    half_angle=math.tan(math.radians(LENS['vertical_fov'])/2)
    focal=height/(2*half_angle)
    x=(pixel[0]+.5-width/2)/focal
    y=(height/2-pixel[1]-.5)/focal
    scale=math.hypot(x,y,1)
    ray=(x/scale,(y*math.cos(TILT)+math.sin(TILT))/scale,(y*math.sin(TILT)-math.cos(TILT))/scale)
    depth=probe['depth']
    maximum=scale*LENS['near']/(1-depth+depth*LENS['near']/LENS['far'])
    footprint=intersect(EYE,ray,(-.5,-100,-5.5),(1.5,100,-4.5),maximum)
    if not footprint:
        return ray,maximum,0.,None
    start, end=footprint
    s, velocity=1.5-EYE[0],-ray[0]
    a=curvature*velocity*velocity
    b=ray[1]+2*curvature*s*velocity
    c=EYE[1]-CREST+curvature*s*s
    roots=[start,end]
    for level in (0,thickness):
        delta=b*b-4*a*(c-level)
        if a>1e-15 and delta>0:
            roots.extend(t for t in ((-b-math.sqrt(delta))/(2*a),(-b+math.sqrt(delta))/(2*a)) if start<t<end)
        if a<=1e-15 and b and start<(level-c)/b<end:
            roots.append((level-c)/b)
    length=0.
    boundary=None
    ordered=sorted(roots)
    for lo,hi in zip(ordered,ordered[1:]):
        mid=(lo+hi)/2
        if 0<(a*mid+b)*mid+c<thickness:
            if boundary is None and (lo>start or hi<end):
                enters=lo>start
                at=lo if enters else hi
                outer=(a*at+b)*at+c>thickness/2
                boundary=(at,s+at*velocity,-5-EYE[2]-at*ray[2],1 if outer else -1,enters,length+(at-lo))
            length+=hi-lo
    return ray,maximum,length,boundary


def reflected_mix(ray,boundary,curvature,speed,tick):
    if boundary is None:
        return 0.
    distance,along,across,side,enters,_=boundary
    footprint=2*distance*math.tan(math.radians(LENS['vertical_fov'])/2)/SIZE[1]
    u=max(0,min(1,(footprint-.006)/(.06-.006)))
    fade=1-u*u*(3-2*u)
    phase=along/speed-tick/60
    a=math.tau*(4*across+3*phase)
    b=math.tau*(11*across-5*phase)
    dx=2*curvature*along-fade*.025*math.sin(a-b)
    dz=-fade*(.055*math.cos(a)+.02*math.cos(b))
    norm=math.hypot(dx,1,dz)
    face=side if enters else -side
    nv=max(0,min(1,face*(dx*ray[0]-ray[1]+dz*ray[2])/norm))
    return fresnel(-nv if enters else nv)


class SheetFixture(FlowFixture):
    def author(self, client):
        revision=super().author(client)
        revision=self.transact(client,revision,dict(op='entity.create',id=CAMERA,name='Spillway view'),
            component(CAMERA,'Transform',transform(EYE)|dict(rotation=[math.sin(TILT/2),0,0,math.cos(TILT/2)])),
            component(CAMERA,'Camera',LENS))
        self.revision=revision
        if self.args.gpu is not None:
            session=self.mount(client)
            self.step(client,session,0,GATE)
            self.step(client,session,1,GATE)
            self.control=self.capture(client,session,2,'control-water',reuse=True,camera=CAMERA,width=SIZE[0],height=SIZE[1],
                scene_product_probes=[dict(x=x,y=y) for x,y in SITES])
            assert 'falling_sheets' not in self.control['report']['render_diagnostics']['water_optics']
            client.call('runtime.stop',dict(session_id=session))
        old=json.loads(next(self.project.rglob(self.data['plan_sha256']+'.pwater')).read_bytes())
        old['storage']['ports'][0]['sheet']=SHAPE
        recipe=json.dumps(dict(format='poima.terrain.heightfield',version=1,samples_x=3,samples_z=3,tile_cells=2,
            origin_m=[-2,0,-7],spacing_m=[2,2],heights_m=[0]*9))
        params=dict(revision=revision,terrain=dict(recipe=recipe),expected_terrain_recipe_sha256=old['terrain_recipe_sha256'],
            tile_entities=[TERRAIN],grid=old['grid'],initial_depths_m=[.25],optics=OPTICS,listener_audio=AUDIO,storage=old['storage'])
        for bad in (SHAPE|dict(outlet_floor_m=.7),SHAPE|dict(outlet_xz_m=[2,-5]),SHAPE|dict(lip_xz_m=[-.5,-5])):
            old['storage']['ports'][0]['sheet']=bad
            expect_error(-32602,'world.water.plan',client,params)
        old['storage']['ports'][0]['sheet']=SHAPE
        plan=client.call('world.water.plan',params)
        assert client.call('world.water.cook',params|dict(expected_plan_sha256=plan['plan_sha256']))==plan
        self.data['plan_sha256']=pin=plan['plan_sha256']
        assert json.loads(next(self.project.rglob(pin+'.pwater')).read_bytes())['version']==22
        revision=self.transact(client,revision,dict(op='water.set',asset=pin))
        assert client.call('world.dependencies')['needs_water_sheets']
        return revision

    def capture_spray(self, client, session, tick, label, sprites):
        state=client.call('runtime.water.inspect',dict(session_id=session,tick=tick,cells=[[0,0]]))
        spill=state['storage']['ports'][0]
        sample=spill['active_sheet']
        step={2:1,DRY_TICK:0,DRY_TICK+1:2}[tick]
        if step:
            tank,terrain,rate=expected_spill(step)
            drop=CREST-terrain
            speed=2/math.sqrt(2*drop/9.81)
            thickness=rate/speed
            near(sample['vertical_depth_m'],thickness)
            near(sample['drop_per_m2'],drop/4)
            assert sample['length_m']==2 and sample['forward_xz']==[-1,0]
            near(spill['discharge_m3_s'],rate)
        else:
            assert sample is None and spill['discharge_m3_s']==0
        self.data.setdefault('sheets',{})[label]=sample
        if self.args.gpu is None:
            return
        shot=self.capture(client,session,tick,label,camera=CAMERA,reuse=True,width=SIZE[0],height=SIZE[1],
            scene_product_probes=[dict(x=x,y=y) for x,y in SITES])
        diagnostics=shot['report']['render_diagnostics']
        assert diagnostics['water_optics'].get('falling_sheets',0)==bool(step)
        assert diagnostics['last_draws']['sprite_instances']==sprites
        self.data['pixel_counts'][label]=dict(image=shot['image'],sprite_instances=sprites)
        if label!='source-spray':
            return
        baseline=self.control['report']['render_diagnostics']['scene_products']['probes']
        selected=diagnostics['scene_products']['probes']
        errors,wet,dry=[],0,0
        for pixel,before,after in zip(SITES,baseline,selected,strict=True):
            assert before['raw_hdr']==after['raw_hdr'] and before['depth']==after['depth']
            ray,maximum,length,boundary=projected_path(pixel,after,drop/4,thickness)
            if intersect(EYE,ray,(-1,0,-5.5),(0,terrain,-4.5),maximum) or \
               intersect(EYE,ray,(1.5,0,-5.5),(2.5,tank,-4.5),maximum):
                continue
            assert before['raw_hdr']==before['resolved_hdr']
            if length==0:
                assert after['resolved_hdr']==before['resolved_hdr']
                dry+=1
                continue
            mix=reflected_mix(ray,boundary,drop/4,speed,tick)
            for i,(extinction,scatter) in enumerate(zip(OPTICS['extinction_per_m'],OPTICS['scattered_radiance'])):
                straight=scatter+(after['raw_hdr'][i]-scatter)*math.exp(-extinction*length)
                reflected=scatter
                if boundary is not None and boundary[4]:
                    reflected+=(AMBIENT[i]-scatter)*math.exp(-extinction*boundary[5])
                expected=straight*(1-mix)+reflected*mix
                errors.append(abs(expected-after['resolved_hdr'][i]))
            wet+=1
        assert wet>=2 and dry>=2 and max(errors)<.002,(wet,dry,errors)
        self.data['independent_optics']=dict(sheet_probes=wet,unchanged_dry_probes=dry,max_linear_error=max(errors))

    def run(self):
        super().run()
        assert self.data['sheets']['source-spray']==self.data['sheets']['warm-spray']
        if self.args.runtime_root:
            assert self.cli('game','inspect',self.native(self.root/'Relocated'/'game.json'))['needs_water_sheets']


if __name__=='__main__':
    main(SheetFixture)
