#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded retained Relay Yard 85->86 recovery/upgrade on a supplied host/export.

Windows Python, inside switchback_ownership.gated. Caller supplies immutable
retained inputs and an actual target export. No compiler, publisher or GPU is
started here. Original wire requests/replies remain in each owner directory.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import time
import traceback
import uuid

from workflow_components_game_service import (Checked, FIELDS, TREE_CAP, assigned_job,
    contained, exact, manifest, need, pin, read, tree, write)
import relay_yard_upgrade as legacy

SLOT, HEALTHY = 'relay-yard', 'upgraded-relay-yard'
NEW_FIELD, NEW_ID = legacy.NEW_FIELD, legacy.NEW_ID
ROUTE, CONFIG = legacy.driver_module.ROUTE, legacy.driver_module.CONFIG
DRIVER = legacy.driver_module
CORRUPTION = b'Intentional corruption of this test-owned newest Relay Yard checkpoint.\n'


def active_authored_tree(directory):
    """Hash authored content; describe only the exact zero-byte writer lock.

    The source owner holds world.json.lock with exclusive Windows sharing.
    Stat its ordinary-file shape without opening it. Every other file still
    uses pin(), so an unrelated permission error cannot become an omission.
    After all source owners close, the normal tree() reads this sidecar too.
    """
    rows = {}
    for path in sorted(directory.rglob('*')):
        info = path.lstat()
        reparse = bool(getattr(info, 'st_file_attributes', 0) &
                       getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))
        need(not path.is_symlink() and not getattr(path, 'is_junction', lambda: False)()
             and not reparse, 'Linked/reparse authored input: '+str(path))
        relative = path.relative_to(directory).as_posix()
        if relative == 'world.json.lock':
            need(stat.S_ISREG(info.st_mode) and info.st_size == 0 and info.st_nlink == 1,
                 'Writer sidecar must be an ordinary unlinked zero-byte file')
            rows[relative] = dict(bytes=0, type='regular', links=1,
                                  runtime_writer_lock=True, content_opened=False)
        elif stat.S_ISREG(info.st_mode):
            rows[relative] = pin(path)
        else:
            need(stat.S_ISDIR(info.st_mode), 'Nonregular authored input: '+str(path))
    need(sum(row['bytes'] for row in rows.values()) <= TREE_CAP, 'Authored tree exceeds 512 MiB')
    return rows


def descriptor(path):
    path, document, files, image = legacy.artifact(path)
    # The old helper checks every declared member; tree additionally rejects
    # reparse/link members and bounds the complete input closure.
    tree(path.parent)
    return path, document, files, image


def stored(root, slot):
    folder = root / ('slot-' + slot)
    document = read(folder/'current.json')
    row = document['payload']['current']
    need(row['generation'] == document['payload']['generation'], 'Manifest generation join')
    path = contained(folder, row['file'])
    need(pin(path) == dict(bytes=row['bytes'], sha256=row['sha256']), 'Durable payload identity')
    saved = read(path)
    need(saved['format'] == 'poima.world-save' and saved['version'] == 1
         and saved['snapshot']['format'] == 'poima.runtime-snapshot', 'Native save envelope')
    need(legacy.raw_module.raw_digest(path.read_text(encoding='utf-8'), ('snapshot', 'payload'))
         == saved['snapshot']['sha256'], 'Original native snapshot token checksum')
    return saved, path, row


def boundary(owner, result):
    need(result['session_id'] == owner.session and result['tick'] == owner.tick,
         'Observation crossed the requested owner boundary')
    # Only the explicit RPC envelope session differs across fresh owners.
    # No recursive removal of saved ticket epochs, IDs, numbers or values.
    return {key: copy.deepcopy(value) for key, value in result.items() if key != 'session_id'}


