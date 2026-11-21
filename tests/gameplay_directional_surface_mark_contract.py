#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check travel-aligned stamps through both gameplay hosts, cold saves and Vulkan."""
import argparse
from dataclasses import dataclass
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
    BASE, CAMERA, CAMERA_CENTER, FRONT, LENS, MOVER, MOVED, NEIGHBOR, NEIGHBOR_CENTER,
    ROUGHNESS, STAMPED, START, SurfaceMarksCapture, radiance, ray, world_position,
)
from world_particles_contract import PLAYER, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid
from poima_client import RpcError


TYPE = 'Poima.Tests.ManagedDirectionalSurfaceMarkGame'
IDENTITY = 'poima.test.managed-directional-surface-mark'
FEATURES = {'baseline_v7', 'surface_mark_v1', 'surface_mark_direction_v2'}
TEMPLATE = uid(110)
QUARTER_TURN_Z = [0, 0, math.sqrt(.5), math.sqrt(.5)]
HALF_TURN_Z = [0, 0, 1, 0]
SOURCE_STOP = [-1.0625, 1.25, -1]
SOURCE_AXES = [0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0]
CHECKPOINT_TICK, FINAL_TICK = 1, 7


@dataclass(frozen=True)
class ExpectedMark:
    label: str
    receiver: str
    center: tuple[float, float]
    tangent: tuple[float, float]
    color: tuple[float, float, float]
    opacity: float
    roughness: float
    major: float = .25
    minor: float = .0625
    edge: float = .25

    def fields(self):
        return dict(receiver=self.receiver, half_extent_m=[self.major, self.minor], half_depth_m=.125,
                    minimum_normal_dot=.8, edge_fraction=self.edge, linear_rgb=list(self.color),
                    opacity=self.opacity, roughness=self.roughness)


# The checkpoint receiver has a quarter turn around Z. World +X projects to local -Y.
# The compiled source's quarter turn around Y maps (2,3,4) to (4,3,-2).
# Projection removes Z, then the receiver-local major axis becomes (3/5,-4/5).
MARKS = (
    ExpectedMark('caller', MOVER, (-.25, .1875), (0, -1), (.875, .125, .125), .875, .375),
    ExpectedMark('near-normal', NEIGHBOR, (-.25, .25), (1, 0), (.125, .125, .875), .875, .625),
    ExpectedMark('compiled', MOVER, (.25, -.1875), (.6, -.8), (.125, .875, .125), .8125, .5),
    ExpectedMark('legacy', NEIGHBOR, (.25, -.25), (1, 0), (.875, .625, .125), 1, .375, major=.1875),
)


def world_delta(receiver, tick, local):
    x, y = local
    if receiver == NEIGHBOR:
        return x, y
    return (-y, x) if tick == CHECKPOINT_TICK else (-x, -y)


def receiver_center(receiver, tick):
    if receiver == NEIGHBOR:
        return NEIGHBOR_CENTER
    return STAMPED if tick == CHECKPOINT_TICK else MOVED


def world_point(receiver, tick, local):
    x, y = world_delta(receiver, tick, local)
    center = receiver_center(receiver, tick)
    return center[0]+x, center[1]+y, FRONT


def expected_material(point, receiver, tick):
    base, roughness = BASE, ROUGHNESS
    for stamp in MARKS:
        if stamp.receiver != receiver:
            continue
        cx, cy, _ = world_point(receiver, tick, stamp.center)
        tx, ty = world_delta(receiver, tick, stamp.tangent)
        dx, dy = point[0]-cx, point[1]-cy
        radius = math.hypot((dx*tx+dy*ty)/stamp.major, (-dx*ty+dy*tx)/stamp.minor)
        ramp = min(1, max(0, (radius-1+stamp.edge)/stamp.edge))
        amount = stamp.opacity*(1-ramp*ramp*(3-2*ramp))
        base = tuple(a+(b-a)*amount for a, b in zip(base, stamp.color, strict=True))
        roughness += (stamp.roughness-roughness)*amount
    return base, roughness


def caller_marks():
    return [stamp.fields() | dict(ray=ray(*world_point(stamp.receiver, CHECKPOINT_TICK, stamp.center)[:2]),
                                 travel_direction=direction)
            for stamp, direction in zip(MARKS[:2], ([2, 0, 3], [0, 2e-7, 1]), strict=True)]


