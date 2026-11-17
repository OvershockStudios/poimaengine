#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the public source-pose diagnostic against an independently specified GLB."""
import argparse
import copy
import json
import math
from pathlib import Path
import struct
import time
import uuid

import world_physical_contract as harness
from animation_fixture import ribbon
from gltf_fixture import glb

METHOD = 'asset.animation.physical_limits'
RIG = harness.RIG
ROOT = Path(__file__).resolve().parents[1]
IDENTITY = [0, 0, 0, 1]


def rotation(axis, angle):
    value = [0., 0., 0., math.cos(angle / 2)]
    value[axis] = math.sin(angle / 2)
    return value


def model_fixture():
    doc, source = ribbon()
    blob = bytearray(source)
    # Node 1 precedes its parent 5 and grandparent 2. The unprofiled parent
    # rotates 0.25 rad. The common Z rotation must cancel in joint space.
    doc['nodes'][2].update(rotation=rotation(2, .5), children=[5, 3, 4])
    doc['nodes'].extend([dict(name='Swing'), dict(name='Quiet'),
                         dict(name='Spacer', rotation=rotation(0, .25), children=[1])])
    clip = doc['animations'][0]
    for node, end in [(1, rotation(0, 1.2)), (3, [0, 1, 0, 0])]:
        while len(blob) % 4:
            blob.append(0)
        doc['bufferViews'].append(dict(buffer=0, byteOffset=len(blob), byteLength=32))
        blob.extend(struct.pack('<8f', *IDENTITY, *end))
        doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1,
                                     componentType=5126, count=2, type='VEC4'))
        clip['channels'].append(dict(sampler=len(clip['samplers']), target=dict(node=node, path='rotation')))
        clip['samplers'].append(dict(input=clip['samplers'][0]['input'],
                                     output=len(doc['accessors'])-1, interpolation='LINEAR'))
    doc['buffers'][0]['byteLength'] = len(blob)
    return glb(doc, bytes(blob))


def rig_fixture():
    rig = harness.profile()
    root, tip = rig['parts']
    hinge = rig['joints'][0]
    for node, name in [(3, 'swing'), (4, 'quiet')]:
        part = dict(copy.deepcopy(tip), name=name, node=node)
        joint = dict(copy.deepcopy(hinge), name=name, child=name)
        joint['parent_frame']['rotation'] = rotation(0, -.35)
        rig['parts'].append(part)
        rig['joints'].append(joint)
    root['body_in_bone']['rotation'] = rotation(0, .35)
    tip['body_in_bone']['rotation'] = rotation(0, .3)
    hinge['parent_frame']['rotation'] = rotation(0, .1)
    hinge['child_frame']['rotation'] = rotation(0, -.1)
    hinge['limits'] = dict(kind='hinge', minimum=-.5, maximum=.5)
    return rig


def near(value, expected):
    if not math.isclose(value, expected, abs_tol=2e-6):
        raise AssertionError(f'Expected {expected}, received {value}')


