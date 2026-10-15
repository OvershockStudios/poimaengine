#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compiled save-failure ledger and recovery in source/exported Windows games.

Use the original adopted ComponentGame with Mode0 initial values, an immutable
export, and a fresh output outside both. Run inside switchback_ownership.gated.
No compiler, GPU, filesystem fault injection or resolving-state claim.
"""
import argparse
import copy
import os
from pathlib import Path
import shutil
import sys
import time
import traceback
import uuid
import json

from workflow_components_game_service import (Checked, HEALTH, artifact, assigned_job,
    checkpoint, contained, exact, manifest, need, payload, pin, read, ticket, tree, write)

OWNER = f'{1:032x}'
CURRENT, SCORE = f'{1:032x}', f'{3:032x}'
SLOT = 'components-game'


def capture(owner, identities, descriptor):
    return dict(checkpoint=checkpoint(owner, identities, descriptor), routing=owner.call('save.status'),
                queue=owner.call('runtime.save.status', dict(session_id=owner.session)),
                slot=owner.call('save.inspect', dict(slot=SLOT)), files=tree(owner.expected_save_root))


def armed(owner, mode):
    owner.edit(Mode=mode, SaveHigh='0', SaveLow='0', SaveSequence='0', SaveState=0)


def committed(owner, before, after, result):
    need(result['save_serviced'] and not result['runtime_replaced'] and result['session_id'] == result['current_session_id'] == owner.session,
         'Storage failure replaced the runtime or was not serviced')
    need(after['runtime']['tick'] == before['runtime']['tick']+1 and result['tick'] == owner.tick,
         'Successful gameplay Tick did not commit before storage result')
    old, new = payload(before, OWNER, HEALTH), payload(after, OWNER, HEALTH)
    need(new[CURRENT] == old[CURRENT]-1 and new[SCORE] == str(int(old[SCORE])+1)
         and after['registry']['component_revision'] == before['registry']['component_revision']+1,
         'Storage outcome rolled back or skipped committed native component writes')
    old_values, new_values = before['gameplay']['module']['values'], after['gameplay']['module']['values']
    need(new_values['Ticks'] == old_values['Ticks']+1 and new_values['ReadBeforeWrite'] == 1
         and new_values['SaveRequests'] == old_values['SaveRequests']+1,
         'Committed compiled callback/global read-before-write state differs')


def operation(owner, state, step, kind, error_code=None, marker=None):
    identity = ticket(state['gameplay']['module']['values'])
    result = owner.call('runtime.save.result', identity)
    need(exact(result, step['save_operation']) and result['kind'] == kind
         and result['state'] == (3 if error_code is None else 4), 'Actual step/ledger operation identity or terminal state differs')
    need(result['epoch'] == owner.call('runtime.save.status', dict(session_id=owner.session))['epoch'], 'Terminal ticket is not this runtime epoch')
    need(result['requested_tick']+1 == result['committed_tick'] == owner.tick, 'Ticket does not match one-Tick request/commit boundary')
    need(state['gameplay']['module']['values']['SaveState'] == 1, 'Callback did not observe its queued ticket before owner service')
    if error_code is not None:
        need(result['error_code'] == error_code and marker in result['diagnostic']
             and result['generation'] == 0 and result['recovered'] is False
             and result['restored_epoch'] is None and result['restored_tick'] == 0, 'Failed storage cause/restore metadata differs')
    else:
        need(result['generation'] == 1 and result['error_code'] == 0, 'First successful write completion differs')
    return identity, result


def repeat_queries(owner, identities, descriptor, expected):
    before = capture(owner, identities, descriptor)
    for identity, result in expected:
        for _ in range(2):
            need(exact(owner.call('runtime.save.result', identity), result), 'Repeated ticket query consumed or changed retained completion')
    need(exact(capture(owner, identities, descriptor), before), 'Ticket queries changed full selected state/queue/routing/store')


def historical_step(owner, identities, descriptor, request, result):
    before = capture(owner, identities, descriptor)
    replay = owner.call('runtime.step', request)
    need(exact(replay, dict(result, replayed=True)), 'Historical step retry did not retain original operation/outcome')
    after = capture(owner, identities, descriptor)
    need(exact(after, before), 'Historical step resubmitted a save or changed current state/store')
    return dict(request=request, result=replay, before=before, after=after, unchanged=True)


def bad_queries(owner, identities, descriptor, known):
    id_error = 'ID must be 32 lowercase hexadecimal characters.'
    ticket_error = 'Save ticket must have a nonzero epoch and positive sequence.'
    numeric_error = 'Revision must be a safe nonnegative JSON integer.'
    cases = [(dict(epoch='bad', sequence=1), id_error), (dict(epoch='0'*32, sequence=1), ticket_error),
             (dict(epoch=False, sequence=1), id_error), (dict(epoch=known['epoch'], sequence=0), ticket_error),
             (dict(epoch=known['epoch'], sequence=True), numeric_error),
             (dict(epoch=known['epoch'], sequence=1.0), numeric_error)]
    results = []
    for params, marker in cases:
        before = capture(owner, identities, descriptor)
        error = owner.call('runtime.save.result', params, error=-32602)
        need(marker in error['message'], 'Malformed ticket rejection has an unrelated cause')
        after = capture(owner, identities, descriptor)
        need(exact(after, before), 'Malformed ticket query changed selected state or storage')
        results.append(dict(request=params, error=error, before=before, after=after, unchanged=True))
    unknown = dict(epoch=uuid.uuid4().hex, sequence=1)
    need(unknown['epoch'] != known['epoch'], 'Unexpected random unknown-ticket collision')
    before = capture(owner, identities, descriptor)
    expired = owner.call('runtime.save.result', unknown)
    need(expired['state'] == 0 and expired['kind'] == 0 and expired['epoch'] == unknown['epoch']
         and expired['sequence'] == 1, 'Well-formed unknown ticket did not return its expired identity')
    need(exact(owner.call('runtime.save.result', unknown), expired) and exact(capture(owner, identities, descriptor), before),
         'Expired ticket query consumed data or changed state/storage')
    return results, dict(request=unknown, result=expired, unchanged=True)


def save_request(owner, state):
    return dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration, slot='failure-recovery',
        expected_generation=0, session_id=owner.session, expected_tick=owner.tick,
        expected_gameplay_revision=state['gameplay']['revision'], expected_component_revision=state['registry']['component_revision'],
        expected_structure_revision=state['registry']['structure_revision'], expected_ui_revision=state['ui']['ui_revision'],
        expected_control_sequence=state['ui']['control_sequence'], expected_material_revision=state['runtime']['material_revision'])


def load_request(owner, state):
    return dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration, slot='failure-recovery',
        expected_generation=1, revision=state['runtime']['current_authored_revision'],
        expected_session_id=owner.session, expected_tick=owner.tick, expected_gameplay_revision=state['gameplay']['revision'],
        expected_component_revision=state['registry']['component_revision'], expected_structure_revision=state['registry']['structure_revision'],
        expected_ui_revision=state['ui']['ui_revision'], expected_control_sequence=state['ui']['control_sequence'],
        expected_material_revision=state['runtime']['material_revision'], new_session_id=uuid.uuid4().hex)


def history_failure(owner, identities, descriptor):
    state = checkpoint(owner, identities, descriptor)
    writes = []
    for generation in range(1, 33):
        params = save_request(owner, state)
        params.update(slot=SLOT, expected_generation=generation)
        result = owner.call('save.write', params)
        need(result['generation'] == generation+1 and not result['replayed'], 'Legitimate history-extension write failed')
        writes.append(dict(request=params, result=result))
    verified = owner.call('save.inspect', dict(slot=SLOT))
    need(verified['generation'] == 33 and verified['selected']['verified'], 'History fixture did not reach verified generation33')
    manifest_path = owner.expected_save_root/('slot-'+SLOT)/'current.json'
    actual_manifest = read(manifest_path)
    receipts = actual_manifest['payload']['receipts']
    need(len(receipts) == 32 and [r['generation'] for r in receipts] == list(range(2, 34)),
         'Real storage did not evict the first generation from its bounded receipt window')
    armed(owner, 8)
    before = capture(owner, identities, descriptor)
    request, step = owner.step()
    failed = checkpoint(owner, identities, descriptor)
    committed(owner, before['checkpoint'], failed, step)
    identity, result = operation(owner, failed, step, 1, -32009, 'Save generation changed; inspect before writing.')
    after_files = tree(owner.expected_save_root)
    need(after_files == before['files'] and exact(owner.call('save.inspect', dict(slot=SLOT)), before['slot']),
         'Prepublication CAS rejection outside retained history changed persisted data')
    owner.step()
    seen = checkpoint(owner, identities, descriptor)
    need(seen['gameplay']['module']['values']['SaveState'] == 4
         and seen['gameplay']['module']['values']['SaveRequests'] == failed['gameplay']['module']['values']['SaveRequests'],
         'Outside-history failure remained unresolved, blocked another Tick, or resubmitted')
    repeat_queries(owner, identities, descriptor, [(identity, result)])
    retry = historical_step(owner, identities, descriptor, request, step)
    return failed, seen, identity, result, dict(writes=writes, verified_slot=verified,
        manifest=actual_manifest, manifest_identity=pin(manifest_path), failed_before=before,
        failed_after_files=after_files, native_operation=result, retry=retry)


def exercise(owner, identities, descriptor):
    initial = checkpoint(owner, identities, descriptor)
    need(initial['gameplay']['module']['values']['Mode'] == 0 and payload(initial, OWNER, HEALTH) is not None,
         'Original default ComponentGame baseline required')
    need(not owner.call('save.inspect', dict(slot=SLOT))['exists'], 'Use fresh storage before missing-load test')
    armed(owner, 9)
    before_missing = capture(owner, identities, descriptor)
    missing_request, missing_step = owner.step()
    missing = checkpoint(owner, identities, descriptor)
    committed(owner, before_missing['checkpoint'], missing, missing_step)
    missing_ticket, missing_result = operation(owner, missing, missing_step, 2, -32070, 'Save slot does not exist.')
    need(tree(owner.expected_save_root) == before_missing['files'] and not owner.call('save.inspect', dict(slot=SLOT))['exists'],
         'Missing load created storage or altered original slot')
    owner.step()
    missing_seen = checkpoint(owner, identities, descriptor)
    need(missing_seen['gameplay']['module']['values']['SaveState'] == 4
         and missing_seen['gameplay']['module']['values']['SaveRequests'] == missing['gameplay']['module']['values']['SaveRequests'],
         'Next callback did not see retained failed load or submitted again')
    repeat_queries(owner, identities, descriptor, [(missing_ticket, missing_result)])
    missing_retry = historical_step(owner, identities, descriptor, missing_request, missing_step)

    armed(owner, 8)
    before_success = checkpoint(owner, identities, descriptor)
    _, success_step = owner.step()
    success = checkpoint(owner, identities, descriptor)
    committed(owner, before_success, success, success_step)
    success_ticket, success_result = operation(owner, success, success_step, 1)
    good_files = tree(owner.expected_save_root)
    good_slot = owner.call('save.inspect', dict(slot=SLOT))
    need(good_slot['generation'] == 1 and good_slot['selected']['verified'], 'Successful first save is not durably verified')
    owner.step()
    need(owner.game()['module']['values']['SaveState'] == 3, 'Next callback did not see completed first save')

    armed(owner, 8)  # Original fixture deliberately requests generation zero again.
    before_cas = capture(owner, identities, descriptor)
    cas_request, cas_step = owner.step()
    failed_cas = checkpoint(owner, identities, descriptor)
    committed(owner, before_cas['checkpoint'], failed_cas, cas_step)
    cas_ticket, cas_result = operation(owner, failed_cas, cas_step, 1, -32009, 'Save generation changed; inspect before writing.')
    need(cas_ticket != success_ticket and cas_ticket != missing_ticket, 'Fresh failure reused a prior request ticket')
    need(tree(owner.expected_save_root) == good_files and exact(owner.call('save.inspect', dict(slot=SLOT)), good_slot),
         'Failed CAS changed actual retained save generations or files')
    owner.step()
    cas_seen = checkpoint(owner, identities, descriptor)
    need(cas_seen['gameplay']['module']['values']['SaveState'] == 4
         and cas_seen['gameplay']['module']['values']['SaveRequests'] == failed_cas['gameplay']['module']['values']['SaveRequests'],
         'Next callback lost failed CAS or submitted again')
    repeat_queries(owner, identities, descriptor, [(missing_ticket, missing_result), (success_ticket, success_result), (cas_ticket, cas_result)])
    cas_retry = historical_step(owner, identities, descriptor, cas_request, cas_step)
    malformed, expired = bad_queries(owner, identities, descriptor, cas_ticket)
    history_failed, history_seen, history_ticket, history_result, history_checks = history_failure(owner, identities, descriptor)
    repeat_queries(owner, identities, descriptor, [(missing_ticket, missing_result),
        (success_ticket, success_result), (cas_ticket, cas_result), (history_ticket, history_result)])

    owner.edit(Mode=0)  # Restore and continue without triggering another storage request.
    saved = checkpoint(owner, identities, descriptor)
    write_params = save_request(owner, saved)
    stored = owner.call('save.write', write_params)
    need(stored['generation'] == 1, 'Failure poisoned a subsequent guarded external save')
    recovered_slot = owner.call('save.inspect', dict(slot='failure-recovery'))
    need(recovered_slot['selected']['verified'] and recovered_slot['generation'] == 1, 'Recovery checkpoint not durable/verified')
    owner.step(2)
    changed = checkpoint(owner, identities, descriptor)
    load_params = load_request(owner, changed)
    restored_reply = owner.call('save.load', load_params)
    need(restored_reply['session_id'] == load_params['new_session_id'] and restored_reply['session_id'] != owner.session,
         'Recovery restore did not bind fresh requested runtime identity')
    owner.session, owner.tick = restored_reply['session_id'], restored_reply['tick']
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Same-owner failure recovery restore lost full selected state')
    need(exact(owner.call('runtime.save.result', history_ticket), history_result), 'Same owner lost failed ticket after external recovery load')
    owner.step()
    continued = checkpoint(owner, identities, descriptor)
    saved_values, continued_values = saved['gameplay']['module']['values'], continued['gameplay']['module']['values']
    saved_health = payload(saved, OWNER, HEALTH)
    need(continued_values['Mode'] == 0 and continued_values['Ticks'] == saved_values['Ticks']+1
         and continued_values['SaveRequests'] == saved_values['SaveRequests']
         and continued_values['LastHealth'] == saved_health[CURRENT]
         and continued_values['LastScore'] == saved_health[SCORE]
         and exact(payload(continued, OWNER, HEALTH), payload(saved, OWNER, HEALTH)), 'Mode0 recovery continuation changed components or retriggered save')
    return dict(initial=initial, missing_load=missing, missing_observed=missing_seen, good_save=success,
                failed_cas=failed_cas, failed_cas_observed=cas_seen, outside_history_failed=history_failed,
                outside_history_observed=history_seen, recovery_saved=saved,
                recovery_restored=restored, recovery_continued=continued), dict(
        missing_operation=missing_result, successful_operation=success_result, cas_operation=cas_result,
        missing_step_retry=missing_retry, cas_step_retry=cas_retry, malformed_queries=malformed,
        expired_query=expired, outside_history_checks=history_checks), history_ticket


def cold(owner, identities, descriptor, saved, continued, old_ticket):
    initial = checkpoint(owner, identities, descriptor)
    params = load_request(owner, initial)
    reply = owner.call('save.load', params)
    need(reply['session_id'] == params['new_session_id'] and reply['session_id'] != owner.session,
         'Fresh readonly recovery did not bind actual guarded new session')
    owner.session, owner.tick = reply['session_id'], reply['tick']
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Cold readonly recovery lost full saved globals/components/entity/UI state')
    expired = owner.call('runtime.save.result', old_ticket)
    need(expired['state'] == 0 and expired['epoch'] == old_ticket['epoch'] and expired['sequence'] == old_ticket['sequence'],
         'Fresh owner fabricated old failed ledger completion')
    owner.step()
    continuation = checkpoint(owner, identities, descriptor)
    need(exact(continuation, continued), 'Cold Mode0 recovery continuation differs from retained same-owner state')
    return dict(restored=restored, continued=continuation, expired_prior_failed_ticket=expired)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('binary', 'source', 'game', 'output'):
        parser.add_argument('--'+arg, required=True, type=Path)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Windows Python without optimization required')
    need(30 <= args.timeout <= 600, 'Timeout must be30..600 seconds')
    assigned_job()
    args.binary, args.output = args.binary.resolve(), args.output.resolve()
    source, game = manifest(args.source, 'project.json'), manifest(args.game, 'game.json')
    need(not args.output.exists() and not args.output.is_relative_to(source.parent) and not args.output.is_relative_to(game.parent),
         'Fresh independent output outside source/game required')
    originals = dict(source=tree(source.parent), game=tree(game.parent))
    args.output.mkdir(parents=True)
    started, owner = time.monotonic(), None
    deadline = started+args.timeout
    report = dict(passed=False, harness=pin(Path(__file__)), source_binary=pin(args.binary), original_inputs=originals,
        owners=[], checkpoints={}, ledger_checks={}, errors=[], limits=[
            'Missing-load and generation-CAS terminal failures, including32 legitimate writes reachinggeneration33/oldestreceipt2; no unknown-outcome resolving, gameplay-ledger eviction, corrupted recovery/upgrades or arbitrary filesystem fault proof.',
            'Full selected scalar component/gameplay/entity/UI states, not complete internal snapshot equivalence; random saved ticket globals retained exactly.',
            'No renderer/GPU, physical input, listening, performance, source-absence/clean-machine or broader stable API/ABI release.',
            'Native error markers identify these fixture conditions; natural closure requires the supervising Job receipt, not a global census.'])
    try:
        spec, exported = read(source), read(game)
        need(exact(spec['gameplay'].get('values', {}), exported['gameplay']['values']), 'Declared source/bundled initial values differ')
        binary = contained(game.parent, exported['engine']['executable'])
        rows = [row for row in exported['files'] if row['path'] == exported['engine']['executable']]
        need(len(rows) == 1 and rows[0]['role'] == 'runtime' and pin(binary) == dict(bytes=rows[0]['size'], sha256=rows[0]['sha256']),
             'Declared bundled engine file identity differs')
        report['bundled_binary'] = dict(path=exported['engine']['executable'], **pin(binary))
        copied = args.output/'source-project'
        shutil.copytree(source.parent, copied)
        copied_before = tree(copied)
        world = contained(copied, spec['entry']['world'])
        identities = sorted(read(world)['entities'])
        need(3 <= len(identities) <= 32, 'Bounded complete fixture entity rows required')
        source_descriptor = contained(copied, spec['gameplay']['descriptor'])
        game_descriptor = contained(game.parent, exported['gameplay']['descriptor'])
        (sd, sl, _), (gd, gl, _) = artifact(source_descriptor), artifact(game_descriptor)
        need(sd['identity'] == gd['identity'] == 'poima.test.components' and exact(sd['schema'], gd['schema'])
             and sl['sha256'] == gl['sha256'] and sl['size'] == gl['size'], 'Source/bundled trusted artifact cohort differs')
        states = {}
        for name, readonly in [('source-standalone', False), ('readonly-shared', True)]:
            saves = args.output/(name+'-saves')
            saves.mkdir()
            owner = Checked(binary if readonly else args.binary, args.output/name, deadline,
                world=None if readonly else world, game=game if readonly else None, saves=saves)
            if not readonly:
                owner.session, owner.tick = uuid.uuid4().hex, 0
                owner.call('runtime.start', dict(session_id=owner.session, revision=owner.call('world.inspect')['revision']))
                owner.call('runtime.gameplay.load_native', dict(session_id=owner.session, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=0, descriptor=str(source_descriptor), values=spec['gameplay'].get('values', {})))
            state, checks, failed_ticket = exercise(owner, identities, game_descriptor if readonly else source_descriptor)
            states[name] = state
            report['checkpoints'][name], report['ledger_checks'][name] = state, checks
            if readonly:
                readonly_saves, readonly_ticket = saves, failed_ticket
            owner.close()
            report['owners'].append(dict(name=name, calls=len(owner.calls), validated_successes=owner.validated,
                returncode=owner.transport.returncode, host_returncode=None if owner.host is None else owner.host.returncode))
            owner = None
            need(dict(source=tree(source.parent), game=tree(game.parent)) == originals, 'Original input bytes changed')
        need(exact(states['source-standalone']['initial'], states['readonly-shared']['initial']), 'Initial pre-ticket source/read-only selected state differs')
        report['initial_cross_owner_typed_pairs'], report['same_owner_recovery_restore_typed_pairs'] = 1, 2
        owner = Checked(binary, args.output/'readonly-cold', deadline, game=game, saves=readonly_saves)
        report['checkpoints']['readonly-cold'] = cold(owner, identities, game_descriptor,
            states['readonly-shared']['recovery_saved'], states['readonly-shared']['recovery_continued'], readonly_ticket)
        owner.close()
        report['owners'].append(dict(name='readonly-cold', calls=len(owner.calls), validated_successes=owner.validated,
            returncode=owner.transport.returncode, host_returncode=owner.host.returncode))
        owner = None
        report['cold_recovery_restore_continuation_typed_pairs'] = 2
        after = tree(copied)
        need(all(after.get(name) == row for name, row in copied_before.items()), 'Owned source input drift')
        extra = set(after)-set(copied_before)
        need(extra <= {world.relative_to(copied).as_posix()+'.lock'} and all(after[name]['bytes'] == 0 for name in extra), 'Unexpected source sidecar')
        report['copied_source_sidecars'] = {name: after[name] for name in extra}
        need(dict(source=tree(source.parent), game=tree(game.parent)) == originals, 'Original source/bundle changed')
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
