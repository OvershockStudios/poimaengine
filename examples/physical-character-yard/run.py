#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Open an explicitly selected compiled Physical Character Yard in the native player.

Prepare the project first. Select CoreCLR assembly/bridge/hostfxr OR a Native
AOT descriptor. This launcher builds/downloads nothing. Each launch uses a
fresh output directory and its own save root; source authoring stays intact.
The player starts paused at the Begin menu and runs until its window closes.
Compact startup/terminal/error reports and host logs are always retained. Full
RPC traces are diagnostic-only: --trace opts into a 64 MiB per-stream limit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

TYPE = 'Poima.Examples.PhysicalCharacterGame'
MODULE = 'poima.example.physical-character'


def identity(path):
    with Path(path).open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(bytes=Path(path).stat().st_size, sha256=digest)


def project_member(root, relative):
    if not isinstance(relative, str) or '\\' in relative:
        raise ValueError('Project members must be portable relative paths.')
    path = Path(relative)
    if path.is_absolute() or any(part in ('.', '..') for part in path.parts):
        raise ValueError('Project member must stay below its project directory.')
    resolved = (root/path).resolve(strict=True)
    if not resolved.is_relative_to(root) or not resolved.is_file():
        raise ValueError('Project member is not a contained existing file: '+relative)
    return resolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'project', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--assembly', type=Path)
    choice.add_argument('--descriptor', type=Path)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--hostfxr', type=Path)
    parser.add_argument('--gpu', type=int, default=None, help='Explicit native GPU index; omit for native selection.')
    parser.add_argument('--max-seconds', type=int, default=7200, help='Session bound, 1–14400 seconds (default 7200).')
    parser.add_argument('--trace', action='store_true', help='Retain every RPC request/reply for diagnostics (64 MiB per stream). Reaching the trace limit ends the session; off by default. Compact run.json and host logs are always retained.')
    args = parser.parse_args()
    if args.assembly and not (args.bridge and args.hostfxr):
        parser.error('CoreCLR requires --assembly, --bridge and --hostfxr from the selected build.')
    if args.descriptor and (args.bridge or args.hostfxr):
        parser.error('Native AOT uses only --descriptor; do not supply CoreCLR artifacts.')
    if not 1 <= args.max_seconds <= 14400 or (args.gpu is not None and args.gpu < 0):
        parser.error('Choose a 1–14400 second budget and a nonnegative GPU index, or omit --gpu.')
    try:
        for name in ('binary', 'project', 'assembly', 'descriptor', 'bridge', 'hostfxr'):
            path = getattr(args, name)
            if path is not None:
                path = path.resolve(strict=True)
                if not path.is_file():
                    raise ValueError('Expected a compiled/input file: '+str(path))
                setattr(args, name, path)
        if os.name != 'nt' and args.binary.suffix.lower() == '.exe':
            raise ValueError('Run this launcher with Windows Python for Windows poima.exe.')
        args.output = args.output.absolute()
        if args.output.exists() or args.output.is_symlink() or not args.output.parent.is_dir():
            raise ValueError('Use a fresh output directory with an existing parent.')
        manifest = json.loads(args.project.read_bytes())
        root = args.project.parent
        world = project_member(root, manifest['entry']['world'])
        profile = project_member(root, manifest['input_profile'])
        if manifest['entry']['controller'] != f'{2:032x}' or manifest['entry']['camera'] != f'{3:032x}':
            raise ValueError('This sample requires its separately authored player2 and camera3.')
        # The report is a source receipt, not a replacement for runtime checks.
        preparation = json.loads((root/'preparation.json').read_bytes())
        if preparation.get('format') != 'poima.physical-character-preparation.v1' or preparation.get('passed') is not True:
            raise ValueError('Run prepare.py successfully before launching this sample.')
        for relative, pin in preparation['files'].items():
            # A native gameplay adoption may legitimately update project.json.
            if relative != 'project.json' and identity(project_member(root, relative)) != pin:
                raise ValueError('Prepared content changed; prepare a fresh project: '+relative)
        selected = [path for path in (args.binary, args.project, world, profile,
            args.assembly, args.descriptor, args.bridge, args.hostfxr) if path is not None]
        pins = {str(path): identity(path) for path in selected}
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, str(error)+'\nNo player was started.\n')

    args.output.mkdir()
    saves = args.output/'saves'; saves.mkdir()
    report = dict(format='poima.physical-character-interactive.v1', passed=False,
        inputs=pins, calls=[], errors=[], save_root=str(saves),
        rpc_trace=dict(enabled=args.trace, per_stream_limit_bytes=64*1024*1024 if args.trace else None),
        scope='Interactive sample run; no automated gameplay or visual-quality qualification.')
    client = None
    host = None
    player = None
    deadline = time.monotonic()+args.max_seconds
    try:
        # A live player requires the shared owner's event pump. Standalone
        # world stdio deliberately rejects player.start; connecting a client
        # never transfers ownership of this separately launched host.
        endpoint = 'physical-yard-'+uuid.uuid4().hex
        command = [str(args.binary), 'serve', str(world), '--endpoint', endpoint]
        report['host'] = dict(command=command, endpoint=endpoint, pid=None,
            exit_code=None, closed=False, forced_cleanup=False, cleanup_actions=[])
        with (args.output/'host.stdout.log').open('xb') as stdout, (args.output/'host.stderr.log').open('xb') as stderr:
            host = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr, shell=False)
        report['host']['pid'] = host.pid
        tracing = dict(trace_directory=str(args.output/'stdio'), trace_limit_bytes=64*1024*1024) if args.trace else {}
        client = WorldClient.connect(str(args.binary), endpoint, cwd=ROOT, timeout_ms=15000,
            close_timeout=5, **tracing)
        def call(method, params=None, retain=True):
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Interactive session reached its explicit time budget.')
            row = dict(method=method, params=params or {})
            if retain:
                report['calls'].append(row)
            try:
                result = client.call(method, row['params'], timeout=min(45, remaining))
            except RpcError as error:
                row['error'] = dict(code=error.code, message=error.message, data=error.data)
                if not retain:
                    report['calls'].append(row)
                raise
            row['result'] = result
            return result
        revision = call('world.inspect')['revision']
        inspected_input = call('input.inspect', dict(path=str(profile), read_only=True))
        session = uuid.uuid4().hex
        call('runtime.start', dict(session_id=session, revision=revision))
        call('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(saves)))
        module = call('runtime.gameplay.inspect', dict(session_id=session, tick=0))
        config = (dict(descriptor=str(args.descriptor),
                       expected_descriptor_sha256=pins[str(args.descriptor)]['sha256']) if args.descriptor else
                  dict(assembly=str(args.assembly), bridge=str(args.bridge),
                       hostfxr=str(args.hostfxr), type=TYPE))
        loaded = call('runtime.gameplay.load_native' if args.descriptor else 'runtime.gameplay.load',
            dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
                 expected_revision=module['revision'], **config))
        actual = loaded['module']
        backend = 'native_aot' if args.descriptor else 'coreclr'
        if actual['type'] != TYPE or actual['backend'] != backend or actual['schema']['identity'] != MODULE:
            raise ValueError('Selected artifact does not implement the exact sample module/type/backend.')
        if args.assembly and actual['assembly_sha256'] != identity(args.assembly)['sha256']:
            raise ValueError('Loaded CoreCLR assembly differs from the selected file.')
        # Modal selection is runtime state, not an authored ui.element field.
        # Set it before presenting the initial paused menu so a viewport click
        # cannot bypass Begin and capture native movement input.
        ui = call('runtime.ui.inspect', dict(session_id=session, tick=0, limit=1))
        call('runtime.ui.edit', dict(session_id=session, request_id=uuid.uuid4().hex,
            expected_tick=0, expected_ui_revision=ui['ui_revision'], edits=[], modal=f'{500:032x}'))
        launch = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
            expected_generation=0, initially_paused=True, mode='interactive',
            controller=manifest['entry']['controller'], camera=manifest['entry']['camera'],
            input_profile=str(profile), input_revision=inspected_input['revision'],
            pose_interpolation=True, lens_interpolation=True, width=1280, height=720,
            samples=4, shadow_filter='pcf3_fractional')
        if args.gpu is not None:
            launch['gpu'] = args.gpu
        player = call('player.start', launch)
        print('Begin in the menu. WASD/mouse move/look; Space jump; left click fire. '
              'Target motion starts/stops the Knight. Tab releases the cursor. '
              'Save/Load use this launch’s save folder; Retry restores its initial checkpoint. '
              'Loaded checkpoints open paused: choose Resume to continue.', flush=True)
        while player['active']:
            time.sleep(min(.5, max(0., deadline-time.monotonic())))
            player = call('player.inspect', dict(player_id=player['player_id']), retain=False)
        report['player'] = player
        rendered = player.get('report')
        if not isinstance(rendered, dict) or rendered.get('success') is not True:
            raise RuntimeError('Native player did not close successfully; inspect its retained report.')
        if any(identity(Path(path)) != pin for path, pin in pins.items()):
            raise RuntimeError('A selected compiled artifact or authored input changed during play.')
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            # Cleanup has its own finite budget after the play deadline. Never
            # reconnect or replay an uncertain mutation on a failed channel.
            if not client.closed and host is not None and host.poll() is None:
                try:
                    info = client.call('player.inspect', timeout=5)
                    report['cleanup_player'] = info
                    if info['active']:
                        report['cleanup_stop'] = client.call('player.control', dict(
                            player_id=info['player_id'], request_id=uuid.uuid4().hex,
                            expected_control_revision=info['control_revision'], action='stop'), timeout=5)
                    report['host']['shutdown'] = client.shutdown_host(timeout=5)
                    if report['host']['shutdown'].get('closed') is not True:
                        raise RuntimeError('Shared host did not acknowledge shutdown.')
                except BaseException:
                    report['passed'] = False
                    report['errors'].append('Owned shared-host shutdown failed:\n'+traceback.format_exc())
            try:
                client.close()
            except BaseException:
                report['passed'] = False
                report['errors'].append(traceback.format_exc())
            report['close'] = dict(closed=client.closed, pid=client.transport.process_id,
                exit_code=client.transport.returncode,
                cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error))
            if not client.closed or client.transport.returncode != 0 or client.cleanup_error is not None:
                report['passed'] = False
        if host is not None:
            try:
                host.wait(timeout=5)
            except subprocess.TimeoutExpired:
                report['passed'] = False
                report['host']['forced_cleanup'] = True
                for action in ('terminate', 'kill'):
                    if host.poll() is not None:
                        break
                    try:
                        report['host']['cleanup_actions'].append(action)
                        getattr(host, action)()
                        host.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        continue
                    except BaseException:
                        report['errors'].append('Owned host cleanup failed:\n'+traceback.format_exc())
            except BaseException:
                report['passed'] = False
                report['errors'].append('Owned host wait failed:\n'+traceback.format_exc())
            report['host'].update(closed=host.poll() is not None, exit_code=host.returncode)
            if host.poll() is None:
                report['host']['known_live_pid'] = host.pid
                report['errors'].append('Owned native host did not close within its cleanup budget.')
            report['host']['streams'] = {name:identity(args.output/name)
                for name in ('host.stdout.log', 'host.stderr.log')}
            if host.returncode != 0 or report['host']['forced_cleanup']:
                report['passed'] = False
        (args.output/'run.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=report['passed'], report=str(args.output/'run.json'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
