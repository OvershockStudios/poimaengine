#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare one walk-to-run transition against clip-zero playback on the original Knight."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import time
import uuid

from character_library_intake import LibraryOracle, interpolate_rotation, multiply, transform_matrix
import world_physical_contract as native


def command(clip, **fields):
    return dict(dict(entity=native.RIG, clip=clip, time=0, speed=1, loop=True, playing=True), **fields)


def blend_matrix(oracle, source, target, node, weight):
    before, after = source[node], target[node]
    local = transform_matrix(
        position=[a + weight * (b - a) for a, b in zip(before['position'], after['position'])],
        rotation=interpolate_rotation(before['rotation'], after['rotation'], weight),
        scale=[a + weight * (b - a) for a, b in zip(before['scale'], after['scale'])])
    if node not in oracle.parents:
        return local
    parent = blend_matrix(oracle, source, target, oracle.parents[node], weight)
    return multiply(parent, local)


def run(session, oracle, gpu, camera):
    walk, run = 72, 48
    assert [oracle.clips[i]['name'] for i in (walk, run)] == ['Walking_A', 'Running_A']
    # These annotated samples are low-toe landmarks in the original in-place
    # cycles. They are not observations of a sole planted on physical terrain.
    source_marks = [7 / 60, 39 / 60]
    target_marks = [9 / 60, 33 / 60]
    toes = (22, 26)
    assert [oracle.nodes[i]['name'] for i in toes] == ['toes.l', 'toes.r']
    results = []
    for side, source_time, target_time, toe in zip(('left', 'right'), source_marks, target_marks, toes):
        sample_height = oracle.pose(run, target_time)[2][toe][13]
        restart_height = oracle.pose(run, 0)[2][toe][13]
        assert sample_height < .03 and restart_height > .28
        walk_pose = oracle.pose(walk, source_time)[0]
        trajectories = {}
        for mode in ('clip_zero', 'phase_markers'):
            owner = uuid.uuid4().hex
            session.rpc('runtime.start', dict(session_id=owner, revision=1))
            tick = 0

            def advance(ticks, animations=(), phase_matches=()):
                nonlocal tick
                values = dict(session_id=owner, request_id=uuid.uuid4().hex, expected_tick=tick,
                              ticks=ticks, animations=list(animations), animation_phase_matches=list(phase_matches))
                committed = session.rpc('runtime.step', values)
                tick += ticks
                assert committed['tick'] == tick

            advance(1, [command(walk, time=source_time, playing=False)])
            blend = command(run, blend_ticks=12)
            if mode == 'phase_markers':
                blend['phase_sync'] = dict(source=source_marks, destination=target_marks)
            samples = []
            for offset in range(1, 13):
                if offset == 1 and mode == 'phase_markers':
                    advance(1, phase_matches=[blend])
                else:
                    advance(1, [blend] if offset == 1 else [])
                state = session.rpc('runtime.entity', dict(session_id=owner, tick=tick, id=native.RIG))['animation']
                expected = ((target_time if mode == 'phase_markers' else 0) + offset / 60)
                assert math.isclose(state['time'], expected, abs_tol=1e-12)
                assert (state['transition'] is not None) == (offset < 12)
                foot = session.rpc('runtime.entity', dict(session_id=owner, tick=tick, id=native.node(toe)))
                expected_pose = blend_matrix(oracle, walk_pose, oracle.pose(run, expected)[0], toe, offset/12)
                residual = max(abs(a - b) for a, b in zip(expected_pose, foot['world_matrix']))
                assert residual < 2e-6, (side, mode, offset, residual)
                samples.append(dict(ticks_into_fade=offset, destination_seconds=state['time'],
                                    toe_height=foot['world_matrix'][13], matrix_error=residual))
                if gpu is not None and offset in (4, 8):
                    path = session.directory / f'{side}-{mode}-{offset:02d}.bmp'
                    frame = session.rpc('runtime.capture', dict(session_id=owner, tick=tick, camera=camera,
                        path=native.native(path), width=960, height=540, gpu=gpu, samples=4))
                    assert frame['capture_written'] and frame['hardware'] and frame['nvrhi_errors'] == 0
                    samples[-1]['capture'] = dict(filename=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            trajectories[mode] = samples
            session.rpc('runtime.stop', dict(session_id=owner))
        improvements = [a['toe_height'] - b['toe_height'] for a,b in zip(
            trajectories['clip_zero'][:5], trajectories['phase_markers'][:5])]
        results.append(dict(side=side, source_time=source_time, destination_time=target_time,
                            destination_toe_height=sample_height, clip_zero_toe_height=restart_height,
                            first_five_ticks_lower_by=improvements, trajectories=trajectories))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True, help='Unmodified CC0 Knight.glb')
    parser.add_argument('--output', type=Path, required=True, help='Fresh evidence directory')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, help='Capture matched views using the selected Vulkan adapter')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()
    original = args.source.read_bytes()
    oracle = LibraryOracle(original)
    native.ARGS = args
    native.DEADLINE = time.monotonic() + 180
    report = dict(passed=False, gpu=args.gpu, source_sha256=hashlib.sha256(original).hexdigest(),
                  executable_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session = native.Session(args.output / 'session')
    try:
        imported = session.rpc('asset.import', dict(source=native.native(args.source)))
        ops = [dict(op='asset.instantiate', id=native.RIG, name='Knight', asset=imported['asset'])]
        camera = uuid.uuid4().hex
        if args.gpu is not None:
            x, z = -2.0, 2.1
            half_yaw = math.atan2(x, z) / 2
            ops.extend([dict(op='entity.create', id=camera, name='Gait observer'),
                        native.component(camera, 'Camera', dict(vertical_fov=37, near=.02, far=30)),
                        native.component(camera, 'Transform', dict(position=[x,.7,z], scale=[1,1,1],
                            rotation=[0, math.sin(half_yaw), 0, math.cos(half_yaw)]))])
        session.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
        before = session.world.read_bytes()
        report['take_comparisons'] = run(session, oracle, args.gpu, camera)
        assert session.world.read_bytes() == before
        report['passed'] = True
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'], rpc_count=len(native.REPORT['calls']))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print('Verified two original-clip foot landmarks and four fixed-tick transitions.')


if __name__ == '__main__':
    main()
