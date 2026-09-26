#!/usr/bin/env python3
"""Explicit GPU integration: author, capture, edit and compare real scene pixels."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid


def pixels(path):
    data = path.read_bytes()
    assert data[:2] == b'BM'
    offset = struct.unpack_from('<I', data, 10)[0]
    size, width, signed_height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    assert size >= 40 and width > 0 and signed_height != 0 and planes == 1 and bits in (24, 32)
    assert compression in (0, 3)
    if compression == 3: assert struct.unpack_from('<III', data, 54) == (0xff0000, 0xff00, 0xff)
    height = abs(signed_height)
    stride = ((width * bits + 31) // 32) * 4
    assert len(data) >= offset + stride * height
    rows = []
    for y in range(height):
        source_y = height - 1 - y if signed_height > 0 else y
        row = []
        for x in range(width):
            index = offset + source_y * stride + x * (bits // 8)
            b, g, r = data[index:index+3]
            row.append((r, g, b))
        rows.append(row)
    return rows


def mask(image, channel):
    points = [(x, y) for y, row in enumerate(image) for x, value in enumerate(row)
              if value[channel] > 70 and all(value[channel] > value[k] * 1.7 for k in range(3) if k != channel)]
    return {'count': len(points), 'centroid_x': sum(p[0] for p in points) / max(1, len(points))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # A unique run directory prevents old artifacts from satisfying this test.
    run = args.output / uuid.uuid4().hex
    run.mkdir()
    def native(path):
        text = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True).strip() if args.windows_interop else text
    def uid(n): return f'{n:032x}'
    def create(n, name, parent=None): return {'op': 'entity.create', 'id': uid(n), 'name': name, 'parent': parent}
    def component(n, kind, value): return {'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
    def transform(n, position, scale=(1, 1, 1)):
        return component(n, 'Transform', {'position': position, 'rotation': [0, 0, 0, 1], 'scale': scale})
    def mesh(n, color, visible=True):
        return component(n, 'MeshRenderer', {'primitive': 'box', 'albedo': color, 'visible': visible})
    requests = []
    def request(method, params):
        requests.append({'jsonrpc': '2.0', 'id': len(requests)+1, 'method': method, 'params': params})
        return len(requests)
    def txn(rev, ops): return request('world.transact', {'base_revision': rev, 'request_id': uuid.uuid4().hex, 'ops': ops})
    captures = {}
    def capture(name, rev, samples=4, **extra):
        path = run / (name + '.bmp')
        request_id = request('world.capture', {'revision': rev, 'camera': uid(100), 'path': native(path),
            'gpu': args.gpu, 'samples': samples, 'width': 960, 'height': 540, **extra})
        captures[name] = (request_id, path)
        return request_id
    txn(0, [create(100, 'Camera'), transform(100, [0, 0, 8]),
        component(100, 'Camera', {'vertical_fov': 60, 'near': 0.1, 'far': 100}),
        create(1, 'Near red'), transform(1, [0, 0, 0], [2, 2, 2]), mesh(1, [0.85, 0.015, 0.01]),
        create(2, 'Far green, drawn after near red'), transform(2, [0, 0, -2], [4, 4, 1]), mesh(2, [0.01, 0.85, 0.015]),
        create(4, 'Blue parent'), create(3, 'Blue child', uid(4)), transform(3, [3, 0, 0]), mesh(3, [0.01, 0.02, 0.85]),
        create(5, 'Behind camera'), transform(5, [0, 0, 10], [2, 2, 1]), mesh(5, [0.85, 0.85, 0.01])])
    capture('baseline', 1)
    capture('single-sample', 1, samples=1)
    txn(1, [transform(4, [-1, 0, 0]), mesh(1, [0.85, 0.015, 0.01], visible=False)])
    capture('edited', 2)
    txn(2, [transform(100, [1, 0, 8])])
    capture('camera-moved', 3)
    stale = capture('stale', 2)
    missing_gpu = capture('missing-gpu', 3, gpu=4095)
    existing = request('world.capture', {'revision': 3, 'camera': uid(100), 'path': native(captures['baseline'][1])})
    inspected = request('world.inspect', {})
    command = [str(args.binary.resolve()), 'world', native(run / 'world.json')]
    process = subprocess.run(command, input=''.join(json.dumps(r)+'\n' for r in requests), capture_output=True,
                             text=True, encoding='utf-8', timeout=120)
    evidence = {'command': command, 'gpu_index': args.gpu, 'exit_code': process.returncode,
                'stderr': process.stderr, 'run_directory': str(run.resolve()), 'requests': requests}
    try:
        assert process.returncode == 0, process.stdout + process.stderr
        responses = {r['id']: r for r in map(json.loads, process.stdout.splitlines())}
        evidence['responses'] = responses
        assert len(responses) == len(requests)
        for request_id, response in responses.items():
            if request_id not in (stale, missing_gpu, existing): assert 'result' in response, response
        for request_id, code in [(stale, -32009), (missing_gpu, -32020), (existing, -32602)]:
            assert responses[request_id]['error']['code'] == code, responses[request_id]
        assert responses[inspected]['result']['revision'] == 3
        assert not captures['stale'][1].exists() and not captures['missing-gpu'][1].exists()
        images = {}
        evidence['captures'] = {}
        for name, rev, samples in [('baseline', 1, 4), ('single-sample', 1, 1), ('edited', 2, 4), ('camera-moved', 3, 4)]:
            request_id, path = captures[name]
            report = responses[request_id]['result']
            assert report['revision'] == rev and report['samples'] == samples
            assert report['hardware'] and report['capture_written'] and report['nvrhi_errors'] == 0
            assert report['frames_presented'] == 2 and report['width'] == 960 and report['height'] == 540
            assert report['object_count'] == (4 if rev == 1 else 3)
            images[name] = pixels(path)
            evidence['captures'][name] = {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'report': report, 'red': mask(images[name], 0), 'green': mask(images[name], 1), 'blue': mask(images[name], 2)}
        metrics = evidence['captures']
        for name in ['baseline', 'single-sample']:
            center = images[name][270][480]
            assert center[0] > center[1]*2 and center[0] > center[2]*2, ('Depth occlusion failed', center)
            assert all(metrics[name][color]['count'] > 2000 for color in ['red', 'green', 'blue'])
        for name in ['edited', 'camera-moved']:
            assert metrics[name]['red']['count'] == 0, 'Hidden object still rendered'
        center = images['edited'][270][480]
        assert center[1] > center[0]*2 and center[1] > center[2]*2, ('Expected revealed green object', center)
        assert metrics['baseline']['blue']['centroid_x'] - metrics['edited']['blue']['centroid_x'] > 45
        assert metrics['edited']['blue']['centroid_x'] - metrics['camera-moved']['blue']['centroid_x'] > 45
        # MSAA correctly blends the red/green silhouettes into yellow corner
        # samples. The single-sample image isolates behind-camera clipping.
        yellow = sum(r > 80 and g > 80 and r > b*2 and g > b*2 for row in images['single-sample'] for r,g,b in row)
        assert yellow == 0, 'Object behind camera was not clipped'
        differences = sum(a != b for row1,row2 in zip(images['baseline'], images['single-sample']) for a,b in zip(row1,row2))
        assert 100 < differences < 20000, ('Expected localized MSAA edge differences', differences)
        reopen_path = run / 'reopened.bmp'
        reopen_request = {'jsonrpc': '2.0', 'id': 1, 'method': 'world.capture', 'params': {
            'revision': 3, 'camera': uid(100), 'path': native(reopen_path), 'gpu': args.gpu,
            'samples': 4, 'width': 960, 'height': 540}}
        reopened = subprocess.run(command, input=json.dumps(reopen_request)+'\n', capture_output=True,
                                  text=True, encoding='utf-8', timeout=60)
        evidence['reopened'] = {'exit_code': reopened.returncode, 'stderr': reopened.stderr,
                                'response': json.loads(reopened.stdout)}
        assert reopened.returncode == 0 and 'result' in evidence['reopened']['response'], evidence['reopened']
        reopened_report = evidence['reopened']['response']['result']
        assert reopened_report['revision'] == 3 and reopened_report['nvrhi_errors'] == 0
        assert reopened_report['world_id'] == responses[inspected]['result']['world_id']
        assert pixels(reopen_path) == images['camera-moved'], 'Reopened scene changed rendered pixels'
        evidence['reopened']['sha256'] = hashlib.sha256(reopen_path.read_bytes()).hexdigest()
        evidence['checks'] = {'depth_occlusion': True, 'hierarchy_edit_visible': True, 'camera_edit_visible': True,
            'visibility': True, 'behind_camera_clipped': True, 'msaa_changed_pixels': differences,
            'revision_and_failure_guards': True, 'reopened_scene_pixel_equality': True}
        evidence['passed'] = True
    finally:
        (args.output / f'gpu-{args.gpu}.json').write_text(json.dumps(evidence, indent=2)+'\n')
    print(json.dumps({'passed': True, 'checks': evidence['checks'], 'evidence': str(args.output / f'gpu-{args.gpu}.json')}))


if __name__ == '__main__': main()
