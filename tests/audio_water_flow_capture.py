#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a native spillway loop against recorded unbound PCM and saved discharge."""

import json
import math
import struct
import wave

from particle_collision_capture import main
from retained_capture import component, expect_error, new_id, transform, tree_hashes, uid
from water_flow_particles_capture import QUANTUM, SEAL, SPRAY, microlitres
from water_listener_audio_contract import FRAMES_PER_TICK, RATE, pcm
from water_overflow_capture import expected_spill
from water_reservoir_capture import ABOVE, ABOVE_EYE, GATE
from water_sheet_capture import SheetFixture

EMITTER, REVERSE, CAPPED = map(uid, (1201, 1202, 1203))
FULL_DISCHARGE = .3
EVENT_GAIN = .5
GAIN = .625
BUS_GAIN = .75
ACTIONS = (GATE, GATE, SEAL, None, SEAL, None, SEAL, None)
STEPS = (0, 1, 0, 0, 2, 3, 0, 0)


def profile(port='spill', full=FULL_DISCHARGE, **options):
    return dict(port=port, full_discharge_m3_s=full) | options


def write_probe(path):
    # An intentionally non-tick-aligned clip makes restart/phase errors visible.
    frames = 3163
    samples = [round(1600 * (math.sin(math.tau * 19 * i / frames) +
                             .4 * math.sin(math.tau * 83 * i / frames))) for i in range(frames)]
    with wave.open(str(path), 'wb') as output:
        output.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        output.writeframes(struct.pack(f'<{frames}h', *samples))


