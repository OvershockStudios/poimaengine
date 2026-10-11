#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compiled named actions through native Tick, rollback, reload and durable saves.

Builds nothing and opens no renderer/audio device. An external process-tree owner
must bound the invocation; the fixture bounds its RPC admission and owned close.
Native AOT remains process-pinned; compatible reload is checked only for CoreCLR.
"""
import argparse
import copy
import json
import queue
from pathlib import Path
import subprocess
import time
import uuid

from character_gameplay_contract import fixture
from components_gameplay_contract import Session, sha, uid

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedInputActionsGame'
IDENTITY = 'poima.test.managed-input-actions'
CAP = 32*1024*1024


def need(value, message):
    if not value:
        raise AssertionError(message)


def fresh():
    return uuid.uuid4().hex


def exact(left, right):
    return json.dumps(left, sort_keys=True, separators=(',', ':'), allow_nan=False) == json.dumps(
        right, sort_keys=True, separators=(',', ':'), allow_nan=False)


class OwnedSession(Session):
    def __init__(self, args, world, evidence):
        super().__init__(args, world, evidence)
        self.owner = dict(world=str(world), process_id=self.process.pid, exit_code=None,
                          forced_cleanup=False)
        evidence['owners'].append(self.owner)

    def rpc(self, method, params=None, error=False):
        need(time.monotonic() < self.args.deadline, 'Fixture deadline expired')
        request = dict(jsonrpc='2.0', id=len(self.evidence['calls'])+1,
                       method=method, params=copy.deepcopy(params or {}))
        row = dict(request=request)
        self.evidence['calls'].append(row)  # Preserve failed sends and replies too.
        self.process.stdin.write(json.dumps(request, allow_nan=False)+'\n')
        self.process.stdin.flush()
        try:
            line = self.output.get(timeout=min(20, max(.001, self.args.deadline-time.monotonic())))
        except queue.Empty:
            raise TimeoutError('Owned native RPC timed out') from None
        need(line is not None, 'Owned native process closed before its response')
        reply = json.loads(line)
        row['response'] = reply
        need(reply.get('id') == request['id'], 'RPC correlation differs')
        need(time.monotonic() <= self.args.deadline, 'RPC completed beyond fixture deadline')
        need(len(json.dumps(self.evidence, allow_nan=False).encode()) <= CAP, 'Evidence exceeds32MiB')
        if error:
            need('error' in reply, 'Expected native rejection')
            if type(error) is int:
                need(reply['error']['code'] == error, 'Unexpected native rejection code')
            return reply['error']
        need('result' in reply and 'error' not in reply, 'Native request failed: '+str(reply.get('error')))
        return reply['result']

    def close(self):
        if self.stderr.closed:
            return
        try:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.owner['forced_cleanup'] = True
                self.process.kill()
                self.process.wait(timeout=5)
            self.owner['exit_code'] = self.process.returncode
            need(not self.owner['forced_cleanup'] and self.process.returncode == 0,
                 'Native owner did not close normally')
        finally:
            self.stderr.close()
            self.owner['stderr_sha256'] = sha(self.world.with_suffix('.stderr'))
            need(self.world.with_suffix('.stderr').stat().st_size == 0, 'Native owner stderr is nonempty')


def scene():
    # Reuse the capsule/camera contract; put an independent static target on its
    # actual -Z ray, and a second capsule plus falling body off that ray.
    ops = [op for op in fixture(actor=False) if op['id'] != uid(2)]
    for op in ops:
        if op.get('type') == 'Transform' and op['id'] == uid(100):
            op['value']['position'] = [0, 1, 2]
    for identity, name, position, kind, value in (
        (300, 'Actual hitscan target', [0, 2.6, -4], 'BoxCollider',
         dict(half_extents=[.8, .8, .8], motion='static', mass=1, friction=.5, restitution=0)),
        (400, 'Independent capsule', [4, 1, 2], 'CharacterController',
         dict(radius=.3, height=1.8, speed=3, jump_speed=5, camera=None)),
        (500, 'Earlier physics rollback witness', [10, 10, 0], 'BoxCollider',
         dict(half_extents=[.5, .5, .5], motion='dynamic', mass=1, friction=.5, restitution=0))):
        ops += [dict(op='entity.create', id=uid(identity), name=name),
                dict(op='component.set', id=uid(identity), type='Transform', value=dict(
                    position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
                dict(op='component.set', id=uid(identity), type=kind, value=value)]
    return ops


def action(name, held=False, pressed=False, released=False):
    return dict(name=name, held=held, pressed=pressed, released=released)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if bool(args.native_descriptor) == bool(args.hostfxr or args.bridge or args.assembly):
        parser.error('Supply either CoreCLR paths or one native descriptor')
    if not args.native_descriptor and not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('CoreCLR requires --hostfxr --bridge --assembly')

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    args.native = native
    run = args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    args.deadline = time.monotonic()+180
    started = time.monotonic()
    files = {str(Path(__file__).resolve()): Path(__file__).resolve()}
    for path in (ROOT/'tests/character_gameplay_contract.py', ROOT/'tests/components_gameplay_contract.py',
                 ROOT/'tests/managed_input_actions_gameplay/ManagedInputActionsGame.cs',
                 ROOT/'tests/managed_input_actions_gameplay/Poima.ManagedInputActionsGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and
             metadata['services_version'] == 7 and metadata['minimum_services_bytes'] == 264 and
             'input_actions_v1' in metadata['required_features'], 'Wrong negotiated native action consumer')
        for row in metadata['files']:
            rel = Path(row['path']); path = (descriptor.parent/rel).resolve(strict=True)
            need(not rel.is_absolute() and '..' not in rel.parts and path.is_relative_to(descriptor.parent) and
                 path.is_file() and path.stat().st_size == row['size'] and sha(path) == row['sha256'],
                 'Native artifact closure differs')
            files[str(path)] = path
        image = (descriptor.parent/metadata['library']).resolve(strict=True)
        need(str(image) in files, 'Native image absent from artifact closure')
        config = dict(descriptor=native(descriptor), expected_descriptor_sha256=sha(descriptor))
    else:
        image = args.assembly.resolve()
        for owner in (args.assembly.resolve(), args.bridge.resolve()):
            sdk = owner.parent/'Poima.Gameplay.dll'
            need(sdk.is_file(), 'Consumer/bridge SDK missing'); files[str(sdk)] = sdk
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                dependency = owner.with_suffix(suffix)
                if dependency.is_file():
                    files[str(dependency)] = dependency
        config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    pins = {name: sha(path) for name, path in files.items()}
    evidence = dict(passed=False, calls=[], owners=[], checks=[], source_pins=pins,
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    limitations=['Synthetic compiled hitscan/ammo/ADS/reload logic; no window, physical input, GPU, audio, animation or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    try:
        world = run/'world.json'
        process = OwnedSession(args, world, evidence)
        authored = process.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene()))
        revision = authored['revision']; original = world.read_bytes()
        process.start(revision)
        loaded = process.load(config)
        need(loaded['module']['backend'] == evidence['backend'] and loaded['module']['assembly_sha256'] == sha(image),
             'Wrong compiled consumer/backend')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False), 'Native consumer unexpectedly supports dynamic code')
        saves = run/'saves'; saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        sequence = 0

        def values():
            return process.game()['module']['values']

        def snapshot(label):
            nonlocal sequence
            sequence += 1; slot = 'actions-'+str(sequence)
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision']))
            paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths) == 1 and paths[0].stat().st_size <= 16*1024*1024 and sha(paths[0]) == receipt['sha256'],
                 'Durable payload receipt differs')
            payload = json.loads(paths[0].read_text(encoding='utf-8'))
            evidence.setdefault('snapshots', []).append(dict(label=label, slot=slot, receipt=receipt,
                                                           snapshot=payload['snapshot']))
            return copy.deepcopy(payload['snapshot']), slot, receipt

        def step(ticks=1, records=None, entity=100, use=False, move=(0, 0), error=False, retry=False):
            row = dict(entity=uid(entity), move=list(move), use=use)
            if records is not None:
                row['actions'] = records
            request = dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick,
                           ticks=ticks, inputs=[row])
            result = process.rpc('runtime.step', request, error)
            if not error:
                process.tick = result['current_tick']; process.session = result['current_session_id']
                if retry:
                    before, _, _ = snapshot('before-step-retry')
                    need(process.rpc('runtime.step', request) == dict(result, replayed=True), 'Step retry receipt differs')
                    after, _, _ = snapshot('after-step-retry')
                    need(exact(before, after), 'Step retry reticked or changed full state')
            return result

        step(3, [action('aim', True, True), action('fire', True, True)], use=True, retry=True)
        state = values()
        need(all(state[name] == expected for name, expected in dict(Ticks=3, Shots=3, Hits=3, Ammo=0,
             AdsTicks=3, FirePressed=1, AimPressed=1, LegacyUses=1, TargetHeldTicks=0).items()), 'Held fire/ADS or first-tick edges/hits differ')
        step(2, [action('aim', True), action('fire', True)])
        state = values(); need(state['Shots'] == 3 and state['AdsTicks'] == 5 and state['FirePressed'] == 1,
                               'Held repeated without edge or exhausted ammo differs')
        step(records=[action('aim', released=True), action('fire', released=True), action('reload', pressed=True, released=True)])
        state = values(); need(state['Ammo'] == 3 and state['Reloads'] == 1 and state['FireReleased'] == state['AimReleased'] == 1,
                               'Release/reload quick tap differs')
        step(records=[action('fire', pressed=True, released=True)])
        state = values(); need(state['Shots'] == state['Hits'] == 4 and state['Ammo'] == 2 and state['FirePressed'] == state['FireReleased'] == 2,
                               'Quick tap must carry both edges and cause one real hit')
        before = values(); step(use=True); state = values()
        need(state['AbsentTicks'] == before['AbsentTicks']+1 and state['LegacyUses'] == before['LegacyUses']+1,
             'Missing named record or legacy Use differs')
        before = values(); step(2, [action('fire')]); state = values()
        need(state['AbsentTicks'] == before['AbsentTicks'] and state['Shots'] == before['Shots'], 'Present neutral action inferred activity or absence')
        before = values()
        other = process.rpc('runtime.entity', dict(session_id=process.session, tick=process.tick, id=uid(400)))
        step(records=[action('fire', True, True)], entity=400, move=(1, 0))
        moved = process.rpc('runtime.entity', dict(session_id=process.session, tick=process.tick, id=uid(400)))
        need(moved['world_matrix'][12] > other['world_matrix'][12] and values()['Shots'] == before['Shots'] and
             values()['TargetHeldTicks'] == 0, 'Actions leaked between independent controlled/live entities')
        evidence['checks'].append('Actual hitscan/ammo/held fire, ADS, quick tap, release/reload, neutral/absent records, independent capsule movement and old Use; successful retry never reticks')

        malformed = [None, {}, [action('Fire')], [action('1fire')], [action('f\0ire')],
                     [action('a'*65)], [action('fire'), action('fire')], [action('reload'), action('fire')],
                     [dict(name='fire', held=False, pressed=False)], [dict(action('fire'), extra=0)],
                     [dict(action('fire'), held=1)], [dict(action('fire'), released=None)],
                     [action('action.'+f'{i:02}') for i in range(33)]]
        for index, records in enumerate(malformed):
            before, _, _ = snapshot('before-malformed-'+str(index))
            request = dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick,
                           ticks=2, inputs=[dict(entity=uid(100), actions=records)])
            process.rpc('runtime.step', request, error=-32602)
            after, _, _ = snapshot('after-malformed-'+str(index))
            need(exact(after, before) and world.read_bytes() == original, 'Malformed action record changed complete native state')
        for mode, message in ((1, 'Action name'), (2, 'Runtime entity does not exist'), (3, '1024')):
            process.edit(Mode=mode)
            before, _, _ = snapshot('before-callback-'+str(mode))
            failure = step(records=[action('fire', True, True)], error=True)
            need(message.lower() in failure['message'].lower(), 'Wrong compiled action failure: '+failure['message'])
            after, _, _ = snapshot('after-callback-'+str(mode))
            need(exact(after, before), 'SDK/dead-target/quota failure did not roll back complete native state')
        process.edit(Mode=0, FailAt=str(process.tick+1))
        before, _, _ = snapshot('before-later-tick')
        failure = step(2, [action('fire', True, True)], error=True)
        need('later-tick rollback' in failure['message'], 'Later Tick fixture did not execute')
        after, _, _ = snapshot('after-later-tick')
        need(exact(after, before), 'Later Tick lost complete batch rollback including earlier physics and game state')
        process.edit(FailAt='-1'); step(records=[action('reload', pressed=True)])
        evidence['checks'].append('Malformed records reject before native mutation; SDK name, dead entity, 1024-read quota and actual second-Tick exception retain exact complete durable snapshots; valid Tick recovers')

        if not args.native_descriptor:
            before, _, _ = snapshot('before-compatible-reload')
            prior_game = process.game(); process.load(config)
            after, _, _ = snapshot('after-compatible-reload')
            # Reload changes the saved gameplay revision and therefore its payload
            # digest. Everything else, including all gameplay fields, stays exact.
            expected = copy.deepcopy(before)
            expected['payload']['gameplay_revision'] += 1
            expected['sha256'] = after['sha256']
            need(exact(after, expected) and after['sha256'] != before['sha256'] and
                 process.game()['revision'] == prior_game['revision']+1,
                 'Compatible CoreCLR reload lost complete logical state')
            migration = process.game()['module']['migration']
            need(migration['added'] == migration['removed'] == [] and
                 migration['preserved'] == sorted(prior_game['module']['values']), 'Reload failed field preservation')
            evidence['checks'].append('Actual same-schema CoreCLR reload preserves complete native checkpoint and every decision field')

        saved, slot, receipt = snapshot('fresh-source')
        step(3, [action('aim', True, True), action('fire', True, True)])
        continuation, _, _ = snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop', dict(session_id=process.session)); process.close(); process = None
        process = OwnedSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        new_session = fresh()
        restored = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=new_session, gameplay=config))
        process.session = new_session; process.tick = restored['tick']
        restored_snapshot, _, _ = snapshot('fresh-restored')
        need(exact(restored_snapshot, saved), 'Fresh native owner did not restore exact durable payload')
        step(3, [action('aim', True, True), action('fire', True, True)])
        continued_snapshot, _, _ = snapshot('fresh-actual-continuation')
        need(exact(continued_snapshot, continuation) and world.read_bytes() == original, 'Fresh actual compiled continuation diverged')
        source_paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
        need(len(source_paths) == 1 and sha(source_paths[0]) == receipt['sha256'], 'Original durable generation changed')
        process.rpc('runtime.stop', dict(session_id=process.session)); process.close(); process = None
        evidence['checks'].append('Fresh process restores full logical payload and original decision fields; three real continued Ticks exactly match the prior owner, original authored bytes and save generation remain immutable')
        need(all(sha(path) == pins[name] for name, path in files.items()), 'Consumed source/image closure changed')
        need(time.monotonic() < args.deadline, 'Final whole fixture deadline expired')
        evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        if process:
            try:
                process.close()
            except BaseException as error:
                evidence['close_error'] = repr(error); evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-started
        evidence['rpc_count'] = len(evidence['calls'])
        data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode()
        need(len(data) <= CAP, 'Final evidence exceeds32MiB')
        (run/'evidence.json').write_bytes(data)
        if time.monotonic() > args.deadline:
            evidence['passed'] = False; evidence['deadline_error'] = 'Final publication exceeded180s'
            (run/'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            raise TimeoutError(evidence['deadline_error'])
        print(run/'evidence.json', flush=True)
        if 'close_error' in evidence and 'error' not in evidence:
            raise RuntimeError(evidence['close_error'])


if __name__ == '__main__':
    main()
