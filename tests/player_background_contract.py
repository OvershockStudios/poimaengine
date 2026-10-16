#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded actual Windows background-player protocol and compiled UI exercise.

Requires an existing Vulkan/simulation/UI binary and the retained ABI7/min176
ManagedUiGame NativeAOT artifact. Run Windows Python in an externally supervised
Windows Job. No build, download, physical input or focus write occurs. One paused
Vulkan BMP readback is a renderer observation, not an OS screenshot.
This is a distinct elapsed-clock, zero-input lane, not foreground qualification,
a performance benchmark, or proof of the supervising Job's descendant closure.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys
import struct
import time
import traceback
import uuid

from workflow_response_native import Recorded


def need(value, message):
    if not value:
        raise AssertionError(message)


def exact(left, right):
    if type(left) is not type(right):
        return False
    if type(left) is dict:
        return left.keys() == right.keys() and all(exact(left[k], right[k]) for k in left)
    if type(left) is list:
        return len(left) == len(right) and all(exact(a, b) for a, b in zip(left, right))
    return left == right


def pin(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 256*1048576,
         'Expected bounded ordinary input file: '+str(path))
    with path.open('rb') as stream:
        return dict(bytes=path.stat().st_size,
                    sha256=hashlib.file_digest(stream, 'sha256').hexdigest())


