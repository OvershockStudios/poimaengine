#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored aerial perspective through Vulkan capture and relocated solar replay."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import tempfile
import traceback
import uuid

from atmosphere_capture import (
    AtmosphereContract, CAMERA, EXPOSURE, GROUND, LENS as SKY_LENS, MIE_G, MIE_SCATTERING, PROFILE, RAYLEIGH, SIZE, TOP,
    camera_ray, densities, depth_to_sun, dot, path_length, sun_direction, transmittance,
)
from scene_capture import pixels
from sky_capture import display
from solar_lighting_contract import FINAL, VALIDATION_FAILURES, VALIDATION_LAYER, solar_reference
from world_particles_contract import RpcError, component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


MAX_DISTANCE = 8000
EYE_HEIGHT = 1450
SUN_ELEVATION = 28
SIGHTLINES = {
    'near': ((74, 96), 1320),
    'distant': ((155, 97), 4700),
    'past_maximum': ((244, 96), 11400),
    'sky': ((157, 53), None),
}
LENS = SKY_LENS | dict(near=10, far=20000)
EMISSION = (.1875, .3125, .4375)
AERIAL_PROFILE = PROFILE | dict(aerial_perspective_m=MAX_DISTANCE)
FOG_PROFILE = dict(density_per_m=.002, base_height_m=1400, height_falloff_per_m=0,
                   start_distance_m=32, max_distance_m=512, ambient_radiance=[.48, .22, .08], anisotropy=0)
TABLE_BYTES = 32 * 18 * 24 * 8 * 2


def panel(index, pixel, depth):
    entity = uid(6710 + index)
    ray = camera_ray(0, 0, *pixel)
    distance = depth / -ray[2]
    center = [distance * ray[0], EYE_HEIGHT + distance * ray[1], -depth - 4]
    return [dict(op='entity.create', id=entity, name=f'Emissive witness at {depth} m'),
            component(entity, 'Transform', transform(center) | dict(scale=[.11 * depth, .11 * depth, 8])),
            component(entity, 'MeshRenderer', dict(primitive='box', albedo=[0]*3, visible=True,
                                                    cast_shadows=False, receive_shadows=False)),
            component(entity, 'PbrMaterial', dict(base_color=[0]*3, roughness=.85, metallic=1,
                                                  emissive=list(EMISSION), double_sided=False))]


def segment_optics(ray, distance_m, solar):
    """Integrate a single view ray directly in double precision, without lookup interpolation."""
    radius = GROUND + EYE_HEIGHT * .001
    ground = path_length(radius, ray[1], GROUND, far=False) if ray[1] < 0 else None
    boundary = ground if ground is not None and ground >= 0 else path_length(radius, ray[1], TOP, far=True)
    distance_km = min(boundary, distance_m * .001)
    sun = solar['toward_sun'] if solar is not None else sun_direction(SUN_ELEVATION)
    sky_gain = solar['sky_gain'] if solar is not None else 1
    tint = solar['sun_tint'] if solar is not None else (1, 1, 1)
    aerosol = AERIAL_PROFILE['aerosol_strength']
    phase_cosine = dot(ray, sun)
    rayleigh_phase = 3 * (1 + phase_cosine**2) / (16 * math.pi)
    mie_phase = (1 - MIE_G**2) / (4 * math.pi * (1 + MIE_G**2 - 2 * MIE_G * phase_cosine)**1.5)
    columns, scattered = [0.0]*3, [0.0]*3
    steps = 192
    stride = distance_km / steps
    for index in range(steps):
        distance = (index + .5) * stride
        point = (distance * ray[0], radius + distance * ray[1], distance * ray[2])
        sample_radius = math.hypot(*point)
        density = densities(sample_radius)
        to_sun = transmittance(depth_to_sun(sample_radius, dot(point, sun) / sample_radius), aerosol)
        to_camera = transmittance([total + .5 * value * stride for total, value in zip(columns, density, strict=True)],
                                  aerosol)
        for channel, coefficient in enumerate(RAYLEIGH):
            source = density[0] * rayleigh_phase * coefficient + density[1] * mie_phase * MIE_SCATTERING * aerosol
            scattered[channel] += to_camera[channel] * to_sun[channel] * source * stride
        columns = [total + value * stride for total, value in zip(columns, density, strict=True)]
    scale = AERIAL_PROFILE['radiance_scale'] * sky_gain
    return transmittance(columns, aerosol), [scale * source * color for source, color in zip(scattered, tint, strict=True)]


