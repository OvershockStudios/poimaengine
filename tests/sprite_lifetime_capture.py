#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored sprite curves and flipbooks through saved state and Vulkan HDR."""
import argparse
import json
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from scene_capture import pixels
from sprite_particles_capture import BASE, CAMERA, HEIGHT, LENS, WIDTH, point
from texture_fixture import png
from world_particles_contract import ParticleContract, component, counters, inputs, transform
from world_terrain_contract import error, file_hash, rpc, success, tree_hashes, uid


BURST = 'sprite-lifetime'
TEMPLATE, TEMPLATE_BURST = uid(110), 'template-lifetime'
BIRTH, LIFETIME = 3, 8
CHECKPOINT, FINAL = 3, 6  # Particle ages.
EMITTERS = {'loop': (54, 64), 'hold': (158, 64), 'static': (54, 166), 'opaque': (158, 166)}
BASE_SIZE, DEPTH = .5, 2
RED, GREEN, BLUE, CYAN, YELLOW = (1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 1, 1), (1, 1, 0)
FRAME_TEXELS = ((RED, RED), (GREEN, CYAN), (BLUE, BLUE), (YELLOW, YELLOW))
FRAME_RECTS = ([0, 0, 2, 2], [2, 0, 2, 2], [0, 2, 2, 2], [2, 2, 2, 2])
LOOP = dict(columns=2, rows=2, first_frame=1, frame_count=3, ticks_per_frame=2)
HOLD = dict(columns=2, rows=2, frame_count=2, ticks_per_frame=1, loop=False)
SIZE_CURVE = [[0, 1], [.25, .5], [.5, 1], [.625, 0], [.75, .5], [1, 1]]
OPACITY_CURVE = [[0, .5], [.5, 1], [1, .5]]
HOLD_OPACITY = [[0, 1], [.125, .75], [.25, .5], [.5, 0], [.625, .25], [.75, .5], [.875, .75], [1, 1]]
# Explicit oracle at each live tick. Entries are (atlas frame, size in metres,
# effective opacity), independent of the engine's interpolation and addressing.
LOOP_LIFE = ((1, .5, .25), (1, .375, .3125), (2, .25, .375), (2, .375, .4375),
             (3, .5, .5), (3, 0, .4375), (1, .25, .375), (1, .375, .3125))
HOLD_LIFE = ((0, .5, .5), (1, .5, .375), (1, .5, .25), (1, .5, .125),
             (1, .5, 0), (1, .5, .125), (1, .5, .25), (1, .5, .375))
CAPTURES = {-1: 'closed', 0: 'birth', CHECKPOINT: 'checkpoint', 4: 'zero-opacity',
            5: 'zero-size', FINAL: 'wrapped', LIFETIME: 'expired'}


def atlas_png():
    channels = []
    for row in range(2):
        left, right = FRAME_TEXELS[row*2:row*2+2]
        strip = [(*[int(channel*255) for channel in rgb], 255) for rgb in (*left, *right)]
        channels.extend(channel for texel in strip*2 for channel in texel)
    return png(4, 4, channels)


def lifetime(name, age):
    if name == 'static':
        return None, BASE_SIZE, 1
    return (LOOP_LIFE if name == 'loop' else HOLD_LIFE)[age]


def sprite_presentation(texture, name, age):
    frame, size, opacity = lifetime(name, age)
    rect = [0, 0, 4, 4] if frame is None else FRAME_RECTS[frame]
    return dict(texture=texture, size_m=size, opacity=opacity, texel_rect=rect, frame=frame,
                visible=size > 0 and opacity > 0)


def blend(rgb, opacity):
    return tuple(color*opacity + background*(1-opacity) for color, background in zip(rgb, BASE, strict=True))


