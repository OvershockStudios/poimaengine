#!/usr/bin/env python3
"""Native scalar-map RPC publication/recovery; no renderer or external content."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
import unittest
import uuid
import fbx_scalar_fixture as fixture
import player_service_contract as shared

ROOT = Path(__file__).resolve().parents[1]
ARGS = None
RECORD = dict(owners=[], calls=[], checks=[], source_pins={}, state_guards=[], package_guards=[])
DEADLINE = None


def exact(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def identity(path):
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def native(path):
    text = str(Path(path).resolve())
    return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip() if ARGS.windows_interop else text


class Owner(shared.ProcessOwner):
    def __init__(self, world, label):
        super().__init__([str(ARGS.binary.resolve()), 'world', native(world)], label, native(world), 'world')
        self.next_id = 0
        self.record['forced_cleanup'] = False

    def rpc(self, method, params=None, error=None):
        remaining = DEADLINE-time.monotonic()
        if remaining <= 0 or len(RECORD['calls']) >= 600:
            raise AssertionError('RPC deadline/count bound exceeded')
        self.next_id += 1
        request = dict(jsonrpc='2.0', id=self.next_id, method=method, params={} if params is None else params)
        line = json.dumps(request, allow_nan=False)+'\n'
        row = dict(owner=self.record['label'], request=request, request_line=line)
        RECORD['calls'].append(row)
        self.process.stdin.write(line)
        self.process.stdin.flush()
        response_line = self.lines.get(timeout=min(90, remaining))
        row['response_line'] = response_line if isinstance(response_line, str) else repr(response_line)
        if not isinstance(response_line, str): raise AssertionError('Missing complete response')
        response = json.loads(response_line)
        row['response'] = response
        if response.get('jsonrpc') != '2.0' or type(response.get('id')) is not int or response['id'] != self.next_id:
            raise AssertionError('Response envelope identity mismatch')
        if set(response) not in ({'jsonrpc', 'id', 'result'}, {'jsonrpc', 'id', 'error'}):
            raise AssertionError('Response envelope shape mismatch')
        if sum(len(c.get('response_line', ''))+len(c['request_line']) for c in RECORD['calls']) > 32*1024*1024:
            raise AssertionError('Raw RPC retention exceeds32MiB')
        if error is None:
            if 'result' not in response: raise AssertionError(response)
            return response['result']
        if 'error' not in response or type(response['error'].get('code')) is not int or response['error']['code'] != error:
            raise AssertionError('Expected exact rejection code: '+repr(response))
        return response['error']

    def close(self, expected=True):
        # Reuse the established cleanup helper, retaining whether it was needed.
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                try: self.process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    self.record['forced_cleanup'] = True
                    shared.cleanup_process(self.process, self.marker, self.role)
        finally:
            if self.process.poll() is None:
                self.record['forced_cleanup'] = True
                shared.cleanup_process(self.process, self.marker, self.role)
            self.record['exit_code'] = self.process.returncode
            for thread in self.threads: thread.join(timeout=3)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                if not stream.closed: stream.close()
        if expected and self.record['exit_code'] != 0: raise AssertionError('Nonzero native owner exit')
        if expected and self.record['forced_cleanup']: raise AssertionError('Owner required forced cleanup')
        if expected and (self.record['stderr'] or self.record['stderr_truncated']):
            raise AssertionError('Unexpected native stderr')


def packed_package(path, have_rough=True, have_metal=True):
    """Read cooked binary layout directly; no native codec as oracle."""
    data = path.read_bytes()
    if data[:8] not in (b'POIMAM03', b'POIMAM04') or len(data) > 64*1024*1024:
        raise AssertionError('Unexpected bounded model package')
    header_bytes, binary_bytes = struct.unpack_from('<II', data, 8)
    if len(data) != 16+header_bytes+binary_bytes: raise AssertionError('Model length mismatch')
    meta = json.loads(data[16:16+header_bytes])
    offset = 16+header_bytes+meta['geometry_bytes']+meta.get('animation_bytes', 0)
    images = []
    for image in meta['images']:
        levels = []
        for level in image['mips']:
            length = level['bytes']
            levels.append(data[offset:offset+length]); offset += length
        images.append(levels)
    if offset != len(data): raise AssertionError('Image payload closure mismatch')
    primitive = meta['primitives'][0]
    map_ = primitive['textures'][1]
    image = meta['images'][map_['image']]
    if image['srgb'] is not False or image['mips'] != [dict(width=2, height=2, bytes=16), dict(width=1, height=1, bytes=4)]:
        raise AssertionError('Packed image metadata differs')
    wanted = bytes(c for r, m in zip(fixture.ROUGH, fixture.METAL)
                   for c in (255, r if have_rough else 255, m if have_metal else 255, 255))
    if images[map_['image']] != [wanted, bytes((255, 112 if have_rough else 255, 112 if have_metal else 255, 255))]:
        raise AssertionError('Independent packed channels/mip differ')
    return meta


class ScalarContract(unittest.TestCase):
    def setUp(self):
        self.directory = ARGS.output/self._testMethodName
        self.directory.mkdir()
        self.sources = fixture.write_fixtures(self.directory/'sources')
        self.world = self.directory/'world.json'
        self.owners = []
        self.open('initial')
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0,
            ops=[dict(op='entity.create', id='1'*32, name='Existing authored sentinel')]))

    def open(self, label):
        self.owner = Owner(self.world, self._testMethodName+'/'+label)
        self.owners.append(self.owner)

    def close(self):
        self.owners.pop().close()

    def tearDown(self):
        errors = []
        while self.owners:
            try: self.close()
            except BaseException as error: errors.append(repr(error))
        self.assertEqual(errors, [])

    def rpc(self, method, params=None, error=None): return self.owner.rpc(method, params, error)

    def source(self, name): return native(self.sources/(name+'.fbx'))

    def imported(self, name, error=None): return self.rpc('asset.import', dict(source=self.source(name)), error)

    def package(self, asset): return Path(str(self.world)+'.assets')/(asset+'.pmodel')

    def state(self):
        state = dict(inspect=self.rpc('world.inspect'), history=self.rpc('world.history'),
                     document=self.world.read_bytes().hex())
        RECORD['state_guards'].append(dict(test=self.id(), owner=self.owner.record['label'], state=state))
        return state

    def packages(self):
        directory = Path(str(self.world)+'.assets')
        result = {}
        for p in sorted(directory.rglob('*')):
            if p.is_symlink(): raise AssertionError('Unexpected linked asset-store entry')
            result[p.relative_to(directory).as_posix()] = identity(p) if p.is_file() else dict(directory=True)
        RECORD['package_guards'].append(dict(test=self.id(), owner=self.owner.record['label'], files=result))
        return result

    def unchanged(self, state, packages):
        self.assertEqual(exact(self.state()), exact(state))
        self.assertEqual(exact(self.packages()), exact(packages))

    def test_profiles_codecs_and_shared_inventory(self):
        state = self.state()
        for name, rough, metal, rf, mf, count in [
            ('two', True, True, .5, .25, 1), ('rough_only', True, False, .5, .25, 1),
            ('metal_only', False, True, .5, .25, 1), ('absent_factors', True, True, 1, 1, 1),
            ('zero_factors', True, True, 0, 0, 1), ('shared_pair', True, True, .5, .25, 1),
            ('transient_retained', True, True, .5, .25, 2), ('animated', True, True, .5, .25, 1)]:
            result = self.imported(name)
            self.assertEqual(result['images'], count)
            self.assertEqual(identity(self.package(result['asset']))['sha256'], result['asset'])
            meta = packed_package(self.package(result['asset']), rough, metal)
            material = meta['primitives'][0]['material']
            self.assertEqual((material['roughness'], material['metallic']), (rf, mf))
            if name in ('shared_pair', 'transient_retained'):
                self.assertEqual(len(meta['primitives']), 2)
                self.assertEqual(meta['primitives'][0]['textures'][1], meta['primitives'][1]['textures'][1])
            self.assertEqual(self.imported('binary/'+name)['asset'], result['asset'])
            self.assertEqual(exact(self.state()), exact(state))
        for name in ('no_maps', 'untextured_defaults', 'disconnected'):
            self.assertEqual(self.imported(name)['images'], 0)
        RECORD['checks'].append(dict(name=self._testMethodName, profiles=11, ascii_binary_pairs=8, passed=True))

    def test_failures_are_atomic_then_exact_source_repairs(self):
        self.imported('two')
        state, packages = self.state(), self.packages()
        for prefix in ('', 'binary/'):
            for name in fixture.INVALID:
                rejected = self.imported(prefix+name, error=-32050)
                if name == 'shader':
                    self.assertIn('FBX scalar-map shader graphs are unsupported.', rejected['message'])
                self.unchanged(state, packages)
            params = dict(source=self.source(prefix+'missing'))
            failure = self.rpc('asset.import', params, error=-32050)
            self.assertEqual(failure['data'], dict(type='fbx_texture_dependency', version=1,
                reason='unavailable', relative_path='repair.png', file_bytes=None,
                byte_limit=32*1024*1024, source_role='base', donor_index=None))
            self.unchanged(state, packages)
            before_source = identity(self.sources/(prefix+'missing.fbx'))
            shutil.copyfile(self.sources/(prefix+'rough.png'), self.sources/(prefix+'repair.png'))
            recovered = self.rpc('asset.import', params)  # identical request/source, no relaxed admission
            self.assertEqual(before_source, identity(self.sources/(prefix+'missing.fbx')))
            self.assertEqual(recovered['asset'], self.imported('two')['asset'])
            packed_package(self.package(recovered['asset']))
            self.assertEqual(exact(self.state()), exact(state))
            packages = self.packages()
        RECORD['checks'].append(dict(name=self._testMethodName, rejected_policies=len(fixture.INVALID)*2,
            repaired_identical_requests=2, passed=True))

    def test_cold_source_absence_retains_full_cooked_pose(self):
        result = self.imported('animated')
        self.assertEqual((result['skins'], result['animations']), (1, 2))
        asset = result['asset']
        nodes = self.rpc('asset.inspect', dict(asset=asset, section='nodes'))
        primitives = self.rpc('asset.inspect', dict(asset=asset, section='primitives'))
        images = self.rpc('asset.inspect', dict(asset=asset, section='images'))
        clips = self.rpc('asset.inspect', dict(asset=asset, section='animations'))['items']
        mesh = next(row for row in nodes['items'] if row['primitives'])
        move = next(row['index'] for row in clips if row['name'] == 'Move')
        params = dict(asset=asset, clip=move, time=.5, loop=False, section='vertices',
                      node=mesh['index'], primitive=mesh['primitives'][0])
        sample = self.rpc('asset.animation.sample', params)
        expected = sorted([[2, 0, 0], [3.25, 0, 0], [2.5, 1, 0]])
        actual = sorted(row['world_position'] for row in sample['items'])
        self.assertEqual(len(actual), 3)
        for point, wanted in zip(actual, expected):
            for value, target in zip(point, wanted): self.assertAlmostEqual(value, target, delta=2e-4)
        state, packages = self.state(), self.packages()
        self.close()
        self.sources.rename(self.directory/'retained-original-sources')
        self.assertFalse(self.sources.exists())
        self.open('source-absent-cold')
        self.assertEqual(exact(self.rpc('asset.inspect', dict(asset=asset, section='nodes'))), exact(nodes))
        self.assertEqual(exact(self.rpc('asset.inspect', dict(asset=asset, section='primitives'))), exact(primitives))
        self.assertEqual(exact(self.rpc('asset.inspect', dict(asset=asset, section='images'))), exact(images))
        self.assertEqual(exact(self.rpc('asset.animation.sample', params)), exact(sample))
        self.assertEqual(exact(self.rpc('world.inspect')), exact(state['inspect']))
        self.assertEqual(self.world.read_bytes().hex(), state['document'])
        self.assertEqual(exact(self.packages()), exact(packages))
        packed_package(self.package(asset))
        RECORD['checks'].append(dict(name=self._testMethodName, cold_source_absent=True,
            full_cooked_sections=['nodes', 'primitives', 'images', 'vertices'], passed=True))


def main():
    global ARGS, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    ARGS = parser.parse_args()
    ARGS.output = (ARGS.output or ROOT/'build'/('fbx-scalars-contract-'+uuid.uuid4().hex)).resolve()
    if ARGS.output.exists(): parser.error('--output must be new')
    ARGS.output.mkdir(parents=True)
    DEADLINE = time.monotonic()+600
    shared.ARGS = SimpleNamespace(windows_interop=ARGS.windows_interop, timeout=90)
    shared.RECORD = RECORD
    paths = [Path(__file__), Path(fixture.__file__), Path(fixture.__file__).with_name('fbx_fixture.py'),
             Path(fixture.__file__).with_name('texture_fixture.py'), Path(fixture.__file__).with_name('gltf_fixture.py'),
             Path(shared.__file__), Path(shared.__file__).with_name('scene_capture.py'), ARGS.binary]
    RECORD['source_pins'] = {str(p.resolve()): identity(p) for p in paths}
    began = time.monotonic()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ScalarContract))
    RECORD.update(passed=result.wasSuccessful(), tests=result.testsRun, failures=len(result.failures),
                  errors=len(result.errors), skipped=len(result.skipped), seconds=time.monotonic()-began,
                  scope='Headless original FBX scalar profile only; no renderer/exporter-application qualification')
    try:
        if time.monotonic() > DEADLINE: raise AssertionError('Whole protocol deadline exceeded')
        for path, pin in RECORD['source_pins'].items():
            if identity(Path(path)) != pin: raise AssertionError('Source/binary changed')
        size = sum(p.stat().st_size for p in ARGS.output.rglob('*') if p.is_file())
        if size > 256*1024*1024: raise AssertionError('Output exceeds256MiB')
        if any(o['exit_code'] != 0 or o['forced_cleanup'] for o in RECORD['owners']):
            raise AssertionError('Native owner did not close naturally')
    except BaseException as error:
        RECORD['passed'] = False
        RECORD['integrity_error'] = repr(error)
    destination = ARGS.evidence or ARGS.output/'evidence.json'
    encoded = json.dumps(RECORD, indent=2, ensure_ascii=False, allow_nan=False)+'\n'
    if len(encoded.encode('utf-8')) > 64*1024*1024: raise AssertionError('Evidence exceeds64MiB')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(encoded, encoding='utf-8')
    return 0 if RECORD['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
