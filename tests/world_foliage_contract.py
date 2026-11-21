#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check foliage plans through native authoring, collision and optional Windows Vulkan."""
import argparse
import copy
import itertools
import json
import math
from pathlib import Path
import struct
import tempfile
import traceback

from animation_fixture import ribbon
from gltf_fixture import glb, sphere
from scene_capture import pixels
from world_terrain_contract import (CAPTURE_LENS, CAPTURE_SIZE, TerrainContract, encode, error, file_hash,
                                    height_at, projection, read_inventory, rpc, success, tagged, tree_hashes, uid)


PATCH = '0123456789abcdef'


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def marker_glb(radius, channel):
    doc, raw = sphere(rings=6, segments=10)
    blob = bytearray(raw)
    for offset in range(0, doc['bufferViews'][0]['byteLength'], 24):
        x, y, z = struct.unpack_from('<fff', raw, offset)
        struct.pack_into('<fff', blob, offset, x*radius, (y+1)*radius, z*radius)
    doc['accessors'][0].update(min=[-radius, 0, -radius], max=[radius, 2*radius, radius])
    doc['materials'] = [dict(emissiveFactor=[.65 if i == channel else 0 for i in range(3)],
        pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0, roughnessFactor=1))]
    return glb(doc, bytes(blob)), doc['accessors'][2]['count']//3


