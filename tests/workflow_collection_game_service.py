#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""All-five-kind collection consumer over actual NativeAOT source/readonly games.

Explicit trusted adopted project, exported bundle and fresh external output.
Run inside switchback_ownership.gated on Windows. No compile, rendering, physical
input, internal-snapshot equivalence or broader stable API qualification.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import sys
import time
import traceback
import uuid

from workflow_components_game_service import (Checked, FIELDS, artifact, assigned_job,
    boundary, contained, exact, manifest, module_payload, need, pin, read, tree, write)

TYPE = '7c0e14507c0e14507c0e14507c0e1450'
IDENTITY = 'poima.test.collection-wire'
FIELD_IDS = [f'{0x14501+i:032x}' for i in range(6)]
OWNER, OTHER = f'{1:032x}', f'{2:032x}'
KINDS = ('int32', 'int64', 'float32', 'float64', 'entity')
KNOWN = dict(zip(FIELD_IDS[:5], ([(-2**31), 2**31-1], [str(-2**63), str(2**63-1)],
                               [0.5, -65536.0], [1e-200, -1e200], [OWNER, OTHER])))
EDITED = dict(zip(FIELD_IDS, ([0, -7], ['9007199254740993', '-9007199254740993'],
                            [1.25, -2.5], [-1e-100, 1e100], ['0'*32, OWNER], 7)))


def checkpoint(owner, identities, descriptor):
    runtime = owner.call('runtime.inspect', dict(session_id=owner.session))
    registry = owner.call('runtime.components', dict(session_id=owner.session))
    game = owner.game()
    need(len(registry['schemas']) == 1 and registry['schemas'][0]['id'] == TYPE,
         'Collection fixture must have exactly its nonempty registry')
    schema = registry['schemas'][0]
    need(schema['version'] == 2 and [f['id'] for f in schema['fields']] == FIELD_IDS, 'Generated schema version/field IDs differ')
    for field, kind in zip(schema['fields'][:5], KINDS):
        need(field['kind'] == 'array' and field['element_kind'] == kind and field['capacity'] == 2 and field['default'] == [],
             'Generated array kind/capacity/default differs')
    need(schema['fields'][5]['kind'] == 'int32' and schema['fields'][5]['default'] == 0, 'Trailing scalar schema differs')
    rows, instances = [], []
    for identity in identities:
        rows.append(boundary(owner, owner.call('runtime.entity', dict(session_id=owner.session, tick=owner.tick, id=identity))))
        instances.append(boundary(owner, owner.call('runtime.component.get', dict(session_id=owner.session,
            tick=owner.tick, id=identity, type=TYPE, structure_revision=registry['structure_revision']))))
    observed = owner.call('runtime.observe', dict(session_id=owner.session, tick=owner.tick, ids=identities,
        structure_revision=registry['structure_revision'], component_revision=registry['component_revision'],
        entity_fields=list(FIELDS), components=[dict(type=TYPE)], include_schemas=True))
    need([r['id'] for r in observed['entities']] == identities, 'Observation lost full selected membership')
    for row in observed['entities']:
        instance = next(r for r in instances if r['id'] == row['id'])
        entity = next(r for r in rows if r['id'] == row['id'])
        need(exact(row['components'][TYPE], instance['values']), 'Observation array/scalar values differ from complete read')
        need(exact(row['state'], {key: entity[key] for key in FIELDS}), 'Observation entity projection differs from full selected state')
    ui = owner.call('runtime.ui.inspect', dict(session_id=owner.session, tick=owner.tick, limit=256))
    need(ui['next_after'] is None, 'Fixture UI exceeds complete bounded page')
    gameplay = boundary(owner, game)
    gameplay['module'] = module_payload(game['module'], descriptor)
    result = dict(runtime=boundary(owner, runtime), registry=boundary(owner, registry), gameplay=gameplay,
                  entities=rows, components=instances, observation=boundary(owner, observed), ui=boundary(owner, ui))
    queue = owner.call('runtime.save.status', dict(session_id=owner.session))
    need(queue['enabled'] and queue['configuration_generation'] == owner.configuration and queue['pending'] is None,
         'Unexpected save routing/pending operation')
    return result


def values(state, entity):
    return next(row['values'] for row in state['components'] if row['id'] == entity)


def members(state):
    return [row['id'] for row in state['components'] if row['values'] is not None]


def capture(owner, identities, descriptor):
    return dict(checkpoint=checkpoint(owner, identities, descriptor), save_routing=owner.call('save.status'),
                save_queue=owner.call('runtime.save.status', dict(session_id=owner.session)), save_files=tree(owner.expected_save_root))


