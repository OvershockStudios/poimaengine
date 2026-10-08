#!/usr/bin/env python3
"""Compare legacy and joined reads using the same compiled Workshop Relay game.

Runs the public replay once per mode in fresh directories. Only session_id values
are normalized when comparing complete checked state. Local evidence/logs can
contain machine paths; public-summary.json uses an explicit allowlist.
No provider, GPU capture, game repair or engine build is performed.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
REPLAY = ROOT / 'tests/agent_workshop_replay.py'
SNAPSHOTS = ('checkpoint', 'compiled_restore', 'fresh_process_restore',
             'win', 'win_after_compiled_load', 'fresh_process_win')
NORMALIZATION = ('Replace only values of dictionary keys named session_id with '
                 '<runtime-session>; require each original value to be a 32hex ID. '
                 'No path, entity ID, tick, revision, pose, motion, velocity, UI or buffer normalization.')
BYTE_DEFINITION = ('Compact Python UTF-8 reserialization, ensure_ascii=False and no NaN: '
    'request body is {method,params}; result body is the parsed native result. '
    'Excludes JSON-RPC envelopes, framing, stderr, MCP/provider data; not exact native wire bytes or tokens.')
REQUIRED_CHECKS = frozenset((
    'Genuine native camera-ray miss Use preserves resources/counters.',
    'Actual aimed native pickup beyond three meters cannot be collected.',
    'Full-capacity pickup rejection preserved exact inventory/native pickup set and poses.',
    'Wrong AAA recipe rejection preserved exact inventory/native pickup set and poses.',
    'Locked second station rejection preserved exact inventory/native pickup set and poses.',
    'Generated native capacity-three int32 buffer and six compiled-spawned kind-tagged collision props.',
    'AAA capacity rejection, wrong recipe and locked station preserve exact resources.',
    'Compiled Drop Control only queues; next Tick removes LAST and spawns a reachable matching-kind native prop in the tested open-space placement.',
    'Per-kind conservation through two exact deliveries, seven successful collects and one completed drop.',
    'Pause/Resume owner intents at unchanged ticks and durable compiled Save/Load.',
    'Full reflected state, ordered buffer, native identities/poses, runtime revisions and logical modal UI restore before callback.',
    'Fresh native process loads without Initialize and independently completes second recipe.',
    'Terminal neutral/Use/Drop inputs preserve victory/resources.'))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(65536), b''):
            value.update(chunk)
    return value.hexdigest()


def compact(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':'))


def json_bytes(value):
    return len(compact(value).encode('utf-8'))


def canonical(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(',', ':'))


def read_json(path):
    require(path.is_file() and path.stat().st_size <= 64 * 1024 * 1024,
            'Missing or excessive replay evidence: '+str(path))
    return json.loads(path.read_text(encoding='utf-8'))


def normalize(value):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key == 'session_id':
                require(isinstance(item, str) and re.fullmatch('[0-9a-f]{32}', item),
                        'Normalization encountered an invalid runtime session ID')
                result[key] = '<runtime-session>'
            else:
                result[key] = normalize(item)
        return result
    if isinstance(value, list):
        return [normalize(item) for item in value]
    return value


def linux_identity(pid):
    try:
        # Fields after the command's final ')' start at state (field 3).
        fields = (Path('/proc') / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()
        return int(fields[1]), fields[19]
    except (OSError, ValueError, IndexError):
        return None


def force_owned_cleanup(process):
    """Failure-only cleanup; never signal an unowned endpoint or global process."""
    if process.poll() is not None:
        return
    if sys.platform == 'win32':
        completed = subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        if completed.returncode != 0 and process.poll() is None:
            raise RuntimeError('Owned replay process tree could not be terminated')
    else:
        # SDK native children start separate sessions, so killpg alone misses
        # them. Stop the owned Python parent before collecting its descendants.
        original = linux_identity(process.pid)
        if original is not None:
            os.kill(process.pid, signal.SIGSTOP)
            records = {}
            for entry in Path('/proc').iterdir():
                if entry.name.isdigit():
                    identity = linux_identity(int(entry.name))
                    if identity is not None:
                        records[int(entry.name)] = identity
            owned, frontier = [], [process.pid]
            while frontier:
                parent = frontier.pop()
                children = [pid for pid, (ppid, _) in records.items() if ppid == parent]
                owned.extend(children)
                frontier.extend(children)
            for pid in reversed(owned):
                if linux_identity(pid) == records[pid]:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            if linux_identity(process.pid) == original:
                os.kill(process.pid, signal.SIGKILL)
        elif process.poll() is None:
            process.kill()
    process.wait(timeout=10)


def run_replay(mode, args, inputs, sources, directory, evidence):
    command = [sys.executable, str(REPLAY)]
    for key, path in inputs.items():
        command.extend(['--'+key, str(path)])
    for source in sources:
        command.extend(['--source', str(source)])
    command.extend(['--output', str(directory / mode), '--timeout', str(args.timeout)])
    if mode == 'observe':
        command.append('--observe')
    record = dict(mode=mode, command=command, timeout_seconds=min(220, args.timeout+20),
                  closed=False, returncode=None)
    evidence['replays'].append(record)
    process = None
    with (directory / (mode+'.log')).open('wb') as log:
        try:
            options = dict(stdout=log, stderr=subprocess.STDOUT)
            if sys.platform == 'win32':
                options['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                options['start_new_session'] = True
            process = subprocess.Popen(command, **options)
            record['pid'] = process.pid
            process.wait(timeout=record['timeout_seconds'])
        finally:
            if process is not None:
                try:
                    force_owned_cleanup(process)
                except BaseException:
                    evidence['cleanup_errors'].append(traceback.format_exc())
                record['closed'] = process.poll() is not None
                record['returncode'] = process.returncode
    require(record['closed'] and record['returncode'] == 0, 'Replay failed in '+mode+' mode; see local log')
    return read_json(directory / mode / 'local-evidence.json')


def verify_run(value, mode, inputs, sources, replay_hash):
    require(value['passed'] and value['work_complete'] and not value.get('error') and not value['cleanup_errors'],
            'Replay did not qualify all work and cleanup in '+mode)
    require(value['observation_mode'] == ('runtime.observe' if mode == 'observe' else 'legacy'),
            'Replay used a different observation mode')
    require(len(value['processes']) == 2 and
            all(p['closed'] and p['returncode'] == 0 for p in value['processes']),
            'Each replay must own two clean native processes')
    require(value['runner_sha256'] == replay_hash and value['hashes']['runner'] == replay_hash,
            'Replay source changed before execution')
    require(value['source_hashes'] == {str(path): digest(path) for path in sources},
            'Replay source inputs differ')
    for key, path in inputs.items():
        require(value['hashes'][key] == digest(path), 'Replay artifact bytes differ: '+key)
    require(REQUIRED_CHECKS.issubset(value['checks']), 'Required independent game checks were removed')
    require(value['json_byte_definition'] == BYTE_DEFINITION, 'Different JSON byte-count definition')
    request_bytes, result_bytes = 0, 0
    methods = Counter()
    for call in value['calls']:
        require('result' in call and 'error' not in call, 'A passing replay contains a failed native call')
        request = json_bytes(dict(method=call['method'], params=call['params']))
        result = json_bytes(call['result'])
        require(call['request_body_json_bytes'] == request and call['result_body_json_bytes'] == result,
                'Recorded JSON body byte counts do not match actual results')
        request_bytes += request
        result_bytes += result
        methods[call['method']] += 1
    require(len(value['route']) == 13 and Counter(row['event'] for row in value['route']) == {'collect': 9, 'deliver': 4},
            'Recorded route omitted a delivery or collection continuation')
    if mode == 'observe':
        require(methods['runtime.observe'] > 0 and all(methods[name] == 0 for name in
                ('runtime.entity', 'runtime.component.get', 'runtime.component.query')),
                'Joined mode did not use its required native reads')
    else:
        require(methods['runtime.observe'] == 0, 'Default replay unexpectedly used joined reads')
    return dict(calls=len(value['calls']), method_counts=dict(sorted(methods.items())),
                request_body_json_bytes=request_bytes, result_body_json_bytes=result_bytes,
                owned_native_processes=2, cleanup_ok=True)


def reduction(before, after):
    return dict(legacy=before, observe=after, saved=before-after,
                reduction_percent=100*(before-after)/before if before else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'world', 'manifest', 'assembly', 'hostfxr', 'bridge', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--source', type=Path, action='append', default=[])
    parser.add_argument('--timeout', type=float, default=180,
                        help='Replay deadline per mode, 30..200 seconds; outer subprocess bound <=220')
    args = parser.parse_args()
    require(sys.platform in ('win32', 'linux') and not sys.flags.optimize, 'Use native Windows/Linux Python without optimization')
    require(30 <= args.timeout <= 200, 'Per-replay deadline must be 30..200 seconds')
    inputs = {name: getattr(args, name).resolve() for name in ('engine', 'world', 'manifest', 'assembly', 'hostfxr', 'bridge')}
    sources = [path.resolve() for path in args.source]
    require(all(path.is_file() for path in [*inputs.values(), *sources, REPLAY]), 'Every input must be an actual file')
    directory = args.output.resolve()
    require(not directory.exists(), 'Paired output must be a new directory')
    assets = Path(str(inputs['world'])+'.assets').resolve()
    require(assets not in (directory, *directory.parents), 'Output must be outside source world assets')
    directory.mkdir(parents=True)
    replay_hash = digest(REPLAY)
    input_hashes = {key: digest(path) for key, path in inputs.items()}
    source_hashes = {str(path): digest(path) for path in sources}
    evidence = dict(passed=False, replays=[], cleanup_errors=[], normalization=NORMALIZATION,
                    json_byte_definition=BYTE_DEFINITION, inputs={key:str(path) for key,path in inputs.items()},
                    source_hashes=source_hashes, runner_sha256=digest(__file__), replay_sha256=replay_hash,
                    started_unix=time.time())
    public = dict(format='poima.agent-workshop-observation-compare', version=1, passed=False,
                  normalization=NORMALIZATION, json_byte_definition=BYTE_DEFINITION,
                  limitations=['One synthetic native replay pair on the supplied game/artifacts.',
                    'RPC counts and reserialized JSON body sizes only; no timing, provider-token, FPS or success-rate conclusion.',
                    'Retains the replay fixture limitations; no new physical input, rendering or shipping qualification.'])
    try:
        values, results = {}, {}
        for mode in ('legacy', 'observe'):
            value = run_replay(mode, args, inputs, sources, directory, evidence)
            results[mode] = verify_run(value, mode, inputs, sources, replay_hash)
            values[mode] = value
        projections = {}
        for name in SNAPSHOTS:
            before, after = normalize(values['legacy'][name]), normalize(values['observe'][name])
            require(canonical(before) == canonical(after), 'Complete checked state differs: '+name)
            projections[name] = hashlib.sha256(canonical(before).encode('utf-8')).hexdigest()
        require(canonical(values['legacy']['route']) == canonical(values['observe']['route']), 'Actual native routes/ticks/IDs differ')
        require(canonical(values['legacy']['checks']) == canonical(values['observe']['checks']), 'Independent game checks differ')
        require(values['legacy']['asset_hashes'] == values['observe']['asset_hashes'], 'Source assets differ')
        require(results['observe']['calls'] < results['legacy']['calls'], 'Joined replay did not reduce native requests')
        require(digest(REPLAY) == replay_hash and input_hashes == {key:digest(path) for key,path in inputs.items()} and
                source_hashes == {str(path):digest(path) for path in sources}, 'Paired replay changed input bytes')
        require(not evidence['cleanup_errors'], 'Paired process cleanup failed')
        public.update(passed=True, hashes=dict(input_hashes, replay=replay_hash, comparison_runner=evidence['runner_sha256']),
            compared_snapshots=list(SNAPSHOTS), snapshot_sha256=projections, route_events=len(values['legacy']['route']),
            game_checks=len(values['legacy']['checks']), runs=results,
            reductions={key:reduction(results['legacy'][key],results['observe'][key]) for key in
                ('calls','request_body_json_bytes','result_body_json_bytes')})
        evidence['passed'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        evidence['finished_unix'] = time.time()
        evidence['public_summary'] = public
        (directory/'local-paired-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
        (directory/'public-summary.json').write_text(json.dumps(public,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(dict(passed=evidence['passed'],summary=str(directory/'public-summary.json'))))
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
