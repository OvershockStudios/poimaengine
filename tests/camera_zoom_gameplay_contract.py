#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compiled fixed-Tick ADS easing, independent curve oracles and exact saves.

Consumes an externally compiled CoreCLR assembly or Native AOT descriptor.
Builds nothing, opens no graphics/audio device, and requires an external
process-tree owner. Continuation compares two cold-restored native owners.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import subprocess
import time

from input_actions_gameplay_contract import OwnedSession, exact, fresh, need
from components_gameplay_contract import sha, uid
from character_gameplay_contract import fixture

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedCameraZoomGame'
IDENTITY = 'poima.test.managed-camera-zoom'
CAMERA = uid(101)
CAP = 32*1024*1024
OUTPUT_CAP = 64*1024*1024


def curve(start, target, elapsed, duration):
    """Independent perspective-power oracle, rather than the SDK log formula."""
    need(all(math.isfinite(x) and 5 <= x <= 150 for x in (start, target)), 'Invalid oracle endpoints')
    need(type(elapsed) is int and type(duration) is int and 0 <= elapsed <= 3600 and
         0 <= duration <= 3600, 'Invalid oracle phase')
    if duration == 0 or elapsed >= duration:
        return target
    if elapsed == 0 or start == target:
        return start
    u = elapsed/duration
    weight = u*u*(3-2*u)
    a, b = math.tan(math.radians(start)/2), math.tan(math.radians(target)/2)
    return math.degrees(2*math.atan(a*(b/a)**weight))


