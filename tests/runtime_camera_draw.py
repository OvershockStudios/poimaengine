#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless CameraDraw authoring/frozen-save checks; externally gate native execution.

Run matching-platform Python with --binary ENGINE --output NEW_DIRECTORY.
This fixture validates metadata and ownership; it makes no rendered-pixel claim.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError
from poima_client.transport import JsonRpcProcess


def need(value, message):
    if not value:
        raise AssertionError(message)


def uid(n): return f'{n:032x}'
def new(): return uuid.uuid4().hex
def transform(): return dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
def component(n, kind, value): return dict(op='component.set', id=uid(n), type=kind, value=value)
def entity(n, parent=None):
    return [dict(op='entity.create', id=uid(n), name=f'Object {n}', parent=parent),
            component(n, 'Transform', transform())]
def mesh(visible=True): return dict(primitive='box', albedo=[.5]*3, visible=visible)
def lens(fov=75): return dict(vertical_fov=fov, near=.01, far=100)
def camera(foreground=True):
    result = dict(vertical_fov=60, near=.1, far=100)
    if foreground: result['viewmodel'] = lens()
    return result
def draw(n=100, mode='viewmodel'): return dict(camera=uid(n), mode=mode)
def material(color=.5):
    return dict(base_color=[color]*3, emissive=[0]*3, metallic=0, roughness=.5, double_sided=False)
