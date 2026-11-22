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
from world_particles_contract import RpcError, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, success, tree_hashes, uid


PLAYER, CAMERA, ANCHOR, ITEM, SUPPORT = map(uid, (2, 3, 8240, 8241, 8242))
GAME = 'Poima.Tests.AttachmentContactGame'
IDENTITY = 'poima.test.item-attachment-contact'
REPLAY = [dict(ticks=1)]
VALIDATION_MESSAGES = ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')
AA_BOARD, AA_LABEL = map(uid, (8243, 8244))
AA_FOG = dict(density_per_m=.024, base_height_m=.3, height_falloff_per_m=.18,
              start_distance_m=1.8, max_distance_m=11.5,
              ambient_radiance=[.07, .11, .20], anisotropy=0)
AA_BOARD_CENTER = (495, 75)
AA_EDGE_REGION = (431, 19, 559, 131)
AA_UI_REGION = (40, 43, 176, 78)


def without_session(values):
    return {key: value for key, value in values.items() if key != 'session_id'}


def antialiasing_controls():
    width, height = CAPTURE_SIZE
    focal = height / (2*math.tan(math.radians(LENS['vertical_fov'])/2))
    x, y = AA_BOARD_CENTER
    distance = 3.4
    coordinates = [(x-width/2)*distance/focal, (height/2-y)*distance/focal, -distance]
    camera = camera_matrix()
    center = [EYE[i] + sum(coordinates[j]*camera[j*4+i] for j in range(3)) for i in range(3)]
    # Orient a bright face toward the lens, then rotate it 27 degrees to
    # put two long diagonal edges against the empty, dark background.
    sx, cx = math.sin(PITCH/2), math.cos(PITCH/2)
    sz, cz = math.sin(math.radians(27)/2), math.cos(math.radians(27)/2)
    rotation = [sx*cz, -sx*sz, cx*sz, cx*cz]
    left, top, right, bottom = AA_UI_REGION
    offsets = dict(left=left, top=top, width=right-left, height=bottom-top)
    label_layout = {key: dict(unit='dp', value=value) for key, value in offsets.items()}
    return [
        dict(op='entity.create', id=AA_BOARD, name='Diagonal antialiasing witness'),
        component(AA_BOARD, 'Transform', transform(center, rotation) | dict(scale=[.9, .55, .025])),
        component(AA_BOARD, 'MeshRenderer', dict(primitive='box', albedo=[0]*3,
            visible=True, cast_shadows=False, receive_shadows=False)),
        component(AA_BOARD, 'PbrMaterial', dict(base_color=[0]*3, emissive=[1]*3,
            metallic=0, roughness=.8, double_sided=False)),
        dict(op='ui.element.set', id=AA_LABEL, element=dict(parent=None, name='UI glyph control',
            kind='label', text='AA X /', action=None, visible=True, enabled=True,
            layout=dict(position='absolute', padding=[0]*4, **label_layout),
            style=dict(background_color='#38253e', color='#f2f6fa', font_size=28,
                       border_width=0, text_align='center'))),
    ]


