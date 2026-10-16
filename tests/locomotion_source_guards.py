#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Launcher request-shape tests with fake clients; no native execution."""
from pathlib import Path
import importlib.util
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


launcher = load('locomotion_guard_launcher', ROOT/'examples/locomotion-yard/run.py')
knight = load('locomotion_guard_knight', ROOT/'examples/locomotion-yard/knight_profile.py')
BASE, RUN, IDLE, COMPOSED = ('a'*64, 'b'*64, 'c'*64, 'd'*64)


class FakeAuthor:
    """Only the authoring protocol surface; never opens a native client."""
    def __init__(self, args, directory, knight_profile=False):
        self.args, self.directory, self.manifest = args, directory, {}
        self.calls, self.knight_profile = [], knight_profile
        self.takes = [dict(index=i, name='Other'+str(i), channels=0) for i in range(76)]
        self.takes[0]['channels'] = 8467
        self.takes[36].update(name='Idle', channels=123)
        self.takes[48].update(name='Running_A', channels=122)

    def native(self, path):
        return str(path)

    def rpc(self, method, params=None):
        self.calls.append((method, params))
        if method == 'asset.source.inspect':
            name = Path(params['source']).name
            return dict(model_sha256={'characterMedium.fbx': BASE, 'run.fbx': RUN,
                                      'idle.fbx': IDLE, 'Knight.glb': BASE}[name])
        if method == 'asset.import':
            return dict(asset=COMPOSED if 'animations' in params else BASE, nodes=1)
        if method == 'world.navigation.bake':
            return dict(asset='e'*64)
        if method == 'asset.provenance.create':
            return dict(record='f'*64)
        if method in ('component.schema.import', 'world.transact'):
            return {}
        raise AssertionError(('Unexpected fake operation', method, params))

    def page(self, method, params):
        if self.knight_profile:
            if params['section'] == 'nodes':
                return [dict(index=i) for i in range(57)]
            return self.takes
        if method == 'asset.animation.sample':
            return [dict(world_position=[0, 0, 0]), dict(world_position=[0, 2.4, 0])]
        if method == 'asset.inspect':
            return [dict(index=0, name='Run'), dict(index=1, name='Idle')]
        if Path(params['source']).name == 'characterMedium.fbx':
            return [dict(index=0, primitives=[0])]
        if params['section'] == 'nodes':
            return [dict(index=0, name='HipsCtrl'), dict(index=1, name='Hips')]
        name = Path(params['source']).stem.title()
        return [dict(index=0, name='Root|0.Targeting Pose'), dict(index=1, name='Root|'+name)]