class Relay(Checked):
    def __init__(self, *args, descriptor, trusted, **kwargs):
        self.descriptor, self.trusted = descriptor, trusted
        super().__init__(*args, **kwargs)
        self.record = dict(calls=self.calls)

    def module(self):
        result = self.game()
        module = result['module']
        need(module is not None and module['backend'] == 'native_aot', 'Actual NativeAOT module required')
        metadata, image = self.trusted[pin(self.descriptor)['sha256']]
        need(module['assembly_sha256'] == image and exact(module['schema'], metadata['schema'])
             and Path(module['native_library']).resolve() == contained(self.descriptor.parent, metadata['library']),
             'Observed image/schema is not the explicitly trusted artifact')
        return result

    def rpc(self, method, params=None, error=None):
        return self.call(method, params, error)

    def call(self, method, params=None, error=None):
        need(method == 'host.shutdown' or len(self.calls) < 1200,
             'Bounded consumer exceeded 1200 calls in one owner')
        return super().call(method, params, error)

    def inspect(self):
        return self.call('runtime.inspect', dict(session_id=self.session))

    def step(self, ticks=1):
        # Retained spawned roots make a structure guard mandatory. The generic
        # ComponentGame helper has no structural mutations; use the Relay
        # driver's observed guard rather than inheriting that narrower helper.
        state = self.inspect()
        need(state['session_id'] == self.session and state['tick'] == self.tick,
             'Step guard crossed the current owner boundary')
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_structure_revision=state['structure_revision'], ticks=ticks)
        result = self.call('runtime.step', params)
        need(not result['runtime_replaced'] and result['current_session_id'] == self.session
             and result['current_tick'] == self.tick+ticks,
             'Bounded source callback replaced runtime or advanced a different tick count')
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return params, result

    def values(self):
        return self.module()['module']['values']

    def entity(self, identity=DRIVER.ACTOR):
        return self.call('runtime.entity', dict(session_id=self.session, tick=self.tick,
            id=DRIVER.uid(identity) if type(identity) is int else identity))

    def route(self):
        return self.call('runtime.component.get', dict(session_id=self.session, tick=self.tick,
            id=DRIVER.uid(DRIVER.ACTOR), type=ROUTE))

    def visual_config(self):
        return self.call('runtime.component.get', dict(session_id=self.session, tick=self.tick,
            id=DRIVER.uid(DRIVER.ACTOR), type=CONFIG))

    def logical_ui(self):
        result = self.call('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick, limit=256))
        need(result['session_id'] == self.session and result['tick'] == self.tick
             and result['next_after'] is None, 'Logical UI boundary or complete-page limit differs')
        return result

    def control(self, action):
        state, module = self.inspect(), self.module()
        ui = self.logical_ui()
        identity = DRIVER.uid(DRIVER.ACTION_IDS[action])
        selected = [element for element in ui['elements'] if element['id'] == identity]
        need(ui['ui_revision'] == state['ui_revision'] and ui['control_sequence'] == state['control_sequence']
             and len(selected) == 1 and selected[0]['kind'] == 'button'
             and selected[0]['action'] == action.removeprefix('welcome.')
             and selected[0]['eligible'] is True, 'Requested compiled UI action is not currently eligible: '+action)
        result = self.call('runtime.ui.activate', dict(request_id=uuid.uuid4().hex,
            session_id=self.session, expected_tick=self.tick, expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'], expected_gameplay_revision=module['revision'],
            expected_structure_revision=state['structure_revision'], id=identity))
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def guards(self):
        keys = ('expected_session_id', 'expected_tick', 'expected_gameplay_revision',
                'expected_component_revision', 'expected_structure_revision',
                'expected_ui_revision', 'expected_control_sequence', 'expected_material_revision')
        if self.session is None:
            return dict.fromkeys(keys)
        state, game = self.inspect(), self.module()
        registry = self.call('runtime.components', dict(session_id=self.session))
        return dict(expected_session_id=self.session, expected_tick=self.tick,
            expected_gameplay_revision=game['revision'], expected_component_revision=registry['component_revision'],
            **{'expected_'+key: state[key] for key in
               ('structure_revision', 'ui_revision', 'control_sequence', 'material_revision')})

    def load_parameters(self, slot=SLOT, generation=3, plan=None, recovery=False):
        params = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
            slot=slot, expected_generation=generation, revision=self.call('world.inspect')['revision'],
            new_session_id=uuid.uuid4().hex, gameplay=dict(descriptor=str(self.descriptor),
                expected_descriptor_sha256=pin(self.descriptor)['sha256']), **self.guards())
        if recovery:
            params['allow_recovery'] = True
        if plan is not None:
            params['upgrade'] = dict(path=str(plan), expected_sha256=pin(plan)['sha256'])
        return params

    def loaded(self, params):
        result = self.call('save.load', params)
        need(result['session_id'] == params['new_session_id'] and result['session_id'] != self.session,
             'Load did not install its fresh explicitly requested session')
        self.session, self.tick = result['session_id'], result['tick']
        status = self.call('runtime.save.status', dict(session_id=self.session))
        restore = status['last_restore']
        need(restore is not None and restore['generation'] == result['selected_generation']
             and restore['recovered'] is result['recovered'] and restore['restored_tick'] == result['tick'],
             'Selected generation/restore metadata join differs')
        self.module()  # Reestablish actual artifact/schema context after replacement.
        return result

    def saved(self, slot, generation=0):
        guards = self.guards()
        guards['session_id'] = guards.pop('expected_session_id')
        params = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
                      slot=slot, expected_generation=generation, **guards)
        result = self.call('save.write', params)
        saved, path, row = stored(self.expected_save_root, slot)
        need(result['sha256'] == row['sha256'] and result['bytes'] == row['bytes']
             and result['generation'] == generation+1 and not result['replayed'], 'Write/disk join differs')
        self.live_ids = sorted(entity['id'] for entity in saved['snapshot']['payload']['entities'])
        need(len(self.live_ids) == len(set(self.live_ids)) == self.inspect()['entities'],
             'Actual native snapshot membership differs from runtime entity count')
        return saved, path, result

    def snapshot(self, label, record):
        saved, path, result = self.saved('probe-' + uuid.uuid4().hex)
        record.setdefault('native_snapshots', {})[label] = dict(path=str(path),
            identity=pin(path), snapshot=saved['snapshot'], receipt=result)
        return saved, path


