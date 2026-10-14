#!/usr/bin/env python3
"""Report explicitly retired CMake objects; apply only an exact, fresh reviewed plan."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import time

FORMAT = 'poima.retired-build-cleanup.v1'
PLAN_TTL_SECONDS = 900
MAX_PLAN_BYTES = 16 * 1024 * 1024
MAX_OBJECT_BYTES = 512 * 1024 * 1024
MAX_ENTRIES = 50000
MAX_ROOTS = 8
MACHINES = {0x14c, 0x8664, 0x1c0, 0x1c4, 0xaa64}


class Refused(ValueError):
    pass


def require(value, message):
    if not value:
        raise Refused(message)


def identity(st):
    return {key: getattr(st, 'st_' + key) for key in ('dev', 'ino', 'size', 'mtime_ns', 'mode', 'nlink')}


def checked(path):
    """Check every existing ancestor without first resolving away a link."""
    path = Path(path)
    require('..' not in path.parts, 'Parent traversal in input path')
    if not path.is_absolute():
        path = Path.cwd() / path
    for parent in reversed((path, *path.parents)):
        try:
            info = parent.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) &
                getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400), 'Linked/reparse ancestry: ' + str(parent))
    return path


def label_path(root, label):
    require(type(label) is str and label and '\\' not in label and ':' not in label and
            not label.startswith('/') and all(p not in ('', '.', '..') for p in label.split('/')),
            'Use checkout-relative paths with forward slashes')
    path = checked(root / label)
    require(path.is_relative_to(root), 'Path escapes checkout')
    return path


def read_file(path):
    checked(path)
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, 'Nonregular/hardlinked input: ' + str(path))
        digest = hashlib.sha256()
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
        require(identity(before) == identity(os.fstat(fd)), 'File changed while hashing: ' + str(path))
        require(identity(before) == identity(path.lstat()), 'File replaced while hashing: ' + str(path))
        return dict(identity=identity(before), sha256=digest.hexdigest())
    finally:
        os.close(fd)


def json_read(path):
    require(path.stat().st_size <= MAX_PLAN_BYTES, 'Bounded JSON required')
    def unique(pairs):
        out = {}
        for key, value in pairs:
            require(key not in out, 'Duplicate JSON key: ' + key)
            out[key] = value
        return out
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def preset_protection(root):
    """Fail closed for syntax not used by this repository's current presets."""
    paths, sources = [], {}
    for name in ('CMakePresets.json', 'CMakeUserPresets.json'):
        file = root / name
        if not file.exists():
            sources[name] = None
            continue
        sources[name] = read_file(file)
        document = json_read(file)
        require(not document.get('include'), 'Preset includes need explicit maintenance-tool support')
        for preset in document.get('configurePresets', []):
            binary = preset.get('binaryDir')
            if binary is None and preset.get('hidden'):
                continue
            require(type(binary) is str, 'Preset binaryDir inheritance needs explicit tool support')
            binary = binary.replace('${sourceDir}', str(root)).replace('${presetName}', preset['name'])
            require('$' not in binary, 'Unresolved preset variable; refusing cleanup')
            path = checked(binary if Path(binary).is_absolute() else root / binary)
            paths.append(path)
    return paths, sources


