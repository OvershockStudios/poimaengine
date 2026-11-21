#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check authored terrain, native mesh raycasts and optional Windows Vulkan captures."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from scene_capture import pixels


CAPTURE_SIZE = (640, 360)
CAPTURE_LENS = dict(vertical_fov=55, near=.1, far=100)


def uid(number):
    return f'{number:032x}'


def rpc(method, **params):
    return dict(jsonrpc='2.0', method=method, params=params)


def success(reply):
    assert 'result' in reply, reply
    return reply['result']


def error(reply, code):
    assert reply.get('error', {}).get('code') == code, reply
    return reply['error']


def encode(recipe):
    return json.dumps(recipe, separators=(',', ':'))


def read_inventory(directory):
    if not directory.is_dir():
        return {}
    return {child.name: (child.read_bytes(), child.stat().st_mtime_ns, child.stat().st_ino)
            for child in directory.iterdir() if child.is_file()}


def file_hash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_hashes(root):
    return {path.relative_to(root).as_posix(): file_hash(path)
            for path in sorted(root.rglob('*')) if path.is_file()}


def height_at(x, z):
    return 4 + .5*min(x-10, 2) + max(x-12, 0) + .25*(z+4)


def projection(camera, lens, point):
    width, height = CAPTURE_SIZE
    delta = [value-origin for value, origin in zip(point, camera[12:15])]
    view = [sum(camera[4*axis+i]*delta[i] for i in range(3)) for axis in range(3)]
    depth = -view[2]
    assert lens['near'] < depth < lens['far'], (point, view, lens)
    focal = height/(2*math.tan(math.radians(lens['vertical_fov'])/2))
    return [width/2 + focal*view[0]/depth, height/2 - focal*view[1]/depth]


def polygon_edges(points):
    center = [sum(point[axis] for point in points) / len(points) for axis in range(2)]
    edges = []
    for (x0, y0), (x1, y1) in zip(points, points[1:]+points[:1]):
        dx, dy = x1-x0, y1-y0
        inward = (1 if dx*(center[1]-y0) - dy*(center[0]-x0) > 0 else -1) / math.hypot(dx, dy)
        edges.append((-dy*inward, dx*inward, (dy*x0 - dx*y0)*inward))
    return edges


def tagged(rgb, channel):
    return rgb[channel] > 100 and all(rgb[channel] > 2*rgb[k] for k in range(3) if k != channel)


def silhouette(image, camera, lens, rays, evidence):
    width, height = CAPTURE_SIZE
    assert len(image) == height and all(len(row) == width for row in image)
    # The fixture is two planar ramps with identity entity transforms.
    quads = [[[x, height_at(x, z), z] for x, z in [(lo, -4), (hi, -4), (hi, -2), (lo, -2)]]
             for lo, hi in [(10, 12), (12, 14)]]
    outlines = [[projection(camera, lens, point) for point in quad] for quad in quads]
    assert all(5 < x < width-5 and 5 < y < height-5 for quad in outlines for x, y in quad), outlines
    edges = [polygon_edges(quad) for quad in outlines]
    # Probe almost two pixels on either side of the common edge, away from its ends.
    seam_edges = [1, 3]
    required, actual = [0, 0], [0, 0]
    seam_pixels = 0
    failures = []
    for y, row in enumerate(image):
        for x, rgb in enumerate(row):
            observed = next((channel for channel in (0, 1) if tagged(rgb, channel)), None)
            if observed is not None:
                actual[observed] += 1
            distances = [[a*(x+.5) + b*(y+.5) + c for a, b, c in polygon] for polygon in edges]
            for channel, d in enumerate(distances):
                if min(d) >= 1.25:
                    required[channel] += 1
                    if observed != channel and len(failures) < 16:
                        failures.append(dict(pixel=[x, y], color=rgb, expected_channel=channel))
            if observed is not None and min(distances[observed]) < -1.25 and len(failures) < 16:
                failures.append(dict(pixel=[x, y], color=rgb, reason='outside tagged surface'))
            seam_band = any(-.02 <= d[edge] < 1.75 and min(d[:edge] + d[edge+1:]) > 2
                            for d, edge in zip(distances, seam_edges))
            if seam_band:
                seam_pixels += 1
                if observed is None and len(failures) < 16:
                    failures.append(dict(pixel=[x, y], color=rgb, reason='uncovered shared edge'))
    witnesses = {}
    for label, ray in rays.items():
        x, y = map(math.floor, projection(camera, lens, ray['hit']['position']))
        rgb = image[y][x]
        witnesses[label] = dict(pixel=[x, y], color=rgb, hit=ray['hit']['position'])
        expected_channels = (0, 1) if label == 'tile seam' else (0,) if label == 'left interior' else (1,)
        if not any(tagged(rgb, channel) for channel in expected_channels):
            failures.append(dict(pixel=[x, y], color=rgb, reason=f'missing projected collision hit: {label}'))
    evidence.update(projected_corners=outlines, required_pixels=required, observed_pixels=actual,
                    seam_pixels=seam_pixels, collider_witnesses=witnesses, mismatches=failures)
    assert min(required) >= 600 and min(actual) >= 600 and seam_pixels >= 30 and not failures, evidence


