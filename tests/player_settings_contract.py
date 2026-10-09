#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Network-free persisted player settings and optional real Vulkan consumption.

Settings are checked independently of authored worlds. The optional capture
gate compares player FOV with a separately authored reference camera and a
screen-space projection oracle; recorded replay look remains semantic degrees.
No keyboard, mouse or controller hardware qualification is claimed.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
FIELDS = {'camera.vertical_fov', 'input.sensitivity_x', 'input.sensitivity_y',
          'input.invert_x', 'input.invert_y', 'ui.scale', 'graphics.samples',
          'graphics.frames_in_flight'}
SAMPLE = {'camera.vertical_fov': 90, 'input.sensitivity_x': .23,
          'input.sensitivity_y': .17, 'input.invert_x': True,
          'input.invert_y': False, 'ui.scale': 1.5, 'graphics.samples': 1,
          'graphics.frames_in_flight': 1}
ARGS = None
EVIDENCE = None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def uid(value):
    return f'{value:032x}'


def native(path):
    text = str(path.resolve())
    return subprocess.check_output(['wslpath', '-w', text], text=True,
                                   timeout=10).strip() if ARGS.windows_interop else text


def rpc(method, **params):
    return {'jsonrpc': '2.0', 'method': method, 'params': params}


def result(reply):
    if 'result' not in reply:
        raise AssertionError(reply)
    return reply['result']


def error(reply, code):
    if reply.get('error', {}).get('code') != code or 'result' in reply:
        raise AssertionError(reply)
    if not isinstance(reply['error'].get('message'), str) or not reply['error']['message']:
        raise AssertionError(reply)


def terminate_owned(process, marker):
    if ARGS.windows_interop:
        # Unique owned world/report paths identify the native process behind
        # WSL's forwarding PID; never terminate every process named poima.
        literal = marker.replace("'", "''")
        script = ("$m='" + literal + "'; Get-CimInstance Win32_Process | "
                  "Where-Object { $_.Name -eq 'poima.exe' -and $_.CommandLine "
                  "-and $_.CommandLine.Contains($m) } | ForEach-Object { "
                  "& taskkill.exe /PID $_.ProcessId /T /F | Out-Null }")
        encoded = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive',
                        '-EncodedCommand', encoded], capture_output=True, timeout=30)
    if process.poll() is None:
        process.kill()


def requests(world, calls, label):
    calls = [dict(call, id=index + 1) for index, call in enumerate(calls)]
    command = [str(ARGS.binary.resolve()), 'world', native(world)]
    owner = {'label': label, 'command': command, 'requests': calls}
    EVIDENCE['owners'].append(owner)
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8')
    owner['process_id'] = process.pid
    try:
        out, err = process.communicate(''.join(json.dumps(call, allow_nan=False) + '\n'
                                               for call in calls), timeout=ARGS.timeout)
    except subprocess.TimeoutExpired:
        terminate_owned(process, native(world))
        out, err = process.communicate(timeout=20)
        owner.update(exit_code=process.returncode, stderr=err, timed_out=True)
        raise AssertionError('Owned settings process exceeded timeout')
    owner.update(exit_code=process.returncode, stderr=err, timed_out=False)
    if process.returncode != 0:
        raise AssertionError((process.returncode, out, err))
    replies = [json.loads(line) for line in out.splitlines()]
    owner['responses'] = replies
    EVIDENCE['rpc_count'] += len(calls)
    if len(replies) != len(calls):
        raise AssertionError((len(replies), len(calls), err))
    for index, reply in enumerate(replies):
        if reply.get('jsonrpc') != '2.0' or reply.get('id') != index + 1:
            raise AssertionError(reply)
        if ('result' in reply) == ('error' in reply):
            raise AssertionError(reply)
    return replies


def transaction(path, revision, values=None, reset=None, request_id=None, **extra):
    return rpc('settings.transact', path=native(path), expected_revision=revision,
               request_id=request_id or uuid.uuid4().hex, set=values or {},
               reset=reset or [], **extra)


