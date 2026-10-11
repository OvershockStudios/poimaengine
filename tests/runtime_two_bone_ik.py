#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless native two-bone IK protocol checks; an external owner must gate execution.

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

    def state(rig=400, slot=1, **extra):
        params=dict(session_id=session,tick=tick,entity=uid(rig),slot=slot);params.update(extra)
        return call('runtime.ik',params)['state']

    def near(actual, expected, tolerance=1e-6):
        need(len(actual)==len(expected) and all(math.isfinite(a) and abs(a-b)<=tolerance for a,b in zip(actual,expected)), 'Independent position differs: '+repr((actual,expected)))

    def command(**extra):
        result=dict(entity=uid(400),slot=1,target=[1,1,0],pole=[0,0,1]);result.update(extra);return result

    def step(count=1, **extra):
        nonlocal tick
        params=dict(request_id=new(),session_id=session,expected_tick=tick,ticks=count);params.update(extra)
        result=call('runtime.step',params);tick=result['tick'];return params,result

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
        need(description['schema_revision']>=87 and 'runtime.ik' in description['methods'] and 'ik' in description['components']['AnimationRig']['properties'],'IK discovery absent')
        doc,blob=ribbon();doc['nodes'][1]['children']=[3];doc['nodes'].append(dict(name='Effector',translation=[0,1,0]))
        # Explicit STEP scale becomes unsupported on the second advanced Tick.
        blob=bytearray(blob)
        def accessor(rows,kind):
            while len(blob)%4:blob.append(0)
            start=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+'f'*len(flat),*flat))
            doc['bufferViews'].append(dict(buffer=0,byteOffset=start,byteLength=len(blob)-start))
            doc['accessors'].append(dict(bufferView=len(doc['bufferViews'])-1,componentType=5126,count=len(rows),type=kind))
            return len(doc['accessors'])-1
        times=accessor([(0,),(1/64,),(1/32,)],'SCALAR');doc['accessors'][times].update(min=[0],max=[1/32])
        values=accessor([(1,1,1),(1,1,1),(2,1,1)],'VEC3')
        doc['animations'].append(dict(name='Unsupported evaluated scale',samplers=[dict(input=times,output=values,interpolation='STEP')],channels=[dict(sampler=0,target=dict(node=2,path='scale'))]))
        doc['buffers'][0]['byteLength']=len(blob)
        source=args.output/'a.glb';source.write_bytes(glb(doc,bytes(blob)))
        asset=call('asset.import',dict(source=str(source.resolve())))['asset']
        tx(entity(100)+[set_component(100,'BoxCollider',dict(half_extents=[.2]*3,motion='kinematic',mass=1,friction=.5,restitution=0)),dict(op='asset.instantiate',id=uid(400),name='Three-joint IK',asset=asset),dict(op='entity.reparent',id=uid(400),parent=uid(100),mode='keep_local')])
        authored=json.loads((args.output/'w.json').read_text());rig=authored['entities'][uid(400)]['components']['AnimationRig']
        rig.update(clip=None,playing=False,ik=[dict(slot=1,root=2,middle=1,end=3,target=[1,1,0],pole=[0,0,1])])
        tx([set_component(400,'AnimationRig',rig)])
        original=(args.output/'w.json').read_bytes()
        for change in [dict(slot=True),dict(slot=9),dict(root=1),dict(end=0),dict(root=9999),dict(enabled=1),dict(weight=True),dict(space='local'),dict(target=[0,0]),dict(pole=[0,0,1000001]),dict(unknown=1)]:
            bad=copy.deepcopy(rig);bad['ik'][0].update(change);tx([set_component(400,'AnimationRig',bad)],-32602)
            need((args.output/'w.json').read_bytes()==original,'Rejected IK authoring changed bytes')
        bad=copy.deepcopy(rig);bad['ik'].append(dict(bad['ik'][0],slot=2));tx([set_component(400,'AnimationRig',bad)],-32602)
        middle=hashlib.sha256(f'poima.instance.v1/{uid(400)}/node/1'.encode()).hexdigest()[:32]
        end=hashlib.sha256(f'poima.instance.v1/{uid(400)}/node/3'.encode()).hexdigest()[:32]
        tx([dict(op='entity.reparent',id=end,parent=uid(400),mode='keep_local')],-32602)
        tx([dict(op='entity.delete',id=end,recursive=True)],-32602)
        need((args.output/'w.json').read_bytes()==original,'Rejected chain graph changed bytes')
        tx([dict(op='component.set',id=end,type='HitRegion',value=dict(owner=uid(400),name='effector',shape='sphere',radius=.1))])
        authored=json.loads((args.output/'w.json').read_text())
        # A complete local rig template copies model indices, remapping its IDs.
        members={}
        for key,value in authored['entities'].items():
            parent=key
            while parent is not None and parent!=uid(400):parent=authored['entities'][parent]['parent']
            if parent==uid(400):members[key]=copy.deepcopy(value)
        members[uid(400)]['parent']=None;members[uid(400)]['components']['AnimationRig']=copy.deepcopy(rig)
        tx([dict(op='template.set',id=uid(600),name='IK graph',root=uid(400),entities=members)])
        record['checks'].append('strict_frozen_chain_graph_and_template_admission')
        call('runtime.start',dict(session_id=session,revision=revision));near(state()['end_position'],[1,1,0]);near([state()['target_error']],[0])
        need(state()['root']==2 and state()['middle']==1 and state()['end']==3 and not state()['clamped'],'Frozen chain identity wrong')
        need(state(100) is None and state(slot=8) is None,'Absent slot/nonrig inspection not null')
        call('runtime.ik',dict(session_id=session,tick=1,entity=uid(400),slot=1),-32009)
        call('runtime.ik',dict(session_id=new(),tick=tick,entity=uid(400),slot=1),-32030)
        call('runtime.ik',dict(session_id=session,tick=tick,entity=uid(999),slot=1),-32004)
        for changes in [dict(enabled=0),dict(slot=1.0),dict(weight=True),dict(space='local'),dict(target=[0,0]),dict(unknown=1)]:
            call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,ik=[command(**changes)]),-32602)
        call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,ik=[command(),command()]),-32602)
        p,result=step(ik=[command()]);before=copy.deepcopy(state())
        explicit=copy.deepcopy(p);explicit['ik'][0].update(enabled=True,space='world',weight=1)
        need(call('runtime.step',explicit)['replayed'] and exact(state(),before),'Default-normalized retry mutated state')
        explicit['ik'][0]['target']=[0,1,0];call('runtime.step',explicit,-32010)
        step(2,motions=[dict(entity=uid(100),position=[.25,0,0],rotation=[0,0,0,1],duration_ticks=2)])
        near(state()['end_position'],[1,1,0]);near([state()['target_error']],[0])
        step(ik=[command(space='rig')]);near(state()['end_position'],[1.25,1,0])
        step(ik=[command(space='rig',target=[4,0,0])]);near(state()['end_position'],[2.25,0,0]);need(state()['clamped'],'Unreachable target not clamped')
        step(ik=[command(enabled=False)]);near(state()['end_position'],[.25,2,0])
        step(ik=[command(weight=0)]);near(state()['end_position'],[.25,2,0])
        step(ik=[command()]);near(state()['end_position'],[1,1,0])
        tip=call('runtime.entity',dict(session_id=session,tick=tick,id=end))['world_matrix'];near(tip[12:15],[1,1,0])
        hit=call('runtime.hit_test',dict(session_id=session,tick=tick,origin=[1,1,2],direction=[0,0,-1],distance=10))['hit']
        need(hit['entity']==uid(400) and hit['region_entity']==end and hit['region']=='effector','HitRegion did not follow IK effector')
        near([hit['distance']],[1.9])
        before=copy.deepcopy(state());edited=copy.deepcopy(rig);edited['ik'][0]['target']=[0,1,0]
        tx([set_component(400,'AnimationRig',edited)]);need(exact(state(),before),'Authored edit changed frozen IK')
        call('world.undo',dict(request_id=new(),base_revision=revision));revision+=1
        record['checks'].append('committed_geometry_world_motion_rig_space_defaults_retries_and_frozen_edits')
        saves.mkdir();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation']
        original_save=saved('before')
        failed=dict(request_id=new(),session_id=session,expected_tick=tick,ticks=3,animations=[dict(entity=uid(400),clip=1,time=0,speed=1,loop=False,playing=True)])
        call('runtime.step',failed,-32040)
        need(call('runtime.inspect',dict(session_id=session))['tick']==tick and exact(state(),before),'Later Tick failure changed IK/tick')
        need(exact(saved('after'),original_save),'Later Tick failure changed complete native payload')
        bad=dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,ik=[command(slot=8)])
        call('runtime.step',bad,-32040);need(exact(saved('invalid'),original_save),'Invalid live slot changed complete payload')
        born=call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=0,spawns=[dict(template_id=uid(600))]));structure=1
        spawned=born['spawned'][0];mapping=call('runtime.instance',dict(session_id=session,tick=tick,id=spawned,expected_structure_revision=1))
        spawned_state=call('runtime.ik',dict(session_id=session,tick=tick,entity=mapping['root'],slot=1))['state'];near(spawned_state['end_position'],[1,1,0])
        checkpoint=saved('checkpoint');checkpoint_state=copy.deepcopy(state())
        source.unlink();call('runtime.stop',dict(session_id=session));close_client();client=open_client()
        config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation'];session=new()
        loaded=call('save.load',dict(request_id=new(),configuration_generation=config,slot='checkpoint',expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=session))
        need(loaded['tick']==tick and exact(state(),checkpoint_state),'Fresh restored IK differs')
        need(exact(saved('restored'),checkpoint),'Fresh restore changed complete payload')
        step(2,expected_structure_revision=structure);near(state()['end_position'],[1,1,0]);near([state()['target_error']],[0])
        call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=1,despawns=[mapping['root']]));structure=2
        call('runtime.ik',dict(session_id=session,tick=tick,entity=mapping['root'],slot=1),-32004)
        record['checks'].append('whole_batch_rollback_instance_defaults_fresh_restore_and_retirement')
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
