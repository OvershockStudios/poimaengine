#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify the exposed-HDR output grade with independent color and ownership checks."""
import argparse
from copy import deepcopy
from functools import lru_cache
import json
import math
from pathlib import Path
import sys
import traceback
import uuid

from hdr_composition_capture import check, component, display, uid
from project_post_effect_capture import (
    BLOOM, CAMERA, CONTROLLER, HEIGHT, NUMBER_TOLERANCE, SKY, WIDTH, Qualification,
    bilinear, bloom_grid, difference, fixture_environment, fixture_operations,
    scene_grid, sha256, ui_operation, verify_image,
)

INPUT_MAX = 1.25
STRENGTH = .75
# The different slopes witness each channel, interpolation and the highlight
# tail. The authored values are control points, not the expected pixel values.
KNOTS = [[1.6*t*t + .2*t, .68*t + .05*t*t, .28*t + 1.85*t*t*t]
         for t in (index/32 for index in range(33))]
GRADE = {'input_max': INPUT_MAX, 'strength': STRENGTH, 'knots': KNOTS}
DISABLED = {**GRADE, 'strength': 0}
RECONSTRUCTION = ('fsr3_native', 'fsr3_quality', 'fsr3_balanced', 'fsr3_performance')
PROBES = ((5, 7), (67, 49), (102, 65), (136, 97))
VALIDATION_MARKER = 'Insert instance layer "VK_LAYER_KHRONOS_validation"'


def grade_color(exposed, strength=STRENGTH):
    result = []
    for channel, value in enumerate(exposed):
        if value >= INPUT_MAX:
            corrected = KNOTS[-1][channel] + value - INPUT_MAX
        else:
            position = 32 * value / INPUT_MAX
            low = math.floor(position)
            fraction = position - low
            corrected = (1 - fraction) * KNOTS[low][channel] + fraction * KNOTS[low+1][channel]
        result.append((1-strength) * value + strength * corrected)
    return result


@lru_cache(maxsize=10)
def reference(exposure=1., strength=STRENGTH, effect=False, bloom=False, wrong=None):
    source = scene_grid(effect)
    filtered = bloom_grid(effect) if bloom else None
    rows = []
    for y, row in enumerate(source):
        output = []
        for x, radiance in enumerate(row):
            halo = bilinear(filtered, x, y) if bloom else (0., 0., 0.)
            linear = [base + light*BLOOM['intensity'] for base, light in zip(radiance, halo)]
            if wrong == 'grade_before_exposure':
                corrected = [value*exposure for value in grade_color(linear, strength)]
            elif wrong == 'grade_before_bloom':
                corrected = [value + light*BLOOM['intensity']*exposure for value, light in
                             zip(grade_color([base*exposure for base in radiance], strength), halo)]
            elif wrong == 'grade_clips_highlights':
                corrected = grade_color([min(INPUT_MAX, value*exposure) for value in linear], strength)
            else:
                corrected = grade_color([value*exposure for value in linear], strength)
            output.append(tuple(display(value) for value in corrected))
        rows.append(output)
    return rows


def equivalent(actual, expected):
    if expected is None:
        return actual is None
    if not isinstance(actual, dict) or set(actual) != {'input_max', 'strength', 'knots'}:
        return False
    if actual['input_max'] != expected['input_max'] or actual['strength'] != expected['strength']:
        return False
    return len(actual['knots']) == 33 and all(
        len(value) == 3 and all(math.isclose(a, b, rel_tol=2e-7, abs_tol=1e-8) for a, b in zip(value, knot))
        for value, knot in zip(actual['knots'], expected['knots'])
    )


