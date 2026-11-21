#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify scene-linear Vulkan height fog with independently integrated sightlines."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import tempfile
import traceback

from hdr_composition_capture import display
from input_actions_gameplay_contract import exact
from scene_capture import pixels
from screen_reflections_capture import dot, entry, unit
from world_particles_contract import ParticleContract, component, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, success, tree_hashes, uid


CORE_CAMERAS = {uid(8400): (0, 2.6, 8), uid(8401): (24, .6, 8)}
SPOT_CAMERA, SPOTLIGHT, OCCLUDER = map(uid, (8402, 8403, 8404))
CAMERAS = CORE_CAMERAS | {SPOT_CAMERA: (52, .6, 8)}
LENS = dict(vertical_fov=65, near=.25, far=55)
SKY = [.0625, .125, .25]
FOG_PROFILE = dict(density_per_m=.22, base_height_m=1.5, height_falloff_per_m=.72,
                start_distance_m=1.8, max_distance_m=11.5, ambient_radiance=[.32, .15, .46], anisotropy=.20)
EMISSION = (.72, .44, .24)
FOG_HALF_TOLERANCE = .0018
SPOT_ORIGIN, SPOT_TARGET = (50.64, 2.9, 7.1), (51.4, .15, 2.8)
SPOT_DIRECTION = unit(tuple(b-a for a, b in zip(SPOT_ORIGIN, SPOT_TARGET, strict=True)))
SPOT_RANGE, SPOT_INNER, SPOT_OUTER = 8.4, 17, 30
SPOT_INTENSITY, SPOT_GAIN = 180, .7
SPOT_COLOR = (1, .35, .10)


@dataclass(frozen=True)
class Board:
    entity: str
    center: tuple[float, float, float]
    size: tuple[float, float, float]


# Each board is centered on a chosen pixel-center ray at the commanded camera
# depth. None places a sky witness, with no geometry along that entire ray.
SITES = dict(zip(CORE_CAMERAS, (
    {
        'before_start': ((205, 140), 1.15),
        'above_near_level': ((276, 181), 5.4),
        'ascending_above': ((434, 88), 6.8),
        'descending_crossing': ((100, 290), 7.5),
        'off_axis_within_range': ((555, 240), 7.8),
        'opaque_past_maximum': ((320, 270), 16),
        'ascending_sky': ((514, 54), None),
    }, {
        'before_start': ((350, 182), 1.2),
        'constant_below': ((200, 178), 6.5),
        'ascending_crossing': ((125, 68), 8.5),
        'ascending_sky': ((480, 38), None),
        'descending_sky': ((521, 310), None),
    },
), strict=True)) | {SPOT_CAMERA: {
    'occluded_beam': ((270, 213), None),
    'clear_beam': ((350, 213), None),
    'outside_beam': ((550, 120), None),
}}


def view_ray(pixel, jitter=(0, 0)):
    width, height = CAPTURE_SIZE
    x, y = (p+.5-offset for p, offset in zip(pixel, jitter, strict=True))
    focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    return (x-width/2)/focal, (height/2-y)/focal, -1


def board_from_pixel(entity, eye, pixel, depth):
    travel = view_ray(pixel)
    thickness = .18
    point = tuple(start+d*depth for start, d in zip(eye, travel, strict=True))
    return Board(entity, (point[0], point[1], point[2]-thickness/2), (.225*depth, .165*depth, thickness))


def spot_blocker():
    guide = unit(view_ray(SITES[SPOT_CAMERA]['occluded_beam'][0]))
    point = tuple(start+direction*5.1 for start, direction in zip(CAMERAS[SPOT_CAMERA], guide, strict=True))
    center = tuple(.62*lamp+.38*sample for lamp, sample in zip(SPOT_ORIGIN, point, strict=True))
    return Board(OCCLUDER, center, (.57, .72, .18))


def boards_for_world():
    boards = {}
    for camera, sites in SITES.items():
        for name, (pixel, depth) in sites.items():
            if depth is not None:
                entity = uid(8410+len(boards))
                boards[(camera, name)] = board_from_pixel(entity, CAMERAS[camera], pixel, depth)
    return boards


