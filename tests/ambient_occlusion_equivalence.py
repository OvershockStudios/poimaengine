#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paired hardware captures for the bounded GTAO integral optimization.

Run the retained binary without --reference, then the candidate with that result
directory. Native Windows Python is required. This checks image/probe equivalence
on deterministic fixtures, not physical AO accuracy or production performance.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient
from gltf_fixture import glb
from texture_fixture import quad
from scene_capture import pixels

FORMAT = 'poima.ao-integral-equivalence'
WIDTH, HEIGHT = 320, 240
BASE = [.5, .25, .125]
AMBIENT = [.2, .4, .8]
IMAGE_BUDGET, PROBE_BUDGET = 1, .002
LATTICE = [dict(x=x, y=y) for y in (5, 80, 100, 120, 140, 160, 235)
           for x in (5, 80, 140, 160, 180, 240, 315)]
PROBES = LATTICE + [dict(x=240, y=115), dict(x=240, y=125)]
WORK_PROBES = [dict(x=x, y=y) for y in (90, 240, 390, 540, 690, 840, 990)
               for x in (160, 426, 693, 960, 1226, 1493, 1760)]


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def object_hash(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def read_json(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 64 * 1024 * 1024,
         'JSON must be a bounded regular file.')

    def unique(pairs):
        result = {}
        for key, value in pairs:
            need(key not in result, 'Duplicate JSON key.')
            result[key] = value
        return result

    def nonfinite(value):
        raise ValueError('Nonfinite JSON: ' + value)

    def floating(value):
        result = float(value)
        need(math.isfinite(result), 'Nonfinite JSON float.')
        return result

    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_float=floating, parse_constant=nonfinite)


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def inventory(directory):
    need(directory.is_dir() and not directory.is_symlink(), 'Cooked store is missing or linked.')
    rows, seen, total = [], set(), 0
    for path in sorted(directory.rglob('*')):
        mode = path.lstat().st_mode
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)() and
             (stat.S_ISREG(mode) or stat.S_ISDIR(mode)), 'Cooked store contains a link/special file.')
        relative = path.relative_to(directory).as_posix()
        need(relative.casefold() not in seen, 'Cooked store case collision.')
        seen.add(relative.casefold())
        if path.is_dir():
            continue
        size = path.stat().st_size
        total += size
        need(size <= 256 * 1024 * 1024 and total <= 1024 * 1024 * 1024 and len(rows) < 10000,
             'Cooked store exceeds the bounded fixture budget.')
        rows.append(dict(path=relative, bytes=size, sha256=sha(path)))
    return rows


def binary_dependencies(binary):
    # The Windows CLI imports phonon.dll. The optional desktop bridge in some
    # build folders is not loaded by this CLI and is deliberately outside this pin.
    # Windows system/driver libraries are not claimed as a captured closure.
    rows = []
    path = binary.parent / 'phonon.dll'
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 256 * 1024 * 1024,
         'Required staged phonon.dll is missing or exceeds the bounded pin budget.')
    rows.append(dict(path=path.name, bytes=path.stat().st_size, sha256=sha(path)))
    return rows


def uid(value):
    return f'{value:032x}'


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def pose(position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return dict(position=list(position), rotation=list(rotation), scale=list(scale))


def create(entity, name, transform):
    return [dict(op='entity.create', id=entity, name=name, parent=None),
            component(entity, 'Transform', transform)]


def generated_quad(normal=(0, 0, 1), positions=None):
    """Reviewable parameterized geometry; all vertex bytes follow this recipe."""
    doc, blob = quad([], dict(doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=BASE + [1], metallicFactor=0, roughnessFactor=.6)))
    data = bytearray(blob)
    for index in range(4):
        struct.pack_into('<3f', data, index * 32 + 12, *normal)
        if positions is not None:
            struct.pack_into('<3f', data, index * 32, *positions[index])
    if positions is not None:
        doc['accessors'][0]['min'] = [min(vertex[axis] for vertex in positions) for axis in range(3)]
        doc['accessors'][0]['max'] = [max(vertex[axis] for vertex in positions) for axis in range(3)]
    return glb(doc, bytes(data))


