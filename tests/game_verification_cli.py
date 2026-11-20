#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise gameplay verification on a newly exported starter game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, out = args.runtime.resolve(strict=True), args.output.resolve()
    out.mkdir()
    runtime = json.loads((root / 'runtime.json').read_text(encoding='utf-8'))
    engine = root / runtime['executable']

    def run(label, binary, *values, code=0):
        command = subprocess.run([binary, *values], capture_output=True, timeout=120)
        (out / f'{label}.stdout.json').write_bytes(command.stdout)
        (out / f'{label}.stderr.log').write_bytes(command.stderr)
        if command.returncode != code:
            raise RuntimeError(f'{label} exited {command.returncode}, expected {code}: {command.stdout[:768]!r}')
        return json.loads(command.stdout)

    def require(condition, message):
        if not condition:
            raise AssertionError(message)

    project = out / 'Starter'
    created = run('create', engine, 'project', 'create', project, '--name', 'Verification Starter')['result']
    manifest = Path(created['manifest'])
    source = run('source', engine, 'project', 'inspect', manifest)['result']
    controller = source['entry']['controller']
    bundle = out / 'Bundle'
    run('export', engine, 'project', 'build', manifest, '--output', bundle, '--runtime', root)
    project.rename(out / 'Original unavailable')
    bundle.rename(out / 'Relocated game')
    bundle = out / 'Relocated game'
    game = bundle / 'game.json'
    packaged = bundle / 'runtime' / runtime['executable']
    inventory = {str(path.relative_to(bundle)): sha(path) for path in bundle.rglob('*') if path.is_file()}

    requests = [{'jsonrpc': '2.0', 'id': 1, 'method': 'world.describe', 'params': {'view': 'full'}},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'project.verification.write', 'params': {}}]
    session = subprocess.run([packaged, 'game', 'stdio', game],
                             input=''.join(json.dumps(request) + '\n' for request in requests),
                             capture_output=True, text=True, timeout=30, check=True)
    (out / 'read-only.stdout.ndjson').write_text(session.stdout, encoding='utf-8')
    description, refused = [json.loads(line) for line in session.stdout.splitlines()]
    require(all(method not in description['result']['methods'] for method in
                ('project.verification.read', 'project.verification.write')), 'Packaged world advertises suite authoring')
    require(refused['error']['code'] == -32081, 'Packaged world did not reject suite authoring')

    def expect(label, path, comparison, value):
        return {'expect': {'label': label, 'method': 'runtime.entity', 'params': {'id': controller},
                           'path': path, comparison: value}}

    def suite(path, groups):
        path.write_text(json.dumps({'format': 'poima.game-verification', 'version': 1, 'groups': groups}) + '\n',
                        encoding='utf-8')

    first = {'name': 'Walk forward', 'sequence': [
        {'step': {'ticks': 15, 'inputs': [{'entity': controller, 'move': [0, 1]}]}},
        expect('Walk advances the controller', '/world_matrix/14', 'between', [0.5, 1.8]),
    ]}
    second = {'name': 'Start fresh', 'sequence': [expect('Independent start position', '/world_matrix/14', 'equals', 2)]}
    specification, report = out / 'pass.json', out / 'report.json'
    suite(specification, [first, second])
    params = ('game', 'verify', game, '--suite', specification)
    passed = run('passed', packaged, *params, '--report', report)['result']
    require(passed == json.loads(report.read_text(encoding='utf-8')) and passed['passed'], 'Wrong published report')
    require(passed['bundle_sha256'] == inventory['game.json'] and passed['suite_sha256'] == sha(specification),
            'Report identity differs from the verified inputs')
    require(passed['groups_passed'] == passed['groups_total'] == 2 and passed['groups'][0]['ticks'] == 15,
            'Passed response lacks completed native steps')
    require(run('passed-repeat', packaged, *params)['result'] == passed, 'Same suite reported different observations')
    report_sha = sha(report)
    require(run('report-exists', packaged, *params, '--report', report, code=4)['result'] is None and sha(report) == report_sha,
            'Existing report was reused or overwritten')
    require(run('wrong-executable', engine, *params, code=4)['result'] is None,
            'External executable claimed bundle verification')
    unsafe = bundle / 'unexpected.json'
    require(run('bundle-report', packaged, *params, '--report', unsafe, code=4)['result'] is None and not unsafe.exists(),
            'Verifier wrote into the immutable bundle')

    fail_spec = out / 'fail.json'
    suite(fail_spec, [{'name': 'Wrong expected values', 'sequence': [
        expect('Incorrect position', '/world_matrix/14', 'equals', 500),
        expect('Absent field differs from null', '/missing', 'equals', None),
    ]}, second])
    failure = run('failed', packaged, 'game', 'verify', game, '--suite', fail_spec, code=4)['result']
    require(not failure['passed'] and failure['groups_passed'] == 1 and failure['groups_total'] == 2,
            'A failed group was ignored or prevented independent verification')
    require(failure['groups'][0]['checks'][0]['actual'] == 2 and not failure['groups'][0]['checks'][1]['observed'],
            'Failure observations are incomplete')
    malformed = out / 'duplicate.json'
    malformed.write_text(specification.read_text(encoding='utf-8').replace('"version": 1', '"version": 1, "version": 1'),
                         encoding='utf-8')
    require(run('duplicate', packaged, 'game', 'verify', game, '--suite', malformed, code=4)['result'] is None,
            'Duplicate field was admitted')
    suite(malformed, [{'name': 'Mutation', 'sequence': [{'expect': {'label': 'Set state',
          'method': 'runtime.gameplay.edit', 'path': '', 'equals': {}}}]}])
    require(run('mutation', packaged, 'game', 'verify', game, '--suite', malformed, code=4)['result'] is None,
            'A mutation was admitted as an expectation')
    require(inventory == {str(path.relative_to(bundle)): sha(path) for path in bundle.rglob('*') if path.is_file()},
            'Qualification modified the relocated bundle')
    print(json.dumps({'passed': True, 'platform': os.name, 'passed_groups': passed['groups_passed'],
                      'bundle_sha256': passed['bundle_sha256'], 'bundle_files': len(inventory)}))


if __name__ == '__main__':
    main()
