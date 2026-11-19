#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify the published pickup module inside a relocated Windows game bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from item_attachment_pickup import ACTOR, APPLY, HOLDER, INITIAL, INPUT, ITEM, START
from runtime_pose_frames import id_request,transform,uid
import world_physical_contract as native

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import RpcError, WorldClient

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--world',type=Path,required=True,help='Authored world.json from the native pickup fixture.')
parser.add_argument('--runtime',type=Path,required=True,help='Installed matching Windows runtime directory.')
parser.add_argument('--artifact',type=Path,required=True,help='Published Windows Native AOT pickup descriptor.')
parser.add_argument('--output',type=Path,required=True,help='New directory for the project, bundle, saves and traces.')
parser.add_argument('--windows-interop',action='store_true',help='Translate paths for the Windows executable from WSL.')
args=parser.parse_args()
out=args.output.resolve();out.mkdir()
runtime=args.runtime.resolve()
engine=runtime/'bin/poima.exe'
source=out/'source';source.mkdir()
author_world=args.world.resolve()
world=source/'world.json';shutil.copy2(author_world,world)
shutil.copytree(author_world.with_suffix('.json.assets'),world.with_suffix('.json.assets'))
artifact=args.artifact.resolve()
manifest=source/'project.json'
bundle=out/'exported'
relocated=out/'Relocated pickup fixture'
saves=out/'saves';saves.mkdir()
entry=uid(190)
report=dict(passed=False,build_requests=[],native_calls=0)


def win(path):
    if not args.windows_interop:
        return str(path.resolve())
    return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True,timeout=5).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*parts):
    argv=[str(engine),*(win(part) if isinstance(part,Path) else part for part in parts)]
    result=subprocess.run(argv,text=True,capture_output=True,timeout=120)
    with (out/'cli.jsonl').open('a') as calls:
        calls.write(json.dumps(dict(argv=argv,returncode=result.returncode,stdout=result.stdout,stderr=result.stderr))+'\n')
    response=json.loads(result.stdout)
    assert result.returncode==0 and response['status']=='ok',response
    report['build_requests'].append(response)
    return response['result']


def call(client,method,**params):
    report['native_calls']+=1
    return client.call(method,params,timeout=60)


def saved(slot):
    packages=list((saves/f'slot-{slot}').glob('p-*.bin'))
    assert len(packages)==1,packages
    return packages[0].read_bytes()


def save(client,sid,slot,tick,structure):
    receipt=call(client,'save.write',session_id=sid,request_id=id_request(),configuration_generation=1,slot=slot,
        expected_generation=0,expected_tick=tick,expected_gameplay_revision=1,expected_structure_revision=structure)
    snapshot=saved(slot)
    assert receipt['sha256']==hashlib.sha256(snapshot).hexdigest()
    return snapshot


def step(client,sid,start,count,structure,**extras):
    done=call(client,'runtime.step',session_id=sid,request_id=id_request(),expected_tick=start,
        expected_structure_revision=structure,ticks=count,**extras)
    assert done['tick']==start+count
    return done


def inventory():
    return {str(path.relative_to(relocated)):sha(path) for path in relocated.rglob('*') if path.is_file()}


try:
    doc=json.loads(world.read_bytes())
    ctrl=dict(doc['entities'][ACTOR]['components']['CharacterController'],camera=entry)
    with WorldClient.open(str(engine),win(world),trace_directory=out/'authoring') as author:
        changed=call(author,'world.transact',request_id=id_request(),base_revision=doc['revision'],ops=[
            dict(op='entity.create',id=entry,name='Standalone entry camera',parent=ACTOR),
            native.component(entry,'Transform',transform((0,1.5,0))),
            native.component(entry,'Camera',dict(vertical_fov=60,near=.05,far=40)),
            native.component(ACTOR,'CharacterController',ctrl)])
        revision=changed['revision']
    project=dict(format='poima.project',version=1,project_id=id_request(),name='Item Pickup Check',
        entry=dict(world='world.json',controller=ACTOR,camera=entry),audio=False)
    manifest.write_text(json.dumps(project,indent=2)+'\n')
    adoption=out/'adopt.json'
    adoption.write_text(json.dumps(dict(request_id=id_request(),
        expected_project_sha256=sha(manifest),expected_artifact_sha256=sha(artifact),values=INITIAL))+'\n')
    command('project','adopt-gameplay',manifest,'--artifact',artifact,'--request',adoption)
    command('project','build',manifest,'--runtime',runtime,'--output',bundle)
    bundle.rename(relocated)
    source.rename(out/'source-offline')
    binary=relocated/'runtime/bin/poima.exe'
    game=relocated/'game.json'
    result=command('game','inspect',game)
    assert result['revision']==revision
    frozen=inventory()
    with WorldClient.open_game(str(binary),win(game),save_root=win(saves),trace_directory=out/'warm') as client:
        description=call(client,'world.describe')
        assert description['read_only'] and APPLY in description['methods']
        sid=call(client,'runtime.status')['session_id']
        receipt=call(client,APPLY,session_id=sid,request_id=id_request(),expected_tick=0,expected_structure_revision=0,
            commands=[dict(entity=ITEM,mode='keep_world',anchor=START)])
        assert receipt['structure_revision']==1
        step(client,sid,0,22,1)
        miss,=call(client,'runtime.attachment.outcomes',session_id=sid,tick=22,structure_revision=2)['outcomes']
        assert miss['status']=='surface_miss'
        binding,=call(client,'runtime.attachment.inspect',session_id=sid,tick=22,structure_revision=2)['bindings']
        assert binding['released'] is True
        body=binding['anchor']
        ready=step(client,sid,22,1,2,inputs=INPUT)
        assert ready['save_serviced'] and ready['save_operation']['committed_tick']==23
        found,=call(client,'runtime.attachment.inspect',session_id=sid,tick=23,structure_revision=3)['bindings']
        assert found['anchor']==HOLDER and 'released' not in found
        try:
            call(client,'runtime.entity',session_id=sid,id=body,tick=23,structure_revision=3)
        except RpcError as e:
            assert e.code==-32004,e
        else:
            raise AssertionError('Retired collision body still resolves.')
        assert save(client,sid,'picked-observed',23,3)==saved('pickup-complete')
        step(client,sid,23,6,3)
        continued=save(client,sid,'warm-continued',29,3)
    assert client.transport.returncode==0

    with WorldClient.open_game(str(binary),win(game),save_root=win(saves),trace_directory=out/'cold') as client:
        existing=call(client,'runtime.status')['session_id']
        target=id_request()
        loaded=call(client,'save.load',request_id=id_request(),configuration_generation=1,slot='pickup-complete',
            expected_generation=1,revision=revision,expected_session_id=existing,expected_tick=0,
            expected_gameplay_revision=1,expected_structure_revision=0,new_session_id=target)
        assert loaded['tick']==23 and loaded['structure_revision']==3 and not loaded['source_stale']
        assert save(client,target,'cold-restored',23,3)==saved('pickup-complete')
        step(client,target,23,6,3)
        assert save(client,target,'cold-continued',29,3)==continued
    assert client.transport.returncode==0 and inventory()==frozen and not source.exists()
    report.update(passed=True,source_renamed=True,inventory_files=len(frozen),bundle_manifest_sha256=sha(game),
        bundled_engine_sha256=sha(binary),artifact_sha256=sha(artifact),
        checked_slots=['pickup-miss','pickup-complete','warm-continued','cold-continued'],complete_checkpoint_pairs=3)
finally:
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('Relocated native game dropped, picked up and restored the same prop.')