def probe_points(age):
    active = 0 <= age < LIFETIME
    loop_frame, size, loop_opacity = LOOP_LIFE[age] if active else (1, BASE_SIZE, 0)
    hold_frame, _, hold_opacity = HOLD_LIFE[age] if active else (0, BASE_SIZE, 0)
    loop_left, loop_right = FRAME_TEXELS[loop_frame]
    loop_middle = tuple((a+b)/2 for a, b in zip(loop_left, loop_right, strict=True))
    hold_left, hold_right = FRAME_TEXELS[hold_frame]
    hold_middle = tuple((a+b)/2 for a, b in zip(hold_left, hold_right, strict=True))
    x, y = EMITTERS['loop']
    hx, hy = EMITTERS['hold']
    sx, sy = EMITTERS['static']
    # At 2 m, a one-metre card spans 64 pixels. A point one pixel inside the
    # edge uses both sides of that edge in an unclamped bilinear footprint.
    extent = max(1, int(size*32)-1)
    loop_opacity = loop_opacity if size else 0
    return {
        'loop_center': ((x, y), blend(loop_middle, loop_opacity)),
        'loop_left': ((x-extent, y), blend(loop_left, loop_opacity)),
        'loop_right': ((x+extent, y), blend(loop_right, loop_opacity)),
        'loop_top': ((x, y-extent), blend(loop_middle, loop_opacity)),
        'loop_bottom': ((x, y+extent), blend(loop_middle, loop_opacity)),
        'loop_fixed_radius': ((x+13, y), blend(loop_right, loop_opacity) if size == BASE_SIZE else BASE),
        'hold_center': ((hx, hy), blend(hold_middle, hold_opacity)),
        'hold_right': ((hx+15, hy), blend(hold_right, hold_opacity)),
        'static_full_image': ((sx-12, sy-12), RED if active else BASE),
        'opaque': (EMITTERS['opaque'], BLUE if active else BASE),
    }


