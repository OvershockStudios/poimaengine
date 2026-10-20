#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Consume an existing same-platform game bundle through native game stdio.

Builds, exports and downloads nothing. Run inside an admitted external process
owner for descendant-tree qualification. This fixture observes and reaps its
direct subprocesses; it is not a Job/subreaper or an OS sandbox. Exact continued
save equality is a strict witness for the supplied bundle, not a guarantee for
arbitrary contact dynamics or gameplay which deliberately changes after Load.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient
from poima_client.errors import RpcError

MIB = 1024 * 1024
BUNDLE_BYTES, OUTPUT_BYTES = 128 * MIB, 512 * MIB
MAX_FILES, MAX_RPC, STREAM_BYTES = 8192, 160, 8 * MIB


def need(value, message):
    if not value:
        raise AssertionError(message)


def strict(raw):
    def pairs(rows):
        result = {}
        for key, value in rows:
            need(key not in result, 'Duplicate JSON member: ' + key)
            result[key] = value
        return result
    def floating(text):
        value = float(text)
        need(math.isfinite(value), 'Nonfinite JSON number')
        return value
    def invalid(text):
        raise AssertionError('Nonfinite JSON number: ' + text)
    return json.loads(raw, object_pairs_hook=pairs, parse_float=floating,
                      parse_constant=invalid)


