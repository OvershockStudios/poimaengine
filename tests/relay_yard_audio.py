#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify compiled Relay Yard cues with genuine input and retained checkpoints.

Caller supplies licensed character/audio inputs and a prebuilt Native AOT game.
No builds, downloads, teleports, gameplay patches or external sound commands.
Windows device runs use continuous runtime.play replay, not paused GPU stepping.
--offline-only records headless coverage without claiming device qualification.
Every operation and failure remains in a new owned output directory.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]


def need(value, message):
    if not value:
        raise AssertionError(message)


def read(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 64*1024*1024,
         'Expected bounded regular JSON: '+str(path))
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(directory):
    return {p.relative_to(directory).as_posix(): dict(bytes=p.stat().st_size, sha256=sha(p))
            for p in sorted(directory.rglob('*')) if p.is_file()}


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    need(spec is not None and spec.loader is not None, 'Missing helper: '+str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


contract = helper('poima_relay_audio_driver', ROOT/'tests/relay_yard_contract.py')
launcher = contract.load_launcher()
uid = contract.uid
CADENCE = 'c0870000000000000000000000000001'
EPOCH_FIELDS = {'ObservedEpochHigh', 'ObservedEpochLow'}
RESTORE_FIELDS = EPOCH_FIELDS | {'SavePending', 'SaveNotice', 'SaveStatusKey',
                               'LastSaveState', 'LastSaveError', 'LastSaveGeneration'}


def artifact(path, target_os):
    path = path.resolve(strict=True)
    if path.is_dir():
        path /= 'native-gameplay.json'
    descriptor = read(path)
    need(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2 and
         descriptor['type'] == launcher.TYPE and descriptor['schema']['identity'] ==
         'poima.examples.relay-yard' and descriptor['target_os'] == target_os and
         descriptor['target_arch'] == 'x86_64', 'Supply the native Relay Yard artifact for this host')
    names = {'native-gameplay.json'}
    metadata = []
    for row in descriptor['files']:
        relative = Path(row['path'])
        key = relative.as_posix().casefold()
        need(not relative.is_absolute() and '..' not in relative.parts and key not in names,
             'Unsafe or duplicate artifact path')
        names.add(key)
        member = path.parent/relative
        need(member.resolve(strict=True).is_relative_to(path.parent) and not member.is_symlink() and
             member.is_file() and member.stat().st_size == row['size'] and sha(member) == row['sha256'],
             'Native artifact integrity differs')
        if row['role'] == 'metadata':
            metadata.append(member)
    frozen = inventory(path.parent)
    need(set(frozen) == {'native-gameplay.json', *(r['path'] for r in descriptor['files'])},
         'Artifact is not its exact published closure')
    need(len(metadata) == 1, 'Artifact must contain one component manifest')
    manifest = read(metadata[0])
    need(manifest['format'] == 'poima.components' and manifest['version'] == 1 and
         manifest['schemas'] == descriptor['schema']['components'] and
         len(manifest['schemas']) == 3 and {s['id'] for s in manifest['schemas']} ==
         {launcher.locomotion.CONFIG, launcher.locomotion.COMPONENT, CADENCE},
         'Artifact lacks the exact optional audio component declarations')
    return path, descriptor, manifest, frozen


def voices(game):
    result = game.rpc('runtime.audio.voices', dict(session_id=game.session, tick=game.tick, limit=256))
    need(not result['has_more'], 'Voice observation was truncated')
    return contract.normalized(result)


def cadence(game, manifest):
    fields = {f['id']: f['name'] for s in manifest['schemas'] if s['id'] == CADENCE for f in s['fields']}
    result = {}
    for actor in (100, 300):
        raw = game.rpc('runtime.component.get', dict(session_id=game.session,
                      tick=game.tick, id=uid(actor), type=CADENCE))
        result[uid(actor)] = dict(raw=contract.normalized(raw),
                                 values={fields[k]: v for k, v in raw['values'].items()})
    return result


def observation(game, manifest):
    return dict(snapshot=contract.RelayDriver(game, game.record).snapshot(),
                voices=voices(game), cadence=cadence(game, manifest))


def stable_globals(values, excluded):
    return {k: v for k, v in values.items() if k not in excluded}


def compare_replays(left, right):
    # Only the native save-owner epoch differs across fresh processes. Neither
    # side issues gameplay controls after restore, so no other fields are ignored.
    a, b = copy.deepcopy(left), copy.deepcopy(right)
    for value in (a, b):
        value['snapshot']['values'] = stable_globals(value['snapshot']['values'], EPOCH_FIELDS)
    need(a == b, 'Offline/device fresh replay changed native game, voices or cadence')


def checkpoint(game, slot, directory, record):
    request_start = len(record['calls'])
    result = game.write(slot)
    requests = [r for r in record['calls'][request_start:] if r['method'] == 'save.write']
    need(len(requests) == 1 and result['generation'] == 1, 'Checkpoint did not commit generation one')
    need(game.rpc('save.write', requests[0]['params']) == dict(result, replayed=True),
         'Exact save retry did not return its original receipt')
    folder = directory/('slot-'+slot)
    manifest = read(folder/'current.json')
    row = manifest['payload']['current']
    relative = Path(row['file'])
    need(not relative.is_absolute() and '..' not in relative.parts and len(relative.parts) == 1,
         'Unsafe checkpoint member')
    payload = folder/relative
    need(row['generation'] == manifest['payload']['generation'] == 1 and
         payload.stat().st_size == row['bytes'] and sha(payload) == row['sha256'],
         'Checkpoint manifest/payload integrity differs')
    saved = read(payload)
    need(saved['format'] == 'poima.world-save' and saved['version'] == 1 and
         saved['snapshot']['format'] == 'poima.runtime-snapshot' and
         saved['snapshot']['payload']['tick'] == game.tick,
         'Checkpoint does not bind the actual current simulation')
    return dict(result=result, manifest_sha256=sha(folder/'current.json'),
                payload_sha256=sha(payload), payload=saved['snapshot']['payload'])


def extract_sequence(calls):
    sequence = []
    for row in calls:
        if row['method'] != 'runtime.step':
            continue
        params = row['params']
        need(not params.get('motions') and not params.get('sounds') and
             len(params['inputs']) == 1 and params['inputs'][0]['entity'] == uid(100),
             'Trace contains another controller or external world/sound commands')
        segment = {k: copy.deepcopy(v) for k, v in params['inputs'][0].items() if k != 'entity'}
        segment['ticks'] = params['ticks']
        sequence.append(segment)
    need(1 <= len(sequence) <= 256 and 1 <= sum(s['ticks'] for s in sequence) <= 3600,
         'Trace exceeds the shared player/offline replay bounds')
    need(sum(bool(s.get('use')) for s in sequence) == 1, 'Trace must contain exactly one real Use edge')
    return sequence


def replay(game, sequence, path, device, gpu):
    previous, session = game.tick, game.session
    total = sum(segment['ticks'] for segment in sequence)
    state = game.inspect()
    params = dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=previous,
                  expected_structure_revision=state['structure_revision'], path=game.native(path))
    if device:
        params.update(controller=uid(100), camera=uid(101), mode='replay', sequence=sequence,
                      audio=True, gpu=gpu, width=512, height=288, gamepad=dict(mode='disabled'))
        method = 'runtime.play'
    else:
        params.update(listener=uid(101), sequence=[dict(ticks=s['ticks'], inputs=[dict(
            entity=uid(100), **{k: v for k, v in s.items() if k != 'ticks'})]) for s in sequence])
        method = 'runtime.audio.replay'
    result = game.rpc(method, params, timeout=180)
    need(result['success'] and result['tick'] == previous+total and
         result['current_session_id'] == session, 'Replay failed, replaced runtime or advanced another tick count')
    game.tick = result['tick']
    if device:
        audio = result['audio']
        need(result['stop_reason'] == 'replay_complete' and result['runtime_replacements'] == 0 and
             result['nvrhi_errors'] == 0 and result['hardware'] and result['capture_written'] and
             audio['enabled'] and audio['stream_drained'] and audio['timeline_resets'] == 0 and
             audio['driver'] not in ('dummy', 'disk', 'unknown', '') and
             audio['submitted_frames'] == total*800 and audio['max_queued_frames'] <= 5824 and
             audio['peak'] > 0, 'Continuous native player/device diagnostics differ')
        need(path.is_file() and path.read_bytes()[:2] == b'BM', 'Actual native player readback is absent')
    else:
        need(result['capture']['frames'] == total*800 and result['capture']['channels'] == 2 and
             result['capture']['sample_rate'] == 48000 and path.is_file() and
             sha(path) == result['capture']['sha256'] and result['stream']['peak'] > 0,
             'Offline replay did not record its actual finite positive audio')
    before_retry = voices(game)
    need(game.rpc(method, params, timeout=180) == dict(result, replayed=True) and
         voices(game) == before_retry, 'Replay retry advanced state or submitted another timeline')
    return result


