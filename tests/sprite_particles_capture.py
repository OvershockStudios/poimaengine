#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify texture-backed particles in a relocated standalone Vulkan game."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from scene_capture import pixels
from texture_fixture import png
from world_particles_contract import ParticleContract, PLAYER, action, component, counters, inputs, near_vector, transform
from world_terrain_contract import encode, error, file_hash, rpc, success, tree_hashes, uid


WIDTH, HEIGHT = 256, 256  # Binary-exact focal scale at 90 degrees.
CAMERA = uid(7)
LENS = dict(vertical_fov=90, near=1, far=3)
BURST = 'sprite-burst'
LIFETIME = 6
BIRTH = 3
LIVE = 5
EXPIRED = BIRTH+LIFETIME
BASE = (.25, .25, .25)
PROBES = {
    'overlap': (80, 67),
    'tie': (240, 67),
    'red_texel': (146, 67),
    'filtered_edge': (160, 67),
    'transparent_texel': (174, 67),
    'behind_blocker': (53, 155),
    'half_fade': (113, 155),
    'hard_depth': (173, 155),
    'full_fade': (248, 155),
    'opaque_particle': (43, 32),
}
# Individual interiors are deliberately disjoint except for the two order witnesses.
# Equal-depth sprites are born in order. The overlap is born near, far, middle.
# Each row is (pixel, depth in metres, texture, opacity, fade distance in metres).
SPRITES = (
    (PROBES['overlap'], 1.25, 'red', .5, 0),
    (PROBES['overlap'], 2.5, 'red', .5, 0),
    (PROBES['overlap'], 1.75, 'green', .5, 0),
    (PROBES['tie'], 1.75, 'red', .5, 0),
    (PROBES['tie'], 1.75, 'green', .5, 0),
    (PROBES['filtered_edge'], 1.5, 'fringe', 1, 0),
    (PROBES['behind_blocker'], 2.5, 'red', 1, 0),
    (PROBES['half_fade'], 1.5, 'red', .5, 1),
    (PROBES['hard_depth'], 1.5, 'red', .5, 0),
    (PROBES['full_fade'], 1.25, 'red', .5, 1),
)
OPAQUE = (PROBES['opaque_particle'], 1.5)
TOTAL = len(SPRITES)+1
BLOCKED = {'behind_blocker', 'half_fade', 'hard_depth'}
EXPECTED_HDR = {
    'overlap': (.65625, .28125, .03125),               # Far red, middle green, near red.
    'tie': (.3125, .5625, .0625),                      # Red then green by persistent ID.
    'red_texel': (1, 0, 0),
    'filtered_edge': (.625, .125, .125),              # Half red over the base, zero blue from the texel.
    'transparent_texel': BASE,
    'behind_blocker': BASE,
    'half_fade': (.4375, .1875, .1875),                # Half opacity times half fade.
    'hard_depth': (.625, .125, .125),
    'full_fade': (.625, .125, .125),
    'opaque_particle': (0, 0, 1),
}


def point(pixel, depth):
    """Unproject a pixel centre through the fixture's identity, 90-degree camera."""
    x, y = pixel
    focal = HEIGHT/2
    return [(x+.5-WIDTH/2)*depth/focal, (HEIGHT/2-y-.5)*depth/focal, -depth]


def profiles():
    held = dict(ticks=BIRTH-1, actions=[action(BURST)])
    pressed = dict(ticks=LIVE-BIRTH+1, actions=[action(BURST, pressed=True)])
    return [('closed', LIVE, [dict(ticks=LIVE, actions=[action(BURST)])]),
            ('live', LIVE, [held, pressed]),
            ('expired', EXPIRED, [held, pressed, dict(ticks=EXPIRED-LIVE, actions=[action(BURST)])])]


def expected_counts(label):
    if label == 'live':
        return counters(TOTAL, TOTAL)
    if label == 'expired':
        return counters(0, TOTAL, expired=TOTAL)
    return counters(0, 0)


