# SPDX-License-Identifier: Apache-2.0
"""Independent original-source skinning/depth reference for Character Yard.

Builds/downloads nothing and never opens a native process or graphics device.
The CLI reads a completed CPU workflow and emits selected pixel-depth references.
"""
from pathlib import Path
from array import array
import math, hashlib, sys
sys.dont_write_bytecode=True
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'tests/character_library_intake.py').is_file() and (p/'CMakeLists.txt').is_file())
sys.path.insert(0,str(ROOT/'tests'))
from imported_character_contract import multiply,point,transform_matrix


W, H, FOV, NEAR, FAR = (640, 480, 45.0, 0.1, 100.0)
CAMERA_POSITION = [0.0, 3.5, 9.0]
pitch = -math.atan2(2.3, 9.0)
CAMERA_LOCAL = dict(position=CAMERA_POSITION, rotation=[math.sin(pitch / 2), 0.0, 0.0, math.cos(pitch / 2)], scale=[1.0, 1.0, 1.0])
THRESHOLDS = dict(frame_margin=4, covered_pixels=1024, deep_pixels=256, stale_depth_witnesses=32, stale_device_depth_separation=3e-05, depth_acceptance_tolerance=3e-06, deep_guard_pixels=3)

def need(value, message):
    if not value:
        raise AssertionError(message)

def identity(path):
    raw = Path(path).read_bytes()
    return dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())

def inverse(matrix):
    a, b, c = (matrix[0], matrix[4], matrix[8])
    d, e, f = (matrix[1], matrix[5], matrix[9])
    g, h, i = (matrix[2], matrix[6], matrix[10])
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    need(math.isfinite(determinant) and abs(determinant) > 1e-12, 'Nonsingular camera')
    rows = [[(e * i - f * h) / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant], [(f * g - d * i) / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant], [(d * h - e * g) / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant]]
    translation = [-sum((r[k] * matrix[12 + k] for k in range(3))) for r in rows]
    return [rows[r][c] if r < 3 else 0.0 for c in range(3) for r in range(4)] + translation + [1.0]

