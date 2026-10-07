#!/usr/bin/env python3
"""Real stdio MCP authoring/persistence checks; no socket or GPU qualification."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--windows-interop', action='store_true')
args = parser.parse_args()
BINARY = str(args.binary.resolve())
ROOT = Path(__file__).resolve().parents[1] / 'build/mcp-contract'
ROOT.mkdir(parents=True, exist_ok=True)


def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())


class Client:
    def __init__(self, path):
        self.process = subprocess.Popen([BINARY, 'mcp', '--world', native(path)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        self.errors = []
        def read():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        def errors():
            self.errors.extend(self.process.stderr)
        self.threads = [threading.Thread(target=read, daemon=True), threading.Thread(target=errors, daemon=True)]
        for thread in self.threads:
            thread.start()

    def send(self, request):
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()

    def rpc(self, method, params=None, error=None):
        identity = uuid.uuid4().hex
        self.send({'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': {} if params is None else params})
        line = self.lines.get(timeout=30)
        assert line is not None, self.errors
        response = json.loads(line)
        assert response.get('id') == identity and response.get('jsonrpc') == '2.0', response
        if error is not None:
            assert response['error']['code'] == error, response
            return response['error']
        assert 'result' in response, response
        return response['result']

    def initialize(self):
        result = self.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                        'clientInfo': {'name': 'poima-contract', 'version': '1'}})
        assert result['protocolVersion'] == '2025-11-25' and 'tools' in result['capabilities'], result
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.rpc('ping')  # Barrier also catches an illicit notification response.

    def tool(self, name, arguments=None, error=None):
        result = self.rpc('tools/call', {'name': name, 'arguments': arguments or {}})
        structured = result['structuredContent']
        assert json.loads(result['content'][0]['text']) == structured, result
        if error is None:
            assert not result.get('isError', False) and 'result' in structured, result
            return structured['result']
        assert result['isError'] and structured['error']['code'] == error, result
        return structured['error']

    def world(self, method, params=None, error=None):
        return self.tool('poima_call', {'method': method, 'params': params or {}}, error)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=20)
        for thread in self.threads:
            thread.join(timeout=5)
        assert self.process.returncode == 0, self.errors
        remaining = []
        while not self.lines.empty():
            remaining.append(self.lines.get_nowait())
        assert all(line is None for line in remaining), remaining
        self.process.stdout.close()
        self.process.stderr.close()


class McpContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.path = Path(self.temp.name) / 'world.json'
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            if client.process.poll() is None:
                client.close()
        self.temp.cleanup()

    def client(self):
        client = Client(self.path)
        self.clients.append(client)
        client.initialize()
        return client

    def test_authoring_receipts_conflicts_undo_and_eof_persistence(self):
        client = self.client()
        tools = client.rpc('tools/list')['tools']
        self.assertEqual({tool['name'] for tool in tools}, {'poima_discover', 'poima_call'})
        catalog = client.tool('poima_discover')
        self.assertEqual(catalog['view'], 'catalog')
        self.assertTrue(catalog['partial'])
        schema = client.tool('poima_discover', {'view': 'full'})
        self.assertEqual(sorted(schema['methods']), catalog['methods'])
        self.assertEqual(client.world('world.inspect')['revision'], 0)
        entity = uuid.uuid4().hex
        transaction = {'request_id': uuid.uuid4().hex, 'base_revision': 0,
                       'ops': [{'op': 'entity.create', 'id': entity, 'name': 'MCP durable entity'}]}
        first = client.world('world.transact', transaction)
        self.assertEqual(first['revision'], 1)
        retry = client.world('world.transact', transaction)
        self.assertTrue(retry['replayed'])
        self.assertEqual(retry['revision'], 1)
        client.world('world.transact', {**transaction, 'request_id': uuid.uuid4().hex}, error=-32009)
        query = client.world('entity.query', {'revision': 1})
        self.assertEqual([item['id'] for item in query['entities']], [entity])
        undo = client.world('world.undo', {'request_id': uuid.uuid4().hex, 'base_revision': 1})
        self.assertEqual(undo['revision'], 2)
        self.assertEqual(client.world('entity.query')['entities'], [])
        client.world('world.redo', {'request_id': uuid.uuid4().hex, 'base_revision': 2})
        client.close()
        # A different public transport opens the persisted document after EOF.
        requests = [{'jsonrpc': '2.0', 'id': 1, 'method': 'entity.get', 'params': {'id': entity}},
                    {'jsonrpc': '2.0', 'id': 2, 'method': 'world.inspect', 'params': {}}]
        direct = subprocess.run([BINARY, 'world', native(self.path)], input=''.join(json.dumps(row)+'\n' for row in requests),
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(direct.returncode, 0, direct.stderr)
        replies = [json.loads(line) for line in direct.stdout.splitlines()]
        self.assertEqual(len(replies), 2)
        self.assertEqual(replies[0]['result']['value']['name'], 'MCP durable entity')
        self.assertEqual(replies[1]['result']['revision'], 3)

    def test_writer_exclusion_and_startup_stdout_cleanliness(self):
        client = self.client()
        client.world('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': 0,
                     'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex, 'name': 'Held writer'}]})
        frozen = self.path.read_bytes()
        second = subprocess.run([BINARY, 'mcp', '--world', native(self.path)], input='', capture_output=True, text=True, timeout=30)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(second.stdout, '')
        self.assertTrue(second.stderr)
        self.assertEqual(self.path.read_bytes(), frozen)
        self.assertEqual(client.world('world.inspect')['revision'], 1)

    def test_oversized_frame_is_drained_and_eof_frame_is_processed(self):
        ping = json.dumps({'jsonrpc': '2.0', 'id': 'after-large-frame', 'method': 'ping'})
        # Deliberately omit the final newline: EOF is also a frame boundary.
        result = subprocess.run([BINARY, 'mcp', '--world', native(self.path)],
                                input=' ' * (1024*1024 + 1) + '\n' + ping,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]['error']['code'], -32700)
        self.assertEqual(rows[1], {'jsonrpc': '2.0', 'id': 'after-large-frame', 'result': {}})
        self.assertFalse(self.path.exists())

    def test_notifications_cannot_mutate_and_invalid_tool_arguments(self):
        client = self.client()
        transaction = {'request_id': uuid.uuid4().hex, 'base_revision': 0,
                       'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex, 'name': 'Forbidden notification'}]}
        client.send({'jsonrpc': '2.0', 'method': 'tools/call', 'params': {'name': 'poima_call',
                     'arguments': {'method': 'world.transact', 'params': transaction}}})
        client.rpc('ping')
        self.assertEqual(client.world('world.inspect')['revision'], 0)
        self.assertFalse(self.path.exists())
        client.tool('poima_call', {'method': 1}, error=-32602)
        client.tool('poima_discover', {'view': 'method', 'name': 'missing.method'}, error=-32602)
        client.rpc('tools/call', {'name': 'unknown_tool', 'arguments': {}}, error=-32602)
        client.rpc('missing/method', error=-32601)


if __name__ == '__main__':
    unittest.main(argv=['mcp_contract'])
