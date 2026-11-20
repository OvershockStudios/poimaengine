#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise owned verification jobs with a real installed runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/python'))
from poima_client import WorldClient, RpcError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    runtime, root = args.runtime.resolve(strict=True), args.output.resolve()
    root.mkdir()
    engine = runtime / json.loads((runtime / 'runtime.json').read_text())['executable']
    projects, products = root / 'projects', root / 'products'
    projects.mkdir()
    products.mkdir()
    for name in ('Starter', 'Other'):
        subprocess.run([engine, 'project', 'create', projects / name, '--name', name],
                       check=True, capture_output=True, timeout=30)
    manifest = projects / 'Starter/project.poimaproject'
    profile = root / 'profile.json'
    profile.write_text(json.dumps({'format': 'poima.development-profiles', 'version': 1, 'profiles': [{
        'name': 'verification', 'project_root': str(projects), 'output_root': str(products),
        'python': str(engine), 'publisher': str(engine), 'dotnet': str(engine),  # Pinned only.
        'exporter': str(engine), 'runtime_root': str(runtime), 'environment': [],
        'target_rid': 'win-x64' if engine.suffix == '.exe' else 'linux-x64',
    }]}), encoding='utf-8')
    params = {'profile': 'verification', 'project': 'Starter/project.poimaproject'}

    with WorldClient.open(engine, project=manifest, development_profiles=profile,
                          trace_directory=root / 'trace') as client:
        def reject(method, request, code):
            try:
                client.call(method, request)
            except RpcError as problem:
                assert problem.code == code, problem
            else:
                raise AssertionError(f'{method} accepted invalid request')

        def terminal(receipt):
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                status = client.call('development.inspect', {'job_id': receipt['job_id'],
                                                             'include_output': False, 'include_process': True})
                if status['terminal']:
                    return status
                time.sleep(.05)
            raise AssertionError(f'Job {receipt["job_id"]} did not finish')

        def set_suite(expected, guard=None):
            suite = {'format': 'poima.game-verification', 'version': 1, 'groups': [{'name': 'Initial pose', 'sequence': [
                {'expect': {'label': 'Controller starts at the expected depth', 'method': 'runtime.entity',
                            'params': {'id': '0' * 31 + '2'}, 'path': '/world_matrix/14', 'equals': expected}},
            ]}]}
            return client.call('project.verification.write', {**params, 'expected_sha256': guard,
                                                              'text': json.dumps(suite)})['sha256']

        assert client.call('development.profiles')['profiles'][0]['verify']
        original_suite = set_suite(2)
        exported = client.call('development.export', {**params, 'request_id': uuid.uuid4().hex, 'output': 'candidate'})
        export_result = terminal(exported)
        assert export_result['state'] == 'succeeded', export_result
        bundle = Path(export_result['result']['manifest']).parent
        executable = bundle / 'runtime' / engine.relative_to(runtime)
        request = {**params, 'request_id': uuid.uuid4().hex, 'output': 'passed.json',
                   'export_job_id': exported['job_id'], 'expected_suite_sha256': original_suite}
        reject('development.verify', {**request, 'export_job_id': exported['job_id'] + 1000}, -32009)
        reject('development.verify', {**request, 'expected_suite_sha256': '0' * 64}, -32009)
        reject('development.verify', {**request, 'project': 'Other/project.poimaproject'}, -32009)
        reject('development.verify', {**request, 'output': 'candidate/unexpected.json'}, -32009)
        accepted = client.call('development.verify', request)
        passed = terminal(accepted)
        report = (products / 'passed.json').read_bytes()
        assert passed['state'] == 'succeeded' and passed['result']['passed'], passed
        assert passed['result']['suite_sha256'] == original_suite
        assert passed['result']['bundle_sha256'] == export_result['result']['manifest_sha256']
        assert passed['result']['report_sha256'] == hashlib.sha256(report).hexdigest()
        retry = client.call('development.verify', request)
        assert retry['replayed'] and retry['job_id'] == accepted['job_id'] and (products / 'passed.json').read_bytes() == report
        reject('development.verify', {**request, 'output': 'reused.json'}, -32009)

        failed_suite = set_suite(10, original_suite)
        rejected_check = terminal(client.call('development.verify', {**request, 'request_id': uuid.uuid4().hex,
                                                                       'output': 'rejected-check.json',
                                                                       'expected_suite_sha256': failed_suite}))
        assert rejected_check['state'] == 'succeeded' and rejected_check['exit_code'] == 0, rejected_check
        assert not rejected_check['result']['passed'] and rejected_check['result']['failures'][0]['actual'] == 2
        assert rejected_check['result']['suite_sha256'] == failed_suite

        original_executable = executable.read_bytes()
        executable.write_bytes(b'changed fixture executable')
        try:
            refused = terminal(client.call('development.verify', {**request, 'request_id': uuid.uuid4().hex,
                                                                   'output': 'must-not-run.json',
                                                                   'expected_suite_sha256': failed_suite}))
            assert refused['state'] == 'launch_failed' and refused['process'] is None, refused
            assert refused['result'] is None and not (products / 'must-not-run.json').exists()
        finally:
            executable.write_bytes(original_executable)
    print(json.dumps({'passed': True, 'pass': passed['result'], 'rejected_check': rejected_check['result'],
                      'tampered_bundle_started_no_child': True}))


if __name__ == '__main__':
    main()
