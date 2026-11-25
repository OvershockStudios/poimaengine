#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check authored volumetric clouds, sky occlusion, and saved wind in a relocated game."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback
import uuid

from atmosphere_capture import AtmosphereContract, CAMERA, EXPOSURE, PROFILE, SIZE, SUN, camera_ray
from scene_capture import pixels
from sky_capture import display, rotation
from solar_lighting_contract import CHECKPOINT, FINAL, VALIDATION_FAILURES, VALIDATION_LAYER, solar_reference
from world_particles_contract import RpcError, component, near_vector, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


FEATURE = 'volumetric_clouds'
DEPENDENCY = 'needs_volumetric_clouds'
CLOUD = dict(base_height_m=2000, thickness_m=1600, coverage=.5625, extinction_per_km=2.25, wind_m_per_s=[2, -1])
CLOUD_PROFILE = PROFILE | dict(clouds=CLOUD)
BOUNDS = dict(base_height_m=(250, 12000), thickness_m=(100, 6000), coverage=(0, 1), extinction_per_km=(0, 32))
NOISE_BYTES = 64**3 * 4
VIEW_BYTES = 192 * 108 * 8
YAW, PITCH, EYE_HEIGHT_M = 0, 20, 1100
SUN_ELEVATION = 35
PANEL = uid(6870)
PANEL_POINT = (160, 64)
PANEL_COLOR = (.375, .0625, .1875)
SUN_POINT = (74, 41)
# Pairwise sky comparisons exclude the known projected bounds of the opaque panel.
LEFT = (24, 4, 132, 104)
RIGHT = (188, 4, 296, 104)
BELOW_LAYER = (0, 170, SIZE[0], SIZE[1])
OCCLUDER_INTERIOR = (154, 59, 167, 70)


def region(image, rectangle):
    left, top, right, bottom = rectangle
    return [pixel for row in image[top:bottom] for pixel in row[left:right]]


def compare_sky(left, right):
    pairs = [(a, b) for bounds in (LEFT, RIGHT)
             for a, b in zip(region(left, bounds), region(right, bounds), strict=True)]
    distance = [max(abs(a-b) for a, b in zip(before, after, strict=True)) for before, after in pairs]
    return dict(pixels=len(distance), changed=sum(delta > 2 for delta in distance),
                max_channel_delta=max(distance), mean_channel_delta=sum(distance) / len(distance))