class SettingsContract(unittest.TestCase):
    def setUp(self):
        self.directory = ARGS.output / self._testMethodName
        self.directory.mkdir()
        self.world = self.directory / 'world.json'
        self.profile = self.directory / 'player.poima-settings.json'
        result(self.call(rpc('world.transact', base_revision=0,
                             request_id=uuid.uuid4().hex,
                             ops=[{'op': 'entity.create', 'id': uid(1),
                                   'name': 'Preferences do not own this entity'}]))[0])
        self.before = self.world.read_bytes()

    def tearDown(self):
        self.assertEqual(self.world.read_bytes(), self.before,
                         'Player preferences changed authored world bytes')

    def call(self, *calls, world=None):
        return requests(world or self.world, calls, self._testMethodName)

    def inspect(self):
        return result(self.call(rpc('settings.inspect', path=native(self.profile)))[0])

    def test_discovery_sparse_missing_and_fresh_owner_persistence(self):
        discovery, description, missing = map(result, self.call(
            rpc('world.describe'), rpc('settings.describe'),
            rpc('settings.inspect', path=native(self.profile))))
        for name in ('settings.describe', 'settings.inspect', 'settings.transact'):
            self.assertIn(name, discovery['methods'])
        for name in ('settings_profile', 'settings_revision', 'settings_overrides'):
            self.assertIn(name, discovery['methods']['runtime.play']['properties'])
        encoded = json.dumps(description)
        for name in FIELDS:
            self.assertIn(name, encoded)
        self.assertEqual(missing['values'], {})
        self.assertEqual(missing['revision'], 0)
        self.assertFalse(missing['persisted'])
        self.assertEqual(missing['application'], 'next_player')
        self.assertEqual(missing['state'], 'stored_intent')
        self.assertFalse(self.profile.exists())
        stored = result(self.call(transaction(self.profile, 0,
                                               {'camera.vertical_fov': 91}))[0])
        self.assertEqual(stored['values'], {'camera.vertical_fov': 91})
        self.assertEqual(stored['revision'], 1)
        self.assertTrue(stored['persisted'])
        raw = self.profile.read_bytes()
        fresh = self.inspect()
        self.assertEqual(fresh['values'], stored['values'])
        self.assertEqual(fresh['content_hash'], stored['content_hash'])
        other = self.directory / 'unused-world.json'
        shared = result(self.call(rpc('settings.inspect', path=native(self.profile)),
                                  world=other)[0])
        self.assertEqual(shared, fresh)
        self.assertFalse(other.exists())
        self.assertEqual(self.profile.read_bytes(), raw)

    def test_patch_reset_preview_and_inheritance(self):
        preview = result(self.call(transaction(self.profile, 0, SAMPLE, preview=True))[0])
        self.assertTrue(preview['preview'])
        self.assertEqual(preview['revision'], 0)
        self.assertFalse(self.profile.exists())
        result(self.call(transaction(self.profile, 0, SAMPLE))[0])
        raw = self.profile.read_bytes()
        result(self.call(transaction(self.profile, 1,
                                     {'input.sensitivity_x': .5},
                                     ['camera.vertical_fov', 'ui.scale'], preview=True))[0])
        self.assertEqual(raw, self.profile.read_bytes())
        changed = result(self.call(transaction(self.profile, 1,
                                               {'input.sensitivity_x': .5},
                                               ['camera.vertical_fov', 'ui.scale']))[0])
        expected = {**SAMPLE, 'input.sensitivity_x': .5}
        expected.pop('camera.vertical_fov'); expected.pop('ui.scale')
        self.assertEqual(changed['values'], expected)
        self.assertEqual(changed['revision'], 2)
        emptied = result(self.call(transaction(self.profile, 2, reset=sorted(expected)))[0])
        self.assertEqual(emptied['values'], {})
        self.assertEqual(self.inspect()['values'], {})

    def test_retries_revision_guards_and_request_parameter_conflicts(self):
        request = transaction(self.profile, 0, SAMPLE)
        first = result(self.call(request)[0]); raw = self.profile.read_bytes()
        retry = copy.deepcopy(request); retry['params']['preview'] = False
        self.assertEqual(result(self.call(retry)[0]), {**first, 'replayed': True})
        self.assertEqual(self.profile.read_bytes(), raw)
        changed = copy.deepcopy(request); changed['params']['set']['input.invert_y'] = True
        error(self.call(changed)[0], -32010)
        error(self.call(transaction(self.profile, 0, SAMPLE))[0], -32009)
        self.assertEqual(self.profile.read_bytes(), raw)
        result(self.call(transaction(self.profile, 1, {'ui.scale': 2}))[0])
        current = self.profile.read_bytes()
        self.assertEqual(result(self.call(request)[0]), {**first, 'replayed': True})
        self.assertEqual(self.profile.read_bytes(), current)
        self.assertEqual(self.inspect()['revision'], 2)

    def test_invalid_batch_types_ranges_and_reset_are_atomic(self):
        result(self.call(transaction(self.profile, 0, SAMPLE))[0])
        raw = self.profile.read_bytes()
        invalid = [('camera.vertical_fov', 4.999), ('camera.vertical_fov', 150.001),
                   ('input.sensitivity_x', -.001), ('input.sensitivity_y', 10.001),
                   ('input.invert_x', 1), ('input.invert_y', 'false'),
                   ('ui.scale', .249), ('ui.scale', 8.001),
                   ('graphics.samples', 2), ('graphics.samples', True),
                   ('graphics.frames_in_flight', 0), ('graphics.frames_in_flight', 3),
                   ('graphics.frames_in_flight', 1.5), ('unknown', 1)]
        for name, value in invalid:
            error(self.call(transaction(self.profile, 1,
                                        {'camera.vertical_fov': 100, name: value}))[0], -32602)
            self.assertEqual(self.profile.read_bytes(), raw)
        for reset in (['unknown'], ['ui.scale', 'ui.scale']):
            error(self.call(transaction(self.profile, 1, reset=reset))[0], -32602)
            self.assertEqual(self.profile.read_bytes(), raw)
        error(self.call(transaction(self.profile, 1, {'ui.scale': 2}, ['ui.scale']))[0], -32602)
        self.assertEqual(self.profile.read_bytes(), raw)
        self.assertEqual(self.inspect()['values'], SAMPLE)

    def test_bounds_valid_and_world_reserved_paths_rejected(self):
        for values in ({'camera.vertical_fov': 5, 'input.sensitivity_x': 0,
                        'input.sensitivity_y': 10, 'ui.scale': .25,
                        'graphics.samples': 4, 'graphics.frames_in_flight': 2},
                       {'camera.vertical_fov': 150, 'ui.scale': 8}):
            revision = self.inspect()['revision']
            self.assertEqual(result(self.call(transaction(self.profile, revision, values))[0])
                             ['revision'], revision + 1)
        reply = self.call(transaction(self.world, 0, SAMPLE))[0]
        self.assertIn('error', reply)
        assets = Path(str(self.world)+'.assets'); assets.mkdir(exist_ok=True)
        blocked = self.call(transaction(assets/'blocked.poima-settings.json', 0, SAMPLE))[0]
        self.assertIn('error', blocked)
        self.assertFalse((assets/'blocked.poima-settings.json').exists())

    def test_corrupt_missing_and_unsafe_storage_fail_closed(self):
        self.profile.write_bytes(b'{"format":')
        raw = self.profile.read_bytes()
        self.assertIn('error', self.call(rpc('settings.inspect', path=native(self.profile)))[0])
        self.assertIn('error', self.call(transaction(self.profile, 0, SAMPLE))[0])
        self.assertEqual(self.profile.read_bytes(), raw)
        bad = self.directory / 'missing' / 'player.poima-settings.json'
        self.assertIn('error', self.call(transaction(bad, 0, SAMPLE))[0])
        self.assertFalse(bad.exists())
        folder = self.directory / 'directory.poima-settings.json'; folder.mkdir()
        self.assertIn('error', self.call(rpc('settings.inspect', path=native(folder)))[0])

    def test_retained_receipt_and_missing_reset_noop(self):
        request = transaction(self.profile, 0, {'ui.scale': 1})
        first = result(self.call(request)[0])
        calls = [transaction(self.profile, revision, {'ui.scale': 1+revision*.01})
                 for revision in range(1, 31)]
        for reply in self.call(*calls):
            result(reply)
        self.assertEqual(result(self.call(request)[0]), {**first, 'replayed': True})
        current = self.inspect()
        no_op = result(self.call(transaction(self.profile, current['revision'],
                                            reset=['camera.vertical_fov']))[0])
        self.assertFalse(no_op['changed'])
        self.assertEqual(no_op['values'], current['values'])
        self.assertEqual(no_op['content_hash'], current['content_hash'])


