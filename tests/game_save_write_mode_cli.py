#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native headless CLI save-mode admission/forwarding; no rendering or gameplay ticks.

Use a matching-platform executable and an existing verified game bundle. An
external supervisor must bound the whole script and own descendant cleanup on
Windows. Emergency direct-child cleanup always fails this qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(root):
    paths = sorted(root.rglob('*'))
    need(len(paths) <= 2048, 'Bundle inventory exceeds fixture bound')
    result = {}
    for path in paths:
        st = path.lstat()
        need(not path.is_symlink() and not getattr(st, 'st_file_attributes', 0) & 0x400,
             'Linked fixture payload is unsupported')
        if path.is_file():
            result[path.relative_to(root).as_posix()] = (st.st_size, sha(path))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--client-root', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'tools/python')
    args = parser.parse_args()
    sys.path.insert(0, str(args.client_root.resolve()))
    from poima_client import WorldClient
    binary, game, output = args.engine.resolve(strict=True), args.game.resolve(strict=True), args.output.resolve()
    need(game.name == 'game.json', 'Fixture must be a verified exported game.json')
    fixture = json.loads(game.read_text(encoding='utf-8'))
    need(fixture.get('format') == 'poima.game' and fixture.get('version') == 1 and
         fixture.get('audio') is False and 'gameplay' not in fixture,
         'Use a primitive version-1 bundle without compiled gameplay or audio')
    need(output != game.parent and game.parent not in output.parents and output not in game.parents,
         'Output must not overlap the game bundle')
    output.mkdir(parents=True, exist_ok=False)
    before, binary_hash = inventory(game.parent), sha(binary)
    pins = {str(path): sha(path) for path in
            [Path(__file__).resolve(), *sorted((args.client_root / 'poima_client').glob('*.py')),
             *sorted((args.client_root / 'poima_client').glob('*.json'))]}
    evidence = dict(passed=False, commands=[], calls=[], forced_cleanup=[], cleanup_errors=[],
                    source_sha256=pins, binary_sha256=binary_hash, bundle_inventory=before,
                    limitations=['Launch admission and native save-mode forwarding only.',
                                 'No player, ticks, durable save, latency or paused blocking-player claim.'])

    def logs(path):
        need(path.stat().st_size <= 65536, 'Native output exceeded fixture bound')
        return path.read_text(encoding='utf-8')

    def force(process):
        if process.poll() is None:
            evidence['forced_cleanup'].append(process.pid)
            process.kill()
        process.wait(timeout=3)

    def terminal_cli(arguments, code, workers='2'):
        row = dict(command=[str(binary), *arguments], expected_exit=code)
        evidence['commands'].append(row)
        prefix = output / ('rejection-' + str(len(evidence['commands'])))
        env = dict(os.environ, POIMA_JOB_WORKERS=workers)
        with prefix.with_suffix('.stdout').open('wb') as stdout, prefix.with_suffix('.stderr').open('wb') as stderr:
            process = subprocess.Popen(row['command'], stdout=stdout, stderr=stderr, env=env, cwd=output)
            row['process_id'] = process.pid
            try:
                row['exit_code'] = process.wait(timeout=5)
            finally:
                if process.poll() is None:
                    force(process)
                row['exit_code'] = process.poll()
        row.update(stdout=logs(prefix.with_suffix('.stdout')), stderr=logs(prefix.with_suffix('.stderr')))
        reply = json.loads(row['stdout'])
        row['reply'] = reply
        need(row['exit_code'] == code and not row['stderr'] and
             reply['status'] == ('ok' if code == 0 else 'error'), row)
        return reply

    def rejected(arguments, code, message, workers='2'):
        reply = terminal_cli(arguments, code, workers)
        need(any(message in d['message'] for d in reply['diagnostics']), reply)
        if code == 2:
            need(any(d['code'] == 'invalid_arguments' for d in reply['diagnostics']), reply)

    def forwarded(mode, with_root=True):
        index = len(evidence['commands'])
        endpoint = 'save-mode-' + uuid.uuid4().hex
        storage = output / ('external-' + str(index))
        storage.mkdir()
        command = [str(binary), 'game', 'serve', str(game), '--endpoint', endpoint]
        if with_root:
            command += ['--save-root', str(storage)]
        if mode is not None:
            command += ['--save-write-mode', mode]
        row = dict(command=command)
        evidence['commands'].append(row)
        prefix = output / ('host-' + str(index))
        client = None
        with prefix.with_suffix('.stdout').open('wb') as stdout, prefix.with_suffix('.stderr').open('wb') as stderr:
            host = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                    env=dict(os.environ, POIMA_JOB_WORKERS='2'), cwd=output)
            row['process_id'] = host.pid
            try:
                deadline = time.monotonic() + 5
                expected = 'Poima shared world ready: ' + endpoint + '\n'
                while logs(prefix.with_suffix('.stderr')) != expected:
                    need(host.poll() is None, 'Native game host exited before readiness')
                    need(time.monotonic() < deadline, 'Native host readiness timed out')
                    time.sleep(.01)
                client = WorldClient.connect(binary, endpoint, timeout_ms=3000, close_timeout=3)
                row['client'] = dict(process_id=client.transport.process_id)
                for method in ('save.status', 'runtime.status', 'player.inspect'):
                    result = client.call(method, timeout=3)
                    evidence['calls'].append(dict(host_index=index, method=method, params={}, result=result))
                    if method == 'save.status':
                        need(result == dict(generation=1 if with_root else 0,
                            root=str(storage) if with_root else None, write_mode=mode or 'synchronous'), result)
                    elif method == 'runtime.status':
                        need(result['active'] and result['tick'] == 0, result)
                    else:
                        need(not result['active'], result)
                final_runtime = client.call('runtime.status', timeout=3)
                evidence['calls'].append(dict(host_index=index, method='runtime.status', params={}, result=final_runtime))
                need(final_runtime['active'] and final_runtime['tick'] == 0, 'Admission advanced native simulation')
                closed = client.call('host.shutdown', timeout=3)
                evidence['calls'].append(dict(host_index=index, method='host.shutdown', params={}, result=closed))
                need(closed['closed'], 'Host did not accept shutdown')
                client.close()
                row['client'].update(exit_code=client.transport.returncode, stderr=client.transport.stderr_tail,
                                     stderr_truncated=client.transport.stderr_truncated)
                need(client.transport.returncode == 0 and not client.transport.stderr_tail and
                     not client.transport.stderr_truncated, 'Client did not exit cleanly')
                client = None
                row['exit_code'] = host.wait(timeout=5)
            finally:
                if client is not None:
                    try:
                        client.close()
                    except Exception as error:
                        evidence['cleanup_errors'].append(repr(error))
                    finally:
                        row['client'].update(exit_code=client.transport.returncode, stderr=client.transport.stderr_tail,
                                             stderr_truncated=client.transport.stderr_truncated)
                if host.poll() is None:
                    force(host)
                row['exit_code'] = host.poll()
        row.update(stdout=logs(prefix.with_suffix('.stdout')), stderr=logs(prefix.with_suffix('.stderr')))
        need(row['exit_code'] == 0 and not row['stdout'] and row['stderr'] == expected, row)
        need(not list(storage.iterdir()), 'Admission unexpectedly wrote save payloads')

    try:
        schema = terminal_cli(['schema', 'game'], 0)['result']
        option = schema['properties']['save_write_mode']
        need(option['enum'] == ['synchronous', 'background'] and option['default'] == 'synchronous' and
             schema['dependentRequired']['save_write_mode'] == ['save_root'],
             'Native CLI discovery omits the exact save-mode contract')
        help_result = terminal_cli(['help'], 0)['result']
        usage = next(row['arguments'] for row in help_result['commands'] if row['name'] == 'game')
        need(usage.count('--save-write-mode synchronous|background') == 2,
             'Both launch forms must expose save mode in native help')
        missing = output / 'missing-game.json'
        external = output / 'parser-root'
        external.mkdir()
        # Invalid combinations must fail at the CLI boundary before missing-game admission.
        for action in ('run', 'serve'):
            base = ['game', action, str(missing)]
            if action == 'serve':
                base += ['--endpoint', 'save-mode-invalid-' + uuid.uuid4().hex]
            for mode in ('bad', '', 'Background', 'null'):
                rejected(base + ['--save-root', str(external), '--save-write-mode', mode], 2,
                         'Save write mode' if mode or action == 'run' else 'nonempty values')
            for mode in ('synchronous', 'background'):
                rejected(base + ['--save-write-mode', mode], 2, 'requires --save-root')
            rejected(base + ['--save-root', str(external), '--save-write-mode', 'background',
                              '--save-write-mode', 'background'], 2,
                     'repeated' if action == 'run' else 'Game serve accepts')
            rejected(base + ['--save-root', str(external), '--save-write-mode'], 2, '')
        for mode in (None, 'synchronous', 'background'):
            forwarded(mode)
        forwarded(None, with_root=False)
        serve = ['game', 'serve', str(game), '--endpoint', 'save-mode-root-' + uuid.uuid4().hex]
        for mode in ('synchronous', 'background'):
            rejected(serve + ['--save-root', str(output / 'absent'), '--save-write-mode', mode], 4, 'existing ordinary directories')
            rejected(serve + ['--save-root', str(game.parent), '--save-write-mode', mode], 4, 'immutable game bundle')
        rejected(serve + ['--save-root', str(external), '--save-write-mode', 'background'], 4,
                 'at least one job worker', workers='0')
        need(not list(external.iterdir()), 'Rejected launch wrote external data')
        need(not evidence['forced_cleanup'] and not evidence['cleanup_errors'], 'Cleanup was not normal')
        need(inventory(game.parent) == before and sha(binary) == binary_hash and
             all(sha(Path(path)) == digest for path, digest in pins.items()), 'Immutable inputs changed')
        evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        (output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print('Headless save-mode CLI: admission/default/forwarding/root guards passed')


if __name__ == '__main__':
    main()
