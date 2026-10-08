#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Export, relocate and play the actual Windows Native AOT navigation fixture.

Original compiled NPC/route/static topology are retained. A separate spectator
is the player entry. Headless save/replan qualification is a separate world,
not in-player save UX. Requires prebuilt real inputs; performs no compilation.
"""
import argparse
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
from navigation_runtime_contract import TYPE, COMPONENT, fixture, uid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient


def spectator():
    return [
        dict(op='entity.create',id=uid(900),name='Separate spectator',parent=None),
        dict(op='component.set',id=uid(900),type='Transform',value=dict(position=[-7,0,7],rotation=[0,0,0,1],scale=[1,1,1])),
        dict(op='component.set',id=uid(900),type='CharacterController',value=dict(radius=.3,height=1.8,speed=3,jump_speed=5,camera=uid(901))),
        dict(op='entity.create',id=uid(901),name='Spectator camera',parent=uid(900)),
        dict(op='component.set',id=uid(901),type='Transform',value=dict(position=[0,1.6,0],rotation=[0,0,0,1],scale=[1,1,1])),
        dict(op='component.set',id=uid(901),type='Camera',value=dict(vertical_fov=70,near=.1,far=200))]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','runtime','artifact','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=1)
    parser.add_argument('--ticks',type=int,default=1400)
    parser.add_argument('--timeout',type=float,default=600)
    parser.add_argument('--capture',action='store_true',help='Also retain a genuine Vulkan BMP readback.')
    parser.add_argument('--skip-headless',action='store_true',help='Omit the distinct bundled-runtime save/replan world; record this narrower scope.')
    args=parser.parse_args()
    if os.name!='nt':parser.error('Use native Windows Python and Windows paths')
    if sys.flags.optimize:parser.error('Assertions must be enabled for the reused independent headless verifier')
    if not 0<=args.gpu<=4095 or not 1<=args.ticks<=1400:parser.error('GPU0..4095; replay ticks1..1400')
    if not math.isfinite(args.timeout) or not 60<=args.timeout<=900:parser.error('Timeout must be finite60..900seconds')
    binary=args.binary.resolve(strict=True);distribution=args.runtime.resolve(strict=True);artifact=args.artifact.resolve(strict=True)
    check(binary.is_file() and distribution.is_dir() and artifact.is_file(),'Missing native input')
    descriptor=read_json(artifact);runtime=read_json(distribution/'runtime.json')
    check(descriptor['format']=='poima.native-gameplay' and descriptor['version']==2 and descriptor['type']==TYPE and descriptor['target_os']=='Windows' and descriptor['target_arch']=='x86_64','Expected actual Windows NavigationGame artifact')
    check(descriptor['call_version']==1 and descriptor['call_bytes']==80 and descriptor['services_version']==7 and descriptor['minimum_services_bytes']==224,'Actual navigation artifact ABI differs')
    check(set(descriptor['required_features'])=={'baseline_v7','component_collections_v1','character_input_v1','navigation_query_v1'},'Actual original navigation fixture requirements differ')
    metadata=[row for row in descriptor['files'] if row['role']=='metadata']
    check(len(metadata)==1,'Actual artifact must inventory its generated component manifest')
    manifest_path=artifact.parent/metadata[0]['path'];manifest=read_json(manifest_path)
    check(manifest['schemas']==descriptor['schema']['components'] and len(manifest['schemas'])==1 and manifest['schemas'][0]['id']==COMPONENT,'Published component schema differs')
    check(runtime['target_os']=='Windows' and runtime['target_arch']=='x86_64' and runtime['gameplay_services_version']==7 and runtime['gameplay_call_version']==1 and runtime['gameplay_call_bytes']==80 and runtime['gameplay_services_bytes']>=224 and 'navigation_query_v1' in runtime['gameplay_features'],'Runtime navigation contract differs')
    check(all(runtime['features'].get(key) is True for key in ('simulation','renderer','native_gameplay','game_ui','navigation')),'Actual runtime lacks fixture/player features')
    for row in descriptor['files']:
        relative=Path(row['path']);check(not relative.is_absolute() and '..' not in relative.parts,'Artifact path escapes root')
        path=(artifact.parent/relative).resolve(strict=True)
        check(path.is_relative_to(artifact.parent) and path.is_file() and path.stat().st_size==row['size'] and sha(path)==row['sha256'],'Actual artifact inventory mismatch')
    check((distribution/'share/poima/licenses/RecastNavigation/License.txt').is_file(),'Actual runtime Recast license missing')
    runtime_before=inventory(distribution);artifact_before=inventory(artifact.parent)
    output=args.output.resolve();check(not output.exists(),'Output must be new');output.mkdir(parents=True);logs=output/'logs';logs.mkdir()
    record=dict(passed=False,runner_sha256=sha(__file__),binary_sha256=sha(binary),artifact_sha256=sha(artifact),runtime_descriptor_sha256=sha(distribution/'runtime.json'),
        sources={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'tests/managed_navigation_gameplay/NavigationGame.cs',ROOT/'tests/navigation_runtime_contract.py',ROOT/'tests/collection_gameplay_bundle.py')},
        settings=dict(gpu=args.gpu,ticks=args.ticks,capture=args.capture,headless=not args.skip_headless),checks=[],commands=[],calls=[],owners=[],cleanup_errors=[],
        limitations=['Development Windows host with .NET installed; not clean-machine deployment.',
            'Scripted Vulkan replay, no physical input, GUI/editor, frame-time, crowds or dynamic-obstacle qualification.',
            'Player reports spectator/camera entities and compiled arrival state; exact NPC pose/save checks require the separate fresh headless world (omitted with --skip-headless).',
            'No in-player save/replan UX or general raw malformed ABI qualification.',
            'Actual game loads at tick0 in the player; this is distinct from headless settling before load.'])
    deadline=time.monotonic()+args.timeout;author=None

    def execute(command,cwd=output,env=None,timeout=220):
        left=deadline-time.monotonic();check(left>0,'Overall deadline exceeded')
        index=len(record['commands']);stdout=logs/f'command-{index}.stdout';stderr=logs/f'command-{index}.stderr'
        row=dict(command=list(map(str,command)),cwd=str(cwd),exit_code=None,timed_out=False,forced_cleanup=False,stdout=str(stdout),stderr=str(stderr));record['commands'].append(row)
        process=None
        with stdout.open('wb') as out,stderr.open('wb') as err:
            try:
                process=subprocess.Popen(row['command'],cwd=cwd,env=env,stdin=subprocess.DEVNULL,stdout=out,stderr=err);row['pid']=process.pid
                try:process.wait(timeout=min(timeout,left))
                except subprocess.TimeoutExpired:row['timed_out']=True;raise
            finally:
                if process is not None:
                    if process.poll() is None:
                        row['forced_cleanup']=True
                        try:
                            killer=Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'
                            cleanup=subprocess.run([str(killer),'/PID',str(process.pid),'/T','/F'],capture_output=True,timeout=10)
                            row['tree_cleanup_exit_code']=cleanup.returncode
                            if cleanup.returncode!=0:record['cleanup_errors'].append('Process-tree cleanup failed: '+str(cleanup.returncode))
                        except BaseException:record['cleanup_errors'].append(traceback.format_exc())
                        try:
                            if process.poll() is None:process.kill()
                            process.wait(timeout=10)
                        except BaseException:record['cleanup_errors'].append(traceback.format_exc())
                    row['exit_code']=process.poll()
        check(row['exit_code']==0 and not row['forced_cleanup'],'Command failed or forced cleanup; see logs')
        return stdout

    def cli(executable,*arguments,cwd=output,env=None,timeout=220):
        reply=read_json(execute([executable,*arguments],cwd,env,timeout))
        check(reply.get('status')=='ok' and 'result' in reply,'Native CLI returned failure')
        record['commands'][-1]['result']=reply['result'];return reply['result']

    def rpc(method,params):
        left=deadline-time.monotonic();check(left>0,'Authoring deadline exceeded')
        row=dict(method=method,params=params);record['calls'].append(row)
        row['result']=author.call(method,params,timeout=min(30,left));return row['result']

    def close_author():
        nonlocal author
        if author is None:return
        owner=author;author=None;owner.close();code=owner.transport.returncode
        record['owners'].append(dict(label='authoring',exit_code=code));check(code==0,'Authoring owner did not exit cleanly')

    try:
        check(cli(binary,'version')['version']==runtime['engine_version'],'Exporter and actual runtime engine versions differ')
        project=output/'Owned navigation source';project.mkdir();world=project/'world.json'
        author=WorldClient.open(binary,world)
        rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=0,manifest=manifest))
        rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=fixture()))
        baked=rpc('world.navigation.bake',dict(revision=2,profile=dict(radius=.3,height=1.8,climb=0)))
        asset=baked['asset'];rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=2,ops=[dict(op='navigation.set',asset=asset)]))
        original=read_json(world);checked=rpc('world.navigation.inspect',dict(revision=3,asset=asset))
        package=Path(str(world)+'.assets')/(asset+'.pnav');package_hash=sha(package)
        rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=3,ops=spectator()))
        final=read_json(world);after=rpc('world.navigation.inspect',dict(revision=4,asset=asset))
        check(checked['source_fingerprint']==after['source_fingerprint']==baked['source_fingerprint'] and package_hash==sha(package)==asset,'Spectator changed original static navigation')
        check(final['navigation']==original['navigation'] and all(final['entities'][key]==value for key,value in original['entities'].items()),'Original fixture/NPC/static geometry changed')
        check(set(final['entities'])-set(original['entities'])=={uid(900),uid(901)},'Unexpected source entity changes')
        close_author();record['navigation']=dict(asset=asset,source_fingerprint=after['source_fingerprint'],bytes=package.stat().st_size)
        owned_artifact=project/'gameplay';owned_artifact.mkdir();shutil.copyfile(artifact,owned_artifact/'native-gameplay.json')
        for row in descriptor['files']:
            path=owned_artifact/row['path'];path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(artifact.parent/row['path'],path)
        project_file=project/'project.json';project_file.write_text(json.dumps(dict(format='poima.project',version=2,project_id=uuid.uuid4().hex,name='Navigation shipping acceptance',
            entry=dict(world='world.json',controller=uid(900),camera=uid(901)),audio=False,gameplay=dict(descriptor='gameplay/native-gameplay.json',values={})),indent=2)+'\n',encoding='utf-8')
        source_before=inventory(project);cli(binary,'project','inspect',project_file)
        exported=output/'Exported navigation game';cli(binary,'project','build',project_file,'--runtime',distribution,'--output',exported)
        check(inventory(project)==source_before and inventory(distribution)==runtime_before and inventory(artifact.parent)==artifact_before,'Export changed source or inputs')
        relocation=Path(tempfile.mkdtemp(prefix='Poima navigation relocation ',dir=ROOT.parent));check(not relocation.is_relative_to(ROOT),'Relocation must be outside checkout')
        bundle=relocation/'Relocated navigation game';shutil.move(str(exported),bundle);shutil.rmtree(project)
        check(not project.exists() and not exported.exists(),'Owned source/export remains');record['relocation_root']=str(relocation)
        unrelated=relocation/'Unrelated working directory';unrelated.mkdir();game_file=bundle/'game.json';game=read_json(game_file);frozen=inventory(bundle)
        check(game['version']==2 and game['gameplay']['values']=={} and game['entry']['controller']==uid(900) and game['entry']['camera']==uid(901),'Bundled entry/initial values differ')
        check({key[len('runtime/'):]:value for key,value in frozen.items() if key.startswith('runtime/')}==runtime_before,'Bundled runtime differs')
        check(frozen['gameplay/native-gameplay.json']==sha(artifact) and all(frozen['gameplay/'+row['path']]==row['sha256'] for row in descriptor['files']),'Bundled actual artifact differs')
        check(frozen['content/world.json.assets/'+asset+'.pnav']==asset and read_json(bundle/'content/world.json')['navigation']==dict(asset=asset),'Bound navigation closure differs')
        forbidden={'hostfxr.dll','libhostfxr.so','coreclr.dll','libcoreclr.so','poima.gameplay.dll','poima.managedbridge.dll','poima.navigationgameplay.dll'}
        check(not any(Path(name).name.lower() in forbidden for name in frozen) and not any(managed_pe(bundle/name) for name in frozen),'Managed development payload shipped')
        installed=(bundle/game['engine']['executable']).resolve(strict=True)
        bundled_descriptor=(bundle/game['gameplay']['descriptor']).resolve(strict=True)
        check(installed.is_relative_to(bundle) and bundled_descriptor.is_relative_to(bundle),'Relocated executable/gameplay path escapes bundle')
        check(sha(bundled_descriptor)==sha(artifact),'Resolved bundled descriptor differs from supplied artifact')
        record['packaged_binary_sha256']=sha(installed)
        check(record['packaged_binary_sha256']==sha(binary),'Bundled executable differs from supplied qualified host')
        environment=dict(os.environ);system=Path(environment['SYSTEMROOT']);environment['PATH']=str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_','COREHOST_','POIMA_')):environment.pop(key)
        cli(installed,'game','inspect',game_file,cwd=unrelated,env=environment)
        replay=output/'replay.json';remaining=args.ticks;segments=[]
        while remaining:count=min(600,remaining);segments.append(dict(ticks=count));remaining-=count
        replay.write_text(json.dumps(segments)+'\n',encoding='utf-8');report=output/'player-report.json';storage=output/'Player external saves';storage.mkdir()
        arguments=['game','run',game_file,'--save-root',storage,'--replay',replay,'--gpu',args.gpu,'--samples','1','--width','512','--height','288','--report',report]
        if args.capture:arguments.extend(['--capture',output/'player.bmp'])
        played=cli(installed,*arguments,cwd=unrelated,env=environment)
        check(played['success'] and played['runtime']['tick']==played['play']['tick']==args.ticks,'Player did not finish requested replay')
        check(played['play']['success'] and played['play']['stop_reason']=='replay_complete' and played['play']['runtime_replacements']==0 and played['play']['nvrhi_errors']==0 and played['play']['hardware'],'Renderer/lifecycle failed')
        check(played['runtime']['session_id']==played['play']['current_session_id']==played['play']['initial_session_id'],'Unexpected runtime replacement')
        module=played['gameplay']['module'];values=module['values'];library=next(row for row in descriptor['files'] if row['role']=='library')
        check(module['backend']=='native_aot' and module['assembly_sha256']==library['sha256'] and module['schema']==descriptor['schema'] and module['type']==TYPE and module['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Actual bundled module differs')
        check(values['Arrived']==1 and values['Plans']==1 and values['LastStatus']==0 and values['LastCount']>=4 and values['Ticks']==args.ticks and values['Agent']==uid(300) and values['Replans']==0,'Unchanged compiled NPC did not genuinely plan/arrive')
        check(read_json(report)==played and inventory(bundle)==frozen,'Player report differs or bundle mutated')
        if args.capture:
            capture=output/'player.bmp';check(played['play']['capture_written'] and capture.is_file() and capture.stat().st_size>54,'Requested Vulkan readback missing');record['capture_sha256']=sha(capture)
        record['player']=played;record['checks'].append('Actual exported/relocated native player loads original NPC at tick0, plans a complete multi-corner path and arrives; spectator input stays disjoint.')
        if not args.skip_headless:
            verifier=ROOT/'tests/navigation_runtime_contract.py';verification=output/'Bundled headless route'
            execute([sys.executable,verifier,'--binary',installed,'--descriptor',bundled_descriptor,'--manifest',bundle/'gameplay'/metadata[0]['path'],'--output',verification,'--timeout','220'],unrelated,environment,220)
            evidence=read_json(verification/'evidence.json')
            check(evidence['passed'] and evidence['backend']=='native_aot' and not evidence['cleanup_errors'] and len(evidence['checks'])==8 and len(evidence['owners'])==3 and all(owner['exit_code']==0 for owner in evidence['owners']),'Separate bundled headless route did not pass its complete supplied-host checks')
            verifier_sources=[value for key,value in evidence['sources'].items() if Path(key).as_posix()=='tests/navigation_runtime_contract.py']
            check(evidence['hashes']['binary']==sha(installed) and evidence['hashes']['descriptor']==sha(bundled_descriptor) and verifier_sources==[sha(verifier)],'Headless input/source bytes differ')
            modules=[row['result']['module'] for row in evidence['calls'] if row['method']=='runtime.gameplay.inspect' and 'result' in row and row['result']['module'] is not None]
            check(modules and all(m['backend']=='native_aot' and m['assembly_sha256']==library['sha256'] for m in modules),'Separate headless world used another module')
            record['headless']=dict(evidence_sha256=sha(verification/'evidence.json'),checks=evidence['checks'],calls=len(evidence['calls']),owners=evidence['owners'],cleanup_errors=evidence['cleanup_errors'])
            record['checks'].append('Separate fresh worlds use exact bundled runtime/artifact to verify native route pose, quota, rollback, saves, fresh continuation and replan; not in-player save UX.')
        check(inventory(bundle)==frozen and inventory(distribution)==runtime_before and inventory(artifact.parent)==artifact_before,'Inputs/bundle changed after execution')
        record['bundle_inventory']=frozen;record['runtime_inventory']=runtime_before;record['checks'].append('Actual .pnav/license/runtime/artifact hashes preserved after relocation, owned source removal and isolated-environment execution; no managed PE/runtime shipped.')
        record['passed']=not record['cleanup_errors'] and all(row['exit_code']==0 and not row['forced_cleanup'] for row in record['commands'])
    except BaseException:record['error']=traceback.format_exc()
    finally:
        try:close_author()
        except BaseException:record['cleanup_errors'].append(traceback.format_exc());record['passed']=False
        (output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(output/'evidence.json')
    if not record['passed']:raise SystemExit('Navigation bundle qualification failed; see evidence.json')


if __name__=='__main__':main()
