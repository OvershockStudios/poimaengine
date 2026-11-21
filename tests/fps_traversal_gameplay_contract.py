#!/usr/bin/env python3
"""Run a published FPS traversal fixture through the native runtime."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import uuid

from collection_gameplay_contract import Session, sha


def uid(value):
    return f'{value:032x}'


def box(name, position, half_extents, motion='static'):
    return dict(name=name, parent=None, components={
        'Transform': dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]),
        'BoxCollider': dict(half_extents=half_extents, motion=motion, mass=10, friction=.5, restitution=0),
    })


def world(path, geometry):
    items = {
        uid(1): box('Floor', [0, -.5, 0], [4, .5, 4]),
        uid(2): box('Traversal actor', [0, .825, 2], [.15, .45, .15], 'kinematic'),
    }
    if geometry == 'vault':
        items[uid(3)] = box('Rail', [0, .275, 1.1], [.8, .275, .1])
    elif geometry != 'empty':
        width = .08 if geometry == 'narrow' else .8
        motion = 'kinematic' if geometry == 'dynamic' else 'static'
        items[uid(3)] = box('Ledge', [0, .4, .7], [width, .4, .5], motion)
    if geometry == 'ceiling':
        items[uid(4)] = box('Ceiling', [0, 2.04, 1.7], [.5, .08, .5])
    elif geometry == 'interrupted':
        items[uid(4)] = box('Moving blocker', [2, 2.04, 1.7], [.3, .08, .5], 'kinematic')
    path.write_text(json.dumps(dict(format='poima.authored-world', version=1, world_id=uid(50),
                                    revision=1, entities=items, receipts=[], retired_ids=[])) + '\n')


def position(owner):
    result = owner.rpc('runtime.entity', dict(session_id=owner.session, tick=owner.tick, id=uid(2)))
    return result['world_matrix'][12:15]


def values(owner):
    return owner.game()['module']['values']


def target_is_near(actual, expected):
    assert len(actual) == len(expected) == 3, actual
    assert all(abs(a - b) <= 1e-4 for a, b in zip(actual, expected)), (actual, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'descriptor', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run without -O')
    output = args.output.resolve()
    output.mkdir(parents=True)
    binary = args.binary.resolve(strict=True)
    descriptor = args.descriptor.resolve(strict=True)
    spec = json.loads(descriptor.read_bytes())
    assert spec['type'] == 'Poima.TraversalFixture.Game' and 'capsule_cast_v1' in spec['required_features']
    config = dict(descriptor=str(descriptor), expected_descriptor_sha256=sha(descriptor))
    evidence = dict(passed=False, calls=[], scenarios={}, descriptor_sha256=sha(descriptor), binary_sha256=sha(binary))
    files = {}
    owners = []

    def start(case, kind=0):
        path = output / (case + '.json')
        world(path, case)
        files[path] = sha(path)
        owner = Session(binary, path, evidence)
        owners.append(owner)
        owner.start(1, dict(config, values={'Kind': kind}))
        return owner

    try:
        mantle = start('mantle')
        store = output / 'save'
        store.mkdir()
        mantle.configure(store)
        mantle.step(13)
        before = values(mantle)
        assert before['Status'] == 0 and before['Elapsed'] == 13, before
        at_save = position(mantle)
        mantle.write('traversal', 0)
        expected_sequence = []
        for _ in range(19):
            mantle.step(1)
            expected_sequence.append(dict(position=position(mantle), values=values(mantle)))
        target_is_near(expected_sequence[-1]['position'], [0, 1.625, .85])
        assert expected_sequence[-1]['values']['Status'] == 1
        mantle.close()

        restored = Session(binary, output / 'mantle.json', evidence)
        owners.append(restored)
        restored.configure(store)
        restored.session = uuid.uuid4().hex
        result = restored.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='traversal',
            expected_generation=1, revision=1, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=restored.session, gameplay=config))
        restored.tick = result['tick']
        assert restored.tick == 13 and values(restored) == before and position(restored) == at_save
        for expected in expected_sequence:
            restored.step(1)
            assert dict(position=position(restored), values=values(restored)) == expected
        restored.close()
        evidence['scenarios']['mantle'] = dict(ticks=32, cold_restored_tick=13, compared_ticks=19,
                                              final=expected_sequence[-1])

        vault = start('vault', 1)
        vault.step(32)
        result = values(vault)
        assert result['Status'] == 1 and result['Elapsed'] == 32 and result['Support'] == uid(1), result
        target_is_near(position(vault), [0, .825, .3])
        evidence['scenarios']['vault'] = dict(position=position(vault), values=result)
        vault.close()

        for case, failure, blocker in (('empty', 1, uid(0)), ('narrow', 4, uid(3)),
                                        ('dynamic', 1, uid(3)), ('ceiling', 6, uid(4))):
            owner = start(case)
            owner.step(3)
            result = values(owner)
            assert result['Status'] == -1 and result['Elapsed'] == 0 and result['Failure'] == failure, (case, result)
            assert result['Colliding'] == blocker, (case, result)
            target_is_near(position(owner), [0, .825, 2])
            evidence['scenarios'][case] = result
            owner.close()

        interrupted = start('interrupted')
        interrupted.step(5)
        status = interrupted.inspect()
        result = interrupted.rpc('runtime.step', dict(session_id=interrupted.session, request_id=uuid.uuid4().hex,
            expected_tick=interrupted.tick, expected_structure_revision=status['structure_revision'], ticks=3,
            motions=[dict(entity=uid(4), position=[0, 2.04, 1.7], rotation=[0, 0, 0, 1], duration_ticks=3)]))
        interrupted.tick = result['current_tick']
        interrupted.step(24)
        result = values(interrupted)
        assert result['Status'] == -1 and 5 < result['Elapsed'] < 32, result
        assert result['Failure'] == 6 and result['Colliding'] == uid(4), result
        assert position(interrupted)[2] > 1.2
        evidence['scenarios']['interrupted'] = dict(position=position(interrupted), values=result)
        interrupted.close()

        assert all(sha(path) == expected for path, expected in files.items())
        evidence['passed'] = True
    finally:
        try:
            for owner in owners:
                owner.close()
        finally:
            (output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')

    print('Native mantle, vault, four refusals, moving blocker and fresh-owner continuation passed.')


if __name__ == '__main__':
    main()
