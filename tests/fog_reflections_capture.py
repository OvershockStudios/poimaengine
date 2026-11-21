#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify fog modulation of native screen reflections in relocated Vulkan games."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from screen_reflections_capture import (
    BLUE, CAMERA, ENVIRONMENT, EYE, LENS, RED, SETTINGS, ReflectionCapture, camera_matrix, ideal_gain,
    validate_reference as validate_reflection_reference, witness, witness_pixels,
)
from world_particles_contract import component, near_vector
from world_terrain_contract import CAPTURE_SIZE, file_hash, success, tree_hashes


EXPOSURE = 1.35
FOG = dict(density_per_m=.14, base_height_m=-3.5, height_falloff_per_m=0,
           start_distance_m=1.6, max_distance_m=8.7, ambient_radiance=[.34, .20, .26], anisotropy=0)
HITS = {'warm_hit': RED.emission, 'cool_hit': BLUE.emission}


def reference():
    samples = {}
    for label, pixel in witness_pixels().items():
        projective = witness(label, pixel, (0, 0))
        distance = math.dist(EYE, projective['point']) if projective['surface'] else None
        limit = FOG['max_distance_m']
        clipped = min(distance, limit) if distance is not None else limit
        path_length = max(0, clipped-FOG['start_distance_m'])
        transmittance = math.exp(-FOG['density_per_m']*path_length)
        samples[label] = dict(pixel=pixel, projective=projective, eye_distance_m=distance,
                              fog_length_m=path_length, transmittance=transmittance)
    for label in HITS:
        row = samples[label]
        assert .43 < row['transmittance'] < .62, row
        assert row['eye_distance_m'] > row['projective']['camera_depth_m']+.06, row
    assert samples['warm_target']['eye_distance_m'] > FOG['max_distance_m']
    assert samples['clear']['projective']['surface'] is None
    assert samples['clear']['transmittance'] == samples['warm_target']['transmittance']
    return samples


def fogged(radiance, transmittance):
    return tuple(ambient+(before-ambient)*transmittance
                 for before, ambient in zip(radiance, FOG['ambient_radiance'], strict=True))