def pages(owner, state):
    registry = state['registry']
    collected, cursor = [], None
    for _ in range(32):
        params = dict(session_id=owner.session, tick=owner.tick, type=TYPE, limit=1,
                      structure_revision=registry['structure_revision'])
        if cursor is not None:
            params['after'] = cursor
        result = owner.call('runtime.component.query', params)
        collected.extend(result['entities'])
        cursor = result['next_after']
        if cursor is None:
            break
    else:
        raise AssertionError('Collection query exceeded bounded pages')
    need(collected == members(state), 'Sorted query changed component membership')
    absent = next(row['id'] for row in state['components'] if row['values'] is None)
    params = dict(session_id=owner.session, tick=owner.tick, ids=[absent], query=dict(type=TYPE, limit=1),
                  entity_fields=list(FIELDS), components=[dict(type=TYPE)], include_schemas=True)
    collected, cursor = [], None
    for _ in range(32):
        if cursor is not None:
            params['query']['after'] = cursor
            params.update(structure_revision=registry['structure_revision'], component_revision=registry['component_revision'])
        result = owner.call('runtime.observe', params)
        for row in result['entities']:
            reference = next(r for r in state['observation']['entities'] if r['id'] == row['id'])
            need(exact(row, reference), 'Continuation changed full typed collection values')
        collected.extend(result['query']['entities'])
        cursor = result['query']['next_after']
        if cursor is None:
            break
    else:
        raise AssertionError('Joined collection query exceeded bounded pages')
    need(collected == members(state), 'Joined query lost or duplicated membership')
    # A compact selected-field read remains typed with the prior registry context.
    projected = owner.call('runtime.observe', dict(session_id=owner.session, tick=owner.tick,
        ids=[OWNER, absent], entity_fields=[], components=[dict(type=TYPE, fields=[FIELD_IDS[1], FIELD_IDS[4]])]))
    for row in projected['entities']:
        expected = values(state, row['id'])
        expected = None if expected is None else {field: expected[field] for field in (FIELD_IDS[1], FIELD_IDS[4])}
        need(row['state'] == {} and exact(row['components'][TYPE], expected), 'Compact long/entity selector changed wire types')


def edit_request(owner, state, entity, contents):
    return dict(session_id=owner.session, request_id=uuid.uuid4().hex, expected_tick=owner.tick,
        expected_revision=state['registry']['component_revision'], expected_structure_revision=state['registry']['structure_revision'],
        id=entity, type=TYPE, values=copy.deepcopy(contents))


def edit_checks(owner, identities, descriptor, prior):
    need(values(prior, OTHER) is not None, 'Author a second collection member for external edits')
    request = edit_request(owner, prior, OTHER, EDITED)
    changed = owner.call('runtime.component.edit', request)
    need(changed['component_revision'] == prior['registry']['component_revision']+1 and exact(changed['values'], EDITED),
         'Valid native component edit changed typed array values or revision')
    edited = checkpoint(owner, identities, descriptor)
    need(exact(values(edited, OTHER), EDITED), 'Published valid array edit differs')
    length_states = {'filled': edited}
    for label, contents in (
            ('shortened', {**{field: [EDITED[field][-1]] for field in FIELD_IDS[:5]}, FIELD_IDS[5]: 8}),
            ('cleared', {**{field: [] for field in FIELD_IDS[:5]}, FIELD_IDS[5]: 9}),
            ('refilled', EDITED)):
        params = edit_request(owner, edited, OTHER, contents)
        previous_revision = edited['registry']['component_revision']
        result = owner.call('runtime.component.edit', params)
        need(result['component_revision'] == previous_revision+1 and exact(result['values'], contents),
             'Native array length edit changed values/revision: ' + label)
        edited = checkpoint(owner, identities, descriptor)
        need(exact(values(edited, OTHER), contents) and edited['registry']['component_revision'] == previous_revision+1,
             'Short/empty/refilled published array state differs: ' + label)
        length_states[label] = edited
    before_retry = capture(owner, identities, descriptor)
    replay = owner.call('runtime.component.edit', request)
    need(exact(replay, dict(changed, replayed=True)) and exact(capture(owner, identities, descriptor), before_retry),
         'Historical array edit with stale original guards changed its retained result or current state')
    failures = []
    cases = [('capacity', FIELD_IDS[0], [0, 1, 2], 'Component array value exceeds its capacity'),
             ('int32_boolean', FIELD_IDS[0], [True], 'Component int32 value is out of range.'),
             ('int64_numeric', FIELD_IDS[1], [9007199254740993], 'Component int64 values require canonical decimal strings.'),
             ('float32_overflow', FIELD_IDS[2], [1e100], 'Component float32 overflow.'),
             ('float64_boolean', FIELD_IDS[3], [True], 'Component floating value must be numeric.'),
             ('entity_last_unresolved', FIELD_IDS[4], [OWNER, 'f'*32], 'Component entity reference does not resolve')]
    for label, field, invalid, marker in cases:
        contents = copy.deepcopy(EDITED)
        contents[field] = invalid
        params = edit_request(owner, edited, OTHER, contents)
        before = capture(owner, identities, descriptor)
        error = owner.call('runtime.component.edit', params, error=-32602)
        need(marker in error['message'], 'Native array edit did not identify armed failure: ' + label)
        after = capture(owner, identities, descriptor)
        need(exact(after, before), 'Rejected array edit changed full selected state/external saves: ' + label)
        failures.append(dict(case=label, request=params, error=error, before=before, after=after, unchanged=True))
    params = edit_request(owner, edited, OTHER, EDITED)
    params['expected_revision'] -= 1
    before = capture(owner, identities, descriptor)
    error = owner.call('runtime.component.edit', params, error=-32009)
    need('Component revision conflict.' in error['message'], 'Native stale-edit rejection has another cause')
    after = capture(owner, identities, descriptor)
    need(exact(after, before), 'Stale array edit changed full selected state/external saves')
    failures.append(dict(case='stale_revision', request=params, error=error, before=before, after=after, unchanged=True))
    return edited, failures, length_states


