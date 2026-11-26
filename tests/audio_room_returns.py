#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a source room's late tail can emerge after its dry impulse ends."""

import json
import math
import struct
import wave

from audio_portal import CLOSED, GATE, OPEN, PORTAL, SOURCE, PortalFixture, energy, main, motion
from retained_capture import component, expect_error, file_hash, new_id, transform, uid
from water_listener_audio_contract import RATE, pcm

ROOM = uid(920)
PROFILE = dict(half_extents=[3,2,1.5],decay_seconds=2.6,high_frequency_decay_seconds=.8,
               wet_gain=.8,portal_send=.9)
SEQUENCE = [dict(ticks=6,sounds=[dict(op='play',emitter=SOURCE)]),
            motion(OPEN),dict(ticks=24),motion(CLOSED),dict(ticks=10),
            motion(OPEN),dict(ticks=24),motion(CLOSED),dict(ticks=10)]


class RoomReturnFixture(PortalFixture):
    project_name = 'Late return behind a door'
    sequence = SEQUENCE
    total_ticks = sum(part['ticks'] for part in SEQUENCE)

    def author_source(self, client, position):
        wav = self.project/'Door tail impulse.wav'
        with wave.open(str(wav),'wb') as output:
            output.setparams((1,2,RATE,0,'NONE','not compressed'))
            output.writeframes(struct.pack('<h',24576))
        asset = client.call('asset.audio.import',dict(source=self.native(wav)))['asset']
        wav.unlink()
        self.edit(client,dict(op='entity.create',id=SOURCE,name='Short local impulse'),
            component(SOURCE,'Transform',transform(position)),
            component(SOURCE,'AudioEmitter',dict(asset=asset,gain=.8,loop=False,enabled=True)),
            dict(op='entity.create',id=ROOM,name='Reverberant room'),
            component(ROOM,'Transform',transform(position)),component(ROOM,'AudioRoom',PROFILE))
        for invalid in (-1,1.1,True):
            expect_error(-32602,'world.transact',client,dict(base_revision=self.revision,
                request_id=new_id(),ops=[component(ROOM,'AudioRoom',PROFILE | dict(portal_send=invalid))]))

    def inspect_return(self, client, live=None, listener=None):
        listener = listener or self.camera
        if live:
            session,tick = live
            return client.call('runtime.audio.inspect',dict(session_id=session,tick=tick,listener=listener))
        return client.call('world.audio.inspect',dict(revision=self.revision,listener=listener))

    def observe(self, client, live=None):
        report = self.inspect_return(client,live)
        voice, = report['sources']
        returned, = report['room_returns']
        assert voice['entity']==SOURCE and voice['sending_room']==ROOM and voice['direct_visibility']==0
        assert returned['entity']==ROOM and returned['portal_send']==PROFILE['portal_send']
        assert 'listener_room' not in report and not report['audio_portals']['budget_exceeded']
        return voice | dict(room_return=returned['portal_path'])

    def aperture(self, source, visible):
        dry = super().aperture(source,visible)
        returned = source['room_return']
        assert returned==dry
        return returned

    def frozen_capture(self, client, name):
        file = self.root/(name+'.wav')
        report = client.call('world.audio.capture',dict(revision=self.revision,listener=self.camera,
                             path=self.native(file),frames=RATE))
        assert report['capture']['sha256']==file_hash(file)
        return pcm(file)

    def record(self, client, session, label):
        file = self.root/(label+'.wav')
        sequence = [command | dict(inputs=[dict(entity=self.controller)]) for command in self.sequence]
        params = dict(session_id=session,expected_tick=0,request_id=new_id(),listener=self.camera,
                      path=self.native(file),sequence=sequence)
        report = client.call('runtime.audio.replay',params)
        assert report['success'] and report['tick']==self.total_ticks
        assert report['capture']['sha256']==file_hash(file) and report['stream']['voices_started']==1
        assert report['stream']['portal_handoffs']==0 and not report['stream']['portal_budget_exceeded']
        assert client.call('runtime.audio.replay',params) == report | dict(replayed=True)
        self.data['replays'][label] = report
        return pcm(file)

    def replay(self, client, session, label):
        samples = self.record(client,session,label)
        assert self.data['replays'][label]['stream']['room_return_route_changes']==4
        self.aperture(self.observe(client,(session,self.total_ticks)),0)
        return samples

    def static_controls(self, client):
        self.aperture(self.observe(client),0)
        self.edit(client,component(GATE,'Transform',transform(OPEN)))
        open_route = self.aperture(self.observe(client),9)
        self.data['paths']['open'] = open_route
        local = self.inspect_return(client,listener=SOURCE)
        assert local['listener_room']['entity']==ROOM
        assert local['room_returns'][0]['portal_path'] is None
        wet = self.frozen_capture(client,'Frozen room return')
        self.edit(client,component(ROOM,'AudioRoom',PROFILE | dict(portal_send=0)))
        dry = self.frozen_capture(client,'Frozen dry control')
        first = open_route['propagation_delay_samples']+743
        assert wet[:2*first]==dry[:2*first]
        late = energy(wet,12,30)
        dry_late = energy(dry,12,30)
        # Frozen HRTF convolution leaves a numerical floor after the
        # direct impulse. Isolate the room return above that control.
        assert late>1e-7 and dry_late<1e-13 and late>1e8*dry_late
        self.data['frozen'] = dict(identical_frames_before_room_arrival=first,late_energy=late,
                                   dry_late_energy=dry_late)
        self.edit(client,component(GATE,'Transform',transform(CLOSED)))
        session = self.mount(client)
        assert not any(self.record(client,session,'control without return'))
        client.call('runtime.stop',dict(session_id=session))
        self.edit(client,component(ROOM,'AudioRoom',PROFILE))

    def review_capture(self, samples):
        ranges = dict(before_open=(0,6),after_open=(21,39),after_seal=(57,63),
                      after_reopening=(79,96),after_final_seal=(115,122))
        measured = {label:energy(samples,*interval) for label,interval in ranges.items()}
        assert measured['before_open']==measured['after_seal']==measured['after_final_seal']==0
        assert measured['after_open']>1e-7 and 0<measured['after_reopening']<measured['after_open']
        left,right = (energy(samples,*ranges['after_open'],ear) for ear in (0,1))
        assert math.isfinite(left+right) and right>left
        self.data['energy'] = measured | dict(left=left,right=right)
        self.data['energy_windows'] = ranges

    def player(self, game, expected):
        super().player(game,expected)
        audio = json.loads(self.data['player']['stdout'])['result']['play']['audio']
        assert audio['room_return_route_changes']==4


if __name__=='__main__':
    main(RoomReturnFixture,__doc__)