def inventory(path):
    return {str(file.relative_to(path)).replace('\\', '/'): sha(file)
            for file in sorted(path.rglob('*')) if file.is_file()}


def cli(binary, arguments, label, marker, success=True):
    command = [str(binary.resolve()), *arguments]
    record = {'label': label, 'command': command}
    EVIDENCE.setdefault('commands', []).append(record)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding='utf-8')
    record['process_id'] = process.pid
    try:
        out, err = process.communicate(timeout=ARGS.timeout)
    except subprocess.TimeoutExpired:
        terminate_owned(process, marker)
        out, err = process.communicate(timeout=20)
        record.update(exit_code=process.returncode, stderr=err, timed_out=True)
        raise AssertionError('Owned game/export process exceeded timeout')
    record.update(exit_code=process.returncode, stderr=err, timed_out=False)
    reply = json.loads(out); record['response'] = reply
    assert (process.returncode == 0) == success, reply
    assert reply['status'] == ('ok' if success else 'error'), reply
    return reply.get('result')


def bundle_gate(profile):
    directory = ARGS.output/'bundle'; directory.mkdir()
    source = directory/'source'
    cli(ARGS.binary, ['project', 'create', native(source), '--name', 'Player preferences'],
        'bundle-project-create', native(source))
    project = source/'project.json'
    before = inventory(source)
    export = directory/'export'
    cli(ARGS.binary, ['project', 'build', native(project), '--output', native(export),
                     '--runtime', native(ARGS.runtime)], 'bundle-export', native(export))
    assert inventory(source) == before
    relocated = directory/'relocated'; export.rename(relocated)
    bundle_before = inventory(relocated)
    profile_before = {file.name: sha(file) for file in profile.parent.glob(profile.name+'*')
                      if file.is_file()}
    shutil.rmtree(source)  # Only the exclusively created project from this run.
    assert not source.exists() and not export.exists()
    game = relocated/'game.json'
    executables = list((relocated/'runtime'/'bin').glob('poima*'))
    executables = [path for path in executables if path.name in ('poima', 'poima.exe')]
    assert len(executables) == 1, executables
    binary = executables[0]
    replay = directory/'replay.json'; replay.write_text('[{"ticks":2}]\n', encoding='utf-8')
    report = directory/'profile-report.json'
    arguments = ['game', 'run', native(game), '--replay', native(replay), '--gpu', str(ARGS.gpu),
                 '--settings-profile', native(profile), '--settings-revision', '1',
                 '--width', '640', '--height', '480', '--report', native(report)]
    played = cli(binary, arguments, 'source-free-bundled-settings', native(game))
    settings = played['play']['settings']
    assert played['success'] and played['play']['success']
    assert played['play']['samples'] == 1
    assert played['play']['render_diagnostics']['frame_execution']['limit'] == 1
    for name, value in SAMPLE.items():
        field = settings['fields'][name]
        assert field['source'] == 'profile' and field['effective'] == value, (name, field)
    explicit_report = directory/'explicit-report.json'
    explicit = arguments[:-2] + ['--report', native(explicit_report), '--samples', '4',
                                '--frames-in-flight', '2', '--settings-overrides',
                                '{"camera.vertical_fov":80,"graphics.samples":1}']
    overridden = cli(binary, explicit, 'source-free-bundled-explicit-precedence', native(game))
    assert overridden['success']
    assert overridden['play']['samples'] == 4
    assert overridden['play']['render_diagnostics']['frame_execution']['limit'] == 2
    fields = overridden['play']['settings']['fields']
    assert fields['camera.vertical_fov']['effective'] == 80
    assert fields['camera.vertical_fov']['source'] == 'session_override'
    for name, value in [('graphics.samples', 4), ('graphics.frames_in_flight', 2)]:
        assert fields[name]['effective'] == value and fields[name]['source'] == 'explicit_option'
    stale = list(arguments)
    stale[stale.index('--settings-revision')+1] = '0'
    stale[stale.index('--report')+1] = native(directory/'stale-report.json')
    cli(binary, stale, 'bundle-stale-preference-revision', native(game), success=False)
    assert not (directory/'stale-report.json').exists()
    duplicate = list(arguments)
    duplicate[duplicate.index('--report')+1] = native(directory/'duplicate-report.json')
    duplicate += ['--settings-overrides', '{"camera.vertical_fov":75,"camera.vertical_fov":80}']
    cli(binary, duplicate, 'bundle-duplicate-json-override-key', native(game), success=False)
    assert not (directory/'duplicate-report.json').exists()
    assert inventory(relocated) == bundle_before
    assert {file.name: sha(file) for file in profile.parent.glob(profile.name+'*')
            if file.is_file()} == profile_before, 'Read-only game changed preferences/sidecars'
    EVIDENCE['bundle'] = {'passed': True, 'source_removed_before_play': True,
                          'settings': settings, 'explicit_settings': overridden['play']['settings'],
                          'bundle_inventory': bundle_before, 'runtime_descriptor_sha256':
                          sha(ARGS.runtime/'runtime.json'), 'immutable_profile_sidecars': profile_before}