class SourceGuards(unittest.TestCase):
    def kenney(self, flag):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            for name in launcher.SOURCE_SHA:
                path = directory/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()  # Empty fake input; no imported asset or generated art.
            args = SimpleNamespace(source_profile='kenney-fbx')
            if flag is not None:
                args.guard_sources = flag
            game = FakeAuthor(args, directory)
            with patch.object(launcher, 'sha', lambda p: launcher.SOURCE_SHA[Path(p).relative_to(directory).as_posix()]):
                launcher.OwnedGame.author(game, directory)
            return game, directory

    def expected_kenney(self, directory, guarded):
        source = str(directory/'Model/characterMedium.fbx')
        baseline = dict(source=source)
        if guarded:
            baseline['expected_model_sha256'] = BASE
        selections = []
        for name, fingerprint in [('run', RUN), ('idle', IDLE)]:
            policy = dict(policy='reference-rotation-v1', source_pose=dict(kind='sample', clip=0, time=0),
                          target_pose=dict(kind='rest'), positions=dict(kind='reference_delta', nodes=[0, 1], scale=1),
                          scales='target_reference', expected_source_model_sha256=fingerprint,
                          expected_target_model_sha256=BASE)
            selection = dict(source=str(directory/('Animations/'+name+'.fbx')), clip=1, name=name.title(), retarget=policy)
            if guarded:
                selection['expected_model_sha256'] = fingerprint
            selections.append(selection)
        return [baseline, dict(baseline, animations=selections)]

    def test_omitted_and_false_preserve_complete_kenney_import_requests(self):
        for flag in (None, False):
            with self.subTest(flag=flag):
                game, directory = self.kenney(flag)
                self.assertEqual([p for method, p in game.calls if method == 'asset.import'],
                                 self.expected_kenney(directory, False))

    def test_guarded_kenney_binds_both_base_imports_and_each_donor(self):
        game, directory = self.kenney(True)
        imports = [p for method, p in game.calls if method == 'asset.import']
        self.assertEqual(imports, self.expected_kenney(directory, True))
        self.assertNotIn(BASE, launcher.SOURCE_SHA.values())
        self.assertNotIn(RUN, launcher.SOURCE_SHA.values())
        self.assertNotIn(IDLE, launcher.SOURCE_SHA.values())

    def test_donor_guard_is_independent_of_selection_policy_and_does_not_mutate(self):
        frame = dict(policy='reference-frame-v1', source_pose=dict(kind='rest'), target_pose=dict(kind='rest'))
        rotation = dict(policy='reference-rotation-v1', source_pose=dict(kind='rest'), target_pose=dict(kind='rest'),
                        positions=dict(kind='target_reference'), scales='target_reference',
                        expected_source_model_sha256=RUN, expected_target_model_sha256=BASE)
        for key, policy in ((None, None), ('frame_transfer', frame), ('retarget', rotation)):
            selection = dict(source='donor.fbx', clip=2, name='Selected')
            if policy is not None:
                selection[key] = policy
            with self.subTest(policy=policy):
                self.assertEqual(launcher.source_selection(selection, RUN), selection)
                self.assertEqual(launcher.source_selection(selection, RUN, False), selection)
                self.assertEqual(launcher.source_selection(selection, RUN, True), dict(selection, expected_model_sha256=RUN))
                self.assertNotIn('expected_model_sha256', selection)

    def test_intact_knight_import_guard_is_opt_in_and_uses_normalized_fingerprint(self):
        for flag in (None, False, True):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as folder:
                directory = Path(folder)
                args = SimpleNamespace()
                if flag is not None:
                    args.guard_sources = flag
                game = FakeAuthor(args, directory, knight_profile=True)
                with patch.object(knight, 'original', lambda path, size, digest: dict(bytes=size, sha256=digest)):
                    knight.author_knight(game, directory, launcher)
                expected = dict(source=str(directory/'Knight.glb'))
                if flag:
                    expected['expected_model_sha256'] = BASE
                self.assertEqual([p for method, p in game.calls if method == 'asset.import'], [expected])
                self.assertNotEqual(BASE, knight.SOURCE_SHA)
                self.assertEqual(len(game.authored['nodes']), 57)

    def test_rpc_preserves_structured_and_falsy_error_data_without_inventing_null(self):
        for value in (None, {}, [], False, 0, '', dict(reason='profile', nodes=[dict(index=4)])):
            with self.subTest(value=value):
                error = launcher.RpcError(-32050, 'Native rejection', value)
                client = SimpleNamespace(call=lambda *a, **kw: (_ for _ in ()).throw(error))
                owner = object.__new__(launcher.OwnedGame)
                owner.deadline, owner.label, owner.calls = time.monotonic()+10, 'fake', 0
                owner.record, owner.client = dict(calls=[]), client
                result = owner.rpc('asset.import', dict(source='source.fbx'), error=-32050)
                self.assertEqual(result['code'], -32050)
                self.assertEqual(result['message'], 'Native rejection')
                if value is None:
                    self.assertNotIn('data', result)
                else:
                    self.assertIs(result['data'], value)
                self.assertIs(owner.record['calls'][0]['error'], result)

    def test_snapshot_uses_complete_authored_membership_for_either_profile(self):
        for count in (57, 61):
            with self.subTest(count=count):
                owner = object.__new__(launcher.OwnedGame)
                identities = [launcher.uid(1000+i) for i in range(count)]
                owner.authored, owner.tick, owner.session = dict(nodes=identities), 4, 'session'
                owner.inspect = lambda: dict(structure_revision=0, ui_revision=1, control_sequence=0)
                owner.module = lambda: dict(module=dict(values={}, backend='fake'), revision=1)
                owner.route = lambda: dict(component_revision=2)
                owner.entity = lambda identity=launcher.ACTOR: dict(id=identity)
                owner.visual_config = lambda: {}
                owner.rpc = lambda *args: {}
                result = owner.snapshot()
                self.assertEqual(result['nodes'], [dict(id=v) for v in identities])


if __name__ == '__main__':
    unittest.main()
