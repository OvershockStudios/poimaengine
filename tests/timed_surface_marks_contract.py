#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive timed marks to receiver and scene limits, then replay from a cold save."""
import argparse
from dataclasses import dataclass, field
import json
import math
from pathlib import Path
import shutil
import tempfile
import time
import traceback
from uuid import uuid4

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from surface_marks_capture import (
    BASE, CAMERA, CAMERA_CENTER, DRAWABLE, FRONT, LENS, LIMITS, MATERIAL, MOVER, NEIGHBOR,
    NEIGHBOR_CENTER, ROUGHNESS, START, SurfaceMarksCapture, TERRAIN, mark, normalized, radiance,
    ray, world_position,
)
from textured_surface_marks_contract import TexturedSurfaceMarkContract, profile_edit, sample_mask
from world_particles_contract import PLAYER, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes, uid


TYPE = 'Poima.Tests.ManagedTimedSurfaceMarkGame'
IDENTITY = 'poima.test.managed-timed-surface-mark'
FEATURES = {'baseline_v7', 'surface_mark_timed_v4'}
PROFILE = uid(132)
MASK = ((255, 255, 0, 0), (255, 255, 0, 0))
RECYCLE = 'recycle_oldest_timed'
LIFE_LIMIT = 216000
BULK_RECEIVERS = {uid(300+i): [20+2*(i%8), 1+2*(i//8), -4.5] for i in range(65)}
VISIBLE_OFFSETS = dict(left=(-.3125, 0), right=(.3125, 0), upper_left=(-.25, .25),
                       upper_right=(.25, .25), lower_left=(-.07, -.25), lower_right=(.07, -.25))


def placed(offset, color, lifetime=None, *, recycle=False, profile=None):
    stamp = mark(START, offset, .125, color, 1, .5, edge=.25)
    stamp['half_extent_m'] = [.125, .09375]
    if lifetime is not None:
        stamp['lifetime_ticks'] = lifetime
    if recycle:
        stamp['overflow'] = RECYCLE
    if profile is not None:
        stamp['profile'] = profile
    return stamp


LEFT = placed(VISIBLE_OFFSETS['left'], (.875, .125, .125))
RIGHT = placed(VISIBLE_OFFSETS['right'], (.875, .625, .125))
BLUE = placed(VISIBLE_OFFSETS['upper_left'], (.125, .125, .875), 9)
CYAN = placed(VISIBLE_OFFSETS['upper_right'], (.125, .875, .875), 4, recycle=True)
YELLOW = placed(VISIBLE_OFFSETS['upper_left'], (.875, .875, .125), 1)
RED = placed(VISIBLE_OFFSETS['upper_right'], (.875, .125, .125), 2, recycle=True)
GREEN = placed((0, -.25), (.125, .875, .125), 2, recycle=True, profile=PROFILE)
PURPLE = placed((0, -.25), (.625, .125, .875), 3, recycle=True)


def terrain_placement(x=10.25, *, timed=False):
    stamp = dict(receiver=TERRAIN, ray=dict(origin=[x, 8, -4.375], direction=[0, -1, 0], distance=12),
                 half_extent_m=[.25, .125], half_depth_m=.03125, minimum_normal_dot=.75,
                 edge_fraction=.5, linear_rgb=[.125, .875, .25], opacity=.75, roughness=.5)
    return stamp | dict(lifetime_ticks=8, overflow=RECYCLE) if timed else stamp


def bulk_placement(receiver, lifetime=None, *, recycle=False):
    stamp = mark(BULK_RECEIVERS[receiver], (0, 0), .09375, (.125, .625, .875), 1, .5) | dict(receiver=receiver)
    if lifetime is not None:
        stamp['lifetime_ticks'] = lifetime
    if recycle:
        stamp['overflow'] = RECYCLE
    return stamp


@dataclass
class MarkOracle:
    """An ordered ledger that advances in fixed ticks using fixture inputs only."""
    tick: int = 0
    uses: int = 0
    next_id: int = 1
    sustained: int = 0
    rows: list = field(default_factory=list)
    counts: dict = field(default_factory=lambda: dict(expired=0, recycled=0, dropped=0))

    def lifecycle(self):
        if any(self.counts.values()) or any(row['expires_tick'] is not None for row in self.rows):
            return self.counts
        return None

    def deposit(self, stamp):
        local = [row for row in self.rows if row['stamp']['receiver'] == stamp['receiver']]
        if len(local) == LIMITS['marks_per_receiver'] or len(self.rows) == LIMITS['marks_per_scene']:
            assert stamp.get('overflow') == RECYCLE, stamp
            candidates = local if len(local) == LIMITS['marks_per_receiver'] else self.rows
            oldest = next((row for row in candidates if row['expires_tick'] is not None), None)
            if oldest is None:
                self.counts['dropped'] += 1
                return
            self.rows.remove(oldest)
            self.counts['recycled'] += 1
        life = stamp.get('lifetime_ticks')
        self.rows.append(dict(id=self.next_id, born=self.tick, stamp=stamp,
                              expires_tick=self.tick+life if life is not None else None))
        self.next_id += 1

    def advance(self, ticks, stamps=(), use=False):
        for offset in range(ticks):
            self.tick += 1
            survivors = [item for item in self.rows if item['expires_tick'] is None or item['expires_tick'] > self.tick]
            self.counts['expired'] += len(self.rows)-len(survivors)
            self.rows = survivors
            if offset == 0:
                for stamp in stamps:
                    self.deposit(stamp)
            if self.sustained:
                slot = (self.tick-1) % 30
                if slot < 20:
                    self.deposit(terrain_placement(x=10.25+.09375*(slot%4), timed=True))
            elif offset == 0 and use:
                self.uses += 1
                if self.uses <= 2:
                    self.deposit(GREEN if self.uses == 1 else PURPLE)


class TimedSurfaceMarksContract(SurfaceMarksCapture):
    def configure_gameplay(self):
        if not self.args.native_descriptor:
            files = (self.args.hostfxr, self.args.bridge, self.args.assembly,
                     self.args.bridge.parent/'Poima.Gameplay.dll', self.args.assembly.parent/'Poima.Gameplay.dll')
            self.measurements['gameplay_artifact'] = {str(path): file_hash(path) for path in files}
            self.image_hash = file_hash(self.args.assembly)
            return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge),
                        assembly=self.native(self.args.assembly), type=TYPE)
        descriptor = self.args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert (metadata['type'], metadata['identity']) == (TYPE, IDENTITY), metadata
        assert (metadata['call_version'], metadata['call_bytes'], metadata['services_version'],
                metadata['minimum_services_bytes']) == (1, 80, 7, 568), metadata
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

    def edit(self, revision, ops):
        result = self.world_call('world.transact', request_id=uuid4().hex, base_revision=revision, ops=ops)
        assert result['committed'] and result['revision'] == revision+1, result
        return result['revision']

    def author(self):
        revision = super().author()
        self.mask_sha256 = TexturedSurfaceMarkContract.import_mask(self, 'timed-stamp', MASK)
        revision = self.edit(revision, [profile_edit(PROFILE, self.mask_sha256)])
        self.measurements['mask'] = dict(id=PROFILE, sha256=self.mask_sha256)
        if not self.args.runtime_root:
            revision, tile = self.author_terrain_and_template(revision)
            self.measurements['terrain'] = tile
            ops = []
            for index, (receiver, position) in enumerate(BULK_RECEIVERS.items()):
                ops.extend([
                    dict(op='entity.create', id=receiver, name=f'Capacity receiver {index}'),
                    component(receiver, 'Transform', transform(position)),
                    component(receiver, 'MeshRenderer', DRAWABLE),
                    component(receiver, 'PbrMaterial', MATERIAL),
                    component(receiver, 'BoxCollider', dict(motion='static', half_extents=[.5]*3,
                        mass=1, friction=.5, restitution=0)),
                ])
            for begin in range(0, len(ops), 200):
                revision = self.edit(revision, ops[begin:begin+200])
        self.config = self.configure_gameplay()
        return revision

    def discovery(self):
        discovery = self.world_call('world.describe.select', methods=['runtime.step'],
                                    sections=['surface_marks', 'gameplay_surface_marks'])
        lifecycle = discovery['sections']['surface_marks']['lifecycle']
        abi = discovery['sections']['gameplay_surface_marks']['timed']
        step = discovery['methods']['runtime.step']
        items = step['properties']['surface_marks']['items']
        lifetime = items['properties']['lifetime_ticks']
        overflow = items['properties']['overflow']
        assert lifetime == dict(type='integer', minimum=1, maximum=LIFE_LIMIT), lifetime
        assert overflow == dict(enum=['reject', RECYCLE], default='reject'), overflow
        assert items['allOf'][0]['then']['required'] == ['lifetime_ticks'], items
        assert lifecycle['max_lifetime_ticks'] == LIFE_LIMIT, lifecycle
        assert abi == dict(feature='surface_mark_timed_v4', services_bytes=568, command_bytes=184, command_version=4), abi
        self.measurements['discovery'] = dict(abi=abi, lifecycle=lifecycle, lifetime=lifetime, overflow=overflow)

    def inspect_marks(self, client, session, tick, structure, **scope):
        result = client.call('runtime.surface_marks.inspect',
                             dict(session_id=session, tick=tick, structure_revision=structure) | scope)
        assert (result['session_id'], result['tick'], result['structure_revision']) == (session, tick, structure)
        assert result['limits'] == LIMITS and isinstance(result['marks'], list), result
        return result

    def state(self, client, session, tick):
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        assert (info['session_id'], info['tick']) == (session, tick), info
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity']) == (backend, TYPE, IDENTITY)
        assert module['assembly_sha256'] == self.image_hash, module
        if self.args.native_descriptor:
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
        structure = info['structure_revision']
        observation = self.inspect_marks(client, session, tick, structure)
        return dict(session=session, tick=tick, revision=info['revision'], structure=structure, values=module['values'],
                    marks=observation['marks'], lifecycle=observation.get('lifecycle'))

    def start(self, client, saves, game, oracle):
        session = self.initialize(client, self.revision, uuid4().hex, saves, game)
        if not game:
            method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
            loaded = client.call(method, dict(session_id=session, request_id=uuid4().hex, expected_tick=0,
                                               expected_revision=0, values=dict(Sustained=oracle.sustained), **self.config))
            assert loaded['revision'] == 1 and loaded['replayed'] is False, loaded
        state = self.state(client, session, 0)
        self.check_model(state, oracle)
        return state

    def check_model(self, state, oracle):
        assert state['tick'] == oracle.tick and state['structure'] == 0, state
        assert exact(state['values'], dict(Ticks=oracle.tick, Uses=oracle.uses, Sustained=oracle.sustained)), state
        assert exact(state['lifecycle'], oracle.lifecycle()), (state, oracle.counts)
        assert len(state['marks']) == len(oracle.rows), (state['marks'], oracle.rows)
        for actual, predicted in zip(state['marks'], oracle.rows, strict=True):
            stamp = predicted['stamp']
            receiver = stamp['receiver']
            assert (actual['id'], actual['tick'], actual['receiver']) == (str(predicted['id']), predicted['born'], receiver)
            expires = predicted['expires_tick']
            assert ('expires_tick' in actual) == (expires is not None), actual
            if expires is not None:
                assert type(actual['expires_tick']) is int and actual['expires_tick'] == expires, actual
            assert actual.get('profile') == stamp.get('profile'), actual
            assert actual.get('mask_sha256') == (self.mask_sha256 if stamp.get('profile') else None), actual
            x, y, z = stamp['ray']['origin']
            if receiver == TERRAIN:
                center = [x, 1+.25*(x-10)+.5*(z+5), z]
                normal = normalized((-.25, 1, -.5))
                tangent = normalized([1-normal[0]**2, -normal[0]*normal[1], -normal[0]*normal[2]])
            else:
                receiver_position = START if receiver == MOVER else BULK_RECEIVERS[receiver]
                center = [x-receiver_position[0], y-receiver_position[1], .5]
                normal, tangent = (0, 0, 1), (1, 0, 0)
            near_vector(actual['center_m'], center)
            near_vector(actual['normal'], normal)
            near_vector(actual['tangent'], tangent)
            for key in ('half_extent_m', 'linear_rgb'):
                near_vector(actual[key], stamp[key])
            for key in ('half_depth_m', 'minimum_normal_dot', 'edge_fraction', 'opacity', 'roughness'):
                assert math.isclose(actual[key], stamp[key], rel_tol=0, abs_tol=1e-7), (key, actual)

    def request(self, state, *, ticks=1, use=False, stamps=()):
        return dict(session_id=state['session'], expected_tick=state['tick'], request_id=uuid4().hex,
                    ticks=ticks, expected_structure_revision=state['structure'], inputs=[dict(entity=PLAYER, use=use)],
                    surface_marks=list(stamps))

    def step(self, client, state, oracle, *, ticks=1, use=False, stamps=(), retry=False, observe=True):
        request = self.request(state, ticks=ticks, use=use, stamps=stamps)
        result = client.call('runtime.step', request)
        oracle.advance(ticks, stamps, use)
        session = state['session']
        assert (result['current_session_id'], result['current_tick']) == (session, oracle.tick), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        if retry:
            assert exact(client.call('runtime.step', request), result | dict(replayed=True))
        if not observe:
            return state | dict(tick=oracle.tick)
        after = self.state(client, session, oracle.tick)
        self.check_model(after, oracle)
        return after

    def archive(self, client, saves, state, slot, oracle):
        receipt = client.call('save.write', dict(request_id=uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=state['session'], expected_tick=state['tick'],
            expected_gameplay_revision=state['revision'], expected_structure_revision=state['structure']))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        fingerprint = dict(tick=state['tick'], bytes=len(raw), sha256=file_hash(path))
        assert receipt['sha256'] == fingerprint['sha256'] and receipt['bytes'] == fingerprint['bytes'], receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['tick'] == oracle.tick, payload
        version = 3 if oracle.lifecycle() is not None else 2 if any('profile' in row for row in state['marks']) else 1
        expected = dict(version=version, next_id=oracle.next_id,
                        marks=[row | dict(id=int(row['id'])) for row in state['marks']])
        if version == 3:
            expected['lifecycle'] = oracle.lifecycle()
        assert exact(payload['surface_marks'], expected), payload['surface_marks']
        self.measurements.setdefault('archives', {})[slot] = fingerprint
        return raw

    def restore(self, client, fresh, previous, slot, oracle):
        session = uuid4().hex
        receipt = client.call('save.load', dict(request_id=uuid4().hex, configuration_generation=1, slot=slot,
            revision=self.revision, expected_generation=1, expected_session_id=fresh['session'],
            expected_tick=fresh['tick'], expected_gameplay_revision=fresh['revision'],
            expected_structure_revision=fresh['structure'], new_session_id=session))
        assert (receipt['session_id'], receipt['tick'], receipt['structure_revision'], receipt['source_stale']) == (
            session, oracle.tick, 0, False), receipt
        state = self.state(client, session, oracle.tick)
        self.check_model(state, oracle)
        assert exact(state, previous | dict(session=session)), state
        return state

    def capture(self, client, state, label, oracle):
        camera = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *CAMERA_CENTER, 1]
        points = {name: (START[0]+u, START[1]+v, FRONT) for name, (u, v) in VISIBLE_OFFSETS.items()}
        points['neighbor'] = (-.375, NEIGHBOR_CENTER[1], FRONT)
        coords = [tuple(map(math.floor, projection(camera, LENS, point))) for point in points.values()]
        output = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=state['session'], tick=state['tick'],
            structure_revision=state['structure'], camera=CAMERA, path=self.native(output), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path, reconstruction='none',
            capture_frames=2, scene_product_probes=[dict(x=x, y=y) for x, y in coords]))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0, report
        assert report['nvrhi_errors'] == 0 and report['tick'] == state['tick'] and report['lens'] == LENS, report
        assert report['revision'] == self.revision and report['camera_world'] == camera, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        rendered = pixels(output)
        assert len(rendered) == height and all(len(row) == width for row in rendered)
        record = dict(tick=state['tick'], gpu=report['gpu'], lighting_path=self.args.lighting_path,
                      sha256=file_hash(output), probes={})
        self.measurements.setdefault('captures', {})[label] = record
        for (name, (x, y)), probe in zip(zip(points, coords, strict=True), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            assert probe['surface_valid'] and abs(probe['depth']-(1-1/4)*10/9) < 2e-6, probe
            near_vector(probe['shading_normal'][:3], [0, 0, 1])
            point = world_position(x, y)
            base, roughness = BASE, ROUGHNESS
            if name != 'neighbor':
                for row in oracle.rows:
                    stamp = row['stamp']
                    cx, cy, _ = stamp['ray']['origin']
                    sx = (point[0]-cx)/stamp['half_extent_m'][0]
                    sy = (point[1]-cy)/stamp['half_extent_m'][1]
                    radius = math.hypot(sx, sy)
                    if radius > 1:
                        continue
                    ramp = min(1, max(0, (radius-1+stamp['edge_fraction'])/stamp['edge_fraction']))
                    weight = stamp['opacity']*(1-ramp*ramp*(3-2*ramp))
                    if stamp.get('profile'):
                        weight *= sample_mask(MASK, sx, sy)
                    base = tuple(a+(b-a)*weight for a, b in zip(base, stamp['linear_rgb'], strict=True))
                    roughness += (stamp['roughness']-roughness)*weight
            expected = radiance(point, base, roughness)
            bmp = [display(channel) for channel in expected]
            item = dict(pixel=[x, y], raw_hdr=probe['raw_hdr'], resolved_hdr=probe['resolved_hdr'],
                        expected_hdr=expected, bmp_rgb=rendered[y][x], expected_bmp=bmp, base=base, roughness=roughness)
            record['probes'][name] = item
            assert probe['raw_hdr'] == probe['resolved_hdr'], item
            near_vector(probe['resolved_hdr'], expected, tolerance=.002)
            assert max(abs(a-b) for a, b in zip(rendered[y][x], bmp, strict=True)) <= 2, item
        stamped = {2: {'left', 'right', 'upper_right', 'lower_left'},
                   3: {'left', 'right', 'upper_left', 'upper_right'}, 9: {'left', 'right'}}
        assert {name for name, data in record['probes'].items() if data['base'] != BASE} == stamped[state['tick']]
        return record

    def refusal_checks(self, client, saves, checkpoint, checkpoint_bytes, oracle):
        bad_ray = CYAN | dict(ray=ray(4.5, 1))
        cases = (
            ('zero-lifetime', [CYAN | dict(lifetime_ticks=0)], -32602),
            ('fractional-lifetime', [CYAN | dict(lifetime_ticks=1.5)], -32602),
            ('large-lifetime', [CYAN | dict(lifetime_ticks=LIFE_LIMIT+1)], -32602),
            ('unknown-overflow', [CYAN | dict(overflow='overwrite-anything')], -32602),
            ('recycle-permanent', [LEFT | dict(overflow=RECYCLE)], -32602),
            ('expiry-then-ray-miss', [YELLOW, bad_ray], -32040),
        )
        refusals = self.measurements['refusals'] = {}
        for name, stamps, code in cases:
            request = self.request(checkpoint, ticks=2, stamps=stamps)
            refusals[name] = self.reject(client, 'runtime.step', request, code)
            assert exact(self.state(client, checkpoint['session'], checkpoint['tick']), checkpoint)
        assert 'must first hit' in refusals['expiry-then-ray-miss']['message'], refusals
        assert self.archive(client, saves, checkpoint, 'after-refusals', oracle) == checkpoint_bytes

    def continue_visible(self, client, saves, state, oracle, suffix):
        expiry = self.step(client, state, oracle, stamps=[YELLOW])
        assert [row['id'] for row in expiry['marks']] == ['1', '2', '5', '6']
        assert expiry['lifecycle'] == dict(expired=1, recycled=1, dropped=0)
        captures = {}
        if self.args.gpu is not None:
            captures['expiry'] = self.capture(client, expiry, 'expiry-'+suffix, oracle)
        state = self.step(client, expiry, oracle, ticks=2)
        replacement = self.step(client, state, oracle, use=True, stamps=[RED], retry=True)
        assert [row['id'] for row in replacement['marks']] == ['1', '2', '7', '8']
        final = self.step(client, replacement, oracle, ticks=3)
        assert final['tick'] == 9 and [row['id'] for row in final['marks']] == ['1', '2']
        assert final['lifecycle'] == dict(expired=5, recycled=1, dropped=0)
        if self.args.gpu is not None:
            captures['final'] = self.capture(client, final, 'final-'+suffix, oracle)
        slot = 'final' if suffix == 'warm' else 'restored-final'
        return dict(expiry=expiry, replacement=replacement, final=final), self.archive(client, saves, final, slot, oracle), captures

    def visible(self, saves, game=None):
        with self.owner('timed marks warm', game=game, saves=saves) as (client, warm):
            oracle = MarkOracle()
            state = self.start(client, saves, game, oracle)
            placed_initial = self.step(client, state, oracle, use=True, stamps=[LEFT, RIGHT, BLUE])
            assert [row['id'] for row in placed_initial['marks']] == ['1', '2', '3', '4']
            assert placed_initial['lifecycle'] == dict(expired=0, recycled=0, dropped=0)
            self.archive(client, saves, placed_initial, 'initial-timed', oracle)
            checkpoint = self.step(client, placed_initial, oracle, stamps=[CYAN], retry=True)
            assert [row['id'] for row in checkpoint['marks']] == ['1', '2', '4', '5']
            assert checkpoint['lifecycle'] == dict(expired=0, recycled=1, dropped=0)
            filtered = self.inspect_marks(client, checkpoint['session'], 2, 0, entity=MOVER)
            assert exact((filtered['marks'], filtered['lifecycle']), (checkpoint['marks'], checkpoint['lifecycle']))
            saved = self.archive(client, saves, checkpoint, 'checkpoint', oracle)
            if self.args.gpu is not None:
                checkpoint_frame = self.capture(client, checkpoint, 'checkpoint-warm', oracle)
            if not game:
                self.refusal_checks(client, saves, checkpoint, saved, oracle)
            states, final_bytes, captures = self.continue_visible(client, saves, checkpoint, oracle, 'warm')

        with self.owner('timed marks cold', game=game, saves=saves) as (client, cold):
            oracle = MarkOracle()
            fresh = self.start(client, saves, game, oracle)
            oracle.advance(1, [LEFT, RIGHT, BLUE], use=True)
            oracle.advance(1, [CYAN])
            restored = self.restore(client, fresh, checkpoint, 'checkpoint', oracle)
            assert self.archive(client, saves, restored, 'restored-checkpoint', oracle) == saved
            if self.args.gpu is not None:
                assert exact(self.capture(client, restored, 'checkpoint-cold', oracle), checkpoint_frame)
            continued, payload, restored_captures = self.continue_visible(client, saves, restored, oracle, 'cold')
            assert payload == final_bytes and exact(captures, restored_captures)
            assert exact(continued, {label: value | dict(session=restored['session']) for label, value in states.items()})
        assert cold['pid'] != warm['pid']
        self.measurements['continuation'] = dict(checkpoint=checkpoint, final=states['final'],
            boundary_states=dict(initial=placed_initial, expiry=states['expiry'], replacement=states['replacement']), exact=True)

    def sustained(self):
        saves = self.root/'Sustained terrain slots'
        saves.mkdir()
        checkpoints = []
        with self.owner('terrain pulse workload warm', saves=saves) as (client, warm):
            oracle = MarkOracle(sustained=1)
            state = self.start(client, saves, None, oracle)
            hit = client.call('runtime.raycast', dict(session_id=state['session'], tick=0) | terrain_placement()['ray'])['hit']
            assert (hit['entity'], hit['triangle']) == (TERRAIN, 1), hit
            near_vector(hit['position'], [10.25, 1.375, -4.375])
            start = time.perf_counter_ns()
            state = self.step(client, state, oracle, ticks=20, stamps=[terrain_placement()])
            assert state['lifecycle'] == dict(expired=0, recycled=17, dropped=0)
            assert [row['id'] for row in state['marks']] == ['1', '19', '20', '21']
            checkpoints.append(dict(tick=20, ids=[row['id'] for row in state['marks']], lifecycle=state['lifecycle']))
            state = self.step(client, state, oracle, ticks=10)
            assert state['lifecycle'] == dict(expired=3, recycled=17, dropped=0)
            assert [row['id'] for row in state['marks']] == ['1']
            checkpoints.append(dict(tick=30, ids=[row['id'] for row in state['marks']], lifecycle=state['lifecycle']))
            middle = self.step(client, state, oracle, ticks=420)
            assert middle['lifecycle'] == dict(expired=45, recycled=255, dropped=0)
            assert [row['id'] for row in middle['marks']] == ['1']
            checkpoints.append(dict(tick=450, ids=['1'], lifecycle=middle['lifecycle']))
            middle_bytes = self.archive(client, saves, middle, 'terrain-middle', oracle)
            final = self.step(client, middle, oracle, ticks=450)
            wall_ns = time.perf_counter_ns()-start
            final_bytes = self.archive(client, saves, final, 'terrain-final', oracle)
            assert final['tick'] == 900 and [row['id'] for row in final['marks']] == ['1']
            assert final['lifecycle'] == dict(expired=90, recycled=510, dropped=0), final
            assert oracle.next_id == 602

        with self.owner('terrain pulse workload cold', saves=saves) as (client, cold):
            oracle = MarkOracle(sustained=1)
            fresh = self.start(client, saves, None, oracle)
            oracle.advance(450, [terrain_placement()])
            restored = self.restore(client, fresh, middle, 'terrain-middle', oracle)
            assert self.archive(client, saves, restored, 'terrain-middle-cold', oracle) == middle_bytes
            cold_final = self.step(client, restored, oracle, ticks=450)
            assert exact(cold_final, final | dict(session=restored['session']))
            assert self.archive(client, saves, cold_final, 'terrain-final-cold', oracle) == final_bytes
        assert cold['pid'] != warm['pid']
        self.measurements['sustained'] = dict(ticks=900, attempts=600, retained_ids=['1'], next_id=602,
            lifecycle=final['lifecycle'], sampled_checkpoints=checkpoints, triangle_hit=hit,
            wall_interval_ns=wall_ns, interval_kind='harness wall time through batch advances and intermediate save',
            cold_continuation_exact=True)

    def global_capacity(self):
        saves = self.root/'Capacity slots'
        saves.mkdir()
        receivers = list(BULK_RECEIVERS)
        first, second, permanent = receivers[:3]
        extra = receivers[-1]
        checks = self.measurements['global_capacity'] = {}
        with self.owner('scene capacity', saves=saves) as (client, _):
            oracle = MarkOracle()
            state = self.start(client, saves, None, oracle)
            for begin in range(0, 64, 4):
                stamps = []
                for receiver in receivers[begin:begin+4]:
                    stamps.append(bulk_placement(receiver, 64 if receiver in (first, second) else None))
                    stamps.extend([bulk_placement(receiver)]*3)
                state = self.step(client, state, oracle, stamps=stamps, observe=begin==60)
            assert len(state['marks']) == 256 and [state['marks'][i]['id'] for i in (0, 4, 255)] == ['1', '5', '256']
            before_ids = {row['id'] for row in state['marks']}
            state = self.step(client, state, oracle, stamps=[bulk_placement(second, 2, recycle=True),
                                                            bulk_placement(extra, 2, recycle=True)])
            after_ids = {row['id'] for row in state['marks']}
            assert before_ids-after_ids == {'1', '5'} and after_ids-before_ids == {'257', '258'}
            assert [row['id'] for row in state['marks'][-2:]] == ['257', '258']
            checks['full_scene'] = dict(live=len(state['marks']), recycled_receiver='5', recycled_scene='1',
                                        new_ids=['257', '258'], counters=state['lifecycle'])
            full_stamp = bulk_placement(permanent, 2, recycle=True)
            state = self.step(client, state, oracle, stamps=[full_stamp])
            assert {row['id'] for row in state['marks']} == after_ids and oracle.next_id == 259
            assert state['lifecycle'] == dict(expired=0, recycled=2, dropped=1), state
            checks['no_local_victim'] = dict(live=len(state['marks']), counters=state['lifecycle'], next_id=259)
            state = self.step(client, state, oracle, stamps=[bulk_placement(first), bulk_placement(second)])
            assert [row['id'] for row in state['marks'][-2:]] == ['259', '260']
            state = self.step(client, state, oracle, stamps=[bulk_placement(extra, 2, recycle=True)], retry=True)
            assert all('expires_tick' not in row for row in state['marks']) and oracle.next_id == 261
            assert state['lifecycle'] == dict(expired=2, recycled=2, dropped=2), state
            checks['no_scene_victim'] = dict(live=len(state['marks']), counters=state['lifecycle'], next_id=261)
            saved = self.archive(client, saves, state, 'capacity-final', oracle)
            failures = checks['overflow_refusals'] = {}
            for label, stamp in (
                ('reject-policy', bulk_placement(extra)),
                ('drop-still-validates-ray', bulk_placement(extra, 2, recycle=True) | dict(ray=ray(500, 1))),
                ('drop-still-validates-profile', bulk_placement(extra, 2, recycle=True) | dict(profile=uid(900))),
            ):
                failures[label] = self.reject(client, 'runtime.step', self.request(state, stamps=[stamp]), -32040)
            invalid_style = bulk_placement(extra, 2, recycle=True) | dict(roughness=0)
            failures['drop-still-validates-style'] = self.reject(client, 'runtime.step',
                self.request(state, stamps=[invalid_style]), -32602)
            assert 'Scene surface mark capacity' in failures['reject-policy']['message'], failures
            assert 'must first hit' in failures['drop-still-validates-ray']['message'], failures
            assert 'profile' in failures['drop-still-validates-profile']['message'].lower(), failures
            assert exact(self.state(client, state['session'], state['tick']), state)
            assert self.archive(client, saves, state, 'capacity-after-refusals', oracle) == saved

    def legacy(self):
        saves = self.root/'Legacy extension slots'
        saves.mkdir()
        masked = RIGHT | dict(profile=PROFILE)
        with self.owner('legacy extension warm', saves=saves) as (client, warm):
            oracle = MarkOracle()
            fresh = self.start(client, saves, None, oracle)
            plain = self.step(client, fresh, oracle, stamps=[LEFT])
            v1 = self.archive(client, saves, plain, 'legacy-v1', oracle)
            textured = self.step(client, plain, oracle, stamps=[masked])
            v2 = self.archive(client, saves, textured, 'legacy-v2', oracle)
            assert (json.loads(v1)['snapshot']['payload']['surface_marks']['version'],
                    json.loads(v2)['snapshot']['payload']['surface_marks']['version']) == (1, 2)

        with self.owner('legacy extension cold', saves=saves) as (client, cold):
            oracle = MarkOracle()
            fresh = self.start(client, saves, None, oracle)
            oracle.advance(1, [LEFT])
            restored = self.restore(client, fresh, plain, 'legacy-v1', oracle)
            assert self.archive(client, saves, restored, 'legacy-v1-cold', oracle) == v1
            continued = self.step(client, restored, oracle, stamps=[masked])
            assert exact(continued, textured | dict(session=restored['session']))
            assert self.archive(client, saves, continued, 'legacy-v2-continued', oracle) == v2
            cold_v2 = self.restore(client, continued, textured, 'legacy-v2', oracle)
            assert self.archive(client, saves, cold_v2, 'legacy-v2-cold', oracle) == v2
        assert cold['pid'] != warm['pid']
        self.measurements['legacy'] = dict(v1=True, v2=True, bytes_exact=True)

    def run(self):
        self.revision = self.measurements['authored_revision'] = self.author()
        self.discovery()
        saves = self.root/'External save slots'
        saves.mkdir()
        if not self.args.runtime_root:
            frozen = tree_hashes(self.project)
            self.visible(saves)
            self.sustained()
            self.global_capacity()
            self.legacy()
            assert tree_hashes(self.project) == frozen
            return

        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 568, info
        assert info['needs_surface_mark_masks'] is True, info
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle = self.root/'Relocated Timed Marks Game'
        detached = self.root/'Unavailable source'
        output.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        game = bundle/'game.json'
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=frozen, detached_source=authored)
        assert self.image_hash in frozen.values()
        assert [value for path, value in frozen.items() if path.endswith('.pimage')] == [self.mask_sha256]
        self.visible(saves, game)
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
        parser.error('Supply a Native AOT descriptor or the CoreCLR hostfxr, bridge and assembly.')
    if args.runtime_root and not args.native_descriptor:
        parser.error('Standalone export requires --native-descriptor.')
    if args.gpu is not None and (not args.runtime_root or not 0 <= args.gpu <= 4095):
        parser.error('--gpu requires a standalone runtime and a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='timed-surface-marks-', dir=args.output.resolve()))
    test = TimedSurfaceMarksContract(args, root)
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
