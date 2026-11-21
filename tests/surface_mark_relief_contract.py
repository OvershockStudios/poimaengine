#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify planted surface relief, geometric invariants and relocated save replay."""

import argparse
import json
import math
from pathlib import Path
import struct
import tempfile
import traceback
from uuid import uuid4

from footprint_surface_contract import (
    ACTOR, BASE, CAMERA, CHECKPOINT, FINAL, LENS, MASK, MOVED_CENTER, MOVED_ROTATION,
    PAD, PAD_SURFACE, PLAIN, SPARE, TERRAIN, TERRAIN_PLANT, TERRAIN_SURFACE, VISIBLE,
    FootprintContract, manual_stamp, motion, pose,
)
from hdr_composition_capture import display
from imported_character_contract import point
from input_actions_gameplay_contract import exact
from runtime_ground_contacts import cross, dot, unit, vector
from scene_capture import pixels
from textured_surface_marks_contract import sample_mask
from world_particles_contract import component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, error, file_hash, projection, rpc, success, tree_hashes, uid


MAX_RELIEF = .025
PAD_RELIEF = PAD_SURFACE | dict(relief_depth_m=.015625, opacity=0, edge_fraction=0)
SLOPE_RELIEF = TERRAIN_SURFACE | dict(relief_depth_m=.021484375, opacity=0, edge_fraction=0)
CAMERA_SLOPE = uid(916)
RED_LIGHT, BLUE_LIGHT = uid(917), uid(918)
GRAZE = {RED_LIGHT: unit([1, .22, 0]), BLUE_LIGHT: unit([-1, .22, 0])}
AMBIENT, LIGHT_INTENSITY, ROUGHNESS = .12, 3, .8
FLAT_NORMAL = [0, 1, 0]
SLOPE_NORMAL = unit([-.05, 1, 0])
UPRIGHT = [-math.sqrt(.5), 0, 0, math.sqrt(.5)]


def binary32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def difference(a, b):
    return [x-y for x, y in zip(a, b, strict=True)]


def offset(a, b, factor):
    return [x+factor*y for x, y in zip(a, b, strict=True)]


def camera_ray(camera, x, y):
    width, height = CAPTURE_SIZE
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    return vector(camera, [(x+.5-width/2)/focal, (height/2-y-.5)/focal, -1])


def intersection(camera, normal, anchor, x, y):
    origin = camera[12:15]
    ray = camera_ray(camera, x, y)
    distance = dot(normal, difference(anchor, origin))/dot(normal, ray)
    return offset(origin, ray, distance), distance


def depth_at(distance):
    near, far = LENS['near'], LENS['far']
    return far/(far-near)*(1-near/distance)


def fade(start, stop, width):
    ramp = max(0., min(1., (width-start)/(stop-start)))
    return 1-ramp*ramp*(3-2*ramp)


def stamp_frame(support, mark):
    center = point(support, mark['center_m'])
    axis = unit(vector(support, mark['tangent']))
    normal = unit(vector(support, mark['normal']))
    return center, axis, cross(normal, axis)


