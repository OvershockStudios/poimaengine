#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create, adopt and export the compiled ComponentGame qualification fixture.

Supply an already published trusted tests/components_gameplay artifact and a
matching installed runtime. This runs native commands, not a C# compiler.
On Windows, execute inside a supervising Job (see the fixture guide).
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient


def require(value, message):
    if not value:
        raise RuntimeError(message)


def pin(path):
    require(path.is_file() and not path.is_symlink(), 'Expected regular input: '+str(path))
    with path.open('rb') as stream:
        return dict(bytes=path.stat().st_size,
                    sha256=hashlib.file_digest(stream, 'sha256').hexdigest())


def inventory(root):
    require(root.is_dir() and not root.is_symlink(), 'Expected an ordinary input directory')
    paths = list(root.rglob('*'))
    require(not any(p.is_symlink() or getattr(p, 'is_junction', lambda: False)() for p in paths),
            'Linked input entries are unsupported')
    files = {p.relative_to(root).as_posix(): pin(p) for p in sorted(paths) if p.is_file()}
    require(len(files) <= 4096 and sum(v['bytes'] for v in files.values()) <= 1024*1048576,
            'Input inventory exceeds fixture budget')
    return files


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def equal(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return a.keys() == b.keys() and all(equal(a[k], b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    return a == b


def windows_job():
    if os.name != 'nt':
        return
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel.IsProcessInJob.restype = wintypes.BOOL
    assigned = wintypes.BOOL()
    require(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(assigned))
            and assigned.value, 'A supervising Windows Job is required')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    windows_job()
    require(not sys.flags.optimize, 'Run without Python optimization')
    binary = args.binary.resolve(strict=True)
    descriptor = args.artifact/'native-gameplay.json' if args.artifact.is_dir() else args.artifact
    descriptor = descriptor.resolve(strict=True)
    require(descriptor.name == 'native-gameplay.json', 'Expected a Native AOT descriptor')
    artifact_spec = json.loads(descriptor.read_bytes())
    require(artifact_spec.get('identity') == 'poima.test.components'
            and artifact_spec.get('type') == 'Poima.Tests.ComponentGame',
            'Supply the published original ComponentGame fixture')
    component_manifest = descriptor.parent/'game.poima-components.json'
    pin(component_manifest)
    runtime = args.runtime.resolve(strict=True)
    artifact_before, runtime_before = inventory(descriptor.parent), inventory(runtime)
    output = args.output.resolve()
    require(not output.exists(), 'Choose a fresh output directory')
    require(not output.is_relative_to(descriptor.parent) and not output.is_relative_to(runtime),
            'Output must be outside the immutable artifact and runtime trees')
    output.mkdir(parents=True)
    started = time.monotonic()
    report = dict(passed=False, binary=pin(binary), artifact=pin(descriptor),
                  harness=pin(Path(__file__)), commands=[], calls=[])
    try:
        def cli(command, name):
            result = subprocess.run([str(p) for p in command], capture_output=True, timeout=60,
                                    stdin=subprocess.DEVNULL, shell=False)
            (output/(name+'-stdout.txt')).write_bytes(result.stdout)
            (output/(name+'-stderr.txt')).write_bytes(result.stderr)
            require(len(result.stdout)+len(result.stderr) <= 8*1048576, 'CLI log budget exceeded')
            require(result.returncode == 0, 'Native command failed: '+name)
            report['commands'].append(dict(name=name, returncode=result.returncode,
                stdout=pin(output/(name+'-stdout.txt')), stderr=pin(output/(name+'-stderr.txt'))))

        project, game = output/'source-project', output/'game'
        cli([binary, 'project', 'create', project, '--name', 'Typed Component Workflow'], 'create')
        manifest = project/'project.json'
        spec = json.loads(manifest.read_bytes())
        world = project/spec['entry']['world']
        with WorldClient.open(binary, world, close_timeout=5) as client:
            def call(method, params=None):
                params = {} if params is None else params
                result = client.call(method, params, timeout=20)
                report['calls'].append(dict(method=method, params=params, result=result))
                return result
            revision = call('world.inspect')['revision']
            imported = call('component.schema.import', dict(request_id=uuid.uuid4().hex,
                base_revision=revision, manifest=json.loads(component_manifest.read_bytes())))
            ids = [f'{value:032x}' for value in (1, 2, 3)]
            fields = [f'{value:032x}' for value in (1, 2, 3)]
            ops = [dict(op='component.set', id=identity, type='game:'+'1'*32,
                value={fields[0]:100, fields[1]:100, fields[2]:str(-(2**63))}) for identity in ids]
            ops.append(dict(op='component.set', id=ids[0], type='game:'+'2'*32,
                            value={fields[0]:ids[1], fields[1]:.125}))
            call('world.transact', dict(request_id=uuid.uuid4().hex,
                                       base_revision=imported['revision'], ops=ops))
        require(client.cleanup_error is None and client.transport.returncode == 0,
                'Authoring owner failed natural closure')
        request = output/'adopt-request.json'
        write(request, dict(request_id=uuid.uuid4().hex, expected_project_sha256=pin(manifest)['sha256'],
                            expected_artifact_sha256=pin(descriptor)['sha256'], values=dict(Mode=0)))
        cli([binary, 'project', 'adopt-gameplay', manifest, '--artifact', descriptor,
             '--request', request], 'adopt')
        source_before = inventory(project)
        cli([binary, 'project', 'build', manifest, '--runtime', runtime, '--output', game], 'export')
        game_spec = json.loads((game/'game.json').read_bytes())
        expected = json.loads(world.read_bytes())
        expected['receipts'] = []
        for key in ('retired_ids', 'retired_template_ids', 'retired_ui_ids', 'retired_component_schemas'):
            if key in expected:
                expected[key] = []
        actual = json.loads((game/game_spec['entry']['world']).read_bytes())
        require(equal(expected, actual), 'Whole typed export differs from declared freeze')
        bundle = inventory(game)
        for item in game_spec['files']:
            require(bundle[item['path']] == dict(bytes=item['size'], sha256=item['sha256']),
                    'Exported file identity mismatch')
        require(inventory(project) == source_before and inventory(descriptor.parent) == artifact_before
                and inventory(runtime) == runtime_before and pin(binary) == report['binary'], 'Input drift')
        report.update(passed=True, source_files=source_before, bundle_files=bundle,
                      whole_typed_export_matches=True, original_inputs_unchanged=True)
    except Exception:
        report['error'] = traceback.format_exc()
    finally:
        report['seconds'] = time.monotonic()-started
        write(output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], seconds=report['seconds'], error=report.get('error'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
