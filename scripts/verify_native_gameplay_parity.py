#!/usr/bin/env python3
"""Compare unchanged example gameplay under CoreCLR and Native AOT through Poima."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import uuid

from verify_native_gameplay import ROOT, Session, blend_fixture, component, transform, uid, node, glb


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ParitySession(Session):
    """Reuse the guarded RPC harness, with optional WSL-to-Windows path conversion."""
    def __init__(self, binary, world, record, native_path):
        self.record, self.world = record, world
        self.stderr = world.with_suffix('.stderr').open('w')
        self.process = subprocess.Popen([str(binary.resolve()), 'world', native_path(world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            text=True, encoding='utf-8', bufsize=1)
        self.output = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.output.put(line)
            self.output.put(None)
        threading.Thread(target=read, daemon=True).start()
        self.session, self.tick, self.revision = None, 0, 0


def without_session(value):
    if isinstance(value, dict):
        return {key: without_session(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [without_session(item) for item in value]
    return value


def checkpoint(process, entities):
    game = process.game()
    # Complete game schema and field values are semantic. Artifact paths/hashes,
    # backend diagnostics and migration bookkeeping intentionally differ.
    return dict(tick=game['tick'], revision=game['revision'],
        schema=game['module']['schema'], values=game['module']['values'],
        entities=[without_session(process.entity(entity)) for entity in entities])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['binary', 'hostfxr', 'bridge', 'door', 'animation', 'door-assembly', 'animation-assembly', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    run = args.output.resolve() / uuid.uuid4().hex
    run.mkdir(parents=True)
    def native(path):
        path = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if args.windows_interop else path
    files = dict(binary=args.binary, hostfxr=args.hostfxr, bridge=args.bridge,
        door_descriptor=args.door, animation_descriptor=args.animation,
        door_assembly=args.door_assembly, animation_assembly=args.animation_assembly,
        harness=Path(__file__), reused_harness=ROOT / 'scripts/verify_native_gameplay.py',
        fixture=ROOT / 'tests/runtime_animation_blend_contract.py',
        door_source=ROOT / 'examples/managed/DoorGame/DoorGame.cs',
        animation_source=ROOT / 'examples/managed/AnimationGame/AnimationGame.cs')
    record = dict(passed=False, hashes={name: sha(path) for name, path in files.items()},
        comparison='Exact schema, complete values, ticks and complete runtime entities except session_id; no numeric tolerance.',
        calls=[], checks={}, scenarios={})
    process = None
    try:
        for scenario in ('door', 'animation'):
            descriptor = getattr(args, scenario)
            artifact = json.loads(descriptor.read_text())
            record['hashes'][scenario + '_native_library'] = sha(descriptor.parent / artifact['library'])
            for backend in ('coreclr', 'native_aot'):
                label = scenario + '_' + backend
                result = record['scenarios'][label] = dict(checkpoints={})
                process = ParitySession(args.binary, run / (label + '.json'), record, native)
                if scenario == 'door':
                    fixture = json.loads((ROOT / 'examples/interaction-room.jsonl').read_text())
                    process.rpc(fixture['method'], fixture['params'])
                    ids = [uid(i) for i in (2, 3, 100, 101)]
                    initial = dict(OpenX=2.8, UseDistance=4)
                else:
                    model = run / (label + '.glb')
                    document, blob = blend_fixture(); model.write_bytes(glb(document, blob))
                    asset = process.rpc('asset.import', dict(source=native(model)))['asset']
                    ops = [dict(op='asset.instantiate', id=uid(100), name='Parity rig', asset=asset),
                        dict(op='entity.create', id=uid(10), name='Player'),
                        component(uid(10), 'Transform', transform((0, 1, 0))),
                        component(uid(10), 'CharacterController', dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=uid(11))),
                        dict(op='entity.create', id=uid(11), name='Camera'),
                        component(uid(11), 'Transform', transform((0, 1.6, 0))),
                        dict(op='entity.reparent', id=uid(11), parent=uid(10), mode='keep_local'),
                        component(uid(11), 'Camera', dict(vertical_fov=60, near=.1, far=100))]
                    process.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
                    ids = [uid(100), *(node(uid(100), i) for i in range(3)), uid(10), uid(11)]
                    initial = dict(NextClip=1, Changes=3, Rig=uid(100))
                authored = process.world.read_bytes()
                process.start()
                params = dict(session_id=process.session, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=0, values=initial)
                if backend == 'native_aot':
                    method = 'runtime.gameplay.load_native'
                    params.update(descriptor=native(descriptor), expected_descriptor_sha256=sha(descriptor))
                else:
                    method = 'runtime.gameplay.load'
                    params.update(hostfxr=native(args.hostfxr), bridge=native(args.bridge),
                        assembly=native(getattr(args, scenario + '_assembly')),
                        type='Poima.Examples.' + ('DoorGame' if scenario == 'door' else 'AnimationGame'))
                loaded = process.rpc(method, params)
                process.revision = 1
                assert loaded['revision'] == 1 and loaded['module']['backend'] == backend, loaded
                if backend == 'native_aot':
                    assert loaded['module']['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
                for key, value in initial.items():
                    assert loaded['module']['values'][key] == value, loaded
                def save(name):
                    state = checkpoint(process, ids)
                    result['checkpoints'][name] = state
                    return state
                save('typed_initial_values')
                if scenario == 'door':
                    process.step(120)
                    process.step(150, [dict(entity=uid(100), move=[0, 1])])
                    save('approached_door')
                    process.step(120, [dict(entity=uid(100), use=True)])
                    state = save('use_moves_kinematic_door')
                    assert state['values']['Activations'] == 1 and state['values']['Open'] == 1
                    assert abs(state['entities'][0]['world_matrix'][12] - 2.8) < 1e-5
                    process.edit(OpenX=3.5, UseDistance=4.5)
                    save('typed_state_edit')
                else:
                    process.step(1, [dict(entity=uid(10), use=True)])
                    state = save('first_crossfade')
                    animation = state['entities'][0]['animation']
                    assert state['values']['Changes'] == 4 and animation['clip'] == 1
                    assert animation['transition']['elapsed_ticks'] == 1
                    assert animation['transition']['source_frozen'] is False
                    assert state['values']['ObservedClip'] == -1  # Query precedes the queued write.
                    process.step(8)
                    save('advancing_crossfade')
                    process.step(1, [dict(entity=uid(10), use=True)])
                    state = save('frozen_interruption')
                    animation = state['entities'][0]['animation']
                    assert state['values']['Changes'] == 5 and animation['clip'] == 0
                    assert animation['transition']['source_frozen'] is True
                    # Fail while a frozen-source fade is active. Both the managed
                    # state and existing pose/physics must survive the failed tick.
                    process.edit(NextClip=999)
                    before = save('before_invalid_tick')
                    process.step(1, [dict(entity=uid(10), use=True)], error=-32040)
                    assert save('after_invalid_tick') == before
                    process.edit(NextClip=0)
                    process.step(29)
                    state = save('exact_fade_completion')
                    assert state['entities'][0]['animation']['transition'] is None
                    assert abs(state['entities'][0]['animation']['time'] - .5) < 1e-12
                    process.step(1, [dict(entity=uid(10), use=True)])
                    assert save('recovery_command')['values']['Changes'] == 6
                assert process.world.read_bytes() == authored
                result['authored_bytes_unchanged'] = True
                process.close(); process = None
            left = record['scenarios'][scenario + '_coreclr']['checkpoints']
            right = record['scenarios'][scenario + '_native_aot']['checkpoints']
            for name in left:
                assert left[name] == right[name], f'{scenario}/{name}: semantic mismatch; inspect recorded checkpoints'
                record['checks'][scenario + '/' + name] = True
        record['passed'] = True
    finally:
        if process is not None:
            if process.process.poll() is None:
                process.process.kill()
            process.process.wait(); process.stderr.close()
        (run / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(dict(passed=True, checks=len(record['checks']), calls=len(record['calls']), evidence=str(run / 'evidence.json'))))


if __name__ == '__main__':
    main()