def footprint_normal(support, probes, samples, marks, base_normal, mask):
    """Differentiate the authored height field on the receiver, in world metres."""
    position, side, next_row = probes
    total = [0., 0., 0.]
    # A central difference of values supplies an independent check on the shader's
    # analytic derivative. Witnesses stay away from exact mask-cell boundaries.
    radius_epsilon_m = 1e-6
    for mark in marks:
        depth = mark.get('relief_depth_m', 0.)
        if depth == 0:
            continue
        center, u_axis, v_axis = stamp_frame(support, mark)
        half_u, half_v = mark['half_extent_m']

        def project(delta):
            return [dot(delta, u_axis)/half_u, dot(delta, v_axis)/half_v]

        u, v = project(difference(position, center))
        radius2 = u*u+v*v
        if radius2 >= 1:
            continue
        screen = [project(difference(neighbor, position)) for neighbor in (side, next_row)]
        radial_fade = fade(.04, .22, max(math.hypot(*pair) for pair in screen))
        if 'profile' in mark:
            height, width = len(mask), len(mask[0])
            texture_fade = fade(1, 3, max(math.hypot(a*width/2, b*height/2) for a, b in screen))
            m = sample_mask(mask, u, v)
        else:
            texture_fade = 0.
            m = 1.
        samples.append(dict(mark=mark['id'], uv=[u, v], value=m,
                            radius_fade=radial_fade, texel_fade=texture_fade))

        def envelope(uv):
            return max(0, 1-uv[0]**2-uv[1]**2)**2

        for index, (axis, half) in enumerate(((u_axis, half_u), (v_axis, half_v))):
            h = radius_epsilon_m/half
            left = (u-h, v) if index == 0 else (u, v-h)
            right = (u+h, v) if index == 0 else (u, v+h)

            derivative = (envelope(right)-envelope(left))*m
            if 'profile' in mark:
                derivative += envelope((u, v))*texture_fade*(
                    sample_mask(mask, *right)-sample_mask(mask, *left))
            total = offset(total, axis, -depth*radial_fade*derivative/(2*radius_epsilon_m))
    total = offset(total, base_normal, -dot(total, base_normal))
    size = math.hypot(*total)
    return unit(offset(base_normal, total, -min(1, .75/size))) if size else base_normal


def expected_radiance(position, normal, camera):
    """Evaluate independent dielectric microfacet lighting for the two colored lights."""
    view = unit(difference(camera[12:15], position))
    alpha_squared = ROUGHNESS**4
    result = [AMBIENT*value for value in BASE]
    for lane, light in ((0, GRAZE[RED_LIGHT]), (2, GRAZE[BLUE_LIGHT])):
        incident = max(0., dot(normal, light))
        if incident == 0:
            continue
        half = unit(offset(view, light, 1))
        reflected = max(1e-5, dot(normal, view))
        distribution = alpha_squared / (math.pi*(1+(alpha_squared-1)*dot(normal, half)**2)**2)
        projected_light = math.sqrt(alpha_squared+(1-alpha_squared)*incident**2)
        projected_view = math.sqrt(alpha_squared+(1-alpha_squared)*reflected**2)
        smith = .5/(incident*projected_view+reflected*projected_light)
        fresnel = .04+.96*(1-dot(view, half))**5
        brdf = (1-fresnel)*BASE[lane]/math.pi+distribution*smith*fresnel
        result[lane] += LIGHT_INTENSITY*incident*brdf
    return result


