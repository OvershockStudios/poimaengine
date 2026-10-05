#!/usr/bin/env python3
"""Drive actual native C# gameplay through the engine's guarded world service."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from runtime_animation_blend_contract import blend_fixture, component, transform, uid, node
from gltf_fixture import glb


class Session:
    def __init__(self, binary, world, record):
        self.record, self.world = record, world
        self.stderr = world.with_suffix('.stderr').open('w')
        self.process = subprocess.Popen([str(binary.resolve()), 'world', str(world)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding='utf-8', bufsize=1)
        self.output = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.output.put(line)
            self.output.put(None)
        threading.Thread(target=read, daemon=True).start()
        self.session, self.tick, self.revision = None, 0, 0
    def rpc(self, method, params=None, error=None):
        request = dict(jsonrpc='2.0', id=len(self.record['calls']) + 1, method=method, params=params or {})
        self.process.stdin.write(json.dumps(request) + '\n'); self.process.stdin.flush()
        line = self.output.get(timeout=60)
        assert line is not None, self.world.with_suffix('.stderr').read_text()
        response = json.loads(line)
        self.record['calls'].append(dict(request=request, response=response))
        assert response['id'] == request['id'], response
        if error is not None:
            assert response.get('error', {}).get('code') == error, response
            return response
        assert 'result' in response, response
        return response['result']
    def start(self):
        if self.session:
            self.rpc('runtime.stop', dict(session_id=self.session))
        self.session, self.tick, self.revision = uuid.uuid4().hex, 0, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=1))
    def load(self, descriptor, error=None, **extra):
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick,
            expected_revision=self.revision, descriptor=str(descriptor.resolve()), **extra)
        result = self.rpc('runtime.gameplay.load_native', params, error)
        if error is None:
            self.revision += 1
            module = result['module']
            assert module['backend'] == 'native_aot', module
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
            assert result['revision'] == self.revision
            assert self.rpc('runtime.gameplay.load_native', params) == dict(result, replayed=True)
        return result
    def game(self):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))
    def edit(self, **values):
        self.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=self.revision, values=values))
        self.revision += 1
    def step(self, ticks, inputs=(), error=None, animations=()):
        result = self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, ticks=ticks, inputs=list(inputs), animations=list(animations)), error)
        if error is None:
            self.tick += ticks
            assert result['tick'] == self.tick
        return result
    def entity(self, value):
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=value))
    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=15)
        assert self.process.returncode == 0
        self.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['binary', 'door', 'animation', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--probe', type=Path, help='Optional compiled verify_gameplay_animation.PROBE artifact for later-tick rollback.')
    args = parser.parse_args()
    run = args.output.resolve() / uuid.uuid4().hex; run.mkdir(parents=True)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    record = dict(passed=False, binary_sha256=sha(args.binary), test_sha256=sha(Path(__file__)), calls=[], checks={})
    process = None
    try:
        # Exact unchanged DoorGame source uses native entity/ray/kinematic APIs.
        process = Session(args.binary, run / 'door-world.json', record)
        fixture = json.loads((ROOT / 'examples/interaction-room.jsonl').read_text())
        process.rpc(fixture['method'], fixture['params']); authored = process.world.read_bytes()
        process.start()
        process.load(args.door, error=-32060, expected_descriptor_sha256='0' * 64)
        process.load(args.door, error=-32060, values={'OpenX': 'not a number'})
        counts = process.rpc('runtime.gameplay.collect', dict(session_id=process.session))
        assert counts['active_modules'] == 0 and 'native' not in counts
        assert process.game()['module'] is None
        record['checks']['stale_descriptor_and_invalid_values_reject_before_module_execution'] = True
        result = process.load(args.door, expected_descriptor_sha256=sha(args.door))
        initial = copy.deepcopy(result['module']['values'])
        process.step(120); process.step(150, [dict(entity=uid(100), move=[0, 1])])
        process.step(120, [dict(entity=uid(100), use=True)])
        assert process.game()['module']['values']['Activations'] == 1
        assert abs(process.entity(uid(2))['world_matrix'][12] - 2.2) < 1e-5
        record['checks']['unchanged_door_native_queries_and_kinematic_motion'] = True
        before = process.game()
        process.load(args.door, error=-32060)
        assert process.game() == before
        process.edit(OpenX=3.5, UseDistance=4)
        assert process.game()['module']['values']['OpenX'] == 3.5
        record['checks']['state_edit_receipt_retry_and_replacement_refused'] = True
        process.start(); process.load(args.door)
        assert process.game()['module']['values'] == initial
        counts = process.rpc('runtime.gameplay.collect', dict(session_id=process.session))
        assert counts['active_modules'] == 1 and counts['native']['retired_alive'] == 0
        process.start()
        process.load(args.animation, error=-32060)
        assert process.game()['module'] is None
        process.load(args.door)
        assert process.world.read_bytes() == authored
        record['checks']['fresh_instance_independence_and_process_module_pin'] = True
        process.close(); process = None

        # Each compiled artifact has a distinct process lifetime. AnimationGame
        # is the actual checked-in example, not a native substitute.
        process = Session(args.binary, run / 'animation-world.json', record)
        model = run / 'rig.glb'; doc, blob = blend_fixture(); model.write_bytes(glb(doc, blob))
        asset = process.rpc('asset.import', dict(source=str(model)))['asset']
        ops = [dict(op='asset.instantiate', id=uid(100), name='Native C# rig', asset=asset),
            dict(op='entity.create', id=uid(10), name='Player'), component(uid(10), 'Transform', transform((0, 1, 0))),
            component(uid(10), 'CharacterController', dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=uid(11))),
            dict(op='entity.create', id=uid(11), name='Camera'), component(uid(11), 'Transform', transform((0, 1.6, 0))),
            dict(op='entity.reparent', id=uid(11), parent=uid(10), mode='keep_local'),
            component(uid(11), 'Camera', dict(vertical_fov=60, near=.1, far=100))]
        process.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
        authored = process.world.read_bytes()
        process.start(); process.load(args.animation)
        process.step(1, [dict(entity=uid(10), use=True)])
        assert process.game()['module']['values']['Changes'] == 1
        assert process.entity(uid(100))['animation']['transition']['elapsed_ticks'] == 1
        process.step(8)
        process.step(1, [dict(entity=uid(10), use=True)])
        assert process.entity(uid(100))['animation']['transition']['source_frozen'] is True
        process.step(29)
        assert process.entity(uid(100))['animation']['transition'] is None
        assert process.game()['module']['values']['Changes'] == 2
        record['checks']['unchanged_animation_queries_crossfades_and_frozen_interruption'] = True
        # Invalid queued target must roll back managed fields AND existing fade.
        process.edit(NextClip=999)
        before = (process.game(), process.entity(uid(100)), process.entity(node(uid(100), 1)), process.entity(uid(10)))
        process.step(1, [dict(entity=uid(10), use=True)], error=-32040)
        assert before == (process.game(), process.entity(uid(100)), process.entity(node(uid(100), 1)), process.entity(uid(10)))
        process.edit(NextClip=0)
        process.step(1, [dict(entity=uid(10), use=True)])
        assert process.game()['module']['values']['Changes'] == 3
        record['checks']['invalid_native_game_command_rolls_back_state_pose_and_physics'] = True
        process.start(); process.load(args.animation)
        process.step(1, [dict(entity=uid(10), use=True)]); process.step(24)
        whole = (process.game()['module']['values'], process.entity(uid(100))['animation'], process.entity(node(uid(100), 1))['local_transform'])
        process.start(); process.load(args.animation)
        process.step(1, [dict(entity=uid(10), use=True)])
        for _ in range(6): process.step(4)
        split = (process.game()['module']['values'], process.entity(uid(100))['animation'], process.entity(node(uid(100), 1))['local_transform'])
        assert whole == split
        assert process.world.read_bytes() == authored
        record['checks']['native_game_fixed_tick_partition_and_authoring_immutability'] = True
        process.close(); process = None

        if args.probe:
            process = Session(args.binary, run / 'probe-world.json', record)
            asset = process.rpc('asset.import', dict(source=str(model)))['asset']
            sound = run / 'probe.wav'
            with wave.open(str(sound), 'wb') as wav:
                wav.setparams((1, 2, 48000, 4800, 'NONE', 'not compressed')); wav.writeframes(b'\x00\x01' * 4800)
            audio = process.rpc('asset.audio.import', dict(source=str(sound)))['asset']
            ops = [dict(op='asset.instantiate', id=uid(1000), name='Probe rig', asset=asset)]
            for value, position in [(1, (0, -.5, 0)), (2, (5, 1, 0)), (3, (0, 0, 0)), (4, (5, 3, 0)), (20, (0, 1, 0))]:
                ops += [dict(op='entity.create', id=uid(value), name=f'Entity {value}'), component(uid(value), 'Transform', transform(position))]
            for value, motion, extents in [(1, 'static', [20, .5, 20]), (2, 'kinematic', [.5, .5, .5]), (4, 'dynamic', [.25, .25, .25])]:
                ops += [component(uid(value), 'BoxCollider', dict(half_extents=extents, motion=motion, mass=1, friction=.5, restitution=0))]
            ops += [component(uid(20), 'AudioEmitter', dict(asset=audio, gain=1, loop=True, enabled=True))]
            process.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
            process.start(); process.load(args.probe)
            def snapshot():
                return dict(game=process.game(), entities=[process.entity(uid(i)) for i in (1000, 2, 4)],
                    tip=process.entity(node(uid(1000), 1)), voices=process.rpc('runtime.audio.voices', dict(session_id=process.session, tick=process.tick)))
            process.edit(Mode=1, Serial=1, Clip=0, BlendTicks=30)
            process.step(10)
            process.edit(Mode=1, Serial=2, Clip=1, BlendTicks=30)
            process.step(1)
            assert process.entity(uid(1000))['animation']['transition']['source_frozen']
            process.edit(Mode=7, Serial=3, Clip=0, BlendTicks=60, ThrowAt=str(process.tick + 2))
            before = snapshot()
            process.step(5, error=-32040)
            assert snapshot() == before
            process.edit(ThrowAt='-1')
            process.step(1)
            assert len(process.rpc('runtime.audio.voices', dict(session_id=process.session, tick=process.tick))['voices']) == 1
            assert process.game()['module']['values']['AppliedSerial'] == 3
            record['checks']['later_native_exception_restores_frozen_fade_state_physics_audio_and_queue'] = True
            process.edit(Mode=2, Serial=4)
            before = snapshot(); process.step(1, error=-32040); assert snapshot() == before
            process.edit(Mode=3, Serial=5); process.step(1)
            assert process.game()['module']['values']['NullProbe'] == 1
            process.edit(Mode=4, Serial=6)
            before = snapshot(); process.step(1, error=-32040); assert snapshot() == before
            record['checks']['native_services_duplicate_rejection_nonrig_query_and_unknown_entity'] = True
            process.close(); process = None

        # Metadata mismatch is checked before Initialize. Constructor failure
        # releases the instance but keeps the loaded NativeAOT image pinned.
        copy_root = run / 'wrong-schema'; shutil.copytree(args.door.parent, copy_root)
        descriptor = copy_root / args.door.name
        data = json.loads(descriptor.read_text()); data['schema']['identity'] = data['identity'] = 'incorrect.generated.identity'
        descriptor.write_text(json.dumps(data))
        process = Session(args.binary, run / 'schema-world.json', record)
        process.rpc(fixture['method'], fixture['params']); process.start()
        process.load(descriptor, error=-32060)
        counts = process.rpc('runtime.gameplay.collect', dict(session_id=process.session))
        assert counts['active_modules'] == 0
        data['schema']['identity'] = data['identity'] = json.loads(args.door.read_text())['identity']
        descriptor.write_text(json.dumps(data))
        process.load(descriptor)
        record['checks']['actual_generated_schema_comparison_and_failed_instance_release'] = True
        process.close(); process = None
        record['passed'] = True
    finally:
        if process is not None:
            process.process.kill(); process.process.wait(); process.stderr.close()
        (run / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(dict(passed=True, checks=len(record['checks']), calls=len(record['calls']), evidence=str(run / 'evidence.json'))))


if __name__ == '__main__':
    main()
