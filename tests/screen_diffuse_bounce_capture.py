#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a local diffuse color bounce through the native deferred Vulkan path."""

import argparse
import json
import math
from pathlib import Path
import sys
import traceback
import uuid

from hdr_composition_capture import check, component, uid
from project_post_effect_capture import HEIGHT, WIDTH, Qualification, sha256
from screen_contact_shadow_capture import entry, transform


CAMERA, ENVIRONMENT, RECEIVER, DONOR, SIDE_LIGHT, CONTROLLER = range(8650, 8656)
LENS = dict(vertical_fov=60, near=.1, far=60)
RECEIVER_BOX = ((0, 0, -4.08), (4.4, 1.6, .16))
DONOR_BOX = ((.4125, 0, -3.345), (.185, 1.44, 1.27))
EYE = (0, 0, 0)
BASE = (.65, .55, .3)
DONOR_BASE = (.35, .7, .9)
EMISSION = (1, 0, 0)
SKY = (.05, .16, .27)
AMBIENT = (.08, .21, .31)
PROFILE = dict(radius_m=1.75, thickness_m=.2, strength=1)
FEATURE = 'screen_diffuse_bounce'
SITES = {
    'near': [(x, y) for x in (158, 163, 166) for y in (104, 120, 136)],
    'far': [(x, y) for x in (58, 64, 70) for y in (104, 120, 136)],
    'donor': [(183, 104), (183, 120), (183, 136)],
    'sky': [(162, 45), (162, 195)],
}
LINEAR_TOLERANCE = .002


def material(base, *, metallic=0, emissive=(0, 0, 0)):
    return dict(base_color=base, metallic=metallic, roughness=.85,
                emissive=emissive, double_sided=False)


def donor_form_factor(point, radius):
    """Integrate projected solid angle over the visible donor's flat left face."""
    center, size = DONOR_BOX
    dx = center[0] - size[0]/2 - point[0]
    low_y, low_z = (center[i] - size[i]/2 - point[i] for i in (1, 2))
    bins = 64
    dy, dz = size[1]/bins, size[2]/bins
    weight = 0.
    for yi in range(bins):
        y = low_y + (yi+.5)*dy
        for zi in range(bins):
            z = low_z + (zi+.5)*dz
            distance_squared = dx*dx + y*y + z*z
            if z > 0 and distance_squared <= radius*radius:
                # Cosines for the +Z receiver and -X donor over the same distance.
                weight += dx * z / (math.pi * distance_squared**2)
    return weight * dy * dz


def reference_sites():
    focal = HEIGHT / (2 * math.tan(math.radians(LENS['vertical_fov'])/2))
    references = {}
    for group, coords in SITES.items():
        references[group] = []
        for x, y in coords:
            ray = ((x+.5-WIDTH/2)/focal, (HEIGHT/2-y-.5)/focal, -1.)
            receiver, donor = (entry(box, EYE, ray) for box in (RECEIVER_BOX, DONOR_BOX))
            factor = 0.
            if group in ('near', 'far'):
                check(receiver is not None and donor is None, (group, x, y))
                point = tuple(value * receiver for value in ray)
                factor = donor_form_factor(point, PROFILE['radius_m'])
                if group == 'near':
                    check(.24 < factor < .42 and donor_form_factor(point, .2) == 0, (x, y, factor))
                else:
                    check(factor == 0, (x, y, factor))
                z = receiver
            elif group == 'donor':
                check(donor is not None and receiver is not None and donor < receiver, (group, x, y))
                z = donor
            else:
                check(receiver is None and donor is None, (group, x, y))
                z = None
            near, far = LENS['near'], LENS['far']
            depth = far * (1 - near/z) / (far-near) if z is not None else 1
            references[group].append(dict(x=x, y=y, camera_depth_m=z,
                expected_depth=depth, visible_donor_form_factor=factor))
    return references


