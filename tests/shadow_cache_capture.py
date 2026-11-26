#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare retained per-layer shadow depth against the previous Vulkan renderer."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import WorldClient, new_id

from scene_capture import pixels
from shadow_index_culling_capture import component, create, transform

CAMERA, ENV, RECEIVER, LEFT, RIGHT, LEFT_LIGHT, RIGHT_LIGHT = (
    f'{value:032x}' for value in range(1, 8)
)
IDENTITY = [0, 0, 0, 1]
WIDTH, HEIGHT = 480, 270


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def operations():
    def drawable(entity, color, cast):
        return component(entity, 'MeshRenderer', dict(
            primitive='box', albedo=color, visible=True, cast_shadows=cast))
    ops = [
        create(CAMERA, 'Fixed shadow camera'), transform(CAMERA, (0, 0, 4)),
        component(CAMERA, 'Camera', dict(vertical_fov=55, near=.1, far=60)),
        create(ENV, 'Two-view lighting'),
        component(ENV, 'LightingEnvironment', dict(ambient=[.04]*3, exposure=1, shadow_resolution=1024)),
        create(RECEIVER, 'Non-casting receiver'), transform(RECEIVER, (0, 0, -6), scale=(14, 7, .2)),
        drawable(RECEIVER, [.52, .45, .4], False),
    ]
    for entity, light, x, color in ((LEFT, LEFT_LIGHT, -1.85, [1, .35, .22]),
                                    (RIGHT, RIGHT_LIGHT, 1.85, [.22, .45, 1])):
        ops += [
            create(entity, 'Box caster'), transform(entity, (x, .22, -3.25), scale=(.6, .7, .25)),
            drawable(entity, [.22, .26, .29], True),
            create(light, 'Separate spot shadow'), transform(light, (x*1.42, .22, -.85)),
            component(light, 'Light', dict(kind='spot', color=color, intensity=275, range=10,
                enabled=True, inner_angle=17, outer_angle=27,
                shadow=dict(enabled=True, distance=10, bias=.0001, normal_bias=.005))),
        ]
    # A collider owner moves the left visual child without resizing its box geometry.
    owner = f'{20:032x}'
    ops.extend([
        create(owner, 'Kinematic caster owner'), transform(owner, (-1.85, .22, -3.25)),
        component(owner, 'BoxCollider', dict(half_extents=[.3, .35, .125], motion='kinematic',
            mass=1, friction=.5, restitution=0)),
        dict(op='entity.reparent', id=LEFT, parent=owner, mode='keep_local'),
        transform(LEFT, (0, 0, 0), scale=(.6, .7, .25)),
    ])
    return ops


def check_profiler(client, capture, frames):
    status = client.call('profiler.stop', dict(capture_id=capture))
    assert status['state']=='stopped' and status['dropped']==0 and status['open']==0, status
    view_events = {}
    offset = 0
    while offset is not None:
        page = client.call('profiler.events', dict(capture_id=capture, offset=offset, limit=1024))
        for event in page['events']:
            if not event['name'].startswith('renderer.shadow_view.'):
                continue
            key = event['render_submission']
            pair = key['renderer_id'], key['submission_id']
            by_name = view_events.setdefault(pair, {})
            assert event['name'] not in by_name, event
            by_name[event['name']] = event['value']
        offset = page['next_offset']
    assert len(view_events)==len(frames)
    for image in frames.values():
        d = image['report']['render_diagnostics']
        f = d['frame_execution']
        views = view_events[(f['renderer_id'], f['last_completed_submission_id'])]
        counts = d['last_draws']
        assert len(views)==10, views
        assert sum(views[f'renderer.shadow_view.{i}.reused'] for i in (0,1))==counts['shadow_reused_views']
        assert sum(views[f'renderer.shadow_view.{i}.batches'] for i in (0,1))==counts['shadow_batches']
        for i in (0,1):
            prefix = f'renderer.shadow_view.{i}.'
            if views[prefix+'reused']:
                assert all(views[prefix+field]==0 for field in ('draws','batches','triangles')), views
        image['profile_views'] = views
    return len(view_events)


