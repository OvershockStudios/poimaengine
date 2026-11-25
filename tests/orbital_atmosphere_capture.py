#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify radial atmospheric sky authoring, saves, and native Vulkan readback."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback
import uuid

from atmosphere_capture import (
    AtmosphereContract, CAMERA, EXPOSURE, GROUND, MIE_G, MIE_SCATTERING, PALETTE,
    RAYLEIGH, SIZE, TEXTURE_BYTES, TOP, camera_ray, densities, depth_to_sun,
    dot, path_length, sun_direction, transmittance,
)
from scene_capture import pixels
from sky_capture import display, rotation
from solar_lighting_contract import VALIDATION_FAILURES, VALIDATION_LAYER
from world_particles_contract import RpcError, component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


PROFILE = dict(planet_center_m=[0, 0, 0], aerosol_strength=1, radiance_scale=4)
FEATURE = 'orbital_atmosphere'
DEPENDENCY = 'needs_orbital_atmosphere'
ALTITUDE_M = 400_000
FAR_ALTITUDE_M = 2_000_000
SHIFT = [237_000_000, -819_000_000, 412_000_000]
AXIS_TURN = [0, 0, math.sqrt(.5), math.sqrt(.5)]


def rotate_about_z(vector):
    return (-vector[1], vector[0], vector[2])


def multiply_quaternions(left, right):
    x, y, z, w = left
    a, b, c, d = right
    return [w*a + x*d + y*c - z*b, w*b - x*c + y*d + z*a,
            w*c + x*b - y*a + z*d, w*d - x*a - y*b - z*c]


def ray_interval(radius, up):
    """Find the visible forward interval through the atmosphere in kilometres."""
    exit_top = path_length(radius, up, TOP, far=True)
    if exit_top is None or exit_top <= 0:
        return 'outside', 0., 0.
    entry_top = path_length(radius, up, TOP, far=False)
    assert entry_top is not None
    start = max(0., entry_top)
    ground = path_length(radius, up, GROUND, far=False) if up < 0 else None
    if ground is not None and ground >= start:
        return 'ground', start, min(ground, exit_top)
    return 'limb', start, exit_top


def integrate_ray(view, sun, altitude_km, aerosol):
    """Fine distance integration with direct double-precision quadratic roots."""
    radius = GROUND + altitude_km
    kind, start, end = ray_interval(radius, view[1])
    if end <= start:
        return kind, (0., 0., 0.)
    count = 512
    step_km = (end-start) / count
    cosine = dot(view, sun)
    rayleigh_phase = 3*(1 + cosine*cosine)/(16*math.pi)
    mie_phase = (1-MIE_G*MIE_G)/(4*math.pi*(1+MIE_G*MIE_G-2*MIE_G*cosine)**1.5)
    columns = [0., 0., 0.]
    rgb = [0., 0., 0.]
    for index in range(count):
        distance = start + (index+.5)*step_km
        location = (view[0]*distance, radius + view[1]*distance, view[2]*distance)
        sample_radius = math.hypot(*location)
        density = densities(sample_radius)
        sun_columns = depth_to_sun(sample_radius, dot(location, sun)/sample_radius)
        light = transmittance(sun_columns, aerosol)
        view_columns = tuple(a + .5*b*step_km for a,b in zip(columns, density, strict=True))
        outgoing = transmittance(view_columns, aerosol)
        for channel in range(3):
            scatter = RAYLEIGH[channel]*rayleigh_phase*density[0] + MIE_SCATTERING*aerosol*mie_phase*density[1]
            rgb[channel] += scatter*step_km*light[channel]*outgoing[channel]
            columns[channel] += density[channel]*step_km
    return kind, tuple(rgb)


def tangent_elevation(radius, boundary):
    return -math.degrees(math.acos(boundary/radius))


def limb_pose(altitude_m):
    radius = GROUND + altitude_m/1000
    ground = tangent_elevation(radius, GROUND)
    outer = tangent_elevation(radius, TOP)
    return 47., (ground + outer)/2


