#!/usr/bin/env python3
"""Windows GPU qualification of a relocated native game bundle, without .NET.

Builds a cooked interaction room, removes its original project location, and
runs the copied executable outside the checkout from an unrelated directory.
Captured outputs stay outside the immutable game directory.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid
from scene_capture import pixels

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gpu', type=int, default=0)
p.add_argument('--relocation-root', type=Path, help='Parent outside checkout; defaults to checkout parent drive.')
args = p.parse_args()
assert os.name == 'nt', 'Run this relocation harness with Windows Python.'
ROOT = Path(__file__).resolve().parents[1]
run = args.output/uuid.uuid4().hex
run.mkdir(parents=True)
source = run/'Source project'
source.mkdir()
world, manifest = source/'room.json', source/'project.json'
profile = source/'controls.poima-input.json'
record = {'passed': False, 'gpu': args.gpu, 'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'commands': [], 'requests': [], 'checks': [],
          'limitations': ['Native gameplay only; no managed runtime redistribution qualification.',
                         'Audio assets and logical sound commands are exercised with device output disabled.',
                         'Replay validates the saved profile but does not exercise physical remapped input.']}
relocation_parent = (args.relocation_root or ROOT.parent).resolve()
assert relocation_parent != ROOT and ROOT not in relocation_parent.parents, 'Relocation root must be outside the checkout.'
relocation = Path(tempfile.mkdtemp(prefix='Poima portable 试验 ', dir=relocation_parent))
unrelated = relocation/'Unrelated working directory'
unrelated.mkdir()
record['relocation'] = str(relocation)
binary = str(args.binary.resolve())

def uid(n):
    return f'{n:032x}'

def tree(root):
    return {str(p.relative_to(root)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file()}

def cli(executable, *argv, success=True, env=None, cwd=None, timeout=120):
    command = [str(executable), *map(str, argv)]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', env=env, cwd=cwd, timeout=timeout)
    record['commands'].append({'command': command, 'cwd': str(cwd) if cwd else None,
                              'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
    reply = json.loads(result.stdout)
    assert (result.returncode == 0) == success, reply
    assert reply['status'] == ('ok' if success else 'error'), reply
    if not success:
        assert reply['diagnostics'], reply
    return reply.get('result')

stderr_path = run/'author-stderr.txt'
stderr = stderr_path.open('w', encoding='utf-8')
author = subprocess.Popen([binary, 'world', str(world.resolve())], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=stderr, text=True, encoding='utf-8', bufsize=1)

def rpc(method, **params):
    request = {'jsonrpc': '2.0', 'id': len(record['requests'])+1, 'method': method, 'params': params}
    author.stdin.write(json.dumps(request)+'\n')
    author.stdin.flush()
    response = json.loads(author.stdout.readline())
    record['requests'].append({'request': request, 'response': response})
    assert 'result' in response, response
    return response['result']

try:
    used = rpc('asset.import', source=str((ROOT/'examples/assets/textured-sphere.glb').resolve()))['asset']
    unused = rpc('asset.import', source=str((ROOT/'examples/assets/material-sphere.glb').resolve()))['asset']
    sound = rpc('asset.audio.import', source=str((ROOT/'examples/assets/acoustic-probe.wav').resolve()))['asset']
    fixture = json.loads((ROOT/'examples/interaction-room.jsonl').read_text(encoding='utf-8'))
    fixture['params']['ops'] += [
        {'op': 'asset.instantiate', 'id': uid(300), 'asset': used, 'name': 'Cooked textured prop'},
        {'op': 'component.set', 'id': uid(300), 'type': 'Transform',
         'value': {'position': [-3, 1, -7], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}},
        {'op': 'component.set', 'id': uid(2), 'type': 'AudioEmitter',
         'value': {'asset': sound, 'gain': .25, 'loop': True, 'enabled': True}}]
    rpc(fixture['method'], **fixture['params'])
    controls = {'bindings': {'forward': ['key.up'], 'backward': ['key.down'], 'left': ['key.left'],
        'right': ['key.right'], 'jump': ['key.space'], 'use': ['mouse.3']},
        'sensitivity_x': .15, 'sensitivity_y': .2, 'invert_x': False, 'invert_y': True}
    configured = rpc('input.transact', path=str(profile.resolve()), request_id=uuid.uuid4().hex,
                     expected_revision=0, profile=controls)
    sequence = [{'ticks': 120}, {'ticks': 150, 'move': [0, 1]},
        {'ticks': 120, 'motions': [{'entity': uid(2), 'position': [2.2, 1.5, -3],
            'rotation': [0, 0, 0, 1], 'duration_ticks': 120}], 'sounds': [{'op': 'play', 'emitter': uid(2)}]},
        {'ticks': 90, 'move': [0, 1], 'sounds': [{'op': 'stop', 'voice': 1}]}]
    replay = run/'replay.json'
    replay.write_text(json.dumps(sequence), encoding='utf-8')
    baseline_image = run/'baseline.bmp'
    rpc('runtime.start', session_id=uid(900), revision=1)
    baseline = rpc('runtime.play', session_id=uid(900), request_id=uuid.uuid4().hex, expected_tick=0,
        controller=uid(100), camera=uid(101), mode='replay', sequence=sequence, input_profile=str(profile.resolve()),
        input_revision=1, audio=False, gpu=args.gpu, width=960, height=540, samples=4, path=str(baseline_image.resolve()))
    assert baseline['success'] and baseline['tick'] == 480 and baseline['nvrhi_errors'] == 0, baseline
    baseline_entities = {entity: rpc('runtime.entity', session_id=uid(900), tick=480, id=entity) for entity in [uid(100), uid(101)]}
    author.stdin.close()
    assert author.wait(timeout=15) == 0
    # A packaged project contains content, not authoring lock/backup artifacts.
    for sidecar in source.rglob('*.lock'):
        sidecar.unlink()
    document = {'format': 'poima.project', 'version': 1, 'project_id': uuid.uuid4().hex, 'name': 'Portable interaction room',
        'entry': {'world': world.name, 'controller': uid(100), 'camera': uid(101)}, 'audio': False, 'input_profile': profile.name}
    manifest.write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
    source_before = tree(source)
    inspected = cli(binary, 'project', 'inspect', manifest.resolve())
    assert inspected['revision'] == 1 and tree(source) == source_before
    exported = run/'Exported native game'
    cli(binary, 'project', 'build', manifest.resolve(), '--output', exported.resolve(), '--runtime', args.runtime.resolve())
    assert tree(source) == source_before, 'Export changed its source project.'
    assert any(p.name == used+'.pmodel' for p in exported.rglob('*'))
    assert any(p.name == sound+'.paudio' for p in exported.rglob('*'))
    assert not any(p.name == unused+'.pmodel' for p in exported.rglob('*')), 'Unreferenced model was exported.'
    assert not list(exported.rglob('*.lock')) and not list(exported.rglob('*.previous'))
    bundle = relocation/'Relocated game'
    shutil.move(str(exported), bundle)
    detached_source = source.with_name('Source project removed from original path')
    source.rename(detached_source)
    assert not source.exists() and not exported.exists()
    installed_executable = bundle/'bin'/'poima.exe'
    if not installed_executable.exists():
        candidates = list(bundle.rglob('poima.exe'))
        assert len(candidates) == 1, candidates
        installed_executable = candidates[0]
    game = bundle/'game.json'
    environment = dict(os.environ)
    system_root = Path(environment['SYSTEMROOT'])
    environment['PATH'] = str(system_root/'System32')+';'+str(system_root)
    for key in list(environment):
        if key.startswith(('DOTNET_', 'COREHOST_', 'POIMA_')):
            environment.pop(key)
    frozen = tree(bundle)
    checked = cli(installed_executable, 'game', 'inspect', game, env=environment, cwd=unrelated)
    assert checked['revision'] == 1 and checked['project_id'] == document['project_id'], checked
    assert tree(bundle) == frozen, 'Headless inspection altered the relocated game directory.'
    capture, report_path = relocation/'Relocated capture.bmp', relocation/'Run report.json'
    result = cli(installed_executable, 'game', 'run', game, '--gpu', args.gpu, '--replay', replay.resolve(),
        '--capture', capture, '--report', report_path, '--width', 960, '--height', 540, '--samples', 4,
        env=environment, cwd=unrelated)
    assert json.loads(report_path.read_text(encoding='utf-8')) == result
    played = result['play']
    assert played['success'] and played['tick'] == 480 and played['stop_reason'] == 'replay_complete', played
    assert played['hardware'] and played['nvrhi_errors'] == 0 and played['capture_written'], played
    assert result['runtime']['tick'] == 480 and not played['audio']['enabled']
    metadata = played['input_profile']
    assert metadata['source'] == 'profile' and metadata['revision'] == 1 and metadata['applied'] is False, metadata
    assert metadata['content_hash'] == configured['content_hash'], metadata
    for entity, expected in baseline_entities.items():
        actual = dict(result['entities'][entity])
        expected = dict(expected)
        actual.pop('session_id')
        expected.pop('session_id')
        assert actual == expected, (actual, expected)
    assert result['entities'][uid(100)]['world_matrix'][14] < -7, result['entities'][uid(100)]
    assert pixels(capture) == pixels(baseline_image), 'Relocated bundle differs from direct-source GPU replay.'
    assert tree(bundle) == frozen, 'Game launch created or changed files inside its bundle.'
    assert tree(detached_source) == source_before, 'Relocated execution touched the source project.'
    record['checks'] += ['referenced cooked model/audio/profile exported; unused model omitted',
        'relocated executable works outside checkout with unrelated cwd/system-only PATH',
        'immutable headless inspect and 480-tick game launch create no bundle-side files',
        'saved input profile hash, final entities and pixels match direct-source replay']
    cli(installed_executable, 'game', 'run', game, '--replay', replay.resolve(), '--capture', bundle/'forbidden.bmp',
        env=environment, cwd=unrelated, success=False)
    assert tree(bundle) == frozen
    packaged_world = next(p for p in bundle.rglob('*.json') if p.name != 'game.json' and
        '"poima.authored-world"' in p.read_text(encoding='utf-8', errors='replace'))
    original = packaged_world.read_bytes()
    packaged_world.write_bytes(original+b' ')
    tampered = tree(bundle)
    rejected_capture, rejected_report = relocation/'Rejected.bmp', relocation/'Rejected.json'
    cli(installed_executable, 'game', 'inspect', game, env=environment, cwd=unrelated, success=False)
    cli(installed_executable, 'game', 'run', game, '--replay', replay.resolve(), '--capture', rejected_capture,
        '--report', rejected_report, env=environment, cwd=unrelated, success=False)
    assert not rejected_capture.exists() and not rejected_report.exists()
    assert tree(bundle) == tampered, 'Integrity failure mutated the game bundle.'
    packaged_world.write_bytes(original)
    assert tree(bundle) == frozen
    record['checks'].append('in-bundle output and tampered content rejected before rendering/writing reports')
    record.update(passed=True, bundle=str(bundle), source_inventory=source_before, bundle_inventory=frozen,
        play=played, image_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(), final_runtime=result['runtime'])
except BaseException as error:
    record['failure'] = repr(error)
    raise
finally:
    if author.poll() is None:
        author.kill()
        author.wait(timeout=10)
    stderr.close()
    record['author_stderr'] = stderr_path.read_text(encoding='utf-8')
    record['author_exit_code'] = author.returncode
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    (args.output/f'game-bundle-gpu-{args.gpu}.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(run/'evidence.json')
