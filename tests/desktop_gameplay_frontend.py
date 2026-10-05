#!/usr/bin/env python3
"""Real self-contained editor C# configure/edit/reload qualification, with isolated fixtures."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid
from xml.sax.saxutils import escape
from PIL import ImageGrab

p = argparse.ArgumentParser(description=__doc__)
for name in ('editor', 'binary', 'dotnet', 'output'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--gpu', type=int, default=1)
a = p.parse_args()
assert os.name == 'nt', 'Run with Windows Python.'
ROOT = Path(__file__).resolve().parents[1]
run = a.output.resolve()/uuid.uuid4().hex; run.mkdir(parents=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
record = dict(passed=False, gpu=a.gpu, test_sha256=sha(Path(__file__)), editor_sha256=sha(a.editor.parent/'Poima.Editor.dll'),
              binary_sha256=sha(a.binary), builds=[], window_capture={'status': 'not_attempted'},
              limitations=['Semantic accessible controls, not physical keyboard/IME qualification.',
                           'Prebuilt assemblies; no editor build worker or filesystem watch qualification.'])
environment = {**os.environ, 'DOTNET_CLI_TELEMETRY_OPTOUT': '1', 'DOTNET_GENERATE_ASPNET_CERTIFICATE': 'false'}
source = (ROOT/'examples/managed/DoorGame/DoorGame.cs').read_text()

def build(name, code):
    directory = run/name; directory.mkdir()
    (directory/'DoorGame.cs').write_text(code)
    sdk = escape(str((a.editor.parent/'gameplay/Poima.Gameplay.dll').resolve()))
    project = directory/'EditorDoor.csproj'
    project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework>'
                      '<LangVersion>14.0</LangVersion><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable>'
                      '<Deterministic>true</Deterministic><TreatWarningsAsErrors>true</TreatWarningsAsErrors>'
                      '</PropertyGroup><ItemGroup><Reference Include="Poima.Gameplay"><HintPath>'+sdk+
                      '</HintPath></Reference></ItemGroup></Project>')
    done = subprocess.run([str(a.dotnet.resolve()), 'build', str(project), '-c', 'Release', '--nologo'],
                          capture_output=True, text=True, env=environment, timeout=120)
    record['builds'].append(dict(name=name, exit_code=done.returncode, stdout=done.stdout, stderr=done.stderr))
    assert done.returncode == 0, done.stdout+done.stderr
    return directory/'bin/Release/net10.0/EditorDoor.dll'

base = build('base', source)
upgrade = build('upgrade', source.replace('public int Open,Activations;', 'public int Open,Activations; public long Counter;')
                .replace('state.OpenX=2.2;', 'state.Counter=long.MaxValue;state.OpenX=9;'))
bad = build('failure', source.replace('state.OpenX=2.2;state.UseDistance=3;',
            'state.OpenX=2.2;state.UseDistance=3;throw new InvalidOperationException("intentional editor reload failure");'))

def cli(args, data=None, timeout=20):
    done = subprocess.run([str(a.binary.resolve()), *map(str, args)], input=data, capture_output=True,
                          text=True, encoding='utf-8', timeout=timeout)
    assert done.returncode == 0, done.stdout+done.stderr
    return [json.loads(line) for line in done.stdout.splitlines()]

project = run/'Project'; project.mkdir()
world = project/'world.json'
fixture = json.loads((ROOT/'examples/interaction-room.jsonl').read_text())
assert 'result' in cli(['world', world], json.dumps(fixture)+'\n')[0]
world_hash = sha(world)
uid = lambda n: f'{n:032x}'
endpoint = 'gameplay-ui-'+uuid.uuid4().hex
session = uuid.uuid4().hex
actions = []; frame = 3

def action(op, **values):
    global frame
    actions.append(dict(frame=frame, op=op, **values)); frame += 2

def text(name, value): action('set_text', control='Gameplay '+name, text=value)
def press(name): action('click_control', control='Gameplay '+name)
def inspect(tag): action('inspect', tag=tag)
def rpc(method, params, **values): action('rpc', method=method, params=params, **values)
def values(): press('Revert / refresh values')
def live(name, value): text('Value '+name, value)
def paused_human_start():
    global frame
    action('runtime_toggle'); frame -= 2; action('runtime_pause')

action('show_gameplay'); inspect('initial')
text('Assembly', str(base)); text('Game type', 'Poima.Examples.DoorGame')
action('runtime_toggle', error_contains='Gameplay drafts')
rpc('desktop.gameplay.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, profile=None))
press('Configure launch'); inspect('dirty_config_conflict')
action('assert_text', control='Gameplay Assembly', text=str(base))
press('Revert configuration')
text('Assembly', str(base)); text('Game type', 'Poima.Examples.DoorGame'); text('Initial values JSON', '{"UseDistance":10}')
press('Configure launch'); inspect('configured')
action('render_gameplay', path=str(run/'gameplay-configuration.png'), tag='configuration_visual')
paused_human_start(); values(); inspect('human_start')
action('runtime_toggle')
rpc('desktop.play.start', dict(session_id=session, revision=1, paused=True, expected_gameplay_generation=2))
values(); live('OpenX', '2.6'); press('Apply live values'); inspect('edited')
rpc('desktop.play.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=120))
rpc('desktop.play.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=120, ticks=120,
                             inputs=[dict(entity=uid(100), use=True)]))
values(); inspect('door_open'); action('runtime_entity', id=uid(2), tag='door')
live('OpenX', 'NaN'); press('Apply live values'); inspect('invalid_float'); action('close_guard')
action('assert_text', control='Gameplay Value OpenX', text='NaN'); values()
live('OpenX', '2.8')
rpc('runtime.gameplay.edit', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=240, expected_revision=2, values={'UseDistance': 7}))
press('Apply live values'); inspect('stale_values'); action('assert_text', control='Gameplay Value OpenX', text='2.8')
action('assert_text', control='Gameplay Live status', text=
       f'paused · tick 240 · gameplay revision 3\npoima.example.sliding-door\nImage SHA-256: {sha(base)}'
       '\nObserved values are stale. Refresh before applying.\nUnapplied live values'
       f'\nObserved session {session}\nObserved tick 240 · gameplay revision 2')
values(); live('OpenX', '2.8'); press('Apply live values')
text('Reload assembly', str(upgrade)); press('Reload assembly retaining state'); inspect('upgrade')
text('Reload assembly', str(base)); live('Counter', str(-(2**63))); press('Apply live values'); inspect('int64')
action('assert_text', control='Gameplay Value Counter', text=str(-(2**63)))
action('assert_text', control='Gameplay Reload assembly', text=str(base)); values()
live('Player', 'not-an-entity'); press('Apply live values'); inspect('invalid_entity'); values()
text('Reload assembly', str(bad)); press('Reload assembly retaining state'); inspect('failed_reload')
values(); inspect('recovered')
for _ in range(100): press('Reload assembly retaining state')
rpc('runtime.gameplay.collect', dict(session_id=session), tag='collected')
inspect('reloaded_100')
action('scroll_gameplay', position='top')
action('render_gameplay', path=str(run/'gameplay-live-top.png'), tag='live_top_visual')
action('scroll_gameplay', position='bottom')
action('render_gameplay', path=str(run/'gameplay-live-controls.png'), tag='live_controls_visual')
action('game_camera', camera=uid(101)); action('show_panel', panel='Game')
rpc('desktop.capture', dict(revision=1, view='game', path=str(run/'game.bmp')))
frame += 12; inspect('capture_ready')
# Leave time for a guarded screenshot of the actual Gameplay tool window.
frame += 80
action('runtime_toggle'); values(); inspect('stopped')
profile = dict(hostfxr=str((a.editor.parent/'hostfxr.dll').resolve()), bridge=str((a.editor.parent/'gameplay/Poima.ManagedBridge.dll').resolve()),
               assembly=str(base), type='Poima.Examples.DoorGame', values={'UseDistance': 4})
rpc('desktop.gameplay.configure', dict(request_id=uuid.uuid4().hex, expected_generation=2, profile=profile))
inspect('clean_external_config')
paused_human_start(); values(); inspect('restart_initial'); action('runtime_toggle'); values()
text('Initial values JSON', '{"Unknown":1}'); press('Configure launch')
action('runtime_toggle', error_contains='(-32060)'); inspect('failed_start')
press('Clear launch'); inspect('cleared')
script, report = run/'actions.json', run/'report.json'
script.write_text(json.dumps({'actions': actions}, indent=2))

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

u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]


process = None; raised = None; was_topmost = False
try:
    with (run/'stdout.txt').open('w', encoding='utf-8') as stdout, (run/'stderr.txt').open('w', encoding='utf-8') as stderr:
        process = subprocess.Popen([str(a.editor.resolve()), str(world), '--gpu', str(a.gpu), '--endpoint', endpoint,
            '--layout', str(run/'layout.json'), '--script', str(script), '--frames', str(frame+20), '--report', str(report)], stdout=stdout, stderr=stderr)
        deadline = time.monotonic()+100; captured = False
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(.25)
            if captured or not (run/'game.bmp').is_file(): continue
            available, reason = input_desktop_available()
            if available:
                windows = []
                @callback
                def found(hwnd, unused):
                    pid = w.DWORD(); u.GetWindowThreadProcessId(hwnd, c.byref(pid))
                    title = c.create_unicode_buffer(256); u.GetWindowTextW(hwnd, title, 256)
                    if pid.value == process.pid and u.IsWindowVisible(hwnd) and title.value == 'C# Gameplay — Poima': windows.append(hwnd)
                    return True
                u.EnumWindows(found, 0)
                if not windows: continue
                raised = windows[0]; was_topmost = bool(u.GetWindowLongPtrW(raised, -20) & 8)
                u.SetWindowPos(raised, w.HWND(-1), 0, 0, 0, 0, 0x13)
                time.sleep(.15)
                available, reason = input_desktop_available()
                if available:
                    rectangle = w.RECT(); origin = w.POINT()
                    assert u.GetClientRect(raised, c.byref(rectangle)) and u.ClientToScreen(raised, c.byref(origin))
                    image = ImageGrab.grab(bbox=(origin.x, origin.y, origin.x+rectangle.right, origin.y+rectangle.bottom), all_screens=True)
                    available, reason = input_desktop_available()
                    if available: image.save(run/'gameplay-window.png')
                    image.close()
            record['window_capture'] = {'status': 'captured', 'path': 'gameplay-window.png'} if available else {'status': 'unavailable', 'reason': reason}
            captured = True
        assert process.poll() is not None, 'Editor timeout.'
        assert report.is_file(), (run/'stderr.txt').read_text()
        data = json.loads(report.read_text()); record['desktop'] = data
        assert process.returncode == 0 and data['success'], data.get('error')
        assert len(data['actions']) == len(actions)
        stages = {spec['tag']: result for spec, result in zip(actions, data['actions']) if 'tag' in spec}
        draft = lambda tag: stages[tag]['gameplay_draft']
        module = lambda tag: draft(tag)['observation']['module']
        assert draft('dirty_config_conflict')['config_dirty'] and draft('dirty_config_conflict')['config_conflict']
        assert '(-32009)' in draft('dirty_config_conflict')['error']
        config = stages['configured']['native']['gameplay']
        assert config['generation'] == 2 and config['configured']
        assert stages['human_start']['native']['runtime']['tick'] == 0
        assert module('human_start')['values']['UseDistance'] == 10
        assert module('edited')['values']['OpenX'] == 2.6 and draft('edited')['observation']['tick'] == 0
        assert module('door_open')['values']['Activations'] == 1 and module('door_open')['values']['Open'] == 1
        assert abs(stages['door']['result']['world_matrix'][12]-2.6) < 1e-5
        assert 'finite' in draft('invalid_float')['error'] and draft('invalid_float')['values_dirty']
        assert draft('stale_values')['live_conflict'] and '(-32009)' in draft('stale_values')['error']
        assert module('upgrade')['values']['OpenX'] == 2.8 and module('upgrade')['values']['Activations'] == 1
        assert module('upgrade')['values']['Counter'] == str(2**63-1)
        assert module('int64')['values']['Counter'] == str(-(2**63))
        assert draft('int64')['reload_dirty'], 'Apply silently discarded the independent assembly-path draft.'
        assert '32 hexadecimal' in draft('invalid_entity')['error']
        assert 'intentional editor reload failure' in draft('failed_reload')['error']
        assert module('recovered') == module('int64'), 'Failed replacement changed the retained module.'
        assert stages['collected']['result'] == {'active_modules': 1, 'retired_alive': 0}
        assert module('reloaded_100')['values'] == module('int64')['values']
        assert draft('reloaded_100')['observation']['tick'] == 240
        assert draft('reloaded_100')['observation']['revision'] == draft('int64')['observation']['revision']+100
        assert not stages['stopped']['native']['runtime']['active']
        assert draft('clean_external_config')['generation'] == 3 and not draft('clean_external_config')['config_dirty']
        assert module('restart_initial')['values']['UseDistance'] == 4 and module('restart_initial')['values']['Activations'] == 0
        assert not stages['failed_start']['native']['runtime']['active']
        assert stages['cleared']['native']['gameplay']['generation'] == 5 and not stages['cleared']['native']['gameplay']['configured']
        assert sha(world) == world_hash and data['state']['revision'] == 1
        assert (run/'game.bmp').is_file() and captured
        visuals = {}
        for tag in ('configuration_visual', 'live_top_visual', 'live_controls_visual'):
            visual = stages[tag]['visual']; path = Path(visual['path'])
            assert visual['kind'] == 'avalonia_visual_content' and path.is_file()
            assert 32 <= visual['width'] <= 4096 and 32 <= visual['height'] <= 4096
            visuals[tag] = dict(visual, sha256=sha(path))
        assert visuals['live_controls_visual']['scroll_y'] > visuals['live_top_visual']['scroll_y']
        record['ui_content_renders'] = visuals
        output = [json.loads(line) for line in (run/'stdout.txt').read_text().splitlines() if line.startswith('{')]
        assert len(output) == 1 and output[0] == data, 'Editor report stdout was redirected by cohosted gameplay.'
        record.update(passed=True, actions=len(actions), compatible_reloads=100, world_sha256=world_hash,
                      capture_sha256=sha(run/'game.bmp'), checks=['actual self-contained editor cohosts packaged CoreCLR bridge',
                      'human Play auto-loads configured gameplay before first tick', 'paused typed live edits and real C# door motion',
                      'state-preserving schema upgrade, exact int64, entity/finite validation and stale-draft protection',
                      'failed reload preserves module; 100 compatible GUI reloads release every retired context',
                      'external clean configuration adoption and dirty configuration conflict',
                      'Stop discards live state; failed startup leaves no runtime; authored world unchanged',
                      'structured report remains on original stdout after managed bridge loads'])
        if record['window_capture']['status'] == 'captured': record['window_sha256'] = sha(run/'gameplay-window.png')
except BaseException as error:
    record['error'] = repr(error); raise
finally:
    if raised: u.SetWindowPos(raised, w.HWND(-1 if was_topmost else -2), 0, 0, 0, 0, 0x13)
    if process and process.poll() is None: process.kill(); process.wait(timeout=10)
    if report.is_file() and 'desktop' not in record: record['desktop'] = json.loads(report.read_text())
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(run/'evidence.json')
