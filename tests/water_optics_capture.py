#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify committed water depth and sightline optics on native Vulkan."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from atmosphere_capture import PROFILE as ATMOSPHERE
from height_fog_capture import Board, author_board, environment
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, near, rpc, success, uid


CAMERA = uid(6501)
EYE = (10.55, 6, -2.55)
LENS = dict(vertical_fov=55, near=.05, far=40)
AMBIENT = (.08, .16, .28)
OPTICS = dict(extinction_per_m=[1.15, .38, .16], scattered_radiance=[.025, .15, .25])
SURFACES = {(0, 0): 6.125, (0, 1): 5.875, (1, 0): 5.75, (2, 1): 6.05}
SITES = [(x, y) for y in (42, 100, 174, 190, 213, 258, 314) for x in (65, 210, 320, 428, 560)]
MEDIA_TOLERANCE = .002


def sightline(pixel):
    width, height = CAPTURE_SIZE
    focal = height / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
    offset = ((pixel[0] + .5 - width / 2) / focal, (height / 2 - pixel[1] - .5) / focal, -1)
    length = math.hypot(*offset)
    return tuple(value / length for value in offset), length


def intersect(eye, ray, lower, upper, distance):
    """Brute force all slabs. The shader walks only cells crossed by the ray."""
    start, end = 0, distance
    for origin, direction, low, high in zip(eye, ray, lower, upper, strict=True):
        if direction == 0:
            if not low <= origin < high:
                return None
        else:
            ends = sorted(((low - origin) / direction, (high - origin) / direction))
            start, end = max(start, ends[0]), min(end, ends[1])
    return (start, end) if start < end else None


def fresnel(ray_y):
    a, b = (1, 1.333) if ray_y < 0 else (1.333, 1)
    incident = math.acos(abs(ray_y))
    transmitted_sin = math.sin(incident) * a / b
    if transmitted_sin >= 1:
        return 1
    transmitted = math.asin(transmitted_sin)
    perpendicular = (a * math.cos(incident) - b * math.cos(transmitted)) / (
        a * math.cos(incident) + b * math.cos(transmitted))
    parallel = (b * math.cos(incident) - a * math.cos(transmitted)) / (
        b * math.cos(incident) + a * math.cos(transmitted))
    return (perpendicular**2 + parallel**2) * .5


def oracle(pixel, raw, depth, tick, *, water=True):
    ray, ray_scale = sightline(pixel)
    near_z, far_z = LENS['near'], LENS['far']
    distance = near_z / ((1 - depth) + depth * near_z / far_z) * ray_scale
    spans, tops = [], []
    for (x, z), top in SURFACES.items() if water else ():
        if (x, z) == (0, 1):
            top += tick * .25
        x0, z0 = 10 + x, -4 + z
        floor = bed(*cell_center((x, z)))
        span = intersect(EYE, ray, (x0, floor, z0), (x0 + 1, top, z0 + 1), distance)
        if span:
            spans.append(span)
        if ray[1] != 0:
            t = (top - EYE[1]) / ray[1]
            at_x, at_z = EYE[0] + ray[0] * t, EYE[2] + ray[2] * t
            if 0 <= t < distance and x0 < at_x < x0 + 1 and z0 < at_z < z0 + 1:
                tops.append(t)
    total = sum(end - start for start, end in spans)
    top = min(tops, default=None)
    reflected = fresnel(ray[1]) if top is not None else 0.0
    before = sum(max(0, min(top, end) - start) for start, end in spans) if top is not None else 0
    answer = []
    for channel, (extinction, source) in enumerate(zip(OPTICS['extinction_per_m'], OPTICS['scattered_radiance'], strict=True)):
        through = math.exp(-total * extinction)
        reflection = AMBIENT[channel] if ray[1] < 0 else source
        answer.append(source + (1-reflected)*through*(raw[channel]-source) +
                      reflected*math.exp(-before*extinction)*(reflection-source))
    return dict(hdr=answer, distance=distance, water_length_m=total, interfaces=len(tops), fresnel=reflected,
                water_spans=len(spans))


