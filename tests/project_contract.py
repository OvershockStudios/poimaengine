#!/usr/bin/env python3
"""Portable project manifests, immutable inspection and optional bundle export."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from base64 import b64decode
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
p.add_argument('--windows-interop', action='store_true')
p.add_argument('--runtime', type=Path, help='Optional CMake-installed native runtime root.')
p.add_argument('--evidence', type=Path)
args = p.parse_args()
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT/'build/project-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []

def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

def tree(path):
    return {str(p.relative_to(path)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(path.rglob('*')) if p.is_file()}

class ProjectContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.tmp.name)
        self.project = self.root/'Portable project'
        self.created = self.run_cli('project', 'create', native(self.project), '--name', 'Portable room')
        self.manifest = self.project/'project.poimaproject'
        self.document = json.loads(self.manifest.read_text(encoding='utf-8'))

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv, success=True):
        command = [str(args.binary.resolve()), *argv]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        try:
            envelope = json.loads(result.stdout)
        except ValueError:
            self.fail(f'Expected structured reply: {result.returncode} {result.stdout} {result.stderr}')
        CALLS.append({'command': command, 'exit_code': result.returncode, 'reply': envelope, 'stderr': result.stderr})
        self.assertEqual(result.returncode == 0, success, envelope)
        self.assertEqual(envelope['status'], 'ok' if success else 'error', envelope)
        if not success:
            self.assertTrue(envelope['diagnostics'], envelope)
        return envelope.get('result')

    def write(self, doc):
        self.manifest.write_text(json.dumps(doc, indent=2)+'\n', encoding='utf-8')

    def inspect(self, success=True):
        before = tree(self.project)
        result = self.run_cli('project', 'inspect', native(self.manifest), success=success)
        self.assertEqual(tree(self.project), before, 'Project inspection wrote or altered project files.')
        return result

    def configure(self, patch, expected, *, success=True):
        payload = patch if isinstance(patch, str) else json.dumps(patch)
        return self.run_cli('project', 'configure', native(self.manifest), '--patch', payload,
                            '--expected-manifest-sha256', expected, success=success)

    def world_rpc(self, method, **params):
        world = self.project/self.document['entry']['world']
        request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}
        result = subprocess.run([str(args.binary.resolve()), 'world', native(world)], input=json.dumps(request)+'\n',
            capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        reply = json.loads(result.stdout)
        self.assertIn('result', reply, reply)
        return reply['result']

    def test_starter_manifest_and_no_write_inspection(self):
        doc = self.document
        self.assertEqual(self.created['manifest'].replace('\\', '/'), native(self.manifest).replace('\\', '/'))
        self.assertFalse((self.project/'project.json').exists(), 'Starter wrote a second manifest.')
        self.assertIn(self.created['publication'], ('atomic_directory', 'manifest_last'))
        self.assertFalse((self.project/'.poima-incomplete').exists(), 'Successful creation retained an incomplete marker.')
        agent_guide = (self.project/'AGENTS.md').read_text(encoding='utf-8')
        self.assertIn('agree on a game brief before changing', agent_guide)
        self.assertIn('Use the answers to decide the next questions.', agent_guide)
        self.assertEqual((self.project/'CLAUDE.md').read_text(encoding='utf-8'), '@AGENTS.md\n')
        self.assertEqual((doc['format'], doc['version'], doc['name']), ('poima.project', 1, 'Portable room'))
        self.assertRegex(doc['project_id'], '^[0-9a-f]{32}$')
        self.assertFalse(Path(doc['entry']['world']).is_absolute())
        world = json.loads((self.project/doc['entry']['world']).read_text(encoding='utf-8'))
        self.assertIn('CharacterController', world['entities'][doc['entry']['controller']]['components'])
        self.assertIn('Camera', world['entities'][doc['entry']['camera']]['components'])
        environments = [e['components']['LightingEnvironment'] for e in world['entities'].values()
                        if 'LightingEnvironment' in e['components']]
        self.assertEqual(len(environments), 1)
        self.assertEqual(environments[0]['shadow_resolution'],1024)
        self.assertEqual(environments[0]['shadow_filter'],'pcf3_fractional')
        sky = environments[0]['sky']
        self.assertTrue(sky['enabled'])
        sun = world['entities'][sky['sun']]['components']['Light']
        self.assertEqual(sun['kind'], 'directional')
        self.assertTrue(sun['enabled'])
        self.assertEqual(sun['shadow'],{'enabled':True,'distance':80,'bias':.0001,'normal_bias':.005})
        inspected = self.inspect()
        self.assertIs(inspected['startup_splash'], True)
        self.assertNotIn('startup_render', doc)
        self.assertNotIn('startup_render', inspected)
        self.assertEqual(inspected['revision'], world['revision'])
        self.assertEqual(inspected['project_id'], doc['project_id'])
        self.assertFalse(list(self.project.rglob('*.lock')), 'Starter/inspection unexpectedly created a lock sidecar.')

    def test_opt_in_gameplay_sources_are_portable_and_unattached(self):
        self.assertFalse((self.project/'gameplay-src').exists())
        target = self.root/'Café game with source'
        self.run_cli('project', 'create', native(target), '--name', 'Café game', '--gameplay')
        before = tree(target)
        source = target/'gameplay-src'
        self.assertEqual(sorted(item.name for item in source.iterdir()), ['Game.cs','Game.csproj'])
        self.assertIn('$(PoimaAuthoringRoot)/build/Poima.Gameplay.props', (source/'Game.csproj').read_text())
        self.assertIn('namespace ProjectGameplay;', (source/'Game.cs').read_text())
        self.assertIn('ProjectGameplay.Game', (target/'AGENTS.md').read_text())
        result = self.run_cli('project', 'inspect', native(target/'project.poimaproject'))
        self.assertNotIn('gameplay', result)
        self.assertEqual(result['revision'], 1)
        self.run_cli('project', 'create', native(target), '--name', 'Replacement', '--gameplay', success=False)
        self.assertEqual(tree(target), before)

    def test_manifest_name_compatibility(self):
        original = self.inspect()
        for name in ('project.json', 'Room with spaces.poimaproject'):
            alternate = self.project/name
            self.manifest.rename(alternate)
            self.manifest = alternate
            restored = self.inspect()
            self.assertNotEqual(original['manifest'], restored['manifest'])
            self.assertEqual({k: v for k, v in original.items() if k != 'manifest'},
                             {k: v for k, v in restored.items() if k != 'manifest'})

    def test_agent_guides_are_editable_and_optional(self):
        guide = self.project/'AGENTS.md'
        guide.write_text('# Game decisions\n\nThe brief is approved.\n', encoding='utf-8')
        self.inspect()
        guide.unlink()
        (self.project/'CLAUDE.md').unlink()
        self.inspect()

    def test_incomplete_project_is_rejected_without_repair(self):
        marker = self.project/'.poima-incomplete'
        marker.write_text('Publication is incomplete.\n', encoding='utf-8')
        self.inspect(False)
        self.assertEqual(marker.read_text(encoding='utf-8'), 'Publication is incomplete.\n')
        marker.unlink()
        self.inspect()

    def test_existing_create_is_never_overwritten(self):
        before = tree(self.project)
        self.run_cli('project', 'create', native(self.project), '--name', 'Overwrite attempt', success=False)
        self.assertEqual(tree(self.project), before)
        existing = self.root/'existing-empty'
        existing.mkdir()
        self.run_cli('project', 'create', native(existing), '--name', 'Existing', success=False)
        self.assertEqual(list(existing.iterdir()), [])

    def test_duplicate_unknown_and_wrong_type_manifest_fields(self):
        raw = self.manifest.read_text(encoding='utf-8')
        self.manifest.write_text(raw.replace('{', '{"version":1,', 1), encoding='utf-8')
        self.inspect(False)
        for field, value in [('version', 2), ('name', 19), ('project_id', 'BAD'), ('audio', 'true'),
                             ('startup_splash', 'false'),
                             ('unknown', True), ('gameplay', {'assembly': 'unimplemented.dll'})]:
            with self.subTest(field=field):
                doc = copy.deepcopy(self.document)
                doc[field] = value
                self.write(doc)
                self.inspect(False)

    def test_world_path_must_stay_portable_and_inside_project(self):
        source = self.project/self.document['entry']['world']
        outside = self.root/'outside.json'
        shutil.copyfile(source, outside)
        for value in ['../outside.json', native(outside), 'C:/outside.json', 'world\x00.json']:
            with self.subTest(path=value):
                doc = copy.deepcopy(self.document)
                doc['entry']['world'] = value
                self.write(doc)
                self.inspect(False)

    def test_missing_world_and_invalid_entry_entities(self):
        for key, value in [('world', 'absent.json'), ('camera', 'f'*32), ('controller', self.document['entry']['camera'])]:
            with self.subTest(field=key):
                doc = copy.deepcopy(self.document)
                doc['entry'][key] = value
                self.write(doc)
                self.inspect(False)

    def test_missing_cooked_mesh_rejected_without_repair(self):
        world = self.project/self.document['entry']['world']
        doc = json.loads(world.read_text(encoding='utf-8'))
        entity = uuid.uuid4().hex
        self.world_rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=doc['revision'], ops=[
            {'op': 'entity.create', 'id': entity, 'name': 'Missing package'},
            {'op': 'component.set', 'id': entity, 'type': 'StaticMesh',
             'value': {'asset': 'f'*64, 'primitive': 0, 'visible': True}}])
        self.inspect(False)

    def test_valid_input_profile_readonly_and_malformed_profile_rejected(self):
        profile = self.project/'controls.poima-input.json'
        value = {'bindings': {'forward': ['key.up'], 'backward': ['key.down'], 'left': ['key.left'],
            'right': ['key.right'], 'jump': ['key.space'], 'use': ['key.e']},
            'sensitivity_x': .2, 'sensitivity_y': .15, 'invert_x': False, 'invert_y': False}
        self.world_rpc('input.transact', path=native(profile), request_id=uuid.uuid4().hex, expected_revision=0, profile=value)
        # Remove test-created writer locks before exercising immutable readers.
        for lock in self.project.rglob('*.lock'):
            lock.unlink()
        doc = copy.deepcopy(self.document)
        doc['input_profile'] = profile.name
        self.write(doc)
        self.inspect()
        self.assertFalse(list(self.project.rglob('*.lock')))
        profile.write_text('{"format":"poima.input.v99"}', encoding='utf-8')
        self.inspect(False)

    def test_missing_runtime_descriptor_rejects_export(self):
        runtime = self.root/'not-runtime'
        runtime.mkdir()
        destination = self.root/'bad-export'
        before = tree(self.project)
        self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(runtime), success=False)
        self.assertEqual(tree(self.project), before)
        self.assertFalse(destination.exists(), 'Failed preflight published a partial game bundle.')

    def metadata_runtime(self):
        # Metadata-only runtime, never executed: this checks the no-gameplay
        # compatibility branch independently of a developer's installed build.
        runtime = self.root/'Legacy runtime'
        (runtime/'bin').mkdir(parents=True)
        notices = runtime/'share/poima'; notices.mkdir(parents=True)
        for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
            (notices/name).write_text('Original metadata-only test fixture.\n')
        target = 'Windows' if args.windows_interop or os.name == 'nt' else 'Linux'
        executable = 'bin/poima.exe' if target == 'Windows' else 'bin/poima'
        (runtime/executable).write_bytes(b'Metadata-only runtime fixture; never executed.\n')
        (runtime/executable).chmod(0o755)
        spec = dict(format='poima.runtime', version=1, engine_version=self.run_cli('version')['version'],
                    target_os=target, target_arch='x86_64', executable=executable,
                    features=dict(simulation=True, renderer=True, audio=False, managed=False, editor=False))
        return runtime, spec

    def test_legacy_runtime_descriptor_remains_usable_without_gameplay(self):
        runtime, spec = self.metadata_runtime()
        source = tree(self.project)
        for abi in (None, 3):
            if abi is not None: spec['gameplay_services_version'] = abi
            (runtime/'runtime.json').write_text(json.dumps(spec))
            original = tree(runtime)
            bundle = self.root/('No gameplay ABI '+str(abi))
            self.run_cli('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(bundle))
            manifest = bundle/'game.json'
            self.assertNotIn('startup_render', json.loads(manifest.read_bytes()))
            self.assertNotIn('startup_render', self.run_cli('game', 'inspect', native(manifest)))
            self.assertEqual(tree(runtime), original)
        self.assertEqual(tree(self.project), source)

    def test_exact_physics_checkpoint_requires_explicit_runtime_capability(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime))
        legacy = self.root/'Legacy checkpoint bundle'
        self.assertNotIn('needs_physics_checkpoint', self.inspect())
        self.run_cli(*build, '--output', native(legacy))
        self.assertNotIn('needs_physics_checkpoint', self.run_cli('game', 'inspect', native(legacy/'game.json')))

        world = self.project/self.document['entry']['world']
        self.world_rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=json.loads(world.read_bytes())['revision'],
                       ops=[dict(op='physics_checkpoint.set', definition={'version': 1})])
        self.assertIs(self.inspect()['needs_physics_checkpoint'], True)
        self.assertIs(self.world_rpc('world.dependencies')['needs_physics_checkpoint'], True)
        source = tree(self.project)
        requirement = 'Exact physics checkpoints require a compatible simulation runtime.'
        output = self.root/'Exact checkpoint bundle'
        for flag, expected in ((None, requirement), (False, requirement), ('true', 'Runtime feature flags must be Boolean.')):
            with self.subTest(flag=flag):
                if flag is None:
                    spec['features'].pop('physics_checkpoint', None)
                else:
                    spec['features']['physics_checkpoint'] = flag
                descriptor.write_text(json.dumps(spec), encoding='utf-8')
                self.run_cli(*build, '--output', native(output), success=False)
                self.assertEqual(CALLS[-1]['reply']['diagnostics'], [dict(code='project.failed', message=expected)])
                self.assertFalse(output.exists(), 'Unsupported checkpoint export published a bundle.')

        spec['features']['physics_checkpoint'] = True
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        runtime_source = tree(runtime)
        self.run_cli(*build, '--output', native(output))
        relocated = self.root/'Relocated exact checkpoint bundle'
        output.rename(relocated)
        manifest = relocated/'game.json'
        self.assertIs(self.run_cli('game', 'inspect', native(manifest))['needs_physics_checkpoint'], True)
        self.assertEqual(json.loads(manifest.read_bytes())['version'], 1)
        self.assertEqual(json.loads((relocated/'content/world.json').read_bytes())['physics_checkpoint'], {'version': 1})

        # Update the listed hash and size so the unsupported profile admission
        # runs even when bundle inventory integrity is intact.
        del spec['features']['physics_checkpoint']
        bundled_descriptor = relocated/'runtime/runtime.json'
        bundled_descriptor.write_text(json.dumps(spec), encoding='utf-8')
        bundle_value = json.loads(manifest.read_bytes())
        entry = next(row for row in bundle_value['files'] if row['path'] == 'runtime/runtime.json')
        entry.update(size=bundled_descriptor.stat().st_size, sha256=hashlib.sha256(bundled_descriptor.read_bytes()).hexdigest())
        manifest.write_text(json.dumps(bundle_value), encoding='utf-8')
        rejected_bundle = tree(relocated)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertEqual(CALLS[-1]['reply']['diagnostics'], [dict(code='game.failed', message=requirement)])
        self.assertEqual(tree(relocated), rejected_bundle)
        self.assertEqual(tree(runtime), runtime_source)
        self.assertEqual(tree(self.project), source)

    def test_splash_toggle_is_guarded_and_exported(self):
        initial = self.inspect()
        world = self.project / self.document['entry']['world']
        original_world = world.read_bytes()
        arguments = ('project', 'splash', native(self.manifest))
        self.run_cli(*arguments, '--enabled', 'false', '--expected-manifest-sha256', '0' * 64, success=False)
        self.assertEqual(json.loads(self.manifest.read_bytes()), self.document)
        self.assertFalse(list(self.project.rglob('*.lock')))
        off = self.run_cli(*arguments, '--enabled', 'false', '--expected-manifest-sha256', initial['manifest_sha256'])
        self.assertIs(off['changed'], True)
        self.assertIs(self.inspect()['startup_splash'], False)
        self.assertEqual(json.loads(self.manifest.read_bytes()), {**self.document, 'startup_splash': False})
        self.assertEqual(world.read_bytes(), original_world)
        stable = tree(self.project)
        self.run_cli(*arguments, '--enabled', 'true', '--expected-manifest-sha256', initial['manifest_sha256'], success=False)
        replay = self.run_cli(*arguments, '--enabled', 'false', '--expected-manifest-sha256', off['manifest_sha256'])
        self.assertIs(replay['changed'], False)
        self.assertEqual(tree(self.project), stable)
        runtime, spec = self.metadata_runtime()
        (runtime/'runtime.json').write_text(json.dumps(spec))
        bundle = self.root / 'Splash disabled'
        self.run_cli('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(bundle))
        manifest = bundle / 'game.json'
        self.assertIs(json.loads(manifest.read_bytes())['startup_splash'], False)
        self.assertIs(self.run_cli('game', 'inspect', native(manifest))['startup_splash'], False)
        self.assertEqual(tree(self.project), stable)

    def test_configure_startup_render_digest_and_atomic_splash_update(self):
        initial = self.inspect()['manifest_sha256']
        source = tree(self.project)
        profile = {'reconstruction': 'fsr3_quality', 'lighting_path': 'deferred'}
        patch = {'startup_splash': False, 'startup_render': profile}

        self.run_cli('project', 'configure', native(self.manifest), '--patch', json.dumps(patch), success=False)
        self.configure(patch, '0' * 64, success=False)
        self.configure({'startup_splash': False, 'startup_render': {'samples': 2}}, initial, success=False)
        unchanged = self.configure({'startup_splash': True, 'startup_render': None}, initial)
        self.assertEqual(unchanged, {'startup_splash': True, 'manifest_sha256': initial, 'changed': False})
        self.assertEqual(tree(self.project), source)

        applied = self.configure(patch, initial)
        self.assertIs(applied['changed'], True)
        self.assertIs(applied['startup_splash'], False)
        self.assertEqual(applied['startup_render'], profile)
        payload = self.manifest.read_bytes()
        self.assertEqual(applied['manifest_sha256'], hashlib.sha256(payload).hexdigest())
        self.assertNotEqual(applied['manifest_sha256'], initial)
        self.assertEqual(json.loads(payload), {**self.document, **patch})
        self.assertEqual(self.inspect()['startup_render'], profile)
        stable = tree(self.project)
        for path, digest in source.items():
            if path != self.manifest.name:
                self.assertEqual(stable[path], digest)

        self.configure(patch, initial, success=False)
        reordered = {'startup_render': {'lighting_path': 'deferred', 'reconstruction': 'fsr3_quality'},
                     'startup_splash': False}
        replay = self.configure(reordered, applied['manifest_sha256'])
        self.assertEqual(replay, {**applied, 'changed': False})
        self.assertEqual(tree(self.project), stable)

        replaced = self.configure({'startup_render': {'samples': 1}}, applied['manifest_sha256'])
        self.assertIs(replaced['changed'], True)
        self.assertEqual(self.inspect()['startup_render'], {'samples': 1})
        empty = self.configure({'startup_render': {}}, replaced['manifest_sha256'])
        self.assertIs(empty['changed'], True)
        self.assertEqual(self.inspect()['startup_render'], {})
        removed = self.configure({'startup_render': None}, empty['manifest_sha256'])
        self.assertIs(removed['changed'], True)
        self.assertIs(removed['startup_splash'], False)
        self.assertNotIn('startup_render', removed)
        self.assertNotIn('startup_render', self.inspect())
        self.assertEqual(json.loads(self.manifest.read_bytes()), {**self.document, 'startup_splash': False})
        stable = tree(self.project)
        self.assertEqual(self.configure({'startup_render': None}, removed['manifest_sha256']),
                         {**removed, 'changed': False})
        self.assertEqual(tree(self.project), stable)

    def test_configure_rejects_invalid_render_options_without_partial_changes(self):
        applied = self.configure({'startup_render': {'lighting_path': 'forward', 'samples': 4}},
                                 self.inspect()['manifest_sha256'])
        stable = tree(self.project)
        cases = [
            'null',
            '{}',
            '{"startup_render":{},"startup_render":null}',
            '{"startup_render":{"samples":1,"samples":4}}',
            {'startup_splash': 'false', 'startup_render': {}},
            {'startup_splash': False, 'name': 'Unknown patch member'},
        ]
        invalid_profiles = [
            False, [], 'deferred',
            {'samples': True}, {'samples': 1.0}, {'samples': 2}, {'samples': None},
            {'lighting_path': True}, {'lighting_path': 'clustered'},
            {'post_aa': 1}, {'post_aa': 'taa'},
            {'reconstruction': False}, {'reconstruction': 'dlss'},
            {'viewmodel_samples': 4},
            {'lighting_path': 'deferred', 'samples': 4},
            {'post_aa': 'fxaa', 'samples': 4},
            {'reconstruction': 'fsr3_native', 'samples': 4},
            {'reconstruction': 'fsr3_performance', 'post_aa': 'fxaa'},
        ]
        cases.extend({'startup_splash': False, 'startup_render': value} for value in invalid_profiles)
        for patch in cases:
            with self.subTest(patch=patch):
                self.configure(patch, applied['manifest_sha256'], success=False)
                self.assertEqual(tree(self.project), stable)
        self.assertIs(self.inspect()['startup_splash'], True)

        for profile in [None, *invalid_profiles]:
            with self.subTest(manifest_profile=profile):
                self.write({**self.document, 'startup_render': profile})
                self.inspect(False)

    def test_startup_render_can_repair_a_new_sprite_conflict(self):
        applied = self.configure({'startup_render': {'samples': 4}}, self.inspect()['manifest_sha256'])
        image = self.root/'Opaque test pixel.png'
        image.write_bytes(b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII='))
        texture = self.world_rpc('asset.image.import', source=native(image), color_space='srgb')['asset']
        world = self.project/self.document['entry']['world']
        entity = uuid.uuid4().hex
        revision = json.loads(world.read_bytes())['revision']
        emitter = dict(spawn_space='world', simulation_space='world', lifetime_ticks=12,
                       particles_per_second=30, size_m=.25, linear_rgb=[1, 1, 1], sprite={'texture': texture})
        self.world_rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=revision, ops=[
            {'op': 'entity.create', 'id': entity, 'name': 'Textured sprite'},
            {'op': 'component.set', 'id': entity, 'type': 'ParticleEmitter', 'value': emitter},
        ])
        conflicted = self.inspect()
        self.assertEqual(conflicted['startup_render'], {'samples': 4})
        self.assertEqual(conflicted['manifest_sha256'], applied['manifest_sha256'])
        source = tree(self.project)
        runtime, spec = self.metadata_runtime()
        spec['features'].update(scene_particles=True, scene_particle_sprites=True)
        (runtime/'runtime.json').write_text(json.dumps(spec), encoding='utf-8')
        output = self.root/'Sprite bundle'
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(output))
        self.run_cli(*build, success=False)
        self.assertIn('sprites require samples=1', CALLS[-1]['reply']['diagnostics'][0]['message'])
        self.assertFalse(output.exists())
        self.configure({'startup_splash': False}, applied['manifest_sha256'], success=False)
        self.assertEqual(tree(self.project), source)
        repaired = self.configure({'startup_render': {'samples': 1}}, applied['manifest_sha256'])
        self.assertIs(repaired['changed'], True)
        current = self.inspect()
        self.assertEqual(current['startup_render'], {'samples': 1})
        self.assertEqual(current['revision'], revision + 1)
        self.assertIs(current['startup_splash'], True)
        self.assertEqual({name: digest for name, digest in tree(self.project).items() if name != self.manifest.name},
                         {name: digest for name, digest in source.items() if name != self.manifest.name})
        self.run_cli(*build)
        game = output/'game.json'
        self.assertEqual(self.run_cli('game', 'inspect', native(game))['startup_render'], {'samples': 1})
        spec = json.loads(game.read_bytes())
        spec['startup_render']['samples'] = 4
        game.write_text(json.dumps(spec), encoding='utf-8')
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertIn('sprites require samples=1', CALLS[-1]['reply']['diagnostics'][0]['message'])

    def test_startup_render_checkpoint_and_draft_preserve_sparse_profile(self):
        profile = {'post_aa': 'fxaa', 'reconstruction': 'none'}
        self.configure({'startup_render': profile}, self.inspect()['manifest_sha256'])
        lock = '.poima-gameplay-adoptions.lock'
        self.assertEqual((self.project/lock).read_bytes(), b'')
        stage = '.poima-splash-' + uuid.uuid4().hex
        (self.project/stage).mkdir()
        (self.project/stage/'manifest.pending').write_bytes(b'Unpublished manifest bytes.')
        nested = self.project/'ordinary' / stage
        nested.mkdir(parents=True)
        (nested/lock).write_bytes(b'Ordinary project file with a matching leaf name.')
        source = tree(self.project)
        base = self.root/'Render profile checkpoint'
        saved = self.run_cli('project', 'checkpoint', native(self.manifest), '--output', native(base))
        expected_source = {path: value for path, value in source.items()
                           if path != lock and not path.startswith(stage+'/')}
        self.assertEqual(tree(base/'source'), expected_source)
        self.assertFalse((base/'source'/stage).exists())
        draft = self.root/'Render profile draft'
        draft_manifest = draft/self.manifest.name
        pin = ('--expected-base-sha256', saved['source_sha256'])
        self.run_cli('project', 'draft', native(base/'checkpoint.json'), '--output', native(draft), *pin)
        self.assertEqual(self.run_cli('project', 'inspect', native(draft_manifest))['startup_render'], profile)
        baseline = tree(draft)
        review = self.run_cli('project', 'draft-changes', native(base/'checkpoint.json'),
                              '--draft', native(draft_manifest), *pin)
        self.assertEqual(review['files'], [])
        self.assertEqual(tree(draft), baseline)
        self.assertEqual(tree(self.project), source)

    def test_legacy_checkpoint_with_inventoried_writer_lock_remains_readable(self):
        base = self.root/'Legacy lock checkpoint'
        self.run_cli('project', 'checkpoint', native(self.manifest), '--output', native(base))
        checkpoint = base/'checkpoint.json'
        document = json.loads(checkpoint.read_bytes())
        lock = '.poima-gameplay-adoptions.lock'
        # Versions before the exclusion could include this root lock as an ordinary
        # source file. Preserve the shape of such a checkpoint and its valid digest.
        (base/'source'/lock).write_bytes(b'')
        document['files'].append({'path': 'source/'+lock, 'size': 0, 'role': 'source',
                                  'sha256': hashlib.sha256(b'').hexdigest()})
        document['files'].sort(key=lambda row: row['path'])
        checkpoint.write_text(json.dumps(document), encoding='utf-8')
        pin = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        before = tree(base)
        draft = self.root/'Draft from legacy lock checkpoint'
        self.run_cli('project', 'draft', native(checkpoint), '--output', native(draft),
                     '--expected-base-sha256', pin)
        self.assertEqual(tree(base), before)
        self.assertNotIn('startup_render', self.run_cli('project', 'inspect', native(draft/self.manifest.name)))

    def test_export_startup_render_profile_requires_runtime_capabilities(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime))
        output = self.root/'Authored render bundle'
        profile = {'reconstruction': 'fsr3_balanced', 'lighting_path': 'deferred'}
        self.configure({'startup_render': profile}, self.inspect()['manifest_sha256'])
        source = tree(self.project)
        for renderer, fsr, expected in [(False, True, 'render'), (True, None, 'fsr'),
                                        (True, False, 'fsr'), (True, 'true', 'boolean')]:
            with self.subTest(renderer=renderer, fsr=fsr):
                spec['features']['renderer'] = renderer
                if fsr is None:
                    spec['features'].pop('fsr3_upscaler')
                else:
                    spec['features']['fsr3_upscaler'] = fsr
                descriptor.write_text(json.dumps(spec), encoding='utf-8')
                self.run_cli(*build, '--output', native(output), success=False)
                self.assertIn(expected, CALLS[-1]['reply']['diagnostics'][0]['message'].lower())
                self.assertFalse(output.exists())
                self.assertEqual(tree(self.project), source)

        spec['features']['fsr3_upscaler'] = True
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        runtime_source = tree(runtime)
        self.run_cli(*build, '--output', native(output))
        relocated = self.root/'Relocated authored render bundle'
        output.rename(relocated)
        manifest = relocated/'game.json'
        game = json.loads(manifest.read_bytes())
        self.assertEqual(game['version'], 1)
        self.assertEqual(game['startup_render'], profile)
        self.assertEqual(self.run_cli('game', 'inspect', native(manifest))['startup_render'], profile)

        # A coherent inventory without the required feature still fails admission.
        bundled_descriptor = relocated/'runtime/runtime.json'
        del spec['features']['fsr3_upscaler']
        bundled_descriptor.write_text(json.dumps(spec), encoding='utf-8')
        descriptor_bytes = bundled_descriptor.read_bytes()
        row = next(item for item in game['files'] if item['path'] == 'runtime/runtime.json')
        row.update(size=len(descriptor_bytes), sha256=hashlib.sha256(descriptor_bytes).hexdigest())
        manifest.write_text(json.dumps(game), encoding='utf-8')
        unqualified = tree(relocated)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('fsr', CALLS[-1]['reply']['diagnostics'][0]['message'].lower())
        self.assertEqual(tree(relocated), unqualified)
        self.assertEqual(tree(runtime), runtime_source)
        self.assertEqual(tree(self.project), source)

    def test_auto_exposure_requires_runtime_feature_when_authored_in_a_template(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime))
        legacy = self.root/'Legacy light bundle'
        self.run_cli(*build, '--output', native(legacy))
        self.assertNotIn('needs_auto_exposure', self.run_cli('game', 'inspect', native(legacy/'game.json')))

        transform = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
        lighting = dict(ambient=[.1, .1, .1], exposure=.25, auto_exposure={})
        root = uuid.uuid4().hex
        recipe = dict(op='template.set', id=uuid.uuid4().hex, name='Exposure fixture', root=root,
                      entities={root: dict(name='Exposure', parent=None,
                          components=dict(Transform=transform, LightingEnvironment=lighting))})
        initial = self.inspect()
        self.world_rpc('world.transact', base_revision=initial['revision'], request_id=uuid.uuid4().hex,
                       ops=[recipe])
        self.assertIs(self.inspect()['needs_auto_exposure'], True)
        self.assertIs(self.world_rpc('world.dependencies')['needs_auto_exposure'], True)
        self.assertNotIn('auto_exposure', self.world_rpc('world.lighting'))

        output = self.root/'Auto exposure bundle'
        source = tree(self.project)
        for feature in (None, False, 'true'):
            with self.subTest(feature=feature):
                if feature is None:
                    spec['features'].pop('auto_exposure', None)
                else:
                    spec['features']['auto_exposure'] = feature
                descriptor.write_text(json.dumps(spec), encoding='utf-8')
                self.run_cli(*build, '--output', native(output), success=False)
                self.assertIn('Boolean' if isinstance(feature, str) else 'auto exposure',
                              CALLS[-1]['reply']['diagnostics'][0]['message'])
                self.assertFalse(output.exists())
                self.assertEqual(tree(self.project), source)

        spec['features']['auto_exposure'] = True
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        self.run_cli(*build, '--output', native(output))
        manifest = output/'game.json'
        self.assertIs(self.run_cli('game', 'inspect', native(manifest))['needs_auto_exposure'], True)
        game = json.loads(manifest.read_bytes())
        del spec['features']['auto_exposure']
        descriptor_bytes = json.dumps(spec).encode('utf-8')
        (output/'runtime/runtime.json').write_bytes(descriptor_bytes)
        row = next(item for item in game['files'] if item['path'] == 'runtime/runtime.json')
        row.update(size=len(descriptor_bytes), sha256=hashlib.sha256(descriptor_bytes).hexdigest())
        manifest.write_text(json.dumps(game), encoding='utf-8')
        unchanged = tree(output)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('auto exposure', CALLS[-1]['reply']['diagnostics'][0]['message'])
        self.assertEqual(tree(output), unchanged)
        self.assertEqual(tree(self.project), source)

    def test_atmosphere_requires_runtime_feature_when_authored_in_a_template(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime))
        output = self.root/'Atmospheric sky bundle'
        world = json.loads((self.project/self.document['entry']['world']).read_bytes())
        environment, = (row['components']['LightingEnvironment'] for row in world['entities'].values()
                        if 'LightingEnvironment' in row['components'])
        self.assertNotIn('needs_atmosphere_sky', self.inspect())
        self.assertNotIn('needs_atmosphere_sky', self.world_rpc('world.dependencies'))

        transform = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
        environment_id, sun_id = uuid.uuid4().hex, uuid.uuid4().hex
        lighting = dict(ambient=[0, 0, 0], exposure=1, sky=environment['sky'] | dict(sun=sun_id),
                        atmosphere=dict(sea_level_m=0, aerosol_strength=1, radiance_scale=1))
        sun = dict(kind='directional', color=[1, 1, 1], intensity=20, enabled=False)
        recipe = dict(op='template.set', id=uuid.uuid4().hex, name='Atmosphere fixture', root=environment_id,
                      entities={environment_id: dict(name='Environment', parent=None,
                          components=dict(Transform=transform, LightingEnvironment=lighting)),
                          sun_id: dict(name='Sun', parent=environment_id,
                          components=dict(Transform=transform, Light=sun))})
        self.world_rpc('world.transact', base_revision=world['revision'], request_id=uuid.uuid4().hex, ops=[recipe])
        self.assertIs(self.inspect()['needs_atmosphere_sky'], True)
        self.assertIs(self.world_rpc('world.dependencies')['needs_atmosphere_sky'], True)
        self.assertNotIn('needs_aerial_perspective', self.inspect())
        self.assertNotIn('needs_aerial_perspective', self.world_rpc('world.dependencies'))
        self.assertNotIn('atmosphere', self.world_rpc('world.lighting'))

        source = tree(self.project)
        for feature in (None, False, 'true'):
            with self.subTest(feature=feature):
                if feature is None:
                    spec['features'].pop('atmosphere_sky', None)
                else:
                    spec['features']['atmosphere_sky'] = feature
                descriptor.write_text(json.dumps(spec), encoding='utf-8')
                self.run_cli(*build, '--output', native(output), success=False)
                self.assertIn('Boolean' if isinstance(feature, str) else 'Atmospheric sky',
                              CALLS[-1]['reply']['diagnostics'][0]['message'])
                self.assertFalse(output.exists())
                self.assertEqual(tree(self.project), source)

        spec['features']['atmosphere_sky'] = True
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        self.run_cli(*build, '--output', native(output))
        manifest = output/'game.json'
        inspected = self.run_cli('game', 'inspect', native(manifest))
        self.assertIs(inspected['needs_atmosphere_sky'], True)
        self.assertNotIn('needs_aerial_perspective', inspected)
        game = json.loads(manifest.read_bytes())
        del spec['features']['atmosphere_sky']
        descriptor_bytes = json.dumps(spec).encode('utf-8')
        (output/'runtime/runtime.json').write_bytes(descriptor_bytes)
        row = next(item for item in game['files'] if item['path'] == 'runtime/runtime.json')
        row.update(size=len(descriptor_bytes), sha256=hashlib.sha256(descriptor_bytes).hexdigest())
        manifest.write_text(json.dumps(game), encoding='utf-8')
        unchanged = tree(output)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('Atmospheric sky', CALLS[-1]['reply']['diagnostics'][0]['message'])
        self.assertEqual(tree(output), unchanged)
        self.assertEqual(tree(self.project), source)

    def test_aerial_perspective_requires_explicit_export_runtime_feature(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        world = json.loads((self.project/self.document['entry']['world']).read_bytes())
        environment_id, = (key for key, row in world['entities'].items()
                           if 'LightingEnvironment' in row['components'])
        lighting = world['entities'][environment_id]['components']['LightingEnvironment']
        lighting['atmosphere'] = dict(sea_level_m=0, aerosol_strength=1, radiance_scale=1, aerial_perspective_m=4096)
        self.world_rpc('world.transact', base_revision=world['revision'], request_id=uuid.uuid4().hex,
                       ops=[dict(op='component.set', id=environment_id, type='LightingEnvironment', value=lighting)])
        self.assertIs(self.inspect()['needs_aerial_perspective'], True)
        self.assertIs(self.world_rpc('world.dependencies')['needs_aerial_perspective'], True)
        source = tree(self.project)
        output = self.root/'Aerial perspective bundle'
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(output))
        for features, diagnostic in ((dict(atmosphere_sky=True), 'Aerial perspective'),
                                     (dict(atmosphere_sky=True, aerial_perspective=False), 'Aerial perspective'),
                                     (dict(atmosphere_sky=True, aerial_perspective='true'), 'Boolean'),
                                     (dict(aerial_perspective=True), 'requires the atmospheric sky feature')):
            with self.subTest(features=features):
                descriptor.write_text(json.dumps(dict(spec, features=spec['features'] | features)), encoding='utf-8')
                self.run_cli(*build, success=False)
                self.assertIn(diagnostic, CALLS[-1]['reply']['diagnostics'][0]['message'])
                self.assertFalse(output.exists())
                self.assertEqual(tree(self.project), source)

        spec['features'].update(atmosphere_sky=True, aerial_perspective=True)
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        self.run_cli(*build)
        manifest = output/'game.json'
        inspected = self.run_cli('game', 'inspect', native(manifest))
        self.assertIs(inspected['needs_atmosphere_sky'], True)
        self.assertIs(inspected['needs_aerial_perspective'], True)
        game = json.loads(manifest.read_bytes())
        del spec['features']['aerial_perspective']
        descriptor_bytes = json.dumps(spec).encode('utf-8')
        (output/'runtime/runtime.json').write_bytes(descriptor_bytes)
        row = next(item for item in game['files'] if item['path'] == 'runtime/runtime.json')
        row.update(size=len(descriptor_bytes), sha256=hashlib.sha256(descriptor_bytes).hexdigest())
        manifest.write_text(json.dumps(game), encoding='utf-8')
        unchanged = tree(output)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('Aerial perspective', CALLS[-1]['reply']['diagnostics'][0]['message'])
        self.assertEqual(tree(output), unchanged)
        self.assertEqual(tree(self.project), source)

    def test_volumetric_clouds_require_explicit_export_runtime_feature(self):
        runtime, spec = self.metadata_runtime()
        descriptor = runtime/'runtime.json'
        world = json.loads((self.project/self.document['entry']['world']).read_bytes())
        environment_id, = (key for key, row in world['entities'].items()
                           if 'LightingEnvironment' in row['components'])
        lighting = world['entities'][environment_id]['components']['LightingEnvironment']
        clouds = dict(base_height_m=1500, thickness_m=2000, coverage=.5, extinction_per_km=4, wind_m_per_s=[5, 0])
        lighting['atmosphere'] = dict(sea_level_m=0, aerosol_strength=1, radiance_scale=1, clouds=clouds)
        self.world_rpc('world.transact', base_revision=world['revision'], request_id=uuid.uuid4().hex,
                       ops=[dict(op='component.set', id=environment_id, type='LightingEnvironment', value=lighting)])
        feature = 'volumetric_clouds'
        dependency = 'needs_volumetric_clouds'
        self.assertIs(self.inspect()[dependency], True)
        self.assertIs(self.world_rpc('world.dependencies')[dependency], True)
        self.assertNotIn('needs_aerial_perspective', self.inspect())
        source = tree(self.project)
        output = self.root/'Volumetric cloud bundle'
        build = ('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(output))
        for flags, diagnostic in ((dict(atmosphere_sky=True), 'Volumetric clouds'),
                                  (dict(atmosphere_sky=True, volumetric_clouds=False), 'Volumetric clouds'),
                                  (dict(atmosphere_sky=True, volumetric_clouds='true'), 'Boolean'),
                                  (dict(volumetric_clouds=True), 'Runtime volumetric clouds require the atmospheric sky feature.')):
            with self.subTest(flags=flags):
                descriptor.write_text(json.dumps(dict(spec, features=spec['features'] | flags)), encoding='utf-8')
                self.run_cli(*build, success=False)
                self.assertIn(diagnostic, CALLS[-1]['reply']['diagnostics'][0]['message'])
                self.assertFalse(output.exists())
                self.assertEqual(tree(self.project), source)
        spec['features'].update(atmosphere_sky=True, volumetric_clouds=True)
        descriptor.write_text(json.dumps(spec), encoding='utf-8')
        self.run_cli(*build)
        manifest = output/'game.json'
        inspected = self.run_cli('game', 'inspect', native(manifest))
        self.assertIs(inspected[dependency], True)
        self.assertIs(inspected['needs_atmosphere_sky'], True)
        game = json.loads(manifest.read_bytes())
        del spec['features'][feature]
        descriptor_bytes = json.dumps(spec).encode('utf-8')
        (output/'runtime/runtime.json').write_bytes(descriptor_bytes)
        row = next(item for item in game['files'] if item['path'] == 'runtime/runtime.json')
        row.update(size=len(descriptor_bytes), sha256=hashlib.sha256(descriptor_bytes).hexdigest())
        manifest.write_text(json.dumps(game), encoding='utf-8')
        unchanged = tree(output)
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('Volumetric clouds', CALLS[-1]['reply']['diagnostics'][0]['message'])
        self.assertEqual(tree(output), unchanged)
        self.assertEqual(tree(self.project), source)

    @unittest.skipUnless(args.windows_interop or os.name == 'nt', 'Reproduces the Windows staging path limit.')
    def test_windows_export_reads_world_and_profile_in_deep_staging_paths(self):
        runtime, spec = self.metadata_runtime()
        (runtime/'runtime.json').write_text(json.dumps(spec), encoding='utf-8')
        length = len(native(self.root).encode('utf-16-le')) // 2
        before = tree(self.project)
        relative_profile = 'content/default.poima-input.json'
        # The stage adds a 45-character directory to this parent. At the
        # first depth the input profile exceeds MAX_PATH, followed by the
        # shorter world filename at the second depth.
        for parent_length in (189, 205):
            with self.subTest(parent_length=parent_length):
                parent = self.root/('p' * (parent_length - length - 1))
                parent.mkdir(exist_ok=True)
                destination = parent/'game'
                self.run_cli('project', 'build', native(self.manifest), '--runtime', native(runtime),
                             '--output', native(destination))
                bundle = tree(destination)
                self.assertEqual(bundle[relative_profile], before[self.document['input_profile']])
                manifest = native(destination/'game.json')
                inspected = self.run_cli('game', 'inspect', manifest)
                self.assertEqual(inspected['manifest'].replace('\\', '/'), manifest.replace('\\', '/'))
                self.assertEqual(inspected['input_profile'], relative_profile)
                self.assertEqual(tree(destination), bundle, 'Reading the bundle changed its files.')
        self.assertEqual(tree(self.project), before, 'Export changed the source project.')

    def test_authored_ui_requires_explicit_runtime_capability(self):
        runtime = self.root/'UI metadata runtime'
        (runtime/'bin').mkdir(parents=True)
        notices = runtime/'share/poima'; notices.mkdir(parents=True)
        for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
            (notices/name).write_text('Metadata-only test fixture.\n')
        target = 'Windows' if args.windows_interop or os.name == 'nt' else 'Linux'
        executable = 'bin/poima.exe' if target == 'Windows' else 'bin/poima'
        (runtime/executable).write_bytes(b'Metadata-only fixture; never executed.\n')
        (runtime/executable).chmod(0o755)
        spec = dict(format='poima.runtime', version=1, engine_version=self.run_cli('version')['version'],
                    target_os=target, target_arch='x86_64', executable=executable,
                    features=dict(simulation=True, renderer=True, audio=False, managed=False, editor=False))
        world = self.project/self.document['entry']['world']
        revision = json.loads(world.read_text())['revision']
        self.world_rpc('world.transact', request_id=uuid.uuid4().hex, base_revision=revision, ops=[
            dict(op='ui.element.set', id='1'*32, element=dict(parent=None, name='Status', kind='label',
                 text='Health 100', action=None, visible=True, enabled=True))])
        for lock in self.project.rglob('*.lock'): lock.unlink()
        source = tree(self.project)
        for flag in (None, False, 'true'):
            if flag is None: spec['features'].pop('game_ui', None)
            else: spec['features']['game_ui'] = flag
            (runtime/'runtime.json').write_text(json.dumps(spec))
            destination = self.root/('Rejected UI '+str(flag))
            self.run_cli('project', 'build', native(self.manifest), '--runtime', native(runtime),
                         '--output', native(destination), success=False)
            self.assertIn('Boolean' if isinstance(flag, str) else 'game-UI-enabled',
                          json.dumps(CALLS[-1]['reply']))
            self.assertFalse(destination.exists())
        spec['features']['game_ui'] = True
        (runtime/'runtime.json').write_text(json.dumps(spec))
        bundle = self.root/'UI bundle'
        self.run_cli('project', 'build', native(self.manifest), '--runtime', native(runtime), '--output', native(bundle))
        self.run_cli('game', 'inspect', native(bundle/'game.json'))
        # Rehash a coherent inventory after removing the capability: inspection
        # must enforce the content requirement, independently of hash checking.
        descriptor = bundle/'runtime/runtime.json'
        spec['features'].pop('game_ui')
        descriptor.write_text(json.dumps(spec))
        manifest = bundle/'game.json'; game = json.loads(manifest.read_text())
        row = next(row for row in game['files'] if row['path'] == 'runtime/runtime.json')
        row.update(size=descriptor.stat().st_size, sha256=hashlib.sha256(descriptor.read_bytes()).hexdigest())
        manifest.write_text(json.dumps(game))
        self.run_cli('game', 'inspect', native(manifest), success=False)
        self.assertIn('game-UI-enabled', json.dumps(CALLS[-1]['reply']))
        self.assertEqual(tree(self.project), source)

    @unittest.skipUnless(args.runtime, 'Requires explicit installed runtime root.')
    def test_export_inventory_inspection_tamper_and_source_unchanged(self):
        before = tree(self.project)
        destination = self.root/'Exported game'
        result = self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(args.runtime))
        self.assertEqual(result['output'].replace('\\', '/'), native(destination).replace('\\', '/'))
        self.assertGreater(result['file_count'], 0)
        self.assertIn(result['publication'], ('atomic_directory', 'manifest_last'))
        self.assertFalse((destination/'.poima-incomplete').exists(), 'Successful export retained an incomplete marker.')
        self.assertEqual(tree(self.project), before)
        game = destination/'game.json'
        self.assertTrue(game.is_file())
        game_document = json.loads(game.read_text(encoding='utf-8'))
        self.assertEqual(game_document['version'], 1, 'Projects without gameplay retain v1 bundles.')
        self.assertNotIn('gameplay', game_document)
        bundle = tree(destination)
        self.assertFalse(any(Path(path).name in ('AGENTS.md', 'CLAUDE.md') for path in bundle))
        inspected = self.run_cli('game', 'inspect', native(game))
        self.assertEqual(inspected['project_id'], self.document['project_id'])
        self.assertEqual(tree(destination), bundle)
        marker = destination/'.poima-incomplete'
        marker.write_text('Publication is incomplete.\n', encoding='utf-8')
        incomplete = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), incomplete, 'Inspection changed an incomplete bundle.')
        marker.unlink()
        self.run_cli('game', 'inspect', native(game))
        self.assertEqual(tree(destination), bundle, 'Inspection changed the restored complete bundle.')
        self.run_cli('project', 'build', native(self.manifest), '--output', native(destination), '--runtime', native(args.runtime), success=False)
        self.assertEqual(tree(destination), bundle)
        packaged_world = next(p for p in destination.rglob('*.json') if p.name != 'game.json' and
            '"poima.authored-world"' in p.read_text(encoding='utf-8', errors='replace'))
        original = packaged_world.read_bytes()
        packaged_world.write_bytes(original+b' ')
        tampered = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), tampered)
        packaged_world.write_bytes(original)
        packaged_world.unlink()
        missing = tree(destination)
        self.run_cli('game', 'inspect', native(game), success=False)
        self.assertEqual(tree(destination), missing)

    @unittest.skipUnless(args.runtime, 'Requires explicit installed runtime root.')
    def test_export_cannot_replace_source_project(self):
        before = tree(self.project)
        self.run_cli('project', 'build', native(self.manifest), '--output', native(self.project), '--runtime', native(args.runtime), success=False)
        self.assertEqual(tree(self.project), before)

if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProjectContract))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({'tests': result.testsRun, 'passed': result.wasSuccessful(),
            'skipped': len(result.skipped), 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'calls': CALLS}, indent=2)+'\n', encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