def capture(owner, authored):
    state = boundary(owner, owner.inspect())
    ids = owner.live_ids
    need(len(ids) == state['entities'], 'Selected IDs do not cover actual current live membership')
    registry = boundary(owner, owner.call('runtime.components', dict(session_id=owner.session)))
    types = sorted(row['id'] for row in registry['schemas'])
    need(1 <= len(types) <= 4 and 1 <= len(ids) <= 96, 'Bounded complete retained selection required')
    module = boundary(owner, owner.module())
    # A path comparison is made against the trusted descriptor above; this one
    # explicitly identified field differs between source and bundled copies.
    module['module'].pop('native_library')
    pages, joined = [], []
    for start in range(0, len(ids), 32):
        batch = ids[start:start+32]
        page = boundary(owner, owner.call('runtime.observe', dict(session_id=owner.session,
            tick=owner.tick, ids=batch, structure_revision=registry['structure_revision'],
            component_revision=registry['component_revision'], gameplay_revision=module['revision'],
            ui_revision=state['ui_revision'], control_sequence=state['control_sequence'],
            entity_fields=list(FIELDS), components=[dict(type=type_id) for type_id in types], include_schemas=True)))
        need([row['id'] for row in page['entities']] == batch and page['query'] is None,
             'Observation page membership/query differs')
        for key in ('structure_revision', 'component_revision'):
            need(page[key] == registry[key], 'Observation page registry boundary differs')
        need(page['gameplay_revision'] == module['revision']
             and page['ui_revision'] == state['ui_revision']
             and page['control_sequence'] == state['control_sequence'], 'Observation page logical boundary differs')
        if pages:
            need(exact({key:value for key,value in page.items() if key != 'entities'},
                       {key:value for key,value in pages[0].items() if key != 'entities'}),
                 'Observation page metadata/schema changed')
        pages.append(page); joined.extend(page['entities'])
    need([row['id'] for row in joined] == ids, 'Complete joined observation membership differs')
    # One projection contains every retained live entity and every field of
    # each custom type. Cross-check native direct reads on the actual player,
    # courier, rig and cameras without issuing hundreds of redundant RPCs.
    entities = [boundary(owner, owner.entity(identity)) for identity in
                (DRIVER.PLAYER, 101, 102, DRIVER.ACTOR, DRIVER.RIG)]
    components = [boundary(owner, owner.call('runtime.component.get', dict(session_id=owner.session,
        tick=owner.tick, id=DRIVER.uid(DRIVER.ACTOR), type=type_id,
        structure_revision=registry['structure_revision']))) for type_id in types]
    for entity in entities:
        row = next(item for item in joined if item['id'] == entity['id'])
        need(exact(row['state'], {key: entity[key] for key in FIELDS}), 'Direct/projected entity mismatch')
    actor = next(row for row in joined if row['id'] == DRIVER.uid(DRIVER.ACTOR))
    for direct in components:
        need(exact(actor['components'][direct['type']], direct['values']), 'Direct/projected component mismatch')
    ui = boundary(owner, owner.call('runtime.ui.inspect', dict(session_id=owner.session, tick=owner.tick, limit=256)))
    need(ui['next_after'] is None, 'Complete UI exceeds bounded page')
    queue = owner.call('runtime.save.status', dict(session_id=owner.session))
    need(queue['enabled'] and queue['pending'] is None, 'Unexpected disabled or pending save routing')
    return dict(session_id=owner.session, selected=dict(runtime=state, registry=registry, gameplay=module,
        entities=entities, components=components, observation_pages=pages, observation_entities=joined, ui=ui), queue=queue,
        routing=owner.call('save.status'), authored=active_authored_tree(authored), files=tree(owner.expected_save_root))


def legacy_reconstruction(original, observed, world):
    """Narrow v5 root-prop -> current camera records, with original types exact."""
    old, new = original['payload'], observed['payload']
    need(original['version'] == 5 and observed['version'] == 7, 'Unreviewed legacy reconstruction format')
    expected = copy.deepcopy(old)
    expected['cameras'] = []
    for identity, entity in sorted(world['entities'].items()):
        camera = entity['components'].get('Camera')
        if camera is not None:
            need(set(camera) == {'near', 'far', 'vertical_fov'}, 'Unreviewed retained camera fields')
            expected['cameras'].append(dict(entity=identity,
                world={key: float(camera[key]) for key in ('vertical_fov', 'near', 'far')}, viewmodel=None))
    for birth in expected['structure']['spawned']:
        template = world['templates'][birth['template_id']]
        need('entities' not in template or not template['entities'], 'Legacy edge must use root-only templates')
        birth['nodes'] = {birth['template_id']: birth['id']}
    need(exact(new, expected), 'Retained v5 fields/types changed or unapproved reconstructed v7 fields')
    return dict(source_version=5, observed_version=7, preserved_original_fields=True,
        additions=['payload.cameras from frozen Camera components',
                   'payload.structure.spawned[*].nodes singleton template/root maps'],
        scope='Explicit current-format reconstruction; original v5 and observed v7 remain verbatim in evidence.')


