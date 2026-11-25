#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify refraction against independent Snell rays, foreground guards and cold exports."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from height_fog_capture import environment
from water_optics_capture import AMBIENT, OPTICS, fresnel
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, rpc, success, uid


SURFACE = 6.15
CAMERAS = {
    'air': (uid(6601), (11.55, 7.20, -1.75), -45),
    'water': (uid(6602), (11.55, 5.75, -2.45), 54),
}
LENS = dict(vertical_fov=50, near=.05, far=40)
PROBES = [(x, y) for y in range(38, 330, 19) for x in (304, 337, 376)]
FLOOR, CANOPY = 5.4, 7.6
STRIPE_ORIGIN, STRIPE_WIDTH, STRIPES = -5.75, .23, 20
PALETTE = ((.84, .035, .07), (.022, .65, .08), (.065, .10, .86), (.88, .53, .024))
GUARD_COLOR = (.77, .026, .58)


def add(a, b, scale):
    return tuple(x + scale * y for x, y in zip(a, b, strict=True))


def direction(pixel, angle):
    width, height = CAPTURE_SIZE
    focal = height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    x, y = (pixel[0] + .5 - width / 2) / focal, (height / 2 - pixel[1] - .5) / focal
    cosine, sine = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    ray = (x, y * cosine + sine, y * sine - cosine)
    length = math.hypot(*ray)
    return tuple(value / length for value in ray)


def project(eye, angle, position):
    x, y, z = (p - e for p, e in zip(position, eye, strict=True))
    cosine, sine = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    up, forward = y * cosine + z * sine, y * sine - z * cosine
    if forward <= 0:
        return None
    width, height = CAPTURE_SIZE
    focal = height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    return width / 2 + focal * x / forward, height / 2 - focal * up / forward


def palette(position):
    offset = (position[2] - STRIPE_ORIGIN) / STRIPE_WIDTH
    stripe = math.floor(offset)
    if not (10.12 < position[0] < 13.88 and 0 <= stripe < STRIPES):
        return None
    fraction = offset - stripe
    return PALETTE[stripe % len(PALETTE)] if .16 < fraction < .84 else None


def expected_color(source, water_length_m, reflectance, before_m, entering):
    result = []
    for index, (coefficient, scattered) in enumerate(zip(OPTICS['extinction_per_m'], OPTICS['scattered_radiance'], strict=True)):
        reflection = AMBIENT[index] if entering else scattered
        transmitted = scattered + (source[index] - scattered) * math.exp(-water_length_m * coefficient)
        reflected = scattered + (reflection - scattered) * math.exp(-before_m * coefficient)
        result.append(transmitted * (1 - reflectance) + reflected * reflectance)
    return result


def witness(camera, pixel):
    _, eye, angle = CAMERAS[camera]
    entering = camera == 'air'
    ray = direction(pixel, angle)
    if ray[1] * (-1 if entering else 1) <= .01:
        return None
    entry_distance = (SURFACE - eye[1]) / ray[1]
    entry = add(eye, ray, entry_distance)
    if not (10.02 < entry[0] < 13.98 and -3.97 < entry[2] < -2.04):
        return None
    height = FLOOR if entering else CANOPY
    straight_m = (height - eye[1]) / ray[1]
    straight = add(eye, ray, straight_m)
    raw_color = palette(straight)
    if raw_color is None:
        return None
    reflection = fresnel(ray[1])
    before = 0 if entering else entry_distance
    if reflection == 1:
        return dict(tir=True, raw=raw_color, expected=expected_color(raw_color, before, reflection, before, entering))
    ratio = 1 / 1.333 if entering else 1.333
    tangent_sq = ratio**2 * (ray[0]**2 + ray[2]**2)
    if tangent_sq > .98:
        return None
    transmitted = (ratio * ray[0], math.copysign(math.sqrt(1 - tangent_sq), ray[1]), ratio * ray[2])
    bent_m = (height - SURFACE) / transmitted[1]
    target = add(entry, transmitted, bent_m)
    if entering and not (10.03 < target[0] < 13.97 and -3.96 < target[2] < -2.03):
        return None
    target_pixel = project(eye, angle, target)
    if target_pixel is None or not (12 < target_pixel[0] < CAPTURE_SIZE[0] - 12 and 12 < target_pixel[1] < CAPTURE_SIZE[1] - 12):
        return None
    bent_color = palette(target)
    if bent_color is None or raw_color == bent_color:
        return None
    total = before + (bent_m if entering else 0)
    return dict(tir=False, raw=raw_color, bent=bent_color,
                expected=expected_color(bent_color, total, reflection, before, entering),
                hit_pixel=tuple(math.floor(value) for value in target_pixel), entry_m=entry_distance, bent_m=bent_m,
                target=target)


