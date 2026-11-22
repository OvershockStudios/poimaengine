#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check compiled solar lighting, rollback, exact saves, and an optional Vulkan export."""

import argparse
import json
import math
from pathlib import Path
import shutil
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from sky_capture import display
from world_particles_contract import ParticleContract, RpcError, component, near_vector
from world_terrain_contract import file_hash, success, tree_hashes, uid


TYPE = 'Poima.Tests.ManagedSolarLightingGame'
IDENTITY = 'poima.test.managed-solar-lighting'
LIGHT_COLOR = [1, .75, .5]
LIGHT_LUX = 8
SKY_COLOR = [.25, .5, .75]
DISK_INTENSITY = 4
EXPOSURE = .5
IBL = dict(enabled=True, diffuse_intensity=.75, specular_intensity=.5)
SIZE = (320, 180)
PHASES = {
    1: (25_200_000, .5, [1, .375, .125], .5, [.03125, .0625, .125]),
    2: (43_200_000, 1, [1, 1, 1], 1, [.125, .125, .125]),
    4: (61_200_000, .5, [1, .5, .25], .375, [.0625, .09375, .1875]),
    5: (86_400_000, 0, [.25, .5, 1], .0625, [.0078125, .015625, .03125]),
}
CHECKPOINT = 2
FINAL = 5
VALIDATION_FAILURES = ('VUID-', 'SYNC-HAZARD', 'Validation Error', 'Validation Warning')
VALIDATION_LAYER = 'Inserted device layer "VK_LAYER_KHRONOS_validation"'


def solar_reference(tick):
    if tick == 0:
        return None
    time, gain, tint, sky, ambient = PHASES[2 if tick == 3 else tick]
    # For zero obliquity, rotate the unit hour circle about world east by
    # this fixture's latitude. The independent oracle does not call SolarClock.
    hour_angle = math.tau * ((time % 86_400_000) / 86_400_000 - .5)
    toward = [-math.sin(hour_angle), math.cos(hour_angle) * math.sqrt(3) / 2, math.cos(hour_angle) / 2]
    return dict(version=1, world_milliseconds=time, toward_sun=toward, sun_gain=gain, sun_tint=tint,
                sky_gain=sky, ambient=ambient)


def without_session(result):
    return {key: value for key, value in result.items() if key != 'session_id'}


