#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original-model admission guards through the real native asset importer.

Original analytic glTF and FBX sources only. No renderer, managed compiler or
download. Requests, complete responses, original source versions and every
authoring/store snapshot remain in a fresh output. An optional retained native
baseline independently checks legacy responses, fingerprints and package bytes.
Windows execution requires a caller-owned process-tree Job.
"""
import argparse
import copy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

from rotation_retarget_fixture import write_fixtures
from gltf_fixture import glb, sphere
from switchback_game_service import Budget, RetainedTransport

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient

ARGS = None
DEADLINE = 0
WORK_DEADLINE = 0
MAX_CALLS = 1600
MAX_BYTES = 128 * 1024**2
MAX_FILES = 4096
RECORD = dict(format='poima.asset-import-guards.v1', passed=False, calls=[],
              owners=[], checks=[], source_files={}, source_versions=[])


def need(value, message):
    if not value:
        raise AssertionError(message)


def same(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def pin(path):
    info = path.lstat()
    need(stat.S_ISREG(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
         'Expected an ordinary unlinked input file')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(bytes=info.st_size, sha256=digest)


def native(path):
    value = str(Path(path).resolve())
    if ARGS.windows_interop and os.name != 'nt':
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip()
    return value


def default_output(windows_interop=False):
    """Retain native POSIX observations on its OS-local temporary filesystem."""
    parent = (Path(tempfile.gettempdir()) / 'poima-asset-import-guards'
              if os.name == 'posix' and not windows_interop
              else ROOT / 'build/asset-import-guards')
    return parent / uuid.uuid4().hex


def work_remaining():
    remaining = WORK_DEADLINE - time.monotonic()
    need(remaining > 0, 'Whole fixture work deadline exhausted; cleanup budget reserved')
    return remaining


def census(reserve=0, reserve_files=0):
    count = total = 0
    for path in ARGS.output.rglob('*'):
        try:
            info = path.lstat()
        except FileNotFoundError:
            # Compiler/engine staging retirement is not an ordinary-file claim.
            # Stable before/after publication inventories below remain exact.
            continue
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
             'Linked fixture output')
        if stat.S_ISDIR(info.st_mode):
            continue
        need(stat.S_ISREG(info.st_mode), 'Nonordinary fixture output')
        count += 1
        total += info.st_size
    need(count + reserve_files <= MAX_FILES and total + reserve <= MAX_BYTES,
         'Retained fixture output exceeds its declared bound')
    return dict(files=count, bytes=total)


def windows_job():
    if os.name != 'nt':
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    flag = ctypes.c_int()
    need(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(flag)) and flag.value,
         'Run native Windows tests inside a caller-owned Windows Job')


class ImportGuards(unittest.TestCase):
    def setUp(self):
        if RECORD.get('halted'):
            self.skipTest('Earlier owner has an unknown outcome or failed natural closure; no new producer')
        work_remaining()
        self.directory = ARGS.output / self._testMethodName
        self.directory.mkdir()
        self.sources = write_fixtures(self.directory / 'sources')
        self.document, self.blob = sphere(4, 8)
        (self.sources / 'original.glb').write_bytes(glb(self.document, self.blob))
        self.originals = {path.name: path.read_bytes() for path in self.sources.iterdir() if path.is_file()}
        for name in self.originals:
            RECORD['source_files'][str((self.sources / name).resolve())] = pin(self.sources / name)
        self.client = self.transport = None
        self.serial = self.snapshots = 0
        self.world = self.directory / 'world.json'
        self.addCleanup(self.close)
        self.addCleanup(self.restore_originals)
        self.baseline_results = []
        if self._testMethodName == 'test_matching_and_legacy_package_identity' and ARGS.baseline:
            self.open(ARGS.baseline, self.directory / 'baseline-world.json', 'legacy')
            self.old_summaries = {name: self.summary(name) for name in self.used_sources()}
            for request in self.accepted_requests(self.old_summaries):
                result = self.call('asset.import', request)
                self.baseline_results.append(dict(request=copy.deepcopy(request), result=result,
                                                  package=self.package(result['asset'])))
            self.close()
        self.open(ARGS.binary, self.world, 'current')
        for revision, identity in enumerate(('1' * 32, '2' * 32)):
            self.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=revision,
                ops=[dict(op='entity.create', id=identity, name='Retained import guard entity')]))

    @staticmethod
    def used_sources():
        return ('original.glb', 'skin.fbx', 'animated.fbx', 'move_donor.fbx',
                'turn_donor.fbx', 'gauge_rest.fbx', 'gauge_proportion.fbx')

    def restore_originals(self):
        for name, data in self.originals.items():
            need((self.sources / name).read_bytes() == data, 'Original fixture input was not restored: ' + name)

    def open(self, binary, world, label):
        work_remaining()
        need(self.client is None, 'Only one actual native owner may be live')
        folder = self.directory / ('owner-' + str(self.serial) + '-' + label)
        self.serial += 1
        folder.mkdir()
        self.raw = []
        self.active_world = world
        self.owner_record = dict(test=self.id(), label=label, directory=str(folder),
                                 argv=[str(binary), 'world', native(world)], exit_code=None,
                                 cleanup_error=None, raw_response_count=0)
        RECORD['owners'].append(self.owner_record)
        try:
            self.transport = RetainedTransport(self.owner_record['argv'], folder,
                                               Budget(DEADLINE, []), self.raw.append)
            self.owner_record['process_id'] = self.transport.process.pid
            self.client = WorldClient(self.transport)
        except BaseException as error:
            RECORD['halted'] = True
            self.owner_record['startup_error'] = repr(error)
            raise

    def close(self):
        client, self.client = self.client, None
        if client is None:
            return
        try:
            client.close()
            need(client.cleanup_error is None and self.transport.returncode == 0,
                 'Native owner did not close naturally with code zero')
            need(not self.transport.stderr_truncated, 'Native stderr was truncated')
        except BaseException as error:
            RECORD['halted'] = True
            self.owner_record['cleanup_error'] = repr(error)
            raise
        finally:
            self.owner_record.update(exit_code=self.transport.returncode,
                raw_response_count=len(self.raw), stderr=self.transport.stderr_tail,
                stderr_truncated=self.transport.stderr_truncated,
                transport_broken=self.transport.broken)

    def call(self, method, params=None, error=None):
        need(len(RECORD['calls']) < MAX_CALLS, 'Native request bound exhausted')
        work_remaining()
        census()
        remaining = work_remaining()
        row = dict(test=self.id(), owner=self.owner_record['directory'], method=method,
                   params=copy.deepcopy(params or {}))
        RECORD['calls'].append(row)
        try:
            result = self.client.call(method, row['params'], timeout=min(12, remaining))
        except RpcError as failure:
            row['rpc_id'] = self.raw[-1]['id']
            row['error'] = copy.deepcopy(self.raw[-1]['error'])
            need(same(failure.data, row['error'].get('data')), 'Client lost native error.data')
            need(error is not None and type(failure.code) is int and failure.code == error,
                 'Unexpected native rejection: ' + repr(row['error']))
            return row['error']
        except BaseException as failure:
            RECORD['halted'] = True
            row['unknown_outcome'] = repr(failure)
            raise
        row.update(rpc_id=self.raw[-1]['id'], result=copy.deepcopy(result))
        need(error is None, 'Expected native rejection, got success')
        return result

    def source(self, name):
        return native(self.sources / name)

    def summary(self, name):
        result = self.call('asset.source.inspect', dict(source=self.source(name)))
        self.assertRegex(result['model_sha256'], r'^[0-9a-f]{64}$')
        self.assertIs(result['published'], False)
        return result

    def package(self, asset):
        path = Path(str(self.active_world) + '.assets') / (asset + '.pmodel')
        data = path.read_bytes()
        self.assertLessEqual(len(data), 64 * 1024**2)
        self.assertEqual(hashlib.sha256(data).hexdigest(), asset)
        return data

    def state(self):
        # Read native state first to establish completed authoring I/O. The
        # operational exclusive lock is metadata-only while its owner is live.
        native_state = dict(inspect=self.call('world.inspect'), history=self.call('world.history'),
            query=self.call('entity.query'), entities=[self.call('entity.get', dict(id=identity))
                                                      for identity in ('1' * 32, '2' * 32)])
        folder = self.directory / ('snapshot-' + str(self.snapshots))
        self.snapshots += 1
        folder.mkdir()
        files = {}
        for suffix in ('', '.previous', '.pending', '.previous.pending'):
            path = Path(str(self.world) + suffix)
            try:
                info = path.lstat()
            except FileNotFoundError:
                files['world.json' + suffix] = None
                continue
            need(stat.S_ISREG(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'Nonordinary authoring payload')
            data = path.read_bytes()
            self.assertLessEqual(len(data), 16 * 1024**2)
            (folder / ('world.json' + suffix)).write_bytes(data)
            files['world.json' + suffix] = data
        lock = Path(str(self.world) + '.lock').lstat()
        need(stat.S_ISREG(lock.st_mode) and lock.st_size == 0 and lock.st_nlink == 1,
             'Owned operational writer-lock shape differs')
        store = Path(str(self.world) + '.assets')
        packages = {}
        try:
            info = store.lstat()
        except FileNotFoundError:
            info = None
        if info is not None:
            need(stat.S_ISDIR(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'Asset store is not an ordinary directory')
            for path in sorted(store.iterdir()):
                need(stat.S_ISREG(path.lstat().st_mode) and not path.is_symlink(), 'Nonordinary asset package')
                data = path.read_bytes()
                self.assertLessEqual(len(data), 64 * 1024**2)
                packages[path.name] = data
                (folder / path.name).write_bytes(data)
        result = dict(native=native_state, files=files, store_exists=info is not None, packages=packages,
                      lock=dict(exists=True, bytes=lock.st_size, contents_read=False))
        summary = dict(native=native_state, files={key: None if value is None else
                       dict(bytes=len(value), sha256=hashlib.sha256(value).hexdigest()) for key, value in files.items()},
                       store_exists=result['store_exists'], packages={key: dict(bytes=len(value),
                       sha256=hashlib.sha256(value).hexdigest()) for key, value in packages.items()}, lock=result['lock'])
        (folder / 'state.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        census()
        return result

    def rejected(self, params, code=-32602, data=None):
        before = self.state()
        result = self.call('asset.import', params, error=code)
        self.assertTrue(same(self.state(), before), 'Rejected import changed complete authoring/store state')
        if data is not None:
            self.assertTrue(same(result.get('data'), data), 'Complete native mismatch diagnostic differs')
        RECORD['checks'].append(dict(kind='rejection_atomicity', test=self.id(), params=copy.deepcopy(params),
                                      error=copy.deepcopy(result), snapshots=[self.snapshots - 2, self.snapshots - 1]))
        return result

    def mismatch(self, params, expected, actual, donor=None):
        return self.rejected(params, -32050, dict(type='model_source_changed', version=1,
            source_role='base' if donor is None else 'animation_donor', donor_index=donor,
            expected_model_sha256=expected, actual_model_sha256=actual))

    def rotation(self, source, base):
        return dict(policy='reference-rotation-v1', source_pose=dict(kind='rest'),
                    target_pose=dict(kind='rest'), positions=dict(kind='target_reference'),
                    scales='target_reference', expected_source_model_sha256=source,
                    expected_target_model_sha256=base)

    def accepted_requests(self, summaries):
        base = summaries['skin.fbx']['model_sha256']
        return [dict(source=self.source('original.glb')), dict(source=self.source('skin.fbx')),
            dict(source=self.source('skin.fbx'), animations=[self.source('move_donor.fbx')]),
            dict(source=self.source('skin.fbx'), animations=[dict(source=self.source('animated.fbx'), clip=1, name='Selected')]),
            dict(source=self.source('skin.fbx'), animations=[dict(source=self.source('gauge_rest.fbx'), clip=1,
                 name='Frame', frame_transfer=dict(policy='reference-frame-v1', source_pose=dict(kind='rest'), target_pose=dict(kind='rest')))]),
            dict(source=self.source('skin.fbx'), animations=[dict(source=self.source('gauge_proportion.fbx'), clip=1,
                 name='Rotation', retarget=self.rotation(summaries['gauge_proportion.fbx']['model_sha256'], base))]),
            dict(source=self.source('skin.fbx'), animations=[dict(source=self.source('move_donor.fbx'), clip=0, name='MoveOne'),
                                                           dict(source=self.source('turn_donor.fbx'), clip=0, name='TurnTwo')])]

    def guarded(self, request, summaries):
        result = copy.deepcopy(request)
        by_path = {self.source(name): summary['model_sha256'] for name, summary in summaries.items()}
        result['expected_model_sha256'] = by_path[result['source']]
        if 'animations' in result:
            result['animations'] = [dict(source=item) if isinstance(item, str) else item for item in result['animations']]
            for item in result['animations']:
                item['expected_model_sha256'] = by_path[item['source']]
        return result

    def test_matching_and_legacy_package_identity(self):
        before = self.state()
        summaries = {name: self.summary(name) for name in self.used_sources()}
        self.assertTrue(same(self.state(), before), 'Read-only source inspection published or mutated state')
        if ARGS.baseline:
            self.assertTrue(same(summaries, self.old_summaries), 'Current original source inspection differs from old native baseline')
        for index, request in enumerate(self.accepted_requests(summaries)):
            original = self.call('asset.import', request)
            data = self.package(original['asset'])
            committed = self.state()
            guarded = self.guarded(request, summaries)
            actual = self.call('asset.import', guarded)
            self.assertTrue(same(actual, original), 'Guard changed complete legacy success response')
            self.assertEqual(self.package(actual['asset']), data, 'Guard changed any package byte')
            self.assertTrue(same(self.state(), committed), 'Idempotent guarded import changed state/store')
            if ARGS.baseline:
                old = self.baseline_results[index]
                self.assertTrue(same(old['request'], request) and same(old['result'], original), 'Retained native legacy response changed')
                self.assertEqual(old['package'], data, 'Retained native package bytes changed')
            RECORD['checks'].append(dict(kind='whole_legacy_and_guarded_response_package', index=index,
                legacy_baseline=ARGS.baseline is not None, asset=actual['asset'], request=request, guarded=guarded,
                package=dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())))

    def test_stale_original_sources_and_recovery(self):
        summaries = {name: self.summary(name) for name in self.used_sources()}
        original = summaries['original.glb']['model_sha256']
        changed = copy.deepcopy(self.document)
        changed['nodes'][1]['name'] = 'Changed original parsed node'
        path = self.sources / 'original.glb'
        changed_bytes = glb(changed, self.blob)
        retained = self.directory / 'changed-original.glb'
        retained.write_bytes(changed_bytes)
        RECORD['source_versions'].append(dict(path=str(retained), identity=pin(retained), reason='Deliberate valid parsed-model change'))
        path.write_bytes(changed_bytes)
        try:
            actual = self.summary('original.glb')['model_sha256']
            self.assertNotEqual(actual, original)
            request = dict(source=self.source('original.glb'), expected_model_sha256=original)
            self.mismatch(request, original, actual)
            repaired_request = dict(request, expected_model_sha256=actual)
            imported = self.call('asset.import', repaired_request)
            self.assertEqual(imported['asset'], actual)
            self.assertTrue(same(imported, self.call('asset.import', dict(source=self.source('original.glb')))))
        finally:
            path.write_bytes(self.originals['original.glb'])
        self.assertEqual(self.summary('original.glb')['model_sha256'], original)
        self.assertEqual(self.call('asset.import', request)['asset'], original, 'Exact original rejected request did not recover after source repair')
        # Valid selected files still reach the stale base guard before any
        # composition/publication. Existing aggregate file-size preflight is
        # allowed; this is not a promise of zero source metadata I/O.
        self.mismatch(dict(source=self.source('skin.fbx'), expected_model_sha256=original,
                           animations=[self.source('move_donor.fbx')]), original, summaries['skin.fbx']['model_sha256'])
        requests = self.accepted_requests(summaries)
        for index in (3, 4, 5, 6):
            legacy = requests[index]
            result = self.call('asset.import', legacy)
            guarded = self.guarded(legacy, summaries)
            donor_index = 1 if index == 6 else 0
            donor = guarded['animations'][donor_index]
            original_hash = donor['expected_model_sha256']
            self.assertNotEqual(result['asset'], original_hash, 'Fixture must distinguish original from composed/selected identity')
            donor['expected_model_sha256'] = result['asset']
            self.mismatch(guarded, result['asset'], original_hash, donor_index)
            donor['expected_model_sha256'] = original_hash
            self.assertTrue(same(self.call('asset.import', guarded), result))
        # New admission guards do not supersede the existing mandatory inner
        # rotation-policy guards. Their original errors remain byte-for-byte
        # unchanged when the new outer guard matches.
        for field in ('expected_source_model_sha256', 'expected_target_model_sha256'):
            legacy = copy.deepcopy(requests[5])
            legacy['animations'][0]['retarget'][field] = '0' * 64
            original_error = self.rejected(legacy, -32050)
            guarded = self.guarded(legacy, summaries)
            self.assertTrue(same(self.rejected(guarded, -32050), original_error),
                            'Outer admission guard replaced an independent inner-policy rejection')
        # A real original donor mutation, followed by deliberate reinspection,
        # then restoration with the exact originally rejected request.
        donor_path = self.sources / 'move_donor.fbx'
        data = self.originals['move_donor.fbx']
        self.assertEqual(data.count(b'AnimStack::Move'), 1)
        changed_bytes = data.replace(b'AnimStack::Move', b'AnimStack::ChangedMove')
        retained = self.directory / 'changed-donor.fbx'
        retained.write_bytes(changed_bytes)
        RECORD['source_versions'].append(dict(path=str(retained), identity=pin(retained), reason='Deliberate original donor take rename'))
        request = self.guarded(dict(source=self.source('skin.fbx'), animations=[dict(source=self.source('move_donor.fbx'), clip=0, name='Recovered')]), summaries)
        donor_path.write_bytes(changed_bytes)
        try:
            actual = self.summary('move_donor.fbx')['model_sha256']
            expected = summaries['move_donor.fbx']['model_sha256']
            self.assertNotEqual(actual, expected)
            self.mismatch(request, expected, actual, 0)
            revised = copy.deepcopy(request)
            revised['animations'][0]['expected_model_sha256'] = actual
            self.call('asset.import', revised)
        finally:
            donor_path.write_bytes(data)
        self.assertEqual(self.summary('move_donor.fbx')['model_sha256'], expected)
        self.call('asset.import', request)

    def test_invalid_guards_reject_before_source_io(self):
        # All source paths are deliberately absent: malformed native fields
        # must reject -32602 before importer I/O, not an import error -32050.
        absent = self.source('absent.glb')
        values = [None, False, True, 0, 1.5, [], {}, '', 'a' * 63, 'a' * 65, 'A' * 64, 'g' * 64, '\u00e9' * 64]
        for value in values:
            self.rejected(dict(source=absent, expected_model_sha256=value))
            self.rejected(dict(source=absent, animations=[dict(source=absent, expected_model_sha256=value)]))
        for params in (dict(source=absent, expected_source_model_sha256='a' * 64),
                       dict(source=absent, animations=[dict(source=absent, expected_source_model_sha256='a' * 64)]),
                       dict(source=absent, expected_model_sha256='a' * 64, unexpected=True),
                       dict(source=absent, animations=[dict(source=absent, expected_model_sha256='a' * 64, unexpected=True)])):
            self.rejected(params)


def main():
    global ARGS, DEADLINE, WORK_DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--baseline', '--legacy-binary', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--record', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=180)
    ARGS = parser.parse_args()
    if not math.isfinite(ARGS.timeout) or not 60 <= ARGS.timeout <= 180:
        parser.error('Whole fixture timeout must be finite within60..180seconds')
    ARGS.binary = ARGS.binary.resolve(strict=True)
    if ARGS.baseline:
        ARGS.baseline = ARGS.baseline.resolve(strict=True)
    ARGS.output = (ARGS.output if ARGS.output is not None else default_output(ARGS.windows_interop)).resolve()
    if ARGS.output.exists() or (ARGS.record is not None and ARGS.record.exists()):
        parser.error('Use fresh output and optional record paths')
    windows_job()
    need(not sys.flags.optimize, 'Run without Python optimization')
    ARGS.output.mkdir(parents=True)
    DEADLINE = time.monotonic() + ARGS.timeout
    WORK_DEADLINE = DEADLINE - 10
    helpers = [Path(__file__).with_name(name) for name in ('rotation_retarget_fixture.py',
        'frame_transfer_fixture.py', 'fbx_fixture.py', 'texture_fixture.py', 'gltf_fixture.py',
        'switchback_game_service.py', 'switchback_oracle.py')]
    helpers += [p for p in sorted((ROOT / 'tools/python/poima_client').rglob('*')) if p.is_file() and p.suffix in ('.py', '.json')]
    inputs = {str(path.resolve()): pin(path) for path in [Path(__file__), ARGS.binary, *helpers]}
    if ARGS.baseline:
        inputs[str(ARGS.baseline)] = pin(ARGS.baseline)
    RECORD.update(inputs=inputs, deadline_seconds=ARGS.timeout, bounds=dict(rpc=MAX_CALLS, bytes=MAX_BYTES, files=MAX_FILES),
        scope='Native original-model fingerprint admission; exact retained legacy responses/package bytes when a baseline is supplied; full authored/store rejection atomicity; no graphics or general exporter qualification')
    try:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ImportGuards))
        RECORD.update(passed=result.wasSuccessful(), tests=result.testsRun, failures=len(result.failures),
                      errors=len(result.errors), skipped=len(result.skipped), rpc_count=len(RECORD['calls']))
        RECORD['failure_details'] = [dict(test=test.id(), traceback=trace) for test, trace in result.failures]
        RECORD['error_details'] = [dict(test=test.id(), traceback=trace) for test, trace in result.errors]
        RECORD['inputs_unchanged'] = all(pin(Path(path)) == identity for path, identity in inputs.items())
        RECORD['source_inputs_unchanged'] = all(pin(Path(path)) == identity for path, identity in RECORD['source_files'].items())
        RECORD['passed'] &= RECORD['inputs_unchanged'] and RECORD['source_inputs_unchanged'] and all(
            owner['exit_code'] == 0 and owner['cleanup_error'] is None and not owner.get('transport_broken', True)
            for owner in RECORD['owners'])
    finally:
        payload = (json.dumps(RECORD, indent=2, allow_nan=False, ensure_ascii=False) + '\n').encode('utf-8')
        need(len(payload) <= 8 * 1024**2, 'Summary exceeds8MiB; raw wires remain retained')
        extra_record = ARGS.record is not None and ARGS.record.resolve() != (ARGS.output / 'evidence.json').resolve()
        record_inside_output = extra_record and ARGS.record.resolve().is_relative_to(ARGS.output)
        census(len(payload) * (2 if record_inside_output else 1), 2 if record_inside_output else 1)
        (ARGS.output / 'evidence.json').write_bytes(payload)
        if ARGS.record is not None and ARGS.record.resolve() != (ARGS.output / 'evidence.json').resolve():
            ARGS.record.parent.mkdir(parents=True, exist_ok=True)
            with ARGS.record.open('xb') as stream:
                stream.write(payload)
    print(json.dumps(dict(passed=RECORD['passed'], output=str(ARGS.output),
                          evidence=str(ARGS.output / 'evidence.json'))), flush=True)
    return 0 if RECORD['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
