#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify deferred Vulkan reflections with independent geometry and HDR probes."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from world_particles_contract import ParticleContract, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid


CAMERA = uid(8199)
ENVIRONMENT = uid(4)
EYE = (0, 2.2, 6)
PITCH = math.radians(-12)
LENS = dict(vertical_fov=55, near=.35, far=50)
SETTINGS = dict(strength=1, max_distance_m=9.5, thickness_m=.30)
EXPOSURE = 2
HDR_TOLERANCE = .002
FLOOR_BASE = .36
FLOOR_METALLIC = .65


@dataclass(frozen=True)
class Surface:
    entity: str
    name: str
    center: tuple[float, float, float]
    size: tuple[float, float, float]
    roughness: float
    emission: tuple[float, float, float] = (0, 0, 0)


MIRROR = Surface(uid(8200), 'Smooth slate floor', (-1.4, -.13, .9), (4, .26, 4.6), .11)
ROUGH = Surface(uid(8201), 'Rough slate floor', (1.6, -.13, .9), (2, .26, 4.6), .68)
RED = Surface(uid(8202), 'Warm emissive panel', (-1.55, 1.2, -3.2), (1.46, 1.52, .16), .8, (1, .14, .075))
BLUE = Surface(uid(8203), 'Cool emissive panel', (1.1, 1.2, -3.2), (2.15, 1.52, .16), .8, (.08, .64, 1))
SURFACES = (MIRROR, ROUGH, RED, BLUE)

# Project each interior point to a raster pixel. The oracle recomputes the
# actual hit from that pixel center, including any reconstruction jitter.
SITES = {
    'warm_hit': ((-.96, 0, .06), MIRROR, RED),
    'cool_hit': ((.29, 0, .11), MIRROR, BLUE),
    'miss': ((-2.18, 0, .25), MIRROR, None),
    'rough_guard': ((1.10, 0, .10), ROUGH, BLUE),
    'warm_target': ((-1.55, 1.12, -3.12), RED, None),
    'cool_target': ((1.10, 1.12, -3.12), BLUE, None),
    'clear': ((0, 3.10, -3.12), None, None),
}


def dot(a, b):
    return sum(x*y for x, y in zip(a, b, strict=True))


def unit(v):
    length = math.hypot(*v)
    return tuple(x/length for x in v)


def camera_matrix():
    c, s = math.cos(PITCH), math.sin(PITCH)
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, *EYE, 1]


def ray(raster_x, raster_y):
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    u, v = (raster_x-width/2)/focal, (height/2-raster_y)/focal
    matrix = camera_matrix()
    return unit(tuple(u*matrix[i]+v*matrix[4+i]-matrix[8+i] for i in range(3)))


def entry(surface, origin, direction):
    """Return the exact first intersection and outward normal of a fixture box."""
    low, high, normal = -math.inf, math.inf, None
    for axis, (p, d, center, size) in enumerate(zip(origin, direction, surface.center, surface.size, strict=True)):
        if abs(d) < 1e-12:
            if abs(p-center) > size/2:
                return None
            continue
        front, back = sorted(((center-size/2-p)/d, (center+size/2-p)/d))
        if front > low:
            low = front
            normal = tuple(-math.copysign(1, d) if i == axis else 0 for i in range(3))
        high = min(high, back)
    if not (1e-6 < low < high):
        return None
    point = tuple(p+d*low for p, d in zip(origin, direction, strict=True))
    return dict(surface=surface, distance=low, point=point, normal=normal)


def first_hit(origin, direction):
    hits = (entry(surface, origin, direction) for surface in SURFACES)
    return min((hit for hit in hits if hit is not None), key=lambda hit: hit['distance'], default=None)


def normal_margin(surface, point, normal):
    return min(size/2-abs(p-c)
               for size, p, c, n in zip(surface.size, point, surface.center, normal, strict=True) if n == 0)


def project(point):
    return projection(camera_matrix(), LENS, point)


def witness_pixels():
    width, height = CAPTURE_SIZE
    result = {label: tuple(map(math.floor, project(point))) for label, (point, _, _) in SITES.items()}
    assert len(set(result.values())) == len(result)
    assert all(2 < x < width-3 and 2 < y < height-3 for x, y in result.values()), result
    return result


