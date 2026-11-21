#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify persistent cooked masks in headless gameplay and relocated Vulkan games."""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import traceback
from uuid import uuid4
import zlib

from gameplay_directional_surface_mark_contract import (
    BASE, CAMERA, CAMERA_CENTER, CHECKPOINT_TICK, DirectionalSurfaceMarkContract, FINAL_TICK,
    LENS, MARKS, MOVED, MOVER, QUARTER_TURN_Z, ROUGHNESS, STAMPED, caller_marks, world_delta, world_point,
)
from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from instance_save_evolution import raw_digest, raw_text, replace_member, stored
from scene_capture import pixels
from surface_marks_capture import radiance, world_position
from world_particles_contract import near_vector
from world_terrain_contract import CAPTURE_SIZE, error, file_hash, projection, rpc, success, tree_hashes, uid


TYPE = 'Poima.Tests.ManagedTexturedSurfaceMarkGame'
IDENTITY = 'poima.test.managed-textured-surface-mark'
FEATURES = {'baseline_v7', 'surface_mark_v1', 'surface_mark_texture_v3'}
PRIMARY, ALIAS, COMPILED, INVALID = (uid(i) for i in range(130, 134))
MASK_A = ((0, 0, 255, 255), (0, 0, 255, 255), (255, 255, 0, 0))
MASK_B = ((255, 255, 0), (0, 0, 255))
STAMP_PROFILES = (PRIMARY, ALIAS, COMPILED, None)
STAMP_MASKS = (MASK_A, MASK_A, MASK_B, None)


