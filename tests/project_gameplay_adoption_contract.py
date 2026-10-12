#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual supplied NativeAOT project adoption, guarded retry and file recovery.

Builds nothing. An external process-tree owner must bound native invocations.
Journal-boundary cases synthesize durable file states and prove recovery from
those states; they do not simulate or claim an operating-system crash.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
FILE_CAP = 256 * 1048576
EVIDENCE_CAP = 64 * 1048576
OUTPUT_CAP = 512 * 1048576
LOG_CAP = 8 * 1048576
ROW_CAP = 2000


def need(value, message):
    if not value:
        raise AssertionError(message)


def fresh():
    return uuid.uuid4().hex


def io_path(path):
    # Extended paths apply only to Python file I/O. Native selectors, request
    # identities and inventory labels retain their ordinary logical spelling.
    path = Path(path)
    if os.name != 'nt':return path
    text = os.path.abspath(os.fspath(path))
    if text.startswith('\\\\?\\'):return Path(text)
    if text.startswith('\\\\'):return Path('\\\\?\\UNC\\' + text[2:])
    return Path('\\\\?\\' + text)


def sha(path):
    with io_path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def identity(path):
    logical = Path(path);path = io_path(logical)
    need(path.is_file() and not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(),
         'Regular file required: ' + str(logical))
    need(path.stat().st_size <= FILE_CAP, 'Selected file exceeds256MiB')
    return dict(bytes=path.stat().st_size, sha256=sha(path))


def exact(left, right):
    return json.dumps(left, sort_keys=True, separators=(',', ':'), allow_nan=False) == json.dumps(
        right, sort_keys=True, separators=(',', ':'), allow_nan=False)