def limb_probes(altitude_m, yaw, pitch):
    radius = GROUND + altitude_m/1000
    ground = tangent_elevation(radius, GROUND)
    outer = tangent_elevation(radius, TOP)
    wanted = [ground-2, ground+.35*(outer-ground), ground+.7*(outer-ground), outer+2]
    width, height = SIZE
    result = []
    for x in (width//2-30, width//2, width//2+30):
        for elevation in wanted:
            up = math.sin(math.radians(elevation))
            y = min(range(height), key=lambda row: abs(camera_ray(yaw,pitch,x,row)[1] - up))
            result.append((x,y))
    assert len(set(result)) == len(result), result
    return result


class OrbitalContract(AtmosphereContract):
    def author_orbital(self, client):
        discovery = client.call('world.describe', dict(view='component', name='LightingEnvironment'))
        profile = discovery['components']['LightingEnvironment']['properties']['atmosphere']
        center = profile['properties']['planet_center_m']
        assert center['type'] == 'array' and center['minItems'] == center['maxItems'] == 3, center
        assert (center['items']['type'], center['items']['minimum'], center['items']['maximum']) == (
            'number', -1e9, 1e9), center
        assert profile['if']['required'] == ['planet_center_m'], profile
        excluded = {branch['required'][0] for branch in profile['then']['not']['anyOf']}
        assert excluded == {'sea_level_m','clouds','aerial_perspective_m'}, profile
        before = self.world.read_bytes()
        assert DEPENDENCY not in client.call('world.dependencies')
        invalid = {
            'length two': PROFILE | dict(planet_center_m=[1, 2]),
            'length four': PROFILE | dict(planet_center_m=[1, 2, 3, 4]),
            'object': PROFILE | dict(planet_center_m={'x':0,'y':0,'z':0}),
            'boolean coordinate': PROFILE | dict(planet_center_m=[0, True, 0]),
            'string coordinate': PROFILE | dict(planet_center_m=[0, 0, '0']),
            'above maximum': PROFILE | dict(planet_center_m=[1e9+1, 0, 0]),
            'below minimum': PROFILE | dict(planet_center_m=[0, -1e9-1, 0]),
            'local water plane': PROFILE | dict(sea_level_m=0),
            'world-y aerial estimate': PROFILE | dict(aerial_perspective_m=1_000),
            'world-y clouds': PROFILE | dict(clouds=dict(base_height_m=1_000, thickness_m=750,
                coverage=.6, extinction_per_km=2, wind_m_per_s=[0, 0])),
            'missing center and sea level': {key: value for key,value in PROFILE.items()
                                           if key != 'planet_center_m'},
        }
        rejected = {}
        for label, value in invalid.items():
            try:
                client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
                    ops=[self.observer(2_000, 0, 0), self.env(value)]))
            except RpcError as exc:
                assert exc.code == -32602, (label, exc)
                rejected[label] = exc.message
            else:
                raise AssertionError(('Invalid radial profile committed', label))
            assert self.world.read_bytes() == before, label
        view = self.observer(ALTITUDE_M, *limb_pose(ALTITUDE_M))
        preview = client.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=self.revision,
            preview=True, ops=[self.env(PROFILE), view]))
        assert not preview['committed'] and self.world.read_bytes() == before, preview
        self.edit(client, self.env(PROFILE), view, self.light_pose(60))
        light = client.call('world.lighting', dict(revision=self.revision))
        assert light['atmosphere'] == PROFILE, light
        component_state = client.call('entity.get', dict(id=self.environment))['value']['components']
        assert component_state['LightingEnvironment']['atmosphere'] == PROFILE, component_state
        dependencies = client.call('world.dependencies')
        assert dependencies[DEPENDENCY] is True and dependencies['needs_atmosphere_sky'] is True, dependencies
        self.measurements.update(discovery=profile, rejected=rejected, authored_lighting=light,
                                 dependencies=dependencies)

    def observer(self, altitude_m, yaw, pitch, center=PROFILE['planet_center_m'], turned=False):
        position = (0, GROUND*1000+altitude_m, 0)
        orientation = rotation(yaw, pitch)
        if turned:
            position = rotate_about_z(position)
            orientation = multiply_quaternions(AXIS_TURN, orientation)
        world_position = [a+b for a,b in zip(center,position,strict=True)]
        return component(CAMERA, 'Transform', transform(world_position, orientation))

    def orbital_capture(self, client, name, locations, *, altitude_m=ALTITUDE_M,
                        center=PROFILE['planet_center_m'], session=None):
        width, height = SIZE
        path = self.root/(name+'.bmp')
        params = dict(camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
                      lighting_path=self.args.lighting_path, samples=self.args.samples,
                      reconstruction='none', capture_frames=2)
        if self.args.samples == 1:
            params['scene_product_probes'] = [dict(x=x,y=y) for x,y in locations]
        if session is None:
            report = client.call('world.capture', params | dict(revision=self.revision))
        else:
            report = client.call('runtime.capture', params | dict(session_id=session,tick=0,structure_revision=0))
        assert report['capture_written'] and report['hardware'] and not report['nvrhi_errors'], report
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, self.args.samples, 2), report
        diagnostics = report['render_diagnostics']
        optic = diagnostics['atmosphere_sky']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        assert optic['density_generation'] == optic['view_generation'] == 1, optic
        assert optic['texture_bytes'] == TEXTURE_BYTES and optic['planet_center_m'] == center, optic
        assert abs(optic['observer_height_m']-altitude_m) <= .5, optic
        image = pixels(path)
        probes = [dict(location=list(p), rgb=image[p[1]][p[0]]) for p in locations]
        if self.args.samples == 1:
            measurements = diagnostics['scene_products']['probes']
            for point,witness in zip(probes,measurements,strict=True):
                x,y = point['location']
                assert (witness['x'],witness['y'],witness['resolved_x'],witness['resolved_y']) == (x,y,x,y)
                assert not witness['surface_valid'], witness
                point['hdr'] = witness['raw_hdr']
                predicted = [display(value*EXPOSURE) for value in point['hdr']]
                assert max(abs(a-b) for a,b in zip(point['rgb'],predicted,strict=True)) <= 2, point
        result = dict(sha256=file_hash(path), probes=probes, optics=optic)
        self.measurements['captures'][name] = result
        return result, image

    def reference(self, capture, yaw, pitch, altitude_m, elevation):
        solar = sun_direction(elevation)
        maximum_hdr_error, maximum_display_error = 0., 0
        for point in capture['probes']:
            ray = camera_ray(yaw, pitch, *point['location'])
            kind,rgb = integrate_ray(ray, solar, altitude_m/1000, PROFILE['aerosol_strength'])
            linear = [PALETTE + PROFILE['radiance_scale']*channel for channel in rgb]
            predicted = [display(channel*EXPOSURE) for channel in linear]
            error = max(abs(a-b) for a,b in zip(point['rgb'],predicted,strict=True))
            point['reference'] = dict(kind=kind, direction=ray, linear=linear, rgb=predicted, display_error=error)
            maximum_display_error = max(maximum_display_error,error)
            if 'hdr' in point:
                deltas = [abs(a-b) for a,b in zip(point['hdr'],linear,strict=True)]
                maximum_hdr_error = max(maximum_hdr_error,*deltas)
                assert all(delta <= max(.003, .15*value) for delta,value in zip(deltas,linear,strict=True)), point
            assert error <= 7, point
        capture.update(maximum_hdr_error=maximum_hdr_error, maximum_display_error=maximum_display_error)

    def check_orbital(self, client):
        yaw,pitch = limb_pose(ALTITUDE_M)
        locations = limb_probes(ALTITUDE_M, yaw, pitch)
        orbit,original = self.orbital_capture(client, 'illuminated-limb', locations)
        self.reference(orbit, yaw, pitch, ALTITUDE_M, 60)
        by_kind = {kind:[p for p in orbit['probes'] if p['reference']['kind']==kind]
                   for kind in ('outside','limb','ground')}
        assert all(by_kind.values()), by_kind
        assert all(p['rgb'] == (display(PALETTE*EXPOSURE),)*3 for p in by_kind['outside']), by_kind['outside']
        assert any(p['rgb'][2] > p['rgb'][0]+8 for p in by_kind['limb']), by_kind['limb']

        profile = PROFILE | dict(planet_center_m=SHIFT)
        self.edit(client, self.env(profile), self.observer(ALTITUDE_M,yaw,pitch,SHIFT))
        shifted,_ = self.orbital_capture(client,'translated-origin',locations,center=SHIFT)
        assert shifted['sha256'] == orbit['sha256'], shifted

        sunlight = multiply_quaternions(AXIS_TURN, rotation(yaw=180,pitch=-60))
        self.edit(client, self.observer(ALTITUDE_M,yaw,pitch,SHIFT,turned=True),
                  component(self.sun,'Transform',transform([0,0,0],sunlight)))
        turned,rotated_image = self.orbital_capture(client,'rotated-radial-frame',locations,center=SHIFT)
        maximum = max(abs(a-b) for lhs,rhs in zip(original,rotated_image,strict=True)
                      for left,right in zip(lhs,rhs,strict=True) for a,b in zip(left,right,strict=True))
        assert maximum <= 2, (maximum,turned)
        self.measurements['radial_rotation_max_display_delta'] = maximum

        self.edit(client, self.env(PROFILE), self.observer(ALTITUDE_M,yaw,pitch), self.light_pose(-90))
        night,_ = self.orbital_capture(client,'unlit-limb',locations)
        self.reference(night,yaw,pitch,ALTITUDE_M,-90)
        assert all(max(p['rgb']) <= display(PALETTE*EXPOSURE)+1 for p in night['probes']), night

        far_yaw, far_pitch = limb_pose(FAR_ALTITUDE_M)
        far_locations = limb_probes(FAR_ALTITUDE_M, far_yaw, far_pitch)
        self.edit(client, self.observer(FAR_ALTITUDE_M, far_yaw, far_pitch), self.light_pose(60))
        far, _ = self.orbital_capture(client, 'distant-limb', far_locations, altitude_m=FAR_ALTITUDE_M)
        self.reference(far, far_yaw, far_pitch, FAR_ALTITUDE_M, 60)
        assert any(p['rgb'][2] > p['rgb'][0]+8 and p['reference']['kind'] == 'limb'
                   for p in far['probes']), far

        center = [(SIZE[0]//2,SIZE[1]//2)]
        self.edit(client, self.observer(ALTITUDE_M,0,-90), self.light_pose(-90))
        shadow,_ = self.orbital_capture(client,'planet-shadow',center)
        self.edit(client,self.env(PROFILE,disk=1))
        blocked,_ = self.orbital_capture(client,'occulted-sun',center)
        assert blocked['sha256'] == shadow['sha256'], blocked
        self.edit(client, self.observer(ALTITUDE_M,0,90), self.light_pose(90))
        clear,_ = self.orbital_capture(client,'clear-sun',center)
        pixel = clear['probes'][0]
        assert all(abs(channel-display(EXPOSURE*(PALETTE+1.025)))<=1 for channel in pixel['rgb']), pixel

        self.edit(client,self.env(PROFILE),self.observer(ALTITUDE_M,yaw,pitch),self.light_pose(60))
        return orbit, locations

    def standalone(self, expected, locations):
        initial = self.root/'Orbital fixture export'
        older = self.root/'Runtime missing orbital atmosphere'
        older.mkdir()
        config = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert config['features'].pop(FEATURE) is True, config
        (older/'runtime.json').write_text(json.dumps(config),encoding='utf-8')
        command = ['project','build',self.native(self.manifest),'--output',self.native(initial),'--runtime']
        process = subprocess.run([str(self.args.binary.resolve()),*command,self.native(older)],
                                 capture_output=True,text=True,timeout=60)
        self.calls.append(dict(label='old runtime rejection',exit=process.returncode,
                               stdout=process.stdout,stderr=process.stderr))
        assert process.returncode == 4 and not initial.exists(), process
        message = json.loads(process.stdout)['diagnostics'][0]['message']
        assert 'orbital' in message.lower() and 'atmosphere' in message.lower(), message
        success(json.loads(self.cli(*command, self.native(self.args.runtime_root))))
        target = self.root/'Relocated orbital fixture'
        hidden = self.root/'Unavailable orbital source'
        initial.rename(target)
        self.project.rename(hidden)
        try:
            manifest = target/'game.json'
            executable = target/json.loads(manifest.read_bytes())['engine']['executable']
            report = success(json.loads(self.cli('game','inspect',self.native(manifest),executable=executable)))
            assert report[DEPENDENCY] is True, report
            inventory,source = tree_hashes(target),tree_hashes(hidden)
            saves = self.root/'Orbital export saves'
            saves.mkdir()
            with self.owner('relocated orbital fixture',game=manifest,saves=saves) as (client,_):
                session = self.initialize(client,self.revision,uid(6710),saves,manifest)
                actual,_ = self.orbital_capture(client,'relocated-render',locations,session=session)
                assert actual['sha256'] == expected['sha256'], actual
            assert tree_hashes(target) == inventory and tree_hashes(hidden) == source
            self.measurements['standalone'] = dict(older_runtime_rejected=message, files=len(inventory),
                                                  game_sha256=file_hash(manifest))
        finally:
            hidden.rename(self.project)

    def run(self):
        features = success(json.loads(self.cli('capabilities')))['features']
        assert features['simulation'], features
        assert features[FEATURE] is features['atmosphere_sky'] is features['render_smoke'], features
        if self.args.gpu is not None:
            assert features[FEATURE], features
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Orbital Atmosphere Fixture')))
        self.measurements['captures'] = {}
        with self.owner('author orbital atmosphere') as (client,_):
            self.create_world(client,json.loads(self.world.read_bytes()))
            self.author_orbital(client)
            if self.args.gpu is not None:
                expected, locations = self.check_orbital(client)
        inspected = success(json.loads(self.cli('project','inspect',self.native(self.manifest))))
        assert inspected[DEPENDENCY] is True, inspected
        self.frozen_source(PROFILE)
        if self.args.runtime_root:
            self.standalone(expected,locations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--samples',type=int,choices=(1,4),default=1)
    parser.add_argument('--runtime-root',type=Path,help='Installed Vulkan runtime for the relocated export.')
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--require-validation',action='store_true')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is not None and not 0 <= args.gpu <= 4095):
        parser.error('Run with assertions enabled and a GPU index from 0 to 4095.')
    if args.samples == 4 and args.lighting_path == 'deferred':
        parser.error('Deferred capture requires one sample.')
    if (args.runtime_root or args.require_validation) and args.gpu is None:
        parser.error('Standalone export and Vulkan validation require a GPU capture.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='orbital-atmosphere-',dir=args.output.resolve()))
    fixture = OrbitalContract(args,root)
    result = dict(format='poima.orbital-atmosphere-capture.v1',passed=False,
                  binary_sha256=file_hash(args.binary),harness_sha256=file_hash(Path(__file__)),
                  gpu=args.gpu,lighting_path=args.lighting_path,samples=args.samples,
                  calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        for call in fixture.calls:
            stderr = call.get('wire',{}).get('stderr.log',call.get('stderr',''))
            assert not any(marker in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') in ('author orbital atmosphere','relocated orbital fixture'):
                assert VALIDATION_LAYER in stderr, stderr
        result['passed'] = True
    except BaseException:
        result['failure']=traceback.format_exc()
        raise
    finally:
        path=root/'evidence.json'
        path.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
