#!/usr/bin/env python3
"""Verify a supplied Native AOT inertial game, exact rollback and fresh saves.

The marked ProbeGame is compiled separately. This runner never builds, changes
game source, or patches runtime poses. Each distinct artifact path has its own
process because Native AOT libraries stay pinned until process exit.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from verify_gameplay_inertial import (Game, WorldClient, FIXTURE, TYPE, digest,
    require, without_session, blend_fixture, command, component, node, transform,
    uid, correction, glb)


class NativeGame(Game):
    def load(self, descriptor, error=None):
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=self.revision,
            descriptor=self.native(descriptor), expected_descriptor_sha256=digest(descriptor))
        result = self.rpc('runtime.gameplay.load_native', params, error)
        if error is None:
            self.revision += 1
            require(result['revision'] == self.revision, 'Native load revision differs')
            module = result['module']
            require(module['backend'] == 'native_aot', 'Actual native backend was not selected')
            require(module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
                    'Compiled module did not report Native AOT execution')
            require('requirements' not in module['schema'], 'Service requirements leaked into saved state schema')
            require(module['schema'] == json.loads(descriptor.read_text(encoding='utf-8'))['schema'],
                    'Actual exported state schema differs from its verified descriptor')
            require(self.rpc('runtime.gameplay.load_native', params) == dict(result, replayed=True),
                    'Native load retry did not return its retained receipt')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'descriptor', 'output', 'legacy-binary'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    require(60 <= args.timeout <= 1200, 'Timeout must be 60..1200 seconds')
    inputs = {name: getattr(args, name).resolve() for name in ('binary', 'descriptor', 'legacy_binary')}
    inputs['fixture'] = FIXTURE.resolve()
    inputs['runner'] = Path(__file__).resolve()
    for name, path in inputs.items():
        require(path.is_file(), 'Missing input: '+name)
    descriptor = inputs['descriptor']
    spec = json.loads(descriptor.read_text(encoding='utf-8'))
    require(spec['format'] == 'poima.native-gameplay' and spec['version'] == 2 and spec['type'] == TYPE,
            'Supply the actual published marked ProbeGame descriptor')
    require(spec['minimum_services_bytes'] == 192 and
            'animation_inertial_v1' in spec['required_features'] and 'baseline_v7' in spec['required_features'],
            'Marked artifact must declare the 192-byte animation extension')
    payloads = {}
    for row in spec['files']:
        path = descriptor.parent / row['path']
        require(not path.is_symlink() and path.resolve().is_relative_to(descriptor.parent), 'Artifact payload escapes its directory')
        require(path.is_file() and path.stat().st_size == row['size'] and digest(path) == row['sha256'],
                'Supplied artifact payload differs: '+row['path'])
        payloads[row['path']] = dict(path=path, sha256=row['sha256'])
    output = args.output.resolve()
    require(not output.exists(), 'Output must be a new directory; preserve previous evidence')
    output.mkdir(parents=True)
    deadline = time.monotonic()+args.timeout
    record = dict(passed=False, calls=[], processes=[], checks={}, hashes={name:digest(path) for name,path in inputs.items()},
        artifact_payload_sha256={name:row['sha256'] for name,row in payloads.items()},
        limits=[
            'Native AOT qualification against supplied binaries/artifact; source publication provenance is a separate build record.',
            'No compatible Native AOT reload: replacement is rejected; each library is pinned until process exit.',
            'No GPU, installed GUI, physical input or clean-machine deployment qualification.',
            'No malformed raw Runtime POD/callback testing; separate ABI fixtures qualify SDK/bridge guards and mock callback transfer only.',
            'Rejection proves no game constructor/Initialize/Tick sentinel for these cohorts; arbitrary DLL/module initialization side effects are outside the guarantee.',
            'Measured outgoing secant motion, not foot locking, seam continuity or overshoot prevention.'])
    clients = []
    previous_sentinel, previous_wslenv = os.environ.get('POIMA_INERTIAL_SENTINEL'), os.environ.get('WSLENV')
    if args.windows_interop and os.name != 'nt':
        entries = [entry for entry in (previous_wslenv or '').split(':')
                   if entry and entry.split('/', 1)[0] != 'POIMA_INERTIAL_SENTINEL']
        os.environ['WSLENV'] = ':'.join(entries + ['POIMA_INERTIAL_SENTINEL'])

    def native(path):
        if args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', str(Path(path).resolve())], text=True, timeout=10).strip()
        return str(Path(path).resolve())

    def open_game(label, binary, world):
        require(deadline > time.monotonic(), 'Qualification deadline exceeded before opening host')
        stamp = output/(label+'.sentinel')
        require(not stamp.exists(), 'Sentinel already exists')
        os.environ['POIMA_INERTIAL_SENTINEL'] = native(stamp)
        client = WorldClient.open(str(binary.resolve()), native(world), close_timeout=10)
        clients.append((label, client))
        return NativeGame(client, record, label, native, None, None, deadline), stamp

    def copy_world(source, target):
        shutil.copyfile(source, target)
        assets = Path(str(source)+'.assets')
        if assets.exists():
            shutil.copytree(assets, Path(str(target)+'.assets'))

    def write(game, slot):
        return game.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=game.session, expected_tick=game.tick,
            expected_gameplay_revision=game.revision))

    try:
        world = output/'world.json'
        game, sentinel = open_game('marked-native', inputs['binary'], world)
        doc, blob = blend_fixture()
        model = output/'original.glb';model.write_bytes(glb(doc, blob))
        asset = game.rpc('asset.import', dict(source=native(model)))['asset'];model.unlink()
        audio = output/'original.wav'
        with wave.open(str(audio), 'wb') as stream:
            stream.setparams((1, 2, 48000, 4800, 'NONE', 'not compressed'))
            stream.writeframes(struct.pack('<'+'h'*4800, *([1000, -1000]*2400)))
        sound = game.rpc('asset.audio.import', dict(source=native(audio)))['asset'];audio.unlink()
        ops = [dict(op='asset.instantiate', id=uid(1000+i), name='Rig '+str(i), asset=asset) for i in range(65)]
        for identity, position, motion, half in [(1, (0,-.5,0), 'static', (20,.5,20)),
                (2, (5,1,0), 'kinematic', (.5,.5,.5)), (4, (5,30,0), 'dynamic', (.25,.25,.25))]:
            ops += [dict(op='entity.create', id=uid(identity), name='Body '+str(identity)),
                component(uid(identity), 'Transform', transform(position)),
                component(uid(identity), 'BoxCollider', dict(half_extents=list(half), motion=motion, mass=1, friction=.5, restitution=0))]
        ops += [dict(op='entity.create', id=uid(3), name='Plain'), dict(op='entity.create', id=uid(20), name='Sound'),
            component(uid(20), 'AudioEmitter', dict(asset=sound, gain=1, loop=True, enabled=True))]
        game.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
        authored = world.read_bytes()

        game.start();game.load(descriptor)
        game.configure(3);game.step(1)
        require(game.values()['NullProbe'] == 1, 'Extended non-rig query did not return null')
        require(sentinel.exists() and {'constructor', 'initialize', 'tick'} <= set(sentinel.read_text().splitlines()),
                'Positive compiled sentinel mechanism did not reach all gameplay phases')
        game.configure(Clip=0, Time=.5, Speed=1, Loop=0, Playing=1, BlendTicks=0);game.step(30);game.position((1,1,0))
        game.configure(Clip=1, Time=.25, Speed=2, BlendTicks=60);game.step(15)
        def original(t):
            return (1.75-2*t+correction(-.75,3,t,1), 3+correction(-2,0,t,1), 0)
        game.position(original(.25));game.step(15);game.position(original(.5))
        current, previous = original(.5), original(29/60)
        velocity = [(a-b)*60 for a,b in zip(current, previous)]
        game.configure(Clip=0, Time=1.5, Playing=0, BlendTicks=30);game.step(15)
        def interrupted(t):
            return (1.5+correction(current[0]-1.5,velocity[0],t,.5),
                    1+correction(current[1]-1,velocity[1],t,.5), 0)
        game.position(interrupted(.25))
        require(game.animation()['transition']['mode'] == 'inertial', 'Compiled command did not commit inertial mode')
        before, stamp_before = game.snapshot(), sentinel.read_bytes()
        game.load(descriptor, error=-32060)
        require(game.snapshot() == before and sentinel.read_bytes() == stamp_before,
                'Rejected Native AOT replacement changed state or ran game construction')
        game.step(1);game.position(interrupted(16/60))
        require(game.values()['StagingChecks'] > 0, 'Compiled callback did not observe staged-write isolation')
        game.step(14);game.position((1.5,1,0));game.step(1)
        require(game.animation()['transition'] is None and game.values()['QActiveMode'] == -1 and game.values()['QProgress'] == -1,
                'Extended mode/progress did not clear on completion')
        record['checks']['native_analytic_motion_extended_queries_staging_and_replacement_rejection'] = True

        game.start();game.load(descriptor)
        cases = [(2,{}), (1,dict(Clip=999)), (1,dict(Time=-1)), (1,dict(Speed=9)),
            (1,dict(BlendTicks=3601)), (1,dict(InertialMode=2)), (1,dict(Rig=uid(3)))]
        for mode, update in cases:
            fields = dict(Clip=0, Time=0, Speed=1, BlendTicks=30, InertialMode=1, Rig=game.rig);fields.update(update)
            game.configure(mode, **fields);before = game.snapshot();game.step(3, error=-32040)
            require(game.snapshot() == before, 'Invalid compiled animation callback changed shared state')
            game.configure(0, Rig=game.rig);game.step(1)
            require(game.animation()['clip'] is None, 'Failed compiled callback left a stale staged command')
        game.configure(Clip=1, Time=0, Speed=1, BlendTicks=30);before = game.snapshot()
        game.step(3, [command(game.rig)], error=-32040)
        require(game.snapshot() == before, 'Caller/native-game animation conflict was not atomic')
        game.configure(0);game.step(1)
        game.configure(5, Count=64, Clip=1, Time=.5, Playing=0, BlendTicks=0);game.step(1)
        require(all(game.animation(uid(1000+i))['clip'] == 1 for i in range(64)) and game.animation(uid(1064))['clip'] is None,
                'Exact 64 compiled commands did not commit')
        for count, caller in [(65,[]), (64,[command(uid(1064))])]:
            game.configure(5, Count=count);before = game.snapshot();game.step(1, caller, error=-32040)
            require(game.snapshot() == before, 'Over-limit compiled batch changed state')
        game.configure(5, Count=63, Clip=0, Time=.75)
        game.step(1, [command(uid(1064), time=.25, playing=False)])
        require(game.animation(uid(1062))['clip'] == 0 and game.animation(uid(1063))['clip'] == 1 and game.animation(uid(1064))['clip'] == 0,
                'Combined exact-64 caller/compiled command limit failed')
        record['checks']['duplicate_invalid_mode_target_conflict_and_exact_64_limit'] = True

        saves = output/'saves';saves.mkdir()
        require(game.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))['generation'] == 1,
                'Save configuration generation differs')
        game.start();game.load(descriptor)
        game.configure(Clip=0, Time=0, Speed=1, Playing=1, BlendTicks=0);game.step(30)
        game.configure(Clip=1, Time=.25, Speed=2, BlendTicks=120);game.step(15)
        for kind in ('invalid-sample', 'later-throw'):
            game.configure(7, Clip=2 if kind == 'invalid-sample' else 1, Time=0, Speed=1, Playing=1, BlendTicks=120,
                ThrowAt=str(game.tick+2) if kind == 'later-throw' else '-1')
            before, saved = game.snapshot(), write(game, kind+'-before')
            game.step(50 if kind == 'invalid-sample' else 5, error=-32040)
            require(game.snapshot() == before, 'Failed native batch changed selected animation/physics/audio/game state')
            after = write(game, kind+'-after')
            require(after['sha256'] == saved['sha256'] and after['bytes'] == saved['bytes'],
                    'Failed native batch changed complete serialized history/physics/audio/game state')
            record.setdefault('rollback_payloads', {})[kind] = dict(before_sha256=saved['sha256'], after_sha256=after['sha256'], bytes=after['bytes'])
            elapsed = game.animation()['transition']['elapsed_ticks']
            game.configure(0, ThrowAt='-1');game.step(1)
            require(game.animation()['transition']['elapsed_ticks'] == elapsed+1 and game.entity(uid(2))['kinematic_target'] is None,
                    'Failed batch lost the prior correction or retained queued motion')
            require(game.rpc('runtime.audio.voices', dict(session_id=game.session, tick=game.tick))['next_voice'] == 1,
                    'Failed batch consumed a voice handle')
        game.configure(7, Clip=1, Time=.5, Playing=0, BlendTicks=30);game.step(3)
        require(game.entity(uid(2))['kinematic_target'] is not None and
                game.rpc('runtime.audio.voices', dict(session_id=game.session, tick=game.tick))['next_voice'] == 2,
                'Same native services could not commit after rollback')
        record['checks']['active_history_and_full_payload_rollback_with_physics_audio_and_compiled_state'] = True

        game.start();game.load(descriptor)
        game.configure(Clip=0, Time=0, Speed=1, Playing=1, Loop=0, BlendTicks=0);game.step(30)
        game.configure(Clip=1, Time=.25, Speed=2, BlendTicks=120);game.step(17)
        checkpoint = without_session(game.snapshot());saved = write(game, 'inertial')
        fresh_world = output/'fresh-world.json';copy_world(world, fresh_world)
        fresh, fresh_stamp = open_game('fresh-native-restore', inputs['binary'], fresh_world)
        generation = fresh.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))['generation']
        session = uuid.uuid4().hex
        loaded = fresh.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=generation,
            slot='inertial', expected_generation=1, revision=1, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session,
            gameplay=dict(descriptor=native(descriptor), expected_descriptor_sha256=digest(descriptor))))
        fresh.session, fresh.tick = session, loaded['tick']
        fresh.revision = fresh.inspect()['revision'];fresh.serial = int(fresh.values()['Serial'])
        require(fresh_stamp.read_text().splitlines() == ['constructor'], 'Stopped native restore unexpectedly ran Initialize or Tick')
        require(without_session(fresh.snapshot()) == checkpoint, 'Fresh native process did not restore complete selected state')
        require(write(fresh, 'restored')['sha256'] == saved['sha256'], 'Fresh process changed complete serialized correction/history/state bytes')
        for value in (game, fresh):
            value.configure(Clip=0, Time=1.5, Playing=0, BlendTicks=60);value.step(1)
        require(without_session(game.snapshot()) == without_session(fresh.snapshot()), 'Immediate compiled re-interruption lost restored velocity history')
        game.step(19);fresh.step(3);fresh.step(16)
        require(without_session(game.snapshot()) == without_session(fresh.snapshot()), 'Fresh native continuation depends on request partition')
        game.step(41);fresh.step(41)
        require(without_session(game.snapshot()) == without_session(fresh.snapshot()) and game.animation()['transition'] is None,
                'Fresh compiled correction missed completion')
        record['checks']['fresh_process_complete_payload_and_immediate_compiled_reinterruption'] = True

        copy_root = output/'omitted-feature-artifact';copy_root.mkdir()
        for name, row in payloads.items():
            target = copy_root/name;target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(row['path'], target)
        omitted_descriptor = copy_root/descriptor.name
        omitted = dict(spec, minimum_services_bytes=176, required_features=['baseline_v7'])
        omitted_descriptor.write_text(json.dumps(omitted, indent=2)+'\n', encoding='utf-8')
        record['omitted_descriptor_sha256'] = digest(omitted_descriptor)
        for label, binary, selected in [('descriptor-omission', inputs['binary'], omitted_descriptor),
                ('legacy-native-host', inputs['legacy_binary'], descriptor)]:
            rejected_world = output/(label+'.world.json');copy_world(world, rejected_world)
            cohort, stamp = open_game(label, binary, rejected_world);cohort.start()
            before = cohort.snapshot();failure = cohort.load(selected, error=-32060)
            require(not stamp.exists(), 'Rejected artifact ran game constructor/Initialize/Tick')
            require(cohort.snapshot() == before, 'Rejected artifact changed host state')
            counts = cohort.rpc('runtime.gameplay.collect', dict(session_id=cohort.session))
            require(counts['active_modules'] == 0 and counts['retired_alive'] == 0,
                    'Rejected artifact retained a managed module handle')
            require(counts.get('native', {}).get('active_modules', 0) == 0, 'Rejected artifact retained a native module handle')
            if label == 'descriptor-omission':
                require('host lacks animation_inertial_v1' in failure['message'].lower(), 'Omission did not reach pre-construction Binding host validation')
                require('native' in counts, 'Omission did not actually reach the compiled native entry')
            else:
                require('service prefix' in failure['message'] or 'Unknown required gameplay feature' in failure['message'],
                        'Legacy rejection was not caused by the unsupported service contract')
                require('native' not in counts, 'Legacy host initialized the unsupported native library')
            record['checks'][label.replace('-', '_')+'_rejects_before_game_callbacks'] = True
        require(world.read_bytes() == authored, 'Verification changed original authored world bytes')
        require(all(digest(path) == record['hashes'][name] for name, path in inputs.items()), 'Verification changed preserved input/source binaries')
        require(all(digest(row['path']) == row['sha256'] for row in payloads.values()), 'Verification changed original artifact payloads')
        record['checks']['authored_world_fixture_original_descriptor_and_payloads_unchanged'] = True
        record['passed'] = True
    except BaseException as failure:
        record['failure'] = dict(type=type(failure).__name__, message=str(failure))
        raise
    finally:
        for label, client in reversed(clients):
            cleanup = None
            try:
                client.close()
            except BaseException as failure:
                cleanup = str(failure);record['passed'] = False
            record['processes'].append(dict(host=label, pid=client.transport.process_id, exit_code=client.transport.returncode,
                closed=client.closed, cleanup_error=cleanup, stderr=client.transport.stderr_tail))
            if client.transport.returncode != 0:
                record['passed'] = False
        if previous_sentinel is None:
            os.environ.pop('POIMA_INERTIAL_SENTINEL', None)
        else:
            os.environ['POIMA_INERTIAL_SENTINEL'] = previous_sentinel
        if previous_wslenv is None:
            os.environ.pop('WSLENV', None)
        else:
            os.environ['WSLENV'] = previous_wslenv
        (output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(output/'evidence.json')
    require(record['passed'], 'Owned process cleanup did not complete successfully')
    print(json.dumps(dict(passed=True, checks=record['checks'], rpc_calls=len(record['calls']))))


if __name__ == '__main__':
    main()