class CloudContract(AtmosphereContract):
    def cloud_viewpoint(self, offset=(0, 0)):
        return self.viewpoint(YAW, PITCH, EYE_HEIGHT_M, offset=offset)

    def create_witnesses(self, client):
        self.create_world(client, json.loads(self.world.read_bytes()))
        ray = camera_ray(YAW, PITCH, *PANEL_POINT)
        center = [13 * ray[0], EYE_HEIGHT_M + 13 * ray[1], 13 * ray[2]]
        self.edit(client, self.cloud_viewpoint(), self.light_pose(SUN_ELEVATION), self.env(PROFILE),
            dict(op='entity.create', id=PANEL, name='Opaque cloud-occlusion witness'),
            component(PANEL, 'Transform', transform(center) | dict(scale=[1.75, 1.55, .4])),
            component(PANEL, 'MeshRenderer', dict(primitive='box', albedo=[0, 0, 0], visible=True,
                                                 cast_shadows=False, receive_shadows=False)),
            component(PANEL, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=PANEL_COLOR, roughness=.75,
                                                metallic=0, double_sided=False)))

    def check_authoring(self, client, *, has_cloud_shadows):
        catalog = client.call('world.describe', dict(view='component', name='LightingEnvironment'))
        atmosphere = catalog['components']['LightingEnvironment']['properties']['atmosphere']
        schema = atmosphere['properties']['clouds']
        assert 'clouds' not in atmosphere['required'] and schema['additionalProperties'] is False, schema
        assert set(schema['required']) == set(CLOUD), schema
        optional = {'shadow_strength'} if has_cloud_shadows else set()
        assert set(schema['properties']) == set(CLOUD) | optional, schema
        if has_cloud_shadows:
            strength = schema['properties']['shadow_strength']
            assert (strength['type'], strength['minimum'], strength['maximum']) == ('number', 0, 1), strength
        for field, bounds in BOUNDS.items():
            item = schema['properties'][field]
            assert (item['type'], item['minimum'], item['maximum']) == ('number', *bounds), item
        wind = schema['properties']['wind_m_per_s']
        assert wind['type'] == 'array' and wind['minItems'] == wind['maxItems'] == 2, wind
        assert (wind['items']['type'], wind['items']['minimum'], wind['items']['maximum']) == ('number', -100, 100)
        assert DEPENDENCY not in client.call('world.dependencies')

        before = self.world.read_bytes()
        invalid = [('null', None), ('unknown field', CLOUD | dict(texture='not admitted')),
                   ('short wind', CLOUD | dict(wind_m_per_s=[0])),
                   ('long wind', CLOUD | dict(wind_m_per_s=[0, 0, 0])),
                   ('outside wind', CLOUD | dict(wind_m_per_s=[-100.01, 100])),
                   ('boolean wind', CLOUD | dict(wind_m_per_s=[False, 0]))]
        for name, (low, high) in BOUNDS.items():
            invalid.extend(((f'{name} below range', CLOUD | {name: low - .01}),
                            (f'{name} above range', CLOUD | {name: high + .01})))
        invalid.extend((f'missing {name}', {k: v for k, v in CLOUD.items() if k != name}) for name in CLOUD)
        rejected = {}
        for label, clouds in invalid:
            try:
                client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                    ops=[self.viewpoint(YAW, PITCH, EYE_HEIGHT_M + 3), self.env(PROFILE | dict(clouds=clouds))]))
            except RpcError as error:
                assert error.code == -32602, (label, error)
                rejected[label] = error.message
            else:
                raise AssertionError(('Invalid cloud edit accepted', label))
            assert self.world.read_bytes() == before, label
        preview = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
            ops=[self.env(CLOUD_PROFILE)], preview=True))
        assert preview['committed'] is False and self.world.read_bytes() == before, preview
        assert DEPENDENCY not in client.call('world.dependencies')
        self.measurements.update(cloud_schema=schema, rejected_authoring=rejected)

    def check_selection(self, client):
        lighting = client.call('world.lighting', dict(revision=self.revision))
        entity = client.call('entity.get', dict(id=self.environment))['value']
        assert lighting['atmosphere'] == entity['components']['LightingEnvironment']['atmosphere'] == CLOUD_PROFILE
        dependencies = client.call('world.dependencies')
        inspected = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        for record in (dependencies, inspected):
            assert record[DEPENDENCY] is True and record['needs_atmosphere_sky'] is True, record
            assert record['revision'] == self.revision, record
        self.measurements.update(authored_lighting=lighting, dependencies=dependencies, project=inspected)

    @staticmethod
    def check_diagnostics(report, reason, time=0, profile=False):
        diagnostics = report['render_diagnostics']
        if reason is None:
            assert FEATURE not in diagnostics, diagnostics
            return
        active = reason == 'active'
        expected = dict(active=active, reason=reason, noise_bytes=NOISE_BYTES if active else 0,
                        view_bytes=VIEW_BYTES if active else 0, world_milliseconds=time)
        observed = diagnostics[FEATURE]
        assert {key: value for key, value in observed.items() if key != 'gpu'} == expected, observed
        if active and profile and diagnostics['gpu']['available']:
            timing = observed['gpu']
            assert timing['samples'] == diagnostics['completed_submissions'], timing
            assert math.isfinite(timing['mean_ms']) and timing['mean_ms'] >= 0, timing
        elif not profile:
            assert 'gpu' not in observed, observed

    def capture_clouds(self, client, name, reason, *, session=None, tick=0, profile=False, fsr=False):
        path = self.root / (name + '.bmp')
        width, height = SIZE
        frames = 8 if fsr else 2
        params = dict(camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
                      lighting_path=self.args.lighting_path, samples=self.args.samples, profile=profile,
                      reconstruction='fsr3_native' if fsr else 'none', capture_frames=frames)
        x, y = SUN_POINT
        if self.args.samples == 1:
            params['scene_product_probes'] = [dict(x=x, y=y)]
        if session is None:
            report = client.call('world.capture', params | dict(revision=self.revision))
        else:
            report = client.call('runtime.capture', params | dict(session_id=session, tick=tick, structure_revision=0))
        assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, self.args.samples, frames), report
        image = pixels(path)
        capture = dict(sha256=file_hash(path), probe=[x, y], rgb=image[y][x], report=report)
        self.measurements['captures'][name] = capture
        if self.args.samples == 1:
            [probe] = diagnostics['scene_products']['probes']
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            assert not probe['surface_valid'], probe
            capture['hdr'] = probe['resolved_hdr']
            expected = [display(channel * EXPOSURE) for channel in capture['hdr']]
            assert max(abs(a-b) for a, b in zip(capture['rgb'], expected, strict=True)) <= 2, capture
        reconstruction = diagnostics['reconstruction']
        assert reconstruction['active'] is fsr, reconstruction
        if fsr:
            assert reconstruction['mode'] == 'fsr3_native', reconstruction
            assert (reconstruction['render_width'], reconstruction['render_height']) == SIZE, reconstruction
            assert (reconstruction['output_width'], reconstruction['output_height']) == SIZE, reconstruction
            assert reconstruction['history_sequence'] == frames and not reconstruction['history_reset'], reconstruction
            assert all(math.isfinite(jitter) and abs(jitter) <= .5
                       for jitter in reconstruction['jitter_pixels']), reconstruction
        sample = solar_reference(tick) if session and tick else None
        observed = report['lighting'].get('solar_lighting')
        if sample is None:
            assert observed is None, observed
        else:
            assert observed | dict(toward_sun=sample['toward_sun']) == sample, observed
            near_vector(observed['toward_sun'], sample['toward_sun'], tolerance=1e-7)
        self.check_diagnostics(report, reason, sample['world_milliseconds'] if sample else 0, profile=profile)
        return capture, image

    def capture_authoring_controls(self, client):
        clear, clear_image = self.capture_clouds(client, 'omitted-clouds', None)
        if self.args.baseline_binary:
            self.compare_legacy(clear)
        if self.args.fsr:
            _, clear_fsr = self.capture_clouds(client, 'omitted-clouds-fsr', None, fsr=True)
        self.edit(client, self.env(CLOUD_PROFILE))
        self.check_selection(client)
        active, active_image = self.capture_clouds(client, 'active-day', 'active', profile=True)
        expected_panel = [display(channel * EXPOSURE) for channel in PANEL_COLOR]
        panel = region(active_image, OCCLUDER_INTERIOR)
        assert panel == region(clear_image, OCCLUDER_INTERIOR)
        assert all(max(abs(a-b) for a, b in zip(pixel, expected_panel, strict=True)) <= 2 for pixel in panel), panel
        assert region(active_image, BELOW_LAYER) == region(clear_image, BELOW_LAYER)
        cloud_presence = compare_sky(clear_image, active_image)
        assert cloud_presence['changed'] > cloud_presence['pixels'] // 12, cloud_presence
        assert cloud_presence['max_channel_delta'] > 12, cloud_presence
        self.measurements.update(day_cloud_delta=cloud_presence, opaque_interior_pixels=len(panel))
        if self.args.fsr:
            _, cloud_fsr = self.capture_clouds(client, 'active-clouds-fsr', 'active', profile=True, fsr=True)
            self.compare_fsr(clear_fsr, cloud_fsr)
        for field in ('coverage', 'extinction_per_km'):
            self.edit(client, self.env(PROFILE | dict(clouds=CLOUD | {field: 0})))
            reason = 'zero_coverage' if field == 'coverage' else 'zero_extinction'
            disabled, _ = self.capture_clouds(client, reason, reason)
            assert clear['sha256'] == disabled['sha256'], (field, clear, disabled)
        self.edit(client, component(self.sun, 'Light', SUN | dict(enabled=False)), self.env(CLOUD_PROFILE))
        fallback, _ = self.capture_clouds(client, 'clouds-with-disabled-sun', 'atmosphere_inactive')
        self.edit(client, self.env(PROFILE))
        disabled, _ = self.capture_clouds(client, 'clear-with-disabled-sun', None)
        assert fallback['sha256'] == disabled['sha256'], (fallback, disabled)

        self.edit(client, self.env(CLOUD_PROFILE), component(self.sun, 'Light', SUN),
                  self.cloud_viewpoint(offset=(3200, 1200)))
        _, moved = self.capture_clouds(client, 'translated-clouds', 'active')
        self.edit(client, self.env(PROFILE))
        _, translated_clear = self.capture_clouds(client, 'translated-clear-sky', None)
        assert compare_sky(clear_image, translated_clear)['max_channel_delta'] == 0
        parallax = compare_sky(active_image, moved)
        assert parallax['changed'] > parallax['pixels'] // 14, parallax
        self.measurements['camera_translation_delta'] = parallax

        self.edit(client, self.env(CLOUD_PROFILE), self.cloud_viewpoint(offset=(64_000, -64_000)))
        _, tiled = self.capture_clouds(client, 'repeated-world-tile', 'active')
        assert compare_sky(active_image, tiled)['max_channel_delta'] == 0
        self.edit(client, self.cloud_viewpoint(), self.env(CLOUD_PROFILE | dict(aerial_perspective_m=8000)))
        combined, combined_image = self.capture_clouds(client, 'clouds-and-aerial', 'active')
        assert combined['report']['render_diagnostics']['aerial_perspective']['active'] is (self.args.samples == 1)
        assert compare_sky(active_image, combined_image)['max_channel_delta'] == 0
        self.edit(client, self.env(CLOUD_PROFILE), self.light_pose(-90))
        _, midnight = self.capture_clouds(client, 'active-midnight', 'active')
        self.edit(client, self.env(PROFILE))
        _, clear_night = self.capture_clouds(client, 'clear-midnight', None)
        night_cloud_delta = compare_sky(clear_night, midnight)
        assert night_cloud_delta['mean_channel_delta'] < cloud_presence['mean_channel_delta'], night_cloud_delta
        self.measurements['midnight_cloud_delta'] = night_cloud_delta
        self.edit(client, self.env(CLOUD_PROFILE), self.light_pose(SUN_ELEVATION))
        repeated, _ = self.capture_clouds(client, 'active-day-repeated', 'active')
        assert repeated['sha256'] == active['sha256'], (repeated, active)

    def compare_fsr(self, clear, cloudy):
        presence = compare_sky(clear, cloudy)
        assert presence['changed'] > presence['pixels'] // 12, presence
        assert presence['max_channel_delta'] > 12, presence
        panel_delta = max(abs(a-b) for before, after in zip(region(clear, OCCLUDER_INTERIOR),
                          region(cloudy, OCCLUDER_INTERIOR), strict=True) for a, b in zip(before, after, strict=True))
        assert panel_delta <= 2, panel_delta
        self.measurements['fsr_native'] = dict(cloud_delta=presence, foreground_max_channel_delta=panel_delta)

    def frozen_cloud_selection(self):
        saves = self.root / 'Frozen cloud saves'
        saves.mkdir()
        with self.owner('freeze cloud selection') as (client, warm):
            session = self.initialize(client, self.revision, uid(6871), saves, None)
            self.step(client, session, 0, 6872, ticks=2, expected_structure_revision=0)
            selected = self.light_at(client, session, 2)
            assert selected['atmosphere'] == CLOUD_PROFILE, selected
            still = CLOUD | dict(wind_m_per_s=[0, 0])
            self.edit(client, self.env(PROFILE | dict(clouds=still)))
            assert client.call('world.lighting', dict(revision=self.revision))['atmosphere']['clouds'] == still
            assert self.light_at(client, session, 2) == selected
            archive = self.save_archive(client, session, 2, saves, 'selected-cloud')
            assert file_hash(self.world) != self.measurements['source_revision_sha256']
            frozen = json.loads(archive)['document']['entities'][self.environment]['components']['LightingEnvironment']
            assert frozen['atmosphere'] == CLOUD_PROFILE, frozen
        with self.owner('restore cloud selection') as (client, cold):
            client.call('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(saves)))
            session = uid(6873)
            loaded = client.call('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                slot='selected-cloud', expected_generation=1, revision=self.revision, expected_session_id=None,
                expected_tick=None, expected_gameplay_revision=None, expected_structure_revision=None,
                new_session_id=session))
            assert loaded['source_stale'] and loaded['selected_generation'] == 1 and not loaded['recovered'], loaded
            assert self.light_at(client, session, 2) == selected
            assert self.save_archive(client, session, 2, saves, 'restored-cloud') == archive
            self.edit(client, self.env(CLOUD_PROFILE))
        assert cold['pid'] != warm['pid'], (cold, warm)
        self.measurements['frozen_selection_restored'] = selected

    def static_wind_controls(self):
        # Tick two of the pinned solar module has unit sky gain and white Sun.
        # Author its Sun direction directly, leave the clock absent and change
        # only the observer's X/Z coordinates according to the expected drift.
        sample = solar_reference(CHECKPOINT)
        assert (sample['sky_gain'], sample['sun_tint']) == (1, [1, 1, 1]), sample
        x, y, z = sample['toward_sun']
        pitch = -math.degrees(math.asin(y))
        yaw = math.degrees(math.atan2(x, z))
        solar_pose = component(self.sun, 'Transform', transform([0, 0, 0], rotation(yaw=yaw, pitch=pitch)))
        seconds = sample['world_milliseconds'] / 1000
        shift = tuple(-axis*seconds for axis in CLOUD['wind_m_per_s'])
        with self.owner('author isolated wind controls') as (client, _):
            self.edit(client, solar_pose)
            stationary, still_image = self.capture_clouds(client, 'clockless-no-translation', 'active')
            self.edit(client, self.cloud_viewpoint(offset=shift))
            shifted, shifted_image = self.capture_clouds(client, 'clockless-expected-translation', 'active')
            for capture in (stationary, shifted):
                near_vector(capture['report']['lighting']['sky_sun']['direction'], sample['toward_sun'], tolerance=1e-7)
            self.edit(client, self.cloud_viewpoint(), self.light_pose(SUN_ELEVATION))
        separation = compare_sky(still_image, shifted_image)
        assert separation['changed'] > separation['pixels'] // 12, separation
        self.measurements['isolated_wind'] = dict(control_translation_m=shift, control_separation=separation)
        return still_image, shifted_image

    def check_export_rejection(self):
        descriptor = json.loads((self.args.runtime_root / 'runtime.json').read_bytes())
        assert descriptor['features'].pop(FEATURE) is True, descriptor
        runtime = self.root / 'Runtime descriptor without clouds'
        runtime.mkdir()
        (runtime / 'runtime.json').write_text(json.dumps(descriptor) + '\n', encoding='utf-8')
        output = self.root / 'Refused cloud export'
        try:
            self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                     '--runtime', self.native(runtime))
        except AssertionError:
            call = self.calls[-1]
            assert call['exit'] != 0, call
            reply = json.loads(call['stdout'])
            assert reply['status'] == 'error', reply
            assert reply['diagnostics'][0]['message'] == 'Volumetric clouds require a compatible Vulkan runtime.', reply
            assert not output.exists()
            self.measurements['declined_runtime'] = reply
        else:
            raise AssertionError('Runtime descriptor without clouds was admitted for export.')

    def compiled_state(self, client, session, tick):
        lighting = self.light_at(client, session, tick)
        assert lighting['atmosphere'] == CLOUD_PROFILE, lighting
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = info['module']
        assert module['backend'] == 'native_aot' and module['assembly_sha256'] == self.image_hash, module
        assert module['values'] == dict(Ticks=tick, FailAt='-1'), module
        return dict(lighting=lighting, gameplay=module['values'], gameplay_revision=info['revision'])

    def relocated_clouds(self):
        if self.args.native_descriptor:
            still_image, shifted_image = self.static_wind_controls()
            self.configure_artifact()
        self.check_export_rejection()
        initial = self.root / 'Original cloud export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(initial),
                                    '--runtime', self.native(self.args.runtime_root))))
        relocated = self.root / 'Relocated cloud game'
        detached = self.root / 'Unavailable cloud source'
        initial.rename(relocated)
        self.project.rename(detached)
        assert not self.project.exists() and not initial.exists()
        game = relocated / 'game.json'
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        assert inspected[DEPENDENCY] is True and inspected['needs_atmosphere_sky'] is True, inspected
        assert json.loads((relocated / 'runtime/runtime.json').read_bytes())['features'][FEATURE] is True
        inventory, source = tree_hashes(relocated), tree_hashes(detached)
        saves = self.root / 'Relocated cloud saves'
        saves.mkdir()
        with self.owner('relocated clouds', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, self.revision, uid(6890), saves, game)
            assert self.light_at(client, session, 0)['atmosphere'] == CLOUD_PROFILE
            final, _ = self.capture_clouds(client, 'relocated-authored', 'active', session=session)
            assert final['sha256'] == self.measurements['captures']['active-day']['sha256']
            if self.args.native_descriptor:
                checkpoint, archive, final = self.advance_compiled_clouds(client, session, saves, still_image, shifted_image)
        if self.args.native_descriptor:
            with self.owner('restore relocated cloud clock', game=game, saves=saves) as (client, cold):
                previous = self.initialize(client, self.revision, uid(6891), saves, game)
                initial_state = self.compiled_state(client, previous, 0)
                restored, _ = self.restore(client, previous, initial_state, CHECKPOINT, 6892)
                state = self.compiled_state(client, restored, CHECKPOINT)
                assert state == checkpoint, state
                assert self.archive(client, saves, restored, state, 'cold-restored') == archive
                cold_frame, _ = self.capture_clouds(client, 'cold-wind-checkpoint', 'active',
                                                    session=restored, tick=CHECKPOINT)
                assert cold_frame['sha256'] == self.measurements['captures']['relocated-solar-2']['sha256']
                self.step(client, restored, CHECKPOINT, 6894, ticks=FINAL-CHECKPOINT, expected_structure_revision=0)
                continued, _ = self.capture_clouds(client, 'cold-wind-continued', 'active', session=restored, tick=FINAL)
                assert continued['sha256'] == final['sha256'], continued
            assert cold['pid'] != warm['pid'], (cold, warm)
        with self.owner('continuous cloud replay', game=game, saves=saves) as (client, _):
            self.verify_replay(client, client.call('runtime.status')['session_id'], final)
            clock = solar_reference(FINAL)['world_milliseconds'] if self.args.native_descriptor else 0
            self.check_diagnostics(self.measurements['replay']['report'], 'active', clock)
        assert tree_hashes(relocated) == inventory and tree_hashes(detached) == source
        self.measurements['bundle'] = dict(game_sha256=file_hash(game), inventory=inventory, detached_source=source)

    def advance_compiled_clouds(self, client, session, saves, still_image, shifted_image):
        self.measurements['archives'] = {}
        self.step(client, session, 0, 6900, ticks=CHECKPOINT, expected_structure_revision=0)
        checkpoint = self.compiled_state(client, session, CHECKPOINT)
        archive = self.archive(client, saves, session, checkpoint, f'warm-{CHECKPOINT}')
        _, actual = self.capture_clouds(client, 'relocated-solar-2', 'active', session=session, tick=CHECKPOINT)
        observed, stationary = compare_sky(actual, shifted_image), compare_sky(actual, still_image)
        assert observed['max_channel_delta'] <= 2, observed
        assert stationary['changed'] > stationary['pixels'] // 12, stationary
        self.measurements['isolated_wind'].update(matches_translated_control=observed, relative_to_stationary=stationary)
        self.step(client, session, CHECKPOINT, 6901, ticks=FINAL-CHECKPOINT, expected_structure_revision=0)
        final, _ = self.capture_clouds(client, 'relocated-solar-5', 'active', session=session, tick=FINAL)
        return checkpoint, archive, final

    def run(self):
        features = success(json.loads(self.cli('capabilities')))['features']
        assert features['simulation'] and features[FEATURE] is features['atmosphere_sky'], features
        if self.args.gpu is not None:
            assert features[FEATURE], features
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Volumetric cloud witness')))
        self.measurements['captures'] = {}
        with self.owner('author volumetric clouds') as (client, _):
            self.create_witnesses(client)
            self.check_authoring(client, has_cloud_shadows='cloud_shadows' in features)
            if self.args.gpu is None:
                self.edit(client, self.env(CLOUD_PROFILE))
                self.check_selection(client)
            else:
                self.capture_authoring_controls(client)
        self.measurements['source_revision_sha256'] = file_hash(self.world)
        self.frozen_cloud_selection()
        if self.args.runtime_root:
            self.relocated_clouds()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--runtime-root', type=Path, help='Installed Vulkan runtime for a relocated export.')
    parser.add_argument('--native-descriptor', type=Path, help='Pinned compiled solar fixture for wind and save replay.')
    parser.add_argument('--baseline-binary', type=Path, help='Retained runtime for omitted-clouds image parity.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    parser.add_argument('--fsr', action='store_true', help='Also check one-sample FSR Native AA with clouds.')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is not None and not 0 <= args.gpu <= 4095):
        parser.error('Run with assertions enabled and a GPU index from 0 through 4095.')
    if args.samples == 4 and args.lighting_path == 'deferred':
        parser.error('Deferred capture uses one sample.')
    if args.fsr and (args.gpu is None or args.samples != 1):
        parser.error('FSR requires a one-sample GPU capture.')
    if args.gpu is None and (args.runtime_root or args.baseline_binary or args.require_validation):
        parser.error('Standalone export, retained parity and Vulkan validation require a capture.')
    if args.native_descriptor and not args.runtime_root:
        parser.error('The compiled solar fixture requires a relocated game.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='clouds-', dir=args.output.resolve()))
    fixture = CloudContract(args, root)
    evidence = dict(format='poima.volumetric-cloud-capture.v1', passed=False,
                    binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                    gpu=args.gpu, lighting_path=args.lighting_path, samples=args.samples, fsr=args.fsr,
                    require_validation=args.require_validation, calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        gpu_calls = {'author volumetric clouds', 'author isolated wind controls', 'relocated clouds',
                     'restore relocated cloud clock', 'continuous cloud replay'}
        for call in fixture.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert not any(marker in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') in gpu_calls:
                assert VALIDATION_LAYER in stderr, stderr
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        result = root / 'evidence.json'
        result.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(result)


if __name__ == '__main__':
    main()
