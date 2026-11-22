#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify authored directional wave layers with native water and optional Vulkan."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from scene_capture import pixels
from water_ripples_capture import (
    FAR, LENS, MOVED, NEAR, NORMAL_TOLERANCE, ORIGIN_XZ, RIPPLES, VALIDATION_LAYER,
    RippleCapture, camera_matrix, dot, plane_point, probe_reference, probes_for, smooth_fade, unit,
)
from world_particles_contract import near_vector
from world_terrain_contract import CAPTURE_SIZE, encode, error, file_hash, rpc, success, tree_hashes, uid
from world_water_contract import CELLS, CHECKPOINT, FINAL, GATE, SILL, TILES, reference


LAYERS = [
    dict(wavelength_m=.72, slope=.105, period_ticks=32, heading_degrees=27.),
    dict(wavelength_m=.41, slope=.080, period_ticks=19, heading_degrees=-41.),
    dict(wavelength_m=1.37, slope=.105, period_ticks=53, heading_degrees=111.),
    dict(wavelength_m=.22, slope=.065, period_ticks=11, heading_degrees=-157.),
]
PROFILES = {'unlayered': None, 'zero': [layer | dict(slope=0.) for layer in LAYERS], 'layered': LAYERS}
LEGACY_PROFILES = ({}, {'rain_cells': [[0, 0]]}, {'surface_ripples': RIPPLES | {'heading_degrees': 37.}})
# Stock 0.0.384 native cooks of the exact terrain and physics recipe below.
LEGACY_HASHES = (
    'd63181362a1307fad328fc8780b7a27a1027e40efb2e4c8f7efeb4a04c7a67a1',
    '927cfae1cdb112b6270820d764d26362bf917f55e99ff178b1ecec79764162e0',
    'bccb6eaf2cc1bfd7316111e68b45524223a478fe44e0002bba219408dc1f2941',
)
FEATURE = 'water_wave_layers'
REQUIREMENT = 'needs_water_wave_layers'
UNSUPPORTED = 'Water wave layers require a compatible Vulkan runtime.'
VIEW_SAMPLES = ((0, NEAR), (CHECKPOINT, NEAR), (CHECKPOINT, FAR), (CHECKPOINT, MOVED), (FINAL, NEAR))


def recipe_parameters(test, revision):
    return dict(revision=revision, terrain={'source': test.recipe.name},
        expected_terrain_recipe_sha256=test.terrain['recipe_sha256'], tile_entities=TILES,
        grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4, relaxation_per_second=6),
        initial_depths_m=test.oracle[0]['depths'],
        faces=[dict(cell=[1, 0], axis='x', openness=0, sill_height_m=SILL, action=GATE, action_openness=.5),
               dict(cell=[1, 1], axis='x', openness=0, sill_height_m=SILL)],
        exchanges=[dict(cell=[0, 0], source_m3_per_second=.6, drain_m3_per_second=0),
                   dict(cell=[3, 0], source_m3_per_second=0, drain_m3_per_second=.24)])


def normal_for(layers, camera, pixel, surface_y, tick, *, filtered=True, local=True):
    """Sum analytic derivatives of independent traveling sinusoidal heights."""
    x, z = pixel
    point, _ = plane_point(camera, pixel, surface_y)
    row, col = z-z%2, x-x%2
    left, _ = plane_point(camera, (col, z), surface_y)
    right, _ = plane_point(camera, (col+1, z), surface_y)
    above, _ = plane_point(camera, (x, row), surface_y)
    below, _ = plane_point(camera, (x, row+1), surface_y)
    dx = (right[0]-left[0], right[2]-left[2])
    dy = (below[0]-above[0], below[2]-above[2])
    coordinate = (point[0]-ORIGIN_XZ[0], point[2]-ORIGIN_XZ[1]) if local else (point[0], point[2])
    contributions, fades = [], []
    for layer in layers or ():
        if layer['slope'] == 0:
            continue
        direction = math.radians(layer['heading_degrees'])
        axis = (math.cos(direction), math.sin(direction))
        frequency = math.tau/layer['wavelength_m']
        wavevector = tuple(frequency*v for v in axis)
        footprint = max(abs(dot(dx, wavevector)), abs(dot(dy, wavevector)))
        fade = smooth_fade(footprint) if filtered else 1
        offset = math.tau*(tick % layer['period_ticks'])/layer['period_ticks']
        signed_slope = layer['slope'] * math.cos(dot(coordinate, wavevector)-offset) * fade
        contributions.append(tuple(signed_slope*v for v in axis))
        fades.append(fade)
    east, north = (sum(item[i] for item in contributions) for i in range(2))
    return unit((-east, 1, -north)), fades


