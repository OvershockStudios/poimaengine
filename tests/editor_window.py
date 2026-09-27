#!/usr/bin/env python3
"""Owned Win32 messages exercise the visible editor and its window lifecycle.

Run with Windows Python. No global SendInput, cursor movement, semantic action
script or direct world mutation is used. Physical devices remain unqualified.
Use --lifecycle-only for resize/minimize/close qualification without UI input.
Interaction-mode default hit points predate docking; supply current points
before attempting interaction qualification against the docking editor.
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

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--gpu', type=int, default=0)
parser.add_argument('--lifecycle-only', action='store_true',
                    help='Skip all mouse/key actions; qualify window lifecycle and final capture only.')
# Coordinates are initial 1440x900 client units. These legacy defaults predate
# docking and must be overridden with current widget positions for UI testing.
parser.add_argument('--create-point', type=int, nargs=2, default=[94, 18])
parser.add_argument('--name-point', type=int, nargs=2, default=[1240, 79])
parser.add_argument('--undo-point', type=int, nargs=2, default=[242, 18])
parser.add_argument('--redo-point', type=int, nargs=2, default=[285, 18])
args = parser.parse_args()
run = args.output / uuid.uuid4().hex
run.mkdir(parents=True)
world, report, capture = run/'world.json', run/'report.json', run/'editor.bmp'
u = c.WinDLL('user32', use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.PostMessageW.restype = w.BOOL
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.SetWindowPos.restype = w.BOOL
u.ShowWindow.argtypes = [w.HWND, c.c_int]
u.SetForegroundWindow.argtypes = [w.HWND]
u.GetForegroundWindow.restype = w.HWND
u.MapVirtualKeyW.argtypes = [w.UINT, w.UINT]
u.SetThreadDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetThreadDpiAwarenessContext.restype = c.c_void_p
assert u.SetThreadDpiAwarenessContext(c.c_void_p(-4)), c.get_last_error()

record = {'passed': False, 'lifecycle_only': args.lifecycle_only,
          'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'input_source': 'Child HWND-targeted Win32 messages only; no physical input qualification.',
          'limitations': ['Synthetic input does not qualify physical mouse, keyboard, IME or gamepad.',
                         'Interaction-mode default hit points predate docking; current widget positions must be supplied.',
                         'Synthetic middle-button hold prevents the SDL backend global-cursor fallback during clicks.'],
          'checks': {}, 'messages': []}
command = [str(args.binary.resolve()), 'editor', str(world.resolve()), '--gpu', str(args.gpu),
           '--no-layout', '--width', '1440', '--height', '900',
           '--capture', str(capture.resolve()), '--report', str(report.resolve())]
record['command'] = command
# Files avoid pipe backpressure if graphics validation emits many diagnostics.
stdout_file, stderr_file = run/'stdout.txt', run/'stderr.txt'
stdout_stream, stderr_stream = stdout_file.open('w', encoding='utf-8'), stderr_file.open('w', encoding='utf-8')
p = subprocess.Popen(command, stdout=stdout_stream, stderr=stderr_stream)

def poll(predicate, timeout=15, description='condition', required=True):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        assert p.poll() is None, f'Editor exited while waiting for {description}.'
        time.sleep(.05)
    if required:
        raise AssertionError(f'Timed out waiting for {description}.')
    return None

def find_window():
    found = []
    @callback
    def visit(hwnd, _):
        pid = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        if pid.value == p.pid:
            title = c.create_unicode_buffer(512)
            u.GetWindowTextW(hwnd, title, len(title))
            if title.value.startswith('Poima Editor'):
                found.append(hwnd)
        return True
    u.EnumWindows(visit, 0)
    return found[0] if found else None

def post(message, key=0, value=0):
    assert u.PostMessageW(hwnd, message, key, value), c.get_last_error()
    record['messages'].append([message, key, value])

def client_size():
    rect = w.RECT()
    assert u.GetClientRect(hwnd, c.byref(rect)), c.get_last_error()
    return [rect.right-rect.left, rect.bottom-rect.top]

def click(point):
    width, height = initial_size
    x, y = round(point[0]*width/1440), round(point[1]*height/900)
    packed = x | (y << 16)
    post(0x0200, 0, packed)       # WM_MOUSEMOVE
    post(0x0207, 0x10, packed)    # WM_MBUTTONDOWN; pins synthetic position
    time.sleep(.10)
    post(0x0200, 0x10, packed)
    post(0x0201, 0x11, packed)    # WM_LBUTTONDOWN
    time.sleep(.12)
    post(0x0202, 0x10, packed)    # WM_LBUTTONUP, middle remains down
    time.sleep(.12)
    post(0x0208, 0, packed)       # WM_MBUTTONUP
    time.sleep(.10)

def key(vk, down):
    scan = u.MapVirtualKeyW(vk, 4)  # MAPVK_VK_TO_VSC_EX preserves E0 navigation keys
    extended = (1 << 24) if (scan >> 8) in (0xE0, 0xE1) else 0
    post(0x0100 if down else 0x0101, vk,
         1 | ((scan & 255) << 16) | extended | (0 if down else (3 << 30)))

def tap(vk):
    key(vk, True)
    time.sleep(.06)
    key(vk, False)
    time.sleep(.08)

def chord(vk):
    key(0x11, True)
    time.sleep(.08)
    tap(vk)
    key(0x11, False)
    time.sleep(.10)

def document():
    try:
        return json.loads(world.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None

def has_revision(revision):
    doc = document()
    return doc if doc and doc['revision'] == revision else None

try:
    hwnd = poll(find_window, timeout=30, description='owned editor window')
    record['foreground_request_succeeded'] = bool(u.SetForegroundWindow(hwnd))
    time.sleep(.5)
    record['initial_foreground'] = u.GetForegroundWindow() == hwnd
    initial_size = client_size()
    record['initial_client_size'] = initial_size
    created = None
    if not args.lifecycle_only:
        click(args.create_point)
        record['foreground_after_click'] = u.GetForegroundWindow() == hwnd
        foreground_available = record['initial_foreground'] or record['foreground_after_click']
        created = poll(lambda: has_revision(1), timeout=15 if foreground_available else 2,
                       description='toolbar cube commit', required=foreground_available)
    entity = None
    if created:
        assert len(created['entities']) == 1, created
        entity, value = next(iter(created['entities'].items()))
        assert value['components']['MeshRenderer']['primitive'] == 'box', value
        original_name = value['name']
        record['checks']['visible_toolbar_creates_cube'] = True
        click(args.name_point)
        # Avoid depending on the OS-wide modifier snapshot for select-all: move
        # to the start and remove the known initial label with child key events.
        tap(0x24)                   # Home
        for _ in original_name:
            tap(0x2E)               # Delete
        name = 'Window edited cube'
        for char in name:
            post(0x0102, ord(char))   # WM_CHAR follows normal SDL text input
            time.sleep(.012)
        tap(0x0D)
        renamed = poll(lambda: has_revision(2), description='Inspector rename commit')
        assert renamed['entities'][entity]['name'] == name, renamed
        record['checks']['inspector_text_input_persists_rename'] = True
        # Move focus out of the InputText edit session before global history keys.
        click([350, 300])
        chord(ord('Z'))
        time.sleep(.3)
        keyboard_undo = bool(has_revision(3))
        if not keyboard_undo:
            click(args.undo_point)
        undone = poll(lambda: has_revision(3), description='visible undo')
        assert undone['entities'][entity]['name'] == original_name, undone
        chord(ord('Y'))
        time.sleep(.3)
        keyboard_redo = bool(has_revision(4))
        if not keyboard_redo:
            click(args.redo_point)
        redone = poll(lambda: has_revision(4), description='visible redo')
        assert redone['entities'][entity]['name'] == name, redone
        record['checks'].update(undo_redo_restores_same_entity=True,
                                targeted_keyboard_undo=keyboard_undo, targeted_keyboard_redo=keyboard_redo)
        if not keyboard_undo or not keyboard_redo:
            record['limitations'].append('One or both synthetic history chords were unavailable; visible toolbar history controls were used.')
        record['qualification'] = 'UI interactions and window lifecycle'
    else:
        # Desktop focus restrictions must not turn unavailable input into a
        # passing interaction test. Still exercise this owned window's lifecycle.
        assert not world.exists(), 'Unexpected authored data appeared without the expected cube commit.'
        reason = ('Explicit --lifecycle-only mode skips every mouse/key action and all UI interaction checks.'
                  if args.lifecycle_only else
                  'Foreground unavailable and targeted toolbar click produced no authored commit.')
        record['qualification'] = ('Window lifecycle only; UI input intentionally skipped'
                                   if args.lifecycle_only else
                                   'Window lifecycle only; UI input unavailable')
        record['limitations'].append(reason)
        record['input_checks_skipped'] = {
            check: reason for check in ('visible_toolbar_creates_cube',
                'inspector_text_input_persists_rename', 'undo_redo_restores_same_entity',
                'targeted_keyboard_undo', 'targeted_keyboard_redo')}
        record['checks'].update({check: None for check in record['input_checks_skipped']})
    saved = world.read_bytes() if world.exists() else None
    u.ShowWindow(hwnd, 6)
    time.sleep(.6)
    assert p.poll() is None, 'Editor exited while minimized.'
    u.ShowWindow(hwnd, 9)
    u.SetForegroundWindow(hwnd)
    time.sleep(.4)
    assert u.SetWindowPos(hwnd, None, 0, 0, 1120, 760, 0x0002 | 0x0004), c.get_last_error()
    time.sleep(.7)
    expected_size = client_size()
    assert expected_size != initial_size, (initial_size, expected_size)
    post(0x0010)                 # WM_CLOSE to this child only
    assert p.wait(timeout=30) == 0, 'Editor exited unsuccessfully.'
    after = world.read_bytes() if world.exists() else None
    assert after == saved, 'Window lifecycle unexpectedly changed authored data.'
    evidence = json.loads(report.read_text(encoding='utf-8'))
    record['editor'] = evidence
    assert evidence['success'] and evidence['render']['nvrhi_errors'] == 0, evidence
    assert evidence['render']['capture_written'] and capture.exists(), evidence
    bmp = capture.read_bytes()
    assert bmp[:2] == b'BM', 'Expected BMP editor capture.'
    image_size = [int.from_bytes(bmp[18:22], 'little', signed=True), abs(int.from_bytes(bmp[22:26], 'little', signed=True))]
    assert image_size == expected_size, (image_size, expected_size)
    record['checks'].update(minimize_restore_survives=True, resize_capture_matches_client=True,
                            close_preserves_authored_world=True, no_renderer_validation_errors=True)
    record.update(passed=True, entity=entity, final_revision=json.loads(after)['revision'] if after is not None else 0,
                  persisted=after is not None, expected_size=expected_size,
                  image={'path': str(capture.resolve()), 'sha256': hashlib.sha256(bmp).hexdigest()})
except BaseException as error:
    record['failure'] = repr(error)
    raise
finally:
    if p.poll() is None:
        p.kill()
        p.wait(timeout=10)
    stdout_stream.close()
    stderr_stream.close()
    record.update(exit_code=p.returncode, stdout=stdout_file.read_text(encoding='utf-8'),
                  stderr=stderr_file.read_text(encoding='utf-8'))
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    (args.output/'window.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(run/'evidence.json')