def cases():
    contact = generated_quad((-1, 0, 0), [(.6, -100, 0), (.6, 100, 0),
                                                       (.6, 100, 2), (.6, -100, 2)])
    ordinary = generated_quad()
    x_rotation = lambda degrees: [math.sin(math.radians(degrees) / 2), 0, 0,
                                  math.cos(math.radians(degrees) / 2)]
    return [
        dict(name='plane', geometry=[('plane', ordinary, pose(scale=(100, 100, 1)))],
             settings=[('low', .01)]),
        dict(name='corner', geometry=[('plane', ordinary, pose(scale=(100, 100, 1))),
                                     ('contact-wall', contact, pose())],
             settings=[('medium', 1.25), ('high', 100)]),
        dict(name='sloped-grazing', geometry=[
            ('sloped', ordinary, pose((-1.4, 0, 0), x_rotation(30), (1.1, 1.8, 1))),
            ('grazing', ordinary, pose((1.4, 0, 0), x_rotation(78), (1.1, 1.8, 1)))],
             settings=[('low', 1.25), ('high', 100)]),
        dict(name='imported-odd-normals', geometry=[
            ('rear-normal', generated_quad((0, 0, -1)), pose((-1.4, 0, 0), scale=(1.1, 1.8, 1))),
            ('odd-normal', generated_quad((1, 0, .02)), pose((1.4, 0, 0), scale=(1.1, 1.8, 1)))],
             settings=[('medium', 1.25)]),
        dict(name='background-edges', geometry=[('bounded-plane', ordinary, pose(scale=(.5, .5, 1)))],
             settings=[('high', .01)]),
    ]


def normalized_world(path):
    document = read_json(path)
    # New worlds choose their own identity. Every authored field and durable
    # receipt remains in the comparison; capture does not mutate either.
    document.pop('world_id', None)
    return object_hash(document)


class Owner:
    def __init__(self, binary, world, record, label):
        self.record, self.label = record, label
        self.client = WorldClient.open(binary, world, cwd=ROOT)
        self.entry = dict(label=label, calls=[], exit_code=None)
        record['owners'].append(self.entry)
        self.deadline = time.monotonic() + 1200

    def initialize(self):
        # Called only after the caller records this owner for failure cleanup.
        self.revision = self.rpc('world.inspect', {})['revision']
        discovery = self.rpc('world.describe', {})
        need(type(discovery['schema_revision']) is int and discovery['schema_revision'] >= 72,
             'AO fixture requires current supported discovery (schema >=72).')
        properties = discovery['methods']['world.capture']['properties']
        ao = properties['ambient_occlusion']
        need(ao['type'] == 'object' and ao['additionalProperties'] is False and
             ao['properties']['mode']['enum'] == ['none', 'gtao'] and
             ao['properties']['quality']['enum'] == ['low', 'medium', 'high'] and
             ao['properties']['radius']['minimum'] == .01 and ao['properties']['radius']['maximum'] == 100 and
             'ambient_occlusion' in properties['scene_debug_view']['enum'], 'AO discovery contract changed.')
        self.entry['schema_revision'] = discovery['schema_revision']

    def rpc(self, method, params):
        remaining = self.deadline - time.monotonic()
        need(remaining > 0, 'Owner deadline expired.')
        entry = dict(method=method, params=copy.deepcopy(params))
        self.entry['calls'].append(entry)
        try:
            result = self.client.call(method, params, timeout=min(180, remaining))
        except BaseException as error:
            entry['error'] = repr(error)
            raise
        entry['result'] = result
        return result

    def transact(self, name, operations):
        request = hashlib.sha256(('ao-equivalence-v1/' + self.label + '/' + name).encode()).hexdigest()[:32]
        self.revision = self.rpc('world.transact', dict(request_id=request,
            base_revision=self.revision, ops=operations))['revision']

    def close(self):
        try:
            self.client.close()
        finally:
            self.entry.update(exit_code=self.client.transport.returncode,
                stderr=self.client.transport.stderr_tail, stderr_truncated=self.client.transport.stderr_truncated)
        need(self.entry['exit_code'] == 0 and not self.entry['stderr'] and not self.entry['stderr_truncated'],
             'Native owner did not exit cleanly or emitted stderr.')