def capture_gate():
    """Optional GPU gate is filled from the frozen player application contract."""
    directory = ARGS.output / 'vulkan'; directory.mkdir()
    world, reference = directory/'world.json', directory/'reference.json'
    profile = directory/'player.poima-settings.json'
    # A camera-independent player controller advances once while a stationary
    # observer looks down -Z. A red box gives an analytic
    # screen-space silhouette unaffected by physics or ambient sky details.
    def entity(n, position, parent=None, **components):
        ops = [{'op': 'entity.create', 'id': uid(n), 'name': str(n), 'parent': parent},
               {'op': 'component.set', 'id': uid(n), 'type': 'Transform',
                'value': {'position': position, 'rotation': [0,0,0,1], 'scale': [1,1,1]}}]
        ops += [{'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
                for kind, value in components.items()]
        return ops
    ops = entity(10, [0,0,0], Camera={'vertical_fov': 60, 'near': .1, 'far': 100})
    ops += entity(20, [0,0,-5], MeshRenderer={'primitive': 'box', 'visible': True,
                                          'albedo': [.9,.01,.01]})
    ops += entity(100, [100,10,0], CharacterController={'radius': .3, 'height': 1.8,
                        'speed': 4, 'jump_speed': 5, 'camera': uid(101)})
    ops += entity(101, [0,1.6,0], parent=uid(100),
                  Camera={'vertical_fov': 70, 'near': .1, 'far': 100})
    def dp(value):
        return {'unit': 'dp', 'value': value}
    def ui(n, parent, kind, text='', action=None, **properties):
        return {'op': 'ui.element.set', 'id': uid(n), 'element': {
            'parent': parent, 'name': f'Settings proof {n}', 'kind': kind,
            'text': text, 'action': action, 'visible': True, 'enabled': True, **properties}}
    # Fixed dp dimensions make absolute scale independently measurable. The
    # top-left panel ends at x264 at1.5x, leaving the central box unobstructed.
    ops += [ui(500, None, 'panel', layout={
                'position': 'absolute', 'left': dp(16), 'top': dp(16),
                'width': dp(160), 'height': dp(88), 'direction': 'column',
                'align': 'stretch', 'justify': 'start', 'padding': [8,8,8,8],
                'gap': 8, 'hit_test': 'pass_through'},
                style={'background_color': '#14202a', 'border_width': 0, 'border_radius': 0}),
            ui(501, uid(500), 'label', 'UI scale', layout={'width': dp(144), 'height': dp(20)},
                style={'font_size': 14, 'color': '#f7e8eb'}),
            ui(502, uid(500), 'button', 'Settings', 'settings-proof',
                layout={'width': dp(144), 'height': dp(36)},
                style={'background_color': '#224466', 'color': '#f7e8eb',
                       'font_size': 14, 'border_width': 0, 'border_radius': 0,
                       'hover': {'background_color': '#224466'},
                       'focus': {'background_color': '#224466'},
                       'pressed': {'background_color': '#224466'}})]
    result(requests(world, [rpc('world.transact', base_revision=0,
                      request_id=uuid.uuid4().hex, ops=ops)], 'gpu-author')[0])
    reference_ops = copy.deepcopy(ops)
    for op in reference_ops:
        if op.get('type') == 'Camera' and op['id'] == uid(10):
            op['value']['vertical_fov'] = 90
    result(requests(reference, [rpc('world.transact', base_revision=0,
                  request_id=uuid.uuid4().hex, ops=reference_ops)], 'gpu-reference-author')[0])
    original = world.read_bytes()
    result(requests(world, [transaction(profile, 0, SAMPLE)], 'gpu-store')[0])
    profile_bytes = profile.read_bytes()
    def play_params(session, name, **extra):
        return {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0,
                'camera': uid(10), 'controller': uid(100), 'mode': 'replay',
                'sequence': [{'ticks': 2, 'look': [7,-3]}], 'width': 640, 'height': 480,
                'gpu': ARGS.gpu, 'path': native(directory/name), **extra}
    session = uuid.uuid4().hex
    first = requests(world, [rpc('runtime.start', session_id=session, revision=1),
            rpc('runtime.play', **play_params(session, 'profile.bmp',
                settings_profile=native(profile), settings_revision=1)),
            rpc('runtime.entity', session_id=session, id=uid(100))], 'gpu-persisted')
    report = result(first[1]); state = result(first[2])
    if not report['success']:
        raise AssertionError(report)
    assert report['hardware'] and report['nvrhi_errors'] == 0 and report['capture_written']
    assert report['samples'] == 1
    assert report['render_diagnostics']['frame_execution']['limit'] == 1
    application = report['settings']; fields = application['fields']
    assert application['format'] == 'poima.settings.application.v1'
    assert application['profile']['source'] == 'profile'
    assert application['profile']['revision'] == 1
    assert application['application'] == 'next_player' and set(fields) == FIELDS
    for name, value in SAMPLE.items():
        assert fields[name]['effective'] == value, (name, fields[name])
        assert fields[name]['source'] == 'profile'
        assert fields[name]['outcome'] == ('validated_only' if name.startswith('input.') else 'applied')
    reference_session = uuid.uuid4().hex
    def capture(name, scale):
        return rpc('runtime.capture', session_id=reference_session, tick=2, ui_revision=0,
                   camera=uid(10), gpu=ARGS.gpu, path=native(directory/name), width=640,
                   height=480, samples=1, frames_in_flight=1, ui_scale=scale)
    second = requests(reference, [rpc('runtime.start', session_id=reference_session, revision=1),
            rpc('runtime.step', session_id=reference_session, request_id=uuid.uuid4().hex,
                expected_tick=0, ticks=1, inputs=[{'entity': uid(100), 'look': [7,-3]}]),
            rpc('runtime.step', session_id=reference_session, request_id=uuid.uuid4().hex,
                expected_tick=1, ticks=1), capture('reference.bmp', 1.5),
            capture('unscaled-ui.bmp', 1),
            rpc('runtime.entity', session_id=reference_session, id=uid(100)),
            rpc('world.ui.layout', revision=1, width=640, height=480, scale=1.5),
            rpc('world.ui.layout', revision=1, width=640, height=480, scale=1)],
            'gpu-authored-reference')
    for index, scale in [(3, 1.5), (4, 1)]:
        captured = result(second[index])
        assert captured['capture_written'] and captured['hardware'] and captured['nvrhi_errors'] == 0
        assert captured['ui_scale'] == scale
    actual, expected = pixels(directory/'profile.bmp'), pixels(directory/'reference.bmp')
    assert actual == expected, 'Persisted FOV differs from independent authored-camera image'
    a, b = copy.deepcopy(state), result(second[5]); a.pop('session_id'); b.pop('session_id')
    assert a == b, 'Replay input was reinterpreted through pointer preferences'
    unscaled = pixels(directory/'unscaled-ui.bmp')
    assert unscaled != actual, 'Explicit UI scale did not change any rendered pixels'
    ui_bounds = {}
    for scale, image, layout_index in [(1.5, actual, 6), (1, unscaled, 7)]:
        rows = {row['id']: row for row in result(second[layout_index])['controls']}
        expected_button = [24*scale, 52*scale, 168*scale, 88*scale]
        button = rows[uid(502)]
        assert button['hittable'] and button['visible'] and button['enabled']
        assert all(abs(a-b) <= .001 for a,b in zip(button['bounds'], expected_button)), button
        # Independent authored opaque color, excluding text-covered pixels.
        points = [(x,y) for y,row in enumerate(image[:160]) for x,value in enumerate(row[:280])
                  if all(abs(a-b) <= 1 for a,b in zip(value, (34,68,102)))]
        assert points, 'Opaque proof button not observed in player/capture pixels'
        bounds = [min(x for x,y in points), min(y for x,y in points),
                  max(x for x,y in points)+1, max(y for x,y in points)+1]
        assert all(abs(a-b) <= 1 for a,b in zip(bounds, expected_button)), (bounds, expected_button)
        ui_bounds[str(scale)] = {'independent_expected_bounds': expected_button,
                                'pixel_bounds': bounds, 'native_layout': button}
    # Exact box front face at z=-4.5: projected vertical half-size .5/4.5.
    ys = [y for y, row in enumerate(actual) for red, green, blue in row[290:350]
          if red > 70 and red > green * 2 and red > blue * 2]
    assert ys, 'Projection reference has no visible red silhouette'
    measured = max(ys)-min(ys)+1
    projected = 480 * .5 / (4.5 * math.tan(math.radians(90)/2))
    assert abs(measured-projected) <= 3, (measured, projected)
    override_session = uuid.uuid4().hex
    override = requests(world, [rpc('runtime.start', session_id=override_session, revision=1),
        rpc('runtime.play', **play_params(override_session, 'overrides.bmp', samples=4,
            frames_in_flight=2, settings_profile=native(profile), settings_revision=1,
            settings_overrides={'camera.vertical_fov': 60, 'graphics.samples': 1,
                                'graphics.frames_in_flight': 1}))], 'gpu-explicit-precedence')
    overridden = result(override[1]); assert overridden['success']
    assert overridden['samples'] == 4
    assert overridden['render_diagnostics']['frame_execution']['limit'] == 2
    fields = overridden['settings']['fields']
    assert fields['camera.vertical_fov']['source'] == 'session_override'
    assert fields['camera.vertical_fov']['effective'] == 60
    for name, value in [('graphics.samples', 4), ('graphics.frames_in_flight', 2)]:
        assert fields[name]['source'] == 'explicit_option' and fields[name]['effective'] == value
    overridden_image = pixels(directory/'overrides.bmp')
    assert overridden_image != actual
    override_ys = [y for y,row in enumerate(overridden_image) for red,green,blue in row[290:350]
                   if red > 70 and red > green*2 and red > blue*2]
    assert override_ys
    override_height = max(override_ys)-min(override_ys)+1
    override_expected = 480*.5/(4.5*math.tan(math.radians(60)/2))
    assert abs(override_height-override_expected) <= 3, (override_height, override_expected)
    bad_session = uuid.uuid4().hex
    failed = requests(world, [rpc('runtime.start', session_id=bad_session, revision=1),
        rpc('runtime.play', **{**play_params(bad_session, 'bad-device.bmp',
            settings_profile=native(profile), settings_revision=1), 'gpu': 4095}),
        rpc('runtime.inspect', session_id=bad_session)], 'gpu-failed-presentation')
    failed_report = result(failed[1])
    assert not failed_report['success'] and failed_report['frames_presented'] == 0
    for name, field in failed_report['settings']['fields'].items():
        assert field['outcome'] == ('validated_only' if name.startswith('input.') else 'not_presented')
    assert result(failed[2])['tick'] == 0 and not (directory/'bad-device.bmp').exists()
    assert world.read_bytes() == original and profile.read_bytes() == profile_bytes
    EVIDENCE['vulkan'] = {'passed': True, 'gpu': ARGS.gpu,
        'hardware_name': report['gpu'], 'projection_height_pixels': measured,
        'independent_expected_height_pixels': projected,
        'override_projection_height_pixels': override_height,
        'independent_override_expected_height_pixels': override_expected,
        'exact_authored_reference_pixels': True, 'semantic_replay_unchanged': True,
        'ui_scale_rendered_bounds': ui_bounds, 'unscaled_ui_pixels_differ': True,
        'settings': application, 'explicit_settings': overridden['settings'],
        'images': {name: sha(directory/name) for name in
                   ['profile.bmp', 'reference.bmp', 'unscaled-ui.bmp', 'overrides.bmp']},
        'limitations': ['Recorded look values are semantic degrees, not physical pointer input',
                       'UI layout hittability is queried virtually; no physical activation test']}
    if ARGS.runtime:
        bundle_gate(profile)


def main():
    global ARGS, EVIDENCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path,
                        help='New evidence directory; optional for network/GPU-free protocol checks')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--runtime', type=Path, help='Installed matching runtime for optional export gate')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=180)
    ARGS = parser.parse_args()
    if not ARGS.binary.is_file() or not 30 <= ARGS.timeout <= 1800:
        parser.error('Supply an existing binary and timeout 30..1800')
    explicit_gpu = any(value == '--gpu' or value.startswith('--gpu=') for value in sys.argv[1:])
    if ARGS.output is None and (ARGS.capture or ARGS.runtime or explicit_gpu):
        parser.error('GPU/export checks require an explicit new --output directory')
    if ARGS.runtime and (not ARGS.capture or not (ARGS.runtime/'runtime.json').is_file()):
        parser.error('--runtime requires --capture and an installed runtime.json')
    owned_temporary = ARGS.output is None
    if owned_temporary:
        scratch = ROOT/'build'/'player-settings-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        # mkdtemp owns a fresh exclusive directory. Keep it on failure instead
        # of relying on TemporaryDirectory's unconditional destructor cleanup.
        ARGS.output = Path(tempfile.mkdtemp(prefix='protocol-', dir=scratch)).resolve()
    else:
        ARGS.output = ARGS.output.resolve(); ARGS.output.mkdir(parents=True, exist_ok=False)
    EVIDENCE = {'passed': False, 'binary_sha256': sha(ARGS.binary),
                'verifier_sha256': sha(Path(__file__)), 'rpc_count': 0, 'owners': [],
                'limitations': ['No gameplay ABI extension or live settings menu tested',
                                'Read-only mutation rejection requires a native WorldSession gate']}
    try:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SettingsContract)
        run = unittest.TextTestRunner(verbosity=2).run(suite)
        EVIDENCE.update(tests_run=run.testsRun, failures=len(run.failures), errors=len(run.errors))
        if not run.wasSuccessful():
            raise AssertionError('Settings protocol contract failed')
        if ARGS.capture:
            capture_gate()
        else:
            EVIDENCE['vulkan'] = {'skipped': True, 'reason': '--capture not requested'}
        assert sha(ARGS.binary) == EVIDENCE['binary_sha256']
        assert sha(Path(__file__)) == EVIDENCE['verifier_sha256']
        EVIDENCE['passed'] = True
    finally:
        (ARGS.output/'evidence.json').write_text(json.dumps(EVIDENCE, indent=2)+'\n', encoding='utf-8')
        if not EVIDENCE['passed']:
            print(f'Settings contract evidence retained: {ARGS.output}', file=sys.stderr)
    if owned_temporary:
        shutil.rmtree(ARGS.output)  # Only this run's exclusively created directory.
    print(json.dumps({key: EVIDENCE[key] for key in ('passed', 'tests_run', 'rpc_count')}))


if __name__ == '__main__':
    main()
