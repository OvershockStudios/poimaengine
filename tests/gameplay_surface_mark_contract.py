#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise compiled surface marks through motion, ordered placement, rollback and cold saves."""
import argparse
import json
import math
from pathlib import Path
import shutil
import tempfile
import traceback
from uuid import uuid4

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from surface_marks_capture import (
    CAMERA, CAMERA_CENTER, DRAWABLE, EXTRA_OFFSETS, FIRST_OFFSET, FRONT, LENS, MATERIAL, MOVED,
    MOVER, NEIGHBOR, NEIGHBOR_CENTER, STAMPED, START, SurfaceMarksCapture, first_marks, mark, motion,
    next_marks, radiance, surface, world_position,
)
from world_particles_contract import PLAYER, component, counters, fixed, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid
from poima_client import RpcError


TYPE = 'Poima.Tests.ManagedSurfaceMarkGame'
IDENTITY = 'poima.test.managed-surface-mark'
REQUIRED_FEATURES = ['baseline_v7', 'particle_burst_v1', 'surface_mark_v1']
SOURCE, TEMPLATE = uid(120), uid(110)
EXTRA_RECEIVERS = {uid(130): [6, 1, -4.5], uid(131): [8, 1, -4.5], uid(132): [10, 1, -4.5]}
SOURCE_START = [-1.25, -.25, -1]
SOURCE_STOP = [-1.25, 1, -1]
SOURCE_ROTATION = [0, math.sqrt(.5), 0, math.sqrt(.5)]
TEMPORARY_START = [1, .75, -1]
TEMPORARY_STOP = [.25, .75, -1]
SOURCE_BODY = dict(motion='kinematic', half_extents=[.08]*3, mass=1, friction=.5, restitution=0)


def caller_mark():
    # A quarter turn around Y maps local +X to world -Z and local +Z to world +X.
    return first_marks()[0] | dict(source=SOURCE, ray=dict(origin=[.5, 0, .3125],
                                                           direction=[1, 0, 0], distance=8))


def neighbor_mark():
    return mark(NEIGHBOR_CENTER, (.25, -.25), .09375, (.125, .125, .875), 1, .5) | dict(receiver=NEIGHBOR)


def placements_at(tick):
    initial = first_marks()
    if tick == 5:
        dx = MOVED[0]-STAMPED[0]
        initial = [request | dict(ray=request['ray'] | dict(origin=[request['ray']['origin'][0]+dx, 1, 0]))
                   for request in initial]
    return initial + [neighbor_mark()] + (next_marks() if tick == 5 else [])


