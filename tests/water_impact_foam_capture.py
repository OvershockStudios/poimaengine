#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify flow-driven surface foam, including a saved pool with its gate closed."""

import json
import math

from particle_collision_capture import main
from retained_capture import expect_error, new_id, tree_hashes
from snow_foot_release_save_refusals import reseal_slot
from water_flow_particles_capture import DRY_TICK, SEAL, SPRAY
from water_reservoir_capture import AMBIENT, AUDIO, GATE, LENS, OPTICS, TERRAIN
from water_sheet_capture import CAMERA, EYE, SHAPE, SIZE, SheetFixture, projected_path
from water_shore_foam_capture import ease, sample_noise
from water_overflow_capture import expected_spill
from water_storage_capture import near

PROFILE = dict(radius_m=.72, half_life_s=.5, spread_s=.22, volume_at_full_coverage_m3=.004)
SITES = [(x,y) for x in (40,58,76,94,112,130,148,166) for y in (115,129,143,157,171,185,199,213)]
REOPENED = DRY_TICK+1
EXPIRED = REOPENED+601
FOAM_SAVE = ('snapshot','payload','water','storage','impact_foam',0,1)


def levels(tick):
    decay=.5**(1/(60*PROFILE['half_life_s']))
    expansion=1-math.exp(-1/(60*PROFILE['spread_s']))
    core=fringe=0.
    for i in range(1,tick+1):
        q = expected_spill(1 if i==2 else 2)[2] if i in (2, REOPENED) else 0.
        moved=core*decay*expansion
        core=min(1.,core*decay-moved+q/(60*PROFILE['volume_at_full_coverage_m3']))
        fringe=min(1.,fringe*decay+moved)
        if core+fringe<.00001:
            core=fringe=0.
    return core,fringe


def coverage(pixel, target_y, probe, core, fringe):
    ray, far, _, _ = projected_path(pixel,probe,.1,.01)
    if ray[1]>=0:
        return 0.
    distance=(target_y-EYE[1])/ray[1]
    x=EYE[0]+ray[0]*distance
    z=EYE[2]+ray[2]*distance
    if not (0<distance<far and -1<=x<0 and -5.5<=z<-4.5):
        return 0.
    dx, dz=x+.5, z+5
    u,v=-dx/PROFILE['radius_m'], -dz/PROFILE['radius_m']
    radial=math.hypot(u,v)
    central=1-ease(.12,.42,math.hypot(u*1.2,v))
    outer=(1-ease(.48,1,math.hypot((u-.23)*.8,v*1.1)))*(.35+.65*ease(.05,.44,radial))
    angle=math.pi*DRY_TICK/60
    direction=(math.cos(angle),math.sin(angle))
    footprint=2*distance*math.tan(math.radians(LENS['vertical_fov'])/2)/(SIZE[1]*max(.03,abs(ray[1])))
    low=.5+(1-ease(.042,.145,footprint))*(sample_noise(dx*6.3+.5*direction[0],dz*6.3+.5*direction[1])-.5)
    high=.5+(1-ease(.017,.060,footprint))*(sample_noise(dx*16-.8*direction[0],dz*16-.8*direction[1])-.5)
    return ease(.18,.77,.64*low+.36*high)*min(.94,central*core+.82*outer*fringe)


