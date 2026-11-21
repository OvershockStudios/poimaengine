#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify relocated Vulkan wetness color, geometry and save continuation."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from world_particles_contract import near_vector
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, success, tree_hashes
from world_wetness_contract import (
    AMBIENT, BASE, CAMERA, CAMERA_LENS, CAMERA_POSITION, CAMERA_ROTATION, COVERED, INTENSITY,
    MATERIAL, MOVING_ROOF, MOVING_ROOF_SCALE, OPEN, PHASES, PROFILE, PROP, RECEIVERS, ROOF,
    ROOF_SCALE, Reference, WetnessContract,
)


SURFACES = (*RECEIVERS, ROOF)
NAMES = {OPEN: 'open', COVERED: 'covered', PROP: 'prop', ROOF: 'roof'}
ROOF_BASE = (.18, .12, .09)
HDR_TOLERANCE = .002
CAMERA_DEPTH_TOLERANCE_M = .0002


def camera_matrix():
    pitch = 2*math.atan2(CAMERA_ROTATION[0], CAMERA_ROTATION[3])
    c, s = math.cos(pitch), math.sin(pitch)
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, *CAMERA_POSITION, 1]


def ray(camera, pixel):
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(CAMERA_LENS['vertical_fov'])/2))
    u, v = (pixel[0]+.5-width/2)/focal, (height/2-pixel[1]-.5)/focal
    return [camera[i]*u+camera[i+4]*v-camera[i+8] for i in range(3)]


def unit(vector):
    length = math.hypot(*vector)
    return tuple(component/length for component in vector)


def material_at(state, entity):
    dry = BASE if entity in RECEIVERS else ROOF_BASE
    amount = float(state.film.get(entity, 0)/PROFILE['capacity_nm'])
    base = tuple(channel*(1-amount*PROFILE['darkening']) for channel in dry)
    roughness = MATERIAL['roughness']*(1-amount*(1-PROFILE['roughness_factor']))
    return base, roughness


def brightness(point, base, roughness):
    """Isotropic GGX with white normal-incidence light and a flat top face."""
    view = unit(tuple(eye-p for eye, p in zip(CAMERA_POSITION, point, strict=True)))
    halfway = unit((view[0], view[1]+1, view[2]))
    nv, nh = view[1], halfway[1]
    vh = sum(v*h for v, h in zip(view, halfway, strict=True))
    alpha2 = roughness**4
    distribution = alpha2/(math.pi*(nh*nh*(alpha2-1)+1)**2)
    visibility = .5/(math.sqrt(nv*nv*(1-alpha2)+alpha2)+nv)
    fresnel = .04+.96*(1-vh)**5
    return tuple(channel*AMBIENT+INTENSITY*((1-fresnel)*channel/math.pi+distribution*visibility*fresnel)
                 for channel in base)


def box_entry(origin, direction, center, scale):
    """Entry distance into an axis-aligned box for the fixture camera ray."""
    lower, upper = 0., math.inf
    for start, axis, middle, size in zip(origin, direction, center, scale, strict=True):
        if abs(axis) < 1e-12:
            if abs(start-middle) > size/2:
                return None
            continue
        first, last = sorted(((middle-size/2-start)/axis, (middle+size/2-start)/axis))
        lower, upper = max(lower, first), min(upper, last)
    return lower if lower < upper else None


