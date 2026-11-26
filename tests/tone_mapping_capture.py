#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Witness the authored SDR display transform on the native Vulkan output."""
import argparse
from copy import deepcopy
import json
import math
from pathlib import Path
import traceback
import uuid

from hdr_composition_capture import check, component, encode
from output_grade_capture import GRADE, GradeQualification, grade_color
from project_post_effect_capture import (
    BLOOM, CAMERA, CONTROLLER, HEIGHT, SKY, WIDTH, bilinear, bloom_grid,
    difference, fixture_environment, fixture_operations, sha256, ui_operation, verify_image,
)

MODES = ('reinhard', 'agx', 'pbr_neutral')


def transform(color, mode):
    if mode == 'reinhard':
        return [value / (1 + value) for value in color]
    if mode == 'pbr_neutral':
        low = min(color)
        offset = low - low * low / .16 if low <= .08 else .04
        peak = max(color) - offset
        if peak <= .76:
            return [value - offset for value in color]
        shoulder = 1 - .24**2 / (peak - .52)
        desaturation = 1 - 1 / (1 + .15 * (peak - shoulder))
        return [shoulder * (desaturation + (1-desaturation) * (value-offset)/peak)
                for value in color]

    def from_basis(basis, values):
        return [sum(vector[channel] * value for vector, value in zip(basis, values))
                for channel in range(3)]

    bt2020 = from_basis(((.6274, .0691, .0164), (.3293, .9195, .0880), (.0433, .0113, .8956)), color)
    log_input = from_basis(((.8566271533, .1373189729, .1118982130),
                            (.0951212405, .7612419906, .0767994186),
                            (.0482516061, .1014390365, .8113023684)), bt2020)
    encoded = [min(1., max(0., (math.log2(max(value, 1e-10)) + 12.47393) / 16.5))
               for value in log_input]
    film = [sum(coefficient * sample**power for power, coefficient in enumerate(
        (-.00232, .1191, .4298, -6.868, 31.96, -40.14, 15.5))) for sample in encoded]
    linear = [max(0., value)**2.2 for value in from_basis((
        (1.1271005818, -.1413297635, -.1413297635),
        (-.1106066431, 1.1578237022, -.1106066431),
        (-.0164939387, -.0164939387, 1.2519364066)), film)]
    return [min(1., max(0., value)) for value in from_basis((
        (1.6605, -.1246, -.0182), (-.5876, 1.1329, -.1006), (-.0728, -.0083, 1.1187)), linear)]


def expected_rgb(color, mode):
    return tuple(encode(value) for value in transform(color, mode))