def expected_depth(camera, lens, x, y, slope):
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(lens['vertical_fov'])/2))
    local = [(x+.5-width/2)/focal, (height/2-y-.5)/focal, -1]
    ray = [sum(camera[4*axis+i]*local[axis] for axis in range(3)) for i in range(3)]
    normal = [-slope, 1, -.25]
    # Both planes share the point [12, 5, -4]. A unit of ray parameter
    # travels one metre along the camera's forward axis.
    numerator = sum(n*(point-origin) for n, point, origin in zip(normal, [12, 5, -4], camera[12:15]))
    distance = numerator/sum(a*b for a, b in zip(normal, ray))
    return lens['far']/(lens['far']-lens['near'])*(1-lens['near']/distance)


class TerrainContract:
    def __init__(self, args, root):
        self.args, self.root = args, root
        self.calls = []
        self.measurements = {}
        self.project = root / 'Terrain Project'
        self.manifest = self.project / 'project.poimaproject'
        self.world = self.project / 'world.json'
        self.assets = self.project / 'world.json.assets'

    def native(self, path):
        path = str(path.resolve())
        if self.args.windows_interop:
            return subprocess.check_output(['wslpath', '-w', path], text=True).strip()
        return path

    def cli(self, *arguments, stdin=None, executable=None, cwd=None):
        command = [str((executable or self.args.binary).resolve()), *map(str, arguments)]
        data = None if stdin is None else ''.join(encode(row) + '\n' for row in stdin)
        call = dict(argv=command, cwd=str(cwd) if cwd else None)
        if stdin is not None:
            call['requests'] = stdin
        self.calls.append(call)
        try:
            child = subprocess.run(command, input=data, capture_output=True, text=True, encoding='utf-8', timeout=90, cwd=cwd)
        except subprocess.TimeoutExpired as exc:
            def decoded(value):
                return value.decode('utf-8', 'replace') if isinstance(value, bytes) else value
            call.update(timeout=exc.timeout, stdout=decoded(exc.stdout), stderr=decoded(exc.stderr))
            raise
        call.update(exit=child.returncode, stderr=child.stderr, stdout=child.stdout)
        assert child.returncode == 0, (command, child.stdout, child.stderr)
        return child.stdout

    def world_calls(self, *requests):
        requests = [dict(request, id=i+1) for i, request in enumerate(requests)]
        output = self.cli('world', '--project', self.native(self.manifest), stdin=requests)
        results = [json.loads(line) for line in output.splitlines()]
        assert [row['id'] for row in results] == list(range(1, len(requests)+1)), results
        return results

    def world_call(self, method, **params):
        return success(self.world_calls(rpc(method, **params))[0])

    def publish_request(self, raw, plan):
        return rpc('world.terrain.cook', recipe=raw, expected_recipe_sha256=plan['recipe_sha256'])

    def verify_existing_paths(self, publish, packages):
        # Every path under this temporary root belongs to the fixture.
        self.assets.mkdir(exist_ok=True)
        first_id = next(iter(packages))
        pending = self.assets / f'{first_id}.pending'
        blocker = b'owned staging sentinel\n'
        pending.write_bytes(blocker)
        error(self.world_calls(publish)[0], -32050)
        assert pending.read_bytes() == blocker and not list(self.assets.glob('*.pmodel'))
        pending.unlink()

        target = self.assets / f'{first_id}.pmodel'
        target.write_bytes(b'corrupt package at immutable identity')
        original = read_inventory(self.assets)
        error(self.world_calls(publish)[0], -32050)
        assert read_inventory(self.assets) == original
        target.unlink()

    def verify_store_alias(self, publish):
        outside = self.root / 'unrelated store'
        outside.mkdir()
        (outside / 'sentinel.txt').write_bytes(b'owned unrelated file\n')
        original = read_inventory(outside)
        existed = self.assets.exists()
        retained = self.root / 'retained asset directory'
        if existed:
            self.assets.rename(retained)
        try:
            try:
                self.assets.symlink_to(outside, target_is_directory=True)
            except (OSError, NotImplementedError):
                return False
            error(self.world_calls(publish)[0], -32050)
            assert self.assets.is_symlink() and read_inventory(outside) == original
            return True
        finally:
            if self.assets.is_symlink():
                self.assets.unlink()
            if existed:
                retained.rename(self.assets)

    def verify_package_alias(self, publish, package):
        # A valid byte-for-byte package behind a link must still be rejected.
        outside = self.root / 'retained original model.pmodel'
        package.rename(outside)
        original = outside.read_bytes()
        try:
            try:
                package.symlink_to(outside)
            except (OSError, NotImplementedError):
                return False
            error(self.world_calls(publish)[0], -32050)
            assert package.is_symlink() and outside.read_bytes() == original
            return True
        finally:
            if package.is_symlink():
                package.unlink()
            outside.rename(package)

    def raycast(self, revision, session, terrain_entities):
        cases = [('left interior', 10.75, -3.5, terrain_entities[0], .5),
                 ('tile seam', 12, -2.75, None, None),
                 ('right interior', 12.5, -3.25, terrain_entities[1], 1)]
        calls = [rpc('runtime.start', session_id=session, revision=revision)]
        calls.extend(rpc('runtime.raycast', session_id=session, tick=0, origin=[x, 20, z], direction=[0, -1, 0], distance=25)
                     for _, x, z, _, _ in cases)
        replies = self.world_calls(*calls, rpc('session.close'))
        assert success(replies[0])['session_id'] == session and success(replies[-1])['closed'] is True
        hits = {}
        for (label, x, z, owner, slope), reply in zip(cases, replies[1:-1]):
            hit = success(reply)['hit']
            assert hit and hit['entity'] in terrain_entities and type(hit['triangle']) is int and 0 <= hit['triangle'] < 8, reply
            if owner is not None:
                assert hit['entity'] == owner, hit
            else:
                slope = .5 if hit['entity'] == terrain_entities[0] else 1
            expected_height = height_at(x, z)
            expected_normal = [-slope, 1, -.25]
            length = math.sqrt(sum(value*value for value in expected_normal))
            assert len(hit['position']) == len(hit['normal']) == 3, hit
            assert all(math.isclose(actual, expected, abs_tol=2e-5) for actual, expected in
                       zip(hit['position'], [x, expected_height, z])), (label, hit, expected_height)
            assert all(math.isclose(actual, expected/length, abs_tol=2e-5) for actual, expected in
                       zip(hit['normal'], expected_normal)), (label, hit, expected_normal)
            hits[label] = {'expected_height': expected_height, 'hit': hit}
        return hits

    def check_cold_save(self, revision, hits):
        saves = self.root / 'Terrain saves'
        saves.mkdir()
        root = self.native(saves)
        source = self.world.read_bytes()

        def step(session, start, ticks, request):
            return rpc('runtime.step', session_id=session, request_id=uid(request),
                       expected_tick=start, ticks=ticks)

        def write(session, tick, slot, request):
            return rpc('save.write', request_id=uid(request), configuration_generation=1,
                       slot=slot, expected_generation=0, session_id=session,
                       expected_tick=tick, expected_gameplay_revision=0)

        warm, cold = uid(802), uid(803)
        replies = list(map(success, self.world_calls(
            rpc('save.configure', request_id=uid(900), expected_generation=0, root=root),
            rpc('runtime.start', session_id=warm, revision=revision),
            step(warm, 0, 3, 903), write(warm, 3, 'checkpoint', 904),
            step(warm, 3, 2, 905), write(warm, 5, 'warm', 906), rpc('session.close'))))
        checkpoint, warm_save = replies[3], replies[5]
        assert replies[2]['tick'] == 3 and replies[4]['tick'] == 5 and replies[-1]['closed'], replies
        rays = [rpc('runtime.raycast', session_id=cold, tick=3,
                    origin=[hit['hit']['position'][0], 20, hit['hit']['position'][2]],
                    direction=[0, -1, 0], distance=25) for hit in hits.values()]
        restored = list(map(success, self.world_calls(
            rpc('runtime.status'),
            rpc('save.configure', request_id=uid(910), expected_generation=0, root=root),
            rpc('save.load', request_id=uid(911), configuration_generation=1, slot='checkpoint',
                expected_generation=checkpoint['generation'], revision=revision, expected_session_id=None,
                expected_tick=None, expected_gameplay_revision=None, new_session_id=cold),
            *rays, step(cold, 3, 2, 912), write(cold, 5, 'cold', 913), rpc('session.close'))))
        result = restored[2]
        assert restored[0]['active'] is False and result['session_id'] == cold and result['tick'] == 3, restored
        assert result['authored_revision'] == revision and not result['source_stale'] and not result['recovered'], result
        assert [row['hit'] for row in restored[3:6]] == [row['hit'] for row in hits.values()], restored
        assert restored[-3]['tick'] == 5 and restored[-1]['closed'], restored
        payloads = [list((saves / f'slot-{slot}').glob('p-*.bin')) for slot in ('checkpoint', 'warm', 'cold')]
        assert all(len(paths) == 1 for paths in payloads), payloads
        initial_bytes, warm_bytes, cold_bytes = (paths[0].read_bytes() for paths in payloads)
        assert initial_bytes != warm_bytes and warm_bytes == cold_bytes and self.world.read_bytes() == source
        assert warm_save['sha256'] == restored[-2]['sha256'] == hashlib.sha256(cold_bytes).hexdigest()
        self.measurements['cold_save'] = dict(checkpoint_tick=3, continuation_tick=5, matching_rays=len(rays),
            checkpoint_sha256=checkpoint['sha256'], continuation_sha256=warm_save['sha256'],
            continuation_bytes=len(cold_bytes))

    def check_render(self, name, path, report, camera, lens, revision):
        entry = self.measurements.setdefault('captures', {})[name] = dict(path=str(path), report=report,
            camera_world=camera, lens=lens)
        assert path.is_file(), report
        entry['sha256'] = file_hash(path)
        assert report['hardware'] is True and report['capture_written'] is True and report['nvrhi_errors'] == 0, report
        assert report['frames_presented'] > 0 and report['revision'] == revision and report['gpu'], report
        assert (report['width'], report['height'], report['samples']) == (*CAPTURE_SIZE, 1), report
        assert len(camera) == 16 and lens == CAPTURE_LENS, (camera, lens)
        diagnostics = report['render_diagnostics']
        drawn = diagnostics['last_draws']
        assert diagnostics['completed_submissions'] > 0 and diagnostics['scene_receipts']['presented']['camera_id'] == uid(3), diagnostics
        assert drawn['objects'] == drawn['camera_draws'] == drawn['camera_batches'] == 2, drawn
        assert drawn['camera_triangles'] == 16 and drawn['camera_culled'] == 0, drawn
        silhouette(pixels(path), camera, lens, self.measurements['rays'], entry.setdefault('silhouette', {}))

    def capture_authored(self, revision, ids):
        def set_component(id_, kind, value):
            return dict(op='component.set', id=id_, type=kind, value=value)

        pitch = math.radians(-40)/2
        ops = [set_component(uid(3), 'Transform', dict(position=[12, 9, 0], scale=[1, 1, 1],
                rotation=[math.sin(pitch), 0, 0, math.cos(pitch)])),
               set_component(uid(3), 'Camera', CAPTURE_LENS),
               set_component(uid(4), 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1)),
               dict(op='component.remove', id=uid(5), type='Light'),
               dict(op='component.remove', id=uid(1), type='MeshRenderer')]
        for id_, color in zip(ids, ([.65, 0, 0], [0, .65, 0])):
            ops.append(set_component(id_, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=color,
                metallic=0, roughness=1, double_sided=False)))
        edit = self.world_call('world.transact', request_id=uid(1001), base_revision=revision, ops=ops)
        revision += 1
        assert edit['committed'] and edit['revision'] == revision, edit
        camera = self.world_call('entity.world_transform', id=uid(3), revision=revision)['matrix']
        assert camera[12:15] == [12, 10, 2], camera
        witnesses = [(10.75, -3.5, .5), (11.95, -2.75, .5), (12.05, -2.75, 1), (12.5, -3.25, 1)]
        probes = []
        for x, z, _ in witnesses:
            u, v = map(math.floor, projection(camera, CAPTURE_LENS, [x, height_at(x, z), z]))
            probes.append(dict(x=u, y=v))
        before = self.world.read_bytes()
        image = self.root / 'authored terrain.bmp'
        width, height = CAPTURE_SIZE
        report = self.world_call('world.capture', camera=uid(3), revision=revision, width=width, height=height,
            gpu=self.args.gpu, samples=1, path=self.native(image), scene_product_probes=probes)
        assert report['source'] == 'authored' and report['tick'] is None and report['camera_world'] == camera, report
        assert self.world.read_bytes() == before
        self.check_render('authored', image, report, camera, report['lens'], revision)
        product = report['render_diagnostics']['scene_products']
        assert product['available'] and len(product['probes']) == len(probes), product
        measurements = self.measurements['captures']['authored']['depth_probes'] = []
        for (x, z, slope), request, actual in zip(witnesses, probes, product['probes']):
            depth = expected_depth(camera, CAPTURE_LENS, request['x'], request['y'], slope)
            measurements.append(dict(world=[x, height_at(x, z), z], request=request, expected_depth=depth, readback=actual))
            assert actual['surface_valid'] and [actual['x'], actual['y']] == [request['x'], request['y']], actual
            assert math.isclose(actual['depth'], depth, rel_tol=0, abs_tol=3e-6), measurements[-1]
        return revision

    def capture_relocated_game(self, bundle, detached, revision):
        game = bundle / 'game.json'
        spec = json.loads(game.read_bytes())
        executable = bundle / spec['engine']['executable']
        manifest_digest = file_hash(game)
        frozen, source = tree_hashes(bundle), tree_hashes(detached)
        game_report = self.measurements['relocated_game'] = dict(bundle=str(bundle), manifest_sha256=manifest_digest,
            executable_sha256=frozen[spec['engine']['executable']], inventory=frozen,
            source_path_unavailable=not self.project.exists())
        assert game_report['source_path_unavailable'], game_report
        unrelated = self.root / 'Unrelated working directory'
        unrelated.mkdir()
        replay, frame, report_path = (unrelated / name for name in ('neutral tick.json', 'standalone.bmp', 'play report.json'))
        replay.write_text('[{"ticks":1}]\n', encoding='utf-8')
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable, cwd=unrelated)))
        assert inspected['revision'] == revision and tree_hashes(bundle) == frozen, inspected
        width, height = CAPTURE_SIZE
        result = success(json.loads(self.cli('game', 'run', self.native(game), '--gpu', self.args.gpu,
            '--replay', self.native(replay), '--capture', self.native(frame), '--report', self.native(report_path),
            '--width', width, '--height', height, '--samples', 1, executable=executable, cwd=unrelated)))
        assert result == json.loads(report_path.read_bytes()), result
        play = result['play']
        assert result['runtime']['tick'] == play['tick'] == 1 and play['success'] is True and play['stop_reason'] == 'replay_complete', result
        assert play['gpu'] == self.measurements['captures']['authored']['report']['gpu'], play
        presented = play['render_diagnostics']['scene_receipts']['presented']
        matrix = presented.get('camera_pose', {}).get('camera_world') or play['camera_world']
        self.check_render('relocated_game', frame, play, matrix, presented['camera_lens'], revision)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == source

    def run(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Terrain Contract')))
        baseline = self.world.read_bytes()
        baseline_store_exists = self.assets.exists()
        baseline_assets = read_inventory(self.assets)
        recipe = dict(format='poima.terrain.heightfield', version=1, samples_x=5, samples_z=3, tile_cells=2,
                      origin_m=[10, 4, -4], spacing_m=[1, 1],
                      heights_m=[base + z *.25 for z in range(3) for base in [0, .5, 1, 2, 3]])
        raw = encode(recipe)
        recipe_path = self.project / 'Terrain recipe.json'
        recipe_path.write_text(raw, encoding='utf-8')
        described, world, preview, equivalent, from_file = map(success, self.world_calls(
            rpc('world.describe.select', methods=['world.terrain.inspect', 'world.terrain.cook']),
            rpc('world.inspect'), rpc('world.terrain.inspect', recipe=raw),
            rpc('world.terrain.inspect', recipe=json.dumps(dict(reversed(list(recipe.items()))), indent=2)),
            rpc('world.terrain.inspect', source=recipe_path.name)))
        revision = world['revision']
        cook_schema = described['methods']['world.terrain.cook']
        assert 'expected_recipe_sha256' in cook_schema['required'] and {'recipe', 'source'} <= cook_schema['properties'].keys()
        assert preview == equivalent == from_file and preview['tiles_x'] == 2 and preview['tiles_z'] == 1, preview
        tiles = preview['tiles']
        assert [(row['x'], row['z'], row['primitive'], row['vertices'], row['triangles']) for row in tiles] == [
            (0, 0, 0, 9, 8), (1, 0, 0, 9, 8)], tiles
        assert tiles[0]['bounds'] == {'min': [10, 4, -4], 'max': [12, 5.5, -2]}
        assert tiles[1]['bounds'] == {'min': [12, 5, -4], 'max': [14, 7.5, -2]}
        packages = {tile['asset']: tile for tile in tiles}
        assert len(packages) == 2 and all(len(asset) == 64 for asset in packages)

        changed = copy.deepcopy(recipe)
        changed['heights_m'][0] = 1
        recipe_path.write_text(encode(changed), encoding='utf-8')
        digest = preview['recipe_sha256']
        requests = [rpc('world.terrain.cook', recipe=raw),
                    rpc('world.terrain.cook', recipe=raw, expected_recipe_sha256='Z'*64),
                    rpc('world.terrain.inspect', recipe=recipe),
                    rpc('world.terrain.inspect', recipe='{"version":1,' + raw[1:]),
                    rpc('world.terrain.cook', recipe=encode(changed), expected_recipe_sha256=digest),
                    rpc('world.terrain.unknown', recipe=raw),
                    rpc('world.terrain.inspect', recipe=raw, source=recipe_path.name),
                    rpc('world.terrain.cook', source=recipe_path.name, expected_recipe_sha256=digest)]
        failures = self.world_calls(*requests)
        for actual, code in zip(failures, [-32602, -32602, -32602, -32602, -32009, -32601, -32602, -32009]):
            error(actual, code)
        digest_error = failures[4]['error']['data']
        assert digest_error['expected_recipe_sha256'] == digest and digest_error['recipe_sha256'] != digest
        assert failures[-1]['error']['data'] == digest_error
        assert self.world.read_bytes() == baseline and read_inventory(self.assets) == baseline_assets
        assert self.assets.exists() == baseline_store_exists

        publish = self.publish_request(raw, preview)
        self.measurements['store_symlink_tested'] = self.verify_store_alias(publish)
        self.verify_existing_paths(publish, packages)
        recipe_path.write_text(raw, encoding='utf-8')
        assert self.world_call('world.terrain.cook', source=recipe_path.name, expected_recipe_sha256=digest) == preview
        recipe_path.unlink()
        assert self.world.read_bytes() == baseline
        content = read_inventory(self.assets)
        assert set(content) == {asset + '.pmodel' for asset in packages}, content.keys()
        for asset, tile in packages.items():
            binary = content[asset + '.pmodel'][0]
            assert len(binary) == tile['bytes'] and hashlib.sha256(binary).hexdigest() == asset
        assert success(self.world_calls(publish)[0]) == preview and read_inventory(self.assets) == content
        self.measurements['package_symlink_tested'] = self.verify_package_alias(publish, self.assets / (tiles[0]['asset'] + '.pmodel'))

        extra = copy.deepcopy(recipe)
        extra['origin_m'][0] = 40
        extra_raw = encode(extra)
        extra_plan = self.world_call('world.terrain.inspect', recipe=extra_raw)
        assert success(self.world_calls(self.publish_request(extra_raw, extra_plan))[0]) == extra_plan
        excluded = {row['asset'] for row in extra_plan['tiles']}
        assert not (excluded & packages.keys()) and self.world.read_bytes() == baseline

        ids = [uid(100), uid(101)]
        ops = []
        for id_, tile in zip(ids, tiles):
            part = dict(asset=tile['asset'], primitive=tile['primitive'])
            ops.extend([dict(op='entity.create', id=id_, name=f"Terrain tile {tile['x']}, {tile['z']}"),
                        dict(op='component.set', id=id_, type='StaticMesh', value=dict(part, visible=True)),
                        dict(op='component.set', id=id_, type='MeshCollider', value=dict(part, friction=.5, restitution=0))])
        authored = self.world_call('world.transact', request_id=uid(1000), base_revision=revision, ops=ops)
        assert authored['committed'] and authored['revision'] == revision+1 and authored['changed_ids'] == ids

        def check_closure():
            closure = self.world_call('world.dependencies')
            expected = [dict(filename=asset+'.pmodel', sha256=asset, bytes=packages[asset]['bytes']) for asset in sorted(packages)]
            assert closure['revision'] == revision+1 and closure['assets'] == expected, closure
            inspected = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
            assert inspected['assets'] == expected and inspected['revision'] == revision+1
            refs = self.world_call('world.asset.references', revision=revision+1)
            terrain_refs = {(edge['owner']['id'], edge['component'], edge['asset'], edge['subresource']['index'])
                            for edge in refs['edges'] if edge['asset'] in packages}
            assert terrain_refs == {(id_, name, tile['asset'], 0) for id_, tile in zip(ids, tiles)
                                    for name in ('StaticMesh', 'MeshCollider')}, refs
            return expected

        self.measurements['closure'] = check_closure()
        self.measurements['recipe_sha256'] = digest
        self.measurements['tiles'] = tiles
        persisted = self.world.read_bytes()
        scene = json.loads(persisted)
        for id_, tile in zip(ids, tiles):
            components = scene['entities'][id_]['components']
            assert components['StaticMesh'] == dict(asset=tile['asset'], primitive=0, visible=True)
            assert components['MeshCollider'] == dict(asset=tile['asset'], primitive=0, friction=.5, restitution=0)

        if self.args.runtime:
            self.measurements['rays'] = self.raycast(revision+1, uid(800), ids)
        old_project, self.project = self.project, self.root / 'Relocated Terrain'
        old_project.rename(self.project)
        self.manifest = self.project / self.manifest.name
        self.world, self.assets = self.project / self.world.name, self.project / self.assets.name
        assert not old_project.exists() and self.world.read_bytes() == persisted
        assert check_closure() == self.measurements['closure']
        relocated_assets = read_inventory(self.assets)
        assert success(self.world_calls(publish)[0]) == preview and read_inventory(self.assets) == relocated_assets
        if self.args.runtime:
            assert self.raycast(revision+1, uid(801), ids) == self.measurements['rays']
            self.check_cold_save(revision+1, self.measurements['rays'])
        self.measurements['relocated_project_reopened'] = True

        export_revision = revision+1
        if self.args.capture_output:
            export_revision = self.capture_authored(export_revision, ids)
        if self.args.runtime_root:
            output = self.root / 'Exported Terrain'
            success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                      '--runtime', self.native(self.args.runtime_root))))
            models = list(output.rglob('*.pmodel'))
            files = {file.stem: file for file in models}
            assert len(models) == len(packages) and files.keys() == packages.keys()
            for asset, path in files.items():
                assert path.read_bytes() == (self.assets / path.name).read_bytes()
            detached = self.root / 'Retained Source Outside Export'
            self.project.rename(detached)
            assert not self.project.exists() and not old_project.exists()
            if self.args.capture_output:
                bundle = self.root / 'Moved game' / 'Terrain'
                bundle.parent.mkdir()
                output.rename(bundle)
                assert not output.exists()
                packaged_entities = json.loads((bundle/'content'/'world.json').read_bytes())['entities']
                source_entities = json.loads((detached/self.world.name).read_bytes())['entities']
                for id_ in ids:
                    assert packaged_entities[id_] == source_entities[id_]
                self.capture_relocated_game(bundle, detached, export_revision)
            else:
                success(json.loads(self.cli('game', 'inspect', self.native(output / 'game.json'))))
            self.measurements['exported'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--runtime-root', type=Path, help='Optional matching installed runtime for an actual export.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture-output', type=Path, help='Retain an authored and relocated Windows Vulkan capture run here.')
    parser.add_argument('--gpu', type=int, default=0, metavar='INDEX')
    parser.add_argument('--evidence', type=Path, help='Write exact commands, replies and measurements.')
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires an index from 0 through 4095.')
    if args.capture_output and not (args.runtime_root and args.runtime):
        parser.error('--capture-output requires --runtime-root and native simulation.')

    def execute(directory):
        test = TerrainContract(args, Path(directory))
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
            if args.capture_output:
                evidence = Path(directory) / 'evidence.json'
                evidence.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
                print(f'Evidence file {evidence}')
            if args.evidence:
                args.evidence.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')

    if args.capture_output:
        args.capture_output.mkdir(parents=True, exist_ok=True)
        execute(tempfile.mkdtemp(prefix=f'gpu-{args.gpu}-', dir=args.capture_output.resolve()))
    else:
        scratch = Path(__file__).resolve().parents[1] / 'build' / 'world-terrain-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            execute(directory)
    print('Terrain world contract passed exact publication, guarded retries, authored references and relocated closure.' +
          (' Native raycasts hit both interiors and the tile seam.' if args.runtime else '') +
          (' Authored and relocated Vulkan captures matched the projected terrain.' if args.capture_output else ''))


if __name__ == '__main__':
    main()
