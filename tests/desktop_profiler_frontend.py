#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real native-backed Profiler window controls; isolated projects and owned visual-content renders."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('editor', 'binary', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    assert os.name == 'nt', 'Use native Windows Python for the packaged Avalonia editor.'
    for name in ('editor', 'binary', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    run = args.output/uuid.uuid4().hex; run.mkdir(parents=True)
    record = dict(passed=False, gpu=args.gpu, test_sha256=sha(__file__), binary_sha256=sha(args.binary),
                  frontend_sha256=sha(args.editor.parent/'Poima.Editor.dll'),
                  bridge_sha256=sha(args.editor.parent/'poima_desktop.dll'), checks=[],
                  limitations=['Routed actual control events and native service calls; not physical input or screen-reader qualification.',
                               'PNG is attached Avalonia visual content, not an OS screenshot.',
                               'Timing values establish instrumentation, not performance targets or low overhead.'])
    process = None
    try:
        project = run/'Profiler project'
        created = subprocess.run([str(args.binary), 'project', 'create', str(project), '--name', 'Profiler qualification'],
                                 text=True, capture_output=True, encoding='utf-8', timeout=30)
        assert created.returncode == 0, created.stdout+created.stderr
        world = project/'world.json'; original = sha(world)
        revision = json.loads(world.read_text())['revision']
        actions = []; frame = 3
        def add(op, advance=3, **fields):
            nonlocal frame
            actions.append(dict(frame=frame, op=op, **fields)); frame += advance
        def press(name, **fields): add('click_control', control='Profiler '+name, **fields)
        def observe(tag): add('inspect_profiler', tag=tag)
        def rpc(method, params, **fields): add('rpc', method=method, params=params, **fields)
        session = uuid.uuid4().hex
        add('reset_layout', split_views=True)
        add('wait_scene_error', expected=False, view='scene'); add('wait_scene_error', expected=False, view='game')
        add('open_profiler'); observe('initial'); press('Record'); observe('recording')
        rpc('desktop.play.start', dict(revision=revision, session_id=session, paused=True))
        rpc('desktop.play.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=240))
        frame += 15
        press('Stop'); add('wait_profiler'); observe('loaded')
        add('profiler_select_cpu', tag='selected')
        add('choose_control', control='Profiler Sort', choice='P95')
        add('choose_control', control='Profiler Source', choice='editor_scene')
        add('choose_control', control='Profiler Source', choice='All CPU sources')
        trace = run/'capture.json'; add('profiler_export', path=str(trace), tag='export')
        add('profiler_export', path=str(trace), error_contains='already exists', tag='no_overwrite')
        add('render_profiler', path=str(run/'profiler.png'), tag='visual')
        # Replace the capture outside the GUI model and attempt a stale Record in
        # the same semantic frame, before status polling can adopt the replacement.
        empty = uuid.uuid4().hex
        add('profiler_external_start', capture_id=empty, advance=0)
        rpc('profiler.stop', dict(capture_id=empty), advance=0)
        press('Record', advance=0); observe('stale_record')
        press('Review current capture'); press('Refresh'); add('wait_profiler'); observe('empty_loaded')
        full = uuid.uuid4().hex
        add('profiler_external_start', capture_id=full, capacity=64)
        frame += 12
        press('Refresh'); add('wait_profiler'); observe('full_loaded')
        add('profiler_export', path=str(run/'full.json'), tag='full_export')
        # Tool lifetime is not recorder ownership: closing it must not stop a
        # capture created through the common native authority.
        external = uuid.uuid4().hex
        add('profiler_external_start', capture_id=external, advance=0)
        add('close_profiler'); frame += 6
        rpc('profiler.status', {}, tag='closed_recording')
        add('open_profiler'); press('Refresh'); observe('reopened_recording')
        press('Stop'); add('wait_profiler'); observe('reopened_loaded')
        rpc('desktop.play.stop', dict(session_id=session))
        add('inspect', tag='final')
        script = run/'actions.json'; script.write_text(json.dumps(dict(actions=actions), indent=2)+'\n')
        report = run/'report.json'
        command = [str(args.editor), str(world), '--gpu', str(args.gpu), '--endpoint', 'profiler-ui-'+uuid.uuid4().hex,
                   '--layout', str(run/'layout.json'), '--script', str(script), '--frames', str(frame+80), '--report', str(report)]
        with (run/'stdout.txt').open('w', encoding='utf-8') as out, (run/'stderr.txt').open('w', encoding='utf-8') as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
            process.wait(timeout=150)
        data = json.loads(report.read_text()) if report.exists() else None
        record.update(exit_code=process.returncode, report=data)
        assert process.returncode == 0 and data is not None and data['success'], (data, (run/'stderr.txt').read_text())
        assert len(data['actions']) == len(actions)
        tags = {spec['tag']: result for spec, result in zip(actions, data['actions']) if 'tag' in spec}
        initial = tags['initial']['profiler']; recording = tags['recording']['profiler']; loaded = tags['loaded']['profiler']
        assert initial['status']['state'] == 'empty' and initial['loaded_events'] == 0
        assert recording['status']['state'] == 'recording'
        assert loaded['loaded_capture'] == recording['status']['capture_id'] and loaded['loaded_events'] > 1024 and not loaded['loading']
        assert loaded['status']['open'] == 0 and loaded['status']['state'] == 'stopped'
        groups = loaded['summary']['groups']
        assert {'editor_scene', 'editor_game', 'editor_poll'} <= {row['source'] for row in groups}
        assert any(row['kind'] == 'gpu' and row['unit'] == 'ns' and row['samples'] > 0 for row in groups)
        assert any(row['kind'] == 'cpu' and row['samples'] > 0 for row in groups)
        for group in groups:
            assert group['min'] <= group['p50'] <= group['p95'] <= group['p99'] <= group['max']
            assert group['min'] <= group['mean'] <= group['max']
            assert group['min'] <= group['last'] <= group['max']
            if group['kind'] == 'counter':
                assert group['unit'] == ('bytes' if 'bytes' in group['name'] else 'count')
        selected = tags['selected']['profiler']['selected']
        assert selected['kind'] == 'cpu' and selected['id'] > 0 and selected['duration_ns'] >= 0
        exported = json.loads(trace.read_text())
        assert exported['poima']['capture_id'] == loaded['loaded_capture'] and exported['poima']['count'] == loaded['loaded_events']
        actual = [event for event in exported['traceEvents'] if event['ph'] != 'M']
        assert len(actual) == loaded['loaded_events']
        chosen = next(event for event in actual if event.get('args', {}).get('event_id') == selected['id'])
        assert chosen['name'] == selected['name'] and chosen['dur'] == selected['duration_ns']/1000
        assert any(event['ph'] == 'C' and 'duration_ns' in event['args'] for event in actual), 'GPU durations must not become CPU-aligned spans.'
        assert tags['no_overwrite']['expected_error'] and tags['export']['export']['bytes'] == trace.stat().st_size
        record['checks'].append('Actual controls record native fixed ticks and both viewport sources; summary percentiles, CPU selection and exact native export agree.')
        stale = tags['stale_record']['profiler']
        assert stale['error'] and 'changed' in stale['error'] and stale['pending_start'] is not None
        assert stale['loaded_capture'] == loaded['loaded_capture'] and stale['loaded_events'] == loaded['loaded_events']
        empty_loaded = tags['empty_loaded']['profiler']
        assert empty_loaded['loaded_capture'] == empty and empty_loaded['loaded_events'] == 0 and empty_loaded['summary']['groups'] == []
        assert empty_loaded['pending_start'] is None and empty_loaded['error'] is None
        full_loaded = tags['full_loaded']['profiler']
        assert full_loaded['loaded_capture'] == full and full_loaded['status']['state'] == 'full'
        assert full_loaded['loaded_events'] == 64 and full_loaded['status']['open'] == 0 and full_loaded['status']['dropped'] >= 1
        assert not full_loaded['status']['recording']
        assert tags['closed_recording']['result']['capture_id'] == external and tags['closed_recording']['result']['state'] == 'recording'
        assert tags['reopened_recording']['profiler']['status']['capture_id'] == external
        assert tags['reopened_loaded']['profiler']['loaded_capture'] == external
        assert not tags['reopened_loaded']['profiler']['stale'] and tags['reopened_loaded']['profiler']['error'] is None
        record['checks'].append('Stale Record preserves the loaded capture and pending request; empty/full/replaced captures load explicitly; closing the tool preserves native recording.')
        assert sha(world) == original
        assert tags['final']['native']['runtime']['active'] is False
        assert tags['visual']['visual']['kind'] == 'avalonia_visual_content'
        assert (run/'profiler.png').stat().st_size > 1000
        record.update(passed=True, action_count=len(actions), trace_sha256=sha(trace), visual_sha256=sha(run/'profiler.png'))
    except BaseException as error:
        record['error'] = repr(error)
        if process is not None and process.poll() is None:
            process.kill(); process.wait(timeout=10)
        raise
    finally:
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
        print(run/'evidence.json')


if __name__ == '__main__':
    main()
