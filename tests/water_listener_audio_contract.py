#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare stock waterline audio against a dry native mix and an independent filter oracle."""

import argparse
import json
import math
from pathlib import Path
import struct
import tempfile
import traceback
import wave

from world_particles_contract import ParticleContract
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, near, rpc, success, uid


LISTENER, EMITTER = uid(6001), uid(6002)
EYE = [10.5, 6, -3.5]
FREQUENCIES = (200, 6000)
PROFILE = dict(cutoff_hz=900, gain=.6, full_depth_m=.5)
RATE, FRAMES_PER_TICK, TRANSITION_FRAMES = 48000, 800, 2400


def write_tones(path):
    samples = [round(3000 * sum(math.sin(2 * math.pi * f * i / RATE) for f in FREQUENCIES))
               for i in range(4800)]
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        stream.writeframes(struct.pack('<4800h', *samples))


def pcm(path):
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE', path
    chunks = {}
    offset = 12
    while offset < len(data):
        size, = struct.unpack_from('<I', data, offset + 4)
        chunks[data[offset:offset + 4]] = data[offset + 8:offset + 8 + size]
        offset += 8 + size + size % 2
    assert struct.unpack_from('<HHI', chunks[b'fmt ']) == (3, 2, RATE)
    return tuple(value[0] for value in struct.iter_unpack('<f', chunks[b'data']))


def filter_error(dry, wet, targets, initial):
    assert len(dry) == len(wet) and len(wet) % 2 == 0
    # Solve the digital pole from the desired magnitude at the corner, then
    # evaluate the recurrence against the recorded unprocessed signal.
    angle = 2 * math.pi * PROFILE['cutoff_hz'] / RATE
    term = 2 - math.cos(angle)
    pole = 1 / (term + math.sqrt(term * term - 1))
    filtered = [0, 0]
    mix = initial
    error = 0
    for frame, index in enumerate(range(0, len(wet), 2)):
        target = targets(frame)
        mix = min(target, mix + 1 / TRANSITION_FRAMES) if target >= mix else max(target, mix - 1 / TRANSITION_FRAMES)
        for channel in range(2):
            input_value = dry[index + channel]
            filtered[channel] = pole * filtered[channel] + (1 - pole) * input_value
            expect = input_value * (1 - mix) + filtered[channel] * PROFILE['gain'] * mix
            error = max(error, abs(expect - wet[index + channel]))
    assert error < 2e-7, error
    return error


def amplitude(samples, frequency, first, end):
    accumulators = [0, 0]
    for frame in range(first, end):
        sample = samples[frame * 2]
        angle = 2 * math.pi * frequency * frame / RATE
        accumulators[0] += sample * math.sin(angle)
        accumulators[1] += sample * math.cos(angle)
    return math.hypot(*accumulators) * 2 / (end - first)