def parity(offline, device):
    for key in ('peak', 'over_range_samples', 'voices_started'):
        need(offline['stream'][key] == device['audio'][key], 'Mixer parity differs: '+key)
    need(offline['capture']['frames'] == device['audio']['submitted_frames'], 'PCM frame count differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'artifact', 'source-directory', 'audio-directory', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--offline-only', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    args.source_directory = args.source_directory.resolve(strict=True)
    args.audio_directory = args.audio_directory.resolve(strict=True)
    args.output = args.output.resolve()
    target_os = 'Windows' if os.name == 'nt' or args.windows_interop else 'Linux'
    if not args.offline_only and target_os != 'Windows':
        parser.error('Device cohort is Windows; use --offline-only for native Linux')
    args.descriptor, descriptor, manifest, frozen_artifact = artifact(args.artifact, target_os)
    for protected in (args.descriptor.parent, args.source_directory, args.audio_directory):
        if args.output.is_relative_to(protected) or protected.is_relative_to(args.output):
            parser.error('Output must be outside supplied artifact/content roots')
    if args.output.exists():
        parser.error('Use a new output directory; failed runs retain all artifacts')
    args.output.mkdir(parents=True)

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() \
            if args.windows_interop and os.name != 'nt' else value

    record = dict(passed=False, calls=[], owners=[], groups=[], test_sha256=sha(Path(__file__)),
                  binary_sha256=sha(args.binary), helper_sha256=sha(ROOT/'tests/relay_yard_contract.py'),
                  launcher_sha256=sha(ROOT/'examples/relay-yard/run.py'), offline_only=args.offline_only,
                  artifact=descriptor, scope='Compiled real-input cues, fresh checkpoint continuity and mixer/device submission; no listening or loopback claim.')
    deadline = time.monotonic()+1200
    owners = []
    error = None
    try:
        input_paths = [args.source_directory/name for name in launcher.locomotion.SOURCE_SHA]
        audio_manifest = read(args.audio_directory/'manifest.json')
        input_paths.append(args.audio_directory/'manifest.json')
        for row in (*audio_manifest['packs'].values(), *audio_manifest['sounds'].values()):
            for key in ('license_path', 'path', 'original_path'):
                if key in row:
                    relative = Path(row[key])
                    need(not relative.is_absolute() and '..' not in relative.parts, 'Unsafe audio input path')
                    input_paths.append(args.audio_directory/relative)
        input_pins = {str(p): sha(p) for p in input_paths}
        record['content_input_pins'] = input_pins
        original = args.output/'original'
        original.mkdir()
        game = launcher.OwnedRelay(args, original/'world.json', 'trace-author', record, deadline, native, manifest)
        owners.append(game)
        authored = game.author(args.source_directory)
        need('audio' in authored, 'Original licensed audio graph was not authored')
        world_pin, asset_pin = sha(game.world), inventory(Path(str(game.world)+'.assets'))
        record['authored'] = authored
        game.start(); game.load(); game.saves(original/'saves')
        need(game.module()['module']['backend'] == 'native_aot', 'Gameplay is not the supplied Native AOT module')
        driver = contract.RelayDriver(game, record)
        driver.step(1)
        driver.control('begin')
        need(driver.values()['Started'] == 1 and game.tick == 1,
             'Begin did not gate the real initialized game at tick one')
        # There is no attached player or wall-time simulation clock here.
        # Begin's resume intent cannot advance until the next explicit input.
        baseline = observation(game, manifest)
        baseline_checkpoint = checkpoint(game, 'audio-baseline', original/'saves', record)
        trace_start = len(record['calls'])
        driver.drive_to(-6, 9, 'audible-stride-out')
        driver.drive_to(-6, 7, 'audible-stride-and-cell')
        cell = driver.values()['CellOne']
        driver.use_target(cell)
        sequence = extract_sequence(record['calls'][trace_start:])
        need(driver.values()['Collected'] == 1 and driver.values()['CellOne'] == uid(0) and
             driver.values()['Won'] == 0, 'Single real pickup did not commit')
        game.rpc('runtime.entity', dict(session_id=game.session, tick=game.tick, id=cell), error=-32004)
        active = observation(game, manifest)
        cues = [v for v in active['voices']['voices'] if v['emitter'] == uid(610)]
        need(len(cues) == 1 and cues[0]['emitting'] and 0 < cues[0]['clip_frame'] < 13912 and
             cues[0]['clip_frame'] == (game.tick-cues[0]['start_tick'])*800,
             'Pickup cue did not survive collectible retirement at its current cursor')
        game.entity(610)
        steps = active['cadence'][uid(100)]['values']
        need(int(steps['StepCount']) >= 1 and int(steps['LastVoice']) > 0 and
             any(v['voice'] == int(steps['LastVoice']) and v['emitter'] in (uid(620), uid(621))
                 for v in active['voices']['voices']), 'Real committed travel did not produce a retained footstep')
        active_checkpoint = checkpoint(game, 'audio-active', original/'saves', record)
        need(observation(game, manifest) == active, 'Checkpoint capture changed active cue state')
        record.update(trace=sequence, baseline=baseline, active=active,
                      checkpoints=dict(baseline=baseline_checkpoint, active=active_checkpoint))
        game.close()
        original_pin = inventory(original)
        stage_results = {}
        for stage, slot, expected, trace in (
            ('pickup', 'audio-baseline', baseline, sequence),
            ('continuation', 'audio-active', active, [dict(ticks=20)])):
            runs = {}
            for device in ((False,) if args.offline_only else (False, True)):
                label = stage+('-device' if device else '-offline')
                directory = args.output/label
                shutil.copytree(original, directory)
                owner = launcher.OwnedRelay(args, directory/'world.json', label, record,
                                            deadline, native, manifest)
                owners.append(owner)
                owner.saves(directory/'saves')
                restored = owner.restore(slot)
                immediate = observation(owner, manifest)
                need(immediate == expected, 'Fresh owner changed the exact pre-callback checkpoint')
                output = directory/('replay.bmp' if device else 'replay.wav')
                report = replay(owner, trace, output, device, args.gpu)
                final = observation(owner, manifest)
                if stage == 'pickup':
                    need(final['voices'] == active['voices'] and final['cadence'] == active['cadence'] and
                         stable_globals(final['snapshot']['values'], RESTORE_FIELDS) ==
                         stable_globals(active['snapshot']['values'], RESTORE_FIELDS),
                         'Replayed controller trace did not reproduce original pickup/cadence')
                else:
                    need(final['voices']['next_voice'] == active['voices']['next_voice'] and
                         [v['voice'] for v in final['voices']['voices']] ==
                         [v['voice'] for v in active['voices']['voices']] and
                         not next(v for v in final['voices']['voices'] if v['voice'] == cues[0]['voice'])['emitting'] and
                         final['snapshot']['values']['Collected'] == 1,
                         'Restored neutral continuation duplicated or restarted a completed pickup cue')
                    need(final['cadence'][uid(100)]['values']['StepCount'] == steps['StepCount'],
                         'Stationary continuation generated additional footsteps')
                runs['device' if device else 'offline'] = dict(report=report, final=final)
                record['groups'].append(dict(name=label, restore=restored, exact_pre_callback=True,
                                              report=report, final=final, output_sha256=sha(output)))
                owner.close()
                need(sha(directory/'world.json') == world_pin and
                     inventory(Path(str(directory/'world.json')+'.assets')) == asset_pin and
                     inventory(directory/'saves') == inventory(original/'saves'),
                     'Fresh replay mutated authored/cooked content or retained saves')
            if not args.offline_only:
                parity(runs['offline']['report'], runs['device']['report'])
                compare_replays(runs['offline']['final'], runs['device']['final'])
            stage_results[stage] = dict(parity_qualified=not args.offline_only)
        need(inventory(original) == original_pin and inventory(args.descriptor.parent) == frozen_artifact and
             all(sha(Path(p)) == digest for p, digest in input_pins.items()),
             'Original trace/checkpoints, supplied native artifact or licensed content inputs changed')
        record['stages'] = stage_results
    except BaseException as failure:
        error = failure
        record['error'] = repr(failure)
        record['traceback'] = traceback.format_exc()
    finally:
        for owner in reversed(owners):
            try:
                owner.close()
                transport = owner.client.transport
                need(not transport.stderr_tail and not transport.stderr_truncated,
                     'Native owner produced unexpected stderr')
            except BaseException as failure:
                record.setdefault('cleanup_errors', []).append(repr(failure))
                if error is None:
                    error = failure
        record['passed'] = error is None
        record['rpc_calls'] = len(record['calls'])
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    if error is not None:
        raise error
    print(json.dumps(dict(passed=True, rpc_calls=record['rpc_calls'], groups=len(record['groups']),
                          owners=len(record['owners']), offline_only=args.offline_only)))


if __name__ == '__main__':
    main()
