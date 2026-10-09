#!/usr/bin/env python3
"""Native FBX import/composition, analytic CPU pose inspection and publication.

Original local fixtures only; no renderer, network or external application.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import unittest
import uuid
from fbx_fixture import write_fixtures

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError
ARGS = None
CALLS, OWNERS = [], []


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native(path):
    text = str(Path(path).resolve())
    return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip() if ARGS.windows_interop else text


class FbxContract(unittest.TestCase):
    def setUp(self):
        self.directory = ARGS.output / self._testMethodName
        self.directory.mkdir()
        self.fixtures = write_fixtures(self.directory / 'sources')
        self.world = self.directory / 'world.json'
        self.owners = []
        self.open()
        self.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0,
                  ops=[dict(op='entity.create', id='1' * 32, name='Existing authored object')]))

    def open(self):
        self.client = WorldClient.open(str(ARGS.binary.resolve()), native(self.world))
        self.owners.append(self.client)

    def close(self):
        owner = self.owners.pop()
        try:
            owner.close()
        finally:
            code = owner.transport.returncode
            OWNERS.append(dict(test=self.id(), exit_code=code))
        self.assertEqual(code, 0)

    def tearDown(self):
        errors = []
        while self.owners:
            try:
                self.close()
            except BaseException as error:
                errors.append(str(error))
        self.assertEqual(errors, [])

    def call(self, method, params=None, error=None):
        params = {} if params is None else params
        row = dict(test=self.id(), method=method, params=params)
        CALLS.append(row)
        try:
            result = self.client.call(method, params, timeout=60)
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=str(failure))
            self.assertIsNotNone(error)
            self.assertEqual(failure.code, error)
            return
        row['result'] = result
        self.assertIsNone(error, 'Expected rejection')
        return result

    def imported(self, source, animations=None, error=None):
        params = dict(source=native(self.fixtures / (source + '.fbx')))
        if animations is not None:
            params['animations'] = [native(self.fixtures / (name + '.fbx')) for name in animations]
        return self.call('asset.import', params, error)

    def state(self):
        return (self.call('world.inspect'), self.call('world.history'), self.world.read_bytes())

    def package_tree(self):
        directory = Path(str(self.world) + '.assets')
        return {p.name: sha(p) for p in directory.glob('*') if p.is_file()}

    def pose(self, asset, t=.137, clip_name='Move'):
        nodes = self.call('asset.inspect', dict(asset=asset, section='nodes'))['items']
        mesh = next(row for row in nodes if row['primitives'])
        clips = self.call('asset.inspect', dict(asset=asset, section='animations'))['items']
        move = next(row['index'] for row in clips if row['name'] == clip_name)
        sample = self.call('asset.animation.sample', dict(asset=asset, clip=move, time=t, loop=False,
                           section='vertices', node=mesh['index'], primitive=mesh['primitives'][0]))
        actual = sorted(row['world_position'] for row in sample['items'])
        expected = sorted([[2, 0, 0], [3 + .5*t, 0, 0], [2+t, 1, 0]])
        self.assertEqual(len(actual), 3)
        for point, wanted in zip(actual, expected):
            for value, reference in zip(point, wanted):
                self.assertAlmostEqual(value, reference, delta=2e-4)
        return sample

    def test_embedded_takes_identity_and_fresh_source_independence(self):
        before = self.state()
        first = self.imported('animated')
        self.assertEqual((first['skins'], first['animations'], first['triangles']), (1, 2, 1))
        second = self.imported('animated')
        self.assertEqual(first['asset'], second['asset'])
        binary = self.imported('binary/animated')
        self.assertEqual(binary['asset'], first['asset'])
        package = Path(str(self.world) + '.assets') / (first['asset'] + '.pmodel')
        self.assertEqual(sha(package), first['asset'])
        self.assertEqual(self.state(), before)
        observed = self.pose(first['asset'])
        self.close()
        shutil.rmtree(self.fixtures)
        self.open()
        self.assertEqual(self.pose(first['asset']), observed)
        self.assertEqual(self.call('world.inspect'), before[0])
        self.assertEqual(self.world.read_bytes(), before[2])

    def test_composition_instantiation_preview_retry_and_history(self):
        imported = self.imported('skin', ['move_donor', 'turn_donor'])
        asset = imported['asset']
        self.assertEqual(imported['animations'], 2)
        self.assertEqual(self.imported('skin', ['move_donor', 'turn_donor'])['asset'], asset)
        self.assertNotEqual(self.imported('skin')['asset'], asset)
        self.pose(asset, .813)
        for donor in ['meter_move_donor', 'z_up_move_donor']:
            equivalent = self.imported('skin', [donor, 'turn_donor'])
            self.pose(equivalent['asset'], .813)
        before = self.state()
        params = dict(request_id=uuid.uuid4().hex, base_revision=1,
                      ops=[dict(op='asset.instantiate', id='2' * 32, name='FBX rig', asset=asset)])
        self.call('world.transact', dict(params, preview=True))
        self.assertEqual(self.state(), before)
        result = self.call('world.transact', params)
        committed = self.state()
        retry = self.call('world.transact', params)
        self.assertEqual(retry['revision'], result['revision'])
        self.assertEqual(self.state(), committed)
        entities = json.loads(self.world.read_text())['entities']
        self.assertEqual(entities['2' * 32]['components']['AnimationRig']['asset'], asset)
        undo = self.call('world.undo', dict(request_id=uuid.uuid4().hex, base_revision=result['revision']))
        self.assertNotIn('2' * 32, json.loads(self.world.read_text())['entities'])
        self.call('world.redo', dict(request_id=uuid.uuid4().hex, base_revision=undo['revision']))
        self.assertEqual(json.loads(self.world.read_text())['entities'], entities)

    def test_clip_selection_renaming_and_normal_convention(self):
        before = self.state()
        source = native(self.fixtures / 'skin.fbx')
        take_source = native(self.fixtures / 'animated.fbx')
        selected = self.call('asset.import', dict(source=source, animations=[
            dict(source=take_source, clip=0, name='SelectedMove')]))
        rows = self.call('asset.inspect', dict(asset=selected['asset'], section='animations'))['items']
        self.assertEqual([row['name'] for row in rows], ['SelectedMove'])
        self.pose(selected['asset'], clip_name='SelectedMove')
        named = self.call('asset.import', dict(source=source, animations=[
            dict(source=native(self.fixtures / 'move_donor.fbx'), name='Renamed')]))
        rows = self.call('asset.inspect', dict(asset=named['asset'], section='animations'))['items']
        self.assertEqual([row['name'] for row in rows], ['Renamed'])
        self.pose(named['asset'], clip_name='Renamed')
        normal_source = native(self.fixtures / 'normal_map.fbx')
        default = self.call('asset.import', dict(source=normal_source))
        opengl = self.call('asset.import', dict(source=normal_source, fbx_normal_map='opengl'))
        directx = self.call('asset.import', dict(source=normal_source, fbx_normal_map='directx'))
        self.assertEqual(default['asset'], opengl['asset'])
        self.assertNotEqual(opengl['asset'], directx['asset'])
        packages = self.package_tree()
        for donors in [[dict(source=take_source, name='Ambiguous')],
                       [dict(source=take_source, clip=255)],
                       [dict(source=take_source, clip=0, name='Duplicate'),
                        dict(source=take_source, clip=1, name='Duplicate')]]:
            self.call('asset.import', dict(source=source, animations=donors), error=-32050)
            self.assertEqual(self.package_tree(), packages)
        for selector in [dict(source=take_source, clip=-1), dict(source=take_source, clip=256),
                         dict(source=take_source, clip=True), dict(source=take_source, name=''),
                         dict(source=take_source, name='é'*129), dict(source=take_source, unknown=1)]:
            self.call('asset.import', dict(source=source, animations=[selector]), error=-32602)
        self.call('asset.import', dict(source=normal_source, fbx_normal_map='unknown'), error=-32602)
        self.assertEqual(self.package_tree(), packages)
        self.assertEqual(self.state(), before)

    def test_identity_white_is_explicitly_admitted(self):
        before = self.state()
        plain, white = self.imported('static'), self.imported('white_color')
        self.assertEqual((white['vertices'], white['triangles'], white['primitives']),
                         (plain['vertices'], plain['triangles'], plain['primitives']))
        diagnostics = [text for text in white['diagnostics'] if 'white' in text and 'color' in text]
        self.assertEqual(len(diagnostics), 1)
        a = self.call('asset.inspect', dict(asset=plain['asset'], section='primitives'))['items']
        b = self.call('asset.inspect', dict(asset=white['asset'], section='primitives'))['items']
        self.assertEqual(a, b)
        self.assertEqual(self.imported('binary/white_color')['asset'], white['asset'])
        inactive = self.imported('inactive_normal')
        self.assertEqual(self.call('asset.inspect', dict(asset=inactive['asset'], section='primitives'))['items'], a)
        self.assertEqual(self.imported('binary/inactive_normal')['asset'], inactive['asset'])
        self.assertEqual(self.state(), before)

    def test_filesystem_diagnostics_preserve_owner_and_publication(self):
        if ARGS.windows_interop or os.name != 'posix':
            self.skipTest('Arbitrary invalid UTF-8 filename bytes are a POSIX boundary')
        self.close()
        # The request stays valid UTF-8; only the native world parent contains
        # arbitrary filesystem bytes, which file_size embeds in its error.
        parent = Path(os.fsdecode(os.fsencode(self.directory) + b'/parent-\xff'))
        parent.mkdir()
        self.world.rename(parent / 'world.json')
        self.world = parent / 'world.json'
        shutil.copytree(self.fixtures, parent / 'sources')
        self.open()
        before, packages = self.state(), self.package_tree()
        self.call('asset.import', dict(source='sources/skin.fbx',
                                      animations=['missing-take.fbx']), error=-32050)
        message = CALLS[-1]['error']['message']
        self.assertIn('missing-take.fbx', message)
        self.assertIn('\ufffd', message)
        self.assertLessEqual(len(message.encode('utf-8', errors='strict')), 1024)
        self.assertEqual(self.state(), before)
        self.assertEqual(self.package_tree(), packages)
        # Exercise output truncation at multibyte boundaries as well as repair.
        long_missing = '/'.join(['é' * 50] * 14) + '/missing.fbx'
        self.call('asset.import', dict(source=long_missing), error=-32050)
        self.assertLessEqual(len(CALLS[-1]['error']['message'].encode('utf-8', errors='strict')), 1024)
        self.assertEqual(self.state(), before)
        self.assertEqual(self.package_tree(), packages)
        recovered = self.call('asset.import', dict(source='sources/skin.fbx'))
        self.assertEqual(recovered['skins'], 1)
        self.assertEqual(self.state(), before)
        self.assertEqual(sha(Path(str(self.world) + '.assets') / (recovered['asset'] + '.pmodel')), recovered['asset'])

    def test_publication_rejects_existing_stages_and_linked_store(self):
        first = self.imported('static')
        asset = first['asset']
        directory = Path(str(self.world) + '.assets')
        package = directory / (asset + '.pmodel')
        expected = package.read_bytes()
        package.unlink()
        stage = directory / (asset + '.pending')
        stage.write_bytes(b'owned abandoned stage must not be truncated')
        before, protected = self.state(), self.package_tree()
        self.imported('static', error=-32050)
        self.assertEqual(self.state(), before)
        self.assertEqual(self.package_tree(), protected)
        self.assertFalse(package.exists())
        stage.unlink()
        victim = self.directory / 'owned-victim.bin'
        victim.write_bytes(b'owned unrelated data must remain exact')
        victim_hash = sha(victim)
        stage.symlink_to(victim.resolve())
        self.imported('static', error=-32050)
        self.assertTrue(stage.is_symlink())
        self.assertEqual(sha(victim), victim_hash)
        self.assertFalse(package.exists())
        self.assertEqual(self.state(), before)
        stage.unlink()
        recovered = self.imported('static')
        self.assertEqual(recovered['asset'], asset)
        self.assertEqual(package.read_bytes(), expected)
        self.assertEqual(self.state(), before)
        # A separate owned directory must not receive a package through an
        # asset-store link. Keep the legitimate store intact for recovery.
        retained = self.directory / 'retained-asset-store'
        directory.rename(retained)
        foreign = self.directory / 'owned-foreign-store'
        foreign.mkdir()
        directory.symlink_to(foreign.resolve(), target_is_directory=True)
        try:
            self.imported('static', error=-32050)
            self.assertEqual(list(foreign.iterdir()), [])
            self.assertEqual((retained / package.name).read_bytes(), expected)
            self.assertEqual(sha(victim), victim_hash)
            self.assertEqual(self.state(), before)
        finally:
            directory.unlink()
            retained.rename(directory)
        self.assertEqual(self.imported('static')['asset'], asset)
        self.assertEqual(self.state(), before)

    def test_rejections_do_not_publish_or_mutate_authoring(self):
        self.imported('skin')
        before, packages = self.state(), self.package_tree()
        for source, donors in [('skin', ['move_donor', 'move_donor']),
                               ('skin', ['rest_mismatch']), ('animated', ['move_donor']),
                               ('dual_quaternion', None), ('influence_overflow', None),
                               ('blend_shape', None), ('node_overflow', None),
                               ('position_overflow', None), ('truncated', None),
                               ('nonwhite_color', None), ('nonidentity_alpha', None),
                               ('multiple_colors', None), ('invalid_color_index', None),
                               ('nonfinite_color', None), ('active_normal_response', None),
                               ('duplicate_object_id', None), ('truncated_materials', None),
                               ('binary/active_normal_response', None),
                               ('binary/duplicate_object_id', None), ('binary/truncated_materials', None)]:
            self.imported(source, donors, error=-32050)
            self.assertEqual(self.state(), before)
            self.assertEqual(self.package_tree(), packages)
        for animations in [[], [native(self.fixtures / 'move_donor.fbx')] * 33, [None], [''], ['bad\0path']]:
            self.call('asset.import', dict(source=native(self.fixtures / 'skin.fbx'), animations=animations), error=-32602)
            self.assertEqual(self.state(), before)
            self.assertEqual(self.package_tree(), packages)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--evidence', type=Path)
    ARGS = parser.parse_args()
    if ARGS.output is None:
        ARGS.output = ROOT / 'build' / ('fbx-contract-' + uuid.uuid4().hex)
    if ARGS.output.exists():
        parser.error('--output must be new')
    ARGS.output.mkdir(parents=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(FbxContract))
    evidence = dict(passed=result.wasSuccessful(), tests=result.testsRun, failures=len(result.failures),
                    errors=len(result.errors), skipped=len(result.skipped), calls=CALLS, owners=OWNERS,
                    binary_sha256=sha(ARGS.binary), source_sha256=sha(Path(__file__)),
                    fixture_sha256=sha(Path(__file__).with_name('fbx_fixture.py')))
    destination = ARGS.evidence or ARGS.output / 'evidence.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    sys.exit(0 if evidence['passed'] else 1)
