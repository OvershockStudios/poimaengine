#!/usr/bin/env python3
"""Real gameplay-requested restore inside World's Vulkan player and audio replay adapters."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import json
from pathlib import Path
import subprocess
import uuid
from gameplay_save_requests import ROOT, TYPE, Session, sha, inventory


def uid(number):
    return f'{number:032x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    args = parser.parse_args()
    if args.gpu < 0 or args.gpu > 4095:
        parser.error('GPU must be 0..4095.')
    if args.native_descriptor:
        if any((args.hostfxr, args.bridge, args.assembly)):
            parser.error('Choose NativeAOT or CoreCLR paths.')
    elif not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('CoreCLR needs hostfxr, bridge and assembly.')
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value
    args.native = native
    args.config = dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(
        hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(args.assembly), type=TYPE)
    run = args.output.resolve()/uuid.uuid4().hex; run.mkdir(parents=True)
    record = dict(passed=False, gpu=args.gpu, backend='native_aot' if args.native_descriptor else 'coreclr',
        binary_sha256=sha(args.binary), test_sha256=sha(__file__), helper_sha256=sha(ROOT/'tests/gameplay_save_requests.py'),
        fixture_sha256=sha(ROOT/'tests/gameplay-save/SaveRequests.cs'), calls=[], checks=[],
        limitations=['Bounded recorded replay stops on restore; not physical input or interactive continuation qualification.',
                     'Audio replay records a committed source prefix and stops on replacement; no output-device or subjective audio test.',
                     'Test-owned worlds and windows only; no desktop screenshot.'])
    for name in ('hostfxr', 'bridge', 'assembly', 'native_descriptor'):
        if getattr(args, name): record[name+'_sha256'] = sha(getattr(args, name))
    session = None
    try:
        capabilities = subprocess.run([str(args.binary.resolve()), 'capabilities'], capture_output=True, text=True, timeout=30, check=True)
        features = json.loads(capabilities.stdout)['result']['features']
        assert features['render_smoke'], features
        audio = features['audio_stream_capture']; record['audio_available'] = audio
        world = run/'world.json'; storage = run/'saves'; storage.mkdir()
        session = Session(args, world, record)
        fixture = json.loads((ROOT/'examples/interaction-room.jsonl').read_text())
        if audio:
            imported = session.rpc('asset.audio.import', dict(source=native(ROOT/'examples/assets/acoustic-probe.wav')))
            fixture['params']['ops'].append(dict(op='component.set', id=uid(2), type='AudioEmitter',
                value=dict(asset=imported['asset'], gain=.25, loop=True, enabled=True)))
        session.rpc(fixture['method'], fixture['params']); original_world = sha(world)
        session.configure(storage); session.start(); session.arm(1)
        first = dict(session_id=session.session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=3)
        if audio: first['sounds'] = [dict(op='play', emitter=uid(2))]
        saved, _ = session.step(request=first)
        assert saved['save_operation']['state'] == 3 and session.tick == 3
        saved_fields = session.values(); files = inventory(storage)
        session.step(3)
        session.edit(Mode=2, TriggerTick=session.tick+2, ExpectedGeneration=1)
        source, before = session.session, session.tick
        capture = run/'restored-player.bmp'
        request = dict(session_id=source, request_id=uuid.uuid4().hex, expected_tick=before,
            controller=uid(100), camera=uid(101), mode='replay', sequence=[dict(ticks=10, move=[0, 1])],
            gpu=args.gpu, width=512, height=288, samples=1, path=native(capture))
        played = session.rpc('runtime.play', request)
        assert played['success'] and played['stop_reason'] == 'runtime_replaced', played
        assert played['initial_session_id'] == source and played['current_session_id'] != source
        assert played['session_id'] == played['current_session_id'] and played['runtime_replacements'] == 1
        assert played['previous_tick'] == before and played['tick'] == 3
        assert played['nvrhi_errors'] == 0 and played['frames_presented'] >= 1 and played['capture_written']
        assert capture.is_file() and capture.stat().st_size > 512*288*3
        session.session, session.tick = played['current_session_id'], played['tick']
        assert session.values() == saved_fields, 'Player adapter failed exact registered state restoration.'
        captured_hash = sha(capture)
        assert session.rpc('runtime.play', request) == dict(played, replayed=True)
        assert sha(capture) == captured_hash and inventory(storage) == files
        state = session.rpc('runtime.status'); assert state['session_id'] == session.session and state['tick'] == 3
        changed = copy.deepcopy(request); changed['sequence'][0]['ticks'] = 9
        session.rpc('runtime.play', changed, error=-32010)
        session.step(); fields = session.values()
        assert fields['Ticks'] == 4 and fields['RestoreSeen'] == 1 and fields['RestoreInitiated'] == 1
        assert fields['LastState'] == 3 and fields['TicketSequence'] == '0' and fields['Requests'] == 1
        assert session.rpc('runtime.play', request) == dict(played, replayed=True)
        assert session.rpc('runtime.status')['tick'] == 4
        record['player'] = played; record['capture_sha256'] = captured_hash
        record['checks'].append('Actual World player adapter restores from Tick, stops replay, preserves source/current identities and retry receipt, and the replacement advances afterward.')

        if audio:
            session.edit(Mode=2, TriggerTick=session.tick+2, ExpectedGeneration=1)
            source, before = session.session, session.tick
            path = run/'source-prefix.wav'
            request = dict(session_id=source, request_id=uuid.uuid4().hex, expected_tick=before,
                listener=uid(101), path=native(path), sequence=[dict(ticks=10)])
            recorded = session.rpc('runtime.audio.replay', request)
            assert recorded['success'] and recorded['stop_reason'] == 'runtime_replaced', recorded
            assert recorded['session_id'] == source and recorded['current_session_id'] != source and recorded['tick'] == 3
            # AudioStream emits complete 512-frame blocks until finish. A load
            # discards the old stream, so the last 64 samples are not flushed.
            assert recorded['capture']['frames'] == (2*800//512)*512 and recorded['stream']['peak'] > 0
            assert recorded['stream']['blocks'] == 3
            assert recorded['capture']['sha256'] == sha(path)
            session.session, session.tick = recorded['current_session_id'], recorded['tick']
            assert session.values() == saved_fields
            wave_hash = sha(path)
            assert session.rpc('runtime.audio.replay', request) == dict(recorded, replayed=True)
            assert sha(path) == wave_hash and session.rpc('runtime.status')['tick'] == 3
            changed = copy.deepcopy(request); changed['sequence'][0]['ticks'] = 9
            session.rpc('runtime.audio.replay', changed, error=-32010)
            session.step(); assert session.values()['Ticks'] == 4
            record['audio'] = recorded
            record['checks'].append('Audio replay services the real gameplay load, records the complete 512-frame blocks from two source ticks before replacement, and old-source retries neither rewrite nor advance.')
        else:
            record['audio_skipped'] = 'This executable reports audio_stream_capture=false.'
        assert inventory(storage) == files and sha(world) == original_world
        record['checks'].append('Authored world and checkpoint bytes stay unchanged during player/audio loads and exact retries.')
        record['world_sha256'] = original_world; record['passed'] = True
        session.close(); session = None
    except BaseException as error:
        record['error'] = repr(error)
        raise
    finally:
        if session is not None:
            try: session.close()
            except Exception as error: record['shutdown_error'] = repr(error)
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(run/'evidence.json')


if __name__ == '__main__':
    main()
