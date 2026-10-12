#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify a real Windows Traversal Yard NativeAOT export and relocated player.

Source-independent playback precedes the relocated cooked-world course/save
contract. Authoring originals and failed artifacts are always retained. Requires supplied frozen artifacts; performs no builds,
downloads, pose edits or automatic mutation retries.
"""
import argparse
import importlib.util
import json
import math
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

from collection_gameplay_bundle import check, inventory, managed_pe, read_json, sha

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {'baseline_v7', 'component_collections_v1', 'animation_inertial_v1',
            'character_input_v1', 'navigation_query_v1', 'input_actions_v1',
            'character_stance_v1', 'camera_lens_v1'}
MAX_LOCAL_EVIDENCE = 128 * 1024 * 1024



MAX_EXPORT_FILE = 256 * 1024 * 1024
MAX_EXPORT_PAYLOAD = 1024 * 1024 * 1024


def shipping_pe(path):
    # This checks symbol removal only. The root-owned strip equivalence receipt
    # independently joins loadable sections/directories to qualified originals.
    path = Path(path)
    check(path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_EXPORT_FILE,
          'Shipping PE exceeds the unchanged export per-file budget')
    with path.open('rb') as stream:
        dos = stream.read(64)
        check(len(dos) == 64 and dos[:2] == b'MZ', 'Shipping runtime is not PE')
        offset = struct.unpack_from('<I', dos, 60)[0]
        check(64 <= offset <= 1024 * 1024 and offset + 24 <= path.stat().st_size,
              'Shipping PE header offset is invalid')
        stream.seek(offset);header = stream.read(24)
        check(len(header) == 24 and header[:4] == b'PE\0\0', 'Shipping PE signature differs')
        machine, count, _, symbols, symbol_count, optional_bytes, _ = struct.unpack_from('<HHIIIHH', header, 4)
        check(machine == 0x8664 and 1 <= count <= 96 and symbols == symbol_count == 0 and
              112 <= optional_bytes <= 4096, 'Shipping PE contains COFF symbols or invalid sections')
        optional = stream.read(optional_bytes)
        check(len(optional) == optional_bytes and struct.unpack_from('<H', optional)[0] == 0x20b,
              'Shipping runtime is not PE32+')
        sections = stream.read(count * 40)
        check(len(sections) == count * 40, 'Shipping PE section table is incomplete')
        names = []
        for index in range(count):
            item = sections[index * 40:(index + 1) * 40]
            name = item[:8].split(b'\0', 1)[0]
            size, raw = struct.unpack_from('<II', item, 16)
            check(name and not name.startswith((b'.debug', b'.zdebug', b'.stab', b'/')) and
                  (size == 0 or (raw >= offset + 24 + optional_bytes + count * 40 and
                                 raw + size <= path.stat().st_size)),
                  'Shipping PE retains debug sections or invalid raw section bounds')
            names.append(name.decode('ascii'))
        check(len(names) == len(set(names)), 'Duplicate shipping PE sections')
    return dict(bytes=path.stat().st_size, sha256=sha(path), coff_symbols=0,
                debug_sections_absent=True, section_count=count)


def export_bounds(directory):
    files = [p for p in Path(directory).rglob('*') if p.is_file()]
    check(1 <= len(files) <= 4096 and all(not p.is_symlink() and p.stat().st_size <= MAX_EXPORT_FILE for p in files),
          'Shipping payload exceeds the unchanged export file limits')
    total = sum(p.stat().st_size for p in files)
    check(total <= MAX_EXPORT_PAYLOAD, 'Shipping payload exceeds the unchanged export total limit')
    return dict(files=len(files), bytes=total, per_file_limit=MAX_EXPORT_FILE,
                total_limit=MAX_EXPORT_PAYLOAD, limits_unchanged=True)


def read_local_evidence(path):
    # Full per-RPC observations and exact continuation snapshots are retained
    # locally. Their bound is separate from the stricter CLI response reader.
    with Path(path).open('rb') as stream:
        data = stream.read(MAX_LOCAL_EVIDENCE + 1)
    check(len(data) <= MAX_LOCAL_EVIDENCE, 'Local contract evidence exceeds128MiB')
    result = json.loads(data.decode('utf-8'))
    check(isinstance(result,dict), 'Local contract evidence must be an object')
    return result


def load_launcher():
    path = ROOT/'examples/traversal-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_traversal_yard_bundle_launcher', path)
    check(spec is not None and spec.loader is not None, 'Traversal Yard launcher is missing')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
          descriptor['type'] == launcher.TYPE and descriptor['identity'] == 'poima.examples.traversal-yard' and
          descriptor['target_os'] == 'Windows' and descriptor['target_arch'] == 'x86_64',
          'Supply the actual Windows Traversal Yard artifact')
    check((descriptor['call_version'], descriptor['call_bytes'], descriptor['services_version'],
           descriptor['minimum_services_bytes']) == (1, 80, 7, 336) and
          set(descriptor['required_features']) == REQUIRED and
          len(descriptor['required_features']) == len(REQUIRED), 'Traversal Yard artifact requirements differ')
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
          'Traversal Yard custom component identities differ')
    route = {field['id']: field for field in schemas[launcher.COMPONENT]['fields']}
    check(route[launcher.intake.COORDINATES]['kind'] == 'array' and
          route[launcher.intake.COORDINATES]['element_kind'] == 'float32' and
          route[launcher.intake.COORDINATES]['capacity'] == 30 and route[launcher.intake.CURSOR]['kind'] == 'int32',
          'Traversal Yard typed route schema differs')
    visual = {field['id']:field for field in schemas[launcher.CONFIG]['fields']}
    check(set(visual) == {launcher.uid(i) for i in range(1,12)} and
          visual[launcher.uid(1)]['kind'] == 'entity' and
          all(visual[launcher.uid(i)]['kind'] == 'int32' for i in range(2,6)) and
          all(visual[launcher.uid(i)]['kind'] == 'float64' for i in range(6,12)),
          'Locomotion visual/speed/slowdown schema differs')
    return descriptor, manifest, metadata[0]['path'], libraries[0]


def raw_join(record):
    from traversal_yard_contract import exact
    requests, responses, used = {}, {}, set()
    for row in record['transport']:
        message = row['message']
        check(message['jsonrpc'] == '2.0' and type(message['id']) is int, 'Malformed raw native envelope')
        key = (row['owner'], message['id'])
        check(row['direction'] in ('request', 'response'), 'Unknown native transport direction')
        target = requests if row['direction'] == 'request' else responses
        check(key not in target, 'Duplicate raw native envelope');target[key] = message
    check(requests.keys() == responses.keys() and len(requests) == len(record['calls']), 'Lossless RPC population differs')
    for call in record['calls']:
        key = (call['owner'], call['rpc_id']);check(key not in used, 'Duplicate interpreted RPC');used.add(key)
        check(requests[key]['method'] == call['method'] and exact(requests[key]['params'], call['params']), 'Native request differs')
        response = responses[key]
        if 'result' in call:
            check('error' not in response and exact(response['result'], call['result']), 'Native result differs')
        else:
            failure = response['error']
            check('result' not in response and set(failure) <= {'code', 'message', 'data'} and
                  failure['code'] == call['error']['code'] and failure['message'] == call['error']['message'] and
                  exact(failure.get('data'), call['error']['data']), 'Native error differs')
    check(used == requests.keys(), 'Unconsumed raw native requests')


def durable_snapshots(directory, evidence):
    from traversal_yard_contract import exact
    for row in evidence['snapshots']:
        folder = directory/'saves'/('slot-'+row['slot'])
        current = read_json(folder/'current.json')['payload'];selected = current['current'];name = selected['file']
        check(len(name) == 38 and name.startswith('p-') and name.endswith('.bin') and
              all(c in '0123456789abcdef' for c in name[2:-4]), 'Unsafe durable selected native payload')
        path = folder/name;receipt = row['receipt']
        check(path.is_file() and not path.is_symlink() and path.stat().st_size == row['payload_bytes'] == selected['bytes'] == receipt['bytes'] and
              sha(path) == row['payload_sha256'] == selected['sha256'] == receipt['sha256'] and
              current['generation'] == selected['generation'] == receipt['generation'] == 1,
              'Selected durable native payload/receipt identity differs')
        payload = read_json(path)
        check(exact(payload['snapshot'], row['snapshot']) and payload['snapshot']['version'] == 9,
              'Recorded full native/compiled state differs from its durable bytes')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'source-directory', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--ticks', type=int, default=1400)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--timeout', type=float, default=1800)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Use native Windows Python and Windows paths')
    if sys.flags.optimize: parser.error('Assertions must remain enabled in the independent contract')
    if not 0 <= args.gpu <= 4095 or not 1200 <= args.ticks <= 1800:
        parser.error('GPU must be0..4095 and replay ticks1200..1800')
    if not math.isfinite(args.timeout) or not 180 <= args.timeout <= 3600:
        parser.error('Timeout must be finite180..3600seconds')
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel.IsProcessInJob.restype = wintypes.BOOL
    assigned = wintypes.BOOL()
    check(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(assigned)) and assigned.value,
          'Run the fixture beneath a root-owned Windows process-tree Job.')
    output = args.output.resolve()
    if output.exists(): parser.error('--output must be new; earlier evidence is preserved')
    output.mkdir(parents=True);logs = output/'logs';logs.mkdir()
    record = dict(passed=False, runner_sha256=sha(__file__), calls=[], transport=[], owners=[], commands=[], checks=[], cleanup_errors=[],
        settings=dict(gpu=args.gpu, ticks=args.ticks, capture=args.capture),
        limitations=['Windows development host with .NET installed; not clean-machine deployment.',
            'A native Windows root-owned Job is required; all observations use supplied frozen binaries and publication artifacts.',
            'Relocated replay qualifies first-person rendering, named crouch/aim input and courier progress; the separate cooked-world semantic-input contract completes the full objective.',
            'Original Idle/Run use the existing explicit reference-rotation intake. No generated art, IK, stride fitting or general animation retargeting claim.',
            'Complete exact continuation compares two cold-restored owners, not uninterrupted warm contact-cache trajectories.',
            '960x540 4xMSAA readbacks qualify the selected GPU recipe only; AA quality is not measured. No physical-input, production frame-rate, AA-quality or broad hardware claim.',
            'This authored course has no foreground/viewmodel geometry; no viewmodel pixel reference claim.',
            'Replay CLI retains its default pcf3_point shadow filter; the separate cooked-world RPC captures explicitly select pcf3_fractional. Both use4xMSAA; no cross-filter pixel equality claim.'])
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
                try:
                    command_deadline = time.monotonic()+min(timeout, remaining)
                    while process.poll() is None:
                        check(out.stat().st_size+err.stat().st_size <= 16*1024*1024, 'Native command log cap exceeded')
                        if time.monotonic() > command_deadline:
                            row['timed_out'] = True
                            raise TimeoutError('Native command deadline exceeded')
                        time.sleep(.025)
                    check(time.monotonic() <= command_deadline and out.stat().st_size+err.stat().st_size <= 16*1024*1024,
                          'Terminal command deadline/log cap exceeded')
                except BaseException: raise
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
                stdout.flush();stderr.flush()
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
            try:owner.close()
            finally:
                transport = owner.client.transport
                row = record['owners'][-1]
                row.update(process_id=transport.process_id,stderr=transport.stderr_tail,
                           stderr_truncated=transport.stderr_truncated)
            check(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'],
                  'Authoring owner did not exit cleanly')

    try:
        launcher = load_launcher()
        binary = args.binary.resolve(strict=True);runtime = args.runtime.resolve(strict=True)
        artifact = args.artifact.resolve(strict=True);source_directory = args.source_directory.resolve(strict=True)
        check(binary.is_file() and runtime.is_dir() and artifact.is_file() and source_directory.is_dir(), 'Missing actual qualification input')
        originals = {name:source_directory/name for name in launcher.SOURCE_SHA}
        check(all(path.is_file() and sha(path) == launcher.SOURCE_SHA[name] for name,path in originals.items()),
              'Supply the unchanged pinned Kenney Protagonists1.1 body/Run/Idle/CC0 license')
        descriptor, manifest, metadata_name, library = artifact_inputs(artifact, launcher)
        runtime_spec = read_json(runtime/'runtime.json')
        check(runtime_spec['target_os'] == 'Windows' and runtime_spec['target_arch'] == 'x86_64' and
              runtime_spec['gameplay_services_version'] == 7 and runtime_spec['gameplay_services_bytes'] >= 336 and
              runtime_spec['gameplay_call_version'] == 1 and runtime_spec['gameplay_call_bytes'] == 80 and
              REQUIRED <= set(runtime_spec['gameplay_features']), 'Actual runtime contract differs')
        check(all(runtime_spec['features'].get(feature) is True for feature in
                  ('simulation', 'renderer', 'native_gameplay', 'game_ui', 'navigation', 'asset_provenance')),
              'Actual runtime lacks Traversal Yard features')
        check((runtime/'share/poima/licenses/RecastNavigation/License.txt').is_file(), 'Runtime Recast license is missing')
        check(sha(binary) == sha(runtime/'bin/poima.exe'),
              'Exporter must be the exact supplied shipping runtime executable')
        record['shipping_runtime'] = dict(exporter_matches_runtime=True,
            payload=export_bounds(runtime),
            executable=shipping_pe(runtime/'bin/poima.exe'),
            desktop=shipping_pe(runtime/'bin/poima_desktop.dll'))
        protected = dict(runtime=(runtime, inventory(runtime)), artifact=(artifact.parent, inventory(artifact.parent)))
        source_paths = [Path(__file__), ROOT/'examples/traversal-yard/run.py',
            ROOT/'examples/traversal-yard/TraversalYardGame.cs',ROOT/'examples/traversal-yard/Poima.TraversalYardGame.csproj',ROOT/'examples/locomotion-yard/run.py',ROOT/'tests/locomotion_yard_contract.py',
            *[p for p in sorted((ROOT/'tools/python/poima_client').rglob('*')) if p.is_file() and p.suffix in ('.py', '.json')],
            *[ROOT/'tests'/name for name in ('traversal_yard_contract.py','collection_gameplay_bundle.py','collection_gameplay_contract.py',
                'animation_rotation_retarget.py','animation_rotation_retarget_capture.py','rotation_retarget_fixture.py',
                'frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')]]
        record['inputs'] = dict(binary_sha256=sha(binary), original_source_hashes={name:sha(path) for name,path in originals.items()},
            artifact_sha256=sha(artifact), runtime_descriptor_sha256=sha(runtime/'runtime.json'),
            sources={path.relative_to(ROOT).as_posix():sha(path) for path in source_paths})
        check(cli(binary, 'version')['version'] == runtime_spec['engine_version'] == descriptor['engine_version'] == '0.0.127',
              'Exporter, runtime and requested artifact version must form one cohort')
        project = output/'Owned Traversal Yard source';project.mkdir();world = project/'world.json'
        owned_source = project/'Source assets';owned_source.mkdir()
        for name,path in originals.items():
            copied = owned_source/name;copied.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,copied);check(sha(copied) == launcher.SOURCE_SHA[name], 'Original FBX/license copy changed')
        author = launcher.OwnedGame(args, world, 'authoring', record, deadline, lambda path: str(Path(path).resolve()), manifest)
        authored = author.author(owned_source)
        profile = project/'traversal.poima-input.json';author.input_profile(profile)
        close_author();record['authored'] = authored
        local_artifact = project/'gameplay';local_artifact.mkdir();shutil.copy2(artifact, local_artifact/'native-gameplay.json')
        for row in descriptor['files']:
            destination = local_artifact/row['path'];destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(artifact.parent/row['path'], destination)
        project_file = project/'project.json'
        project_file.write_text(json.dumps(dict(format='poima.project', version=2, project_id=uuid.uuid4().hex,
            name='Traversal Yard', entry=dict(world='world.json', controller=launcher.uid(launcher.PLAYER),
            camera=launcher.uid(launcher.CAMERA)), input_profile='traversal.poima-input.json', audio=False,
            gameplay=dict(descriptor='gameplay/native-gameplay.json', values={})), indent=2)+'\n', encoding='utf-8')
        project_before = inventory(project);original_world = read_json(world)
        record['authoring_world_sha256'] = sha(world)
        cli(binary, 'project', 'inspect', project_file)
        exported = output/'Exported Traversal Yard game'
        cli(binary, 'project', 'build', project_file, '--runtime', runtime, '--output', exported)
        check(inventory(project) == project_before, 'Inspect/export changed the owned source project')
        relocation = Path(tempfile.mkdtemp(prefix='Poima Traversal Yard relocation ', dir=ROOT.parent))
        check(not relocation.is_relative_to(ROOT), 'Bundle relocation must be outside the checkout')
        bundle = relocation/'Relocated Traversal Yard game';shutil.move(str(exported), bundle)
        record['relocation_root'] = str(relocation)
        unrelated = relocation/'Unrelated working directory';unrelated.mkdir()
        game_file = bundle/'game.json';game = read_json(game_file);frozen = inventory(bundle)
        record['shipping_bundle'] = export_bounds(bundle)
        check(shipping_pe(bundle/'runtime/bin/poima.exe') == record['shipping_runtime']['executable'] and
              shipping_pe(bundle/'runtime/bin/poima_desktop.dll') == record['shipping_runtime']['desktop'],
              'Exported shipping PE identities or symbol removal differ')
        check(game['version'] == 2 and game['input_profile'] == 'content/default.poima-input.json' and
              frozen['content/default.poima-input.json'] == project_before['traversal.poima-input.json'] and game['gameplay']['values'] == {} and
              game['entry']['controller'] == launcher.uid(launcher.PLAYER) and
              game['entry']['camera'] == launcher.uid(launcher.CAMERA), 'Exported initial values or entry differ')
        from traversal_yard_contract import exact
        check(exact(read_json(bundle/'content/world.json'), shipping_document(original_world)),
              'Export changed world data beyond the explicit shipping history projection')
        check({key[len('runtime/'):]:value for key,value in frozen.items() if key.startswith('runtime/')} == protected['runtime'][1],
              'Exported runtime closure differs')
        check(frozen['gameplay/native-gameplay.json'] == sha(artifact) and
              all(frozen['gameplay/'+row['path']] == row['sha256'] for row in descriptor['files']), 'Exported artifact closure differs')
        required_assets = {authored['asset']+'.pmodel', authored['navigation']+'.pnav'}
        check(all(frozen['content/world.json.assets/'+name] == name.rsplit('.', 1)[0] for name in required_assets),
              'Actual model or navigation package missing/changed')
        check('content/world.json.assets/'+authored['baseline']+'.pmodel' not in frozen,
              'Unreferenced baseline/source-observation package leaked into export')
        roles = {row['path']:row['role'] for row in game['files']}
        provenance = [name for name,role in roles.items() if role == 'asset_provenance']
        check(provenance and all(name.startswith('content/world.json.assets/') and name.endswith('.pprov') for name in provenance),
              'Exported asset provenance closure is missing')
        check(roles.get('content/ASSET_CREDITS.txt') == 'asset_credits', 'Exported credits are missing')
        credits = (bundle/'content/ASSET_CREDITS.txt').read_text(encoding='utf-8')
        check('Kenney' in credits and 'CC0-1.0' in credits and launcher.SOURCE_URL in credits, 'Original source attribution changed')
        forbidden = {'hostfxr.dll', 'libhostfxr.so', 'coreclr.dll', 'libcoreclr.so', 'poima.gameplay.dll',
                     'poima.managedbridge.dll', 'poima.traversalyardgame.dll', 'poima.locomotionyardgame.dll'}
        check(not any(Path(name).name.lower() in forbidden or Path(name).suffix.lower() in ('.glb', '.gltf', '.fbx', '.cs', '.csproj')
                      for name in frozen) and not any(managed_pe(bundle/name) for name in frozen), 'Development IL/runtime/source leaked into bundle')
        installed = (bundle/game['engine']['executable']).resolve(strict=True)
        bundled_descriptor = (bundle/game['gameplay']['descriptor']).resolve(strict=True)
        check(installed.is_relative_to(bundle) and bundled_descriptor.is_relative_to(bundle) and sha(installed) == sha(binary),
              'Resolved bundled executable differs from qualified input')
        environment = dict(os.environ);system = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')): environment.pop(key)
        retained_project = output/'Retained Traversal Yard authoring source'
        check(not retained_project.exists(), 'Retained source destination must be new')
        project.rename(retained_project)
        check(inventory(retained_project) == project_before and not project.exists() and not exported.exists() and not owned_source.exists(),
              'Original authoring paths remain or retained source bytes changed')
        record['retained_project'] = str(retained_project)
        record['original_authoring_paths_absent_before_player'] = True
        cli(installed, 'game', 'inspect', game_file, cwd=unrelated, env=environment)
        replay = output/'replay.json'
        # No movement, rig command, pose edit or gameplay values shortcut. The
        # named profile is exported; semantic held actions exercise compiled
        # crouch/aim while the original animated courier follows its own route.
        segments = [dict(ticks=60), dict(ticks=12, actions=[
            dict(name='aim', held=True, pressed=False, released=False),
            dict(name='crouch', held=True, pressed=False, released=False)]),
            dict(ticks=12)]
        remaining = args.ticks-84
        while remaining:
            count = min(600, remaining);segments.append(dict(ticks=count));remaining -= count
        replay.write_text(json.dumps(segments)+'\n', encoding='utf-8')
        report = output/'player-report.json';save_root = output/'Player external saves';save_root.mkdir()
        arguments = ['game', 'run', game_file, '--save-root', save_root, '--replay', replay, '--gpu', args.gpu,
                     '--samples', '4', '--width', '960', '--height', '540', '--report', report]
        if args.capture: arguments.extend(['--capture', output/'player.bmp'])
        played = cli(installed, *arguments, cwd=unrelated, env=environment,timeout=1200)
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
        check(values['Ticks'] == args.ticks and values['PlayerTicks'] == args.ticks and
              values['Actor'] == launcher.uid(launcher.ACTOR) and values['AnimatedVisual'] == launcher.uid(launcher.RIG) and
              values['Phase'] == 2 and values['Arrivals'] >= 1 and values['Plans'] >= 1 and values['Dispatches'] >= 1 and
              values['AnimationChanges'] >= 2 and values['AimTransitions'] >= 2 and values['StanceWrites'] > 0,
              'Relocated replay did not run its real courier and named crouch/aim controls')
        check(played['play']['samples'] == 4 and played['play']['render_diagnostics']['shadow_filter'] == 'pcf3_point' and played['play']['width'] == 960 and played['play']['height'] == 540 and
              played['play']['frames_presented'] >= args.ticks,
              'Actual source-free first-person recipe or presented-frame count differs')
        check(read_json(report) == played and inventory(bundle) == frozen, 'Player report differs or bundle mutated')
        if args.capture:
            capture = output/'player.bmp'
            check(played['play']['capture_written'] and capture.is_file() and capture.stat().st_size > 54, 'Requested Vulkan readback is missing')
            from scene_capture import pixels
            image = pixels(capture);check(len(image) == 540 and len(image[0]) == 960, 'First-person readback dimensions differ')
            record['capture_sha256'] = sha(capture)
        record['player'] = played
        record['checks'].append('Relocated source-independent NativeAOT replay starts with empty values, keeps one owner, presents first-person frames and drives real named crouch/aim and courier callbacks.')
        record['source_free_player_complete_before_contract'] = True
        verifier = ROOT/'tests/traversal_yard_contract.py';headless_output = output/'Bundled cooked course and saves'
        remaining = deadline-time.monotonic();check(remaining >= 120, 'Insufficient time for source-independent cooked course contract')
        command = [sys.executable, verifier, '--binary', installed, '--descriptor', bundled_descriptor,
            '--manifest', bundle/'gameplay'/metadata_name, '--prepared-world', bundle/'content/world.json',
            '--output', headless_output, '--width', '960', '--height', '540', '--timeout', str(min(1200, remaining)), '--gpu', str(args.gpu)]
        if args.capture: command.append('--capture')
        execute(command, cwd=unrelated, env=environment, timeout=min(1220, remaining))
        evidence = read_local_evidence(headless_output/'evidence.json')
        check(evidence['passed'] and evidence['backend'] == 'native_aot' and not evidence['cleanup_errors'] and
              evidence['source_unchanged'] and len(evidence['owners']) == 2 and
              all(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'] and
                  not row.get('cleanup_error') for row in evidence['owners']), 'Cooked bundled-runtime course/save contract failed')
        values = evidence['stages']['values']
        check(values['Won'] == 1 and values['Collected'] == 3 and values['BeaconMask'] == 7 and values['CrouchedBeacon'] == 1 and
              values['LiftMoves'] > 0 and evidence['stages']['ui_save']['save_serviced'] and
              evidence['stages']['ui_load']['save_serviced'] and evidence['stages']['ui_load']['runtime_replaced'],
              'Actual NativeAOT objective/lift/UI-save-load proof is incomplete')
        check(evidence['input_mode'] == 'prepared_world' and evidence['prepared_world_sha256'] == sha(bundle/'content/world.json'),
              'Source-independent contract must consume the exact relocated cooked document')
        check(all(Path(p).is_file() and sha(Path(p)) == pin for p,pin in evidence['source_pins'].items()), 'Contract source/artifact closure changed')
        raw_join(record);raw_join(evidence);durable_snapshots(headless_output, evidence)
        modules = [r['result']['module'] for r in evidence['calls'] if r['method'] == 'runtime.gameplay.inspect' and
                   'result' in r and isinstance(r['result'].get('module'), dict)]
        check(modules and all(m['backend'] == 'native_aot' and m['type'] == launcher.TYPE and
              m['assembly_sha256'] == library['sha256'] for m in modules), 'Cooked contract loaded a different NativeAOT image')
        snapshots = {r['label']:r['snapshot'] for r in evidence['snapshots']}
        from traversal_yard_contract import exact
        for left,right in (('mid-lift','after-denied-aot-replacement'), ('mid-lift','cold-reference'),
                           ('mid-lift','cold-fresh'), ('collected-reference','collected-fresh'),
                           ('ui-save-checkpoint','ui-load-restored')):
            check(exact(snapshots[left],snapshots[right]), 'Complete exact native/managed snapshot pair differs: '+left+'/'+right)
        if args.capture:
            captures = evidence['captures']
            check(len(captures) == 3 and {r['camera'] for r in captures} == {launcher.uid(101), launcher.uid(102)},
                  'Real first-person and independent overview cameras were not captured')
            from scene_capture import pixels
            for captured in captures:
                image = headless_output/(captured['label']+'.bmp')
                check(sha(image) == captured['sha256'] and image.stat().st_size == captured['bytes'] and len(pixels(image)) == 540 and len(pixels(image)[0]) == 960,
                      'Cooked-world readback identity or dimensions differ')
                result = captured['result']
                calls = [r for r in evidence['calls'] if r['method'] == 'runtime.capture' and 'result' in r and
                         Path(r['params']['path']).resolve() == image.resolve()]
                check(len(calls) == 1 and exact(calls[0]['result'], result) and calls[0]['params']['camera'] == captured['camera'],
                      'Readback does not join one actual native capture call')
                check(result['render_diagnostics']['last_draws']['camera_draws'] > 0, 'Capture drew no real authored geometry')
                check(result['hardware'] and result['capture_written'] and result['samples'] == 4 and result['render_diagnostics']['shadow_filter'] == 'pcf3_fractional' and result['nvrhi_errors'] == 0,
                      'Real hardware native capture receipt differs')
        record['headless'] = dict(evidence_sha256=sha(headless_output/'evidence.json'), rpc_count=evidence['rpc_count'],
            checks=evidence['checks'], owners=evidence['owners'], captures=evidence['captures'],
            exact_complete_snapshot_pairs=5, snapshot_receipts=len(evidence['snapshots']), completed=True)
        record['checks'].append('The exact relocated cooked world completes three beacons, crouch passage, physical lift and gate using semantic native inputs, compiled UI saves and two exact cold owners.')
        check(inventory(bundle) == frozen and all(sha(path) == launcher.SOURCE_SHA[name] for name,path in originals.items()) and
              sha(binary) == record['inputs']['binary_sha256'] and
              all(inventory(path) == initial for path,initial in protected.values()), 'Bundle or supplied inputs changed')
        check(all(sha(ROOT/path) == expected for path,expected in record['inputs']['sources'].items()), 'Verifier/sample source changed during qualification')
        check(sha(__file__) == record['runner_sha256'], 'Bundle verifier changed during qualification')
        check(inventory(retained_project) == project_before, 'Retained authoring project changed after source-independent execution')
        record['bundle_inventory'] = frozen
        record['checks'].append('Model/navigation/provenance/credits, runtime/licenses and native artifact inventory remain exact after export, source-path absence, relocation and both executions.')
        record['passed'] = not record['cleanup_errors']
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        try: close_author()
        except BaseException: record['cleanup_errors'].append(traceback.format_exc());record['passed'] = False
        # Preserve a failed qualification and leave the owned/relocated evidence
        # in place. Never remove caller source, runtime or artifact directories.
        try:
            data = (json.dumps(record, indent=2, allow_nan=False)+'\n').encode('utf-8')
            check(len(data) <= MAX_LOCAL_EVIDENCE, 'Bundle summary exceeds128MiB')
            if time.monotonic() > deadline:
                record['passed'] = False
                data = (json.dumps(record, indent=2, allow_nan=False)+'\n').encode('utf-8')
            evidence_path = output/'evidence.json'
            evidence_path.write_bytes(data)
            if time.monotonic() > deadline:
                record['passed'] = False
                late = output/'late-evidence.json'
                check(not late.exists(), 'Late evidence destination exists; original retained in place')
                evidence_path.rename(late)
                raise TimeoutError('Bundle final publication exceeded deadline; complete late artifact retained')
        except BaseException:
            (output/'failure.json').write_text(json.dumps(dict(passed=False, first_error=record.get('error'),
                serialization_error=traceback.format_exc()), indent=2)+'\n', encoding='utf-8')
            record['passed'] = False
        print(json.dumps(dict(passed=record['passed'], evidence=str(output/'evidence.json'),
                              commands=len(record['commands']), owners=len(record['owners']))))
    return 0 if record['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
