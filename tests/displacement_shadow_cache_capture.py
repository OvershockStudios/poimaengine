#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare reused displaced shadows with the previous renderer's live redraw."""

import argparse
import json
from pathlib import Path
import traceback

from shadow_cache_capture import check_profiler
from snow_compaction_contract import (
    CAMERA, CAMERA_POSITION, REGULAR_DEPTH, REGULAR_RADIUS, TILES, TOP_DOWN, vector_near,
)
from snow_shadow_contract import LAMP, SnowShadowContract, lamp
from texture_fixture import png
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, uid


WIDTH, HEIGHT = 480, 270
SECOND_LIGHT = uid(804)


class ShadowFixture(SnowShadowContract):
    def setup(self):
        revision = self.add_test_light(self.author())
        doc = json.loads(self.world.read_bytes())
        source = self.root / 'Linear height.png'
        source.write_bytes(png(1, 1, [255, 255, 255, 255]))
        asset = self.world_call('asset.image.import', source=self.native(source), color_space='linear')['asset']
        source.unlink()
        height = dict(texture=asset, axis='y', tile_mm=2000, height_m=.002)
        operations = [
            component(LAMP, 'Light', lamp(True)),
            dict(op='entity.create', id=SECOND_LIGHT, name='Independent shadow light'),
            component(SECOND_LIGHT, 'Transform', transform([4, 1.05, .5], TOP_DOWN)),
            component(SECOND_LIGHT, 'Light', dict(kind='spot', color=[1, 1, 1], intensity=1, enabled=True,
                range=1.3, inner_angle=20, outer_angle=27,
                shadow=dict(enabled=True, near=.05, distance=1.3, bias=.0001, normal_bias=.001))),
        ]
        for tile in TILES:
            mesh = doc['entities'][tile]['components']['StaticMesh']
            operations.append(component(tile, 'StaticMesh', dict(mesh, displacement=height)))
        if self.args.rigid:
            operations.append(dict(op='snow.set', definition=None))
        result = self.world_call('world.transact', request_id=uid(805), base_revision=revision, ops=operations)
        assert result['committed'], result
        return result['revision']

    def run(self, current):
        revision = self.setup()
        saves = self.root / 'Save slots'
        saves.mkdir()
        capture_id = uid(806)
        frames = self.measurements['frames'] = {}
        session = uid(807)
        tick = 0
        camera_position = CAMERA_POSITION
        with self.owner('shadow reuse') as (client, _):
            self.initialize(client, revision, session, saves, None)
            if current:
                opened = client.call('profiler.start', dict(capture_id=capture_id, expected_capture_id=None,
                                                            capacity=16384, gpu_passes=True))
                assert opened['state'] == 'recording', opened

            def capture(label, reused, *, offscreen=False, new_renderer=False):
                # The probe list is part of the retained capture session key.
                probe_y = (48, 105, 254)
                image = self.root / (label + '.bmp')
                result = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=CAMERA,
                    path=self.native(image), width=WIDTH, height=HEIGHT, samples=1, gpu=self.args.gpu,
                    lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=1, reuse_renderer=True,
                    profile=True, scene_product_probes=[dict(x=x, y=y) for x in (219, 240, 261) for y in probe_y]), timeout=90)
                assert result['hardware'] and result['capture_written'] and result['frames_presented'] == 1, result
                assert result['tick'] == tick and result['nvrhi_errors'] == 0, result
                assert result['renderer_reused'] is (not new_renderer), result
                vector_near(result['camera_world'][12:15], camera_position, 1e-9)
                render = result['render_diagnostics']
                draws = render['last_draws']
                expected = reused if current else (0 if new_renderer else 1)
                assert draws['shadow_views'] == 2 and draws['shadow_reused_views'] == expected, draws
                assert (draws['shadow_batches'] == 0) is (expected == 2), draws
                assert render['gpu']['available'] and draws['displacement_requested'] == 2, render
                if offscreen:
                    assert draws['camera_draws'] == 1, draws
                if not self.args.rigid:
                    uploads = 0 if offscreen and current and reused == 2 else 2
                    assert draws['streamed_draws'] == uploads, draws
                probes = render['scene_products']['probes']
                assert any(row['surface_valid'] for row in probes), probes
                frames[label] = dict(sha256=file_hash(image), report=result)

            def move(position, request):
                nonlocal tick, camera_position
                delta = [value - base for value, base in zip(position, CAMERA_POSITION, strict=True)]
                offset = [delta[0], -delta[2], delta[1]]
                self.step(client, session, tick, request, camera_poses=[dict(entity=CAMERA,
                    world=dict(position=offset, rotation=[0, 0, 0, 1]))])
                tick += 1
                camera_position = position

            capture('cold', 0, new_renderer=True)
            for i in range(3):
                capture(f'warm-{i}', 2)
            if not self.args.rigid:
                stamp = self.stamp(client, session, tick, 0, REGULAR_RADIUS, REGULAR_DEPTH)
                self.update(client, session, tick, 810, stamp)
                tick += 1
                capture('stamped', 1)
                capture('stamped-warm', 2)
                assert frames['stamped']['sha256'] != frames['cold']['sha256']
            move([CAMERA_POSITION[0] + .11, *CAMERA_POSITION[1:]], 811)
            capture('camera-moved', 1)
            capture('camera-warm', 2)
            move([1, 1.5, -.76], 812)
            capture('offscreen-casters', 1, offscreen=True)
            capture('offscreen-warm', 2, offscreen=True)
            assert frames['offscreen-casters']['sha256'] == frames['offscreen-warm']['sha256']
            assert client.call('runtime.capture.release', dict(session_id=session))['released']
            capture('reopened', 0, offscreen=True, new_renderer=True)
            capture('reopened-warm', 2, offscreen=True)
            assert frames['offscreen-casters']['sha256'] == frames['reopened']['sha256']
            if current:
                self.measurements['profiled_submissions'] = check_profiler(client, capture_id, frames)
                for label, frame in frames.items():
                    values = frame['profile_views']
                    local_dirty = label in ('cold', 'stamped', 'camera-moved', 'offscreen-casters', 'reopened')
                    const_dirty = label in ('cold', 'reopened')
                    assert values['renderer.shadow_view.0.reused'] == int(not local_dirty), (label, values)
                    assert values['renderer.shadow_view.1.reused'] == int(not const_dirty), (label, values)
            client.call('runtime.capture.release', dict(session_id=session))
            client.call('runtime.stop', dict(session_id=session))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--rigid', action='store_true')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Use assertions and a GPU index from 0 to 4095.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = dict(passed=False, shader_path=args.lighting_path, rigid=args.rigid, harness_sha256=file_hash(Path(__file__)))
    try:
        for label, binary in (('reference', args.reference), ('current', args.binary)):
            root = args.output / label
            root.mkdir()
            options = argparse.Namespace(**(vars(args) | dict(binary=binary.resolve(strict=True))))
            fixture = ShadowFixture(options, root)
            evidence[label] = dict(binary_sha256=file_hash(options.binary), calls=fixture.calls, measurements=fixture.measurements)
            fixture.run(current=label == 'current')
        current = evidence['current']['measurements']['frames']
        reference = evidence['reference']['measurements']['frames']
        assert current.keys() == reference.keys()
        for label in current:
            assert current[label]['sha256'] == reference[label]['sha256'], label
            a, b = (cohort[label]['report']['render_diagnostics']['scene_products']['probes'] for cohort in (current, reference))
            assert a == b, label
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = args.output / 'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
