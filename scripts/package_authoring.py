#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build one bounded NativeAOT authoring pack into new, independent directories.

Uses the caller's installed .NET SDK and existing framework/cache. Package
sources are cleared for these builds. No SDK install or global settings change
is performed. Failed work/output is retained for inspection.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
from authoring_pack import FILE_LIMIT, TOTAL_LIMIT, PAYLOAD_ROLES, load_pack

ROOT = Path(__file__).resolve().parents[1]
PROPS = '''<Project>
  <!-- Import this file once from an original game project. -->
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion>
    <Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="Poima.Gameplay"><HintPath>$(MSBuildThisFileDirectory)../sdk/Poima.Gameplay.dll</HintPath><Private>true</Private></Reference>
  </ItemGroup>
  <Import Project="$(MSBuildThisFileDirectory)Poima.Components.targets" />
</Project>
'''
TARGETS = '''<Project>
  <ItemGroup>
    <Analyzer Include="$(MSBuildThisFileDirectory)../analyzers/Poima.Components.Generator.dll" />
  </ItemGroup>
</Project>
'''


def _need(value, message):
    if not value:
        raise ValueError(message)


def _regular(path):
    value = path.lstat()
    _need(stat.S_ISREG(value.st_mode) and not getattr(value, 'st_file_attributes', 0) & 0x400 and
          value.st_nlink == 1 and 1 <= value.st_size <= FILE_LIMIT,
          'Pack source must be a bounded independent regular file: ' + path.name)


def _new_directory(value):
    path = Path(value).absolute()
    _need(not path.exists() and not path.is_symlink(), 'Output/work directory must be new.')
    # Existing ancestors may not redirect writes through links/reparse points.
    for ancestor in (path.parent, *path.parent.parents):
        if ancestor.exists() or ancestor.is_symlink():
            info = ancestor.lstat()
            _need(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode) and
                  not getattr(info, 'st_file_attributes', 0) & 0x400, 'Linked output/work ancestor.')
    return path.resolve(strict=False)


def _contains(first, second):
    return first == second or first in second.parents


def _digest(path):
    digest = hashlib.sha256()
    count = 0
    with path.open('rb') as stream:
        while True:
            block = stream.read(min(1024 * 1024, FILE_LIMIT + 1 - count))
            if not block:
                break
            count += len(block)
            _need(count <= FILE_LIMIT, 'Pack resource exceeds its byte bound.')
            digest.update(block)
    return digest.hexdigest()


def _source_identity(path):
    _regular(path)
    before = path.stat()
    identity = (before.st_size, _digest(path))
    _regular(path)
    after = path.stat()
    _need((before.st_size, before.st_mtime_ns, before.st_ino) ==
          (after.st_size, after.st_mtime_ns, after.st_ino), 'Pack source changed during admission.')
    return identity


def _copy(source, destination, expected):
    _regular(source)
    before = source.stat()
    destination.parent.mkdir(parents=True, exist_ok=True)
    count, copied = 0, hashlib.sha256()
    with source.open('rb') as incoming, destination.open('xb') as outgoing:
        while True:
            block = incoming.read(min(1024 * 1024, FILE_LIMIT + 1 - count))
            if not block:
                break
            count += len(block)
            _need(count <= FILE_LIMIT, 'Pack source grew beyond its byte bound.')
            copied.update(block)
            outgoing.write(block)
    _regular(source)
    _regular(destination)
    after = source.stat()
    _need((before.st_size, before.st_mtime_ns, before.st_ino) ==
          (after.st_size, after.st_mtime_ns, after.st_ino) and count == before.st_size,
          'Pack source changed while copying.')
    _need((count, copied.hexdigest()) == expected and _digest(destination) == expected[1],
          'Pack copy differs from the admitted source.')