def witness(name, pixel, jitter):
    _, surface, target = SITES[name]
    incoming = ray(pixel[0]+.5-jitter[0], pixel[1]+.5-jitter[1])
    receiver = first_hit(EYE, incoming)
    assert (receiver['surface'] if receiver else None) == surface, (name, receiver)
    if receiver is None:
        return dict(surface=None)
    point, normal = receiver['point'], receiver['normal']
    assert normal_margin(surface, point, normal) > .18, (name, receiver)
    result = dict(surface=surface.entity, point=point, normal=normal, ray=incoming,
                  camera_depth_m=dot(tuple(a-b for a, b in zip(point, EYE, strict=True)), camera_forward()))
    if surface in (RED, BLUE):
        assert normal == (0, 0, 1), receiver
        return result
    assert normal == (0, 1, 0), receiver
    reflected = tuple(d-2*dot(incoming, normal)*n for d, n in zip(incoming, normal, strict=True))
    hit = first_hit(point, reflected)
    assert (hit['surface'] if hit else None) == target, (name, point, reflected, hit)
    if hit is not None:
        # It is the same front face seen by the camera, and the covered
        # neighborhood extends well past one full bounded march step.
        assert hit['normal'] == (0, 0, 1) and normal_margin(target, hit['point'], hit['normal']) > .29, hit
        assert 1 < hit['distance'] < .65*SETTINGS['max_distance_m'], hit
        x, y = project(hit['point'])
        for u, v in ((x, y), (x-6, y), (x+6, y), (x, y-6), (x, y+6)):
            visible = first_hit(EYE, ray(u, v))
            assert visible is not None and visible['surface'] == target, (name, u, v, visible)
        result.update(reflected_ray=reflected, reflected_hit=target.entity,
                      reflected_distance_m=hit['distance'], reflected_point=hit['point'],
                      reflected_normal=hit['normal'], projected_target=[x, y])
    return result


def camera_forward():
    return tuple(-value for value in camera_matrix()[8:11])


def bracket_witness(geometry, target):
    """Keep the planned hit away from an ambiguous screen-depth discontinuity."""
    # The selected thickness reaches the .015 m upper bound of the ray bias.
    bias = .015
    point, normal, reflected = (geometry[key] for key in ('point', 'normal', 'reflected_ray'))
    start = tuple(p+(n+d)*bias for p, n, d in zip(point, normal, reflected, strict=True))
    end = tuple(p+d*(SETTINGS['max_distance_m']-bias) for p, d in zip(start, reflected, strict=True))
    start_uv, end_uv = project(start), project(end)
    width, height = CAPTURE_SIZE
    assert all(0 < p[0] < width and 0 < p[1] < height for p in (start_uv, end_uv))
    forward = camera_forward()
    start_z, end_z = (dot(tuple(a-b for a, b in zip(p, EYE, strict=True)), forward) for p in (start, end))

    def probe(fraction):
        q = [(1-fraction)*a+fraction*b for a, b in zip(start_uv, end_uv, strict=True)]
        hit = first_hit(EYE, ray(*(math.floor(v)+.5 for v in q)))
        if hit is None or hit['surface'] != target:
            return None
        ray_z = 1/((1-fraction)/start_z+fraction/end_z)
        target_z = dot(tuple(a-b for a, b in zip(hit['point'], EYE, strict=True)), forward)
        assert hit['normal'] == (0, 0, 1), hit
        return ray_z-target_z

    steps = 32
    assert max(abs(a-b) for a, b in zip(start_uv, end_uv, strict=True)) > steps
    depths = [probe(i/steps) for i in range(steps+1)]
    pairs = [(i, a, b) for i, (a, b) in enumerate(zip(depths, depths[1:]))
             if a is not None and b is not None and a < 0 <= b]
    assert len(pairs) == 1, pairs
    step, front, behind = pairs[0]
    low, high = step/steps, (step+1)/steps
    for _ in range(4):
        middle = (low+high)/2
        inside = probe(middle)
        assert inside is not None, middle
        if inside >= 0:
            high, behind = middle, inside
        else:
            low = middle
    assert 0 <= behind < SETTINGS['thickness_m']/4, behind
    return dict(screen_step=step, front_gap_m=front, refined_gap_m=behind)


def ideal_gain(geometry):
    normal_view = -dot(geometry['ray'], geometry['normal'])
    f0 = .04+(FLOOR_BASE-.04)*FLOOR_METALLIC
    return .5*(f0+(1-f0)*(1-normal_view)**5)*(1-MIRROR.roughness)**2


