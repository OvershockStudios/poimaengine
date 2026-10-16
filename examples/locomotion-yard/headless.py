#!/usr/bin/env python3
"""Author, verify, and export Locomotion Yard without opening a window.

Supply the licensed source directory, generated component manifest, and a built
Native AOT descriptor. This workflow does not download assets or compile gameplay.
Each output directory is a new, retained run. See evidence.json and traces/ for
the original requests, responses, saves, and process outcomes.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

_spec = importlib.util.spec_from_file_location('poima_locomotion_launcher', Path(__file__).with_name('run.py'))
if _spec is None or _spec.loader is None:
    raise ImportError('The Locomotion Yard launcher is missing.')
yard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(yard)

CAP, MAX_FILES, MAX_CALLS = 512*1024**2, 4096, 4096
TRACE_CAP, LOG_CAP = 32*1024**2, 16*1024**2
PAYLOAD_KEYS = {'animation', 'authored_revision', 'cameras', 'components', 'content_sha256',
                'control_sequence', 'entities', 'gameplay', 'gameplay_revision', 'sound',
                'structure', 'tick', 'ui', 'world_id'}


def need(value, message):
    if not value:
        raise RuntimeError(message)


def exact(left, right):
    """JSON equality that preserves integer/float and boolean distinctions."""
    if type(left) is not type(right):
        return False
    if type(left) is dict:
        return left.keys() == right.keys() and all(exact(left[k], right[k]) for k in left)
    if type(left) is list:
        return len(left) == len(right) and all(exact(a, b) for a, b in zip(left, right))
    return left == right


def parse(data):
    def pairs(rows):
        result = {}
        for key, value in rows:
            need(key not in result, 'Duplicate JSON member: '+key)
            result[key] = value
        return result

    def bad(value):
        raise ValueError('Nonfinite JSON value: '+value)
    return json.loads(data, object_pairs_hook=pairs, parse_constant=bad)


def ordinary(path):
    info = Path(path).lstat()
    need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and
         not getattr(info, 'st_file_attributes', 0) & 0x400,
         'Expected an ordinary file: '+str(path))
    return info


def raw(path, limit=64*1024**2):
    need(ordinary(path).st_size <= limit, 'File exceeds its read bound: '+str(path))
    return Path(path).read_bytes()


def read(path):
    return parse(raw(path))


def pin(path):
    before = ordinary(path)
    need(before.st_size <= 512*1024**2, 'File exceeds the hashing bound: '+str(path))
    with Path(path).open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = ordinary(path)
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), 'File changed while hashing')
    return dict(bytes=after.st_size, sha256=digest)


def write(path, value):
    data = (json.dumps(value, indent=2, allow_nan=False)+'\n').encode('utf-8')
    need(len(data) <= 16*1024**2, 'JSON output exceeds 16 MiB')
    with Path(path).open('xb') as stream:
        stream.write(data)


def inventory(root):
    result, total = {}, 0
    for path in sorted(Path(root).rglob('*')):
        info = path.lstat()
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
             'Linked directory inventory member: '+str(path))
        if stat.S_ISDIR(info.st_mode):
            continue
        need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'Nonordinary inventory member')
        total += info.st_size
        need(len(result) < MAX_FILES and total <= CAP, 'Inventory exceeds 4096 files or 512 MiB')
        value = pin(path)
        result[path.relative_to(root).as_posix()] = value
    return result


def contained(root, label):
    need(type(label) is str and '\\' not in label, 'Expected a portable relative path')
    path = PurePosixPath(label)
    need(label == path.as_posix() and not path.is_absolute() and path.parts and
         all(part not in ('', '.', '..') and ':' not in part for part in path.parts),
         'Unsafe relative artifact path: '+label)
    target = Path(root).joinpath(*path.parts)
    need(target.resolve().is_relative_to(Path(root).resolve()), 'Artifact path escapes its root')
    return target


def member(text, key):
    """Locate an original JSON value without reserializing native float tokens."""
    decoder, cursor = json.JSONDecoder(), 0
    def ws(at):
        while at < len(text) and text[at].isspace():
            at += 1
        return at
    cursor = ws(cursor)
    need(cursor < len(text) and text[cursor] == '{', 'Expected a JSON object')
    cursor += 1
    while True:
        cursor = ws(cursor)
        need(cursor < len(text), 'Incomplete JSON object')
        if text[cursor] == '}':
            break
        name, end = decoder.raw_decode(text, cursor)
        cursor = ws(end)
        need(type(name) is str and text[cursor] == ':', 'Expected a JSON member')
        start = ws(cursor+1)
        _, end = decoder.raw_decode(text, start)
        if name == key:
            return text[start:end]
        cursor = ws(end)
        if text[cursor] == '}':
            break
        need(text[cursor] == ',', 'Expected a JSON comma')
        cursor += 1
    raise KeyError(key)


class Workflow:
    def __init__(self, args):
        self.args, self.output = args, args.output
        self.started = time.monotonic()
        self.deadline = self.started+args.timeout
        self.work_deadline = self.deadline-10
        self.unknown = False
        self.active = None
        self.host = None
        self.host_threads, self.host_faults = [], []
        self.host_row = None
        self.project = self.output/'project'
        self.world = self.project/'world.json'
        self.descriptor = args.descriptor
        self.record = dict(format='poima.locomotion-yard.headless.v1', passed=False,
                           stage=args.stage, source_profile=args.source_profile,
                           guard_sources=args.guard_sources, calls=[], owners=[], commands=[],
                           checkpoints=[], immediate_restore_pairs=[], restores=[], observations=[],
                           errors=[], cleanup_errors=[], unknown_outcome=False,
                           scope='Native AOT headless authoring and manual-tick gameplay. Complete immediate '
                           'restore comparisons use original save bytes. Later observations are selected '
                           'gameplay checks; complete later saves remain retained without a cross-owner '
                           'post-callback equality claim. No renderer, input, contact, IK or performance qualification.')
        self.inputs = {}

    def remaining(self, limit=30, cleanup=False):
        left = (self.deadline if cleanup else self.work_deadline)-time.monotonic()
        need(left > 0, 'Headless workflow time budget exhausted')
        return min(limit, left)

    def census(self):
        result = dict(files=0, bytes=0)
        for path in self.output.rglob('*'):
            info = path.lstat()
            need(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                 'Linked output member')
            if stat.S_ISDIR(info.st_mode):
                continue
            need(stat.S_ISREG(info.st_mode), 'Nonordinary output member')
            result['files'] += 1
            result['bytes'] += info.st_size
            need(result['files'] <= MAX_FILES and result['bytes'] <= CAP, 'Whole output bound exceeded')
        return result

    def initialize(self):
        self.manifest = read(self.args.manifest)
        self.artifact_spec = read(self.args.descriptor)
        spec = self.artifact_spec
        need(spec['format'] == 'poima.native-gameplay' and spec['version'] == 2 and
             spec['type'] == yard.TYPE, 'A version-2 Locomotion Yard Native AOT artifact is required')
        need(exact(spec['schema']['components'], self.manifest['schemas']) and
             {row['id'] for row in self.manifest['schemas']} == {yard.CONFIG, yard.COMPONENT},
             'The generated manifest must match both complete artifact component schemas')
        fields = spec['schema']['fields']
        need(len(fields) == 45 and len({f['name'] for f in fields}) == 45,
             'Expected all 45 Locomotion Yard gameplay fields')
        # Adoption/export give the descriptor its canonical project-owned name;
        # the caller may have supplied the same descriptor under another name.
        self.artifact = {'native-gameplay.json': pin(self.args.descriptor)}
        for row in spec['files']:
            path = contained(self.args.descriptor.parent, row['path'])
            need(row['path'] not in self.artifact, 'Duplicate artifact member')
            value = pin(path)
            need(value == dict(bytes=row['size'], sha256=row['sha256']), 'Artifact payload fingerprint differs')
            self.artifact[row['path']] = value
        need(spec['library'] in self.artifact, 'Artifact library must be inventoried')
        self.record['artifact'] = dict(descriptor=str(self.args.descriptor),
                                       engine_version=spec['engine_version'], files=self.artifact)
        paths = [Path(__file__).resolve(), Path(yard.__file__).resolve(),
                 Path(__file__).with_name('knight_profile.py'), self.args.binary, self.args.manifest,
                 self.args.descriptor]
        paths += [contained(self.args.descriptor.parent, row['path']) for row in spec['files']]
        paths += [p for p in (ROOT/'tools/python/poima_client').rglob('*')
                  if p.suffix in ('.py', '.json') and '__pycache__' not in p.parts and p.is_file()]
        if self.args.binary.with_name('phonon.dll').exists():
            paths.append(self.args.binary.with_name('phonon.dll'))
        self.inputs = {str(p): pin(p) for p in paths}
        self.project.mkdir()
        owned_source = self.project/'Source'
        owned_source.mkdir()
        names = list(yard.SOURCE_SHA) if self.args.source_profile == 'kenney-fbx' else ['Knight.glb', 'LICENSE.txt']
        self.record['source_files'] = {}
        for name in names:
            source = contained(self.args.source_directory, name)
            target = contained(owned_source, name)
            self.inputs[str(source)] = pin(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            need(pin(target) == self.inputs[str(source)], 'Owned source copy differs')
            self.record['source_files'][name] = self.inputs[str(source)]
        self.record['inputs'] = self.inputs
        for name in ('traces', 'logs', 'checkpoints', 'saves'):
            (self.output/name).mkdir()
        self.remaining()
        self.census()

    def cli(self, binary, *arguments):
        need(not self.unknown, 'An unknown process outcome prevents further commands')
        folder = self.output/'logs'/('cli-'+str(len(self.record['commands'])))
        folder.mkdir()
        row = dict(argv=[str(binary), *map(str, arguments)], cwd=str(self.output), exit_code=None,
                   stdout=str(folder/'stdout.txt'), stderr=str(folder/'stderr.txt'), forced_cleanup=False)
        self.record['commands'].append(row)
        proc = None
        try:
            with Path(row['stdout']).open('xb') as out, Path(row['stderr']).open('xb') as err:
                proc = subprocess.Popen(row['argv'], cwd=self.output, stdin=subprocess.DEVNULL,
                                        stdout=out, stderr=err)
                row['pid'] = proc.pid
                end = time.monotonic()+self.remaining(90)
                while proc.poll() is None:
                    need(time.monotonic() < end, 'Public CLI deadline exceeded; outcome unknown')
                    need(sum(Path(row[k]).stat().st_size for k in ('stdout', 'stderr')) <= LOG_CAP,
                         'Public CLI log bound exceeded; outcome unknown')
                    time.sleep(.02)
                row['exit_code'] = proc.returncode
                need(sum(Path(row[k]).stat().st_size for k in ('stdout', 'stderr')) <= LOG_CAP,
                     'Public CLI terminal log bound exceeded')
            need(proc.returncode == 0, 'Public CLI failed; original stdout/stderr are retained')
            reply = read(row['stdout'])
            row['reply'] = reply
            need(reply['status'] == 'ok', 'Public CLI returned a failure')
            self.census()
            return reply['result']
        except BaseException:
            if proc is not None and proc.poll() is None:
                self.unknown = True
                row['outcome_unknown'] = True
                row['forced_cleanup'] = True
                try:
                    proc.terminate()
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=2)
                except BaseException:
                    self.record['cleanup_errors'].append(traceback.format_exc())
                row['exit_code'] = proc.returncode
            raise
        finally:
            row['streams'] = {k: pin(Path(row[k])) for k in ('stdout', 'stderr') if Path(row[k]).exists()}

    def descriptor_from(self, manifest):
        spec = read(manifest)
        return contained(manifest.parent, spec['gameplay']['descriptor'])

    def author(self):
        self.active = Owner(self, self.world, 'author', self.args.binary)
        authored = self.active.author(self.project/'Source')
        need(self.active.rpc('world.inspect')['revision'] == 3, 'Expected complete authored revision three')
        self.canonical = self.active.rpc('component.schemas')['schemas']
        self.authored = authored
        self.record['authored'] = authored
        self.record['canonical_components'] = self.canonical
        self.active.close()
        self.active = None
        project = self.project/'project.json'
        write(project, dict(format='poima.project', version=1, project_id=uuid.uuid4().hex,
                            name='Locomotion Yard', entry=dict(world='world.json',
                            controller=yard.uid(yard.PLAYER), camera=yard.uid(yard.CAMERA)), audio=False))
        self.cli(self.args.binary, 'project', 'inspect', project)
        world_before = pin(self.world)
        request = self.output/'adopt-request.json'
        write(request, dict(request_id=uuid.uuid4().hex, expected_project_sha256=pin(project)['sha256'],
                            expected_artifact_sha256=pin(self.args.descriptor)['sha256'], values={}))
        self.record['adoption'] = self.cli(self.args.binary, 'project', 'adopt-gameplay', project,
                                           '--artifact', self.args.descriptor, '--request', request)
        need(pin(self.world) == world_before and read(project)['version'] == 2,
             'Adoption must preserve the complete authored world')
        self.descriptor = self.descriptor_from(project)
        need(pin(self.descriptor) == pin(self.args.descriptor), 'Project-owned descriptor differs')
        for name, value in self.artifact.items():
            need(pin(contained(self.descriptor.parent, name)) == value, 'Adopted artifact closure differs')
        self.frozen = copy.deepcopy(read(self.world))
        for key in ('receipts', 'retired_ids', 'retired_component_schemas', 'retired_template_ids', 'retired_ui_ids'):
            need(key in self.frozen, 'Expected authored-world freeze ledger: '+key)
            self.frozen[key] = []
        self.record['authored_world'] = dict(path=str(self.world), identity=pin(self.world),
                                             frozen_document=self.frozen)
        self.record['project'] = str(project)
        self.record['adoption_request'] = dict(path=str(request), identity=pin(request), request=read(request))
        self.census()

    def validate_module(self, owner):
        module = owner.module()['module']
        need(module['backend'] == 'native_aot' and module['type'] == self.artifact_spec['type'] and
             module['assembly_sha256'] == self.artifact[self.artifact_spec['library']]['sha256'] and
             exact(module['schema'], self.artifact_spec['schema']), 'Loaded native module/schema differs from the supplied artifact')
        self.validate_values(module['values'])
        canonical = owner.rpc('runtime.components', dict(session_id=owner.session))['schemas']
        need(exact(canonical, self.canonical), 'Complete native canonical component registry differs')
        self.record['observations'].append(dict(owner=owner.label, kind='loaded-module', module=module,
                                                canonical_components=canonical))
        return module

    def validate_values(self, values):
        fields = self.artifact_spec['schema']['fields']
        need(type(values) is dict and set(values) == {f['name'] for f in fields}, 'All 45 typed gameplay values are required')
        for field in fields:
            value, kind = values[field['name']], field['kind']
            valid = ((kind == 'int32' and type(value) is int and -(2**31) <= value < 2**31) or
                     (kind == 'int64' and type(value) is str and re.fullmatch(r'0|-?[1-9][0-9]*', value) is not None
                      and -(2**63) <= int(value) < 2**63) or
                     (kind == 'float64' and type(value) is float and math.isfinite(value)) or
                     (kind == 'entity' and type(value) is str and re.fullmatch('[0-9a-f]{32}', value) is not None))
            need(valid, 'Wrong native state type: '+field['name'])

    def checkpoint(self, owner, label):
        request = dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration,
                       slot=label, expected_generation=0, **owner.guards())
        receipt = owner.rpc('save.write', request)
        inspected = owner.rpc('save.inspect', dict(slot=label))
        need(receipt['replayed'] is False and type(receipt['generation']) is int and receipt['generation'] == 1
             and inspected['selected']['verified'] is True, 'Fresh verified save receipt required')
        folder = owner.save_root/('slot-'+label)
        entry = read(folder/'current.json')['payload']['current']
        name = entry['file']
        need(type(name) is str and len(name) == 38 and name.startswith('p-') and name.endswith('.bin')
             and re.fullmatch('[0-9a-f]{32}', name[2:-4]) is not None, 'Unsafe native save payload name')
        source = folder/name
        identity = pin(source)
        need(identity == dict(bytes=receipt['bytes'], sha256=receipt['sha256']) ==
             dict(bytes=entry['bytes'], sha256=entry['sha256']), 'Save receipt/manifest/payload differ')
        need(identity == dict(bytes=inspected['selected']['bytes'], sha256=inspected['selected']['sha256']) and
             type(inspected['selected']['generation']) is int and inspected['selected']['generation'] == 1,
             'Verified save inspection differs from the retained payload')
        destination = self.output/'checkpoints'/(label+'.bin')
        shutil.copyfile(source, destination)
        need(pin(destination) == identity, 'Retained complete save copy differs')
        original = raw(destination)
        document = parse(original)
        need(set(document) == {'format', 'version', 'document', 'snapshot'} and
             document['format'] == 'poima.world-save' and type(document['version']) is int and
             document['version'] == 1 and exact(document['document'], self.frozen), 'Complete frozen world-save envelope differs')
        snapshot = document['snapshot']
        need(set(snapshot) == {'format', 'version', 'sha256', 'payload'} and
             snapshot['format'] == 'poima.runtime-snapshot' and type(snapshot['version']) is int and
             snapshot['version'] == 7 and set(snapshot['payload']) == PAYLOAD_KEYS,
             'Expected the complete 14-key native v7 snapshot')
        need(hashlib.sha256(member(member(original.decode('utf-8'), 'snapshot'), 'payload').encode('utf-8')).hexdigest()
             == snapshot['sha256'], 'Original native snapshot checksum differs')
        gameplay = snapshot['payload']['gameplay']
        need(set(gameplay) == {'assembly_sha256', 'backend', 'schema', 'type', 'values'} and
             gameplay['backend'] == 'native_aot' and gameplay['type'] == yard.TYPE and
             gameplay['assembly_sha256'] == self.artifact[self.artifact_spec['library']]['sha256'] and
             exact(gameplay['schema'], self.artifact_spec['schema']), 'Complete saved module identity differs')
        self.validate_values(gameplay['values'])
        self.record['checkpoints'].append(dict(owner=owner.label, label=label, file=str(destination),
                                               identity=identity, write_request=request, write_result=receipt,
                                               inspect=inspected, source_payload=str(source)))
        self.census()
        return destination

    def restored(self, owner, label, midfade):
        result = owner.restore('midfade')
        need(result['replayed'] is False and type(result['selected_generation']) is int and
             result['selected_generation'] == 1 and result['recovered'] is False, 'Expected a fresh exact-generation restore')
        after = self.checkpoint(owner, label+'-restored')
        need(raw(after) == raw(midfade) and exact(read(after), read(midfade)), 'Complete immediate restored save differs')
        self.record['immediate_restore_pairs'].append(dict(label=label, before=str(midfade), after=str(after),
                                                          whole_bytes_equal=True, whole_typed_equal=True))
        status = owner.rpc('runtime.save.status', dict(session_id=owner.session))
        self.record['restores'].append(dict(owner=owner.label, result=result, status=status))
        self.validate_module(owner)

    def finish(self, owner, label):
        idle = self.authored['clips']['Idle']['index']
        samples = []
        for _ in range(100):
            before = owner.entity()['world_matrix']
            owner.step(10)
            after = owner.entity()['world_matrix']
            values = owner.values()
            need(values['Phase'] != 3 and math.dist([before[12], before[14]], [after[12], after[14]]) <= 4/6+2e-5,
                 'Courier blocked or exceeded its native movement bound')
            gap = math.hypot(max(abs(after[12])-.5, 0), max(abs(after[14])-3, 0))-.42
            need(gap >= -2e-5, 'Observed capsule endpoint entered cover')
            samples.append(dict(tick=owner.tick, position=[after[12], after[14]], cover_gap=gap,
                                arrivals=values['Arrivals'], phase=values['Phase']))
            if values['Arrivals'] == 1:
                break
        else:
            raise RuntimeError('Courier did not deliver within 1000 continuation ticks')
        need(math.hypot(after[12]-4, after[14]) < .16, 'Courier missed the delivery endpoint')
        owner.step(24)
        rig, values = owner.entity(yard.RIG), owner.values()
        need(rig['animation']['clip'] == idle and values['Arrivals'] == 1, 'Delivery did not settle to source Idle')
        self.record['observations'].append(dict(owner=owner.label, kind='delivery', samples=samples,
                                                rig=rig, values=values,
                                                scope='Sparse native capsule endpoints and objective; not every contact or rendered pose.'))
        return self.checkpoint(owner, label+'-delivered')

    def verify(self):
        warm_inputs = inventory(self.project)
        self.active = Owner(self, self.world, 'warm', self.args.binary)
        warm = self.active
        warm.start()
        warm.load()
        self.validate_module(warm)
        warm.saves(self.output/'saves')
        warm.step(30)
        rig, values = warm.entity(yard.RIG), warm.values()
        idle, run = (self.authored['clips'][name]['index'] for name in ('Idle', 'Run'))
        need(rig['animation']['clip'] == idle and values['AnimationClip'] == idle, 'Source Idle is not active')
        self.record['observations'].append(dict(owner='warm', kind='idle', rig=rig, values=values))
        for _ in range(180):
            warm.step(1)
            rig, values = warm.entity(yard.RIG), warm.values()
            if (rig['animation']['clip'] == run and rig['animation']['transition'] is not None and
                    values['Moving'] == 1 and values['LastHorizontalSpeed'] > 0):
                break
        else:
            raise RuntimeError('Source Run and a moving native crossfade were not observed')
        warm.step(3)
        rig = warm.entity(yard.RIG)
        need(rig['animation']['clip'] == run and rig['animation']['transition'] is not None,
             'The checkpoint must remain inside the moving crossfade')
        route = warm.route()
        coordinates = route['values'][yard.COORDINATES]
        need(len(coordinates) >= 12 and len(coordinates) % 3 == 0 and
             any(abs(coordinates[i+2]) > 3.4 for i in range(0, len(coordinates), 3)), 'Expected a persisted cover detour')
        ray = warm.rpc('runtime.raycast', dict(session_id=warm.session, tick=warm.tick,
                       origin=[-4, .8, 0], direction=[1, 0, 0], distance=8, ignore=[yard.uid(yard.ACTOR)]))
        need(ray['hit']['entity'] == yard.uid(2), 'Direct route must meet the authored cover')
        self.record['route'] = dict(component=route, ray=ray, moving_rig=rig)
        midfade = self.checkpoint(warm, 'midfade')
        warm.step(20)
        self.restored(warm, 'warm', midfade)
        self.finish(warm, 'warm')
        warm.stop()
        warm.close()
        self.active = None
        need(inventory(self.project) == warm_inputs, 'Warm runtime verification changed the authored project')
        self.record['warm_project_inventory'] = warm_inputs
        old_project, old_source = self.project, self.project/'Source'
        before = inventory(old_project)
        self.project = self.output/'relocated-project'
        old_project.rename(self.project)
        need(not old_project.exists() and inventory(self.project) == before, 'Complete project relocation differs')
        (self.project/'Source').rename(self.output/'source-retained')
        need(not old_source.exists() and not (self.project/'Source').exists(), 'The admitted source path must be unavailable')
        self.world = self.project/'world.json'
        self.descriptor = self.descriptor_from(self.project/'project.json')
        project_inputs = inventory(self.project)
        self.record['relocation'] = dict(original_project=str(old_project), relocated_project=str(self.project),
                                         original_project_path_absent=True, admitted_source_path=str(old_source),
                                         admitted_source_path_absent=True, caller_originals_preserved=True)
        self.active = Owner(self, self.world, 'cold', self.args.binary)
        cold = self.active
        cold.saves(self.output/'saves')
        self.restored(cold, 'cold', midfade)
        self.finish(cold, 'cold')
        cold.stop()
        cold.close()
        self.active = None
        need(inventory(self.project) == project_inputs, 'Runtime verification changed the authored project')
        self.record['project'] = str(self.project/'project.json')
        return midfade

    def pump_host(self, source, destination):
        try:
            with destination.open('xb') as output:
                total = 0
                while True:
                    block = source.read(8192)
                    if not block:
                        break
                    allowed = min(len(block), TRACE_CAP-total)
                    output.write(block[:allowed])
                    output.flush()
                    total += allowed
                    need(allowed == len(block), 'Host stream exceeds 32 MiB; retained prefix is explicit')
        except BaseException:
            self.host_faults.append(traceback.format_exc())
        finally:
            source.close()

    def export(self, midfade):
        runtime_files = inventory(self.args.runtime)
        self.record['runtime'] = dict(path=str(self.args.runtime), files=runtime_files)
        source = self.output/'export-source-project'
        original_project = inventory(self.project)
        shutil.copytree(self.project, source)
        need(inventory(source) == original_project, 'Export source copy differs')
        bundle = self.output/'exported-game'
        self.cli(self.args.binary, 'project', 'inspect', source/'project.json')
        self.record['export'] = self.cli(self.args.binary, 'project', 'build', source/'project.json',
                                         '--output', bundle, '--runtime', self.args.runtime)
        game = read(bundle/'game.json')
        bundle_files = inventory(bundle)
        expected = {row['path']: dict(bytes=row['size'], sha256=row['sha256']) for row in game['files']}
        need(set(bundle_files) == set(expected) | {'game.json'} and
             all(bundle_files[name] == value for name, value in expected.items()), 'Complete game inventory differs')
        need({n[len('runtime/'):]: v for n, v in bundle_files.items() if n.startswith('runtime/')} == runtime_files,
             'Exported runtime must exactly match the supplied installed runtime')
        need({n[len('gameplay/'):]: v for n, v in bundle_files.items() if n.startswith('gameplay/')} == self.artifact,
             'Exported gameplay must retain the complete provided artifact')
        need(exact(read(bundle/'content/world.json'), self.frozen), 'Exported frozen world differs')
        source.rename(self.output/'export-source-retained-away')
        relocated = self.output/'relocated-game'
        bundle.rename(relocated)
        need(not source.exists() and not bundle.exists() and inventory(relocated) == bundle_files,
             'Export source/bundle relocation failed')
        save_root = self.output/'export-saves'
        shutil.copytree(self.output/'saves', save_root)
        need(inventory(save_root) == inventory(self.output/'saves'), 'External save copy differs')
        unrelated = self.output/'unrelated-cwd'
        unrelated.mkdir()
        bundled = relocated/'runtime/bin'/('poima.exe' if os.name == 'nt' else 'poima')
        self.cli(bundled, 'game', 'inspect', relocated/'game.json')
        endpoint = 'poima-yard-'+uuid.uuid4().hex
        host_folder = self.output/'logs'/'host'
        host_folder.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('POIMA', 'DOTNET', 'COMPLUS', 'CORECLR'))}
        if os.name == 'nt':
            env['PATH'] = str(Path(os.environ['SYSTEMROOT'])/'System32')
        else:
            env['PATH'] = '/usr/bin:/bin'
        self.host_row = dict(argv=[str(bundled), 'game', 'serve', str(relocated/'game.json'),
                                  '--endpoint', endpoint, '--save-root', str(save_root)], cwd=str(unrelated),
                             endpoint=endpoint, stdout=str(host_folder/'stdout.txt'), stderr=str(host_folder/'stderr.txt'),
                             exit_code=None, shutdown_acknowledged=False, forced_cleanup=False,
                             environment_policy=dict(path=env['PATH'], removed_prefixes=['POIMA', 'DOTNET', 'COMPLUS', 'CORECLR']))
        self.record['host'] = self.host_row
        self.host = subprocess.Popen(self.host_row['argv'], cwd=unrelated, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.host_row['pid'] = self.host.pid
        for name in ('stdout', 'stderr'):
            thread = threading.Thread(target=self.pump_host, args=(getattr(self.host, name), Path(self.host_row[name])), daemon=True)
            thread.start()
            self.host_threads.append(thread)
        marker = ('Poima shared world ready: '+endpoint).encode('utf-8')
        ready_end = time.monotonic()+self.remaining(45)
        while True:
            need(not self.host_faults and self.host.poll() is None, 'Standalone host failed before readiness')
            stderr = Path(self.host_row['stderr'])
            if stderr.exists() and marker in raw(stderr):
                break
            need(time.monotonic() < ready_end, 'Standalone host readiness deadline exceeded')
            time.sleep(.02)
        self.host_row['readiness'] = dict(marker=marker.decode(), observed=True)
        self.descriptor = contained(relocated, game['gameplay']['descriptor'])
        self.active = Owner(self, relocated/'content/world.json', 'export', bundled, endpoint=endpoint, cwd=unrelated)
        exported = self.active
        status = exported.rpc('runtime.status')
        need(status['active'] is True and status['tick'] == 0 and exported.rpc('player.inspect')['active'] is False,
             'Standalone service must own a tick-zero headless runtime')
        exported.session, exported.tick = status['session_id'], status['tick']
        exported.configuration, exported.save_root = 1, save_root
        self.validate_module(exported)
        self.restored(exported, 'export', midfade)
        self.finish(exported, 'export')
        exported.stop()
        ack = exported.rpc('host.shutdown')
        need(ack['closed'] is True, 'Standalone host did not acknowledge shutdown')
        self.host_row['shutdown_acknowledged'] = True
        exported.close()
        self.active = None
        self.host.wait(timeout=self.remaining(5, cleanup=True))
        self.host_row['exit_code'] = self.host.returncode
        for thread in self.host_threads:
            thread.join(timeout=self.remaining(2, cleanup=True))
        need(self.host.returncode == 0 and not any(t.is_alive() for t in self.host_threads) and not self.host_faults,
             'Standalone host/stream readers did not close naturally')
        self.host_row['streams'] = {name: pin(Path(self.host_row[name])) for name in ('stdout', 'stderr')}
        need(self.host_row['streams']['stdout']['bytes'] == 0, 'Unexpected unsolicited standalone host stdout')
        need(inventory(relocated) == bundle_files and inventory(self.project) == original_project and
             inventory(self.args.runtime) == runtime_files, 'Immutable game/project/runtime changed')
        self.record['bundle'] = dict(path=str(relocated), files=bundle_files, manifest=game,
                                     unchanged=True, original_bundle_path_absent=True,
                                     export_source_project_path_absent=True,
                                     caller_originals_preserved=True, external_saves=str(save_root))

    def cleanup(self):
        if self.active is not None:
            if self.active.client is None:
                self.record['cleanup_errors'].append('Native owner construction failed; no usable client or verified closure')
            else:
                try:
                    if not self.unknown and self.active.session:
                        self.active.stop(cleanup=True)
                except BaseException:
                    self.record['cleanup_errors'].append(traceback.format_exc())
                finally:
                    try:
                        self.active.close()
                    except BaseException:
                        self.record['cleanup_errors'].append(traceback.format_exc())
        if self.host is not None:
            try:
                if self.host.poll() is None:
                    self.host_row['forced_cleanup'] = True
                    self.unknown = True
                    self.host.terminate()
                    try:
                        self.host.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.host.kill()
                        self.host.wait(timeout=2)
            except BaseException:
                self.record['cleanup_errors'].append(traceback.format_exc())
            finally:
                self.host_row['exit_code'] = self.host.poll()
                for thread in self.host_threads:
                    thread.join(timeout=1)
                self.host_row['reader_threads_closed'] = not any(t.is_alive() for t in self.host_threads)
                self.host_row['stream_errors'] = self.host_faults
                if not self.host_row['reader_threads_closed'] or self.host_faults:
                    self.record['cleanup_errors'].append('Standalone host stream closure failed')

    def run(self):
        try:
            self.initialize()
            self.author()
            if self.args.stage != 'author':
                midfade = self.verify()
                if self.args.stage == 'export':
                    self.export(midfade)
            self.record['functional_completed'] = True
        except BaseException:
            self.record['errors'].append(traceback.format_exc())
        finally:
            self.cleanup()
            try:
                self.record['inputs_unchanged'] = all(pin(Path(path)) == value for path, value in self.inputs.items())
                need(self.record['inputs_unchanged'], 'An original workflow input changed')
                self.record['census_before_report'] = self.census()
            except BaseException:
                self.record['errors'].append(traceback.format_exc())
            self.record['unknown_outcome'] = self.unknown
            self.record['seconds'] = time.monotonic()-self.started
            self.record['passed'] = (self.record.get('functional_completed') is True and
                                     self.record.get('inputs_unchanged') is True and not self.record['errors'] and
                                     not self.record['cleanup_errors'] and not self.unknown and
                                     self.record['seconds'] < self.args.timeout)
            data = (json.dumps(self.record, indent=2, allow_nan=False)+'\n').encode('utf-8')
            census = self.census()
            need(len(data) <= 16*1024**2 and census['bytes']+len(data) <= CAP and census['files']+1 <= MAX_FILES,
                 'Final evidence reservation exceeds the retained output bound')
            with (self.output/'evidence.json').open('xb') as stream:
                stream.write(data)
        print(json.dumps(dict(passed=self.record['passed'], output=str(self.output),
                              evidence=str(self.output/'evidence.json'))), flush=True)
        return 0 if self.record['passed'] else 1


class Owner(yard.OwnedGame):
    """The existing sample owner with public SDK wire traces and strict bounds."""
    def __init__(self, workflow, world, label, binary, endpoint=None, cwd=None):
        self.workflow = workflow
        self.args, self.world, self.label, self.record = workflow.args, Path(world), label, workflow.record
        self.native, self.manifest = lambda p: str(Path(p).resolve()), workflow.manifest
        self.deadline = workflow.work_deadline
        self.session, self.tick, self.revision, self.closed, self.calls = None, 0, 0, False, 0
        self.cleanup_mode = False
        self.client = None
        self.trace = workflow.output/'traces'/label
        self.row = dict(label=label, world=str(world), trace_directory=str(self.trace), closed=False,
                        exit_code=None, calls=0, cleanup_error=None)
        self.record['owners'].append(self.row)
        workflow.active = self
        options = dict(trace_directory=self.trace, trace_limit_bytes=TRACE_CAP,
                       close_timeout=min(3, workflow.remaining()))
        try:
            if endpoint is None:
                self.client = WorldClient.open(str(binary), str(world), cwd=cwd, **options)
            else:
                self.client = WorldClient.connect(str(binary), endpoint, timeout_ms=int(workflow.remaining(10)*1000),
                                                   cwd=cwd, **options)
        except BaseException:
            workflow.unknown = True
            self.row['startup_error'] = traceback.format_exc()
            raise
        self.row['pid'] = self.client.transport.process_id

    def rpc(self, method, params=None, error=None, timeout=20):
        need(not self.workflow.unknown and len(self.record['calls']) < MAX_CALLS, 'Unknown outcome or 4096-RPC bound')
        self.workflow.remaining(cleanup=self.cleanup_mode)
        before = len(self.record['calls'])
        try:
            return super().rpc(method, params, error, timeout)
        except RpcError:
            raise
        except BaseException:
            # An accepted response followed by a local assertion is known. Missing
            # result/error is an unknown native request outcome, never retried.
            if len(self.record['calls']) > before and not any(k in self.record['calls'][-1] for k in ('result', 'error')):
                self.workflow.unknown = True
                self.record['calls'][-1]['outcome_unknown'] = True
            raise
        finally:
            if len(self.record['calls']) > before:
                self.record['calls'][-1]['call_index'] = before

    def config(self):
        return dict(descriptor=str(self.workflow.descriptor), expected_descriptor_sha256=pin(self.workflow.descriptor)['sha256'])

    def guards(self):
        state, module = self.inspect(), self.module()
        components = self.rpc('runtime.components', dict(session_id=self.session))
        return dict(session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=module['revision'],
                    expected_component_revision=components['component_revision'], expected_structure_revision=state['structure_revision'],
                    expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
                    expected_material_revision=state['material_revision'])

    def saves(self, path):
        self.save_root = Path(path)
        self.save_root.mkdir(exist_ok=True)
        super().saves(path)

    def restore(self, slot):
        params = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
                      slot=slot, expected_generation=1, revision=self.rpc('world.inspect')['revision'],
                      new_session_id=uuid.uuid4().hex, gameplay=self.config())
        if self.session:
            params.update(self.guards())
            params['expected_session_id'] = params.pop('session_id')
        else:
            params.update({name: None for name in ('expected_session_id', 'expected_tick', 'expected_gameplay_revision',
                          'expected_component_revision', 'expected_structure_revision', 'expected_ui_revision',
                          'expected_control_sequence', 'expected_material_revision')})
        result = self.rpc('save.load', params)
        self.session, self.tick = params['new_session_id'], result['tick']
        return result

    def stop(self, cleanup=False):
        if self.session is not None:
            if cleanup:
                # Cleanup remains inside its reserved budget, with the same
                # recorded request path; never issue this after unknown outcome.
                self.deadline = self.workflow.deadline
                self.cleanup_mode = True
            self.rpc('runtime.stop', dict(session_id=self.session))
            self.session = None

    def close(self):
        if self.closed:
            return
        try:
            self.client.close()
        except BaseException:
            self.row['cleanup_error'] = traceback.format_exc()
            raise
        finally:
            self.closed = self.client.transport.closed
            self.row.update(closed=self.closed, calls=self.calls, exit_code=self.client.transport.returncode,
                            transport_cleanup_error=str(self.client.cleanup_error) if self.client.cleanup_error else None)
            if self.closed:
                self.row['traces'] = inventory(self.trace)
        need(self.closed and self.row['exit_code'] == 0 and not self.row['cleanup_error'] and not self.client.cleanup_error,
             'Native owner did not close successfully; retained trace may be partial')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source-directory', 'manifest', 'descriptor', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--source-profile', choices=('kenney-fbx', 'kaykit-knight-original-glb-v1'), default='kenney-fbx')
    parser.add_argument('--guard-sources', action='store_true')
    parser.add_argument('--stage', choices=('author', 'verify', 'export'), default='verify')
    parser.add_argument('--runtime', type=Path, help='Matching installed runtime; required for export.')
    parser.add_argument('--timeout', type=float, default=600, help='Whole-run seconds, including a 10-second cleanup reserve (60..1800).')
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or not 60 <= args.timeout <= 1800:
        parser.error('--timeout must be finite and between 60 and 1800 seconds')
    if (args.stage == 'export') != (args.runtime is not None):
        parser.error('--runtime is required only for --stage export')
    for name in ('binary', 'source_directory', 'manifest', 'descriptor', 'runtime'):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve(strict=True))
    args.output = args.output.resolve()
    if args.output.exists():
        parser.error('--output must be a fresh directory')
    protected = [args.source_directory, args.descriptor.parent]
    if args.runtime is not None:
        protected.append(args.runtime)
    if any(args.output.is_relative_to(p) or p.is_relative_to(args.output) for p in protected):
        parser.error('--output must be separate from the source, artifact, and installed runtime')
    args.output.mkdir(parents=True)
    return Workflow(args).run()


if __name__ == '__main__':
    raise SystemExit(main())
