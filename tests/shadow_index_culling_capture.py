#!/usr/bin/env python3
"""Qualify shadow index-range culling against a fully submitted imported mesh."""
# SPDX-License-Identifier: Apache-2.0

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import traceback
import uuid

from gltf_fixture import glb
from scene_capture import pixels
from texture_fixture import quad


TRIANGLES_PER_BLOCK = 1024
WIDTH, HEIGHT = 640, 360
CAMERA, LIGHT, ENVIRONMENT, RECEIVER, CASTER = (f'{value:032x}' for value in range(1, 6))


def file_sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def grouped_mesh():
    """Four contiguous index blocks, with only the second inside the spot cone."""
    vertices, indices = [], []
    for center_x in (-35, 1, 35, 60):
        first = len(vertices)
        for x, y, u, v in ((-.4, -.4, 0, 1), (.4, -.4, 1, 1), (.4, .4, 1, 0), (-.4, .4, 0, 0)):
            vertices.append((center_x + x, y, 2, 0, 0, 1, u, v))
        pair = [first + offset for offset in (0, 1, 2, 0, 2, 3)]
        indices.extend(pair * (TRIANGLES_PER_BLOCK // 2))

    vertex_data = b''.join(struct.pack('<8f', *vertex) for vertex in vertices)
    index_data = struct.pack(f'<{len(indices)}I', *indices)
    document = {
        'asset': {'version': '2.0', 'generator': 'Poima shadow index regression fixture'},
        'buffers': [{'byteLength': len(vertex_data) + len(index_data)}],
        'bufferViews': [
            {'buffer': 0, 'byteLength': len(vertex_data), 'byteStride': 32, 'target': 34962},
            {'buffer': 0, 'byteOffset': len(vertex_data), 'byteLength': len(index_data), 'target': 34963},
        ],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC3',
             'min': [min(vertex[i] for vertex in vertices) for i in range(3)],
             'max': [max(vertex[i] for vertex in vertices) for i in range(3)]},
            {'bufferView': 0, 'byteOffset': 12, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC3'},
            {'bufferView': 0, 'byteOffset': 24, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC2'},
            {'bufferView': 1, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'},
        ],
        'materials': [{'doubleSided': True, 'pbrMetallicRoughness': {'baseColorFactor': [.2, .2, .2, 1],
                                                                    'metallicFactor': 0, 'roughnessFactor': .6}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
                                    'indices': 3, 'material': 0}]}],
        'nodes': [{'name': 'One grouped original mesh', 'mesh': 0}],
        'scenes': [{'nodes': [0]}],
        'scene': 0,
    }
    return glb(document, vertex_data + index_data)


def create(entity, name):
    return dict(op='entity.create', id=entity, name=name)


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def transform(entity, position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return component(entity, 'Transform', dict(position=list(position), rotation=list(rotation), scale=list(scale)))


def mesh(entity, asset, cast_shadows):
    return component(entity, 'StaticMesh', dict(asset=asset, primitive=0, visible=True, cast_shadows=cast_shadows))


class Fixture:
    def __init__(self, args, evidence):
        self.args = args
        self.evidence = evidence
        self.world = args.output / 'shadow-index-culling.world.json'
        self.revision = 0

    def native(self, path):
        absolute = str(path.resolve())
        if self.args.windows_interop:
            return subprocess.check_output(['wslpath', '-aw', absolute], text=True).strip()
        return absolute

    def call(self, requests):
        rows = [dict(jsonrpc='2.0', id=index, method=method, params=params)
                for index, (method, params) in enumerate(requests, 1)]
        receipt = dict(requests=rows)
        self.evidence['runs'].append(receipt)
        command = [str(self.args.binary), 'world', self.native(self.world)]
        process = subprocess.run(command, input=''.join(json.dumps(row) + '\n' for row in rows),
                                 capture_output=True, encoding='utf-8', timeout=120)
        receipt.update(command=command, exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr)
        assert process.returncode == 0, receipt
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        assert [reply['id'] for reply in replies] == list(range(1, len(rows) + 1)), replies
        assert all('result' in reply for reply in replies), replies
        return [reply['result'] for reply in replies]

    def edit(self, ops):
        [result] = self.call([('world.transact', dict(request_id=uuid.uuid4().hex,
                                                      base_revision=self.revision, ops=ops))])
        assert result['committed'] and not result['replayed'] and result['revision'] == self.revision + 1, result
        self.revision = result['revision']

    def capture(self, name, culling):
        sample_count = 1 if self.args.lighting_path == 'deferred' else 4
        return 'world.capture', dict(camera=CAMERA, path=self.native(self.args.output / f'{name}.bmp'),
            revision=self.revision, gpu=self.args.gpu, width=WIDTH, height=HEIGHT, samples=sample_count,
            lighting_path=self.args.lighting_path, culling=culling, profile=False)

    def audit(self, result, culling, expected):
        diagnostics = result['render_diagnostics']
        draws = diagnostics['last_draws']
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, result
        assert result['frames_presented'] == diagnostics['completed_submissions'] == 2, result
        assert diagnostics['lighting_path'] == self.args.lighting_path and diagnostics['culling'] == culling, diagnostics
        assert {key: draws[key] for key in expected} == expected, draws
        return draws

    def run(self):
        receiver_source = self.args.output / 'receiver.glb'
        receiver_source.write_bytes(glb(*quad([], {'pbrMetallicRoughness': {'baseColorFactor': [.6, .2, .05, 1],
                                                                          'metallicFactor': 0, 'roughnessFactor': .6}})))
        caster_source = self.args.output / 'four-shadow-blocks.glb'
        caster_source.write_bytes(grouped_mesh())
        receiver, caster = (result['asset'] for result in self.call([
            ('asset.import', {'source': self.native(path)}) for path in (receiver_source, caster_source)
        ]))
        self.evidence['assets'] = {receiver: file_sha(receiver_source), caster: file_sha(caster_source)}
        half_angle = .5 * math.atan2(2, 4)
        self.edit([
            create(CAMERA, 'Shadow comparison camera'),
            transform(CAMERA, (0, 0, 6)),
            component(CAMERA, 'Camera', dict(vertical_fov=60, near=.1, far=100)),
            create(LIGHT, 'Single spot shadow view'),
            transform(LIGHT, (2, 0, 4), (0, math.sin(half_angle), 0, math.cos(half_angle))),
            component(LIGHT, 'Light', dict(kind='spot', color=[1, 1, 1], intensity=20 * math.pi,
                enabled=True, inner_angle=10, outer_angle=28,
                shadow=dict(enabled=True, distance=30, bias=.0001, normal_bias=.005))),
            create(ENVIRONMENT, 'Ambient reference'),
            component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[.02] * 3, exposure=1, shadow_resolution=1024)),
            create(RECEIVER, 'Non-casting receiver'),
            transform(RECEIVER, scale=(4, 4, 4)),
            mesh(RECEIVER, receiver, False),
            create(CASTER, 'Single dense rigid caster'),
            mesh(CASTER, caster, True),
        ])
        source_hash = file_sha(self.world)
        full, culled = self.call([self.capture('full-indices', False), self.capture('culled-indices', True)])
        assert file_sha(self.world) == source_hash
        base = dict(shadow_views=1, shadow_candidates=1, shadow_draws=1, shadow_culled=0, shadow_batches=1)
        full_counts = self.audit(full, False, base | dict(shadow_triangles=4 * TRIANGLES_PER_BLOCK))
        culled_counts = self.audit(culled, True, base | dict(shadow_triangles=TRIANGLES_PER_BLOCK))
        assert full_counts['camera_triangles'] == culled_counts['camera_triangles'] == 4 * TRIANGLES_PER_BLOCK + 2

        self.edit([mesh(CASTER, caster, False)])
        [cast_disabled] = self.call([self.capture('cast-disabled', True)])
        disabled_counts = self.audit(cast_disabled, True,
            dict(shadow_views=1, shadow_candidates=0, shadow_draws=0, shadow_culled=0, shadow_batches=0, shadow_triangles=0))
        assert disabled_counts['camera_triangles'] == full_counts['camera_triangles']
        assert full['gpu'] == culled['gpu'] == cast_disabled['gpu']
        paths = {name: self.args.output / f'{name}.bmp' for name in ('full-indices', 'culled-indices', 'cast-disabled')}
        captures = {name: file_sha(path) for name, path in paths.items()}
        assert captures['full-indices'] == captures['culled-indices'], captures
        shadow_rgb = pixels(paths['culled-indices'])[HEIGHT // 2][WIDTH // 2]
        disabled_rgb = pixels(paths['cast-disabled'])[HEIGHT // 2][WIDTH // 2]
        assert disabled_rgb[0] - shadow_rgb[0] >= 30, (shadow_rgb, disabled_rgb)
        self.evidence.update(device=full['gpu'], build_version=full['build_version'], captures=captures,
            shadow_rgb=shadow_rgb, cast_disabled_rgb=disabled_rgb,
            draw_counts=dict(full=full_counts, culled=culled_counts, cast_disabled=disabled_counts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.binary = args.binary.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    evidence = dict(passed=False, binary_sha256=file_sha(args.binary), fixture_sha256=file_sha(Path(__file__)),
                    gpu_index=args.gpu, lighting_path=args.lighting_path, runs=[])
    try:
        Fixture(args, evidence).run()
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        (args.output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: evidence[key] for key in ('passed', 'device', 'build_version', 'captures', 'shadow_rgb',
                                                   'cast_disabled_rgb')}, indent=2))


if __name__ == '__main__':
    main()