class SpriteLifetimeCapture(ParticleContract):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Sprite Lifetime Contract')))
        path = self.project/'lifetime-atlas.png'
        path.write_bytes(atlas_png())
        receipt = self.world_call('asset.image.import', source=path.name, color_space='srgb')
        self.texture = receipt['asset']
        assert receipt['color_space'] == 'srgb' and (receipt['mips'][0]['width'], receipt['mips'][0]['height']) == (4, 4)
        assert self.world_call('asset.image.inspect', asset=self.texture) == receipt
        source = path.rename(self.root/path.name)
        cooked = self.assets/(self.texture+'.pimage')
        assert file_hash(cooked) == self.texture and cooked.stat().st_size == receipt['bytes']
        self.measurements['texture'] = dict(receipt=receipt, source=str(source), source_sha256=file_hash(source))

        backdrop = uid(8)
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1)),
               dict(op='entity.create', id=CAMERA, name='Fixed probe camera'),
               component(CAMERA, 'Transform', transform([0, 0, 0])), component(CAMERA, 'Camera', LENS),
               dict(op='entity.create', id=backdrop, name='Constant backdrop'),
               component(backdrop, 'Transform', transform([0, 0, -3]) | dict(scale=[8, 8, .25])),
               component(backdrop, 'MeshRenderer', dict(primitive='box', albedo=[0, 0, 0], visible=True,
                                                        cast_shadows=False, receive_shadows=False)),
               component(backdrop, 'PbrMaterial', dict(base_color=[0, 0, 0], emissive=list(BASE), metallic=0,
                                                       roughness=1, double_sided=True))]
        styles = dict(loop=dict(texture=self.texture, opacity=.5, flipbook=LOOP,
                                size_over_life=SIZE_CURVE, opacity_over_life=OPACITY_CURVE),
                      hold=dict(texture=self.texture, opacity=.5, flipbook=HOLD,
                                size_over_life=[[0, 1], [1, 1]], opacity_over_life=HOLD_OPACITY),
                      static=dict(texture=self.texture))
        self.entities, effects = {}, {}
        for index, (name, pixel) in enumerate(EMITTERS.items()):
            self.entities[name] = entity = uid(100+index)
            value = dict(spawn_space='world', simulation_space='world', lifetime_ticks=LIFETIME, size_m=BASE_SIZE,
                         linear_rgb=[1, 1, 1] if name in styles else list(BLUE), burst=dict(action=BURST, count=1))
            if name in styles:
                value['sprite'] = styles[name]
            effects[name] = value
            ops.extend([dict(op='entity.create', id=entity, name=name),
                        component(entity, 'Transform', transform(point(pixel, DEPTH))),
                        component(entity, 'ParticleEmitter', value)])
        if self.args.headless:
            effect = effects['loop'] | dict(burst=dict(action=TEMPLATE_BURST, count=1))
            ops.append(dict(op='template.set', id=TEMPLATE, name='Detached lifetime source',
                            components=dict(Transform=transform([0, 0, -DEPTH]), ParticleEmitter=effect)))
        revision = self.world_call('world.inspect')['revision']
        reply = self.world_call('world.transact', request_id=uid(1000), base_revision=revision, ops=ops)
        assert reply['committed'] and reply['revision'] == revision+1, reply
        revision = reply['revision']
        for name, entity in self.entities.items():
            assert self.world_call('entity.get', id=entity, component='ParticleEmitter')['value'] == effects[name]
        self.check_admission(revision, effects['loop'])
        revision = self.check_grid(revision, effects['hold'])
        closure = self.measurements['closure'] = [dict(filename=cooked.name, sha256=self.texture, bytes=receipt['bytes'])]
        for owner in (self.world_call('world.dependencies'),
                      success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))):
            assert owner['revision'] == revision and owner['assets'] == closure, owner
            assert owner['needs_particles'] is True and owner['needs_sprite_particles'] is True, owner
        return revision

    def check_admission(self, revision, effect):
        baseline = self.world.read_bytes()
        invalid = [dict(size_over_life=curve) for curve in (
            [[0, 1]], [[n/8, 1] for n in range(9)], [[.125, 1], [1, 1]], [[0, 1], [.875, 1]],
            [[0, 1], [.5, 2], [.5, 3], [1, 1]], [[0, 1], [.75, 2], [.25, 3], [1, 1]],
            [[0, 1, 1], [1, 1]], [[0, True], [1, 1]], [[0, -.125], [1, 1]], [[0, 8.125], [1, 1]],
        )]
        invalid.append(dict(opacity_over_life=[[0, 1.125], [1, 1]]))
        invalid.extend(dict(flipbook=LOOP | change) for change in (
            dict(columns=0), dict(rows=0), dict(first_frame=-1), dict(frame_count=0), dict(frame_count=4),
            dict(ticks_per_frame=0), dict(ticks_per_frame=1.5), dict(loop=1), dict(unrecognized=True),
        ))
        entity = self.entities['loop']
        requests = [rpc('world.transact', request_id=uid(1010+index), base_revision=revision,
                        ops=[component(entity, 'ParticleEmitter', effect | dict(sprite=effect['sprite'] | patch))])
                    for index, patch in enumerate(invalid)]
        for reply in self.world_calls(*requests):
            error(reply, -32602)
        assert self.world.read_bytes() == baseline
        self.measurements['malformed_rejected'] = len(invalid)

    def check_grid(self, revision, effect):
        # The same texture is already referenced by the valid loop emitter.
        entity = self.entities['hold']
        bad = effect | dict(sprite=effect['sprite'] | dict(flipbook=HOLD | dict(columns=3)))
        changed = self.world_call('world.transact', request_id=uid(1050), base_revision=revision,
                                  ops=[component(entity, 'ParticleEmitter', bad)])
        assert changed['committed'] and changed['revision'] == revision+1, changed
        # The project admits the syntax. Reopen the entry world directly so
        # a failed cooked dependency check cannot prevent repairing the edit.
        requests = [dict(rpc('world.dependencies'), id=1),
                    dict(rpc('world.transact', request_id=uid(1051), base_revision=revision+1,
                             ops=[component(entity, 'ParticleEmitter', effect)]), id=2)]
        replies = self.cli('world', self.native(self.world), stdin=requests).splitlines()
        broken, restored = map(json.loads, replies)
        rejected = error(broken, -32050)
        assert self.texture in rejected['message'] and 'evenly divide' in rejected['message'], rejected
        repaired = success(restored)
        assert repaired['committed'] and repaired['revision'] == revision+2, repaired
        self.measurements['incompatible_grid'] = dict(error=rejected, repaired_revision=repaired['revision'])
        return repaired['revision']

    def state(self, client, session, age):
        observed = self.observed(client, session, BIRTH+age)
        total = len(EMITTERS)
        spawned = total if age >= 0 else 0
        expired = total if age == LIFETIME else 0
        expected = counters(spawned-expired, spawned, expired=expired)
        assert observed['counts'] == expected, observed
        sources = {row['entity']: row['id'] for row in observed['sources']}
        assert set(sources) == set(self.entities.values()), observed
        if 0 <= age < LIFETIME:
            for index, (name, row) in enumerate(zip(EMITTERS, observed['particles'], strict=True), 1):
                expected_row = dict(id=str(index), emitter=sources[self.entities[name]], age_ticks=age, lifetime_ticks=LIFETIME,
                                    position_m=point(EMITTERS[name], DEPTH), velocity_m_s=[0, 0, 0], size_m=BASE_SIZE,
                                    linear_rgba=list(BLUE)+[1] if name == 'opaque' else [1, 1, 1, 1 if name == 'static' else .5])
                if name != 'opaque':
                    present = sprite_presentation(self.texture, name, age)
                    expected_row['presentation'] = present
                    actual = row['presentation']
                    assert actual['visible'] is present['visible'] and type(actual['frame']) is type(present['frame']), row
                    assert all(type(n) is int for n in actual['texel_rect']), row
                assert row == expected_row, (name, row, expected_row)
        return observed

    def capture(self, client, state, age, revision, output):
        session, tick = state['session_id'], state['tick']
        witnesses = probe_points(age)
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(output), width=WIDTH, height=HEIGHT, samples=1, gpu=self.args.gpu,
            frames_in_flight=2, lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for (x, y), _ in witnesses.values()]))
        assert (report['source'], report['session_id'], report['tick'], report['revision']) == ('runtime', session, tick, revision)
        assert (report['width'], report['height'], report['samples'], report['lens']) == (WIDTH, HEIGHT, 1, LENS), report
        assert report['hardware'] and report['frames_presented'] > 0 and report['capture_written'] and report['nvrhi_errors'] == 0
        assert report['camera_world'] == [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], report
        diagnostics = report['render_diagnostics']
        assert diagnostics['completed_submissions'] > 0 and diagnostics['lighting_path'] == self.args.lighting_path
        presented = diagnostics['scene_receipts']['presented']
        assert (presented['camera_id'], presented['camera_lens'], presented['authored_revision']) == (CAMERA, LENS, revision)
        draws = diagnostics['last_draws']
        visible = sum(row.get('presentation', {}).get('visible', False) for row in state['particles'])
        assert draws['sprite_instances'] == visible and draws['sprite_batches'] == bool(visible), draws
        assert draws['streamed_draws'] == int(0 <= age < LIFETIME), draws
        products = diagnostics['scene_products']
        assert products['available'] and products['motion_available'] and products['view'] == 'color', products
        image = pixels(output)
        assert len(image) == HEIGHT and all(len(row) == WIDTH for row in image)
        record = dict(report=report, path=str(output), sha256=file_hash(output), probes={})
        for (name, ((x, y), color)), probe in zip(witnesses.items(), products['probes'], strict=True):
            rgb, expected = image[y][x], [display(channel) for channel in color]
            item = record['probes'][name] = dict(probe, hdr_expected=color, bmp_rgb=rgb, bmp_expected=expected)
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), item
            assert probe['raw_hdr'] == probe['resolved_hdr'] == list(color), item
            assert max(abs(a-b) for a, b in zip(rgb, expected, strict=True)) <= 1, item
            depth = DEPTH if name == 'opaque' and 0 <= age < LIFETIME else 2.875
            assert probe['surface_valid'] and abs(probe['depth'] - 1.5*(1-1/depth)) < 2e-6, item
        return record

    def exercise(self, revision, external, game=None):
        saves = external/'slots'
        saves.mkdir()
        originals, archives, captures = {}, {}, {}
        restored_states, restored_captures = {}, {}
        self.measurements['continuation'] = dict(original_states=originals, restored_states=restored_states,
                                                 original_captures=captures, restored_captures=restored_captures)
        slots = {CHECKPOINT: 'checkpoint', FINAL: 'future'}
        with self.owner('original lifetime', game=game, saves=saves) as (client, original):
            session = self.initialize(client, revision, uid(1090), saves, game)
            self.step(client, session, 0, 1100, ticks=BIRTH-1, inputs=inputs(BURST))
            for age in range(-1, LIFETIME+1):
                if age >= 0:
                    self.step(client, session, BIRTH+age-1, 1110+age, inputs=inputs(BURST, pressed=age == 0))
                state = originals[age] = self.state(client, session, age)
                if age in slots:
                    archives[age] = self.save(client, saves, slots[age], session, BIRTH+age, 1130+age)
                if game and age in CAPTURES:
                    captures[age] = self.capture(client, state, age, revision, external/(CAPTURES[age]+'.bmp'))

        restored = uid(1092)
        with self.owner('restored lifetime', game=game, saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(1091), saves, game)
            assert len({session, previous, restored}) == 3
            assert self.load(client, revision, previous, restored, 'checkpoint', BIRTH+CHECKPOINT, 1150) == 0
            for age in range(CHECKPOINT, LIFETIME+1):
                if age > CHECKPOINT:
                    self.step(client, restored, BIRTH+age-1, 1160+age, inputs=inputs(BURST))
                state = restored_states[age] = self.state(client, restored, age)
                assert state == dict(originals[age], session_id=restored), state
                if age in slots:
                    assert self.save(client, saves, 'restored-'+slots[age], restored, BIRTH+age, 1180+age) == archives[age]
                if game and age in (CHECKPOINT, 4, FINAL):
                    result = restored_captures[age] = self.capture(client, state, age, revision,
                                                                   external/('restored-'+CAPTURES[age]+'.bmp'))
                    assert result['report']['gpu'] == captures[age]['report']['gpu']
                    assert result['probes'] == captures[age]['probes'] and result['sha256'] == captures[age]['sha256']
        assert original['pid'] != cold['pid'] and archives[CHECKPOINT] != archives[FINAL]
        if game:
            closed, expired = captures[-1], captures[LIFETIME]
            assert closed['sha256'] == expired['sha256'] and closed['probes'] == expired['probes']
            assert len({captures[age]['sha256'] for age in (0, CHECKPOINT, 4, 5, FINAL)}) == 5
            for capture in captures.values():
                for name in ('hold_center', 'hold_right', 'static_full_image', 'loop_center', 'loop_fixed_radius'):
                    for key in ('depth', 'shading_normal', 'surface_valid', 'motion', 'motion_valid'):
                        assert capture['probes'][name][key] == closed['probes'][name][key], (name, key, capture['probes'][name])

    def template_drain(self, revision, external):
        saves = external/'template slots'
        saves.mkdir()
        expected, restored = {}, {}
        self.measurements['template_drain'] = dict(original=expected, restored=restored)
        with self.owner('original detached template', saves=saves) as (client, owner):
            session = self.initialize(client, revision, uid(1200), saves, None)
            spawn = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(1201),
                expected_tick=0, expected_structure_revision=0, spawns=[dict(template_id=TEMPLATE)]))
            entity, = spawn['spawned']
            assert spawn['structure_revision'] == 1, spawn
            self.step(client, session, 0, 1202, expected_structure_revision=1, inputs=inputs(TEMPLATE_BURST, pressed=True))
            born = self.observed(client, session, 1)
            birth, = born['particles']
            assert birth['presentation'] == sprite_presentation(self.texture, 'loop', 0), birth
            removed = client.call('runtime.structure.transact', dict(session_id=session, request_id=uid(1203),
                expected_tick=1, expected_structure_revision=1, despawns=[entity]))
            structure = removed['structure_revision']
            assert structure == 2 and removed['despawned'] == [entity], removed
            draining = expected[0] = self.observed(client, session, 1)
            source, = (row for row in draining['sources'] if row['entity'] == entity)
            assert source['draining'] is True and source['emitting'] is False and source['id'] == birth['emitter'], source
            assert draining['particles'] == [birth] and draining['counts'] == counters(1, 1), draining
            self.save(client, saves, 'draining', session, 1, 1204, structure=structure)
            self.step(client, session, 1, 1205, ticks=FINAL, expected_structure_revision=structure)
            continued = expected[FINAL] = self.observed(client, session, FINAL+1)
            future, = continued['particles']
            assert future == dict(birth, age_ticks=FINAL, presentation=sprite_presentation(self.texture, 'loop', FINAL)), future
            archive = self.save(client, saves, 'continued', session, FINAL+1, 1206, structure=structure)
            self.step(client, session, FINAL+1, 1207, ticks=LIFETIME-FINAL, expected_structure_revision=structure)
            expired = expected[LIFETIME] = self.observed(client, session, LIFETIME+1)
            assert expired['counts'] == counters(0, 1, expired=1), expired
            assert {row['entity'] for row in expired['sources']} == set(self.entities.values()), expired

        with self.owner('restored detached template', saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(1210), saves, None)
            session = uid(1211)
            assert self.load(client, revision, previous, session, 'draining', 1, 1212) == structure
            restored[0] = self.observed(client, session, 1)
            self.step(client, session, 1, 1213, ticks=FINAL, expected_structure_revision=structure)
            restored[FINAL] = self.observed(client, session, FINAL+1)
            assert self.save(client, saves, 'restored-continued', session, FINAL+1, 1214, structure=structure) == archive
            self.step(client, session, FINAL+1, 1215, ticks=LIFETIME-FINAL, expected_structure_revision=structure)
            restored[LIFETIME] = self.observed(client, session, LIFETIME+1)
            assert restored == {age: dict(state, session_id=session) for age, state in expected.items()}
        assert owner['pid'] != cold['pid']

    def run(self):
        revision = self.measurements['authored_revision'] = self.author()
        if self.args.author_only:
            return
        external = self.root/'External lifetime verification'
        external.mkdir()
        if self.args.headless:
            source = tree_hashes(self.project)
            self.exercise(revision, external)
            self.template_drain(revision, external)
            assert tree_hashes(self.project) == source
            return

        output = self.root/'Initial sprite export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle, detached = self.root/'Relocated Sprite Game', self.root/'Unavailable authoring project'
        output.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        assert not list(detached.glob('*.png')) and not list((bundle/'content').rglob('*.png'))
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        gameset = bundle/'game.json'
        runtime = json.loads((bundle/'runtime'/'runtime.json').read_bytes())['features']
        assert runtime['scene_particles'] is True and runtime['scene_particle_sprites'] is True, runtime
        assert [(Path(path).name, digest) for path, digest in frozen.items() if path.endswith('.pimage')] == [
            (self.texture+'.pimage', self.texture)]
        info = success(json.loads(self.cli('game', 'inspect', self.native(gameset))))
        assert info['revision'] == revision, info
        self.measurements['bundle'] = dict(manifest=str(gameset), inventory=frozen, detached_source=authored)
        self.exercise(revision, external, game=gameset)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--author-only', action='store_true', help='Check admission and cooked asset closure.')
    mode.add_argument('--headless', action='store_true', help='Check authoring and native cold save continuation.')
    mode.add_argument('--runtime-root', type=Path, help='Matching installed Windows runtime for relocated Vulkan capture.')
    parser.add_argument('--output', '--capture-output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    label = 'author' if args.author_only else 'headless' if args.headless else f'{args.lighting_path}-gpu-{args.gpu}'
    root = Path(tempfile.mkdtemp(prefix=f'sprite-lifetime-{label}-', dir=args.output.resolve()))
    test = SpriteLifetimeCapture(args, root)
    result = dict(passed=False, mode=label, executable_sha256=file_hash(args.binary), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
