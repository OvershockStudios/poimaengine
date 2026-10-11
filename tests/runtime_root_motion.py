#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless native planar root-motion protocol checks; an external owner must gate execution.

Uses original analytic geometry/animation, no renderer or managed compiler.
Run matching-platform Python with --binary ENGINE --output NEW_DIRECTORY.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
import struct
import math
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError
from animation_fixture import ribbon
from gltf_fixture import glb


def need(value, message):
    if not value:
        raise AssertionError(message)


def uid(n): return f'{n:032x}'
def new(): return uuid.uuid4().hex

def transform(position=(0, 0, 0)):
    return dict(position=list(position), rotation=[0, 0, 0, 1], scale=[1, 1, 1])


def set_component(n, kind, value):
    return dict(op='component.set', id=uid(n), type=kind, value=value)


def entity(n, parent=None, position=(0, 0, 0)):
    return [dict(op='entity.create', id=uid(n), name=f'Object {n}', parent=parent),
            set_component(n, 'Transform', transform(position))]


def sphere(owner=100, name='head'):
    return dict(owner=uid(owner), name=name, shape='sphere', radius=.25)


def exact(a, b):
    return json.dumps(a, sort_keys=True, separators=(',', ':'), allow_nan=False) == json.dumps(b, sort_keys=True, separators=(',', ':'), allow_nan=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    need(0 < args.timeout <= 180, 'Timeout must be finite and at most 180 seconds')
    need(not args.output.exists(), 'Fresh output required')
    args.output.mkdir(parents=True)
    deadline = time.monotonic() + args.timeout
    record = dict(passed=False, calls=[], owners=[], checks=[])
    source_pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in [Path(__file__).resolve(), ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py',
                             *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
                             *sorted((ROOT/'tools/python/poima_client').glob('*.json'))]}
    client = None
    total = 0

    def remaining():
        value = deadline - time.monotonic()
        need(value > 0, 'Whole qualification deadline exhausted')
        return min(15, value)

    def open_client():
        return WorldClient.open(str(args.binary.resolve()), str((args.output/'w.json').resolve()), close_timeout=5)

    def call(method, params=None, error=None):
        nonlocal total
        row = dict(method=method, params=copy.deepcopy(params or {}))
        try:
            result = client.call(method, params or {}, timeout=remaining())
            row['result'] = copy.deepcopy(result)
            need(error is None, 'Expected native rejection: '+method)
            return result
        except RpcError as exc:
            row['error'] = dict(code=exc.code, message=str(exc), data=exc.data)
            need(error == exc.code, 'Unexpected native error: '+str(exc))
            return row['error']
        finally:
            encoded = json.dumps(row, allow_nan=False).encode()
            total += len(encoded)
            need(len(encoded) <= 1024*1024 and total <= 16*1024*1024, 'RPC evidence budget exceeded')
            record['calls'].append(row)

    def close_client():
        nonlocal client
        if client is not None:
            owned = client
            client = None
            owned.close()
            receipt = dict(exit_code=owned.transport.returncode, stderr=owned.transport.stderr_tail,
                           stderr_truncated=owned.transport.stderr_truncated)
            record['owners'].append(receipt)
            need(receipt['exit_code'] == 0 and not receipt['stderr'] and not receipt['stderr_truncated'], 'Native owner not clean')

    revision = 0
    session = new()
    tick = 0

    def tx(ops, error=None):
        nonlocal revision
        result = call('world.transact', dict(request_id=new(), base_revision=revision, ops=ops), error)
        if error is None:
            revision = result['revision']
        return result

    def state(rig=400, **extra):
        params=dict(session_id=session,tick=tick,entity=uid(rig));params.update(extra)
        return call('runtime.root_motion',params)['state']

    def step(count=1, **extra):
        nonlocal tick
        params=dict(request_id=new(),session_id=session,expected_tick=tick,ticks=count)
        if structure:params['expected_structure_revision']=structure
        params.update(extra);result=call('runtime.step',params);tick=result['tick'];return params,result

    def near(actual, expected, tolerance=1e-6):
        need(len(actual)==len(expected) and all(math.isfinite(a) and abs(a-b)<=tolerance for a,b in zip(actual,expected)), 'Independent position differs: '+repr((actual,expected)))

    saves=args.output/'s';generations={};config=0;structure=0
    def saved(slot):
        call('save.write',dict(request_id=new(),configuration_generation=config,slot=slot,expected_generation=generations.get(slot,0),session_id=session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=structure))
        folder=saves/('slot-'+slot);manifest=json.loads((folder/'current.json').read_text());entry=manifest['payload']['current']
        name=entry['file'];need(len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe durable member')
        data=(folder/name).read_bytes();need(len(data)==entry['bytes'] and len(data)<=2*1024*1024 and hashlib.sha256(data).hexdigest()==entry['sha256'],'Durable digest differs')
        generations[slot]=manifest['payload']['generation'];payload=json.loads(data)
        record.setdefault('durable_payloads',{})[slot]=payload;record.setdefault('durable_receipts',{})[slot]=entry
        return payload

    try:
        client=open_client();description=call('world.describe')
        need(description['schema_revision']>=88 and 'runtime.root_motion' in description['methods'] and 'root_motion' in description['components']['AnimationRig']['properties'],'Root-motion discovery absent')
        doc,blob=ribbon();blob=bytearray(blob)
        def accessor(rows,kind):
            while len(blob)%4:blob.append(0)
            start=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+'f'*len(flat),*flat))
            doc['bufferViews'].append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start))
            doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1,componentType=5126,count=len(rows),type=kind))
            return len(doc['accessors'])-1
        times=accessor([(0,),(1,)],'SCALAR');doc['accessors'][times].update(min=[0],max=[1])
        values=accessor([(0,0,0),(1,0,0)],'VEC3')
        doc['animations'].append(dict(name='Original planar one meter loop',samplers=[dict(input=times,output=values,interpolation='LINEAR')],channels=[dict(sampler=0,target=dict(node=2,path='translation'))]))
        doc['buffers'][0]['byteLength']=len(blob);source=args.output/'a.glb';source.write_bytes(glb(doc,bytes(blob)))
        asset=call('asset.import',dict(source=str(source.resolve())))['asset']
        ops=entity(100)+[set_component(100,'CharacterController',dict(radius=.2,height=1.6,speed=4,jump_speed=5,camera=None))]
        for n,position,extent in [(200,(0,-.2,0),(20,.2,20)),(201,(2,1,0),(.1,1,3))]:
            ops+=entity(n,position=position)+[set_component(n,'BoxCollider',dict(half_extents=list(extent),motion='static',mass=1,friction=.5,restitution=0))]
        ops += [dict(op='asset.instantiate',id=uid(400),name='Character root driver',asset=asset),dict(op='entity.reparent',id=uid(400),parent=uid(100),mode='keep_local'),dict(op='asset.instantiate',id=uid(800),name='Extract observer',asset=asset)]
        tx(ops);authored=json.loads((args.output/'w.json').read_text());rig=copy.deepcopy(authored['entities'][uid(400)]['components']['AnimationRig'])
        rig.update(clip=1,time=0,speed=1,loop=True,playing=True,root_motion=dict(node=2,mode='character',actor=uid(100)))
        extract=copy.deepcopy(rig);extract['root_motion']=dict(node=2);tx([set_component(400,'AnimationRig',rig),set_component(800,'AnimationRig',extract),set_component(800,'Transform',transform((-3,0,0)))])
        original=(args.output/'w.json').read_bytes()
        for change in [dict(node=True),dict(node=1.0),dict(node=9999),dict(mode='wrong'),dict(actor=None),dict(actor=uid(200)),dict(actor=uid(800)),dict(enabled=0),dict(yaw=1),dict(unknown=1)]:
            bad=copy.deepcopy(rig);bad['root_motion'].update(change);tx([set_component(400,'AnimationRig',bad)],-32602)
            need((args.output/'w.json').read_bytes()==original,'Rejected profile changed authored bytes')
        bad=copy.deepcopy(extract);bad['root_motion']['actor']=uid(100);tx([set_component(800,'AnimationRig',bad)],-32602)
        duplicate=copy.deepcopy(rig)
        tx([dict(op='entity.reparent',id=uid(800),parent=uid(100),mode='keep_local'),set_component(800,'AnimationRig',duplicate)],-32602)
        layered=copy.deepcopy(rig);layered['layers']=[dict(slot=1,mode='override',clip=None,time=0,speed=1,loop=True,playing=False,weight=0,mask=[dict(node=2,weight=1)])]
        tx([set_component(400,'AnimationRig',layered)],-32602)
        tx([dict(op='entity.reparent',id=uid(400),parent=None,mode='keep_local')],-32602)
        tx([dict(op='component.remove',id=uid(100),type='CharacterController')],-32602)
        # Freeze a complete controller+rig graph for real instance remapping.
        authored=json.loads((args.output/'w.json').read_text());members={}
        for key,value in authored['entities'].items():
            parent=key
            while parent is not None and parent!=uid(100):parent=authored['entities'][parent]['parent']
            if parent==uid(100):members[key]=copy.deepcopy(value)
        tx([dict(op='template.set',id=uid(600),name='Root-motion actor graph',root=uid(100),entities=members)])
        record['checks'].append('strict_frozen_source_actor_graph_and_template_admission')
        call('runtime.start',dict(session_id=session,revision=revision))
        need(state()['from_tick']==state()['to_tick']==0 and state()['enabled'] and not state()['yaw'],'Initial interval not neutral/defaulted')
        need(state(800)['actor'] is None and state(200) is None,'Extract/nonrig shape differs')
        call('runtime.root_motion',dict(session_id=session,tick=1,entity=uid(400)),-32009)
        call('runtime.root_motion',dict(session_id=new(),tick=tick,entity=uid(400)),-32030)
        call('runtime.root_motion',dict(session_id=session,tick=tick,entity=uid(999)),-32004)
        for controls in [[dict(entity=uid(400),enabled=1)],[dict(entity=uid(400),enabled=True,unknown=1)],[dict(entity=uid(400)),dict(entity=uid(400))]]:
            call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,root_motion=controls),-32602)
        call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,root_motion=[dict(entity=uid(400))]*65),-32602)
        p,result=step(root_motion=[dict(entity=uid(400))]);before=copy.deepcopy(state())
        explicit=copy.deepcopy(p);explicit['root_motion'][0]['enabled']=True
        need(call('runtime.step',explicit)['replayed'] and exact(state(),before),'Default retry consumed another interval')
        explicit['root_motion'][0]['enabled']=False;call('runtime.step',explicit,-32010)
        near(state()['local_delta'],[1/60,0,0]);need(state()['from_tick']==0 and state()['to_tick']==1,'Wrong consumed interval')
        near(state(800)['world_applied'],[0,0,0]);near([state(800)['translation_error']],[0])
        for _ in range(3):need(exact(state(),before),'Inspection consumed movement')
        step(59);actor=call('runtime.entity',dict(session_id=session,tick=tick,id=uid(100)))
        need(.98<=actor['world_matrix'][12]<=1.002,'Real capsule did not move one unclipped cycle')
        root=hashlib.sha256(f'poima.instance.v1/{uid(400)}/node/2'.encode()).hexdigest()[:32]
        posed=call('runtime.entity',dict(session_id=session,tick=tick,id=root))
        near([posed['world_matrix'][12]-actor['world_matrix'][12]],[0],1e-5)
        step(60);actor=call('runtime.entity',dict(session_id=session,tick=tick,id=uid(100)))
        need(1.65<actor['world_matrix'][12]<1.73,'Collision capsule penetrated wall or root loop stalled')
        observed=state();need(observed['translation_error']>.005 and observed['world_requested'][0]>.016,'Blocked requested/actual interval not distinguished')
        step(root_motion=[dict(entity=uid(400),enabled=False)],inputs=[dict(entity=uid(100),move=[-1,0])])
        need(not state()['enabled'] and state()['world_requested']==[0,0,0],'Disabled driver still emits motion')
        step(root_motion=[dict(entity=uid(400))]);need(state()['enabled'],'Re-enable failed')
        saves.mkdir();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation']
        baseline=saved('before');stable=copy.deepcopy(state())
        for extra in [dict(inputs=[dict(entity=uid(100),move=[1,0])]),dict(root_motion=[dict(entity=uid(200))])]:
            call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=2,**extra),-32040)
            need(exact(saved('rejected'),baseline) and exact(state(),stable),'Rejected input/control changed whole payload')
        edited=copy.deepcopy(rig);edited['root_motion']['enabled']=False;tx([set_component(400,'AnimationRig',edited)])
        need(exact(state(),stable),'Authored profile changed frozen driver');call('world.undo',dict(request_id=new(),base_revision=revision));revision+=1
        record['checks'].append('physical_character_loop_wall_stripping_extract_retry_disable_and_rollback')
        born=call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=0,spawns=[dict(template_id=uid(600),transform=transform((-6,0,0)))]));structure=1
        mapping=call('runtime.instance',dict(session_id=session,tick=tick,id=born['spawned'][0],expected_structure_revision=structure))
        spawned_rig=mapping['nodes'][uid(400)]
        spawned=call('runtime.root_motion',dict(session_id=session,tick=tick,entity=spawned_rig))['state']
        need(spawned['actor']==mapping['root'] and spawned['from_tick']==spawned['to_tick']==tick,'Instance binding/cursor not remapped at birth')
        checkpoint=saved('checkpoint');snapshot_state=copy.deepcopy(state());step(3);continued=saved('continued');continued_state=copy.deepcopy(state())
        call('runtime.stop',dict(session_id=session));close_client();source.rename(args.output/'retained-a.glb');need(not source.exists(),'Original source still exists');client=open_client();session=new()
        config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation'];tick=snapshot_state['to_tick']
        loaded=call('save.load',dict(request_id=new(),configuration_generation=config,slot='checkpoint',expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=session))
        need(loaded['tick']==tick and exact(state(),snapshot_state),'Fresh interval restore differs')
        need(exact(saved('restored'),checkpoint),'Fresh complete checkpoint differs');step(3)
        need(exact(state(),continued_state) and exact(saved('fresh'),continued),'Fresh real continuation differs')
        call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=structure,despawns=[mapping['root']]));structure=2
        call('runtime.root_motion',dict(session_id=session,tick=tick,entity=spawned_rig),-32004)
        record['checks'].append('instance_reference_rebind_full_save_source_absent_fresh_continuation_and_retirement')
        call('runtime.stop',dict(session_id=session));close_client();record['passed']=True
    except BaseException as exc:
        record['error']=repr(exc)
    finally:
        try: close_client()
        except BaseException as exc: record['cleanup_error']=repr(exc);record['passed']=False
        record.update(source_pins=source_pins,source_unchanged=all(hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==digest for name,digest in source_pins.items()),binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
        record['passed']=record['passed'] and record['source_unchanged']
        if time.monotonic()>=deadline:record['passed']=False;record['deadline_exhausted']=True
        data=(json.dumps(record,indent=2,allow_nan=False)+'\n').encode()
        need(len(data)<=32*1024*1024 and len(data)+sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())<=64*1024*1024,'Final evidence/output budget exceeded')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic()>=deadline:
            record['passed']=False;record['deadline_exhausted']=True
            (args.output/'evidence.json').write_text(json.dumps(record,indent=2,allow_nan=False)+'\n')
    print(json.dumps(dict(passed=record['passed'],checks=record['checks'],calls=len(record['calls']))))
    return 0 if record['passed'] else 1

if __name__=='__main__':sys.exit(main())