def verify_antialiasing_pixels(plain, filtered):
    def patch(image, bounds):
        left, top, right, bottom = bounds
        return [pixel for row in image[top:bottom] for pixel in row[left:right]]

    for x, y in (AA_BOARD_CENTER, (586, 63)):
        box = (x-3, y-3, x+4, y+4)
        pixels_before = patch(plain, box)
        assert pixels_before == patch(filtered, box), (x, y)
        assert max(pixel[0] for pixel in pixels_before)-min(pixel[0] for pixel in pixels_before) <= 2
    x, y = AA_BOARD_CENTER
    assert plain[y][x][0] > plain[63][586][0]+70, 'The diagonal fixture needs two visible flat colors.'

    interface = patch(plain, AA_UI_REGION)
    assert interface == patch(filtered, AA_UI_REGION), 'FXAA changed the composited UI.'
    assert min(color[0] for color in interface) < 70 and max(color[0] for color in interface) > 190
    assert len(set(interface)) >= 6, 'The UI control must contain rendered glyph edges.'

    left, top, right, bottom = AA_EDGE_REGION
    mixed = 0
    changed_rows = set()
    for y in range(top, bottom):
        for x in range(left, right):
            before, after = plain[y][x][0], filtered[y][x][0]
            if abs(before-after) <= 2:
                continue
            near = [plain[v][u][0] for v in range(y-1, y+2) for u in range(x-1, x+2)]
            low, high = min(near), max(near)
            assert high-low >= 45, (x, y, before, after, low, high)
            if low+3 < after < high-3:
                mixed += 1
                changed_rows.add(y)
    assert mixed >= 40 and len(changed_rows) >= 16, (mixed, changed_rows)
    return dict(interior_mixed_pixels=mixed, rows=len(changed_rows),
                flat_and_sky_pixels_exact=True, ui_pixels_exact=True)


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
        if self.args.fxaa:
            revision = self.transact(revision, 8254, *antialiasing_controls())
            layout = self.world_call('world.ui.layout', revision=revision, scale=1,
                                     width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1])
            label, = layout['controls']
            assert label['id'] == AA_LABEL and label['bounds'] == list(AA_UI_REGION), label
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

    def launch(self, label, extras, *, path=None, samples=None, fxaa=False):
        output = self.root/label
        output.mkdir()
        storage = output/'external saves'
        storage.mkdir()
        report, capture = output/'report.json', output/'frame.bmp'
        args = ['game', 'run', self.native(self.game), '--replay', self.native(self.replay),
            '--save-root', self.native(storage), '--report', self.native(report), '--capture', self.native(capture),
            '--gpu', str(self.args.gpu), '--width', str(CAPTURE_SIZE[0]), '--height', str(CAPTURE_SIZE[1])]
        if self.args.fxaa:
            args += ['--settings-overrides', '{"ui.scale":1}']
        args += extras
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
        assert draws['camera_draws'] == (5 if self.args.fxaa else 4) and draws['camera_culled'] >= 1, draws
        reflections = diagnostics['screen_reflections']
        assert reflections['active'] is (path == 'deferred'), reflections
        assert reflections['reason'] == ('active' if path == 'deferred' else 'forward'), reflections
        assert reflections['buffer_bytes'] == (math.prod(CAPTURE_SIZE)*10 if path == 'deferred' else 0)
        if self.args.fxaa:
            self.check_antialiasing_diagnostics(diagnostics, samples, fxaa)
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

    def check_antialiasing_diagnostics(self, diagnostics, samples, active):
        if active:
            assert samples == 1
            assert diagnostics['post_aa'] == dict(mode='fxaa', active=True, buffer_bytes=math.prod(CAPTURE_SIZE)*4)
        else:
            assert 'post_aa' not in diagnostics
        assert diagnostics['reconstruction']['mode'] == 'none' and not diagnostics['reconstruction']['active']
        fog = diagnostics['height_fog']
        assert fog['active'] is (samples == 1) and fog['reason'] == ('active' if samples == 1 else 'multisample')

    def check_pre_antialiasing_probes(self):
        storage = self.root/'probe saves'
        storage.mkdir()
        views = {}
        with self.owner('pre-antialiasing opaque probes', game=self.game, saves=storage) as (client, _):
            owner = client.call('runtime.status')['session_id']
            assert client.call('runtime.step', dict(session_id=owner, request_id=uid(8303), expected_tick=0,
                ticks=1, inputs=[dict(entity=PLAYER)]))['save_serviced']
            ui = client.call('runtime.ui.inspect', dict(session_id=owner, tick=1))
            basic = dict(session_id=owner, tick=1, structure_revision=1, camera=CAMERA,
                ui_revision=ui['ui_revision'], ui_scale=1, width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1],
                gpu=self.args.gpu, samples=1, lighting_path='deferred', reconstruction='none')
            for override in (dict(samples=4, lighting_path='forward'), dict(reconstruction='fsr3_native'),
                             dict(scene_debug_view='depth')):
                rejected = self.root/'unpublished-probe.bmp'
                request = basic | override | dict(post_aa='fxaa', path=self.native(rejected))
                try:
                    client.call('runtime.capture', request)
                except RpcError as error:
                    assert error.code == -32602, error
                else:
                    raise AssertionError(f'Incompatible FXAA runtime capture succeeded with {override!r}')
                assert not rejected.exists()
            sites = [*witness_pixels().values(), AA_BOARD_CENTER, (586, 63)]
            for mode in ('none', 'fxaa'):
                output = self.root/f'opaque-probe-{mode}.bmp'
                request = basic | dict(post_aa=mode, path=self.native(output),
                    scene_product_probes=[dict(x=x, y=y) for x, y in sites])
                captured = client.call('runtime.capture', request)
                assert captured['hardware'] and captured['capture_written'] and captured['nvrhi_errors'] == 0
                assert captured['tick'] == 1 and captured['samples'] == 1
                diagnostics = captured['render_diagnostics']
                self.check_antialiasing_diagnostics(diagnostics, 1, mode == 'fxaa')
                assert diagnostics['lighting_path'] == 'deferred' and diagnostics['screen_reflections']['active']
                products = diagnostics['scene_products']
                assert products['available'] and products['view'] == 'color'
                assert [(row['x'], row['y']) for row in products['probes']] == sites
                views[mode] = dict(probes=products['probes'], sha256=file_hash(output))
        assert exact(views['none']['probes'], views['fxaa']['probes'])
        assert views['none']['sha256'] != views['fxaa']['sha256']
        assert self.read_save(storage)[0] == self.baseline[0]
        self.check_immutable()
        self.measurements['pre_antialiasing'] = views

    def run(self):
        self.measurements.update(bundles={}, runs={}, rejections={})
        revision = self.author_fixture()
        self.original_bundle = self.root/'enabled original export'
        self.game = self.bundle('enabled', SETTINGS, 1, revision, 8253, height_fog=AA_FOG if self.args.fxaa else None)
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
        if self.args.fxaa:
            self.launch('invalid-post-aa', ['--post-aa', 'unknown'])
            self.launch('duplicate-post-aa', ['--post-aa', 'fxaa', '--post-aa', 'none'])
            self.launch('forward-four-fxaa', ['--post-aa', 'fxaa', '--samples', '4'])
            self.launch('deferred-four-fxaa', ['--lighting-path', 'deferred', '--post-aa', 'fxaa', '--samples', '4'])
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
        if self.args.fxaa:
            self.check_antialiasing(deferred)

    def check_antialiasing(self, default):
        deferred = ['--lighting-path', 'deferred']
        explicit_none = self.launch('deferred-aa-none', [*deferred, '--post-aa', 'none'],
                                    path='deferred', samples=1)
        filtered = self.launch('deferred-fxaa', [*deferred, '--post-aa', 'fxaa'],
                               path='deferred', samples=1, fxaa=True)
        self.launch('forward-one-fxaa', ['--lighting-path', 'forward', '--post-aa', 'fxaa', '--samples', '1'],
                    path='forward', samples=1, fxaa=True)
        assert default['capture_sha256'] == explicit_none['capture_sha256'] != filtered['capture_sha256']
        assert default['gpu'] == explicit_none['gpu'] == filtered['gpu']
        plain, active = (pixels(self.root/label/'frame.bmp') for label in ('deferred-aa-none', 'deferred-fxaa'))
        self.measurements['antialiasing_pixels'] = verify_antialiasing_pixels(plain, active)
        self.check_pre_antialiasing_probes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('runtime-root', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--fxaa', action='store_true', help='Qualify post-AA with composition and immutable replay.')
    parser.add_argument('--headless-preflight', action='store_true',
                        help='Run fixture authoring, save and parser controls without presenting.')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Assertions and a valid GPU index are required.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    test = StandaloneLighting(args, args.output)
    record = dict(format='poima.game-launch-lighting.v1', passed=False, preflight=args.headless_preflight,
        gpu=args.gpu, fxaa=args.fxaa,
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
