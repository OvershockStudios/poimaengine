#!/usr/bin/env python3
"""Verify a frozen grip attachment on the unchanged KayKit Knight source rig."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import time

from character_depth_oracle import inverse
from character_library_intake import LibraryOracle, near, point
from runtime_pose_frames import frame, id_request, pose_camera, uid
from runtime_two_bone_ik_orientation import same_rotation
import world_physical_contract as native

ARM_ROOT, ELBOW, WRIST, SWORD = 9, 8, 7, 11
IDLE, CHOP = 13, 8


def multiply_quaternions(a, b):
    x, y, z, w = a
    i, j, k, l = b
    return [w*i+x*l+y*k-z*j, w*j-x*k+y*l+z*i, w*k+x*j-y*i+z*l, w*l-x*i-y*j-z*k]


def logical_rotation(oracle, local, node):
    local_rotation = local[node]['rotation']
    if node not in oracle.parents:
        return local_rotation
    return multiply_quaternions(logical_rotation(oracle, local, oracle.parents[node]), local_rotation)


def reference_grip(oracle):
    local, _, world = oracle.pose(IDLE, .1)
    target = point(inverse(world[SWORD]), world[WRIST][12:15])
    pole = point(inverse(world[SWORD]), world[ELBOW][12:15])
    sword_rotation = logical_rotation(oracle, local, SWORD)
    inverse_sword = [-x for x in sword_rotation[:3]] + sword_rotation[3:]
    orientation = multiply_quaternions(inverse_sword, logical_rotation(oracle, local, WRIST))
    length = math.hypot(*orientation)
    return dict(space='rig', target=target, pole=pole, end_orientation=dict(
        rotation=[x/length for x in orientation], weight=1))


def configure(session, source, oracle, gpu):
    asset = session.rpc('asset.import', dict(source=native.native(source)))['asset']
    revision = native.transact(session, [
        dict(op='asset.instantiate', id=native.RIG, name='Knight', asset=asset),
    ], 0)['revision']
    doc = json.loads(session.world.read_bytes())
    rig = doc['entities'][native.RIG]['components']['AnimationRig']
    rig.update(clip=IDLE, time=.1, playing=False, loop=False)
    chain = dict(slot=1, root=ARM_ROOT, middle=ELBOW, end=WRIST, target_node=SWORD,
                 metric='near-uniform-v1', **reference_grip(oracle))
    rig['ik'] = [chain]
    ops = [native.component(native.RIG, 'AnimationRig', rig)]
    if gpu is not None:
        for index, eye in ((31, (2.5, 1.55, 2.05)), (32, (1.55, 3.15, .15))):
            identity = uid(index)
            ops.extend((dict(op='entity.create', id=identity, name='Grip review camera'),
                        native.component(identity, 'Camera', dict(vertical_fov=39, near=.01, far=30)),
                        native.component(identity, 'Transform', pose_camera(eye, (.15, .95, 0)))))
        # Keep the broadsword and body; the library scene includes every item.
        optional_props = {native.node(i) for i in (*range(5), 10)}
        hidden_meshes = [identity for identity, entity in doc['entities'].items()
                         if entity['parent'] in optional_props and 'StaticMesh' in entity['components']]
        assert len(hidden_meshes) == len(optional_props)
        ops.extend(dict(op='entity.delete', id=identity, recursive=False) for identity in hidden_meshes)
    revision = native.transact(session, ops, revision)['revision']
    original = session.world.read_bytes()
    for bad in (WRIST, 9999, True, float(SWORD)):
        changed = copy.deepcopy(rig)
        changed['ik'][0]['target_node'] = bad
        native.transact(session, [native.component(native.RIG, 'AnimationRig', changed)], revision, -32602)
        assert session.world.read_bytes() == original
    for bad_space in ('world', None):
        changed = copy.deepcopy(rig)
        if bad_space is None:
            del changed['ik'][0]['space']
        else:
            changed['ik'][0]['space'] = bad_space
        native.transact(session, [native.component(native.RIG, 'AnimationRig', changed)], revision, -32602)
    assert session.world.read_bytes() == original
    return revision, chain


def save(session, saves, slot, owner, tick):
    receipt = session.rpc('save.write', dict(request_id=id_request(), configuration_generation=1,
        slot=slot, expected_generation=0, session_id=owner, expected_tick=tick, expected_gameplay_revision=0))
    folder = saves / ('slot-' + slot)
    current = json.loads((folder/'current.json').read_bytes())['payload']['current']
    assert re.fullmatch(r'p-[0-9a-f]{32}\.bin', current['file'])
    raw = (folder/current['file']).read_bytes()
    assert len(raw) == receipt['bytes'] == current['bytes']
    assert hashlib.sha256(raw).hexdigest() == receipt['sha256'] == current['sha256']
    return raw


def traverse(session, owner, oracle, chain, gpu):
    tick = 0
    controls = {key: chain[key] for key in ('slot', 'target', 'pole', 'space', 'end_orientation')}
    controls['entity'] = native.RIG

    def step(count=1, error=None, **params):
        nonlocal tick
        result = session.rpc('runtime.step', dict(request_id=id_request(), session_id=owner,
            expected_tick=tick, ticks=count, **params), error)
        if error is None:
            tick = result['tick']
        return result

    def observe(include=True):
        return session.rpc('runtime.ik', dict(session_id=owner, tick=tick, entity=native.RIG,
            slot=1, include_end_orientation=True, include_target_node=include))['state']

    def measure(offset):
        return session.rpc('runtime.pose.compare', dict(session_id=owner, tick=tick, structure_revision=0,
            pairs=[dict(source=frame(native.node(WRIST)), target=frame(native.node(SWORD), offset, [0,0,0,1]))]))['pairs'][0]

    def capture(stage):
        files = []
        for camera, label in ((31, 'front'), (32, 'overhead')):
            path = session.directory / f'{stage}-{label}-{tick}.bmp'
            result = session.rpc('runtime.capture', dict(session_id=owner, tick=tick, camera=uid(camera),
                path=native.native(path), width=960, height=540, samples=4, gpu=gpu))
            assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0
            assert result['tick'] == tick
            files.append(dict(file=path.name, tick=tick, sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        return files

    evidence = []
    for clip, phase in ((IDLE, .1), (IDLE, .55), (CHOP, .15), (CHOP, .53), (CHOP, 1)):
        local, _, world = oracle.pose(clip, phase)
        animation = dict(entity=native.RIG, clip=clip, time=phase, speed=1, playing=False, loop=False)
        should_capture = gpu is not None and clip == CHOP and phase in (.15, .53)
        step(animations=[animation], ik=[dict(controls, enabled=False)])
        before = measure(chain['target'])
        baseline_captures = capture('baseline') if should_capture else []
        goal = point(world[SWORD], chain['target'])
        near(before['source']['world_matrix'][12:15], world[WRIST][12:15], 4e-6, 'Disabled original wrist')
        assert math.isclose(before['position_error_m'], math.dist(world[WRIST][12:15], goal), abs_tol=4e-6)
        step(ik=[controls])
        observed = observe()
        after = measure(chain['target'])
        expected_rotation = multiply_quaternions(logical_rotation(oracle, local, SWORD), chain['end_orientation']['rotation'])
        same_rotation(observed['end_orientation']['actual_rotation'], expected_rotation)
        assert observed['end_orientation']['angular_error'] < 1e-8
        assert observed['target_node'] == SWORD and observed['target'] == chain['target'] and observed['pole'] == chain['pole']
        near(after['target']['world_matrix'], before['target']['world_matrix'], 1e-14, 'Solving changes the independent sword branch')
        near(after['source']['world_matrix'][12:15], observed['end_position'], 2e-10, 'Published wrist differs from cached solve')
        near(after['target']['world_matrix'][12:15], goal, 4e-6, 'Attachment-local point composed with original sword')
        assert math.isclose(after['position_error_m'], observed['target_error'], abs_tol=2e-10)
        root, elbow, wrist = (world[index][12:15] for index in (ARM_ROOT, ELBOW, WRIST))
        reach = math.dist(root, elbow) + math.dist(elbow, wrist)
        excess = max(0, math.dist(goal, root)-reach)
        assert observed['clamped'] == (excess > 0)
        assert math.isclose(observed['target_error'], excess, abs_tol=4e-6)
        row = dict(clip=oracle.clips[clip]['name'], seconds=phase, tick=tick, baseline_m=before['position_error_m'],
                   corrected_m=after['position_error_m'], angular_error_rad=observed['end_orientation']['angular_error'],
                   clamped=observed['clamped'], baseline_captures=baseline_captures,
                   corrected_captures=capture('corrected') if should_capture else [])
        evidence.append(row)

    assert evidence[3]['baseline_m'] > .15 and evidence[3]['corrected_m'] < 4e-6
    assert evidence[4]['clamped'] and evidence[4]['corrected_m'] > .1
    current = observe()
    assert 'target_node' not in observe(include=False)
    assert session.rpc('runtime.ik', dict(session_id=owner, tick=tick, entity=native.RIG, slot=1,
        include_target_node=True))['state']['target_node'] == SWORD
    session.rpc('runtime.ik', dict(session_id=owner, tick=tick, entity=native.RIG, slot=1,
        include_target_node=1), -32602)
    step(3, error=-32040, ik=[dict(controls, space='world')])
    assert observe() == current

    adjusted = dict(controls, target=[x + delta for x,delta in zip(chain['target'], (.01,.015,-.005))])
    step(animations=[dict(entity=native.RIG, clip=CHOP, time=.2, speed=1, playing=True, loop=False)], ik=[adjusted])
    assert observe()['target'] == adjusted['target'] and measure(adjusted['target'])['position_error_m'] < 4e-6
    return tick, evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()
    source = args.source.read_bytes()
    oracle = LibraryOracle(source)
    assert (oracle.parents[WRIST], oracle.parents[ELBOW], oracle.nodes[SWORD]['name']) == (ELBOW, ARM_ROOT, '2H_Sword')
    native.ARGS, native.DEADLINE = args, time.monotonic() + 180
    report = dict(passed=False, gpu=args.gpu, source_sha256=hashlib.sha256(source).hexdigest(),
                  executable_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session = native.Session(args.output/'session')
    try:
        revision, chain = configure(session, args.source, oracle, args.gpu)
        original = session.world.read_bytes()
        saves = native.save_root(session)
        owner = id_request()
        session.rpc('runtime.start', dict(session_id=owner, revision=revision))
        tick, report['samples'] = traverse(session, owner, oracle, chain, args.gpu)
        payload = save(session, saves, 'checkpoint', owner, tick)
        saved_chain = json.loads(payload)['snapshot']['payload']['animation']['rigs'][0]['ik'][0]
        assert saved_chain['target_node'] == SWORD and saved_chain['space'] == 'rig'
        result = session.rpc('runtime.step', dict(request_id=id_request(), session_id=owner,
            expected_tick=tick, ticks=4))
        continued = save(session, saves, 'expected', owner, result['tick'])
        assert session.world.read_bytes() == original
        session.rpc('runtime.stop', dict(session_id=owner))
        session.close()
        session = native.Session(args.output/'cold', world=session.world)
        session.rpc('save.configure', dict(request_id=id_request(), expected_generation=0, root=native.native(saves)))
        owner = id_request()
        restored = session.rpc('save.load', dict(request_id=id_request(), configuration_generation=1, slot='checkpoint',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, expected_structure_revision=None, new_session_id=owner))
        assert restored['tick'] == tick
        assert save(session, saves, 'restored', owner, tick) == payload
        for expected_tick in range(tick, tick+4):
            session.rpc('runtime.step', dict(request_id=id_request(), session_id=owner, expected_tick=expected_tick, ticks=1))
        assert save(session, saves, 'continued', owner, tick+4) == continued
        assert session.world.read_bytes() == original
        session.rpc('runtime.stop', dict(session_id=owner))
        session.close()
        report.update(passed=True, cold_exact_save=True, four_tick_replay_exact=True)
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
        (args.output/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(f"Verified five original poses, live regrip and byte-exact cold continuation in {report['rpc_count']} native calls.")


if __name__ == '__main__':
    main()
