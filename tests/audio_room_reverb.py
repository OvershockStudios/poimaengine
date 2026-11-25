#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check room selection, impulse decay and continuing tails across a room exit."""

from argparse import ArgumentParser
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

from retained_capture import Fixture, component, expect_error, file_hash, new_id, transform, tree_hashes, uid
from water_listener_audio_contract import FRAMES_PER_TICK, RATE, pcm

ROOM, OVERLAP, SOURCE = map(uid, (801, 802, 803))
PROFILE = dict(half_extents=[1.2, 8, 4], decay_seconds=1.4,
               high_frequency_decay_seconds=.65, wet_gain=.6)
POSITION = [3, 6, 0]
INSIDE = [0, 6, 0]
TOTAL_TICKS = 84
SEQUENCE = [
    dict(ticks=8, motions=[dict(entity=ROOM, position=INSIDE, rotation=[0,0,0,1], duration_ticks=8)]),
    dict(ticks=3, sounds=[dict(op='play', emitter=SOURCE)]),
    dict(ticks=8, motions=[dict(entity=ROOM, position=POSITION, rotation=[0,0,0,1], duration_ticks=8)]),
    dict(ticks=TOTAL_TICKS-19),
]


def energy(samples, first_seconds, last_seconds):
    first = 2*round(first_seconds*RATE)
    last = 2*round(last_seconds*RATE)
    return sum(sample**2 for sample in samples[first:last])


def decay_time(samples):
    # Reverse-integrated impulse energy with a -5 to -35 dB fit. Samples
    # are stereo interleaved; using sample pairs keeps the time axis in frames.
    reverse_energy = []
    accumulated = 0.
    for index in range(len(samples)-2, -1, -2):
        accumulated += samples[index]**2 + samples[index+1]**2
        reverse_energy.append(accumulated)
    first, last = (next(index for index, value in enumerate(reversed(reverse_energy))
                        if value <= accumulated * 10**(db/10)) for db in (-5,-35))
    return 2*(last-first)/RATE


