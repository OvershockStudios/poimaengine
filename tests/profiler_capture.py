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
    parser.add_argument('--require-barriers', action='store_true')
    parser.add_argument('--require-submission', action='store_true')
    parser.add_argument('--require-gpu-passes', action='store_true')
    parser.add_argument('--reconstruct-rich', action='store_true', help='Use the optional FSR build for the detailed deferred capture.')
    args = parser.parse_args()
    if args.require_gpu_passes:
        args.require_submission = True
    elif args.reconstruct_rich:
        parser.error('--reconstruct-rich requires --require-gpu-passes')
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
    renderer_ids = set()

    def start(schedule=False):
        nonlocal current_capture
        wanted = uuid.uuid4().hex
        params = {'capture_id': wanted, 'expected_capture_id': current_capture, 'capacity': 16384}
        if args.require_gpu_passes:
            params['gpu_passes'] = schedule
        result = call('profiler.start', params)
        assert result['state'] == 'recording' and result['count'] == 0, result
        if args.require_gpu_passes:
            assert result['gpu_passes'] == schedule
        current_capture = wanted

    def seal(expected_source, result, session=None, extra_passes=(), capture_passes=()):
        frame_count = result['frames_presented']
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
                'render.submit', 'render.present', 'render.retire',
                'renderer.capture_readback_write', 'renderer.camera_draws',
                'renderer.camera_triangles', 'renderer.texture_payload_bytes'} <= names, names
        pass_order = ('skinning', 'light_assignment', 'shadows', 'scene_clear', 'sky',
                      'opaque', 'ambient_occlusion', 'ambient_occlusion_filter', 'deferred_lighting',
                      'screen_diffuse_bounce', 'screen_diffuse_resolve', 'resolve', 'auto_exposure',
                      'temporal_inputs', 'reconstruction', 'bloom_extract', 'bloom_horizontal', 'bloom_vertical',
                      'output', 'game_ui', 'overlay', 'editor_clear', 'editor_ui', 'product_probes',
                      'exposure_readback', 'capture', 'present')
        ranks = {f'render.pass.{name}': i for i, name in enumerate(pass_order)}
        recorded_passes = defaultdict(list)
        for event in observed:
            if event['name'].startswith('render.pass.'):
                assert event['kind'] == 'cpu' and event['name'] in ranks, event
                assert by_id[event['parent']]['name'] == 'render.record', event
                recorded_passes[event['parent']].append(event['name'])
        assert len(recorded_passes) == frame_count, recorded_passes
        required_passes = {f'render.pass.{name}' for name in
                           ('skinning', 'light_assignment', 'shadows', 'scene_clear', 'opaque', 'output', 'present', *extra_passes)}
        for sequence in recorded_passes.values():
            assert required_passes <= set(sequence), sequence
            assert len(set(sequence)) == len(sequence), sequence
            assert sequence == sorted(sequence, key=ranks.__getitem__), sequence
        if capture_passes:
            requested = {f'render.pass.{name}' for name in capture_passes}
            found = [sequence for sequence in recorded_passes.values() if 'render.pass.capture' in sequence]
            assert len(found) == 1 and requested <= set(found[0]), found
            assert all(not requested.intersection(sequence) for sequence in recorded_passes.values()
                       if 'render.pass.capture' not in sequence)
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

        diagnostics = result['render_diagnostics']
        execution = diagnostics['frame_execution']
        assert execution['limit'] == 2 and 0 < execution['peak_outstanding'] <= 2, execution
        assert execution['submitted'] == diagnostics['completed_submissions'] == frame_count and not execution['outstanding'], execution
        if args.require_submission:
            renderer = execution['renderer_id']
            assert type(renderer) is int and renderer > 0 and renderer not in renderer_ids, execution
            renderer_ids.add(renderer)
            tracked = [e for e in observed if 'render_submission' in e]
            assert all(e['render_submission']['renderer_id'] == renderer for e in tracked), tracked
            scopes = [e for e in tracked if e['name'] == 'render.submit']
            assert len(scopes) == frame_count, scopes
            submissions = [e['render_submission']['submission_id'] for e in scopes]
            assert all(type(value) is int and value > 0 for value in submissions)
            assert submissions == sorted(set(submissions)), submissions
            assert submissions[-1] == execution['last_completed_submission_id']
            for name in ('gpu.total.ns', 'renderer.completed_submissions'):
                rows = [e for e in observed if e['name'] == name]
                assert [e['render_submission']['submission_id'] for e in rows] == submissions, rows
            assert all(e['render_submission']['submission_id'] in submissions for e in gpu)
            for scope in scopes:
                owner = by_id[scope['parent']]
                assert owner['name'] == 'render.frame' and owner['render_submission'] == scope['render_submission']
            evidence.setdefault('renderer_submissions', []).append(
                {'renderer_id': renderer, 'submission_ids': submissions, 'source': expected_source})
        if args.require_gpu_passes:
            detailed = [e for e in observed if e['name'].startswith(('gpu.pass.', 'gpu.render_schedule.'))]
            if stopped['gpu_passes']:
                by_submission = defaultdict(list)
                assert all(e['kind'] == 'gpu' and e['parent'] == 0 and e['duration_ns'] == 0 and
                           type(e['value']) is int and e['value'] >= 0 for e in detailed)
                for row in detailed:
                    by_submission[row['render_submission']['submission_id']].append(row)
                assert list(by_submission) == submissions
                schedule_rows = []
                for scope in scopes:
                    parent = by_id[scope['parent']]
                    record, = [e for e in observed if e['name']=='render.record' and e['parent']==parent['id']]
                    serial = scope['render_submission']['submission_id']
                    rows = by_submission[serial]
                    names = [f'gpu.pass.{name.removeprefix("render.pass.")}.ns' for name in recorded_passes[record['id']]]
                    assert [row['name'] for row in rows] == names + ['gpu.render_schedule.total.ns']
                    total = rows[-1]['value']
                    assert total > 0 and 0 <= total - sum(row['value'] for row in rows[:-1]) < len(rows)
                    schedule_rows.append({'submission_id': serial, 'total_ns': total,
                                          'pass_ns': {row['name']: row['value'] for row in rows[:-1]}})
                evidence.setdefault('schedule_gpu_captures', []).append({'renderer_id': renderer, 'rows': schedule_rows})
            else:
                assert not detailed
        last_draws = diagnostics['last_draws']
        view_count = last_draws['shadow_views']
        lights = result['lighting']['lights']
        assert view_count == 4 and [(light['id'], light['kind']) for light in lights] == [(uid(300), 'directional')], lights
        assert not any(e['name'] == 'gpu.shadow_views.samples_dropped' for e in events), events
        metrics = ('draws', 'batches', 'triangles', 'light_index')
        timing_names = ['gpu.shadows.ns', 'gpu.shadow_clear.ns'] + [f'gpu.shadow_view.{i}.ns' for i in range(view_count)]
        work_names = [f'renderer.shadow_view.{i}.{metric}' for i in range(view_count) for metric in metrics]
        complete = 'renderer.completed_submissions'
        batches, pending = [], []
        for event in observed:
            name = event['name']
            if name in ('gpu.shadows.ns', 'gpu.shadow_clear.ns', complete) or name.startswith(('gpu.shadow_view.', 'renderer.shadow_view.')):
                pending.append(event)
            if name == complete:
                batches.append(pending)
                pending = []
        assert not pending and len(batches) == frame_count, (pending, frame_count, len(batches))
        totals = []
        for index, batch in enumerate(batches, 1):
            # Deferred counters share the submission ticket's time and context.
            # Retirement order disambiguates repeated replay ticks.
            assert [event['name'] for event in batch] == timing_names + work_names + [complete], batch
            assert batch[-1]['value'] == index, batch[-1]
            ticket = batch[-1]
            assert all(e['parent'] == 0 and e['start_ns'] == ticket['start_ns'] and
                       e['session'] == ticket['session'] and e['tick'] == ticket['tick'] and
                       e['duration_ns'] == 0 and type(e['value']) is int and e['value'] >= 0 for e in batch), batch
            assert all(e['kind'] == 'gpu' for e in batch[:len(timing_names)]), batch
            assert all(e['kind'] == 'counter' for e in batch[len(timing_names):]), batch
            values = {event['name']: event['value'] for event in batch}
            assert sum(values[name] for name in timing_names[1:]) <= values['gpu.shadows.ns'], values
            summed = dict.fromkeys(metrics[:-1], 0)
            for i in range(view_count):
                view = {metric: values[f'renderer.shadow_view.{i}.{metric}'] for metric in metrics}
                assert view['light_index'] == 0 and 0 <= view['batches'] <= view['draws'] <= view['triangles'], view
                assert bool(view['batches']) == bool(view['draws']), view
                for metric in summed:
                    summed[metric] += view[metric]
            totals.append(summed)
        assert totals[-1]['draws'] > 0 and totals[-1] == {field: last_draws[f'shadow_{field}'] for field in totals[-1]}, (totals[-1], last_draws)

        if args.require_barriers:
            prefix = 'renderer.barriers.'
            fields = {'calls', 'memory_entries', 'buffer_entries', 'image_entries'}
            barrier_batches, pending = [], []
            for event in observed:
                if event['name'].startswith(prefix):
                    pending.append(event)
                if event['name'] == complete:
                    barrier_batches.append((event, pending))
                    pending = []
            assert not pending and len(barrier_batches) == frame_count, barrier_batches
            barrier_totals = []
            for (ticket, batch), sequence in zip(barrier_batches, recorded_passes.values(), strict=True):
                assert all(event['kind'] == 'counter' and event['parent'] == 0 and
                           event['duration_ns'] == 0 and
                           all(event[key] == ticket[key] for key in ('start_ns', 'source', 'session', 'tick'))
                           for event in batch), batch
                values = {event['name'][len(prefix):]: event['value'] for event in batch}
                assert len(values) == len(batch) and fields <= values.keys(), values
                phases = values.keys() - fields
                assert phases <= {'before_passes', 'after_passes'} | {name.rsplit('.', 1)[1] for name in sequence}, values
                assert values['before_passes'] == values['memory_entries'] == 1, values
                assert values['opaque'] > 0 and values['shadows'] > 0, values
                assert values['buffer_entries'] > 0 and values['image_entries'] > 0, values
                assert sum(values[name] for name in phases) == values['calls'], values
                assert values['calls'] <= sum(values[name] for name in fields - {'calls'}), values
                barrier_totals.append(values)
            evidence.setdefault('barrier_captures', []).append(barrier_totals)

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
        if args.require_submission:
            exported_keys = {row['args']['event_id']: row['args']['render_submission']
                             for row in trace['traceEvents'] if 'render_submission' in row.get('args', {})}
            assert exported_keys == {e['id']: e['render_submission'] for e in events if 'render_submission' in e}
        (run / f'{current_capture}.trace.json').write_text(json.dumps(trace, indent=2) + '\n')
        evidence.setdefault('captures', []).append({'status': stopped, 'summary': summary,
                                                   'events': events, 'trace': f'{current_capture}.trace.json'})
        return stopped

    def capture(name, samples, **options):
        result = call('world.capture', {'revision': 1, 'camera': uid(200), 'gpu': args.gpu,
                      'samples': samples, 'frames_in_flight': 2, 'profile': False, 'path': native(run / f'{name}.bmp'), **options})
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
            start(schedule=args.require_gpu_passes and samples==4)
            on = capture(f'on-{samples}', samples)
            assert on['render_diagnostics']['gpu']['available']
            stopped = seal('request', on)
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
                start(schedule=args.require_gpu_passes)
            result = call('runtime.play', {'session_id': session, 'request_id': uuid.uuid4().hex,
                          'expected_tick': 0, 'controller': uid(100), 'camera': uid(101),
                          'mode': 'replay', 'sequence': sequence, 'gpu': args.gpu, 'frames_in_flight': 2,
                          'profile': False, 'path': native(run / f'player-{traced}.bmp')})
            assert result['success'] and result['tick'] == ticks and result['nvrhi_errors'] == 0, result
            assert result['capture_written'] and result['hardware']
            if traced:
                seal('player', result, session)
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
        evidence['checks'].append('two-slot submission tickets retain shadow clear and four cascade timings and reconcile view work at retirement')
        if args.require_barriers:
            evidence['checks'].append('Vulkan frame, pass and barrier-entry counts reconcile at retirement')
        if args.require_submission:
            evidence['checks'].append('each native submission joins its CPU scopes, delayed GPU samples, trace export and final retirement receipt')
        if args.require_gpu_passes:
            lighting = {'ambient': [.1, .12, .14], 'exposure': 1, 'shadow_resolution': 512,
                        'auto_exposure': {}, 'bloom': {'intensity': .3, 'threshold': .5, 'radius': 8},
                        'screen_diffuse_bounce': {'radius_m': 2, 'strength': .7, 'thickness_m': .1,
                                                  'resolution': 'half'}}
            call('world.transact', {'request_id': uid(10000), 'base_revision': 1, 'ops': [
                {'op': 'component.set', 'id': uid(301), 'type': 'LightingEnvironment', 'value': lighting}
            ]})
            rich_world = world.read_bytes()
            options = {'revision': 2, 'lighting_path': 'deferred', 'capture_frames': 4,
                       'ambient_occlusion': {'mode': 'gtao', 'quality': 'high', 'radius': 1},
                       'scene_product_probes': [{'x': 221, 'y': 173}]}
            stages = ('ambient_occlusion', 'ambient_occlusion_filter', 'deferred_lighting', 'screen_diffuse_bounce',
                      'screen_diffuse_resolve', 'auto_exposure', 'bloom_extract', 'bloom_horizontal',
                      'bloom_vertical')
            if args.reconstruct_rich:
                options['reconstruction'] = 'fsr3_native'
                stages += ('temporal_inputs', 'reconstruction')
            capture('rich-off', 1, **options)
            start(schedule=True)
            seal('request', capture('rich-on', 1, **options), extra_passes=stages,
                 capture_passes=('product_probes', 'exposure_readback'))
            assert pixels(run / 'rich-off.bmp') == pixels(run / 'rich-on.bmp')
            assert world.read_bytes() == rich_world
            evidence['checks'].append('ordered GPU durations cover every scheduled pass, with exact pixels and explicit opt-in')
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