def read(path):
    need(io_path(path).stat().st_size <= EVIDENCE_CAP, 'Bounded JSON file required')
    return json.loads(io_path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    io_path(path).write_bytes((json.dumps(value, indent=2, allow_nan=False) + '\n').encode('utf-8'))


def inventory(directory):
    # Complete directories and link identities are part of the oracle. Linked
    # trees are never traversed; ordinary files retain both bytes and SHA.
    directory = Path(directory)
    result = {}
    def visit(current):
        for entry in sorted(io_path(current).iterdir()):
            path = current/entry.name
            label = path.relative_to(directory).as_posix()
            linked = entry.is_symlink() or getattr(entry, 'is_junction', lambda: False)()
            if linked:
                result[label] = dict(kind='link', target=os.readlink(entry))
            elif entry.is_dir():
                result[label] = dict(kind='directory');visit(path)
            else:
                result[label] = dict(kind='file', **identity(path))
    visit(directory)
    return result


def copy_tree(source, destination):
    need(not io_path(destination).exists(), 'Fresh copy destination required')
    before = inventory(source)
    need(all(row['kind'] != 'link' for row in before.values()), 'Supply an unlinked frozen input tree')
    shutil.copytree(io_path(source), io_path(destination))
    need(inventory(destination) == before and inventory(source) == before, 'Independent copy changed input bytes')
    return before


class Owner:
    def __init__(self, args, evidence, world, label, *, mcp=False, profiles=None):
        need(time.monotonic() < args.deadline, 'Fixture deadline expired before native owner')
        self.args, self.evidence, self.label = args, evidence, label
        self.queue = queue.Queue();self.counter = 0;self.closed = False;self.stderr_bytes = 0
        self.logs = args.output / ('owner-' + label);self.logs.mkdir()
        self.stdout_log = (self.logs / 'stdout.jsonl').open('xb')
        self.stderr_log = (self.logs / 'stderr.txt').open('xb')
        command = [str(args.binary), 'mcp', '--world', str(world)] if mcp else [str(args.binary), 'world', str(world)]
        if profiles is not None:
            command += ['--development-profiles', str(profiles)]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.row = dict(label=label, command=command, exit_code=None, forced_cleanup=False)
        evidence['owners'].append(self.row)
        def output():
            try:
                while True:
                    line = self.process.stdout.readline(4 * 1048576 + 1)
                    if not line:break
                    self.stdout_log.write(line);self.stdout_log.flush()
                    if len(line) > 4 * 1048576 or not line.endswith(b'\n'):
                        self.queue.put(RuntimeError('Native stdout frame exceeds4MiB or is incomplete'));break
                    self.queue.put(line)
            finally:self.queue.put(None)
        def error():
            while True:
                chunk = self.process.stderr.read(65536)
                if not chunk:break
                self.stderr_bytes += len(chunk)
                if self.stderr_bytes <= LOG_CAP:self.stderr_log.write(chunk)
        self.output_thread = threading.Thread(target=output, daemon=True);self.output_thread.start()
        self.error_thread = threading.Thread(target=error, daemon=True);self.error_thread.start()
        if mcp:
            self.rpc('initialize', dict(protocolVersion='2025-11-25', capabilities={}, clientInfo=dict(name='adoption-contract', version='1')))
            self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n');self.process.stdin.flush()

    def rpc(self, method, params=None, *, error=None):
        need(time.monotonic() < self.args.deadline and len(self.evidence['calls']) < ROW_CAP, 'Bounded RPC admission failed')
        self.counter += 1
        request = dict(jsonrpc='2.0', id=self.counter, method=method, params=copy.deepcopy(params or {}))
        self.evidence['transport'].append(dict(owner=self.label, direction='request', message=request))
        self.process.stdin.write((json.dumps(request, allow_nan=False) + '\n').encode());self.process.stdin.flush()
        line = self.queue.get(timeout=min(30, max(.001, self.args.deadline - time.monotonic())))
        if isinstance(line, BaseException):raise line
        need(line is not None, 'Native owner ended before reply')
        response = json.loads(line);need(response.get('id') == request['id'] and response.get('jsonrpc') == '2.0', 'Exact RPC correlation required')
        self.evidence['transport'].append(dict(owner=self.label, direction='response', message=response))
        row = dict(owner=self.label, rpc_id=self.counter, method=method, params=request['params'])
        row.update(error=response['error']) if 'error' in response else row.update(result=response['result'])
        self.evidence['calls'].append(row)
        need(time.monotonic() <= self.args.deadline and len(json.dumps(self.evidence, allow_nan=False).encode()) <= EVIDENCE_CAP,
             'RPC evidence/deadline bound exceeded')
        if error is not None:
            need('error' in response and (error is True or response['error']['code'] == error), 'Expected native error differs')
            return response['error']
        need('result' in response and 'error' not in response, 'Unexpected native rejection: ' + str(response.get('error')))
        return response['result']

    def tool(self, method, params, *, build=False, error=None):
        value = self.rpc('tools/call', dict(name='poima_build' if build else 'poima_call', arguments=dict(method=method, params=params)))
        data = value['structuredContent']
        need(exact(json.loads(value['content'][0]['text']), data), 'MCP textual/structured reply differs')
        if error is not None:
            need(value['isError'] is True and 'error' in data and (error is True or data['error']['code'] == error), 'MCP expected rejection differs')
            return data['error']
        need(value['isError'] is False and 'result' in data, 'MCP operation failed')
        return data['result']

    def close(self):
        if self.closed:return
        self.closed = True
        try:
            self.process.stdin.close()
            try:self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.row['forced_cleanup'] = True;self.process.kill();self.process.wait(timeout=5)
            self.output_thread.join(timeout=3);self.error_thread.join(timeout=3)
            need(not self.output_thread.is_alive() and not self.error_thread.is_alive(), 'Native log readers did not close')
            self.row['exit_code'] = self.process.returncode
        finally:
            self.stdout_log.close();self.stderr_log.close()
            self.row.update(stdout=identity(self.logs/'stdout.jsonl'), stderr=identity(self.logs/'stderr.txt'))
        need(self.row['exit_code'] == 0 and not self.row['forced_cleanup'] and self.stderr_bytes == 0,
             'Native owner did not exit naturally without stderr')


class Harness:
    def __init__(self, args, evidence):
        self.args, self.evidence = args, evidence;self.sequence = 0;self.owners = []

    def cli(self, label, arguments, *, success=True, detail=None):
        need(time.monotonic() < self.args.deadline, 'CLI admission deadline')
        self.sequence += 1;folder = self.args.output / ('cli-' + str(self.sequence) + '-' + label);io_path(folder).mkdir()
        stdout, stderr = folder/'stdout.json', folder/'stderr.txt'
        row = dict(label=label, command=[str(self.args.binary), *map(str, arguments)], exit_code=None, forced_cleanup=False)
        self.evidence['commands'].append(row)
        with stdout.open('xb') as out, stderr.open('xb') as err:
            child = subprocess.Popen(row['command'], stdout=out, stderr=err)
            try:
                while child.poll() is None:
                    need(time.monotonic() < self.args.deadline and stdout.stat().st_size + stderr.stat().st_size <= LOG_CAP, 'CLI deadline/log bound')
                    time.sleep(.025)
            except BaseException:
                row['forced_cleanup'] = True;child.kill();child.wait(timeout=5);raise
        row.update(exit_code=child.returncode, stdout=identity(stdout), stderr=identity(stderr))
        need(row['stdout']['bytes'] + row['stderr']['bytes'] <= LOG_CAP and (child.returncode == 0) == success and time.monotonic() < self.args.deadline,
             'Actual CLI exit/log contract differs')
        reply = read(stdout);row['reply'] = reply
        need(reply['status'] == ('ok' if success else 'error'), 'Actual CLI status differs')
        if detail is not None:need(detail in json.dumps(reply['diagnostics']), 'CLI failed for an unrelated reason')
        if not success:
            need(type(reply['diagnostics'][0]['error_code']) is int, 'Native adoption rejection lacks typed error code')
        return reply['result'] if success else reply

    def owner(self, world, label, **kwargs):
        owner = Owner(self.args, self.evidence, world, label, **kwargs);self.owners.append(owner);return owner

    def finish(self):
        errors = []
        for owner in reversed(self.owners):
            try:owner.close()
            except BaseException:errors.append(traceback.format_exc())
        self.evidence['cleanup_errors'] = errors
        need(not errors, 'Native owners failed cleanup')


    def inspect(self, manifest):
        before = inventory(manifest.parent)
        result = self.cli('inspect', ['project', 'inspect', manifest])
        need(inventory(manifest.parent) == before and result['manifest_sha256'] == sha(manifest),
             'Read-only inspection or complete manifest authority differs')
        world = manifest.parent / read(manifest)['entry']['world']
        need(result['world_sha256'] == sha(world), 'Inspection world authority differs')
        return result

    def adopt(self, manifest, artifact, request, *, success=True, label='adopt', error_code=None):
        request_path = self.args.output / ('request-' + fresh() + '.json');write(request_path, request)
        reply = self.cli(label, ['project', 'adopt-gameplay', manifest, '--artifact', artifact, '--request', request_path], success=success)
        if error_code is not None:need(not success and reply['diagnostics'][0]['error_code'] == error_code, 'Expected exact adoption error code differs')
        return reply

    def project(self, label, *, starter=False):
        directory = self.args.output / 'projects' / label
        self.cli('create-' + label, ['project', 'create', directory, '--name', 'Adoption ' + label])
        manifest = directory / 'project.json';spec = read(manifest)
        if self.args.prepared_world is not None and not starter:
            world = directory / spec['entry']['world']
            world.rename(directory / 'retained-starter-world.json')
            shutil.copy2(self.args.prepared_world, world)
            source_assets = Path(str(self.args.prepared_world) + '.assets')
            if source_assets.exists():copy_tree(source_assets, Path(str(world) + '.assets'))
            spec['entry']['controller'] = f'{100:032x}';spec['entry']['camera'] = f'{101:032x}'
            write(manifest, spec)
        else:
            world = directory / spec['entry']['world']
            owner = self.owner(world, 'schemas-' + label)
            initial = owner.rpc('world.inspect')
            owner.rpc('component.schema.import', dict(request_id=fresh(), base_revision=initial['revision'],
                      manifest=dict(format='poima.components', version=1, schemas=self.args.descriptor['schema'].get('components', []))))
            owner.close()
        self.inspect(manifest)
        return manifest


def request_for(manifest, artifact, *, values=None):
    result = dict(request_id=fresh(), expected_project_sha256=sha(manifest), expected_artifact_sha256=sha(artifact))
    if values is not None:result['values'] = values
    return result


def admitted_project(manifest, descriptor, values, before, result, *, replayed=False, attached=True):
    need(result['replayed'] is replayed and result['attached'] is attached and result['current_manifest_sha256'] == sha(manifest),
         'Durable historical/current adoption receipt differs')
    after = inventory(manifest.parent)
    for name, pin in before.items():
        if name != manifest.name:need(after.get(name) == pin, 'Adoption changed or removed an original project/world/input member: ' + name)
    document = read(manifest)
    need(document['format'] == 'poima.project' and document['version'] == 2 and
         document['gameplay']['values'] == values, 'Adopted project version or typed values differ')
    relative = document['gameplay']['descriptor']
    need(isinstance(relative, str) and ':' not in relative and not relative.startswith('/') and
         all(part not in ('', '.', '..') for part in relative.split('/')), 'Unsafe adopted descriptor path')
    copied = manifest.parent / relative
    need(copied.resolve().is_relative_to(manifest.parent.resolve()) and identity(copied) == identity(descriptor),
         'Copied NativeAOT descriptor identity differs')
    expected = {descriptor.name: identity(descriptor)}
    for member in read(descriptor)['files']:
        expected[member['path'].replace('\\', '/')] = dict(bytes=member['size'], sha256=member['sha256'])
    copied_inventory = inventory(copied.parent)
    expected_tree = {name:dict(kind='file', **pin) for name,pin in expected.items()}
    for name in expected:
        for parent in Path(name).parents:
            if parent != Path('.'):expected_tree[parent.as_posix()] = dict(kind='directory')
    need(copied_inventory == expected_tree, 'Copied complete gameplay file/directory inventory differs')
    need(relative == 'gameplay/adopt-'+result['request_id']+'/native-gameplay.json', 'Adopted descriptor is not request-owned')
    journal = read(manifest.parent/'.poima-gameplay-adoptions'/(result['request_id']+'.json'))
    need(journal['payload']['state'] == 'succeeded' and
         hashlib.sha256(journal['payload']['before_manifest'].encode()).hexdigest() == before[manifest.name]['sha256'],
         'Native before-manifest authority differs from independent complete inventory')
    original = json.loads(journal['payload']['before_manifest']);candidate = copy.deepcopy(original)
    candidate['version'] = 2;candidate['gameplay'] = dict(descriptor=relative, values=values)
    need(exact(candidate, document) and result['previous_manifest_sha256'] == before[manifest.name]['sha256'] and
         result['manifest_sha256'] == sha(manifest) and result['artifact_descriptor_sha256'] == sha(descriptor),
         'Adoption changed unrelated project fields or stable receipt identities')
    allowed = {'.poima-gameplay-adoptions', '.poima-gameplay-adoptions.lock',
               '.poima-gameplay-adoptions/'+result['request_id']+'.json',
               '.poima-gameplay-adoption-staging', '.poima-gameplay-adoption-staging/'+result['request_id']}
    prefix = copied.parent.relative_to(manifest.parent).as_posix()
    allowed.update(prefix+'/'+name for name in expected_tree);allowed.add(prefix)
    allowed.update(parent.as_posix() for parent in Path(prefix).parents if parent != Path('.'))
    need(set(after)-set(before) <= allowed, 'Adoption introduced unowned project/control-plane members')
    return copied


def unchanged_failure(manifest, source, operation):
    project_before, source_before = inventory(manifest.parent), inventory(source.parent)
    operation()
    need(inventory(manifest.parent) == project_before and inventory(source.parent) == source_before,
         'Rejected adoption changed the complete project or artifact tree')


def native_links(harness, target, link):
    need(not link.exists(), 'Fresh linked-path fixture required')
    if os.name == 'nt':
        # Owned target/link names only. No global privilege/policy changes.
        need(not any(c in str(target) + str(link) for c in '&|<>^%\r\n"'), 'Unsafe owned junction path')
        path = harness.args.output / ('junction-' + fresh())
        with path.with_suffix('.stdout').open('xb') as out, path.with_suffix('.stderr').open('xb') as err:
            child = subprocess.Popen(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(target)], stdout=out, stderr=err)
            child.wait(timeout=min(10, max(.001, harness.args.deadline-time.monotonic())))
        need(child.returncode == 0 and link.is_junction(), 'Cannot create owned junction fixture')
        harness.evidence['links'].append(dict(path=str(link), target=str(target), stdout=identity(path.with_suffix('.stdout')), stderr=identity(path.with_suffix('.stderr'))))
    else:link.symlink_to(target, target_is_directory=True)