class ToneQualification(GradeQualification):
    def __init__(self, options):
        super().__init__(options)
        path = Path(__file__)
        self.report['test_sha256'] = sha256(path.read_bytes())
        self.report['oracle_helpers_sha256']['output_grade_capture.py'] = sha256(
            path.with_name('output_grade_capture.py').read_bytes())

    def select(self, mode=None, color=SKY, exposure=1, graded=False):
        value = fixture_environment(bloom=graded, exposure=exposure)
        for name in ('zenith', 'horizon', 'ground'):
            value['sky'][name] = list(color)
        if mode is not None:
            value['tone_mapper'] = mode
        if graded:
            value['output_grade'] = GRADE
        self.edit([component(2, 'LightingEnvironment', value)])

    def capture_tone(self, label, tone=None, **kwargs):
        data, image = self.capture_grade(label, **kwargs)
        properties = self.report['images'][label]['render']['lighting']
        check(properties.get('tone_mapper') == tone, properties)
        return data, image

    def authoring(self):
        description, discover = self.rpc(
            ('world.describe', {'view': 'component', 'name': 'LightingEnvironment'}),
            ('world.lighting', {'revision': self.revision}),
        )
        schema = description['components']['LightingEnvironment']['properties']['tone_mapper']
        check(schema['enum'] == list(MODES) and schema['default'] == 'reinhard'
              and 'tone_mapper' not in discover, (schema, discover))
        self.select('agx')
        prior = self.world.read_bytes()
        invalid_values = ('filmic', 'AgX', 1, False, None, {})
        calls = [('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': self.revision,
                  'ops': [component(2, 'LightingEnvironment',
                                    {**fixture_environment(), 'tone_mapper': invalid})]})
                 for invalid in invalid_values]
        errors = self.rpc(*calls, expected_errors={index: -32602 for index in range(len(calls))})
        lighting, dependencies = self.rpc(('world.lighting', {'revision': self.revision}),
                                          ('world.dependencies', {}))
        check(prior == self.world.read_bytes() and lighting['tone_mapper'] == 'agx'
              and dependencies['needs_tone_mapping'] is True, 'Invalid selector changed the world')
        self.report['checks']['invalid_selectors'] = errors

    def color(self):
        self.select()
        baseline, _ = self.capture_tone('default')
        raw = self.probe_values('default')
        if self.options.reference:
            old = self.options.reference.resolve(strict=True)
            check(self.cli(old, 'version')['version'] == '0.0.454', 'Expected the 0.0.454 reference')
            retained, _ = self.capture_tone('retained', binary=old)
            check(baseline == retained, 'Default tone mapping changed 0.0.454 BMP bytes')
            self.report['reference_sha256'] = sha256(old.read_bytes())
        self.select('reinhard')
        explicit, _ = self.capture_tone('explicit', 'reinhard')
        check(baseline == explicit and self.probe_values('explicit') == raw, 'Legacy path lost exact equality')
        self.report['checks']['explicit_matches_default'] = True

        comparisons = self.report['checks']['colors'] = {}
        colors = [('midtone', SKY, 1), ('warm', (1., .375, .0625), 3),
                  ('white', (1., 1., 1.), 8), ('toe', (.0625, .03125, .015625), .75),
                  ('black', SKY, 0)]
        for tag, source, exposure in colors:
            for mode in MODES[1:]:
                label = f'{mode}-{tag}'
                self.select(mode, source, exposure)
                data, image = self.capture_tone(label, mode)
                target = expected_rgb([value * exposure for value in source], mode)
                verify_image(image, [[target] * WIDTH] * HEIGHT, comparisons.setdefault(label, {}))
                if tag == 'midtone':
                    check(data != baseline and self.probe_values(label) == raw, 'HDR changed or display did not')
        self.report['checks']['upstream_exact'] = True

        self.select('agx', exposure=1.375, graded=True)
        _, image = self.capture_tone('bloom-graded-agx', 'agx', grade=GRADE)
        halo = bloom_grid(False)
        rows, wrong_rows = [], []
        for y in range(HEIGHT):
            row, wrong_row = [], []
            for x in range(WIDTH):
                lit = [base + glow * BLOOM['intensity']
                       for base, glow in zip(SKY, bilinear(halo, x, y))]
                exposed = [value * 1.375 for value in lit]
                row.append(expected_rgb(grade_color(exposed), 'agx'))
                wrong_row.append(tuple(encode(min(1., value)) for value in grade_color(transform(exposed, 'agx'))))
            rows.append(row)
            wrong_rows.append(wrong_row)
        verify_image(image, rows, comparisons.setdefault('bloom-graded-agx', {}), wrong=wrong_rows)

    def reconstruction_and_hud(self):
        for reconstruction in self.options.reconstruction:
            self.select()
            self.capture_tone(f'{reconstruction}-default', mode=reconstruction)
            self.select('agx')
            _, image = self.capture_tone(f'{reconstruction}-agx', 'agx', mode=reconstruction)
            check(self.probe_values(f'{reconstruction}-default') == self.probe_values(f'{reconstruction}-agx'),
                  'FSR HDR changed')
            expected = expected_rgb(SKY, 'agx')
            values = [image[HEIGHT*y//8][WIDTH*x//8] for x in (1, 3, 5, 7) for y in (1, 3, 5, 7)]
            error = max(difference(value, expected) for value in values)
            check(error <= 2, (values, expected))
            self.report['checks'][reconstruction] = {'maximum_rgb_error': error, 'checked_pixels': len(values)}
        if self.options.check_ui:
            self.edit([ui_operation()])
            patches = []
            for mode in MODES:
                self.select(mode, exposure=0 if mode == 'pbr_neutral' else 1)
                _, image = self.capture_tone(f'hud-{mode}', mode, runtime=True)
                patches.append([image[y][x] for y in range(24, 29) for x in range(25, 30)])
            check(patches[0] == patches[1] == patches[2]
                  and all(difference(pixel, (84, 162, 219)) <= 2 for pixel in patches[0]),
                  'Tone mapping changed the authored HUD')
            self.report['checks']['hud_pixels_exact'] = len(patches[0])

    def export(self):
        self.select('agx')
        manifest = self.source / 'project.poimaproject'
        manifest.write_text(json.dumps({'format': 'poima.project', 'version': 1,
            'project_id': uuid.uuid4().hex, 'name': 'Tone mapping fixture', 'audio': False,
            'startup_splash': False, 'entry': {'world': self.world.name, 'camera': CAMERA,
                                                 'controller': CONTROLLER}}) + '\n', encoding='utf-8')
        check(self.cli(self.binary, 'project', 'inspect', self.native(manifest))['needs_tone_mapping'],
              'Project did not retain the runtime requirement')
        runtime = self.options.runtime.resolve(strict=True)
        descriptor = json.loads((runtime / 'runtime.json').read_bytes())
        check(descriptor['features']['tone_mapping'] is True, 'Installed runtime lacks tone mapping')
        unsupported = self.run / 'Incompatible runtime'
        unsupported.mkdir()
        missing = deepcopy(descriptor)
        del missing['features']['tone_mapping']
        (unsupported / 'runtime.json').write_text(json.dumps(missing), encoding='utf-8')
        build = [self.binary, 'project', 'build', self.native(manifest)]
        refused = self.run / 'Unpublished'
        rejection = self.invoke_refusal([*build, '--output', self.native(refused),
                                         '--runtime', self.native(unsupported)])
        check(not refused.exists() and [error['message'] for error in rejection['diagnostics']]
              == ['Authored tone mapping requires a compatible Vulkan runtime.'], rejection)
        initial = self.run / 'Initial'
        payload = self.run / 'Relocated'
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(initial),
                 '--runtime', self.native(runtime))
        initial.rename(payload)
        detached = self.run / 'Unavailable source'
        self.source.rename(detached)
        try:
            game = payload / 'game.json'
            bundle = json.loads(game.read_bytes())
            engine = payload / bundle['engine']['executable']
            world = payload / bundle['entry']['world']
            frozen = world.read_bytes()
            check(self.cli(engine, 'game', 'inspect', self.native(game))['needs_tone_mapping'],
                  'Relocated game lost the required runtime feature')
            replay = self.run / 'Replay.json'
            replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
            path = self.run / 'Relocated.bmp'
            played = self.cli(engine, 'game', 'run', self.native(game), '--gpu', self.options.gpu,
                             '--replay', self.native(replay), '--capture', self.native(path),
                             '--width', WIDTH, '--height', HEIGHT, '--samples', 1, cwd=self.run)['play']
            self.require_validation()
            check(not self.source.exists() and played['lighting']['tone_mapper'] == 'agx'
                  and played['tick'] == 1 and played['stop_reason'] == 'replay_complete'
                  and world.read_bytes() == frozen, played)
            _, image = self.image('relocated', path, played, 1)
            target = expected_rgb(SKY, 'agx')
            records = self.report['checks']['export'] = {'rejected': rejection, 'source_unavailable': True}
            verify_image(image, [[target] * WIDTH] * HEIGHT, records)
        finally:
            detached.rename(self.source)

    def run_checks(self):
        evidence = self.run / 'report.json'
        try:
            self.edit(fixture_operations())
            self.authoring()
            if not self.options.authoring_only:
                self.color()
                if self.options.runtime:
                    self.export()
                self.reconstruction_and_hud()
            self.report['passed'] = True
        except BaseException:
            self.report['failure'] = traceback.format_exc()
            raise
        finally:
            evidence.write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')
            print(f'Evidence: {evidence}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--expected-gpu')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--reconstruction', choices=('fsr3_native',), action='append', default=[])
    parser.add_argument('--check-ui', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--authoring-only', action='store_true')
    parser.set_defaults(samples=1)
    ToneQualification(parser.parse_args()).run_checks()


if __name__ == '__main__':
    main()
