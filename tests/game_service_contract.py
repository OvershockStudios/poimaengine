#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable exported-game service, receipts and immutable-bundle qualification.

Requires a matching already-installed simulation runtime. Builds and downloads
nothing; opens no renderer or physical devices. The source project is deleted
before the relocated game's own executable serves its verified game.json.
"""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient
from poima_client.errors import RpcError


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            digest.update(block)
    return digest.hexdigest()


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        stat = path.lstat()
        need(not path.is_symlink() and not getattr(stat, 'st_file_attributes', 0) & 0x400,
             'Inventory contains a link/reparse point: '+str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = {'bytes': stat.st_size, 'sha256': sha(path)}
    return result


class Exercise:
    def __init__(self, args):
        self.args = args
        self.record = {'passed': False, 'commands': [], 'owners': [], 'calls': [], 'checks': [],
                       'forced_cleanup': [], 'timeouts': [], 'cleanup_errors': [],
                       'limitations': ['Primitive no-gameplay fixture; compiled gameplay has a separate qualification.',
                           'No renderer, physical input, audible output, throughput or clean-machine claim.']}
        self.hosts, self.clients = [], []
        self.binary = args.binary.resolve()
        self.runtime = args.runtime.resolve()
        self.output = args.output.resolve()
        self.output.mkdir(parents=True)
        self.pins = {path.relative_to(ROOT).as_posix(): sha(path) for path in
                     [Path(__file__).resolve(), *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
                      *sorted((ROOT/'tools/python/poima_client').glob('*.json'))]}
        self.runtime_inventory = inventory(self.runtime)
        self.record.update(sources=self.pins, exporter_sha256=sha(self.binary),
                           runtime_inventory=self.runtime_inventory)

    def native(self, path):
        text = str(path.resolve())
        if self.args.windows_interop:
            text = subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip()
        return text

    def cli(self, binary, *argv, success=True):
        command = [str(binary), *argv]
        row = {'command': command}
        self.record['commands'].append(row)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding='utf-8', cwd=self.output,
                                   start_new_session=os.name == 'posix')
        try:
            stdout, stderr = process.communicate(timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            self.record['timeouts'].append(command)
            self.terminate(process, command)
            raise
        row.update(exit_code=process.returncode, stderr=stderr, stdout=stdout)
        reply = json.loads(stdout)
        row['reply'] = reply
        need((process.returncode == 0) == success and
             reply.get('status') == ('ok' if success else 'error'), row)
        if not success:
            need(isinstance(reply.get('diagnostics'), list) and reply['diagnostics'], row)
        return reply

    def terminate(self, process, command):
        """Emergency owned-tree cleanup; any use makes qualification fail."""
        self.record['forced_cleanup'].append(command)
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           capture_output=True, timeout=20, check=True)
        elif self.args.windows_interop:
            # Never use an endpoint alone: the duplicate-owner test shares one
            # deliberately. Require the owned command's manifest/source path.
            native_output = self.native(self.output)
            markers = [arg for arg in command[1:] if native_output in arg]
            if len(command) == 4 and command[1:3] == ['mcp', '--endpoint']:
                markers = [' mcp ', command[3]]
            need(markers, 'Cannot identify an interop child without an owned path')
            quoted = ','.join("'"+value.replace("'", "''")+"'" for value in markers)
            script = "$m=@("+quoted+"); Get-CimInstance Win32_Process | Where-Object { $p=$_; $p.Name -eq 'poima.exe' -and $p.CommandLine -and (@($m | Where-Object { -not $p.CommandLine.Contains($_) }).Count -eq 0) } | ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F | Out-Null }"
            subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand',
                            base64.b64encode(script.encode('utf-16-le')).decode('ascii')],
                           capture_output=True, timeout=30, check=True)
            if process.poll() is None:
                process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=20)

    def host(self, executable, manifest, endpoint, save_root=None):
        command = [str(executable), 'game', 'serve', self.native(manifest), '--endpoint', endpoint]
        if save_root is not None:
            command += ['--save-root', self.native(save_root)]
        index = len(self.hosts)
        row = {'command': command, 'endpoint': endpoint, 'exit_code': None}
        stdout = (self.output/f'host-{index}.stdout').open('wb')
        stderr = (self.output/f'host-{index}.stderr').open('wb')
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                       cwd=self.output, start_new_session=os.name == 'posix')
        except BaseException:
            stdout.close(); stderr.close()
            raise
        owner = (process, stdout, stderr, row)
        self.hosts.append(owner)
        self.record['owners'].append(row)
        row['process_id'] = process.pid
        return owner

    def connect(self, executable, endpoint):
        client = WorldClient.connect(str(executable), endpoint, timeout_ms=self.args.timeout*1000,
                                     cwd=self.output, close_timeout=10)
        row = {'role': 'connect', 'endpoint': endpoint, 'process_id': client.transport.process_id}
        self.clients.append((client, row))
        self.record['owners'].append(row)
        return client

    def call(self, client, method, params=None, error=None):
        row = {'process_id': client.transport.process_id, 'method': method,
               'params': copy.deepcopy({} if params is None else params)}
        self.record['calls'].append(row)
        try:
            result = client.call(method, params, timeout=self.args.timeout)
        except RpcError as exc:
            row['error'] = {'code': exc.code, 'message': exc.message, 'data': exc.data}
            need(error is not None and exc.code == error, row)
            return row['error']
        need(error is None, 'Expected RPC rejection: '+repr(row))
        row['result'] = result
        return result

    def close_client(self, client):
        client.close()
        row = next(row for owned, row in self.clients if owned is client)
        row.update(exit_code=client.transport.returncode, stderr=client.transport.stderr_tail,
                   stderr_truncated=client.transport.stderr_truncated)
        need(row['exit_code'] == 0 and not row['stderr_truncated'], row)

    def stop_host(self, owner):
        process, stdout, stderr, row = owner
        try:
            process.wait(timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            self.record['timeouts'].append(row['command'])
            raise
        stdout.close(); stderr.close()
        index = self.hosts.index(owner)
        row.update(exit_code=process.returncode,
                   stdout=(self.output/f'host-{index}.stdout').read_text(encoding='utf-8'),
                   stderr=(self.output/f'host-{index}.stderr').read_text(encoding='utf-8'))
        need(row['exit_code'] == 0, row)

    def rejected_start(self, executable, manifest, *argv, diagnostic=None):
        result = self.cli(executable, 'game', 'serve', self.native(manifest), *argv, success=False)
        if diagnostic is not None:
            need(any(diagnostic.lower() in str(item.get('message', '')).lower()
                     for item in result['diagnostics']), result)
        need(inventory(self.bundle) == self.bundle_inventory,
             'Rejected startup changed the verified bundle')
        return result

    def mcp_batch(self, endpoint, version, runtime, catalog, world, owner):
        """Actual MCP handshake and tools through the same exported host.

        One finite batch closes stdin; no extra SDK notification API or test
        protocol state machine is introduced. A response to the notification
        violates the exact six-reply oracle.
        """
        requests = [
            dict(jsonrpc='2.0', id=1, method='initialize', params=dict(protocolVersion='2025-11-25',
                 capabilities={}, clientInfo=dict(name='poima-game-service-contract', version='1'))),
            dict(jsonrpc='2.0', method='notifications/initialized'),
            dict(jsonrpc='2.0', id=2, method='ping', params={}),
            dict(jsonrpc='2.0', id=3, method='tools/list', params={})]
        for identity, method, params in (
            (4, 'runtime.status', {}),
            (5, 'world.describe', {'view': 'catalog'}),
            (6, 'world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': world['revision'],
                 'ops': [{'op': 'entity.create', 'id': uuid.uuid4().hex, 'name': 'Forbidden MCP authoring'}]})):
            requests.append(dict(jsonrpc='2.0', id=identity, method='tools/call',
                params={'name': 'poima_call', 'arguments': {'method': method, 'params': params}}))
        command = [str(self.executable), 'mcp', '--endpoint', endpoint]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding='utf-8', cwd=self.output,
            start_new_session=os.name == 'posix')
        row = {'role': 'mcp', 'endpoint': endpoint, 'command': command, 'process_id': process.pid,
               'exit_code': None}
        self.record['owners'].append(row)
        try:
            stdout, stderr = process.communicate(''.join(json.dumps(item)+'\n' for item in requests),
                                                timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            self.record['timeouts'].append(command)
            self.terminate(process, command)
            raise
        row.update(exit_code=process.returncode, stdout=stdout, stderr=stderr)
        need(process.returncode == 0 and len(stdout.encode('utf-8')) <= 4*1024*1024 and
             len(stderr.encode('utf-8')) <= 16384, row)
        replies = [json.loads(line) for line in stdout.splitlines()]
        need(len(replies) == 6 and [item.get('id') for item in replies] == list(range(1, 7)) and
             all(item.get('jsonrpc') == '2.0' and 'result' in item and 'error' not in item for item in replies), replies)
        self.record.setdefault('mcp_batches', []).append({'requests': requests, 'replies': replies})
        initialized = replies[0]['result']
        need(initialized.get('protocolVersion') == '2025-11-25' and
             initialized.get('serverInfo') == {'name': 'Poima', 'version': version} and
             'tools' in initialized.get('capabilities', {}), initialized)
        need(replies[1]['result'] == {} and
             {tool['name'] for tool in replies[2]['result']['tools']} == {'poima_call', 'poima_discover'}, replies)
        for reply, expected, error in ((replies[3], runtime, None), (replies[4], catalog, None),
                                       (replies[5], None, -32081)):
            result = reply['result']
            structured = result['structuredContent']
            need(len(result['content']) == 1 and result['content'][0]['type'] == 'text' and
                 json.loads(result['content'][0]['text']) == structured, result)
            if error is None:
                need(result.get('isError') is False and structured.get('result') == expected and
                     'error' not in structured, result)
            else:
                need(result.get('isError') is True and structured['error']['code'] == error and
                     'result' not in structured, result)
        need(owner[0].poll() is None, 'Closing the MCP client killed the native game host')
        self.record['checks'].append('Actual MCP initialization, notification barrier, ping/catalog and tools share the CLI native session; readonly rejection preserves identical text/structured error content and closing MCP detaches cleanly.')

    def exercise(self):
        spec = json.loads((self.runtime/'runtime.json').read_text(encoding='utf-8'))
        need(spec['features']['simulation'] is True, 'Use a simulation runtime, not an authoring-only build')
        version = self.cli(self.binary, 'version')['result']['version']
        need(spec['engine_version'] == version, 'Exporter and installed runtime must match')
        project = self.output/'Owned source project'
        self.cli(self.binary, 'project', 'create', self.native(project), '--name', 'Exported service room')
        manifest = project/'project.json'
        authored = json.loads(manifest.read_text(encoding='utf-8'))
        authored['audio'] = False
        manifest.write_text(json.dumps(authored, indent=2)+'\n', encoding='utf-8')
        source = inventory(project)
        staged = self.output/'Exported game'
        self.cli(self.binary, 'project', 'build', self.native(manifest), '--output', self.native(staged),
                 '--runtime', self.native(self.runtime))
        need(inventory(project) == source, 'Export modified authored files')
        self.bundle = self.output/'Relocated game'
        staged.rename(self.bundle)
        shutil.rmtree(project)
        need(not project.exists(), 'Owned source project was not removed')
        self.manifest = self.bundle/'game.json'
        game = json.loads(self.manifest.read_text(encoding='utf-8'))
        self.executable = self.bundle/game['engine']['executable']
        self.bundle_inventory = inventory(self.bundle)
        inspected = self.cli(self.executable, 'game', 'inspect', self.native(self.manifest))['result']
        need(inspected['revision'] == game['source']['revision'] and inspected['entry'] == game['entry'], inspected)
        if 'input_profile' in game:
            need(inspected.get('input_profile') == game['input_profile'], 'Game inspection lost bundled input profile')
        self.record.update(bundle_inventory=self.bundle_inventory, game=game,
                           packaged_binary_sha256=sha(self.executable), source_removed=True,
                           relocated=True, inspection=inspected)

        endpoint = 'game-service-'+uuid.uuid4().hex
        saves = self.output/'External saves'; saves.mkdir()
        owner = self.host(self.executable, self.manifest, endpoint, saves)
        client = self.connect(self.executable, endpoint)
        catalog = self.call(client, 'world.describe', {'view': 'catalog'})
        need(catalog['read_only'] and catalog['mode'] == 'read_only_runtime' and catalog['runtime_available'], catalog)
        runtime = self.call(client, 'runtime.status')
        need(runtime['available'] and runtime['active'] and runtime['tick'] == 0 and
             runtime['authored_revision'] == game['source']['revision'], runtime)
        session = runtime['session_id']
        need(isinstance(session, str) and len(session) == 32, runtime)
        player = self.call(client, 'player.inspect')
        need(not player['active'] and player['generation'] == 0, 'Game service opened a player unrequested')
        world = self.call(client, 'world.inspect')
        need(world['read_only'] and world['revision'] == runtime['authored_revision'], world)
        self.mcp_batch(endpoint, version, runtime, catalog, world, owner)
        need(self.call(client, 'runtime.status') == runtime and self.call(client, 'world.inspect') == world,
             'MCP observation or rejected authoring changed native state')
        entities = self.call(client, 'entity.query', {'revision': world['revision'], 'limit': 256})
        need(entities['entities'], 'Exported primitive world query was empty')
        original = self.call(client, 'runtime.entity', {'session_id': session, 'id': game['entry']['controller'], 'tick': 0})
        profile = self.call(client, 'input.inspect', {'path': self.native(self.bundle/game['input_profile'])}) if 'input_profile' in game else None
        self.record['checks'].append('Relocated source-free bundle starts native simulation at authored revision and tick0 without a player; queries/profile reads share the authoritative session.')

        # Admission ordering: a missing manifest cannot win over an occupied
        # endpoint, and the existing host remains fully usable.
        missing = self.output/'missing-game.json'
        self.rejected_start(self.executable, missing, '--endpoint', endpoint, diagnostic='endpoint')
        need(self.call(client, 'runtime.status') == runtime, 'Duplicate host changed original session')
        self.record['checks'].append('Endpoint collision rejects before accessing a missing game manifest and leaves the existing owner intact.')

        forbidden = ['world.transact', 'world.undo', 'world.redo', 'input.transact', 'settings.transact',
                     'development.compile', 'development.jobs', 'asset.import', 'asset.material.generate',
                     'world.navigation.bake', 'asset.provenance.create']
        for method in forbidden:
            self.call(client, method, {}, error=-32081)
        before_config = self.call(client, 'save.status')
        need(before_config['generation'] == 1 and
             before_config['root'].replace('\\', '/').rstrip('/').split('/')[-1] == saves.name, before_config)
        # A sibling of content/world.json is still inside the protected bundle.
        for path in (self.bundle, self.bundle/'runtime', self.bundle/'runtime/bin'):
            self.call(client, 'save.configure', {'request_id': uuid.uuid4().hex,
                'expected_generation': 1, 'root': self.native(path)}, error=-32070)
        need(self.call(client, 'save.status') == before_config and inventory(saves) == {},
             'Rejected writes changed configuration or created save files')
        need(self.call(client, 'runtime.status') == runtime and self.call(client, 'world.inspect') == world,
             'Read-only rejections changed authoring/runtime')
        self.record['checks'].append('Authoring, input/settings writes and development methods reject; save protection includes runtime directories outside the content folder.')

        request = {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': 0,
                   'ticks': 12, 'inputs': [{'entity': game['entry']['controller'], 'move': [.25, .5], 'look': [8, 0]}]}
        stepped = self.call(client, 'runtime.step', request)
        need(stepped['tick'] == 12 and not stepped['replayed'], stepped)
        moved = self.call(client, 'runtime.entity', {'session_id': session, 'id': game['entry']['controller'], 'tick': 12})
        need(moved['world_matrix'] != original['world_matrix'], 'Native controller did not move')
        self.close_client(client)
        need(owner[0].poll() is None, 'Detaching client ended the service')
        client = self.connect(self.executable, endpoint)
        need(self.call(client, 'runtime.step', request) == {**stepped, 'replayed': True}, 'Reconnect replay receipt differs')
        need(self.call(client, 'runtime.entity', {'session_id': session, 'id': game['entry']['controller'], 'tick': 12}) == moved,
             'Receipt retry advanced or changed simulation')
        self.call(client, 'runtime.step', {**request, 'ticks': 11}, error=-32010)
        self.call(client, 'runtime.step', {**request, 'request_id': uuid.uuid4().hex}, error=-32009)
        self.record['checks'].append('Real native controller step survives detach/reconnect; exact receipt retry does not advance and changed/stale requests reject.')

        saved = self.call(client, 'save.write', {'request_id': uuid.uuid4().hex, 'configuration_generation': 1,
            'slot': 'service', 'expected_generation': 0, 'session_id': session, 'expected_tick': 12,
            'expected_gameplay_revision': 0})
        save_files = inventory(saves)
        need(saved['generation'] == 1 and save_files, 'External durable save was not written')
        continued = self.call(client, 'runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
            'expected_tick': 12, 'ticks': 3})
        fresh = uuid.uuid4().hex
        restore = {'request_id': uuid.uuid4().hex, 'configuration_generation': 1, 'slot': 'service',
            'expected_generation': 1, 'revision': world['revision'], 'expected_session_id': session,
            'expected_tick': 15, 'expected_gameplay_revision': 0, 'new_session_id': fresh}
        loaded = self.call(client, 'save.load', restore)
        need(continued['tick'] == 15 and loaded['tick'] == 12 and loaded['session_id'] == fresh, loaded)
        restored = self.call(client, 'runtime.entity', {'session_id': fresh, 'id': game['entry']['controller'], 'tick': 12})
        need({k: v for k, v in restored.items() if k != 'session_id'} ==
             {k: v for k, v in moved.items() if k != 'session_id'}, 'Durable load did not restore native entity state')
        need(self.call(client, 'save.load', restore) == {**loaded, 'replayed': True} and inventory(saves) == save_files,
             'Load receipt retried storage or replacement')
        need(inventory(self.bundle) == self.bundle_inventory, 'Live service wrote into immutable bundle')
        self.record.update(save_inventory=save_files, saved=saved, loaded=loaded, stepped=stepped,
                           original_entity=original, moved_entity=moved, restored_entity=restored, input_profile=profile)
        self.record['checks'].append('External save captures tick12; actual replacement restores exact controller state and an exact Load retry is inert; full bundle bytes remain unchanged.')
        need(self.call(client, 'host.shutdown')['closed'], 'Host did not accept shutdown')
        self.close_client(client); self.stop_host(owner)

        # Clean exit must release the same endpoint; a fresh owner gets tick0,
        # rather than retaining the previous owner's session and replay cache.
        again = self.host(self.executable, self.manifest, endpoint)
        client = self.connect(self.executable, endpoint)
        restarted = self.call(client, 'runtime.status')
        need(restarted['active'] and restarted['tick'] == 0 and restarted['session_id'] not in (session, fresh), restarted)
        need(self.call(client, 'save.status') == {'generation': 0, 'root': None}, 'Save routing leaked into fresh owner')
        need(self.call(client, 'host.shutdown')['closed'], 'Restarted host did not shut down')
        self.close_client(client); self.stop_host(again)
        self.record['checks'].append('Clean shutdown releases the endpoint; restart creates a fresh tick0 native session without inherited save configuration.')

        endpoint = 'game-invalid-'+uuid.uuid4().hex
        invalid_arguments = [[], ['--endpoint'], ['--endpoint', endpoint, '--endpoint', endpoint],
            ['--endpoint', 'bad.name'], ['--endpoint', 'x'*65], ['--endpoint', 'é'],
            ['--endpoint', endpoint, '--unknown'], ['--endpoint', endpoint, '--save-root'],
            ['--endpoint', endpoint, '--save-root', self.native(self.output/'missing-saves')],
            ['--endpoint', endpoint, '--save-root', self.native(self.bundle)],
            ['--endpoint', endpoint, '--save-root', self.native(self.bundle/'runtime')]]
        for arguments in invalid_arguments:
            self.rejected_start(self.executable, self.manifest, *arguments)

        # Change only owned payloads after all live owners have exited. Exact
        # originals are restored in finally, so each rejection isolates a cause.
        self.rejected_start(self.executable, missing, '--endpoint', endpoint)
        original_manifest = self.manifest.read_bytes()
        world_path = self.bundle/game['entry']['world']
        original_world = world_path.read_bytes()
        cases = [
            ('malformed_json', self.manifest, b'{"format":'),
            ('duplicate_manifest_field', self.manifest, original_manifest.replace(b'{', b'{"version":1,', 1)),
            ('unsupported_manifest_version', self.manifest,
             (json.dumps({**game, 'version': 999})+'\n').encode()),
            ('invalid_camera_entry', self.manifest,
             (json.dumps({**game, 'entry': {**game['entry'], 'camera': game['entry']['controller']}})+'\n').encode()),
            ('payload_hash_mismatch', world_path, original_world+b' ')]
        for label, path, invalid in cases:
            original = path.read_bytes()
            path.write_bytes(invalid)
            invalid_inventory = inventory(self.bundle)
            try:
                reply = self.cli(self.executable, 'game', 'serve', self.native(self.manifest),
                                 '--endpoint', endpoint, success=False)
                need(inventory(self.bundle) == invalid_inventory, 'Invalid-bundle rejection repaired/wrote files')
                self.record.setdefault('invalid_bundles', []).append({'case': label, 'reply': reply})
            finally:
                path.write_bytes(original)
            need(inventory(self.bundle) == self.bundle_inventory, 'Owned test restoration was incomplete')
        extra = self.bundle/'unlisted.txt'; extra.write_text('Owned uninventoried payload.\n', encoding='utf-8')
        try:
            before = inventory(self.bundle)
            self.cli(self.executable, 'game', 'serve', self.native(self.manifest), '--endpoint', endpoint, success=False)
            need(inventory(self.bundle) == before, 'Extra-file rejection changed bundle')
        finally:
            extra.unlink()
        # Rehash intentionally coherent inventories to reach admission checks;
        # a hash mismatch alone would not prove platform/runtime capability
        # validation. Runtime metadata edits are confined to our copied bundle.
        runtime_descriptor = self.bundle/'runtime/runtime.json'
        descriptor_bytes = runtime_descriptor.read_bytes()
        descriptor = json.loads(descriptor_bytes)
        for label in ('wrong_platform', 'simulation_disabled', 'audio_required'):
            changed_game, changed_runtime = copy.deepcopy(game), copy.deepcopy(descriptor)
            renames = []
            invocation = self.executable
            if label == 'wrong_platform':
                other = 'Linux' if descriptor['target_os'] == 'Windows' else 'Windows'
                changed_game['engine']['target_os'] = other
                changed_runtime['target_os'] = other
                changed_runtime['features']['audio'] = False
                other_executable = 'bin/poima' if other == 'Linux' else 'bin/poima.exe'
                changed_runtime['executable'] = other_executable
                changed_game['engine']['executable'] = 'runtime/'+other_executable
                old_launcher = 'launch.cmd' if descriptor['target_os'] == 'Windows' else 'launch.sh'
                other_launcher = 'launch.sh' if other == 'Linux' else 'launch.cmd'
                for old, new in ((game['engine']['executable'], changed_game['engine']['executable']),
                                 (old_launcher, other_launcher)):
                    old_path, new_path = self.bundle/old, self.bundle/new
                    old_path.rename(new_path)
                    renames.append((old_path, new_path))
                    entry = next(item for item in changed_game['files'] if item['path'] == old)
                    entry['path'] = new
                changed_game['files'].sort(key=lambda item: item['path'])
                # Verify a coherent foreign-target metadata fixture using the
                # actual host executable; never execute a foreign binary or
                # interpret a mismatched binary as a real foreign build.
                invocation = self.binary
            elif label == 'simulation_disabled':
                changed_runtime['features']['simulation'] = False
            else:
                changed_runtime['features']['audio'] = False
                changed_game['audio'] = True
            changed_bytes = (json.dumps(changed_runtime, indent=2)+'\n').encode('utf-8')
            runtime_descriptor.write_bytes(changed_bytes)
            file_record = next(item for item in changed_game['files'] if item['path'] == 'runtime/runtime.json')
            file_record.update(size=len(changed_bytes), sha256=hashlib.sha256(changed_bytes).hexdigest())
            self.manifest.write_text(json.dumps(changed_game, indent=2)+'\n', encoding='utf-8')
            before = inventory(self.bundle)
            try:
                if label == 'wrong_platform':
                    self.cli(invocation, 'game', 'inspect', self.native(self.manifest))
                reply = self.cli(invocation, 'game', 'serve', self.native(self.manifest),
                                 '--endpoint', endpoint, success=False)
                if label == 'wrong_platform':
                    need(any('different platform' in item['message'].lower()
                             for item in reply['diagnostics']), 'Foreign target did not reach host platform admission')
                need(inventory(self.bundle) == before, 'Admission rejection changed coherent bundle')
                self.record.setdefault('admission_rejections', []).append({'case': label, 'reply': reply})
            finally:
                runtime_descriptor.write_bytes(descriptor_bytes)
                self.manifest.write_bytes(original_manifest)
                for old_path, new_path in reversed(renames):
                    new_path.rename(old_path)
            need(inventory(self.bundle) == self.bundle_inventory, 'Admission test restoration was incomplete')
        # All failed startups must release admission; successful use of the
        # exact endpoint is stronger than a pathname/socket disappearance check.
        after = self.host(self.executable, self.manifest, endpoint)
        client = self.connect(self.executable, endpoint)
        need(self.call(client, 'runtime.status')['tick'] == 0, 'Failed startup poisoned endpoint ownership')
        need(self.call(client, 'host.shutdown')['closed'], 'Final host did not shut down')
        self.close_client(client); self.stop_host(after)
        self.record['checks'].append('Malformed argv, nonexistent/inside-bundle save roots, malformed/duplicate/version/hash/extra-file bundles reject without writes; endpoint remains reusable.')
        need(inventory(self.bundle) == self.bundle_inventory and inventory(self.runtime) == self.runtime_inventory,
             'Bundle or installed runtime changed')
        need(sha(self.binary) == self.record['exporter_sha256'], 'Exporter changed during qualification')
        for name, expected in self.pins.items():
            need(sha(ROOT/name) == expected, 'Verifier dependency changed: '+name)

    def cleanup(self):
        for client, row in reversed(self.clients):
            if client.closed:
                continue
            try:
                client.close()
                row.update(exit_code=client.transport.returncode, stderr=client.transport.stderr_tail,
                           stderr_truncated=client.transport.stderr_truncated)
            except BaseException as exc:
                self.record['cleanup_errors'].append(repr(exc))
        for process, stdout, stderr, row in reversed(self.hosts):
            if process.poll() is None:
                try:
                    self.terminate(process, row['command'])
                except BaseException as exc:
                    self.record['cleanup_errors'].append(repr(exc))
            stdout.close(); stderr.close()
            row['exit_code'] = process.poll()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=int, default=30)
    args = parser.parse_args()
    if not args.binary.is_file() or not (args.runtime/'runtime.json').is_file():
        parser.error('Supply an existing exporter and installed runtime/runtime.json')
    if args.output.exists() or not 10 <= args.timeout <= 120:
        parser.error('Output must be new; timeout must be 10..120 seconds')
    exercise = Exercise(args)
    try:
        exercise.exercise()
        exercise.record['passed'] = True
    except BaseException as exc:
        exercise.record.update(error=repr(exc), traceback=traceback.format_exc())
    finally:
        exercise.cleanup()
        if any(exercise.record[key] for key in ('timeouts', 'forced_cleanup', 'cleanup_errors')):
            exercise.record['passed'] = False
        exercise.record['rpc_count'] = len(exercise.record['calls'])
        exercise.record['mcp_request_count'] = sum(len(batch['replies']) for batch in exercise.record.get('mcp_batches', []))
        exercise.record['mcp_notification_count'] = len(exercise.record.get('mcp_batches', []))
        (exercise.output/'evidence.json').write_text(json.dumps(exercise.record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'passed': exercise.record['passed'], 'rpc_count': exercise.record['rpc_count'],
                      'evidence': str(exercise.output/'evidence.json')}))
    return 0 if exercise.record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