def cli_cases(harness, artifact):
    args, evidence = harness.args, harness.evidence
    manifest = harness.project('cli')
    before = inventory(manifest.parent);request = request_for(manifest, artifact, values={})
    result = harness.adopt(manifest, artifact, request)
    copied = admitted_project(manifest, artifact, {}, before, result)
    evidence['stages']['cli_adoption'] = dict(result=result, before=before, after=inventory(manifest.parent), descriptor=str(copied))
    stable = inventory(manifest.parent)
    retry = harness.adopt(manifest, artifact, request, label='exact-retry')
    need(retry['replayed'] is True and retry['attached'] is True and inventory(manifest.parent) == stable,
         'Exact retry changed files or lost its attachment')
    unchanged_failure(manifest, artifact, lambda: harness.adopt(manifest, artifact, dict(request, values={'Ticks': 1}), success=False, label='changed-retry', error_code=-32009))
    # Change the real authored world through a fresh native guarded transaction.
    spec = read(manifest);world = manifest.parent/spec['entry']['world']
    owner = harness.owner(world, 'ordinary-edit')
    current = owner.rpc('world.inspect')
    owner.rpc('world.transact', dict(request_id=fresh(), base_revision=current['revision'], ops=[dict(op='entity.rename', id=spec['entry']['controller'], name='Independent valid later edit')]))
    owner.close();harness.inspect(manifest)
    edited = inventory(manifest.parent);old_after_edit = harness.adopt(manifest, artifact, request, label='historical-after-world-edit')
    need(old_after_edit['replayed'] is True and old_after_edit['attached'] is True and old_after_edit['current_manifest_sha256'] == sha(manifest) and inventory(manifest.parent) == edited,
         'Historical retry after ordinary world edit changed current files')
    second_request = request_for(manifest, artifact, values={'Ticks': 1})
    second = harness.adopt(manifest, artifact, second_request, label='second-adoption')
    second_inventory = inventory(manifest.parent)
    older = harness.adopt(manifest, artifact, request, label='older-after-new-adoption')
    need(older['replayed'] is True and older['attached'] is False and older['current_manifest_sha256'] == sha(manifest) and inventory(manifest.parent) == second_inventory,
         'Older receipt reattached or overwrote the later adoption')
    evidence['stages']['historical_retries'] = dict(world_edited=True, original_request=request, after_edit=old_after_edit, second=second, older=older)
    # A retained rename makes the original artifact selector absent. A fresh CLI
    # process must replay from the durable copied closure without reading it.
    retained = artifact.parent.with_name(artifact.parent.name + '-retained')
    need(not retained.exists(), 'Fresh retained artifact path required');artifact.parent.rename(retained)
    need(not artifact.exists(), 'Original artifact path still exists')
    terminal = inventory(manifest.parent)
    replay = harness.adopt(manifest, artifact, second_request, label='source-absent-restart-retry')
    need(replay['replayed'] is True and replay['attached'] is True and inventory(manifest.parent) == terminal,
         'Fresh-process source-absent retry changed current project')
    evidence['stages']['source_absent_retry'] = dict(result=replay, original_path_absent=True, retained_inventory=inventory(retained))
    current_copy = manifest.parent/read(manifest)['gameplay']['descriptor'];payload = current_copy.parent/read(current_copy)['library']
    original_payload = payload.read_bytes();retained_payload = harness.args.output / ('retained-copied-payload-' + fresh() + '.bin')
    retained_payload.write_bytes(original_payload);changed = bytearray(original_payload);changed[-1] ^= 1;payload.write_bytes(changed)
    tampered = inventory(manifest.parent)
    harness.adopt(manifest, artifact, second_request, success=False, label='copied-payload-tamper', error_code=-32091)
    need(inventory(manifest.parent) == tampered, 'Rejected copied-payload tamper changed current files')
    evidence['checks'].append('CLI v1-to-v2 adoption, exact/historical/older receipts, source-absent fresh-process retry and copied-payload tamper rejection preserve complete inventories.')
    return retained/'native-gameplay.json'


