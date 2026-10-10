#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare a bounded, reproducible workload from an authored audio-enabled Relay Yard.

Copies the supplied world and cooked store, then authors through the native service.
No download, import, build, gameplay execution, rendering or input injection occurs.
The manifest reports authored workload; measured visibility and timings are separate.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient

RECIPE = 'performance-yard-v1'
ROUTE = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76'
CADENCE = 'c0870000000000000000000000000001'
PERFORMANCE = 'c0940000000000000000000000000001'
WAYPOINTS = [[-7, 0, 6.5], [7, 0, 6.5], [7, 0, -6.5], [-7, 0, -6.5]]
PALETTE = [[.28, .08, .11], [.24, .22, .19], [.15, .25, .23], [.31, .27, .18]]
MAX_DOCUMENT = 16 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)


def uid(number):
    return f'{number:032x}'


def identity(group, index=0):
    return f'e094{group:04x}{index:024x}'


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path):
    require(Path(path).stat().st_size <= MAX_DOCUMENT, 'JSON exceeds 16 MiB.')

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON key: ' + key)
            result[key] = value
        return result

    def invalid(value):
        raise ValueError('Nonfinite JSON number: ' + value)

    def floating(value):
        result = float(value)
        require(math.isfinite(result), 'Nonfinite JSON number.')
        return result

    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_float=floating, parse_constant=invalid)


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False,
                                    allow_nan=False) + '\n', encoding='utf-8')


def inventory(directory):
    """Bound the copied closure and reject links, special files and case collisions."""
    require(directory.is_dir() and not directory.is_symlink(), 'Missing regular cooked store.')
    result, seen, total = [], set(), 0
    for path in sorted(directory.rglob('*')):
        mode = path.lstat().st_mode
        require(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)() and
                (stat.S_ISDIR(mode) or stat.S_ISREG(mode)),
                'Cooked store contains a link or special file.')
        relative = path.relative_to(directory).as_posix()
        require(relative.casefold() not in seen, 'Cooked-store case collision.')
        seen.add(relative.casefold())
        if path.is_dir():
            continue
        size = path.stat().st_size
        total += size
        require(size <= 64 * 1024 * 1024 and total <= 1024 * 1024 * 1024 and len(result) < 50000,
                'Cooked store exceeds the bounded copy budget.')
        result.append(dict(path=relative, bytes=size, sha256=sha(path)))
    require(result, 'Empty cooked store.')
    return result


def transform(position, scale=(1, 1, 1), rotation=(0, 0, 0, 1)):
    return dict(position=list(position), rotation=list(rotation), scale=list(scale))


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def create(entity, name, pose):
    return [dict(op='entity.create', id=entity, name=name, parent=None),
            component(entity, 'Transform', pose)]


def box(entity, name, pose, color, dynamic=False):
    operations = create(entity, name, pose)
    operations += [component(entity, 'MeshRenderer', dict(primitive='box', visible=True, albedo=color)),
                   component(entity, 'PbrMaterial', dict(base_color=color, emissive=[0, 0, 0],
                                                        metallic=.05, roughness=.75, double_sided=False))]
    if dynamic:
        operations.append(component(entity, 'BoxCollider', dict(half_extents=[.5, .5, .5],
            motion='dynamic', mass=5, friction=.55, restitution=.15)))
    return operations


