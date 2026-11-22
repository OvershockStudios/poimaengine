#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored startup render settings on a relocated Native AOT game."""

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path, PureWindowsPath
import subprocess
import traceback

from game_launch_lighting import AA_UI_REGION, CAMERA, PLAYER, VALIDATION_MESSAGES
from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from reconstruction_composition_capture import (
    CAPTURE_SIZE, DEVICE_VALIDATION, FOG, SETTINGS, SPRITE_REPLAY, CompositionCapture,
    extent, intended_hdr, mapped_pixels, projected_sample, raster_ray_y, sprite_alpha,
    verify_reconstruction, with_sprite,
)
from scene_capture import pixels
from screen_reflections_capture import BLUE, RED
from world_terrain_contract import file_hash, success, tree_hashes


STARTUP = dict(lighting_path='deferred', reconstruction='fsr3_quality', samples=1)
EXPLICIT = ('--lighting-path', 'deferred', '--reconstruction', 'fsr3_quality', '--samples', '1')
OVERRIDE = ('--lighting-path', 'forward', '--reconstruction', 'none')


class StartupRenderCapture(CompositionCapture):
    def reject(self, *arguments, executable=None, cwd=None):
        argv = [str((executable or self.args.binary).resolve()), *arguments]
        run = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.calls.append(dict(argv=argv, cwd=str(cwd) if cwd else None, exit=run.returncode,
                               stdout=run.stdout, stderr=run.stderr))
        reply = json.loads(run.stdout)
        assert run.returncode == 4 and reply['status'] == 'error', reply
        return reply

    def configure(self):
        inspection = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert 'startup_render' not in inspection
        old = tree_hashes(self.project)
        assert inspection['manifest_sha256'] == old[self.manifest.name]
        patch = json.dumps(dict(startup_render=STARTUP), separators=(',', ':'))
        receipt = success(json.loads(self.cli('project', 'configure', self.native(self.manifest), '--patch', patch,
            '--expected-manifest-sha256', inspection['manifest_sha256'])))
        assert receipt['changed'] is True and receipt['startup_render'] == STARTUP, receipt
        assert receipt['manifest_sha256'] == file_hash(self.manifest) != inspection['manifest_sha256'], receipt
        new = tree_hashes(self.project)
        # Configure can create its coordination lock. All previously present
        # source files outside the manifest have to keep their original bytes.
        assert {name: new[name] for name in old if name != self.manifest.name} == {
            name: digest for name, digest in old.items() if name != self.manifest.name}
        inspection = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert inspection['manifest_sha256'] == receipt['manifest_sha256'] and inspection['startup_render'] == STARTUP
        self.measurements['configuration'] = dict(patch=json.loads(patch), receipt=receipt, inspection=inspection)

    def refuse_missing_fsr(self):
        runtime = self.root/'FSR-disabled runtime descriptor'
        runtime.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features']['fsr3_upscaler'] is True
        descriptor['features']['fsr3_upscaler'] = False
        (runtime/'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
        output = self.root/'Unpublished FSR-disabled export'
        files = tree_hashes(self.project)
        reply = self.reject('project', 'build', self.native(self.manifest), '--output', self.native(output),
                            '--runtime', self.native(runtime))
        assert [row['message'] for row in reply['diagnostics']] == [
            'startup_render reconstruction requires an FSR3-enabled runtime.'], reply
        assert not output.exists() and tree_hashes(self.project) == files
        self.measurements['rejections']['runtime_without_fsr'] = reply

    def export(self, name, revision, startup):
        original = self.root/f'{name} original export'
        relocated = self.root/f'{name} relocated game'
        build = success(json.loads(self.cli('project', 'build', self.native(self.manifest),
            '--output', self.native(original), '--runtime', self.native(self.args.runtime_root))))
        assert build['revision'] == revision and original.is_dir(), build
        inventory = tree_hashes(original)
        original.rename(relocated)
        assert not original.exists() and tree_hashes(relocated) == inventory
        game = relocated/'game.json'
        spec = json.loads(game.read_bytes())
        assert spec['entry']['camera'] == CAMERA and spec['entry']['controller'] == PLAYER
        executable = relocated/spec['engine']['executable']
        assert file_hash(executable) == file_hash(self.args.binary)
        inspect = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        for result in (spec, inspect):
            assert ('startup_render' in result) is (startup is not None)
            assert result.get('startup_render') == startup
        for key in ('needs_height_fog', 'needs_screen_reflections', 'needs_sprite_particles'):
            assert inspect[key] is True, inspect
        assets = [value for path, value in inventory.items() if Path(path).name == self.sprite_texture+'.pimage']
        assert assets == [self.sprite_texture], assets
        self.measurements['bundles'][name] = dict(build=build, inspection=inspect, inventory=inventory,
                                                 manifest_sha256=file_hash(game))
        return game, executable

    def check_immutable(self):
        assert not self.project.exists()
        assert tree_hashes(self.detached_source) == self.measurements['source_inventory']
        for name, (game, _) in self.games.items():
            assert not (self.root/f'{name} original export').exists()
            assert tree_hashes(game.parent) == self.measurements['bundles'][name]['inventory']
        assert file_hash(self.profile) == self.profile_sha
        assert file_hash(self.replay) == self.measurements['replay_sha256']

    def run_arguments(self, name, output):
        game, _ = self.games[name]
        width, height = CAPTURE_SIZE
        return ['game', 'run', self.native(game), '--replay', self.native(self.replay),
                '--report', self.native(output/'report.json'), '--capture', self.native(output/'frame.bmp'),
                '--save-root', self.native(output/'saves'), '--gpu', str(self.args.gpu),
                '--width', str(width), '--height', str(height)]

    def refuse_explicit_four(self):
        output = self.root/'Legacy explicit 4x rejection'
        (output/'saves').mkdir(parents=True)
        _, executable = self.games['legacy']
        reply = self.reject(*self.run_arguments('legacy', output), '--samples', '4', executable=executable,
                            cwd=self.launch_directory)
        assert [row['message'] for row in reply['diagnostics']] == ['Textured particle sprites require samples=1.'], reply
        assert sorted(path.name for path in output.iterdir()) == ['saves'] and not list((output/'saves').iterdir())
        self.check_immutable()
        self.measurements['rejections']['legacy_sprite_explicit_four'] = reply

    def automatic_launch(self, game, arguments):
        launcher = game.parent/'launch.cmd'
        assert file_hash(launcher) == self.measurements['bundles']['authored']['inventory']['launch.cmd']
        tokens = [self.native(launcher), *arguments]
        assert all(not any(char in value for char in '"%\r\n') for value in tokens), tokens
        if self.args.windows_interop:
            # WSL's interop argv quoting does not preserve cmd /S /C's outer
            # quotes. PowerShell passes the literal arguments to the batch.
            literals = ' '.join("'"+value.replace("'", "''")+"'" for value in tokens)
            command = ['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                       '& '+literals+'; exit $LASTEXITCODE']
        else:
            # Windows subprocess accepts the raw command line needed by cmd.
            shell = os.environ.get('COMSPEC', 'cmd.exe')
            command = f'"{shell}" /d /v:off /s /c "'+' '.join(f'"{value}"' for value in tokens)+'"'
        call = dict(argv=command, cwd=str(self.launch_directory), launcher=tokens[0], launcher_arguments=arguments)
        self.calls.append(call)
        try:
            run = subprocess.run(command, cwd=self.launch_directory, capture_output=True, text=True,
                                 encoding='utf-8', timeout=90)
        except subprocess.TimeoutExpired as exc:
            call.update(timeout=exc.timeout, stdout=(exc.stdout or b'').decode('utf-8', 'replace'),
                        stderr=(exc.stderr or b'').decode('utf-8', 'replace'))
            raise
        call.update(exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
        assert run.returncode == 0, (command, run.stdout, run.stderr)
        return run.stdout

    def capture_startup(self, label, bundle, flags, lighting, mode):
        output = self.root/label
        saves = output/'saves'
        saves.mkdir(parents=True)
        game, executable = self.games[bundle]
        common = self.run_arguments(bundle, output)
        if bundle == 'legacy' or label == 'override':
            common += ['--settings-profile', self.native(self.profile)]
        if label == 'automatic':
            stdout = self.automatic_launch(game, common[3:])
        else:
            stdout = self.cli(*common, *flags, executable=executable, cwd=self.launch_directory)
        result = success(json.loads(stdout))
        report, bmp = output/'report.json', output/'frame.bmp'
        assert result['success'] and exact(json.loads(report.read_bytes()), result), result
        call = self.calls[-1]
        assert all(marker not in call['stderr'] for marker in VALIDATION_MESSAGES), call['stderr']
        if self.args.require_validation:
            assert DEVICE_VALIDATION in call['stderr'], call['stderr']
        play = result['play']
        assert play['success'] and play['hardware'] and play['capture_written'] and play['nvrhi_errors'] == 0, play
        assert play['stop_reason'] == 'replay_complete' and play['tick'] == 1 and play['frames_presented'] >= 2, play
        assert play['runtime_replacements'] == 0 and play['current_session_id'] == play['initial_session_id'], play
        assert (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, 1), play
        diag = play['render_diagnostics']
        rec = verify_reconstruction(diag, mode, reflections='active' if lighting == 'deferred' else 'forward',
                                    fog='active', mask=mode != 'none', lighting=lighting, sprites=True)
        if bundle == 'legacy' or label == 'override':
            settings = play['settings']
            assert settings['fields']['graphics.samples']['effective'] == 1, settings
            assert settings['profile']['content_hash'] == self.measurements['profile_receipt']['content_hash']
        data, saved = self.read_save(saves)
        terminal = self.inspect_terminal(result, play['current_session_id'])
        assert data == self.baselines[bundle][0] and exact(terminal, self.baselines[bundle][1])

        frame = pixels(bmp)
        witnesses = {}
        jitter = rec['jitter_pixels'] if mode != 'none' else (0, 0)
        for site, surface in (('warm_target', RED), ('cool_target', BLUE)):
            point = mapped_pixels(mode)[site]
            x, y = (int((v+.5)*size/grid) for v, size, grid in zip(point, CAPTURE_SIZE, extent(mode), strict=True))
            geometry = projected_sample(site, point, mode, jitter)
            alpha = sprite_alpha(self.cards, site, point, mode, jitter, geometry)
            fogged = intended_hdr(site, surface.emission, geometry, False, True, raster_ray_y(point, mode, jitter))
            expected = tuple(display(channel) for channel in with_sprite(fogged, alpha))
            actual = frame[y][x]
            assert max(abs(color-reference) for color, reference in zip(actual, expected, strict=True)) <= 3, (
                label, site, actual, expected)
            witnesses[site] = dict(pixel=[x, y], rgb=actual, expected_rgb=expected, alpha=alpha)
        left, top, right, bottom = AA_UI_REGION
        interface = [pixel for row in frame[top:bottom] for pixel in row[left:right]]
        assert min(color[0] for color in interface) < 70 and max(color[0] for color in interface) > 190
        assert len(set(interface)) >= 6
        measurement = dict(bundle=bundle, flags=flags, lighting=lighting, samples=play['samples'], gpu=play['gpu'],
                           reconstruction=rec, saved=saved, report_sha256=file_hash(report),
                           capture_sha256=file_hash(bmp), witnesses=witnesses)
        self.measurements['runs'][label] = measurement
        self.check_immutable()
        return measurement, interface

    def compare_headless_relocations(self):
        descriptor = json.loads(self.args.artifact.read_bytes())
        library, = [row for row in descriptor['files'] if row['role'] == 'library']
        relative = 'gameplay/'+descriptor['library']
        assert descriptor['library'] == library['path']
        paths, states = {}, {}
        for name, (game, _) in self.games.items():
            _, terminal = self.baselines[name]
            states[name] = deepcopy(terminal)
            observed = states[name]['gameplay']['module'].pop('native_library')
            expected = self.native(game.parent/relative)
            assert PureWindowsPath(observed) == PureWindowsPath(expected), (observed, expected)
            assert self.measurements['bundles'][name]['inventory'][relative] == library['sha256']
            paths[name] = observed
        assert PureWindowsPath(paths['legacy']) != PureWindowsPath(paths['authored'])
        assert self.baselines['legacy'][0] == self.baselines['authored'][0]
        assert exact(states['legacy'], states['authored'])
        self.measurements['headless_relocation'] = dict(native_libraries=paths, library_sha256=library['sha256'],
                                                      remaining_state_exact=True, save_bytes_exact=True)

    def run(self):
        self.measurements.update(bundles={}, profiles={}, runs={}, rejections={})
        self.sprite_texture = None
        revision = self.author_sprites(self.author_fixture())
        revision = self.author_profile('composed', revision, 8681, SETTINGS, FOG)
        self.games = {'legacy': self.export('legacy', revision, None)}
        self.configure()
        self.refuse_missing_fsr()
        self.games['authored'] = self.export('authored', revision, STARTUP)
        old, new = (self.measurements['bundles'][label]['inventory'] for label in ('legacy', 'authored'))
        assert set(old) == set(new) and [name for name in old if old[name] != new[name]] == ['game.json']

        self.detached_source = self.root/'Unavailable authoring source'
        self.project.rename(self.detached_source)
        self.measurements['source_inventory'] = tree_hashes(self.detached_source)
        self.replay = self.root/'one-tick.replay.json'
        self.replay.write_text(json.dumps(SPRITE_REPLAY)+'\n', encoding='utf-8')
        self.measurements['replay_sha256'] = file_hash(self.replay)
        self.launch_directory = self.root/'Unrelated working directory'
        self.launch_directory.mkdir()
        self.baselines = {}
        for label, (self.game, _) in self.games.items():
            self.baselines[label] = self.headless_baseline(label+' headless')
        self.compare_headless_relocations()
        self.refuse_explicit_four()
        if self.args.headless_preflight:
            return

        automatic, ui = self.capture_startup('automatic', 'authored', (), 'deferred', 'fsr3_quality')
        explicit, explicit_ui = self.capture_startup('explicit', 'authored', EXPLICIT, 'deferred', 'fsr3_quality')
        override, override_ui = self.capture_startup('override', 'authored', OVERRIDE, 'forward', 'none')
        legacy, legacy_ui = self.capture_startup('legacy-fallback', 'legacy', (), 'forward', 'none')
        assert automatic['capture_sha256'] == explicit['capture_sha256'] != override['capture_sha256'] == legacy['capture_sha256']
        assert ui == explicit_ui and override_ui == legacy_ui
        # The label's opaque colors must cover the same pixels with either
        # lighting path. At partial coverage, the world can affect the blend.
        glyph, panel = (242, 246, 250), (56, 37, 62)
        opaque = lambda patch: [pixel if pixel in (glyph, panel) else None for pixel in patch]
        assert ui.count(glyph) > 100 and ui.count(panel) > 2000
        assert opaque(ui) == opaque(override_ui), 'A graphics profile changed the label glyph or panel coverage.'
        self.measurements['cross_lighting_ui'] = dict(
            opaque_glyph_pixels=ui.count(glyph), opaque_panel_pixels=ui.count(panel),
            changed_partial_coverage_pixels=sum(a != b for a, b in zip(ui, override_ui, strict=True)),
            maximum_channel_difference=max(abs(a-b) for left, right in zip(ui, override_ui, strict=True)
                                           for a, b in zip(left, right, strict=True)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('runtime-root', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    parser.add_argument('--headless-preflight', action='store_true')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Assertions and a valid GPU index are required.')
    args.fxaa = True  # Reuse the prior fixture's opaque UI and diagonal witness.
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    fixture = StartupRenderCapture(args, args.output)
    record = dict(format='poima.project-startup-render-capture.v1', passed=False, gpu=args.gpu,
                  headless_only=args.headless_preflight, binary_sha256=file_hash(args.binary),
                  artifact_sha256=file_hash(args.artifact), runtime_descriptor_sha256=file_hash(args.runtime_root/'runtime.json'),
                  fixture_sha256={name: file_hash(Path(__file__).with_name(name)) for name in (
                      'project_startup_render_capture.py', 'reconstruction_composition_capture.py', 'game_launch_lighting.py')},
                  calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        output = args.output/'evidence.json'
        output.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {output}')


if __name__ == '__main__':
    main()