def validate_reference():
    samples = witness_pixels()
    for dx, dy in ((0, 0), (-.5, -.5), (-.5, .5), (.5, -.5), (.5, .5)):
        for name, pixel in samples.items():
            witness(name, pixel, (dx, dy))
    assert min(front.center[2]-front.size[2]/2 - (back.center[2]+back.size[2]/2)
               for front in (MIRROR, ROUGH) for back in (RED, BLUE)) > 1.5
    return {label: bracket_witness(witness(label, samples[label], (0, 0)), target)
            for label, target in (('warm_hit', RED), ('cool_hit', BLUE), ('rough_guard', BLUE))}


def surface_components(surface):
    floor = surface in (MIRROR, ROUGH)
    base = [FLOOR_BASE]*3 if floor else [0, 0, 0]
    return [
        dict(op='entity.create', id=surface.entity, name=surface.name),
        component(surface.entity, 'Transform', transform(list(surface.center)) | dict(scale=list(surface.size))),
        component(surface.entity, 'MeshRenderer', dict(primitive='box', albedo=base, visible=True,
                                                        cast_shadows=False, receive_shadows=False)),
        component(surface.entity, 'PbrMaterial', dict(base_color=base, emissive=list(surface.emission),
            metallic=FLOOR_METALLIC if floor else 0, roughness=surface.roughness, double_sided=False)),
        component(surface.entity, 'BoxCollider', dict(motion='static', half_extents=[.5]*3,
                                                       mass=1, friction=.5, restitution=0)),
    ]


