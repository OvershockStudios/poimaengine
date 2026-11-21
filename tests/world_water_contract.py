#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise terrain-bound water, rollback, saves and optional water-and-particle Vulkan replay."""
import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import shutil
import sys
import tempfile
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/python'))
from poima_client import WorldClient
from scene_capture import pixels
from world_terrain_contract import (CAPTURE_LENS, CAPTURE_SIZE, TerrainContract, encode, error, file_hash,
                                    projection, read_inventory, rpc, success, tagged, tree_hashes, uid)


CELLS = [(x, z) for z in range(2) for x in range(4)]
TILES = [uid(100), uid(101)]
DOWNSTREAM = (2, 0)
GATE = 'sluice'
SILL = 5.75
CHECKPOINT = 4
OPENED = 14
FINAL = 16
SPARK = uid(102)
SPARK_ORIGIN = [12, 6.75, -3.5]
SPARK_VELOCITY = [6, 0, 0]
VERTEX_BYTES_PER_QUAD = 4 * (3 + 3 + 2 + 4) * 4  # MeshVertex float fields
INDEX_BYTES_PER_QUAD = 6 * 4  # uint32 indices


def bed(x, z):
    return 5 - .25*(x-10) + .125*(z+4)


def cell_center(cell):
    return 10.5 + cell[0], -3.5 + cell[1]


def near(a, b, tolerance=2e-11):
    assert math.isclose(a, b, rel_tol=0, abs_tol=tolerance), (a, b)


def action(pressed):
    return dict(name=GATE, held=True, pressed=pressed, released=False)


def replay_profiles():
    held = dict(ticks=CHECKPOINT-1, actions=[action(False)])
    return [('closed', CHECKPOINT-1, [held]),
            ('opened', OPENED, [held, dict(ticks=OPENED-CHECKPOINT+1, actions=[action(True)])])]


def spark_position(age):
    return [origin + velocity*age/60 for origin, velocity in zip(SPARK_ORIGIN, SPARK_VELOCITY, strict=True)]


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def reference():
    """Independent single-metre cell ledger for the test's authored ramp."""
    floors = [bed(*cell_center(cell)) for cell in CELLS]
    depths = [6.5-floor if x < 2 else 0 for (x, _), floor in zip(CELLS, floors)]
    states = [dict(depths=depths, openness=0, volume=sum(depths), ledger=None)]
    edges = [(low, low+1, x == 1, z) for low, (x, z) in enumerate(CELLS) if x < 3]
    edges += [(low, low+4, False, z) for low, (_, z) in enumerate(CELLS) if z == 0]
    for tick in range(1, FINAL+1):
        opened = CHECKPOINT <= tick <= OPENED
        exchanged = depths.copy()
        supply, drain = .6/60, min(exchanged[3], .24/60)
        exchanged[0] += supply
        exchanged[3] -= drain
        depths_next = exchanged.copy()
        for low, high, barrier, z in edges:
            if barrier and (z != 0 or not opened):
                continue
            threshold = max(floors[low], floors[high], SILL if barrier else -math.inf)
            heads = [max(0, floors[i]+exchanged[i]-threshold) for i in (low, high)]
            transfer = (heads[0]-heads[1])*(6/60)*(.5 if barrier else 1)
            depths_next[low] -= transfer
            depths_next[high] += transfer
        volume = sum(depths_next)
        states.append(dict(depths=depths_next, openness=.5 if opened else 0, volume=volume,
            ledger=dict(tick=tick, volume_before_m3=sum(depths), supplied_m3=supply,
                        drained_m3=drain, volume_after_m3=volume)))
        depths = depths_next
    return states