def compare_images(candidate, reference, allowance):
    left, right = pixels(candidate), pixels(reference)
    need(len(left) == len(right) and len(left[0]) == len(right[0]), 'Image extents differ.')
    maximum, changed = 0, 0
    for left_row, right_row in zip(left, right):
        for a, b in zip(left_row, right_row):
            delta = max(abs(x - y) for x, y in zip(a, b))
            maximum = max(maximum, delta)
            changed += int(delta != 0)
    need(maximum <= allowance, f'Image difference {maximum} exceeds fixed budget {allowance}.')
    return dict(maximum_channel_difference=maximum, changed_pixels=changed, budget=allowance)


def capture(owner, directory, label, camera, size, probes, settings, view, record, reference):
    width, height = size
    path = directory / (label + '.bmp')
    need(not path.exists(), 'Capture destination already exists.')
    params = dict(revision=owner.revision, camera=camera, path=str(path), gpu=record['gpu_index'],
        width=width, height=height, samples=1, lighting_path='deferred', capture_frames=2, profile=True,
        scene_debug_view=view, scene_product_probes=probes, ambient_occlusion=settings)
    immutable = sha(directory / 'world.json')
    response = owner.rpc('world.capture', params)
    row = dict(label=label, image=str(path.relative_to(Path(record['output']))),
        request={**params, 'path': '@capture/' + label + '.bmp'}, world_sha256=immutable,
        normalized_world_sha256=normalized_world(directory / 'world.json'), result=response)
    record['captures'].append(row)
    need(sha(directory / 'world.json') == immutable, 'Capture mutated the authored world.')
    need(response['capture_written'] and response['hardware'] and response['nvrhi_errors'] == 0 and
         response['width'] == width and response['height'] == height and response['samples'] == 1,
         'Native hardware capture failed.')
    need(path.is_file(), 'Native capture image is missing.')
    image = pixels(path)
    need(len(image) == height and all(len(line) == width for line in image), 'Actual image extent differs from the request.')
    row['image_sha256'] = sha(path)
    actual_device = response.get('gpu', response.get('gpu_name'))
    need(isinstance(actual_device, str) and actual_device, 'Actual GPU identity is missing.')
    if 'device' not in record:
        record['device'] = actual_device
    need(record['device'] == actual_device, 'Device changed during the cohort.')
    diagnostic = response['render_diagnostics']
    enabled = settings['mode'] == 'gtao'
    need(diagnostic['lighting_path'] == 'deferred' and diagnostic['ambient_occlusion']['mode'] == settings['mode'] and
         diagnostic['ambient_occlusion']['quality'] == settings['quality'] and
         abs(diagnostic['ambient_occlusion']['radius'] - settings['radius']) <= 1e-6,
         'Effective AO settings differ.')
    need(diagnostic['ambient_occlusion_buffer_bytes'] == (width * height * 4 if enabled else 0),
         'AO resource allocation differs.')
    for field in ('ambient_occlusion_gpu', 'ambient_occlusion_filter_gpu'):
        timing = diagnostic[field]
        need((timing['samples'] > 0 and math.isfinite(timing['last_ms']) and timing['last_ms'] >= 0)
             if enabled else timing['samples'] == 0, 'AO timing/pass admission differs.')
    observed = diagnostic['scene_products']['probes']
    need(len(observed) == len(probes) and diagnostic['scene_products']['view'] == view, 'Missing/mismatched product probes.')
    for requested, sample in zip(probes, observed):
        need((sample['x'], sample['y']) == (requested['x'], requested['y']), 'Probe coordinate mismatch.')
        for field in ('raw_ambient_visibility', 'ambient_visibility'):
            need(math.isfinite(sample[field]) and 0 <= sample[field] <= 1, 'AO probe outside finite [0,1].')
            if not enabled:
                need(abs(sample[field] - 1) <= 1e-6, 'Disabled AO changed visibility.')
    row['probes'] = observed
    need(any(sample['surface_valid'] for sample in observed), 'Fixture contains no probed surface.')
    if reference is not None:
        old = reference['captures_by_label'][label]
        need(old['request'] == row['request'] and old['image'] == row['image'] and
             old['normalized_world_sha256'] == row['normalized_world_sha256'],
             'Reference world, camera, settings or request recipe differs.')
        need(reference['device'] == actual_device, 'Reference and candidate use different actual GPUs.')
        old_image = reference['directory'] / old['image']
        need(sha(old_image) == old['image_sha256'], 'Retained reference image bytes changed.')
        need(len(old['probes']) == len(observed), 'Reference probes differ.')
        largest = 0
        for previous, current in zip(old['probes'], observed):
            need((previous['x'], previous['y'], previous['surface_valid']) ==
                 (current['x'], current['y'], current['surface_valid']), 'Surface/probe identity differs.')
            for field in ('raw_ambient_visibility', 'ambient_visibility'):
                difference = abs(previous[field] - current[field])
                need(math.isfinite(previous[field]) and 0 <= previous[field] <= 1 and difference <= PROBE_BUDGET,
                     f'{label}: {field} differs by {difference}, fixed budget {PROBE_BUDGET}.')
                largest = max(largest, difference)
        row['comparison'] = dict(probe_maximum_difference=largest, probe_budget=PROBE_BUDGET,
            image=compare_images(path, old_image, IMAGE_BUDGET if enabled else 0))
    return observed


