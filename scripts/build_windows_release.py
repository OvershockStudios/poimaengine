#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a complete local Windows release set from a configured WSL checkout."""

import argparse
import errno
from hashlib import file_digest
import json
from pathlib import Path
import shutil
import subprocess
import sys
from zipfile import ZipFile

from build_desktop import ROOT, digest, source_engine_version
from package_windows_preview import PYTHON_EMBED_SHA256, add_zip_member, embedded_python_files, package_preview
from windows_release_inventory import build_inventory

COLLECTION = ROOT / 'examples/collection-game'
LAUNCHER = ROOT / 'desktop/Poima.Launcher/Poima.Launcher.csproj'
SDK = ROOT / '.cache/toolchains/dotnet-10.0.401/dotnet'
RUNTIME_PACKS = {
    'microsoft.netcore.app.runtime.win-x64': ('LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT'),
    'microsoft.windowsdesktop.app.runtime.win-x64': ('LICENSE',),
}
LAUNCHER_START = b"""# Poima Launcher

Open Poima.Launcher.exe on Windows x64. Choose **Check and install**
to use the Windows engine releases on GitHub.

If you received an engine ZIP and its matching .manifest.json, keep them
together and choose **Install from ZIP**. Select **Open Poima** when
installation finishes.

Run Poima.Launcher.exe --portable to keep launcher data and new projects
in PoimaData beside the executable.

https://github.com/OvershockStudios/poimaengine/blob/main/docs/WINDOWS_LAUNCHER.md
"""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def convert_path(path, direction):
    return subprocess.check_output(['wslpath', direction, str(path)], text=True).strip()


def win(path):
    return convert_path(Path(path).resolve(), '-aw')


def run(command, work, label, *, data=None):
    print(f'Building {label}…', file=sys.stderr, flush=True)
    response = work / f'{label}.log'
    errors = work / f'{label}.stderr.log'
    with response.open('xb') as stdout, errors.open('xb') as stderr:
        result = subprocess.run(list(map(str, command)), cwd=ROOT, input=data, stdout=stdout, stderr=stderr)
    if result.returncode:
        raise RuntimeError(f'{label} exited with {result.returncode}. See {response} and {errors}.')
    return response


def build_sample(work, python_embed, dotnet):
    python_dir = work / 'python'
    python_dir.mkdir()
    for name, data in embedded_python_files(python_embed).items():
        (python_dir / name).write_bytes(data)
    runtime = work / 'runtime'
    run(['cmake', '--install', ROOT/'build/windows-runtime', '--strip', '--prefix', runtime], work, 'runtime')
    cli = runtime / 'bin/poima.exe'
    project = work / 'collection-source'
    project.mkdir()
    request = json.loads((COLLECTION/'world.jsonl').read_bytes())
    recipe = (json.dumps(request, separators=(',', ':'))+'\n').encode('utf-8')
    world = project / 'world.json'
    response = run([cli, 'world', win(world)], work, 'collection-world', data=recipe)
    reply = json.loads(response.read_bytes())
    require(reply.get('id') == request['id'] and 'result' in reply and world.is_file(),
            'The native authoring service rejected the Collection Room recipe.')
    manifest = project / 'project.poimaproject'
    manifest.write_text(json.dumps({
        'format': 'poima.project', 'version': 2, 'name': 'Collection Room',
        'project_id': 'b94f1a11756b46fdabba0658b8eb028c',
        'entry': {'world': world.name, 'camera': f'{101:032x}', 'controller': f'{100:032x}'},
        'audio': False, 'gameplay': {'descriptor': 'gameplay/native-gameplay.json', 'values': {}}
    }, indent=2)+'\n', encoding='utf-8')
    run([python_dir/'python.exe', win(ROOT/'scripts/publish_native_gameplay.py'),
         '--project', win(COLLECTION/'Poima.CollectionGame.csproj'), '--type', 'Poima.Examples.CollectionGame',
         '--dotnet', win(dotnet), '--rid', 'win-x64', '--output', win(project/'gameplay'),
         '--work', win(work/'collection-aot')], work, 'collection-native-aot')
    bundle = work / 'Collection Room'
    run([cli, 'project', 'build', win(manifest), '--runtime', win(runtime), '--output', win(bundle)],
        work, 'collection-export')
    run([cli, 'game', 'inspect', win(bundle/'game.json')], work, 'collection-inspect')


def build_launcher(work, dotnet, version):
    obj = work / 'launcher-build'
    launcher = work / 'launcher'
    run([dotnet, 'publish', win(LAUNCHER), '-c', 'Release', '-p:Version='+version,
         '--artifacts-path', win(obj), '-o', win(launcher), '--disable-build-servers'], work, 'launcher')
    require({p.name for p in launcher.iterdir()} == {'Poima.Launcher.exe'},
            'The launcher publish produced an unexpected payload.')


