#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify the compiled vignette on native Windows Vulkan and retain raw evidence."""

import argparse
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import traceback
import uuid

from hdr_composition_capture import check, component, display, environment, uid
from scene_capture import pixels


WIDTH, HEIGHT = 321, 241
SKY = (.25, .5, 1.)
BLOOM = {'threshold': .875, 'intensity': 1, 'radius': 8}
CAMERA, CONTROLLER = uid(1), uid(3)
PROBES = ((0, 0), (WIDTH - 1, 0), (WIDTH // 2, 14), (42, 34),
          (WIDTH // 2, HEIGHT // 2), (0, HEIGHT - 1), (WIDTH - 1, HEIGHT - 1))
NUMBER_TOLERANCE = 2


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def half(value):
    return struct.unpack('<e', struct.pack('<e', value))[0]


def vignette_gain(x, y):
    u = (2*x + 1 - WIDTH) / WIDTH
    v = (2*y + 1 - HEIGHT) / HEIGHT
    fraction = min(1., max(0., (u*u + v*v - .32) / 1.28))
    return 1 - .28 * fraction**2 * (3 - 2*fraction)


@lru_cache(maxsize=2)
def scene_grid(effect):
    return [[tuple(half(c * vignette_gain(x, y)) for c in SKY) if effect else SKY
             for x in range(WIDTH)] for y in range(HEIGHT)]


@lru_cache(maxsize=2)
def bloom_grid(effect):
    # Build an independent reference in linear RGB. Radius eight places taps at
    # integral bloom texels, eliminating ambiguity about filter sample centers.
    source = scene_grid(effect)
    size_x, size_y = (WIDTH + 3) // 4, (HEIGHT + 3) // 4
    threshold = BLOOM['threshold']
    cells = []
    for y in range(size_y):
        row = []
        for x in range(size_x):
            region = [color for src in source[4*y:4*y+4] for color in src[4*x:4*x+4]]
            bright = [tuple(c * max(0, 1 - threshold/max(color)) for c in color)
                      for color in region]
            row.append(tuple(half(sum(channel)/len(bright)) for channel in zip(*bright)))
        cells.append(row)
    weights = (1, 4, 6, 4, 1)
    horizontal = [
        [tuple(half(sum(weight * row[min(size_x-1, max(0, x+offset))][channel]
                        for offset, weight in enumerate(weights, -2))/16) for channel in range(3))
         for x in range(size_x)] for row in cells]
    return [
        [tuple(half(sum(weight * horizontal[min(size_y-1, max(0, y+offset))][x][channel]
                        for offset, weight in enumerate(weights, -2))/16) for channel in range(3))
         for x in range(size_x)] for y in range(size_y)]


def bilinear(grid, x, y):
    # Source positions straddle quarter-resolution texel centers. Coordinates
    # in a partial edge cell still use the full four-texel spacing.
    u = min(len(grid[0])-1, max(0., (x+.5)/4-.5))
    v = min(len(grid)-1, max(0., (y+.5)/4-.5))
    left, top = int(u), int(v)
    right, bottom = min(len(grid[0])-1, left+1), min(len(grid)-1, top+1)
    fx, fy = u-left, v-top
    return tuple((1-fy)*((1-fx)*a + fx*b) + fy*((1-fx)*c + fx*d)
                 for a, b, c, d in zip(grid[top][left], grid[top][right],
                                       grid[bottom][left], grid[bottom][right]))


@lru_cache(maxsize=6)
def reference(effect=False, bloom=False, exposure=1, wrong_bloom_order=False):
    scene = scene_grid(effect)
    output = []
    for y, row in enumerate(scene):
        line = []
        for x, color in enumerate(row):
            if bloom:
                add = bilinear(bloom_grid(effect and not wrong_bloom_order), x, y)
                color = tuple(c + b * BLOOM['intensity'] for c, b in zip(color, add))
            line.append(tuple(display(c, exposure) for c in color))
        output.append(line)
    return output


def difference(left, right):
    return max(abs(a-b) for a, b in zip(left, right))


def verify_image(actual, expected, record, skip_rect=None, wrong=None):
    metric = {'checked_pixels': 0, 'maximum_rgb_error': 0, 'out_of_tolerance': 0,
              'first_mismatches': [], 'refutes_wrong_order': 0, 'witnesses': []}
    record.update(metric)
    for y, row in enumerate(actual):
        for x, color in enumerate(row):
            if skip_rect and skip_rect[0] <= x < skip_rect[2] and skip_rect[1] <= y < skip_rect[3]:
                continue
            target = expected[y][x]
            error = difference(color, target)
            record['checked_pixels'] += 1
            record['maximum_rgb_error'] = max(record['maximum_rgb_error'], error)
            if error > NUMBER_TOLERANCE:
                record['out_of_tolerance'] += 1
                if len(record['first_mismatches']) < 12:
                    record['first_mismatches'].append({'x': x, 'y': y, 'actual': color, 'expected': target})
            if wrong is not None and error <= NUMBER_TOLERANCE and difference(color, wrong[y][x]) > NUMBER_TOLERANCE:
                record['refutes_wrong_order'] += 1
    for x, y in PROBES:
        witness = {'x': x, 'y': y, 'actual': actual[y][x], 'expected': expected[y][x]}
        if wrong is not None:
            witness['wrong_order_expected'] = wrong[y][x]
        record['witnesses'].append(witness)
    check(record['checked_pixels'] >= WIDTH * HEIGHT // 2, 'Too few scene pixels checked')
    check(record['out_of_tolerance'] == 0, f"Scene mismatches: {record['first_mismatches']}")
    if wrong is not None:
        check(record['refutes_wrong_order'] > 1000, 'Capture does not distinguish the incorrect stage order')


def wrong_display_order(exposure):
    return [[tuple(round(display(c, exposure) * vignette_gain(x, y)) for c in SKY)
             for x in range(WIDTH)] for y in range(HEIGHT)]


def fixture_environment(effect=None, bloom=False, exposure=1):
    result = environment(exposure)
    for name in ('zenith', 'horizon', 'ground'):
        result['sky'][name] = list(SKY)
    if effect is not None:
        result['post_effect'] = effect
    if bloom:
        result['bloom'] = BLOOM
    return result


def fixture_operations():
    position = {'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}
    return [
        {'op': 'entity.create', 'id': CONTROLLER, 'name': 'Player'},
        {'op': 'entity.create', 'id': CAMERA, 'name': 'Capture camera', 'parent': CONTROLLER},
        {'op': 'entity.create', 'id': uid(2), 'name': 'Constant HDR sky'},
        component(3, 'Transform', {**position, 'position': [0, 1, 2]}),
        component(3, 'CharacterController', {'radius': .3, 'height': 1.8, 'speed': 4,
                                             'jump_speed': 5, 'camera': CAMERA}),
        component(1, 'Transform', {**position, 'position': [0, 1.6, 0]}),
        component(1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
        component(2, 'LightingEnvironment', fixture_environment()),
    ]


def ui_operation():
    return {'op': 'ui.element.set', 'id': uid(5), 'element': {
        'parent': None, 'name': 'Opaque UI witness', 'kind': 'panel', 'text': '',
        'action': None, 'visible': True, 'enabled': True,
        'layout': {'position': 'absolute', 'hit_test': 'pass_through', **{
            field: {'unit': 'dp', 'value': value}
            for field, value in {'left': 10, 'top': 10, 'width': 36, 'height': 32}.items()}},
        'style': {'background_color': '#54a2db', 'border_width': 0},
    }}


def self_test():
    check(vignette_gain(WIDTH//2, HEIGHT//2) == 1 and vignette_gain(0, 0) == .72,
          'Camera-center or corner gain differs')
    check(reference()[0][0] == tuple(display(c) for c in SKY), 'Disabled reference differs')
    # Bright center is untouched by the vignette. The bloom gather has no
    # eligible texels around the upper left corner after the vignette.
    check(reference(True, True)[0][0] == reference(True)[0][0], 'Unexpected corner bloom')
    check(reference(True, True)[HEIGHT//2][WIDTH//2] == reference(False, True)[HEIGHT//2][WIDTH//2],
          'Center bloom differs')
    for correct, wrong in ((reference(True), wrong_display_order(1)),
                           (reference(True, True), reference(True, True, wrong_bloom_order=True))):
        verify_image(correct, correct, {}, wrong=wrong)
        record = {}
        try:
            verify_image(wrong, correct, record)
        except RuntimeError:
            check(record['out_of_tolerance'] > 1000, 'Wrong-stage oracle lacked a meaningful witness')
        else:
            raise RuntimeError('Oracle accepted a deliberately incorrect stage')
    print('PASS: analytic vignette and bloom oracles discriminate wrong stage ordering. No GPU executed.')


class Qualification:
    def __init__(self, options):
        self.options = options
        self.binary = options.binary.resolve(strict=True)
        self.run = options.output.resolve() / uuid.uuid4().hex
        self.source = self.run / 'Authored project'
        self.source.mkdir(parents=True)
        self.world = self.source / 'world.json'
        self.revision = 0
        self.report = {'passed': False, 'gpu_index': options.gpu, 'extent': [WIDTH, HEIGHT],
                       'calls': [], 'images': {}, 'checks': {},
                       'binary': str(self.binary), 'binary_sha256': sha256(self.binary.read_bytes()),
                       'test_sha256': sha256(Path(__file__).read_bytes()),
                       'oracle_helpers_sha256': {name: sha256(Path(__file__).with_name(name).read_bytes())
                                                 for name in ('hdr_composition_capture.py', 'scene_capture.py')}}

    def native(self, path):
        path = str(path.resolve())
        if self.options.windows_interop:
            return subprocess.check_output(['wslpath', '-w', path], text=True, timeout=10).strip()
        return path

    def invoke(self, argv, *, stdin=None, cwd=None, timeout=120):
        raw = {'command': [str(arg) for arg in argv], 'cwd': str(cwd) if cwd else None, 'stdin': stdin}
        self.report['calls'].append(raw)
        try:
            process = subprocess.run(raw['command'], input=stdin, cwd=cwd, capture_output=True,
                                     encoding='utf-8', errors='replace', timeout=timeout)
        except subprocess.TimeoutExpired as error:
            # subprocess has reaped the timed-out process. Keep its partial output.
            def decoded(value):
                return value.decode('utf-8', 'replace') if isinstance(value, bytes) else (value or '')
            raw.update(timed_out=True, stdout=decoded(error.stdout), stderr=decoded(error.stderr))
            raise
        raw.update(exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr)
        check(process.returncode == 0, f'Poima exited {process.returncode}: {process.stderr}')
        forbidden = ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD', 'Undefined-Value')
        check(not any(token in process.stderr for token in forbidden), 'Vulkan validation diagnostic in stderr')
        return process.stdout

    def rpc(self, *calls):
        requests = [{'jsonrpc': '2.0', 'id': uuid.uuid4().hex, 'method': method, 'params': params}
                    for method, params in calls]
        raw = self.invoke([self.binary, 'world', self.native(self.world)],
                          stdin=''.join(json.dumps(request) + '\n' for request in requests))
        replies = [json.loads(line) for line in raw.splitlines()]
        check(len(replies) == len(requests), 'Unexpected number of JSON-RPC replies')
        for request, reply in zip(requests, replies):
            check(reply.get('id') == request['id'] and 'result' in reply,
                  f"Unexpected {request['method']} response: {reply}")
        return [reply['result'] for reply in replies]

    def cli(self, binary, *arguments, cwd=None):
        reply = json.loads(self.invoke([binary, *arguments], cwd=cwd))
        check(reply['status'] == 'ok', f'Unexpected CLI result: {reply}')
        return reply['result']

    def edit(self, operations):
        receipt, = self.rpc(('world.transact', {'request_id': uuid.uuid4().hex,
                            'base_revision': self.revision, 'ops': operations}))
        check(receipt['committed'] is True and receipt['revision'] == self.revision + 1,
              f'Scene transaction did not commit: {receipt}')
        self.revision = receipt['revision']

    def light(self, effect=None, bloom=False, exposure=1):
        self.edit([component(2, 'LightingEnvironment', fixture_environment(effect, bloom, exposure))])

    def import_artifact(self):
        artifact = self.options.artifact.resolve(strict=True)
        original = artifact.read_bytes()
        digest = sha256(original)
        metadata = json.loads(original)
        manifest = metadata['manifest']
        check(manifest['id'] == 'soft_vignette', '--artifact must be the compiled soft_vignette fixture')
        staged = self.source / 'Vignette.poimapost'
        staged.write_bytes(original)
        self.report['artifact'] = {'path': str(artifact), 'staged': str(staged),
                                   'sha256': digest, 'bytes': len(original), 'manifest': manifest}
        receipt, = self.rpc(('asset.post-effect.import', {'source': staged.name, 'expected_sha256': digest}))
        check(receipt == {'asset': digest, 'id': 'soft_vignette', 'bytes': len(original),
                          'words': metadata['spirv']['bytes'] // 4}, f'Wrong import receipt: {receipt}')
        package = self.world.with_name(f'{self.world.name}.assets') / f'{digest}.poimapost'
        check(package.read_bytes() == original, 'Asset import changed compiled bytes')
        dependencies, = self.rpc(('world.dependencies', {}))
        check(dependencies['assets'] == [], 'Import selected the effect without a transaction')
        return digest

    def image(self, name, path, result, samples):
        record = self.report['images'][name] = {'path': str(path), 'render': result}
        data = path.read_bytes() if path.is_file() else None
        if data is not None:
            record.update(sha256=sha256(data), bytes=len(data))
        check(result['hardware'] is True and result['capture_written'] is True
              and result['nvrhi_errors'] == 0 and result['frames_presented'] > 0,
              f'Vulkan capture did not complete cleanly: {result}')
        check((result['width'], result['height'], result['samples']) == (WIDTH, HEIGHT, samples),
              f'Capture extent or sample count differs: {result}')
        check(data is not None, f'Capture receipt has no BMP at {path}')
        image = pixels(path)
        check(len(image) == HEIGHT and all(len(row) == WIDTH for row in image), 'Unexpected BMP extent')
        return data, image

    def capture(self, name, samples, effect, *, runtime=False):
        path = self.run / f'{name}.bmp'
        before = self.world.read_bytes()
        params = {'path': self.native(path), 'camera': CAMERA, 'width': WIDTH, 'height': HEIGHT,
                  'gpu': self.options.gpu, 'samples': samples, 'capture_frames': 2}
        if runtime:
            session = uuid.uuid4().hex
            _, result, _ = self.rpc(
                ('runtime.start', {'session_id': session, 'revision': self.revision}),
                ('runtime.capture', {**params, 'session_id': session, 'tick': 0, 'ui_revision': 0, 'ui_scale': 1}),
                ('runtime.stop', {'session_id': session}),
            )
        else:
            result, = self.rpc(('world.capture', {**params, 'revision': self.revision}))
        self.report['images'][name] = {'path': str(path), 'render': result}
        check(result['revision'] == self.revision and result['source'] == ('runtime' if runtime else 'authored')
              and result['tick'] == (0 if runtime else None), 'Capture used unexpected source ownership')
        lighting = result['lighting']
        check(lighting.get('post_effect') == effect and ('post_effect' in lighting) == (effect is not None),
              'Capture used unexpected authored effect')
        check(self.world.read_bytes() == before, 'Capture changed the authored world')
        return self.image(name, path, result, samples)

    def authored_cases(self):
        self.edit(fixture_operations())
        cases = self.report['checks']['authored'] = {}
        baselines = {}
        for samples in (1, 4):
            name = f'baseline-{samples}x'
            baselines[samples], actual = self.capture(name, samples, None)
            verify_image(actual, reference(), cases.setdefault(name, {}))
        artifact = self.import_artifact()
        cases_to_capture = (
            ('vignette', artifact, False, 1),
            ('vignette-exposed', artifact, False, 2),
            ('bloom', None, True, 1),
            ('vignette-bloom', artifact, True, 1),
            ('reverted', None, False, 1),
        )
        for label, effect, bloom, exposure in cases_to_capture:
            self.light(effect, bloom, exposure)
            expected = reference(effect is not None, bloom, exposure)
            wrong = None
            if effect is not None:
                wrong = reference(True, True, exposure, True) if bloom else wrong_display_order(exposure)
            for samples in (1, 4):
                name = f'{label}-{samples}x'
                data, actual = self.capture(name, samples, effect)
                entry = cases.setdefault(name, {})
                verify_image(actual, expected, entry, wrong=wrong)
                if label == 'reverted':
                    entry['identical_to_original_bmp'] = data == baselines[samples]
                    check(entry['identical_to_original_bmp'], 'Omitting the effect changed the original BMP bytes')
        return artifact

    def ui_cases(self, artifact):
        self.edit([ui_operation()])
        cases = self.report['checks']['runtime_ui'] = {}
        ui_values = []
        for label, effect, exposure in (('baseline', None, 1), ('vignette', artifact, 1),
                                         ('zero-exposure', artifact, 0)):
            name = f'runtime-ui-{label}'
            self.light(effect, exposure=exposure)
            _, actual = self.capture(name, 1, effect, runtime=True)
            entry = cases.setdefault(name, {})
            verify_image(actual, reference(effect is not None, exposure=exposure), entry,
                         skip_rect=(9, 9, 47, 43))
            patch = [actual[y][x] for y in range(24, 29) for x in range(25, 30)]
            entry['opaque_panel'] = {'top_left': (25, 24), 'extent': (5, 5),
                                     'actual': patch, 'expected': (84, 162, 219)}
            check(all(difference(pixel, (84, 162, 219)) <= NUMBER_TOLERANCE for pixel in patch),
                  f'UI witness missed its authored display color: {patch}')
            ui_values.append(patch)
        check(ui_values[0] == ui_values[1] == ui_values[2], 'Effect or exposure changed composited UI bytes')

    def export_case(self, artifact):
        self.light(artifact, bloom=True)
        report = self.report['checks']['relocated_export'] = {}
        runtime = self.options.runtime.resolve(strict=True)
        manifest = self.source / 'project.poimaproject'
        project = {'format': 'poima.project', 'version': 1, 'project_id': uuid.uuid4().hex,
                   'name': 'Project post-effect capture', 'audio': False, 'startup_splash': False,
                   'entry': {'world': self.world.name, 'camera': CAMERA, 'controller': CONTROLLER}}
        manifest.write_text(json.dumps(project, indent=2) + '\n', encoding='utf-8')
        output = self.run / 'Initial export'
        def inventory(root):
            return {path.relative_to(root).as_posix(): sha256(path.read_bytes())
                    for path in sorted(root.rglob('*')) if path.is_file()}
        authored = inventory(self.source)
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(output),
                 '--runtime', self.native(runtime))
        check(inventory(self.source) == authored, 'Export changed source project files')
        bundle = self.run / 'Relocated' / 'Game'
        bundle.parent.mkdir()
        output.rename(bundle)
        detached = self.run / 'Unavailable at original source path'
        self.source.rename(detached)
        try:
            game = bundle / 'game.json'
            spec = json.loads(game.read_bytes())
            engine = bundle / spec['engine']['executable']
            expected = f'content/world.json.assets/{artifact}.poimapost'
            files = [entry for entry in spec['files'] if entry['role'] == 'asset']
            check(len(files) == 1 and files[0]['path'] == expected and files[0]['sha256'] == artifact,
                  f'Unexpected packaged asset closure: {files}')
            frozen = inventory(bundle)
            check(frozen[expected] == artifact and not any(path.endswith('Vignette.poimapost') for path in frozen),
                  'Relocated compiled asset differs from the admitted digest')
            report.update(runtime=str(runtime), bundle=str(bundle), inventory=frozen, binary=str(engine),
                          binary_sha256=sha256(engine.read_bytes()), source_path_unavailable=not self.source.exists())
            check(report['source_path_unavailable'], 'Original source path still exists')
            unrelated = self.run / 'External working directory'
            unrelated.mkdir()
            replay, frame, play_report = (unrelated / name for name in ('replay.json', 'standalone.bmp', 'play.json'))
            replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
            inspected = self.cli(engine, 'game', 'inspect', self.native(game), cwd=unrelated)
            check(inspected['revision'] == self.revision and inspected['project_id'] == project['project_id'],
                  'Relocated game manifest identity differs')
            result = self.cli(engine, 'game', 'run', self.native(game), '--gpu', self.options.gpu,
                              '--replay', self.native(replay), '--capture', self.native(frame),
                              '--report', self.native(play_report), '--width', WIDTH, '--height', HEIGHT,
                              '--samples', 1, cwd=unrelated)
            check(result == json.loads(play_report.read_bytes()), 'Written play report differs from CLI stdout')
            played = result['play']
            check(played['success'] and played['tick'] == 1 and played['stop_reason'] == 'replay_complete'
                  and played['lighting']['post_effect'] == artifact and played['revision'] == self.revision,
                  f'Relocated game did not complete its replay: {played}')
            _, actual = self.image('relocated-export', frame, played, 1)
            verify_image(actual, reference(True, True), report.setdefault('color_oracle', {}),
                         wrong=reference(True, True, wrong_bloom_order=True))
            check(inventory(bundle) == frozen and inventory(detached) == authored,
                  'Relocated game changed its bundle or source project')
        finally:
            detached.rename(self.source)

    def run_checks(self):
        evidence = self.run / 'report.json'
        try:
            artifact = self.authored_cases()
            if self.options.runtime:
                self.export_case(artifact)
            if self.options.check_ui:
                self.ui_cases(artifact)
            self.report['passed'] = True
        except BaseException:
            self.report['failure'] = traceback.format_exc()
            raise
        finally:
            evidence.write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')
            print(f'Evidence: {evidence}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, nargs='?')
    parser.add_argument('--artifact', type=Path, help='Compiled soft_vignette .poimapost fixture')
    parser.add_argument('--output', type=Path, help='Directory for retained capture runs')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--runtime', type=Path, help='Matching installed Windows runtime for relocated export')
    parser.add_argument('--check-ui', action='store_true', help='Enable additional UI capture checks')
    parser.add_argument('--self-test', action='store_true')
    options = parser.parse_args()
    check(not sys.flags.optimize, 'Run without Python optimization because the BMP reader uses assertions')
    if options.self_test:
        self_test()
    else:
        if not (options.binary and options.artifact and options.output):
            parser.error('binary, --artifact, and --output are required for native capture')
        Qualification(options).run_checks()


if __name__ == '__main__':
    main()