def _build(dotnet, project, work, label):
    artifacts = work / (label + '-artifacts')
    output = work / (label + '-output')
    command = [str(dotnet), 'build', str(project), '-c', 'Release', '-o', str(output),
               '--artifacts-path', str(artifacts), '--nologo', '--disable-build-servers',
               '-nodeReuse:false', '-p:UseSharedCompilation=false', '-p:UseAppHost=false',
               '-p:RestoreConfigFile=' + str(work / 'NuGet.Config')]
    # A separate artifacts root isolates each project's bin/obj, including all
    # ProjectReferences, without a shared BaseIntermediateOutputPath collision.
    subprocess.run(command, cwd=work, check=True, env=dict(os.environ,
        DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false'))
    return output


def _outputs(directory, required, optional):
    _need(directory.is_dir() and not directory.is_symlink(), 'Build output is missing.')
    present = set()
    for path in directory.iterdir():
        _regular(path)
        _need(path.name in required | optional, 'Additional build dependency needs an explicit packaging policy: ' + path.name)
        present.add(path.name)
    _need(required <= present, 'Build output omits required authoring resources.')


def package_authoring(dotnet, output, work, engine_version=None):
    """Compile SDK/analyzer/generator, copy fixed resources and publish manifest last."""
    output, work = _new_directory(output), _new_directory(work)
    _need(not _contains(output, work) and not _contains(work, output), 'Output and work must not overlap.')
    version = engine_version
    if version is None:
        match = re.search(r'project\(Poima\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)(?:\s|\))',
                          (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'), re.I)
        _need(match is not None, 'Cannot determine current engine version.')
        version = match.group(1)
    _need(type(version) is str and len(version) <= 128 and re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version),
          'Engine version must use numeric major.minor.patch.')
    executable = Path(dotnet).resolve(strict=True)
    _need(executable.is_file(), 'Explicit installed dotnet executable required.')
    work.mkdir(parents=True, exist_ok=False)
    (work / 'NuGet.Config').write_bytes(b'<configuration><packageSources><clear /></packageSources></configuration>\n')
    generator = _build(executable, ROOT / 'managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj', work, 'generator')
    analyzer = _build(executable, ROOT / 'managed/Poima.Components.Generator/Poima.Components.Generator.csproj', work, 'analyzer')
    generator_files = {'Poima.NativeGame.Generator.dll', 'Poima.NativeGame.Generator.deps.json',
                       'Poima.NativeGame.Generator.runtimeconfig.json', 'Poima.Gameplay.dll'}
    _outputs(generator, generator_files, {'Poima.NativeGame.Generator.pdb', 'Poima.Gameplay.pdb', 'Poima.Gameplay.deps.json'})
    _outputs(analyzer, {'Poima.Components.Generator.dll'}, {'Poima.Components.Generator.pdb', 'Poima.Components.Generator.deps.json'})
    resources = {
        'sdk/Poima.Gameplay.dll': generator / 'Poima.Gameplay.dll',
        'analyzers/Poima.Components.Generator.dll': analyzer / 'Poima.Components.Generator.dll',
        **{'tools/native-generator/' + name: generator / name for name in generator_files},
        'native/Entry.cs': ROOT / 'managed/Poima.NativeGame/Entry.cs',
        'tools/publish_native_gameplay.py': ROOT / 'scripts/publish_native_gameplay.py',
        'tools/authoring_pack.py': ROOT / 'scripts/authoring_pack.py',
        'notices/POIMA-LICENSE.txt': ROOT / 'LICENSE',
        'notices/POIMA-THIRD-PARTY-NOTICES.md': ROOT / 'THIRD_PARTY_NOTICES.md',
    }
    # Validate source closure before reserving output; any later failure leaves
    # an incomplete directory with no valid authoring.json, never a valid pack.
    identities = {relative: _source_identity(source) for relative, source in resources.items()}
    total = sum(identity[0] for identity in identities.values()) + len(PROPS.encode()) + len(TARGETS.encode())
    _need(total <= TOTAL_LIMIT, 'Authoring payload total exceeds 64 MiB.')
    output.mkdir(parents=True, exist_ok=False)
    for relative, source in resources.items():
        _copy(source, output / relative, identities[relative])
    (output / 'build').mkdir(exist_ok=True)
    (output / 'build/Poima.Gameplay.props').write_bytes(PROPS.encode('utf-8'))
    (output / 'build/Poima.Components.targets').write_bytes(TARGETS.encode('utf-8'))
    entries = []
    for relative, role in sorted(PAYLOAD_ROLES.items()):
        payload = output / relative
        _regular(payload)
        digest = _digest(payload)
        entries.append(dict(path=relative, size=payload.stat().st_size, sha256=digest, role=role))
    manifest = dict(format='poima.authoring-tools', version=1, engine_version=version,
                    framework='net10.0', language='14.0', files=entries)
    data = (json.dumps(manifest, indent=2, allow_nan=False) + '\n').encode('utf-8')
    _need(len(data) <= 64 * 1024, 'Authoring manifest exceeds 64 KiB.')
    with (output / 'authoring.json').open('xb') as stream:
        stream.write(data)
    pack = load_pack(output)
    pack.verify()
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--engine-version')
    args = parser.parse_args()
    pack = package_authoring(args.dotnet, args.output, args.work, args.engine_version)
    print(json.dumps(dict(authoring_pack=str(pack.root), engine_version=pack.engine_version,
                          manifest_sha256=pack.manifest_sha256, files=13)))


if __name__ == '__main__':
    main()