def write_mask(path, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    # Different green, blue and alpha values make channel selection observable.
    scanlines = [b'\0' + bytes(channel for value in row for channel in (value, 255-value, 73, 47)) for row in rows]
    header = struct.pack('>IIBBBBB', len(rows[0]), len(rows), 8, 6, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                     + chunk(b'IDAT', zlib.compress(b''.join(scanlines))) + chunk(b'IEND', b''))


def profile_edit(profile, mask):
    return dict(op='surface_mark.profile.set', id=profile, mask=mask)


def sample_mask(rows, local_x, local_y):
    height, width = len(rows), len(rows[0])
    column = min(width-1, max(0., (.5+.5*local_x)*width-.5))
    row = min(height-1, max(0., (.5-.5*local_y)*height-.5))
    x, y = math.floor(column), math.floor(row)
    next_x, next_y = min(width-1, x+1), min(height-1, y+1)
    fx, fy = column-x, row-y
    top = rows[y][x]*(1-fx)+rows[y][next_x]*fx
    bottom = rows[next_y][x]*(1-fx)+rows[next_y][next_x]*fx
    return (top*(1-fy)+bottom*fy)/255


def expected_material(point, receiver, tick):
    base, roughness = BASE, ROUGHNESS
    details = {}
    for stamp, mask in zip(MARKS, STAMP_MASKS, strict=True):
        if stamp.receiver != receiver:
            continue
        cx, cy, _ = world_point(receiver, tick, stamp.center)
        tx, ty = world_delta(receiver, tick, stamp.tangent)
        dx, dy = point[0]-cx, point[1]-cy
        u, v = (dx*tx+dy*ty)/stamp.major, (-dx*ty+dy*tx)/stamp.minor
        radius = math.hypot(u, v)
        ramp = min(1., max(0., (1-radius)/stamp.edge))
        edge = ramp*ramp*(3-2*ramp)
        texture = sample_mask(mask, u, v) if mask else 1.
        weight = stamp.opacity*edge*texture
        base = tuple(a+(b-a)*weight for a, b in zip(base, stamp.color, strict=True))
        roughness += (stamp.roughness-roughness)*weight
        details[stamp.label] = dict(coverage=texture, edge=edge, weight=weight)
    return base, roughness, details


def capture_witnesses(tick, camera):
    controls = (
        {'black': (-.60, .30), 'white': (.60, .30), 'filtered': (0, .30), 'corner': (.72, .38)},
        {'black': (-.60, .30), 'white': (.60, .30), 'filtered': (0, .30), 'outside': (0, 1.55)},
        {'black': (-.35, -.55), 'white': (-.35, .55), 'filtered': (-.50, 0), 'corner': (.83, .20)},
        {'analytic': (0, 0), 'outside': (0, 1.55)},
    )
    result = {}
    for stamp, samples in zip(MARKS, controls, strict=True):
        cx, cy = stamp.center
        tx, ty = stamp.tangent
        for witness, (u, v) in samples.items():
            local = (cx+stamp.major*u*tx-stamp.minor*v*ty, cy+stamp.major*u*ty+stamp.minor*v*tx)
            point = world_point(stamp.receiver, tick, local)
            x, y = map(math.floor, projection(camera, LENS, point))
            result[f'{stamp.label}_{witness}'] = (stamp, witness, x, y)
    return result


def stencil_commands(alias=ALIAS):
    primary, secondary = caller_marks()
    secondary.pop('travel_direction')
    return [primary | dict(profile=PRIMARY), secondary | dict(profile=alias)]


class TexturedSurfaceMarkContract(DirectionalSurfaceMarkContract):
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
                metadata['minimum_services_bytes']) == (1, 80, 7, 560), metadata
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

    def import_mask(self, name, rows, color_space='linear'):
        source = self.root/(name+'.png')
        write_mask(source, rows)
        imported = self.world_call('asset.image.import', source=self.native(source), color_space=color_space)
        assert imported['color_space'] == color_space
        mip = imported['mips'][0]
        assert (mip['width'], mip['height'], mip['bytes']) == (len(rows[0]), len(rows), len(rows[0])*len(rows)*4)
        asset = imported['asset']
        path = self.assets/(asset+'.pimage')
        assert file_hash(path) == asset and path.stat().st_size == imported['bytes'], imported
        source.unlink()
        return asset

    def author(self):
        revision = super().author()
        first = self.import_mask('primary', MASK_A)
        second = self.import_mask('compiled', MASK_B)
        srgb = self.import_mask('wrong-color-space', MASK_A, 'srgb')
        large = self.import_mask('too-wide', ((0,)*257,))
        assert len({first, second, srgb, large}) == 4
        self.catalog = {PRIMARY: first, ALIAS: first, COMPILED: second}
        before = self.world.read_bytes()
        refusals = self.measurements['authoring_refusals'] = {}
        for label, mask in (('srgb', srgb), ('missing', 'e'*64), ('too-wide', large)):
            request = rpc('world.transact', request_id=uuid4().hex, base_revision=revision,
                          ops=[profile_edit(PRIMARY, first), profile_edit(INVALID, mask)])
            refusals[label] = error(self.world_calls(request)[0], -32050)
            assert self.world.read_bytes() == before and self.world_call('world.inspect')['revision'] == revision
        over_capacity = rpc('world.transact', request_id=uuid4().hex, base_revision=revision,
                            ops=[profile_edit(uid(i), first) for i in range(200, 233)])
        refusals['too-many-profiles'] = error(self.world_calls(over_capacity)[0], -32602)
        assert self.world.read_bytes() == before and self.world_call('world.inspect')['revision'] == revision

        edited = self.world_call('world.transact', request_id=uuid4().hex, base_revision=revision,
                                 ops=[profile_edit(profile, mask) for profile, mask in self.catalog.items()])
        assert edited['committed'] and edited['revision'] == revision+1, edited
        assert edited['changed_world_fields'] == ['surface_mark_profiles'], edited
        active = self.world_call('world.inspect')
        assert active['surface_mark_profiles'] == {profile: dict(mask=mask) for profile, mask in self.catalog.items()}
        self.check_dependencies(self.catalog)
        owner = dict(kind='world', id=active['world_id'])
        paged = self.world_call('world.asset.references', owner=owner, revision=edited['revision'], limit=1)
        full = self.world_call('world.asset.references', owner=owner, revision=edited['revision'])
        expected_cursor = dict(owner=owner, component='surface_mark_profiles', path=f'/{PRIMARY}/mask')
        assert paged['next_after'] == expected_cursor and len(paged['edges']) == 1, paged
        remaining = self.world_call('world.asset.references', owner=owner, revision=edited['revision'],
                                    after=expected_cursor)
        assert remaining['next_after'] is None and full['edges'] == paged['edges']+remaining['edges']
        edges = full['edges']
        assert [edge['path'] for edge in edges] == [f'/{key}/mask' for key in self.catalog], edges
        assert all(edge['component'] == 'surface_mark_profiles' and edge['asset'] == self.catalog[key]
                   for edge, key in zip(edges, self.catalog, strict=True)), edges
        assert self.world_call('world.asset.references', revision=edited['revision'], asset=first)['edges'] == edges[:2]
        self.measurements['mask_catalog'] = dict(profiles=self.catalog, assets=[first, second], edges=edges,
                                                first_mask_red=MASK_A, second_mask_red=MASK_B)
        return edited['revision']

    def check_dependencies(self, catalog):
        dependencies = self.world_call('world.dependencies')
        assert dependencies['needs_surface_mark_masks'] is True, dependencies
        assert sorted(asset['filename'] for asset in dependencies['assets']) == sorted(
            mask+'.pimage' for mask in set(catalog.values())), dependencies

    def state(self, client, session, tick):
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        assert (info['session_id'], info['tick']) == (session, tick), info
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity']) == (backend, TYPE, IDENTITY), module
        assert module['assembly_sha256'] == self.image_hash, module
        if self.args.native_descriptor:
            assert module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False)
        structure = info['structure_revision']
        values = module['values']
        source_id = values['TemporarySource']
        source_pose = None if source_id == uid(0) else self.pose(client, session, tick, source_id)
        return dict(session=session, tick=tick, revision=info['revision'], structure=structure, values=values,
                    source_pose=source_pose, receiver_pose=self.pose(client, session, tick, MOVER),
                    marks=self.inspect(client, session, tick, structure=structure))

    def check_state(self, state, catalog=None, profiles=STAMP_PROFILES):
        super().check_state(state)
        bindings = self.catalog if catalog is None else catalog
        for mark, profile in zip(state['marks'], profiles, strict=True):
            if profile:
                assert (mark['profile'], mark['mask_sha256']) == (profile, bindings[profile]), mark
            else:
                assert 'profile' not in mark and 'mask_sha256' not in mark, mark

    def archive(self, client, saves, state, slot):
        receipt = client.call('save.write', dict(request_id=uuid4().hex, configuration_generation=1,
            slot=slot, expected_generation=0, session_id=state['session'], expected_tick=state['tick'],
            expected_gameplay_revision=state['revision'], expected_structure_revision=state['structure']))
        path, = (saves/f'slot-{slot}').glob('p-*.bin')
        raw = path.read_bytes()
        assert receipt['sha256'] == file_hash(path) and receipt['bytes'] == len(raw), receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert payload['tick'] == state['tick']
        assert exact(payload['surface_marks'], dict(version=2, next_id=5,
            marks=[row | dict(id=int(row['id'])) for row in state['marks']]))
        self.measurements.setdefault('archives', {})[slot] = dict(tick=state['tick'], bytes=len(raw),
                                                                  sha256=receipt['sha256'])
        return raw

    def refuse_unknown_profile(self, client, saves, checkpoint, raw):
        valid, second = stencil_commands()
        request = self.request(checkpoint, ticks=2, stamps=[valid, second | dict(profile=INVALID)],
                               motion=(MOVED, QUARTER_TURN_Z, 2))
        failure = self.reject(client, 'runtime.step', request, -32040)
        assert 'profile' in failure['message'].lower(), failure
        after = self.state(client, checkpoint['session'], checkpoint['tick'])
        assert exact(after, checkpoint) and self.archive(client, saves, after, 'unknown-profile') == raw
        self.measurements['refusals'] = {'unknown_profile': failure}

    def refuse_changed_mask(self, client, saves, checkpoint, raw):
        original = saves/'slot-checkpoint'
        original_files = tree_hashes(original)
        slot = 'mismatched-mask'
        negative = saves/f'slot-{slot}'
        shutil.copytree(original, negative)
        _, path, entry = stored(saves, slot)
        original_mask, replacement_mask = self.catalog[PRIMARY], self.catalog[COMPILED]
        mark_path = ('snapshot', 'payload', 'surface_marks', 'marks', 0)
        saved_mark = json.loads(raw)['snapshot']['payload']['surface_marks']['marks'][0]
        assert (saved_mark['profile'], saved_mark['mask_sha256']) == (PRIMARY, original_mask)
        assert original_mask != replacement_mask

        # Rewrite only the owned negative slot. Preserve the exact encoding of
        # all other native fields, including shortest-roundtrip float tokens.
        changed = replace_member(raw_text(path), (*mark_path, 'mask_sha256'), replacement_mask)
        changed = replace_member(changed, ('snapshot', 'sha256'), raw_digest(changed, ('snapshot', 'payload')))
        path.write_bytes(changed.encode('utf-8'))
        identity = dict(sha256=file_hash(path), bytes=path.stat().st_size)
        manifest = negative/'current.json'
        contents = raw_text(manifest)
        receipts = json.loads(contents)['payload']['receipts']
        receipt, = (i for i, row in enumerate(receipts) if row['generation'] == entry['generation'])
        for target in (('current',), ('receipts', receipt)):
            for key, value in identity.items():
                contents = replace_member(contents, ('payload', *target, key), value)
        contents = replace_member(contents, ('sha256',), raw_digest(contents, ('payload',)))
        manifest.write_bytes(contents.encode('utf-8'))
        selected = client.call('save.inspect', dict(slot=slot))
        assert selected['selected']['sha256'] == identity['sha256'] and selected['selected']['verified'], selected
        request = self.restore_request(checkpoint, uuid4().hex, self.revision, slot)
        failure = self.reject(client, 'save.load', request, -32070)
        assert 'Saved surface mark mask does not match its frozen profile.' in failure['message'], failure
        live = self.state(client, checkpoint['session'], checkpoint['tick'])
        assert exact(live, checkpoint) and self.archive(client, saves, live, 'after-mask-mismatch') == raw
        assert tree_hashes(original) == original_files
        self.measurements['mask_mismatch_restore'] = dict(
            profile=PRIMARY, original_mask=original_mask, substituted_mask=replacement_mask,
            payload_sha256=identity['sha256'], manifest_sha256=file_hash(manifest),
            store_verified=selected['selected']['verified'], rejection=failure, active_archive_exact=True)

    def capture(self, client, state, label):
        tick = state['tick']
        camera = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *CAMERA_CENTER, 1]
        points = capture_witnesses(tick, camera)
        path = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        report = client.call('runtime.capture', dict(session_id=state['session'], tick=tick,
            structure_revision=state['structure'], camera=CAMERA, path=self.native(path), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path, reconstruction='none',
            capture_frames=2, scene_product_probes=[dict(x=x, y=y) for _, _, x, y in points.values()]))
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
        for (name, (stamp, witness, x, y)), probe in zip(points.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            assert probe['surface_valid'] and abs(probe['depth']-(1-1/4)*10/9) < 2e-6, probe
            near_vector(probe['shading_normal'][:3], [0, 0, 1])
            point = world_position(x, y)
            base, roughness, details = expected_material(point, stamp.receiver, tick)
            sample = details[stamp.label]
            expected = radiance(point, base, roughness)
            bmp = [display(channel) for channel in expected]
            item = dict(hdr=probe['resolved_hdr'], expected_hdr=expected, bmp_rgb=image[y][x], expected_bmp=bmp,
                        pixel=[x, y], depth=probe['depth'], base=base, roughness=roughness, **sample)
            record['probes'][name] = item
            assert probe['raw_hdr'] == probe['resolved_hdr'], item
            near_vector(probe['resolved_hdr'], expected, tolerance=.002)
            assert max(abs(a-b) for a, b in zip(image[y][x], bmp, strict=True)) <= 2, item
            if witness == 'black':
                assert sample['coverage'] < .06 and sample['edge'] > .9, (name, item)
            elif witness == 'white':
                assert sample['coverage'] > .94 and sample['edge'] > .9, (name, item)
            elif witness == 'filtered':
                assert .30 < sample['coverage'] < .70 and sample['edge'] > .9, (name, item)
            elif witness == 'outside':
                assert base == BASE and roughness == ROUGHNESS, (name, item)
            elif witness == 'analytic':
                assert sample['weight'] == stamp.opacity, (name, item)
            elif witness == 'corner' and stamp.label == 'compiled':
                assert .1 < sample['coverage'] < .9 and .1 < sample['edge'] < .9, (name, item)
        return record

    def restore_request(self, live, session, revision, slot='checkpoint'):
        return dict(request_id=uuid4().hex, configuration_generation=1, slot=slot,
                    revision=revision, expected_generation=1, expected_session_id=live['session'],
                    expected_tick=live['tick'], expected_gameplay_revision=live['revision'],
                    expected_structure_revision=live['structure'], new_session_id=session)

    def exercise(self, saves, game=None):
        with self.owner('textured marks warm', game=game, saves=saves) as (client, warm):
            initial = self.start(client, saves, game)
            checkpoint = self.step(client, initial, use=True, stamps=stencil_commands(),
                                   motion=(STAMPED, QUARTER_TURN_Z, 1), retry=True)
            self.check_state(checkpoint)
            saved = self.archive(client, saves, checkpoint, 'checkpoint')
            if not game:
                self.refuse_unknown_profile(client, saves, checkpoint, saved)
                self.refuse_changed_mask(client, saves, checkpoint, saved)
            if self.args.gpu is not None:
                checkpoint_frame = self.capture(client, checkpoint, 'checkpoint-warm')
            final = self.continue_scene(client, checkpoint)
            final_bytes = self.archive(client, saves, final, 'final')
            if self.args.gpu is not None:
                final_frame = self.capture(client, final, 'final-warm')
                assert final_frame['sha256'] != checkpoint_frame['sha256']

        with self.owner('textured marks cold', game=game, saves=saves) as (client, cold):
            fresh, session = self.start(client, saves, game), uuid4().hex
            restored = client.call('save.load', self.restore_request(fresh, session, self.revision))
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
        return checkpoint, saved, final, final_bytes

    def verify_replacement_restore(self, saves, original):
        checkpoint, saved, final, final_bytes = original
        old_revision = self.revision
        replacement = {profile: self.catalog[COMPILED] for profile in (PRIMARY, COMPILED)}
        authored = self.world_call('world.transact', request_id=uuid4().hex, base_revision=old_revision,
            ops=[profile_edit(PRIMARY, replacement[PRIMARY]), profile_edit(ALIAS, None)])
        assert authored['committed'] and authored['revision'] == old_revision+1, authored
        self.revision = authored['revision']
        self.check_dependencies(replacement)
        assert self.world_call('world.inspect')['surface_mark_profiles'] == {
            profile: dict(mask=mask) for profile, mask in replacement.items()}
        with self.owner('textured marks reauthored', saves=saves) as (client, _):
            initial = self.start(client, saves, None)
            fresh = self.step(client, initial, use=True, stamps=stencil_commands(PRIMARY),
                              motion=(STAMPED, QUARTER_TURN_Z, 1))
            self.check_state(fresh, replacement, (PRIMARY, PRIMARY, COMPILED, None))
            old_mask = self.assets/(self.catalog[PRIMARY]+'.pimage')
            withheld = self.root/'withheld original.pimage'
            old_mask.rename(withheld)
            try:
                refusal = self.reject(client, 'save.load', self.restore_request(fresh, uuid4().hex, self.revision), -32070)
                assert exact(self.state(client, fresh['session'], fresh['tick']), fresh)
            finally:
                withheld.rename(old_mask)
            session = uuid4().hex
            restored = client.call('save.load', self.restore_request(fresh, session, self.revision))
            assert restored['source_stale'] is True and restored['authored_revision'] == old_revision, restored
            assert restored['current_authored_revision'] == self.revision, restored
            actual = self.state(client, session, CHECKPOINT_TICK)
            assert exact(actual, checkpoint | dict(session=session))
            assert self.archive(client, saves, actual, 'reauthored-checkpoint') == saved
            continued = self.continue_scene(client, actual)
            assert exact(continued, final | dict(session=session))
            assert self.archive(client, saves, continued, 'reauthored-final') == final_bytes
        self.measurements['replacement_restore'] = dict(original_revision=old_revision,
            replacement_revision=self.revision, replacement_catalog=replacement,
            fresh_mask=fresh['marks'][0]['mask_sha256'], restored_mask=actual['marks'][0]['mask_sha256'],
            missing_original=refusal, checkpoint_exact=True, continued_exact=True)

    def refuse_maskless_export(self):
        copied_runtime = self.root/'Runtime without mask support'
        copied_runtime.mkdir()
        installed = self.args.runtime_root/'runtime.json'
        descriptor = json.loads(installed.read_bytes())
        assert descriptor['features']['surface_mark_masks'] is True, descriptor
        descriptor['features']['surface_mark_masks'] = False
        copy = copied_runtime/installed.name
        copy.write_text(json.dumps(descriptor, indent=2)+'\n', encoding='utf-8')
        output = self.root/'Rejected export'
        command = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                   '--output', self.native(output), '--runtime', self.native(copied_runtime)]
        child = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.calls.append(dict(argv=command, exit=child.returncode, stdout=child.stdout, stderr=child.stderr))
        reply = json.loads(child.stdout)
        assert child.returncode == 4 and reply['status'] == 'error' and reply['command'] == 'project.build', reply
        assert reply['diagnostics'] == [
            dict(code='project.failed', message='Project surface mark masks require a compatible Vulkan runtime.')], reply
        assert not output.exists()
        self.measurements['maskless_export'] = dict(runtime_sha256=file_hash(copy), rejection=reply)

    def run(self):
        self.revision = self.measurements['authored_revision'] = self.author()
        saves = self.root/'External save slots'
        saves.mkdir()
        if not self.args.runtime_root:
            source = tree_hashes(self.project)
            baseline = self.exercise(saves)
            assert tree_hashes(self.project) == source
            self.verify_replacement_restore(saves, baseline)
            return
        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 560, info
        assert info['needs_surface_mark_masks'] is True, info
        self.refuse_maskless_export()
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
        assert {value for path, value in frozen.items() if path.endswith('.pimage')} == set(self.catalog.values())
        assert sum(path.endswith('.pimage') for path in frozen) == 2
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
        parser.error('Supply a Native AOT descriptor or the CoreCLR hostfxr, bridge and assembly.')
    if args.runtime_root and not args.native_descriptor:
        parser.error('Standalone export requires --native-descriptor.')
    if args.gpu is not None and (not args.runtime_root or not 0 <= args.gpu <= 4095):
        parser.error('--gpu requires a standalone runtime and a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='textured-surface-marks-', dir=args.output.resolve()))
    test = TexturedSurfaceMarkContract(args, root)
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
