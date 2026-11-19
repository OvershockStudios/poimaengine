#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check committed pose capsules and distinguish endpoint overlaps from obstructed travel."""

import argparse
from contextlib import closing
import hashlib
import json
import math
from pathlib import Path
import tempfile
import time

from runtime_ik_attachment import save
from runtime_pose_frames import id_request, qz, transform, uid
import world_physical_contract as native

METHOD = 'runtime.pose.clearance'
CARRIER, WALL = uid(11), uid(12)


def endpoint(point):
    return dict(entity=CARRIER, point=point)


def segment(start, end, radius=.025):
    return dict(endpoints=[endpoint(start), endpoint(end)], radius=radius)


def verify(owner):
    method = owner.rpc('world.describe', dict(view='method', name=METHOD))['methods'][METHOD]
    assert set(method['required']) == {'session_id', 'tick', 'structure_revision', 'segments'}
    assert method['properties']['segments']['maxItems'] == 16
    ops = []
    for entity, name, pose, motion, extents in (
            (CARRIER, 'Pose carrier', transform((-.3, 2, 0), qz(90), (2, 1, 1)), 'kinematic', [.02, .02, .02]),
            (WALL, 'Thin obstacle', transform((0, 0, 0)), 'static', [.04, .3, .05])):
        ops.extend([dict(op='entity.create', id=entity, name=name),
                    native.component(entity, 'Transform', pose),
                    native.component(entity, 'BoxCollider', dict(half_extents=extents, motion=motion,
                        mass=1, friction=.5, restitution=0))])
    revision = native.transact(owner, ops, 0)['revision']
    authored = owner.world.read_bytes()
    saves = native.save_root(owner)
    session = id_request()
    owner.rpc('runtime.start', dict(session_id=session, revision=revision))

    # Rotation and nonuniform scale put the simulated hand below the carrier.
    hand = segment([-1.05, 0, 0], [-.95, 0, 0])
    raised = segment([-.75, 0, 0], [-.65, 0, 0])
    tick = 0

    def query(segments, error=None, **overrides):
        params = dict(session_id=session, tick=tick, structure_revision=0, segments=segments)
        return owner.rpc(METHOD, dict(params, **overrides), error)

    previous = query([hand, raised])
    assert previous['tick'] == 0 and previous['structure_revision'] == 0
    assert [row['status'] for row in previous['segments']] == ['endpoint_only', 'endpoint_only']
    from_hand, from_raised = [row['committed'] for row in previous['segments']]
    for actual, expected in zip(from_hand, [[-.3, -.1, 0], [-.3, .1, 0]]):
        assert math.dist(actual, expected) < 2e-7, (actual, expected)

    owner.rpc('runtime.step', dict(session_id=session, request_id=id_request(), expected_tick=0,
        expected_structure_revision=0, ticks=1, motions=[
            dict(entity=CARRIER, position=[.3, 2, 0], rotation=qz(90), duration_ticks=1)]))
    tick = 1
    before = save(owner, saves, 'before', session, tick)
    hand_path = dict(hand, previous=from_hand)
    raised_path = dict(raised, previous=from_raised)
    cube_center = [[0, -.1, 0], [0, .1, 0]]
    starting_inside = dict(hand, previous=cube_center)
    final_inside = segment([-1.05, .3, 0], [-.95, .3, 0])
    point_sphere = segment([-1.05, .3, 0], [-1.05, .3, 0])
    motion, high, old, new, sphere = query(
        [hand_path, raised_path, starting_inside, final_inside, point_sphere])['segments']
    assert motion['previous_hit'] is None and motion['committed_hit'] is None
    assert motion['status'] == 'blocked_sweep' and motion['sweep_blocker']['interval'] == 2, motion
    contact = motion['sweep_blocker']['hit']
    assert contact['entity'] == WALL and not contact['initial_overlap']
    # At the centre path's first contact, x = -(box half width + radius).
    fraction = (motion['sweep_blocker']['interval'] - 1 + contact['fraction']) / 4
    assert abs(fraction - ((.3 - .04 - .025) / .6)) < .003, (fraction, motion)
    for actual, expected in zip(motion['committed'], [[.3, -.1, 0], [.3, .1, 0]]):
        assert math.dist(actual, expected) < 2e-7, (actual, expected)

    assert high['status'] == 'clear' and high['sweep_blocker'] is None
    assert old['status'] == 'blocked_endpoint' and old['previous_hit']['entity'] == WALL
    assert old['committed_hit'] is None and old['sweep_blocker'] is None
    assert new['status'] == 'blocked_endpoint' and new['committed_hit']['entity'] == WALL
    assert new['previous_hit'] is None and new['sweep_blocker'] is None
    assert sphere['status'] == 'blocked_endpoint' and sphere['committed'][0] == sphere['committed'][1]
    assert all(row['committed_hit']['penetration_depth'] > .02 for row in (new, sphere))
    assert query([hand_path], ignore=[WALL])['segments'][0]['status'] == 'clear'
    assert query([dict(hand, previous=motion['committed'])])['segments'][0]['status'] == 'clear'
    assert len(query([hand_path] * 16)['segments']) == 16

    query([hand_path], -32009, tick=0)
    query([hand_path], -32009, structure_revision=1)
    query([hand_path], -32030, session_id=id_request())
    query([hand_path], -32004, ignore=[uid(666)])
    query([dict(hand, endpoints=[endpoint([0, 0, 0]), dict(entity=uid(666))])], -32004)
    query([hand_path], -32602, ignore=[WALL, WALL])
    query([hand_path] * 17, -32602)
    query([dict(hand, radius=False)], -32602)
    query([dict(hand, radius=.0009)], -32602)
    query([dict(hand, previous=[[], []])], -32602)
    query([dict(hand, previous=[[-1e6-1, 0, 0], [0, 0, 0]])], -32602)
    assert save(owner, saves, 'after', session, tick) == before
    assert owner.world.read_bytes() == authored
    owner.rpc('runtime.stop', dict(session_id=session))
    return dict(path_fraction=fraction, interval=motion['sweep_blocker']['interval'],
                offsets_at_committed_pose=motion['committed'], endpoint_penetration_m=new['committed_hit']['penetration_depth'],
                sphere_penetration_m=sphere['committed_hit']['penetration_depth'],
                all_statuses=[row['status'] for row in [previous['segments'][0], motion, high, old, new]],
                exact_archive_pair=True, rejected_requests=11)


def run(args, output):
    output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic() + 150
    report = dict(passed=False, engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    try:
        with closing(native.Session(output / 'session')) as owner:
            report.update(verify(owner))
        report['passed'] = True
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
        (output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(f'Verified pose clearance in {report["rpc_count"]} native calls.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if args.output:
        run(args, args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='poima-pose-clearance-', dir=args.binary.resolve().parent) as path:
            run(args, Path(path) / 'check')