class CompiledSurfaceMarkContract(SurfaceMarksCapture):
    def configure_gameplay(self):
        descriptor = self.args.native_descriptor
        if descriptor:
            descriptor = descriptor.resolve(strict=True)
            metadata = json.loads(descriptor.read_bytes())
            assert (metadata['type'], metadata['identity']) == (TYPE, IDENTITY)
            assert (metadata['call_version'], metadata['call_bytes'], metadata['services_version'],
                    metadata['minimum_services_bytes']) == (1, 80, 7, 544), metadata
            assert metadata['required_features'] == REQUIRED_FEATURES, metadata
            files = {}
            for record in metadata['files']:
                path = (descriptor.parent/record['path']).resolve(strict=True)
                assert path.is_relative_to(descriptor.parent) and file_hash(path) == record['sha256'], record
                assert path.stat().st_size == record['size'], record
                files[record['path']] = path
            self.image_hash = file_hash(files[metadata['library']])
            digest = file_hash(descriptor)
            self.measurements['gameplay_artifact'] = dict(descriptor_sha256=digest, files=metadata['files'])
            if self.args.runtime_root:
                destination = self.project/'gameplay'
                destination.mkdir()
                shutil.copyfile(descriptor, destination/'native-gameplay.json')
                for name, source in files.items():
                    target = destination/name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
                assert file_hash(destination/'native-gameplay.json') == digest
                manifest = json.loads(self.manifest.read_bytes())
                manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
                self.manifest.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
            return dict(descriptor=self.native(descriptor), expected_descriptor_sha256=digest)

        self.image_hash = file_hash(self.args.assembly)
        files = (self.args.hostfxr, self.args.bridge, self.args.assembly,
                 self.args.bridge.parent/'Poima.Gameplay.dll', self.args.assembly.parent/'Poima.Gameplay.dll')
        self.measurements['gameplay_artifact'] = {str(path): file_hash(path) for path in files}
        return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge),
                    assembly=self.native(self.args.assembly), type=TYPE)

    def author(self):
        revision = super().author()
        emitter = dict(spawn_space='local', simulation_space='world', seed='000000000000005c',
                       lifetime_ticks=2, size_m=.1, linear_rgb=[1, .2, .1], live_limit=5,
                       offset_m=fixed([0, 2, 0]), velocity_m_s=fixed([0, 0, 0]), emitting=False)
        ops = [dict(op='entity.create', id=SOURCE, name='Rotated kinematic mark source'),
               component(SOURCE, 'Transform', transform(SOURCE_START, SOURCE_ROTATION)),
               component(SOURCE, 'BoxCollider', SOURCE_BODY),
               component(SOURCE, 'ParticleEmitter', emitter),
               dict(op='template.set', id=TEMPLATE, name='Temporary mark source',
                    components=dict(Transform=transform(TEMPORARY_START), BoxCollider=SOURCE_BODY))]
        for receiver, position in EXTRA_RECEIVERS.items():
            ops.extend([dict(op='entity.create', id=receiver, name='Budget witness receiver'),
                        component(receiver, 'Transform', transform(position)),
                        component(receiver, 'MeshRenderer', DRAWABLE), component(receiver, 'PbrMaterial', MATERIAL),
                        component(receiver, 'BoxCollider', SOURCE_BODY | dict(motion='static', half_extents=[.5]*3))])
        authored = self.world_call('world.transact', request_id=uuid4().hex, base_revision=revision, ops=ops)
        assert authored['committed'] and authored['revision'] == revision+1, authored
        self.config = self.configure_gameplay()
        return authored['revision']

    def start(self, client, saves, game=None, **values):
        session = self.initialize(client, self.revision, uuid4().hex, saves, game)
        if not game:
            method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
            loaded = client.call(method, dict(session_id=session, request_id=uuid4().hex, expected_tick=0,
                                               expected_revision=0, values=values, **self.config))
            assert loaded['revision'] == 1 and loaded['replayed'] is False, loaded
        start = self.state(client, session, 0)
        assert not start['marks'] and start['structure'] == 0
        assert start['particles']['counts'] == counters(0, 0), start
        return session

    def position(self, client, session, tick, entity):
        result = client.call('runtime.entity', dict(session_id=session, tick=tick, id=entity))
        return result['world_matrix'][12:15]

    def state(self, client, session, tick):
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity']) == (backend, TYPE, IDENTITY), module
        assert module['assembly_sha256'] == self.image_hash, module
        if self.args.native_descriptor:
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False), module
        assert (info['session_id'], info['tick']) == (session, tick), info
        structure, values = info['structure_revision'], module['values']
        temporary = values['TemporarySource']
        positions = {entity: self.position(client, session, tick, entity) for entity in (MOVER, SOURCE)}
        if temporary != uid(0):
            positions[temporary] = self.position(client, session, tick, temporary)
        return dict(session=session, tick=tick, revision=info['revision'], structure=structure, values=values,
                    positions=positions, marks=self.inspect(client, session, tick, structure=structure),
                    particles=self.observed(client, session, tick))

    def advance(self, client, state, *, ticks=1, use=False, receiver_position=None, stamps=(), retry=False):
        session, tick = state['session'], state['tick']
        request = dict(session_id=session, expected_tick=tick, request_id=uuid4().hex, ticks=ticks,
                       expected_structure_revision=state['structure'], inputs=[dict(entity=PLAYER, use=use)],
                       surface_marks=list(stamps))
        if receiver_position is not None:
            request['motions'] = [motion(receiver_position)]
        result = client.call('runtime.step', request)
        assert (result['current_session_id'], result['current_tick']) == (session, tick+ticks), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        after = self.state(client, session, tick+ticks)
        if retry:
            assert exact(client.call('runtime.step', request), result | dict(replayed=True)), result
            assert exact(self.state(client, session, tick+ticks), after)
        return after

    def archive(self, client, saves, state, slot):
        receipt = client.call('save.write', dict(request_id=uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=state['session'], expected_tick=state['tick'],
            expected_gameplay_revision=state['revision'], expected_structure_revision=state['structure']))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['tick'] == state['tick'] == payload['particles']['state']['status']['tick']
        assert payload['particles']['version'] == 1
        if state['marks']:
            extension = payload['surface_marks']
            assert extension['version'] == 1 and extension['next_id'] == len(state['marks'])+1
            assert exact(extension['marks'], [record | dict(id=int(record['id'])) for record in state['marks']])
        else:
            assert 'surface_marks' not in payload
        self.measurements.setdefault('archives', {})[slot] = dict(tick=state['tick'], bytes=len(raw), sha256=file_hash(path))
        return raw

    def check_success(self, state):
        tick, observed = state['tick'], state['marks']
        assert tick in (1, 5) and state['structure'] == 1, state
        values = state['values']
        temporary = values['TemporarySource']
        assert temporary not in (uid(0), SOURCE, MOVER, NEIGHBOR, TEMPLATE), values
        assert exact(values, dict(TemporarySource=temporary, Mode=0, Uses=1 if tick == 1 else 2,
                                  Ticks=tick, FailAt='-1')), values
        near_vector(state['positions'][SOURCE], SOURCE_STOP)
        near_vector(state['positions'][temporary], TEMPORARY_STOP)
        near_vector(state['positions'][MOVER], STAMPED if tick == 1 else MOVED)

        positions = [FIRST_OFFSET, FIRST_OFFSET, (.25, -.25), *EXTRA_OFFSETS]
        birth_ticks = (1, 1, 1, 3, 3)
        assert len(observed) == (3 if tick == 1 else 5), observed
        for index, (row, request) in enumerate(zip(observed, placements_at(tick), strict=True)):
            assert (row['id'], row['tick'], row['receiver']) == (str(index+1), birth_ticks[index], request['receiver'])
            near_vector(row['center_m'], [*positions[index], .5])
            near_vector(row['normal'], [0, 0, 1])
            near_vector(row['tangent'], [1, 0, 0])
            for name in ('half_extent_m', 'linear_rgb'):
                near_vector(row[name], request[name])
            for name in ('half_depth_m', 'minimum_normal_dot', 'edge_fraction', 'opacity', 'roughness'):
                assert math.isclose(row[name], request[name], rel_tol=0, abs_tol=1e-7), (name, row)

        particles = state['particles']
        expected_counts = counters(2, 2) if tick == 1 else counters(0, 3, expired=3)
        assert exact(particles['counts'], expected_counts), particles
        assert len(particles['sources']) == 1 and particles['sources'][0]['entity'] == SOURCE, particles
        for sample in particles['particles']:
            assert sample['age_ticks'] == 0 and sample['lifetime_ticks'] == 2, sample
            near_vector(sample['position_m'], [SOURCE_STOP[0], SOURCE_STOP[1]+2, SOURCE_STOP[2]])

    def capture(self, client, state, label):
        width, height = CAPTURE_SIZE
        path = self.root/(label+'.bmp')
        tick = state['tick']
        report = client.call('runtime.capture', dict(session_id=state['session'], tick=tick,
            structure_revision=state['structure'], camera=CAMERA, path=self.native(path), width=width, height=height,
            samples=1, gpu=self.args.gpu, reconstruction='none', capture_frames=2))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0
        assert report['nvrhi_errors'] == 0 and report['tick'] == tick and report['lens'] == LENS, report
        image = pixels(path)
        receiver = STAMPED if tick == 1 else MOVED
        targets = [('overlap', MOVER, (receiver[0]+FIRST_OFFSET[0], 1, FRONT)),
                   ('spawned-source', NEIGHBOR, (.25, .75, FRONT)),
                   ('unmarked', NEIGHBOR, (-.375, 1.25, FRONT))]
        if tick == 5:
            targets.append(('world-ray', MOVER, (-2.6875, 1.25, FRONT)))
        marks = placements_at(tick)
        witnesses = {}
        for label_name, entity, point in targets:
            x, y = map(math.floor, projection(report['camera_world'], LENS, point))
            pixel_position = world_position(x, y)
            material = surface(pixel_position, [row for row in marks if row['receiver'] == entity])
            expected = [display(channel) for channel in radiance(pixel_position, *material)]
            observed = image[y][x]
            assert max(abs(a-b) for a, b in zip(observed, expected, strict=True)) <= 2, (label_name, observed, expected)
            witnesses[label_name] = dict(pixel=[x, y], rgb=observed, expected=expected)
        data = dict(gpu=report['gpu'], tick=tick, sha256=file_hash(path), witnesses=witnesses)
        self.measurements.setdefault('captures', {})[label] = data
        return data

    def continue_scene(self, client, checkpoint):
        idle = self.advance(client, checkpoint)
        moved = self.advance(client, idle, use=True, receiver_position=MOVED)
        final = self.advance(client, moved, ticks=2)
        self.check_success(final)
        return final

    def exercise(self, saves, game=None):
        with self.owner('original marks', game=game, saves=saves) as (client, warm):
            session = self.start(client, saves, game)
            initial = self.state(client, session, 0)
            near_vector(initial['positions'][SOURCE], SOURCE_START)
            old_ray = dict(origin=[SOURCE_START[0]+FIRST_OFFSET[0], SOURCE_START[1], -1.5], direction=[0, 0, -1],
                           distance=8)
            pre_move = client.call('runtime.raycast', dict(session_id=session, tick=0) | old_ray)
            assert pre_move['hit'] is None or pre_move['hit']['entity'] != MOVER, pre_move
            self.measurements['pre_move_ray'] = pre_move
            checkpoint = self.advance(client, initial, use=True, receiver_position=STAMPED,
                                      stamps=[caller_mark()], retry=True)
            self.check_success(checkpoint)
            saved = self.archive(client, saves, checkpoint, 'checkpoint')
            if self.args.gpu is not None:
                checkpoint_frame = self.capture(client, checkpoint, 'checkpoint-warm')
            final = self.continue_scene(client, checkpoint)
            final_bytes = self.archive(client, saves, final, 'final')
            if self.args.gpu is not None:
                final_frame = self.capture(client, final, 'final-warm')

        with self.owner('cold marks', game=game, saves=saves) as (client, cold):
            previous, session = self.start(client, saves, game), uuid4().hex
            restored = client.call('save.load', dict(request_id=uuid4().hex, configuration_generation=1,
                slot='checkpoint', revision=self.revision, expected_generation=1, expected_session_id=previous,
                expected_tick=0, expected_gameplay_revision=1, expected_structure_revision=0, new_session_id=session))
            assert (restored['session_id'], restored['tick'], restored['structure_revision']) == (session, 1, 1)
            assert restored['source_stale'] is False, restored

            def rebound(state):
                return state | dict(session=session, particles=state['particles'] | dict(session_id=session))

            actual = self.state(client, session, 1)
            assert exact(actual, rebound(checkpoint))
            assert self.archive(client, saves, actual, 'restored-checkpoint') == saved
            if self.args.gpu is not None:
                assert exact(self.capture(client, actual, 'checkpoint-cold'), checkpoint_frame)
            continued = self.continue_scene(client, actual)
            assert exact(continued, rebound(final))
            assert self.archive(client, saves, continued, 'restored-final') == final_bytes
            if self.args.gpu is not None:
                assert exact(self.capture(client, continued, 'final-cold'), final_frame)
        assert warm['pid'] != cold['pid']
        self.measurements['continuation'] = dict(checkpoint=checkpoint, final=final, exact=True)

    def caller_budget(self):
        positions = {MOVER: START, NEIGHBOR: NEIGHBOR_CENTER} | dict(list(EXTRA_RECEIVERS.items())[:2])
        return [mark(position, (0, 0), .125, (.25, .5, .75), 1, .5) | dict(receiver=entity)
                for entity, position in positions.items() for _ in range(4)]

    def refusals(self, saves):
        cases = (('combined-limit', dict(Mode=1), dict(use=True, stamps=self.caller_budget()),
                  'Surface mark command budget exceeded.'),
                 ('blocked', dict(Mode=2), dict(use=True, receiver_position=STAMPED),
                  'Surface mark ray must first hit the expected receiver with a surface normal.'),
                 ('later-tick', dict(FailAt='1'),
                  dict(ticks=2, use=True, receiver_position=STAMPED, stamps=[caller_mark()]),
                  'Surface mark fixture later-tick failure.'))
        failures = self.measurements['refusals'] = {}
        for label, values, commands, diagnostic in cases:
            with self.owner('rejected '+label, saves=saves) as (client, _):
                session = self.start(client, saves, **values)
                before = self.state(client, session, 0)
                raw = self.archive(client, saves, before, label+'-before')
                try:
                    self.advance(client, before, **commands)
                except RpcError as error:
                    assert error.code == -32040 and diagnostic in error.message, error
                    failures[label] = dict(code=error.code, diagnostic=error.message)
                else:
                    raise AssertionError(f'{label} unexpectedly committed')
                after = self.state(client, session, 0)
                assert exact(before, after), (before, after)
                assert self.archive(client, saves, after, label+'-after') == raw

    def qualify_budget(self, saves):
        caller = self.caller_budget()[:-1]
        with self.owner('accepted combined limit', saves=saves) as (client, _):
            session = self.start(client, saves, Mode=1)
            after = self.advance(client, self.state(client, session, 0), use=True, stamps=caller, retry=True)
            marks = after['marks']
            assert [row['id'] for row in marks] == [str(index+1) for index in range(16)], marks
            assert [row['receiver'] for row in marks] == [row['receiver'] for row in caller]+[uid(132)], marks
            assert all(row['tick'] == 1 for row in marks), marks
            assert after['particles']['counts'] == counters(1, 1), after
            self.measurements['accepted_budget'] = dict(caller=len(caller), gameplay=1, marks=marks)

    def run(self):
        self.revision = self.measurements['authored_revision'] = self.author()
        saves = self.root/'External save slots'
        saves.mkdir()
        if not self.args.runtime_root:
            source = tree_hashes(self.project)
            self.exercise(saves)
            self.refusals(saves)
            self.qualify_budget(saves)
            assert tree_hashes(self.project) == source
            return

        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 544, info
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Mark Game', self.root/'Unavailable source'
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
    parser.add_argument('--gpu', type=int, help='Capture the relocated game on this Windows GPU.')
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
    root = Path(tempfile.mkdtemp(prefix='gameplay-surface-mark-', dir=args.output.resolve()))
    test = CompiledSurfaceMarkContract(args, root)
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
