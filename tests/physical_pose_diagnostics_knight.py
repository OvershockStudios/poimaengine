#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify the authored joint-limit query against the unmodified Knight GLB."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'examples/physical-character-yard'))
import world_physical_contract as harness
from character_library_intake import LibraryOracle, SOURCE_SHA
from knight_profile import compose, relative, source_world_bodies, quaternion, conjugate
from physical_character_animation import admitted_target, rotation_difference


def identity(path):
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def run(session, world, oracle, observations):
    components = world['entities'][harness.RIG]['components']
    profile = components['PhysicalRig']
    package = Path(str(session.world) + '.assets')
    observed_files = [session.world, *sorted(package.iterdir())]
    pins = {p.name: identity(p) for p in observed_files}
    history = session.rpc('world.history')
    for clip, seconds, name in [(None, 0, 'rest'), (36, 0, 'Idle'), (36, .1, 'Idle'),
                                (48, 0, 'Running_A'), (48, .1, 'Running_A'), (48, .25, 'Running_A')]:
        assert clip is None or oracle.clips[clip]['name'] == name
        params = dict(entity=harness.RIG, revision=world['revision'], clip=clip, time=seconds)
        result = session.rpc('asset.animation.physical_limits', params)
        assert result['asset'] == components['AnimationRig']['asset']
        assert result['entity'] == harness.RIG and result['revision'] == world['revision']
        assert result['clip'] == clip and result['sample_time'] == seconds
        assert len(result['joints']) == len(profile['joints']) == 14
        bodies = source_world_bodies(oracle, profile, clip, seconds)
        outside = []
        maximum = 0
        for joint, row in zip(profile['joints'], result['joints']):
            assert row['joint'] == joint['name'] and row['limits'] == joint['limits']
            assert row['parent']['part'] == joint['parent'] and row['child']['part'] == joint['child']
            parent = compose(bodies[joint['parent']], joint['parent_frame'])
            child = compose(bodies[joint['child']], joint['child_frame'])
            requested = relative(parent, child)['rotation']
            error = rotation_difference(requested, row['requested_rotation'])
            maximum = max(maximum, error)
            assert error < 2e-6, (clip, seconds, joint['name'], error)
            fit = row['fit']
            assert fit is not None and math.isclose(math.hypot(*fit['rotation']), 1, abs_tol=1e-12)
            admitted_target(fit['rotation'], joint['limits'])
            delta = quaternion(fit['rotation'], conjugate(row['requested_rotation']))
            distance = 2 * math.atan2(math.hypot(*delta[:3]), abs(delta[3]))
            assert math.isclose(distance, fit['correction_radians'], abs_tol=1e-10)
            if row['status'] == 'outside_limits':
                outside.append(dict(joint=joint['name'], correction_radians=distance))
            else:
                assert row['status'] == 'within_limits' and distance < 1e-10
        assert result['counts'] == dict(within_limits=14-len(outside), outside_limits=len(outside), indeterminate=0)
        assert result['status'] == ('outside_limits' if outside else 'within_limits')
        observations.append(dict(clip=clip, name=name, time=seconds, maximum_component_error=maximum, outside=outside))
    assert any(row['outside'] for row in observations)
    assert session.rpc('world.history') == history
    assert {p.name: identity(p) for p in observed_files} == pins
    return pins


def main():
    assert __debug__
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True, help='Prepared Physical Character Yard with the original Knight and 14-joint profile')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    clone = args.output / 'project'
    clone.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.project / 'world.json', clone / 'world.json')
    shutil.copytree(args.project / 'world.json.assets', clone / 'world.json.assets')
    source = args.project / 'content/Knight.glb'
    original = source.read_bytes()
    source_identity = dict(bytes=len(original), sha256=hashlib.sha256(original).hexdigest())
    assert source_identity == dict(bytes=3659532, sha256=SOURCE_SHA), 'Expected the original Knight GLB'
    oracle = LibraryOracle(original)
    assert len(oracle.nodes) == 57 and len(oracle.clips) == 76
    world = json.loads((clone / 'world.json').read_bytes())
    harness.ARGS = args
    harness.DEADLINE = time.monotonic() + 60
    report = dict(format='poima.knight-source-joints.v1', passed=False, source=source_identity,
                  executable=identity(args.binary), observations=[])
    try:
        session = harness.Session(args.output / 'session', clone / 'world.json')
        try:
            report['unchanged_world_and_packages'] = run(session, world, oracle, report['observations'])
        finally:
            session.close()
        report['passed'] = True
    finally:
        report.update(owners=harness.REPORT['owners'], rpc_count=len(harness.REPORT['calls']))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
        print(json.dumps(report, allow_nan=False))


if __name__ == '__main__':
    main()
