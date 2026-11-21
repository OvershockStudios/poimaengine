#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify a compiled press mechanism through scene particles, rollback and cold saves."""
import argparse
import json
import math
from pathlib import Path
import shutil
import tempfile
import traceback
from uuid import uuid4

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from world_particles_contract import (ParticleContract, PLAYER, action, component, counters, fixed,
                                      near_vector, transform)
from world_terrain_contract import CAPTURE_LENS, CAPTURE_SIZE, file_hash, projection, success, tagged, tree_hashes, uid
from poima_client import RpcError


TYPE = 'Poima.Tests.ManagedParticleBurstGame'
IDENTITY = 'poima.test.managed-particle-burst'
REQUIRED_FEATURES = ['baseline_v7', 'particle_burst_v1']
PRESS, TEMPLATE, CAMERA = uid(100), uid(110), uid(120)
NAMED_ACTION = 'press-spark'
PRESS_START = [-1, 1, -4]
PRESS_FIRST_STOP = [-2, 1, -4]
PRESS_SECOND_STOP = [-3, 1, -4]
TEMPORARY_STOP = [1, 1, -4]
# The first stop rotates through twice asin(0.1) about the local Z axis.
# Its cosine is 1 - 2 * 0.1^2, and its sine is 2 * 0.1 * sqrt(1 - 0.1^2).
FIRST_COS, FIRST_SIN = .98, .2*math.sqrt(.99)
FIRST_CENTER = [-2+.25*FIRST_COS, 1+.25*FIRST_SIN, -4]
FIRST_VELOCITY = [-.6*FIRST_SIN, .6*FIRST_COS, 0]
SECOND_CENTER = [-2.75, 1, -4]
TEMPORARY_CENTER = [1.25, 1, -4]
COLORS = ([1, .015, .015], [.015, 1, .015])
BODY = dict(motion='kinematic', half_extents=[.08]*3, mass=1, friction=.5, restitution=0)


def emitter(temporary):
    effect = dict(spawn_space='local', simulation_space='world', seed=f'{73+temporary:016x}',
                  lifetime_ticks=4 if temporary else 6, size_m=.22, linear_rgb=COLORS[temporary],
                  offset_m=fixed([.25, 0, 0]), velocity_m_s=fixed([0, .6, 0]),
                  emitting=False, live_limit=5)
    if not temporary:
        effect['burst'] = dict(action=NAMED_ACTION, count=2)
    return effect


def input_rows(use, named):
    return [dict(entity=PLAYER, use=use, actions=[action(NAMED_ACTION, pressed=named)])]


