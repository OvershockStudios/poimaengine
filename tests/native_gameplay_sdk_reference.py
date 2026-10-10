#!/usr/bin/env python3
"""Publish and execute the same native fixture through both SDK reference forms.

Requires a matching x64 .NET/native-linker host and a built compatibility test.
All build products, logs and evidence are retained under a new output directory.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
GAME_TYPE = 'Poima.Tests.ManagedUiGame'
IDENTITY = 'poima.test.managed-ui'
TIMEOUT = 900


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def executable(value):
    selected = shutil.which(value)
    path = Path(selected if selected else value).resolve(strict=True)
    need(path.is_file(), 'Executable must be a regular file: ' + str(path))
    return path


class Runner:
    def __init__(self, args, evidence):
        self.args, self.evidence = args, evidence
        self.env = dict(os.environ)
        for name in ('dotnet-home', 'temp'):
            (args.output / name).mkdir()
        self.env.update(DOTNET_CLI_HOME=str(args.output / 'dotnet-home'),
                        DOTNET_CLI_TELEMETRY_OPTOUT='1',
                        DOTNET_GENERATE_ASPNET_CERTIFICATE='false',
                        DOTNET_NOLOGO='1',
                        TMP=str(args.output / 'temp'), TEMP=str(args.output / 'temp'),
                        TMPDIR=str(args.output / 'temp'))
        # Respect an explicitly supplied shared package cache. Never populate
        # the user's default global cache when no cache was supplied.
        if not self.env.get('NUGET_PACKAGES'):
            (args.output / 'nuget').mkdir()
            self.env['NUGET_PACKAGES'] = str(args.output / 'nuget')
        else:
            self.env['NUGET_PACKAGES'] = str(Path(self.env['NUGET_PACKAGES']).resolve())
        evidence['nuget_cache_inherited'] = bool(os.environ.get('NUGET_PACKAGES'))

    def terminate_tree(self, process, row):
        if os.name == 'nt':
            cleanup = subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                                     capture_output=True, timeout=20)
            row['tree_cleanup'] = dict(method='taskkill /T /F', exit_code=cleanup.returncode,
                                       stdout=cleanup.stdout.decode(errors='replace'),
                                       stderr=cleanup.stderr.decode(errors='replace'))
            if process.poll() is None:
                process.kill()
        else:
            row['tree_cleanup'] = dict(method='isolated process group TERM then KILL')
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            time.sleep(.2)
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=20)

    def run(self, label, command, *, success=True):
        stdout = self.args.output / (label + '.stdout.log')
        stderr = self.args.output / (label + '.stderr.log')
        row = dict(label=label, command=[str(x) for x in command], timeout_seconds=TIMEOUT, expected_success=success,
                   stdout=stdout.name, stderr=stderr.name)
        self.evidence['commands'].append(row)
        started = time.monotonic()
        with stdout.open('xb') as out, stderr.open('xb') as err:
            flags = dict(creationflags=subprocess.CREATE_NEW_PROCESS_GROUP) if os.name == 'nt' else dict(start_new_session=True)
            try:
                process = subprocess.Popen(row['command'], cwd=self.args.output, env=self.env,
                                           stdin=subprocess.DEVNULL, stdout=out, stderr=err, **flags)
            except BaseException:
                row.update(launch_error=traceback.format_exc(), exit_code=None, pid=None,
                           duration_seconds=time.monotonic() - started,
                           stdout_sha256=sha(stdout), stderr_sha256=sha(stderr))
                raise
            row['pid'] = process.pid
            try:
                process.wait(timeout=TIMEOUT)
            except BaseException as error:
                row['interrupted_or_timed_out'] = True
                row['timed_out'] = isinstance(error, subprocess.TimeoutExpired)
                try:
                    self.terminate_tree(process, row)
                except BaseException:
                    row['cleanup_error'] = traceback.format_exc()
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=20)
                raise
            finally:
                row['exit_code'] = process.poll()
                row['duration_seconds'] = time.monotonic() - started
                out.flush(); err.flush()
                row['stdout_sha256'], row['stderr_sha256'] = sha(stdout), sha(stderr)
        need((row['exit_code'] == 0) == success, label + ' returned an unexpected exit code; see retained logs')
        return row

    def artifact(self, path):
        descriptor = path / 'native-gameplay.json'
        data = json.loads(descriptor.read_text(encoding='utf-8'))
        need(data['format'] == 'poima.native-gameplay' and data['version'] == 2,
             'Publisher produced an unexpected artifact format')
        need(data['engine_version'] == self.args.engine_version and data['type'] == GAME_TYPE and
             data['identity'] == IDENTITY and data['schema']['identity'] == IDENTITY,
             'Published fixture identity/version differs')
        need(tuple(data[key] for key in ('call_version', 'call_bytes', 'services_version',
                                        'minimum_services_bytes')) == (1, 80, 7, 176),
             'Preserved fixture no longer uses the 176-byte ABI baseline')
        need('baseline_v7' in data['required_features'] and data['target_arch'] == 'x86_64' and
             data['target_os'] == ('Windows' if os.name == 'nt' else 'Linux'),
             'Published target/baseline differs')
        inventory = {'native-gameplay.json': sha(descriptor)}
        libraries = []
        for member in data['files']:
            relative = PurePosixPath(member['path'])
            need(not relative.is_absolute() and '\\' not in member['path'] and
                 not any(part in ('..', '.') for part in relative.parts), 'Unsafe artifact member')
            file = path.joinpath(*relative.parts)
            need(not file.is_symlink() and file.resolve().is_relative_to(path.resolve()) and
                 file.is_file() and member['path'] not in inventory, 'Invalid/duplicate artifact member')
            need(file.stat().st_size == member['size'] and sha(file) == member['sha256'],
                 'Published member checksum/size differs: ' + member['path'])
            inventory[member['path']] = member['sha256']
            if member['role'] == 'library':
                libraries.append(member['path'])
        actual = [file for file in path.rglob('*') if file.is_file()]
        need(not any(file.is_symlink() for file in path.rglob('*')) and
             set(inventory) == {file.relative_to(path).as_posix() for file in actual},
             'Artifact closure contains missing or unlisted files')
        need(libraries == [data['library']], 'Artifact must contain its one declared native library')
        return dict(descriptor_sha256=sha(descriptor), inventory=inventory, descriptor=data)

    def verify(self):
        args = self.args
        source = ROOT / 'tests/managed_ui_gameplay/ManagedUiGame.cs'
        source_pin = sha(source)
        reference = args.output / 'sdk-reference'
        self.run('sdk-build', [args.dotnet, 'build', ROOT / 'managed/Poima.Gameplay/Poima.Gameplay.csproj',
                              '-c', 'Release', '-o', reference, '--nologo'])
        sdk = reference / 'Poima.Gameplay.dll'
        need(sdk.is_file(), 'SDK build produced no gameplay library')
        sdk_pin = sha(sdk)
        fixture = args.output / 'binary-fixture'
        fixture.mkdir()
        shutil.copy2(source, fixture / source.name)
        project = fixture / 'Poima.ManagedUiGame.csproj'
        project.write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion>
    <Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic>
    <AssemblyName>Poima.ManagedUiGame</AssemblyName>
  </PropertyGroup>
  <ItemGroup><Reference Include="Poima.Gameplay"><HintPath>{escape(str(sdk))}</HintPath></Reference></ItemGroup>
</Project>
''', encoding='utf-8')
        need(not ET.parse(project).getroot().findall('.//ProjectReference'), 'Binary fixture has a ProjectReference')
        self.evidence['sources'] = dict(fixture_sha256=source_pin, sdk_sha256=sdk_pin,
                                       publisher_sha256=sha(ROOT / 'scripts/publish_native_gameplay.py'))
        for label, selected in [('binary-reference', project),
                                ('project-reference', ROOT / 'tests/managed_ui_gameplay/Poima.ManagedUiGame.csproj')]:
            work, artifact = args.output / (label + '-work'), args.output / (label + '-artifact')
            self.run(label + '-publish', [sys.executable, ROOT / 'scripts/publish_native_gameplay.py',
                     '--project', selected, '--type', GAME_TYPE, '--dotnet', args.dotnet,
                     '--engine-version', args.engine_version, '--work', work, '--output', artifact])
            staged_sdk = work / 'game/Poima.Gameplay.dll'
            need(staged_sdk.is_file(), 'Selected build output has no SDK')
            generated = work / 'generated/Poima.NativeGame.csproj'
            hints = [item.findtext('HintPath') for item in ET.parse(generated).getroot().findall('.//Reference')
                     if item.attrib.get('Include') == 'Poima.Gameplay']
            need(len(hints) == 1 and hints[0] is not None and
                 Path(hints[0]).resolve(strict=True) == staged_sdk.resolve(strict=True),
                 'Generated AOT calls do not reference the exact selected SDK build output')
            if label == 'binary-reference':
                need(sha(staged_sdk) == sdk_pin, 'Binary fixture SDK was substituted during staging')
            qualified = self.artifact(artifact)
            qualified.update(staged_sdk_sha256=sha(staged_sdk), generated_project_sha256=sha(generated),
                             selected_sdk_hint=str(staged_sdk))
            self.evidence.setdefault('fixtures', {})[label] = qualified
            run = self.run(label + '-callbacks', [args.compatibility_test, '--native', artifact / 'native-gameplay.json'])
            need('with compiled NativeAOT Tick/Control' in (args.output / run['stdout']).read_text(errors='replace'),
                 'Compatibility harness did not report real native Tick/Control callbacks')
            need(self.artifact(artifact) == {key: qualified[key] for key in ('descriptor_sha256', 'inventory', 'descriptor')},
                 'Compatibility execution mutated the published artifact')
        need(sha(source) == source_pin and sha(fixture / source.name) == source_pin and sha(sdk) == sdk_pin,
             'Fixture source or SDK reference changed during regression')
        negative = args.output / 'noncopying-fixture'
        negative.mkdir()
        shutil.copy2(source, negative / source.name)
        negative_project = negative / project.name
        negative_project.write_text(project.read_text(encoding='utf-8').replace(
            '</HintPath>', '</HintPath><Private>false</Private>'), encoding='utf-8')
        need(sha(negative / source.name) == source_pin, 'Negative fixture source changed')
        work, artifact = args.output / 'noncopying-work', args.output / 'noncopying-artifact'
        rejected = self.run('noncopying-publish', [sys.executable, ROOT / 'scripts/publish_native_gameplay.py',
                     '--project', negative_project, '--type', GAME_TYPE, '--dotnet', args.dotnet,
                     '--engine-version', args.engine_version, '--work', work, '--output', artifact], success=False)
        diagnostics = (args.output / rejected['stderr']).read_text(errors='replace')
        need('Selected project must copy its Poima.Gameplay SDK dependency to the build output:' in diagnostics,
             'Noncopying SDK reference did not produce the actionable missing-copy error')
        need((work / 'game/Poima.ManagedUiGame.dll').is_file() and
             not (work / 'game/Poima.Gameplay.dll').exists() and
             not (work / 'generator').exists() and not artifact.exists(),
             'Missing-copy rejection did not occur after genuine game build and before generation/publication')
        self.evidence['noncopying_reference_rejected'] = True
        self.evidence['passed'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet', default='dotnet')
    parser.add_argument('--compatibility-test', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--engine-version')
    args = parser.parse_args()
    need(platform.machine().lower() in ('x86_64', 'amd64') and sys.platform in ('win32', 'linux'),
         'Use a matching Windows or Linux x64 build host')
    args.dotnet, args.compatibility_test = executable(args.dotnet), executable(args.compatibility_test)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = dict(passed=False, commands=[], runner_sha256=sha(__file__), host_os=sys.platform,
                    compatibility_test_sha256=sha(args.compatibility_test))
    try:
        if args.engine_version is None:
            args.engine_version = re.search(r'project\(poima\s+VERSION\s+([^\s)]+)',
                                          (ROOT / 'CMakeLists.txt').read_text(), re.I).group(1)
        need(re.fullmatch(r'\d+\.\d+\.\d+', args.engine_version), 'Expected numeric engine version')
        evidence['engine_version'] = args.engine_version
        Runner(args, evidence).verify()
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        (args.output / 'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