def negative_cases(harness, source):
    fields = read(source)['schema']['fields'];integer_field = next(f['name'] for f in fields if f['kind'] == 'int32')
    for label, change, code in (
        ('stale-project', {'expected_project_sha256': '0'*64}, -32009),
        ('stale-artifact', {'expected_artifact_sha256': '0'*64}, -32009),
        ('typed-bool', {'values': {integer_field: True}}, -32602),
        ('unknown-value', {'values': {'UnknownIndependentField': 1}}, -32602),
    ):
        manifest = harness.project(label);request = dict(request_for(manifest, source), **change)
        unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, request, success=False, label=label, error_code=code))
    for label, change in (
        ('bad-request-id', {'request_id':'bad'}),
        ('nonobject-values', {'values':[]}),
        ('unexpected-request-field', {'IndependentUnknownRequestField':True}),
    ):
        manifest = harness.project(label);request = dict(request_for(manifest, source), **change)
        unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, request, success=False,
                             label=label, error_code=-32602))
    manifest = harness.project('omitted-values');before = inventory(manifest.parent)
    absent = request_for(manifest, source)
    result = harness.adopt(manifest, source, absent, label='omitted-values-adoption')
    admitted_project(manifest, source, {}, before, result)
    stable = inventory(manifest.parent)
    repeat = harness.adopt(manifest, source, absent, label='omitted-values-exact-retry')
    need(repeat['replayed'] is True and inventory(manifest.parent) == stable, 'Omitted-value exact retry differs')
    unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, dict(absent, values={}), success=False,
                         label='omitted-vs-explicit-retry', error_code=-32009))
    explicit = request_for(manifest, source, values={'Ticks':1})
    harness.adopt(manifest, source, explicit, label='typed-v2-values')
    carried = harness.adopt(manifest, source, request_for(manifest, source), label='v2-omitted-values')
    need(carried['values'] == {'Ticks':1} and read(manifest)['gameplay']['values'] == {'Ticks':1},
         'Omitted values did not retain the exact existing v2 values')
    for label, mutate in (
        ('schema-overlap', lambda d: d['schema']['fields'][1].update(offset=d['schema']['fields'][0]['offset'])),
        ('descriptor-extra', lambda d: d.update(unknown_independent_field=True)),
    ):
        tree = harness.args.output/'products'/label;copy_tree(source.parent, tree);descriptor = tree/source.name
        document = read(descriptor);mutate(document);write(descriptor, document)
        manifest = harness.project(label);request = request_for(manifest, descriptor)
        unchanged_failure(manifest, descriptor, lambda: harness.adopt(manifest, descriptor, request, success=False, label=label, error_code=-32091))
    tree = harness.args.output/'products'/'source-payload-tamper';copy_tree(source.parent, tree);descriptor = tree/source.name
    payload = tree/read(descriptor)['library'];original = payload.read_bytes();(harness.args.output/'retained-source-payload.bin').write_bytes(original)
    data = bytearray(original);data[-1] ^= 1;payload.write_bytes(data)
    manifest = harness.project('source-payload-tamper');request = request_for(manifest, descriptor)
    unchanged_failure(manifest, descriptor, lambda: harness.adopt(manifest, descriptor, request, success=False, label='source-payload-tamper', error_code=-32091))
    manifest = harness.project('linked-source');alias = harness.args.output/'products'/'linked-artifact';native_links(harness, source.parent, alias)
    request = request_for(manifest, source)
    unchanged_failure(manifest, source, lambda: harness.adopt(manifest, alias/source.name, request, success=False, label='linked-source', error_code=-32091))
    manifest = harness.project('missing-world-declaration', starter=True)
    document = read(manifest);owner = harness.owner(manifest.parent/document['entry']['world'], 'remove-unused-schema')
    current = owner.rpc('world.inspect');kind = harness.args.descriptor['schema']['components'][0]['id']
    owner.rpc('world.transact', dict(request_id=fresh(), base_revision=current['revision'],
                                   ops=[dict(op='component.schema.remove', id=kind)]))
    owner.close();request = request_for(manifest, source)
    unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, request, success=False,
                         label='missing-world-declaration', error_code=-32091))
    evidence = harness.evidence
    evidence['checks'].append('Stale hashes, typed/schema/unknown-field/source payload corruption and linked artifact paths reject without changing either complete input inventory.')


def profile_config(args):
    template = read(args.profile_template)
    selected = [p for p in template['profiles'] if p['name'] == args.profile]
    need(len(selected) == 1, 'Supply one known owner-selected profile')
    profile = copy.deepcopy(selected[0])
    profile.update(project_root=str(args.output/'projects'), output_root=str(args.output/'products'))
    need(profile['target_rid'] == ('win-x64' if os.name == 'nt' else 'linux-x64'), 'Profile must match native host')
    path = args.output/'profiles.json'
    write(path, dict(format='poima.development-profiles', version=1, profiles=[profile]))
    return path


def service_cases(harness, source, config):
    args, evidence = harness.args, harness.evidence
    for mcp, label in ((False, 'service'), (True, 'mcp')):
        manifest = harness.project(label);artifact_folder = args.output/'products'/('source-' + label)
        copy_tree(source.parent, artifact_folder);artifact = artifact_folder/source.name
        relative_project = manifest.relative_to(args.output/'projects').as_posix()
        relative_artifact = artifact.relative_to(args.output/'products').as_posix()
        world = args.output/('authority-' + label + '.json')
        owner = harness.owner(world, label, mcp=mcp, profiles=config)
        def call(method, params, *, mutate=False, error=None):
            return owner.tool(method, params, build=mutate, error=error) if mcp else owner.rpc(method, params, error=error)
        if mcp:
            names = {x['name'] for x in owner.rpc('tools/list')['tools']}
            need({'poima_call', 'poima_build', 'poima_discover'} <= names, 'MCP authorization tools missing')
        profiles = call('development.profiles', {})['profiles']
        selected = next(p for p in profiles if p['name'] == args.profile)
        need(selected['project_inspect'] is True and selected['gameplay_adopt'] is True and 'environment' not in selected,
             'Profile operation discovery or policy privacy differs')
        params = dict(profile=args.profile, project=relative_project, artifact=relative_artifact, **request_for(manifest, artifact, values={}))
        inspect_params = dict(profile=args.profile, project=relative_project)
        complete_before = inventory(manifest.parent)
        inspected = call('project.inspect', inspect_params)
        need(inspected['manifest_sha256'] == sha(manifest) and inventory(manifest.parent) == complete_before, 'Profile-scoped inspect mutated project')
        if mcp:
            owner.tool('project.gameplay.adopt', params, error=-32602)
            owner.tool('project.inspect', inspect_params, build=True, error=-32602)
            need(inventory(manifest.parent) == complete_before, 'Denied MCP tool route changed files')
        failures = (
            ('bad-project-selector', dict(params, project='../outside/project.json'), -32602),
            ('bad-artifact-selector', dict(params, artifact='../outside/native-gameplay.json'), -32602),
            ('absolute-artifact-selector', dict(params, artifact=str(artifact)), -32602),
            ('missing-profile', dict(params, profile='UnavailableIndependentProfile'), -32004),
            ('malformed-hash', dict(params, expected_artifact_sha256='bad'), -32602),
            ('stale-project', dict(params, request_id=fresh(), expected_project_sha256='0'*64), -32009),
            ('unknown-values', dict(params, request_id=fresh(), values={'IndependentUnknownField': 1}), -32602),
        )
        for domain, wrong, code in failures:
            before_project, before_source = inventory(manifest.parent), inventory(artifact.parent)
            failure = call('project.gameplay.adopt', wrong, mutate=True, error=code)
            need(inventory(manifest.parent) == before_project and inventory(artifact.parent) == before_source,
                 'Profile adoption failure mutated complete inputs: ' + domain)
            evidence['errors'].append(dict(owner=label, domain=domain, code=failure['code']))
        alias = args.output/'projects'/('alias-' + label);native_links(harness, manifest.parent, alias)
        call('project.inspect', dict(inspect_params, project=alias.name+'/project.json'), error=-32602)
        before = inventory(manifest.parent);result = call('project.gameplay.adopt', params, mutate=True)
        admitted_project(manifest, artifact, {}, before, result)
        stable = inventory(manifest.parent)
        replayed = call('project.gameplay.adopt', params, mutate=True)
        need(replayed['replayed'] is True and inventory(manifest.parent) == stable, 'Profile exact replay changed files')
        call('project.gameplay.adopt', dict(params, values={'Ticks': 1}), mutate=True, error=-32009)
        need(not call('development.jobs', {})['jobs'], 'Synchronous adoption launched a development job')
        owner.close()
        retained = artifact_folder.with_name(artifact_folder.name+'-retained');artifact_folder.rename(retained)
        restarted = harness.owner(world, label+'-restart', mcp=mcp, profiles=config)
        if mcp:answer = restarted.tool('project.gameplay.adopt', params, build=True)
        else:answer = restarted.rpc('project.gameplay.adopt', params)
        need(answer['replayed'] is True and answer['attached'] is True and answer['current_manifest_sha256'] == sha(manifest) and
             inventory(manifest.parent) == stable and not artifact.exists(), 'Profile host-restart source-absent replay differs')
        restarted.close()
        evidence['stages'][label] = dict(adopt=result, retry=replayed, restarted=answer, original_source_absent=True,
                                       retained_inventory=inventory(retained), inspected=inspected)
    evidence['checks'].append('Profile-scoped synchronous Service and MCP adoption preserves guards/path confinement, separate build consent, no jobs and durable host-restart replay.')


