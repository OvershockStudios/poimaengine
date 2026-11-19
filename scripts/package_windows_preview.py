#!/usr/bin/env python3
"""Package a self-contained Poima desktop build and its verified Windows sample."""
# SPDX-License-Identifier: Apache-2.0

import argparse
from hashlib import sha256
import json
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from build_desktop import MANIFEST, digest, inventory, prior_output, source_engine_version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', type=Path, required=True, help='Output of build_desktop.py.')
    parser.add_argument('--sample', type=Path, required=True, help='Collection Room export from poima project build.')
    parser.add_argument('--output', type=Path, required=True, help='A new ZIP file.')
    args = parser.parse_args()
    desktop, sample, output = args.desktop.resolve(), args.sample.resolve(), args.output.absolute()
    version = source_engine_version()
    files = prior_output(desktop)
    if not files or 'Poima.Editor.exe' not in files or 'poima.exe' not in files:
        parser.error('Supply a complete Poima desktop build.')
    desktop_manifest = json.loads((desktop/MANIFEST).read_text(encoding='utf-8'))
    if desktop_manifest.get('native_version') != version:
        parser.error('The desktop build must match the selected engine version.')
    if any(name.split('/')[0] in ('samples', 'START.md') for name in files):
        parser.error('The desktop input already includes preview content.')
    manifest = json.loads((sample/'game.json').read_text(encoding='utf-8'))
    if (manifest.get('format') != 'poima.game' or manifest.get('name') != 'Collection Room' or
            manifest.get('engine') != dict(version=version, target_os='Windows', target_arch='x86_64',
                                          executable='runtime/bin/poima.exe')):
        parser.error('The sample must be a Windows x64 Collection Room export from this engine version.')
    sample_files = inventory(sample)
    if 'runtime/bin/poima.exe' not in sample_files or 'content/world.json' not in sample_files:
        parser.error('The sample must include its runtime and world.')
    if output.suffix.lower() != '.zip' or output.is_symlink() or output.exists():
        parser.error('Choose a new .zip file.')
    if any(root in output.resolve().parents for root in (desktop, sample)):
        parser.error('The archive must be outside the input trees.')
    prefix = f'Poima-{version}-Windows-x64'
    guide = (Path(__file__).resolve().parents[1]/'docs'/'WINDOWS_PREVIEW.md').read_bytes()
    desktop_files = {name: checksum for name, checksum in files.items()
                     if name != MANIFEST and Path(name).suffix.lower() != '.pdb'}
    desktop_manifest['files'] = desktop_files
    receipt = (json.dumps(desktop_manifest, indent=2)+'\n').encode('utf-8')
    payload = {f'{prefix}/{name}': (desktop/name, checksum) for name,checksum in desktop_files.items()}
    payload.update({f'{prefix}/samples/Collection Room/{name}': (sample/name, checksum)
                    for name,checksum in sample_files.items()})
    with tempfile.TemporaryDirectory(prefix='.poima-preview-', dir=output.parent) as directory:
        staged = Path(directory)/'preview.zip'
        with ZipFile(staged, 'x', compression=ZIP_DEFLATED, compresslevel=6) as archive:
            for name,(path,_) in sorted(payload.items()):
                archive.write(path, name)
            archive.writestr(f'{prefix}/{MANIFEST}', receipt)
            archive.writestr(f'{prefix}/START.md', guide)
        with ZipFile(staged) as archive:
            for name,(_,checksum) in payload.items():
                with archive.open(name) as member:
                    value = sha256()
                    for block in iter(lambda: member.read(1024*1024), b''):
                        value.update(block)
                if value.hexdigest() != checksum:
                    raise RuntimeError('Package input changed during assembly: '+name)
            if archive.read(f'{prefix}/START.md') != guide:
                raise RuntimeError('Archive guide integrity check failed.')
            if archive.read(f'{prefix}/{MANIFEST}') != receipt:
                raise RuntimeError('Archive desktop receipt integrity check failed.')
        output.hardlink_to(staged)
    print(json.dumps(dict(archive=str(output), version=version, sha256=digest(output), bytes=output.stat().st_size)))


if __name__ == '__main__':
    main()