def make_plan(original, original_path, target, target_path, source_image, target_image):
    source, destination = legacy.identity(original, original_path), legacy.identity(target, target_path)
    old_schema = original['snapshot']['payload']['gameplay']['schema']
    schema = target['snapshot']['payload']['gameplay']['schema']
    persistence = schema['persistent']
    need(source['image_sha256'] == source_image and destination['image_sha256'] == target_image
         and source_image != target_image and source['content_sha256'] == destination['content_sha256'],
         'Global-only actual image/content identities differ')
    fields = {row['name']: row for row in persistence['fields']}
    old_values = original['snapshot']['payload']['gameplay']['values']
    need(persistence['revision'] == 2 and len(old_schema['fields']) == 49 and len(schema['fields']) == 50
         and set(fields) == set(old_values) | {NEW_FIELD}
         and fields[NEW_FIELD]['id'] == NEW_ID and fields[NEW_FIELD]['kind'] == 'int32'
         and fields[NEW_FIELD]['default'] == 2, 'Actual target persistence metadata differs')
    for field in old_schema['fields']:
        need(fields[field['name']]['kind'] == field['kind'], 'Original scalar kind changed')
    legacy_ids = sorted([dict(name=row['name'], id=fields[row['name']]['id'])
                         for row in old_schema['fields']], key=lambda row: row['id'])
    plan = dict(format='poima.save-upgrade', version=1, id=uuid.uuid4().hex,
        source=source, target=destination, legacy_global_ids=legacy_ids, components=[],
        **{'global':dict(preserve=[row['id'] for row in legacy_ids], retire=[], default=[NEW_ID])})
    return plan, schema


def upgrade_report(receipt, plan, plan_hash):
    report = receipt['upgrade']
    need(report['plan_id'] == plan['id'] and report['plan_sha256'] == plan_hash,
         'Upgrade did not bind exact reviewed plan bytes')
    for side in ('source', 'target'):
        for key in ('content_sha256', 'image_sha256', 'schema_sha256'):
            need(report[side+'_'+key] == plan[side][key], 'Upgrade report identity join: '+side+':'+key)
    need(report['preserved_globals'] == 49 and report['defaulted_globals'] == 1
         and report['retired_globals'] == 0 and report['components'] == [], '49/1/0 global-only report differs')


def lifecycle_globals(status):
    """Predict only the three fields Relay Restored() reads from this owner."""
    epoch = status['epoch']
    need(type(epoch) is str and len(epoch) == 32 and epoch != legacy.ZERO
         and all(char in '0123456789abcdef' for char in epoch), 'Actual lifecycle epoch is not a nonzero ID')
    need(status['enabled'] is True and status['pending'] is None and status['last_restore'] is not None,
         'Continuation requires actual enabled, idle routing and a completed restore')
    generation = status['last_restore']['generation']
    need(type(generation) is int and 1 <= generation < 2**63, 'Relay restore generation exceeds signed int64')
    halves = [int(epoch[start:start+16], 16) for start in (0, 16)]
    signed = [str(value if value < 2**63 else value-2**64) for value in halves]
    return dict(ObservedEpochHigh=signed[0], ObservedEpochLow=signed[1], LastSaveGeneration=str(generation))


def expected_cold_continuation(warm_selected, warm_payload, cold_lifecycle):
    """Copy untouched warm state and replace three explicit lifecycle globals."""
    names = {'ObservedEpochHigh', 'ObservedEpochLow', 'LastSaveGeneration'}
    need(set(cold_lifecycle) == names and all(type(value) is str for value in cold_lifecycle.values()),
         'Lifecycle expectation must contain exactly three signed int64 strings')
    selected, payload = copy.deepcopy(warm_selected), copy.deepcopy(warm_payload)
    selected_values = selected['gameplay']['module']['values']
    payload_values = payload['gameplay']['values']
    need(names <= selected_values.keys() and names <= payload_values.keys(), 'Missing Relay lifecycle globals')
    for name in names:
        selected_values[name] = cold_lifecycle[name]
        payload_values[name] = cold_lifecycle[name]
    return selected, payload


def continuation(owner, record, collected=2):
    driver = DRIVER.RelayDriver(owner, record)
    before = copy.deepcopy(owner.values())
    save_status = owner.call('runtime.save.status', dict(session_id=owner.session))
    expected_lifecycle = lifecycle_globals(save_status)
    initial_ui = owner.logical_ui()
    need(initial_ui['modal'] in (None, DRIVER.uid(920)), 'Continuation encountered an unreviewed logical modal')
    actions = (['menu'] if initial_ui['modal'] is None else []) + ['refresh', 'close']
    for action in actions:
        driver.control(action)  # Owner validates current native eligibility before every Control.
    need(owner.logical_ui()['modal'] is None, 'Compiled close did not release the actual menu modal')
    driver.step(1)
    after = owner.values()
    need(after[NEW_FIELD] == 2 and after['Ticks'] == before['Ticks']+1
         and after['Collected'] == collected and after['Started'] == 1 and after['Phase'] == 0
         and after['SavePending'] == 0, 'Actual target Control/Tick continuation differs')
    need(exact({name:after[name] for name in expected_lifecycle}, expected_lifecycle),
         'Compiled Restored did not report this actual owner epoch/restore generation as signed int64 strings')
    need(exact(owner.call('runtime.save.status', dict(session_id=owner.session)), save_status),
         'Bounded continuation changed save epoch/restore metadata or submitted another operation')
    return dict(before=before, after=copy.deepcopy(after), initial_modal=initial_ui['modal'], actions=actions,
                actual_save_status=save_status, expected_lifecycle_globals=expected_lifecycle)


