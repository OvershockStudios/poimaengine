#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare actual prior/current character input, complete saves and cold replay.

Builds nothing; run beneath an external process-tree owner. Both independent
hosts receive the same authored world bytes, explicit session ID and logical
commands. Session IDs are deliberately shared so complete replies can be compared
without removing any fields. Raw requests/responses and process closure remain
in evidence. This checks caller inputs, not compiled callback observation timing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import uuid

from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import OwnedSession, exact, need

ACTORS = (100, 200, 300, 400, 500)
CAMERAS = (101, 201, 301, 401)
STANCED = (200, 400)
ENTITIES = (1, 20, 110, 210, 310, 410, *ACTORS, *CAMERAS)
PAYLOAD_CAP = 16*1024*1024
OUTPUT_CAP = 64*1024*1024


def scene():
    ops = []

    def entity(identity, name, position, parent=None):
        ops.extend([dict(op='entity.create', id=uid(identity), name=name, parent=parent),
                    dict(op='component.set', id=uid(identity), type='Transform', value=dict(
                        position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1]))])

    def component(identity, kind, value):
        ops.append(dict(op='component.set', id=uid(identity), type=kind, value=value))

    def box(identity, name, position, half_extents, motion='static'):
        entity(identity, name, position)
        component(identity, 'BoxCollider', dict(half_extents=half_extents,
                  motion=motion, mass=1, friction=.5, restitution=0))

    box(1, 'Floor', [0, -.5, 0], [30, .5, 30])
    box(20, 'Moving support', [20, .5, 10], [2, .5, 2], 'kinematic')
    for identity, x in zip(ACTORS[:4], (-12, -4, 4, 12)):
        box(identity+10, 'Lane wall '+str(identity), [x, 1.5, -3.5], [2, 1.5, .2])
        entity(identity, 'Input actor '+str(identity), [x, .1, 1])
        profile = dict(radius=.3, height=1.8, speed=3, jump_speed=5, camera=uid(identity+1))
        if identity >= 300:
            profile['locomotion'] = dict(step_height=.4, ground_snap=.5, max_slope_degrees=45)
        if identity in STANCED:
            profile['stance'] = dict(crouched_height=1, crouched_speed=1, camera_drop=.5)
        component(identity, 'CharacterController', profile)
        entity(identity+1, 'Owned camera '+str(identity), [.05, 1.6, .02], uid(identity))
        component(identity+1, 'Camera', dict(vertical_fov=70, near=.1, far=100))
    entity(500, 'Neutral supported virtual actor', [20, 1.1, 10])
    component(500, 'CharacterController', dict(radius=.3, height=1.8, speed=3,
              jump_speed=5, camera=None, locomotion=dict(step_height=.4, ground_snap=.5,
                                                        max_slope_degrees=45)))
    return ops