def panel(entity, name, center, size, emission):
    return [dict(op='entity.create', id=entity, name=name),
            component(entity, 'Transform', transform(center) | dict(scale=size)),
            component(entity, 'MeshRenderer', dict(primitive='box', visible=True,
                cast_shadows=False, receive_shadows=False, albedo=[0, 0, 0])),
            component(entity, 'PbrMaterial', dict(base_color=[0, 0, 0], metallic=1, roughness=.9,
                double_sided=False, emissive=emission))]


class RefractionCapture(WaterContract):
    owner = ParticleContract.owner

    def author(self):
        recipe, _, revision = self.arm_terrain()
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(1), type='BoxCollider'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', environment() | dict(ambient=AMBIENT))]
        for label, (camera, eye, angle) in CAMERAS.items():
            half_angle = math.radians(angle) / 2
            ops.extend([dict(op='entity.create', id=camera, name=label + ' camera'),
                        component(camera, 'Transform', transform(eye, [math.sin(half_angle), 0, 0, math.cos(half_angle)])),
                        component(camera, 'Camera', LENS)])
        for face, height in enumerate((FLOOR - .02, CANOPY + .02)):
            for stripe in range(STRIPES):
                center_z = STRIPE_ORIGIN + STRIPE_WIDTH * (stripe + .5)
                ops.extend(panel(uid(6610 + face * STRIPES + stripe), f'Color witness {face}-{stripe}',
                    (12, height, center_z), (4, .04, STRIPE_WIDTH), PALETTE[stripe % len(PALETTE)]))
        revision = success(self.world_calls(self.transaction(revision, 6660, *ops))[0])['revision']
        self.parameters = dict(terrain=dict(source=recipe.name), tile_entities=TILES,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4,
                      relaxation_per_second=0),
            initial_depths_m=[SURFACE - bed(*cell_center(cell)) for cell in CELLS])
        return revision

    def cook(self, revision, optics):
        params = self.parameters | dict(revision=revision, optics=optics)
        plan = self.world_call('world.water.plan', **params)
        pin = plan['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=pin) == plan
        path = self.assets / (pin + '.pwater')
        package = json.loads(path.read_bytes())
        refracted = optics.get('refraction', False)
        assert package['version'] == (7 if refracted else 6) and file_hash(path) == pin
        assert package['optics'] == (OPTICS | {'refraction': True} if refracted else OPTICS)
        return pin

    def capture(self, client, session, tick, label, camera, probes, enabled, *, temporal=False):
        output = self.root / (label + '.bmp')
        response = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=CAMERAS[camera][0],
            path=self.native(output), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1], gpu=self.args.gpu, samples=1,
            lighting_path=self.args.lighting_path, profile=True, capture_frames=2,
            reconstruction='fsr3_native' if temporal else 'none',
            scene_product_probes=[dict(x=x, y=y) for x, y in probes]))
        assert response['hardware'] and response['capture_written'] and response['nvrhi_errors'] == 0
        assert response['frames_presented'] == 2 and response['tick'] == tick
        diagnostics = response['render_diagnostics']
        assert diagnostics['water_optics']['active'] and diagnostics['water_optics'].get('refraction', False) is enabled
        assert diagnostics['reconstruction']['active'] is temporal
        self.measurements[label] = dict(report=response, sha256=file_hash(output))
        return diagnostics['scene_products']['probes']

    def bind(self, revision, pin, request, *ops):
        return success(self.world_calls(self.transaction(revision, request, dict(op='water.set', asset=pin), *ops))[0])['revision']

    def pair(self, revision, tag, enabled, probes=PROBES, cameras=CAMERAS, prefix=6670):
        with self.owner(tag) as (client, _):
            session = uid(prefix)
            client.call('runtime.start', self.start(session, revision)['params'])
            return {camera: self.capture(client, session, 0, f'{tag}-{camera}', camera, probes, enabled)
                    for camera in cameras}

    def check_images(self, straight, bent):
        evidence = {}
        for name in CAMERAS:
            selected = []
            for pixel, old, new in zip(PROBES, straight[name], bent[name], strict=True):
                assert old['raw_hdr'] == new['raw_hdr'] and old['depth'] == new['depth'], pixel
                model = witness(name, pixel)
                if model is None:
                    continue
                near_vector(old['raw_hdr'], model['raw'], tolerance=.002)
                near_vector(new['resolved_hdr'], model['expected'], tolerance=.003)
                if model['tir']:
                    assert old['resolved_hdr'] == new['resolved_hdr']
                else:
                    assert math.dist(old['resolved_hdr'], new['resolved_hdr']) > .1
                selected.append(model | dict(pixel=pixel, result=new['resolved_hdr']))
            assert sum(not model['tir'] for model in selected) >= 3, selected
            if name == 'water':
                assert sum(model['tir'] for model in selected) >= 3, selected
            evidence[name] = selected
        self.measurements['oracle'] = evidence

    def foreground(self, revision, straight, bent):
        pixel, model = next((pixel, model) for model in self.measurements['oracle']['air']
                            if not model['tir'] for pixel in [model['pixel']]
                            if 130 < pixel[1] < 260)
        _, eye, _ = CAMERAS['air']
        target = model['target']
        marker_y = (SURFACE + eye[1]) * .5
        along = (marker_y - eye[1]) / (target[1] - eye[1])
        center = add(eye, tuple(b-a for a, b in zip(eye, target, strict=True)), along)
        ops = panel(uid(6690), 'Dry magenta marker beside entry ray', center, (.10, .035, .16), GUARD_COLOR)
        pixels = [pixel, model['hit_pixel']]
        revision = self.bind(revision, straight, 6691, *ops)
        without = self.pair(revision, 'blocked-straight', False, pixels, ['air'], prefix=6692)['air']
        revision = self.bind(revision, bent, 6693)
        with_refraction = self.pair(revision, 'blocked-bent', True, pixels, ['air'], prefix=6694)['air']
        near_vector(with_refraction[1]['raw_hdr'], GUARD_COLOR, tolerance=.002)
        assert with_refraction[1]['resolved_hdr'] == with_refraction[1]['raw_hdr']
        assert with_refraction[0]['raw_hdr'] == without[0]['raw_hdr']
        assert with_refraction[0]['resolved_hdr'] == without[0]['resolved_hdr']
        self.measurements['guard'] = dict(witness=pixel, blocked_color_pixel=model['hit_pixel'])

    def relocated(self, revision, pin):
        runtime = self.args.runtime_root
        manifest = json.loads((runtime/'runtime.json').read_bytes())
        assert manifest['features']['water_refraction']
        old_features = self.root/'missing feature runtime'
        old_features.mkdir()
        del manifest['features']['water_refraction']
        (old_features/'runtime.json').write_text(json.dumps(manifest), encoding='utf-8')
        output = self.root/'original bundle'
        args = ['project', 'build', self.native(self.manifest), '--output', self.native(output), '--runtime']
        refused = subprocess.run([str(self.args.binary.resolve()), *args, self.native(old_features)],
                                 capture_output=True, text=True, timeout=60)
        assert refused.returncode == 4 and not output.exists(), refused
        assert json.loads(refused.stdout)['diagnostics'][0]['message'] == 'Water refraction requires a compatible Vulkan runtime.'
        self.measurements['admission_rejection'] = json.loads(refused.stdout)['diagnostics'][0]['message']
        success(json.loads(self.cli(*args, self.native(runtime))))
        moved = self.root/'relocated bundle'
        output.rename(moved)
        parked = self.root/'parked source'
        self.project.rename(parked)
        try:
            game = moved/'game.json'
            executable = moved/json.loads(game.read_bytes())['engine']['executable']
            assert success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))['needs_water_refraction']
            package, = moved.rglob('*.pwater')
            assert file_hash(package) == pin
            saves = self.root/'player saves'
            saves.mkdir()
            with self.owner('relocated refraction', game=game, saves=saves) as (client, _):
                status = client.call('runtime.status')
                assert status['authored_revision'] == revision and status['tick'] == 0
                self.capture(client, status['session_id'], 0, 'relocated-air', 'air', PROBES, True)
            assert self.measurements['relocated-air']['sha256'] == self.measurements['bent-air']['sha256']
        finally:
            parked.rename(self.project)

    def wave_phase(self, revision):
        self.parameters['surface_wave_layers'] = [
            dict(wavelength_m=.63, slope=.14, period_ticks=37, heading_degrees=30),
            dict(wavelength_m=.38, slope=.12, period_ticks=29, heading_degrees=-70)]
        flat_ray = self.cook(revision, OPTICS)
        bent_ray = self.cook(revision, OPTICS | {'refraction': True})
        captures = {}
        for name, pin, enabled in [('straight', flat_ray, False), ('refracted', bent_ray, True)]:
            revision = self.bind(revision, pin, 6700 + int(enabled))
            with self.owner('wave ' + name) as (client, _):
                session = uid(6702 + int(enabled))
                client.call('runtime.start', self.start(session, revision)['params'])
                captures[0, name] = self.capture(client, session, 0, 'waves-' + name + '-0', 'air', PROBES, enabled)
                stepped = client.call('runtime.step', self.step(session, 0, 8, 6704 + int(enabled))['params'])
                assert stepped['current_tick'] == 8
                captures[8, name] = self.capture(client, session, 8, 'waves-' + name + '-8', 'air', PROBES, enabled)
        displacement = []
        for tick in (0, 8):
            before = captures[tick, 'straight']
            after = captures[tick, 'refracted']
            assert all(a['raw_hdr'] == b['raw_hdr'] and a['depth'] == b['depth']
                       for a, b in zip(before, after, strict=True))
            observed = [math.dist(a['resolved_hdr'], b['resolved_hdr']) for a, b in zip(before, after, strict=True)]
            assert sum(value > .1 for value in observed) >= 3, observed
            displacement.append(observed)
        assert sum(abs(a - b) > .08 for a, b in zip(*displacement, strict=True)) >= 3
        self.measurements['wave_hue_shifts'] = displacement
        return revision

    def run(self):
        revision = self.author()
        self.cases(('bad_flag', rpc('world.water.plan', **(self.parameters | dict(revision=revision,
            optics=OPTICS | {'refraction': 1}))), -32602))
        normal = self.cook(revision, OPTICS)
        assert self.cook(revision, OPTICS | {'refraction': False}) == normal
        optical = self.cook(revision, OPTICS | {'refraction': True})
        assert optical != normal
        revision = self.bind(revision, normal, 6661)
        before = self.pair(revision, 'straight', False)
        revision = self.bind(revision, optical, 6662)
        assert self.world_call('world.dependencies')['needs_water_refraction']
        after = self.pair(revision, 'bent', True)
        self.check_images(before, after)
        if self.args.fsr:
            with self.owner('reconstruction') as (client, _):
                session = uid(6680)
                client.call('runtime.start', self.start(session, revision)['params'])
                self.capture(client, session, 0, 'fsr-air', 'air', PROBES, True, temporal=True)
        if self.args.runtime_root:
            self.relocated(revision, optical)
        if self.args.waves:
            revision = self.wave_phase(revision)
        self.foreground(revision, normal, optical)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--fsr', action='store_true')
    parser.add_argument('--waves', action='store_true', help='Also compare wave-driven refraction across two committed ticks.')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-refraction-', dir=args.output))
    test = RefractionCapture(args, root)
    report = dict(passed=False, calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        logs = [call['wire']['stderr.log'] for call in test.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD'))
                            for log in logs)
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Water refraction evidence {root / "evidence.json"}')


if __name__ == '__main__':
    main()
