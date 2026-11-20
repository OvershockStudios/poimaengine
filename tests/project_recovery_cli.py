#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the native checkpoint and recovery workflow with an installed runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, out = args.runtime.resolve(strict=True), args.output.resolve()
    out.mkdir()
    runtime = json.loads((root / 'runtime.json').read_text(encoding='utf-8'))
    engine = root / runtime['executable']

    def run(label, *values, binary=engine, code=0):
        use = subprocess.run([binary, *values], capture_output=True, timeout=120)
        (out / f'{label}.stdout.json').write_bytes(use.stdout)
        (out / f'{label}.stderr.log').write_bytes(use.stderr)
        if use.returncode != code:
            raise RuntimeError(f'{label} exited {use.returncode}, expected {code}: {use.stdout[:1024]!r}')
        return json.loads(use.stdout)['result']

    project = out / 'Starter'
    manifest = Path(run('created', 'project', 'create', project, '--name', 'Checkpoint Starter')['manifest'])
    original = run('original', 'project', 'inspect', manifest)
    creator_file = project / 'Design.txt'
    creator_file.write_bytes(b'Original editable design\n')
    (project / 'code/obj').mkdir(parents=True)
    (project / 'code/obj/project.assets.json').write_text('{}', encoding='utf-8')
    (project / '.git').mkdir()
    (project / '.git/config').write_text('Git metadata\n', encoding='utf-8')
    history = out / 'History'
    history.mkdir()
    suite = project / 'game.verify.json'
    suite.write_text(json.dumps({'format': 'poima.game-verification', 'version': 1, 'groups': [
        {'name': 'The player can move', 'sequence': [
            {'step': {'ticks': 15, 'inputs': [{'entity': original['entry']['controller'], 'move': [0, 1]}]}},
            {'expect': {'label': 'Player moved forward', 'method': 'runtime.entity',
                        'params': {'id': original['entry']['controller']}, 'path': '/world_matrix/14', 'between': [0.5, 1.8]}},
        ]},
    ]}), encoding='utf-8')
    ready_path = history / '20261120-0500-a'
    ready = run('checkpoint', 'project', 'checkpoint', manifest, '--output', ready_path)
    snapshot = Path(ready['source_manifest']).parent
    assert (snapshot / 'Design.txt').read_bytes() == creator_file.read_bytes()
    assert not (snapshot / 'code/obj').exists() and not (snapshot / '.git').exists()
    assert run('incomplete-history', 'project', 'history', history, '--project-id', original['project_id'])['latest'] is None
    bundle = ready_path / 'game'
    run('exported', 'project', 'build', ready['source_manifest'], '--output', bundle, '--runtime', root)
    verified = run('verified', 'game', 'verify', bundle / 'game.json', '--suite', snapshot / 'game.verify.json',
                   '--report', ready_path / 'checks.json', binary=bundle / 'runtime' / runtime['executable'])
    assert verified['passed']
    creator_file.write_bytes(b'A later working copy edit\n')
    newer_path = history / '20261120-0600-b'
    run('newer-unverified', 'project', 'checkpoint', manifest, '--output', newer_path)
    latest = run('history', 'project', 'history', history, '--project-id', original['project_id'])
    assert Path(latest['latest']['checkpoint']) == Path(ready['checkpoint']) and latest['skipped'] == 1
    assert latest['latest']['bundle_sha256'] == verified['bundle_sha256']
    report = ready_path / 'checks.json'
    saved_report = report.read_bytes()
    report.write_text(json.dumps({**verified, 'engine_version': '999.0.0'}), encoding='utf-8')
    try:
        mismatch = run('wrong-verifier-history', 'project', 'history', history, '--project-id', original['project_id'])
        assert mismatch['latest'] is None and mismatch['skipped'] == 2
        rejected = out / 'Wrong verifier version'
        run('reject-wrong-verifier', 'project', 'recover', ready['checkpoint'], '--output', rejected, code=4)
        assert not rejected.exists()
    finally:
        report.write_bytes(saved_report)
    run('reject-existing', 'project', 'recover', ready['checkpoint'], '--output', project, code=4)
    assert creator_file.read_bytes() == b'A later working copy edit\n'
    old = out / 'Original unavailable'
    project.rename(old)
    cover = snapshot / 'Design.txt'
    before = cover.read_bytes()
    cover.write_bytes(b'Tampered archived design\n')
    try:
        missing = run('changed-history', 'project', 'history', history, '--project-id', original['project_id'])
        assert missing['latest'] is None and missing['skipped'] == 2
        run('reject-changed', 'project', 'recover', ready['checkpoint'], '--output', out / 'Rejected', code=4)
        assert not (out / 'Rejected').exists()
    finally:
        cover.write_bytes(before)
    result = run('recovered', 'project', 'recover', ready['checkpoint'], '--output', out / 'Recovered')
    copy = run('copy', 'project', 'inspect', result['restored_manifest'])
    assert copy['world_sha256'] == original['world_sha256'] and copy['revision'] == original['revision']
    assert copy['project_id'] != original['project_id'] and copy['project_id'] == result['restored_project_id']
    paths = [Path(entry['path']).relative_to('source') for entry in json.loads(
        Path(ready['checkpoint']).read_text(encoding='utf-8'))['files']]
    for path in paths:
        if path == Path(manifest.name):
            continue
        assert (snapshot / path).read_bytes() == (out / 'Recovered' / path).read_bytes(), path
    source_spec = json.loads((snapshot / manifest.name).read_text(encoding='utf-8'))
    recovered_spec = json.loads(Path(result['restored_manifest']).read_text(encoding='utf-8'))
    assert recovered_spec == {**source_spec, 'project_id': copy['project_id']}
    assert (old / 'Design.txt').read_bytes() == b'A later working copy edit\n' and not project.exists()
    print(json.dumps({'passed': True, 'source_files': len(paths), 'bundle_sha256': hashlib.sha256(
        (bundle / 'game.json').read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
