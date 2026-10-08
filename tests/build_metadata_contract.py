#!/usr/bin/env python3
"""Black-box agreement of compiled CLI, MCP and exported profiler metadata.

Only disposable authored worlds are created. No graphics, gameplay module,
installed runtime, provider or game bundle is loaded. Explicit --output retains
private evidence; the default CTest invocation cleans its owned temporary tree.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import JsonRpcProcess, WorldClient


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result
    def invalid(value):
        raise AssertionError('Non-finite JSON number: ' + value)
    return json.loads(data, object_pairs_hook=pairs, parse_constant=invalid)


def image_target(path):
    """Independently read the executable header, rather than its filename."""
    with path.open('rb') as stream:
        header = stream.read(64)
        if header.startswith(b'\x7fELF'):
            require(len(header) == 64 and header[4:6] == b'\x02\x01', 'Expected little-endian ELF64 executable')
            machine = struct.unpack_from('<H', header, 18)[0]
            require(machine == 62, 'This qualification expects an x86_64 executable')
            return 'Linux', 'x86_64'
        require(header.startswith(b'MZ') and len(header) == 64, 'Expected actual native ELF or PE binary')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        pe = stream.read(6)
        require(pe[:4] == b'PE\0\0' and len(pe) == 6 and struct.unpack_from('<H', pe, 4)[0] == 0x8664,
                'Expected actual x86_64 PE executable')
        return 'Windows', 'x86_64'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--expected-version', required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=90)
    args = parser.parse_args()
    require(math.isfinite(args.timeout) and 20 <= args.timeout <= 300, 'Timeout must be 20..300 seconds')
    require(0 < len(args.expected_version) <= 128 and not any(ord(c) < 32 for c in args.expected_version),
            'Expected version must be nonempty diagnostic text')
    binary = args.binary.resolve()
    require(binary.is_file(), 'Native binary is missing')
    expected_os, expected_arch = image_target(binary)
    temporary = None
    if args.output is None:
        scratch = ROOT / 'build/build-metadata-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='metadata-', dir=scratch)
        output = Path(temporary.name)
    else:
        output = args.output.resolve()
        require(not output.exists(), 'Output must be new; preserve previous evidence')
        output.mkdir(parents=True)
    deadline = time.monotonic() + args.timeout
    record = dict(passed=False, expected_version=args.expected_version,
                  binary_sha256=digest(binary), test_sha256=digest(Path(__file__).resolve()),
                  checks={}, owners=[], outputs={}, calls=[], limits=[
                      'CLI/MCP/profiler metadata agreement only; no graphics or game execution.',
                      'MCP initialize response is qualified, not a provider connection or a full tool workflow.',
                      'No installed-runtime or project/game bundle compatibility gate is exercised.',
                      'Default CTest output is temporary; explicit --output retains private evidence.'])
    owners = []
    cli = None
    def remaining():
        value = deadline - time.monotonic()
        require(value > 0, 'Metadata qualification deadline exceeded')
        return min(20, value)
    def native(path):
        if args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', str(path.resolve())],
                                           text=True, timeout=remaining()).strip()
        return str(path.resolve())
    def write(name, value):
        path = output / name
        path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
        record['outputs'][name] = digest(path)
    def rpc(owner, method, params=None):
        result = owner.call(method, params or {}, timeout=remaining())
        record['calls'].append(dict(method=method, params=params or {}, result=result))
        return result
    try:
        cli = subprocess.Popen([str(binary), 'version'], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = cli.communicate(timeout=remaining())
        except BaseException:
            if cli.poll() is None:
                cli.kill()
            cli.communicate(timeout=10)
            raise
        record['owners'].append(dict(kind='version', pid=cli.pid, exit_code=cli.returncode,
                                     stderr=stderr.decode('utf-8', errors='replace')))
        require(cli.returncode == 0, 'CLI version exited nonzero')
        require(len(stdout) <= 1024 * 1024, 'CLI metadata output exceeded fixture bound')
        envelope = strict_json(stdout.decode('utf-8'))
        require(type(envelope.get('protocol_version')) is int and envelope['protocol_version'] == 1 and
                envelope.get('command') == 'version' and envelope.get('status') == 'ok' and
                envelope.get('request_id') is None and envelope.get('diagnostics') == [], 'Invalid CLI version envelope')
        metadata = envelope.get('result')
        require(isinstance(metadata, dict), 'CLI version result must be an object')
        for field in ('version', 'compiler', 'compiler_version', 'target_os', 'target_arch'):
            value = metadata.get(field)
            require(isinstance(value, str) and 0 < len(value) <= 128 and not any(ord(c) < 32 for c in value),
                    'Missing or untyped compiled metadata: ' + field)
        require(metadata['version'] == args.expected_version, 'CLI version differs from expected build version')
        require(metadata['target_os'] == expected_os and metadata['target_arch'] == expected_arch,
                'CLI target metadata disagrees with actual native executable header')
        record['metadata'] = metadata
        write('version.json', envelope)
        record['checks']['typed_cli_metadata_matches_expected_version_and_executable_target'] = True

        mcp = JsonRpcProcess([str(binary), 'mcp', '--world', native(output / 'mcp-world.json')], close_timeout=10)
        owners.append(('mcp', mcp))
        initialized = mcp.request('initialize', dict(protocolVersion='2025-11-25', capabilities={},
            clientInfo=dict(name='poima-build-metadata-contract', version='1')), timeout=remaining())
        require(isinstance(initialized, dict) and initialized.get('protocolVersion') == '2025-11-25' and
                isinstance(initialized.get('capabilities'), dict) and 'tools' in initialized['capabilities'],
                'Actual MCP initialize response is malformed')
        require(initialized.get('serverInfo') == dict(name='Poima', version=metadata['version']),
                'MCP serverInfo does not report the compiled CLI version')
        record['mcp_initialize_calls'] = 1
        write('mcp-initialize.json', initialized)
        record['checks']['actual_mcp_initialize_version_matches_cli'] = True

        world_path = output / 'profiled-world.json'
        world = WorldClient.open(str(binary), native(world_path), close_timeout=10)
        owners.append(('world', world.transport))
        initial = rpc(world, 'world.inspect')
        require(initial['revision'] == 0 and initial['entity_count'] == 0 and not initial['read_only'],
                'Profiler fixture is not a fresh authoring world')
        capture = uuid.uuid4().hex
        started = rpc(world, 'profiler.start', dict(capture_id=capture, expected_capture_id=None, capacity=64))
        require(started['recording'] and started['count'] == 0, 'Profiler did not start an empty recording')
        rpc(world, 'world.inspect')
        identity = uuid.uuid4().hex
        changed = rpc(world, 'world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0,
            ops=[dict(op='entity.create', id=identity, name='Metadata qualification')]))
        require(changed['revision'] == 1, 'Disposable authoring mutation did not commit')
        final = rpc(world, 'world.inspect')
        require(final['revision'] == 1 and final['entity_count'] == 1, 'Disposable world state differs after mutation')
        stopped = rpc(world, 'profiler.stop', dict(capture_id=capture))
        require(not stopped['recording'] and stopped['open'] == 0 and stopped['dropped'] == 0,
                'Profiler did not finish a complete bounded capture')
        exported = rpc(world, 'profiler.export', dict(capture_id=capture))
        require(exported['capture_id'] == capture, 'Exported capture identity differs')
        trace = exported['trace']
        require(isinstance(trace, dict) and trace['poima']['engine_version'] == metadata['version'] and
                trace['poima']['recording'] is False and trace['poima']['open'] == 0,
                'Profiler trace metadata disagrees with compiled CLI version')
        events = [e for e in trace['traceEvents'] if e.get('ph') == 'X']
        require(any(e['name'] == 'world.transact' and not e['args']['failed'] for e in events) and
                sum(e['name'] == 'world.inspect' and not e['args']['failed'] for e in events) == 2,
                'Exported trace is not the actually exercised authoring capture')
        record['trace_cpu_spans'] = len(events)
        write('profiler-trace.json', trace)
        record['checks']['actual_authoring_capture_exports_matching_engine_version'] = True
        require(world_path.is_file(), 'Committed disposable authored file is absent')
        document = strict_json(world_path.read_text(encoding='utf-8'))
        require(document['revision'] == 1 and document['entities'][identity]['name'] == 'Metadata qualification',
                'Actual persisted authored state differs')
        record['outputs']['profiled-world.json'] = digest(world_path)
        require(digest(binary) == record['binary_sha256'], 'Supplied binary changed during qualification')
        record['passed'] = True
    except BaseException as failure:
        record['failure'] = dict(type=type(failure).__name__, message=str(failure))
        raise
    finally:
        for kind, owner in reversed(owners):
            error = None
            try:
                owner.close()
            except BaseException as failure:
                error = str(failure)
            row = dict(kind=kind, pid=owner.process_id, exit_code=owner.returncode,
                       closed=owner.closed, cleanup_error=error, stderr=owner.stderr_tail)
            record['owners'].append(row)
            if error or owner.returncode != 0 or not owner.closed:
                record['passed'] = False
        record['world_rpc_calls'] = len(record['calls'])
        evidence = output / 'evidence.json'
        evidence.write_text(json.dumps(record, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')
        summary = dict(passed=record['passed'], checks=record['checks'],
                       owned_exit_codes=[o['exit_code'] for o in record['owners']],
                       binary_sha256=record['binary_sha256'], evidence_sha256=digest(evidence),
                       evidence_retained=temporary is None)
        if temporary is not None:
            temporary.cleanup()
        print(json.dumps(summary))
    require(record['passed'], 'Owned process cleanup failed')


if __name__ == '__main__':
    main()