def raw_join(evidence, *, legacy_sdk_null_data=False):
    requests, replies = {}, {};used = set();projections = 0
    for envelope in evidence['transport']:
        message = envelope['message'];key = (envelope['owner'], message['id'])
        table = requests if envelope['direction'] == 'request' else replies
        need(key not in table and message['jsonrpc'] == '2.0', 'Duplicate or malformed raw envelope')
        table[key] = message
    need(requests.keys() == replies.keys() and len(requests) == len(evidence['calls']), 'Lossless raw population differs')
    for call in evidence['calls']:
        key = (call['owner'], call['rpc_id']);need(key not in used, 'Duplicate interpreted key');used.add(key)
        need(requests[key]['method'] == call['method'] and exact(requests[key]['params'], call['params']), 'Raw request differs')
        field = 'error' if 'error' in call else 'result'
        raw, interpreted = replies[key][field], call[field]
        if not exact(raw, interpreted):
            # The legacy Python SDK uses error.get('data') when constructing
            # RpcError; its consumer records that optional field as null.
            # Only the supplied compiled-course SDK proof enables this one
            # documented projection. Our own RPC envelopes remain exact.
            need(legacy_sdk_null_data and field == 'error' and call['method'] == 'runtime.gameplay.load_native' and
                 isinstance(raw, dict) and isinstance(interpreted, dict) and raw.get('code') == -32060 and
                 set(raw) == {'code', 'message'} and set(interpreted) == {'code', 'message', 'data'} and
                 interpreted['data'] is None and type(raw['code']) is int and type(interpreted['code']) is int and
                 isinstance(raw['message'], str) and isinstance(interpreted['message'], str) and
                 exact(raw, {name:interpreted[name] for name in ('code', 'message')}), 'Raw native reply differs')
            projections += 1
    need(used == requests.keys(), 'Raw envelopes left unconsumed')
    return projections