def expected_fog(color, distance_m):
    length = max(0, min(distance_m, FOG_PROFILE['max_distance_m']) - FOG_PROFILE['start_distance_m'])
    transmission = math.exp(-length * FOG_PROFILE['density_per_m'])
    return [channel * transmission + fog * (1 - transmission)
            for channel, fog in zip(color, FOG_PROFILE['ambient_radiance'], strict=True)]


class AerialPerspectiveContract(AtmosphereContract):
    def environment_op(self, profile, fog=False):
        operation = self.env(profile)
        if fog:
            operation['value']['height_fog'] = FOG_PROFILE
        return operation

    def create_witnesses(self, client, document):
        self.create_world(client, document)
        ops = [self.viewpoint(0, 0, EYE_HEIGHT), self.light_pose(SUN_ELEVATION),
               component(CAMERA, 'Camera', LENS)]
        for index, (pixel, depth) in enumerate(SIGHTLINES.values()):
            if depth is not None:
                ops.extend(panel(index, pixel, depth))
        self.edit(client, *ops)

    def check_authoring(self, client):
        schema = client.call('world.describe', dict(view='component', name='LightingEnvironment'))
        atmosphere = schema['components']['LightingEnvironment']['properties']['atmosphere']
        name = 'aerial_perspective_m'
        field = atmosphere['properties'][name]
        assert name not in atmosphere['required'], atmosphere
        assert (field['type'], field['minimum'], field['maximum']) == ('number', 128, 65536), field
        dependency = 'needs_aerial_perspective'
        assert dependency not in client.call('world.dependencies')
        baseline = self.world.read_bytes()
        rejected = {}
        for label, invalid in (('below minimum', 127.99), ('above maximum', 65536.01), ('boolean', True),
                               ('string', '8000'), ('null', None)):
            try:
                client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                    ops=[self.viewpoint(0, 0, 0), self.env(PROFILE | {name: invalid})]))
            except RpcError as error:
                assert error.code == -32602 and name in error.message, (label, error)
                rejected[label] = error.message
            else:
                raise AssertionError(('Invalid aerial distance accepted', label, invalid))
            assert self.world.read_bytes() == baseline, label
        preview = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                              ops=[self.env(PROFILE | {name: 128})], preview=True))
        assert not preview['committed'] and self.world.read_bytes() == baseline, preview
        self.edit(client, self.env(PROFILE))
        assert dependency not in client.call('world.dependencies')
        self.measurements['discovery'] = field
        self.measurements['rejected'] = rejected

    def expect_profile(self, client):
        lighting = client.call('world.lighting', dict(revision=self.revision))
        assert lighting['atmosphere'] == AERIAL_PROFILE, lighting
        dependencies = client.call('world.dependencies')
        assert dependencies['needs_aerial_perspective'] is True and dependencies['needs_atmosphere_sky'] is True
        inspection = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert inspection['needs_aerial_perspective'] is True and inspection['revision'] == self.revision, inspection
        self.measurements.update(authored_lighting=lighting, dependencies=dependencies, project=inspection)

    def expect_diagnostics(self, report, authored):
        diagnostics = report['render_diagnostics']
        if not authored:
            assert 'aerial_perspective' not in diagnostics, diagnostics
            return
        active = self.args.samples == 1
        optics = diagnostics['aerial_perspective']
        assert optics == dict(active=active, reason='active' if active else 'multisample',
                              table_bytes=TABLE_BYTES if active else 0, max_distance_m=MAX_DISTANCE), optics

    def capture_witnesses(self, client, name, *, authored=False, fog=False, session=None, tick=0):
        path = self.root / (name + '.bmp')
        width, height = SIZE
        params = dict(camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
                      lighting_path=self.args.lighting_path, samples=self.args.samples,
                      reconstruction='none', capture_frames=2)
        if self.args.samples == 1:
            params['scene_product_probes'] = [dict(x=x, y=y) for (x, y), _ in SIGHTLINES.values()]
        if session is None:
            report = client.call('world.capture', params | dict(revision=self.revision))
        else:
            report = client.call('runtime.capture', params | dict(session_id=session, tick=tick, structure_revision=0))
        assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, self.args.samples, 2), report
        self.expect_diagnostics(report, authored)
        if fog:
            height_fog = diagnostics['height_fog']
            assert height_fog['active'] is (self.args.samples == 1), height_fog
        image = pixels(path)
        # All three panels are below this band. Check the sky across the full
        # viewport width, including the outer cells of the view-aligned table.
        sky_bytes = bytes(channel for row in image[12:68] for pixel in row for channel in pixel)
        frame = dict(sha256=file_hash(path), sky_sha256=hashlib.sha256(sky_bytes).hexdigest(), report=report, sites={})
        self.measurements['captures'][name] = frame
        for label, ((x, y), _) in SIGHTLINES.items():
            frame['sites'][label] = dict(rgb=image[y][x])
        if self.args.samples == 1:
            solar = None if tick == 0 else solar_reference(tick)
            for (label, layout), probe in zip(SIGHTLINES.items(), diagnostics['scene_products']['probes'], strict=True):
                self.expect_probe(frame['sites'][label], layout, probe, authored, fog, solar)
        return frame

    def expect_probe(self, site, layout, probe, aerial, fog, solar):
        (x, y), depth = layout
        site['sample'] = probe
        assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
        assert probe['surface_valid'] is (depth is not None), probe
        raw, resolved = probe['raw_hdr'], probe['resolved_hdr']
        expected = raw
        ray = camera_ray(0, 0, x, y)
        distance_m = math.inf
        if depth is not None:
            distance_m = depth / -ray[2]
            near, far = LENS['near'], LENS['far']
            observed_depth = near / ((1 - probe['depth']) + probe['depth'] * near / far)
            assert abs(observed_depth - depth) < 2, site
            assert max(abs(a - b) for a, b in zip(raw, EMISSION, strict=True)) < .001, site
            site['radial_distance_m'] = distance_m
            if aerial:
                integration_distance = min(distance_m, MAX_DISTANCE)
                extinction, scattered = segment_optics(ray, integration_distance, solar)
                expected = [radiance * transmission + scatter for radiance, transmission, scatter
                            in zip(raw, extinction, scattered, strict=True)]
                site['integrated_distance_m'] = integration_distance
                site['expected_scattered_hdr'] = scattered
                site['expected_rgb_transmittance'] = extinction
        else:
            assert probe['depth'] == 1, probe
        if fog:
            expected = expected_fog(expected, distance_m)
        site['expected_resolved_hdr'] = expected
        if not fog and (not aerial or depth is None):
            assert raw == resolved, site
        else:
            tolerance = .004 if aerial and depth is not None else .0018
            assert max(abs(a - b) for a, b in zip(resolved, expected, strict=True)) <= tolerance, site
        codes = [display(value * EXPOSURE) for value in resolved]
        assert max(abs(a - b) for a, b in zip(site['rgb'], codes, strict=True)) <= 2, site

    def compare_effects(self, plain, haze, fog_only, layered):
        assert plain['sky_sha256'] == haze['sky_sha256']
        assert fog_only['sky_sha256'] == layered['sky_sha256']
        if self.args.samples == 4:
            assert plain['sha256'] == haze['sha256'] == fog_only['sha256'] == layered['sha256']
            return
        assert math.dist(haze['sites']['near']['sample']['resolved_hdr'],
                         haze['sites']['distant']['sample']['resolved_hdr']) > .009
        for name, (_, depth) in SIGHTLINES.items():
            if depth is None:
                assert fog_only['sites'][name]['sample'] == layered['sites'][name]['sample']
                continue
            base, active, fog, both = [frame['sites'][name]['sample'] for frame in (plain, haze, fog_only, layered)]
            assert base['raw_hdr'] == active['raw_hdr'] == fog['raw_hdr'] == both['raw_hdr']
            assert math.dist(base['resolved_hdr'], active['resolved_hdr']) > .002
            ordered = expected_fog(active['resolved_hdr'], haze['sites'][name]['radial_distance_m'])
            assert max(abs(a - b) for a, b in zip(both['resolved_hdr'], ordered, strict=True)) < .0018
        past = haze['sites']['past_maximum']
        ray = camera_ray(0, 0, *SIGHTLINES['past_maximum'][0])
        missed_extinction, missed_scattering = segment_optics(ray, past['radial_distance_m'], None)
        unclamped = [value * transmission + scatter for value, transmission, scatter
                     in zip(EMISSION, missed_extinction, missed_scattering, strict=True)]
        assert math.dist(unclamped, past['sample']['resolved_hdr']) > .012, (unclamped, past)

    def reject_runtime_without_feature(self):
        # Feature admission runs before runtime payload discovery. Use the real
        # descriptor with only this one capability removed and require the
        # corresponding diagnostic, with no staged output.
        baseline_runtime = self.root / 'Runtime Descriptor Without Aerial Perspective'
        baseline_runtime.mkdir()
        descriptor = json.loads((self.args.runtime_root / 'runtime.json').read_bytes())
        del descriptor['features']['aerial_perspective']
        (baseline_runtime / 'runtime.json').write_text(json.dumps(descriptor) + '\n', encoding='utf-8')
        refused = self.root / 'Refused Aerial Export'
        try:
            self.cli('project', 'build', self.native(self.manifest), '--output', self.native(refused),
                     '--runtime', self.native(baseline_runtime))
        except AssertionError:
            call = self.calls[-1]
            assert call['exit'] != 0, call
            reply = json.loads(call['stdout'])
            assert reply['status'] == 'error', reply
            assert reply['diagnostics'][0]['message'] == 'Aerial perspective requires a compatible Vulkan runtime.', reply
            assert not refused.exists()
            self.measurements['rejected_runtime'] = reply
        else:
            raise AssertionError('Export accepted an installed runtime without the aerial perspective feature')

    def relocate_witnesses(self):
        self.reject_runtime_without_feature()
        if self.args.native_descriptor:
            self.configure_artifact()
        original = self.root / 'Original Aerial Perspective Export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(original),
                                    '--runtime', self.native(self.args.runtime_root))))
        relocated, unavailable = self.root / 'Relocated Aerial Game', self.root / 'Unavailable Aerial Source'
        original.rename(relocated)
        self.project.rename(unavailable)
        assert not self.project.exists() and not original.exists()
        game = relocated / 'game.json'
        inspection = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        assert inspection['needs_aerial_perspective'] is True and inspection['needs_atmosphere_sky'] is True, inspection
        runtime = json.loads((relocated / 'runtime/runtime.json').read_bytes())
        assert runtime['features']['aerial_perspective'] is True, runtime
        published, source = tree_hashes(relocated), tree_hashes(unavailable)
        saves = self.root / 'Aerial Perspective Game Saves'
        saves.mkdir()
        with self.owner('relocated aerial perspective', game=game, saves=saves) as (client, _):
            session = self.initialize(client, self.revision, uid(6730), saves, game)
            initial_lighting = self.light_at(client, session, 0)
            assert initial_lighting['atmosphere'] == AERIAL_PROFILE, initial_lighting
            target = self.capture_witnesses(client, 'relocated-authored', authored=True, fog=True, session=session)
            assert target['sha256'] == self.measurements['captures']['aerial-and-height-fog']['sha256'], target
            if self.args.native_descriptor:
                for tick in range(1, FINAL + 1):
                    self.step(client, session, tick - 1, 6740 + tick, expected_structure_revision=0)
                    if tick not in (1, 2, FINAL):
                        continue
                    lighting = self.light_at(client, session, tick)['solar_lighting']
                    expected = solar_reference(tick)
                    assert lighting | dict(toward_sun=expected['toward_sun']) == expected, lighting
                    assert math.dist(lighting['toward_sun'], expected['toward_sun']) < 1e-7, lighting
                    target = self.capture_witnesses(client, f'relocated-solar-{tick}', authored=True, fog=True,
                                                    session=session, tick=tick)
        with self.owner('continuous aerial replay', game=game, saves=saves) as (client, _):
            self.verify_replay(client, client.call('runtime.status')['session_id'], target)
            self.expect_diagnostics(self.measurements['replay']['report'], True)
        assert tree_hashes(relocated) == published and tree_hashes(unavailable) == source
        self.measurements['bundle'] = dict(game_sha256=file_hash(game), inventory=published, detached_source=source)

    def run(self):
        features = success(json.loads(self.cli('capabilities')))['features']
        assert features['simulation'], features
        assert features['aerial_perspective'] is features['atmosphere_sky'], features
        if self.args.gpu is not None:
            assert features['aerial_perspective'], features
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Aerial Perspective Witnesses')))
        self.measurements['captures'] = {}
        document = json.loads(self.world.read_bytes())
        with self.owner('author aerial perspective') as (client, _):
            self.create_witnesses(client, document)
            self.check_authoring(client)
            capture = self.args.gpu is not None
            if capture:
                plain = self.capture_witnesses(client, 'atmosphere-without-aerial')
                if self.args.baseline_binary:
                    self.compare_legacy(plain)
            self.edit(client, self.env(AERIAL_PROFILE))
            self.expect_profile(client)
            if capture:
                haze = self.capture_witnesses(client, 'aerial-perspective', authored=True)
                self.edit(client, self.environment_op(PROFILE, fog=True))
                fog = self.capture_witnesses(client, 'height-fog-only', fog=True)
                self.edit(client, self.environment_op(AERIAL_PROFILE, fog=True))
                layered = self.capture_witnesses(client, 'aerial-and-height-fog', authored=True, fog=True)
                self.compare_effects(plain, haze, fog, layered)
        if self.args.runtime_root:
            self.relocate_witnesses()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--runtime-root', type=Path, help='Installed Vulkan runtime for one relocated export.')
    parser.add_argument('--native-descriptor', type=Path, help='Pinned compiled solar fixture for the relocated game.')
    parser.add_argument('--baseline-binary', type=Path, help='Retained runtime for omitted aerial perspective parity.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is not None and not 0 <= args.gpu <= 4095):
        parser.error('Run with assertions enabled and a GPU index from 0 to 4095.')
    if args.samples == 4 and args.lighting_path == 'deferred':
        parser.error('Deferred capture requires one sample.')
    if (args.runtime_root or args.baseline_binary or args.require_validation) and args.gpu is None:
        parser.error('Standalone export, retained parity, and Vulkan validation require a GPU capture.')
    if args.native_descriptor and not args.runtime_root:
        parser.error('The compiled solar fixture requires a relocated runtime.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='aerial-', dir=args.output.resolve()))
    fixture = AerialPerspectiveContract(args, root)
    record = dict(format='poima.aerial-perspective-capture.v1', passed=False,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  gpu=args.gpu, lighting_path=args.lighting_path, samples=args.samples,
                  require_validation=args.require_validation, calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        for call in fixture.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert not any(marker in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') in (
                    'author aerial perspective', 'relocated aerial perspective', 'continuous aerial replay'):
                assert VALIDATION_LAYER in stderr, stderr
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        result = root / 'evidence.json'
        result.write_text(json.dumps(record, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(result)


if __name__ == '__main__':
    main()
