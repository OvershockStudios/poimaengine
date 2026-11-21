#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check actual planted contacts, same-tick footprints and relocated save replay."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import traceback
from uuid import uuid4

from character_library_intake import LICENSE_SHA, SOURCE_SHA, decode_cooked, LibraryOracle
from imported_character_contract import point
from input_actions_gameplay_contract import exact
from runtime_foot_planting import inverse_rigid_point
from runtime_ground_contacts import CALIBRATION, CHAINS, RIG, unit, vector
from scene_capture import pixels
from surface_marks_capture import LIMITS, SurfaceMarksCapture
from textured_surface_marks_contract import profile_edit, TexturedSurfaceMarkContract
from world_particles_contract import component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, encode, error, file_hash, projection, rpc, success, tree_hashes, uid


ACTOR, PAD, PLAIN, TERRAIN, PROFILE, SPARE = map(uid, (920, 910, 911, 912, 913, 914))
CAMERA = uid(915)
PAD_CENTER = [0, -.45, 0]
MOVED_CENTER = [.06, -.45, .03]
MOVED_ROTATION = [0, math.sin(.08), 0, math.cos(.08)]
LENS = dict(vertical_fov=50, near=.1, far=15)
PAD_LIFE, TERRAIN_LIFE = 18, 4
CHECKPOINT, VISIBLE, TERRAIN_PLANT, FINAL = 4, 7, 13, 19
PAD_COLOR, TERRAIN_COLOR = [.95, .025, .016], [.04, .8, .07]
BASE = [.17, .19, .22]
STYLE = dict(half_extent_m=[.16, .09], half_depth_m=.025, minimum_normal_dot=.8,
             edge_fraction=.25, linear_rgb=PAD_COLOR, opacity=1, roughness=.86)
PAD_SURFACE = STYLE | dict(profile=PROFILE, lifetime_ticks=PAD_LIFE)
TERRAIN_SURFACE = STYLE | dict(linear_rgb=TERRAIN_COLOR, lifetime_ticks=TERRAIN_LIFE, sole_tolerance_m=.025)
MASK = ((0, 255, 255, 0), (255, 255, 255, 255), (0, 255, 255, 0), (0, 255, 255, 0))
MATERIAL = dict(base_color=BASE, emissive=[0, 0, 0], metallic=0, roughness=.8, double_sided=False)
DRAWABLE = dict(primitive='box', albedo=BASE, visible=True, cast_shadows=False, receive_shadows=False)
PLATFORM_BOX = dict(motion='kinematic', half_extents=[.5]*3, mass=1, friction=.5, restitution=0)
IDENTITY = [0, 0, 0, 1]


def motion(entity, position, duration, rotation=IDENTITY):
    return dict(entity=entity, position=position, rotation=rotation, duration_ticks=duration)


def pose(clip):
    return dict(entity=RIG, clip=clip, time=0, speed=1, loop=False, playing=False)


def manual_stamp(receiver=PAD, x=.395, z=.365):
    return STYLE | dict(receiver=receiver, ray=dict(origin=[x, .8, z], direction=[0, -1, 0], distance=2),
                        half_extent_m=[.065, .045], linear_rgb=[.04, .12, .9])


