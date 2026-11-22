#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check standalone Vulkan lighting selection on one relocated reflection scene."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from screen_reflections_capture import (
    EYE, LENS, PITCH, SETTINGS, ReflectionCapture, camera_matrix, witness_pixels,
)
from world_particles_contract import component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, success, tree_hashes, uid


PLAYER, CAMERA, ANCHOR, ITEM, SUPPORT = map(uid, (2, 3, 8240, 8241, 8242))
GAME = 'Poima.Tests.AttachmentContactGame'
IDENTITY = 'poima.test.item-attachment-contact'
REPLAY = [dict(ticks=1)]
VALIDATION_MESSAGES = ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')


def without_session(values):
    return {key: value for key, value in values.items() if key != 'session_id'}


class StandaloneLighting(ReflectionCapture):
    def author_fixture(self):
        revision = self.author()
        # The standalone entry uses the controller's own camera. Match the
        # independent probe rig, with a non-rendered floor beneath its feet.
        revision = self.transact(revision, 8251,
            component(PLAYER, 'Transform', transform([EYE[0], 0, EYE[2]])),
            component(CAMERA, 'Transform', transform([0, EYE[1], 0],
                [math.sin(PITCH/2), 0, 0, math.cos(PITCH/2)])),
            component(CAMERA, 'Camera', LENS),
            dict(op='entity.create', id=SUPPORT, name='Camera support'),
            component(SUPPORT, 'Transform', transform([EYE[0], -.5, EYE[2]])),
            component(SUPPORT, 'BoxCollider', dict(motion='static', half_extents=[1, .5, 1],
                                                   mass=1, friction=.5, restitution=0)),
            dict(op='entity.create', id=ANCHOR, name='Save fixture anchor'),
            component(ANCHOR, 'Transform', transform([0, 1, 12])),
            dict(op='entity.create', id=ITEM, name='Off-camera fixture item'),
            component(ITEM, 'Transform', transform([.25, 1, 12])),
            component(ITEM, 'MeshRenderer', dict(primitive='box', albedo=[.8, .3, .1],
                                                 visible=True, cast_shadows=False)),
        )
        # Reuse the compiled attachment-contact test consumer. On tick one it
        # commits an off-camera binding and saves without embedding a host token.
        descriptor = json.loads(self.args.artifact.read_bytes())
        assert descriptor['type'] == GAME and descriptor['schema']['identity'] == IDENTITY
        assert (descriptor['target_os'], descriptor['target_arch']) == ('Windows', 'x86_64')
        destination = self.project/'gameplay'
        destination.mkdir()
        shutil.copyfile(self.args.artifact, destination/'native-gameplay.json')
        for entry in descriptor['files']:
            relative = Path(entry['path'])
            assert not relative.is_absolute() and '..' not in relative.parts
            source = self.args.artifact.parent/relative
            assert source.stat().st_size == entry['size'] and file_hash(source) == entry['sha256']
            target = destination/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        project = json.loads(self.manifest.read_bytes())
        assert project['entry']['controller'] == PLAYER and project['entry']['camera'] == CAMERA
        project.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json',
            values=dict(Command=1, Item=ITEM, First=ANCHOR, Second=ANCHOR, OffsetX=.25)))
        self.manifest.write_text(json.dumps(project, indent=2)+'\n', encoding='utf-8')
        self.profile = self.root/'saved-four.poima-settings.json'
        profile = self.world_call('settings.transact', path=self.native(self.profile),
                                  request_id=uid(8252), expected_revision=0, set={'graphics.samples':4})
        self.measurements['profile_receipt'] = profile
        self.profile_sha = file_hash(self.profile)
        assert profile['revision'] == 1
        return revision

    def read_save(self, storage):
        assert {path.name for path in storage.iterdir()} == {'slot-contact-auto'}
        slot = storage/'slot-contact-auto'
        receipt = json.loads((slot/'current.json').read_bytes())['payload']
        assert receipt['generation'] == 1 and receipt['previous'] is None and len(receipt['receipts']) == 1
        archive, = slot.glob('p-*.bin')
        data = archive.read_bytes()
        current = receipt['current']
        assert archive.name == current['file'] and file_hash(archive) == current['sha256']
        assert len(data) == current['bytes']
        saved = json.loads(data)['snapshot']['payload']
        assert saved['tick'] == 1
        return data, dict(file=archive.relative_to(self.root).as_posix(), sha256=current['sha256'], bytes=len(data))

    def inspect_terminal(self, report, session):
        runtime = report['runtime']
        assert runtime['session_id'] == session and runtime['tick'] == 1
        assert runtime['structure_revision'] == 1 and not runtime['source_stale']
        entities = report['entities']
        assert set(entities) == {PLAYER, CAMERA}
        near_vector(entities[CAMERA]['world_matrix'], camera_matrix(), tolerance=.005)
        gameplay = report['gameplay']
        module = gameplay['module']
        assert module['backend'] == 'native_aot' and module['schema']['identity'] == IDENTITY
        assert module['values']['Ticks'] == 1 and module['values']['Command'] == 0
        return dict(runtime=without_session(runtime), gameplay=without_session(gameplay),
                    entities={key: without_session(view) for key, view in entities.items()})

    def headless_baseline(self, label):
        storage = self.root/(label+' saves')
        storage.mkdir()
        with self.owner(label, game=self.game, saves=storage) as (client, _):
            status = client.call('runtime.status')
            assert status['active'] is True and status['tick'] == 0
            owner = status['session_id']
            assert REPLAY == [dict(ticks=1)]
            stepped = client.call('runtime.step', dict(session_id=owner, request_id=uid(8302),
                expected_tick=0, ticks=REPLAY[0]['ticks'], inputs=[dict(entity=PLAYER)]))
            assert stepped['tick'] == 1 and stepped['save_serviced']
            report = dict(runtime=client.call('runtime.inspect', dict(session_id=owner)),
                entities={key: client.call('runtime.entity', dict(session_id=owner, id=key))
                          for key in (PLAYER, CAMERA)},
                gameplay=client.call('runtime.gameplay.inspect', dict(session_id=owner, include_schema=True)))
            normalized = self.inspect_terminal(report, owner)
        data, saved = self.read_save(storage)
        self.measurements[label] = dict(saved=saved, terminal=normalized)
        self.check_immutable()
        return data, normalized

    def check_immutable(self):
        assert tree_hashes(self.game.parent) == self.measurements['bundles']['enabled']['inventory']
        assert not self.original_bundle.exists() and not self.project.exists()
        assert tree_hashes(self.detached_source) == self.measurements['source_inventory']
        assert file_hash(self.profile) == self.profile_sha
        assert file_hash(self.replay) == self.measurements['replay_sha256']

    def launch(self, label, extras, *, path=None, samples=None):
        output = self.root/label
        output.mkdir()
        storage = output/'external saves'
        storage.mkdir()
        report, capture = output/'report.json', output/'frame.bmp'
        args = ['game', 'run', self.native(self.game), '--replay', self.native(self.replay),
            '--save-root', self.native(storage), '--report', self.native(report), '--capture', self.native(capture),
            '--gpu', str(self.args.gpu), '--width', str(CAPTURE_SIZE[0]), '--height', str(CAPTURE_SIZE[1]), *extras]
        if path is None:
            argv = [str(self.executable), *args]
            child = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8',
                                   timeout=30, cwd=self.launch_directory)
            self.calls.append(dict(argv=argv, cwd=str(self.launch_directory), exit=child.returncode,
                                   stdout=child.stdout, stderr=child.stderr))
            rejected = json.loads(child.stdout)
            assert child.returncode == 2 and rejected['status'] == 'error', rejected
            assert {row['code'] for row in rejected['diagnostics']} == {'invalid_arguments'}, rejected
            assert not report.exists() and not capture.exists() and not list(storage.iterdir())
            self.measurements['rejections'][label] = rejected
            self.check_immutable()
            return None

        result = success(json.loads(self.cli(*args, executable=self.executable, cwd=self.launch_directory)))
        assert json.loads(report.read_bytes()) == result and result['success']
        play = result['play']
        assert play['success'] and play['hardware'] and play['capture_written'] and play['nvrhi_errors'] == 0, play
        assert play['stop_reason'] == 'replay_complete' and play['frames_presented'] >= 1 and play['tick'] == 1, play
        assert play['runtime_replacements'] == 0 and play['current_session_id'] == play['initial_session_id'], play
        assert (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, samples), play
        diagnostics = play['render_diagnostics']
        assert diagnostics['lighting_path'] == path and diagnostics['culling'] is True, diagnostics
        draws = diagnostics['last_draws']
        assert draws['camera_draws'] == 4 and draws['camera_culled'] >= 1, draws
        reflections = diagnostics['screen_reflections']
        assert reflections['active'] is (path == 'deferred'), reflections
        assert reflections['reason'] == ('active' if path == 'deferred' else 'forward'), reflections
        assert reflections['buffer_bytes'] == (math.prod(CAPTURE_SIZE)*10 if path == 'deferred' else 0)
        terminal = self.inspect_terminal(result, play['current_session_id'])
        data, saved = self.read_save(storage)
        assert data == self.baseline[0] and exact(terminal, self.baseline[1])
        image = pixels(capture)
        assert len(image) == CAPTURE_SIZE[1] and all(len(row) == CAPTURE_SIZE[0] for row in image)
        samples_rgb = {name: image[y][x] for name, (x, y) in witness_pixels().items()}
        measured = dict(samples=samples, lighting_path=path, gpu=play['gpu'], saved=saved,
            report_sha256=file_hash(report), capture_sha256=file_hash(capture), probes=samples_rgb)
        self.measurements['runs'][label] = measured
        self.check_immutable()
        return measured

    def run(self):
        self.measurements.update(bundles={}, runs={}, rejections={})
        revision = self.author_fixture()
        self.original_bundle = self.root/'enabled original export'
        self.game = self.bundle('enabled', SETTINGS, 1, revision, 8253)
        descriptor = json.loads(self.game.read_bytes())
        assert descriptor['entry']['controller'] == PLAYER and descriptor['entry']['camera'] == CAMERA
        self.executable = (self.game.parent/descriptor['engine']['executable']).resolve()
        assert file_hash(self.executable) == file_hash(self.args.binary)
        info = success(json.loads(self.cli('game', 'inspect', self.native(self.game), executable=self.executable)))
        assert info['needs_screen_reflections'] is True
        self.detached_source = self.root/'Detached reflection authoring'
        self.project.rename(self.detached_source)
        self.measurements['source_inventory'] = tree_hashes(self.detached_source)
        self.replay = self.root/'one-tick.replay.json'
        self.replay.write_text(json.dumps(REPLAY)+'\n', encoding='utf-8')
        self.measurements['replay_sha256'] = file_hash(self.replay)
        self.launch_directory = self.root/'unrelated working directory'
        self.launch_directory.mkdir()
        self.baseline = self.headless_baseline('headless')
        self.launch('invalid-path', ['--lighting-path', 'unknown'])
        self.launch('duplicate-path', ['--lighting-path', 'deferred', '--lighting-path', 'forward'])
        self.launch('missing-path', ['--lighting-path'])
        self.launch('deferred-four', ['--lighting-path', 'deferred', '--samples', '4'])
        if self.args.headless_preflight:
            assert self.headless_baseline('headless-repeat') == self.baseline
            return

        default = self.launch('default-forward', [], path='forward', samples=4)
        forward = self.launch('explicit-forward', ['--lighting-path', 'forward'], path='forward', samples=4)
        deferred = self.launch('default-deferred', ['--lighting-path', 'deferred'], path='deferred', samples=1)
        one = self.launch('explicit-one', ['--lighting-path', 'deferred', '--samples', '1'], path='deferred', samples=1)
        stored = self.launch('stored-four-deferred', ['--lighting-path', 'deferred',
                             '--settings-profile', self.native(self.profile)], path='deferred', samples=1)
        assert default['capture_sha256'] == forward['capture_sha256']
        assert deferred['capture_sha256'] == one['capture_sha256'] == stored['capture_sha256']
        assert deferred['capture_sha256'] != forward['capture_sha256']
        assert default['gpu'] == forward['gpu'] == deferred['gpu'] == one['gpu'] == stored['gpu']
        for name, channel in (('warm_hit', 0), ('cool_hit', 2)):
            assert deferred['probes'][name][channel] > forward['probes'][name][channel]+3, (name, deferred, forward)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('runtime-root', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--headless-preflight', action='store_true',
                        help='Run fixture authoring, save and parser controls without presenting.')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Assertions and a valid GPU index are required.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    test = StandaloneLighting(args, args.output)
    record = dict(format='poima.game-launch-lighting.v1', passed=False, preflight=args.headless_preflight, gpu=args.gpu,
        binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
        reflection_fixture_sha256=file_hash(Path(__file__).with_name('screen_reflections_capture.py')),
        artifact_descriptor_sha256=file_hash(args.artifact),
        runtime_descriptor_sha256=file_hash(args.runtime_root/'runtime.json'),
        calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        for call in test.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in VALIDATION_MESSAGES), stderr
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = args.output/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
