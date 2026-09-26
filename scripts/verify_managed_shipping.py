#!/usr/bin/env python3
"""Build and compare the bounded CoreCLR/Native AOT shipping fixture on Linux x64."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import time

from measure_native_module import summarize

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def expected(schema, tick, energy=0):
    data = bytearray(b'PMLAB001' + struct.pack('<QQQ', schema, 1000, tick))
    for index in range(1000):
        velocity = index % 7 + 1
        data += struct.pack('<QQQQQ', index, index * 3 + velocity * tick, velocity, tick, energy)
    return bytes(data + struct.pack('<Q', checksum(data)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet', type=Path, default=ROOT / '.cache/toolchains/dotnet-10.0.401/dotnet')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/managed-shipping-evidence')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('This qualification runner currently targets Linux x64 only.')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / f'run-{time.time_ns()}'
    run.mkdir()
    env = {**os.environ, 'DOTNET_CLI_TELEMETRY_OPTOUT': '1', 'DOTNET_NOLOGO': '1',
           'DOTNET_PROCESSOR_COUNT': '2', 'DOTNET_CLI_USE_MSBUILD_SERVER': '0',
           'DOTNET_CLI_HOME': str(ROOT / '.cache/dotnet-user'),
           'NUGET_PACKAGES': str(ROOT / '.cache/nuget/packages')}
    dotnet = args.dotnet.resolve()
    record = {'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'fixture': 'managed_shipping_v1', 'platform': platform.platform(),
              'builds': [], 'runs': [], 'checks': {}, 'passed': False}
    artifacts = []

    def command(label, argv):
        print(label, flush=True)
        started = time.perf_counter()
        result = subprocess.run(argv, cwd=run, env=env, capture_output=True, text=True, timeout=600)
        item = {'name': label, 'command': [str(x) for x in argv],
                'elapsed_ms': (time.perf_counter() - started) * 1000,
                'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
        record['builds'].append(item)
        if result.returncode:
            raise AssertionError(item)
        return result.stdout

    try:
        record['sdk'] = command('Inspect SDK', [dotnet, '--info'])
        record['clang'] = command('Inspect native toolchain', ['clang', '--version'])
        headers = dotnet.parent / 'packs/Microsoft.NETCore.App.Host.linux-x64/10.0.12/runtimes/linux-x64/native'
        command('Configure native kernel', ['cmake', '-S', ROOT / 'experiments/managed_gameplay',
                '-B', run / 'native', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                f'-DPOIMA_DOTNET_HOST_HEADERS={headers}'])
        command('Build native kernel', ['cmake', '--build', run / 'native', '--parallel', '2'])
        kernel = run / 'native/libpoima_managed_kernel.so'
        artifacts.append(kernel)
        native = json.loads((ROOT / 'docs/evidence/m0-native-module.json').read_text())
        assert hashlib.sha256(expected(1, 100)).hexdigest() == native['checkpoint_sha256']
        record['checks']['independent_checkpoint_matches_cpp_and_recorded_coreclr'] = True
        executables = {}
        for schema in [1, 2]:
            source = run / f'schema-{schema}/source'
            shutil.copytree(ROOT / 'experiments/managed_gameplay', source,
                            ignore=shutil.ignore_patterns('bin', 'obj'))
            (source / 'NuGet.Config').write_text('<configuration><packageSources><clear/>'
                '<add key="nuget.org" value="https://api.nuget.org/v3/index.json" />'
                '</packageSources></configuration>\n')
            if schema == 2:
                game = source / 'Game/Game.cs'
                game.write_text('#define LAYOUT_V2\n' + game.read_text())
            project = source / 'Shipping/Poima.ShippingLab.csproj'
            for mode in ['jit', 'aot']:
                publish = run / f'schema-{schema}/{mode}'
                argv = [dotnet, 'publish', project, '-c', 'Release', '-r', 'linux-x64',
                        '-o', publish, '--nologo', '-p:RuntimeFrameworkVersion=10.0.12',
                        '-p:UseSharedCompilation=false', '-m:2',
                        f'-p:PublishAot={str(mode == "aot").lower()}',
                        '--self-contained', str(mode == 'aot').lower()]
                command(f'Publish schema {schema} {mode}', argv)
                binary = publish / ('Poima.ShippingLab' if mode == 'aot' else 'Poima.ShippingLab.dll')
                executables[schema, mode] = [binary] if mode == 'aot' else [dotnet, binary]
                artifacts.append(binary)
                if mode == 'aot':
                    shutil.copy2(kernel, publish / kernel.name)
                    notices = publish / 'notices'
                    notices.mkdir(exist_ok=True)
                    for name in ['LICENSE.txt', 'ThirdPartyNotices.txt']:
                        shutil.copy2(dotnet.parent / name, notices / ('dotnet-' + name))
                    shutil.copy2(ROOT / 'LICENSE', notices / 'Poima-LICENSE')
                    record[f'schema_{schema}_package_files'] = [str(p.relative_to(publish))
                        for p in sorted(publish.rglob('*')) if p.is_file()]
                    assert not list(publish.glob('*.dll')), 'AOT package unexpectedly contains managed DLLs'
                    assert binary.read_bytes()[:4] == b'\x7fELF'
                    record[f'schema_{schema}_native_dependencies'] = command('Inspect ELF dependencies', ['ldd', binary])
                    assert 'libcoreclr' not in record[f'schema_{schema}_native_dependencies']

        def invoke(schema, mode, source, output, ticks, succeeds=True):
            service = executables[schema, mode][0].parent / kernel.name if mode == 'aot' else kernel
            argv = [*executables[schema, mode], service, source, output, str(ticks)]
            started = time.perf_counter()
            # AOT must execute directly even when DOTNET_ROOT points to nothing.
            runtime_env = {**env, 'DOTNET_ROOT': str(run / 'absent-dotnet')}
            result = subprocess.run(argv, cwd=run, env=runtime_env, capture_output=True, text=True, timeout=30)
            reply = json.loads(result.stdout)
            assert result.returncode == (0 if succeeds else 4), (result, reply)
            assert not result.stderr, result.stderr
            assert reply['status'] == ('ok' if succeeds else 'error'), reply
            record['runs'].append({'schema': schema, 'mode': mode, 'input': str(source), 'ticks': ticks,
                                   'process_ms': (time.perf_counter() - started) * 1000, 'reply': reply})
            if succeeds:
                assert reply['dynamic_code_supported'] == (mode == 'jit'), reply
                assert reply['dynamic_code_compiled'] == (mode == 'jit'), reply
                assert reply['native_calls'] == ticks * 1000 and reply['native_live_bytes'] == 0, reply
            return reply

        for schema in [1, 2]:
            for mode in ['jit', 'aot']:
                output = run / f'{schema}-{mode}-100.bin'
                invoke(schema, mode, '-', output, 100)
                assert output.read_bytes() == expected(schema, 100, 200 if schema == 2 else 0)
                roundtrip = run / f'{schema}-{mode}-roundtrip.bin'
                invoke(schema, mode, output, roundtrip, 0)
                assert output.read_bytes() == roundtrip.read_bytes()
        record['checks']['jit_aot_initialization_and_all_1000_entities_match_oracle'] = True
        record['checks']['both_schemas_roundtrip'] = True

        for mode, opposite in [('jit', 'aot'), ('aot', 'jit')]:
            output = run / f'{mode}-resume.bin'
            invoke(1, mode, run / f'1-{opposite}-100.bin', output, 37)
            assert output.read_bytes() == expected(1, 137)
            upgraded = run / f'{mode}-upgraded.bin'
            invoke(2, mode, output, upgraded, 7)
            assert upgraded.read_bytes() == expected(2, 144, 107)
            sentinel = run / f'{mode}-retained.bin'
            sentinel.write_bytes(b'keep existing output')
            invoke(1, mode, upgraded, sentinel, 0, False)
            assert sentinel.read_bytes() == b'keep existing output'
        record['checks']['cross_runtime_resume_and_schema_upgrade'] = True
        record['checks']['downgrade_rejected_without_overwriting_output'] = True

        bad = bytearray(expected(1, 100)); bad[48] ^= 1
        malformed = {'checksum': bytes(bad), 'length': b'PMLAB001'}
        for label, offset, value in [('schema', 8, 999), ('identity', 32, 99), ('overflow', 24, (1 << 64) - 1)]:
            bad = bytearray(expected(1, 100))
            struct.pack_into('<Q', bad, offset, value)
            struct.pack_into('<Q', bad, len(bad) - 8, checksum(bad[:-8]))
            malformed[label] = bytes(bad)
        for label, data in malformed.items():
            source = run / f'bad-{label}.bin'; source.write_bytes(data)
            for mode in ['jit', 'aot']:
                output = run / 'error-sentinel.bin'; output.write_bytes(b'unchanged')
                invoke(1, mode, source, output, 1, False)
                assert output.read_bytes() == b'unchanged'
        record['checks']['malformed_checkpoints_rejected_without_overwriting_output'] = True
        for mode in ['jit', 'aot']:
            output = run / 'bad-ticks.bin'; output.write_bytes(b'unchanged')
            invoke(1, mode, '-', output, 10001, False)
            assert output.read_bytes() == b'unchanged'
            invoke(1, mode, '-', run / 'missing-directory/state.bin', 1, False)
        record['checks']['invalid_ticks_and_output_errors_reported'] = True

        # Small-fixture launch/update/save costs, not a game performance benchmark.
        timings = {}
        for mode in ['jit', 'aot']:
            values = []
            for index in range(20):
                output = run / f'timed-{mode}.bin'
                invoke(1, mode, '-', output, 1000)
                assert output.read_bytes() == expected(1, 1000)
                values.append(record['runs'][-1]['process_ms'])
            timings[mode] = summarize(values)
        record['process_timings'] = timings
        record['checks']['20_process_restarts_each_runtime_preserve_expected_behavior'] = True
        artifacts += list(run.glob('schema-*/source/Shipping/obj/project.assets.json'))
        record['passed'] = True
    finally:
        sources = [p for p in (ROOT / 'experiments/managed_gameplay').rglob('*')
                   if p.is_file() and not {'bin', 'obj'}.intersection(p.parts)]
        sources += [Path(__file__).resolve(), ROOT / 'scripts/bootstrap_tools.py']
        record['source_sha256'] = {str(p.relative_to(ROOT)): sha(p) for p in sorted(sources)}
        record['artifacts'] = {str(p): {'sha256': sha(p), 'bytes': p.stat().st_size} for p in artifacts if p.exists()}
        (args.output / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'checks': record['checks'],
                      'process_timings': record['process_timings']}, indent=2))


if __name__ == '__main__':
    main()
