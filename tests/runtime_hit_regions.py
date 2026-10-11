#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless native HitRegion protocol checks; an external owner must gate execution.

Uses original analytic geometry/animation, no renderer or managed compiler.
Run matching-platform Python with --binary ENGINE --output NEW_DIRECTORY.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
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

    def query(x=0, method='runtime.hit_test', **extra):
        params = dict(session_id=session, tick=tick, origin=[x, 2, 2], direction=[0, 0, -1], distance=10)
        params.update(extra)
        return call(method, params)['hit']

    try:
        client = open_client()
        description = call('world.describe')
        need(description['schema_revision'] >= 86 and 'HitRegion' in description['components'] and 'runtime.hit_test' in description['methods'], 'Discovery absent')
        doc, blob = ribbon()
        source = args.output/'a.glb'; source.write_bytes(glb(doc, blob))
        asset = call('asset.import', dict(source=str(source.resolve())))['asset']
        ops = entity(100) + [set_component(100, 'BoxCollider', dict(half_extents=[1, 3, 1], motion='static', mass=1, friction=.5, restitution=0))]
        ops += entity(101, uid(100), (0, 2, 0)) + [set_component(101, 'HitRegion', sphere())]
        ops += entity(102, uid(100), (2, 2, 0)) + [set_component(102, 'HitRegion', dict(owner=uid(100), name='chest', shape='box', half_extents=[.25]*3))]
        ops += entity(103, uid(100), (-2, 2, 0)) + [set_component(103, 'HitRegion', dict(owner=uid(100), name='leg', shape='capsule', radius=.25, half_height=.5))]
        ops += entity(200, position=(0, 2, -3)) + [set_component(200, 'BoxCollider', dict(half_extents=[.5, .5, .1], motion='static', mass=1, friction=.5, restitution=0))]
        ops += [dict(op='asset.instantiate', id=uid(400), name='Analytic animated rig', asset=asset), set_component(400, 'Transform', transform((10, 0, 0)))]
        tip = hashlib.sha256(f'poima.instance.v1/{uid(400)}/node/1'.encode()).hexdigest()[:32]
        ops += [dict(op='component.set', id=tip, type='HitRegion', value=sphere(400, 'tip'))]
        local_root, local_region = uid(300), uid(301)
        members = {local_root:dict(name='Root', parent=None, components=dict(Transform=transform((6, 0, 0)))),
                   local_region:dict(name='Region', parent=local_root, components=dict(Transform=transform((0, 2, 0)), HitRegion=sphere(300, 'head')))}
        ops += [dict(op='template.set', id=uid(600), name='Mapped region', root=local_root, entities=members)]
        tx(ops)
        original = (args.output/'w.json').read_bytes()
        bad = [dict(sphere(), radius=True), dict(sphere(), enabled=1), dict(sphere(), center=[0, 0]),
               dict(sphere(), name='Head'), dict(sphere(), owner=uid(200)), dict(sphere(), shape='box'),
               dict(sphere(), half_extents=[1, 1, 1]), dict(sphere(), radius=0), dict(sphere(), surprise=1)]
        for value in bad:
            tx([set_component(101, 'HitRegion', value)], -32602)
            need((args.output/'w.json').read_bytes() == original, 'Rejected component changed authored bytes')
        tx([set_component(102, 'HitRegion', dict(sphere(), enabled=False))], -32602)
        tx([dict(op='entity.reparent', id=uid(101), parent=uid(200))], -32602)
        tx([dict(op='entity.reparent', id=uid(101), parent=uid(200)), dict(op='entity.delete', id=uid(100), recursive=True)], -32602)
        tx([dict(op='template.set',id=uid(601),name='Legacy unsupported',components=dict(Transform=transform(),HitRegion=sphere(601)))],-32602)
        bad_members=copy.deepcopy(members);bad_members[local_region]['components']['HitRegion']['owner']=uid(100)
        tx([dict(op='template.set',id=uid(601),name='External owner',root=local_root,entities=bad_members)],-32602)
        need((args.output/'w.json').read_bytes() == original, 'Rejected graph edit changed authored bytes')
        record['checks'].append('strict_shape_and_atomic_owner_graph')
        call('runtime.start', dict(session_id=session, revision=revision))
        for x, attachment, name in [(0, 101, 'head'), (2, 102, 'chest'), (-2, 103, 'leg')]:
            hit = query(x)
            need(hit['entity'] == uid(100) and hit['region_entity'] == uid(attachment) and hit['region'] == name and abs(hit['distance']-1.75) < 1e-6 and hit['triangle'] is None, 'Region intersection/owner wrong')
        old = query(method='runtime.raycast')
        need(old['entity'] == uid(100) and 'region' not in old and old['distance'] < 1.1, 'Legacy proxy ray changed')
        for ignored in [uid(100), uid(101)]:
            hit = query(ignore=[ignored]); need(hit['entity'] == uid(200) and hit['region'] is None and hit['region_entity'] is None, 'Ignore/proxy/body result wrong')
        inside = call('runtime.hit_test', dict(session_id=session, tick=tick, origin=[0, 2, 0], direction=[0, 0, -1], distance=1))['hit']
        need(inside['distance'] == 0 and inside['normal'] is None, 'Inside normal contract wrong')
        call('runtime.hit_test', dict(session_id=session, tick=1, origin=[0,2,2], direction=[0,0,-1], distance=10), -32009)
        for extra in [dict(ignore=[uid(999)]), dict(ignore=[uid(100),uid(100)]), dict(direction=[0,0,0]), dict(distance=True), dict(unknown=0)]:
            params=dict(session_id=session,tick=tick,origin=[0,2,2],direction=[0,0,-1],distance=10);params.update(extra)
            call('runtime.hit_test',params,-32602)
        before = query()
        tx([set_component(101, 'HitRegion', dict(sphere(), radius=.5))])
        need(exact(query(), before), 'Authored edit changed frozen live region')
        call('world.undo', dict(request_id=new(), base_revision=revision));revision += 1
        record['checks'].append('three_shapes_proxy_ignore_inside_stale_and_frozen')
        call('runtime.stop',dict(session_id=session));session=new()
        call('runtime.start',dict(session_id=session,revision=revision))
        call('runtime.step', dict(request_id=new(), session_id=session, expected_tick=tick, ticks=60,
             animations=[dict(entity=uid(400), clip=0, time=0, speed=1, loop=False, playing=True)]))
        tick = 60
        pose = call('runtime.entity', dict(session_id=session,id=tip,tick=tick))['world_matrix']
        animated = call('runtime.hit_test',dict(session_id=session,tick=tick,origin=[pose[12],pose[13],2],direction=[0,0,-1],distance=10))['hit']
        need(animated['region_entity']==tip and animated['entity']==uid(400) and abs(pose[12]-11)<1e-5, 'Region did not follow committed bone pose')
        call('runtime.step',dict(request_id=new(),session_id=session,expected_tick=tick,ticks=1,animations=[dict(entity=uid(100),clip=0,time=0,speed=1,loop=False,playing=True)]),-32040)
        need(call('runtime.inspect',dict(session_id=session))['tick']==tick and exact(query(),before), 'Rejected step changed pose/query')
        born=call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=0,spawns=[dict(template_id=uid(600))]))
        spawned=born['spawned'][0]
        mapping=call('runtime.instance',dict(session_id=session,tick=tick,id=spawned,expected_structure_revision=1))
        hit=query(6);need(hit['entity']==mapping['root'] and hit['region_entity']==mapping['nodes'][local_region], 'Template owner was not remapped')
        record['checks'].append('committed_animation_failed_step_and_template_remap')
        saves=args.output/'s';saves.mkdir()
        config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation']
        saved_hit=copy.deepcopy(hit)
        call('save.write',dict(request_id=new(),configuration_generation=config,slot='checkpoint',expected_generation=0,session_id=session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=1))
        def durable(slot):
            folder=saves/('slot-'+slot)
            manifest=json.loads((folder/'current.json').read_text())
            entry=manifest['payload']['current'];name=entry['file']
            need(type(name) is str and len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe durable payload member')
            data=(folder/name).read_bytes()
            need(len(data)==entry['bytes'] and len(data)<=1024*1024 and hashlib.sha256(data).hexdigest()==entry['sha256'],'Durable payload hash/size mismatch')
            result=json.loads(data);need(result['format']=='poima.world-save' and result['version']==1,'Wrong native save envelope')
            record.setdefault('durable_payloads',{})[slot]=result
            record.setdefault('durable_receipts',{})[slot]=entry
            return result
        original_save=durable('checkpoint')

        call('runtime.stop',dict(session_id=session));close_client()
        client=open_client();config=call('save.configure',dict(request_id=new(),expected_generation=0,root=str(saves.resolve())))['generation']
        session=new()
        loaded=call('save.load',dict(request_id=new(),configuration_generation=config,slot='checkpoint',expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=session))
        need(loaded['tick']==tick and exact(query(6),saved_hit), 'Fresh save restore changed region/owner')
        call('save.write',dict(request_id=new(),configuration_generation=config,slot='restored',expected_generation=0,session_id=session,expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=1))
        need(exact(durable('restored'),original_save),'Fresh restore changed complete native save payload')
        retired=mapping['root'];call('runtime.structure.transact',dict(session_id=session,request_id=new(),expected_tick=tick,expected_structure_revision=1,despawns=[retired]))
        need(query(6) is None, 'Retired template region remained queryable')
        record['checks'].append('fresh_restore_and_retired_query_membership')
        call('runtime.stop',dict(session_id=session));close_client()
        record['passed']=True
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
