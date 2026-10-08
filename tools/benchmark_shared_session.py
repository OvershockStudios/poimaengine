#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare native standalone/shared authoring reads, not game frame rates.

Run Python on the engine's operating system. All worlds/endpoints/processes are
disposable and owned by this tool. Public output is allowlisted; local evidence
retains paths, bounded diagnostics and individual timing samples. No provider,
runtime simulation, GPU, builds or authored mutations are used.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient

WORLD_LIMIT = 16 * 1024 * 1024
DIAGNOSTIC_LIMIT = 65536
MAX_REVISION = 9007199254740991


class BudgetExceeded(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bounded_integer(low, high):
    def parse(value):
        number = int(value)
        if not low <= number <= high:
            raise argparse.ArgumentTypeError('Expected integer {}..{}'.format(low, high))
        return number
    return parse


def bounded_seconds(low, high):
    def parse(value):
        number = float(value)
        if not math.isfinite(number) or not low <= number <= high:
            raise argparse.ArgumentTypeError('Expected finite seconds {}..{}'.format(low, high))
        return number
    return parse


def remaining(deadline, cap):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise BudgetExceeded('Overall benchmark request budget exhausted')
    return min(seconds, cap)


def digest(path, deadline):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            remaining(deadline, 1)
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            result.update(chunk)
    return result.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Source world has duplicate JSON keys')
        result[key] = value
    return result


def source_identity(world):
    with world.open('rb') as stream:
        encoded = stream.read(WORLD_LIMIT + 1)
    require(len(encoded) <= WORLD_LIMIT, 'Source world exceeds 16 MiB')
    def invalid_constant(value):
        raise ValueError('Source world contains a nonfinite JSON constant')
    document = json.loads(encoded.decode('utf-8'), object_pairs_hook=unique_object,
                          parse_constant=invalid_constant)
    require(isinstance(document, dict), 'Source must be an authored world object')
    revision = document.get('revision')
    require(type(revision) is int and 0 <= revision <= MAX_REVISION,
            'Source revision must be a safe nonnegative integer')
    world_id = document.get('world_id')
    require(isinstance(world_id, str) and re.fullmatch('[0-9a-f]{32}', world_id) is not None,
            'Source world identity must be 32 lowercase hexadecimal digits')
    entities = document.get('entities')
    require(isinstance(entities, dict) and len(entities) <= 10000,
            'Source entities must be a bounded authored entity map')
    return dict(world_id=world_id, revision=revision, entity_count=len(entities))


def check_inspection(value, expected):
    for name in ('world_id', 'revision', 'entity_count'):
        require(value[name] == expected[name], 'Inspection changed expected ' + name)
    require(value['persisted'] is True and value['read_only'] is False
            and value['mode'] == 'authoring', 'Inspection is not a persisted authoring world')


def statistics_for(samples, total_ms):
    ordered = sorted(samples)
    return dict(samples=len(samples), p50_ms=statistics.median(samples),
                p95_ms=ordered[math.ceil(len(samples) * .95) - 1],
                mean_ms=statistics.mean(samples), min_ms=ordered[0], max_ms=ordered[-1],
                timed_total_ms=total_ms)


class OwnedHost:
    """Drain a trusted native host's stderr without retaining unbounded output."""
    def __init__(self, engine, world, endpoint):
        self.tail = bytearray()
        self.stderr_bytes = 0
        self.reader_error = None
        self.forced = False
        self.process = subprocess.Popen([str(engine), 'serve', str(world), '--endpoint', endpoint],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            shell=False, bufsize=0)
        self.reader = threading.Thread(target=self._drain, name='poima-benchmark-host-stderr', daemon=True)
        try:
            self.reader.start()
        except BaseException:
            self.process.kill()
            self.process.wait(timeout=5)
            self.process.stderr.close()
            raise

    def _drain(self):
        try:
            while True:
                chunk = self.process.stderr.read(4096)
                if not chunk:
                    return
                self.stderr_bytes += len(chunk)
                self.tail.extend(chunk)
                del self.tail[:-DIAGNOSTIC_LIMIT]
        except Exception as error:
            self.reader_error = type(error).__name__

    def close(self, graceful, timeout):
        deadline = time.monotonic() + timeout
        if not graceful and self.process.poll() is None:
            self.forced = True
            try:
                self.process.terminate()
            except OSError:
                if self.process.poll() is None:
                    raise
        try:
            self.process.wait(timeout=max(.001, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            self.forced = True
            self.process.kill()
            self.process.wait(timeout=1)
        self.reader.join(timeout=max(0, min(1, deadline - time.monotonic())))
        if self.reader.is_alive():
            raise RuntimeError('Owned host diagnostic reader did not stop within cleanup budget')
        self.process.stderr.close()
        return dict(exit_code=self.process.returncode, forced=self.forced,
                    stderr_bytes=self.stderr_bytes,
                    stderr_truncated=self.stderr_bytes > DIAGNOSTIC_LIMIT,
                    stderr_tail=bytes(self.tail).decode('utf-8', errors='replace'),
                    diagnostic_error=self.reader_error)


def measure(client, options, expected, deadline):
    for unused in range(options.warmup):
        check_inspection(client.inspect(timeout=remaining(deadline, options.rpc_timeout_seconds)), expected)
    samples = []
    start = time.perf_counter_ns()
    for unused in range(options.samples):
        timeout = remaining(deadline, options.rpc_timeout_seconds)
        before = time.perf_counter_ns()
        reply = client.inspect(timeout=timeout)
        samples.append((time.perf_counter_ns() - before) / 1e6)
        check_inspection(reply, expected)
    total_ms = (time.perf_counter_ns() - start) / 1e6
    return statistics_for(samples, total_ms), samples


def trial(mode, repeat, sequence, options, expected, input_hash, deadline, record):
    client = host = None
    graceful = False
    errors = []
    record.update(mode=mode, repeat=repeat, sequence=sequence, passed=False, cleanup_errors=errors)
    clone = options.output / ('trial-{:02d}-{}.json'.format(sequence, mode))
    record['world_path'] = str(clone)
    try:
        remaining(deadline, options.rpc_timeout_seconds)
        shutil.copyfile(options.world, clone)
        require(digest(clone, deadline) == input_hash, 'World clone differs from source input')
        transport_options = dict(close_timeout=options.cleanup_timeout_seconds)
        if mode == 'standalone':
            client = WorldClient.open(options.engine, clone, **transport_options)
        else:
            endpoint = 'benchmark-' + uuid.uuid4().hex
            record['endpoint'] = endpoint
            host = OwnedHost(options.engine, clone, endpoint)
            client = WorldClient.connect(options.engine, endpoint,
                timeout_ms=max(100, int(remaining(deadline, options.rpc_timeout_seconds) * 1000)),
                **transport_options)
        metadata = client.discover(timeout=remaining(deadline, options.rpc_timeout_seconds))
        require(metadata['protocol_version'] == 1, 'Unexpected native protocol version')
        revision = metadata.get('schema_revision')
        require(type(revision) is int and 0 <= revision <= MAX_REVISION, 'Invalid native schema revision')
        if mode == 'shared':
            require(metadata.get('session_scope') == 'shared_headless', 'Connected host scope differs')
        record['schema_revision'] = revision
        record['statistics'], record['samples_ms'] = measure(client, options, expected, deadline)
        record['all_inspections_correct'] = True
        require(digest(clone, deadline) == input_hash, 'Read-only benchmark modified its world clone')
        if host is not None:
            require(client.call('host.shutdown', timeout=remaining(deadline, options.rpc_timeout_seconds))['closed'] is True,
                    'Owned host did not acknowledge shutdown')
            graceful = True
    finally:
        if client is not None:
            try:
                client.close()
                record['client_exit_code'] = client.transport.returncode
                if record['client_exit_code'] != 0:
                    errors.append('Owned native client did not exit with code zero')
            except BaseException as error:
                errors.append('Client cleanup: ' + str(error)[:1024])
        if host is not None:
            try:
                record['host_cleanup'] = host.close(graceful, options.cleanup_timeout_seconds)
                cleanup = record['host_cleanup']
                if cleanup['exit_code'] != 0 or cleanup['forced'] or cleanup['diagnostic_error']:
                    errors.append('Owned shared host did not close cleanly')
            except BaseException as error:
                errors.append('Host cleanup: ' + str(error)[:1024])
    require(not errors, 'Trial cleanup failed; inspect local report')
    record['passed'] = True


def public_summary(report):
    trials = []
    aggregate = {}
    for item in report['trials']:
        row = {key: item[key] for key in ('mode', 'repeat', 'sequence', 'passed',
            'schema_revision', 'statistics', 'all_inspections_correct', 'client_exit_code') if key in item}
        row['cleanup_error_count'] = len(item['cleanup_errors'])
        if 'host_cleanup' in item:
            row['host_exit_code'] = item['host_cleanup']['exit_code']
            row['host_forced'] = item['host_cleanup']['forced']
        trials.append(row)
    for mode in ('standalone', 'shared'):
        complete = [item for item in report['trials'] if item['mode'] == mode and item['passed']]
        if complete:
            samples = [value for item in complete for value in item['samples_ms']]
            aggregate[mode] = statistics_for(samples, sum(item['statistics']['timed_total_ms'] for item in complete))
            aggregate[mode]['complete_trials'] = len(complete)
    result = {key: report[key] for key in ('format', 'passed', 'hashes', 'world', 'parameters',
        'python_version', 'platform', 'context', 'percentile_method', 'input_hashes_stable',
        'schema_revision') if key in report}
    result.update(trials=trials, modes=aggregate)
    if 'failure' in report:
        result['failure_kind'] = report['failure']['kind']
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--world', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New local evidence directory')
    parser.add_argument('--warmup', type=bounded_integer(0, 100), default=10)
    parser.add_argument('--samples', type=bounded_integer(1, 1000), default=100)
    parser.add_argument('--repeats', type=bounded_integer(1, 8), default=3)
    parser.add_argument('--timeout-seconds', type=bounded_seconds(1, 600), default=120)
    parser.add_argument('--rpc-timeout-seconds', type=bounded_seconds(.1, 30), default=10)
    parser.add_argument('--cleanup-timeout-seconds', type=bounded_seconds(1, 15), default=5)
    options = parser.parse_args(argv)
    options.engine = options.engine.resolve()
    options.world = options.world.resolve()
    options.output = options.output.resolve()
    require(options.engine.is_file() and options.world.is_file(), 'Engine/world must be existing files')
    require(not options.output.exists(), 'Output directory must be new')
    deadline = time.monotonic() + options.timeout_seconds
    expected = source_identity(options.world)
    hash_paths = {'engine': options.engine, 'world_input': options.world,
                  'benchmark_tool': Path(__file__).resolve()}
    for name in ('client.py', 'transport.py', 'responses.py', 'errors.py', 'core_responses.v1.candidate.json'):
        hash_paths['python_sdk/' + name] = ROOT / 'tools/python/poima_client' / name
    hashes = {key: digest(path, deadline) for key, path in hash_paths.items()}
    options.output.mkdir(parents=True, exist_ok=False)
    report = dict(format='poima.shared-session-benchmark.v1', passed=False,
        hashes=hashes, world=expected, python_version=list(sys.version_info[:3]), platform=sys.platform,
        parameters=dict(warmup=options.warmup, samples=options.samples, repeats=options.repeats,
            overall_request_budget_seconds=options.timeout_seconds,
            rpc_timeout_seconds=options.rpc_timeout_seconds,
            cleanup_timeout_per_owned_process_seconds=options.cleanup_timeout_seconds,
            max_inspect_calls=2 * options.repeats * (options.warmup + options.samples)),
        percentile_method='nearest-rank ceil(n*0.95); p50 median; pooled mode samples',
        context=['Sequential SDK world.inspect calls with core response validation.',
            'Alternating standalone/shared order; odd repeat counts cannot balance first positions exactly.',
            'Timed samples exclude startup, discovery, warmup and cleanup; total includes correctness checks.',
            'Overall deadline applies to request loops; bounded cleanup follows separately.',
            'Each SDK close allows its configured cleanup timeout plus 0.1 seconds; host kill/reap fallback allows one extra second.',
            'Synchronous process creation/filesystem calls are not forcibly preempted by the deadline.',
            'Same-machine authoring latency probe, not game FPS, provider tokens or game creation time.',
            'No runtime simulation, builds, provider, GPU, authored writes or global timer changes.'],
        local_paths=dict(engine=str(options.engine), world=str(options.world), output=str(options.output)),
        trials=[])
    exit_code = 1
    try:
        sequence = 0
        for repeat in range(1, options.repeats + 1):
            order = ('standalone', 'shared') if repeat % 2 else ('shared', 'standalone')
            for mode in order:
                sequence += 1
                record = {}
                report['trials'].append(record)
                trial(mode, repeat, sequence, options, expected, hashes['world_input'], deadline, record)
        require(len({item['schema_revision'] for item in report['trials']}) == 1,
                'Native schema revision changed between trials')
        report['schema_revision'] = report['trials'][0]['schema_revision']
        report['input_hashes_stable'] = all(digest(path, deadline) == hashes[key]
                                          for key, path in hash_paths.items())
        require(report['input_hashes_stable'], 'Engine, source world or SDK changed during benchmark')
        report['passed'] = True
        exit_code = 0
    except BaseException as error:
        report['failure'] = dict(kind=type(error).__name__, message=str(error)[:4096])
        exit_code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        summary = public_summary(report)
        (options.output / 'local-report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        (options.output / 'public-summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(json.dumps(summary, indent=2, allow_nan=False))
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
