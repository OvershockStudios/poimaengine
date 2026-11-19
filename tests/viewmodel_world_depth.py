#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check Vulkan depth ordering and a same-tick release to world physics."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import time
from uuid import uuid4

from gltf_fixture import glb
from runtime_attachment import close_matrix
from runtime_pose_frames import transform, uid
from scene_capture import pixels
from texture_fixture import quad
import world_physical_contract as native

CAMERA, HAND, PROP, WALL, ENVIRONMENT, PROXY = map(uid, (1, 2, 3, 4, 5, 6))
WORLD = dict(vertical_fov=65, near=.1, far=100)
FORE = dict(vertical_fov=43, near=.005, far=8)
RECIPES = (
    (HAND, (0, 0, -1.2), (.65, .4, 1), [.8, 0, 0]),
    (PROP, (.18, 0, -1.02), (.21, .14, 1), [0, .8, 0]),
    (WALL, (-.41, 0, -.88), (.13, .2, 1), [0, 0, .8]),
)
WITNESSES = ((192, 120), (72, 120), (142, 92))


def configure(owner, mode):
    schema = owner.rpc('world.describe', dict(view='component', name='Camera'))['components']['Camera']
    depth = schema['properties']['viewmodel_world_depth']
    assert depth['type'] == 'boolean' and depth['default'] is False
    assert 'viewmodel' in schema['dependentRequired']['viewmodel_world_depth']
    source = owner.directory / 'origin.glb'
    source.write_bytes(glb(*quad([], dict(doubleSided=True))))
    asset = owner.rpc('asset.import', dict(source=native.native(source)))['asset']
    camera = dict(WORLD)
    if mode == 'depth':
        for invalid in (dict(camera, viewmodel_world_depth=True),
                        dict(camera, viewmodel=FORE, viewmodel_world_depth=1)):
            native.transact(owner, [dict(op='entity.create', id=CAMERA, name='Camera'),
                native.component(CAMERA, 'Camera', invalid)], 0, error=-32602)
    if mode != 'reference':
        camera.update(viewmodel=FORE, viewmodel_match_world_fov=True)
    if mode == 'depth':
        camera['viewmodel_world_depth'] = True
    ops = [dict(op='entity.create', id=CAMERA, name='Camera'), native.component(CAMERA, 'Camera', camera),
           dict(op='entity.create', id=ENVIRONMENT, name='Dark environment'),
           native.component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[0,0,0], exposure=1)),
           dict(op='template.set', id=PROXY, name='Released collision proxy', components=dict(
               Transform=transform(), BoxCollider=dict(half_extents=[.07,.07,.02], motion='dynamic',
                   mass=.6, friction=.5, restitution=0)))]
    for identity, position, scale, color in RECIPES:
        bound = mode != 'reference' and identity in (HAND, PROP)
        ops.extend([dict(op='entity.create', id=identity, name='Depth witness', parent=CAMERA if bound else None),
                    native.component(identity, 'Transform', transform(position, scale=scale)),
                    native.component(identity, 'StaticMesh', dict(asset=asset, primitive=0, visible=True, cast_shadows=False)),
                    native.component(identity, 'PbrMaterial', dict(base_color=[0,0,0], emissive=color,
                        metallic=0, roughness=1, double_sided=True))])
        if bound:
            ops.append(native.component(identity, 'CameraDraw', dict(camera=CAMERA, mode='viewmodel')))
    return native.transact(owner, ops, 0)['revision']


