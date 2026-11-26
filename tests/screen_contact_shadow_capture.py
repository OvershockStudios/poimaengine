#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify short-range directional contact shadows on Windows Vulkan."""

import argparse
import json
import math
from pathlib import Path
import sys
import traceback
import uuid

from hdr_composition_capture import check, component, uid
from project_post_effect_capture import HEIGHT, WIDTH, Qualification, sha256


CAMERA, ENVIRONMENT, SUN, FILL, RECEIVER, OCCLUDER, CONTROLLER = range(8600, 8607)
LENS = dict(vertical_fov=60, near=.1, far=60)
IDENTITY = (0, 0, 0, 1)
EYE = (0, 0, 0)
LIGHT_RAY = (math.sqrt(.5), 0, math.sqrt(.5))
RECEIVER_BOX = ((0, 0, -4.08), (3.2, 1.6, .16))
OCCLUDER_BOX = ((.4, .05, -3.72), (.32, .75, .18))
SKY = (.0625, .125, .25)
BASE = (.625, .5, .375)
EMISSION = (.09, .05, .017)
AMBIENT = (.125, .3125, .1875)
PROFILE = dict(light=uid(SUN), length_m=.95, thickness_m=.11, strength=.8)
SITES = {
    'covered': [(x, y) for x in (161, 164, 166) for y in (108, 120, 132)],
    'uncovered': [(x, y) for x in (106, 130, 152) for y in (108, 120, 132)],
    'occluder': [(183, 120)],
    'sky': [(162, 52), (162, 187)],
}
HALF_TOLERANCE = .002


def transform(position, rotation=IDENTITY, scale=(1, 1, 1)):
    return dict(position=position, rotation=rotation, scale=scale)


def mesh(*, cast, receive):
    return dict(primitive='box', visible=True, albedo=list(BASE),
                cast_shadows=cast, receive_shadows=receive)


def light(color, enabled=True):
    return dict(kind='directional', color=color, intensity=2.75, enabled=enabled,
                shadow=dict(enabled=False))


def entry(box, point, ray):
    """Intersect a Euclidean ray with the exact authored axis-aligned box."""
    enter, leave = 0., math.inf
    center, size = box
    for p, d, c, extent in zip(point, ray, center, size, strict=True):
        low, high = c - extent/2, c + extent/2
        if abs(d) < 1e-12:
            if not low <= p <= high:
                return None
        else:
            lo, hi = sorted(((low-p)/d, (high-p)/d))
            enter, leave = max(enter, lo), min(leave, hi)
    return enter if 0 < enter < leave else None


def reference_sites():
    """Use world triangles' box bounds and the focal length, independently of GPU tracing."""
    focal = HEIGHT / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    references = {}
    for family, coords in SITES.items():
        references[family] = []
        for x, y in coords:
            ray = ((x+.5-WIDTH/2)/focal, (HEIGHT/2-y-.5)/focal, -1.)
            receiver = entry(RECEIVER_BOX, EYE, ray)
            occluder = entry(OCCLUDER_BOX, EYE, ray)
            distance = None
            if family in ('covered', 'uncovered'):
                check(receiver is not None and occluder is None, (family, x, y))
                point = tuple(depth * receiver for depth in ray)
                distance = entry(OCCLUDER_BOX, point, LIGHT_RAY)
                if family == 'covered':
                    check(distance is not None and .1 < distance < .7, (family, x, y, distance))
                else:
                    check(distance is None, (family, x, y, distance))
                z = receiver
            elif family == 'occluder':
                check(occluder is not None and receiver is not None and occluder < receiver, (family, x, y))
                z = occluder
            else:
                check(receiver is None and occluder is None, (family, x, y))
                z = None
            near, far = LENS['near'], LENS['far']
            depth = far * (1 - near/z) / (far-near) if z is not None else 1
            references[family].append(dict(x=x, y=y, camera_depth_m=z,
                expected_depth=depth, nearest_light_ray_hit_m=distance))
    return references