def references(state):
    """Choose interior samples and prove the known cover cannot hide them."""
    width, height = CAPTURE_SIZE
    camera = camera_matrix()
    scale = {ROOF: ROOF_SCALE, MOVING_ROOF: MOVING_ROOF_SCALE}
    origin = CAMERA_POSITION
    all_solids = (*SURFACES, MOVING_ROOF)
    result = {}
    for entity in SURFACES:
        center = state.positions[entity]
        sx, sy, sz = scale.get(entity, (1, 1, 1))
        y = center[1]+sy/2
        locations = (('center', 0, 0), ('corner', .16, -.19))
        for label, dx, dz in locations:
            target = (center[0]+dx, y, center[2]+dz)
            pixel = tuple(map(math.floor, projection(camera, CAMERA_LENS, target)))
            assert 0 <= pixel[0] < width and 0 <= pixel[1] < height, pixel
            direction = ray(camera, pixel)
            assert direction[1] < 0, direction
            depth = (y-origin[1])/direction[1]
            point = tuple(p+d*depth for p, d in zip(origin, direction, strict=True))
            assert abs(point[0]-center[0]) < .4*sx and abs(point[2]-center[2]) < .4*sz, point
            hits = sorted((distance, other) for other in all_solids
                          if (distance := box_entry(origin, direction, state.positions[other],
                                                   scale.get(other, (1, 1, 1)))) is not None)
            assert hits and hits[0][1] == entity and math.isclose(hits[0][0], depth, abs_tol=1e-9), (label, hits)
            base, roughness = material_at(state, entity)
            hdr = brightness(point, base, roughness)
            dry_base = BASE if entity in RECEIVERS else ROOF_BASE
            result[f'{NAMES[entity]}_{label}'] = dict(
                pixel=pixel, entity=entity, point=point, camera_depth=depth, ray_direction=unit(direction),
                base=base, roughness=roughness, expected_hdr=hdr, expected_bmp=tuple(map(display, hdr)),
                darkening_only=brightness(point, base, MATERIAL['roughness']),
                roughness_only=brightness(point, dry_base, roughness))
    assert len({row['pixel'] for row in result.values()}) == len(result)
    return result


