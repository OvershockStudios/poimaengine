#!/usr/bin/env python3
"""Exercise isolated source drafts and guarded native project integration."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid
import wave

from texture_fixture import png


ROOT = Path(__file__).resolve().parents[1]
CALLS = []
MANIFEST = 'project.poimaproject'
DRAFT_MARKER = '.poima-draft.json'
GATE, LIGHT, CUE = (f'{number:032x}' for number in (101, 102, 103))


def native(path):
    path = str(path.resolve())
    if ARGS.windows_interop:
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip()
    return path


def read_json(path):
    return json.loads(path.read_bytes())


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def tree(root):
    return {path.relative_to(root).as_posix(): None if path.is_dir() else path.read_bytes()
            for path in root.rglob('*')}


def tone(path, amplitude):
    samples = (0, amplitude, -amplitude, 0)
    with wave.open(str(path), 'wb') as writer:
        writer.setparams((1, 2, 48000, len(samples), 'NONE', 'not compressed'))
        writer.writeframes(struct.pack('<4h', *samples))


def create(entity, name):
    return {'op': 'entity.create', 'id': entity, 'name': name}


def rename(entity, name):
    return {'op': 'entity.rename', 'id': entity, 'name': name}


def emitter(entity, asset):
    return {'op': 'component.set', 'id': entity, 'type': 'AudioEmitter',
            'value': {'asset': asset, 'gain': 1, 'loop': False, 'enabled': False}}


class ProjectDraftContract(unittest.TestCase):
    def setUp(self):
        if ARGS.scratch_root:
            self.root = ARGS.scratch_root / self._testMethodName
            self.root.mkdir()
        else:
            scratch = ROOT / 'build/project-draft-contract'
            scratch.mkdir(parents=True, exist_ok=True)
            temporary = tempfile.TemporaryDirectory(dir=scratch)
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)

        self.live = self.root / 'Live project'
        self.cli('project', 'create', native(self.live), '--name', 'Draft contract', '--gameplay')
        self.spec = read_json(self.live / MANIFEST)
        self.world_path = self.spec['entry']['world']
        (self.live / 'Design.txt').write_bytes(b'Base design\n')
        tone(self.live / 'Base.wav', 100)
        self.base_asset = self.world(self.live, 'asset.audio.import', source='Base.wav')['asset']
        changes = [create(GATE, 'Base gate'), create(LIGHT, 'Base light'), emitter(GATE, self.base_asset)]
        initial_revision = self.world(self.live, 'world.inspect')['revision']
        self.revision = self.edit(self.live, initial_revision, changes)['revision']
        self.original = tree(self.live)
        self.live_summary = self.inspect(self.live)

        self.base = self.root / 'Base checkpoint'
        summary = self.cli('project', 'checkpoint', native(self.live / MANIFEST), '--output', native(self.base))
        self.checkpoint = self.base / 'checkpoint.json'
        self.source = self.base / 'source'
        self.base_sha256 = summary['source_sha256']
        self.assertEqual(self.base_sha256, sha256(self.checkpoint.read_bytes()))
        self.assertEqual(summary['revision'], self.revision)
        self.assert_path(summary['source_manifest'], self.source / MANIFEST)
        self.assertFalse((self.base / 'checks.json').exists())
        sidecars = {path + suffix for path in (self.world_path, self.spec['input_profile'])
                    for suffix in ('.lock', '.pending', '.previous')}
        self.assertEqual(tree(self.source), {path: content for path, content in self.original.items()
                                            if path not in sidecars})
        self.base_bytes = tree(self.base)
        self.assertEqual(tree(self.live), self.original)

    def run_native(self, *argv, stdin=None):
        command = [str(ARGS.binary.resolve()), *argv]
        completed = subprocess.run(command, input=stdin, capture_output=True, text=True,
                                   encoding='utf-8', timeout=30)
        CALLS.append({'test': self._testMethodName, 'command': command, 'stdin': stdin,
                      'stdout': completed.stdout, 'stderr': completed.stderr, 'exit_code': completed.returncode})
        try:
            reply = json.loads(completed.stdout)
        except ValueError:
            self.fail(f'Expected a native JSON reply for {command}: {completed.stdout}\n{completed.stderr}')
        return completed.returncode, reply

    def cli(self, *argv, error=None):
        code, reply = self.run_native(*argv)
        self.assertEqual(code, 4 if error else 0, reply)
        self.assertEqual(reply['status'], 'error' if error else 'ok', reply)
        if error:
            diagnostic = reply['diagnostics'][0]
            self.assertEqual(diagnostic['code'], 'project.failed', reply)
            self.assertRegex(diagnostic['message'], '(?i)' + error)
            return None
        return reply['result']

    def world(self, root, method, **params):
        request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}
        code, reply = self.run_native('world', native(root / self.world_path), stdin=json.dumps(request) + '\n')
        self.assertEqual(code, 0, reply)
        self.assertEqual(reply['id'], 1, reply)
        self.assertIn('result', reply, reply)
        return reply['result']

    def inspect(self, root):
        return self.cli('project', 'inspect', native(root / MANIFEST))

    def assert_path(self, reported, expected):
        self.assertEqual(reported.replace('\\', '/'), native(expected).replace('\\', '/'))

    def assert_fork(self, root, result):
        self.assert_path(result['manifest'], root / MANIFEST)
        self.assertRegex(result['project_id'], '^[0-9a-f]{32}$')
        self.assertNotEqual(result['project_id'], self.spec['project_id'])
        self.assertEqual(result['base_sha256'], self.base_sha256)
        self.assertEqual(result['base_revision'], self.revision)
        self.assertEqual(read_json(root / MANIFEST), {**self.spec, 'project_id': result['project_id']})

    def reject(self, error, *argv):
        before = tree(self.root)
        self.cli(*argv, error=error)
        self.assertEqual(tree(self.root), before, 'A rejected command changed files or left staging directories.')

    def draft(self, target, *, base_sha256=None, error=None):
        argv = ('project', 'draft', native(self.checkpoint), '--output', native(target),
                '--expected-base-sha256', self.base_sha256 if base_sha256 is None else base_sha256)
        if error:
            return self.reject(error, *argv)
        result = self.cli(*argv)
        self.assert_fork(target, result)
        self.assertEqual(read_json(target / DRAFT_MARKER), {
            'format': 'poima.project-draft', 'version': 1, 'base_sha256': self.base_sha256,
            'base_project_id': self.spec['project_id'], 'base_revision': self.revision,
            'project_id': result['project_id'], 'project': MANIFEST,
        })
        summary = self.inspect(target)
        for field in ('entry', 'revision', 'world_sha256', 'assets'):
            self.assertEqual(summary[field], self.live_summary[field])
        self.assertEqual(summary['project_id'], result['project_id'])
        untouched = {path: contents for path, contents in tree(target).items()
                     if path not in (MANIFEST, DRAFT_MARKER)}
        self.assertEqual(untouched, {path: contents for path, contents in tree(self.source).items()
                                    if path != MANIFEST})
        return result

    def draft_changes(self, target, *, base_sha256=None, error=None):
        argv = ('project', 'draft-changes', native(self.checkpoint), '--draft', native(target / MANIFEST),
                '--expected-base-sha256', self.base_sha256 if base_sha256 is None else base_sha256)
        if error:
            return self.reject(error, *argv)
        return self.cli(*argv)

    def transaction(self, revision, ops):
        return {'request_id': uuid.uuid4().hex, 'base_revision': revision, 'ops': ops}

    def edit(self, root, revision, ops):
        receipt = self.world(root, 'world.transact', **self.transaction(revision, ops))
        self.assertIs(receipt['committed'], True)
        self.assertEqual(receipt['revision'], revision + 1)
        return receipt

    def pick(self, root, *paths):
        return {'manifest': native(root / MANIFEST),
                'files': [{'path': path, 'sha256': sha256((root / path).read_bytes())} for path in paths]}

    def pick_scene(self, root, *paths):
        return {**self.pick(root, *paths), 'world_sha256': self.inspect(root)['world_sha256']}

    def integrate(self, target, request, error=None):
        request_path = self.root / f'integrate-{uuid.uuid4().hex}.json'
        payload = request if isinstance(request, str) else json.dumps(request) + '\n'
        request_path.write_bytes(payload.encode('utf-8'))
        argv = ('project', 'integrate', native(self.checkpoint), '--output', native(target),
                '--request', native(request_path))
        if error:
            return self.reject(error, *argv)
        result = self.cli(*argv)
        self.assert_fork(target, result)
        return result

    def names(self, root, revision):
        return {entity['id']: entity['name'] for entity in self.world(root, 'entity.query',
                                                                   revision=revision)['entities']}

    def test_two_drafts_publish_one_composed_project(self):
        gate = self.root / 'Gate draft'
        light = self.root / 'Light draft'
        with ThreadPoolExecutor(max_workers=2) as workers:
            gate_id, light_id = [result['project_id'] for result in workers.map(self.draft, (gate, light))]
        self.assertNotEqual(gate_id, light_id)

        (gate / 'Gate.txt').write_bytes(b'Gate worker design\n')
        (light / 'Design.txt').write_bytes(b'Light worker design\n')
        tone(gate / 'Worker.wav', 600)
        imported = self.world(gate, 'asset.audio.import', source='Worker.wav')
        asset = imported['asset']
        self.assertNotEqual(asset, self.base_asset)
        package = f'{self.world_path}.assets/{asset}.paudio'
        self.assertEqual(sha256((gate / package).read_bytes()), asset)
        gate_ops = [rename(GATE, 'Worker gate'), create(CUE, 'Gate cue'), emitter(CUE, asset)]
        light_ops = [rename(LIGHT, 'Worker light')]
        with ThreadPoolExecutor(max_workers=2) as workers:
            gate_edit = workers.submit(self.edit, gate, self.revision, gate_ops)
            light_edit = workers.submit(self.edit, light, self.revision, light_ops)
            revised_gate = gate_edit.result()['revision']
            revised_light = light_edit.result()['revision']

        gate_names = self.names(gate, revised_gate)
        light_names = self.names(light, revised_light)
        self.assertEqual([gate_names[key] for key in (GATE, LIGHT, CUE)], ['Worker gate', 'Base light', 'Gate cue'])
        self.assertEqual([light_names[key] for key in (GATE, LIGHT)], ['Base gate', 'Worker light'])
        self.assertNotIn(CUE, light_names)
        self.assertFalse((light / package).exists())

        request = {'base_sha256': self.base_sha256,
                   'drafts': [self.pick(gate, package, 'Gate.txt'), self.pick(light, 'Design.txt')],
                   'transaction': self.transaction(self.revision, gate_ops + light_ops)}
        missing_asset = {**request, 'drafts': [self.pick(gate, 'Gate.txt'), self.pick(light, 'Design.txt')]}
        candidate = self.root / 'Integrated candidate'
        self.integrate(candidate, missing_asset, error='asset|cooked|paudio')

        gate_bytes, light_bytes = tree(gate), tree(light)
        result = self.integrate(candidate, request)
        self.assertNotIn(result['project_id'], (gate_id, light_id))
        revision = self.revision + 1
        self.assertEqual(result['revision'], revision)
        self.assertIs(result['transaction']['committed'], True)
        self.assertEqual(result['transaction']['revision'], revision)
        selected = [selection for draft in request['drafts'] for selection in draft['files']]
        self.assertEqual(result['merged_files'], sorted(selected, key=lambda selection: selection['path']))

        candidate_bytes = tree(candidate)
        expected = {**tree(self.source), package: gate_bytes[package], 'Gate.txt': gate_bytes['Gate.txt'],
                    'Design.txt': light_bytes['Design.txt']}
        for path in (MANIFEST, self.world_path):
            self.assertNotEqual(candidate_bytes.pop(path), expected.pop(path))
        self.assertEqual(candidate_bytes, expected)
        summary = self.inspect(candidate)
        self.assertEqual(summary['entry'], self.spec['entry'])
        self.assertEqual(summary['project_id'], result['project_id'])
        self.assertEqual(summary['revision'], revision)
        self.assertEqual({record['sha256'] for record in summary['assets']}, {asset, self.base_asset})
        names = self.names(candidate, revision)
        self.assertEqual([names[key] for key in (GATE, LIGHT, CUE)], ['Worker gate', 'Worker light', 'Gate cue'])
        cue = self.world(candidate, 'entity.get', id=CUE, component='AudioEmitter', revision=revision)
        self.assertEqual(cue['value'], emitter(CUE, asset)['value'])
        self.assertEqual(self.world(candidate, 'asset.audio.inspect', asset=asset), imported)

        pinned = {'base_sha256': self.base_sha256,
                  'drafts': [self.pick_scene(gate, 'Gate.txt'), self.pick_scene(light, 'Design.txt')]}
        automatic = self.root / 'Automatically merged candidate'
        merged = self.integrate(automatic, pinned)
        self.assertEqual(merged['revision'], revision)
        self.assertEqual([row['world_sha256'] for row in merged['scene_sources']],
                         [selection['world_sha256'] for selection in pinned['drafts']])
        self.assertEqual(merged['world_sha256'], sha256((automatic / self.world_path).read_bytes()))
        self.assertEqual(merged['merged_files'], result['merged_files'])
        self.assertEqual(self.names(automatic, revision), names)
        self.assertEqual(self.world(automatic, 'asset.audio.inspect', asset=asset), imported)
        manual_doc, automatic_doc = (read_json(root / self.world_path) for root in (candidate, automatic))
        self.assertEqual(automatic_doc.pop('receipts'), read_json(self.source / self.world_path)['receipts'])
        manual_doc.pop('receipts')
        self.assertEqual(automatic_doc, manual_doc)
        self.assertEqual(tree(gate), gate_bytes)
        self.assertEqual(tree(light), light_bytes)
        self.assertEqual(tree(self.live), self.original)
        self.assertEqual(tree(self.base), self.base_bytes)

    def test_pinned_scenes_compose_disjoint_fields_and_retire_identities(self):
        a, b = (self.root / name for name in ('Draft A', 'Draft B'))
        for root in (a, b):
            self.draft(root)
        label = dict(parent=None, name='Gate status', kind='label', text='Ready', action=None,
                     visible=True, enabled=True)
        self.edit(a, self.revision, [rename(GATE, 'Open gate'),
                  {'op': 'ui.element.set', 'id': CUE, 'element': label}])
        material = {'primitive': 'box', 'visible': True, 'albedo': [.4, .2, .1]}
        self.edit(b, self.revision, [{'op': 'component.set', 'id': GATE, 'type': 'MeshRenderer', 'value': material},
                  {'op': 'entity.delete', 'id': LIGHT, 'recursive': False}])
        picks = [self.pick_scene(a), self.pick_scene(b)]
        original = {root: tree(root) for root in (a, b, self.live, self.base)}
        digests = []
        for i, order in enumerate((picks, list(reversed(picks)))):
            candidate = self.root / f'Merged {i}'
            result = self.integrate(candidate, {'base_sha256': self.base_sha256, 'drafts': order})
            digests.append(result['world_sha256'])
            doc = read_json(candidate / self.world_path)
            self.assertEqual(doc['version'], 4)
            self.assertEqual(doc['ui'][CUE], label)
            self.assertEqual(doc['entities'][GATE]['components']['MeshRenderer'], material)
            self.assertEqual(doc['entities'][GATE]['name'], 'Open gate')
            self.assertNotIn(LIGHT, doc['entities'])
            self.assertIn(LIGHT, doc['retired_ids'])
        self.assertEqual(*digests)
        for root, baseline in original.items():
            self.assertEqual(tree(root), baseline)

    def test_pinned_scenes_reject_conflicts_in_both_orders_and_stale_hashes(self):
        a, b = (self.root / name for name in ('Draft A', 'Draft B'))
        for root in (a, b):
            self.draft(root)
        candidate = self.root / 'Conflicted candidate'
        def request(*selections):
            return {'base_sha256': self.base_sha256, 'drafts': list(selections)}

        self.integrate(candidate, request(self.pick_scene(a)), error='changed scenes')
        self.edit(a, self.revision, [rename(GATE, 'Gate A')])
        self.edit(b, self.revision, [rename(GATE, 'Gate B')])
        a_pick, b_pick = self.pick_scene(a), self.pick_scene(b)
        for order in ((a_pick, b_pick), (b_pick, a_pick)):
            self.integrate(candidate, request(*order), error=f'/entities/{GATE}/name')
        transaction = self.transaction(self.revision, [rename(GATE, 'Lead gate')])
        self.integrate(candidate, {**request(a_pick), 'transaction': transaction}, error='explicit world transaction')
        revision = self.revision + 1
        self.edit(b, revision, [{'op': 'entity.delete', 'id': GATE, 'recursive': False}])
        self.integrate(candidate, request(a_pick, b_pick), error='world hash mismatch')
        for order in ((a_pick, self.pick_scene(b)), (self.pick_scene(b), a_pick)):
            self.integrate(candidate, request(*order), error=f'/entities/{GATE}')
        self.assertEqual(tree(self.live), self.original)
        self.assertEqual(tree(self.base), self.base_bytes)

    def test_surface_mark_profiles_merge_by_id_with_frozen_mask_dependencies(self):
        left, right = (self.root / name for name in ('Left mask draft', 'Right mask draft'))
        for draft in (left, right):
            self.draft(draft)

        def import_mask(draft, red):
            (draft / 'Mask.png').write_bytes(png(1, 1, bytes((red, 0, 0, 255))))
            digest = self.world(draft, 'asset.image.import', source='Mask.png', color_space='linear')['asset']
            relative = f'{self.world_path}.assets/{digest}.pimage'
            self.assertEqual(sha256((draft / relative).read_bytes()), digest)
            return digest, relative

        def bind(profile, mask):
            return {'op': 'surface_mark.profile.set', 'id': profile, 'mask': mask}

        left_mask, left_file = import_mask(left, 64)
        right_mask, right_file = import_mask(right, 192)
        self.assertNotEqual(left_mask, right_mask)
        self.edit(left, self.revision, [bind(GATE, left_mask)])
        self.edit(right, self.revision, [bind(LIGHT, right_mask)])
        left_pick, right_pick = self.pick_scene(left), self.pick_scene(right)
        incomplete = {'base_sha256': self.base_sha256, 'drafts': [self.pick(left, left_file)],
                      'transaction': self.transaction(self.revision, [bind(GATE, left_mask), bind(LIGHT, right_mask)])}
        self.integrate(self.root / 'Missing mask candidate', incomplete, error='surface mark profile')

        profiles = {GATE: {'mask': left_mask}, LIGHT: {'mask': right_mask}}
        world_digests = []
        for index, picks in enumerate(([left_pick, right_pick], [right_pick, left_pick])):
            candidate = self.root / f'Profile candidate {index}'
            result = self.integrate(candidate, {'base_sha256': self.base_sha256, 'drafts': picks})
            world_digests.append(result['world_sha256'])
            self.assertEqual({record['path']: record['sha256'] for record in result['merged_files']},
                             {left_file: left_mask, right_file: right_mask})
            self.assertEqual(self.world(candidate, 'world.inspect')['surface_mark_profiles'], profiles)
            dependencies = self.world(candidate, 'world.dependencies')
            self.assertTrue(dependencies['needs_surface_mark_masks'])
            self.assertEqual({record['sha256'] for record in dependencies['assets']},
                             {self.base_asset, left_mask, right_mask})
        self.assertEqual(*world_digests)

        self.edit(right, self.revision + 1, [bind(GATE, right_mask)])
        right_pick = self.pick_scene(right)
        for picks in ([left_pick, right_pick], [right_pick, left_pick]):
            self.integrate(self.root / 'Conflicted profile candidate',
                           {'base_sha256': self.base_sha256, 'drafts': picks},
                           error=f'/surface_mark_profiles/{GATE}')
        self.assertEqual(tree(self.live), self.original)
        self.assertEqual(tree(self.base), self.base_bytes)

    def test_pinned_scenes_validate_the_assembled_hierarchy(self):
        a, b = (self.root / name for name in ('Draft A', 'Draft B'))
        for root in (a, b):
            self.draft(root)
        self.edit(a, self.revision, [{'op': 'entity.reparent', 'id': GATE, 'parent': LIGHT, 'mode': 'keep_local'}])
        self.edit(b, self.revision, [{'op': 'entity.delete', 'id': LIGHT, 'recursive': False}])
        self.integrate(self.root / 'Invalid candidate', {'base_sha256': self.base_sha256,
                      'drafts': [self.pick_scene(a), self.pick_scene(b)]}, error='parent')

    def test_draft_requires_verified_source_and_new_nonoverlapping_output(self):
        target = self.root / 'Draft'
        self.draft(target, base_sha256='0' * 64, error='base source checkpoint changed')
        self.draft(self.base / 'Nested draft', error='overlap|outside')
        target.mkdir()
        (target / 'Keep.txt').write_bytes(b'Existing destination\n')
        self.draft(target, error='exist')
        (self.source / 'Design.txt').write_bytes(b'Corrupted checkpoint file\n')
        self.draft(self.root / 'Draft from corrupt source', error='Design\\.txt')
        self.assertEqual(tree(self.live), self.original)

    def test_draft_changes_review_and_pin_current_source(self):
        worker = self.root / 'Source draft'
        fork = self.draft(worker)
        before = tree(self.root)
        empty = self.draft_changes(worker)
        self.assertEqual(empty, {'manifest': empty['manifest'], 'project_id': fork['project_id'],
                                 'base_project_id': self.spec['project_id'], 'base_sha256': self.base_sha256,
                                 'base_revision': self.revision, 'world_sha256': self.live_summary['world_sha256'],
                                 'files': []})
        self.assert_path(empty['manifest'], worker / MANIFEST)
        self.assertEqual(tree(self.root), before)

        added, replaced = 'gameplay-src/Logic/Score.cs', 'gameplay-src/Game.cs'
        base_game = (self.source / replaced).read_bytes()
        score_code = b'internal static class Score { internal const int Goal = 3; }\n'
        game_code = base_game + b'\n// Reviewed worker edit.\n'
        (worker / added).parent.mkdir()
        (worker / added).write_bytes(score_code)
        (worker / replaced).write_bytes(game_code)
        (worker / 'Outside.cs').write_bytes(b'// Outside the gameplay source directory.\n')
        (worker / 'gameplay-src/Design.txt').write_bytes(b'Not C# source.\n')
        (worker / 'gameplay-src/obj').mkdir()
        (worker / 'gameplay-src/obj/project.assets.json').write_bytes(b'{}\n')
        (worker / 'gameplay-src/obj/Generated.cs').write_bytes(b'// Generated compiler output.\n')
        self.edit(worker, self.revision, [rename(GATE, 'Reviewed gate')])
        after_worker = tree(worker)
        review = self.draft_changes(worker)
        self.assertEqual(review['world_sha256'], sha256(after_worker[self.world_path]))
        self.assertNotEqual(review['world_sha256'], empty['world_sha256'])
        self.assertEqual(review['files'], [
            {'path': replaced, 'change': 'replaced', 'base_sha256': sha256(base_game), 'sha256': sha256(game_code)},
            {'path': added, 'change': 'added', 'base_sha256': None, 'sha256': sha256(score_code)},
        ])
        self.assertEqual(tree(worker), after_worker)

        request = {'base_sha256': self.base_sha256, 'drafts': [
            {'manifest': review['manifest'], 'world_sha256': review['world_sha256'],
             'files': [{'path': item['path'], 'sha256': item['sha256']} for item in review['files']]},
        ]}
        candidate = self.root / 'Reviewed candidate'
        (worker / added).write_bytes(score_code + b'// Edited after review.\n')
        self.integrate(candidate, request, error='draft file hash mismatch')
        (worker / added).write_bytes(score_code)
        result = self.integrate(candidate, request)
        self.assertEqual((candidate / added).read_bytes(), score_code)
        self.assertEqual((candidate / replaced).read_bytes(), game_code)
        self.assertFalse((candidate / 'Outside.cs').exists())
        self.assertEqual(self.names(candidate, result['revision'])[GATE], 'Reviewed gate')

        (worker / replaced).unlink()
        deletion = self.draft_changes(worker)
        self.assertEqual(deletion['files'], [
            {'path': replaced, 'change': 'deleted', 'base_sha256': sha256(base_game), 'sha256': None},
            review['files'][1],
        ])
        self.assertEqual(tree(self.live), self.original)
        self.assertEqual(tree(self.base), self.base_bytes)

    def test_draft_changes_reject_invalid_lineage_and_linked_inputs(self):
        worker = self.root / 'Source draft'
        self.draft(worker)
        self.draft_changes(worker, base_sha256='0' * 64, error='base source checkpoint changed')
        self.draft_changes(self.live, error=r'draft\.json')
        self.reject('absolute draft manifest', 'project', 'draft-changes', native(self.checkpoint),
                    '--draft', MANIFEST, '--expected-base-sha256', self.base_sha256)
        marker = worker / DRAFT_MARKER
        original = marker.read_bytes()
        marker.write_text(json.dumps({**json.loads(original), 'base_revision': self.revision + 1}), encoding='utf-8')
        self.draft_changes(worker, error='lineage')
        marker.write_bytes(original)
        world = worker / self.world_path
        world_bytes = world.read_bytes()
        world.write_text(json.dumps({**json.loads(world_bytes), 'world_id': uuid.uuid4().hex}), encoding='utf-8')
        self.draft_changes(worker, error='world identity')
        world.write_bytes(world_bytes)

        source = worker / 'gameplay-src/Game.cs'
        alias = worker / 'gameplay-src/Alias.cs'
        os.link(source, alias)
        self.draft_changes(worker, error='hard link')
        alias.unlink()
        if not ARGS.windows_interop and os.name != 'nt':
            alias.symlink_to(source)
            self.draft_changes(worker, error='symlink|symbolic link')
            alias.unlink()
        lower_case = source.with_name('game.cs')
        # Move through a third spelling so the rename also works on Windows.
        source.rename(alias)
        alias.rename(lower_case)
        self.draft_changes(worker, error='case-insensitive')
        lower_case.rename(alias)
        alias.rename(source)

        nested = self.base / 'Nested draft'
        worker.rename(nested)
        self.draft_changes(nested, error='overlap')

    def test_integration_rejects_unreviewed_or_unsafe_inputs(self):
        first = self.root / 'First draft'
        second = self.root / 'Second draft'
        self.draft(first)
        self.draft(second)
        for worker, text in ((first, b'First design\n'), (second, b'Second design\n')):
            (worker / 'Design.txt').write_bytes(text)
        request = {'base_sha256': self.base_sha256, 'drafts': [self.pick(first, 'Design.txt')]}
        target = self.root / 'Candidate'
        self.integrate(target, {**request, 'base_sha256': '0' * 64}, error='base source checkpoint changed')
        self.integrate(target, {**request, 'drafts': [*request['drafts'], self.pick(second, 'Design.txt')]},
                       error='divergent draft files')
        stale = self.transaction(self.revision + 1, [rename(GATE, 'Stale gate')])
        self.integrate(target, {**request, 'transaction': stale}, error='base revision')

        (first / 'Design.txt').write_bytes(b'Changed after review\n')
        self.integrate(target, request, error='draft file hash mismatch')
        request['drafts'] = [self.pick(first, 'Design.txt')]
        self.integrate(first / 'Nested candidate', request, error='overlap')
        self.integrate(self.base / 'Nested candidate', request, error='overlap|outside')
        target.mkdir()
        (target / 'Keep.txt').write_bytes(b'Existing candidate\n')
        self.integrate(target, request, error='exist')

        new_target = self.root / 'New candidate'
        self.integrate(new_target, {**request, 'unknown': True}, error='unknown')
        duplicate_key = json.dumps(request).replace('{', '{"base_sha256":null,', 1)
        self.integrate(new_target, duplicate_key, error='duplicate')
        preview = {**self.transaction(self.revision, [rename(GATE, 'Preview gate')]), 'preview': False}
        self.integrate(new_target, {**request, 'transaction': preview}, error='preview')
        self.integrate(new_target, {**request, 'drafts': [self.pick(first, self.world_path)]},
                       error='world|authoritative')
        marker = first / DRAFT_MARKER
        marker_bytes = marker.read_bytes()
        lineage = json.loads(marker_bytes)
        marker.write_text(json.dumps({**lineage, 'base_sha256': '0' * 64}), encoding='utf-8')
        self.integrate(new_target, request, error='lineage')
        marker.write_bytes(marker_bytes)
        self.assertEqual(tree(self.live), self.original)
        self.assertEqual(tree(self.base), self.base_bytes)

        (self.source / 'Design.txt').write_bytes(b'Corrupted checkpoint file\n')
        self.integrate(new_target, request, error='Design\\.txt')


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--scratch-root', type=Path, help='Optional new directory for retaining fixture files.')
    parser.add_argument('--evidence', type=Path, help='Optional JSON transcript path.')
    ARGS = parser.parse_args()
    if ARGS.scratch_root:
        ARGS.scratch_root.mkdir()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProjectDraftContract))
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True, exist_ok=True)
        evidence = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
                    'binary_sha256': sha256(ARGS.binary.read_bytes()),
                    'test_sha256': sha256(Path(__file__).read_bytes()), 'calls': CALLS}
        ARGS.evidence.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
