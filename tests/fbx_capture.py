#!/usr/bin/env python3
"""Original FBX Vulkan skinning, independent geometry and live runtime capture.

The analytic GLB is a separate, unskinned triangle written directly in meters.
Its positions come from the fixture's published weights and linear motion, not
from Poima/ufbx sampling. All source files are removed before capture. Runtime
qualification advances the instantiated rig through 30 ordinary fixed ticks.
No editor, external content, physical input, or C# qualification is implied.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import uuid
from fbx_fixture import write_fixtures
from gltf_fixture import glb
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError

TIME = .5
WIDTH, HEIGHT = 640, 480


def analytic_triangle():
    # Root-only, half-root/half-child, child-only. Mesh world offset is +2m X.
    positions = [(2, 0, 0), (3+.5*TIME, 0, 0), (2+TIME, 1, 0)]
    data = struct.pack('<18f3I', *(value for p in positions for value in (*p, 0, 0, 1)), 0, 1, 2)
    document = {
        'asset': {'version': '2.0', 'generator': 'Poima independent FBX skin oracle'},
        'buffers': [{'byteLength': len(data)}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 72, 'byteStride': 24},
                        {'buffer': 0, 'byteOffset': 72, 'byteLength': 12}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
             'min': [2, 0, 0], 'max': [3+.5*TIME, 1, 0]},
            {'bufferView': 0, 'byteOffset': 12, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
            {'bufferView': 1, 'componentType': 5125, 'count': 3, 'type': 'SCALAR'}],
        'materials': [{'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1],
                                             'metallicFactor': 0, 'roughnessFactor': 1}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2, 'material': 0}]}],
        'nodes': [{'name': 'Independent analytic triangle', 'mesh': 0}],
        'scenes': [{'nodes': [0]}], 'scene': 0,
    }
    return glb(document, data)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def need(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('--output must be new')
    run = args.output.resolve(); run.mkdir(parents=True)
    world = run / 'fbx.world.json'
    record = dict(passed=False, binary_sha256=sha(args.binary), source_sha256=sha(Path(__file__)),
                  fixture_sha256=sha(Path(__file__).with_name('fbx_fixture.py')), gpu_index=args.gpu,
                  samples=args.samples, time=TIME, calls=[], owners=[], captures={}, comparisons={})
    owner = None

    def native(path):
        path = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', path], text=True, timeout=10).strip() if args.windows_interop else path

    def call(method, params=None, error=None):
        params = {} if params is None else params
        row = dict(method=method, params=params); record['calls'].append(row)
        try:
            result = owner.call(method, params, timeout=150)
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=str(failure))
            need(error is not None and failure.code == error, 'Unexpected RPC rejection: '+str(failure))
            return None
        row['result'] = result
        need(error is None, 'Expected rejection: '+method)
        return result

    def uid(value):
        return f'{value:032x}'

    def component(entity, kind, value):
        return dict(op='component.set', id=uid(entity), type=kind, value=value)

    def capture(name, method, params, mode=None):
        path = run / (name+'.bmp')
        before = world.read_bytes()
        request = dict(camera=uid(1), path=native(path), width=WIDTH, height=HEIGHT,
                       gpu=args.gpu, samples=args.samples, profile=True, **params)
        report = call(method, request)
        need(world.read_bytes() == before, 'Capture changed authored world bytes')
        need(report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0,
             'Capture did not complete on Vulkan hardware')
        need(report['object_count'] == 1 and report['samples'] == args.samples, 'Unexpected rendered scene/AA mode')
        if mode is not None:
            need(report['animation']['skinning'] == ('gpu_compute' if mode == 'gpu' else 'cpu_reference'),
                 'Requested skinning path was not exercised')
            count = report['render_diagnostics']['last_draws']['skinned_instances']
            need(count == (1 if mode == 'gpu' else 0), 'Wrong GPU skinning work count')
            if mode == 'gpu':
                need(report['render_diagnostics']['last_draws']['skinned_vertices'] == 3, 'Wrong vertex work count')
                need(report['render_diagnostics']['gpu']['skinning']['samples'] >= 1, 'No GPU skinning timing samples')
        record['captures'][name] = dict(report=report, sha256=sha(path))
        return report

    def compare(name, left, right, limit=64):
        a, b = pixels(run/(left+'.bmp')), pixels(run/(right+'.bmp'))
        need(len(a) == HEIGHT and len(b) == HEIGHT and all(len(row) == WIDTH for row in a+b), 'Unexpected image extent')
        errors = [max(abs(x-y) for x, y in zip(p, q)) for ar, br in zip(a, b) for p, q in zip(ar, br)]
        result = dict(maximum_channel_error=max(errors), pixels_over_2=sum(v > 2 for v in errors),
                      changed_pixels=sum(v != 0 for v in errors), mean_max_channel_error=sum(errors)/len(errors))
        record['comparisons'][name] = result
        if limit is not None:
            need(result['pixels_over_2'] <= limit and result['mean_max_channel_error'] <= .1,
                 'Rendered comparison failed: '+name+' '+str(result))
        return result

    try:
        sources = write_fixtures(run/'sources')
        reference = sources/'independent.glb'; reference.write_bytes(analytic_triangle())
        owner = WorldClient.open(str(args.binary.resolve()), native(world))
        imported = call('asset.import', dict(source=native(sources/'animated.fbx')))
        binary = call('asset.import', dict(source=native(sources/'binary/animated.fbx')))
        need(imported['asset'] == binary['asset'], 'Equivalent original ASCII/binary cooked identities differ')
        composed = call('asset.import', dict(source=native(sources/'skin.fbx'),
                        animations=[native(sources/'meter_move_donor.fbx'), native(sources/'turn_donor.fbx')]))
        reference_asset = call('asset.import', dict(source=native(reference)))['asset']
        asset = imported['asset']
        clips = call('asset.inspect', dict(asset=asset, section='animations'))['items']
        move = next(row['index'] for row in clips if row['name'] == 'Move')
        call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=[
            dict(op='entity.create', id=uid(1), name='Camera'),
            component(1, 'Transform', dict(position=[2.6, .5, 3], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            component(1, 'Camera', dict(vertical_fov=40, near=.1, far=100)),
            dict(op='asset.instantiate', id=uid(100), name='Independent analytic triangle', asset=reference_asset)]))
        before = world.read_bytes()
        shutil.rmtree(sources)
        need(not sources.exists(), 'Owned source removal failed')
        previous = owner; owner = None
        try:
            previous.close()
        finally:
            record['owners'].append(dict(exit_code=previous.transport.returncode))
        need(previous.transport.returncode == 0, 'Import owner did not exit cleanly')
        owner = WorldClient.open(str(args.binary.resolve()), native(world))
        need(world.read_bytes() == before, 'Fresh capture owner changed authored bytes')
        inspected = call('world.inspect'); history = call('world.history')
        pose = dict(revision=1, asset=asset, clip=move, time=TIME)
        capture('rest-gpu', 'asset.animation.capture', dict(pose, time=0, skinning='gpu'), 'gpu')
        capture('posed-gpu', 'asset.animation.capture', dict(pose, skinning='gpu'), 'gpu')
        capture('posed-cpu', 'asset.animation.capture', dict(pose, skinning='cpu'), 'cpu')
        capture('posed-unculled', 'asset.animation.capture', dict(pose, skinning='gpu', culling=False), 'gpu')
        capture('analytic', 'world.capture', dict(revision=1))
        compare('gpu_vs_cpu', 'posed-gpu', 'posed-cpu')
        compare('gpu_vs_independent_triangle', 'posed-gpu', 'analytic')
        compare('culling', 'posed-gpu', 'posed-unculled', 0)
        motion = compare('visible_motion', 'rest-gpu', 'posed-gpu', None)
        need(motion['pixels_over_2'] > 1000, 'Skin animation has no meaningful visible motion')
        stale = run/'stale.bmp'
        call('world.capture', dict(revision=0, camera=uid(1), path=native(stale), gpu=args.gpu), error=-32009)
        need(not stale.exists(), 'Stale request published a capture')
        need(world.read_bytes() == before and call('world.inspect') == inspected and call('world.history') == history,
             'Read-only preview changed authored state/history')

        # Replace only the explicitly-owned analytic reference with the cooked
        # composed rig. This is the ordinary instantiated runtime consumer.
        call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1, ops=[
            dict(op='entity.delete', id=uid(100), recursive=True),
            dict(op='asset.instantiate', id=uid(200), name='Runtime FBX rig', asset=composed['asset'])]))
        before = world.read_bytes(); inspected = call('world.inspect'); history = call('world.history')
        clips = call('asset.inspect', dict(asset=composed['asset'], section='animations'))['items']
        move = next(row['index'] for row in clips if row['name'] == 'Move')
        nodes = call('asset.inspect', dict(asset=composed['asset'], section='nodes'))['items']
        child = next(row['index'] for row in nodes if row['name'] == 'Child')
        child_id = hashlib.sha256(f'poima.instance.v1/{uid(200)}/node/{child}'.encode()).hexdigest()[:32]
        session = uid(900)
        call('runtime.start', dict(session_id=session, revision=2))
        step = call('runtime.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=30,
                    animations=[dict(entity=uid(200), clip=move, time=0, speed=1, loop=False, playing=True)]))
        need(step['tick'] == 30, 'Runtime did not advance 30 fixed ticks')
        joint = call('runtime.entity', dict(session_id=session, tick=30, id=child_id))
        need(abs(joint['local_transform']['position'][0]-TIME) < 2e-5 and
             abs(joint['local_transform']['position'][1]-1) < 2e-5, 'Runtime did not apply metric FBX animation')
        runtime_report = capture('runtime-composed', 'runtime.capture', dict(session_id=session, tick=30))
        need(runtime_report['render_diagnostics']['last_draws']['skinned_instances'] == 1, 'Runtime did not use GPU skinning')
        compare('runtime_vs_independent_triangle', 'runtime-composed', 'analytic')
        compare('runtime_vs_asset_preview', 'runtime-composed', 'posed-gpu')
        call('runtime.stop', dict(session_id=session))
        need(world.read_bytes() == before and call('world.inspect') == inspected and call('world.history') == history,
             'Runtime animation changed authored state/history')
        need(sha(args.binary) == record['binary_sha256'], 'Host binary changed during capture')
        record.update(passed=True, source_independent=True, authored_state_preserved_by_capture_and_runtime=True,
                      runtime_ticks=30, cpu_reference=True, independent_static_geometry=True,
                      limits=['No C# gameplay execution, physical input, editor or performance qualification.'])
    except BaseException as error:
        record['error'] = repr(error)
    finally:
        if owner is not None:
            try:
                owner.close()
            except BaseException as error:
                record['passed'] = False; record['close_error'] = repr(error)
            code = owner.transport.returncode; record['owners'].append(dict(exit_code=code))
            if code != 0:
                record['passed'] = False
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(run/'evidence.json')
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