def compiled_rejections(owner, identities, descriptor):
    failures = []
    for mode, label, marker in ((2, 'duplicate_staged_target', 'Duplicate gameplay component target in one tick.'),
                                (3, 'unresolved_last_entity', 'Component entity reference does not resolve')):
        owner.edit(Mode=mode)
        before = capture(owner, identities, descriptor)
        params = dict(session_id=owner.session, request_id=uuid.uuid4().hex, expected_tick=owner.tick, ticks=1)
        error = owner.call('runtime.step', params, error=-32040)
        need(marker in error['message'], 'Compiled rejection did not identify armed failure: ' + label)
        after = capture(owner, identities, descriptor)
        need(exact(after, before), 'Compiled collection failure changed full selected state/external saves')
        failures.append(dict(case=label, request=params, error=error, before=before, after=after, unchanged=True))
    owner.edit(Mode=1)
    return failures


def write_params(owner, state):
    return dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration, slot='collection-wire',
        expected_generation=0, session_id=owner.session, expected_tick=owner.tick,
        expected_gameplay_revision=state['gameplay']['revision'], expected_component_revision=state['registry']['component_revision'],
        expected_structure_revision=state['registry']['structure_revision'], expected_ui_revision=state['ui']['ui_revision'],
        expected_control_sequence=state['ui']['control_sequence'], expected_material_revision=state['runtime']['material_revision'])


def load_params(owner, state):
    return dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration, slot='collection-wire',
        expected_generation=1, revision=state['runtime']['current_authored_revision'],
        expected_session_id=owner.session, expected_tick=owner.tick, expected_gameplay_revision=state['gameplay']['revision'],
        expected_component_revision=state['registry']['component_revision'], expected_structure_revision=state['registry']['structure_revision'],
        expected_ui_revision=state['ui']['ui_revision'], expected_control_sequence=state['ui']['control_sequence'],
        expected_material_revision=state['runtime']['material_revision'], new_session_id=uuid.uuid4().hex)


