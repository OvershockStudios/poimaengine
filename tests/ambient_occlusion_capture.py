#!/usr/bin/env python3
"""Bounded Vulkan AO API, plane, sloped-plane and debug-view qualification.

An isolated plane has no geometric ambient occluder. Its visibility is therefore
one, independently of sampling quality, radius or exposure. This fixture does
not claim general physical AO accuracy, temporal stability or performance.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import uuid
from texture_fixture import quad, png
from gltf_fixture import glb
from scene_capture import pixels

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--gpu', type=int, default=0)
args = parser.parse_args()
run = args.output / uuid.uuid4().hex
run.mkdir(parents=True)
world = run / 'ambient-occlusion.world.json'
rev = 0
WIDTH, HEIGHT = 320, 240
PROBES = [{'x': x, 'y': y} for x, y in ((160, 120), (140, 100), (180, 140))]
BASE, AMBIENT = [.5, .25, .125], [.2, .4, .8]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

sources = [Path(__file__), Path(__file__).with_name('texture_fixture.py'),
           Path(__file__).with_name('gltf_fixture.py'), Path(__file__).with_name('scene_capture.py')]
record = {'passed': False, 'scope': __doc__, 'binary_sha256': digest(args.binary),
          'source_sha256': {p.name: digest(p) for p in sources}, 'gpu_index': args.gpu,
          'runs': [], 'samples': [], 'output': str(run.resolve())}

def uid(n):
    return f'{n:032x}'

def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

def batch(requests):
    rows = [{'jsonrpc': '2.0', 'id': i + 1, 'method': method, 'params': params}
            for i, (method, params) in enumerate(requests)]
    process = subprocess.run([str(args.binary.resolve()), 'world', native(world)],
                             input=''.join(json.dumps(row) + '\n' for row in rows),
                             capture_output=True, text=True, timeout=180)
    entry = {'requests': rows, 'stdout': process.stdout, 'stderr': process.stderr,
             'exit_code': process.returncode}
    record['runs'].append(entry)
    assert process.returncode == 0, entry
    for marker in ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD', 'Undefined-Value'):
        assert marker not in process.stdout + process.stderr, entry
    replies = [json.loads(line) for line in process.stdout.splitlines()]
    entry['responses'] = replies
    assert len(replies) == len(rows), entry
    for row, reply in zip(rows, replies):
        assert reply.get('id') == row['id'] and 'result' in reply, reply
    return [reply['result'] for reply in replies]

def component(n, kind, value):
    return {'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}

def transform(n, position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return component(n, 'Transform', {'position': list(position), 'rotation': list(rotation), 'scale': list(scale)})

def create(n):
    return {'op': 'entity.create', 'id': uid(n), 'name': f'AO fixture {n}', 'parent': None}

def environment(exposure=1):
    return component(2, 'LightingEnvironment', {'ambient': AMBIENT, 'exposure': exposure})

def edit(ops):
    global rev
    batch([('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': rev, 'ops': ops})])
    rev += 1

def close(actual, expected, tolerance, label):
    assert math.isfinite(actual) and abs(actual - expected) <= tolerance, (label, actual, expected, tolerance)

def capture(name, ao=None, view='color', probes=PROBES):
    params = {'revision': rev, 'camera': uid(1), 'path': native(run / (name + '.bmp')),
              'gpu': args.gpu, 'width': WIDTH, 'height': HEIGHT, 'samples': 1,
              'lighting_path': 'deferred', 'capture_frames': 2, 'profile': True,
              'scene_debug_view': view, 'scene_product_probes': probes}
    if ao is not None:
        params['ambient_occlusion'] = ao
    value = batch([('world.capture', params)])[0]
    assert value['capture_written'] and value['hardware'] and value['nvrhi_errors'] == 0, value
    assert value['width'] == WIDTH and value['height'] == HEIGHT and value['samples'] == 1, value
    device = value.get('gpu', value.get('gpu_name'))
    assert isinstance(device, str) and device, value
    if 'device' not in record:
        record['device'] = device
    assert record['device'] == device, value
    diag = value['render_diagnostics']
    effective = {'mode': 'none', 'quality': 'medium', 'radius': 1, **(ao or {})}
    assert diag['lighting_path'] == 'deferred', diag
    assert diag['ambient_occlusion']['mode'] == effective['mode'] and diag['ambient_occlusion']['quality'] == effective['quality'], diag
    close(diag['ambient_occlusion']['radius'], effective['radius'], 1e-6, 'effective radius')
    enabled = effective['mode'] != 'none'
    assert diag['ambient_occlusion_buffer_bytes'] == (WIDTH * HEIGHT * 4 if enabled else 0), diag
    for key in ('ambient_occlusion_gpu', 'ambient_occlusion_filter_gpu'):
        timing = diag[key]
        if enabled:
            assert timing['samples'] > 0 and math.isfinite(timing['last_ms']) and timing['last_ms'] >= 0, timing
        else:
            assert timing['samples'] == 0, timing
    actual = diag['scene_products']['probes']
    assert len(actual) == len(probes) and diag['scene_products']['view'] == view, diag
    for requested, observed in zip(probes, actual):
        assert (observed['x'], observed['y']) == (requested['x'], requested['y']), observed
        for key in ('raw_ambient_visibility', 'ambient_visibility'):
            assert math.isfinite(observed[key]) and 0 <= observed[key] <= 1, observed
    record['samples'].append({'name': name, 'effective': effective, 'probes': actual})
    return actual

def plane_oracle(probes, normal=(0, 0, 1)):
    for probe in probes:
        assert probe['surface_valid'], probe
        for key in ('raw_ambient_visibility', 'ambient_visibility'):
            close(probe[key], 1, .01, key)
        for actual, expected in zip(probe['shading_normal'], normal):
            close(actual, expected, .002, 'plane normal')
        for actual, base, ambient in zip(probe['raw_hdr'], BASE, AMBIENT):
            close(actual, base * ambient, .002, 'unoccluded ambient radiance')

try:
    description = batch([('world.describe', {})])[0]
    assert description['schema_revision'] == 50, description['schema_revision']
    for method in ('world.capture', 'runtime.capture', 'asset.animation.capture', 'runtime.play'):
        props = description['methods'][method]['properties']
        ao = props['ambient_occlusion']
        assert ao['type'] == 'object' and ao['additionalProperties'] is False, ao
        assert ao['properties']['mode'] == {'enum': ['none', 'gtao'], 'default': 'none'}, ao
        assert ao['properties']['quality'] == {'enum': ['low', 'medium', 'high'], 'default': 'medium'}, ao
        assert ao['properties']['radius'] == {'type': 'number', 'minimum': .01, 'maximum': 100, 'default': 1}, ao
        assert 'ambient_occlusion' in props['scene_debug_view']['enum'], props
    doc, blob = quad([], {'pbrMetallicRoughness': {'baseColorFactor': BASE + [1], 'metallicFactor': 0, 'roughnessFactor': .6}})
    asset_path = run / 'plane.glb'
    asset_path.write_bytes(glb(doc, blob))
    asset = batch([('asset.import', {'source': native(asset_path)})])[0]['asset']
    edit([create(1), transform(1, (0, 0, 4)), component(1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
          create(2), environment(), {'op': 'asset.instantiate', 'id': uid(100), 'name': 'AO plane', 'asset': asset},
          transform(100, scale=(10, 10, 10))])
    for name, settings in [('omitted', None), ('empty', {}), ('disabled', {'mode': 'none', 'quality': 'high', 'radius': 100})]:
        plane_oracle(capture(name, settings))
    for quality in ('low', 'medium', 'high'):
        for radius in (.01, 1, 100):
            plane_oracle(capture(f'plane-{quality}-{radius}', {'mode': 'gtao', 'quality': quality, 'radius': radius}))
    # Rotate actual geometry, rather than merely modifying its shading normal.
    edit([transform(100, rotation=(math.sin(math.pi / 12), 0, 0, math.cos(math.pi / 12)), scale=(10, 10, 10))])
    for quality in ('low', 'medium', 'high'):
        samples = capture('slope-' + quality, {'mode': 'gtao', 'quality': quality})
        plane_oracle(samples, (0, -.5, math.sqrt(3) / 2))
        assert abs(samples[1]['depth'] - samples[2]['depth']) > .0001, samples
    # A bounded patch leaves clear background. Visibility remains one there,
    # while surface_valid distinguishes it from covered geometry.
    edit([transform(100, scale=(.5, .5, .5))])
    probes = [{'x': 160, 'y': 120}, {'x': 5, 'y': 5}]
    debug_images = []
    for exposure in (0, 1, 64):
        edit([environment(exposure)])
        name = 'debug-exposure-' + str(exposure)
        samples = capture(name, {'mode': 'gtao'}, 'ambient_occlusion', probes)
        assert samples[0]['surface_valid'] and not samples[1]['surface_valid'], samples
        for sample in samples:
            close(sample['raw_ambient_visibility'], 1, .01, 'debug raw visibility')
            close(sample['ambient_visibility'], 1, .01, 'debug filtered visibility')
        image = pixels(run / (name + '.bmp'))
        assert len(image) == HEIGHT and len(image[0]) == WIDTH
        for y in range(119, 122):
            for x in range(159, 162):
                assert min(image[y][x]) >= 252, ('AO debug must display unoccluded geometry as white', image[y][x])
        debug_images.append(image)
    assert debug_images[0] == debug_images[1] == debug_images[2], 'Exposure affected AO debug output'
    # Normal-map shading detail must not become geometric occlusion. Import an
    # actual mapped material and verify its observed normal, rather than merely
    # asserting that AO stayed white when the texture might not have loaded.
    normal_bytes = [204, 76, 230]
    mapped_doc, mapped_blob = quad([png(1, 1, normal_bytes + [255])], {
        'pbrMetallicRoughness': {'baseColorFactor': BASE + [1], 'metallicFactor': 0, 'roughnessFactor': .6},
        'normalTexture': {'index': 0}})
    mapped_path = run / 'normalmapped.glb'
    mapped_path.write_bytes(glb(mapped_doc, mapped_blob))
    mapped_asset = batch([('asset.import', {'source': native(mapped_path)})])[0]['asset']
    edit([transform(100, position=(1000, 0, 0)), environment(1),
          {'op': 'asset.instantiate', 'id': uid(200), 'name': 'Mapped AO plane', 'asset': mapped_asset},
          transform(200, scale=(10, 10, 10))])
    # The fixture UV basis has a downward V axis.
    mapped_normal = [normal_bytes[0] / 255 * 2 - 1, -(normal_bytes[1] / 255 * 2 - 1), normal_bytes[2] / 255 * 2 - 1]
    length = math.sqrt(sum(value * value for value in mapped_normal))
    mapped_normal = [value / length for value in mapped_normal]
    for quality in ('low', 'medium', 'high'):
        plane_oracle(capture('normalmapped-' + quality, {'mode': 'gtao', 'quality': quality}), mapped_normal)
    assert digest(args.binary) == record['binary_sha256'], 'Binary changed during qualification'
    assert {p.name: digest(p) for p in sources} == record['source_sha256'], 'Fixture changed during qualification'
    record['passed'] = True
except Exception as error:
    record['error'] = str(error)
    raise
finally:
    record['images'] = {p.stem: {'path': str(p.resolve()), 'sha256': digest(p)} for p in run.glob('*.bmp')}
    evidence = run / 'evidence.json'
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'gpu_index': args.gpu, 'device': record.get('device'),
                      'captures': len(record['samples']), 'evidence': str(evidence.resolve())}))
