#!/usr/bin/env python3
"""Vulkan proof of transaction-authored UI geometry, opaque colors and freezing.

Requires native simulation, layout and rendering. No compiled callbacks,
physical devices, editor interaction or frame-rate claim is exercised.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import uuid
from scene_capture import pixels
from ui_authoring_contract import ROOT, FIXTURE, definition, operations, expected_controls, uid

sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / ('ui-authored-' + uuid.uuid4().hex)
    run.mkdir()
    record = dict(passed=False, gpu_index=args.gpu, samples=args.samples,
                  fixture_sha256=hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
                  engine_sha256=hashlib.sha256(args.engine.read_bytes()).hexdigest(),
                  captures={}, calls=[])
    images = {}
    client = None

    def native(path):
        text = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=5).strip() if args.windows_interop else text

    def call(method, **params):
        result = client.call(method, params, timeout=120 if method.endswith('.capture') else 12)
        record['calls'].append(dict(method=method, params=params, result=result))
        return result

    def capture(name, width=960, height=540, authored=False, revision=1, session=None, ui_revision=0):
        path = run / (name + '.bmp')
        params = dict(camera=uid(1), path=native(path), width=width, height=height,
                      samples=args.samples, gpu=args.gpu)
        if authored:
            params['revision'] = revision
        else:
            # Match virtual layout explicitly; omitted density follows the host.
            params.update(session_id=session, tick=0, ui_revision=ui_revision, ui_scale=1)
        result = call('world.capture' if authored else 'runtime.capture', **params)
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, result
        assert result['source'] == ('authored' if authored else 'runtime'), result
        assert result['tick'] == (None if authored else 0), result
        assert result['ui_revision'] == (None if authored else ui_revision), result
        if not authored:
            assert result['ui_scale'] == 1, result
        assert result['samples'] == args.samples, result
        image = pixels(path)
        assert len(image) == height and len(image[0]) == width
        images[name] = image
        record['captures'][name] = dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(), report=result)
        return image

    def color(image, x, y, hexadecimal):
        expected = tuple(int(hexadecimal[index:index + 2], 16) for index in (1, 3, 5))
        measured = []
        # A small interior patch avoids glyph coverage and edge rasterization;
        # opaque UI must reproduce the authored sRGB bytes within rounding.
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                actual = image[round(y) + dy][round(x) + dx]
                assert all(abs(a - b) <= 2 for a, b in zip(actual, expected)), (x, y, hexadecimal, actual)
                measured.append(actual)
        return dict(x=x, y=y, expected=expected, measured=measured)

    def validate_layout(result, width, height):
        rows = {row['id']: row for row in result['controls']}
        expected = expected_controls(width, height)
        assert set(rows) == set(expected), rows
        for identity, bounds in expected.items():
            assert len(rows[identity]['bounds']) == 4
            assert all(abs(a - b) <= .05 for a, b in zip(rows[identity]['bounds'], bounds)), rows[identity]
        return rows

    def witness(image, width, height, resume='#6f243b'):
        menu_x, menu_y = (width - 320) / 2, (height - 200) / 2
        controls = expected_controls(width, height)
        resume_bounds, save_bounds = controls[uid(113)], controls[uid(112)]
        return {
            'menu': color(image, menu_x + 4, menu_y + 4, '#241c24'),
            'resume': color(image, resume_bounds[2] - 12, (resume_bounds[1] + resume_bounds[3]) / 2, resume),
            'save': color(image, save_bounds[2] - 12, (save_bounds[1] + save_bounds[3]) / 2, '#302a33'),
            'hud': color(image, 28, height - 28, '#241c24'),
        }

    try:
        client = WorldClient.open(str(args.engine.resolve()), native(run / 'world.json'), close_timeout=5)
        camera = [dict(op='entity.create', id=uid(1), name='UI capture camera'),
                  dict(op='component.set', id=uid(1), type='Transform',
                       value=dict(position=[0, 0, 6], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
                  dict(op='component.set', id=uid(1), type='Camera', value=dict(vertical_fov=60, near=.1, far=100)),
                  dict(op='entity.create', id=uid(2), name='Scene-only reference'),
                  dict(op='component.set', id=uid(2), type='MeshRenderer',
                       value=dict(primitive='box', visible=True, albedo=[.15, .35, .65]))]
        call('world.transact', request_id=uuid.uuid4().hex, base_revision=0, ops=camera + operations())
        observed = call('world.ui.list', limit=256)
        assert {row['id']: row['element'] for row in observed['elements']} == definition()
        record['layouts'] = []
        for width, height in ((960, 540), (1280, 720), (1024, 768)):
            result = call('world.ui.layout', revision=1, width=width, height=height, scale=1)
            validate_layout(result, width, height)
            record['layouts'].append(result)
        authored = capture('authored', authored=True)
        session = uuid.uuid4().hex
        call('runtime.start', session_id=session, revision=1)
        before = call('runtime.ui.inspect', session_id=session, tick=0)
        rows = {row['id']: row for row in before['elements']}
        assert len(rows) == 8 and rows[uid(113)]['style'] == definition()[uid(113)]['style']
        assert rows[uid(113)]['layout'] == definition()[uid(113)]['layout']
        summary_before = call('runtime.inspect', session_id=session)
        record['rejected_ui_scales'] = []
        for index, invalid in enumerate((True, None, '1', [1], {'value': 1}, .24, 8.01)):
            path = run / ('invalid-ui-scale-' + str(index) + '.bmp')
            params = dict(session_id=session, tick=0, ui_revision=0, camera=uid(1), path=native(path),
                          width=960, height=540, gpu=args.gpu, samples=args.samples, ui_scale=invalid)
            try:
                client.call('runtime.capture', params, timeout=12)
            except RpcError as error:
                assert error.code == -32602, error
                record['calls'].append(dict(method='runtime.capture', params=params,
                                            error=dict(code=error.code, message=str(error))))
            else:
                raise AssertionError('Malformed ui_scale was accepted: ' + repr(invalid))
            assert not path.exists(), path
            assert call('runtime.ui.inspect', session_id=session, tick=0) == before
            assert call('runtime.inspect', session_id=session) == summary_before
            record['rejected_ui_scales'].append(invalid)
        record['color_witnesses'] = {}
        first = capture('runtime-960', session=session)
        second = capture('runtime-1280', 1280, 720, session=session)
        aspect = capture('runtime-1024', 1024, 768, session=session)
        assert first != authored, 'Authored UI did not compose into runtime pixels.'
        record['color_witnesses']['960'] = witness(first, 960, 540)
        record['color_witnesses']['1280'] = witness(second, 1280, 720)
        record['color_witnesses']['1024'] = witness(aspect, 1024, 768)

        changed = copy.deepcopy(definition()[uid(113)])
        changed['style']['background_color'] = '#224466'
        call('world.transact', request_id=uuid.uuid4().hex, base_revision=1,
             ops=[dict(op='ui.element.set', id=uid(113), element=changed)])
        frozen = capture('runtime-frozen', session=session)
        assert frozen == first, 'Authored style edit leaked into frozen runtime presentation.'
        assert call('runtime.ui.inspect', session_id=session, tick=0) == before
        assert capture('authored-after', authored=True, revision=2) == authored

        call('runtime.ui.edit', session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
             expected_ui_revision=0, edits=[dict(id=uid(100), visible=False), dict(id=uid(200), visible=False)])
        assert capture('runtime-hidden', session=session, ui_revision=1) == authored
        call('runtime.ui.edit', session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
             expected_ui_revision=1, edits=[dict(id=uid(100), visible=True), dict(id=uid(200), visible=True),
                                            dict(id=uid(201), text='Health 83')])
        edited = capture('runtime-text', session=session, ui_revision=2)
        assert edited != first, 'Live HUD text did not change captured pixels.'
        record['color_witnesses']['live_text'] = witness(edited, 960, 540)
        summary = call('runtime.inspect', session_id=session)
        assert summary['tick'] == 0 and summary['ui_revision'] == 2 and summary['control_sequence'] == 0, summary
        call('runtime.stop', session_id=session)
        session = uuid.uuid4().hex
        call('runtime.start', session_id=session, revision=2)
        updated = capture('runtime-new-style', session=session)
        assert updated != first, 'Fresh runtime failed to adopt authored style.'
        record['color_witnesses']['new_style'] = witness(updated, 960, 540, '#224466')
        record['checks'] = dict(transaction_authored_exact_definition=True, responsive_layout_three_extents=True,
                                numeric_opaque_color_witnesses=True, authored_scene_ui_free=True,
                                runtime_style_frozen=True, fresh_runtime_adopts_new_style=True,
                                hidden_roots_remove_all_ui=True, same_tick_literal_text_update=True,
                                simulation_tick_and_control_sequence_unchanged=True, explicit_capture_density_matches_layout=True,
                                invalid_capture_density_rejected_without_file_or_runtime_change=True)
        client.close()
        record['owner_exit_code'] = client.transport.returncode
        assert client.transport.returncode == 0, client.transport.stderr_tail
        record['passed'] = True
    except BaseException as error:
        record['failure'] = repr(error)
        raise
    finally:
        original_failure = sys.exc_info()[0] is not None
        try:
            if client is not None:
                client.close()
                record['owner_exit_code'] = client.transport.returncode
                assert client.transport.returncode == 0, client.transport.stderr_tail
        except BaseException as error:
            record['passed'] = False
            record['cleanup_failure'] = repr(error)
            if not original_failure:
                raise
        finally:
            (run / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, evidence=str(run / 'result.json'), captures=len(record['captures']))))


if __name__ == '__main__':
    main()
