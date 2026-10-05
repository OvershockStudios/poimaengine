#!/usr/bin/env python3
"""Publish the Windows desktop into a verified, replaceable build directory."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'poima-desktop-build.json'
NOTICE_PREFIXES = ('license', 'licence', 'notice', 'thirdparty', 'third-party', 'copying')


def update_launcher_pointer(output):
    relative = (output/'Poima.Editor.exe').relative_to(ROOT)
    value = str(relative).replace('/', '\\')
    if (relative.parts[0] != 'build'
            or any(part in ('.', '..') or part.endswith(('.', ' ')) for part in relative.parts)
            or not re.fullmatch(r'[A-Za-z0-9 _./\\-]+', value)):
        print('Desktop published; launcher pointer unchanged because its path is not safe ASCII. '
              'Launch the executable directly or choose an ASCII --output path under build.')
        return False
    pointer = ROOT/'build/desktop-current.txt'
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='ascii', newline='\n',
                                         prefix='.desktop-current-', dir=pointer.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(value+'\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, pointer)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return True


def digest(file):
    value = hashlib.sha256()
    with file.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def inventory(directory):
    files = {}
    for file in sorted(directory.rglob('*')):
        if file.is_symlink():
            raise RuntimeError('Desktop output cannot contain symlinks: '+str(file))
        if file.is_file():
            files[file.relative_to(directory).as_posix()] = digest(file)
        elif not file.is_dir():
            raise RuntimeError('Unexpected desktop output entry: '+str(file))
    return files


def prior_output(directory):
    if not directory.exists():
        return None
    if directory.is_symlink() or not directory.is_dir() or not (directory/MANIFEST).is_file():
        raise RuntimeError('Existing output is not a recorded desktop build; preserve it and choose a new --output: '+str(directory))
    record = json.loads((directory/MANIFEST).read_text())
    if record.get('format') != 'poima.desktop-build' or record.get('version') != 1 or not isinstance(record.get('files'), dict):
        raise RuntimeError('Existing desktop output has an unsupported build manifest.')
    actual = inventory(directory)
    manifest_hash = actual.pop(MANIFEST)
    if actual != record['files']:
        raise RuntimeError('Existing desktop output contains added/changed/missing files; preserve it and choose a new --output.')
    return dict(actual, **{MANIFEST: manifest_hash})


def publish(stage, destination, previous):
    if prior_output(destination) != previous:
        raise RuntimeError('Desktop output changed while building; new publish was not installed.')
    backup = None
    if previous is not None:
        backup = Path(tempfile.mkdtemp(prefix='.desktop-previous-', dir=destination.parent))
        backup.rmdir()
        destination.rename(backup)
    try:
        if destination.exists():
            raise RuntimeError('Desktop destination appeared during publication.')
        stage.rename(destination)
    except BaseException:
        if backup is not None and not destination.exists():
            backup.rename(destination)
        raise
    if backup is not None:
        # Delete only the verified old payload. Unexpected additions are retained
        # in the backup, never recursively removed as if owned build artifacts.
        # Publication has committed. A locked old file must not turn a successful
        # installation into a reported failure or prevent launcher activation.
        try:
            for relative, expected in previous.items():
                old = backup/relative
                if old.is_file() and not old.is_symlink() and digest(old) == expected:
                    old.unlink()
            for folder in sorted(backup.rglob('*'), key=lambda p: len(p.parts), reverse=True):
                if folder.is_dir() and not folder.is_symlink():
                    try:
                        folder.rmdir()
                    except OSError:
                        pass
            backup.rmdir()
        except OSError as error:
            print('Desktop published; retained previous output files in '+str(backup)+': '+str(error))


def package_notice(package, notices, identity, sha512, fallback):
    if not package.is_dir():
        raise RuntimeError('Resolved package is absent: '+str(package))
    specs = list(package.glob('*.nuspec'))
    if len(specs) != 1:
        raise RuntimeError('Expected one package specification: '+identity)
    elements = {element.tag.rsplit('}', 1)[-1]: element for element in ET.parse(specs[0]).getroot().iter()}
    license_element = elements.get('license')
    if license_element is None:
        raise RuntimeError('Package license metadata needs review: '+identity)
    license_description = {'type': license_element.get('type'), 'value': license_element.text}
    copied = []
    for file in package.rglob('*'):
        if file.is_file() and (file.suffix == '.nuspec' or file.name.lower().startswith(NOTICE_PREFIXES)):
            relative = file.relative_to(package)
            target = notices/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, target)
            copied.append(relative.as_posix())
    if license_description['type'] == 'file':
        source = (package/license_description['value']).resolve()
        if package.resolve() not in source.parents or not source.is_file():
            raise RuntimeError('Declared package license file is missing: '+identity)
        relative = source.relative_to(package.resolve())
        target = notices/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        if relative.as_posix() not in copied:
            copied.append(relative.as_posix())
    elif license_description['type'] == 'expression':
        has_text = any(Path(path).name.lower().startswith(('license', 'licence', 'copying')) for path in copied)
        if not has_text:
            source = fallback.get(identity.lower())
            repository = elements.get('repository')
            if source is None or repository is None or repository.get('commit') != source['commit'] or license_description['value'] != source['license']:
                raise RuntimeError('No verified license text for expressed package license: '+identity)
            shutil.copy2(ROOT/'third_party/desktop_notices'/source['file'], notices/'LICENSE.upstream.txt')
            copied.append('LICENSE.upstream.txt')
    else:
        raise RuntimeError('Unsupported package license metadata: '+identity)
    return {'package': identity, 'sha512': sha512, 'license': license_description, 'notice_files': sorted(copied)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-native', action='store_true', help='Use an already built native bridge.')
    parser.add_argument('--no-activate', action='store_true', help='Publish a candidate without changing the editor launcher pointer.')
    parser.add_argument('--output', type=Path, default=ROOT/'build/desktop-win-x64')
    args = parser.parse_args()
    output = args.output.absolute()
    if output.is_symlink():
        parser.error('Publish output cannot be a symlink.')
    output = output.resolve()
    if ROOT/'build' not in output.parents:
        parser.error('Publish output must be a subdirectory of this checkout build directory.')
    previous = prior_output(output)
    sdk = ROOT/'.cache/toolchains/dotnet-10.0.401/dotnet'
    if not sdk.is_file():
        parser.error('Bootstrap the pinned .NET SDK with scripts/bootstrap_tools.py --only dotnet first.')
    environment = dict(os.environ)
    environment.update(DOTNET_CLI_HOME=str(ROOT/'.cache/dotnet-home'), NUGET_PACKAGES=str(ROOT/'.cache/nuget'),
                       DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')

    def run(command):
        subprocess.run(list(map(str, command)), cwd=ROOT, env=environment, check=True)

    if not args.skip_native:
        run(['cmake', '--preset', 'windows-runtime', '-DPOIMA_BUILD_DESKTOP_BRIDGE=ON'])
        run(['cmake', '--build', '--preset', 'windows-runtime', '--target', 'poima_desktop', 'poima'])
    native = ROOT/'build/windows-runtime'
    for name in ('poima_desktop.dll', 'poima.exe'):
        if not (native/name).is_file():
            parser.error('Missing native artifact: '+name)
    project = ROOT/'desktop/Poima.Editor/Poima.Editor.csproj'
    run([sdk, 'restore', project, '-r', 'win-x64', '-p:SelfContained=true', '--locked-mode'])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.desktop-publish-', dir=output.parent) as temporary:
        stage = Path(temporary)/'payload'
        installed = Path(temporary)/'native-install'
        run([sdk, 'publish', project, '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true', '--no-restore', '-o', stage])
        for name in ('poima_desktop.dll', 'poima.exe', 'phonon.dll'):
            if (native/name).is_file():
                shutil.copy2(native/name, stage/name)
        # A fresh install tree also prevents obsolete native notices persisting.
        run(['cmake', '--install', native, '--prefix', installed])
        shutil.copytree(installed/'share/poima', stage/'licenses/engine')
        # Inter is embedded in the desktop resources; Source Sans remains in
        # the optional native ImGui editor shipped alongside the desktop host.
        fonts = stage/'licenses/SourceSans3'
        fonts.mkdir(parents=True)
        shutil.copy2(ROOT/'third_party/source_sans/LICENSE.md', fonts/'LICENSE.md')
        inter = stage/'licenses/Inter'
        inter.mkdir(parents=True)
        for name in ('LICENSE.txt', 'README.md'):
            shutil.copy2(ROOT/'third_party/inter'/name, inter/name)
        upstream = ROOT/'third_party/desktop_notices'
        provenance = json.loads((upstream/'provenance.json').read_text())
        fallback = {}
        for source in provenance['sources']:
            if digest(upstream/source['file']) != source['sha256']:
                raise RuntimeError('Vendored desktop license hash changed: '+source['file'])
            for name, version in source['packages'].items():
                fallback[(name+'/'+version).lower()] = source
        shutil.copytree(upstream, stage/'licenses/upstream')
        assets = json.loads((project.parent/'obj/project.assets.json').read_text())
        libraries = assets['libraries']
        if not libraries:
            raise RuntimeError('Resolved NuGet package inventory is empty.')
        caches = [Path(folder) for folder in assets['packageFolders']]

        def locate(relative):
            for cache in caches:
                candidate = cache/relative
                if candidate.is_dir():
                    return candidate
            raise RuntimeError('Resolved package not found in restore folders: '+relative)

        package_rows = []
        for identity, metadata in sorted(libraries.items()):
            if metadata['type'] != 'package':
                continue
            package_rows.append(package_notice(locate(metadata['path']), stage/'licenses/nuget'/metadata['path'],
                                               identity, metadata.get('sha512'), fallback))
        # Framework/runtime packs are downloadDependencies, not ordinary NuGet
        # libraries. Match the actual published runtime graph to resolved packs.
        deps = json.loads((stage/'Poima.Editor.deps.json').read_text())
        rid = deps['runtimeTarget']['name'].rsplit('/', 1)[-1]
        required = {name.removeprefix('runtimepack.') for name, item in deps['libraries'].items() if item['type'] == 'runtimepack'}
        downloads = {}
        for framework in assets['project']['frameworks'].values():
            for item in framework.get('downloadDependencies', []):
                bounds = item['version'].strip('[]').split(',')
                if len(bounds) != 2 or bounds[0].strip() != bounds[1].strip():
                    raise RuntimeError('Runtime download version is not exact: '+str(item))
                downloads[item['name'].lower()] = (item['name'], bounds[0].strip())
        host = downloads.get(('Microsoft.NETCore.App.Host.'+rid).lower())
        if not required or host is None:
            raise RuntimeError('Published runtime/apphost pack provenance could not be resolved.')
        required.add('/'.join(host))
        runtime_rows = []
        for identity in sorted(required):
            name, version = identity.split('/', 1)
            if downloads.get(name.lower(), (None, None))[1] != version:
                raise RuntimeError('Published runtime pack differs from resolved download: '+identity)
            relative = name.lower()+'/'+version
            package = locate(relative)
            sums = list(package.glob('*.nupkg.sha512'))
            if len(sums) != 1:
                raise RuntimeError('Missing runtime pack checksum: '+identity)
            for notice in ('LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT'):
                if not (package/notice).is_file():
                    raise RuntimeError('Missing runtime pack notice: '+identity+'/'+notice)
            runtime_rows.append(package_notice(package, stage/'licenses/dotnet'/relative, identity,
                                               sums[0].read_text().strip(), fallback))
        manifest = {'format': 'poima.desktop-build', 'version': 1,
                    'native_version': json.loads((installed/'runtime.json').read_text())['engine_version'],
                    'runtime': rid+', self-contained CoreCLR', 'packages': package_rows, 'runtime_packs': runtime_rows,
                    'limitations': ['Prototype; complete source-level license audit and clean-machine distribution qualification remain required.',
                                    'Windows native viewport only; GUI and Scene request Vulkan. Explicit --software-ui affects chrome only.'],
                    'files': inventory(stage)}
        (stage/MANIFEST).write_text(json.dumps(manifest, indent=2)+'\n')
        try:
            publish(stage, output, previous)
        except OSError as error:
            raise RuntimeError('Desktop publication failed; an open editor may lock its build directory. '
                               'Keep the app open and choose a new destination, for example '
                               '--output build/desktop-next. The launcher pointer was not changed.') from error
        if not args.no_activate:
            update_launcher_pointer(output)
    print(output/'Poima.Editor.exe')


if __name__ == '__main__':
    main()
