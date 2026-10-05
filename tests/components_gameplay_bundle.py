#!/usr/bin/env python3
"""Export and relocate a component-bearing NativeAOT game; run generated C# and save native components."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
from components_gameplay_contract import Session, HEALTH, INTERACTION, uid


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {path.relative_to(root).as_posix(): sha(path) for path in sorted(root.rglob('*')) if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095: parser.error('GPU must be 0..4095.')
    def native(path):
        text = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True).strip() if args.windows_interop else text
    run = args.output.resolve()/uuid.uuid4().hex; run.mkdir(parents=True)
    record = dict(passed=False, binary_sha256=sha(args.binary), test_sha256=sha(Path(__file__)),
        calls=[], artifact_sha256=sha(args.artifact), runtime_descriptor_sha256=sha(args.runtime/'runtime.json'), gpu=args.gpu,
        commands=[], checks=[], limitations=['This Windows host has .NET installed; not a clean-machine test.',
            'Actual packaged NativeAOT Tick, storage and finite Vulkan replay; not physical input, subjective audio or power-loss qualification.'])
    def cli(executable, *arguments, success=True):
        command = [str(executable.resolve()), *arguments]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
        reply = json.loads(result.stdout)
        record['commands'].append(dict(command=command, exit_code=result.returncode, reply=reply, stderr=result.stderr))
        assert (result.returncode == 0) == success, reply
        assert reply['status'] == ('ok' if success else 'error'), reply
        return reply
    try:
        descriptor = json.loads(args.artifact.read_text(encoding='utf-8'))
        assert descriptor['type'] == 'Poima.Tests.ComponentGame' and descriptor['services_version'] == 5
        project = run/'Source project'
        cli(args.binary, 'project', 'create', native(project), '--name', 'Packaged gameplay save probe')
        manifest = project/'project.json'; spec = json.loads(manifest.read_text(encoding='utf-8'))
        artifact = project/'gameplay'; artifact.mkdir()
        # Copy precisely the verified descriptor payloads, not build intermediates.
        shutil.copyfile(args.artifact, artifact/'native-gameplay.json')
        for entry in descriptor['files']:
            relative = Path(entry['path'])
            assert not relative.is_absolute() and '..' not in relative.parts
            source = args.artifact.parent/relative
            assert source.stat().st_size == entry['size'] and sha(source) == entry['sha256']
            destination = artifact/relative; destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        metadata = json.loads((artifact/'game.poima-components.json').read_text())
        assert metadata['schemas'] == descriptor['schema']['components']
        assert [f['path'] for f in descriptor['files'] if f['role']=='metadata'] == ['game.poima-components.json']
        args.native = native
        session = Session(args, project/spec['entry']['world'], record)
        try:
            imported = session.rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=1,manifest=metadata))
            session.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=imported['revision'],ops=[
                dict(op='component.set',id=uid(1),type='game:'+HEALTH,value={uid(1):100,uid(2):100,uid(3):str(-(2**63))}),
                dict(op='component.set',id=uid(1),type='game:'+INTERACTION,value={uid(1):uid(2),uid(2):.125})]))
        finally: session.close()
        spec.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values=dict(Mode=8)))
        manifest.write_text(json.dumps(spec, indent=2)+'\n', encoding='utf-8')
        original_project = inventory(project)
        cli(args.binary, 'project', 'inspect', native(manifest))
        bundle = run/'Exported game'
        cli(args.binary, 'project', 'build', native(manifest), '--output', native(bundle), '--runtime', native(args.runtime))
        assert inventory(project) == original_project
        # Relocation and absent source ensure execution uses only bundled content.
        relocated = run/'Relocated game'; bundle.rename(relocated); bundle = relocated
        project.rename(run/'Source unavailable')
        game_manifest = bundle/'game.json'; game = json.loads(game_manifest.read_text(encoding='utf-8'))
        assert game['version'] == 2 and game['gameplay']['values'] == dict(Mode=8)
        files = inventory(bundle)
        forbidden = {'hostfxr.dll', 'libhostfxr.so', 'coreclr.dll', 'libcoreclr.so', 'poima.gameplay.dll', 'poima.managedbridge.dll', 'poima.componentgame.dll'}
        assert not any(Path(path).name.lower() in forbidden for path in files)
        runtime = bundle/game['engine']['executable']; assert runtime.is_file()
        record['packaged_binary_sha256'] = sha(runtime)
        assert sha(runtime) == sha(args.runtime/'bin'/('poima.exe' if descriptor['target_os'] == 'Windows' else 'poima'))
        storage = run/'External saves'; storage.mkdir()
        replay = run/'replay.json'; replay.write_text(json.dumps([dict(ticks=3)]), encoding='utf-8')
        report = run/'game-report.json'; capture = run/'game.bmp'
        played = cli(runtime, 'game', 'run', native(game_manifest), '--save-root', native(storage),
            '--replay', native(replay), '--gpu', str(args.gpu), '--samples', '1', '--width', '512', '--height', '288',
            '--report', native(report), '--capture', native(capture))['result']
        assert played['success'] and played['runtime']['tick'] == 3 and played['play']['tick'] == 3
        assert played['play']['success'] and played['play']['runtime_replacements'] == 0 and played['play']['nvrhi_errors'] == 0
        assert played['runtime']['session_id'] == played['play']['current_session_id'] == played['play']['initial_session_id']
        module = played['gameplay']['module']; values = module['values']
        assert module['backend'] == 'native_aot' and module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
        assert values['Ticks'] == 3 and values['Queried'] == 1 and values['ReadBeforeWrite'] == 1
        assert values['LastHealth'] == 98 and values['LastScore'] == str(-(2**63)+2) and values['LastWeight'] == .125
        assert values['SaveRequests'] == 1 and values['SaveState'] == 3
        slot = storage/'slot-components-game'; committed = json.loads((slot/'current.json').read_text(encoding='utf-8'))['payload']
        assert committed['generation'] == 1 and committed['previous'] is None
        payload = slot/committed['current']['file']
        assert sha(payload) == committed['current']['sha256'] and payload.stat().st_size == committed['current']['bytes']
        assert len(committed['receipts']) == 1
        owner_save = json.loads(payload.read_text())
        assert owner_save['format']=='poima.world-save' and owner_save['version']==1
        saved = owner_save['snapshot']
        assert saved['format']=='poima.runtime-snapshot' and saved['version'] == 2
        component_state = saved['payload']['components']
        health = next(entry for entry in component_state['types'] if entry['id']==HEALTH)
        assert health['instances'][0]['entity']==uid(1) and health['instances'][0]['values']==[99,100,str(-(2**63)+1)]
        assert component_state['revision']==1
        assert saved['payload']['gameplay']['schema']['components']==descriptor['schema']['components']
        assert capture.is_file() and played['play']['capture_written']
        assert json.loads(report.read_text(encoding='utf-8')) == played
        assert inventory(bundle) == files
        record['checks'].append('Relocated real component-bearing NativeAOT bundle queries/writes native components for three ticks; C#-initiated save contains exact tick1 custom state and schema fingerprints.')
        rejected = cli(runtime, 'game', 'run', native(game_manifest), '--save-root', native(bundle),
            '--replay', native(replay), '--gpu', str(args.gpu), success=False)
        assert any('immutable' in diagnostic['message'].lower() and 'bundle' in diagnostic['message'].lower() for diagnostic in rejected['diagnostics'])
        assert inventory(bundle) == files and not (bundle/'slot-components-game').exists()
        record['checks'].append('Save root inside the bundle is rejected before play; complete bundle inventory remains unchanged.')
        record.update(passed=True, artifact_payloads=len(descriptor['files']), bundled_files=len(files),
            bundle_inventory=files, save_inventory=inventory(storage), capture_sha256=sha(capture), result=played)
    except BaseException as error:
        record['error'] = repr(error)
        raise
    finally:
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(run/'evidence.json')


if __name__ == '__main__':
    main()
