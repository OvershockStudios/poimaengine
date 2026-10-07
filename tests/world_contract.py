"""Exercise persistent authored-world operations through the real native process."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import queue
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=Path)
parser.add_argument('--windows-interop', action='store_true')
args = parser.parse_args()
BINARY = str(args.binary.resolve())
SCRATCH = Path(__file__).resolve().parents[1] / 'build/world-contract'
SCRATCH.mkdir(parents=True, exist_ok=True)


def native(path):
    path = str(path.resolve())
    return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if args.windows_interop else path


def uid(n):
    return f'{n:032x}'


def create(n, parent=None):
    return {'op': 'entity.create', 'id': uid(n), 'name': f'Entity {n}', 'parent': parent}


class Client:
    def __init__(self, path):
        self.process = subprocess.Popen([BINARY, 'world', native(path)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        self.errors = []
        def output():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        def errors():
            self.errors.extend(self.process.stderr)
        threading.Thread(target=output, daemon=True).start()
        threading.Thread(target=errors, daemon=True).start()

    def raw(self, text):
        self.process.stdin.write(text + '\n'); self.process.stdin.flush()
        value = self.lines.get(timeout=20)
        assert value is not None, (self.process.poll(), self.errors)
        return json.loads(value)

    def rpc(self, method, params=None, error=None):
        request_id = str(uuid.uuid4())
        response = self.raw(json.dumps({'jsonrpc': '2.0', 'id': request_id, 'method': method,
                                       'params': {} if params is None else params}))
        assert response['jsonrpc'] == '2.0' and response['id'] == request_id, response
        if error is not None:
            assert response['error']['code'] == error, response
            return response['error']
        assert 'error' not in response, response
        return response['result']

    def txn(self, rev, ops, error=None, **extra):
        return self.rpc('world.transact', {'base_revision': rev, 'ops': ops,
                        'request_id': uuid.uuid4().hex, **extra}, error)

    def close(self):
        if self.process.poll() is None:
            self.rpc('session.close')
            self.process.wait(timeout=10)
        for stream in [self.process.stdin, self.process.stdout, self.process.stderr]:
            stream.close()


class WorldContract(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='world-', dir=SCRATCH)
        self.path = Path(self.directory.name) / '世界 scene.json'
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            try:
                client.close()
            finally:
                if client.process.poll() is None:
                    client.process.kill(); client.process.wait(timeout=10)
        self.directory.cleanup()

    def open(self):
        client = Client(self.path); self.clients.append(client)
        return client

    def test_authoring_preview_atomicity_restart_and_receipts(self):
        client = self.open()
        self.assertFalse(client.rpc('world.inspect')['persisted'])
        descriptor = client.rpc('world.describe')
        self.assertIn('world.transact', descriptor['methods'])
        for method in ('world.capture', 'runtime.capture', 'runtime.play'):
            self.assertEqual(descriptor['methods'][method]['properties']['frames_in_flight'],
                             {'type': 'integer', 'minimum': 1, 'maximum': 2, 'default': 2})
            self.assertEqual(descriptor['methods'][method]['properties']['scene_debug_view']['enum'],
                             ['color', 'depth', 'shading_normal', 'motion', 'motion_validity'])
        original = {'request_id': uuid.uuid4().hex, 'base_revision': 0,
                    'ops': [create(2, uid(1)), {**create(1), 'name': '大厅 🌊'}]}
        client.rpc('world.transact', original)
        self.assertEqual(client.rpc('entity.get', {'id': uid(1)})['value']['name'], '大厅 🌊')
        before = self.path.read_bytes()
        rename = {'op': 'entity.rename', 'id': uid(1), 'name': 'Changed'}
        self.assertFalse(client.txn(1, [rename], preview=True)['committed'])
        self.assertEqual(self.path.read_bytes(), before)
        client.txn(1, [rename, {'op': 'entity.reparent', 'id': uid(1), 'parent': uid(2), 'mode': 'keep_local'}], error=-32602)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(client.rpc('world.inspect')['revision'], 1)
        transform = {'position': [3, 2, -1], 'rotation': [0, 0, 0, 1], 'scale': [1, 2, 3]}
        client.txn(1, [rename, {'op': 'component.set', 'id': uid(2), 'type': 'Transform', 'value': transform}])
        self.assertEqual(Path(str(self.path) + '.previous').read_bytes(), before)
        client.close()
        client = self.open()
        self.assertEqual(client.rpc('entity.get', {'id': uid(2), 'component': 'Transform'})['value'], transform)
        replay = client.rpc('world.transact', original)
        self.assertTrue(replay['replayed']); self.assertEqual(replay['revision'], 1)
        self.assertEqual(client.rpc('world.inspect')['revision'], 2)
        client.rpc('world.transact', {**original, 'ops': [create(3)]}, error=-32010)
        client.txn(1, [create(3)], error=-32009)
        client.txn(2, [rename], typo=True, error=-32602)
        for bad in [{'position': [0, 0, 0], 'rotation': [0, 0, 0, 0], 'scale': [1, 1, 1]},
                    {**transform, 'scale': [1, 0, 1]}, {**transform, 'position': [1e10, 0, 0]}]:
            client.txn(2, [{'op': 'component.set', 'id': uid(2), 'type': 'Transform', 'value': bad}], error=-32602)

    def test_components_hierarchy_matrices_and_capture_validation(self):
        import math
        client = self.open()
        def component(n, kind, value):
            return {'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
        camera = {'vertical_fov': 60, 'near': 0.1, 'far': 1000}
        mesh = {'primitive': 'box', 'visible': True, 'albedo': [0.5, 0.1, 0.2]}
        transform = {'position': [10, 0, 0], 'rotation': [0, 0, math.sqrt(0.5), math.sqrt(0.5)], 'scale': [2, 3, 4]}
        child = {'position': [1, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 2, 1]}
        client.txn(0, [create(1), create(2, uid(1)), create(3), component(1, 'Transform', transform),
                       component(2, 'Transform', child), component(2, 'MeshRenderer', mesh), component(3, 'Camera', camera)])
        expected = [0, 2, 0, 0, -6, 0, 0, 0, 0, 0, 4, 0, 10, 2, 0, 1]
        result = client.rpc('entity.world_transform', {'id': uid(2), 'revision': 1})
        self.assertEqual(result['layout'], 'column_major')
        for actual, wanted in zip(result['matrix'], expected): self.assertAlmostEqual(actual, wanted, places=10)
        for kind, n in [('MeshRenderer', 2), ('Camera', 3)]:
            page = client.rpc('entity.query', {'component': kind, 'revision': 1})
            self.assertEqual([e['id'] for e in page['entities']], [uid(n)])
            self.assertIn(kind, page['entities'][0]['components'])
        self.assertEqual(client.rpc('entity.get', {'id': uid(2), 'component': 'MeshRenderer'})['value'], mesh)
        saved = self.path.read_bytes()
        for kind, value in [('Camera', {**camera, 'far': 0.01}), ('Camera', {**camera, 'vertical_fov': 0}),
                            ('MeshRenderer', {**mesh, 'albedo': [-0.1, 0, 0]}), ('MeshRenderer', {**mesh, 'primitive': 'missing'})]:
            client.txn(1, [component(2, kind, value)], error=-32602)
        capture = {'revision': 1, 'camera': uid(3), 'path': native(Path(self.directory.name) / 'new.bmp')}
        client.rpc('world.capture', {**capture, 'revision': 0}, error=-32009)
        client.rpc('world.capture', {**capture, 'camera': uid(1)}, error=-32004)
        client.rpc('world.capture', {**capture, 'path': native(self.path)}, error=-32602)
        client.rpc('world.capture', {**capture, 'path': native(Path(str(self.path)+'.pending'))}, error=-32602)
        if args.windows_interop:
            client.rpc('world.capture', {**capture, 'path': native(Path(str(self.path)+'.PENDING'))}, error=-32602)
        for extra in [{'width': 127}, {'height': 4097}, {'gpu': -1}, {'samples': 2}]:
            client.rpc('world.capture', {**capture, **extra}, error=-32602)
        for bad in (0, 1, 'true', None, [], {}):
            response = client.rpc('world.capture', {**capture, 'clustered_lighting': bad}, error=-32602)
            self.assertIn('clustered_lighting', response['message'])
            self.assertEqual(self.path.read_bytes(), saved)
            self.assertFalse((Path(self.directory.name) / 'new.bmp').exists())
        history = client.rpc('world.history')
        for bad in (0, 3, -1, True, False, 1.0, 2.0, '2', None, [], {}):
            client.rpc('world.capture', {**capture, 'frames_in_flight': bad}, error=-32602)
            self.assertEqual(self.path.read_bytes(), saved)
            self.assertEqual(client.rpc('world.history'), history)
            self.assertFalse((Path(self.directory.name) / 'new.bmp').exists())
        for bad in (None, True, 1, [], {}, 'normals', ''):
            client.rpc('world.capture', {**capture, 'samples': 1, 'scene_debug_view': bad}, error=-32602)
        for mode in ('depth', 'shading_normal', 'motion', 'motion_validity'):
            client.rpc('world.capture', {**capture, 'scene_debug_view': mode}, error=-32602)
            client.rpc('world.capture', {**capture, 'samples': 4, 'scene_debug_view': mode}, error=-32602)
        for probes in (None, True, {}, [None], [{'x': 0}], [{'x': True, 'y': 0}],
                       [{'x': -1, 'y': 0}], [{'x': 960, 'y': 0}], [{'x': 0, 'y': 540}],
                       [{'x': 0, 'y': 0, 'extra': 1}], [{'x': 0, 'y': 0}]*65):
            client.rpc('world.capture', {**capture, 'samples': 1, 'scene_product_probes': probes}, error=-32602)
        client.rpc('world.capture', {**capture, 'scene_product_probes': [{'x': 0, 'y': 0}]}, error=-32602)
        self.assertEqual(client.rpc('world.history'), history)
        self.assertFalse((Path(self.directory.name) / 'new.bmp').exists())
        capabilities = json.loads(subprocess.check_output([BINARY, 'capabilities'], text=True))['result']['features']
        if not capabilities['scene_capture']: client.rpc('world.capture', capture, error=-32003)
        self.assertEqual(self.path.read_bytes(), saved)
        client.txn(1, [{'op': 'component.remove', 'id': uid(2), 'type': 'Transform'}], error=-32602)
        client.txn(1, [{'op': 'component.remove', 'id': uid(2), 'type': 'MeshRenderer'}])
        client.rpc('entity.get', {'id': uid(2), 'component': 'MeshRenderer'}, error=-32004)
        client.rpc('entity.world_transform', {'id': uid(2), 'revision': 1}, error=-32009)
        client.close(); client = self.open()
        self.assertEqual(client.rpc('entity.get', {'id': uid(3), 'component': 'Camera'})['value'], camera)
        self.assertEqual(client.rpc('entity.query', {'component': 'MeshRenderer'})['entities'], [])
        client.txn(2, [{'op': 'entity.reparent', 'id': uid(3), 'parent': uid(1), 'mode': 'keep_local'}])
        client.rpc('world.capture', {**capture, 'revision': 3}, error=-32602)

    def test_runtime_discovery_and_optional_build(self):
        client = self.open()
        description = client.rpc('world.describe')
        for name in ['runtime.start', 'runtime.step', 'runtime.entity', 'runtime.capture', 'runtime.stop']:
            self.assertIn(name, description['methods'])
        for name in ['BoxCollider', 'CharacterController']:
            self.assertIn(name, description['components'])
        if not description['runtime_available']:
            client.rpc('runtime.start', {'session_id': uid(900), 'revision': 0}, error=-32003)
        self.assertEqual(client.rpc('world.inspect')['revision'], 0)
        self.assertFalse(self.path.exists())

    def test_pagination_recursive_delete_and_identity_retirement(self):
        client = self.open()
        client.txn(0, [create(1)] + [create(n, uid(1)) for n in range(2, 67)])
        seen, after = [], None
        while True:
            params = {'revision': 1, 'limit': 17}
            if after: params['after'] = after
            page = client.rpc('entity.query', params)
            seen += [e['id'] for e in page['entities']]
            after = page['next_after']
            if after is None: break
        self.assertEqual(seen, [uid(n) for n in range(1, 67)])
        roots = client.rpc('entity.query', {'parent': None})['entities']
        self.assertEqual([e['id'] for e in roots], [uid(1)])
        client.rpc('entity.query', {'after': uid(1)}, error=-32602)
        client.txn(1, [{'op': 'entity.delete', 'id': uid(1), 'recursive': False}], error=-32602)
        result = client.txn(1, [{'op': 'entity.delete', 'id': uid(1), 'recursive': True}])
        self.assertEqual(len(result['changed_ids']), 66)
        client.rpc('entity.query', {'revision': 1}, error=-32009)
        client.txn(2, [create(1)], error=-32602)
        client.close(); client = self.open()
        client.txn(2, [create(66)], error=-32602)
        self.assertEqual(client.rpc('world.inspect')['entity_count'], 0)

    def test_storage_failure_external_edit_and_exclusive_writer(self):
        client = self.open(); client.txn(0, [create(1)])
        before = self.path.read_bytes()
        second = subprocess.run([BINARY, 'world', native(self.path)], input='', capture_output=True,
                                text=True, encoding='utf-8', timeout=10)
        self.assertEqual(second.returncode, 4)
        self.assertEqual(json.loads(second.stdout)['status'], 'error')
        pending = Path(str(self.path) + '.pending'); pending.mkdir()
        client.txn(1, [create(2)], error=-32000)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(client.rpc('world.inspect')['revision'], 1)
        pending.rmdir()
        client.txn(1, [create(2)])
        external = self.path.read_text() + '\n'; self.path.write_text(external)
        client.txn(2, [create(3)], error=-32009)
        self.assertEqual(self.path.read_text(), external)

    @unittest.skipIf(args.windows_interop, 'Abrupt Windows process termination is not qualified by the WSL harness')
    def test_crash_releases_lock_and_ignores_unpublished_staging(self):
        client = self.open(); client.txn(0, [create(1)])
        Path(str(self.path) + '.pending').write_text('{incomplete')
        client.process.kill(); client.process.wait(timeout=10)
        client = self.open()
        self.assertEqual(client.rpc('world.inspect')['revision'], 1)
        client.txn(1, [create(2)])
        self.assertEqual(client.rpc('world.inspect')['entity_count'], 2)

    def test_protocol_errors_and_notification(self):
        client = self.open()
        for text, code in [('{bad', -32700), ('[]', -32600),
                           ('{"jsonrpc":"2.0","id":1,"method":"world.inspect","params":{"x":1,"x":2}}', -32700),
                           ('[' * 70 + ']' * 70, -32700), (' ' * 1048577, -32700)]:
            self.assertEqual(client.raw(text)['error']['code'], code)
        client.rpc('not.a.method', error=-32601)
        client.rpc('world.inspect', [], error=-32602)
        client.process.stdin.write('{"jsonrpc":"2.0","method":"world.inspect"}\n'); client.process.stdin.flush()
        self.assertEqual(client.rpc('world.inspect')['entity_count'], 0)
        self.assertFalse(self.path.exists())

    def test_corrupt_primary_preserves_previous_snapshot(self):
        client = self.open(); client.txn(0, [create(1)]); client.txn(1, [create(2)]); client.close()
        previous = Path(str(self.path) + '.previous').read_bytes()
        self.path.write_text('{corrupt')
        process = subprocess.run([BINARY, 'world', native(self.path)], input='', capture_output=True, text=True, timeout=10)
        self.assertEqual(process.returncode, 4)
        self.assertEqual(self.path.read_text(), '{corrupt')
        self.assertEqual(Path(str(self.path) + '.previous').read_bytes(), previous)
        self.path.write_bytes(previous)
        client = self.open(); self.assertEqual(client.rpc('world.inspect')['revision'], 1)

    def test_receipt_eviction_preserves_stale_revision_guard(self):
        client = self.open()
        request = {'request_id': uuid.uuid4().hex, 'base_revision': 0, 'ops': [create(1)]}
        client.rpc('world.transact', request)
        for rev in range(1, 130):
            client.txn(rev, [{'op': 'entity.rename', 'id': uid(1), 'name': f'Revision {rev}'}])
        self.assertEqual(len(json.loads(self.path.read_text())['receipts']), 128)
        client.rpc('world.transact', request, error=-32009)
        self.assertEqual(client.rpc('world.inspect')['revision'], 130)


if __name__ == '__main__':
    unittest.main(argv=['world_contract'], verbosity=2)
