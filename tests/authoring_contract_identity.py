#!/usr/bin/env python3
"""Check released identity through owned standalone and shared-headless hosts.

Read-only and shared-editor request scopes are exercised in world_session_native;
this fixture does not claim desktop presentation or physical-input coverage.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from contextlib import contextmanager
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('engine', type=Path)
parser.add_argument('--windows-interop', action='store_true')
args, unittest_args = parser.parse_known_args()
BINARY = str(args.engine.resolve())

IDENTITY = {
    'id': 'poima.authoring-core', 'version': 1, 'status': 'stable',
    'scope': 'authored-world', 'request_baseline': 'authoring-core-v1.json',
    'response_baseline': 'authoring-core-responses-v1.json',
    'methods': ['entity.get', 'entity.query', 'entity.world_transform', 'world.describe',
                'world.history', 'world.inspect', 'world.redo', 'world.transact', 'world.undo'],
    'components': ['Transform'],
    'mutations': ['component.set', 'entity.create', 'entity.delete', 'entity.rename', 'entity.reparent'],
    'availability': {'mode': 'authoring', 'mutations': True},
}


def native(path):
    path = str(path.resolve())
    if args.windows_interop:
        return subprocess.check_output(['wslpath', '-w', path], text=True, timeout=5).strip()
    return path


class IdentityContract(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build' / 'authoring-contract-identity'
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix='identity-', dir=scratch)
        self.addCleanup(self.directory.cleanup)
        self.world = Path(self.directory.name) / 'world.json'

    @contextmanager
    def standalone(self):
        client = WorldClient.open(BINARY, native(self.world), close_timeout=3)
        with client:
            yield client
        self.assertEqual(client.transport.returncode, 0, 'Standalone owner did not exit cleanly.')

    @contextmanager
    def shared(self):
        # An ephemeral endpoint and a fresh authored path isolate all owners.
        # Files drain startup/error diagnostics without risking a full pipe.
        endpoint = 'identity-' + uuid.uuid4().hex
        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            host = subprocess.Popen([BINARY, 'serve', native(self.world), '--endpoint', endpoint],
                                    stdout=output, stderr=errors)
            client = None
            forced = False
            try:
                client = WorldClient.connect(BINARY, endpoint, timeout_ms=10000, close_timeout=3)
                client.call('world.inspect', timeout=12)  # Also qualifies native connection readiness.
                yield client
            finally:
                original_failure = sys.exc_info()[0] is not None
                cleanup_error = None
                try:
                    if client is not None and not client.closed and host.poll() is None:
                        client.call('host.shutdown', timeout=5)
                except Exception as error:
                    cleanup_error = error
                try:
                    if client is not None:
                        client.close()
                except Exception as error:
                    cleanup_error = cleanup_error or error
                try:
                    host.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    forced = True
                    host.kill()
                    host.wait(timeout=5)
                if not original_failure:
                    self.assertIsNone(cleanup_error, 'Owned shared cleanup failed: ' + str(cleanup_error))
                    self.assertFalse(forced, 'Shared owner required forced cleanup.')
                    self.assertEqual(host.returncode, 0, 'Shared owner did not exit cleanly.')
                    self.assertEqual(client.transport.returncode, 0, 'Shared client did not exit cleanly.')

    def check_views(self, client, scope):
        state, history = client.inspect(), client.history()
        original = self.world.read_bytes() if self.world.exists() else None
        views = [
            {}, {'view': 'full'}, {'view': 'catalog'},
            {'view': 'method', 'name': 'world.inspect'},
            {'view': 'component', 'name': 'Transform'},
            {'view': 'section', 'name': 'invariants'},
            {'view': 'mutation', 'operation': 'component.set', 'type': 'Transform'},
        ]
        for params in views:
            with self.subTest(params=params):
                result = client.call('world.describe', params, timeout=12)
                self.assertGreaterEqual(result['schema_revision'], 49)
                self.assertEqual(result['authoring_contract'], IDENTITY)
                self.assertEqual(result['mode'], IDENTITY['availability']['mode'])
                self.assertFalse(result['read_only'])
                if scope is None:
                    self.assertNotIn('session_scope', result)
                else:
                    self.assertEqual(result['session_scope'], scope)
                if params.get('view') == 'catalog':
                    self.assertNotIn('authoring_contract', result['sections'])
                self.assertEqual(client.inspect(), state)
                self.assertEqual(client.history(), history)
        with self.assertRaises(RpcError) as caught:
            client.call('world.describe', {'view': 'section', 'name': 'authoring_contract'}, timeout=12)
        self.assertEqual(caught.exception.code, -32602)
        self.assertEqual(client.inspect(), state)
        self.assertEqual(client.history(), history)
        self.assertEqual(self.world.read_bytes() if self.world.exists() else None, original)

    def test_standalone_identity_is_consistent_and_observational(self):
        with self.standalone() as client:
            self.check_views(client, None)

    def test_shared_headless_identity_is_consistent_and_observational(self):
        with self.shared() as client:
            self.check_views(client, 'shared_headless')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]] + unittest_args)
