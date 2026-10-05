#!/usr/bin/env python3
"""Profile actual compiled gameplay rollback and host-serviced save/load handoff."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import subprocess
import uuid

from gameplay_save_requests import ROOT, TYPE, Session, inventory, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.native_descriptor:
        if any((args.hostfxr, args.bridge, args.assembly)):
            parser.error('Choose NativeAOT descriptor or CoreCLR paths, not both.')
    elif not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('Supply --native-descriptor or all three CoreCLR paths.')

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    args.native = native
    args.config = dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(
        hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(args.assembly), type=TYPE)
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    evidence = dict(passed=False, backend='native_aot' if args.native_descriptor else 'coreclr', calls=[], checks=[],
        binary_sha256=sha(args.binary), test_sha256=sha(__file__), helper_sha256=sha(ROOT/'tests/gameplay_save_requests.py'),
        fixture_sha256=sha(ROOT/'tests/gameplay-save/SaveRequests.cs'),
        limitations=['Headless compiled gameplay; no GPU timing or managed stack/allocation sampling.',
                     'Timing values are observations, with no performance threshold or cross-platform timing equality claim.'])
    for name in ('hostfxr', 'bridge', 'assembly', 'native_descriptor'):
        path = getattr(args, name)
        if path:
            evidence[name+'_sha256'] = sha(path)
    session = None
    try:
        world, storage = run/'world.json', run/'saves'
        storage.mkdir()
        session = Session(args, world, evidence)
        session.author()
        authored_hash = sha(world)
        session.configure(storage)
        session.start()
        assert session.game()['module']['backend'] == evidence['backend']
        source_session = session.session
        session.arm(1, throw=1)
        original = session.game()
        capture = uuid.uuid4().hex
        session.rpc('profiler.start', dict(capture_id=capture, expected_capture_id=None, capacity=65536))
        step_request = dict(session_id=source_session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=2)
        session.step(request=step_request, error=-32040)
        assert session.rpc('runtime.status')['tick'] == 0 and session.game() == original
        assert inventory(storage) == {} and list(storage.iterdir()) == []
        evidence['checks'].append('Actual later Tick exception rolls back staged save, all gameplay state and tick; no storage side effects.')

        session.edit(ThrowTick=-1)
        saved, _ = session.step(request=step_request)
        operation = saved['save_operation']
        assert operation['state'] == 3 and operation['sequence'] == 1 and operation['generation'] == 1
        assert operation['requested_tick'] == 0 and operation['committed_tick'] == 2 and session.tick == 2
        disk = inventory(storage)
        assert disk
        session.step()
        assert session.values()['LastState'] == 3 and session.values()['TicketSequence'] == '0'
        evidence['checks'].append('Same failed request ID retries successfully; exactly one durable save and next-Tick completion.')

        session.arm(2, expected=1)
        loaded, request = session.step(3)
        destination_session = session.session
        assert loaded['runtime_replaced'] and loaded['save_operation']['state'] == 3
        assert source_session != destination_session and session.tick == 2 and loaded['tick'] == 6
        assert session.rpc('runtime.step', request) == dict(loaded, replayed=True)
        session.step()
        fields = session.values()
        assert fields['RestoreSeen'] == 1 and fields['RestoreInitiated'] == 1 and fields['TicketSequence'] == '0'
        assert fields['Requests'] == 1 and fields['LastState'] == 3
        session.step(3)
        assert session.values()['Requests'] == 1 and inventory(storage) == disk
        assert sha(world) == authored_hash
        evidence['checks'].append('Gameplay load replaces runtime identity; saved ticket resolves without reenqueuing or changing authored bytes.')

        status = session.rpc('profiler.stop', dict(capture_id=capture))
        assert status['state'] == 'stopped' and status['open'] == 0 and status['dropped'] == 0
        assert not status['clock_saturated']
        events, offset = [], 0
        while True:
            page = session.rpc('profiler.events', dict(capture_id=capture, offset=offset, limit=37))
            events.extend(page['events'])
            if page['next_offset'] is None:
                break
            offset = page['next_offset']
        assert len(events) == status['count']
        assert [event['id'] for event in events] == list(range(1, len(events)+1))
        by_id = {event['id']: event for event in events}
        for event in events:
            assert event['duration_ns'] >= 0 and event['start_ns'] >= 0
            if event['parent']:
                parent = by_id[event['parent']]
                assert parent['id'] < event['id'] and parent['kind'] == 'cpu'
                assert parent['start_ns'] <= event['start_ns']
                assert event['start_ns']+event['duration_ns'] <= parent['start_ns']+parent['duration_ns']
        assert any(event['name'] == 'runtime.gameplay.tick' and event['failed'] and event['tick'] == 1 and event['session'] == source_session for event in events)
        assert any(event['name'] == 'runtime.rollback' and not event['failed'] and event['session'] == source_session for event in events)
        assert any(event['name'] == 'runtime.batch' and event['failed'] and event['session'] == source_session for event in events)
        for name in ('save.service', 'save.snapshot', 'save.restore.prepare'):
            assert any(event['name'] == name and event['session'] == source_session for event in events), name
        assert any(event['name'] == 'runtime.tick' and event['tick'] == 2 and event['session'] == source_session for event in events)
        assert any(event['name'] == 'runtime.tick' and event['tick'] == 2 and event['session'] == destination_session for event in events)
        assert all(event['session'] in (None, source_session, destination_session) for event in events)
        evidence['checks'].append('Trace retains failed Tick and successful rollback; save phases and overlapping tick numbers remain distinguishable by source/destination session.')

        summary = session.rpc('profiler.summary', dict(capture_id=capture))
        gameplay_group = [group for group in summary['groups'] if group['name'] == 'runtime.gameplay.tick']
        assert sum(group['failed'] for group in gameplay_group) == 1
        exported = session.rpc('profiler.export', dict(capture_id=capture))
        trace_path = run/'trace.json'
        trace_path.write_text(json.dumps(exported['trace'], indent=2)+'\n', encoding='utf-8')
        parsed = json.loads(trace_path.read_text(encoding='utf-8'))
        cpu = [row for row in parsed['traceEvents'] if row['ph'] == 'X']
        assert len(cpu) == len([event for event in events if event['kind'] == 'cpu'])
        assert any(row['args']['session'] == destination_session for row in cpu)
        assert any(row['name'] == 'runtime.gameplay.tick' and row['args']['failed'] for row in cpu)
        assert inventory(storage) == disk and sha(world) == authored_hash
        evidence['checks'].append('Sealed event pagination, aggregate failure counts and parseable Chrome trace agree without further storage/world mutations.')
        evidence.update(passed=True, capture_id=capture, source_session=source_session, destination_session=destination_session,
                        trace_sha256=sha(trace_path), event_count=len(events), status=status, summary=summary,
                        authored_sha256=authored_hash, storage_inventory=disk)
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        if session is not None:
            try:
                session.close()
            except Exception as error:
                evidence['shutdown_error'] = repr(error)
                evidence['passed'] = False
                raise
            finally:
                (run/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
                print(run/'evidence.json')
        else:
            (run/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
            print(run/'evidence.json')


if __name__ == '__main__':
    main()
