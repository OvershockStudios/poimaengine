#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify refracted solar scattering against a closed-form flat-water integral."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from height_fog_capture import Board, author_board, environment, mesh
from water_optics_capture import LENS, fresnel, sightline
from world_particles_contract import ParticleContract, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, tree_hashes
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, rpc, success, uid


FEATURE = 'water_sun_shafts'
STRENGTH = .8
TOP = 7.
EXTINCTION = [.46, .23, .13]
SUN_COLOR = [1., .92, .78]
SUN_INTENSITY = 4.
VIEWS = dict(clear=(uid(8620), [22, 4.5, -3]), shadow=(uid(8621), [17, 4.5, -3]),
             shadow_edge=(uid(8622), [19, 4.5, -3]), dry=(uid(8623), [22, 10, -3]))
SITES = [(x, y) for x in (264, 292, 319, 348, 375) for y in (130, 178, 217)]


def open_water_delta(pixel, depth):
    """Integrate clear vertical light and eye extinction analytically."""
    ray, scale = sightline(pixel)
    distance = scale * LENS['near'] / (1 - depth + depth * LENS['near'] / LENS['far'])
    assert 5.98 < distance / scale < 6.02, distance
    assert TOP - 12 < VIEWS['clear'][1][1] + ray[1]*distance < TOP
    g = .7
    phase = (1 - g*g) / (4 * math.pi * (1 + g*g - 2*g*ray[1])**1.5)
    irradiance = STRENGTH * SUN_INTENSITY * phase * (1 - fresnel(-1))
    initial_depth = TOP - VIEWS['clear'][1][1]
    direction = 1 - ray[1]
    return [irradiance * color * math.exp(-extinction*initial_depth) *
            -math.expm1(-extinction*direction*distance) / direction
            for color, extinction in zip(SUN_COLOR, EXTINCTION, strict=True)]


