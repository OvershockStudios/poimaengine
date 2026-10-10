#!/usr/bin/env python3
"""Independent Depot Run gameplay/save gate; no compilation or candidate edits.

Only handoff identifiers/field names are consumed. Geometry, ordering, cargo,
interaction range and timer semantics below are independent frozen expectations.
The optional real-player paused-Begin probe is reported separately from stepping.
Explicit managed-development mode is partial qualification, never a native or export gate.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback
import uuid
from pathlib import Path

# SPDX-License-Identifier: Apache-2.0
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def fresh():
    return uuid.uuid4().hex


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def normalize(value):
    """Remove only session identity; preserve all other state and JSON types."""
    if isinstance(value, dict):
        return {key: normalize(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [normalize(item) for item in value]
    return value


CENTERS = dict(start=(0, 0, 3), pickup_a=(-3, 0, 0), delivery_a=(3, 0, 0),
               pickup_b=(-3, 0, -5), delivery_b=(3, 0, -5), finish=(0, 0, -8))
ORDER = ('pickup_a', 'delivery_a', 'pickup_b', 'delivery_b', 'finish')
CARGO = (0, 1, 0, 2, 0)
RADIUS = .8


def exact_keys(value, keys, label):
    need(isinstance(value, dict) and set(value) == set(keys), label+' fields differ')


def required_keys(value, keys, optional, label):
    need(isinstance(value, dict) and set(keys) <= set(value) <= set(keys)|set(optional),
         label+' required/allowed fields differ')


def read(path, limit=64*1024*1024):
    path = Path(path)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= limit,
         'Bounded regular JSON required: '+str(path))
    def pairs(entries):
        result = {}
        for key, value in entries:
            need(key not in result, 'Duplicate JSON field: '+key)
            result[key] = value
        return result
    def number(token):
        result = float(token)
        need(math.isfinite(result), 'Nonfinite JSON number')
        return result
    def invalid(token):
        raise AssertionError('Nonfinite JSON constant: '+token)
    result = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs,
                        parse_float=number, parse_constant=invalid)
    def bounded(value, depth=0):
        need(depth <= 64, 'JSON nesting exceeds 64')
        if isinstance(value, dict):
            for item in value.values(): bounded(item, depth+1)
        elif isinstance(value, list):
            for item in value: bounded(item, depth+1)
    bounded(result)
    return result


def inventory(directory):
    directory = Path(directory)
    need(directory.is_dir() and not directory.is_symlink(), 'Regular inventory root required')
    entries, folded = {}, set()
    for path in sorted(directory.rglob('*')):
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(), 'Inventory contains link/reparse junction')
        if path.is_dir(): continue
        need(path.is_file(), 'Inventory contains nonregular entry')
        name = path.relative_to(directory).as_posix()
        need(name.casefold() not in folded, 'Case-insensitive inventory collision')
        folded.add(name.casefold())
        entries[name] = dict(bytes=path.stat().st_size, sha256=sha(path))
        need(len(entries) <= 50000, 'Inventory exceeds 50000 files')
    return entries


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(',', ':'))


def position(entity):
    matrix = entity['world_matrix']
    need(len(matrix) == 16 and all(type(x) in (int, float) and math.isfinite(x) for x in matrix),
         'Native entity matrix is not finite')
    return matrix[12:15]


def inside(point, center):
    return -.6 <= point[1] <= .6 and math.hypot(point[0]-center[0], point[2]-center[2]) <= RADIUS


def managed_pe(path):
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if header[:2] != b'MZ': return False
        need(len(header) == 64, 'Truncated PE header')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        coff = stream.read(24)
        need(len(coff) == 24 and coff[:4] == b'PE\0\0', 'Malformed PE header')
        count = struct.unpack_from('<H', coff, 20)[0]
        need(2 <= count <= 4096, 'Invalid PE optional header')
        optional = stream.read(count)
        need(len(optional) == count, 'Truncated PE optional header')
        magic = struct.unpack_from('<H', optional)[0]
        need(magic in (0x10b, 0x20b), 'Unsupported PE optional header')
        offset = 96 if magic == 0x10b else 112
        need(len(optional) >= offset, 'Truncated PE directory count')
        if struct.unpack_from('<I', optional, offset-4)[0] <= 14: return False
        need(len(optional) >= offset+15*8, 'Truncated PE COM descriptor')
        return any(struct.unpack_from('<II', optional, offset+14*8))


class BaseOwnership:
    """Thin native calls and controller steering; authoritative state stays native."""

    def native(self, path):
        path = str(Path(path).resolve())
        if self.args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', path], text=True, timeout=10).strip()
        return path

    def rpc(self, method, params=None):
        need(time.monotonic() < self.limit, 'Oracle total deadline exceeded')
        row = dict(method=method, params=params or {})
        self.record['calls'].append(row)
        try:
            row['result'] = self.client.call(method, params or {}, timeout=30)
            return row['result']
        except BaseException as error:
            row['error'] = repr(error)
            raise

    def state(self):
        return self.rpc('runtime.inspect', dict(session_id=self.session))

    def module(self):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))

    def values(self):
        return self.module()['module']['values']

    def entity(self, identifier):
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=identifier))

    def voices(self):
        value = self.rpc('runtime.audio.voices', dict(session_id=self.session, tick=self.tick, limit=256))
        need(not value['has_more'], 'Voice list truncated')
        return value

    def control(self, key):
        state, gameplay = self.state(), self.module()
        self.last_control = dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=gameplay['revision'], expected_structure_revision=state['structure_revision'],
            id=self.manifest['ui'][key])
        result = self.rpc('runtime.ui.activate', self.last_control)
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def drive(self, x, z, label):
        # Steer exclusively through real controller input in the live yaw basis.
        recent = []
        for _ in range(600):
            entity = self.entity(self.manifest['player'])
            point = position(entity)
            dx, dz = x-point[0], z-point[2]
            distance = math.hypot(dx, dz)
            if distance <= .09:
                self.step()
                return
            matrix = entity['world_matrix']
            strength = min(.8, distance*4)
            right, forward = (matrix[0], matrix[2]), (-matrix[8], -matrix[10])
            need(abs(math.hypot(*right)-1) < 1e-6 and abs(math.hypot(*forward)-1) < 1e-6 and
                 abs(sum(a*b for a, b in zip(right, forward))) < 1e-6,
                 'Player heading basis is not unscaled yaw-only')
            self.step((strength*(dx*right[0]+dz*right[1])/distance,
                       strength*(dx*forward[0]+dz*forward[1])/distance))
            recent.append(position(self.entity(self.manifest['player'])))
            if len(recent) > 60:
                need(math.dist(recent[-1], recent[-60]) > .01, 'Native route stalled: '+label)
        raise AssertionError('Native route budget exhausted: '+label)


class Oracle(BaseOwnership):
    def __init__(self, args, record):
        self.args, self.record = args, record
        self.client = self.host = self.host_log = None
        self.serial = 0
        self.limit = time.monotonic()+1800
        self.player_attached = False
        self.previous_use = False
        self.workspace = args.workspace.resolve(strict=True)
        self.manifest = read(self.workspace/'exercise-manifest.json', 1024*1024)
        m = self.manifest
        required_keys(m, ('format', 'version', 'game', 'project', 'player', 'camera', 'regions',
                          'fields', 'ui', 'save', 'marshal', 'finish_audio'), ('state_fields',), 'Handoff')
        need(m['format'] == 'poima.agent-depot' and type(m['version']) is int and m['version'] == 1,
             'Wrong Depot handoff format')
        exact_keys(m['game'], ('project', 'type', 'identity', 'native_descriptor', 'components_manifest'), 'Game')
        exact_keys(m['regions'], CENTERS, 'Regions')
        exact_keys(m['fields'], ('phase', 'stage', 'cargo', 'elapsed', 'rejected'), 'Fields')
        self.fields = m['fields']
        self.state_fields = m.get('state_fields', {})
        need(isinstance(self.state_fields, dict) and len(self.state_fields) <= 123 and
             all(re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,127}', key) for key in self.state_fields),
             'Bounded named extra mutable state fields required')
        names = [*self.fields.values(), *self.state_fields.values()]
        need(all(isinstance(x, str) and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,127}', x) for x in names) and
             len(set(names)) == len(names), 'Distinct declared complete gameplay field names required')
        self.declared_state = set(names)
        need(isinstance(m['ui'], dict) and 10 <= len(m['ui']) <= 128 and
             {'welcome', 'begin', 'welcome_load', 'hud', 'menu_button', 'menu',
              'resume', 'save', 'load', 'save_status'} <= set(m['ui']) and
             all(re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,127}', key) for key in m['ui']),
             'Required UI IDs plus bounded optional named UI IDs required')
        exact_keys(m['save'], ('checkpoint_slot',), 'Save')
        need(m['save']['checkpoint_slot'] == 'depot-checkpoint', 'Frozen save slot differs')
        exact_keys(m['marshal'], ('root', 'rig', 'asset', 'idle_clip', 'provenance'), 'Marshal')
        exact_keys(m['finish_audio'], ('emitter', 'asset', 'provenance'), 'Finish audio')
        identifiers = [m['player'], m['camera'], m['marshal']['root'], m['marshal']['rig'],
                       m['finish_audio']['emitter'], *m['ui'].values()]
        for value in m['regions'].values():
            exact_keys(value, ('entity',), 'Region')
            identifiers.append(value['entity'])
        need(all(isinstance(x, str) and re.fullmatch('[0-9a-f]{32}', x) and int(x, 16) for x in identifiers),
             'Nonzero canonical entity/UI identifiers required')
        need(len(set(x['entity'] for x in m['regions'].values())) == 6, 'Duplicate region entities')
        for content in (m['marshal'], m['finish_audio']):
            records = content['provenance']
            if isinstance(records, str): records = [records]
            need(isinstance(records, list) and 1 <= len(records) <= 8 and len(set(records)) == len(records),
                 'Provenance must be one record hash or bounded unique record list')
            need(all(isinstance(x, str) and re.fullmatch('[0-9a-f]{64}', x) for x in [content['asset'], *records]),
                 'Cooked asset and provenance hashes required')
        need(type(m['marshal']['idle_clip']) is int and m['marshal']['idle_clip'] >= 0, 'Invalid idle clip')
        self.managed = bool(args.managed_development)
        self.descriptor = self.artifact = None
        if self.managed:
            need(not args.bundle and args.assembly and args.hostfxr and args.bridge,
                 'Partial managed mode requires assembly/hostfxr/bridge and forbids source-free bundle')
            self.assembly = self.local(args.assembly)
            self.bridge = self.local(args.bridge)
            self.hostfxr = args.hostfxr.resolve(strict=True)
            need(self.hostfxr.is_file() and not self.hostfxr.is_symlink() and
                 not getattr(self.hostfxr, 'is_junction', lambda: False)(), 'Regular explicit hostfxr required')
            need(managed_pe(self.assembly) and managed_pe(self.bridge), 'Managed PE assembly and bridge required')
            self.pins_root = self.assembly.parent
            self.pins = inventory(self.pins_root)
            self.bridge_pins = inventory(self.bridge.parent)
            self.hostfxr_pin = sha(self.hostfxr)
            need('Poima.Gameplay.dll' in self.pins and 'Poima.Gameplay.dll' in self.bridge_pins and
                 self.pins['Poima.Gameplay.dll'] == self.bridge_pins['Poima.Gameplay.dll'],
                 'Compiled game and explicit bridge SDK closure differ')
            record.update(managed_inputs=dict(assembly=str(self.assembly), assembly_sha256=sha(self.assembly),
                bridge=str(self.bridge), bridge_inventory=self.bridge_pins,
                hostfxr=str(self.hostfxr), hostfxr_sha256=self.hostfxr_pin),
                native_gate_reason='Explicit managed-only scope; native publication and export are not verified.')
        else:
            need(not any((args.assembly, args.hostfxr, args.bridge)), 'Managed loader arguments require explicit partial mode')
            self.descriptor = self.local(m['game']['native_descriptor'])
            self.artifact = read(self.descriptor, 1024*1024)
            a = self.artifact
            need(a['format'] == 'poima.native-gameplay' and type(a['version']) is int and a['version'] == 2 and
                 a['engine_version'] == args.artifact_engine_version and a['type'] == m['game']['type'] and
                 a['identity'] == m['game']['identity'] == a['schema']['identity'], 'Wrong genuine native artifact')
            need(a['target_os'] == ('Windows' if args.windows_interop or os.name == 'nt' else 'Linux') and
                 a['target_arch'] == 'x86_64', 'Native artifact target differs')
            self.pins_root = self.descriptor.parent
            self.pins = inventory(self.pins_root)
            need(set(self.pins) == {self.descriptor.name, *(x['path'] for x in a['files'])}, 'Native artifact closure differs')
            for item in a['files']:
                need(self.pins[item['path']] == dict(bytes=item['size'], sha256=item['sha256']), 'Native artifact member integrity differs')
            need(sum(x['role'] == 'library' for x in a['files']) == 1, 'Exactly one native library required')
            library = next(x for x in a['files'] if x['role'] == 'library')
            need(not managed_pe(self.descriptor.parent/library['path']), 'Managed PE gameplay cannot qualify Native AOT')
        self.project_path = self.local(m['project'])
        project = read(self.project_path, 1024*1024)
        self.project = project
        need(project['entry']['controller'] == m['player'] and project['entry']['camera'] == m['camera'],
             'Project entry differs from declared player/camera')
        if not self.managed:
            need(sha(self.project_member(project['gameplay']['descriptor'])) == sha(self.descriptor), 'Project selected another native artifact')
        self.local(m['game']['project'])
        if m['game']['components_manifest'] is not None: self.local(m['game']['components_manifest'])
        self.source_world = self.project_member(project['entry']['world'])
        self.source_world_pin = sha(self.source_world)
        self.source_assets_pin = inventory(Path(str(self.source_world)+'.assets'))
        self.bundle = args.bundle.resolve(strict=True) if args.bundle else None
        if self.bundle and self.bundle.is_dir(): self.bundle /= 'game.json'
        if self.bundle:
            spec = read(self.bundle, 4*1024*1024)
            need(args.engine == (self.bundle.parent/spec['engine']['executable']).resolve(strict=True),
                 'Source-free run must use the bundled native engine')
            self.bundle_pin = inventory(self.bundle.parent)
            need(set(self.bundle_pin) == {'game.json', *(x['path'] for x in spec['files'])}, 'Bundle closure differs')
            for item in spec['files']:
                need(self.bundle_pin[item['path']] == dict(bytes=item['size'], sha256=item['sha256']), 'Bundle member integrity differs')
            forbidden = {'hostfxr.dll', 'libhostfxr.so', 'coreclr.dll', 'libcoreclr.so',
                         'poima.gameplay.dll', 'poima.managedbridge.dll'}
            for name in self.bundle_pin:
                member = self.bundle.parent/name
                need(member.name.casefold() not in forbidden and member.suffix.casefold() not in
                     {'.cs', '.csproj', '.sln', '.fbx', '.gltf', '.glb', '.pdb'} and not managed_pe(member),
                     'Source or managed runtime content in native bundle: '+name)
            self.world = self.bundle.parent/'content/world.json'
        else:
            self.world = args.output/'owned-world.json'
            shutil.copy2(self.source_world, self.world)
            shutil.copytree(Path(str(self.source_world)+'.assets'), Path(str(self.world)+'.assets'))
        self.document = read(self.world, 16*1024*1024)
        self.world_pin = sha(self.world)
        self.assets_pin = inventory(Path(str(self.world)+'.assets'))
        if self.bundle:
            source = read(self.source_world, 16*1024*1024)
            for field in ('receipts', 'retired_ids', 'retired_component_schemas', 'retired_template_ids', 'retired_ui_ids'):
                if field in source: source[field] = []
            need(canonical(source) == canonical(self.document), 'Bundle differs from documented exported authoring state')
        else:
            need(self.world_pin == self.source_world_pin, 'Copied authored world differs from handoff source')
        self.storage = args.output/'saves'
        self.storage.mkdir()
        record.update(manifest=m, artifact=self.artifact, oracle_sha256=sha(__file__), engine_sha256=sha(args.engine), artifact_inventory=self.pins,
                      artifact_scope='managed_development_partial' if self.managed else 'native_aot',
                      frozen_geometry=CENTERS, frozen_radius=RADIUS)

    def local(self, name):
        need(isinstance(name, str) and name and '\\' not in name and ':' not in name and not Path(name).is_absolute(),
             'Workspace-relative portable handoff path required')
        need(all(x not in ('.', '..', '') for x in name.split('/')), 'Handoff traversal/ambiguous path')
        candidate = self.workspace/name
        walk = self.workspace
        for part in Path(name).parts:
            walk /= part
            need(not walk.is_symlink() and not getattr(walk, 'is_junction', lambda: False)(), 'Linked handoff path')
        candidate = candidate.resolve(strict=True)
        need(candidate.is_file() and candidate.is_relative_to(self.workspace), 'Handoff path escapes workspace')
        return candidate

    def project_member(self, name):
        need(isinstance(name, str) and name and '\\' not in name and ':' not in name and
             not Path(name).is_absolute() and all(x not in ('', '.', '..') for x in name.split('/')),
             'Project content path must be portable and relative to the project directory')
        relative = (self.project_path.parent/Path(name)).relative_to(self.workspace)
        return self.local(relative.as_posix())

    def rpc(self, method, params=None):
        need(len(self.record['calls']) < 50000, 'Native oracle RPC budget exceeded')
        return super().rpc(method, params)

    def numbers(self, values=None):
        values = self.values() if values is None else values
        result = {}
        for key, field in self.fields.items():
            value = values[field]
            need(type(value) is int or (isinstance(value, str) and re.fullmatch('-?[0-9]+', value)),
                 'Expected native integer gameplay field: '+key)
            result[key] = int(value)
        need(result['phase'] in (0, 1, 2) and result['stage'] in range(5) and result['cargo'] == CARGO[result['stage']] and
             result['elapsed'] >= 0 and result['rejected'] >= 0, 'Invalid phase/stage/cargo/timer domain')
        return result

    def ui(self):
        value = self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick, limit=256))
        need(not value.get('has_more', False) and value.get('next_after') is None, 'UI inspection truncated')
        rows = {x['id']: x for x in value['elements']}
        need(all(x in rows for x in self.manifest['ui'].values()), 'Declared UI element missing')
        return value, rows

    def menu_open(self):
        return self.ui()[1][self.manifest['ui']['menu']]['effective_visible']

    def start(self, label):
        self.close()
        self.serial += 1
        self.previous_use = False
        if self.bundle:
            self.endpoint = 'depot_'+fresh()
            self.host_log = (self.args.output/(label+'-host.log')).open('wb')
            self.host = subprocess.Popen([str(self.args.engine), 'game', 'serve', self.native(self.bundle),
                '--endpoint', self.endpoint, '--save-root', self.native(self.storage)], stdin=subprocess.DEVNULL,
                stdout=self.host_log, stderr=self.host_log, cwd=self.args.output)
            self.client = WorldClient.connect(str(self.args.engine), self.endpoint, cwd=self.args.output)
            status = self.rpc('runtime.status')
            self.session, self.tick = status['session_id'], status['tick']
        else:
            if label == 'paused-begin':
                # Live-player mutations require the shared owner's event pump,
                # whereas ordinary world stdio suffices for headless semantics.
                self.endpoint = 'depot_'+fresh()
                self.host_log = (self.args.output/(label+'-host.log')).open('wb')
                self.host = subprocess.Popen([str(self.args.engine), 'serve', self.native(self.world),
                    '--endpoint', self.endpoint], stdin=subprocess.DEVNULL,
                    stdout=self.host_log, stderr=self.host_log, cwd=self.args.output)
                self.client = WorldClient.connect(str(self.args.engine), self.endpoint, cwd=self.args.output)
            else:
                self.client = WorldClient.open(str(self.args.engine), self.native(self.world), cwd=self.args.output)
            self.session, self.tick = fresh(), 0
            self.rpc('runtime.start', dict(session_id=self.session, revision=self.rpc('world.inspect')['revision']))
            parameters = dict(session_id=self.session, request_id=fresh(), expected_tick=0,
                expected_revision=self.module()['revision'], values=self.project['gameplay'].get('values', {}))
            if self.managed:
                parameters.update(hostfxr=self.native(self.hostfxr), bridge=self.native(self.bridge),
                                  assembly=self.native(self.assembly), type=self.manifest['game']['type'])
                self.rpc('runtime.gameplay.load', parameters)
            else:
                parameters.update(descriptor=self.native(self.descriptor), expected_descriptor_sha256=sha(self.descriptor))
                self.rpc('runtime.gameplay.load_native', parameters)
            self.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=self.native(self.storage)))
        module = self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))['module']
        need(module['type'] == self.manifest['game']['type'] and
             module['schema']['identity'] == self.manifest['game']['identity'], 'Actual game type/identity differs')
        schema_names = [x['name'] for x in module['schema']['fields']]
        need(len(schema_names) == len(set(schema_names)) and set(schema_names) == self.declared_state == set(module['values']),
             'Declared state does not cover every actual scalar schema/value field')
        kinds = {x['name']: x['kind'] for x in module['schema']['fields']}
        need(all(kinds[name] in ('int32', 'int64') for name in self.fields.values()), 'Objective fields must be native integers')
        if self.managed:
            need(module['backend'] == 'coreclr' and module['assembly_sha256'] == sha(self.assembly),
                 'Actual partial development module/assembly differs')
        else:
            library = next(x for x in self.artifact['files'] if x['role'] == 'library')
            need(module['backend'] == 'native_aot' and canonical(module['schema']) == canonical(self.artifact['schema']) and
                 module['assembly_sha256'] == library['sha256'] and
                 module['native_diagnostics'] == dict(dynamic_code_compiled=False, dynamic_code_supported=False),
                 'Actual native module differs')
        need(self.numbers() == dict(phase=0, stage=0, cargo=0, elapsed=0, rejected=0), 'Fresh game objective is not Ready')
        for name, center in CENTERS.items():
            identifier = self.manifest['regions'][name]['entity']
            need(max(abs(a-b) for a,b in zip(position(self.entity(identifier)), center)) < 1e-9, 'Frozen native region differs: '+name)
        player = self.entity(self.manifest['player'])
        authored_player = self.document['entities'][self.manifest['player']]
        need(authored_player['parent'] is None and authored_player['components']['Transform']['scale'] == [1, 1, 1] and
             player['is_character'] and player['has_body'] and math.dist(position(player), CENTERS['start']) < 1e-6,
             'Fresh unparented native physics character differs')
        camera = self.document['entities'][self.manifest['camera']]
        need(camera['parent'] == self.manifest['player'] and 'Camera' in camera['components'], 'Camera is not a direct child')
        current = self.manifest['marshal']['rig']
        while current != self.manifest['marshal']['root']:
            current = self.document['entities'][current]['parent']
            need(current is not None, 'Declared marshal rig is outside its root')
        need(current in self.document['entities'], 'Declared marshal root does not exist')
        rig = self.document['entities'][self.manifest['marshal']['rig']]['components']['AnimationRig']
        need(rig['asset'] == self.manifest['marshal']['asset'] and rig['clip'] == self.manifest['marshal']['idle_clip'] and
             rig['playing'] and rig['loop'] and self.entity(self.manifest['marshal']['rig'])['animation']['clip'] == rig['clip'],
             'Imported idle rig binding/playback differs')
        need(any(x['components'].get('SkinnedMesh', {}).get('rig') == self.manifest['marshal']['rig']
                 for x in self.document['entities'].values()), 'Marshal has no actual weighted mesh')
        need(self.document['entities'][self.manifest['finish_audio']['emitter']]['components']['AudioEmitter']['asset'] ==
             self.manifest['finish_audio']['asset'], 'Permanent finish emitter asset differs')
        provenance = []
        for content in (self.manifest['marshal'], self.manifest['finish_audio']):
            expected = content['provenance']
            if isinstance(expected, str): expected = [expected]
            need(sorted(self.document['asset_provenance'][content['asset']]) == sorted(expected), 'Selected provenance differs')
            for identifier in expected:
                item = self.rpc('asset.provenance.inspect', dict(record=identifier))
                need(item['metadata']['asset'] == content['asset'] and item['metadata']['license']['identifier'] == 'CC0-1.0',
                     'Approved supplied CC0 asset record differs')
                provenance.append(item)
        _, rows = self.ui()
        need(rows[self.manifest['ui']['welcome']]['effective_visible'] and rows[self.manifest['ui']['begin']]['eligible'] and
             rows[self.manifest['ui']['welcome_load']]['eligible'], 'Initial compiled welcome actions are not eligible')
        self.record.setdefault('runs', []).append(dict(label=label, initial_session=self.session, module=module, provenance=provenance))

    def step(self, move=(0, 0), look=(0, 0), use=False):
        before = self.numbers()
        point = position(self.entity(self.manifest['player']))
        menu = self.menu_open()
        expected = dict(before)
        if before['phase'] == 1 and not menu:
            expected['elapsed'] += 1
            if use and not self.previous_use:
                if inside(point, CENTERS[ORDER[before['stage']]]):
                    if before['stage'] == 4: expected['phase'] = 2
                    else:
                        expected['stage'] += 1
                        expected['cargo'] = CARGO[expected['stage']]
                else: expected['rejected'] += 1
        state = self.state()
        parameters = dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
            expected_structure_revision=state['structure_revision'], ticks=1,
            inputs=[dict(entity=self.manifest['player'], move=list(move), look=list(look), use=bool(use))])
        result = self.rpc('runtime.step', parameters)
        need(result['stepped'] == 1 and result['current_tick'] == self.tick+1 and
             result['current_session_id'] == self.session and not result['runtime_replaced'], 'Unexpected native advance or replacement')
        self.tick = result['current_tick']
        self.last_step, self.last_step_result = parameters, result
        self.previous_use = bool(use)
        after = self.numbers()
        need(after == expected, 'Independent prephysics objective oracle differs: '+repr(dict(before=before, expected=expected, after=after, position=point, use=use, menu=menu)))
        self.record.setdefault('transitions', []).append(dict(tick=self.tick, before=before, after=after,
            position=point, menu=menu, move=list(move), look=list(look), use=bool(use)))
        return after

    def drive(self, x, z, label):
        initial = position(self.entity(self.manifest['player']))
        super().drive(x, z, label)
        final = position(self.entity(self.manifest['player']))
        need(math.hypot(final[0]-x, final[2]-z) <= .09 and
             (math.hypot(initial[0]-x, initial[2]-z) <= .09 or math.dist(initial, final) > .01), 'Route did not physically move to target')
        self.record.setdefault('routes', []).append(dict(label=label, initial=initial, final=final, target=[x, z]))

    def use_at(self, name, idle_ticks=3):
        center = CENTERS[name]
        self.drive(center[0], center[2], name)
        self.step(use=False)
        self.step(use=True)
        for _ in range(idle_ticks): self.step(use=False)
        self.step(use=False)

    def batch_outside_press(self):
        # Native semantic Use is a press pulse. For a multi-tick request native
        # applies it only on the first Tick; this is not physical held input.
        self.step(use=False)
        before = self.numbers()
        point = position(self.entity(self.manifest['player']))
        need(before['phase'] == 1 and not self.menu_open() and
             not any(inside(point, center) for center in CENTERS.values()), 'Batch negative probe must be outside all regions')
        state = self.state()
        parameters = dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
            expected_structure_revision=state['structure_revision'], ticks=3,
            inputs=[dict(entity=self.manifest['player'], move=[0, 0], look=[0, 0], use=True)])
        result = self.rpc('runtime.step', parameters)
        need(result['stepped'] == 3 and result['current_tick'] == self.tick+3 and
             result['current_session_id'] == self.session and not result['runtime_replaced'], 'Batch press advanced/replaced unexpectedly')
        self.tick = result['current_tick']
        self.previous_use = False
        expected = dict(before, elapsed=before['elapsed']+3, rejected=before['rejected']+1)
        need(self.numbers() == expected, 'One native batch press produced repeated rejection or wrong timer')
        self.record['batch_press'] = dict(parameters=parameters, result=result, before=before, after=expected,
                                         input_scope='one semantic first-Tick pulse, not physical held input')
        self.step(use=False)

    def begin(self):
        previous_tick = self.tick
        self.control('begin')
        need(self.tick == previous_tick and self.numbers() == dict(phase=1, stage=0, cargo=0, elapsed=0, rejected=0),
             'Compiled Begin did not enter Running at unchanged time')
        _, rows = self.ui()
        need(not rows[self.manifest['ui']['welcome']]['effective_visible'] and rows[self.manifest['ui']['menu_button']]['eligible'],
             'Begin did not release welcome/modal input')

    def snapshot(self):
        # Native module state, all authored entities/components, UI/revisions,
        # complete native audio allocator/voices: only runtime session IDs differ.
        state = self.state()
        need(state['entities'] == len(self.document['entities']), 'This stable-membership gate cannot omit runtime-spawned entities')
        module = self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))
        entities = {identifier: self.entity(identifier) for identifier in self.document['entities']}
        components = {identifier: {kind: self.rpc('runtime.component.get', dict(session_id=self.session,
            tick=self.tick, id=identifier, type=kind[5:])) for kind in entity['components'] if kind.startswith('game:')}
            for identifier, entity in self.document['entities'].items()}
        return normalize(dict(tick=self.tick, entities=entities, components=components,
            values=module['module']['values'], schema=module['module']['schema'], gameplay_revision=module['revision'],
            revisions={key: state[key] for key in ('structure_revision', 'ui_revision', 'control_sequence')},
            ui=self.ui()[0], voices=self.voices()))

    def assert_snapshot(self, expected, label):
        actual = self.snapshot()
        need(canonical(actual) == canonical(expected), label+' differs before another callback')
        return actual

    def checkpoint(self):
        need(self.numbers()['stage'] == 3 and self.numbers()['cargo'] == 2 and
             not any(inside(position(self.entity(self.manifest['player'])), c) for c in CENTERS.values()),
             'Partial checkpoint must carry B off every interaction region')
        self.control('menu_button')
        need(self.menu_open(), 'Compiled Menu did not open')
        original = self.numbers()
        for _ in range(3):
            self.step(use=True)
            self.step(use=False)
        self.step(use=False)
        need(self.numbers() == original, 'Menu allowed objective/timer changes')
        saved_result = self.control('save')
        save_request = dict(self.last_control)
        need(saved_result['save_serviced'] and not saved_result['runtime_replaced'], 'Compiled save not serviced')
        directory = self.storage/'slot-depot-checkpoint'
        manifest = read(directory/'current.json', 1024*1024)
        current = manifest['payload']['current']
        payload = (directory/current['file']).resolve(strict=True)
        need(payload.parent == directory.resolve() and payload.stat().st_size == current['bytes'] and sha(payload) == current['sha256'],
             'Durable native payload integrity differs')
        saved, durable = self.snapshot(), inventory(self.storage)
        need(self.rpc('runtime.ui.activate', save_request) == dict(saved_result, replayed=True), 'Save retry receipt differs')
        self.assert_snapshot(saved, 'Save retry state')
        need(inventory(self.storage) == durable, 'Save retry rewrote durable files')
        self.record['partial'] = dict(snapshot=saved, generation=current['generation'], manifest_sha256=sha(directory/'current.json'),
            payload_sha256=sha(payload), payload_bytes=payload.stat().st_size, durable_inventory=durable, exact_save_retry=True)
        old_session = self.session
        self.control('resume')
        self.drive(3, -5, 'advance-after-save')
        self.step(use=True)
        self.step(use=False)
        need(self.numbers()['stage'] == 4, 'Post-save progress did not visibly diverge')
        self.control('menu_button')
        loaded = self.control('load')
        need(loaded['save_serviced'] and loaded['runtime_replaced'] and self.session != old_session, 'Same-owner compiled Load did not replace')
        self.assert_snapshot(saved, 'Same-owner exact restore')
        need(inventory(self.storage) == durable, 'Load modified checkpoint store')
        self.previous_use = False
        self.record['same_owner_exact_restore'] = dict(old_session=old_session, new_session=self.session, receipt=loaded)
        return saved, durable

    def complete(self, label):
        self.control('resume')
        self.step(use=False)
        self.use_at('delivery_b')
        need(self.numbers()['stage'] == 4 and self.numbers()['cargo'] == 0, 'Final real delivery absent')
        self.drive(0, -8, label+'-finish')
        self.step(use=False)
        prior = self.voices()
        winning_tick = self.tick
        self.step(use=True)
        need(self.numbers()['phase'] == 2, 'Ordered actual finish did not win')
        voices = self.voices()
        emitted = [v for v in voices['voices'] if int(v['voice']) >= int(prior['next_voice']) and
                   v['emitter'] == self.manifest['finish_audio']['emitter']]
        need(len(emitted) == 1 and emitted[0]['asset'] == self.manifest['finish_audio']['asset'] and
             emitted[0]['start_tick'] == winning_tick and emitted[0]['stop_sample'] is None and not emitted[0]['loop'],
             'Winning Use did not allocate exactly one original native completion cue')
        allocated, terminal = voices['next_voice'], self.numbers()
        for _ in range(5): self.step(use=False)
        self.step(use=False)
        self.step(use=True)
        for _ in range(20): self.step(use=False)
        need(self.numbers() == terminal and self.voices()['next_voice'] == allocated, 'Terminal timer/objective/cue allocation repeated')
        self.record[label] = dict(snapshot=self.snapshot(), winning_tick=winning_tick, cue=emitted[0], terminal_ticks=27)

    def stop_player(self):
        if not self.player_attached or self.client is None: return
        live = self.rpc('player.inspect')
        if live['active']:
            self.rpc('player.control', dict(player_id=live['player_id'], request_id=fresh(),
                expected_control_revision=live['control_revision'], action='stop'))
            deadline = time.monotonic()+30
            while time.monotonic() < deadline:
                live = self.rpc('player.inspect')
                if not live['active']: break
                time.sleep(.01)
            need(not live['active'], 'Native player stop timed out')
        self.player_attached = False
        need(not live.get('error') and isinstance(live.get('report'), dict) and
             live['report'].get('nvrhi_errors') == 0, 'Native player terminal error or missing accepted-player report')
        self.record['paused_begin_player_terminal'] = live

    def close(self):
        errors = []
        try: self.stop_player()
        except BaseException: errors.append(traceback.format_exc())
        # Shared world serve owners are shut down as well as game serve owners. Reap the
        # host/log even when endpoint connection or native player launch fails.
        client, self.client = self.client, None
        host, self.host = self.host, None
        log, self.host_log = self.host_log, None
        if client is not None:
            try:
                state = client.call('runtime.status', timeout=10)
                if state['active']: client.call('runtime.stop', dict(session_id=state['session_id']), timeout=10)
                if host is not None: client.call('host.shutdown', timeout=10)
            except BaseException: errors.append(traceback.format_exc())
            try: client.close()
            except BaseException: errors.append(traceback.format_exc())
            transport = client.transport
            if transport.returncode != 0: errors.append('Client exit was not zero')
            if transport.stderr_tail or transport.stderr_truncated:
                errors.append('Client stderr was nonempty or truncated')
            self.record.setdefault('owners', []).append(dict(pid=transport.process_id,
                exit_code=transport.returncode, stderr=transport.stderr_tail,
                stderr_truncated=transport.stderr_truncated, cleanup_errors=list(errors)))
        if host is not None:
            forced = False
            try: host.wait(timeout=20)
            except subprocess.TimeoutExpired:
                forced = True
                errors.append('Forced host cleanup')
                try:
                    host.terminate()
                    try: host.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        host.kill()
                        host.wait(timeout=5)
                except BaseException: errors.append(traceback.format_exc())
            except BaseException: errors.append(traceback.format_exc())
            if host.returncode != 0: errors.append('Host exit was not zero')
            self.record.setdefault('hosts', []).append(dict(pid=host.pid, exit_code=host.returncode,
                forced_cleanup=forced, cleanup_errors=list(errors)))
        if log is not None:
            try:
                log.close()
                self.record.setdefault('host_logs', []).append(dict(path=str(log.name), sha256=sha(log.name)))
            except BaseException: errors.append(traceback.format_exc())
        if errors:
            self.record.setdefault('cleanup_failures', []).extend(errors)
            raise AssertionError('Owned Depot cleanup failed')

    def paused_begin(self):
        self.start('paused-begin')
        result = self.rpc('player.start', dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
            expected_generation=0, camera=self.manifest['camera'], controller=self.manifest['player'], mode='interactive',
            initially_paused=True, gamepad=dict(mode='disabled'), audio=False, width=960, height=540,
            gpu=self.args.gpu, samples=4, frames_in_flight=1))
        self.player_attached = True
        deadline = time.monotonic()+30
        while time.monotonic() < deadline:
            live = self.rpc('player.inspect')
            need(live['active'], 'Paused native player stopped before ready')
            if live['ready']: break
            time.sleep(.01)
        need(live['ready'] and live['paused'] and live['report']['hardware'] and live['report']['nvrhi_errors'] == 0,
             'Actual initially paused native player was not ready')
        before = self.rpc('runtime.status')['tick']
        time.sleep(.1)
        need(self.rpc('runtime.status')['tick'] == before, 'Initially paused owner advanced before Begin')
        control = self.control('begin')
        need(control['intent'] == 1, 'Compiled Begin did not request native Resume')
        # No explicit step or host/player resume is issued: only compiled Begin.
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            live = self.rpc('player.inspect')
            state = self.rpc('runtime.status')
            if not live['paused'] and state['tick'] > before: break
            need(live['active'], 'Begin stopped native player')
            time.sleep(.01)
        need(not live['paused'] and state['tick'] > before, 'Compiled Begin left actual paused owner stuck')
        running_observation = dict(player=live, status=state)
        # Once Begin itself is proved, pause through the separate native player
        # guard before reading tick-guarded UI/gameplay. This pause cannot make
        # Begin succeed and is not a claim that a racing UI query is atomic.
        self.rpc('player.control', dict(player_id=live['player_id'], request_id=fresh(),
            expected_control_revision=live['control_revision'], action='pause'))
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            live = self.rpc('player.inspect')
            if live['paused']: break
            time.sleep(.01)
        need(live['paused'], 'Post-proof native player pause failed')
        state = self.rpc('runtime.status')
        self.session, self.tick = state['session_id'], state['tick']
        menu_control = self.control('menu_button')
        need(menu_control['intent'] == 2 and self.menu_open(), 'Compiled Menu did not request Pause/open its logical modal')
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            live = self.rpc('player.inspect')
            if live['paused']: break
            time.sleep(.01)
        need(live['paused'], 'Already paused native player did not remain paused after compiled Menu')
        status = self.rpc('runtime.status')
        self.session, self.tick = status['session_id'], status['tick']
        need(self.numbers()['phase'] == 1 and self.numbers()['stage'] == 0, 'Paused Begin objective differs')
        frozen = self.numbers()
        time.sleep(.1)
        need(self.rpc('runtime.status')['tick'] == self.tick and self.numbers() == frozen,
             'Native owner/objective advanced while its compiled menu was open and player paused')
        self.record['paused_begin'] = dict(passed=True, launch=result, compiled_begin=control,
            before_tick=before, after_tick=self.tick, actual_player=live, explicit_resume_or_step=False)
        self.record['paused_begin'].update(running_observation=running_observation, host_pause_after_proof=True,
            compiled_menu=menu_control, running_menu_to_paused_qualified=False,
            menu_scope='RequestPause and menu/modal freeze from an already explicitly paused player; running external runtime.ui.activate is prohibited.')
        self.stop_player()
        self.close()

    def verify(self):
        self.start('negative')
        self.step()
        frozen = self.snapshot()
        need(self.rpc('runtime.step', self.last_step) == dict(self.last_step_result, replayed=True), 'Exact input retry differs')
        self.assert_snapshot(frozen, 'Exact input retry')
        self.step(use=True)
        for _ in range(2): self.step(use=False)
        self.step(use=False)
        need(self.numbers() == dict(phase=0, stage=0, cargo=0, elapsed=0, rejected=0), 'Ready accepted input or advanced objective timer')
        self.control('welcome_load')
        need(self.numbers()['phase'] == 0 and not list(self.storage.iterdir()), 'Missing checkpoint Load changed objective or wrote storage')
        self.begin()
        for name in ('delivery_a', 'pickup_b', 'finish'):
            before = self.numbers()['rejected']
            self.use_at(name)
            need(self.numbers()['stage'] == 0 and self.numbers()['rejected'] == before+1, 'Negative in-region input was not rejected once')
        self.drive(0, -2.5, 'outside-region')
        self.batch_outside_press()
        need(self.numbers()['stage'] == 0 and self.numbers()['rejected'] == 4, 'Outside-range batch press rejected incorrectly')
        need(not any(v['emitter'] == self.manifest['finish_audio']['emitter'] for v in self.voices()['voices']), 'Early finish played victory cue')
        self.record['negative'] = dict(snapshot=self.snapshot(), cases=4)
        self.start('positive')
        self.begin()
        self.use_at('pickup_a')
        self.use_at('delivery_a')
        self.use_at('pickup_b')
        self.drive(0, -2.5, 'off-region-partial')
        saved, durable = self.checkpoint()
        self.complete('same_owner_completion')
        self.close()
        self.start('fresh-checkpoint')
        initialized = self.session
        loaded = self.control('welcome_load')
        need(loaded['save_serviced'] and loaded['runtime_replaced'] and self.session != initialized, 'Fresh compiled Welcome Load did not replace runtime')
        self.assert_snapshot(saved, 'Fresh-process exact restore')
        need(inventory(self.storage) == durable, 'Fresh restore modified durable files')
        self.previous_use = False
        self.record['fresh_process_exact_restore'] = dict(initial_session=initialized, restored_session=self.session, receipt=loaded)
        self.complete('fresh_process_completion')
        self.close()
        need(sha(self.source_world) == self.source_world_pin and inventory(Path(str(self.source_world)+'.assets')) == self.source_assets_pin and
             sha(self.world) == self.world_pin and
             inventory(Path(str(self.world)+'.assets')) == self.assets_pin and inventory(self.pins_root) == self.pins,
             'Immutable authored/native/content input changed')
        if self.managed:
            need(inventory(self.bridge.parent) == self.bridge_pins and sha(self.hostfxr) == self.hostfxr_pin,
                 'Explicit managed loader/bridge input changed')
        need(inventory(self.storage) == durable, 'Continuation replayed a pending save/load as a fresh write')
        if self.bundle: need(inventory(self.bundle.parent) == self.bundle_pin, 'Immutable source-free bundle changed')
        self.record.update(same_owner_complete=True, fresh_process_complete=True,
            headless_subgates=dict(passed=True, scope='managed_development_partial' if self.managed else 'native_aot',
                checks=['ordered progress', 'genuine movement', 'negative uses', 'terminal cue stability',
                        'complete immediate same-owner restore', 'complete immediate fresh-owner restore',
                        'saved continuation', 'immutable gameplay/content/storage inputs']))
        if self.args.paused_begin: self.paused_begin()
        self.record.update(passed=True, same_owner_complete=True, fresh_process_complete=True,
            source_free_bundle_scope=bool(self.bundle), paused_begin_scope=bool(self.args.paused_begin),
            managed_gameplay_qualified=self.managed, native_gate_passed=not self.managed,
            export_gate_passed=bool(self.bundle) and not self.managed, workflow_completed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'workspace', 'output'): parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--managed-development', action='store_true',
                        help='Explicit partial-only gameplay/save scope; never verifies native publication or export')
    parser.add_argument('--assembly', help='Workspace-relative existing managed assembly (no compilation)')
    parser.add_argument('--bridge', help='Workspace-relative existing matched bridge DLL')
    parser.add_argument('--hostfxr', type=Path, help='Existing native hostfxr file for explicit managed-development scope')
    parser.add_argument('--artifact-engine-version', default='0.0.91')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--paused-begin', action='store_true')
    parser.add_argument('--gpu', type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.workspace = args.workspace.resolve(strict=True)
    args.engine = args.engine.resolve(strict=True)
    need(not args.output.exists() and not args.output.is_relative_to(args.workspace), 'Fresh output outside workspace required')
    if args.bundle:
        bundle = args.bundle.resolve(strict=True)
        root = bundle if bundle.is_dir() else bundle.parent
        need(not args.output.is_relative_to(root) and not root.is_relative_to(args.output), 'Output must be outside immutable bundle')
    need(not sys.flags.optimize, 'Qualification must run without Python optimization')
    args.output.mkdir(parents=True)
    record = dict(passed=False, calls=[], oracle_sha256=sha(__file__), qualification_scope='managed_development_partial' if args.managed_development else 'native_aot',
        native_gate_passed=False, export_gate_passed=False, workflow_completed=False, managed_gameplay_qualified=False, limitations=[
        'Native semantic inputs and compiled UI callbacks; no physical input, audible listening or performance benchmark.',
        'Headless oracle does not establish paused Begin; the optional real native player probe reports its own scope.',
        'Use is a native semantic press pulse; neutral separators and first-Tick batching do not establish physical held-input behavior.',
        'This bounded external-game oracle is not a cross-engine or population-level agent success-rate benchmark.'])
    if args.managed_development:
        record['limitations'].append('Explicit CoreCLR gameplay/save qualification only: native publication, source-free export and external workflow completion are not established.')
    game = None
    try:
        game = Oracle(args, record)
        game.verify()
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        if game:
            try: game.close()
            except BaseException: record['cleanup_error'] = traceback.format_exc()
        if record.get('cleanup_error') or record.get('cleanup_failures'):
            record.update(passed=False, native_gate_passed=False, export_gate_passed=False, managed_gameplay_qualified=False)
        record['rpc_count'] = len(record['calls'])
        encoded = (json.dumps(record, indent=2, allow_nan=False)+'\n').encode('utf-8')
        need(len(encoded) <= 128*1024*1024, 'Retained oracle evidence exceeds 128 MiB')
        (args.output/'evidence.json').write_bytes(encoded)
        print(json.dumps({key: record.get(key) for key in ('passed', 'rpc_count', 'error', 'cleanup_error',
            'same_owner_complete', 'fresh_process_complete', 'source_free_bundle_scope', 'paused_begin_scope',
            'qualification_scope', 'native_gate_passed', 'export_gate_passed', 'workflow_completed',
            'managed_gameplay_qualified')}), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
