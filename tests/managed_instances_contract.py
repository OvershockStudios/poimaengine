#!/usr/bin/env python3
"""Compiled hierarchical actors from caller-supplied original licensed FBX.

No builds, downloads, source edits, teleports or clock overrides. Original-source
poses use the native importer; separate quaternion/FK/weighted-geometry math is
reused as the oracle. Source-free exported-player qualification is separate.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback
import uuid
from locomotion_yard_contract import OriginalOracle, maximum_matrix_error, normalized
from animation_rotation_retarget import from_columns, matrix, multiply

ROOT=Path(__file__).resolve().parents[1]
TYPE='Poima.Tests.ManagedInstanceGame'
TEMPLATE, ACTOR, RIG = ('eeeeeeeeeeeeeeeeeeeeeeeeeeee7801','eeeeeeeeeeeeeeeeeeeeeeeeeeee7802','eeeeeeeeeeeeeeeeeeeeeeeeeeee7803')
LINK='eeeeeeeeeeeeeeeeeeeeeeeeeeee7810'
FIELD=lambda n:f'{n:032x}'
ACTIONS={name:FIELD(810+i) for i,name in enumerate(('inspect','birth','fail','cancel','break-references','repair','despawn','unknown','pause','resume','save','load'))}

def need(value,message):
    if not value:raise AssertionError(message)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def launcher():
    path=ROOT/'examples/locomotion-yard/run.py'
    spec=importlib.util.spec_from_file_location('poima_managed_instances_launcher',path)
    need(spec is not None and spec.loader is not None,'Existing intake transport is missing')
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def continuation(value):
    result=copy.deepcopy(value)
    # The first Tick after restore deliberately observes its new save epoch.
    for name in ('ObservedEpochHigh','ObservedEpochLow'):result['values'].pop(name)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','manifest','source-directory','output'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('assembly','hostfxr','bridge','descriptor'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--timeout',type=float,default=1200)
    args=parser.parse_args()
    if bool(args.descriptor)==bool(args.assembly) or (not args.descriptor and not(args.hostfxr and args.bridge)):
        parser.error('Supply descriptor OR assembly with hostfxr and bridge')
    if not math.isfinite(args.timeout) or not 60<=args.timeout<=1800:parser.error('Timeout must be finite60..1800seconds')
    if args.output.exists():parser.error('--output must be new')
    output=args.output.resolve();output.mkdir(parents=True);world=output/'world.json'
    library=launcher();manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
    need(manifest['format']=='poima.components' and manifest['version']==1 and len(manifest['schemas'])==1 and manifest['schemas'][0]['id']==LINK and manifest['schemas'][0]['version']==2,'Supply actual compiled InstanceLink manifest')
    fields={f['id']:f for f in manifest['schemas'][0]['fields']}
    need(set(fields)=={FIELD(n) for n in range(1,5)} and fields[FIELD(1)]['kind']=='int32' and
         fields[FIELD(2)]['kind']==fields[FIELD(3)]['kind']=='entity' and fields[FIELD(4)]['kind']=='array' and
         fields[FIELD(4)]['element_kind']=='entity' and fields[FIELD(4)]['capacity']==2,'Compiled link schema differs')
    inputs={name:getattr(args,name).resolve(strict=True) for name in ('binary','manifest','assembly','hostfxr','bridge','descriptor') if getattr(args,name)}
    for name,path in inputs.items():need(path.is_file(),'Missing artifact file: '+name);setattr(args,name,path)
    if args.descriptor:
        descriptor=json.loads(args.descriptor.read_text(encoding='utf-8'))
        need(descriptor['type']==TYPE and descriptor['identity']=='poima.test.managed-instances' and descriptor['minimum_services_bytes']==232 and descriptor['services_version']==7 and
             {'baseline_v7','component_collections_v1','character_input_v1','hierarchical_instances_v1'}<=set(descriptor['required_features']),'Supply actual232-byte hierarchical consumer artifact')
        for index,item in enumerate(descriptor['files']):
            relative=Path(item['path']);need(not relative.is_absolute() and '..' not in relative.parts,'Unsafe artifact path')
            path=(args.descriptor.parent/relative).resolve(strict=True)
            need(path.is_relative_to(args.descriptor.parent) and path.is_file() and path.stat().st_size==item['size'] and sha(path)==item['sha256'],'Artifact inventory mismatch')
            inputs['artifact_'+str(index)]=path
    args.source_directory=args.source_directory.resolve(strict=True)
    originals={name:args.source_directory/name for name in library.SOURCE_SHA}
    need(all(path.is_file() and sha(path)==library.SOURCE_SHA[name] for name,path in originals.items()),'Supply unchanged Kenney Protagonists1.1 body/Run/Idle/license')
    pins={name:sha(path) for name,path in originals.items()}
    sources=[Path(__file__),ROOT/'examples/locomotion-yard/run.py',ROOT/'tests/managed_instance_gameplay/ManagedInstanceGame.cs',
        ROOT/'tests/managed_instance_gameplay/Poima.ManagedInstanceGame.csproj',*[ROOT/'tests'/p for p in
        ('locomotion_yard_contract.py','animation_rotation_retarget.py','animation_rotation_retarget_capture.py','rotation_retarget_fixture.py','frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')],
        *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
    record=dict(passed=False,calls=[],owners=[],checks=[],cleanup_errors=[],backend='native_aot' if args.descriptor else 'coreclr',
        hashes={name:sha(path) for name,path in inputs.items()},sources={p.relative_to(ROOT).as_posix():sha(p) for p in sources},original_source_hashes=pins,
        limits=['Source-pose sampling uses normalized original native FBX intake, not an independent FBX decoder.',
            'Independent quaternion-chain/FK/weighted original geometry oracle; no stride/contact/IK/loop-repair or crowd-performance qualification.',
            'Owned source copies are removed before fresh-owner continuation; caller originals remain read-only for the separate oracle.',
            'Headless compiled/native lifecycle test. Exported player, renderer readback, GUI and physical input are qualified separately.'])
    owned=output/'owned-sources'
    for name,path in originals.items():
        target=owned/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target);need(sha(target)==pins[name],'Owned source copy differs')
    deadline=time.monotonic()+args.timeout;owners=[]
    def native(path):
        path=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',path],text=True,timeout=10).strip() if args.windows_interop and os.name!='nt' else path

    class Game(library.OwnedGame):
        def inspect(self):
            result=super().inspect()
            result['component_revision']=self.rpc('runtime.components',dict(session_id=self.session))['component_revision']
            return result
        def step(self,count=1,error=None):
            result=self.rpc('runtime.step',dict(request_id=uuid.uuid4().hex,session_id=self.session,expected_tick=self.tick,ticks=count,
                expected_structure_revision=self.inspect()['structure_revision']),error)
            if error is None:self.session,self.tick=result['current_session_id'],result['current_tick']
            return result
        def load(self,error=None):
            method='runtime.gameplay.load_native' if args.descriptor else 'runtime.gameplay.load'
            result=self.rpc(method,dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,
                expected_revision=self.module()['revision'],expected_structure_revision=self.inspect()['structure_revision'],**self.config()),error)
            if error is None:
                self.revision=result['revision'];need(result['module']['backend']==('native_aot' if args.descriptor else 'coreclr'),'Actual compiled backend differs')
            return result
        def config(self):
            return dict(descriptor=native(args.descriptor)) if args.descriptor else dict(assembly=native(args.assembly),hostfxr=native(args.hostfxr),bridge=native(args.bridge),type=TYPE)
        def control(self,name):
            state,module=self.inspect(),self.module()
            result=self.rpc('runtime.ui.activate',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,
                expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=module['revision'],expected_structure_revision=state['structure_revision'],id=ACTIONS[name]))
            self.session,self.tick=result['current_session_id'],result['current_tick'];return result
        def instance(self,identity):
            return self.rpc('runtime.instance',dict(session_id=self.session,id=identity,tick=self.tick,expected_structure_revision=self.inspect()['structure_revision']))
        def write(self,slot):
            state,module=self.inspect(),self.module()
            return self.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=self.configuration,slot=slot,expected_generation=0,
                session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=module['revision'],expected_component_revision=state['component_revision'],
                expected_structure_revision=state['structure_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        def restore(self,slot):
            state,module=(self.inspect(),self.module()) if self.session else (None,None)
            params=dict(request_id=uuid.uuid4().hex,configuration_generation=self.configuration,slot=slot,expected_generation=1,revision=self.rpc('world.inspect')['revision'],
                expected_session_id=self.session,expected_tick=self.tick if self.session else None,expected_gameplay_revision=module['revision'] if module else None,
                expected_component_revision=state['component_revision'] if state else None,expected_structure_revision=state['structure_revision'] if state else None,
                expected_ui_revision=state['ui_revision'] if state else None,expected_control_sequence=state['control_sequence'] if state else None,new_session_id=uuid.uuid4().hex,gameplay=self.config())
            result=self.rpc('save.load',params);self.session,self.tick=params['new_session_id'],result['tick'];self.revision=self.module()['revision'];return result
    def open_game(label):
        game=Game(args,world,label,record,deadline,native,manifest);owners.append(game);return game
    def close_game(game):
        game.close();transport=game.client.transport;row=next(r for r in record['owners'] if r['label']==game.label)
        row.update(process_id=transport.process_id,stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated)
        need(row['exit_code']==0 and not row['stderr'] and not row['stderr_truncated'],'Nonclean native owner exit')
    def tx(game,ops):return game.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=game.rpc('world.inspect')['revision'],ops=ops))
    def snapshot(game):
        state,module=game.inspect(),game.module();values=module['module']['values'];instances={};entities={identity:game.entity(identity) for identity in (FIELD(1),FIELD(100))};components={}
        for name in ('First','Second'):
            root=values[name]
            if root==FIELD(0):continue
            instance=game.instance(root);instances[name]=instance
            for identity in sorted(instance['nodes'].values()):entities[identity]=game.entity(identity)
            components[root]=game.rpc('runtime.component.get',dict(session_id=game.session,tick=game.tick,id=root,type=LINK))
        return normalized(dict(tick=game.tick,values=values,instances=instances,entities=entities,components=components,backend=module['module']['backend'],gameplay_revision=module['revision'],
            revisions={key:state[key] for key in ('structure_revision','component_revision','ui_revision','control_sequence')},
            ui=game.rpc('runtime.ui.inspect',dict(session_id=game.session,tick=game.tick))))
    def pose(game,oracle,recipe):
        for name in ('First','Second'):
            root=game.values()[name]
            if root==FIELD(0):continue
            instance=game.instance(root);actor,rig=game.entity(root),game.entity(instance['nodes'][RIG]);clock=rig['animation']
            need(clock['clip']==0 and clock['loop'] and clock['playing'] and clock.get('transition') is None,'Actual loop playback differs')
            wrapper=multiply(from_columns(actor['world_matrix']),matrix(**recipe['entities'][RIG]['components']['Transform']))
            need(maximum_matrix_error(wrapper,from_columns(rig['world_matrix']))<1e-5,'Controller attachment differs')
            wanted={i:multiply(wrapper,m) for i,m in oracle.evaluate(game,0,clock['time']).items()}
            actual={i:from_columns(game.entity(instance['nodes'][library.node_id(RIG,i)])['world_matrix']) for i in range(61)}
            error=max(maximum_matrix_error(wanted[i],actual[i]) for i in range(61));need(error<8e-5,'Actual member worlds differ from original-source oracle')
            skin_error=max(math.dist(a,b) for a,b in zip(oracle.skin_points(wanted),oracle.skin_points(actual)))
            need(skin_error<1.5e-4,'Original weighted geometry differs under actual native palettes')
            record.setdefault('pose_observations',[]).append(dict(actor=name,tick=game.tick,clock=clock,maximum_joint_matrix_error=error,maximum_weighted_vertex_error=skin_error))
    try:
        game=open_game('compiled-instances');game.rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=0,manifest=manifest))
        base=owned/'Model/characterMedium.fbx';info=game.rpc('asset.source.inspect',dict(source=native(base)))
        base_nodes=game.page('asset.source.inspect',dict(source=native(base),section='nodes',expected_model_sha256=info['model_sha256']));selections=[]
        for name in ('run','idle'):
            path=owned/('Animations/'+name+'.fbx');donor=game.rpc('asset.source.inspect',dict(source=native(path)));guard=dict(source=native(path),expected_model_sha256=donor['model_sha256'])
            clips=game.page('asset.source.inspect',dict(guard,section='animations'));nodes=game.page('asset.source.inspect',dict(guard,section='nodes'))
            reference=[c for c in clips if c['name']=='Root|0.Targeting Pose'];motion=[c for c in clips if c['name']=='Root|'+name.title()];pelvis=[n['index'] for n in nodes if n['name'] in ('HipsCtrl','Hips')]
            need(len(reference)==len(motion)==1 and len(pelvis)==2,'Original donor profile differs')
            selections.append(dict(source=native(path),clip=motion[0]['index'],name=name.title(),retarget=dict(policy='reference-rotation-v1',
                source_pose=dict(kind='sample',clip=reference[0]['index'],time=0),target_pose=dict(kind='rest'),positions=dict(kind='reference_delta',nodes=sorted(pelvis),scale=1),scales='target_reference',
                expected_source_model_sha256=donor['model_sha256'],expected_target_model_sha256=info['model_sha256'])))
        imported=game.rpc('asset.import',dict(source=native(base),animations=selections));asset=imported['asset']
        tx(game,[dict(op='asset.instantiate',id=RIG,name='Original reusable visual',asset=asset)])
        graph=copy.deepcopy(json.loads(world.read_text(encoding='utf-8'))['entities']);graph[RIG]['parent']=ACTOR
        oracle=OriginalOracle(game,args.source_directory,world,imported,record,native)
        points=oracle.skin_points(oracle.base_worlds);minimum=min(p[1] for p in points);height=max(p[1] for p in points)-minimum
        need(2*.42<height<=4,'Original capsule dimensions exceed controller profile')
        attachment=dict(position=[0,-minimum,0],rotation=[0,1,0,0],scale=[1,1,1]);graph[RIG]['components']['Transform']=attachment
        graph[RIG]['components']['AnimationRig'].update(clip=0,time=0,speed=1,loop=True,playing=True)
        identity=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])
        graph[ACTOR]=dict(name='Reusable native capsule',parent=None,components=dict(Transform=copy.deepcopy(identity),CharacterController=dict(radius=.42,height=height,speed=4,jump_speed=5,camera=None),
            **{'game:'+LINK:{FIELD(1):7,FIELD(2):FIELD(0),FIELD(3):RIG,FIELD(4):[ACTOR,RIG]}}))
        recipe=dict(op='template.set',id=TEMPLATE,name='Original compiled actor assembly',root=ACTOR,entities=graph)
        ops=[dict(op='entity.delete',id=RIG,recursive=True),dict(op='entity.create',id=FIELD(1),name='Ground'),
            dict(op='component.set',id=FIELD(1),type='Transform',value=dict(identity,position=[0,-.5,0],scale=[40,1,40])),
            dict(op='component.set',id=FIELD(1),type='BoxCollider',value=dict(half_extents=[.5,.5,.5],motion='static',mass=1,friction=.5,restitution=0)),
            dict(op='entity.create',id=FIELD(100),name='Independent observer'),dict(op='component.set',id=FIELD(100),type='Camera',value=dict(vertical_fov=60,near=.05,far=100)),recipe]
        # Keep both actors visible: explicit compact controls replace the broad
        # compatibility container, using the sample's wine/neutral palette.
        dp=lambda value:dict(unit='dp',value=value)
        for index,(action,identity_id) in enumerate(ACTIONS.items()):
            ops.append(dict(op='ui.element.set',id=identity_id,element=dict(
                parent=None,name=action,kind='button',text=action,action='instances.'+action,visible=True,enabled=True,
                layout=dict(position='absolute',left=dp(16),top=dp(16+40*index),width=dp(184),height=dp(32)),
                style=dict(color='#f7e8eb',background_color='#722f37' if action=='birth' else '#30252a',
                    border_color='#4f1a23',border_width=1,border_radius=4,font_size=14,
                    hover=dict(background_color='#4f1a23'),focus=dict(border_color='#d46a7e')))))
        tx(game,ops);record['recipe']=recipe;authored=world.read_bytes();game.start();game.load();game.saves(output/'saves')
        initial=snapshot(game);game.control('fail');before=snapshot(game);error=game.step(error=-32040)
        need('Intentional hierarchical birth rollback' in error['message'] and snapshot(game)==before,'Compiled throw did not roll back reserved birth/commands/state')
        game.control('cancel');game.step();canceled=snapshot(game)
        need(canceled['values']['Canceled']==1 and not canceled['instances'] and game.inspect()['entities']==2,'Canceled reserved graph leaked into live state')
        game.control('birth');birth_tick=game.tick;game.step();values=game.values();first,second=values['First'],values['Second']
        need(first!=second and values['Births']==2,'Actual compiled two-birth callback did not publish distinct roots')
        # Independent scalar allocator oracle: authored1/100 are excluded, a
        # rejected callback consumes nothing, the canceled graph consumes all
        # its reservations, and the next two complete graphs allocate in order.
        frontier=1;reserved=[]
        for _ in range(3*len(graph)):
            while frontier in (1,100):frontier+=1
            reserved.append(FIELD(frontier));frontier+=1
        need(first==reserved[len(graph)] and second==reserved[2*len(graph)],'Failed/canceled births changed the public allocation lineage')
        first_map,second_map=game.instance(first),game.instance(second)
        need(len(first_map['nodes'])==len(second_map['nodes'])==len(graph) and not(set(first_map['nodes'].values())&set(second_map['nodes'].values())),'Instances alias or omit members')
        for root,mapping,other,expected_value in ((first,first_map,second,11),(second,second_map,first,22)):
            link=game.rpc('runtime.component.get',dict(session_id=game.session,tick=game.tick,id=root,type=LINK))['values']
            need(link[FIELD(1)]==expected_value and link[FIELD(2)]==other and link[FIELD(3)]==mapping['nodes'][RIG] and link[FIELD(4)]==[root,mapping['nodes'][RIG]],'Birth staged typed references differ')
        origin={root:game.entity(root)['world_matrix'][12:15] for root in (first,second)};game.step(19)
        for root,mapping,speed in ((first,first_map,1),(second,second_map,2)):
            clock=game.entity(mapping['nodes'][RIG])['animation'];expected=math.fmod((game.tick-birth_tick)*speed/60,clock['duration'])
            need(abs(clock['time']-expected)<3e-8 and clock['speed']==speed,'Independent birth clocks changed or restarted')
            now=game.entity(root)['world_matrix'][12:15];need(math.hypot(now[0]-origin[root][0],now[2]-origin[root][2])>.1,'Compiled native controller did not physically move')
        pose(game,oracle,recipe);game.control('unknown');game.step();need(game.values()['Rejected']==1,'Compiled unknown-local check did not execute')
        checkpoint=snapshot(game);receipt=game.write('hierarchy');payloads=list((output/'saves/slot-hierarchy').glob('p-*.bin'))
        need(len(payloads)==1 and sha(payloads[0])==receipt['sha256'],'Actual save receipt lacks exact disk payload')
        payload=json.loads(payloads[0].read_text(encoding='utf-8'));need(payload['snapshot']['version']==6 and len(payload['snapshot']['payload']['structure']['spawned'])==2,'Saved compiled instances lack v6 provenance')
        if args.descriptor:
            before=snapshot(game);game.load(error=-32060);need(snapshot(game)==before,'Unsupported AOT reload mutated state')
        else:
            before=snapshot(game);game.load();after=snapshot(game)
            need(all(after[k]==before[k] for k in before if k!='gameplay_revision') and after['gameplay_revision']>before['gameplay_revision'],'Compatible compiled reload lost live instances')
        game.restore('hierarchy');need(snapshot(game)==checkpoint,'Same-owner immediate restore changed complete checkpoint')
        game.step(17);expected= snapshot(game);pose(game,oracle,recipe)
        game.restore('hierarchy')
        for count in (1,3,2,5,6):game.step(count)
        need(continuation(snapshot(game))==continuation(expected),'Tick partitioning changed complete instance continuation')
        game.control('break-references');before=snapshot(game);game.step(error=-32040)
        need(snapshot(game)==before,'Rejected whole deletion changed membership/physics/clock/typed state')
        game.control('repair');game.step();survivor=game.instance(second)
        need(game.values()['First']==game.values()['FirstRig']==FIELD(0) and survivor['nodes']==second_map['nodes'],'Reference repair failed whole-root retirement')
        for identity_id in first_map['nodes'].values():game.rpc('runtime.entity',dict(session_id=game.session,tick=game.tick,id=identity_id),error=-32004)
        game.control('inspect');need(game.values()['ResolvedControls']>0,'Control resolver did not inspect committed survivor')
        game.control('despawn');game.step();need(game.inspect()['entities']==2 and game.inspect()['characters']==0,'Final retirement retained native membership')
        game.control('birth');game.step();need(game.values()['First']>second and game.values()['Second']>second,'Retired or canceled generated identities reused')
        close_game(game);shutil.rmtree(owned);need(not owned.exists(),'Owned source removal failed')
        fresh=open_game('fresh-without-owned-authoring-sources');fresh.saves(output/'saves');fresh.restore('hierarchy')
        need(snapshot(fresh)==checkpoint,'Fresh immediate restoration lost instance map/typed/native state')
        fresh.step(17);need(continuation(snapshot(fresh))==continuation(expected),'Fresh compiled physics/clock continuation differs');pose(fresh,oracle,recipe)
        need(fresh.instance(first)['nodes']==first_map['nodes'] and fresh.instance(second)['nodes']==second_map['nodes'],'Fresh owner remapped saved identities')
        record.update(checkpoint=checkpoint,checkpoint_payload_sha256=receipt['sha256'],expected_continuation=expected,fresh_continuation=snapshot(fresh),
            failed_birth=error,canceled=canceled,source_removal=dict(owned_removed=True,caller_originals_preserved=True))
        record['checks']=['Original imported graph instanced twice with disjoint native IDs and remapped scalar/array references.',
            'Actual compiled232-byte resolver, reserved read denial, independent speeds and physical controllers.',
            'Reserved cancellation, whole-batch throw and incoming-reference rejection preserve membership, native clocks and typed state.',
            'Repaired whole deletion retires all descendants; surviving IDs persist and retired IDs are not reused.',
            'Exact v6 same/fresh owner checkpoints and unequal fixed-tick continuation retain original-source poses and typed state.',
            'Compatible CoreCLR reload or explicit AOT reload rejection leaves instance membership/clocks intact.']
        need(world.read_bytes()==authored,'Runtime verification changed authored bytes')
        need(all(sha(path)==pins[name] for name,path in originals.items()),'Caller originals changed')
        need(all(sha(path)==record['hashes'][name] for name,path in inputs.items()),'Supplied artifact changed')
        need(all(sha(ROOT/path)==pin for path,pin in record['sources'].items()),'Frozen verifier dependencies changed')
        record['passed']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        for game in reversed(owners):
            if not game.closed:
                try:close_game(game)
                except BaseException:record['cleanup_errors'].append(traceback.format_exc());record['passed']=False
        for label,path,pin in [(name,path,pins[name]) for name,path in originals.items()]+[(name,path,record['hashes'][name]) for name,path in inputs.items()]:
            try:need(path.is_file() and sha(path)==pin,'Protected input changed: '+label)
            except BaseException:record['cleanup_errors'].append(traceback.format_exc());record['passed']=False
        record['rpc_count']=len(record['calls'])
        with (output/'evidence.json').open('x',encoding='utf-8') as stream:json.dump(record,stream,indent=2);stream.write('\n')
    print(json.dumps(dict(passed=record['passed'],rpc_count=record['rpc_count'],owners=len(record['owners']))))
    if not record['passed']:print(record.get('error') or record['cleanup_errors'],file=sys.stderr)
    return 0 if record['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