class WaterContract(TerrainContract):
    def cases(self, *cases, raw_world=False):
        requests = [case[1] for case in cases]
        if raw_world:
            requests = [dict(request, id=index) for index, request in enumerate(requests, 1)]
            output = self.cli('world', self.native(self.world), stdin=requests)
            replies = [json.loads(line) for line in output.splitlines()]
            assert [reply['id'] for reply in replies] == [request['id'] for request in requests], replies
        else:
            replies = self.world_calls(*requests)
        return {case[0]: error(reply, case[2]) if len(case) == 3 else success(reply)
                for case, reply in zip(cases, replies, strict=True)}

    def transaction(self, revision, request, *ops):
        return rpc('world.transact', request_id=uid(request), base_revision=revision, ops=list(ops))

    def start(self, session, revision):
        return rpc('runtime.start', session_id=session, revision=revision)

    def state(self, session, tick, **extra):
        return rpc('runtime.water.inspect', session_id=session, tick=tick,
                   **({'cells': [list(cell) for cell in CELLS]} | extra))

    def step(self, session, tick, ticks, request, pressed=None):
        params = dict(session_id=session, request_id=uid(request), expected_tick=tick, ticks=ticks)
        if pressed is not None:
            params['inputs'] = [dict(entity=uid(2), actions=[action(pressed)])]
        return rpc('runtime.step', **params)

    def write(self, session, tick, slot, request):
        return rpc('save.write', request_id=uid(request), configuration_generation=1, slot=slot,
                   expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0)

    def load(self, session, revision, request, active=None, *, active_tick=FINAL):
        return rpc('save.load', request_id=uid(request), configuration_generation=1, slot='checkpoint',
                   expected_generation=1, revision=revision, expected_session_id=active,
                   expected_tick=active_tick if active else None, expected_gameplay_revision=0 if active else None,
                   new_session_id=session)

    def arm_terrain(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Water Contract')))
        recipe = self.project/'Water bed.json'
        raw = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=5, samples_z=3,
            tile_cells=2, origin_m=[10, 4, -4], spacing_m=[1, 1],
            heights_m=[1-.25*x+.125*z for z in range(3) for x in range(5)]))
        recipe.write_text(raw, encoding='utf-8')
        plan = self.world_call('world.terrain.inspect', source=recipe.name)
        assert self.world_call('world.terrain.cook', source=recipe.name, expected_recipe_sha256=plan['recipe_sha256']) == plan
        assert [(tile['x'], tile['z'], tile['primitive']) for tile in plan['tiles']] == [(0, 0, 0), (1, 0, 0)]
        ops = []
        for entity, tile in zip(TILES, plan['tiles']):
            part = dict(asset=tile['asset'], primitive=0)
            ops.extend([dict(op='entity.create', id=entity, name=f"Water bed {tile['x']}"),
                        component(entity, 'StaticMesh', dict(part, visible=True)),
                        component(entity, 'MeshCollider', dict(part, friction=.5, restitution=0))])
        revision = self.world_call('world.inspect')['revision']
        result = success(self.world_calls(self.transaction(revision, 2000, *ops))[0])
        assert result['committed'] and result['revision'] == revision+1, result
        self.terrain = plan
        return recipe, raw, result['revision']

    def verify_plan(self, params, raw):
        revision = params['revision']
        before, assets = self.world.read_bytes(), read_inventory(self.assets)
        requests = self.cases(('schema', rpc('world.describe', view='method', name='world.water.cook')),
            ('plan', rpc('world.water.plan', **params)),
            ('inline', rpc('world.water.plan', **(params | {'terrain': {'recipe': raw},
                'faces': params['faces'][::-1], 'exchanges': params['exchanges'][::-1]}))))
        required = requests['schema']['methods']['world.water.cook']['required']
        assert {'revision', 'terrain', 'expected_terrain_recipe_sha256', 'tile_entities', 'grid', 'expected_plan_sha256'} <= set(required)
        plan = requests['plan']
        assert plan == requests['inline'] and plan['revision'] == revision and plan['cells'] == len(CELLS), plan
        assert plan['tiles'] == [dict(entity=entity, asset=tile['asset']) for entity, tile in zip(TILES, self.terrain['tiles'])]
        assert plan['terrain_recipe_sha256'] == self.terrain['recipe_sha256'] and plan['asset_bytes'] > 0, plan
        pin = plan['plan_sha256']
        assert len(pin) == 64 and all(ch in '0123456789abcdef' for ch in pin), plan
        self.cases(
            ('stale_revision', rpc('world.water.plan', **(params | {'revision': revision-1})), -32009),
            ('stale_recipe', rpc('world.water.plan', **(params | {'expected_terrain_recipe_sha256': '0'*64})), -32009),
            ('stale_plan', rpc('world.water.cook', **params, expected_plan_sha256='0'*64), -32009),
            ('missing_guard', rpc('world.water.cook', **params), -32602),
            ('wrong_tile_order', rpc('world.water.plan', **(params | {'tile_entities': TILES[::-1]})), -32009),
            ('duplicate_face', rpc('world.water.plan', **(params | {'faces': [params['faces'][0]]*2})), -32602),
            ('uncovered_corner', rpc('world.water.plan', **(params | {'grid': params['grid'] | {'origin_x_m': 9.75}})), -32602))
        assert self.world.read_bytes() == before and read_inventory(self.assets) == assets
        cook = rpc('world.water.cook', **params, expected_plan_sha256=pin)
        assert success(self.world_calls(cook)[0]) == plan and self.world.read_bytes() == before
        asset = self.assets / (pin+'.pwater')
        assert asset.stat().st_size == plan['asset_bytes'] and file_hash(asset) == pin
        decoded = json.loads(asset.read_bytes())
        assert decoded['format'] == 'poima.water.region' and decoded['version'] == 1
        assert decoded['terrain_recipe_sha256'] == plan['terrain_recipe_sha256'] and decoded['tiles'] == plan['tiles']
        assert decoded['floor_heights_m'] == [bed(*cell_center(cell)) for cell in CELLS]
        inventory = read_inventory(self.assets)
        assert success(self.world_calls(cook)[0]) == plan and read_inventory(self.assets) == inventory
        return plan

    def verify_closure(self, revision, plan):
        expected = [(tile['asset']+'.pmodel', tile['asset'], tile['bytes']) for tile in self.terrain['tiles']]
        expected += [(plan['plan_sha256']+'.pwater', plan['plan_sha256'], plan['asset_bytes'])]
        expected = [dict(filename=name, sha256=digest, bytes=size) for name, digest, size in sorted(expected)]
        world = self.world_call('world.dependencies')
        project = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert world['revision'] == project['revision'] == revision and world['assets'] == project['assets'] == expected
        for entry in expected:
            path = self.assets/entry['filename']
            assert file_hash(path) == entry['sha256'] and path.stat().st_size == entry['bytes']
        self.measurements['closure'] = expected

    def verify_state(self, state, tick):
        expected = self.oracle[tick]
        assert state['active'] is True and state['tick'] == tick and state['plan_sha256'] == self.plan['plan_sha256'], state
        assert [tuple(row['cell']) for row in state['cells']] == CELLS, state
        near(state['volume_m3'], expected['volume'])
        near(sum(row['volume_m3'] for row in state['cells']), state['volume_m3'])
        for row, depth in zip(state['cells'], expected['depths']):
            floor = bed(*cell_center(row['cell']))
            near(row['floor_height_m'], floor)
            near(row['depth_m'], depth)
            near(row['surface_height_m'], floor+depth)
            near(row['volume_m3'], depth)
        faces = {(tuple(row['cell']), row['axis']): row for row in state['faces']}
        gate = faces[((1, 0), 'x')]
        assert gate['action'] == GATE and gate['action_openness'] == .5 and gate['sill_height_m'] == SILL
        assert gate['openness'] == expected['openness'] and faces[((1, 1), 'x')]['openness'] == 0
        ledger = state['last_step']
        if tick == 0:
            assert ledger is None, state
        else:
            assert ledger['tick'] == tick
            for key, expected_value in expected['ledger'].items():
                near(ledger[key], expected_value)
            near(ledger['volume_before_m3'] + ledger['supplied_m3'] - ledger['drained_m3'], ledger['volume_after_m3'])

    def continuation_cases(self, name, session, receipts):
        cases = []
        for tick in range(CHECKPOINT, 14):
            cases.extend([(f'{name}-step-{tick+1}', self.step(session, tick, 1, receipts+tick, pressed=False)),
                          (f'{name}-{tick+1}', self.state(session, tick+1))])
        cases.extend([(f'{name}-close', self.step(session, 14, 2, receipts+14, pressed=True)),
                      (f'{name}-{FINAL}', self.state(session, FINAL))])
        return cases

    def warm_save(self, revision):
        self.saves = self.root/'External Water Saves'
        self.saves.mkdir()
        warm, restored = uid(900), uid(901)
        floor_rays = []
        for col in (1, 2):
            x, z = cell_center((col, 0))
            floor_rays.append((f'floor-{col}', rpc('runtime.raycast', session_id=warm, tick=0,
                origin=[x, 20, z], direction=[0, -1, 0], distance=25)))
        a = self.cases(('configure', rpc('save.configure', request_id=uid(2100), expected_generation=0, root=self.native(self.saves))),
            ('start', self.start(warm, revision)), ('initial', self.state(warm, 0)),
            *floor_rays, ('summary', rpc('runtime.water.inspect', session_id=warm, tick=0)),
            ('bad_tick', self.state(warm, 1), -32009), ('bad_session', self.state(uid(999), 0), -32030),
            ('bad_cell', self.state(warm, 0, cells=[[4, 0]]), -32602),
            ('subset', self.state(warm, 0, cells=[[0, 0], [3, 1]])),
            ('held_without_edge', self.step(warm, 0, 3, 2102, pressed=False)), ('closed', self.state(warm, 3)),
            ('open', self.step(warm, 3, 1, 2103, pressed=True)), ('pulse', self.state(warm, CHECKPOINT)),
            ('checkpoint', self.write(warm, CHECKPOINT, 'checkpoint', 2104)),
            *self.continuation_cases('warm', warm, 2200), ('warm_save', self.write(warm, FINAL, 'warm', 2105)),
            ('warm_reload', self.load(restored, revision, 2106, active=warm)), ('warm_restored', self.state(restored, CHECKPOINT)),
            *self.continuation_cases('warm-replay', restored, 2300),
            ('warm_replay_save', self.write(restored, FINAL, 'warm-replay', 2107)), ('close', rpc('session.close')))
        assert a['close']['closed'] is True and a['warm_reload']['tick'] == CHECKPOINT
        assert a['summary'] == dict(a['initial'], cells=[])
        assert a['subset']['cells'] == [a['initial']['cells'][0], a['initial']['cells'][-1]]
        for index, col in enumerate((1, 2)):
            hit = a[f'floor-{col}']['hit']
            assert hit['entity'] == TILES[index] and type(hit['triangle']) is int, hit
            near(hit['position'][1], a['initial']['cells'][col]['floor_height_m'], tolerance=2e-5)
        for key, tick in (('initial', 0), ('closed', 3), ('pulse', CHECKPOINT)):
            self.verify_state(a[key], tick)
        assert all(row['depth_m'] == 0 for row in a['closed']['cells'] if row['cell'][0] >= 2)
        upstream = a['closed']['cells'][1]['surface_height_m']
        near(a['pulse']['cells'][2]['depth_m'], (upstream-SILL)*.05)
        assert all(row['depth_m'] == 0 for index, row in enumerate(a['pulse']['cells']) if row['cell'][0] >= 2 and index != 2)
        assert dict(a['pulse'], session_id=restored) == a['warm_restored']
        for tick in [*range(CHECKPOINT+1, 15), FINAL]:
            self.verify_state(a[f'warm-{tick}'], tick)
            assert dict(a[f'warm-{tick}'], session_id=restored) == a[f'warm-replay-{tick}']
        assert a['warm-14']['last_step']['drained_m3'] > 0 and a['warm-14']['cells'][3]['depth_m'] > 0
        assert a['warm_save']['sha256'] == a['warm_replay_save']['sha256']
        self.measurements['warm'] = {key: a[key] for key in ('initial', 'closed', 'pulse', 'warm-14', 'warm-16', 'warm_save')}
        return a

    def fresh_save(self, revision, warm):
        fresh = uid(902)
        b = self.cases(('status', rpc('runtime.status')),
            ('configure', rpc('save.configure', request_id=uid(2400), expected_generation=0, root=self.native(self.saves))),
            ('load', self.load(fresh, revision, 2401)), ('restored', self.state(fresh, CHECKPOINT)),
            *self.continuation_cases('fresh', fresh, 2500), ('save', self.write(fresh, FINAL, 'fresh', 2402)), ('close', rpc('session.close')))
        assert not b['status']['active'] and b['load']['tick'] == CHECKPOINT and not b['load']['source_stale'] and b['close']['closed']
        assert dict(warm['pulse'], session_id=fresh) == b['restored']
        for tick in [*range(CHECKPOINT+1, 15), FINAL]:
            assert dict(warm[f'warm-{tick}'], session_id=fresh) == b[f'fresh-{tick}']
        payloads = [list((self.saves/f'slot-{name}').glob('p-*.bin')) for name in ('checkpoint', 'warm', 'warm-replay', 'fresh')]
        assert all(len(slot) == 1 for slot in payloads)
        checkpoint, *saved = (slot[0].read_bytes() for slot in payloads)
        assert checkpoint != saved[0] and saved[0] == saved[1] == saved[2]
        assert file_hash(payloads[-1][0]) == b['save']['sha256'] == warm['warm_save']['sha256']
        self.measurements['exact_saved_continuation'] = dict(tick=FINAL, sha256=b['save']['sha256'], bytes=len(saved[0]))

    def negative_controls(self, params, revision):
        root = self.root/'Negative control'
        root.mkdir()
        test = WaterContract(self.args, root)
        test.calls = self.calls
        shutil.copytree(self.project, test.project)
        identity = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
        stale = component(TILES[0], 'Transform', identity | {'position': [0, .125, 0]})
        edited = success(test.world_calls(test.transaction(revision, 2600, stale))[0])
        assert edited['committed'] and edited['revision'] == revision+1
        revision += 1
        before = test.world.read_bytes()
        # Project preflight rejects the stale binding before opening an RPC owner.
        # Address the raw entry world for both the rejection and the repair.
        test.cases(('stale_tile', rpc('world.water.plan', **(params | {'revision': revision})), -32009),
            ('stale_freeze', rpc('world.dependencies'), -32009),
            ('stale_runtime', test.start(uid(910), revision), -32009), raw_world=True)
        assert test.world.read_bytes() == before
        restored = test.cases(('restore', test.transaction(revision, 2601, component(TILES[0], 'Transform', identity))),
                              raw_world=True)['restore']
        assert restored['committed'] and restored['revision'] == revision+1
        revision += 1
        assert test.world_call('world.dependencies')['assets'] == self.measurements['closure']
        self.measurements['stale_tile_freeze_rejected'] = True

        # The first tick opens the gate and is valid. The second rejects its
        # exchange before any transfer. A two-tick request must roll both back.
        overflow = params | dict(revision=revision, grid=params['grid'] | {'cells_x': 2, 'cells_z': 1},
            initial_depths_m=[975000, 0], faces=[dict(cell=[0, 0], axis='x', openness=0, sill_height_m=1_000_000,
            action=GATE, action_openness=.5)], exchanges=[dict(cell=[0, 0], source_m3_per_second=1_000_000, drain_m3_per_second=0)])
        overflow_plan = test.world_call('world.water.plan', **overflow)
        pin = overflow_plan['plan_sha256']
        assert test.world_call('world.water.cook', **overflow, expected_plan_sha256=pin) == overflow_plan
        binding = success(test.world_calls(test.transaction(revision, 2602, dict(op='water.set', asset=pin)))[0])
        assert binding['committed'] and binding['revision'] == revision+1
        session = uid(911)
        cells = dict(cells=[[0, 0], [1, 0]])
        output = test.cases(('start', test.start(session, revision+1)), ('before', test.state(session, 0, **cells)),
            ('overflow', test.step(session, 0, 2, 2603, pressed=True), -32040), ('after_reject', test.state(session, 0, **cells)),
            ('first_tick', test.step(session, 0, 1, 2604, pressed=True)), ('after_tick', test.state(session, 1, **cells)),
            ('overflow_after_tick', test.step(session, 1, 1, 2605), -32040), ('after_second_reject', test.state(session, 1, **cells)),
            ('close', rpc('session.close')))
        assert 'exceeds the supported depth' in output['overflow']['message'], output
        assert output['before'] == output['after_reject'] and output['after_tick'] == output['after_second_reject']
        assert output['before']['faces'][0]['openness'] == 0 and output['after_tick']['faces'][0]['openness'] == .5
        assert output['before']['cells'][0]['depth_m'] == 975000
        near(output['after_tick']['cells'][0]['depth_m'], 975000 + 1_000_000/60, tolerance=1e-8)
        assert output['close']['closed']
        self.measurements['late_rollback'] = output

    def prepare_standalone(self, revision):
        ops = [dict(op='entity.create', id=SPARK, name='Sluice spark'),
               component(SPARK, 'Transform', dict(position=SPARK_ORIGIN, rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
               component(SPARK, 'ParticleEmitter', dict(spawn_space='world', simulation_space='world',
                   lifetime_ticks=24, size_m=.32, linear_rgb=[1, .015, .015], burst=dict(action=GATE, count=1),
                   velocity_m_s=dict(minimum=SPARK_VELOCITY, maximum=SPARK_VELOCITY)))]
        if self.args.capture_output:
            pitch = math.radians(-40)/2
            ops.extend([component(uid(3), 'Transform', dict(position=[12, 9, 0], scale=[1, 1, 1],
                            rotation=[math.sin(pitch), 0, 0, math.cos(pitch)])), component(uid(3), 'Camera', CAPTURE_LENS),
                        component(uid(4), 'LightingEnvironment', dict(ambient=[1, 1, 1], exposure=1)),
                        dict(op='component.remove', id=uid(5), type='Light'),
                        dict(op='component.remove', id=uid(1), type='MeshRenderer')])
            for tile in TILES:
                ops.append(component(tile, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=[.12]*3,
                    metallic=0, roughness=1, double_sided=False)))
        result = success(self.world_calls(self.transaction(revision, 2700, *ops))[0])
        assert result['committed'] and result['revision'] == revision+1
        return revision+1

    def verify_game(self, bundle, detached, revision):
        game = bundle/'game.json'
        executable = bundle/json.loads(game.read_bytes())['engine']['executable']
        external = self.root/'External Water Verification'
        external.mkdir()
        suite, report = external/'water.verify.json', external/'water.report.json'
        cell = list(DOWNSTREAM)
        groups = []
        profiles = replay_profiles()
        for label, tick, replay in profiles:
            depth = self.oracle[tick]['depths'][CELLS.index(DOWNSTREAM)]
            checks = [('Downstream cell address', '/cells/0/cell', dict(equals=cell)),
                      ('Downstream depth in metres', '/cells/0/depth_m',
                       dict(equals=0) if depth == 0 else dict(between=[depth-2e-11, depth+2e-11]))]
            sequence = [dict(step=dict(ticks=step['ticks'], inputs=[dict(entity=uid(2), actions=step['actions'])]))
                        for step in replay]
            sequence.extend(dict(expect=dict(label=check_label, method='runtime.water.inspect', path=path,
                                              params=dict(cells=[cell]), **comparison))
                            for check_label, path, comparison in checks)
            live = int(tick >= CHECKPOINT)
            particle_checks = [('Authored spark source', '/sources/0/entity', dict(equals=SPARK)),
                               ('Sparks spawned', '/counts/spawned', dict(equals=live)),
                               ('Live sparks', '/counts/live', dict(equals=live))]
            if live:
                age = tick-CHECKPOINT
                particle_checks.append(('Ticks since the sluice press', '/particles/0/age_ticks', dict(equals=age)))
                for axis, position in enumerate(spark_position(age)):
                    particle_checks.append((f'Spark position in metres ({"XYZ"[axis]})', f'/particles/0/position_m/{axis}',
                                            dict(between=[position-2e-11, position+2e-11])))
            sequence.extend(dict(expect=dict(label=name, method='runtime.particles.inspect', path=path, **comparison))
                            for name, path, comparison in particle_checks)
            groups.append(dict(name=label, sequence=sequence))
        specification = dict(format='poima.game-verification', version=1, groups=groups)
        suite.write_text(encode(specification), encoding='utf-8')
        evidence = self.measurements['standalone_verification'] = dict(suite=specification)
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        assert not self.project.exists() and not (detached/'Water bed.json').exists()
        result = success(json.loads(self.cli('game', 'verify', self.native(game), '--suite', self.native(suite),
            '--report', self.native(report), '--completed-exit', 'true', '--expected-bundle-sha256', frozen['game.json'],
            '--expected-suite-sha256', file_hash(suite), executable=executable, cwd=external)))
        evidence['report'] = result
        assert result == json.loads(report.read_bytes()) and result['passed'] and result['revision'] == revision, result
        assert result['bundle_sha256'] == frozen['game.json'] and result['suite_sha256'] == file_hash(suite), result
        assert result['groups_passed'] == result['groups_total'] == len(profiles), result
        for group, (label, tick, _) in zip(result['groups'], profiles, strict=True):
            assert (group['name'], group['ticks'], group['passed']) == (label, tick, True), group
            checks = group['checks']
            assert all(check['tick'] == tick and check['passed'] and check['observed'] and not check['actual_omitted']
                       for check in checks), group
            address, measured, source, spawned, live, *particle = checks
            assert address['actual'] == cell, group
            near(measured['actual'], self.oracle[tick]['depths'][CELLS.index(DOWNSTREAM)])
            expected_live = int(tick >= CHECKPOINT)
            assert source['actual'] == SPARK and spawned['actual'] == live['actual'] == expected_live, group
            if expected_live:
                age, *position = [check['actual'] for check in particle]
                assert age == tick-CHECKPOINT, group
                for actual, expected in zip(position, spark_position(age), strict=True):
                    near(actual, expected)
            else:
                assert not particle, group
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored

    def standalone_save(self, bundle, detached, revision):
        game = bundle/'game.json'
        executable = bundle/json.loads(game.read_bytes())['engine']['executable']
        external = self.root/'External Standalone Water Saves'
        external.mkdir()
        saves = external/'slots'
        saves.mkdir()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        assert not self.project.exists() and not (detached/'Water bed.json').exists()
        evidence = self.measurements['standalone_saved_continuation'] = dict(owners=[], slots={})

        @contextmanager
        def owner(label):
            trace = external/label
            manifest, storage = self.native(game), self.native(saves)
            record = dict(argv=[str(executable.resolve()), 'game', 'stdio', manifest, '--save-root', storage],
                          cwd=str(external), trace=str(trace))
            self.calls.append(record)
            evidence['owners'].append(record)
            client = None
            try:
                with WorldClient.open_game(executable.resolve(), manifest, save_root=storage, cwd=external,
                                           trace_directory=trace, trace_limit_bytes=1024*1024) as client:
                    record['process_id'] = client.transport.process_id
                    yield client
                    assert client.call('session.close')['closed'] is True
            finally:
                if client is not None:
                    record.update(exit=client.transport.returncode, closed=client.closed,
                                  cleanup_error=str(client.cleanup_error) if client.cleanup_error else None)
                record['wire'] = {file.name: file.read_bytes().decode('utf-8', 'backslashreplace')
                                  for file in sorted(trace.glob('*'))}
            assert record['exit'] == 0 and record['closed'] and record['cleanup_error'] is None, record

        def run_cases(client, *cases):
            return {name: client.call(request['method'], request['params']) for name, request in cases}

        def initial(client):
            status = client.call('runtime.status')
            assert status['active'] is True and status['tick'] == 0 and status['authored_revision'] == revision, status
            session = status['session_id']
            state = run_cases(client, ('initial', self.state(session, 0)))['initial']
            assert state['session_id'] == session, state
            self.verify_state(state, 0)
            return session

        def payload(slot, tick, receipt):
            file, = (saves/f'slot-{slot}').glob('p-*.bin')
            raw = file.read_bytes()
            evidence['slots'][slot] = dict(file=str(file), receipt=receipt, archive_utf8=raw.decode('utf-8'))
            assert len(raw) == receipt['bytes'] and file_hash(file) == receipt['sha256'], receipt
            decoded = json.loads(raw)['snapshot']['payload']
            assert decoded['tick'] == decoded['water']['tick'] == tick
            assert decoded['water']['plan_sha256'] == self.plan['plan_sha256']
            effects = decoded['particles']
            assert [source['entity'] for source in effects['sources']] == [SPARK]
            state = effects['state']
            assert state['status']['tick'] == tick
            ages = [0] if tick == CHECKPOINT else [FINAL-CHECKPOINT, FINAL-(OPENED+1)]
            assert state['status']['particles'] == dict(live=len(ages), spawned=len(ages), dropped=0, expired=0, removed=0)
            for particle, age in zip(state['particles'], ages, strict=True):
                assert particle['age_ticks'] == age
                for actual, position in zip(particle['position_m'], spark_position(age), strict=True):
                    near(actual, position)
            return raw

        with owner('original') as game_client:
            original = initial(game_client)
            first = run_cases(game_client,
                ('held', self.step(original, 0, 3, 2800, pressed=False)),
                ('open', self.step(original, 3, 1, 2801, pressed=True)),
                ('at-checkpoint', self.state(original, CHECKPOINT)),
                ('checkpoint', self.write(original, CHECKPOINT, 'checkpoint', 2802)),
                *self.continuation_cases('original', original, 2810),
                ('final', self.write(original, FINAL, 'original', 2803)))
            self.verify_state(first['at-checkpoint'], CHECKPOINT)
            checkpoint = payload('checkpoint', CHECKPOINT, first['checkpoint'])
            uninterrupted = payload('original', FINAL, first['final'])
            assert checkpoint != uninterrupted
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored

        restored = uid(2900)
        with owner('restored') as game_client:
            reopened = initial(game_client)
            assert len({original, reopened, restored}) == 3
            second = run_cases(game_client,
                ('load', self.load(restored, revision, 2901, active=reopened, active_tick=0)),
                ('at-checkpoint', self.state(restored, CHECKPOINT)),
                *self.continuation_cases('restored', restored, 2910),
                ('final', self.write(restored, FINAL, 'restored', 2902)))
            load = second['load']
            assert load['session_id'] == restored and load['tick'] == CHECKPOINT and load['source_stale'] is False, load
            assert dict(first['at-checkpoint'], session_id=restored) == second['at-checkpoint']
            for tick in [*range(CHECKPOINT+1, 15), FINAL]:
                self.verify_state(first[f'original-{tick}'], tick)
                assert dict(first[f'original-{tick}'], session_id=restored) == second[f'restored-{tick}']
            replayed = payload('restored', FINAL, second['final'])
            assert uninterrupted == replayed
        first_owner, second_owner = evidence['owners']
        assert first_owner['process_id'] != second_owner['process_id']
        assert payload('checkpoint', CHECKPOINT, first['checkpoint']) == checkpoint
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored
        evidence.update(runtime_sessions=dict(original=original, reopened=reopened, restored=restored),
                        tick=FINAL, bytes=len(replayed), sha256=second['final']['sha256'])

    def capture_game(self, bundle, detached, revision):
        game = bundle/'game.json'
        executable = bundle/json.loads(game.read_bytes())['engine']['executable']
        external = self.root/'External Water Replay'
        external.mkdir()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        captures = self.measurements['vulkan'] = dict(bundle=str(bundle), inventory=frozen)
        assert not self.project.exists()
        for label, tick, sequence in replay_profiles():
            replay, bmp, receipt = (external/(label+extension) for extension in ('.json', '.bmp', '.report.json'))
            replay.write_text(encode(sequence), encoding='utf-8')
            width, height = CAPTURE_SIZE
            result = success(json.loads(self.cli('game', 'run', self.native(game), '--gpu', self.args.gpu,
                '--replay', self.native(replay), '--capture', self.native(bmp), '--report', self.native(receipt),
                '--width', width, '--height', height, '--samples', 1, '--frames-in-flight', 2,
                executable=executable, cwd=external)))
            play = result['play']
            item = captures[label] = dict(play=play, image=str(bmp), image_sha256=file_hash(bmp))
            assert json.loads(receipt.read_bytes()) == result and result['runtime']['tick'] == play['tick'] == tick
            assert play['success'] and play['stop_reason'] == 'replay_complete' and play['revision'] == revision, play
            assert play['hardware'] and play['frames_presented'] > 0 and play['capture_written'] and play['nvrhi_errors'] == 0
            assert (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, 1), play
            diagnostics = play['render_diagnostics']
            draws = diagnostics['last_draws']
            wet = sum(depth > 0 for depth in self.oracle[tick]['depths'])
            live = int(tick >= CHECKPOINT)
            assert diagnostics['completed_submissions'] > 0 and draws['objects'] == draws['camera_draws'] == 3+live, draws
            assert draws['camera_triangles'] == 16 + 2*(wet+live), draws
            assert draws['streamed_draws'] == 1+live, draws
            assert draws['streamed_vertex_bytes'] == (wet+live)*VERTEX_BYTES_PER_QUAD, draws
            assert draws['streamed_index_bytes'] == (wet+live)*INDEX_BYTES_PER_QUAD, draws
            execution = diagnostics['frame_execution']
            assert execution['limit'] == 2, execution
            assert play['materials']['presented']['tick'] == tick, play['materials']
            presented = diagnostics['scene_receipts']['presented']
            assert presented['camera_id'] == uid(3) and presented['camera_lens'] == CAPTURE_LENS
            camera = presented.get('camera_pose', {}).get('camera_world') or play['camera_world']
            image = pixels(bmp)
            assert len(image) == height and all(len(row) == width for row in image)
            samples = item['surface_witnesses'] = {}
            for channel, cell in [('upstream', (0, 0)), ('downstream', DOWNSTREAM)]:
                x, z = cell_center(cell)
                depth = self.oracle[tick]['depths'][CELLS.index(cell)]
                point = [x, bed(x, z)+depth, z]
                u, v = map(math.floor, projection(camera, CAPTURE_LENS, point))
                assert 1 <= u < width-1 and 1 <= v < height-1, (point, u, v)
                neighborhood = [image[y][x_] for y in range(v-1,v+2) for x_ in range(u-1,u+2)]
                blue = sum(b > 70 and b > r+20 and b > g+4 for r, g, b in neighborhood)
                samples[channel] = dict(world=point, pixel=[u, v], depth_m=depth, colors=neighborhood, water_pixels=blue)
                assert blue == (9 if depth > 0 else 0), samples[channel]
            witnesses = item['particle_witnesses'] = {}
            for name, age, expected in [('birth', 0, False), ('advanced', OPENED-CHECKPOINT, live == 1)]:
                point = spark_position(age)
                u, v = map(math.floor, projection(camera, CAPTURE_LENS, point))
                assert 1 <= u < width-1 and 1 <= v < height-1, (point, u, v)
                colors = [image[y][x] for y in range(v-1, v+2) for x in range(u-1, u+2)]
                red = sum(tagged(color, 0) for color in colors)
                witnesses[name] = dict(world=point, pixel=[u, v], expected=expected, colors=colors, particle_pixels=red)
                assert red == (9 if expected else 0), witnesses[name]
            assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored
        closed, opened = (captures[label]['play'] for label in ('closed', 'opened'))
        assert closed['gpu'] == opened['gpu'], (closed['gpu'], opened['gpu'])
        idle_waits = [play['render_diagnostics']['frame_execution']['device_idle_waits'] for play in (closed, opened)]
        assert idle_waits[0] == idle_waits[1], idle_waits

    def export(self, revision):
        output = self.root/'Initial water export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        assets = [file for file in output.rglob('*') if file.suffix in ('.pwater', '.pmodel')]
        assert sorted(file.name for file in assets) == [entry['filename'] for entry in self.measurements['closure']]
        assert all(file_hash(file) == file.stem for file in assets)
        assert not list(output.rglob('Water bed.json'))
        bundle, detached = self.root/'Moved Water Standalone', self.root/'Retained Water Source'
        output.rename(bundle)
        self.project.rename(detached)
        assert not output.exists() and not self.project.exists()
        info = success(json.loads(self.cli('game', 'inspect', self.native(bundle/'game.json'))))
        assert info['revision'] == revision, info
        if self.args.runtime:
            self.verify_game(bundle, detached, revision)
            self.standalone_save(bundle, detached, revision)
        if self.args.capture_output:
            self.capture_game(bundle, detached, revision)
        self.measurements['exported'] = True

    def run(self):
        self.oracle = reference()
        recipe, raw, revision = self.arm_terrain()
        params = dict(revision=revision, terrain={'source': recipe.name},
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'], tile_entities=TILES,
            grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4, relaxation_per_second=6),
            initial_depths_m=self.oracle[0]['depths'],
            faces=[dict(cell=[1, 0], axis='x', openness=0, sill_height_m=SILL, action=GATE, action_openness=.5),
                   dict(cell=[1, 1], axis='x', openness=0, sill_height_m=SILL)],
            exchanges=[dict(cell=[0, 0], source_m3_per_second=.6, drain_m3_per_second=0),
                       dict(cell=[3, 0], source_m3_per_second=0, drain_m3_per_second=.24)])
        self.plan = self.verify_plan(params, raw)
        self.measurements['plan'] = self.plan
        pin = self.plan['plan_sha256']
        if self.args.runtime:
            session = uid(899)
            inactive = self.cases(('start', self.start(session, revision)), ('water', self.state(session, 0)))['water']
            assert inactive['active'] is False and inactive['tick'] == 0, inactive
        bind = dict(op='water.set', asset=pin)
        world = self.world.read_bytes()
        self.cases(('stale_revision', self.transaction(revision-1, 2001, bind), -32009),
                   ('invalid_asset', self.transaction(revision, 2002, dict(op='water.set', asset='z'*64)), -32602))
        assert self.world.read_bytes() == world
        request = self.transaction(revision, 2003, bind)
        published, retry = map(success, self.world_calls(request, request))
        revision += 1
        assert published['committed'] and published['revision'] == revision and retry == dict(published, replayed=True)
        self.verify_closure(revision, self.plan)
        if self.args.runtime:
            warm = self.warm_save(revision)
            self.negative_controls(params, revision)
        old_project, self.project = self.project, self.root/'Relocated Water'
        retained_recipe = self.root/'Retained original bed.json'
        recipe.rename(retained_recipe)
        before = self.world.read_bytes()
        old_project.rename(self.project)
        self.manifest, self.world, self.assets = (self.project/path.name for path in (self.manifest, self.world, self.assets))
        assert self.world.read_bytes() == before and not old_project.exists() and not (self.project/recipe.name).exists()
        self.verify_closure(revision, self.plan)
        if self.args.runtime:
            self.fresh_save(revision, warm)
            assert self.world.read_bytes() == before
        self.measurements['source_recipe_unavailable'] = True
        if self.args.runtime_root:
            if self.args.runtime:
                revision = self.prepare_standalone(revision)
            self.export(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed standalone runtime.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture-output', type=Path, help='Retain closed and opened Windows Vulkan water and particle captures.')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    if args.capture_output and not (args.runtime_root and args.runtime):
        parser.error('--capture-output requires --runtime-root and native simulation.')

    def execute(directory):
        test = WaterContract(args, Path(directory))
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
            paths = {args.evidence, Path(directory)/'evidence.json' if args.capture_output else None} - {None}
            for path in paths:
                path.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
                print(f'Evidence file {path}')

    if args.capture_output:
        args.capture_output.mkdir(parents=True, exist_ok=True)
        execute(tempfile.mkdtemp(prefix=f'water-gpu-{args.gpu}-', dir=args.capture_output.resolve()))
    else:
        scratch = Path(__file__).resolve().parents[1]/'build'/'world-water-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            execute(directory)
    print('Water world contract passed guarded planning and relocated asset closure.' +
          (' The native valve, mass ledger, rollback and save continuations matched.' if args.runtime else ''))


if __name__ == '__main__':
    main()
