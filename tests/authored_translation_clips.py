#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise new translation tracks in mixed clips and a committed support trace."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import time

from animation_fixture import ribbon
from authored_rotation_clips import inventory, step
from gltf_fixture import glb
from runtime_ik_attachment import save
from runtime_pose_frames import id_request, pose_camera, qy, transform, uid
from runtime_two_bone_ik_orientation import near, same_rotation
import world_physical_contract as native


METHOD = 'asset.animation.author'
ROOT_JOINT = 2
TIP_JOINT = 1
SOLE = 3
CAMERA = uid(31)
SOLE_OFFSET = [.375, -1, .125]
RIG_ORIGIN = [.75, 0, 2]
START = [.125, 0, -.25]
TOP = [.375, .25, -.125]
END = [.5, 0, .25]
REST = [-.25, .125, .375]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture_document():
    doc, blob = ribbon()
    doc['nodes'][ROOT_JOINT]['translation'] = REST
    doc['nodes'][TIP_JOINT]['children'] = [SOLE]
    doc['nodes'].append(dict(name='Sole marker', translation=SOLE_OFFSET))
    return doc, blob


def specs():
    return [
        dict(name='Lift and plant', tracks=[
            dict(node=ROOT_JOINT, keys=[
                dict(time=0, rotation=qy(0)), dict(time=.5, rotation=qy(60)),
                dict(time=1, rotation=qy(30)),
            ]),
            dict(node=ROOT_JOINT, path='translation', keys=[
                dict(time=0, translation=START), dict(time=.5, translation=TOP),
                dict(time=1, translation=END),
            ]),
        ]),
        dict(name='Held sole offsets', tracks=[
            dict(node=TIP_JOINT, path='translation', interpolation='step', keys=[
                dict(time=.25, translation=[.25, 1, 0]),
                dict(time=.75, translation=[-.5, 1.25, .125]),
            ]),
        ]),
    ]


def foreign_position(value, degrees):
    radians = math.radians(degrees)
    sine, cosine = math.sin(radians), math.cos(radians)
    x, _, z = SOLE_OFFSET
    # An independent rigid-body composition in model coordinates, with the
    # marker's Y offset cancelling the middle joint's +1 m rest translation.
    return [value[0] + cosine*x + sine*z, value[1],
            value[2] - sine*x + cosine*z]


def authored_position(seconds):
    if seconds <= .5:
        left, right, u = START, TOP, seconds*2
        degrees = 60*u
    else:
        left, right, u = TOP, END, (seconds-.5)*2
        degrees = 60 - 30*u
    return [a+(b-a)*u for a,b in zip(left,right)], degrees