def original_meshes(oracle, worlds, camera_world, stale_local_pose_worlds=None):
    """Full original15-mesh world LBS and perspective, with unchanged rigid parts."""
    need(len(worlds) == 57, 'Whole original node membership')
    palette = [multiply(worlds[j], bind) for j, bind in zip(oracle.skin['joints'], oracle.binds)]
    camera_inverse = inverse(camera_world)
    result = []
    for node, definition in enumerate(oracle.nodes):
        if 'mesh' not in definition:
            continue
        mesh = oracle.meshes[definition['mesh']]
        effective_palette = palette
        if 'skin' in definition and stale_local_pose_worlds is not None:
            old_mesh_inverse = inverse(stale_local_pose_worlds[node])
            effective_palette = [multiply(worlds[node], multiply(old_mesh_inverse, multiply(stale_local_pose_worlds[j], bind))) for j, bind in zip(oracle.skin['joints'], oracle.binds)]
        vertices = []
        for index, p in enumerate(mesh['positions']):
            if 'skin' not in definition:
                world = point(worlds[node], p)
            else:
                joints, weights = (mesh['joints'][index], mesh['weights'][index])
                total = sum(weights)
                need(total > 0, 'Original positive skin weights')
                world = [0.0, 0.0, 0.0]
                for joint, weight in zip(joints, weights):
                    if weight:
                        transformed = point(effective_palette[joint], p)
                        for axis in range(3):
                            world[axis] += transformed[axis] * weight / total
            view = point(camera_inverse, world)
            distance = -view[2]
            need(NEAR < distance < FAR, 'Whole source vertex near/far framing')
            pixels = H / (2 * math.tan(math.radians(FOV) / 2) * distance)
            vertices.append((W / 2 + view[0] * pixels, H / 2 - view[1] * pixels, distance))
        result.append(dict(node=node, skinned='skin' in definition, vertices=vertices, indices=mesh['indices']))
    need(len(result) == 15 and sum((len(m['indices']) // 3 for m in result)) == 6952, 'Complete original15mesh/6952triangle population')
    return result

def edge(a, b, x, y):
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])

def raster(meshes, camera_world):
    """Double-sided source raster at pixel centers with perspective device depth.

    Original material is double-sided opaque. Nearest device depth includes all
    rigid accessories, so covered skinned surfaces cannot masquerade as visible.
    """
    depth = array('d', [1.0]) * (W * H)
    nodes = array('h', [-1]) * (W * H)
    need(camera_world[13] > 0, 'Frozen actor-relative camera remains above floor')
    tangent = math.tan(math.radians(FOV) / 2)
    for y in range(H):
        vy = (1 - 2 * (y + 0.5) / H) * tangent
        for x in range(W):
            vx = (2 * (x + 0.5) / W - 1) * tangent * W / H
            direction = [camera_world[a] * vx + camera_world[4 + a] * vy - camera_world[8 + a] for a in range(3)]
            if direction[1] >= 0:
                continue
            distance = -camera_world[13] / direction[1]
            if not NEAR < distance < FAR:
                continue
            at_x = camera_world[12] + direction[0] * distance
            at_z = camera_world[14] + direction[2] * distance
            if -12 <= at_x <= 12 and -12 <= at_z <= 12:
                pixel = y * W + x
                depth[pixel] = FAR / (FAR - NEAR) - FAR * NEAR / (FAR - NEAR) / distance
                nodes[pixel] = -2
    triangle_count = 0
    for mesh in meshes:
        vertices = mesh['vertices']
        indices = mesh['indices']
        for at in range(0, len(indices), 3):
            triangle_count += 1
            a, b, c = (vertices[indices[at + k]] for k in range(3))
            area = edge(a, b, c[0], c[1])
            if abs(area) < 1e-10:
                continue
            lo_x = max(0, math.floor(min(a[0], b[0], c[0])))
            hi_x = min(W - 1, math.ceil(max(a[0], b[0], c[0])))
            lo_y = max(0, math.floor(min(a[1], b[1], c[1])))
            hi_y = min(H - 1, math.ceil(max(a[1], b[1], c[1])))
            for y in range(lo_y, hi_y + 1):
                for x in range(lo_x, hi_x + 1):
                    bary = (edge(b, c, x + 0.5, y + 0.5) / area, edge(c, a, x + 0.5, y + 0.5) / area, edge(a, b, x + 0.5, y + 0.5) / area)
                    if min(bary) < -1e-10:
                        continue
                    reciprocal = bary[0] / a[2] + bary[1] / b[2] + bary[2] / c[2]
                    z = FAR / (FAR - NEAR) - FAR * NEAR / (FAR - NEAR) * reciprocal
                    pixel = y * W + x
                    if z < depth[pixel]:
                        depth[pixel] = z
                        nodes[pixel] = mesh['node']
    need(triangle_count == 6952, 'No skipped source triangle population')
    return (depth, nodes)

def inspect_case(oracle, name, worlds, wrapper, actor, stale_source_worlds):
    camera = multiply(actor, transform_matrix(**CAMERA_LOCAL))
    meshes = original_meshes(oracle, worlds, camera)
    positions = [v for mesh in meshes for v in mesh['vertices']]
    bounds = [min((v[a] for v in positions)) for a in range(2)] + [max((v[a] for v in positions)) for a in range(2)]
    current, nodes = raster(meshes, camera)
    stale_meshes = original_meshes(oracle, worlds, camera, stale_local_pose_worlds=stale_source_worlds)
    stale, stale_nodes = raster(stale_meshes, camera)
    covered = [i for i, n in enumerate(nodes) if n >= 0]
    skinned = {i for i, d in enumerate(oracle.nodes) if 'skin' in d}
    deep = []
    witnesses = []
    for pixel in covered:
        x, y = (pixel % W, pixel // W)
        if x < 3 or x >= W - 3 or y < 3 or (y >= H - 3):
            continue
        if not all((nodes[(y + dy) * W + x + dx] >= 0 for dy in range(-3, 4) for dx in range(-3, 4))):
            continue
        deep.append(pixel)
        if nodes[pixel] not in skinned:
            continue
        delta = abs(current[pixel] - stale[pixel])
        if delta >= THRESHOLDS['stale_device_depth_separation']:
            witnesses.append((delta, pixel))

    def select(population, separation, with_stale):
        groups = {}
        for score, pixel in sorted(population, reverse=True):
            groups.setdefault(nodes[pixel], []).append((score, pixel))
        selected = []
        indices = {n: 0 for n in groups}
        while len(selected) < 32:
            changed = False
            for node in sorted(groups):
                while indices[node] < len(groups[node]):
                    score, pixel = groups[node][indices[node]]
                    indices[node] += 1
                    x, y = (pixel % W, pixel // W)
                    if any((abs(x - r['x']) + abs(y - r['y']) < separation for r in selected)):
                        continue
                    record = dict(x=x, y=y, node=node, depth=current[pixel])
                    if with_stale:
                        record.update(stale_depth=stale[pixel], depth_delta=score, stale_node=stale_nodes[pixel])
                    selected.append(record)
                    changed = True
                    break
                if len(selected) == 32:
                    break
            if not changed:
                break
        return selected
    selected = select(witnesses, 4, True)
    current_probes = select([(0, pixel) for pixel in deep], 8, False)
    margin = THRESHOLDS['frame_margin']
    frame = bounds[0] > margin and bounds[1] > margin and (bounds[2] < W - margin) and (bounds[3] < H - margin)
    return dict(name=name, original_meshes=15, original_triangles=6952, original_vertices=len(positions), original_material_unchanged=True, camera_world=camera, stale_control='Frozen original played-six mesh-local palettes with CURRENT drawable/model transforms; current rigid accessories and full-scene nearest-depth occlusion.', projected_bounds=bounds, whole_source_frame_passed=frame, covered_pixels=len(covered), deep_pixels=len(deep), original_skinned_visible_pixels=sum((n in skinned for n in nodes)), current_original_depth_probes=current_probes, current_original_depth_probes_by_node={str(n): sum((p['node'] == n for p in current_probes)) for n in sorted({p['node'] for p in current_probes})}, stale_depth_candidate_pixels=len(witnesses), stale_selected_probes=selected, stale_selected_probes_by_node={str(n): sum((p['node'] == n for p in selected)) for n in sorted({p['node'] for p in selected})}, framing_passed=frame and len(covered) >= THRESHOLDS['covered_pixels'] and (len(deep) >= THRESHOLDS['deep_pixels']), stale_control_feasible=len(selected) >= THRESHOLDS['stale_depth_witnesses'])


def lookup_path(value):
    """Locate an original recorded path; never rewrite its recorded contents."""
    import os
    from pathlib import PureWindowsPath
    if os.name != 'nt':
        recorded = PureWindowsPath(value)
        if len(recorded.drive) == 2 and recorded.drive[1] == ':':
            need('..' not in recorded.parts, 'Recorded path cannot traverse parents')
            return Path('/mnt')/recorded.drive[0].lower()/Path(*recorded.parts[1:])
    return Path(value)


def make_reference(source, cpu_report):
    import json
    from managed_physical_gameplay_contract import strict_json
    from character_library_intake import LibraryOracle
    from physical_character_transitions import SourceSchedule
    import world_physical_contract as physical
    manifest = strict_json((ROOT/'examples/physical-character-yard/source-manifest.json').read_bytes())
    need(identity(source) == {k:manifest['model'][k] for k in ('bytes','sha256')}, 'Unchanged original model identity')
    need(cpu_report.stat().st_size <= 32*1024*1024, 'Bounded completed CPU report')
    cpu = strict_json(cpu_report.read_bytes())
    need(cpu['format'] == 'poima.physical-character-controller-handoff.v1' and cpu['passed'] is True and cpu['errors'] == [], 'Completed original-character CPU workflow required')
    need(len(cpu['owners']) == 3 and all(o['natural_exit'] is True and o['forced_cleanup'] is False and o['exit_code'] == 0 and o['reader_failure'] is None for o in cpu['owners']), 'Native writers must close before references are read')
    original = LibraryOracle(source.read_bytes())
    need(original.document['materials'][0].get('doubleSided') is True and original.document['materials'][0].get('alphaMode','OPAQUE') == 'OPAQUE', 'Original opaque double-sided reference material')
    schedule = SourceSchedule(original)
    late = cpu['final_tick']
    need(type(late) is int and 60 <= late <= 318, 'Bounded source-derived later endpoint')
    selections = [('idle6','idle-moving',6,False), ('run12','run-mid',12,True), ('death22','death-mid',22,True), ('takeover23','held-takeover',23,True), ('passive27','held-mid',27,True), ('late','passive-future',late,True)]
    result = dict(format='poima.character-depth-reference.v1',source_model=identity(source),cpu_report=identity(cpu_report),camera=dict(local=CAMERA_LOCAL,width=W,height=H,vertical_fov=FOV,near=NEAR,far=FAR),thresholds=THRESHOLDS,cases=[])
    for label,slot,tick,positive in selections:
        rows = [r for r in cpu['archives'] if r['slot'] == slot and r['tick'] == tick]
        need(len(rows) == 1, 'One complete original checkpoint per reference')
        archived = rows[0];path = lookup_path(archived['path'])
        need(identity(path) == dict(bytes=archived['bytes'],sha256=archived['sha256']), 'Original checkpoint bytes changed')
        full = strict_json(path.read_bytes());observation = archived['observation']
        need(full['snapshot']['payload']['tick'] == observation['tick'] == tick, 'Exact native checkpoint/observation clock')
        saved = {r['id']:r for r in full['snapshot']['payload']['entities']}
        nodes = observation['all_nodes']
        need(len(nodes) == 57 and all(r['id'] == physical.node(i) and r['tick'] == tick for i,r in enumerate(nodes)), 'Complete ordered original node membership')
        for row in nodes + [observation[k] for k in ('actor','wrapper','camera','observer')]:
            need(physical.exact(saved[row['id']]['local'],row['local_transform']), 'Whole saved/native local-transform witness')
        camera = observation['observer']
        need(physical.exact(camera['local_transform'],CAMERA_LOCAL), 'Frozen actor-relative observer')
        expected = multiply(observation['actor']['world_matrix'],transform_matrix(**CAMERA_LOCAL))
        need(max(abs(a-b) for a,b in zip(camera['world_matrix'],expected)) <= 1e-10, 'Actual observer world product')
        floor = full['document']['entities'][physical.FLOOR]['components']
        need(floor['BoxCollider']['half_extents'] == [12,.25,12] and floor['Transform'] == dict(position=[0,-.25,0],rotation=[0,0,0,1],scale=[1,1,1]), 'Actual bounded y=0 reference floor')
        geometry = inspect_case(original,label,[r['world_matrix'] for r in nodes],observation['wrapper']['world_matrix'],observation['actor']['world_matrix'],schedule.pose(6)[2])
        need(geometry['framing_passed'] and len(geometry['current_original_depth_probes']) == 32, 'Full original geometry framing/current controls')
        need(not positive or geometry['stale_control_feasible'], 'Changed pose lacks frozen stale-palette witnesses')
        probes = geometry['current_original_depth_probes'] + (geometry['stale_selected_probes'] if positive else [])
        result['cases'].append(dict(label=label,slot=slot,tick=tick,stale_positive=positive,archive=identity(path),oracle=geometry,probes=probes))
    return result


def verify_depth_samples(case, capture):
    """Compare raw runtime.capture response values to a complete reference case."""
    need(capture['capture_written'] is True and capture['hardware'] is True and capture['nvrhi_errors'] == 0 and capture['samples'] == 1 and capture['width'] == W and capture['height'] == H, 'Actual single-sample hardware capture required')
    need(capture['tick'] == case['tick'], 'Capture uses the reference committed tick')
    products = capture['render_diagnostics']['scene_products']
    need(products['available'] is True and products['depth_convention'] == 'device depth [0,1], near 0, far/clear 1', 'Raw device-depth product convention')
    probes = products['probes'];need(len(probes) == len(case['probes']), 'Complete ordered probe population')
    errors = [];stale = []
    for sample, expected in zip(probes,case['probes']):
        need(type(sample['x']) is int and type(sample['y']) is int and (sample['x'],sample['y']) == (expected['x'],expected['y']) and sample['surface_valid'] is True, 'Captured surface/probe identity')
        need(type(sample['depth']) in (int,float) and math.isfinite(sample['depth']), 'Finite raw device depth')
        error = abs(sample['depth']-expected['depth'])
        need(error <= THRESHOLDS['depth_acceptance_tolerance'], 'Captured depth differs from independent original-source reference');errors.append(error)
        if 'stale_depth' in expected:
            need(abs(expected['depth']-expected['stale_depth']) >= THRESHOLDS['stale_device_depth_separation'], 'Declared stale witness separation')
            separation = abs(sample['depth']-expected['stale_depth'])
            need(separation > THRESHOLDS['depth_acceptance_tolerance'], 'Captured depth does not reject stale mesh-local palette');stale.append(separation)
    return dict(samples=len(errors),max_depth_error=max(errors),stale_rejections=len(stale),minimum_stale_gap=min(stale) if stale else None)


def main():
    import argparse,json
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--cpu-report',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a = p.parse_args()
    need(not a.output.exists() and a.output.parent.is_dir(), 'New output file and existing parent required')
    result = make_reference(a.source,a.cpu_report)
    raw = (json.dumps(result,indent=2,allow_nan=False)+'\n').encode()
    need(len(raw) <= 4*1024*1024, 'Bounded depth-reference output')
    with a.output.open('xb') as stream:stream.write(raw)
    print(a.output)


if __name__ == '__main__':
    main()
