#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify that a projected cloud layer attenuates only its linked direct sun."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import tempfile
import traceback
import uuid

from atmosphere_capture import AtmosphereContract, CAMERA, EXPOSURE, PROFILE, SIZE, SUN, camera_ray
from scene_capture import pixels
from sky_capture import display, rotation
from solar_lighting_contract import CHECKPOINT, VALIDATION_FAILURES, VALIDATION_LAYER, solar_reference
from volumetric_cloud_capture import CLOUD, region
from world_particles_contract import RpcError, component, near_vector, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


FEATURE = 'cloud_shadows'
DEPENDENCY = 'needs_cloud_shadows'
TEXTURE_BYTES = 128 * 128 * 8
LENS = dict(vertical_fov=48, near=100, far=34000)
PITCH = -16
EYE = (2900, 1750, 7350)
FLOOR = uid(6911)
NO_SHADOW = uid(6912)
FILL = uid(6913)
AMBIENT = [.125] * 3
GROUND_COLOR = [.5, .375, .25]
SUN_LIGHT = SUN | dict(color=[1, 0, 0], intensity=4, shadow=dict(enabled=False))
FILL_LIGHT = SUN_LIGHT | dict(color=[0, 1, 0], intensity=3)
LAYER = CLOUD | dict(coverage=.75, extinction_per_km=3.5)
NOON = solar_reference(CHECKPOINT)['toward_sun']
SKY_BAND = (16, 1, SIZE[0] - 16, 14)
GROUND_BAND = (68, 105, SIZE[0] - 32, SIZE[1] - 8)
UNSHADOWED_PIXEL = (36, 152)
UNSHADOWED_PATCH = (31, 148, 42, 157)
GROUND_PROBES = [(x, y) for y in (111, 137, 164) for x in (81, 130, 202, 264)]


def lighting_rotation(toward_sun):
    x, y, z = toward_sun
    return rotation(yaw=math.degrees(math.atan2(x, z)), pitch=-math.degrees(math.asin(y)))


def profile(strength=None):
    cloud = LAYER if strength is None else LAYER | dict(shadow_strength=strength)
    return PROFILE | dict(clouds=cloud)


def red_difference(left, right, bounds):
    return [a[0] - b[0] for a, b in zip(region(left, bounds), region(right, bounds), strict=True)]


def decoded(linear_srgb_code):
    srgb = linear_srgb_code / 255
    encoded = srgb / 12.92 if srgb <= .04045 else ((srgb + .055) / 1.055) ** 2.4
    assert encoded < 1, 'The witness must preserve highlight headroom.'
    return encoded / (1 - encoded) / EXPOSURE