class ReflectionCapture(ParticleContract):
    def transact(self, revision, request, *ops):
        reply = self.world_call('world.transact', request_id=uid(request), base_revision=revision, ops=list(ops))
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Screen Reflections')))
        revision = self.world_call('world.inspect')['revision']
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            dict(op='component.remove', id=uid(5), type='Light'),
            component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[.23]*3, exposure=1)),
            dict(op='entity.create', id=CAMERA, name='Reflection probe camera'),
            component(CAMERA, 'Transform', transform(list(EYE), [math.sin(PITCH/2), 0, 0, math.cos(PITCH/2)])),
            component(CAMERA, 'Camera', LENS),
        ]
        for surface in SURFACES:
            ops.extend(surface_components(surface))
        return self.transact(revision, 8250, *ops)

    def bundle(self, name, profile, exposure, revision, request):
        environment = dict(ambient=[.23, .23, .23], exposure=exposure)
        if profile is not None:
            environment['screen_reflections'] = profile
        revision = self.transact(revision, request, component(ENVIRONMENT, 'LightingEnvironment', environment))
        if name == 'enabled':
            self.check_runtime_requirements()
        original = self.root/(name+' original export')
        moved = self.root/(name+' standalone')
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(original),
                                   '--runtime', self.native(self.args.runtime_root))))
        original.rename(moved)
        assert not original.exists()
        self.measurements['bundles'][name] = dict(profile=profile, exposure=exposure, revision=revision,
                                                   inventory=tree_hashes(moved))
        return moved/'game.json'

    def check_runtime_requirements(self):
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info.get('needs_screen_reflections') is True, info
        for mode in ('missing', 'disabled'):
            source = self.root/f'Runtime reflection {mode}'
            source.mkdir()
            descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
            assert descriptor['features']['screen_reflections'] is True, descriptor
            if mode == 'missing':
                del descriptor['features']['screen_reflections']
            else:
                descriptor['features']['screen_reflections'] = False
            (source/'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
            target = self.root/f'Unpublished reflection {mode}'
            args = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                    '--output', self.native(target), '--runtime', self.native(source)]
            outcome = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=90)
            self.calls.append(dict(argv=args, exit=outcome.returncode, stdout=outcome.stdout, stderr=outcome.stderr))
            reply = json.loads(outcome.stdout)
            assert outcome.returncode != 0 and not target.exists(), reply
            assert [item['message'] for item in reply['diagnostics']] == [
                'Screen reflections require a compatible Vulkan runtime.'], reply

    def capture(self, client, session, family, path='deferred', view='color', reconstruction='none', reason=None):
        width, height = CAPTURE_SIZE
        name = f'{family}-{path}-{view}-{reconstruction}'
        output = self.root/(name+'.bmp')
        coords = witness_pixels()
        metrics = self.measurements['bundles'][family]
        report = client.call('runtime.capture', dict(session_id=session, tick=0, structure_revision=0,
            camera=CAMERA, path=self.native(output), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=path, reconstruction=reconstruction, capture_frames=2, profile=True,
            scene_debug_view=view, scene_product_probes=[dict(x=x, y=y) for x, y in coords.values()]))
        self.measurements['reports'][name] = report
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] == 2, report
        assert report['revision'] == metrics['revision'] and report['tick'] == 0 and report['lens'] == LENS, report
        assert report['nvrhi_errors'] == 0 and (report['width'], report['height'], report['samples']) == (width, height, 1), report
        near_vector(report['camera_world'], camera_matrix(), tolerance=1e-8)
        diag = report['render_diagnostics']
        assert diag['lighting_path'] == path, diag
        settings = diag.get('screen_reflections')
        if reason is None:
            assert settings is None, settings
        else:
            assert settings['active'] is (reason == 'active') and settings['reason'] == reason, settings
            # R16 roughness plus RGBA16F composed color at the opaque extent.
            assert settings['buffer_bytes'] == (width*height*10 if settings['active'] else 0), settings
        temporal = reconstruction != 'none'
        fsr = diag['reconstruction']
        assert fsr['active'] is temporal, fsr
        jitter = fsr['jitter_pixels'] if temporal else (0, 0)
        if temporal:
            assert (fsr['render_width'], fsr['render_height']) == CAPTURE_SIZE, fsr
            assert fsr['history_sequence'] == 2 and fsr['history_reset'] is False, fsr
        assert len(jitter) == 2 and all(abs(value) <= .5 for value in jitter), jitter
        products = diag['scene_products']
        assert products['available'] and products['view'] == view, products
        image = pixels(output)
        assert len(image) == height and all(len(row) == width for row in image)
        record = dict(gpu=report['gpu'], raster_view=view, sha256=file_hash(output), probes={})
        self.measurements['captures'][name] = record
        for (key, (x, y)), probe in zip(coords.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            expected = witness(key, (x, y), jitter)
            item = record['probes'][key] = dict(projective=expected, raster=probe, rgb=image[y][x])
            assert probe['surface_valid'] is bool(expected['surface']), item
            if expected['surface']:
                n, f = LENS['near'], LENS['far']
                camera_depth = n/(1-probe['depth']*(f-n)/f)
                assert abs(camera_depth-expected['camera_depth_m']) < .0003, item
                near_vector(probe['shading_normal'][:3], expected['normal'], tolerance=.002)
                hit = client.call('runtime.raycast', dict(session_id=session, tick=0, origin=list(EYE),
                    direction=list(expected['ray']), distance=LENS['far']))['hit']
                assert hit['entity'] == expected['surface'], (key, hit, expected)
                near_vector(hit['position'], expected['point'], tolerance=.0001)
                near_vector(hit['normal'], expected['normal'], tolerance=.002)
                if view == 'color' and expected['surface'] in (RED.entity, BLUE.entity):
                    e = RED.emission if expected['surface'] == RED.entity else BLUE.emission
                    near_vector(probe['raw_hdr'], e, tolerance=HDR_TOLERANCE)
            if not temporal and (reason != 'active' or key not in ('warm_hit', 'cool_hit')):
                assert exact(probe['resolved_hdr'], probe['raw_hdr']), item
            if view == 'color' and not temporal:
                intended = tuple(display(value, metrics['exposure']) for value in probe['resolved_hdr'])
                assert max(abs(a-b) for a, b in zip(item['rgb'], intended, strict=True)) <= 2, item
        return record

    def exercise(self, family, game):
        saves = self.root/(family+' saves')
        saves.mkdir()
        variants = [('deferred', 'color', 'none', None if family == 'omitted' else
                     ('zero_strength' if family == 'zero' else 'active'))]
        if family in ('enabled', 'omitted'):
            variants += [('forward', 'color', 'none', 'forward' if family == 'enabled' else None),
                         ('deferred', 'depth', 'none', 'diagnostic' if family == 'enabled' else None)]
            if self.args.fsr:
                variants.append(('deferred', 'color', 'fsr3_native', 'reconstruction' if family == 'enabled' else None))
        with self.owner(f'{family} relocated screen reflections', game=game, saves=saves) as (client, _):
            status = client.call('runtime.status')
            assert status['active'] is True and status['tick'] == 0, status
            session = status['session_id']
            return {(path, view, rec): self.capture(client, session, family, path, view, rec, reason)
                    for path, view, rec, reason in variants}

    def compare(self, frames):
        primary = ('deferred', 'color', 'none')
        baseline = frames['omitted'][primary]
        assert exact(baseline, frames['zero'][primary]), 'Zero strength changed the original deferred output.'
        assert exact(baseline, frames['short'][primary]), 'A trace range below the nearest surface gap changed the output.'
        for key in frames['enabled']:
            if key != primary:
                assert exact(frames['enabled'][key], frames['omitted'][key]), key
        enabled, bright = frames['enabled'][primary], frames['bright'][primary]
        assert exact(enabled['gpu'], bright['gpu'])
        assert enabled['sha256'] != baseline['sha256'] and bright['sha256'] != enabled['sha256']
        assert exact({label: p['raster'] for label, p in enabled['probes'].items()},
                     {label: p['raster'] for label, p in bright['probes'].items()})
        colors = {}
        for label, p in enabled['probes'].items():
            before = baseline['probes'][label]
            raw, composed = p['raster']['raw_hdr'], p['raster']['resolved_hdr']
            assert exact({k: v for k, v in p['raster'].items() if k != 'resolved_hdr'},
                         {k: v for k, v in before['raster'].items() if k != 'resolved_hdr'}), label
            if label not in ('warm_hit', 'cool_hit'):
                assert exact(before, p), label
                continue
            delta = tuple(after-start for after, start in zip(composed, raw, strict=True))
            target = RED.emission if label == 'warm_hit' else BLUE.emission
            dominant = max(range(3), key=target.__getitem__)
            assert delta[dominant] > .08, (label, delta)
            gain = delta[dominant]/target[dominant]
            near_vector(delta, tuple(c*gain for c in target), tolerance=.007)
            expected_gain = ideal_gain(p['projective'])
            assert abs(gain-expected_gain) < .005, (label, gain, expected_gain)
            assert bright['probes'][label]['rgb'][dominant] > p['rgb'][dominant]+8, label
            colors[label] = dict(delta_hdr=delta, gain=gain, expected_gain=expected_gain)
        self.measurements['comparisons'] = dict(exact_fallbacks=[list(mode) for mode in frames['enabled'] if mode != primary],
            zero_exact=True, short_exact=True, same_pre_exposure_products=True, target_colors=colors)

    def run(self):
        self.measurements.update(bundles={}, reports={}, captures={}, bracket_reference=validate_reference())
        revision = self.author()
        profiles = (
            ('omitted', None, 1), ('zero', SETTINGS | dict(strength=0), 1),
            ('enabled', SETTINGS, 1), ('short', SETTINGS | dict(max_distance_m=.25), 1),
            ('bright', SETTINGS, EXPOSURE),
        )
        games = {}
        for i, (name, profile, exposure) in enumerate(profiles):
            game = self.bundle(name, profile, exposure, revision, 8260+i)
            revision = self.measurements['bundles'][name]['revision']
            games[name] = game
        source = self.root/'Detached Reflection Authoring'
        self.project.rename(source)
        source_identity = tree_hashes(source)
        assert not self.project.exists()
        captures = {}
        for family, game in games.items():
            descriptor = json.loads(game.read_bytes())
            executable = game.parent/descriptor['engine']['executable']
            inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
            assert inspected.get('needs_screen_reflections', False) is (family != 'omitted'), inspected
            assert ('needs_screen_reflections' in inspected) is (family != 'omitted'), inspected
            captures[family] = self.exercise(family, game)
            assert tree_hashes(game.parent) == self.measurements['bundles'][family]['inventory']
            assert not self.project.exists() and tree_hashes(source) == source_identity
        self.measurements['detached_source'] = source_identity
        for call in self.calls:
            logs = call.get('wire', {})
            stderr = logs.get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in ('VUID-', 'Validation Error', 'Validation Warning',
                                                             'SYNC-HAZARD', 'Undefined-Value')), stderr
        self.compare(captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True, help='Installed Windows runtime used for export.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--fsr', action='store_true', help='Qualify native-resolution reconstruction fallback too.')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='screen-reflections-', dir=args.output.resolve()))
    test = ReflectionCapture(args, root)
    record = dict(format='poima.screen-reflections-capture.v1', passed=False, gpu=args.gpu, fsr=args.fsr,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