def self_test():
    check(grade_color((0., 0., 0.)) == [0., 0., 0.], 'Grade black anchor differs')
    check(grade_color((.25, .5, 2.), 0.) == [.25, .5, 2.], 'Zero strength differs')
    check(grade_color((INPUT_MAX + 1.,) * 3, 1.) == [value + 1. for value in KNOTS[-1]],
          'Highlight slopes differ')
    check(reference(0.)[0][0] == (0, 0, 0), 'Zero-exposure reference differs')
    for args, wrong in (((.75, STRENGTH, False, False), 'grade_before_exposure'),
                        ((1.75, STRENGTH, False, False), 'grade_clips_highlights'),
                        ((1.375, STRENGTH, True, True), 'grade_before_bloom')):
        expected = reference(*args)
        incorrect = reference(*args, wrong=wrong)
        verify_image(expected, expected, {}, wrong=incorrect)
    print('PASS: independent grade, exposure, highlight and bloom order oracles. No GPU executed.')


class GradeQualification(Qualification):
    def __init__(self, options):
        super().__init__(options)
        self.report.update(test_sha256=sha256(Path(__file__).read_bytes()),
                           lighting_path=options.lighting_path, samples=options.samples,
                           options={'authoring_only': options.authoring_only,
                                    'reconstruction': options.reconstruction})
        parent = Path(__file__).with_name('project_post_effect_capture.py')
        self.report['oracle_helpers_sha256'][parent.name] = sha256(parent.read_bytes())

    def require_validation(self):
        if self.options.require_validation:
            stderr = self.report['calls'][-1]['stderr']
            check(VALIDATION_MARKER in stderr, 'Vulkan loader did not report the Khronos validation layer')

    def rpc(self, *calls, binary=None, expected_errors=None):
        requests = [{'jsonrpc': '2.0', 'id': uuid.uuid4().hex, 'method': method, 'params': params}
                    for method, params in calls]
        raw = self.invoke([binary or self.binary, 'world', self.native(self.world)],
                          stdin=''.join(json.dumps(request) + '\n' for request in requests))
        replies = [json.loads(line) for line in raw.splitlines()]
        check(len(replies) == len(requests), 'Unexpected number of JSON-RPC replies')
        results = []
        for index, (request, reply) in enumerate(zip(requests, replies)):
            check(reply.get('id') == request['id'], f'Unexpected JSON-RPC reply: {reply}')
            if expected_errors and index in expected_errors:
                check(reply.get('error', {}).get('code') == expected_errors[index], reply)
                results.append(reply['error'])
            else:
                check('result' in reply, f"Unexpected {request['method']} response: {reply}")
                results.append(reply['result'])
        return results

    def select_grade(self, grade=None, exposure=1., effect=None, bloom=False):
        value = fixture_environment(effect, bloom, exposure)
        if grade is not None:
            value['output_grade'] = grade
        self.edit([component(2, 'LightingEnvironment', value)])

    def inspect_grade(self, grade):
        present = grade is not None
        lighting, dependencies = self.rpc(
            ('world.lighting', {'revision': self.revision}),
            ('world.dependencies', {}),
        )
        check(('output_grade' in lighting) == present
              and equivalent(lighting.get('output_grade'), grade), lighting)
        check(dependencies['revision'] == self.revision
              and ('needs_output_grade' in dependencies) == present
              and dependencies.get('needs_output_grade', False) is present, dependencies)
        return lighting

    def authoring_cases(self):
        self.inspect_grade(None)
        description, = self.rpc(('world.describe', {'view': 'component', 'name': 'LightingEnvironment'}))
        catalog = description['components']['LightingEnvironment']['properties']['output_grade']['properties']
        limits = {'input_max': (.125, 64), 'strength': (0, 1)}
        check(all((catalog[key]['minimum'], catalog[key]['maximum']) == bounds for key, bounds in limits.items())
              and catalog['knots']['minItems'] == catalog['knots']['maxItems'] == 33, catalog)
        self.select_grade(GRADE)
        authored = self.inspect_grade(GRADE)
        malformed = {
            'null': None, 'array': [], 'missing-strength': {'input_max': INPUT_MAX, 'knots': KNOTS},
            'extra-field': {**GRADE, 'exposure': 1}, 'input-zero': {**GRADE, 'input_max': 0},
            'input-large': {**GRADE, 'input_max': 64.01}, 'strength-false': {**GRADE, 'strength': False},
            'strength-negative': {**GRADE, 'strength': -.01}, 'strength-large': {**GRADE, 'strength': 1.01},
            'knots-short': {**GRADE, 'knots': KNOTS[:-1]}, 'knots-long': {**GRADE, 'knots': KNOTS + [[1]*3]},
        }
        for name, index, knot in (('lifted-black', 0, [0, .01, 0]), ('negative', 1, [-.01, 0, 0]),
                                  ('overrange', 1, [0, 16.01, 0]), ('bool', 1, [0, 0, True]),
                                  ('string', 1, [0, '0', 0]), ('knot-short', 2, [0, 0]),
                                  ('knot-long', 2, [0, 0, 0, 0])):
            candidate = deepcopy(GRADE)
            candidate['knots'][index] = knot
            malformed[name] = candidate

        prior = self.world.read_bytes()
        calls = [('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': self.revision,
                                    'ops': [component(2, 'LightingEnvironment',
                                                      {**fixture_environment(), 'output_grade': invalid})]})
                 for invalid in malformed.values()]
        errors = self.rpc(*calls, expected_errors={index: -32602 for index in range(len(calls))})
        check(prior == self.world.read_bytes() and authored == self.inspect_grade(GRADE),
              'Invalid grade transaction changed authored bytes or lighting')

        if self.options.authoring_only:
            self.select_grade(DISABLED)
        else:
            old, fresh = (uuid.uuid4().hex for _ in range(2))
            replace = ('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': self.revision,
                                         'ops': [component(2, 'LightingEnvironment',
                                                           {**fixture_environment(), 'output_grade': DISABLED})]})
            _, first, receipt, frozen, _, _, reopened, _ = self.rpc(
                ('runtime.start', {'session_id': old, 'revision': self.revision}),
                ('runtime.lighting', {'session_id': old, 'tick': 0}),
                replace,
                ('runtime.lighting', {'session_id': old, 'tick': 0}),
                ('runtime.stop', {'session_id': old}),
                ('runtime.start', {'session_id': fresh, 'revision': self.revision + 1}),
                ('runtime.lighting', {'session_id': fresh, 'tick': 0}),
                ('runtime.stop', {'session_id': fresh}),
            )
            check(receipt['committed'] and receipt['revision'] == self.revision + 1, receipt)
            self.revision = receipt['revision']
            check(first == frozen and equivalent(first['output_grade'], GRADE)
                  and equivalent(reopened['output_grade'], DISABLED), 'Runtime grade ownership differed')
            self.report['checks']['runtime_freezes_authored_selection'] = True
        self.inspect_grade(DISABLED)
        high = {'input_max': 64, 'strength': 1, 'knots': [[0, 0, 0]] + [[16, 16, 16]]*32}
        low = {**GRADE, 'input_max': .125}
        for bound in (high, low):
            self.select_grade(bound)
            self.inspect_grade(bound)
        self.select_grade()
        self.inspect_grade(None)
        self.report['checks']['authoring'] = {'rejected': dict(zip(malformed, errors)),
            'preserves_explicit_zero_strength': True,
            'accepts_declared_bounds': True, 'omits_absent_grade': True}

    def capture_grade(self, label, grade=None, *, effect=None, runtime=False, binary=None,
                      mode='none', debug='color'):
        path = self.run / f'{label}.bmp'
        prior = self.world.read_bytes()
        params = {'path': self.native(path), 'camera': CAMERA, 'width': WIDTH, 'height': HEIGHT,
                  'gpu': self.options.gpu, 'samples': self.options.samples,
                  'lighting_path': self.options.lighting_path, 'reconstruction': mode,
                  'capture_frames': 2 if mode == 'none' else 6, 'scene_debug_view': debug}
        if self.options.samples == 1:
            params['scene_product_probes'] = [dict(x=x, y=y) for x, y in PROBES]
        if runtime:
            session = uuid.uuid4().hex
            _, result, _ = self.rpc(
                ('runtime.start', {'session_id': session, 'revision': self.revision}),
                ('runtime.capture', {**params, 'session_id': session, 'tick': 0, 'ui_revision': 0,
                                     'ui_scale': 1}),
                ('runtime.stop', {'session_id': session}),
                binary=binary,
            )
        else:
            result, = self.rpc(('world.capture', {**params, 'revision': self.revision}), binary=binary)
        self.require_validation()
        self.report['images'][label] = {'path': str(path), 'render': result}
        check(self.world.read_bytes() == prior, 'Capture changed authored bytes')
        lighting = result['lighting']
        check(('output_grade' in lighting) == (grade is not None)
              and equivalent(lighting.get('output_grade'), grade)
              and lighting.get('post_effect') == effect, 'Capture used different scene lighting')
        check(result['revision'] == self.revision and result['source'] == ('runtime' if runtime else 'authored')
              and result['tick'] == (0 if runtime else None), 'Unexpected capture owner')
        diagnostic = result['render_diagnostics']
        check(diagnostic['lighting_path'] == self.options.lighting_path
              and diagnostic['scene_products']['view'] == debug, 'Requested rendering route did not run')
        if mode != 'none':
            check(diagnostic['reconstruction']['active'] and diagnostic['reconstruction']['mode'] == mode,
                  'Requested reconstruction did not run')
        receipts = diagnostic['scene_receipts']
        check(receipts['presented'] and receipts['presented'] == receipts['submitted'],
              'Capture lacks a matching presentation receipt')
        if self.options.expected_gpu:
            check(result['gpu'] == self.options.expected_gpu, 'Capture switched graphics adapter')
        return self.image(label, path, result, self.options.samples)

    def probe_values(self, label):
        result = self.report['images'][label]['render']['render_diagnostics']['scene_products']
        check(result['available'] and len(result['probes']) == len(PROBES), 'Missing raw HDR witnesses')
        return [{key: item[key] for key in ('x', 'y', 'raw_hdr', 'resolved_hdr', 'resolved_x', 'resolved_y')}
                for item in result['probes']]

    def color_cases(self):
        checks = self.report['checks']['color'] = {}
        original, image = self.capture_grade('omitted')
        verify_image(image, reference(strength=0), checks.setdefault('omitted', {}))
        if self.options.reference:
            binary = self.options.reference.resolve(strict=True)
            info = self.cli(binary, 'version')
            check(info['version'] == '0.0.371', 'Expected the retained 0.0.371 comparison executable')
            self.report['reference'] = {'path': str(binary), 'sha256': sha256(binary.read_bytes()), **info}
            old, _ = self.capture_grade('retained-371', binary=binary)
            check(original == old, 'The omitted grade changed retained 0.0.371 BMP bytes')
            checks['reference_omitted_identical'] = True
        self.select_grade(DISABLED)
        disabled, _ = self.capture_grade('explicit-zero', DISABLED)
        check(original == disabled, 'A zero-strength grade changed BMP bytes')
        checks['omitted_and_zero_identical'] = True
        for tag, exposure in (('exposed-low', .75), ('exposed-tail', 1.75), ('black', 0)):
            self.select_grade(GRADE, exposure=exposure)
            _, image = self.capture_grade(tag, GRADE)
            wrong = {'exposed-low': 'grade_before_exposure', 'exposed-tail': 'grade_clips_highlights'}.get(tag)
            verify_image(image, reference(exposure), checks.setdefault(tag, {}),
                         wrong=reference(exposure, wrong=wrong) if wrong else None)
        if self.options.samples == 1:
            expected = self.probe_values('omitted')
            check(all(p['raw_hdr'][:3] == list(SKY) for p in expected), 'Raw witnesses did not sample the sky')
            for label in ('explicit-zero', 'exposed-low', 'exposed-tail', 'black'):
                check(self.probe_values(label) == expected, 'Output grade or exposure changed upstream HDR probes')
            checks['upstream_hdr_exact'] = True
        artifact = self.import_artifact() if self.options.artifact else None
        self.select_grade(GRADE, exposure=1.375, effect=artifact, bloom=True)
        _, image = self.capture_grade('graded-bloom', GRADE, effect=artifact)
        verify_image(image, reference(1.375, effect=bool(artifact), bloom=True),
                     checks.setdefault('graded-bloom', {}),
                     wrong=reference(1.375, effect=bool(artifact), bloom=True, wrong='grade_before_bloom'))
        return artifact

    def reconstruction_cases(self):
        checks = self.report['checks']['reconstruction'] = {}
        for mode in self.options.reconstruction:
            self.select_grade(exposure=.75)
            baseline_label = f'{mode}-omitted'
            _, baseline = self.capture_grade(baseline_label, mode=mode)
            self.select_grade(GRADE, exposure=.75)
            grade_label = f'{mode}-graded'
            _, graded = self.capture_grade(grade_label, GRADE, mode=mode)
            expected_off = tuple(display(channel, .75) for channel in SKY)
            expected_on = tuple(display(channel) for channel in grade_color([channel*.75 for channel in SKY]))
            # Check central and off-center display pixels away from source
            # edges. These are display-sized coordinates even in Quality mode.
            coords = [(WIDTH*x//8, HEIGHT*y//8) for x in (1, 3, 5, 7) for y in (1, 3, 5, 7)]
            values = checks[mode] = [{'xy': (x, y), 'baseline': baseline[y][x], 'graded': graded[y][x]}
                                     for x, y in coords]
            check(all(difference(v['baseline'], expected_off) <= NUMBER_TOLERANCE
                      and difference(v['graded'], expected_on) <= NUMBER_TOLERANCE
                      and difference(v['baseline'], v['graded']) > NUMBER_TOLERANCE for v in values),
                  f'Reconstruction did not preserve the exposed grading order: {mode}, {values}')
            check(self.probe_values(baseline_label) == self.probe_values(grade_label),
                  f'Grade changed upstream raw or FSR-resolved HDR: {mode}')

    def ui_cases(self):
        self.edit([ui_operation()])
        records = self.report['checks']['ui'] = {}
        values = []
        for label, grade, exposure in (('omitted', None, .75), ('active', GRADE, .75), ('black', GRADE, 0)):
            self.select_grade(grade, exposure=exposure)
            _, image = self.capture_grade(f'hud-{label}', grade, runtime=True)
            verify_image(image, reference(exposure, 0 if grade is None else STRENGTH),
                         records.setdefault(label, {}), skip_rect=(9, 9, 47, 43))
            patch = [image[y][x] for y in range(24, 29) for x in range(25, 30)]
            check(all(difference(pixel, (84, 162, 219)) <= NUMBER_TOLERANCE for pixel in patch),
                  f'No authored opaque HUD witness: {patch}')
            records[label]['hud_rgb'] = patch
            values.append(patch)
        check(values[0] == values[1] == values[2], 'Output grade changed the HUD')

    def diagnostic_case(self):
        witness = 6
        self.edit([
            {'op': 'entity.create', 'id': uid(witness), 'name': 'Opaque diagnostic witness'},
            component(witness, 'Transform', {'position': [0, 2.6, -1], 'scale': [1, 1, 1],
                                             'rotation': [0, 0, 0, 1]}),
            component(witness, 'MeshRenderer', {'primitive': 'box', 'albedo': [0.2, 0.3, 0.4],
                                                'visible': True}),
        ])
        self.select_grade()
        original, image = self.capture_grade('diagnostic-omitted', debug='shading_normal')
        self.select_grade(GRADE)
        graded, _ = self.capture_grade('diagnostic-graded', GRADE, debug='shading_normal')
        nonblack = sum(pixel != (0, 0, 0) for row in image for pixel in row)
        check(nonblack > 100 and original == graded,
              'Grade changed shaded-normal diagnostic pixels or fixture had no visible witness')
        self.report['checks']['diagnostic'] = {'unchanged': True, 'nonblack_pixels': nonblack}

    def relocated_case(self, artifact):
        self.select_grade(GRADE, exposure=1.375, effect=artifact, bloom=True)
        manifest = self.source / 'project.poimaproject'
        project = {'format': 'poima.project', 'version': 1, 'project_id': uuid.uuid4().hex,
                   'name': 'Output grade fixture', 'audio': False, 'startup_splash': False,
                   'entry': {'world': self.world.name, 'camera': CAMERA, 'controller': CONTROLLER}}
        manifest.write_text(json.dumps(project) + '\n', encoding='utf-8')
        installed = self.options.runtime.resolve(strict=True)
        installed_descriptor = json.loads((installed / 'runtime.json').read_bytes())
        check(installed_descriptor['features'].get('output_grade') is True, 'Installed runtime does not advertise grading')
        project_inspection = self.cli(self.binary, 'project', 'inspect', self.native(manifest))
        check(project_inspection.get('needs_output_grade') is True, 'Project lost its grading requirement')
        refusals = {}
        for mode in ('missing', 'disabled'):
            incapable = self.run / f'Runtime grade {mode}'
            incapable.mkdir()
            descriptor = deepcopy(installed_descriptor)
            if mode == 'missing':
                del descriptor['features']['output_grade']
            else:
                descriptor['features']['output_grade'] = False
            (incapable / 'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
            unpublished = self.run / f'Unpublished grade {mode}'
            reply = self.invoke_refusal([self.binary, 'project', 'build', self.native(manifest),
                                         '--output', self.native(unpublished), '--runtime', self.native(incapable)])
            messages = [item['message'] for item in reply['diagnostics']]
            check(messages == ['Project output grading requires a compatible Vulkan runtime.']
                  and not unpublished.exists(), reply)
            refusals[mode] = reply
        initial = self.run / 'Initial bundle'
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(initial),
                 '--runtime', self.native(installed))
        bundle = self.run / 'Relocated bundle'
        initial.rename(bundle)
        detached = self.run / 'Unavailable source'
        self.source.rename(detached)
        scratch = self.run / 'Playback working directory'
        scratch.mkdir()
        checks = self.report['checks']['relocated'] = {'source_path_unavailable': not self.source.exists(),
                                                        'unsupported_runtime_rejections': refusals}
        try:
            check(checks['source_path_unavailable'], 'Original project path remains accessible')
            game = bundle / 'game.json'
            descriptor = json.loads(game.read_bytes())
            engine = bundle / descriptor['engine']['executable']
            packed_world = bundle / descriptor['entry']['world']
            package = packed_world.with_name(packed_world.name + '.assets') / f'{artifact}.poimapost'
            assets = [entry for entry in descriptor['files'] if entry['role'] == 'asset']
            check(len(assets) == 1 and assets[0]['path'] == package.relative_to(bundle).as_posix()
                  and assets[0]['sha256'] == sha256(package.read_bytes()) == artifact,
                  'Relocated v1 effect copy is not the admitted artifact')
            absent = self.run / 'Withheld.poimapost'
            package.rename(absent)
            try:
                result = self.invoke_refusal([engine, 'game', 'inspect', self.native(game)], cwd=scratch)
                checks['cold_missing_artifact_rejected'] = result
            finally:
                absent.rename(package)
            frozen = packed_world.read_bytes()
            tampered = json.loads(frozen)
            lighting = tampered['entities'][uid(2)]['components']['LightingEnvironment']
            check(equivalent(lighting['output_grade'], GRADE), 'The exported world lost the authored grade')
            lighting['output_grade']['strength'] = 0
            packed_world.write_text(json.dumps(tampered) + '\n', encoding='utf-8')
            try:
                checks['cold_modified_grade_rejected'] = self.invoke_refusal(
                    [engine, 'game', 'inspect', self.native(game)], cwd=scratch)
            finally:
                packed_world.write_bytes(frozen)
            replay = scratch / 'Replay.json'
            replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
            path = scratch / 'Frame.bmp'
            inspected = self.cli(engine, 'game', 'inspect', self.native(game), cwd=scratch)
            check(inspected['revision'] == self.revision and inspected['project_id'] == project['project_id']
                  and inspected.get('needs_output_grade') is True,
                  'Relocated manifest identity differs')
            result = self.cli(engine, 'game', 'run', self.native(game), '--gpu', self.options.gpu,
                              '--replay', self.native(replay), '--capture', self.native(path),
                              '--width', WIDTH, '--height', HEIGHT, '--samples', 1, cwd=scratch)
            self.require_validation()
            played = result['play']
            check(played['success'] and played['tick'] == 1 and played['stop_reason'] == 'replay_complete'
                  and played['lighting']['post_effect'] == artifact
                  and equivalent(played['lighting'].get('output_grade'), GRADE),
                  'Relocated game did not play the saved grade and effect')
            _, image = self.image('relocated-forward', path, played, 1)
            verify_image(image, reference(1.375, effect=True, bloom=True), checks.setdefault('image', {}),
                         wrong=reference(1.375, effect=True, bloom=True, wrong='grade_before_bloom'))
            check(sha256(package.read_bytes()) == artifact and packed_world.read_bytes() == frozen,
                  'Playback modified the packaged world or compiled effect')
        finally:
            detached.rename(self.source)

    def invoke_refusal(self, command, *, cwd=None):
        # The shared invoker records stdout/stderr before enforcing a zero exit.
        try:
            self.invoke(command, cwd=cwd)
        except RuntimeError:
            entry = self.report['calls'][-1]
            parsed = json.loads(entry['stdout'])
            check(entry['exit_code'] != 0 and parsed['status'] == 'error', entry)
            return parsed
        raise RuntimeError(f'Expected the command to refuse: {command}')

    def run_checks(self):
        evidence = self.run / 'report.json'
        try:
            self.edit(fixture_operations())
            self.authoring_cases()
            if not self.options.authoring_only:
                artifact = self.color_cases()
                if self.options.reconstruction:
                    self.reconstruction_cases()
                if self.options.runtime:
                    self.relocated_case(artifact)
                if self.options.check_ui:
                    self.ui_cases()
                if self.options.samples == 1:
                    self.diagnostic_case()
            self.report['passed'] = True
        except BaseException:
            self.report['failure'] = traceback.format_exc()
            raise
        finally:
            evidence.write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')
            print(f'Evidence: {evidence}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reference', type=Path, help='Retained 0.0.371 executable for exact omitted-grade comparison')
    parser.add_argument('--artifact', type=Path, help='Compiled soft_vignette .poimapost fixture')
    parser.add_argument('--runtime', type=Path, help='Matching installed Windows runtime for relocated export')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--expected-gpu', help='Require the exact adapter name returned by the renderer')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--reconstruction', choices=RECONSTRUCTION, action='append', default=[])
    parser.add_argument('--check-ui', action='store_true')
    parser.add_argument('--require-validation', action='store_true',
                        help='Require confirmation from the Windows Vulkan loader that Khronos validation ran')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--authoring-only', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    check(not sys.flags.optimize, 'Run without Python optimization because the BMP reader uses assertions')
    if args.self_test:
        self_test()
        return
    if not (args.binary and args.output):
        parser.error('binary and --output are required')
    if args.samples != 1 and (args.lighting_path == 'deferred' or args.reconstruction or args.runtime):
        parser.error('Deferred, reconstruction, and relocated fixture checks require one sample')
    if args.runtime and not args.artifact:
        parser.error('--runtime needs the compiled fixture at --artifact for the cold package guard')
    GradeQualification(args).run_checks()


if __name__ == '__main__':
    main()
