#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check authored height fog, reference integrity and frozen native lighting."""

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import RpcError, WorldClient, new_id


def uid(number):
    return f'{number:032x}'


ENVIRONMENT, SPOT, DISABLED, POINT, EMPTY = map(uid, range(1, 6))
IDENTITY = dict(position=[0, 0, 0], scale=[1, 1, 1], rotation=[0, 0, 0, 1])
BASE = dict(ambient=[.125, .25, .5], exposure=1)
FOG = dict(density_per_m=0, base_height_m=-1024, height_falloff_per_m=.5,
           start_distance_m=4, max_distance_m=128, ambient_radiance=[0, 2, 32], anisotropy=-.5)
SPOTS = [dict(entity=SPOT, scattering=1), dict(entity=DISABLED, scattering=.25)]
PROFILE = FOG | dict(spot_lights=SPOTS)


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def light(entity, enabled=True, kind='spot'):
    return component(entity, 'Light', dict(kind=kind, color=[1, .5, .25], intensity=8,
                                          range=16, enabled=enabled))


def edit(value):
    return component(ENVIRONMENT, 'LightingEnvironment', BASE | dict(height_fog=value))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Fixture:
    def __init__(self, binary, root, evidence):
        self.binary, self.root, self.evidence = binary.resolve(), root, evidence
        self.world = root / 'height-fog.world.json'

    @contextmanager
    def owner(self, label, world=None):
        trace = self.root / (label + '-trace')
        client = WorldClient.open(self.binary, world or self.world, trace_directory=trace, trace_limit_bytes=1024*1024)
        record = dict(label=label, pid=client.transport.process_id, trace=str(trace))
        self.evidence['owners'].append(record)
        try:
            with client:
                yield client
                assert client.call('session.close')['closed'] is True
        finally:
            record.update(exit=client.transport.returncode, closed=client.closed,
                          cleanup_error=str(client.cleanup_error) if client.cleanup_error else None)
        assert record['exit'] == 0 and record['closed'] and record['cleanup_error'] is None, record

    @staticmethod
    def transaction(owner, revision, *operations, reject=False):
        params = dict(request_id=new_id(), base_revision=revision, ops=list(operations))
        try:
            result = owner.call('world.transact', params)
        except RpcError as failure:
            assert reject and failure.code == -32602, (params, failure)
            return failure.message
        assert not reject and result['committed'] is True and result['revision'] == revision + 1, (params, result)
        return result

    def author(self, owner):
        self.transaction(owner, 0, *[
            dict(op='entity.create', id=entity, name=name)
            for entity, name in ((ENVIRONMENT, 'Height fog'), (SPOT, 'Active spotlight'),
                                 (DISABLED, 'Inactive spotlight'), (POINT, 'Point control'), (EMPTY, 'No light'))
        ], component(ENVIRONMENT, 'LightingEnvironment', BASE), light(SPOT), light(DISABLED, enabled=False),
            light(POINT, kind='point'))
        discovery = owner.call('world.describe', dict(view='component', name='LightingEnvironment'))
        schema = discovery['components']['LightingEnvironment']['properties']['height_fog']
        assert discovery['schema_revision'] == 197, discovery
        assert set(schema['required']) == set(FOG) and schema['additionalProperties'] is False
        lights = schema['properties']['spot_lights']
        assert lights['maxItems'] == 2 and set(lights['items']['required']) == {'entity', 'scattering'}
        assert 'height_fog' not in owner.call('world.lighting')
        assert 'needs_height_fog' not in owner.call('world.dependencies')

        before, history = self.world.read_bytes(), owner.call('world.history')
        invalid = [None, {}, [], PROFILE | dict(unknown=True)]
        for field in FOG:
            invalid.append({key: value for key, value in PROFILE.items() if key != field})
        bounds = dict(density_per_m=(0, .75), base_height_m=(-8192, 8192),
                      height_falloff_per_m=(0, 4), start_distance_m=(0, 300),
                      max_distance_m=(.1, 512), anisotropy=(-.8, .8))
        for field, (minimum, maximum) in bounds.items():
            for value in (minimum - 1e-8, maximum + 1e-8, False, '1', None):
                invalid.append(PROFILE | {field: value})
        invalid.extend([
            PROFILE | dict(max_distance_m=4), PROFILE | dict(max_distance_m=3.9),
            PROFILE | dict(ambient_radiance=[0, 1]), PROFILE | dict(ambient_radiance=[0, 1, 2, 3]),
            PROFILE | dict(ambient_radiance=[0, True, 2]), PROFILE | dict(ambient_radiance=[-.01, 1, 2]),
            PROFILE | dict(ambient_radiance=[0, 1, 32.00000001]), PROFILE | dict(ambient_radiance=None),
        ])
        for value in (None, {}, [None], [{}, {}], SPOTS + [SPOTS[0]],
                      [dict(entity=SPOT)], [dict(scattering=1)],
                      [dict(entity=SPOT, scattering=1, extra=0)],
                      [dict(entity=SPOT, scattering=1), dict(entity=SPOT, scattering=.25)]):
            invalid.append(PROFILE | dict(spot_lights=value))
        for entity in (POINT, EMPTY, 'f'*32, 'A'*32, True):
            invalid.append(PROFILE | dict(spot_lights=[dict(entity=entity, scattering=.5)]))
        for scattering in (-.001, 1.000000001, True, '1', None):
            invalid.append(PROFILE | dict(spot_lights=[dict(entity=SPOT, scattering=scattering)]))
        for value in invalid:
            self.transaction(owner, 1, component(EMPTY, 'Transform', IDENTITY | dict(position=[24, 0, 0])),
                             edit(value), reject=True)
            assert self.world.read_bytes() == before
        assert owner.call('world.history') == history

        self.transaction(owner, 1, edit(PROFILE))
        observed = owner.call('world.lighting', dict(revision=2))
        assert observed['height_fog'] == PROFILE and DISABLED not in {entry['id'] for entry in observed['lights']}
        assert owner.call('world.dependencies')['needs_height_fog'] is True
        self.evidence['authoring'] = dict(rejections=len(invalid), selected=observed['height_fog'])

    def frozen(self, owner):
        raw = owner.call('entity.get', dict(id=ENVIRONMENT, revision=2))
        assert raw['value']['components']['LightingEnvironment']['height_fog'] == PROFILE
        session = new_id()
        started = owner.call('runtime.start', dict(session_id=session, revision=2))
        assert started['tick'] == 0 and started['authored_revision'] == 2
        profile = dict(session_id=session, tick=0)
        frozen = owner.call('runtime.lighting', profile)
        assert frozen['height_fog'] == PROFILE
        before, history = self.world.read_bytes(), owner.call('world.history')
        for operation in (dict(op='entity.delete', id=DISABLED, recursive=True),
                          dict(op='component.remove', id=DISABLED, type='Light'),
                          light(DISABLED, enabled=False, kind='point')):
            assert 'Height fog spot light' in self.transaction(owner, 2, operation, reject=True)
            assert self.world.read_bytes() == before and owner.call('world.history') == history
            assert owner.call('runtime.lighting', profile) == frozen

        active = FOG | dict(density_per_m=.5, spot_lights=[SPOTS[0]])
        self.transaction(owner, 2, dict(op='entity.delete', id=DISABLED, recursive=True), edit(active))
        published = owner.call('world.lighting', dict(revision=3))
        assert published['height_fog'] == active
        assert owner.call('runtime.lighting', profile) == frozen
        self.evidence['frozen'] = dict(runtime=frozen['height_fog'], authored=published['height_fog'])

    def template_references(self):
        template, local_environment, local_spot = map(uid, (200, 201, 202))

        def recipe(target, kind='spot'):
            environmental = BASE | dict(height_fog=FOG | dict(spot_lights=[dict(entity=target, scattering=.5)]))
            return dict(op='template.set', id=template, name='Local fog sources', root=local_environment, entities={
                local_environment: dict(name='Fog owner', parent=None,
                    components=dict(Transform=IDENTITY, LightingEnvironment=environmental)),
                local_spot: dict(name='Local spotlight', parent=local_environment, components={
                    'Transform': IDENTITY, 'Light': light(local_spot, enabled=False, kind=kind)['value']}),
            })

        world = self.root / 'fog-template.world.json'
        with self.owner('template', world=world) as owner:
            self.transaction(owner, 0, dict(op='entity.create', id=SPOT, name='External spotlight'), light(SPOT))
            before = world.read_bytes()
            for candidate in (recipe(SPOT), recipe(local_spot, kind='point')):
                self.transaction(owner, 1, candidate, reject=True)
                assert world.read_bytes() == before
            accepted = recipe(local_spot)
            self.transaction(owner, 1, accepted)
            session = new_id()
            owner.call('runtime.start', dict(session_id=session, revision=2))
            assert 'height_fog' not in owner.call('runtime.lighting', dict(session_id=session, tick=0))
            spawned = owner.call('runtime.structure.transact', dict(session_id=session, request_id=new_id(),
                expected_tick=0, expected_structure_revision=0, spawns=[dict(template_id=template)]))
            root, = spawned['spawned']
            members = owner.call('runtime.instance', dict(session_id=session, id=root, tick=0,
                                                          expected_structure_revision=1))['nodes']
            fog = owner.call('runtime.lighting', dict(session_id=session, tick=0))['height_fog']
            assert members[local_spot] != local_spot and fog['spot_lights'] == [
                dict(entity=members[local_spot], scattering=.5)], (fog, members)
            authored = owner.call('template.get', dict(id=template, revision=2))['template']
            assert authored['entities'] == accepted['entities']
            self.evidence['template'] = dict(local_spot=local_spot, runtime_spot=members[local_spot], height_fog=fog)

    def run(self):
        with self.owner('author') as owner:
            self.author(owner)
        with self.owner('runtime') as owner:
            self.frozen(owner)
        with self.owner('reopen') as owner:
            source_only = FOG | dict(density_per_m=.5)
            self.transaction(owner, 3, edit(source_only))
            result = owner.call('world.lighting', dict(revision=4))
            assert result['height_fog'] == source_only
            empty_list = source_only | dict(spot_lights=[])
            self.transaction(owner, 4, edit(empty_list))
            assert owner.call('world.lighting', dict(revision=5))['height_fog'] == source_only
            authored = owner.call('entity.get', dict(id=ENVIRONMENT, revision=5))['value']
            assert authored['components']['LightingEnvironment']['height_fog'] == empty_list
            self.transaction(owner, 5, component(ENVIRONMENT, 'LightingEnvironment', BASE))
            assert 'height_fog' not in owner.call('world.lighting', dict(revision=6))
            assert 'needs_height_fog' not in owner.call('world.dependencies')
        self.template_references()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='height-fog-contract-', dir=args.output.resolve()))
    evidence = dict(format='poima.height-fog-contract.v1', passed=False, binary_sha256=digest(args.binary),
                    fixture_sha256=digest(Path(__file__)), owners=[])
    try:
        Fixture(args.binary, root, evidence).run()
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