def reference_discrimination():
    """Establish that the chosen samples separate different wave hypotheses."""
    states = reference()
    gaps = {'frozen_tick': 0., 'world_origin': 0., 'unfiltered': 0., 'missing_layer': [0.]*len(LAYERS)}
    for tick, camera in VIEW_SAMPLES:
        for cell, pixel in probes_for(camera, states[tick]).values():
            if cell is None:
                continue
            point, _ = probe_reference(camera, states[tick], cell, pixel, (0, 0))
            y = point[1]
            observed, _ = normal_for(LAYERS, camera, pixel, y, tick)
            frozen, _ = normal_for(LAYERS, camera, pixel, y, 0)
            world, _ = normal_for(LAYERS, camera, pixel, y, tick, local=False)
            unfiltered, _ = normal_for(LAYERS, camera, pixel, y, tick, filtered=False)
            for key, alternate in (('frozen_tick', frozen), ('world_origin', world), ('unfiltered', unfiltered)):
                gaps[key] = max(gaps[key], math.dist(observed, alternate))
            for i in range(len(LAYERS)):
                missing, _ = normal_for(LAYERS[:i]+LAYERS[i+1:], camera, pixel, y, tick)
                gaps['missing_layer'][i] = max(gaps['missing_layer'][i], math.dist(observed, missing))
    assert min(gaps[key] for key in ('frozen_tick', 'world_origin', 'unfiltered')) > .01, gaps
    assert min(gaps['missing_layer']) > .02, gaps
    return gaps


