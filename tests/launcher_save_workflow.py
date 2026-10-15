#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify actual generated Windows launchers with an existing GameplaySaveProbe.

No compiler or new gameplay image. The caller explicitly trusts the supplied
engine, complete installed runtime and compiled fixture. This self-supervises
all native commands/player/service descendants using the existing Windows Job.
Replay stops on runtime replacement; continuation is a separate readonly owner.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
from native_gameplay_owner import (NativeOwner, assigned_job, contained, exact,
    need, pin, read, tree, trusted_descriptor)
from workflow_components_game_service import write
from workflow_save_upgrade import stored

LONGS = {'TriggerTick','ThrowTick','ExpectedGeneration','TicketHigh','TicketLow','TicketSequence',
         'EpochHigh','EpochLow','LastRequestedTick','LastCommittedTick','LastGeneration','RestoredTick'}
INTS = {'Ticks','Mode','Requests','Rejection','QueuedState','BusyRejection','LastState','LastError',
        'ExpiredSeen','RestoreSeen','RestoreInitiated','Enabled'}
LAUNCHER = ('@echo off\r\nsetlocal DisableDelayedExpansion\r\n'
            '"%~dp0runtime\\bin\\poima.exe" game run "%~dp0game.json" --default-save-root true %*\r\n'
            'exit /b %errorlevel%\r\n').encode('utf-8')


def unchanged(admission):
    need(all(pin(Path(name)) == row for name,row in admission['files'].items()), 'Admitted source/execution input changed')
    for label in ('artifact','runtime'):
        need(exact(tree(Path(admission[label+'_root'])), admission[label+'_files']), label+' closure changed')


def dependencies():
    names = ['tests/launcher_save_workflow.py','tests/native_gameplay_owner.py','tests/switchback_ownership.py',
        'tests/workflow_components_game_service.py','tests/workflow_response_native.py','tests/workflow_save_upgrade.py',
        'tests/switchback_game_service.py','tests/switchback_oracle.py','tests/relay_yard_upgrade.py',
        'tests/relay_yard_contract.py','tests/instance_save_evolution.py','examples/relay-yard/run.py',
        'examples/locomotion-yard/run.py','tests/gameplay-save/SaveRequests.cs']
    return [ROOT/name for name in names] + [p for p in sorted((ROOT/'tools/python/poima_client').rglob('*'))
        if p.is_file() and p.suffix in ('.py','.json')]


def frozen_world(document):
    expected = copy.deepcopy(document)
    expected['receipts'] = []
    for name in ('retired_ids','retired_template_ids','retired_ui_ids','retired_component_schemas'):
        if name in expected: expected[name] = []
    return expected


def module(result, descriptor):
    metadata = read(descriptor)
    library = next(row for row in metadata['files'] if row['role'] == 'library')
    observed = result['gameplay']['module']
    need(observed['backend'] == 'native_aot' and observed['assembly_sha256'] == library['sha256']
         and observed['type'] == metadata['type'] and exact(observed['schema'], metadata['schema']),
         'Actual player module differs from admitted compiled fixture')
    return observed['values']


def signed_half(text):
    value = int(text,16)
    return str(value if value < (1<<63) else value-(1<<64))


def continued_values(saved, status):
    """Forecast all24 globals after one callback following an external cold load."""
    need(status['enabled'] is True and status['pending'] is None and status['last_restore'] is not None,
         'Continuation needs the actual completed native restore')
    restore = status['last_restore']
    need(restore['initiating_epoch'] == '0'*32 and restore['initiating_sequence'] == 0
         and restore['restored_tick'] == 1 and restore['generation'] == 1,
         'Explicit external load unexpectedly reports a compiled initiating ticket')
    expected = copy.deepcopy(saved)
    expected.update(Ticks=saved['Ticks']+1, RestoreSeen=saved['RestoreSeen']+1, RestoreInitiated=0,
        RestoredTick='1', EpochHigh=signed_half(status['epoch'][:16]), EpochLow=signed_half(status['epoch'][16:]),
        LastState=0, LastError=0, LastRequestedTick='0', LastCommittedTick='0', LastGeneration='0',
        ExpiredSeen=saved['ExpiredSeen']+1, TicketSequence='0')
    need(set(expected) == LONGS|INTS and saved['Mode'] == 0 and int(saved['TicketSequence']) > 0,
         'Unexpected fixture state for exact continuation forecast')
    return expected


