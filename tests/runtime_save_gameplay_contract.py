#!/usr/bin/env python3
"""Real managed save-slot ownership, trusted configuration and fresh-process restore."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from verify_native_gameplay import Session as BaseSession


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def uid(value):
    return f'{value:032x}'


def without_session(value):
    if isinstance(value, dict):
        return {key: without_session(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [without_session(item) for item in value]
    return value


class Session(BaseSession):
    def __init__(self, binary, world, record, native, environment):
        self.world, self.record = world, record
        self.stderr = world.with_suffix('.stderr').open('w')
        self.process = subprocess.Popen([str(binary.resolve()), 'world', native(world)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            env=environment, text=True, encoding='utf-8', bufsize=1)
        self.output = queue.Queue()
        def reader():
            for line in self.process.stdout:
                self.output.put(line)
            self.output.put(None)
        threading.Thread(target=reader, daemon=True).start()
        self.session, self.tick, self.revision = None, 0, 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor', 'evidence'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--type', default='Poima.Tests.RuntimeSaveProbe')
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if args.native_descriptor:
        if any((args.hostfxr, args.bridge, args.assembly)):
            parser.error('--native-descriptor cannot be combined with CoreCLR paths.')
    elif not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('Supply --native-descriptor or all of --hostfxr --bridge --assembly.')
    run = ROOT / 'build/runtime-save-gameplay-contract' / uuid.uuid4().hex
    run.mkdir(parents=True)
    world, marker, save_root = run/'world.json', run/'reject-initialize', run/'saves'
    save_root.mkdir()
    def native(path):
        text = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True).strip() if args.windows_interop else text
    environment = os.environ.copy()
    environment['POIMA_SAVE_FIXTURE_INITIALIZE_MARKER'] = native(marker)
    if args.windows_interop:
        # Values are already Windows paths; pass the marker variable verbatim
        # through WSL rather than asking WSLENV to translate it a second time.
        existing = environment.get('WSLENV', '')
        environment['WSLENV'] = ':'.join(filter(None, [existing, 'POIMA_SAVE_FIXTURE_INITIALIZE_MARKER']))
    config = (dict(descriptor=native(args.native_descriptor), expected_descriptor_sha256=sha(args.native_descriptor))
              if args.native_descriptor else dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge),
                                                 assembly=native(args.assembly), type=args.type))
    backend = 'native_aot' if args.native_descriptor else 'coreclr'
    files = dict(binary=args.binary, harness=Path(__file__), helper=ROOT/'scripts/verify_native_gameplay.py',
                 source=ROOT/'tests/fixtures/runtime_save_gameplay/SaveProbe.cs')
    for name in ('hostfxr', 'bridge', 'assembly', 'native_descriptor'):
        if getattr(args, name):
            files[name] = getattr(args, name)
    if args.native_descriptor:
        descriptor = json.loads(args.native_descriptor.read_text())
        files['native_library'] = args.native_descriptor.parent/descriptor['library']
    record = dict(passed=False, backend=backend, hashes={name: sha(path) for name, path in files.items()},
                  checks=[], calls=[], limitations=['Actual managed RPC and filesystem integration; no GUI or GPU.',
                      'Initialize suppression is observed. Trusted constructors/statics may still run when registering modules.'])
    process = None
    try:
        process = Session(args.binary, world, record, native, environment)
        fixture = json.loads((ROOT/'examples/interaction-room.jsonl').read_text())
        process.rpc(fixture['method'], fixture['params'])
        authored = world.read_bytes()
        def configure():
            return process.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(save_root)))
        assert configure()['generation'] == 1
        process.start()
        def load_game(error=None):
            method = 'runtime.gameplay.load_native' if args.native_descriptor else 'runtime.gameplay.load'
            params = dict(session_id=process.session, request_id=uuid.uuid4().hex, expected_tick=process.tick,
                          expected_revision=process.revision, **config)
            params['values'] = dict(Target=uid(2), OptionalTarget=uid(101))
            result = process.rpc(method, params, error)
            if error is None:
                process.revision += 1
            return result
        load_game()
        process.rpc('runtime.step', dict(session_id=process.session, request_id=uuid.uuid4().hex, expected_tick=0,
            ticks=17, motions=[dict(entity=uid(2), position=[3, 1.5, -3], rotation=[0, 0, 0, 1], duration_ticks=120)]))
        process.tick = 17
        values = process.game()['module']['values']
        assert values['Ticks'] == 17 and values['Accumulator'] == '136' and values['Minimum'] == '-9223372036854775808'
        assert values['Fraction'] == 4.5 and abs(values['LastX']-.4) < 1e-5
        process.edit(Precise=321.125)
        def state():
            game = process.game(); module = game['module']
            return dict(tick=game['tick'], revision=game['revision'], module=None if module is None else
                {key: module[key] for key in ('backend', 'assembly_sha256', 'type', 'schema', 'values')},
                entities=[without_session(process.entity(uid(i))) for i in (2, 3, 100, 101)])
        saved = state()
        write_params = dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='managed', expected_generation=0,
            session_id=process.session, expected_tick=17, expected_gameplay_revision=2)
        written = process.rpc('save.write', write_params)
        assert written['generation'] == 1 and not written['replayed']
        def store_tree():
            return {p.relative_to(save_root).as_posix(): sha(p) for p in sorted(save_root.rglob('*')) if p.is_file()}
        stored = store_tree()
        # Host module locations are absent from both checkpoint and manifests.
        forbidden = [value.replace('\\', '/').rsplit('/', 1)[-1].encode() for key, value in config.items()
                     if key in ('hostfxr', 'bridge', 'assembly', 'descriptor')]
        for path in save_root.rglob('*'):
            if path.is_file():
                data = path.read_bytes()
                for basename in forbidden:
                    assert basename not in data, 'Save unexpectedly persisted an executable selection path.'
        marker.write_text('Initialize must fail after the original save.\n')
        record['checks'].append('Actual probe typed state and midmoving-door physics saved; executable paths absent from save storage.')
        process.step(23)
        assert process.game()['module']['values']['Ticks'] == 40
        def load_params(explicit=False):
            params = dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='managed', expected_generation=1,
                revision=1, expected_session_id=process.session, expected_tick=process.tick if process.session else None,
                expected_gameplay_revision=process.revision if process.session else None, new_session_id=uuid.uuid4().hex)
            if explicit:
                params['gameplay'] = config
            return params
        before = state()
        wrong = (dict(config, expected_descriptor_sha256='0'*64) if args.native_descriptor
                 else dict(config, type='Poima.Tests.AlternateSaveProbe'))
        for selection in (None, wrong):
            process.rpc('save.load', dict(load_params(), gameplay=selection), error=-32070)
            assert state() == before and store_tree() == stored and world.read_bytes() == authored
            collected = process.rpc('runtime.gameplay.collect', dict(session_id=process.session))
            assert collected['active_modules'] == 1 and collected['retired_alive'] == 0
        record['checks'].append('Absent/wrong trusted selection rejects staged replacement without changing live module, entities, slot files or authored bytes.')
        params = load_params()  # Omission deliberately inherits the current trusted configuration.
        restored = process.rpc('save.load', params)
        process.session, process.tick, process.revision = params['new_session_id'], restored['tick'], 2
        assert restored['tick'] == 17 and state() == saved
        process.step(23)
        later = state()
        assert later['module']['values']['Ticks'] == 40 and later['module']['values']['Accumulator'] == '780'
        assert later['module']['values']['Precise'] == 324
        assert process.rpc('save.load', params) == dict(restored, replayed=True)
        assert state() == later and store_tree() == stored
        assert process.rpc('save.write', write_params) == dict(written, replayed=True)
        assert state() == later
        record['checks'].append('Inherited trusted configuration restores a fresh runtime ID with Initialize suppressed; continuation and delayed save/load retries preserve clocks and typed state.')
        process.close(); process = None

        process = Session(args.binary, world, record, native, environment)
        assert configure()['generation'] == 1
        assert process.rpc('save.write', write_params) == dict(written, replayed=True)
        assert not process.rpc('runtime.status')['active']
        # Establish that the inherited environment guard is active in the new
        # process, rather than relying only on the successful restore result.
        process.start()
        failure = load_game(error=-32060)
        assert 'Initialize must not execute during snapshot restore' in failure['error']['message'], failure
        assert process.game()['module'] is None
        process.rpc('runtime.stop', dict(session_id=process.session))
        process.session, process.tick, process.revision = None, 0, 0
        failed = load_params()  # No active runtime from which to inherit code.
        process.rpc('save.load', failed, error=-32070)
        assert not process.rpc('runtime.status')['active'] and store_tree() == stored
        explicit = load_params(explicit=True)
        loaded = process.rpc('save.load', explicit)
        process.session, process.tick, process.revision = explicit['new_session_id'], loaded['tick'], 2
        assert state() == saved
        process.step(103)
        final = state()
        assert final['tick'] == 120 and final['module']['values']['Ticks'] == 120
        assert final['module']['values']['Accumulator'] == '7140'
        assert final['module']['values']['Precise'] == 334
        assert abs(final['entities'][0]['world_matrix'][12]-3) < 1e-5
        assert final['entities'][0]['motion_remaining_ticks'] == 0
        assert world.read_bytes() == authored and store_tree() == stored
        record['checks'].append('Fresh process requires explicit trusted code, independently proves Initialize guard, restores full typed state and finishes the original door motion.')
        record['final_state'] = final
        process.close(); process = None
        record['passed'] = True
    except Exception:
        record['error'] = traceback.format_exc()
        raise
    finally:
        if process is not None:
            if process.process.poll() is None:
                process.process.kill()
            process.process.wait(); process.stderr.close()
        evidence = args.evidence or run/'evidence.json'
        evidence.parent.mkdir(parents=True, exist_ok=True)
        evidence.write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(dict(passed=True, checks=len(record['checks']), calls=len(record['calls']), evidence=str(evidence))))


if __name__ == '__main__':
    main()