class DiffuseQualification(Qualification):
    def __init__(self, options):
        super().__init__(options)
        path = Path(__file__)
        self.report['test_sha256'] = sha256(path.read_bytes())
        for name in ('project_post_effect_capture.py', 'screen_contact_shadow_capture.py'):
            self.report['oracle_helpers_sha256'][name] = sha256(path.with_name(name).read_bytes())
        self.reference = reference_sites()
        self.report['checks']['analytic_sites'] = self.reference

    def environment(self, profile=None, *, reflections=False):
        value = dict(ambient=AMBIENT, exposure=1, sky=dict(enabled=True, zenith=SKY, horizon=SKY, ground=SKY,
            horizon_falloff=.35, sun=None, sun_size_degrees=.53, sun_intensity=0))
        if profile is not None:
            value[FEATURE] = profile
        if reflections:
            value['screen_reflections'] = dict(strength=.6, max_distance_m=4, thickness_m=.12)
        return component(ENVIRONMENT, 'LightingEnvironment', value)

    def author(self):
        entities = {CAMERA: 'Fixed camera', ENVIRONMENT: 'Room lighting',
                    RECEIVER: 'Matte receiver', DONOR: 'Colored donor'}
        ops = [dict(op='entity.create', id=uid(entity), name=name) for entity, name in entities.items()]
        ops.extend([component(CAMERA, 'Transform', transform(EYE)),
                    component(CAMERA, 'Camera', LENS), self.environment()])
        for entity, (center, size), color in ((RECEIVER, RECEIVER_BOX, BASE), (DONOR, DONOR_BOX, DONOR_BASE)):
            ops += [component(entity, 'Transform', transform(center, scale=size)),
                    component(entity, 'MeshRenderer', dict(primitive='box', visible=True, albedo=color)),
                    component(entity, 'PbrMaterial', material(color, emissive=EMISSION if entity == DONOR else EYE))]
        self.edit(ops)

    def capture(self, label, profile=None, *, reason=None, route='deferred', reconstruction='none',
                debug='color', binary=None):
        path = self.run / (label + '.bmp')
        before = self.world.read_bytes()
        sites = [site for rows in self.reference.values() for site in rows]
        params = dict(revision=self.revision, camera=uid(CAMERA), path=self.native(path), width=WIDTH,
            height=HEIGHT, samples=1, gpu=self.options.gpu, lighting_path=route,
            reconstruction=reconstruction, capture_frames=2, scene_debug_view=debug, profile=True,
            scene_product_probes=[dict(x=site['x'], y=site['y']) for site in sites])
        if binary is None:
            rendered, = self.rpc(('world.capture', params))
        else:
            request = dict(jsonrpc='2.0', id=1, method='world.capture', params=params)
            response = self.invoke([binary, 'world', self.native(self.world)], stdin=json.dumps(request) + '\n')
            reply = json.loads(response)
            check(reply.get('id') == 1 and 'result' in reply, reply)
            rendered = reply['result']
        data, _ = self.image(label, path, rendered, 1)
        check(self.world.read_bytes() == before and rendered['revision'] == self.revision, rendered)
        if self.options.expected_gpu:
            check(rendered['gpu'] == self.options.expected_gpu, rendered['gpu'])
        diagnostics = rendered['render_diagnostics']
        check(rendered['frames_presented'] == 2 and diagnostics['lighting_path'] == route, diagnostics)
        receipts = diagnostics['scene_receipts']
        check(receipts['presented'] == receipts['submitted'] and receipts['presented'], receipts)
        admitted = rendered['lighting'].get(FEATURE)
        if profile is None:
            check(admitted is None and not diagnostics.get(FEATURE, {}).get('active', False), diagnostics)
        else:
            check(admitted is not None and set(admitted) == set(profile)
                  and all(math.isclose(admitted[key], value, rel_tol=1e-7, abs_tol=1e-8)
                          for key, value in profile.items()), admitted)
            self.check_status(diagnostics[FEATURE], reason)
        if reconstruction != 'none':
            reconstruction_info = diagnostics['reconstruction']
            check(reconstruction_info['active'] and reconstruction_info['mode'] == reconstruction,
                  reconstruction_info)
        probes = diagnostics['scene_products']['probes']
        for actual, site in zip(probes, sites, strict=True):
            expected_depth = site['expected_depth']
            if reconstruction != 'none':
                # A subpixel shift across the near-depth strip moves the
                # independently projected donor side depth, not the plane.
                tolerance = .001
            else:
                tolerance = .00006
            check((actual['x'], actual['y']) == (site['x'], site['y'])
                  and math.isclose(actual['depth'], expected_depth, rel_tol=0, abs_tol=tolerance),
                  (label, actual, site))
        return data, dict(zip(((site['x'], site['y']) for site in sites), probes, strict=True))

    def check_status(self, status, reason):
        active = reason == 'active'
        check(status['active'] is active and status['reason'] == reason
              and status['rays'] == status['max_steps'] == (12 if active else 0)
              and status['buffer_bytes'] == (16*WIDTH*HEIGHT if active else 0), status)

    def exact_hdr(self, clean, changed, label):
        for pixel, plain in clean.items():
            candidate = changed[pixel]
            for field in ('depth', 'surface_valid', 'shading_normal', 'raw_hdr', 'resolved_hdr'):
                check(candidate[field] == plain[field], (label, pixel, field, plain[field], candidate[field]))

    def verify_bounce(self, clean, active, half):
        result = {}
        for name, sites in self.reference.items():
            rows = result[name] = []
            for site in sites:
                pixel = (site['x'], site['y'])
                original, full, partial = (capture[pixel] for capture in (clean, active, half))
                for field in ('depth', 'surface_valid', 'shading_normal', 'raw_hdr'):
                    check(original[field] == full[field] == partial[field], (name, pixel, field))
                a, b, c = (sample['resolved_hdr'] for sample in (original, full, partial))
                if name != 'near':
                    check(a == b == c, (name, pixel, a, b, c))
                    if name == 'donor':
                        check(a[0] > EMISSION[0] and a[1] > .12 and a[2] > .22,
                              ('Donor lacks its colored ambient control', pixel, a))
                    continue
                delta = b[0] - a[0]
                check(.05 < delta < .7 and a[1:] == b[1:] == c[1:], (pixel, a, b, c))
                check(all(abs(actual-expected*ambient) <= LINEAR_TOLERANCE
                          for actual, expected, ambient in zip(a, BASE, AMBIENT, strict=True)), (pixel, a))
                ratio = delta / (site['visible_donor_form_factor'] * PROFILE['strength'] * BASE[0] * EMISSION[0])
                check(.23 < ratio < 1.8, (pixel, ratio, delta, site))
                error = abs(c[0] - (a[0] + delta/2))
                check(error <= LINEAR_TOLERANCE, (pixel, a, b, c, error))
                rows.append(dict(pixel=pixel, red_delta_hdr=delta, strength_half_error=error,
                    measured_over_projected_solid_angle=ratio))
        self.report['checks']['red_transfer'] = result['near']
        self.report['checks']['exact_other_sites'] = sum(len(SITES[key]) for key in ('far', 'sky', 'donor'))

    def verify_direct_diffuse(self):
        self.edit([
            dict(op='entity.create', id=uid(SIDE_LIGHT), name='Tangent red directional light'),
            component(SIDE_LIGHT, 'Transform', transform(EYE, (0, -math.sqrt(.5), 0, math.sqrt(.5)))),
            component(SIDE_LIGHT, 'Light', dict(kind='directional', enabled=True, color=(1, 0, 0),
                                                intensity=6.5, shadow=dict(enabled=False))),
            component(DONOR, 'PbrMaterial', material((1, .7, .9))),
            self.environment(),
        ])
        _, clean = self.capture('lit-donor-omitted')
        self.edit([self.environment(PROFILE)])
        _, bounced = self.capture('lit-donor-bounce', PROFILE, reason='active')
        changes = []
        for pixel in SITES['near']:
            plain, enabled = clean[pixel], bounced[pixel]
            delta = enabled['resolved_hdr'][0] - plain['resolved_hdr'][0]
            check(plain['raw_hdr'] == enabled['raw_hdr'] and delta > .045
                  and plain['resolved_hdr'][1:] == enabled['resolved_hdr'][1:], (pixel, plain, enabled))
            changes.append(delta)
        for pixel in SITES['donor']:
            check(clean[pixel]['raw_hdr'][0] > 1.65, (pixel, clean[pixel]))
        self.report['checks']['direct_diffuse_red_delta_hdr'] = changes

    def verify_reflections_route(self, baseline, clean, enabled, active):
        self.edit([component(DONOR, 'PbrMaterial', material(DONOR_BASE, emissive=EMISSION)),
                   self.environment(reflections=True)])
        plain, plain_samples = self.capture('reflections-only')
        self.edit([self.environment(PROFILE, reflections=True)])
        combined, combined_samples = self.capture('reflections-and-bounce', PROFILE, reason='active')
        for label in ('reflections-only', 'reflections-and-bounce'):
            diag = self.report['images'][label]['render']['render_diagnostics']['screen_reflections']
            check(diag['active'] and diag['reason'] == 'active'
                  and diag['buffer_bytes'] == 10*WIDTH*HEIGHT, diag)
        # The matte scene activates the reflection route while ruling out
        # specular hits. Its composition must preserve the preceding bounce.
        self.exact_hdr(clean, plain_samples, 'reflections-only')
        self.exact_hdr(active, combined_samples, 'reflections-and-bounce')
        check((plain, combined) == (baseline, enabled), 'Matte reflection composition changed the color')
        self.report['checks']['matte_reflections_composition'] = 'exact HDR probes and BMPs'

    def relocate(self):
        self.edit([
            dict(op='entity.create', id=uid(CONTROLLER), name='Player'),
            component(CONTROLLER, 'Transform', transform(EYE)),
            component(CONTROLLER, 'CharacterController',
                      dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=uid(CAMERA))),
            dict(op='entity.reparent', id=uid(CAMERA), parent=uid(CONTROLLER), mode='keep_local'),
            component(DONOR, 'PbrMaterial', material(DONOR_BASE, emissive=EMISSION)),
            self.environment(PROFILE),
        ])
        manifest = self.source / 'project.poimaproject'
        spec = dict(format='poima.project', version=1, project_id=uuid.uuid4().hex,
                    name='Diffuse Bounce Fixture', audio=False, startup_splash=False,
                    startup_render=dict(lighting_path='deferred'),
                    entry=dict(world=self.world.name, camera=uid(CAMERA), controller=uid(CONTROLLER)))
        manifest.write_text(json.dumps(spec) + '\n', encoding='utf-8')
        authored = self.cli(self.binary, 'project', 'inspect', self.native(manifest))
        check(authored['needs_' + FEATURE], authored)
        runtime = self.options.runtime.resolve(strict=True)
        descriptor = json.loads((runtime / 'runtime.json').read_bytes())
        check(descriptor['features'].pop(FEATURE) is True, descriptor)
        unsupported = self.run / 'Incompatible runtime'
        unsupported.mkdir()
        (unsupported / 'runtime.json').write_text(json.dumps(descriptor) + '\n', encoding='utf-8')
        unpublished = self.run / 'Unpublished'
        build = [self.binary, 'project', 'build', self.native(manifest)]
        try:
            self.invoke([*build, '--output', self.native(unpublished), '--runtime', self.native(unsupported)])
        except RuntimeError:
            entry = self.report['calls'][-1]
            rejection = json.loads(entry['stdout'])
            check(entry['exit_code'] != 0 and rejection['status'] == 'error' and not unpublished.exists()
                  and any('diffuse bounce' in item['message'].lower() for item in rejection['diagnostics']), rejection)
        else:
            raise RuntimeError('An incompatible runtime accepted the authored diffuse bounce')
        initial, bundle = (self.run / name for name in ('Initial bundle', 'Relocated bundle'))
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(initial),
                 '--runtime', self.native(runtime))
        initial.rename(bundle)
        detached = self.run / 'Unavailable source'
        self.source.rename(detached)
        try:
            game = bundle / 'game.json'
            packaged = json.loads(game.read_bytes())
            engine = bundle / packaged['engine']['executable']
            world = bundle / packaged['entry']['world']
            frozen = world.read_bytes()
            inspected = self.cli(engine, 'game', 'inspect', self.native(game), cwd=self.run)
            check(inspected['needs_' + FEATURE] and inspected['startup_render']['lighting_path'] == 'deferred', inspected)
            replay, path = self.run / 'Replay.json', self.run / 'Relocated.bmp'
            replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
            played = self.cli(engine, 'game', 'run', self.native(game), '--gpu', self.options.gpu,
                              '--replay', self.native(replay), '--capture', self.native(path),
                              '--width', WIDTH, '--height', HEIGHT, cwd=self.run)['play']
            self.check_status(played['render_diagnostics'][FEATURE], 'active')
            check(played['tick'] == 1 and played['stop_reason'] == 'replay_complete'
                  and not self.source.exists() and world.read_bytes() == frozen, played)
            _, image = self.image('relocated', path, played, 1)
            near = [image[y][x] for x, y in SITES['near']]
            far = [image[y][x] for x, y in SITES['far']]
            for a, b in zip(near, far, strict=True):
                check(a[0] > b[0] + 12 and all(abs(x-y) <= 1 for x, y in zip(a[1:], b[1:], strict=True)), (a, b))
            self.report['checks']['relocated'] = dict(source_unavailable=True, incompatible_runtime_rejected=True,
                near=near, far=far)
        finally:
            detached.rename(self.source)

    def qualify(self):
        self.author()
        baseline, clean = self.capture('omitted')
        check(all(sample['raw_hdr'] == sample['resolved_hdr'] for sample in clean.values()), clean)
        if self.options.reference:
            retained, _ = self.capture('retained', binary=self.options.reference.resolve(strict=True))
            check(retained == baseline, 'Omitted profile changed retained renderer bytes')
            self.report['checks']['retained_binary_sha256'] = sha256(self.options.reference.read_bytes())
        forward, _ = self.capture('forward-omitted', route='forward')
        self.edit([self.environment(PROFILE)])
        dependencies, = self.rpc(('world.dependencies', {}))
        check(dependencies.get('needs_' + FEATURE) is True, dependencies)
        enabled, active = self.capture('enabled', PROFILE, reason='active')
        fallback, _ = self.capture('forward-fallback', PROFILE, reason='forward', route='forward')
        check(forward == fallback and baseline != enabled, 'Display witnesses did not isolate the bounce')

        half_profile = PROFILE | dict(strength=PROFILE['strength']/2)
        self.edit([self.environment(half_profile)])
        _, partial = self.capture('half-strength', half_profile, reason='active')
        self.verify_bounce(clean, active, partial)
        for label, profile, reason in (
                ('zero-strength', PROFILE | dict(strength=0), 'zero_strength'),
                ('short-radius', PROFILE | dict(radius_m=.2, thickness_m=.05), 'active')):
            self.edit([self.environment(profile)])
            image, samples = self.capture(label, profile, reason=reason)
            self.exact_hdr(clean, samples, label)
            if label == 'zero-strength':
                check(baseline == image, 'Zero strength changed the matched scene')

        for label, edited in (
                ('metallic-receiver', component(RECEIVER, 'PbrMaterial', material(BASE, metallic=1))),
                ('ambient-only-donor', component(DONOR, 'PbrMaterial', material(DONOR_BASE)))):
            self.edit([edited, self.environment()])
            before, control = self.capture(label + '-omitted')
            self.edit([self.environment(PROFILE)])
            after, candidate = self.capture(label + '-enabled', PROFILE, reason='active')
            self.exact_hdr(control, candidate, label)
            check(before == after and any(p['raw_hdr'][1] > .12 for p in control.values()), label)
            self.edit([component(RECEIVER, 'PbrMaterial', material(BASE))])

        if self.options.reflections:
            self.verify_reflections_route(baseline, clean, enabled, active)
        if self.options.fsr:
            self.edit([component(DONOR, 'PbrMaterial', material(DONOR_BASE, emissive=EMISSION)), self.environment()])
            before, samples = self.capture('native-aa-omitted', reconstruction='fsr3_native')
            self.edit([self.environment(PROFILE)])
            after, restored = self.capture('native-aa-enabled', PROFILE, reason='active', reconstruction='fsr3_native')
            dirtied = [restored[pixel]['resolved_hdr'][0]-samples[pixel]['resolved_hdr'][0] for pixel in SITES['near']]
            check(before != after and min(dirtied) > .035, dirtied)
            self.report['checks']['fsr_native_red_delta_hdr'] = dirtied
        if self.options.runtime:
            self.relocate()
        if self.options.direct:
            self.verify_direct_diffuse()

    def run_checks(self):
        try:
            self.qualify()
            self.report['passed'] = True
        except BaseException:
            self.report['failure'] = traceback.format_exc()
            raise
        finally:
            evidence = self.run / 'report.json'
            evidence.write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')
            print(f'Evidence: {evidence}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--expected-gpu')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--fsr', action='store_true')
    parser.add_argument('--direct', action='store_true')
    parser.add_argument('--reflections', action='store_true')
    options = parser.parse_args()
    check(not sys.flags.optimize, 'The shared BMP oracle requires Python assertions')
    DiffuseQualification(options).run_checks()


if __name__ == '__main__':
    main()