def worker(args, admission):
    assigned_job(); unchanged(admission)
    out = args.output/'consumer'; out.mkdir(exist_ok=False)
    deadline = time.monotonic()+args.seconds
    record = dict(passed=False, commands=[], owners=[], checks={}, errors=[],
        limits=['Actual generated cmd, native save/load, one selected Windows GPU and test-owned profile only.',
                'Replay terminates at restored boundary; continuation uses a separate explicitly stepped readonly owner.',
                'No compilation, cross-image upgrade, physical input, clean-machine, crash/power-loss or Alpha-all-gates claim.',
                'Caller selects trusted engine/runtime/artifact; this test records their pins, not an invented producer.'])
    owner = None

    def execute(command, label, *, environment=None, shell_command=False, expected=0):
        need(time.monotonic() < deadline, 'Worker deadline reached')
        stdout, stderr = out/(label+'-stdout.txt'), out/(label+'-stderr.txt')
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', POIMA_JOB_WORKERS='2')
        if environment is not None:
            for key,value in environment.items():
                if value is None: env.pop(key,None)
                else: env[key] = value
        row = dict(command=command, expected_exit=expected, environment_overrides=environment)
        record['commands'].append(row)
        with stdout.open('wb') as output, stderr.open('wb') as error:
            process = subprocess.Popen(command, executable=admission['shell'] if shell_command else None,
                cwd=out, env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=error)
            row['pid'] = process.pid
            row['exit_code'] = process.wait(timeout=min(90,max(.1,deadline-time.monotonic())))
        need(stdout.stat().st_size+stderr.stat().st_size <= 8*1048576, 'Command log budget exceeded')
        row.update(stdout=pin(stdout), stderr=pin(stderr))
        need(row['exit_code'] == expected, 'Actual command failed: '+label)
        return stdout, stderr

    def cli(arguments, label):
        stdout, _ = execute([str(args.engine), *map(str,arguments)], label)
        reply = read(stdout); need(reply['status'] == 'ok', 'Native CLI envelope failed: '+label)
        return reply['result']

    def adopt(project, mode, label):
        spec = read(project/'project.json')
        request = out/(label+'-request.json')
        values = dict(Mode=mode,TriggerTick='0',ExpectedGeneration='0' if mode == 1 else '1')
        write(request,dict(request_id=uuid.uuid4().hex, expected_project_sha256=pin(project/'project.json')['sha256'],
            expected_artifact_sha256=pin(args.artifact)['sha256'],values=values))
        cli(['project','adopt-gameplay',project/'project.json','--artifact',args.artifact,'--request',request],label)
        result = read(project/'project.json')
        need(result['project_id'] == spec['project_id'] and exact(result['gameplay']['values'],values),
             'Adoption changed project identity or launch values')
        return result

    def export(project, destination, label):
        before = tree(project)
        cli(['project','build',project/'project.json','--runtime',args.runtime,'--output',destination],label)
        need(exact(tree(project),before), 'Export changed adopted project')
        spec = read(destination/'game.json'); original = read(project/'project.json')
        need(spec['project_id'] == original['project_id'] and exact(spec['gameplay']['values'],original['gameplay']['values']),
             'Export changed project identity or launch initialization')
        need(exact(read(destination/spec['entry']['world']),frozen_world(read(project/original['entry']['world']))),
             'Exported complete typed world differs from actual freeze policy')
        need((destination/'launch.cmd').read_bytes() == LAUNCHER, 'Generated Windows launcher bytes differ')
        runtime = {row['path'][len('runtime/'):]:dict(bytes=row['size'],sha256=row['sha256'])
                   for row in spec['files'] if row['role'] == 'runtime'}
        need(exact(runtime,admission['runtime_files']) and exact(tree(destination/'runtime'),runtime),
             'Exported runtime differs from independently admitted installed runtime')
        descriptor = contained(destination,spec['gameplay']['descriptor'])
        _, metadata, image = trusted_descriptor(descriptor)
        need(image == admission['artifact_image'] and exact(metadata,read(args.artifact)), 'Exported native image/schema changed')
        cli(['game','inspect',destination/'game.json'],label+'-inspect')
        return spec, tree(destination)

    def launch(bundle, label, profile, *, override=None):
        report, replay = out/(label+'-report.json'), out/'replay.json'
        arguments = ['--replay',str(replay),'--gpu',str(args.gpu),'--samples','1','--width','512',
                     '--height','288','--report',str(report)]
        if override is not None: arguments += ['--save-root',str(override)]
        tokens = [str(bundle/'launch.cmd'), *arguments]
        need(all('"' not in value and '\n' not in value and '\r' not in value for value in tokens),
             'Windows shell tokens must not contain quotes/newlines')
        # Supply the native cmd command line directly: subprocess's CRT argv
        # quoting is not cmd.exe's /S /C outer-quote convention.
        command = '"'+admission['shell']+'" /d /v:off /s /c "'+' '.join('"'+value+'"' for value in tokens)+'"'
        before = tree(bundle)
        stdout, _ = execute(command,label,environment={'LOCALAPPDATA':None if profile is None else str(profile)},shell_command=True)
        envelope = read(stdout); need(envelope['status'] == 'ok', 'Generated launcher returned native failure')
        played = read(report); need(exact(played,envelope['result']) and played['success'] is True,
                                   'Player report/actual launcher output differs')
        need(played['play']['success'] is True and played['play']['nvrhi_errors'] == 0
             and played['play']['frames_presented'] > 0 and exact(tree(bundle),before),
             'Actual player failed or changed immutable bundle')
        record['checks'][label] = dict(report=pin(report),result=played,bundle_files=before)
        return played

    try:
        project = out/'source-project'
        cli(['project','create',project,'--name','Launcher save workflow'],'create')
        first = adopt(project,1,'adopt-save')
        world_identity = pin(project/first['entry']['world'])
        save_bundle, load_bundle = out/'Save export',out/'Load export'
        save_spec, save_files = export(project,save_bundle,'export-save')
        second = adopt(project,2,'adopt-load')
        need(pin(project/second['entry']['world']) == world_identity, 'Launch configuration changed source world')
        load_spec, load_files = export(project,load_bundle,'export-load')
        need(save_spec['project_id'] == load_spec['project_id'] and
             exact(read(save_bundle/save_spec['entry']['world']),read(load_bundle/load_spec['entry']['world'])),
             'Save/Load export changed the stable game/world identity')
        write(out/'replay.json',[dict(ticks=3)])
        profile = out/'Owned profile & (wine) ! ^ % café'; profile.mkdir()
        default_root = profile/'Poima'/'Games'/save_spec['project_id']/'Saves'
        saved_run = launch(save_bundle,'default-save',profile)
        saved, payload_path, payload_row = stored(default_root,'quick')
        saved_values = saved['snapshot']['payload']['gameplay']['values']
        need(saved['snapshot']['payload']['tick'] == 1 and payload_row['generation'] == 1
             and saved_values['Ticks'] == saved_values['Requests'] == saved_values['QueuedState'] == 1
             and saved_values['Mode'] == 0 and saved_values['BusyRejection'] == 2
             and saved_values['Enabled'] == 1 and saved_values['TicketSequence'] == '1',
             'Actual compiled request did not save the first committed boundary')
        expected = copy.deepcopy(saved_values)
        expected.update(Ticks=3,LastState=3,LastError=0,LastRequestedTick='0',LastCommittedTick='1',
                        LastGeneration='1',TicketSequence='0')
        need(set(saved_values) == LONGS|INTS and exact(module(saved_run,args.artifact),expected)
             and saved_run['runtime']['tick'] == saved_run['play']['tick'] == 3
             and saved_run['play']['runtime_replacements'] == 0, 'Finite compiled Save replay forecast differs')
        quick_files = tree(default_root/'slot-quick')
        record['checks']['durable-save'] = dict(path=str(payload_path),identity=pin(payload_path),row=payload_row,document=saved)
        for label,bundle in [('cold-load',load_bundle),('relocated-load',out/'Relocated & (wine) ! ^ % café')]:
            if label == 'relocated-load':
                load_bundle.rename(bundle); load_bundle = bundle
            restored = launch(bundle,label,profile)
            need(restored['play']['stop_reason'] == 'runtime_replaced' and restored['play']['runtime_replacements'] == 1
                 and restored['play']['current_session_id'] != restored['play']['initial_session_id']
                 and restored['runtime']['session_id'] == restored['play']['current_session_id']
                 and restored['runtime']['tick'] == restored['play']['tick'] == 1
                 and exact(module(restored,args.artifact),saved_values)
                 and exact(tree(default_root/'slot-quick'),quick_files), 'Cold cmd load did not preserve restored boundary')
        # The actual generated cmd loads above; this independent owner captures
        # the complete durable state BEFORE callbacks and explicitly continues.
        owner = NativeOwner(out/'readonly',deadline,saves=default_root,game=load_bundle/'game.json',
                            qualified_runtime=admission['runtime_files'])
        params = dict(request_id=uuid.uuid4().hex,configuration_generation=owner.configuration,slot='quick',
            expected_generation=1,revision=owner.inspect()['current_authored_revision'],new_session_id=uuid.uuid4().hex,
            **owner.guards())
        loaded = owner.call('save.load',params)
        need(loaded['session_id'] == params['new_session_id'] and loaded['tick'] == 1, 'Explicit cold restore identity differs')
        owner.session, owner.tick = loaded['session_id'],loaded['tick']
        before_callbacks = owner.probe('launcher-restored')
        need(exact(before_callbacks['document'],saved), 'Complete native restore differs before any restored callback')
        captured = owner.capture(before_callbacks,max_entities=16,max_component_cells=64)
        record['checks']['before-callbacks'] = captured
        for label in ('cold-load','relocated-load'):
            actual = record['checks'][label]['result']
            for identity,row in actual['entities'].items():
                reference = next(item for item in captured['entities'] if item['id'] == identity)
                need(row['session_id'] == actual['runtime']['session_id'] and reference['session_id'] == owner.session
                     and row['tick'] == reference['tick'] == 1, 'Player/service restored entity boundary differs')
                need(exact({k:v for k,v in row.items() if k != 'session_id'},
                           {k:v for k,v in reference.items() if k != 'session_id'}),
                     'Actual cold player entity differs from independent native saved-boundary observation')
        status = owner.idle(); expected = continued_values(saved_values,status)
        owner.step()
        continued = owner.probe('launcher-continued')
        need(exact(owner.module()['module']['values'],expected) and owner.tick == 2
             and exact(tree(default_root/'slot-quick'),quick_files), 'Explicit restored callback/global forecast differs')
        record['checks']['continued'] = owner.capture(continued,max_entities=16,max_component_cells=64)
        record['owners'].append(owner.close_recorded()); owner = None
        # Override is supplied through the ACTUAL generated launcher; remove
        # LOCALAPPDATA entirely, rather than replacing it with an empty string.
        override = out/'Explicit override'; override.mkdir()
        explicit = launch(save_bundle,'explicit-save',None,override=override)
        override_saved, _, override_row = stored(override,'quick')
        need(override_saved['snapshot']['payload']['tick'] == 1 and override_row['generation'] == 1
             and module(explicit,args.artifact)['LastState'] == 3
             and module(explicit,args.artifact)['LastGeneration'] == '1', 'Explicit override did not save with absent environment')
        unused_profile = out/'Unused override profile'; unused_profile.mkdir()
        override2 = out/'Second override'; override2.mkdir()
        launch(save_bundle,'explicit-precedence',unused_profile,override=override2)
        stored(override2,'quick')
        namespace = unused_profile/'Poima'
        need(not namespace.exists() and not namespace.is_symlink() and not namespace.is_junction(),
             'Explicit override also created default save directories')
        record['checks']['explicit-precedence'].update(default_namespace_absent=True,
            profile_files=tree(unused_profile),
            scope='Poima save namespace only; graphics drivers may create their own caches.')
        # Junctions do not require symlink privilege: this is a real reparse
        # input made only beneath this test's owned output, with no target write.
        junction_profile = out/'Junction profile'; junction_profile.mkdir()
        junction_target = out/'Junction target'; junction_target.mkdir()
        junction = junction_profile/'Poima'
        mklink = '"'+admission['shell']+'" /d /v:off /s /c mklink /J "'+str(junction)+'" "'+str(junction_target)+'"'
        execute(mklink,'create-junction',shell_command=True)
        need(junction.is_dir() and bool(junction.lstat().st_file_attributes & 0x400), 'Owned junction was not a real reparse point')
        stdout,_ = execute([str(args.engine),'game','run',str(save_bundle/'game.json'),'--default-save-root','true',
            '--replay',str(out/'replay.json')],'reject-junction',environment={'LOCALAPPDATA':str(junction_profile)},expected=4)
        rejection = read(stdout)
        need(rejection['status'] == 'error' and any('reparse' in row['message'] or 'symbolic links' in row['message']
             for row in rejection['diagnostics']) and not list(junction_target.iterdir()), 'Default root followed an owned junction')
        record['checks']['junction-rejection'] = dict(reply=rejection,
            attributes=junction.lstat().st_file_attributes,target_files=tree(junction_target))
        junction.rmdir()  # Remove only this verified test-owned junction, never its target.
        need(not junction.exists() and not list(junction_target.iterdir()), 'Owned junction cleanup changed its target')
        need(exact(tree(save_bundle),save_files) and exact(tree(load_bundle),load_files), 'Original export bytes changed')
        unchanged(admission); record.update(passed=True,original_inputs_unchanged=True)
    except BaseException:
        record['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try: record['owners'].append(owner.close_recorded())
            except BaseException: record['errors'].append(traceback.format_exc())
        write(out/'report.json',record)
    return 0 if record['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine','runtime','artifact','output'): parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=1)
    parser.add_argument('--seconds',type=int,default=420)
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--admission-sha256',help=argparse.SUPPRESS)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Native Windows Python without optimization required')
    need(120 <= args.seconds <= 900 and 0 <= args.gpu <= 4095, 'Bounded deadline/GPU required')
    sys.dont_write_bytecode = True; os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    args.engine = args.engine.resolve(strict=True); args.runtime = args.runtime.resolve(strict=True)
    args.artifact = (args.artifact/'native-gameplay.json' if args.artifact.is_dir() else args.artifact).resolve(strict=True)
    args.output = args.output.resolve()
    if args.worker:
        admission_path = args.output/'admission.json'
        need(pin(admission_path)['sha256'] == args.admission_sha256, 'Admission identity changed')
        admission = read(admission_path)
        need(all(admission[key] == str(getattr(args,key)) for key in ('engine','runtime','artifact'))
             and admission['seconds'] == args.seconds and admission['gpu'] == args.gpu, 'Worker selection changed')
        return worker(args,admission)
    shell = (Path(os.environ['SystemRoot'])/'System32/cmd.exe').resolve(strict=True)
    _,metadata,image = trusted_descriptor(args.artifact)
    need(metadata['type'] == 'Poima.Tests.GameplaySaveProbe' and metadata['identity'] == 'poima.test.gameplay-save-requests'
         and metadata['services_version'] == 7 and metadata['minimum_services_bytes'] == 176
         and {row['name'] for row in metadata['schema']['fields']} == LONGS|INTS, 'Wrong existing compiled save fixture')
    runtime_files = tree(args.runtime)
    need(len(runtime_files) == 49, 'This bounded consumer requires the admitted49-file installed runtime cohort')
    need('bin/poima.exe' in runtime_files and 'bin/phonon.dll' in runtime_files, 'Installed Windows runtime is incomplete')
    # The explicitly selected authoring CLI can retain debug symbols. Native
    # project build enforces exact engine-version compatibility with runtime.
    need(not args.output.exists() and all(not args.output.is_relative_to(path) and not path.is_relative_to(args.output)
         for path in (args.runtime,args.artifact.parent,args.engine.parent)), 'Fresh output must not overlap immutable inputs')
    files = dependencies()+[Path(sys.executable),args.engine,args.engine.parent/'phonon.dll',shell]
    admission = dict(format='poima.launcher-save-workflow.admission.v1',engine=str(args.engine),runtime=str(args.runtime),
        artifact=str(args.artifact),artifact_root=str(args.artifact.parent),artifact_image=image,
        artifact_files=tree(args.artifact.parent),runtime_root=str(args.runtime),runtime_files=runtime_files,
        files={str(p.resolve()):pin(p) for p in files},shell=str(shell),seconds=args.seconds,gpu=args.gpu)
    args.output.mkdir(parents=True); write(args.output/'admission.json',admission)
    spec = importlib.util.spec_from_file_location('launcher_save_job',ROOT/'tests/switchback_ownership.py')
    gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
    command = [sys.executable,str(Path(__file__).resolve()),'--engine',str(args.engine),'--runtime',str(args.runtime),
        '--artifact',str(args.artifact),'--output',str(args.output),'--gpu',str(args.gpu),'--seconds',str(args.seconds),
        '--worker','--admission-sha256',pin(args.output/'admission.json')['sha256']]
    job = gate.gated(command,args.output/'job',args.seconds+45)
    result = dict(passed=False,job=job,admission=pin(args.output/'admission.json'),errors=[])
    try:
        unchanged(admission)
        report = read(args.output/'consumer/report.json'); result['report'] = pin(args.output/'consumer/report.json')
        need(job['passed'] is True and not job['forced_cleanup'] and not job['cleanup_errors'] and report['passed'] is True,
             'Native workflow or supervising Job failed')
        for key in ('job_accounting_at_child_exit','job_accounting_after_cleanup'):
            need(job[key]['active_processes'] == job[key]['terminated_processes'] == 0, 'Owned processes did not close naturally')
        result['passed'] = True
    except BaseException: result['errors'].append(traceback.format_exc())
    write(args.output/'qualification.json',result)
    print(json.dumps(dict(passed=result['passed'],seconds=job['seconds'],forced_cleanup=job['forced_cleanup'],errors=result['errors'])))
    return 0 if result['passed'] else 1

if __name__ == '__main__': raise SystemExit(main())