class FoliageContract(TerrainContract):
    def import_model(self, filename, data):
        path = self.project / filename
        path.write_bytes(data)
        asset = self.world_call('asset.import', source=self.native(path))['asset']
        path.unlink()
        return asset

    def transaction(self, base, receipt, ops):
        return rpc('world.transact', request_id=uid(receipt), base_revision=base, ops=ops)

    def verify_plan(self, params, revision):
        def request(**updates):
            return rpc('world.foliage.plan', **(params | updates))

        reordered = copy.deepcopy(params['scatter'])
        for field in ('species', 'suppressed_cells', 'exclude_rectangles_m'):
            reordered[field].reverse()
        reordered['density'] = 1
        first, complete, equivalent = map(success, self.world_calls(request(limit=3), request(limit=64),
            request(terrain={'recipe': self.terrain_raw}, scatter=reordered, limit=64)))
        assert complete == equivalent and first['placements'] == complete['placements'][:3], (complete, equivalent)
        assert first['offset'] == 0 and first['next_offset'] == 3 and complete['next_offset'] is None
        rows, pin, counts = complete['placements'], complete['plan_sha256'], complete['counts']
        assert first['plan_sha256'] == pin and first['counts'] == counts and first['revision'] == revision
        assert complete['semantics'] == 'poima.foliage.plan.v1' and complete['revision'] == revision
        assert complete['terrain_recipe_sha256'] == params['expected_terrain_recipe_sha256'] and len(pin) == 64
        offsets = range(3, len(rows), 3)
        pages = list(map(success, self.world_calls(*(request(offset=offset, limit=3, expected_plan_sha256=pin) for offset in offsets))))
        for offset, page in zip(offsets, pages):
            assert page['revision'] == revision and page['offset'] == offset and page['plan_sha256'] == pin
            assert page['counts'] == counts and page['placements'] == rows[offset:offset+3], page
            assert page['next_offset'] == (offset+3 if offset+3 < len(rows) else None)
        assert first['placements'] + [row for page in pages for row in page['placements']] == rows

        assert counts['cells'] == 60 and counts['placed'] == len(rows)
        assert sum(value for name, value in counts.items() if name != 'cells') == counts['cells']
        assert [counts[k] for k in ('density_rejected', 'manual_suppressed', 'rectangle_excluded', 'outside_terrain')] == [0, 3, 1, 27], counts
        assert counts['slope_rejected'] > 0
        cells = [(row, column) for column, row in (item['cell'] for item in rows)]
        assert cells == sorted(set(cells)), cells
        recipe = params['scatter']
        species = {item['id']: item for item in recipe['species']}
        for item in rows:
            column, row = item['cell']
            assert item['key'] == f'{PATCH}{row:08x}{column:08x}' and item['cell'] not in recipe['suppressed_cells']
            transform = item['transform']
            x, y, z = transform['position']
            assert 10 < x < 14 and -4 < z < -2 and math.isclose(y, height_at(x, z), abs_tol=1e-11, rel_tol=0), item
            assert 9.5+column*.5 < x < 10+column*.5 and -4.5+row*.5 < z < -4+row*.5, item
            assert all(not (x0 <= x < x1 and z0 <= z < z1) for x0, z0, x1, z1 in recipe['exclude_rectangles_m'])
            definition = species[item['species']]
            slope = math.degrees(math.atan(math.hypot(.5 if x < 12 else 1, .25)))
            assert definition['slope_degrees'][0] <= slope <= definition['slope_degrees'][1], item
            scale, rotation = transform['scale'], transform['rotation']
            assert scale[0] == scale[1] == scale[2] and definition['uniform_scale'][0] <= scale[0] <= definition['uniform_scale'][1]
            assert rotation[0] == rotation[2] == 0 and math.isclose(sum(q*q for q in rotation), 1, abs_tol=1e-12)
            assert 0 <= item['variant'] < len(definition['variants'])
            assert item['static_mesh'] == dict(definition['variants'][item['variant']], visible=True), item
        for a, b in itertools.combinations(rows, 2):
            ax, _, az = a['transform']['position']
            bx, _, bz = b['transform']['position']
            assert math.hypot(ax-bx, az-bz) >= recipe['minimum_spacing_m'] - 1e-12, (a, b)
        return complete

    def verify_rejections(self, params, plan, weighted, source_path):
        def request(**updates):
            return rpc('world.foliage.plan', **(params | updates))

        def replaced_variant(asset, primitive=0):
            scatter = copy.deepcopy(params['scatter'])
            scatter['species'][0]['variants'] = [dict(asset=asset, primitive=primitive)]
            return request(scatter=scatter)

        bad_continuation = request(offset=1)
        pin = plan['plan_sha256']
        wrong_plan = request(offset=1, expected_plan_sha256='0'*64)
        bad_recipe = request(scatter=params['scatter'] | {'grid': [4097, 1]})
        wrong_source = request(terrain={'recipe': self.terrain_raw, 'source': source_path.name})
        variant = plan['placements'][0]['static_mesh']['asset']
        cases = [bad_continuation, wrong_plan, bad_recipe, wrong_source, replaced_variant('f'*64),
                 replaced_variant(weighted), replaced_variant(variant, 1)]
        responses = self.world_calls(*cases)
        for response, code in zip(responses, (-32602, -32009, -32602, -32602, -32050, -32050, -32050)):
            error(response, code)
        for response, asset, primitive in zip(responses[4:], ('f'*64, weighted, variant), (0, 0, 1)):
            assert response['error']['data'] == dict(asset=asset, primitive=primitive), response
        assert pin in encode(responses[1]['error']['data']) and '0'*64 in encode(responses[1]['error']['data'])

        original = source_path.read_bytes()
        changed = json.loads(original)
        changed['heights_m'][0] += .125
        try:
            source_path.write_text(encode(changed), encoding='utf-8')
            actual = self.world_call('world.terrain.inspect', source=source_path.name)['recipe_sha256']
            stale = error(self.world_calls(request())[0], -32009)
            assert actual in encode(stale['data']) and params['expected_terrain_recipe_sha256'] in encode(stale['data'])
        finally:
            source_path.write_bytes(original)

        package = self.assets / (variant+'.pmodel')
        retained = self.root / 'retained foliage package.pmodel'
        package.rename(retained)
        try:
            package.write_bytes(b'corrupt foliage fixture\n')
            error(self.world_calls(request())[0], -32050)
            assert package.read_bytes() == b'corrupt foliage fixture\n'
        finally:
            package.unlink(missing_ok=True)
            retained.rename(package)

    def check_bindings(self, mapping, rows, expected, revision):
        closure = self.world_call('world.dependencies')
        assert closure['revision'] == revision and closure['assets'] == expected, closure
        inspected = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert inspected['assets'] == expected and inspected['revision'] == revision, inspected
        scene = json.loads(self.world.read_bytes())['entities']
        refs = self.world_call('world.asset.references', revision=revision)
        actual = {(edge['owner']['id'], edge['asset'], edge['subresource']['index']) for edge in refs['edges']
                  if edge['owner']['id'] in mapping.values() and edge['component'] == 'StaticMesh'}
        assert actual == {(mapping[row['key']], row['static_mesh']['asset'], 0) for row in rows}, refs
        for row in rows:
            entity = scene[mapping[row['key']]]
            assert entity['parent'] is None and entity['components'] == {
                'Transform': row['transform'], 'StaticMesh': row['static_mesh']}, entity

    def contacts(self, rows, revision, session):
        witnesses = [next(item for item in rows if (item['transform']['position'][0] < 12) == left) for left in (True, False)]
        requests = [rpc('runtime.start', session_id=session, revision=revision)]
        for row in witnesses:
            x, _, z = row['transform']['position']
            requests.append(rpc('runtime.raycast', session_id=session, tick=0, origin=[x, 20, z], direction=[0, -1, 0], distance=25))
        responses = list(map(success, self.world_calls(*requests, rpc('session.close'))))
        assert responses[0]['session_id'] == session and responses[-1]['closed'] is True
        hits = {}
        for tile, (row, result) in enumerate(zip(witnesses, responses[1:-1])):
            hit = result['hit']
            assert hit and hit['entity'] == uid(100+tile) and type(hit['triangle']) is int and 0 <= hit['triangle'] < 8, result
            assert len(hit['position']) == 3 and all(math.isclose(a, b, rel_tol=0, abs_tol=2e-5)
                for a, b in zip(hit['position'], row['transform']['position'])), (hit, row)
            hits[row['key']] = hit
        return hits

    def prepare_camera(self, revision):
        pitch = math.radians(-40)/2
        ops = [component(uid(3), 'Transform', dict(position=[12, 9, 0], scale=[1, 1, 1],
                   rotation=[math.sin(pitch), 0, 0, math.cos(pitch)])), component(uid(3), 'Camera', CAPTURE_LENS),
               component(uid(4), 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1)),
               dict(op='component.remove', id=uid(5), type='Light'), dict(op='component.remove', id=uid(1), type='MeshRenderer')]
        for index in (100, 101):
            ops.append(component(uid(index), 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=[.12]*3,
                metallic=0, roughness=1, double_sided=False)))
        reply = success(self.world_calls(self.transaction(revision, 2100, ops))[0])
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return revision+1

    def capture_game(self, bundle, rows, variants, source, revision):
        game = bundle / 'game.json'
        exe = bundle / json.loads(game.read_bytes())['engine']['executable']
        external = self.root / 'Capture working directory'
        external.mkdir()
        replay, image, report_file = (external / name for name in ('neutral.json', 'foliage.bmp', 'report.json'))
        replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
        frozen, original = tree_hashes(bundle), tree_hashes(source)
        assert not self.project.exists()
        width, height = CAPTURE_SIZE
        result = success(json.loads(self.cli('game', 'run', self.native(game), '--gpu', self.args.gpu,
            '--replay', self.native(replay), '--capture', self.native(image), '--report', self.native(report_file),
            '--width', width, '--height', height, '--samples', 1, executable=exe, cwd=external)))
        play = result['play']
        capture = self.measurements['capture'] = dict(report=play, image=str(image), bundle=str(bundle))
        assert json.loads(report_file.read_bytes()) == result and result['runtime']['tick'] == 1
        assert play['hardware'] is True and play['capture_written'] is True and play['nvrhi_errors'] == 0
        assert play['success'] is True and play['stop_reason'] == 'replay_complete' and play['frames_presented'] > 0
        assert play['revision'] == revision and (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, 1)
        diagnostics = play['render_diagnostics']
        drawn = diagnostics['last_draws']
        assert diagnostics['completed_submissions'] > 0 and drawn['objects'] == drawn['camera_draws'] == len(rows)+2, drawn
        assert drawn['camera_triangles'] == sum(variants[row['static_mesh']['asset']]['triangles'] for row in rows)+16, drawn
        presented = diagnostics['scene_receipts']['presented']
        assert presented['camera_id'] == uid(3) and presented['camera_lens'] == CAPTURE_LENS
        camera = presented.get('camera_pose', {}).get('camera_world') or play['camera_world']
        frame = pixels(image)
        assert len(frame) == height and all(len(line) == width for line in frame)
        area = [sum(tagged(value, channel) for line in frame for value in line) for channel in (0, 1)]
        witnesses = []
        for row in rows:
            variant = variants[row['static_mesh']['asset']]
            x, y, z = row['transform']['position']
            center = [x, y + variant['radius']*row['transform']['scale'][0], z]
            u, v = map(math.floor, projection(camera, CAPTURE_LENS, center))
            assert 1 <= u < width-1 and 1 <= v < height-1, (row, u, v)
            channel = variant['channel']
            matched = any(tagged(frame[v+dy][u+dx], channel) for dx, dy in itertools.product(range(-1, 2), repeat=2))
            witnesses.append(dict(key=row['key'], channel=channel, pixel=[u, v], matched=matched))
        capture.update(sha256=file_hash(image), executable_sha256=frozen[exe.relative_to(bundle).as_posix()],
                       colored_pixels=area, projected_markers=witnesses, bundle_inventory=frozen)
        assert min(area) >= 100 and all(sum(row['matched'] and row['channel'] == c for row in witnesses) >= 2 for c in (0, 1)), capture
        assert tree_hashes(bundle) == frozen and tree_hashes(source) == original

    def run(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Foliage Contract')))
        source = self.project / 'Foliage terrain.json'
        self.terrain_raw = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=5, samples_z=3,
            tile_cells=2, origin_m=[10, 4, -4], spacing_m=[1, 1],
            heights_m=[base+.25*z for z in range(3) for base in [0, .5, 1, 2, 3]]))
        source.write_text(self.terrain_raw, encoding='utf-8')
        terrain = self.world_call('world.terrain.inspect', source=source.name)
        assert self.world_call('world.terrain.cook', source=source.name, expected_recipe_sha256=terrain['recipe_sha256']) == terrain
        assert [(tile['x'], tile['z'], tile['primitive']) for tile in terrain['tiles']] == [(0, 0, 0), (1, 0, 0)]
        variants = {}
        for channel, radius in enumerate((.13, .16)):
            data, triangles = marker_glb(radius, channel)
            asset = self.import_model(f'Marker {channel}.glb', data)
            variants[asset] = dict(channel=channel, radius=radius, triangles=triangles)
        weighted = self.import_model('Weighted ribbon.glb', glb(*ribbon()))
        assets = [dict(asset=asset, primitive=0) for asset in variants]
        flat_limit = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=2, samples_z=2,
            tile_cells=1, origin_m=[0, 10000, 0], spacing_m=[2, 2], heights_m=[0, 0, 0, 0]))
        high_pin = self.world_call('world.terrain.inspect', recipe=flat_limit)['recipe_sha256']
        high_plan = self.world_call('world.foliage.plan', terrain={'recipe': flat_limit},
            expected_terrain_recipe_sha256=high_pin, scatter=dict(patch_key='0000000000000001',
                seed='000000000000008b', origin_m=[.21, .37], cell_size_m=.83, grid=[1, 1],
                species=[dict(id=1, variants=assets[:1])]))
        assert high_plan['counts']['placed'] == 1 and high_plan['placements'][0]['transform']['position'][1] == 10000
        scatter = dict(patch_key=PATCH, seed='fedcba9876543210', origin_m=[9.5, -4.5], cell_size_m=.5, grid=[10, 6],
            minimum_spacing_m=.25, species=[dict(id=10, weight=3, slope_degrees=[0, 35], uniform_scale=[.9, 1.1], variants=assets),
                dict(id=20, weight=2, slope_degrees=[0, 60], uniform_scale=[.9, 1.1], variants=list(reversed(assets)))],
            exclude_rectangles_m=[[11, -3.5, 11.5, -3], [13, -3, 13.5, -2.5]], suppressed_cells=[[2, 1], [3, 2], [0, 0]])
        params = dict(terrain={'source': source.name}, expected_terrain_recipe_sha256=terrain['recipe_sha256'], scatter=scatter)
        discovery, view = map(success, self.world_calls(rpc('world.describe', view='method', name='world.foliage.plan'), rpc('world.inspect')))
        assert set(discovery['methods']['world.foliage.plan']['required']) == {'terrain', 'expected_terrain_recipe_sha256', 'scatter'}
        revision = view['revision']
        initial_world, initial_assets = self.world.read_bytes(), read_inventory(self.assets)
        plan = self.verify_plan(params, revision)
        self.verify_rejections(params, plan, weighted, source)
        assert self.world.read_bytes() == initial_world and read_inventory(self.assets) == initial_assets
        assert source.read_bytes() == self.terrain_raw.encode('utf-8')
        self.measurements['plan'] = plan

        rows = plan['placements']
        mapping = {row['key']: uid(200+i) for i, row in enumerate(rows)}
        def plant(row, entity):
            return [dict(op='entity.create', id=entity, name='Foliage '+row['key']),
                    component(entity, 'Transform', row['transform']), component(entity, 'StaticMesh', row['static_mesh'])]

        ops = []
        for index, tile in enumerate(terrain['tiles']):
            entity, part = uid(100+index), dict(asset=tile['asset'], primitive=0)
            ops.extend([dict(op='entity.create', id=entity, name=f'Terrain {index}'),
                        component(entity, 'StaticMesh', dict(part, visible=True)),
                        component(entity, 'MeshCollider', dict(part, friction=.5, restitution=0))])
        for row in rows:
            ops.extend(plant(row, mapping[row['key']]))
        error(self.world_calls(self.transaction(revision-1, 2000, ops))[0], -32009)
        assert self.world.read_bytes() == initial_world
        applied = success(self.world_calls(self.transaction(revision, 2001, ops))[0])
        revision += 1
        assert applied['committed'] and applied['revision'] == revision, applied

        removed = rows[0]
        key = removed['key']
        suppressed = scatter | {'suppressed_cells': scatter['suppressed_cells'] + [removed['cell']]}
        smaller = self.world_call('world.foliage.plan', **(params | {'scatter': suppressed, 'limit': 64}))
        assert smaller['plan_sha256'] != plan['plan_sha256'] and key not in {row['key'] for row in smaller['placements']}
        deleted = success(self.world_calls(self.transaction(revision, 2002,
            [dict(op='entity.delete', id=mapping[key], recursive=False)]))[0])
        revision += 1
        assert deleted['committed'] and deleted['revision'] == revision
        restored = self.world_call('world.foliage.plan', **params, expected_plan_sha256=plan['plan_sha256'], limit=64)
        assert restored == dict(plan, revision=revision), restored
        error(self.world_calls(self.transaction(revision, 2003, plant(removed, mapping[key])))[0], -32602)
        mapping[key] = uid(700)
        created = success(self.world_calls(self.transaction(revision, 2004, plant(removed, mapping[key])))[0])
        revision += 1
        assert created['committed'] and created['revision'] == revision
        self.measurements['entity_mapping'] = mapping

        selected = {row['static_mesh']['asset'] for row in rows}
        assert selected == variants.keys() and weighted not in selected
        selected.update(tile['asset'] for tile in terrain['tiles'])
        expected = []
        for asset in sorted(selected):
            path = self.assets / (asset+'.pmodel')
            assert file_hash(path) == asset
            expected.append(dict(filename=path.name, sha256=asset, bytes=path.stat().st_size))
        self.check_bindings(mapping, rows, expected, revision)
        if self.args.runtime:
            self.measurements['contacts'] = self.contacts(rows, revision, uid(800))

        old_project, self.project = self.project, self.root / 'Relocated Foliage'
        persisted = self.world.read_bytes()
        old_project.rename(self.project)
        self.manifest, self.world, self.assets = (self.project/path.name for path in (self.manifest, self.world, self.assets))
        assert not old_project.exists() and self.world.read_bytes() == persisted
        self.check_bindings(mapping, rows, expected, revision)
        reopened = self.world_call('world.foliage.plan', **params, expected_plan_sha256=plan['plan_sha256'], limit=64)
        assert reopened == dict(restored, revision=revision), reopened
        if self.args.runtime:
            assert self.contacts(rows, revision, uid(801)) == self.measurements['contacts']
        self.measurements.update(closure=expected, relocated_project_reopened=True)

        if self.args.runtime_root:
            if self.args.capture_output:
                revision = self.prepare_camera(revision)
            export = self.root / 'Initial export'
            success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(export),
                                       '--runtime', self.native(self.args.runtime_root))))
            models = list(export.rglob('*.pmodel'))
            assert len(models) == len(expected) and {path.stem for path in models} == selected
            assert all(file_hash(path) == path.stem for path in models) and not list(export.rglob(source.name))
            detached, bundle = self.root / 'Retained authored source', self.root / 'Moved standalone'
            self.project.rename(detached)
            export.rename(bundle)
            assert not self.project.exists() and not old_project.exists() and not export.exists()
            success(json.loads(self.cli('game', 'inspect', self.native(bundle/'game.json'))))
            packaged = json.loads((bundle/'content'/'world.json').read_bytes())['entities']
            authored = json.loads((detached/self.world.name).read_bytes())['entities']
            for entity in (*mapping.values(), uid(100), uid(101)):
                assert packaged[entity] == authored[entity]
            if self.args.capture_output:
                self.capture_game(bundle, rows, variants, detached, revision)
            self.measurements['exported'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed runtime for standalone export.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture-output', type=Path, help='Retain an optional Windows Vulkan replay.')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if args.capture_output and not (args.runtime_root and args.runtime):
        parser.error('--capture-output requires --runtime-root and native simulation.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires an index from 0 through 4095.')

    def execute(directory):
        test = FoliageContract(args, Path(directory))
        passed, failure = False, None
        try:
            test.run()
            passed = True
        except BaseException:
            failure = traceback.format_exc()
            raise
        finally:
            record = dict(passed=passed, runtime=bool(args.runtime), calls=test.calls, measurements=test.measurements)
            if failure:
                record['failure'] = failure
            for evidence in {args.evidence, Path(directory)/'evidence.json' if args.capture_output else None} - {None}:
                evidence.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
                print(f'Evidence file {evidence}')

    if args.capture_output:
        args.capture_output.mkdir(parents=True, exist_ok=True)
        execute(tempfile.mkdtemp(prefix=f'foliage-gpu-{args.gpu}-', dir=args.capture_output.resolve()))
    else:
        scratch = Path(__file__).resolve().parents[1] / 'build' / 'world-foliage-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            execute(directory)
    print('Foliage world contract passed guarded planning, authored placements and relocated asset closure.')


if __name__ == '__main__':
    main()
