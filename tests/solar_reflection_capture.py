#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check nocturnal screen-hit radiance with solar lighting and enabled sky IBL."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from screen_reflections_capture import (
    BLUE, CAMERA, CAPTURE_SIZE, EYE, LENS, PITCH, RED, SETTINGS, SURFACES,
    camera_matrix, surface_components, validate_reference, witness, witness_pixels,
)
from sky_capture import display
from solar_lighting_contract import EXPOSURE, SKY_COLOR, SolarContract, VALIDATION_FAILURES, VALIDATION_LAYER
from world_particles_contract import component, near_vector, transform
from world_terrain_contract import file_hash, uid


SPECULAR_IBL = dict(enabled=True, diffuse_intensity=0, specular_intensity=1)


class SolarReflectionCapture(SolarContract):
    def author(self):
        super().author()
        self.camera = CAMERA
        environment = self.authored_environment | dict(ibl=SPECULAR_IBL, screen_reflections=SETTINGS)
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            component(self.environment, 'LightingEnvironment', environment),
            dict(op='entity.create', id=CAMERA, name='Solar reflection probe camera'),
            component(CAMERA, 'Transform', transform(list(EYE), [math.sin(PITCH/2), 0, 0, math.cos(PITCH/2)])),
            component(CAMERA, 'Camera', LENS),
        ]
        for surface in SURFACES:
            ops.extend(surface_components(surface))
        result = self.world_call('world.transact', request_id=uid(6600), base_revision=self.revision, ops=ops)
        assert result['committed'] and result['revision'] == self.revision+1, result
        self.revision = result['revision']
        self.measurements['reflection_geometry'] = validate_reference()

    def capture_probes(self, client, session, state, label):
        tick = state['lighting']['tick']
        coordinates = witness_pixels()
        output = self.root / f'{label}.bmp'
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, width=width, height=height, path=self.native(output), gpu=self.args.gpu,
            samples=1, lighting_path='deferred', reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in coordinates.values()]))
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors'] == 0, report
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (
            width, height, 1, 2), report
        assert report['revision'] == self.revision and report['tick'] == tick and report['lens'] == LENS, report
        near_vector(report['camera_world'], camera_matrix(), tolerance=1e-8)
        sample = state['lighting']['solar_lighting']
        assert exact(report['lighting']['solar_lighting'], sample), report['lighting']
        assert report['lighting']['ibl'] == SPECULAR_IBL, report['lighting']
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == 'deferred' and diagnostics['environment_ibl']['enabled'], diagnostics
        assert diagnostics['screen_reflections']['active'] is True, diagnostics
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        bitmap = pixels(output)
        assert len(bitmap) == height and all(len(row) == width for row in bitmap)
        observed = {}
        for (name, (x, y)), probe in zip(coordinates.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            geometry = witness(name, (x, y), (0, 0))
            assert probe['surface_valid'] is bool(geometry['surface']), (name, probe, geometry)
            if geometry['surface']:
                hit = client.call('runtime.raycast', dict(session_id=session, tick=tick,
                    origin=list(EYE), direction=list(geometry['ray']), distance=LENS['far']))['hit']
                assert hit['entity'] == geometry['surface'], (name, hit, geometry)
                near_vector(hit['position'], geometry['point'], tolerance=.0001)
                near_vector(probe['shading_normal'][:3], geometry['normal'], tolerance=.002)
            if name not in ('warm_hit', 'cool_hit'):
                assert exact(probe['raw_hdr'], probe['resolved_hdr']), (name, probe)
            expected_rgb = [display(value * EXPOSURE) for value in probe['resolved_hdr']]
            assert max(abs(a-b) for a, b in zip(bitmap[y][x], expected_rgb, strict=True)) <= 2, (name, probe)
            observed[name] = dict(geometry=geometry, raster=probe, rgb=bitmap[y][x])
        near_vector(observed['clear']['raster']['raw_hdr'],
                    [value * sample['sky_gain'] for value in SKY_COLOR], tolerance=.001)
        self.measurements['captures'][label] = dict(path=str(output), sha256=file_hash(output),
            report=report, probes=observed)
        return observed

    def exercise(self, game):
        saves = self.root / 'External Solar Reflection Saves'
        saves.mkdir()
        with self.owner('relocated solar reflection player', game=game, saves=saves) as (client, _):
            session = self.start(client, saves, 6610, game)
            self.advance(client, session, 0, 6611, ticks=2)
            noon = self.state(client, session, 2)
            daytime = self.capture_probes(client, session, noon, 'noon')
            self.advance(client, session, 2, 6612, ticks=3)
            midnight = self.state(client, session, 5)
            nighttime = self.capture_probes(client, session, midnight, 'midnight')
        gain = midnight['lighting']['solar_lighting']['sky_gain']
        assert noon['lighting']['solar_lighting']['sky_gain'] == 1 and gain == .0625

        comparisons = {}
        for label, target in (('warm', RED), ('cool', BLUE)):
            receiver, source = label+'_hit', label+'_target'
            channel = max(range(3), key=target.emission.__getitem__)
            rows = []
            for observed, sky_gain in ((daytime, 1), (nighttime, gain)):
                geometry = observed[receiver]['geometry']
                assert geometry['reflected_hit'] == target.entity, geometry
                probe = observed[receiver]['raster']
                local = observed[source]['raster']['raw_hdr'][channel]
                sky = SKY_COLOR[channel] * sky_gain
                delta = probe['resolved_hdr'][channel] - probe['raw_hdr'][channel]
                assert local > .95 and local-sky > .2 and delta > .025, (label, sky_gain, local, sky, delta)
                rows.append(dict(sky_gain=sky_gain, local_hdr=local, fallback_hdr=sky,
                                 delta_hdr=delta, response=delta/(local-sky)))
            # Geometry, roughness and specular gain are fixed. The reflection
            # response after removing the change in sky fallback is conserved.
            # Adjacent samples of the same emissive plane can differ slightly.
            ratio = rows[1]['response']/rows[0]['response']
            assert .8 < ratio < 1.2, (label, rows, ratio)
            comparisons[label] = dict(response_ratio=ratio, phases=rows)
        self.measurements.update(reflection_response=comparisons, states=dict(noon=noon, midnight=midnight))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--native-descriptor', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--require-validation', action='store_true')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Enable assertions and supply a GPU index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='solar-reflections-', dir=args.output.resolve()))
    test = SolarReflectionCapture(args, root)
    evidence = dict(format='poima.solar-reflection-capture.v1', passed=False,
        gpu=args.gpu, require_validation=args.require_validation, binary_sha256=file_hash(args.binary),
        harness_sha256=file_hash(Path(__file__)), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        for call in test.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert not any(marker in stderr for marker in VALIDATION_FAILURES), stderr
            if args.require_validation and call.get('label') == 'relocated solar reflection player':
                assert VALIDATION_LAYER in stderr, stderr
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