class ContactQualification(Qualification):
    def __init__(self, options):
        super().__init__(options)
        directory = Path(__file__).parent
        self.report['test_sha256'] = sha256(Path(__file__).read_bytes())
        self.report['oracle_helpers_sha256']['project_post_effect_capture.py'] = sha256(
            (directory / 'project_post_effect_capture.py').read_bytes())
        self.reference = reference_sites()
        self.report['checks']['analytic_sites'] = self.reference

    def author(self):
        entities = {CAMERA: 'Fixed camera', ENVIRONMENT: 'Lighting', SUN: 'Selected red light',
                    FILL: 'Unrelated green light', RECEIVER: 'Shadow receiver', OCCLUDER: 'Shadow caster'}
        ops = [dict(op='entity.create', id=uid(entity), name=name) for entity, name in entities.items()]
        ops.extend([
            component(CAMERA, 'Transform', transform(EYE)),
            component(CAMERA, 'Camera', LENS),
            component(SUN, 'Transform', transform(EYE, (0, math.sin(math.pi/8), 0, math.cos(math.pi/8)))),
            component(SUN, 'Light', light((1, 0, 0))),
            component(FILL, 'Transform', transform(EYE)),
            component(FILL, 'Light', light((0, 1, 0))),
            self.environment(),
        ])
        for entity, (center, size) in ((RECEIVER, RECEIVER_BOX), (OCCLUDER, OCCLUDER_BOX)):
            ops += [
                component(entity, 'Transform', transform(center, scale=size)),
                component(entity, 'MeshRenderer', mesh(cast=entity == OCCLUDER, receive=entity == RECEIVER)),
                component(entity, 'PbrMaterial', dict(base_color=BASE, metallic=0, roughness=.8,
                                                      emissive=EMISSION, double_sided=False)),
            ]
        self.edit(ops)

    def environment(self, profile=None):
        value = dict(ambient=AMBIENT, exposure=1, sky=dict(enabled=True, zenith=SKY, horizon=SKY, ground=SKY,
            horizon_falloff=.35, sun=None, sun_size_degrees=.53, sun_intensity=0))
        if profile is not None:
            value['screen_contact_shadows'] = profile
        return component(ENVIRONMENT, 'LightingEnvironment', value)

    def capture(self, label, profile=None, *, reason=None, route='deferred', reconstruction='none',
                binary=None):
        path = self.run / (label + '.bmp')
        before = self.world.read_bytes()
        params = dict(revision=self.revision, camera=uid(CAMERA), path=self.native(path), width=WIDTH,
            height=HEIGHT, samples=1, gpu=self.options.gpu, lighting_path=route,
            reconstruction=reconstruction, capture_frames=2, profile=True,
            scene_product_probes=[dict(x=site['x'], y=site['y'])
                                   for entries in self.reference.values() for site in entries])
        if binary is None:
            rendered, = self.rpc(('world.capture', params))
        else:
            request = dict(jsonrpc='2.0', id=1, method='world.capture', params=params)
            raw = self.invoke([binary, 'world', self.native(self.world)], stdin=json.dumps(request) + '\n')
            reply = json.loads(raw)
            check(reply.get('id') == 1 and 'result' in reply, reply)
            rendered = reply['result']
        data, _ = self.image(label, path, rendered, 1)
        check(self.world.read_bytes() == before and rendered['revision'] == self.revision, rendered)
        expected_gpu = self.options.expected_gpu
        if expected_gpu:
            check(rendered['gpu'] == expected_gpu, rendered['gpu'])
        diagnostics = rendered['render_diagnostics']
        check(rendered['frames_presented'] == 2 and diagnostics['lighting_path'] == route, diagnostics)
        scene = diagnostics['scene_receipts']
        check(scene['presented'] == scene['submitted'] and scene['presented'], scene)
        admitted = rendered['lighting'].get('screen_contact_shadows')
        if profile is not None:
            check(admitted is not None and set(admitted) == set(profile) and admitted['light'] == profile['light']
                  and all(math.isclose(admitted[key], profile[key], rel_tol=1e-7, abs_tol=1e-8)
                          for key in ('length_m', 'thickness_m', 'strength')), admitted)
            status = diagnostics['screen_contact_shadows']
            active = reason == 'active'
            check(status['active'] is active and status['light'] == uid(SUN)
                  and status['max_samples'] == (16 if active else 0) and status['reason'] == reason, status)
        else:
            check(admitted is None and not diagnostics.get('screen_contact_shadows', {}).get('active', False),
                  diagnostics)
        if reconstruction != 'none':
            options = diagnostics['reconstruction']
            check(options['active'] and options['mode'] == reconstruction, options)
        probes = diagnostics['scene_products']['probes']
        expected = [site for entries in self.reference.values() for site in entries]
        for actual, site in zip(probes, expected, strict=True):
            check((actual['x'], actual['y']) == (site['x'], site['y'])
                  and math.isclose(actual['depth'], site['expected_depth'], rel_tol=0, abs_tol=.00006),
                  (label, actual, site))
        return data, probes

    def compare(self, clean, active, partial, no_sun):
        measurements = []
        covered_count = len(SITES['covered'])
        for index, (base, full, half, dark) in enumerate(zip(clean, active, partial, no_sun, strict=True)):
            label = (base['x'], base['y'])
            for layer in (full, half, dark):
                check((layer['depth'], layer['surface_valid'], layer['shading_normal'])
                      == (base['depth'], base['surface_valid'], base['shading_normal']), (label, base, layer))
                check(layer['raw_hdr'][1:] == base['raw_hdr'][1:], (label, base, layer))
            key = 'raw_hdr'
            original, shadowed, reduced, no_direct = (view[key][0] for view in (base, full, half, dark))
            if index < covered_count:
                sun_contribution = original - no_direct
                check(sun_contribution > .24, (label, original, no_direct))
                check(shadowed >= no_direct and shadowed < original - .12, (label, original, shadowed, no_direct))
                check(abs((original + shadowed) / 2 - reduced) <= HALF_TOLERANCE, (label, original, shadowed, reduced))
                check(abs(no_direct - (EMISSION[0] + AMBIENT[0]*BASE[0])) <= HALF_TOLERANCE,
                      (label, no_direct))
                measurements.append(dict(pixel=label, red_hdr_delta=original-shadowed,
                    selected_light_fraction_removed=(original-shadowed)/sun_contribution,
                    strength_half_error=abs((original+shadowed)/2-reduced)))
            else:
                check(base[key] == full[key] == half[key], (label, base, full, half))
        self.report['checks']['directional_isolation'] = {
            'receivers': measurements, 'exact_uncovered_pixels': len(clean) - covered_count,
            'unchanged_geometry_and_unrelated_channels': True}

    def relocate(self):
        self.edit([
            dict(op='entity.create', id=uid(CONTROLLER), name='Player'),
            component(CONTROLLER, 'Transform', transform(EYE)),
            component(CONTROLLER, 'CharacterController',
                      dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=uid(CAMERA))),
            dict(op='entity.reparent', id=uid(CAMERA), parent=uid(CONTROLLER), mode='keep_local'),
            component(SUN, 'Light', light((1, 0, 0))),
            self.environment(PROFILE),
        ])
        manifest = self.source / 'project.poimaproject'
        spec = dict(format='poima.project', version=1, project_id=uuid.uuid4().hex,
                    name='Contact Shadow Fixture', audio=False, startup_splash=False,
                    startup_render=dict(lighting_path='deferred'),
                    entry=dict(world=self.world.name, camera=uid(CAMERA), controller=uid(CONTROLLER)))
        manifest.write_text(json.dumps(spec) + '\n', encoding='utf-8')
        authored = self.cli(self.binary, 'project', 'inspect', self.native(manifest))
        check(authored['needs_screen_contact_shadows'], authored)
        runtime = self.options.runtime.resolve(strict=True)
        descriptor = json.loads((runtime / 'runtime.json').read_bytes())
        check(descriptor['features'].pop('screen_contact_shadows') is True, descriptor)
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
            check(entry['exit_code'] != 0 and rejection['status'] == 'error', rejection)
            check(not unpublished.exists() and [item['message'] for item in rejection['diagnostics']]
                  == ['Screen contact shadows require a compatible Vulkan runtime.'], rejection)
        else:
            raise RuntimeError('An incompatible runtime accepted the authored contact shadows')
        initial = self.run / 'Initial bundle'
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(initial),
                 '--runtime', self.native(runtime))
        bundle = self.run / 'Relocated bundle'
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
            check(inspected['needs_screen_contact_shadows']
                  and inspected['startup_render']['lighting_path'] == 'deferred', inspected)
            replay = self.run / 'Replay.json'
            replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
            path = self.run / 'Relocated.bmp'
            played = self.cli(engine, 'game', 'run', self.native(game),
                              '--gpu', self.options.gpu, '--replay', self.native(replay),
                              '--capture', self.native(path), '--width', WIDTH, '--height', HEIGHT,
                              cwd=self.run)['play']
            status = played['render_diagnostics']['screen_contact_shadows']
            check(played['tick'] == 1 and played['stop_reason'] == 'replay_complete'
                  and status['active'] and status['light'] == uid(SUN)
                  and not self.source.exists() and world.read_bytes() == frozen, played)
            _, image = self.image('relocated', path, played, 1)
            # Nearby, independently located samples on the same plane.
            covered, clear = image[120][164], image[120][152]
            check(covered[0] + 18 < clear[0] and abs(covered[1]-clear[1]) <= 1
                  and abs(covered[2]-clear[2]) <= 1, (covered, clear))
            self.report['checks']['relocated'] = dict(covered=covered, clear=clear,
                source_unavailable=True, incompatible_runtime_rejected=True)
        finally:
            detached.rename(self.source)

    def qualify(self):
        self.author()
        baseline, clean = self.capture('omitted')
        if self.options.reference:
            retained, _ = self.capture('retained', binary=self.options.reference.resolve(strict=True))
            check(retained == baseline, 'Omitted profile changed retained renderer bytes')
            self.report['checks']['retained_binary_sha256'] = sha256(self.options.reference.read_bytes())
        forward, _ = self.capture('forward-omitted', route='forward')

        self.edit([self.environment(PROFILE)])
        present, = self.rpc(('world.dependencies', {}))
        check(present.get('needs_screen_contact_shadows') is True, present)
        enabled, active = self.capture('enabled', PROFILE, reason='active')
        forward_active, _ = self.capture('forward-fallback', PROFILE, reason='forward', route='forward')
        check(forward_active == forward and enabled != baseline, 'Unsupported path or enabled effect differed unexpectedly')

        partial_profile = PROFILE | dict(strength=PROFILE['strength']/2)
        self.edit([self.environment(partial_profile)])
        _, partial = self.capture('partial-strength', partial_profile, reason='active')
        for label, profile, reason in (
                ('zero-strength', PROFILE | dict(strength=0), 'zero_strength'),
                ('short-ray', PROFILE | dict(length_m=.1, thickness_m=.025), 'active')):
            self.edit([self.environment(profile)])
            image, _ = self.capture(label, profile, reason=reason)
            check(image == baseline, f'{label} changed the matched scene')

        self.edit([self.environment(PROFILE), component(OCCLUDER, 'MeshRenderer', mesh(cast=False, receive=False))])
        cast_off, _ = self.capture('caster-disabled', PROFILE, reason='active')
        check(cast_off == baseline, 'Non-casting surface still made a contact shadow')
        self.edit([component(OCCLUDER, 'MeshRenderer', mesh(cast=True, receive=False)),
                   component(RECEIVER, 'MeshRenderer', mesh(cast=False, receive=False))])
        receive_off, _ = self.capture('receiver-disabled', PROFILE, reason='active')
        check(receive_off == baseline, 'Receiver marked to ignore shadows changed appearance')

        self.edit([component(RECEIVER, 'MeshRenderer', mesh(cast=False, receive=True)),
                   component(SUN, 'Light', light((1, 0, 0), enabled=False)), self.environment()])
        disabled, dark = self.capture('light-disabled-omitted')
        self.edit([self.environment(PROFILE)])
        disabled_active, _ = self.capture('light-disabled', PROFILE, reason='light_disabled')
        check(disabled == disabled_active, 'Disabled light contact shadows changed the frame')
        self.compare(clean, active, partial, dark)
        if self.options.fsr:
            self.edit([component(SUN, 'Light', light((1, 0, 0))), self.environment()])
            before, _ = self.capture('native-aa-omitted', reconstruction='fsr3_native')
            self.edit([self.environment(PROFILE)])
            after, _ = self.capture('native-aa-enabled', PROFILE, reason='active', reconstruction='fsr3_native')
            check(before != after, 'Reconstruction output omitted the contact shadows')

    def run_checks(self):
        try:
            self.qualify()
            if self.options.runtime:
                self.relocate()
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
    options = parser.parse_args()
    check(not sys.flags.optimize, 'The shared BMP oracle requires Python assertions')
    ContactQualification(options).run_checks()


if __name__ == '__main__':
    main()