class ZoomSession(OwnedSession):
    def rpc(self, method, params=None, error=False):
        result = super().rpc(method, params, error)
        need(len(self.evidence['calls']) <= 1000, 'Raw RPC population exceeds1000')
        need(len(json.dumps(self.evidence['calls'][-1], allow_nan=False).encode()) <= 1024*1024,
             'Settled RPC exceeds1MiB')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if bool(args.native_descriptor) == bool(args.hostfxr or args.bridge or args.assembly):
        parser.error('Supply either CoreCLR paths or one native descriptor')
    if not args.native_descriptor and not all((args.hostfxr, args.bridge, args.assembly)):
        parser.error('CoreCLR requires --hostfxr --bridge --assembly')
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value
    args.native = native
    run = args.output.resolve();run.mkdir(parents=True, exist_ok=False)
    started = time.monotonic();args.deadline = started+180
    files = {str(Path(__file__).resolve()): Path(__file__).resolve()}
    for path in (ROOT/'tests/input_actions_gameplay_contract.py', ROOT/'tests/character_gameplay_contract.py',
                 ROOT/'tests/components_gameplay_contract.py', ROOT/'managed/Directory.Build.props',
                 ROOT/'managed/Poima.Gameplay/CameraFovTransition.cs',
                 ROOT/'tests/managed_camera_zoom_gameplay/ManagedCameraZoomGame.cs',
                 ROOT/'tests/managed_camera_zoom_gameplay/Poima.ManagedCameraZoomGame.csproj',
                 args.binary, args.hostfxr, args.bridge, args.assembly, args.native_descriptor):
        if path:
            files[str(path.resolve())] = path.resolve()
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve()
        need(descriptor.stat().st_size <= 1024*1024, 'Native descriptor exceeds1MiB')
        metadata = json.loads(descriptor.read_text(encoding='utf-8'))
        need(metadata['type'] == GAME and metadata['identity'] == IDENTITY and metadata['services_version'] == 7 and
             metadata['minimum_services_bytes'] == 320 and metadata['required_features'] == ['baseline_v7', 'camera_lens_v1'],
             'Wrong negotiated native zoom consumer')
        for row in metadata['files']:
            rel = Path(row['path']);path = (descriptor.parent/rel).resolve(strict=True)
            need(not rel.is_absolute() and '..' not in rel.parts and path.is_relative_to(descriptor.parent) and
                 path.is_file() and path.stat().st_size == row['size'] and sha(path) == row['sha256'],
                 'Native artifact closure differs')
            files[str(path)] = path
        image = (descriptor.parent/metadata['library']).resolve(strict=True)
        need(str(image) in files, 'Native image absent from artifact closure')
        config = dict(descriptor=native(descriptor), expected_descriptor_sha256=sha(descriptor))
    else:
        image = args.assembly.resolve()
        for owner in (image, args.bridge.resolve()):
            sdk = owner.parent/'Poima.Gameplay.dll';need(sdk.is_file(), 'Consumer/bridge SDK missing');files[str(sdk)] = sdk
            for suffix in ('.deps.json', '.runtimeconfig.json'):
                dependency = owner.with_suffix(suffix)
                if dependency.is_file():files[str(dependency)] = dependency
        config = dict(hostfxr=native(args.hostfxr), bridge=native(args.bridge), assembly=native(image), type=GAME)
    for path in (ROOT/'tools/python/poima_client').rglob('*'):
        if path.is_file() and path.suffix in ('.py', '.json'):files[str(path.resolve())] = path.resolve()
    pins = {name: sha(path) for name, path in files.items()}
    evidence = dict(passed=False, calls=[], owners=[], checks=[], snapshots=[], numeric_samples=[], source_pins=pins,
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    limits=dict(seconds=180, rpc_rows=1000, rpc_bytes=1024*1024, evidence_bytes=CAP, output_bytes=OUTPUT_CAP),
                    limitations=['Compiled synthetic fixed-Tick world/foreground easing; no rendering, physical input, audio or performance claim.',
                                 'Rapid retarget preserves position continuity, not velocity continuity.',
                                 'Fresh continuation compares independently cold-restored owners; no warm contact-cache equality claim.',
                                 'Native AOT is process-pinned; compatible reload is CoreCLR only.'])
    process = None
    def lens(fov, near=.1, far=100):
        return dict(vertical_fov=float(fov), near=float(near), far=float(far))
    def output_bound():
        need(sum(p.stat().st_size for p in run.rglob('*') if p.is_file()) <= OUTPUT_CAP, 'Whole output exceeds64MiB')
    try:
        world = run/'w.json';process = ZoomSession(args, world, evidence)
        ops = fixture(actor=False)
        for row in ops:
            if row.get('type') == 'Camera':row['value']['viewmodel'] = lens(75, .01, 10)
        ops += [dict(op='entity.create', id=uid(900), name='Falling rollback witness', parent=None),
                dict(op='component.set', id=uid(900), type='Transform',
                     value=dict(position=[10,20,0], rotation=[0,0,0,1], scale=[1,1,1])),
                dict(op='component.set', id=uid(900), type='BoxCollider',
                     value=dict(half_extents=[.5,.5,.5], motion='dynamic', mass=1, friction=.5, restitution=0))]
        revision = process.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=ops))['revision']
        original = world.read_bytes();process.start(revision)
        def structure():
            return process.rpc('runtime.inspect', dict(session_id=process.session))['structure_revision']
        def values():
            return process.game()['module']['values']
        def load():
            return process.rpc('runtime.gameplay.load_native' if args.native_descriptor else 'runtime.gameplay.load',
                dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick,
                     expected_revision=process.game()['revision'], expected_structure_revision=structure(), **config))
        def edit(**fields):
            return process.rpc('runtime.gameplay.edit', dict(session_id=process.session, request_id=fresh(),
                expected_tick=process.tick, expected_revision=process.game()['revision'],
                expected_structure_revision=structure(), values=fields))
        def read():
            return process.rpc('runtime.camera_lens', dict(session_id=process.session, tick=process.tick, entity=CAMERA))['state']
        loaded = load()
        need(loaded['module']['backend'] == evidence['backend'] and loaded['module']['assembly_sha256'] == sha(image),
             'Wrong compiled zoom consumer/backend')
        if args.native_descriptor:
            need(loaded['module']['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
                 'Native consumer allows dynamic code')
        saves = run/'s';saves.mkdir()
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        sequence = 0
        def snapshot(label):
            nonlocal sequence
            sequence += 1;slot = 'z'+str(sequence)
            receipt = process.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=0, session_id=process.session, expected_tick=process.tick,
                expected_gameplay_revision=process.game()['revision'], expected_structure_revision=structure(),
                expected_material_revision=0))
            paths = list((saves/('slot-'+slot)).glob('p-*.bin'))
            need(len(paths) == 1 and paths[0].stat().st_size <= 16*1024*1024 and
                 paths[0].stat().st_size == receipt['bytes'] and sha(paths[0]) == receipt['sha256'], 'Durable receipt differs')
            payload = json.loads(paths[0].read_text(encoding='utf-8'));state = payload['snapshot']
            need(state['version'] == 7 and payload['document']['entities'][CAMERA]['components']['Camera'] ==
                 dict(vertical_fov=70, near=.1, far=100, viewmodel=lens(75,.01,10)), 'Saved zoom rewrote authored baseline')
            evidence['snapshots'].append(dict(label=label, slot=slot, receipt=receipt, snapshot=state))
            output_bound();return copy.deepcopy(state),slot,receipt
        def step(ticks=1, error=False, retry=False):
            request = dict(session_id=process.session, request_id=fresh(), expected_tick=process.tick,
                           expected_structure_revision=structure(), ticks=ticks)
            result = process.rpc('runtime.step', request, error)
            if not error:
                process.tick = result['current_tick'];process.session = result['current_session_id']
                if retry:
                    before,_,_ = snapshot('before-step-retry')
                    need(process.rpc('runtime.step', request) == dict(result, replayed=True), 'Step retry differs')
                    after,_,_ = snapshot('after-step-retry');need(exact(before,after), 'Retry advanced zoom/full state')
            return result
        def numeric(label):
            v = values();native_lens = read()
            need(native_lens['authored_world'] == lens(70), 'Zoom changed frozen authored lens')
            need(native_lens['world']['near'] == .1 and native_lens['world']['far'] == 100 and
                 native_lens['viewmodel']['near'] == .01 and native_lens['viewmodel']['far'] == 10,
                 'FOV-only transition changed clipping')
            errors = {}
            for prefix, channel in (('World','world'), ('Viewmodel','viewmodel')):
                expected = curve(v[prefix+'Start'], v[prefix+'Target'], v['ElapsedTicks'], v['ActiveDurationTicks'])
                actual = native_lens[channel]['vertical_fov']
                need(math.isfinite(actual) and actual == v[prefix+'Current'], 'Committed lens differs from saved current field')
                limit = 2e-12*max(1, abs(expected));error = abs(actual-expected)
                need(error <= limit, 'Compiled easing differs from independent perspective-power oracle')
                if v['ElapsedTicks'] == v['ActiveDurationTicks']:
                    need(actual == v[prefix+'Target'], 'Completed easing did not publish exact endpoint')
                errors[channel] = dict(expected=expected, actual=actual, absolute_error=error, limit=limit)
            evidence['numeric_samples'].append(dict(label=label, tick=process.tick, fields=v,
                                                    state=native_lens, oracle=errors))
            return v,native_lens
        need(read() == dict(entity=CAMERA, world=lens(70), viewmodel=lens(75,.01,10), authored_world=lens(70)),
             'Initial lens baseline differs')
        edit(Mode=1, DurationTicks=12);step(3, retry=True);v,_ = numeric('enter-quarter')
        need(v['ElapsedTicks'] == 3 and v['ActiveDurationTicks'] == 12 and v['Invisible'] == v['Writes'] == 3,
             'Enter ramp phase/staged read differs')
        step(3);numeric('enter-midpoint');step(6);v,_ = numeric('enter-complete')
        need(v['TransitionActive'] == 0 and v['WorldCurrent'] == 35 and v['ViewmodelCurrent'] == 50, 'Enter did not settle')
        edit(Mode=2);step(3);numeric('release-quarter');step(9);v,_ = numeric('release-complete')
        need(v['WorldCurrent'] == 70 and v['ViewmodelCurrent'] == 75, 'Release did not restore exact neutral lenses')
        evidence['checks'].append('Independent quarter/midpoint/end world and foreground curve, exact neutral/aim endpoints, staged invisibility and idempotent step retry')
        edit(Mode=3);last = read();retargets = values()['Retargets']
        for age in range(8):
            step();v,current = numeric('script-'+str(age))
            if age in (0,3,5,7):
                need(v['WorldStart'] == last['world']['vertical_fov'] and
                     v['ViewmodelStart'] == last['viewmodel']['vertical_fov'] and v['ElapsedTicks'] == 1,
                     'Interrupted reversal did not start from the last committed value')
            last = current
        need(v['ScriptStep'] == 4 and v['ScriptActive'] == 0 and v['Retargets'] == retargets+4,
             'Fixed-Tick reversal script differed')
        step(11);v,_ = numeric('script-settled');need(v['WorldCurrent'] == 70 and v['ViewmodelCurrent'] == 75,
                                                          'Rapid reversal did not settle')
        edit(Mode=6, DurationTicks=120);step();v,_ = numeric('zero-duration')
        need(v['ActiveDurationTicks'] == v['ElapsedTicks'] == v['TransitionActive'] == 0 and
             v['WorldCurrent'] == 35 and v['ViewmodelCurrent'] == 50, 'Zero-duration mode was not an exact snap')
        edit(Mode=2, DurationTicks=12);step(12);numeric('neutral-before-rollback')
        evidence['checks'].append('Tick-relative interrupted enter/release/re-enter/release seeds actual committed starts; zero duration snaps exactly despite configured120-Tick duration')
        edit(Mode=4, FailAt=str(process.tick+1));before,_,_ = snapshot('before-later-Tick')
        failure = step(2, error=True)
        need('later-Tick rollback' in failure['message'], 'Later callback failure absent')
        after,_,_ = snapshot('after-later-Tick')
        need(exact(before,after) and not (saves/'slot-zoom-intent').exists(), 'Zoom/game/physics/save-intent batch rollback incomplete')
        edit(Mode=4, FailAt='-1');step();v,_ = numeric('durable-intent')
        epoch = f'{int(v["TicketHigh"]) & ((1<<64)-1):016x}{int(v["TicketLow"]) & ((1<<64)-1):016x}'
        receipt = process.rpc('runtime.save.result', dict(epoch=epoch, sequence=int(v['TicketSequence'])))
        need(receipt['kind'] == 1 and receipt['state'] == 3 and receipt['generation'] == 1 and
             receipt['committed_tick'] == process.tick, 'Zoom durable intent not published')
        evidence['save_intent'] = receipt;edit(Mode=5);step();v,_ = numeric('durable-result-read')
        need(v['SaveSucceeded'] == 1 and v['ElapsedTicks'] == 2, 'Saved result read advanced incorrect phase')
        intent_inventory = {p.name:sha(p) for p in (saves/'slot-zoom-intent').iterdir() if p.is_file()}
        evidence['checks'].append('Later-Tick failure preserves complete native snapshot including falling physics/game phase/lenses and prevents durable intent publication; successful intent/result retained')
        before,_,_ = snapshot('before-readonly-observations')
        numeric('readonly-first');numeric('readonly-second')
        after,_,_ = snapshot('after-readonly-observations')
        need(exact(before,after), 'Readonly lens/game observations advanced the transition or changed full state')
        evidence['checks'].append('Repeated native lens/game inspections do not advance saved midfade phase or change complete native state')
        if not args.native_descriptor:
            before,_,_ = snapshot('before-compatible-reload');old_game = process.game();load()
            after,_,_ = snapshot('after-compatible-reload')
            expected = copy.deepcopy(before);expected['payload']['gameplay_revision'] += 1;expected['sha256'] = after['sha256']
            need(exact(after,expected) and after['sha256'] != before['sha256'], 'Midfade reload changed complete phase/lenses/native state')
            migration = process.game()['module']['migration']
            need(migration['added'] == migration['removed'] == [] and migration['preserved'] == sorted(old_game['module']['values']),
                 'Reload primitive-field preservation differs')
            evidence['checks'].append('Compatible CoreCLR reload retains complete midfade native/game state; only gameplay revision and snapshot digest change')
        saved,slot,saved_receipt = snapshot('fresh-source');saved_structure = structure()
        need(values()['TransitionActive'] == 1 and values()['ElapsedTicks'] == 2, 'Durable continuation source was not midfade')
        def restore(existing):
            session = fresh()
            result = process.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=slot,
                expected_generation=1, revision=revision, expected_session_id=process.session if existing else None,
                expected_tick=process.tick if existing else None,
                expected_gameplay_revision=process.game()['revision'] if existing else None,
                expected_structure_revision=structure() if existing else None, new_session_id=session, gameplay=config))
            process.session = session;process.tick = result['tick']
        restore(True);reference,_,_ = snapshot('fresh-reference-restored')
        need(exact(reference,saved), 'Same-owner cold reconstruction changed full checkpoint')
        step(3);numeric('reference-continued');continuation,_,_ = snapshot('fresh-expected-continuation')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        process = ZoomSession(args, world, evidence)
        process.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=native(saves)))
        restore(False);actual,_,_ = snapshot('fresh-restored')
        need(exact(actual,saved) and structure() == saved_structure, 'Fresh restore changed full midfade state')
        step(3);numeric('fresh-continued');actual,_,_ = snapshot('fresh-actual-continuation')
        need(exact(actual,continuation), 'Independently reconstructed three-Tick full continuation differs')
        need(world.read_bytes() == original and intent_inventory ==
             {p.name:sha(p) for p in (saves/'slot-zoom-intent').iterdir() if p.is_file()}, 'Authored bytes/original intent slot changed')
        original_payload = list((saves/('slot-'+slot)).glob('p-*.bin'))
        need(len(original_payload) == 1 and sha(original_payload[0]) == saved_receipt['sha256'], 'Original midfade generation changed')
        process.rpc('runtime.stop', dict(session_id=process.session));process.close();process = None
        evidence['checks'].append('Saved midfade restores exactly in same and fresh native owners; independently cold-restored three-Tick continuation matches complete snapshots and original artifacts remain unchanged')
        need(all(sha(path) == pins[name] for name,path in files.items()), 'Consumed source/image closure changed')
        output_bound();need(time.monotonic() < args.deadline, 'Whole fixture deadline expired');evidence['passed'] = True
    except BaseException as error:
        evidence['error'] = repr(error);raise
    finally:
        if process:
            try:process.close()
            except BaseException as error:evidence['close_error'] = repr(error);evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-started;evidence['rpc_count'] = len(evidence['calls'])
        try:
            data = (json.dumps(evidence, indent=2, allow_nan=False)+'\n').encode()
            need(len(data) <= CAP, 'Final evidence exceeds32MiB')
            output_bound();need(sum(p.stat().st_size for p in run.rglob('*') if p.is_file())+len(data) <= OUTPUT_CAP,
                                'Final output exceeds64MiB')
            (run/'evidence.json').write_bytes(data)
            if time.monotonic() > args.deadline:
                evidence['passed'] = False;evidence['deadline_error'] = 'Final publication exceeded180s'
                (run/'evidence.json').rename(run/'late-evidence.json')
                raise TimeoutError(evidence['deadline_error'])
        except BaseException as publication_error:
            summary = dict(passed=False, error=evidence.get('error'), close_error=evidence.get('close_error'),
                           publication_error=repr(publication_error), rpc_count=evidence['rpc_count'],
                           seconds=time.monotonic()-started)
            (run/'failure.json').write_text(json.dumps(summary, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            if 'error' not in evidence:raise
        print(run/'evidence.json', flush=True)
        if 'close_error' in evidence and 'error' not in evidence:raise RuntimeError(evidence['close_error'])


if __name__ == '__main__':main()
