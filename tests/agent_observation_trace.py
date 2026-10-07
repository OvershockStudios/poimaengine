#!/usr/bin/env python3
"""Check a recorded agent's MCP image receipt and answer against captured pixels."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
from scene_capture import pixels, mask


def verify(provider, events, capture):
    pending, calls, answers = {}, [], []
    complete = False
    for line in events.read_text(encoding='utf-8').splitlines():
        event = json.loads(line)
        if provider == 'codex':
            complete |= event.get('type') == 'turn.completed'
            item = event.get('item', {})
            if event.get('type') != 'item.completed':
                continue
            assert item.get('type') in ('mcp_tool_call', 'agent_message', 'reasoning'), 'Unexpected activity'
            if item.get('type') == 'agent_message':
                answers.append(item['text'])
            if item.get('type') == 'mcp_tool_call':
                assert item['server'] == 'poima' and item.get('result'), 'Unexpected or failed tool'
                calls.append((item['tool'], item['arguments'], item['result']['content'],
                              item['result']['structured_content']))
        else:
            if event.get('type') == 'result':
                assert not event.get('is_error') and event.get('subtype') == 'success'
                complete = True
                answers.append(event['result'])
            for item in event.get('message', {}).get('content', []):
                if not isinstance(item, dict):
                    continue
                if item.get('type') == 'tool_use':
                    assert item['name'].startswith('mcp__poima__')
                    assert item['id'] not in pending
                    pending[item['id']] = (item['name'].removeprefix('mcp__poima__'), item['input'])
                if item.get('type') == 'tool_result':
                    name, arguments = pending.pop(item['tool_use_id'])
                    blocks = item['content']
                    if isinstance(blocks, str):
                        blocks = [{'type': 'text', 'text': blocks}]
                    texts = [json.loads(b['text']) for b in blocks if b['type'] == 'text']
                    assert len(texts) == 1
                    calls.append((name, arguments, blocks, texts[0]))
    assert complete and not pending and answers, 'Incomplete run'
    assert all(name in ('poima_discover', 'poima_call') for name, *_ in calls)
    assert any(name == 'poima_discover' and args == {'view': 'method', 'name': 'world.capture'}
               for name, args, *_ in calls), 'Missing capture discovery'
    operations = [row for row in calls if row[0] == 'poima_call']
    assert len(operations) == 1 and operations[0][1]['method'] == 'world.capture', 'Unexpected operations'
    _, arguments, blocks, structured = operations[0]
    assert 'image' in arguments and 'image' not in arguments.get('params', {})
    receipt, observation = structured['result'], structured['observation']
    assert receipt['hardware'] and receipt['capture_written'] and receipt['nvrhi_errors'] == 0
    assert observation['state'] == 'ready' and observation['format'] == 'PNG'
    assert receipt['width'] == observation['source_width'] == observation['width'] == 640
    assert receipt['height'] == observation['source_height'] == observation['height'] == 400
    digest = hashlib.sha256(capture.read_bytes()).hexdigest()
    assert observation['source_sha256'] == digest, 'Receipt does not match captured BMP'
    images = [b for b in blocks if b['type'] == 'image']
    assert len(images) == 1, 'Missing image block'
    source = images[0] if provider == 'codex' else images[0]['source']
    assert source.get('mimeType', source.get('media_type')) == 'image/png'
    png = base64.b64decode(source['data'], validate=True)
    assert png[:8] == b'\x89PNG\r\n\x1a\n' and png[12:16] == b'IHDR'
    assert struct.unpack_from('>II', png, 16) == (640, 400)
    measured = {color: mask(pixels(capture), index) for index, color in enumerate(('red', 'green', 'blue'))}
    visible = sorted((color for color in measured if measured[color]['count'] > 1000),
                     key=lambda color: measured[color]['centroid_x'])
    assert len(visible) == 2
    assert measured[visible[0]]['centroid_x'] < 320 < measured[visible[1]]['centroid_x']
    answer = json.loads(answers[-1])
    assert answer == {'left': visible[0], 'right': visible[1]}, 'Answer differs from actual pixels'
    return {'passed': True, 'provider': provider, 'gpu': receipt['gpu'], 'answer': answer,
            'image_dimensions': [640, 400], 'tool_calls': len(calls),
            'events_sha256': hashlib.sha256(events.read_bytes()).hexdigest(),
            'capture_sha256': digest, 'delivered_png_sha256': hashlib.sha256(png).hexdigest(),
            'delivered_png_bytes': len(png)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('provider', choices=['codex', 'claude'])
    parser.add_argument('events', type=Path)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.provider, args.events, args.capture), indent=2))
