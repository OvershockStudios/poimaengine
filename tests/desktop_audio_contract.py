#!/usr/bin/env python3
"""Real Windows desktop audio lifecycle; dummy output by default, actual device always muted."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid
import wave


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def uid(number):
    return f'{number:032x}'


class Desktop:
    def __init__(self, bridge, world, record):
        self.record, self.sequence = record, 0
        self.search = os.add_dll_directory(str(bridge.resolve().parent))
        self.library = ctypes.CDLL(str(bridge.resolve()))
        pointer, string = ctypes.c_void_p, ctypes.c_char_p
        for suffix, arguments, result in [
                ('create', [string, string, ctypes.c_int32, ctypes.c_uint32], pointer),
                ('call', [pointer, string], pointer), ('poll', [pointer], pointer),
                ('error', [pointer], pointer), ('destroy', [pointer], None)]:
            function = getattr(self.library, 'poima_desktop_' + suffix)
            function.argtypes, function.restype = arguments, result
        self.host = self.library.poima_desktop_create(str(world).encode(), None, -1, 1)
        check(self.host, self.decode(self.library.poima_desktop_error(None)))

    @staticmethod
    def decode(pointer):
        return ctypes.string_at(pointer).decode('utf-8') if pointer else ''

    def rpc(self, method, params=None, error=None):
        self.sequence += 1
        request = {'jsonrpc': '2.0', 'id': self.sequence, 'method': method,
                   'params': {} if params is None else params}
        pointer = self.library.poima_desktop_call(self.host, json.dumps(request).encode())
        check(pointer, self.decode(self.library.poima_desktop_error(self.host)))
        reply = json.loads(self.decode(pointer))
        self.record['calls'].append({'request': request, 'reply': reply})
        check(reply.get('id') == self.sequence, reply)
        if error is not None:
            check(reply.get('error', {}).get('code') == error, reply)
            return reply['error']
        check('result' in reply, reply)
        return reply['result']

    def poll(self):
        started = time.perf_counter()
        pointer = self.library.poima_desktop_poll(self.host)
        duration = time.perf_counter() - started
        check(pointer, self.decode(self.library.poima_desktop_error(self.host)))
        state = json.loads(self.decode(pointer))
        self.record['polls'].append({'seconds': duration, 'audio': state['audio'],
                                     'playback': state['playback'], 'closing': state['closing']})
        return state

    def wait(self, predicate, description, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(.025)
            state = self.poll()
            check(state['audio']['error'] is None, state['audio'])
            check(state['playback']['last_error'] is None, state['playback'])
            if predicate(state):
                return state
        raise AssertionError('Timed out waiting for ' + description)

    def audio(self):
        return self.rpc('desktop.audio.inspect')

    def destroy(self):
        if self.host:
            self.library.poima_desktop_destroy(self.host)
            self.host = None
        self.search.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--driver', choices=['dummy', 'actual'], default='dummy')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Run with native Windows Python. No HWND or GPU is created.')
    # Set before loading any SDL-using DLL. Actual output is forcibly muted in
    # every accepted configuration, regardless of the environment/device name.
    if args.driver == 'dummy':
        os.environ['SDL_AUDIO_DRIVER'] = 'dummy'
    else:
        os.environ.pop('SDL_AUDIO_DRIVER', None)
    muted = args.driver == 'actual'
    root = Path(__file__).resolve().parents[1]
    run = args.output.resolve() / ('run-' + uuid.uuid4().hex)
    run.mkdir(parents=True)
    world = run / 'world.json'
    source_files = ['src/desktop_bridge.cpp', 'src/editor_audio.cpp', 'src/editor_audio.hpp',
                    'src/world.cpp', 'include/poima/world.hpp', 'src/audio_stream.cpp']
    record = {'success': False, 'driver_requested': args.driver, 'actual_output_forced_muted': muted,
              'binary_sha256': digest(args.binary), 'bridge_sha256': digest(args.bridge),
              'test_sha256': digest(Path(__file__)),
              'source_sha256': {p: digest(root / p) for p in source_files if (root / p).exists()},
              'checks': [], 'calls': [], 'polls': [],
              'limitations': ['Headless real native bridge and Steam Audio DSP; no HWND, GPU, physical input, loopback or subjective listening qualification.',
                              'Dummy-driver output cannot reach hardware. Actual-driver cohort is muted for its entire lifetime.',
                              'Poll timings are descriptive on a shared machine, not a real-time scheduling guarantee.']}
    host = None
    try:
        with wave.open(str(run / 'tone.wav'), 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(48000)
            wav.writeframes(b''.join(struct.pack('<h', round(1200 * math.sin(2 * math.pi * 440 * n / 48000))) for n in range(48000)))
        host = Desktop(args.bridge, world, record)
        rpc = host.rpc
        methods = rpc('desktop.describe')['methods']
        check(all(name in methods for name in ['desktop.audio.configure', 'desktop.audio.inspect',
              'desktop.audio.retry', 'desktop.close.begin']), 'Missing audio/close discovery')
        initial = host.audio()
        check(initial['available'] and not initial['config']['enabled'] and not initial['device_open']
              and not initial['worker_started'], initial)
        asset = rpc('asset.audio.import', {'source': str(run / 'tone.wav')})['asset']
        room = json.loads((root / 'examples/interaction-room.jsonl').read_text(encoding='utf-8'))
        room['params']['ops'].append({'op': 'component.set', 'id': uid(2), 'type': 'AudioEmitter',
                                    'value': {'asset': asset, 'gain': .25, 'loop': True, 'enabled': True}})
        room['params']['ops'].append({'op': 'component.set', 'id': uid(2), 'type': 'AcousticMaterial',
                                    'value': {'absorption': [.1, .2, .3], 'transmission': [.5, .15, .03],
                                              'scattering': .5, 'enabled': True}})
        revision = rpc(room['method'], room['params'])['revision']
        original, history = world.read_bytes(), rpc('world.history')
        config = dict(request_id=uuid.uuid4().hex, expected_generation=0, enabled=True, muted=muted, volume=.25)
        accepted = rpc('desktop.audio.configure', config)
        check(accepted['generation'] == 1 and not accepted['replayed'], accepted)
        check(rpc('desktop.audio.configure', config) == {**accepted, 'replayed': True}, 'Exact audio receipt changed')
        stopped = host.poll()['audio']
        check(not stopped['worker_started'] and not stopped['device_open'] and stopped['stream']['frames'] == 0, stopped)
        for changes in [dict(enabled=1), dict(muted=0), dict(volume=True), dict(volume=-.1),
                        dict(volume=1.1), dict(volume='0.5'), dict(volume=None), dict(extra=1)]:
            rpc('desktop.audio.configure', {**config, 'request_id': uuid.uuid4().hex,
                                            'expected_generation': 1, **changes}, error=-32602)
        rpc('desktop.audio.configure', {**config, 'request_id': uuid.uuid4().hex}, error=-32009)
        rpc('desktop.audio.configure', {**config, 'volume': .5}, error=-32009)
        after = host.audio()
        check(after['generation'] == 1 and after['config'] == stopped['config'] and after['epoch'] == stopped['epoch'], after)
        record['checks'].append('Strict complete configuration, stale/conflicting guards, exact receipts, and enabled-stopped laziness')

        sid = uuid.uuid4().hex
        rpc('desktop.play.start', dict(session_id=sid, revision=revision, paused=True))
        rpc('desktop.play.step', dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1,
                                     sounds=[dict(op='play', emitter=uid(2), gain=.5)]))
        voices = rpc('runtime.audio.voices', dict(session_id=sid, tick=1))
        record['initial_voices'] = voices
        rpc('desktop.play.resume', dict(session_id=sid))
        no_listener = host.poll()['audio']
        check(not no_listener['device_open'] and no_listener['last_submitted_tick'] == 0, no_listener)
        rpc('desktop.game.camera', {'camera': uid(101)})
        audible = host.wait(lambda state: state['audio']['stream']['frames'] >= 4096
                           and state['audio']['stream']['voices_started'] >= 1 and state['audio']['device_open'],
                           'actual worker DSP/device initialization')['audio']
        check(audible['listener'] == uid(101) and audible['session'] == sid and audible['stream']['peak'] > 0, audible)
        if args.driver == 'dummy':
            audible = host.wait(lambda state: state['audio']['submitted_frames'] > 0, 'dummy stream submission')['audio']
            check(audible['driver'] == 'dummy', audible)
        else:
            check(audible['submitted_frames'] == 0 and audible['config']['muted'], audible)
            check(audible['driver'] not in ['', 'dummy', 'disk', 'unknown'], audible)
        record['checks'].append('Real committed logical voice drives persistent Steam Audio PCM; listener baseline is lazy and device mode is safe')

        # The legacy Game selection is sufficient for semantic focus without a
        # window. Focus release must not mute independently running audio.
        rpc('desktop.view', {'mode': 'game', 'camera': uid(101)})
        rpc('desktop.input.configure', {'session_id': sid, 'controller': uid(100)})
        rpc('desktop.input.focus', {'session_id': sid, 'focused': True})
        focus_audio = host.wait(lambda state: state['audio']['active'], 'audio after Game selection')['audio']
        rpc('desktop.input.focus', {'session_id': sid, 'focused': False})
        continued = host.wait(lambda state: state['audio']['processed_tick'] > focus_audio['processed_tick'],
                              'audio while game input focus is released')['audio']
        check(continued['active'] and continued['epoch'] == focus_audio['epoch'], continued)
        before_invalid = host.audio()
        rpc('desktop.audio.configure', {**config, 'request_id': uuid.uuid4().hex,
                                        'expected_generation': 1, 'volume': 2}, error=-32602)
        after_invalid = host.audio()
        check(after_invalid['config'] == before_invalid['config'] and after_invalid['epoch'] == before_invalid['epoch'], after_invalid)
        record['checks'].append('Input focus and audio activation are independent; failed live reconfiguration preserves the timeline')

        rpc('desktop.play.pause', {'session_id': sid})
        paused = host.audio()
        check(not paused['active'] and paused['queued_frames'] == 0 and paused['device_queued_frames'] == 0
              and paused['epoch'] > continued['epoch'], paused)
        for _ in range(3):
            host.poll()
        check(host.audio()['epoch'] == paused['epoch'], 'Repeated paused poll invalidated the timeline again')
        tick = rpc('desktop.play.inspect')['tick']
        rpc('desktop.play.step', dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=tick, ticks=37))
        manual = host.poll()['audio']
        check(not manual['active'] and manual['queued_frames'] == 0 and manual['device_queued_frames'] == 0, manual)
        rpc('desktop.play.resume', {'session_id': sid})
        resumed = host.wait(lambda state: state['audio']['active'] and state['audio']['processed_tick'] > tick + 37,
                            'new audio baseline after explicit tick gap')['audio']
        check(resumed['epoch'] >= paused['epoch'] and resumed['error'] is None, resumed)
        old_listener_epoch = resumed['epoch']
        rpc('desktop.game.camera', {'camera': uid(200)})
        moved = host.wait(lambda state: state['audio']['listener'] == uid(200) and state['audio']['active'],
                         'replacement listener baseline')['audio']
        check(moved['epoch'] > old_listener_epoch, moved)
        rpc('desktop.game.camera', {'camera': uid(999)}, error=-32000)
        check(host.audio()['listener'] == uid(200) and host.audio()['epoch'] == moved['epoch'], 'Rejected camera selection altered audio')
        record['checks'].append('Pause clears output idempotently; manual multi-tick gaps and listener changes rebase without invalid stream advances')

        rpc('desktop.play.pause', {'session_id': sid})
        (run / 'saves').mkdir()
        routing = rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(run / 'saves')))['generation']
        saved_tick = rpc('desktop.play.inspect')['tick']
        rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=routing, slot='audio',
                              expected_generation=0, session_id=sid, expected_tick=saved_tick, expected_gameplay_revision=0))
        rpc('desktop.play.step', dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=saved_tick, ticks=2,
                                     sounds=[dict(op='stop', voice=1)]))
        replacement = uuid.uuid4().hex
        before_load = host.audio()['epoch']
        load = dict(request_id=uuid.uuid4().hex, configuration_generation=routing, slot='audio', expected_generation=1,
                    revision=revision, expected_session_id=sid, expected_tick=saved_tick+2, expected_gameplay_revision=0,
                    new_session_id=replacement)
        restored = rpc('save.load', load)
        check(restored['session_id'] == replacement and restored['tick'] == saved_tick, restored)
        after_load = host.poll()
        check(not after_load['audio']['active'] and after_load['audio']['queued_frames'] == 0
              and after_load['audio']['device_queued_frames'] == 0 and after_load['playback']['state'] == 'paused', after_load)
        sid = replacement
        rpc('desktop.play.resume', {'session_id': sid})
        new_audio = host.wait(lambda state: state['audio']['session'] == sid and state['audio']['active']
                             and state['audio']['processed_tick'] > saved_tick, 'restored runtime audio')['audio']
        check(new_audio['epoch'] >= before_load and new_audio['epoch'] > moved['epoch'] and new_audio['listener'] == uid(200), new_audio)
        retry_epoch = host.audio()['epoch']
        # Save/load is blocked while playing, even for its old receipt. Pause
        # first; an exact retry must not recreate/reseed the restored session.
        rpc('desktop.play.pause', {'session_id': sid})
        paused_retry_epoch = host.audio()['epoch']
        check(rpc('save.load', load)['replayed'], 'Load receipt not retained')
        check(host.audio()['epoch'] == paused_retry_epoch, 'Exact load retry invalidated presentation')
        check(paused_retry_epoch >= retry_epoch, 'Audio epoch went backward')
        record['checks'].append('Actual saved logical voice survives fresh-runtime load; old timeline is invalidated, paused, and exact retry is inert')

        rpc('desktop.play.stop', {'session_id': sid})
        stopped = host.poll()['audio']
        check(not stopped['active'] and stopped['queued_frames'] == 0 and stopped['device_queued_frames'] == 0, stopped)
        sid = uuid.uuid4().hex
        rpc('desktop.play.start', dict(session_id=sid, revision=revision, paused=True))
        rpc('desktop.play.step', dict(session_id=sid, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1,
                                     sounds=[dict(op='play', emitter=uid(2), gain=.5)]))
        rpc('desktop.play.resume', {'session_id': sid})
        restarted = host.wait(lambda state: state['audio']['session'] == sid and state['audio']['processed_tick'] > 1,
                              'new runtime with reused logical voice handle')['audio']
        check(restarted['epoch'] >= stopped['epoch'] and restarted['error'] is None, restarted)
        check(world.read_bytes() == original and rpc('world.history') == history, 'Audio altered authored world/history')
        record['checks'].append('Stop/restart isolates session-local voice handles without changing authored bytes or history')

        closing = rpc('desktop.close.begin')
        check(closing['closing'] and closing['playback']['state'] == 'paused', closing)
        rpc('desktop.play.resume', {'session_id': sid}, error=-32009)
        rpc('desktop.audio.configure', config, error=-32009)
        rpc('world.transact', {}, error=-32009)
        rpc('world.inspect')
        closed = host.wait(lambda state: state['audio']['closed'], 'asynchronous worker retirement')
        check(closed['closing'] and not closed['audio']['device_open'] and not closed['audio']['active'], closed)
        again = rpc('desktop.close.begin')
        check(again['audio']['closed'] and again['runtime']['tick'] == closed['runtime']['tick'], 'Repeated close changed simulation')
        record['checks'].append('Asynchronous close retires worker/device before destruction and rejects further mutation while allowing final inspection')
        record['final_state'] = closed
        record['success'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if host is not None:
            try:
                host.rpc('desktop.close.begin')
                deadline = time.monotonic()+15
                while time.monotonic() < deadline and not host.poll()['audio']['closed']:
                    time.sleep(.025)
            except BaseException:
                record['cleanup_error'] = traceback.format_exc()
            host.destroy()
        timings = sorted(item['seconds'] for item in record['polls'])
        if timings:
            record['poll_timing_seconds'] = dict(count=len(timings), median=timings[len(timings)//2],
                                                p95=timings[min(len(timings)-1, math.ceil(len(timings)*.95)-1)], max=max(timings))
        evidence = run / 'evidence.json'
        evidence.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(success=record['success'], checks=len(record['checks']),
                          calls=len(record['calls']), evidence=str(evidence))))
    if not record['success']:
        print(record.get('error'), file=sys.stderr)
    return 0 if record['success'] else 1


if __name__ == '__main__':
    sys.exit(main())