def exercise(owner, identities, descriptor):
    initial = checkpoint(owner, identities, descriptor)
    need(members(initial) == [f'{n:032x}' for n in (1, 2, 3)] and len(identities) > 3, 'Author exactly three members plus absent instances')
    empty = {**{field: [] for field in FIELD_IDS[:5]}, FIELD_IDS[5]: 0}
    for identity in members(initial):
        need(exact(values(initial, identity), empty), 'Declare empty native arrays and zero scalar initially')
    need(initial['gameplay']['module']['values'] == dict(Mode=0, Ticks=0, ObservedCount=0, ReadBeforeWrite=0), 'Declare default scalar globals')
    owner.step()
    noop = checkpoint(owner, identities, descriptor)
    need(exact(noop['components'], [{**row, 'tick': 1} for row in initial['components']]), 'No-op compiled Tick changed collections')
    need(noop['gameplay']['module']['values']['ObservedCount'] == 3, 'Compiled sorted query count differs')
    pages(owner, noop)
    owner.edit(Mode=1)
    owner.step(2)
    written = checkpoint(owner, identities, descriptor)
    expected = {**KNOWN, FIELD_IDS[5]: 2}
    need(exact(values(written, OWNER), expected) and written['registry']['component_revision'] == 2
         and written['gameplay']['module']['values']['ReadBeforeWrite'] == 1, 'Compiled all-kind buffer fill/published reads differ')
    edited, failures, lengths = edit_checks(owner, identities, descriptor, written)
    pages(owner, edited)
    callback_failures = compiled_rejections(owner, identities, descriptor)
    owner.step()
    saved = checkpoint(owner, identities, descriptor)
    need(exact(values(saved, OWNER), {**KNOWN, FIELD_IDS[5]: 3}) and exact(values(saved, OTHER), EDITED),
         'Next compiled Tick failed prior published read or changed another edited member')
    write_request = write_params(owner, saved)
    stored = owner.call('save.write', write_request)
    need(stored['generation'] == 1 and not stored['replayed'], 'External durable save failed')
    inspected = owner.call('save.inspect', dict(slot='collection-wire'))
    need(inspected['generation'] == 1 and inspected['selected']['verified'], 'Array save payload not verified')
    owner.step(2)
    changed = checkpoint(owner, identities, descriptor)
    need(not exact(changed, saved) and values(changed, OWNER)[FIELD_IDS[5]] == 5, 'Post-save Tick did not change stored collection scalar')
    load_request = load_params(owner, changed)
    restored_reply = owner.call('save.load', load_request)
    need(restored_reply['session_id'] == load_request['new_session_id'] and restored_reply['session_id'] != owner.session,
         'Restore did not bind fresh requested runtime identity')
    owner.session, owner.tick = restored_reply['session_id'], restored_reply['tick']
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Same-owner save restore changed full selected typed collections/state')
    owner.step()
    continuation = checkpoint(owner, identities, descriptor)
    need(exact(values(continuation, OWNER), {**KNOWN, FIELD_IDS[5]: 4}) and exact(values(continuation, OTHER), EDITED),
         'Restored compiled collection continuation differs')
    before_retry = capture(owner, identities, descriptor)
    need(exact(owner.call('save.write', write_request), dict(stored, replayed=True)), 'Durable write retry differs')
    need(exact(owner.call('save.load', load_request), dict(restored_reply, replayed=True)), 'Historical load retry differs')
    need(exact(capture(owner, identities, descriptor), before_retry), 'Save retry replayed mutation or changed external files')
    return dict(initial=initial, noop=noop, written=written, filled=lengths['filled'], shortened=lengths['shortened'],
                cleared=lengths['cleared'], edited=edited, saved=saved,
                restored=restored, continuation=continuation), dict(array_edit_rejections=failures, callback_rejections=callback_failures)