def capture(owner, args, mode, label, released=False, **guard):
    path = owner.directory / (label + '.bmp')
    method = 'runtime.capture' if 'session_id' in guard else 'world.capture'
    answer = owner.rpc(method, dict(guard, camera=CAMERA, path=native.native(path), gpu=args.gpu,
        width=320, height=240, samples=1, lighting_path=args.lighting_path, ambient_occlusion={'mode': 'none'},
        scene_product_probes=[dict(x=x, y=y) for x,y in WITNESSES]))
    assert answer['hardware'] and answer['capture_written'] and answer['nvrhi_errors'] == 0
    diagnostics = answer['render_diagnostics']
    receipts = diagnostics['scene_receipts']
    observed, presented = copy.deepcopy(receipts['observed']), copy.deepcopy(receipts['presented'])
    assert receipts['submitted'] == presented
    for key in ('frame_sequence', 'submission_id'):
        assert presented.pop(key) > 0
        observed.pop(key)
    assert observed == presented and presented['camera_lens'] == WORLD
    draw = presented.get('camera_draw')
    if mode == 'reference':
        assert draw is None
    else:
        assert [x['entity_id'] for x in draw['viewmodels']] == ([HAND] if released else [HAND, PROP])
        assert draw['viewmodel_lens'] == (WORLD if mode == 'depth' else dict(FORE, vertical_fov=WORLD['vertical_fov']))
        assert draw.get('viewmodel_world_depth', False) == (mode == 'depth')
    assert ('viewmodel' in diagnostics) == (mode == 'overlay')
    assert answer['object_count'] == (1+released if mode == 'overlay' else 3)
    if mode == 'overlay':
        assert diagnostics['viewmodel']['rendered'] and diagnostics['viewmodel']['draws']['camera_draws'] == 2-released
    probes = diagnostics['scene_products']['probes']
    image = pixels(path)
    return dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        colors=[list(image[y][x]) for x,y in WITNESSES],
        depths=[p['depth'] if p['surface_valid'] else None for p in probes])


def run(args, mode):
    owner = native.Session(args.output / mode)
    try:
        revision = configure(owner, mode)
        authored = capture(owner, args, mode, 'authored', revision=revision)
        session = uuid4().hex
        start = owner.rpc('runtime.start', dict(session_id=session, revision=revision))
        assert start['tick'] == 0
        receipt = owner.rpc('runtime.attachment.apply', dict(session_id=session, request_id=uuid4().hex,
            expected_tick=0, expected_structure_revision=0, commands=[dict(entity=PROP, mode='keep_world', anchor=CAMERA)]))
        assert receipt['tick'] == 0 and receipt['structure_revision'] == 1
        before = capture(owner, args, mode, 'held', session_id=session, tick=0, structure_revision=1)
        assert authored == before
        matrix = owner.rpc('runtime.entity', dict(session_id=session, tick=0, id=PROP, structure_revision=1))['world_matrix']
        dropped = owner.rpc('runtime.attachment.release', dict(session_id=session, request_id=uuid4().hex,
            expected_tick=0, expected_structure_revision=1, item=PROP, body_template=PROXY,
            linear_velocity_m_s=[0,0,0], angular_velocity_rad_s=[0,0,0]))
        assert dropped['tick'] == 0 and dropped['structure_revision'] == 2 and dropped['body'] != PROP
        close_matrix(owner.rpc('runtime.entity', dict(session_id=session, tick=0, structure_revision=2,
            id=PROP))['world_matrix'], matrix)
        after = capture(owner, args, mode, 'world', released=True, session_id=session, tick=0, structure_revision=2)
        return dict(before=before, after=after)
    finally:
        owner.close()


def channel(color, index):
    return color[index] > 80 and all(value < 8 for i,value in enumerate(color) if i != index)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output.mkdir()
    native.ARGS, native.DEADLINE = args, time.monotonic() + 180
    report = dict(passed=False, gpu=args.gpu, lighting_path=args.lighting_path, cases={}, owners=native.REPORT['owners'])
    try:
        for mode in ('reference', 'overlay', 'depth'):
            report['cases'][mode] = run(args, mode)
        ref, overlay, shared = (report['cases'][mode] for mode in ('reference', 'overlay', 'depth'))
        assert shared == ref and shared['before'] == shared['after']
        for color, index in zip(shared['before']['colors'], (1, 2, 0)):
            assert channel(color, index), shared
        for moment, index in (('before', 1), ('after', 0)):
            assert channel(overlay[moment]['colors'][0], index)
            assert channel(overlay[moment]['colors'][1], 0)
        assert shared['before']['depths'][2] is not None and overlay['before']['depths'][2] is None
        report['passed'] = True
    finally:
        report['calls'] = len(native.REPORT['calls'])
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('World-depth membership, wall occlusion and the grip handoff matched the ordinary world reference.')


if __name__ == '__main__':
    main()
