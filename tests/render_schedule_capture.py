#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare native frame fixtures from reference and candidate renderers on one GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import traceback
import uuid
from scene_capture import pixels


def require(value, detail):
    if not value:
        raise RuntimeError(str(detail))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    require(not sys.flags.optimize, 'Image helper assertions must remain enabled')
    run = args.output.resolve() / uuid.uuid4().hex
    run.mkdir(parents=True)
    helper = Path(__file__).with_name('frame_execution_capture.py').resolve()
    watched = [args.reference.resolve(), args.candidate.resolve(), Path(__file__).resolve(), helper,
               helper.with_name('frame_execution_native.cpp'), helper.with_name('scene_capture.py')]
    evidence = {'passed': False, 'comparisons': [], 'runs': [],
                'input_sha256': {str(path): digest(path) for path in watched}}
    try:
        records = []
        for label, binary in [('reference', args.reference), ('candidate', args.candidate)]:
            output = run / label
            command = [sys.executable, str(helper), str(binary.resolve()), '--output', str(output), '--gpu', str(args.gpu)]
            if args.windows_interop:
                command.append('--windows-interop')
            # The nested native fixture owns its timeout. Do not strand a native
            # child by terminating only its Python wrapper on an outer timeout.
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
            evidence['runs'].append({'label': label, 'command': command, 'exit_code': result.returncode,
                                     'stdout': result.stdout, 'stderr': result.stderr})
            require(result.returncode == 0, f'{label} frame qualification failed')
            files = list(output.glob('*/evidence.json'))
            require(len(files) == 1, 'Missing or ambiguous frame evidence')
            record = json.loads(files[0].read_text())
            require(record['passed'] and record['native']['passed'], 'Nested frame checks failed')
            require(record['binary_sha256'] == evidence['input_sha256'][str(binary.resolve())], 'Fixture binary changed')
            records.append((files[0], record))
        require(records[0][1]['gpu'] == records[1][1]['gpu'], 'Reference and candidate selected different GPUs')
        evidence['gpu'] = records[0][1]['gpu']
        evidence['nested_evidence_sha256'] = [digest(path) for path, _ in records]
        def captures(entry):
            path, record = entry
            images = {}
            for mode in record['native']['modes']:
                for capture in mode['captures']:
                    key = (mode['limit'], capture['stage'])
                    require(key not in images, 'Duplicate capture identity')
                    image = path.parent / f'frame-slots{key[0]}-{key[1]}.bmp'
                    require(digest(image) == capture['sha256'], 'Recorded capture changed')
                    images[key] = image
            return images
        reference, candidate = map(captures, records)
        require(reference.keys() == candidate.keys() and len(reference) == 16, 'Capture coverage differs')
        for key, path in reference.items():
            left, right = pixels(path), pixels(candidate[key])
            require(len(left) == len(right) and all(len(a) == len(b) for a, b in zip(left, right)), 'Image extent differs')
            changed = sum(a != b for row, other in zip(left, right) for a, b in zip(row, other))
            maximum = max(abs(x-y) for row, other in zip(left, right) for a, b in zip(row, other) for x, y in zip(a, b))
            evidence['comparisons'].append({'limit': key[0], 'stage': key[1], 'changed_pixels': changed,
                                             'max_channel_difference': maximum, 'reference_sha256': digest(path),
                                             'candidate_sha256': digest(candidate[key])})
            require(changed == 0, f'Render schedule changed image: {key}')
        require(all(digest(path) == expected for path, expected in
                    ((Path(name), value) for name, value in evidence['input_sha256'].items())), 'Qualification inputs changed')
        evidence['passed'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        (run / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    require(evidence['passed'], f'Render schedule comparison failed: {run / "evidence.json"}')
    print(json.dumps({'passed': True, 'evidence': str(run / 'evidence.json')}))


if __name__ == '__main__':
    main()
