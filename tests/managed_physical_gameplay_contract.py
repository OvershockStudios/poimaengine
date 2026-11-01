#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prebuilt CoreCLR/NativeAOT physical-rig consumer: callbacks, rollback and cold saves.

Builds nothing and opens no renderer/audio device. An external process-tree owner
must bound this finite run. Literal requests/replies, complete typed observations,
complete manifests/receipts/archives and all original failures are retained.
Source alone is not execution evidence or a fresh-agent qualification.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import time
import uuid

import world_physical_contract as fixture

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT/'tests/managed_physical_gameplay'
GAME = 'Poima.Tests.PhysicalGame'
IDENTITY = 'poima.test.physical-rig'
SECONDS, RPC_LIMIT, EVIDENCE_LIMIT = 180, 600, 32*1024*1024
FIELDS = {
    **dict.fromkeys(('Rig', 'RootNode', 'TipNode'), 'entity'),
    **dict.fromkeys(('Mode', 'Ticks', 'ObservedMode', 'BodyCount', 'JointCount', 'Writes',
                     'CommittedReadChecks', 'RootActive', 'TipActive'), 'int32'),
    **dict.fromkeys(('RootBody', 'TipBody', 'FailAt', 'ObservedTick'), 'int64'),
    **dict.fromkeys(('RootX', 'RootY', 'RootZ', 'TipX', 'TipY', 'TipZ',
                     'RootVx', 'RootVy', 'RootVz', 'TipVx', 'TipVy', 'TipVz',
                     'RootMass', 'TipMass', 'ImpulseX', 'ImpulseY', 'ImpulseZ'), 'float64'),
}
need, exact = fixture.need, fixture.exact


def fresh():
    return uuid.uuid4().hex


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def strict_json(raw):
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            need(key not in result, 'Duplicate artifact JSON field: '+key)
            result[key] = value
        return result
    def bad_constant(value):
        raise AssertionError('Nonfinite JSON constant: '+value)
    return json.loads(raw, object_pairs_hook=object_pairs, parse_constant=bad_constant)



def image_os(path, library=False):
    """Read actual x64 ELF/PE headers; filenames and declared targets are not proof."""
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        need(len(header) == 64, 'Native image header is truncated')
        if header[:4] == b'\x7fELF':
            need(header[4:6] == b'\x02\x01' and int.from_bytes(header[18:20], 'little') == 62 and
                 int.from_bytes(header[16:18], 'little') in ((3,) if library else (2, 3)), 'Expected x64 little-endian ELF image')
            return 'Linux'
        need(header[:2] == b'MZ', 'Native image is neither ELF nor PE')
        offset = int.from_bytes(header[60:64], 'little')
        need(64 <= offset <= Path(path).stat().st_size-26, 'Invalid PE header offset')
        stream.seek(offset); pe = stream.read(26)
        need(pe[:4] == b'PE\0\0' and int.from_bytes(pe[4:6], 'little') == 0x8664 and
             int.from_bytes(pe[24:26], 'little') == 0x20b and
             (not library or int.from_bytes(pe[22:24], 'little') & 0x2000), 'Expected native x64 PE32+ image')
        return 'Windows'


