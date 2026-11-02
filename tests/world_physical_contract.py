#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native CLI PhysicalRig authoring, committed projections, transactions and saves.

Uses the existing original analytic ribbon recipe through real import/instantiate.
Builds nothing and opens no window. Retains exact request/reply lines, authored
worlds, imported sources/packages and complete save archives. Requires an external
process-tree owner. Source alone is not evidence that these checks have passed.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import queue
import re
import subprocess
import threading
import time
import uuid
from animation_fixture import ribbon
from gltf_fixture import glb

ROOT = Path(__file__).resolve().parents[1]
ARGS = None
REPORT = {'format': 'poima.world-physical-contract.v1', 'groups': [], 'owners': [], 'archives': [], 'calls': []}
DEADLINE = 0.0
MAX_CALLS, MAX_LINE, MAX_RECORDED = 300, 2*1024*1024, 16*1024*1024
RIG, FLOOR, SESSION = (f'{n:032x}' for n in (100, 10, 900))
PART_FIELDS = {'name', 'node', 'node_entity', 'body_id', 'position', 'rotation', 'linear_velocity', 'angular_velocity',
               'mass', 'inertia', 'enrolled', 'active', 'sleeping', 'max_linear_velocity', 'max_angular_velocity',
               'linear_damping', 'angular_damping', 'friction', 'restitution', 'at_linear_velocity_limit', 'at_angular_velocity_limit'}
JOINT_FIELDS = {'name', 'parent', 'child', 'hinge', 'relative_rotation', 'relative_angular_velocity',
                'evaluated_passive_child_torque', 'twist_angle', 'swing_y_angle', 'swing_z_angle',
                'twist_min', 'twist_max', 'swing_y_limit', 'swing_z_limit', 'friction_torque'}


def need(value, message):
    if not value:
        raise AssertionError(message)


def exact(left, right):
    return json.dumps(left, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(',', ':')) == json.dumps(right, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(',', ':'))


def native(path):
    value = str(Path(path).resolve())
    return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() if ARGS.windows_interop else value


def node(index):
    return hashlib.sha256(f'poima.instance.v1/{RIG}/node/{index}'.encode()).hexdigest()[:32]


def component(entity, kind, value):
    return dict(op='component.set', id=entity, type=kind, value=value)


def pose(position=(0, 0, 0)):
    return dict(position=list(position), rotation=[0, 0, 0, 1])


def profile():
    def part(name, index, mass):
        return dict(name=name, node=index, body_in_bone=pose(), shape=dict(kind='sphere', radius=.2), mass=mass,
                    friction=.5, restitution=0, linear_damping=.05, angular_damping=.05)
    return dict(root='root', initial_mode='animated', parts=[part('root', 2, 3), part('tip', 1, 1)],
                joints=[dict(name='spine', parent='root', child='tip', parent_frame=pose((0, .5, 0)), child_frame=pose((0, -.5, 0)),
                             limits=dict(kind='swing_twist', swing_y=.6, swing_z=.6, twist_min=-.6, twist_max=.6),
                             neutral_rotation=[0, 0, 0, 1], rest_stiffness=2, rest_damping=.1,
                             soft_limit_stiffness=2, soft_limit_zone=.1, max_torque=1000, friction_torque=0)])