class SpriteCapture(ParticleContract):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Sprite Particle Contract')))
        sources = {'red': [(255, 0, 0, 255)], 'green': [(0, 255, 0, 255)],
                   'fringe': [(255, 0, 0, 255), (0, 0, 255, 0)]}
        imported = self.measurements['textures'] = {}
        retained = self.root/'Retained raw images'
        retained.mkdir()
        for label, texels in sources.items():
            path = self.project/(label+'.png')
            path.write_bytes(png(len(texels), 1, [channel for texel in texels for channel in texel]))
            receipt = self.world_call('asset.image.import', source=path.name, color_space='srgb')
            assert receipt['color_space'] == 'srgb' and receipt['mips'][0]['width'] == len(texels), receipt
            saved = path.rename(retained/path.name)
            imported[label] = dict(source=str(saved), source_sha256=file_hash(saved), receipt=receipt)

        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1)),
               dict(op='entity.create', id=CAMERA, name='Fixed probe camera'),
               component(CAMERA, 'Transform', transform([0, 0, 0])), component(CAMERA, 'Camera', LENS)]
        material = dict(base_color=[0, 0, 0], emissive=list(BASE), metallic=0, roughness=1, double_sided=True)
        for number, name, location, scale in (
            (8, 'Constant backdrop', [0, 0, -3], [8, 8, .25]),
            (9, 'Opaque blocker', point(PROBES['half_fade'], 2)[:2]+[-2.0625], [2.8, .85, .125]),
        ):
            entity = uid(number)
            ops.extend([dict(op='entity.create', id=entity, name=name),
                        component(entity, 'Transform', transform(location) | dict(scale=scale)),
                        component(entity, 'MeshRenderer', dict(primitive='box', albedo=[0, 0, 0], visible=True,
                                                              cast_shadows=False, receive_shadows=False)),
                        component(entity, 'PbrMaterial', material)])

        self.centres = []
        for index, (pixel, depth, texture, opacity, fade) in enumerate((*SPRITES, (*OPAQUE, None, 1, 0))):
            entity, centre = uid(100+index), point(pixel, depth)
            self.centres.append(centre)
            value = dict(spawn_space='world', simulation_space='world', lifetime_ticks=LIFETIME,
                         size_m=.375 if texture == 'fringe' else .5, linear_rgb=[1, 1, 1] if texture else [0, 0, 1],
                         burst=dict(action=BURST, count=1))
            if texture:
                sprite = dict(texture=imported[texture]['receipt']['asset'])
                if opacity != 1:
                    sprite['opacity'] = opacity
                if fade:
                    sprite['depth_fade_m'] = fade
                value['sprite'] = sprite
            ops.extend([dict(op='entity.create', id=entity, name=f'Emitter {index+1}'),
                        component(entity, 'Transform', transform(centre)), component(entity, 'ParticleEmitter', value)])

        revision = self.world_call('world.inspect')['revision']
        change = success(self.world_calls(rpc('world.transact', request_id=uid(900), base_revision=revision, ops=ops))[0])
        assert change['committed'] and change['revision'] == revision+1, change
        dependency = self.world_call('world.dependencies')
        project = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        for result in (dependency, project):
            assert result['needs_particles'] is True and result['needs_sprite_particles'] is True, result
        expected = sorted((entry['receipt']['asset']+'.pimage', entry['receipt']['asset'], entry['receipt']['bytes'])
                          for entry in imported.values())
        assert len({entry[0] for entry in expected}) == 3
        assert dependency['assets'] == project['assets'] == [dict(filename=name, sha256=sha, bytes=size)
                                                               for name, sha, size in expected], dependency
        for label, item in imported.items():
            receipt = item['receipt']
            package = self.assets/(receipt['asset']+'.pimage')
            assert not (self.project/(label+'.png')).exists()
            assert file_hash(package) == receipt['asset'] and package.stat().st_size == receipt['bytes']
            assert self.world_call('asset.image.inspect', asset=receipt['asset']) == receipt
        self.measurements['closure'] = dependency['assets']
        if self.args.author_only:
            return self.check_sprite_admission(change['revision'], imported['red']['receipt']['asset'])
        return change['revision']

    def check_sprite_admission(self, revision, valid_texture):
        base = self.world.read_bytes()
        entity = uid(100)
        value = self.world_call('entity.get', id=entity, component='ParticleEmitter')['value']
        style = value['sprite']
        assert style['texture'] == valid_texture
        invalid = [dict(opacity=1.1), dict(depth_fade_m=-.5), dict(texture='Z'*64), dict(extra=1)]
        requests = [rpc('world.transact', request_id=uid(920+i), base_revision=revision,
                        ops=[component(entity, 'ParticleEmitter', value | dict(sprite=style | fields))])
                    for i, fields in enumerate(invalid)]
        for rejected in self.world_calls(*requests):
            error(rejected, -32602)
        assert self.world.read_bytes() == base

        missing = '0'*64
        assert missing != valid_texture and not (self.assets/(missing+'.pimage')).exists()
        transaction = rpc('world.transact', request_id=uid(924), base_revision=revision,
                          ops=[component(entity, 'ParticleEmitter', value | dict(sprite=style | dict(texture=missing)))])
        accepted = success(self.world_calls(transaction)[0])
        assert accepted['revision'] == revision+1
        broken, restore = (
            rpc('world.dependencies'),
            rpc('world.transact', request_id=uid(925), base_revision=revision+1,
                ops=[component(entity, 'ParticleEmitter', value)]),
        )
        results = self.cli('world', self.native(self.world), stdin=[dict(broken, id=1), dict(restore, id=2)])
        reported, restored = map(json.loads, results.splitlines())
        assert missing in error(reported, -32050)['message']
        assert success(restored)['revision'] == revision+2
        assert self.world_call('world.dependencies')['assets'] == self.measurements['closure']
        self.measurements['admission'] = dict(invalid_fields=len(invalid), missing_texture_error=-32050, repaired=True)
        return revision+2

    def verify_game(self, game, executable, external, revision):
        def expect(label, path, expected):
            return dict(expect=dict(label=label, method='runtime.particles.inspect', path=path, equals=expected))

        groups = []
        for label, tick, replay in profiles():
            sequence = [dict(step=dict(ticks=row['ticks'], inputs=inputs(BURST, row['actions'][0]['pressed'])))
                        for row in replay]
            sequence.append(expect('Particle counts', '/counts', expected_counts(label)))
            if label == 'live':
                sequence.extend([expect('Birth age', '/particles/0/age_ticks', tick-BIRTH),
                                 expect('Near particle born first', '/particles/0/position_m/2', -1.25),
                                 expect('Far particle born second', '/particles/1/position_m/2', -2.5),
                                 expect('Middle particle born third', '/particles/2/position_m/2', -1.75),
                                 expect('Red card stable ID', '/particles/3/id', '4'),
                                 expect('Green card stable ID', '/particles/4/id', '5')])
            groups.append(dict(name=label, sequence=sequence))
        specification = dict(format='poima.game-verification', version=1, groups=groups)
        suite, report = external/'sprites.verify.json', external/'sprites.report.json'
        suite.write_text(encode(specification), encoding='utf-8')
        evidence = self.measurements['verification'] = dict(suite=specification)
        result = success(json.loads(self.cli('game', 'verify', self.native(game), '--suite', self.native(suite),
            '--report', self.native(report), '--completed-exit', 'true', '--expected-bundle-sha256', file_hash(game),
            '--expected-suite-sha256', file_hash(suite), executable=executable, cwd=external)))
        evidence['result'] = result
        assert result == json.loads(report.read_bytes()) and result['passed'] and result['revision'] == revision, result
        assert result['bundle_sha256'] == file_hash(game) and result['suite_sha256'] == file_hash(suite), result
        assert result['groups_passed'] == result['groups_total'] == len(groups), result
        for group, (label, tick, _) in zip(result['groups'], profiles(), strict=True):
            assert (group['name'], group['ticks'], group['passed']) == (label, tick, True), group
            assert len(group['checks']) == (7 if label == 'live' else 1), group
            for check in group['checks']:
                assert check['passed'] and check['observed'] and not check['actual_omitted'] and check['tick'] == tick, check
            assert group['checks'][0]['actual'] == expected_counts(label), group

    def capture_options(self, image):
        return dict(camera=CAMERA, path=self.native(image), width=WIDTH, height=HEIGHT, samples=1,
                    gpu=self.args.gpu, frames_in_flight=2, lighting_path=self.args.lighting_path,
                    reconstruction='none', scene_product_probes=[dict(x=x, y=y) for x, y in PROBES.values()])

    def continue_live(self, client, saves, slot, session, request):
        self.step(client, session, LIVE, request, inputs=inputs(BURST))
        state = self.observed(client, session, LIVE+1)
        assert state['counts'] == expected_counts('live'), state
        assert all(row['age_ticks'] == LIVE+1-BIRTH for row in state['particles']), state
        return state, self.save(client, saves, slot, session, LIVE+1, request+1)

    def capture(self, game, external, revision):
        captures = self.measurements['captures'] = {}
        saves = external/'External sprite saves'
        saves.mkdir()
        for label, tick, sequence in profiles():
            image_path = external/(label+'.bmp')
            with self.owner(label, game=game, saves=saves) as (client, owner):
                session = self.initialize(client, revision, None, saves, game)
                params = dict(session_id=session, expected_tick=0, request_id=uid(901), controller=PLAYER,
                              mode='replay', sequence=sequence, **self.capture_options(image_path))
                play = client.call('runtime.play', params)
                state = self.observed(client, session, tick)
                record = captures[label] = dict(play=play, state=state, image=str(image_path), sha256=file_hash(image_path))
                assert play['success'] and play['stop_reason'] == 'replay_complete', play
                assert play['materials']['presented']['tick'] == tick, play['materials']
                assert state['counts'] == expected_counts(label), state
                for row, centre in zip(state['particles'], self.centres if label == 'live' else [], strict=True):
                    assert row['age_ticks'] == tick-BIRTH, row
                    near_vector(row['position_m'], centre, tolerance=2e-6)
                self.check_capture(label, tick, revision, play, image_path, record)
                if label == 'live':
                    checkpoint = self.save(client, saves, 'checkpoint', session, LIVE, 902)
                    continued, archive = self.continue_live(client, saves, 'continued', session, 903)
                    assert checkpoint != archive
            record['owner_pid'] = owner['pid']

        closed, live, expired = (captures[label] for label, _, _ in profiles())
        assert len({record['owner_pid'] for record in (closed, live, expired)}) == 3
        assert closed['sha256'] == expired['sha256'] and closed['probes'] == expired['probes']
        assert live['sha256'] != closed['sha256']
        assert len({record['play']['gpu'] for record in (closed, live, expired)}) == 1
        waits = [record['play']['render_diagnostics']['frame_execution']['device_idle_waits']
                 for record in (closed, live, expired)]
        assert waits[0] == waits[1] == waits[2], waits
        for name in PROBES.keys()-{'opaque_particle'}:
            for key in ('depth', 'shading_normal', 'surface_valid', 'motion', 'motion_valid'):
                assert live['probes'][name][key] == closed['probes'][name][key], (name, key, live['probes'][name])
        self.restore_capture(game, external, revision, saves, live, checkpoint, continued, archive)

    def restore_capture(self, game, external, revision, saves, original, checkpoint, continued, archive):
        restored = uid(905)
        image = external/'restored.bmp'
        record = self.measurements['cold_continuation'] = dict(tick=LIVE, continued_tick=LIVE+1)
        with self.owner('cold restore', game=game, saves=saves) as (client, owner):
            previous = self.initialize(client, revision, None, saves, game)
            assert len({previous, restored, original['state']['session_id']}) == 3
            assert self.load(client, revision, previous, restored, 'checkpoint', LIVE, 906) == 0
            observed = self.observed(client, restored, LIVE)
            assert observed == dict(original['state'], session_id=restored), observed
            captured = client.call('runtime.capture', dict(session_id=restored, tick=LIVE, structure_revision=0,
                               capture_frames=2, **self.capture_options(image)))
            record.update(capture=captured, state=observed, image=str(image), sha256=file_hash(image))
            assert captured['source'] == 'runtime' and captured['session_id'] == restored, captured
            self.check_capture('live', LIVE, revision, captured, image, record)
            assert record['sha256'] == original['sha256'] and record['probes'] == original['probes']
            assert captured['gpu'] == original['play']['gpu'], captured
            assert self.save(client, saves, 'restored-checkpoint', restored, LIVE, 907) == checkpoint
            state, replayed = self.continue_live(client, saves, 'restored-continuation', restored, 908)
            assert state == dict(continued, session_id=restored), state
            assert replayed == archive
        assert owner['pid'] != original['owner_pid']
        record.update(owner_pid=owner['pid'], checkpoint_sha256=sha256(checkpoint).hexdigest(),
                      continuation_sha256=sha256(archive).hexdigest(), continuation_bytes=len(archive))

    def check_capture(self, label, tick, revision, play, image_path, record):
        active = label == 'live'
        assert play['tick'] == tick and play['revision'] == revision and play['camera'] == CAMERA, play
        assert play['hardware'] and play['frames_presented'] > 0 and play['capture_written'] and play['nvrhi_errors'] == 0, play
        assert (play['width'], play['height'], play['samples']) == (WIDTH, HEIGHT, 1), play
        assert play['camera_world'] == [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], play
        diagnostics = play['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path and diagnostics['frame_execution']['limit'] == 2
        assert diagnostics['completed_submissions'] > 0, diagnostics
        presented = diagnostics['scene_receipts']['presented']
        assert presented['camera_id'] == CAMERA and presented['camera_lens'] == LENS, presented
        draws = diagnostics['last_draws']
        assert draws['sprite_instances'] == (len(SPRITES) if active else 0), draws
        assert draws['sprite_batches'] == (6 if active else 0), draws  # Red, green, red, green, fringe, red.
        assert draws['streamed_draws'] == int(active), draws
        products = diagnostics['scene_products']
        assert products['available'] and products['motion_available'] and products['view'] == 'color', products
        image = pixels(image_path)
        assert len(image) == HEIGHT and all(len(row) == WIDTH for row in image)
        witnesses = record['probes'] = {}
        for (name, (x, y)), probe in zip(PROBES.items(), products['probes'], strict=True):
            radiance = EXPECTED_HDR[name] if active else BASE
            display_rgb = [display(channel) for channel in radiance]
            color = image[y][x]
            witness = witnesses[name] = dict(probe, expected_hdr=radiance, bmp_rgb=color, expected_bmp=display_rgb)
            assert (probe['x'], probe['y']) == (x, y) and (probe['resolved_x'], probe['resolved_y']) == (x, y), witness
            assert probe['raw_hdr'] == probe['resolved_hdr'] == list(radiance), witness
            assert max(abs(a-b) for a, b in zip(color, display_rgb, strict=True)) <= 1, witness
            depth = 1.5 if active and name == 'opaque_particle' else 2 if name in BLOCKED else 2.875
            expected_depth = LENS['far']/(LENS['far']-LENS['near'])*(1-LENS['near']/depth)
            assert probe['surface_valid'] and abs(probe['depth']-expected_depth) < 2e-6, witness

    def run(self):
        revision = self.measurements['authored_revision'] = self.author()
        if self.args.author_only:
            return
        output = self.root/'Initial sprite export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle = self.root/'Relocated Sprite Game'
        detached = self.root/'Retained authoring project'
        output.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        assert not list(detached.glob('*.png')) and not list((bundle/'content').rglob('*.png'))
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        observed_images = {(Path(path).name, sha) for path, sha in frozen.items() if path.endswith('.pimage')}
        assert observed_images == {(entry['filename'], entry['sha256']) for entry in self.measurements['closure']}
        game = bundle/'game.json'
        executable = bundle/json.loads(game.read_bytes())['engine']['executable']
        runtime = json.loads((bundle/'runtime'/'runtime.json').read_bytes())
        assert runtime['features']['scene_particles'] is True and runtime['features']['scene_particle_sprites'] is True
        self.measurements['bundle'] = dict(manifest=str(game), inventory=frozen)
        external = self.root/'External sprite verification'
        external.mkdir()
        self.verify_game(game, executable, external, revision)
        self.capture(game, external, revision)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed standalone runtime.')
    parser.add_argument('--author-only', action='store_true', help='Check cooked texture assets and dependency closure.')
    parser.add_argument('--output', '--capture-output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    if not args.author_only and not args.runtime_root:
        parser.error('--runtime-root is required for standalone verification and capture.')
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = 'sprites-author-' if args.author_only else f'sprites-{args.lighting_path}-gpu-{args.gpu}-'
    root = Path(tempfile.mkdtemp(prefix=prefix, dir=args.output.resolve()))
    test = SpriteCapture(args, root)
    result = dict(passed=False, author_only=args.author_only, executable_sha256=file_hash(args.binary), gpu_index=args.gpu,
                  lighting_path=args.lighting_path, calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        evidence = root/'evidence.json'
        evidence.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(f'Evidence file {evidence}')


if __name__ == '__main__':
    main()
