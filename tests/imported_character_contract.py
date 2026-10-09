#!/usr/bin/env python3
"""Network-free original-character import, native playback and optional Vulkan checks.

Supply the unmodified RiggedFigure.glb from Khronos glTF Sample Assets. The
original model is copyright 2017 Cesium, licensed CC-BY-4.0:
https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/RiggedFigure
https://creativecommons.org/licenses/by/4.0/

This Apache-2.0 test contains algorithms, not embedded character artwork. Its
optional reference meshes are derivatives of that separately supplied licensed
source, with attribution recorded in their GLB metadata and the evidence file.
No network access, conversion application, C# build or engine build is performed.
The one clip opens a stationary figure's arms; it is not a locomotion benchmark.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import bisect
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError
from scene_capture import pixels

SOURCE_SHA = 'd6be85417d3e256861ee733eea6916093a7af7c79c16366181fd8abcaeb38cf5'
SOURCE_URL = ('https://github.com/KhronosGroup/glTF-Sample-Assets/blob/'
              'edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/RiggedFigure/glTF-Binary/RiggedFigure.glb')
TIMES = (0, .3125, .625, .9375)
MATRIX_LIMIT, VERTEX_LIMIT, NORMAL_LIMIT = 2e-6, 2e-5, 3e-5
WIDTH, HEIGHT = 640, 480


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unit(values):
    length = math.sqrt(sum(value * value for value in values))
    need(math.isfinite(length) and length > 1e-12, 'Invalid source direction/quaternion')
    return [value / length for value in values]


def multiply(a, b):
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
            for c in range(4) for r in range(4)]


def point(matrix, value):
    return [sum(matrix[k * 4 + r] * value[k] for k in range(3)) + matrix[12 + r]
            for r in range(3)]


def transform_matrix(position, rotation, scale):
    x, y, z, w = unit(rotation)
    return [(1 - 2*y*y - 2*z*z)*scale[0], (2*x*y + 2*z*w)*scale[0], (2*x*z - 2*y*w)*scale[0], 0,
            (2*x*y - 2*z*w)*scale[1], (1 - 2*x*x - 2*z*z)*scale[1], (2*y*z + 2*x*w)*scale[1], 0,
            (2*x*z + 2*y*w)*scale[2], (2*y*z - 2*x*w)*scale[2], (1 - 2*x*x - 2*y*y)*scale[2], 0,
            *position, 1]


def normal_direction(matrix, normal):
    # Cofactor matrix / determinant is inverse transpose of the linear map.
    a, b, c = matrix[0], matrix[4], matrix[8]
    d, e, f = matrix[1], matrix[5], matrix[9]
    g, h, i = matrix[2], matrix[6], matrix[10]
    cofactors = [[e*i-f*h, f*g-d*i, d*h-e*g],
                 [c*h-b*i, a*i-c*g, b*g-a*h],
                 [b*f-c*e, c*d-a*f, a*e-b*d]]
    determinant = a*cofactors[0][0] + b*cofactors[0][1] + c*cofactors[0][2]
    need(abs(determinant) > 1e-12, 'Singular source skin matrix')
    return unit([sum(row[k]*normal[k] for k in range(3))/determinant for row in cofactors])


def interpolate_rotation(a, b, fraction):
    a, b = unit(a), unit(b)
    dot = sum(x*y for x, y in zip(a, b))
    if dot < 0:
        b, dot = [-value for value in b], -dot
    if dot > .9995:
        return unit([x + (y-x)*fraction for x, y in zip(a, b)])
    angle = math.acos(max(-1, min(1, dot)))
    return unit([x*math.sin((1-fraction)*angle)/math.sin(angle) +
                 y*math.sin(fraction*angle)/math.sin(angle) for x, y in zip(a, b)])


class SourceOracle:
    """Read raw source arrays and evaluate them without an engine pose API."""
    def __init__(self, data):
        need(hashlib.sha256(data).hexdigest() == SOURCE_SHA, 'Expected pinned unmodified original RiggedFigure.glb')
        magic, version, total = struct.unpack_from('<III', data)
        need((magic, version, total) == (0x46546c67, 2, len(data)), 'Invalid original GLB header')
        length, kind = struct.unpack_from('<II', data, 12)
        need(kind == 0x4e4f534a, 'Missing source JSON chunk')
        self.document = json.loads(data[20:20+length])
        size, kind = struct.unpack_from('<II', data, 20+length)
        need(kind == 0x004e4942 and 28+length+size == len(data), 'Missing source binary chunk')
        self.blob = data[28+length:]
        self.nodes = self.document['nodes']
        self.parents = {child: parent for parent, node in enumerate(self.nodes) for child in node.get('children', [])}
        self.clip = self.document['animations'][0]
        self.channels = []
        for channel in self.clip['channels']:
            sampler = self.clip['samplers'][channel['sampler']]
            need(sampler.get('interpolation', 'LINEAR') == 'LINEAR', 'Original interpolation profile changed')
            self.channels.append(dict(node=channel['target']['node'], path=channel['target']['path'],
                times=[row[0] for row in self.accessor(sampler['input'])], values=self.accessor(sampler['output'])))
        self.duration = max(channel['times'][-1] for channel in self.channels)
        self.mesh_node = next(i for i, node in enumerate(self.nodes) if 'mesh' in node)
        self.primitive = self.document['meshes'][self.nodes[self.mesh_node]['mesh']]['primitives'][0]
        attributes = self.primitive['attributes']
        self.positions = self.accessor(attributes['POSITION'])
        self.normals = [unit(value) for value in self.accessor(attributes['NORMAL'])]
        self.joints = self.accessor(attributes['JOINTS_0'])
        self.weights = self.accessor(attributes['WEIGHTS_0'])
        self.indices = [row[0] for row in self.accessor(self.primitive['indices'])]
        self.skin = self.document['skins'][self.nodes[self.mesh_node]['skin']]
        self.binds = self.accessor(self.skin['inverseBindMatrices'])
        need((len(self.nodes), len(self.skin['joints']), len(self.positions), len(self.indices)//3,
              len(self.channels), self.duration) == (22, 19, 370, 256, 57, 1.25), 'Original source profile changed')
        need(max(abs(sum(row)-1) for row in self.weights) < 1e-6, 'Source weights are not normalized')

    def accessor(self, index):
        accessor = self.document['accessors'][index]
        view = self.document['bufferViews'][accessor['bufferView']]
        need('sparse' not in accessor and not accessor.get('normalized', False) and view['buffer'] == 0,
             'Unsupported original accessor profile')
        count = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[accessor['type']]
        fmt = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}[accessor['componentType']]
        width = struct.calcsize('<'+fmt)*count
        stride = view.get('byteStride', width)
        start = view.get('byteOffset', 0)+accessor.get('byteOffset', 0)
        need(start+(accessor['count']-1)*stride+width <= len(self.blob), 'Source accessor exceeds binary chunk')
        return [struct.unpack_from('<'+fmt*count, self.blob, start+i*stride) for i in range(accessor['count'])]

    def evaluate(self, time=None):
        locals_ = [dict(position=list(node.get('translation', [0, 0, 0])),
                        rotation=list(node.get('rotation', [0, 0, 0, 1])),
                        scale=list(node.get('scale', [1, 1, 1]))) for node in self.nodes]
        if time is not None:
            for channel in self.channels:
                times, values = channel['times'], channel['values']
                right = min(len(times)-1, bisect.bisect_right(times, time))
                left = max(0, bisect.bisect_right(times, time)-1)
                fraction = 0 if left == right else max(0, min(1, (time-times[left])/(times[right]-times[left])))
                a, b = values[left], values[right]
                path = channel['path']
                value = interpolate_rotation(a, b, fraction) if path == 'rotation' else [x+(y-x)*fraction for x, y in zip(a, b)]
                locals_[channel['node']][{'translation': 'position', 'rotation': 'rotation', 'scale': 'scale'}[path]] = value
        local_matrices = [node.get('matrix', transform_matrix(**local)) for node, local in zip(self.nodes, locals_)]
        matrices = {}
        def world(index):
            if index not in matrices:
                parent = self.parents.get(index)
                matrices[index] = multiply(world(parent), local_matrices[index]) if parent is not None else local_matrices[index]
            return matrices[index]
        worlds = [world(i) for i in range(len(self.nodes))]
        palette = [multiply(worlds[joint], bind) for joint, bind in zip(self.skin['joints'], self.binds)]
        vertices, normals = [], []
        for position, normal, joints, weights in zip(self.positions, self.normals, self.joints, self.weights):
            blended = [sum(weights[k]*palette[int(joints[k])][entry] for k in range(4)) for entry in range(16)]
            vertices.append(point(blended, position))
            normals.append(normal_direction(blended, normal))
        return dict(time=time, locals=locals_, local_matrices=local_matrices, worlds=worlds, vertices=vertices, normals=normals,
                    bounds=dict(min=[min(v[k] for v in vertices) for k in range(3)],
                                max=[max(v[k] for v in vertices) for k in range(3)]))

    def reference_glb(self, pose):
        # Re-evaluate source skinning into a separately imported, unweighted mesh.
        rows = [(*p, *n) for p, n in zip(pose['vertices'], pose['normals'])]
        blob = struct.pack('<'+'f'*(len(rows)*6), *(value for row in rows for value in row))
        offset = len(blob)
        blob += struct.pack('<'+'I'*len(self.indices), *self.indices)
        document = dict(asset=dict(version='2.0', generator='Poima independent original-source skin oracle',
            copyright='Copyright 2017 Cesium; CC-BY-4.0; source '+SOURCE_URL,
            extras=dict(attribution='Rigged Figure by Cesium, 2017', license='https://creativecommons.org/licenses/by/4.0/',
                        source=SOURCE_URL, changes='Skinning baked into unweighted positions/normals for correctness comparison')),
            buffers=[dict(byteLength=len(blob))],
            bufferViews=[dict(buffer=0, byteOffset=0, byteLength=offset, byteStride=24),
                         dict(buffer=0, byteOffset=offset, byteLength=len(blob)-offset)],
            accessors=[dict(bufferView=0, componentType=5126, count=len(rows), type='VEC3', **pose['bounds']),
                       dict(bufferView=0, byteOffset=12, componentType=5126, count=len(rows), type='VEC3'),
                       dict(bufferView=1, componentType=5125, count=len(self.indices), type='SCALAR')],
            materials=self.document['materials'],
            meshes=[dict(primitives=[dict(attributes=dict(POSITION=0, NORMAL=1), indices=2, material=0)])],
            nodes=[dict(name='Independent licensed source pose', mesh=0)], scenes=[dict(nodes=[0])], scene=0)
        encoded = json.dumps(document, separators=(',', ':')).encode()
        encoded += b' '*((-len(encoded)) % 4)
        blob += b'\0'*((-len(blob)) % 4)
        return (struct.pack('<III', 0x46546c67, 2, 28+len(encoded)+len(blob)) +
                struct.pack('<II', len(encoded), 0x4e4f534a)+encoded + struct.pack('<II', len(blob), 0x004e4942)+blob)


def maximum_error(actual, expected):
    need(len(actual) == len(expected), 'Reference vector/matrix length differs')
    need(all(math.isfinite(value) for value in actual), 'Observed nonfinite output')
    return max(abs(a-b) for a, b in zip(actual, expected))


def node_id(rig, index):
    return hashlib.sha256(f'poima.instance.v1/{rig}/node/{index}'.encode()).hexdigest()[:32]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--runtime', type=int, choices=(0, 1), default=1)
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    if not args.source.is_file() or args.source.stat().st_size != 50116 or sha(args.source) != SOURCE_SHA:
        parser.error('--source must be the unmodified pinned 50,116-byte RiggedFigure.glb')
    run = args.output.resolve()
    try:
        run.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('--output must be new; existing files are never overwritten')
    world = run/'character.world.json'
    report = dict(passed=False, binary_sha256=sha(args.binary), test_sha256=sha(Path(__file__)),
        source_sha256=SOURCE_SHA, source_bytes=50116, source_url=SOURCE_URL, runtime_requested=bool(args.runtime),
        dependencies_sha256={str(path.relative_to(ROOT)): sha(path) for path in
            [Path(__file__).with_name('scene_capture.py'), *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]},
        python_version=sys.version,
        attribution='Rigged Figure, copyright 2017 Cesium, CC-BY-4.0; https://creativecommons.org/licenses/by/4.0/',
        oracle='Original source arrays, independent quaternion/TRS/matrix hierarchy, inverse-bind and weighted skinning',
        calls=[], owners=[], checks={}, errors={}, poses=[], captures={}, comparisons={},
        tolerances=dict(local_world_matrix=MATRIX_LIMIT, world_vertex_m=VERTEX_LIMIT, normal_component=NORMAL_LIMIT,
                        raster_pixels_over_2=128, raster_mean_max_channel_error=.1),
        limitations=['One 19-joint original arm-opening clip; no locomotion, retargeting or Kenney multi-source FBX qualification.',
                     'Native frozen-playback seeks; no C#, navigation, saves, export, physical input, editor or performance claim.',
                     'Optional bounded 640x480 Vulkan correctness; exact pixels are claimed only if observed.',
                     'References derive only from the separately provided original licensed GLB; test makes no network request.'])
    owner = None

    def native(path):
        text = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip() if args.windows_interop and os.name != 'nt' else text

    def call(method, params=None):
        row = dict(method=method, params={} if params is None else params, owner_process_id=owner.transport.process_id)
        report['calls'].append(row)
        try:
            row['result'] = owner.call(method, params, timeout=180 if method.endswith('capture') else 60)
            return row['result']
        except RpcError as error:
            row['error'] = dict(code=error.code, message=error.message, data=error.data)
            raise

    def open_owner():
        nonlocal owner
        owner = WorldClient.open(str(args.binary.resolve()), native(world), close_timeout=15)

    def close_owner():
        nonlocal owner
        previous, owner = owner, None
        if previous is not None:
            try:
                previous.close()
            finally:
                report['owners'].append(dict(process_id=previous.transport.process_id,
                    exit_code=previous.transport.returncode, closed=previous.closed, stderr=previous.transport.stderr_tail,
                    argv=[str(args.binary.resolve()), 'world', native(world)]))
            need(previous.transport.returncode == 0 and previous.closed, 'Owned native process did not exit cleanly')

    def page(method, params):
        result, offset = [], 0
        for _ in range(1024):
            current = call(method, dict(params, offset=offset, limit=64))
            result.extend(current['items'])
            next_offset = current.get('next_offset')
            if next_offset is None:
                return result
            need(isinstance(next_offset, int) and next_offset > offset, 'Native pagination did not advance')
            offset = next_offset
        raise AssertionError('Native pagination exceeded bounded traversal')

    def check_matrix(name, actual, expected):
        error = maximum_error(actual, expected)
        report['errors'][name] = max(report['errors'].get(name, 0), error)
        need(error <= MATRIX_LIMIT, f'{name} differs from independent source: {error}')

    def package_tree():
        directory = Path(str(world)+'.assets')
        return {str(path.relative_to(directory)): sha(path) for path in directory.rglob('*') if path.is_file()}

    def observe_pose(asset, expected):
        sampled = page('asset.animation.sample', dict(asset=asset, clip=0, time=expected['time'], loop=False, section='nodes'))
        need([node['index'] for node in sampled] == list(range(22)), 'Sampled node order/count differs')
        for node in sampled:
            i = node['index']
            check_matrix('sample_local', transform_matrix(node['position'], node['rotation'], node['scale']), expected['local_matrices'][i])
            check_matrix('sample_world', node['world'], expected['worlds'][i])
        vertices = page('asset.animation.sample', dict(asset=asset, clip=0, time=expected['time'], loop=False,
                        section='vertices', node=oracle.mesh_node, primitive=0))
        need([vertex['index'] for vertex in vertices] == list(range(370)), 'Sampled vertex order/count differs')
        position_error = normal_error = 0
        for index, vertex in enumerate(vertices):
            position_error = max(position_error, maximum_error(vertex['world_position'], expected['vertices'][index]))
            world_normal = normal_direction(expected['worlds'][oracle.mesh_node], vertex['normal'])
            normal_error = max(normal_error, maximum_error(world_normal, expected['normals'][index]))
        need(position_error <= VERTEX_LIMIT and normal_error <= NORMAL_LIMIT,
             f'CPU skinning differs from independent source: position={position_error}, normal={normal_error}')
        report['errors']['world_vertex_m'] = max(report['errors'].get('world_vertex_m', 0), position_error)
        report['errors']['world_normal_component'] = max(report['errors'].get('world_normal_component', 0), normal_error)
        return sampled, vertices

    def capture(name, method, params):
        target = run/(name+'.bmp')
        need(not target.exists(), 'Capture target already exists')
        before = world.read_bytes()
        request = dict(camera=camera, path=native(target), width=WIDTH, height=HEIGHT,
                       gpu=args.gpu, samples=args.samples, profile=True, **params)
        result = call(method, request)
        need(result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0,
             'Capture did not finish on Vulkan hardware')
        need(result['object_count'] == 1 and result['samples'] == args.samples, 'Unexpected draw/AA count')
        need(world.read_bytes() == before, 'Read-only capture changed authored document')
        report['captures'][name] = dict(report=result, sha256=sha(target))
        return pixels(target)

    def image_compare(name, a, b, enforce=True):
        need(len(a) == len(b) == HEIGHT and all(len(row) == WIDTH for row in a+b), 'Unexpected capture dimensions')
        differences = [max(abs(x-y) for x, y in zip(p, q)) for row, other in zip(a, b) for p, q in zip(row, other)]
        statistics = dict(changed_pixels=sum(value != 0 for value in differences),
            pixels_over_2=sum(value > 2 for value in differences), maximum_channel_error=max(differences),
            mean_max_channel_error=sum(differences)/len(differences))
        report['comparisons'][name] = statistics
        if enforce:
            need(statistics['pixels_over_2'] <= 128 and statistics['mean_max_channel_error'] <= .1,
                 'Rendered source-reference error exceeded explicit raster bounds: '+str(statistics))
        return statistics

    rig, camera = f'{100:032x}', f'{1:032x}'
    try:
        oracle = SourceOracle(args.source.read_bytes())
        expected_poses = [oracle.evaluate(time) for time in TIMES]
        rest = oracle.evaluate()
        report['poses'] = [dict(time=pose['time'], bounds=pose['bounds'], skeleton_root_position=pose['worlds'][2][12:15]) for pose in expected_poses]
        need(max(maximum_error(pose['worlds'][2][12:15], expected_poses[0]['worlds'][2][12:15]) for pose in expected_poses) == 0,
             'Expected original clip has acquired root displacement')
        movement = max(math.dist(a, b) for a, b in zip(expected_poses[0]['vertices'], expected_poses[-1]['vertices']))
        need(movement > .25, 'Independent source has no nontrivial arm motion')
        report['source_motion_max_vertex_m'] = movement
        sources = run/'owned-sources'; sources.mkdir()
        copied = sources/'RiggedFigure.glb'
        with copied.open('xb') as output:
            output.write(args.source.read_bytes())
        need(sha(copied) == SOURCE_SHA, 'Owned import copy differs from caller source')
        open_owner()
        before_import = call('world.inspect')
        imported = call('asset.import', dict(source=native(copied)))
        report['imported'] = imported
        need(tuple(imported[key] for key in ('nodes', 'skins', 'vertices', 'triangles', 'animations')) == (22, 1, 370, 256, 1),
             'Imported original geometry/skin/clip counts differ')
        need(call('world.inspect') == before_import, 'Asset import changed authored revision')
        asset = imported['asset']
        nodes = page('asset.inspect', dict(asset=asset, section='nodes'))
        skins = page('asset.inspect', dict(asset=asset, section='skins'))
        clips = page('asset.inspect', dict(asset=asset, section='animations'))
        need(len(skins) == 1 and skins[0]['joints'] == 19 and len(clips) == 1 and
             clips[0]['channels'] == 57 and clips[0]['duration'] == 1.25, 'Imported skeleton/take profile differs')
        for i, node in enumerate(nodes):
            need(node['index'] == i and node['name'] == oracle.nodes[i]['name'] and
                 node['parent'] == oracle.parents.get(i, -1), 'Imported node mapping differs from original source')
            check_matrix('import_local', transform_matrix(node['position'], node['rotation'], node['scale']), rest['local_matrices'][i])
        palette = page('asset.animation.skin', dict(asset=asset, skin=0))
        need([row['node'] for row in palette] == oracle.skin['joints'], 'Imported skin joint order differs')
        for row, expected in zip(palette, oracle.binds):
            check_matrix('inverse_bind', row['inverse_bind'], expected)
        references = []
        if args.capture:
            for index, pose in enumerate(expected_poses):
                path = sources/f'licensed-reference-{index}.glb'
                with path.open('xb') as output:
                    output.write(oracle.reference_glb(pose))
                reference = call('asset.import', dict(source=native(path)))
                references.append(reference['asset'])
                report.setdefault('reference_inputs', []).append(dict(time=pose['time'], sha256=sha(path), asset=reference['asset']))
        component = lambda identifier, kind, value: dict(op='component.set', id=identifier, type=kind, value=value)
        identity = dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
        call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=[
            dict(op='asset.instantiate', id=rig, name='Original licensed Rigged Figure', asset=asset),
            dict(op='entity.create', id=camera, name='Reference observer'),
            component(camera, 'Transform', dict(identity, position=[0, .75, 3])),
            component(camera, 'Camera', dict(vertical_fov=40, near=.05, far=100))]))
        authored = world.read_bytes()
        authored_inspection, authored_history = call('world.inspect'), call('world.history')
        for i in range(22):
            entity = call('entity.get', dict(id=node_id(rig, i), revision=1))['value']
            local = entity['components']['Transform']
            check_matrix('authored_local', transform_matrix(**local), rest['local_matrices'][i])
            check_matrix('authored_world', call('entity.world_transform', dict(id=node_id(rig, i), revision=1))['matrix'], rest['worlds'][i])
        # Remove only our byte-identical import copy and owned reference sources.
        for path in sources.iterdir():
            need(path.is_file() and not path.is_symlink(), 'Unexpected owned source entry')
            path.unlink()
        sources.rmdir()
        packages = package_tree()
        need(all(packages.get(key+'.pmodel') == key for key in [asset, *references]),
             'Cooked character/reference is not content addressed')
        report['cooked_inputs'] = packages
        close_owner()
        open_owner()
        need(world.read_bytes() == authored and call('world.inspect') == authored_inspection, 'Fresh owner changed authored state')
        need(package_tree() == packages and not sources.exists(), 'Fresh owner lost cooked closure or source independence')
        # Undo history is owner-session local; compare runtime against this new
        # owner's baseline rather than requiring persistence across reopening.
        authored_history = call('world.history')
        session = uuid.uuid4().hex
        if args.runtime:
            call('runtime.start', dict(session_id=session, revision=1))
        tick = 0
        for expected in expected_poses:
            observe_pose(asset, expected)
            if args.runtime:
                step = call('runtime.step', dict(session_id=session, request_id=uuid.uuid4().hex, expected_tick=tick, ticks=1,
                    animations=[dict(entity=rig, clip=0, time=expected['time'], speed=1, loop=False, playing=False)]))
                tick += 1
                need(step['tick'] == tick, 'Native runtime did not advance one atomic seek tick')
                for i in range(22):
                    result = call('runtime.entity', dict(session_id=session, tick=tick, id=node_id(rig, i)))
                    check_matrix('runtime_local', transform_matrix(**result['local_transform']), expected['local_matrices'][i])
                    check_matrix('runtime_world', result['world_matrix'], expected['worlds'][i])
                clock = call('runtime.entity', dict(session_id=session, tick=tick, id=rig))['animation']
                need(clock['time'] == expected['time'] and not clock['playing'], 'Frozen clip seek advanced or changed playback')
        if args.runtime:
            call('runtime.stop', dict(session_id=session))
        else:
            report['runtime_skipped'] = 'Explicit --runtime 0; only import, authoring, CPU sampling and cooked closure qualified'
        need(world.read_bytes() == authored and call('world.inspect') == authored_inspection and
             call('world.history') == authored_history and package_tree() == packages, 'Sampling/playback changed authored or cooked data')
        report['checks'].update(import_profile=True, source_reference_matrices=True, independent_cpu_skinning=True,
            authored_rest=True, fresh_owner_cooked_closure=True,
            owned_sources_removed=True, sampling_preserved_authored_history=True)
        if args.runtime:
            report['checks'].update(runtime_frozen_seek=True, runtime_preserved_authored_history=True)
        if args.capture:
            revision = 1
            first_preview = last_preview = None
            previous_reference = rig
            for index, expected in enumerate(expected_poses):
                # Entity IDs remain retired after deletion, including generated
                # child IDs; each independent reference needs a fresh root.
                reference_id = f'{200+index:032x}'
                ops = [dict(op='entity.delete', id=previous_reference, recursive=True),
                       dict(op='asset.instantiate', id=reference_id, name='Independent source reference', asset=references[index])]
                call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=revision, ops=ops))
                previous_reference = reference_id
                revision += 1
                frozen, history = world.read_bytes(), call('world.history')
                preview = dict(revision=revision, asset=asset, clip=0, time=expected['time'], loop=False)
                gpu = capture(f'{index}-original-gpu', 'asset.animation.capture', dict(preview, skinning='gpu'))
                cpu = capture(f'{index}-original-cpu', 'asset.animation.capture', dict(preview, skinning='cpu'))
                reference = capture(f'{index}-independent', 'world.capture', dict(revision=revision))
                gpu_report = report['captures'][f'{index}-original-gpu']['report']
                need(gpu_report['animation']['skinning'] == 'gpu_compute' and
                     gpu_report['render_diagnostics']['last_draws']['skinned_vertices'] == 370,
                     'Preview did not exercise actual GPU skinning of original character')
                need(report['captures'][f'{index}-original-cpu']['report']['animation']['skinning'] == 'cpu_reference',
                     'Preview did not exercise CPU reference skinning')
                image_compare(f'{index}-gpu-vs-independent', gpu, reference)
                image_compare(f'{index}-cpu-vs-independent', cpu, reference)
                image_compare(f'{index}-gpu-vs-cpu', gpu, cpu)
                background = reference[0][0]
                visible = sum(pixel != background for row in reference for pixel in row)
                need(visible > 2000, 'Independent reference capture is blank or too small')
                report.setdefault('visible_reference_pixels', {})[str(index)] = visible
                need(world.read_bytes() == frozen and call('world.history') == history and package_tree() == packages,
                     'Capture altered authored history/cooked packages')
                if first_preview is None:
                    first_preview = gpu
                last_preview = gpu
            motion = image_compare('visible_arm_motion', first_preview, last_preview, enforce=False)
            need(motion['pixels_over_2'] > 500, 'Rendered original character has no meaningful visible arm motion')
            report['checks']['vulkan_independent_reference'] = True
        report['checks']['caller_source_unchanged'] = sha(args.source) == SOURCE_SHA
        report['checks']['binary_unchanged'] = sha(args.binary) == report['binary_sha256']
        need(report['checks']['caller_source_unchanged'] and report['checks']['binary_unchanged'], 'Caller source or native image changed')
        report['passed'] = True
    except BaseException as error:
        report['failure'] = dict(type=type(error).__name__, message=str(error), traceback=traceback.format_exc())
    finally:
        try:
            close_owner()
        except BaseException as error:
            report['passed'] = False
            report['close_failure'] = repr(error)
        if sha(args.source) != SOURCE_SHA or sha(args.binary) != report['binary_sha256']:
            report['passed'] = False
            report['immutability_failure'] = 'Caller source or native image changed'
        report['rpc_count'] = len(report['calls'])
        with (run/'evidence.json').open('x', encoding='utf-8') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
        print(run/'evidence.json')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
