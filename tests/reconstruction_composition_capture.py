#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify native FSR input composition and replay a relocated game with exact saves."""

import argparse
import json
import math
from pathlib import Path
import traceback

from game_launch_lighting import AA_UI_REGION, CAMERA, PLAYER, REPLAY, VALIDATION_MESSAGES, StandaloneLighting
from hdr_composition_capture import display
from height_fog_capture import optical_depth
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from screen_reflections_capture import (BLUE, CAMERA as PROBE_CAMERA, ENVIRONMENT, EYE, LENS, RED, SETTINGS,
                                        camera_forward, camera_matrix, dot, ideal_gain, project, ray,
                                        witness, witness_pixels)
from texture_fixture import png
from world_particles_contract import RpcError, action, component, counters, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, success, tree_hashes, uid


FOG = dict(density_per_m=.16, base_height_m=.4, height_falloff_per_m=.18,
           start_distance_m=1.8, max_distance_m=11.5,
           ambient_radiance=[.09, .35, .68], anisotropy=0)
MODES = {'none': 1, 'fsr3_native': 1, 'fsr3_quality': 1.5}
FRAMES = 8
FOREGROUND, SPRITE, BLOCKED_SPRITE = (uid(value) for value in (8260, 8261, 8262))
SPRITE_TEXEL = (0, 255, 0, 192)
SPRITE_OPACITY, SPRITE_FADE, SPRITE_SIZE = .75, 1, .7
SPRITE_BURST = 'temporal-sprites'
SPRITE_REPLAY = [dict(ticks=1, actions=[action(SPRITE_BURST, pressed=True)])]
REFLECTED = {'warm_hit': RED.emission, 'cool_hit': BLUE.emission}
HDR_TOLERANCE = .025
DEVICE_VALIDATION = 'Inserted device layer "VK_LAYER_KHRONOS_validation"'


def extent(mode):
    return tuple(int(value/MODES[mode]) for value in CAPTURE_SIZE)


