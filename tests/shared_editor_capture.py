#!/usr/bin/env python3
"""Windows GPU integration: two CLI clients share one live native editor.

Uses real IPC/world transactions and deferred viewport screenshots. Inspector
draft setup uses the same scripted UI helpers, not synthetic physical input.
Only the owned editor HWND receives WM_CLOSE; no global input is generated.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid
from scene_capture import pixels

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--gpu', type=int, default=0)
args = parser.parse_args()
assert os.name == 'nt', 'Run this lifecycle harness with Windows Python.'
run = args.output / uuid.uuid4().hex
run.mkdir(parents=True)
binary = str(args.binary.resolve())
record = {'passed': False, 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'gpu': args.gpu, 'requests': [], 'responses': [], 'editors': [], 'checks': [],
          'limitations': ['No physical input or unattended agent policy qualification.',
                         'Inspector draft setup uses the visible UI action dispatcher through a script.']}
u = c.WinDLL('user32', use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.PostMessageW.restype = w.BOOL
editors, clients = [], []

def uid():
    return uuid.uuid4().hex

def transform(x=0):
    return {'position': [x, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [2, 2, 2]}

def component(entity, kind, value):
    return {'op': 'component.set', 'id': entity, 'type': kind, 'value': value}

def wait_for(predicate, description, timeout=30):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError('Timed out waiting for '+description)

class Editor:
    def __init__(self, label, actions=None, seed_ops=None):
        self.root = run/label
        self.root.mkdir()
        self.world, self.report = self.root/'world.json', self.root/'report.json'
        self.endpoint = 'poima-test-'+uid()
        if seed_ops is not None:
            seed = {'jsonrpc': '2.0', 'id': 1, 'method': 'world.transact',
                    'params': {'base_revision': 0, 'request_id': uid(), 'ops': seed_ops}}
            seeded = subprocess.run([binary, 'world', str(self.world.resolve())],
                input=json.dumps(seed)+'\n', text=True, capture_output=True, timeout=30)
            assert seeded.returncode == 0 and 'result' in json.loads(seeded.stdout), seeded.stdout+seeded.stderr
        command = [binary, 'editor', str(self.world.resolve()), '--endpoint', self.endpoint,
                   '--gpu', str(args.gpu), '--width', '1440', '--height', '900', '--no-layout', '--report', str(self.report.resolve())]
        if actions is not None:
            script = self.root/'actions.json'
            script.write_text(json.dumps({'actions': actions}), encoding='utf-8')
            command += ['--script', str(script.resolve())]
        self.stdout, self.stderr = self.root/'stdout.txt', self.root/'stderr.txt'
        self.streams = [self.stdout.open('w', encoding='utf-8'), self.stderr.open('w', encoding='utf-8')]
        self.process = subprocess.Popen(command, stdout=self.streams[0], stderr=self.streams[1])
        self.entry = {'command': command, 'endpoint': self.endpoint}
        record['editors'].append(self.entry)
        editors.append(self)
        self.hwnd = wait_for(self.window, 'owned editor window')

    def window(self):
        assert self.process.poll() is None, 'Editor exited before opening a window.'
        found = []
        @callback
        def visit(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, c.byref(pid))
            if pid.value == self.process.pid:
                title = c.create_unicode_buffer(512)
                u.GetWindowTextW(hwnd, title, len(title))
                if title.value.startswith('Poima Editor'):
                    found.append(hwnd)
            return True
        u.EnumWindows(visit, 0)
        return found[0] if found else None

    def close(self):
        if self.process.poll() is None:
            assert u.PostMessageW(self.hwnd, 0x0010, 0, 0), c.get_last_error()
            assert self.process.wait(timeout=30) == 0, 'Editor failed during owned-window close.'
        evidence = json.loads(self.report.read_text(encoding='utf-8'))
        self.entry['report'] = evidence
        assert evidence['success'] and evidence['render']['hardware'], evidence
        assert evidence['render']['nvrhi_errors'] == 0, evidence

class Client:
    def __init__(self, editor, label):
        self.label = label
        self.stderr_path = editor.root/(label+'-stderr.txt')
        self.stderr = self.stderr_path.open('w', encoding='utf-8')
        self.process = subprocess.Popen([binary, 'connect', editor.endpoint, '--timeout-ms', '30000'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        def read():
            try:
                for line in self.process.stdout:
                    self.lines.put(json.loads(line))
            except BaseException as error:
                self.lines.put(error)
            finally:
                self.lines.put(None)
        threading.Thread(target=read, daemon=True).start()
        clients.append(self)

    def request(self, method, **params):
        request = {'jsonrpc': '2.0', 'id': len(record['requests'])+1, 'method': method, 'params': params}
        record['requests'].append({'client': self.label, **request})
        self.process.stdin.write(json.dumps(request)+'\n')
        self.process.stdin.flush()
        response = self.lines.get(timeout=40)
        assert isinstance(response, dict), f'Connection ended or returned malformed JSON: {response!r}'
        record['responses'].append({'client': self.label, **response})
        assert response.get('id') == request['id'], response
        return response

    def ok(self, method, **params):
        response = self.request(method, **params)
        assert 'result' in response, response
        return response['result']

    def error(self, method, code=None, **params):
        response = self.request(method, **params)
        assert 'error' in response and (code is None or response['error']['code'] == code), response
        return response['error']

    def transact(self, revision, ops):
        return self.ok('world.transact', request_id=uid(), base_revision=revision, ops=ops)

    def capture(self, editor, label, revision, source='authored', tick=None, scene_revision=None):
        path = editor.root/(label+'.bmp')
        result = self.ok('editor.capture', revision=revision, path=str(path.resolve()))
        assert result['revision'] == revision and result['source'] == source, result
        assert result['scene_revision'] == (revision if scene_revision is None else scene_revision), result
        assert result['tick'] == tick and result['frame'] >= 0, result
        assert Path(result['path']).resolve() == path.resolve(), result
        image = pixels(path)
        assert [len(image[0]), len(image)] == [result['width'], result['height']], result
        layout = self.ok('editor.inspect')['layout']
        CAPTURE_RECTS[id(image)] = layout['scene_viewport']
        editor.entry.setdefault('captures', []).append({**result, 'layout': layout, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        return path, image

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            assert self.process.wait(timeout=10) == 0, 'CLI bridge failed during close.'

CAPTURE_RECTS = {}
def scene_pixels(image):
    # Use the actual docked Scene content area; omit help/diagnostic overlays.
    x, y, width, height = CAPTURE_RECTS[id(image)]
    left, right = max(0, int(x)+8), min(len(image[0]), int(x+width)-8)
    top, bottom = max(0, int(y)+100), min(len(image), int(y+height)-8)
    assert right > left and bottom > top, 'Scene viewport is unavailable for pixel comparison.'
    return [pixel for row in image[top:bottom] for pixel in row[left:right]]

try:
    editor = Editor('shared')
    first, second = Client(editor, 'first'), Client(editor, 'second')
    described = first.ok('editor.describe')
    for method in ('editor.inspect', 'editor.capture', 'editor.select', 'editor.play', 'editor.step'):
        assert method in described['methods'], described
    assert second.ok('world.inspect')['revision'] == 0
    entity = uid()
    first.transact(0, [{'op': 'entity.create', 'id': entity, 'name': 'Remote cube'},
        component(entity, 'Transform', transform()),
        component(entity, 'MeshRenderer', {'primitive': 'box', 'albedo': [.9, .04, .02], 'visible': True})])
    second.ok('editor.select', id=entity)
    state = second.ok('editor.inspect')
    assert state['revision'] == 1 and state['selected'] == entity and not state['draft']['dirty'], state
    assert state['draft']['components']['Transform'] == transform(), state
    initial_path, initial = first.capture(editor, 'initial', 1)
    second.transact(1, [component(entity, 'Transform', transform(3)),
        component(entity, 'MeshRenderer', {'primitive': 'box', 'albedo': [.02, .04, .9], 'visible': True})])
    state = first.ok('editor.inspect')
    assert state['revision'] == 2 and state['draft']['components']['Transform'] == transform(3), state
    _, changed = second.capture(editor, 'changed', 2)
    changed_pixels = sum(a != b for a, b in zip(scene_pixels(initial), scene_pixels(changed)))
    assert changed_pixels > 500, f'Expected visible scene change; only {changed_pixels} pixels differed.'
    first.ok('world.undo', request_id=uid(), base_revision=2)
    _, restored = first.capture(editor, 'restored', 3)
    assert scene_pixels(restored) == scene_pixels(initial), 'Undo did not restore viewport scene pixels.'
    record['checks'].append('two clients share authoring, Inspector refresh, undo and fresh scene pixels')
    existing = initial_path.read_bytes()
    second.error('editor.capture', revision=3, path=str(initial_path.resolve()))
    assert initial_path.read_bytes() == existing, 'Capture replaced an existing output.'
    stale = editor.root/'stale.bmp'
    second.error('editor.capture', -32009, revision=2, path=str(stale.resolve()))
    assert not stale.exists()
    for method in ('world.capture', 'runtime.capture', 'asset.animation.capture', 'runtime.play'):
        first.error(method, -32080)
    record['checks'].append('stale/overwriting screenshots and nested graphics commands are rejected')
    runtime_id = uid()
    first.ok('runtime.start', session_id=runtime_id, revision=3)
    state = second.ok('editor.inspect')['runtime']
    assert state['active'] and state['paused'] and state['session_id'] == runtime_id and state['tick'] == 0, state
    time.sleep(.2)
    assert second.ok('runtime.inspect', session_id=runtime_id)['tick'] == 0
    second.ok('runtime.step', session_id=runtime_id, request_id=uid(), expected_tick=0, ticks=2)
    assert first.ok('editor.inspect')['runtime']['tick'] == 2
    first.ok('editor.step', ticks=3)
    assert second.ok('runtime.inspect', session_id=runtime_id)['tick'] == 5
    second.transact(3, [component(entity, 'Transform', transform(5))])
    first.capture(editor, 'frozen-runtime', 4, source='runtime', tick=5, scene_revision=3)
    first.ok('runtime.stop', session_id=runtime_id)
    assert not second.ok('editor.inspect')['runtime']['active']
    first.ok('editor.play')
    state = second.ok('editor.inspect')['runtime']
    assert state['active'] and state['paused'] and state['tick'] == 0, state
    second.ok('editor.pause', paused=True)
    first.ok('editor.step', ticks=1)
    assert second.ok('editor.inspect')['runtime']['tick'] == 1
    second.ok('editor.stop')
    record['checks'].append('remote and GUI runtime controls synchronize; fresh captures identify frozen runtime revision/tick')
    _, healthy = second.capture(editor, 'before-missing-asset', 4)
    missing_asset = 'f'*64
    result = first.transact(4, [
        {'op': 'component.remove', 'id': entity, 'type': 'MeshRenderer'},
        component(entity, 'StaticMesh', {'asset': missing_asset, 'primitive': 0, 'visible': True})])
    assert result['revision'] == 5
    assert second.ok('entity.get', id=entity)['value']['components']['StaticMesh']['asset'] == missing_asset
    def unavailable_scene():
        state = second.ok('editor.inspect')
        return state if state['revision'] == 5 and state['presentation']['error'] and not state['presentation']['current'] else None
    unavailable = wait_for(unavailable_scene, 'recoverable missing-asset presentation error')
    assert unavailable['presentation']['error'] and unavailable['presentation']['has_snapshot'], unavailable
    assert unavailable['presentation']['scene_revision'] == 4, unavailable
    unavailable_path = editor.root/'unavailable.bmp'
    error = first.error('editor.capture', -32003, revision=5, path=str(unavailable_path.resolve()))
    assert 'unavailable' in error['message'].lower() and not unavailable_path.exists(), error
    assert editor.process.poll() is None and second.ok('world.inspect')['revision'] == 5
    first.ok('world.undo', request_id=uid(), base_revision=5)
    _, recovered = second.capture(editor, 'recovered-asset', 6)
    assert scene_pixels(recovered) == scene_pixels(healthy), 'Recovered asset did not restore the last valid scene.'
    state = first.ok('editor.inspect')
    assert state['presentation']['current'] and state['presentation']['error'] is None, state
    record['checks'].append('accepted missing-asset edit keeps GUI/IPC alive, refuses stale capture and recovers after undo')
    first.error('session.close', -32080)
    first.close()
    assert editor.process.poll() is None
    assert second.ok('world.inspect')['revision'] == 6
    third = Client(editor, 'third')
    assert third.ok('editor.inspect')['revision'] == 6
    second.close()
    third.close()
    editor.close()
    record['checks'].append('session.close is rejected without closing GUI; client disconnect preserves authoring and owned-window close flushes report')

    draft_entity, other_entity = uid(), uid()
    draft_editor = Editor('dirty-draft', [
        {'frame': 0, 'op': 'create', 'kind': 'cube', 'id': draft_entity, 'name': 'Draft cube', 'scale': [2, 2, 2]},
        {'frame': 0, 'op': 'draft_component', 'type': 'Transform', 'value': transform(7)},
        # The interactive cap leaves at least six seconds for the real external
        # transaction. Check this deadline explicitly instead of accepting a
        # stale-draft test that never actually encountered a remote revision.
        {'frame': 360, 'op': 'apply_draft', 'type': 'Transform', 'expected_error': True},
        {'frame': 361, 'op': 'reload_draft'},
        {'frame': 362, 'op': 'draft_component', 'type': 'Transform', 'value': transform(9)},
        {'frame': 363, 'op': 'apply_draft', 'type': 'Transform'}])
    draft_client = Client(draft_editor, 'draft-client')
    def draft_ready():
        state = draft_client.ok('editor.inspect')
        return state if state['draft']['dirty'] else None
    before = wait_for(draft_ready, 'scripted Inspector draft')
    assert before['revision'] == 1 and before['draft']['base_revision'] == 1, before
    draft_client.transact(1, [component(draft_entity, 'Transform', transform(2)),
        {'op': 'entity.create', 'id': other_entity, 'name': 'Other entity'}])
    conflict = draft_client.ok('editor.inspect')
    assert conflict['frame'] < 360, 'Fixture deadline elapsed before external draft conflict was inspected.'
    assert conflict['revision'] == 2 and conflict['draft']['dirty'] and conflict['draft']['conflict'], conflict
    assert conflict['draft']['base_revision'] == 1 and conflict['draft']['components']['Transform'] == transform(7), conflict
    draft_client.ok('editor.select', id=draft_entity)
    assert draft_client.ok('editor.inspect')['draft'] == conflict['draft'], 'Same-ID selection discarded a dirty conflict.'
    draft_client.error('editor.select', -32009, id=other_entity)
    assert draft_client.ok('entity.get', id=draft_entity)['value']['components']['Transform'] == transform(2)
    draft_client.capture(draft_editor, 'conflict', 2)
    def draft_applied():
        state = draft_client.ok('editor.inspect')
        return state if state['revision'] == 3 and not state['draft']['dirty'] else None
    applied = wait_for(draft_applied, 'rejected stale Apply followed by Reload and valid Apply', timeout=30)
    assert applied['draft']['base_revision'] == 3 and not applied['draft']['conflict'], applied
    assert applied['draft']['components']['Transform'] == transform(9), applied
    draft_client.capture(draft_editor, 'reloaded-applied', 3)
    draft_client.close()
    draft_editor.close()
    persisted = json.loads(draft_editor.world.read_text(encoding='utf-8'))
    assert persisted['revision'] == 3 and persisted['entities'][draft_entity]['components']['Transform'] == transform(9)
    expected_errors = [a for a in draft_editor.entry['report']['actions'] if a.get('expected_error')]
    assert len(expected_errors) == 1 and expected_errors[0]['op'] == 'apply_draft', expected_errors
    assert 'conflict' in expected_errors[0]['error'].lower(), expected_errors
    record['checks'].append('dirty Inspector survives remote edit, rejects stale Apply/selection, then Reload and valid Apply succeed')

    deleted_entity = uid()
    deletion_editor = Editor('deleted-draft', [
        {'frame': 0, 'op': 'create', 'kind': 'cube', 'id': deleted_entity, 'name': 'Soon deleted', 'scale': [2, 2, 2]},
        {'frame': 0, 'op': 'draft_component', 'type': 'Transform', 'value': transform(11)},
        {'frame': 180, 'op': 'reload_draft'}])
    deletion_client = Client(deletion_editor, 'deletion-client')
    def deletion_draft_ready():
        state = deletion_client.ok('editor.inspect')
        return state if state['draft']['dirty'] else None
    deletion_before = wait_for(deletion_draft_ready, 'Inspector draft before remote deletion')
    deletion_client.ok('editor.select', id=deleted_entity)
    assert deletion_client.ok('editor.inspect')['draft'] == deletion_before['draft']
    deletion_client.transact(1, [{'op': 'entity.delete', 'id': deleted_entity, 'recursive': True}])
    deleted = deletion_client.ok('editor.inspect')
    assert deleted['frame'] < 180, 'Fixture deadline elapsed before remote deletion was inspected.'
    assert deleted['selected'] == deleted_entity and deleted['revision'] == 2, deleted
    assert deleted['draft']['entity'] == deleted_entity and deleted['draft']['dirty'] and deleted['draft']['conflict'], deleted
    assert deleted['draft']['base_revision'] == 1 and deleted['draft']['components']['Transform'] == transform(11), deleted
    deletion_client.error('editor.select', id=deleted_entity)
    assert deletion_client.ok('editor.inspect')['draft'] == deleted['draft'], 'Rejected selection discarded a deleted entity draft.'
    deletion_client.capture(deletion_editor, 'deleted-draft-preserved', 2)
    def deletion_reloaded():
        state = deletion_client.ok('editor.inspect')
        return state if state['selected'] is None and state['draft']['entity'] is None else None
    reloaded = wait_for(deletion_reloaded, 'Reload clearing deleted entity selection')
    assert reloaded['revision'] == 2 and not reloaded['draft']['dirty'] and not reloaded['draft']['conflict'], reloaded
    assert reloaded['draft']['components'] == {}, reloaded
    deletion_client.close()
    deletion_editor.close()
    assert json.loads(deletion_editor.world.read_text(encoding='utf-8'))['entities'] == {}
    record['checks'].append('remote deletion preserves dirty draft; failed same-ID selection retains it; explicit Reload clears selection')
    cold_entity = uid()
    cold_editor = Editor('unavailable-start', seed_ops=[
        {'op': 'entity.create', 'id': cold_entity, 'name': 'Missing at startup'},
        component(cold_entity, 'StaticMesh', {'asset': 'e'*64, 'primitive': 0, 'visible': True})])
    cold_client = Client(cold_editor, 'cold-client')
    cold_state = cold_client.ok('editor.inspect')
    assert cold_state['presentation']['error'] and not cold_state['presentation']['has_snapshot'], cold_state
    assert cold_state['presentation']['scene_revision'] is None and cold_state['presented_revision'] is None, cold_state
    cold_path = cold_editor.root/'unavailable.bmp'
    cold_client.error('editor.capture', -32003, revision=1, path=str(cold_path.resolve()))
    assert not cold_path.exists()
    cold_client.transact(1, [{'op': 'component.remove', 'id': cold_entity, 'type': 'StaticMesh'},
        component(cold_entity, 'MeshRenderer', {'primitive': 'box', 'albedo': [.1, .8, .3], 'visible': True})])
    cold_client.capture(cold_editor, 'startup-repaired', 2)
    assert cold_client.ok('editor.inspect')['presentation']['current']
    cold_client.close()
    cold_editor.close()
    record['checks'].append('invalid startup opens empty fallback with null scene revision and supports live repair/capture')
    record['passed'] = True
except BaseException as error:
    record['failure'] = repr(error)
    raise
finally:
    for client in clients:
        if client.process.poll() is None:
            client.process.kill()
            client.process.wait(timeout=10)
        client.stderr.close()
        record.setdefault('client_diagnostics', []).append({'label': client.label, 'exit_code': client.process.returncode,
            'stderr': client.stderr_path.read_text(encoding='utf-8')})
    for editor in editors:
        if editor.process.poll() is None:
            editor.process.kill()
            editor.process.wait(timeout=10)
        for stream in editor.streams:
            stream.close()
        editor.entry.update(exit_code=editor.process.returncode, stdout=editor.stdout.read_text(encoding='utf-8'),
                            stderr=editor.stderr.read_text(encoding='utf-8'))
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    (args.output/'shared-editor.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(run/'evidence.json')