def run(session):
    rpc = session.rpc
    describe = rpc('world.describe', dict(view='method', name=METHOD))
    schema = describe['methods'][METHOD]
    assert set(schema['required']) == {'entity', 'revision', 'clip', 'time'}
    source = session.directory/'calibrated-rig.glb'
    source.write_bytes(model_fixture())
    asset = rpc('asset.import', dict(source=harness.native(source)))['asset']
    assert harness.transact(session, [dict(op='asset.instantiate', id=RIG, name='Diagnostic rig', asset=asset)], 0)['revision'] == 1
    base = dict(entity=RIG, revision=1, clip=None, time=0)
    rpc(METHOD, base, -32602)
    profile = rig_fixture()
    assert harness.transact(session, [harness.component(RIG, 'PhysicalRig', profile)], 1)['revision'] == 2
    rpc(METHOD, base, -32009)
    base['revision'] = 2
    world_bytes = session.world.read_bytes()
    package = Path(str(session.world) + '.assets') / (asset + '.pmodel')
    cooked_bytes = package.read_bytes()

    def sample(clip, seconds, loop=False):
        result = rpc(METHOD, dict(base, clip=clip, time=seconds, loop=loop))
        assert result['format'] == 'poima.physical-pose-diagnostics.v1'
        assert result['source'] == 'cooked_model' and result['angle_unit'] == 'radians'
        assert result['rotation_space'] == 'child_joint_relative_to_parent_joint' and result['rotation_order'] == 'xyzw'
        assert result['entity'] == RIG and result['revision'] == 2 and result['asset'] == asset
        assert result['clip'] == clip and result['requested_time'] == seconds and result['loop'] is loop
        assert [row['joint'] for row in result['joints']] == ['spine', 'swing', 'quiet']
        return result

    rest = sample(None, 0)
    assert rest['status'] == 'within_limits' and rest['counts'] == dict(within_limits=3, outside_limits=0, indeterminate=0)
    hinge = rest['joints'][0]
    assert hinge['parent']['node'] == 2 and hinge['child']['node'] == 1
    assert hinge['parent']['part'] == 'root' and hinge['child']['part'] == 'tip'
    assert hinge['limits'] == profile['joints'][0]['limits']
    near(hinge['fit']['correction_radians'], 0)
    middle = sample(0, 1)
    assert middle['counts'] == dict(within_limits=1, outside_limits=2, indeterminate=0)
    assert middle['joints'][1]['fit']['swing_clamped'] is True
    near(middle['joints'][1]['fit']['correction_radians'], math.pi/2-.6)

    end = sample(0, 9)
    assert end['sample_time'] == 2 and end['status'] == 'outside_limits'
    assert end['counts'] == dict(within_limits=1, outside_limits=1, indeterminate=1)
    hinge, singular, quiet = end['joints']
    assert len(hinge['requested_rotation']) == len(hinge['fit']['rotation']) == 4
    for actual, expected in zip(hinge['requested_rotation'], rotation(0, 1.2)):
        near(actual, expected)
    for actual, expected in zip(hinge['fit']['rotation'], rotation(0, .5)):
        near(actual, expected)
    assert hinge['status'] == 'outside_limits' and hinge['fit']['twist_clamped'] is True
    near(hinge['fit']['correction_radians'], .7)
    assert singular['status'] == 'indeterminate' and singular['fit'] is None and 'singular' in singular['reason']
    assert quiet['status'] == 'within_limits' and quiet['fit']['correction_radians'] == 0
    wrapped = sample(0, 3, True)
    assert wrapped['sample_time'] == 1 and harness.exact(middle['joints'], wrapped['joints'])
    assert harness.exact(rest, sample(None, 0))
    for overrides in [dict(time=1), dict(loop=True), dict(clip=True), dict(clip=1), dict(time=-1), dict(loop=1)]:
        rpc(METHOD, dict(base, **overrides), -32602)
    rpc(METHOD, {k: v for k, v in base.items() if k != 'clip'}, -32602)
    rpc(METHOD, dict(base, entity='f'*32), -32004)
    assert world_bytes == session.world.read_bytes() and cooked_bytes == package.read_bytes()

    # A one-part rigid body has no joint angles to judge.
    root_only = dict(profile, parts=profile['parts'][:1], joints=[])
    assert harness.transact(session, [harness.component(RIG, 'PhysicalRig', root_only)], 2)['revision'] == 3
    empty = rpc(METHOD, dict(base, revision=3))
    assert empty['status'] == 'no_joints' and empty['joints'] == []
    assert empty['counts'] == dict(within_limits=0, outside_limits=0, indeterminate=0)


def main():
    if not __debug__:
        raise RuntimeError("Run without Python optimization to enable fixture assertions")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    harness.ARGS = args
    harness.DEADLINE = time.monotonic()+70
    output = (args.output or ROOT/'build/physical-pose-diagnostics'/uuid.uuid4().hex).resolve()
    output.mkdir(parents=True, exist_ok=False)
    session = harness.Session(output/'session')
    report = dict(format='poima.physical-pose-diagnostics-contract.v1', passed=False)
    try:
        try:
            run(session)
        finally:
            session.close()
        report['passed'] = True
    finally:
        report.update(owners=harness.REPORT['owners'], calls=harness.REPORT['calls'])
        (output/'report.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f"Physical pose diagnostics passed={report['passed']}. Evidence in {output}")


if __name__ == '__main__':
    main()