def cold(owner, identities, descriptor, saved, continuation):
    initial = checkpoint(owner, identities, descriptor)
    params = load_params(owner, initial)
    restored_reply = owner.call('save.load', params)
    need(restored_reply['session_id'] == params['new_session_id'] and restored_reply['session_id'] != owner.session,
         'Cold owner restore did not use fresh guarded runtime identity')
    owner.session, owner.tick = restored_reply['session_id'], restored_reply['tick']
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Cold readonly restore lost typed array/scalar/global state')
    owner.step()
    continued = checkpoint(owner, identities, descriptor)
    need(exact(continued, continuation), 'Cold readonly compiled continuation differs from retained same-owner continuation')
    return dict(restored=restored, continuation=continued)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--source', '--project', dest='source', required=True, type=Path)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Windows Python without optimization required')
    need(30 <= args.timeout <= 600, 'Timeout must be30..600 seconds')
    assigned_job()
    args.binary, args.output = args.binary.resolve(), args.output.resolve()
    source, game = manifest(args.source, 'project.json'), manifest(args.game, 'game.json')
    need(not args.output.exists() and not args.output.is_relative_to(source.parent)
         and not args.output.is_relative_to(game.parent), 'Fresh output outside immutable source/game required')
    inputs = dict(source=tree(source.parent), game=tree(game.parent))
    args.output.mkdir(parents=True)
    started = time.monotonic()
    deadline = started+args.timeout
    owner = None
    report = dict(passed=False, harness=pin(Path(__file__)), source_binary=pin(args.binary), original_inputs=inputs,
        owners=[], checkpoints={}, rejection_checks={}, errors=[],
        limits=['All five supported capacity2 array kinds plus one scalar, with JSON length2/1/0/refill reads; no invisible wire-padding or generated Clear/RemoveAt/TryAdd-full claim, arbitrary capacities, nesting or global buffers.',
                'Full selected entity/component/gameplay/UI observations, not complete internal snapshot equivalence.',
                'No renderer/GPU, physical input, audio listening, representative performance, clean-machine/source-absence, complete ABI or Alpha release.',
                'Native errors identify fixture conditions; no general error-message stability promise. Join final natural closure with supervising Job receipt.'])
    try:
        spec, bundled = read(source), read(game)
        game_binary = contained(game.parent, bundled['engine']['executable'])
        rows = [row for row in bundled['files'] if row['path'] == bundled['engine']['executable']]
        need(len(rows) == 1 and rows[0]['role'] == 'runtime'
             and pin(game_binary) == dict(bytes=rows[0]['size'], sha256=rows[0]['sha256']), 'Bundled executable identity differs')
        report['bundled_binary'] = dict(path=bundled['engine']['executable'], **pin(game_binary))
        need(exact(spec['gameplay'].get('values', {}), bundled['gameplay']['values']), 'Declared initial globals differ')
        copied = args.output/'source-project'
        shutil.copytree(source.parent, copied)
        copied_before = tree(copied)
        world = contained(copied, spec['entry']['world'])
        identities = sorted(read(world)['entities'])
        need(4 <= len(identities) <= 32, 'Fixture needs complete bounded member/absent rows')
        source_descriptor = contained(copied, spec['gameplay']['descriptor'])
        game_descriptor = contained(game.parent, bundled['gameplay']['descriptor'])
        (sd, sl, _), (gd, gl, _) = artifact(source_descriptor), artifact(game_descriptor)
        need(sd['identity'] == gd['identity'] == IDENTITY and exact(sd['schema'], gd['schema'])
             and sl['sha256'] == gl['sha256'] and sl['size'] == gl['size'], 'Trusted source/bundle artifact cohort differs')
        all_states = {}
        for name, readonly in [('source-standalone', False), ('readonly-shared', True)]:
            saves = args.output/(name+'-saves')
            saves.mkdir()
            owner = Checked(game_binary if readonly else args.binary, args.output/name, deadline,
                world=None if readonly else world, game=game if readonly else None, saves=saves)
            if not readonly:
                revision = owner.call('world.inspect')['revision']
                owner.session, owner.tick = uuid.uuid4().hex, 0
                owner.call('runtime.start', dict(session_id=owner.session, revision=revision))
                owner.call('runtime.gameplay.load_native', dict(session_id=owner.session, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=0, descriptor=str(source_descriptor), values=spec['gameplay'].get('values', {})))
            states, rejected = exercise(owner, identities, game_descriptor if readonly else source_descriptor)
            all_states[name] = states
            report['checkpoints'][name], report['rejection_checks'][name] = states, rejected
            if readonly:
                readonly_saves = saves
            owner.close()
            report['owners'].append(dict(name=name, calls=len(owner.calls), validated_successes=owner.validated,
                returncode=owner.transport.returncode, host_returncode=None if owner.host is None else owner.host.returncode))
            owner = None
            need(dict(source=tree(source.parent), game=tree(game.parent)) == inputs, 'Original source/game changed')
        for name, state in all_states['source-standalone'].items():
            need(exact(state, all_states['readonly-shared'][name]), 'Full selected source/readonly typed pair differs: '+name)
        report['source_readonly_typed_pairs'] = len(all_states['source-standalone'])
        report['same_owner_save_restore_typed_pairs'] = 2
        owner = Checked(game_binary, args.output/'readonly-cold', deadline, game=game, saves=readonly_saves)
        report['checkpoints']['readonly-cold'] = cold(owner, identities, game_descriptor,
            all_states['readonly-shared']['saved'], all_states['readonly-shared']['continuation'])
        owner.close()
        report['owners'].append(dict(name='readonly-cold', calls=len(owner.calls), validated_successes=owner.validated,
            returncode=owner.transport.returncode, host_returncode=owner.host.returncode))
        owner = None
        report['cold_readonly_restore_continuation_typed_pairs'] = 2
        after = tree(copied)
        need(all(after.get(name) == row for name, row in copied_before.items()), 'Copied source input drift')
        extras = set(after)-set(copied_before)
        need(extras <= {world.relative_to(copied).as_posix()+'.lock'} and all(after[name]['bytes'] == 0 for name in extras), 'Unexpected owned source sidecars')
        report['copied_source_sidecars'] = {name: after[name] for name in extras}
        need(dict(source=tree(source.parent), game=tree(game.parent)) == inputs, 'Original input drift')
        report['original_inputs_unchanged'], report['passed'] = True, True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:
                owner.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
                report['passed'] = False
        report['seconds'] = time.monotonic()-started
        write(args.output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], seconds=report['seconds'], errors=len(report['errors']))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
