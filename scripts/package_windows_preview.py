#!/usr/bin/env python3
"""Package a self-contained Poima desktop build and its verified Windows sample."""
# SPDX-License-Identifier: Apache-2.0

import argparse
from hashlib import sha256
import json
import shutil
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from authoring_pack import load_pack
from build_desktop import MANIFEST, digest, inventory, prior_output, source_engine_version

PYTHON_EMBED_SHA256 = '9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38'
CREATOR_REFERENCE = (
    'CREATOR_GAMEPLAY.md', 'MANAGED_GAMEPLAY.md', 'GAME_UI.md', 'GAME_VERIFICATION.md',
)


def embedded_python_files(path):
    if digest(path) != PYTHON_EMBED_SHA256:
        raise ValueError('Embedded Python differs from the pinned official x64 archive.')
    files = {}
    with ZipFile(path) as archive:
        for member in archive.infolist():
            name = member.filename
            if Path(name).name != name or '\\' in name or ':' in name or name in files or member.is_dir():
                raise ValueError('Unexpected embedded Python archive entry.')
            files[name] = archive.read(member)
    if not {'python.exe', 'LICENSE.txt'} <= files.keys():
        raise ValueError('Embedded Python executable and license are required.')
    return files


def add_zip_member(archive, name, source):
    # Stable member metadata keeps the archive independent of source timestamps.
    info = ZipInfo(name)
    info.create_system = 0
    info.external_attr = 0x20
    info.compress_type = ZIP_DEFLATED
    if isinstance(source, Path):
        info.file_size = source.stat().st_size
        with source.open('rb') as stream, archive.open(info, 'w') as member:
            shutil.copyfileobj(stream, member, length=1024*1024)
    else:
        archive.writestr(info, source)


def package_preview(desktop, sample, output, authoring=None, python_embed=None):
    desktop, sample, output = desktop.resolve(), sample.resolve(), output.absolute()
    version = source_engine_version()
    files = prior_output(desktop)
    if not files or 'Poima.Editor.exe' not in files or 'poima.exe' not in files:
        raise ValueError('Supply a complete Poima desktop build.')
    desktop_manifest = json.loads((desktop/MANIFEST).read_text(encoding='utf-8'))
    if desktop_manifest.get('native_version') != version:
        raise ValueError('The desktop build must match the selected engine version.')
    if any(name.split('/')[0] in ('samples', 'START.md', 'authoring', 'publisher-python', 'reference') for name in files):
        raise ValueError('The desktop input already includes preview content.')
    manifest = json.loads((sample/'game.json').read_text(encoding='utf-8'))
    if (manifest.get('format') != 'poima.game' or manifest.get('name') != 'Collection Room' or
            manifest.get('engine') != dict(version=version, target_os='Windows', target_arch='x86_64',
                                          executable='runtime/bin/poima.exe')):
        raise ValueError('The sample must be a Windows x64 Collection Room export from this engine version.')
    sample_files = inventory(sample)
    if 'runtime/bin/poima.exe' not in sample_files or 'content/world.json' not in sample_files:
        raise ValueError('The sample must include its runtime and world.')
    if output.suffix.lower() != '.zip' or output.is_symlink() or output.exists():
        raise ValueError('Choose a new .zip file.')
    if bool(authoring) != bool(python_embed):
        raise ValueError('Supply the authoring pack and embedded Python together.')
    roots = [desktop, sample]
    python_files = {}
    if authoring:
        authoring = authoring.resolve(strict=True)
        pack = load_pack(authoring)
        if pack.engine_version != version:
            raise ValueError('The authoring pack must match the engine version.')
        roots.append(authoring)
        python_files = embedded_python_files(python_embed.resolve(strict=True))
    if any(root in output.resolve().parents for root in roots):
        raise ValueError('The archive must be outside the input trees.')
    prefix = f'Poima-{version}-Windows-x64'
    docs = Path(__file__).resolve().parents[1]/'docs'
    readme_files = {f'{prefix}/START.md': (docs/'WINDOWS_PREVIEW.md').read_bytes()}
    desktop_files = {name: checksum for name, checksum in files.items()
                     if name != MANIFEST and Path(name).suffix.lower() != '.pdb'}
    desktop_manifest['files'] = desktop_files
    receipt = (json.dumps(desktop_manifest, indent=2)+'\n').encode('utf-8')
    payload = {f'{prefix}/{name}': (desktop/name, checksum) for name,checksum in desktop_files.items()}
    payload.update({f'{prefix}/samples/Collection Room/{name}': (sample/name, checksum)
                    for name,checksum in sample_files.items()})
    if authoring:
        auth_files = {name: checksum for name, _, checksum, _ in pack._entries}
        auth_files['authoring.json'] = pack.manifest_sha256
        payload.update({f'{prefix}/authoring/{name}': (authoring/name, checksum)
                        for name, checksum in auth_files.items()})
        readme_files.update({f'{prefix}/reference/{name}': (docs/name).read_bytes() for name in CREATOR_REFERENCE})
    with tempfile.TemporaryDirectory(prefix='.poima-preview-', dir=output.parent) as directory:
        staged = Path(directory)/'preview.zip'
        with ZipFile(staged, 'x', compression=ZIP_DEFLATED, compresslevel=6) as archive:
            for name,(path,_) in sorted(payload.items()):
                add_zip_member(archive, name, path)
            for name, contents in sorted(python_files.items()):
                add_zip_member(archive, f'{prefix}/publisher-python/{name}', contents)
            add_zip_member(archive, f'{prefix}/{MANIFEST}', receipt)
            for name, contents in sorted(readme_files.items()):
                add_zip_member(archive, name, contents)
        with ZipFile(staged) as archive:
            for name,(_,checksum) in payload.items():
                with archive.open(name) as member:
                    value = sha256()
                    for block in iter(lambda: member.read(1024*1024), b''):
                        value.update(block)
                if value.hexdigest() != checksum:
                    raise RuntimeError('Package input changed during assembly: '+name)
            for name, contents in readme_files.items():
                if archive.read(name) != contents:
                    raise RuntimeError('Archive guide integrity check failed: '+name)
            if archive.read(f'{prefix}/{MANIFEST}') != receipt:
                raise RuntimeError('Archive desktop receipt integrity check failed.')
            for name, contents in python_files.items():
                if archive.read(f'{prefix}/publisher-python/{name}') != contents:
                    raise RuntimeError('Archive embedded Python integrity check failed: '+name)
        output.hardlink_to(staged)
    return dict(archive=str(output), version=version, sha256=digest(output), bytes=output.stat().st_size,
                installed_authoring_tools=bool(authoring))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', type=Path, required=True, help='Output of build_desktop.py.')
    parser.add_argument('--sample', type=Path, required=True, help='Collection Room export from poima project build.')
    parser.add_argument('--authoring', type=Path, help='Installed authoring pack from package_authoring.py.')
    parser.add_argument('--python-embed', type=Path, help='Official Python 3.13.17 embeddable x64 ZIP.')
    parser.add_argument('--output', type=Path, required=True, help='A new ZIP file.')
    args = parser.parse_args()
    try:
        result = package_preview(args.desktop, args.sample, args.output, args.authoring, args.python_embed)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result))


if __name__ == '__main__':
    main()