class ComparedSession(OwnedSession):
    def __init__(self, args, world, evidence, role):
        self.role = role
        super().__init__(args, world, evidence)
        self.owner['role'] = role
        self.owner['image_sha256'] = sha(args.binary)

    def rpc(self, method, params=None, error=False):
        index = len(self.evidence['calls'])
        try:
            return super().rpc(method, params, error)
        finally:
            if len(self.evidence['calls']) > index:
                self.evidence['calls'][index]['owner_role'] = self.role


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'legacy-binary', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()

    def native(path):
        path = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if args.windows_interop else path

    args.native = native
    args.deadline = time.monotonic()+240
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    old_args = argparse.Namespace(**vars(args))
    old_args.binary = args.legacy_binary
    files = (Path(__file__), Path(__file__).with_name('components_gameplay_contract.py'),
             Path(__file__).with_name('input_actions_gameplay_contract.py'),
             Path(__file__).with_name('character_gameplay_contract.py'), args.binary, args.legacy_binary)
    evidence = dict(passed=False, calls=[], owners=[], checks=[], observations=[], archives=[],
                    source_pins={str(path.resolve()): sha(path) for path in files},
                    identity_policy='Same authored bytes and explicitly shared session IDs in independent hosts; no response/snapshot fields removed or rewritten.',
                    profiles=['legacy', 'legacy_stance', 'virtual', 'virtual_stance', 'virtual_supported'],
                    limitations=['Caller-input regression only; no compiled callback timing, controller-to-ragdoll transfer, GPU, GUI, physical input or performance qualification.',
                                 'Cold continuation compares independently restored old/current hosts, not equality of an unreconstructed warm Jolt cache.'])
    hosts = []
    save_number = 0
    try:
        need(sha(args.binary) != sha(args.legacy_binary), 'Old and current native images must differ')
        old_world, current_world = out/'old.json', out/'current.json'
        old = ComparedSession(old_args, old_world, evidence, 'old-warm')
        hosts.append(old)
        revision = old.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=scene()))['revision']
        authored = old_world.read_bytes()
        shutil.copyfile(old_world, current_world)
        need(current_world.read_bytes() == authored, 'Copied authored world differs')
        current = ComparedSession(args, current_world, evidence, 'current-warm')
        hosts.append(current)
        old_saves, current_saves = out/'old-saves', out/'current-saves'
        for host, storage in ((old, old_saves), (current, current_saves)):
            storage.mkdir()
            host.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(storage)))
            host.session = uid(9601)
            host.rpc('runtime.start', dict(session_id=host.session, revision=revision))

        def observe(label):
            rows = []
            for host in (old, current):
                guard = dict(session_id=host.session, tick=host.tick)
                row = dict(entities={str(i): host.rpc('runtime.entity', dict(guard, id=uid(i))) for i in ENTITIES},
                           cameras={str(i): host.rpc('runtime.camera_pose', dict(guard, entity=uid(i))) for i in CAMERAS},
                           stance={str(i): host.rpc('runtime.character_stance', dict(guard, entity=uid(i))) for i in STANCED})
                rows.append(row)
            need(exact(*rows), 'Complete old/current observations differ at '+label)
            encoded = json.dumps(rows[0], sort_keys=True, separators=(',', ':'), allow_nan=False).encode()
            evidence['observations'].append(dict(label=label, tick=old.tick, sha256=hashlib.sha256(encoded).hexdigest()))
            return rows[0]

        def step(label, ticks=1, move=(0, 0), look=(0, 0), jump=False, neutral=False, **extra):
            inputs = [] if neutral else [dict(entity=uid(i), move=list(move), look=list(look), jump=jump, use=False) for i in ACTORS[:4]]
            params = dict(session_id=old.session, request_id=uuid.uuid4().hex, expected_tick=old.tick,
                          ticks=ticks, inputs=inputs, **extra)
            replies = [host.rpc('runtime.step', params) for host in (old, current)]
            need(exact(*replies), 'Complete step results differ at '+label)
            for host, reply in zip((old, current), replies):
                host.tick = reply['current_tick']
                host.session = reply['current_session_id']
            need(old.tick == params['expected_tick']+ticks, 'Step did not complete the requested ticks')
            return observe(label)

        def archives(label):
            nonlocal save_number
            save_number += 1
            slot = 'input-plan-'+str(save_number)
            records = []
            for host, storage in ((old, old_saves), (current, current_saves)):
                receipt = host.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                    slot=slot, expected_generation=0, session_id=host.session, expected_tick=host.tick,
                    expected_gameplay_revision=0))
                folder = storage/('slot-'+slot)
                manifest = json.loads((folder/'current.json').read_bytes())
                member = manifest['payload']['current']
                name = member['file']
                need(len(name) == 38 and name.startswith('p-') and name.endswith('.bin') and
                     all(c in '0123456789abcdef' for c in name[2:-4]), 'Unsafe selected payload name')
                path = folder/name
                need(path.stat().st_size <= PAYLOAD_CAP, 'Save payload exceeds16MiB')
                data = path.read_bytes()
                need(len(data) == member['bytes'] == receipt['bytes'] and
                     sha(path) == member['sha256'] == receipt['sha256'], 'Archive identity differs from manifest/receipt')
                payload = json.loads(data)
                records.append((data, payload))
                evidence['archives'].append(dict(label=label, role=host.role, file=str(path),
                                                bytes=len(data), sha256=sha(path), receipt=receipt))
            need(records[0][0] == records[1][0] and exact(records[0][1], records[1][1]),
                 'Complete old/current archive bytes or JSON differ at '+label)
            return slot, records[0][0]

        settled = step('settled', 120, neutral=True)
        need(all(settled['entities'][str(i)]['ground'] == 'on_ground' for i in ACTORS), 'Grounded jump/support precondition absent')
        jumped = step('grounded-jump', jump=True)
        need(all(jumped['entities'][str(i)]['velocity'][1] > 0 for i in ACTORS[:4]), 'Grounded jump did not produce upward motion')
        coasting = step('airborne-coast', 2, neutral=True)
        need(all(coasting['entities'][str(i)]['ground'] == 'in_air' for i in ACTORS[:4]), 'Airborne jump precondition absent')
        airborne = step('airborne-jump-request', jump=True)
        need(all(airborne['entities'][str(i)]['velocity'][1] < coasting['entities'][str(i)]['velocity'][1]
                 for i in ACTORS[:4]), 'Airborne jump request reset upward velocity')
        step('land', 90, neutral=True)
        step('diagonal-held-batch', 8, move=(1, 1), look=(37, 11))
        high = step('yaw-wrap-positive-pitch-clamp', look=(180, 180))
        need(all(high['entities'][str(i)]['yaw'] == -143 and high['entities'][str(i)]['pitch'] == 85
                 for i in ACTORS[:4]), 'Positive yaw wrap/pitch clamp differs')
        step('second-wrap', look=(180, 180))
        low = step('negative-pitch-clamp-heading-reset', look=(-37, -180))
        need(all(low['entities'][str(i)]['yaw'] == 0 and low['entities'][str(i)]['pitch'] == -85
                 for i in ACTORS[:4]), 'Negative pitch clamp or heading reset differs')
        step('crouched-speed', 12, move=(0, 1), character_stances=[dict(entity=uid(i), crouch=True) for i in STANCED])
        archives('crouched-input-state')
        standing = step('stand', neutral=True, character_stances=[dict(entity=uid(i), crouch=False) for i in STANCED])
        need(all(not standing['stance'][str(i)]['state']['crouched'] for i in STANCED), 'Standing shape did not return')
        blocked = step('held-wall-input', 120, move=(0, 1))
        need(all(blocked['entities'][str(i)]['world_matrix'][14] > -3.4 for i in ACTORS[:4]), 'Character crossed an authored lane wall')
        step('omitted-input-neutralizes', 2, neutral=True)
        effects = [dict(entity=uid(i), world=dict(position=[.03, -.02, .01], rotation=[0, 0, 0, 1])) for i in CAMERAS]
        step('camera-base-and-effects', look=(13, 9), camera_poses=effects)
        platform_before = observe('before-moving-support')['entities']['500']['world_matrix'][12]
        carried = step('moving-support-neutral-actor', 60, neutral=True, motions=[dict(entity=uid(20),
                       position=[21.5, .5, 10], rotation=[0, 0, 0, 1], duration_ticks=60)])
        need(1 < carried['entities']['500']['world_matrix'][12]-platform_before < 2,
             'Neutral virtual actor was not carried by the translating support')
        slot, original_save = archives('before-cold-replay')
        evidence['checks'].append('Complete old/current replies and archives match for both motor backends, optional stance, held edges, jumps, diagonal input, yaw/pitch limits, walls, cameras and moving support')

        # Close the warm owners before replacements. Copy original archive files
        # verbatim; the current fresh process consumes an old-image save.
        for host in hosts:
            host.close()
        hosts.clear()
        cold_current_saves = out/'current-old-save-copy'
        shutil.copytree(old_saves, cold_current_saves)
        for path in old_saves.rglob('*'):
            if path.is_file():
                need(sha(path) == sha(cold_current_saves/path.relative_to(old_saves)), 'Old save copy changed bytes')
        current_saves = cold_current_saves
        old = ComparedSession(old_args, old_world, evidence, 'old-cold')
        hosts.append(old)
        current = ComparedSession(args, current_world, evidence, 'current-cold')
        hosts.append(current)
        for host, storage in ((old, old_saves), (current, current_saves)):
            host.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(storage)))
            host.session = uid(9602)
            result = host.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                slot=slot, expected_generation=1, revision=revision, expected_session_id=None,
                expected_tick=None, expected_gameplay_revision=None, new_session_id=host.session, gameplay=None))
            host.tick = result['tick']
        observe('fresh-old-save-restoration')
        _, restored_save = archives('fresh-old-save-roundtrip')
        need(restored_save == original_save, 'Cold restoration changed complete original archive bytes')
        step('cold-look-jump-continuation', move=(1, -1), look=(-26, 15), jump=True)
        step('cold-held-continuation', 30, move=(-1, 0), look=(17, -8))
        step('cold-neutral-continuation', 30, neutral=True)
        archives('complete-cold-continuation')
        need(old_world.read_bytes() == authored and current_world.read_bytes() == authored, 'Runtime altered authored world')
        need(all(sha(path) == evidence['source_pins'][str(path.resolve())] for path in files),
             'A pinned helper, verifier or native image changed during execution')
        evidence['checks'].append('Fresh current and old native owners restore the exact original old-image archive and retain complete archive equality through future input; no state fields are excluded')
        evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error)
        raise
    finally:
        close_errors = []
        for host in hosts:
            try:
                host.close()
            except BaseException as error:
                close_errors.append(repr(error))
        if close_errors:
            evidence['passed'] = False
            evidence['close_errors'] = close_errors
        evidence['rpc_count'] = len(evidence['calls'])
        evidence['natural_closed_owners'] = sum(row['exit_code'] == 0 and not row['forced_cleanup'] for row in evidence['owners'])
        need(sum(path.stat().st_size for path in out.rglob('*') if path.is_file()) <= OUTPUT_CAP,
             'Fixture artifacts exceed64MiB')
        (out/'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(out/'evidence.json', flush=True)
        if close_errors and 'error' not in evidence:
            raise RuntimeError('; '.join(close_errors))


if __name__ == '__main__':
    main()