def optical_depth(profile, eye_y, direction_y, distance):
    density = profile['density_per_m']
    base = profile['base_height_m']
    falloff = profile['height_falloff_per_m']
    start, end = profile['start_distance_m'], min(distance, profile['max_distance_m'])
    if end <= start:
        return 0
    boundaries = [start, end]
    if direction_y != 0:
        crossing = (base-eye_y)/direction_y
        if start < crossing < end:
            boundaries.insert(1, crossing)
    tau = 0
    for a, b in zip(boundaries, boundaries[1:]):
        length = b-a
        if eye_y+direction_y*(a+b)/2 <= base:
            tau += density*length
        else:
            offset = max(0, eye_y+direction_y*a-base)
            k = falloff*direction_y
            integral = length if k == 0 else -math.expm1(-k*length)/k
            tau += density*math.exp(-falloff*offset)*integral
    return tau


def oracle(boards, camera, name, jitter=(0, 0)):
    origin = CAMERAS[camera]
    pixel, wanted_depth = SITES[camera][name]
    view = view_ray(pixel, jitter)
    ray = tuple(value/math.hypot(*view) for value in view)
    hits = [hit for board in boards.values() if (hit := entry(board, origin, ray)) is not None]
    closest = min(hits, default=None, key=lambda hit: hit['distance'])
    assert (closest['surface'] if closest else None) == boards.get((camera, name)), (name, closest)
    surface_distance = closest['distance'] if closest else None
    limit = FOG_PROFILE['max_distance_m']
    tau = optical_depth(FOG_PROFILE, origin[1], ray[1], surface_distance if closest else limit)
    result = dict(pixel=pixel, ray=ray, surface=closest['surface'].entity if closest else None,
                  camera_depth_m=None, surface_distance_m=surface_distance, tau=tau, transmittance=math.exp(-tau))
    if closest:
        assert closest['normal'] == (0, 0, 1)
        assert abs(wanted_depth+ray[2]*surface_distance) < 1e-9, closest
        assert all(abs(closest['point'][i]-closest['surface'].center[i]) < closest['surface'].size[i]/6
                   for i in (0, 1)), (name, closest)
        result.update(camera_depth_m=wanted_depth, point=closest['point'])
    return result


def validate_reference(boards, include_spot=False):
    expected = {}
    for camera, sites in SITES.items():
        if camera == SPOT_CAMERA and not include_spot:
            continue
        expected[camera] = {}
        for name in sites:
            case = expected[camera][name] = oracle(boards, camera, name)
            for dx, dy in ((-.5, -.5), (-.5, .5), (.5, -.5), (.5, .5)):
                oracle(boards, camera, name, (dx, dy))
            # A composite independent midpoint quadrature checks the analytic
            # expression and split against the authored density definition.
            density, base, falloff, start, maximum = (FOG_PROFILE[key] for key in (
                'density_per_m', 'base_height_m', 'height_falloff_per_m', 'start_distance_m', 'max_distance_m'))
            distance = case['surface_distance_m']
            end = min(maximum, distance) if distance is not None else maximum
            steps, eye_y, slope = 4096, CAMERAS[camera][1], case['ray'][1]
            stride = max(0, end-start)/steps
            numeric = density*stride*sum(math.exp(-falloff*max(0, eye_y+(start+(i+.5)*stride)*slope-base))
                                         for i in range(steps))
            assert abs(numeric-case['tau']) < 1e-7, (name, numeric, case)
            assert name != 'before_start' or case['tau'] == 0, case
    high, low = (expected[camera] for camera in CORE_CAMERAS)
    assert .02 < high['ascending_above']['tau'] < .35
    assert high['ascending_above']['tau'] < high['above_near_level']['tau'] < high['descending_crossing']['tau']
    assert 1.4 < high['descending_crossing']['tau'] < 1.8
    assert high['opaque_past_maximum']['surface_distance_m'] > FOG_PROFILE['max_distance_m']+4
    assert high['off_axis_within_range']['camera_depth_m']+2 < high['off_axis_within_range']['surface_distance_m'] < FOG_PROFILE['max_distance_m']
    assert low['ascending_crossing']['tau'] < low['constant_below']['tau'] < low['descending_sky']['tau']
    if include_spot:
        for name, case in expected[SPOT_CAMERA].items():
            full = spot_integral(case['ray'], shadow=False)
            blocked = spot_integral(case['ray'], shadow=True)
            case.update(unshadowed_red_hdr=full, shadowed_red_hdr=blocked)
            if name == 'occluded_beam':
                assert full > .12 and .06 < full-blocked < .11, (full, blocked)
            elif name == 'clear_beam':
                assert full > .06 and full == blocked, (full, blocked)
            else:
                assert full == blocked == 0, (full, blocked)
    return expected


