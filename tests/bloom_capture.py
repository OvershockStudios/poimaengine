#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify the authored HDR bloom output against disabled captures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import traceback
import uuid

from hdr_composition_capture import check, component, environment, uid
from scene_capture import pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    options = parser.parse_args()
    check(not sys.flags.optimize, 'Run without Python optimization because the shared BMP parser uses assertions')
    binary = options.binary.resolve(strict=True)
    run = options.output.resolve() / uuid.uuid4().hex
    run.mkdir(parents=True)
    width, height = 321, 241
    world = run / 'world.json'
    revision = 0
    report = {'passed': False, 'gpu': options.gpu, 'captures': {}, 'calls': [],
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'parser_sha256': hashlib.sha256(Path(__file__).with_name('scene_capture.py').read_bytes()).hexdigest()}

    def native(path):
        if options.windows_interop:
            return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
        return str(path)

    def call(method, params):
        request = {'jsonrpc': '2.0', 'id': uuid.uuid4().hex, 'method': method, 'params': params}
        process = subprocess.run([binary, 'world', native(world)], input=json.dumps(request) + '\n',
                                 capture_output=True, text=True, encoding='utf-8', timeout=90)
        report['calls'].append({'request': request, 'exit_code': process.returncode,
                                'stdout': process.stdout, 'stderr': process.stderr})
        check(process.returncode == 0, 'Poima failed')
        reply = json.loads(process.stdout)
        check(reply.get('id') == request['id'] and 'result' in reply, f'Unexpected reply: {reply}')
        return reply['result']

    def edit(ops):
        nonlocal revision
        receipt = call('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': revision, 'ops': ops})
        check(receipt['committed'] and receipt['revision'] == revision + 1, 'Scene edit did not commit')
        revision = receipt['revision']

    def setup_light(bloom=None, exposure=1):
        values = environment(exposure, 0)
        if bloom is not None:
            values['bloom'] = bloom
        edit([component(2, 'LightingEnvironment', values)])

    def capture(name, samples):
        path = run / (name + '.bmp')
        before = world.read_bytes()
        result = call('world.capture', {'path': native(path), 'camera': uid(1), 'revision': revision,
            'width': width, 'height': height, 'samples': samples, 'gpu': options.gpu})
        check(result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0,
              'Vulkan hardware or validation gate failed')
        check(result['samples'] == samples, 'Requested MSAA unavailable')
        check(world.read_bytes() == before, 'Capture changed the authored world')
        image = pixels(path)
        check(len(image) == height and all(len(row) == width for row in image), 'Capture extent differs')
        report['captures'][name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                    'path': str(path), 'result': result}
        return image

    try:
        edit([{'op': 'entity.create', 'id': uid(number), 'name': label} for number, label in
              [(1, 'Observer'), (2, 'Bloom environment'), (3, 'Emitter'), (4, 'Disabled light')]] + [
            component(1, 'Transform', {'position': [0, 0, 4], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}),
            component(1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
            component(2, 'LightingEnvironment', environment(1, 0)),
            component(3, 'Transform', {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [.5, .5, .1]}),
            component(3, 'MeshRenderer', {'primitive': 'box', 'albedo': [0, 0, 0], 'visible': True}),
            component(3, 'PbrMaterial', {'base_color': [0, 0, 0], 'emissive': [1, 1/3, 0], 'roughness': 1,
                                        'metallic': 0, 'double_sided': False}),
            component(4, 'Light', {'kind': 'directional', 'color': [1, 1, 1], 'intensity': 0, 'enabled': True}),
        ])
        for samples in (1, 4):
            setup_light()
            baseline = capture(f'default-{samples}', samples)
            setup_light({'intensity': 0, 'threshold': 0, 'radius': 32})
            check(capture(f'disabled-{samples}', samples) == baseline, 'Zero intensity changed output pixels')
            setup_light({'intensity': .5, 'threshold': 65504, 'radius': 20})
            check(capture(f'suppressed-{samples}', samples) == baseline, 'Clipped highlights still bloomed')
            setup_light({'intensity': .5, 'threshold': .25, 'radius': 20})
            blurred = capture(f'bloom-{samples}', samples)
            halo = [(x, y) for y in range(height) for x in range(width)
                    if baseline[y][x] == (0, 0, 0) and blurred[y][x][0] >= 10]
            check(len(halo) > 80, 'No visible bloom surrounding the silhouette')
            check(all(blurred[y][x][0] > blurred[y][x][1] >= blurred[y][x][2] for x, y in halo),
                  'Bloom lost the emitter color')
            check(all(abs(x - width // 2) <= 42 and abs(y - height // 2) <= 42 for x, y in halo),
                  'Bloom leaked beyond the bounded filter footprint')
            setup_light({'intensity': .5, 'threshold': .25, 'radius': 20}, exposure=0)
            check(all(color == (0, 0, 0) for row in capture(f'exposure-zero-{samples}', samples) for color in row),
                  'Bloom bypassed exposure')
            report['captures'][f'bloom-{samples}']['halo_pixels'] = len(halo)
        report['passed'] = True
        print(f'Bloom Vulkan checks passed at 1× and 4× MSAA. Evidence: {run}')
    except Exception:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        (run / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