class RoomFixture(Fixture):
    def transact(self, client, *ops):
        result = client.call('world.transact', dict(base_revision=self.revision, request_id=new_id(), ops=ops))
        assert result['committed']
        self.revision = result['revision']

    def author(self, client):
        self.revision = client.call('world.inspect')['revision']
        probe = self.root/'Unit impulse.wav'
        with wave.open(str(probe), 'wb') as output:
            output.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
            output.writeframes(struct.pack('<h', 16384))
        asset = client.call('asset.audio.import', dict(source=self.native(probe)))['asset']
        self.transact(client,
            dict(op='entity.create', id=ROOM, name='Room volume'),
            component(ROOM, 'Transform', transform(POSITION)),
            component(ROOM, 'BoxCollider', dict(half_extents=[.05,.05,.05], motion='kinematic',
                                                mass=1, friction=.5, restitution=0)),
            component(ROOM, 'AudioRoom', PROFILE),
            dict(op='entity.create', id=SOURCE, name='Impulse probe'),
            component(SOURCE, 'Transform', transform([0,1.7,-1])),
            component(SOURCE, 'AudioEmitter', dict(asset=asset, gain=.8, loop=False, enabled=True)))
        assert client.call('world.dependencies')['needs_audio_room_reverb']
        assert client.call('entity.get', dict(id=ROOM, component='AudioRoom'))['value'] == PROFILE
        for update in (dict(decay_seconds=.01), dict(high_frequency_decay_seconds=2),
                       dict(wet_gain=True), dict(half_extents=[2,3]), dict(priority=256)):
            expect_error(-32602, 'world.transact', client, dict(base_revision=self.revision, request_id=new_id(),
                ops=[component(ROOM, 'AudioRoom', PROFILE | update)]))
        assert client.call('world.inspect')['revision'] == self.revision

    def inspect(self, client):
        return client.call('world.audio.inspect', dict(revision=self.revision, listener=self.camera))

    def capture(self, client, name):
        path = self.root/(name+'.wav')
        result = client.call('world.audio.capture', dict(revision=self.revision, listener=self.camera,
                             path=self.native(path), frames=2*RATE))
        assert result['capture']['sha256'] == file_hash(path)
        self.data['frozen'][name] = result
        return pcm(path)

    def frozen(self, client):
        assert 'listener_room' not in self.inspect(client)
        self.transact(client, component(ROOM, 'Transform', transform(INSIDE)))
        assert self.inspect(client)['listener_room']['entity'] == ROOM
        self.transact(client, dict(op='entity.create', id=OVERLAP, name='Priority volume'),
            component(OVERLAP, 'Transform', transform(INSIDE)),
            component(OVERLAP, 'AudioRoom', PROFILE))
        assert self.inspect(client)['listener_room']['entity'] == ROOM
        self.transact(client, component(OVERLAP, 'AudioRoom', PROFILE | dict(priority=1)))
        assert self.inspect(client)['listener_room']['entity'] == OVERLAP
        assert len(client.call('entity.query', dict(component='AudioRoom'))['entities']) == 2
        self.transact(client, dict(op='component.remove', id=OVERLAP, type='AudioRoom'))
        self.transact(client, component(ROOM, 'AudioRoom', PROFILE | dict(wet_gain=0)))
        dry = self.capture(client, 'frozen dry')
        self.transact(client, component(ROOM, 'AudioRoom', PROFILE | dict(decay_seconds=.35,
                                                                      high_frequency_decay_seconds=.35)))
        short = self.capture(client, 'frozen short')
        self.transact(client, component(ROOM, 'AudioRoom', PROFILE))
        long = self.capture(client, 'frozen long')
        first = next(i for i, sample in enumerate(dry) if sample)
        assert short[:first+2*743] == dry[:first+2*743] == long[:first+2*743]
        short_tail = tuple(wet-d for wet,d in zip(short,dry,strict=True))
        long_tail = tuple(wet-d for wet,d in zip(long,dry,strict=True))
        short_decay, long_decay = map(decay_time, (short_tail, long_tail))
        late_energy_ratio = energy(long_tail,.65,.85)/energy(short_tail,.65,.85)
        assert .20 < short_decay < .55 and .70 < long_decay < 1.6, (short_decay,long_decay)
        assert late_energy_ratio > 100, late_energy_ratio
        self.data['decay'] = dict(short_t30_seconds=short_decay, long_t30_seconds=long_decay,
                                  late_energy_ratio=late_energy_ratio, identical_pre_arrival_frames=first//2+743)
        self.transact(client, component(ROOM, 'Transform', transform(POSITION)))

    def replay(self, client, session, name):
        path = self.root/(name+'.wav')
        sequence = [item | dict(inputs=[dict(entity=self.controller)]) for item in SEQUENCE]
        params = dict(session_id=session, request_id=new_id(), expected_tick=0, listener=self.camera,
                      sequence=sequence, path=self.native(path))
        result = client.call('runtime.audio.replay', params)
        assert result['success'] and result['tick'] == TOTAL_TICKS
        assert result['capture']['sha256'] == file_hash(path)
        assert result['stream']['voices_started'] == 1 and result['stream']['room_transitions'] == 2
        assert result['listener_room']['initial'] is None and result['listener_room']['final'] is None
        retry = client.call('runtime.audio.replay', params)
        assert retry == result | dict(replayed=True)
        self.data['replays'][name] = result
        return pcm(path)

    def move(self, client, session, wet):
        target = self.replay(client, session, 'moving wet' if wet else 'moving dry')
        client.call('runtime.stop', dict(session_id=session))
        return target

    def saved_room_capture(self, client, session, name):
        path = self.root/(name+'.wav')
        result = client.call('runtime.audio.capture', dict(session_id=session, tick=8,
            listener=self.camera, path=self.native(path), frames=RATE))
        assert result['listener_room']['entity'] == ROOM
        assert result['capture']['sha256'] == file_hash(path)
        self.data['save_captures'][name] = result['capture']['sha256']
        return result['capture']['sha256']

    def restore_room(self, client, session, tick):
        replacement = new_id()
        result = client.call('save.load', dict(request_id=new_id(), configuration_generation=1,
            slot='inside', expected_generation=1, revision=self.revision, expected_session_id=session,
            expected_tick=tick, expected_gameplay_revision=0, expected_structure_revision=0,
            new_session_id=replacement))
        assert result['session_id'] == replacement and result['tick'] == 8
        return replacement

    def save_and_restore(self, client, saves):
        client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=self.native(saves)))
        active = self.mount(client)
        result = client.call('runtime.step', dict(session_id=active, request_id=new_id(), expected_tick=0,
                            **SEQUENCE[0]))
        assert result['current_tick'] == 8
        self.save(client, active, 8, 'inside')
        captured = self.saved_room_capture(client, active, 'Saved original room')
        result = client.call('runtime.step', dict(session_id=active, request_id=new_id(), expected_tick=8,
                            **SEQUENCE[2]))
        assert result['current_tick'] == 16
        dry = client.call('runtime.audio.inspect', dict(session_id=active, tick=16, listener=self.camera))
        assert 'listener_room' not in dry
        restored = self.restore_room(client, active, 16)
        assert self.saved_room_capture(client, restored, 'Warm restored room') == captured
        return captured

    def launch_player(self, game, expected):
        binary = game.parent/json.loads(game.read_bytes())['engine']['executable']
        raw, replay = (self.root/name for name in ('Player disk output.raw', 'Room replay.json'))
        replay.write_text(json.dumps(SEQUENCE)+'\n')
        variables = dict(SDL_AUDIO_DRIVER='disk', SDL_AUDIO_DISK_OUTPUT_FILE=self.native(raw),
                         SDL_AUDIO_FORMAT='F32', SDL_AUDIO_CHANNELS='2', SDL_AUDIO_FREQUENCY='48000')
        env = os.environ | variables
        if self.args.windows_interop:
            env['WSLENV'] = ':'.join(filter(None, [env.get('WSLENV'), *variables]))
        argv = [str(binary),'game','run',self.native(game),'--replay',self.native(replay),
                '--gpu',str(self.args.gpu),'--width','320','--height','180']
        launched = subprocess.run(argv, env=env, text=True, capture_output=True, timeout=90)
        self.data['player'] = dict(exit=launched.returncode, stdout=launched.stdout, stderr=launched.stderr)
        assert launched.returncode == 0, launched
        result = json.loads(launched.stdout)['result']['play']
        assert result['tick'] == TOTAL_TICKS and result['hardware'] and result['nvrhi_errors'] == 0
        output = result['audio']
        assert output['driver'] == 'disk' and output['stream_drained'] and output['room_transitions'] == 2
        assert output['voices_started'] == 1 and output['submitted_frames'] == TOTAL_TICKS*FRAMES_PER_TICK
        received = tuple(item[0] for item in struct.iter_unpack('<f', raw.read_bytes()))
        first = next(i for i, sample in enumerate(expected) if sample)
        offset = next(i for i, sample in enumerate(received) if sample)-first
        assert offset >= 0 and offset%2 == 0
        assert received[offset:offset+len(expected)] == expected
        self.data['player_pcm'] = dict(frames=len(expected)//2, sha256=file_hash(raw), leading_silence=offset//2)

    def standalone(self, expected, saves, saved):
        info = self.cli('project','inspect',self.native(self.manifest))
        assert info['needs_audio_room_reverb']
        self.cli('project','configure',self.native(self.manifest),'--patch','{"audio":true}',
                 '--expected-manifest-sha256',info['manifest_sha256'])
        descriptor = json.loads((self.args.runtime/'runtime.json').read_bytes())
        assert descriptor['features'].pop('audio_room_reverb') is True
        legacy, refused = (self.root/name for name in ('Legacy runtime', 'Refused export'))
        legacy.mkdir()
        (legacy/'runtime.json').write_text(json.dumps(descriptor)+'\n')
        args = [str(self.args.binary), 'project','build',self.native(self.manifest),'--output',self.native(refused),
                '--runtime',self.native(legacy)]
        proc = subprocess.run(args, text=True, capture_output=True, timeout=90)
        self.data['refused_export'] = dict(exit=proc.returncode, output=json.loads(proc.stdout))
        assert proc.returncode != 0 and not refused.exists() and 'audio rooms require' in proc.stdout
        stage, bundle = (self.root/name for name in ('Staged game','Relocated game'))
        self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),'--runtime',self.native(self.args.runtime))
        stage.rename(bundle)
        self.project.rename(self.root/'Hidden original')
        manifest = bundle/'game.json'
        assert self.cli('game','inspect',self.native(manifest))['needs_audio_room_reverb']
        before = tree_hashes(bundle)
        with self.owner('relocated',game=manifest,saves=saves) as client:
            active = client.call('runtime.status')
            assert active['active'] and active['tick'] == 0
            assert self.replay(client, active['session_id'], 'relocated wet') == expected
            restored = self.restore_room(client, active['session_id'], TOTAL_TICKS)
            assert self.saved_room_capture(client, restored, 'Relocated restored room') == saved
        if self.args.gpu is not None:
            self.launch_player(manifest, expected)
        assert tree_hashes(bundle) == before and not self.project.exists()
        self.data['relocated_files'] = len(before)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Room transition probe')
        entry = json.loads(self.manifest.read_bytes())['entry']
        self.camera, self.controller = entry['camera'], entry['controller']
        saves = self.root/'Saves'
        saves.mkdir()
        with self.owner('author-and-replay') as client:
            self.author(client)
            self.frozen(client)
            self.transact(client, component(ROOM, 'AudioRoom', PROFILE | dict(wet_gain=0)))
            dry = self.move(client, self.mount(client), False)
            self.transact(client, component(ROOM, 'AudioRoom', PROFILE))
            wet = self.move(client, self.mount(client), True)
            first = next(i for i, sample in enumerate(dry) if sample)
            assert wet[:first+2*743] == dry[:first+2*743]
            tail = tuple(w-d for w,d in zip(wet,dry,strict=True))
            post_exit_energy = energy(tail, .4, .65)
            assert post_exit_energy > 1e-6 and energy(dry,.4,.65) == 0
            self.data['exit_tail'] = dict(energy_at_400_to_650_ms=post_exit_energy, frames=len(wet)//2)
            saved = self.save_and_restore(client, saves)
        if self.args.runtime:
            self.standalone(wet, saves, saved)


if __name__ == '__main__':
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    args = parser.parse_args()
    if not __debug__ or (args.gpu is not None and not args.runtime):
        parser.error('Assertions must be enabled and --gpu requires an installed runtime.')
    args.binary = args.binary.resolve()
    root = Path(tempfile.mkdtemp(prefix='room-reverb-', dir=args.output.resolve()))
    data = dict(binary_sha256=file_hash(args.binary), owners=[], cli=[], frozen={}, replays={},
                save_captures={}, passed=False)
    try:
        RoomFixture(args, root, data).run()
        data['passed'] = True
    finally:
        evidence = root/'evidence.json'
        evidence.write_text(json.dumps(data, indent=2, allow_nan=False)+'\n')
        print(evidence, flush=True)
