#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check atmospheric sky authoring, frozen saves, and optional Vulkan captures."""

import argparse
import json
import math
import shutil
from pathlib import Path
import tempfile
import traceback
import uuid

from scene_capture import pixels
from sky_capture import display, rotation
from solar_lighting_contract import FINAL, SolarContract, VALIDATION_FAILURES, VALIDATION_LAYER, solar_reference
from world_particles_contract import RpcError, component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


SIZE = (320, 180)
LENS = dict(vertical_fov=48, near=.1, far=200)
CAMERA = uid(6600)
PROFILE = dict(sea_level_m=0, aerosol_strength=1, radiance_scale=4)
PALETTE = .00390625
EXPOSURE = .5
SUN = dict(kind='directional', enabled=True, color=[1, 1, 1], intensity=0)
GROUND = 6371.0
TOP = GROUND + 100
RAYLEIGH = (.005802, .013558, .033100)
OZONE = (.000650, .001881, .000085)
MIE_SCATTERING, MIE_EXTINCTION, MIE_G = .003996, .004440, .8
TEXTURE_BYTES = 128*32*16 + 128*96*8


def unit(vector):
    norm = math.hypot(*vector)
    return tuple(value / norm for value in vector)


def dot(left, right):
    return sum(a*b for a, b in zip(left, right, strict=True))


def sun_direction(elevation):
    r = math.radians(elevation)
    return (0, math.sin(r), -math.cos(r))


def camera_ray(yaw, pitch, x, y):
    width, height = SIZE
    focal = height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    u, v = (x + .5 - width/2) / focal, (height/2 - y - .5) / focal
    yaw, pitch = math.radians(yaw), math.radians(pitch)
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    return unit((u * cy + v * sy * sp - sy * cp, v * cp + sp, -u * sy + v * cy * sp - cy * cp))


def path_length(radius, mu, boundary, far):
    # Solve the ray/sphere quadratic directly in double precision. The oracle
    # never samples or reconstructs the GPU's lookup tables.
    projection = radius * mu
    discriminant = projection*projection + boundary*boundary - radius*radius
    if discriminant < 0:
        return None
    return -projection + (1 if far else -1)*math.sqrt(discriminant)


def densities(radius):
    altitude = max(0.0, radius - GROUND)
    return (math.exp(-altitude / 8), math.exp(-altitude / 1.2),
            max(0.0, 1 - abs(altitude - 25) / 15))


def depth_to_sun(radius, mu, steps=144):
    ground = path_length(radius, mu, GROUND, far=False) if mu < 0 else None
    if ground is not None and ground >= 0:
        return None
    length = path_length(radius, mu, TOP, far=True)
    length = max(0.0, length)
    ds = length / steps
    sums = [0.0, 0.0, 0.0]
    horizontal = radius * math.sqrt(max(0.0, 1 - mu*mu))
    along = radius * mu
    # Composite Simpson integration in distance, independent of the GPU's
    # sphere/horizon parameterization and density-column quadrature.
    for step in range(steps + 1):
        values = densities(math.hypot(horizontal, along + step * ds))
        weight = 1 if step in (0, steps) else 4 if step % 2 else 2
        for column, value in enumerate(values):
            sums[column] += weight * value
    return tuple(value * ds / 3 for value in sums)


def transmittance(columns, aerosol):
    if columns is None:
        return (0.0, 0.0, 0.0)
    rayleigh, mie, ozone = columns
    return tuple(math.exp(-(r*rayleigh + MIE_EXTINCTION*aerosol*mie + o*ozone))
                 for r, o in zip(RAYLEIGH, OZONE, strict=True))


