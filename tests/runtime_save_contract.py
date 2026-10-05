#!/usr/bin/env python3
"""Qualify WorldSession save slots through real RPC, assets and native physics."""
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
import wave
from gltf_fixture import glb
from texture_fixture import png, quad

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--runtime', choices=[0, 1], type=int, default=1)
parser.add_argument('--windows-interop', action='store_true')
parser.add_argument('--evidence', type=Path)
ARGS = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / 'build/runtime-save-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)
CALLS, MEASUREMENTS = [], []


def uid(value):
    return f'{value:032x}'


def native(path):
    path = str(path.resolve())
    return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if ARGS.windows_interop else path


def tree(path):
    return {file.relative_to(path).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in sorted(path.rglob('*')) if file.is_file()}


def component(identity, kind, value):
    return dict(op='component.set', id=uid(identity), type=kind, value=value)


def transform(identity, position, scale=(1, 1, 1)):
    return component(identity, 'Transform', dict(position=list(position), rotation=[0, 0, 0, 1], scale=list(scale)))


def without_session(value):
    if isinstance(value, dict):
        return {key: without_session(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [without_session(item) for item in value]
    return value


class Session:
    def __init__(self, world, test):
        self.world, self.test = world, test
        self.errors = world.with_suffix('.stderr').open('w')
        self.process = subprocess.Popen([str(ARGS.binary.resolve()), 'world', native(world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors,
            text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        def reader():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=reader, daemon=True).start()

    def rpc(self, method, params=None, error=None):
        request = dict(jsonrpc='2.0', id=len(CALLS)+1, method=method, params={} if params is None else params)
        self.process.stdin.write(json.dumps(request)+'\n'); self.process.stdin.flush()
        line = self.lines.get(timeout=60)
        assert line is not None, self.world.with_suffix('.stderr').read_text()
        reply = json.loads(line)
        CALLS.append(dict(test=self.test, request=request, response=reply))
        assert reply.get('id') == request['id'], reply
        if error is not None:
            assert reply.get('error', {}).get('code') == error, reply
            return reply['error']
        assert 'result' in reply, reply
        return reply['result']

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()
                raise
        self.errors.close()
        assert self.process.returncode == 0, self.world.with_suffix('.stderr').read_text()


class RuntimeSaves(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.world = self.directory / 'authored world.json'
        self.saves = self.directory / 'save root'; self.saves.mkdir()
        self.sessions = []
        self.addCleanup(self.close_sessions)
        self.process = self.open()
        self.configuration = 0
        self.session, self.tick, self.revision = None, 0, 0

    def close_sessions(self):
        for process in self.sessions:
            if not process.errors.closed:
                process.close()

    def open(self):
        process = Session(self.world, self.id()); self.sessions.append(process)
        return process

    def rpc(self, *args, **kwargs):
        return self.process.rpc(*args, **kwargs)

    def configure(self):
        params = dict(request_id=uuid.uuid4().hex, expected_generation=self.configuration, root=native(self.saves))
        result = self.rpc('save.configure', params)
        self.configuration = result['generation']
        return params, result

    def author(self, assets=False):
        fixture = json.loads((ROOT / 'examples/interaction-room.jsonl').read_text())
        ops = fixture['params']['ops']
        for op in ops:
            if op.get('type') == 'Transform' and op['id'] == uid(3):
                op['value']['position'] = [7, 200, 0]
        ops += [dict(op='entity.create', id=uid(400), name='Door handle', parent=uid(2)), transform(400, [.4, 0, 0])]
        packages = []
        if assets:
            model = self.directory / 'source.glb'
            doc, blob = quad([], material={'pbrMetallicRoughness': {'baseColorFactor': [.2, .5, .8, 1]}})
            model.write_bytes(glb(doc, blob))
            model_id = self.rpc('asset.import', dict(source=native(model)))['asset']
            image = self.directory / 'source.png'; image.write_bytes(png(1, 1, [64, 128, 192, 255]))
            image_id = self.rpc('asset.image.import', dict(source=native(image), color_space='srgb'))['asset']
            sound = self.directory / 'source.wav'
            with wave.open(str(sound), 'wb') as stream:
                stream.setparams((1, 2, 48000, 4800, 'NONE', 'not compressed')); stream.writeframes(b'\x00\x01'*4800)
            sound_id = self.rpc('asset.audio.import', dict(source=native(sound)))['asset']
            ops += [dict(op='entity.create', id=uid(500), name='Hidden saved visual'), transform(500, [15, 2, 0]),
                component(500, 'StaticMesh', dict(asset=model_id, primitive=0, visible=False)),
                component(500, 'PbrTextures', dict(base_color={'asset': image_id})),
                dict(op='entity.create', id=uid(501), name='Collision-only saved geometry'), transform(501, [18, 2, 0]),
                component(501, 'MeshCollider', dict(asset=model_id, primitive=0, friction=.5, restitution=0)),
                dict(op='entity.create', id=uid(502), name='Disabled saved sound'),
                component(502, 'AudioEmitter', dict(asset=sound_id, gain=.5, loop=True, enabled=False))]
            packages = [Path(str(self.world)+'.assets')/(identity+extension) for identity, extension in
                        [(model_id, '.pmodel'), (image_id, '.pimage'), (sound_id, '.paudio')]]
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
        self.revision = 1
        return packages

    def start(self):
        self.session, self.tick = uuid.uuid4().hex, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=self.revision))

    def step(self, ticks, inputs=(), motions=()):
        result = self.rpc('runtime.step', dict(request_id=uuid.uuid4().hex, session_id=self.session,
            expected_tick=self.tick, ticks=ticks, inputs=list(inputs), motions=list(motions)))
        self.tick += ticks
        self.assertEqual(result['tick'], self.tick)

    def state(self):
        return dict(entities=[self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=uid(identity)))
                              for identity in (2, 3, 100, 101, 400)],
                    gameplay=self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True)),
                    voices=self.rpc('runtime.audio.voices', dict(session_id=self.session, tick=self.tick)))

    def write_params(self, slot='checkpoint', generation=0):
        return dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration, slot=slot,
                    expected_generation=generation, session_id=self.session, expected_tick=self.tick,
                    expected_gameplay_revision=0)

    def load_params(self, slot='checkpoint', generation=1):
        return dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration, slot=slot,
                    expected_generation=generation, revision=self.revision, expected_session_id=self.session,
                    expected_tick=self.tick if self.session else None,
                    expected_gameplay_revision=0 if self.session else None, new_session_id=uuid.uuid4().hex)

    def loaded(self, params):
        result = self.rpc('save.load', params)
        self.session, self.tick = params['new_session_id'], result['tick']
        return result

    def mid_motion(self):
        self.step(120)
        self.step(150, [dict(entity=uid(100), move=[0, 1])])
        self.step(30, [dict(entity=uid(100), look=[25, -12])],
                  [dict(entity=uid(2), position=[3, 1.5, -3], rotation=[0, 0, 0, 1], duration_ticks=120)])
        state = self.state()
        self.assertAlmostEqual(state['entities'][0]['world_matrix'][12], .75, delta=1e-5)
        self.assertEqual(state['entities'][0]['motion_remaining_ticks'], 90)
        self.assertLess(state['entities'][1]['velocity'][1], -10)
        return state

    def test_configuration_discovery_validation_and_receipts(self):
        methods = self.rpc('world.describe')['methods']
        self.assertTrue({'save.configure', 'save.inspect', 'save.write', 'save.load'} <= set(methods))
        params, configured = self.configure()
        self.assertEqual(configured['generation'], 1)
        self.assertEqual(configured['root'], native(self.saves))
        self.assertFalse(configured['replayed'])
        self.assertEqual(self.rpc('save.configure', params), dict(configured, replayed=True))
        self.rpc('save.configure', dict(params, root=None), error=-32010)
        self.rpc('save.configure', dict(params, request_id=uuid.uuid4().hex), error=-32009)
        before = tree(self.saves)
        for slot in ('', 'Upper', '../outside', 'a/b', 'a'*65):
            self.rpc('save.inspect', dict(slot=slot), error=-32602)
        missing = self.rpc('save.inspect', dict(slot='missing'))
        self.assertFalse(missing['exists']); self.assertEqual(missing['generation'], 0)
        self.assertIsNone(missing['selected']); self.assertEqual(missing['configuration_generation'], 1)
        self.assertEqual(tree(self.saves), before)
        self.assertFalse(self.world.exists(), 'Save configuration unexpectedly authored a world.')

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_midmotion_save_load_guards_retries_and_authoring_preservation(self):
        self.author(); self.configure(); self.start()
        saved = self.mid_motion(); authored, history = self.world.read_bytes(), self.rpc('world.history')
        params = self.write_params()
        before = self.state()
        for update in [dict(expected_tick=self.tick-1), dict(expected_gameplay_revision=1),
                       dict(configuration_generation=0), dict(session_id=uid(999))]:
            self.rpc('save.write', dict(params, **update), error=-32009)
            self.assertEqual(self.state(), before)
        result = self.rpc('save.write', params)
        self.assertEqual(result['generation'], 1); self.assertFalse(result['replayed'])
        self.assertGreater(result['bytes'], 0); self.assertEqual(len(result['sha256']), 64)
        status = self.rpc('save.inspect', dict(slot='checkpoint'))
        self.assertTrue(status['exists']); self.assertEqual(status['generation'], 1)
        self.assertEqual(status['selected']['sha256'], result['sha256']); self.assertTrue(status['selected']['verified'])
        files = tree(self.saves)
        self.step(7)
        advanced = self.state()
        self.assertEqual(self.rpc('save.write', params), dict(result, replayed=True))
        self.assertEqual(self.rpc('save.write', dict(params, acknowledge_recovery=False)), dict(result, replayed=True))
        self.assertEqual(self.state(), advanced); self.assertEqual(tree(self.saves), files)
        self.rpc('save.write', dict(params, expected_tick=self.tick), error=-32010)
        good_load = self.load_params()
        for update in [dict(expected_tick=self.tick-1), dict(expected_session_id=uid(998)),
                       dict(expected_gameplay_revision=1), dict(revision=0), dict(expected_generation=0),
                       dict(configuration_generation=0)]:
            self.rpc('save.load', dict(good_load, **update), error=-32009)
            self.assertEqual(self.state(), advanced)
        loaded = self.loaded(good_load)
        self.assertEqual(self.tick, 300); self.assertEqual(loaded['authored_revision'], 1)
        restored = self.state()
        self.assertEqual(without_session(saved), without_session(restored))
        self.step(5); after = self.state()
        self.assertEqual(self.rpc('save.load', good_load), dict(loaded, replayed=True))
        self.assertEqual(self.rpc('save.load', dict(good_load, allow_recovery=False)), dict(loaded, replayed=True))
        self.assertEqual(self.state(), after)
        self.rpc('save.load', dict(good_load, new_session_id=uuid.uuid4().hex), error=-32010)
        self.step(85)
        door = self.state()['entities'][0]
        self.assertAlmostEqual(door['world_matrix'][12], 3, delta=1e-5)
        self.assertEqual(door['motion_remaining_ticks'], 0); self.assertIsNone(door['kinematic_target'])
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)
        self.assertEqual(tree(self.saves), files)
        MEASUREMENTS.append(dict(test=self.id(), saved_tick=300, final_tick=self.tick,
                                 door_endpoint=door['world_matrix'][12], sha256=result['sha256']))

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_fresh_process_restore_and_durable_write_retry(self):
        self.author(); self.configure(); self.configure(); self.start()
        self.assertEqual(self.configuration, 2)
        saved = self.mid_motion(); params = self.write_params('portable')
        written = self.rpc('save.write', params); files = tree(self.saves)
        authored = self.world.read_bytes()
        self.process.close(); self.process = self.open(); self.configuration = 0
        self.session, self.tick = None, 0
        self.configure()
        self.assertEqual(self.configuration, 1)
        # The root routing generation is session-local. Refresh only that guard
        # after reconnecting; the persisted write's semantic identity is unchanged.
        self.rpc('save.write', params, error=-32009)
        self.assertFalse(self.rpc('runtime.status')['active'])
        self.assertEqual(tree(self.saves), files)
        refreshed = dict(params, configuration_generation=self.configuration)
        replayed = self.rpc('save.write', refreshed)
        self.assertEqual(replayed, dict(written, replayed=True,
                                       configuration_generation=self.configuration))
        self.assertFalse(self.rpc('runtime.status')['active']); self.assertEqual(tree(self.saves), files)
        restored = self.loaded(self.load_params('portable'))
        self.assertEqual(restored['tick'], 300)
        self.assertEqual(without_session(self.state()), without_session(saved))
        self.step(90)
        self.assertAlmostEqual(self.state()['entities'][0]['world_matrix'][12], 3, delta=1e-5)
        self.assertEqual(self.world.read_bytes(), authored)

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_wrong_world_and_missing_slot_preserve_existing_runtime(self):
        self.author(); self.configure(); self.start(); self.step(12)
        self.rpc('save.write', self.write_params('original'))
        original_bytes = self.world.read_bytes()
        foreign_world = self.directory / 'different world.json'
        document = json.loads(original_bytes); document['world_id'] = uuid.uuid4().hex
        foreign_world.write_text(json.dumps(document))
        foreign_bytes = foreign_world.read_bytes()
        self.process.close()
        self.process = Session(foreign_world, self.id()); self.sessions.append(self.process)
        self.configuration = 0; self.configure(); self.start(); self.step(3)
        before = self.state()
        self.rpc('save.load', self.load_params('original'), error=-32070)
        self.assertEqual(self.state(), before)
        self.rpc('save.load', self.load_params('missing', generation=0), error=-32070)
        self.assertEqual(self.state(), before)
        self.assertEqual(self.world.read_bytes(), original_bytes)
        self.assertEqual(foreign_world.read_bytes(), foreign_bytes)

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_saved_frozen_world_restores_without_overwriting_new_authoring(self):
        self.author(); self.configure(); self.start(); saved = self.mid_motion()
        self.rpc('save.write', self.write_params())
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1,
                                       ops=[transform(2, [50, 1.5, -3], [2, 3, .4])]))
        self.revision = 2; authored, history = self.world.read_bytes(), self.rpc('world.history')
        self.step(6)
        loaded = self.loaded(self.load_params())
        self.assertEqual(loaded['authored_revision'], 1); self.assertEqual(loaded['current_authored_revision'], 2)
        self.assertEqual(without_session(self.state()), without_session(saved))
        self.assertTrue(self.rpc('runtime.inspect', dict(session_id=self.session))['source_stale'])
        self.assertEqual(self.rpc('entity.get', dict(id=uid(2), component='Transform'))['value']['position'][0], 50)
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_explicit_payload_recovery_quarantine_and_manifest_read_only_recovery(self):
        self.author(); self.configure(); self.start(); self.step(5)
        prior = self.state(); first = self.rpc('save.write', self.write_params('recovery'))
        self.step(10)
        second = self.rpc('save.write', self.write_params('recovery', generation=1))
        self.assertEqual((first['generation'], second['generation']), (1, 2))
        self.step(3)
        live = self.state(); authored, history = self.world.read_bytes(), self.rpc('world.history')
        slot = self.saves / 'slot-recovery'
        manifest = slot / 'current.json'
        newest_name = json.loads(manifest.read_text())['payload']['current']['file']
        newest = slot / newest_name
        self.assertEqual(newest.resolve().parent, slot.resolve())
        self.assertTrue(newest_name.startswith('p-') and newest_name.endswith('.bin'))
        damaged = b'Intentional corruption of this test-owned newest checkpoint.'
        newest.write_bytes(damaged)
        corrupt_tree = tree(self.saves)
        status = self.rpc('save.inspect', dict(slot='recovery'))
        self.assertTrue(status['recovered']); self.assertFalse(status['manifest_recovered'])
        self.assertEqual(status['generation'], 2)
        self.assertEqual(status['selected']['generation'], 1)
        self.assertTrue(status['selected']['verified']); self.assertFalse(status['current']['verified'])
        self.assertEqual(status['selected']['sha256'], first['sha256'])
        self.assertTrue(status['recovery_reason'])
        load = self.load_params('recovery', generation=2)
        self.rpc('save.load', load, error=-32070)
        self.assertEqual(self.state(), live); self.assertEqual(tree(self.saves), corrupt_tree)
        load['allow_recovery'] = True
        restored = self.loaded(load)
        self.assertTrue(restored['recovered']); self.assertEqual(restored['selected_generation'], 1)
        self.assertEqual(self.tick, 5); self.assertEqual(without_session(self.state()), without_session(prior))
        self.step(3); continued = self.state()
        self.assertEqual(self.rpc('save.load', load), dict(restored, replayed=True))
        self.assertEqual(self.state(), continued); self.assertEqual(tree(self.saves), corrupt_tree)
        replacement = self.write_params('recovery', generation=2)
        self.rpc('save.write', replacement, error=-32070)
        self.assertEqual(self.state(), continued); self.assertEqual(tree(self.saves), corrupt_tree)
        replacement['acknowledge_recovery'] = True
        committed = self.rpc('save.write', replacement)
        self.assertEqual(committed['generation'], 3); self.assertFalse(committed['replayed'])
        self.assertEqual(newest.read_bytes(), damaged, 'Acknowledged recovery removed or changed quarantined evidence.')
        healthy = self.rpc('save.inspect', dict(slot='recovery'))
        self.assertEqual(healthy['selected']['generation'], 3); self.assertFalse(healthy['recovered'])
        self.assertEqual(healthy['previous']['generation'], 1)
        self.assertEqual(healthy['quarantined']['generation'], 2); self.assertFalse(healthy['quarantined']['verified'])
        committed_tree = tree(self.saves)
        self.assertEqual(self.rpc('save.write', replacement), dict(committed, replayed=True))
        # The retained load receipt survives both a live tick change and a new
        # disk generation; retry cannot silently load the older state again.
        self.assertEqual(self.rpc('save.load', load), dict(restored, replayed=True))
        self.assertEqual(self.state(), continued); self.assertEqual(tree(self.saves), committed_tree)

        # A corrupt manifest has potentially lost receipt history. Read recovery
        # remains explicit, but even acknowledgement cannot authorize a write.
        manifest.write_bytes(b'Intentional corruption of this test-owned current manifest.')
        manifest_tree = tree(self.saves)
        fallback = self.rpc('save.inspect', dict(slot='recovery'))
        self.assertTrue(fallback['manifest_recovered']); self.assertTrue(fallback['recovered'])
        self.assertEqual(fallback['selected']['generation'], 1)
        for acknowledged in (False, True):
            params = self.write_params('recovery', generation=fallback['generation'])
            params['acknowledge_recovery'] = acknowledged
            self.rpc('save.write', params, error=-32070)
            self.assertEqual(self.state(), continued); self.assertEqual(tree(self.saves), manifest_tree)
        recovered_load = self.load_params('recovery', generation=fallback['generation'])
        self.rpc('save.load', recovered_load, error=-32070)
        self.assertEqual(self.state(), continued)
        recovered_load['allow_recovery'] = True
        self.loaded(recovered_load)
        self.assertEqual(without_session(self.state()), without_session(prior))
        self.assertEqual(tree(self.saves), manifest_tree)
        self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)
        MEASUREMENTS.append(dict(test=self.id(), newest_generation=2, recovered_generation=1,
                                 acknowledged_generation=3, quarantine_preserved=True,
                                 manifest_recovery_write_refused=True))

    @unittest.skipUnless(ARGS.runtime, 'Requires native simulation.')
    def test_fresh_dependency_validation_for_hidden_disabled_and_collision_only_assets(self):
        packages = self.author(assets=True); self.configure(); self.start(); self.step(5)
        saved = self.state(); self.rpc('save.write', self.write_params('assets'))
        # Keep the current references for this phase so model caches remain
        # warm: a successful old import cannot bypass a fresh disk check.
        authored, history, store = self.world.read_bytes(), self.rpc('world.history'), tree(self.saves)
        for path in packages:
            original = path.read_bytes()
            try:
                for corrupt in (False, True):
                    if corrupt:
                        path.write_bytes(b'corrupt referenced package')
                    else:
                        path.unlink()
                    before = self.state()
                    self.rpc('save.load', self.load_params('assets'), error=-32070)
                    self.assertEqual(self.state(), before)
                    self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)
                    self.assertEqual(tree(self.saves), store)
                    self.step(1)  # Existing cached runtime remains independently usable.
                    path.write_bytes(original)
            finally:
                path.write_bytes(original)
        # Remove references from current authoring: restoration must validate the
        # stored frozen document's closure, not merely today's authored world.
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1,
            ops=[dict(op='entity.delete', id=uid(i), recursive=True) for i in (500, 501, 502)]))
        self.revision = 2
        authored, history, store = self.world.read_bytes(), self.rpc('world.history'), tree(self.saves)
        for path in packages:
            original = path.read_bytes()
            try:
                path.unlink(); before = self.state()
                self.rpc('save.load', self.load_params('assets'), error=-32070)
                self.assertEqual(self.state(), before)
                self.assertEqual(self.world.read_bytes(), authored); self.assertEqual(self.rpc('world.history'), history)
                self.assertEqual(tree(self.saves), store)
            finally:
                path.write_bytes(original)
        self.loaded(self.load_params('assets'))
        self.assertEqual(without_session(self.state()), without_session(saved))
        self.assertEqual(self.world.read_bytes(), authored)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RuntimeSaves)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    evidence = dict(passed=result.wasSuccessful(), tests=result.testsRun,
        skipped=len(result.skipped), failures=len(result.failures), errors=len(result.errors),
        binary_sha256=hashlib.sha256(ARGS.binary.read_bytes()).hexdigest(),
        test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        calls=CALLS, measurements=MEASUREMENTS,
        limitations=['RPC integration and explicit external save-root tests; storage crash durability and managed restoration have separate suites.'])
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True, exist_ok=True)
        ARGS.evidence.write_text(json.dumps(evidence, indent=2)+'\n')
    raise SystemExit(0 if result.wasSuccessful() else 1)
