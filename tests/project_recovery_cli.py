#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the native checkpoint and recovery workflow with an installed runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from zipfile import ZipFile


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
    history = out / '.poima-history' / original['project_id']
    history.mkdir(parents=True)
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
    incomplete = run('incomplete-history', 'project', 'history', history, '--project-id', original['project_id'])
    assert incomplete['latest'] is None and incomplete['latest_check'] is None
    bundle = ready_path / 'game'
    run('exported', 'project', 'build', ready['source_manifest'], '--output', bundle, '--runtime', root)
    candidate_report = ready_path / 'candidate-checks.json'
    verified = run('verified', 'game', 'verify', bundle / 'game.json', '--suite', snapshot / 'game.verify.json',
                   '--report', candidate_report, binary=bundle / 'runtime' / runtime['executable'])
    assert verified['passed']
    pending = run('pending-retention', 'project', 'history', history, '--project-id', original['project_id'])
    assert pending['latest'] is None and pending['skipped'] == 1
    assert pending['latest_check']['passed'] and not pending['latest_check']['retained']
    assert pending['latest_check']['report_sha256'] == hashlib.sha256(candidate_report.read_bytes()).hexdigest()
    run('reject-unretained', 'project', 'recover', ready['checkpoint'], '--output', out / 'Unretained', code=4)
    assert not (out / 'Unretained').exists()
    creator_file.write_bytes(b'A later working copy edit\n')
    captured_report = candidate_report.read_bytes()
    keep = ('project', 'keep', manifest, '--checkpoint', ready['checkpoint'], '--expected-snapshot-sha256',
            pending['latest_check']['snapshot_sha256'])
    candidate_report.write_bytes(captured_report + b'\n')
    try:
        run('reject-changed-candidate', *keep, code=4)
        assert not (ready_path / 'checks.json').exists()
    finally:
        candidate_report.write_bytes(captured_report)
    kept = run('keep-after-verifier-exit', *keep)
    assert not kept['replayed'] and kept['bundle_sha256'] == verified['bundle_sha256']
    assert (ready_path / 'checks.json').read_bytes() == candidate_report.read_bytes() == captured_report
    assert run('replay-keep', *keep) == {**kept, 'replayed': True}
    assert creator_file.read_bytes() == b'A later working copy edit\n'
    newer_path = history / '20261120-0600-b'
    run('newer-unverified', 'project', 'checkpoint', manifest, '--output', newer_path)
    latest = run('history', 'project', 'history', history, '--project-id', original['project_id'])
    assert Path(latest['latest']['checkpoint']) == Path(ready['checkpoint']) and latest['skipped'] == 1
    assert latest['latest']['bundle_sha256'] == verified['bundle_sha256']
    assert latest['latest_check']['retained'] and latest['latest_check']['passed']
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

    # A failing verifier run, including a stopped group, has a report but no playable build.
    test_suite = json.loads((old / suite.name).read_text(encoding='utf-8'))
    movement = test_suite['groups'][0]['sequence']
    test_suite['groups'].extend([
        {'name': 'A distant marker', 'sequence': [
            movement[0], {'expect': {
                **movement[1]['expect'], 'label': 'Player reaches the marker', 'between': [100, 101],
            }},
        ]},
        {'name': 'A missing entity', 'sequence': [
            movement[0], {'expect': {
                **movement[1]['expect'], 'params': {'id': 'f' * 32},
            }},
        ]},
    ])
    (old / suite.name).write_text(json.dumps(test_suite), encoding='utf-8')
    failed_path = history / '20261120-0700-c'
    failure = run('failed-checkpoint', 'project', 'checkpoint', old / manifest.name, '--output', failed_path)
    failed_bundle = failed_path / 'game'
    run('failed-build', 'project', 'build', failure['source_manifest'], '--output', failed_bundle, '--runtime', root)
    failed_suite = Path(failure['source_manifest']).with_name(suite.name)
    failed_report = failed_path / 'candidate-checks.json'
    failed = run('failed-playtest', 'game', 'verify', failed_bundle / 'game.json', '--suite', failed_suite,
                 '--report', failed_report, binary=failed_bundle / 'runtime' / runtime['executable'], code=4)
    assert not failed['passed'] and failed['groups_passed'] == 1 and failed['groups_total'] == 3
    assert failed['groups'][1]['checks'][0]['observed'] and failed['groups'][2]['issue']['operation'] == 2
    mixed = run('failed-attempt-history', 'project', 'history', history, '--project-id', original['project_id'])
    assert mixed['latest'] == latest['latest'] and mixed['skipped'] == 2
    attempt = mixed['latest_check']
    assert Path(attempt['report']) == failed_report and Path(attempt['checkpoint']) == Path(failure['checkpoint'])
    assert not attempt['passed'] and not attempt['retained'] and attempt['groups_total'] == 3
    assert attempt['report_sha256'] == hashlib.sha256(failed_report.read_bytes()).hexdigest()
    assert attempt['bundle_sha256'] == failed['bundle_sha256']
    run('reject-keep-failed', 'project', 'keep', old / manifest.name, '--checkpoint', attempt['checkpoint'],
        '--expected-snapshot-sha256', attempt['snapshot_sha256'], code=4)
    assert not (failed_path / 'checks.json').exists()
    failed_report.rename(failed_path / 'checks.json')
    failed_report = failed_path / 'checks.json'
    run('reject-failed', 'project', 'recover', failure['checkpoint'], '--output', out / 'Failed', code=4)
    assert not (out / 'Failed').exists()
    manual = run('failed-manual-history', 'project', 'history', history, '--project-id', original['project_id'])
    assert Path(manual['latest_check']['report']) == failed_report
    assert manual['latest_check'] == {**attempt, 'report': manual['latest_check']['report']} and manual['latest'] == latest['latest']

    for suffix in 'abcd':
        (history / f'20261120-0800-{suffix}').mkdir()
    page = run('versions-first-page', 'project', 'versions', history, '--project-id', original['project_id'])
    assert page['skipped'] == 5 and page['next_before'] == newer_path.name
    assert len(page['entries']) == 1 and not page['entries'][0]['passed'] and not page['entries'][0]['retained']
    assert Path(page['entries'][0]['checkpoint']) == Path(failure['checkpoint'])
    params = ('project', 'versions', history, '--project-id', original['project_id'], '--before')
    older = run('versions-next-page', *params, page['next_before'])
    assert older['skipped'] == 0 and older['next_before'] is None and len(older['entries']) == 1
    selection, = older['entries']
    assert selection['retained'] and selection['directory'] == ready_path.name
    assert selection['bundle_sha256'] == verified['bundle_sha256'] and Path(selection['checkpoint']) == Path(ready['checkpoint'])
    run('reject-misplaced-version-cursor', *params, '../bad', code=4)

    # A harmless report serialization change is still a different selection.
    destination = out / 'Restored selected version'
    guarded = ('project', 'recover', selection['checkpoint'], '--output', destination,
               '--expected-snapshot-sha256', selection['snapshot_sha256'])
    report.write_bytes(saved_report+b'\n')
    try:
        run('reject-changed-version', *guarded, code=4)
        assert not destination.exists()
    finally:
        report.write_bytes(saved_report)
    selected = run('restore-selected-version', *guarded)
    assert selected['bundle_sha256'] == verified['bundle_sha256']
    assert (destination / 'Design.txt').read_bytes() == b'Original editable design\n'

    fabricated = json.loads(failed_report.read_text(encoding='utf-8'))
    fabricated['groups'][1]['checks'][0]['label'] = 'An assertion from a different suite'
    failed_report.write_text(json.dumps(fabricated), encoding='utf-8')
    ignored = run('mismatched-check-history', 'project', 'history', history, '--project-id', original['project_id'])
    assert ignored['latest'] == latest['latest'] and ignored['latest_check'] == latest['latest_check']

    archive = out / 'Playable game.zip'
    def share(label, output=archive, checkpoint=ready['checkpoint'], digest=verified['bundle_sha256'], code=0):
        return run(label, 'project', 'share', checkpoint, '--expected-bundle-sha256', digest, '--output', output, code=code)

    bad = out / 'Rejected ZIP.zip'
    share('reject-incorrect-share-hash', bad, digest='0' * 64, code=4)
    share('reject-failed-share', bad, checkpoint=failure['checkpoint'], code=4)
    assert not bad.exists()
    share('reject-share-within-checkpoint', ready_path / 'Inside.zip', code=4)
    assert not (ready_path / 'Inside.zip').exists()
    share('reject-invalid-share-extension', out / 'Wrong.txt', code=4)
    shared = share('share')
    assert Path(shared['archive']) == archive and shared['bytes'] == archive.stat().st_size
    assert shared['bundle_sha256'] == verified['bundle_sha256']
    assert shared['project_id'] == original['project_id'] and shared['revision'] == original['revision']
    def archive_hash(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    original_archive_hash = archive_hash(archive)
    share('reject-existing-share', code=4)
    assert archive_hash(archive) == original_archive_hash
    repeat = out / 'Repeat.zip'
    share('repeat-share', repeat)
    assert archive_hash(repeat) == original_archive_hash
    repeat.unlink()

    receipt = json.loads((bundle / 'game.json').read_bytes())
    expected = {'game.json': verified['bundle_sha256'], **{item['path']: item['sha256'] for item in receipt['files']}}
    relocated = out / 'Recipient - unpacked elsewhere'
    with ZipFile(archive) as portable:
        assert portable.testzip() is None
        assert len(portable.namelist()) == shared['files'] == len(expected)
        assert set(portable.namelist()) == set(expected)
        for name, digest in expected.items():
            with portable.open(name) as member:
                assert hashlib.file_digest(member, 'sha256').hexdigest() == digest, name
        portable.extractall(relocated)
    relocated_suite = out / 'original-checks.json'
    relocated_suite.write_bytes((snapshot / suite.name).read_bytes())
    ready_path.rename(out / 'Original checkpoint unavailable')
    assert not bundle.exists() and not project.exists()
    replayed = run('share-relocated-playtest', 'game', 'verify', relocated / 'game.json', '--suite', relocated_suite,
                   '--expected-bundle-sha256', verified['bundle_sha256'], binary=relocated / 'runtime' / runtime['executable'])
    assert replayed == verified
    assert not list(out.glob('.poima-stage-*'))
    print(json.dumps({'passed': True, 'source_files': len(paths), 'bundle_sha256': hashlib.sha256(
        (relocated / 'game.json').read_bytes()).hexdigest(), 'latest_failed_groups': failed['groups_total'] - failed['groups_passed'],
        'archive_sha256': original_archive_hash, 'archive_files': shared['files']}))


if __name__ == '__main__':
    main()
