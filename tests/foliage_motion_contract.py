#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check native foliage-motion authoring, phase and save continuation without a renderer."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import traceback

from gltf_fixture import glb, sphere

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import RpcError, WorldClient, new_id


def uid(number):
    return f'{number:032x}'


PLANT, ACTOR, CAMERA, PLAIN, TEMPLATE = map(uid, range(1, 6))
WIND = dict(direction=[1, 0], tip_m=.5, period_ticks=16, wavelength_m=8)
TOUCH = dict(actor=ACTOR, radius_m=2, tip_m=.5)
PROFILE = dict(root_y=0, tip_y=1, wind=WIND, touch=TOUCH)
TEMPLATE_PROFILE = dict(root_y=0, tip_y=1, wind=WIND)
CHECKPOINT = 8
FINAL = 26


def create(entity, name):
    return dict(op='entity.create', id=entity, name=name)


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def transform(position=(0, 0, 0), scale=(1, 1, 1)):
    return dict(position=list(position), scale=list(scale), rotation=[0, 0, 0, 1])


def inventory(directory):
    return {path.name: path.read_bytes() for path in directory.iterdir() if path.is_file()}


def exact(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def without_session(value):
    return {key: field for key, field in value.items() if key != 'session_id'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Contract:
    def __init__(self, binary, root, evidence):
        self.binary, self.root, self.evidence = binary.resolve(), root, evidence
        self.world = root / 'foliage.world.json'
        self.saves = root / 'saves'
        self.saves.mkdir()
        self.owners = evidence['owners']

    @contextmanager
    def owner(self, label):
        trace = self.root / (label + '-trace')
        client = WorldClient.open(self.binary, self.world, trace_directory=trace, trace_limit_bytes=1024*1024)
        record = dict(label=label, pid=client.transport.process_id, trace=str(trace))
        self.owners.append(record)
        try:
            with client:
                yield client
                assert client.call('session.close')['closed'] is True
        finally:
            record.update(exit=client.transport.returncode, closed=client.closed,
                          cleanup_error=str(client.cleanup_error) if client.cleanup_error else None)
        assert record['exit'] == 0 and record['closed'] and record['cleanup_error'] is None, record

    def transaction(self, owner, revision, operations):
        return owner.call('world.transact', dict(request_id=new_id(), base_revision=revision, ops=operations))

    @staticmethod
    def refuse(owner, method, code, params, fragment=None):
        try:
            owner.call(method, params)
        except RpcError as error:
            assert error.code == code, (method, params, error)
            if fragment is not None:
                assert fragment in error.message, error
        else:
            raise AssertionError(f'{method} accepted rejected request {params}')

    def author(self, owner):
        source = self.root / 'original-analytic-leaf.glb'
        source.write_bytes(glb(*sphere(rings=6, segments=10)))
        self.asset = owner.call('asset.import', dict(source=str(source)))['asset']
        self.static_mesh = dict(asset=self.asset, primitive=0, visible=True)
        first = [
            create(PLANT, 'Moving wind witness'),
            component(PLANT, 'Transform', transform((2, 3, -3), (2, 1, 1))),
            component(PLANT, 'StaticMesh', self.static_mesh),
            component(PLANT, 'BoxCollider', dict(half_extents=[.2, .5, .2], motion='kinematic',
                                                 mass=1, friction=.5, restitution=0)),
            create(ACTOR, 'Remote touch controller'),
            component(ACTOR, 'Transform', transform((24, 3, -3))),
            component(ACTOR, 'CharacterController', dict(radius=.3, height=1.8, speed=4,
                                                          jump_speed=5, camera=None)),
            create(CAMERA, 'External scene camera'),
            component(CAMERA, 'Camera', dict(vertical_fov=60, near=.1, far=100)),
            create(PLAIN, 'Rigid control'),
            component(PLAIN, 'StaticMesh', self.static_mesh),
        ]
        assert self.transaction(owner, 0, first)['revision'] == 1

        discovered = owner.call('world.describe', dict(view='component', name='StaticMesh'))
        assert discovered['schema_revision'] == 196, discovered
        schema = discovered['components']['StaticMesh']['properties']['foliage_motion']
        assert schema['additionalProperties'] is False and set(schema['required']) == {'root_y', 'tip_y'}
        assert {tuple(rule['required']) for rule in schema['anyOf']} == {('wind',), ('touch',)}
        assert 'foliage_motion' not in owner.call('world.describe',
            dict(view='component', name='SkinnedMesh'))['components']['SkinnedMesh']['properties']
        method = owner.call('world.describe', dict(view='method', name='runtime.foliage.inspect'))['methods']
        assert set(method['runtime.foliage.inspect']['required']) == {'session_id', 'tick', 'entity'}
        assert set(method['runtime.foliage.inspect']['properties']) == {'session_id', 'tick', 'entity', 'structure_revision'}

        before = self.world.read_bytes()
        saved_assets = inventory(self.world.with_name(self.world.name + '.assets'))
        history = owner.call('world.history')
        invalid = [
            None, [], {}, dict(root_y=0, tip_y=1), PROFILE | dict(extra=True),
            PROFILE | dict(root_y=True), PROFILE | dict(tip_y='1'),
            PROFILE | dict(root_y=-1024.001), PROFILE | dict(tip_y=1024.001),
            PROFILE | dict(tip_y=.0499), PROFILE | dict(tip_y=128.0001),
            PROFILE | dict(wind=None), PROFILE | dict(touch=None),
            PROFILE | dict(wind={key: value for key, value in WIND.items() if key != 'period_ticks'}),
            PROFILE | dict(touch={key: value for key, value in TOUCH.items() if key != 'actor'}),
        ]
        for edits in (dict(direction=[0, 0]), dict(direction=[.00009, 0]), dict(direction=[1, 0, 0]),
                      dict(direction=[1.0001, 0]), dict(direction=[True, 0]),
                      dict(tip_m=-.001), dict(tip_m=2.001), dict(period_ticks=1), dict(period_ticks=3601),
                      dict(period_ticks=16.0), dict(period_ticks=True), dict(wavelength_m=.499),
                      dict(wavelength_m=256.001), dict(extra=0)):
            invalid.append(PROFILE | dict(wind=WIND | edits))
        for edits in (dict(actor=PLANT), dict(actor=PLAIN), dict(actor='f'*32), dict(actor='A'*32),
                      dict(actor=True), dict(radius_m=.049), dict(radius_m=8.001), dict(tip_m=-.001),
                      dict(tip_m=2.001), dict(extra=0)):
            invalid.append(PROFILE | dict(touch=TOUCH | edits))

        for value in invalid:
            self.refuse(owner, 'world.transact', -32602, dict(request_id=new_id(), base_revision=1,
                ops=[component(PLAIN, 'Transform', transform((500, 0, 0))), self.set_profile(value)]))
            assert self.world.read_bytes() == before
        self.refuse(owner, 'world.transact', -32602, dict(request_id=new_id(), base_revision=1,
            ops=[self.set_profile(PROFILE), component(PLANT, 'CameraDraw', dict(camera=CAMERA, mode='hidden'))]),
            'without CameraDraw')
        self.refuse(owner, 'world.transact', -32602, dict(request_id=new_id(), base_revision=1,
            ops=[self.template(PROFILE)]), 'outside the spawn catalog')
        assert self.world.read_bytes() == before and owner.call('world.history') == history
        assert inventory(self.world.with_name(self.world.name + '.assets')) == saved_assets

        published = self.transaction(owner, 1, [self.set_profile(PROFILE), self.template(TEMPLATE_PROFILE)])
        assert published['committed'] is True and published['revision'] == 2, published
        self.assert_authored(owner, PROFILE, revision=2)
        record = owner.call('template.get', dict(id=TEMPLATE, revision=2))
        assert exact(record['template']['components']['StaticMesh']['foliage_motion']) == exact(TEMPLATE_PROFILE)
        self.evidence['authoring'] = dict(revision=2, rejections=len(invalid)+2, profile=PROFILE, asset=self.asset)

    def set_profile(self, value):
        return component(PLANT, 'StaticMesh', self.static_mesh | dict(foliage_motion=value))

    def template(self, profile):
        return dict(op='template.set', id=TEMPLATE, name='Wind-only leaf',
                    components={'Transform': transform(), 'StaticMesh': self.static_mesh | dict(foliage_motion=profile)})

    def assert_authored(self, owner, profile, revision):
        response = owner.call('entity.get', dict(id=PLANT, revision=revision))
        assert exact(response['value']['components']['StaticMesh']) == exact(self.static_mesh | dict(foliage_motion=profile))

    def configure(self, owner):
        configured = owner.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=str(self.saves)))
        assert configured['generation'] == 1, configured

    def advance(self, owner, session, start, stop, structure, **more):
        stepped = owner.call('runtime.step', dict(request_id=new_id(), session_id=session, expected_tick=start,
            expected_structure_revision=structure, ticks=stop-start, **more))
        assert stepped['previous_tick'] == start and stepped['tick'] == stop and stepped['current_tick'] == stop, stepped

    def observe(self, owner, session, tick, dx, structure=0, entity=PLANT):
        params = dict(session_id=session, tick=tick, entity=entity, structure_revision=structure)
        result = owner.call('runtime.foliage.inspect', params)
        assert set(result) == {'session_id', 'tick', 'structure_revision', 'entity', 'tip_local_xz',
                               'tip_world_offset_m', 'touch_weight'}, result
        assert all(result[key] == value for key, value in params.items()), result
        # The witness is scaled by two along X. Spawned foliage has identity scale.
        local_scale = 2 if entity == PLANT else 1
        for observed, expected in zip(result['tip_local_xz'] + result['tip_world_offset_m'],
                                      [dx/local_scale, 0, dx, 0, 0], strict=True):
            assert math.isclose(observed, expected, abs_tol=2e-6, rel_tol=0), (result, dx)
        assert result['touch_weight'] == 0, result
        return result

    def write(self, owner, session, slot, tick):
        result = owner.call('save.write', dict(request_id=new_id(), configuration_generation=1, slot=slot,
            expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0,
            expected_structure_revision=1))
        path, = (self.saves / ('slot-' + slot)).glob('p-*.bin')
        assert digest(path) == result['sha256'], result
        return path

    def restore(self, owner, new_session, active=None):
        result = owner.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='checkpoint',
            expected_generation=1, revision=2, expected_session_id=active,
            expected_tick=FINAL if active else None, expected_gameplay_revision=0 if active else None,
            expected_structure_revision=1 if active else None, new_session_id=new_session))
        assert result['tick'] == CHECKPOINT and result['source_stale'] is False, result

    def continuation(self, owner, session, spawned):
        # The plant moves two metres in two 60 Hz ticks. At tick 10 the root
        # is at half a wavelength, so the wind phase is one eighth of a cycle.
        # At tick 26 it repeats. The wind-only spawn at x=8 is one wavelength
        # from the origin and gives the unshifted quarter-cycle at tick 12.
        movement = dict(entity=PLANT, position=[4, 3, -3], rotation=[0, 0, 0, 1], duration_ticks=2)
        self.advance(owner, session, CHECKPOINT, 10, 1, motions=[movement])
        position = owner.call('runtime.entity', dict(session_id=session, tick=10, id=PLANT,
                                                     structure_revision=1))['world_matrix'][12]
        assert math.isclose(position, 4, abs_tol=1e-6, rel_tol=0), position
        eighth = math.sqrt(2)/4
        results = {'tick-10': self.observe(owner, session, 10, eighth, 1)}
        previous = 10
        for tick, expected in ((12, .5), (16, 0), (20, -.5), (26, eighth)):
            self.advance(owner, session, previous, tick, 1)
            results[f'tick-{tick}'] = self.observe(owner, session, tick, expected, 1)
            if tick == 12:
                results['spawned-12'] = self.observe(owner, session, tick, -.5, 1, entity=spawned)
            previous = tick
        return results

    def run(self):
        with self.owner('author') as owner:
            self.author(owner)

        warm_session, restored_session, cold_session = (new_id() for _ in range(3))
        with self.owner('warm') as owner:
            self.assert_authored(owner, PROFILE, revision=2)
            self.configure(owner)
            started = owner.call('runtime.start', dict(session_id=warm_session, revision=2))
            assert started['tick'] == 0 and started['started'] is True, started
            inactive = dict(session_id=warm_session, tick=0, entity=PLAIN)
            self.refuse(owner, 'runtime.foliage.inspect', -32004, inactive)
            self.refuse(owner, 'runtime.foliage.inspect', -32030, inactive | dict(session_id=uid(999)))
            initial = self.observe(owner, warm_session, 0, -.5)
            self.advance(owner, warm_session, 0, 4, 0)
            self.observe(owner, warm_session, 4, 0)
            self.advance(owner, warm_session, 4, CHECKPOINT, 0)
            born = owner.call('runtime.structure.transact', dict(session_id=warm_session, request_id=new_id(),
                expected_tick=CHECKPOINT, expected_structure_revision=0,
                spawns=[dict(template_id=TEMPLATE, transform=transform((8, 3, 0)))]))
            assert born['structure_revision'] == 1 and len(born['spawned']) == 1, born
            spawned = born['spawned'][0]
            pinned = dict(session_id=warm_session, tick=CHECKPOINT, entity=PLANT)
            checkpoint = self.observe(owner, warm_session, CHECKPOINT, .5, 1)
            assert exact(owner.call('runtime.foliage.inspect', pinned)) == exact(checkpoint)
            self.refuse(owner, 'runtime.foliage.inspect', -32009, pinned | dict(structure_revision=0))
            self.refuse(owner, 'runtime.foliage.inspect', -32009, pinned | dict(tick=CHECKPOINT-1))
            self.refuse(owner, 'runtime.foliage.inspect', -32602, pinned | dict(structure_revision=True))

            self.write(owner, warm_session, 'checkpoint', CHECKPOINT)
            original = self.continuation(owner, warm_session, spawned)
            first_save = self.write(owner, warm_session, 'warm', FINAL)
            self.restore(owner, restored_session, active=warm_session)
            assert exact(without_session(self.observe(owner, restored_session, CHECKPOINT, .5, 1))) == exact(
                without_session(checkpoint))
            replay = self.continuation(owner, restored_session, spawned)
            second_save = self.write(owner, restored_session, 'replay', FINAL)
            assert first_save.read_bytes() == second_save.read_bytes()

        with self.owner('cold') as owner:
            assert owner.call('runtime.status')['active'] is False
            self.configure(owner)
            self.restore(owner, cold_session)
            assert exact(without_session(self.observe(owner, cold_session, CHECKPOINT, .5, 1))) == exact(
                without_session(checkpoint))
            fresh = self.continuation(owner, cold_session, spawned)
            third_save = self.write(owner, cold_session, 'cold', FINAL)
            assert first_save.read_bytes() == third_save.read_bytes()
            assert exact({key: without_session(value) for key, value in original.items()}) == exact(
                {key: without_session(value) for key, value in replay.items()}) == exact(
                {key: without_session(value) for key, value in fresh.items()})

            revised = PROFILE | dict(wind=WIND | dict(tip_m=1.5))
            assert self.transaction(owner, 2, [self.set_profile(revised)])['revision'] == 3
            self.assert_authored(owner, revised, revision=3)
            still = owner.call('runtime.foliage.inspect', dict(session_id=cold_session, tick=FINAL,
                                                              entity=PLANT, structure_revision=1))
            assert exact(still) == exact(fresh['tick-26'])

        self.evidence['continuation'] = dict(initial=initial, checkpoint=checkpoint, warm=original,
            same_owner_reload=replay, fresh_owner_reload=fresh, save_sha256=digest(first_save), spawned=spawned)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='foliage-motion-', dir=args.output.resolve()))
    evidence = dict(format='poima.foliage-motion-contract.v1', passed=False, binary_sha256=digest(args.binary),
                    fixture_sha256=digest(Path(__file__)), owners=[])
    try:
        Contract(args.binary, root, evidence).run()
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