def native_artifact(descriptor, binary):
    """Independent bounded descriptor/inventory admission, before any native load."""
    descriptor = Path(descriptor).absolute()
    need(descriptor.is_file() and not descriptor.is_symlink() and not getattr(descriptor, 'is_junction', lambda: False)() and
         descriptor.stat().st_size <= 1024*1024, 'Descriptor must be a bounded regular file')
    descriptor = descriptor.resolve(strict=True); root = descriptor.parent
    spec = strict_json(descriptor.read_bytes())
    fields = {'format', 'version', 'engine_version', 'target_os', 'target_arch', 'call_version', 'call_bytes',
              'services_version', 'minimum_services_bytes', 'required_features', 'entry', 'library', 'identity', 'type', 'schema', 'files'}
    need(type(spec) is dict and set(spec) == fields, 'Complete native descriptor field membership differs')
    need(spec['format'] == 'poima.native-gameplay' and type(spec['version']) is int and spec['version'] == 2 and
         type(spec['engine_version']) is str and 1 <= len(spec['engine_version']) <= 128 and
         re.fullmatch(r'\d+\.\d+\.\d+', spec['engine_version']) and
         spec['target_arch'] == 'x86_64' and spec['target_os'] == image_os(binary) and
         spec['identity'] == IDENTITY and spec['type'] == GAME and spec['entry'] == 'poima_gameplay_entry', 'Native artifact identity/host/format differs')
    for key, expected in [('call_version', 1), ('call_bytes', 80), ('services_version', 7), ('minimum_services_bytes', 424)]:
        need(type(spec[key]) is int and spec[key] == expected, 'Native artifact ABI extent/epoch differs: '+key)
    need(exact(spec['required_features'], ['baseline_v7', 'physical_rig_v1']), 'Physical-only consumer must request exactly baseline + physical rights')
    schema = spec['schema']
    need(type(schema) is dict and set(schema) in ({'identity', 'bytes', 'fields'}, {'identity', 'bytes', 'fields', 'components'}) and
         schema['identity'] == IDENTITY and type(schema['bytes']) is int and
         ('components' not in schema or exact(schema['components'], [])), 'Native schema has unexpected state/component membership')
    # Independent CLR sequential/pack8 layout from the original declaration.
    offset, expected_layout = 0, {}
    for name, kind in FIELDS.items():
        size = 4 if kind == 'int32' else 16 if kind == 'entity' else 8
        alignment = min(size, 8); offset = (offset+alignment-1)//alignment*alignment
        expected_layout[name] = dict(name=name, kind=kind, offset=offset, bytes=size); offset += size
    state_bytes = (offset+7)//8*8
    need(schema['bytes'] == state_bytes == 256 and type(schema['fields']) is list and len(schema['fields']) == 33 and
         [row.get('name') for row in schema['fields'] if type(row) is dict] == sorted(FIELDS), 'Native complete state size/order differs')
    for row in schema['fields']:
        need(type(row) is dict and set(row) == {'name', 'kind', 'offset', 'bytes'} and
             type(row['offset']) is int and type(row['bytes']) is int and exact(row, expected_layout[row['name']]),
             'Native typed field layout differs from original sequential declaration')

    def relative(value):
        need(type(value) is str and 1 <= len(value) <= 1024 and not value.startswith('/') and not value.endswith('/'), 'Native inventory path must be relative/bounded')
        for part in value.split('/'):
            stem = part.split('.')[0].lower()
            need(1 <= len(part) <= 128 and part not in ('.', '..') and not part.endswith(('.', ' ')) and
                 re.fullmatch('[A-Za-z0-9_. -]+', part) is not None and stem not in ('con', 'prn', 'aux', 'nul') and
                 not re.fullmatch(r'(?:com|lpt)[1-9]', stem), 'Invalid/reserved native inventory path component')
        return value

    rows = spec['files']; need(type(rows) is list and 1 <= len(rows) <= 256, 'Native payload inventory count differs')
    expected = {relative(descriptor.name)}; folded = {descriptor.name.lower()}; pins = {str(descriptor): sha(descriptor)}
    audit, libraries, total = [], [], 0
    for row in rows:
        need(type(row) is dict and set(row) == {'path', 'size', 'sha256', 'role'}, 'Complete native payload row differs')
        name = relative(row['path']); path = root
        need(name.lower() not in folded, 'Native payload paths alias by case'); folded.add(name.lower()); expected.add(name)
        for part in name.split('/'):
            path = path/part
            need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(), 'Native payload path contains a symlink/junction')
        need(path.is_file() and path.resolve(strict=True).is_relative_to(root), 'Native payload is absent or escapes its artifact root')
        need(type(row['size']) is int and 0 <= row['size'] <= 256*1024*1024 and path.stat().st_size == row['size'] and
             type(row['sha256']) is str and re.fullmatch('[0-9a-f]{64}', row['sha256']) and sha(path) == row['sha256'], 'Native payload hash/size differs')
        total += row['size']; need(total <= 1024*1024*1024, 'Native payload inventory exceeds1GiB')
        need(row['role'] in ('library', 'dependency', 'notice'), 'Physical scalar-only game has an unexpected payload role')
        if row['role'] in ('library', 'dependency'):
            need(image_os(path, library=True) == spec['target_os'], 'Native payload executable target differs')
        if row['role'] == 'library': libraries.append(name)
        if row['role'] == 'notice': need(row['size'] > 0, 'Packaged notice is empty')
        pins[str(path)] = row['sha256']; audit.append(copy.deepcopy(row))
    need(libraries == [relative(spec['library'])], 'Exact single inventoried native library required')
    notices = {row['path'] for row in rows if row['role'] == 'notice'}
    need({'notices/POIMA-LICENSE.txt', 'notices/POIMA-THIRD-PARTY-NOTICES.md'} <= notices, 'Engine license/notices are missing')
    runtime_versions, aot_versions = [], []
    for label, versions in [('DOTNET-RUNTIME', runtime_versions), ('DOTNET-NATIVEAOT', aot_versions)]:
        prefix = 'notices/'+label+'-'
        for name in notices:
            match = re.fullmatch(re.escape(prefix)+r'(\d+\.\d+\.\d+)-LICENSE\.txt', name)
            if match:
                versions.append(match.group(1))
                need(prefix+match.group(1)+'-THIRD-PARTY-NOTICES.txt' in notices, 'Runtime notice pair is incomplete')
    need(len(runtime_versions) == len(aot_versions) == 1 and runtime_versions == aot_versions, 'Exact matching .NET runtime/AOT notice versions required')
    actual, tree_names, entries = set(), set(), 0
    for path in root.rglob('*'):
        entries += 1; need(entries <= 1024, 'Native artifact tree exceeds1024 entries')
        name = relative(path.relative_to(root).as_posix())
        need(name.lower() not in tree_names, 'Native artifact tree has a case-insensitive path collision'); tree_names.add(name.lower())
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)(), 'Native artifact tree contains a linked entry')
        if path.is_file(): actual.add(name)
        else: need(path.is_dir(), 'Native artifact tree contains a nonregular entry')
    need(actual == expected, 'Native artifact has extra/missing un-inventoried files')
    library = root/spec['library']
    return dict(descriptor=str(descriptor), descriptor_sha256=pins[str(descriptor)], library=str(library), library_sha256=sha(library),
                format=spec['format'], version=spec['version'], engine_version=spec['engine_version'], target_os=spec['target_os'],
                target_arch=spec['target_arch'], required_features=spec['required_features'], services_bytes=424,
                schema=copy.deepcopy(schema), state_bytes=state_bytes, files=audit, notice_version=runtime_versions[0],
                complete_tree_entries=entries, source_pins=pins,
                scope='Read-only whole artifact inventory/native-image/schema admission; actual loading and callbacks are separate later RPCs. Notice file inventory is checked; this is not a new license-origin audit.')


