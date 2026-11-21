#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cook, navigate, edit and export a two-tile ramp with retained native evidence."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time
import traceback


ROOT = Path(__file__).resolve().parents[1]
PROFILE = dict(radius=.3, height=1.8, climb=.3, slope=45, cell_size=.125, cell_height=.05)
TILES = ('0' * 30 + '64', '0' * 30 + '65')
SECTIONS = [(-4.5, 4.5), (-4.5, 0), (-4.5, 3), (4.5, -4.5)]
POINTS = [(-2.25, .75), (0, .75), (4.5, .75)]


def encode(value):
    return json.dumps(value, separators=(',', ':'), allow_nan=False)


def sha256(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def recipe(slope):
    return dict(format='poima.terrain.heightfield', version=1, samples_x=17, samples_z=9,
                tile_cells=8, origin_m=[-6, 0, -3], spacing_m=[.75, .75],
                heights_m=[max(0, -6 + .75 * column) * slope for _ in range(9) for column in range(17)])


def request(method, **params):
    return dict(jsonrpc='2.0', method=method, params=params)


def checked(reply, error=None):
    if error is None:
        assert 'result' in reply and 'error' not in reply, reply
        return reply['result']
    assert reply.get('error', {}).get('code') == error, reply
    return reply['error']


def near(actual, expected, tolerance):
    assert math.isclose(actual, expected, rel_tol=0, abs_tol=tolerance), (actual, expected, tolerance)


def tile_bindings(plan, create=False):
    operations = []
    for entity, tile in zip(TILES, plan['tiles'], strict=True):
        if create:
            operations.append(dict(op='entity.create', id=entity, name=f"Bank tile {tile['x']}"))
        part = dict(asset=tile['asset'], primitive=tile['primitive'])
        operations.extend([
            dict(op='component.set', id=entity, type='StaticMesh', value=dict(part, visible=True)),
            dict(op='component.set', id=entity, type='MeshCollider', value=dict(part, friction=.5, restitution=0)),
        ])
    return operations


def set_navigation(asset):
    return dict(op='navigation.set', asset=asset)


class Fixture:
    def __init__(self, binary, output, runtime_root, export_timeout):
        self.binary = binary.resolve(strict=True)
        self.root = output.resolve()
        self.root.mkdir()
        self.runtime_root = runtime_root.resolve(strict=True) if runtime_root else None
        self.export_timeout = export_timeout
        self.project = self.root / 'Ramp'
        self.manifest = self.project / 'project.poimaproject'
        self.world = self.project / 'world.json'
        self.assets = self.project / 'world.json.assets'
        self.calls = []
        self.next_transaction = 100
        self.measurements = {}

    def cli(self, *arguments, stdin=None, code=0, timeout=45):
        argv = [str(self.binary), *map(str, arguments)]
        evidence = dict(argv=argv, stdin=stdin)
        self.calls.append(evidence)
        started = time.monotonic()
        try:
            child = subprocess.run(argv, input=stdin, capture_output=True, text=True,
                                   encoding='utf-8', timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            evidence.update(timeout=exc.timeout, terminated_by_timeout=True, stdout=exc.stdout, stderr=exc.stderr)
            for stream in ('stdout', 'stderr'):
                if isinstance(evidence[stream], bytes):
                    evidence[stream] = evidence[stream].decode('utf-8', 'replace')
            raise
        finally:
            evidence['elapsed_seconds'] = round(time.monotonic() - started, 3)
        evidence.update(exit_code=child.returncode, stdout=child.stdout, stderr=child.stderr)
        assert child.returncode == code, evidence
        return [json.loads(line) for line in child.stdout.splitlines()]

    def command(self, *arguments, error=None, timeout=45):
        result, = self.cli(*arguments, code=4 if error else 0, timeout=timeout)
        if error:
            assert result['status'] == 'error' and error in result['diagnostics'][0]['message'].lower(), result
            return result['diagnostics'][0]
        assert result['status'] == 'ok', result
        return result['result']

    def rpc_batch(self, *requests):
        # Address the raw world during stale-binding recovery. The project manifest
        # route validates the complete freeze before opening the authoring owner.
        rows = [dict(row, id=i) for i, row in enumerate((*requests, request('session.close')), 1)]
        replies = self.cli('world', self.world, stdin=''.join(encode(row) + '\n' for row in rows))
        assert [row.get('id') for row in replies] == [row['id'] for row in rows], replies
        assert checked(replies.pop())['closed'] is True
        return replies

    def rpc(self, method, *, error=None, **params):
        reply, = self.rpc_batch(request(method, **params))
        return checked(reply, error=error)

    def transact(self, revision, operations, error=None):
        transaction = self.next_transaction
        self.next_transaction += 1
        result = self.rpc('world.transact', base_revision=revision, ops=operations,
                          request_id=f'{transaction:032x}', error=error)
        if error is not None:
            return result
        assert result['committed'] is True and result['revision'] == revision + 1, result
        return result['revision']

    def cook(self, slope, label):
        raw = encode(recipe(slope))
        (self.root / f'{label}-recipe.json').write_text(raw + '\n', encoding='utf-8')
        before = sha256(self.world)
        plan = self.rpc('world.terrain.inspect', recipe=raw)
        assert (plan['tiles_x'], plan['tiles_z']) == (2, 1), plan
        assert [(tile['x'], tile['z'], tile['vertices'], tile['triangles']) for tile in plan['tiles']] == [
            (0, 0, 81, 128), (1, 0, 81, 128)], plan
        assert self.rpc('world.terrain.cook', recipe=raw, expected_recipe_sha256=plan['recipe_sha256']) == plan
        assert sha256(self.world) == before
        for tile in plan['tiles']:
            path = self.assets / f"{tile['asset']}.pmodel"
            assert path.stat().st_size == tile['bytes'] and sha256(path) == tile['asset'], tile
        return plan

    def bake(self, revision, plan):
        before = sha256(self.world)
        first = self.rpc('world.navigation.bake', revision=revision, profile=PROFILE)
        second = self.rpc('world.navigation.bake', revision=revision, profile=PROFILE)
        assert first['asset'] == second['asset'] and second['reused'] is True, (first, second)
        assert first['source_fingerprint'] == second['source_fingerprint'] and sha256(self.world) == before
        statistics = first['metadata']['statistics']
        assert statistics['source_triangles'] == sum(tile['triangles'] for tile in plan['tiles']), first
        assert statistics['source_vertices'] == sum(tile['vertices'] for tile in plan['tiles']), first
        assert sha256(self.assets / f"{first['asset']}.pnav") == first['asset']
        return first

    def path_request(self, revision, asset, slope, start, end):
        a, b = ([x, max(0, x) * slope + .05, 0] for x in (start, end))
        return request('world.navigation.path', revision=revision, asset=asset, start=a, end=b,
                       extents=[.25, .3, .25], max_polygons=64, max_corners=16, max_nodes=512)

    def routes(self, revision, navigation, slope):
        rows = [self.path_request(revision, navigation['asset'], slope, *pair) for pair in SECTIONS]
        routes = []
        for (start, end), reply in zip(SECTIONS, self.rpc_batch(*rows), strict=True):
            selected = checked(reply)
            assert selected['revision'] == revision and selected['source_fingerprint'] == navigation['source_fingerprint'], selected
            route = selected['path']
            assert route['status'] == 'complete' and route['complete'] is True, route
            # Recast's endpoint heights are quantized. The exact terrain values
            # are verified independently by Jolt rays against the same meshes.
            for key, x in (('projected_start', start), ('projected_end', end), ('reachable_end', end)):
                actual_x, actual_height, actual_z = route[key]
                near(actual_x, x, .01)
                near(actual_z, 0, .01)
                near(actual_height, max(0, x) * slope, .15)
            assert len(route['corners']) >= 2 and route['polygons'] >= 1, route
            direction = 1 if start < end else -1
            assert all((b[0]-a[0]) * direction >= -.01 for a, b in zip(route['corners'], route['corners'][1:])), route
            near(route['corners'][0][0], start, .01)
            near(route['corners'][-1][0], end, .01)
            routes.append(route)
        return routes

    def rays(self, revision, slope, session):
        rows = [request('runtime.start', session_id=session, revision=revision)]
        rows.extend(request('runtime.raycast', session_id=session, tick=0, origin=[x, 5, z],
                            direction=[0, -1, 0], distance=10) for x, z in POINTS)
        initial, *rays = self.rpc_batch(*rows)
        assert checked(initial)['session_id'] == session
        observations = []
        for (x, z), reply in zip(POINTS, rays, strict=True):
            hit = checked(reply)['hit']
            assert hit and hit['entity'] in TILES and type(hit['triangle']) is int and 0 <= hit['triangle'] < 128, hit
            if x:
                assert hit['entity'] == TILES[x > 0], hit
            expected_height = max(0, x) * slope
            for got, expected in zip(hit['position'], [x, expected_height, z], strict=True):
                near(got, expected, 2e-5)
            local_slope = slope if hit['entity'] == TILES[1] else 0
            norm = math.hypot(1, local_slope)
            for got, expected in zip(hit['normal'], [-local_slope/norm, 1/norm, 0], strict=True):
                near(got, expected, 2e-5)
            observations.append(hit)
        return observations

    def closure(self, revision, plan, navigation):
        paths = {f"{tile['asset']}.pmodel" for tile in plan['tiles']} | {f"{navigation['asset']}.pnav"}
        expected = [dict(filename=name, sha256=sha256(self.assets / name), bytes=(self.assets / name).stat().st_size)
                    for name in sorted(paths)]
        dependencies = self.rpc('world.dependencies')
        assert dependencies['revision'] == revision and dependencies['assets'] == expected and dependencies['needs_navigation'] is True, dependencies
        project = self.command('project', 'inspect', self.manifest)
        assert project['revision'] == revision and project['assets'] == expected, project
        return expected

    def export(self, revision, closure, navigation):
        if self.runtime_root is None:
            return {'status': 'skipped_matching_renderer_runtime_not_supplied'}
        descriptor = json.loads((self.runtime_root / 'runtime.json').read_text(encoding='utf-8'))
        assert descriptor['engine_version'] == self.measurements['version']['version'], descriptor
        assert descriptor['features']['renderer'] is True and descriptor['features']['navigation'] is True, descriptor
        output = self.root / 'Standalone Ramp'
        result = self.command('project', 'build', self.manifest, '--output', output, '--runtime', self.runtime_root,
                              timeout=self.export_timeout)
        assert result['revision'] == revision, result
        manifest = output / 'game.json'
        game = json.loads(manifest.read_text(encoding='utf-8'))
        expected = [dict(path=f"content/world.json.assets/{entry['filename']}", role='asset',
                         sha256=entry['sha256'], size=entry['bytes']) for entry in closure]
        actual = [entry for entry in game['files'] if entry['path'].startswith('content/world.json.assets/')]
        assert expected == actual, (expected, actual)
        for entry in expected:
            asset = output / entry['path']
            assert sha256(asset) == entry['sha256'] and asset.stat().st_size == entry['size'], entry
        assert game['source']['revision'] == revision and game['source']['world_sha256'] == sha256(self.world), game
        frozen = json.loads((output / game['entry']['world']).read_text(encoding='utf-8'))
        assert frozen['navigation'] == {'asset': navigation['asset']}
        assert any(entry['path'] == 'runtime/share/poima/licenses/RecastNavigation/License.txt' for entry in game['files'])

        # Relocate only the fixture-owned bundle and withhold its authored source.
        relocated = self.root / 'Relocated Ramp'
        output.rename(relocated)
        self.project.rename(self.root / 'Retained Ramp Source')
        assert not self.project.exists()
        inspected = self.command('game', 'inspect', relocated / 'game.json', timeout=self.export_timeout)
        return dict(status='validated_relocated_bundle', runtime_target=descriptor['target_os'], publication=result,
                    runtime_descriptor_sha256=sha256(self.runtime_root / 'runtime.json'),
                    runtime_binary_sha256=sha256(self.runtime_root / descriptor['executable']),
                    manifest_sha256=sha256(relocated / 'game.json'), inventory=expected, inspection=inspected)

    def run(self):
        binary_sha = sha256(self.binary)
        version = self.command('--version')
        self.measurements.update(version=version, executable_sha256=binary_sha)
        self.command('project', 'create', self.project, '--name', 'Terrain Navigation Ramp')
        layout = json.loads(self.world.read_text(encoding='utf-8'))
        project = json.loads(self.manifest.read_text(encoding='utf-8'))
        floor, = (entity for entity, value in layout['entities'].items() if value['name'] == 'Floor')
        controller = project['entry']['controller']
        transform = layout['entities'][controller]['components']['Transform']
        transform = dict(transform, position=[-4.5, 1, 0])
        original = self.cook(1/6, 'baseline')
        baseline_revision = self.transact(layout['revision'], [
            dict(op='entity.delete', id=floor, recursive=False),
            dict(op='component.set', id=controller, type='Transform', value=transform),
            *tile_bindings(original, create=True),
        ])
        first = self.bake(baseline_revision, original)
        baseline_bound = self.transact(baseline_revision, [set_navigation(first['asset'])])
        baseline_routes = self.routes(baseline_bound, first, 1/6)
        baseline_rays = self.rays(baseline_bound, 1/6, 'f' * 32)
        self.measurements['baseline'] = dict(plan=original, navigation=first, routes=baseline_routes, rays=baseline_rays,
                                             closure=self.closure(baseline_bound, original, first))

        changed = self.cook(1/3, 'revised')
        assert changed['recipe_sha256'] != original['recipe_sha256']
        assert changed['tiles'][1]['asset'] != original['tiles'][1]['asset']
        assert changed['tiles'][1]['bounds']['max'][1] == 2 and original['tiles'][1]['bounds']['max'][1] == 1
        edited_revision = self.transact(baseline_bound, tile_bindings(changed))
        edited_hash = sha256(self.world)
        stale_path, stale_closure = self.rpc_batch(self.path_request(edited_revision, first['asset'], 1/3, -4.5, 4.5),
                                                   request('world.dependencies'))
        self.measurements['stale'] = dict(path=checked(stale_path, error=-32009),
                                          dependencies=checked(stale_closure, error=-32009))
        assert sha256(self.world) == edited_hash
        self.measurements['stale']['project'] = self.command('project', 'inspect', self.manifest, error='stale')
        if self.runtime_root is not None:
            output = self.root / 'Forbidden Stale Export'
            self.measurements['stale']['export'] = self.command('project', 'build', self.manifest, '--output', output,
                                                              '--runtime', self.runtime_root, error='stale')
            assert not output.exists()

        second = self.bake(edited_revision, changed)
        assert second['asset'] != first['asset'] and second['source_fingerprint'] != first['source_fingerprint'], (first, second)
        bound = self.transact(edited_revision, [set_navigation(second['asset'])])
        updated_routes = self.routes(bound, second, 1/3)
        updated_rays = self.rays(bound, 1/3, 'e' * 32)
        near(updated_rays[-1]['position'][1] - baseline_rays[-1]['position'][1], .75, 2e-5)
        near(updated_routes[0]['projected_end'][1] - baseline_routes[0]['projected_end'][1], .75, .15)
        closure = self.closure(bound, changed, second)
        assert sha256(self.assets / f"{first['asset']}.pnav") == first['asset']
        self.measurements['revised'] = dict(plan=changed, navigation=second, routes=updated_routes,
                                            rays=updated_rays, closure=closure)
        self.measurements['export'] = self.export(bound, closure, second)
        assert sha256(self.binary) == binary_sha, 'Native executable changed during the fixture.'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/linux-game/poima')
    parser.add_argument('--output', type=Path, required=True, help='New directory for this attempt and its raw evidence.')
    parser.add_argument('--runtime-root', type=Path, help='Existing matching simulation/renderer/navigation distribution.')
    parser.add_argument('--export-timeout', type=int, default=180,
                        help='Seconds allowed per export and game inspection operation (default 180).')
    args = parser.parse_args()
    if args.export_timeout <= 0:
        parser.error('--export-timeout must be positive')
    fixture = Fixture(args.binary, args.output, args.runtime_root, args.export_timeout)
    test_sha = sha256(Path(__file__))
    passed, failure = False, None
    try:
        fixture.run()
        assert sha256(Path(__file__)) == test_sha, 'Fixture source changed during the run.'
        passed = True
    except Exception:
        failure = traceback.format_exc()
        raise
    finally:
        evidence = dict(passed=passed, failure=failure, test_sha256=test_sha,
                        calls=fixture.calls, measurements=fixture.measurements)
        (fixture.root / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(f"Terrain navigation passed with {len(fixture.calls)} native CLI owners and {fixture.measurements['export']['status']}.")


if __name__ == '__main__':
    main()