class WetnessCapture(WetnessContract):
    def material_baseline(self, client, session, tick):
        inspected = client.call('runtime.material.inspect', dict(session_id=session, tick=tick, material_revision=0,
                                                                 ids=sorted(SURFACES)))
        assert exact([inspected['session_id'], inspected['tick'], inspected['material_revision']],
                     [session, tick, 0]), inspected
        assert inspected['source_stale'] is False, inspected
        assert inspected['authored_revision'] == self.revision, inspected
        assert [row['id'] for row in inspected['materials']] == sorted(SURFACES), inspected
        for row in inspected['materials']:
            material = row['material']
            near_vector(material['base_color'], ROOF_BASE if row['id'] == ROOF else BASE, tolerance=1e-6)
            assert math.isclose(material['roughness'], MATERIAL['roughness'], abs_tol=1e-6)
        baseline = {k: v for k, v in inspected.items() if k not in ('session_id', 'tick')}
        first = self.measurements.setdefault('frozen_materials', baseline)
        assert exact(baseline, first), (baseline, first)

    def observe_phase(self, client, session, state, label):
        reading = super().observe_phase(client, session, state, label)
        tick = state.tick
        self.material_baseline(client, session, tick)
        witnesses = references(state)
        camera = client.call('runtime.entity', dict(session_id=session, tick=tick, id=CAMERA))['world_matrix']
        near_vector(camera, camera_matrix(), tolerance=1e-8)
        width, height = CAPTURE_SIZE
        output = self.root/(label+'.bmp')
        report = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(output), width=width, height=height, samples=1, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=row['pixel'][0], y=row['pixel'][1]) for row in witnesses.values()]))
        assert report['hardware'] and report['capture_written'] and report['frames_presented'] > 0, report
        assert report['nvrhi_errors'] == 0 and report['tick'] == tick and report['lens'] == CAMERA_LENS, report
        assert report['revision'] == self.revision and report['camera_world'] == camera, report
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == 'color', products
        bitmap = pixels(output)
        assert len(bitmap) == height and all(len(row) == width for row in bitmap)
        record = dict(tick=tick, gpu=report['gpu'], lighting_path=self.args.lighting_path,
                      sha256=file_hash(output), probes={})
        self.measurements.setdefault('captures', {})[label] = record
        self.measurements.setdefault('capture_reports', {})[label] = report
        near_plane, far_plane = CAMERA_LENS['near'], CAMERA_LENS['far']
        for (name, expected), probe in zip(witnesses.items(), products['probes'], strict=True):
            x, y = expected['pixel']
            item = record['probes'][name] = dict(expected, rendered=probe, bmp_rgb=bitmap[y][x])
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), item
            assert probe['surface_valid'] is True, item
            near_vector(probe['shading_normal'][:3], [0, 1, 0], tolerance=.002)
            actual_distance = near_plane/(1-probe['depth']*(far_plane-near_plane)/far_plane)
            item['depth_error_m'] = abs(actual_distance-expected['camera_depth'])
            assert item['depth_error_m'] < CAMERA_DEPTH_TOLERANCE_M, item
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick, origin=CAMERA_POSITION,
                direction=list(expected['ray_direction']), distance=CAMERA_LENS['far']))['hit']
            assert hit['entity'] == expected['entity'], (hit, item)
            near_vector(hit['position'], expected['point'], tolerance=.0001)
            near_vector(hit['normal'], [0, 1, 0], tolerance=.002)
            item['collision'] = hit
            actual = probe['raw_hdr']
            assert exact(actual, probe['resolved_hdr']), item
            near_vector(actual, expected['expected_hdr'], tolerance=HDR_TOLERANCE)
            assert max(abs(a-b) for a, b in zip(item['bmp_rgb'], expected['expected_bmp'], strict=True)) <= 2, item
            if state.film.get(expected['entity']) == PROFILE['capacity_nm']:
                assert distance(actual, expected['darkening_only']) > .005, item
                assert distance(actual, expected['roughness_only']) > .10, item
        return reading

    def compare_frames(self):
        captures = self.measurements['captures']
        comparisons = {}
        initial = captures['warm initial']['probes']
        for phase in PHASES:
            label = phase.name
            warm = captures['warm '+label]
            if 'cold '+label in captures:
                assert exact(warm, captures['cold '+label]), label
                comparisons[label] = warm['sha256']
            for name, dry in initial.items():
                current = warm['probes'][name]
                if current['entity'] in (OPEN, COVERED, ROOF):
                    fields = ('depth', 'shading_normal')
                    assert all(exact(current['rendered'][key], dry['rendered'][key]) for key in fields), name
                if current['entity'] in (COVERED, ROOF):
                    assert exact(current['rendered']['raw_hdr'], dry['rendered']['raw_hdr']), name
                    assert exact(current['bmp_rgb'], dry['bmp_rgb']), name
        assert len(comparisons) == len(PHASES)-1
        self.measurements['exact_fresh_captures'] = comparisons
        a = captures['warm rain']['probes']['open_center']['bmp_rgb']
        b = captures['warm under-roof']['probes']['open_center']['bmp_rgb']
        assert all(before-after >= 8 for before, after in zip(a, b, strict=True)), (a, b)
        a = captures['warm under-roof']['probes']['prop_center']['bmp_rgb']
        b = captures['warm drying']['probes']['prop_center']['bmp_rgb']
        assert all(after-before >= 5 for before, after in zip(a, b, strict=True)), (a, b)
        assert captures['warm initial']['sha256'] != captures['warm rain']['sha256']

    def run(self):
        validate_references()
        self.revision = self.author()
        output = self.root/'Initial Wetness Export'
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        relocated, detached = self.root/'Relocated Wetness Game', self.root/'Unavailable Wetness Source'
        output.rename(relocated)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        bundle_identity, source_identity = tree_hashes(relocated), tree_hashes(detached)
        game = relocated/'game.json'
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=bundle_identity,
                                           detached_source=source_identity)
        self.exercise(self.revision, game=game)
        self.compare_frames()
        assert not self.project.exists() and not output.exists()
        assert tree_hashes(relocated) == bundle_identity and tree_hashes(detached) == source_identity


def distance(a, b):
    return max(abs(x-y) for x, y in zip(a, b, strict=True))


def validate_references():
    """Keep the geometry and the material contrast checkable before GPU runs."""
    state = Reference()
    references(state)
    for phase in PHASES:
        state.finish(phase)
        known = references(state)
        if phase.name == 'saturation':
            for row in known.values():
                if row['entity'] in (OPEN, PROP):
                    assert distance(row['expected_hdr'], row['darkening_only']) > .007, row
                    assert distance(row['expected_hdr'], row['roughness_only']) > .11, row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Windows authoring and runtime executable.')
    parser.add_argument('--runtime-root', type=Path, required=True, help='Installed Windows runtime for export.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='wetness-render-', dir=args.output.resolve()))
    test = WetnessCapture(args, root)
    result = dict(format='poima.world-wetness-capture.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, binary_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)),
                  reference_sha256=file_hash(Path(__file__).with_name('world_wetness_contract.py')),
                  calls=test.calls, measurements=test.measurements)
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
