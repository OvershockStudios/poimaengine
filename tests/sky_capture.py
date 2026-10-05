#!/usr/bin/env python3
"""Image qualification of procedural sky on actual Vulkan hardware.

Metamorphic comparisons use identical camera/scene state where exact parity is
required. Sun location/diameter use independent pinhole geometry; exposure uses
scalar linear-light display conversion. No screenshots are synthesized.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import traceback
import uuid

WIDTH, HEIGHT = 640, 360


def uid(value):
    return f'{value:032x}'


def rotation(yaw=0, pitch=0):
    """Quaternion yaw(Y) * pitch(X), degrees."""
    y, p = math.radians(yaw)/2, math.radians(pitch)/2
    return [math.cos(y)*math.sin(p), math.sin(y)*math.cos(p),
            -math.sin(y)*math.sin(p), math.cos(y)*math.cos(p)]


def bitmap(path):
    """Compact RGB readback, preserving top-left pixel coordinates."""
    data = path.read_bytes()
    assert data[:2] == b'BM'
    offset = struct.unpack_from('<I', data, 10)[0]
    size, width, signed_height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    assert size >= 40 and planes == 1 and bits in (24, 32) and compression in (0, 3)
    assert width == WIDTH and abs(signed_height) == HEIGHT
    if compression == 3:
        assert struct.unpack_from('<III', data, 54) == (0xff0000, 0xff00, 0xff)
    stride = ((width*bits+31)//32)*4
    assert len(data) >= offset+stride*HEIGHT
    result = bytearray(WIDTH*HEIGHT*3)
    for y in range(HEIGHT):
        source_y = HEIGHT-1-y if signed_height > 0 else y
        row = data[offset+source_y*stride:offset+source_y*stride+WIDTH*(bits//8)]
        for c in range(3):
            result[y*WIDTH*3+c:(y+1)*WIDTH*3:3] = row[2-c::bits//8]
    return bytes(result)


def pixel(image, x, y):
    at = (y*WIDTH+x)*3
    return list(image[at:at+3])


def display(value):
    value = value/(1+value)
    return round(255*(12.92*value if value <= .0031308 else 1.055*value**(1/2.4)-.055))


def disk(image):
    points = [(i//3 % WIDTH, i//3//WIDTH) for i in range(0, len(image), 3)
              if min(image[i:i+3]) > 240]
    assert len(points) > 50, ('No sufficiently bright sun disk', len(points))
    return {'pixels': len(points), 'x': sum(x for x, _ in points)/len(points),
            'y': sum(y for _, y in points)/len(points)}


def expected_sun(yaw=180, pitch=-15, fov=60):
    # The disk follows transformed local +Z; project toward the -Z camera.
    y, p = math.radians(yaw), math.radians(pitch)
    dx, dy, dz = math.sin(y)*math.cos(p), -math.sin(p), math.cos(y)*math.cos(p)
    focal = HEIGHT/(2*math.tan(math.radians(fov)/2))
    return [WIDTH/2+focal*dx/-dz-.5, HEIGHT/2-focal*dy/-dz-.5]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    run = args.output/uuid.uuid4().hex
    run.mkdir(parents=True)
    record = {'passed': False, 'gpu_index': args.gpu, 'run_directory': str(run),
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'runs': [], 'checks': {}, 'images': {}}

    def native(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    def create(n, parent=None):
        return {'op': 'entity.create', 'id': uid(n), 'name': f'Sky fixture {n}', 'parent': uid(parent) if parent is not None else None}

    def component(n, kind, value):
        return {'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}

    def transform(n, position=(0, 0, 0), yaw=0, pitch=0, scale=(1, 1, 1)):
        return component(n, 'Transform', {'position': list(position), 'rotation': rotation(yaw, pitch), 'scale': list(scale)})

    def camera(fov=60):
        return component(1, 'Camera', {'vertical_fov': fov, 'near': .1, 'far': 100})

    def sky(**changes):
        return {'enabled': True, 'zenith': [.02, .12, .9], 'horizon': [.7, .1, .03],
                'ground': [.03, .7, .1], 'horizon_falloff': .35,
                'sun': None, 'sun_size_degrees': 8, 'sun_intensity': 32, **changes}

    def environment(value=None, exposure=1):
        return component(2, 'LightingEnvironment', {'ambient': [0, 0, 0], 'exposure': exposure,
                                                    **({'sky': value} if value is not None else {})})

    def light(enabled=True):
        return component(3, 'Light', {'kind': 'directional', 'color': [1, 1, 1], 'intensity': 0, 'enabled': enabled})

    try:
        for samples in (1, 4):
            rows, captures = [], {}
            revision = 0

            def request(method, params):
                index = len(rows)+1
                rows.append({'jsonrpc': '2.0', 'id': index, 'method': method, 'params': params})
                return index

            def edit(ops):
                nonlocal revision
                request('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': revision, 'ops': ops})
                revision += 1

            def capture(name, session=None, tick=0):
                path = run/f'{samples}x-{name}.bmp'
                params = {'camera': uid(1), 'path': native(path), 'width': WIDTH, 'height': HEIGHT,
                          'samples': samples, 'gpu': args.gpu}
                if session:
                    params.update(session_id=session, tick=tick)
                else:
                    params['revision'] = revision
                index = request('runtime.capture' if session else 'world.capture', params)
                captures[name] = (index, path)
                return index

            edit([create(1), camera(), create(2), environment()])
            capture('legacy')
            edit([environment(sky(enabled=False))])
            capture('disabled')
            edit([environment(sky())])
            capture('gradient')
            edit([transform(1, (1024, -231, 791))])
            capture('translated')
            edit([transform(1, yaw=77)])
            capture('yaw-invariant')
            edit([transform(1, pitch=90)])
            capture('zenith')
            edit([transform(1, pitch=-90)])
            capture('ground')
            edit([transform(1), camera(30)])
            capture('narrow')
            uniform = sky(zenith=[.2, .4, .8], horizon=[.2, .4, .8], ground=[.2, .4, .8])
            edit([camera(), environment(uniform)])
            capture('uniform-1')
            edit([environment(uniform, exposure=2)])
            capture('uniform-2')
            dark = sky(zenith=[.01]*3, horizon=[.01]*3, ground=[.01]*3)
            edit([environment(dark), create(3), transform(3, yaw=180, pitch=-15), light()])
            capture('sun-unreferenced')
            sun = {**dark, 'sun': uid(3)}
            edit([environment(sun)])
            capture('sun')
            edit([camera(90)])
            capture('sun-wide')
            edit([camera(), transform(3, yaw=200, pitch=-15)])
            capture('sun-rotated')
            edit([light(False)])
            capture('sun-disabled')
            edit([light(), transform(3, yaw=180, pitch=-15), create(4), transform(4, (0, 0, -3), scale=(1.4, 2, 1)),
                  component(4, 'MeshRenderer', {'primitive': 'box', 'visible': True, 'albedo': [0, 0, 0]}),
                  component(4, 'PbrMaterial', {'base_color': [0, 0, 0], 'emissive': [.5, 0, 0], 'metallic': 0,
                                               'roughness': 1, 'double_sided': False})])
            capture('occluded')
            edit([component(4, 'MeshRenderer', {'primitive': 'box', 'visible': False, 'albedo': [0, 0, 0]}),
                  create(10), component(10, 'CharacterController', {'radius': .3, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': uid(11)}),
                  create(11, 10), transform(11, (0, 1.6, 0)), component(11, 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
                  {'op': 'entity.reparent', 'id': uid(3), 'parent': uid(10), 'mode': 'keep_local'}])
            session = uid(900+samples)
            request('runtime.start', {'session_id': session, 'revision': revision})
            capture('runtime-initial', session)
            request('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'ticks': 1,
                                     'inputs': [{'entity': uid(10), 'look': [20, 0]}]})
            capture('runtime-rotated', session, 1)
            edit([environment(sun, exposure=0), transform(3, yaw=135, pitch=-15)])
            capture('runtime-frozen', session, 1)
            capture('authored-dark')
            player_path = run/f'{samples}x-player.bmp'
            player = request('runtime.play', {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 1,
                             'controller': uid(10), 'camera': uid(1), 'mode': 'replay', 'sequence': [{'ticks': 1}],
                             'width': WIDTH, 'height': HEIGHT, 'samples': samples, 'gpu': args.gpu, 'path': native(player_path)})
            captures['player'] = (player, player_path)
            capture('player-independent', session, 2)
            world = run/f'{samples}x.world.json'
            result = subprocess.run([str(args.binary.resolve()), 'world', native(world)],
                                    input=''.join(json.dumps(row)+'\n' for row in rows),
                                    capture_output=True, text=True, encoding='utf-8', timeout=240)
            evidence = {'samples': samples, 'requests': rows, 'stdout': result.stdout,
                        'stderr': result.stderr, 'exit_code': result.returncode}
            record['runs'].append(evidence)
            assert result.returncode == 0, result.stdout+result.stderr
            responses = {value['id']: value for value in map(json.loads, result.stdout.splitlines())}
            assert len(responses) == len(rows)
            for value in responses.values():
                assert 'result' in value, value
            images = {}
            for name, (index, path) in captures.items():
                report = responses[index]['result']
                assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
                assert report['samples'] == samples and report['width'] == WIDTH and report['height'] == HEIGHT, report
                images[name] = bitmap(path)
                record['images'][f'{samples}x-{name}'] = {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'report': report}
            assert images['legacy'] == images['disabled'], 'Disabled sky changed legacy background.'
            assert images['gradient'] == images['translated'], 'Sky translated with camera position.'
            # Rotated floating-point camera bases may differ by one display
            # quantization step; translation must remain bit-exact above.
            yaw_delta = max(abs(a-b) for a, b in zip(images['gradient'], images['yaw-invariant']))
            assert yaw_delta <= 1, ('Azimuth-symmetric gradient changed under yaw', yaw_delta)
            top, middle, bottom = [pixel(images['gradient'], WIDTH//2, y) for y in (10, HEIGHT//2, HEIGHT-11)]
            assert top[2] > top[0]+30 and bottom[1] > bottom[0]+30 and middle[0] > middle[1]+40, (top, middle, bottom)
            zenith, ground = pixel(images['zenith'], WIDTH//2, HEIGHT//2), pixel(images['ground'], WIDTH//2, HEIGHT//2)
            assert zenith[2] > zenith[0]+100 and ground[1] > ground[0]+100, (zenith, ground)
            assert max(abs(x-y) for x, y in zip(pixel(images['narrow'], WIDTH//2, 40), pixel(images['gradient'], WIDTH//2, 40))) > 8
            for exposure in (1, 2):
                expected = [display(c*exposure) for c in (.2, .4, .8)]
                actual = pixel(images[f'uniform-{exposure}'], WIDTH//2, HEIGHT//2)
                assert max(abs(a-b) for a, b in zip(actual, expected)) <= 2, (actual, expected)
                assert len({images[f'uniform-{exposure}'][i:i+3] for i in range(0, WIDTH*HEIGHT*3, 3)}) == 1
            assert images['sun-unreferenced'] == images['sun-disabled'], 'Disabled referenced light emitted a disk.'
            centers = {}
            for name, yaw, fov in [('sun', 180, 60), ('sun-wide', 180, 90), ('sun-rotated', 200, 60), ('runtime-rotated', 200, 60)]:
                observed, expected = disk(images[name]), expected_sun(yaw=yaw, fov=fov)
                assert abs(observed['x']-expected[0]) < 3 and abs(observed['y']-expected[1]) < 3, (name, observed, expected)
                centers[name] = {'observed': observed, 'expected': expected}
            # An 8-degree angular diameter at 15-degree elevation projects to
            # an ellipse; perspective area scales approximately sec(elevation)^3.
            radius = HEIGHT/(2*math.tan(math.radians(60)/2))*math.tan(math.radians(8)/2)
            expected_area = math.pi*radius*radius/math.cos(math.radians(15))**3
            assert .8*expected_area < centers['sun']['observed']['pixels'] < 1.2*expected_area
            ratio = centers['sun']['observed']['pixels']/centers['sun-wide']['observed']['pixels']
            assert 2.7 < ratio < 3.4, ('Angular disk does not follow FOV', ratio)
            sx, sy = [round(c) for c in expected_sun()]
            blocked = pixel(images['occluded'], sx, sy)
            assert blocked[0] > 100 and max(blocked[1:]) < 3, ('Sky overwrote foreground', blocked)
            for x, y in [(0, 0), (WIDTH-1, 0), (0, HEIGHT-1), (WIDTH-1, HEIGHT-1)]:
                assert pixel(images['occluded'], x, y) == pixel(images['sun'], x, y)
            assert images['runtime-initial'] == images['sun']
            # Jolt stores controller yaw as float; its live quaternion can
            # differ from the authored double-precision reference by ~1e-8.
            runtime_delta = max(abs(a-b) for a, b in zip(images['runtime-rotated'], images['sun-rotated']))
            assert runtime_delta <= 1, ('Live sun differs from authored orientation', runtime_delta)
            assert images['runtime-frozen'] == images['runtime-rotated']
            assert max(images['authored-dark']) == 0
            assert images['player'] == images['player-independent'] == images['runtime-frozen']
            assert responses[player]['result']['success'] and responses[player]['result']['tick'] == 2
            record['checks'][str(samples)] = {'legacy_disabled_exact': True, 'translation_invariant_exact': True,
                'yaw_max_channel_difference': yaw_delta, 'hemisphere_gradient': [top, middle, bottom], 'camera_pitch_fov': True, 'tone_exposure': True,
                'sun_projection': centers, 'sun_area_fov_ratio': ratio, 'sun_disabled_unreferenced_exact': True,
                'foreground_occlusion': blocked, 'runtime_authored_max_channel_difference': runtime_delta,
                'runtime_live_sun_and_frozen_settings': True, 'player_capture_exact': True}
        record['passed'] = True
    except Exception:
        record['error'] = traceback.format_exc()
        raise
    finally:
        (args.output/f'gpu-{args.gpu}.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({'passed': True, 'gpu': args.gpu, 'samples': [1, 4], 'evidence': str(args.output/f'gpu-{args.gpu}.json')}))


if __name__ == '__main__':
    main()
