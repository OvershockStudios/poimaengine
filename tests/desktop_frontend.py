#!/usr/bin/env python3
"""Real Windows desktop + shared service/semantic model qualification.

Only the owned window is raised for its screenshot, then its topmost state is
restored. No global input injection. Semantic actions do not qualify physical
keyboard, drag/drop, IME or accessibility behavior.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid
from PIL import ImageGrab

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--editor', type=Path, required=True)
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gpu', type=int, default=1)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
run = a.output.resolve()/uuid.uuid4().hex
run.mkdir(parents=True)
project = run/'Courtyard'
record = {'passed': False, 'gpu': a.gpu, 'input': 'Semantic C# editor actions and shared CLI commands; no physical input.',
          'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'editor_sha256': hashlib.sha256((a.editor.parent/'Poima.Editor.dll').read_bytes()).hexdigest()}
def cli(arguments, data=None, timeout=30):
    value = subprocess.run([str(a.binary.resolve()), *map(str, arguments)], input=data, capture_output=True,
                           text=True, encoding='utf-8', timeout=timeout)
    assert value.returncode == 0, value.stdout+value.stderr
    return [json.loads(line) for line in value.stdout.splitlines()]
cli(['project', 'create', project, '--name', 'Courtyard'])
world = project/'world.json'
initial = json.loads(world.read_text())['revision']
seed = json.loads((root/'examples/courtyard.jsonl').read_text().splitlines()[0])
seed['params']['base_revision'] = initial
seed['params']['request_id'] = uuid.uuid4().hex
for op in seed['params']['ops']:
    for field in ('id', 'parent'):
        if op.get(field): op[field] = f'{int(op[field], 16)+100:032x}'
# The courtyard replaces the starter floor; keeping both coplanar surfaces
# would turn this UI capture into an unrelated z-fighting fixture.
seed['params']['ops'].insert(0, {'op': 'entity.delete', 'id': f'{1:032x}', 'recursive': True})
seed_reply = cli(['world', world], json.dumps(seed)+'\n')[0]
assert 'result' in seed_reply, seed_reply
for folder in ('Assets/Materials', 'Assets/Models', 'Assets/Textures', 'Assets/Audio', 'Scripts'):
    (project/folder).mkdir(parents=True, exist_ok=True)
(project/'Assets/Materials/README.txt').write_text('Materials are authored through native components.\n')
revision = initial+1
entity = f'{105:032x}'
endpoint = 'desktop-ui-'+uuid.uuid4().hex
actions = []
def action(frame, op, **values): actions.append(dict(frame=frame, op=op, **values))
action(3, 'select', id=entity)
action(5, 'assert_draft', expected={'dirty': False})
action(6, 'set_text', control='Object name', text='Preserved human draft')
action(8, 'rpc', method='world.transact', params={'base_revision': revision, 'request_id': uuid.uuid4().hex,
       'ops': [{'op': 'entity.rename', 'id': entity, 'name': 'External edit'}]})
action(11, 'assert_draft', expected={'dirty': True, 'conflict': True, 'name': 'Preserved human draft'})
action(12, 'apply', error_contains='(-32009)')
action(13, 'reload')
action(14, 'assert_draft', expected={'dirty': False, 'conflict': False, 'name': 'External edit'})
action(15, 'set_text', control='Object name', text='Courtyard pillar')
action(16, 'apply')
action(18, 'undo')
action(19, 'assert_draft', expected={'name': 'External edit', 'dirty': False})
action(20, 'redo')
action(22, 'assert_draft', expected={'name': 'Courtyard pillar', 'dirty': False})
action(23, 'set_text', control='Position X', text='not a number')
action(24, 'apply', error_contains='invalid Inspector')
action(25, 'close_guard')
action(26, 'float', panel='Inspector')
action(28, 'assert_text', control='Position X', text='not a number')
action(30, 'inspect')
action(33, 'reset_layout')
action(35, 'wait_text', control='Position X', text='not a number')
action(36, 'apply', error_contains='invalid Inspector')
action(37, 'reload')
action(38, 'float', panel='Scene')
action(44, 'inspect')
action(47, 'reset_layout')
action(53, 'runtime_toggle')
action(53, 'runtime_pause')  # Same-frame pause: no clock pump between start and pause.
action(56, 'step', ticks=2)
action(58, 'inspect')
action(60, 'runtime_toggle')
# A valid authored reference can outlive its asset. Correcting the document
# must recover drawing without rebuilding the viewport or losing the session.
mesh = next(op['value'] for op in seed['params']['ops']
            if op.get('id') == entity and op.get('type') == 'MeshRenderer')
action(64, 'rpc', method='world.transact', params={'base_revision': revision+4, 'request_id': uuid.uuid4().hex,
       'ops': [{'op': 'component.remove', 'id': entity, 'type': 'MeshRenderer'},
               {'op': 'component.set', 'id': entity, 'type': 'StaticMesh',
                'value': {'asset': 'f'*64, 'primitive': 0, 'visible': True}}]})
action(68, 'assert_scene_error', expected=True)
action(70, 'rpc', method='world.transact', params={'base_revision': revision+5, 'request_id': uuid.uuid4().hex,
       'ops': [{'op': 'component.remove', 'id': entity, 'type': 'StaticMesh'},
               {'op': 'component.set', 'id': entity, 'type': 'MeshRenderer', 'value': mesh}]})
action(75, 'wait_scene_error', expected=False)
action(80, 'rpc', method='desktop.capture', params={'revision': revision+6, 'path': str(run/'viewport.bmp')})
action(85, 'inspect')
action(95, 'scene_frame')
action(97, 'inspect')
action(98, 'scene_input', message='right_down', x=.5, y=.5)
action(99, 'scene_input', message='move', x=.56, y=.46)
action(100, 'scene_input', message='key_down', key=87)
action(101, 'scene_step', seconds=.1)
action(102, 'scene_input', message='key_up', key=87)
action(103, 'scene_input', message='right_up', x=.56, y=.46)
action(104, 'scene_input', message='middle_down')
action(104, 'scene_input', message='move', x=.53, y=.5)
action(104, 'scene_input', message='middle_up', x=.53, y=.5)
action(104, 'scene_input', message='wheel', delta=120)
action(105, 'inspect')
action(106, 'scene_input', message='key_down', key=70)
action(107, 'scene_input', message='key_up', key=70)
action(109, 'scene_input', message='left_down')
action(110, 'scene_input', message='left_up')
action(112, 'assert_draft', expected={'entity': entity, 'dirty': False})
action(115, 'float', panel='Inspector')
action(120, 'save_layout')
action(123, 'reset_layout')
action(130, 'load_layout')
action(135, 'wait_text', control='Position X', text='-4')
action(138, 'inspect')
action(142, 'reset_layout')
action(150, 'scene_input', message='right_down')
action(151, 'scene_input', message='key_down', key=87)
action(152, 'scene_input', message='cancel')
action(153, 'scene_step', seconds=.1)
action(155, 'inspect')
# Keep the established authoring/layout checks above unchanged. These gestures
# enter through this process's actual child HWND subclass, not a simulated
# transform implementation. Named stages are matched to action results below.
action(160, 'scene_frame')
action(161, 'gizmo_configure', mode='move', space='world')
action(162, 'inspect', tag='gizmo_baseline')
action(162, 'rpc', method='world.history', tag='history_baseline')
action(163, 'draft_name', name='Unapplied gizmo draft')
action(164, 'gizmo_begin', axis='z', error_contains='Apply or reload Inspector', tag='dirty_gizmo')
action(165, 'assert_draft', expected={'dirty': True, 'name': 'Unapplied gizmo draft'})
action(166, 'reload')
action(168, 'scene_input', message='left_down', axis='z', tag='gizmo_press')
action(169, 'scene_input', message='move', gizmo_relative=True, dx=45, dy=-10)
action(170, 'inspect', tag='gizmo_preview')
action(170, 'rpc', method='entity.get', params={'id': entity}, tag='preview_authored_entity')
action(171, 'scene_input', message='left_up', gizmo_relative=True, dx=45, dy=-10)
action(172, 'inspect', tag='gizmo_committed')
action(172, 'rpc', method='world.history', tag='history_committed')
action(173, 'undo')
action(174, 'inspect', tag='gizmo_undo')
action(175, 'redo')
action(176, 'inspect', tag='gizmo_redo')
action(177, 'scene_frame')
action(178, 'scene_input', message='left_down', axis='z')
action(179, 'scene_input', message='move', gizmo_relative=True, dx=30, dy=15)
action(180, 'inspect', tag='cancel_preview')
action(181, 'scene_input', message='cancel')
action(182, 'inspect', tag='gizmo_cancelled')
action(183, 'scene_input', message='left_up', gizmo_relative=True, dx=30, dy=15)
action(184, 'scene_input', message='left_down', axis='x')
action(185, 'scene_input', message='move', gizmo_relative=True, dx=35, dy=0)
action(186, 'scene_input', message='key_down', key=27)
action(187, 'scene_input', message='key_up', key=27)
action(188, 'inspect', tag='gizmo_escaped')
action(190, 'scene_input', message='left_down', axis='z')
action(191, 'scene_input', message='move', gizmo_relative=True, dx=25, dy=-10)
action(192, 'rpc', method='world.transact', params={'base_revision': revision+9, 'request_id': uuid.uuid4().hex,
       'ops': [{'op': 'entity.rename', 'id': f'{106:032x}', 'name': 'Externally named pillar foot'}]}, tag='gizmo_external_edit')
action(194, 'inspect', tag='gizmo_invalidated')
action(195, 'scene_input', message='left_up', gizmo_relative=True, dx=25, dy=-10)
for frame, key, mode in ((196, 81, 'none'), (199, 87, 'move'), (202, 69, 'rotate'), (205, 82, 'scale')):
    action(frame, 'scene_input', message='key_down', key=key)
    action(frame+1, 'scene_input', message='key_up', key=key)
    action(frame+2, 'gizmo_inspect', tag='hotkey_'+mode)
action(207, 'scene_input', message='key_down', key=87, control=True)
action(207, 'scene_input', message='key_up', key=87, control=True)
action(207, 'gizmo_inspect', tag='ctrl_hotkey_blocked')
action(207, 'scene_input', message='key_down', key=69, alt=True)
action(207, 'scene_input', message='key_up', key=69, alt=True)
action(207, 'gizmo_inspect', tag='alt_hotkey_blocked')
action(208, 'gizmo_configure', mode='move', space='local')
action(209, 'gizmo_inspect', tag='space_local')
action(210, 'gizmo_configure', mode='rotate', space='world')
action(211, 'scene_input', message='right_down')
action(212, 'scene_input', message='key_down', key=87)
action(213, 'scene_input', message='key_down', key=69)
action(214, 'inspect', tag='rmb_modes')
action(215, 'scene_input', message='key_up', key=87)
action(215, 'scene_input', message='key_up', key=69)
action(216, 'scene_input', message='right_up')
action(218, 'gizmo_configure', mode='move', space='world')
action(220, 'scene_frame')
action(222, 'gizmo_inspect', tag='final_handles')
action(224, 'rpc', method='desktop.capture', params={'revision': revision+10, 'path': str(run/'gizmo-final.bmp')})
action(227, 'inspect', tag='gizmo_final')
action(230, 'save_layout')
# Real-time playback shares the native clock with agent commands. Use tick
# inequalities while playing and exact ticks only after a confirmed pause.
action(240, 'inspect', tag='playback_before')
action(240, 'rpc', method='world.history', tag='playback_history_before')
action(242, 'runtime_toggle')
action(244, 'inspect', tag='playback_running_first')
action(244, 'step', error_contains='Pause playback')
action(250, 'inspect', tag='playback_running_later')
action(251, 'runtime_pause')
action(252, 'inspect', tag='playback_paused')
action(258, 'inspect', tag='playback_still_paused')
action(259, 'step')
action(260, 'inspect', tag='playback_stepped')
action(261, 'view', mode='game')
action(263, 'inspect', tag='game_preview')
action(264, 'scene_input', message='right_down')
action(264, 'scene_input', message='move', x=.65, y=.35)
action(265, 'scene_input', message='right_up')
action(266, 'inspect', tag='game_navigation_blocked')
action(267, 'rpc', method='desktop.capture', params={'revision': revision+10, 'path': str(run/'game-preview.bmp')})
action(270, 'inspect', tag='game_capture')
action(271, 'view', mode='scene')
action(272, 'inspect', tag='scene_restored')
action(273, 'runtime_rpc', command='resume')
action(278, 'inspect', tag='playback_external_resume')
action(279, 'runtime_rpc', command='pause')
action(281, 'inspect', tag='playback_external_pause')
action(287, 'inspect', tag='playback_external_pause_stable')
action(288, 'runtime_toggle')
action(290, 'inspect', tag='playback_stopped')
action(290, 'rpc', method='world.history', tag='playback_history_after')
script = run/'actions.json'; script.write_text(json.dumps({'actions': actions}, indent=2))
report = run/'report.json'
u = c.WinDLL('user32', use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.IsWindowVisible.argtypes = [w.HWND]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.GetWindowLongPtrW.argtypes = [w.HWND, c.c_int]; u.GetWindowLongPtrW.restype = c.c_ssize_t
u.SetThreadDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetThreadDpiAwarenessContext(c.c_void_p(-4))
u.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]; u.OpenInputDesktop.restype = w.HANDLE
u.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
u.GetUserObjectInformationW.restype = w.BOOL
u.CloseDesktop.argtypes = [w.HANDLE]; u.CloseDesktop.restype = w.BOOL
wts = c.WinDLL('wtsapi32', use_last_error=True)
wts.WTSQuerySessionInformationW.argtypes = [w.HANDLE, w.DWORD, c.c_int, c.POINTER(c.c_void_p), c.POINTER(w.DWORD)]
wts.WTSQuerySessionInformationW.restype = w.BOOL
wts.WTSFreeMemory.argtypes = [c.c_void_p]
class WtsSessionLevel1(c.Structure):
    _fields_ = [('SessionId', w.DWORD), ('SessionState', c.c_int), ('SessionFlags', w.LONG),
                ('WinStationName', w.WCHAR * 33), ('UserName', w.WCHAR * 21), ('DomainName', w.WCHAR * 18),
                ('LogonTime', c.c_longlong), ('ConnectTime', c.c_longlong), ('DisconnectTime', c.c_longlong),
                ('LastInputTime', c.c_longlong), ('CurrentTime', c.c_longlong),
                ('IncomingBytes', w.DWORD), ('OutgoingBytes', w.DWORD),
                ('IncomingFrames', w.DWORD), ('OutgoingFrames', w.DWORD),
                ('IncomingCompressedBytes', w.DWORD), ('OutgoingCompressedBytes', w.DWORD)]
class WtsSessionInfo(c.Structure):
    _fields_ = [('Level', w.DWORD), ('Data', WtsSessionLevel1)]
def input_desktop_available():
    # Read-only check: never unlock, switch desktops or inject global input.
    # Modern lock surfaces may coexist with an accessible Default desktop.
    # Require an explicitly unlocked current session as well (Windows 10+).
    # https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w
    buffer = c.c_void_p(); size = w.DWORD()
    if not wts.WTSQuerySessionInformationW(None, 0xFFFFFFFF, 25, c.byref(buffer), c.byref(size)):
        return False, f'Cannot verify current session lock state (error {c.get_last_error()}).'
    try:
        if not buffer or size.value < c.sizeof(WtsSessionInfo):
            return False, 'Current session lock-state response is incomplete.'
        session = c.cast(buffer, c.POINTER(WtsSessionInfo)).contents
        if session.Level != 1 or session.Data.SessionFlags != 1 or session.Data.SessionState != 0:
            return False, f'Current session is locked, disconnected or unknown (level {session.Level}, state {session.Data.SessionState}, flags {session.Data.SessionFlags}).'
    finally:
        wts.WTSFreeMemory(buffer)
    desktop = u.OpenInputDesktop(0, False, 0x0001)  # DESKTOP_READOBJECTS
    if not desktop:
        return False, f'Input desktop unavailable (OpenInputDesktop error {c.get_last_error()}); it may be locked.'
    try:
        name = c.create_unicode_buffer(256); needed = w.DWORD()
        if not u.GetUserObjectInformationW(desktop, 2, name, c.sizeof(name), c.byref(needed)):  # UOI_NAME
            return False, f'Cannot identify input desktop (error {c.get_last_error()}).'
        if name.value.lower() != 'default':
            return False, f'Input desktop is {name.value!r}, not Default; no window screenshot taken.'
        return True, 'Default input desktop is available.'
    finally:
        u.CloseDesktop(desktop)
record['window_capture'] = {'status': 'not_attempted'}
process = None; raised = None; was_topmost = False
try:
    with (run/'stdout.txt').open('w', encoding='utf-8') as out, (run/'stderr.txt').open('w', encoding='utf-8') as err:
        command = [str(a.editor.resolve()), str(world), '--gpu', str(a.gpu), '--endpoint', endpoint,
                   '--script', str(script), '--frames', '420', '--report', str(report), '--layout', str(run/'layout.json')]
        process = subprocess.Popen(command, stdout=out, stderr=err)
        start = time.monotonic(); captured = False; shared = False; next_status = 0; capture_ready = False
        while process.poll() is None and time.monotonic()-start < 70:
            handles = []
            @callback
            def visit(hwnd, _):
                pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
                if pid.value == process.pid and u.IsWindowVisible(hwnd): handles.append(hwnd)
                return True
            u.EnumWindows(visit, 0)
            elapsed = time.monotonic()-start
            if handles and elapsed > 1 and not shared:
                try:
                    reply = cli(['connect', endpoint], '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}\n', timeout=5)[0]
                except (subprocess.TimeoutExpired, AssertionError):
                    if process.poll() is not None: break  # Preserve the editor's actual failure report.
                    raise
                assert 'result' in reply, reply
                record['shared_cli'] = reply['result']; shared = True
            if shared and elapsed > 5 and elapsed >= next_status and not captured:
                next_status = elapsed + .5
                try:
                    state = cli(['connect', endpoint], '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}\n', timeout=5)[0]['result']
                except (subprocess.TimeoutExpired, AssertionError):
                    if process.poll() is not None: break
                    raise
                capture = state.get('capture') or {}
                capture_ready = (capture.get('state') == 'complete'
                                 and capture.get('path', '').lower().endswith('gizmo-final.bmp')
                                 and state.get('selected') == entity
                                 and state.get('gizmo', {}).get('mode') == 'move'
                                 and state.get('gizmo', {}).get('active') is None)
            if len(handles) == 1 and capture_ready and not captured:
                available, reason = input_desktop_available()
                if available:
                    raised = handles[0]; was_topmost = bool(u.GetWindowLongPtrW(raised, -20) & 8)
                    assert u.SetWindowPos(raised, w.HWND(-1), 60, 40, 1440, 920, 0x10)
                    time.sleep(.35)
                    available, reason = input_desktop_available()
                    if available:
                        rectangle = w.RECT(); origin = w.POINT()
                        assert u.GetClientRect(raised, c.byref(rectangle)) and u.ClientToScreen(raised, c.byref(origin))
                        # Capture only the owned client bounds, excluding OS-frame
                        # corners. Recheck before publication if the desktop locks.
                        screenshot = ImageGrab.grab(bbox=(origin.x, origin.y, origin.x+rectangle.right, origin.y+rectangle.bottom), all_screens=True)
                        available, reason = input_desktop_available()
                        if available:
                            screenshot.save(run/'window.png')
                        screenshot.close()
                    u.SetWindowPos(raised, w.HWND(-1 if was_topmost else -2), 0, 0, 0, 0, 0x13)
                    raised = None
                record['window_capture'] = ({'status': 'captured', 'path': 'window.png'} if available
                                            else {'status': 'unavailable', 'reason': reason})
                captured = True  # An explicit unavailable result is not a GUI screenshot.
            time.sleep(.05)
        if process.poll() is None: raise AssertionError('Desktop process did not exit within 70 seconds.')
        record['exit_code'] = process.returncode
        assert report.is_file(), (run/'stderr.txt').read_text()
        evidence = json.loads(report.read_text()); record['desktop'] = evidence
        assert process.returncode == 0 and evidence['success'], evidence
        assert evidence['ui_backend_actual'] == 'Avalonia.Vulkan.VulkanPlatformGraphics', evidence
        assert evidence['font_resolved'] == 'Inter', evidence
        assert len(evidence['actions']) == len(actions)
        stages = {spec['tag']: result for spec, result in zip(actions, evidence['actions']) if 'tag' in spec}
        inspections = [item for item in evidence['actions'] if item['op'] == 'inspect']
        assert inspections[5]['native']['camera']['yaw'] != inspections[4]['native']['camera']['yaw']
        assert inspections[5]['native']['camera']['position'] != inspections[4]['native']['camera']['position']
        assert inspections[5]['navigation']['pressed_keys'] == 0 and inspections[5]['navigation']['drag'] == 'None'
        assert len(inspections[6]['windows']) == 2
        saved = next(item['layout'] for item in evidence['actions'] if item['op'] == 'save_layout')
        assert inspections[6]['layout'] == saved, (saved, inspections[6]['layout'])
        cancelled = next(item for item in evidence['actions'] if item.get('message') == 'cancel')
        assert inspections[7]['native']['camera'] == cancelled['camera']
        assert inspections[7]['navigation']['pressed_keys'] == 0 and inspections[7]['navigation']['drag'] == 'None'
        assert evidence['navigation']['error_count'] == 0 and evidence['navigation']['input_error'] is None
        assert evidence['layout_error'] is None
        assert len(inspections[0]['windows']) == 2 and len(inspections[1]['windows']) == 2, inspections
        assert inspections[1]['native']['frames_presented'] > inspections[0]['native']['frames_presented']
        assert inspections[2]['native']['runtime']['tick'] == 2
        assert inspections[3]['native']['capture']['state'] == 'complete'
        assert evidence['state']['render']['hardware'] and evidence['state']['render']['nvrhi_errors'] == 0
        assert not evidence['state']['runtime']['active']
        recovery = [item for item in evidence['actions'] if item['op'] in ('assert_scene_error', 'wait_scene_error')]
        assert recovery[1]['native']['frames_presented'] > recovery[0]['native']['frames_presented']
        assert recovery[1]['native']['presented_revision'] == recovery[1]['native']['revision']
        assert evidence['draft']['name'] == 'Courtyard pillar' and not evidence['draft']['dirty']
        baseline, preview, committed = (stages[name] for name in ('gizmo_baseline', 'gizmo_preview', 'gizmo_committed'))
        original_transform = baseline['draft']['components']['Transform']
        assert stages['dirty_gizmo'].get('expected_error') and not baseline['draft']['dirty']
        assert stages['gizmo_press']['navigation']['gizmo_drag_id'] is not None
        assert preview['native']['revision'] == baseline['native']['revision'], preview
        assert preview['draft']['components']['Transform'] == original_transform and not preview['draft']['dirty']
        assert stages['preview_authored_entity']['result']['value']['components']['Transform'] == original_transform
        active = preview['native']['gizmo']['active']
        assert active and active['axis'] == 'z' and active['transform'] != original_transform, preview
        changed_transform = committed['draft']['components']['Transform']
        assert changed_transform == active['transform'] and committed['native']['revision'] == baseline['native']['revision']+1
        assert committed['native']['gizmo']['active'] is None and committed['navigation']['gizmo_drag_id'] is None
        assert committed['navigation']['drag'] == 'None' and not committed['draft']['dirty']
        assert stages['history_committed']['result']['undo_count'] == stages['history_baseline']['result']['undo_count']+1
        assert stages['gizmo_undo']['draft']['components']['Transform'] == original_transform
        assert stages['gizmo_redo']['draft']['components']['Transform'] == changed_transform
        assert stages['cancel_preview']['native']['gizmo']['active']['transform'] != changed_transform
        stable_revision = stages['gizmo_redo']['native']['revision']
        for name in ('gizmo_cancelled', 'gizmo_escaped'):
            item = stages[name]
            assert item['native']['revision'] == stable_revision and item['draft']['components']['Transform'] == changed_transform, item
            assert item['native']['gizmo']['active'] is None and item['navigation']['gizmo_drag_id'] is None
            assert item['navigation']['drag'] == 'None'
        invalidated = stages['gizmo_invalidated']
        assert invalidated['native']['revision'] == stages['gizmo_external_edit']['result']['revision'] == stable_revision+1
        assert invalidated['native']['gizmo']['active'] is None and invalidated['navigation']['gizmo_drag_id'] is None
        assert invalidated['draft']['components']['Transform'] == changed_transform and invalidated['navigation']['drag'] == 'None'
        for mode in ('none', 'move', 'rotate', 'scale'):
            assert stages['hotkey_'+mode]['gizmo']['mode'] == mode
        assert stages['ctrl_hotkey_blocked']['gizmo']['mode'] == 'scale'
        assert stages['alt_hotkey_blocked']['gizmo']['mode'] == 'scale'
        for spec, result in zip(actions, evidence['actions']):
            if spec['op'] == 'scene_input':
                expected_modifiers = (1 if spec.get('shift') else 0) | (2 if spec.get('control') else 0) | (4 if spec.get('alt') else 0)
                assert result['requested_modifiers'] == result['thread_modifiers'] == result['delivered_modifiers'] == expected_modifiers, (spec, result)
                assert result['navigation']['qualification_input'], result
                if spec['message'] in ('left_down', 'left_up', 'right_down', 'right_up', 'middle_down', 'middle_up', 'move', 'wheel'):
                    assert result['navigation']['last_input_x'] == result['requested_pointer_x'], (spec, result)
                    assert result['navigation']['last_input_y'] == result['requested_pointer_y'], (spec, result)
        assert stages['space_local']['gizmo']['space'] == 'local'
        assert stages['rmb_modes']['native']['gizmo']['mode'] == 'rotate' and stages['rmb_modes']['navigation']['drag'] == 'Right'
        final = stages['gizmo_final']
        assert final['native']['selected'] == entity and final['native']['gizmo']['mode'] == 'move'
        assert final['native']['gizmo']['space'] == 'world' and final['native']['gizmo']['active'] is None
        assert sum(handle['visible'] for handle in stages['final_handles']['gizmo']['handles']) >= 2
        assert evidence['state']['revision'] == invalidated['native']['revision'] and final['draft']['components']['Transform'] == changed_transform
        before_play = stages['playback_before']
        first_play, later_play = stages['playback_running_first'], stages['playback_running_later']
        paused_play, stable_play, stepped_play = (stages[name] for name in ('playback_paused', 'playback_still_paused', 'playback_stepped'))
        assert first_play['native']['playback']['state'] == first_play['playback']['state'] == 'playing'
        assert first_play['playback_controls']['pause_enabled'] and not first_play['playback_controls']['step_enabled']
        assert first_play['playback_controls']['pause_action'] == 'Pause simulation' and first_play['playback_controls']['pause_has_vector_icon']
        assert later_play['native']['playback']['tick'] > first_play['native']['playback']['tick']
        assert paused_play['native']['playback']['state'] == paused_play['playback']['state'] == 'paused'
        assert paused_play['playback_controls']['pause_action'] == 'Resume simulation' and paused_play['playback_controls']['pause_has_vector_icon'] and paused_play['playback_controls']['step_enabled']
        assert stable_play['native']['playback']['tick'] == paused_play['native']['playback']['tick']
        assert stepped_play['native']['playback']['tick'] == stable_play['native']['playback']['tick'] + 1
        for stage in (first_play, later_play, paused_play, stable_play, stepped_play):
            assert stage['playback']['draft_generation'] == before_play['playback']['draft_generation'], stage
            assert stage['draft']['components'] == before_play['draft']['components'] and not stage['draft']['dirty']
        game_view = stages['game_preview']
        assert game_view['native']['view']['mode'] == game_view['playback']['view'] == 'game'
        assert game_view['native']['view']['camera'] and game_view['playback']['camera'] == game_view['native']['view']['camera']
        assert stages['game_navigation_blocked']['native']['camera'] == game_view['native']['camera']
        assert stages['game_navigation_blocked']['navigation']['drag'] == 'None'
        assert stages['game_capture']['native']['capture']['state'] == 'complete' and (run/'game-preview.bmp').is_file()
        assert stages['scene_restored']['native']['view']['mode'] == 'scene'
        assert stages['scene_restored']['native']['camera'] == before_play['native']['camera']
        resumed = stages['playback_external_resume']
        assert resumed['native']['playback']['state'] == resumed['playback']['state'] == 'playing'
        assert resumed['native']['playback']['tick'] > stepped_play['native']['playback']['tick']
        remote_paused = stages['playback_external_pause']
        assert remote_paused['native']['playback']['state'] == remote_paused['playback']['state'] == 'paused'
        assert remote_paused['playback_controls']['pause_action'] == 'Resume simulation' and remote_paused['playback_controls']['step_enabled']
        assert stages['playback_external_pause_stable']['native']['playback']['tick'] == remote_paused['native']['playback']['tick']
        stopped = stages['playback_stopped']
        assert stopped['native']['playback']['state'] == stopped['playback']['state'] == 'stopped'
        assert not stopped['native']['runtime']['active'] and stopped['playback']['session_id'] is None
        assert not stopped['playback_controls']['pause_enabled'] and not stopped['playback_controls']['step_enabled']
        assert stopped['native']['revision'] == before_play['native']['revision']
        assert stopped['draft']['components'] == before_play['draft']['components'] and not stopped['draft']['dirty']
        assert stages['playback_history_after']['result'] == stages['playback_history_before']['result']
        assert shared and capture_ready and captured and (run/'viewport.bmp').is_file() and (run/'gizmo-final.bmp').is_file()
        assert record['window_capture']['status'] in ('captured', 'unavailable'), record['window_capture']
        if record['window_capture']['status'] == 'captured':
            assert (run/'window.png').is_file()
            record['screenshot_sha256'] = hashlib.sha256((run/'window.png').read_bytes()).hexdigest()
        else:
            assert record['window_capture']['reason'] and not (run/'window.png').exists()
        restart_layout = run/'restart-layout.json'
        restart_layout.write_text(json.dumps(saved))
        restart_script = run/'restart-actions.json'
        restart_script.write_text(json.dumps({'actions': [
            {'frame': 5, 'op': 'select', 'id': entity},
            {'frame': 12, 'op': 'wait_text', 'control': 'Position X', 'text': '-4'},
            {'frame': 18, 'op': 'scene_frame'}, {'frame': 25, 'op': 'inspect'}]}))
        restart_report = run/'restart-report.json'
        restart = subprocess.run([str(a.editor.resolve()), str(world), '--gpu', str(a.gpu),
            '--endpoint', endpoint+'-restart', '--layout', str(restart_layout),
            '--script', str(restart_script), '--frames', '60', '--report', str(restart_report)],
            capture_output=True, text=True, encoding='utf-8', timeout=40)
        assert restart_report.is_file(), restart.stderr
        again = json.loads(restart_report.read_text()); record['desktop_restart'] = again
        assert restart.returncode == 0 and again['success'] and again['layout_error'] is None, again
        restored = next(item for item in again['actions'] if item['op'] == 'inspect')
        assert len(restored['windows']) == 2 and restored['layout'] == saved, restored
        assert restored['native']['frames_presented'] > 0 and restored['navigation']['attached']
        record.update(passed=True,
                      checks=['actual Avalonia Vulkan backend with native child Scene', 'shared CLI connection',
                              'dirty draft preserved; stale Apply rejects; Reload explicit', 'undo/redo and fixed-tick runtime',
                              'invalid raw field survives floating/reset; dirty close cancelled',
                              'missing-asset Scene error recovers after authoring repair without reattachment',
                              'actual Inspector and Scene floating windows; reset and continued presentation',
                              'native HWND look/fly/frame/click messages; input released cleanly',
                              'saved floating layout restores its panel structure and bounds, including a fresh process',
                              'HWND gizmo drag previews without authoring writes; one commit/undo; redo restores actual transform',
                              'dirty Inspector blocks gizmo begin; capture-loss/Escape/external revision cancel previews',
                              'Q/W/E/R tool keys preserve RMB flight; local/world switches; final selected move gizmo visible',
                              'native real-time Play/Pause/Step/Stop; command-driven state sync without per-tick Inspector rebuild',
                              'Game camera preview blocks Scene gestures; switching back preserves inspection camera',
                              'paused Game capture and Stop preserve authored transforms/revision/history',
                              'revision-guarded native viewport capture', 'bounded process exit',
                              'owned desktop client-area screenshot' if record['window_capture']['status'] == 'captured'
                              else 'window screenshot unavailable; semantic GUI and native Scene capture qualified separately'])
finally:
    if raised: u.SetWindowPos(raised, w.HWND(-1 if was_topmost else -2), 0, 0, 0, 0, 0x13)
    if process and process.poll() is None: process.kill(); process.wait()
    if report.is_file() and 'desktop' not in record:
        try: record['desktop'] = json.loads(report.read_text())
        except (OSError, ValueError) as error: record['report_error'] = str(error)
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
    print(run/'evidence.json')