class SunShaftsFixture(WaterContract):
    owner = ParticleContract.owner

    def apply(self, revision, request, *ops):
        result = success(self.world_calls(self.transaction(revision, request, *ops))[0])
        assert result['revision'] == revision + 1 and result['committed']
        return result['revision']

    def author(self):
        recipe, _, revision = self.arm_terrain()
        lighting = environment()
        lighting['sky']['sun'] = uid(5)
        half = math.sqrt(.5)
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            component(uid(4), 'LightingEnvironment', lighting),
            component(uid(5), 'Transform', transform([0, 0, 0], [-half, 0, 0, half])),
            component(uid(5), 'Light', dict(kind='directional', intensity=SUN_INTENSITY, color=SUN_COLOR, enabled=True,
                shadow=dict(enabled=True, distance=24))),
        ]
        for name, (camera, eye) in VIEWS.items():
            ops += [dict(op='entity.create', id=camera, name=name),
                    component(camera, 'Transform', transform(eye)), component(camera, 'Camera', LENS)]
        ops += author_board(Board(uid(8624), (19.5, 7., -9.1), (18., 13., .2)), 'Opaque viewing limit')
        cover = author_board(Board(uid(8625), (16.9, 8.5, -6.5), (4.2, .16, 10.)), 'Cast shadow into water')
        cover[2] = component(uid(8625), 'MeshRenderer', mesh(cast_shadows=True))
        ops += cover
        revision = self.apply(revision, 8626, *ops)
        optics = dict(extinction_per_m=EXTINCTION, scattered_radiance=[.02, .08, .13], follow_terrain=True,
                      offshore=dict(edge='max_x', surface_height_m=TOP, depth_m=12))
        params = dict(revision=revision, terrain=dict(source=recipe.name), tile_entities=TILES,
                      expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
                      grid=dict(cells_x=4, cells_z=2, cell_size_m=1, origin_x_m=10, origin_z_m=-4,
                                relaxation_per_second=0),
                      initial_depths_m=[TOP - bed(*cell_center(cell)) for cell in CELLS], optics=optics)
        schema = self.world_call('world.describe', view='method', name='world.water.plan')
        assert '"sun_shafts"' in json.dumps(schema)
        for bad in (-.01, 1.01, True, None):
            self.cases(('invalid_strength', rpc('world.water.plan', **(params | dict(optics=optics |
                        dict(sun_shafts=bad)))), -32602))
        plain = self.cook(params, 12)
        assert self.cook(params | dict(optics=optics | dict(sun_shafts=0)), 12) == plain
        enabled = self.cook(params | dict(optics=optics | dict(sun_shafts=STRENGTH)), 25)
        self.measurements['cooked'] = dict(clear=plain, enabled=enabled)
        return revision, plain['plan_sha256'], enabled['plan_sha256']

    def cook(self, params, version):
        planned = self.world_call('world.water.plan', **params)
        pin = planned['plan_sha256']
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=pin) == planned
        asset = self.assets / (pin + '.pwater')
        assert file_hash(asset) == pin and json.loads(asset.read_bytes())['version'] == version
        return planned

    def state_projection(self, client, session, tick, active):
        report = client.call('runtime.water.inspect', self.state(session, tick, points=[v[1] for v in VIEWS.values()])['params'])
        assert report['optics'].get('sun_shafts') == (STRENGTH if active else None)
        assert [p['submersion_m'] for p in report['points']] == [2.5, 2.5, 2.5, 0]
        return {key: report[key] for key in ('cells', 'points', 'volume_m3')}

    def capture(self, client, session, tick, label, view, active, reconstruction='none'):
        path = self.root / (label+'.bmp')
        response = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=VIEWS[view][0],
            path=self.native(path), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1], gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, samples=1, profile=True, capture_frames=2,
            reconstruction=reconstruction, scene_product_probes=[dict(x=x, y=y) for x,y in SITES]))
        assert response['hardware'] and response['capture_written'] and response['frames_presented'] == 2
        assert response['nvrhi_errors'] == 0 and response['tick'] == tick
        diagnostics = response['render_diagnostics']
        water = diagnostics['water_optics']
        assert water['active'] and water['offshore'] and water['follow_terrain']
        assert water.get('sun_shafts') == (dict(active=True, reason='active', strength=STRENGTH) if active else None)
        capture = dict(image=file_hash(path), gpu=response['gpu'], probes=diagnostics['scene_products']['probes'])
        self.measurements[label] = capture
        return capture

    def compare(self, baseline, shafts):
        errors, shadow_fractions, near_boundary = [], [], []
        for view in VIEWS:
            for pixel, old, lit in zip(SITES, baseline[view]['probes'], shafts[view]['probes'], strict=True):
                assert old['raw_hdr'] == lit['raw_hdr'] and old['depth'] == lit['depth']
                delta = [b-a for a,b in zip(old['resolved_hdr'][:3], lit['resolved_hdr'][:3], strict=True)]
                if view == 'dry':
                    assert not any(delta), (pixel, delta)
                    continue
                expected = open_water_delta(pixel, old['depth'])
                if view == 'clear':
                    errors.extend(abs(a-b) for a,b in zip(delta, expected, strict=True))
                elif view == 'shadow':
                    shadow_fractions.extend(actual/clear for actual,clear in zip(delta, expected, strict=True))
                else:
                    near_boundary.append((pixel, sum(delta)/sum(expected)))
        assert max(errors) < .0015, errors
        assert max(shadow_fractions) < .2, shadow_fractions
        by_side = [[ratio for (x,_),ratio in near_boundary if (x<319) is left and x!=319] for left in (True,False)]
        left, right = [sum(values)/len(values) for values in by_side]
        assert left < right - .3, near_boundary
        self.measurements['oracle'] = dict(max_abs_error=max(errors), max_shadow_fraction=max(shadow_fractions),
                                          mean_boundary_fractions=[left,right], exact_dry_probes=len(SITES))

    def restore(self, revision, tick, expected, image, game=None):
        saves = self.root/'saves'
        with self.owner('relocated' if game else 'fresh', game=game, saves=saves if game else None) as (client,_):
            previous = client.call('runtime.status')['session_id'] if game else None
            if not game:
                client.call('save.configure', dict(request_id=uid(8635), expected_generation=0, root=self.native(saves)))
            new_session = uid(8636)
            client.call('save.load', self.load(new_session, revision, 8637, active=previous, active_tick=0)['params'])
            assert self.state_projection(client, new_session, tick, True) == expected
            if image:
                shot = self.capture(client, new_session, tick, 'restored-shadow-edge', 'shadow_edge', True)
                assert shot['image'] == image

    def run(self):
        revision, plain, enabled = self.author()
        tick = 3
        states, images, reconstructed = [], [], []
        for active, pin in enumerate((plain, enabled)):
            revision = self.apply(revision, 8630+active, dict(op='water.set', asset=pin))
            assert self.world_call('world.dependencies').get('needs_'+FEATURE, False) == bool(active)
            session = uid(8632+active)
            with self.owner('enabled' if active else 'omitted') as (client,_):
                client.call('runtime.start', self.start(session, revision)['params'])
                client.call('runtime.step', self.step(session, 0, tick, 8640+active)['params'])
                states.append(self.state_projection(client, session, tick, bool(active)))
                if self.args.gpu is not None:
                    images.append({view:self.capture(client,session,tick,f'{active}-{view}',view,bool(active)) for view in VIEWS})
                    if self.args.fsr:
                        reconstructed.append(self.capture(client,session,tick,f'{active}-native-aa',
                            'shadow_edge',bool(active),reconstruction='fsr3_native'))
                if active:
                    saves = self.root/'saves'
                    saves.mkdir()
                    client.call('save.configure', dict(request_id=uid(8642), expected_generation=0, root=self.native(saves)))
                    client.call('save.write', self.write(session,tick,'checkpoint',8643)['params'])
        assert states[0] == states[1]
        expected_image = None
        if images:
            self.compare(*images)
            expected_image = images[1]['shadow_edge']['image']
        if reconstructed:
            assert reconstructed[0]['image'] != reconstructed[1]['image']
        if self.args.runtime_root:
            self.relocate(revision, tick, states[1], expected_image)
        else:
            self.restore(revision, tick, states[1], expected_image)

    def relocate(self, revision, tick, state, expected_image):
        original, moved = self.root/'Export', self.root/'Relocated'
        older = self.root/'Runtime without shafts'
        older.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop(FEATURE)
        (older/'runtime.json').write_text(json.dumps(descriptor), encoding='utf-8')
        build = ['project','build',self.native(self.manifest),'--output',self.native(original),'--runtime']
        rejection = subprocess.run([str(self.args.binary.resolve()),*build,self.native(older)],
                                   capture_output=True, text=True, timeout=60)
        assert rejection.returncode == 4 and not original.exists(), rejection
        assert 'Underwater sun shafts require' in rejection.stdout
        success(json.loads(self.cli(*build,self.native(self.args.runtime_root))))
        original.rename(moved)
        inventory = tree_hashes(moved)
        parked = self.root/'Hidden source'
        self.project.rename(parked)
        try:
            self.restore(revision,tick,state,expected_image,moved/'game.json')
            assert tree_hashes(moved)==inventory
        finally:
            parked.rename(self.project)
        self.measurements['export'] = dict(files=len(inventory), immutable=True, incompatible_refused=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward','deferred'), default='forward')
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--fsr', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = SunShaftsFixture(args, Path(tempfile.mkdtemp(prefix='sun-shafts-', dir=args.output)))
    evidence = dict(passed=False, calls=fixture.calls, measurements=fixture.measurements)
    try:
        fixture.run()
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = fixture.root/'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
