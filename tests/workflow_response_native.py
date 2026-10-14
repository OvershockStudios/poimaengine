#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture focused real workflow replies in source and immutable game services.

Uses the existing retained transport, never reconstructs native responses, and
does not render. The readonly fixture can have an empty component registry;
nonempty component coverage is reported separately in the authored fixtures.
Run under a supervising Windows Job (see switchback_ownership.gated).
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError
from switchback_game_service import (Budget, RetainedTransport, retain_host_stream,
                                     wait_host)


def need(value, message):
    if not value:
        raise AssertionError(message)


def pin(path):
    with path.open('rb') as stream:
        return dict(bytes=path.stat().st_size,
                    sha256=hashlib.file_digest(stream, 'sha256').hexdigest())


def tree(path):
    return {p.relative_to(path).as_posix(): pin(p)
            for p in sorted(path.rglob('*')) if p.is_file()}


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def uid(value):
    return f'{value:032x}'


A, B, C, TYPE = [uid(value) for value in (10, 20, 30, 100)]
FIELDS = [uid(value) for value in (1000, 1001, 1002, 1003)]
SCHEMA = dict(id=TYPE, name='Workflow observations', version=2, fields=[
    dict(id=FIELDS[0], name='Count', kind='int32', default=0),
    dict(id=FIELDS[1], name='Exact score', kind='int64', default='0'),
    dict(id=FIELDS[2], name='Target', kind='entity', default=uid(0)),
    dict(id=FIELDS[3], name='Ordered parts', kind='array', element_kind='int32',
         capacity=3, default=[])])


class Recorded:
    def __init__(self, binary, directory, world=None, game=None, shared=False):
        directory.mkdir()
        self.directory, self.calls, self.raw = directory, [], []
        self.budget = Budget(time.monotonic()+90, [])
        self.host, self.host_threads, self.client = None, [], None
        endpoint = 'workflow-response-'+uuid.uuid4().hex
        if shared:
            prefix = [str(binary), 'game', 'serve', str(game)] if game else [str(binary), 'serve', str(world)]
            self.host = subprocess.Popen([*prefix, '--endpoint', endpoint],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         stdin=subprocess.DEVNULL, bufsize=0)
            outputs = [directory/'host-stdout.txt', directory/'host-stderr.txt']
            for role, path in zip(('stdout', 'stderr'), outputs):
                path.touch()
                self.budget.paths.append(path)
                thread = threading.Thread(target=retain_host_stream,
                    args=(self.host, role, path, self.budget), daemon=True)
                thread.start()
                self.host_threads.append(thread)
            wait_host(self.host, endpoint, *outputs, self.budget)
            argv = [str(binary), 'connect', endpoint, '--timeout-ms', '10000']
        else:
            argv = [str(binary), 'world', str(world)]
        self.transport = RetainedTransport(argv, directory, self.budget, self.raw.append)
        self.client = WorldClient(self.transport)

    def call(self, method, params=None, error=None):
        params = {} if params is None else params
        record = dict(id=self.transport.counter+1, method=method, params=copy.deepcopy(params))
        self.calls.append(record)
        try:
            result = self.client.call(method, params, timeout=15)
            record['result'] = copy.deepcopy(result)
            need(error is None, 'Expected rejection: '+method)
            return result
        except RpcError as rejected:
            record['error'] = copy.deepcopy(self.raw[-1]['error'])
            need(error is not None and rejected.code == error,
                 'Unexpected native error: '+str(rejected))
            return record['error']
        finally:
            need(self.raw and type(self.raw[-1]['id']) is int and
                 self.raw[-1]['id'] == record['id'], 'Native/request identity mismatch')

    def close(self):
        if self.client is not None:
            if self.host is not None:
                self.call('host.shutdown')
            self.client.close()
            need(self.client.cleanup_error is None, 'Client forced/failed cleanup')
        if self.host is not None:
            self.host.wait(timeout=self.budget.remaining(10))
            for thread in self.host_threads:
                thread.join(timeout=self.budget.remaining(5))
            need(self.host.returncode == 0 and all(not t.is_alive() for t in self.host_threads),
                 'Shared owner did not close naturally')
        write(self.directory/'transcript.json', dict(calls=self.calls, raw=self.raw,
              returncode=self.transport.returncode, host_returncode=None if self.host is None else self.host.returncode,
              forced=[], scope='focused response capture; no renderer or performance qualification'))


