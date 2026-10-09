#!/usr/bin/env python3
"""Qualify bounded exact-skeleton error.data through owned native RPC hosts.

Local FBX fixtures have independently specified TRS differences. An optional
caller-owned Kenney directory is read only; its paths, hashes and complete RPC
records belong in private ignored evidence. No renderer, network or retargeter
is started. The report diagnoses a rejected import; it does not convert motion.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import time
import unittest
import uuid

from fbx_fixture import model, prop, scene, write_ascii_fixtures

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient

ARGS = None
DEADLINE = 0
EVIDENCE = dict(passed=False, calls=[], owners=[], source_files={}, checks=[])
THRESHOLDS = dict(translation_meters=1e-5, scale_absolute=1e-6,
                  absolute_quaternion_dot_min=1 - 1e-10)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def remaining(maximum=60):
    seconds = DEADLINE - time.monotonic()
    if seconds <= 0:
        raise RuntimeError('Qualification deadline exhausted')
    return min(seconds, maximum)


def native(path):
    value = str(path.resolve())
    if ARGS.windows_interop:
        return subprocess.check_output(['wslpath', '-w', value], text=True,
                                       timeout=remaining(10)).strip()
    return value


def remember(path):
    path = path.resolve(strict=True)
    EVIDENCE['source_files'][str(path)] = sha(path)
    return path


def generated_child(directory, name, position=(0, 100, 0), rotation=(0, 0, 0),
                    scale=(1, 1, 1), child_name='Child'):
    original = model(102, 'Child', 'LimbNode', (0.0, 100.0, 0.0))
    replacement = model(102, child_name, 'LimbNode', position, rotation=rotation)
    replacement = replacement.replace(prop('Lcl Scaling', 'Lcl Scaling', (1, 1, 1)),
                                      prop('Lcl Scaling', 'Lcl Scaling', scale))
    source = scene(donor=True, takes=('Move',))
    if source.count(original) != 1:
        raise RuntimeError('Analytic fixture no longer has one known Child block')
    path = directory / (name + '.fbx')
    path.write_text(source.replace(original, replacement), encoding='utf-8', newline='\n')
    return path


class CompositionDiagnostics(unittest.TestCase):
    def setUp(self):
        self.directory = ARGS.output / self._testMethodName
        self.directory.mkdir()
        self.sources = write_ascii_fixtures(self.directory / 'sources')
        self.world = self.directory / 'world.json'
        self.client = None
        self.addCleanup(self.close)
        self.open()
        self.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0,
                  ops=[dict(op='entity.create', id='1' * 32, name='Retained authoring')]))
        # Prepopulate the immutable store so a failed import cannot hide
        # overwrites behind an empty-store assertion.
        self.good = self.imported(self.sources / 'skin.fbx')

    def open(self):
        self.client = WorldClient.open(str(ARGS.binary), native(self.world),
                                       close_timeout=10)

    def close(self):
        owner, self.client = self.client, None
        if owner is None:
            return
        row = dict(test=self.id(), exit_code=None, cleanup_error=None)
        EVIDENCE['owners'].append(row)
        try:
            owner.close()
        except BaseException as error:
            row['cleanup_error'] = str(error)
            raise
        finally:
            row['exit_code'] = owner.transport.returncode
            row['stderr_tail'] = owner.transport.stderr_tail
            row['stderr_truncated'] = owner.transport.stderr_truncated
        self.assertEqual(row['exit_code'], 0)
        self.assertEqual(row['stderr_tail'], '')
        self.assertFalse(row['stderr_truncated'])

    def tearDown(self):
        self.close()

    def call(self, method, params=None, error=None):
        params = {} if params is None else params
        row = dict(test=self.id(), method=method, params=params)
        EVIDENCE['calls'].append(row)
        try:
            result = self.client.call(method, params, timeout=remaining())
        except RpcError as failure:
            # This is the SDK-retained native data, not reconstructed from the
            # human message or a second discovery request.
            row['error'] = dict(code=failure.code, message=failure.message,
                                data=failure.data)
            self.assertIsNotNone(error, failure.message)
            self.assertEqual(failure.code, error)
            return failure
        row['result'] = result
        self.assertIsNone(error, 'Expected native rejection')
        return result

    def imported(self, source, donors=None, error=None):
        params = dict(source=native(remember(source)))
        if donors is not None:
            params['animations'] = []
            for donor in donors:
                if isinstance(donor, dict):
                    item = dict(donor)
                    item['source'] = native(remember(Path(item['source'])))
                else:
                    item = native(remember(donor))
                params['animations'].append(item)
        return self.call('asset.import', params, error)

    def state(self):
        store = Path(str(self.world) + '.assets')
        tree = {p.relative_to(store).as_posix(): sha(p)
                for p in sorted(store.rglob('*')) if p.is_file()}
        return (self.call('world.inspect'), self.call('world.history'),
                self.world.read_bytes(), tree)

    def rejected(self, donor, index=0, first=None):
        before = self.state()
        donors = ([dict(source=first, name='AcceptedBeforeFailure')] if first else [])
        donors.append(donor)
        failure = self.imported(self.sources / 'skin.fbx', donors, -32050)
        self.assertEqual(self.state(), before, 'Failed composition published or edited state')
        report = failure.data
        self.report(report, index)
        return report

    def report(self, report, index=0):
        self.assertIsInstance(report, dict)
        self.assertEqual(report['type'], 'animation_composition')
        self.assertEqual(report['version'], 1)
        self.assertEqual(report['policy'], 'exact-skeleton-v1')
        self.assertEqual(report['comparison'], 'normalized_local_rest')
        self.assertEqual(report['donor_index'], index)
        self.assertEqual(report['thresholds'], THRESHOLDS)
        self.assertEqual(report['limit'], 64)
        for name in ('version', 'donor_index', 'required_base_nodes', 'checked_source_nodes',
                     'matched_nodes', 'issue_count', 'limit'):
            self.assertIs(type(report[name]), int)
            self.assertGreaterEqual(report[name], 0)
        self.assertIs(type(report['truncated']), bool)
        self.assertGreater(report['required_base_nodes'], 0)
        self.assertLessEqual(report['matched_nodes'], report['checked_source_nodes'])
        self.assertGreater(report['issue_count'], 0)
        self.assertEqual(len(report['issues']), min(report['issue_count'], 64))
        self.assertEqual(report['truncated'], report['issue_count'] > 64)
        for issue in report['issues']:
            self.assertIn(issue['kind'], ('rest_frame', 'missing_joint_or_ancestor',
                                          'missing_animation_target'))
            for flag in ('required_skeleton', 'animated_ancestry'):
                self.assertIs(type(issue[flag]), bool)
            for side in ('base', 'donor'):
                identity, name = issue[side + '_node'], issue[side + '_name']
                if identity is None:
                    self.assertIsNone(name)
                else:
                    self.assertIs(type(identity), int)
                    self.assertGreaterEqual(identity, 0)
                    self.assertIsInstance(name, str)
                    self.assertTrue(name)
            if issue['kind'] == 'rest_frame':
                self.assertIsNotNone(issue['base_node'])
                self.assertIsNotNone(issue['donor_node'])
                metrics = issue['metrics']
                self.assertEqual(set(metrics), {'max_translation_meters', 'max_scale_absolute',
                                                'absolute_quaternion_dot', 'rotation_degrees'})
                for value in metrics.values():
                    self.assertIsNot(type(value), bool)
                    self.assertTrue(isinstance(value, (int, float)) and math.isfinite(value))
                    self.assertGreaterEqual(value, 0)
                self.assertLessEqual(metrics['rotation_degrees'], 180)
                self.assertEqual(set(issue['mismatches']), {'translation', 'rotation', 'scale'})
                for value in issue['mismatches'].values():
                    self.assertIs(type(value), bool)
                self.assertTrue(any(issue['mismatches'].values()))
            else:
                self.assertNotIn('metrics', issue)
                self.assertNotIn('mismatches', issue)

    def child(self, report):
        matches = [row for row in report['issues']
                   if row['kind'] == 'rest_frame' and row['base_name'] == 'Child']
        self.assertEqual(len(matches), 1)
        self.assertTrue(matches[0]['required_skeleton'])
        self.assertTrue(matches[0]['animated_ancestry'])
        return matches[0]

    def test_original_offset_stability_and_same_owner_recovery(self):
        donor = self.sources / 'rest_mismatch.fbx'
        report = self.rejected(donor)
        self.assertEqual(self.rejected(donor), report)
        issue = self.child(report)
        self.assertAlmostEqual(issue['metrics']['max_translation_meters'], .01, delta=1e-10)
        self.assertEqual(issue['mismatches'], dict(translation=True, rotation=False, scale=False))
        before = self.state()
        accepted = self.imported(self.sources / 'skin.fbx', [self.sources / 'move_donor.fbx'])
        self.assertEqual(accepted['animations'], 1)
        committed = self.state()
        self.assertEqual(committed[:3], before[:3])
        self.close()
        self.open()
        reopened = self.state()
        self.assertEqual(reopened[0], committed[0])
        self.assertEqual(reopened[2], committed[2])
        self.assertEqual(reopened[3], committed[3])
        # Undo/redo is explicitly session-local; persistent authored bytes,
        # inspection and the full post-import asset store must survive exactly.
        self.assertEqual(reopened[1], dict(bytes=0, max_bytes=16777216, max_entries=32,
                                          redo_count=0, revision=committed[0]['revision'],
                                          session_local=True, skipped_large_edits=0, undo_count=0))
        self.assertEqual(self.imported(self.sources / 'skin.fbx')['asset'], self.good['asset'])
        self.assertEqual(self.state(), reopened)
        EVIDENCE['checks'].append('Fresh owner: exact persisted world/inspection/store; explicitly empty session-local history')

    def test_independently_authored_trs_metrics(self):
        cases = [('translation', (3, 104, -2), (0, 0, 0), (1, 1, 1), .04, 0, 0),
                 ('rotation', (0, 100, 0), (0, 0, 90), (1, 1, 1), 0, 90, 0),
                 ('scale', (0, 100, 0), (0, 0, 0), (1, 1.2, .9), 0, 0, .2),
                 ('combined', (3, 104, -2), (0, 0, 90), (1, 1.2, .9), .04, 90, .2)]
        for name, position, rotation, scale, meters, degrees, scale_error in cases:
            with self.subTest(name=name):
                donor = generated_child(self.sources, name, position, rotation, scale)
                issue = self.child(self.rejected(donor))
                metrics = issue['metrics']
                self.assertAlmostEqual(metrics['max_translation_meters'], meters, delta=1e-10)
                self.assertAlmostEqual(metrics['max_scale_absolute'], scale_error, delta=1e-10)
                self.assertAlmostEqual(metrics['rotation_degrees'], degrees, delta=1e-6)
                self.assertAlmostEqual(metrics['absolute_quaternion_dot'],
                                       math.cos(math.radians(degrees) / 2), delta=1e-10)
                self.assertEqual(issue['mismatches'], dict(translation=meters > 0,
                                                           rotation=degrees > 0,
                                                           scale=scale_error > 0))

    def test_missing_required_joint_and_animated_target(self):
        donor = generated_child(self.sources, 'renamed_child', child_name='MissingChild')
        report = self.rejected(donor)
        missing = [row for row in report['issues'] if row['kind'] == 'missing_joint_or_ancestor']
        unknown = [row for row in report['issues'] if row['kind'] == 'missing_animation_target']
        self.assertEqual(len(missing), 1)
        self.assertEqual(len(unknown), 1)
        self.assertEqual(missing[0]['base_name'], 'Child')
        self.assertIsNone(missing[0]['donor_node'])
        self.assertTrue(missing[0]['required_skeleton'])
        self.assertFalse(missing[0]['animated_ancestry'])
        self.assertEqual(unknown[0]['donor_name'], 'MissingChild')
        self.assertIsNone(unknown[0]['base_node'])
        self.assertFalse(unknown[0]['required_skeleton'])
        self.assertTrue(unknown[0]['animated_ancestry'])

    def test_later_donor_generic_errors_and_scoped_discovery(self):
        self.child(self.rejected(self.sources / 'rest_mismatch.fbx', index=1,
                                first=self.sources / 'move_donor.fbx'))
        before = self.state()
        generic = self.imported(self.sources / 'truncated.fbx', error=-32050)
        self.assertIsNone(generic.data)
        invalid = self.call('asset.import', dict(source=native(self.sources / 'skin.fbx'),
                                                animations=[]), -32602)
        self.assertIsNone(invalid.data)
        self.assertEqual(self.state(), before)
        scope = self.call('world.describe', dict(view='method', name='asset.import'))
        method = scope['methods']['asset.import']
        schema = method['x-error-data']['animation_composition']
        self.assertEqual(schema['type'], 'object')
        self.assertIs(schema['additionalProperties'], False)
        properties = schema['properties']
        self.assertEqual(properties['policy']['const'], 'exact-skeleton-v1')
        self.assertEqual(properties['comparison']['const'], 'normalized_local_rest')
        self.assertEqual(properties['issues']['maxItems'], 64)
        self.assertEqual(properties['limit']['const'], 64)
        issue = properties['issues']['items']
        self.assertIs(issue['additionalProperties'], False)
        self.assertIn('allOf', issue)
        self.assertEqual(issue['properties']['metrics']['properties']['rotation_degrees']['maximum'], 180)
        EVIDENCE['checks'].append('Scoped asset.import strict bounded x-error-data schema observed')

    def test_unicode_node_name_is_retained_in_error_data(self):
        name = 'MissingChild_é_骨'
        donor = generated_child(self.sources, 'unicode_child', child_name=name)
        report = self.rejected(donor)
        unknown = [row for row in report['issues'] if row['kind'] == 'missing_animation_target']
        self.assertEqual(len(unknown), 1)
        self.assertEqual(unknown[0]['donor_name'], name)
        self.assertEqual(self.rejected(donor), report)
        # ufbx's configured Unicode policy aborts malformed source names before
        # composition. Do not expect repaired structured rows for an asset
        # that the importer never admitted.
        invalid = self.sources / 'invalid_utf8_child.fbx'
        invalid.write_bytes(donor.read_bytes().replace(name.encode('utf-8'), b'MissingChild_\xff'))
        before = self.state()
        failure = self.imported(self.sources / 'skin.fbx', [invalid], -32050)
        self.assertIsNone(failure.data)
        self.assertEqual(self.state(), before)

    def page(self, asset, section):
        rows, offset = [], 0
        while True:
            page = self.call('asset.inspect', dict(asset=asset, section=section,
                                                  offset=offset, limit=64))
            rows.extend(page['items'])
            if len(page['items']) < 64:
                return rows
            offset += len(page['items'])
            self.assertLessEqual(offset, 10048)

    def test_optional_original_kenney_rejection(self):
        if ARGS.source_directory is None:
            self.skipTest('No caller-owned real source directory supplied')
        files = [p for p in ARGS.source_directory.rglob('*')
                 if p.is_file() and p.suffix.casefold() == '.fbx']
        self.assertLessEqual(len(files), 128, 'Supply a bounded character pack directory')
        def selected(stem):
            candidates = [p for p in files if p.stem.casefold() == stem.casefold()]
            self.assertEqual(len(candidates), 1, 'Expected one original ' + stem + '.fbx')
            return candidates[0]
        base = selected('characterMedium')
        base_asset = self.imported(base)['asset']
        base_nodes = {row['index']: row for row in self.page(base_asset, 'nodes')}
        for stem in ('idle', 'run'):
            with self.subTest(source=stem):
                donor = selected(stem)
                before = self.state()
                # Original animation-only FBX is supported as a donor, not as
                # a standalone published model. Keep all takes here: exact
                # rest matching fails before duplicate-name clip admission.
                failure = self.imported(base, [donor], -32050)
                self.report(failure.data)
                self.assertEqual(self.state(), before)
                self.assertEqual(self.imported(base, [donor], -32050).data, failure.data)
                self.assertEqual(self.state(), before)
                report = failure.data
                self.assertGreater(report['checked_source_nodes'], 0)
                self.assertLessEqual(report['required_base_nodes'], len(base_nodes))
                angles = []
                kinds = {}
                for issue in failure.data['issues']:
                    kinds[issue['kind']] = kinds.get(issue['kind'], 0) + 1
                    if issue['base_node'] is not None:
                        self.assertIn(issue['base_node'], base_nodes)
                        self.assertEqual(issue['base_name'], base_nodes[issue['base_node']]['name'])
                    if issue['kind'] != 'rest_frame':
                        continue
                    metrics, mismatch = issue['metrics'], issue['mismatches']
                    self.assertEqual(mismatch['translation'],
                                     metrics['max_translation_meters'] > THRESHOLDS['translation_meters'])
                    self.assertEqual(mismatch['scale'],
                                     metrics['max_scale_absolute'] > THRESHOLDS['scale_absolute'])
                    self.assertEqual(mismatch['rotation'],
                                     metrics['absolute_quaternion_dot'] < THRESHOLDS['absolute_quaternion_dot_min'])
                    angles.append(metrics['rotation_degrees'])
                self.assertTrue(angles, 'Known original pair must report an incompatible rest frame')
                if not report['truncated']:
                    self.assertEqual(report['checked_source_nodes'], report['matched_nodes'] +
                                     kinds.get('rest_frame', 0) + kinds.get('missing_animation_target', 0))
                EVIDENCE['checks'].append(dict(
                    kind='original_source_report_observation', donor=stem,
                    source_sha256=sha(donor), issue_count=report['issue_count'],
                    recorded_issues=len(report['issues']), recorded_issue_kinds=kinds,
                    maximum_recorded_rotation_degrees=max(angles), truncated=report['truncated'],
                    interpretation='Native diagnostic observation, not an independent original-donor numeric oracle; no retarget performed'))
        EVIDENCE['checks'].append('Original caller sources: diagnosed exact-frame rejection, no retarget performed')


def main():
    global ARGS, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--source-directory', type=Path)
    parser.add_argument('--timeout', type=float, default=600)
    ARGS = parser.parse_args()
    if not math.isfinite(ARGS.timeout) or not 60 <= ARGS.timeout <= 1800:
        parser.error('--timeout must be finite and 60..1800 seconds')
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.output = ARGS.output.resolve()
    if not ARGS.binary.is_file() or ARGS.output.exists():
        parser.error('--binary must be a file and --output must be new')
    if ARGS.source_directory is not None:
        ARGS.source_directory = ARGS.source_directory.resolve(strict=True)
        if not ARGS.source_directory.is_dir() or ARGS.output.is_relative_to(ARGS.source_directory):
            parser.error('Source directory must exist; output cannot be inside caller sources')
    ARGS.output.mkdir(parents=True)
    DEADLINE = time.monotonic() + ARGS.timeout
    EVIDENCE.update(binary_sha256=sha(ARGS.binary), source_sha256=sha(Path(__file__)),
                    fixture_sha256=sha(Path(__file__).with_name('fbx_fixture.py')),
                    client_errors_sha256=sha(ROOT / 'tools/python/poima_client/errors.py'),
                    scope='Owned RPC diagnostics and atomic rejected import; no rendering or retargeting')
    try:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(CompositionDiagnostics))
        EVIDENCE.update(passed=result.wasSuccessful(), tests=result.testsRun,
                        failures=len(result.failures), errors=len(result.errors), skipped=len(result.skipped))
        unchanged = all(Path(path).is_file() and sha(Path(path)) == digest
                        for path, digest in EVIDENCE['source_files'].items())
        EVIDENCE['source_hashes_unchanged'] = unchanged
        EVIDENCE['passed'] = EVIDENCE['passed'] and unchanged
        EVIDENCE['rpc_count'] = len(EVIDENCE['calls'])
        EVIDENCE['clean_owners'] = sum(row['exit_code'] == 0 and row['cleanup_error'] is None
                                      for row in EVIDENCE['owners'])
    finally:
        (ARGS.output / 'evidence.json').write_text(json.dumps(EVIDENCE, indent=2,
            ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
    return 0 if EVIDENCE['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
