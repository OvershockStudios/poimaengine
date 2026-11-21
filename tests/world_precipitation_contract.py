#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check finite weather-driven particles across shelter, rollback, and fresh saves."""
import argparse
import json
from pathlib import Path
import tempfile

from scene_capture import pixels
from snow_foot_release_save_refusals import reseal_slot
from world_particles_contract import ParticleContract, component, counters, fixed, near_vector, transform
from world_terrain_contract import CAPTURE_LENS, CAPTURE_SIZE, error, file_hash, projection, rpc, success, tagged, tree_hashes, uid
from world_wetness_contract import motion


OPEN, COVERED, FIXED_ROOF, SLIDING_ROOF, DYNAMIC_ROOF, CAMERA = map(uid, range(940, 946))
OPEN_POS, COVERED_POS = [-1.55, 2.0, -4], [1.55, 2.0, -4]
CAMERA_POS = [0, 1.8, 0]
ROOF_AWAY, ROOF_COVERING = [-3, 3.2, -4], [-1.55, 3.2, -4]
FIRST_QUANTUM = 800
PROFILE = dict(nm_per_particle=2, max_particles_per_tick=2)
SAVE_RAIN = ('snapshot', 'payload', 'particles', 'precipitation')


def emitter(color):
    return dict(spawn_space='world', simulation_space='world', seed='0000000000000721',
                lifetime_ticks=12, size_m=.13, linear_rgb=color, live_limit=2,
                velocity_m_s=fixed([0, -6, 0]), rain=PROFILE)


def drop_position(origin, age):
    return [origin[0], origin[1]-age*.1, origin[2]]


