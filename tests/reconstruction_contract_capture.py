#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enabled reconstruction RPC contract: six bounded captures, no quality/performance claim."""
import argparse, hashlib, json, math, subprocess, traceback, uuid
from pathlib import Path
from texture_fixture import quad
from animation_fixture import ribbon
from gltf_fixture import glb


def check(ok, why):
    if not ok:
        raise RuntimeError(str(why))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--gpu', type=int, default=0)
    p.add_argument('--windows-interop', action='store_true')
    a = p.parse_args()
    run = a.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    world = run/'world.json'
    camera, sid = uuid.uuid4().hex, uuid.uuid4().hex
    modes = {'fsr3_native': 1., 'fsr3_quality': 1.5, 'fsr3_balanced': 1.7, 'fsr3_performance': 2.}
    sources = [Path(__file__), *(Path(__file__).with_name(n) for n in ('texture_fixture.py', 'animation_fixture.py', 'gltf_fixture.py'))]
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {'passed': False, 'runs': [], 'captures': [], 'binary_sha256': sha(a.binary),
              'source_sha256': {x.name: sha(x) for x in sources}, 'gpu_index': a.gpu,
              'limitations': ['Six small enabled Vulkan captures; no performance, image-quality or physical-input qualification.',
                              'All four modes exercised through world.capture; runtime and animation each exercise one mode.',
                              'runtime.play is checked for option rejection only, not successful interactive playback.']}

    def native(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if a.windows_interop else str(path.resolve())

    def batch(requests):
        rows = [{'jsonrpc': '2.0', 'id': i, 'method': m, 'params': params} for i, (m, params) in enumerate(requests)]
        entry = {'requests': rows}
        report['runs'].append(entry)
        r = subprocess.run([str(a.binary.resolve()), 'world', native(world)], input=''.join(json.dumps(x)+'\n' for x in rows),
                           capture_output=True, text=True, encoding='utf-8', timeout=240)
        entry.update(exit_code=r.returncode, stdout=r.stdout, stderr=r.stderr)
        check(r.returncode == 0, entry)
        replies = [json.loads(x) for x in r.stdout.splitlines()]
        check(len(replies) == len(rows), 'Response count')
        for request, reply in zip(rows, replies):
            check(reply.get('jsonrpc') == '2.0' and reply.get('id') == request['id'], reply)
        return replies

    def good(reply):
        check('result' in reply, reply)
        return reply['result']

    def capture(name, mode, frames, method='world.capture', extra=None):
        rw, rh = int(320/modes[mode]), int(240/modes[mode])
        probes = [{'x': rw//2, 'y': rh//2}, {'x': 0, 'y': 0}, {'x': rw-1, 'y': rh-1}, {'x': rw//2, 'y': rh//2}]
        params = {'revision': 1, 'camera': camera, 'path': native(run/(name+'.bmp')), 'width': 320, 'height': 240,
                  'gpu': a.gpu, 'samples': 1, 'reconstruction': mode, 'capture_frames': frames, 'scene_product_probes': probes}
        if method == 'runtime.capture':
            params.pop('revision')
            params.update(session_id=sid, tick=3)
        params.update(extra or {})
        return method, params

    def validate(request, reply):
        result = good(reply)
        method, params = request
        mode, frames = params['reconstruction'], params['capture_frames']
        rw, rh = int(320/modes[mode]), int(240/modes[mode])
        check(result['hardware'] and result['capture_written'] and result['nvrhi_errors'] == 0, result)
        check(result['samples'] == 1 and result['frames_presented'] == frames, result)
        check(report.setdefault('gpu_name', result['gpu']) == result['gpu'], 'Device changed')
        d = result['render_diagnostics']['reconstruction']
        check(d['active'] and d['mode'] == mode, d)
        check([d['render_width'], d['render_height'], d['output_width'], d['output_height']] == [rw, rh, 320, 240], d)
        check(d['history_sequence'] == frames and d['history_reset'] is (frames == 1), d)
        check(all(math.isfinite(x) and abs(x) <= .5 for x in d['jitter_pixels']), d)
        product = result['render_diagnostics']['scene_products']
        check(product['available'] and product['view'] == 'color', product)
        check(len(product['probes']) == len(params['scene_product_probes']), product)
        for point, sample in zip(params['scene_product_probes'], product['probes']):
            x, y = point['x'], point['y']
            check([sample['x'], sample['y']] == [x, y], sample)
            check([sample['resolved_x'], sample['resolved_y']] == [int((x+.5)*320/rw), int((y+.5)*240/rh)], sample)
            check(all(math.isfinite(v) for key in ('raw_hdr', 'resolved_hdr', 'motion', 'shading_normal') for v in sample[key]), sample)
        check(product['probes'][0] == product['probes'][3], 'Duplicate probe changed')
        if method == 'runtime.capture':
            check(result['tick'] == 3 and result['session_id'] == sid, result)
        if method == 'asset.animation.capture':
            check(result['animation']['requested_time'] == 1 and result['source'] == 'asset_animation', result)
        report['captures'].append(result)

    try:
        c = subprocess.run([str(a.binary.resolve()), 'capabilities'], capture_output=True, text=True, timeout=20)
        report['capabilities_process'] = {'exit_code': c.returncode, 'stdout': c.stdout, 'stderr': c.stderr}
        check(c.returncode == 0 and json.loads(c.stdout)['result']['features']['fsr3_upscaler'], 'Requires enabled FSR build')
        desc = good(batch([('world.describe', {})])[0])
        check(desc['schema_revision'] == 49, desc['schema_revision'])
        for method in ('world.capture', 'runtime.capture', 'asset.animation.capture', 'runtime.play'):
            check(desc['methods'][method]['properties']['reconstruction']['enum'] == ['none', *modes], method)
        plane, rig = run/'plane.glb', run/'rig.glb'
        doc, blob = quad([], {'pbrMetallicRoughness': {'baseColorFactor': [.5, .5, .5, 1]}, 'emissiveFactor': [.5, .25, .125], 'doubleSided': True})
        plane.write_bytes(glb(doc, blob))
        doc, blob = ribbon()
        rig.write_bytes(glb(doc, blob))
        assets = [good(x)['asset'] for x in batch([('asset.import', {'source': native(x)}) for x in (plane, rig)])]
        ops = [{'op': 'entity.create', 'id': camera, 'name': 'Observer'},
               {'op': 'component.set', 'id': camera, 'type': 'Transform', 'value': {'position': [0, 0, 4], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}},
               {'op': 'component.set', 'id': camera, 'type': 'Camera', 'value': {'vertical_fov': 60, 'near': .1, 'far': 20}},
               {'op': 'asset.instantiate', 'id': uuid.uuid4().hex, 'name': 'Plane', 'asset': assets[0]}]
        good(batch([('world.transact', {'base_revision': 0, 'request_id': uuid.uuid4().hex, 'ops': ops})])[0])
        authored = world.read_bytes()
        requests = [capture(mode, mode, frames) for mode, frames in zip(modes, (1, 2, 3, 4))]
        requests.append(capture('animation', 'fsr3_quality', 3, 'asset.animation.capture', {'asset': assets[1], 'clip': 0, 'time': 1, 'loop': False, 'skinning': 'gpu'}))
        for request, reply in zip(requests, batch(requests)):
            validate(request, reply)
        runtime_capture = capture('runtime', 'fsr3_balanced', 5, 'runtime.capture')
        observe = [('runtime.inspect', {'session_id': sid}), ('world.inspect', {})]
        requests = [('runtime.start', {'session_id': sid, 'revision': 1}),
                    ('runtime.step', {'session_id': sid, 'request_id': uuid.uuid4().hex, 'expected_tick': 0, 'ticks': 3}), *observe, runtime_capture, *observe]
        invalid = []
        for method, base in [capture('bad-world', 'fsr3_performance', 2), capture('bad-runtime', 'fsr3_performance', 2, 'runtime.capture'),
                             capture('bad-animation', 'fsr3_performance', 2, 'asset.animation.capture', {'asset': assets[1], 'clip': 0, 'time': 1})]:
            for change, code in [({'samples': 4}, -32602), ({'scene_debug_view': 'depth'}, -32602),
                                 ({'scene_product_probes': [{'x': 160, 'y': 10}]}, -32020)]:
                invalid.append((len(requests), code))
                requests += [(method, {**base, **change}), *observe]
        invalid.append((len(requests), -32602))
        requests += [('runtime.play', {'session_id': sid, 'request_id': uuid.uuid4().hex, 'expected_tick': 3, 'camera': camera,
                                       'mode': 'interactive', 'reconstruction': 'fsr3_native', 'samples': 1, 'capture_frames': 2}), *observe]
        replies = batch(requests)
        for reply in replies[:4]: good(reply)
        baseline = [good(x) for x in replies[2:4]]
        validate(runtime_capture, replies[4])
        check([good(x) for x in replies[5:7]] == baseline, 'Capture changed authoritative state')
        for index, code in invalid:
            reply = replies[index]
            check(reply.get('error', {}).get('code') == code, reply)
            if code == -32020:
                check('probe' in reply['error']['message'].lower(), reply)
            check([good(x) for x in replies[index+1:index+3]] == baseline, 'Rejected request mutated state')
        check(world.read_bytes() == authored, 'Capture changed authored document')
        check(not any(run.glob('bad-*.bmp')), 'Rejected capture published an image')
        report.update(passed=True, rejection_checks=len(invalid), authored_unchanged=True, runtime_tick_unchanged=3)
    except BaseException:
        report['error'] = traceback.format_exc()
    finally:
        report['inputs_unchanged'] = report['binary_sha256'] == sha(a.binary) and all(sha(x) == report['source_sha256'][x.name] for x in sources)
        report['passed'] = report['passed'] and report['inputs_unchanged']
        (run/'evidence.json').write_text(json.dumps(report, indent=2)+'\n')
    check(report['passed'], run/'evidence.json')
    print(json.dumps({'passed': True, 'evidence': str(run/'evidence.json')}))


if __name__ == '__main__':
    main()
