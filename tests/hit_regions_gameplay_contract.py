#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled hit-region queries, bone pose visibility, rollback and saves.

Consumes a supplied CoreCLR assembly or Native AOT descriptor; builds nothing.
Run beneath an external process-tree owner. No renderer, audio or timing claim.
"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import time

from input_actions_gameplay_contract import OwnedSession, exact, fresh, need
from components_gameplay_contract import sha, uid
from shadow_controls_capture import skinned_quad
from gltf_fixture import glb

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedHitRegionsGame'
IDENTITY = 'poima.test.managed-hit-regions'
CAP = 32*1024*1024
RIG = 'a'*32


def scene():
    ops = []
    def entity(identity, name, position, parent=None):
        row = dict(op='entity.create', id=uid(identity), name=name)
        if parent:
            row['parent'] = uid(parent)
        ops.extend([row, dict(op='component.set', id=uid(identity), type='Transform',
                             value=dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]))])
    def collider(identity, motion='static'):
        ops.append(dict(op='component.set', id=uid(identity), type='BoxCollider', value=dict(
            half_extents=[.5]*3, motion=motion, mass=1, friction=.5, restitution=0)))
    entity(300, 'Actor with original physics proxy', [0, 0, -2]);collider(300)
    for identity, name, position, shape, dimensions in (
        (301, 'head', [0, 0, -2], 'sphere', dict(radius=.5)),
        (302, 'torso', [2, 0, -2], 'box', dict(half_extents=[.5]*3)),
        (303, 'limb', [-2, 0, -2], 'capsule', dict(radius=.5, half_height=.5))):
        entity(identity, name, position, 300)
        ops.append(dict(op='component.set', id=uid(identity), type='HitRegion', value=dict(
            owner=uid(300), name=name, shape=shape, **dimensions)))
    for identity, name, position, motion in (
        (400, 'Independent geometry', [6, 0, -4], 'static'),
        (500, 'Geometry behind ignored regions', [0, 0, -8], 'static'),
        (600, 'Earlier physics rollback witness', [15, 10, 0], 'dynamic')):
        entity(identity, name, position);collider(identity, motion)
    return ops


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
    run = args.output.resolve();run.mkdir(parents=True, exist_ok=False)
    args.deadline = time.monotonic()+180;started = time.monotonic()
    files = {str(Path(__file__).resolve()): Path(__file__).resolve()}
    for path in (ROOT/'tests/input_actions_gameplay_contract.py', ROOT/'tests/character_gameplay_contract.py',
                 ROOT/'tests/components_gameplay_contract.py', ROOT/'tests/shadow_controls_capture.py',
                 ROOT/'tests/texture_fixture.py', ROOT/'tests/gltf_fixture.py', ROOT/'tests/scene_capture.py', ROOT/'managed/Directory.Build.props',
                 ROOT/'tests/managed_hit_regions_gameplay/ManagedHitRegionsGame.cs',
                 ROOT/'tests/managed_hit_regions_gameplay/Poima.ManagedHitRegionsGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 272 and metadata['required_features'] == ['baseline_v7', 'hit_regions_v1'],
             'Wrong negotiated native hit consumer')
        for row in metadata['files']:
            rel = Path(row['path']);path = (descriptor.parent/rel).resolve(strict=True)
            need(not rel.is_absolute() and '..' not in rel.parts and path.is_relative_to(descriptor.parent) and
                 path.is_file() and path.stat().st_size == row['size'] and sha(path) == row['sha256'], 'Native artifact closure differs')
            files[str(path)] = path
        image = (descriptor.parent/metadata['library']).resolve(strict=True)
        need(str(image) in files, 'Native image absent from artifact closure')
        config = dict(descriptor=native(descriptor), expected_descriptor_sha256=sha(descriptor))
    else:
        image = args.assembly.resolve()
        for owner in (image, args.bridge.resolve()):
            sdk = owner.parent/'Poima.Gameplay.dll';need(sdk.is_file(), 'Consumer/bridge SDK missing');files[str(sdk)] = sdk
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                dependency = owner.with_suffix(suffix)
                if dependency.is_file():
                    files[str(dependency)] = dependency
        config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    for path in (ROOT/'tools/python/poima_client').rglob('*'):
        if path.is_file() and path.suffix in ('.py', '.json'):
            files[str(path.resolve())] = path.resolve()
    pins = {name: sha(path) for name, path in files.items()}
    evidence = dict(passed=False, calls=[], owners=[], checks=[], snapshots=[], source_pins=pins,
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    limitations=['Synthetic compiled query decisions; no GPU, physical input, audio, damage or performance claim.',
                                 'Native AOT is process-pinned; only CoreCLR checks compatible reload.'])
    process = None
    try:
        world = run/'world.json';process = OwnedSession(args, world, evidence)
        revision = process.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene()))['revision']
        source = run/'analytic-skin.glb';source.write_bytes(glb(*skinned_quad()))
        evidence['analytic_source_sha256'] = sha(source)
        asset = process.rpc('asset.import', dict(source=native(source)))['asset']
        revision = process.rpc('world.transact', dict(request_id=fresh(), base_revision=revision, ops=[
            dict(op='asset.instantiate', id=RIG, asset=asset, name='Compiled bone attachment'),
            dict(op='component.set', id=RIG, type='Transform', value=dict(position=[20, 0, -4], rotation=[0, 0, 0, 1], scale=[1, 1, 1]))]))['revision']
        nodes = process.rpc('entity.query', dict(component='RigNode'))['entities'];joint = None
        for row in nodes:
            value = process.rpc('entity.get', dict(id=row['id']))['value']['components']['RigNode']
            if value['rig'] == RIG and value['node'] == 1:
                need(joint is None, 'Duplicate returned animated node mapping');joint = row['id']
        need(joint is not None, 'Actual instantiated joint mapping missing')
        revision = process.rpc('world.transact', dict(request_id=fresh(), base_revision=revision, ops=[
            dict(op='component.set', id=joint, type='HitRegion', value=dict(owner=RIG, name='animated', shape='sphere', radius=.025))]))['revision']
        evidence['animated_attachment'] = dict(asset=asset, rig=RIG, joint=joint)
        source.unlink()  # Cooked query/restore must not need the authoring GLB.
        original = world.read_bytes();process.start(revision);loaded = process.load(config)
        need(loaded['module']['backend'] == evidence['backend'] and loaded['module']['assembly_sha256'] == sha(image), 'Wrong compiled consumer/backend')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False), 'Native consumer supports dynamic code')
        saves = run/'saves';saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        sequence = 0
        def values():
            return process.game()['module']['values']
        def snapshot(label):
            nonlocal sequence
            sequence += 1;slot = 'hits-'+str(sequence)
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision']))
            paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths) == 1 and paths[0].stat().st_size <= 16*1024*1024 and sha(paths[0]) == receipt['sha256'], 'Durable payload receipt differs')
            payload = json.loads(paths[0].read_text(encoding='utf-8'))
            evidence['snapshots'].append(dict(label=label, slot=slot, receipt=receipt, snapshot=payload['snapshot']))
            return copy.deepcopy(payload['snapshot']), slot, receipt
        def step(ticks=1, error=False, animations=None, retry=False):
            request = dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick, ticks=ticks)
            if animations is not None:
                request['animations'] = animations
            result = process.rpc('runtime.step', request, error)
            if not error:
                process.tick = result['current_tick'];process.session = result['current_session_id']
                if retry:
                    before, _, _ = snapshot('before-step-retry')
                    need(process.rpc('runtime.step', request) == dict(result, replayed=True), 'Step retry differs')
                    after, _, _ = snapshot('after-step-retry');need(exact(before, after), 'Step retry changed full state')
            return result
        step(2, retry=True)
        need(all(values()[name] == expected for name, expected in dict(Ticks=2, Queries=16, RegionHits=8, GeometryHits=6, Misses=2, LegacyHits=2).items()), 'Compiled query decisions differ')
        process.edit(Mode=4);before = values();step(2)
        need(values()['Queries'] == before['Queries']+2048, 'Exact1024 calls or per-Tick budget reset failed')
        process.edit(Mode=0)
        for mode, message in ((1, 'direction'), (2, 'Runtime entity does not exist'), (3, '1024')):
            process.edit(Mode=mode);before, _, _ = snapshot('before-callback-'+str(mode))
            error = step(error=True);need(message.lower() in error['message'].lower(), 'Wrong compiled callback error: '+error['message'])
            after, _, _ = snapshot('after-callback-'+str(mode));need(exact(after, before), 'Callback failure changed full checkpoint')
        process.edit(Mode=5, FailAt=str(process.tick+1));before, _, _ = snapshot('before-later-tick')
        error = step(2, error=True);need('later-tick rollback' in error['message'], 'Second Tick did not fail as intended')
        after, _, _ = snapshot('after-later-tick');need(exact(after, before) and not (saves/'slot-hit-intent').exists(), 'Later Tick failed to roll back physics/game/query/save intent')
        process.edit(Mode=5, FailAt='-1');step()
        state = values();epoch = f'{int(state["TicketHigh"]) & ((1<<64)-1):016x}{int(state["TicketLow"]) & ((1<<64)-1):016x}'
        receipt = process.rpc('runtime.save.result', dict(epoch=epoch, sequence=int(state['TicketSequence'])))
        need(receipt['kind'] == 1 and receipt['state'] == 3 and receipt['generation'] == 1 and receipt['committed_tick'] == process.tick, 'Compiled save intent did not publish after query Tick')
        evidence['save_intent'] = receipt
        process.edit(Mode=7);step();need(values()['SaveSucceeded'] == 1, 'Compiled terminal save result not readable')
        intent_inventory = {p.name: sha(p) for p in (saves/'slot-hit-intent').iterdir() if p.is_file()}
        process.edit(Bone=joint, Mode=0)
        step(animations=[dict(entity=RIG, clip=0, time=1, speed=1, loop=False, playing=False)])
        joint_state = process.rpc('runtime.entity', dict(session_id=process.session, tick=process.tick, id=joint))
        need(abs(joint_state['world_matrix'][12]-20.1) < 1e-6, 'Caller animation did not commit the actual joint pose')
        process.edit(Mode=8);step()
        need(values()['BoneHits'] == 1 and values()['Mode'] == 9, 'Compiled query did not observe actual animated joint/start-of-Tick pose')
        joint_state = process.rpc('runtime.entity', dict(session_id=process.session, tick=process.tick, id=joint))
        need(abs(joint_state['world_matrix'][12]-20) < 1e-6, 'Staged animation did not publish after Tick')
        step();need(values()['BoneMisses'] == 1 and values()['Mode'] == 0, 'Next compiled Tick did not observe newly committed bone pose')
        evidence['checks'].append('Actual sphere/box/capsule owners and attachments, geometry/miss/inside/ignore, suppressed owner proxy and unchanged old raycast; exact1024 per Tick, callback errors and later-Tick full rollback including save intent; actual RigNode pose before and after staged animation')
        if not args.native_descriptor:
            before, _, _ = snapshot('before-compatible-reload');prior_game = process.game();process.load(config)
            after, _, _ = snapshot('after-compatible-reload');expected = copy.deepcopy(before)
            expected['payload']['gameplay_revision'] += 1;expected['sha256'] = after['sha256']
            need(exact(after, expected) and after['sha256'] != before['sha256'] and process.game()['revision'] == prior_game['revision']+1, 'Compatible reload lost full checkpoint')
            migration = process.game()['module']['migration']
            need(migration['added'] == migration['removed'] == [] and migration['preserved'] == sorted(prior_game['module']['values']), 'Reload field preservation differs')
            evidence['checks'].append('Same-schema CoreCLR reload preserves complete native checkpoint and every query decision field')
        saved, slot, receipt = snapshot('fresh-source');step(3)
        continuation, _, _ = snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        process = OwnedSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        session = fresh();restored = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        process.session = session;process.tick = restored['tick']
        actual, _, _ = snapshot('fresh-restored');need(exact(actual, saved), 'Fresh process failed exact durable restore')
        step(3);actual, _, _ = snapshot('fresh-actual-continuation')
        need(exact(actual, continuation) and world.read_bytes() == original and not source.exists(), 'Fresh compiled/cooked continuation differs')
        need(intent_inventory == {p.name: sha(p) for p in (saves/'slot-hit-intent').iterdir() if p.is_file()}, 'Original intent slot changed')
        paths = list((saves/('slot-'+slot)).glob('p-*.bin'));need(len(paths) == 1 and sha(paths[0]) == receipt['sha256'], 'Original saved generation changed')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        evidence['checks'].append('Fresh native process restores full checkpoint without source GLB; three ordinary compiled Ticks match prior continuation and authored/save bytes remain immutable')
        need(all(sha(path) == pins[name] for name, path in files.items()), 'Consumed source/image closure changed')
        need(time.monotonic() < args.deadline, 'Whole fixture deadline expired');evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error);raise
    finally:
        if process:
            try:
                process.close()
            except BaseException as error:
                evidence['close_error'] = repr(error);evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-started;evidence['rpc_count'] = len(evidence['calls'])
        data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode();need(len(data) <= CAP, 'Final evidence exceeds32MiB')
        (run/'evidence.json').write_bytes(data)
        if time.monotonic() > args.deadline:
            evidence['passed'] = False;evidence['deadline_error'] = 'Final publication exceeded180s'
            (run/'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            raise TimeoutError(evidence['deadline_error'])
        print(run/'evidence.json', flush=True)
        if 'close_error' in evidence and 'error' not in evidence:
            raise RuntimeError(evidence['close_error'])


if __name__ == '__main__':
    main()
