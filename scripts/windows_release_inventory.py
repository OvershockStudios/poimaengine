#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create the release inventory for a packaged Windows engine ZIP."""

import argparse
from hashlib import file_digest
import json
from pathlib import Path
import re
import tempfile
from zipfile import ZipFile


def build_inventory(archive: Path) -> dict:
    match = re.fullmatch(
        r'Poima-((?:0|[1-9][0-9]{0,7})\.(?:0|[1-9][0-9]{0,7})\.(?:0|[1-9][0-9]{0,7})'
        r'(?:-(?:alpha|beta|rc)\.[1-9][0-9]{0,7})?)-Windows-x64\.zip', archive.name)
    if not match:
        raise ValueError('The archive must use the Poima-<version>-Windows-x64.zip filename.')
    version = match[1]
    prefix = archive.stem + '/'
    files = {}
    seen = set()
    with archive.open('rb') as source:
        checksum = file_digest(source, 'sha256').hexdigest()
        length = source.tell()
        source.seek(0)
        with ZipFile(source) as package:
            for member in package.infolist():
                if member.is_dir() or not member.filename.startswith(prefix):
                    raise ValueError('Every packaged file must be under the matching release folder.')
                name = member.filename[len(prefix):]
                if not name or name.casefold() in seen:
                    raise ValueError('Repeated or empty package path.')
                seen.add(name.casefold())
                with package.open(member) as file:
                    digest = file_digest(file, 'sha256').hexdigest()
                files[name] = dict(path=name, bytes=member.file_size, sha256=digest)
            for name in ('Poima.Editor.exe', 'poima.exe', 'poima-desktop-build.json', 'START.md'):
                if name not in files:
                    raise ValueError(f'The package is missing {name}.')
            with package.open(prefix + 'poima-desktop-build.json') as receipt:
                if json.load(receipt)['native_version'] != version:
                    raise ValueError('The native engine version must match the archive name.')
        source.seek(0)
        if file_digest(source, 'sha256').hexdigest() != checksum:
            raise ValueError('The archive changed while the inventory was being read.')
    return dict(format='poima.windows-release', version=1, engine_version=version,
                archive=dict(name=archive.name, bytes=length, sha256=checksum),
                files=[files[name] for name in sorted(files)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    source = parser.parse_args().archive.absolute()
    output = source.with_suffix('.manifest.json')
    if output.exists() or output.is_symlink():
        parser.error('Choose an archive without an existing release inventory.')
    release = build_inventory(source)
    with tempfile.TemporaryDirectory(prefix='.poima-windows-inventory-', dir=output.parent) as directory:
        staged = Path(directory) / 'inventory.json'
        staged.write_text(json.dumps(release, indent=2) + '\n', encoding='utf-8')
        output.hardlink_to(staged)
    print(json.dumps(dict(inventory=str(output), files=len(release['files']), archive=release['archive'])))


if __name__ == '__main__':
    main()