class SolarContract(ParticleContract):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Solar Lighting Fixture')))
        doc = json.loads(self.world.read_bytes())
        entry = json.loads(self.manifest.read_bytes())['entry']
        self.camera, self.controller = entry['camera'], entry['controller']
        self.environment = next(key for key, row in doc['entities'].items() if 'LightingEnvironment' in row['components'])
        authored = doc['entities'][self.environment]['components']['LightingEnvironment']
        self.sun = authored['sky']['sun']
        authored['exposure'] = EXPOSURE
        authored['shadow_resolution'] = 256
        authored['sky'].update(zenith=SKY_COLOR, horizon=SKY_COLOR, ground=SKY_COLOR,
                               sun_intensity=DISK_INTENSITY, sun_size_degrees=4)
        if getattr(self.args, 'ibl_replay', False):
            authored['ibl'] = IBL.copy()
        self.authored_environment = authored
        self.authored_sun = doc['entities'][self.sun]['components']['Light'] | dict(
            color=LIGHT_COLOR, intensity=LIGHT_LUX)
        edited = self.world_call('world.transact', request_id=uid(6400), base_revision=doc['revision'],
            ops=[component(self.environment, 'LightingEnvironment', authored),
                 component(self.sun, 'Light', self.authored_sun)])
        assert edited['committed'] and edited['replayed'] is False, edited
        self.revision = edited['revision']
        self.legacy_lighting = self.world_call('world.lighting', revision=self.revision)
        assert 'solar_lighting' not in self.legacy_lighting and self.legacy_lighting['shadow_views'] == 4
        self.measurements['authored_lighting'] = self.legacy_lighting

    def configure_artifact(self):
        args = self.args
        if args.native_descriptor is None:
            sdk = args.assembly.with_name('Poima.Gameplay.dll')
            assert file_hash(sdk) == file_hash(args.bridge.with_name(sdk.name))
            self.image_hash = file_hash(args.assembly)
            self.measurements['artifact'] = dict(assembly_sha256=self.image_hash,
                weather_sha256=file_hash(args.assembly.with_name('Poima.Weather.dll')),
                sdk_sha256=file_hash(sdk), bridge_sha256=file_hash(args.bridge),
                hostfxr_sha256=file_hash(args.hostfxr))
            return 'runtime.gameplay.load', dict(hostfxr=self.native(args.hostfxr), bridge=self.native(args.bridge),
                                                  assembly=self.native(args.assembly), type=TYPE)
        descriptor = args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert (metadata['type'], metadata['identity'], metadata['services_version'],
                metadata['minimum_services_bytes']) == (TYPE, IDENTITY, 7, 584), metadata
        assert set(metadata['required_features']) == {'baseline_v7', 'solar_lighting_v1'}, metadata
        self.image_hash = file_hash(descriptor.parent / metadata['library'])
        self.expected_native_library = self.native(descriptor.parent / metadata['library'])
        digest = file_hash(descriptor)
        self.measurements['artifact'] = dict(descriptor_sha256=digest, files=metadata['files'],
                                           library_sha256=self.image_hash)
        targets = self.project / 'gameplay'
        for row in metadata['files']:
            path = (descriptor.parent / row['path']).resolve(strict=True)
            assert path.is_relative_to(descriptor.parent), row
            assert path.stat().st_size == row['size'] and file_hash(path) == row['sha256'], row
            if args.runtime_root:
                target = targets / row['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        if args.runtime_root:
            shutil.copyfile(descriptor, targets / 'native-gameplay.json')
            manifest = json.loads(self.manifest.read_bytes())
            manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
            self.manifest.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        return 'runtime.gameplay.load_native', dict(descriptor=self.native(descriptor), expected_descriptor_sha256=digest)

    def start(self, client, saves, identity, game=None):
        session = self.initialize(client, self.revision, uid(identity), saves, game)
        if game is None:
            method, config = self.config
            receipt = client.call(method, dict(session_id=session, request_id=uid(identity + 1), expected_tick=0,
                                               expected_revision=0, **config))
            assert receipt['revision'] == 1 and receipt['replayed'] is False, receipt
        return session

    def light_at(self, client, session, tick):
        light = client.call('runtime.lighting', dict(session_id=session, tick=tick))
        assert (light['session_id'], light['tick']) == (session, tick), light
        return without_session(light)

    def state(self, client, session, tick, fail_at=-1):
        light = self.light_at(client, session, tick)
        assert light['shadow_views'] == 4, light
        sample = solar_reference(tick)
        if sample is None:
            assert 'solar_lighting' not in light, light
        else:
            actual = light['solar_lighting']
            assert set(actual) == set(sample), actual
            assert type(actual['version']) is type(actual['world_milliseconds']) is int, actual
            near_vector(actual['toward_sun'], sample['toward_sun'], tolerance=1e-7)
            assert actual | dict(toward_sun=sample['toward_sun']) == sample, actual
            assert light['exposure'] == EXPOSURE and light['ambient'] == sample['ambient'], light
            for key in ('zenith', 'horizon', 'ground'):
                assert light['sky'][key] == SKY_COLOR, light
            [sun] = [source for source in light['lights'] if source['id'] == self.sun]
            near_vector(sun['direction'], [-value for value in sample['toward_sun']], tolerance=1e-6)
            near_vector(sun['color'], [a*b for a, b in zip(LIGHT_COLOR, sample['sun_tint'], strict=True)])
            assert sun['intensity'] == LIGHT_LUX * sample['sun_gain'], sun
            near_vector(light['sky_sun']['direction'], sample['toward_sun'], tolerance=1e-6)
            assert light['sky_sun']['color'] == sun['color'] and light['sky_sun']['intensity'] == sun['intensity'], light
            assert light['sky']['sun_intensity'] == DISK_INTENSITY * sample['sun_gain'], light['sky']
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity'], module['assembly_sha256']) == (
            backend, TYPE, IDENTITY, self.image_hash), module
        assert exact(module['values'], dict(Ticks=tick, FailAt=str(fail_at))), module
        if self.args.native_descriptor:
            assert exact(module['native_diagnostics'],
                         dict(dynamic_code_supported=False, dynamic_code_compiled=False)), module
            actual_path = module['native_library'].replace('\\', '/')
            expected_path = self.expected_native_library.replace('\\', '/')
            if self.args.windows_interop:
                actual_path, expected_path = actual_path.casefold(), expected_path.casefold()
            assert actual_path == expected_path, module
        assert (info['session_id'], info['tick'], info['structure_revision']) == (session, tick, 0), info
        return dict(lighting=light, gameplay=module['values'], gameplay_revision=info['revision'])

    def archive(self, client, saves, session, state, slot):
        tick = state['lighting']['tick']
        receipt = client.call('save.write', dict(request_id=uid(6500 + len(self.measurements['archives'])),
            configuration_generation=1, expected_generation=0, slot=slot, session_id=session, expected_tick=tick,
            expected_gameplay_revision=state['gameplay_revision'], expected_structure_revision=0))
        path, = (saves / ('slot-' + slot)).glob('p-*.bin')
        data = path.read_bytes()
        sha = file_hash(path)
        assert receipt['sha256'] == sha and receipt['bytes'] == len(data), receipt
        payload = json.loads(data)['snapshot']['payload']
        assert payload['tick'] == tick and exact(payload['gameplay']['values'], state['gameplay']), payload
        if 'solar_lighting' in state['lighting']:
            assert exact(payload['solar_lighting'], state['lighting']['solar_lighting']), payload
        else:
            assert 'solar_lighting' not in payload, payload
        self.measurements['archives'][slot] = dict(path=str(path), sha256=sha, bytes=len(data), tick=tick)
        return data

    def advance(self, client, session, tick, identity, ticks=1):
        params = dict(session_id=session, expected_tick=tick, request_id=uid(identity),
                      expected_structure_revision=0, ticks=ticks)
        report = client.call('runtime.step', params)
        assert exact([report['current_session_id'], report['current_tick'], report['stepped']],
                     [session, tick+ticks, ticks]), report
        assert report['replayed'] is False and report['runtime_replaced'] is False, report
        if tick == 0:
            assert exact(client.call('runtime.step', params), report | dict(replayed=True)), report

    def change_failure(self, client, session, state, fail_at, identity):
        edited = client.call('runtime.gameplay.edit', dict(session_id=session, request_id=uid(identity),
            expected_tick=state['lighting']['tick'], expected_revision=state['gameplay_revision'],
            values=dict(FailAt=str(fail_at))))
        assert edited['revision'] == state['gameplay_revision'] + 1 and edited['replayed'] is False, edited

    def rollback(self, client, saves, session, state):
        tick = state['lighting']['tick']
        assert tick == 1
        self.change_failure(client, session, state, 2, 6420)
        guarded = self.state(client, session, tick, fail_at=2)
        before = self.archive(client, saves, session, guarded, 'before-late-failure')
        try:
            self.advance(client, session, tick, 6421, ticks=2)
        except RpcError as error:
            assert error.code == -32040 and 'Solar lighting later-Tick rollback fixture.' in error.message, error
            self.measurements['late_failure'] = dict(code=error.code, message=error.message)
        else:
            raise AssertionError('Expected the later Tick to reject the complete multi-tick batch.')
        after = self.state(client, session, tick, fail_at=2)
        assert exact(after, guarded), after
        assert self.archive(client, saves, session, after, 'after-late-failure') == before
        self.change_failure(client, session, after, -1, 6422)

    def capture(self, client, session, state, label):
        tick = state['lighting']['tick']
        path = self.root / (label + '.bmp')
        width, height = SIZE
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=self.camera, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2))
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors'] == 0, report
        assert report['revision'] == self.revision and report['tick'] == tick, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, report
        if getattr(self.args, 'ibl_replay', False):
            assert report['lighting']['ibl'] == IBL and diagnostics['environment_ibl']['enabled'] is True, report
        sample = state['lighting'].get('solar_lighting')
        assert ('solar_lighting' in report['lighting']) == (sample is not None), report['lighting']
        if sample is not None:
            assert exact(report['lighting']['solar_lighting'], sample), report['lighting']
        bitmap = pixels(path)
        x, y = width // 2, height // 4
        actual = bitmap[y][x]
        # All authored sky stops use the same RGB. The default camera views
        # north and does not face the sun at any nonzero-gain fixture sample.
        # The upper-quarter ray also clears the floor, leaving pure sky.
        expected = [display(channel * EXPOSURE * (sample or {'sky_gain': 1})['sky_gain']) for channel in SKY_COLOR]
        assert max(abs(a-b) for a, b in zip(actual, expected, strict=True)) <= 2, (label, actual, expected)
        row = dict(sha256=file_hash(path), gpu=report['gpu'], tick=tick, rgb=actual, expected_rgb=expected)
        self.measurements['captures'][label] = row
        return row

    def verify_ibl_replay(self, client, session, warm_states, night_capture):
        path = self.root / 'continuous-ibl-replay.bmp'
        width, height = SIZE
        report = client.call('runtime.play', dict(session_id=session, expected_tick=0, request_id=uid(6484),
            expected_structure_revision=0, controller=self.controller, camera=self.camera, mode='replay',
            sequence=[dict(ticks=FINAL)], path=self.native(path), width=width, height=height, samples=1,
            gpu=self.args.gpu, lighting_path=self.args.lighting_path, reconstruction='none', audio=False))
        assert report['success'] and report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        assert exact([report['previous_tick'], report['tick'], report['stop_reason']], [0, FINAL, 'replay_complete']), report
        assert report['initial_session_id'] == report['current_session_id'] == session, report
        assert report['runtime_replacements'] == 0, report
        # The Windows surface can resize as it opens. Rebuilding swapchain
        # targets leaves this renderer's environment resources and generation intact.
        assert report['swapchain_rebuilds'] in (0, 1), report
        # Replay advances exactly one tick before each normal draw, then
        # presents once more for the terminal capture on the same renderer.
        assert report['frames_presented'] == FINAL + 1 and report['gpu'] == night_capture['gpu'], report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        assert diagnostics['completed_submissions'] == report['frames_presented'], diagnostics
        assert report['materials']['presented']['tick'] == FINAL, report['materials']
        ibl = diagnostics['environment_ibl']
        assert ibl['enabled'] is True and ibl['texture_bytes'] > 0 and ibl['generation'] == 1, ibl
        assert report['lighting']['ibl'] == IBL, report['lighting']
        sample = warm_states[FINAL]['lighting']['solar_lighting']
        assert exact(report['lighting']['solar_lighting'], sample), report['lighting']
        state = self.state(client, session, FINAL)
        assert exact(state['lighting'], warm_states[FINAL]['lighting']), state
        digest = file_hash(path)
        assert digest == night_capture['sha256'], (digest, night_capture)
        self.measurements['continuous_ibl_replay'] = dict(rendered_frames=report['frames_presented'],
            terminal_tick=FINAL, capture_sha256=digest, lighting=report['lighting'], environment_ibl=ibl,
            swapchain_rebuilds=report['swapchain_rebuilds'])

    def legacy(self):
        with self.owner('legacy without compiled module') as (client, _):
            session = uid(6430)
            client.call('runtime.start', dict(session_id=session, revision=self.revision))
            initial = self.light_at(client, session, 0)
            assert 'solar_lighting' not in initial
            self.advance(client, session, 0, 6431, ticks=FINAL)
            assert exact(self.light_at(client, session, FINAL), initial | dict(tick=FINAL))

    def restore(self, client, previous, previous_state, tick, identity):
        slot = f'warm-{tick}'
        inspected = client.call('save.inspect', dict(slot=slot))
        selected = inspected['selected']
        archived = self.measurements['archives'][slot]
        assert inspected['recovered'] is False and inspected['manifest_recovered'] is False, inspected
        assert exact(selected, dict(verified=True, generation=1, sha256=archived['sha256'],
                                    bytes=archived['bytes'])), inspected
        session = uid(identity)
        loaded = client.call('save.load', dict(request_id=uid(identity + 1), configuration_generation=1,
            slot=slot, expected_generation=1, revision=self.revision, expected_session_id=previous,
            expected_tick=previous_state['lighting']['tick'], expected_structure_revision=0,
            expected_gameplay_revision=previous_state['gameplay_revision'], new_session_id=session))
        assert exact([loaded['session_id'], loaded['tick'], loaded['structure_revision'], loaded['source_stale']],
                     [session, tick, 0, False]), loaded
        return session, loaded

    def exercise(self, game):
        saves = self.root / 'External Solar Saves'
        saves.mkdir()
        warm_states, warm_captures = {}, {}
        with self.owner('warm compiled solar', game=game, saves=saves) as (client, warm):
            session = self.start(client, saves, 6440, game)
            for tick in range(FINAL + 1):
                state = warm_states[tick] = self.state(client, session, tick)
                if tick in (0, CHECKPOINT, FINAL):
                    archived = self.archive(client, saves, session, state, f'warm-{tick}')
                    if tick == CHECKPOINT:
                        checkpoint_bytes = archived
                if self.args.gpu is not None and tick != 3:
                    warm_captures[tick] = self.capture(client, session, state, f'warm-{tick}')
                if tick == 1:
                    self.rollback(client, saves, session, state)
                if tick < FINAL:
                    self.advance(client, session, tick, 6450 + tick)
            assert exact(warm_states[3]['lighting'], warm_states[2]['lighting'] | dict(tick=3))
            warm_final = archived
            restored, same_process_load = self.restore(client, session, warm_states[FINAL], CHECKPOINT, 6480)
            state = self.state(client, restored, CHECKPOINT)
            assert exact(state, warm_states[CHECKPOINT]), state
            assert self.archive(client, saves, restored, state, 'warm-restored') == checkpoint_bytes
            initial_session, unsampled_load = self.restore(client, restored, state, 0, 6482)
            assert exact(self.state(client, initial_session, 0), warm_states[0])
            if getattr(self.args, 'ibl_replay', False):
                self.verify_ibl_replay(client, initial_session, warm_states, warm_captures[FINAL])

        with self.owner('cold compiled solar', game=game, saves=saves) as (client, cold):
            previous = self.start(client, saves, 6460, game)
            initial = self.state(client, previous, 0)
            session, loaded = self.restore(client, previous, initial, CHECKPOINT, 6462)
            for tick in range(CHECKPOINT, FINAL + 1):
                state = self.state(client, session, tick)
                assert exact(state, warm_states[tick]), state
                if tick in (CHECKPOINT, FINAL):
                    archived = self.archive(client, saves, session, state, f'cold-{tick}')
                    assert archived == (checkpoint_bytes if tick == CHECKPOINT else warm_final)
                    if self.args.gpu is not None:
                        assert exact(self.capture(client, session, state, f'cold-{tick}'), warm_captures[tick])
                if tick < FINAL:
                    self.advance(client, session, tick, 6470 + tick)
        assert warm['pid'] != cold['pid'], (warm, cold)
        self.measurements.update(observations=warm_states, warm_load=same_process_load,
                                 unsampled_load=unsampled_load, cold_load=loaded)

    def run(self):
        self.measurements.update(archives={}, captures={})
        self.author()
        self.legacy()
        world_hash = file_hash(self.world)
        self.config = self.configure_artifact()
        if not self.args.runtime_root:
            self.exercise(None)
            assert file_hash(self.world) == world_hash
            return

        initial = self.root / 'Initial Export'
        inspection = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert inspection['gameplay']['requirements']['minimum_services_bytes'] == 584, inspection
        success(json.loads(self.cli('project', 'build', self.native(self.manifest),
            '--output', self.native(initial), '--runtime', self.native(self.args.runtime_root))))
        relocated = self.root / 'Relocated Solar Game'
        detached = self.root / 'Unavailable Solar Source'
        initial.rename(relocated)
        self.project.rename(detached)
        assert not self.project.exists() and not initial.exists()
        published, source = tree_hashes(relocated), tree_hashes(detached)
        game = relocated / 'game.json'
        package = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        descriptor = relocated / package['gameplay']['descriptor']
        metadata = json.loads(descriptor.read_bytes())
        self.expected_native_library = self.native(descriptor.parent / metadata['library'])
        assert file_hash(descriptor.parent / metadata['library']) == self.image_hash, package
        assert self.image_hash in published.values(), published
        self.measurements['bundle'] = dict(game_sha256=file_hash(game), inventory=published,
                                           detached_source=source)
        self.exercise(game)
        assert tree_hashes(relocated) == published and tree_hashes(detached) == source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for flag in ('hostfxr', 'bridge', 'assembly', 'native-descriptor', 'runtime-root'):
        parser.add_argument('--' + flag, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--ibl-replay', action='store_true',
                        help='Enable authored IBL and check its bake generation across one continuous Vulkan replay.')
    parser.add_argument('--require-validation', action='store_true',
                        help='Require Khronos validation in each Vulkan capture owner.')
    args = parser.parse_args()
    managed = (args.hostfxr, args.bridge, args.assembly)
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if (args.native_descriptor and any(managed)) or (not args.native_descriptor and not all(managed)):
        parser.error('Supply the Native AOT descriptor or all three CoreCLR paths.')
    if args.runtime_root and not args.native_descriptor:
        parser.error('Standalone export requires a Native AOT descriptor.')
    if args.gpu is not None and not 0 <= args.gpu <= 4095:
        parser.error('Supply a GPU index from 0 through 4095.')
    if args.require_validation and args.gpu is None:
        parser.error('Vulkan validation requires a GPU capture.')
    if args.ibl_replay and args.gpu is None:
        parser.error('IBL replay requires a GPU capture.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    test = SolarContract(args, args.output)
    evidence = dict(format='poima.solar-lighting-contract.v1', passed=False, standalone=bool(args.runtime_root),
        gpu=args.gpu, lighting_path=args.lighting_path, require_validation=args.require_validation, ibl_replay=args.ibl_replay,
        binary_sha256=file_hash(args.binary),
        harness_sha256=file_hash(Path(__file__)), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        for call in test.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') in ('warm compiled solar', 'cold compiled solar'):
                assert VALIDATION_LAYER in stderr, stderr
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = args.output / 'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
