#!/usr/bin/env python3
"""Compare native crossfade pixels with independently authored analytic poses."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from gltf_fixture import glb
from scene_capture import pixels
from runtime_animation_blend_contract import blend_fixture, command, component, node, transform, uid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    world = run / 'world.json'
    report = {'passed': False, 'gpu': args.gpu, 'runs': [], 'comparisons': [],
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'fixture_sha256': hashlib.sha256((Path(__file__).parent / 'runtime_animation_blend_contract.py').read_bytes()).hexdigest()}

    def native(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    def batch(requests):
        rows = [{'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params}
                for i, (method, params) in enumerate(requests)]
        proc = subprocess.run([str(args.binary.resolve()), 'world', native(world)],
            input=''.join(json.dumps(row) + '\n' for row in rows),
            capture_output=True, text=True, encoding='utf-8', timeout=240)
        replies = [json.loads(line) for line in proc.stdout.splitlines()]
        report['runs'].append({'requests': rows, 'responses': replies, 'exit_code': proc.returncode, 'stderr': proc.stderr})
        assert proc.returncode == 0 and len(replies) == len(rows), proc.stdout + proc.stderr
        for reply in replies:
            assert 'result' in reply, reply
        return [row['result'] for row in replies]

    rig, tip, camera = uid(100), node(uid(100), 1), uid(1)
    revision = 1
    try:
        doc, blob = blend_fixture()
        source = run / 'original.glb'
        source.write_bytes(glb(doc, blob))
        asset = batch([('asset.import', {'source': native(source)})])[0]['asset']
        source.unlink()
        ops = [{'op': 'asset.instantiate', 'id': rig, 'name': 'Analytic ribbon', 'asset': asset},
               {'op': 'entity.create', 'id': camera, 'name': 'Observer'},
               component(camera, 'Transform', transform((.75, 1.25, 5))),
               component(camera, 'Camera', {'vertical_fov': 50, 'near': .1, 'far': 100})]
        batch([('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': 0, 'ops': ops})])
        for samples in [1, 4]:
            session = uid(900 + samples)
            tick = 0
            requests = []
            captures = []
            inspections = []

            def capture(name):
                return {'camera': camera, 'path': native(run / f'{samples}x-{name}.bmp'),
                        'width': 640, 'height': 480, 'gpu': args.gpu, 'samples': samples, 'profile': True}

            def step(ticks, animations=()):
                nonlocal tick
                requests.append(('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                    'expected_tick': tick, 'ticks': ticks, 'animations': list(animations)}))
                tick += ticks

            def observe(name, position, weight):
                captures.append((len(requests), name))
                requests.append(('runtime.capture', {'session_id': session, 'tick': tick, **capture(name)}))
                inspections.append((len(requests), name, position, weight))
                requests.append(('runtime.entity', {'session_id': session, 'id': tip}))
                requests.append(('runtime.entity', {'session_id': session, 'id': rig}))

            requests.append(('runtime.start', {'session_id': session, 'revision': revision}))
            step(30, [command(rig)])  # Outgoing A(.5).
            step(15, [command(rig, clip=1, time=.25, speed=2, blend_ticks=60)])
            observe('advancing-quarter', (.875, 1.5, 0), .25)
            step(15)
            observe('advancing-midpoint', (.875, 2, 0), .5)
            step(15, [command(rig, time=1.5, playing=False, blend_ticks=30)])
            observe('interrupted-midpoint', (1.1875, 1.5, 0), .5)
            step(15)
            observe('completed', (1.5, 1, 0), None)
            step(15, [command(rig, clip=None, blend_ticks=30)])
            observe('rest-midpoint', (.75, 1, 0), .5)
            step(15)
            observe('rest-completed', (0, 1, 0), None)
            requests.append(('runtime.stop', {'session_id': session}))
            before = world.read_bytes()
            result = batch(requests)
            assert world.read_bytes() == before, 'Runtime crossfades changed authored world bytes.'
            for index, name in captures:
                rendered = result[index]
                assert rendered['capture_written'] and rendered['hardware'] and rendered['nvrhi_errors'] == 0, rendered
            for index, name, position, weight in inspections:
                actual = result[index]['local_transform']['position']
                assert all(abs(a - b) < 1e-8 for a, b in zip(actual, position)), (name, actual, position)
                transition = result[index + 1]['animation']['transition']
                assert (transition is None) == (weight is None), (name, transition)
                if weight is not None:
                    assert transition['weight'] == weight, (name, transition)
                if name == 'interrupted-midpoint':
                    assert transition['source_frozen'], transition

            # Independent reference: author plain local bone TRS, without any clip
            # sampling or transition code, then use the normal authored snapshot.
            reference_requests = []
            reference_indices = []
            for _, name, position, _ in inspections:
                reference_requests.append(('world.transact', {'request_id': uuid.uuid4().hex,
                    'base_revision': revision, 'ops': [component(tip, 'Transform', transform(position))]}))
                revision += 1
                reference_indices.append(len(reference_requests))
                reference_requests.append(('world.capture', {'revision': revision, **capture(name + '-authored')}))
            references = batch(reference_requests)
            for index in reference_indices:
                rendered = references[index]
                assert rendered['capture_written'] and rendered['hardware'] and rendered['nvrhi_errors'] == 0, rendered
            images = {}
            for _, name, position, _ in inspections:
                live_path, authored_path = run / f'{samples}x-{name}.bmp', run / f'{samples}x-{name}-authored.bmp'
                live, authored = pixels(live_path), pixels(authored_path)
                differences = [max(abs(a - b) for a, b in zip(p, q))
                               for row, other in zip(live, authored) for p, q in zip(row, other)]
                stats = {'samples': samples, 'case': name, 'position': list(position),
                         'changed_pixels': sum(value != 0 for value in differences),
                         'maximum_channel_difference': max(differences),
                         'live_sha256': hashlib.sha256(live_path.read_bytes()).hexdigest(),
                         'authored_sha256': hashlib.sha256(authored_path.read_bytes()).hexdigest()}
                report['comparisons'].append(stats)
                assert stats['changed_pixels'] == 0, stats
                images[name] = live
            changed = sum(p != q for row, other in zip(images['rest-completed'], images['advancing-midpoint'])
                          for p, q in zip(row, other))
            assert changed > 1000, f'Fixture animation must visibly change the ribbon: {changed} pixels.'
            report.setdefault('motion_pixels', {})[str(samples)] = changed
        report['passed'] = True
        report['capture_count'] = 24
    finally:
        evidence = run / 'evidence.json'
        evidence.write_text(json.dumps(report, indent=2) + '\n')
        print(evidence)


if __name__ == '__main__':
    main()