class FogReflectionsCapture(ReflectionCapture):
    def export(self, label, revision, with_fog):
        value = dict(ambient=[.23]*3, exposure=EXPOSURE, screen_reflections=SETTINGS)
        if with_fog:
            value['height_fog'] = FOG
        revision = self.transact(revision, 8480+int(with_fog), component(ENVIRONMENT, 'LightingEnvironment', value))
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['revision'] == revision and info['needs_screen_reflections'] is True, info
        assert info.get('needs_height_fog', False) is with_fog, info
        original, relocated = self.root/(label+' export'), self.root/(label+' relocated')
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(original),
                                   '--runtime', self.native(self.args.runtime_root))))
        original.rename(relocated)
        assert not original.exists()
        game = relocated/'game.json'
        executable = relocated/json.loads(game.read_bytes())['engine']['executable']
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert inspected['needs_screen_reflections'] is True, inspected
        assert inspected.get('needs_height_fog', False) is with_fog, inspected
        self.measurements['bundles'][label] = dict(revision=revision, environment=value, inventory=tree_hashes(relocated))
        return revision, game

    def capture_pair_member(self, label, revision, game, with_fog):
        width, height = CAPTURE_SIZE
        path = self.root/(label+'.bmp')
        saves = self.root/(label+' saves')
        saves.mkdir()
        with self.owner(label, game=game, saves=saves) as (client, _):
            session = self.initialize(client, revision, None, saves, game)
            result = client.call('runtime.capture', dict(session_id=session, tick=0, structure_revision=0,
                camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu, samples=1,
                lighting_path='deferred', reconstruction='none', scene_debug_view='color', capture_frames=2,
                scene_product_probes=[dict(x=x, y=y) for x, y in (row['pixel'] for row in self.expected.values())],
                profile=True))
            self.measurements['reports'][label] = result
            assert result['hardware'] and result['capture_written'] and result['frames_presented'] == 2, result
            assert result['nvrhi_errors'] == 0 and result['tick'] == 0 and result['revision'] == revision, result
            assert (result['width'], result['height'], result['samples'], result['lens']) == (width, height, 1, LENS), result
            near_vector(result['camera_world'], camera_matrix(), tolerance=1e-8)
            diag = result['render_diagnostics']
            assert diag['lighting_path'] == 'deferred' and diag['reconstruction']['active'] is False, diag
            reflections = diag['screen_reflections']
            assert reflections['active'] is True and reflections['reason'] == 'active', reflections
            assert reflections['buffer_bytes'] == width*height*10, reflections
            if with_fog:
                fog = diag['height_fog']
                assert fog['active'] is True and fog['reason'] == 'active', fog
                assert fog['buffer_bytes'] == width*height*8 and fog['spot_lights'] == 0, fog
            else:
                assert 'height_fog' not in diag, diag
            products = diag['scene_products']
            assert products['available'] and products['view'] == 'color', products
            image = pixels(path)
            assert len(image) == height and all(len(row) == width for row in image)
            capture = dict(gpu=result['gpu'], sha256=file_hash(path), probes={})
            self.measurements['captures'][label] = capture
            for (name, expected), observed in zip(self.expected.items(), products['probes'], strict=True):
                x, y = expected['pixel']
                geometry = expected['projective']
                sample = capture['probes'][name] = dict(raster=observed, rgb=image[y][x])
                assert (observed['x'], observed['y'], observed['resolved_x'], observed['resolved_y']) == (x, y, x, y), sample
                assert observed['surface_valid'] is bool(geometry['surface']), sample
                if geometry['surface']:
                    near, far = LENS['near'], LENS['far']
                    depth = near/(1-observed['depth']*(far-near)/far)
                    assert abs(depth-geometry['camera_depth_m']) < .0003, sample
                    near_vector(observed['shading_normal'][:3], geometry['normal'], tolerance=.002)
                else:
                    assert observed['depth'] == 1, sample
                expected_rgb = [display(value, EXPOSURE) for value in observed['resolved_hdr']]
                assert max(abs(a-b) for a, b in zip(sample['rgb'], expected_rgb, strict=True)) <= 2, sample
        return capture

    def compare_pair(self, control, active):
        assert control['gpu'] == active['gpu'] and control['sha256'] != active['sha256']
        comparisons = self.measurements['comparison'] = {}
        for name, expected in self.expected.items():
            before, after = (capture['probes'][name]['raster'] for capture in (control, active))
            assert exact({k: v for k, v in before.items() if k != 'resolved_hdr'},
                         {k: v for k, v in after.items() if k != 'resolved_hdr'}), (name, before, after)
            reflected, opaque = before['resolved_hdr'], before['raw_hdr']
            t = expected['transmittance']
            desired = fogged(reflected, t)
            actual = after['resolved_hdr']
            near_vector(actual, desired, tolerance=.0018)
            assert math.dist(actual, reflected) > .055, (name, actual, reflected)
            row = comparisons[name] = dict(expected_fogged_hdr=desired, actual_fogged_hdr=actual,
                                            max_hdr_error=max(abs(a-b) for a, b in zip(actual, desired, strict=True)))
            if name in HITS:
                target = HITS[name]
                dominant = max(range(3), key=target.__getitem__)
                reflected_delta = tuple(a-b for a, b in zip(reflected, opaque, strict=True))
                gain = reflected_delta[dominant]/target[dominant]
                assert reflected_delta[dominant] > .08, (name, before)
                assert abs(gain-ideal_gain(expected['projective'])) < .005, (name, gain)
                near_vector(reflected_delta, tuple(value*gain for value in target), tolerance=.007)
                without_reflection = fogged(opaque, t)
                surviving = tuple(a-b for a, b in zip(actual, without_reflection, strict=True))
                near_vector(surviving, tuple(t*delta for delta in reflected_delta), tolerance=.0018)
                # Detect a skipped reflection and the inverse pass ordering.
                assert .045 < surviving[dominant] < reflected_delta[dominant]-.035, (name, surviving, reflected_delta)
                row.update(reflection_gain=gain, surviving_reflection_hdr=surviving)
            else:
                assert exact(reflected, opaque), (name, before)
                if name in ('warm_target', 'cool_target'):
                    near_vector(opaque, RED.emission if name == 'warm_target' else BLUE.emission, tolerance=.002)

    def run(self):
        self.expected = reference()
        self.measurements.update(reference=self.expected, trace_brackets=validate_reflection_reference(),
                                 bundles={}, reports={}, captures={})
        revision = self.author()
        control_revision, control = self.export('reflections-only', revision, False)
        active_revision, active = self.export('reflections-with-fog', control_revision, True)
        source = self.root/'Detached Authored Reflectors'
        self.project.rename(source)
        source_identity = tree_hashes(source)
        captures = []
        for label, revision, game, with_fog in (('reflections-only', control_revision, control, False),
                                                ('reflections-with-fog', active_revision, active, True)):
            assert not self.project.exists()
            captures.append(self.capture_pair_member(label, revision, game, with_fog))
            assert tree_hashes(game.parent) == self.measurements['bundles'][label]['inventory']
            assert tree_hashes(source) == source_identity
        self.measurements['detached_source'] = source_identity
        for call in self.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')), stderr
        self.compare_pair(*captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Run with assertions enabled and a GPU index between 0 and 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='fog-reflections-', dir=args.output.resolve()))
    test = FogReflectionsCapture(args, root)
    record = dict(format='poima.fog-reflections-capture.v1', passed=False, gpu=args.gpu,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
