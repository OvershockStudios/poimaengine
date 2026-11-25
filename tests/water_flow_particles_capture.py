#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Follow committed spill and supply volumes through saved particle emission."""

import json
import math

from particle_collision_capture import main
from particle_impact_burst_capture import ImpactFixture
from retained_capture import component, expect_error, new_id, transform, tree_hashes, uid
from snow_foot_release_save_refusals import reseal_slot
from water_overflow_capture import OverflowFixture, expected_spill
from water_reservoir_capture import ABOVE, AUDIO, GATE, OPTICS, TERRAIN

SPRAY, CAPPED, REVERSE = map(uid, (1010, 1011, 1012))
SEAL = 'sluice.cutoff'
QUANTUM = 300_000
LIFETIME = 12
DRY_TICK = 2 + LIFETIME
PHASE_PATH = ('snapshot', 'payload', 'particles', 'sources', 0, 'water_flow')


def flow(port, quantum, maximum, direction='forward'):
    return dict(port=port, microlitres_per_particle=quantum, max_particles_per_tick=maximum, direction=direction)


def microlitres(rate):
    return math.floor(rate / 60 * 1e9 + .5)


class FlowFixture(OverflowFixture):
    sprite = ImpactFixture.sprite

    def author(self, client):
        revision = super().author(client)
        asset, = self.project.rglob(self.data['plan_sha256']+'.pwater')
        existing = json.loads(asset.read_bytes())
        recipe = json.dumps(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3, tile_cells=2,
            origin_m=[-2, 0, -7], spacing_m=[2, 2], heights_m=[0]*9))
        spill, supply = existing['storage']['ports']
        spill.update(action=SEAL, action_openness=0)
        supply['from'], supply['to'] = supply['to'], supply['from']
        params = dict(revision=revision, terrain=dict(recipe=recipe), expected_terrain_recipe_sha256=existing['terrain_recipe_sha256'],
            tile_entities=[TERRAIN], grid=existing['grid'], initial_depths_m=[.25], optics=OPTICS,
            listener_audio=AUDIO, storage=existing['storage'])
        plan = client.call('world.water.plan', params)
        assert client.call('world.water.cook', params | dict(expected_plan_sha256=plan['plan_sha256'])) == plan
        self.data['plan_sha256'] = pin = plan['plan_sha256']
        mask = self.sprite(client)
        base = dict(spawn_space='local', simulation_space='world', lifetime_ticks=LIFETIME,
            live_limit=64, size_m=.065, linear_rgb=[.45,.83,1], seed='0000000000000442')
        spray = base | dict(water_flow=flow('spill', QUANTUM, 24), sprite=dict(texture=mask, opacity=.76),
            acceleration_m_s2=[0,-7,0], velocity_m_s=dict(minimum=[-.18,.1,.35], maximum=[.18,.95,1.3]),
            offset_m=dict(minimum=[-.24,-.045,-.06], maximum=[.24,.035,.06]))
        for bad in (spray | dict(particles_per_second=20),
                    spray | dict(water_flow=flow('spill',0,24)),
                    spray | dict(water_flow=flow('spill',QUANTUM,65)),
                    spray | dict(water_flow=flow('spill',QUANTUM,24,'sideways'))):
            expect_error(-32602, 'world.transact', client, dict(request_id=new_id(), base_revision=revision,
                ops=[dict(op='entity.create', id=SPRAY, name='Spray'), component(SPRAY,'ParticleEmitter',bad)]))
        missing = spray | dict(water_flow=flow('not.a.port',QUANTUM,24))
        revision = self.transact(client, revision, dict(op='water.set',asset=pin),
            dict(op='entity.create',id=SPRAY,name='Spill spray'), component(SPRAY,'Transform',transform([2,.76,-4.92])),
            component(SPRAY,'ParticleEmitter',missing))
        expect_error(-32050, 'world.dependencies', client, {})
        revision = self.transact(client, revision, component(SPRAY,'ParticleEmitter',spray),
            component(uid(996),'Transform',transform([2,-.048,-5]) | dict(scale=[1.2,.1,1.2])),
            dict(op='entity.create', id=CAPPED,name='Capped flow witness'), component(CAPPED,'Transform',transform([12,0,-5])),
            component(CAPPED,'ParticleEmitter',base | dict(water_flow=flow('spill',1_000,3,'both'))),
            dict(op='entity.create',id=REVERSE,name='Reverse flow witness'), component(REVERSE,'Transform',transform([13,0,-5])),
            component(REVERSE,'ParticleEmitter',base | dict(water_flow=flow('supply',120_000_000,4,'reverse'))))
        assert client.call('world.dependencies')['needs_particle_water_flow']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_particle_water_flow']
        return revision

    def read(self, client, session, tick):
        report = client.call('runtime.particles.inspect', dict(session_id=session,tick=tick,limit=64))
        report.pop('session_id')
        assert report['next_after'] is None
        rows = {row['entity']:row for row in report['sources']}
        return report, rows

    def check_flow(self, sources, entity, tick, volume, phase, births):
        sample = sources[entity]['water_flow']
        assert (sample['tick'], sample['volume_microlitres'], sample['phase_microlitres'], sample['scheduled_births']) == (
            tick, volume, phase, births), (sample, tick, volume, phase, births)

    def capture_spray(self, client, session, tick, label, sprites):
        if self.args.gpu is None:
            return
        shot = self.capture(client, session, tick, label, camera=ABOVE, width=360, height=240, reuse=True)
        report = shot['report']['render_diagnostics']
        assert report['last_draws']['sprite_instances']==sprites, report['last_draws']
        self.data['pixel_counts'][label] = dict(image=shot['image'], sprite_instances=sprites)

    def begin_flow(self, client, session, label):
        initial, rows = self.read(client,session,0)
        for id_ in (SPRAY,CAPPED,REVERSE):
            self.check_flow(rows,id_,0,0,0,0)
        self.step(client,session,0,GATE)
        filling, rows = self.read(client,session,1)
        self.check_flow(rows,SPRAY,1,0,0,0)
        self.check_flow(rows,REVERSE,1,210_000_000,90_000_000,1)
        self.data['initial'].append([initial,filling])
        self.step(client,session,1,GATE)
        save = client.call('save.write',dict(request_id=new_id(),configuration_generation=1,expected_generation=0,
            slot='spray',session_id=session,expected_tick=2,expected_gameplay_revision=0))
        self.data['save_sha256'][label]=save['sha256']
        return self.continue_flow(client,session,label)

    def continue_flow(self, client, session, label):
        flowing, rows = self.read(client,session,2)
        dose = microlitres(expected_spill(1)[2])
        births, phase = divmod(dose,QUANTUM)
        self.check_flow(rows,SPRAY,2,dose,phase,births)
        self.check_flow(rows,CAPPED,2,dose,0,3)
        self.check_flow(rows,REVERSE,2,0,90_000_000,0)
        primary_id = rows[SPRAY]['id']
        droplets = [p for p in flowing['particles'] if p['emitter']==primary_id]
        assert len(droplets)==births and all(p['age_ticks']==0 for p in droplets), droplets
        self.capture_spray(client,session,2,label+'-spray',births)
        self.step(client,session,2,SEAL)
        sealed, rows = self.read(client,session,3)
        self.check_flow(rows,SPRAY,3,0,phase,0)
        assert rows[SPRAY]['counts']['live']==births and rows[SPRAY]['counts']['spawned']==births
        assert client.call('runtime.step',dict(session_id=session,request_id=new_id(),expected_tick=3,ticks=DRY_TICK-3))['current_tick']==DRY_TICK
        dry, rows = self.read(client,session,DRY_TICK)
        self.check_flow(rows,SPRAY,DRY_TICK,0,phase,0)
        assert dry['counts']['live']==0 and rows[SPRAY]['counts']['spawned']==births
        self.capture_spray(client,session,DRY_TICK,label+'-drained',0)
        self.step(client,session,DRY_TICK,SEAL)
        resumed, rows = self.read(client,session,DRY_TICK+1)
        next_dose = microlitres(expected_spill(2)[2])
        next_births, next_phase = divmod(phase+next_dose,QUANTUM)
        self.check_flow(rows,SPRAY,DRY_TICK+1,next_dose,next_phase,next_births)
        self.capture_spray(client,session,DRY_TICK+1,label+'-resumed',next_births)
        self.data['continuations'][label] = result = [flowing,sealed,dry,resumed]
        return result

    def restore_request(self, old, tick, slot):
        return dict(request_id=new_id(),configuration_generation=1,slot=slot,expected_generation=1,
            revision=self.revision,expected_session_id=old,expected_tick=tick,expected_gameplay_revision=0,new_session_id=new_id())

    def restore_flow(self, client, old, tick, label):
        request = self.restore_request(old,tick,'spray')
        assert client.call('save.load',request)['tick']==2
        return self.continue_flow(client,request['new_session_id'],label)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Spill Spray')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision=self.author(client)
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            session=self.mount(client)
            expected=self.begin_flow(client,session,'source')
            # The outer archive and manifest hashes remain valid in these copies.
            dose=microlitres(expected_spill(1)[2])
            for slot, field, replacement in (('wrong-volume','volume_microlitres',dose+1),('invalid-phase','phase_microlitres',QUANTUM)):
                reseal_slot(saves/'slot-spray',saves,slot,PHASE_PATH+(field,),replacement)
                expect_error(-32070,'save.load',client,self.restore_request(session,DRY_TICK+1,slot))
                observed, _ = self.read(client,session,DRY_TICK+1)
                assert observed==expected[-1]
            assert self.restore_flow(client,session,DRY_TICK+1,'warm')==expected
        if self.args.runtime_root:
            stage,bundle=(self.root/name for name in ('Built','Relocated'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),'--runtime',self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(self.root/'Hidden source')
            game=bundle/'game.json'
            info=self.cli('game','inspect',self.native(game))
            assert info['needs_particle_water_flow'] and info['needs_water_overflow']
            if self.args.windows_interop:
                exe=bundle/json.loads(game.read_bytes())['engine']['executable']
                exe.chmod(exe.stat().st_mode|0o100)
            before=tree_hashes(bundle)
            saves=self.root/'Export saves'
            saves.mkdir()
            with self.owner('relocated',game=game,saves=saves) as client:
                assert self.begin_flow(client,client.call('runtime.status')['session_id'],'relocated')==expected
            with self.owner('fresh',game=game,saves=saves) as client:
                assert self.restore_flow(client,client.call('runtime.status')['session_id'],0,'fresh')==expected
            assert tree_hashes(bundle)==before and not self.project.exists()
            self.data['immutable_files']=len(before)
        if self.args.gpu is not None:
            frames=self.data['pixel_counts']
            assert frames['source-spray']['image']!=frames['source-drained']['image']
            for phase in ('spray','drained','resumed'):
                assert all(row==frames['source-'+phase] for name,row in frames.items() if name.endswith('-'+phase))


if __name__=='__main__':
    main(FlowFixture)