def exact(a, b):
    return json.dumps(a, sort_keys=True, separators=(',', ':'), allow_nan=False) == json.dumps(b, sort_keys=True, separators=(',', ':'), allow_nan=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    need(math.isfinite(args.timeout) and 0 < args.timeout <= 180, 'Timeout must be finite and at most 180 seconds')
    need(not args.output.exists(), 'Fresh output required')
    args.output.mkdir(parents=True)
    deadline = time.monotonic() + args.timeout
    record = dict(passed=False, calls=[], transport=[], owners=[], checks=[], durable_payloads={}, durable_receipts={})
    files = [Path(__file__).resolve(), *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
             *sorted((ROOT/'tools/python/poima_client').glob('*.json'))]
    pins = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    binary_hash = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    client = None
    total = 0
    raw_bytes = 0
    raw_lock = threading.Lock()
    owner_index = 0
    revision = 0
    session = new()
    tick = 0
    structure = 0
    config = 0
    saves = args.output/'s'
    saves.mkdir()

    def remaining():
        value = deadline-time.monotonic()
        need(value > 0, 'Whole deadline exhausted')
        return min(15, value)

    def raw(row):
        nonlocal raw_bytes
        data = json.dumps(row, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()
        with raw_lock:
            need(len(record['transport']) < 1000 and raw_bytes+len(data) <= 16*1024*1024, 'Raw RPC evidence budget exceeded')
            raw_bytes += len(data)
            record['transport'].append(row)

    def open_client():
        nonlocal owner_index
        owner_index += 1
        ordinal = owner_index
        class RecordedTransport(JsonRpcProcess):
            def _accept_response(self, response):
                raw(dict(owner=ordinal, direction='response', message=copy.deepcopy(response)))
                super()._accept_response(response)
        transport = RecordedTransport([str(args.binary.resolve()), 'world', str((args.output/'w.json').resolve())], cwd=ROOT, close_timeout=5)
        request = transport.request
        def logged(method, params=None, timeout=15):
            with transport._condition:
                identifier = transport._next_id
            raw(dict(owner=ordinal, direction='request', message=dict(jsonrpc='2.0', id=identifier,
                method=method, params=copy.deepcopy(params or {}))))
            return request(method, params, timeout=min(timeout, remaining()))
        transport.request = logged
        return WorldClient(transport)

    def call(method, params=None, error=None):
        nonlocal total
        with client.transport._condition:
            identifier = client.transport._next_id
        row = dict(owner=owner_index, rpc_id=identifier, method=method, params=copy.deepcopy(params or {}))
        try:
            result = client.call(method, params or {}, timeout=remaining())
            row['result'] = copy.deepcopy(result)
            need(error is None, 'Expected native rejection: '+method)
            return result
        except RpcError as exc:
            row['error'] = dict(code=exc.code, message=str(exc), data=copy.deepcopy(exc.data))
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
            owned, client = client, None
            owned.close()
            receipt = dict(owner=owner_index, exit_code=owned.transport.returncode, stderr=owned.transport.stderr_tail,
                           stderr_truncated=owned.transport.stderr_truncated)
            record['owners'].append(receipt)
            need(receipt['exit_code'] == 0 and not receipt['stderr'] and not receipt['stderr_truncated'], 'Native owner not clean')

    def tx(ops, error=None):
        nonlocal revision
        world_path = args.output/'w.json'
        before = world_path.read_bytes() if world_path.exists() else None
        result = call('world.transact', dict(request_id=new(), base_revision=revision, ops=ops), error)
        if error is None: revision = result['revision']
        else: need((world_path.read_bytes() if world_path.exists() else None) == before, 'Rejected transaction changed authored bytes')
        return result

    def saved(slot, epoch=0):
        result = call('save.write', dict(request_id=new(), configuration_generation=config, slot=slot,
            expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0,
            expected_structure_revision=structure, expected_material_revision=epoch))
        folder = saves/('slot-'+slot)
        manifest = json.loads((folder/'current.json').read_text())
        row = manifest['payload']['current']
        name = row['file']
        need(type(name) is str and len(name) == 38 and name.startswith('p-') and name.endswith('.bin')
             and all(c in '0123456789abcdef' for c in name[2:-4]), 'Unsafe durable member')
        data = (folder/name).read_bytes()
        need(len(data) == row['bytes'] and len(data) <= 1024*1024 and hashlib.sha256(data).hexdigest() == row['sha256']
             and result['sha256'] == row['sha256'], 'Durable receipt mismatch')
        value = json.loads(data)
        need(value['format'] == 'poima.world-save' and value['version'] == 1, 'Wrong save envelope')
        record['durable_payloads'][slot], record['durable_receipts'][slot] = value, row
        return value

    try:
        client = open_client()
        description = call('world.describe')
        need(description['schema_revision'] >= 90 and 'CameraDraw' in description['components'], 'Discovery absent')
        ops = entity(100)+[component(100, 'Camera', camera())]
        ops += entity(101, uid(100))+[component(101, 'MeshRenderer', mesh()), component(101, 'CameraDraw', draw()), component(101, 'PbrMaterial', material())]
        ops += entity(102)+[component(102, 'MeshRenderer', mesh()), component(102, 'CameraDraw', draw(mode='hidden'))]
        ops += entity(200)+[component(200, 'Camera', camera(False))]
        ops += entity(201)+[component(201, 'MeshRenderer', mesh()), component(201, 'CameraDraw', draw(200, 'hidden'))]
        ops += entity(202)+[component(202, 'MeshRenderer', mesh())]
        members = {uid(300):dict(name='Local camera', parent=None, components=dict(Transform=transform(), Camera=camera())),
            uid(301):dict(name='Local arms', parent=uid(300), components=dict(Transform=transform(), MeshRenderer=mesh(), CameraDraw=draw(300))),
            uid(302):dict(name='Local body', parent=uid(300), components=dict(Transform=transform(), MeshRenderer=mesh(), CameraDraw=draw(300, 'hidden')))}
        ops += [dict(op='template.set', id=uid(600), name='Camera-local presentation', root=uid(300), entities=members)]
        tx(ops)
        original = json.loads((args.output/'w.json').read_text())
        need('viewmodel' not in original['entities'][uid(200)]['components']['Camera'] and
             'CameraDraw' not in original['entities'][uid(202)]['components'], 'Omission changed authored data')
        need(call('entity.query', dict(revision=revision, component='CameraDraw', limit=256))['entities'], 'CameraDraw filter unavailable')
        for value in [None, {}, dict(lens(), surprise=0), dict(lens(), near=True), dict(lens(), vertical_fov=4), dict(lens(), far=.001)]:
            tx([component(100, 'Camera', dict(camera(), viewmodel=value))], -32602)
        for value in [{}, dict(draw(), camera=uid(0)), dict(draw(), camera=uid(999)), dict(draw(), mode='Viewmodel'), dict(draw(), extra=1)]:
            tx([component(101, 'CameraDraw', value)], -32602)
        tx([component(100, 'Camera', camera(False))], -32602)
        tx([dict(op='entity.reparent', id=uid(101), parent=None, mode='keep_local')], -32602)
        tx([dict(op='component.remove', id=uid(101), type='MeshRenderer')], -32602)
        tx([dict(op='component.remove', id=uid(100), type='Camera')], -32602)
        tx([dict(op='entity.delete', id=uid(100), recursive=True)], -32602)  # Surviving external hidden binding.
        bad_members = copy.deepcopy(members); bad_members[uid(301)]['components']['CameraDraw'] = draw()
        tx([dict(op='template.set', id=uid(601), name='External reference', root=uid(300), entities=bad_members)], -32602)
        tx([dict(op='template.set', id=uid(601), name='Legacy reference', components=dict(Transform=transform(), MeshRenderer=mesh(), CameraDraw=draw()))], -32602)
        record['checks'].append('strict_lens_reference_graph_omission_and_atomic_rollback')

        # Already one of each mode on these cameras: 31 hidden/invisible extras reaches exactly 32.
        extras = list(range(1000, 1031))+list(range(1100, 1131))
        ops = []
        for n in extras:
            foreground = n < 1100
            ops += entity(n, uid(100) if foreground else None)+[component(n, 'MeshRenderer', mesh(False)),
                component(n, 'CameraDraw', draw(100 if foreground else 200, 'viewmodel' if foreground else 'hidden'))]
        tx(ops)
        for n, parent, binding in [(1031, uid(100), draw()), (1131, None, draw(200, 'hidden'))]:
            tx(entity(n, parent)+[component(n, 'MeshRenderer', mesh(False)), component(n, 'CameraDraw', binding)], -32602)
        tx([dict(op='entity.delete', id=uid(n), recursive=False) for n in extras])
        record['checks'].append('both_32_invisible_limits')

        call('runtime.start', dict(session_id=session, revision=revision))
        config = call('save.configure', dict(request_id=new(), expected_generation=0, root=str(saves.resolve())))['generation']
        # Authored edits are legal but must not alter the live frozen save baseline.
        tx([component(100, 'Camera', dict(camera(), viewmodel=lens(95))), component(101, 'CameraDraw', draw(mode='hidden'))])
        frozen = saved('frozen')
        need(exact(frozen['document']['entities'][uid(100)]['components']['Camera'], camera()) and
             exact(frozen['document']['entities'][uid(101)]['components']['CameraDraw'], draw()), 'Authored edit changed frozen camera metadata')
        call('world.undo', dict(request_id=new(), base_revision=revision)); revision += 1
        before_material = call('runtime.material.inspect', dict(session_id=session, tick=tick, ids=[uid(101)]))
        tx([component(101, 'PbrMaterial', material(.25))])
        adopted = call('runtime.material.apply', dict(request_id=new(), session_id=session, expected_tick=tick,
            expected_structure_revision=0, expected_material_revision=0, revision=revision,
            expected_authored_revision=before_material['authored_revision'], expected_content_sha256=before_material['content_sha256'], ids=[uid(101)]))
        need(adopted['material_revision'] == 1, 'Material adoption did not advance shared epoch')
        born = call('runtime.structure.transact', dict(session_id=session, request_id=new(), expected_tick=tick,
            expected_structure_revision=0, spawns=[dict(template_id=uid(600))]))
        structure = born['structure_revision']
        mapping = call('runtime.instance', dict(session_id=session, tick=tick, id=born['spawned'][0], expected_structure_revision=structure))
        need(len(mapping['nodes']) == 3 and mapping['nodes'][uid(300)] == mapping['root'] and
             len(set(mapping['nodes'].values())) == 3 and all(v not in members for v in mapping['nodes'].values()), 'Template remapping invalid')
        for identity in mapping['nodes'].values():
            need(call('runtime.entity', dict(session_id=session, tick=tick, structure_revision=structure, id=identity))['id'] == identity, 'Born template member missing')
        call('runtime.step', dict(request_id=new(), session_id=session, expected_tick=tick, expected_structure_revision=structure, ticks=2)); tick = 2
        baseline = saved('adopted', 1)
        need(exact(baseline['document']['entities'][uid(100)]['components']['Camera'], camera()) and
             exact(baseline['document']['entities'][uid(101)]['components']['CameraDraw'], draw()) and
             exact(baseline['document']['templates'][uid(600)]['entities'], members), 'Adoption or birth rewrote frozen camera metadata')
        record['checks'].append('frozen_metadata_live_material_and_template_birth')

        call('runtime.stop', dict(session_id=session)); close_client()
        client = open_client()
        config = call('save.configure', dict(request_id=new(), expected_generation=0, root=str(saves.resolve())))['generation']
        session = new()
        loaded = call('save.load', dict(request_id=new(), configuration_generation=config, slot='adopted', expected_generation=1,
            revision=revision, expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
            expected_structure_revision=None, new_session_id=session))
        need(loaded['tick'] == tick, 'Fresh load changed tick')
        fresh_epoch = call('runtime.material.inspect', dict(session_id=session, tick=tick, ids=[uid(101)]))['material_revision']
        need(exact(saved('restored', fresh_epoch), baseline), 'Fresh save restoration changed complete native payload')
        call('runtime.structure.transact', dict(session_id=session, request_id=new(), expected_tick=tick,
            expected_structure_revision=structure, despawns=[mapping['root']]))
        structure += 1
        call('runtime.entity', dict(session_id=session, tick=tick, structure_revision=structure, id=mapping['nodes'][uid(301)]), -32004)
        record['checks'].append('fresh_complete_save_restore_and_whole_instance_removal')
        call('runtime.stop', dict(session_id=session)); close_client()
        record['passed'] = True
    except BaseException as exc:
        record['error'] = repr(exc)
    finally:
        try: close_client()
        except BaseException as exc: record['cleanup_error'] = repr(exc); record['passed'] = False
        record.update(raw_bytes=raw_bytes, source_pins=pins, source_unchanged=all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest() == h for n, h in pins.items()),
                      binary_sha256=binary_hash, binary_unchanged=hashlib.sha256(args.binary.read_bytes()).hexdigest() == binary_hash)
        record['passed'] = record['passed'] and record['source_unchanged'] and record['binary_unchanged']
        if time.monotonic() >= deadline: record['passed'] = False; record['deadline_exhausted'] = True
        data = (json.dumps(record, indent=2, allow_nan=False)+'\n').encode()
        need(len(data) <= 32*1024*1024 and len(data)+sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file()) <= 64*1024*1024, 'Final evidence/output budget exceeded')
        (args.output/'evidence.json').write_bytes(data)
        if time.monotonic() >= deadline:
            record['passed'] = False; record['deadline_exhausted'] = True
            (args.output/'evidence.json').write_text(json.dumps(record, indent=2, allow_nan=False)+'\n')
    print(json.dumps(dict(passed=record['passed'], checks=record['checks'], calls=len(record['calls']))))
    return 0 if record['passed'] else 1


if __name__ == '__main__': sys.exit(main())
