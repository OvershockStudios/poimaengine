#!/usr/bin/env python3
"""Exercise native logical UI revisions and durable restores without a renderer."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest
import uuid

PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument('binary', type=Path)
PARSER.add_argument('--runtime', type=int, choices=(0, 1), default=1)
PARSER.add_argument('--windows-interop', action='store_true')
PARSER.add_argument('--evidence', type=Path)
ARGS = PARSER.parse_args()
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / 'build/runtime-ui-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS = []


def uid(value):
    return f'{value:032x}'


def native(path):
    value = str(path.resolve())
    return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if ARGS.windows_interop else value


def tree(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(path.rglob('*')) if p.is_file()}


def element(number, parent, kind, text='', visible=True, enabled=True):
    return dict(op='ui.element.set', id=uid(number), element=dict(parent=uid(parent) if parent else None,
        name=f'Control {number}', kind=kind, text=text, action=uid(number+500) if kind == 'button' else None,
        visible=visible, enabled=enabled))


class Session:
    def __init__(self, world, test):
        self.world, self.test = world, test
        self.stderr = world.with_suffix('.stderr').open('w')
        self.process = subprocess.Popen([str(ARGS.binary.resolve()), 'world', native(world)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=read, daemon=True).start()

    def rpc(self, method, params=None, error=None):
        request = dict(jsonrpc='2.0', id=len(CALLS)+1, method=method, params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n'); self.process.stdin.flush()
        line = self.lines.get(timeout=60)
        assert line is not None, self.world.with_suffix('.stderr').read_text()
        response = json.loads(line)
        CALLS.append(dict(test=self.test, request=request, response=response))
        assert response.get('id') == request['id'], response
        if error is not None:
            assert response.get('error', {}).get('code') == error, response
            return response['error']
        assert 'result' in response, response
        return response['result']

    def close(self):
        if self.stderr.closed:
            return
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()
                raise
        self.stderr.close()
        assert self.process.returncode == 0, self.world.with_suffix('.stderr').read_text()


class RuntimeUi(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.world = self.directory / 'world.json'
        self.saves = self.directory / 'save-root'; self.saves.mkdir()
        self.sessions = []
        self.addCleanup(lambda: [s.close() for s in self.sessions])
        self.process = self.open()
        self.session, self.tick, self.ui_revision, self.revision = uuid.uuid4().hex, 0, 0, 1
        self.ops = [element(100, None, 'panel'), element(101, 100, 'label', 'Health 100'),
                    element(102, 100, 'button', 'Continue'), element(103, 100, 'panel', visible=False),
                    element(104, 103, 'button', 'Confirm'), element(105, 100, 'panel', enabled=False),
                    element(106, 105, 'button', 'Locked')]
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=self.ops))

    def open(self):
        session = Session(self.world, self.id()); self.sessions.append(session); return session

    def rpc(self, *args, **kwargs):
        return self.process.rpc(*args, **kwargs)

    def start(self):
        self.rpc('runtime.start', dict(session_id=self.session, revision=self.revision))

    def inspect(self, **extra):
        return self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick, **extra))

    def edit_params(self, edits, **extra):
        return dict(session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick,
                    expected_ui_revision=self.ui_revision, edits=edits, **extra)

    def edit(self, edits, **extra):
        params = self.edit_params(edits, **extra)
        result = self.rpc('runtime.ui.edit', params)
        self.ui_revision = result['ui_revision']
        return params, result

    def step(self, ticks):
        self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
                                    expected_tick=self.tick, ticks=ticks, inputs=[]))
        self.tick += ticks

    def configure(self):
        return self.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(self.saves)))

    def write_params(self):
        return dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='ui', expected_generation=0,
                    session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=0,
                    expected_ui_revision=self.ui_revision, expected_control_sequence=0)

    def load_params(self):
        return dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='ui', expected_generation=1,
                    revision=self.revision, expected_session_id=self.session,
                    expected_tick=self.tick if self.session else None,
                    expected_gameplay_revision=0 if self.session else None,
                    expected_ui_revision=self.ui_revision if self.session else None,
                    expected_control_sequence=0 if self.session else None, new_session_id=uuid.uuid4().hex)

    def load(self, params):
        result = self.rpc('save.load', params)
        self.session, self.tick = result['session_id'], result['tick']
        self.ui_revision = self.inspect()['ui_revision']
        return result

    def test_discovery_and_authored_ui_independent_of_renderer(self):
        description = self.rpc('world.describe')
        self.assertTrue({'runtime.ui.inspect', 'runtime.ui.edit'} <= set(description['methods']))
        for method in ('save.write', 'save.load'):
            self.assertIn('expected_ui_revision', description['methods'][method]['properties'])
        authored = self.rpc('world.ui.list')
        self.assertEqual([e['id'] for e in authored['elements']], [uid(i) for i in range(100, 107)])
        self.assertEqual(json.loads(self.world.read_text())['version'], 4)

    @unittest.skipUnless(ARGS.runtime, 'Requires native runtime.')
    def test_same_tick_guards_atomic_edits_and_retained_retry(self):
        self.start(); authored = self.world.read_bytes(); history = self.rpc('world.history')
        before = self.inspect()
        self.assertEqual(before['ui_revision'], 0)
        params, result = self.edit([dict(id=uid(101), text='Health <50> & ready')])
        self.assertEqual(result['tick'], 0); self.assertEqual(result['ui_revision'], 1)
        state = self.inspect()
        invalid = [(dict(params, request_id=uuid.uuid4().hex, expected_ui_revision=0), -32009),
                   (dict(params, request_id=uuid.uuid4().hex, expected_tick=1), -32009),
                   (dict(params, request_id=uuid.uuid4().hex, session_id=uid(999)), -32030)]
        for request, error in invalid:
            self.rpc('runtime.ui.edit', request, error=error)
            self.assertEqual(self.inspect(), state)
        bad_edits = [[], [dict(id=uid(101))], [dict(id=uid(101), visible=1)],
                     [dict(id=uid(101), text='partially applied'), dict(id=uid(999), enabled=False)],
                     [dict(id=uid(101), text='a'), dict(id=uid(101), text='b')],
                     [dict(id=uid(101), text='x'*16385)], [dict(id=uid(101), extra=True)],
                     [dict(id=uid(100), text='Panels cannot contain text')]]
        for edits in bad_edits:
            self.rpc('runtime.ui.edit', self.edit_params(edits), error=-32602)
            self.assertEqual(self.inspect(), state)
        self.rpc('runtime.ui.edit', dict(params, expected_ui_revision=0.0), error=-32602)
        self.rpc('runtime.ui.edit', dict(params, edits=[dict(id=uid(101), text='different')]), error=-32010)
        self.edit([dict(id=uid(102), enabled=False)])
        self.step(3); later = self.inspect()
        self.assertEqual(self.rpc('runtime.ui.edit', params), dict(result, replayed=True))
        self.assertEqual(self.inspect(), later, 'Retained retry reapplied old UI data or rewound tick.')
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)

    @unittest.skipUnless(ARGS.runtime, 'Requires native runtime.')
    def test_modal_inheritance_and_guarded_pagination(self):
        self.start()
        def rows(): return {e['id']: e for e in self.inspect()['elements']}
        initial = rows()
        self.assertTrue(initial[uid(102)]['eligible'])
        self.assertFalse(initial[uid(101)]['eligible'])
        self.assertFalse(initial[uid(104)]['effective_visible'])
        self.assertFalse(initial[uid(106)]['effective_enabled'])
        self.edit([dict(id=uid(103), visible=True)], modal=uid(103))
        modal = rows()
        self.assertTrue(modal[uid(104)]['eligible']); self.assertFalse(modal[uid(102)]['eligible'])
        self.assertEqual(self.inspect()['modal'], uid(103))
        before = self.inspect()
        for modal_id in (uid(999), uid(104)):
            self.rpc('runtime.ui.edit', self.edit_params([], modal=modal_id), error=-32602)
            self.assertEqual(self.inspect(), before)
        self.edit([dict(id=uid(104), text='Confirm now')])
        self.assertEqual(self.inspect()['modal'], uid(103), 'Omitted modal silently cleared it.')
        self.edit([], modal=None)
        self.assertIsNone(self.inspect()['modal']); self.assertTrue(rows()[uid(102)]['eligible'])
        result, after = [], None
        while True:
            page = self.inspect(ui_revision=self.ui_revision, limit=2, **({'after': after} if after else {}))
            result.extend(page['elements']); after = page['next_after']
            if after is None: break
            self.assertLess(len(result), 20, 'Pagination failed to make progress.')
        self.assertEqual(result, self.inspect()['elements'])
        stale = self.ui_revision
        self.edit([dict(id=uid(100), enabled=False)])
        self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick, ui_revision=stale), error=-32009)
        self.assertTrue(all(not e['eligible'] for e in rows().values()))

    @unittest.skipUnless(ARGS.runtime, 'Requires native runtime.')
    def test_save_guards_frozen_authoring_fresh_process_and_retry(self):
        self.start(); self.configure(); self.step(4)
        self.edit([dict(id=uid(101), text='Saved runtime label'), dict(id=uid(103), visible=True)], modal=uid(103))
        saved = self.inspect(); write = self.write_params(); store = tree(self.saves)
        missing = dict(write); missing.pop('expected_ui_revision')
        self.rpc('save.write', missing, error=-32602)
        self.rpc('save.write', dict(write, expected_ui_revision=0), error=-32009)
        self.assertEqual(tree(self.saves), store); self.assertEqual(self.inspect(), saved)
        written = self.rpc('save.write', write); self.assertEqual(written['generation'], 1)
        self.edit([dict(id=uid(101), text='New live state')]); self.step(2)
        newer = self.inspect()
        self.assertEqual(self.rpc('save.write', write), dict(written, replayed=True))
        self.assertEqual(self.inspect(), newer)
        # Current authoring diverges from the definition frozen into the save.
        changed = element(101, 100, 'label', 'New authored default')
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1, ops=[changed]))
        self.revision = 2
        self.assertEqual(self.inspect(), newer)
        self.assertTrue(self.rpc('runtime.inspect', dict(session_id=self.session))['source_stale'])
        authored, history, store = self.world.read_bytes(), self.rpc('world.history'), tree(self.saves)
        params = self.load_params(); missing = dict(params); missing.pop('expected_ui_revision')
        self.rpc('save.load', missing, error=-32602)
        self.rpc('save.load', dict(params, expected_ui_revision=0), error=-32009)
        self.assertEqual(self.inspect(), newer); self.assertEqual(tree(self.saves), store)
        loaded = self.load(params)
        self.assertTrue(loaded['source_stale']); self.assertEqual(loaded['authored_revision'], 1)
        self.assertEqual(self.inspect(), dict(saved, session_id=self.session))
        self.edit([dict(id=uid(101), text='After restore')]); self.step(1); after = self.inspect()
        self.assertEqual(self.rpc('save.load', params), dict(loaded, replayed=True))
        self.assertEqual(self.inspect(), after, 'Load retry replaced a subsequently edited runtime.')
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)
        self.process.close(); self.process = self.open(); self.session = None; self.configure()
        fresh = self.load(self.load_params())
        self.assertTrue(fresh['source_stale']); self.assertEqual(self.inspect(), dict(saved, session_id=self.session))
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(tree(self.saves), store)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RuntimeUi)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    evidence = dict(passed=result.wasSuccessful(), tests=result.testsRun, skipped=len(result.skipped),
        failures=len(result.failures), errors=len(result.errors), calls=CALLS,
        binary_sha256=hashlib.sha256(ARGS.binary.read_bytes()).hexdigest(),
        test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        limitations=['Logical native UI only; no button callbacks, pixels, physical input, renderer, or C# integration.'])
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True, exist_ok=True)
        ARGS.evidence.write_text(json.dumps(evidence, indent=2)+'\n')
    raise SystemExit(0 if result.wasSuccessful() else 1)
