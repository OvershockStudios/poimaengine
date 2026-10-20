#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Explicit guarded native authoring step for the new sample's recoil-mode menu.

Run after prepare.py, using an admitted native execution route. No compilation,
rendering, retry or historical fixture mutation. Original stdio is retained.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Fresh authoring receipt/stdio directory.')
    args = parser.parse_args()
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error('Output must be fresh with an existing parent.')
    project = json.loads(args.project.read_bytes())
    world = args.project.parent/project['entry']['world']
    if world.resolve() == (ROOT/'docs/evidence/fixtures/agent-switchback-fresh/world.json').resolve():
        parser.error('Configure an independent prepared copy, never the preserved fixture.')
    args.output.mkdir()
    record = dict(passed=False, calls=[], scope='One guarded UI authoring transaction; not gameplay qualification.')
    deadline = time.monotonic()+30
    client = None
    try:
        client = WorldClient.open(str(args.binary.resolve()), str(world.resolve()),
                                 trace_directory=str((args.output/'stdio').resolve()))
        def rpc(method, params=None):
            remaining = deadline-time.monotonic()
            if remaining <= 0: raise TimeoutError('Authoring deadline expired.')
            row = dict(method=method, params=params or {})
            record['calls'].append(row)
            try: result = client.call(method, params or {}, timeout=min(remaining,10))
            except RpcError as error:
                row['error'] = dict(code=error.code, message=error.message)
                raise
            row['result'] = result
            return result
        revision = rpc('world.inspect')['revision']
        element = dict(parent=f'{500:032x}', name='Feedback profile', kind='button',
            text='Feedback: Hybrid', action='feedback-mode', visible=True, enabled=True,
            layout=dict(position='absolute', left=dict(unit='dp',value=320), top=dict(unit='dp',value=145),
                        width=dict(unit='dp',value=280), height=dict(unit='dp',value=42)),
            style=dict(background_color='#722f37', color='#f7e8eb', font_size=18,
                       border_color='#722f37', border_width=1,
                       focus=dict(border_color='#d46a7e')))
        result = rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=revision,
            ops=[dict(op='ui.element.set', id=f'{510:032x}', element=element)]))
        current = rpc('world.inspect')
        if result['committed'] is not True or current['revision'] != result['revision']:
            raise RuntimeError('Guarded authoring did not publish the expected revision.')
        record['passed'] = True
    except BaseException as error:
        record['failure'] = dict(type=type(error).__name__, message=str(error))
        raise
    finally:
        original_failure = sys.exc_info()[0] is not None
        try:
            if client is not None:
                try: client.close()
                except BaseException as error:
                    record['passed'] = False
                    record['close_error'] = str(error)
                    if not original_failure: raise
                finally:
                    prior_error = original_failure or sys.exc_info()[0] is not None
                    try:
                        record['close'] = dict(closed=client.closed, pid=client.transport.process_id,
                            exit_code=client.transport.returncode,
                            cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),
                            scope='Direct SDK lifetime only; external process-tree ownership is caller supplied.')
                        if not client.closed or client.transport.returncode != 0:
                            raise RuntimeError('Native authoring process did not close with exit0.')
                    except BaseException as error:
                        record['passed'] = False
                        record['close_metadata_error'] = repr(error)
                        if not prior_error: raise
        finally:
            (args.output/'report.json').write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')


if __name__ == '__main__': main()