class Session:
    def __init__(self, directory, world=None):
        directory.mkdir(parents=True, exist_ok=False)
        self.directory, self.world = directory, world or directory/'world.json'
        self.errors = (directory/'stderr.log').open('w', encoding='utf-8')
        self.requests = (directory/'requests.jsonl').open('w', encoding='utf-8', newline='')
        self.replies = (directory/'responses.jsonl').open('w', encoding='utf-8', newline='')
        self.process = subprocess.Popen([str(ARGS.binary.resolve()), 'world', native(self.world)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.errors, text=True, encoding='utf-8', bufsize=1)
        self.lines, self.recorded, self.closed = queue.Queue(), 0, False
        self.reader_failure = None
        self.owner = dict(directory=str(directory), pid=self.process.pid, natural_exit=False, forced_cleanup=False)
        REPORT['owners'].append(self.owner)
        def reader():
            total = 0
            try:
                while True:
                    line = self.process.stdout.readline(MAX_LINE+1)
                    if not line:
                        break
                    total += len(line.encode())
                    need(len(line.encode()) <= MAX_LINE and total <= MAX_RECORDED and line.endswith('\n'), 'Native stdout exceeded finite line/total bounds')
                    self.replies.write(line); self.replies.flush()
                    self.lines.put(line)
            except Exception as error:
                self.reader_failure = str(error)
            finally:
                self.lines.put(None)
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()

    def rpc(self, method, params=None, error=None):
        need(time.monotonic() < DEADLINE and len(REPORT['calls']) < MAX_CALLS, 'Finite contract deadline/RPC bound exceeded')
        request = dict(jsonrpc='2.0', id=len(REPORT['calls'])+1, method=method, params=params or {})
        line = json.dumps(request, allow_nan=False)+'\n'
        self.requests.write(line); self.requests.flush()
        self.process.stdin.write(line); self.process.stdin.flush()
        reply = self.lines.get(timeout=min(45, max(.1, DEADLINE-time.monotonic())))
        need(reply is not None and len(reply.encode()) <= MAX_LINE, 'Missing/oversized native RPC reply')
        self.recorded += len(line.encode())+len(reply.encode()); need(self.recorded <= MAX_RECORDED, 'Recorded RPC byte bound exceeded')
        value = json.loads(reply)
        # Retain the literal wire values independently of caller-owned dictionaries.
        REPORT['calls'].append(dict(owner=self.owner['directory'], request=json.loads(line), response=json.loads(reply)))
        need(type(value) is dict and value.get('jsonrpc') == '2.0' and type(value.get('id')) is int and value['id'] == request['id'], 'Native RPC identity/type differs')
        if error is not None:
            need(set(value) == {'jsonrpc', 'id', 'error'} and value['error'].get('code') == error and type(value['error']['code']) is int, f'Expected error{error}: {value}')
            return value['error']
        need(set(value) == {'jsonrpc', 'id', 'result'}, f'Native RPC failed: {value}')
        return value['result']

    def close(self):
        if self.closed:
            return
        self.closed = True
        if not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.owner['forced_cleanup'] = True
            self.process.kill(); self.process.wait(timeout=10)
        self.reader.join(timeout=2); self.process.stdout.close()
        for stream in (self.errors, self.requests, self.replies):
            stream.close()
        self.owner.update(exit_code=self.process.returncode, natural_exit=not self.owner['forced_cleanup'], reader_failure=self.reader_failure)
        need(not self.reader.is_alive() and self.reader_failure is None, 'Native response reader did not close cleanly')
        need(self.lines.qsize() == 1 and self.lines.get_nowait() is None, 'Unsolicited native replies after the recorded RPC population')
        need(not self.owner['forced_cleanup'] and self.process.returncode == 0, 'Native world owner did not close naturally with exit0')


def transact(s, ops, revision, error=None):
    return s.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=revision, ops=ops), error)


def prepare(s):
    doc, blob = ribbon(); source = s.directory/'original-analytic-ribbon.glb'; source.write_bytes(glb(doc, blob))
    imported = s.rpc('asset.import', dict(source=native(source)))
    asset = imported['asset']; need(type(asset) is str and re.fullmatch('[0-9a-f]{64}', asset), 'Native imported model identity missing')
    ops = [dict(op='asset.instantiate', id=RIG, name='Physical analytic ribbon', asset=asset),
           component(RIG, 'Transform', dict(**pose((0, 3, 0)), scale=[1, 1, 1])),
           dict(op='entity.create', id=FLOOR, name='Static independent floor'),
           component(FLOOR, 'Transform', dict(**pose((0, -.5, 0)), scale=[1, 1, 1])),
           component(FLOOR, 'BoxCollider', dict(half_extents=[20, .5, 20], motion='static', mass=1, friction=.5, restitution=0))]
    need(transact(s, ops, 0)['revision'] == 1, 'Imported fixture transaction did not commit revision1')
    return asset


def configure(s):
    need(transact(s, [component(RIG, 'PhysicalRig', profile())], 1)['revision'] == 2, 'Physical authoring did not commit revision2')


def start(s, session=SESSION):
    result = s.rpc('runtime.start', dict(session_id=session, revision=2))
    need(result['session_id'] == session and result['tick'] == 0 and result['started'] is True, 'Native start result differs')


def query(s, tick, session=SESSION, **flags):
    result = s.rpc('runtime.physical_rig', dict(session_id=session, tick=tick, entity=RIG, **flags))
    need(set(result) == {'session_id', 'tick', 'source', 'state'} and result['session_id'] == session and type(result['tick']) is int and result['tick'] == tick and result['source'] == 'runtime', 'Physical query envelope differs')
    return result['state']


