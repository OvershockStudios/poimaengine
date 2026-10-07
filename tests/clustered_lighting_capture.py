#!/usr/bin/env python3
"""Compare real clustered Vulkan captures against the same renderer's full light loop.

Renderer readback only, not physical interaction or performance qualification.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import uuid
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels


def require(ok, detail):
    if not ok:
        raise RuntimeError(str(detail))


def main():
    require(__debug__, 'Run without -O: imported BMP fixture validation requires assertions.')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    args = parser.parse_args()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    world = run / 'clustered.world.json'
    record = {'passed': False, 'gpu_index': args.gpu, 'runs': [], 'comparisons': [],
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'observation': 'Vulkan renderer readback; no physical input or timing claim'}
    revision = 0

    def native(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    def batch(requests):
        rows = [{'jsonrpc': '2.0', 'id': i+1, 'method': method, 'params': params}
                for i, (method, params) in enumerate(requests)]
        entry = {'requests': rows}
        record['runs'].append(entry)
        process = subprocess.run([str(args.binary.resolve()), 'world', native(world)],
                                 input=''.join(json.dumps(row)+'\n' for row in rows),
                                 text=True, capture_output=True, timeout=180)
        entry.update(exit_code=process.returncode, stderr=process.stderr, stdout=process.stdout)
        require(process.returncode == 0, entry)
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        entry['responses'] = replies
        require(len(replies) == len(rows), entry)
        for request, reply in zip(rows, replies):
            require(reply.get('jsonrpc') == '2.0' and reply.get('id') == request['id'] and 'result' in reply, reply)
        return [reply['result'] for reply in replies]

    def uid(n): return f'{n:032x}'
    def create(n): return {'op': 'entity.create', 'id': uid(n), 'name': f'Cluster fixture {n}', 'parent': None}
    def component(n, kind, value): return {'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
    def transform(n, position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
        return component(n, 'Transform', {'position': list(position), 'rotation': list(rotation), 'scale': list(scale)})
    def light(n, kind='point', **values):
        return component(n, 'Light', {'kind': kind, 'color': [1, .7, .3], 'intensity': .8, 'enabled': True, **({'range': .85} if kind != 'directional' else {}), **values})
    def edit(ops):
        nonlocal revision
        for start in range(0, len(ops), 240):
            batch([('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': revision, 'ops': ops[start:start+240]})])
            revision += 1
    def compare(name, samples=1, require_energy=True, expected_lights=None, overflow=False, active=True, captured_results=None):
        paths = [run / f'{name}-{mode}.bmp' for mode in ('clustered', 'brute')]
        results = captured_results if captured_results is not None else batch([('world.capture', {'revision': revision, 'camera': uid(1), 'path': native(path),
                          'gpu': args.gpu, 'width': 384, 'height': 216, 'samples': samples,
                          'clustered_lighting': clustered}) for path, clustered in zip(paths, (True, False))])
        for result in results:
            require(result.get('capture_written') and result.get('hardware') and result.get('nvrhi_errors') == 0, result)
        assignments = [result['render_diagnostics']['light_assignment'] for result in results]
        clustered, brute = assignments
        require(clustered['requested'] and clustered['active'] == active, clustered)
        require(clustered['statistics_available'] if active else bool(clustered['fallback_reason']), clustered)
        require(not brute['requested'] and not brute['active'] and not brute['statistics_available'], brute)
        require(all(brute[key] is None for key in ('candidate_references', 'overflow_clusters', 'max_candidates')), brute)
        if not active:
            require(not clustered['statistics_available'] and all(clustered[key] is None for key in ('candidate_references', 'overflow_clusters', 'max_candidates')), clustered)
        require(clustered['grid'] == [16, 9, 24] and clustered['cluster_count'] == 3456 and clustered['capacity'] == 64, clustered)
        require(0 < clustered['buffer_bytes'] <= 2*1024*1024, clustered)
        if active:
            require(clustered['candidate_references'] <= clustered['cluster_count']*clustered['light_count'], clustered)
            require(clustered['max_candidates'] <= clustered['light_count'], clustered)
            require((clustered['overflow_clusters'] > 0) if overflow else clustered['overflow_clusters'] == 0, clustered)
        if expected_lights is not None:
            require(clustered['light_count'] == expected_lights and brute['light_count'] == expected_lights, assignments)
        images = [pixels(path) for path in paths]
        require(len(images[0]) == len(images[1]) and all(len(a) == len(b) for a, b in zip(*images)), 'Capture dimensions differ')
        difference = max(abs(a-b) for ra, rb in zip(*images) for pa, pb in zip(ra, rb) for a, b in zip(pa, pb))
        lit = sum(max(pixel) > 45 for row in images[1] for pixel in row)
        item = {'name': name, 'samples': samples, 'max_channel_difference': difference,
                'brute_lit_pixels': lit, 'reports': results,
                'images': [{'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in paths]}
        record['comparisons'].append(item)
        # Both paths evaluate retained lights in original order; allow one output
        # quantization step, not a percentage which could hide a dropped light.
        require(difference <= 1, item)
        require(not require_energy or lit > 500, ('Fixture has insufficient visible signal', item))
        return images[1]

    try:
        doc, blob = quad([], {'pbrMetallicRoughness': {'baseColorFactor': [.6, .6, .6, 1], 'metallicFactor': 0, 'roughnessFactor': .7}})
        mesh = run / 'receiver.glb'
        mesh.write_bytes(glb(doc, blob))
        asset = batch([('asset.import', {'source': native(mesh)})])[0]['asset']
        edit([create(1), transform(1, (0, 0, 6)), component(1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
              create(2), component(2, 'LightingEnvironment', {'ambient': [0, 0, 0], 'exposure': .2}),
              {'op': 'asset.instantiate', 'id': uid(10), 'name': 'Receiver', 'asset': asset}, transform(10, scale=(5, 3, 1))])
        # More than the former64-light ceiling, distributed across the receiver.
        edit([op for i in range(129) for op in (create(1000+i), transform(1000+i, (-4+(i % 17)*.5, -2+(i//17)*.5, .4)), light(1000+i))])
        compare('sparse-129', 1, expected_lights=129)
        compare('sparse-129-msaa', 4, expected_lights=129)
        # Only the final light contributes: protects high indices from truncation.
        edit([light(1000+i, intensity=4 if i == 128 else 0, range=20 if i == 128 else .85) for i in range(129)])
        last = compare('last-index-128')
        edit([light(1128, intensity=0, range=20)])
        dark = compare('all-zero', require_energy=False)
        require(last != dark, 'Final-index light did not alter the actual image')
        # Exercise the actual final enabled table entry1023 without dense
        # overflow masking assignment: preceding additional lights are distant.
        edit([op for i in range(129, 1024) for op in
              (create(1000+i), transform(1000+i, (0, 0, .4) if i == 1023 else (1000+i, 0, 0)),
               light(1000+i, intensity=4 if i == 1023 else 0, range=20 if i == 1023 else .1))])
        tail = compare('last-index-1023', expected_lights=1024)
        require(tail != dark, 'Final1024-entry light table slot did not alter the image')
        edit([light(1000+i, enabled=False) for i in range(129, 1024)])
        #65 overlapping finite spheres naturally exceed the64-entry cluster cap.
        edit([light(1000+i, intensity=.04, range=20, enabled=i < 65) for i in range(129)])
        compare('dense-overflow-65', 4, expected_lights=65, overflow=True)
        # Unbounded local lights and directional lights cannot use finite culling.
        edit([light(1000+i, enabled=False) for i in range(129)])
        edit([transform(1000, (0, 0, 2)), light(1000, range=0, intensity=4),
              transform(1001, (0, 0, 3)), light(1001, 'spot', range=0, intensity=3, inner_angle=15, outer_angle=89),
              light(1002, 'directional', intensity=.5)])
        compare('global-unbounded', expected_lights=3, active=False)
        # Restore finite lights: a global-only fixture cannot test conservative
        # camera-dependent assignment, even when its images match.
        edit([op for i in range(129) for op in
              (transform(1000+i, (-4+(i % 17)*.5, -2+(i//17)*.5, .4)), light(1000+i))])
        # Rotated, translated rigid camera and a very large near/far ratio.
        angle = math.radians(8)/2
        edit([transform(1, (.5, .3, 6), (0, math.sin(angle), 0, math.cos(angle)))])
        compare('camera-rigid-finite', 4, expected_lights=129)
        edit([component(1, 'Camera', {'vertical_fov': 70, 'near': .001, 'far': 10000000})])
        compare('camera-depth-extremes', 4, expected_lights=129, active=False)
        # Mixed global/finite contribution, an offscreen sphere which reaches
        # the receiver, and one shadowed spot retain original shadow indices.
        edit([transform(1, (0, 0, 6)), component(1, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
              *[light(1000+i, enabled=False) for i in range(129)]])
        edit([transform(1000, (0, 0, 3)), light(1000, 'spot', range=6, intensity=40, inner_angle=20, outer_angle=50,
              shadow={'enabled': True}), light(1001, 'directional', intensity=.2),
              transform(1002, (6.4, 0, .4)), light(1002, range=2, intensity=40)])
        mixed = compare('mixed-shadow-offscreen', 4, expected_lights=3)
        edit([light(1002, enabled=False)])
        without_offscreen = compare('mixed-without-offscreen', 4, expected_lights=2)
        affected = sum(max(abs(a-b) for a, b in zip(pa, pb)) > 2
                       for ra, rb in zip(mixed, without_offscreen) for pa, pb in zip(ra, rb))
        record['offscreen_contribution_pixels'] = affected
        require(affected > 100, 'Offscreen point did not contribute visibly to the receiver')
        # Place actual receiver geometry on each side of an internal logarithmic
        # depth boundary. This derives only the documented grid boundary, not
        # the implementation's sphere-plane rejection algorithm.
        depth = .1 * (100/.1)**(14/24)
        edge_x = (2*9/16-1)*depth*math.tan(math.radians(60)/2)*(384/216)
        edit([light(1000, enabled=False), light(1001, enabled=False),
              transform(1002, (edge_x, 0, 6-depth+.4)), light(1002, range=.8, intensity=4)])
        for side in (-1, 1):
            edit([transform(10, (0, 0, 6-depth+side*.002), scale=(5, 3, 1))])
            compare(f'depth-boundary-{side}', 4, expected_lights=1)
        # Runtime light movement must use the live hierarchical transform.
        edit([transform(10, scale=(5, 3, 1)), create(5000), transform(5000, (0, 1, 0)),
              component(5000, 'BoxCollider', {'half_extents': [.1, .1, .1], 'motion': 'dynamic', 'mass': 1, 'friction': .5, 'restitution': 0}),
              {'op': 'entity.reparent', 'id': uid(1002), 'parent': uid(5000), 'mode': 'keep_local'},
              transform(1002, (0, 0, .4)), light(1002, range=2, intensity=4)])
        session = uid(9000)
        def runtime_pair(name, tick):
            return [('runtime.capture', {'session_id': session, 'tick': tick, 'camera': uid(1),
                     'path': native(run / f'{name}-{mode}.bmp'), 'gpu': args.gpu, 'width': 384, 'height': 216,
                     'samples': 4, 'clustered_lighting': enabled})
                    for mode, enabled in (('clustered', True), ('brute', False))]
        rows = batch([('runtime.start', {'session_id': session, 'revision': revision}),
                      *runtime_pair('runtime-initial', 0),
                      ('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'ticks': 30}),
                      *runtime_pair('runtime-moved', 30)])
        initial = compare('runtime-initial', 4, expected_lights=1, captured_results=rows[1:3])
        moved = compare('runtime-moved', 4, expected_lights=1, captured_results=rows[4:6])
        require(initial != moved, 'Runtime parent movement did not change the light image')
        # Hierarchical composition reaches a finite GPU light position whose
        # squared distance overflows float32. A nearby light must remain visibly
        # effective; equal black images cannot satisfy this comparison.
        control = compare('extreme-light-control', 4, expected_lights=1)
        edit([create(6000), transform(6000, scale=(1e9, 1e9, 1e9)),
              create(6001), transform(6001, scale=(1e9, 1e9, 1e9)),
              {'op': 'entity.reparent', 'id': uid(6001), 'parent': uid(6000), 'mode': 'keep_local'},
              {'op': 'entity.reparent', 'id': uid(1003), 'parent': uid(6001), 'mode': 'keep_local'},
              transform(1003, (1e9, 0, 0)), light(1003, intensity=1, range=1)])
        resolved = batch([('world.lighting', {'revision': revision})])[0]
        extreme_light = next(item for item in resolved['lights'] if item['id'] == uid(1003))
        position = extreme_light['position']
        require(all(math.isfinite(value) and abs(value) < 3.402823466e38 for value in position), extreme_light)
        require(sum(value*value for value in position) > 3.402823466e38, extreme_light)
        record['extreme_light'] = extreme_light
        extreme = compare('extreme-light-position', 4, expected_lights=2)
        require(extreme == control, 'Distant extreme light changed the nearby light contribution')
        require(len(record['comparisons']) == 17, 'Unexpected paired capture count')
        record['passed'] = True
    except BaseException as error:
        record['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (run / 'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({'passed': True, 'evidence': str(run / 'evidence.json'), 'comparisons': len(record['comparisons'])}))


if __name__ == '__main__':
    main()