class PrecipitationContract(ParticleContract):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Rain Shelter')))
        schema = self.world_call('world.describe.select', components=['ParticleEmitter'], sections=['wetness'])
        rain_schema = schema['components']['ParticleEmitter']['properties']['rain']
        assert set(rain_schema['required']) == set(PROFILE), rain_schema
        limits = schema['sections']['wetness']['precipitation']
        assert (limits['component'], limits['window_ticks'], limits['max_particles_per_tick']) == (
            'ParticleEmitter.rain', 60, 32), limits
        revision = self.world_call('world.inspect')['revision']
        ops = [dict(op='entity.create', id=CAMERA, name='Rain verification camera'),
               component(CAMERA, 'Transform', transform(CAMERA_POS)), component(CAMERA, 'Camera', CAPTURE_LENS)]
        for entity, label, position, color in ((OPEN, 'Rain above the path', OPEN_POS, [1, .01, .04]),
                                               (COVERED, 'Rain under the awning', COVERED_POS, [.01, 1, .04])):
            ops += [dict(op='entity.create', id=entity, name=label),
                    component(entity, 'Transform', transform(position)), component(entity, 'ParticleEmitter', emitter(color))]
        roofs = ((FIXED_ROOF, 'Shelter awning', [1.55, 3.2, -4], 'static'),
                 (SLIDING_ROOF, 'Sliding cover', ROOF_AWAY, 'kinematic'),
                 (DYNAMIC_ROOF, 'Dynamic obstruction control', [-1.55, 10, -4], 'dynamic'))
        for entity, label, position, motion_type in roofs:
            ops += [dict(op='entity.create', id=entity, name=label),
                    component(entity, 'Transform', transform(position)),
                    component(entity, 'BoxCollider', dict(motion=motion_type, half_extents=[.5]*3,
                                                          mass=1, friction=.5, restitution=0))]
            if motion_type != 'dynamic':
                tint = [.18, .12, .09]
                ops += [component(entity, 'MeshRenderer', dict(primitive='box', albedo=tint,
                             visible=True, cast_shadows=False, receive_shadows=False)),
                        component(entity, 'PbrMaterial', dict(base_color=tint, emissive=[0, 0, 0], metallic=0,
                             roughness=.8, double_sided=False))]
        authored = self.world_call('world.transact', request_id=uid(2000), base_revision=revision, ops=ops)
        assert authored['committed'] and authored['revision'] == revision+1, authored
        revision = authored['revision']
        original = self.world.read_bytes()
        authored_effect = emitter([1, .01, .04])
        bad = [component(OPEN, 'ParticleEmitter', authored_effect | override) for override in (
            dict(spawn_space='local'), dict(particles_per_second=20), dict(burst=dict(action='rain', count=1)),
            dict(rain=PROFILE | dict(nm_per_particle=True)), dict(rain=PROFILE | dict(max_particles_per_tick=33)))]
        bad.append(dict(op='template.set', id=uid(949), name='Unsupported spawned rain',
                        components=dict(Transform=transform([0, 0, 0]), ParticleEmitter=authored_effect)))
        self.measurements['authoring_refusals'] = [error(reply, -32602) for reply in self.world_calls(*(
            rpc('world.transact', request_id=uid(2010+index), base_revision=revision, ops=[operation])
            for index, operation in enumerate(bad)))]
        assert self.world.read_bytes() == original
        return revision

    @staticmethod
    def guard(session, tick, request, ticks=1):
        return dict(session_id=session, expected_tick=tick, expected_structure_revision=0,
                    request_id=uid(request), ticks=ticks)

    def inspect(self, client, session, tick, *, depth, remaining, sample_tick, phase, scheduled,
                blocker, spawned, live, dropped):
        observed = self.observed(client, session, tick)
        assert observed['rain'] == dict(rain_nm=depth, ticks_remaining=remaining, sample_tick=sample_tick), observed
        assert observed['counts'] == counters(live, spawned, dropped, spawned-live), observed
        active, sheltered = observed['sources']
        assert active['entity'] == OPEN and sheltered['entity'] == COVERED, observed
        assert active['rain'] == dict(phase=phase, scheduled_births=scheduled, blocker=blocker), active
        assert sheltered['rain'] == active['rain'] | dict(blocker=FIXED_ROOF if sample_tick else None), sheltered
        assert active['counts'] == observed['counts'] and sheltered['counts'] == counters(0, 0), observed
        assert all(row['emitter'] == active['id'] for row in observed['particles']), observed
        return {key: value for key, value in observed.items() if key != 'session_id'}

    def step_rain(self, client, session, tick, request, first_quantum, rain_nm, *, ticks=1, retry=False):
        params = self.guard(session, tick, request, ticks) | dict(wetness_dose=dict(first_quantum=first_quantum,
                                                                                 rain_nm=rain_nm))
        receipt = client.call('runtime.wetness.step', params)
        assert receipt['current_tick'] == tick+ticks and receipt['replayed'] is False, receipt
        if retry:
            assert client.call('runtime.wetness.step', params) == receipt | dict(replayed=True)

    def follow(self, client, session, request):
        states = []
        self.step(client, session, 3, request, 2, motions=[motion(SLIDING_ROOF, ROOF_COVERING, 2)])
        for tick, phase, scheduled, blocker, dropped in ((5, 90, 0, SLIDING_ROOF, 1), (6, 60, 1, SLIDING_ROOF, 1),
                                                         (7, 30, 1, None, 2)):
            if tick > 5:
                options = dict(motions=[motion(SLIDING_ROOF, ROOF_AWAY, 1)]) if tick == 7 else {}
                self.step(client, session, tick-1, request+tick, **options)
            states.append(self.inspect(client, session, tick, depth=90, remaining=60-tick, sample_tick=tick,
                                       phase=phase, scheduled=scheduled, blocker=blocker, spawned=2, live=2, dropped=dropped))
        self.step_rain(client, session, 7, request+8, FIRST_QUANTUM+2, [1_000_000, 0], ticks=14)
        states.append(self.inspect(client, session, 21, depth=0, remaining=0, sample_tick=7, phase=30, scheduled=1,
                                   blocker=None, spawned=2, live=0, dropped=2))
        self.step_rain(client, session, 21, request+9, FIRST_QUANTUM+4, [1_000_000], ticks=60)
        states.append(self.inspect(client, session, 81, depth=1_000_000, remaining=0, sample_tick=81, phase=30, scheduled=2,
                                   blocker=None, spawned=12, live=2, dropped=112))
        self.step(client, session, 81, request+10)
        states.append(self.inspect(client, session, 82, depth=1_000_000, remaining=0, sample_tick=81, phase=30, scheduled=2,
                                   blocker=None, spawned=12, live=0, dropped=112))
        return states

    def exercise(self, revision, game=None):
        saves = self.root/('Exported rain saves' if game else 'Development rain saves')
        saves.mkdir()
        with self.owner('warm '+saves.name, game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(2050), saves, game)
            self.inspect(client, session, 0, depth=None, remaining=0, sample_tick=None, phase=0, scheduled=0,
                         blocker=None, spawned=0, live=0, dropped=0)
            control = client.call('runtime.raycast', dict(session_id=session, tick=0, origin=OPEN_POS,
                                                        direction=[0, 1, 0], distance=100))
            assert control['hit']['entity'] == DYNAMIC_ROOF, control
            if self.args.gpu is not None:
                self.capture(client, session, 0)
            self.step_rain(client, session, 0, 2051, FIRST_QUANTUM, [1_000_000, 90], retry=True)
            self.inspect(client, session, 1, depth=90, remaining=59, sample_tick=1, phase=90, scheduled=0,
                         blocker=None, spawned=0, live=0, dropped=0)
            self.step(client, session, 1, 2052, 2)
            checkpoint = self.inspect(client, session, 3, depth=90, remaining=57, sample_tick=3, phase=30, scheduled=1,
                                      blocker=None, spawned=2, live=2, dropped=0)
            for row, age in zip(checkpoint['particles'], [1, 0], strict=True):
                assert row['age_ticks'] == age, row
                near_vector(row['velocity_m_s'], [0, -6, 0])
                near_vector(row['position_m'], drop_position(OPEN_POS, age))
            if self.args.gpu is not None:
                self.capture(client, session, 3)
            checkpoint_archive = self.save(client, saves, 'checkpoint', session, 3, 2053)
            clock = json.loads(checkpoint_archive)['snapshot']['payload']['wetness']
            assert clock == dict(version=1, next_quantum=FIRST_QUANTUM+2, receivers=[]), clock
            if game is None:
                self.failed_batch(client, session, saves, checkpoint_archive)
                reseal_slot(saves/'slot-checkpoint', saves, 'invalid', SAVE_RAIN+('sources', 0, 'phase'), 120)
                reseal_slot(saves/'slot-checkpoint', saves, 'invalid-window', SAVE_RAIN+('ticks_remaining',), 0)
            expected = self.follow(client, session, 2060)
            final = self.save(client, saves, 'warm-final', session, 82, 2079)

        with self.owner('cold '+saves.name, game=game, saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(2080), saves, game)
            session = uid(2081)
            if game is None:
                invalid = dict(request_id=uid(2082), configuration_generation=1, slot='invalid', expected_generation=1,
                    revision=revision, expected_session_id=previous, expected_tick=0, expected_gameplay_revision=0,
                    expected_structure_revision=0, new_session_id=session)
                failure = self.refuse(client, 'save.load', invalid, -32070)
                assert 'snapshot integer is out of range' in failure, failure
                self.measurements['tampered_phase'] = failure
                failure = self.refuse(client, 'save.load', invalid | dict(request_id=uid(2120), slot='invalid-window'), -32070)
                assert 'window could not have elapsed' in failure, failure
                self.measurements['tampered_window'] = failure
            assert self.load(client, revision, previous, session, 'checkpoint', 3, 2083) == 0
            restored = self.observed(client, session, 3)
            assert {key: value for key, value in restored.items() if key != 'session_id'} == checkpoint
            assert self.save(client, saves, 'restored-checkpoint', session, 3, 2084) == checkpoint_archive
            assert self.follow(client, session, 2090) == expected
            assert self.save(client, saves, 'cold-final', session, 82, 2110) == final
        assert warm['pid'] != cold['pid'], (warm, cold)
        self.measurements[saves.name] = dict(checkpoint=checkpoint, continued=expected)

    @staticmethod
    def refuse(client, method, params, code):
        from poima_client import RpcError
        try:
            client.call(method, params)
        except RpcError as failure:
            assert failure.code == code, failure
            return str(failure)
        raise AssertionError(f'{method} accepted {params!r}')

    def failed_batch(self, client, session, saves, checkpoint):
        # Request a valid mark on the moving cover with a ray that misses after
        # the physics and rain phases have completed the tentative tick.
        mark = dict(receiver=SLIDING_ROOF, ray=dict(origin=[40, 4, 40], direction=[0, -1, 0], distance=6),
                    half_extent_m=[.12, .12], half_depth_m=.05, minimum_normal_dot=.8,
                    edge_fraction=.25, linear_rgb=[.18, .1, .12], opacity=1, roughness=.6)
        request = self.guard(session, 3, 2054) | dict(wetness_dose=dict(first_quantum=FIRST_QUANTUM+2, rain_nm=[96]),
                    motions=[motion(SLIDING_ROOF, ROOF_COVERING, 1)], surface_marks=[mark])
        failure = self.refuse(client, 'runtime.wetness.step', request, -32040)
        assert 'must first hit the expected receiver' in failure, failure
        assert self.save(client, saves, 'after-rejected-tick', session, 3, 2055) == checkpoint
        self.measurements['late_failure'] = failure

    def capture(self, client, session, tick):
        width, height = CAPTURE_SIZE
        path = self.root/f'precipitation-{tick}.bmp'
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0, report
        assert report['tick'] == tick and report['lens'] == CAPTURE_LENS and report['nvrhi_errors'] == 0, report
        image = pixels(path)
        probes = []
        for origin, channel in ((OPEN_POS, 0), (COVERED_POS, 1)):
            for age in (0, 1):
                x, y = map(int, projection(report['camera_world'], CAPTURE_LENS, drop_position(origin, age)))
                color = image[y][x]
                assert tagged(color, channel) == (tick == 3 and channel == 0), (tick, origin, age, color)
                probes.append(dict(age=age, pixel=[x, y], color=color))
        self.measurements.setdefault('captures', {})[tick] = dict(sha256=file_hash(path), report=report, probes=probes)

    def couple_with_wetness(self, revision):
        """Observe the whole dose on film and only its final depth in the particle window."""
        tint = [.4, .4, .4]
        authored = self.world_call('world.transact', request_id=uid(2120), base_revision=revision, ops=[
            component(OPEN, 'MeshRenderer', dict(primitive='box', albedo=tint, visible=True)),
            component(OPEN, 'PbrMaterial', dict(base_color=tint, emissive=[0, 0, 0], metallic=0,
                                                 roughness=.8, double_sided=False)),
            component(OPEN, 'SurfaceWetness', dict(capacity_nm=2_000_000, retention_q16=65_536,
                drying_nm_per_second=0, probe_local_m=[0, 0, 0], darkening=.35, roughness_factor=.32)),
        ])
        assert authored['committed'] and authored['revision'] == revision+1, authored
        saves = self.root/'Mixed rain saves'
        saves.mkdir()
        with self.owner('mixed rain and film', saves=saves) as (client, _):
            session = self.initialize(client, authored['revision'], uid(2121), saves, None)
            self.step_rain(client, session, 0, 2122, FIRST_QUANTUM, [1_000_000, 90], ticks=3)
            rain = self.inspect(client, session, 3, depth=90, remaining=57, sample_tick=3, phase=30,
                                scheduled=1, blocker=None, spawned=2, live=2, dropped=0)
            wetness = client.call('runtime.wetness.inspect', dict(session_id=session, tick=3, structure_revision=0))
            assert wetness['receivers'] == [dict(entity=OPEN, capacity_nm=2_000_000, blocker=None,
                                                  exposed=True, film_nm_q16=1_000_090*65_536)], wetness
            assert wetness['next_quantum'] == FIRST_QUANTUM+2, wetness
            self.measurements['mixed_rain_and_film'] = dict(rain=rain, wetness=wetness)

    def compiled_weather(self, revision):
        """Drive a rain-only scene from the compiled weather package and gameplay service."""
        descriptor = self.args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert metadata['type'] == 'Poima.Tests.ManagedWetnessGame', metadata
        saves = self.root/'Compiled weather saves'
        saves.mkdir()
        with self.owner('compiled weather drives particles', saves=saves) as (client, _):
            session = self.initialize(client, revision, uid(2130), saves, None)
            loaded = client.call('runtime.gameplay.load_native', dict(session_id=session, request_id=uid(2131),
                expected_tick=0, expected_revision=0, descriptor=self.native(descriptor),
                expected_descriptor_sha256=file_hash(descriptor)))
            assert loaded['revision'] == 1 and loaded['replayed'] is False, loaded
            self.step(client, session, 0, 2132, 3)
            rain = self.inspect(client, session, 3, depth=7474, remaining=59, sample_tick=3, phase=0,
                                scheduled=2, blocker=None, spawned=2, live=2, dropped=4)
            wetness = client.call('runtime.wetness.inspect', dict(session_id=session, tick=3, structure_revision=0))
            assert wetness['next_quantum'] == 61 and wetness['receivers'] == [], wetness
            self.measurements['compiled_weather'] = dict(descriptor_sha256=file_hash(descriptor), rain=rain, wetness=wetness)

    def run(self):
        revision = self.author()
        original = tree_hashes(self.project)
        if self.args.runtime_root:
            initial = self.root/'Initial Export'
            success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(initial),
                                       '--runtime', self.native(self.args.runtime_root))))
            bundle, moved = self.root/'Relocated Rain Game', self.root/'Unavailable Rain Source'
            initial.rename(bundle)
            self.project.rename(moved)
            frozen = tree_hashes(bundle)
            self.exercise(revision, bundle/'game.json')
            assert tree_hashes(bundle) == frozen and tree_hashes(moved) == original and not self.project.exists()
            self.measurements['export'] = dict(bundle=frozen, unavailable_source=original)
        else:
            self.exercise(revision)
            if self.args.native_descriptor:
                self.compiled_weather(revision)
            assert tree_hashes(self.project) == original
            self.couple_with_wetness(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed standalone runtime.')
    parser.add_argument('--native-descriptor', type=Path, help='Optional Native AOT ManagedWetnessGame descriptor.')
    parser.add_argument('--output', type=Path, help='Parent directory for captures and wire observations.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if args.gpu is not None and (args.runtime_root is None or args.output is None):
        parser.error('--gpu requires --runtime-root and --output')
    if args.native_descriptor and args.runtime_root:
        parser.error('--native-descriptor runs against the development runtime')
    if args.windows_interop and args.output is None:
        parser.error('--windows-interop requires --output on a Windows-accessible drive')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    def execute(temporary):
        root = Path(temporary)
        fixture = PrecipitationContract(args, root)
        try:
            fixture.run()
        finally:
            if args.output:
                (root/'results.json').write_text(json.dumps(dict(calls=fixture.calls, measurements=fixture.measurements),
                                                            indent=2)+'\n', encoding='utf-8')
        print('Precipitation contract passed.', root if args.output else '')
    if args.output:
        execute(tempfile.mkdtemp(prefix='poima-rain-', dir=args.output))
    else:
        with tempfile.TemporaryDirectory(prefix='poima-rain-') as temporary:
            execute(temporary)


if __name__ == '__main__':
    main()