def verify_admission(session):
    source = session.directory/'original-ribbon.glb'
    source.write_bytes(glb(*fixture_document()))
    base = session.rpc('asset.import', dict(source=native.native(source)))['asset']
    store = session.world.with_name(session.world.name + '.assets')
    initial = inventory(store)
    world_before = session.world.read_bytes() if session.world.is_file() else None
    history_before = session.rpc('world.history')
    discovery = session.rpc('world.describe', dict(view='method', name=METHOD))
    variants = discovery['methods'][METHOD]['properties']['clips']['items']['properties']['tracks']['items']['oneOf']
    translations = [schema for schema in variants if schema['properties'].get('path', {}).get('const') == 'translation']
    assert len(variants) == 3 and len(translations) == 1
    assert set(translations[0]['required']) == {'node', 'path', 'keys'}
    schema = translations[0]['properties']['keys']['items']
    assert set(schema['required']) == {'time', 'translation'}
    assert schema['properties']['translation']['minItems'] == schema['properties']['translation']['maxItems'] == 3

    def assert_world_identical():
        world_after = session.world.read_bytes() if session.world.is_file() else None
        assert world_before == world_after and session.rpc('world.history') == history_before

    def author(clips, error=None, asset=base):
        return session.rpc(METHOD, dict(asset=asset, clips=clips), error=error)

    def reject(patch):
        clips = copy.deepcopy(specs())
        tracks = clips[0]['tracks']
        patch(tracks, tracks[1], tracks[1]['keys'])
        author(clips, -32602)
        assert inventory(store) == initial
        assert_world_identical()

    reject(lambda tracks, track, keys: tracks.append(track))
    reject(lambda tracks, track, keys: track.update(node=True))
    reject(lambda tracks, track, keys: track.update(node=float(ROOT_JOINT)))
    reject(lambda tracks, track, keys: track.update(path='scale'))
    reject(lambda tracks, track, keys: track.update(hinge=dict(axis=[1, 0, 0])))
    reject(lambda tracks, track, keys: track.update(interpolation='cubic'))
    reject(lambda tracks, track, keys: track.pop('path'))
    reject(lambda tracks, track, keys: keys[1].update(translation=[0, 0]))
    reject(lambda tracks, track, keys: keys[1].update(translation=[True, 0, 0]))
    reject(lambda tracks, track, keys: keys[1].update(translation=[1e9 + 1, 0, 0]))
    reject(lambda tracks, track, keys: keys[1].update(translation=[0, -1e9 - 1, 0]))
    reject(lambda tracks, track, keys: keys[1].update(rotation=qy(30)))
    reject(lambda tracks, track, keys: keys[2].update(time=.5000000001))
    reject(lambda tracks, track, keys: keys[2].update(time=3600.01))
    invalid_tail = copy.deepcopy(specs())
    invalid_tail[1]['tracks'][0]['keys'][1]['translation'] = [0, 0, '0']
    author(invalid_tail, -32602)
    author(specs(), -32050, asset='f'*64)
    assert inventory(store) == initial
    assert_world_identical()

    receipt = author(specs())
    derived = receipt['asset']
    assert receipt['base_asset'] == base and derived != base
    assert receipt['clips'] == [
        dict(index=1, name='Lift and plant', duration=1., tracks=2),
        dict(index=2, name='Held sole offsets', duration=.75, tracks=1),
    ]
    assert author(specs()) == receipt
    reordered = copy.deepcopy(specs())
    reordered[0]['tracks'].reverse()
    for key in reordered[0]['tracks'][1]['keys']:
        key['rotation'] = [-v for v in key['rotation']]
    reordered[0]['tracks'][0]['keys'][0]['translation'][1] = -1e-50
    assert author(reordered) == receipt
    assert inventory(store) == {**initial, derived + '.pmodel': derived}
    assert_world_identical()

    for section in ('nodes', 'skins', 'primitives'):
        source_model, new_model = (session.rpc('asset.inspect', dict(asset=asset, section=section))
                                   for asset in (base, derived))
        assert source_model['items'] == new_model['items']
    source_channel, new_channel = (channel(session, asset, 0, 0) for asset in (base, derived))
    assert {**source_channel, 'asset': derived} == new_channel
    assert inspect(session, base, 0, .5, TIP_JOINT)['position'] == [.5, 1, 0]
    session.rpc('asset.animation.sample', dict(asset=base, clip=1, time=0), error=-32602)

    translation, rotation = (channel(session, derived, 1, index) for index in range(2))
    assert translation['node'] == rotation['node'] == ROOT_JOINT
    assert translation['path'] == 'translation' and rotation['path'] == 'rotation'
    assert translation['interpolation'] == rotation['interpolation'] == 'LINEAR'
    assert [row['time'] for row in translation['items']] == [0, .5, 1]
    assert [row['value'][:3] for row in translation['items']] == [START, TOP, END]
    held = channel(session, derived, 2, 0)
    assert held['node'] == TIP_JOINT and held['path'] == 'translation' and held['interpolation'] == 'STEP'

    for moment in (0, .125, .25, .5, .75, .875, 1):
        position, degrees = authored_position(moment)
        row = inspect(session, derived, 1, moment, ROOT_JOINT)
        near(row['position'], position)
        same_rotation(row['rotation'], qy(degrees))
        assert row['scale'] == [1, 1, 1]
        marker = inspect(session, derived, 1, moment, SOLE)
        near(marker['world'][12:15], foreign_position(position, degrees))
    rest = inspect(session, derived, None, 0, ROOT_JOINT)
    assert rest['position'] == REST
    near(inspect(session, derived, 1, 3, ROOT_JOINT)['position'], END)
    near(inspect(session, derived, 1, 1.25, ROOT_JOINT, loop=True)['position'], authored_position(.25)[0])
    for moment, value in ((0, [.25, 1, 0]), (.5, [.25, 1, 0]),
                          (.75, [-.5, 1.25, .125]), (3, [-.5, 1.25, .125])):
        row = inspect(session, derived, 2, moment, TIP_JOINT)
        assert row['position'] == value
        same_rotation(row['rotation'], [0, 0, 0, 1])
    retained = source.with_name('retained-source.glb')
    source.rename(retained)
    assert not source.exists()
    return receipt


