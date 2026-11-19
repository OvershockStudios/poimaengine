#!/usr/bin/env python3
"""Check guarded joint diagnostics against independently specified final poses."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import tempfile
import time

from animation_fixture import ribbon
from authored_rotation_clips import around, step
from gltf_fixture import glb
from runtime_ik_attachment import multiply_quaternions, save
from runtime_pose_frames import id_request, qz
from runtime_two_bone_ik_orientation import same_rotation
import world_physical_contract as native

METHOD = 'runtime.pose.joints'
NEUTRAL = qz(20)
FRAME = qz(90)  # X of the limit frame maps to Y of the rest node.
IDENTITY = [0, 0, 0, 1]


def y(degrees):
    return around([0, 1, 0], degrees)


def local_y(degrees):
    return multiply_quaternions(NEUTRAL, y(degrees))


def configure(session):
    doc, blob = ribbon()
    doc['nodes'][1].update(rotation=NEUTRAL, children=[3])
    doc['nodes'].append(dict(name='End', translation=[0, 1, 0]))
    source = session.directory / 'joint-pose.glb'
    source.write_bytes(glb(doc, blob))
    base = session.rpc('asset.import', dict(source=native.native(source)))['asset']

    def track(node, rotation):
        return dict(node=node, keys=[dict(time=0, rotation=rotation)])

    clips = [
        dict(name='Ready', tracks=[track(1, local_y(30)), track(2, qz(60)),
                                    track(3, [0, 1, 0, 0])]),
        dict(name='Curl', tracks=[track(1, local_y(25))]),
    ]
    asset = session.rpc('asset.animation.author', dict(asset=base, clips=clips))['asset']
    rig = dict(asset=asset, clip=1, time=0, speed=1, playing=False, loop=False,
        layers=[dict(slot=1, mode='additive', clip=2, time=0, speed=1,
                     playing=False, loop=False, weight=0, mask=[dict(node=1, weight=1)])],
        ik=[dict(slot=1, root=2, middle=1, end=3, target=[.3, 1.7, 0], pole=[0, 0, 1],
                 space='rig', enabled=False)])
    result = native.transact(session, [
        dict(op='asset.instantiate', id=native.RIG, name='Joint observation rig', asset=asset),
        native.component(native.RIG, 'AnimationRig', rig),
        native.component(native.node(2), 'Transform',
                         dict(position=[0, 0, 0], scale=[1, 1, 1], rotation=qz(10))),
    ], 0)
    return result['revision']


def verify(session, revision):
    owner, tick = id_request(), 0
    hinge = dict(kind='hinge', minimum=-.2, maximum=math.radians(45))
    joints = [
        dict(node=3, limits=hinge),
        dict(node=1, limits=hinge, limit_frame=FRAME),
        dict(node=2, limits=dict(kind='swing_twist', swing_y=.7, swing_z=math.radians(25),
                                 twist_min=-.3, twist_max=.3)),
    ]

    def request(rows=None, error=None, **overrides):
        params = dict(session_id=owner, tick=tick, structure_revision=0, rig=native.RIG,
                      joints=joints if rows is None else rows)
        return session.rpc(METHOD, {**params, **overrides}, error=error)

    def angles(row, *, twist, swing, correction):
        for value, expected in ((row['twist_radians'], twist), (row['swing_radians'], swing),
                                (row['fit']['correction_radians'], correction)):
            assert abs(value - math.radians(expected)) < 2e-6, row

    request(error=-32030)
    saves = native.save_root(session)
    session.rpc('runtime.start', dict(session_id=owner, revision=revision))
    request(error=-32030, session_id=id_request())
    request(error=-32009, tick=1)
    request(error=-32009, structure_revision=1)
    request(error=-32004, rig=id_request())
    request(error=-32602, rig=native.node(1))
    for rows in ([], joints * 22, [joints[1], joints[1]], [dict(joints[1], node=9999)],
                 [dict(joints[1], node=1.0)], [dict(joints[1], reference_rotation=[0, 0, 0, 0])],
                 [dict(joints[1], limit_frame=[True, 0, 0, 0])],
                 [dict(joints[1], limits=dict(kind='hinge', minimum=.1, maximum=.2))]):
        request(rows, error=-32602)

    first = request()
    assert first['correction_tolerance_radians'] == 1e-6
    assert first['status'] == 'outside_limits'
    assert first['counts'] == dict(within_limits=1, outside_limits=1, indeterminate=1)
    singular, admitted, swing = first['joints']
    assert singular['status'] == 'indeterminate' and singular['fit'] is None
    assert singular['twist_radians'] is None and singular['swing_radians'] is None
    assert abs(singular['rotation_radians'] - math.pi) < 1e-8
    assert admitted['reference_source'] == 'authored_local'
    assert admitted['entity'] == native.node(1) and admitted['parent'] == native.node(2)
    assert admitted['local_rotation'] == admitted['fit']['local_rotation']
    same_rotation(admitted['reference_rotation'], NEUTRAL)
    angles(admitted, twist=30, swing=0, correction=0)
    same_rotation(swing['reference_rotation'], qz(10))
    angles(swing, twist=0, swing=50, correction=25)
    assert swing['fit']['swing_clamped']
    same_rotation(swing['fit']['local_rotation'], qz(35))

    def layer(weight):
        return dict(entity=native.RIG, layer=1, clip=2, time=0, speed=1, loop=False,
                    playing=False, weight=weight)

    tick = step(session, owner, tick, 1, animations=[layer(.5)])
    blended = request([joints[1]])['joints'][0]
    angles(blended, twist=42.5, swing=0, correction=0)
    tick = step(session, owner, tick, 1, animations=[layer(1)])
    checkpoint = save(session, saves, 'before', owner, tick)
    over = request([joints[1]])['joints'][0]
    assert over['status'] == 'outside_limits' and over['fit']['twist_clamped']
    angles(over, twist=55, swing=0, correction=10)
    same_rotation(over['fit']['local_rotation'], local_y(45))
    shifted = request([dict(joints[1], reference_rotation=local_y(60))])['joints'][0]
    assert shifted['reference_source'] == 'explicit'
    angles(shifted, twist=-5, swing=0, correction=0)
    assert save(session, saves, 'after', owner, tick) == checkpoint

    # The position solver moves the parents. Independently compose the known
    # rig-space end orientation with their observed local rotations.
    base = dict(entity=native.RIG, clip=None, time=0, speed=1, loop=False, playing=False)
    tick = step(session, owner, tick, 1, animations=[base, layer(0)])
    end = dict(node=3, limits=hinge)
    angles(request([end])['joints'][0], twist=0, swing=0, correction=0)
    goal = qz(65)
    tick = step(session, owner, tick, 1, ik=[dict(entity=native.RIG, slot=1, space='rig',
        enabled=True, target=[.3, 1.7, 0], pole=[0, 0, 1], weight=1,
        end_orientation=dict(rotation=goal, weight=1))])
    measured = request([end])['joints'][0]
    assert measured['status'] == 'outside_limits' and measured['fit']['hinge_projected']
    rotations = [session.rpc('runtime.entity', dict(session_id=owner, tick=tick, id=native.node(node)))
                 ['local_transform']['rotation'] for node in (2, 1, 3)]
    same_rotation(measured['local_rotation'], rotations[2])
    parent = multiply_quaternions(rotations[0], rotations[1])
    same_rotation(multiply_quaternions(parent, measured['local_rotation']), goal)
    session.rpc('runtime.stop', dict(session_id=owner))
    return dict(composite_twist_degrees=math.degrees(over['twist_radians']),
                composite_fit_degrees=math.degrees(over['fit']['correction_radians']),
                ik_joint_outside=True, complete_save_bytes_equal_around_reads=True)


def run(args, output):
    output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic() + 150
    report = dict(passed=False, engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session = native.Session(output / 'session')
    try:
        info = session.rpc('world.describe', dict(view='method', name=METHOD))
        schema = info['methods'][METHOD]['properties']['joints']
        assert info['schema_revision'] >= 140 and schema['maxItems'] == 64
        revision = configure(session)
        authored = session.world.read_bytes()
        report.update(verify(session, revision))
        assert session.world.read_bytes() == authored
        session.close()
        report['passed'] = True
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
        (output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(f'Verified joint diagnostics in {report["rpc_count"]} native calls.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if args.output:
        run(args, args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='poima-joint-pose-', dir=args.binary.resolve().parent) as path:
            run(args, Path(path) / 'check')


if __name__ == '__main__':
    main()
