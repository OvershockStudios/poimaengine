#!/usr/bin/env python3
"""Explicit Vulkan capture of authoritative runtime UI; no desktop input claims."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / f'gpu{args.gpu}-msaa{args.samples}-{uuid.uuid4().hex}'
    run.mkdir()
    record = dict(passed=False, gpu_index=args.gpu, samples=args.samples, run=str(run.resolve()))
    requests, captures, errors = [], {}, {}

    def uid(n):
        return f'{n:032x}'

    def native(path):
        value = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    def rpc(method, params, error=None):
        request_id = len(requests) + 1
        requests.append(dict(jsonrpc='2.0', id=request_id, method=method, params=params))
        if error is not None:
            errors[request_id] = error
        return request_id

    def capture(name, revision=0, authored=False, size=(640, 360), error=None):
        path = run / f'{name}.bmp'
        params = dict(camera=uid(1), path=native(path), gpu=args.gpu,
                      samples=args.samples, width=size[0], height=size[1])
        if authored:
            params['revision'] = 1
        else:
            params.update(session_id=uid(900), tick=0, ui_revision=revision)
        call = rpc('world.capture' if authored else 'runtime.capture', params, error)
        captures[name] = dict(call=call, path=path, revision=revision, authored=authored, size=size, rejected=error is not None)
        return call

    def element(n, parent, kind, text='', action=None):
        return dict(op='ui.element.set', id=uid(n), element=dict(parent=uid(parent) if parent else None,
            name=f'UI {n}', kind=kind, text=text, action=action, visible=True, enabled=True))

    def edit(revision, values):
        return rpc('runtime.ui.edit', dict(session_id=uid(900), request_id=uuid.uuid4().hex,
            expected_tick=0, expected_ui_revision=revision, edits=values))

    def inspect(revision):
        return rpc('runtime.ui.inspect', dict(session_id=uid(900), tick=0, ui_revision=revision))

    ops = [dict(op='entity.create', id=uid(1), name='Capture camera', parent=None),
           dict(op='component.set', id=uid(1), type='Transform', value=dict(position=[0, 0, 6], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
           dict(op='component.set', id=uid(1), type='Camera', value=dict(vertical_fov=60, near=.1, far=100)),
           dict(op='entity.create', id=uid(2), name='World reference box', parent=None),
           dict(op='component.set', id=uid(2), type='MeshRenderer', value=dict(primitive='box', visible=True, albedo=[.7, .25, .1])),
           element(100, None, 'panel'), element(101, 100, 'label', 'Health 100'),
           element(102, 100, 'button', 'Resume', 'resume'), element(103, 100, 'button', 'Save', 'save')]
    rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
    capture('authored-baseline', authored=True)
    rpc('runtime.start', dict(session_id=uid(900), revision=1))
    initial = inspect(0)
    capture('runtime-initial')
    literal = "Health <button id='injected'>83</button>"
    edit(0, [dict(id=uid(101), text=literal)])
    changed = inspect(1)
    capture('runtime-literal', 1)
    capture('stale-revision', 0, error=-32009)
    # Guard validation precedes resource creation: neither stale nor missing
    # UI revision may produce a capture file.
    missing_path = run / 'missing-revision.bmp'
    rpc('runtime.capture', dict(session_id=uid(900), tick=0, camera=uid(1), path=native(missing_path), gpu=args.gpu), -32602)
    edit(1, [dict(id=uid(100), visible=False)])
    hidden = inspect(2)
    capture('runtime-hidden', 2)
    edit(2, [dict(id=uid(100), visible=True)])
    restored = inspect(3)
    capture('runtime-restored', 3)
    capture('runtime-resized', 3, size=(800, 450))
    capture('authored-after-runtime-edits', authored=True)
    final = rpc('runtime.inspect', dict(session_id=uid(900)))
    command = [str(args.binary.resolve()), 'world', native(run / 'world.json')]
    record.update(command=command, requests=requests)
    try:
        process = subprocess.run(command, input=''.join(json.dumps(r)+'\n' for r in requests),
                                 capture_output=True, text=True, encoding='utf-8', timeout=180)
        record.update(exit_code=process.returncode, stderr=process.stderr, stdout=process.stdout)
        assert process.returncode == 0, process.stdout + process.stderr
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        responses = {r['id']: r for r in replies}
        assert len(responses) == len(requests) == len(replies), responses
        record['responses'] = responses
        for request in requests:
            response = responses[request['id']]
            if request['id'] in errors:
                assert response.get('error', {}).get('code') == errors[request['id']], response
            else:
                assert 'result' in response, response
        images = {}
        record['captures'] = {}
        for name, entry in captures.items():
            path = entry['path']
            if entry['rejected']:
                assert not path.exists(), path
                continue
            report = responses[entry['call']]['result']
            assert report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0, report
            assert report['samples'] == args.samples, report
            assert report['source'] == ('authored' if entry['authored'] else 'runtime'), report
            assert report['tick'] == (None if entry['authored'] else 0), report
            assert report['ui_revision'] == (None if entry['authored'] else entry['revision']), report
            images[name] = pixels(path)
            assert (len(images[name][0]), len(images[name])) == entry['size'], name
            record['captures'][name] = dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest(), report=report)
        assert not missing_path.exists(), missing_path

        def state(call):
            return {e['id']: e for e in responses[call]['result']['elements']}

        before, after, invisible, visible = map(state, (initial, changed, hidden, restored))
        assert set(before) == set(after) == set(invisible) == set(visible) == {uid(n) for n in (100, 101, 102, 103)}
        assert before[uid(101)]['text'] == 'Health 100' and after[uid(101)]['text'] == literal
        assert visible[uid(101)]['text'] == literal
        assert all(not e['effective_visible'] and not e['eligible'] for e in invisible.values())
        assert visible[uid(102)]['eligible'] and visible[uid(103)]['eligible']
        assert images['authored-baseline'] == images['authored-after-runtime-edits'], 'Runtime UI changed authored capture.'
        assert images['runtime-hidden'] == images['authored-baseline'], 'Hidden runtime UI did not return to scene-only pixels.'
        assert images['runtime-initial'] != images['authored-baseline'], 'Runtime UI absent from named-camera capture.'
        assert images['runtime-initial'] != images['runtime-literal'], 'Same-tick UI text edit did not change pixels.'
        assert images['runtime-restored'] == images['runtime-literal'], 'Restoring visibility failed to reproduce unchanged UI.'
        summary = responses[final]['result']
        assert summary['tick'] == 0 and summary['ui_revision'] == 3 and summary['control_sequence'] == 0, summary
        record['checks'] = dict(named_runtime_ui_composed=True, authored_capture_stays_scene_only=True,
            same_tick_literal_text_changes_pixels=True, hidden_panel_removes_all_ui_pixels=True,
            restore_reproduces_pixels=True, resize_extent=True, stale_and_missing_revision_rejected_without_file=True,
            literal_markup_preserves_four_controls=True, physics_tick_and_control_sequence_unchanged=True)
        record['passed'] = True
    except Exception as error:
        record['failure'] = repr(error)
        raise
    finally:
        (run / 'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(dict(passed=True, evidence=str(run / 'result.json'), captures=len(record['captures']))))


if __name__ == '__main__':
    main()
