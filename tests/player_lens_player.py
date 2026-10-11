#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Owned running-window lens interpolation proof using the retained .123 AOT game.

External Windows Job admission is required before imports/native calls. No
compilation, pose interpolation, physical input, reconstruction or latency claim.
"""
import argparse
import copy
import json
import math
import os
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools/python'))
from live_material_player import Owner, need, uid, encoded, same, sha, compare, WIDTH, HEIGHT
from camera_zoom_player import effective, evaluate, recipe, CAM, RED, GREEN, ARM, ENV, GAME
from shadow_controls_capture import component, transform
from scene_capture import pixels
from poima_client import RpcError


def interpolated(previous, target, alpha):
    # Independent geometric-power expression; native uses log/exp interpolation.
    if alpha == 0 or previous == target:
        return previous
    if alpha == 1:
        return target
    return math.degrees(2 * math.atan(
        math.tan(math.radians(previous) / 2) ** (1 - alpha) *
        math.tan(math.radians(target) / 2) ** alpha))


def close_number(actual, expected, message):
    need(type(actual) in (int, float) and math.isfinite(actual) and
         abs(actual - expected) <= 2e-12 * max(1, abs(expected)), message)


def verify_presentation(sample, scene, *, fractional=False, paused=False):
    need(sample['enabled'] is True and type(sample['interpolating']) is bool,
         'Interpolation intent/type differs')
    alpha = sample['alpha']
    need(type(alpha) in (int, float) and math.isfinite(alpha) and 0 <= alpha <= 1,
         'Invalid owned interpolation fraction')
    for key in ('previous_tick', 'target_tick'):
        need(type(sample[key]) is int and sample[key] >= 0, 'Invalid committed bracket Tick')
    need(sample['authored_fov'] == 70 and sample['preference_fov'] == 90,
         'Frozen authored baseline or neutral preference differs')
    if fractional:
        need(sample['interpolating'] and 0 < alpha < 1 and
             sample['previous_tick'] + 1 == sample['target_tick'] and
             sample['reset_reason'] == '' and
             sample['previous_world']['vertical_fov'] != sample['target_world']['vertical_fov'],
             'No genuine changing fractional committed interval')
    if paused:
        need(not sample['interpolating'] and alpha == 1,
             'Paused capture retained an interpolation interval')
    for previous, target, rendered, world in (
        (sample['previous_world'], sample['target_world'], sample['rendered_world'], True),
        (sample['previous_viewmodel'], sample['target_viewmodel'], sample['rendered_viewmodel'], False),
    ):
        need(all(isinstance(value, dict) for value in (previous, target, rendered)),
             'Expected owned world and foreground lens endpoints')
        need(previous['near'] == target['near'] == rendered['near'] and
             previous['far'] == target['far'] == rendered['far'],
             'Clip planes were blended instead of committed')
        expected = interpolated(previous['vertical_fov'], target['vertical_fov'], alpha)
        if world:
            expected = effective(expected)
        close_number(rendered['vertical_fov'], expected, 'Rendered lens differs from independent bracket oracle')
    need(same(scene['camera_lens'], sample['rendered_world']) and
         same(scene['camera_draw']['viewmodel_lens'], sample['rendered_viewmodel']) and
         scene['camera_id'] == CAM and scene['material_revision'] == 0 and
         type(scene['frame_sequence']) is int and scene['frame_sequence'] > 0 and
         type(scene['submission_id']) is int and scene['submission_id'] > 0,
         'Actual submitted/presented scene differs from the owned rendered lens')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'output', 'native-descriptor'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, choices=(0, 1), required=True)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), required=True)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.output = args.output.resolve()
    args.native_descriptor = args.native_descriptor.resolve()
    need(os.name == 'nt' and not sys.flags.optimize and not args.output.exists(),
         'Fresh root-owned Windows fixture required')
    args.output.mkdir(parents=True)
    started = time.monotonic()
    deadline = started + 300
    descriptor = json.loads(args.native_descriptor.read_bytes())
    need(descriptor['type'] == GAME and descriptor['identity'] == 'poima.test.managed-camera-zoom' and
         descriptor['minimum_services_bytes'] == 320, 'Wrong retained native zoom consumer')
    sources = [Path(__file__), args.binary, args.binary.parent / 'phonon.dll', args.native_descriptor]
    sources += [ROOT / 'tests' / name for name in (
        'live_material_player.py', 'camera_zoom_player.py', 'scene_capture.py',
        'shadow_controls_capture.py', 'texture_fixture.py', 'gltf_fixture.py')]
    for row in descriptor['files']:
        relative = Path(row['path'])
        target = (args.native_descriptor.parent / relative).resolve(strict=True)
        need(not relative.is_absolute() and '..' not in relative.parts and
             target.is_relative_to(args.native_descriptor.parent) and target.stat().st_size == row['size'] and
             sha(target) == row['sha256'], 'Retained AOT artifact closure differs')
        sources.append(target)
    sources += [path for path in (ROOT / 'tools/python/poima_client').rglob('*')
                if path.is_file() and path.suffix in ('.py', '.json')]
    pins = {str(path): sha(path) for path in sources}
    record = dict(passed=False, calls=[], transport=[], captures=[], checkpoints=[], checks=[],
                  comparisons=[], references=[], running_presentations=[], windows=[], current_capture_guards=[], source_pins=pins,
                  scope='Opt-in lens-only interpolation of two consecutive committed Ticks, one unchanged '
                        'retained native-AOT game. Genuine unpaused frozen captures; independent authored '
                        'projection references. No pose/input interpolation, FSR, scan-out or latency claim.')
    owner = None
    failure = None

    def game():
        return owner.rpc('runtime.gameplay.inspect', dict(session_id=owner.session))['module']['values']

    def runtime():
        return owner.rpc('runtime.inspect', dict(session_id=owner.session))

    def edit(**values):
        state = runtime()
        module = owner.rpc('runtime.gameplay.inspect', dict(session_id=owner.session))
        return owner.rpc('runtime.gameplay.edit', dict(request_id=uid(), session_id=owner.session,
            expected_tick=state['tick'], expected_revision=module['revision'],
            expected_structure_revision=state['structure_revision'], values=values))

    def ready():
        until = min(deadline, time.monotonic() + 30)
        while time.monotonic() < until:
            info = owner.inspect()
            if info['ready'] and info['report']['materials']['presented']:
                return info
            time.sleep(.01)
        raise AssertionError('No successful native window presentation')

    def start(option):
        info = owner.rpc('player.inspect')
        launch = recipe(args)
        launch.pop('capture_frames')
        if option is not None:
            launch['lens_interpolation'] = option
        tick = runtime()['tick']
        owner.player = owner.rpc('player.start', dict(**launch, session_id=owner.session,
            expected_tick=tick, expected_generation=info['generation'], request_id=uid(),
            initially_paused=True, mode='interactive', audio=False,
            gamepad=dict(mode='disabled'), settings_overrides={'camera.vertical_fov': 90}))
        record['windows'].append(dict(option=option, start=copy.deepcopy(owner.player)))
        ready()

    def presentation(info):
        materials = info['report']['materials']
        receipt = materials['presented']
        need(receipt is not None and same(receipt, materials['submitted']) and
             receipt['session_id'] == owner.session and
             receipt['player_generation'] == owner.player['generation'] and
             receipt['tick'] <= info['tick'], 'Running accepted presentation ownership differs')
        sample = receipt['lens_presentation']
        verify_presentation(sample, receipt['scene'], fractional=True)
        need(sample['target_tick'] == receipt['tick'], 'Presentation bracket differs from native receipt Tick')
        record['running_presentations'].append(copy.deepcopy(info))

    def capture(label, enabled, running=False):
        until = min(deadline, time.monotonic() + 15)
        for attempt in range(32):
            need(time.monotonic() < until, 'Exact capture guard local15s bound expired')
            info = ready()
            need(info['paused'] is not running, 'Capture pause/running phase differs')
            path = args.output / (label + '-' + str(attempt) + '.bmp')
            params = dict(player_id=info['player_id'], request_id=uid(),
                expected_control_revision=info['control_revision'], session_id=owner.session,
                expected_material_revision=0, path=str(path))
            method = 'player.capture_current' if running else 'player.capture'
            if not running:
                params['tick'] = info['tick']
            try:
                if running:
                    # Explicit atomic current capture retains every non-Tick guard;
                    # the native owner freezes its own actual accepted Tick and raw lens.
                    result = owner.rpc(method, params)
                    captured_tick = result['captured_tick']
                    need(type(captured_tick) is int and captured_tick >= info['tick'] and
                         result['tick'] == captured_tick, 'Atomic capture accepted Tick differs')
                    native = result['committed_camera_lens']
                else:
                    native = owner.rpc('runtime.camera_lens', dict(session_id=owner.session,
                        tick=info['tick'], entity=CAM))['state']
                    result = owner.rpc(method, params)
                    captured_tick = params['tick']
            except RpcError as error:
                need(error.code == -32009, 'Fresh guarded capture rejected for another reason')
                continue
            # Read the saved recipe only AFTER the accepted immutable capture. Its
            # starts/targets/TransitionTick/duration stay fixed until this fixture's
            # next explicit retarget; its current values may legitimately be newer.
            values = game()
            captured_elapsed = None
            image = result['capture']
            diagnostics = image['render_diagnostics']
            scene = diagnostics['scene_receipts']['presented']
            need(image['build_version'] == '0.0.124' and image['hardware'] and image['capture_written'] and
                 image['nvrhi_errors'] == 0 and image['width'] == WIDTH and image['height'] == HEIGHT and
                 diagnostics['lighting_path'] == args.lighting_path and
                 same(scene, diagnostics['scene_receipts']['submitted']) and
                 same(scene['camera_lens'], image['lens']), 'Actual frozen native capture failed')
            if not running:
                need(values['Ticks'] == captured_tick and
                     values['WorldCurrent'] == native['world']['vertical_fov'] and
                     values['ViewmodelCurrent'] == native['viewmodel']['vertical_fov'],
                     'Paused compiled phase differs from captured committed target')
            if enabled:
                sample = image['lens_presentation']
                verify_presentation(sample, scene, fractional=running, paused=not running)
                need(sample['target_tick'] == captured_tick and same(sample['target_world'], native['world']) and
                     same(sample['target_viewmodel'], native['viewmodel']),
                     'Frozen target differs from its actual accepted native lens authority')
                if running:
                    transition_tick = int(values['TransitionTick'])
                    captured_elapsed = captured_tick - transition_tick
                    latest_elapsed = min(values['Ticks'] - transition_tick, 600)
                    need(values['Ticks'] >= captured_tick and transition_tick >= 0 and
                         values['ActiveDurationTicks'] == values['DurationTicks'] == 600 and
                         values['ElapsedTicks'] == latest_elapsed and values['Mode'] == 0 and
                         values['ScriptActive'] == 0 and 1 < captured_elapsed < 600,
                         'Post-capture recipe does not describe the captured committed midfade interval')
                    for channel, previous, target in (
                        ('World', sample['previous_world'], sample['target_world']),
                        ('Viewmodel', sample['previous_viewmodel'], sample['target_viewmodel']),
                    ):
                        close_number(target['vertical_fov'], evaluate(values[channel + 'Start'], values[channel + 'Target'],
                            captured_elapsed, values['ActiveDurationTicks']), 'Captured target phase numeric oracle differs')
                        close_number(previous['vertical_fov'], evaluate(values[channel + 'Start'], values[channel + 'Target'],
                            captured_elapsed - 1, values['ActiveDurationTicks']), 'Captured previous phase numeric oracle differs')
                        close_number(values[channel + 'Current'], evaluate(values[channel + 'Start'], values[channel + 'Target'],
                            latest_elapsed, values['ActiveDurationTicks']), 'Post-capture latest saved phase numeric oracle differs')
                    # Observe genuinely running presentation separately; it may be a different owned frame.
                    presentation(info)
            else:
                need('lens_presentation' not in image and all(
                    receipt is None or 'lens_presentation' not in receipt
                    for receipt in info['report']['materials'].values()), 'Default-off emitted interpolation metadata')
                close_number(image['lens']['vertical_fov'], effective(native['world']['vertical_fov']),
                             'Default-off lens projection changed')
            replay = owner.rpc(method, params)
            need(same(replay, {**result, 'replayed': True}), 'Capture retry lost its frozen original sample')
            entry = dict(label=label, path=path.name, bytes=path.stat().st_size, sha256=sha(path),
                method=method, request=params, result=result, guarded_player=info,
                native_target=native, target_source='atomic_current_receipt' if running else 'guarded_getter',
                captured_tick=captured_tick, game=values, running=running,
                interpolation_enabled=enabled, captured_elapsed=captured_elapsed,
                phase_join_scope='Paused phase is exact at captured Tick; running recipe is observed after capture '
                                 'and evaluated at immutable captured Tick and its preceding committed Tick.')
            record['captures'].append(entry)
            owner.bound()
            return entry
        raise AssertionError('No successful fractional frozen capture within32fresh guard attempts')

    def current_capture_guards():
        info = ready()
        need(info['paused'], 'Atomic guard qualification requires a real paused active window')
        before = dict(runtime=runtime(), game=game(), lens=owner.rpc('runtime.camera_lens',
            dict(session_id=owner.session, tick=info['tick'], entity=CAM)))
        base = dict(player_id=info['player_id'], expected_control_revision=info['control_revision'],
            session_id=owner.session, expected_material_revision=0,
            expected_structure_revision=info['structure_revision'], expected_ui_revision=info['ui_revision'])
        cases = (
            ('wrong-player', dict(player_id=uid()), -32004, 'Player identity is not the retained owner.'),
            ('wrong-control', dict(expected_control_revision=info['control_revision'] + 1), -32009, 'Player control revision conflict.'),
            ('wrong-session', dict(session_id=uid()), -32030, 'Runtime session is absent or does not match.'),
            ('wrong-material', dict(expected_material_revision=1), -32009, 'Runtime material revision conflict.'),
            ('wrong-structure', dict(expected_structure_revision=info['structure_revision'] + 1), -32009, 'Runtime structure revision conflict.'),
            ('wrong-ui', dict(expected_ui_revision=info['ui_revision'] + 1), -32009, 'Runtime UI revision conflict.'),
            ('forbidden-tick', dict(tick=info['tick']), -32602, 'Unknown field: tick'),
        )
        for label, changes, code, message in cases:
            path = args.output / ('rejected-' + label + '.bmp')
            params = dict(base, request_id=uid(), path=str(path))
            params.update(changes)
            try:
                owner.rpc('player.capture_current', params)
            except RpcError as error:
                need(error.code == code and error.message == message, 'Atomic current capture guard rejected for another reason')
                record['current_capture_guards'].append(dict(label=label, request=params, code=code, message=message))
            else:
                raise AssertionError('Invalid atomic current capture was accepted')
            need(not path.exists(), 'Rejected atomic guard published an image')
        after = dict(runtime=runtime(), game=game(), lens=owner.rpc('runtime.camera_lens',
            dict(session_id=owner.session, tick=info['tick'], entity=CAM)))
        need(same(before, after) and owner.inspect()['tick'] == info['tick'],
             'Rejected atomic current captures changed native phase, lens or Tick')
        record['current_capture_guard_state'] = dict(before=before, after=after)
        owner.bound()

    def transition(mode, label):
        paused = owner.inspect()
        need(paused['paused'], 'Transition must be authored at a paused native boundary')
        starting = owner.rpc('runtime.camera_lens', dict(session_id=owner.session,
            tick=paused['tick'], entity=CAM))['state']
        edit(Mode=mode, DurationTicks=600)
        beginning = owner.inspect()['tick']
        owner.control('resume')
        until = min(deadline, time.monotonic() + 20)
        while owner.inspect()['tick'] < beginning + 150:
            need(time.monotonic() < until, 'Compiled real fixed Ticks did not advance')
            time.sleep(.01)
        entry = capture(label, True, True)
        need(entry['game']['WorldStart'] == starting['world']['vertical_fov'] and
             entry['game']['ViewmodelStart'] == starting['viewmodel']['vertical_fov'],
             'Compiled retarget did not seed the actual last committed native lens')
        entry['transition_start'] = dict(player=paused, native=starting)
        owner.control('pause')  # Only after the immutable running sample was actually captured.
        return entry

    try:
        owner = Owner(args, record, deadline)
        need(owner.rpc('world.describe')['schema_revision'] == 92, 'Unexpected current native schema')
        ops = [dict(op='entity.create', id=entity, name='Running lens witness',
                    parent=CAM if entity == ARM else None) for entity in (CAM, RED, GREEN, ARM, ENV)]
        ops += [transform(CAM, (0, 0, 3)), component(CAM, 'Camera',
            dict(vertical_fov=70, near=.1, far=100, viewmodel=dict(vertical_fov=75, near=.01, far=10))),
            transform(ENV), component(ENV, 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1))]
        for entity, position, scale, color in (
            (RED, (.5, 0, 0), (.3, .8, .3), [.5, 0, 0]),
            (GREEN, (-.4, .2, -.3), (.4, .3, .3), [0, .5, 0]),
            (ARM, (.2, -.15, -.5), (.15, .08, .1), [0, 0, .5]),
        ):
            ops += [transform(entity, position, scale), component(entity, 'MeshRenderer',
                dict(primitive='box', visible=True, albedo=[0, 0, 0])), component(entity, 'PbrMaterial',
                dict(base_color=[0, 0, 0], emissive=color, metallic=0, roughness=1, double_sided=True))]
        ops.append(component(ARM, 'CameraDraw', dict(camera=CAM, mode='viewmodel')))
        owner.revision = owner.rpc('world.transact', dict(base_revision=0, request_id=uid(), ops=ops))['revision']
        original = sha(args.output / 'world.json')
        owner.session = uid()
        owner.rpc('runtime.start', dict(session_id=owner.session, revision=owner.revision))
        owner.rpc('runtime.gameplay.load_native', dict(session_id=owner.session, request_id=uid(), expected_tick=0,
            expected_revision=0, expected_structure_revision=0, descriptor=str(args.native_descriptor),
            expected_descriptor_sha256=sha(args.native_descriptor)))
        start(None)
        omitted = capture('omitted-default', False)
        owner.control('stop')
        start(False)
        explicit = capture('explicit-false', False)
        need((args.output / omitted['path']).read_bytes() == (args.output / explicit['path']).read_bytes(),
             'Omitted and explicitfalse default BMPs differ')
        owner.control('stop')
        start(True)
        neutral = capture('enabled-paused', True)
        need((args.output / neutral['path']).read_bytes() == (args.output / omitted['path']).read_bytes(),
             'Opt-in paused baseline BMP differs from default')
        current_capture_guards()
        aimed = transition(1, 'running-aim')
        released = transition(2, 'running-release')
        # The release starts at the later committed paused lens, not the earlier rendered interpolation.
        need(released['game']['WorldStart'] <= 70 and released['game']['WorldTarget'] == 70,
             'Release committed target differs')
        edit(Mode=6)
        info = runtime()
        owner.rpc('runtime.step', dict(request_id=uid(), session_id=owner.session,
            expected_tick=info['tick'], ticks=1))
        capture('paused-snap', True)
        need(sha(args.output / 'world.json') == original, 'Presentation altered authored scene')
        owner.control('stop')
        owner.rpc('runtime.stop', dict(session_id=owner.session))
        owner.session = None
        for entry in record['captures']:
            image = entry['result']['capture']
            scene = image['render_diagnostics']['scene_receipts']['presented']
            camera = dict(image['lens'], viewmodel=scene['camera_draw']['viewmodel_lens'])
            owner.revision = owner.rpc('world.transact', dict(base_revision=owner.revision, request_id=uid(),
                ops=[component(CAM, 'Camera', camera)]))['revision']
            path = args.output / ('reference-' + entry['label'] + '.bmp')
            result = owner.rpc('world.capture', dict(**recipe(args), revision=owner.revision, path=str(path)))
            need(result['build_version'] == '0.0.124' and result['hardware'] and result['capture_written'] and
                 result['nvrhi_errors'] == 0, 'Independent authored reference missing')
            metric = compare(pixels(args.output / entry['path']), pixels(path))
            need(metric['max_rgb_delta'] <= 1, 'Frozen player lens differs from independent authored projection')
            record['references'].append(dict(label=entry['label'], path=path.name, bytes=path.stat().st_size,
                sha256=sha(path), result=result))
            record['comparisons'].append(dict(label=entry['label'], **metric))
            owner.bound()
        visible = compare(pixels(args.output / neutral['path']), pixels(args.output / aimed['path']))
        need(visible['strong_changed_pixels'] >= 50, 'Running interpolation capture has no meaningful ADS image change')
        record['visible_effect'] = visible
        record['checks'] += [
            'Seven real active current-capture owner/revision/session/schema guards reject without artifacts or native phase changes',
            'Omitted and explicitfalse options have byte-identical BMPs and omit interpolation metadata',
            'Opt-in paused/reset capture alpha1 preserves exact default baseline pixels',
            'Two unpaused atomic current samples use consecutive committed Tick endpoints and one world/VM alpha',
            'Post-capture immutable recipe derives captured/previous committed phase independently; latest current is not rendered FOV',
            'Independent compiled phase and geometric-power lens interpolation precede neutral preference composition',
            'Explicit atomic current capture freezes actual Tick/raw lens; replay retains its original sample without a second clock advance',
            'Six captures match six independent authored world/foreground projection references',
        ]
        record['passed'] = True
    except BaseException as error:
        failure = repr(error)
        record['error'] = failure
        record['traceback'] = traceback.format_exc()
    finally:
        if owner is not None:
            try:
                owner.close()
                owner.accounting()
            except BaseException as error:
                failure = failure or repr(error)
                record['cleanup_error'] = repr(error)
        try:
            record['seconds'] = time.monotonic() - started
            record['changed_sources'] = [name for name, digest in pins.items()
                if not Path(name).is_file() or sha(name) != digest]
            need(not record['changed_sources'], 'Frozen fixture/artifact/source inputs changed')
            need(record['seconds'] <= 300, 'Whole fixture300s bound expired')
            record['passed'] = bool(record['passed'] and failure is None)
            data = encoded(record) + b'\n'
            need(sum(path.stat().st_size for path in args.output.rglob('*') if path.is_file()) + len(data)
                 <= 64 * 1048576, 'Whole fixture64MiB output cap exceeded')
            output = args.output / 'evidence.json'
            output.write_bytes(data)
            need(time.monotonic() <= deadline, 'Whole300s bound expired during publication')
            need(sum(path.stat().st_size for path in args.output.rglob('*') if path.is_file())
                 <= 64 * 1048576, 'Published output cap exceeded')
        except BaseException as error:
            failure = failure or repr(error)
            record['passed'] = False
            output = args.output / 'evidence.json'
            if output.exists():
                output.rename(args.output / 'unqualified-evidence.json')
            (args.output / 'failure.json').write_bytes(encoded(dict(passed=False, error=failure,
                publication_error=repr(error), seconds=time.monotonic() - started)) + b'\n')
    print(json.dumps(dict(passed=record['passed'], captures=len(record['captures']), error=failure)))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
