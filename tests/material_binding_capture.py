#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check ordered instancing for separately authored material texture overrides."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from gltf_fixture import glb
from rigid_mesh_lod_capture import Fixture, WorldClient, component, digest, transform, uid
from solar_lighting_contract import VALIDATION_FAILURES, VALIDATION_LAYER
from texture_fixture import png, quad


CAMERA, SUN, RECEIVER = map(uid, (10, 11, 12))
SIZE = (960, 540)
ROWS, COLUMNS = 32, 32
COUNT = ROWS * COLUMNS


class MaterialBindingFixture(Fixture):
    def image(self, client, name, data, color_space):
        source = self.root / (name + '.png')
        source.write_bytes(png(8, 8, data))
        image = self.call(client, 'asset.image.import', source=self.native(source), color_space=color_space)
        source.unlink()
        return image['asset']

    def author(self, client):
        source = self.root / 'Original plant.glb'
        source.write_bytes(glb(*quad([], material=dict(doubleSided=True))))
        mesh = self.call(client, 'asset.import', source=self.native(source))['asset']
        source.unlink()
        textures = []
        for name, rgb in (('Green', (50, 190, 35)), ('Rust', (198, 90, 20))):
            data = bytes(v for y in range(8) for x in range(8)
                         for v in (*rgb, 255 if (x + 2*y) % 5 < 3 else 0))
            raw = self.image(client, name, data, 'srgb')
            textures.append(self.call(client, 'asset.image.cutout', asset=raw, alpha_threshold=128)['asset'])
        normal = self.image(client, 'Tilted normal', bytes((186, 110, 235, 255))*64, 'linear')
        angle = math.radians(-15)
        ops = [dict(op='entity.create', id=entity, name=name) for entity, name in
               ((CAMERA, 'Survey camera'), (SUN, 'Daylight'), (RECEIVER, 'Shadow receiver'))]
        ops += [component(CAMERA, 'Transform', transform()),
                component(CAMERA, 'Camera', dict(near=.1, far=35, vertical_fov=60)),
                component(SUN, 'Transform', transform() | dict(rotation=[math.sin(angle), 0, 0, math.cos(angle)])),
                component(SUN, 'Light', dict(kind='directional', color=[1]*3, intensity=2, enabled=True,
                    shadow=dict(enabled=True, distance=24, bias=.0005, normal_bias=.02))),
                component(RECEIVER, 'Transform', transform((0, 0, -9.35)) | dict(scale=[12, 10, .1])),
                component(RECEIVER, 'MeshRenderer', dict(primitive='box', visible=True, albedo=[.4]*3,
                                                        cast_shadows=False, receive_shadows=True))]
        self.revision = self.transaction(client, 0, ops)['revision']
        material = dict(base_color=[1]*3, emissive=[0]*3, roughness=.65, metallic=0, double_sided=True,
                        alpha_mode='mask', alpha_cutoff=.5)
        for start in range(0, COUNT, 32):
            operations = []
            for index in range(start, start + 32):
                identity = uid(100 + index)
                group = index // (COUNT // 4)
                row, column = divmod(index, COLUMNS)
                x, y = .25*(column-(COLUMNS-1)/2), .25*(row-(ROWS-1)/2)
                filtered = group == 2
                base = dict(asset=textures[group == 3], wrap_s=33071, wrap_t=33071,
                            min_filter=9987 if filtered else 9984, mag_filter=9729 if filtered else 9728)
                maps = dict(base_color=base, normal_scale=.3+.25*(index % 7))
                if group == 1:
                    maps['normal'] = dict(asset=normal)
                operations.extend((
                    dict(op='entity.create', id=identity, name=f'Plant {index}'),
                    component(identity, 'Transform', transform((x, y, -8)) | dict(scale=[.11, .105, .11])),
                    component(identity, 'StaticMesh', dict(asset=mesh, primitive=0, visible=True, cast_shadows=True,
                        foliage_motion=dict(root_y=-1, tip_y=1,
                            wind=dict(direction=[1, .25], tip_m=.05, period_ticks=60, wavelength_m=8)))),
                    component(identity, 'PbrMaterial', material),
                    component(identity, 'PbrTextures', maps)))
            self.revision = self.transaction(client, self.revision, operations)['revision']

    def capture(self, client, label, instanced):
        path = self.root / (label + '.bmp')
        result = self.call(client, 'world.capture', revision=self.revision, camera=CAMERA,
                           path=self.native(path), width=SIZE[0], height=SIZE[1], samples=1,
                           gpu=self.args.gpu, lighting_path=self.args.lighting_path,
                           draw_instancing=instanced, capture_frames=1)
        assert result['hardware'] and result['capture_written'] and result['frames_presented'] == 1, result
        assert result['nvrhi_errors'] == 0
        diagnostics = result['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path
        self.evidence['captures'][label] = dict(image_sha256=digest(path), receipt=result)
        return diagnostics['last_draws']

    def run(self):
        counts = {}
        for label, executable in (('prior', self.args.reference), ('candidate', self.args.binary)):
            path = self.root / label
            with WorldClient.open(str(executable.resolve()), self.native(self.world), trace_directory=path) as client:
                if label == 'prior':
                    self.author(client)
                else:
                    counts['unbatched'] = self.capture(client, 'unbatched', False)
                counts[label] = self.capture(client, label, True)
                assert client.call('session.close')['closed']
            logs = (path / 'stderr.log').read_text(encoding='utf-8')
            assert VALIDATION_LAYER in logs and all(marker not in logs for marker in VALIDATION_FAILURES), logs

        images = {capture['image_sha256'] for capture in self.evidence['captures'].values()}
        assert len(images) == 1, images
        left, right = counts['prior'], counts['candidate']
        assert left['camera_draws'] == COUNT + 1 and left['shadow_draws'] >= COUNT, left
        for field in ('camera_draws', 'camera_culled', 'camera_triangles', 'shadow_draws', 'shadow_triangles'):
            assert left[field] == right[field] == counts['unbatched'][field], (field, counts)
        assert left['camera_batches'] == COUNT + 1 and right['camera_batches'] == 5, (left, right)
        assert right['shadow_batches'] < left['shadow_batches'] / 4, (left, right)
        self.evidence['counts'] = counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--gpu', required=True, type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='material-bindings-', dir=args.output.resolve()))
    report = dict(format='poima.material-bindings.v1', passed=False, gpu=args.gpu, lighting_path=args.lighting_path,
                  binary_sha256=digest(args.binary), reference_sha256=digest(args.reference),
                  harness_sha256=digest(Path(__file__)), captures={}, requests=[])
    try:
        MaterialBindingFixture(args, root, report).run()
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