class GameplayBurstContract(ParticleContract):
    def configure_gameplay(self):
        descriptor = self.args.native_descriptor
        if descriptor:
            descriptor = descriptor.resolve(strict=True)
            metadata = json.loads(descriptor.read_bytes())
            assert (metadata['type'], metadata['identity']) == (TYPE, IDENTITY)
            assert (metadata['call_version'], metadata['call_bytes'], metadata['services_version'],
                    metadata['minimum_services_bytes']) == (1, 80, 7, 536)
            assert metadata['required_features'] == REQUIRED_FEATURES, metadata
            files = {}
            for row in metadata['files']:
                relative = Path(row['path'])
                path = (descriptor.parent/relative).resolve(strict=True)
                assert path.is_relative_to(descriptor.parent) and file_hash(path) == row['sha256']
                assert path.stat().st_size == row['size'], row
                files[row['path']] = path
            self.image_hash = file_hash(files[metadata['library']])
            self.measurements['gameplay_artifact'] = dict(descriptor_sha256=file_hash(descriptor), files=metadata['files'])
            if self.args.runtime_root:
                destination = self.project/'gameplay'
                destination.mkdir()
                shutil.copyfile(descriptor, destination/'native-gameplay.json')
                for name, source in files.items():
                    target = destination/name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
                assert file_hash(destination/'native-gameplay.json') == file_hash(descriptor)
                manifest = json.loads(self.manifest.read_bytes())
                manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
                self.manifest.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
            return dict(descriptor=self.native(descriptor), expected_descriptor_sha256=file_hash(descriptor))

        self.image_hash = file_hash(self.args.assembly)
        files = (self.args.hostfxr, self.args.bridge, self.args.assembly,
                 self.args.bridge.parent/'Poima.Gameplay.dll', self.args.assembly.parent/'Poima.Gameplay.dll')
        self.measurements['gameplay_artifact'] = {str(path): file_hash(path) for path in files}
        return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge),
                    assembly=self.native(self.args.assembly), type=TYPE)

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Workshop Spark Press')))
        self.config = self.configure_gameplay()
        before = self.world_call('world.inspect')['revision']
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[.2]*3, exposure=1)),
               dict(op='entity.create', id=PRESS, name='Kinematic shop press'),
               component(PRESS, 'Transform', transform(PRESS_START)),
               component(PRESS, 'BoxCollider', BODY),
               component(PRESS, 'ParticleEmitter', emitter(False)),
               dict(op='template.set', id=TEMPLATE, name='Temporary ejected spark source', components={
                   'Transform': transform([4, 1, -4]), 'BoxCollider': BODY, 'ParticleEmitter': emitter(True)}),
               dict(op='entity.create', id=CAMERA, name='Fixed witness camera'),
               component(CAMERA, 'Transform', transform([0, 1, 0])),
               component(CAMERA, 'Camera', CAPTURE_LENS)]
        result = self.world_call('world.transact', request_id=uuid4().hex, base_revision=before, ops=ops)
        assert result['committed'] and result['revision'] == before+1, result
        return result['revision']

    def gameplay(self, client, session, tick):
        result = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = result['module']
        assert module['backend'] == ('native_aot' if self.args.native_descriptor else 'coreclr'), module
        assert module['type'] == TYPE and module['schema']['identity'] == IDENTITY, module
        assert module['assembly_sha256'] == self.image_hash, module
        if self.args.native_descriptor:
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False), module
        assert (result['session_id'], result['tick']) == (session, tick), result
        return result

    def start(self, client, saves, game=None, **values):
        session = self.initialize(client, self.revision, uuid4().hex, saves, game)
        if not game:
            method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
            result = client.call(method, dict(session_id=session, request_id=uuid4().hex, expected_tick=0,
                                        expected_revision=0, values=values, **self.config))
            assert result['revision'] == 1 and result['replayed'] is False, result
        state = self.state(client, session, 0)
        assert state['particles']['counts'] == counters(0, 0), state
        return session

    def state(self, client, session, tick):
        gameplay = self.gameplay(client, session, tick)
        particles = self.observed(client, session, tick)
        assert type(gameplay['structure_revision']) is int and gameplay['structure_revision'] >= 0, gameplay
        return dict(particles=particles, values=gameplay['module']['values'], revision=gameplay['revision'],
                    structure=gameplay['structure_revision'])

    def archive(self, client, saves, state, slot):
        particles = state['particles']
        receipt = client.call('save.write', dict(request_id=uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=particles['session_id'], expected_tick=particles['tick'],
            expected_gameplay_revision=state['revision'], expected_structure_revision=state['structure']))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert len(raw) == receipt['bytes'] and file_hash(path) == receipt['sha256'], receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['particles']['version'] == 1
        assert payload['particles']['state']['status']['tick'] == payload['tick'] == particles['tick']
        self.measurements.setdefault('archives', {})[slot] = receipt
        return raw

    def advance(self, client, state, *, ticks=1, use=False, named=False, retry=False):
        session, tick = (state['particles'][key] for key in ('session_id', 'tick'))
        request = dict(session_id=session, request_id=uuid4().hex, expected_tick=tick, ticks=ticks,
                       expected_structure_revision=state['structure'], inputs=input_rows(use, named))
        result = client.call('runtime.step', request)
        assert result['current_session_id'] == session and result['current_tick'] == tick+ticks, result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        after = self.state(client, session, tick+ticks)
        if retry:
            assert exact(client.call('runtime.step', request), dict(result, replayed=True)), result
            assert exact(self.state(client, session, tick+ticks), after)
        return after

    def check_scene(self, state):
        phase = state['particles']
        tick = phase['tick']
        expected = {1: counters(5, 5), 2: counters(5, 5), 3: counters(6, 6, 1),
                    4: counters(6, 6, 3), 7: counters(1, 6, 3, 5)}
        assert exact(phase['counts'], expected[tick]), phase
        assert state['structure'] == 1
        uses = 1 if tick < 3 else 2
        temporary = state['values']['TemporarySource']
        assert temporary not in (PRESS, PLAYER, TEMPLATE, uid(0))
        assert exact(state['values'], dict(TemporarySource=temporary, Mode=0, Uses=uses, Ticks=tick, FailAt='-1'))
        sources = {row['entity']: row for row in phase['sources']}
        assert set(sources) == {PRESS, temporary}, sources
        near_vector(sources[PRESS]['position_m'], PRESS_FIRST_STOP if uses == 1 else PRESS_SECOND_STOP)
        near_vector(sources[temporary]['position_m'], TEMPORARY_STOP)
        ids = ['1', '2', '3', '4', '5'] if tick <= 2 else ['1', '2', '3', '4', '5', '6'] if tick <= 4 else ['6']
        assert [row['id'] for row in phase['particles']] == ids, phase
        for row in phase['particles']:
            is_temporary = row['id'] == '5'
            is_first = row['id'] != '6'
            age = tick - (1 if is_first else 3)
            source = sources[temporary if is_temporary else PRESS]
            center = TEMPORARY_CENTER if is_temporary else FIRST_CENTER if is_first else SECOND_CENTER
            velocity = FIRST_VELOCITY if is_first and not is_temporary else [0, .6, 0]
            assert row['emitter'] == source['id'] and row['age_ticks'] == age, row
            assert row['lifetime_ticks'] == (4 if is_temporary else 6), row
            near_vector(row['position_m'], [origin+age*speed/60 for origin, speed in zip(center, velocity, strict=True)])
            near_vector(row['velocity_m_s'], velocity)
            near_vector(row['linear_rgba'], [*COLORS[is_temporary], 1])

    def capture(self, client, state, label):
        tick = state['particles']['tick']
        width, height = CAPTURE_SIZE
        path = self.root/(label+'.bmp')
        report = client.call('runtime.capture', dict(session_id=state['particles']['session_id'], tick=tick,
            structure_revision=1, camera=CAMERA, path=self.native(path), width=width, height=height,
            samples=1, gpu=self.args.gpu, reconstruction='none', capture_frames=2))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0
        assert report['nvrhi_errors'] == 0 and report['tick'] == tick and report['lens'] == CAPTURE_LENS
        assert report['render_diagnostics']['last_draws']['streamed_draws'] == (2 if tick == 2 else 1), report
        image = pixels(path)
        if tick == 2:
            first_aged = [origin+speed/60 for origin, speed in zip(FIRST_CENTER, FIRST_VELOCITY, strict=True)]
            targets = [(first_aged, 0, True), ([1.25, 1.01, -4], 1, True)]
        else:
            assert tick == 7
            targets = [([-2.75, 1.04, -4], 0, True), ([1.25, 1.01, -4], 1, False)]
        witnesses = []
        for point, channel, visible in targets:
            x, y = map(math.floor, projection(report['camera_world'], CAPTURE_LENS, point))
            color = image[y][x]
            assert tagged(color, channel) is visible, (point, color, channel, visible)
            witnesses.append(dict(pixel=[x,y], color=color, visible=visible))
        data = dict(gpu=report['gpu'], tick=tick, sha256=file_hash(path), witnesses=witnesses)
        self.measurements.setdefault('captures', {})[label] = data
        return data

    def continuation(self, client, state):
        second = self.advance(client, state, use=True)
        named = self.advance(client, second, named=True)
        final = self.advance(client, named, ticks=3)
        for result in (second, named, final):
            self.check_scene(result)
        return (second, named, final)

    def exercise(self, saves, game):
        with self.owner('original press', game=game, saves=saves) as (client, warm):
            session = self.start(client, saves, game)
            birth = self.advance(client, self.state(client, session, 0), use=True, named=True, retry=True)
            self.check_scene(birth)
            checkpoint = self.advance(client, birth)
            self.check_scene(checkpoint)
            saved = self.archive(client, saves, checkpoint, 'checkpoint')
            if self.args.gpu is not None:
                checkpoint_capture = self.capture(client, checkpoint, 'checkpoint-warm')
            expected = self.continuation(client, checkpoint)
            expected_archive = self.archive(client, saves, expected[-1], 'final')
            if self.args.gpu is not None:
                final_capture = self.capture(client, expected[-1], 'final-warm')

        with self.owner('cold press', game=game, saves=saves) as (client, cold):
            previous = self.start(client, saves, game)
            session = uuid4().hex
            restored = client.call('save.load', dict(request_id=uuid4().hex, configuration_generation=1,
                slot='checkpoint', revision=self.revision, expected_generation=1, expected_session_id=previous,
                expected_tick=0, expected_gameplay_revision=1, expected_structure_revision=0, new_session_id=session))
            assert restored['session_id'] == session and restored['tick'] == 2
            assert restored['source_stale'] is False and restored['structure_revision'] == 1

            def rebind(state):
                return dict(state, particles=dict(state['particles'], session_id=session))

            actual = self.state(client, session, 2)
            assert exact(actual, rebind(checkpoint))
            assert self.archive(client, saves, actual, 'restored-checkpoint') == saved
            if self.args.gpu is not None:
                assert exact(self.capture(client, actual, 'checkpoint-cold'), checkpoint_capture)
            continuation = self.continuation(client, actual)
            assert exact(continuation, tuple(map(rebind, expected)))
            assert self.archive(client, saves, continuation[-1], 'restored-final') == expected_archive
            if self.args.gpu is not None:
                assert exact(self.capture(client, continuation[-1], 'final-cold'), final_capture)
        assert warm['pid'] != cold['pid']
        self.measurements['continuation'] = dict(birth=birth, checkpoint=checkpoint, future=expected, exact=True)

    def refusals(self, saves):
        cases = ((2, 1, 'Particle burst exceeds the per-emitter tick budget.'),
                 (4, 1, 'Gameplay exceeded 32 particle burst requests in one tick.'),
                 (5, 1, 'Particle burst requires a live or staged ParticleEmitter.'),
                 (6, 1, 'Particle burst target did not survive this tick.'),
                 (7, 1, 'Particle burst count must be 1..1024.'),
                 (8, 1, 'Particle burst count must be 1..1024.'),
                 (0, 2, 'Particle fixture later-tick failure.'))
        failures = self.measurements['refusals'] = []
        with self.owner('rejected press', saves=saves) as (client, _):
            session = self.start(client, saves)
            for mode, ticks, message in cases:
                current = self.gameplay(client, session, 0)
                edited = client.call('runtime.gameplay.edit', dict(session_id=session, request_id=uuid4().hex,
                    expected_tick=0, expected_revision=current['revision'], values=dict(Mode=mode, FailAt='1' if ticks > 1 else '-1')))
                assert edited['revision'] == current['revision']+1, edited
                before = self.state(client, session, 0)
                prefix = f'reject-{mode}'
                raw = self.archive(client, saves, before, prefix+'-before')
                try:
                    self.advance(client, before, ticks=ticks, use=True, named=True)
                except RpcError as error:
                    assert error.code == -32040 and message in error.message, error
                    failures.append(dict(mode=mode, ticks=ticks, native_code=error.code, native_message=error.message))
                else:
                    raise AssertionError(f'Particle request mode {mode} did not fail')
                after = self.state(client, session, 0)
                assert exact(before, after), (before, after)
                assert self.archive(client, saves, after, prefix+'-after') == raw
            client.call('runtime.gameplay.edit', dict(session_id=session, request_id=uuid4().hex,
                expected_tick=0, expected_revision=after['revision'], values=dict(Mode=0, FailAt='-1')))
            recovered = self.advance(client, self.state(client, session, 0), use=True, named=True)
            self.check_scene(recovered)
            self.measurements['after_refusals'] = recovered

    def limits(self, saves):
        rows = self.measurements['accepted_bounds'] = []
        for mode, total in ((1, 1024), (3, 34)):
            with self.owner(f'bounded press {total}', saves=saves) as (client, _):
                session = self.start(client, saves, Mode=mode)
                before = self.state(client, session, 0)
                after = self.advance(client, before, use=True, named=True)
                assert exact(after['particles']['counts'], counters(5, 5, total-5)), after
                assert after['values']['Uses'] == 1 and after['values']['Ticks'] == 1, after
                rows.append(dict(mode=mode, total=total, state=after))

    def run(self):
        self.revision = self.measurements['authored_revision'] = self.author()
        saves = self.root/'External save slots'
        saves.mkdir()
        if not self.args.runtime_root:
            source = tree_hashes(self.project)
            self.exercise(saves, None)
            self.refusals(saves)
            self.limits(saves)
            assert tree_hashes(self.project) == source
            return

        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 536, info
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Press Game', self.root/'Unavailable source'
        output.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        game = bundle/'game.json'
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=frozen, detached_source=authored)
        assert self.image_hash in frozen.values()
        self.exercise(saves, game)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor', 'runtime-root'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, help='Observe standalone Windows output on this GPU.')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run the fixture without -O.')
    managed = (args.hostfxr, args.bridge, args.assembly)
    if (args.native_descriptor and any(managed)) or (not args.native_descriptor and not all(managed)):
        parser.error('Supply one Native AOT descriptor or the CoreCLR hostfxr, bridge and assembly.')
    if args.runtime_root and not args.native_descriptor:
        parser.error('Standalone export requires --native-descriptor.')
    if args.gpu is not None and (not args.runtime_root or not 0 <= args.gpu <= 4095):
        parser.error('--gpu requires a standalone runtime and a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='gameplay-particle-burst-', dir=args.output.resolve()))
    test = GameplayBurstContract(args, root)
    result = dict(passed=False, backend='native_aot' if args.native_descriptor else 'coreclr',
                  standalone=bool(args.runtime_root), executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
