#!/usr/bin/env python3
"""Measure real C# edits and collectible reload in the native CoreCLR host."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

from measure_native_module import Session, native_path, summarize

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['launcher', 'bridge', 'kernel', 'hostfxr', 'dotnet', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--warm-edits', type=int, default=40)
    args = parser.parse_args()
    assert 1 <= args.warm_edits <= 100
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / f'run-{time.time_ns()}'
    source = run / 'source'
    shutil.copytree(ROOT / 'experiments/managed_gameplay', source, ignore=shutil.ignore_patterns('bin', 'obj'))
    project = source / 'Game/Poima.Lab.Game.csproj'
    game_source = source / 'Game/Game.cs'
    original = game_source.read_text()
    game_binary = source / 'Game/bin/Release/net10.0/Poima.Lab.Game.dll'
    win = lambda p: native_path(p, args.windows_interop)
    command = [str(args.dotnet.resolve()), 'build', win(project), '-c', 'Release', '--nologo']
    record = {'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'fixture': 'managed_gameplay_v1', 'entities': 1000, 'warm_edits': [], 'builds': [],
              'reloads': [], 'checks': {}, 'launch': [str(args.launcher.resolve()), win(args.hostfxr), win(args.bridge), win(args.kernel)],
              'compiler_version': subprocess.check_output([str(args.dotnet.resolve()), '--version'], text=True).strip()}
    host = Session(args.launcher, record['launch'][1:])
    loaded = False
    artifacts = []

    def inspect():
        return host.call('inspect')['result']['state']

    def collect(expect_alive=0):
        reply = host.call('collect')
        state = reply['result']['state']
        assert state['retired_contexts_alive'] == expect_alive, reply
        assert state['native_live_bytes'] == (1000 * state['entity_bytes'] if state['loaded'] else 0), reply
        return reply

    def compile_variant(name, scale=1, schema=1, bad_abi=False, reject=False, fail_step=False, retain=False, broken=False):
        text = original.replace('private const ulong Scale = 1;', f'private const ulong Scale = {scale};')
        for key, value in [('RejectMigration', reject), ('FailStep', fail_step), ('RetainProbe', retain)]:
            text = text.replace(f'bool {key} = false;', f'bool {key} = {str(value).lower()};')
        if schema == 2:
            text = '#define LAYOUT_V2\n' + text
        if bad_abi:
            text = text.replace('public uint AbiVersion => 1;', 'public uint AbiVersion => 999;')
        if broken:
            text += '\n#error Intentional compilation failure\n'
        game_source.write_text(text)
        started = time.perf_counter()
        build_command = command + (['--no-restore'] if record['builds'] else [])
        compiler = subprocess.Popen(build_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding='utf-8')
        ticks = 0
        deadline = time.monotonic() + 120
        while compiler.poll() is None:
            if loaded:
                host.call('step 1')
                ticks += 1
            if time.monotonic() > deadline:
                compiler.kill()
                raise AssertionError('C# build timed out')
            time.sleep(0.01)
        log = compiler.communicate(timeout=5)[0]
        elapsed = (time.perf_counter() - started) * 1000
        item = {'name': name, 'build_ms': elapsed, 'ticks_during_build': ticks,
                'exit_code': compiler.returncode, 'command': build_command, 'log': log}
        record['builds'].append(item)
        assert (compiler.returncode != 0) == broken, item
        if broken:
            return None, elapsed
        target = run / (name + '.dll')
        shutil.copy2(game_binary, target)
        item['bytes'] = target.stat().st_size
        item['sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
        artifacts.append(target)
        return target, elapsed

    def load(path, success=True):
        before = inspect()
        reply = host.call('load ' + win(path), success)
        after = reply['result']['state']
        assert after['tick'] == before['tick']
        assert after['native_live_bytes'] == 1000 * after['entity_bytes']
        if not success:
            assert (after['state_hash'], after['revision']) == (before['state_hash'], before['revision'])
        return reply

    try:
        initial, _ = compile_variant('initial')
        first = host.call('load ' + win(initial))['result']['state']
        loaded = True
        assert first['entity_bytes'] == 32 and first['first']['position_mm'] == 0
        current = host.call('step 100')['result']['state']
        assert current['first']['position_mm'] == 100 and current['last']['position_mm'] == 3597
        assert current['native_calls'] == 100000
        record['checks']['native_updates'] = True
        saved = run / 'state.bin'
        host.call('save ' + win(saved))
        saved_state = inspect()
        host.call('step 13')
        host.call('restore ' + win(saved))
        assert inspect()['state_hash'] == saved_state['state_hash']
        roundtrip = run / 'roundtrip.bin'
        host.call('save ' + win(roundtrip))
        assert saved.read_bytes() == roundtrip.read_bytes()
        native_baseline = json.loads((ROOT / 'docs/evidence/m0-native-module.json').read_text())
        assert hashlib.sha256(saved.read_bytes()).hexdigest() == native_baseline['checkpoint_sha256']
        record['checks']['canonical_checkpoint_equals_cpp_windows_and_linux'] = True
        damaged = run / 'damaged.bin'
        data = bytearray(saved.read_bytes()); data[48] ^= 1; damaged.write_bytes(data)
        before = inspect()
        host.call('restore ' + win(damaged), False)
        assert inspect()['state_hash'] == before['state_hash']
        record['checks']['corrupt_checkpoint_rejected'] = True

        latest = initial
        for edit in range(args.warm_edits):
            started = time.perf_counter()
            scale = 2 if edit % 2 == 0 else 1
            latest, build_ms = compile_variant(f'edit-{edit}', scale=scale)
            before = inspect()
            reply = load(latest)
            assert reply['result']['state']['state_hash'] == before['state_hash']
            after = host.call('step 1')['result']['state']
            assert after['first']['position_mm'] == before['first']['position_mm'] + scale
            elapsed = (time.perf_counter() - started) * 1000
            collected = collect()
            record['warm_edits'].append({'build_ms': build_ms, 'reload_ms': reply['result']['operation_ms'],
                                         'observed_total_ms': elapsed, 'diagnostic_collection_ms': collected['result']['operation_ms']})
        record['checks']['real_source_edits_preserve_state'] = True
        compile_variant('broken', broken=True)
        host.call('step 1')
        record['checks']['compile_failure_old_game_keeps_running'] = True
        for name, flags in [('bad-abi', {'bad_abi': True}), ('rejected-migration', {'reject': True})]:
            candidate, _ = compile_variant(name, **flags)
            load(candidate, False)
            collect()
            host.call('step 1')
            record['checks'][name + '_retains_state_and_unloads'] = True
        candidate, _ = compile_variant('failing-update', fail_step=True)
        load(candidate)
        before = inspect()
        host.call('step 3', False)
        after = inspect()
        assert (after['state_hash'], after['tick'], after['revision']) == (before['state_hash'], before['tick'], before['revision'])
        record['checks']['failed_update_discards_partial_writes'] = True
        load(latest)
        collect()

        retaining, _ = compile_variant('retained-root', retain=True)
        load(retaining)
        load(latest)
        collect(expect_alive=1)
        host.call('release-probe')
        collect()
        record['checks']['unload_probe_detects_real_retained_reference'] = True
        for cycle in range(100):
            before = inspect()
            reply = load(initial if cycle % 2 == 0 else latest)
            assert reply['result']['state']['state_hash'] == before['state_hash']
            collected = collect()
            state = collected['result']['state']
            assert state['contexts_collected'] == state['contexts_created'] - 1
            record['reloads'].append({'reload_ms': reply['result']['operation_ms'],
                                      'diagnostic_collection_ms': collected['result']['operation_ms'],
                                      'resident_bytes': state['resident_bytes'], 'managed_heap_bytes': state['managed_heap_bytes'],
                                      'native_live_bytes': state['native_live_bytes'], 'retired_alive': state['retired_contexts_alive']})
            host.call('step 1')
        record['checks']['100_reloads_collect_old_assemblies'] = True
        host.call('step 1000')  # Separate warmup from the recorded hot-loop interval.
        record['hot_loop'] = host.call('step 1000')['result']['state']['last_hot_loop']
        assert record['hot_loop']['allocated_bytes'] == 0, record['hot_loop']
        upgraded, _ = compile_variant('schema-2', schema=2)
        before = inspect()
        after = load(upgraded)['result']['state']
        assert after['schema'] == 2 and after['entity_bytes'] == 40
        assert after['first'] == {**before['first'], 'energy': 100}
        assert host.call('step 1')['result']['state']['first']['energy'] == 101
        load(initial, False)
        collect()
        host.call('restore ' + win(saved))
        after = inspect()
        assert after['first'] == {**saved_state['first'], 'energy': 100}
        assert after['state_hash'] == native_baseline['runs']['windows']['final_state']['state_hash']
        record['checks']['layout_upgrade_downgrade_rejection_and_checkpoint_migration'] = True
        record['final_state'] = after
        record['shutdown'] = host.call('quit')['result']['state']
        assert record['shutdown']['native_live_bytes'] == 0 and record['shutdown']['retired_contexts_alive'] == 0
        assert record['shutdown']['contexts_created'] == record['shutdown']['contexts_collected']
        host.process.wait(timeout=10)
        record['summary'] = {key: summarize([row[key] for row in record['warm_edits']])
            for key in ['build_ms', 'reload_ms', 'observed_total_ms', 'diagnostic_collection_ms']}
        record['passed'] = True
    finally:
        try:
            host.close()
        finally:
            source_paths = [p for p in (ROOT / 'experiments/managed_gameplay').rglob('*')
                            if p.is_file() and 'bin' not in p.parts and 'obj' not in p.parts]
            source_paths += [Path(__file__).resolve(), ROOT / 'scripts/measure_native_module.py']
            record['source_sha256'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_paths}
            record['artifacts'] = {str(p.resolve()): {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
                                   for p in [args.launcher, args.bridge, args.kernel, args.hostfxr, *artifacts]}
            (args.output / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'summary': record['summary'], 'hot_loop': record['hot_loop'], 'checks': record['checks']}, indent=2))


if __name__ == '__main__':
    main()
