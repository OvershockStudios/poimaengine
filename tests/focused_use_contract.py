#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the compiled FocusedUse fixture through native input, physics and saves."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import time

from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import OwnedSession, exact, fresh, need


ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.FocusedUseGame'
IDENTITY = 'poima.test.focused-use'
ACTOR, CAMERA, BLOCKER, FORWARD, SIDE = map(uid, (100, 101, 200, 300, 301))
MISS, BLOCKED, TARGET = range(3)


def scene():
    operations = []

    def component(identity, kind, value):
        operations.append(dict(op='component.set', id=identity, type=kind, value=value))

    def entity(identity, name, position, parent=None):
        operations.append(dict(op='entity.create', id=identity, name=name, parent=parent))
        component(identity, 'Transform', dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]))

    def box(identity, name, position, half_extents, motion='static'):
        entity(identity, name, position)
        component(identity, 'BoxCollider', dict(half_extents=half_extents, motion=motion, mass=1,
                                                 friction=.5, restitution=0))

    box(uid(1), 'Floor', [0, -.5, 0], [8, .5, 8])
    entity(ACTOR, 'Actor', [0, 0, 2])
    component(ACTOR, 'CharacterController', dict(radius=.3, height=1.8, speed=3, jump_speed=5, camera=CAMERA))
    # Start behind the actor's capsule so an ordinary, unfiltered ray must hit it.
    entity(CAMERA, 'Camera', [0, .9, .65], ACTOR)
    component(CAMERA, 'Camera', dict(vertical_fov=70, near=.1, far=32))
    box(BLOCKER, 'Moving blocker', [0, .9, 1.35], [.35, .45, .15], motion='kinematic')
    box(FORWARD, 'Forward target', [0, .9, -.1], [.25, .45, .25])
    box(SIDE, 'Side target', [2.5, .9, 2], [.25, .45, .25])
    return operations


def check_use(values, status, hit, distance, edges, activations):
    expected = dict(UseStatus=status, UseHit=hit, UseEdges=edges, Activations=activations)
    need(all(values[field] == value for field, value in expected.items()) and
         math.isclose(values['UseDistance'], distance, abs_tol=.002),
         f'Unexpected compiled Use outcome {values}, expected {expected} at {distance} m')