def validate_projection(state, passive):
    need(set(state) == {'entity', 'mode', 'body_count', 'joint_count', 'parts', 'joints'}, 'Complete projection membership differs')
    need(state['entity'] == RIG and state['mode'] == ('passive' if passive else 'animated') and type(state['body_count']) is int and state['body_count'] == 2 and type(state['joint_count']) is int and state['joint_count'] == 1, 'Physical mode/count types differ')
    need(len(state['parts']) == 2 and len(state['joints']) == 1, 'Full projection lost owned rows')
    for p, (name, index) in zip(state['parts'], [('root', 2), ('tip', 1)]):
        need(set(p) == PART_FIELDS and p['name'] == name and type(p['node']) is int and p['node'] == index and p['node_entity'] == node(index), 'Part fields/frozen node join differ')
        need(type(p['body_id']) is int and 0 <= p['body_id'] <= 0xffffffff and (p['body_id'] & 0x007fffff) < 4096 and (p['body_id'] & 0x80000000) == 0, 'Invalid raw allocated body identity')
        for field, count in [('position', 3), ('rotation', 4), ('linear_velocity', 3), ('angular_velocity', 3), ('inertia', 9)]:
            need(type(p[field]) is list and len(p[field]) == count and all(type(x) is float and math.isfinite(x) for x in p[field]), 'Part numeric vector type/finite membership differs: '+field)
        for field in ['mass', 'max_linear_velocity', 'max_angular_velocity', 'linear_damping', 'angular_damping', 'friction', 'restitution']:
            need(type(p[field]) is float and math.isfinite(p[field]), 'Part numeric scalar type differs: '+field)
        for field in ['enrolled', 'active', 'sleeping', 'at_linear_velocity_limit', 'at_angular_velocity_limit']:
            need(type(p[field]) is bool, 'Physical Boolean coerced: '+field)
        need(p['enrolled'] is passive and p['sleeping'] == (p['enrolled'] and not p['active']), 'Part enrollment/sleep policy differs')
        need(abs(sum(x*x for x in p['rotation'])-1) < 1e-6 and p['rotation'][3] >= 0, 'Diagnostic quaternion is not canonical/unit')
    need(len({p['body_id'] for p in state['parts']}) == 2, 'Physical part body identities alias')
    j = state['joints'][0]; need(set(j) == JOINT_FIELDS and (j['name'], j['parent'], j['child'], j['hinge']) == ('spine', 'root', 'tip', False) and type(j['hinge']) is bool, 'Joint frozen membership/type differs')
    for field, count in [('relative_rotation', 4), ('relative_angular_velocity', 3), ('evaluated_passive_child_torque', 3)]:
        need(type(j[field]) is list and len(j[field]) == count and all(type(x) is float and math.isfinite(x) for x in j[field]), 'Joint vector type differs: '+field)
    for field in JOINT_FIELDS-{'name', 'parent', 'child', 'hinge', 'relative_rotation', 'relative_angular_velocity', 'evaluated_passive_child_torque'}:
        need(type(j[field]) is float and math.isfinite(j[field]), 'Joint scalar type differs: '+field)


def step(s, tick, commands=(), ticks=1, session=SESSION, request=None, error=None):
    return s.rpc('runtime.step', dict(session_id=session, request_id=request or uuid.uuid4().hex, expected_tick=tick, ticks=ticks, physical_rigs=list(commands)), error)


def impulse(part='root', point=(0, 3, 0), value=(6, 0, 0)):
    return dict(entity=RIG, impulse=dict(part=part, world_point=list(point), impulse=list(value)))


def archive(s, saves, tick, slot, session=SESSION):
    receipt = s.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot=slot,
                                     expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0))
    manifest = json.loads((saves/('slot-'+slot)/'current.json').read_bytes())['payload']['current']
    need(re.fullmatch(r'p-[0-9a-f]{32}\.bin', manifest['file']), 'Unexpected durable archive path')
    path = saves/('slot-'+slot)/manifest['file']; raw = path.read_bytes()
    need(len(raw) <= 64*1024*1024 and len(raw) == receipt['bytes'] == manifest['bytes'] and hashlib.sha256(raw).hexdigest() == receipt['sha256'] == manifest['sha256'], 'Complete native archive/receipt identity differs')
    value = json.loads(raw); need(set(value) == {'format', 'version', 'document', 'snapshot'} and value['format'] == 'poima.world-save' and value['version'] == 1, 'World save envelope differs')
    snapshot = value['snapshot']; need(snapshot['format'] == 'poima.runtime-snapshot' and snapshot['payload']['tick'] == tick and set(snapshot['payload']['physical']) == {'version', 'bootstrap', 'rigs', 'constraint_order', 'owner', 'physics'}, 'Complete physical portable extension missing')
    REPORT['archives'].append(dict(path=str(path), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), slot=slot, tick=tick))
    return value


