#!/usr/bin/env python3
"""Compile real C# gameplay and qualify animation services through public RPC."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import threading
import time
import uuid
import wave
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from runtime_animation_blend_contract import blend_fixture, command, component, node, transform, uid
from gltf_fixture import glb

PROBE = r'''using Poima;
using System.Numerics;
namespace Poima.Verification;
public struct ProbeState
{
    public EntityId Rig, Plain, Body, Emitter;
    public int Mode, Serial, AppliedSerial, Clip, Loop, Playing, BlendTicks, Count, Ticks, NullProbe;
    public double Time, Speed;
    public long ThrowAt, LastVoice, QueryTick, DelayUntil;
    public int QClip, QLoop, QPlaying, QHasTransition, QSourceFrozen, QSourceClip, QSourceLoop, QSourcePlaying;
    public double QTime, QSpeed, QDuration, QWeight, QSourceTime, QSourceSpeed;
    public long QStartTick;
    public int QDurationTicks, QElapsedTicks;
}
[GameModule("poima.verification.animation-services")]
public sealed class ProbeGame : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state)
    {
        state.Rig = new(0, 1000); state.Plain = new(0, 3);
        state.Body = new(0, 2); state.Emitter = new(0, 20);
        state.Clip = 0; state.Speed = 1; state.Loop = state.Playing = 1;
        state.ThrowAt = -1;
    }
    private static void Observe(ref ProbeState s, AnimationState? query, ulong tick)
    {
        s.QueryTick = checked((long)tick);
        if (query is not { } a) { s.QClip = -2; return; }
        s.QClip = a.Clip ?? -1; s.QTime = a.Time; s.QSpeed = a.Speed;
        s.QLoop = a.Loop ? 1 : 0; s.QPlaying = a.Playing ? 1 : 0; s.QDuration = a.Duration;
        s.QHasTransition = a.Transition.HasValue ? 1 : 0;
        if (a.Transition is not { } t) return;
        s.QStartTick = checked((long)t.StartTick); s.QDurationTicks = checked((int)t.DurationTicks);
        s.QElapsedTicks = checked((int)t.ElapsedTicks); s.QWeight = t.Weight;
        s.QSourceFrozen = t.SourceFrozen ? 1 : 0; s.QSourceClip = t.SourceClip ?? -1;
        s.QSourceTime = t.SourceTime ?? -1; s.QSourceSpeed = t.SourceSpeed ?? -1;
        s.QSourceLoop = t.SourceLoop.HasValue ? (t.SourceLoop.Value ? 1 : 0) : -1;
        s.QSourcePlaying = t.SourcePlaying.HasValue ? (t.SourcePlaying.Value ? 1 : 0) : -1;
    }
    private static void Set(ref ProbeState s, GameContext c, EntityId id)
        => c.SetAnimation(id, s.Clip < 0 ? null : s.Clip, s.Time, s.Speed,
                          s.Loop != 0, s.Playing != 0, checked((uint)s.BlendTicks));
    public override void Tick(ref ProbeState state, GameContext context)
    {
        ++state.Ticks;
        var before = context.GetAnimation(state.Rig);
        Observe(ref state, before, context.Tick);
        if (state.Serial != state.AppliedSerial && (state.Mode != 6 || context.Tick >= (ulong)state.DelayUntil))
        {
            switch (state.Mode)
            {
                case 1: case 6: Set(ref state, context, state.Rig); break;
                case 2: Set(ref state, context, state.Rig); Set(ref state, context, state.Rig); break;
                case 3:
                    if (context.GetAnimation(state.Plain) is not null) throw new InvalidOperationException("Non-rig had animation.");
                    state.NullProbe = 1; break;
                case 4: context.GetAnimation(new(0, 999999)); break;
                case 5:
                    for (int i = 0; i < state.Count; ++i) Set(ref state, context, new(state.Rig.High, state.Rig.Low + (ulong)i));
                    break;
                case 7:
                    Set(ref state, context, state.Rig);
                    context.MoveKinematic(state.Body, new(7, 1, 0), Quaternion.Identity, 60);
                    state.LastVoice = context.PlaySound(state.Emitter);
                    break;
            }
            if (!Nullable.Equals(before, context.GetAnimation(state.Rig)))
                throw new InvalidOperationException("Queued animation became visible before Tick returned.");
            state.AppliedSerial = state.Serial;
        }
        if (state.ThrowAt >= 0 && context.Tick >= (ulong)state.ThrowAt)
            throw new InvalidOperationException("Intentional later managed tick failure.");
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ['binary', 'dotnet', 'hostfxr', 'bridge', 'output']:
        parser.add_argument('--' + field, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    run = args.output / uuid.uuid4().hex
    run.mkdir(parents=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    record = {'passed': False, 'run': str(run), 'binary_sha256': sha(args.binary),
              'bridge_sha256': sha(args.bridge), 'sdk_sha256': sha(args.bridge.parent / 'Poima.Gameplay.dll'),
              'test_sha256': sha(Path(__file__)), 'builds': [], 'calls': [], 'checks': {}}

    def native(path, windows=None):
        if args.windows_interop if windows is None else windows:
            return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip()
        return str(path.resolve())

    def build(name, text):
        directory = run / name
        directory.mkdir()
        source = directory / 'Game.cs'
        source.write_text(text)
        project = directory / 'Poima.AnimationVerification.csproj'
        hint = escape(native(args.bridge.parent / 'Poima.Gameplay.dll', args.dotnet.suffix.lower() == '.exe'))
        project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework>'
            '<LangVersion>14.0</LangVersion><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>'
            '<TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic></PropertyGroup>'
            '<ItemGroup><Reference Include="Poima.Gameplay"><HintPath>' + hint + '</HintPath></Reference></ItemGroup></Project>')
        env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false')
        started = time.monotonic()
        result = subprocess.run([str(args.dotnet.resolve()), 'build', native(project, args.dotnet.suffix.lower() == '.exe'),
                                 '-c', 'Release', '--nologo'], capture_output=True, text=True, env=env, timeout=120)
        item = {'name': name, 'source_sha256': sha(source), 'exit_code': result.returncode,
                'milliseconds': (time.monotonic() - started) * 1000, 'log': result.stdout + result.stderr}
        record['builds'].append(item)
        assert result.returncode == 0, item
        assembly = directory / 'bin/Release/net10.0/Poima.AnimationVerification.dll'
        item['assembly_sha256'] = sha(assembly)
        return assembly

    process = None
    stderr = None
    try:
        base = build('base', PROBE)
        upgraded_source = PROBE.replace('public int Mode,', 'public int ReloadMarker; public int Mode,')
        upgraded_source = upgraded_source.replace('state.ThrowAt = -1;', 'state.ThrowAt = -1; state.ReloadMarker = 17;')
        upgraded_source = upgraded_source.replace('++state.Ticks;', '++state.Ticks; ++state.ReloadMarker;')
        upgrade = build('upgrade', upgraded_source)
        example_source = ROOT / 'examples/managed/AnimationGame/AnimationGame.cs'
        example = build('example', example_source.read_text())
        record['example_sha256'] = sha(example_source)
        world = run / 'world.json'
        stderr_path = run / 'engine.stderr.log'
        stderr = stderr_path.open('w', encoding='utf-8')
        process = subprocess.Popen([str(args.binary.resolve()), 'world', native(world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True, encoding='utf-8', bufsize=1)
        output = queue.Queue()
        def reader():
            for line in process.stdout:
                output.put(line)
            output.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def rpc(method, params=None, error=None):
            request = {'jsonrpc': '2.0', 'id': len(record['calls']) + 1, 'method': method, 'params': params or {}}
            process.stdin.write(json.dumps(request) + '\n')
            process.stdin.flush()
            line = output.get(timeout=60)
            assert line is not None, stderr_path.read_text()
            response = json.loads(line)
            record['calls'].append({'request': request, 'response': response})
            assert response['id'] == request['id'], response
            if error is not None:
                assert response.get('error', {}).get('code') == error, response
                return response
            assert 'result' in response, response
            return response['result']

        source = run / 'original.glb'
        doc, blob = blend_fixture()
        source.write_bytes(glb(doc, blob))
        asset = rpc('asset.import', {'source': native(source)})['asset']
        source.unlink()
        audio = run / 'original.wav'
        with wave.open(str(audio), 'wb') as wav:
            wav.setparams((1, 2, 48000, 4800, 'NONE', 'not compressed'))
            wav.writeframes(struct.pack('<' + 'h' * 4800, *([1000, -1000] * 2400)))
        sound_asset = rpc('asset.audio.import', {'source': native(audio)})['asset']
        audio.unlink()
        ops = [{'op': 'asset.instantiate', 'id': uid(1000 + i), 'name': f'Rig {i}', 'asset': asset} for i in range(65)]
        def create(n, position):
            return [{'op': 'entity.create', 'id': uid(n), 'name': f'Entity {n}'}, component(uid(n), 'Transform', transform(position))]
        def box(n, motion, half):
            return component(uid(n), 'BoxCollider', {'half_extents': half, 'motion': motion, 'mass': 1, 'friction': .5, 'restitution': 0})
        ops += create(1, (0, -.5, 0)) + [box(1, 'static', [20, .5, 20])]
        ops += create(2, (5, 1, 0)) + [box(2, 'kinematic', [.5, .5, .5])]
        ops += create(3, (0, 0, 0)) + create(4, (5, 3, 0)) + [box(4, 'dynamic', [.25, .25, .25])]
        ops += create(10, (-5, 1, 0)) + create(11, (0, 1.6, 0))
        ops += [{'op': 'entity.reparent', 'id': uid(11), 'parent': uid(10), 'mode': 'keep_local'},
                component(uid(11), 'Camera', {'vertical_fov': 60, 'near': .1, 'far': 100}),
                component(uid(10), 'CharacterController', {'radius': .3, 'height': 1.8, 'speed': 4, 'jump_speed': 5, 'camera': uid(11)})]
        ops += create(20, (0, 1, 0)) + [component(uid(20), 'AudioEmitter', {'asset': sound_asset, 'gain': 1, 'loop': True, 'enabled': True})]
        rpc('world.transact', {'request_id': uuid.uuid4().hex, 'base_revision': 0, 'ops': ops})
        authored = world.read_bytes()
        rig = uid(1000)
        tip = node(rig, 1)
        session, tick, revision, serial, probe = None, 0, 0, 0, True

        def start(number):
            nonlocal session, tick, revision, serial, probe
            if session:
                rpc('runtime.stop', {'session_id': session})
            session, tick, revision, serial, probe = uid(number), 0, 0, 0, True
            rpc('runtime.start', {'session_id': session, 'revision': 1})

        def inspect():
            return rpc('runtime.gameplay.inspect', {'session_id': session, 'tick': tick})

        def values():
            return inspect()['module']['values']

        def load(assembly, typename='Poima.Verification.ProbeGame', initial=None):
            nonlocal revision
            params = {'session_id': session, 'request_id': uuid.uuid4().hex, 'expected_tick': tick,
                      'expected_revision': revision, 'hostfxr': native(args.hostfxr), 'bridge': native(args.bridge),
                      'assembly': native(assembly), 'type': typename}
            if initial is not None:
                params['values'] = initial
            result = rpc('runtime.gameplay.load', params)
            revision += 1
            assert result['revision'] == revision, result
            return result

        def edit(**fields):
            nonlocal revision
            result = rpc('runtime.gameplay.edit', {'session_id': session, 'request_id': uuid.uuid4().hex,
                'expected_tick': tick, 'expected_revision': revision, 'values': fields})
            revision += 1
            assert result['revision'] == revision, result

        def configure(mode=1, **fields):
            nonlocal serial
            serial += 1
            edit(Mode=mode, Serial=serial, **fields)

        def entity(id=tip):
            return rpc('runtime.entity', {'session_id': session, 'tick': tick, 'id': id})

        def animation(id=rig):
            return entity(id)['animation']

        def check_query(expected, observed, query_tick):
            assert int(observed['QueryTick']) == query_tick, observed
            mapping = {'QClip': expected['clip'] if expected['clip'] is not None else -1,
                       'QTime': expected['time'], 'QSpeed': expected['speed'], 'QLoop': int(expected['loop']),
                       'QPlaying': int(expected['playing']), 'QDuration': expected['duration'],
                       'QHasTransition': int(expected['transition'] is not None)}
            if expected['transition'] is not None:
                t = expected['transition']
                mapping.update(QStartTick=t['start_tick'], QDurationTicks=t['duration_ticks'], QElapsedTicks=t['elapsed_ticks'],
                    QWeight=t['weight'], QSourceFrozen=int(t['source_frozen']), QSourceClip=t['source_clip'] if t['source_clip'] is not None else -1,
                    QSourceTime=t['source_time'] if t['source_time'] is not None else -1,
                    QSourceSpeed=t['source_speed'] if t['source_speed'] is not None else -1,
                    QSourceLoop=int(t['source_loop']) if t['source_loop'] is not None else -1,
                    QSourcePlaying=int(t['source_playing']) if t['source_playing'] is not None else -1)
            for key, value in mapping.items():
                assert abs(float(observed[key]) - value) < 1e-10, (key, observed[key], value)

        def step(count, animations=(), inputs=(), error=None, query_expected=None):
            nonlocal tick
            before = animation() if probe and count == 1 and error is None else None
            result = rpc('runtime.step', {'session_id': session, 'request_id': uuid.uuid4().hex,
                'expected_tick': tick, 'ticks': count, 'animations': list(animations), 'inputs': list(inputs)}, error)
            if error is None:
                old = tick
                tick += count
                assert result['tick'] == tick, result
                if before is not None:
                    check_query(query_expected if query_expected is not None else before, values(), old)
            return result

        def position(expected):
            actual = entity()['local_transform']['position']
            assert all(abs(a - b) < 1e-7 for a, b in zip(actual, expected)), (actual, expected)

        def snapshot():
            return {'game': inspect(), 'rigs': [entity(uid(1000 + i)) for i in range(65)], 'tip': entity(),
                    'bodies': [entity(uid(i)) for i in [1, 2, 4, 10, 11]],
                    'voices': rpc('runtime.audio.voices', {'session_id': session, 'tick': tick})}

        start(900)
        load(base)
        configure(3)
        step(1)
        assert values()['NullProbe'] == 1
        configure(4)
        before = snapshot()
        step(1, error=-32040)
        assert snapshot() == before
        configure(0)
        step(1)
        record['checks']['existing_nonrig_null_unknown_rejects_and_recovery'] = True
        immediate = dict(clip=0, time=.75, speed=1, loop=False, playing=False, duration=2, transition=None)
        step(1, [command(rig, time=.75, playing=False, loop=False)], query_expected=immediate)
        position((.75, 1, 0))
        record['checks']['explicit_first_tick_command_visible_to_managed_query'] = True

        configure(Clip=0, Time=.5, Speed=1, BlendTicks=0)
        step(30)
        position((1, 1, 0))
        configure(Clip=1, Time=.25, Speed=2, BlendTicks=60)
        step(15)
        position((1.25, 1.5, 0))
        step(15)
        position((1.125, 2, 0))
        assert animation()['transition']['weight'] == .5
        configure(Clip=0, Time=1.5, Playing=0, BlendTicks=30)
        step(15)
        position((1.3125, 1.5, 0))
        assert animation()['transition']['source_frozen']
        step(1)  # Compare every frozen metadata field through the real C# query.
        step(14)
        position((1.5, 1, 0))
        configure(Clip=-1, BlendTicks=30)
        step(15)
        position((.75, 1, 0))
        step(1)  # Ordinary outgoing source metadata, incoming rest state.
        step(14)
        position((0, 1, 0))
        assert animation()['transition'] is None
        record['checks']['advancing_clocks_interruption_rest_and_query_metadata'] = True

        configure(Clip=1, Time=.5, Playing=0, Speed=1, BlendTicks=60)
        step(20)
        prior = [entity(), animation()]
        state_before = values()
        loaded = load(upgrade)
        assert [entity(), animation()] == prior
        for key, value in state_before.items():
            assert loaded['module']['values'][key] == value, key
        assert loaded['module']['values']['ReloadMarker'] == 17
        step(10)
        assert animation()['transition']['weight'] == .5
        assert values()['ReloadMarker'] == 27
        step(1)
        assert rpc('runtime.gameplay.collect', {'session_id': session})['retired_alive'] == 0
        record['checks']['reload_preserves_state_and_active_transition_new_code_runs'] = True

        start(901)
        load(base)
        # Every error must restore clocks, managed bytes, all bodies, and sound handles.
        for mode, fields in [(2, {}), (1, {'Clip': 999}), (1, {'Time': -1}),
                             (1, {'Speed': 9}), (1, {'BlendTicks': 3601}), (1, {'Rig': uid(3)})]:
            configure(mode, Clip=fields.get('Clip', 0), Time=fields.get('Time', 0),
                Speed=fields.get('Speed', 1), BlendTicks=fields.get('BlendTicks', 30), Rig=fields.get('Rig', rig))
            before = snapshot()
            step(3, error=-32040)
            assert snapshot() == before
            configure(0, Rig=rig, Clip=0, Time=0, Speed=1, BlendTicks=0)
            step(1)
            assert animation()['clip'] is None and animation()['transition'] is None
        configure(Clip=1, BlendTicks=30)
        before = snapshot()
        step(3, [command(rig)], error=-32040)
        assert snapshot() == before
        configure(0)
        step(1)
        assert animation()['clip'] is None
        record['checks']['duplicate_invalid_and_native_caller_conflicts_atomic_no_stale_queue'] = True
        configure(6, DelayUntil=str(tick + 2), Clip=1, Time=.5, Speed=1, Playing=0, BlendTicks=0)
        first_tick = tick
        step(4, [command(rig, time=.25, playing=False)])
        position((1.5, 3, 0))
        assert values()['AppliedSerial'] == serial
        check_query(dict(clip=1, time=.5, speed=1, loop=True, playing=False, duration=2, transition=None),
                    values(), first_tick + 3)
        record['checks']['caller_target_can_receive_managed_command_on_later_batch_tick'] = True

        configure(5, Count=64, Clip=1, Time=.5, Playing=0, BlendTicks=0)
        step(1)
        assert all(animation(uid(1000 + i))['clip'] == 1 for i in range(64))
        assert animation(uid(1064))['clip'] is None
        configure(5, Count=65)
        before = snapshot()
        step(1, error=-32040)
        assert snapshot() == before
        configure(5, Count=64)
        before = snapshot()
        step(1, [command(uid(1064))], error=-32040)
        assert snapshot() == before
        configure(5, Count=63)
        step(1, [command(uid(1064), time=.5, playing=False)])
        assert animation(uid(1064))['clip'] == 0
        record['checks']['exact_64_limit_and_combined_caller_limit'] = True

        start(902)
        load(base)
        for kind in ['managed_throw', 'invalid_sample']:
            configure(7, Clip=1 if kind == 'managed_throw' else 2, Time=0, Speed=1, Playing=1,
                      BlendTicks=120, ThrowAt=str(tick + 2) if kind == 'managed_throw' else '-1')
            before = snapshot()
            step(5 if kind == 'managed_throw' else 50, error=-32040)
            assert snapshot() == before, kind
            configure(0, ThrowAt='-1')
            step(1)
            assert animation()['clip'] is None and animation()['transition'] is None
            assert entity(uid(2))['kinematic_target'] is None
            voices = rpc('runtime.audio.voices', {'session_id': session, 'tick': tick})
            assert voices['next_voice'] == 1 and voices['voices'] == [], voices
        configure(Clip=1, Time=.5, Playing=0, BlendTicks=60)
        step(15)
        configure(Clip=0, Time=1, Playing=0, BlendTicks=60)
        step(15)
        assert animation()['transition']['source_frozen']
        configure(7, Clip=1, Time=1, Playing=1, BlendTicks=120, ThrowAt=str(tick + 2))
        before = snapshot()
        previous_elapsed = animation()['transition']['elapsed_ticks']
        step(5, error=-32040)
        assert snapshot() == before
        configure(0, ThrowAt='-1')
        step(1)
        assert animation()['transition']['source_frozen']
        assert animation()['transition']['elapsed_ticks'] == previous_elapsed + 1
        assert entity(uid(2))['kinematic_target'] is None
        assert rpc('runtime.audio.voices', {'session_id': session, 'tick': tick})['next_voice'] == 1
        record['checks']['managed_failure_restores_existing_frozen_interrupted_transition'] = True
        # Prove the same services can commit after those failures, not just reject.
        configure(7, Clip=1, Playing=0, Time=.5, BlendTicks=30)
        step(3)
        assert entity(uid(2))['kinematic_target'] is not None
        voices = rpc('runtime.audio.voices', {'session_id': session, 'tick': tick})
        assert voices['next_voice'] == 2 and len(voices['voices']) == 1
        assert animation()['transition']['elapsed_ticks'] == 3
        record['checks']['later_throw_and_sample_failure_restore_animation_physics_sound_game_state'] = True

        partitions = []
        for number, chunks in [(903, [45]), (904, [7, 11, 27])]:
            start(number)
            load(base)
            configure(Clip=1, Time=0, Speed=2, BlendTicks=120)
            step(chunks[0], [command(uid(1001), time=1, playing=False)])
            for count in chunks[1:]:
                step(count)
            partitions.append({'first': entity(), 'other': entity(node(uid(1001), 1)),
                               'animation': animation(), 'values': values(), 'body': entity(uid(4))})
        for result in partitions:
            for field in ['first', 'other', 'body']:
                result[field].pop('session_id')
        assert partitions[0] == partitions[1]
        assert partitions[0]['other']['local_transform']['position'] == [1, 1, 0]
        record['checks']['partition_equivalence_and_independent_rigs'] = True

        start(905)
        probe = False
        load(example, 'Poima.Examples.AnimationGame', {'Rig': rig})
        step(15, inputs=[{'entity': uid(10), 'use': True}])
        assert values()['Changes'] == 1 and values()['NextClip'] == 1
        assert animation()['clip'] == 0 and animation()['transition']['weight'] == .5
        step(15, inputs=[{'entity': uid(10), 'use': True}])
        assert values()['Changes'] == 2 and animation()['clip'] == 1
        assert animation()['transition']['source_frozen']
        step(15)
        assert animation()['transition'] is None and values()['Changes'] == 2
        position((1.5, 3, 0))
        assert world.read_bytes() == authored
        record['checks']['published_example_compiles_runs_and_use_edge_once_per_batch'] = True
        record['checks']['authored_world_bytes_unchanged'] = True
        record['passed'] = True
    finally:
        if process is not None:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            record['exit_code'] = process.returncode
            process.stdout.close()
        if stderr is not None:
            stderr.close()
            record['stderr'] = (run / 'engine.stderr.log').read_text()
        (args.output / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
        print(args.output / 'evidence.json')
    print(json.dumps({'passed': record['passed'], 'checks': record['checks']}))


if __name__ == '__main__':
    main()
