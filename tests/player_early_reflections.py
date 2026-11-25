#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored early reflections through the relocated Windows player and SDL disk output."""

from argparse import ArgumentParser
from pathlib import Path
import json
import math
import os
import struct
import subprocess
import tempfile
import wave

from retained_capture import Fixture, component, expect_error, file_hash, new_id, transform, tree_hashes, uid
from water_listener_audio_contract import pcm

SOURCE, RIGHT, LEFT = map(uid, (701, 702, 703))
TICKS = 12


def panel_position(entity, active):
    return [2 if entity == RIGHT else -4, 2.6, 1 if active else 2]


def move_panel(entity, active):
    return dict(entity=entity, position=panel_position(entity, active), rotation=[0, 0, 0, 1], duration_ticks=1)


SEQUENCE = [
    dict(ticks=2, sounds=[dict(op='play', emitter=SOURCE)]),
    dict(ticks=5, motions=[move_panel(RIGHT, True)]),
    dict(ticks=5, motions=[move_panel(RIGHT, False), move_panel(LEFT, True)]),
]


class ReflectionFixture(Fixture):
    def author(self, client):
        probe = self.project/'test tone.wav'
        amplitudes = [.16*math.sin(i*.13) + .07*math.cos(i*.042) for i in range(911)]
        with wave.open(str(probe), 'wb') as output:
            output.setparams((1, 2, 48_000, 0, 'NONE', 'not compressed'))
            output.writeframes(struct.pack('<911h', *(round(32767*a) for a in amplitudes)))
        asset = client.call('asset.audio.import', dict(source=self.native(probe)))['asset']
        probe.unlink()
        ops = [
            dict(op='entity.create', id=SOURCE, name='Repeating source'),
            component(SOURCE, 'Transform', transform([0, 2.6, 0])),
            component(SOURCE, 'AudioEmitter', dict(asset=asset, gain=.6, loop=True, enabled=True)),
        ]
        for entity, name in ((RIGHT, 'Right reflecting panel'), (LEFT, 'Left reflecting panel')):
            ops.extend([
                dict(op='entity.create', id=entity, name=name),
                component(entity, 'Transform', transform(panel_position(entity, False))),
                component(entity, 'BoxCollider', dict(half_extents=[.5, 4, .35], motion='kinematic',
                    mass=1, friction=.5, restitution=0)),
                component(entity, 'AcousticMaterial', dict(absorption=[.25, .5, .75], scattering=.1,
                    transmission=[0, 0, 0], enabled=True)),
            ])
        result = client.call('world.transact', dict(base_revision=client.call('world.inspect')['revision'],
            request_id=new_id(), ops=ops))
        assert result['committed']
        return result['revision']

    def record(self, client, reflected):
        session = self.mount(client)
        base = dict(session_id=session, expected_tick=0, camera=self.camera, controller=self.controller, mode='replay')
        rejected = expect_error(-32602, 'runtime.play', client,
            base | dict(request_id=new_id(), audio=False, early_reflections=True, sequence=SEQUENCE))
        assert 'require audio output' in rejected
        dest = self.root/('reflected.wav' if reflected else 'direct.wav')
        schedule = [segment | dict(inputs=[dict(entity=self.controller)]) for segment in SEQUENCE]
        result = client.call('runtime.audio.replay', dict(session_id=session, request_id=new_id(), expected_tick=0,
            listener=self.camera, sequence=schedule, path=self.native(dest), early_reflections=reflected))
        assert result['success'] and result['tick'] == TICKS
        assert result['capture']['sha256'] == file_hash(dest) and result['capture']['frames'] == 800*TICKS
        if reflected:
            assert result['stream']['reflection_handoffs'] == 2, result
        return result, pcm(dest)

    def launch(self, game, reflected, tag):
        bundle = game.parent
        exe = bundle/json.loads(game.read_bytes())['engine']['executable']
        exe.chmod(exe.stat().st_mode | 0o100)
        raw = self.root/(tag + '.raw')
        replay = self.root/'moving panels.json'
        if not replay.exists():
            replay.write_text(json.dumps(SEQUENCE)+'\n')
        variables = dict(SDL_AUDIO_DRIVER='disk', SDL_AUDIO_DISK_OUTPUT_FILE=self.native(raw),
            SDL_AUDIO_FREQUENCY='48000', SDL_AUDIO_CHANNELS='2', SDL_AUDIO_FORMAT='F32')
        env = os.environ | variables
        if self.args.windows_interop:
            env['WSLENV'] = ':'.join(filter(None, [env.get('WSLENV'), *variables]))
        command = [str(exe), 'game', 'run', self.native(game), '--replay', self.native(replay), '--gpu',
                   str(self.args.gpu), '--width', '320', '--height', '180']
        before = tree_hashes(bundle)
        launched = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
        self.data['launches'][tag] = dict(exit=launched.returncode, stdout=launched.stdout, stderr=launched.stderr)
        assert launched.returncode == 0, launched
        row = json.loads(launched.stdout)['result']['play']
        assert row['success'] and row['tick'] == TICKS and row['frames_presented'] > 0
        assert row['hardware'] and row['nvrhi_errors'] == 0
        audio = row['audio']
        assert audio['enabled'] and audio['driver'] == 'disk' and audio['stream_drained'], row
        assert audio['submitted_frames'] == TICKS*800 and audio['voices_started'] == 1
        assert audio.get('early_reflections', False) is reflected
        assert audio.get('reflection_handoffs', 0) == (2 if reflected else 0)
        assert tree_hashes(bundle) == before
        return row, raw

    def reject_old_runtime(self):
        unsupported = self.root/'Runtime without reflections'
        unsupported.mkdir()
        descriptor = json.loads((self.args.runtime/'runtime.json').read_bytes())
        feature = descriptor['features'].pop('player_early_reflections')
        assert feature is True
        (unsupported/'runtime.json').write_text(json.dumps(descriptor)+'\n')
        blocked = self.root/'Refused export'
        proc = subprocess.run([str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
            '--output', self.native(blocked), '--runtime', self.native(unsupported)], capture_output=True,
            text=True, timeout=90)
        self.data['old_runtime_refusal'] = dict(exit=proc.returncode, reply=json.loads(proc.stdout))
        assert proc.returncode != 0 and 'Early reflections require a compatible player runtime.' in proc.stdout
        assert not blocked.exists()

    def run(self):
        self.cli('project', 'create', self.native(self.project), '--name', 'Moving Sound Panels')
        project = json.loads(self.manifest.read_bytes())
        self.camera, self.controller = (project['entry'][k] for k in ('camera', 'controller'))
        with self.owner('author') as client:
            self.revision = self.author(client)
        references = {}
        for reflected in (False, True):
            with self.owner('record-reflected' if reflected else 'record-dry') as client:
                references[reflected] = self.record(client, reflected)
        assert references[False][1] != references[True][1]
        outputs = {}
        for reflected in (False, True):
            current = self.cli('project', 'inspect', self.native(self.manifest))
            flags = dict(audio=True, early_reflections=reflected)
            self.cli('project', 'configure', self.native(self.manifest), '--patch', json.dumps(flags),
                '--expected-manifest-sha256', current['manifest_sha256'])
            if reflected:
                self.reject_old_runtime()
            tag = 'reflected' if reflected else 'direct'
            built, relocated = (self.root/(tag+s) for s in (' built', ' relocated'))
            self.cli('project', 'build', self.native(self.manifest), '--output', self.native(built),
                '--runtime', self.native(self.args.runtime))
            built.rename(relocated)
            game = relocated/'game.json'
            info = self.cli('game', 'inspect', self.native(game))
            assert info['audio'] and info.get('early_reflections', False) is reflected
            outputs[reflected] = tag, game
        self.project.rename(self.root/'Hidden original')
        for reflected, (tag, game) in outputs.items():
            row, raw = self.launch(game, reflected, tag)
            reference, expected = references[reflected]
            assert reference['stream']['peak'] == row['audio']['peak']
            # SDL adds silent periods around the stream. Align its disk
            # capture at onset and compare the entire submitted sequence.
            received = tuple(x[0] for x in struct.iter_unpack('<f', raw.read_bytes()))
            first = next(i for i, value in enumerate(received) if value != 0)
            onset = next(i for i, value in enumerate(expected) if value != 0)
            offset = first-onset
            assert offset >= 0 and offset % 2 == 0
            assert received[offset:offset+len(expected)] == expected
            self.data['signals'][tag] = dict(report=row['audio'], frames_checked=len(expected)//2,
                raw_sha256=file_hash(raw), reference_sha256=reference['capture']['sha256'],
                submitted_signal_exact=True, empty_prefix_frames=offset//2)


if __name__ == '__main__':
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix='reflections-', dir=args.output))
    data = dict(binary_sha256=file_hash(args.binary), owners=[], cli=[], launches={}, signals={}, passed=False)
    try:
        ReflectionFixture(args, root, data).run()
        data['passed'] = True
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(data, indent=2)+'\n')
        print(path, flush=True)