class FoamFixture(SheetFixture):
    def picture(self,client,session,tick,label):
        return self.capture(client,session,tick,label,camera=CAMERA,width=SIZE[0],height=SIZE[1],reuse=True,
            scene_product_probes=[dict(x=x,y=y) for x,y in SITES])

    def prime(self,client,session):
        self.step(client,session,0,GATE)
        self.step(client,session,1,GATE)

    def seal(self,client,session):
        self.step(client,session,2,SEAL)
        self.skip(client,session,3,DRY_TICK-3)

    def skip(self,client,session,tick,count):
        result=client.call('runtime.step',dict(session_id=session,request_id=new_id(),expected_tick=tick,ticks=count))
        assert result['current_tick']==tick+count

    def author(self,client):
        revision=super().author(client)
        if self.args.gpu is not None:
            self.revision=revision
            session=self.mount(client)
            self.prime(client,session)
            self.seal(client,session)
            self.clear=self.picture(client,session,DRY_TICK,'sealed-without-foam')
            client.call('runtime.stop',dict(session_id=session))
        old=json.loads(next(self.project.rglob(self.data['plan_sha256']+'.pwater')).read_bytes())
        recipe=json.dumps(dict(format='poima.terrain.heightfield',version=1,samples_x=3,samples_z=3,tile_cells=2,
            origin_m=[-2,0,-7],spacing_m=[2,2],heights_m=[0]*9))
        params=dict(revision=revision,terrain=dict(recipe=recipe),expected_terrain_recipe_sha256=old['terrain_recipe_sha256'],
            tile_entities=[TERRAIN],grid=old['grid'],initial_depths_m=[.25],optics=OPTICS,listener_audio=AUDIO,storage=old['storage'])
        for invalid in (dict(radius_m=0),dict(spread_s=0),dict(half_life_s=31),dict(volume_at_full_coverage_m3=True)):
            old['storage']['ports'][0]['sheet']=SHAPE|dict(impact_foam=PROFILE|invalid)
            expect_error(-32602,'world.water.plan',client,params)
        old['storage']['ports'][0]['sheet']=SHAPE|dict(impact_foam=PROFILE)
        plan=client.call('world.water.plan',params)
        pin=plan['plan_sha256']
        assert client.call('world.water.cook',params|dict(expected_plan_sha256=pin))==plan
        assert json.loads(next(self.project.rglob(pin+'.pwater')).read_bytes())['version']==24
        self.data['plan_sha256']=pin
        revision=self.transact(client,revision,dict(op='water.set',asset=pin))
        assert client.call('world.dependencies')['needs_water_impact_foam']
        return revision

    def check(self,client,session,tick,label):
        water=client.call('runtime.water.inspect',dict(session_id=session,tick=tick,cells=[[0,0]]))
        port=water['storage']['ports'][0]
        foam=port['impact_foam']
        near(water['volume_m3'],2.33)
        core,fringe=levels(tick)
        near(foam['core'],core)
        near(foam['fringe'],fringe)
        falling=tick in (2,REOPENED)
        assert bool(port['active_sheet'])==falling
        if not falling:
            assert port['discharge_m3_s']==0
        _, emitters=self.read(client,session,tick)
        spray=emitters[SPRAY]
        if tick==DRY_TICK:
            assert spray['counts']['live']==0 and core>0 and fringe>0
        water.pop('session_id')
        row=dict(tick=tick,water=water,spray=spray)
        if self.args.gpu is None or tick==0:
            return row
        shot=self.picture(client,session,tick,label)
        diag=shot['report']['render_diagnostics']['water_optics']
        assert diag.get('impact_foam_patches',0)==bool(core or fringe)
        assert diag.get('falling_sheets',0)==falling
        row['picture']=shot['image']
        if label=='source-closed':
            base=self.clear['report']['render_diagnostics']['scene_products']['probes']
            current=shot['report']['render_diagnostics']['scene_products']['probes']
            receiver=water['cells'][0]['surface_height_m']
            changed=unchanged=0
            max_error=0.
            for pixel,before,after in zip(SITES,base,current,strict=True):
                assert before['raw_hdr']==after['raw_hdr'] and before['depth']==after['depth']
                amount=coverage(pixel,receiver,before,core,fringe)
                if amount==0:
                    assert before['resolved_hdr']==after['resolved_hdr']
                    unchanged+=1
                    continue
                expected=[a*(1-amount)+b*.82*amount for a,b in zip(before['resolved_hdr'],AMBIENT,strict=True)]
                max_error=max(max_error,*(abs(a-b) for a,b in zip(expected,after['resolved_hdr'],strict=True)))
                changed+=before['resolved_hdr']!=after['resolved_hdr']
            assert changed>=4 and unchanged>=8 and max_error<.002,(changed,unchanged,max_error)
            self.data['independent_foam']=dict(changed_probes=changed,unchanged_probes=unchanged,max_hdr_error=max_error)
        return row

    def continue_saved(self,client,session,prefix):
        sealed=self.check(client,session,DRY_TICK,prefix+'-closed')
        self.step(client,session,DRY_TICK,SEAL)
        reopened=self.check(client,session,REOPENED,prefix+'-reopened')
        self.step(client,session,REOPENED,SEAL)
        self.skip(client,session,REOPENED+1,600)
        drained=self.check(client,session,EXPIRED,prefix+'-expired')
        return [sealed,reopened,drained]

    def begin(self,client,session,prefix):
        self.check(client,session,0,prefix+'-idle')
        self.prime(client,session)
        self.check(client,session,2,prefix+'-flow')
        self.seal(client,session)
        saved=client.call('save.write',dict(request_id=new_id(),configuration_generation=1,expected_generation=0,
            slot='pool',session_id=session,expected_tick=DRY_TICK,expected_gameplay_revision=0))
        self.data['save_sha256'][prefix]=saved['sha256']
        return self.continue_saved(client,session,prefix)

    def restore_pool(self,client,old,tick,prefix):
        req=self.restore_request(old,tick,'pool')
        assert client.call('save.load',req)['tick']==DRY_TICK
        return self.continue_saved(client,req['new_session_id'],prefix)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Spillway foam')
        saves=self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision=self.author(client)
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            live=self.mount(client)
            expected=self.begin(client,live,'source')
            reseal_slot(saves/'slot-pool',saves,'forged',FOAM_SAVE,1.125)
            refusal=expect_error(-32070,'save.load',client,self.restore_request(live,EXPIRED,'forged'))
            assert 'impact foam' in refusal
            assert client.call('runtime.status')['tick']==EXPIRED
            assert self.restore_pool(client,live,EXPIRED,'warm')==expected
        self.data['continuations']['verified']=expected
        if self.args.runtime_root:
            pending,relocated=(self.root/name for name in ('Built','Relocated'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(pending),
                '--runtime',self.native(self.args.runtime_root))
            pending.rename(relocated)
            self.project.rename(self.root/'Hidden source')
            game=relocated/'game.json'
            assert self.cli('game','inspect',self.native(game))['needs_water_impact_foam']
            if self.args.windows_interop:
                exe=relocated/json.loads(game.read_bytes())['engine']['executable']
                exe.chmod(exe.stat().st_mode|0o100)
            original=tree_hashes(relocated)
            save_root=self.root/'Export saves'
            save_root.mkdir()
            with self.owner('relocated',game=game,saves=save_root) as client:
                assert self.begin(client,client.call('runtime.status')['session_id'],'relocated')==expected
            with self.owner('fresh',game=game,saves=save_root) as client:
                assert self.restore_pool(client,client.call('runtime.status')['session_id'],0,'fresh')==expected
            assert tree_hashes(relocated)==original and not self.project.exists()
            self.data['immutable_files']=len(original)


if __name__=='__main__':
    main(FoamFixture)
