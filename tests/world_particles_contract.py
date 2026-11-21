#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise authored and spawned particle sources through the native owner and standalone game."""
import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import sys
import tempfile
import traceback

from scene_capture import pixels
from world_terrain_contract import (CAPTURE_LENS, CAPTURE_SIZE, TerrainContract, encode, error, file_hash,
                                    projection, rpc, success, tagged, tree_hashes, uid)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/python'))
from poima_client import RpcError, WorldClient


EMITTERS = (uid(100), uid(101))
TEMPLATES = (uid(110), uid(111))
PLAYER = uid(2)
ACTION, TEMPLATE_ACTION = 'spark', 'template-spark'
CHECKPOINT, FINAL, DETACHED, EXPIRED = 6, 19, 20, 24
IDENTITY = [0, 0, 0, 1]
TURN = [0, 0, math.sqrt(.5), math.sqrt(.5)]
HALF_TURN = [0, 0, 1, 0]
ORIGINS = ([-1.5, 1.4, -4], [1.5, 1.4, -4])
BIRTH = ([-1.8, 1.8, -4], [1.8, 1.8, -4])
MOVED = ([-1, 2.3, -4], [2.4, 2.3, -4])
COLORS = ([1, .015, .015], [.015, 1, .015])
COUNT_FIELDS = ('live', 'spawned', 'dropped', 'expired', 'removed')


def transform(position, rotation=IDENTITY):
    return dict(position=position, rotation=rotation, scale=[1, 1, 1])


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def fixed(vector):
    return dict(minimum=vector, maximum=vector)


def emitter(index):
    return dict(spawn_space='local', simulation_space=('local', 'world')[index],
                seed=f'{index+13:016x}', lifetime_ticks=12, size_m=.32, linear_rgb=COLORS[index],
                live_limit=(2, 4)[index], offset_m=fixed([.45, 0, 0]), velocity_m_s=fixed([1.8, 0, 0]),
                acceleration_m_s2=[.6, 0, 0], burst=dict(action=ACTION, count=3))


def action(name=ACTION, pressed=False):
    return dict(name=name, held=True, pressed=pressed, released=False)


def inputs(name=ACTION, pressed=False):
    return [dict(entity=PLAYER, actions=[action(name, pressed)])]


def motions(positions, rotation):
    return [dict(entity=entity, position=position, rotation=rotation, duration_ticks=5)
            for entity, position in zip(EMITTERS, positions, strict=True)]


def near_vector(actual, expected, tolerance=4e-5):
    assert len(actual) == len(expected) and all(math.isclose(a, b, rel_tol=0, abs_tol=tolerance)
                                               for a, b in zip(actual, expected)), (actual, expected)


def counters(live, spawned, dropped=0, expired=0, removed=0):
    return dict(zip(COUNT_FIELDS, (live, spawned, dropped, expired, removed), strict=True))


def displacement(age):
    # Semi-implicit Euler at 60 Hz, with a 0.6 m/s² acceleration.
    return .6*age*(age+1)/(2*60**2)


def integrated_position(origin, age):
    x, y, z = origin
    return [x+.45+1.8*age/60+displacement(age), y, z]