def spot_integral(ray, *, shadow):
    """Integrate twelve bins with independent Euclidean ray/box visibility."""
    eye = CAMERAS[SPOT_CAMERA]
    to_spot = tuple(lamp-origin for origin, lamp in zip(eye, SPOT_ORIGIN, strict=True))
    along = dot(to_spot, ray)
    discriminant = along**2 + SPOT_RANGE**2 - dot(to_spot, to_spot)
    if discriminant <= 0:
        return 0
    radius = math.sqrt(discriminant)
    lower = max(FOG_PROFILE['start_distance_m'], along-radius)
    upper = min(FOG_PROFILE['max_distance_m'], along+radius)
    if upper <= lower:
        return 0
    stride = (upper-lower)/12
    cosine_inner = math.cos(math.radians(SPOT_INNER))
    cosine_outer = math.cos(math.radians(SPOT_OUTER))
    blocker = spot_blocker()
    g = FOG_PROFILE['anisotropy']

    def transmittance(distance):
        return math.exp(-optical_depth(FOG_PROFILE, eye[1], ray[1], distance))

    total = 0
    for i in range(12):
        mid = lower+(i+.5)*stride
        lamp_ray = tuple(d*mid-v for d, v in zip(ray, to_spot, strict=True))
        distance = math.hypot(*lamp_ray)
        direction = unit(lamp_ray)
        fade = (dot(direction, SPOT_DIRECTION)-cosine_outer)/(cosine_inner-cosine_outer)
        if fade <= 0:
            continue
        if shadow:
            hit = entry(blocker, SPOT_ORIGIN, direction)
            if hit is not None and hit['distance'] < distance:
                continue
        weight = transmittance(lower+i*stride)-transmittance(lower+(i+1)*stride)
        phase = (1-g*g)/(4*math.pi*(1+g*g+2*g*dot(direction, ray))**1.5)
        attenuation = min(1, fade)**2*(1-(distance/SPOT_RANGE)**4)/distance**2
        total += weight*phase*attenuation
    return total*SPOT_INTENSITY*SPOT_GAIN


def environment(profile=None):
    value = dict(ambient=[0, 0, 0], exposure=1, shadow_resolution=2048, sky=dict(enabled=True, zenith=SKY, horizon=SKY,
        ground=SKY, horizon_falloff=.35, sun=None, sun_size_degrees=1, sun_intensity=0))
    return value if profile is None else value | dict(height_fog=profile)


def mesh(cast_shadows=False):
    return dict(primitive='box', albedo=[0]*3, visible=True, cast_shadows=cast_shadows, receive_shadows=False)


def author_board(board, label):
    return [dict(op='entity.create', id=board.entity, name=label),
            component(board.entity, 'Transform', transform(list(board.center)) | dict(scale=list(board.size))),
            component(board.entity, 'MeshRenderer', mesh()),
            component(board.entity, 'PbrMaterial', dict(base_color=[0]*3, roughness=.85, metallic=1,
                emissive=list(EMISSION), double_sided=False)),
            component(board.entity, 'BoxCollider', dict(motion='static', half_extents=[.5]*3,
                mass=1, friction=.5, restitution=0))]