def prepared_execution(harness, manifest):
    # The supplied real Traversal artifact is activated only against a compatible
    # prepared course; no synthetic library or game is ever loaded.
    args, evidence = harness.args, harness.evidence
    if args.prepared_world is None:
        evidence['compiled_execution'] = dict(included=False, reason='No compatible prepared course supplied')
        return
    document = read(manifest);descriptor = manifest.parent/document['gameplay']['descriptor']
    module = read(descriptor);metadata = next(x['path'] for x in module['files'] if x['role'] == 'metadata')
    folder = args.output/'compiled-prepared-course'
    command = [sys.executable, str(ROOT/'tests/traversal_yard_contract.py'), '--binary', str(args.binary),
               '--descriptor', str(descriptor), '--manifest', str(descriptor.parent/metadata),
               '--prepared-world', str(manifest.parent/document['entry']['world']), '--output', str(folder),
               '--timeout', str(max(1, min(300, args.deadline-time.monotonic())))]
    log = args.output/'compiled-course.stdout';err = args.output/'compiled-course.stderr'
    with log.open('xb') as out, err.open('xb') as error:
        child = subprocess.Popen(command, stdout=out, stderr=error, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        try:
            while child.poll() is None:
                need(time.monotonic() < args.deadline and log.stat().st_size + err.stat().st_size <= LOG_CAP, 'Prepared compiled-course bound')
                time.sleep(.025)
        except BaseException:child.kill();child.wait(timeout=5);raise
    need(child.returncode == 0 and err.stat().st_size == 0, 'Actual compiled adopted game failed prepared-course verification')
    proof = read(folder/'evidence.json')
    need(proof['passed'] is True and proof['backend'] == 'native_aot' and proof['input_mode'] == 'prepared_world' and
         proof['cleanup_errors'] == [] and len(proof['owners']) == 2 and len(proof['snapshots']) == 9 and
         proof['stages']['values']['Won'] == 1 and proof['stages']['ui_save']['save_serviced'] is True and
         proof['stages']['ui_load']['save_serviced'] is True, 'Actual adopted Tick/UI/save/objective proof differs')
    need(all(x['exit_code'] == 0 and not x['stderr'] and not x['stderr_truncated'] for x in proof['owners']), 'Prepared course owner closure failed')
    sdk_null_data_projections = raw_join(proof, legacy_sdk_null_data=True)
    need(sdk_null_data_projections == 1, 'Expected compiled SDK absent-data/null projection count differs')
    snapshots = {x['label']:x['snapshot'] for x in proof['snapshots']}
    for left, right in (('mid-lift','after-denied-aot-replacement'),('mid-lift','cold-reference'),('mid-lift','cold-fresh'),
                        ('collected-reference','collected-fresh'),('ui-save-checkpoint','ui-load-restored')):
        need(exact(snapshots[left], snapshots[right]), 'Actual complete adopted-game cold/UI pair differs')
    for saved in proof['snapshots']:
        current = read(folder/'saves'/('slot-'+saved['slot'])/'current.json');selected = current['payload']['current']
        payload = folder/'saves'/('slot-'+saved['slot'])/selected['file']
        need(identity(payload) == dict(bytes=saved['payload_bytes'], sha256=saved['payload_sha256']) and
             exact(read(payload)['snapshot'], saved['snapshot']), 'Selected adopted-game complete durable bytes differ')
    for name, pin in proof['source_pins'].items():need(sha(Path(name)) == pin, 'Prepared compiled source/artifact changed')
    evidence['compiled_execution'] = dict(included=True, command=command, evidence=identity(folder/'evidence.json'),
        stdout=identity(log), stderr=identity(err), rpc_count=proof['rpc_count'], native_owners=2,
        snapshots=9, complete_snapshot_pairs=5, backend='native_aot', objective_completed=True,
        sdk_null_error_data_projections=sdk_null_data_projections,
        sdk_null_error_projection_method='runtime.gameplay.load_native', sdk_null_error_projection_code=-32060)
    if args.runtime is not None:
        exported = args.output/'exported-adopted-game'
        harness.cli('export-adopted', ['project', 'build', manifest, '--runtime', args.runtime, '--output', exported])
        relocated = args.output/'relocated-adopted-game';exported.rename(relocated)
        frozen = inventory(relocated);inspection = harness.cli('relocated-inspect', ['game', 'inspect', relocated/'game.json'])
        game_manifest = read(relocated/'game.json')
        relocated_descriptor = relocated/game_manifest['gameplay']['descriptor']
        need(inventory(relocated) == frozen and inspection['gameplay']['descriptor'] == game_manifest['gameplay']['descriptor'] and
             exact(read(relocated_descriptor), module) and inspection['gameplay']['values'] == document['gameplay']['values'],
             'Exported adopted closure or actual native descriptor changed during relocated inspection')
        evidence['compiled_execution']['export'] = dict(included=True, inventory=frozen, inspect=inspection,
            relocated=True, rendered=False, independent_process_gameplay=False)


def journal_payload(manifest, artifact, request):
    # Independently construct the documented native durable format from the
    # exact supplied files. No engine-generated journal is used as a template.
    document = read(manifest);original = io_path(manifest).read_bytes().decode('utf-8')
    descriptor = read(artifact);relative = 'gameplay/adopt-'+request['request_id']+'/native-gameplay.json'
    values = copy.deepcopy(request.get('values', document.get('gameplay', {}).get('values', {})))
    after = copy.deepcopy(document);after['version'] = 2;after['gameplay'] = dict(descriptor=relative, values=values)
    replacement = json.dumps(after, sort_keys=True, indent=2, ensure_ascii=False, allow_nan=False) + '\n'
    after_hash = hashlib.sha256(replacement.encode()).hexdigest()
    world = manifest.parent/document['entry']['world'];input_hash = sha(manifest.parent/document['input_profile']) if 'input_profile' in document else None
    members = [dict(path='native-gameplay.json', size=io_path(artifact).stat().st_size, sha256=sha(artifact), role='descriptor'),
               *copy.deepcopy(descriptor['files'])]
    members.sort(key=lambda row: row['path'])
    normalized = dict(request, artifact_source=str(artifact.absolute()).replace('\\','/'), project_manifest=manifest.name)
    result = dict(request_id=request['request_id'], project_id=document['project_id'],
        previous_manifest_sha256=sha(manifest), manifest_sha256=after_hash, world_sha256=sha(world),
        descriptor=relative, artifact_descriptor_sha256=sha(artifact),
        library_sha256=next(row['sha256'] for row in descriptor['files'] if row['role']=='library'),
        target_os=descriptor['target_os'], target_arch=descriptor['target_arch'], values=values,
        file_count=len(members), payload_bytes=sum(row['size'] for row in members))
    return dict(request=normalized, state='prepared', before_manifest=original, before_project_sha256=sha(manifest),
                after_manifest=replacement, after_project_sha256=after_hash, world_sha256=sha(world),
                input_profile_sha256=input_hash, artifact_inventory=members, result=result)


def write_journal(manifest, request_id, payload):
    folder = manifest.parent/'.poima-gameplay-adoptions';io_path(folder).mkdir(exist_ok=True)
    lock = manifest.parent/'.poima-gameplay-adoptions.lock'
    if not io_path(lock).exists():io_path(lock).write_bytes(b'')
    # Retain the empty publication directory as an independently declared
    # accepted-work boundary. Later rejected recovery need not create sidecars.
    destination = (manifest.parent/payload['result']['descriptor']).parent
    io_path(destination).mkdir(parents=True, exist_ok=True)
    path = folder/(request_id+'.json');need(not io_path(path).exists(), 'Fresh independent journal required')
    canonical = json.dumps(payload, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode()
    write(path, dict(format='poima.project-gameplay-adoption', version=1,
                     sha256=hashlib.sha256(canonical).hexdigest(), payload=payload))
    return path


def recovery_cases(harness, source):
    for boundary in ('before-manifest', 'after-manifest', 'missing-copy-source-present'):
        manifest = harness.project('prepared-'+boundary)
        tree = harness.args.output/'products'/('prepared-source-'+boundary);copy_tree(source.parent, tree);artifact = tree/source.name
        request = request_for(manifest, artifact, values={});payload = journal_payload(manifest, artifact, request)
        copied = manifest.parent/payload['result']['descriptor']
        if boundary != 'missing-copy-source-present':copy_tree(artifact.parent, copied.parent)
        journal = write_journal(manifest, request['request_id'], payload)
        if boundary == 'after-manifest':io_path(manifest).write_bytes(payload['after_manifest'].encode('utf-8'))
        retained = None
        if boundary != 'missing-copy-source-present':
            retained = tree.with_name(tree.name+'-retained');tree.rename(retained);need(not artifact.exists(), 'Original recovery source path still exists')
        before = inventory(manifest.parent)
        result = harness.adopt(manifest, artifact, request, label='recover-'+boundary)
        need(result['replayed'] is True and result['attached'] is True and io_path(manifest).read_bytes() == payload['after_manifest'].encode('utf-8'),
             'Documented prepared boundary did not recover exact manifest')
        final = read(journal)
        need(final['payload']['state'] == 'succeeded' and final['payload']['request'] == payload['request'] and
             final['payload']['result'] == payload['result'], 'Recovered terminal journal identity differs')
        before_manifest = json.loads(payload['before_manifest'])
        need(sha(manifest.parent/before_manifest['entry']['world']) == payload['world_sha256'] and
             (payload['input_profile_sha256'] is None or sha(manifest.parent/before_manifest['input_profile']) == payload['input_profile_sha256']),
             'Prepared recovery changed bound world/input')
        original = artifact if retained is None else retained/source.name
        expected = {source.name: identity(original), **{r['path']:dict(bytes=r['size'],sha256=r['sha256']) for r in read(original)['files']}}
        actual = {name:{k:r[k] for k in ('bytes','sha256')} for name,r in inventory(copied.parent).items() if r['kind']=='file'}
        need(actual == expected, 'Prepared recovery copied a different complete closure')
        harness.evidence['recovery_boundaries'].append(dict(boundary=boundary, synthetic_file_boundary=True,
            operating_system_crash_claim=False, source_absent=retained is not None, request=request,
            independently_authored_journal=payload, before=before, after=inventory(manifest.parent), result=result))
    # A separately identified new explicit intent may proceed under the current
    # manifest guard. Its publication cannot imply success for the older intent.
    manifest = harness.project('prepared-conflict');tree = harness.args.output/'products'/'prepared-conflict-source'
    copy_tree(source.parent, tree);artifact = tree/source.name;request = request_for(manifest, artifact, values={})
    payload = journal_payload(manifest, artifact, request);journal = write_journal(manifest, request['request_id'], payload)
    newer = request_for(manifest, artifact, values={'Ticks': 1})
    replacement = harness.adopt(manifest, artifact, newer, label='explicit-new-intent-with-old-prepared')
    need(replacement['replayed'] is False and replacement['attached'] is True and read(journal)['payload']['state'] == 'prepared',
         'Explicit new intent falsely completed the old prepared request')
    unchanged_failure(manifest, artifact, lambda: harness.adopt(manifest, artifact, request, success=False,
                         label='older-prepared-new-manifest-conflict', error_code=-32009))
    manifest = harness.project('prepared-copy-missing');tree = harness.args.output/'products'/'prepared-copy-missing-source'
    copy_tree(source.parent, tree);artifact = tree/source.name;request = request_for(manifest, artifact, values={})
    payload = journal_payload(manifest, artifact, request);write_journal(manifest, request['request_id'], payload)
    retained = tree.with_name(tree.name+'-retained');tree.rename(retained)
    before = inventory(manifest.parent)
    harness.adopt(manifest, artifact, request, success=False, label='prepared-copy-missing-source-absent', error_code=-32091)
    need(inventory(manifest.parent) == before, 'Unrecoverable prepared request wrote files without source')
    for field in ('world', 'input_profile'):
        manifest = harness.project('prepared-bound-'+field)
        tree = harness.args.output/'products'/('prepared-bound-source-'+field);copy_tree(source.parent, tree);artifact = tree/source.name
        request = request_for(manifest, artifact, values={});payload = journal_payload(manifest, artifact, request)
        copy_tree(artifact.parent, (manifest.parent/payload['result']['descriptor']).parent)
        write_journal(manifest, request['request_id'], payload)
        document = read(manifest);selected = document['entry']['world'] if field == 'world' else document['input_profile']
        bound = manifest.parent/selected;original = bound.read_bytes()
        (harness.args.output/('retained-prepared-bound-'+field+'.bin')).write_bytes(original)
        bound.write_bytes(original+b' ')
        unchanged_failure(manifest, artifact, lambda: harness.adopt(manifest, artifact, request, success=False,
                             label='prepared-changed-'+field, error_code=-32009))
    manifest = harness.project('malformed-journal');request = request_for(manifest, source, values={})
    payload = journal_payload(manifest, source, request);journal = write_journal(manifest, request['request_id'], payload)
    corrupted = read(journal);corrupted['sha256'] = '0'*64;write(journal, corrupted)
    unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, request, success=False,
                         label='invalid-journal-checksum', error_code=-32091))
    harness.evidence['checks'].append('Three independently authored durable prepared-file boundaries recover exact manifest/payloads; old prepared/new-manifest and source-absent missing-copy conflicts reject. No crash injection claim.')