def launcher_notices(work):
    obj = work / 'launcher-build/obj/Poima.Launcher'
    assets = json.loads((obj/'project.assets.json').read_bytes())
    deps = json.loads((obj/'release_win-x64/Poima.Launcher.deps.json').read_bytes())
    folders = [Path(convert_path(path, '-u')) for path in assets['packageFolders']]
    files = {
        'licenses/Poima-LICENSE.txt': ROOT/'LICENSE',
        'licenses/Playball-OFL.txt': ROOT/'docs/branding/Playball-OFL.txt',
    }
    for name, notices in RUNTIME_PACKS.items():
        selected = [key.split('/')[-1] for key in deps['libraries']
                    if key.lower().startswith('runtimepack.'+name+'/')]
        require(len(selected) == 1, f'Expected one selected launcher runtime pack for {name}.')
        for notice in notices:
            source = next((folder/name/selected[0]/notice for folder in folders
                           if (folder/name/selected[0]/notice).is_file()), None)
            require(source is not None, f'Missing launcher notice {name}/{notice}.')
            files['licenses/'+name+'/'+notice] = source
    return files


def assemble_release(work, destination, python_embed, version):
    engine = destination / f'Poima-{version}-Windows-x64.zip'
    preview = package_preview(work/'desktop', work/'Collection Room', engine, work/'authoring', python_embed)
    inventory = build_inventory(engine)
    require(inventory['archive']['sha256'] == preview['sha256'], 'Engine archive changed during inventory creation.')
    manifest = engine.with_suffix('.manifest.json')
    manifest.write_text(json.dumps(inventory, indent=2)+'\n', encoding='utf-8')
    launcher = destination / f'Poima-Launcher-{version}-Windows-x64.zip'
    members = launcher_notices(work)
    members['Poima.Launcher.exe'] = work/'launcher/Poima.Launcher.exe'
    members['START.md'] = LAUNCHER_START
    with ZipFile(launcher, 'x') as output:
        for name, path in sorted(members.items()):
            add_zip_member(output, name, path)
    with ZipFile(launcher) as output:
        for name, source in members.items():
            if isinstance(source, Path):
                with output.open(name) as member:
                    require(file_digest(member,'sha256').hexdigest() == digest(source), f'Launcher payload changed: {name}')
            else:
                require(output.read(name) == source, f'Launcher guide changed: {name}')
    return {
        'version': version, 'tag': 'v'+version, 'engine_files': len(inventory['files']),
        'sample_manifest_sha256': digest(work/'Collection Room/game.json'),
        'assets': [{'name': path.name, 'bytes': path.stat().st_size, 'sha256': digest(path)}
                   for path in (engine, manifest, launcher)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--windows-dotnet', type=Path, required=True, help='Path to the installed Windows x64 dotnet.exe.')
    parser.add_argument('--python-embed', type=Path, required=True, help='The pinned official embedded Windows Python ZIP.')
    parser.add_argument('--work', type=Path, required=True, help='A new directory under this checkout build directory.')
    parser.add_argument('--output', type=Path, required=True, help='A new directory for the finished release assets.')
    parser.add_argument('--skip-native', action='store_true', help='Use an already-current configured native Windows build.')
    args = parser.parse_args()
    work, destination = (path.absolute() for path in (args.work, args.output))
    require(ROOT/'build' in work.resolve().parents and all(not p.exists() and not p.is_symlink()
            for p in (work, destination)), 'Use new output and work paths, with work below the checkout build directory.')
    require(not work.resolve().is_relative_to(destination.resolve()) and not destination.resolve().is_relative_to(work.resolve()),
            'The build work and release directories must be separate.')
    require(args.windows_dotnet.is_file() and SDK.is_file(), 'The configured Windows and WSL .NET SDKs are required.')
    require(args.python_embed.is_file() and digest(args.python_embed) == PYTHON_EMBED_SHA256,
            'Supply the pinned official embedded Python ZIP.')
    version = source_engine_version()
    work.mkdir(parents=True, exist_ok=False)
    script = ROOT/'scripts'
    run([sys.executable, script/'build_desktop.py', '--output', work/'desktop', '--no-activate',
         *(['--skip-native'] if args.skip_native else [])], work, 'desktop')
    run([sys.executable, script/'package_authoring.py', '--dotnet', SDK, '--coreclr',
         '--output', work/'authoring', '--work', work/'authoring-build'], work, 'authoring')
    build_sample(work, args.python_embed, args.windows_dotnet.resolve())
    build_launcher(work, args.windows_dotnet.resolve(), version)

    staged = work/'release'
    staged.mkdir()
    receipt = assemble_release(work, staged, args.python_embed, version)
    # The receipt is visible only after every asset has been published and checked.
    destination.mkdir(parents=True, exist_ok=False)
    for record in receipt['assets']:
        source = staged/record['name']
        final = destination/source.name
        try:
            final.hardlink_to(source)
        except OSError as error:
            if error.errno != errno.EXDEV:
                raise
            with final.open('xb') as output, source.open('rb') as input_file:
                shutil.copyfileobj(input_file, output, length=1024*1024)
        require(digest(final) == record['sha256'], f'Published asset differed: {final.name}.')
    (destination/'release.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
