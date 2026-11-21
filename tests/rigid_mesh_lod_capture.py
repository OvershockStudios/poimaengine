#!/usr/bin/env python3
"""Verify authored rigid LODs and optionally capture Vulkan output from a relocated export."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import traceback
import uuid

from gltf_fixture import glb
from scene_capture import pixels
from texture_fixture import png

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import RpcError, WorldClient


def uid(number):
    return f'{number:032x}'


OBJECT, LIGHT, ENVIRONMENT, PLAYER, TEMPLATE = map(uid, (1, 2, 3, 4, 5))
CAMERAS = [('near', uid(10), 2, 0), ('threshold', uid(11), 5, 1),
           ('middle', uid(12), 7, 1), ('far', uid(13), 10, 2)]
SIZE, FOV = 256, 60
BASE_POINTS = [(-1, -1), (1, -1), (1, 1), (-1, 1), (0, 0)]
BASE_INDICES = [0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]


def transform(position=(0, 0, 0)):
    return dict(position=list(position), rotation=[0, 0, 0, 1], scale=[1, 1, 1])


def component(identity, kind, value):
    return dict(op='component.set', id=identity, type=kind, value=value)


def original_primitive(name, points, indices, channel, uvs=False):
    vertices = struct.pack('<' + 'f' * 6 * len(points), *[
        value for x, y in points for value in (x, y, 0, 0, 0, 1)])
    elements = struct.pack('<' + 'I' * len(indices), *indices)
    blob = vertices + elements
    views = [dict(buffer=0, byteLength=len(vertices), byteStride=24),
             dict(buffer=0, byteOffset=len(vertices), byteLength=len(elements))]
    accessors = [
        dict(bufferView=0, componentType=5126, count=len(points), type='VEC3',
             min=[min(x for x, y in points), min(y for x, y in points), 0],
             max=[max(x for x, y in points), max(y for x, y in points), 0]),
        dict(bufferView=0, byteOffset=12, componentType=5126, count=len(points), type='VEC3'),
        dict(bufferView=1, componentType=5125, count=len(indices), type='SCALAR'),
    ]
    attributes = dict(POSITION=0, NORMAL=1)
    if uvs:
        texcoords = struct.pack('<' + 'f' * 2 * len(points), *[
            value for x, y in points for value in ((x+1)/2, (y+1)/2)])
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(texcoords)))
        accessors.append(dict(bufferView=2, componentType=5126, count=len(points), type='VEC2'))
        attributes['TEXCOORD_0'] = 3
        blob += texcoords
    color = [int(i == channel) for i in range(3)]
    document = dict(asset=dict(version='2.0'), buffers=[dict(byteLength=len(blob))],
                    bufferViews=views, accessors=accessors,
                    meshes=[dict(primitives=[dict(attributes=attributes, indices=2, material=0)])],
                    materials=[dict(emissiveFactor=color,
                                    pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0))],
                    nodes=[dict(name=name, mesh=0)], scenes=[dict(nodes=[0])], scene=0)
    return glb(document, blob)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {path.relative_to(root).as_posix(): digest(path) for path in sorted(root.rglob('*')) if path.is_file()}


class Fixture:
    def __init__(self, args, root, evidence):
        self.args, self.root, self.evidence = args, root, evidence
        self.project = root / 'LOD Project'
        self.project.mkdir()
        self.world = self.project / 'scene.json'
        self.assets = self.project / 'scene.json.assets'
        self.manifest = self.project / 'project.poimaproject'
        self.revision = 1

    def native(self, path):
        resolved = str(path.resolve())
        if self.args.windows_interop:
            return subprocess.check_output(['wslpath', '-w', resolved], text=True, timeout=10).strip()
        return resolved

    def call(self, client, method, error=None, **params):
        record = dict(method=method, params=params)
        self.evidence['requests'].append(record)
        try:
            result = client.call(method, params, timeout=90)
        except RpcError as failure:
            record['error'] = dict(code=failure.code, message=failure.message)
            assert failure.code == error, record
            return record['error']
        record['result'] = result
        assert error is None, record
        return result

    def transaction(self, owner, revision, operations, error=None):
        return self.call(owner, 'world.transact', error=error, request_id=uuid.uuid4().hex,
                         base_revision=revision, ops=operations)

    def imported(self, owner, name, points, indices, channel, uvs=False):
        source = self.project / (name + '.glb')
        source.write_bytes(original_primitive(name, points, indices, channel, uvs))
        return self.call(owner, 'asset.import', source=self.native(source))['asset']

    def author(self, owner):
        # Alternative primitives deliberately have green/blue material and visibly
        # narrower silhouettes. The world uses base red and unchanged collision.
        base = self.imported(owner, 'Base', BASE_POINTS, BASE_INDICES, 0, uvs=True)
        mid = self.imported(owner, 'Middle', [(-.55, -.55), (.55, -.55), (.55, .55), (-.55, .55)],
                            [0, 1, 2, 0, 2, 3], 1)
        far = self.imported(owner, 'Far', [(-.3, -.3), (.3, -.3), (0, .45)], [0, 1, 2], 2)
        template = self.imported(owner, 'Template', [(-.35, -.3), (.3, -.3), (0, .45)], [0, 1, 2], 2)
        unused = self.imported(owner, 'Unused', [(-.45, -.3), (.3, -.3), (0, .45)], [0, 1, 2], 2)
        self.expected = {base, mid, far, template}

        lods = [dict(asset=mid, primitive=0, min_distance_m=5),
                dict(asset=far, primitive=0, min_distance_m=9)]
        chain = dict(asset=base, primitive=0, visible=True, lods=lods)
        operations = [dict(op='entity.create', id=OBJECT, name='LOD witness'),
                      component(OBJECT, 'Transform', transform((0, 0, -10))),
                      component(OBJECT, 'StaticMesh', chain),
                      component(OBJECT, 'MeshCollider', dict(asset=base, primitive=0, friction=.7, restitution=0)),
                      dict(op='entity.create', id=LIGHT, name='Sun'),
                      component(LIGHT, 'Light', dict(kind='directional', color=[1, 0, 0], intensity=2, enabled=True,
                                                      shadow=dict(enabled=True, distance=25, bias=.0001, normal_bias=.005))),
                      dict(op='entity.create', id=ENVIRONMENT, name='Ambient'),
                      component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1)),
                      dict(op='entity.create', id=PLAYER, name='Player'),
                      component(PLAYER, 'CharacterController', dict(radius=.3, height=1.8, speed=4,
                                                                     jump_speed=5, camera=CAMERAS[-1][1]))]
        for label, camera, distance, _ in CAMERAS:
            operations += [dict(op='entity.create', id=camera, name=label,
                                parent=PLAYER if label == 'far' else None),
                           component(camera, 'Transform', transform((0, 0, distance-10))),
                           component(camera, 'Camera', dict(vertical_fov=FOV, near=.1, far=50))]
        operations.append(dict(op='template.set', id=TEMPLATE, name='Template-only LOD',
                               components={'Transform': transform(), 'StaticMesh': dict(
                                   chain, lods=[dict(asset=template, primitive=0, min_distance_m=4)])}))
        assert self.transaction(owner, 0, operations)['revision'] == self.revision

        discovery = self.call(owner, 'world.describe', view='component', name='StaticMesh')
        assert discovery['components']['StaticMesh']['properties']['lods']['maxItems'] == 4, discovery
        skinned = self.call(owner, 'world.describe', view='component', name='SkinnedMesh')
        assert 'lods' not in skinned['components']['SkinnedMesh']['properties'], skinned

        expected_paths = ['/asset', '/asset', '/lods/0/asset', '/lods/1/asset']
        selector = dict(kind='entity', id=OBJECT)
        page = self.call(owner, 'world.asset.references', owner=selector, limit=1)
        pages = [page['edges'][0]]
        while page['next_after'] is not None:
            assert len(pages) < len(expected_paths), pages
            page = self.call(owner, 'world.asset.references', owner=selector, revision=self.revision,
                             after=page['next_after'], limit=1)
            pages.extend(page['edges'])
        assert [row['path'] for row in pages] == expected_paths, pages
        assert [row['asset'] for row in pages] == [base, base, mid, far], pages
        assert all(row['subresource'] == dict(kind='primitive', index=0) for row in pages), pages
        template_edges = self.call(owner, 'world.asset.references', owner=dict(kind='template', id=TEMPLATE))['edges']
        assert [row['asset'] for row in template_edges] == [base, template], template_edges
        dependencies = self.call(owner, 'world.dependencies')
        resolved = {row['sha256'] for row in dependencies['assets']}
        assert resolved == self.expected and unused not in resolved, dependencies
        assert dependencies['needs_mesh_lod'] is True, dependencies

        baseline = self.world.read_bytes()
        for code, change in [
                (-32602, lambda levels: levels[1].update(min_distance_m=5)),
                (-32602, lambda levels: levels[0].update(min_distance_m=True)),
                (-32050, lambda levels: levels[0].update(asset=base)),
                (-32050, lambda levels: levels[0].update(primitive=1)),
                (-32050, lambda levels: levels[1].update(asset='f'*64))]:
            invalid = deepcopy(chain)
            change(invalid['lods'])
            self.transaction(owner, self.revision, [component(OBJECT, 'StaticMesh', invalid)], error=code)
            assert self.world.read_bytes() == baseline, code

        texture = self.project / 'Base map.png'
        texture.write_bytes(png(1, 1, [255, 255, 255, 255]))
        image = self.call(owner, 'asset.image.import', source=self.native(texture), color_space='srgb')['asset']
        self.transaction(owner, self.revision, [component(OBJECT, 'PbrTextures', dict(base_color=dict(asset=image)))],
                         error=-32050)
        assert self.world.read_bytes() == baseline

        # Provenance stays available when a cooked alternative goes missing.
        resource = self.assets / (far + '.pmodel')
        backup = resource.with_suffix('.retained')
        resource.rename(backup)
        try:
            edges = self.call(owner, 'world.asset.references', asset=far)['edges']
            assert len(edges) == 1 and edges[0]['path'] == '/lods/1/asset', edges
            self.call(owner, 'world.dependencies', error=-32050)
        finally:
            backup.rename(resource)

    def capture(self, owner, case, session=None, label='authored'):
        name, camera, distance, tier = case
        output = self.root / f'{label}-{name}.bmp'
        focal = SIZE/(2*math.tan(math.radians(FOV)/2))
        fringe = int(SIZE/2 + focal*.8/distance)
        width = height = SIZE
        guards = dict(session_id=session, tick=0) if session else dict(revision=self.revision)
        report = self.call(owner, 'runtime.capture' if session else 'world.capture', **guards,
                           camera=camera, path=self.native(output), width=width, height=height,
                           samples=1, culling=False, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
                           scene_product_probes=[dict(x=SIZE//2, y=SIZE//2), dict(x=fringe, y=SIZE//2)])
        assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        counts = diagnostics['last_draws']
        assert counts['mesh_lod']['tier_counts'] == [int(i == tier) for i in range(5)], counts
        assert counts['mesh_lod']['sample'] == [dict(id=OBJECT, tier=tier)], counts
        triangles = [4, 2, 1][tier]
        assert counts['camera_draws'] == 1 and counts['camera_triangles'] == triangles, counts
        assert counts['shadow_views'] and counts['shadow_draws'] == counts['shadow_views'], counts
        assert counts['shadow_triangles'] == triangles * counts['shadow_draws'], counts
        probes = diagnostics['scene_products']['probes']
        assert probes[0]['surface_valid'] is True and probes[1]['surface_valid'] is (tier == 0), probes
        assert probes[0]['raw_hdr'][0] > .95 and max(probes[0]['raw_hdr'][1:3]) < .01, probes
        self.evidence['images'][f'{label}-{name}'] = digest(output)
        return output

    def collision(self, owner, session):
        hit = self.call(owner, 'runtime.raycast', session_id=session, tick=0, origin=[.8, 0, 0],
                        direction=[0, 0, -1], distance=20)['hit']
        assert hit['entity'] == OBJECT and hit['triangle'] == 1 and abs(hit['distance'] - 10) < .001, hit

    def run(self):
        with WorldClient.open(str(self.args.binary.resolve()), self.native(self.world)) as owner:
            self.author(owner)
            if not self.args.runtime_root:
                return
            for case in CAMERAS:
                self.capture(owner, case)
            session = uid(555)
            self.call(owner, 'runtime.start', session_id=session, revision=self.revision)
            baseline = self.capture(owner, CAMERAS[-1], session, label='source')
            self.collision(owner, session)

        self.manifest.write_text(json.dumps(dict(
            format='poima.project', version=1, project_id=uid(1234), name='Rigid LOD test',
            entry=dict(world=self.world.name, controller=PLAYER, camera=CAMERAS[-1][1]), audio=False)),
            encoding='utf-8')
        exported = self.root / 'Exported game'
        command = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                   '--output', self.native(exported), '--runtime', self.native(self.args.runtime_root)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120, encoding='utf-8')
        self.evidence['export'] = dict(argv=command, stdout=result.stdout, stderr=result.stderr, exit_code=result.returncode)
        assert result.returncode == 0 and json.loads(result.stdout)['status'] == 'ok', self.evidence['export']
        assert self.expected == {path.stem for path in exported.rglob('*.pmodel')}
        relocated, detached = self.root / 'Relocated game', self.root / 'Detached project'
        exported.rename(relocated)
        self.project.rename(detached)
        frozen = inventory(relocated)
        self.evidence['bundle_inventory'] = frozen
        assert not self.project.exists() and not exported.exists()
        exe = relocated / 'runtime' / 'bin' / 'poima.exe'
        with WorldClient.open_game(str(exe), self.native(relocated / 'game.json'), cwd=self.root) as owner:
            runtime = self.call(owner, 'runtime.status')
            assert runtime['tick'] == 0, runtime
            relocated_image = self.capture(owner, CAMERAS[-1], runtime['session_id'], label='relocated')
            self.collision(owner, runtime['session_id'])
            assert pixels(baseline) == pixels(relocated_image), 'Relocated capture changed pixels.'
        assert inventory(relocated) == frozen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path, help='Installed Windows runtime. Omit to exercise authoring only.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='rigid-lod-', dir=args.output.resolve()))
    evidence = dict(format='poima.rigid-mesh-lod.v1', passed=False, requests=[], images={},
                    binary_sha256=digest(args.binary), fixture_sha256=digest(Path(__file__)),
                    gpu=args.gpu if args.runtime_root else None, lighting_path=args.lighting_path)
    try:
        Fixture(args, root, evidence).run()
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