def save_root(s):
    saves = s.directory/'saves'; saves.mkdir()
    result = s.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
    need(result['generation'] == 1, 'Save host configuration generation differs')
    return saves


def authoring(s):
    prepare(s); configure(s); original = s.world.read_bytes(); history = s.rpc('world.history')
    schema = s.rpc('world.describe'); need('PhysicalRig' in schema['components'] and 'runtime.physical_rig' in schema['methods'] and 'physical_rigs' in schema['methods']['runtime.step']['properties'], 'Physical authoring/runtime discovery missing')
    invalid = []
    for path, value in [(('initial_mode',), 'active'), (('parts', 0, 'node'), 2.0), (('parts', 0, 'node'), True), (('parts', 0, 'node'), 9999), (('parts', 0, 'name'), 'a/b'), (('parts', 0, 'mass'), '3'), (('parts', 0, 'mass'), True), (('parts', 0, 'shape'), dict(kind='mesh', asset='0'*64)), (('parts', 0, 'body_in_bone', 'rotation'), [0, 0, 0, 0])]:
        p = profile(); current = p
        for key in path[:-1]:current = current[key]
        current[path[-1]] = value; invalid.append(p)
    p = profile(); p['extra'] = 1; invalid.append(p)
    p = profile(); del p['parts'][0]['angular_damping']; invalid.append(p)
    for value in invalid:
        transact(s, [component(RIG, 'PhysicalRig', value)], 2, -32602)
        need(s.world.read_bytes() == original and exact(s.rpc('world.history'), history), 'Rejected profile partially authored world/history')
    transact(s, [component(FLOOR, 'PhysicalRig', profile())], 2, -32602)
    transact(s, [dict(op='entity.create', id=f'{500:032x}', name='Unsupported attachment', parent=RIG), component(f'{500:032x}', 'BoxCollider', dict(half_extents=[.1]*3, motion='static', mass=1, friction=.5, restitution=0))], 2, -32602)
    need(s.world.read_bytes() == original, 'Unsupported component ownership partially authored world')


def runtime(s):
    prepare(s); configure(s); original = s.world.read_bytes(); start(s); saves = save_root(s)
    summary = query(s, 0); need(exact(summary, dict(entity=RIG, mode='animated', body_count=2, joint_count=1)), 'Default summary contains hidden projections')
    state = query(s, 0, include_parts=True, include_joints=True); validate_projection(state, False)
    need([p['position'] for p in state['parts']] == [[0.0, 3.0, 0.0], [0.0, 4.0, 0.0]], 'Actual allocated analytic COM origins differ')
    first = archive(s, saves, 0, 'before')
    base = dict(session_id=SESSION, tick=0, entity=RIG)
    for update, code in [(dict(tick=1), -32009), (dict(session_id=f'{999:032x}'), -32030), (dict(entity=f'{999:032x}'), -32004), (dict(include_parts=1), -32602), (dict(include_joints='true'), -32602), (dict(extra=True), -32602)]:
        s.rpc('runtime.physical_rig', dict(base, **update), code)
    need(s.rpc('runtime.physical_rig', dict(base, entity=FLOOR))['state'] is None, 'Nonrig should be null')
    need(set(query(s, 0, include_parts=True)) == set(summary)|{'parts'} and set(query(s, 0, include_joints=True)) == set(summary)|{'joints'}, 'Projection flags are not independent')
    for commands in [[dict(entity=RIG)], [dict(entity=RIG, activate=1)], [dict(entity=RIG, activate=True)]*2, [impulse(value=(100001, 0, 0))], [impulse(part='a/b')], [dict(entity=RIG, activate=True, extra=1)]]:
        step(s, 0, commands, error=-32602)
    step(s, 0, [impulse()], error=-32040)  # Impulse does not activate implicitly.
    step(s, 0, [dict(entity=RIG, activate=True), impulse(part='missing')], error=-32040)  # First native command must roll back.
    need(exact(query(s, 0, include_parts=True, include_joints=True), state), 'Failed native command changed committed physical state')
    need(exact(first, archive(s, saves, 0, 'after-rejection')), 'Failed command changed full save state/history/owner/cache')
    request = uuid.uuid4().hex; commands = [dict(entity=RIG, activate=True), impulse()]
    accepted = step(s, 0, commands, request=request); need(accepted['tick'] == 1 and accepted['replayed'] is False, 'Ordered activation+impulse did not commit')
    after = query(s, 1, include_parts=True, include_joints=True); validate_projection(after, True)
    need([p['body_id'] for p in after['parts']] == [p['body_id'] for p in state['parts']] and after['parts'][0]['linear_velocity'][0] > .01, 'Activation reset identity or lost localized impulse')
    need(exact(step(s, 0, commands, request=request), dict(accepted, replayed=True)), 'Physical retry receipt differs')
    step(s, 0, [dict(entity=RIG, activate=True), impulse(value=(7, 0, 0))], request=request, error=-32010)
    need(exact(query(s, 1, include_parts=True, include_joints=True), after) and s.world.read_bytes() == original, 'Retry/read-only operations changed runtime or authored bytes')


