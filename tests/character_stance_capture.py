#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fixed-Tick stance views against independent authored cameras; external owned Job required.

Five live poses and five references use the same immutable primitive course.
RGB maximum error <=1 and >=100 strongly changed pixels for the initial eye
drop are declared before execution. This does not qualify manual input,
animated body crouching, a player window, or performance.
"""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
sys.path.insert(0, str(ROOT/'tests'))
from live_material_capture import need, sha, encoded, same, compare
from runtime_camera_lens import LensOwner
from shadow_controls_capture import component, transform
from scene_capture import pixels

W, H, CAP = 320, 240, 64*1048576
ACTOR, CAMERA, REFERENCE = (f'{n:032x}' for n in (100, 101, 200))
LENS = dict(vertical_fov=70.0, near=.1, far=100.0)
LABELS = ('standing-before', 'crouched-before', 'crouched-under-door',
          'blocked-under-door', 'standing-after-door')


def uid(n):
    return f'{n:032x}'


def new():
    return uuid.uuid4().hex


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def course():
    ops = []
    # Collision boxes and rendered boxes share their authored physical extents.
    geometry = [(1, [0, -.5, 0], [16, 1, 16], [.04, .04, .04]),
                (2, [0, 1.4, 0], [1.6, .2, 2], [.12, .04, .01]),
                (3, [0, 1.5, -4], [12, 3, .2], [.02, .02, .02])]
    geometry += [(10+i, [0, y, -3.88], [12, .08, .04], color)
                 for i, (y, color) in enumerate(zip((.3, .8, 1.3, 1.8, 2.3),
                     ([.5, 0, 0], [0, .5, 0], [0, 0, .5], [.5, .3, 0], [.3, 0, .5])))]
    for n, position, scale, color in geometry:
        ops += [dict(op='entity.create', id=uid(n), name='Course '+str(n), parent=None),
                transform(uid(n), position, scale),
                component(uid(n), 'MeshRenderer', dict(primitive='box', albedo=[0, 0, 0],
                    visible=True, cast_shadows=False, receive_shadows=False)),
                component(uid(n), 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=color,
                    metallic=0, roughness=1, double_sided=True))]
        if n in (1, 2):
            ops.append(component(uid(n), 'BoxCollider', dict(half_extents=[.5, .5, .5],
                motion='static', mass=1, friction=.5, restitution=0)))
    for identity, parent, position in ((ACTOR, None, [-2, .1, 0]),
            (CAMERA, ACTOR, [0, 1.6, 0]), (REFERENCE, None, [0, 0, 0]),
            (uid(30), None, [0, 0, 0])):
        ops += [dict(op='entity.create', id=identity, name='View '+identity, parent=parent),
                transform(identity, position)]
    ops += [component(ACTOR, 'CharacterController', dict(radius=.3, height=1.8, speed=4,
                jump_speed=5, camera=CAMERA, stance={})),
            component(CAMERA, 'Camera', copy.deepcopy(LENS)),
            component(REFERENCE, 'Camera', copy.deepcopy(LENS)),
            component(uid(30), 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1))]
    return ops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--gpu', required=True, type=int)
    parser.add_argument('--lighting-path', required=True, choices=('forward', 'deferred'))
    args = parser.parse_args()
    args.binary = args.binary.resolve(); args.output = args.output.resolve()
    need(os.name == 'nt' and not sys.flags.optimize and args.gpu in (0, 1) and
         not args.output.exists(), 'Fresh externally owned Windows output required.')
    args.output.mkdir(parents=True); (args.output/'captures').mkdir()
    source_files = [Path(__file__), args.binary, args.binary.parent/'phonon.dll']
    source_files += [ROOT/'tests'/name for name in ('runtime_camera_lens.py',
        'live_material_capture.py', 'shadow_controls_capture.py', 'scene_capture.py',
        'gltf_fixture.py', 'texture_fixture.py')]
    source_files += [p for p in (ROOT/'tools/python/poima_client').rglob('*')
                    if p.is_file() and p.suffix in ('.py', '.json')]
    pins = {str(p): sha(p) for p in source_files}
    started = time.monotonic(); deadline = started+300; owner = None; tick = 0
    record = dict(passed=False, source_pins=pins, calls=[], transport=[], captures=[],
        stages=[], comparisons=[], native_owner=None, checks=[], recipe=dict(gpu=args.gpu,
        lighting_path=args.lighting_path, width=W, height=H, samples=1,
        maximum_rgb_error=1, minimum_eye_drop_strong_pixels=100, maximum_seconds=300,
        maximum_output_bytes=CAP, expected_captures=10), scope=__doc__)

    def entity(identity):
        return owner.rpc('runtime.entity', dict(session_id=owner.session, tick=tick, id=identity))

    def stance():
        return owner.rpc('runtime.character_stance', dict(session_id=owner.session,
            tick=tick, entity=ACTOR))['state']

    def step(count=1, crouch=None, move=False):
        nonlocal tick
        params = dict(session_id=owner.session, request_id=new(), expected_tick=tick,
            ticks=count, character_stances=[] if crouch is None else [dict(entity=ACTOR, crouch=crouch)],
            inputs=[] if not move else [dict(entity=ACTOR, move=[1, 0], look=[0, 0], jump=False, use=False)])
        result = owner.rpc('runtime.step', params)
        need(result['tick'] == tick+count and result['replayed'] is False, 'Tick progression differs.')
        tick = result['tick']

    def capture(label, live, expected_matrix):
        path = args.output/'captures'/(label+'.bmp')
        params = dict(camera=CAMERA if live else REFERENCE, path=str(path), gpu=args.gpu,
            width=W, height=H, samples=1, lighting_path=args.lighting_path,
            frames_in_flight=2, capture_frames=2, ambient_occlusion=dict(mode='none'),
            reconstruction='none', scene_product_probes=[dict(x=x, y=y)
                for y in (H//2-24, H//2, H//2+24) for x in (W//2-32, W//2, W//2+32)])
        if live: params.update(session_id=owner.session, tick=tick, material_revision=0)
        else: params['revision'] = owner.revision
        before_hash = sha(args.output/'world.json')
        result = owner.rpc('runtime.capture' if live else 'world.capture', params)
        need(result['capture_written'] is True and result['hardware'] is True and
             result['nvrhi_errors'] == 0 and result['build_version'] == '0.0.125' and
             result['width'] == W and result['height'] == H and result['samples'] == 1 and
             result['object_count'] == 8 and same(result['lens'], LENS), 'Capture identity/recipe differs.')
        need(same(result['camera_world'], expected_matrix), 'Actual capture camera differs from observed pose.')
        if live:
            need(result['tick'] == tick and result['session_id'] == owner.session,
                 'Capture advanced or replaced runtime.')
        diagnostics = result['render_diagnostics']; receipts = diagnostics['scene_receipts']
        need(same(receipts['submitted'], receipts['presented']), 'Submission/presentation mismatch.')
        shown = receipts['presented']; observed = copy.deepcopy(receipts['observed'])
        identity = copy.deepcopy(shown)
        for row in (observed, identity):
            row.pop('frame_sequence'); row.pop('submission_id')
        need(same(observed, identity) and shown['camera_id'] == params['camera'] and
             shown['world_id'] == result['world_id'] and shown['authored_revision'] == result['revision'] and
             same(shown['camera_lens'], LENS) and shown['frame_sequence'] > 0 and
             shown['submission_id'] > 0 and bool(shown['presentation_source_id']), 'Exact scene receipt missing.')
        need(diagnostics['lighting_path'] == args.lighting_path and
             diagnostics['ambient_occlusion']['mode'] == 'none' and
             diagnostics['reconstruction']['mode'] == 'none' and
             diagnostics['last_draws']['camera_triangles'] > 0, 'Rendering path substituted or empty.')
        probes = diagnostics['scene_products']['probes']
        need(diagnostics['scene_products']['available'] is True and len(probes) == 9 and
             sum(p['surface_valid'] for p in probes) >= 3 and
             all(all(math.isfinite(v) for v in p['raw_hdr']) for p in probes), 'Actual world HDR probes missing.')
        need(sha(args.output/'world.json') == before_hash, 'Capture changed authored course.')
        image = pixels(path)
        need(sum(max(pixel) >= 20 for row in image for pixel in row) >= 1000, 'Visual course is empty.')
        record['captures'].append(dict(label=label, image=path.relative_to(args.output).as_posix(),
            sha256=sha(path), bytes=path.stat().st_size, world_sha256=before_hash, result=result))
        return image

    def live_stage(label, crouched, requested, blocked):
        state = stance(); actor = entity(ACTOR); camera = entity(CAMERA)
        need(state['configured'] is True and state['crouched'] is crouched and
             state['requested_crouch'] is requested and state['stand_blocked'] is blocked,
             'Committed stance differs at '+label)
        expected_drop = f32(.8) if crouched else 0.0
        need(abs(camera['local_transform']['position'][1]-(1.6-expected_drop)) < 1e-6 and
             state['camera_drop'] == expected_drop and state['speed'] == (2 if crouched else 4),
             'Stance eye/speed differs at '+label)
        matrix = camera['world_matrix']
        unit = [1,0,0,0,0,1,0,0,0,0,1,0,None,None,None,1]
        need(len(matrix) == 16 and all(math.isfinite(v) for v in matrix) and
             all(expected is None or value == expected for value, expected in zip(matrix, unit)),
             'Independent camera reference requires the actual unrotated rigid pose.')
        image = capture(label, True, matrix)
        need(same(actor, entity(ACTOR)) and same(camera, entity(CAMERA)) and same(state, stance()),
             'Capture changed committed physical/stance state.')
        record['stages'].append(dict(label=label, tick=tick, stance=state, actor=actor, camera=camera))
        return image

    try:
        owner = LensOwner(args, record, deadline, 1)
        discovery = owner.rpc('world.describe', {})
        need(discovery['schema_revision'] == 93 and 'runtime.character_stance' in discovery['methods'],
             'Stance discovery differs.')
        owner.edit(course()); original_hash = sha(args.output/'world.json')
        original_source = args.output/'course-original.json'
        original_source.write_bytes((args.output/'world.json').read_bytes())
        record['authored_course'] = dict(file=original_source.name, sha256=sha(original_source),
            bytes=original_source.stat().st_size, revision=owner.revision)
        need(record['authored_course']['sha256'] == original_hash, 'Retained original course differs.')
        owner.session = new(); start = owner.rpc('runtime.start', dict(session_id=owner.session, revision=owner.revision))
        need(start['started'] is True and start['tick'] == 0, 'Runtime did not start.')
        record['runtime_start'] = start
        step(60); standing = live_stage(LABELS[0], False, False, False)
        initial_foot = record['stages'][-1]['actor']['world_matrix'][13]
        step(3, True); crouched = live_stage(LABELS[1], True, True, False)
        need(abs(record['stages'][-1]['actor']['world_matrix'][13]-initial_foot) < .03 and
             abs(record['stages'][-1]['actor']['world_matrix'][12]+2) < 1e-6, 'Crouch moved feet horizontally.')
        eye_change = compare(standing, crouched)
        need(eye_change['strong_changed_pixels'] >= 100, 'Eye drop did not visibly change the course.')
        record['eye_drop_comparison'] = eye_change
        step(60, move=True); live_stage(LABELS[2], True, True, False)
        need(abs(record['stages'][-1]['actor']['world_matrix'][12]) < .05, 'Crouched actor missed doorway interior.')
        step(3, False); live_stage(LABELS[3], True, False, True)
        step(60, move=True); live_stage(LABELS[4], False, False, False)
        need(record['stages'][-1]['actor']['world_matrix'][12] > 1.15, 'Automatic rise did not leave the doorway.')
        need(sha(args.output/'world.json') == original_hash, 'Live course mutated authored source.')
        record['checks'].extend(['visible_eye_drop_with_foot_preservation',
            'low_doorway_blocks_standing', 'persistent_stand_request_automatically_recovers'])
        owner.stop()
        # The reference camera has no parent, controller or stance. Its complete
        # transform is authored from the independently observed committed pose;
        # no stance height, drop or Character implementation computes this view.
        for stage in record['stages']:
            matrix = stage['camera']['world_matrix']
            owner.edit([transform(REFERENCE, matrix[12:15])])
            reference = capture(stage['label']+'-reference', False, matrix)
            actual = pixels(args.output/'captures'/(stage['label']+'.bmp'))
            comparison = compare(actual, reference)
            need(comparison['max_rgb_delta'] <= 1, 'Live stance view differs from authored reference.')
            record['comparisons'].append(dict(live=stage['label'], reference=stage['label']+'-reference', **comparison))
        need(len(record['captures']) == 10 and len(record['comparisons']) == 5 and
             len({c['result']['gpu'] for c in record['captures']}) == 1, 'Capture population/adapter differs.')
        live_sources = {c['result']['render_diagnostics']['scene_receipts']['presented']['presentation_source_id']
                        for c in record['captures'][:5]}
        need(len(live_sources) == 1, 'Capture stages replaced the native runtime source.')
        record['checks'].append('five_independent_authored_camera_parity_comparisons')
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if owner is not None:
            try: owner.close()
            except BaseException:
                record['cleanup_error'] = traceback.format_exc(); record['passed'] = False
        record['changed_sources'] = [p for p, digest in pins.items() if not Path(p).is_file() or sha(p) != digest]
        record['seconds'] = time.monotonic()-started
        record['passed'] = bool(record['passed'] and not record['changed_sources'] and
            record['seconds'] <= 300 and record['native_owner'] is not None and len(record['transport']) <= 1000)
        data = encoded(record)+b'\n'
        need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data) <= CAP,
             'Whole output/evidence cap exceeded.')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic() > deadline:
            record['passed'] = False; record['publication_error'] = 'Whole300s deadline exceeded.'
            (args.output/'evidence.json').rename(args.output/'late-evidence.json')
            failed = encoded(record)+b'\n'
            need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(failed) <= CAP,
                 'Failure evidence cap exceeded.')
            (args.output/'failure.json').write_bytes(failed)
    print(json.dumps(dict(passed=record['passed'], captures=len(record['captures']),
        comparisons=len(record['comparisons']), gpu=args.gpu, lighting_path=args.lighting_path)))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
