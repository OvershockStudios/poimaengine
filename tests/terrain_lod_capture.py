#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Audit cooked terrain boundaries and render mixed displaced terrain tiers."""

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import traceback

from snow_compaction_contract import SnowContract, TOP_DOWN, near
from texture_fixture import png
from world_particles_contract import component, transform
from world_terrain_contract import encode, error, file_hash, read_inventory, rpc, success, tree_hashes, uid
from poima_client import WorldClient


CAMERA, SUN = uid(900), uid(5)
TILES = [uid(i) for i in range(921, 924)]
NX, NZ, CELLS = 39, 17, 16
ORIGIN = (64, 0, 20)
WIDTH, HEIGHT = 641, 321
LENS = dict(vertical_fov=64, near=.1, far=40)
FOCAL = HEIGHT / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
START, SHIFTED = [69.25, 6, 24], [75.25, 6, 24]
RELIEF = .1
LEVELS = [dict(stride=4, min_distance_m=7.1), dict(stride=8, min_distance_m=12)]


def heights():
    return [(8*x + 3*z + (3*x + z) % 7 * 13 + (500 if (x,z)==(11,6) else 0)) / 1024
            for z in range(NZ) for x in range(NX)]


def decode(path):
    data = path.read_bytes()
    assert data[:8] == b'POIMAM03'
    header, payload = struct.unpack_from('<II', data, 8)
    assert len(data) == 16 + header + payload
    meta = json.loads(data[16:16 + header])
    assert meta['nodes'][0]['primitives'] == [0] and not meta['images'], meta
    offset = 16 + header
    meshes = []
    for shape in meta['primitives']:
        end = offset + 48 * shape['vertices']
        words = list(struct.iter_unpack('<12I', data[offset:end]))
        positions = [p[:3] for p in struct.iter_unpack('<12f', data[offset:end])]
        indices = struct.unpack_from(f'<{shape["indices"]}I', data, end)
        offset = end + 4 * len(indices)
        meshes.append((words, positions, list(zip(indices[::3], indices[1::3], indices[2::3], strict=True))))
    assert offset == len(data)
    return meshes


def boundary(mesh, bounds):
    words, points, triangles = mesh
    counts = Counter()
    used = set()
    area_twice = 0
    for triangle in triangles:
        a, b, c = (points[i] for i in triangle)
        oriented = (b[2]-a[2])*(c[0]-a[0]) - (b[0]-a[0])*(c[2]-a[2])
        assert oriented > 0, (a, b, c)
        area_twice += oriented
        used.update(triangle)
        counts.update(tuple(sorted((p, q))) for p, q in zip(triangle, triangle[1:] + triangle[:1], strict=True))
    assert len(used) == len(points) == len({(p[0], p[2]) for p in points})
    assert len(points) - len(counts) + len(triangles) == 1
    lo, hi = bounds['min'], bounds['max']
    assert area_twice == 2 * (hi[0]-lo[0]) * (hi[2]-lo[2])
    edges = set()
    for (p, q), multiplicity in counts.items():
        assert multiplicity in (1, 2)
        if multiplicity == 1:
            a, b = points[p], points[q]
            assert (a[0] == b[0] and a[0] in (lo[0], hi[0]) and abs(a[2]-b[2]) == .5) or (
                a[2] == b[2] and a[2] in (lo[2], hi[2]) and abs(a[0]-b[0]) == .5), (a, b)
            edges.add(tuple(sorted((words[p], words[q]))))
    return edges