class Author:
    def __init__(self, client, record, recipe_hash):
        self.client, self.record, self.recipe_hash = client, record, recipe_hash
        self.deadline, self.transaction = time.monotonic() + 1800, 0
        self.revision = self.rpc('world.inspect', {})['revision']

    def rpc(self, method, params):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, 'Authoring deadline exceeded.')
        row = dict(method=method, params=copy.deepcopy(params))
        self.record['calls'].append(row)
        try:
            result = self.client.call(method, params, timeout=min(120, remaining))
        except BaseException as error:
            row['error'] = repr(error)
            raise
        row['result'] = result
        return result

    def transact(self, operations):
        require(1 <= len(operations) <= 256, 'Transaction recipe exceeds 256 operations.')
        request = hashlib.sha256(f'{self.recipe_hash}/{self.transaction}'.encode()).hexdigest()[:32]
        self.transaction += 1
        result = self.rpc('world.transact', dict(request_id=request, base_revision=self.revision,
                                               ops=operations))
        self.revision = result['revision']

    def batches(self, recipes):
        operations = []
        for recipe in recipes:
            if len(operations) + len(recipe) > 256:
                self.transact(operations)
                operations = []
            operations.extend(recipe)
        if operations:
            self.transact(operations)

    def page(self, method, params):
        result, offset = [], 0
        for _ in range(1024):
            response = self.rpc(method, dict(params, offset=offset, limit=64))
            result.extend(response['items'])
            following = response['next_offset']
            if following is None:
                return result
            require(type(following) is int and following > offset, 'Asset page did not advance.')
            offset = following
        raise ValueError('Asset pagination exceeded 65,536 items.')

    def query(self, kind):
        result, after = [], None
        for _ in range(256):
            params = dict(revision=self.revision, component=kind, limit=256)
            if after is not None:
                params['after'] = after
            response = self.rpc('entity.query', params)
            result.extend(response['entities'])
            following = response['next_after']
            if following is None:
                return result
            require(isinstance(following, str) and (after is None or following > after),
                    'Entity page did not advance.')
            after = following
        raise ValueError('Entity pagination exceeded 65,536 items.')

    def get(self, entity, kind):
        return self.rpc('entity.get', dict(revision=self.revision, id=entity, component=kind))['value']


def licensed_base(author, document):
    """Require the authored sample boundary rather than accepting an empty benchmark."""
    entities = document['entities']
    for entity in (1, 2, 100, 101, 102, 300, 400, 600, 610, 611, 612, 613, 620, 621, 622, 623):
        require(uid(entity) in entities, 'Base is missing a required Relay Yard entity.')
    require(document.get('navigation') and document.get('ui') and document.get('templates'),
            'Base lacks Relay Yard navigation, UI or templates.')
    require(entities[uid(1)]['components']['Transform'] == transform([0, -.5, 0], [20, 1, 20]),
            'Original 20-meter floor transform is required.')
    floor = entities[uid(1)]['components']['BoxCollider']
    require(floor['motion'] == 'static' and floor['half_extents'] == [.5, .5, .5],
            'Original scaled floor collision is required.')
    require('game:' + ROUTE not in entities[uid(100)]['components'] and
            'game:' + PERFORMANCE not in entities[uid(100)]['components'] and
            PERFORMANCE not in document.get('component_schemas', {}), 'Base already has a performance route.')
    require(CADENCE in document.get('component_schemas', {}) and
            'game:' + CADENCE in entities[uid(100)]['components'] and
            'game:' + CADENCE in entities[uid(300)]['components'], 'Base lacks native audio cadence.')
    require(not author.query('Light') and not author.query('LightingEnvironment'),
            'Use the original Relay Yard without authored lighting.')
    require(len(author.query('AnimationRig')) == 1 and len(author.query('SkinnedMesh')) == 1,
            'Use the original one-character Relay Yard.')
    require(not any(value['components'].get('BoxCollider', {}).get('motion') == 'dynamic'
                    for value in entities.values()), 'Base already contains dynamic boxes.')
    for kind in ('CharacterController', 'Transform'):
        author.get(uid(100), kind)
    rig = author.get(uid(400), 'AnimationRig')
    require(entities[uid(400)]['parent'] == uid(300), 'Original courier rig attachment changed.')
    asset = rig['asset']
    meshes = author.query('SkinnedMesh')
    mesh = author.get(meshes[0]['id'], 'SkinnedMesh')
    require(mesh['rig'] == uid(400) and mesh['asset'] == asset and mesh['visible'],
            'Original imported character mesh does not match its rig.')
    clips = author.page('asset.inspect', dict(asset=asset, section='animations'))
    idle = [item for item in clips if item['name'] == 'Idle']
    run = [item for item in clips if item['name'] == 'Run']
    require(len(idle) == len(run) == 1 and idle[0]['duration'] > 0,
            'Original imported Idle/Run clips are required.')
    primitive = next(item for item in author.page('asset.inspect', dict(asset=asset, section='primitives'))
                     if item['index'] == mesh['primitive'])
    require(primitive['skinned'], 'Character primitive has no native skin weights.')
    vertices = author.page('asset.animation.sample', dict(asset=asset, node=mesh['node'],
        primitive=mesh['primitive'], section='vertices', clip=idle[0]['index'], time=0, loop=True))
    require(len(vertices) == primitive['vertices'] and vertices, 'Skin vertex observation is incomplete.')
    for vertex in vertices:
        weights = vertex.get('weights', [])
        require(weights and all(math.isfinite(weight) and weight >= 0 for weight in weights)
                and abs(sum(weights) - 1) <= 1e-5, 'Imported skin weights are not normalized positive data.')
    assets = {asset}
    for entity in (610, 611, 612, 613, 620, 621, 622, 623):
        emitter = author.get(uid(entity), 'AudioEmitter')
        require(emitter['enabled'], 'Base audio emitter is disabled.')
        author.rpc('asset.audio.inspect', dict(asset=emitter['asset']))
        assets.add(emitter['asset'])
    declarations = {}
    for item in sorted(assets):
        records = document.get('asset_provenance', {}).get(item, [])
        require(records, 'Imported model/audio lacks retained license declarations.')
        declarations[item] = []
        for record in records:
            metadata = author.rpc('asset.provenance.inspect', dict(record=record))['metadata']
            require(metadata['asset'] == item and metadata['license']['identifier'] == 'CC0-1.0',
                    'This sample requires its original CC0 model/audio declarations.')
            declarations[item].append(dict(record=record, metadata=metadata))
    return dict(asset=asset, idle_clip=idle[0]['index'], run_clip=run[0]['index'],
        idle_duration=idle[0]['duration'], node=mesh['node'], primitive=mesh['primitive'],
        vertices_per_rig=primitive['vertices'], triangles_per_rig=primitive['triangles'],
        positive_weight_vertices_per_rig=len(vertices), declarations=declarations,
        attachment=author.get(uid(400), 'Transform'))


