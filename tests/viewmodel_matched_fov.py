#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare real foreground and world silhouettes with different configured lenses."""
import argparse
import copy
import json
from pathlib import Path
import time
from uuid import uuid4

from gltf_fixture import glb
from runtime_pose_frames import transform, uid
from scene_capture import pixels
from texture_fixture import quad
import world_physical_contract as native

CAMERA, DRAW, ENVIRONMENT = map(uid, (1, 2, 3))
FORE = dict(vertical_fov=45, near=.005, far=8)
WORLD = dict(vertical_fov=65, near=.1, far=100)
ZOOM = dict(WORLD, vertical_fov=39)


def capture(owner, args, label, projection, expected, **guard):
    path = owner.directory / (label + '.bmp')
    method = 'runtime.capture' if 'session_id' in guard else 'world.capture'
    answer = owner.rpc(method, dict(guard, camera=CAMERA, path=native.native(path), gpu=args.gpu,
        width=320, height=240, samples=1, viewmodel_samples=1, ambient_occlusion={'mode': 'none'}))
    assert answer['hardware'] and answer['capture_written'] and answer['nvrhi_errors'] == 0
    receipts = answer['render_diagnostics']['scene_receipts']
    observed = copy.deepcopy(receipts['observed'])
    presented = copy.deepcopy(receipts['presented'])
    assert receipts['submitted'] == presented
    for key in ('frame_sequence', 'submission_id'):
        assert presented.pop(key) > 0
        observed.pop(key)
    assert observed == presented and presented['camera_lens'] == projection
    draw = presented.get('camera_draw')
    if expected is None:
        assert draw is None
    else:
        assert draw['viewmodel_lens'] == expected and [x['entity_id'] for x in draw['viewmodels']] == [DRAW]
        assert answer['render_diagnostics']['viewmodel']['draws']['camera_draws'] == 1
    return {(x, y) for y, row in enumerate(pixels(path)) for x, (r, g, b) in enumerate(row)
            if r > 90 and g < 8 and b < 8}


def run(args, mode):
    owner = native.Session(args.output / mode)
    try:
        schema = owner.rpc('world.describe', dict(view='component', name='Camera'))
        camera_schema = schema['components']['Camera']
        matching = camera_schema['properties']['viewmodel_match_world_fov']
        assert matching['type'] == 'boolean' and matching['default'] is False
        assert 'viewmodel' in camera_schema['dependentRequired']['viewmodel_match_world_fov']
        source = owner.directory / 'quad.glb'
        source.write_bytes(glb(*quad([], dict(doubleSided=True))))
        asset = owner.rpc('asset.import', dict(source=native.native(source)))['asset']
        ops = [dict(op='entity.create', id=key, name=label) for key, label in
               ((CAMERA, 'Observer'), (DRAW, 'Off-axis prop'), (ENVIRONMENT, 'Black environment'))]
        if mode != 'world':
            ops.append(dict(op='entity.reparent', id=DRAW, parent=CAMERA, mode='keep_local'))
        camera = dict(WORLD)
        if mode == 'matched':
            invalid = dict(camera, viewmodel_match_world_fov=True)
            native.transact(owner, ops + [native.component(CAMERA, 'Camera', invalid)], 0, error=-32602)
            invalid = dict(camera, viewmodel=FORE, viewmodel_match_world_fov='true')
            native.transact(owner, ops + [native.component(CAMERA, 'Camera', invalid)], 0, error=-32602)
            camera['viewmodel_match_world_fov'] = True
        if mode != 'world':
            camera['viewmodel'] = FORE
            ops.append(native.component(DRAW, 'CameraDraw', dict(camera=CAMERA, mode='viewmodel')))
        ops.extend([native.component(CAMERA, 'Camera', camera),
                    native.component(DRAW, 'Transform', transform((.26, -.12, -1.1), scale=(.38, .24, 1))),
                    native.component(DRAW, 'StaticMesh', dict(asset=asset, primitive=0, visible=True, cast_shadows=False)),
                    native.component(DRAW, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=[.8, 0, 0],
                        metallic=0, roughness=1, double_sided=True)),
                    native.component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1))])
        revision = native.transact(owner, ops, 0)['revision']
        session = uuid4().hex
        started = owner.rpc('runtime.start', dict(session_id=session, revision=revision))
        assert started['started'] and started['tick'] == 0
        expected = None if mode == 'world' else dict(FORE, vertical_fov=65 if mode == 'matched' else 45)
        authored = capture(owner, args, 'authored', WORLD, expected, revision=revision)
        start = capture(owner, args, 'start', WORLD, expected, session_id=session, tick=0)
        command = dict(entity=CAMERA, world=ZOOM)
        changed = dict(FORE, vertical_fov=77, near=.015)
        if mode != 'world':
            command['viewmodel'] = changed
        step = owner.rpc('runtime.step', dict(request_id=uuid4().hex, session_id=session, expected_tick=0,
            ticks=1, camera_lenses=[command]))
        assert step['tick'] == 1
        state = owner.rpc('runtime.camera_lens', dict(session_id=session, entity=CAMERA, tick=1))['state']
        assert state['world'] == ZOOM and state['viewmodel'] == command.get('viewmodel')
        expected = None if mode == 'world' else dict(changed, vertical_fov=39 if mode == 'matched' else 77)
        zoom = capture(owner, args, 'zoom', ZOOM, expected, session_id=session, tick=1)
        assert len(start) > 500 and start == authored and len(start ^ zoom) > 250
        return start, zoom
    finally:
        owner.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic() + 180
    report = dict(passed=False, gpu=args.gpu, checks=[], owners=native.REPORT['owners'])
    try:
        world, independent, matched = (run(args, mode) for mode in ('world', 'independent', 'matched'))
        for index, name in enumerate(('authored_fov', 'zoomed_fov')):
            result = dict(case=name, world_pixels=len(world[index]), independent_pixels=len(independent[index]),
                matched_pixels=len(matched[index]), independent_delta=len(world[index] ^ independent[index]),
                matched_delta=len(world[index] ^ matched[index]))
            report['checks'].append(result)
            assert result['independent_delta'] > 250 and result['matched_delta'] == 0, result
        report['passed'] = True
    finally:
        report['calls'] = len(native.REPORT['calls'])
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Matched foreground and world masks are pixel-identical at both FOVs.')


if __name__ == '__main__':
    main()