def near_seam(camera, seam_x):
    source = heights()
    sample_x = round(2 * (seam_x - ORIGIN[0]))
    result = []
    for y in (HEIGHT//2-52, HEIGHT//2, HEIGHT//2+48):
        depth = camera[1] - RELIEF
        for _ in range(10):
            z = 2 * (camera[2] + (y + .5 - HEIGHT/2)*depth/FOCAL - ORIGIN[2])
            row = math.floor(z)
            low, high = source[row*NX+sample_x], source[(row+1)*NX+sample_x]
            depth = camera[1] - RELIEF - (low + (high-low)*(z-row))
        column = round(WIDTH/2 + (seam_x - camera[0])*FOCAL/depth - .5)
        result.extend((column+x, y) for x in (-3, -1, 0, 1, 3))
    return result


def intersect(camera, pixel, meshes):
    rx = (pixel[0] + .5 - WIDTH / 2) / FOCAL
    rz = (pixel[1] + .5 - HEIGHT / 2) / FOCAL
    depth = math.inf
    for _, points, tris in meshes:
        for ia, ib, ic in tris:
            a, b, c = points[ia], points[ib], points[ic]
            ux, uy, uz = (q-p for p, q in zip(a, b, strict=True))
            vx, vy, vz = (q-p for p, q in zip(a, c, strict=True))
            det = ux*vz - uz*vx
            dx, dz = camera[0]-a[0], camera[2]-a[2]
            slope_x, slope_z = (uy*vz-vy*uz)/det, (ux*vy-vx*uy)/det
            along = (camera[1] - a[1] - RELIEF - slope_x*dx - slope_z*dz)/(1 + slope_x*rx + slope_z*rz)
            x, z = dx + rx*along, dz + rz*along
            u, v = (x*vz-z*vx)/det, (ux*z-uz*x)/det
            if along > 0 and u >= -1e-9 and v >= -1e-9 and u+v <= 1+1e-9:
                depth = min(depth, along)
    assert math.isfinite(depth), (camera, pixel)
    n, f = LENS['near'], LENS['far']
    return f / (f-n) * (1 - n/depth)


class TerrainLod(SnowContract):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Terrain LOD seam')))
        rev = self.world_call('world.inspect')['revision']
        original = dict(format='poima.terrain.heightfield', version=1, samples_x=NX, samples_z=NZ,
                        tile_cells=CELLS, origin_m=ORIGIN, spacing_m=[.5, .5], heights_m=heights())
        previous = self.world_call('world.terrain.inspect', recipe=encode(original))
        self.measurements['v1_plan'] = previous
        if self.args.reference:
            with WorldClient.open(self.args.reference.resolve(strict=True), self.native(self.root/'reference.json')) as client:
                retained = client.call('world.terrain.inspect', dict(recipe=encode(original)))
                assert retained == previous
                assert client.call('session.close')['closed']
            self.measurements['retained_v1_binary_sha256'] = file_hash(self.args.reference)
        request = dict(original, version=2, lods=LEVELS)
        plan = self.world_call('world.terrain.inspect', recipe=encode(request))
        assert self.world_call('world.terrain.inspect', recipe=json.dumps(request, indent=1)) == plan
        assert plan['recipe_sha256'] != previous['recipe_sha256'] and plan['tiles_x'] == 3
        small = dict(request, samples_x=18, heights_m=[0]*(18*NZ))
        tail = self.world_call('world.terrain.inspect', recipe=encode(small))['tiles'][-1]
        assert tail['triangles'] == 2*CELLS and tail['lod_tiers'] == [] and 'lods' not in tail['static_mesh'], tail
        for bad in ([dict(stride=True, min_distance_m=4)], [dict(stride=129, min_distance_m=4)],
                    [LEVELS[0], dict(stride=4, min_distance_m=9)], list(reversed(LEVELS))):
            reply, = self.world_calls(rpc('world.terrain.inspect', recipe=encode(dict(request, lods=bad))))
            error(reply, -32602)
        stale, = self.world_calls(rpc('world.terrain.cook', recipe=encode(dict(request, lods=[LEVELS[0]])),
                                      expected_recipe_sha256=plan['recipe_sha256']))
        error(stale, -32009)
        assert not read_inventory(self.assets)
        assert self.world_call('world.terrain.cook', recipe=encode(request),
                               expected_recipe_sha256=plan['recipe_sha256']) == plan
        snapshot = read_inventory(self.assets)
        assert self.world_call('world.terrain.cook', recipe=encode(original),
                               expected_recipe_sha256=previous['recipe_sha256']) == previous
        base_models = [decode(self.assets / (tile['asset'] + '.pmodel'))[0] for tile in previous['tiles']]
        assert self.world_call('world.terrain.cook', recipe=encode(request),
                               expected_recipe_sha256=plan['recipe_sha256']) == plan
        recooked = read_inventory(self.assets)
        assert all(recooked[name] == value for name, value in snapshot.items())

        source = self.root / 'Height.png'
        source.write_bytes(png(1, 1, [255, 255, 255, 255]))
        texture = self.world_call('asset.image.import', source=self.native(source), color_space='linear')['asset']
        source.unlink()
        operations = [dict(op='component.remove', id=uid(1), type=kind) for kind in ('MeshRenderer', 'BoxCollider')]
        operations += [dict(op='entity.create', id=CAMERA, name='Camera observing two seams'),
                       component(CAMERA, 'Transform', transform(START, TOP_DOWN)), component(CAMERA, 'Camera', LENS),
                       component(SUN, 'Transform', transform([0, 0, 0], TOP_DOWN)),
                       component(SUN, 'Light', dict(kind='directional', color=[1]*3, intensity=1, enabled=True,
                           shadow=dict(enabled=True, distance=24, bias=.0001, normal_bias=.001)))]
        self.tiles = plan['tiles']
        self.models = []
        edges = []
        for entity, tile, base in zip(TILES, self.tiles, base_models, strict=True):
            package = self.assets / (tile['asset'] + '.pmodel')
            assert file_hash(package) == tile['asset']
            levels = decode(package)
            assert len(levels) == 3 and levels[0] == base
            budgets = [len(level[2]) for level in levels]
            assert budgets[0] == tile['triangles'] and budgets[0] > budgets[1] > budgets[2], budgets
            assert budgets[1:] == [row['triangles'] for row in tile['lod_tiers']]
            outline = boundary(base, tile['bounds'])
            assert all(boundary(mesh, tile['bounds']) == outline for mesh in levels[1:])
            edges.append(outline)
            self.models.append(levels)
            appearance = dict(tile['static_mesh'], displacement=dict(texture=texture, axis='y', tile_mm=1000, height_m=RELIEF))
            operations += [dict(op='entity.create', id=entity, name='Terrain tile ' + str(tile['x'])),
                           component(entity, 'Transform', transform([0, 0, 0])), component(entity, 'StaticMesh', appearance),
                           component(entity, 'MeshCollider', dict(asset=tile['asset'], primitive=0, friction=.5, restitution=0))]
        for a, b in zip(edges[:-1], edges[1:], strict=True):
            assert len(a & b) == CELLS
        authored = self.world_call('world.transact', request_id=uid(940), base_revision=rev, ops=operations)
        assert authored['committed']
        world = self.world.read_bytes()
        appearance = json.loads(world)['entities'][TILES[0]]['components']['StaticMesh']
        for i, invalid in enumerate((dict(appearance, lod_anchor=[True, 0, 0]),
                dict(appearance, lod_anchor=[1000001, 0, 0]), {k: v for k,v in appearance.items() if k != 'lods'})):
            reply, = self.world_calls(rpc('world.transact', request_id=uid(941+i), base_revision=authored['revision'],
                                          ops=[component(TILES[0], 'StaticMesh', invalid)]))
            error(reply, -32602)
            assert self.world.read_bytes() == world
        dependencies = self.world_call('world.dependencies')
        assert dependencies['needs_mesh_lod'] and dependencies['needs_mesh_lod_anchor'] and dependencies['needs_material_displacement']
        assert {item['sha256'] for item in dependencies['assets']} == {texture, *(tile['asset'] for tile in self.tiles)}
        refs = self.world_call('world.asset.references')['edges']
        for entity, tile in zip(TILES, self.tiles, strict=True):
            assert {(row['component'], row['path'], row['subresource']['index']) for row in refs
                    if row['owner']['id']==entity and row['asset']==tile['asset']} == {
                ('StaticMesh','/asset',0), ('StaticMesh','/lods/0/asset',1), ('StaticMesh','/lods/1/asset',2), ('MeshCollider','/asset',0)}
        self.measurements.update(v2_plan=plan, closure=dependencies['assets'], edge_segments=[len(a & b) for a,b in zip(edges, edges[1:])])
        return authored['revision']

    def session(self, revision, label, game=None):
        saves = self.root / (label + ' save slots')
        saves.mkdir()
        frames = {}
        with self.owner(label, game=game, saves=saves) as (client, _):
            session = self.initialize(client, revision, uid(949), saves, game)
            all_probes = near_seam(START, 72) + near_seam(SHIFTED, 72) + near_seam(SHIFTED, 80)
            for frame, tick, camera, tiers in (('near', 0, START, [0, 1, 2]), ('shifted', 1, SHIFTED, [1, 0, 1])):
                if tick:
                    self.step(client, session, 0, 950, camera_poses=[dict(entity=CAMERA,
                        world=dict(position=[SHIFTED[0]-START[0], 0, 0], rotation=[0,0,0,1]))])
                hit = client.call('runtime.raycast', dict(session_id=session, tick=tick,
                    origin=[69.5, 6, 23], direction=[0, -1, 0], distance=9))['hit']
                assert hit['entity'] == TILES[0] and hit['triangle'] is not None, hit
                near(hit['position'][1], heights()[6*NX+11], 3e-5)
                if self.args.headless:
                    frames[frame] = dict(ray=hit)
                    continue
                path = self.root / f'{label}-{frame}.bmp'
                receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=CAMERA,
                    path=self.native(path), width=WIDTH, height=HEIGHT, samples=1, gpu=self.args.gpu,
                    lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=1, reuse_renderer=True,
                    scene_product_probes=[dict(x=x, y=y) for x,y in all_probes]), timeout=90)
                assert receipt['hardware'] and receipt['capture_written'] and receipt['nvrhi_errors']==0, receipt
                assert receipt['renderer_reused'] == bool(tick) and receipt['tick']==tick, receipt
                for actual, expected in zip(receipt['camera_world'][12:15], camera, strict=True):
                    near(actual, expected, 1e-9)
                render = receipt['render_diagnostics']
                counts = render['last_draws']
                assert {row['id']: row['tier'] for row in counts['mesh_lod']['sample']} == dict(zip(TILES, tiers, strict=True)), counts
                selected = [models[tier] for models, tier in zip(self.models, tiers, strict=True)]
                assert counts['camera_triangles'] == sum(len(mesh[2]) for mesh in selected[:2+tick]), counts
                assert counts['shadow_draws'] > 0 and counts['displacement_patches'] == counts['camera_triangles'] + counts['shadow_triangles'], counts
                assert render['material_displacement_supported'] and not counts['displacement_budget_fallback'], render
                probes = {(item['x'],item['y']): item for item in render['scene_products']['probes']}
                errors = []
                for seam in ((72,80) if tick else (72,)):
                    for pixel in near_seam(camera, seam):
                        sample = probes[pixel]
                        assert sample['surface_valid'] and not sample['motion_valid'], sample
                        depth = intersect(camera, pixel, selected)
                        near(sample['depth'], depth, 3e-6)
                        errors.append(abs(sample['depth']-depth))
                frames[frame] = dict(sha256=file_hash(path), report=receipt, ray=hit, seam_probes=len(errors), max_depth_error=max(errors))
            client.call('runtime.stop', dict(session_id=session))
        self.measurements[label] = frames
        return frames

    def run(self):
        revision = self.author()
        authored = self.session(revision, 'authored')
        if self.args.headless:
            return
        out = self.root / 'Built terrain'
        build = ('project', 'build', self.native(self.manifest), '--output', self.native(out))
        old = self.root / 'Runtime without anchors'
        old.mkdir()
        descriptor = json.loads((self.args.runtime_root / 'runtime.json').read_bytes())
        assert descriptor['features'].pop('mesh_lod_anchor')
        (old/'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
        refused = subprocess.run([str(self.args.binary.resolve()), *build, '--runtime', self.native(old)],
                                 capture_output=True, text=True, encoding='utf-8', timeout=90)
        refusal = json.loads(refused.stdout)
        assert refused.returncode != 0 and not out.exists() and any('LOD anchors' in d['message'] for d in refusal['diagnostics']), refusal
        self.measurements['missing_feature_refusal'] = refusal
        success(json.loads(self.cli(*build, '--runtime', self.native(self.args.runtime_root))))
        detached, moved = self.root / 'Detached working project', self.root / 'Relocated playable terrain'
        self.project.rename(detached)
        out.rename(moved)
        assert not self.project.exists() and not out.exists()
        frozen = tree_hashes(moved)
        assert {Path(name).stem for name in frozen if name.endswith('.pmodel')} == {tile['asset'] for tile in self.tiles}
        saved = self.session(revision, 'relocated', moved/'game.json')
        for frame in authored:
            assert authored[frame]['sha256'] == saved[frame]['sha256'] and authored[frame]['ray'] == saved[frame]['ray'], frame
        assert tree_hashes(moved) == frozen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless', action='store_true')
    mode.add_argument('--runtime-root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--reference', type=Path, help='Compare the v1 plan against the 0.0.466 executable.')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Use assertions and a GPU index from 0 to 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='terrain-lod-', dir=args.output.resolve()))
    fixture = TerrainLod(args, root)
    result = dict(format='poima.terrain-lod-witness.v1', passed=False, headless=args.headless,
                  binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
