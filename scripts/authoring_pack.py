#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate an inventoried native/managed authoring pack without executing its tools.

Integrity checks observe filesystem boundaries; they are not a sandbox or a
lease against concurrent writers. Call verify() at each consumption boundary.
"""
from dataclasses import dataclass
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
from types import MappingProxyType

MANIFEST_LIMIT = 64 * 1024
FILE_LIMIT = 16 * 1024 * 1024
TOTAL_LIMIT = 64 * 1024 * 1024
PAYLOAD_ROLES = MappingProxyType({
    'sdk/Poima.Gameplay.dll': 'sdk',
    'analyzers/Poima.Components.Generator.dll': 'analyzer',
    'tools/native-generator/Poima.NativeGame.Generator.dll': 'generator',
    'tools/native-generator/Poima.NativeGame.Generator.deps.json': 'generator',
    'tools/native-generator/Poima.NativeGame.Generator.runtimeconfig.json': 'generator',
    'tools/native-generator/Poima.Gameplay.dll': 'sdk',
    'native/Entry.cs': 'source',
    'build/Poima.Gameplay.props': 'build',
    'build/Poima.Components.targets': 'build',
    'tools/publish_native_gameplay.py': 'publisher',
    'tools/authoring_pack.py': 'validator',
    'notices/POIMA-LICENSE.txt': 'notice',
    'notices/POIMA-THIRD-PARTY-NOTICES.md': 'notice',
})
CORECLR_PAYLOAD_ROLES = MappingProxyType({
    **PAYLOAD_ROLES,
    'bridge/Poima.ManagedBridge.dll': 'bridge',
    'bridge/Poima.ManagedBridge.deps.json': 'bridge',
    'bridge/Poima.ManagedBridge.runtimeconfig.json': 'bridge',
    'bridge/Poima.Gameplay.dll': 'sdk',
})
_HASH = re.compile(r'[0-9a-f]{64}\Z')
_VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+\Z')


def _need(condition, message):
    if not condition:
        raise ValueError(message)


def _plain_status(path):
    value = path.lstat()
    _need(not stat.S_ISLNK(value.st_mode) and not getattr(value, 'st_file_attributes', 0) & 0x400,
          'Authoring pack paths must not be links or reparse points.')
    return value


def _root(path):
    value = Path(path).absolute()
    # Check before resolve() could erase evidence of a linked ancestor.
    for part in reversed((value, *value.parents)):
        info = _plain_status(part)
        _need(stat.S_ISDIR(info.st_mode), 'Authoring pack root must be a regular directory.')
    return value.resolve(strict=True)


def _file(path, limit, *, contents=False):
    before = _plain_status(path)
    _need(stat.S_ISREG(before.st_mode) and before.st_nlink == 1,
          'Authoring pack files must be independent regular files.')
    _need(1 <= before.st_size <= limit, 'Authoring pack file exceeds its byte bounds.')
    digest = hashlib.sha256()
    data = [] if contents else None
    count = 0
    with path.open('rb') as stream:
        opened = os.fstat(stream.fileno())
        _need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
              (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns),
              'Authoring pack file changed while opening.')
        while True:
            block = stream.read(min(1024 * 1024, limit + 1 - count))
            if not block:
                break
            count += len(block)
            _need(count <= limit, 'Authoring pack file grew beyond its byte bound.')
            digest.update(block)
            if contents:
                data.append(block)
    after = _plain_status(path)
    _need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_nlink) ==
          (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_nlink) and count == before.st_size,
          'Authoring pack file changed while reading.')
    return count, digest.hexdigest(), b''.join(data) if contents else None


def _object(pairs):
    result = {}
    for key, value in pairs:
        _need(key not in result, 'Duplicate authoring manifest key.')
        result[key] = value
    return result


def _nonfinite(_):
    raise ValueError('Nonfinite authoring manifest number.')


def _inventory(root, roles=PAYLOAD_ROLES):
    files = {'authoring.json', *roles}
    directories = {parent.as_posix() for name in roles
                   for parent in Path(name).parents if parent != Path('.')}
    found_files, found_directories, aliases = set(), set(), set()
    pending, count = [(root, 0)], 0
    while pending:
        directory, depth = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                count += 1
                _need(count <= 64 and depth + 1 <= 4, 'Authoring pack inventory exceeds its bounds.')
                path = directory / entry.name
                name = path.relative_to(root).as_posix()
                _need(name.casefold() not in aliases, 'Authoring pack contains a case alias.')
                aliases.add(name.casefold())
                info = _plain_status(path)
                if stat.S_ISDIR(info.st_mode):
                    _need(name in directories, 'Unexpected authoring pack directory.')
                    found_directories.add(name)
                    pending.append((path, depth + 1))
                else:
                    _need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and name in files,
                          'Unexpected or linked authoring pack file.')
                    found_files.add(name)
    _need(found_files == files and found_directories == directories, 'Authoring pack inventory is incomplete.')


@dataclass(frozen=True)
class Pack:
    root: Path
    engine_version: str
    manifest_sha256: str
    _entries: tuple

    def resource(self, relative):
        name = relative.as_posix() if isinstance(relative, Path) else relative
        _need(type(name) is str and name in CORECLR_PAYLOAD_ROLES and any(row[0] == name for row in self._entries),
              'Resource is not inventoried by this pack.')
        return self.root / name

    def verify(self):
        fresh = load_pack(self.root, expected_sha256=self.manifest_sha256)
        _need(fresh == self, 'Admitted authoring pack identity changed.')


def load_pack(root, expected_sha256=None):
    """Admit fixed v1 (thirteen) or v2 (seventeen) independent payloads."""
    try:
        _need(expected_sha256 is None or type(expected_sha256) is str and _HASH.fullmatch(expected_sha256),
              'Expected authoring manifest hash must be lowercase SHA256.')
        directory = _root(root)
        _, manifest_hash, raw = _file(directory / 'authoring.json', MANIFEST_LIMIT, contents=True)
        _need(expected_sha256 is None or manifest_hash == expected_sha256, 'Authoring manifest identity differs.')
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_object, parse_constant=_nonfinite)
        _need(type(value) is dict and set(value) == {'format', 'version', 'engine_version', 'framework', 'language', 'files'},
              'Invalid authoring manifest fields.')
        _need(value['format'] == 'poima.authoring-tools' and type(value['version']) is int and value['version'] in (1, 2),
              'Unsupported authoring pack format/version.')
        roles = PAYLOAD_ROLES if value['version'] == 1 else CORECLR_PAYLOAD_ROLES
        version = value['engine_version']
        _need(type(version) is str and len(version) <= 128 and _VERSION.fullmatch(version), 'Invalid authoring engine version.')
        _need(value['framework'] == 'net10.0' and value['language'] == '14.0', 'Unsupported authoring framework/language.')
        entries = value['files']
        _need(type(entries) is list and len(entries) == len(roles),
              'Authoring pack must inventory exactly thirteen payloads.' if value['version'] == 1 else
              'Authoring pack must inventory exactly seventeen payloads.')
        admitted, total = [], 0
        for item in entries:
            _need(type(item) is dict and set(item) == {'path', 'size', 'sha256', 'role'}, 'Invalid authoring resource fields.')
            name, size, digest, role = (item[key] for key in ('path', 'size', 'sha256', 'role'))
            _need(type(name) is str and name in roles and type(role) is str and role == roles[name],
                  'Unknown authoring resource path/role.')
            _need(type(size) is int and 1 <= size <= FILE_LIMIT, 'Invalid authoring resource size.')
            _need(type(digest) is str and _HASH.fullmatch(digest), 'Invalid authoring resource SHA256.')
            total += size
            _need(total <= TOTAL_LIMIT, 'Authoring payload total exceeds 64 MiB.')
            admitted.append((name, size, digest, role))
        _need([row[0] for row in admitted] == sorted(roles), 'Authoring resource order/membership differs.')
        _inventory(directory, roles)
        for name, size, digest, _ in admitted:
            actual_size, actual_hash, _ = _file(directory / name, FILE_LIMIT)
            _need((actual_size, actual_hash) == (size, digest), 'Authoring resource identity differs: ' + name)
        identities = {row[0]: row[1:3] for row in admitted}
        _need(identities['sdk/Poima.Gameplay.dll'] == identities['tools/native-generator/Poima.Gameplay.dll'],
              'Authoring SDK copies differ.')
        if value['version'] == 2:
            _need(identities['sdk/Poima.Gameplay.dll'] == identities['bridge/Poima.Gameplay.dll'],
                  'Authoring SDK copies differ.')
        return Pack(directory, version, manifest_hash, tuple(admitted))
    except (OSError, TypeError, RecursionError) as error:
        raise ValueError('Cannot admit the bounded authoring pack.') from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack_root', type=Path)
    args = parser.parse_args()
    pack = load_pack(args.pack_root)
    print(json.dumps(dict(engine_version=pack.engine_version, manifest_sha256=pack.manifest_sha256,
                          files=len(pack._entries))))


if __name__ == '__main__':
    main()