def read(path):
    need(path.stat().st_size <= 16*1048576, 'JSON file exceeds 16 MiB')
    return json.loads(path.read_text(encoding='utf-8'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def uid(number):
    return f'{number:032x}'


def fresh():
    return uuid.uuid4().hex


def assigned_job():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel.IsProcessInJob.restype = wintypes.BOOL
    assigned = wintypes.BOOL()
    need(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(assigned)) and assigned.value,
         'An external supervising Windows Job is required; no forced cleanup is supplied')


def descriptor_inputs(path):
    document = read(path)
    need(document['format'] == 'poima.native-gameplay' and document['version'] == 2 and
         document['identity'] == 'poima.test.managed-ui' and
         document['type'] == 'Poima.Tests.ManagedUiGame' and
         document['target_os'] == 'Windows' and document['target_arch'] == 'x86_64' and
         document['call_version'] == 1 and document['call_bytes'] == 80 and
         document['services_version'] == 7 and document['minimum_services_bytes'] == 176,
         'Use the original qualified managed-UI artifact, not a replacement oracle')
    expected = {path.name: pin(path)}
    for row in document['files']:
        relative = Path(row['path'])
        need(not relative.is_absolute() and '..' not in relative.parts and
             relative.as_posix().casefold() not in {key.casefold() for key in expected},
             'Unsafe/duplicate artifact member')
        target = path.parent / relative
        need(target.resolve().is_relative_to(path.parent), 'Artifact member escapes its root')
        identity = pin(target)
        need(identity == dict(bytes=row['size'], sha256=row['sha256']), 'Artifact bytes differ')
        expected[relative.as_posix()] = identity
    actual = {item.relative_to(path.parent).as_posix(): pin(item)
              for item in sorted(path.parent.rglob('*')) if item.is_file()}
    need(exact(actual, expected), 'Artifact complete membership differs')
    return expected


def window_fact(pid):
    """Read only the actual HWND belonging to this fixture's native host PID."""
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user.EnumWindows.restype = wintypes.BOOL
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    getter = user.GetWindowLongPtrW if ctypes.sizeof(ctypes.c_void_p) == 8 else user.GetWindowLongW
    getter.argtypes = [wintypes.HWND, ctypes.c_int]
    getter.restype = ctypes.c_ssize_t
    matches = []

    @callback
    def collect(hwnd, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            text = ctypes.create_unicode_buffer(user.GetWindowTextLengthW(hwnd)+1)
            user.GetWindowTextW(hwnd, text, len(text))
            if text.value.startswith('Poima player'):
                matches.append(dict(hwnd=int(hwnd), pid=owner.value, title=text.value,
                                    extended_style=int(getter(hwnd, -20))))
        return True

    need(user.EnumWindows(collect, 0), 'Could not enumerate actual owned windows')
    need(len(matches) == 1, 'Expected exactly one real owned player HWND')
    result = matches[0]
    result['is_foreground'] = user.GetForegroundWindow() == result['hwnd']
    need(result['extended_style'] & 0x08000000 and not result['is_foreground'],
         'Background window lacks WS_EX_NOACTIVATE or owns foreground')
    return result


def readiness(owner, paused=None):
    state = owner['readiness']
    need(owner['mode'] == state['mode'] == 'background' and
         state['clock_mode'] == 'elapsed_zero_input' and
         state['hardware_input_enabled'] is False and state['replay'] is False and
         state['interactive_active'] is False, 'Wrong background lane/readiness')
    if state['initialized']:
        need(state['focusable'] is False and state['focused'] is False,
             'Actual background window policy/focus observation differs')
    else:
        need(state['focusable'] is None, 'Uninitialized policy invented an OS observation')
    expected_pause = ['owner_pause'] if state['owner_paused'] else []
    expected_ready = [key for key, blocked in (
        ('backend_unavailable', not state['backend_available']), ('not_initialized', not state['initialized']),
        ('no_current_presentation', not state['ui_presented']), ('closing', state['closing']),
        ('finished', state['finished'])) if blocked]
    need(state['pause_conditions'] == expected_pause and state['ready_conditions'] == expected_ready and
         owner['paused'] is bool(expected_pause) and owner['ready'] is (owner['active'] and not expected_ready),
         'Background readiness predicates differ from actual state')
    if paused is not None:
        need(owner['paused'] is paused, 'Unexpected owner pause state')
    return state


def entity(number, position, parent=None, **components):
    rows = [dict(op='entity.create', id=uid(number), name='Background '+str(number), parent=parent),
            dict(op='component.set', id=uid(number), type='Transform', value=dict(
                position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]))]
    rows += [dict(op='component.set', id=uid(number), type=kind, value=value)
             for kind, value in components.items()]
    return rows


class Exercise:
    def __init__(self, owner, args, record, saves):
        self.owner, self.args, self.record, self.saves = owner, args, record, saves
        self.session, self.configuration = fresh(), None

    def call(self, method, params=None, error=None):
        need(len(self.owner.calls) < 1000, 'Protocol call budget exhausted')
        self.owner.budget.remaining(1)
        return self.owner.call(method, params, error)

    def observe(self):
        result = self.call('player.inspect')
        readiness(result)
        return result

    def await_owner(self, predicate, label, seconds=12):
        deadline, reads = min(time.monotonic()+seconds, self.owner.budget.deadline), []
        self.record.setdefault('waits', {})[label] = reads
        while time.monotonic() < deadline:
            result = self.observe()
            reads.append(result)
            need(result['active'], 'Player unexpectedly terminated: '+label)
            if predicate(result):
                return result
            time.sleep(.025)
        raise AssertionError('Bounded owner wait failed: '+label)

    def state(self):
        return self.call('runtime.inspect', dict(session_id=self.session))

    def game(self):
        return self.call('runtime.gameplay.inspect', dict(session_id=self.session))

    def guards(self):
        state, game = self.state(), self.game()
        registry = self.call('runtime.components', dict(session_id=self.session))
        return dict(session_id=self.session, expected_tick=state['tick'],
            expected_gameplay_revision=game['revision'], expected_component_revision=registry['component_revision'],
            **{'expected_'+key: state[key] for key in
               ('structure_revision', 'ui_revision', 'control_sequence', 'material_revision')})

    def snapshot(self, label):
        slot = 'probe-'+fresh()
        receipt = self.call('save.write', dict(request_id=fresh(), configuration_generation=self.configuration,
            slot=slot, expected_generation=0, **self.guards()))
        document = self.stored(slot)
        self.record.setdefault('snapshots', {})[label] = dict(receipt=receipt, document=document)
        return document['snapshot']

    def stored(self, slot):
        folder = self.saves/('slot-'+slot)
        manifest = read(folder/'current.json')
        row = manifest['payload']['current']
        need(Path(row['file']).name == row['file'] and
             row['generation'] == manifest['payload']['generation'], 'Save manifest join differs')
        path = folder/row['file']
        need(pin(path) == dict(bytes=row['bytes'], sha256=row['sha256']), 'Original saved bytes differ')
        document = read(path)
        need(document['format'] == 'poima.world-save' and document['version'] == 1 and
             document['snapshot']['format'] == 'poima.runtime-snapshot', 'Save envelope differs')
        return document

    def control(self, action):
        current = self.observe()
        params = dict(player_id=current['player_id'], request_id=fresh(),
                      expected_control_revision=current['control_revision'], action=action)
        return params, self.call('player.control', params)

    def ui(self, number):
        state, game = self.state(), self.game()
        result = self.call('runtime.ui.activate', dict(session_id=self.session, request_id=fresh(),
            expected_tick=state['tick'], expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'], expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], id=uid(number)))
        self.session = result['current_session_id']
        return result

    def progress(self, label):
        first = self.observe()
        readiness(first, False)
        result = self.await_owner(lambda value: value['tick'] >= first['tick']+8, label)
        readiness(result, False)
        return first, result

    def run(self):
        discovery = self.call('world.describe')
        need(discovery['session_scope'] == 'shared_headless' and
             'background' in discovery['methods']['player.start']['properties']['mode']['enum'],
             'Background mode is not discoverable in native schema')
        ops = entity(200, [0, -.5, 0], MeshRenderer=dict(primitive='box', visible=True, albedo=[.45,.18,.23]),
            BoxCollider=dict(half_extents=[10,.5,10], motion='static', mass=10, friction=.5, restitution=0))
        ops += entity(100, [0, 0, 0], CharacterController=dict(radius=.3, height=1.8, speed=4,
                     jump_speed=5, camera=uid(101)))
        ops += entity(101, [0, 1.6, 0], parent=uid(100), Camera=dict(vertical_fov=65, near=.1, far=100))
        definitions = [(1,'panel','',None,None,True),(2,'label','Original',None,1,True),
            (3,'button','Edit','edit',1,True),(4,'button','Save','save',1,True),
            (5,'button','Resume','resume',1,True),(6,'button','Pause','pause',1,True),
            (7,'button','Throw','throw',1,True),(8,'button','Modal','modal',1,True),
            (9,'panel','',None,1,False),(10,'button','Clear','clear_modal',9,True),
            (11,'button','Modal resume','resume',9,True),(13,'button','Load','load',1,True)]
        for number, kind, text, action, parent, visible in definitions:
            ops.append(dict(op='ui.element.set', id=uid(number), element=dict(
                parent=uid(parent) if parent is not None else None, name='Control '+str(number),
                kind=kind, text=text, action=action, visible=visible, enabled=True)))
        self.call('world.transact', dict(base_revision=0, request_id=fresh(), ops=ops))
        authored = pin(self.args.output/'world.json')
        self.configuration = self.call('save.configure', dict(request_id=fresh(), expected_generation=0,
                                                               root=str(self.saves)))['generation']
        self.call('runtime.start', dict(session_id=self.session, revision=1))
        loaded = self.call('runtime.gameplay.load_native', dict(session_id=self.session, request_id=fresh(),
            expected_tick=0, expected_revision=0, descriptor=str(self.args.native_descriptor),
            expected_descriptor_sha256=pin(self.args.native_descriptor)['sha256']))
        need(loaded['module']['backend'] == 'native_aot' and
             loaded['module']['schema']['identity'] == 'poima.test.managed-ui', 'Wrong actual compiled module')
        params = dict(session_id=self.session, request_id=fresh(), expected_tick=0, expected_generation=0,
            camera=uid(101), controller=uid(100), mode='background', initially_paused=True,
            gamepad=dict(mode='only_connected'), width=320, height=180, gpu=self.args.gpu, samples=1,
            frames_in_flight=1, max_frames=12000, audio=False, lens_interpolation=True,
            settings_overrides={'input.sensitivity_x':.3})
        before_start = self.snapshot('before-start')
        for change in (dict(mode='unknown'), dict(mode=True), dict(sequence=[dict(ticks=1)]),
                       dict(initially_paused=1), dict(expected_generation=0.0)):
            self.call('player.start', dict(params, **change), -32602)
        need(exact(before_start, self.snapshot('after-invalid-starts')), 'Invalid start changed full native state')
        first = self.call('player.start', params)
        readiness(first, True)
        need(first['active'] and not first['ready'] and first['state'] == 'initializing', 'Start was not deferred')
        need(exact(self.call('player.start', params), dict(first, replayed=True)), 'Start retry differs')
        current = self.await_owner(lambda value: value['ready'], 'initial-ready')
        readiness(current, True)
        need(current['report']['hardware'] and current['report']['nvrhi_errors'] == 0 and
             current['report']['input_profile']['applied'] is False, 'Native renderer/input policy differs')
        self.record['window'] = window_fact(self.owner.host.pid)
        need(exact(current['report']['gamepad'], dict(mode='disabled', assigned=None)),
             'Background player initialized/routed a hardware gamepad')
        need(exact(before_start, self.snapshot('after-native-initialize')), 'Paused initialization changed complete state')
        frozen = self.snapshot('before-readonly-guards')
        for _ in range(3):
            readiness(self.observe(), True)
            self.call('player.diagnostics.inspect', dict(player_id=current['player_id'], generation=current['generation']))
        bad = dict(player_id=current['player_id'], request_id=fresh(),
                   expected_control_revision=current['control_revision']+1, action='resume')
        self.call('player.control', bad, -32009)
        self.call('player.control', dict(bad, expected_control_revision=True), -32602)
        time.sleep(.1)
        need(exact(frozen, self.snapshot('after-readonly-guards')), 'Paused reads/errors changed complete state')
        perf, trace = fresh(), fresh()
        self.call('performance.start', dict(capture_id=perf, expected_capture_id=None,
                                          player_id=current['player_id'], capacity=4096))
        self.call('profiler.start', dict(capture_id=trace, expected_capture_id=None, capacity=65536))
        resume, resumed = self.control('resume')
        first_progress, advanced = self.progress('elapsed-progress')
        need(exact(self.call('player.control', resume), dict(resumed, replayed=True)), 'Historical resume receipt changed')
        self.call('runtime.step', dict(session_id=self.session, request_id=fresh(),
                                      expected_tick=advanced['tick'], ticks=1), -32080)
        self.control('pause')
        paused = self.observe()
        values = self.game()['module']['values']
        need(values['Ticks'] == paused['tick'] and values['Ticks'] >= 8 and values['ControlCalls'] == 0,
             'Elapsed zero-input native Tick did not invoke actual compiled gameplay')
        character = self.call('runtime.entity', dict(session_id=self.session, id=uid(100)))
        need(character['is_character'] and character['layout'] == 'column_major' and
             character['local_transform']['position'][0] == character['local_transform']['position'][2] == 0 and
             character['yaw'] == character['pitch'] == 0,
             'Zero-input advancement moved or rotated the camera-bound controller horizontally')
        self.record['zero-input-character'] = character
        before_capture = self.snapshot('before-capture')
        boundary = self.observe()
        path = self.args.output/'paused-background.bmp'
        capture_request = dict(player_id=boundary['player_id'], request_id=fresh(),
            expected_control_revision=boundary['control_revision'], session_id=self.session,
            tick=boundary['tick'], expected_structure_revision=boundary['structure_revision'],
            expected_ui_revision=boundary['ui_revision'], path=str(path))
        receipt = self.call('player.capture', capture_request)
        captured = receipt['capture']
        need(receipt['player_id'] == boundary['player_id'] and receipt['generation'] == boundary['generation'] and
             receipt['control_revision'] == boundary['control_revision'] and
             receipt['session_id'] == self.session and receipt['tick'] == boundary['tick'] and
             captured['capture_written'] is True and captured['hardware'] and captured['nvrhi_errors'] == 0 and
             captured['camera'] == uid(101) and captured['ui_revision'] == boundary['ui_revision'] and
             captured['lens_presentation']['target_tick'] == boundary['tick'],
             'Paused background capture lost its exact owner/camera/UI/lens boundary')
        image = path.read_bytes()
        need(len(image) <= 4*1048576 and image[:2] == b'BM' and
             struct.unpack_from('<I', image, 2)[0] == len(image) and
             struct.unpack_from('<ii', image, 18) == (320, 180) and
             struct.unpack_from('<H', image, 28)[0] in (24, 32),
             'Actual capture is not the requested bounded320x180 BMP')
        image_pin = pin(path)
        image_stamp = path.stat().st_mtime_ns
        need(exact(self.call('player.capture', capture_request), dict(receipt, replayed=True)) and
             pin(path) == image_pin and path.stat().st_mtime_ns == image_stamp,
             'Historical capture retry changed receipt or rewrote original BMP')
        need(exact(before_capture, self.snapshot('after-capture')),
             'Paused capture/retry changed complete native state')
        self.record['capture'] = dict(request=capture_request, receipt=receipt, image=image_pin,
                                     path=str(path), scope='Renderer BMP readback; not OS screenshot')
        self.ui(8)  # Compiled Control opens the modal without a Tick.
        self.ui(11)  # Compiled Control resumes while the modal remains open.
        self.progress('modal-does-not-block-background-clock')
        self.control('pause')
        self.ui(10)
        self.ui(6)  # Compiled owner Pause is valid even when already paused.
        before_throw = self.snapshot('before-throw')
        state, game = self.state(), self.game()
        self.call('runtime.ui.activate', dict(session_id=self.session, request_id=fresh(), expected_tick=state['tick'],
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'], expected_structure_revision=state['structure_revision'],
            id=uid(7)), -32040)
        need(exact(before_throw, self.snapshot('after-throw')), 'Failed compiled Control changed complete state')
        self.ui(4)
        saved = self.stored('ui-control')['snapshot']
        self.record['compiled-save'] = saved
        self.ui(3)
        old_owner = self.observe()
        result = self.ui(13)
        need(result['runtime_replaced'] and self.session != old_owner['session_id'], 'Compiled Load did not replace runtime')
        replaced = self.await_owner(lambda value: value['ready'], 'replacement-ready')
        readiness(replaced, True)
        need(replaced['player_id'] == old_owner['player_id'] and replaced['generation'] == old_owner['generation'] and
             replaced['session_id'] == replaced['current_session_id'] == self.session and
             replaced['control_revision'] > old_owner['control_revision'], 'Replacement changed owner or failed to pause')
        need(exact(saved, self.snapshot('after-compiled-load')), 'Complete restored native snapshot differs')
        self.call('player.control', dict(player_id=old_owner['player_id'], request_id=fresh(),
            expected_control_revision=old_owner['control_revision'], action='resume'), -32009)
        self.ui(5)
        self.progress('replacement-resume-progress')
        # Minimize this owned nonactivating window only. Do not restore, focus,
        # post input, or manipulate any other application's window.
        actual_window = window_fact(self.owner.host.pid)
        user = ctypes.WinDLL('user32', use_last_error=True)
        user.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]
        user.ShowWindowAsync.restype = wintypes.BOOL
        user.IsIconic.argtypes = [wintypes.HWND]
        user.IsIconic.restype = wintypes.BOOL
        need(user.ShowWindowAsync(actual_window['hwnd'], 6), 'Owned minimize request was not accepted')
        minimized = self.await_owner(lambda value: value['readiness']['drawable'] is False,
                                    'actual-minimized-drawable')
        need(user.IsIconic(actual_window['hwnd']), 'Actual owned HWND is not minimized')
        readiness(minimized, False)
        _, advanced_minimized = self.progress('minimized-elapsed-progress')
        need(not advanced_minimized['ready'] and advanced_minimized['readiness']['drawable'] is False,
             'Minimized background falsely claims a current presentation')
        self.record['minimized-window'] = window_fact(self.owner.host.pid)
        self.control('pause')
        self.call('performance.stop', dict(capture_id=perf))
        summary = self.call('performance.summary', dict(capture_id=perf))
        rows, offset = [], 0
        while True:
            page = self.call('performance.frames', dict(capture_id=perf, offset=offset, limit=512))
            rows.extend(page['frames'])
            if page['next_offset'] is None:
                break
            offset = page['next_offset']
        focused = summary['flag_bits']['focused']
        need(rows and len(rows) == summary['count'] and summary['frames_failed'] == 0 and
             summary['present_return_cadence_eligible']['samples'] == 0 and
             all(not row['flags'] & focused for row in rows) and
             any(row['tick_after'] > row['tick_before'] for row in rows) and
             any(row['successful_present'] for row in rows), 'Background observations were treated as foreground or did not advance')
        self.record['performance'] = dict(summary=summary, frames=rows,
                                         scope='raw tiny-fixture observations; no timing qualification')
        self.call('profiler.stop', dict(capture_id=trace))
        trace_summary = self.call('profiler.summary', dict(capture_id=trace))
        events = self.call('profiler.events', dict(capture_id=trace, offset=0, limit=256))
        need(events['events'] and events['total'] > 0, 'No real profiler observations were retained')
        self.record['profiler'] = dict(summary=trace_summary, first_page=events)
        final = self.snapshot('before-stop')
        self.record['final-window'] = window_fact(self.owner.host.pid)
        self.control('stop')
        terminal = self.observe()
        need(not terminal['active'] and terminal['state'] == 'finished' and terminal['report']['success'] and
             terminal['report']['stop_reason'] == 'requested_stop' and
             terminal['report']['capture_written'] is False and terminal['report']['path'] is None and
             terminal['report']['nvrhi_errors'] == 0, 'Player did not close naturally or fabricated a configured terminal capture')
        need(exact(final, self.snapshot('after-stop')), 'Stopping presentation changed complete native state')
        need(pin(self.args.output/'world.json') == authored, 'Runtime workflow changed original authored world')
        self.call('runtime.stop', dict(session_id=self.session))
        self.record['checks'] = dict(native_nonfocusable=True, elapsed_zero_input_ticks=True,
            modal_controls=True, failed_control_full_rollback=True, same_window_save_replacement=True,
            complete_saved_state_equality=True, historical_receipts=True, foreground_qualification=False,
            minimized_elapsed_progress=True, physical_input=False, capture_requested=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--native-descriptor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without optimization')
    need(0 <= args.gpu <= 4095 and 30 <= args.timeout <= 180, 'GPU/deadline exceeds explicit bounds')
    assigned_job()
    args.binary, args.native_descriptor, args.output = [path.resolve() for path in
        (args.binary, args.native_descriptor, args.output)]
    need(not args.output.exists(), 'Output must be a new externally owned directory')
    inputs = dict(binary=pin(args.binary), descriptor=descriptor_inputs(args.native_descriptor),
                  fixture=pin(Path(__file__)), retained_transport=pin(Path(sys.modules[Recorded.__module__].__file__)))
    args.output.mkdir(parents=True)
    saves = args.output/'saves'
    saves.mkdir()
    record = dict(passed=False, inputs=inputs, gpu=args.gpu, max_wall_seconds=args.timeout,
                  forced_cleanup=[], physical_input=False, foreground_qualification=False)
    owner = None
    try:
        deadline = time.monotonic()+args.timeout
        owner = Recorded(args.binary, args.output/'owner', world=args.output/'world.json', shared=True)
        owner.budget.deadline = deadline
        Exercise(owner, args, record, saves).run()
        owner.close()
        need(pin(args.binary) == inputs['binary'] and descriptor_inputs(args.native_descriptor) == inputs['descriptor'],
             'Admitted binary/artifact changed')
        files = [path for path in args.output.rglob('*') if path.is_file()]
        need(len(files) <= 512 and sum(path.stat().st_size for path in files) <= 64*1048576,
             'Qualification output exceeds resource bounds')
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        if owner is not None:
            record['calls'], record['raw'] = owner.calls, owner.raw
            record['host_exit_code'] = None if owner.host is None else owner.host.poll()
            record['transport_exit_code'] = owner.transport.returncode
        write(args.output/'result.json', record)
    print(json.dumps(dict(passed=True, output=str(args.output), calls=len(owner.calls))))


if __name__ == '__main__':
    main()