class ListenerContract(WaterContract):
    owner = ParticleContract.owner

    def cook(self, params, expected_version):
        planned = self.world_call('world.water.plan', **params)
        pin = planned['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=pin) == planned
        package = json.loads((self.assets / (pin + '.pwater')).read_bytes())
        assert package['version'] == expected_version and package.get('listener_audio') == params.get('listener_audio')
        return pin

    def cold_save(self, revision):
        saves = self.root / 'water saves'
        saves.mkdir()
        session, reopened = uid(6301), uid(6302)
        starting = self.cases(
            ('configure', rpc('save.configure', request_id=uid(6310), expected_generation=0,
                              root=self.native(saves))),
            ('start', self.start(session, revision)),
            ('advance', self.step(session, 0, 2, 6311)),
            ('water', self.state(session, 2, points=[EYE])),
            ('saved', self.write(session, 2, 'checkpoint', 6312)))
        cold = self.cases(
            ('configure', rpc('save.configure', request_id=uid(6313), expected_generation=0,
                              root=self.native(saves))),
            ('loaded', self.load(reopened, revision, 6314)),
            ('water', self.state(reopened, 2, points=[EYE])),
            ('audio', rpc('runtime.audio.inspect', session_id=reopened, tick=2, listener=LISTENER)))
        assert cold['loaded']['tick'] == 2
        assert cold['water'] == starting['water'] | {'session_id': reopened}
        assert cold['audio']['listener_water']['target_blend'] == .5
        self.measurements['restored_target'] = cold['audio']['listener_water']

    def relocated(self, pin, revision, expected_pcm):
        runtime = self.args.runtime_root
        spec = json.loads((runtime / 'runtime.json').read_bytes())
        assert spec['features']['water_listener_audio'], spec
        source, moved = self.root / 'original build', self.root / 'relocated build'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest),
            '--output', self.native(source), '--runtime', self.native(runtime))))
        source.rename(moved)
        hidden = self.root / 'parked source project'
        self.project.rename(hidden)
        try:
            game = moved / 'game.json'
            executable = moved / json.loads(game.read_bytes())['engine']['executable']
            info = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
            assert info['needs_water_listener_audio'] and info['revision'] == revision, info
            assert list(moved.rglob('*.pwater'))[0].stem == pin
            saves = self.root / 'relocated saves'
            saves.mkdir()
            with self.owner('cold relocated', game=game, saves=saves) as (client, _):
                session = client.call('runtime.status')['session_id']
                advanced = self.step(session, 0, 2, 6323)['params']
                assert client.call('runtime.step', advanced)['current_tick'] == 2
                recorded = self.root / 'relocated-frozen.wav'
                response = client.call('runtime.audio.capture', dict(session_id=session, tick=2,
                    listener=LISTENER, path=self.native(recorded), frames=6500))
                assert response['listener_water']['target_blend'] == .5
                assert pcm(recorded) == expected_pcm
                self.measurements['relocated_capture'] = response['capture']['sha256']
        finally:
            hidden.rename(self.project)

    def run(self):
        caps = success(json.loads(self.cli('capabilities')))['features']
        assert caps['water_listener_audio'], caps
        recipe, _, revision = self.arm_terrain()
        source = self.root / 'test-tones.wav'
        write_tones(source)
        clip = self.world_call('asset.audio.import', source=self.native(source))['asset']
        floor = bed(*cell_center((0, 0)))
        wet_cell = 5.75 - floor
        params = dict(terrain=dict(source=recipe.name), tile_entities=TILES,
                      expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
                      grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4,
                                relaxation_per_second=0),
                      initial_depths_m=[wet_cell] + [0] * (len(CELLS) - 1),
                      exchanges=[dict(cell=[0, 0], source_m3_per_second=15, drain_m3_per_second=0)])
        self.cases(*[(key, rpc('world.water.plan', **(params | dict(revision=revision,
                         listener_audio=PROFILE | {key: value}))), -32602)
                     for key, value in [('cutoff_hz', True), ('gain', 1.01), ('full_depth_m', .001)]])
        dry_pin = self.cook(params | dict(revision=revision), 1)
        wet_pin = self.cook(params | dict(revision=revision, listener_audio=PROFILE), 5)
        assert wet_pin != dry_pin
        source.unlink()

        def pose(entity, position):
            return component(entity, 'Transform', dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]))

        initial = self.transaction(revision, 6101,
            dict(op='entity.create', id=LISTENER, name='Listener'),
            pose(LISTENER, EYE),
            component(LISTENER, 'BoxCollider', dict(half_extents=[.05, .05, .05], motion='kinematic',
                                                   mass=1, friction=.5, restitution=0)),
            dict(op='entity.create', id=EMITTER, name='Two-tone emitter'),
            pose(EMITTER, [12.5, 6, -3.5]),
            component(EMITTER, 'AudioEmitter', dict(asset=clip, gain=1, loop=True, enabled=True)))
        revision = success(self.world_calls(initial)[0])['revision']
        reports = {}
        for variant, pin in [('dry', dry_pin), ('wet', wet_pin)]:
            revision = success(self.world_calls(self.transaction(revision, 6102 + revision,
                dict(op='water.set', asset=pin)))[0])['revision']
            session = uid(6201)
            observe = lambda tick: rpc('runtime.audio.inspect', session_id=session, tick=tick, listener=LISTENER)
            move = dict(entity=LISTENER, position=[11.5, 6, -3.5], rotation=[0, 0, 0, 1], duration_ticks=1)
            play = rpc('runtime.audio.replay', session_id=session, request_id=uid(6202), expected_tick=0,
                listener=LISTENER, path=self.native(self.root / f'{variant}.wav'),
                sequence=[dict(ticks=12, sounds=[dict(op='play', emitter=EMITTER)]), dict(ticks=6, motions=[move])])
            capture = rpc('runtime.audio.capture', session_id=session, tick=2, listener=LISTENER,
                          path=self.native(self.root / f'{variant}-frozen.wav'), frames=6500)
            points = [EYE, [10.5, 6.25, -3.5], [10.5, floor, -3.5], [10.5, floor-.01, -3.5],
                      [11, 6, -3.5], [14, 6, -3.5], [1e300, 6, -3.5]]
            cases = self.cases(
                ('start', self.start(session, revision)),
                ('initial', observe(0)),
                ('bad_points', self.state(session, 0, points=[[0, True, 0]]), -32602),
                ('overflow_points', self.state(session, 0, points=[EYE] * 65), -32602),
                ('record', play), ('retry', play), ('returned', observe(18)),
                ('close', rpc('session.close')))
            assert cases['record']['success'] and cases['record']['capture']['frames'] == 14400, cases['record']
            assert cases['retry'] == cases['record'] | {'replayed': True}
            frozen = self.cases(('start', self.start(session, revision)),
                ('surface', self.step(session, 0, 1, 6204)), ('surface_audio', observe(1)),
                ('flooded', self.step(session, 1, 1, 6205)),
                ('stale', self.state(session, 1, points=points), -32009),
                ('points', self.state(session, 2, points=points)), ('frozen', capture))
            rows = frozen['points']['points']
            assert [row['point_world'] for row in rows] == points
            assert rows[0]['column']['cell'] == [0, 0] and rows[4]['column']['cell'] == [1, 0]
            assert rows[5]['column'] is None and rows[6]['column'] is None
            for row, depth in zip(rows, (.25, 0, 6.25 - floor, 0, 0, 0, 0), strict=True):
                near(row['submersion_m'], depth)
            if variant == 'wet':
                assert frozen['points']['listener_audio'] == PROFILE
                assert frozen['surface_audio']['listener_water']['target_blend'] == 0
                assert frozen['frozen']['listener_water']['target_blend'] == .5
                assert cases['record']['listener_water']['transition_frames'] == TRANSITION_FRAMES
                assert cases['record']['listener_water']['initial']['target_blend'] == 0
                assert cases['returned']['listener_water']['target_blend'] == 0
            else:
                assert 'listener_water' not in cases['record'] and 'listener_water' not in frozen['frozen']
            reports[variant] = cases['record']
        dry, wet = [pcm(self.root / f'{variant}.wav') for variant in ('dry', 'wet')]
        assert dry[:2 * FRAMES_PER_TICK] == wet[:2 * FRAMES_PER_TICK]
        assert dry[-2 * FRAMES_PER_TICK:] == wet[-2 * FRAMES_PER_TICK:]

        def target(frame):
            tick = frame // FRAMES_PER_TICK + 1
            return 0 if tick == 1 or tick >= 13 else .5 if tick == 2 else 1

        frozen_dry, frozen_wet = [pcm(self.root / f'{variant}-frozen.wav') for variant in ('dry', 'wet')]
        ratios = [amplitude(wet, f, 4800, 9600) / amplitude(dry, f, 4800, 9600) for f in FREQUENCIES]
        assert .55 < ratios[0] < .65 and .07 < ratios[1] < .13, ratios
        self.measurements.update(replay=reports, stream_error=filter_error(dry, wet, target, 0),
            frozen_error=filter_error(frozen_dry, frozen_wet, lambda _: .5, .5), amplitude_ratios=ratios)
        self.cold_save(revision)
        if self.args.runtime_root:
            self.relocated(wet_pin, revision, frozen_wet)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-listener-', dir=args.output))
    test = ListenerContract(args, root)
    report = dict(passed=False, calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        (root / 'evidence.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {root / "evidence.json"}')


if __name__ == '__main__':
    main()
