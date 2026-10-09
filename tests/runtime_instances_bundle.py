#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Two-instance export and relocated Windows Vulkan qualification.

No builds/downloads. Caller inputs remain unchanged. Actual source-free player
execution precedes separate source-free headless reconstruction and captures.
"""
import argparse
import copy
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'tools/python'))
from collection_gameplay_bundle import check, inventory, managed_pe, read_json, sha
from poima_client import WorldClient
from scene_capture import pixels

TYPE = 'Poima.Tests.ManagedInstanceGame'
TEMPLATE, ACTOR, RIG = ('eeeeeeeeeeeeeeeeeeeeeeeeeeee7801',
                       'eeeeeeeeeeeeeeeeeeeeeeeeeeee7802',
                       'eeeeeeeeeeeeeeeeeeeeeeeeeeee7803')
LINK = 'eeeeeeeeeeeeeeeeeeeeeeeeeeee7810'
FEATURES = {'baseline_v7', 'component_collections_v1', 'character_input_v1',
            'hierarchical_instances_v1'}
def uid(n): return f'{n:032x}'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'world', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, action='append', help='Repeat to qualify both GPUs; default0,1')
    parser.add_argument('--ticks', type=int, default=90)
    parser.add_argument('--timeout', type=float, default=900)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Use native Windows Python and native Windows paths')
    if sys.flags.optimize: parser.error('Do not disable assertions in capture decoding')
    gpus = args.gpu if args.gpu is not None else [0, 1]
    if len(gpus) > 2 or len(set(gpus)) != len(gpus) or any(not 0 <= n <= 4095 for n in gpus):
        parser.error('Supply one or two distinct GPU indices0..4095')
    if not 30 <= args.ticks <= 240 or not math.isfinite(args.timeout) or not 120 <= args.timeout <= 1800:
        parser.error('Ticks30..240; finite timeout120..1800seconds')
    output = args.output.resolve()
    if output.exists(): parser.error('--output must be new')
    output.mkdir(parents=True); logs = output/'logs'; logs.mkdir()
    record = dict(passed=False, commands=[], calls=[], owners=[], cleanup_errors=[], checks=[],
                  source_hashes={str(Path(__file__).relative_to(ROOT)):sha(__file__),
                     'tests/collection_gameplay_bundle.py':sha(ROOT/'tests/collection_gameplay_bundle.py'),
                     'tests/collection_gameplay_contract.py':sha(ROOT/'tests/collection_gameplay_contract.py'),
                     'tests/scene_capture.py':sha(ROOT/'tests/scene_capture.py'),
                     **{str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'tools/python/poima_client').glob('*.py'))}},
                  limits=['Original FBX-to-pose/weighted-geometry oracles are separate managed-instance contract evidence.',
                    'The image comparison replays the same compiled artifact through native player and separate runtime capture; it is not an independent original-source image oracle.',
                    'An added observer controller is required by the existing bundle entry contract; the two NPCs are born solely by the compiled Tick callback.',
                    'Scripted Vulkan replay/readbacks on an equipped Windows development host; no clean-machine, physical-input, GUI, crowd or performance claim.'])
    deadline = time.monotonic()+args.timeout; client = None; protected = {}; input_world = None
    def remaining():
        value = deadline-time.monotonic(); check(value > 0, 'Overall deadline expired'); return value
    def rpc(method, params=None):
        row = dict(method=method, params=params or {}); record['calls'].append(row)
        result = client.call(method, params or {}, timeout=min(180, remaining())); row['result'] = result; return result
    def open_owner(binary, world, label):
        nonlocal client
        check(client is None, 'Owner already active')
        client = WorldClient.open(str(binary), str(world));record['owners'].append(dict(label=label, exit_code=None))
    def close_owner():
        nonlocal client
        if client is None: return
        owner, client = client, None
        row = record['owners'][-1]
        try: owner.close()
        except BaseException: row['cleanup_error'] = traceback.format_exc(); raise
        finally:
            transport = owner.transport
            row.update(exit_code=transport.returncode, process_id=transport.process_id,
                       stderr=transport.stderr_tail, stderr_truncated=transport.stderr_truncated)
        check(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'], 'Owner did not exit cleanly')
    def cli(binary, *arguments, cwd=output, env=None):
        index=len(record['commands']); stdout=logs/f'{index}.stdout';stderr=logs/f'{index}.stderr'
        row=dict(command=list(map(str,[binary,*arguments])),cwd=str(cwd),exit_code=None,forced_cleanup=False,
                 stdout=str(stdout),stderr=str(stderr));record['commands'].append(row);process=None
        with stdout.open('wb') as out, stderr.open('wb') as err:
            try:
                process=subprocess.Popen(row['command'],cwd=cwd,env=env,stdin=subprocess.DEVNULL,stdout=out,stderr=err)
                row['pid']=process.pid;process.wait(timeout=min(300,remaining()))
            finally:
                if process is not None:
                    if process.poll() is None:
                        row['forced_cleanup']=True
                        try:
                            killed=subprocess.run([str(Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'),'/PID',str(process.pid),'/T','/F'],capture_output=True,timeout=15)
                            row['tree_cleanup_exit_code']=killed.returncode
                            check(killed.returncode == 0, 'Process-tree cleanup failed')
                        finally:
                            if process.poll() is None: process.kill()
                            process.wait(timeout=10)
                    row['exit_code']=process.returncode
        row.update(stdout_sha256=sha(stdout),stderr_sha256=sha(stderr))
        check(row['exit_code']==0 and not row['forced_cleanup'],'Native CLI failed; retained logs identify failure')
        response=read_json(stdout);check(response['status']=='ok','CLI structured error');row['result']=response['result'];return response['result']
    def transact(ops):
        return rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=rpc('world.inspect')['revision'],ops=ops))
    def same_values(a,b):
        return {k:v for k,v in a.items() if k not in ('ObservedEpochHigh','ObservedEpochLow')} == {k:v for k,v in b.items() if k not in ('ObservedEpochHigh','ObservedEpochLow')}
    try:
        binary=args.binary.resolve(strict=True);runtime=args.runtime.resolve(strict=True)
        artifact=args.artifact.resolve(strict=True);input_world=args.world.resolve(strict=True)
        input_assets=Path(str(input_world)+'.assets');check(input_assets.is_dir(),'Missing actual authored model closure')
        descriptor=read_json(artifact);installed=read_json(runtime/'runtime.json');original=read_json(input_world)
        check(descriptor['type']==TYPE and descriptor['identity']=='poima.test.managed-instances' and descriptor['target_os']=='Windows' and
              descriptor['minimum_services_bytes']==232 and descriptor['services_version']==7 and set(descriptor['required_features'])==FEATURES,
              'Supply the actual Windows hierarchical NativeAOT consumer')
        check(descriptor['engine_version']==installed['engine_version']==cli(binary,'version')['version']=='0.0.78','Inputs must be one .78 cohort')
        check(FEATURES <= set(installed['gameplay_features']) and installed['gameplay_services_bytes']>=232 and
              all(installed['features'].get(k) for k in ('simulation','renderer','native_gameplay','game_ui','asset_provenance')),'Installed runtime lacks required actual features')
        check(set(original['entities'])=={uid(1),uid(100)} and set(original['templates'])=={TEMPLATE},'Supply the managed-instance authored fixture, not a pre-spawned graph')
        recipe=original['templates'][TEMPLATE];members=len(recipe['entities'])
        check(recipe['root']==ACTOR and recipe['entities'][ACTOR]['components']['CharacterController']['camera'] is None and
              recipe['entities'][RIG]['components']['AnimationRig']['clip']==0 and
              recipe['entities'][RIG]['components']['AnimationRig']['loop'] is True,'Original actor recipe differs')
        asset=recipe['entities'][RIG]['components']['AnimationRig']['asset']
        protected=dict(runtime=(runtime,inventory(runtime)),artifact=(artifact.parent,inventory(artifact.parent)),assets=(input_assets,inventory(input_assets)))
        record['inputs']=dict(binary_sha256=sha(binary),world_sha256=sha(input_world),artifact_sha256=sha(artifact),runtime_sha256=sha(runtime/'runtime.json'))
        project=output/'Owned source project';project.mkdir();world=project/'world.json';shutil.copy2(input_world,world);shutil.copytree(input_assets,Path(str(world)+'.assets'))
        open_owner(binary,world,'owned-observer-authoring')
        transact([dict(op='entity.create',id=uid(1000),name='Independent export observer'),
           dict(op='component.set',id=uid(1000),type='Transform',value=dict(position=[0,0,14],rotation=[0,0,0,1],scale=[1,1,1])),
           dict(op='component.set',id=uid(1000),type='CharacterController',value=dict(radius=.3,height=1.8,speed=4,jump_speed=5,camera=uid(100))),
           dict(op='entity.reparent',id=uid(100),parent=uid(1000),mode='keep_local'),
           dict(op='component.set',id=uid(100),type='Transform',value=dict(position=[0,2,0],rotation=[0,0,0,1],scale=[1,1,1]))])
        model=rpc('asset.inspect',dict(asset=asset));check(model['vertices']==1029,'Original weighted source vertex count differs')
        close_owner();authored=read_json(world);check(authored['templates']==original['templates'],'Observer authoring changed NPC recipes')
        local=project/'gameplay';local.mkdir();shutil.copy2(artifact,local/'native-gameplay.json')
        for item in descriptor['files']:
            path=Path(item['path']);check(not path.is_absolute() and '..' not in path.parts,'Unsafe artifact inventory')
            source=artifact.parent/path;check(source.stat().st_size==item['size'] and sha(source)==item['sha256'],'Artifact payload mismatch')
            target=local/path;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        check(not managed_pe(local/descriptor['library']),'Gameplay image still contains IL')
        manifest=project/'project.json';manifest.write_text(json.dumps(dict(format='poima.project',version=2,project_id=uuid.uuid4().hex,name='Compiled hierarchical actors',
            entry=dict(world='world.json',controller=uid(1000),camera=uid(100)),audio=False,
            gameplay=dict(descriptor='gameplay/native-gameplay.json',values=dict(Mode=1))))+'\n',encoding='utf-8')
        before=inventory(project);cli(binary,'project','inspect',manifest)
        exported=output/'Exported game';cli(binary,'project','build',manifest,'--runtime',runtime,'--output',exported)
        check(inventory(project)==before,'Export changed owned source project')
        relocation=Path(tempfile.mkdtemp(prefix='Poima .78 instance relocation ',dir=ROOT.parent));check(not relocation.is_relative_to(ROOT),'Relocation must leave checkout')
        bundle=relocation/'game';shutil.move(str(exported),bundle);unrelated=relocation/'unrelated';unrelated.mkdir()
        frozen=inventory(bundle);game_file=bundle/'game.json';game=read_json(game_file);bundle_binary=bundle/game['engine']['executable'];bundle_artifact=bundle/game['gameplay']['descriptor']
        check(game['gameplay']['values']==dict(Mode=1) and game['entry']['camera']==uid(100) and game['entry']['controller']==uid(1000),'Typed initial mode/entry changed')
        check(sha(bundle_binary)==sha(binary) and sha(bundle_artifact)==sha(artifact),'Bundle runtime/game image changed')
        check(all(frozen['gameplay/'+r['path']]==r['sha256'] for r in descriptor['files']),'Gameplay closure changed')
        check(all(not managed_pe(bundle/p) and Path(p).suffix.lower() not in ('.fbx','.gltf','.glb','.cs','.csproj') for p in frozen),'Development source or IL leaked into bundle')
        check(not any(Path(p).name.lower() in ('hostfxr.dll','coreclr.dll','poima.gameplay.dll','poima.managedbridge.dll') for p in frozen),'Development runtime leaked')
        environment=dict(os.environ);system=Path(environment['SYSTEMROOT']);environment['PATH']=str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_','COREHOST_','POIMA_')):environment.pop(key)
        shutil.rmtree(project);check(not project.exists() and not exported.exists(),'Owned authoring source remains')
        record['source_removed_before_player']=True;record['relocation_root']=str(relocation)
        cli(bundle_binary,'game','inspect',game_file,cwd=unrelated,env=environment)
        replay=output/'neutral-replay.json';replay.write_text(json.dumps([dict(ticks=args.ticks)])+'\n',encoding='utf-8')
        record['players']=[]
        for gpu in gpus:
            capture=output/f'player-gpu-{gpu}.bmp';report=output/f'player-gpu-{gpu}.json'
            played=cli(bundle_binary,'game','run',game_file,'--replay',replay,'--gpu',gpu,'--samples',4,'--width',960,'--height',540,
                '--settings-overrides','{"ui.scale":1}','--capture',capture,'--report',report,cwd=unrelated,env=environment)
            module=played['gameplay']['module'];values=module['values'];play=played['play'];summary=played['runtime']
            check(played['success'] and play['success'] and play['stop_reason']=='replay_complete' and summary['tick']==play['tick']==args.ticks and
                  play['nvrhi_errors']==0 and play['hardware'] and play['runtime_replacements']==0,'Actual relocated Vulkan player failed')
            check(module['backend']=='native_aot' and module['type']==TYPE and module['assembly_sha256']==next(r['sha256'] for r in descriptor['files'] if r['role']=='library') and
                  module['schema']==descriptor['schema'] and module['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),'Different native compiled module loaded')
            check(values['Ticks']==args.ticks and values['Births']==2 and values['Mode']==0 and values['First']!=values['Second'] and values['FirstRig']!=values['SecondRig'] and
                  all(values[n]!=uid(0) for n in ('First','Second','FirstRig','SecondRig')) and values['Drive']==1,'Actual first Tick did not birth two actors')
            check(values['FirstZ']<-.1 and values['SecondX']<3.9 and summary['entities']==3+2*members and summary['characters']==3,'Compiled actor movement/live membership missing')
            draws=play['render_diagnostics']['last_draws'];check(draws['skinned_instances']==2 and draws['skinned_vertices']==2058 and draws['camera_draws']>=2,'Actual original weighted actors were not submitted for GPU skinning/rendering')
            check(play['capture_written'] and capture.is_file() and read_json(report)==played and inventory(bundle)==frozen,'Readback/report missing or bundle mutated')
            image=pixels(capture);check(len(image)==540 and all(len(row)==960 for row in image),'Player viewport differs')
            record['players'].append(dict(gpu=gpu,result=played,capture_sha256=sha(capture),capture_path=str(capture)))
        record['source_free_players_completed_before_reconstruction']=True
        # Separate owned copy, never modify the verified bundle or supply content
        # to the completed source-free players. No original FBX is imported here.
        reconstructed=output/'Separate bundled content runtime';reconstructed.mkdir();headless_world=reconstructed/'world.json'
        shutil.copy2(bundle/'content/world.json',headless_world);shutil.copytree(bundle/'content/world.json.assets',Path(str(headless_world)+'.assets'))
        open_owner(bundle_binary,headless_world,'separate-source-free-runtime')
        session=uuid.uuid4().hex;rpc('runtime.start',dict(session_id=session,revision=rpc('world.inspect')['revision']))
        rpc('runtime.gameplay.load_native',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,expected_structure_revision=0,
            descriptor=str(bundle_artifact),expected_descriptor_sha256=sha(bundle_artifact),values=dict(Mode=1)))
        rpc('runtime.step',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,expected_structure_revision=0,ticks=args.ticks))
        observed=rpc('runtime.gameplay.inspect',dict(session_id=session,include_schema=True));check(same_values(observed['module']['values'],record['players'][0]['result']['gameplay']['module']['values']),'Compiled player/headless state diverged beyond explicit save epoch')
        instances={};values=observed['module']['values'];structure=rpc('runtime.inspect',dict(session_id=session))['structure_revision']
        check(structure==1,'First compiled two-birth Tick must publish one structural batch')
        for name,speed in (('First',1),('Second',2)):
            instance=rpc('runtime.instance',dict(session_id=session,tick=args.ticks,id=values[name],expected_structure_revision=structure));nodes=instance['nodes'];instances[name]=instance
            check(len(nodes)==members and nodes[ACTOR]==values[name] and nodes[RIG]==values[name+'Rig'],'Actual compiled instance lookup differs')
            rig=rpc('runtime.entity',dict(session_id=session,tick=args.ticks,id=nodes[RIG]));clock=rig['animation']
            check(clock['clip']==0 and clock['speed']==speed and clock['loop'] and clock['playing'] and abs(clock['time']-math.fmod(args.ticks*speed/60,clock['duration']))<3e-8,'Native independent instance clock differs')
            component=rpc('runtime.component.get',dict(session_id=session,tick=args.ticks,id=values[name],type=LINK))['values']
            check(component[uid(3)]==nodes[RIG] and component[uid(4)]==[values[name],nodes[RIG]],'Compiled scalar/array instance links differ')
        check(not(set(instances['First']['nodes'].values())&set(instances['Second']['nodes'].values())),'Live instance maps alias')
        record['instances']=instances;record['headless_gameplay']=observed;record['capture_comparisons']=[]
        for player in record['players']:
            gpu=player['gpu'];path=output/f'runtime-gpu-{gpu}.bmp'
            boundary=rpc('runtime.inspect',dict(session_id=session))
            check(boundary['tick']==args.ticks,'Runtime advanced before guarded capture')
            capture=rpc('runtime.capture',dict(session_id=session,tick=args.ticks,ui_revision=boundary['ui_revision'],camera=uid(100),path=str(path),width=960,height=540,samples=4,gpu=gpu,ui_scale=1,profile=True))
            check(capture['capture_written'] and capture['hardware'] and capture['nvrhi_errors']==0,'Separate source-free runtime capture failed')
            a,b=pixels(Path(player['capture_path'])),pixels(path);check(len(a)==len(b)==540,'Capture height differs')
            maximum=0;changed=0;over2=0
            for ar,br in zip(a,b):
                check(len(ar)==len(br)==960,'Capture width differs')
                for av,bv in zip(ar,br):
                    delta=max(abs(x-y) for x,y in zip(av,bv));maximum=max(maximum,delta);changed+=delta>0;over2+=delta>2
            record['capture_comparisons'].append(dict(gpu=gpu,maximum_rgb_difference=maximum,changed_pixels=changed,pixels_over_two=over2,runtime_capture_sha256=sha(path),report=capture))
            check(maximum<=2,'Player and separate native runtime readbacks differ; investigate retained images before changing thresholds')
        close_owner();check(inventory(bundle)==frozen,'Separate execution mutated bundle')
        record['bundle_inventory']=frozen;record['checks']=['Actual Mode1 NativeAOT first Tick births two complete original imported actors; no static authored actor substitution.',
            'Owned source project removed, bundle relocated, ordinary PATH and unrelated working directory before actual Vulkan playback.',
            'Both compiled actor movement, two skin dispatches2058vertices, complete native maps/links and speed1/2 clocks observed.',
            'Completed source-free player readbacks compared with separately reconstructed same-artifact native runtime captures.']
        record['passed']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        try:close_owner()
        except BaseException:record['cleanup_errors'].append(traceback.format_exc());record['passed']=False
        try:
            if input_world is not None and 'inputs' in record:
                check(sha(input_world)==record['inputs']['world_sha256'],'Caller authored world changed')
                check(sha(args.binary.resolve(strict=True))==record['inputs']['binary_sha256'],'Caller binary changed')
            for name,(path,before) in protected.items():check(inventory(path)==before,'Protected input changed: '+name)
            for name,pin in record['source_hashes'].items():check(sha(ROOT/name)==pin,'Verifier source changed: '+name)
        except BaseException:record['cleanup_errors'].append(traceback.format_exc());record['passed']=False
        record['rpc_count']=len(record['calls']);(output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(dict(passed=record['passed'],evidence=str(output/'evidence.json'),rpc_count=record['rpc_count'])))
    return 0 if record['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
