#!/usr/bin/env python3
"""Owned RPC qualification of explicit same-topology reference-frame transfer.

Original analytic FBX sources and closed-form target FK/skin expectations only.
No original provider assets, network, renderer, runtime ticks or retargeter-based
ground truth. Evidence preserves calls and source hashes in a new owned folder.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import subprocess
import time
import unittest
import uuid

from frame_transfer_fixture import expected_motion, write_fixtures

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError

ARGS = None
DEADLINE = 0
RECORD = dict(passed=False, calls=[], owners=[], source_files={}, checks=[])
POLICY = 'reference-frame-v1'
MATRIX_ERROR = 3e-5
VERTEX_ERROR = 5e-5


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def timeout(maximum=60):
    remaining = DEADLINE - time.monotonic()
    if remaining <= 0:
        raise RuntimeError('Qualification deadline exhausted')
    return min(maximum, remaining)


def native(path):
    value = str(path.resolve())
    if ARGS.windows_interop:
        return subprocess.check_output(['wslpath', '-w', value], text=True,
                                       timeout=timeout(10)).strip()
    return value


def transfer(source=None, target=None, alignment=None):
    result = dict(policy=POLICY, source_pose=source or dict(kind='rest'),
                  target_pose=target or dict(kind='rest'))
    if alignment is not None:
        result['alignment'] = alignment
    return result


class FrameTransferContract(unittest.TestCase):
    def setUp(self):
        self.directory = ARGS.output / self._testMethodName
        self.directory.mkdir()
        self.sources = write_fixtures(self.directory / 'sources')
        self.world = self.directory / 'world.json'
        self.client = None
        self.addCleanup(self.close)
        self.open()
        self.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0,
                  ops=[dict(op='entity.create', id='1' * 32, name='Retained frame authoring')]))
        self.baseline = self.imported('skin')

    def open(self):
        self.client = WorldClient.open(str(ARGS.binary), native(self.world), close_timeout=10)

    def close(self):
        owner, self.client = self.client, None
        if owner is None:
            return
        row = dict(test=self.id(), exit_code=None, cleanup_error=None)
        RECORD['owners'].append(row)
        try:
            owner.close()
        except BaseException as error:
            row['cleanup_error'] = str(error)
            raise
        finally:
            row.update(exit_code=owner.transport.returncode, stderr=owner.transport.stderr_tail,
                       stderr_truncated=owner.transport.stderr_truncated)
        self.assertEqual(row['exit_code'], 0)
        self.assertEqual(row['stderr'], '')
        self.assertFalse(row['stderr_truncated'])

    def call(self, method, params=None, error=None):
        row = dict(test=self.id(), method=method, params={} if params is None else params)
        RECORD['calls'].append(row)
        try:
            result = self.client.call(method, row['params'], timeout=timeout())
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message, data=failure.data)
            self.assertIsNotNone(error, failure.message)
            self.assertEqual(failure.code, error)
            return failure
        row['result'] = result
        self.assertIsNone(error, 'Expected rejection')
        return result

    def source(self, name):
        path = self.sources / (name + '.fbx')
        RECORD['source_files'][str(path.resolve())] = sha(path)
        return native(path)

    def imported(self, base, donor=None, policy=None, *, clip=1, name='TransferredMotion', error=None):
        params = dict(source=self.source(base))
        if donor is not None:
            item = dict(source=self.source(donor), clip=clip, name=name)
            if policy is not None:
                item['frame_transfer'] = policy
            params['animations'] = [item]
        return self.call('asset.import', params, error)

    def state(self):
        store = Path(str(self.world) + '.assets')
        return (self.call('world.inspect'), self.call('world.history'), self.world.read_bytes(),
                {p.relative_to(store).as_posix(): sha(p) for p in sorted(store.rglob('*')) if p.is_file()})

    def sample_motion(self, asset, clip_name='TransferredMotion', times=(0, .137, .375, .813, 1)):
        nodes = self.call('asset.inspect', dict(asset=asset, section='nodes'))['items']
        clips = self.call('asset.inspect', dict(asset=asset, section='animations'))['items']
        clip = next(row['index'] for row in clips if row['name'] == clip_name)
        self.assertAlmostEqual(clips[clip]['duration'], 1, delta=1e-7)
        identities = {row['name']: row['index'] for row in nodes}
        mesh = next(row for row in nodes if row['primitives'])
        observed = []
        for tick in times:
            wanted = expected_motion(tick)
            pose = self.call('asset.animation.sample', dict(asset=asset, clip=clip, time=tick,
                                                           loop=False, section='nodes'))
            values = {row['index']: row for row in pose['items']}
            for label in ('Root', 'Child'):
                matrix = values[identities[label]]['world']
                error = max(abs(a-b) for a, b in zip(matrix, wanted[label.lower()]))
                self.assertLessEqual(error, MATRIX_ERROR, f'{label} FK mismatch at {tick}: {error}')
                RECORD['checks'].append(dict(kind='independent_global_matrix', node=label,
                                              time=tick, maximum_error=error))
            vertices = self.call('asset.animation.sample', dict(asset=asset, clip=clip, time=tick,
                loop=False, section='vertices', node=mesh['index'], primitive=mesh['primitives'][0]))
            # Original triangle indices identify the source vertices. Sorting
            # by floating coordinates mispairs equal-X vertices when a rigid
            # basis change introduces a sub-ULP residual.
            self.assertEqual([row['index'] for row in vertices['items']], list(range(3)))
            actual = [row['world_position'] for row in vertices['items']]
            expected = wanted['vertices']
            self.assertEqual(len(actual), len(expected))
            error = max(abs(a-b) for actual_vertex, wanted_vertex in zip(actual, expected)
                        for a, b in zip(actual_vertex, wanted_vertex))
            self.assertLessEqual(error, VERTEX_ERROR, f'Original weighted skin mismatch at {tick}: {error}')
            RECORD['checks'].append(dict(kind='independent_original_skin', time=tick, maximum_error=error))
            observed.append(dict(time=tick, nodes=pose, vertices=vertices))
        return observed

    def diagnostics(self, result, source_pose=None, target_pose=None):
        text = '\n'.join(result['diagnostics'])
        self.assertIn(POLICY, text)
        references = re.findall(r'source_reference=(rest|clip:\d+) source_time=([^ ]+) '
                                r'target_reference=(rest|clip:\d+) target_time=([^\s]+)', text)
        self.assertEqual(len(references), 1)
        for selector, recorded_selector, recorded_time in (
            (source_pose or dict(kind='rest'), references[0][0], references[0][1]),
            (target_pose or dict(kind='rest'), references[0][2], references[0][3])):
            expected = 'rest' if selector['kind'] == 'rest' else 'clip:' + str(selector['clip'])
            self.assertEqual(recorded_selector, expected)
            self.assertEqual(float(recorded_time), selector.get('time', 0))
        identities = {}
        for key in ('source_model_sha256', 'target_model_sha256'):
            matches = re.findall(re.escape(key) + r'[^0-9a-f]+([0-9a-f]{64})(?![0-9a-f])', text)
            self.assertEqual(len(matches), 1, f'Missing/ambiguous normalized-model identity: {key}')
            identities[key] = matches[0]
        RECORD['checks'].append(dict(kind='normalized_model_fingerprints', **identities))
        return identities

    def rejected(self, base, donor, policy, *, error=-32050, clip=1):
        before = self.state()
        result = self.imported(base, donor, policy, clip=clip, error=error)
        self.assertEqual(self.state(), before, 'Rejected conversion published or changed authoring/history')
        return result

    def test_rest_gauges_motion_and_source_independent_reopen(self):
        before = self.state()
        result = self.imported('skin', 'gauge_rest', transfer())
        self.assertEqual(result['animations'], 1)
        self.assertEqual(self.state()[:3], before[:3])
        self.diagnostics(result)
        original = self.sample_motion(result['asset'])
        self.assertEqual(self.imported('skin', 'gauge_rest', transfer())['asset'], result['asset'])
        committed = self.state()
        self.close()
        retained = self.directory / 'retained-owned-sources'
        self.sources.rename(retained)
        try:
            # Only owned analytic fixtures are hidden, never caller assets.
            self.open()
            self.assertEqual(self.sample_motion(result['asset']), original)
            reopened = self.state()
            self.assertEqual((reopened[0], reopened[2], reopened[3]),
                             (committed[0], committed[2], committed[3]))
            self.assertEqual(reopened[1], dict(bytes=0, max_bytes=16777216, max_entries=32,
                redo_count=0, revision=committed[0]['revision'], session_local=True,
                skipped_large_edits=0, undo_count=0))
        finally:
            retained.rename(self.sources)

    def test_sampled_reference_is_resolved_before_take_filter(self):
        policy = transfer(dict(kind='sample', clip=0, time=.5), dict(kind='sample', clip=0, time=0))
        result = self.imported('base_with_reference', 'gauge_sample', policy,
                               clip=1, name='SelectedOriginalMotion')
        self.assertEqual(result['animations'], 3)
        identities = self.diagnostics(result, policy['source_pose'], policy['target_pose'])
        self.sample_motion(result['asset'], 'SelectedOriginalMotion')
        clips = self.call('asset.inspect', dict(asset=result['asset'], section='animations'))['items']
        self.assertEqual([row['name'] for row in clips], ['Move', 'Turn', 'SelectedOriginalMotion'])
        same = self.imported('base_with_reference', 'gauge_sample', policy,
                             clip=1, name='SelectedOriginalMotion')
        self.assertEqual(same['asset'], result['asset'])
        self.assertEqual(self.diagnostics(same, policy['source_pose'], policy['target_pose']), identities)
        self.rejected('base_with_reference', 'gauge_sample', transfer())

    def test_explicit_alignment_preserves_known_motion(self):
        alignment = dict(position=[0, 0, 0], rotation=[0, 0, math.sqrt(.5), math.sqrt(.5)])
        result = self.imported('skin', 'gauge_aligned', transfer(alignment=alignment))
        self.diagnostics(result)
        self.sample_motion(result['asset'])
        self.rejected('skin', 'gauge_aligned', transfer())

    def test_exact_default_and_invalid_reference_proportion_scales(self):
        before = self.state()
        omitted = self.imported('skin', 'gauge_rest', error=-32050)
        self.assertEqual(omitted.data['policy'], 'exact-skeleton-v1')
        self.assertEqual(self.state(), before)
        exact = self.imported('skin', 'move_donor', clip=0, name='ExactMove')
        self.assertEqual(self.imported('skin', 'move_donor', clip=0, name='ExactMove')['asset'], exact['asset'])
        self.assertFalse(any(POLICY in value for value in exact['diagnostics']))
        for base, donor, policy in [
            ('skin', 'gauge_rest', transfer(dict(kind='sample', clip=255, time=0))),
            ('skin', 'gauge_rest', transfer(dict(kind='sample', clip=0, time=1.1))),
            ('skin', 'gauge_rest', transfer(target=dict(kind='sample', clip=0, time=0))),
            ('skin', 'gauge_proportion', transfer()),
            ('skin', 'gauge_scale', transfer())]:
            with self.subTest(base=base, donor=donor, policy=policy):
                self.rejected(base, donor, policy)

    def test_strict_typed_policy_and_focused_discovery(self):
        valid = transfer()
        invalid = [False, {}, dict(valid, policy='unknown'), dict(valid, extra=1),
            dict(valid, source_pose=dict(kind='rest', time=0)),
            dict(valid, source_pose=dict(kind='sample', clip=True, time=0)),
            dict(valid, source_pose=dict(kind='sample', clip=0, time=True)),
            dict(valid, source_pose=dict(kind='sample', clip=0, time=-1)),
            dict(valid, source_pose=dict(kind='sample', clip=0, time=3601)),
            dict(valid, target_pose=dict(kind='other')),
            dict(valid, alignment=dict(position=[0, 0, 0])),
            dict(valid, alignment=dict(position=[1e9+1, 0, 0], rotation=[0, 0, 0, 1])),
            dict(valid, alignment=dict(position=[0, 0, 0], rotation=[0, 0, 0, 0])),
            dict(valid, alignment=dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], extra=1))]
        for policy in invalid:
            with self.subTest(policy=policy):
                self.rejected('skin', 'gauge_rest', policy, error=-32602)
        scoped = self.call('world.describe', dict(view='method', name='asset.import'))
        items = scoped['methods']['asset.import']['properties']['animations']['items']['anyOf']
        selection = next(item for item in items if item.get('type') == 'object')
        schema = selection['properties']['frame_transfer']
        self.assertIs(schema['additionalProperties'], False)
        self.assertEqual(schema['properties']['policy']['const'], POLICY)
        self.assertEqual(set(schema['required']), {'policy', 'source_pose', 'target_pose'})
        accepted = self.imported('skin', 'gauge_rest', valid)
        self.sample_motion(accepted['asset'], times=(.375,))


def main():
    global ARGS, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=600)
    ARGS = parser.parse_args()
    if not math.isfinite(ARGS.timeout) or not 60 <= ARGS.timeout <= 1800:
        parser.error('--timeout must be finite 60..1800 seconds')
    ARGS.binary = ARGS.binary.resolve(strict=True)
    if ARGS.output is None:
        ARGS.output = ROOT / 'build/animation-frame-transfer-contract' / uuid.uuid4().hex
    ARGS.output = ARGS.output.resolve()
    if not ARGS.binary.is_file() or ARGS.output.exists():
        parser.error('--binary must be a file and --output must be new')
    ARGS.output.mkdir(parents=True)
    DEADLINE = time.monotonic() + ARGS.timeout
    RECORD.update(binary_sha256=sha(ARGS.binary), source_sha256=sha(Path(__file__)),
                  fixture_sha256=sha(Path(__file__).with_name('frame_transfer_fixture.py')),
                  source_fixture_sha256=sha(Path(__file__).with_name('fbx_fixture.py')),
                  scope='Explicit analytic reference-frame transfer, FK/skin source oracle and atomic RPC rejection; no general retarget or locomotion qualification')
    try:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(FrameTransferContract))
        RECORD.update(passed=result.wasSuccessful(), tests=result.testsRun, failures=len(result.failures),
                      errors=len(result.errors), skipped=len(result.skipped))
        unchanged = all(Path(path).is_file() and sha(Path(path)) == digest
                        for path, digest in RECORD['source_files'].items())
        RECORD['source_hashes_unchanged'] = unchanged
        RECORD['passed'] = RECORD['passed'] and unchanged
        RECORD['rpc_count'] = len(RECORD['calls'])
        RECORD['clean_owners'] = sum(row['exit_code'] == 0 and row['cleanup_error'] is None
                                      for row in RECORD['owners'])
    finally:
        (ARGS.output / 'evidence.json').write_text(json.dumps(RECORD, indent=2,
            ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
    return 0 if RECORD['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
