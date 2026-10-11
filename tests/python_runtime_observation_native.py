#!/usr/bin/env python3
"""Qualify bounded Python observation pages against an actual headless engine.

No managed compilation, renderer or player is used. Run with matching-platform
Python and engine paths; an external supervisor owns the whole-process deadline.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def identity(number):
    return f'{number:032x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, type=Path)
    parser.add_argument('--installed', action='store_true',
                        help='Use installed poima-client instead of checkout sources.')
    args = parser.parse_args()
    if not args.installed:
        sys.path.insert(0, str(ROOT/'tools/python'))
    from poima_client import WorldClient, RpcError, ObservationError, new_id

    deadline = time.monotonic() + 25

    class CountedClient(WorldClient):
        def __init__(self, transport, **kwargs):
            super().__init__(transport, **kwargs)
            self.calls = []
            request = transport.request

            def counted_request(method, params=None, timeout=30.0):
                # All public helpers, including mutation receipts, use this
                # transport boundary. Count and bound each actual request once.
                remaining = deadline - time.monotonic()
                need(remaining > 0, 'Native observation qualification exceeded its deadline')
                self.calls.append(method)
                return request(method, params, timeout=min(timeout, 3, remaining))

            transport.request = counted_request

    component_type, empty_type = identity(1000), identity(2000)
    fields = [identity(3000+i) for i in range(4)]
    ids = [identity(10000+i) for i in range(65)]
    schema = dict(id=component_type, name='Paged native inventory', version=2, fields=[
        dict(id=fields[0], name='Count', kind='int32', default=0),
        dict(id=fields[1], name='Score', kind='int64', default='0'),
        dict(id=fields[2], name='Target', kind='entity', default=identity(0)),
        dict(id=fields[3], name='Ordered parts', kind='array',
             element_kind='int32', capacity=3, default=[])])
    empty_schema = dict(id=empty_type, name='Empty membership', version=1, fields=[
        dict(id=identity(4000), name='Value', kind='float64', default=0)])
    state_fields = sorted(('world_matrix', 'layout', 'local_transform', 'animation',
        'motion', 'kinematic_target', 'motion_remaining_ticks', 'velocity',
        'has_body', 'is_character', 'ground', 'yaw', 'pitch'))
    pins = ('structure_revision', 'component_revision', 'gameplay_revision',
            'ui_revision', 'control_sequence')
    source = ('authored_revision', 'current_authored_revision', 'source_stale')
    header = ('session_id', 'tick', *pins, *source)
    expected_values = {}
    ops = [dict(op='component.schema.set', schema=schema),
           dict(op='component.schema.set', schema=empty_schema)]
    for index, entity in enumerate(ids):
        values = dict(zip(fields, [index, '9223372036854775807',
            ids[(index+1) % len(ids)], [9, index, 9]]))
        expected_values[entity] = values
        ops.extend([
            dict(op='entity.create', id=entity, name=f'Observed entity {index}'),
            dict(op='component.set', id=entity, type='Transform', value=dict(
                position=[index*.1, 0, 1], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=entity, type='game:'+component_type, value=values)])

    result = dict(passed=False, groups=[], entity_count=len(ids), rpc_count=0,
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    client = None
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='poima-observation-native-') as scratch:
            world = Path(scratch)/'world.json'
            client = CountedClient.open(args.engine.resolve(), world, close_timeout=3)
            with client:
                catalog = client.discover()
                need({'runtime.start', 'runtime.observe', 'runtime.component.edit'}
                     <= set(catalog['methods']), 'Required native runtime methods are unavailable')
                authored = client.transact(ops, 0, request_id=new_id(), timeout=3)
                need(authored['revision'] == 1, 'Fixture was not authored in one transaction')
                original_world = world.read_bytes()
                session = new_id()
                client.call('runtime.start', dict(session_id=session, revision=1))
                count = client.calls.count('runtime.observe')
                pages = list(client.iter_component_observations(session, 0, component_type,
                    limit=32, max_pages=3, entity_fields=state_fields,
                    components=[dict(type=component_type)], include_schemas=True, timeout=3))
                need(client.calls.count('runtime.observe')-count == 3,
                     'Three-page traversal issued unexpected reads')
                need([len(page['entities']) for page in pages] == [32, 32, 1],
                     'Native page boundaries differ')
                need([page['query']['next_after'] for page in pages] == [ids[31], ids[63], None],
                     'Exclusive native cursors differ')
                rows = [row for page in pages for row in page['entities']]
                need([row['id'] for row in rows] == ids, 'Traversal membership is incomplete or reordered')
                boundary = {key: pages[0][key] for key in header}
                for page in pages:
                    need({key: page[key] for key in header} == boundary,
                         'Traversal mixed native boundaries')
                for row in rows:
                    entity = client.call('runtime.entity', dict(session_id=session, tick=0,
                        structure_revision=0, id=row['id']))
                    component = client.call('runtime.component.get', dict(session_id=session,
                        tick=0, structure_revision=0, id=row['id'], type=component_type))
                    need(row['state'] == {key: entity[key] for key in state_fields},
                         'Selected state differs from individual native observation')
                    need(row['components'][component_type] == component['values']
                         == expected_values[row['id']], 'Typed values differ from native fixture')
                    metadata = pages[0]['component_types'][0]
                    need(metadata['schema'] == component['schema']
                         and metadata['fingerprint'] == component['schema']['fingerprint'],
                         'Native schema metadata differs')
                result['groups'].append('65_entities_three_pages_individual_parity')

                selected_pins = {key: boundary[key] for key in pins}
                empty = list(client.iter_component_observations(session, 0, empty_type,
                    pins=selected_pins, limit=32, max_pages=1, entity_fields=[],
                    components=[dict(type=empty_type, fields=[])], timeout=3))
                need(len(empty) == 1 and empty[0]['entities'] == []
                     and empty[0]['query'] == dict(type=empty_type, entities=[], next_after=None),
                     'Empty native query did not finish explicitly')
                need({key: empty[0][key] for key in header} == boundary,
                     'Empty query changed the supplied observation boundary')
                result['groups'].append('empty_membership_supplied_pins')

                bounded = client.iter_component_observations(session, 0, component_type,
                    limit=32, max_pages=1, timeout=3)
                count = client.calls.count('runtime.observe')
                need(next(bounded)['query']['next_after'] == ids[31], 'Missing first bounded page')
                try:
                    next(bounded)
                    raise AssertionError('Page budget silently returned a completed traversal')
                except ObservationError:
                    pass
                need(client.calls.count('runtime.observe')-count == 1,
                     'Page budget issued an extra native read')
                result['groups'].append('page_budget_never_silently_truncates')

                traversal = client.iter_component_observations(session, 0, component_type,
                    pins=selected_pins, limit=32, max_pages=3,
                    components=[dict(type=component_type)], timeout=3)
                next(traversal)
                changed = dict(expected_values[ids[40]])
                changed[fields[0]] = 999
                client.call('runtime.component.edit', dict(session_id=session, request_id=new_id(),
                    expected_tick=0, expected_revision=0, expected_structure_revision=0,
                    id=ids[40], type=component_type, values=changed))
                before = client.call('runtime.inspect', dict(session_id=session))
                count = client.calls.count('runtime.observe')
                try:
                    next(traversal)
                    raise AssertionError('Iterator continued through a real component conflict')
                except RpcError as error:
                    need(error.code == -32009, 'Native stale-component error was not preserved')
                need(client.calls.count('runtime.observe')-count == 1,
                     'Iterator retried a conflicting native request')
                need(client.call('runtime.inspect', dict(session_id=session)) == before,
                     'Rejected read mutated native runtime state')
                current = client.call('runtime.component.get', dict(session_id=session, tick=0,
                    structure_revision=0, id=ids[40], type=component_type))
                need(current['values'] == changed, 'Rejected read changed committed component values')
                need(world.read_bytes() == original_world, 'Runtime reads/edit rewrote authored content')
                result['groups'].append('real_same_tick_conflict_no_retry_no_mutation')
                client.call('runtime.stop', dict(session_id=session))
                need(not client.call('runtime.status')['active'], 'Normal runtime stop did not finish')
            need(client.transport.closed and client.transport.returncode == 0,
                 'Owned native process did not close successfully')
            need(not client.transport.stderr_tail and not client.transport.stderr_truncated,
                 'Owned native process produced unexpected or truncated stderr')
        result['passed'] = True
    except Exception as error:
        result['error'] = f'{type(error).__name__}: {error}'
    finally:
        if client is not None:
            result['rpc_count'] = len(client.calls)
            result['owner'] = dict(closed=client.transport.closed,
                returncode=client.transport.returncode,
                stderr_truncated=client.transport.stderr_truncated)
        result['elapsed_seconds'] = time.monotonic()-started
        print(json.dumps(result, allow_nan=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