def _object_kind(path):
    """Conservative format recognition, never extension alone; unknowns stay."""
    suffix = path.suffix.lower()
    if suffix not in ('.o', '.obj', '.pch', '.gch'):
        return None
    size = path.stat().st_size
    if not 20 <= size <= MAX_OBJECT_BYTES:
        return None
    with path.open('rb') as stream:
        header = stream.read(64)
        if suffix in ('.pch', '.gch'):
            # Only CMake's generated PCH filenames and known GCC/Clang headers.
            if not re.fullmatch(r'cmake_pch\.(h|hxx)\.(pch|gch)', path.name) or size < 64:
                return None
            if suffix == '.pch' and header[:8] == b'CPCH\x01\x08\0\0':
                return 'Clang-CMake-PCH'
            if suffix == '.gch' and re.fullmatch(b'gpch[+COo][0-9]{3}', header[:8]):
                return 'GCC-CMake-PCH'
            return None
        if header[:4] == b'\x7fELF':
            if len(header) < 52 or header[4] not in (1, 2) or header[5] not in (1, 2) or header[6] != 1:
                return None
            endian = '<' if header[5] == 1 else '>'
            is64 = header[4] == 2
            if len(header) < (64 if is64 else 52):
                return None
            fields = struct.unpack_from(endian + ('HHIQQQIHHHHHH' if is64 else 'HHIIIIIHHHHHH'), header, 16)
            kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shnames = fields
            expected = 64 if is64 else 52
            if not (kind == 1 and machine in (3, 40, 62, 183) and version == 1 and entry == 0 and phnum == 0 and
                    ehsize == expected and shsize == (64 if is64 else 40) and 0 < shnum <= 65535 and
                    shoff >= expected and shoff + shnum * shsize <= size and shnames < shnum):
                return None
            for index in range(shnum):
                stream.seek(shoff + index * shsize)
                section = struct.unpack(endian + ('IIQQQQIIQQ' if is64 else 'IIIIIIIIII'), stream.read(shsize))
                if section[1] != 8 and section[4] + section[5] > size:
                    return None
            return 'ELF-relocatable'
        # Ordinary and Microsoft bigobj COFF; never PE executables/import stubs.
        if header[:4] == b'\0\0\xff\xff':
            if len(header) < 56 or struct.unpack_from('<H', header, 4)[0] != 2 or header[12:28] != bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8'):
                return None
            machine = struct.unpack_from('<H', header, 6)[0]
            count, symbols, symbol_count = struct.unpack_from('<III', header, 44)
            offset, symbol_size = 56, 20
        else:
            machine, count, stamp, symbols, symbol_count, optional, flags = struct.unpack_from('<HHIIIHH', header)
            if optional or flags & (2 | 0x2000):
                return None
            offset, symbol_size = 20, 18
        if machine not in MACHINES or not 0 < count <= 65535 or offset + count * 40 > size:
            return None
        if bool(symbols) != bool(symbol_count) or (symbols and (symbols < offset + count * 40 or symbols + symbol_count * symbol_size + 4 > size)):
            return None
        if symbols:
            stream.seek(symbols + symbol_count * symbol_size)
            strings = struct.unpack('<I', stream.read(4))[0]
            if strings < 4 or symbols + symbol_count * symbol_size + strings > size:
                return None
        for index in range(count):
            stream.seek(offset + index * 40)
            section = stream.read(40)
            length, raw, reloc, lines = struct.unpack_from('<IIII', section, 16)
            reloc_count, line_count = struct.unpack_from('<HH', section, 32)
            uninitialized = bool(struct.unpack_from('<I', section, 36)[0] & 0x80) and raw == 0
            if (length and not uninitialized and (raw < offset + count * 40 or raw + length > size)) or reloc + reloc_count * 10 > size or lines + line_count * 6 > size:
                return None
        return 'COFF-relocatable'


def object_kind(path):
    try:
        return _object_kind(path)
    except (struct.error, OverflowError):
        return None  # Malformed/truncated data is preserved, never extension-only.


def ninja_compiler_outputs(selected):
    graph = selected / 'build.ninja'
    if not graph.exists():
        return set(), None
    require(graph.stat().st_size <= MAX_PLAN_BYTES, 'Ninja graph exceeds review cap')
    pin = read_file(graph)
    text = graph.read_text(encoding='utf-8')
    require('# CMAKE generated file: DO NOT EDIT!' in text, 'Unrecognized Ninja generator graph')
    outputs = set()
    # Only literal generated outputs, not link inputs or arbitrary .o names.
    # Unknown variable expansion and other graph dialects are left untouched.
    for line in text.replace('$\n', '').splitlines():
        match = re.fullmatch(r'build (.+?)(?<!\$): ((?:C|CXX|ASM)_COMPILER__[A-Za-z0-9_]+)(?: .*|)', line)
        if not match:
            continue
        for token in re.findall(r'(?:\$.|[^\s])+', match[1]):
            if token == '|' or re.search(r'\$(?![ $:])', token):
                continue
            label = re.sub(r'\$([ $:])', r'\1', token)
            if ':' not in label and '\\' not in label and not label.startswith('/') and all(p not in ('', '.', '..') for p in label.split('/')):
                outputs.add(label)
    return outputs, pin