def loaded_backend(module, report):
    need(module['backend'] == report['backend'] and module['assembly_sha256'] == report['compiled_image_sha256'], 'Wrong compiled backend/image')
    if report['backend'] == 'native_aot':
        diagnostics = module['native_diagnostics']
        need(type(diagnostics) is dict and diagnostics.get('dynamic_code_supported') is False and
             diagnostics.get('dynamic_code_compiled') is False, 'Loaded NativeAOT export did not prove dynamic code disabled')
        need(exact(module['schema'], report['native_artifact']['schema']), 'Loaded native schema differs from inventoried descriptor')


def validate(values):
    need(type(values) is dict and set(values) == set(FIELDS), 'Complete original33-field gameplay state differs')
    for key, kind in FIELDS.items():
        value = values[key]
        if kind == 'entity':
            valid = type(value) is str and re.fullmatch('[0-9a-f]{32}', value) is not None
        elif kind == 'int32':
            valid = type(value) is int and -(2**31) <= value < 2**31
        elif kind == 'int64':
            valid = type(value) is str and len(value) <= 20 and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None
            if valid:
                valid = -(2**63) <= int(value) < 2**63
        else:
            valid = type(value) is float and math.isfinite(value)
        need(valid, 'Original typed gameplay value invalid: '+key)
    return values  # Preserve every original spelling/type/value; no coercion.


