#!/usr/bin/env python3
"""Qualify a real Windows NativeAOT game export, relocation and Vulkan replay.

Uses only newly created fixtures. The host may have .NET installed: this proves
native execution diagnostics and a closed bundle, not clean-machine deployment.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from scene_capture import pixels


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}


def native_pe(path):
    """Check actual PE CLI directory, not just whether its filename looks native."""
    data = path.read_bytes()
    assert data[:2] == b'MZ', path
    pe = struct.unpack_from('<I', data, 60)[0]
    assert data[pe:pe+4] == b'PE\0\0', path
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    assert magic in (0x10b, 0x20b), path
    directory = optional + (112 if magic == 0x20b else 96)
    count = struct.unpack_from('<I', data, directory-4)[0]
    if count > 14:
        assert struct.unpack_from('<II', data, directory+14*8) == (0, 0), 'Managed IL payload: '+str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--relocation-root', type=Path)
    args = parser.parse_args()
    assert os.name == 'nt', 'Run with native Windows Python.'
    args.output = args.output.resolve()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    source = run / 'Source project'
    source.mkdir()
    world, manifest = source / 'world.json', source / 'project.json'
    record = dict(passed=False, gpu=args.gpu, test_sha256=sha(Path(__file__)), binary_sha256=sha(args.binary),
                  artifact_sha256=sha(args.artifact), commands=[], checks=[],
                  limitations=['Windows host has .NET installed; this is not a clean-machine qualification.',
                               'No physical input or audio-device qualification; deterministic replay uses native gameplay.',
                               'game.run exposes controller/camera states, not the door entity; bundle door behavior is compared through gameplay fields and pixels.'])
    uid = lambda n: f'{n:032x}'

    def command(executable, argv, data=None, cwd=None, env=None, success=True):
        result = subprocess.run([str(executable.resolve()), *map(str, argv)], input=data, cwd=cwd, env=env,
                                capture_output=True, text=True, encoding='utf-8', timeout=120)
        record['commands'].append(dict(command=[str(executable.resolve()), *map(str, argv)],
                                       cwd=str(cwd) if cwd else None, exit_code=result.returncode,
                                       stdout=result.stdout, stderr=result.stderr))
        assert (result.returncode == 0) == success, result.stdout+result.stderr
        return result

    def cli(executable, *argv, success=True, **kwargs):
        response = json.loads(command(executable, argv, success=success, **kwargs).stdout)
        assert response['status'] == ('ok' if success else 'error'), response
        return response.get('result')

    try:
        descriptor = json.loads(args.artifact.read_text(encoding='utf-8'))
        assert descriptor['target_os'] == 'Windows' and descriptor['type'] == 'Poima.Examples.DoorGame'
        shutil.copytree(args.artifact.parent, source / 'gameplay')
        local_descriptor = source / 'gameplay' / args.artifact.name
        values = {'OpenX': 2.4}  # Prove project values survive packaging; differs from Initialize's 2.2.
        sequence = [{'ticks': 120}, {'ticks': 150, 'move': [0, 1]},
                    {'ticks': 120, 'use': True}, {'ticks': 90, 'move': [0, 1]}]
        replay = run / 'replay.json'
        replay.write_text(json.dumps(sequence), encoding='utf-8')
        baseline_image = run / 'baseline.bmp'
        requests = []

        def request(method, **params):
            requests.append(dict(jsonrpc='2.0', id=len(requests)+1, method=method, params=params))
            return len(requests)

        fixture = json.loads((ROOT / 'examples/interaction-room.jsonl').read_text())
        request(fixture['method'], **fixture['params'])
        session = uuid.uuid4().hex
        request('runtime.start', session_id=session, revision=1)
        loaded = request('runtime.gameplay.load_native', session_id=session, request_id=uuid.uuid4().hex,
                         expected_tick=0, expected_revision=0, descriptor=str(local_descriptor),
                         expected_descriptor_sha256=sha(local_descriptor), values=values)
        played = request('runtime.play', session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
                         controller=uid(100), camera=uid(101), mode='replay', sequence=sequence, audio=False,
                         gpu=args.gpu, width=960, height=540, samples=4, path=str(baseline_image))
        entities = {uid(n): request('runtime.entity', session_id=session, tick=480, id=uid(n)) for n in (100, 101, 2)}
        game_state = request('runtime.gameplay.inspect', session_id=session, tick=480, include_schema=True)
        record['reference_requests'] = requests
        raw = command(args.binary, ['world', world], data=''.join(json.dumps(r)+'\n' for r in requests))
        replies = {r['id']: r for r in map(json.loads, raw.stdout.splitlines())}
        assert len(replies) == len(requests)
        assert all('result' in reply for reply in replies.values()), replies
        result = lambda ident: replies[ident]['result']
        baseline = result(played)
        assert baseline['success'] and baseline['tick'] == 480 and baseline['capture_written']
        assert baseline['nvrhi_errors'] == 0 and baseline['hardware']
        native_flags = dict(dynamic_code_supported=False, dynamic_code_compiled=False)
        assert result(loaded)['module']['native_diagnostics'] == native_flags
        baseline_module = result(game_state)['module']
        assert baseline_module['backend'] == 'native_aot' and baseline_module['native_diagnostics'] == native_flags
        assert baseline_module['values']['Activations'] == 1 and baseline_module['values']['Open'] == 1
        assert baseline_module['values']['OpenX'] == 2.4
        assert abs(result(entities[uid(2)])['world_matrix'][12]-2.4) < 1e-5
        assert result(entities[uid(100)])['world_matrix'][14] < -7
        document = dict(format='poima.project', version=2, project_id=uuid.uuid4().hex,
                        name='Native CSharp portable door', audio=False,
                        entry=dict(world=world.name, controller=uid(100), camera=uid(101)),
                        gameplay=dict(descriptor='gameplay/'+args.artifact.name, values=values))
        manifest.write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
        source_before = tree(source)
        inspected = cli(args.binary, 'project', 'inspect', manifest)
        assert inspected['gameplay']['backend'] == 'native_aot' and inspected['gameplay']['values'] == values
        exported = run / 'Exported native game'
        cli(args.binary, 'project', 'build', manifest, '--output', exported, '--runtime', args.runtime.resolve())
        assert tree(source) == source_before, 'Inspect/export changed the source project.'
        spec = json.loads((exported / 'game.json').read_text())
        assert spec['version'] == 2 and spec['gameplay']['values'] == values
        inventory = {item['path']: item for item in spec['files']}
        assert any(item['role'] == 'gameplay_library' for item in inventory.values())
        for relative, item in inventory.items():
            path = exported / relative
            assert path.stat().st_size == item['size'] and sha(path) == item['sha256']
            lower = path.name.lower()
            assert lower not in {'hostfxr.dll', 'coreclr.dll', 'clrjit.dll', 'system.private.corelib.dll',
                                 'poima.gameplay.dll', 'poima.managedbridge.dll'}
            assert not lower.endswith(('.deps.json', '.runtimeconfig.json', '.cs', '.csproj'))
            if path.suffix.lower() in ('.exe', '.dll'):
                native_pe(path)
        record['checks'].append('v2 export includes real NativeAOT image and values; every PE has no CLI directory; no development IL or hostfxr inventory')
        relocation_parent = (args.relocation_root or ROOT.parent).resolve()
        assert relocation_parent != ROOT and ROOT not in relocation_parent.parents
        relocation = Path(tempfile.mkdtemp(prefix='Poima Native AOT 试验 ', dir=relocation_parent))
        bundle = relocation / 'Relocated game'
        shutil.move(str(exported), bundle)
        detached_source = source.with_name('Source project unavailable at original path')
        source.rename(detached_source)
        assert not source.exists() and not exported.exists()
        unrelated = relocation / 'Unrelated working directory'
        unrelated.mkdir()
        executable = bundle / spec['engine']['executable']
        assert sha(executable) == sha(args.runtime / 'bin/poima.exe')
        environment = dict(os.environ)
        system_root = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system_root / 'System32')+';'+str(system_root)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')):
                environment.pop(key)
        frozen = tree(bundle)
        record.update(bundle=str(bundle), relocated_binary_sha256=sha(executable), bundle_inventory=frozen,
                      source_inventory=source_before, installed_runtime_inventory=tree(args.runtime))
        checked = cli(executable, 'game', 'inspect', bundle / 'game.json', cwd=unrelated, env=environment)
        assert checked['revision'] == 1 and checked['project_id'] == document['project_id']
        assert tree(bundle) == frozen
        capture, report = relocation / 'Native game.bmp', relocation / 'Native game report.json'
        bundled = cli(executable, 'game', 'run', bundle / 'game.json', '--gpu', args.gpu, '--replay', replay,
                      '--capture', capture, '--report', report, '--width', 960, '--height', 540, '--samples', 4,
                      cwd=unrelated, env=environment)
        assert json.loads(report.read_text()) == bundled
        playback = bundled['play']
        assert playback['success'] and playback['tick'] == 480 and playback['stop_reason'] == 'replay_complete'
        assert playback['hardware'] and playback['nvrhi_errors'] == 0 and playback['capture_written']
        module = bundled['gameplay']['module']
        assert module['backend'] == 'native_aot' and module['native_diagnostics'] == native_flags
        for field in ('schema', 'values', 'type', 'assembly_sha256', 'migration'):
            assert module[field] == baseline_module[field], (field, module[field], baseline_module[field])
        assert Path(module['native_library']).resolve().is_relative_to(bundle.resolve())
        for ident in (uid(100), uid(101)):
            expected, actual = dict(result(entities[ident])), dict(bundled['entities'][ident])
            expected.pop('session_id'); actual.pop('session_id')
            assert actual == expected, (actual, expected)
        assert pixels(capture) == pixels(baseline_image), 'Relocated native game differs from direct-source replay.'
        assert tree(bundle) == frozen and tree(detached_source) == source_before
        record['checks'] += ['relocated bundled executable runs with original project path absent, unrelated cwd and system-only PATH',
                             '480-tick native gameplay fields and player/camera states match direct reference',
                             'direct door opens to configured 2.4 units; final relocated pixels match exactly',
                             'inspect and launch leave every bundle and source file hash unchanged']
        # Corruption must fail before a new report/capture or native module load.
        packaged_descriptor = bundle / spec['gameplay']['descriptor']
        original = packaged_descriptor.read_bytes()
        try:
            packaged_descriptor.write_bytes(original+b' ')
            rejected_image, rejected_report = relocation / 'Rejected.bmp', relocation / 'Rejected.json'
            cli(executable, 'game', 'run', bundle / 'game.json', '--gpu', args.gpu, '--replay', replay,
                '--capture', rejected_image, '--report', rejected_report, cwd=unrelated, env=environment, success=False)
            assert not rejected_image.exists() and not rejected_report.exists()
        finally:
            packaged_descriptor.write_bytes(original)
        assert tree(bundle) == frozen
        record['checks'].append('tampered packaged gameplay descriptor rejected before output creation')
        record.update(passed=True, reference_play=baseline, reference_module=baseline_module,
                      reference_door=result(entities[uid(2)]), bundled_result=bundled,
                      images={str(path): sha(path) for path in (baseline_image, capture)})
    except BaseException as error:
        record['failure'] = repr(error)
        raise
    finally:
        evidence = run / 'evidence.json'
        evidence.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(evidence)


if __name__ == '__main__':
    main()