class FootprintContract(SurfaceMarksCapture):
    def edit(self, ops):
        reply = self.world_call('world.transact', request_id=uuid4().hex, base_revision=self.revision, ops=ops)
        assert reply['committed'] and reply['revision'] == self.revision+1, reply
        self.revision = reply['revision']

    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Planted Footprints')))
        self.revision = self.world_call('world.inspect')['revision']
        source = self.args.source
        notice = self.args.license.read_text(encoding='utf-8')
        assert file_hash(source) == SOURCE_SHA and file_hash(self.args.license) == LICENSE_SHA
        oracle = LibraryOracle(source.read_bytes())
        model = self.world_call('asset.source.inspect', source=self.native(source))
        asset = self.world_call('asset.import', source=self.native(source), expected_model_sha256=model['model_sha256'])['asset']
        cooked = self.assets/(asset+'.pmodel')
        assert file_hash(cooked) == asset
        decode_cooked(cooked.read_bytes(), oracle)
        record = dict(format='poima.asset-provenance', version=1, asset=asset, kind='model',
                      title='KayKit Adventurers 1.0 Knight', creator='Kay Lousberg',
                      license=dict(identifier='CC0-1.0', notice=notice))
        credit = self.world_call('asset.provenance.create', record=record)['record']
        self.edit([dict(op='asset.instantiate', id=RIG, asset=asset, name='Original Knight'),
                   dict(op='asset.provenance.set', asset=asset, records=[credit])])
        self.measurements['content'] = dict(source_sha256=SOURCE_SHA, license_sha256=LICENSE_SHA,
                                            model_sha256=asset, provenance_sha256=credit)
        self.mask = TexturedSurfaceMarkContract.import_mask(self, 'sole-mask', MASK)
        assert self.world_call('world.describe', view='component', name='FootprintSurface')['components'].keys() == {'FootprintSurface'}
        self.measurements['content']['mask_sha256'] = self.mask
        rig = json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig']
        rig.update(clip=36, time=0, speed=1, loop=False, playing=False,
                   ik=[dict(slot=slot, root=root, middle=middle, end=end, target=[0, 0, 0], pole=[0, 1, 1],
                            space='rig', metric='near-uniform-v1',
                            grounding=dict(calibration, reference_height=0, plant_lock=True, max_correction=.4))
                       for (slot, root, middle, end), calibration in zip(CHAINS, CALIBRATION, strict=True)])
        idle = oracle.pose(36, 0)[2]
        run = oracle.pose(48, 0)[2]
        self.source_idle = [point(idle[end], c['sole_offset']) for (_, _, _, end), c in zip(CHAINS, CALIBRATION, strict=True)]
        self.source_lift = [point(run[end], c['sole_offset']) for (_, _, _, end), c in zip(CHAINS, CALIBRATION, strict=True)]
        assert all(foot[1] < .03 for foot in self.source_idle) and all(foot[1] > .15 for foot in self.source_lift)
        terrain_recipe = encode(dict(format='poima.terrain.heightfield', version=1, samples_x=3, samples_z=3,
                                     tile_cells=2, origin_m=[-2.5, .025, -.5], spacing_m=[.5, .5],
                                     heights_m=[.025*x for _z in range(3) for x in range(3)]))
        terrain = self.world_call('world.terrain.inspect', recipe=terrain_recipe)
        assert self.world_call('world.terrain.cook', recipe=terrain_recipe, expected_recipe_sha256=terrain['recipe_sha256']) == terrain
        tile, = terrain['tiles']
        part = dict(asset=tile['asset'], primitive=tile['primitive'])
        self.measurements['content']['terrain'] = tile
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(5), type='Light'),
            component(uid(4), 'LightingEnvironment', dict(ambient=[.75]*3, exposure=1)),
            dict(op='entity.create', id=CAMERA, name='Top-down footprint witness camera'),
            component(CAMERA, 'Transform', transform([0, 3, 0], [-math.sqrt(.5), 0, 0, math.sqrt(.5)])),
            component(CAMERA, 'Camera', LENS),
            dict(op='entity.create', id=ACTOR, name='Knight physical wrapper'),
            component(ACTOR, 'BoxCollider', PLATFORM_BOX | dict(half_extents=[.05]*3)),
            dict(op='entity.reparent', id=RIG, parent=ACTOR, mode='keep_local'),
            component(RIG, 'AnimationRig', rig),
            dict(op='entity.create', id=PAD, name='Moving footprint pad'),
            component(PAD, 'Transform', transform(PAD_CENTER)),
            component(PAD, 'BoxCollider', PLATFORM_BOX),
            component(PAD, 'MeshRenderer', DRAWABLE),
            component(PAD, 'PbrMaterial', MATERIAL),
            component(PAD, 'FootprintSurface', PAD_SURFACE),
            dict(op='entity.create', id=PLAIN, name='Plain unmarked support'),
            component(PLAIN, 'Transform', transform([2, -.45, 0])),
            component(PLAIN, 'BoxCollider', PLATFORM_BOX | dict(motion='static')),
            component(PLAIN, 'MeshRenderer', DRAWABLE),
            component(PLAIN, 'PbrMaterial', MATERIAL),
            dict(op='entity.create', id=TERRAIN, name='Sloped cooked footprint terrain'),
            component(TERRAIN, 'StaticMesh', part | dict(visible=True, cast_shadows=False, receive_shadows=False)),
            component(TERRAIN, 'MeshCollider', part | dict(friction=.5, restitution=0)),
            component(TERRAIN, 'PbrMaterial', MATERIAL),
            component(TERRAIN, 'FootprintSurface', TERRAIN_SURFACE),
            dict(op='entity.create', id=SPARE, name='Explicit mark capacity control'),
            component(SPARE, 'Transform', transform([4, -.45, 0])),
            component(SPARE, 'BoxCollider', PLATFORM_BOX | dict(motion='static')),
            component(SPARE, 'MeshRenderer', DRAWABLE),
            component(SPARE, 'PbrMaterial', MATERIAL),
            profile_edit(PROFILE, self.mask),
        ]
        self.edit(ops)
        authored = self.world.read_bytes()
        self.measurements['authoring_rejections'] = {}
        invalid_ops = (
            ('zero-lifetime', [component(PAD, 'FootprintSurface', PAD_SURFACE | dict(lifetime_ticks=0))]),
            ('boolean-lifetime', [component(PAD, 'FootprintSurface', PAD_SURFACE | dict(lifetime_ticks=True))]),
            ('missing-profile', [component(PAD, 'FootprintSurface', PAD_SURFACE | dict(profile=uid(998)))]),
            ('remove-used-profile', [profile_edit(PROFILE, None)]),
            ('invalid-tolerance', [component(PAD, 'FootprintSurface', PAD_SURFACE | dict(sole_tolerance_m=.11))]),
        )
        for label, invalid in invalid_ops:
            reply, = self.world_calls(rpc('world.transact', request_id=uuid4().hex,
                                          base_revision=self.revision, ops=invalid))
            self.measurements['authoring_rejections'][label] = error(reply, -32602)
            assert self.world.read_bytes() == authored
        self.measurements['authored_revision'] = self.revision

    def step(self, client, session, tick, ticks=1, retry=False, **params):
        request = dict(request_id=uuid4().hex, session_id=session, expected_tick=tick, ticks=ticks,
                       expected_structure_revision=0, **params)
        result = client.call('runtime.step', request)
        assert (result['current_session_id'], result['current_tick']) == (session, tick+ticks), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        if retry:
            assert exact(client.call('runtime.step', request), result | dict(replayed=True))
        return tick+ticks

    def marks(self, client, session, tick):
        receipt = client.call('runtime.surface_marks.inspect',
                              dict(session_id=session, tick=tick, structure_revision=0))
        assert (receipt['session_id'], receipt['tick'], receipt['structure_revision']) == (session, tick, 0)
        assert receipt['limits'] == LIMITS, receipt
        return dict(marks=receipt['marks'], lifecycle=receipt.get('lifecycle'))

    def contacts(self, client, session, tick):
        states = []
        for slot, *_ in CHAINS:
            reply = client.call('runtime.ik', dict(session_id=session, tick=tick, entity=RIG, slot=slot,
                                                   include_grounding=True, include_planting=True))
            assert (reply['session_id'], reply['tick']) == (session, tick), reply
            state = reply['state']
            assert state['grounding']['decision_tick'] == tick, state
            states.append(state)
        return states

    def entity_matrix(self, client, session, tick, entity):
        return client.call('runtime.entity', dict(session_id=session, tick=tick, id=entity))['world_matrix']

    def planted_rows(self, client, session, tick, receiver, rows, first_id, style, source_offset):
        feet = self.contacts(client, session, tick)
        support = self.entity_matrix(client, session, tick, receiver)
        for index, (foot, mark) in enumerate(zip(feet, rows, strict=True)):
            p, g = foot['planting'], foot['grounding']
            assert p['status'] == 'locked' and p['anchor']['acquired_tick'] == tick, p
            assert g['status'] == 'contact' and g['cast'] and g['hit']['entity'] == receiver, g
            assert p['anchor']['support'] == receiver and p['position_error'] < .0002, p
            near_vector(g['input_sole'], [a+b for a, b in zip(self.source_idle[index], source_offset, strict=True)])
            near_vector(point(support, mark['center_m']), g['hit']['position'])
            near_vector(p['target_sole'], g['hit']['position'])
            near_vector(mark['center_m'], inverse_rigid_point(support, p['target_sole']))
            near_vector(unit(vector(support, mark['normal'])), g['hit']['normal'])
            near_vector(unit(vector(support, mark['tangent'])), p['target_forward'])
            near_vector(g['actual_sole'], p['target_sole'], tolerance=.0002)
            assert (mark['id'], mark['receiver'], mark['tick'], mark['expires_tick']) == (
                str(first_id+index), receiver, tick, tick+style['lifetime_ticks']), mark
            assert mark.get('profile') == style.get('profile')
            assert mark.get('mask_sha256') == (self.mask if receiver == PAD else None)
            near_vector(mark['half_extent_m'], style['half_extent_m'])
            near_vector(mark['linear_rgb'], style['linear_rgb'])
            assert (g['hit']['triangle'] is not None) == (receiver == TERRAIN), g
            if receiver == TERRAIN:
                near_vector(g['hit']['normal'], unit([-.05, 1, 0]))
                assert math.isclose(g['hit']['position'][1], .025+.05*(g['hit']['position'][0]+2.5), abs_tol=4e-5)

    def lifted(self, client, session, tick, original, position):
        states = self.contacts(client, session, tick)
        for index, (state, old) in enumerate(zip(states, original, strict=True)):
            g, p = state['grounding'], state['planting']
            assert g['status'] == p['status'] == 'lifted' and not g['cast'] and p['anchor'] is None and p['armed'], state
            assert g['casts'] == old['grounding']['casts'], g
            near_vector(g['input_sole'], [a+b for a, b in zip(self.source_lift[index], position, strict=True)])

    def capture(self, client, session, tick, label, witnesses):
        path = self.root/(label+'.bmp')
        camera = self.entity_matrix(client, session, tick, CAMERA)
        coordinates = [tuple(map(math.floor, projection(camera, LENS, p))) for p in witnesses]
        assert len(set(coordinates)) == len(witnesses)
        width, height = CAPTURE_SIZE
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in coordinates]))
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera and receipt['nvrhi_errors'] == 0, receipt
        diagnostic = receipt['render_diagnostics']
        assert diagnostic['lighting_path'] == self.args.lighting_path, diagnostic
        bitmap = pixels(path)
        samples = []
        for (x, y), item in zip(coordinates, diagnostic['scene_products']['probes'], strict=True):
            assert (item['x'], item['y']) == (x, y) and item['surface_valid'] is True, item
            normal = item['shading_normal'][:3]
            near_vector(unit(normal), [0, 1, 0])
            # The scene product stores normal components in FP16. Check unit
            # length within one representable step at 1 as well as direction.
            assert abs(math.hypot(*normal)-1) <= 2**-10, item
            samples.append(dict(pixel=[x, y], rgb=bitmap[y][x], hdr=item['resolved_hdr'], depth=item['depth'],
                                normal=normal))
        capture = dict(sha256=file_hash(path), gpu=receipt['gpu'], lighting_path=diagnostic['lighting_path'],
                       samples=samples, tick=tick)
        self.measurements.setdefault('captures', {})[label] = capture
        return capture

    def continuation(self, client, session, checkpoint, saves, suffix):
        tick = CHECKPOINT
        prior = self.contacts(client, session, tick)
        tick = self.step(client, session, tick, 3, animations=[pose(48)], motions=[motion(ACTOR, [2, 0, 0], 3)])
        assert tick == VISIBLE
        self.lifted(client, session, tick, prior, [2, 0, 0])
        assert exact(self.marks(client, session, tick), checkpoint)
        support = self.entity_matrix(client, session, tick, PAD)
        targets = [point(support, row['center_m']) for row in checkpoint['marks'][1:]]
        control = point(support, [0, .5, -.34])
        capture = self.capture(client, session, tick, 'uncovered-'+suffix, [*targets, control]) if self.args.gpu is not None else None
        tick = self.step(client, session, tick, animations=[pose(36)])
        plain = self.contacts(client, session, tick)
        assert all(foot['planting']['status'] == 'locked' and foot['planting']['anchor']['support'] == PLAIN
                   and foot['planting']['anchor']['acquired_tick'] == tick for foot in plain), plain
        assert exact(self.marks(client, session, tick), checkpoint)
        tick = self.step(client, session, tick, 4, animations=[pose(48)], motions=[motion(ACTOR, [-2, 0, 0], 4)])
        self.lifted(client, session, tick, plain, [-2, 0, 0])
        tick = self.step(client, session, tick, animations=[pose(36)])
        assert tick == TERRAIN_PLANT
        terrain = self.marks(client, session, tick)
        assert exact(terrain['marks'][:3], checkpoint['marks'])
        self.planted_rows(client, session, tick, TERRAIN, terrain['marks'][3:], 4, TERRAIN_SURFACE, [-2, 0, 0])
        tick = self.step(client, session, tick, 3)
        assert exact(self.marks(client, session, tick), terrain)
        tick = self.step(client, session, tick)
        after_terrain = self.marks(client, session, tick)
        assert exact(after_terrain['marks'], checkpoint['marks'])
        assert after_terrain['lifecycle'] == dict(expired=2, recycled=0, dropped=0), after_terrain
        tick = self.step(client, session, tick, 2)
        assert tick == FINAL
        final = self.marks(client, session, tick)
        assert exact(final['marks'], checkpoint['marks'][:1])
        assert final['lifecycle'] == dict(expired=4, recycled=0, dropped=0), final
        if capture:
            cleared = self.capture(client, session, tick, 'expired-'+suffix, [*targets, control])
            for before, after in zip(capture['samples'][:2], cleared['samples'][:2], strict=True):
                red, green, blue = before['rgb']
                assert red > max(green, blue)+30, (before, after)
                assert before['rgb'] != after['rgb'] and abs(before['depth']-after['depth']) < 1e-6, (before, after)
                assert before['normal'] == after['normal'], (before, after)
                assert before['hdr'][0] > after['hdr'][0]+.15, (before, after)
            assert capture['samples'][2] == cleared['samples'][2]
        archive = self.archive(client, saves, session, tick, 730 if suffix == 'warm' else 731, 'final-'+suffix)
        return dict(terrain=terrain, after_terrain=after_terrain, final=final), archive

    def verify(self, game=None):
        saves = self.root/'External footprint slots'
        saves.mkdir()
        with self.owner('warm footprint owner', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, self.revision, uuid4().hex, saves, game)
            assert self.marks(client, session, 0) == dict(marks=[], lifecycle=None)
            unplanted = self.contacts(client, session, 0)
            assert all(foot['grounding']['casts'] == 0 for foot in unplanted)
            snapshot = self.archive(client, saves, session, 0, 719, 'before-rejected-step', has_marks=False)
            missed = manual_stamp(x=20)
            refusal = self.reject(client, 'runtime.step', dict(request_id=uuid4().hex, session_id=session,
                expected_tick=0, expected_structure_revision=0, ticks=1, surface_marks=[manual_stamp(), missed]), -32040)
            assert 'must first hit' in refusal['message'], refusal
            assert exact(self.contacts(client, session, 0), unplanted)
            assert self.archive(client, saves, session, 0, 720, 'after-rejected-step', has_marks=False) == snapshot
            tick = self.step(client, session, 0, surface_marks=[manual_stamp()], retry=True)
            initial = self.marks(client, session, tick)
            assert initial['lifecycle'] == dict(expired=0, recycled=0, dropped=0), initial
            explicit = initial['marks'][0]
            assert (explicit['id'], explicit['receiver'], explicit['tick']) == ('1', PAD, 1)
            assert 'expires_tick' not in explicit
            near_vector(explicit['center_m'], [.395, .5, .365])
            self.planted_rows(client, session, tick, PAD, initial['marks'][1:], 2, PAD_SURFACE, [0, 0, 0])
            tick = self.step(client, session, tick, 3, motions=[motion(PAD, MOVED_CENTER, 3, MOVED_ROTATION)])
            assert tick == CHECKPOINT
            self.confirm_pad_transport(client, session, tick, initial)
            saved = self.archive(client, saves, session, tick, 721, 'checkpoint')
            expected, final = self.continuation(client, session, initial, saves, 'warm')

        with self.owner('cold footprint owner', game=game, saves=saves) as (client, cold):
            fresh = self.initialize(client, self.revision, uuid4().hex, saves, game)
            session = uuid4().hex
            assert self.load(client, self.revision, fresh, session, 'checkpoint', CHECKPOINT, 722) == 0
            self.confirm_pad_transport(client, session, CHECKPOINT, initial)
            assert self.archive(client, saves, session, CHECKPOINT, 723, 'restored-checkpoint') == saved
            observed, restored = self.continuation(client, session, initial, saves, 'cold')
            assert restored == final and exact(observed, expected), observed
            if self.args.gpu is not None:
                captures = self.measurements['captures']
                assert captures['uncovered-warm'] == captures['uncovered-cold']
                assert captures['expired-warm'] == captures['expired-cold']
        assert warm['pid'] != cold['pid']
        self.measurements['replay'] = dict(initial=initial, **expected, cold_continuation_exact=True)

    def confirm_pad_transport(self, client, session, tick, initial):
        assert exact(self.marks(client, session, tick), initial)
        support = self.entity_matrix(client, session, tick, PAD)
        near_vector(support[12:15], MOVED_CENTER)
        for state, mark in zip(self.contacts(client, session, tick), initial['marks'][1:], strict=True):
            plant, g = state['planting'], state['grounding']
            assert plant['status'] == 'locked' and plant['anchor']['acquired_tick'] == 1, plant
            assert g['casts'] == tick and g['decision_tick'] == tick and g['cast'], g
            near_vector(point(support, mark['center_m']), plant['target_sole'])
            near_vector(unit(vector(support, mark['tangent'])), plant['target_forward'])
            near_vector(g['actual_sole'], plant['target_sole'], tolerance=.0002)

    def residual_policy(self):
        saves = self.root/'Sole tolerance slots'
        saves.mkdir()
        with self.owner('residual threshold', saves=saves) as (client, _):
            session = self.initialize(client, self.revision, uuid4().hex, saves, None)
            controls = [dict(entity=RIG, slot=slot, enabled=True, weight=.1) for slot, *_ in CHAINS]
            tick = self.step(client, session, 0, grounding=controls)
            feet = self.contacts(client, session, tick)
            assert all(state['planting']['status'] == 'locked' and state['planting']['position_error'] > .025
                       for state in feet), feet
            assert self.marks(client, session, tick) == dict(marks=[], lifecycle=None)
            higher_weight = [control | dict(weight=1) for control in controls]
            tick = self.step(client, session, tick, grounding=higher_weight)
            assert self.marks(client, session, tick) == dict(marks=[], lifecycle=None)
            tick = self.step(client, session, tick, animations=[pose(48)])
            tick = self.step(client, session, tick, animations=[pose(36)])
            self.planted_rows(client, session, tick, PAD, self.marks(client, session, tick)['marks'],
                              1, PAD_SURFACE, [0, 0, 0])
        self.measurements['residual_rejection'] = dict(initial_errors_m=[s['planting']['position_error'] for s in feet],
                                                     stamp_after_replant_tick=tick)

    def shared_budget(self):
        saves = self.root/'Placement budget slots'
        saves.mkdir()
        offsets = [-.3, -.1, .1, .3]
        explicit = [manual_stamp(PAD, x, -.33) for x in offsets[:3]]
        explicit += [manual_stamp(receiver, center+x, -.33)
                     for receiver, center in ((PLAIN, 2), (TERRAIN, -2), (SPARE, 4)) for x in offsets]
        with self.owner('shared placement budget', saves=saves) as (client, _):
            session = self.initialize(client, self.revision, uuid4().hex, saves, None)
            tick = self.step(client, session, 0, surface_marks=explicit)
            placed = self.marks(client, session, tick)
            assert [mark['id'] for mark in placed['marks']] == [str(i) for i in range(1, 17)], placed
            assert [mark['receiver'] for mark in placed['marks'][:15]] == [stamp['receiver'] for stamp in explicit]
            assert all('expires_tick' not in mark for mark in placed['marks'][:15]), placed
            assert placed['lifecycle'] == dict(expired=0, recycled=0, dropped=0), placed
            automatic = placed['marks'][15]
            feet = self.contacts(client, session, tick)
            assert all(foot['planting']['anchor']['acquired_tick'] == tick for foot in feet)
            assert (automatic['receiver'], automatic['expires_tick']) == (PAD, tick+PAD_LIFE)
            near_vector(automatic['center_m'], feet[0]['planting']['anchor']['local_point'])
            tick = self.step(client, session, tick)
            assert exact(self.marks(client, session, tick), placed)
        self.measurements['placement_budget'] = dict(explicit=15, eligible=2, emitted=1,
                                                    no_late_retry=True, automatic=automatic)

    def refuse_ineligible_receiver(self, frozen):
        detached = self.root/'Invalid footprint project'
        shutil.copytree(self.project, detached)
        manifest = self.native(detached/self.manifest.name)
        mutation = rpc('world.transact', request_id=uuid4().hex, base_revision=self.revision,
                       ops=[component(PAD, 'BoxCollider', PLATFORM_BOX | dict(half_extents=[.4, .5, .5]))])
        reply = success(json.loads(self.cli('world', '--project', manifest, stdin=[mutation | dict(id=1)])))
        assert reply['committed'] and reply['revision'] == self.revision+1, reply
        before = tree_hashes(detached)
        command = [str(self.args.binary.resolve()), 'project', 'inspect', manifest]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        reply = json.loads(result.stdout)
        self.calls.append(dict(argv=command, exit=result.returncode, reply=reply, stderr=result.stderr))
        assert result.returncode == 4 and reply['status'] == 'error', reply
        diagnostic, = reply['diagnostics']
        assert diagnostic['code'] == 'project.failed', reply
        assert 'FootprintSurface' in diagnostic['message'] and 'half extents' in diagnostic['message'], reply
        assert tree_hashes(detached) == before and tree_hashes(self.project) == frozen
        self.measurements['ineligible_receiver'] = dict(diagnostic=diagnostic, project_bytes_unchanged=True)

    def run(self):
        self.author()
        frozen = tree_hashes(self.project)
        if not self.args.runtime_root:
            self.refuse_ineligible_receiver(frozen)
            self.verify()
            self.residual_policy()
            self.shared_budget()
            assert tree_hashes(self.project) == frozen
            return
        output, relocated = self.root/'Initial export', self.root/'Relocated Footprint Game'
        detached = self.root/'Unavailable authored project'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--runtime',
                                    self.native(self.args.runtime_root), '--output', self.native(output))))
        output.rename(relocated)
        self.project.rename(detached)
        bundle = tree_hashes(relocated)
        assert not self.project.exists() and not output.exists()
        game = relocated/'game.json'
        assert self.mask in bundle.values() and self.measurements['content']['model_sha256'] in bundle.values()
        assert self.measurements['content']['provenance_sha256'] in bundle.values()
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=bundle, source_path_unavailable=True)
        self.verify(game)
        assert tree_hashes(relocated) == bundle and tree_hashes(detached) == frozen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for path in ('source', 'license', 'output'):
        parser.add_argument('--'+path, type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run the fixture without -O.')
    if args.gpu is not None and (not args.runtime_root or not 0 <= args.gpu <= 4095):
        parser.error('--gpu requires a standalone runtime and a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='footprints-', dir=args.output.resolve()))
    fixture = FootprintContract(args, root)
    result = dict(format='poima.footprint-surface-contract.v1', passed=False,
                  harness_sha256=file_hash(Path(__file__)), executable_sha256=file_hash(args.binary),
                  calls=fixture.calls, measurements=fixture.measurements, standalone=bool(args.runtime_root))
    try:
        fixture.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}', flush=True)


if __name__ == '__main__':
    main()
