#!/usr/bin/env python3
"""Real CoreCLR/NativeAOT Tick-driven save requests through their owning WorldSession."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
TYPE = 'Poima.Tests.GameplaySaveProbe'
LONG_FIELDS = {'TriggerTick', 'ThrowTick', 'ExpectedGeneration', 'TicketHigh', 'TicketLow', 'TicketSequence',
               'EpochHigh', 'EpochLow', 'LastRequestedTick', 'LastCommittedTick', 'LastGeneration', 'RestoredTick'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inventory(root):
    return {str(path.relative_to(root)): sha(path) for path in sorted(root.rglob('*')) if path.is_file()}


class Session:
    def __init__(self, args, world, evidence):
        self.args, self.world, self.evidence = args, world, evidence
        self.stderr = world.with_suffix('.stderr').open('a', encoding='utf-8')
        self.process = subprocess.Popen([str(args.binary.resolve()), 'world', args.native(world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        threading.Thread(target=read, daemon=True).start()
        self.session, self.tick = None, 0

    def rpc(self, method, params=None, error=None):
        request = dict(jsonrpc='2.0', id=len(self.evidence['calls'])+1, method=method, params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n'); self.process.stdin.flush()
        line = self.lines.get(timeout=90)
        assert line is not None, self.world.with_suffix('.stderr').read_text(encoding='utf-8')
        response = json.loads(line)
        self.evidence['calls'].append(dict(request=request, response=response))
        assert response.get('id') == request['id'], response
        if error is not None:
            assert response.get('error', {}).get('code') == error, response
            return response['error']
        assert 'result' in response, response
        return response['result']

    def author(self):
        fixture = json.loads((ROOT/'examples/interaction-room.jsonl').read_text())
        return self.rpc(fixture['method'], fixture['params'])

    def start(self):
        self.session, self.tick = uuid.uuid4().hex, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=1))
        method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
        self.rpc(method, dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=0, expected_revision=0, **self.args.config))

    def configure(self, root):
        state = self.rpc('save.status')
        self.rpc('save.configure', dict(request_id=uuid.uuid4().hex,
            expected_generation=state['generation'], root=self.args.native(root)))

    def game(self):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))

    def values(self):
        return self.game()['module']['values']

    def edit(self, **values):
        observed = self.game()
        values = {key: str(value) if key in LONG_FIELDS else value for key, value in values.items()}
        return self.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=observed['revision'], values=values))

    def arm(self, mode, expected=-1, throw=-1):
        self.edit(Mode=mode, TriggerTick=self.tick, ExpectedGeneration=expected, ThrowTick=throw)

    def step(self, ticks=1, request=None, error=None):
        request = request or dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, ticks=ticks)
        result = self.rpc('runtime.step', request, error)
        if error is None:
            assert result['session_id'] == request['session_id']
            assert result['tick'] == request['expected_tick']+request['ticks']
            assert result['stepped'] == request['ticks']
            self.session, self.tick = result['current_session_id'], result['current_tick']
        return result, request

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=20)
            assert self.process.returncode == 0, self.process.returncode
        finally:
            if self.process.poll() is None:
                self.process.kill(); self.process.wait(timeout=10)
            self.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for option in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+option, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if args.native_descriptor:
        if any((args.hostfxr, args.bridge, args.assembly)):
            parser.error('Use NativeAOT descriptor or CoreCLR paths, not both.')
    elif not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('Supply --native-descriptor or --hostfxr, --bridge, --assembly.')
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value
    args.native = native
    args.config = dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(
        hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(args.assembly), type=TYPE)
    run = args.output.resolve()/uuid.uuid4().hex; run.mkdir(parents=True)
    evidence = dict(passed=False, backend='native_aot' if args.native_descriptor else 'coreclr',
        binary_sha256=sha(args.binary), test_sha256=sha(__file__), fixture_sha256=sha(ROOT/'tests/gameplay-save/SaveRequests.cs'),
        calls=[], checks=[], limitations=['Headless real gameplay and durable storage; no GUI, graphics, physical input or power-loss qualification.',
                                        'External save.load is used only for the separate fresh-process expired-token test. Main save/load requests originate in Tick.'])
    for name in ('hostfxr', 'bridge', 'assembly', 'native_descriptor'):
        if getattr(args, name): evidence[name+'_sha256'] = sha(getattr(args, name))
    session = None
    try:
        world = run/'world.json'; storage = run/'saves'; storage.mkdir()
        session = Session(args, world, evidence); session.author(); authored_hash = sha(world); session.start()
        module = session.game()['module']
        assert module['backend'] == ('native_aot' if args.native_descriptor else 'coreclr'), module['backend']
        session.arm(1); disabled, _ = session.step()
        assert not disabled['save_serviced'] and session.values()['Rejection'] == 1 and session.values()['Enabled'] == 0
        assert inventory(storage) == {}
        capabilities = session.rpc('runtime.save.status', dict(session_id=session.session))
        assert not capabilities['enabled'] and capabilities['pending'] is None and capabilities['last_restore'] is None
        session.configure(storage)
        session.arm(3); invalid, _ = session.step()
        assert not invalid['save_serviced'] and session.values()['Rejection'] == 3
        assert inventory(storage) == {}
        evidence['checks'].append('Disabled and invalid requests reject in actual gameplay without storage writes.')

        before = session.tick; session.arm(1)
        saved, save_request = session.step(5)
        operation = saved['save_operation']; saved_tick = before+5
        assert saved['save_serviced'] and not saved['runtime_replaced'] and operation['state'] == 3
        assert operation['requested_tick'] == before and operation['committed_tick'] == saved_tick
        assert operation['generation'] == 1
        fields = session.values()
        assert fields['Ticks'] == saved_tick and fields['QueuedState'] == 1 and fields['BusyRejection'] == 2
        assert fields['TicketSequence'] == '1'
        disk = inventory(storage)
        ticket = dict(epoch=operation['epoch'], sequence=operation['sequence'])
        hidden_storage = storage.with_name('saves-temporarily-unavailable')
        storage.rename(hidden_storage)
        try:
            assert session.rpc('runtime.save.result', ticket) == operation
            assert session.rpc('runtime.save.result', ticket) == operation
            capabilities = session.rpc('runtime.save.status', dict(session_id=session.session))
            assert capabilities['enabled'] and capabilities['pending'] is None and capabilities['epoch'] == operation['epoch']
            assert session.rpc('runtime.save.result', dict(epoch=uuid.uuid4().hex, sequence=1))['state'] == 0
            session.rpc('runtime.save.result', dict(epoch=operation['epoch'], sequence=0), error=-32602)
        finally:
            hidden_storage.rename(storage)
        assert inventory(storage) == disk
        assert session.rpc('runtime.step', save_request) == dict(saved, replayed=True)
        assert inventory(storage) == disk
        session.step(); fields = session.values()
        assert fields['LastState'] == 3 and fields['TicketSequence'] == '0'
        assert fields['LastRequestedTick'] == str(before) and fields['LastCommittedTick'] == str(saved_tick)
        evidence['checks'].append('Save queues within Tick, captures whole-batch boundary, reports completion next Tick; memory-only queries work with absent storage and exact retry does not rewrite.')

        session.arm(1, expected=0); failed, _ = session.step()
        assert failed['save_serviced'] and failed['save_operation']['state'] == 4 and failed['save_operation']['error_code'] == -32009
        assert inventory(storage) == disk
        session.step(); assert session.values()['LastState'] == 4 and session.values()['LastError'] == -32009
        session.arm(4); missing, _ = session.step()
        assert missing['save_operation']['state'] == 4 and not missing['runtime_replaced']
        assert not (storage/'slot-missing').exists()
        session.step()
        evidence['checks'].append('Storage CAS and missing-load errors leave committed simulation active and report failed operations.')

        session.arm(2, expected=1); source_session = session.session; source_tick = session.tick
        loaded, load_request = session.step(3)
        assert loaded['runtime_replaced'] and loaded['save_serviced'] and loaded['tick'] == source_tick+3
        assert session.session != source_session and session.tick == saved_tick
        assert loaded['save_operation']['state'] == 3 and loaded['save_operation']['restored_tick'] == saved_tick
        live_saves = session.rpc('runtime.save.status', dict(session_id=session.session))
        assert live_saves['epoch'] == loaded['save_operation']['restored_epoch']
        assert live_saves['last_restore']['initiating_epoch'] == loaded['save_operation']['epoch']
        assert live_saves['last_restore']['source_tick'] == loaded['tick'] and live_saves['last_restore']['restored_tick'] == saved_tick
        load_ticket = dict(epoch=loaded['save_operation']['epoch'], sequence=loaded['save_operation']['sequence'])
        assert session.rpc('runtime.save.result', load_ticket) == loaded['save_operation']
        assert session.values()['Ticks'] == saved_tick and session.values()['TicketSequence'] == '1'
        assert session.rpc('runtime.step', load_request) == dict(loaded, replayed=True)
        assert session.rpc('runtime.status')['tick'] == saved_tick and inventory(storage) == disk
        different = copy.deepcopy(load_request); different['ticks'] += 1
        session.rpc('runtime.step', different, error=-32010)
        session.step(); fields = session.values()
        assert fields['RestoreSeen'] == 1 and fields['RestoreInitiated'] == 1 and fields['RestoredTick'] == str(saved_tick)
        assert fields['TicketSequence'] == '0' and fields['LastState'] == 3 and fields['Requests'] == 1
        session.step(4); assert inventory(storage) == disk and session.values()['Requests'] == 1
        session.rpc('runtime.stop', dict(session_id=session.session))
        assert session.rpc('runtime.step', load_request) == dict(loaded, replayed=True)
        assert not session.rpc('runtime.status')['active']
        assert session.rpc('runtime.save.result', load_ticket) == loaded['save_operation']
        session.start(); new_active = session.session
        assert session.rpc('runtime.step', load_request) == dict(loaded, replayed=True)
        active = session.rpc('runtime.status')
        assert active['session_id'] == new_active and active['tick'] == 0
        assert sha(world) == authored_hash
        evidence['checks'].append('Gameplay load replaces epoch/session; old advance retry survives load, stop and restart; saved pending token resolves without reenqueue.')
        session.close(); session = None

        session = Session(args, world, evidence); session.configure(storage)
        assert session.rpc('runtime.save.result', ticket)['state'] == 0
        new_session = uuid.uuid4().hex
        restored = session.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
            slot='quick', expected_generation=1, revision=1, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=new_session, gameplay=args.config))
        session.session, session.tick = new_session, restored['tick']
        assert session.values()['TicketSequence'] == '1'
        session.step(); fields = session.values()
        assert fields['LastState'] == 0 and fields['ExpiredSeen'] == 1 and fields['TicketSequence'] == '0'
        assert fields['RestoreSeen'] == 1 and fields['RestoreInitiated'] == 0
        session.step(3); assert fields['Requests'] == session.values()['Requests'] == 1 and inventory(storage) == disk
        evidence['checks'].append('Fresh-process restore expires saved old ticket and continues without a pending loop or new save.')
        session.close(); session = None

        rollback_storage = run/'rollback-saves'; rollback_storage.mkdir()
        session = Session(args, run/'rollback-world.json', evidence); session.author(); session.configure(rollback_storage); session.start()
        session.arm(1, throw=1)
        original = session.game()
        request = dict(session_id=session.session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=2)
        session.step(request=request, error=-32040)
        assert session.rpc('runtime.status')['tick'] == 0 and session.game() == original
        assert inventory(rollback_storage) == {} and list(rollback_storage.iterdir()) == []
        session.edit(ThrowTick=-1)
        retry, _ = session.step(request=request)
        assert retry['save_operation']['state'] == 3 and retry['save_operation']['sequence'] == 1
        assert retry['save_operation']['generation'] == 1 and retry['save_operation']['committed_tick'] == 2
        assert session.values()['Requests'] == 1 and session.values()['Ticks'] == 2
        evidence['checks'].append('Later Tick failure rolls back state, queue and ticket allocation with zero slot files; retry commits exactly one save.')
        session.close(); session = None
        evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        if session is not None:
            try: session.close()
            except Exception as error: evidence['shutdown_error'] = repr(error)
        (run/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
        print(run/'evidence.json')


if __name__ == '__main__':
    main()
