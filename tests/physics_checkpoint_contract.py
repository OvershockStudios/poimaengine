#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare warm and fresh-owner continuation on original sloped supports."""
import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import traceback

from snow_foot_release_save_refusals import reseal_slot
from world_terrain_contract import encode, file_hash, uid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/python'))
from poima_client import RpcError, WorldClient


RAMP, ACTOR, PLATFORM, SLIDER = map(uid, (16, 107, 200, 201))
SETTLED, STOP, CHECKPOINT, FINAL = 30, 46, 75, 120
AX, AZ = .0322, .0643


def entity_ops(entity, name, position, rotation=(0, 0, 0, 1), **components):
    components = dict(Transform=dict(position=position, rotation=rotation, scale=[1]*3)) | components
    return [dict(op='entity.create', id=entity, name=name),
            *(dict(op='component.set', id=entity, type=kind, value=value) for kind, value in components.items())]


def binding(definition):
    return dict(op='physics_checkpoint.set', definition=definition)


class Continuation:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        output.mkdir()
        self.saves = output/'saves'
        self.saves.mkdir()
        self.world = output/'slopes.world.json'
        self.sequence = 1000
        self.report = {'owners': [], 'archives': {}}

    def request(self):
        self.sequence += 1
        return uid(self.sequence)

    def native(self, path):
        if self.args.windows_interop:
            return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
        return str(path)

    @contextmanager
    def owner(self, label):
        trace = self.output/f'{label}-wire'
        opened = WorldClient.open(self.args.binary, world_path=self.native(self.world), cwd=self.output,
                                 trace_directory=trace, trace_limit_bytes=2*1024*1024)
        log = dict(label=label, pid=opened.transport.process_id, trace=str(trace))
        self.report['owners'].append(log)
        try:
            with opened as client:
                yield client
                assert client.call('session.close')['closed'] is True
        finally:
            log.update(exit=opened.transport.returncode, closed=opened.closed)
        assert log['exit'] == 0 and log['closed'] and opened.cleanup_error is None, log

    def transaction(self, client, revision, operations):
        receipt = client.call('world.transact', dict(request_id=self.request(),
                              base_revision=revision, ops=operations))
        assert receipt['committed'] is True and receipt['revision'] == revision+1, receipt
        return receipt['revision']

    def author(self, client, selected):
        discovery = client.call('world.describe')
        self.report['schema_revision'] = discovery['schema_revision']
        if not self.args.legacy_repro:
            profile = discovery['physics_checkpoint']
            assert (profile['available'] is True and profile['binding_operation'] == 'physics_checkpoint.set'
                    and profile['profile_version'] == 1), profile
        heights = [AX*(x-2)*1.5+AZ*(z-2)*1.5 for z in range(5) for x in range(5)]
        recipe = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=5, samples_z=5,
                           tile_cells=4, origin_m=[45, 0, -3], spacing_m=[1.5, 1.5], heights_m=heights))
        plan = client.call('world.terrain.inspect', dict(recipe=recipe))
        assert client.call('world.terrain.cook', dict(recipe=recipe,
                           expected_recipe_sha256=plan['recipe_sha256'])) == plan
        tile, = plan['tiles']
        half_angle = math.radians(4)
        rotation = (0, 0, math.sin(half_angle), math.cos(half_angle))
        ops = entity_ops(ACTOR, 'Sloped capsule', [48.21, .4, .29], CharacterController=dict(
            radius=.3, height=1.8, speed=4.55, jump_speed=4.8, camera=None,
            stance=dict(camera_drop=.02, crouched_height=1.1, crouched_speed=1.98)))
        ops += entity_ops(RAMP, 'Cooked inclined terrain', [0, 0, 0], MeshCollider=dict(
            asset=tile['asset'], primitive=tile['primitive'], friction=.7, restitution=0))
        ops += entity_ops(PLATFORM, 'Tilted box support', [61, -.25/math.cos(2*half_angle), 0], rotation,
            BoxCollider=dict(half_extents=[8, .25, 5], motion='static', mass=1, friction=.5, restitution=0))
        ops += entity_ops(SLIDER, 'Frictionless dynamic slider', [61, .4, 0], rotation,
            BoxCollider=dict(half_extents=[.3, .2, .3], motion='dynamic', mass=1, friction=0, restitution=0))
        revision = self.transaction(client, 0, ops)
        assert 'physics_checkpoint' not in client.call('world.inspect')
        if selected:
            preview = client.call('world.transact', dict(request_id=self.request(), base_revision=revision,
                preview=True, ops=[binding(dict(version=1))]))
            assert preview['committed'] is False and preview['changed_world_fields'] == ['physics_checkpoint']
            assert 'physics_checkpoint' not in client.call('world.inspect')
            sent = dict(request_id=self.request(), base_revision=revision, ops=[binding(dict(version=1))])
            receipt = client.call('world.transact', sent)
            assert receipt['committed'] and receipt['revision'] == revision + 1
            assert client.call('world.transact', sent) == receipt | dict(replayed=True)
            try:
                client.call('world.transact', sent | dict(ops=[binding(dict(version=1.0))]))
            except RpcError as error:
                assert error.code == -32010, error
            else:
                raise AssertionError('Float version accepted as an identical integer-version retry.')
            revision = receipt['revision']
            assert client.call('world.inspect')['physics_checkpoint'] == dict(version=1)
            assert client.call('world.dependencies')['needs_physics_checkpoint'] is True
            before = self.world.read_bytes()
            rejected = []
            for invalid in (True, {}, dict(version=2), dict(version=True), dict(version=1, extra=0)):
                try:
                    client.call('world.transact', dict(request_id=self.request(), base_revision=revision,
                                                      ops=[binding(invalid)]))
                except RpcError as error:
                    assert error.code == -32602, error
                    rejected.append(dict(definition=invalid, code=error.code, message=error.message))
                else:
                    raise AssertionError(f'Invalid physics checkpoint profile accepted: {invalid!r}')
                assert self.world.read_bytes() == before
            actor = client.call('entity.get', dict(id=ACTOR, component='CharacterController'))['value']
            try:
                client.call('world.transact', dict(request_id=self.request(), base_revision=revision,
                    ops=[dict(op='component.set', id=ACTOR, type='CharacterController',
                        value=actor | dict(locomotion={}))]))
            except RpcError as error:
                assert error.code == -32602 and 'requires rigid-body character controllers' in error.message, error
            else:
                raise AssertionError('Virtual motor accepted into a joined ordinary checkpoint world.')
            assert self.world.read_bytes() == before
            undo = client.call('world.undo', dict(request_id=self.request(), base_revision=revision))
            assert undo['changed_world_fields'] == ['physics_checkpoint']
            assert 'physics_checkpoint' not in client.call('world.inspect')
            redo = client.call('world.redo', dict(request_id=self.request(), base_revision=undo['revision']))
            assert redo['changed_world_fields'] == ['physics_checkpoint']
            assert client.call('world.inspect')['physics_checkpoint'] == dict(version=1)
            revision = redo['revision']
            self.report['profile_refusals'] = rejected
        elif not self.args.legacy_repro:
            assert 'needs_physics_checkpoint' not in client.call('world.dependencies')
        return revision

    def configure(self, client):
        return client.call('save.configure', dict(request_id=self.request(), expected_generation=0,
                           root=self.native(self.saves)))['generation']

    def write(self, client, session, tick, generation, slot):
        receipt = client.call('save.write', dict(session_id=session, expected_tick=tick,
            expected_gameplay_revision=0, configuration_generation=generation,
            request_id=self.request(), slot=slot, expected_generation=0))
        path, = (self.saves/('slot-'+slot)).glob('p-*.bin')
        raw = path.read_bytes()
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        assert json.loads(raw)['snapshot']['payload']['tick'] == tick
        self.report['archives'][slot] = dict(receipt=receipt, path=str(path))
        return raw

    def step(self, client, session, tick, count=1, inputs=None):
        params = dict(session_id=session, expected_tick=tick, request_id=self.request(), ticks=count)
        if inputs is not None:
            params['inputs'] = inputs
        result = client.call('runtime.step', params)
        assert result['tick'] == tick+count, result

    def load(self, client, revision, generation, session, slot, current=None):
        return client.call('save.load', dict(request_id=self.request(), configuration_generation=generation,
            slot=slot, expected_generation=1, revision=revision, expected_session_id=current,
            expected_tick=CHECKPOINT if current else None, expected_gameplay_revision=0 if current else None,
            expected_structure_revision=0 if current else None, new_session_id=session))

    def reject_corruption(self, client, revision, generation, session, checkpoint):
        payload = ('snapshot', 'payload')
        rows = json.loads(checkpoint)['snapshot']['payload']['entities']
        actor_index, = (i for i, row in enumerate(rows) if row['id'] == ACTOR)
        original = self.saves/'slot-checkpoint'
        cases = (
            ('bad-joined-version', payload + ('physical', 'version'), 2, 'snapshot extension version differs'),
            ('bad-body-witness', payload + ('entities', actor_index, 'body', 'linear_velocity', 0),
             .125, 'body differs from its ordinary saved witness'),
        )
        observations = self.observe(client, session, CHECKPOINT)
        failures = []
        for index, (slot, path, replacement, diagnostic) in enumerate(cases):
            identity = reseal_slot(original, self.saves, slot, path, replacement)
            try:
                self.load(client, revision, generation, uid(160+index), slot, current=session)
            except RpcError as error:
                assert error.code == -32070 and diagnostic in error.message, error
                failures.append(dict(slot=slot, receipt=identity, code=error.code, message=error.message))
            else:
                raise AssertionError(f'Sealed malformed checkpoint accepted: {slot}')
            assert self.observe(client, session, CHECKPOINT) == observations
        self.report['archive_refusals'] = failures
        assert self.write(client, session, CHECKPOINT, generation, 'after-rejections') == checkpoint

    def observe(self, client, session, tick):
        def actor_observation(entity, floor):
            guard = dict(session_id=session, tick=tick)
            row = client.call('runtime.entity', guard | dict(id=entity))
            position = row['world_matrix'][12:15]
            hit = client.call('runtime.raycast', guard | dict(origin=[position[0], position[1]+.3, position[2]],
                direction=[0, -1, 0], distance=2, ignore=[entity]))['hit']
            assert hit['entity'] == floor and hit['normal'][1] > .98, (row, hit)
            return dict(position=position, velocity=row['velocity'], ground=row['ground'], support=hit)
        actor, slider = actor_observation(ACTOR, RAMP), actor_observation(SLIDER, PLATFORM)
        assert actor['ground'] == 'on_ground' and type(actor['support']['triangle']) is int, actor
        assert abs(actor['position'][1]-actor['support']['position'][1]) < .003, actor
        assert slider['support']['triangle'] is None and slider['velocity'][0] < -1, slider
        assert .19 < slider['position'][1]-slider['support']['position'][1] < .21, slider
        return dict(actor=actor, slider=slider)

    def timeline(self, client, session, generation, label):
        observations = [self.observe(client, session, CHECKPOINT)]
        for tick in range(CHECKPOINT, FINAL):
            self.step(client, session, tick)
            observations.append(self.observe(client, session, tick+1))
            if tick == CHECKPOINT:
                after_one = self.write(client, session, tick+1, generation, label+'-first')
        final = self.write(client, session, FINAL, generation, label+'-final')
        return observations, after_one, final

    def run(self, selected):
        with self.owner('warm') as client:
            revision = self.author(client, selected)
            generation = self.configure(client)
            session = uid(110)
            client.call('runtime.start', dict(session_id=session, revision=revision))
            self.step(client, session, 0, SETTLED)
            moving = dict(entity=ACTOR, move=[.28, .4], look=[0, 0], jump=False, use=False)
            self.step(client, session, SETTLED, STOP-SETTLED, [moving])
            self.step(client, session, STOP, CHECKPOINT-STOP)
            checkpoint = self.write(client, session, CHECKPOINT, generation, 'checkpoint')
            warm, warm_first, warm_final = self.timeline(client, session, generation, 'warm')

        with self.owner('cold') as client:
            generation = self.configure(client)
            session = uid(120)
            load = self.load(client, revision, generation, session, 'checkpoint')
            assert load['tick'] == CHECKPOINT and load['source_stale'] is False, load
            restored = self.write(client, session, CHECKPOINT, generation, 'restored')
            if selected:
                self.reject_corruption(client, revision, generation, session, checkpoint)
            cold, cold_first, cold_final = self.timeline(client, session, generation, 'cold')

        assert len({row['pid'] for row in self.report['owners']}) == 2
        assert checkpoint == restored and warm[0] == cold[0]
        actor_rows = [row for row in json.loads(checkpoint)['snapshot']['payload']['entities'] if row['id'] == ACTOR]
        actor, = actor_rows
        assert actor['body']['active'] is True and 'motor' not in actor['controller'], actor
        assert warm[-1]['slider']['position'][0] < warm[0]['slider']['position'][0]-1
        differences = [index+CHECKPOINT for index, (a, b) in enumerate(zip(warm, cold, strict=True)) if a != b]
        distance = math.dist(warm[-1]['actor']['position'], cold[-1]['actor']['position'])
        self.report.update(checkpoint_bytes_equal=checkpoint == restored, first_tick_bytes_equal=warm_first == cold_first,
                           terminal_bytes_equal=warm_final == cold_final, differing_ticks=differences,
                           terminal_actor_delta_m=distance, warm=warm, cold=cold)
        for raw in (checkpoint, warm_first, cold_first, warm_final, cold_final):
            snapshot = json.loads(raw)['snapshot']
            payload = snapshot['payload']
            if selected:
                joined = payload['physical']
                assert set(joined) == {'version', 'bootstrap', 'rigs', 'constraint_order', 'owner', 'physics'}
                assert joined['version'] == 1 and joined['rigs'] == joined['constraint_order'] == [], joined
                assert 'kinematic_sleep' not in payload
            else:
                assert snapshot['version'] == 8 and 'physical' not in payload and 'kinematic_sleep' not in payload
        if selected:
            joined = json.loads(checkpoint)['snapshot']['payload']['physical']
            mapped = {row['entity']: row['id'] for row in joined['bootstrap']['bodies']}
            assert set(mapped) == {ACTOR, RAMP, SLIDER, PLATFORM}, mapped
            native = {row['id']: row for row in joined['physics']['bodies']}
            assert native[mapped[ACTOR]]['active'] is native[mapped[SLIDER]]['active'] is True, native
            assert native[mapped[ACTOR]]['motion']['sleep_timer'] > .45
            active = joined['owner']['active_rigid_order']
            assert len(active) == 2 and set(active) == {mapped[ACTOR], mapped[SLIDER]}, active
            support_pairs = {frozenset((pair['body_a'], pair['body_b']))
                             for pair in joined['physics']['pairs'] if pair['manifolds']}
            assert {frozenset((mapped[ACTOR], mapped[RAMP])),
                    frozenset((mapped[SLIDER], mapped[PLATFORM]))} <= support_pairs, support_pairs
            assert not differences and warm_first == cold_first and warm_final == cold_final, differences
        else:
            assert differences and differences[0] == CHECKPOINT+1 and warm_first != cold_first, differences
            assert warm[1]['actor']['velocity'] == [0, 0, 0]
            assert math.hypot(*cold[1]['actor']['velocity']) > .01
            assert .005 < distance < .0065 and warm_final != cold_final, distance
        self.report['selected'] = selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, help='Retain the raw requests, saves and matched observations.')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--legacy-repro', action='store_true', help='Run the stock pre-checkpoint negative control alone.')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.binary = args.binary.resolve(strict=True)

    def execute(root):
        result = dict(passed=False, binary_sha256=file_hash(args.binary), cases={})
        try:
            for label, selected in [('legacy', False), ('selected', True)]:
                if selected and args.legacy_repro:
                    break
                fixture = Continuation(args, root/label)
                result['cases'][label] = fixture.report
                fixture.run(selected)
            result['passed'] = True
        except BaseException:
            result['failure'] = traceback.format_exc()
            raise
        finally:
            (root/'evidence.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')

    if args.output:
        root = args.output.resolve()
        root.mkdir(parents=True)
        execute(root)
    else:
        scratch = Path(__file__).resolve().parents[1]/'build'/'physics-checkpoint-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as root:
            execute(Path(root))
    print('Legacy slope divergence reproduced.' if args.legacy_repro else
          'Legacy slope divergence and exact selected rigid checkpoint continuation verified.')


if __name__ == '__main__':
    main()
