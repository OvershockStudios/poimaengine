#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real installed CoreCLR authoring, failed compilation, reload and save continuation.

Run with native Windows Python inside a supervising Job and explicit matching
runtime, v2 authoring pack, SDK and hostfxr. The public helper closure may be
copied outside the engine checkout before its original path is made unavailable
by an external owner. This fixture never hides/restores that checkout itself.
Trusted MSBuild and gameplay execute; this is not a sandbox or clean installer.
"""
import argparse
import copy
import os
from pathlib import Path
import shutil
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
from authoring_tools_workflow import (ACTOR, COUNT, EXACT, GAME_ID, GAME_TYPE,
    INITIAL_EXACT, TYPE, Limits, environment, helper_pins as aot_helper_pins,
    load, output_size, payload, point, prepare_world, warm)
from native_gameplay_owner import NativeOwner, bound
from workflow_components_game_service import Checked, assigned_job, exact, need, pin, read, tree, write
from workflow_save_upgrade import active_authored_tree

PAYLOAD_KEYS = frozenset(('content_sha256','world_id','authored_revision','tick',
    'gameplay_revision','entities','animation','sound','gameplay','components',
    'ui','control_sequence','cameras','structure'))
FIELD_NAMES = sorted(('Ticks','Controls','ObservedCount','ObservedExact'))


def helpers():
    result = aot_helper_pins()
    path = Path(__file__).resolve()
    result[str(path)] = pin(path)
    return result


class ManagedOwner(NativeOwner):
    """Reuse complete native capture helpers, with an explicit CoreCLR loader.

    No NativeAOT descriptor, generated state, reconstructed response or modified
    saved document participates in the CoreCLR observation path.
    """
    def __init__(self, directory, limits, *, engine, world, saves, dotnet,
                 hostfxr, pack, assembly, start=False):
        self.limits, self.pack = limits, pack
        self.assembly, self.image = assembly, None
        self.hostfxr, self.dotnet = hostfxr, dotnet
        self.stable_validated = 0
        limits.check(); pack.verify()
        Checked.__init__(self, engine, directory, limits.deadline, world=world, saves=saves)
        if start:
            self.start()

    def call(self, method, params=None, error=None):
        self.limits.call()
        return NativeOwner.call(self, method, params, error)

    def start(self):
        revision = self.call('world.inspect')['revision']
        self.session, self.tick = uuid.uuid4().hex, 0
        self.call('runtime.start',dict(session_id=self.session,revision=revision))
        return self.load_game()

    def load_game(self):
        self.pack.verify(); self.limits.check()
        need(pin(self.assembly.parent/'Poima.Gameplay.dll') == pin(self.pack.resource('sdk/Poima.Gameplay.dll')),
             'Compiled game and installed bridge/SDK cohort differ')
        before = self.game()
        request = dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,expected_revision=before['revision'],
            expected_structure_revision=before['structure_revision'],hostfxr=str(self.hostfxr),
            bridge=str(self.pack.resource('bridge/Poima.ManagedBridge.dll')),
            assembly=str(self.assembly),type=GAME_TYPE)
        image = pin(self.assembly)['sha256']
        result = self.call('runtime.gameplay.load',request)
        self.image = image
        need(result['replayed'] is False, 'Fresh managed load replayed')
        retry = self.call('runtime.gameplay.load',request)
        need(exact(retry,dict(result,replayed=True)), 'Exact load retry changed its native result')
        self.module()
        return dict(request=request,result=result,retry=retry,image_sha256=image)

    def module(self):
        result = bound(self,self.game())
        module = result['module']
        need(module is not None and module['backend'] == 'coreclr'
             and Path(module['assembly']).resolve() == self.assembly
             and module['type'] == GAME_TYPE and module['assembly_sha256'] == self.image
             and module['schema']['identity'] == GAME_ID,
             'Actual CoreCLR module path/image/type/identity differs')
        return result

    def compile(self, label, project, products, report, *, success, restore):
        self.pack.verify(); self.limits.check()
        request = dict(request_id=uuid.uuid4().hex,executable=str(self.dotnet),
            project=str(project),output=str(products),configuration='Release',
            restore=restore,timeout_ms=180000)
        accepted = self.call('development.compile',request)
        row = dict(label=label,request=request,accepted=accepted,
                   source=pin(project.parent/'InstalledGame.cs'))
        report['jobs'].append(row)
        need(accepted['replayed'] is False, 'Fresh compile replayed')
        while True:
            status = self.call('development.inspect',dict(job_id=accepted['job_id']))
            if status['terminal']:
                break
            self.limits.check(); time.sleep(.1)
        row['terminal'] = status
        row['diagnostics'] = self.call('development.diagnostics',dict(job_id=accepted['job_id']))
        row['retry'] = self.call('development.compile',request)
        need(exact(row['retry'],dict(job_id=accepted['job_id'],replayed=True,receipt_scope='session')),
             'Exact completed compile retry relaunched or changed receipt')
        need(status['stdout_truncated'] is False and status['stderr_truncated'] is False,
             'Compiler diagnostics were truncated')
        if success:
            need(status['state'] == 'succeeded' and status['exit_code'] == 0 and status['error'] == '',
                 'Actual installed SDK compilation failed')
            need(self.assembly.is_file(), 'Successful compile did not produce expected game assembly')
            row['assembly'] = pin(self.assembly)
        else:
            need(status['state'] == 'failed' and type(status['exit_code']) is int and status['exit_code'] != 0
                 and status['result'] is None, 'Invalid C# did not fail without a verified artifact')
            need(any(item['severity'] == 'error' and item['code'] == 'CS1525'
                     and item['location'] and item['location']['line'] > 0
                     and item['origin'].replace('\\','/').rsplit('/',1)[-1] == 'InstalledGame.cs'
                     for item in row['diagnostics']['diagnostics']),
                 'Failed compile did not retain native structured source diagnostics')
        return row


def checked_point(owner, label, report):
    result = point(owner,label,report['checkpoints'])
    document = result['probe']['document']
    need(document['snapshot']['version'] == 7 and set(payload(result)) == PAYLOAD_KEYS,
         'Unreviewed native snapshot version or payload membership')
    return result


def changed_forecast(checkpoint, *, runtime_ticks, callback_ticks, controls,
                     observed_count, observed_exact):
    need(exact(checkpoint['gameplay']['module']['values'],dict(Ticks=callback_ticks,
        Controls=controls,ObservedCount=observed_count,ObservedExact=str(observed_exact))),
         'Changed compiled callback did not produce the complete four-global forecast')
    need(checkpoint['runtime']['tick'] == runtime_ticks, 'Changed callback advanced wrong native tick count')
    selected = [row for row in checkpoint['components'] if row['id'] == ACTOR and row['type'] == TYPE]
    need(len(selected) == 1 and exact(selected[0]['values'],
        {COUNT:2+runtime_ticks,EXACT:str(INITIAL_EXACT+runtime_ticks)})
        and exact(checkpoint['queries'],{TYPE:[ACTOR]}),
        'Changed compiled callback lost generated components or exact int64')


def run_command(argv, output, label, limits, report):
    limits.check()
    stdout, stderr = output/(label+'-stdout.txt'), output/(label+'-stderr.txt')
    with stdout.open('xb') as out, stderr.open('xb') as err:
        process = subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=out,stderr=err,shell=False)
        row = dict(argv=argv,pid=process.pid,stdout=str(stdout),stderr=str(stderr))
        report['commands'].append(row)
        process.wait(timeout=min(60,max(.001,limits.deadline-time.monotonic())))
        row['returncode'] = process.returncode
    need(process.returncode == 0 and stdout.stat().st_size+stderr.stat().st_size <= 8*1024**2,
         'Installed native/build-time command failed or exceeded retained log bound')
    limits.check()
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','pack','dotnet','hostfxr','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--source',type=Path,default=ROOT/'tests/authoring_tools_gameplay/InstalledGame.cs')
    parser.add_argument('--assert-absent',type=Path)
    parser.add_argument('--seconds',type=int,default=600)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Native Windows nonoptimized Python required')
    assigned_job()
    need(180 <= args.seconds <= 1200, 'Deadline must be 180..1200 seconds')
    runtime, source, dotnet, hostfxr = [path.resolve(strict=True)
        for path in (args.runtime,args.source,args.dotnet,args.hostfxr)]
    pack = load_pack(args.pack)
    need(read(pack.root/'authoring.json')['version'] == 2, 'CoreCLR requires the installed v2 pack')
    runtime_files, runtime_spec = tree(runtime), read(runtime/'runtime.json')
    need(len(runtime_files) == 49 and runtime_spec['engine_version'] == pack.engine_version
         and runtime_spec['target_os'] == 'Windows' and runtime_spec['target_arch'] == 'x86_64'
         and runtime_spec['executable'] == 'bin/poima.exe', 'Matching qualified Windows runtime required')
    engine = (runtime/runtime_spec['executable']).resolve(strict=True)
    need(pin(pack.resource('bridge/Poima.Gameplay.dll')) == pin(pack.resource('sdk/Poima.Gameplay.dll')),
         'Installed CoreCLR bridge/SDK copies differ')
    output = args.output.resolve()
    need(not output.exists() and not output.is_symlink(), 'Fresh workflow output required')
    for immutable in (runtime,pack.root,source.parent,dotnet.parent,hostfxr.parent,ROOT):
        need(not output.is_relative_to(immutable) and not immutable.is_relative_to(output),
             'Output overlaps immutable installed/helper inputs')
    absent = None if args.assert_absent is None else args.assert_absent.absolute()
    limits = Limits(time.monotonic()+args.seconds,absent)
    limits.check()
    frozen = dict(pack=tree(pack.root),runtime=runtime_files,source=pin(source),dotnet=pin(dotnet),
        hostfxr=pin(hostfxr),helpers=helpers(),interpreter=pin(Path(sys.executable).resolve()))
    output.mkdir(parents=True)
    env = dict(environment(output))
    # The external owner selects the installed SDK. This test supplies its own
    # profile/config caches; no package source or account configuration is added.
    (output/'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>\n',encoding='utf-8')
    project, products, saves = output/'project',output/'compiled',output/'saves'
    saves.mkdir()
    report = dict(format='poima.installed-coreclr-workflow.v1',passed=False,errors=[],
        jobs=[],owners=[],commands=[],checkpoints={},inputs=frozen,
        runtime_version=runtime_spec['engine_version'],pack_manifest_sha256=pack.manifest_sha256,
        original_checkout_absent=str(absent) if absent else None,
        scope='Installed v2 SDK/analyzer/generator/bridge, actual native compile/reload and full v7 saves on an existing configured Windows machine; no NativeAOT, clean-machine, timing, rendering or broad ABI claim',
        limits=dict(seconds=args.seconds,rpc_calls=3000,terminal_output_bytes=2*1024**3,terminal_output_files=20000))
    owner = None
    saved_environment = dict(os.environ)
    try:
        os.environ.clear();os.environ.update(env)
        run_command([str(engine),'project','create',str(project),'--name','Installed CoreCLR workflow'],
                    output,'create',limits,report)
        world = project/read(project/'project.json')['entry']['world']
        managed = project/'managed';managed.mkdir()
        working_source = managed/'InstalledGame.cs'
        shutil.copyfile(source,working_source)
        original_source = working_source.read_text(encoding='utf-8')
        token = '++state.Ticks;++ledger.Count;'
        need(original_source.count(token) == 1, 'Unreviewed fixture source; expected unique Tick mutation')
        csproj = managed/'InstalledGame.csproj'
        compiler_work = output/'compiler-work'/'obj'
        compiler_work.mkdir(parents=True)
        xml_path = lambda path: escape(str(path),{'"':'&quot;'})
        # Set intermediate/project-extension roots before SDK props are read.
        # No restore/generated source/cache file is allowed in the original
        # authored project, including during invalid/repaired compilations.
        csproj.write_text('<Project><PropertyGroup><BaseIntermediateOutputPath>'+xml_path(compiler_work)+
            '/</BaseIntermediateOutputPath><MSBuildProjectExtensionsPath>'+xml_path(compiler_work)+
            '/</MSBuildProjectExtensionsPath></PropertyGroup>'+
            '<Import Project="Sdk.props" Sdk="Microsoft.NET.Sdk" />'+
            '<Import Project="'+xml_path(pack.resource('build/Poima.Gameplay.props'))+'" />'+
            '<PropertyGroup><AssemblyName>InstalledGame</AssemblyName></PropertyGroup>'+
            '<Import Project="Sdk.targets" Sdk="Microsoft.NET.Sdk" /></Project>\n',encoding='utf-8')
        assembly = products/'managed/InstalledGame.dll'
        project_pin, nuget_pin = pin(csproj), pin(output/'NuGet.Config')
        kwargs = dict(engine=engine,world=world,saves=saves,dotnet=dotnet,
                      hostfxr=hostfxr,pack=pack,assembly=assembly)
        owner = ManagedOwner(output/'warm',limits,**kwargs)
        report['baseline_compile'] = owner.compile('baseline',csproj,products,report,success=True,restore=True)
        first_image = pin(assembly)
        component_manifest = output/'generated-components.json'
        run_command([str(dotnet),str(pack.resource('tools/native-generator/Poima.NativeGame.Generator.dll')),
            '--components',str(assembly),str(component_manifest)],output,'components',limits,report)
        report['authored'] = prepare_world(owner,component_manifest)
        # An active Windows world owns its zero-byte writer sidecar exclusively.
        # This existing helper validates that one file's exact ordinary shape
        # without opening it; every other authored file is content-hashed.
        authored = active_authored_tree(project)
        report['baseline_load'] = owner.start()
        report['baseline_flow'] = warm(owner,'coreclr-baseline',report['checkpoints'])
        before_failure = checked_point(owner,'coreclr-before-failed-build',report)
        complete_products = tree(products)
        # Invalid C# must not replace the currently executing collectible image.
        working_source.write_text(original_source.replace(token,'state.Ticks += ;++ledger.Count;'),encoding='utf-8')
        report['failed_compile'] = owner.compile('invalid-source',csproj,products,report,success=False,restore=False)
        need(pin(assembly) == first_image, 'Failed compile changed prior successful DLL')
        after_failure = checked_point(owner,'coreclr-after-failed-build',report)
        need(exact(after_failure['probe']['document'],before_failure['probe']['document']),
             'Failed SDK compilation changed the complete live document/snapshot')
        report['failed_compile']['prior_products'] = complete_products
        report['failed_compile']['retained_assembly'] = pin(assembly)
        working_source.write_text(original_source.replace(token,'state.Ticks+=2;++ledger.Count;'),encoding='utf-8')
        report['repair_compile'] = owner.compile('compatible-repair',csproj,products,report,success=True,restore=False)
        need(pin(assembly) != first_image, 'Compatible repair did not compile changed assembly bytes')
        before_reload = checked_point(owner,'coreclr-before-reload',report)
        need(exact(before_reload['probe']['document'],before_failure['probe']['document']),
             'Successful compilation changed live state before explicit reload')
        expected = copy.deepcopy(payload(before_reload))
        expected['gameplay_revision'] += 1
        expected['gameplay']['assembly_sha256'] = pin(assembly)['sha256']
        report['compatible_reload'] = owner.load_game()
        after_reload = checked_point(owner,'coreclr-after-reload',report)
        need(exact(payload(after_reload),expected),
             'Compatible reload changed complete native state beyond assembly identity/revision')
        expected_document = copy.deepcopy(before_reload['probe']['document'])
        expected_document['snapshot']['payload'] = expected
        # stored(), used by NativeOwner.probe(), verifies the original native
        # payload-token checksum before returning this document. Only that
        # derived checksum accompanies the two approved payload changes.
        expected_document['snapshot']['sha256'] = after_reload['probe']['document']['snapshot']['sha256']
        need(exact(after_reload['probe']['document'],expected_document),
             'Compatible reload changed outer save/world fields or snapshot metadata')
        need(exact(after_reload['gameplay']['module']['migration'],
                   dict(added=[],removed=[],preserved=FIELD_NAMES)), 'Compatible reload migration membership differs')
        owner.step()
        changed = checked_point(owner,'coreclr-changed-saved',report)
        changed_forecast(changed,runtime_ticks=3,callback_ticks=4,controls=1,
                         observed_count=4,observed_exact=INITIAL_EXACT+2)
        owner.step(2)
        advanced = checked_point(owner,'coreclr-changed-advanced',report)
        changed_forecast(advanced,runtime_ticks=5,callback_ticks=8,controls=1,
                         observed_count=6,observed_exact=INITIAL_EXACT+4)
        report['changed_warm_restore'] = load(owner,'installed-coreclr-changed-saved')
        restored = checked_point(owner,'coreclr-changed-restored',report)
        need(exact(restored['probe']['document'],changed['probe']['document']),
             'Same-owner restored complete document differs before callbacks')
        owner.step()
        continued = checked_point(owner,'coreclr-changed-continued',report)
        changed_forecast(continued,runtime_ticks=4,callback_ticks=6,controls=1,
                         observed_count=5,observed_exact=INITIAL_EXACT+3)
        need(len(owner.call('development.jobs')['jobs']) == 3, 'Retried builds added unexpected jobs')
        report['owners'].append(dict(role='warm-build-reload',**owner.close_recorded()));owner=None
        owner = ManagedOwner(output/'cold',limits,start=True,**kwargs)
        report['changed_cold_restore'] = load(owner,'installed-coreclr-changed-saved')
        cold_restored = checked_point(owner,'coreclr-cold-restored',report)
        need(exact(cold_restored['probe']['document'],changed['probe']['document']),
             'Fresh-process complete restore differs before callbacks')
        owner.step()
        cold_continued = checked_point(owner,'coreclr-cold-continued',report)
        changed_forecast(cold_continued,runtime_ticks=4,callback_ticks=6,controls=1,
                         observed_count=5,observed_exact=INITIAL_EXACT+3)
        need(exact(cold_continued['probe']['document'],continued['probe']['document']),
             'Fresh-process complete continuation differs from same-owner continuation')
        report['owners'].append(dict(role='cold-save-continuation',**owner.close_recorded()));owner=None
        need(pin(csproj) == project_pin and pin(output/'NuGet.Config') == nuget_pin,
             'Compile modified project/configuration inputs')
        final_project = active_authored_tree(project)
        expected_project = copy.deepcopy(authored)
        expected_project['managed/InstalledGame.cs'] = pin(working_source)
        need(exact(final_project,expected_project), 'Workflow changed authored project beyond the explicit source edit')
        report['authored_project'] = dict(before=authored,after=final_project,
            writer_sidecar_scope='world.json.lock: ordinary zero-byte single-link shape; every other file has a content identity')
        report['source_versions'] = dict(original=frozen['source'],changed=pin(working_source))
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:
                # close_recorded requires a started runtime; startup/build
                # failures still retain the raw owner and naturally close EOF.
                if owner.session is None:
                    owner.close()
                    need(owner.transport.returncode == 0, 'Pre-start owner did not close naturally')
                    report['owners'].append(dict(role='failure-before-start',calls=len(owner.calls),
                        returncode=owner.transport.returncode,transcript=str(owner.directory/'transcript.json')))
                else:
                    report['owners'].append(dict(role='failure-close',**owner.close_recorded()))
            except BaseException:
                report['errors'].append(traceback.format_exc())
        os.environ.clear();os.environ.update(saved_environment)
        try:
            pack.verify();limits.check()
            report['final_input_fences'] = dict(pack=exact(tree(pack.root),frozen['pack']),
                runtime=exact(tree(runtime),frozen['runtime']),source=pin(source) == frozen['source'],
                dotnet=pin(dotnet) == frozen['dotnet'],hostfxr=pin(hostfxr) == frozen['hostfxr'],
                helpers=exact(helpers(),frozen['helpers']),
                interpreter=pin(Path(sys.executable).resolve()) == frozen['interpreter'])
            need(all(report['final_input_fences'].values()), 'Immutable installed/tool/helper input changed')
        except BaseException:
            report['errors'].append(traceback.format_exc())
        try:
            report['terminal_output'] = output_size(output)
        except BaseException:
            report['errors'].append(traceback.format_exc())
        report['calls'] = limits.calls
        report['passed'] = report['passed'] and not report['errors']
        write(output/'report.json',report)
    print('installed CoreCLR authoring: '+('PASS' if report['passed'] else 'FAIL')+'; '+str(limits.calls)+' native RPCs')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