def run(args, binary, root, current, evidence):
    root.mkdir()
    world = root / 'world.json'
    data = evidence['current' if current else 'retained'] = {}
    data['binary_sha256'] = digest(binary)
    captures = data['captures'] = {}

    def native(path):
        if not args.windows_interop:
            return str(path)
        return subprocess.check_output(['wslpath', '-aw', str(path)], text=True).strip()

    with WorldClient.open(binary, native(world), cwd=root, trace_directory=root/'wire',
                          trace_limit_bytes=4*1024*1024) as client:
        revision = client.call('world.inspect')['revision']
        edit = client.call('world.transact', dict(
            request_id=new_id(), base_revision=revision, ops=operations()))
        assert edit['committed']
        revision = edit['revision']
        session = new_id()
        assert client.call('runtime.start', dict(session_id=session, revision=revision))['started']
        capture_id = new_id()
        if current and args.check_profiler:
            opened = client.call('profiler.start', dict(capture_id=capture_id, expected_capture_id=None,
                                                        capacity=16384, gpu_passes=True))
            assert opened['state']=='recording'
        tick = 0
        completed = 0

        def capture(name, reused, batches, *, width=WIDTH, renderer_reused=True):
            nonlocal completed
            file = root / f'{name}.bmp'
            sample_grid = [{'x': round(width*x), 'y': round(HEIGHT*y)}
                           for x in (.28, .39, .61, .72) for y in (.45, .53)]
            frame = client.call('runtime.capture', dict(
                session_id=session, tick=tick, camera=CAMERA, path=native(file),
                width=width, height=HEIGHT, capture_frames=1, gpu=args.gpu, samples=1,
                lighting_path=args.lighting_path, reconstruction=args.reconstruction,
                reuse_renderer=True, profile=True, scene_product_probes=sample_grid), timeout=60)
            assert frame['hardware'] and frame['capture_written'] and frame['nvrhi_errors']==0, frame
            assert frame['renderer_reused'] is renderer_reused and frame['tick']==tick, frame
            diagnostics = frame['render_diagnostics']
            assert frame['frames_presented']==1 and diagnostics['gpu']['available']
            completed = completed+1 if renderer_reused else 1
            assert diagnostics['completed_submissions']==completed, diagnostics
            draws = diagnostics['last_draws']
            assert draws['shadow_views']==2, draws
            assert draws['shadow_reused_views' if current else 'shadow_views']==(reused if current else 2), draws
            assert draws['shadow_batches']==(batches if current else draws['shadow_draws']), draws
            captures[name] = dict(
                sha256=digest(file), report=frame,
                gpu_shadow_ms=diagnostics['gpu']['shadows']['last_ms'])

        capture('cold', 0, 2, renderer_reused=False)
        for index in range(6):
            capture(f'warm-{index}', 2, 0)
        saves = root/'saves'
        saves.mkdir()
        client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=native(saves)))
        checkpoint = client.call('save.write', dict(request_id=new_id(), configuration_generation=1,
            expected_generation=0, slot='stationary', session_id=session, expected_tick=0,
            expected_gameplay_revision=0, expected_structure_revision=0))
        moved = client.call('runtime.step', dict(
            session_id=session, request_id=new_id(), expected_tick=tick, ticks=1,
            motions=[dict(entity=f'{20:032x}', position=[-1.85, .88, -3.25],
                          rotation=IDENTITY, duration_ticks=1)]))
        tick = moved['current_tick']
        assert tick==1
        capture('caster-moved', 1, 1)
        capture('caster-still', 2, 0)
        moved = client.call('runtime.step', dict(
            session_id=session, request_id=new_id(), expected_tick=tick, ticks=1,
            camera_poses=[dict(entity=CAMERA, world=dict(position=[.3, 0., 0.], rotation=IDENTITY))]))
        tick = moved['current_tick']
        assert tick==2
        capture('camera-moved', 2, 0)
        capture('resized', 0, 2, width=512, renderer_reused=False)
        capture('resized-warm', 2, 0, width=512)
        assert client.call('runtime.capture.release', dict(session_id=session))['released']
        capture('reopened', 0, 2, renderer_reused=False)
        replacement = new_id()
        restored = client.call('save.load', dict(request_id=new_id(), configuration_generation=1,
            slot='stationary', expected_generation=1, revision=revision, expected_session_id=session,
            expected_tick=tick, expected_gameplay_revision=0, expected_structure_revision=0,
            new_session_id=replacement))
        assert restored['session_id']==replacement and restored['tick']==0 and not restored['recovered']
        session = replacement
        tick = restored['tick']
        capture('restored', 0, 2, renderer_reused=False)
        assert captures['restored']['sha256']==captures['cold']['sha256']
        capture('restored-warm', 2, 0)
        data['restored_checkpoint_sha256'] = checkpoint['sha256']
        assert client.call('runtime.capture.release', dict(session_id=session))['released']
        client.call('runtime.stop', dict(session_id=session))
        if current and args.check_profiler:
            data['profiled_submissions'] = check_profiler(client, capture_id, captures)
    warm = pixels(root/'warm-5.bmp')
    moved = pixels(root/'caster-moved.bmp')
    if args.reconstruction=='none':
        left = sum(a!=b for row, next_row in zip(warm,moved,strict=True)
                   for a,b in zip(row[:WIDTH//2],next_row[:WIDTH//2],strict=True))
        right = sum(a!=b for row, next_row in zip(warm,moved,strict=True)
                    for a,b in zip(row[WIDTH//2:],next_row[WIDTH//2:],strict=True))
        assert left>=256 and right==0, (left,right)
        data['moving_silhouette_pixels'] = {'affected_half':left,'separate_light_half':right}
    return captures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--lighting-path', choices=('forward','deferred'), default='forward')
    parser.add_argument('--reconstruction', choices=('none','fsr3_native'), default='none')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--check-profiler', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = dict(passed=False, gpu_index=args.gpu, lighting_path=args.lighting_path,
                    reconstruction=args.reconstruction, fixture_sha256=digest(Path(__file__)))
    try:
        stock = run(args, args.reference.resolve(strict=True), args.output/'reference', False, evidence)
        current = run(args, args.binary.resolve(strict=True), args.output/'current', True, evidence)
        assert current.keys()==stock.keys()
        for name in current:
            a, b = current[name], stock[name]
            assert a['report']['build_version']=='0.0.463' and b['report']['build_version']=='0.0.462'
            assert a['report']['gpu']==b['report']['gpu']
            assert a['sha256']==b['sha256'], name
            assert a['report']['render_diagnostics']['scene_products']['probes']==\
                   b['report']['render_diagnostics']['scene_products']['probes'], name
        evidence['passed'] = True
        print(f'{len(current)} exact frames and HDR/depth probes on {current["cold"]["report"]["gpu"]}')
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        (args.output/'report.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__':
    main()