def channel(session, asset, clip, ordinal):
    return session.rpc('asset.animation.channel', dict(asset=asset, clip=clip, channel=ordinal))


def inspect(session, asset, clip, moment, node, **extra):
    selection = {} if clip is None else dict(clip=clip)
    return session.rpc('asset.animation.sample', dict(asset=asset, time=moment,
        offset=node, limit=1, **selection, **extra))['items'][0]


def install(session, asset, capture):
    rig = dict(asset=asset, clip=1, time=0, speed=1, playing=True, loop=False)
    ops = [
        dict(op='asset.instantiate', id=native.RIG, name='Local translation rig', asset=asset),
        native.component(native.RIG, 'Transform', transform(RIG_ORIGIN, qy(90))),
        native.component(native.RIG, 'AnimationRig', rig),
        dict(op='entity.create', id=native.FLOOR, name='Independent support'),
        native.component(native.FLOOR, 'Transform', transform((0, -.5, 0))),
        native.component(native.FLOOR, 'BoxCollider', dict(half_extents=[10, .5, 10],
            motion='static', mass=1, friction=.5, restitution=0)),
    ]
    if capture:
        ops.extend([
            dict(op='entity.create', id=CAMERA, name='Support review camera'),
            native.component(CAMERA, 'Camera', dict(vertical_fov=49, near=.03, far=30)),
            native.component(CAMERA, 'Transform', pose_camera([1.74, 2.4, 5.8], [.95, .97, 1.84])),
        ])
    return native.transact(session, ops, 0)['revision']


def runtime_pose(session, owner, tick):
    params = dict(session_id=owner, tick=tick)
    joint = session.rpc('runtime.entity', dict(params, id=native.node(ROOT_JOINT)))
    marker = session.rpc('runtime.entity', dict(params, id=native.node(SOLE)))
    translation, degrees = authored_position(tick/60)
    near(joint['local_transform']['position'], translation)
    same_rotation(joint['local_transform']['rotation'], qy(degrees))
    x, y, z = foreign_position(translation, degrees)
    position = marker['world_matrix'][12:15]
    near(position, [RIG_ORIGIN[0] + z, y, RIG_ORIGIN[2] - x], 5e-6)
    ray = session.rpc('runtime.raycast', dict(params, origin=[position[0], position[1]+.5, position[2]],
        direction=[0, -1, 0], distance=2))['hit']
    assert ray['entity'] == native.FLOOR
    near(ray['position'], [position[0], 0, position[2]], 5e-6)
    assert math.isclose(position[1] - ray['position'][1], translation[1], abs_tol=5e-6)
    return dict(tick=tick, local_transform=joint['local_transform'], sole=position, support=ray['position'])


