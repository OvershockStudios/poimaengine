#!/usr/bin/env python3
"""Owned standalone SDL HWND -> native player -> real compiled C# UI controls.

Targeted synthetic Win32 messages only. No global SendInput or physical-device
claim. Durable compiled Save callbacks witness paused ticks while runtime.play
owns the serialized RPC loop. Final captures are engine readbacks, not screenshots.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
import sys
from pathlib import Path
import threading
import time
import traceback
import uuid
from components_gameplay_contract import Session, sha, uid


def check(value, message):
    if not value:
        raise AssertionError(message)


class OwnedWindow:
    def __init__(self, process, evidence):
        self.process, self.hwnd = process, None
        self.evidence = evidence
        self.user = u = c.WinDLL('user32', use_last_error=True)
        self.callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        u.EnumWindows.argtypes = [self.callback, w.LPARAM]
        u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
        u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
        u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        u.PostMessageW.restype = w.BOOL
        u.SetForegroundWindow.argtypes = [w.HWND]
        u.ShowWindowAsync.argtypes = [w.HWND, c.c_int]
        u.GetForegroundWindow.restype = w.HWND
        u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
        u.GetDpiForWindow.argtypes = [w.HWND]
        u.GetDpiForWindow.restype = w.UINT
        u.SetThreadDpiAwarenessContext.argtypes = [c.c_void_p]
        u.SetThreadDpiAwarenessContext.restype = c.c_void_p
        check(u.SetThreadDpiAwarenessContext(c.c_void_p(-4)), 'Cannot enable per-monitor-aware test coordinates.')

    def owned(self):
        pid = w.DWORD()
        if self.hwnd:
            self.user.GetWindowThreadProcessId(self.hwnd, c.byref(pid))
        return pid.value == self.process.pid

    def find(self, done):
        deadline = time.monotonic()+20
        while time.monotonic() < deadline:
            matches = []
            @self.callback
            def visit(hwnd, _):
                pid = w.DWORD()
                self.user.GetWindowThreadProcessId(hwnd, c.byref(pid))
                if pid.value == self.process.pid:
                    title = c.create_unicode_buffer(512)
                    self.user.GetWindowTextW(hwnd, title, 512)
                    if title.value.startswith('Poima player'):
                        matches.append(hwnd)
                return True
            self.user.EnumWindows(visit, 0)
            check(len(matches) <= 1, 'Child owns multiple player windows; refusing ambiguous input.')
            if matches:
                self.hwnd = matches[0]
                attempts = []
                focused = False
                for _ in range(12):
                    self.user.ShowWindowAsync(self.hwnd, 9)  # SW_RESTORE, owned child only.
                    requested = bool(self.user.SetForegroundWindow(self.hwnd))
                    time.sleep(.25)
                    focused = self.user.GetForegroundWindow() == self.hwnd
                    attempts.append(dict(request_succeeded=requested, actual_foreground=focused))
                    if focused: break
                self.evidence.setdefault('focus_attempts', []).append(dict(attempts=attempts, synthesized=False))
                check(focused, 'Owned player could not receive foreground focus.')
                return
            check(not done.is_set(), 'runtime.play completed before its window appeared.')
            check(self.process.poll() is None, 'Child exited before opening player window.')
            time.sleep(.02)
        raise AssertionError('Owned player window did not appear.')

    def post(self, message, key=0, value=0):
        check(self.owned(), 'Refusing to message a window not owned by the test child.')
        check(self.user.PostMessageW(self.hwnd, message, key, value), f'PostMessage failed: {c.get_last_error()}')

    def key(self, virtual, scan):
        self.post(0x0100, virtual, 1 | (scan << 16))
        self.post(0x0101, virtual, 1 | (scan << 16) | (1 << 30) | (1 << 31))

    def choose(self, index):
        # Successful Control resets presenter focus. Stable IDs order the three
        # eligible buttons: Pause, Save, Resume. No editor semantic input API.
        for _ in range(index):
            self.key(9, 0x0f)
        self.key(13, 0x1c)
        time.sleep(.15)

    def pointer_pause(self):
        rect = w.RECT()
        check(self.user.GetClientRect(self.hwnd, c.byref(rect)), 'Cannot inspect owned client extent.')
        scale = self.user.GetDpiForWindow(self.hwnd)/96
        # Actual generated vertical menu, first button below its single label.
        # If layout changes these clicks must fail rather than falling back to RPC.
        x = round(rect.right*.03+40*scale)
        y = round(rect.bottom*.03+65*scale)
        point = x | (y << 16)
        self.post(0x0200, 0, point)
        self.post(0x0201, 1, point)
        self.post(0x0202, 0, point)
        time.sleep(.15)

    def close(self):
        if self.owned():
            self.post(0x0010)


def wait_saved(root, done):
    manifest = root/'slot-ui-control/current.json'
    deadline = time.monotonic()+10
    while time.monotonic() < deadline:
        if manifest.exists():
            document = json.loads(manifest.read_text(encoding='utf-8'))
            current = document['payload']['current']
            check(Path(current['file']).name == current['file'], 'Unexpected save payload name.')
            payload = manifest.parent/current['file']
            check(sha(payload) == current['sha256'], 'Saved witness payload checksum differs.')
            check(payload.stat().st_size == current['bytes'], 'Saved witness payload size differs.')
            return json.loads(payload.read_text(encoding='utf-8'))['snapshot']['payload'], dict(
                manifest=str(manifest), manifest_sha256=sha(manifest), payload=str(payload), payload_sha256=sha(payload))
        check(not done.is_set(), 'Player completed before compiled Save produced its witness.')
        time.sleep(.025)
    raise AssertionError('Compiled Save callback did not produce a durable witness.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Use native Windows Python with an unlocked desktop.')
    args.native = lambda path: str(Path(path).resolve())
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    evidence = dict(passed=False, checks=[], calls=[], runs=[], gpu=args.gpu,
        hashes={name: sha(getattr(args, name)) for name in ('binary', 'hostfxr', 'bridge', 'assembly')},
        harness_sha256=sha(__file__), input_source='Targeted synthetic Win32 messages to child PID-owned SDL HWND only.',
        physical_input_qualified=False, limitations=['No physical device qualification.', 'Captures are renderer readbacks, not OS screenshots.', 'CoreCLR fixture only; no NativeAOT player-input qualification.'])
    session, window, worker = None, None, None
    try:
        session = Session(args, run/'world.json', evidence)
        camera = 'c'*32
        operations = [dict(op='entity.create', id=camera, name='Menu-only camera'),
            dict(op='component.set', id=camera, type='Transform', value=dict(position=[0,0,6], rotation=[0,0,0,1], scale=[1,1,1])),
            dict(op='component.set', id=camera, type='Camera', value=dict(vertical_fov=60, near=.1, far=100))]
        for number, kind, text, action in [(1,'panel','',''), (2,'label','Original',''), (3,'button','Pause','pause'), (4,'button','Save','save'), (5,'button','Resume','resume')]:
            operations.append(dict(op='ui.element.set', id=uid(number), element=dict(parent=None if number == 1 else uid(1),
                name=f'UI {number}', kind=kind, text=text, action=action or None, visible=True, enabled=True)))
        session.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=operations))
        session.start(1)
        session.load(dict(hostfxr=str(args.hostfxr.resolve()), bridge=str(args.bridge.resolve()), assembly=str(args.assembly.resolve()), type='Poima.Tests.ManagedUiGame'))
        check(session.rpc('runtime.inspect', dict(session_id=session.session))['characters'] == 0, 'Fixture unexpectedly has a controller.')
        window = OwnedWindow(session.process, evidence)
        expected_calls = 0
        for index, resume in enumerate((False, True)):
            storage = run/f'Saves{index}'; storage.mkdir()
            session.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=index, root=str(storage)))
            capture = run/f'compiled-ui-{index}.bmp'
            request = dict(session_id=session.session, request_id=uuid.uuid4().hex, expected_tick=session.tick,
                camera=camera, mode='interactive', gpu=args.gpu, width=640, height=480, samples=1, path=str(capture))
            # Deliberately omit controller. The production World adapter must
            # drive a camera-only world and actual CoreCLR Control handlers.
            result, failed, done = {}, [], threading.Event()
            def play():
                try: result.update(session.rpc('runtime.play', request))
                except BaseException: failed.append(traceback.format_exc())
                finally: done.set()
            worker = threading.Thread(target=play, daemon=True); worker.start()
            window.find(done)
            window.choose(1)  # compiled Pause
            expected_calls += 1
            window.choose(2)  # compiled Save at the paused tick
            expected_calls += 1
            snapshot, witness = wait_saved(storage, done)
            saved = snapshot['gameplay']['values']
            check(saved['ControlCalls'] == expected_calls and saved['LastAction'] == 2, 'Saved witness did not observe precisely Pause then Save.')
            check(int(saved['SeenTick']) == snapshot['tick'] and saved['Ticks'] == snapshot['tick'], 'Save control advanced or misreported its tick.')
            time.sleep(.25)
            if resume:
                window.choose(3)  # compiled Resume
                expected_calls += 1
                time.sleep(.25)
            window.pointer_pause()  # real pointer hit -> compiled Pause
            expected_calls += 1
            time.sleep(.25)
            window.close(); worker.join(timeout=30)
            check(done.is_set(), 'Player did not finish after closing its owned window.')
            check(not failed, '\n'.join(failed))
            check(result['success'] and result['stop_reason'] == 'window_closed' and result['nvrhi_errors'] == 0, result)
            check(result['capture_written'] and capture.exists(), 'Final compiled UI capture was not written.')
            session.tick = result['tick']; session.session = result['current_session_id']
            current = session.game()['module']['values']
            check(current['ControlCalls'] == expected_calls and current['LastAction'] == 4, 'Pointer pause did not reach exactly one real compiled callback.')
            check(int(current['SeenTick']) == session.tick and current['Ticks'] == session.tick, 'Pause did not stop later simulation ticks.')
            check(session.tick > snapshot['tick'] if resume else session.tick == snapshot['tick'], 'Typed Resume/Pause did not govern the real player clock.')
            slot = session.rpc('save.inspect', dict(slot='ui-control'))
            check(slot['selected']['verified'] and slot['generation'] == 1, 'Owner-serviced Save witness failed engine verification.')
            evidence['runs'].append(dict(resume=resume, report=result, saved_tick=snapshot['tick'], saved_values=saved,
                final_values=current, witness=witness, capture=dict(path=str(capture), sha256=sha(capture), kind='renderer_readback')))
            evidence['checks'].append('Compiled Pause/Save and pointer Pause remain at one tick without a controller.' if not resume else
                'Compiled Resume advances real player ticks; pointer Pause stops them; durable owner-serviced Save verifies the prior paused tick.')
            window.hwnd = None
        evidence['passed'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        if window: window.close()
        if worker and worker.is_alive(): worker.join(timeout=10)
        if session:
            if worker and worker.is_alive(): session.process.kill(); session.process.wait(timeout=10)
            try: session.close()
            except BaseException:
                evidence['cleanup_error'] = traceback.format_exc(); evidence['passed'] = False
        path = run/'evidence.json'; path.write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(passed=evidence['passed'], checks=len(evidence['checks']), evidence=str(path))))
    if not evidence['passed']: print(evidence.get('error', evidence.get('cleanup_error')), file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