def initial_values():
    values = {key: '0'*32 if kind == 'entity' else '0' if kind == 'int64' else
              0.0 if kind == 'float64' else 0 for key, kind in FIELDS.items()}
    values.update(Rig=fixture.RIG, FailAt='-1', ImpulseX=1.0, ImpulseY=.5)
    return values


class Consumer:
    def __init__(self, session, report, config, model=None):
        self.io, self.report, self.config = session, report, config
        self.session, self.tick = fixture.SESSION, 0
        self.passive = False
        self.model = copy.deepcopy(model if model is not None else initial_values())

    def game(self):
        return self.io.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))

    def inspect(self):
        value = self.io.rpc('runtime.inspect', dict(session_id=self.session))
        need(type(value['tick']) is int and value['tick'] == self.tick, 'Actual native clock differs from tracked receipts')
        return value

    def physical(self):
        value = fixture.query(self.io, self.tick, session=self.session, include_parts=True, include_joints=True)
        fixture.validate_projection(value, self.passive)
        return value

    def observe(self, label):
        module = self.game()['module']; loaded_backend(module, self.report)
        value = validate(module['values'])
        need(exact(value, self.model), 'Complete independently expected typed state differs: '+label)
        self.report['observations'].append(dict(label=label, tick=self.tick, values=copy.deepcopy(value), expected=copy.deepcopy(self.model)))
        return value

    def edit(self, **changes):
        need(set(changes) <= {'Mode', 'FailAt', 'ImpulseX', 'ImpulseY', 'ImpulseZ'}, 'Verifier may edit only explicit fixture controls')
        self.io.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
                    expected_revision=self.game()['revision'], values=changes))
        self.model.update(changes)
        self.observe('typed control edit')

    def single(self, label, caller=()):
        before_tick = self.tick
        before = self.physical()
        mode = self.model['Mode']
        expected = copy.deepcopy(self.model)
        expected.update(Ticks=expected['Ticks']+1, ObservedTick=str(before_tick),
                        ObservedMode=1 if before['mode'] == 'passive' else 0,
                        BodyCount=2, JointCount=1, CommittedReadChecks=expected['CommittedReadChecks']+1)
        for prefix, part in zip(('Root', 'Tip'), before['parts']):
            expected[prefix+'Node'] = fixture.node(2 if prefix == 'Root' else 1)
            expected[prefix+'Body'] = str(part['body_id'])
            expected[prefix+'Active'] = 1 if part['active'] else 0
            expected[prefix+'Mass'] = part['mass']
            for suffix, actual in zip(('X', 'Y', 'Z'), part['position']):
                expected[prefix+suffix] = actual
            for suffix, actual in zip(('Vx', 'Vy', 'Vz'), part['linear_velocity']):
                expected[prefix+suffix] = actual
        need(mode in (0, 1, 2), 'Success step must use a known nonfaulting mode')
        if mode:
            expected['Writes'] += 1 if mode == 1 else 3
            expected['Mode'] = 0
        result = fixture.step(self.io, before_tick, caller, session=self.session)
        need(type(result['tick']) is int and result['tick'] == before_tick+1 and result['replayed'] is False,
             'Actual step receipt did not commit exactly one tick')
        self.tick = result['tick']
        if mode == 1 or any(command.get('activate') is True for command in caller):
            self.passive = True
        self.model = expected
        self.inspect()
        self.observe(label)
        after = self.physical()
        need([p['body_id'] for p in before['parts']] == [p['body_id'] for p in after['parts']], 'Tick relabeled owned part identities')
        self.report['native_observations'].append(dict(label=label, before_tick=before_tick, after_tick=self.tick,
                                                      before=before, after=after, caller_commands=list(caller)))
        return before, after

    def snapshot(self, saves, label):
        slot = 'managed-physical-'+str(len(self.report['archives'])+1)
        state, game = self.inspect(), self.game()
        receipt = self.io.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=0, session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], expected_material_revision=state['material_revision'],
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence']))
        directory = saves/('slot-'+slot)
        manifest_path = directory/'current.json'
        need(manifest_path.stat().st_size <= 1024*1024, 'Manifest exceeds finite1MiB bound')
        manifest = strict_json(manifest_path.read_bytes()); current = manifest['payload']['current']
        need(type(current['file']) is str and re.fullmatch(r'p-[0-9a-f]{32}\.bin', current['file']), 'Unexpected archive payload path')
        path = directory/current['file']; size = path.stat().st_size
        need(size <= 64*1024*1024, 'Archive exceeds64MiB')
        raw = path.read_bytes(); digest = hashlib.sha256(raw).hexdigest()
        need(type(receipt['bytes']) is int and type(current['bytes']) is int and size == receipt['bytes'] == current['bytes'] and
             receipt['sha256'] == current['sha256'] == digest and current['generation'] == receipt['generation'] == 1,
             'Whole native archive/manifest/receipt identities differ')
        archive = strict_json(raw)
        need(set(archive) == {'format', 'version', 'document', 'snapshot'} and archive['format'] == 'poima.world-save' and
             type(archive['version']) is int and archive['version'] == 1, 'Complete world-save envelope differs')
        payload = archive['snapshot']['payload']
        need(payload['tick'] == self.tick and payload['gameplay']['backend'] == self.report['backend'] and
             payload['gameplay']['assembly_sha256'] == self.report['compiled_image_sha256'] and payload['gameplay']['type'] == GAME and
             exact(payload['gameplay']['schema'], self.report['loaded_module']['schema']) and
             exact(validate(payload['gameplay']['values']), self.model), 'Whole archive lost a typed gameplay field or compiled backend/image/schema identity')
        physical = payload['physical']
        need(set(physical) == {'version', 'bootstrap', 'rigs', 'constraint_order', 'owner', 'physics'} and
             physical['version'] == 1 and len(physical['rigs']) == 1 and physical['rigs'][0]['entity'] == fixture.RIG and
             physical['rigs'][0]['membership']['mode'] == ('passive' if self.passive else 'animated'),
             'Physical extension membership is incomplete')
        row = dict(label=label, slot=slot, tick=self.tick, expected_passive=self.passive, path=str(path), bytes=size, sha256=digest,
                   receipt=receipt, manifest=manifest, manifest_path=str(manifest_path), manifest_sha256=sha(manifest_path), archive=archive)
        self.report['archives'].append(copy.deepcopy(row))
        return row

    def restore(self, saves, row, existing=True):
        state, game = (self.inspect(), self.game()) if existing else (None, None)
        result = self.io.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=row['slot'], expected_generation=1,
            revision=2, expected_session_id=self.session if existing else None, expected_tick=self.tick if existing else None,
            expected_gameplay_revision=game['revision'] if existing else None,
            expected_structure_revision=state['structure_revision'] if existing else None,
            expected_material_revision=state['material_revision'] if existing else None,
            expected_ui_revision=state['ui_revision'] if existing else None,
            expected_control_sequence=state['control_sequence'] if existing else None,
            new_session_id=fresh(), gameplay=self.config))
        self.session = result['session_id'] if 'session_id' in result else result['new_session_id']
        self.tick = result['tick']
        self.passive = row['expected_passive']
        need(self.tick == row['tick'], 'Native restore receipt clock differs')
        self.model = copy.deepcopy(row['archive']['snapshot']['payload']['gameplay']['values'])
        # This expected row was previously checked in full against the independent
        # model, and its original complete payload/receipt remain pinned.
        self.observe('restored complete original typed state')