class CloudShadowContract(AtmosphereContract):
    def environment_op(self, lighting_profile):
        operation = self.env(lighting_profile)
        operation['value']['ambient'] = AMBIENT
        return operation

    def receiver_transforms(self, dx=0, dy=0, dz=0):
        ray = camera_ray(0, PITCH, *UNSHADOWED_PIXEL)
        distance = (12 - EYE[1]) / ray[1]
        patch_x = EYE[0] + distance * ray[0]
        patch_z = EYE[2] + distance * ray[2]
        return [self.viewpoint(0, PITCH, EYE[1] + dy, (EYE[0] + dx, EYE[2] + dz)),
                component(FLOOR, 'Transform', transform([dx, dy - 50, dz - 8000]) |
                          dict(scale=[50000, 100, 50000])),
                component(NO_SHADOW, 'Transform', transform([patch_x + dx, dy + 6, patch_z + dz]) |
                          dict(scale=[340, 12, 440]))]

    def create_witnesses(self, client):
        self.create_world(client, json.loads(self.world.read_bytes()))
        material = dict(base_color=GROUND_COLOR, emissive=[0, 0, .25], roughness=.875,
                        metallic=0, double_sided=False)
        ops = [dict(op='entity.create', id=FILL, name='Independent green directional light'),
               component(FILL, 'Transform', transform([0, 0, 0], lighting_rotation(NOON))),
               component(FILL, 'Light', FILL_LIGHT),
               component(self.sun, 'Transform', transform([0, 0, 0], lighting_rotation(NOON))),
               component(self.sun, 'Light', SUN_LIGHT), component(CAMERA, 'Camera', LENS),
               self.environment_op(profile())]
        for entity, name, receive in ((FLOOR, 'Cloud shadow receiver', True),
                                      (NO_SHADOW, 'Unshadowed control surface', False)):
            ops.extend((dict(op='entity.create', id=entity, name=name),
                        component(entity, 'MeshRenderer', dict(primitive='box', albedo=GROUND_COLOR, visible=True,
                                                               cast_shadows=False, receive_shadows=receive)),
                        component(entity, 'PbrMaterial', material)))
        self.edit(client, *ops, *self.receiver_transforms())

    def check_authoring(self, client):
        catalog = client.call('world.describe', dict(view='component', name='LightingEnvironment'))
        atmosphere = catalog['components']['LightingEnvironment']['properties']['atmosphere']
        schema = atmosphere['properties']['clouds']
        bound = schema['properties']['shadow_strength']
        assert 'shadow_strength' not in schema['required'], schema
        assert (bound['type'], bound['minimum'], bound['maximum']) == ('number', 0, 1), bound
        assert DEPENDENCY not in client.call('world.dependencies')
        before = self.world.read_bytes()
        invalid = [('negative', -.001), ('above one', 1.001), ('Boolean', False), ('string', '.5'), ('null', None)]
        rejected = {}
        for label, value in invalid:
            bad = PROFILE | dict(clouds=LAYER | dict(shadow_strength=value))
            try:
                client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                    ops=[self.viewpoint(0, PITCH, EYE[1] + 4), self.environment_op(bad)]))
            except RpcError as error:
                assert error.code == -32602, (label, error)
                rejected[label] = error.message
            else:
                raise AssertionError(('Invalid cloud shadow strength accepted', label))
            assert self.world.read_bytes() == before, label
        preview = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
            ops=[self.environment_op(profile(1))], preview=True))
        assert preview['committed'] is False and self.world.read_bytes() == before, preview
        assert DEPENDENCY not in client.call('world.dependencies')
        self.measurements['rejected_authoring'] = rejected
        self.measurements['authoring_schema'] = bound

    def check_selection(self, client, strength):
        selected = client.call('world.lighting', dict(revision=self.revision))
        assert selected['atmosphere'] == profile(strength), selected
        dependencies = client.call('world.dependencies')
        inspection = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        for record in (dependencies, inspection):
            assert record.get(DEPENDENCY) is True and record.get('needs_volumetric_clouds') is True, record
            assert record['revision'] == self.revision, record
        self.measurements['selection'] = selected

    def diagnostics(self, report, reason, generation=1, *, profiled=False):
        diagnostics = report['render_diagnostics']
        if reason is None:
            assert FEATURE not in diagnostics, diagnostics
            return
        active = reason == 'active'
        observed = diagnostics[FEATURE]
        expected = dict(active=active, reason=reason, texture_bytes=TEXTURE_BYTES if active else 0,
                        generation=generation if active else 0, light=self.sun if active else '')
        assert {key: value for key, value in observed.items() if key != 'gpu'} == expected, observed
        if 'gpu' in observed:
            assert active and profiled and diagnostics['gpu']['available'], observed
            gpu = observed['gpu']
            assert gpu['samples'] == 1 and math.isfinite(gpu['mean_ms']) and gpu['mean_ms'] >= 0, gpu

    def capture_shadows(self, client, name, reason, *, session=None, tick=0, profiled=False, fsr=False):
        path = self.root / (name + '.bmp')
        width, height = SIZE
        frames = 8 if fsr else 2
        params = dict(camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
                      lighting_path=self.args.lighting_path, samples=self.args.samples,
                      profile=profiled, reconstruction='fsr3_native' if fsr else 'none', capture_frames=frames)
        sites = [*GROUND_PROBES, UNSHADOWED_PIXEL]
        if self.args.samples == 1:
            params['scene_product_probes'] = [dict(x=x, y=y) for x, y in sites]
        if session is None:
            report = client.call('world.capture', params | dict(revision=self.revision))
        else:
            report = client.call('runtime.capture', params | dict(session_id=session, tick=tick, structure_revision=0))
        assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, self.args.samples, frames), report
        assert report['render_diagnostics']['lighting_path'] == self.args.lighting_path, report
        reconstruction = report['render_diagnostics']['reconstruction']
        assert reconstruction['active'] is fsr, reconstruction
        if fsr:
            assert reconstruction['mode'] == 'fsr3_native', reconstruction
            assert (reconstruction['render_width'], reconstruction['render_height']) == SIZE, reconstruction
            assert (reconstruction['output_width'], reconstruction['output_height']) == SIZE, reconstruction
            assert reconstruction['history_sequence'] == frames and not reconstruction['history_reset'], reconstruction
        self.diagnostics(report, reason, profiled=profiled)
        image = pixels(path)
        capture = dict(sha256=file_hash(path), report=report, sites={})
        for x, y in sites:
            capture['sites'][f'{x},{y}'] = dict(rgb=image[y][x])
        if self.args.samples == 1:
            samples = report['render_diagnostics']['scene_products']['probes']
            for (x, y), probe in zip(sites, samples, strict=True):
                assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
                assert probe['surface_valid'], probe
                if not fsr:
                    assert probe['raw_hdr'] == probe['resolved_hdr'], probe
                hdr = probe['resolved_hdr']
                expected = [display(channel * EXPOSURE) for channel in hdr]
                assert max(abs(a - b) for a, b in zip(expected, image[y][x], strict=True)) <= 2, probe
                capture['sites'][f'{x},{y}']['hdr'] = hdr
        self.measurements['captures'][name] = capture
        return capture, image

    def check_receiver_optics(self, clear, half, full, sunless):
        c, a, b, u = (capture['sites'] for capture in (clear, half, full, sunless))
        key = 'hdr' if self.args.samples == 1 else 'rgb'
        interpolation_errors = []
        for site in c:
            clear_rgb, half_rgb, full_rgb, sunless_rgb = (observations[site][key] for observations in (c, a, b, u))
            assert clear_rgb[1:] == half_rgb[1:] == full_rgb[1:] == sunless_rgb[1:], site
            margin = .002 if key == 'hdr' else 1
            assert clear_rgb[0] > sunless_rgb[0] + margin, site
            assert sunless_rgb[0] - margin <= full_rgb[0] <= half_rgb[0] + margin <= clear_rgb[0] + 2 * margin, site
            if key == 'hdr':
                expected = (clear_rgb[0] + full_rgb[0]) / 2
                measured = half_rgb[0]
                assert abs(expected - measured) <= .002, (site, expected, measured)
            else:
                expected = display(EXPOSURE * (decoded(clear_rgb[0]) + decoded(full_rgb[0])) / 2)
                measured = half_rgb[0]
                assert abs(expected - measured) <= 2, (site, expected, measured)
            interpolation_errors.append(abs(expected - measured))
        self.measurements['receiver_optics'] = dict(comparison=key, checked_sites=len(c),
                                                    max_half_strength_error=max(interpolation_errors))

    def check_fsr(self, clear, shaded):
        red = red_difference(clear, shaded, GROUND_BAND)
        visibly_dimmed = sum(value > 8 for value in red)
        assert min(red) >= -2 and visibly_dimmed >= len(red) // 8, (min(red), max(red))
        unaffected = max(abs(a - b) for clear_site, shaded_site in zip(
            region(clear, GROUND_BAND), region(shaded, GROUND_BAND), strict=True)
            for a, b in zip(clear_site[1:], shaded_site[1:], strict=True))
        assert unaffected <= 2, unaffected
        self.measurements['fsr_native'] = dict(ground_pixels=len(red), visibly_dimmed=visibly_dimmed,
                                                max_red_delta=max(red), max_green_blue_delta=unaffected)

    def check_geometry(self, client):
        clear, clear_image = self.capture_shadows(client, 'omitted', None)
        if self.args.baseline_binary:
            self.compare_legacy(clear)
        if self.args.fsr:
            _, clear_fsr = self.capture_shadows(client, 'omitted-fsr', None, fsr=True)
        self.edit(client, self.environment_op(profile(0)))
        self.check_selection(client, 0)
        zero, _ = self.capture_shadows(client, 'zero-strength', 'zero_strength')
        assert zero['sha256'] == clear['sha256']
        self.edit(client, self.environment_op(profile(.5)))
        half, _ = self.capture_shadows(client, 'half-strength', 'active')
        self.edit(client, self.environment_op(profile(1)))
        full, full_image = self.capture_shadows(client, 'full-strength', 'active', profiled=True)
        assert region(full_image, SKY_BAND) == region(clear_image, SKY_BAND)
        assert region(full_image, UNSHADOWED_PATCH) == region(clear_image, UNSHADOWED_PATCH)
        red = red_difference(clear_image, full_image, GROUND_BAND)
        assert min(red) >= -1 and sum(value > 8 for value in red) >= len(red) // 8, (min(red), max(red))
        for a, b in zip(region(clear_image, GROUND_BAND), region(full_image, GROUND_BAND), strict=True):
            assert a[1:] == b[1:], (a, b)
        self.measurements['red_only_attenuation'] = dict(ground_pixels=len(red),
            visibly_dimmed=sum(value > 8 for value in red), max_display_delta=max(red),
            unchanged_sky_pixels=len(region(clear_image, SKY_BAND)),
            unchanged_unshadowed_pixels=len(region(clear_image, UNSHADOWED_PATCH)))
        if self.args.fsr:
            _, shaded_fsr = self.capture_shadows(client, 'full-strength-fsr', 'active', fsr=True)
            self.check_fsr(clear_fsr, shaded_fsr)
        self.edit(client, component(self.sun, 'Light', SUN_LIGHT | dict(intensity=0)))
        sunless, _ = self.capture_shadows(client, 'unlit-sun', 'sun_unlit')
        self.check_receiver_optics(clear, half, full, sunless)
        self.edit(client, component(self.sun, 'Light', SUN_LIGHT), self.light_pose(2))
        low, _ = self.capture_shadows(client, 'low-sun', 'low_sun')
        self.edit(client, self.environment_op(profile()))
        low_clear, _ = self.capture_shadows(client, 'low-sun-omitted', None)
        assert low_clear['sha256'] == low['sha256']

        self.edit(client, component(self.sun, 'Transform', transform([0, 0, 0], lighting_rotation(NOON))),
                  self.environment_op(profile(1)), *self.receiver_transforms(dx=64000, dz=-64000))
        _, repeated = self.capture_shadows(client, 'repeated-shadows-tile', 'active')
        delta = red_difference(full_image, repeated, GROUND_BAND)
        assert max(map(abs, delta)) <= 1, (min(delta), max(delta))
        self.measurements['repeat_tile_max_display_delta'] = max(map(abs, delta))

        # Translate the receiver along the solar ray to keep the same projected
        # base coordinates, so only the overlying column changes.
        center_height = LAYER['base_height_m'] + LAYER['thickness_m'] * .5
        self.edit(client, *self.receiver_transforms(dy=center_height, dz=center_height * NOON[2] / NOON[1]))
        _, middle = self.capture_shadows(client, 'inside-layer', 'active')
        light_return = red_difference(middle, full_image, GROUND_BAND)
        assert min(light_return) >= -1 and sum(value > 3 for value in light_return) >= len(light_return) // 12
        self.measurements['inside_layer'] = dict(max_restored_display_red=max(light_return))

        high = LAYER['base_height_m'] + LAYER['thickness_m'] + 100
        self.edit(client, *self.receiver_transforms(dy=high))
        above, _ = self.capture_shadows(client, 'above-layer', 'active')
        self.edit(client, self.environment_op(profile()))
        above_clear, _ = self.capture_shadows(client, 'above-layer-omitted', None)
        assert above['sha256'] == above_clear['sha256']
        self.edit(client, self.environment_op(profile(1)), *self.receiver_transforms())

    def check_export_rejection(self):
        descriptor = json.loads((self.args.runtime_root / 'runtime.json').read_bytes())
        assert descriptor['features'].pop(FEATURE) is True, descriptor
        incompatible = self.root / 'Runtime without projected cloud shadows'
        incompatible.mkdir()
        (incompatible / 'runtime.json').write_text(json.dumps(descriptor) + '\n', encoding='utf-8')
        rejected = self.root / 'Refused projected cloud shadow export'
        try:
            self.cli('project', 'build', self.native(self.manifest), '--output', self.native(rejected),
                     '--runtime', self.native(incompatible))
        except AssertionError:
            call = self.calls[-1]
            assert call['exit'] != 0, call
            reply = json.loads(call['stdout'])
            assert reply['status'] == 'error', reply
            message = reply['diagnostics'][0]['message'].lower()
            assert 'cloud shadow' in message and 'compatible vulkan runtime' in message, reply
            assert not rejected.exists()
            self.measurements['rejected_runtime'] = reply
        else:
            raise AssertionError('Runtime descriptor without cloud shadows was admitted for export.')

    def compiled_state(self, client, session, tick):
        lighting = self.light_at(client, session, tick)
        assert lighting['atmosphere'] == profile(1), lighting
        sample = solar_reference(tick)
        if sample:
            actual = lighting['solar_lighting']
            near_vector(actual['toward_sun'], sample['toward_sun'], tolerance=1e-7)
            assert actual | dict(toward_sun=sample['toward_sun']) == sample, actual
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = info['module']
        assert module['backend'] == 'native_aot' and module['assembly_sha256'] == self.image_hash, module
        assert module['values'] == dict(Ticks=tick, FailAt='-1'), module
        return dict(lighting=lighting, gameplay=module['values'], gameplay_revision=info['revision'])

    def replay_noon(self, game, saves, expected):
        path = self.root / 'continuous-noon.bmp'
        width, height = SIZE
        with self.owner('replay cached cloud shadow map', game=game, saves=saves) as (client, _):
            session = client.call('runtime.status')['session_id']
            report = client.call('runtime.play', dict(session_id=session, expected_tick=0,
                expected_structure_revision=0, request_id=uid(6920), controller=self.controller,
                camera=CAMERA, mode='replay', sequence=[dict(ticks=3)], path=self.native(path),
                width=width, height=height, samples=self.args.samples, gpu=self.args.gpu,
                lighting_path=self.args.lighting_path, reconstruction='none', audio=False))
            assert report['capture_written'] and report['hardware'] and report['success'] and not report['nvrhi_errors'], report
            assert (report['tick'], report['stop_reason'], report['frames_presented']) == (3, 'replay_complete', 4), report
            assert report['initial_session_id'] == report['current_session_id'] == session, report
            # Replay advances before drawing. Dawn and committed noon need one
            # bake each. Tick three and the final capture reuse the noon map.
            self.diagnostics(report, 'active', generation=2)
            assert file_hash(path) == expected['sha256'], (file_hash(path), expected['sha256'])
            self.measurements['cached_map_replay'] = dict(sha256=file_hash(path), report=report)

    def relocate(self):
        if self.args.native_descriptor:
            seconds = solar_reference(CHECKPOINT)['world_milliseconds'] / 1000
            shift_x, shift_z = (-seconds * axis for axis in LAYER['wind_m_per_s'])
            with self.owner('author wind displacement control') as (client, _):
                self.edit(client, *self.receiver_transforms(dx=shift_x, dz=shift_z))
                _, expected_wind = self.capture_shadows(client, 'expected-wind-displacement', 'active')
                self.edit(client, *self.receiver_transforms())
            self.configure_artifact()
        self.check_export_rejection()
        output = self.root / 'Original projected cloud shadow export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                    '--runtime', self.native(self.args.runtime_root))))
        relocated, detached = self.root / 'Relocated game', self.root / 'Unavailable source'
        output.rename(relocated)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        game = relocated / 'game.json'
        inspection = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        assert inspection[DEPENDENCY] is True and inspection['needs_volumetric_clouds'] is True, inspection
        assert json.loads((relocated / 'runtime/runtime.json').read_bytes())['features'][FEATURE] is True
        inventory, source = tree_hashes(relocated), tree_hashes(detached)
        saves = self.root / 'Cloud shadow saves'
        saves.mkdir()
        with self.owner('play relocated cloud shadows', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, self.revision, uid(6915), saves, game)
            first, still = self.capture_shadows(client, 'relocated-authored', 'active', session=session)
            assert first['sha256'] == self.measurements['captures']['full-strength']['sha256']
            if self.args.native_descriptor:
                self.measurements['archives'] = {}
                self.step(client, session, 0, 6916, ticks=CHECKPOINT, expected_structure_revision=0)
                checkpoint = self.compiled_state(client, session, CHECKPOINT)
                saved = self.archive(client, saves, session, checkpoint, f'warm-{CHECKPOINT}')
                noon, wind = self.capture_shadows(client, 'relocated-wind', 'active', session=session, tick=CHECKPOINT)
                discrepancy = red_difference(wind, expected_wind, GROUND_BAND)
                displacement = red_difference(wind, still, GROUND_BAND)
                assert max(map(abs, discrepancy)) <= 2, (min(discrepancy), max(discrepancy))
                moving = sum(abs(value) > 6 for value in displacement)
                assert moving >= len(displacement) // 12, moving
                self.measurements['wind_displacement'] = dict(translated_by_m=[shift_x, shift_z],
                    changing_pixels=moving, maximum_control_difference=max(map(abs, discrepancy)))
                self.step(client, session, CHECKPOINT, 6917, expected_structure_revision=0)
                repeated, _ = self.capture_shadows(client, 'relocated-wind-repeated', 'active', session=session, tick=3)
                assert repeated['sha256'] == noon['sha256']
        if self.args.native_descriptor:
            with self.owner('restore projected cloud shadow save', game=game, saves=saves) as (client, cold):
                previous = self.initialize(client, self.revision, uid(6918), saves, game)
                restored, _ = self.restore(client, previous, self.compiled_state(client, previous, 0), CHECKPOINT, 6919)
                state = self.compiled_state(client, restored, CHECKPOINT)
                assert state == checkpoint and self.archive(client, saves, restored, state, 'cold-restored') == saved
                cold_frame, _ = self.capture_shadows(client, 'cold-wind', 'active', session=restored, tick=CHECKPOINT)
                assert cold_frame['sha256'] == noon['sha256']
            assert cold['pid'] != warm['pid'], (warm, cold)
            self.replay_noon(game, saves, repeated)
        assert tree_hashes(relocated) == inventory and tree_hashes(detached) == source
        self.measurements['bundle'] = dict(game_sha256=file_hash(game), inventory=inventory, detached_source=source)

    def run(self):
        features = success(json.loads(self.cli('capabilities')))['features']
        assert features['simulation'] and features[FEATURE] is features['volumetric_clouds'], features
        if self.args.gpu is not None:
            assert features[FEATURE], features
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Cloud shadow witness')))
        self.measurements['captures'] = {}
        with self.owner('author projected cloud shadows') as (client, _):
            self.create_witnesses(client)
            self.check_authoring(client)
            if self.args.gpu is not None:
                self.check_geometry(client)
            else:
                self.edit(client, self.environment_op(profile(0)))
                self.check_selection(client, 0)
                self.edit(client, self.environment_op(profile(1)))
            self.check_selection(client, 1)
        before = self.world.read_bytes()
        with self.owner('reopen cloud shadow selection') as (client, _):
            self.check_selection(client, 1)
            assert self.world.read_bytes() == before
        if self.args.runtime_root:
            self.relocate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--runtime-root', type=Path, help='Installed Vulkan runtime for a relocated export.')
    parser.add_argument('--native-descriptor', type=Path, help='Compiled solar fixture for wind and cold replay.')
    parser.add_argument('--baseline-binary', type=Path, help='Retained runtime for omitted cloud shadow parity.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    parser.add_argument('--fsr', action='store_true', help='Also check one-sample FSR Native AA.')
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
    root = Path(tempfile.mkdtemp(prefix='cloud-shadow-', dir=args.output.resolve()))
    fixture = CloudShadowContract(args, root)
    evidence = dict(format='poima.cloud-shadow-capture.v1', passed=False,
                    binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                    gpu=args.gpu, lighting_path=args.lighting_path, samples=args.samples,
                    fsr=args.fsr, require_validation=args.require_validation, calls=fixture.calls,
                    measurements=fixture.measurements)
    try:
        fixture.run()
        gpu_calls = {'author projected cloud shadows', 'author wind displacement control', 'play relocated cloud shadows',
                     'restore projected cloud shadow save', 'replay cached cloud shadow map'}
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
