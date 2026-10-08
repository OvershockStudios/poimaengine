#!/usr/bin/env python3
"""Capture recorded agent-authored gameplay through a fresh Vulkan replay.

Run Python on the engine's native operating system. This does not start an agent:
it captures three checkpoints of the retained, genuinely agent-authored fixture.
Full RPC records stay in the new output directory; public-summary.json contains
only selected provenance, checks and image metadata.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import datetime
import html
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'docs/evidence/fixtures/agent-escape'
HUD_ID = '000000000000000000000000000000c9'
LABEL = 'Recorded agent authoring + fresh Vulkan replay'


def load_replay():
    specification = importlib.util.spec_from_file_location('poima_agent_escape_replay', FIXTURE / 'replay.py')
    if specification is None or specification.loader is None:
        raise RuntimeError('Cannot load the public replay fixture.')
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def bitmap(path, width, height, require):
    data = path.read_bytes()
    require(len(data) >= 54 and data[:2] == b'BM', 'Capture did not produce a BMP.')
    offset = struct.unpack_from('<I', data, 10)[0]
    header, actual_width, signed_height, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    require(header >= 40 and actual_width == width and abs(signed_height) == height
            and planes == 1 and bits in (24, 32) and compression in (0, 3), 'BMP dimensions/format differ.')
    stride = ((width * bits + 31) // 32) * 4
    require(offset >= 54 and len(data) >= offset + stride * height, 'BMP pixels are incomplete.')
    if compression == 3:
        require(len(data) >= 66 and struct.unpack_from('<III', data, 54) == (0xff0000, 0xff00, 0xff),
                'Unsupported BMP channel masks.')
    # A successful receipt with a black/constant bitmap is not useful visual proof.
    colors = set()
    for y in range(0, height, max(1, height // 16)):
        for x in range(0, width, max(1, width // 32)):
            at = offset + y * stride + x * (bits // 8)
            colors.add(bytes(data[at:at + 3]))
    require(len(colors) >= 4, 'Capture has insufficient sampled pixel variation.')
    return dict(width=actual_width, height=abs(signed_height), bits_per_pixel=bits,
                sampled_distinct_colors=len(colors))


def write_preview(output, frames):
    panels = []
    for frame in frames:
        caption = html.escape(frame['caption'])
        panels.append(f'<figure><img src="{frame["file"]}" alt="{caption}"><figcaption>'
                      f'{caption}<span>Tick {frame["tick"]} · Keys {frame["keys"]}/3</span></figcaption></figure>')
    page = ('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">'
            '<title>Poima — recorded agent game replay</title><style>'
            'body{margin:0;background:#181619;color:#ede8eb;font:16px system-ui,sans-serif}'
            'main{max-width:1100px;margin:auto;padding:32px}h1{font-weight:600}h1::before{content:"";'
            'display:inline-block;width:8px;height:28px;background:#8b3d55;margin-right:12px}'
            'p,span{color:#bbb2b8}figure{margin:24px 0;background:#252126;border:1px solid #44363f}'
            'img{display:block;width:100%;height:auto}figcaption{padding:16px}span{display:block;margin-top:5px}'
            '</style><main><h1>Poima · Prism Vault</h1><p>' + LABEL +
            '. Three renderer readbacks; this is a checkpoint storyboard, not a live authoring session '
            'or a real-time gameplay video.</p>' + ''.join(panels) + '</main>')
    (output / 'preview.html').write_text(page, encoding='utf-8')


def media_dependencies(contact_sheet, gif):
    """Resolve requested offline dependencies before any native/GPU work."""
    if not contact_sheet and not gif:
        return None
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as error:
        raise RuntimeError('--contact-sheet/--gif requires Pillow; install nothing automatically.') from error
    try:
        font = ImageFont.truetype(str(ROOT / 'third_party/source_sans/SourceSans3-Regular.ttf'), 22)
        heading = ImageFont.truetype(str(ROOT / 'third_party/source_sans/SourceSans3-Semibold.ttf'), 26)
    except OSError as error:
        raise RuntimeError('--contact-sheet/--gif requires readable Source Sans fonts with FreeType support.') from error
    return Image, ImageDraw, font, heading


def make_media(output, frames, contact_sheet, gif, digest, dependencies):
    """Optional offline derivatives; original BMPs remain authoritative."""
    if not contact_sheet and not gif:
        return []
    Image, ImageDraw, font, heading = dependencies
    panels = []
    for frame in frames:
        with Image.open(output / frame['file']) as source:
            image = source.convert('RGB')
        panel = Image.new('RGB', (image.width, image.height + 96), '#201b20')
        panel.paste(image, (0, 96))  # No resizing or retouching of renderer pixels.
        draw = ImageDraw.Draw(panel)
        draw.rectangle((0, 0, 7, 95), fill='#8b3d55')
        draw.text((20, 9), frame['caption'], font=heading, fill='#f1e9ed')
        draw.text((20, 45), f'Tick {frame["tick"]} | Keys {frame["keys"]}/3 | Captured Vulkan replay',
                  font=font, fill='#c6b8c0')
        panels.append(panel)
    results = []
    if contact_sheet:
        sheet = Image.new('RGB', (panels[0].width, sum(panel.height for panel in panels)), '#201b20')
        top = 0
        for panel in panels:
            sheet.paste(panel, (0, top))
            top += panel.height
        path = output / 'checkpoints.png'
        sheet.save(path)
        results.append(dict(file=path.name, sha256=digest(path), kind='captioned checkpoint contact sheet'))
    if gif:
        path = output / 'checkpoints.gif'
        panels[0].save(path, save_all=True, append_images=panels[1:], duration=2500, loop=0)
        results.append(dict(file=path.name, sha256=digest(path),
                            kind='three-checkpoint slideshow; 2.5 seconds per image; not real-time playback'))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'hostfxr', 'bridge', 'assembly', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, required=True, help='Explicit native Vulkan device index.')
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=540)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    parser.add_argument('--allow-rebuilt-assembly', action='store_true',
                        help='Allow an unfamiliar rebuilt DLL; its source provenance is not independently verified.')
    parser.add_argument('--contact-sheet', action='store_true', help='Make a captioned PNG with optional Pillow.')
    parser.add_argument('--gif', action='store_true', help='Make a checkpoint slideshow with optional Pillow.')
    args = parser.parse_args()
    if not (128 <= args.width <= 4096 and 128 <= args.height <= 4096) or args.gpu < 0:
        parser.error('Dimensions must be 128..4096 and GPU index nonnegative.')
    paths = {name: getattr(args, name).resolve() for name in ('engine', 'hostfxr', 'bridge', 'assembly', 'output')}
    for name in ('engine', 'hostfxr', 'bridge', 'assembly'):
        if not paths[name].is_file():
            parser.error(f'--{name} must name an existing file.')
    try:
        dependencies = media_dependencies(args.contact_sheet, args.gif)
    except RuntimeError as error:
        parser.error(str(error))
    output = paths['output']
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('--output must be a new directory.')

    evidence = dict(passed=False, label=LABEL, recorded_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    live_agent_session=False, real_time_video=False, physical_input_qualified=False,
                    raster_hud_visual_review_pending=True,
                    cleanup_errors=[], frames=[], media=[], checks=[])
    engine = None
    completed = False
    replay = None
    try:
        replay = load_replay()
        manifest = json.loads((FIXTURE / 'manifest.json').read_text(encoding='utf-8'))
        plan = json.loads((FIXTURE / 'plan.json').read_text(encoding='utf-8'))
        authoring = json.loads((ROOT / 'docs/evidence/m2-agent-game.json').read_text(encoding='utf-8'))
        portable = json.loads((ROOT / 'docs/evidence/m2-agent-game-replay.json').read_text(encoding='utf-8'))
        evidence['hashes'] = {name: replay.digest(paths[name]) for name in ('engine', 'hostfxr', 'bridge', 'assembly')}
        evidence['hashes'].update({name: replay.digest(FIXTURE / name) for name in
                                  ('Escape.cs', 'world.json', 'manifest.json', 'plan.json', 'replay.py')})
        evidence['hashes']['capture_agent_replay.py'] = replay.digest(Path(__file__).resolve())
        replay.require(evidence['hashes']['Escape.cs'] == authoring['source']['sha256'],
                       'Fixture source no longer matches the recorded external agent source.')
        known_assemblies = {authoring['game_assembly_sha256'], portable['hashes']['assembly']}
        known_assembly = evidence['hashes']['assembly'] in known_assemblies
        replay.require(known_assembly or args.allow_rebuilt_assembly,
                       'Unfamiliar assembly hash; use --allow-rebuilt-assembly to accept unverified rebuild provenance.')
        evidence['authoring_provenance'] = dict(provider=authoring['client']['provider'],
            version=authoring['client']['version'], model=authoring['client']['model'],
            recorded_evidence='docs/evidence/m2-agent-game.json', source_retained_verbatim=True,
            assembly_matches_recorded_or_qualified_rebuild=known_assembly,
            supplied_rebuild_provenance_independently_verified=known_assembly,
            assembly_provenance=('Recorded agent artifact or qualified portable rebuild.' if known_assembly else
                                 'Explicitly accepted supplied rebuild; source provenance is not independently verified.'))
        if not known_assembly:
            print('Accepted unfamiliar rebuilt assembly: supplied source provenance is not independently verified.',
                  file=sys.stderr)
        shutil.copyfile(FIXTURE / 'world.json', output / 'world.json')
        (output / 'saves').mkdir()
        engine = replay.Engine(paths['engine'], output)
        authored = engine.rpc('world.inspect')
        engine.rpc('runtime.start', dict(session_id=engine.session, revision=authored['revision']))
        loaded = engine.rpc('runtime.gameplay.load', dict(session_id=engine.session, request_id=uuid.uuid4().hex,
            expected_tick=0, expected_revision=0, hostfxr=str(paths['hostfxr']), bridge=str(paths['bridge']),
            assembly=str(paths['assembly']), type=manifest['type']))
        replay.require(loaded['module']['assembly_sha256'] == evidence['hashes']['assembly'],
                       'Loaded gameplay differs from the supplied compiled artifact.')
        engine.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0,
                                         root=str(output / 'saves')))
        replay.check_state(engine, manifest, 0, 0)

        def capture(name, caption, keys, escaped):
            state = engine.inspect()
            values = replay.check_state(engine, manifest, keys, escaped)
            player = engine.entity(manifest['player_id'])['world_matrix']
            camera = engine.entity(manifest['camera_id'])['world_matrix']
            ui = engine.rpc('runtime.ui.inspect', dict(session_id=engine.session, tick=engine.tick))
            hud = next(row['text'] for row in ui['elements'] if row['id'] == HUD_ID)
            path = output / (name + '.bmp')
            report = engine.rpc('runtime.capture', dict(session_id=engine.session, tick=engine.tick,
                ui_revision=state['ui_revision'], camera=manifest['camera_id'], path=str(path),
                width=args.width, height=args.height, gpu=args.gpu, samples=args.samples))
            replay.require(report['capture_written'] and report['hardware'] and report['nvrhi_errors'] == 0,
                           'Vulkan capture failed or reported graphics errors.')
            replay.require(report['source'] == 'runtime' and report['session_id'] == engine.session
                           and report['tick'] == engine.tick and report['ui_revision'] == state['ui_revision']
                           and report['camera'] == manifest['camera_id'] and report['camera_world'] == camera,
                           'Capture does not identify the observed live camera and UI state.')
            replay.require(report['width'] == args.width and report['height'] == args.height
                           and report['samples'] == args.samples, 'Capture silently changed extent or MSAA.')
            replay.require(engine.inspect() == state and engine.values() == values
                           and engine.entity(manifest['player_id'])['world_matrix'] == player,
                           'Capturing changed authoritative gameplay state.')
            image = bitmap(path, args.width, args.height, replay.require)
            evidence['frames'].append(dict(file=path.name, caption=caption, sha256=replay.digest(path),
                tick=engine.tick, keys=keys, escaped=escaped, hud=hud, gpu=report['gpu'],
                samples=report['samples'], engine_version=report['build_version'],
                nvrhi_errors=report['nvrhi_errors'], image=image, native_report=report))

        locked = replay.phase(engine, plan['phases'][0], manifest)
        replay.require(locked['LockedAttempts'] >= 1, 'The locked exit was not tested.')
        replay.check_hud(engine, 'LOCKED')
        capture('01-locked', 'Exit locked · find three keys', 0, 0)
        one_key = replay.phase(engine, plan['phases'][1], manifest)
        saved_tick = engine.tick
        saved_matrix = engine.entity(manifest['player_id'])['world_matrix']
        replay.require(engine.control(manifest['pause_ui_id'])['intent'] == 2, 'Compiled Pause failed.')
        replay.require(engine.control(manifest['resume_ui_id'])['intent'] == 1, 'Compiled Resume failed.')
        saved = engine.control(manifest['save_ui_id'])
        replay.require(saved.get('save_serviced') and saved['save_operation']['state'] == 3
                       and saved['save_operation']['kind'] == 1, 'Compiled checkpoint Save failed.')
        slot = engine.rpc('save.inspect', dict(slot=manifest['checkpoint_slot']))
        replay.require(slot['selected']['verified'] and slot['generation'] == 1, 'Durable checkpoint is unverified.')
        replay.phase(engine, plan['phases'][2], manifest)
        replay.check_hud(engine, 'ESCAPED')
        capture('02-escaped', 'Three keys collected · escaped', 3, 1)
        previous_session = engine.session
        restored = engine.control(manifest['load_ui_id'])
        replay.require(restored['runtime_replaced'] and engine.session != previous_session
                       and restored.get('save_serviced') and restored['save_operation']['state'] == 3
                       and restored['save_operation']['kind'] == 2, 'Compiled checkpoint Load failed.')
        replay.check_restore(engine, manifest, saved_tick, saved_matrix, one_key)
        capture('03-restored', 'Checkpoint restored · one key retained', 1, 0)
        replay.require(engine.control(manifest['resume_ui_id'])['intent'] == 1, 'Restored Resume failed.')
        replay.phase(engine, plan['restore_and_finish'], manifest)
        replay.check_hud(engine, 'ESCAPED')
        replay.require(engine.tick == 340, 'Restored route ended at an unexpected tick.')
        replay.require(replay.digest(output / 'world.json') == evidence['hashes']['world.json'],
                       'Replay changed the authored world document.')
        replay.require(len({frame['sha256'] for frame in evidence['frames']}) == 3,
                       'Different replay checkpoints produced identical readbacks.')
        engine.close()
        evidence['checks'] = ['Guarded controller input rejects the early exit and collects all three keys.',
            'Compiled Pause/Resume and durable Save/Load callbacks restore the exact player matrix and key handles.',
            'Three distinct hardware Vulkan readbacks identify the live camera, tick and logical UI revision.',
            'Captures preserve gameplay state; the restored controller route completes again at tick 340.']
        write_preview(output, evidence['frames'])
        evidence['media'] = make_media(output, evidence['frames'], args.contact_sheet, args.gif,
                                       replay.digest, dependencies)
        completed = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        if engine is not None:
            try:
                engine.close()
            except BaseException:
                evidence['cleanup_errors'].append(traceback.format_exc())
            evidence['process'] = dict(pid=engine.process.pid, exit_code=engine.process.returncode, calls=engine.calls)
        evidence['passed'] = completed and 'error' not in evidence and not evidence['cleanup_errors']
        (output / 'capture-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
        # Explicit allowlist: raw native reports, RPC arguments, paths and diagnostics remain private.
        summary = {name: evidence[name] for name in ('passed', 'label', 'recorded_utc', 'live_agent_session',
            'real_time_video', 'physical_input_qualified', 'raster_hud_visual_review_pending', 'checks', 'media')}
        for name in ('hashes', 'authoring_provenance'):
            if name in evidence:
                summary[name] = evidence[name]
        summary['frames'] = [{key: frame[key] for key in ('file', 'caption', 'sha256', 'tick', 'keys', 'escaped',
            'hud', 'gpu', 'samples', 'engine_version', 'nvrhi_errors', 'image')} for frame in evidence['frames']]
        summary['cleanup'] = dict(exit_code=None if engine is None else engine.process.returncode,
                                 clean=bool(engine is not None and engine.closed and engine.process.returncode == 0
                                            and not evidence['cleanup_errors']))
        summary['limits'] = ['Previously recorded agent authoring; no new provider turn or fresh game creation.',
            'Three captured checkpoints, not continuous gameplay recording or physical-device qualification.',
            'HUD text is verified semantically; raster HUD presentation still requires visual inspection.',
            'One primitive fixture and selected GPU; not a renderer quality or performance benchmark.',
            'Raw RPC records, absolute paths and failure diagnostics remain in private capture-evidence.json.']
        (output / 'public-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=evidence['passed'], frames=len(evidence['frames']),
                          evidence=str(output / 'capture-evidence.json'), preview=str(output / 'preview.html'))))
    if not evidence['passed']:
        print(evidence.get('error') or evidence['cleanup_errors'], file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
