#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Export and play the genuine .81 settings NativeAOT fixture on Windows.

Requires an already published Windows artifact, an installed renderer runtime,
and its matching exporter. Builds no C# and downloads nothing. Removes the owned
source project before launching the relocated game from an unrelated directory.
Targeted Win32 messages reach only the owned player HWND; real native hit tests
dispatch compiled menu callbacks. Requires an unlocked interactive desktop.

Example (native Windows Python):
  python tests/player_preferences_gameplay_bundle.py --binary PATH/poima.exe \
    --runtime PATH/runtime --artifact PATH/native-gameplay.json \
    --output NEW/qualification --gpu 1

Fixed input spacing is not a presentation observation. Durable compiled saves,
the final native report, and renderer-readback pixels are the acceptance oracles.
"""
import argparse
import copy
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

from collection_gameplay_bundle import check, managed_pe, read_json, sha
import player_preferences_gameplay_contract as fixture

ROOT = Path(__file__).resolve().parents[1]
FEATURES = {'baseline_v7', 'player_preferences_v1'}
WIDTH, HEIGHT = 960, 540


def clean_inventory(root):
    """Refuse links/reparse points before hashing an owned or supplied tree."""
    result = {}
    spellings = set()
    for path in sorted(root.rglob('*')):
        info = path.lstat()
        check(not path.is_symlink() and not getattr(info, 'st_file_attributes', 0) & 0x400,
              'Inventory contains a link or Windows reparse point: ' + str(path))
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            check(relative.casefold() not in spellings, 'Case-colliding inventory path')
            spellings.add(relative.casefold())
            result[relative] = sha(path)
    return result


def relative_path(value):
    check(isinstance(value, str) and value and '\\' not in value,
          'Payload path must use nonempty relative POSIX spelling')
    posix, windows = PurePosixPath(value), PureWindowsPath(value)
    check(not posix.is_absolute() and not windows.is_absolute() and not windows.drive and
          all(part not in ('', '.', '..') and ':' not in part for part in value.split('/')),
          'Payload path escapes its root')
    return Path(*posix.parts)


def near(actual, expected, name):
    check(type(actual) in (float, int) and math.isfinite(actual) and
          math.isclose(actual, expected, rel_tol=0, abs_tol=1e-6),
          name + ' differs: ' + repr(actual))


def numeric_values(values, schema):
    """Native gameplay int64 values are canonical decimal JSON strings."""
    result = dict(values)
    for field in schema['fields']:
        name, kind = field['name'], field['kind']
        if kind == 'int64':
            value = values[name]
            check(type(value) is str and 0 < len(value) <= 20,
                  'Expected decimal-string int64 gameplay field: '+name)
            converted = int(value)
            check(str(converted) == value and -(1 << 63) <= converted < (1 << 63),
                  'Invalid canonical int64 gameplay field: '+name)
            result[name] = converted
        elif kind == 'int32':
            check(type(values[name]) is int and -(1 << 31) <= values[name] < (1 << 31),
                  'Expected int32 gameplay field: '+name)
    return result


def shipping_document(authored):
    result = copy.deepcopy(authored)
    for name in ('receipts', 'retired_ids', 'retired_component_schemas',
                 'retired_template_ids', 'retired_ui_ids'):
        if name in result:
            result[name] = []
    return result


def author(client):
    fixture.author(client)
    def transform(position, scale=(1, 1, 1)):
        return dict(position=list(position), rotation=[0, 0, 0, 1], scale=list(scale))
    ops = [dict(op='component.set', id=fixture.uid(100), type='Transform', value=transform((0, 0, 0))),
           dict(op='component.set', id=fixture.uid(20), type='Transform', value=transform((0, 1.6, -5))),
           dict(op='entity.create', id=fixture.uid(102), name='Exported player support floor'),
           dict(op='component.set', id=fixture.uid(102), type='Transform', value=transform((0, -.5, 0), (20, 1, 20))),
           dict(op='component.set', id=fixture.uid(102), type='BoxCollider', value=dict(
               half_extents=[.5, .5, .5], motion='static', mass=10, friction=.5, restitution=0)),
           dict(op='component.set', id=fixture.uid(102), type='MeshRenderer', value=dict(
               primitive='box', visible=True, albedo=[.14, .2, .24]))]
    # The genuine consumer/action identities are unchanged. This exported probe
    # adds two usable save/load controls and a three-column percentage layout.
    # Zero panel padding makes the hit targets independent of UI density/DPI.
    def percent(value):
        return dict(unit='percent', value=value)
    ops.append(dict(op='ui.element.set', id=fixture.uid(700), element=dict(
        parent=None, name='Compiled exported settings and checkpoints', kind='panel',
        text='', action=None, visible=False, enabled=True,
        layout=dict(position='absolute', left=percent(0), top=percent(0), width=percent(100),
                    height=percent(100), padding=[0, 0, 0, 0]),
        style=dict(background_color='#1e1b20', border_radius=0, border_width=0))))
    for row in range(8):
        # Existing percentage row panels have no children after reparenting.
        ops.append(dict(op='ui.element.set', id=fixture.uid(730+row), element=dict(
            parent=fixture.uid(700), name='Unused original row', kind='panel', text='',
            action=None, visible=False, enabled=False)))
    actions = list(fixture.MENU_ACTIONS) + ['pref.stage.save', 'pref.stage.load']
    targets = {}
    for index, action in enumerate(actions):
        row, column = divmod(index, 3)
        identity = fixture.MENU_IDS.get(action, fixture.IDS.get(action))
        left, top = 2 + 32*column, 25 + 11*row
        targets[action] = dict(id=identity, center_percent=[left+15, top+4.5],
                               bounds_percent=[left, top, 30, 9])
        ops.append(dict(op='ui.element.set', id=identity, element=dict(
            parent=fixture.uid(700), name=action, kind='button', text=action,
            action=action, visible=True, enabled=True,
            layout=dict(position='absolute', left=percent(left), top=percent(top),
                        width=percent(30), height=percent(9), padding=[0, 0, 0, 0]),
            style=dict(font_size=10, color='#f7e8eb', border_width=0, border_radius=0,
                       background_color='#722f37' if action == 'pref.close' else '#353039',
                       hover=dict(background_color='#4f1a23'),
                       focus=dict(background_color='#4f1a23'),
                       pressed=dict(background_color='#2b0d13')))))
    client.call('world.transact', dict(base_revision=2, request_id=uuid.uuid4().hex, ops=ops))
    return targets


class OwnedWindow:
    """PID-owned, foreground checked input; never global SendInput/unlock."""
    def __init__(self, process, evidence, remaining, pause):
        self.process, self.record, self.remaining, self.pause = process, evidence, remaining, pause
        self.hwnd = None
        self.user = u = c.WinDLL('user32', use_last_error=True)
        self.callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        u.EnumWindows.argtypes = [self.callback, w.LPARAM]
        u.EnumWindows.restype = w.BOOL
        u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
        u.GetWindowThreadProcessId.restype = w.DWORD
        u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
        u.IsWindowVisible.argtypes = [w.HWND]
        u.IsWindowVisible.restype = w.BOOL
        u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        u.PostMessageW.restype = w.BOOL
        u.GetForegroundWindow.restype = w.HWND
        u.SetForegroundWindow.argtypes = [w.HWND]
        u.SetForegroundWindow.restype = w.BOOL
        u.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
        u.AttachThreadInput.restype = w.BOOL
        kernel = c.WinDLL('kernel32', use_last_error=True)
        kernel.GetCurrentThreadId.argtypes = []
        kernel.GetCurrentThreadId.restype = w.DWORD
        self.current_thread = kernel.GetCurrentThreadId()
        u.ShowWindowAsync.argtypes = [w.HWND, c.c_int]
        u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
        u.GetClientRect.restype = w.BOOL
        u.GetDpiForWindow.argtypes = [w.HWND]
        u.GetDpiForWindow.restype = w.UINT
        u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
        u.ClientToScreen.restype = w.BOOL
        u.WindowFromPoint.argtypes = [w.POINT]
        u.WindowFromPoint.restype = w.HWND
        u.GetDC.argtypes = [w.HWND]
        u.GetDC.restype = w.HDC
        u.ReleaseDC.argtypes = [w.HWND, w.HDC]
        u.ReleaseDC.restype = c.c_int
        u.SetThreadDpiAwarenessContext.argtypes = [c.c_void_p]
        u.SetThreadDpiAwarenessContext.restype = c.c_void_p
        u.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        u.OpenInputDesktop.restype = w.HANDLE
        u.CloseDesktop.argtypes = [w.HANDLE]
        u.CloseDesktop.restype = w.BOOL
        u.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, c.c_void_p,
                                               w.DWORD, c.POINTER(w.DWORD)]
        u.GetUserObjectInformationW.restype = w.BOOL
        self.old_dpi = u.SetThreadDpiAwarenessContext(c.c_void_p(-4))
        check(self.old_dpi, 'Cannot enable per-monitor-v2 client coordinates')

    def desktop(self):
        # Opening the current input desktop fails on inaccessible/locked secure
        # desktops. Do not switch it or record user/account/window names.
        handle = self.user.OpenInputDesktop(0, False, 1)  # DESKTOP_READOBJECTS
        check(handle, 'Input desktop inaccessible or locked; no input injected')
        try:
            name = c.create_unicode_buffer(256)
            needed = w.DWORD()
            check(self.user.GetUserObjectInformationW(handle, 2, name, c.sizeof(name), c.byref(needed)),
                  'Cannot inspect input desktop; no input injected')
            check(name.value.casefold() == 'default', 'Interactive Default desktop unavailable; no input injected')
        finally:
            self.user.CloseDesktop(handle)

    def owned(self):
        pid = w.DWORD()
        if self.hwnd:
            self.user.GetWindowThreadProcessId(self.hwnd, c.byref(pid))
        return self.process.poll() is None and pid.value == self.process.pid

    def find(self):
        self.desktop()
        deadline = time.monotonic() + min(20, self.remaining())
        while time.monotonic() < deadline:
            matches = []
            @self.callback
            def visit(hwnd, unused):
                pid = w.DWORD()
                self.user.GetWindowThreadProcessId(hwnd, c.byref(pid))
                if pid.value == self.process.pid and self.user.IsWindowVisible(hwnd):
                    title = c.create_unicode_buffer(256)
                    self.user.GetWindowTextW(hwnd, title, len(title))
                    if title.value.startswith('Poima player'):
                        matches.append(hwnd)
                return True
            check(self.user.EnumWindows(visit, 0), 'Owned window enumeration failed')
            check(len(matches) <= 1, 'Ambiguous owned player windows; refusing input')
            if matches:
                self.hwnd = matches[0]
                attempts = []
                for unused in range(12):
                    self.desktop()
                    check(self.owned(), 'Player exited before focus')
                    self.user.ShowWindowAsync(self.hwnd, 9)
                    requested = bool(self.user.SetForegroundWindow(self.hwnd))
                    attached = False
                    # Windows can deny foreground activation to a WSL-launched
                    # console. Briefly join the foreground queue for activation
                    # only, then always detach. No input is sent to that queue's
                    # window and no global key/cursor events are manufactured.
                    if self.user.GetForegroundWindow() != self.hwnd:
                        foreground_thread = self.user.GetWindowThreadProcessId(self.user.GetForegroundWindow(), None)
                        if foreground_thread and foreground_thread != self.current_thread:
                            attached = bool(self.user.AttachThreadInput(self.current_thread, foreground_thread, True))
                            if attached:
                                try:
                                    requested = bool(self.user.SetForegroundWindow(self.hwnd))
                                finally:
                                    check(self.user.AttachThreadInput(self.current_thread, foreground_thread, False),
                                          'Cannot detach temporary foreground input queue')
                    self.pause(.25)
                    foreground = self.user.GetForegroundWindow() == self.hwnd
                    attempts.append(dict(request_succeeded=requested, owned_foreground=foreground,
                                         temporary_queue_attachment=attached))
                    if foreground:
                        break
                self.record['focus_attempts'] = attempts
                check(foreground, 'Owned player cannot receive foreground focus; activation denied or desktop unavailable')
                rect = self.size()
                self.record['window'] = dict(pid=self.process.pid, hwnd=int(self.hwnd),
                    client_size=list(rect), dpi=self.user.GetDpiForWindow(self.hwnd),
                    coordinate_space='per-monitor-v2 client pixels')
                return
            check(self.process.poll() is None, 'Game exited before its player window appeared')
            self.pause(.05)
        raise RuntimeError('Owned player HWND deadline exceeded')

    def size(self):
        check(self.owned(), 'Player HWND is no longer owned')
        rect = w.RECT()
        check(self.user.GetClientRect(self.hwnd, c.byref(rect)), 'Cannot inspect owned client rectangle')
        width, height = rect.right-rect.left, rect.bottom-rect.top
        check(0 < width < 32768 and 0 < height < 32768, 'Invalid client coordinate extent')
        return width, height

    def post(self, message, key=0, value=0, *, closing=False):
        check(self.owned(), 'Refusing to message an unowned/stale HWND')
        if not closing:
            self.remaining()
            self.desktop()
            check(self.user.IsWindowVisible(self.hwnd) and self.user.GetForegroundWindow() == self.hwnd,
                  'Owned foreground lost; refusing input')
        check(self.user.PostMessageW(self.hwnd, message, key, value),
              'Owned PostMessage failed: ' + str(c.get_last_error()))
        self.record.setdefault('messages', []).append(dict(message=message, wparam=key, lparam=value,
                                                          elapsed=time.monotonic()-self.record['started_monotonic']))

    def observe(self, rect):
        """Read a bounded rectangle clipped to the visible owned client."""
        self.remaining(); self.desktop()
        check(self.owned() and self.user.GetForegroundWindow() == self.hwnd,
              'Owned foreground lost before presentation observation')
        width, height = self.size()
        left, top, right, bottom = rect
        check(0 <= left < right <= width and 0 <= top < bottom <= height and
              (right-left)*(bottom-top)*4 <= 1024*1024, 'Invalid bounded client observation')
        origin = w.POINT(left, top)
        check(self.user.ClientToScreen(self.hwnd, c.byref(origin)), 'Client observation conversion failed')
        wide, high = right-left, bottom-top
        for x, y in ((0,0), (wide-1,0), (0,high-1), (wide-1,high-1), (wide//2,high//2)):
            target = self.user.WindowFromPoint(w.POINT(origin.x+x, origin.y+y))
            check(target == self.hwnd, 'Owned observation is obscured; refusing screen readback')
        class Header(c.Structure):
            _fields_ = [('size',w.DWORD),('width',w.LONG),('height',w.LONG),
                        ('planes',w.WORD),('bits',w.WORD),('compression',w.DWORD),
                        ('image_size',w.DWORD),('xppm',w.LONG),('yppm',w.LONG),
                        ('colors',w.DWORD),('important',w.DWORD)]
        class Info(c.Structure):
            _fields_ = [('header',Header),('color',w.DWORD)]
        g = c.WinDLL('gdi32', use_last_error=True)
        g.CreateCompatibleDC.argtypes=[w.HDC];g.CreateCompatibleDC.restype=w.HDC
        g.CreateDIBSection.argtypes=[w.HDC,c.POINTER(Info),w.UINT,c.POINTER(c.c_void_p),w.HANDLE,w.DWORD]
        g.CreateDIBSection.restype=w.HANDLE
        g.SelectObject.argtypes=[w.HDC,w.HANDLE];g.SelectObject.restype=w.HANDLE
        g.BitBlt.argtypes=[w.HDC,c.c_int,c.c_int,c.c_int,c.c_int,w.HDC,c.c_int,c.c_int,w.DWORD]
        g.BitBlt.restype=w.BOOL
        g.GdiFlush.argtypes=[];g.GdiFlush.restype=w.BOOL
        g.DeleteObject.argtypes=[w.HANDLE];g.DeleteObject.restype=w.BOOL
        g.DeleteDC.argtypes=[w.HDC];g.DeleteDC.restype=w.BOOL
        screen = memory = bitmap = previous = None
        try:
            screen=self.user.GetDC(None);check(screen,'Screen DC unavailable')
            memory=g.CreateCompatibleDC(screen);check(memory,'Client observation DC unavailable')
            info=Info();info.header=Header(c.sizeof(Header),wide,-high,1,32,0,wide*high*4,0,0,0,0)
            bits=c.c_void_p()
            bitmap=g.CreateDIBSection(screen,c.byref(info),0,c.byref(bits),None,0)
            check(bitmap and bits.value,'Bounded client bitmap unavailable')
            previous=g.SelectObject(memory,bitmap)
            check(previous and previous != c.c_void_p(-1).value,'Client bitmap selection failed')
            check(g.BitBlt(memory,0,0,wide,high,screen,origin.x,origin.y,0x00CC0020),'Client readback failed')
            check(g.GdiFlush(),'Client readback synchronization failed')
            check(self.owned() and self.user.GetForegroundWindow()==self.hwnd,
                  'Owned foreground changed during client observation')
            # DIB memory access follows GdiFlush. The unused X byte in a 32-bit
            # BI_RGB bitmap is not visible content and must not affect guards.
            raw=c.string_at(bits,wide*high*4)
            rgb=bytearray(wide*high*3)
            rgb[0::3]=raw[2::4];rgb[1::3]=raw[1::4];rgb[2::3]=raw[0::4]
            return bytes(rgb),wide,high
        finally:
            if previous and memory:g.SelectObject(memory,previous)
            if bitmap:g.DeleteObject(bitmap)
            if memory:g.DeleteDC(memory)
            if screen:self.user.ReleaseDC(None,screen)

    def wait_opener(self):
        deadline=time.monotonic()+min(15,self.remaining())
        attempts=0
        while time.monotonic()<deadline:
            raw,wide,high=self.observe((200,16,340,52));attempts+=1
            corners=[]
            for x,y in ((3,3),(wide-4,3),(3,high-4),(wide-4,high-4)):
                at=(y*wide+x)*3;corners.append(tuple(raw[at:at+3]))
            colors=((53,48,57),(79,26,35))
            if sum(any(all(abs(a-b)<=1 for a,b in zip(pixel,color)) for color in colors) for pixel in corners)>=3:
                self.record.setdefault('presentation_observations',[]).append(dict(
                    label='initial_opener',kind='owned_visible_client_os_readback',attempts=attempts,
                    sha256=hashlib.sha256(raw).hexdigest(),corners=corners))
                return
            self.pause(.1)
        raise RuntimeError('Initial native opener never became visibly presented')

    def header(self):
        width,height=self.size()
        return self.observe((0,0,width,round(height*.24)))[0]

    def wait_restored_header(self, previous):
        deadline=time.monotonic()+min(15,self.remaining());attempts=0
        while time.monotonic()<deadline:
            raw=self.header();attempts+=1
            if raw != previous:
                self.record.setdefault('presentation_observations',[]).append(dict(
                    label='restored_header_changed',kind='owned_visible_client_os_readback',attempts=attempts,
                    before_sha256=hashlib.sha256(previous).hexdigest(),sha256=hashlib.sha256(raw).hexdigest(),
                    limitation='Changed menu-header pixels; final compiled state independently verifies the restored owner and callbacks.'))
                return
            self.pause(.1)
        raise RuntimeError('Restored menu header never became visibly distinct')

    def open_menu(self):
        # SDL deliberately may eat a first focus click. Retrying this opener is
        # safe: once the fullscreen modal exists the same point hits its header,
        # not the opener behind it. Never retry a value-changing menu action.
        for attempt in range(3):
            if self.user.GetForegroundWindow()!=self.hwnd:
                self.find()
            self.click('pref.open',270,34)
            stop=time.monotonic()+min(1,self.remaining())
            while time.monotonic()<stop:
                raw=self.header();wide,high=self.size();high=round(high*.24)
                corners=[tuple(raw[(y*wide+x)*3:(y*wide+x)*3+3])
                         for x,y in ((3,3),(wide-4,3),(3,high-4),(wide-4,high-4))]
                if all(all(abs(a-b)<=1 for a,b in zip(pixel,(30,27,32))) for pixel in corners):
                    self.record.setdefault('presentation_observations',[]).append(dict(
                        label='opened_modal_presented',kind='owned_visible_client_os_readback',
                        opener_attempts=attempt+1,sha256=hashlib.sha256(raw).hexdigest(),corners=corners))
                    return
                self.pause(.1)
        raise RuntimeError('Native settings modal never became visibly presented')

    def tab(self):
        self.post(0x100, 9, 1 | (0x0f << 16))
        self.pause(.12)
        self.post(0x101, 9, 1 | (0x0f << 16) | (3 << 30))
        self.pause(.5)

    def click(self, action, x, y):
        width, height = self.size()
        check(0 <= x < width and 0 <= y < height, 'Click outside owned client rectangle')
        packed = int(x) | (int(y) << 16)
        self.record.setdefault('activations', []).append(dict(action=action, point=[x, y], client_size=[width, height]))
        # SDL can otherwise replace an injected position with the OS global
        # cursor. Existing editor_window.py uses this same targeted middle hold.
        self.post(0x200, 0, packed)
        self.post(0x207, 0x10, packed)
        self.pause(.12)
        self.post(0x200, 0x10, packed)
        self.post(0x201, 0x11, packed)
        self.pause(.15)
        self.post(0x202, 0x10, packed)
        self.pause(.15)
        self.post(0x208, 0, packed)
        self.pause(.5)

    def activate(self, action, target):
        width, height = self.size()
        px, py = target['center_percent']
        self.click(action, round(width*px/100), round(height*py/100))

    def close(self):
        if self.owned():
            self.post(0x10, closing=True)

    def restore_dpi(self):
        if self.old_dpi:
            check(self.user.SetThreadDpiAwarenessContext(self.old_dpi), 'Cannot restore thread DPI context')
            self.old_dpi = None


def capture_oracle(path, targets):
    """Native readback must contain the authored panel AND visible menu rows."""
    raw = path.read_bytes()
    check(54 <= len(raw) <= 16*1024*1024 and raw[:2] == b'BM', 'Invalid/bounded native BMP readback')
    offset, dib = struct.unpack_from('<II', raw, 10)
    width, height, planes, bits, compression = struct.unpack_from('<iiHHI', raw, 18)
    check(dib >= 40 and width == WIDTH and abs(height) == HEIGHT and planes == 1 and
          bits in (24, 32) and compression in (0, 3), 'Readback BMP format/extent differs')
    if compression == 3:
        check(bits == 32 and dib >= 56 and
              struct.unpack_from('<IIII',raw,54)==(0xff0000,0xff00,0xff,0xff000000),
              'Readback BMP bitfield masks differ')
    stride = ((width*bits+31)//32)*4
    check(offset >= 14+dib and offset+stride*abs(height) <= len(raw), 'BMP pixels truncated')
    def pixel(x, y):
        row = y if height < 0 else abs(height)-1-y
        point = offset+row*stride+x*(bits//8)
        return tuple(reversed(raw[point:point+3]))
    def matches(value, color):
        return all(abs(a-b) <= 1 for a, b in zip(value, color))
    panel = sum(matches(pixel(x, y), (30, 27, 32))
                for y in range(0, HEIGHT, 4) for x in range(0, WIDTH, 4))
    check(panel > (WIDTH//4)*(HEIGHT//4)*.3, 'Readback does not contain the visible compiled modal panel')
    rows = {}
    for action, target in targets.items():
        left, top, wide, high = target['bounds_percent']
        # Interior corners avoid centered text; refreshed button may be hovered.
        colors = [(114, 47, 55)] if action == 'pref.close' else [(53, 48, 57), (79, 26, 35)]
        samples = [pixel(round(WIDTH*(left+dx)/100), round(HEIGHT*(top+dy)/100))
                   for dx in (1, wide-1) for dy in (1, high-1)]
        observed = sum(any(matches(value, color) for color in colors) for value in samples)
        check(observed >= 3, 'Visible native menu rectangle absent/misplaced: ' + action)
        rows[action] = dict(matching_interior_corners=observed, samples=samples)
    return dict(kind='native_renderer_readback', sha256=sha(path), bytes=len(raw),
                width=width, height=abs(height), panel_sample_count=panel, button_oracles=rows,
                limitation='Opaque native UI rectangles; no OCR, scene projection, physical input or performance claim.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    check(os.name == 'nt', 'Use native Windows Python with an unlocked interactive desktop')
    check(not sys.flags.optimize, 'Run without Python optimization')
    check(0 <= args.gpu <= 4095 and math.isfinite(args.timeout) and 30 <= args.timeout <= 1800,
          'GPU must be 0..4095 and timeout 30..1800 seconds')
    binary, distribution, artifact = (path.resolve(strict=True) for path in
                                     (args.binary, args.runtime, args.artifact))
    check(binary.is_file() and distribution.is_dir() and artifact.is_file() and
          artifact.name == 'native-gameplay.json', 'Supply executable, installed runtime and native-gameplay.json')
    output = args.output.resolve()
    check(not output.exists() and not output.is_relative_to(distribution) and
          not output.is_relative_to(artifact.parent), 'Output must be new and outside supplied closures')
    output.mkdir(parents=True)
    logs = output/'logs'; logs.mkdir()
    started = time.monotonic(); deadline = started+args.timeout
    record = dict(passed=False, runner_sha256=sha(__file__), binary_sha256=sha(binary),
        artifact_sha256=sha(artifact), gpu=args.gpu, commands=[], author_calls=[], checks=[],
        cleanup_errors=[], started_monotonic=started,
        input_source='Targeted synthetic Win32 messages to one PID-owned foreground SDL HWND; middle hold pins SDL position.',
        physical_input_qualified=False, limitations=[
            'One supplied NativeAOT fixture and selected GPU; no whole Alpha 1 or imported-character qualification.',
            'Windows host may have .NET installed; source deletion and sanitized environment are not a clean-machine deployment test.',
            'No physical controls, audio sink, broad graphics settings, rollback faults, or settings persistence qualification.',
            'Input spacing is not an observed presentation barrier; acceptance uses native save/report/readback oracles.',
            'Relocated bundle/evidence are retained; only owned source is deleted and owned processes are stopped.'])
    client = process = window = None
    player_streams = []
    player_entry = None
    protected_runtime = protected_artifact = None

    def remaining():
        value = deadline-time.monotonic()
        check(value > 0, 'Overall qualification deadline exceeded')
        return value

    def pause(duration):
        stop = time.monotonic()+min(duration, remaining())
        while time.monotonic() < stop:
            remaining()
            check(process is None or process.poll() is None, 'Player exited during input spacing; see logs')
            time.sleep(min(.05, max(0, stop-time.monotonic())))

    def cleanup_owned(child, entry):
        if child.poll() is None:
            entry['forced_cleanup'] = True
            killer = Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'
            try:
                killed = subprocess.run([str(killer), '/PID', str(child.pid), '/T', '/F'],
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
                entry['tree_cleanup_exit_code'] = killed.returncode
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
            try:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=10)
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        entry['exit_code'] = child.poll()

    def begin(command, cwd, env=None):
        remaining()
        index = len(record['commands'])
        out = logs/('command-'+str(index)+'.stdout')
        err = logs/('command-'+str(index)+'.stderr')
        entry = dict(command=list(map(str, command)), cwd=str(cwd), stdout=str(out), stderr=str(err),
                     exit_code=None, timed_out=False, forced_cleanup=False)
        record['commands'].append(entry)
        streams = [out.open('wb'), err.open('wb')]
        try:
            child = subprocess.Popen(entry['command'], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=streams[0], stderr=streams[1])
            entry['pid'] = child.pid
            return child, entry, streams
        except BaseException:
            for stream in streams:
                stream.close()
            raise

    def cli(executable, *arguments, cwd=None, env=None):
        child, entry, streams = begin([executable, *arguments], cwd or output, env)
        try:
            try:
                child.wait(timeout=min(120, remaining()))
            except subprocess.TimeoutExpired:
                entry['timed_out'] = True
                raise
        finally:
            cleanup_owned(child, entry)
            for stream in streams:
                stream.close()
        check(entry['exit_code'] == 0 and not entry['forced_cleanup'], 'Native CLI failed; see command logs')
        envelope = read_json(entry['stdout'])
        check(envelope.get('status') == 'ok' and 'result' in envelope, 'Unsuccessful native CLI envelope')
        entry['result'] = envelope['result']
        return envelope['result']

    try:
        descriptor = read_json(artifact)
        check(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2 and
              descriptor['engine_version'] in ('0.0.81', '0.0.82') and descriptor['type'] == fixture.GAME and
              descriptor['identity'] == fixture.IDENTITY and descriptor['target_os'] == 'Windows' and
              descriptor['target_arch'] == 'x86_64', 'Supply an actual published .81/.82 Windows settings fixture')
        check((descriptor['call_version'], descriptor['call_bytes'], descriptor['services_version'],
               descriptor['minimum_services_bytes']) == (1, 80, 7, 256) and
              set(descriptor['required_features']) == FEATURES and
              len(descriptor['required_features']) == len(FEATURES), 'Fixture ABI/features differ')
        check(not descriptor['schema'].get('components'), 'Settings fixture unexpectedly declares custom components')
        rows = descriptor['files']; seen = set()
        for row in rows:
            relative = relative_path(row['path'])
            key = relative.as_posix().casefold()
            check(key not in seen and key != 'native-gameplay.json', 'Duplicate/reserved artifact payload path')
            seen.add(key)
            path = artifact.parent/relative
            check(path.resolve(strict=True).is_relative_to(artifact.parent) and path.is_file() and
                  path.stat().st_size == row['size'] and sha(path) == row['sha256'], 'Artifact payload integrity differs')
        libraries = [row for row in rows if row['role'] == 'library']
        check(len(libraries) == 1 and libraries[0]['path'] == descriptor['library'] and
              sum(row['role'] == 'notice' for row in rows) >= 6, 'Native image/notice closure differs')
        protected_artifact = clean_inventory(artifact.parent)
        check(set(protected_artifact) == {artifact.name, *(row['path'] for row in rows)},
              'Published artifact directory must be an exact source-free closure')
        runtime = read_json(distribution/'runtime.json')
        check(runtime['target_os'] == 'Windows' and runtime['target_arch'] == 'x86_64' and
              runtime['gameplay_call_version'] == 1 and runtime['gameplay_call_bytes'] == 80 and
              runtime['gameplay_services_version'] == 7 and runtime['gameplay_services_bytes'] >= 256 and
              FEATURES <= set(runtime['gameplay_features']) and
              all(runtime['features'].get(name) is True for name in ('simulation', 'renderer', 'native_gameplay', 'game_ui')),
              'Selected installed runtime lacks the actual player/settings contract')
        check(cli(binary, 'version')['version'] == runtime['engine_version'], 'Exporter/runtime engine versions differ')
        protected_runtime = clean_inventory(distribution)
        record['runtime_descriptor'] = runtime
        record['artifact_descriptor'] = descriptor
        source_paths = [Path(__file__), Path(fixture.__file__),
            Path(__file__).with_name('player_live_settings_contract.py'),
            Path(__file__).with_name('player_service_contract.py'),
            Path(__file__).with_name('collection_gameplay_bundle.py'),
            ROOT/'tests/managed_player_preferences_gameplay/PlayerPreferencesMenuGame.cs',
            ROOT/'tests/managed_player_preferences_gameplay/Poima.PlayerPreferencesMenuGame.csproj',
            *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
        record['fixture_sources'] = {str(path.relative_to(ROOT)):sha(path) for path in source_paths}
        record['artifact_lineage'] = dict(engine_version=descriptor['engine_version'],
            type=descriptor['type'], image_sha256=libraries[0]['sha256'],
            descriptor_sha256=sha(artifact), running_engine_version=runtime['engine_version'])
        project = output/'Owned settings source project'
        cli(binary, 'project', 'create', project, '--name', 'Exported compiled settings acceptance')
        project_file = project/'project.json'; spec = read_json(project_file)
        # project create ships a starter world already at revision one. The
        # reused contract author deliberately starts a separate fresh world at
        # revision zero; retain the native-created input profile unchanged.
        world_file = project/'compiled-settings-world.json'
        sys.path.insert(0, str(ROOT/'tools/python'))
        from poima_client import WorldClient
        client = WorldClient.open(binary, world_file, cwd=project, close_timeout=10)
        class AuthorClient:
            def call(self, method, params=None):
                result = client.call(method, params, timeout=min(30, remaining()))
                record['author_calls'].append(dict(method=method, params=params, result=result))
                return result
        targets = author(AuthorClient())
        AuthorClient().call('session.close')
        client.close()
        record['author_owner'] = dict(pid=client.transport.process_id,
            exit_code=client.transport.returncode, stderr=client.transport.stderr_tail,
            stderr_truncated=client.transport.stderr_truncated)
        check(record['author_owner']['exit_code'] == 0 and not record['author_owner']['stderr'] and
              not record['author_owner']['stderr_truncated'], 'Native author owner did not exit cleanly')
        client = None
        original_world = read_json(world_file)
        owned_artifact = project/'gameplay'; owned_artifact.mkdir()
        for name in protected_artifact:
            destination = owned_artifact/relative_path(name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(artifact.parent/name, destination)
        spec.update(version=2, audio=False, entry=dict(world=world_file.relative_to(project).as_posix(),
                    controller=fixture.uid(100), camera=fixture.uid(101)),
                    gameplay=dict(descriptor='gameplay/native-gameplay.json', values=dict(Mode=0)))
        project_file.write_text(json.dumps(spec, indent=2)+'\n', encoding='utf-8')
        source_before = clean_inventory(project)
        cli(binary, 'project', 'inspect', project_file)
        exported = output/'Exported settings game'
        cli(binary, 'project', 'build', project_file, '--runtime', distribution, '--output', exported)
        check(clean_inventory(project) == source_before and clean_inventory(distribution) == protected_runtime,
              'Native inspection/export mutated selected runtime or owned source')
        relocation = Path(tempfile.mkdtemp(prefix='Poima settings relocation ', dir=ROOT.parent)).resolve()
        record['relocation_root'] = str(relocation)
        check(not relocation.is_relative_to(ROOT), 'Bundle relocation must be outside the checkout')
        bundle = relocation/'Relocated settings game'; shutil.move(str(exported), bundle)
        shutil.rmtree(project)
        check(not project.exists() and not exported.exists(), 'Owned source/export locations remain available')
        record['source_removed_before_player'] = True
        game_file = bundle/'game.json'; game = read_json(game_file)
        check(game['version'] == 2 and game['gameplay']['values'] == dict(Mode=0), 'Exported launch state differs')
        check(read_json(bundle/'content/world.json') == shipping_document(original_world), 'Exported world projection differs')
        frozen = clean_inventory(bundle)
        check({name[8:]:digest for name,digest in frozen.items() if name.startswith('runtime/')} == protected_runtime,
              'Relocated runtime is not the selected exact distribution')
        check({name[9:]:digest for name,digest in frozen.items() if name.startswith('gameplay/')} == protected_artifact,
              'Relocated AOT closure differs')
        forbidden = {'hostfxr.dll', 'coreclr.dll', 'poima.gameplay.dll', 'poima.managedbridge.dll',
                     'poima.playerpreferencesmenugame.dll', 'libhostfxr.so', 'libcoreclr.so'}
        check(not any(Path(name).name.casefold() in forbidden for name in frozen) and
              not any(managed_pe(bundle/name) for name in frozen), 'Managed development runtime/PE shipped')
        check(not any(Path(name).suffix.casefold() in {'.cs', '.csproj', '.pdb', '.sln'} for name in frozen),
              'Source/debug development payload shipped')
        installed = (bundle/relative_path(game['engine']['executable'])).resolve(strict=True)
        check(installed.is_relative_to(bundle), 'Bundled executable escapes root')
        unrelated = relocation/'Unrelated working directory'; unrelated.mkdir()
        environment = dict(os.environ)
        system = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')):
                environment.pop(key)
        cli(installed, 'game', 'inspect', game_file, cwd=unrelated, env=environment)
        storage = output/'External player saves'; storage.mkdir()
        report = output/'game-report.json'; capture = output/'compiled-settings-menu.bmp'
        overrides = json.dumps({'ui.scale':1, 'camera.vertical_fov':60, 'audio.master_gain':1}, separators=(',', ':'))
        process, player_entry, player_streams = begin([installed, 'game', 'run', game_file,
            '--save-root', storage, '--settings-overrides', overrides, '--gpu', args.gpu,
            '--samples', 1, '--frames-in-flight', 1, '--width', WIDTH, '--height', HEIGHT,
            '--capture', capture, '--report', report], unrelated, environment)
        window = OwnedWindow(process, record, remaining, pause); window.find(); window.wait_opener()
        window.tab()  # Native controller capture -> free/pause UI; no control RPC.
        # The launch override fixes effective UI scale to one; dp opener maps to
        # physical client pixels independently of OS DPI, unlike inherited scale.
        window.open_menu()
        for action in ('pref.ui.plus', 'pref.fov.plus', 'pref.gain.minus', 'pref.refresh', 'pref.stage.save'):
            window.activate(action, targets[action])
        manifest_path = storage/('slot-'+fixture.SLOT)/'current.json'
        save_deadline = time.monotonic()+min(15, remaining())
        while not manifest_path.is_file() and time.monotonic() < save_deadline:
            pause(.05)
        check(manifest_path.is_file(), 'Actual menu Save callback produced no durable witness')
        manifest = read_json(manifest_path)
        current = manifest['payload']['current']
        check(manifest['format'] == 'poima.save-slot' and manifest['version'] == 1 and
              manifest['payload']['generation'] == current['generation'] == 1,
              'Actual compiled Save did not publish exactly generation one')
        payload_relative = relative_path(current['file'])
        check(len(payload_relative.parts) == 1, 'Save witness payload is not a basename')
        payload_path = manifest_path.parent/payload_relative
        check(payload_path.stat().st_size == current['bytes'] and sha(payload_path) == current['sha256'],
              'Durable save witness integrity differs')
        saved_document = read_json(payload_path); saved = saved_document['snapshot']['payload']
        saved_values = numeric_values(saved['gameplay']['values'], descriptor['schema'])
        for name, expected in dict(Mode=0, Available=1, Replay=0, Controls=6, Revision=3,
            TicketSequence=4, StageRejection=0, ResultState=1, ResultRejection=0, AcceptedRevision=-1,
            ReadAfterRevision=3, ReadUnchanged=1, FovPresent=1, UiPresent=1).items():
            check(saved_values[name] == expected, 'Saved actual callback witness differs: '+name)
        for name, expected in dict(Fov=65, UiScale=1.25, Gain=.9).items():
            near(saved_values[name], expected, 'Saved '+name)
        owner = (saved_values['OwnerHigh'], saved_values['OwnerLow'])
        check(owner != (0, 0) and (saved_values['TicketHigh'], saved_values['TicketLow']) == owner,
              'Save witness did not retain the actual player owner/ticket')
        check(saved_values['SaveSequence'] == 1 and
              (saved_values['SaveHigh'], saved_values['SaveLow']) != (0, 0),
              'Actual compiled Save request ticket absent')
        check(saved_values['Ticks'] == saved['tick'], 'Durable paused compiled tick differs')
        check(not any(name in saved for name in ('preferences', 'player_preferences', 'live_settings', 'settings')),
              'Runtime save imported settings authority')
        saved_hashes = clean_inventory(storage)
        record['saved_witness'] = dict(manifest_sha256=sha(manifest_path), payload_sha256=sha(payload_path),
                                      tick=saved['tick'], values=saved_values)
        for action in ('pref.fov.plus', 'pref.refresh'):
            window.activate(action, targets[action])
        previous_header = window.header()
        window.activate('pref.stage.load', targets['pref.stage.load'])
        # Loading can block the owner while later Win32 messages accumulate.
        # Those messages correctly cannot act on pre-replacement hit regions.
        # Isolate this one refresh attempt from the restore. This is pacing,
        # not an observed redraw barrier; keep all final callback assertions.
        record['post_load_input_spacing_seconds'] = 2
        pause(2)
        window.wait_restored_header(previous_header)
        window.activate('pref.refresh', targets['pref.refresh'])
        # Closing captures the still-visible modal after its final real Refresh.
        # Final observations below reject missed clicks, lost redraws, duplicate
        # callbacks, failed load, stale receipts and unapplied settings.
        pause(.5); window.close()
        try:
            process.wait(timeout=min(30, remaining()))
        except subprocess.TimeoutExpired:
            player_entry['timed_out'] = True
            raise
        cleanup_owned(process, player_entry)
        for stream in player_streams:
            stream.close()
        player_streams = []
        check(player_entry['exit_code'] == 0 and not player_entry['forced_cleanup'], 'Exported player failed or required forced cleanup')
        envelope = read_json(player_entry['stdout'])
        check(envelope.get('status') == 'ok', 'Exported interactive game returned an unsuccessful envelope')
        played = envelope['result']; player_entry['result'] = played
        check(read_json(report) == played and played['success'], 'Native final report differs/failed')
        play = played['play']
        check(play['success'] and play['stop_reason'] == 'window_closed' and play['hardware'] and
              play['nvrhi_errors'] == 0 and play['capture_written'] and play['samples'] == 1 and
              play['width'] == WIDTH and play['height'] == HEIGHT, 'Native interactive renderer result differs')
        check(play['runtime_replacements'] == 1 and play['current_session_id'] != play['initial_session_id'] and
              played['runtime']['session_id'] == play['current_session_id'], 'Compiled Load did not replace actual exported runtime')
        check(play['tick'] == played['runtime']['tick'] == saved['tick'], 'Paused controls or restored save advanced simulation')
        module = played['gameplay']['module']; values = numeric_values(module['values'], descriptor['schema'])
        check(module['backend'] == 'native_aot' and module['type'] == fixture.GAME and
              module['schema'] == descriptor['schema'] and module['assembly_sha256'] == libraries[0]['sha256'] and
              module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
              'Actual exported module is not the supplied genuine NativeAOT fixture')
        for name, expected in dict(Mode=0, Available=1, Replay=0, Controls=7, Revision=6,
            TicketSequence=4, StageRejection=0, ResultState=2, ResultRejection=0, AcceptedRevision=4,
            ReadAfterRevision=3, ReadUnchanged=1, FovPresent=1, UiPresent=1,
            EffectiveFovPresent=1, EffectiveUiPresent=1, ObservedRevision=6,
            AppliedRevision=6, PresentedRevision=6).items():
            check(values[name] == expected, 'Actual restored/queried callback state differs: '+name)
        check((values['OwnerHigh'], values['OwnerLow']) == owner and
              (values['TicketHigh'], values['TicketLow']) == owner, 'Load changed preference owner or retained ticket')
        for name, expected in dict(Fov=80, UiScale=1.75, Gain=.7, EffectiveFov=80, EffectiveUi=1.75).items():
            near(values[name], expected, 'Compiled final '+name)
        live = play['live_settings']
        check(live['revision'] == 6, 'Final native configuration revision differs')
        for name, expected in {'camera.vertical_fov':80, 'ui.scale':1.75, 'audio.master_gain':.7}.items():
            near(live['values'][name], expected, 'Native configured '+name)
        application = live['application']
        for name in ('observed_revision', 'applied_revision', 'presented_revision'):
            check(application[name] == 6, 'Final native application observation differs: '+name)
        for name, expected in dict(effective_vertical_fov=80, effective_ui_scale=1.75, requested_master_gain=.7).items():
            near(application[name], expected, 'Native actual '+name)
        check(application['audio_outcome'] == 'disabled' and application['sink_gain'] is None,
              'Audio-disabled export invented a verified sink gain')
        check(clean_inventory(storage) == saved_hashes, 'Loading/settings changes modified durable save witness')
        record['capture'] = capture_oracle(capture, targets)
        check(clean_inventory(bundle) == frozen and clean_inventory(distribution) == protected_runtime and
              clean_inventory(artifact.parent) == protected_artifact, 'Player mutated bundle or supplied closures')
        check(all(sha(ROOT/name) == digest for name,digest in record['fixture_sources'].items()),
              'Fixture sources changed during qualification')
        record['bundle_inventory'] = frozen
        record['result'] = played
        record['checks'] = [
            'Owned foreground HWND keyboard release and actual pointer hit tests dispatch genuine compiled modal/tuning/save/load controls.',
            'Durable compiled Save retains staged ticket4 and committed callback revision3; later Load commits settings revision6 before replacing runtime.',
            'Restored callback queries saved ticket4 accepted revision4 while fresh owner snapshot/actual presentation remain revision6, FOV80, UI1.75, gain0.7.',
            'Native hardware final report and renderer BMP contain the visible settings/checkpoint menu; paused saved tick survives one actual runtime replacement.',
            'Owned source removed before execution, exact relocated NativeAOT/runtime closure, sanitized environment, no managed PE, immutable bundle and durable save.']
        record['passed'] = not record['cleanup_errors']
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if window:
            try:
                window.close()
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        if process and player_entry:
            if process.poll() is None:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            cleanup_owned(process, player_entry)
        for stream in player_streams:
            stream.close()
        if client:
            try:
                client.close()
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        if window:
            try:
                window.restore_dpi()
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        if record['cleanup_errors'] or any(row['forced_cleanup'] or row['exit_code'] != 0 for row in record['commands']):
            record['passed'] = False
        record['elapsed_seconds'] = time.monotonic()-started
        record.pop('started_monotonic', None)
        (output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(passed=record['passed'], checks=len(record['checks']), evidence=str(output/'evidence.json'))))
    if not record['passed']:
        raise SystemExit('Settings exported-player qualification failed; see evidence.json')


if __name__ == '__main__':
    main()
