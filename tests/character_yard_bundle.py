#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify a real Windows Character Yard NativeAOT export and relocated player.

Source-free playback precedes the separately reconstructed bundled-runtime
pose/save contract. Requires supplied frozen artifacts; performs no builds,
downloads, pose edits or automatic mutation retries.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

from collection_gameplay_bundle import check, inventory, managed_pe, read_json, sha

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {'baseline_v7', 'component_collections_v1', 'animation_inertial_v1',
            'character_input_v1', 'navigation_query_v1'}


def load_launcher():
    path = ROOT/'examples/character-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_character_yard_bundle_launcher', path)
    check(spec is not None and spec.loader is not None, 'Character Yard launcher is missing')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def continuation(snapshot):
    result = json.loads(json.dumps(snapshot))
    for name in ('ObservedEpochHigh', 'ObservedEpochLow'):
        result['values'].pop(name)
    return result


def shipping_document(authored):
    # WorldSession::freeze_content clears authoring receipts and retired editor
    # identities. No entity, component, template, UI, asset or revision is removed.
    result = json.loads(json.dumps(authored))
    for name in ('receipts', 'retired_ids', 'retired_component_schemas',
                 'retired_template_ids', 'retired_ui_ids'):
        if name in result: result[name] = []
    return result


def artifact_inputs(path, launcher):
    descriptor = read_json(path)
    check(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2 and
          descriptor['type'] == launcher.TYPE and descriptor['identity'] == 'poima.examples.character-yard' and
          descriptor['target_os'] == 'Windows' and descriptor['target_arch'] == 'x86_64',
          'Supply the actual Windows Character Yard artifact')
    check((descriptor['call_version'], descriptor['call_bytes'], descriptor['services_version'],
           descriptor['minimum_services_bytes']) == (1, 80, 7, 224) and
          set(descriptor['required_features']) == REQUIRED and
          len(descriptor['required_features']) == len(REQUIRED), 'Character Yard artifact requirements differ')
    files = descriptor['files']
    check(1 <= len(files) <= 256, 'Artifact file count exceeds its contract')
    names, folded = set(), set()
    for row in files:
        relative = Path(row['path'])
        check(not relative.is_absolute() and relative.parts and '..' not in relative.parts and
              row['path'] not in names and row['path'].lower() not in folded, 'Invalid/duplicate artifact path')
        names.add(row['path']); folded.add(row['path'].lower())
        original = path.parent/relative
        check(not original.is_symlink(), 'Artifact payload must not be a symlink')
        payload = original.resolve(strict=True)
        check(payload.is_relative_to(path.parent) and payload.is_file() and
              payload.stat().st_size == row['size'] and sha(payload) == row['sha256'], 'Artifact payload bytes differ')
    check(set(inventory(path.parent)) == names | {path.name}, 'Artifact contains missing or unlisted payloads')
    libraries = [row for row in files if row['role'] == 'library']
    metadata = [row for row in files if row['role'] == 'metadata']
    notices = [row for row in files if row['role'] == 'notice']
    check(len(libraries) == 1 and libraries[0]['path'] == descriptor['library'] and
          len(metadata) == 1 and len(notices) >= 6, 'Actual artifact library/metadata/notices are incomplete')
    check(not managed_pe(path.parent/descriptor['library']), 'Gameplay library still contains managed IL')
    notice_names = {Path(row['path']).name for row in notices}
    check({'POIMA-LICENSE.txt', 'POIMA-THIRD-PARTY-NOTICES.md'} <= notice_names,
          'Poima notices are missing')
    for prefix in ('DOTNET-RUNTIME-', 'DOTNET-NATIVEAOT-'):
        check(any(name.startswith(prefix) and name.endswith('-LICENSE.txt') for name in notice_names) and
              any(name.startswith(prefix) and name.endswith('-THIRD-PARTY-NOTICES.txt') for name in notice_names),
              'Published .NET runtime license/notice pair is missing')
    manifest_path = path.parent/metadata[0]['path']
    manifest = read_json(manifest_path)
    check(manifest['format'] == 'poima.components' and manifest['version'] == 1 and
          manifest['schemas'] == descriptor['schema']['components'], 'Published component manifest/schema differ')
    schemas = {value['id']: value for value in manifest['schemas']}
    check(set(schemas) == {launcher.CONFIG, launcher.COMPONENT} and len(manifest['schemas']) == 2 and
          schemas[launcher.CONFIG]['version'] == 1 and schemas[launcher.COMPONENT]['version'] == 2,
          'Character Yard custom component identities differ')
    route = {field['id']: field for field in schemas[launcher.COMPONENT]['fields']}
    check(route[launcher.COORDINATES]['kind'] == 'array' and
          route[launcher.COORDINATES]['element_kind'] == 'float32' and
          route[launcher.COORDINATES]['capacity'] == 30 and route[launcher.CURSOR]['kind'] == 'int32',
          'Character Yard typed route schema differs')
    return descriptor, manifest, metadata[0]['path'], libraries[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'source', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--ticks', type=int, default=1400)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--timeout', type=float, default=900)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Use native Windows Python and Windows paths')
    if sys.flags.optimize: parser.error('Assertions must remain enabled in the independent contract')
    if not 0 <= args.gpu <= 4095 or not 1200 <= args.ticks <= 1800:
        parser.error('GPU must be0..4095 and replay ticks1200..1800')
    if not math.isfinite(args.timeout) or not 180 <= args.timeout <= 1200:
        parser.error('Timeout must be finite180..1200seconds')
    output = args.output.resolve()
    if output.exists(): parser.error('--output must be new; earlier evidence is preserved')
    output.mkdir(parents=True);logs = output/'logs';logs.mkdir()
    record = dict(passed=False, runner_sha256=sha(__file__), calls=[], owners=[], commands=[], checks=[], cleanup_errors=[],
        settings=dict(gpu=args.gpu, ticks=args.ticks, capture=args.capture),
        limitations=['Windows development host with .NET installed; not clean-machine deployment.',
            'Scripted Vulkan replay and renderer readbacks; no physical-input, GUI/editor, performance or general crowds qualification.',
            'The imported source has one stationary arm-opening motion; native routing is not foot-matched locomotion.',
            'Source-free player playback starts at tick zero and completes before separate reconstructed headless pose/save checks.',
            'The independent headless contract intentionally uses the preserved caller GLB to reconstruct a different world; it is not part of the source-free playback claim.'])
    deadline = time.monotonic()+args.timeout;author = None;protected = {}

    def execute(command, cwd=output, env=None, timeout=300):
        remaining = deadline-time.monotonic();check(remaining > 0, 'Overall qualification deadline expired')
        index = len(record['commands']);out = logs/f'command-{index}.stdout';err = logs/f'command-{index}.stderr'
        row = dict(command=list(map(str, command)), cwd=str(cwd), exit_code=None, timed_out=False,
                   forced_cleanup=False, stdout=str(out), stderr=str(err));record['commands'].append(row)
        process = None
        with out.open('wb') as stdout, err.open('wb') as stderr:
            try:
                process = subprocess.Popen(row['command'], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                           stdout=stdout, stderr=stderr)
                row['pid'] = process.pid
                try: process.wait(timeout=min(timeout, remaining))
                except subprocess.TimeoutExpired: row['timed_out'] = True;raise
            finally:
                if process is not None:
                    if process.poll() is None:
                        row['forced_cleanup'] = True
                        try:
                            result = subprocess.run([str(Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'),
                                '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
                            row['tree_cleanup_exit_code'] = result.returncode
                            if result.returncode != 0: record['cleanup_errors'].append('Process-tree cleanup failed: '+str(result.returncode))
                        except BaseException: record['cleanup_errors'].append(traceback.format_exc())
                        try:
                            if process.poll() is None: process.kill()
                            process.wait(timeout=10)
                        except BaseException: record['cleanup_errors'].append(traceback.format_exc())
                    row['exit_code'] = process.poll()
        row.update(stdout_sha256=sha(out), stderr_sha256=sha(err))
        check(row['exit_code'] == 0 and not row['forced_cleanup'], 'Native command failed; retained logs identify the failure')
        return out

    def cli(binary, *arguments, cwd=output, env=None, timeout=300):
        reply = read_json(execute([binary, *arguments], cwd, env, timeout))
        check(reply.get('status') == 'ok' and 'result' in reply, 'Native CLI returned an error')
        record['commands'][-1]['result'] = reply['result'];return reply['result']

    def close_author():
        nonlocal author
        if author is not None:
            owner, author = author, None
            owner.close()

    try:
        launcher = load_launcher()
        binary = args.binary.resolve(strict=True);runtime = args.runtime.resolve(strict=True)
        artifact = args.artifact.resolve(strict=True);source = args.source.resolve(strict=True)
        check(binary.is_file() and runtime.is_dir() and artifact.is_file() and source.is_file(), 'Missing actual qualification input')
        check(source.stat().st_size == 50116 and sha(source) == launcher.SOURCE_SHA, 'Supply the pinned unchanged original GLB')
        descriptor, manifest, metadata_name, library = artifact_inputs(artifact, launcher)
        runtime_spec = read_json(runtime/'runtime.json')
        check(runtime_spec['target_os'] == 'Windows' and runtime_spec['target_arch'] == 'x86_64' and
              runtime_spec['gameplay_services_version'] == 7 and runtime_spec['gameplay_services_bytes'] >= 224 and
              runtime_spec['gameplay_call_version'] == 1 and runtime_spec['gameplay_call_bytes'] == 80 and
              REQUIRED <= set(runtime_spec['gameplay_features']), 'Actual runtime contract differs')
        check(all(runtime_spec['features'].get(feature) is True for feature in
                  ('simulation', 'renderer', 'native_gameplay', 'game_ui', 'navigation', 'asset_provenance')),
              'Actual runtime lacks Character Yard features')
        check((runtime/'share/poima/licenses/RecastNavigation/License.txt').is_file(), 'Runtime Recast license is missing')
        protected = dict(runtime=(runtime, inventory(runtime)), artifact=(artifact.parent, inventory(artifact.parent)))
        record['inputs'] = dict(binary_sha256=sha(binary), source_sha256=sha(source),
            artifact_sha256=sha(artifact), runtime_descriptor_sha256=sha(runtime/'runtime.json'),
            sources={str(path.relative_to(ROOT)): sha(path) for path in
                (ROOT/'examples/character-yard/run.py', ROOT/'examples/character-yard/CharacterYardGame.cs',
                 ROOT/'tests/character_yard_contract.py', ROOT/'tests/imported_character_contract.py',
                 ROOT/'tests/collection_gameplay_bundle.py')})
        check(cli(binary, 'version')['version'] == runtime_spec['engine_version'] == descriptor['engine_version'],
              'Exporter, runtime and requested artifact version must form one cohort')
        project = output/'Owned Character Yard source';project.mkdir();world = project/'world.json'
        owned_source = project/'Source assets';owned_source.mkdir();original_copy = owned_source/'RiggedFigure.glb'
        shutil.copy2(source, original_copy);check(sha(original_copy) == launcher.SOURCE_SHA, 'Original GLB copy changed')
        author = launcher.OwnedGame(args, world, 'authoring', record, deadline, lambda path: str(Path(path).resolve()), manifest)
        authored = author.author(original_copy);close_author();record['authored'] = authored
        local_artifact = project/'gameplay';local_artifact.mkdir();shutil.copy2(artifact, local_artifact/'native-gameplay.json')
        for row in descriptor['files']:
            destination = local_artifact/row['path'];destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(artifact.parent/row['path'], destination)
        project_file = project/'project.json'
        project_file.write_text(json.dumps(dict(format='poima.project', version=2, project_id=uuid.uuid4().hex,
            name='Character Yard', entry=dict(world='world.json', controller=launcher.uid(launcher.PLAYER),
            camera=launcher.uid(launcher.CAMERA)), audio=False,
            gameplay=dict(descriptor='gameplay/native-gameplay.json', values={})), indent=2)+'\n', encoding='utf-8')
        project_before = inventory(project);original_world = read_json(world)
        record['authoring_world_sha256'] = sha(world)
        cli(binary, 'project', 'inspect', project_file)
        exported = output/'Exported Character Yard game'
        cli(binary, 'project', 'build', project_file, '--runtime', runtime, '--output', exported)
        check(inventory(project) == project_before, 'Inspect/export changed the owned source project')
        relocation = Path(tempfile.mkdtemp(prefix='Poima Character Yard relocation ', dir=ROOT.parent))
        check(not relocation.is_relative_to(ROOT), 'Bundle relocation must be outside the checkout')
        bundle = relocation/'Relocated Character Yard game';shutil.move(str(exported), bundle)
        record['relocation_root'] = str(relocation)
        unrelated = relocation/'Unrelated working directory';unrelated.mkdir()
        game_file = bundle/'game.json';game = read_json(game_file);frozen = inventory(bundle)
        check(game['version'] == 2 and game['gameplay']['values'] == {} and
              game['entry']['controller'] == launcher.uid(launcher.PLAYER) and
              game['entry']['camera'] == launcher.uid(launcher.CAMERA), 'Exported initial values or entry differ')
        check(read_json(bundle/'content/world.json') == shipping_document(original_world),
              'Export changed world data beyond the explicit shipping history projection')
        check({key[len('runtime/'):]:value for key,value in frozen.items() if key.startswith('runtime/')} == protected['runtime'][1],
              'Exported runtime closure differs')
        check(frozen['gameplay/native-gameplay.json'] == sha(artifact) and
              all(frozen['gameplay/'+row['path']] == row['sha256'] for row in descriptor['files']), 'Exported artifact closure differs')
        required_assets = {authored['asset']+'.pmodel', authored['navigation']+'.pnav'}
        check(all(frozen['content/world.json.assets/'+name] == name.rsplit('.', 1)[0] for name in required_assets),
              'Actual model or navigation package missing/changed')
        roles = {row['path']:row['role'] for row in game['files']}
        provenance = [name for name,role in roles.items() if role == 'asset_provenance']
        check(provenance and all(name.startswith('content/world.json.assets/') and name.endswith('.pprov') for name in provenance),
              'Exported asset provenance closure is missing')
        check(roles.get('content/ASSET_CREDITS.txt') == 'asset_credits', 'Exported credits are missing')
        credits = (bundle/'content/ASSET_CREDITS.txt').read_text(encoding='utf-8')
        check('Cesium' in credits and 'CC-BY-4.0' in credits and launcher.SOURCE_URL in credits, 'Original source attribution changed')
        forbidden = {'hostfxr.dll', 'libhostfxr.so', 'coreclr.dll', 'libcoreclr.so', 'poima.gameplay.dll',
                     'poima.managedbridge.dll', 'poima.characteryardgame.dll'}
        check(not any(Path(name).name.lower() in forbidden or Path(name).suffix.lower() in ('.glb', '.gltf', '.cs', '.csproj')
                      for name in frozen) and not any(managed_pe(bundle/name) for name in frozen), 'Development IL/runtime/source leaked into bundle')
        installed = (bundle/game['engine']['executable']).resolve(strict=True)
        bundled_descriptor = (bundle/game['gameplay']['descriptor']).resolve(strict=True)
        check(installed.is_relative_to(bundle) and bundled_descriptor.is_relative_to(bundle) and sha(installed) == sha(binary),
              'Resolved bundled executable differs from qualified input')
        environment = dict(os.environ);system = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')): environment.pop(key)
        shutil.rmtree(project)
        check(not project.exists() and not exported.exists() and not original_copy.exists(), 'Owned authoring source remains')
        record['source_removed_before_player'] = True
        cli(installed, 'game', 'inspect', game_file, cwd=unrelated, env=environment)
        replay = output/'replay.json';remaining = args.ticks;segments = []
        while remaining:
            count = min(600, remaining);segments.append(dict(ticks=count));remaining -= count
        replay.write_text(json.dumps(segments)+'\n', encoding='utf-8')
        report = output/'player-report.json';save_root = output/'Player external saves';save_root.mkdir()
        arguments = ['game', 'run', game_file, '--save-root', save_root, '--replay', replay, '--gpu', args.gpu,
                     '--samples', '1', '--width', '960', '--height', '540', '--report', report]
        if args.capture: arguments.extend(['--capture', output/'player.bmp'])
        played = cli(installed, *arguments, cwd=unrelated, env=environment)
        check(played['success'] and played['runtime']['tick'] == played['play']['tick'] == args.ticks and
              played['play']['success'] and played['play']['stop_reason'] == 'replay_complete' and
              played['play']['runtime_replacements'] == 0 and played['play']['nvrhi_errors'] == 0 and played['play']['hardware'],
              'Actual source-free Vulkan player failed')
        check(played['runtime']['session_id'] == played['play']['current_session_id'] == played['play']['initial_session_id'],
              'Player unexpectedly replaced its runtime')
        module = played['gameplay']['module'];values = module['values']
        check(module['backend'] == 'native_aot' and module['type'] == launcher.TYPE and
              module['assembly_sha256'] == library['sha256'] and module['schema'] == descriptor['schema'] and
              module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
              'Source-free player loaded another compiled game')
        check(values['Ticks'] == args.ticks and values['Actor'] == launcher.uid(launcher.ACTOR) and
              values['AnimatedVisual'] == launcher.uid(launcher.RIG) and values['Phase'] == 2 and
              values['Arrivals'] == 1 and values['Plans'] == 1 and values['Dispatches'] == 1 and
              values['LastPathStatus'] == 0 and values['LastCornerCount'] >= 4 and values['AnimationChanges'] >= 2,
              'Tick-zero compiled courier failed to route, animate and arrive without patched values')
        check(read_json(report) == played and inventory(bundle) == frozen, 'Player report differs or bundle mutated')
        if args.capture:
            capture = output/'player.bmp'
            check(played['play']['capture_written'] and capture.is_file() and capture.stat().st_size > 54, 'Requested Vulkan readback is missing')
            record['capture_sha256'] = sha(capture)
        record['player'] = played
        record['checks'].append('Actual relocated source-free player loads NativeAOT at tick zero with empty initial values, routes and animates its real courier, and arrives without runtime replacement.')
        record['source_free_player_complete_before_reconstruction'] = True
        # This intentionally reconstructed world is a separate pose/save test.
        # It must never supply cooked content to the source-free player above.
        verifier = ROOT/'tests/character_yard_contract.py';headless_output = output/'Separate bundled runtime pose and saves'
        remaining = deadline-time.monotonic();check(remaining >= 30, 'Insufficient time for independent bundled-runtime contract')
        execute([sys.executable, verifier, '--binary', installed, '--descriptor', bundled_descriptor,
                 '--manifest', bundle/'gameplay'/metadata_name, '--source', source, '--output', headless_output,
                 '--timeout', str(min(600, remaining))], cwd=unrelated, env=environment, timeout=min(620, remaining))
        evidence = read_json(headless_output/'evidence.json')
        check(evidence['passed'] and evidence['backend'] == 'native_aot' and not evidence['cleanup_errors'] and
              len(evidence['owners']) >= 2 and all(row['exit_code'] == 0 for row in evidence['owners']),
              'Independent bundled-runtime pose/save contract failed')
        verifier_hashes = [value for key,value in evidence['sources'].items()
                           if Path(key).as_posix() == 'tests/character_yard_contract.py']
        check(evidence['hashes']['binary'] == sha(installed) and evidence['hashes']['descriptor'] == sha(bundled_descriptor) and
              evidence['hashes']['source'] == launcher.SOURCE_SHA and
              verifier_hashes == [sha(verifier)], 'Headless contract used different immutable inputs')
        modules = [row['result']['module'] for row in evidence['calls'] if row['method'] == 'runtime.gameplay.inspect' and
                   'result' in row and row['result'].get('module') is not None]
        check(modules and all(row['backend'] == 'native_aot' and row['assembly_sha256'] == library['sha256'] for row in modules),
              'Headless contract loaded a different game image')
        compared = [row for row in evidence['visual_observations'] if row['source_pose_compared']]
        check(compared and all(row['maximum_joint_matrix_error'] < 4e-6 and row['attachment_matrix_error'] < 2e-6 and
              row['feet_root_height_error'] < .06 and row['forward_alignment'] > .98 for row in compared),
              'Independent original-source geometry/pose/facing/feet checks are missing')
        check(evidence['continuation_exclusions'] == ['values.ObservedEpochHigh', 'values.ObservedEpochLow'],
              'Continuation silently excluded additional state')
        check(continuation(evidence['grouped_continuation']) == continuation(evidence['fresh_grouped_continuation']) ==
              continuation(evidence['fresh_single_continuation']) and
              continuation(evidence['immediate_redispatch']) == continuation(evidence['fresh_redispatch']),
              'Bundled same/fresh-owner continuation diverged')
        check(evidence['ui_save_operation']['save_serviced'] and evidence['ui_load_operation']['save_serviced'] and
              evidence['ui_load_operation']['runtime_replaced'], 'Actual compiled save/load controls were not exercised')
        record['headless'] = dict(evidence_sha256=sha(headless_output/'evidence.json'), rpc_count=evidence['rpc_count'],
            checks=evidence['checks'], owners=evidence['owners'], source_pose_observations=len(compared),
            continuation_exclusions=evidence['continuation_exclusions'])
        record['checks'].append('Separate reconstructed worlds use the exact bundled runtime/artifact for original-source joint/geometry/facing/feet oracles, compiled UI saves and exact same/fresh-owner continuation.')
        check(inventory(bundle) == frozen and sha(source) == launcher.SOURCE_SHA and
              sha(binary) == record['inputs']['binary_sha256'] and
              all(inventory(path) == initial for path,initial in protected.values()), 'Bundle or supplied inputs changed')
        check(all(sha(ROOT/path) == expected for path,expected in record['inputs']['sources'].items()), 'Verifier/sample source changed during qualification')
        record['bundle_inventory'] = frozen
        record['checks'].append('Model/navigation/provenance/credits, runtime/licenses and native artifact inventory remain exact after export, source removal, relocation and both independent executions.')
        record['passed'] = not record['cleanup_errors']
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        try: close_author()
        except BaseException: record['cleanup_errors'].append(traceback.format_exc());record['passed'] = False
        # Preserve a failed qualification and leave the owned/relocated evidence
        # in place. Never remove caller source, runtime or artifact directories.
        (output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(passed=record['passed'], evidence=str(output/'evidence.json'),
                              commands=len(record['commands']), owners=len(record['owners']))))
    return 0 if record['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
