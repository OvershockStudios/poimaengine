#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Upgrade a retained, genuinely played Relay Yard checkpoint and finish it.

Caller supplies an unchanged 0.0.85 passing contract directory and separate
old/new Native AOT artifacts. No builds, downloads or gameplay edits. Optional
Windows captures compare the identical source/target checkpoint with actual
imported weighted geometry submissions.
All execution uses owned native processes; originals are read only.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
import traceback
from types import SimpleNamespace
import uuid

ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent / 'examples/relay-yard/run.py').is_file())
SLOT = 'relay-yard'
ZERO = '0' * 32
NEW_FIELD = 'CheckpointVersion'
NEW_ID = 'f8ad1d8f0fab3a847f1731ff1fb01f3d'


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inventory(directory):
    return {path.relative_to(directory).as_posix(): dict(bytes=path.stat().st_size, sha256=sha(path))
            for path in sorted(directory.rglob('*')) if path.is_file()}


def read(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= 64 * 1024 * 1024,
         'Expected a bounded regular JSON file: ' + str(path))
    return json.loads(path.read_text(encoding='utf-8'))


def import_source(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    need(spec is not None and spec.loader is not None, 'Missing source helper: ' + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DRIVER_SOURCE = ROOT / 'tests/relay_yard_contract.py'
RAW_SOURCE = ROOT / 'tests/instance_save_evolution.py'
driver_module = import_source('poima_relay_upgrade_driver', DRIVER_SOURCE)
raw_module = import_source('poima_relay_upgrade_raw_json', RAW_SOURCE)
launcher = driver_module.load_launcher()


def artifact(path):
    path = path.resolve(strict=True)
    if path.is_dir():
        path = path / 'native-gameplay.json'
    document = read(path)
    need(document['format'] == 'poima.native-gameplay' and document['version'] == 2 and
         document['type'] == 'Poima.Examples.RelayYardGame' and
         document['schema']['identity'] == 'poima.examples.relay-yard',
         'Supply the genuine Relay Yard descriptor, not another fixture')
    files = {str(path): sha(path)}
    names = {'native-gameplay.json'}
    library = None
    for row in document['files']:
        relative = Path(row['path'])
        need(not relative.is_absolute() and '..' not in relative.parts and
             relative.as_posix().casefold() not in names, 'Unsafe/duplicate artifact member')
        names.add(relative.as_posix().casefold())
        member = path.parent / relative
        need(member.is_file() and not member.is_symlink() and member.stat().st_size == row['size'] and
             sha(member) == row['sha256'], 'Artifact payload differs: ' + str(member))
        files[str(member)] = sha(member)
        if row['path'] == document['library']:
            need(library is None and row['role'] == 'library', 'Duplicate/wrong library role')
            library = member
    need(library is not None, 'Artifact library is absent')
    need({str(p) for p in path.parent.rglob('*') if p.is_file()} == set(files),
         'Native artifact must be its exact published closure')
    return path, document, files, sha(library)


def stored(root, slot):
    folder = root / ('slot-' + slot)
    manifest = read(folder / 'current.json')
    row = manifest['payload']['current']
    need(row['generation'] == manifest['payload']['generation'], 'Save generation binding differs')
    relative = Path(row['file'])
    need(not relative.is_absolute() and '..' not in relative.parts, 'Unsafe checkpoint payload path')
    path = folder / relative
    need(path.is_file() and path.stat().st_size == row['bytes'] and sha(path) == row['sha256'],
         'Checkpoint bytes differ from actual manifest')
    saved = read(path)
    need(saved['format'] == 'poima.world-save' and saved['version'] == 1 and
         saved['snapshot']['format'] == 'poima.runtime-snapshot' and saved['snapshot']['version'] == 5,
         'Supply the retained version-five root-prop game checkpoint')
    text = path.read_text(encoding='utf-8')
    need(raw_module.raw_digest(text, ('snapshot', 'payload')) == saved['snapshot']['sha256'],
         'Native snapshot checksum differs from its original token bytes')
    return saved, path, row


def identity(saved, path):
    payload, game = saved['snapshot']['payload'], saved['snapshot']['payload']['gameplay']
    return dict(world_id=payload['world_id'], content_sha256=payload['content_sha256'],
        backend=game['backend'], module_identity=game['schema']['identity'], type=game['type'],
        image_sha256=game['assembly_sha256'],
        schema_sha256=raw_module.raw_digest(path.read_text(encoding='utf-8'),
                                           ('snapshot', 'payload', 'gameplay', 'schema')))


class Owner(launcher.OwnedRelay):
    def __init__(self, binary, descriptor, world, saves, label, record, deadline):
        super().__init__(SimpleNamespace(binary=binary, descriptor=descriptor), world, label,
                         record, deadline, lambda path: str(Path(path).resolve()), None)
        self.storage = saves

    def guards(self):
        if not self.session:
            return {key: None for key in ('expected_session_id', 'expected_tick',
                'expected_gameplay_revision', 'expected_component_revision',
                'expected_structure_revision', 'expected_ui_revision', 'expected_control_sequence')}
        state, module, route = self.inspect(), self.module(), self.route()
        return dict(expected_session_id=self.session, expected_tick=self.tick,
            expected_gameplay_revision=module['revision'], expected_component_revision=route['component_revision'],
            **{'expected_' + key: state[key] for key in
               ('structure_revision', 'ui_revision', 'control_sequence')})

    def load_parameters(self, slot=SLOT, generation=2, upgrade=None):
        result = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
            slot=slot, expected_generation=generation, revision=self.rpc('world.inspect')['revision'],
            new_session_id=uuid.uuid4().hex, gameplay=self.config(), **self.guards())
        if upgrade is not None:
            result['upgrade'] = dict(path=str(upgrade.resolve()), expected_sha256=sha(upgrade))
        return result

    def external_load(self, parameters, error=None):
        result = self.rpc('save.load', parameters, error=error)
        if error is None:
            self.session, self.tick = parameters['new_session_id'], result['tick']
        return result

    def write(self, slot, generation=0):
        guards = self.guards()
        guards['session_id'] = guards.pop('expected_session_id')
        result = self.rpc('save.write', dict(request_id=uuid.uuid4().hex,
            configuration_generation=self.configuration, slot=slot, expected_generation=generation, **guards))
        saved, path, row = stored(self.storage, slot)
        need(result['sha256'] == row['sha256'], 'Published save response differs from durable bytes')
        return saved, path, result

    def full_snapshot(self, label):
        saved, path, receipt = self.write('probe-' + uuid.uuid4().hex)
        self.record.setdefault('snapshots', {})[self.label + ':' + label] = dict(
            snapshot=saved['snapshot'], path=str(path), receipt=receipt)
        return saved['snapshot']

    def capture(self, label, gpu, output):
        before = self.fingerprint()
        path = output / (label + '.bmp')
        need(not path.exists(), 'Capture output must be new')
        state = self.inspect()
        response = self.rpc('runtime.capture', dict(session_id=self.session, tick=self.tick,
            ui_revision=state['ui_revision'], camera=driver_module.uid(102),
            path=str(path), width=960, height=540, samples=4, gpu=gpu, ui_scale=1, profile=True))
        need(response['hardware'] and response['capture_written'] and response['nvrhi_errors'] == 0 and
             self.fingerprint() == before, 'Capture failed or changed authoritative state')
        draws = response['render_diagnostics']['last_draws']
        need(draws['camera_draws'] > 0 and draws['skinned_instances'] > 0 and
             draws['skinned_vertices'] > 0, 'Upgrade comparison must submit actual imported weighted geometry')
        self.record.setdefault('captures', {})[label] = dict(response=response, sha256=sha(path),
            path=str(path), observed_draws={key: draws[key] for key in
                ('camera_draws', 'camera_triangles', 'skinned_instances', 'skinned_vertices')})
        return path

    def fingerprint(self):
        return dict(state=self.inspect(), module=self.module(), route=self.route(),
                    saves=self.rpc('runtime.save.status', dict(session_id=self.session)))


def qualify(args, output, record, owners, deadline, baseline, source_descriptor, target_descriptor,
            source_image, target_image):
    historical = read(baseline / 'evidence.json')
    need(historical['passed'] is True and historical['format'] == 'poima.relay-yard-contract' and
         historical['immutable_inputs_on_exit']['authored'] and
         historical['immutable_inputs_on_exit']['cooked'] and
         historical['immutable_inputs_on_exit']['durable_checkpoint'],
         'Baseline must be an actual passing retained .85 game contract')
    world = baseline / 'world.json'
    need(sha(world) == historical['immutable_world']['sha256'] and
         inventory(baseline / 'world.json.assets') == historical['immutable_world']['assets'],
         'Retained authored/cooked closure differs from .85 evidence')
    need(inventory(baseline / 'saves' / ('slot-' + SLOT)) == historical['durable_checkpoint_pin'],
         'Retained slot differs from the actual .85 checkpoint inventory')
    original, original_path, entry = stored(baseline / 'saves', SLOT)
    old = original['snapshot']['payload']
    old_values, old_schema = old['gameplay']['values'], old['gameplay']['schema']
    need(entry['generation'] == 2 and old['tick'] == 567 and len(old_schema['fields']) == 49 and
         'persistent' not in old_schema and old_values['Initialized'] == 1 and old_values['Started'] == 1 and
         old_values['Collected'] == 1 and old_values['Phase'] == 0 and old_values['Moving'] == 0 and
         old_values['CellOne'] == ZERO and
         old_values['CellTwo'] != ZERO and old_values['CellThree'] != ZERO and
         old['gameplay']['backend'] == 'native_aot' and old['gameplay']['assembly_sha256'] == source_image,
         'Supply the actual one-cell stationary checkpoint and its original native image')
    need(old_values == historical['checkpoint']['saved']['values'] and
         old['tick'] == historical['checkpoint']['saved']['tick'],
         'Retained checkpoint does not match the independently recorded game state')
    record['baseline_checkpoint'] = dict(path=str(original_path), entry=entry,
        sha256=sha(original_path), old_values=old_values)
    storage = output / 'saves'
    shutil.copytree(baseline / 'saves', storage)
    source_world, target_world = output / 'source.json', output / 'target.json'
    for destination in (source_world, target_world):
        shutil.copy2(world, destination)
        shutil.copytree(baseline / 'world.json.assets', Path(str(destination) + '.assets'))
    record['copied_content_pins'] = {str(path): sha(path) for path in (source_world, target_world)}
    record['copied_asset_pins'] = {str(Path(str(path) + '.assets')):
        inventory(Path(str(path) + '.assets')) for path in (source_world, target_world)}
    source_slot_pin = inventory(storage / ('slot-' + SLOT))
    record['source_slot_pin'] = source_slot_pin

    def owner(label, descriptor, selected_world=target_world):
        game = Owner(args.binary, descriptor, selected_world, storage, label, record, deadline)
        owners.append(game)
        # Register the opened native process before the first mutation so a
        # configuration rejection/unknown outcome cannot escape cleanup.
        game.saves(storage)
        return game

    source = owner('retained-source', source_descriptor, source_world)
    source.external_load(source.load_parameters())
    source_snapshot = source.full_snapshot('exact-restored')
    need(source_snapshot == original['snapshot'], 'Actual source exact restore differs from retained checkpoint')
    source_pixels = source.capture('source-checkpoint', args.gpu, output) if args.capture else None
    source.close()
    record['checks'].append('Original native artifact exactly restores retained real-game checkpoint')

    probe = owner('target-probe', target_descriptor)
    probe.start(); probe.load()
    initialized = probe.values()
    need(initialized[NEW_FIELD] == 2 and initialized['Initialized'] == 0 and initialized['Started'] == 0 and
         initialized['Collected'] == 0 and all(initialized[field] == ZERO for field in driver_module.CELL_FIELDS),
         'Ordinary target Initialize must be distinct from preserved played state')
    probed, probe_path, _ = probe.write('target-metadata')
    record['ordinary_target_initialization'] = initialized
    probe.close()
    source_identity, target_identity = identity(original, original_path), identity(probed, probe_path)
    need(target_identity['image_sha256'] == target_image and source_image != target_image and
         target_identity['content_sha256'] == source_identity['content_sha256'],
         'Global-only upgrade must change actual image while preserving exact frozen content')
    schema = probed['snapshot']['payload']['gameplay']['schema']
    persistence = schema['persistent']
    need(persistence['revision'] == 2 and len(schema['fields']) == 50,
         'Target must expose actual revised persistent field metadata')
    target_fields = {field['name']: field for field in persistence['fields']}
    need(set(target_fields) == set(old_values) | {NEW_FIELD} and
         target_fields[NEW_FIELD]['id'] == NEW_ID and target_fields[NEW_FIELD]['kind'] == 'int32' and
         target_fields[NEW_FIELD]['default'] == 2,
         'Target fields must preserve all original names and add literal version-two default')
    legacy = sorted([dict(name=field['name'], id=target_fields[field['name']]['id'])
                     for field in old_schema['fields']], key=lambda field: field['id'])
    for field in old_schema['fields']:
        need(target_fields[field['name']]['kind'] == field['kind'], 'Legacy scalar kind changed')
    plan = dict(format='poima.save-upgrade', version=1, id=uuid.uuid4().hex,
        source=source_identity, target=target_identity,
        **{'global': dict(preserve=[field['id'] for field in legacy], retire=[], default=[NEW_ID]),
           'legacy_global_ids': legacy, 'components': []})
    plan_path = output / 'approved-upgrade.json'
    plan_bytes = (json.dumps(plan, indent=2) + '\n').encode()
    plan_path.write_bytes(plan_bytes)
    record.update(plan=plan, plan_sha256=sha(plan_path), source_identity=source_identity,
                  target_identity=target_identity)

    target = owner('explicit-target', target_descriptor)
    target.start(); target.load()
    before = target.full_snapshot('before-rejections')
    before_fingerprint = target.fingerprint()

    def reject(label, parameters, error=-32070):
        failure = target.external_load(parameters, error=error)
        need(target.full_snapshot('after-' + label) == before and target.fingerprint() == before_fingerprint and
             inventory(storage / ('slot-' + SLOT)) == source_slot_pin,
             'Rejected upgrade changed complete current state or original source slot: ' + label)
        record.setdefault('rejections', {})[label] = failure

    reject('exact-changed-image', target.load_parameters())
    wrong_hash = target.load_parameters(upgrade=plan_path)
    wrong_hash['upgrade']['expected_sha256'] = '0' * 64
    reject('wrong-plan-hash', wrong_hash)
    incomplete = copy.deepcopy(plan); incomplete['global']['preserve'].pop()
    incomplete_path = output / 'missing-preserve.json'
    incomplete_path.write_text(json.dumps(incomplete, indent=2) + '\n', encoding='utf-8')
    reject('missing-preserve', target.load_parameters(upgrade=incomplete_path))

    request = target.load_parameters(upgrade=plan_path)
    receipt = target.external_load(request)
    report = receipt['upgrade']
    need(report['plan_id'] == plan['id'] and report['plan_sha256'] == sha(plan_path),
         'Upgrade receipt does not identify the approved plan bytes')
    for side, observed in (('source', source_identity), ('target', target_identity)):
        for key in ('content_sha256', 'image_sha256', 'schema_sha256'):
            need(report[side + '_' + key] == observed[key],
                 'Upgrade receipt identity differs: ' + side + ':' + key)
    upgraded = target.full_snapshot('immediate-upgrade')
    payload = upgraded['payload']
    need(upgraded['version'] == original['snapshot']['version'] and
         {key: value for key, value in payload.items() if key != 'gameplay'} ==
         {key: value for key, value in old.items() if key != 'gameplay'},
         'Global-only upgrade altered physics/animation/layers/history/handles/allocator/UI/control or components')
    need(payload['gameplay']['values'] == dict(old_values, CheckpointVersion=2) and
         payload['gameplay']['schema'] == schema and payload['gameplay']['assembly_sha256'] == target_image and
         payload['gameplay']['backend'] == target_identity['backend'] and
         payload['gameplay']['type'] == target_identity['type'],
         'Upgrade did not preserve all49 values, add literal default and bind actual target')
    need(receipt['upgrade']['preserved_globals'] == 49 and receipt['upgrade']['defaulted_globals'] == 1 and
         receipt['upgrade']['retired_globals'] == 0 and receipt['upgrade']['components'] == [],
         'Actual upgrade report differs from the approved global-only edge')
    record['immediate_upgrade'] = dict(receipt=receipt, snapshot=upgraded,
        initialization_scope='Preserved played state contradicts fresh Initialize; source restore path skips Initialize. Constructors/statics are not dynamically traced.')

    if args.capture:
        from scene_capture import pixels
        target_pixels = target.capture('target-upgrade', args.gpu, output)
        need(pixels(source_pixels) == pixels(target_pixels) and
             record['captures']['source-checkpoint']['observed_draws'] ==
             record['captures']['target-upgrade']['observed_draws'],
             'Same-tick source/target pixels or imported-geometry submissions differ')
        record['render_preservation'] = dict(passed=True, exact_pixels=True, gpu=args.gpu,
            scope='Same saved camera/tick native output and positive weighted mesh submissions; no independent source-pose oracle or performance qualification.')
        record['checks'].append('Same-tick source/target Vulkan pixels and positive imported weighted geometry submissions remain exact')

    plan_path.unlink()
    try:
        retry = target.rpc('save.load', request)
        need(retry == dict(receipt, replayed=True) and target.full_snapshot('removed-plan-retry') == upgraded,
             'Exact retry reread removed plan or activated a second runtime')
        record['removed_plan_retry'] = retry
    finally:
        plan_path.write_bytes(plan_bytes)
    target.rpc('save.load', dict(request, new_session_id=uuid.uuid4().hex), error=-32010)
    need(target.full_snapshot('receipt-collision') == upgraded, 'Changed request-ID retry mutated runtime')
    stale = target.load_parameters(upgrade=plan_path)
    stale['expected_tick'] = target.tick - 1
    target.external_load(stale, error=-32009)
    need(target.full_snapshot('stale-guard') == upgraded, 'Stale upgrade guard mutated runtime')
    record['checks'].append('Explicit legacy-ID edge preserves complete native state and all49 values; denied edges/retries stay inert')

    driver = driver_module.RelayDriver(target, record)
    driver.control('refresh')
    need(target.values()['SavePending'] == 0 and target.values()['SaveNotice'] == 6 and
         int(target.values()['LastSaveGeneration']) == 2 and target.values()[NEW_FIELD] == 2,
         'Real target Control did not validate version or reconcile restored pending ticket')
    driver.control('close')
    remaining = [old_values['CellTwo'], old_values['CellThree']]
    for index, identifier in enumerate(remaining, 1):
        driver.drive_to(*driver_module.CELL_APPROACHES[index], 'upgraded-cell-' + str(index))
        values = driver.use_target(identifier)
        need(values['Collected'] == index + 1 and values[NEW_FIELD] == 2,
             'Compiled upgraded game did not collect remaining actual ray target')
    driver.wait_for_delivery()
    driver.follow_waypoints(((5.2, 5.2), (5.2, 1.4)), 'upgraded-terminal')
    final = driver.use_target(driver_module.TERMINAL)
    need(final['Won'] == 1 and final['Arrivals'] == 1 and final['Collected'] == 3 and final[NEW_FIELD] == 2,
         'Retained upgraded game cannot finish its real native objective')
    completed, completed_path, completed_receipt = target.write('upgraded-relay-yard')
    need(completed_receipt['generation'] == 1, 'New target slot must start with actual generation one')
    record['completed'] = dict(values=final, saved=completed['snapshot'], receipt=completed_receipt,
                               path=str(completed_path))
    target.close()

    fresh = owner('fresh-target-continuation', target_descriptor)
    fresh.external_load(fresh.load_parameters('upgraded-relay-yard', 1))
    need(fresh.full_snapshot('exact-target-reopen') == completed['snapshot'],
         'Fresh target owner did not exactly reopen deliberately written upgraded slot')
    reopened = driver_module.RelayDriver(fresh, record)
    reopened.control('menu'); reopened.control('refresh'); reopened.control('close')
    reopened.step(5)
    values = fresh.values()
    need(values[NEW_FIELD] == 2 and values['Won'] == 1 and values['Collected'] == 3 and values['Arrivals'] == 1 and
         values['Ticks'] == final['Ticks'] + 5, 'Fresh process failed real target Control/Tick continuation')
    record['fresh_continuation'] = dict(values=values, snapshot=fresh.full_snapshot('after-real-callbacks'))
    fresh.close()
    need(inventory(storage / ('slot-' + SLOT)) == source_slot_pin, 'Upgrade or target Save rewrote retained source slot')
    record['checks'].append('Remaining genuine pickups, courier and terminal complete; deliberate target save exactly reopens and continues in fresh process')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'binary', 'source-artifact', 'target-artifact', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=900)
    parser.add_argument('--gpu', type=int, choices=(0, 1))
    parser.add_argument('--capture', action='store_true')
    args = parser.parse_args()
    need(not args.capture or (os.name == 'nt' and args.gpu is not None),
         'Capture requires native Windows Python and an explicit GPU')
    need(not sys.flags.optimize, 'Run without Python optimization')
    need(math.isfinite(args.timeout) and 60 <= args.timeout <= 1800, 'Timeout must be finite60..1800 seconds')
    args.binary = args.binary.resolve(strict=True)
    baseline = args.baseline.resolve(strict=True)
    need(args.binary.is_file() and baseline.is_dir(), 'Supply a native binary and retained baseline directory')
    source_descriptor, source_artifact, source_files, source_image = artifact(args.source_artifact)
    target_descriptor, target_artifact, target_files, target_image = artifact(args.target_artifact)
    need((source_artifact['target_os'], source_artifact['target_arch']) ==
         (target_artifact['target_os'], target_artifact['target_arch']),
         'Upgrade must retain its actual backend/platform')
    need(source_artifact['minimum_services_bytes'] == target_artifact['minimum_services_bytes'] == 256 and
         source_artifact['services_version'] == target_artifact['services_version'] == 7 and
         set(target_artifact['required_features']) ==
         set(source_artifact['required_features']) | {'gameplay_persistence_v1'},
         'Persistent metadata must be the only added negotiated feature, preserving epoch/prefix')
    need(target_artifact['target_os'] == ('Windows' if os.name == 'nt' else 'Linux') and
         target_artifact['target_arch'] == 'x86_64', 'Use native Python and artifacts for the engine OS')
    output = args.output.resolve()
    need(all(not output.is_relative_to(root) and not root.is_relative_to(output)
             for root in (source_descriptor.parent, target_descriptor.parent)),
         'Output must be outside immutable artifact roots')
    need(not output.is_relative_to(baseline) and not baseline.is_relative_to(output),
         'Output must be independent of the retained baseline')
    output.mkdir(parents=True, exist_ok=False)
    source_paths = [Path(__file__), DRIVER_SOURCE, RAW_SOURCE, ROOT / 'examples/relay-yard/run.py',
        ROOT / 'examples/relay-yard/RelayYardGame.cs', ROOT / 'examples/relay-yard/Poima.RelayYardGame.csproj',
        ROOT / 'examples/locomotion-yard/run.py', ROOT / 'examples/locomotion-yard/LocomotionYardGame.cs',
        *sorted((ROOT / 'tools/python/poima_client').glob('*.py')),
        *([ROOT / 'tests/scene_capture.py'] if args.capture else [])]
    protected = {str(args.binary): sha(args.binary), **source_files, **target_files,
                 **{str(path): sha(path) for path in source_paths}}
    record = dict(format='poima.relay-yard-upgrade', version=1, passed=False,
        calls=[], owners=[], checks=[], cleanup_errors=[], input_pins=protected,
        baseline_inventory=inventory(baseline), limitations=[
            'One real Native AOT global-only legacy-to-persistent upgrade; component/content changes are excluded.',
            'Complete native semantic continuation; optional same-tick Vulkan captures are not physical input, representative performance or general game-update qualification.',
            'Ordinary exact loading remains default; host supplies the trusted one-edge plan and target executable.',
            'Target Initialize separation is observed through real default versus retained state, not a dynamic constructor/static-initializer trace.'])
    owners, started = [], time.monotonic()
    try:
        qualify(args, output, record, owners, started + args.timeout, baseline,
                source_descriptor, target_descriptor, source_image, target_image)
        record['passed'] = True
    except BaseException as exc:
        record.update(failure=repr(exc), traceback=traceback.format_exc())
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        for game in owners:
            try:
                game.close()
                transport = game.client.transport
                row = next(row for row in record['owners'] if row['label'] == game.label)
                row.update(process_id=transport.process_id, stderr=transport.stderr_tail,
                           stderr_truncated=transport.stderr_truncated)
                need(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'],
                     'Owned native process did not exit cleanly: ' + game.label)
            except BaseException as exc:
                record['cleanup_errors'].append(repr(exc))
        try:
            immutable = dict(inputs=all(sha(Path(path)) == digest for path, digest in protected.items()),
                baseline=inventory(baseline) == record['baseline_inventory'])
            if 'copied_content_pins' in record:
                immutable['copied_worlds'] = all(sha(Path(path)) == digest
                    for path, digest in record['copied_content_pins'].items())
                immutable['copied_assets'] = all(inventory(Path(path)) == pins
                    for path, pins in record['copied_asset_pins'].items())
            if 'source_slot_pin' in record:
                immutable['retained_slot'] = inventory(output / 'saves' / ('slot-' + SLOT)) == record['source_slot_pin']
            record['immutable_inputs_on_exit'] = immutable
            need(all(immutable.values()), 'Original inputs/content changed during upgrade verification')
        except BaseException as exc:
            record['cleanup_errors'].append('Immutable verification: ' + repr(exc))
        if record['cleanup_errors']:
            record['passed'] = False
        record.update(elapsed_seconds=time.monotonic() - started, rpc_count=len(record['calls']))
        (output / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        if record['cleanup_errors'] and not active_failure:
            raise AssertionError(record['cleanup_errors'])
    print(json.dumps(dict(passed=record['passed'], checks=len(record['checks']), rpc_count=record['rpc_count'])))


if __name__ == '__main__':
    main()