class LayerContract(RippleCapture):
    def cook(self, params, version):
        authored, inventory = self.world.read_bytes(), tree_hashes(self.assets)
        plan = self.world_call('world.water.plan', **params)
        assert plan['revision'] == params['revision'] and plan['cells'] == len(CELLS), plan
        assert self.world.read_bytes() == authored and tree_hashes(self.assets) == inventory
        pin = plan['plan_sha256']
        result = self.world_call('world.water.cook', **params, expected_plan_sha256=pin)
        assert exact(result, plan) and self.world.read_bytes() == authored, result
        asset = self.assets/(pin+'.pwater')
        assert file_hash(asset) == pin and asset.stat().st_size == plan['asset_bytes'], plan
        frozen = tree_hashes(self.assets)
        assert exact(self.world_call('world.water.cook', **params, expected_plan_sha256=pin), plan)
        assert tree_hashes(self.assets) == frozen == inventory | {asset.name: pin}
        content = json.loads(asset.read_bytes())
        assert content['format'] == 'poima.water.region' and type(content['version']) is int
        assert content['version'] == version and content['tiles'] == plan['tiles'], content
        for key in ('surface_ripples', 'surface_wave_layers'):
            if key in params:
                assert exact(content[key], params[key]) and exact(plan[key], params[key]), (content, plan)
            else:
                assert key not in plan and key not in content, (plan, content)
        return plan

    def admission(self, params):
        method = 'world.water.plan'
        authored = self.world.read_bytes()
        inventory = tree_hashes(self.assets)
        history = self.world_call('world.history')
        for name in (method, 'world.water.cook'):
            methods = self.world_call('world.describe', view='method', name=name)['methods']
            schema = methods[name]['properties']['surface_wave_layers']
            assert schema['type'] == 'array' and schema['minItems'] == 1 and schema['maxItems'] == 4, schema
            assert set(schema['items']['required']) == set(RIPPLES), schema
        valid = {'surface_wave_layers': LAYERS}
        old = LAYERS[0]
        invalid = [
            dict(surface_wave_layers=None), dict(surface_wave_layers=[]),
            dict(surface_wave_layers=old), dict(surface_wave_layers=LAYERS+[old]),
            dict(surface_wave_layers=[old | {'slope': True}]),
            dict(surface_wave_layers=[old | {'slope': -.01}]),
            dict(surface_wave_layers=[old | {'slope': .401}]),
            dict(surface_wave_layers=[old | {'wavelength_m': .099}]),
            dict(surface_wave_layers=[old | {'wavelength_m': 8.001}]),
            dict(surface_wave_layers=[old | {'period_ticks': 12.5}]),
            dict(surface_wave_layers=[old | {'period_ticks': 7}]),
            dict(surface_wave_layers=[old | {'period_ticks': 3601}]),
            dict(surface_wave_layers=[old | {'heading_degrees': 180.01}]),
            dict(surface_wave_layers=[old | {'unrecognized': 0}]),
            dict(surface_wave_layers=[{k: v for k, v in old.items() if k != 'slope'}]),
            dict(surface_wave_layers=[old | {'slope': .125}]*4),
            valid | {'surface_ripples': RIPPLES},
        ]
        replies = self.world_calls(*(rpc(method, **(params | alternate)) for alternate in invalid),
            rpc('world.water.cook', **(params | invalid[-1]), expected_plan_sha256='0'*64),
            rpc('world.water.cook', **(params | valid), expected_plan_sha256='0'*64))
        for reply in replies[:-1]:
            error(reply, -32602)
        error(replies[-1], -32009)
        assert self.world.read_bytes() == authored and tree_hashes(self.assets) == inventory
        assert exact(self.world_call('world.history'), history)
        self.measurements['invalid_requests'] = len(replies)-1
        self.measurements['stale_plan_refusals'] = 1

    def reject_cli(self, *arguments, code, executable=None):
        command = [str((executable or self.args.binary).resolve()), *map(str, arguments)]
        proc = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.calls.append(dict(argv=command, exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr))
        assert proc.returncode == 4, (command, proc.returncode)
        assert json.loads(proc.stdout)['diagnostics'] == [dict(code=code, message=UNSUPPORTED)], proc.stdout

    def export(self, variant, revision, plan):
        runtime = self.args.runtime_root
        spec = json.loads((runtime/'runtime.json').read_bytes())
        assert spec['features'][FEATURE] is True, spec
        output = self.root/(variant+' original game')
        if variant == 'layered':
            for state in ('missing', 'false'):
                refused = self.root/('unsupported runtime '+state)
                refused.mkdir()
                unsupported = dict(spec, features=spec['features'].copy())
                if state == 'missing':
                    del unsupported['features'][FEATURE]
                else:
                    unsupported['features'][FEATURE] = False
                (refused/'runtime.json').write_text(encode(unsupported), encoding='utf-8')
                self.reject_cli('project', 'build', self.native(self.manifest),
                                '--output', self.native(output), '--runtime', self.native(refused), code='project.failed')
                assert not output.exists()
        success(json.loads(self.cli('project', 'build', self.native(self.manifest),
                                   '--output', self.native(output), '--runtime', self.native(runtime))))
        detached = self.root/(variant+' relocated game')
        output.rename(detached)
        assert not output.exists()
        game = detached/'game.json'
        executable = detached/json.loads(game.read_bytes())['engine']['executable']
        result = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert result['revision'] == revision, result
        self.check_required_feature(variant, result)
        assets = sorted(path for path in detached.rglob('*') if path.suffix in ('.pmodel', '.pwater'))
        assert sorted(path.name for path in assets) == sorted([plan['plan_sha256']+'.pwater',
            *(tile['asset']+'.pmodel' for tile in self.terrain['tiles'])])
        assert all(file_hash(path) == path.stem for path in assets)
        assert not list(detached.rglob(self.recipe.name))
        if variant == 'layered':
            self.reject_relocated(detached)
        return game

    def reject_relocated(self, supported):
        mutated = self.root/'unsupported relocated game'
        shutil.copytree(supported, mutated)
        manifest = mutated/'game.json'
        original = json.loads(manifest.read_bytes())
        relative = 'runtime/runtime.json'
        runtime = mutated/relative
        descriptor = json.loads(runtime.read_bytes())
        executable = mutated/original['engine']['executable']
        for state in ('missing', 'false'):
            variant = dict(descriptor, features=descriptor['features'].copy())
            if state == 'missing':
                del variant['features'][FEATURE]
            else:
                variant['features'][FEATURE] = False
            runtime.write_text(encode(variant), encoding='utf-8')
            changed = json.loads(json.dumps(original))
            inventory = next(row for row in changed['files'] if row['path'] == relative)
            inventory.update(size=runtime.stat().st_size, sha256=file_hash(runtime))
            manifest.write_text(encode(changed), encoding='utf-8')
            self.measurements.setdefault('unsupported_relocated', {})[state] = dict(
                runtime=variant, manifest_sha256=file_hash(manifest))
            self.reject_cli('game', 'inspect', self.native(manifest), executable=executable, code='game.failed')

    @staticmethod
    def check_required_feature(variant, info):
        if variant == 'unlayered':
            assert REQUIREMENT not in info, info
        else:
            assert info.get(REQUIREMENT) is True, info

    def bind(self, variant, revision, plan, request):
        changed = success(self.world_calls(self.transaction(revision, request,
            dict(op='water.set', asset=plan['plan_sha256'])))[0])
        revision += 1
        assert changed['committed'] and changed['revision'] == revision, changed
        self.verify_closure(revision, plan)
        for info in (self.world_call('world.dependencies'),
                     success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))):
            self.check_required_feature(variant, info)
        return revision

    def observe_wave_scene(self, client, session, tick, layers):
        observed = self.call(client, self.state(session, tick))
        assert 'surface_ripples' not in observed, observed
        if layers is None:
            assert 'surface_wave_layers' not in observed, observed
        else:
            assert exact(observed['surface_wave_layers'], layers), observed
        # Keep the original native surface and terrain ray checks. Strip
        # only the independently checked rendering selection for the
        # cross-profile comparison of the complete physical observations.
        state = self.verify_scene_state(client, session, tick, None)
        assert exact({k: v for k, v in observed.items() if k not in ('session_id', 'plan_sha256')}, state['water'])
        state['water'].pop('surface_wave_layers', None)
        return state

    def capture_wave_scene(self, client, session, tick, camera, layers, label):
        state = self.oracle[tick]
        sites = probes_for(camera, state)
        output = self.root/(label+'.bmp')
        width, height = CAPTURE_SIZE
        matrix = client.call('runtime.entity', dict(session_id=session, tick=tick, id=camera))['world_matrix']
        near_vector(matrix, camera_matrix(camera), tolerance=1e-8)
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=camera, path=self.native(output), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path, capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for _, (x, y) in sites.values()]))
        self.measurements['capture_reports'][label] = report
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] == 2, report
        assert report['tick'] == tick and report['lens'] == LENS and report['camera_world'] == matrix, report
        assert report['nvrhi_errors'] == 0, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        receipts = diagnostics['scene_receipts']
        assert receipts['presented'] and receipts['submitted'] == receipts['presented'], receipts
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        image = pixels(output)
        assert len(image) == height and all(len(row) == width for row in image)
        captured = dict(tick=tick, camera=camera, gpu=report['gpu'], sha256=file_hash(output), probes={})
        near, far = LENS['near'], LENS['far']
        for (name, (cell, pixel)), probe in zip(sites.items(), products['probes'], strict=True):
            x, y = pixel
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            point, distance = probe_reference(camera, state, cell, pixel, (0, 0))
            expected, fades = normal_for(layers if cell is not None else None, camera, pixel, point[1], tick)
            depth = near/(1-probe['depth']*(far-near)/far)
            item = dict(cell=cell, point=point, expected_normal=expected, fades=fades,
                        depth_error_m=abs(depth-distance), rgb=image[y][x], raster=probe)
            assert probe['surface_valid'] is True and item['depth_error_m'] < .002, item
            near_vector(probe['shading_normal'][:3], expected, tolerance=NORMAL_TOLERANCE)
            assert exact(probe['raw_hdr'], probe['resolved_hdr']), item
            if cell is not None:
                assert probe['motion_valid'] is False, item
            captured['probes'][name] = item
        return captured

    def exercise_profile(self, variant, revision, game):
        layers = PROFILES[variant]
        saves = self.root/(variant+' saves')
        saves.mkdir()

        def sequence(client, session, offset, start_tick):
            frames = {}
            sample_ticks = (0, CHECKPOINT, FINAL) if start_tick == 0 else (CHECKPOINT, FINAL)
            states = {}
            for tick in sample_ticks:
                states[tick] = self.observe_wave_scene(client, session, tick, layers)
                if self.args.gpu is not None:
                    for candidate_tick, camera in VIEW_SAMPLES:
                        if candidate_tick == tick:
                            frames[f'{tick}-{camera}'] = self.capture_wave_scene(
                                client, session, tick, camera, layers, f'{variant}-{offset}-{tick}-{camera}')
                if tick == 0:
                    self.call(client, self.step(session, 0, CHECKPOINT-1, offset, pressed=False))
                    self.call(client, self.step(session, CHECKPOINT-1, 1, offset+1, pressed=True))
                if tick == CHECKPOINT:
                    checkpoint = self.save_bytes(client, saves, session, tick, f'checkpoint-{offset}', offset+2)
                    self.finish(client, session, offset+3)
            terminal = self.save_bytes(client, saves, session, FINAL, f'final-{offset}', offset+5)
            return states, frames, checkpoint, terminal

        with self.owner(f'{variant} warm', game=game, saves=saves) as (client, warm):
            if game:
                session = self.initial(client, revision)
            else:
                client.call('save.configure', dict(request_id=uid(9040), expected_generation=0, root=self.native(saves)))
                session = uid(9041)
                self.call(client, self.start(session, revision))
            physical, captures, checkpoint, terminal = sequence(client, session, 9100, 0)
        if variant == 'layered':
            with self.owner(f'{variant} cold', game=game, saves=saves) as (client, cold):
                if game:
                    active = self.initial(client, revision)
                else:
                    active = None
                    client.call('save.configure', dict(request_id=uid(9042), expected_generation=0, root=self.native(saves)))
                session = uid(9043)
                request = self.load(session, revision, 9044, active=active, active_tick=0)
                request['params']['slot'] = 'checkpoint-9100'
                loaded = self.call(client, request)
                assert loaded['tick'] == CHECKPOINT and loaded['session_id'] == session and not loaded['source_stale'], loaded
                resumed, restored, loaded_bytes, final_bytes = sequence(client, session, 9200, CHECKPOINT)
                assert checkpoint == loaded_bytes and terminal == final_bytes
                assert exact(resumed, {tick: value for tick, value in physical.items() if tick != 0})
                assert exact(restored, {key: value for key, value in captures.items() if not key.startswith('0-')})
            assert warm['pid'] != cold['pid'], (warm, cold)
        return dict(physical=physical, captures=captures)

    def compare_profiles(self, profiles):
        base, zero, active = (profiles[variant] for variant in PROFILES)
        assert exact(base, zero), 'Zero-slope profiles must match unlayered water and dry controls.'
        assert exact(base['physical'], active['physical']), 'Wave layers changed physical water observations.'
        if self.args.gpu is not None:
            hashes_changed = 0
            hdr_change = 0.
            for key, reference_frame in base['captures'].items():
                frame = active['captures'][key]
                hashes_changed += frame['sha256'] != reference_frame['sha256']
                for name, original in reference_frame['probes'].items():
                    sample = frame['probes'][name]
                    if original['cell'] is None:
                        assert exact(sample, original), (key, name, sample, original)
                        continue
                    assert exact(original['point'], sample['point']), (key, name)
                    previous, actual = original['raster'], sample['raster']
                    for field in ('depth', 'surface_valid', 'motion_valid', 'motion'):
                        assert exact(previous[field], actual[field]), (key, name, field)
                    hdr_change = max(hdr_change, math.dist(previous['raw_hdr'], actual['raw_hdr']))
            assert hashes_changed >= 3 and hdr_change > .01, (hashes_changed, hdr_change)
            self.measurements['render_difference'] = dict(frames_changed=hashes_changed, max_hdr_delta=hdr_change)
        for slot in ('checkpoint-9100', 'final-9100'):
            expected = self.measurements['saves']['unlayered saves/'+slot]['water']
            assert all(exact(self.measurements['saves'][variant+' saves/'+slot]['water'], expected)
                       for variant in PROFILES), slot

    def run(self):
        self.measurements.update(capture_reports={}, saves={}, profiles={}, plans={})
        self.measurements['analytic_discrimination'] = reference_discrimination()
        self.oracle = reference()
        revision = self.author()
        params = recipe_parameters(self, revision)
        legacy = [self.cook(params | profile, i) for i, profile in enumerate(LEGACY_PROFILES, start=1)]
        assert tuple(plan['plan_sha256'] for plan in legacy) == LEGACY_HASHES, legacy
        self.measurements['legacy_cooks'] = legacy
        self.admission(params)
        plans = {'unlayered': legacy[0]}
        for variant in ('zero', 'layered'):
            plans[variant] = self.cook(params | {'surface_wave_layers': PROFILES[variant]}, 4)
        reordered = self.cook(params | {'surface_wave_layers': LAYERS[::-1]}, 4)
        assert reordered['plan_sha256'] != plans['layered']['plan_sha256'], reordered
        rain = self.cook(params | {'surface_wave_layers': LAYERS, 'rain_cells': [[0, 0]]}, 4)
        assert rain['plan_sha256'] != plans['layered']['plan_sha256'], rain
        self.measurements.update(plans=plans, different_order=reordered, rain_and_layers=rain)
        exported = {}
        for i, (variant, plan) in enumerate(plans.items()):
            self.plan = plan
            revision = self.bind(variant, revision, plan, 9050+i)
            if self.args.runtime_root:
                exported[variant] = (revision, self.export(variant, revision, plan))
            else:
                self.measurements['profiles'][variant] = self.exercise_profile(variant, revision, None)
        if exported:
            hidden = self.root/'Hidden Water Authoring'
            self.project.rename(hidden)
            source = tree_hashes(hidden)
            for variant, (revision, game) in exported.items():
                self.plan = plans[variant]
                files = tree_hashes(game.parent)
                self.measurements['profiles'][variant] = self.exercise_profile(variant, revision, game)
                assert not self.project.exists() and tree_hashes(hidden) == source
                assert tree_hashes(game.parent) == files
        self.compare_profiles(self.measurements['profiles'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path, help='Matching installed Vulkan runtime for standalone export.')
    parser.add_argument('--gpu', type=int, help='Capture the exported scene on this Windows Vulkan adapter.')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--require-validation', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if args.gpu is not None and (not args.runtime_root or not 0 <= args.gpu <= 4095):
        parser.error('--gpu requires an adapter index in 0 through 4095 and --runtime-root.')
    if args.require_validation and args.gpu is None:
        parser.error('--require-validation requires --gpu.')
    args.output.mkdir(exist_ok=True, parents=True)
    root = Path(tempfile.mkdtemp(prefix='water-wave-layers-', dir=args.output.resolve()))
    test = LayerContract(args, root)
    report = dict(format='poima.water-wave-layers-contract.v1', passed=False,
                  tested_runtime=args.runtime_root is not None, captured=args.gpu is not None,
                  lighting_path=args.lighting_path, gpu=args.gpu, binary_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)), calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        logs = [call['wire']['stderr.log'] for call in test.calls if 'wire' in call]
        validation_errors = ('VUID-', 'Validation Error', 'Validation Warning', 'SYNC-HAZARD', 'Undefined-Value')
        assert logs and all(not any(marker in log for marker in validation_errors) for log in logs)
        if args.require_validation:
            assert all(VALIDATION_LAYER in log for log in logs), 'Vulkan validation did not load for every capture owner.'
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
