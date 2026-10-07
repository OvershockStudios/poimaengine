#!/usr/bin/env python3
"""Qualify native profiler observations against actual Vulkan pixels and replay.

Tracing is deliberately independent from legacy RenderOptions.profile. This is
a correctness test of instrumentation, not a frame-time performance benchmark.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import traceback
import uuid

from scene_capture import pixels


def uid(value):
    return f'{value:032x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    args = parser.parse_args()
    args.output = args.output.resolve()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    world = run / 'world.json'
    evidence = {'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'gpu_index': args.gpu, 'requests': [], 'responses': [], 'checks': [],
                'performance_qualified': False, 'physical_input_qualified': False}

    def native(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

    stderr = (run / 'stderr.log').open('w', encoding='utf-8')
    process = subprocess.Popen([str(args.binary.resolve()), 'world', native(world)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                               text=True, encoding='utf-8', bufsize=1)
    replies = queue.Queue()

    def read():
        for line in process.stdout:
            replies.put(line)
        replies.put(None)

    threading.Thread(target=read, daemon=True).start()

    def call(method, params):
        request = {'jsonrpc': '2.0', 'id': len(evidence['requests']) + 1,
                   'method': method, 'params': params}
        evidence['requests'].append(request)
        process.stdin.write(json.dumps(request) + '\n')
        process.stdin.flush()
        line = replies.get(timeout=180)
        assert line is not None, f'Engine exited during {method}'
        response = json.loads(line)
        evidence['responses'].append(response)
        assert response.get('id') == request['id'] and 'result' in response, response
        return response['result']

    current_capture = None

    def start():
        nonlocal current_capture
        wanted = uuid.uuid4().hex
        result = call('profiler.start', {'capture_id': wanted,
                      'expected_capture_id': current_capture, 'capacity': 16384})
        assert result['state'] == 'recording' and result['count'] == 0, result
        current_capture = wanted

    def seal(expected_source, frame_count, session=None):
        stopped = call('profiler.stop', {'capture_id': current_capture})
        assert stopped['state'] == 'stopped' and stopped['open'] == 0 and stopped['dropped'] == 0, stopped
        events, offset = [], 0
        while True:
            page = call('profiler.events', {'capture_id': current_capture, 'offset': offset, 'limit': 73})
            assert page['offset'] == offset and page['total'] == stopped['count'], page
            events.extend(page['events'])
            if page['next_offset'] is None:
                break
            offset = page['next_offset']
        assert len(events) == stopped['count'] and events
        by_id = {event['id']: event for event in events}
        assert len(by_id) == len(events)
        for event in events:
            assert not event['failed'] and not event['name_truncated'], event
            assert event['start_ns'] >= 0 and event['duration_ns'] >= 0
            if event['parent']:
                parent = by_id[event['parent']]
                assert parent['kind'] == 'cpu' and parent['id'] < event['id'], (parent, event)
                assert parent['start_ns'] <= event['start_ns'], (parent, event)
                assert event['start_ns'] + event['duration_ns'] <= parent['start_ns'] + parent['duration_ns'], (parent, event)

        observed = [e for e in events if e['source'] == expected_source]
        names = {e['name'] for e in observed}
        assert {'renderer.prepare', 'render.frame', 'render.acquire', 'render.record',
                'render.submit', 'render.present', 'render.wait',
                'renderer.capture_readback_write', 'renderer.camera_draws',
                'renderer.camera_triangles', 'renderer.texture_payload_bytes'} <= names, names
        gpu = [e for e in observed if e['kind'] == 'gpu']
        for name in ('skinning', 'light_assignment', 'shadows', 'opaque', 'post', 'total'):
            samples = [e for e in gpu if e['name'] == f'gpu.{name}.ns']
            assert len(samples) == frame_count, (name, frame_count, samples)
            assert all(e['duration_ns'] == 0 and isinstance(e['value'], int) and e['value'] >= 0 for e in samples)
            if name == 'total':
                assert all(e['value'] > 0 for e in samples)
        assert any(e['name'] == 'renderer.camera_triangles' and e['value'] > 0 for e in observed)
        assert any(e['name'] == 'renderer.camera_draws' and e['value'] > 0 for e in observed)
        if session is not None:
            assert all(e['session'] == session for e in gpu), gpu
            assert all(e['tick'] is not None for e in gpu), gpu

        summary = call('profiler.summary', {'capture_id': current_capture})
        groups = defaultdict(list)
        for event in events:
            groups[(event['name'], event['kind'], event['source'])].append(
                event['duration_ns'] if event['kind'] == 'cpu' else event['value'])
        assert len(summary['groups']) == len(groups)
        for group in summary['groups']:
            values = sorted(groups[(group['name'], group['kind'], group['source'])])
            assert group['samples'] == len(values)
            assert group['min'] == values[0] and group['max'] == values[-1]
            for percentile in (50, 95, 99):
                assert group[f'p{percentile}'] == values[(len(values)*percentile+99)//100-1]
        exported = call('profiler.export', {'capture_id': current_capture})
        trace = exported['trace']
        gpu_rows = [row for row in trace['traceEvents'] if row.get('cat') == 'gpu']
        assert len(gpu_rows) == len([e for e in events if e['kind'] == 'gpu'])
        assert all(row['ph'] == 'C' and 'dur' not in row and 'duration_ns' in row['args'] for row in gpu_rows)
        (run / f'{current_capture}.trace.json').write_text(json.dumps(trace, indent=2) + '\n')
        evidence.setdefault('captures', []).append({'status': stopped, 'summary': summary,
                                                   'events': events, 'trace': f'{current_capture}.trace.json'})
        return stopped

    def capture(name, samples):
        result = call('world.capture', {'revision': 1, 'camera': uid(200), 'gpu': args.gpu,
                      'samples': samples, 'profile': False, 'path': native(run / f'{name}.bmp')})
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, result
        assert not result['render_diagnostics']['profile_requested']
        return result

    try:
        fixture = json.loads((Path(__file__).resolve().parents[1] / 'examples/physics-room.jsonl').read_text())
        fixture['params']['ops'] += [
            {'op': 'entity.create', 'id': uid(300), 'name': 'Profiler sun', 'parent': None},
            {'op': 'component.set', 'id': uid(300), 'type': 'Light',
             'value': {'kind': 'directional', 'color': [1, .9, .8], 'intensity': 3,
                       'enabled': True, 'shadow': {'enabled': True, 'distance': 40}}},
            {'op': 'entity.create', 'id': uid(301), 'name': 'Profiler environment', 'parent': None},
            {'op': 'component.set', 'id': uid(301), 'type': 'LightingEnvironment',
             'value': {'ambient': [.1, .12, .14], 'exposure': 1, 'shadow_resolution': 512}}]
        call('world.transact', fixture['params'])
        original = world.read_bytes()
        for samples in (1, 4):
            off = capture(f'off-{samples}', samples)
            assert not off['render_diagnostics']['gpu']['available']
            start()
            on = capture(f'on-{samples}', samples)
            assert on['render_diagnostics']['gpu']['available']
            stopped = seal('request', on['frames_presented'])
            again = capture(f'stopped-{samples}', samples)
            assert not again['render_diagnostics']['gpu']['available']
            assert call('profiler.status', {}) == stopped
            reference = pixels(run / f'off-{samples}.bmp')
            assert reference == pixels(run / f'on-{samples}.bmp') == pixels(run / f'stopped-{samples}.bmp')
            evidence['checks'].append(f'{samples}x MSAA exact pixels with off/on/stopped tracing and real lazy GPU queries')

        sequence = [{'ticks': 8}, {'ticks': 4, 'move': [.25, 1]}, {'ticks': 1, 'look': [9, -3], 'jump': True}, {'ticks': 4}]
        ticks = sum(segment['ticks'] for segment in sequence)
        states = []
        for traced in (False, True):
            session = uid(900 + int(traced))
            call('runtime.start', {'session_id': session, 'revision': 1})
            if traced:
                start()
            result = call('runtime.play', {'session_id': session, 'request_id': uuid.uuid4().hex,
                          'expected_tick': 0, 'controller': uid(100), 'camera': uid(101),
                          'mode': 'replay', 'sequence': sequence, 'gpu': args.gpu,
                          'profile': False, 'path': native(run / f'player-{traced}.bmp')})
            assert result['success'] and result['tick'] == ticks and result['nvrhi_errors'] == 0, result
            assert result['capture_written'] and result['hardware']
            if traced:
                seal('player', result['frames_presented'], session)
            rows = []
            for entity in (2, 3, 100, 101):
                state = call('runtime.entity', {'session_id': session, 'id': uid(entity), 'tick': ticks})
                state.pop('session_id')
                rows.append(state)
            states.append(rows)
            call('runtime.stop', {'session_id': session})
        assert states[0] == states[1], states
        assert pixels(run / 'player-False.bmp') == pixels(run / 'player-True.bmp')
        assert world.read_bytes() == original
        evidence['checks'].append('traced replay preserves exact controller/body state and final pixels; GPU records carry player session/tick')
        evidence['checks'].append('sealed pages, parent intervals, independently recomputed percentiles, GPU-duration export semantics')
        call('session.close', {})
        process.stdin.close()
        assert process.wait(timeout=20) == 0
        evidence['passed'] = True
    except BaseException:
        evidence['passed'] = False
        evidence['error'] = traceback.format_exc()
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        stderr.close()
        (run / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print(json.dumps({'passed': evidence.get('passed', False), 'evidence': str(run / 'evidence.json'),
                          'checks': len(evidence['checks'])}))


if __name__ == '__main__':
    main()
