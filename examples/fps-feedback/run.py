#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Launch the separately built FPS Feedback Range through the native player.

This is an explicit interactive graphics operation when invoked. It does not
build gameplay, adopt an artifact, download content, retry or alter the source
fixture. Use a simulation/Vulkan build with camera_pose_v1 support.
"""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient

TYPE = 'Poima.Examples.FpsFeedbackGame'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    module = parser.add_mutually_exclusive_group(required=True)
    module.add_argument('--assembly', type=Path)
    module.add_argument('--descriptor', type=Path)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--hostfxr', type=Path)
    parser.add_argument('--save-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Fresh log/stdio directory.')
    parser.add_argument('--gpu', type=int, default=-1)
    parser.add_argument('--pose-interpolation', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--lens-interpolation', action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()
    if args.assembly and not (args.bridge and args.hostfxr):
        parser.error('CoreCLR requires explicit bridge and hostfxr.')
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error('Output must be fresh with an existing parent.')
    manifest = json.loads(args.project.read_bytes())
    world = args.project.parent/manifest['entry']['world']
    args.output.mkdir()
    args.save_root.mkdir(parents=True, exist_ok=True)
    records = []
    # Paths have the executable platform's conventions; use native Python with
    # native Windows paths when running a Windows binary.
    with WorldClient.open(str(args.binary.resolve()), str(world.resolve()),
                          trace_directory=str((args.output/'stdio').resolve())) as client:
        def call(method, params=None, timeout=30):
            row = dict(method=method, params=params or {})
            records.append(row)
            result = client.call(method, params or {}, timeout=timeout)
            row['result'] = result
            return result
        try:
            session = uuid.uuid4().hex
            revision = call('world.inspect')['revision']
            call('runtime.start', dict(session_id=session, revision=revision))
            gameplay = call('runtime.gameplay.inspect', dict(session_id=session, tick=0))
            config = (dict(descriptor=str(args.descriptor.resolve())) if args.descriptor else
                      dict(assembly=str(args.assembly.resolve()), bridge=str(args.bridge.resolve()),
                           hostfxr=str(args.hostfxr.resolve()), type=TYPE))
            call('runtime.gameplay.load_native' if args.descriptor else 'runtime.gameplay.load',
                 dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=0,
                      expected_revision=gameplay['revision'], **config))
            call('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0,
                                        root=str(args.save_root.resolve())))
            print('Begin in the UI. WASD/mouse move and look; C crouches; right mouse aims; '
                  'left mouse fires; R reloads. Menu provides save/load. Tab releases the cursor.', flush=True)
            call('runtime.play', dict(request_id=uuid.uuid4().hex, session_id=session,
                 expected_tick=0, controller=manifest['entry']['controller'], camera=manifest['entry']['camera'],
                 mode='interactive', pose_interpolation=args.pose_interpolation, lens_interpolation=args.lens_interpolation, width=1280, height=720, gpu=args.gpu), timeout=86400)
        finally:
            (args.output/'calls.json').write_text(json.dumps(records, indent=2, allow_nan=False)+'\n', encoding='utf-8')


if __name__ == '__main__': main()
