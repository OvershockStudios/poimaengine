#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native model-space posed bounds, with original analytic geometry and raw wires.

Windows execution requires a caller-owned Job. Native POSIX outputs default to
an OS-local retained directory. Optional original character inputs are read
without modification; no renderer, compiler, download or generated art is used.
"""
import argparse
import copy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

from animation_fixture import ribbon
from gltf_fixture import glb

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

MAX_CALLS, MAX_BYTES, MAX_FILES = 1000, 128*1024**2, 4096
POINTS = [(-1., -1., 0.), (2., -1., 0.), (0., 3., 0.), (11., 7., 5.)]
RESULT_KEYS = {'asset', 'clip', 'requested_time', 'sample_time', 'space', 'empty',
               'bounds', 'primitive_instances', 'vertex_instances'}


def need(value, message):
    if not value: raise AssertionError(message)


def same(a, b):
    if type(a) is not type(b): return False
    if type(a) is dict: return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if type(a) is list: return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def pin(path):
    s = path.lstat()
    need(stat.S_ISREG(s.st_mode) and not getattr(s, 'st_file_attributes', 0) & 0x400,
         'Expected ordinary input/output file')
    need(s.st_size <= 64*1024**2, 'Individual fixture file exceeds 64 MiB')
    with path.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.lstat()
    need((s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), 'File changed while hashing')
    return dict(bytes=s.st_size, sha256=digest)


def mesh_fixture(vertices=4, bindings=1, primitives=1):
    """Shared indexed geometry retains unused vertices, independently of indices."""
    points = POINTS + [POINTS[0]]*(vertices-4)
    packed = b''.join(struct.pack('<6f', *p, 0., 0., 1.) for p in points)
    blob = packed+struct.pack('<3H', 0, 1, 2)
    document = dict(asset=dict(version='2.0', generator='Poima original analytic bounds fixture'),
        buffers=[dict(byteLength=len(blob))],
        bufferViews=[dict(buffer=0, byteOffset=0, byteLength=len(packed), byteStride=24),
                     dict(buffer=0, byteOffset=len(packed), byteLength=6)],
        accessors=[dict(bufferView=0, componentType=5126, count=vertices, type='VEC3',
                        min=[-1., -1., 0.], max=[11., 7., 5.]),
                   dict(bufferView=0, byteOffset=12, componentType=5126, count=vertices, type='VEC3'),
                   dict(bufferView=1, componentType=5123, count=3, type='SCALAR')],
        meshes=[dict(primitives=[dict(attributes=dict(POSITION=0, NORMAL=1), indices=2)
                                 for _ in range(primitives)])],
        nodes=[dict(name='Binding '+str(i), mesh=0) for i in range(bindings)],
        scenes=[dict(nodes=list(range(bindings)))], scene=0)
    return document, blob


class Fixture:
    def __init__(self, args):
        self.args, self.output = args, args.output
        self.deadline = time.monotonic()+180
        self.work_deadline = self.deadline-10
        self.client = None
        self.unknown = False
        self.record = dict(format='poima.model-bounds-contract.v1', passed=False, calls=[], owners=[],
                           checks=[], snapshots=[], sources={}, errors=[], cleanup_errors=[])
        self.sources = self.output/'sources'
        self.sources.mkdir()
        self.world = self.output/'world.json'
        inputs = [Path(__file__).resolve(), ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py', args.binary]
        inputs += [p for p in (ROOT/'tools/python/poima_client').iterdir() if p.suffix in ('.py', '.json')]
        if args.binary.with_name('phonon.dll').exists(): inputs.append(args.binary.with_name('phonon.dll'))
        self.record['inputs'] = {str(p): pin(p) for p in inputs}

    def remaining(self):
        left = self.work_deadline-time.monotonic()
        need(left > 0, 'Work deadline exhausted; cleanup time reserved')
        return left

    def native(self, path):
        text = str(Path(path).resolve())
        if self.args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=5).strip()
        return text

    def census(self, reserve=0, files=0):
        count = total = 0
        pending = [self.output]
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    try: info = entry.stat(follow_symlinks=False)
                    except FileNotFoundError: continue
                    need(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                         'Linked fixture output')
                    if stat.S_ISDIR(info.st_mode): pending.append(Path(entry.path))
                    else:
                        need(stat.S_ISREG(info.st_mode), 'Nonordinary output')
                        count += 1
                        total += info.st_size
        need(count+files <= MAX_FILES and total+reserve <= MAX_BYTES, 'Whole output bound exceeded')
        return dict(files=count, bytes=total)

    def open(self, label):
        self.remaining()
        need(self.client is None and not self.unknown, 'No overlapping or unknown native owner')
        self.label = label
        trace = self.output/('trace-'+label)
        self.row = dict(label=label, closed=False, exit_code=None, trace=str(trace))
        self.record['owners'].append(self.row)
        try:
            self.client = WorldClient.open(str(self.args.binary), self.native(self.world),
                trace_directory=trace, trace_limit_bytes=32*1024**2, close_timeout=3)
            self.row['pid'] = self.client.transport.process_id
        except BaseException:
            self.unknown = True
            self.row['startup_error'] = traceback.format_exc()
            raise

    def call(self, method, params=None, error=None):
        self.remaining()
        need(not self.unknown and len(self.record['calls']) < MAX_CALLS, 'Unknown outcome or RPC bound')
        self.census()
        timeout = min(20, self.remaining())
        row = dict(owner=self.label, method=method, params=params if params is not None else {})
        self.record['calls'].append(row)
        try:
            value = self.client.call(method, row['params'], timeout=timeout)
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message)
            if failure.data is not None: row['error']['data'] = failure.data
            need(error is not None and same(failure.code, error), 'Unexpected native rejection: '+str(row))
            return row['error']
        except BaseException:
            self.unknown = True
            row['outcome_unknown'] = True
            raise
        row['result'] = value
        need(error is None, 'Expected native rejection, received success')
        return value

    def close(self):
        if self.client is None: return
        client, self.client = self.client, None
        try:
            client.close()
            need(client.transport.returncode == 0 and client.cleanup_error is None, 'Native closure failed')
        except BaseException:
            self.unknown = True
            self.row['cleanup_error'] = traceback.format_exc()
            raise
        finally:
            self.row.update(closed=client.transport.closed, exit_code=client.transport.returncode,
                            stream_threads_closed=all(not t.is_alive() for t in client.transport._threads))
            self.row['traces'] = {p.name: pin(p) for p in Path(self.row['trace']).iterdir() if p.is_file()}

    def state(self, label):
        value = dict(inspect=self.call('world.inspect'), history=self.call('world.history'),
                     entities=self.call('entity.query'))
        value['files'] = {}
        for suffix in ('', '.previous', '.pending', '.previous.pending'):
            path = Path(str(self.world)+suffix)
            try: info = path.lstat()
            except FileNotFoundError:
                value['files'][suffix] = None
                continue
            need(stat.S_ISREG(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'Nonordinary authored payload')
            need(info.st_size <= 16*1024**2, 'Authored payload bound')
            value['files'][suffix] = path.read_bytes().hex()
        store = Path(str(self.world)+'.assets')
        value['store_exists'] = store.exists()
        value['store'] = {}
        if value['store_exists']:
            info = store.lstat()
            need(stat.S_ISDIR(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'Asset store is not an ordinary directory')
            value['store'] = {p.name: pin(p) for p in sorted(store.iterdir())}
        self.record['snapshots'].append(dict(label=label, owner=self.label, value=value))
        return value

    def source(self, name, document, blob):
        path = self.sources/name
        path.write_bytes(glb(document, blob))
        self.record['sources'][str(path)] = pin(path)
        return path

    def imported(self, source):
        self.record['sources'][str(source)] = pin(source)
        return self.call('asset.import', dict(source=self.native(source)))['asset']

    def rejected_request(self, request, label):
        before = self.state(label+'-before')
        result = self.call('asset.animation.bounds', request, -32602)
        need(same(before, self.state(label+'-after')), 'Malformed request changed the complete state')
        self.record['checks'].append(dict(label=label, request=request, result=result, whole_state_preserved=True))

    def bounds(self, asset, time_value, minimum=None, maximum=None, instances=0, vertices=0,
               clip=None, loop=None, sample_time=None, label='bounds', error=None, extra=None,
               error_message=None, exact_extrema=False):
        request = dict(asset=asset, time=time_value)
        if clip is not None: request['clip'] = clip
        if loop is not None: request['loop'] = loop
        if extra: request.update(extra)
        before = self.state(label+'-before')
        result = self.call('asset.animation.bounds', request, error)
        after = self.state(label+'-after')
        need(same(before, after), 'Read/rejection changed complete authored/native/history/store state')
        if error_message is not None:
            need(result['message'] == error_message, 'Expected exact native failure reason')
        if error is None:
            need(type(result) is dict and set(result) == RESULT_KEYS and result['asset'] == asset
                 and same(result['clip'], clip) and type(result['requested_time']) is float
                 and result['requested_time'] == time_value and type(result['sample_time']) is float
                 and result['sample_time'] == (time_value if sample_time is None else sample_time)
                 and result['space'] == 'model' and same(result['empty'], minimum is None)
                 and same(result['primitive_instances'], instances) and same(result['vertex_instances'], vertices),
                 'Closed result shape, time and per-binding counts')
            if minimum is None: need(result['bounds'] is None, 'Empty geometry must have explicit null bounds')
            else:
                need(set(result['bounds']) == {'min', 'max'}, 'Closed AABB shape')
                for key, expected in (('min', minimum), ('max', maximum)):
                    actual = result['bounds'][key]
                    need(type(actual) is list and len(actual) == len(expected) == 3,
                         'Exact three-axis AABB')
                    for a, b in zip(actual, expected):
                        need(type(a) is float and math.isfinite(a) and abs(a-b) <= 2e-5*(1+abs(b)),
                             'Independent analytic/page-extrema AABB differs')
                if exact_extrema:
                    need(same(result['bounds'], dict(min=minimum, max=maximum)),
                         'Whole bounds differ from complete stored vertex-page extrema')
        self.record['checks'].append(dict(label=label, request=request, result=result,
                                         whole_state_preserved=True, expected_min=minimum, expected_max=maximum))
        return result

    def paged_extrema(self, asset, time_value, clip=None, loop=False, label='sampled'):
        """Independent traversal of selected bindings and every vertex response."""
        before = self.state(label+'-pages-before')
        def page(section):
            rows, offset = [], 0
            while True:
                response = self.call('asset.inspect', dict(asset=asset, section=section, offset=offset, limit=64))
                rows.extend(response['items'])
                follow = response['next_offset']
                if follow is None:
                    need(len(rows) == response[section], 'Whole cooked metadata census')
                    return rows, response
                need(type(follow) is int and follow > offset, 'Metadata page progression')
                offset = follow
        nodes, summary = page('nodes')
        primitives, _ = page('primitives')
        need([n['index'] for n in nodes] == list(range(len(nodes))) and
             [p['index'] for p in primitives] == list(range(len(primitives))), 'Complete original metadata order')
        selected, pending = set(), list(summary['roots'])
        children = {n['index']: [] for n in nodes}
        for node in nodes:
            if node['parent'] >= 0: children[node['parent']].append(node['index'])
        while pending:
            index = pending.pop()
            need(index not in selected, 'Selected hierarchy has duplicate ancestry')
            selected.add(index)
            pending.extend(children[index])
        minimum, maximum, instances, vertices = [math.inf]*3, [-math.inf]*3, 0, 0
        effective = None
        for index in sorted(selected):
            for primitive in nodes[index]['primitives']:
                instances += 1
                offset, count = 0, 0
                while True:
                    request = dict(asset=asset, time=time_value, loop=loop, section='vertices',
                                   node=index, primitive=primitive, offset=offset, limit=64)
                    if clip is not None: request['clip'] = clip
                    response = self.call('asset.animation.sample', request)
                    if effective is None: effective = response['sample_time']
                    need(same(response['sample_time'], effective), 'All bound instances share one sample clock')
                    for item in response['items']:
                        need(same(item['index'], count), 'Complete sampled vertex sequence')
                        point = item['world_position']
                        need(type(point) is list and len(point) == 3 and all(math.isfinite(x) for x in point), 'Complete finite vertex')
                        for axis in range(3):
                            minimum[axis] = min(minimum[axis], point[axis])
                            maximum[axis] = max(maximum[axis], point[axis])
                        count += 1
                    follow = response['next_offset']
                    if follow is None:
                        need(count == response['total'] == primitives[primitive]['vertices'], 'Every stored vertex included')
                        break
                    need(type(follow) is int and follow > offset, 'Vertex page progression')
                    offset = follow
                vertices += count
        after = self.state(label+'-pages-after')
        need(same(before, after), 'Full vertex paging changed the complete world/store')
        self.bounds(asset, time_value, minimum if vertices else None, maximum if vertices else None,
                    instances, vertices, clip=clip, loop=loop, sample_time=effective, label=label,
                    exact_extrema=True)

    def run(self):
        try:
            self.open('warm')
            description = self.call('world.describe', dict(view='method', name='asset.animation.bounds'))
            schema = description['methods']['asset.animation.bounds']
            need(set(schema['properties']) == {'asset','time','clip','loop'} and
                 set(schema['required']) == {'asset','time'} and schema['additionalProperties'] is False,
                 'Discoverable closed request schema')
            for i in range(2):
                self.call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=i,
                    ops=[dict(op='entity.create', id=str(i+1)*32, name='Bounds read preservation '+str(i))]))
            document, blob = mesh_fixture()
            document['nodes'] = [dict(name='Selected parent', translation=[10,20,30], scale=[2,3,4],
                                     rotation=[0,0,math.sqrt(.5),math.sqrt(.5)], children=[1]),
                                 dict(name='Selected child', mesh=0),
                                 dict(name='Shared instance', mesh=0, translation=[-5,0,0]),
                                 dict(name='Unselected geometry', mesh=0, translation=[100000,0,0])]
            document['scenes'][0]['nodes'] = [0,2]
            static = self.imported(self.source('roots-shared.glb', document, blob))
            self.bounds(static, 17, [-11,-1,0], [13,42,50], 2, 8, label='roots-shared-unused-vertex')
            self.bounds(static, 1e9, [-11,-1,0], [13,42,50], 2, 8, loop=True, label='rest-max-time-loop-noop')
            self.paged_extrema(static, 17, label='static-all-vertices')
            empty_doc = copy.deepcopy(document)
            empty_doc['nodes'] = [dict(name='Selected empty'), dict(name='Unselected mesh', mesh=0)]
            empty_doc['scenes'][0]['nodes'] = [0]
            empty = self.imported(self.source('empty-selected.glb', empty_doc, blob))
            self.bounds(empty, 99, label='empty-selected')
            document, blob = ribbon()
            rigid_mesh = copy.deepcopy(document['meshes'][0])
            rigid_mesh['primitives'][0]['attributes'].pop('JOINTS_0')
            rigid_mesh['primitives'][0]['attributes'].pop('WEIGHTS_0')
            document['meshes'].append(rigid_mesh)
            document['nodes'].extend([dict(name='Rigid ribbon binding', mesh=1, translation=[-4,-1,2], scale=[2,1,1]),
                                      dict(name='Second skin binding', mesh=0, skin=0, translation=[100,0,0])])
            document['scenes'][0]['nodes'].extend([3,4])
            mixed = self.imported(self.source('mixed-rigid-skin.glb', document, blob))
            for requested, selected_clip, looping, effective, xmax, label in (
                (99,None,False,99,.5,'mixed-rest'), (1,0,False,1,1.5,'mixed-linear'),
                (3,0,False,2,2.5,'mixed-clamp'), (3,0,True,1,1.5,'mixed-wrap'),
                (2,0,True,0,.5,'mixed-loop-boundary')):
                self.bounds(mixed, requested, [-5,-1,0], [xmax,2,2], 3, 18,
                            clip=selected_clip, loop=looping, sample_time=effective, label=label)
            self.paged_extrema(mixed, 1, clip=0, label='mixed-all-vertices')
            singular_doc, singular_blob = ribbon()
            singular_data = bytearray(singular_blob)
            singular_data.extend(b'\0'*((-len(singular_data)) % 4))
            start = len(singular_data)
            singular_data.extend(struct.pack('<8f', 0,0,1,0, 0,0,1,0))
            singular_doc['bufferViews'].append(dict(buffer=0, byteOffset=start, byteLength=32))
            singular_doc['accessors'].append(dict(bufferView=len(singular_doc['bufferViews'])-1,
                                                 componentType=5126, count=2, type='VEC4'))
            singular_doc['buffers'][0]['byteLength'] = len(singular_data)
            singular_doc['animations'][0]['samplers'][0]['output'] = len(singular_doc['accessors'])-1
            singular_doc['animations'][0]['channels'][0]['target']['path'] = 'rotation'
            singular = self.imported(self.source('singular-blended-pose.glb', singular_doc, singular_data))
            self.bounds(singular, 1, clip=0, label='singular-pose', error=-32050,
                        error_message='Singular transform.')
            original = ROOT/'examples/assets/animated-ribbon.glb'
            existing = self.imported(original)
            self.paged_extrema(existing, 0, label='original-ribbon-rest')
            self.paged_extrema(existing, 1, clip=0, label='original-ribbon-posed')
            self.paged_extrema(existing, 3, clip=0, loop=True, label='original-ribbon-loop')
            for vertices, bindings, primitives, code, label in (
                (1000,1000,1,None,'exact-million-vertex-instances'),
                (1000,1001,1,-32050,'over-million-vertex-instances'),
                (4,5000,2,None,'exact-ten-thousand-primitive-instances'),
                (4,5001,2,-32050,'over-ten-thousand-primitive-instances')):
                doc, blob = mesh_fixture(vertices, bindings, primitives)
                asset = self.imported(self.source(label+'.glb', doc, blob))
                summary = self.call('asset.inspect', dict(asset=asset, section='primitives'))
                need(len(summary['items']) == primitives and
                     all(same(item['vertices'], vertices) for item in summary['items']),
                     'Budget recipe retained its declared complete cooked vertices')
                self.bounds(asset, 0, [-1,-1,0], [11,7,5], bindings*primitives,
                            vertices*bindings*primitives, label=label, error=code,
                            error_message=('Model bounds vertex-instance budget exceeded.' if vertices == 1000 else
                                           'Model bounds primitive-instance budget exceeded.') if code is not None else None)
            invalid = [dict(time=-1), dict(time=True), dict(time='1'), dict(time=None), dict(time=1e9+1),
                       dict(clip=-1), dict(clip=True), dict(clip=0.0), dict(clip=None), dict(clip=256), dict(loop=1),
                       dict(offset=0), dict(limit=64), dict(node=0), dict(section='vertices'),
                       dict(primitive=0), dict(asset='bad'), dict(asset=1)]
            for index, change in enumerate(invalid):
                self.bounds(mixed, 0, label='invalid-'+str(index), error=-32602, extra=change)
            for request, label in (({}, 'missing-both'), ({'asset':mixed}, 'missing-time'),
                                   ({'time':0}, 'missing-asset'), ([], 'params-array')):
                self.rejected_request(request, label)
            self.bounds(mixed, 0, clip=1, label='in-range-selector-outside-model', error=-32602)
            self.bounds(mixed, 0, clip=999, label='outside-global-clip-limit', error=-32602)
            self.bounds('0'*64, 0, label='missing-package', error=-32050)
            package = Path(str(self.world)+'.assets')/(static+'.pmodel')
            broken = bytearray(package.read_bytes())
            broken[0] ^= 1
            corrupt = hashlib.sha256(broken).hexdigest()
            bad_path = package.with_name(corrupt+'.pmodel')
            bad_path.write_bytes(broken)
            self.bounds(corrupt, 0, label='content-hash-valid-invalid-codec', error=-32050)
            for option, label in ((self.args.knight_source, 'original-knight'), (self.args.kenney_source, 'original-kenney')):
                if option is not None:
                    expected = ('60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168'
                                if label == 'original-knight' else
                                '18835fef534eede635b081ee7fe647d01a885550a591d2e6bf071010906167d8')
                    need(pin(option)['sha256'] == expected, 'Optional original character source differs')
                    asset = self.imported(option)
                    self.paged_extrema(asset, 0, label=label+'-rest')
                    if label == 'original-knight': self.paged_extrema(asset, .5, clip=36, label=label+'-idle')
            before = self.state('warm-before-close')
            cold_expected = copy.deepcopy(before)
            for key in ('undo_count', 'redo_count', 'bytes', 'skipped_large_edits'):
                cold_expected['history'][key] = 0
            self.close()
            away = self.output/'sources-retained-away'
            self.sources.rename(away)
            need(not self.sources.exists(), 'Owned admitted source paths remain available')
            self.open('cold')
            after = self.state('cold-open')
            need(same(cold_expected, after), 'Complete cold state differs from explicit new-owner history forecast')
            self.bounds(mixed, 1, [-5,-1,0], [1.5,2,2], 3, 18, clip=0, label='source-absent-cold-bounds')
            self.close()
            self.record['sources_unchanged'] = all(pin((away/p.relative_to(self.sources)) if p.is_relative_to(self.sources) else p) == value
                for name, value in self.record['sources'].items() for p in [Path(name)])
            need(self.record['sources_unchanged'], 'Original source input changed')
            self.record['inputs_unchanged'] = all(pin(Path(name)) == value for name, value in self.record['inputs'].items())
            need(self.record['inputs_unchanged'], 'Original executable/helper/SDK input changed')
            self.record['functional_completed'] = True
        except BaseException:
            self.record['errors'].append(traceback.format_exc())
        finally:
            try: self.close()
            except BaseException: self.record['cleanup_errors'].append(traceback.format_exc())
            self.record['unknown_outcome'] = self.unknown
            self.record['seconds'] = 180-(self.deadline-time.monotonic())
            self.record['passed'] = self.record.get('functional_completed') is True and not self.unknown and not self.record['errors'] and not self.record['cleanup_errors'] and self.record['seconds'] < 180
            try:
                encoded = (json.dumps(self.record, separators=(',', ':'), allow_nan=False)+'\n').encode()
                self.record['census_before_report'] = self.census(len(encoded)+1024, 1)
                encoded = (json.dumps(self.record, separators=(',', ':'), allow_nan=False)+'\n').encode()
                self.census(len(encoded), 1)
                with (self.output/'evidence.json').open('xb') as stream: stream.write(encoded)
            except BaseException:
                traceback.print_exc()
                return 1
        print(json.dumps(dict(passed=self.record['passed'], evidence=str(self.output/'evidence.json'))), flush=True)
        return 0 if self.record['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--knight-source', type=Path)
    parser.add_argument('--kenney-source', type=Path)
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    for name in ('knight_source', 'kenney_source'):
        if getattr(args, name) is not None: setattr(args, name, getattr(args, name).resolve(strict=True))
    if args.output is None:
        parent = Path(tempfile.gettempdir())/'poima-model-bounds' if os.name == 'posix' and not args.windows_interop else ROOT/'build/model-bounds'
        args.output = parent/uuid.uuid4().hex
    args.output = args.output.resolve()
    need(not args.output.exists(), 'Output must be a fresh directory')
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        contained = ctypes.c_int()
        need(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(contained)) and contained.value,
             'Windows qualification requires a caller-owned Job')
    args.output.mkdir(parents=True)
    return Fixture(args).run()


if __name__ == '__main__':
    raise SystemExit(main())
