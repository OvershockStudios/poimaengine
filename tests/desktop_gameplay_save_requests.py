#!/usr/bin/env python3
"""Headless Windows desktop-clock qualification of real C# save/load requests."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid
from gameplay_save_requests import LONG_FIELDS, TYPE, sha, inventory


def check(value, message):
    if not value:
        raise AssertionError(message)


class Desktop:
    def __init__(self, args, world, record):
        self.args, self.record, self.request_id = args, record, 0
        self.endpoint = 'game-saves-' + uuid.uuid4().hex
        self.search = os.add_dll_directory(str(args.bridge.resolve().parent))
        self.lib = ctypes.CDLL(str(args.bridge.resolve()))
        pointer, text = ctypes.c_void_p, ctypes.c_char_p
        for name, arguments, returned in [
            ('create', [text, text, ctypes.c_int32, ctypes.c_uint32], pointer),
            ('call', [pointer, text], pointer), ('poll', [pointer], pointer),
            ('error', [pointer], pointer), ('destroy', [pointer], None)]:
            function = getattr(self.lib, 'poima_desktop_' + name)
            function.argtypes, function.restype = arguments, returned
        self.host = self.lib.poima_desktop_create(str(world).encode(), self.endpoint.encode(), -1, 1)
        check(self.host, self.text(self.lib.poima_desktop_error(None)))

    @staticmethod
    def text(pointer):
        return ctypes.string_at(pointer).decode('utf-8') if pointer else ''

    def rpc(self, method, params=None, error=None):
        self.request_id += 1
        request = dict(jsonrpc='2.0', id=self.request_id, method=method, params=params or {})
        pointer = self.lib.poima_desktop_call(self.host, json.dumps(request).encode())
        check(pointer, self.text(self.lib.poima_desktop_error(self.host)))
        reply = json.loads(self.text(pointer))
        check(reply.get('id') == request['id'], reply)
        self.record['calls'].append(dict(transport='C ABI', request=request, reply=reply))
        if error is not None:
            check(reply.get('error', {}).get('code') == error, reply)
            return reply['error']
        check('result' in reply, reply)
        return reply['result']

    def poll(self):
        pointer = self.lib.poima_desktop_poll(self.host)
        check(pointer, self.text(self.lib.poima_desktop_error(self.host)))
        state = json.loads(self.text(pointer))
        self.record['polls'].append(state)
        return state

    def wait(self, predicate, description):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            time.sleep(.025)
            state = self.poll()
            if predicate(state):
                return state
            check(state['playback']['last_error'] is None, state['playback'])
        raise AssertionError('Timed out waiting for ' + description)

    def ipc_guards(self):
        requests = [dict(jsonrpc='2.0', id=i+1, method=method, params={}) for i, method in
                    enumerate(['save.configure', 'save.write', 'save.load', 'save.status'])]
        def communicate():
            process = subprocess.run([str(self.args.binary.resolve()), 'connect', self.endpoint, '--timeout-ms', '4000'],
                input=''.join(json.dumps(r)+'\n' for r in requests), capture_output=True,
                text=True, encoding='utf-8', timeout=10)
            check(process.returncode == 0, process.stdout + process.stderr)
            return [json.loads(line) for line in process.stdout.splitlines()]
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(communicate)
            deadline = time.monotonic()+12
            while not future.done() and time.monotonic() < deadline:
                self.poll()
                time.sleep(.005)
            check(future.done(), 'Local IPC did not finish.')
            replies = future.result()
        check(len(replies) == len(requests), replies)
        for index, (request, reply) in enumerate(zip(requests, replies)):
            check(reply.get('id') == request['id'], reply)
            self.record['calls'].append(dict(transport='local IPC', request=request, reply=reply))
            check(reply.get('error', {}).get('code') == -32009 if index < 3 else reply.get('result', {}).get('generation') == 1, reply)

    def gameplay(self, sid):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=sid, include_schema=True))

    def edit(self, sid, **values):
        seen = self.gameplay(sid)
        values = {key: str(value) if key in LONG_FIELDS else value for key, value in values.items()}
        return self.rpc('runtime.gameplay.edit', dict(session_id=sid, request_id=uuid.uuid4().hex,
            expected_tick=seen['tick'], expected_revision=seen['revision'], values=values))

    def close(self):
        if self.host:
            self.lib.poima_desktop_destroy(self.host)
            self.host = None
        self.search.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bridge', 'binary', 'hostfxr', 'gameplay-bridge', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--headless', action='store_true', help='Required: no HWND, graphics, GUI or OS input qualification.')
    args = parser.parse_args()
    if os.name != 'nt' or not args.headless:
        parser.error('Use native Windows Python with --headless; this test never creates a window.')
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    record = dict(success=False, checks=[], calls=[], polls=[], hashes={
        name: sha(getattr(args, name)) for name in ('bridge', 'binary', 'hostfxr', 'gameplay_bridge', 'assembly')},
        test_sha256=sha(__file__), fixture_sha256=sha(Path(__file__).parent/'gameplay-save/SaveRequests.cs'),
        limitations=['Native C ABI and local IPC with real CoreCLR gameplay and storage.',
                    'No GPU, HWND, Avalonia widgets, physical input/cursor or power-loss qualification.',
                    'Wall-clock polling verifies bounded automatic stepping; no frame-rate/performance claim.'])
    desktop = None
    try:
        project = run/'Project'
        created = subprocess.run([str(args.binary.resolve()), 'project', 'create', str(project), '--name', 'Gameplay save desktop clock'],
            capture_output=True, text=True, encoding='utf-8', timeout=30)
        check(created.returncode == 0, created.stdout + created.stderr)
        manifest = json.loads((project/'project.json').read_text(encoding='utf-8'))
        world = project/manifest['entry']['world']
        controller, camera = manifest['entry']['controller'], manifest['entry']['camera']
        source_hash = sha(world)
        storage = run/'External saves'; storage.mkdir()
        desktop = Desktop(args, world, record)
        revision = desktop.rpc('world.inspect')['revision']
        history = desktop.rpc('world.history')
        desktop.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(storage)))
        profile = dict(hostfxr=str(args.hostfxr.resolve()), bridge=str(args.gameplay_bridge.resolve()),
            assembly=str(args.assembly.resolve()), type=TYPE, values={'Mode': 1, 'TriggerTick': '0', 'ExpectedGeneration': '0'})
        desktop.rpc('desktop.gameplay.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, profile=profile))
        sid = uuid.uuid4().hex
        desktop.rpc('desktop.play.start', dict(session_id=sid, revision=revision, expected_gameplay_generation=1, paused=True))
        check(desktop.gameplay(sid)['module']['backend'] == 'coreclr', 'Wrong fixture backend.')
        check(not list(storage.iterdir()), 'Configuration/Initialize touched storage.')
        desktop.rpc('desktop.play.resume', dict(session_id=sid))
        first = desktop.wait(lambda s: s['runtime']['tick'] > 0, 'automatic Tick-originated save')
        values = desktop.gameplay(sid)['module']['values']
        status = desktop.rpc('runtime.save.status', dict(session_id=sid))
        ticket = dict(epoch=status['epoch'], sequence=int(values['TicketSequence']))
        operation = desktop.rpc('runtime.save.result', ticket)
        saved_tick = operation['committed_tick']
        check(operation['state'] == 3 and operation['kind'] == 1 and operation['generation'] == 1, operation)
        check(operation['requested_tick'] == 0 and 1 <= saved_tick <= 8 and first['runtime']['tick'] == saved_tick, operation)
        check(values['Requests'] == 1 and values['QueuedState'] == 1 and values['BusyRejection'] == 2 and status['pending'] is None, values)
        check(first['playback']['state'] == 'playing', 'Save unexpectedly paused the automatic clock.')
        slot = desktop.rpc('save.inspect', dict(slot='quick'))
        check(slot['current']['generation'] == 1 and slot['current']['verified'], slot)
        store_hashes = inventory(storage)
        record['checks'].append('Automatic playback executes real Tick save after its bounded whole batch; memory result identifies requested and committed ticks.')

        for method in ('save.configure', 'save.write', 'save.load'):
            desktop.rpc(method, {}, error=-32009)
        desktop.ipc_guards()
        check(inventory(storage) == store_hashes, 'Rejected external mutations changed storage.')
        record['checks'].append('External save mutations remain rejected while playing through both direct C ABI and local IPC.')
        desktop.wait(lambda s: s['runtime']['tick'] > saved_tick, 'next callback observing save completion')
        values = desktop.gameplay(sid)['module']['values']
        check(values['LastState'] == 3 and values['TicketSequence'] == '0' and int(values['LastCommittedTick']) == saved_tick, values)
        check(desktop.rpc('runtime.save.result', ticket) == operation, 'Repeated memory query changed the terminal operation.')
        record['checks'].append('A later real game callback observes terminal completion and clears its pending state without repeating the write.')

        desktop.rpc('desktop.play.pause', dict(session_id=sid))
        source_tick = desktop.rpc('desktop.play.inspect')['tick']
        check(source_tick > saved_tick, 'Load fixture did not advance beyond the save.')
        desktop.edit(sid, Mode=2, TriggerTick=source_tick, ExpectedGeneration=1)
        desktop.rpc('desktop.view', dict(mode='game', camera=camera))
        desktop.rpc('desktop.input.configure', dict(session_id=sid, controller=controller))
        desktop.rpc('desktop.play.resume', dict(session_id=sid))
        desktop.rpc('desktop.input.focus', dict(session_id=sid, focused=True))
        desktop.rpc('desktop.input.events', dict(session_id=sid, request_id=uuid.uuid4().hex,
            events=[{'control': 'key.w', 'down': True}, {'motion': [5, -2]}]))
        check(desktop.rpc('desktop.input.inspect')['focused'], 'Protocol input fixture failed to engage.')
        after = desktop.wait(lambda s: s['runtime']['session_id'] != sid, 'automatic Tick-originated load')
        fresh = after['runtime']['session_id']
        check(after['runtime']['tick'] == saved_tick and after['playback']['state'] == 'paused', after['playback'])
        check(after['playback']['last_error'] is None and after['playback']['dropped_seconds'] == 0, after['playback'])
        check(not after['input']['configured'] and not after['input']['focused'] and after['input']['accepted_batches'] == 0, after['input'])
        check(after['gameplay']['runtime']['session_id'] == fresh, 'Gameplay metadata retained the old session.')
        restored = desktop.rpc('runtime.save.status', dict(session_id=fresh))
        restore = restored['last_restore']
        check(restored['epoch'] != ticket['epoch'] and restore['restored_tick'] == saved_tick and source_tick < restore['source_tick'] <= source_tick+8, restore)
        load_ticket = dict(epoch=restore['initiating_epoch'], sequence=restore['initiating_sequence'])
        loaded = desktop.rpc('runtime.save.result', load_ticket)
        check(loaded['state'] == 3 and loaded['kind'] == 2 and loaded['restored_epoch'] == restored['epoch'], loaded)
        time.sleep(.08)
        check(desktop.poll()['runtime']['tick'] == saved_tick, 'Restored paused runtime consumed leftover catch-up ticks.')
        check(inventory(storage) == store_hashes and sha(world) == source_hash and desktop.rpc('world.history') == history, 'Gameplay restore changed authored world/history or slot bytes.')
        record['checks'].append('Automatic game load adopts a fresh paused session, clears native input/clock/module caches and leaves authoring and save bytes unchanged.')

        stepped = desktop.rpc('desktop.play.step', dict(session_id=fresh, request_id=uuid.uuid4().hex, expected_tick=saved_tick, ticks=1))
        check(not stepped['runtime_replaced'] and stepped['current_session_id'] == fresh, stepped)
        values = desktop.gameplay(fresh)['module']['values']
        check(values['RestoreSeen'] == 1 and values['RestoreInitiated'] == 1 and values['TicketSequence'] == '0' and values['Requests'] == 1, values)
        desktop.edit(fresh, Mode=2, TriggerTick=saved_tick+1, ExpectedGeneration=1)
        command = dict(session_id=fresh, request_id=uuid.uuid4().hex, expected_tick=saved_tick+1, ticks=3)
        replaced = desktop.rpc('desktop.play.step', command)
        second = replaced['current_session_id']
        check(replaced['runtime_replaced'] and replaced['session_id'] == fresh and second != fresh and
              replaced['tick'] == saved_tick+4 and replaced['current_tick'] == saved_tick, replaced)
        check(desktop.rpc('desktop.play.inspect')['session_id'] == second, 'Paused step did not adopt its replacement.')
        desktop.rpc('desktop.input.configure', dict(session_id=second, controller=controller))
        check(desktop.rpc('desktop.play.step', command) == dict(replaced, replayed=True), 'Exact paused-step retry lost its replacement outcome.')
        check(desktop.rpc('desktop.input.inspect')['configured'], 'Exact retry repeated session activation/input clearing.')
        check(inventory(storage) == store_hashes and sha(world) == source_hash, 'Continuation/retry changed stored checkpoint or world.')
        record['checks'].append('Paused step exposes source/current replacement metadata; exact old-session retry is inert and restored saved tokens do not reenact commands.')
        record.update(success=True, saved_tick=saved_tick, load_source_tick=restore['source_tick'],
            first_session=sid, restored_session=fresh, second_restored_session=second,
            authored_sha256=source_hash, final_state=desktop.rpc('desktop.inspect'))
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if desktop:
            desktop.close()
        record['rpc_count'] = len(record['calls']); record['poll_count'] = len(record['polls'])
        evidence = run/'evidence.json'
        evidence.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(success=record['success'], checks=len(record['checks']), calls=record['rpc_count'], evidence=str(evidence))))
    if not record['success']:
        print(record.get('error', 'Desktop gameplay save qualification failed.'), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
