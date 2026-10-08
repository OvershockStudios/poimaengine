#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Export, relocate and execute the actual Windows collection Native AOT game.

The unchanged headless collection contract runs separately through the bundled
runtime. Its save checks are not claims about in-player inventory/save UX.
"""
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
import time
import traceback
import uuid

from collection_gameplay_contract import GAME, TYPE, uid, world

ROOT = Path(__file__).resolve().parents[1]
MAX_RESPONSE = 16 * 1024 * 1024


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(root):
    return {path.relative_to(root).as_posix(): sha(path)
            for path in sorted(root.rglob('*')) if path.is_file()}


def read_json(path):
    with Path(path).open('rb') as stream:
        data = stream.read(MAX_RESPONSE + 1)
    check(len(data) <= MAX_RESPONSE, 'JSON output exceeds bounded read size')
    return json.loads(data.decode('utf-8'))


def managed_pe(path):
    """Check the PE COM descriptor, rather than relying only on DLL filenames."""
    with path.open('rb') as stream:
        header = stream.read(64)
        if header[:2] != b'MZ':
            return False
        check(len(header) == 64, 'Truncated PE header in bundle')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        coff = stream.read(24)
        check(len(coff) == 24 and coff[:4] == b'PE\0\0', 'Malformed PE header in bundle')
        optional_bytes = struct.unpack_from('<H', coff, 20)[0]
        check(2 <= optional_bytes <= 4096, 'Invalid PE optional header size')
        optional = stream.read(optional_bytes)
        check(len(optional) == optional_bytes, 'Truncated PE optional header')
        magic = struct.unpack_from('<H', optional)[0]
        check(magic in (0x10b, 0x20b), 'Unsupported PE optional header')
        directories = 96 if magic == 0x10b else 112
        check(len(optional) >= directories, 'Truncated PE directory count')
        count = struct.unpack_from('<I', optional, directories - 4)[0]
        if count <= 14:
            return False
        check(len(optional) >= directories + 15 * 8, 'Truncated PE COM descriptor')
        address, size = struct.unpack_from('<II', optional, directories + 14 * 8)
        return address != 0 or size != 0


def shipping_world(manifest):
    document = world(manifest)
    entities = document['entities']

    def entity(number, name, position, scale=(1, 1, 1), parent=None):
        value = dict(name=name, parent=parent, components=dict(Transform=dict(
            position=list(position), rotation=[0, 0, 0, 1], scale=list(scale))))
        entities[uid(number)] = value
        return value['components']

    player = entity(100, 'Player', (0, 1, 2))
    player['CharacterController'] = dict(radius=.3, height=1.8, speed=4,
                                       jump_speed=5, camera=uid(101))
    camera = entity(101, 'Player camera', (0, 1.6, 0), parent=uid(100))
    camera['Camera'] = dict(vertical_fov=70, near=.1, far=200)
    floor = entity(102, 'Floor', (0, -.5, 0), (20, 1, 20))
    floor['MeshRenderer'] = dict(primitive='box', visible=True, albedo=[.14, .2, .24])
    floor['BoxCollider'] = dict(half_extents=[.5, .5, .5], motion='static',
                               mass=10, friction=.5, restitution=0)
    reference = entity(103, 'Visible reference', (0, 1, -4), (2, 2, 2))
    reference['MeshRenderer'] = dict(primitive='box', visible=True, albedo=[.44, .14, .23])
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    args = parser.parse_args()
    check(os.name == 'nt', 'Run this qualification with native Windows Python')
    check(not sys.flags.optimize, 'Run without Python optimization')
    check(0 <= args.gpu <= 4095, 'GPU index must be 0..4095')
    binary = args.binary.resolve(strict=True)
    distribution = args.runtime.resolve(strict=True)
    artifact_input = args.artifact.resolve(strict=True)
    output = args.output.resolve()
    check(not output.exists(), 'Output directory must be new')
    output.mkdir(parents=True)
    logs = output / 'logs'
    logs.mkdir()
    record = dict(passed=False, runner_sha256=sha(__file__), binary_sha256=sha(binary),
                  artifact_sha256=sha(artifact_input), gpu=args.gpu, commands=[], checks=[],
                  cleanup_errors=[], limitations=[
                      'This Windows host has .NET installed; not a clean-machine deployment test.',
                      'Seven semantic replay ticks on one GPU; no physical controls, production performance or broad graphics qualification.',
                      'The separate headless save contract uses its own fixture world through the bundled runtime, not in-player save UX.',
                      'The supplied publication/runtime inputs remain untouched; the owned project and its copied artifact are removed before execution.',
                      'Collection gameplay covers entity/int32 capacity-four buffers, not all scalar kinds or capacity migration.'])
    deadline = time.monotonic() + 300

    def execute(command, cwd, env=None, timeout=120):
        index = len(record['commands'])
        stdout = logs / ('command-' + str(index) + '.stdout')
        stderr = logs / ('command-' + str(index) + '.stderr')
        entry = dict(command=list(map(str, command)), cwd=str(cwd), exit_code=None,
                     timed_out=False, forced_cleanup=False, stdout=str(stdout), stderr=str(stderr))
        record['commands'].append(entry)
        remaining = deadline - time.monotonic()
        check(remaining > 0, 'Overall qualification deadline exceeded')
        process = None
        with stdout.open('wb') as out, stderr.open('wb') as err:
            try:
                process = subprocess.Popen(entry['command'], cwd=cwd, env=env,
                                           stdin=subprocess.DEVNULL, stdout=out, stderr=err)
                entry['pid'] = process.pid
                try:
                    process.wait(timeout=min(timeout, remaining))
                except subprocess.TimeoutExpired:
                    entry['timed_out'] = True
                    raise
            finally:
                if process is not None:
                    if process.poll() is None:
                        entry['forced_cleanup'] = True
                        killer = Path(os.environ['SYSTEMROOT']) / 'System32' / 'taskkill.exe'
                        try:
                            killed = subprocess.run([str(killer), '/PID', str(process.pid), '/T', '/F'],
                                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
                            entry['tree_cleanup_exit_code'] = killed.returncode
                        except BaseException:
                            record['cleanup_errors'].append(traceback.format_exc())
                        try:
                            if process.poll() is None:
                                process.kill()
                            process.wait(timeout=10)
                        except BaseException:
                            record['cleanup_errors'].append(traceback.format_exc())
                    entry['exit_code'] = process.poll()
        check(not entry['forced_cleanup'] and entry['exit_code'] == 0,
              'Owned command failed or required forced cleanup; see recorded log')
        return stdout

    def cli(executable, *arguments, cwd=None, env=None):
        result = read_json(execute([str(executable), *map(str, arguments)], cwd or output, env))
        check(result.get('status') == 'ok' and 'result' in result, 'CLI returned an unsuccessful envelope')
        record['commands'][-1]['result'] = result['result']
        return result['result']

    try:
        descriptor = read_json(artifact_input)
        fixture = Path(__file__).with_name('collection_gameplay')
        record['fixture_sources'] = {name: sha(fixture / name) for name in
                                     ('CollectionGameplay.cs', 'Poima.CollectionGameplay.csproj')}
        check(descriptor['version'] == 2 and descriptor['type'] == GAME and
              descriptor['target_os'] == 'Windows' and descriptor['target_arch'] == 'x86_64',
              'Expected actual Windows collection fixture artifact')
        check(descriptor['call_version'] == 1 and descriptor['call_bytes'] == 80 and
              descriptor['services_version'] == 7 and descriptor['minimum_services_bytes'] == 176,
              'Native gameplay compatibility requirement differs')
        check(descriptor['required_features'] == ['baseline_v7', 'component_collections_v1'],
              'Collection artifact feature requirements differ')
        check([row['path'] for row in descriptor['files'] if row['role'] == 'metadata'] ==
              ['game.poima-components.json'], 'Artifact lacks its sole component metadata payload')
        manifest = read_json(artifact_input.parent / 'game.poima-components.json')
        check(manifest['schemas'] == descriptor['schema']['components'], 'Published component manifest differs')
        fixture_world = shipping_world(manifest)
        runtime_before = inventory(distribution)
        record['runtime_inventory'] = runtime_before
        record['runtime_descriptor_sha256'] = sha(distribution / 'runtime.json')
        project = output / 'Owned source project'
        cli(binary, 'project', 'create', project, '--name', 'Packaged collection acceptance')
        project_file = project / 'project.json'
        project_spec = read_json(project_file)
        (project / project_spec['entry']['world']).write_text(json.dumps(fixture_world, indent=2) + '\n', encoding='utf-8')
        project_spec.update(version=2, entry=dict(world='world.json', controller=uid(100), camera=uid(101)),
                            gameplay=dict(descriptor='gameplay/native-gameplay.json', values=dict(Mode=1)))
        owned_artifact = project / 'gameplay'
        owned_artifact.mkdir()
        shutil.copyfile(artifact_input, owned_artifact / 'native-gameplay.json')
        for row in descriptor['files']:
            relative = Path(row['path'])
            check(not relative.is_absolute() and '..' not in relative.parts, 'Artifact payload path escapes root')
            source = (artifact_input.parent / relative).resolve(strict=True)
            check(source.is_relative_to(artifact_input.parent) and source.stat().st_size == row['size'] and
                  sha(source) == row['sha256'], 'Published artifact payload integrity differs')
            destination = owned_artifact / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        project_file.write_text(json.dumps(project_spec, indent=2) + '\n', encoding='utf-8')
        source_before = inventory(project)
        cli(binary, 'project', 'inspect', project_file)
        exported = output / 'Exported game'
        cli(binary, 'project', 'build', project_file, '--runtime', distribution, '--output', exported)
        check(inventory(project) == source_before and inventory(distribution) == runtime_before,
              'Inspection/export mutated source or selected runtime')
        relocation = Path(tempfile.mkdtemp(prefix='Poima collection relocation ', dir=ROOT.parent))
        record['relocation_root'] = str(relocation)
        check(not relocation.is_relative_to(ROOT), 'Relocation must be outside the checkout')
        bundle = relocation / 'Relocated game'
        shutil.move(str(exported), bundle)
        shutil.rmtree(project)
        check(not project.exists() and not exported.exists(), 'Original owned source/export locations remain available')
        unrelated = relocation / 'Unrelated working directory'
        unrelated.mkdir()
        game_file = bundle / 'game.json'
        game = read_json(game_file)
        check(game['version'] == 2 and game['gameplay']['values'] == dict(Mode=1), 'Bundled launch values differ')
        frozen = inventory(bundle)
        check({key[len('runtime/'):]: value for key, value in frozen.items() if key.startswith('runtime/')} == runtime_before,
              'Bundled runtime differs from exact selected distribution')
        for row in descriptor['files']:
            check(frozen['gameplay/' + row['path']] == row['sha256'], 'Bundled AOT payload differs')
        check(frozen['gameplay/native-gameplay.json'] == record['artifact_sha256'], 'Bundled artifact descriptor differs')
        forbidden = {'hostfxr.dll', 'libhostfxr.so', 'coreclr.dll', 'libcoreclr.so',
                     'poima.gameplay.dll', 'poima.managedbridge.dll', 'poima.collectiongameplay.dll'}
        check(not any(Path(name).name.lower() in forbidden for name in frozen), 'Managed development runtime/assembly shipped')
        check(not any(managed_pe(bundle / name) for name in frozen), 'Bundle contains a managed PE image')
        installed = (bundle / game['engine']['executable']).resolve(strict=True)
        record['packaged_binary_sha256'] = sha(installed)
        environment = dict(os.environ)
        system = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system / 'System32') + ';' + str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')):
                environment.pop(key)
        cli(installed, 'game', 'inspect', game_file, cwd=unrelated, env=environment)
        replay = output / 'replay.json'
        replay.write_text(json.dumps([dict(ticks=7)]) + '\n', encoding='utf-8')
        report = output / 'game-report.json'
        capture = output / 'game.bmp'
        storage = output / 'Player external saves'
        storage.mkdir()
        played = cli(installed, 'game', 'run', game_file, '--save-root', storage,
                     '--replay', replay, '--gpu', args.gpu, '--samples', '1', '--width', '512',
                     '--height', '288', '--capture', capture, '--report', report, cwd=unrelated, env=environment)
        check(played['success'] and played['runtime']['tick'] == played['play']['tick'] == 7,
              'Bundled player failed to complete seven ticks')
        check(played['play']['success'] and played['play']['stop_reason'] == 'replay_complete' and
              played['play']['runtime_replacements'] == 0 and played['play']['nvrhi_errors'] == 0,
              'Bundled replay stopped early or reported renderer/lifecycle errors')
        check(played['play']['hardware'] and played['runtime']['session_id'] ==
              played['play']['current_session_id'] == played['play']['initial_session_id'],
              'Bundled replay did not retain its hardware renderer/runtime session')
        module = played['gameplay']['module']
        check(module['backend'] == 'native_aot' and module['native_diagnostics'] ==
              dict(dynamic_code_supported=False, dynamic_code_compiled=False), 'Bundled execution is not Native AOT')
        check(module['schema']['components'] == manifest['schemas'], 'Loaded module collection schemas differ')
        values = module['values']
        for name, expected in dict(Mode=1, Ticks=7, Started=1, ObservedCount=4,
                                   FullRejected=2, PublishedRead=1, Inventories=1).items():
            check(values[name] == expected, 'Bundled gameplay counter differs: ' + name)
        spawned = [values[name] for name in ('FirstItem', 'SecondItem', 'ThirdItem', 'LastItem')]
        check(len(set(spawned)) == 4 and all(item != uid(0) and item not in fixture_world['entities'] for item in spawned),
              'Bundled compiled spawn identities differ')
        check(played['play']['capture_written'] and capture.is_file() and capture.stat().st_size > 54,
              'Actual Vulkan capture absent')
        check(read_json(report) == played and inventory(bundle) == frozen, 'Report differs or player modified immutable bundle')
        record['checks'].append('Real exported/relocated Native AOT player fills entity/int32 capacity-four buffers, rejects two full appends and preserves published reads during seven semantic replay ticks.')
        record['result'] = played
        record['capture_sha256'] = sha(capture)
        # This is a distinct headless acceptance world. The actual bundled engine
        # and descriptor are retained; this does not manufacture a player save UI.
        config = output / 'bundled-native-config.json'
        config.write_text(json.dumps(dict(descriptor=str(bundle / game['gameplay']['descriptor']))) + '\n', encoding='utf-8')
        verifier = Path(__file__).with_name('collection_gameplay_contract.py').resolve()
        verification = output / 'Bundled runtime headless contract'
        execute([sys.executable, str(verifier), '--binary', str(installed), '--config', str(config),
                 '--manifest', str(bundle / 'gameplay/game.poima-components.json'), '--output', str(verification)],
                unrelated, environment, timeout=180)
        evidence = read_json(verification / 'evidence.json')
        check(evidence['passed'] and not evidence['cleanup_errors'] and len(evidence['checks']) == 3 and
              len(evidence['calls']) == 151 and evidence['runner_sha256'] == sha(verifier) and
              evidence['hashes'][str(installed)] == record['packaged_binary_sha256'],
              'Unchanged headless collection contract failed')
        native_modules = [row['response']['result']['module'] for row in evidence['calls']
                          if row['request']['method'] == 'runtime.gameplay.inspect' and 'result' in row['response']]
        check(native_modules and all(module['backend'] == 'native_aot' and module['native_diagnostics'] ==
              dict(dynamic_code_supported=False, dynamic_code_compiled=False) for module in native_modules),
              'Separate bundled-runtime contract did not execute Native AOT')
        check(inventory(bundle) == frozen, 'Separate headless contract changed immutable bundle')
        record['headless_contract'] = dict(evidence_sha256=sha(verification / 'evidence.json'),
                                         runner_sha256=sha(verifier), calls=len(evidence['calls']),
                                         checks=evidence['checks'], final_values=evidence['final_values'],
                                         cleanup_errors=evidence['cleanup_errors'])
        record['checks'].append('Unchanged headless contract separately passes real buffer ordering, rejection, byte-exact rollback and two fresh-process save continuations through the bundled runtime/artifact.')
        record['checks'].append('Selected runtime and AOT payload hashes match after relocation; removed owned source, sanitized environment, no managed PE/hostfxr/CoreCLR payload, clean command exits and unchanged complete bundle inventory.')
        record['bundle_inventory'] = frozen
        record['passed'] = not record['cleanup_errors'] and all(row['exit_code'] == 0 and not row['forced_cleanup'] for row in record['commands'])
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        (output / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        print(output / 'evidence.json')
    if not record['passed']:
        raise SystemExit('Collection bundle qualification failed; see evidence.json')


if __name__ == '__main__':
    main()
