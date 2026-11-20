#!/usr/bin/env python3
"""Real stdio MCP authoring/persistence and shared-host checks; no GPU qualification."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import unittest
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--windows-interop', action='store_true')
args = parser.parse_args()
BINARY = str(args.binary.resolve())
# Use the native Linux temporary filesystem for atomic publication. Windows
# interop needs a scratch path shared between WSL and the Windows subprocess.
ROOT = Path(__file__).resolve().parents[1] / 'build/mcp-contract' if args.windows_interop or os.name == 'nt' else None
if ROOT is not None:
    ROOT.mkdir(parents=True, exist_ok=True)


def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())


class Client:
    def __init__(self, path=None, endpoint=None, *, project=None, cwd=None, profiles=None):
        if endpoint:
            target = ['--endpoint', endpoint]
        elif project:
            target = ['--project', native(project)]
        else:
            target = ['--world', native(path)]
        if profiles:
            target.extend(['--development-profiles', native(profiles)])
        self.process = subprocess.Popen([BINARY, 'mcp', *target], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        encoding='utf-8', bufsize=1, cwd=cwd)
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
        self.temp = tempfile.TemporaryDirectory(prefix='poima-mcp-', dir=ROOT)
        self.path = Path(self.temp.name) / 'world.json'
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            if client.process.poll() is None:
                client.close()
        self.temp.cleanup()

    def client(self, *, cwd=None):
        client = Client(self.path, cwd=cwd)
        self.clients.append(client)
        client.initialize()
        return client

    def create_project(self):
        root = Path(self.temp.name) / 'My project'
        created = subprocess.run([BINARY, 'project', 'create', native(root), '--name', 'MCP project'],
                                 capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        manifest = root / Path(json.loads(created.stdout)['result']['manifest']).name
        spec = json.loads(manifest.read_bytes())
        levels = root / 'Levels'
        levels.mkdir()
        world = levels / 'room.json'
        (root / spec['entry']['world']).rename(world)
        spec['entry']['world'] = world.relative_to(root).as_posix()
        named = manifest.rename(root / 'Game Lab.poimaproject')
        named.write_text(json.dumps(spec), encoding='utf-8')
        return named, world

    def test_project_mcp_edits_entry_world_and_reopens_legacy_manifest(self):
        manifest, world = self.create_project()
        source = manifest.read_bytes()
        revision = json.loads(world.read_bytes())['revision']
        client = Client(project=manifest, cwd=self.temp.name)
        self.clients.append(client)
        client.initialize()
        self.assertEqual(client.world('world.inspect')['revision'], revision)
        transaction = {'request_id': uuid.uuid4().hex, 'base_revision': revision,
                       'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex, 'name': 'Via project'}]}
        self.assertEqual(client.world('world.transact', transaction)['revision'], revision + 1)
        self.assertTrue(client.world('world.transact', transaction)['replayed'])
        refused = subprocess.run([BINARY, 'mcp', '--project', native(manifest)], input='',
                                 capture_output=True, text=True, timeout=30)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(refused.stdout, '')
        self.assertTrue(refused.stderr)
        client.close()
        self.assertEqual(manifest.read_bytes(), source)
        self.assertEqual(json.loads(world.read_bytes())['revision'], revision + 1)
        self.assertFalse((Path(self.temp.name) / world.name).exists())

        legacy = manifest.rename(manifest.with_name('project.json'))
        client = Client(project=legacy)
        self.clients.append(client)
        client.initialize()
        self.assertEqual(client.world('world.inspect')['revision'], revision + 1)
        client.close()
        request = {'jsonrpc': '2.0', 'id': 1, 'method': 'world.inspect'}
        direct = subprocess.run([BINARY, 'world', '--project', native(legacy)],
                                input=json.dumps(request)+'\n', capture_output=True, text=True, timeout=30)
        self.assertEqual(direct.returncode, 0, direct.stdout + direct.stderr)
        self.assertEqual(json.loads(direct.stdout)['result']['revision'], revision + 1)

    def test_project_files_are_scoped_and_guarded(self):
        root = Path(self.temp.name)
        projects = root/'projects'; projects.mkdir()
        products = root/'products'; products.mkdir()
        runtime = root/'runtime'; runtime.mkdir()
        (runtime/'runtime.json').write_text('{}')
        for name in ('Main', 'Other'):
            subprocess.run([BINARY, 'project', 'create', native(projects/name), '--name', name, '--gameplay'],
                           check=True, capture_output=True, timeout=30)
        manifest = projects/'Main/project.poimaproject'
        original_manifest = manifest.read_bytes()
        original_world = (manifest.parent/'world.json').read_bytes()
        path = manifest.parent/'gameplay-src/Game.cs'
        fixture_tool = native(Path(BINARY))  # Only pinned, never executed as a compiler.
        entry = dict(name='local', project_root=native(projects), output_root=native(products),
                     python=fixture_tool, publisher=fixture_tool, dotnet=fixture_tool,
                     exporter=fixture_tool, runtime_root=native(runtime), environment=[],
                     target_rid='win-x64' if args.windows_interop or os.name == 'nt' else 'linux-x64')
        profiles = root/'profiles.json'
        profiles.write_text(json.dumps(dict(format='poima.development-profiles', version=1, profiles=[entry])))
        client = Client(project=manifest, profiles=profiles)
        self.clients.append(client); client.initialize()
        configured = client.world('development.profiles')['profiles'][0]
        self.assertTrue(configured['gameplay_source'])
        self.assertTrue(configured['verification_suite'])
        params = dict(profile='local', project='Main/project.poimaproject', path='gameplay-src/Game.cs')
        source = client.world('project.source.read', params)
        self.assertEqual(source['text'].encode(), path.read_bytes())
        self.assertEqual(source['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())

        def write(options, error=None):
            return client.tool('poima_source', {'method': 'project.source.write', 'params': options}, error)

        changed = dict(params, expected_sha256=source['sha256'], text=source['text']+'// café\r\n')
        client.world('project.source.write', changed, error=-32602)
        client.tool('poima_build', dict(method='project.source.write', params=changed), error=-32602)
        self.assertTrue(write(changed)['changed'])
        stamp = path.stat().st_mtime_ns
        self.assertFalse(write(changed)['changed'])
        self.assertEqual(path.stat().st_mtime_ns, stamp)
        self.assertEqual(path.read_bytes(), changed['text'].encode())
        write(dict(changed, text='stale'), error=-32009)
        write(dict(changed, text='a\0b'), error=-32602)
        write(dict(changed, text='a'*131073), error=-32602)
        for bad in ('gameplay-src/../world.json', 'gameplay-src/Game.csproj', 'AGENTS.md'):
            client.world('project.source.read', dict(params, path=bad), error=-32602)
        client.world('project.source.read', dict(params, project='Other/project.poimaproject'), error=-32009)
        client.world('project.source.read', dict(params, path='gameplay-src/game.cs'), error=-32091)

        new_file = dict(params, path='gameplay-src/Extra.cs')
        self.assertFalse(client.world('project.source.read', new_file)['exists'])
        self.assertTrue(write(dict(new_file, expected_sha256=None, text=''))['changed'])
        alias = path.with_name('Alias.cs')
        os.link(path, alias)
        client.world('project.source.read', dict(params, path='gameplay-src/Alias.cs'), error=-32091)
        alias.unlink()
        self.assertEqual(sorted(f.name for f in path.parent.iterdir()), ['Extra.cs', 'Game.cs', 'Game.csproj'])

        selectors = dict(profile='local', project='Main/project.poimaproject')
        checks_path = manifest.parent/'game.verify.json'
        self.assertEqual(client.world('project.verification.read', selectors),
                         dict(path=checks_path.name, exists=False, sha256=None, text=None))
        checks = dict(format='poima.game-verification', version=1, groups=[dict(name='Prenez le courant ⚡', sequence=[
            dict(expect=dict(label='Player begins at spawn', method='runtime.entity',
                             params={'id': '0'*31+'2'}, path='/world_matrix/14', equals=2))])])
        replacement = dict(selectors, expected_sha256=None, text=json.dumps(checks, ensure_ascii=False) + '\r\n')

        def put_checks(options, error=None, tool='poima_checks'):
            return client.tool(tool, {'method': 'project.verification.write', 'params': options}, error)

        for tool in ('poima_call', 'poima_build', 'poima_source'):
            put_checks(replacement, error=-32602, tool=tool)
        put_checks(dict(replacement, path='world.json'), error=-32602)
        put_checks(dict(replacement, project='Other/project.poimaproject'), error=-32009)
        for bad in ('{}', replacement['text'].replace('"version": 1', '"version": 1, "version": 1'),
                    replacement['text'].replace('runtime.entity', 'runtime.gameplay.edit')):
            put_checks(dict(replacement, text=bad), error=-32602)
        self.assertFalse(checks_path.exists())
        stored = put_checks(replacement)
        self.assertTrue(stored['changed'])
        self.assertEqual(stored['sha256'], hashlib.sha256(checks_path.read_bytes()).hexdigest())
        self.assertEqual(client.world('project.verification.read', selectors)['text'], replacement['text'])
        stamp = checks_path.stat().st_mtime_ns
        self.assertFalse(put_checks(replacement)['changed'])
        self.assertEqual(checks_path.stat().st_mtime_ns, stamp)
        revised = dict(replacement, text=replacement['text'].replace('"equals": 2', '"equals": 3'))
        put_checks(revised, error=-32009)
        self.assertTrue(put_checks(dict(revised, expected_sha256=stored['sha256']))['changed'])
        alias = checks_path.with_name('suite-alias.json')
        os.link(checks_path, alias)
        client.world('project.verification.read', selectors, error=-32091)
        alias.unlink()
        self.assertFalse(any(manifest.parent.glob('.poima-source-*')))
        self.assertEqual(manifest.read_bytes(), original_manifest)
        self.assertEqual((manifest.parent/'world.json').read_bytes(), original_world)
        client.close()

    def test_project_mcp_rejects_bad_targets_without_protocol_output(self):
        manifest, world = self.create_project()
        source = manifest.read_bytes()
        bad_spec = json.loads(source)
        bad_spec['entry']['world'] = '../outside.json'
        manifest.write_text(json.dumps(bad_spec), encoding='utf-8')
        for target in (manifest, manifest.with_name('missing.poimaproject'), world):
            with self.subTest(target=target.name):
                result = subprocess.run([BINARY, 'mcp', '--project', native(target)], input='',
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 4)
                self.assertEqual(result.stdout, '')
                self.assertTrue(result.stderr)
        manifest.write_bytes(source)
        world.unlink()
        result = subprocess.run([BINARY, 'mcp', '--project', native(manifest)], input='',
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 4)
        self.assertEqual(result.stdout, '')
        self.assertFalse(world.exists(), 'A missing project entry must not open an empty world')
        self.assertFalse(list(manifest.parent.rglob('*.lock')))

    def test_authoring_receipts_conflicts_undo_and_eof_persistence(self):
        client = self.client()
        tools = client.rpc('tools/list')['tools']
        self.assertEqual({tool['name'] for tool in tools}, {'poima_discover', 'poima_reference', 'poima_call',
                                                          'poima_build', 'poima_source', 'poima_checks'})
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

    def test_grouped_discovery_matches_individual_lookups(self):
        client = self.client()
        args = {
            'methods': ['entity.query', 'world.inspect'],
            'components': ['Transform'],
            'sections': ['transport'],
            'mutations': [{'operation': 'entity.create'}, {'operation': 'component.set', 'type': 'Transform'}],
        }
        catalog = client.tool('poima_discover')
        self.assertIn('world.describe.select', catalog['methods'])
        before = client.world('world.inspect')
        history = client.world('world.history')
        group = client.tool('poima_discover', {'selection': args})
        self.assertEqual(group, client.world('world.describe.select', args))
        self.assertTrue(group['partial'])
        self.assertEqual(group['view'], 'selection')
        singles = []
        for view in ('method', 'component', 'section'):
            field = view + 's'
            expected = {}
            for name in args[field]:
                single = client.tool('poima_discover', {'view': view, 'name': name})
                singles.append(single)
                expected.update(single[field])
                for key in ('schema_revision', 'authoring_contract', 'runtime_game_session_contract'):
                    self.assertEqual(group[key], single[key])
            self.assertEqual(group[field], expected)
        for index, selection in enumerate(args['mutations']):
            single = client.tool('poima_discover', {'view': 'mutation', **selection})
            singles.append(single)
            self.assertEqual(group['mutations'][index], {
                'selection': selection, 'schema': single['methods']['world.transact']})
        size = lambda payload: len(json.dumps(payload, separators=(',', ':')).encode())
        self.assertLess(size(group), sum(map(size, singles)))
        self.assertLessEqual(size(group), 24 * 1024)
        for bad in (None, {}, {'methods': []}, {'methods': ['entity.get', 'entity.get']},
                    {'components': [True]}, {'sections': ['é' * 65]},
                    {'sections': ['authoring_contract']}, {'methods': ['entity.get', 'missing.method']},
                    {'methods': catalog['methods'][:12], 'components': ['Transform']},
                    {'mutations': [{'operation': 'entity.create', 'type': 'Transform'}]},
                    {'mutations': [{'operation': 'entity.create'}] * 2},
                    {'methods': ['world.transact']}):
            with self.subTest(bad=bad):
                client.tool('poima_discover', {'selection': bad}, error=-32602)
        client.tool('poima_discover', {'view': 'catalog', 'selection': args}, error=-32602)
        self.assertEqual(client.world('world.inspect'), before)
        self.assertEqual(client.world('world.history'), history)

    def test_mutation_discovery_advertises_selectors_and_keeps_guards(self):
        client = self.client()
        discover = next(tool for tool in client.rpc('tools/list')['tools']
                        if tool['name'] == 'poima_discover')
        properties = discover['inputSchema']['properties']
        self.assertIn('mutation', properties['view']['enum'])
        self.assertIn('operation', properties)
        self.assertIn('type', properties)
        before = client.world('world.inspect')
        full = client.tool('poima_discover', {'view': 'method', 'name': 'world.transact'})
        selected = client.tool('poima_discover', {'view': 'mutation',
                                                'operation': 'component.set', 'type': 'Transform'})
        self.assertEqual(selected['selection'], {'operation': 'component.set', 'type': 'Transform'})
        schema = selected['methods']['world.transact']
        original = full['methods']['world.transact']
        self.assertEqual(schema['required'], original['required'])
        for field in ('request_id', 'base_revision', 'preview'):
            self.assertEqual(schema['properties'][field], original['properties'][field])
        self.assertEqual(schema['properties']['ops']['maxItems'], original['properties']['ops']['maxItems'])
        branches = schema['properties']['ops']['items']['oneOf']
        expected = [branch for branch in original['properties']['ops']['items']['oneOf']
                    if branch['properties'].get('op', {}).get('const') == 'component.set'
                    and branch['properties'].get('type', {}).get('const') == 'Transform']
        self.assertEqual(branches, expected)
        self.assertEqual(client.world('world.inspect'), before)
        for params in ({'view': 'mutation'},
                       {'view': 'mutation', 'operation': 'entity.create', 'type': 'Transform'},
                       {'view': 'mutation', 'operation': 'component.remove', 'type': 'Transform'},
                       {'view': 'mutation', 'operation': 'missing.operation'}):
            with self.subTest(params=params):
                client.tool('poima_discover', params, error=-32602)
                self.assertEqual(client.world('world.inspect'), before)
        # Focused discovery does not restrict later mixed atomic edits.
        entity = uuid.uuid4().hex
        result = client.world('world.transact', {'request_id': uuid.uuid4().hex,
                       'base_revision': before['revision'], 'ops': [
                           {'op': 'entity.create', 'id': entity, 'name': 'Scoped MCP'},
                           {'op': 'component.set', 'id': entity, 'type': 'Transform',
                            'value': {'position': [1, 2, 3], 'rotation': [0, 0, 0, 1],
                                      'scale': [1, 1, 1]}}]})
        self.assertTrue(result['committed'])
        self.assertEqual(client.world('entity.get', {'id': entity, 'component': 'Transform'})['value']['position'], [1, 2, 3])

    def test_engine_reference_pages_match_built_public_guides(self):
        client = self.client(cwd=self.temp.name)
        method = 'world.reference'
        before = client.world('world.inspect'), client.world('world.history')
        discovery = client.tool('poima_discover', {'view': 'method', 'name': method})
        reference = next(tool for tool in client.rpc('tools/list')['tools'] if tool['name'] == 'poima_reference')
        self.assertEqual(reference['inputSchema'], discovery['methods'][method])
        self.assertEqual(reference['annotations'], {'readOnlyHint': True, 'destructiveHint': False,
                                                    'idempotentHint': True, 'openWorldHint': False})
        catalog = client.tool('poima_reference')
        self.assertEqual(catalog, client.world(method))
        examples = Path(__file__).resolve().parents[1] / 'docs'
        expected = {'creator-gameplay': 'CREATOR_GAMEPLAY.md', 'managed-gameplay': 'MANAGED_GAMEPLAY.md',
                    'game-ui': 'GAME_UI.md', 'game-verification': 'GAME_VERIFICATION.md'}
        self.assertEqual(set(discovery['methods'][method]['properties']['guide']['enum']), set(expected))
        self.assertIn('section', discovery['methods'][method]['properties'])
        self.assertEqual({guide['guide'] for guide in catalog['guides']}, set(expected))
        for guide in catalog['guides']:
            key = guide['guide']
            data = (examples / expected[key]).read_bytes()
            with self.subTest(guide=key):
                self.assertEqual(guide['sha256'], hashlib.sha256(data).hexdigest())
                self.assertEqual(guide['bytes'], len(data))
                outline = client.tool('poima_reference', {'guide': key})
                self.assertEqual(outline, client.world(method, {'guide': key}))
                headings = outline['headings']
                self.assertTrue(headings)
                lines = data.splitlines(keepends=True)
                for heading in headings:
                    prefix = b'#' * heading['level'] + b' '
                    self.assertEqual(lines[heading['line']-1].rstrip(), prefix + heading['title'].encode('utf-8'))
                pages = []
                line = 1
                while line is not None:
                    page = client.world(method, {'guide': key, 'line': line, 'lines': 180})
                    payload = page['text'].encode('utf-8')
                    self.assertTrue(0 < len(payload) <= catalog['page_bytes'])
                    self.assertEqual(page['sha256'], guide['sha256'])
                    self.assertEqual(page['engine_version'], catalog['engine_version'])
                    self.assertEqual(page['line'], line)
                    following = line + page['line_count']
                    self.assertEqual(page['next_line'], following if following <= guide['line_count'] else None)
                    pages.append(payload)
                    line = page['next_line']
                self.assertEqual(b''.join(pages), data)
        sections = (
            ('creator-gameplay', '## Publish and attach', '## Check the playable build', 5),
            ('managed-gameplay', '## Control two-bone IK from C#', '## Control planar root motion from C#', 17),
            ('game-verification', '## Write a suite', '## Run and inspect', 180),
            ('game-ui', '# Native game UI foundation', None, 180),
        )
        for key, heading, end_heading, page_lines in sections:
            with self.subTest(guide=key, section=heading):
                data = (examples / expected[key]).read_bytes()
                lines = data.splitlines(keepends=True)
                start = next(i for i, line in enumerate(lines, 1) if line.rstrip() == heading.encode())
                end = (next(i for i, line in enumerate(lines, 1) if line.rstrip() == end_heading.encode()) - 1
                       if end_heading else len(lines))
                title = heading.split(' ', 1)[1]
                selector = {'guide': key, 'section': title, 'lines': page_lines}
                line = start
                content = bytearray()
                while line is not None:
                    page = client.tool('poima_reference', selector)
                    self.assertEqual(page, client.world(method, selector))
                    self.assertEqual(page['section'], title)
                    self.assertEqual(page['section_start_line'], start)
                    self.assertEqual(page['section_end_line'], end)
                    self.assertEqual(page['sha256'], hashlib.sha256(data).hexdigest())
                    self.assertEqual(page['engine_version'], catalog['engine_version'])
                    self.assertEqual(page['line'], line)
                    payload = page['text'].encode('utf-8')
                    self.assertTrue(0 < len(payload) <= catalog['page_bytes'])
                    self.assertTrue(0 < page['line_count'] <= page_lines)
                    following = line + page['line_count']
                    self.assertEqual(page['next_line'], following if following <= end else None)
                    content.extend(payload)
                    line = page['next_line']
                    selector['line'] = line
                self.assertEqual(bytes(content), b''.join(lines[start-1:end]))
                for invalid_line in (True, start-1, end+1):
                    client.world(method, {**selector, 'line': invalid_line}, error=-32602)
        for invalid in ({'guide': '../authoring.json'}, {'path': 'reference'}, {'line': 1},
                        {'guide': 'game-ui', 'lines': 1}, {'guide': 'game-ui', 'line': True},
                        {'guide': 'game-ui', 'line': 9999}, {'guide': 'game-ui', 'line': 1, 'lines': 181},
                        {'section': 'Publish and attach'}, {'guide': 'creator-gameplay', 'section': ''},
                        {'guide': 'creator-gameplay', 'section': True}, {'guide': 'creator-gameplay', 'section': 'é' * 65},
                        {'guide': 'creator-gameplay', 'section': 'publish and attach'},
                        {'guide': 'creator-gameplay', 'section': 'Publish and attach', 'lines': 181}):
            with self.subTest(params=invalid):
                self.assertEqual(client.tool('poima_reference', invalid, error=-32602),
                                 client.world(method, invalid, error=-32602))
        illegal_methods = ('world.transact', 'project.source.write', 'project.verification.write', 'development.publish')
        for method_override in illegal_methods:
            original = {'guide': 'creator-gameplay', 'section': 'Publish and attach'}
            invalid = {**original, 'method': method_override, 'params': {}}
            self.assertEqual(client.tool('poima_reference', invalid, error=-32602),
                             client.world(method, invalid, error=-32602))
        self.assertEqual((client.world('world.inspect'), client.world('world.history')), before)
        self.assertFalse(self.path.exists())

    def test_asset_reference_discovery_reads_and_stale_error_translation(self):
        client = self.client()
        method = 'world.asset.references'
        discovery = client.tool('poima_discover', {'view': 'method', 'name': method})
        self.assertEqual(set(discovery['methods']), {method})
        self.assertIn('owner', discovery['methods'][method]['properties'])
        asset = 'b' * 64  # Syntactically valid; no cooked package is created.
        ids = ['1' * 32, '2' * 32]
        operations = []
        for index, entity in enumerate(ids):
            operations.extend([
                {'op': 'entity.create', 'id': entity, 'name': 'MCP provenance ' + str(index)},
                {'op': 'component.set', 'id': entity, 'type': 'StaticMesh',
                 'value': {'asset': asset, 'primitive': index, 'visible': index == 1}}])
        committed = client.world('world.transact', {'request_id': uuid.uuid4().hex,
                                                   'base_revision': 0, 'ops': operations})
        self.assertEqual(committed['revision'], 1)
        before = client.world('world.inspect'), client.world('world.history'), self.path.read_bytes()
        edges = [{'owner': {'kind': 'entity', 'id': entity}, 'component': 'StaticMesh',
                  'path': '/asset', 'asset': asset, 'kind': 'model',
                  'subresource': {'kind': 'primitive', 'index': index}}
                 for index, entity in enumerate(ids)]
        self.assertEqual(client.world(method), {'revision': 1, 'scope': 'authored',
                                               'selection': {}, 'edges': edges, 'next_after': None})
        owner = {'kind': 'entity', 'id': ids[1]}
        self.assertEqual(client.world(method, {'owner': owner, 'revision': 1}),
                         {'revision': 1, 'scope': 'authored', 'selection': {'owner': owner},
                          'edges': [edges[1]], 'next_after': None})
        client.world(method, {'revision': 0}, error=-32009)
        self.assertEqual((client.world('world.inspect'), client.world('world.history'),
                          self.path.read_bytes()), before)

    def test_animation_composition_error_data_is_preserved_and_atomic(self):
        from fbx_fixture import write_ascii_fixtures
        sources = write_ascii_fixtures(Path(self.temp.name) / 'sources')
        base, donor, valid = (sources / name for name in
                              ('skin.fbx', 'rest_mismatch.fbx', 'move_donor.fbx'))
        def digest(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()
        source_hashes = {path: digest(path) for path in (base, donor, valid)}
        client = self.client()
        client.world('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': 0,
                     'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex,
                              'name': 'Retained MCP authoring'}]})
        imported = client.world('asset.import', {'source': native(base)})
        nodes = client.world('asset.inspect', {'asset': imported['asset'], 'section': 'nodes'})['items']
        child = next(node for node in nodes if node['name'] == 'Child')
        def state():
            store = Path(str(self.path) + '.assets')
            return (client.world('world.inspect'), client.world('world.history'),
                    self.path.read_bytes(), {path.relative_to(store).as_posix(): digest(path)
                    for path in sorted(store.rglob('*')) if path.is_file()})
        before = state()
        focused = client.tool('poima_discover', {'view': 'method', 'name': 'asset.import'})
        self.assertEqual(set(focused['methods']), {'asset.import'})
        schema = focused['methods']['asset.import']['x-error-data']['animation_composition']
        self.assertIs(schema['additionalProperties'], False)
        self.assertEqual(schema['properties']['type']['const'], 'animation_composition')
        self.assertEqual(schema['properties']['policy']['const'], 'exact-skeleton-v1')
        self.assertEqual(schema['properties']['issues']['maxItems'], 64)
        params = {'source': native(base), 'animations': [native(donor)]}
        # Client.tool verifies MCP isError and equality of the COMPLETE JSON
        # structuredContent with decoded text content on each rejected call.
        error = client.world('asset.import', params, error=-32050)
        self.assertEqual(set(error), {'code', 'message', 'data'})
        report = error['data']
        self.assertEqual(report['type'], 'animation_composition')
        self.assertEqual(report['version'], 1)
        self.assertEqual(report['policy'], 'exact-skeleton-v1')
        self.assertEqual(report['comparison'], 'normalized_local_rest')
        self.assertEqual((report['donor_index'], report['required_base_nodes'],
                          report['checked_source_nodes'], report['matched_nodes'],
                          report['issue_count'], report['limit']), (0, 3, 3, 2, 1, 64))
        self.assertIs(report['truncated'], False)
        self.assertEqual(report['thresholds'], {'translation_meters': 1e-5,
                         'scale_absolute': 1e-6, 'absolute_quaternion_dot_min': 1-1e-10})
        self.assertEqual(len(report['issues']), 1)
        issue = report['issues'][0]
        self.assertEqual(set(issue), {'kind', 'base_node', 'donor_node', 'base_name',
                                     'donor_name', 'required_skeleton', 'animated_ancestry',
                                     'metrics', 'mismatches'})
        self.assertEqual(issue['kind'], 'rest_frame')
        self.assertIs(type(issue['base_node']), int)
        self.assertIs(type(issue['donor_node']), int)
        self.assertEqual(issue['base_node'], child['index'])
        self.assertEqual(issue['donor_node'], child['index'])
        self.assertEqual((issue['base_name'], issue['donor_name']), ('Child', 'Child'))
        self.assertIs(issue['required_skeleton'], True)
        self.assertIs(issue['animated_ancestry'], True)
        self.assertEqual(set(issue['metrics']), {'max_translation_meters', 'max_scale_absolute',
                                               'absolute_quaternion_dot', 'rotation_degrees'})
        self.assertAlmostEqual(issue['metrics']['max_translation_meters'], .01, delta=1e-10)
        self.assertEqual(issue['metrics']['max_scale_absolute'], 0)
        self.assertEqual(issue['metrics']['absolute_quaternion_dot'], 1)
        self.assertEqual(issue['metrics']['rotation_degrees'], 0)
        self.assertEqual(issue['mismatches'], {'translation': True, 'rotation': False, 'scale': False})
        self.assertEqual(state(), before)
        self.assertEqual(client.world('asset.import', params, error=-32050), error)
        self.assertEqual(state(), before)
        generic = client.world('asset.import', {'source': native(sources / 'truncated.fbx')}, error=-32050)
        self.assertNotIn('data', generic, 'Generic import failure fabricated composition metrics')
        self.assertEqual(state(), before)
        accepted = client.world('asset.import', {'source': native(base), 'animations': [native(valid)]})
        self.assertEqual(accepted['animations'], 1)
        self.assertEqual(state()[:3], before[:3])
        self.assertEqual({path: digest(path) for path in source_hashes}, source_hashes)

    def test_shared_endpoint_receipts_conflicts_and_detach(self):
        self.check_shared_endpoint([native(self.path)], self.path, 0)

    def test_project_shared_endpoint_receipts_conflicts_and_detach(self):
        manifest, world = self.create_project()
        self.check_shared_endpoint(['--project', native(manifest)], world, json.loads(world.read_bytes())['revision'])

    def check_shared_endpoint(self, target, world, revision):
        endpoint = 'mcp-' + uuid.uuid4().hex
        host = subprocess.Popen([BINARY, 'serve', *target, '--endpoint', endpoint],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def direct(method, params=None, timeout=3000):
            request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}}
            return subprocess.run([BINARY, 'connect', endpoint, '--timeout-ms', str(timeout)],
                                  input=json.dumps(request)+'\n', capture_output=True, text=True, timeout=10)
        client = None
        try:
            deadline = time.monotonic() + 15
            while True:
                probe = direct('world.inspect', timeout=100)
                if probe.returncode == 0:
                    break
                self.assertIsNone(host.poll(), probe.stdout + probe.stderr)
                self.assertLess(time.monotonic(), deadline, 'Host did not become ready')
                time.sleep(.02)
            client = Client(endpoint=endpoint)
            client.initialize()
            catalog = client.tool('poima_discover')
            self.assertEqual(catalog['session_scope'], 'shared_headless')
            self.assertNotIn('session.close', catalog['methods'])
            grouped = client.tool('poima_discover', {'selection': {'methods': ['host.shutdown', 'world.inspect']}})
            self.assertEqual(grouped['session_scope'], 'shared_headless')
            self.assertEqual(sorted(grouped['methods']), ['host.shutdown', 'world.inspect'])
            client.tool('poima_discover', {'selection': {'methods': ['world.inspect', 'session.close']}}, error=-32602)
            guide = {'guide': 'game-verification', 'section': 'Write a suite'}
            reference = direct('world.reference', guide)
            self.assertEqual(reference.returncode, 0, reference.stderr)
            self.assertEqual(client.tool('poima_reference', guide), json.loads(reference.stdout)['result'])
            transaction = {'request_id': uuid.uuid4().hex, 'base_revision': revision,
                           'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex, 'name': 'Shared MCP entity'}]}
            self.assertEqual(client.world('world.transact', transaction)['revision'], revision + 1)
            receipt = direct('world.transact', transaction)
            self.assertEqual(receipt.returncode, 0, receipt.stderr)
            self.assertTrue(json.loads(receipt.stdout)['result']['replayed'])
            client.world('world.transact', {**transaction, 'request_id': uuid.uuid4().hex}, error=-32009)
            client.close()
            client = None
            self.assertIsNone(host.poll(), 'MCP detach shut down the host')
            state = direct('world.inspect')
            self.assertEqual(state.returncode, 0, state.stderr)
            self.assertEqual(json.loads(state.stdout)['result']['revision'], revision + 1)
            self.assertEqual(json.loads(world.read_text())['revision'], revision + 1)
        finally:
            if client is not None:
                client.close()
            if host.poll() is None:
                try:
                    direct('host.shutdown')
                    host.wait(timeout=5)
                finally:
                    if host.poll() is None:
                        host.kill()
                        host.wait(timeout=5)
            host.communicate()
        self.assertEqual(host.returncode, 0)

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
