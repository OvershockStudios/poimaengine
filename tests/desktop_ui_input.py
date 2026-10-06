#!/usr/bin/env python3
"""Windows Game HWND presentation input through real compiled Control callbacks."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback
import uuid
from desktop_gameplay_save_requests import Desktop, check
from gameplay_save_requests import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bridge', 'binary', 'hostfxr', 'gameplay-bridge', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=-1)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Use native Windows Python with an unlocked desktop.')
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    record = dict(success=False, checks=[], calls=[], polls=[], gpu=args.gpu,
                  hashes={name: sha(getattr(args, name)) for name in ('bridge', 'binary', 'hostfxr', 'gameplay_bridge', 'assembly')},
                  test_sha256=sha(__file__), limitations=['Synthetic semantic input into a real HWND/Vulkan presentation; no physical device or Avalonia widget claim.'])
    desktop = None
    windows = []
    try:
        project = run/'Project'
        result = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'UI input'], capture_output=True, text=True, timeout=30)
        check(result.returncode == 0, result.stdout+result.stderr)
        world = project/json.loads((project/'project.json').read_text())['entry']['world']
        desktop = Desktop(args, world, record)
        if args.gpu != -1:
            desktop.lib.poima_desktop_destroy(desktop.host)
            desktop.host = desktop.lib.poima_desktop_create(str(world).encode(), desktop.endpoint.encode(), args.gpu, 1)
            check(desktop.host, desktop.text(desktop.lib.poima_desktop_error(None)))
        uid = lambda n: f'{n:032x}'
        definitions = [(1, 'panel', '', ''), (2, 'label', 'Original', ''), (3, 'button', 'Pause', 'pause'),
                       (4, 'button', 'Resume', 'resume'), (5, 'button', 'Throw', 'throw')]
        ops = [dict(op='ui.element.set', id=uid(n), element=dict(parent=None if n == 1 else uid(1), name=f'UI {n}', kind=kind,
               text=text, action=action or None, visible=True, enabled=True)) for n, kind, text, action in definitions]
        revision = desktop.rpc('world.inspect')['revision']
        desktop.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=revision, ops=ops))
        revision = desktop.rpc('world.inspect')['revision']
        storage = run/'Saves'; storage.mkdir()
        desktop.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(storage)))
        profile = dict(hostfxr=str(args.hostfxr.resolve()), bridge=str(args.gameplay_bridge.resolve()), assembly=str(args.assembly.resolve()), type='Poima.Tests.ManagedUiGame')
        desktop.rpc('desktop.gameplay.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, profile=profile))
        sid = uuid.uuid4().hex
        desktop.rpc('desktop.play.start', dict(session_id=sid, revision=revision, expected_gameplay_generation=1, paused=True))
        camera = desktop.rpc('desktop.cameras')['cameras'][0]['id']
        desktop.rpc('desktop.game.camera', dict(camera=camera))
        lib = desktop.lib
        for name, types, returned in [('attach_view', [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p], ctypes.c_int),
                                      ('draw_view', [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
                                      ('detach_view', [ctypes.c_void_p, ctypes.c_char_p], None)]:
            fn = getattr(lib, 'poima_desktop_'+name); fn.argtypes, fn.restype = types, returned
        user = ctypes.WinDLL('user32', use_last_error=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]; kernel.GetModuleHandleW.restype = wintypes.HMODULE
        user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p]
        user.CreateWindowExW.restype = wintypes.HWND
        user.DestroyWindow.argtypes = [wintypes.HWND]
        user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
        user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
        user.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        user.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        parent = user.CreateWindowExW(0, 'STATIC', 'Poima UI input qualification', 0x12CF0000, 80, 80, 900, 640, None, None, kernel.GetModuleHandleW(None), None)
        check(parent, ctypes.get_last_error()); windows.append(parent)
        child = user.CreateWindowExW(0, 'STATIC', '', 0x56000000, 0, 0, 800, 540, parent, None, kernel.GetModuleHandleW(None), None)
        check(child, ctypes.get_last_error()); windows.append(child)
        check(lib.poima_desktop_attach_view(desktop.host, b'game', child) == 1, desktop.text(lib.poima_desktop_error(desktop.host)))
        def draw():
            message = wintypes.MSG()
            while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                user.TranslateMessage(ctypes.byref(message)); user.DispatchMessageW(ctypes.byref(message))
            check(lib.poima_desktop_draw_view(desktop.host, b'game') == 1, desktop.text(lib.poima_desktop_error(desktop.host)))
        def event(kind, **extra):
            request = dict(session_id=sid, camera=camera, request_id=uuid.uuid4().hex, kind=kind, **extra)
            return request, desktop.rpc('desktop.ui.input', request)
        def calls(): return desktop.gameplay(sid)['module']['values']['ControlCalls']
        draw()
        event('pointer_down', x=65, y=80)
        release, accepted = event('pointer_up', x=65, y=80)
        check(accepted['activated'] == uid(3) and calls() == 1, 'First displayed click was dropped or targeted wrong button.')
        check(desktop.rpc('runtime.inspect', dict(session_id=sid))['tick'] == 0, 'UI click advanced simulation.')
        duplicate = desktop.rpc('desktop.ui.input', release)
        check(duplicate['replayed'] and calls() == 1, 'Presentation retry executed twice.')
        desktop.rpc('desktop.ui.input', dict(release, x=66), error=-32010)
        record['checks'].append('First displayed click executes real compiled Control once at unchanged Tick; duplicate receipt and changed-payload rejection.')
        draw(); event('pointer_down', x=65, y=80); event('pointer_up', x=750, y=480)
        check(calls() == 1, 'Drag-out activated.')
        draw(); event('pointer_down', x=65, y=80)
        user.SetWindowPos(child, None, 0, 0, 810, 540, 0x14)
        event('pointer_up', x=65, y=80)
        check(calls() == 1, 'Resize completed an old gesture.')
        draw(); event('pointer_down', x=65, y=80); desktop.rpc('desktop.ui.reset'); draw(); event('pointer_up', x=65, y=80)
        check(calls() == 1, 'Reset retained an armed pointer gesture.')
        record['checks'].append('Pointer drag-out, resize and explicit cancellation preserve callback count.')
        draw(); event('focus_next'); event('focus_next'); event('accept_down'); request, resumed = event('accept_up')
        check(resumed['activated'] == uid(4) and calls() == 2 and desktop.rpc('desktop.play.inspect')['state'] == 'playing', 'Keyboard resume did not reach owner.')
        desktop.rpc('desktop.play.pause', dict(session_id=sid)); desktop.rpc('desktop.ui.input', request)
        check(calls() == 2 and desktop.rpc('desktop.play.inspect')['state'] == 'paused', 'Replayed resume revived manually paused playback.')
        event('focus_next'); event('accept_down')
        failed = dict(session_id=sid, camera=camera, request_id=uuid.uuid4().hex, kind='accept_up')
        desktop.rpc('desktop.ui.input', failed, error=-32040)
        desktop.rpc('desktop.ui.input', failed, error=-32040)
        check(calls() == 2, 'Rejected callback or its retry escaped rollback.')
        record['checks'].append('Keyboard activation applies Resume once; rejected compiled callback retains rollback and failure on retry.')
        # Change authoritative nonmodal HUD state between actual draws. The
        # still-visible old button must own input without running stale Control.
        def hud(text):
            state = desktop.rpc('runtime.inspect', dict(session_id=sid))
            desktop.rpc('runtime.ui.edit', dict(session_id=sid, request_id=uuid.uuid4().hex,
                expected_tick=state['tick'], expected_ui_revision=state['ui_revision'],
                edits=[dict(id=uid(2), text=text)]))
        draw(); event('pointer_down', x=65, y=80); hud('Changed')
        _, stale = event('pointer_up', x=65, y=80)
        check(stale['consumed'] and stale['activated'] is None and calls() == 2, 'Between-draws HUD update leaked an owned release or activated stale UI.')
        _, stale = event('pointer_down', x=65, y=80)
        check(stale['consumed'] and stale['activated'] is None, 'Still-visible stale button leaked a gameplay acquisition click.')
        _, release_outside = event('pointer_up', x=750, y=480)
        check(release_outside['consumed'] and release_outside['activated'] is None, 'Stale UI lost drag-out release ownership.')
        _, outside = event('pointer_down', x=750, y=480)
        check(not outside['consumed'], 'Stale nonmodal UI swallowed input outside displayed regions.')
        event('pointer_up', x=750, y=480)
        event('pointer_down', x=65, y=80); draw()
        _, redrawn = event('pointer_up', x=65, y=80)
        check(redrawn['consumed'] and redrawn['activated'] is None and calls() == 2, 'Redraw rearmed a stale consumed press.')
        event('pointer_down', x=65, y=80); _, recovered = event('pointer_up', x=65, y=80)
        check(recovered['activated'] == uid(3) and calls() == 3, 'Fresh presentation did not restore ordinary button activation.')
        record['checks'].append('Between-draws nonmodal HUD changes consume old visible hits and held releases without activation; outside input stays free and fresh presentation restores activation.')

        desktop.rpc('desktop.ui.input', dict(session_id=uuid.uuid4().hex, camera=camera, request_id=uuid.uuid4().hex, kind='focus_next'), error=-32009)
        lib.poima_desktop_detach_view(desktop.host, b'game')
        record['success'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if desktop: desktop.close()
        if windows:
            for hwnd in reversed(windows): user.DestroyWindow(hwnd)
        path = run/'evidence.json'; path.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(success=record['success'], checks=len(record['checks']), evidence=str(path))))
    if not record['success']: print(record.get('error', 'failed'), file=sys.stderr)
    return 0 if record['success'] else 1


if __name__ == '__main__': raise SystemExit(main())
