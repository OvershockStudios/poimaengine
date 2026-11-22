#!/usr/bin/env python3
"""Exercise authored hinge and quaternion clips on original generated rig data."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import time

from animation_fixture import ribbon
from gltf_fixture import glb
from runtime_ik_attachment import multiply_quaternions, save
from runtime_pose_frames import id_request, qz
from runtime_two_bone_ik_orientation import near, same_rotation
import world_physical_contract as native


def around(axis, degrees):
    radians = math.radians(degrees) / 2
    return [element * math.sin(radians) for element in axis] + [math.cos(radians)]


def fixture_source(directory):
    doc, blob = ribbon()
    doc['nodes'][1]['rotation'] = qz(20)
    source = directory / 'original-ribbon.glb'
    source.write_bytes(glb(doc, blob))
    return source


def authored_clips():
    curl = dict(
        name='Curl and release',
        tracks=[dict(
            node=1,
            hinge=dict(axis=[1, 0, 0], minimum_degrees=0, maximum_degrees=60),
            keys=[dict(time=0, degrees=0), dict(time=.5, degrees=60), dict(time=1, degrees=0)],
        )],
    )
    offset = dict(
        name='Local pose changes',
        tracks=[
            dict(node=2, interpolation='step', keys=[
                dict(time=0, rotation=[0, 0, 0, 1]), dict(time=.5, rotation=qz(45)),
            ]),
            dict(node=1, keys=[dict(time=0, rotation=qz(-10))]),
        ],
    )
    held = dict(name='Static grip', tracks=[dict(
        node=1,
        hinge=dict(axis=[1, 0, 0], minimum_degrees=0, maximum_degrees=60),
        keys=[dict(time=0, degrees=30)],
    )])
    return [curl, offset, held]


def inventory(directory):
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in directory.iterdir() if path.is_file()}


def configure(session):
    source = fixture_source(session.directory)
    base = session.rpc('asset.import', dict(source=native.native(source)))
    package_folder = session.world.with_name(session.world.name + '.assets')
    original_packages = inventory(package_folder)
    discovery = session.rpc('world.describe', dict(view='method', name='asset.animation.author'))
    schema = discovery['methods']['asset.animation.author']
    assert discovery['schema_revision'] >= 138
    assert schema['properties']['clips']['maxItems'] == 8
    variants = schema['properties']['clips']['items']['properties']['tracks']['items']['oneOf']
    assert len(variants) == (3 if discovery['schema_revision'] >= 200 else 2)
    assert variants[0]['properties']['keys']['items']['required'] == ['time', 'rotation']
    assert 'hinge' in variants[1]['required']
    assert variants[1]['properties']['keys']['items']['required'] == ['time', 'degrees']
    status_before = session.rpc('world.history')

    def attempt(clips, error=None, asset=None):
        return session.rpc('asset.animation.author', dict(asset=asset or base['asset'], clips=clips), error=error)

    def reject(clip, patch):
        clips = copy.deepcopy(authored_clips())
        patch(clips[clip])
        attempt(clips, -32602)
        assert inventory(package_folder) == original_packages
        assert session.rpc('world.history') == status_before

    reject(0, lambda v: v['tracks'][0]['keys'][1].update(degrees=61))
    reject(0, lambda v: v['tracks'][0]['hinge'].update(axis=[0, 0, 0]))
    reject(0, lambda v: v['tracks'][0]['hinge'].update(maximum_degrees=180))
    reject(0, lambda v: v['tracks'][0]['keys'][2].update(time=.5000000001))
    reject(0, lambda v: v['tracks'][0].update(node=1.0))
    reject(1, lambda v: v['tracks'].append(v['tracks'][0]))
    reject(1, lambda v: v['tracks'][0]['keys'][0].update(rotation=[0, 0, 0, 0]))
    reject(1, lambda v: v['tracks'][0]['keys'][0].update(degrees=0))
    reject(1, lambda v: v['tracks'][0]['keys'][0].update(rotation=[False, 0, 0, 1]))
    reject(1, lambda v: v['tracks'][0].update(interpolation='cubic'))
    reject(1, lambda v: v.update(name='Bend'))
    reject(1, lambda v: v.update(name='Curl and release'))
    attempt(authored_clips(), -32050, 'f' * 64)

    result = attempt(authored_clips())
    assert result['base_asset'] == base['asset'] and result['asset'] != base['asset']
    assert result['clips'] == [
        dict(index=1, name='Curl and release', duration=1., tracks=1),
        dict(index=2, name='Local pose changes', duration=.5, tracks=2),
        dict(index=3, name='Static grip', duration=0., tracks=1),
    ]
    reordered = authored_clips()
    reordered[1]['tracks'].reverse()
    reordered[1]['tracks'][0]['keys'][0]['rotation'] = [-v for v in reordered[1]['tracks'][0]['keys'][0]['rotation']]
    assert attempt(reordered) == result
    assert session.rpc('world.history') == status_before
    expected_packages = {**original_packages, result['asset'] + '.pmodel': result['asset']}
    assert inventory(package_folder) == expected_packages
    for section in ('nodes', 'skins', 'primitives'):
        left = session.rpc('asset.inspect', dict(asset=base['asset'], section=section))
        right = session.rpc('asset.inspect', dict(asset=result['asset'], section=section))
        assert left['items'] == right['items']
    left = session.rpc('asset.animation.channel', dict(asset=base['asset'], clip=0, channel=0))
    right = session.rpc('asset.animation.channel', dict(asset=result['asset'], clip=0, channel=0))
    assert {**left, 'asset': result['asset']} == right

    def sample(clip, moment, node):
        row = session.rpc('asset.animation.sample', dict(asset=result['asset'], clip=clip, time=moment,
            offset=node, limit=1))['items'][0]
        assert row['index'] == node
        return row

    for moment, degrees in ((0, 0), (.25, 30), (.5, 60), (.75, 30), (1, 0)):
        pose = sample(1, moment, 1)
        expected = multiply_quaternions(qz(20), around([1, 0, 0], degrees))
        same_rotation(pose['rotation'], expected)
        assert pose['position'] == [0, 1, 0] and pose['scale'] == [1, 1, 1]
    same_rotation(sample(2, .25, 2)['rotation'], [0, 0, 0, 1])
    same_rotation(sample(2, .5, 2)['rotation'], qz(45))
    same_rotation(sample(2, 0, 1)['rotation'], qz(-10))
    same_rotation(sample(3, 200, 1)['rotation'],
                  multiply_quaternions(qz(20), around([1, 0, 0], 30)))
    snap = session.rpc('asset.animation.channel', dict(asset=result['asset'], clip=2, channel=1))
    assert snap['node'] == 2 and snap['path'] == 'rotation' and snap['interpolation'] == 'STEP'

    revision = native.transact(session, [
        dict(op='asset.instantiate', id=native.RIG, name='Authored rotation check', asset=result['asset']),
    ], 0)['revision']
    rig = dict(asset=result['asset'], clip=0, time=0, speed=1, playing=True, loop=False, layers=[
        dict(slot=1, mode='additive', clip=1, time=0, speed=1, playing=True, loop=False,
             weight=.5, mask=[dict(node=1, weight=1)]),
    ])
    revision = native.transact(session, [native.component(native.RIG, 'AnimationRig', rig)], revision)['revision']
    retained_source = source.with_name('retained-original-ribbon.glb')
    source.rename(retained_source)
    assert not source.exists()
    return revision, result


def state_at(session, owner, tick, position, angle):
    row = session.rpc('runtime.entity', dict(session_id=owner, tick=tick, id=native.node(1), structure_revision=0))
    transform = row['local_transform']
    near(transform['position'], position, 4e-6)
    assert transform['scale'] == [1, 1, 1]
    same_rotation(transform['rotation'], multiply_quaternions(qz(20), around([1, 0, 0], angle)))
    return row


def step(session, owner, tick, count, **options):
    result = session.rpc('runtime.step', dict(request_id=id_request(), session_id=owner,
        expected_tick=tick, ticks=count, **options))
    assert result['tick'] == tick + count
    return result['tick']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic() + 180
    report = dict(passed=False, executable_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session = native.Session(args.output / 'source')
    try:
        revision, report['derived_model'] = configure(session)
        authored = session.world.read_bytes()
        saves = native.save_root(session)
        owner = id_request()
        session.rpc('runtime.start', dict(session_id=owner, revision=revision))
        tick = step(session, owner, 0, 15)
        state_at(session, owner, tick, [.25, 1, 0], 15)
        tick = step(session, owner, tick, 15, animations=[
            dict(entity=native.RIG, layer=1, clip=1, time=.25, speed=1, loop=False, playing=True, weight=1),
        ])
        state_at(session, owner, tick, [.5, 1, 0], 60)
        tick = step(session, owner, tick, 10, animations=[
            dict(entity=native.RIG, layer=1, clip=1, time=.5, speed=1, loop=False, playing=True,
                 weight=0, weight_blend_ticks=30),
        ])
        state_at(session, owner, tick, [2/3, 1, 0], (60-20) * (2/3))
        checkpoint = save(session, saves, 'checkpoint', owner, tick)
        end = step(session, owner, tick, 15)
        state_at(session, owner, end, [11/12, 1, 0], 10/6)
        final = save(session, saves, 'expected', owner, end)
        assert session.world.read_bytes() == authored
        session.rpc('runtime.stop', dict(session_id=owner))
        session.close()
        session = native.Session(args.output / 'cold', world=session.world)
        session.rpc('save.configure', dict(request_id=id_request(), expected_generation=0, root=native.native(saves)))
        owner = id_request()
        restored = session.rpc('save.load', dict(request_id=id_request(), configuration_generation=1, slot='checkpoint',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, expected_structure_revision=None, new_session_id=owner))
        assert restored['tick'] == tick
        assert save(session, saves, 'restored', owner, tick) == checkpoint
        tick = step(session, owner, tick, 5)
        end = step(session, owner, tick, 10)
        assert save(session, saves, 'continued', owner, end) == final
        assert session.world.read_bytes() == authored
        session.rpc('runtime.stop', dict(session_id=owner))
        session.close()
        report.update(passed=True, exact_cold_save=True, exact_partitioned_continuation=True,
                      runtime_pose_samples=4, rejected_requests=13)
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
        (args.output/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(f"Verified authored rotation clips and exact cold continuation in {report['rpc_count']} native calls.")


if __name__ == '__main__':
    main()