def mapped_pixels(mode):
    width, height = CAPTURE_SIZE
    rw, rh = extent(mode)
    return {key: (x*rw//width, y*rh//height) for key, (x, y) in witness_pixels().items()}


def projected_sample(key, render_point, mode, jitter):
    # The actual jitter is in render pixels. Convert that pixel's center back
    # to the same screen-normalized ray evaluated by the independent box oracle.
    mapped = [(v+.5-offset)*out/size for v, offset, out, size in
              zip(render_point, jitter, CAPTURE_SIZE, extent(mode), strict=True)]
    nominal = witness_pixels()[key]
    equivalent_jitter = tuple(v+.5-s for v, s in zip(nominal, mapped, strict=True))
    return witness(key, nominal, equivalent_jitter)


def intended_hdr(name, raw_hdr, geometry, reflections, fog, ray_y):
    color = raw_hdr
    if reflections and name in REFLECTED:
        color = [value+gain*ideal_gain(geometry) for value, gain in
                 zip(raw_hdr, REFLECTED[name], strict=True)]
    if fog:
        distance = math.dist(geometry['point'], EYE) if geometry['surface'] else FOG['max_distance_m']
        transmission = math.exp(-optical_depth(FOG, EYE[1], ray_y, distance))
        color = [c*transmission+a*(1-transmission)
                 for c, a in zip(color, FOG['ambient_radiance'], strict=True)]
    return color


def raster_point(point, mode, jitter):
    return tuple((v+.5-offset)*out/size for v, offset, out, size in
                 zip(point, jitter, CAPTURE_SIZE, extent(mode), strict=True))


def raster_ray_y(point, mode, jitter):
    return ray(*raster_point(point, mode, jitter))[1]


def sprite_cards():
    """Place camera-facing cards half a metre ahead of and behind known opaque hits."""
    cards = {}
    for site, entity, offset in (('warm_target', SPRITE, -.5), ('cool_target', BLOCKED_SPRITE, .5)):
        pixel = witness_pixels()[site]
        receiver = witness(site, pixel, (0, 0))
        depth = receiver['camera_depth_m']+offset
        direction = ray(*(value+.5 for value in pixel))
        distance = depth/dot(direction, camera_forward())
        center = list(origin+axis*distance for origin, axis in zip(EYE, direction, strict=True))
        cards[site] = dict(entity=entity, center=center, depth=depth)
    return cards


def sprite_alpha(cards, site, point, mode, jitter, geometry):
    raster = raster_point(point, mode, jitter)
    focal = CAPTURE_SIZE[1]/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    for target, card in cards.items():
        center = project(card['center'])
        half_size = SPRITE_SIZE*focal/(2*card['depth'])
        margin = half_size-max(abs(a-b) for a,b in zip(raster, center, strict=True))
        # The output probe is near the card center. FSR's filter footprint
        # fits inside the constant-texel interior or stays outside the card.
        assert margin > 5 if target == site else margin < -5, (site, target, margin)
    if site not in cards:
        return 0
    gap = geometry['camera_depth_m']-cards[site]['depth']
    return SPRITE_TEXEL[3]/255*SPRITE_OPACITY*max(0, min(1, gap/SPRITE_FADE))


def with_sprite(background, alpha):
    # The fixture texture's sRGB channels are at endpoints and decode to
    # exactly (0,1,0). The fragment shader blends its premultiplied color.
    return [channel*(1-alpha)+color*alpha for channel, color in zip(background, (0, 1, 0), strict=True)]


def verify_reconstruction(diag, mode, *, reflections, fog, mask, lighting='deferred', sprites=False):
    assert diag['lighting_path'] == lighting and 'post_aa' not in diag, diag
    rw, rh = extent(mode)
    for effect, reason, stride in (('screen_reflections', reflections, 10), ('height_fog', fog, 8)):
        if reason is None:
            assert effect not in diag, diag
        else:
            detail = diag[effect]
            assert detail['reason'] == reason and detail['active'] is (reason == 'active'), detail
            assert detail['buffer_bytes'] == (rw*rh*stride if detail['active'] else 0), detail
    reconstruction = diag['reconstruction']
    assert reconstruction['mode'] == mode and reconstruction['active'] is (mode != 'none'), reconstruction
    if mode != 'none':
        assert (reconstruction['render_width'], reconstruction['render_height']) == (rw, rh), reconstruction
        assert (reconstruction['output_width'], reconstruction['output_height']) == CAPTURE_SIZE, reconstruction
        assert all(math.isfinite(value) and abs(value) <= .5 for value in reconstruction['jitter_pixels'])
    if mask:
        assert reconstruction['composition_mask_bytes'] == rw*rh, reconstruction
    else:
        assert 'composition_mask_bytes' not in reconstruction, reconstruction
    sprite_bytes = rw*rh*9 if sprites and mode != 'none' else 0
    assert reconstruction.get('sprite_radiance_bytes', 0) == sprite_bytes, reconstruction
    draws = diag['last_draws']
    assert (draws['sprite_instances'], draws['sprite_batches']) == ((2, 1) if sprites else (0, 0)), draws
    return reconstruction


class CompositionCapture(StandaloneLighting):
    def author_profile(self, label, revision, request, reflection, fog, *ops):
        environment = dict(ambient=[.23]*3, exposure=1)
        if reflection is not None:
            environment['screen_reflections'] = reflection
        if fog is not None:
            environment['height_fog'] = fog
        revision = self.transact(revision, request, component(ENVIRONMENT, 'LightingEnvironment', environment), *ops)
        self.measurements['profiles'][label] = dict(revision=revision, lighting=environment)
        return revision

    def capture(self, client, label, revision, session, mode, reflection, fog, *, lighting='deferred',
                camera=CAMERA, tick=0, sprites=False):
        output = self.root/f'{label}-{lighting}-{mode}.bmp'
        ui = client.call('runtime.ui.inspect', dict(session_id=session, tick=tick))
        coords = mapped_pixels(mode)
        frames = 2 if mode == 'none' else FRAMES
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=camera,
            ui_revision=ui['ui_revision'], ui_scale=1, path=self.native(output), width=CAPTURE_SIZE[0],
            height=CAPTURE_SIZE[1], gpu=self.args.gpu, samples=1, lighting_path=lighting,
            reconstruction=mode, capture_frames=frames, scene_product_probes=[dict(x=x, y=y) for x,y in coords.values()]))
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors'] == 0
        assert report['revision'] == revision and report['tick'] == tick and report['lens'] == LENS, report
        assert (report['width'], report['height'], report['samples'], report['frames_presented']) == (*CAPTURE_SIZE, 1, frames)
        near_vector(report['camera_world'], camera_matrix(), tolerance=1e-8)
        assert self.measurements.setdefault('gpu_name', report['gpu']) == report['gpu'], report
        diag = report['render_diagnostics']
        mask = mode != 'none' and 'active' in (reflection, fog)
        rec = verify_reconstruction(diag, mode, reflections=reflection, fog=fog, mask=mask, lighting=lighting,
                                    sprites=sprites)
        jitter = rec['jitter_pixels'] if mode != 'none' else (0, 0)
        if mode != 'none':
            assert rec['history_sequence'] == frames and rec['history_reset'] is False, rec
        if 'world_depth_foreground' in (reflection, fog):
            receipt = diag['scene_receipts']['presented']['camera_draw']
            assert receipt['viewmodel_world_depth'] is True
            assert [row['entity_id'] for row in receipt['viewmodels']] == [FOREGROUND], receipt
        image = pixels(output)
        assert len(image) == CAPTURE_SIZE[1] and all(len(row) == CAPTURE_SIZE[0] for row in image)
        products = diag['scene_products']
        assert products['available'] is True and products['view'] == 'color', products
        focused = {}
        for (key, point), sample in zip(coords.items(), products['probes'], strict=True):
            x, y = (int((v+.5)*out/size) for v, out, size in zip(point, CAPTURE_SIZE, extent(mode), strict=True))
            assert (sample['x'], sample['y'], sample['resolved_x'], sample['resolved_y']) == (*point, x, y), sample
            geometry = projected_sample(key, point, mode, jitter)
            assert sample['surface_valid'] is bool(geometry['surface']), (key, sample, geometry)
            if geometry['surface']:
                n, f = LENS['near'], LENS['far']
                view_depth = n/(1-sample['depth']*(f-n)/f)
                assert abs(view_depth-geometry['camera_depth_m']) < .0005, (key, sample, geometry)
                near_vector(sample['shading_normal'][:3], geometry['normal'], tolerance=.002)
            expected = intended_hdr(key, sample['raw_hdr'], geometry, reflection == 'active',
                                    fog == 'active', raster_ray_y(point, mode, jitter))
            alpha = sprite_alpha(self.cards, key, point, mode, jitter, geometry) if sprites else 0
            expected = with_sprite(expected, alpha)
            near_vector(sample['resolved_hdr'], expected, tolerance=HDR_TOLERANCE)
            rgb = image[y][x]
            expected_display = [display(max(0, value)) for value in sample['resolved_hdr']]
            assert max(abs(a-b) for a,b in zip(expected_display, rgb, strict=True)) <= 2, (key, sample, rgb)
            focused[key] = dict(sample=sample, independent_hdr=expected, rgb=rgb)
            if sprites:
                focused[key]['independent_sprite_alpha'] = alpha
        left, top, right, bottom = AA_UI_REGION
        interface = [pixel for row in image[top:bottom] for pixel in row[left:right]]
        assert len(set(interface)) >= 6
        assert min(pixel[0] for pixel in interface) < 70 and max(pixel[0] for pixel in interface) > 190
        result = dict(sha256=file_hash(output), focused=focused, interface=interface, jitter=jitter)
        self.measurements['captures'][f'{label}/{lighting}/{mode}'] = dict(path=str(output), result=report,
            focused=focused, sha256=result['sha256'])
        return result

    def authored_captures(self, label, revision, reflection, fog, variants):
        outputs = {}
        world_hash = file_hash(self.world)
        with self.owner(label+' authored') as (client, _):
            session = uid(8600)
            started = client.call('runtime.start', dict(session_id=session, revision=revision))
            assert started['tick'] == 0 and started['authored_revision'] == revision, started
            before = client.call('runtime.inspect', dict(session_id=session))
            if not self.args.headless_preflight:
                for lighting, mode in variants:
                    outputs[(lighting, mode)] = self.capture(client, label, revision, session, mode,
                        'forward' if reflection == 'active' and lighting == 'forward' else reflection,
                        fog, lighting=lighting)
            after = client.call('runtime.inspect', dict(session_id=session))
            assert exact(before, after), (before, after)
        assert file_hash(self.world) == world_hash
        return outputs

    def compare(self, images):
        observations = self.measurements['comparisons']
        for lighting, mode in (('deferred', 'fsr3_native'), ('deferred', 'fsr3_quality')):
            key = lighting, mode
            base, zero, reflections, fog, combined = (images[name][key] for name in
                ('omitted', 'zero', 'reflections', 'fog', 'combined'))
            assert exact(base, zero), mode
            assert len({frame['sha256'] for frame in (base, reflections, fog, combined)}) == 4, mode
            assert all(exact(base['interface'], frame['interface']) for frame in (reflections, fog, combined)), mode
            for site in base['focused']:
                probes = [frame['focused'][site]['sample'] for frame in (base, reflections, fog, combined)]
                # Retain every full probe receipt. The only differing HDR field is the
                # reconstructed result, because the camera and opaque scene are fixed.
                invariant = {k:v for k,v in probes[0].items() if k != 'resolved_hdr'}
                assert all(exact(invariant, {k:v for k,v in row.items() if k != 'resolved_hdr'}) for row in probes)
                before, reflected, hazed, both = (row['resolved_hdr'] for row in probes)
                if site in REFLECTED:
                    channel = max(range(3), key=REFLECTED[site].__getitem__)
                    assert reflected[channel]-before[channel] > .075, (site, probes)
                    assert both[channel]-hazed[channel] > .025, (site, probes)
                else:
                    near_vector(before, reflected, tolerance=.01)
                    near_vector(hazed, both, tolerance=.01)
                observations[f'{mode}/{site}'] = dict(reflected_change=math.dist(before, reflected),
                    fog_change=math.dist(before, hazed), reflection_through_fog=math.dist(hazed, both))
            assert observations[f'{mode}/clear']['fog_change'] > .15
        key = 'forward', 'fsr3_quality'
        assert images['omitted'][key]['sha256'] != images['fog'][key]['sha256']
        assert images['omitted'][key]['interface'] == images['fog'][key]['interface']
        key = 'deferred', 'fsr3_quality'
        assert exact(images['foreground-omitted'][key], images['foreground-combined'][key])
        key = 'deferred', 'none'
        assert exact(images['omitted'][key], images['zero'][key])

    def legacy_sprite_rejection(self, revision):
        path = self.project/'one-pixel.png'
        path.write_bytes(png(1, 1, [255, 255, 255, 255]))
        texture = self.world_call('asset.image.import', source=path.name, color_space='srgb')['asset']
        renderable = dict(spawn_space='world', simulation_space='world', lifetime_ticks=12,
            size_m=.25, linear_rgb=[1, 1, 1], sprite=dict(texture=texture), particles_per_second=30)
        revision = self.transact(revision, 8268, dict(op='entity.create', id=SPRITE, name='Textured sprite'),
                                 component(SPRITE, 'ParticleEmitter', renderable))
        with self.owner('textured sprite') as (client, _):
            session = uid(8601)
            client.call('runtime.start', dict(session_id=session, revision=revision))
            assert client.call('runtime.particles.inspect', dict(session_id=session, tick=0))['active']
            if self.args.headless_preflight:
                return revision
            ui = client.call('runtime.ui.inspect', dict(session_id=session, tick=0))
            previous = client.call('runtime.inspect', dict(session_id=session))
            output = self.root/'unpublished-sprite-fsr.bmp'
            params = dict(session_id=session, tick=0, ui_revision=ui['ui_revision'], camera=CAMERA,
                          path=self.native(output), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1], gpu=self.args.gpu,
                          samples=1, lighting_path='deferred', reconstruction='fsr3_quality')
            try:
                client.call('runtime.capture', params)
            except RpcError as error:
                assert error.code == -32020 and 'Textured particle sprites require one world sample' in str(error), error
            else:
                raise AssertionError('Textured particle sprites unexpectedly accepted FSR.')
            assert not output.exists()
            assert exact(previous, client.call('runtime.inspect', dict(session_id=session)))
        return revision

    def author_sprites(self, revision):
        path = self.project/'test-sprite.png'
        path.write_bytes(png(1, 1, list(SPRITE_TEXEL)))
        image = self.world_call('asset.image.import', source=path.name, color_space='srgb')
        assert image['color_space'] == 'srgb' and image['mips'][0]['width'] == image['mips'][0]['height'] == 1, image
        self.sprite_texture = image['asset']
        self.cards = sprite_cards()
        self.measurements['sprites'] = dict(texture=image, texel=SPRITE_TEXEL,
                                           opacity=SPRITE_OPACITY, depth_fade_m=SPRITE_FADE, cards=self.cards,
                                           pairs={})
        ops = []
        for site, card in self.cards.items():
            entity = card['entity']
            renderable = dict(spawn_space='world', simulation_space='world', lifetime_ticks=12,
                size_m=SPRITE_SIZE, linear_rgb=[1, 1, 1], burst=dict(action=SPRITE_BURST, count=1),
                sprite=dict(texture=self.sprite_texture, opacity=SPRITE_OPACITY, depth_fade_m=SPRITE_FADE))
            ops.extend([dict(op='entity.create', id=entity, name=f'Sprite {site}'),
                        component(entity, 'Transform', transform(card['center'])),
                        component(entity, 'ParticleEmitter', renderable)])
        return self.transact(revision, 8268, *ops)

    def step_burst(self, client, session, request):
        controls = [dict(entity=PLAYER, actions=SPRITE_REPLAY[0]['actions'])]
        result = client.call('runtime.step', dict(session_id=session, expected_tick=0, request_id=uid(request),
                                                  ticks=SPRITE_REPLAY[0]['ticks'], inputs=controls))
        assert result['tick'] == 1, result
        return result

    def verify_particle_state(self, client, session, tick):
        observed = self.observed(client, session, tick)
        assert tick in (0, 1) and observed['counts'] == counters(2*tick, 2*tick), observed
        by_source = {source['id']: source['entity'] for source in observed['sources']}
        assert set(by_source.values()) == {card['entity'] for card in self.cards.values()}, by_source
        assert len(observed['particles']) == 2*tick, observed
        cards = {card['entity']: card for card in self.cards.values()}
        for particle in observed['particles']:
            card = cards.pop(by_source[particle['emitter']])
            assert particle['age_ticks'] == 0, particle
            near_vector(particle['position_m'], card['center'], tolerance=2e-6)
            near_vector(particle['velocity_m_s'], [0, 0, 0])
            style = particle['presentation']
            assert style['texture'] == self.sprite_texture and style['texel_rect'] == [0, 0, 1, 1], style
            assert style['visible'] and math.isclose(style['opacity'], SPRITE_OPACITY), style
        assert len(cards) == 2*(1-tick)
        return observed

    def qualify_sprites(self, label, revision, reason):
        world_hash = file_hash(self.world)
        phases = {}
        states = {}
        with self.owner(label+' authored') as (client, _):
            session = uid(8602)
            started = client.call('runtime.start', dict(session_id=session, revision=revision))
            assert started['tick'] == 0 and started['authored_revision'] == revision, started
            for tick, stage in ((0, 'empty'), (1, 'live')):
                if tick:
                    self.step_burst(client, session, 8610)
                states[stage] = self.verify_particle_state(client, session, tick)
                before = client.call('runtime.inspect', dict(session_id=session))
                if not self.args.headless_preflight:
                    phases[stage] = {mode: self.capture(client, f'{label}-{stage}', revision, session, mode,
                        reason, reason, camera=PROBE_CAMERA, tick=tick, sprites=bool(tick))
                        for mode in MODES if mode != 'none'}
                assert exact(before, client.call('runtime.inspect', dict(session_id=session)))
                assert exact(states[stage], self.verify_particle_state(client, session, tick))
        assert file_hash(self.world) == world_hash
        pair = self.measurements['sprites']['pairs'][label] = dict(states=states, observations={})
        if not self.args.headless_preflight:
            for mode in phases['live']:
                pair['observations'][mode] = self.compare_sprites(label, mode, phases['empty'][mode], phases['live'][mode])

    def compare_sprites(self, label, mode, empty, live):
        assert empty['sha256'] != live['sha256'] and exact(empty['interface'], live['interface']), mode
        width, height = extent(mode)
        before_rec, after_rec = (self.measurements['captures'][f'{label}-{stage}/deferred/{mode}']['result']
                                 ['render_diagnostics']['reconstruction'] for stage in ('empty', 'live'))
        assert after_rec['logical_bytes']-before_rec['logical_bytes'] == width*height*9, (before_rec, after_rec)
        observations = {}
        for name, before in empty['focused'].items():
            after = live['focused'][name]
            left, right = before['sample'], after['sample']
            assert exact({k: v for k,v in left.items() if k != 'resolved_hdr'},
                         {k: v for k,v in right.items() if k != 'resolved_hdr'}), (mode, name, left, right)
            alpha = after['independent_sprite_alpha']
            if name == 'warm_target':
                assert .25 < alpha < .32 and right['resolved_hdr'][1]-left['resolved_hdr'][1] > .15, (left,right,alpha)
                assert after['rgb'][1]-before['rgb'][1] > 12, (before, after)
            else:
                assert alpha == 0
                near_vector(left['resolved_hdr'], right['resolved_hdr'], tolerance=.01)
                assert max(abs(a-b) for a,b in zip(before['rgb'], after['rgb'], strict=True)) <= 2, (before, after)
            observations[name] = dict(alpha=alpha, before_hdr=left['resolved_hdr'], after_hdr=right['resolved_hdr'],
                                      before_rgb=before['rgb'], after_rgb=after['rgb'])
        return observations

    def read_save(self, storage):
        data, saved = super().read_save(storage)
        if self.sprite_texture is not None:
            extension = json.loads(data)['snapshot']['payload']['particles']
            assert extension['version'] == 1
            assert {row['entity'] for row in extension['sources']} == {card['entity'] for card in self.cards.values()}
            state = extension['state']
            assert state['status']['tick'] == 1 and state['status']['particles'] == counters(2, 2), state
            for row, card in zip(state['particles'], self.cards.values(), strict=True):
                assert row['age_ticks'] == 0, row
                near_vector(row['position_m'], card['center'], tolerance=2e-6)
        return data, saved

    def headless_baseline(self, label):
        if self.sprite_texture is None:
            return super().headless_baseline(label)
        storage = self.root/(label+' saves')
        storage.mkdir()
        with self.owner(label, game=self.game, saves=storage) as (client, _):
            status = client.call('runtime.status')
            assert status['active'] is True and status['tick'] == 0
            session = status['session_id']
            assert self.step_burst(client, session, 8302)['save_serviced']
            sprites = self.verify_particle_state(client, session, 1)
            report = dict(runtime=client.call('runtime.inspect', dict(session_id=session)),
                entities={entity: client.call('runtime.entity', dict(session_id=session, id=entity))
                          for entity in (PLAYER, CAMERA)},
                gameplay=client.call('runtime.gameplay.inspect', dict(session_id=session, include_schema=True)))
            terminal = self.inspect_terminal(report, session)
        data, saved = self.read_save(storage)
        self.measurements[label] = dict(saved=saved, terminal=terminal, sprites=sprites)
        self.check_immutable()
        return data, terminal

    def capture_relocated_sprites(self, revision):
        saves = self.root/'relocated sprite capture saves'
        saves.mkdir()
        with self.owner('relocated sprite captures', game=self.game, saves=saves) as (client, _):
            status = client.call('runtime.status')
            assert status['active'] is True and status['tick'] == 0
            session = status['session_id']
            assert self.step_burst(client, session, 8303)['save_serviced']
            state = self.verify_particle_state(client, session, 1)
            assert exact({k: v for k,v in state.items() if k != 'session_id'},
                         {k: v for k,v in self.measurements['headless']['sprites'].items() if k != 'session_id'})
            before = client.call('runtime.inspect', dict(session_id=session))
            for mode in ('fsr3_native', 'fsr3_quality'):
                relocated = self.capture(client, 'relocated-sprites-live', revision, session, mode, 'active', 'active',
                                         camera=PROBE_CAMERA, tick=1, sprites=True)
                authored = self.measurements['captures'][f'sprites-combined-live/deferred/{mode}']
                assert relocated['sha256'] == authored['sha256'], (mode, relocated['sha256'], authored['sha256'])
                assert exact(relocated['focused'], authored['focused']), mode
            assert exact(before, client.call('runtime.inspect', dict(session_id=session)))
            assert exact(state, self.verify_particle_state(client, session, 1))
        data, saved = self.read_save(saves)
        assert data == self.baseline[0]
        self.measurements['sprites']['relocated_capture_save'] = saved

    def verify_replay_sprites(self, image_path, mode):
        image = pixels(image_path)
        # This capture follows the live birth and retains actual FSR history.
        # Use the converged analytic-color check in capture() and a visible delta
        # here to allow the already-tested SDK its first-frame history response.
        key = f'sprites-combined-empty/deferred/{"fsr3_native" if mode == "none" else mode}'
        control = self.measurements['captures'][key]['focused']
        values = {}
        for label in ('warm_target', 'cool_target'):
            point = mapped_pixels(mode)[label]
            x, y = (int((v+.5)*out/size) for v, out, size in zip(point, CAPTURE_SIZE, extent(mode), strict=True))
            rgb = image[y][x]
            before = control[label]['rgb']
            if label == 'warm_target':
                assert rgb[1]-before[1] > 12, (mode, x, y, rgb, before)
            else:
                assert max(abs(a-b) for a,b in zip(rgb, before, strict=True)) <= 6, (mode, x, y, rgb, before)
            values[label] = dict(pixel=[x, y], rgb=rgb, empty_rgb=before)
        return values

    def actual_replay(self, mode):
        output = self.root/mode
        output.mkdir()
        saves = output/'saves'
        saves.mkdir()
        report, bmp = output/'report.json', output/'frame.bmp'
        result = success(json.loads(self.cli('game', 'run', self.native(self.game), '--replay', self.native(self.replay),
            '--save-root', self.native(saves), '--report', self.native(report), '--capture', self.native(bmp),
            '--gpu', self.args.gpu, '--width', CAPTURE_SIZE[0], '--height', CAPTURE_SIZE[1], '--lighting-path', 'deferred',
            '--reconstruction', mode, '--settings-profile', self.native(self.profile), '--settings-overrides', '{"ui.scale":1}',
            executable=self.executable, cwd=self.launch_directory)))
        assert exact(json.loads(report.read_bytes()), result) and result['success'], result
        play = result['play']
        assert play['hardware'] and play['capture_written'] and play['nvrhi_errors'] == 0 and play['frames_presented'] >= 2, play
        assert play['stop_reason'] == 'replay_complete' and play['tick'] == 1, play
        assert play['gpu'] == self.measurements['gpu_name'], play
        assert (play['width'], play['height'], play['samples']) == (*CAPTURE_SIZE, 1), play
        sprites = self.sprite_texture is not None
        rec = verify_reconstruction(play['render_diagnostics'], mode, reflections='active', fog='active',
                                    mask=mode != 'none', sprites=sprites)
        saved_bytes, saved = self.read_save(saves)
        assert saved_bytes == self.baseline[0]
        assert exact(self.inspect_terminal(result, play['current_session_id']), self.baseline[1])
        self.check_immutable()
        measurement = dict(report_sha256=file_hash(report), image_sha256=file_hash(bmp), saved=saved,
                           reconstruction=rec, presented=play['frames_presented'])
        if sprites:
            measurement['sprite_pixels'] = self.verify_replay_sprites(bmp, mode)
        self.measurements['replays'][mode] = measurement

    def qualify_authoring(self, revision):
        captures = {}
        standard = [('deferred', 'fsr3_native'), ('deferred', 'fsr3_quality')]
        families = [
            ('omitted', None, None, None, None, [('deferred', 'none'), ('forward', 'fsr3_quality')]),
            ('zero', SETTINGS | dict(strength=0), FOG | dict(density_per_m=0), 'zero_strength', 'zero_density', [('deferred', 'none')]),
            ('reflections', SETTINGS, None, 'active', None, []),
            ('fog', None, FOG, None, 'active', [('forward', 'fsr3_quality')]),
            ('combined', SETTINGS, FOG, 'active', 'active', []),
        ]
        for index, (label, reflection, fog, reflection_reason, fog_reason, extra) in enumerate(families):
            revision = self.author_profile(label, revision, 8670+index, reflection, fog)
            captures[label] = self.authored_captures(label, revision, reflection_reason, fog_reason, standard+extra)
        projection = LENS | dict(viewmodel=LENS, viewmodel_world_depth=True)
        revision = self.author_profile('foreground-omitted', revision, 8675, None, None,
            component(CAMERA, 'Camera', projection), dict(op='entity.create', id=FOREGROUND, name='Depth foreground', parent=CAMERA),
            component(FOREGROUND, 'Transform', transform([1.4, .22, -2.1]) | dict(scale=[.18]*3)),
            component(FOREGROUND, 'MeshRenderer', dict(primitive='box', albedo=[1, 0, 1], visible=True, cast_shadows=False)),
            component(FOREGROUND, 'CameraDraw', dict(camera=CAMERA, mode='viewmodel')))
        quality = [('deferred', 'fsr3_quality')]
        captures['foreground-omitted'] = self.authored_captures('foreground-omitted', revision, None, None, quality)
        revision = self.author_profile('foreground-combined', revision, 8676, SETTINGS, FOG)
        captures['foreground-combined'] = self.authored_captures('foreground-combined', revision, 'world_depth_foreground',
                                                              'world_depth_foreground', quality)
        if not self.args.headless_preflight:
            self.compare(captures)
        revision = self.transact(revision, 8677, dict(op='entity.delete', id=FOREGROUND, recursive=False),
                                 component(CAMERA, 'Camera', LENS))
        if self.args.legacy_sprite_refusal:
            revision = self.legacy_sprite_rejection(revision)
            return self.transact(revision, 8678, dict(op='entity.delete', id=SPRITE, recursive=False))
        revision = self.author_sprites(revision)
        revision = self.author_profile('sprites-only', revision, 8680, None, None)
        self.qualify_sprites('sprites-only', revision, None)
        revision = self.author_profile('sprites-combined', revision, 8681, SETTINGS, FOG)
        self.qualify_sprites('sprites-combined', revision, 'active')
        return revision

    def run(self):
        self.measurements.update(bundles={}, profiles={}, captures={}, comparisons={}, replays={})
        self.sprite_texture = None
        revision = self.qualify_authoring(self.author_fixture())
        self.original_bundle = self.root/'enabled original export'
        self.game = self.bundle('enabled', SETTINGS, 1, revision, 8679, height_fog=FOG)
        revision = self.measurements['bundles']['enabled']['revision']
        descriptor = json.loads(self.game.read_bytes())
        self.executable = (self.game.parent/descriptor['engine']['executable']).resolve()
        assert descriptor['entry']['controller'] == PLAYER and descriptor['entry']['camera'] == CAMERA
        assert file_hash(self.executable) == file_hash(self.args.binary)
        info = success(json.loads(self.cli('game', 'inspect', self.native(self.game), executable=self.executable)))
        assert info['needs_screen_reflections'] is True and info['needs_height_fog'] is True, info
        if self.sprite_texture is not None:
            closure = self.world_call('world.dependencies')
            assert closure['needs_sprite_particles'] is True, closure
            paths = self.measurements['bundles']['enabled']['inventory']
            texture, = [path for path in paths if Path(path).name == self.sprite_texture+'.pimage']
            assert paths[texture] == self.sprite_texture, paths[texture]
        self.detached_source = self.root/'Unavailable composed authoring'
        self.project.rename(self.detached_source)
        self.measurements['source_inventory'] = tree_hashes(self.detached_source)
        self.replay = self.root/'one-tick.replay.json'
        self.replay.write_text(json.dumps(REPLAY if self.sprite_texture is None else SPRITE_REPLAY)+'\n', encoding='utf-8')
        self.measurements['replay_sha256'] = file_hash(self.replay)
        self.launch_directory = self.root/'unrelated working directory'
        self.launch_directory.mkdir()
        self.baseline = self.headless_baseline('headless')
        if self.args.headless_preflight:
            assert self.headless_baseline('headless-repeat') == self.baseline
            return
        captures_saves = self.root/'relocated capture saves'
        captures_saves.mkdir()
        with self.owner('relocated composed captures', game=self.game, saves=captures_saves) as (client, _):
            session = client.call('runtime.status')['session_id']
            before = client.call('runtime.inspect', dict(session_id=session))
            for mode in ('fsr3_native', 'fsr3_quality'):
                relocated = self.capture(client, 'relocated', revision, session, mode, 'active', 'active')
                authored = self.measurements['captures'][f'combined/deferred/{mode}']
                assert relocated['sha256'] == authored['sha256'], (mode, relocated['sha256'], authored['sha256'])
            assert exact(before, client.call('runtime.inspect', dict(session_id=session)))
        assert not list(captures_saves.iterdir())
        if self.sprite_texture is not None:
            self.capture_relocated_sprites(revision)
        self.check_immutable()
        for mode in MODES:
            self.actual_replay(mode)
        assert len({value['image_sha256'] for value in self.measurements['replays'].values()}) == len(MODES)
        gpu_labels = {name+' authored' for name in self.measurements['profiles']}
        gpu_labels.update(('textured sprite', 'relocated composed captures', 'relocated sprite captures'))
        validated = 0
        for call in self.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in VALIDATION_MESSAGES), (call.get('label', call['argv']), stderr)
            if call.get('label') in gpu_labels or call['argv'][1:3] == ['game', 'run']:
                if DEVICE_VALIDATION in stderr:
                    validated += 1
                elif self.args.require_validation:
                    raise AssertionError(('Khronos device validation was not observed', call.get('label', call['argv'])))
        self.measurements['validation_owners'] = validated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('runtime-root', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true', help='Require Vulkan loader device-layer diagnostics.')
    parser.add_argument('--headless-preflight', action='store_true', help='Run source and exported save checks without Vulkan.')
    parser.add_argument('--legacy-sprite-refusal', action='store_true', help='Expect the 0.0.395 renderer to refuse textured sprites with FSR.')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Assertions and a valid GPU index are required.')
    args.fxaa = True  # Reuse the authored opaque UI and emissive diagonal fixture.
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    fixture = CompositionCapture(args, args.output)
    record = dict(format='poima.reconstruction-composition-capture.v1', passed=False, gpu=args.gpu,
                  headless_only=args.headless_preflight, legacy_sprite_refusal=args.legacy_sprite_refusal,
                  binary_sha256=file_hash(args.binary),
                  artifact_sha256=file_hash(args.artifact), runtime_descriptor_sha256=file_hash(args.runtime_root/'runtime.json'),
                  source_sha256={name:file_hash(Path(__file__).with_name(name)) for name in (
                      'reconstruction_composition_capture.py', 'game_launch_lighting.py', 'screen_reflections_capture.py', 'height_fog_capture.py')},
                  calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = args.output/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