def qualify(args, report, opened, deadline):
    baseline = args.baseline
    history = legacy.read(baseline/'evidence.json')  # Retained evidence is >16 MiB.
    need(history['passed'] is True and history['format'] == 'poima.relay-yard-contract'
         and all(history['immutable_inputs_on_exit'].values()), 'Actual passing retained baseline required')
    source_descriptor, sd, source_files, source_image = descriptor(args.source_artifact)
    target_descriptor, td, target_files, target_image = descriptor(args.target_artifact)
    world = baseline/'world.json'
    need(pin(world)['sha256'] == history['immutable_world']['sha256']
         and tree(Path(str(world)+'.assets')) == history['immutable_world']['assets']
         and tree(baseline/'saves'/('slot-'+SLOT)) == history['durable_checkpoint_pin'],
         'Retained authored/cooked/durable closure differs from historical run')
    original, original_path, checkpoint_entry = legacy.stored(baseline/'saves', SLOT)
    old = original['snapshot']['payload']
    need(checkpoint_entry['generation'] == 2 and old['tick'] == 567 and len(old['gameplay']['schema']['fields']) == 49
         and old['gameplay']['assembly_sha256'] == source_image
         and exact(old['gameplay']['values'], history['checkpoint']['saved']['values'])
         and old['gameplay']['values']['Collected'] == 1 and old['gameplay']['values']['Moving'] == 0,
         'Retained stationary one-cell checkpoint identity/state differs')
    report['retained_checkpoint'] = dict(identity=pin(original_path), snapshot=original['snapshot'],
        descriptor=pin(source_descriptor), image_sha256=source_image)
    owned = args.output/'owned-source'
    owned.mkdir()
    owned_world = owned/'world.json'
    shutil.copy2(world, owned_world)
    shutil.copytree(Path(str(world)+'.assets'), Path(str(owned_world)+'.assets'))
    shutil.copytree(source_descriptor.parent, owned/'artifact')
    owned_descriptor = owned/'artifact'/'native-gameplay.json'
    owned_before = tree(owned)
    storage = args.output/'saves'
    shutil.copytree(baseline/'saves', storage)
    initial_slot = tree(storage/('slot-'+SLOT))
    exported = read(args.game)
    bundled_binary = contained(args.game.parent, exported['engine']['executable'])
    game_descriptor = contained(args.game.parent, exported['gameplay']['descriptor'])
    _, gd, _, game_image = descriptor(game_descriptor)
    need(exact(gd, td) and game_image == target_image, 'Actual bundled target artifact differs')
    # Validate the entire declared export closure, including license records;
    # no inference from executable existence or a copied descriptor alone.
    declared = {'game.json'}
    for exported_entry in exported['files']:
        need(exported_entry['path'].casefold() not in {name.casefold() for name in declared}, 'Duplicate export file')
        member = contained(args.game.parent, exported_entry['path'])
        need(pin(member) == dict(bytes=exported_entry['size'], sha256=exported_entry['sha256']), 'Declared export member differs')
        declared.add(exported_entry['path'])
    need(set(tree(args.game.parent)) == declared, 'Unexpected/unlisted exported content')
    report['bundled_binary'] = dict(path=exported['engine']['executable'], **pin(bundled_binary))
    trusted = {pin(owned_descriptor)['sha256']:(sd,source_image), pin(game_descriptor)['sha256']:(gd,target_image)}
    report['trusted_schemas'] = {source_image:sd['schema'], target_image:gd['schema']}

    def owner(label, readonly=False):
        game = Relay(bundled_binary if readonly else args.binary, args.output/label, deadline,
            world=None if readonly else owned_world, game=args.game if readonly else None,
            saves=storage, descriptor=game_descriptor if readonly else owned_descriptor, trusted=trusted)
        opened.append(game)
        return game

    def close(game, label):
        game.close(); opened.remove(game)
        report['owners'].append(dict(name=label, calls=len(game.calls), validated_successes=game.validated,
            returncode=game.transport.returncode, host_returncode=None if game.host is None else game.host.returncode))

    def capture_now(game):
        return capture(game, owned)

    def reject(game, label, params, code, marker):
        before = capture_now(game)
        error = game.call('save.load', params, error=code)
        need(marker in error['message'], 'Rejection has unrelated cause: '+label)
        after = capture_now(game)
        need(exact(before, after), 'Rejected load changed selected state/queue/routing/authored/store: '+label)
        report['rejections'][label] = dict(request=params, error=error, before=before, after=after)

    source = owner('retained-source')
    admission = source.loaded(source.load_parameters(generation=2))
    need(admission['tick'] == 567 and not admission['recovered'], 'Healthy retained source did not exact-load')
    restored_source, restored_path = source.snapshot('retained-source-current-format', report)
    report['legacy_reconstruction'] = legacy_reconstruction(original['snapshot'], restored_source['snapshot'], read(owned_world))
    need(exact(source.values(), old['gameplay']['values']), 'Any original global changed before first callback')
    report['source_before_callback'] = capture_now(source)
    third, third_path, third_result = source.saved(SLOT, 2)
    need(third_result['generation'] == 3 and exact(third['snapshot'], restored_source['snapshot']), 'Legitimate guarded gen3 changed state')
    need(tree(storage/('slot-'+SLOT)) != initial_slot, 'Legitimate write did not publish generation3')
    manifest_path = storage/('slot-'+SLOT)/'current.json'
    manifest_before = pin(manifest_path)
    third_path.write_bytes(CORRUPTION)
    need(pin(manifest_path) == manifest_before, 'Corruption changed manifest instead of owned newest payload')
    corrupt_slot = tree(storage/('slot-'+SLOT))
    inspected = source.call('save.inspect', dict(slot=SLOT))
    need(inspected['generation'] == inspected['current']['generation'] == 3
         and inspected['current']['verified'] is False and inspected['previous']['generation'] == 2
         and inspected['selected']['generation'] == 2 and inspected['selected']['verified'] is True
         and inspected['selected']['sha256'] == checkpoint_entry['sha256'] and inspected['recovered'] is True
         and inspected['manifest_recovered'] is False and bool(inspected['recovery_reason']), 'Actual previous-payload recovery differs')
    report['corruption'] = dict(write=third_result, corrupted_payload=pin(third_path),
        fixed_bytes_utf8=CORRUPTION.decode(), manifest=manifest_before, inspection=inspected, files=corrupt_slot)
    reject(source, 'recovery-consent-source', source.load_parameters(), -32070, 'Save recovery requires allow_recovery:true')
    close(source, 'retained-source')

    recovered = owner('recovered-source')
    request = recovered.load_parameters(recovery=True)
    receipt = recovered.loaded(request)
    need(receipt['generation'] == 3 and receipt['selected_generation'] == 2 and receipt['recovered'] is True
         and receipt['tick'] == 567, 'Source recovered-load generation/header differs')
    recovered_native, _ = recovered.snapshot('recovered-source-before-callback', report)
    need(exact(recovered_native['snapshot'], restored_source['snapshot']), 'Recovered source current-format state changed')
    report['recovered_source_before_callback'] = capture_now(recovered)
    recovered.step(1)
    need(recovered.values()['Ticks'] == old['gameplay']['values']['Ticks']+1, 'Real recovered source Tick did not run')
    before = capture_now(recovered)
    need(exact(recovered.call('save.load', request), dict(receipt, replayed=True)), 'Historical recovery receipt changed')
    need(exact(before, capture_now(recovered)), 'Historical recovered load replaced runtime or reread store')
    report['source_recovery_retry'] = dict(request=request, receipt=receipt, unchanged=True)
    close(recovered, 'recovered-source')

    target = owner('readonly-target', True)
    target_native, target_path = target.snapshot('ordinary-target-metadata', report)
    need(target.values()[NEW_FIELD] == 2 and target.values()['Initialized'] == 0, 'Target ordinary initialization differs')
    plan, target_schema = make_plan(original, original_path, target_native, target_path, source_image, target_image)
    plan_path = args.output/'approved-upgrade.json'
    write(plan_path, plan)
    plan_pin, plan_bytes = pin(plan_path), plan_path.read_bytes()
    report['plan'], report['plan_identity'] = plan, plan_pin
    reject(target, 'recovery-consent-target-plan', target.load_parameters(plan=plan_path), -32070,
           'Save recovery requires allow_recovery:true')
    reject(target, 'exact-changed-image', target.load_parameters(recovery=True), -32070,
           'Snapshot gameplay module/schema differs from the trusted module.')
    wrong = target.load_parameters(plan=plan_path, recovery=True)
    wrong['upgrade']['expected_sha256'] = '0'*64
    reject(target, 'wrong-plan-hash', wrong, -32070, 'Save upgrade plan byte hash differs.')
    stale = target.load_parameters(plan=plan_path, recovery=True)
    stale['expected_gameplay_revision'] += 1
    reject(target, 'stale-typed-guard', stale, -32009, 'Runtime replacement clock/revision conflict.')
    upgrade_request = target.load_parameters(plan=plan_path, recovery=True)
    upgrade_receipt = target.loaded(upgrade_request)
    need(upgrade_receipt['generation'] == 3 and upgrade_receipt['selected_generation'] == 2
         and upgrade_receipt['recovered'] is True and upgrade_receipt['tick'] == 567, 'Recovered upgrade receipt differs')
    upgrade_report(upgrade_receipt, plan, plan_pin['sha256'])
    upgraded, _ = target.snapshot('upgraded-before-callback', report)
    expected = copy.deepcopy(restored_source['snapshot']['payload'])
    expected['gameplay'] = dict(backend=plan['target']['backend'], assembly_sha256=target_image,
        type=plan['target']['type'], schema=target_schema, values=dict(old['gameplay']['values'], CheckpointVersion=2))
    need(upgraded['snapshot']['version'] == restored_source['snapshot']['version']
         and exact(upgraded['snapshot']['payload'], expected), 'Upgrade changed fields outside approved global image/schema/default map')
    report['upgraded_before_callback'] = capture_now(target)
    report['upgrade_receipt'] = upgrade_receipt
    plan_path.unlink()
    before_retry = capture_now(target)
    replay = target.call('save.load', upgrade_request)
    need(exact(replay, dict(upgrade_receipt, replayed=True)) and exact(capture_now(target), before_retry)
         and not plan_path.exists(), 'Historical retry reread missing plan, changed receipt or replaced runtime')
    changed = dict(upgrade_request, new_session_id=uuid.uuid4().hex)
    reject(target, 'altered-request-id-reuse', changed, -32010, 'Save request ID reused with different parameters.')
    report['removed_plan_retry'] = dict(request=upgrade_request, receipt=replay, unchanged=True)
    # Keep the approved bytes as a distinct retained evidence file, not at the
    # requested path: retries above genuinely encountered an absent plan.
    (args.output/'approved-upgrade.retained.json').write_bytes(plan_bytes)
    report['first_target_callbacks'] = continuation(target, report, collected=1)
    after_callbacks = capture_now(target)
    historical_after_callbacks = target.call('save.load', upgrade_request)
    need(exact(historical_after_callbacks, dict(upgrade_receipt, replayed=True))
         and exact(capture_now(target), after_callbacks) and not plan_path.exists(),
         'Historical upgrade retry reapplied old state after real target callbacks')
    report['removed_plan_after_callbacks_retry'] = dict(receipt=historical_after_callbacks, unchanged=True)
    pickup_id = old['gameplay']['values']['CellTwo']
    need(pickup_id in target.live_ids and target.values()['CellTwo'] == pickup_id,
         'Remaining retained second cell is not alive before semantic pickup')
    driver = DRIVER.RelayDriver(target, report)
    driver.drive_to(*DRIVER.CELL_APPROACHES[1], 'upgraded-second-cell', max_iterations=24)
    pickup_values = driver.use_target(pickup_id)
    need(pickup_values['Collected'] == 2 and pickup_values['CellTwo'] == legacy.ZERO
         and pickup_values['CellThree'] == old['gameplay']['values']['CellThree']
         and pickup_values[NEW_FIELD] == 2, 'Actual camera-ray/Use did not collect exactly the retained second cell')
    missing_cell = target.call('runtime.entity', dict(session_id=target.session, tick=target.tick,
        id=pickup_id), error=-32004)
    need(missing_cell['message'] == 'Runtime entity does not exist.', 'Collected entity was not actually retired')
    expected_membership = sorted(identity for identity in target.live_ids if identity != pickup_id)
    saved_target, saved_path, healthy = target.saved(HEALTHY)
    need(target.live_ids == expected_membership and len(target.live_ids) == 73,
         'Actual pickup snapshot did not retire exactly one retained cell')
    report['actual_pickup'] = dict(target=pickup_id, values=copy.deepcopy(pickup_values),
        missing_entity_error=missing_cell, saved_live_ids=target.live_ids,
        scope='Real semantic controller movement, actual camera ray and compiled Use; no synthetic alive:false RPC.')
    healthy_inspection = target.call('save.inspect', dict(slot=HEALTHY))
    need(healthy_inspection['generation'] == 1 and healthy_inspection['selected']['verified']
         and not healthy_inspection['recovered'], 'New target slot is not healthy generation1')
    saved_selected = capture_now(target)['selected']
    report['healthy_target'] = dict(identity=pin(saved_path), snapshot=saved_target['snapshot'], receipt=healthy)
    report['warm_continuation'] = continuation(target, report)
    warm_selected = capture_now(target)['selected']
    warm_native, _ = target.snapshot('warm-continuation', report)
    close(target, 'readonly-target')

    cold = owner('readonly-cold', True)
    cold_reply = cold.loaded(cold.load_parameters(HEALTHY, 1))
    need(not cold_reply['recovered'] and 'upgrade' not in cold_reply, 'Healthy cold load unexpectedly recovered/upgraded')
    cold_native, _ = cold.snapshot('cold-restored', report)
    need(exact(cold_native['snapshot'], saved_target['snapshot'])
         and exact(capture_now(cold)['selected'], saved_selected), 'Fresh bundled owner did not exactly restore target state')
    report['cold_continuation'] = continuation(cold, report)
    cold_continued, _ = cold.snapshot('cold-continuation', report)
    expected_cold_selected, expected_cold_payload = expected_cold_continuation(warm_selected,
        warm_native['snapshot']['payload'], report['cold_continuation']['expected_lifecycle_globals'])
    need(exact(report['cold_continuation']['actions'], report['warm_continuation']['actions'])
         and exact(capture_now(cold)['selected'], expected_cold_selected)
         and exact(cold_continued['snapshot']['format'], warm_native['snapshot']['format'])
         and exact(cold_continued['snapshot']['version'], warm_native['snapshot']['version'])
         and exact(cold_continued['snapshot']['payload'], expected_cold_payload),
         'Cold callback continuation differs outside independently checked owner lifecycle globals')
    report['cold_warm_lifecycle_comparison'] = dict(
        replaced_globals=report['cold_continuation']['expected_lifecycle_globals'],
        expected_selected=expected_cold_selected, expected_native_payload=expected_cold_payload,
        warm_native_payload_sha256=warm_native['snapshot']['sha256'],
        cold_native_payload_sha256=cold_continued['snapshot']['sha256'],
        scope='Exact typed comparison after replacing only ObservedEpochHigh, ObservedEpochLow and LastSaveGeneration '
              'in untouched warm copies from actual cold runtime.save.status. Original warm/cold values remain retained; '
              'each raw native payload checksum was independently verified by stored(). Precallback cold restore remains full exact.')
    close(cold, 'readonly-cold')
    need(tree(storage/('slot-'+SLOT)) == corrupt_slot, 'Recovery/upgrade/continuation rewrote original corrupt recovery slot')
    final_owned = tree(owned)
    need(all(final_owned.get(name) == value for name, value in owned_before.items()), 'Owned source input drift')
    extras = set(final_owned)-set(owned_before)
    need(extras <= {'world.json.lock'} and all(final_owned[name]['bytes'] == 0 for name in extras), 'Unexpected owned source sidecar')
    report['owned_source_sidecars'] = {name:final_owned[name] for name in extras}
    report['closed_owned_source_inventory'] = final_owned
    report['recovery_slot_unchanged'], report['cold_restore_and_continuation'] = True, True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'baseline', 'source-artifact', 'target-artifact', 'game', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Windows Python without optimization required')
    need(30 <= args.timeout <= 600, 'Timeout must be30..600 seconds')
    assigned_job()
    args.binary, args.baseline, args.output = args.binary.resolve(), args.baseline.resolve(), args.output.resolve()
    args.game = manifest(args.game, 'game.json')
    args.source_artifact = manifest(args.source_artifact, 'native-gameplay.json')
    args.target_artifact = manifest(args.target_artifact, 'native-gameplay.json')
    input_roots = dict(baseline=args.baseline, source_artifact=args.source_artifact.parent,
                       target_artifact=args.target_artifact.parent, game=args.game.parent)
    need(not args.output.exists() and all(not args.output.is_relative_to(path) for path in input_roots.values()),
         'Fresh independent output outside all original inputs required')
    originals = {name:tree(path) for name,path in input_roots.items()}
    args.output.mkdir(parents=True)
    started, opened = time.monotonic(), []
    report = dict(format='poima.workflow-save-upgrade.v1', passed=False, harness=pin(Path(__file__)),
        source_binary=pin(args.binary), original_inputs=originals, owners=[], rejections={}, errors=[], limits=[
            'One retained85->86 NativeAOT global-only edge:49 preserved/default1/retired0, one owned current-payload corruption and verified previous-payload recovery. No manifest recovery, acknowledged recovery write, quarantine, hierarchical/array/new-type upgrade or universal compatibility claim.',
            'Legacy v5 to current v7 reconstruction is an explicit frozen-camera and singleton spawned-node addition oracle, not full byte equality across snapshot versions. Current source/target native snapshots are compared fully except the approved gameplay image/schema/default map.',
            'Complete bounded retained live membership/components/entity/UI selection and queue/routing/storage invariance for denial/retry checks. Save ledger epochs are compared only within the same owner, never erased recursively.',
            'Cold before-callback restore is full exact. Postcallback cold/warm equality allows exactly three Relay Restored globals independently derived from each actual owner epoch/restore generation: ObservedEpochHigh/Low signed-int64 decimal strings and LastSaveGeneration. Raw values/checksums remain intact; every other selected/native payload field is type-exact.',
            'During active source ownership only the exact regular non-link zero-byte world.json.lock is represented by existence/type/size without opening its exclusively held handle. Every authored content/store file retains byte/hash checks; the full owned source tree, including sidecar bytes/hash, is read after source owners close.',
            'Bounded real Relay movement, actual camera ray/Use and one remaining pickup, plus Control/Tick and native animation-rate observations; not the prior full courier/terminal completion campaign.',
            'No compiler/provider/publisher/GPU/render/audio listening/performance/clean-machine/source-absence qualification; natural closure requires the separate supervising Job receipt.'])
    try:
        qualify(args, report, opened, started+args.timeout)
        need({name:tree(path) for name,path in input_roots.items()} == originals, 'Original caller inputs changed')
        report['original_inputs_unchanged'], report['passed'] = True, True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        for owner in reversed(opened):
            try:
                owner.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
                report['passed'] = False
            finally:
                report['owners'].append(dict(name=owner.directory.name, calls=len(owner.calls),
                    validated_successes=owner.validated, returncode=owner.transport.returncode,
                    host_returncode=None if owner.host is None else owner.host.returncode,
                    interrupted_sequence=True))
        report['seconds'] = time.monotonic()-started
        report['native_calls'] = sum(owner['calls'] for owner in report['owners'])
        report['candidate_validated_successes'] = sum(owner['validated_successes'] for owner in report['owners'])
        # Even a rejected/incompatible retained edge preserves inputs and raw
        # records; never reinterpret failure as permission for a new baseline.
        try:
            report['original_inputs_on_exit'] = {name:tree(path) for name,path in input_roots.items()}
            need(report['original_inputs_on_exit'] == originals, 'Original input drift on exit')
        except BaseException:
            report['errors'].append(traceback.format_exc()); report['passed'] = False
        write(args.output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], seconds=report['seconds'], errors=len(report['errors']))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