def torn_stage_cases(harness, source):
    torn = b'owned synthetic interrupted stage\x00\xff\n'
    for kind in ('payload', 'manifest', 'terminal-journal'):
        manifest = harness.project('torn-'+kind)
        tree = harness.args.output/'products'/('torn-source-'+kind);copy_tree(source.parent, tree);artifact = tree/source.name
        request = request_for(manifest, artifact, values={});payload = journal_payload(manifest, artifact, request)
        copied = manifest.parent/payload['result']['descriptor']
        if kind != 'payload':copy_tree(artifact.parent, copied.parent)
        journal = write_journal(manifest, request['request_id'], payload)
        root = manifest.parent/'.poima-gameplay-adoption-staging'/request['request_id']
        if kind == 'payload':
            name = read(artifact)['library'];digest = hashlib.sha256(name.encode('utf-8')).hexdigest()
            stage = root/('payload-'+digest+'.pending');io_path(stage.parent).mkdir(parents=True)
        elif kind == 'manifest':stage = manifest.parent/('.poima-adopt-'+request['request_id']+'.pending')
        else:
            io_path(manifest).write_bytes(payload['after_manifest'].encode('utf-8'))
            stage = Path(str(journal)+'.pending')
        io_path(stage).write_bytes(torn)
        retained_source = None
        if kind != 'payload':
            retained_source = tree.with_name(tree.name+'-retained');tree.rename(retained_source)
        before = inventory(manifest.parent)
        result = harness.adopt(manifest, artifact, request, label='recover-torn-'+kind)
        need(result['replayed'] is True and result['attached'] is True and not io_path(stage).exists() and
             io_path(manifest).read_bytes() == payload['after_manifest'].encode('utf-8') and
             read(journal)['payload']['state'] == 'succeeded', 'Authorized torn-stage recovery did not complete exact publication')
        retained = root/'retained';diagnostics = [retained/p.name for p in io_path(retained).iterdir() if p.is_file()]
        need(len(diagnostics) == 1 and re.fullmatch(r'torn-[0-9a-f]{32}\.bin', diagnostics[0].name) and
             io_path(diagnostics[0]).read_bytes() == torn, 'Exact torn owned stage was not retained as bounded diagnostics')
        original = artifact if retained_source is None else retained_source/source.name
        expected = {'native-gameplay.json':identity(original), **{m['path']:dict(bytes=m['size'],sha256=m['sha256']) for m in read(original)['files']}}
        actual = {n:{k:v[k] for k in ('bytes','sha256')} for n,v in inventory(copied.parent).items() if v['kind']=='file'}
        need(actual == expected and sha(manifest.parent/read(manifest)['entry']['world']) == payload['world_sha256'] and
             (payload['input_profile_sha256'] is None or sha(manifest.parent/read(manifest)['input_profile']) == payload['input_profile_sha256']),
             'Recovered stage changed full payload or bound world/input')
        harness.evidence['recovery_boundaries'].append(dict(boundary='torn-'+kind, synthetic_file_boundary=True,
            operating_system_crash_claim=False, source_absent=retained_source is not None,
            independently_authored_journal=payload, before=before, after=inventory(manifest.parent), result=result,
            retained_torn_stage=identity(diagnostics[0])))
    # No valid canonical journal authorizes replacement of a torn first-prepare
    # stage. Rejection must retain all bytes and invent no completion receipt.
    manifest = harness.project('torn-first-prepare');request = request_for(manifest, source, values={})
    folder = manifest.parent/'.poima-gameplay-adoptions';io_path(folder).mkdir()
    io_path(manifest.parent/'.poima-gameplay-adoptions.lock').write_bytes(b'')
    stage = folder/(request['request_id']+'.json.pending');io_path(stage).write_bytes(torn)
    unchanged_failure(manifest, source, lambda: harness.adopt(manifest, source, request, success=False,
                         label='unauthorized-torn-first-prepare', error_code=-32091))
    need(io_path(stage).read_bytes() == torn and not io_path(folder/(request['request_id']+'.json')).exists(), 'Torn first prepare was falsely recovered')
    harness.evidence['checks'].append('Valid independently authored prepared authority recovers torn payload/manifest/terminal stages while retaining exact diagnostic bytes; unauthorised first prepare rejects. No killed-process crash claim.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','artifact','profile-template','output'):parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--profile', default='native');parser.add_argument('--prepared-world', type=Path)
    parser.add_argument('--runtime', type=Path);parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args();started = time.monotonic()
    need(60 <= args.timeout <= 900, 'Fixture timeout must be60..900 seconds');args.deadline = started + args.timeout
    args.binary = args.binary.resolve(strict=True);args.artifact = args.artifact.resolve(strict=True);args.profile_template = args.profile_template.resolve(strict=True)
    args.output = args.output.absolute();need(not args.output.exists(), 'Fresh fixture output required');args.output.mkdir(parents=True)
    if os.name == 'nt':need(os.environ.get('CHECKPOINT98_JOB_ASSIGNED') == '1', 'Root-assigned Windows Job required before any native execution')
    if args.prepared_world is not None:args.prepared_world = args.prepared_world.resolve(strict=True)
    if args.runtime is not None:args.runtime = args.runtime.resolve(strict=True)
    args.descriptor = read(args.artifact)
    need(args.descriptor['format'] == 'poima.native-gameplay' and args.descriptor['version'] == 2 and
         args.descriptor['identity'] == 'poima.examples.traversal-yard' and args.descriptor['type'] == 'Poima.Examples.TraversalYardGame',
         'Supply the actual retained Traversal Yard NativeAOT descriptor')
    need(args.descriptor['call_version'] == 1 and args.descriptor['services_version'] == 7 and
         args.descriptor['minimum_services_bytes'] == 336, 'Supplied artifact ABI requirement differs')
    source_before = inventory(args.artifact.parent)
    directory_pins = {str(args.artifact.parent):source_before}
    if args.prepared_world is not None:
        assets = Path(str(args.prepared_world)+'.assets')
        if assets.exists():directory_pins[str(assets)] = inventory(assets)
    if args.runtime is not None:directory_pins[str(args.runtime)] = inventory(args.runtime)
    for member in args.descriptor['files']:
        name = member['path'].replace('\\', '/')
        need(':' not in name and not name.startswith('/') and all(part not in ('', '.', '..') for part in name.split('/')), 'Unsafe actual artifact path')
        need(identity(args.artifact.parent/name) == dict(bytes=member['size'], sha256=member['sha256']), 'Supplied actual artifact payload changed')
    pins = {str(path.resolve()):identity(path) for path in (Path(__file__), args.binary, args.artifact, args.profile_template)}
    for path in args.artifact.parent.rglob('*'):
        if path.is_file():pins[str(path.resolve())] = identity(path)
    if args.prepared_world is not None:
        pins[str(args.prepared_world)] = identity(args.prepared_world)
        for path in Path(str(args.prepared_world)+'.assets').rglob('*'):
            if path.is_file():pins[str(path.resolve())] = identity(path)
    if args.runtime is not None:
        for path in args.runtime.rglob('*'):
            if path.is_file():pins[str(path.resolve())] = identity(path)
    # Optional prepared-course runner and all its transitive source utilities are
    # supplied references, never generated or compiled by this fixture.
    for path in sorted((ROOT/'tools/python/poima_client').rglob('*')):
        if path.is_file() and path.suffix in ('.py','.json'):pins[str(path.resolve())] = identity(path)
    for name in ('tests/traversal_yard_contract.py','tests/locomotion_yard_contract.py','tests/locomotion_yard_capture.py',
                 'tests/animation_rotation_retarget.py','tests/animation_rotation_retarget_capture.py','tests/rotation_retarget_fixture.py',
                 'tests/frame_transfer_fixture.py','tests/fbx_fixture.py','tests/texture_fixture.py','tests/gltf_fixture.py','tests/scene_capture.py',
                 'examples/traversal-yard/run.py','examples/locomotion-yard/run.py'):
        path = ROOT/name
        if path.exists():pins[str(path.resolve())] = identity(path)
    evidence = dict(passed=False, commands=[], calls=[], transport=[], owners=[], errors=[], links=[], checks=[], stages={},
        recovery_boundaries=[], cleanup_errors=[], source_pins=pins, input_inventories=directory_pins, limits=dict(timeout=args.timeout,rpc_rows=ROW_CAP,evidence_bytes=EVIDENCE_CAP,output_bytes=OUTPUT_CAP),
        scope='Actual supplied NativeAOT project adoption plus synthetic durable-file recovery boundaries; no builds, provider calls, GPU or crash simulation.')
    harness = Harness(args, evidence);original_error = None
    try:
        (args.output/'projects').mkdir();(args.output/'products').mkdir()
        config = profile_config(args);pins[str(config)] = identity(config)
        profile = read(config)['profiles'][0]
        for name in ('python','publisher','dotnet','exporter'):
            selected = Path(profile[name]).resolve(strict=True);pins[str(selected)] = identity(selected)
        descriptor_path = Path(profile['runtime_root'])/'runtime.json';pins[str(descriptor_path.resolve())] = identity(descriptor_path)
        private = args.output/'products'/'cli-original';copy_tree(args.artifact.parent, private)
        retained = cli_cases(harness, private/args.artifact.name)
        negative_cases(harness, retained)
        service_cases(harness, retained, config)
        recovery_cases(harness, retained)
        torn_stage_cases(harness, retained)
        activated = harness.project('compiled-compatible')
        result = harness.adopt(activated, retained, request_for(activated, retained, values={}), label='compiled-compatible-adoption')
        need(result['replayed'] is False, 'Fresh compiled-compatible adoption unexpectedly replayed')
        prepared_execution(harness, activated)
        raw_join(evidence)
        evidence['rpc_count'] = len(evidence['calls'])
        need(all(inventory(Path(name)) == original for name,original in directory_pins.items()), 'Original complete artifact/assets/runtime inventory changed')
        need(all(identity(Path(name)) == pin for name,pin in pins.items()), 'A supplied source/runtime/tool identity changed')
        evidence['passed'] = True
    except BaseException:
        original_error = traceback.format_exc();evidence['error'] = original_error
    finally:
        try:harness.finish()
        except BaseException:
            if original_error is None:evidence['error'] = traceback.format_exc()
        evidence['seconds'] = time.monotonic()-started
        evidence['passed'] = bool(evidence['passed'] and not evidence.get('error') and not evidence['cleanup_errors'] and evidence['seconds'] < args.timeout)
        try:
            evidence['changed_inputs'] = [name for name,pin in pins.items() if not io_path(name).is_file() or identity(Path(name)) != pin]
            evidence['changed_directories'] = [name for name,original in directory_pins.items() if not io_path(name).is_dir() or inventory(Path(name)) != original]
        except BaseException:evidence['rescan_error'] = traceback.format_exc();evidence['changed_inputs'] = None
        evidence['passed'] = bool(evidence['passed'] and evidence['changed_inputs'] == [] and evidence.get('changed_directories') == [])
        data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode()
        need(len(data) <= EVIDENCE_CAP and sum(p.stat().st_size for p in io_path(args.output).rglob('*') if p.is_file() and not p.is_symlink()) + len(data) <= OUTPUT_CAP,
             'Terminal evidence/output budget exceeded')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic() >= args.deadline:
            late = args.output/'late-evidence.json';need(not late.exists(), 'Late evidence retention path occupied')
            (args.output/'evidence.json').rename(late);evidence['passed'] = False
            (args.output/'evidence.json').write_bytes((json.dumps(dict(passed=False,error='Fixture finalpublication deadline; full late evidence retained'),indent=2)+'\n').encode('utf-8'))
    print(json.dumps(dict(passed=evidence['passed'],rpc_count=evidence.get('rpc_count'),owners=len(evidence['owners']),checks=len(evidence['checks']),error=evidence.get('error'))))
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
