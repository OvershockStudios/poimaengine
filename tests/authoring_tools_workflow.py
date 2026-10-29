#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Installed NativeAOT authoring, adoption/export and exact save continuation.

Run on native Windows inside a supervising Job with an already installed
matching runtime, authoring pack and SDK/linker environment. Root may copy this
test's public helper closure outside the checkout and make that checkout absent.
This executes trusted MSBuild/game code; it is not a sandbox or installer.
"""
import argparse
import copy
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time
import traceback
import uuid
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'tools/python'))
sys.path.insert(0, str(ROOT/'scripts'))
from authoring_pack import load_pack
from native_gameplay_owner import NativeOwner, trusted_descriptor
from workflow_components_game_service import assigned_job, exact, need, pin, read, tree, write
from workflow_response_native import Recorded
from switchback_game_service import Budget, RetainedTransport
from poima_client import WorldClient

TYPE = 'a1620000000000000000000000000001'
COUNT, EXACT = 'a1620000000000000000000000000011', 'a1620000000000000000000000000012'
ACTOR, LABEL, INSPECT, FAIL, PANEL = [f'{n:032x}' for n in (1621, 1622, 1623, 1624, 1620)]
INITIAL_EXACT = 9007199254740993
GAME_TYPE = 'Poima.Tests.InstalledGame'
GAME_ID = 'poima.test.installed-authoring'
MAX_CALLS, OUTPUT_BYTES, OUTPUT_FILES = 3000, 2*1024**3, 20000


class Limits:
    def __init__(self, deadline, absent):
        self.deadline, self.absent, self.calls = deadline, absent, 0

    def check(self):
        need(time.monotonic() < self.deadline, 'Workflow deadline expired')
        if self.absent is not None:
            need(not self.absent.exists() and not self.absent.is_symlink(),
                 'Original checkout is available at an asserted consumption boundary')

    def call(self):
        self.check()
        need(self.calls < MAX_CALLS, 'Workflow native RPC budget exhausted')
        self.calls += 1


class ProfileOwner(Recorded):
    """The existing raw-retained direct transport with a host-selected profile."""
    def __init__(self, engine, world, profiles, directory, limits):
        self.limits = limits
        limits.check()
        directory.mkdir()
        self.directory, self.calls, self.raw = directory, [], []
        self.budget = Budget(limits.deadline, [])
        self.host, self.host_threads, self.client = None, [], None
        self.transport = RetainedTransport([str(engine), 'world', str(world),
            '--development-profiles', str(profiles)], directory, self.budget, self.raw.append)
        self.client = WorldClient(self.transport)

    def call(self, method, params=None, error=None):
        self.limits.call()
        return super().call(method, params, error)

    def completed(self, method, params):
        accepted = self.call(method, params)
        need(accepted['replayed'] is False, 'Fresh development submission replayed')
        while True:
            state = self.call('development.inspect', dict(job_id=accepted['job_id']))
            if state['terminal']:
                need(state['state'] == 'succeeded' and state['exit_code'] == 0
                     and type(state['result']) is dict and state['error'] == ''
                     and state['stdout_truncated'] is False and state['stderr_truncated'] is False,
                     'Development job did not pass its native post-exit verifier')
                replay = self.call(method, params)
                need(replay['replayed'] is True and replay['job_id'] == accepted['job_id'],
                     'Exact completed development retry relaunched')
                return dict(request=params, accepted=accepted, terminal=state, retry=replay)
            self.limits.check()
            time.sleep(.1)


class GameOwner(NativeOwner):
    def __init__(self, *args, limits, **kwargs):
        self.limits = limits
        limits.check()
        super().__init__(*args, **kwargs)

    def call(self, method, params=None, error=None):
        self.limits.call()
        return super().call(method, params, error)


def output_size(root):
    """Terminal evidence bound, not a continuous allocation/RSS guarantee."""
    count, size = 0, 0
    for path in root.rglob('*'):
        info = path.lstat()
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0)&0x400,
             'Linked workflow output')
        if stat.S_ISREG(info.st_mode):
            count += 1
            size += info.st_size
            need(count <= OUTPUT_FILES and size <= OUTPUT_BYTES, 'Workflow output exceeds terminal evidence bound')
        else:
            need(stat.S_ISDIR(info.st_mode), 'Nonregular workflow output')
    return dict(files=count, bytes=size, scope='Before final report publication; enclosing owner bounds the running process')


def helper_pins():
    names = ['tests/authoring_tools_workflow.py','tests/native_gameplay_owner.py',
        'tests/workflow_components_game_service.py','tests/workflow_response_native.py',
        'tests/workflow_save_upgrade.py','tests/switchback_game_service.py','tests/switchback_oracle.py',
        'tests/relay_yard_upgrade.py','tests/relay_yard_contract.py','tests/instance_save_evolution.py',
        'examples/relay-yard/run.py','examples/locomotion-yard/run.py','scripts/authoring_pack.py']
    paths = [ROOT/name for name in names]
    paths.extend(path for path in sorted((ROOT/'tools/python/poima_client').rglob('*'))
                 if path.is_file() and path.suffix in ('.py','.json'))
    return {str(path.resolve()):pin(path) for path in paths}


def environment(output):
    # Only explicitly named OS/SDK/linker variables are copied. No provider,
    # account, startup-hook, arbitrary-prefix or credential variables are read.
    names = {'PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','COMSPEC','OS','PROGRAMFILES',
        'PROGRAMFILES(X86)','PROGRAMW6432','PROCESSOR_ARCHITECTURE','PROGRAMDATA','ALLUSERSPROFILE',
        'INCLUDE','LIB','LIBPATH','VCTOOLSINSTALLDIR','VCINSTALLDIR','VSINSTALLDIR','WINDOWSSDKDIR',
        'WINDOWSSDKVERSION','UNIVERSALCRTSDKDIR','UCRTVERSION','WINDOWSSDKBINPATH',
        'WINDOWSSDKVERBINPATH','FRAMEWORKDIR','FRAMEWORKVERSION','FRAMEWORKVERSION64',
        'FRAMEWORKDIR64','NETFXSDKDIR','NUGET_PACKAGES','DOTNET_ROOT','DOTNET_ROOT_X64',
        'ILCUSEENVIRONMENTALTOOLS'}
    values = {key:value for key,value in os.environ.items() if key.upper() in names}
    state = output/'tool-state'
    for name in ('home','appdata','localappdata','temporary','http-cache','plugins-cache'):
        (state/name).mkdir(parents=True, exist_ok=False)
    if os.name == 'nt':
        # Native tools may initialize WinINet's compatibility cache beneath
        # USERPROFILE even with separate APPDATA/LOCALAPPDATA. Supply ordinary
        # owned directories before tool startup so Windows need not create the
        # Content.IE5 junction. Strict output/link checks remain unchanged.
        cache = state/'home/AppData/Local/Microsoft/Windows/INetCache'
        for name in ('IE', 'Content.IE5'):
            (cache/name).mkdir(parents=True, exist_ok=False)
    values.update(DOTNET_CLI_HOME=str(state/'home'),USERPROFILE=str(state/'home'),
        APPDATA=str(state/'appdata'),LOCALAPPDATA=str(state/'localappdata'),
        TEMP=str(state/'temporary'),TMP=str(state/'temporary'),
        NUGET_HTTP_CACHE_PATH=str(state/'http-cache'),NUGET_PLUGINS_CACHE_PATH=str(state/'plugins-cache'),
        DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_GENERATE_ASPNET_CERTIFICATE='false',DOTNET_NOLOGO='1',
        DOTNET_PROCESSOR_COUNT='2',MSBUILDDISABLENODEREUSE='1',UseSharedCompilation='false',
        BuildInParallel='false',IlcSingleThreaded='true',PYTHONDONTWRITEBYTECODE='1')
    return sorted(values.items())


def prepare_world(owner, component_manifest):
    document = read(component_manifest)
    schemas = document['schemas']
    need(len(schemas) == 1 and schemas[0]['id'] == TYPE, 'Generated component schema membership differs')
    fields = {field['id']:field for field in schemas[0]['fields']}
    need(set(fields) == {COUNT, EXACT}
         and fields[COUNT]['kind'] == 'int32' and fields[COUNT]['default'] == 2
         and fields[EXACT]['kind'] == 'int64' and fields[EXACT]['default'] == str(INITIAL_EXACT),
         'Actual generated defaults or exact-int64 schema differ')
    before = owner.call('world.inspect')
    imported = owner.call('component.schema.import', dict(request_id=uuid.uuid4().hex,
        base_revision=before['revision'], manifest=document))
    def element(identity, kind, text, action=None, parent=PANEL):
        return dict(op='ui.element.set', id=identity, element=dict(parent=parent,
            name='Installed '+kind, kind=kind, text=text, action=action, visible=True, enabled=True))
    ops = [dict(op='entity.create', id=ACTOR, name='Installed generated actor'),
        dict(op='component.set', id=ACTOR, type='game:'+TYPE,
             value={COUNT:2, EXACT:str(INITIAL_EXACT)}),
        element(PANEL,'panel','',parent=None), element(LABEL,'label','Initial'),
        element(INSPECT,'button','Inspect','inspect'), element(FAIL,'button','Fail','fail')]
    result = owner.call('world.transact', dict(request_id=uuid.uuid4().hex,
        base_revision=imported['revision'], ops=ops))
    return dict(generated_manifest=pin(component_manifest), schemas=schemas, imported=imported, transaction=result)


def payload(checkpoint):
    return checkpoint['probe']['document']['snapshot']['payload']


def point(owner, label, records):
    result = owner.capture(owner.probe('installed-'+label), max_entities=8, max_component_cells=16)
    records[label] = result
    return result


def values(checkpoint):
    return checkpoint['gameplay']['module']['values']


def forecast(checkpoint, *, ticks, controls, observed_count, observed_exact):
    need(exact(values(checkpoint), dict(Ticks=ticks, Controls=controls,
        ObservedCount=observed_count, ObservedExact=str(observed_exact))), 'Complete four-global forecast differs')
    need(checkpoint['runtime']['tick'] == ticks, 'Native Tick versus compiled callback count differs')
    selected = [row for row in checkpoint['components'] if row['id'] == ACTOR and row['type'] == TYPE]
    need(len(selected) == 1 and exact(selected[0]['values'],
        {COUNT:2+ticks, EXACT:str(INITIAL_EXACT+ticks)}), 'Generated component read/write or lossless int64 differs')
    need(checkpoint['queries'] == {TYPE:[ACTOR]}, 'Actual generated component query membership differs')


def load(owner, slot):
    previous = owner.session
    params = dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration,
        slot=slot, expected_generation=1, revision=owner.inspect()['current_authored_revision'],
        new_session_id=uuid.uuid4().hex, **owner.guards())
    result = owner.call('save.load', params)
    need(result['session_id'] == params['new_session_id'] and result['session_id'] != previous,
         'Guarded load did not replace the requested owner runtime')
    owner.session, owner.tick = result['session_id'],result['tick']
    return dict(params=params,result=result)


def warm(owner, prefix, records):
    initial = point(owner,prefix+'-initial',records)
    forecast(initial,ticks=0,controls=0,observed_count=0,observed_exact=0)
    owner.step()
    ticked = point(owner,prefix+'-tick',records)
    forecast(ticked,ticks=1,controls=0,observed_count=2,observed_exact=INITIAL_EXACT)
    before = payload(ticked)
    control_params, control_result = owner.control(INSPECT,'inspect')
    saved = point(owner,prefix+'-saved',records)
    forecast(saved,ticks=1,controls=1,observed_count=3,observed_exact=INITIAL_EXACT+1)
    expected = copy.deepcopy(before)
    expected['gameplay_revision'] += 1
    expected['control_sequence'] += 1
    expected['gameplay']['values'].update(Controls=1,ObservedCount=3,ObservedExact=str(INITIAL_EXACT+1))
    expected['ui']['revision'] += 1
    expected['ui']['elements'][LABEL]['text'] = 'Count 3; controls 1'
    need(exact(payload(saved),expected), 'Successful unticked Control differs from complete predicted native payload')
    save_state, save_config, files_before = owner.idle(), owner.call('save.status'), tree(owner.expected_save_root)
    state, game = owner.inspect(), owner.module()
    fail_params = dict(session_id=owner.session,request_id=uuid.uuid4().hex,
        expected_tick=owner.tick,expected_ui_revision=state['ui_revision'],
        expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],
        expected_structure_revision=state['structure_revision'],id=FAIL)
    error = owner.call('runtime.ui.activate',fail_params,error=-32040)
    need('Installed control rollback.' in error['message'], 'Did not reach armed compiled callback failure')
    files_after = tree(owner.expected_save_root)
    need(exact(owner.idle(),save_state) and exact(owner.call('save.status'),save_config)
         and exact(files_before,files_after), 'Failed Control changed save queue/configuration/files')
    rollback = point(owner,prefix+'-rollback',records)
    need(exact(rollback['probe']['document'],saved['probe']['document']),
         'Failed Control changed complete authored document/native snapshot')
    owner.step(2)
    advanced = point(owner,prefix+'-advanced',records)
    forecast(advanced,ticks=3,controls=1,observed_count=4,observed_exact=INITIAL_EXACT+2)
    restored = load(owner,'installed-'+prefix+'-saved')
    before_callbacks = point(owner,prefix+'-restored',records)
    need(exact(before_callbacks['probe']['document'],saved['probe']['document']),
         'Warm load differs from complete durable state before callbacks')
    owner.step()
    continued = point(owner,prefix+'-continued',records)
    forecast(continued,ticks=2,controls=1,observed_count=3,observed_exact=INITIAL_EXACT+1)
    return dict(saved=saved,continued=continued,control=dict(params=control_params,result=control_result),
        failed_control=dict(params=fail_params,error=error,save_status=save_state,save_configuration=save_config,
            files_before=files_before,files_after=files_after),warm_load=restored)


def cold(owner, prefix, warm_result, records):
    restored = load(owner,'installed-'+prefix+'-saved')
    before_callbacks = point(owner,prefix+'-cold-restored',records)
    need(exact(before_callbacks['probe']['document'],warm_result['saved']['probe']['document']),
         'Fresh owner lost complete saved state before restored callbacks')
    owner.step()
    continued = point(owner,prefix+'-cold-continued',records)
    forecast(continued,ticks=2,controls=1,observed_count=3,observed_exact=INITIAL_EXACT+1)
    need(exact(continued['probe']['document'],warm_result['continued']['probe']['document']),
         'Fresh owner continuation differs from complete warm continuation')
    return dict(load=restored)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','pack','dotnet','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--source',type=Path,default=ROOT/'tests/authoring_tools_gameplay/InstalledGame.cs')
    parser.add_argument('--assert-absent',type=Path)
    parser.add_argument('--seconds',type=int,default=900)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Native Windows nonoptimized Python required')
    assigned_job()
    need(120 <= args.seconds <= 1200, 'Deadline must be120..1200 seconds')
    runtime, source, dotnet = [p.resolve(strict=True) for p in (args.runtime,args.source,args.dotnet)]
    pack = load_pack(args.pack)
    runtime_files = tree(runtime)
    need(len(runtime_files) == 49, 'Explicit qualified49 runtime closure required')
    runtime_spec = read(runtime/'runtime.json')
    need(runtime_spec['engine_version'] == pack.engine_version and runtime_spec['target_os'] == 'Windows'
         and runtime_spec['target_arch'] == 'x86_64' and runtime_spec['executable'] == 'bin/poima.exe',
         'Installed runtime and pack versions/target must exactly match')
    engine = (runtime/runtime_spec['executable']).resolve(strict=True)
    execution = {str(p):pin(p) for p in (engine,engine.parent/'phonon.dll')}
    output = args.output.resolve()
    absent = None if args.assert_absent is None else args.assert_absent.absolute()
    limits = Limits(time.monotonic()+args.seconds,absent)
    limits.check()
    need(not output.exists() and not output.is_symlink(), 'Fresh workflow output required')
    for immutable in (runtime,pack.root,source.parent,dotnet.parent):
        need(not output.is_relative_to(immutable) and not immutable.is_relative_to(output), 'Output overlaps immutable input')
    frozen = dict(pack=tree(pack.root),runtime=runtime_files,source=pin(source),dotnet=pin(dotnet),
        helpers=helper_pins(),interpreter=pin(Path(sys.executable).resolve()))
    output.mkdir(parents=True)
    # Both the external project and publisher-owned temporary host project are
    # descendants: use only the caller's already populated offline package cache.
    (output/'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>\n',encoding='utf-8')
    projects, products = output/'projects', output/'products'
    projects.mkdir();products.mkdir()
    report = dict(passed=False,errors=[],owners=[],commands=[],checkpoints={},inputs=frozen,
        runtime_version=runtime_spec['engine_version'],pack_manifest_sha256=pack.manifest_sha256,
        original_checkout_absent=str(absent) if absent else None,
        limits=dict(seconds=args.seconds,rpc_calls=MAX_CALLS,terminal_output_bytes=OUTPUT_BYTES,
            terminal_output_files=OUTPUT_FILES),
        scope='Installed NativeAOT pack, generated component and logical Control/native save workflows; no renderer, CoreCLR pack, clean-machine or ABI-stability claim')
    author = owner = None
    project = projects/'Installed'
    held = projects/'Installed-held'
    hidden = False
    try:
        limits.check();pack.verify()
        argv = [str(engine),'project','create',str(project),'--name','Installed authoring workflow']
        stdout,stderr = output/'create-stdout.txt',output/'create-stderr.txt'
        with stdout.open('xb') as out,stderr.open('xb') as err:
            process = subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=out,stderr=err,shell=False)
            row = dict(argv=argv,pid=process.pid);report['commands'].append(row)
            process.wait(timeout=min(60,max(.001,limits.deadline-time.monotonic())))
            row['returncode'] = process.returncode
        need(process.returncode == 0 and stdout.stat().st_size+stderr.stat().st_size <= 8*1024**2,
             'Native project create failed or exceeded logs')
        native_project = project/'project.json'
        world = project/read(native_project)['entry']['world']
        managed = project/'managed';managed.mkdir()
        shutil.copyfile(source,managed/'InstalledGame.cs')
        csproj = managed/'InstalledGame.csproj'
        csproj.write_text('<Project Sdk="Microsoft.NET.Sdk"><Import Project="'+
            escape(str(pack.resource('build/Poima.Gameplay.props')), {'"':'&quot;'})+'" /></Project>\n',encoding='utf-8')
        # Explicit installed policy, not dynamically discovered tool authority.
        profile = dict(name='installed',project_root=str(projects),output_root=str(products),
            python=str(Path(sys.executable).resolve()),publisher=str(pack.resource('tools/publish_native_gameplay.py')),
            dotnet=str(dotnet),exporter=str(engine),runtime_root=str(runtime),authoring_pack=str(pack.root),
            target_rid='win-x64',environment=environment(output))
        config = output/'profiles.json'
        write(config,dict(format='poima.development-profiles',version=1,profiles=[profile]))
        author = ProfileOwner(engine,world,config,output/'authoring',limits)
        report['profile_discovery'] = author.call('development.profiles')
        report['publish'] = author.completed('development.publish',dict(request_id=uuid.uuid4().hex,
            profile='installed',project='Installed/managed/InstalledGame.csproj',output='native',
            type=GAME_TYPE,timeout_ms=min(900000,args.seconds*1000)))
        descriptor,metadata,image = trusted_descriptor(products/'native/native-gameplay.json')
        need(metadata['identity'] == GAME_ID and metadata['type'] == GAME_TYPE
             and metadata['engine_version'] == pack.engine_version, 'Published fixture identity differs')
        artifact_before = tree(descriptor.parent)
        generated = [row for row in metadata['files'] if row['role'] == 'metadata'
                     and row['path'] == 'game.poima-components.json']
        need(len(generated) == 1, 'Actual generated component manifest missing/ambiguous')
        report['authored'] = prepare_world(author,descriptor.parent/generated[0]['path'])
        report['adoption'] = author.call('project.gameplay.adopt',dict(profile='installed',
            project='Installed/project.json',artifact='native/native-gameplay.json',request_id=uuid.uuid4().hex,
            expected_project_sha256=pin(native_project)['sha256'],expected_artifact_sha256=pin(descriptor)['sha256'],values={}))
        report['export'] = author.completed('development.export',dict(request_id=uuid.uuid4().hex,
            profile='installed',project='Installed/project.json',output='game',timeout_ms=120000))
        author.close();need(author.transport.returncode == 0, 'Profile owner failed natural EOF closure')
        report['owners'].append(dict(role='publish-adopt-export',calls=len(author.calls),returncode=author.transport.returncode,
            transcript=str(author.directory/'transcript.json')));author=None
        project_files = tree(project)
        bound_project = read(native_project)
        adopted = project/bound_project['gameplay']['descriptor']
        need(trusted_descriptor(adopted)[2] == image, 'Adoption selected a different native image')
        report['published_artifact'] = dict(descriptor=pin(descriptor),files=artifact_before,image_sha256=image,
            schema=metadata['schema'])
        source_saves = output/'source-saves';source_saves.mkdir()
        source_kwargs = dict(saves=source_saves,binary=engine,world=world,descriptor=adopted,
            initial_values=bound_project['gameplay']['values'],admitted_execution_pins=execution,limits=limits)
        owner = GameOwner(output/'source-warm',limits.deadline,**source_kwargs)
        source_warm = warm(owner,'source',report['checkpoints'])
        report['source_flow'] = {key:value for key,value in source_warm.items() if key not in ('saved','continued')}
        report['owners'].append(dict(role='source-warm',**owner.close_recorded()));owner=None
        owner = GameOwner(output/'source-cold',limits.deadline,**source_kwargs)
        report['source_cold'] = cold(owner,'source',source_warm,report['checkpoints'])
        report['owners'].append(dict(role='source-cold',**owner.close_recorded()));owner=None
        need(tree(project) == project_files, 'Runtime/source owner changed authored project or managed inputs')
        exported = products/'game'
        original_bundle = tree(exported)
        bundle_spec = read(exported/'game.json')
        expected_world = read(world)
        expected_world['receipts'] = []
        for key in ('retired_ids','retired_template_ids','retired_ui_ids','retired_component_schemas'):
            if key in expected_world:
                expected_world[key] = []
        need(exact(read(exported/bundle_spec['entry']['world']),expected_world),
             'Complete typed export differs beyond explicit authoring receipt/retirement sanitation')
        need(bundle_spec['source']['world_sha256'] == pin(world)['sha256'], 'Export source-world metadata identity differs')
        report['whole_typed_export'] = dict(source_world=pin(world),
            bundled_world=pin(exported/bundle_spec['entry']['world']),
            exact_except='Only receipts and present retirement journals are cleared by the native exporter')
        relocated = output/'Relocated installed game'
        shutil.copytree(exported,relocated)
        need(exact(tree(relocated),original_bundle), 'Bundle relocation changed bytes/membership')
        # Only this test-created source tree is moved. The outer owner alone
        # hides/restores the engine checkout; these are separate boundaries.
        project.rename(held);hidden=True
        need(not project.exists(), 'Original project path still available')
        bundle_saves = output/'bundle-saves';bundle_saves.mkdir()
        bundle_kwargs = dict(saves=bundle_saves,game=relocated/'game.json',qualified_runtime=runtime_files,limits=limits)
        owner = GameOwner(output/'bundle-warm',limits.deadline,**bundle_kwargs)
        bundle_warm = warm(owner,'bundle',report['checkpoints'])
        report['bundle_flow'] = {key:value for key,value in bundle_warm.items() if key not in ('saved','continued')}
        report['owners'].append(dict(role='bundle-warm',**owner.close_recorded()));owner=None
        owner = GameOwner(output/'bundle-cold',limits.deadline,**bundle_kwargs)
        report['bundle_cold'] = cold(owner,'bundle',bundle_warm,report['checkpoints'])
        report['owners'].append(dict(role='bundle-cold',**owner.close_recorded()));owner=None
        need(not project.exists() and exact(tree(held),project_files) and exact(tree(relocated),original_bundle)
             and exact(tree(exported),original_bundle), 'Bundle workflow mutated inputs or recovered original project path')
        report['relocation'] = dict(original_bundle=original_bundle,relocated_bundle=tree(relocated),
            original_project_path_absent=True,scope='Full exact comparisons within each lineage; source/bundle content hashes not normalized or equated')
        need(tree(descriptor.parent) == artifact_before, 'Published artifact changed')
        pack.verify();limits.check()
        need(exact(tree(pack.root),frozen['pack']) and exact(tree(runtime),frozen['runtime'])
             and pin(source) == frozen['source'] and pin(dotnet) == frozen['dotnet']
             and helper_pins() == frozen['helpers']
             and pin(Path(sys.executable).resolve()) == frozen['interpreter'], 'Installed/source input identity changed')
        report.update(passed=True,immutable_inputs_unchanged=True)
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:report['owners'].append(dict(role='failure-close',**owner.close_recorded()))
            except BaseException:report['errors'].append(traceback.format_exc())
        if author is not None:
            try:
                author.close();report['owners'].append(dict(role='failure-author-close',calls=len(author.calls),
                    returncode=author.transport.returncode))
            except BaseException:report['errors'].append(traceback.format_exc())
        if hidden:
            try:
                need(not project.exists(), 'Cannot restore source over another path')
                held.rename(project)
                report['project_restored_exact'] = exact(tree(project),project_files)
                need(report['project_restored_exact'], 'Test-created source project restoration drifted')
            except BaseException:report['errors'].append(traceback.format_exc())
        try:
            pack.verify()
            report['final_input_fences'] = dict(pack=exact(tree(pack.root),frozen['pack']),
                runtime=exact(tree(runtime),frozen['runtime']),source=pin(source) == frozen['source'],
                dotnet=pin(dotnet) == frozen['dotnet'],helpers=helper_pins() == frozen['helpers'],
                interpreter=pin(Path(sys.executable).resolve()) == frozen['interpreter'])
            need(all(report['final_input_fences'].values()), 'Final immutable input fence failed')
        except BaseException:report['errors'].append(traceback.format_exc())
        try:report['terminal_output'] = output_size(output)
        except BaseException:report['errors'].append(traceback.format_exc())
        report['calls'] = limits.calls
        report['passed'] = report['passed'] and not report['errors']
        write(output/'report.json',report)
    print('authoring-tools workflow: '+('PASS' if report['passed'] else 'FAIL')+'; '+str(limits.calls)+' native RPCs')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