class WaterOpticsContract(WaterContract):
    owner = ParticleContract.owner

    def author(self):
        recipe, _, revision = self.arm_terrain()
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            dict(op='component.remove', id=uid(5), type='Light'),
            component(uid(4), 'LightingEnvironment', environment() | dict(ambient=AMBIENT)),
            dict(op='entity.create', id=CAMERA, name='Waterline witness'),
            component(CAMERA, 'Transform', transform(list(EYE))),
            component(CAMERA, 'Camera', LENS),
        ]
        ops += author_board(Board(uid(6502), (11.4, 6, -4.72), (4, 2.2, .2)), 'Submerged wall')
        ops += author_board(Board(uid(6503), (10.27, 6, -2.82), (.16, 1.0, .055)), 'Close occulting post')
        revision = success(self.world_calls(self.transaction(revision, 6504, *ops))[0])['revision']
        params = dict(revision=revision, terrain=dict(source=recipe.name), tile_entities=TILES,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4,
                      relaxation_per_second=0),
            initial_depths_m=[max(0, SURFACES.get(cell, 0) - bed(*cell_center(cell))) for cell in CELLS],
            exchanges=[dict(cell=[0, 1], source_m3_per_second=15, drain_m3_per_second=0)], optics=OPTICS)
        self.cases(('bad_extinction',rpc('world.water.plan',**(params | {'optics': OPTICS | {
            'extinction_per_m': [1, -1, 2]}})), -32602))
        inspected = self.world_call('world.water.plan', **params)
        pin = inspected['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=pin) == inspected
        asset = self.assets / (pin + '.pwater')
        package = json.loads(asset.read_bytes())
        assert package['version'] == 6 and package['optics'] == OPTICS and file_hash(asset) == pin
        self.water_parameters = params
        return pin, revision

    def capture(self, client, session, label, tick, *, samples=1, expect=None, reconstruction='none'):
        path = self.root / f'{label}.bmp'
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=CAMERA,
            path=self.native(path), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1], gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, samples=samples, profile=True, capture_frames=2,
            reconstruction=reconstruction, scene_product_probes=[dict(x=x, y=y) for x, y in SITES] if samples == 1 else []))
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors'] == 0, report
        assert report['frames_presented'] == 2 and report['tick'] == tick, report
        diagnostics = report['render_diagnostics']
        if expect is None:
            assert 'water_optics' not in diagnostics, diagnostics
        else:
            optics = diagnostics['water_optics']
            assert optics['active'] is (expect == 'active') and optics['reason'] == expect and optics['columns'] == 8
            near(optics['camera_submersion_m'], max(0, SURFACES[0, 1] + tick * .25 - EYE[1]), 1e-6)
        result = dict(report=report, sha256=file_hash(path))
        self.measurements[label] = result
        return result

    def advance(self, client, session, tick, request):
        assert client.call('runtime.step', self.step(session, 0, tick, request)['params'])['current_tick'] == tick

    def relocated(self, pin, revision, reference, saves):
        original, moved = self.root / 'original export', self.root / 'relocated export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output',
            self.native(original), '--runtime', self.native(self.args.runtime_root))))
        original.rename(moved)
        hidden = self.root / 'parked source'
        self.project.rename(hidden)
        try:
            game = moved / 'game.json'
            executable = moved / json.loads(game.read_bytes())['engine']['executable']
            result = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
            assert result['needs_water_optics'] is True and result['revision'] == revision, result
            assets = list(moved.rglob('*.pwater'))
            assert len(assets) == 1 and file_hash(assets[0]) == pin
            with self.owner('relocated', game=game, saves=saves) as (client, _):
                active = client.call('runtime.status')['session_id']
                restored = uid(6531)
                client.call('save.load', self.load(restored, revision, 6532, active=active, active_tick=0)['params'])
                image = self.capture(client, restored, 'relocated', 2, expect='active')
                assert image['sha256'] == reference['sha256'], image
        finally:
            hidden.rename(self.project)

    def composed_media(self, revision):
        sky = environment()
        sky['sky'].update(sun=uid(5), sun_intensity=1)
        sky['atmosphere'] = ATMOSPHERE | dict(aerial_perspective_m=128)
        sky['ibl'] = dict(enabled=True, diffuse_intensity=.2, specular_intensity=1)
        half_angle = math.radians(-35) / 2
        transaction = self.transaction(revision, 6540,
            component(uid(4), 'LightingEnvironment', sky),
            component(uid(5), 'Light', dict(kind='directional', color=[1, .92, .78], enabled=True, intensity=2)),
            component(uid(5), 'Transform', transform([0, 0, 0], [math.sin(half_angle), 0, 0, math.cos(half_angle)])))
        revision = success(self.world_calls(transaction)[0])['revision']
        params = self.water_parameters | dict(revision=revision,
            surface_wave_layers=[dict(wavelength_m=.45, slope=.21, period_ticks=48, heading_degrees=27),
                                 dict(wavelength_m=.72, slope=.15, period_ticks=63, heading_degrees=-32)])
        pin = self.world_call('world.water.plan', **params)['plan_sha256']
        self.world_call('world.water.cook', **params, expected_plan_sha256=pin)
        revision = success(self.world_calls(self.transaction(revision, 6541, dict(op='water.set', asset=pin)))[0])['revision']
        with self.owner('water and atmosphere') as (client, _):
            session = uid(6542)
            client.call('runtime.start', self.start(session, revision)['params'])
            result = self.capture(client, session, 'water-and-atmosphere', 0, expect='active')
            diagnostics = result['report']['render_diagnostics']
            assert diagnostics['aerial_perspective']['active'] and diagnostics['environment_ibl']['enabled']
            assert result['sha256'] != self.measurements['above']['sha256']

    def run(self):
        pin, revision = self.author()
        with self.owner('dry') as (client, _):
            session = uid(6521)
            client.call('runtime.start', self.start(session, revision)['params'])
            control = self.capture(client, session, 'dry', 0)
        if self.args.retained:
            compiled = self.args.binary
            try:
                self.args.binary = self.args.retained
                with self.owner('retained dry') as (client, _):
                    session = uid(6520)
                    client.call('runtime.start', self.start(session, revision)['params'])
                    retained = self.capture(client, session, 'retained-dry', 0)
                    assert retained['sha256'] == control['sha256']
            finally:
                self.args.binary = compiled
        revision = success(self.world_calls(self.transaction(revision, 6522, dict(op='water.set', asset=pin)))[0])['revision']
        assert self.world_call('world.dependencies')['needs_water_optics'] is True
        saves = self.root / 'external saves'
        saves.mkdir()
        with self.owner('water') as (client, _):
            session = uid(6523)
            client.call('runtime.start', self.start(session, revision)['params'])
            self.capture(client, session, 'above', 0, expect='active')
            if self.args.lighting_path == 'forward':
                self.capture(client, session, 'multisample', 0, samples=4, expect='multisample')
            client.call('save.configure', dict(request_id=uid(6524), expected_generation=0, root=self.native(saves)))
            self.advance(client, session, 2, 6525)
            client.call('save.write', self.write(session, 2, 'checkpoint', 6526)['params'])
            underwater = self.capture(client, session, 'under', 2, expect='active')
            if self.args.fsr:
                temporal = self.capture(client, session, 'under-fsr', 2, expect='active', reconstruction='fsr3_native')
                assert temporal['report']['render_diagnostics']['reconstruction']['active']
        with self.owner('fresh save') as (client, _):
            client.call('save.configure', dict(request_id=uid(6527), expected_generation=0, root=self.native(saves)))
            restored = uid(6528)
            client.call('save.load', self.load(restored, revision, 6529)['params'])
            state = client.call('runtime.water.inspect', self.state(restored, 2, points=[EYE])['params'])
            near(state['points'][0]['submersion_m'], .375)
            reread = self.capture(client, restored, 'restored', 2, expect='active')
            assert reread['sha256'] == underwater['sha256']
        if self.args.runtime_root:
            self.relocated(pin, revision, underwater, saves)
        for label, tick in [('above', 0), ('under', 2)]:
            baseline = control['report']['render_diagnostics']['scene_products']['probes']
            probes = self.measurements[label]['report']['render_diagnostics']['scene_products']['probes']
            references = []
            for pixel, before, after in zip(SITES, baseline, probes, strict=True):
                assert before['raw_hdr'] == after['raw_hdr'] and before['depth'] == after['depth'], (before, after)
                expected = oracle(pixel, after['raw_hdr'], after['depth'], tick)
                near_vector(after['resolved_hdr'], expected['hdr'], tolerance=MEDIA_TOLERANCE)
                if expected['water_length_m'] == 0 and expected['fresnel'] == 0:
                    assert after['resolved_hdr'] == after['raw_hdr'], after
                references.append(expected)
            assert any(row['water_length_m'] > .5 for row in references), references
            assert any(row['interfaces'] and row['fresnel'] > .01 for row in references), references
            self.measurements[label]['oracle'] = references
        if self.args.media:
            self.composed_media(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--retained', type=Path, help='Compare the dry capture with this earlier executable.')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--fsr', action='store_true')
    parser.add_argument('--media', action='store_true', help='Also render wave layers with aerial perspective and IBL.')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-optics-', dir=args.output))
    test = WaterOpticsContract(args, root)
    evidence = dict(passed=False, calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        logs = [call['wire']['stderr.log'] for call in test.calls if 'wire' in call]
        errors = ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD')
        assert logs and all(not any(error in log for error in errors) for log in logs), logs
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        (root / 'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Water optics evidence {root / "evidence.json"}')


if __name__ == '__main__':
    main()