def populate(author, args, character, schema):
    rigs = [uid(400)]
    rig_recipes = []
    for index in range(args.rigs - 1):
        entity = identity(1, index)
        rigs.append(entity)
        side, rank = index % 2, index // 2
        count = (args.rigs - 1 + 1 - side) // 2
        z = -6.5 + 13 * rank / max(1, count - 1)
        pose = transform([(-1 if side == 0 else 1) * 6.2, character['attachment']['position'][1], z],
                         rotation=[0, (-1 if side == 0 else 1) * math.sqrt(.5), 0, math.sqrt(.5)])
        rig_recipes.append([dict(op='asset.instantiate', id=entity, name=f'Idle spectator {index + 1}',
            asset=character['asset'], parent=None), component(entity, 'Transform', pose),
            component(entity, 'AnimationRig', dict(asset=character['asset'], clip=character['idle_clip'],
                loop=True, playing=True, speed=1, time=character['idle_duration'] * index / (args.rigs - 1)))])
    author.batches(rig_recipes)
    props = [identity(2, index) for index in range(args.props)]
    recipes = []
    for index, entity in enumerate(props):
        side, column, row, layer = index % 2, (index // 2) % 4, (index // 8) % 24, index // 192
        x = (-1 if side == 0 else 1) * (7.6 + .6 * column)
        recipes.append(box(entity, f'Shared box detail {index + 1}',
            transform([x, .275 + .6 * layer, -9 + 18 * row / 23], [.55, .55, .55]), PALETTE[index % 4]))
    author.batches(recipes)
    dynamics = [identity(3, index) for index in range(args.dynamic_bodies)]
    recipes = []
    for index, entity in enumerate(dynamics):
        corner, position = index % 4, index // 4
        x = (-1 if corner % 2 == 0 else 1) * (5.6 + .55 * (position % 2))
        z = (-1 if corner < 2 else 1) * (7.5 + .6 * (position // 2))
        recipes.append(box(entity, f'Dynamic box {index + 1}',
            transform([x, 1.1 + .15 * (position % 2), z], [.5, .5, .5]), PALETTE[(index + 1) % 4], True))
    author.batches(recipes)
    sun, environment = identity(4), identity(5)
    shadow = dict(enabled=True, near=.05, distance=30, bias=.0001, normal_bias=.005)
    lights = [sun]
    recipes = [create(sun, 'Yard Sun', transform([0, 8, 0], rotation=[-.5, 0, 0, math.sqrt(.75)])) +
               [component(sun, 'Light', dict(kind='directional', color=[1, .91, .78],
                                            intensity=2.2, enabled=True, shadow=shadow))]]
    for index in range(4):
        entity = identity(4, index + 1)
        lights.append(entity)
        recipes.append(create(entity, f'Corner spotlight {index + 1}', transform(
            [(-1 if index % 2 == 0 else 1) * 6, 7, (-1 if index < 2 else 1) * 6],
            rotation=[-math.sqrt(.5), 0, 0, math.sqrt(.5)])) + [component(entity, 'Light', dict(
                kind='spot', color=[1, .82, .68], intensity=45, enabled=True, range=16,
                inner_angle=25, outer_angle=50, shadow=shadow))])
    for index in range(args.lights - 5):
        entity = identity(4, index + 5)
        lights.append(entity)
        recipes.append(create(entity, f'Finite point light {index + 1}', transform(
            [-8.75 + 2.5 * (index % 8), 3.8 + .8 * (index % 2), -8.75 + 2.5 * (index // 8)])) +
            [component(entity, 'Light', dict(kind='point', color=[1, .72, .58] if index % 2 == 0 else [.62, .78, 1],
                                            intensity=6, enabled=True, range=4.3))])
    recipes.append(create(environment, 'Yard environment', transform([0, 0, 0])) +
        [component(environment, 'LightingEnvironment', dict(ambient=[.05, .06, .08], exposure=1,
            shadow_resolution=512, sky=dict(enabled=True, zenith=[.06, .15, .3], horizon=[.55, .65, .75],
                ground=[.12, .10, .08], horizon_falloff=.35, sun=sun, sun_size_degrees=.53, sun_intensity=20)))])
    author.batches(recipes)
    # Import only the new schema. Every original Relay field/catalog stays exact.
    values = {field['id']: field['default'] for field in schema['fields']}
    values[uid(1)] = int(args.autonomous)
    author.transact([dict(op='component.schema.set', schema=schema),
        component(uid(100), 'game:' + ROUTE, {uid(1): [], uid(2): 0}),
        component(uid(100), 'game:' + PERFORMANCE, values)])
    return dict(rigs=rigs, props=props, dynamics=dynamics, lights=lights, environment=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'base-world', 'manifest', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--rigs', type=int, choices=range(16, 33), default=24, metavar='16..32')
    parser.add_argument('--props', type=int, choices=range(500, 1001), default=768, metavar='500..1000')
    parser.add_argument('--dynamic-bodies', type=int, choices=range(0, 33), default=32, metavar='0..32')
    parser.add_argument('--lights', type=int, choices=(32, 64), default=64)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--autonomous', action='store_true',
        help='Opt in to compiled native navigation/movement; player.start must omit its controller.')
    args = parser.parse_args()
    base = args.base_world.resolve(strict=True)
    store = Path(str(base) + '.assets')
    output = args.output.resolve()
    require(args.binary.is_file() and base.is_file() and not args.base_world.is_symlink(), 'Supply existing regular inputs.')
    require(not args.output.exists() and not args.output.is_symlink(), 'Use a new output directory; failures are retained.')
    require(output != base and not output.is_relative_to(store) and not base.is_relative_to(output),
            'Output overlaps the immutable input closure.')
    original_hash, original_store = sha(base), inventory(store)
    require(args.manifest.is_file() and not args.manifest.is_symlink(), 'Supply the genuine gameplay component manifest.')
    schema_hash = sha(args.manifest)
    schemas = read_json(args.manifest)
    require(schemas.get('format') == 'poima.components' and schemas.get('version') == 1 and
            isinstance(schemas.get('schemas'), list), 'Invalid gameplay component manifest.')
    selected = [value for value in schemas['schemas'] if value.get('id') == PERFORMANCE]
    require(len(selected) == 1, 'Manifest must contain exactly one PerformanceRoute schema.')
    schema = selected[0]
    require(schema['version'] == 1 and len(schema['fields']) == 18 and
            [field['id'] for field in schema['fields']] == [uid(index) for index in range(1, 19)],
            'PerformanceRoute must be the original 18-field schema.')
    expected_kinds = ['int32', 'int32', 'int64', 'int64', 'int32', 'int64', 'float64', 'float64',
                      'float64', 'float64', 'float64', 'int64', 'int64', 'int64', 'int32', 'int32', 'int32', 'int32']
    require([field['kind'] for field in schema['fields']] == expected_kinds, 'PerformanceRoute field kinds changed.')
    expected_names = ['Enabled', 'Corner', 'Laps', 'Samples', 'PreviousValid', 'PreviousTick',
        'PreviousX', 'PreviousY', 'PreviousZ', 'TravelMeters', 'LastDisplacement', 'StageCommands',
        'Plans', 'MotionTicks', 'LastPathStatus', 'RouteValid', 'LastCornerCount', 'FirstCornerReached']
    require([field['name'] for field in schema['fields']] == expected_names, 'PerformanceRoute field names changed.')
    for index, field in enumerate(schema['fields']):
        expected = -1 if index == 14 else 0
        value = str(expected) if field['kind'] == 'int64' else expected
        require(field['default'] == value, 'PerformanceRoute default state changed.')
    document = read_json(base)
    settings = dict(rigs=args.rigs, props=args.props, dynamic_bodies=args.dynamic_bodies,
                    lights=args.lights, autonomous=args.autonomous)
    recipe_hash = hashlib.sha256(json.dumps(dict(recipe=RECIPE, base_sha256=original_hash,
        manifest_sha256=schema_hash, settings=settings), sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    output.mkdir(parents=True)
    world = output / 'world.json'
    copied_store = Path(str(world) + '.assets')
    record = dict(passed=False, recipe=RECIPE, source_sha256=sha(__file__), calls=[], owner=None)
    client, completed, manifest = None, False, None
    try:
        shutil.copy2(base, world)
        shutil.copytree(store, copied_store)
        require(sha(world) == original_hash and inventory(copied_store) == original_store, 'Copied input bytes differ.')
        native_world = str(world)
        if args.windows_interop and os.name != 'nt':
            native_world = subprocess.check_output(['wslpath', '-w', str(world)], text=True, timeout=10).strip()
        client = WorldClient.open(args.binary.resolve(), native_world, cwd=ROOT)
        author = Author(client, record, recipe_hash)
        character = licensed_base(author, document)
        occupied = set(document['entities']) | set(document.get('retired_ids', []))
        require(not any(value.startswith('e094') for value in occupied), 'Performance entity namespace is occupied.')
        identifiers = populate(author, args, character, schema)
        final = read_json(world)
        for entity, value in document['entities'].items():
            actual = copy.deepcopy(final['entities'][entity])
            if entity == uid(100):
                actual['components'].pop('game:' + ROUTE, None)
                actual['components'].pop('game:' + PERFORMANCE, None)
            require(actual == value, 'Original Relay entity was modified: ' + entity)
        for key in ('navigation', 'templates', 'ui', 'asset_provenance', 'world_id', 'retired_ids',
                    'retired_component_schemas', 'retired_template_ids', 'retired_ui_ids'):
            require(final.get(key) == document.get(key), 'Original Relay catalog changed: ' + key)
        original_schemas = copy.deepcopy(final['component_schemas'])
        require(PERFORMANCE in original_schemas, 'Native performance schema is missing.')
        original_schemas.pop(PERFORMANCE)
        require(original_schemas == document['component_schemas'], 'Original component schemas changed.')
        meshes = author.query('SkinnedMesh')
        require({value['id'] for value in author.query('AnimationRig')} == set(identifiers['rigs']) and len(meshes) == args.rigs,
                'Native rig/weighted-mesh counts differ.')
        observed_rigs = set()
        for mesh in meshes:
            value = author.get(mesh['id'], 'SkinnedMesh')
            require(value['rig'] in identifiers['rigs'] and value['asset'] == character['asset'] and value['visible'],
                    'A weighted mesh differs from the declared workload.')
            require(value['rig'] not in observed_rigs, 'Duplicate weighted meshes target one rig.')
            observed_rigs.add(value['rig'])
        baseline_meshes = {entity for entity, value in document['entities'].items() if 'MeshRenderer' in value['components']}
        require({value['id'] for value in author.query('MeshRenderer')} ==
                baseline_meshes | set(identifiers['props']) | set(identifiers['dynamics']),
                'Native shared-box prop IDs differ.')
        for entity in identifiers['rigs'][1:]:
            rig = author.get(entity, 'AnimationRig')
            require(rig['clip'] == character['idle_clip'] and rig['playing'] and rig['loop'] and rig['speed'] == 1,
                    'Spectator rig is not advancing its imported Idle clip.')
        require({item['id'] for item in author.query('Light')} == set(identifiers['lights']), 'Native light IDs differ.')
        lighting = author.rpc('world.lighting', dict(revision=author.revision))
        require(len(lighting['lights']) == args.lights and lighting['shadow_views'] == 8 and
                lighting['shadow_resolution'] == 512, 'Native lighting allocation differs.')
        dynamics = [item['id'] for item in author.query('BoxCollider')
                    if author.get(item['id'], 'BoxCollider')['motion'] == 'dynamic']
        require(set(dynamics) == set(identifiers['dynamics']), 'Native dynamic-body IDs differ.')
        require(inventory(copied_store) == original_store, 'Authoring changed the cooked closure.')
        manifest = dict(format='poima.performance-yard', version=1, recipe=RECIPE, recipe_sha256=recipe_hash,
            settings=settings, world='world.json', base_world_sha256=original_hash, prepared_world_sha256=sha(world),
            component_manifest_sha256=schema_hash, assets=original_store,
            native_revision=author.revision, entities=len(final['entities']), ids=identifiers,
            character=character, skinned_mesh_ids=[item['id'] for item in meshes], lighting=lighting,
            geometry=dict(kind='shared native unit box', detail_instances=args.props, dynamic_instances=args.dynamic_bodies,
                          palette=PALETTE, metallic=.05, roughness=.75, detail_colliders=False),
            route=dict(player=uid(100), camera=uid(101), waypoints=WAYPOINTS,
                       path_component=ROUTE, performance_component=PERFORMANCE),
            preserved=dict(navigation=document['navigation'], source_entities=len(document['entities']),
                           audio_emitters=8, original_gameplay_state=True),
            limitations=['Imported Idle spectators are not additional AI or navigation agents.',
                'Shared simple boxes are not production-art complexity or a batching/instancing claim.',
                'Authored counts do not establish visible GPU submissions, frame rate or memory use.',
                'Dynamic boxes may settle; this is not sustained contact-heavy physics.',
                'Original navigation is unchanged; dynamic-body avoidance is not qualified.',
                'Retained source/license declarations identify supplied bytes, not independent permission verification.'])
        completed = True
    except BaseException:
        record['error'] = traceback.format_exc()
        raise
    finally:
        try:
            if client is not None:
                try:
                    client.close()
                finally:
                    record['owner'] = dict(exit_code=client.transport.returncode,
                        stderr=client.transport.stderr_tail, stderr_truncated=client.transport.stderr_truncated)
                require(record['owner']['exit_code'] == 0 and not record['owner']['stderr'] and
                        not record['owner']['stderr_truncated'], 'Native author cleanup or stderr failed.')
            require(sha(base) == original_hash and inventory(store) == original_store, 'Immutable baseline changed.')
            require(sha(args.manifest) == schema_hash, 'Immutable component manifest changed.')
        except BaseException:
            record['cleanup_error'] = traceback.format_exc()
            raise
        finally:
            record['passed'] = completed and 'cleanup_error' not in record
            write_json(output / 'commands.json', record)
            if record['passed']:
                write_json(output / 'manifest.json', manifest)


if __name__ == '__main__':
    main()
