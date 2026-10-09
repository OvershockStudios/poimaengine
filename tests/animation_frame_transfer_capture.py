#!/usr/bin/env python3
"""Vulkan consumer of explicit reference-frame transfer, with original oracles.

The independent unskinned GLB uses closed-form original weighted positions and
inverse-transpose normals, never importer, CPU skinning or converter output.
All owned source files are removed before reopening and capturing cooked data.
This checks preview and 30 ordinary runtime ticks, not locomotion or an editor.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid

from fbx_capture import HEIGHT, WIDTH, need, sha
from frame_transfer_fixture import expected_motion, write_fixtures
from gltf_fixture import glb
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError

TIME = .5
MATRIX_ERROR, VERTEX_ERROR = 3e-5, 5e-5


def original_normals(moment):
    # Original input normals are +Z. Each original linear skin blend is
    # Rz(A) * ((1-w)I + w Rx(B)). Solve its transpose system analytically.
    angle, bend = math.radians(60 * moment), math.radians(40 * moment)
    result = []
    for weight in (0, .5, 1):
        a = 1 - weight + weight * math.cos(bend)
        b = weight * math.sin(bend)
        norm = math.hypot(a, b)
        result.append([b * math.sin(angle) / norm, -b * math.cos(angle) / norm, a / norm])
    return result


def analytic_triangle():
    positions, normals = expected_motion(TIME)['vertices'], original_normals(TIME)
    data = struct.pack('<18f3I', *(value for p, n in zip(positions, normals)
                                  for value in (*p, *n)), 0, 1, 2)
    document = {
        'asset': {'version': '2.0', 'generator': 'Poima original frame-transfer skin oracle'},
        'buffers': [{'byteLength': len(data)}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 72, 'byteStride': 24},
                        {'buffer': 0, 'byteOffset': 72, 'byteLength': 12}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
             'min': [min(p[i] for p in positions) for i in range(3)],
             'max': [max(p[i] for p in positions) for i in range(3)]},
            {'bufferView': 0, 'byteOffset': 12, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
            {'bufferView': 1, 'componentType': 5125, 'count': 3, 'type': 'SCALAR'}],
        'materials': [{'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1],
                                             'metallicFactor': 0, 'roughnessFactor': 1}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1},
                                    'indices': 2, 'material': 0}]}],
        'nodes': [{'name': 'Independent original weighted triangle', 'mesh': 0}],
        'scenes': [{'nodes': [0]}], 'scene': 0,
    }
    return glb(document, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host_binary', nargs='?', type=Path)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    parser.add_argument('--timeout', type=int, default=900)
    args = parser.parse_args()
    if (args.binary is None) == (args.host_binary is None):
        parser.error('Supply exactly one positional binary or --binary')
    args.binary = (args.binary or args.host_binary).resolve()
    if args.output.exists():
        parser.error('--output must be new')
    if args.gpu < 0 or not 60 <= args.timeout <= 1800:
        parser.error('--gpu must be nonnegative; --timeout must be 60..1800')
    run = args.output.resolve(); run.mkdir(parents=True)
    world = run / 'frame-transfer.world.json'
    deadline = time.monotonic() + args.timeout
    record = dict(passed=False, binary_sha256=sha(args.binary), source_sha256=sha(Path(__file__)),
                  fixture_sha256=sha(Path(__file__).with_name('frame_transfer_fixture.py')),
                  fbx_fixture_sha256=sha(Path(__file__).with_name('fbx_fixture.py')),
                  capture_helper_sha256=sha(Path(__file__).with_name('fbx_capture.py')),
                  gpu_index=args.gpu, samples=args.samples, time=TIME, calls=[], owners=[],
                  captures={}, comparisons={}, checks=[], source_files={},
                  independent_oracle=dict(**expected_motion(TIME), normals=original_normals(TIME)))
    owner = None

    def timeout(maximum=150):
        remaining = deadline - time.monotonic()
        need(remaining > 0, 'Capture qualification deadline exhausted')
        return min(maximum, remaining)

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True,
                                       timeout=timeout(10)).strip() if args.windows_interop else value

    def call(method, params=None):
        row = dict(method=method, params={} if params is None else params)
        record['calls'].append(row)
        try:
            result = owner.call(method, row['params'], timeout=timeout())
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message, data=failure.data)
            raise
        row['result'] = result
        return result

    def close():
        nonlocal owner
        previous, owner = owner, None
        if previous is None:
            return
        row = dict(exit_code=None); record['owners'].append(row)
        try:
            previous.close()
        except BaseException as error:
            row['cleanup_error'] = repr(error)
            raise
        finally:
            row.update(exit_code=previous.transport.returncode, stderr=previous.transport.stderr_tail,
                       stderr_truncated=previous.transport.stderr_truncated)
        need(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'],
             'Native owner did not exit cleanly')

    def state():
        store = Path(str(world) + '.assets')
        return (call('world.inspect'), call('world.history'), world.read_bytes(),
                {p.relative_to(store).as_posix(): sha(p) for p in sorted(store.rglob('*')) if p.is_file()})

    def uid(value):
        return f'{value:032x}'

    def capture(name, method, params, mode=None, runtime=False):
        path = run / (name + '.bmp')
        before = world.read_bytes()
        report = call(method, dict(camera=uid(1), path=native(path), width=WIDTH, height=HEIGHT,
                                  gpu=args.gpu, samples=args.samples, profile=True, **params))
        need(world.read_bytes() == before, 'Capture changed authored bytes')
        need(report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0,
             'Capture did not complete on Vulkan hardware')
        need(report['object_count'] == 1 and report['samples'] == args.samples,
             'Unexpected rendered scene/AA mode')
        if mode is not None:
            need(report['animation']['skinning'] == ('gpu_compute' if mode == 'gpu' else 'cpu_reference'),
                 'Requested skinning path was not exercised')
        if mode is not None or runtime:
            gpu = mode == 'gpu' or runtime
            draws = report['render_diagnostics']['last_draws']
            need(draws['skinned_instances'] == (1 if gpu else 0), 'Wrong GPU skinning instance count')
            if gpu:
                need(draws['skinned_vertices'] == 3, 'Wrong GPU skinning vertex count')
                need(report['render_diagnostics']['gpu']['skinning']['samples'] >= 1,
                     'No actual GPU skinning timing samples')
        record['captures'][name] = dict(report=report, sha256=sha(path))

    def compare(name, left, right, limit=64):
        a, b = pixels(run / (left + '.bmp')), pixels(run / (right + '.bmp'))
        need(len(a) == HEIGHT and len(b) == HEIGHT and all(len(row) == WIDTH for row in a + b),
             'Unexpected image extent')
        errors = [max(abs(x-y) for x, y in zip(p, q)) for ar, br in zip(a, b) for p, q in zip(ar, br)]
        result = dict(maximum_channel_error=max(errors), pixels_over_2=sum(v > 2 for v in errors),
                      changed_pixels=sum(v != 0 for v in errors), mean_max_channel_error=sum(errors)/len(errors))
        record['comparisons'][name] = result
        if limit is not None:
            need(result['pixels_over_2'] <= limit and result['mean_max_channel_error'] <= .1,
                 'Rendered comparison failed: ' + name + ' ' + str(result))
        return result

    def check_matrix(name, actual, wanted):
        need(len(actual) == len(wanted) == 16, 'Unexpected matrix extent')
        error = max(abs(a-b) for a, b in zip(actual, wanted))
        need(error <= MATRIX_ERROR, 'Independent FK mismatch: ' + name + ' ' + str(error))
        record['checks'].append(dict(kind='independent_global_matrix', node=name, maximum_error=error))

    try:
        sources = write_fixtures(run / 'sources')
        reference = sources / 'independent.glb'; reference.write_bytes(analytic_triangle())
        record['source_files'] = {p.relative_to(sources).as_posix(): sha(p)
                                  for p in sorted(sources.rglob('*')) if p.is_file()}
        owner = WorldClient.open(str(args.binary), native(world), close_timeout=10)
        policy = dict(policy='reference-frame-v1', source_pose=dict(kind='sample', clip=0, time=TIME),
                      target_pose=dict(kind='rest'))
        imported = call('asset.import', dict(source=native(sources / 'skin.fbx'), animations=[
            dict(source=native(sources / 'gauge_sample.fbx'), clip=1, name='OriginalGaugeMotion',
                 frame_transfer=policy)]))
        asset = imported['asset']
        need(imported['animations'] == 1, 'Unexpected composed clip count')
        need(any('source_reference=clip:0 source_time=0.5 target_reference=rest target_time=0' in line
                 for line in imported['diagnostics']), 'Original reference selector was not retained')
        reference_asset = call('asset.import', dict(source=native(reference)))['asset']
        clips = call('asset.inspect', dict(asset=asset, section='animations'))['items']
        need(len(clips) == 1 and clips[0]['name'] == 'OriginalGaugeMotion', 'Selected source take/name changed')
        clip = clips[0]['index']
        nodes = call('asset.inspect', dict(asset=asset, section='nodes'))['items']
        identities = {row['name']: row['index'] for row in nodes}
        mesh = next(row for row in nodes if row['primitives'])
        call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=[
            dict(op='entity.create', id=uid(1), name='Camera'),
            dict(op='component.set', id=uid(1), type='Transform', value=dict(
                position=[2.6, .7, 3], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=uid(1), type='Camera', value=dict(vertical_fov=40, near=.1, far=100)),
            dict(op='asset.instantiate', id=uid(100), name='Independent weighted reference', asset=reference_asset)]))
        committed = state()
        need(record['source_files'] == {p.relative_to(sources).as_posix(): sha(p)
                                        for p in sorted(sources.rglob('*')) if p.is_file()},
             'Original owned sources changed during import')
        close()
        shutil.rmtree(sources)
        need(not sources.exists(), 'Owned source removal failed')
        owner = WorldClient.open(str(args.binary), native(world), close_timeout=10)
        reopened = state()
        need((reopened[0], reopened[2], reopened[3]) == (committed[0], committed[2], committed[3]),
             'Fresh owner changed persistent world/cooked assets')
        need(reopened[1] == dict(bytes=0, max_bytes=16777216, max_entries=32, redo_count=0,
                                revision=1, session_local=True, skipped_large_edits=0, undo_count=0),
             'Fresh owner did not reset only session-local history')

        wanted = expected_motion(TIME)
        sampled = call('asset.animation.sample', dict(asset=asset, clip=clip, time=TIME,
                                                      loop=False, section='nodes'))
        observed = {row['index']: row for row in sampled['items']}
        for name in ('Root', 'Child'):
            check_matrix(name, observed[identities[name]]['world'], wanted[name.lower()])
        sampled_vertices = call('asset.animation.sample', dict(asset=asset, clip=clip, time=TIME,
            loop=False, section='vertices', node=mesh['index'], primitive=mesh['primitives'][0]))['items']
        need(len(sampled_vertices) == 3, 'Unexpected sampled vertex count')
        available = list(sampled_vertices)
        for index, (position, normal) in enumerate(zip(wanted['vertices'], original_normals(TIME))):
            vertex = min(available, key=lambda row: sum((a-b)**2 for a, b in zip(row['world_position'], position)))
            available.remove(vertex)
            error = max(abs(a-b) for a, b in zip(vertex['world_position'], position))
            normal_error = max(abs(a-b) for a, b in zip(vertex['normal'], normal))
            need(error <= VERTEX_ERROR and normal_error <= VERTEX_ERROR,
                 'Independent original weighted position/normal mismatch')
            record['checks'].append(dict(kind='independent_original_skin', vertex=index,
                                          maximum_position_error=error, maximum_normal_error=normal_error))
        pose = dict(revision=1, asset=asset, clip=clip, loop=False)
        for moment, name in ((0, 'initial'), (TIME, 'posed')):
            for mode in ('gpu', 'cpu'):
                capture(name + '-' + mode, 'asset.animation.capture', dict(pose, time=moment, skinning=mode), mode)
            compare(name + '_gpu_vs_cpu', name + '-gpu', name + '-cpu')
        capture('posed-unculled', 'asset.animation.capture', dict(pose, time=TIME, skinning='gpu', culling=False), 'gpu')
        capture('independent', 'world.capture', dict(revision=1))
        capture('independent-unculled', 'world.capture', dict(revision=1, culling=False))
        compare('gpu_vs_original_triangle', 'posed-gpu', 'independent')
        compare('cpu_vs_original_triangle', 'posed-cpu', 'independent')
        compare('converted_culling', 'posed-gpu', 'posed-unculled', 0)
        compare('reference_culling', 'independent', 'independent-unculled', 0)
        motion = compare('visible_motion', 'initial-gpu', 'posed-gpu', None)
        need(motion['pixels_over_2'] > 1000, 'Converted animation has no meaningful visible motion')
        need(state() == reopened, 'Read-only preview changed authored state/history/assets')

        call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1, ops=[
            dict(op='entity.delete', id=uid(100), recursive=True),
            dict(op='asset.instantiate', id=uid(200), name='Converted runtime rig', asset=asset)]))
        baseline = state()
        session = uid(900)
        call('runtime.start', dict(session_id=session, revision=2))
        stepped = call('runtime.step', dict(session_id=session, request_id=uuid.uuid4().hex,
            expected_tick=0, ticks=30, animations=[dict(entity=uid(200), clip=clip, time=0,
                                                     speed=1, loop=False, playing=True)]))
        need(stepped['tick'] == 30, 'Runtime did not advance 30 ordinary fixed ticks')
        for name in ('Root', 'Child'):
            node_id = hashlib.sha256(f'poima.instance.v1/{uid(200)}/node/{identities[name]}'.encode()).hexdigest()[:32]
            entity = call('runtime.entity', dict(session_id=session, tick=30, id=node_id))
            check_matrix('runtime ' + name, entity['world_matrix'], wanted[name.lower()])
        capture('runtime', 'runtime.capture', dict(session_id=session, tick=30), runtime=True)
        compare('runtime_vs_original_triangle', 'runtime', 'independent')
        compare('runtime_vs_converted_preview', 'runtime', 'posed-gpu')
        call('runtime.stop', dict(session_id=session))
        need(state() == baseline, 'Runtime changed authored state/history/assets')
        need(not sources.exists(), 'Removed original sources reappeared')
        need(sha(args.binary) == record['binary_sha256'], 'Native binary changed during capture')
        record.update(passed=True, source_independent=True, source_pins_verified_before_owned_removal=True,
                      authored_state_preserved_by_capture_and_runtime=True, runtime_ticks=30,
                      cpu_reference=True, independent_static_geometry_and_normals=True,
                      limits=['No OS screenshot, editor, C#, physical input, performance or human locomotion qualification.'])
    except BaseException as error:
        record['error'] = repr(error)
    finally:
        try:
            close()
        except BaseException as error:
            record['passed'] = False; record['close_error'] = repr(error)
        (run / 'evidence.json').write_text(json.dumps(record, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(run / 'evidence.json')
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