def artifact(args):
    paths = [Path(__file__), ROOT/'tests/input_actions_gameplay_contract.py',
             ROOT/'tests/components_gameplay_contract.py', args.binary]
    paths.extend((ROOT/'tests/fixtures/focused-use').glob('*.cs*'))
    paths.extend((ROOT/'packages/interaction').glob('*.cs*'))
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve(strict=True)
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds 1 MiB')
        spec = json.loads(descriptor.read_text(encoding='utf-8'))
        need(spec['type'] == GAME and spec['identity'] == IDENTITY, 'Wrong native game descriptor')
        closure = set()
        for item in spec['files']:
            relative = Path(item['path'])
            path = (descriptor.parent/relative).resolve(strict=True)
            need(not relative.is_absolute() and '..' not in relative.parts and path.is_relative_to(descriptor.parent) and
                 path.stat().st_size == item['size'] and sha(path) == item['sha256'], 'Native artifact closure differs')
            closure.add(path)
        image = (descriptor.parent/spec['library']).resolve(strict=True)
        need(image in closure, 'Native game image is absent from the descriptor closure')
        paths.extend([descriptor, *sorted(closure)])
        return image, dict(descriptor=args.native(descriptor), expected_descriptor_sha256=sha(descriptor)), paths

    image = args.assembly.resolve(strict=True)
    paths.extend([args.hostfxr, args.bridge, image, image.parent/'Poima.Interaction.dll'])
    for assembly in (image, args.bridge):
        paths.append(assembly.parent/'Poima.Gameplay.dll')
        paths.extend(path for suffix in ('.deps.json', '.runtimeconfig.json')
                     if (path := assembly.with_suffix(suffix)).is_file())
    return image, dict(hostfxr=args.native(args.hostfxr), bridge=args.native(args.bridge), assembly=args.native(image),
                       type=GAME), paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    development = (args.hostfxr, args.bridge, args.assembly)
    if (args.native_descriptor and any(development)) or (not args.native_descriptor and not all(development)):
        parser.error('Supply --native-descriptor or all three CoreCLR paths')

    def native(path):
        value = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    args.native = native
    args.deadline = time.monotonic()+180
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    world, saves = output/'world.json', output/'saves'
    saves.mkdir()
    backend = 'native_aot' if args.native_descriptor else 'coreclr'
    evidence = dict(format='poima.focused-use-contract.v1', backend=backend, passed=False, owners=[], calls=[],
                    observations={}, snapshots={}, scope='Compiled gameplay with native headless input, rays and saves.')
    owner = None
    try:
        image, config, paths = artifact(args)
        pins = {str(path.resolve()): sha(path) for path in paths}
        evidence['input_hashes'] = pins
        owner = OwnedSession(args, world, evidence)
        revision = owner.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene()))['revision']
        authored = world.read_bytes()
        owner.start(revision)
        owner.step(45)
        module = owner.load(config)['module']
        need(module['schema']['identity'] == IDENTITY and module['backend'] == backend and
             module['assembly_sha256'] == sha(image), 'Native runtime loaded a different gameplay artifact')

        def configure_saves():
            owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))

        def checkpoint(label):
            receipt = owner.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=label,
                expected_generation=0, session_id=owner.session, expected_tick=owner.tick,
                expected_gameplay_revision=owner.game()['revision']))
            payload, = (saves/('slot-'+label)).glob('p-*.bin')
            need(payload.stat().st_size <= 16*1024*1024 and sha(payload) == receipt['sha256'], 'Save receipt differs')
            snapshot = json.loads(payload.read_bytes())['snapshot']
            evidence['snapshots'][label] = dict(receipt=receipt, snapshot=snapshot)
            return snapshot, payload, receipt['sha256']

        def step(ticks=1, *, look=0, use=False, motions=None, failure=False, retry=False):
            request = dict(session_id=owner.session, request_id=fresh(), expected_tick=owner.tick, ticks=ticks,
                           inputs=[dict(entity=ACTOR, look=[look, 0], use=use)])
            if motions is not None:
                request['motions'] = motions
            result = owner.rpc('runtime.step', request, error=failure)
            if not failure:
                owner.tick = result['current_tick']
                owner.session = result['current_session_id']
                if retry:
                    before = owner.game()
                    need(owner.rpc('runtime.step', request) == dict(result, replayed=True) and owner.game() == before,
                         'A retried Use receipt reprocessed the action')
            return result

        def values():
            return owner.game()['module']['values']

        def observe(label, status, hit, distance, edges, activations):
            state = values()
            evidence['observations'][label] = dict(tick=owner.tick, values=state)
            check_use(state, status, hit, distance, edges, activations)

        def ray(ignore=(), distance=4):
            sample = dict(session_id=owner.session, tick=owner.tick)
            pose = owner.rpc('runtime.entity', dict(sample, id=CAMERA))['world_matrix']
            return owner.rpc('runtime.raycast', dict(sample, origin=pose[12:15], direction=[-v for v in pose[8:11]],
                                                     distance=distance, ignore=list(ignore)))['hit']

        configure_saves()
        need(ray()['entity'] == ACTOR and ray([ACTOR])['entity'] == BLOCKER,
             'Fixture did not place the actor and blocker on the initial ray')
        step(use=True)
        observe('first-blocker', BLOCKED, BLOCKER, 1.15, 1, 0)
        step(4, look=-90, use=True, retry=True)
        observe('look-and-use-same-tick', TARGET, SIDE, 2.9, 2, 1)
        need(ray()['entity'] == ACTOR and ray([ACTOR])['entity'] == SIDE,
             'Fixture did not turn the camera toward the second target through the actor capsule')

        owner.edit(Reach=2.5)
        step(use=True)
        observe('out-of-reach', MISS, uid(0), 0, 3, 1)
        need(ray([ACTOR], 2.5) is None, 'Short ray unexpectedly intersects another collider')
        owner.edit(Reach=4)
        step(24, motions=[dict(entity=BLOCKER, position=[3, .9, 1.35], rotation=[0, 0, 0, 1], duration_ticks=24)])

        owner.edit(FailAt=str(owner.tick+1))
        before, _, _ = checkpoint('before-rollback')
        failure = step(2, look=90, use=True, failure=True)
        need('Focused Use rollback witness after 2 activations.' in failure['message'],
             'Expected failure after a real activation in the first tick of the batch')
        after, _, _ = checkpoint('after-rollback')
        need(exact(before, after) and ray([ACTOR])['entity'] == SIDE,
             'Rejected batch changed the saved camera, interaction or physics state')

        owner.edit(FailAt='-1')
        step(2, look=90, use=True)
        observe('clear-first-target', TARGET, FORWARD, 2.5, 4, 2)
        need(ray([ACTOR])['entity'] == FORWARD, 'Moving the blocker did not expose the first target')

        saved, payload, receipt_hash = checkpoint('cold-source')
        step(3, look=-90, use=True)
        observe('original-first-continuation', TARGET, SIDE, 2.9, 5, 3)
        step(2, look=90, use=True)
        observe('original-last-continuation', TARGET, FORWARD, 2.5, 6, 4)
        expected, _, _ = checkpoint('original-continuation')

        owner.rpc('runtime.stop', dict(session_id=owner.session))
        owner.close()
        owner = OwnedSession(args, world, evidence)
        configure_saves()
        session = fresh()
        loaded = owner.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot='cold-source',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        owner.session, owner.tick = session, loaded['tick']
        restored, _, _ = checkpoint('cold-restored')
        need(exact(restored, saved), 'Fresh native owner did not restore the full saved state')
        step(3, look=-90, use=True)
        observe('cold-first-continuation', TARGET, SIDE, 2.9, 5, 3)
        step(2, look=90, use=True)
        observe('cold-last-continuation', TARGET, FORWARD, 2.5, 6, 4)
        actual, _, _ = checkpoint('cold-continuation')
        need(exact(actual, expected) and world.read_bytes() == authored and sha(payload) == receipt_hash,
             'Fresh native continuation or original content differs')
        need(all(sha(path) == expected_hash for path, expected_hash in pins.items()), 'A tested input changed')
        owner.rpc('runtime.stop', dict(session_id=owner.session))
        owner.close()
        owner = None
        evidence['passed'] = True
    except BaseException as failure:
        evidence['error'] = repr(failure)
        raise
    finally:
        try:
            if owner:
                owner.close()
        except BaseException as failure:
            evidence['close_error'] = repr(failure)
            if 'error' not in evidence:
                raise
        finally:
            path = output/'evidence.json'
            path.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            print(path)


if __name__ == '__main__':
    main()