def scattered_radiance(view, sun, altitude_km, aerosol):
    radius = GROUND + altitude_km
    ground = path_length(radius, view[1], GROUND, far=False) if view[1] < 0 else None
    length = ground if ground is not None and ground >= 0 else path_length(radius, view[1], TOP, far=True)
    if length <= 0:
        return (0.0, 0.0, 0.0)
    scattering_angle = dot(view, sun)
    rayleigh_phase = 3*(1 + scattering_angle**2)/(16*math.pi)
    mie_phase = (1-MIE_G**2)/(4*math.pi*(1+MIE_G**2-2*MIE_G*scattering_angle)**1.5)
    rgb = [0.0, 0.0, 0.0]
    accumulated = [0.0, 0.0, 0.0]
    count = 320
    ds = length / count
    for step in range(count):
        distance = (step+.5) * ds
        position = (view[0]*distance, radius + view[1]*distance, view[2]*distance)
        sample_radius = math.hypot(*position)
        local_mu = dot(position, sun) / sample_radius
        medium = densities(sample_radius)
        lighting = transmittance(depth_to_sun(sample_radius, local_mu), aerosol)
        outgoing = transmittance(tuple(total + .5*value*ds for total, value in
                                       zip(accumulated, medium, strict=True)), aerosol)
        for channel in range(3):
            scatter = RAYLEIGH[channel]*rayleigh_phase*medium[0] + MIE_SCATTERING*aerosol*mie_phase*medium[1]
            rgb[channel] += outgoing[channel]*lighting[channel]*scatter*ds
        for column, value in enumerate(medium):
            accumulated[column] += value*ds
    return tuple(rgb)


