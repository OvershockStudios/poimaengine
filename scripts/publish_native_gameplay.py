#!/usr/bin/env python3
"""Publish one statically bound C# game as an inventoried NativeAOT artifact.

Builds execute the selected trusted project. Publishing requires a matching-host
.NET SDK and native linker toolchain; this does not cross-compile operating systems.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd, *, capture=False):
    result = subprocess.run([str(p) for p in command], cwd=cwd, text=True,
                            stdout=subprocess.PIPE if capture else None, check=True,
                            env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_GENERATE_ASPNET_CERTIFICATE='false'))
    return result.stdout.strip() if capture else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--type', required=True, dest='game_type')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dotnet', default='dotnet')
    parser.add_argument('--rid', choices=['linux-x64', 'win-x64'])
    parser.add_argument('--engine-version')
    parser.add_argument('--work', type=Path, help='New directory for retained build intermediates (default: temporary directory).')
    args = parser.parse_args()
    project = args.project.resolve(strict=True)
    output = args.output.absolute()
    if output.exists():
        raise ValueError('Output already exists; artifacts are published into a new directory.')
    rid = args.rid or ('win-x64' if os.name == 'nt' else 'linux-x64')
    if (rid == 'win-x64') != (os.name == 'nt') or platform.machine().lower() not in ('x86_64', 'amd64'):
        raise ValueError('NativeAOT publishing requires an x64 host matching the requested operating system.')
    dotnet = str(Path(args.dotnet).resolve()) if Path(args.dotnet).is_file() else args.dotnet
    version = args.engine_version
    if not version:
        version = re.search(r'project\(poima\s+VERSION\s+([^\s)]+)', (ROOT / 'CMakeLists.txt').read_text(), re.I).group(1)
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Engine version must use numeric major.minor.patch.')
    temporary = None
    if args.work:
        work = args.work.absolute()
        work.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='poima-native-game-')
        work = Path(temporary.name)
    # The generated project lives outside repo Directory.Build.props, so every
    # compilation contract is explicit and independent of ambient working paths.
    built = work / 'game'
    run([dotnet, 'build', project, '-c', 'Release', '-o', built, '--nologo'], ROOT)
    name = run([dotnet, 'msbuild', project, '-getProperty:AssemblyName', '-nologo'], ROOT, capture=True)
    assembly = built / (name + '.dll')
    if not assembly.is_file():
        raise ValueError('Selected project produced no game assembly: ' + str(assembly))
    generator = ROOT / 'managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj'
    run([dotnet, 'build', generator, '-c', 'Release', '-o', work / 'generator', '--nologo'], ROOT)
    generated = work / 'generated'
    run([dotnet, work / 'generator/Poima.NativeGame.Generator.dll', assembly, args.game_type, generated], ROOT)
    schema = json.loads((generated / 'schema.json').read_text())
    # Referencing the original project preserves source and dependency semantics;
    # the linker sees direct calls to the exact game and state types.
    project_xml = f'''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion>
    <Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic>
    <AssemblyName>Poima.NativeGame</AssemblyName><PublishAot>true</PublishAot><NativeLib>Shared</NativeLib>
    <StripSymbols>true</StripSymbols><IlcTreatWarningsAsErrors>true</IlcTreatWarningsAsErrors>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="{escape(str(project), {chr(34): "&quot;"})}" />
    <Compile Include="{escape(str(ROOT / 'managed/Poima.NativeGame/Entry.cs'), {chr(34): '&quot;'})}" Link="Entry.cs" />
  </ItemGroup>
</Project>
'''
    generated_project = generated / 'Poima.NativeGame.csproj'
    generated_project.write_text(project_xml, encoding='utf-8')
    published = work / 'publish'
    run([dotnet, 'publish', generated_project, '-c', 'Release', '-r', rid, '-o', published, '--nologo'], ROOT)
    library_name = 'Poima.NativeGame.dll' if rid == 'win-x64' else 'Poima.NativeGame.so'
    library = published / library_name
    if not library.is_file():
        raise ValueError('NativeAOT shared library was not produced.')
    # Never silently drop extra runtime dependencies emitted by a project.
    # Symbols are development-only; all other payloads need an explicit policy.
    allowed = {library_name, library_name + '.dbg', 'Poima.NativeGame.pdb'}
    unexpected = [p.name for p in published.iterdir() if p.name not in allowed and p.suffix.lower() not in ('.pdb', '.dbg')]
    if unexpected:
        raise ValueError('Additional publish payloads require explicit dependency packaging: ' + ', '.join(unexpected))
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.' + output.name + '-', dir=output.parent))
    try:
        shutil.copy2(library, stage / library_name)
        if schema.get("components"):
            shutil.copy2(generated / "game.poima-components.json", stage / "game.poima-components.json")
        notices = stage / 'notices'
        notices.mkdir()
        shutil.copy2(ROOT / 'LICENSE', notices / 'POIMA-LICENSE.txt')
        shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', notices / 'POIMA-THIRD-PARTY-NOTICES.md')
        # NativeAOT incorporates .NET runtime code; retain official SDK runtime
        # license and third-party notices, not only the engine's notices.
        assets = json.loads((generated / 'obj/project.assets.json').read_text())
        package_roots = [Path(path) for path in assets['packageFolders']]
        downloads = assets['project']['frameworks']['net10.0']['downloadDependencies']
        for package, label in [('Microsoft.NETCore.App.Runtime.' + rid, 'DOTNET-RUNTIME'),
                               ('Microsoft.NETCore.App.Runtime.NativeAOT.' + rid, 'DOTNET-NATIVEAOT')]:
            resolved = next((p for p in downloads if p['name'].lower() == package.lower()), None)
            if resolved is None:
                raise ValueError('Resolved publish assets omit runtime package ' + package)
            bounds = resolved['version'].strip('[]').split(',')
            if len(bounds) != 2 or bounds[0].strip() != bounds[1].strip():
                raise ValueError('Runtime package version is not exactly resolved: ' + str(resolved))
            package_version = bounds[0].strip()
            package_path = next((root / package.lower() / package_version for root in package_roots
                                 if (root / package.lower() / package_version).is_dir()), None)
            if package_path is None:
                raise ValueError('Resolved .NET runtime package is missing: ' + package + '/' + package_version)
            for original, suffix in [('LICENSE.TXT', 'LICENSE.txt'), ('THIRD-PARTY-NOTICES.TXT', 'THIRD-PARTY-NOTICES.txt')]:
                source = package_path / original
                if not source.is_file():
                    raise ValueError('Resolved .NET runtime notice is missing: ' + str(source))
                shutil.copy2(source, notices / (label + '-' + package_version + '-' + suffix))
        files = []
        for path in sorted(p for p in stage.rglob('*') if p.is_file()):
            payload = path.read_bytes()
            files.append(dict(path=path.relative_to(stage).as_posix(), size=len(payload), sha256=hashlib.sha256(payload).hexdigest(), role='library' if path.name == library_name else 'metadata' if path.name == 'game.poima-components.json' else 'notice'))
        descriptor = dict(format='poima.native-gameplay', version=1, engine_version=version,
                          target_os='Windows' if rid == 'win-x64' else 'Linux', target_arch='x86_64',
                          call_version=1, services_version=7, entry='poima_gameplay_entry',
                          library=library_name, identity=schema['identity'], type=args.game_type,
                          schema=schema, files=files)
        (stage / 'native-gameplay.json').write_text(json.dumps(descriptor, indent=2) + '\n', encoding='utf-8')
        # Reserve a new destination atomically. POSIX rename would replace an
        # empty directory racing the final check; mkdir never does. The marker
        # and manifest-last policy make incomplete publication uninspectable.
        output.mkdir(exist_ok=False)
        marker = output / '.poima-publishing'
        with marker.open('x', encoding='utf-8') as handle:
            handle.write('Incomplete NativeAOT artifact publication.\n')
        for directory in sorted(path for path in stage.rglob('*') if path.is_dir()):
            (output / directory.relative_to(stage)).mkdir(exist_ok=False)
        payloads = sorted(path for path in stage.rglob('*') if path.is_file() and path.name != 'native-gameplay.json')
        payloads.append(stage / 'native-gameplay.json')
        for source in payloads:
            with source.open('rb') as incoming, (output / source.relative_to(stage)).open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
        # Removing this marker is the only step that exposes a valid artifact.
        marker.unlink()
    finally:
        # Cleanup must neither replace an earlier failure nor report a failed
        # publication after the completed artifact is already visible.
        for cleanup in (lambda: shutil.rmtree(stage) if stage.exists() else None,
                        lambda: temporary.cleanup() if temporary else None):
            try:
                cleanup()
            except OSError as error:
                print('Warning: native gameplay build cleanup failed: ' + str(error), file=sys.stderr)
    print(json.dumps(dict(artifact=str(output / 'native-gameplay.json'), target=rid, schema=schema)))


if __name__ == '__main__':
    main()