def generated_namespace(parts):
    protected = {'src','source','sources','owned-source','owned-sources','sdk','assets','content',
                 'evidence','reports','licensed-inputs','inputs','originals','fixtures','reference'}
    if any(p.casefold() in protected - {'src','source','sources'} or p.casefold().endswith('-src') for p in parts[:-1]):
        return False
    for index, part in enumerate(parts[:-1]):
        if part == 'CMakeFiles' and index + 2 < len(parts) and parts[index+1].endswith('.dir'):
            return not any(p.casefold() in protected or p.casefold().endswith('-src') for p in parts[:index])
    return len(parts) > 1 and parts[0] == 'obj' and not any(p.casefold() in protected for p in parts[1:-1])


def scan(root, selected, protected):
    require(selected.is_relative_to(root / 'build') and selected != root / 'build', 'Select a retired CMake root below build/')
    require(selected.is_dir(), 'Retired build root is absent')
    checked(selected)
    require(not any(selected.is_relative_to(p) or p.is_relative_to(selected) for p in protected), 'Selected root overlaps a protected/preset path')
    cache, cmake = selected / 'CMakeCache.txt', selected / 'CMakeFiles'
    require(cache.is_file() and cmake.is_dir(), 'Not a recognized CMake build root')
    checked(cmake); read_file(cache)
    require(cache.stat().st_size <= 4 * 1024 * 1024, 'CMake cache exceeds metadata cap')
    values = {}
    for line in cache.read_text(encoding='utf-8', errors='strict').splitlines():
        match = re.fullmatch(r'([^:#=]+):[^=]+=(.*)', line)
        if match:
            values[match[1]] = match[2]
    require(Path(values.get('CMAKE_HOME_DIRECTORY', '')).resolve() == root.resolve() and values.get('CMAKE_PROJECT_NAME') == 'Poima', 'CMake cache is not this Poima checkout')
    outputs, graph = ninja_compiler_outputs(selected)
    membership, candidates, skipped = {}, [], []
    for directory, dirs, files in os.walk(selected, followlinks=False):
        dirs.sort()
        for name in sorted(dirs + files):
            require(len(membership) < MAX_ENTRIES, 'Selected tree exceeds50000-entry review cap')
            path = checked(Path(directory) / name)
            relative = path.relative_to(root).as_posix()
            info = path.lstat()
            require(stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode), 'Special file in selected root')
            require(name != '.ninja_lock', 'Ninja lock marker present; stop and retire the build first')
            require(stat.S_ISDIR(info.st_mode) or info.st_nlink == 1, 'Hardlinked file in selected root')
            # Directory mtime changes during our own deletion; pin identity only.
            membership[relative] = dict(dev=info.st_dev, ino=info.st_ino, mode=info.st_mode) if path.is_dir() else identity(info)
            if path.is_dir():
                continue
            with path.open('rb') as stream:
                require(stream.read(8) != b'!<thin>\n', 'Thin archive in selected root; external object retention cannot be inferred')
            relative_parts = path.relative_to(selected).parts
            proven_output = path.relative_to(selected).as_posix() in outputs and generated_namespace(relative_parts)
            kind = object_kind(path) if proven_output else None
            if kind:
                candidates.append(dict(path=relative, kind=kind, **read_file(path)))
            else:
                reason = 'not_a_proven_generated_output' if not proven_output else 'unrecognized_object_or_PCH_format'
                skipped.append(dict(path=relative, reason=reason))
    return dict(path=selected.relative_to(root).as_posix(), identity=dict(dev=selected.stat().st_dev, ino=selected.stat().st_ino),
                cache=read_file(cache), ninja_graph=graph, membership=membership, candidates=candidates, skipped=skipped)


def make_plan(root, retired, protect=(), now_ns=None):
    root = checked(root)
    require(root.is_dir() and (root / 'CMakeLists.txt').is_file(), 'Poima checkout required')
    require(0 < len(retired) <= MAX_ROOTS and len(set(x.casefold() for x in retired)) == len(retired), 'Explicit distinct --retired-build selection required')
    selected = [label_path(root, x) for x in retired]
    require(not any(a != b and (a.is_relative_to(b) or b.is_relative_to(a)) for a in selected for b in selected), 'Overlapping retired roots')
    presets, preset_files = preset_protection(root)
    protected = [label_path(root, x) for x in protect]
    trees = [scan(root, p, presets + protected) for p in selected]
    return dict(format=FORMAT, created_ns=time.time_ns() if now_ns is None else now_ns, checkout=str(root),
                checkout_identity=dict(dev=root.stat().st_dev, ino=root.stat().st_ino), protect=list(protect), preset_files=preset_files,
                trees=trees, candidate_bytes=sum(c['identity']['size'] for t in trees for c in t['candidates']))