class DirectionalSurfaceMarkContract(SurfaceMarksCapture):
    def configure_gameplay(self):
        if not self.args.native_descriptor:
            paths = (self.args.hostfxr, self.args.bridge, self.args.assembly,
                     self.args.bridge.parent/'Poima.Gameplay.dll', self.args.assembly.parent/'Poima.Gameplay.dll')
            self.measurements['gameplay_artifact'] = {str(path): file_hash(path) for path in paths}
            self.image_hash = file_hash(self.args.assembly)
            return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge),
                        assembly=self.native(self.args.assembly), type=TYPE)
        descriptor = self.args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert (metadata['type'], metadata['identity']) == (TYPE, IDENTITY), metadata
        assert (metadata['call_version'], metadata['call_bytes'], metadata['services_version'],
                metadata['minimum_services_bytes']) == (1, 80, 7, 552), metadata
        assert set(metadata['required_features']) == FEATURES and len(metadata['required_features']) == len(FEATURES)
        digest = file_hash(descriptor)
        self.measurements['gameplay_artifact'] = dict(descriptor_sha256=digest, files=metadata['files'])
        frozen = self.project/'gameplay'
        for record in metadata['files']:
            path = (descriptor.parent/record['path']).resolve(strict=True)
            assert path.is_relative_to(descriptor.parent) and file_hash(path) == record['sha256'], record
            assert path.stat().st_size == record['size'], record
            if self.args.runtime_root:
                target = frozen/record['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        self.image_hash = file_hash(descriptor.parent/metadata['library'])
        if self.args.runtime_root:
            shutil.copyfile(descriptor, frozen/'native-gameplay.json')
            assert file_hash(frozen/'native-gameplay.json') == digest
            manifest = json.loads(self.manifest.read_bytes())
            manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
            self.manifest.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        return dict(descriptor=self.native(descriptor), expected_descriptor_sha256=digest)

    def author(self):
        revision = super().author()
        source_body = dict(motion='kinematic', half_extents=[.08]*3, mass=1, friction=.5, restitution=0)
        ops = [component(MOVER, 'Transform', transform(START, QUARTER_TURN_Z)),
               dict(op='template.set', id=TEMPLATE, name='Moving contact source',
                    components=dict(Transform=transform([0, 0, 0]), BoxCollider=source_body))]
        transaction = self.world_call('world.transact', request_id=uuid4().hex, base_revision=revision, ops=ops)
        assert transaction['committed'] and transaction['revision'] == revision+1, transaction
        self.config = self.configure_gameplay()
        return transaction['revision']

    def start(self, client, saves, game):
        session = self.initialize(client, self.revision, uuid4().hex, saves, game)
        if not game:
            method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
            loaded = client.call(method, dict(session_id=session, request_id=uuid4().hex, expected_tick=0,
                                               expected_revision=0, values={}, **self.config))
            assert loaded['revision'] == 1 and loaded['replayed'] is False, loaded
        state = self.state(client, session, 0)
        assert state['structure'] == 0 and state['marks'] == [] and state['source_pose'] is None, state
        return state

    def pose(self, client, session, tick, entity):
        return client.call('runtime.entity', dict(session_id=session, tick=tick, id=entity))['world_matrix']

    def state(self, client, session, tick):
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity']) == (backend, TYPE, IDENTITY), module
        assert module['assembly_sha256'] == self.image_hash, module
        if self.args.native_descriptor:
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
        assert (info['session_id'], info['tick']) == (session, tick), info
        structure, values = info['structure_revision'], module['values']
        source = values['TemporarySource']
        return dict(session=session, tick=tick, revision=info['revision'], structure=structure, values=values,
                    source_pose=None if source == uid(0) else self.pose(client, session, tick, source),
                    receiver_pose=self.pose(client, session, tick, MOVER),
                    marks=self.inspect(client, session, tick, structure=structure))

    def request(self, state, *, ticks=1, use=False, stamps=(), motion=None):
        params = dict(session_id=state['session'], expected_tick=state['tick'], request_id=uuid4().hex, ticks=ticks,
                      expected_structure_revision=state['structure'], inputs=[dict(entity=PLAYER, use=use)],
                      surface_marks=list(stamps))
        if motion is not None:
            position, rotation, duration = motion
            params['motions'] = [dict(entity=MOVER, position=position, rotation=rotation, duration_ticks=duration)]
        return params

    def step(self, client, state, *, retry=False, **commands):
        params = self.request(state, **commands)
        result = client.call('runtime.step', params)
        session, tick = state['session'], state['tick']+params['ticks']
        assert (result['current_session_id'], result['current_tick']) == (session, tick), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        after = self.state(client, session, tick)
        if retry:
            assert exact(client.call('runtime.step', params), result | dict(replayed=True))
            assert exact(self.state(client, session, tick), after)
        return after

    def archive(self, client, saves, state, slot):
        receipt = client.call('save.write', dict(request_id=uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=state['session'], expected_tick=state['tick'],
            expected_gameplay_revision=state['revision'], expected_structure_revision=state['structure']))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['tick'] == state['tick']
        assert exact(payload['surface_marks'], dict(version=1, next_id=5,
            marks=[row | dict(id=int(row['id'])) for row in state['marks']]))
        self.measurements.setdefault('archives', {})[slot] = dict(tick=state['tick'], bytes=len(raw),
                                                                  sha256=receipt['sha256'])
        return raw

    def check_state(self, state):
        tick = state['tick']
        assert tick in (CHECKPOINT_TICK, FINAL_TICK), state
        assert state['values']['Uses'] == (1 if tick == CHECKPOINT_TICK else 2), state
        assert state['values']['Ticks'] == tick, state
        pose = state['receiver_pose']
        near_vector(pose[12:15], receiver_center(MOVER, tick))
        x = world_delta(MOVER, tick, (1, 0))
        y = world_delta(MOVER, tick, (0, 1))
        near_vector(pose[:12], [*x, 0, 0, *y, 0, 0, 0, 0, 1, 0])
        if tick == CHECKPOINT_TICK:
            assert state['structure'] == 1 and state['values']['TemporarySource'] not in (uid(0), MOVER, NEIGHBOR)
            near_vector(state['source_pose'][:12], SOURCE_AXES)
            near_vector(state['source_pose'][12:15], SOURCE_STOP)
        else:
            assert state['structure'] == 2 and state['values']['TemporarySource'] == uid(0)
            assert state['source_pose'] is None
        for index, (row, expected) in enumerate(zip(state['marks'], MARKS, strict=True), 1):
            assert (row['id'], row['tick'], row['receiver']) == (str(index), CHECKPOINT_TICK, expected.receiver)
            near_vector(row['center_m'], [*expected.center, .5])
            near_vector(row['normal'], [0, 0, 1])
            near_vector(row['tangent'], [*expected.tangent, 0])
            for field, value in expected.fields().items():
                if field == 'receiver':
                    continue
                if isinstance(value, list):
                    near_vector(row[field], value)
                else:
                    assert math.isclose(row[field], value, rel_tol=0, abs_tol=1e-7), (field, row)

    def continue_scene(self, client, checkpoint):
        motion = (MOVED, HALF_TURN_Z, 6)
        moving = self.step(client, checkpoint, motion=motion)
        detached = self.step(client, moving, use=True)
        assert detached['marks'] == checkpoint['marks'] and detached['structure'] == 2, detached
        source = checkpoint['values']['TemporarySource']
        with_source = dict(session_id=detached['session'], tick=detached['tick'], id=source)
        self.reject(client, 'runtime.entity', with_source, -32004)
        final = self.step(client, detached, ticks=4)
        assert exact(checkpoint['marks'], final['marks']), final
        self.check_state(final)
        return final

    def refusals(self, client, saves, checkpoint, raw):
        valid, fallback = caller_marks()
        cases = (
            ('zero-direction', [valid | dict(travel_direction=[0, 0, 0])], -32040),
            ('out-of-range', [valid | dict(travel_direction=[0, 0, 1000001])], -32602),
            ('miss-after-valid', [valid, fallback | dict(ray=ray(8, 1))], -32040),
        )
        failures = self.measurements['refusals'] = {}
        for label, stamps, code in cases:
            params = self.request(checkpoint, ticks=2, stamps=stamps, motion=(MOVED, QUARTER_TURN_Z, 2))
            failures[label] = self.reject(client, 'runtime.step', params, code)
            after = self.state(client, checkpoint['session'], checkpoint['tick'])
            assert exact(after, checkpoint), after
            assert self.archive(client, saves, after, label) == raw
        assert 'length at least 1e-8' in failures['zero-direction']['message']
        assert 'must first hit the expected receiver' in failures['miss-after-valid']['message']

    def capture(self, client, state, label):
        tick = state['tick']
        camera = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *CAMERA_CENTER, 1]
        coordinates = {}
        for stamp in MARKS:
            cx, cy = stamp.center
            tx, ty = stamp.tangent
            points = dict(center=(cx, cy), major=(cx+.6*stamp.major*tx, cy+.6*stamp.major*ty),
                          minor_outside=(cx-.125*ty, cy+.125*tx),
                          edge=(cx+.87*stamp.major*tx, cy+.87*stamp.major*ty))
            for name, local in points.items():
                point = world_point(stamp.receiver, tick, local)
                coordinates[f'{stamp.label}_{name}'] = (stamp.receiver, *map(math.floor, projection(camera, LENS, point)))
        path = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=state['session'], tick=tick,
            structure_revision=state['structure'], camera=CAMERA, path=self.native(path), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path, reconstruction='none',
            capture_frames=2, scene_product_probes=[dict(x=x, y=y) for _, x, y in coordinates.values()]))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0, report
        assert report['nvrhi_errors'] == 0 and report['tick'] == tick and report['lens'] == LENS, report
        assert report['revision'] == self.revision and report['camera_world'] == camera, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        image = pixels(path)
        record = dict(tick=tick, gpu=report['gpu'], lighting_path=self.args.lighting_path,
                      sha256=file_hash(path), probes={})
        self.measurements.setdefault('captures', {})[label] = record
        for (name, (receiver, x, y)), probe in zip(coordinates.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            assert probe['surface_valid'] and abs(probe['depth']-(1-1/4)*10/9) < 2e-6, probe
            near_vector(probe['shading_normal'][:3], [0, 0, 1])
            point = world_position(x, y)
            base, roughness = expected_material(point, receiver, tick)
            expected = radiance(point, base, roughness)
            bmp = [display(channel) for channel in expected]
            item = dict(hdr=probe['resolved_hdr'], expected_hdr=expected, bmp_rgb=image[y][x], expected_bmp=bmp,
                        pixel=[x, y], depth=probe['depth'], base=base, roughness=roughness)
            record['probes'][name] = item
            assert probe['raw_hdr'] == probe['resolved_hdr'], item
            near_vector(probe['resolved_hdr'], expected, tolerance=.002)
            assert max(abs(a-b) for a, b in zip(image[y][x], bmp, strict=True)) <= 2, item
            if name.endswith('_minor_outside'):
                assert base == BASE and roughness == ROUGHNESS, item
            elif name.endswith('_major'):
                assert max(abs(a-b) for a, b in zip(base, BASE, strict=True)) > .15, item
        return record

    def exercise(self, saves, game=None):
        with self.owner('directional marks warm', game=game, saves=saves) as (client, warm):
            initial = self.start(client, saves, game)
            checkpoint = self.step(client, initial, use=True, stamps=caller_marks(),
                                   motion=(STAMPED, QUARTER_TURN_Z, 1), retry=True)
            self.check_state(checkpoint)
            saved = self.archive(client, saves, checkpoint, 'checkpoint')
            if not game:
                self.refusals(client, saves, checkpoint, saved)
            if self.args.gpu is not None:
                checkpoint_frame = self.capture(client, checkpoint, 'checkpoint-warm')
            final = self.continue_scene(client, checkpoint)
            final_bytes = self.archive(client, saves, final, 'final')
            if self.args.gpu is not None:
                final_frame = self.capture(client, final, 'final-warm')
                assert final_frame['sha256'] != checkpoint_frame['sha256']

        with self.owner('directional marks cold', game=game, saves=saves) as (client, cold):
            fresh, session = self.start(client, saves, game), uuid4().hex
            restored = client.call('save.load', dict(request_id=uuid4().hex, configuration_generation=1,
                slot='checkpoint', revision=self.revision, expected_generation=1, expected_session_id=fresh['session'],
                expected_tick=0, expected_gameplay_revision=fresh['revision'], expected_structure_revision=0,
                new_session_id=session))
            assert (restored['session_id'], restored['tick'], restored['structure_revision']) == (session, 1, 1)
            assert restored['source_stale'] is False, restored
            actual = self.state(client, session, CHECKPOINT_TICK)
            assert exact(actual, checkpoint | dict(session=session))
            assert self.archive(client, saves, actual, 'restored-checkpoint') == saved
            if self.args.gpu is not None:
                assert exact(self.capture(client, actual, 'checkpoint-cold'), checkpoint_frame)
            continued = self.continue_scene(client, actual)
            assert exact(continued, final | dict(session=session))
            assert self.archive(client, saves, continued, 'restored-final') == final_bytes
            if self.args.gpu is not None:
                assert exact(self.capture(client, continued, 'final-cold'), final_frame)
        assert warm['pid'] != cold['pid']
        self.measurements['continuation'] = dict(checkpoint=checkpoint, final=final, exact=True)

    def run(self):
        self.revision = self.measurements['authored_revision'] = self.author()
        saves = self.root/'External save slots'
        saves.mkdir()
        if not self.args.runtime_root:
            source = tree_hashes(self.project)
            self.exercise(saves)
            assert tree_hashes(self.project) == source
            return
        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 552, info
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
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
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
    root = Path(tempfile.mkdtemp(prefix='gameplay-directional-surface-mark-', dir=args.output.resolve()))
    test = DirectionalSurfaceMarkContract(args, root)
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