class ReliefContract(FootprintContract):
    def author(self):
        super().author()
        focused = self.world_call('world.describe', view='component', name='FootprintSurface')
        depth = focused['components']['FootprintSurface']['properties']['relief_depth_m']
        assert (depth['type'], depth['minimum'], depth['maximum']) == ('number', 0, MAX_RELIEF)
        ops = [component(PAD, 'FootprintSurface', PAD_RELIEF),
               component(TERRAIN, 'FootprintSurface', SLOPE_RELIEF),
               component(uid(4), 'LightingEnvironment', dict(ambient=[AMBIENT]*3, exposure=1)),
               component(CAMERA, 'Transform', transform([MOVED_CENTER[0], 1.25, MOVED_CENTER[2]], UPRIGHT)),
               dict(op='entity.create', id=CAMERA_SLOPE, name='Sloped relief witness camera'),
               component(CAMERA_SLOPE, 'Transform', transform([-2, 1.25, 0], UPRIGHT)),
               component(CAMERA_SLOPE, 'Camera', LENS)]
        for entity, lane in ((RED_LIGHT, 0), (BLUE_LIGHT, 2)):
            x, y, _ = GRAZE[entity]
            rotation = [-y*math.sqrt(.5), x*math.sqrt(.5), 0, math.sqrt(.5)]
            color = [int(i == lane) for i in range(3)]
            ops.extend((dict(op='entity.create', id=entity, name='Grazing light '+str(lane)),
                        component(entity, 'Transform', transform([0, 0, 0], rotation)),
                        component(entity, 'Light', dict(kind='directional', enabled=True,
                                                       color=color, intensity=LIGHT_INTENSITY))))
        self.edit(ops)
        authored = self.world.read_bytes()
        invalid = {}
        for label, value in (('negative', -.0001), ('too_deep', MAX_RELIEF+1e-9), ('boolean', True)):
            reply, = self.world_calls(rpc('world.transact', request_id=uuid4().hex, base_revision=self.revision,
                                          ops=[component(PAD, 'FootprintSurface',
                                                         PAD_RELIEF | dict(relief_depth_m=value))]))
            invalid[label] = error(reply, -32602)
            assert self.world.read_bytes() == authored
        self.measurements.update(authored_revision=self.revision, relief_authoring_rejections=invalid)

    def relief_archive(self, client, saves, session, tick, request, slot, version, next_id):
        raw = self.archive(client, saves, session, tick, request, slot)
        packed = json.loads(raw)['snapshot']['payload']['surface_marks']
        observed = self.marks(client, session, tick)
        lifecycle = observed['lifecycle']
        expected = dict(version=version, next_id=next_id,
                        marks=[row | dict(id=int(row['id'])) for row in observed['marks']])
        if lifecycle is not None:
            expected['lifecycle'] = lifecycle
        assert exact(packed, expected), packed
        self.measurements.setdefault('relief_archives', {})[slot] = dict(
            mark_version=version,
            active_relief_depths_m=[row['relief_depth_m'] for row in packed['marks'] if 'relief_depth_m' in row],
            lifecycle_present='lifecycle' in packed)
        return raw

    def presence(self, client, session, tick, rows, first_id, receiver, style, source_offset):
        self.planted_rows(client, session, tick, receiver, rows, first_id, style, source_offset)
        assert all(row['relief_depth_m'] == binary32(style['relief_depth_m']) and row['opacity'] == 0
                   for row in rows), rows

    def witness_pixels(self, support, marks, camera):
        witnesses = {}
        # Sample each textured or analytic depression away from bilinear texel
        # seams. The control falls on the same unscaled receiver outside all stamps.
        for index, mark in enumerate(marks):
            center, u, v = stamp_frame(support, mark)
            radius_u, radius_v = mark['half_extent_m']
            for name, a, b in (('heel', -.47, .22), ('toe', .46, .19),
                               ('inner', .19, -.38), ('outer', -.13, .41)):
                target = offset(offset(center, u, a*radius_u), v, b*radius_v)
                witnesses[f'{index}-{name}'] = tuple(map(math.floor, projection(camera, LENS, target)))
        control = point(support, [0, .5, -.33]) if marks[0]['receiver'] == PAD else [-2, .05, -.33]
        witnesses['outside'] = tuple(map(math.floor, projection(camera, LENS, control)))
        assert len(set(witnesses.values())) == len(witnesses), witnesses
        return witnesses

    def relief_capture(self, client, session, tick, label, receiver, footprint_marks, active):
        box = receiver == PAD
        camera_id = CAMERA if box else CAMERA_SLOPE
        camera = self.entity_matrix(client, session, tick, camera_id)
        support = self.entity_matrix(client, session, tick, receiver)
        normal = FLAT_NORMAL if box else SLOPE_NORMAL
        anchor = point(support, [0, .5, 0]) if box else [-2, .05, 0]
        witnesses = self.witness_pixels(support, footprint_marks, camera)
        width, height = CAPTURE_SIZE
        path = self.root/(label+'.bmp')
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=camera_id, path=self.native(path), width=width, height=height,
            samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
            reconstruction='none', capture_frames=2,
            scene_product_probes=[dict(x=x, y=y) for x, y in witnesses.values()]))
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera and receipt['nvrhi_errors'] == 0, receipt
        diagnostics = receipt['render_diagnostics']
        products = diagnostics['scene_products']
        assert diagnostics['lighting_path'] == self.args.lighting_path and products['available'], diagnostics
        image = pixels(path)
        observed = {}
        for (name, (x, y)), probe in zip(witnesses.items(), products['probes'], strict=True):
            assert (probe['x'], probe['y'], probe['resolved_x'], probe['resolved_y']) == (x, y, x, y), probe
            assert probe['surface_valid'], probe
            assert probe['motion_valid'] and math.hypot(*probe['motion']) < 1e-6, probe
            assert probe['raw_hdr'] == probe['resolved_hdr'], probe
            location, distance = intersection(camera, normal, anchor, x, y)
            dx, dy = 1-2*(x%2), 1-2*(y%2)
            side, _ = intersection(camera, normal, anchor, x+dx, y)
            next_row, _ = intersection(camera, normal, anchor, x, y+dy)
            gradient_inputs = []
            expected_normal = footprint_normal(support, (location, side, next_row), gradient_inputs,
                                               footprint_marks if active else [], normal, MASK)
            expected_hdr = expected_radiance(location, expected_normal, camera)
            actual_normal = probe['shading_normal'][:3]
            # Deferred octahedral encoding and half-float products round their
            # components. Compare a physical angular witness after unit length.
            near_vector(unit(actual_normal), expected_normal, tolerance=.004)
            assert abs(math.hypot(*actual_normal)-1) <= 2**-10, probe
            assert abs(probe['depth']-depth_at(distance)) < 2e-6, probe
            near_vector(probe['resolved_hdr'], expected_hdr, tolerance=.006)
            rgb = image[y][x]
            assert max(abs(a-display(b)) for a, b in zip(rgb, expected_hdr, strict=True)) <= 3, probe
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick,
                origin=offset(location, normal, .2), direction=offset([0., 0., 0.], normal, -1), distance=.4))['hit']
            assert hit['entity'] == receiver, hit
            near_vector(hit['position'], location)
            near_vector(hit['normal'], normal)
            observed[name] = dict(pixel=[x, y], depth=probe['depth'], rgb=rgb, hdr=probe['resolved_hdr'],
                                  normal=actual_normal, motion=probe['motion'], motion_valid=probe['motion_valid'],
                                  ray_hit=hit, expected_normal=expected_normal, expected_hdr=expected_hdr,
                                  expected_depth=depth_at(distance), gradient_inputs=gradient_inputs)
        record = dict(sha256=file_hash(path), tick=tick, probes=observed,
                      gpu=receipt['gpu'], lighting_path=diagnostics['lighting_path'])
        self.measurements.setdefault('captures', {})[label] = record
        return record

    def compare_captures(self, marked, expired):
        positive = negative = 0
        for name, before in marked['probes'].items():
            after = expired['probes'][name]
            for field in ('pixel', 'depth', 'motion', 'motion_valid', 'ray_hit'):
                assert exact(before[field], after[field]), (name, field, before, after)
            # The two direct lights affect only red and blue. Green independently
            # witnesses the unchanged ambient-only base, including the mask.
            assert before['rgb'][1] == after['rgb'][1] and before['hdr'][1] == after['hdr'][1], (before, after)
            if name == 'outside':
                assert not before['gradient_inputs'] and exact(before, after), (before, after)
            else:
                red, _, blue = difference(before['hdr'], after['hdr'])
                positive += int(red > .02 and blue < -.02)
                negative += int(blue > .02 and red < -.02)
        assert positive and negative, (positive, negative, marked, expired)
        return dict(toward_red=positive, toward_blue=negative)

    def continuation(self, client, session, initial, saves, suffix):
        tick = CHECKPOINT
        prior = self.contacts(client, session, tick)
        tick = self.step(client, session, tick, 3, animations=[pose(48)], motions=[motion(ACTOR, [2, 0, 0], 3)])
        assert tick == VISIBLE
        self.lifted(client, session, tick, prior, [2, 0, 0])
        assert exact(self.marks(client, session, tick), initial)
        pad_marks = initial['marks'][1:]
        if self.args.gpu is not None:
            pad_lit = self.relief_capture(client, session, tick, 'pad-lit-'+suffix, PAD, pad_marks, True)
        tick = self.step(client, session, tick, animations=[pose(36)])
        plain = self.contacts(client, session, tick)
        assert all(foot['planting']['status'] == 'locked' and foot['planting']['anchor']['support'] == PLAIN
                   and foot['planting']['anchor']['acquired_tick'] == tick for foot in plain), plain
        assert exact(self.marks(client, session, tick), initial)
        tick = self.step(client, session, tick, 4, animations=[pose(48)], motions=[motion(ACTOR, [-2, 0, 0], 4)])
        self.lifted(client, session, tick, plain, [-2, 0, 0])
        tick = self.step(client, session, tick, animations=[pose(36)])
        assert tick == TERRAIN_PLANT
        slope = self.marks(client, session, tick)
        assert exact(slope['marks'][:3], initial['marks'])
        slope_marks = slope['marks'][3:]
        self.presence(client, session, tick, slope_marks, 4, TERRAIN, SLOPE_RELIEF, [-2, 0, 0])
        tick = self.step(client, session, tick, 2, animations=[pose(48)], motions=[motion(ACTOR, [-2, 0, 2], 2)])
        assert exact(self.marks(client, session, tick), slope)
        if self.args.gpu is not None:
            slope_lit = self.relief_capture(client, session, tick, 'slope-lit-'+suffix, TERRAIN, slope_marks, True)
        tick = self.step(client, session, tick, 2)
        retired_slope = self.marks(client, session, tick)
        assert exact(retired_slope['marks'], initial['marks'])
        assert retired_slope['lifecycle'] == dict(expired=2, recycled=0, dropped=0)
        if self.args.gpu is not None:
            slope_clear = self.relief_capture(client, session, tick, 'slope-clear-'+suffix, TERRAIN, slope_marks, False)
        tick = self.step(client, session, tick, 2)
        assert tick == FINAL
        final = self.marks(client, session, tick)
        assert exact(final['marks'], initial['marks'][:1])
        assert final['lifecycle'] == dict(expired=4, recycled=0, dropped=0)
        if self.args.gpu is not None:
            pad_clear = self.relief_capture(client, session, tick, 'pad-clear-'+suffix, PAD, pad_marks, False)
            self.measurements.setdefault('grazing_responses', {})[suffix] = dict(
                masked_pad=self.compare_captures(pad_lit, pad_clear),
                analytic_slope=self.compare_captures(slope_lit, slope_clear))
        archive = self.relief_archive(client, saves, session, tick, 730 if suffix == 'warm' else 731,
                                      'final-'+suffix, 3, 6)
        return dict(slope=slope, retired_slope=retired_slope, final=final), archive

    def check_lighting(self, client, session):
        info = client.call('runtime.lighting', dict(session_id=session, tick=0))
        assert (info['session_id'], info['tick']) == (session, 0), info
        lights = info['lights']
        assert {light['id'] for light in lights} == set(GRAZE), lights
        for light in lights:
            near_vector(unit(light['direction']), [-v for v in GRAZE[light['id']]])
            assert light['kind'] == 'directional' and light['intensity'] == LIGHT_INTENSITY, light

    def verify(self, game=None):
        saves = self.root/'External relief slots'
        saves.mkdir()
        with self.owner('warm relief owner', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, self.revision, uuid4().hex, saves, game)
            self.check_lighting(client, session)
            assert self.marks(client, session, 0) == dict(marks=[], lifecycle=None)
            previous = self.archive(client, saves, session, 0, 719, 'before-rejected-step', has_marks=False)
            zero = manual_stamp() | dict(relief_depth_m=0, opacity=0)
            for invalid in (-.001, MAX_RELIEF+1e-9, True):
                self.reject(client, 'runtime.step', dict(request_id=uuid4().hex, session_id=session,
                    expected_tick=0, expected_structure_revision=0, ticks=1,
                    surface_marks=[zero, zero | dict(relief_depth_m=invalid)]), -32602)
            assert self.marks(client, session, 0) == dict(marks=[], lifecycle=None)
            assert all(foot['grounding']['casts'] == 0 for foot in self.contacts(client, session, 0))
            assert self.archive(client, saves, session, 0, 720, 'after-rejected-step', has_marks=False) == previous
            tick = self.step(client, session, 0, surface_marks=[zero], retry=True)
            initial = self.marks(client, session, tick)
            permanent, *automatic = initial['marks']
            assert permanent['id'] == '1' and 'relief_depth_m' not in permanent and 'expires_tick' not in permanent
            self.presence(client, session, tick, automatic, 2, PAD, PAD_RELIEF, [0, 0, 0])
            tick = self.step(client, session, tick, 3, motions=[motion(PAD, MOVED_CENTER, 3, MOVED_ROTATION)])
            assert tick == CHECKPOINT
            self.confirm_pad_transport(client, session, tick, initial)
            checkpoint = self.relief_archive(client, saves, session, tick, 721, 'checkpoint', 4, 4)
            expected, final = self.continuation(client, session, initial, saves, 'warm')
        with self.owner('cold relief owner', game=game, saves=saves) as (client, cold):
            fresh = self.initialize(client, self.revision, uuid4().hex, saves, game)
            session = uuid4().hex
            assert self.load(client, self.revision, fresh, session, 'checkpoint', CHECKPOINT, 722) == 0
            self.confirm_pad_transport(client, session, CHECKPOINT, initial)
            assert self.relief_archive(client, saves, session, CHECKPOINT, 723, 'restored-checkpoint', 4, 4) == checkpoint
            observed, restored = self.continuation(client, session, initial, saves, 'cold')
            assert restored == final and exact(observed, expected), observed
            if self.args.gpu is not None:
                captures = self.measurements['captures']
                for label in ('pad-lit', 'pad-clear', 'slope-lit', 'slope-clear'):
                    assert exact(captures[label+'-warm'], captures[label+'-cold']), label
        assert warm['pid'] != cold['pid']
        self.measurements['replay'] = dict(initial=initial, **expected, cold_continuation_exact=True)

    def permanent_relief(self):
        saves = self.root/'Permanent relief slots'
        saves.mkdir()
        with self.owner('permanent relief owner', saves=saves) as (client, _):
            session = self.initialize(client, self.revision, uuid4().hex, saves, None)
            control = manual_stamp(receiver=SPARE, x=4, z=.1) | dict(opacity=0, relief_depth_m=0)
            changed = manual_stamp(receiver=SPARE, x=4, z=-.2) | dict(opacity=0, relief_depth_m=MAX_RELIEF)
            tick = self.step(client, session, 0, animations=[pose(48)], surface_marks=[control])
            self.relief_archive(client, saves, session, tick, 740, 'zero-depth-v1', 1, 2)
            tick = self.step(client, session, tick, surface_marks=[changed])
            marks = self.marks(client, session, tick)
            assert marks['lifecycle'] is None and len(marks['marks']) == 2, marks
            assert 'relief_depth_m' not in marks['marks'][0] and 'expires_tick' not in marks['marks'][1]
            assert marks['marks'][1]['relief_depth_m'] == binary32(MAX_RELIEF)
            raw = self.relief_archive(client, saves, session, tick, 741, 'permanent-depth-v4', 4, 3)
        with self.owner('permanent relief restore', saves=saves) as (client, _):
            fresh = self.initialize(client, self.revision, uuid4().hex, saves, None)
            session = uuid4().hex
            assert self.load(client, self.revision, fresh, session, 'permanent-depth-v4', tick, 742) == 0
            assert self.relief_archive(client, saves, session, tick, 743, 'permanent-restored', 4, 3) == raw

    def run(self):
        if self.args.runtime_root:
            return super().run()
        self.author()
        frozen = tree_hashes(self.project)
        self.verify()
        self.permanent_relief()
        assert tree_hashes(self.project) == frozen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('source', 'license', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
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
    root = Path(tempfile.mkdtemp(prefix='relief-', dir=args.output.resolve()))
    fixture = ReliefContract(args, root)
    result = dict(format='poima.surface-mark-relief-contract.v1', passed=False,
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
