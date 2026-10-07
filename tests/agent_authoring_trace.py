#!/usr/bin/env python3
"""Verify a recorded agent authoring exercise without calling a provider."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path


def verify(provider, events, world):
    calls, pending = [], {}
    completed = False
    for line in events.read_text(encoding='utf-8').splitlines():
        event = json.loads(line)
        if provider == 'codex':
            completed |= event.get('type') == 'turn.completed'
            item = event.get('item', {})
            if event.get('type') == 'item.completed' and item.get('type') == 'mcp_tool_call':
                assert item['server'] == 'poima', 'Unexpected server'
                assert item.get('result'), 'Missing tool result'
                result = item['result']['structured_content']
                calls.append((item['tool'], item['arguments'], result))
            if event.get('type') == 'item.completed':
                assert item.get('type') in ('mcp_tool_call', 'agent_message', 'reasoning'), 'Unexpected tool activity'
        else:
            if event.get('type') == 'result':
                assert not event.get('is_error') and event.get('subtype') == 'success', 'Agent turn failed'
                completed = True
            for item in event.get('message', {}).get('content', []):
                if not isinstance(item, dict):
                    continue
                if item.get('type') == 'tool_use':
                    assert item['name'].startswith('mcp__poima__'), 'Unexpected tool activity'
                    assert item['id'] not in pending, 'Duplicate tool ID'
                    pending[item['id']] = (item['name'].removeprefix('mcp__poima__'), item['input'])
                if item.get('type') == 'tool_result':
                    name, arguments = pending.pop(item['tool_use_id'])
                    calls.append((name, arguments, json.loads(item['content'])))
    assert completed and not pending, 'Incomplete agent run'
    assert calls and calls[0][0] == 'poima_discover', 'Discovery must precede edits'
    assert any(name == 'poima_discover' and args == {'view': 'section', 'name': 'invariants'}
               for name, args, result in calls), 'Missing invariants discovery'
    operations = [(args['method'], args.get('params', {}), result)
                  for name, args, result in calls if name == 'poima_call']
    assert all(name in ('poima_discover', 'poima_call') for name, _, _ in calls)
    allowed = {'world.inspect', 'world.transact', 'entity.query', 'world.undo', 'world.redo'}
    assert all(method in allowed for method, _, _ in operations), 'Unexpected operation'
    assert operations[0][0] == 'world.inspect' and operations[0][2]['result']['revision'] == 0
    writes = [(method, args, result) for method, args, result in operations
              if method in ('world.transact', 'world.undo', 'world.redo')]
    assert [method for method, _, _ in writes] == ['world.transact']*3 + ['world.undo', 'world.redo']
    first, replay, stale, undo, redo = writes
    assert first[1] == replay[1], 'Retry changed its payload'
    assert first[1]['base_revision'] == 0 and len(first[1]['ops']) == 1
    op = first[1]['ops'][0]
    assert op['op'] == 'entity.create' and op['name'] == 'Agent integration probe'
    assert first[2]['result']['revision'] == 1 and not first[2]['result']['replayed']
    assert replay[2]['result']['revision'] == 1 and replay[2]['result']['replayed']
    assert stale[1]['request_id'] != first[1]['request_id'] and stale[1]['base_revision'] == 0
    assert stale[2]['error']['code'] == -32009
    assert undo[1]['base_revision'] == 1 and undo[2]['result']['revision'] == 2
    assert redo[1]['base_revision'] == 2 and redo[2]['result']['revision'] == 3
    queries = [result['result'] for method, _, result in operations if method == 'entity.query']
    for revision, count in [(1, 1), (2, 0), (3, 1)]:
        assert any(row['revision'] == revision and len(row['entities']) == count for row in queries)
    stored = json.loads(world.read_text())
    assert stored['revision'] == 3 and list(stored['entities']) == [op['id']]
    assert stored['entities'][op['id']]['name'] == op['name']
    return {'passed': True, 'provider': provider, 'tool_calls': len(calls), 'operations': len(operations),
            'final_revision': 3, 'final_entities': 1,
            'events_sha256': hashlib.sha256(events.read_bytes()).hexdigest(),
            'world_sha256': hashlib.sha256(world.read_bytes()).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('provider', choices=['codex', 'claude'])
    parser.add_argument('events', type=Path)
    parser.add_argument('world', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.provider, args.events, args.world), indent=2))