def state(owner, session, world):
    runtime = owner.call('runtime.inspect', dict(session_id=session))
    return dict(authored=pin(world), runtime=runtime,
                gameplay=owner.call('runtime.gameplay.inspect', dict(session_id=session, include_schema=True)),
                components=owner.call('runtime.components', dict(session_id=session)),
                ui=owner.call('runtime.ui.inspect', dict(session_id=session, tick=runtime['tick'])),
                saves=owner.call('save.status'))


def component_fixture(owner, world, saves):
    initial = owner.call('runtime.status')
    need(not initial['active'], 'Source owner unexpectedly started runtime')
    need(owner.call('save.status')['root'] is None, 'Source save store should start disabled')
    owner.call('save.inspect', dict(slot='probe'), error=-32070)
    owner.call('runtime.inspect', dict(session_id=uuid.uuid4().hex), error=-32030)
    ops = [dict(op='component.schema.set', schema=SCHEMA)]
    for identity in (A, B, C):
        ops.append(dict(op='entity.create', id=identity, name='Workflow '+identity[-4:]))
    values = {FIELDS[0]:7, FIELDS[1]:'9223372036854775807', FIELDS[2]:B,
              FIELDS[3]:[9, 2, 9]}
    for identity in (A, C):
        ops.append(dict(op='component.set', id=identity, type='game:'+TYPE, value=values))
    owner.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
    source = pin(world)
    session = uuid.uuid4().hex
    start = dict(session_id=session, revision=1)
    started = owner.call('runtime.start', start)
    need(owner.call('runtime.start', start) == dict(started, replayed=True), 'Start retry changed')
    owner.call('runtime.inspect', dict(session_id=session))
    need(owner.call('runtime.gameplay.inspect', dict(session_id=session))['module'] is None,
         'Authored fixture must have no compiled module')
    registry = owner.call('runtime.components', dict(session_id=session))
    need(len(registry['schemas']) == 1, 'Nonempty registry missing')
    for identity in (A, B):
        result = owner.call('runtime.component.get', dict(session_id=session, tick=0, id=identity, type=TYPE))
        need(result['values'] == (values if identity == A else None), 'Absent/present component conflated')
        owner.call('runtime.entity', dict(session_id=session, tick=0, id=identity))
    first = owner.call('runtime.component.query', dict(session_id=session, tick=0, type=TYPE, limit=1))
    second = owner.call('runtime.component.query', dict(session_id=session, tick=0, type=TYPE,
                                                      limit=1, after=first['next_after']))
    need(first['entities'] == [A] and second['entities'] == [C] and second['next_after'] is None,
         'Component exclusive pagination differs')
    owner.call('runtime.observe', dict(session_id=session, tick=0, ids=[B, A]))
    observation = owner.call('runtime.observe', dict(session_id=session, tick=0, ids=[B, A],
        query=dict(type=TYPE, limit=1), include_schemas=True, components=[dict(type=TYPE)]))
    need([row['id'] for row in observation['entities']] == [A, B], 'Joined membership differs')
    owner.call('runtime.observe', dict(session_id=session, tick=0,
        query=dict(type=TYPE, limit=1, after=observation['query']['next_after']),
        structure_revision=0, component_revision=0, entity_fields=[],
        components=[dict(type=TYPE, fields=[FIELDS[1], FIELDS[3]])]))
    owner.call('runtime.observe', dict(session_id=session, tick=0, ids=[A, B], entity_fields=[],
                                     components=[dict(type=TYPE, fields=[])]))
    before = state(owner, session, world)
    owner.call('runtime.component.get', dict(session_id=session, tick=0.0, id=A, type=TYPE), error=-32602)
    owner.call('runtime.component.get', dict(session_id=session, tick=1, id=A, type=TYPE), error=-32009)
    owner.call('runtime.observe', dict(session_id=session, tick=0, ids=[A, A]), error=-32602)
    owner.call('runtime.observe', dict(session_id=session, tick=0,
        query=dict(type=TYPE, after=A, limit=1)), error=-32602)
    need(state(owner, session, world) == before, 'Rejected reads changed source/runtime/UI/save state')
    configuration = dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(saves))
    configured = owner.call('save.configure', configuration)
    need(owner.call('save.configure', configuration) == dict(configured, replayed=True), 'Configuration retry changed')
    owner.call('save.inspect', dict(slot='probe'))
    owner.call('runtime.save.status', dict(session_id=session))
    ticket = dict(epoch=uuid.uuid4().hex, sequence=1)
    need(owner.call('runtime.save.result', ticket)['state'] == 0, 'Unknown ticket must be expired')
    owner.call('runtime.save.result', dict(ticket, sequence=0), error=-32602)
    write_params = dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='probe',
        expected_generation=0, session_id=session, expected_tick=0, expected_gameplay_revision=0,
        expected_component_revision=0, expected_structure_revision=0, expected_ui_revision=0,
        expected_control_sequence=0, expected_material_revision=0)
    saved = owner.call('save.write', write_params)
    need(owner.call('save.write', write_params) == dict(saved, replayed=True), 'Durable write retry changed')
    step_params = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=2)
    stepped = owner.call('runtime.step', step_params)
    need(owner.call('runtime.step', step_params) == dict(stepped, replayed=True), 'Step retry changed')
    restored_id = uuid.uuid4().hex
    load_params = dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='probe',
        expected_generation=1, revision=1, expected_session_id=session, expected_tick=2,
        expected_gameplay_revision=0, expected_component_revision=0, expected_structure_revision=0,
        expected_ui_revision=0, expected_control_sequence=0, expected_material_revision=0,
        new_session_id=restored_id, gameplay=None)
    restored = owner.call('save.load', load_params)
    need(restored['tick'] == 0, 'Saved tick was not restored')
    need(owner.call('save.load', load_params) == dict(restored, replayed=True), 'Load retry changed')
    owner.call('runtime.component.get', dict(session_id=restored_id, tick=0, id=A, type=TYPE))
    stopped = owner.call('runtime.stop', dict(session_id=restored_id))
    need(owner.call('runtime.stop', dict(session_id=restored_id)) == dict(stopped, replayed=True), 'Stop retry changed')
    owner.call('runtime.status')
    owner.call('runtime.save.result', ticket)
    cold_params = dict(load_params, request_id=uuid.uuid4().hex,
        expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
        expected_component_revision=None, expected_structure_revision=None,
        expected_ui_revision=None, expected_control_sequence=None, expected_material_revision=None,
        new_session_id=uuid.uuid4().hex)
    cold = owner.call('save.load', cold_params)
    need(owner.call('save.load', cold_params) == dict(cold, replayed=True), 'Stopped load retry changed')
    cold_component = owner.call('runtime.component.get', dict(session_id=cold_params['new_session_id'],
                                                             tick=0, id=A, type=TYPE))
    need(cold_component['values'] == values, 'Stopped load lost exact component values')
    owner.call('runtime.stop', dict(session_id=cold_params['new_session_id']))
    owner.call('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=1, root=None))
    owner.call('save.status')
    need(pin(world) == source, 'Runtime/save operations changed authored world')


def compiled_fixture(owner, world, descriptor, saves, readonly):
    current = owner.call('runtime.status')
    if readonly:
        need(current['active'], 'Game owner must automatically start its runtime')
        session = current['session_id']
    else:
        need(not current['active'], 'Source should initially be inactive')
        session = uuid.uuid4().hex
        revision = owner.call('world.inspect')['revision']
        owner.call('runtime.start', dict(session_id=session, revision=revision))
        params = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
                      expected_revision=0, descriptor=str(descriptor))
        first = owner.call('runtime.gameplay.load_native', params)
        owner.call('runtime.gameplay.load_native', dict(params, expected_revision=0.0), error=-32602)
        need(owner.call('runtime.gameplay.load_native', params) == dict(first, replayed=True), 'Native retry changed')
    full = owner.call('runtime.gameplay.inspect', dict(session_id=session, include_schema=True))
    compact = owner.call('runtime.gameplay.inspect', dict(session_id=session))
    need(set(compact['module']) == {'identity', 'assembly_sha256', 'values'}, 'Compact module leaked schema')
    names = list(full['module']['values'])[:2]
    owner.call('runtime.gameplay.inspect', dict(session_id=session, include_schema=False, fields=names))
    owner.call('runtime.gameplay.inspect', dict(session_id=session, include_schema=True, fields=[]))
    owner.call('runtime.gameplay.inspect', dict(session_id=session, fields=[]))
    before = state(owner, session, world)
    owner.call('runtime.gameplay.inspect', dict(session_id=session, fields=['unknown_workflow_field']), error=-32602)
    owner.call('runtime.gameplay.inspect', dict(session_id=session, fields=[names[0], names[0]]), error=-32602)
    need(state(owner, session, world) == before, 'Rejected selectors changed source/runtime/UI/save state')
    configuration = owner.call('save.status')
    owner.call('save.configure', dict(request_id=uuid.uuid4().hex,
        expected_generation=configuration['generation'], root=str(saves)))
    ui = owner.call('runtime.ui.inspect', dict(session_id=session, tick=0))
    begin = next(row for row in ui['elements'] if row['action'] == 'begin')
    activate = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
        expected_ui_revision=ui['ui_revision'], expected_control_sequence=ui['control_sequence'],
        expected_gameplay_revision=full['revision'], id=begin['id'])
    result = owner.call('runtime.ui.activate', activate)
    need(owner.call('runtime.ui.activate', activate) == dict(result, replayed=True), 'Control retry changed')
    session = result['current_session_id']
    owner.call('runtime.inspect', dict(session_id=session))
    owner.call('runtime.components', dict(session_id=session))
    owner.call('runtime.save.status', dict(session_id=session))
    owner.call('runtime.observe', dict(session_id=session, tick=0, ids=[uid(2)]))
    owner.call('runtime.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1))
    owner.call('runtime.ui.inspect', dict(session_id=session, tick=1))
    owner.call('runtime.save.result', dict(epoch=uuid.uuid4().hex, sequence=1))
    owner.call('runtime.stop', dict(session_id=session))
    owner.call('runtime.status')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Windows host without optimization required')
    need(not args.output.exists(), 'Fresh capture directory required')
    args.output.mkdir(parents=True)
    started = time.monotonic()
    originals = dict(source=tree(args.source), game=tree(args.game))
    report = dict(passed=False, engine=pin(args.binary), harness=pin(Path(__file__)), owners=[],
        limits=['No renderer/GPU/player, physical input or performance qualification.',
                'Registered nonempty components are covered in source standalone/shared scopes; '
                'readonly compiled fixture may have an empty registry.',
                'Pending/resolving/failure tickets, recovered/upgraded saves and broad gameplay '
                'reload are separate gates.'])
    owner = None
    try:
        for name, shared in [('component-standalone', False), ('component-shared', True)]:
            fixture = args.output/(name+'-project')
            fixture.mkdir()
            world, saves = fixture/'world.json', args.output/(name+'-saves')
            saves.mkdir()
            owner = Recorded(args.binary, args.output/name, world=world, shared=shared)
            component_fixture(owner, world, saves)
            owner.close()
            report['owners'].append(dict(name=name, calls=len(owner.calls), returncode=owner.transport.returncode))
            owner = None
        copied = args.output/'compiled-source'
        __import__('shutil').copytree(args.source, copied)
        copied_before = tree(copied)
        project = json.loads((copied/'project.json').read_text())
        game = json.loads((args.game/'game.json').read_text())
        for name, readonly in [('compiled-standalone', False), ('compiled-readonly', True)]:
            world = args.game/game['entry']['world'] if readonly else copied/project['entry']['world']
            descriptor = args.game/game['gameplay']['descriptor'] if readonly else copied/project['gameplay']['descriptor']
            saves = args.output/(name+'-saves')
            saves.mkdir()
            owner = Recorded(args.binary, args.output/name, world=world,
                             game=args.game/'game.json' if readonly else None, shared=readonly)
            compiled_fixture(owner, world, descriptor, saves, readonly)
            owner.close()
            report['owners'].append(dict(name=name, calls=len(owner.calls), returncode=owner.transport.returncode))
            owner = None
        need(dict(source=tree(args.source), game=tree(args.game)) == originals,
             'Original source or immutable exported game changed')
        need(tree(copied) == copied_before, 'Runtime operations changed copied compiled source')
        report['original_inputs_unchanged'] = True
        report['passed'] = True
    except BaseException:
        report['error'] = traceback.format_exc()
    finally:
        if owner is not None:
            try:
                owner.close()
            except BaseException:
                report['close_error'] = traceback.format_exc()
        report['seconds'] = time.monotonic()-started
        write(args.output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], seconds=report['seconds'], error=report.get('error'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