class AtmosphereContract(SolarContract):
    def edit(self, client, *ops):
        result = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision, ops=ops))
        assert result['committed'] and not result['replayed'] and result['revision'] == self.revision + 1, result
        self.revision = result['revision']

    def light_pose(self, elevation):
        return component(self.sun, 'Transform', transform([0, 0, 0], rotation(yaw=180, pitch=-elevation)))

    def viewpoint(self, yaw, pitch, altitude_m=0, offset=(0, 0)):
        return component(CAMERA, 'Transform',
                         transform([offset[0], altitude_m, offset[1]], rotation(yaw, pitch)))

    def env(self, profile=None, disk=0, **sky_overrides):
        sky = self.base_sky | sky_overrides | dict(sun_intensity=disk)
        environment = dict(ambient=[0, 0, 0], exposure=EXPOSURE, sky=sky)
        if profile is not None:
            environment['atmosphere'] = profile
        return component(self.environment, 'LightingEnvironment', environment)

    def create_world(self, client, document):
        self.revision = document['revision']
        self.controller = json.loads(self.manifest.read_bytes())['entry']['controller']
        self.environment = next(key for key, row in document['entities'].items()
                                if 'LightingEnvironment' in row['components'])
        self.sun = document['entities'][self.environment]['components']['LightingEnvironment']['sky']['sun']
        self.base_sky = dict(enabled=True, zenith=[PALETTE]*3, horizon=[PALETTE]*3, ground=[PALETTE]*3,
                             horizon_falloff=.35, sun=self.sun, sun_size_degrees=8)
        ops = [dict(op='entity.create', id=CAMERA, name='Atmosphere camera'),
               component(CAMERA, 'Camera', LENS), self.viewpoint(55, 50), self.light_pose(60),
               component(self.sun, 'Light', SUN), self.env()]
        ops.extend(dict(op='component.remove', id=identity, type='MeshRenderer')
                   for identity, entity in document['entities'].items()
                   if 'MeshRenderer' in entity['components'])
        self.edit(client, *ops)

    def authored_contract(self, client):
        discovery = client.call('world.describe', dict(view='component', name='LightingEnvironment'))
        environment = discovery['components']['LightingEnvironment']
        profile = environment['properties']['atmosphere']
        assert set(profile['required']) == {'aerosol_strength', 'radiance_scale'}, profile
        assert profile['else']['required'] == ['sea_level_m'] and not profile['additionalProperties'], profile
        assert 'atmosphere' not in environment['required']
        expected_bounds = dict(sea_level_m=(-1e6, 1e6), aerosol_strength=(0, 4), radiance_scale=(0, 64))
        for key, (low, high) in expected_bounds.items():
            prop = profile['properties'][key]
            assert (prop['type'], prop['minimum'], prop['maximum']) == ('number', low, high), prop

        baseline = self.world.read_bytes()
        absent = client.call('world.lighting', dict(revision=self.revision))
        assert 'atmosphere' not in absent
        assert 'needs_atmosphere_sky' not in client.call('world.dependencies')
        invalid = {key: self.env(PROFILE | change) for key, change in (
            ('sea level lower bound', dict(sea_level_m=-1e6-1)),
            ('sea level upper bound', dict(sea_level_m=1e6+1)),
            ('aerosol lower bound', dict(aerosol_strength=-.01)),
            ('aerosol upper bound', dict(aerosol_strength=4.01)),
            ('radiance lower bound', dict(radiance_scale=-.01)),
            ('radiance upper bound', dict(radiance_scale=64.01)),
            ('Boolean aerosol', dict(aerosol_strength=True)),
            ('string sea level', dict(sea_level_m='0')),
            ('unknown key', dict(density=1)),
        )}
        for key in PROFILE:
            invalid['omitted ' + key] = self.env({name: value for name, value in PROFILE.items() if name != key})
        invalid.update({
            'disabled sky': self.env(PROFILE, enabled=False),
            'omitted sky': component(self.environment, 'LightingEnvironment',
                                     dict(atmosphere=PROFILE, ambient=[0, 0, 0], exposure=EXPOSURE)),
            'null Sun reference': self.env(PROFILE, sun=None),
            'missing Sun entity': self.env(PROFILE, sun=uid(6601)),
        })
        rejected = {}
        for label, op in invalid.items():
            try:
                client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                    ops=[self.viewpoint(1, 2, 3), op]))
            except RpcError as exc:
                assert exc.code == -32602, (label, exc)
                rejected[label] = exc.message
            else:
                raise AssertionError(('Invalid atmosphere edit accepted', label))
            assert self.world.read_bytes() == baseline, label
        try:
            client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                ops=[component(self.sun, 'Light', SUN | dict(kind='point')), self.env(PROFILE)]))
        except RpcError as exc:
            assert exc.code == -32602, exc
            rejected['non-directional Sun'] = exc.message
        else:
            raise AssertionError('Atmosphere accepted a point light as the Sun')
        assert self.world.read_bytes() == baseline
        preview = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                              ops=[self.env(PROFILE)], preview=True))
        assert not preview['committed'] and self.world.read_bytes() == baseline, preview
        self.edit(client, self.env(PROFILE))
        bound = client.call('world.lighting', dict(revision=self.revision))
        assert bound['atmosphere'] == PROFILE and bound['sky_sun']['id'] == self.sun, bound
        readback = client.call('entity.get', dict(id=self.environment))['value']['components']['LightingEnvironment']
        assert readback['atmosphere'] == PROFILE, readback
        dependencies = client.call('world.dependencies')
        assert dependencies['needs_atmosphere_sky'] is True, dependencies
        self.measurements.update(discovery=profile, rejected=rejected, authored_lighting=bound, dependencies=dependencies)

    def capture(self, client, name, *, session=None, tick=0, probe=None):
        width, height = SIZE
        probe = probe or (width//2, height//2)
        path = self.root / (name + '.bmp')
        params = dict(camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
                      lighting_path=self.args.lighting_path, samples=self.args.samples,
                      reconstruction='none', capture_frames=2)
        if self.args.samples == 1:
            params['scene_product_probes'] = [dict(x=probe[0], y=probe[1])]
        if session is None:
            report = client.call('world.capture', params | dict(revision=self.revision))
        else:
            report = client.call('runtime.capture', params | dict(session_id=session, tick=tick, structure_revision=0))
        assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, report
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, self.args.samples, 2), report
        image = pixels(path)
        payload = dict(sha256=file_hash(path), probe=list(probe), rgb=image[probe[1]][probe[0]], report=report)
        if self.args.samples == 1:
            [witness] = diagnostics['scene_products']['probes']
            assert (witness['x'], witness['y'], witness['resolved_x'], witness['resolved_y']) == (*probe, *probe), witness
            assert not witness['surface_valid'], witness
            payload['hdr'] = witness['raw_hdr']
            expected_rgb = [display(channel * EXPOSURE) for channel in payload['hdr']]
            assert max(abs(a-b) for a, b in zip(payload['rgb'], expected_rgb, strict=True)) <= 2, payload
        self.measurements['captures'][name] = payload
        return payload, image

    def optics(self, capture, height_m=None):
        observed = capture['report']['render_diagnostics'].get('atmosphere_sky')
        if height_m is None:
            assert observed is None, observed
            return
        assert observed['texture_bytes'] == TEXTURE_BYTES, observed
        assert observed['density_generation'] == observed['view_generation'] == 1, observed
        assert abs(observed['observer_height_m'] - height_m) <= .01, observed

    def reference(self, capture, yaw, pitch, *, elevation=None, altitude=0, profile=PROFILE, sample=None):
        view = camera_ray(yaw, pitch, *capture['probe'])
        solar = sun_direction(elevation) if sample is None else sample['toward_sun']
        observer_km = min(20, max(0, (altitude-profile['sea_level_m'])/1000))
        self.optics(capture, observer_km*1000)
        light = capture['report']['lighting']
        assert max(abs(x-y) for x,y in zip(light['sky_sun']['direction'], solar, strict=True)) < 1e-5, light
        gain = sample['sky_gain'] if sample else 1
        tint = sample['sun_tint'] if sample else SUN['color']
        basis = scattered_radiance(view, solar, observer_km, profile['aerosol_strength'])
        expected = tuple(gain*(PALETTE + intensity*color*profile['radiance_scale'])
                         for intensity, color in zip(basis, tint, strict=True))
        observed = capture.get('hdr')
        if observed is not None:
            # The direct integral and fixed GPU table use different quadrature
            # and interpolation. Allow an absolute floor near its dark end.
            assert all(abs(a-b) <= max(.003, .12*b) for a,b in zip(observed, expected, strict=True)), (
                capture['probe'], observed, expected)
        expected_rgb = [display(value*EXPOSURE) for value in expected]
        residual = max(abs(a-b) for a,b in zip(capture['rgb'], expected_rgb, strict=True))
        assert residual <= 7, (capture['probe'], capture['rgb'], expected_rgb, expected)
        capture['reference'] = dict(direction=view, altitude_km=observer_km, linear_rgb=expected,
                                    display_rgb=expected_rgb, max_display_step_error=residual)
        return expected

    def visual_contract(self, client, legacy):
        day, by_yaw = self.capture(client, 'day')
        self.reference(day, 55, 50, elevation=60)
        red, green, blue = day['rgb']
        assert blue > green > red + 8, day['rgb']
        self.edit(client, self.viewpoint(55, 50, offset=(300, 900)))
        moved, _ = self.capture(client, 'translated-at-same-altitude')
        assert moved['sha256'] == day['sha256'], (day['sha256'], moved['sha256'])
        self.edit(client, self.viewpoint(-55, 50))
        mirrored, across_sun = self.capture(client, 'mirrored-azimuth')
        self.reference(mirrored, -55, 50, elevation=60)
        delta = max(abs(a-b) for row_a, row_b in zip(by_yaw, across_sun, strict=True)
                    for pixel_a, pixel_b in zip(row_a, reversed(row_b), strict=True)
                    for a, b in zip(pixel_a, pixel_b, strict=True))
        assert delta <= 2, delta
        self.measurements['mirrored_max_display_step_delta'] = delta
        self.edit(client, self.viewpoint(55, 50, 12_000))
        flight, _ = self.capture(client, 'day-at-12km')
        self.reference(flight, 55, 50, elevation=60, altitude=12_000)
        assert flight['rgb'][2] < day['rgb'][2]-10, (flight['rgb'], day['rgb'])
        lowered_sea = PROFILE | dict(sea_level_m=-12_000)
        self.edit(client, self.env(lowered_sea), self.viewpoint(55, 50))
        same_observer, _ = self.capture(client, 'lowered-sea-level')
        self.reference(same_observer, 55, 50, elevation=60, profile=lowered_sea)
        assert same_observer['sha256'] == flight['sha256'], same_observer
        self.edit(client, self.env(PROFILE), self.viewpoint(0, 8), self.light_pose(2))
        sunset, _ = self.capture(client, 'sunset')
        self.reference(sunset, 0, 8, elevation=2)
        assert sunset['rgb'][0] > sunset['rgb'][2]+8, sunset['rgb']
        self.edit(client, self.light_pose(-3))
        twilight, _ = self.capture(client, 'twilight')
        self.reference(twilight, 0, 8, elevation=-3)
        assert max(twilight['rgb']) > max(legacy['rgb'])+4, (twilight['rgb'], legacy['rgb'])
        self.edit(client, self.light_pose(-90), self.viewpoint(55, 50))
        midnight, _ = self.capture(client, 'midnight')
        self.reference(midnight, 55, 50, elevation=-90)
        assert midnight['sha256'] == legacy['sha256'], (midnight['sha256'], legacy['sha256'])
        self.edit(client, self.light_pose(60), component(self.sun, 'Light', SUN | dict(enabled=False)))
        disabled, _ = self.capture(client, 'disabled-sun')
        self.optics(disabled)
        assert disabled['sha256'] == legacy['sha256'], (disabled['sha256'], legacy['sha256'])
        self.edit(client, component(self.sun, 'Light', SUN), self.env(PROFILE | dict(radiance_scale=0)))
        zero, _ = self.capture(client, 'zero-radiance')
        self.optics(zero)
        assert zero['sha256'] == legacy['sha256'], (zero['sha256'], legacy['sha256'])
        self.edit(client, self.env(PROFILE), self.viewpoint(0, 8), self.light_pose(8))
        scatter, _ = self.capture(client, 'solar-direction-without-disk')
        self.reference(scatter, 0, 8, elevation=8)
        no_aerosols = PROFILE | dict(aerosol_strength=0)
        self.edit(client, self.env(no_aerosols))
        clear, _ = self.capture(client, 'aerosol-free-air')
        self.reference(clear, 0, 8, elevation=8, profile=no_aerosols)
        assert scatter['rgb'][0] > clear['rgb'][0]+10, (scatter['rgb'], clear['rgb'])
        self.edit(client, self.env(PROFILE, disk=1))
        disk, _ = self.capture(client, 'attenuated-disk')
        self.optics(disk, 0)
        expected = transmittance(depth_to_sun(GROUND, camera_ray(0, 8, *disk['probe'])[1]), 1)
        if self.args.samples == 1:
            observed = [b-a for a,b in zip(scatter['hdr'], disk['hdr'], strict=True)]
            assert all(abs(a-b*1.025) <= max(.003, .09*b)
                       for a,b in zip(observed, expected, strict=True)), (observed, expected)
            assert observed[0] > observed[2] + .1, observed
            disk['direct_transmittance'] = dict(observed_delta_hdr=observed, expected_without_legacy_halo=expected)
        else:
            difference = [b-a for a,b in zip(scatter['rgb'], disk['rgb'], strict=True)]
            assert difference[0] > difference[2]+7, difference
        self.edit(client, self.env(PROFILE), self.viewpoint(55, 50), self.light_pose(60))

    def save_archive(self, client, session, tick, saves, slot):
        receipt = client.call('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
            expected_generation=0, slot=slot, session_id=session, expected_tick=tick,
            expected_gameplay_revision=0, expected_structure_revision=0))
        [archive] = (saves / ('slot-' + slot)).glob('*.bin')
        assert file_hash(archive) == receipt['sha256'], receipt
        return archive.read_bytes()

    def frozen_source(self, profile=PROFILE):
        saves = self.root / 'Atmosphere source saves'
        saves.mkdir()
        with self.owner('freeze authored atmosphere') as (client, warm):
            session = self.initialize(client, self.revision, uid(6620), saves, None)
            self.step(client, session, 0, 6621, ticks=2, expected_structure_revision=0)
            frozen = self.light_at(client, session, 2)
            assert frozen['atmosphere'] == profile, frozen
            self.edit(client, self.env(profile | dict(aerosol_strength=2)))
            assert client.call('world.lighting', dict(revision=self.revision))['atmosphere']['aerosol_strength'] == 2
            assert self.light_at(client, session, 2) == frozen
            archive = self.save_archive(client, session, 2, saves, 'origin')
            stored = json.loads(archive)['document']['entities'][self.environment]['components']['LightingEnvironment']
            assert stored['atmosphere'] == profile, stored
        with self.owner('restore authored atmosphere') as (client, cold):
            client.call('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0,
                                              root=self.native(saves)))
            session = uid(6622)
            loaded = client.call('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='origin',
                expected_generation=1, revision=self.revision, expected_session_id=None, expected_tick=None,
                expected_gameplay_revision=None, expected_structure_revision=None, new_session_id=session))
            assert loaded['source_stale'] and loaded['selected_generation'] == 1 and not loaded['recovered'], loaded
            assert self.light_at(client, session, 2) == frozen
            assert self.save_archive(client, session, 2, saves, 'restored') == archive
            self.edit(client, self.env(profile))
        assert cold['pid'] != warm['pid'], (cold, warm)
        self.measurements['restored'] = frozen

    def relocated(self):
        if self.args.native_descriptor:
            self.configure_artifact()
        initial = self.root / 'Initial Atmosphere Export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(initial),
                                    '--runtime', self.native(self.args.runtime_root))))
        game_root = self.root / 'Relocated Atmosphere Game'
        detached = self.root / 'Unavailable Atmosphere Source'
        initial.rename(game_root)
        self.project.rename(detached)
        assert not self.project.exists() and not initial.exists()
        game = game_root / 'game.json'
        inspection = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        assert inspection['needs_atmosphere_sky'] is True, inspection
        published, source = tree_hashes(game_root), tree_hashes(detached)
        saves = self.root / 'Atmosphere game saves'
        saves.mkdir()
        with self.owner('relocated atmosphere game', game=game, saves=saves) as (client, _):
            session = self.initialize(client, self.revision, uid(6630), saves, game)
            initial_light = self.light_at(client, session, 0)
            assert initial_light['atmosphere'] == PROFILE, initial_light
            day, _ = self.capture(client, 'relocated-day', session=session)
            self.optics(day, 0)
            assert day['sha256'] == self.measurements['captures']['day']['sha256'], day
            if self.args.native_descriptor:
                # Keep dawn and night on a single relocated build using the
                # already-pinned compiled solar gameplay fixture.
                for tick in range(1, FINAL+1):
                    self.step(client, session, tick-1, 6630+tick, expected_structure_revision=0)
                    if tick not in (1, 2, FINAL):
                        continue
                    light = self.light_at(client, session, tick)
                    sample = light['solar_lighting']
                    wanted = solar_reference(tick)
                    assert sample | dict(toward_sun=wanted['toward_sun']) == wanted, sample
                    assert max(abs(a-b) for a,b in zip(sample['toward_sun'], wanted['toward_sun'], strict=True)) < 1e-7
                    frame, _ = self.capture(client, f'relocated-solar-{tick}', session=session, tick=tick)
                    self.reference(frame, 55, 50, sample=sample)
        target = self.measurements['captures']['relocated-solar-5' if self.args.native_descriptor else 'relocated-day']
        with self.owner('continuous atmosphere replay', game=game, saves=saves) as (client, _):
            self.verify_replay(client, client.call('runtime.status')['session_id'], target)
        assert tree_hashes(game_root) == published and tree_hashes(detached) == source
        self.measurements['bundle'] = dict(game_sha256=file_hash(game), inventory=published, detached_source=source)

    def verify_replay(self, client, session, target):
        path = self.root / 'continuous-replay.bmp'
        width, height = SIZE
        report = client.call('runtime.play', dict(session_id=session, expected_tick=0, request_id=uid(6680),
            expected_structure_revision=0, controller=self.controller, camera=CAMERA, mode='replay',
            sequence=[dict(ticks=FINAL)], path=self.native(path), width=width, height=height,
            samples=self.args.samples, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
            reconstruction='none', audio=False))
        assert report['capture_written'] and report['hardware'] and report['success'] and not report['nvrhi_errors'], report
        assert (report['previous_tick'], report['tick'], report['stop_reason']) == (0, FINAL, 'replay_complete'), report
        assert report['initial_session_id'] == report['current_session_id'] == session, report
        assert report['runtime_replacements'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['completed_submissions'] == report['frames_presented'] == FINAL + 1, diagnostics
        assert report['lighting'] == target['report']['lighting'], report['lighting']
        optics = diagnostics['atmosphere_sky']
        # The solar module changes elevation on ticks 1, 2, 4 and 5. Tick 3
        # keeps tick 2, and the final capture resubmits tick 5. All six draws
        # share the static density table.
        assert optics['density_generation'] == 1 and optics['texture_bytes'] == TEXTURE_BYTES, optics
        assert optics['view_generation'] == (4 if self.args.native_descriptor else 1), optics
        assert file_hash(path) == target['sha256'], (file_hash(path), target['sha256'])
        self.measurements['replay'] = dict(sha256=file_hash(path), report=report)

    def run(self):
        capabilities = success(json.loads(self.cli('capabilities')))
        features = capabilities['features']
        assert features['simulation'], features
        assert features['atmosphere_sky'] is features['render_smoke'], features
        if self.args.gpu is not None:
            assert features['atmosphere_sky'], features
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Atmosphere Fixture')))
        document = json.loads(self.world.read_bytes())
        self.measurements['captures'] = {}
        with self.owner('author atmosphere') as (client, _):
            self.create_world(client, document)
            if self.args.gpu is not None:
                legacy, _ = self.capture(client, 'legacy-gradient')
                self.optics(legacy)
                assert all(abs(channel-display(PALETTE*EXPOSURE)) <= 1 for channel in legacy['rgb']), legacy
                if self.args.baseline_binary:
                    self.compare_legacy(legacy)
            self.authored_contract(client)
            if self.args.gpu is not None:
                self.visual_contract(client, legacy)
        self.frozen_source()
        if self.args.runtime_root:
            self.relocated()

    def compare_legacy(self, legacy):
        path = self.root / 'retained-legacy-gradient.bmp'
        width, height = SIZE
        # Run the reference with its own writer lock on a byte-identical
        # snapshot of the asset-free palette scene.
        reference = self.root / 'retained-palette-world.json'
        shutil.copyfile(self.world, reference)
        output = self.cli('world', self.native(reference), executable=self.args.baseline_binary,
            stdin=[dict(jsonrpc='2.0', id=1, method='world.capture',
                        params=dict(revision=self.revision, camera=CAMERA, path=self.native(path),
                                    width=width, height=height, gpu=self.args.gpu, samples=self.args.samples,
                                    lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2))])
        result = success(json.loads(output.splitlines()[0]))
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, result
        assert file_hash(path) == legacy['sha256'], (file_hash(path), legacy['sha256'])
        self.measurements['retained_legacy'] = dict(binary_sha256=file_hash(self.args.baseline_binary),
                                                    capture_sha256=file_hash(path), report=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--runtime-root', type=Path, help='Installed Vulkan runtime for one relocated export.')
    parser.add_argument('--native-descriptor', type=Path, help='Pinned compiled solar fixture for the relocated game.')
    parser.add_argument('--baseline-binary', type=Path, help='Retained compatible runtime for omitted-profile parity.')
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
    root = Path(tempfile.mkdtemp(prefix='atmosphere-', dir=args.output.resolve()))
    fixture = AtmosphereContract(args, root)
    record = dict(format='poima.atmosphere-capture.v1', passed=False,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  gpu=args.gpu, lighting_path=args.lighting_path, samples=args.samples,
                  require_validation=args.require_validation, calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        for call in fixture.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert not any(marker in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') in (
                    'author atmosphere', 'relocated atmosphere game', 'continuous atmosphere replay'):
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
