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
def cli(arguments, data=None):
    value = subprocess.run([str(a.binary.resolve()), *map(str, arguments)], input=data, capture_output=True,
                           text=True, encoding='utf-8', timeout=30)
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
action(75, 'assert_scene_error', expected=False)
action(80, 'rpc', method='desktop.capture', params={'revision': revision+6, 'path': str(run/'viewport.bmp')})
action(85, 'inspect')
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
process = None; raised = None; was_topmost = False
try:
    with (run/'stdout.txt').open('w', encoding='utf-8') as out, (run/'stderr.txt').open('w', encoding='utf-8') as err:
        command = [str(a.editor.resolve()), str(world), '--gpu', str(a.gpu), '--endpoint', endpoint,
                   '--script', str(script), '--frames', '220', '--report', str(report)]
        process = subprocess.Popen(command, stdout=out, stderr=err)
        start = time.monotonic(); captured = False; shared = False; next_status = 0; capture_ready = False
        while process.poll() is None and time.monotonic()-start < 45:
            handles = []
            @callback
            def visit(hwnd, _):
                pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
                if pid.value == process.pid and u.IsWindowVisible(hwnd): handles.append(hwnd)
                return True
            u.EnumWindows(visit, 0)
            elapsed = time.monotonic()-start
            if handles and elapsed > 1 and not shared:
                reply = cli(['connect', endpoint], '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}\n')[0]
                assert 'result' in reply, reply
                record['shared_cli'] = reply['result']; shared = True
            if shared and elapsed > 5 and elapsed >= next_status and not captured:
                next_status = elapsed + .5
                state = cli(['connect', endpoint], '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}\n')[0]['result']
                capture_ready = (state.get('capture') or {}).get('state') == 'complete'
            if len(handles) == 1 and capture_ready and not captured:
                raised = handles[0]; was_topmost = bool(u.GetWindowLongPtrW(raised, -20) & 8)
                u.SetWindowPos(raised, w.HWND(-1), 60, 40, 1440, 920, 0x10)
                time.sleep(.35)
                rectangle = w.RECT(); origin = w.POINT()
                assert u.GetClientRect(raised, c.byref(rectangle)) and u.ClientToScreen(raised, c.byref(origin))
                # Client bounds exclude OS-frame corners that could expose
                # unrelated windows behind this owned editor window.
                ImageGrab.grab(bbox=(origin.x, origin.y, origin.x+rectangle.right, origin.y+rectangle.bottom), all_screens=True).save(run/'window.png')
                u.SetWindowPos(raised, w.HWND(-1 if was_topmost else -2), 0, 0, 0, 0, 0x13)
                raised = None; captured = True
            time.sleep(.05)
        if process.poll() is None: raise AssertionError('Desktop process did not exit within 45 seconds.')
        record['exit_code'] = process.returncode
        assert report.is_file(), (run/'stderr.txt').read_text()
        evidence = json.loads(report.read_text()); record['desktop'] = evidence
        assert process.returncode == 0 and evidence['success'], evidence
        assert evidence['ui_backend_actual'] == 'Avalonia.Vulkan.VulkanPlatformGraphics', evidence
        assert evidence['font_resolved'] == 'Inter', evidence
        assert len(evidence['actions']) == len(actions)
        inspections = [item for item in evidence['actions'] if item['op'] == 'inspect']
        assert len(inspections[0]['windows']) == 2 and len(inspections[1]['windows']) == 2, inspections
        assert inspections[1]['native']['frames_presented'] > inspections[0]['native']['frames_presented']
        assert inspections[2]['native']['runtime']['tick'] == 2
        assert inspections[3]['native']['capture']['state'] == 'complete'
        assert evidence['state']['render']['hardware'] and evidence['state']['render']['nvrhi_errors'] == 0
        assert not evidence['state']['runtime']['active'] and evidence['state']['revision'] == revision+6
        recovery = [item for item in evidence['actions'] if item['op'] == 'assert_scene_error']
        assert recovery[1]['native']['frames_presented'] > recovery[0]['native']['frames_presented']
        assert evidence['draft']['name'] == 'Courtyard pillar' and not evidence['draft']['dirty']
        assert shared and captured and (run/'viewport.bmp').is_file()
        record.update(passed=True, screenshot_sha256=hashlib.sha256((run/'window.png').read_bytes()).hexdigest(),
                      checks=['actual Avalonia Vulkan backend with native child Scene', 'shared CLI connection',
                              'dirty draft preserved; stale Apply rejects; Reload explicit', 'undo/redo and fixed-tick runtime',
                              'invalid raw field survives floating/reset; dirty close cancelled',
                              'missing-asset Scene error recovers after authoring repair without reattachment',
                              'actual Inspector and Scene floating windows; reset and continued presentation',
                              'revision-guarded viewport capture', 'bounded process exit and actual desktop client-area screenshot'])
finally:
    if raised: u.SetWindowPos(raised, w.HWND(-1 if was_topmost else -2), 0, 0, 0, 0, 0x13)
    if process and process.poll() is None: process.kill(); process.wait()
    if report.is_file() and 'desktop' not in record:
        try: record['desktop'] = json.loads(report.read_text())
        except (OSError, ValueError) as error: record['report_error'] = str(error)
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
    print(run/'evidence.json')