def continuation(s):
    prepare(s); configure(s); original = s.world.read_bytes(); start(s); saves = save_root(s)
    step(s, 0, [dict(entity=RIG, activate=True), impulse()]); step(s, 1, ticks=4)
    checkpoint = archive(s, saves, 5, 'checkpoint'); step(s, 5, ticks=7)
    future = archive(s, saves, 12, 'future-reference'); s.close()
    fresh = Session(s.directory/'fresh-owner', s.world)
    try:
        configured = fresh.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        need(configured['generation'] == 1, 'Fresh save configuration differs')
        restored_session = f'{901:032x}'
        restored = fresh.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='checkpoint', expected_generation=1,
                  revision=2, expected_session_id=None, expected_tick=None, expected_gameplay_revision=None, new_session_id=restored_session))
        need(restored['tick'] == 5, 'Fresh native restore clock differs')
        need(exact(checkpoint, archive(fresh, saves, 5, 'fresh-restored', restored_session)), 'Fresh process changed a field in the complete physical save')
        step(fresh, 5, ticks=7, session=restored_session)
        need(exact(future, archive(fresh, saves, 12, 'fresh-future', restored_session)), 'Fresh continuation differs in complete snapshot/document')
        need(s.world.read_bytes() == original, 'Fresh restore mutated authoring')
    finally:
        fresh.close()


def main():
    global ARGS, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1); parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path); parser.add_argument('--output', type=Path)
    ARGS = parser.parse_args(); DEADLINE = time.monotonic()+180
    output = (ARGS.output or ROOT/'build/world-physical-contract'/uuid.uuid4().hex).resolve(); output.mkdir(parents=True, exist_ok=False)
    REPORT.update(output=str(output), runtime=bool(ARGS.runtime), passed=False)
    try:
        for name, function, enabled in [('authoring', authoring, True), ('runtime', runtime, bool(ARGS.runtime)), ('fresh-continuation', continuation, bool(ARGS.runtime))]:
            row = dict(name=name, skipped=not enabled, passed=False); REPORT['groups'].append(row)
            if not enabled:continue
            session = Session(output/name)
            try:
                function(session); row['passed'] = True
            except Exception as error:
                row['failure'] = dict(type=type(error).__name__, message=str(error))
                raise
            finally:
                try:
                    session.close()
                except Exception as error:
                    row['closure_failure'] = dict(type=type(error).__name__, message=str(error))
                    if 'failure' not in row:
                        raise
        REPORT['passed'] = True
    except Exception as error:
        REPORT['failure'] = dict(type=type(error).__name__, message=str(error))
    finally:
        encoded = json.dumps(REPORT, indent=2, allow_nan=False)+'\n'; (output/'report.json').write_text(encoded, encoding='utf-8')
        if ARGS.evidence:ARGS.evidence.parent.mkdir(parents=True, exist_ok=True); ARGS.evidence.write_text(encoded, encoding='utf-8')
        print(json.dumps(dict(passed=REPORT['passed'], output=str(output), calls=len(REPORT['calls']), groups=REPORT['groups'])))
    return 0 if REPORT['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