def capture_pose(session, owner, tick, gpu):
    if gpu is None:
        return None
    path = session.directory/f'translation-{tick}.bmp'
    result = session.rpc('runtime.capture', dict(session_id=owner, tick=tick, camera=CAMERA,
        path=native.native(path), width=320, height=240, samples=1, gpu=gpu))
    assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0
    assert result['tick'] == tick
    return dict(gpu=result['gpu'], sha256=sha(path.read_bytes()))


def verify_runtime(session, asset, gpu):
    revision = install(session, asset, gpu is not None)
    authored = session.world.read_bytes()
    saves = native.save_root(session)
    owner = id_request()
    start = session.rpc('runtime.start', dict(revision=revision, session_id=owner))
    assert start['tick'] == 0
    observations = [runtime_pose(session, owner, 0)]
    first_capture = capture_pose(session, owner, 0, gpu)
    tick = step(session, owner, 0, 15)
    observations.append(runtime_pose(session, owner, tick))
    checkpoint_tick = step(session, owner, tick, 15)
    checkpoint_pose = runtime_pose(session, owner, checkpoint_tick)
    observations.append(checkpoint_pose)
    checkpoint = save(session, saves, 'checkpoint', owner, checkpoint_tick)
    warm_capture = capture_pose(session, owner, checkpoint_tick, gpu)
    if gpu is not None:
        assert first_capture['sha256'] != warm_capture['sha256']
    warm_tail = after_checkpoint(session, owner, checkpoint_tick)
    observations.extend(warm_tail)
    terminal = save(session, saves, 'expected', owner, 60)
    session.rpc('runtime.stop', dict(session_id=owner))
    session.close()

    with_cold = native.Session(session.directory.parent/'cold', world=session.world)
    try:
        owner = id_request()
        with_cold.rpc('save.configure', dict(request_id=id_request(), expected_generation=0,
            root=native.native(saves)))
        restored = with_cold.rpc('save.load', dict(request_id=id_request(), configuration_generation=1,
            slot='checkpoint', expected_generation=1, revision=revision, expected_session_id=None,
            expected_tick=None, expected_gameplay_revision=None, expected_structure_revision=None,
            new_session_id=owner))
        assert restored['tick'] == checkpoint_tick
        assert runtime_pose(with_cold, owner, checkpoint_tick) == checkpoint_pose
        assert save(with_cold, saves, 'restored', owner, checkpoint_tick) == checkpoint
        cold_capture = capture_pose(with_cold, owner, checkpoint_tick, gpu)
        assert cold_capture == warm_capture
        assert after_checkpoint(with_cold, owner, checkpoint_tick, split=True) == warm_tail
        assert save(with_cold, saves, 'continued', owner, 60) == terminal
        assert session.world.read_bytes() == authored
        with_cold.rpc('runtime.stop', dict(session_id=owner))
    finally:
        with_cold.close()
    return dict(sole_observations=observations, checkpoint_sha256=sha(checkpoint),
                terminal_sha256=sha(terminal), captures=[first_capture, warm_capture, cold_capture]
                if gpu is not None else [])


def after_checkpoint(session, owner, tick, split=False):
    observations = []
    for _ in range(2):
        if split:
            tick = step(session, owner, tick, 7)
        tick = step(session, owner, tick, 8 if split else 15)
        observations.append(runtime_pose(session, owner, tick))
    return observations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, help='Also compare three native hardware captures')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic()+180
    report = dict(passed=False, executable_sha256=sha(args.binary.read_bytes()))
    session = native.Session(args.output/'source')
    try:
        receipt = verify_admission(session)
        report['derived_model'] = receipt
        report.update(verify_runtime(session, receipt['asset'], args.gpu))
        report['passed'] = True
    except BaseException as failure:
        report['failure'] = repr(failure)
        raise
    finally:
        try:
            session.close()
        finally:
            report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
            (args.output/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(f"Verified mixed authored clips and exact support continuation in {report['rpc_count']} native calls.")


if __name__ == '__main__':
    main()