def apply_plan(path, expected_sha256, *, now_ns=None, unlink=None):
    require(re.fullmatch('[0-9a-f]{64}', expected_sha256 or ''), 'Explicit reviewed plan SHA256 required')
    plan_pin = read_file(path)
    require(plan_pin['sha256'] == expected_sha256, 'Plan bytes differ from reviewed hash')
    plan = json_read(path)
    require(read_file(path) == plan_pin and plan['format'] == FORMAT, 'Plan changed while reading')
    now = time.time_ns() if now_ns is None else now_ns
    require(type(plan['created_ns']) is int and 0 <= now - plan['created_ns'] <= PLAN_TTL_SECONDS * 10**9, 'Plan is stale or future-dated; make a new report')
    root = checked(plan['checkout'])
    require(not any(Path(path).absolute().is_relative_to(root / t['path']) for t in plan['trees']), 'Plan must be outside all selected roots')
    current = make_plan(root, [t['path'] for t in plan['trees']], plan['protect'], now_ns=plan['created_ns'])
    require(current == plan, 'Selected roots, files, protections or preset configuration changed; make a new report')
    deleted = []
    try:
        for tree in plan['trees']:
            for item in tree['candidates']:
                target = label_path(root, item['path'])
                # A second full hash immediately precedes each unlink. Portable
                # Python cannot make compare-and-unlink atomic; retired means idle.
                require(read_file(target) == {k: item[k] for k in ('identity', 'sha256')}, 'Object changed before unlink')
                for parent in target.parents:
                    if parent == root / tree['path']:
                        break
                    require(not (parent / '.ninja_lock').exists(), 'Ninja lock appeared during apply')
                require(not (root / tree['path'] / '.ninja_lock').exists(), 'Ninja lock appeared during apply')
                (os.unlink if unlink is None else unlink)(target)
                deleted.append(item['path'])
    except (OSError, Refused) as error:
        return dict(passed=False, plan_sha256=expected_sha256, deleted=deleted, skipped=[x for t in plan['trees'] for x in t['skipped']], error=str(error), remaining=len([x for t in plan['trees'] for x in t['candidates']])-len(deleted))
    return dict(passed=True, plan_sha256=expected_sha256, deleted=deleted, deleted_bytes=plan['candidate_bytes'], skipped=[x for t in plan['trees'] for x in t['skipped']])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--retired-build', action='append', default=[], help='Explicit checkout-relative retired CMake root, e.g. build/old-cache')
    parser.add_argument('--protect', action='append', default=[], help='Active/retained checkout-relative path; overlapping selected roots are refused')
    parser.add_argument('--write-plan', type=Path, help='Create a new immutable plan exclusively; default only prints report')
    parser.add_argument('--apply-plan', type=Path)
    parser.add_argument('--expected-plan-sha256')
    args = parser.parse_args(argv)
    try:
        if args.apply_plan:
            require(not args.retired_build and not args.protect and not args.write_plan, 'Apply takes only the reviewed plan and its hash')
            result = apply_plan(args.apply_plan, args.expected_plan_sha256)
        else:
            require(not args.expected_plan_sha256, 'Plan hash is only for apply')
            plan = make_plan(args.checkout, args.retired_build, args.protect)
            result = dict(passed=True, mode='report', selected=[t['path'] for t in plan['trees']], candidate_bytes=plan['candidate_bytes'],
                          candidates=[c for t in plan['trees'] for c in t['candidates']], skipped=[c for t in plan['trees'] for c in t['skipped']])
            if args.write_plan:
                output = checked(args.write_plan)
                require(not any(output.is_relative_to(Path(plan['checkout']) / t['path']) for t in plan['trees']), 'Write plan outside selected roots')
                payload = json.dumps(plan, indent=2) + '\n'
                require(len(payload.encode('utf-8')) <= MAX_PLAN_BYTES, 'Plan exceeds16MiB review cap')
                with output.open('x', encoding='utf-8') as stream:
                    stream.write(payload)
                result.update(plan=str(output), plan_sha256=read_file(output)['sha256'], valid_for_seconds=PLAN_TTL_SECONDS)
        print(json.dumps(result, indent=2))
        return 0 if result['passed'] else 1
    except (OSError, ValueError, TypeError, KeyError, struct.error) as error:
        print(json.dumps(dict(passed=False, error=str(error))))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