class ParticleContract(TerrainContract):
    def author_project(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Particle Contract')))
        component_schema = self.world_call('world.describe', view='component', name='ParticleEmitter')
        assert set(component_schema['components']) == {'ParticleEmitter'}, component_schema
        catalog = self.world_call('world.describe', view='catalog')
        if self.args.runtime:
            assert 'runtime.particles.inspect' in catalog['methods'], catalog
        revision = self.world_call('world.inspect')['revision']
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[.2]*3, exposure=1)),
               component(uid(3), 'Camera', CAPTURE_LENS)]
        for index, entity in enumerate(EMITTERS):
            ops.extend([dict(op='entity.create', id=entity, name=f'Spark source {index+1}'),
                        component(entity, 'Transform', transform(ORIGINS[index])),
                        component(entity, 'BoxCollider', dict(motion='kinematic', half_extents=[.08]*3,
                                                             mass=1, friction=.5, restitution=0)),
                        component(entity, 'ParticleEmitter', emitter(index))])
        for index, template in enumerate(TEMPLATES):
            effect = emitter(index) | dict(spawn_space='world', seed=f'{42+index:016x}', emitting=False,
                particles_per_second=30, lifetime_ticks=4, burst=dict(action=TEMPLATE_ACTION, count=2),
                offset_m=dict(minimum=[.25, -.2, 0], maximum=[.6, .2, 0]),
                velocity_m_s=dict(minimum=[.4, -.3, 0], maximum=[.8, .3, 0]))
            ops.append(dict(op='template.set', id=template, name=f'Temporary source {index+1}',
                            components=dict(Transform=transform([5+index*2, 3, -4], TURN), ParticleEmitter=effect)))
        transaction = rpc('world.transact', request_id=uid(3000), base_revision=revision, ops=ops)
        authored = success(self.world_calls(transaction)[0])
        assert authored['committed'] and authored['revision'] == revision+1, authored
        revision += 1
        before = self.world.read_bytes()
        invalid = [dict(spawn_space='camera'), dict(seed='INVALID'), dict(lifetime_ticks=0),
                   dict(offset_m=dict(minimum=[1, 0, 0], maximum=[0, 0, 0]))]
        rejects = [rpc('world.transact', request_id=uid(3001+i), base_revision=revision,
            ops=[component(EMITTERS[0], 'ParticleEmitter', emitter(0) | bad)]) for i, bad in enumerate(invalid)]
        rejects.append(rpc('world.transact', request_id=uid(3005), base_revision=revision, ops=[
            dict(op='entity.create', id=uid(120), name='Scaled parent'),
            component(uid(120), 'Transform', transform([0, 0, 0]) | dict(scale=[2, 1, 1])),
            dict(op='entity.reparent', id=EMITTERS[0], parent=uid(120), mode='keep_local')]))
        added = []
        for index in range(15):
            entity = uid(120+index)
            added.extend([dict(op='entity.create', id=entity, name=f'Extra emitter {index}'),
                          component(entity, 'ParticleEmitter', emitter(0))])
        rejects.append(rpc('world.transact', request_id=uid(3006), base_revision=revision, ops=added))
        rejects.append(rpc('world.transact', request_id=uid(3007), base_revision=revision, ops=[
            dict(op='template.set', id=uid(112), name='Scaled source',
                 components=dict(Transform=transform([0, 0, 0]) | dict(scale=[1, 2, 1]),
                                 ParticleEmitter=emitter(0)))]))
        for reply in self.world_calls(*rejects):
            error(reply, -32602)
        assert self.world.read_bytes() == before
        dependencies = self.world_call('world.dependencies')
        project = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert dependencies['revision'] == project['revision'] == revision
        assert dependencies['needs_particles'] is True and project['needs_particles'] is True
        assert dependencies['assets'] == project['assets'] == []
        self.measurements['authored_revision'] = revision
        return revision

    @contextmanager
    def owner(self, label, *, game=None, saves=None):
        trace = self.root/(label+' wire')
        options = dict(cwd=self.root, trace_directory=trace, trace_limit_bytes=1024*1024)
        if game:
            manifest = self.native(game)
            executable = (game.parent/json.loads(game.read_bytes())['engine']['executable']).resolve()
            storage = self.native(saves)
            argv = [str(executable), 'game', 'stdio', manifest, '--save-root', storage]
        else:
            manifest = self.native(self.manifest)
            argv = [str(self.args.binary.resolve()), 'world', '--project', manifest]
        record = dict(label=label, argv=argv, cwd=str(self.root), trace=str(trace))
        self.calls.append(record)
        client = None
        try:
            connection = (WorldClient.open_game(executable, manifest, save_root=storage, **options)
                          if game else WorldClient.open(self.args.binary.resolve(), project=manifest, **options))
            with connection as client:
                record['pid'] = client.transport.process_id
                yield client, record
                assert client.call('session.close')['closed'] is True
        finally:
            if client is not None:
                record.update(exit=client.transport.returncode, closed=client.closed,
                              cleanup_error=str(client.cleanup_error) if client.cleanup_error else None)
            record['wire'] = {file.name: file.read_bytes().decode('utf-8', 'backslashreplace')
                              for file in sorted(trace.glob('*'))}
        assert record['exit'] == 0 and record['closed'] and record['cleanup_error'] is None, record

    def step(self, client, session, tick, request, ticks=1, **params):
        result = client.call('runtime.step', dict(session_id=session, request_id=uid(request),
                                                  expected_tick=tick, ticks=ticks, **params))
        assert result['current_session_id'] == session and result['current_tick'] == tick+ticks, result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result

    def observed(self, client, session, tick, **params):
        result = client.call('runtime.particles.inspect', dict(session_id=session, tick=tick, **params))
        assert result['session_id'] == session and result['tick'] == tick and result['active'] is True, result
        counts = result['counts']
        assert set(counts) == set(COUNT_FIELDS) and all(type(n) is int and n >= 0 for n in counts.values()), counts
        assert counts['spawned'] == counts['live']+counts['expired']+counts['removed'], counts
        for key, id_key in [('sources', 'id'), ('particles', 'id')]:
            ids = [row[id_key] for row in result[key]]
            assert all(isinstance(value, str) and value.isascii() and value.isdecimal() and
                       str(int(value)) == value and int(value) > 0 for value in ids), (key, ids)
            assert list(map(int, ids)) == sorted(set(map(int, ids))), (key, ids)
        source_ids = {row['id'] for row in result['sources']}
        assert all(row['emitter'] in source_ids for row in result['particles']), result
        if not params:
            assert result['next_after'] is None and len(result['particles']) == counts['live'], result
        return result

    def reject_observation(self, client, session, tick, code, **params):
        try:
            self.observed(client, session, tick, **params)
        except RpcError as failure:
            assert failure.code == code, failure
        else:
            raise AssertionError(f'Particle observation accepted {params!r} at tick {tick}')

    def validate_page(self, client, session, tick, full):
        first = self.observed(client, session, tick, limit=1)
        assert first['particles'] == full['particles'][:1], first
        assert first['next_after'] == first['particles'][0]['id'], first
        second = self.observed(client, session, tick, limit=2, after=first['next_after'])
        assert second['particles'] == full['particles'][1:3], second
        assert second['next_after'] == second['particles'][-1]['id'], second
        third = self.observed(client, session, tick, limit=64, after=second['next_after'])
        assert third['particles'] == full['particles'][3:] and third['next_after'] is None, third
        empty = self.observed(client, session, tick, limit=1, after=third['particles'][-1]['id'])
        assert empty['particles'] == [] and empty['next_after'] is None, empty
        for page in (first, second, third, empty):
            assert page['counts'] == full['counts'] and page['sources'] == full['sources'], page
        self.reject_observation(client, session, tick-1, -32009, limit=1, after=first['next_after'])
        self.reject_observation(client, uid(999), tick, -32030)
        self.reject_observation(client, session, tick, -32602, limit=0)
        self.reject_observation(client, session, tick, -32602, limit=65)
        self.reject_observation(client, session, tick, -32602, after='invalid')

    def verify_main(self, state):
        tick = state['tick']
        counts_by_tick = {0: counters(0, 0), 1: counters(0, 0), 5: counters(0, 0),
                          CHECKPOINT: counters(5, 5, 1), 11: counters(5, 5, 1),
                          13: counters(6, 6, 6), FINAL: counters(1, 6, 6, 5)}
        assert state['counts'] == counts_by_tick[tick], state
        sources = {row['entity']: row for row in state['sources']}
        assert set(sources) == set(EMITTERS) and all(row['draining'] is False for row in sources.values()), state
        for index, entity in enumerate(EMITTERS):
            source = sources[entity]
            if tick >= CHECKPOINT:
                near_vector(source['position_m'], (BIRTH if tick == CHECKPOINT else MOVED)[index])
                expected = TURN if tick == CHECKPOINT else HALF_TURN
                sign = 1 if sum(a*b for a, b in zip(source['rotation'], expected)) >= 0 else -1
                near_vector([sign*value for value in source['rotation']], expected)
            elif tick in (0, 1):
                near_vector(source['position_m'], ORIGINS[index])
                near_vector(source['rotation'], IDENTITY)
            elif tick == 5:
                near_vector(source['position_m'], [start+.8*(end-start) for start, end in zip(ORIGINS[index], BIRTH[index])])
            members = [row for row in state['particles'] if row['emitter'] == source['id']]
            assert source['counts']['live'] == len(members), source
            for row in members:
                age = row['age_ticks']
                near_vector(row['linear_rgba'], [*COLORS[index], 1])
                near_vector([row['size_m']], [.32])
                assert row['lifetime_ticks'] == 12
                if index == 0:
                    assert age == tick-CHECKPOINT
                    if tick == CHECKPOINT:
                        position, velocity = [BIRTH[0][0], BIRTH[0][1]+.45, -4], [0, 1.8, 0]
                    else:
                        position = [MOVED[0][0]-.45-1.8*age/60-displacement(age), MOVED[0][1], -4]
                        velocity = [-1.8-.6*age/60, 0, 0]
                elif age == tick-CHECKPOINT:
                    position, velocity = [BIRTH[1][0]+displacement(age), BIRTH[1][1]+.45+1.8*age/60, -4], [.6*age/60, 1.8, 0]
                else:
                    assert age == tick-12
                    position, velocity = [MOVED[1][0]-.45-1.8*age/60+displacement(age), MOVED[1][1], -4], [-1.8+.6*age/60, 0, 0]
                near_vector(row['position_m'], position)
                near_vector(row['velocity_m_s'], velocity)

    def first_burst(self, client, session):
        initial = self.observed(client, session, 0)
        self.step(client, session, 0, 3100, inputs=inputs())
        held = self.observed(client, session, 1)
        self.step(client, session, 1, 3101, ticks=4, motions=motions(BIRTH, TURN), inputs=inputs())
        pending = self.observed(client, session, 5)
        # The press and last kinematic substep share a committed tick.
        self.step(client, session, 5, 3102, inputs=inputs(pressed=True))
        burst = self.observed(client, session, CHECKPOINT)
        for state in (initial, held, pending, burst):
            self.verify_main(state)
        self.validate_page(client, session, CHECKPOINT, burst)
        return burst

    def continue_burst(self, client, session):
        self.step(client, session, CHECKPOINT, 3110, ticks=5, motions=motions(MOVED, HALF_TURN), inputs=inputs())
        moved = self.observed(client, session, 11)
        self.step(client, session, 11, 3111, ticks=2, inputs=inputs(pressed=True))
        second = self.observed(client, session, 13)
        self.step(client, session, 13, 3112, ticks=6, inputs=inputs())
        aged = self.observed(client, session, FINAL)
        for state in (moved, second, aged):
            self.verify_main(state)
        assert [row['id'] for row in moved['particles']] == [row['id'] for row in second['particles'][:-1]], second
        assert aged['particles'][0]['id'] == second['particles'][-1]['id'], aged
        return [moved, second, aged]

    def save(self, client, saves, slot, session, tick, request, structure=0):
        receipt = client.call('save.write', dict(request_id=uid(request), configuration_generation=1,
            slot=slot, expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0,
            expected_structure_revision=structure))
        payload_file, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = payload_file.read_bytes()
        payload = json.loads(raw)['snapshot']['payload']
        extension = payload['particles']
        assert payload['tick'] == extension['state']['status']['tick'] == tick and extension['version'] == 1
        assert len(raw) == receipt['bytes'] and file_hash(payload_file) == receipt['sha256'], receipt
        self.measurements.setdefault('slots', {})[saves.name+'/'+slot] = dict(receipt=receipt, archive_utf8=raw.decode('utf-8'))
        return raw

    def load(self, client, revision, previous, session, slot, tick, request):
        loaded = client.call('save.load', dict(request_id=uid(request), configuration_generation=1, slot=slot,
            expected_generation=1, revision=revision, expected_session_id=previous, expected_tick=0,
            expected_gameplay_revision=0, expected_structure_revision=0, new_session_id=session))
        assert loaded['session_id'] == session and loaded['tick'] == tick and loaded['source_stale'] is False, loaded
        return loaded['structure_revision']

    def initialize(self, client, revision, session, saves, game):
        if not game:
            client.call('save.configure', dict(request_id=uid(3010), expected_generation=0, root=self.native(saves)))
            client.call('runtime.start', dict(session_id=session, revision=revision))
        status = client.call('runtime.status')
        assert status['active'] and status['tick'] == 0 and status['authored_revision'] == revision, status
        assert game or status['session_id'] == session, status
        return status['session_id']

    def remove_sources(self, client, session):
        spawn = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(3130),
            expected_tick=FINAL, expected_structure_revision=0, spawns=[dict(template_id=key) for key in TEMPLATES]))
        attached, detached = spawn['spawned']
        assert spawn['structure_revision'] == 1 and attached != detached, spawn
        registered = self.observed(client, session, FINAL)
        assert {row['entity'] for row in registered['sources']} == {*EMITTERS, attached, detached}, registered
        self.step(client, session, FINAL, 3131, expected_structure_revision=1, inputs=inputs(TEMPLATE_ACTION, pressed=True))
        born = self.observed(client, session, DETACHED)
        assert born['counts'] == counters(5, 10, 6, 5), born
        try:
            client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(3133),
                expected_tick=DETACHED, expected_structure_revision=1,
                spawns=[dict(template_id=TEMPLATES[1]) for _ in range(13)]))
        except RpcError as failure:
            assert failure.code == -32602 and 'particle emitter capacity' in str(failure), failure
        else:
            raise AssertionError('Runtime admitted more than 16 particle sources')
        assert self.observed(client, session, DETACHED) == born
        assert client.call('runtime.status')['structure_revision'] == 1
        self.measurements['capacity_rollback'] = True
        sources = {row['entity']: row['id'] for row in born['sources']}
        detached_source, attached_source = sources[detached], sources[attached]
        for entity, origin in [(attached, [5, 3, -4]), (detached, [7, 3, -4])]:
            births = [row for row in born['particles'] if row['emitter'] == sources[entity]]
            assert len(births) == 2 and births[0]['position_m'] != births[1]['position_m'], births
            for row in births:
                x, y, z = row['position_m']
                assert origin[0]+.25 < x < origin[0]+.6 and 2.8 < y < 3.2 and abs(z+4) < 4e-5, row
                vx, vy, vz = row['velocity_m_s']
                assert .4 < vx < .8 and -.3 < vy < .3 and abs(vz) < 4e-5, row
        attachment = dict(entity=attached, mode='snap', anchor=EMITTERS[0], offset=transform([0, 0, 0], TURN))
        guard = dict(session_id=session, expected_tick=DETACHED, expected_structure_revision=1)
        invalid = attachment | dict(offset=attachment['offset'] | dict(scale=[2, 1, 1]))
        try:
            client.call('runtime.attachment.apply', dict(guard, request_id=uid(3134), commands=[invalid]))
        except RpcError as failure:
            assert failure.code == -32602 and 'proper world transform' in str(failure), failure
        else:
            raise AssertionError('Attachment applied a nonrigid particle emitter pose')
        assert self.observed(client, session, DETACHED) == born
        changed = client.call('runtime.attachment.apply', dict(guard, request_id=uid(3135), commands=[attachment]))
        assert changed['structure_revision'] == 2
        moved = self.observed(client, session, DETACHED)
        assert moved['counts'] == born['counts']
        near_vector(next(row['position_m'] for row in moved['sources'] if row['id'] == attached_source), MOVED[0])
        for before, after in zip(born['particles'], moved['particles'], strict=True):
            if before['emitter'] != attached_source:
                assert before == after
                continue
            x, y, z = before['position_m']
            near_vector(after['position_m'], [MOVED[0][0] - (x-5), MOVED[0][1] - (y-3), z])
            assert after['age_ticks'] == before['age_ticks'] == 0
        reset = client.call('runtime.attachment.apply', dict(guard, request_id=uid(3136),
            expected_structure_revision=2, commands=[dict(entity=attached, mode='reset')]))
        assert reset['structure_revision'] == 3
        assert self.observed(client, session, DETACHED) == born
        self.measurements['same_tick_attachment'] = True
        gone = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(3132),
            expected_tick=DETACHED, expected_structure_revision=3, despawns=[attached, detached]))
        assert gone['despawned'] == [attached, detached] and gone['structure_revision'] == 4, gone
        observed = self.observed(client, session, DETACHED)
        assert observed['counts'] == counters(3, 10, 6, 5, 2), observed
        source_rows = {row['id']: row for row in observed['sources']}
        assert attached_source not in source_rows and source_rows[detached_source]['draining'] is True, observed
        assert source_rows[detached_source]['entity'] == detached and source_rows[detached_source]['emitting'] is False, observed
        assert observed['particles'] == [row for row in born['particles'] if row['emitter'] != attached_source], observed
        return observed, detached_source, gone['structure_revision']

    def finish_drain(self, client, session, structure, detached_source):
        self.step(client, session, DETACHED, 3140, expected_structure_revision=structure)
        draining = self.observed(client, session, DETACHED+1)
        assert draining['counts'] == counters(3, 10, 6, 5, 2), draining
        assert sum(row['emitter'] == detached_source and row['age_ticks'] == 1 for row in draining['particles']) == 2, draining
        self.step(client, session, DETACHED+1, 3141, ticks=3, expected_structure_revision=structure)
        drained = self.observed(client, session, EXPIRED)
        assert drained['counts'] == counters(0, 10, 6, 8, 2), drained
        assert {row['entity'] for row in drained['sources']} == set(EMITTERS), drained
        return draining, drained

    def continuation(self, revision, *, game=None):
        name = 'standalone' if game else 'source'
        saves = self.root/(name+' slots')
        saves.mkdir()
        with self.owner(name+' original', game=game, saves=saves) as (client, original_owner):
            original = self.initialize(client, revision, uid(3090), saves, game)
            burst = self.first_burst(client, original)
            checkpoint = self.save(client, saves, 'checkpoint', original, CHECKPOINT, 3150)
            expected = self.continue_burst(client, original)
            final = self.save(client, saves, 'final', original, FINAL, 3151)
            assert checkpoint != final
            if not game:
                detached, draining_id, detached_structure = self.remove_sources(client, original)
                detached_bytes = self.save(client, saves, 'detached', original, DETACHED, 3152, structure=detached_structure)
                expected_drain = self.finish_drain(client, original, detached_structure, draining_id)
                drained_bytes = self.save(client, saves, 'drained', original, EXPIRED, 3153, structure=detached_structure)
                assert detached_bytes != drained_bytes

        restored = uid(3091)
        with self.owner(name+' restored', game=game, saves=saves) as (client, restored_owner):
            active = self.initialize(client, revision, uid(3092), saves, game)
            assert len({original, active, restored}) == 3
            assert self.load(client, revision, active, restored, 'checkpoint', CHECKPOINT, 3160) == 0
            assert self.observed(client, restored, CHECKPOINT) == dict(burst, session_id=restored)
            assert self.continue_burst(client, restored) == [dict(row, session_id=restored) for row in expected]
            assert self.save(client, saves, 'replayed', restored, FINAL, 3161) == final
        assert original_owner['pid'] != restored_owner['pid']

        if not game:
            cold = uid(3093)
            with self.owner('draining restored', saves=saves) as (client, drain_owner):
                active = self.initialize(client, revision, uid(3094), saves, game)
                structure = self.load(client, revision, active, cold, 'detached', DETACHED, 3170)
                assert structure == detached_structure and self.observed(client, cold, DETACHED) == dict(detached, session_id=cold)
                assert list(self.finish_drain(client, cold, structure, draining_id)) == [dict(row, session_id=cold) for row in expected_drain]
                assert self.save(client, saves, 'drain-replayed', cold, EXPIRED, 3171, structure=structure) == drained_bytes
            assert drain_owner['pid'] not in (original_owner['pid'], restored_owner['pid'])
        self.measurements[name+'_continuation'] = dict(checkpoint=burst, final=expected[-1], exact=True)

    def verify_game(self, game, executable, external, revision):
        def observe(label, path, **comparison):
            return dict(expect=dict(label=label, method='runtime.particles.inspect', path=path, **comparison))

        expected_x = integrated_position(ORIGINS[0], 7)[0]
        held = dict(step=dict(ticks=1, inputs=inputs()))
        groups = [dict(name='Held without press', sequence=[held, observe('No births on held input', '/counts/spawned', equals=0)]),
                  dict(name='One press, then age', sequence=[held, dict(step=dict(ticks=8, inputs=inputs(pressed=True))),
                      observe('Five live particles', '/counts/live', equals=5),
                      observe('One capped birth', '/counts/dropped', equals=1),
                      observe('The first particle aged seven ticks', '/particles/0/age_ticks', equals=7),
                      observe('The first particle moved along its birth axis', '/particles/0/position_m/0',
                              between=[expected_x-4e-5, expected_x+4e-5])])]
        suite, report = external/'particles.verify.json', external/'particles.report.json'
        specification = dict(format='poima.game-verification', version=1, groups=groups)
        suite.write_text(encode(specification), encoding='utf-8')
        result = success(json.loads(self.cli('game', 'verify', self.native(game), '--suite', self.native(suite),
            '--report', self.native(report), '--completed-exit', 'true', '--expected-bundle-sha256', file_hash(game),
            '--expected-suite-sha256', file_hash(suite), executable=executable, cwd=external)))
        self.measurements['standalone_verification'] = dict(suite=specification, report=result)
        assert result == json.loads(report.read_bytes()) and result['passed'] and result['revision'] == revision, result
        assert result['bundle_sha256'] == file_hash(game) and result['suite_sha256'] == file_hash(suite), result
        assert result['groups_passed'] == result['groups_total'] == 2, result
        assert [row['ticks'] for row in result['groups']] == [1, 9], result
        assert [len(row['checks']) for row in result['groups']] == [1, 4], result
        assert all(check['passed'] and check['observed'] and not check['actual_omitted']
                   for row in result['groups'] for check in row['checks']), result

    def capture_game(self, game, executable, external, revision):
        width, height = CAPTURE_SIZE
        captures = self.measurements['vulkan'] = {}
        for label, tick, age in [('closed', 1, 0), ('birth', 2, 0), ('advanced', 9, 7)]:
            active = tick > 1
            sequence = [dict(ticks=1, actions=[action()])]
            if active:
                sequence.append(dict(ticks=tick-1, actions=[action(pressed=True)]))
            replay, bmp, report = (external/(label+suffix) for suffix in ('.json', '.bmp', '.report.json'))
            replay.write_text(encode(sequence), encoding='utf-8')
            result = success(json.loads(self.cli('game', 'run', self.native(game), '--gpu', self.args.gpu,
                '--replay', self.native(replay), '--capture', self.native(bmp), '--report', self.native(report),
                '--width', width, '--height', height, '--samples', 1, '--frames-in-flight', 2,
                executable=executable, cwd=external)))
            play = result['play']
            receipt = captures[label] = dict(play=play, image=str(bmp), sha256=file_hash(bmp), witnesses=[])
            assert json.loads(report.read_bytes()) == result and result['runtime']['tick'] == play['tick'] == tick, result
            assert play['success'] and play['stop_reason'] == 'replay_complete' and play['revision'] == revision, play
            assert play['hardware'] and play['frames_presented'] > 0 and play['capture_written'] and play['nvrhi_errors'] == 0, play
            assert (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, 1), play
            diagnostics = play['render_diagnostics']
            draws = diagnostics['last_draws']
            assert diagnostics['completed_submissions'] > 0, diagnostics
            assert diagnostics['frame_execution']['limit'] == 2, diagnostics
            if active:
                baseline = captures['closed']['play']['render_diagnostics']['frame_execution']
                assert diagnostics['frame_execution']['device_idle_waits'] == baseline['device_idle_waits'], diagnostics
            assert (draws['objects'], draws['camera_draws'], draws['camera_triangles']) == ((2, 2, 10) if active else (0, 0, 0)), draws
            presented = diagnostics['scene_receipts']['presented']
            assert presented['camera_id'] == uid(3) and presented['camera_lens'] == CAPTURE_LENS, presented
            camera = presented.get('camera_pose', {}).get('camera_world') or play['camera_world']
            image = pixels(bmp)
            assert len(image) == height and all(len(row) == width for row in image)
            for channel, origin in enumerate(ORIGINS):
                points = [(integrated_position(origin, age), active)]
                if age:
                    points.append((integrated_position(origin, 0), False))
                for center, expected in points:
                    u, v = map(math.floor, projection(camera, CAPTURE_LENS, center))
                    assert 1 <= u < width-1 and 1 <= v < height-1, (center, u, v)
                    colors = [image[y][x] for y in range(v-1, v+2) for x in range(u-1, u+2)]
                    marked = sum(tagged(color, channel) for color in colors)
                    witness = dict(world=center, pixel=[u, v], expected=expected, colors=colors, marked=marked)
                    receipt['witnesses'].append(witness)
                    assert marked == (9 if expected else 0), witness

    def export(self, revision):
        output = self.root/'Initial particle export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Particle Game', self.root/'Unavailable Particle Project'
        output.rename(bundle)
        self.project.rename(detached)
        assert not output.exists() and not self.project.exists()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        game = bundle/'game.json'
        executable = bundle/json.loads(game.read_bytes())['engine']['executable']
        installed = json.loads((bundle/'runtime'/'runtime.json').read_bytes())
        assert installed['features']['scene_particles'] is True
        external = self.root/'External Particle Replay'
        external.mkdir()
        info = success(json.loads(self.cli('game', 'inspect', self.native(game))))
        assert info['revision'] == revision, info
        if self.args.runtime:
            self.verify_game(game, executable, external, revision)
            self.continuation(revision, game=game)
        if self.args.capture_output:
            self.capture_game(game, executable, external, revision)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored
        self.measurements['export'] = dict(bundle=str(bundle), inventory=frozen, detached_source=authored)

    def run(self):
        revision = self.author_project()
        before = tree_hashes(self.project)
        if self.args.runtime:
            self.continuation(revision)
        assert tree_hashes(self.project) == before
        if self.args.runtime_root:
            self.export(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed standalone runtime.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture-output', type=Path, help='Retain actual Windows Vulkan replay captures.')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    if args.capture_output and not (args.runtime_root and args.runtime):
        parser.error('--capture-output requires --runtime-root and native simulation.')

    def execute(directory):
        test = ParticleContract(args, Path(directory))
        evidence = dict(passed=False, runtime=bool(args.runtime), calls=test.calls, measurements=test.measurements)
        try:
            test.run()
            evidence['passed'] = True
        except BaseException:
            evidence['failure'] = traceback.format_exc()
            raise
        finally:
            paths = {args.evidence, Path(directory)/'evidence.json' if args.capture_output else None} - {None}
            for path in paths:
                path.write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
                print(f'Evidence file {path}')

    if args.capture_output:
        args.capture_output.mkdir(parents=True, exist_ok=True)
        execute(tempfile.mkdtemp(prefix=f'particles-gpu-{args.gpu}-', dir=args.capture_output.resolve()))
    else:
        scratch = Path(__file__).resolve().parents[1]/'build'/'world-particles-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            execute(directory)
    print('Particle authoring contract passed.' + (' Native simulation and cold saves matched.' if args.runtime else ''))


if __name__ == '__main__':
    main()