class WaterAudioFixture(SheetFixture):
    def install_sources(self, client, revision):
        path = self.root/'acoustic probe.wav'
        write_probe(path)
        asset = client.call('asset.audio.import', dict(source=self.native(path)))['asset']
        self.control = dict(asset=asset, gain=GAIN, loop=True, enabled=True, bus='flow')
        self.bound = self.control | dict(water_flow=profile())
        reverse = self.control | dict(water_flow=profile('supply', 5, direction='reverse'), bus='silent')
        capped = self.control | dict(water_flow=profile(full=.02, direction='both'), bus='silent')
        revision = self.transact(client, revision, dict(op='audio.buses.set', buses=dict(flow=BUS_GAIN,silent=0)))
        for entity, name, emitter in ((EMITTER,'Spill bed',self.control),(REVERSE,'Reverse inlet',reverse),
                                      (CAPPED,'Capped flow',capped)):
            x,y,z=ABOVE_EYE
            revision = self.transact(client,revision,dict(op='entity.create',id=entity,name=name),
                component(entity,'Transform',transform([x+.8,y,z-1.0])),component(entity,'AudioEmitter',emitter))
        return revision

    def observations(self, client, session, tick, step):
        state = client.call('runtime.water.inspect', dict(session_id=session,tick=tick,cells=[[0,0]]))
        report = client.call('runtime.audio.inspect', dict(session_id=session,tick=tick,listener=ABOVE))
        sources = {source['entity']:source for source in report['sources']}
        spill, supply = state['storage']['ports']
        ports = {port['id']:port['discharge_m3_s'] for port in (spill,supply)}
        assert (spill['active_sheet'] is not None)==bool(step)
        _, particles = self.read(client,session,tick)
        discharge = microlitres(ports['spill'])
        spray = particles[SPRAY]['water_flow']
        assert spray['volume_microlitres']==discharge
        assert spray['scheduled_births']==discharge//QUANTUM
        for source, port, direction, expected in (
            (EMITTER,'spill','forward',expected_spill(step)[2]/FULL_DISCHARGE),
            (REVERSE,'supply','reverse',float(tick==1)),
            (CAPPED,'spill','both',float(step!=0))):
            sample = sources[source]['water_flow']
            assert (sample['port'],sample['direction'])==(port,direction)
            assert sample['discharge_m3_s']==ports[port]
            assert abs(sample['gain']-expected)<1e-7
            effective = sources[source]['effective_gain']
            assert abs(effective-GAIN*(BUS_GAIN if source==EMITTER else 0)*expected)<1e-7
        if tick==1:
            assert ports['supply']<0
        return dict(tick=tick,discharge=ports,sheet=spill['active_sheet'],spray=spray,
            selected={source:sources[source]['water_flow']
            for source in (EMITTER,REVERSE,CAPPED)})

    def play_audio(self,client,session,tag,first,last):
        sequence = []
        for tick in range(first,last):
            segment = dict(ticks=1)
            if action := ACTIONS[tick]:
                segment['inputs'] = [dict(entity=uid(2),actions=[dict(name=action,held=True,pressed=True,released=False)])]
            if tick == 0:
                segment['sounds'] = [dict(op='play',emitter=EMITTER,gain=EVENT_GAIN)]
            sequence.append(segment)
        path = self.root/(tag+'.wav')
        report = client.call('runtime.audio.replay',dict(session_id=session, request_id=new_id(),
            expected_tick=first,listener=ABOVE,path=self.native(path),sequence=sequence))
        assert report['success'] and report['tick']==last, report
        assert report['capture']['frames']==(last-first)*FRAMES_PER_TICK
        assert report['stream']['voices_started']==1 and report['stream']['over_range_samples']==0, report
        self.data.setdefault('recordings',{})[tag]=report
        return path

    def check_envelope(self, control, driven):
        original, gained = pcm(control), pcm(driven)
        expected = [0.] + [expected_spill(step)[2]/FULL_DISCHARGE for step in STEPS]
        assert len(original)==len(gained)==len(STEPS)*2*FRAMES_PER_TICK
        assert max(abs(sample) for sample in original)>.002
        error = 0.
        for index, (dry,wet) in enumerate(zip(original,gained,strict=True)):
            tick,phase=divmod(index//2,FRAMES_PER_TICK)
            progress=(phase+1)/FRAMES_PER_TICK
            multiplier=expected[tick]*(1-progress)+expected[tick+1]*progress
            error=max(error,abs(dry*multiplier-wet))
        assert error<1e-7,error
        for tick in (0,3,7):
            start=tick*2*FRAMES_PER_TICK
            assert not any(gained[start:start+2*FRAMES_PER_TICK])
        self.data['envelope_test']=dict(max_sample_error=error,frames=len(gained)//2,
            full_silence_ticks=[1,4,8],unbound_peak=max(abs(sample) for sample in original))

    def saved_mix(self, client, session, tag):
        for tick in range(2):
            self.play_audio(client,session,f'{tag}-tick-{tick}',tick,tick+1)
            self.observations(client,session,tick+1,STEPS[tick])
        saved=client.call('save.write',dict(request_id=new_id(),configuration_generation=1,expected_generation=0,
            slot='spill',session_id=session,expected_tick=2,expected_gameplay_revision=0))
        self.data['save_sha256'][tag]=saved['sha256']
        return self.continue_from(client,session,tag)

    def continue_from(self,client,session,tag):
        state=self.observations(client,session,2,1)
        voice,=client.call('runtime.audio.voices',dict(session_id=session,tick=2))['voices']
        assert voice['voice']==1 and voice['emitter']==EMITTER and voice['emitting']
        assert voice['clip_frame']==(2-voice['start_tick'])*FRAMES_PER_TICK
        path=self.play_audio(client,session,tag+'-continuation',2,len(ACTIONS))
        return state,voice,path.read_bytes()

    def resume(self,client,old,tick,tag):
        request=self.restore_request(old,tick,'spill')
        assert client.call('save.load',request)['tick']==2
        return self.continue_from(client,request['new_session_id'],tag)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Spillway acoustics')
        saves=self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.install_sources(client,super().author(client))
            assert client.call('world.describe')['audio_water_flow']['available']
            session = self.mount(client)
            control=self.play_audio(client,session,'control',0,len(ACTIONS))
            client.call('runtime.stop',dict(session_id=session))
            for invalid in (profile(full=0),profile(full=1e7),profile(direction='sideways'),profile(port='NOT VALID')):
                expect_error(-32602,'world.transact',client,dict(request_id=new_id(),base_revision=self.revision,
                    ops=[component(EMITTER,'AudioEmitter',self.control | dict(water_flow=invalid))]))
            revision=self.transact(client,self.revision,
                component(EMITTER,'AudioEmitter',self.control | dict(water_flow=profile('missing'))))
            expect_error(-32050,'world.dependencies',client,{})
            self.revision=self.transact(client,revision,component(EMITTER,'AudioEmitter',self.bound))
            assert client.call('world.dependencies')['needs_audio_water_flow']
            before=client.call('world.audio.inspect',dict(revision=self.revision,listener=ABOVE))
            assert all(row['water_flow']['gain']==0 for row in before['sources'])
            session=self.mount(client)
            driven=self.play_audio(client,session,'driven',0,len(ACTIONS))
            self.check_envelope(control,driven)
            client.call('runtime.stop',dict(session_id=session))
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            session=self.mount(client)
            expected = self.saved_mix(client,session,'source')
            assert self.resume(client,session,len(ACTIONS),'warm')==expected
            self.data['saved_observation'], self.data['saved_voice'] = expected[:2]
            self.data['warm_exact'] = True
        if self.args.runtime_root:
            stage,bundle=(self.root/name for name in ('Built','Relocated'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),
                '--runtime',self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(self.root/'Hidden source')
            game=bundle/'game.json'
            assert self.cli('game','inspect',self.native(game))['needs_audio_water_flow']
            if self.args.windows_interop:
                exe=bundle/json.loads(game.read_bytes())['engine']['executable']
                exe.chmod(exe.stat().st_mode|0o100)
            before=tree_hashes(bundle)
            exported=self.root/'Export saves'
            exported.mkdir()
            with self.owner('relocated',game=game,saves=exported) as client:
                assert self.saved_mix(client,client.call('runtime.status')['session_id'],'relocated')==expected
            with self.owner('fresh',game=game,saves=exported) as client:
                assert self.resume(client,client.call('runtime.status')['session_id'],0,'fresh')==expected
            assert tree_hashes(bundle)==before and not self.project.exists()
            self.data['immutable_files']=len(before)
            self.data['fresh_exact']=True


if __name__=='__main__':
    main(WaterAudioFixture)