def pair(report, label, left, right):
    need(exact(left['archive'], right['archive']) and Path(left['path']).read_bytes() == Path(right['path']).read_bytes(),
         'Whole original archive bytes/document/snapshot differ: '+label)
    report['whole_pairs'].append(dict(label=label, left=left['path'], right=right['path'], bytes_equal=True,
                                      complete_archive_equal=True, exclusions=[]))


def near(value, expected, tolerance, message):
    need(type(value) is float and math.isfinite(value) and abs(value-expected) <= tolerance,
         f'{message}: {value!r} vs {expected!r}, tolerance{tolerance}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--hostfxr', type=Path)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--game', '--assembly', dest='game', type=Path)
    parser.add_argument('--native-descriptor', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    coreclr = all((args.hostfxr, args.bridge, args.game))
    if (args.native_descriptor and any((args.hostfxr, args.bridge, args.game))) or (not args.native_descriptor and not coreclr):
        parser.error('Supply either --native-descriptor or the complete --hostfxr/--bridge/--game triple')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    fixture.ARGS = args; fixture.DEADLINE = time.monotonic()+SECONDS; fixture.MAX_CALLS = RPC_LIMIT
    report = dict(format='poima.managed-physical-gameplay-contract.v1', passed=False, backend='native_aot' if args.native_descriptor else 'coreclr', calls=[], owners=[],
                  archives=[], observations=[], native_observations=[], whole_pairs=[], checks=[], source_pins={},
                  limits=dict(seconds=SECONDS, rpc=RPC_LIMIT, evidence_bytes=EVIDENCE_LIMIT),
                  limitations=['Actual prebuilt '+('NativeAOT artifact' if args.native_descriptor else 'CoreCLR consumer')+' and native CLI; no GPU, physical input, audio device, relocated standalone export or fresh-agent claim.',
                               'Pre-Tick native projections independently check the compiled callback boundary; authored geometry and total momentum supply analytic oracles.',
                               'Root/tip/root impulses at fixed COM points commute; their total effect detects drops/averaging, not permutations. Caller activation before gameplay impulses supplies a noncommuting ordering check.',
                               'External owner must independently prove bounded process-tree ownership; closure results here cover the two retained CLI children.'])
    fixture.REPORT = report
    session = consumer = None
    pins = {}
    try:
        files = [Path(__file__).resolve(), Path(fixture.__file__).resolve(), ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py',
                 SAMPLE/'Game.cs', SAMPLE/'Poima.ManagedPhysicalGame.csproj', ROOT/'managed/Directory.Build.props',
                 ROOT/'managed/Poima.Gameplay/PhysicalRig.cs', ROOT/'managed/Poima.Gameplay/GameplayRequirements.cs',
                 ROOT/'managed/Poima.ManagedBridge/Entry.cs', args.binary.resolve()]
        if args.native_descriptor:
            audit = native_artifact(args.native_descriptor, args.binary)
            report['native_artifact'] = audit; report['compiled_image_sha256'] = audit['library_sha256']
            files += [Path(path) for path in audit['source_pins']]
            files += [ROOT/'managed/Poima.NativeGame/Entry.cs', ROOT/'managed/Poima.NativeGame.Generator/Program.cs', ROOT/'scripts/publish_native_gameplay.py']
            config = dict(descriptor=fixture.native(args.native_descriptor), expected_descriptor_sha256=audit['descriptor_sha256'])
        else:
            report['compiled_image_sha256'] = sha(args.game)
            files += [args.hostfxr.resolve(), args.bridge.resolve(), args.game.resolve()]
            for assembly in (args.game.resolve(), args.bridge.resolve()):
                files.append(assembly.parent/'Poima.Gameplay.dll')
                for suffix in ('.deps.json', '.runtimeconfig.json'):
                    member = assembly.with_suffix(suffix)
                    if member.is_file(): files.append(member)
            config = dict(hostfxr=fixture.native(args.hostfxr), bridge=fixture.native(args.bridge),
                          assembly=fixture.native(args.game), type=GAME)
        for path in set(files):
            need(path.is_file(), 'Required source/compiled artifact missing: '+str(path))
            pins[str(path)] = sha(path)
        report['source_pins'] = pins
        session = fixture.Session(output/'original-owner')
        fixture.prepare(session); fixture.configure(session)
        original = session.world.read_bytes(); report['authored_world_sha256'] = hashlib.sha256(original).hexdigest()
        fixture.start(session)
        consumer = Consumer(session, report, config)
        loaded = session.rpc('runtime.gameplay.load_native' if args.native_descriptor else 'runtime.gameplay.load', dict(session_id=consumer.session, request_id=fresh(), expected_tick=0,
                             expected_revision=consumer.game()['revision'], **config))
        module = loaded['module']; schema = module['schema']
        loaded_backend(module, report)
        need(schema['identity'] == IDENTITY and len(schema['fields']) == len(FIELDS) == 33 and
             {f['name']: f['kind'] for f in schema['fields']} == FIELDS, 'Wrong compiled module/full typed contract')
        report['loaded_module'] = copy.deepcopy(module)
        consumer.observe('initialized exact33 fields')
        saves = fixture.save_root(session)
        before = consumer.physical()
        need([p['position'] for p in before['parts']] == [[0.0, 3.0, 0.0], [0.0, 4.0, 0.0]], 'Analytic authored COM geometry differs')
        for p, mass in zip(before['parts'], (3.0, 1.0)):
            near(p['mass'], mass, 1e-6, 'Authored analytic mass differs')
            need(p['linear_velocity'] == [0.0]*3 and p['active'] is False, 'Initial Animated allocation is not neutral/off-world')
        consumer.single('ordinary committed Animated reads')
        animated = consumer.snapshot(saves, 'animated-before-activation')

        consumer.edit(Mode=1)
        consumer.single('compiled standalone activation')
        need(consumer.physical()['mode'] == 'passive' and consumer.model['Writes'] == 1, 'Compiled activation did not transfer native membership')
        consumer.snapshot(saves, 'standalone-activation-state')
        consumer.restore(saves, animated)
        pair(report, 'same-owner-animated-cold-restore', animated, consumer.snapshot(saves, 'animated-restored'))

        # Native application must activate before the three compiled impulses,
        # although all compiled reads during the callback still see Animated.
        consumer.edit(Mode=2)
        before, after = consumer.single('caller activation then three compiled localized impulses', [dict(entity=fixture.RIG, activate=True)])
        need(before['mode'] == 'animated' and after['mode'] == 'passive' and consumer.model['ObservedMode'] == 0 and consumer.model['Writes'] == 3,
             'Caller/game ordering or committed-read visibility differs')
        impulse = [1.0, .5, 0.0]
        total_momentum = [sum(m*p['linear_velocity'][axis] for m, p in zip((3.0, 1.0), after['parts'])) for axis in range(3)]
        expected_momentum = [(3*impulse[axis]+(4*(-9.81)/60 if axis == 1 else 0))*(1-.05/60) for axis in range(3)]
        for axis in range(3): near(total_momentum[axis], expected_momentum[axis], 2e-5, 'Independent three-impulse/gravity/drag total momentum differs')
        report['momentum_oracle'] = dict(authored_masses=[3.0, 1.0], impulse=impulse, applications=['root', 'tip', 'root'],
                                         dt=1/60, gravity=[0.0, -9.81, 0.0], linear_damping=.05,
                                         expected=expected_momentum, actual=total_momentum, tolerance=2e-5)
        report['checks'].append('Noncommuting caller activation precedes compiled root/tip/root impulses; complete staged reads remain unchanged; analytic total momentum counts all three impulses')

        consumer.edit(Mode=3)
        duplicate_before = consumer.snapshot(saves, 'duplicate-activation-before')
        native_before = consumer.physical()
        error = fixture.step(session, consumer.tick, session=consumer.session, error=-32040)
        need('Duplicate gameplay physical activation' in error['message'], 'Wrong duplicate activation failure')
        consumer.inspect(); consumer.observe('duplicate activation rolled back all fields')
        need(exact(native_before, consumer.physical()), 'Duplicate activation changed a native physical field')
        pair(report, 'duplicate-activation-whole-rollback', duplicate_before, consumer.snapshot(saves, 'duplicate-activation-after'))

        consumer.edit(Mode=2, FailAt=str(consumer.tick+1))
        fault_before = consumer.snapshot(saves, 'later-tick-fault-before')
        native_before = consumer.physical()
        error = fixture.step(session, consumer.tick, ticks=2, session=consumer.session, error=-32040)
        need('Physical fixture later-Tick failure' in error['message'], 'Wrong intentional second-Tick callback failure')
        consumer.inspect(); consumer.observe('second-Tick failure restored first-Tick writes and all fields')
        need(exact(native_before, consumer.physical()), 'Later-Tick failure changed a native physical field')
        pair(report, 'later-tick-whole-rollback', fault_before, consumer.snapshot(saves, 'later-tick-fault-after'))
        consumer.edit(Mode=0, FailAt='-1')

        # Retain actual regular floor contacts, not just airborne joint state.
        for index in range(58): consumer.single('contact warmup '+str(index+1))
        checkpoint = consumer.snapshot(saves, 'warm-contact-checkpoint')
        physical = checkpoint['archive']['snapshot']['payload']['physical']
        floor_ids = [r['id'] for r in physical['bootstrap']['bodies'] if r['entity'] == fixture.FLOOR]
        need(len(floor_ids) == 1, 'Saved ordinary floor identity missing')
        contacts = [p for p in physical['physics']['pairs'] if floor_ids[0] in (p['body_a'], p['body_b'])]
        need(contacts and any(point['normal_lambda'] > 0 for p in contacts for m in p['manifolds'] for point in m['points']),
             'Checkpoint lacks actual nonzero floor contact warm starts')
        checkpoint_model = copy.deepcopy(consumer.model)
        for index in range(7): consumer.single('warm future '+str(index+1))
        future = consumer.snapshot(saves, 'warm-future-reference')
        future_model = copy.deepcopy(consumer.model)
        need(session.world.read_bytes() == original, 'Runtime/managed workflow changed authored world bytes')
        world = session.world
        session.close(); session = None

        session = fixture.Session(output/'fresh-owner', world)
        configured = session.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=fixture.native(saves)))
        need(configured['generation'] == 1, 'Fresh-process save host configuration differs')
        consumer = Consumer(session, report, config, checkpoint_model)
        consumer.restore(saves, checkpoint, existing=False)
        pair(report, 'fresh-process-whole-restoration', checkpoint, consumer.snapshot(saves, 'fresh-restored-checkpoint'))
        for index in range(7): consumer.single('cold future '+str(index+1))
        need(exact(consumer.model, future_model), 'Fresh continuation lost a complete independently expected typed field')
        pair(report, 'fresh-process-whole-future-continuation', future, consumer.snapshot(saves, 'fresh-future'))
        need(world.read_bytes() == original, 'Fresh restore mutated authoring')
        report['checks'].append('Complete33-field native-boundary observations, whole-byte duplicate/later-Tick rollback, actual warm floor contacts and fresh-process exact whole save/future continuation')
        session.close(); session = None
        need(len(report['owners']) == 2 and all(o['natural_exit'] and not o['forced_cleanup'] and o['exit_code'] == 0 for o in report['owners']),
             'Both actual CLI producers must close naturally')
        need(len(report['observations']) == 85 and len(report['native_observations']) == 75 and
             len(report['archives']) == 11 and len(report['whole_pairs']) == 5,
             'Complete expected85 observations/75 native joins/11 archives/5 whole-pair population required')
        need(all(sha(path) == digest for path, digest in pins.items()), 'Input/source identity changed during native qualification')
        report['passed'] = True
    except Exception as error:
        report['failure'] = dict(type=type(error).__name__, message=str(error))
    finally:
        if session is not None:
            try: session.close()
            except Exception as error:
                report['closure_failure'] = dict(type=type(error).__name__, message=str(error)); report['passed'] = False
        report['elapsed_seconds'] = time.monotonic()-(fixture.DEADLINE-SECONDS)
        report['recordings'] = []
        for owner in report['owners']:
            for name in ('requests.jsonl', 'responses.jsonl', 'stderr.log'):
                path = Path(owner['directory'])/name
                if path.is_file(): report['recordings'].append(dict(path=str(path), bytes=path.stat().st_size, sha256=sha(path)))
        encoded = json.dumps(report, indent=2, allow_nan=False)+'\n'
        if len(encoded.encode()) > EVIDENCE_LIMIT:
            report['passed'] = False; report['failure'] = dict(type='EvidenceBound', message='Full original evidence exceeds32MiB; no fields were dropped')
            encoded = json.dumps(report, indent=2, allow_nan=False)+'\n'
        (output/'evidence.json').write_text(encoded, encoding='utf-8')
        print(json.dumps(dict(passed=report['passed'], output=str(output), calls=len(report['calls']), owners=len(report['owners']),
                              observations=len(report['observations']), archives=len(report['archives']), whole_pairs=len(report['whole_pairs']))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