class HeightFogCapture(ParticleContract):
    def transact(self, revision, request, ops):
        result = self.world_call('world.transact', request_id=uid(request), base_revision=revision, ops=ops)
        assert result['committed'] and result['revision'] == revision+1, result
        return result['revision']

    def author(self):
        result = self.cli('project', 'create', self.native(self.project), '--name', 'Height Fog Sightlines')
        success(json.loads(result))
        ops = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
               dict(op='component.remove', id=uid(1), type='BoxCollider'),
               dict(op='component.remove', id=uid(5), type='Light'),
               component(uid(4), 'LightingEnvironment', environment())]
        for camera, location in (CAMERAS if self.args.spot else CORE_CAMERAS).items():
            ops.extend([dict(op='entity.create', id=camera, name=f'Height fog witness camera {camera[-1]}'),
                        component(camera, 'Transform', transform(list(location))), component(camera, 'Camera', LENS)])
        for (_, name), board in self.boards.items():
            ops.extend(author_board(board, name.replace('_', ' ').capitalize()))
        if self.args.spot:
            x, y, z = SPOT_DIRECTION
            aim = unit((y, -x, 0, 1-z))
            ops.extend([dict(op='entity.create', id=SPOTLIGHT, name='Fixed height fog spotlight'),
                        component(SPOTLIGHT, 'Transform', transform(list(SPOT_ORIGIN), list(aim))),
                        component(SPOTLIGHT, 'Light', dict(kind='spot', enabled=True, intensity=SPOT_INTENSITY,
                            color=list(SPOT_COLOR), range=SPOT_RANGE, inner_angle=SPOT_INNER, outer_angle=SPOT_OUTER,
                            shadow=dict(enabled=True, near=.05, distance=SPOT_RANGE, bias=.00015, normal_bias=.001)))])
            ops.extend(author_board(spot_blocker(), 'Height fog witness occluder'))
        return self.transact(self.world_call('world.inspect')['revision'], 8490, ops)

    def relocated_bundle(self, name, revision, profile):
        required = profile is not None
        owner = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert owner['revision'] == revision and owner.get('needs_height_fog', False) is required, owner
        assert ('needs_height_fog' in owner) is required, owner
        exported, relocated = self.root/(name+' original export'), self.root/(name+' relocated')
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(exported),
                                   '--runtime', self.native(self.args.runtime_root))))
        exported.rename(relocated)
        assert not exported.exists()
        game = relocated/'game.json'
        executable = relocated/json.loads(game.read_bytes())['engine']['executable']
        inspected = success(json.loads(self.cli('game', 'inspect', self.native(game), executable=executable)))
        assert inspected.get('needs_height_fog', False) is required, inspected
        assert ('needs_height_fog' in inspected) is required, inspected
        self.measurements['bundles'][name] = dict(revision=revision, profile=profile, inventory=tree_hashes(relocated))
        return game

    def capture(self, client, session, camera, family, *, samples=1, view='color', reconstruction='none', fallback=None):
        width, height = CAPTURE_SIZE
        name = f'{family}-{camera[-1]}-{samples}x-{view}-{reconstruction}'
        path = self.root/(name+'.bmp')
        coords = {label: item[0] for label, item in SITES[camera].items()}
        report = client.call('runtime.capture', dict(session_id=session, tick=0, structure_revision=0,
            camera=camera, path=self.native(path), width=width, height=height, samples=samples, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction=reconstruction, capture_frames=2,
            scene_debug_view=view, profile=True,
            scene_product_probes=[dict(x=x, y=y) for x, y in coords.values()] if samples == 1 else []))
        self.measurements['reports'][name] = report
        assert report['hardware'] is True and report['capture_written'] is True, report
        assert report['frames_presented'] == 2 and report['nvrhi_errors'] == 0, report
        assert report['lens'] == LENS and report['tick'] == 0 and report['revision'] == self.measurements['bundles'][family]['revision'], report
        assert (report['width'], report['height'], report['samples']) == (width, height, samples), report
        eye = CAMERAS[camera]
        near_vector(report['camera_world'], [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *eye, 1])
        diagnostics = report['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        scatter = family in ('spot-open', 'spot-shadow')
        if family == 'omitted':
            assert 'height_fog' not in diagnostics, diagnostics
            active = False
        else:
            reason = 'zero_density' if family == 'zero' else fallback or 'active'
            fog = diagnostics['height_fog']
            active = reason == 'active'
            assert fog['active'] is active and fog['reason'] == reason, fog
            assert fog['buffer_bytes'] == (width*height*8 if active else 0), fog
            assert fog['spot_lights'] == int(active and scatter), fog
        if camera == SPOT_CAMERA:
            draws = diagnostics['last_draws']
            assert draws['shadow_views'] == 1 and draws['shadow_draws'] == int(family == 'spot-shadow'), draws
        result = dict(gpu=report['gpu'], camera=camera, sha256=file_hash(path), probes={})
        self.measurements['captures'][name] = result
        image = pixels(path)
        assert len(image) == height and all(len(row) == width for row in image)
        if samples > 1:
            return result
        products = diagnostics['scene_products']
        assert products['available'] and products['view'] == view, products
        temporal = reconstruction != 'none'
        settings = diagnostics['reconstruction']
        assert settings['active'] is temporal, settings
        jitter = settings['jitter_pixels'] if temporal else (0, 0)
        if temporal:
            assert len(jitter) == 2 and all(abs(d) <= .5 for d in jitter), settings
            assert settings['history_sequence'] == 2 and settings['history_reset'] is False, settings
            assert (settings['render_width'], settings['render_height']) == CAPTURE_SIZE, settings
        for (label, (x, y)), got in zip(coords.items(), products['probes'], strict=True):
            expected = oracle(self.geometry, camera, label, jitter)
            case = result['probes'][label] = dict(oracle=expected, sample=got, rgb=image[y][x])
            assert (got['x'], got['y'], got['resolved_x'], got['resolved_y']) == (x, y, x, y), case
            assert got['surface_valid'] is bool(expected['surface']), case
            if expected['surface']:
                n, f = LENS['near'], LENS['far']
                depth = n/(1-got['depth']*(f-n)/f)
                assert abs(expected['camera_depth_m']-depth) < .0012, case
                near_vector(got['shading_normal'][:3], (0, 0, 1), tolerance=.002)
                ray_hit = client.call('runtime.raycast', dict(session_id=session, tick=0, origin=list(eye),
                    direction=list(expected['ray']), distance=LENS['far']))['hit']
                assert ray_hit is not None and ray_hit['entity'] == expected['surface'], (case, ray_hit)
                near_vector(ray_hit['position'], expected['point'], tolerance=.0001)
            else:
                assert got['depth'] == 1, case
            if view == 'color':
                raw, resolved = got['raw_hdr'], got['resolved_hdr']
                near_vector(raw, EMISSION if expected['surface'] else SKY, tolerance=.001)
                if active:
                    t = expected['transmittance']
                    beam = spot_integral(expected['ray'], shadow=family == 'spot-shadow') if scatter else 0
                    analytical = [prior*t+ambient*(1-t)+color*beam for prior, ambient, color
                                  in zip(raw, FOG_PROFILE['ambient_radiance'], SPOT_COLOR, strict=True)]
                    case['expected_resolved_hdr'] = analytical
                    if camera == SPOT_CAMERA:
                        case['expected_beam_red_hdr'] = beam
                    near_vector(resolved, analytical, tolerance=FOG_HALF_TOLERANCE)
                    if expected['tau'] == 0:
                        assert exact(raw, resolved), case
                elif not temporal:
                    assert exact(raw, resolved), case
                if not temporal:
                    answer = [display(linear) for linear in resolved]
                    assert max(abs(a-b) for a, b in zip(case['rgb'], answer, strict=True)) <= 2, case
        return result

    def exercise(self, family, game):
        root = self.root/(family+' saves')
        root.mkdir()
        selected = {}
        with self.owner(family+' relocated fog', game=game, saves=root) as (client, _):
            revision = self.measurements['bundles'][family]['revision']
            session = self.initialize(client, revision, None, root, game)
            if family.startswith('spot-'):
                return self.capture(client, session, SPOT_CAMERA, family)
            for camera in CORE_CAMERAS:
                selected[camera] = self.capture(client, session, camera, family)
            if family != 'zero':
                camera = next(iter(CORE_CAMERAS))
                selected['depth'] = self.capture(client, session, camera, family, view='depth', fallback='diagnostic')
                if self.args.fsr:
                    selected['fsr'] = self.capture(client, session, camera, family, reconstruction='fsr3_native',
                                                   fallback='reconstruction')
                if self.args.msaa:
                    selected['msaa'] = self.capture(client, session, camera, family, samples=4, fallback='multisample')
        return selected

    def compare(self, completed):
        omitted, zero, fog = (completed[key] for key in ('omitted', 'zero', 'enabled'))
        record = self.measurements['comparisons'] = dict(exact_fallbacks=[], contrasts={})
        for key, baseline in omitted.items():
            if key not in CAMERAS:
                assert exact(baseline, fog[key]), (key, baseline, fog[key])
                record['exact_fallbacks'].append(key)
                continue
            active = fog[key]
            assert exact(baseline, zero[key]), (key, baseline, zero[key])
            assert baseline['sha256'] != active['sha256'] and baseline['gpu'] == active['gpu'], key
            for name, untouched in baseline['probes'].items():
                composed = active['probes'][name]
                assert exact(untouched['oracle'], composed['oracle']), (name, untouched, composed)
                before, after = (item['sample'] for item in (untouched, composed))
                assert exact({k: v for k, v in before.items() if k != 'resolved_hdr'},
                             {k: v for k, v in after.items() if k != 'resolved_hdr'}), (name, before, after)
                if name == 'before_start':
                    assert exact(untouched['rgb'], composed['rgb']), (name, untouched, composed)
                else:
                    hdr_delta = math.dist(before['raw_hdr'], after['resolved_hdr'])
                    assert hdr_delta > .035, (name, before, after)
                    record['contrasts'][f'{key}/{name}'] = dict(hdr_delta=hdr_delta, tau=composed['oracle']['tau'])
        record['omitted_vs_zero_unchanged'] = True
        if self.args.spot:
            self.compare_spot(completed)

    def compare_spot(self, completed):
        zero, lit, shadow = (completed['spot-'+key] for key in ('zero', 'open', 'shadow'))
        assert len({frame['sha256'] for frame in (zero, lit, shadow)}) == 3
        observations = self.measurements['comparisons']['spot'] = {}
        for site in SITES[SPOT_CAMERA]:
            groups = [frame['probes'][site] for frame in (zero, lit, shadow)]
            base, active, blocked = (case['sample']['resolved_hdr'] for case in groups)
            raw = [{k: v for k, v in case['sample'].items() if k != 'resolved_hdr'} for case in groups]
            assert exact(raw[0], raw[1]) and exact(raw[0], raw[2]), (site, raw)
            delta = [after-before for before, after in zip(base, active, strict=True)]
            observations[site] = dict(added_hdr=delta, lost_to_blocker_red=active[0]-blocked[0])
            if site == 'outside_beam':
                assert exact(base, active) and exact(base, blocked), groups
            else:
                assert delta[0] > .06, groups
                near_vector(delta, [delta[0]*c for c in SPOT_COLOR], tolerance=.0018)
                if site == 'occluded_beam':
                    assert active[0]-blocked[0] > .06 and blocked[0]-base[0] > .045, groups
                else:
                    assert exact(active, blocked), groups

    def run(self):
        self.boards = boards_for_world()
        self.geometry = self.boards | ({(SPOT_CAMERA, 'blocker'): spot_blocker()} if self.args.spot else {})
        self.measurements.update(reference=validate_reference(self.geometry, self.args.spot), bundles={}, captures={}, reports={})
        revision = self.author()
        families = [('omitted', None), ('zero', FOG_PROFILE | dict(density_per_m=0)), ('enabled', FOG_PROFILE)]
        if self.args.spot:
            families.extend(('spot-'+mode, FOG_PROFILE | dict(spot_lights=[dict(entity=SPOTLIGHT,
                scattering=0 if mode == 'zero' else SPOT_GAIN)])) for mode in ('zero', 'open', 'shadow'))
        games = {}
        for i, (name, profile) in enumerate(families):
            if profile is not None:
                ops = [component(uid(4), 'LightingEnvironment', environment(profile))]
                if name == 'spot-shadow':
                    ops.append(component(OCCLUDER, 'MeshRenderer', mesh(cast_shadows=True)))
                revision = self.transact(revision, 8491+i, ops)
            games[name] = self.relocated_bundle(name, revision, profile)
        detached = self.root/'Detached Height Fog Authoring'
        self.project.rename(detached)
        source = tree_hashes(detached)
        assert not self.project.exists()
        completed = {}
        for family, game in games.items():
            completed[family] = self.exercise(family, game)
            assert not self.project.exists() and tree_hashes(detached) == source
            assert tree_hashes(game.parent) == self.measurements['bundles'][family]['inventory']
        self.measurements['detached_source'] = source
        for call in self.calls:
            stderr = call.get('wire', {}).get('stderr.log', call.get('stderr', ''))
            assert all(marker not in stderr for marker in ('VUID-', 'Validation Error', 'Validation Warning',
                       'SYNC-HAZARD', 'Undefined-Value')), stderr
        self.compare(completed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--fsr', action='store_true', help='Compare FSR Native AA fallback with omitted fog.')
    parser.add_argument('--msaa', action='store_true', help='Compare 4x MSAA fallback with omitted fog.')
    parser.add_argument('--spot', action='store_true', help='Qualify an isolated spot beam and controlled occluder.')
    args = parser.parse_args()
    if not __debug__ or not 0 <= args.gpu <= 4095:
        parser.error('Run with assertions enabled and a GPU index between 0 and 4095.')
    if args.msaa and args.lighting_path != 'forward':
        parser.error('--msaa requires --lighting-path forward.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='height-fog-', dir=args.output.resolve()))
    test = HeightFogCapture(args, root)
    record = dict(format='poima.height-fog-capture.v1', passed=False, gpu=args.gpu, lighting_path=args.lighting_path,
                  fsr=args.fsr, msaa=args.msaa, spot=args.spot, binary_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
        record['passed'] = True
    except BaseException:
        record['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