def exact(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return set(a) == set(b) and all(exact(a[k], b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(exact(x, y) for x, y in zip(a, b))
    return a == b


def identity(path):
    before = path.lstat()
    need(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and
         not getattr(before, 'st_file_attributes', 0) & 0x400,
         'Expected an ordinary, independent regular file: ' + str(path))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.lstat()
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
         'File changed while reading: ' + str(path))
    return dict(bytes=after.st_size, sha256=digest)


def inventory(root, *, hashes=True, cap=OUTPUT_BYTES, maximum=MAX_FILES):
    need(root.is_dir() and not root.is_symlink(), 'Ordinary inventory root required')
    rows, total = {}, 0
    def visit(directory):
        nonlocal total
        with os.scandir(directory) as entries:
            for entry in entries:
                # Windows DirEntry metadata omits the hard-link count. Query
                # the path itself so the independent-file guard stays strict.
                info = Path(entry.path).lstat()
                need(not stat.S_ISLNK(info.st_mode) and
                     not getattr(info, 'st_file_attributes', 0) & 0x400,
                     'Linked/reparse output or input: ' + entry.path)
                if stat.S_ISDIR(info.st_mode):
                    visit(Path(entry.path))
                else:
                    need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                         'Nonregular or aliased file: ' + entry.path)
                    total += info.st_size
                    need(total <= cap and len(rows) < maximum, 'Observed storage bound')
                    path = Path(entry.path)
                    rows[path.relative_to(root).as_posix()] = (
                        identity(path) if hashes else dict(bytes=info.st_size))
    visit(root)
    return rows


def relative(root, text):
    need(type(text) is str and text and ':' not in text and '\\' not in text and
         all(part not in ('', '.', '..') for part in text.split('/')),
         'Portable contained manifest path required')
    path = root.joinpath(*text.split('/'))
    need(path.resolve().is_relative_to(root.resolve()), 'Manifest path escaped bundle')
    return path


def write(path, value):
    raw = (json.dumps(value, indent=2, allow_nan=False) + '\n').encode()
    need(len(raw) <= 8 * MIB, 'Report exceeds 8 MiB')
    with path.open('xb') as stream:
        stream.write(raw)


class Contract:
    def __init__(self, args):
        self.args, self.started = args, time.monotonic()
        self.until = self.started + args.seconds
        self.output = args.output.absolute()
        self.binary, self.game = args.binary.resolve(strict=True), args.game.resolve(strict=True)
        need(self.game.name == 'game.json' and not self.output.exists(),
             'Existing game.json and fresh output required')
        need(self.output.parent.is_dir(), 'Output parent must already exist')
        need(not self.output.resolve().is_relative_to(self.game.parent) and
             not self.game.parent.is_relative_to(self.output.resolve()) and
             not self.binary.is_relative_to(self.output.resolve()), 'Input/output overlap')
        self.input_inventory = inventory(self.game.parent, cap=BUNDLE_BYTES, maximum=4097)
        self.binary_identity = identity(self.binary)
        with self.binary.open('rb') as stream:
            magic = stream.read(4)
        need((os.name == 'nt' and magic[:2] == b'MZ') or
             (sys.platform.startswith('linux') and magic == b'\x7fELF'),
             'Native Windows/Linux executable required; no interop bridge')
        self.manifest = strict(self.game.read_bytes())
        declared = {row['path']: dict(bytes=row['size'], sha256=row['sha256'])
                    for row in self.manifest['files']}
        need(len(declared) == len(self.manifest['files']) and
             set(self.input_inventory) == set(declared) | {'game.json'} and
             all(self.input_inventory[name] == pin for name, pin in declared.items()),
             'Complete manifest and actual bundle bytes differ')
        self.source_pins = {str(p): identity(p) for p in
            [Path(__file__).resolve(), *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
             *sorted((ROOT/'tools/python/poima_client').glob('*.json'))]}
        need(sum(row['bytes'] for row in self.input_inventory.values()) * 2 +
             32 * MIB < OUTPUT_BYTES, 'Prospective two-copy/log storage reserve')
        self.output.mkdir()
        for directory in ('commands', 'owners', 'checkpoints', 'saves', 'mutations'):
            (self.output/directory).mkdir()
        self.report = dict(format='poima.game-stdio.contract.v1', passed=False,
            inputs=dict(binary=dict(path=str(self.binary), identity=self.binary_identity),
                        game=str(self.game), bundle_inventory=self.input_inventory),
            source_pins=self.source_pins, commands=[], owners=[], calls=[], checkpoints={},
            whole_state_pairs=[], checks=[], errors=[], cleanup_errors=[],
            limits=dict(seconds=args.seconds, call_seconds=args.timeout, close_seconds=10,
                        rpc=MAX_RPC, bundle_bytes=BUNDLE_BYTES, output_bytes=OUTPUT_BYTES,
                        output_files=MAX_FILES, stream_bytes=STREAM_BYTES),
            limitations=['No compiler/export/download, renderer, physical input or audio observation.',
                'Original supplied bundle remains available; only the test-owned initial copy is absent after relocation.',
                'Direct-process closure only; descendant-tree facts require the external admitted owner.',
                'Complete save equality is required without projection or tolerances for this supplied bundle.',
                'No borrowed stable runtime-session profile qualification or complete Alpha claim.'])
        self.live = []

    def check(self, label, value):
        need(value, label)
        self.report['checks'].append(label)

    def budget(self):
        need(time.monotonic() < self.until, 'Fixture admission deadline')
        return inventory(self.output, hashes=False, cap=OUTPUT_BYTES-8*MIB)

    def timeout(self):
        self.budget()
        return max(.001, min(self.args.timeout, self.until-time.monotonic()))

    def command(self, label, arguments, *, data=b'', exit_code=0):
        """File-backed finite subprocess; original streams survive every error."""
        folder = self.output/'commands'/label
        folder.mkdir()
        (folder/'stdin.bin').write_bytes(data)
        command = [str(self.binary), *map(str, arguments)]
        row = dict(label=label, command=command, forced_cleanup=False)
        self.report['commands'].append(row)
        limit = time.monotonic() + self.timeout()
        with (folder/'stdin.bin').open('rb') as stdin, (folder/'stdout.bin').open('xb') as stdout, (folder/'stderr.bin').open('xb') as stderr:
            process = subprocess.Popen(command, stdin=stdin, stdout=stdout, stderr=stderr,
                cwd=self.output, shell=False, start_new_session=os.name == 'posix')
            row['process_id'] = process.pid
            try:
                while process.poll() is None:
                    need(time.monotonic() < limit, 'Owned command deadline: ' + label)
                    need(all(p.stat().st_size <= STREAM_BYTES for p in
                         (folder/'stdout.bin', folder/'stderr.bin')), 'Raw command stream cap')
                    try:
                        process.wait(timeout=min(.2, max(.001, limit-time.monotonic())))
                    except subprocess.TimeoutExpired:
                        pass
            finally:
                if process.poll() is None:
                    row['forced_cleanup'] = True
                    process.kill()
                process.wait(timeout=10)
                row['exit_code'] = process.returncode
        row['streams'] = {p.name: identity(p) for p in folder.iterdir()}
        self.check(label + ' direct child reaped without forced cleanup',
                   row['exit_code'] == exit_code and not row['forced_cleanup'])
        self.budget()
        return (folder/'stdout.bin').read_bytes()

    def cli(self, label, arguments, *, exit_code=0, diagnostic=None):
        value = strict(self.command(label, arguments, exit_code=exit_code))
        self.check(label + ' command envelope', value['status'] == ('ok' if exit_code == 0 else 'error'))
        if exit_code:
            self.check(label + ' original diagnostic', type(value.get('diagnostics')) is list and
                bool(value['diagnostics']) and (diagnostic is None or any(
                    diagnostic in str(row.get('message', '')) for row in value['diagnostics'])))
        return value

    def open(self, label, *, command_factory=False):
        folder = self.output/'owners'/label
        folder.mkdir()
        options = dict(save_root=self.output/'saves',
            save_write_mode='synchronous', cwd=self.output, close_timeout=10,
            trace_directory=folder/'wire', trace_limit_bytes=STREAM_BYTES)
        client = (WorldClient.open_game_command([str(self.binary)], self.relocated/'game.json', **options)
                  if command_factory else WorldClient.open_game(self.binary, self.relocated/'game.json', **options))
        process = client.transport._process
        row = dict(label=label, process_id=client.transport.process_id,
                   factory='open_game_command' if command_factory else 'open_game',
                   command=list(process.args), cleanup_actions=[], trace=str(folder/'wire'))
        # Observe actual direct-child fallback calls without changing their behavior.
        for name in ('terminate', 'kill'):
            original = getattr(process, name)
            def observe(action=name, method=original):
                row['cleanup_actions'].append(action)
                return method()
            setattr(process, name, observe)
        self.live.append((client, row))
        self.report['owners'].append(row)
        return client

    def close(self, client, reason):
        row = next(row for actual, row in self.live if actual is client)
        if row.get('closed'):
            return
        try:
            client.close()
        finally:
            row.update(close_reason=reason, closed=client.closed,
                       exit_code=client.transport.returncode,
                       cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),
                       stderr_truncated=client.transport.stderr_truncated)
            row['trace_inventory'] = inventory(Path(row['trace']), cap=3*STREAM_BYTES, maximum=3)
        self.check(row['label'] + ' natural direct closure', row['closed'] and row['exit_code'] == 0 and
            row['cleanup_actions'] == [] and row['cleanup_error'] is None and not row['stderr_truncated'])
        streams = []
        for name in ('stdin.ndjson', 'stdout.ndjson'):
            raw = (Path(row['trace'])/name).read_bytes()
            need(not raw or raw.endswith(b'\n'), 'Complete terminal raw JSONL frame')
            streams.append([strict(line) for line in raw.splitlines()])
        requests, replies = streams
        need(len(requests) == len(replies), 'Complete original RPC associations')
        seen = set()
        for request, reply in zip(requests, replies):
            key = type(request['id']), request['id']
            need(key not in seen and key[0] in (int, str) and
                 type(reply['id']) is key[0] and reply['id'] == key[1] and
                 request['jsonrpc'] == reply['jsonrpc'] == '2.0' and
                 ('result' in reply) != ('error' in reply), 'Original raw RPC ID/types')
            seen.add(key)
        row['complete_raw_pairs'] = len(requests)

    def call(self, client, method, params=None, *, error=None):
        need(len(self.report['calls']) < MAX_RPC, 'RPC count cap')
        row = dict(process_id=client.transport.process_id, method=method,
                   params=copy.deepcopy({} if params is None else params), before_ns=time.monotonic_ns())
        self.report['calls'].append(row)
        try:
            result = client.call(method, params, timeout=self.timeout())
        except RpcError as failure:
            row.update(after_ns=time.monotonic_ns(), error=dict(code=failure.code,
                       message=failure.message, data=failure.data))
            need(type(error) is int and failure.code == error, 'Unexpected native rejection: ' + str(row))
            return row['error']
        row.update(after_ns=time.monotonic_ns(), result=result)
        need(error is None, 'Expected native rejection: ' + str(row))
        self.budget()
        return result

    def state(self, client):
        status = self.call(client, 'runtime.status')
        need(status['active'] and type(status['tick']) is int, 'Active native runtime required')
        session, tick = status['session_id'], status['tick']
        runtime = self.call(client, 'runtime.inspect', dict(session_id=session))
        game = self.call(client, 'runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        components = self.call(client, 'runtime.components', dict(session_id=session))
        self.check('Joined reads retain the captured runtime identity and tick',
            all(row['session_id'] == session and type(row['tick']) is int and
                row['tick'] == tick for row in (runtime, game, components)) and
            components['structure_revision'] == runtime['structure_revision'])
        guards = dict(expected_tick=tick, expected_gameplay_revision=game['revision'],
            expected_structure_revision=runtime['structure_revision'],
            expected_material_revision=runtime['material_revision'],
            expected_ui_revision=runtime['ui_revision'], expected_control_sequence=runtime['control_sequence'],
            expected_component_revision=components['component_revision'])
        return status, runtime, game, guards

    def initial(self, client):
        catalog = self.call(client, 'world.describe')
        availability = catalog['runtime_game_session_contract']['availability']
        self.check('Standalone readonly discovery without borrowed qualification',
            catalog['mode'] == 'read_only_runtime' and catalog['read_only'] is True and
            catalog['runtime_available'] is True and 'session_scope' not in catalog and
            availability['profile'] is None and availability['qualified'] is False and
            availability['reason'] == ('unqualified_context' if os.name == 'nt' else 'unqualified_target'))
        status, runtime, game, guards = self.state(client)
        self.check('Native tick0 and no automatic player', status['tick'] == 0 and
            self.call(client, 'player.inspect')['active'] is False)
        self.check('Actual active backend matches bundled gameplay presence',
            (game['module'] is None) == ('gameplay' not in self.manifest))
        if game['module'] is not None:
            descriptor_path = relative(self.relocated, self.manifest['gameplay']['descriptor'])
            descriptor = strict(descriptor_path.read_bytes())
            library = relative(descriptor_path.parent, descriptor['library'])
            self.check('Bundled native gameplay image and full schema',
                game['module']['backend'] == 'native_aot' and
                game['module']['assembly_sha256'] == identity(library)['sha256'] and
                exact(game['module']['schema'], descriptor['schema']))
        return status, runtime, game, guards

    def checkpoint(self, client, label):
        used = sum(row['bytes'] for row in self.budget().values())
        need(used + 2*64*MIB <= OUTPUT_BYTES-8*MIB,
             'Reserve one maximum native save plus its complete retained copy')
        status, runtime, game, guards = self.state(client)
        written = self.call(client, 'save.write', dict(request_id=uuid.uuid4().hex,
            configuration_generation=1, slot=label, expected_generation=0,
            session_id=status['session_id'], **guards))
        files = list((self.output/'saves'/('slot-'+label)).glob('*.bin'))
        need(len(files) == 1, 'Exactly one original payload in fresh slot')
        raw = files[0].read_bytes()
        need(len(raw) <= 64*MIB and identity(files[0]) ==
             dict(bytes=written['bytes'], sha256=written['sha256']), 'Save receipt/raw identity')
        document = strict(raw)
        self.check(label + ' complete native archive envelope',
            set(document) == {'format', 'version', 'document', 'snapshot'} and
            document['format'] == 'poima.world-save' and type(document['version']) is int and
            document['version'] == 1 and set(document['snapshot']) == {'format', 'version', 'sha256', 'payload'} and
            document['snapshot']['payload']['tick'] == status['tick'])
        target = self.output/'checkpoints'/(label+'.bin')
        target.write_bytes(raw)
        self.report['checkpoints'][label] = dict(file=str(target), identity=identity(target),
            slot=label, generation=written['generation'], tick=status['tick'],
            payload_members=sorted(document['snapshot']['payload']))
        return label

    def pair(self, left, right):
        a, b = (Path(self.report['checkpoints'][label]['file']) for label in (left, right))
        self.check(left + ' / ' + right + ' full typed and byte equality',
                   a.read_bytes() == b.read_bytes() and exact(strict(a.read_bytes()), strict(b.read_bytes())))
        self.report['whole_state_pairs'].append(dict(left=left, right=right, exclusions=[]))

    def load(self, client, label):
        status, runtime, game, guards = self.state(client)
        fresh = uuid.uuid4().hex
        result = self.call(client, 'save.load', dict(request_id=uuid.uuid4().hex,
            configuration_generation=1, slot=label, expected_generation=1,
            revision=self.manifest['source']['revision'], expected_session_id=status['session_id'],
            new_session_id=fresh, **guards))
        self.check(label + ' actual runtime replacement', result['session_id'] == fresh and
                   result['tick'] == self.report['checkpoints'][label]['tick'])

    def step(self, client, ticks):
        status = self.call(client, 'runtime.status')
        runtime = self.call(client, 'runtime.inspect', dict(session_id=status['session_id']))
        self.check('Step preparation retains captured runtime identity and tick',
            runtime['session_id'] == status['session_id'] and type(runtime['tick']) is int and
            runtime['tick'] == status['tick'])
        request = dict(session_id=status['session_id'], request_id=uuid.uuid4().hex,
            expected_tick=status['tick'], expected_structure_revision=runtime['structure_revision'],
            ticks=ticks, inputs=[dict(entity=self.manifest['entry']['controller'], move=[.25, .5], look=[1, 0])])
        result = self.call(client, 'runtime.step', request)
        self.check('Exact native step completion without replacement', result['current_session_id'] ==
            status['session_id'] and result['current_tick'] == status['tick']+ticks and
            result['runtime_replaced'] is False and result['replayed'] is False)
        return request, result

    def rejection_cases(self):
        variant = self.output/'Negative bundle'
        need(sum(row['bytes'] for row in self.budget().values()) +
             sum(row['bytes'] for row in self.input_inventory.values()) <= OUTPUT_BYTES-8*MIB,
             'Reserve complete second derived bundle before copying')
        shutil.copytree(self.relocated, variant)
        baseline = inventory(variant, cap=BUNDLE_BYTES, maximum=4097)
        game_file = variant/'game.json'
        base = ['game', 'stdio', game_file]
        self.cli('unknown-flag', [*base, '--unknown', 'value'], exit_code=2)
        self.cli('unsupported-framing-flag', [*base, '--framing', 'content-length'], exit_code=2)
        self.cli('internal-save-root', [*base, '--save-root', variant/'runtime'], exit_code=4,
                 diagnostic='immutable')
        chosen = next(row for row in self.manifest['files'] if 0 < row['size'] <= MIB)
        path = relative(variant, chosen['path'])
        original = path.read_bytes()
        corrupt = bytes([original[0] ^ 1]) + original[1:]
        (self.output/'mutations/corrupt-payload.bin').write_bytes(corrupt)
        self.report['negative_mutations'] = dict(corrupt_payload=dict(path=chosen['path'],
            original=self.input_inventory[chosen['path']],
            changed=identity(self.output/'mutations/corrupt-payload.bin')),
            extra_file=dict(path='unexpected-fixture-file', bytes_hex=b'not declared\n'.hex()),
            unsupported_target=dict(field='engine.target_arch/runtime.target_arch', value='arm64',
                scope='Unsupported target metadata with updated runtime-payload inventory hash; no foreign binary execution.'))
        try:
            path.write_bytes(corrupt)
            self.cli('corrupt-payload', base, exit_code=4, diagnostic='Bundle payload changed')
        finally:
            path.write_bytes(original)
        extra = variant/'unexpected-fixture-file'
        try:
            extra.write_bytes(b'not declared\n')
            self.cli('extra-file', base, exit_code=4, diagnostic='extra files')
        finally:
            extra.unlink()
        # Negative unsupported-target metadata, not an invented foreign binary.
        runtime_file = variant/'runtime/runtime.json'
        old_runtime, old_game = runtime_file.read_bytes(), game_file.read_bytes()
        target_runtime, target_game = strict(old_runtime), strict(old_game)
        target_runtime['target_arch'] = target_game['engine']['target_arch'] = 'arm64'
        raw = (json.dumps(target_runtime, indent=2)+'\n').encode()
        for row in target_game['files']:
            if row['path'] == 'runtime/runtime.json':
                row.update(size=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        write(self.output/'mutations/wrong-target-game.json', target_game)
        (self.output/'mutations/wrong-target-runtime.json').write_bytes(raw)
        try:
            runtime_file.write_bytes(raw)
            game_file.write_text(json.dumps(target_game, indent=2)+'\n', encoding='utf-8')
            self.cli('unsupported-target', base, exit_code=4, diagnostic='target')
        finally:
            runtime_file.write_bytes(old_runtime)
            game_file.write_bytes(old_game)
        self.check('Only owned negative mutations restored, complete bundle intact',
                   inventory(variant, cap=BUNDLE_BYTES, maximum=4097) == baseline)

    def exercise(self):
        inspected = self.cli('supplied-inspection', ['game', 'inspect', self.game])['result']
        self.check('Actual supplied bundle inspection', exact(inspected['entry'], self.manifest['entry']) and
            exact(inspected['files'], self.manifest['files']))
        initial = self.output/'Initial game copy'
        shutil.copytree(self.game.parent, initial)
        self.check('Derived initial copy complete', inventory(initial, cap=BUNDLE_BYTES, maximum=4097) == self.input_inventory)
        self.relocated = self.output/'Relocated game'
        initial.rename(self.relocated)
        self.check('Owned initial location absent after relocation', not initial.exists() and
            inventory(self.relocated, cap=BUNDLE_BYTES, maximum=4097) == self.input_inventory)
        self.cli('relocated-inspection', ['game', 'inspect', self.relocated/'game.json'])
        warm = self.open('warm')
        status, runtime, game, guards = self.initial(warm)
        before = self.checkpoint(warm, 'guards-before')
        self.call(warm, 'world.transact', {}, error=-32081)
        self.call(warm, 'development.publish', {}, error=-32081)
        self.call(warm, 'host.shutdown', {}, error=-32080)
        self.call(warm, 'runtime.ui.inspect', dict(session_id=status['session_id'], tick=1), error=-32009)
        self.call(warm, 'save.configure', dict(request_id=uuid.uuid4().hex,
            expected_generation=1, root=str(self.relocated)), error=-32070)
        self.pair(before, self.checkpoint(warm, 'guards-after'))
        ui = self.call(warm, 'runtime.ui.inspect', dict(session_id=status['session_id'], tick=0, limit=256))
        self.report['initial_ui'] = ui
        if self.args.ui_id is not None:
            self.check('Selected actual compiled UI control exists and is eligible', game['module'] is not None and
                any(row['id'] == self.args.ui_id and row['eligible'] is True for row in ui['elements']))
            accepted = self.call(warm, 'runtime.ui.activate', dict(session_id=status['session_id'],
                request_id=uuid.uuid4().hex, id=self.args.ui_id, **{k:v for k,v in guards.items()
                    if k not in ('expected_component_revision', 'expected_material_revision')}))
            self.check('Actual selected UI Control without Save/Load replacement',
                accepted['runtime_replaced'] is False and accepted['save_serviced'] is False)
        request, accepted = self.step(warm, self.args.steps)
        checkpoint = self.checkpoint(warm, 'checkpoint')
        self.check('Exact native step receipt replay', exact(self.call(warm, 'runtime.step', request),
                   dict(accepted, replayed=True)))
        self.call(warm, 'runtime.step', dict(request, ticks=request['ticks']+1), error=-32010)
        self.call(warm, 'runtime.step', dict(request, request_id=uuid.uuid4().hex), error=-32009)
        self.pair(checkpoint, self.checkpoint(warm, 'retries-after'))
        self.step(warm, 2)
        future = self.checkpoint(warm, 'warm-future')
        self.load(warm, checkpoint)
        self.pair(checkpoint, self.checkpoint(warm, 'warm-restored'))
        self.check('Dedicated session.close accepted', self.call(warm, 'session.close')['closed'] is True)
        self.close(warm, 'session.close followed by SDK reap')
        cold = self.open('cold', command_factory=True)
        self.initial(cold)
        self.load(cold, checkpoint)
        self.pair(checkpoint, self.checkpoint(cold, 'cold-restored'))
        self.step(cold, 2)
        self.pair(future, self.checkpoint(cold, 'cold-future'))
        self.close(cold, 'SDK close sends EOF')
        class CallerFailure(Exception):
            pass
        exception_owner = self.open('caller-exception')
        try:
            with exception_owner:
                self.call(exception_owner, 'runtime.status')
                raise CallerFailure('Fixture caller failure; native shutdown must still complete')
        except CallerFailure:
            self.report['caller_exception_preserved'] = True
        self.close(exception_owner, 'context manager propagated caller exception')
        malformed = b'{' + b' '*(MIB+1) + b'\nContent-Length: 2\n'
        requests = [dict(jsonrpc='2.0', id=1, method='runtime.status', params={}),
                    dict(jsonrpc='2.0', id=2, method='session.close', params={})]
        wire = malformed + b''.join((json.dumps(row, separators=(',', ':'))+'\n').encode() for row in requests)
        raw = self.command('native-ndjson-framing', ['game', 'stdio', self.relocated/'game.json'], data=wire)
        frames = [strict(line) for line in raw.splitlines()]
        self.check('Bounded NDJSON rejects oversized/header frames, then remains usable', raw.endswith(b'\n') and len(frames) == 4 and
            all(frame['id'] is None and frame['error']['code'] == -32700 for frame in frames[:2]) and
            type(frames[2]['id']) is int and frames[2]['id'] == 1 and frames[2]['result']['tick'] == 0 and
            type(frames[3]['id']) is int and frames[3]['id'] == 2 and frames[3]['result']['closed'] is True)
        self.rejection_cases()
        self.check('Complete relocated bundle unchanged after all owners',
                   inventory(self.relocated, cap=BUNDLE_BYTES, maximum=4097) == self.input_inventory)

    def finish(self):
        for client, row in reversed(self.live):
            try:
                self.close(client, row.get('close_reason', 'failure cleanup'))
            except BaseException:
                self.report['cleanup_errors'].append(traceback.format_exc())
        try:
            self.check('Original supplied bundle and executable unchanged',
                inventory(self.game.parent, cap=BUNDLE_BYTES, maximum=4097) == self.input_inventory and
                identity(self.binary) == self.binary_identity)
            self.check('Selected public fixture/SDK sources unchanged',
                       all(identity(Path(path)) == pin for path, pin in self.source_pins.items()))
            self.report['observed_output'] = self.budget()
        except BaseException:
            self.report['errors'].append(traceback.format_exc())
        self.report['passed'] = not self.report['errors'] and not self.report['cleanup_errors']
        self.report['seconds'] = time.monotonic()-self.started
        write(self.output/'report.json', self.report)
        # The report itself is included in the final observed bound.
        inventory(self.output, hashes=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'game', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--ui-id', help='Optional exact eligible native Control button; no Save/Load intent')
    parser.add_argument('--steps', type=int, default=4)
    parser.add_argument('--seconds', type=int, default=240)
    parser.add_argument('--timeout', type=float, default=20)
    args = parser.parse_args()
    need(not sys.flags.optimize and 1 <= args.steps <= 60 and 60 <= args.seconds <= 600 and
         math.isfinite(args.timeout) and 1 <= args.timeout <= 30, 'Fixture bounds/nonoptimized Python')
    need(args.ui_id is None or re.fullmatch('[0-9a-f]{32}', args.ui_id), 'Exact lowercase UI ID')
    contract = Contract(args)
    try:
        contract.exercise()
    except BaseException:
        contract.report['errors'].append(traceback.format_exc())
    finally:
        contract.finish()
    print(json.dumps(dict(passed=contract.report['passed'], seconds=contract.report['seconds'],
                         report=str(contract.output/'report.json'))))
    return 0 if contract.report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