def fixture_oracles(name, observed, enabled, quality, radius):
    lookup = {(value['x'], value['y']): value for value in observed}
    if name == 'plane':
        for sample in observed:
            need(sample['surface_valid'], 'Infinite-plane fixture lost its geometry.')
            if enabled:
                need(abs(sample['raw_ambient_visibility'] - 1) <= .01 and
                     abs(sample['ambient_visibility'] - 1) <= .01, 'Isolated plane self-darkened.')
    if name == 'corner' and enabled and quality == 'medium' and radius == 1.25:
        sample = lookup[(160, 120)]
        need(sample['surface_valid'] and .1 < sample['ambient_visibility'] < .99 and
             sample['raw_ambient_visibility'] < .99, 'Contact fixture did not exercise nontrivial AO.')
    if name == 'sloped-grazing':
        for x, expected in ((80, (0, -.5, math.sqrt(.75))), (240, (0, -math.sin(math.radians(78)), math.cos(math.radians(78))))):
            sample = lookup[(x, 120)]
            need(sample['surface_valid'] and max(abs(a - b) for a, b in zip(sample['shading_normal'], expected)) <= .003,
                 'Imported sloped/grazing normal was not observed.')
    if name == 'imported-odd-normals':
        rear, odd = lookup[(80, 120)], lookup[(240, 120)]
        need(rear['surface_valid'] and rear['shading_normal'][2] < -.99 and odd['surface_valid'] and
             odd['shading_normal'][0] > .99 and abs(odd['shading_normal'][2]) < .03,
             'Rear/odd imported normal fixture was not observed.')
    if name == 'background-edges':
        need(lookup[(160, 120)]['surface_valid'] and not lookup[(5, 5)]['surface_valid'],
             'Background/geometry boundary fixture was not observed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--workload-world', type=Path)
    args = parser.parse_args()
    need(os.name == 'nt', 'Use native Windows Python for this hardware cohort.')
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    need(binary.is_file() and not output.exists() and args.gpu >= 0, 'Supply a real binary, new output and nonnegative GPU index.')
    sources = [Path(__file__), ROOT / 'tests/gltf_fixture.py', ROOT / 'tests/texture_fixture.py', ROOT / 'tests/scene_capture.py']
    sources += sorted((ROOT / 'tools/python/poima_client').glob('*.py'))
    sources.append(ROOT / 'tools/python/poima_client/core_responses.v1.json')
    source_pins = {path.relative_to(ROOT).as_posix(): sha(path) for path in sources}
    record = dict(format=FORMAT, version=1, passed=False, role='candidate' if args.reference else 'reference',
        source_pins=source_pins, binary_sha256=sha(binary), binary_dependencies=binary_dependencies(binary),
        gpu_index=args.gpu, output=str(output),
        owners=[], captures=[], fixtures=[], image_budget=IMAGE_BUDGET, probe_budget=PROBE_BUDGET,
        scope='Bounded paired hardware image/probe equivalence; not general quality, physical-input or performance qualification.')
    reference = None
    if args.reference:
        directory = args.reference.resolve(strict=True)
        reference = read_json(directory / 'evidence.json')
        need(reference.get('format') == FORMAT and reference.get('version') == 1 and reference.get('passed') is True and
             reference.get('role') == 'reference' and reference['source_pins'] == source_pins and
             reference['gpu_index'] == args.gpu and reference['image_budget'] == IMAGE_BUDGET and
             reference['probe_budget'] == PROBE_BUDGET and reference['binary_dependencies'] == record['binary_dependencies'],
             'Reference cohort/recipe/source/dependencies/budgets are incompatible.')
        need(all(owner['exit_code'] == 0 and not owner.get('stderr') and not owner.get('stderr_truncated')
                 for owner in reference['owners']), 'Reference native owners are not clean.')
        reference['directory'] = directory
        reference['captures_by_label'] = {row['label']: row for row in reference['captures']}
        need(len(reference['captures_by_label']) == len(reference['captures']), 'Duplicate reference labels.')
        for fixture in reference['fixtures']:
            name = fixture['name']
            need(isinstance(name, str) and name in {spec['name'] for spec in cases()} |
                 {'workload-initial', 'workload-interior'}, 'Unexpected reference fixture name.')
            world = directory / name / 'world.json'
            need(sha(world) == fixture['world_sha256'] and normalized_world(world) == fixture['normalized_world_sha256'] and
                 inventory(Path(str(world) + '.assets')) == fixture['assets'], 'Retained reference world/content changed.')
            for generated in fixture['generated']:
                name = generated['path']
                need(Path(name).name == name and name.endswith('.glb') and
                     sha(world.parent / name) == generated['sha256'], 'Retained generated reference input changed.')
        record['reference_evidence_sha256'] = sha(directory / 'evidence.json')
    output.mkdir(parents=True)
    active, completed, workload_original = None, False, None
    try:
        specs = cases()
        if args.workload_world:
            base = args.workload_world.resolve(strict=True)
            store = Path(str(base) + '.assets')
            need(not output.is_relative_to(store) and not base.is_relative_to(output), 'Output overlaps workload input.')
            workload_original = dict(world=base, world_sha256=sha(base), store=store, assets=inventory(store))
            record['workload_input'] = dict(world_sha256=workload_original['world_sha256'], assets=workload_original['assets'])
            specs += [dict(name='workload-initial', workload=base, camera_position=[-4, 1.6, 7], settings=[('medium', 1)]),
                      dict(name='workload-interior', workload=base, camera_position=[3.2, 1.6, 3], settings=[('medium', 1)])]
        expected_count = 19 + (6 if workload_original else 0)
        if reference is not None:
            need(reference.get('workload_input') == record.get('workload_input') and len(reference['captures']) == expected_count,
                 'Reference workload closure or expected capture set differs.')
        for spec in specs:
            name = spec['name']
            directory = output / name
            directory.mkdir()
            world = directory / 'world.json'
            camera = 'e0950000000000000000000000000001'
            if 'workload' in spec:
                shutil.copy2(spec['workload'], world)
                shutil.copytree(Path(str(spec['workload']) + '.assets'), Path(str(world) + '.assets'))
                need(sha(world) == workload_original['world_sha256'] and
                     inventory(Path(str(world) + '.assets')) == workload_original['assets'], 'Workload copy bytes differ.')
                document = read_json(world)
                need(camera not in document['entities'] and camera not in document.get('retired_ids', []), 'Workload camera namespace occupied.')
            active = Owner(binary, world, record, name)
            active.initialize()
            camera_position = spec.get('camera_position', [0, 0, 4])
            operations = create(camera, 'AO equivalence camera', pose(camera_position)) + [component(camera, 'Camera',
                dict(vertical_fov=60, near=.1, far=100))]
            if 'workload' not in spec:
                operations += create(uid(2), 'Ambient environment', pose()) + [component(uid(2), 'LightingEnvironment',
                    dict(ambient=AMBIENT, exposure=1))]
                generated = []
                for index, (mesh_name, content, transform) in enumerate(spec['geometry']):
                    path = directory / (mesh_name + '.glb')
                    path.write_bytes(content)
                    asset = active.rpc('asset.import', dict(source=str(path)))['asset']
                    generated.append(dict(path=path.name, sha256=sha(path), asset=asset))
                    entity = uid(100 + index)
                    operations += [dict(op='asset.instantiate', id=entity, name=mesh_name, asset=asset),
                                   component(entity, 'Transform', transform)]
            else:
                generated = []
            active.transact('fixture', operations)
            fixture = dict(name=name, normalized_world_sha256=normalized_world(world), world_sha256=sha(world),
                generated=generated, assets=inventory(Path(str(world) + '.assets')))
            record['fixtures'].append(fixture)
            if reference is not None:
                previous = next(row for row in reference['fixtures'] if row['name'] == name)
                need({key: value for key, value in fixture.items() if key != 'world_sha256'} ==
                     {key: value for key, value in previous.items() if key != 'world_sha256'}, 'Authored fixture/content differs from reference.')
            probes, size = (WORK_PROBES, (1920, 1080)) if 'workload' in spec else (PROBES, (WIDTH, HEIGHT))
            disabled = dict(mode='none', quality='medium', radius=1)
            observed = capture(active, directory, name + '-disabled-color', camera, size, probes, disabled, 'color', record, reference)
            fixture_oracles(name, observed, False, 'medium', 1)
            for quality, radius in spec['settings']:
                settings = dict(mode='gtao', quality=quality, radius=radius)
                for view in ('color', 'ambient_occlusion'):
                    label = f'{name}-{quality}-{radius}-{view}'
                    observed = capture(active, directory, label, camera, size, probes, settings, view, record, reference)
                    fixture_oracles(name, observed, True, quality, radius)
            need(sha(world) == fixture['world_sha256'] and inventory(Path(str(world) + '.assets')) == fixture['assets'],
                 'Captures changed fixture/content.')
            active.close()
            active = None
        need(len(record['captures']) == expected_count, 'Incomplete capture cohort.')
        if reference is not None:
            need({row['label'] for row in record['captures']} == set(reference['captures_by_label']), 'Capture sets differ.')
        completed = True
    except BaseException:
        record['error'] = traceback.format_exc()
        raise
    finally:
        try:
            if active is not None:
                active.close()
            need(sha(binary) == record['binary_sha256'] and
                 binary_dependencies(binary) == record['binary_dependencies'] and
                 {path.relative_to(ROOT).as_posix(): sha(path) for path in sources} == source_pins,
                 'Frozen binary/source inputs changed.')
            if workload_original is not None:
                need(sha(workload_original['world']) == workload_original['world_sha256'] and
                     inventory(workload_original['store']) == workload_original['assets'], 'Original workload input changed.')
            if reference is not None:
                need(sha(reference['directory'] / 'evidence.json') == record['reference_evidence_sha256'], 'Reference evidence changed.')
        except BaseException:
            record['cleanup_error'] = traceback.format_exc()
            raise
        finally:
            record['passed'] = completed and 'cleanup_error' not in record
            save_json(output / 'evidence.json', record)


if __name__ == '__main__':
    main()
