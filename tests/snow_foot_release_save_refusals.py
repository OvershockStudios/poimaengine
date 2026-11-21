#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify native refusal of correctly checksummed, invalid snow release ledgers."""

import argparse
import json
from pathlib import Path
import shutil
import tempfile
import traceback
from types import SimpleNamespace

from input_actions_gameplay_contract import exact
from instance_save_evolution import raw_digest, raw_text, replace_member, stored
from snow_compaction_contract import CELLS, HEIGHT_UNIT, Lattice, PLAIN, SPACING, TILES
from snow_foot_release_contract import CHECKPOINT_HELD, CHECKPOINT_READY, ReleasedSoleContract
from world_terrain_contract import file_hash, tree_hashes, uid


SNOW = ('snapshot', 'payload', 'snow')
LEDGER = SNOW + ('foot_release',)


def reseal_slot(source, destination, slot, path, replacement):
    """Copy one retained generation and update only its edited member and integrity fields."""
    target = destination / ('slot-' + slot)
    shutil.copytree(source, target)
    _, archive, entry = stored(destination, slot)
    checkpoint = replace_member(raw_text(archive), path, replacement)
    checkpoint = replace_member(checkpoint, ('snapshot', 'sha256'), raw_digest(checkpoint, ('snapshot', 'payload')))
    archive.write_bytes(checkpoint.encode('utf-8'))

    manifest = target / 'current.json'
    raw_manifest = raw_text(manifest, 1024 * 1024)
    receipts = json.loads(raw_manifest)['payload']['receipts']
    receipt, = (index for index, row in enumerate(receipts) if row['generation'] == entry['generation'])
    identity = dict(sha256=file_hash(archive), bytes=archive.stat().st_size)
    for owner in (('current',), ('receipts', receipt)):
        for field, value in identity.items():
            raw_manifest = replace_member(raw_manifest, ('payload', *owner, field), value)
    raw_manifest = replace_member(raw_manifest, ('sha256',), raw_digest(raw_manifest, ('payload',)))
    manifest.write_bytes(raw_manifest.encode('utf-8'))
    return dict(generation=entry['generation'], **identity)


def ready_order(row):
    return row['released_tick'], row['acquired_tick'], row['rig'], row['slot']


def malformed_cases(held, queued):
    """Each case targets one guard and names the expected native diagnostic."""
    first = queued[0]
    triangle = first['triangle']
    tile_x = TILES.index(first['receiver']) * CELLS
    cell_x, cell_z = triangle // 2 % CELLS + tile_x, triangle // 2 // CELLS
    cell_units = round(SPACING / HEIGHT_UNIT)
    # Move across the diagonal without leaving the named cell or the region.
    offset_x, offset_z = (32, 96) if triangle % 2 == 0 else (96, 32)
    invalid_center = [cell_x * cell_units + offset_x, cell_z * cell_units + offset_z]
    duplicated_ready = [first | dict(released_tick=first['released_tick'] - 1), *queued]
    assert duplicated_ready == sorted(duplicated_ready, key=ready_order)
    predecessor = first | dict(released_tick=CHECKPOINT_HELD)
    next_acquisition = first | dict(acquired_tick=CHECKPOINT_HELD)
    overlapping_ready = sorted([predecessor, next_acquisition, *queued[1:]], key=ready_order)
    still_locked = held[0] | dict(released_tick=CHECKPOINT_HELD)
    counts = dict(applied=0, saturated=0, cancelled=0, overflow=0)

    def with_rig(rig):
        return sorted([first | dict(rig=rig), *queued[1:]], key=ready_order)

    return (
        ('duplicate-holding-slot', 'held', LEDGER + ('holding',),
         [held[0], held[0] | dict(acquired_tick=CHECKPOINT_HELD), *held[1:]], 'without duplicate slots'),
        ('reordered-holding', 'held', LEDGER + ('holding',), held[::-1], 'must be ordered by rig and slot'),
        ('duplicate-ready-acquisition', 'ready', LEDGER + ('ready',), duplicated_ready, 'duplicate acquisition'),
        ('reordered-ready', 'ready', LEDGER + ('ready',), queued[::-1], 'must be in release order'),
        ('duplicate-across-queues', 'ready', LEDGER + ('holding',),
         [{field: value for field, value in first.items() if field != 'released_tick'}], 'duplicate acquisition'),
        ('overlapping-release-intervals', 'ready', LEDGER + ('ready',), overlapping_ready,
         'ready releases for one foot overlap in time'),
        ('overlapping-ready-and-held', 'ready', LEDGER,
         dict(holding=[held[0] | dict(acquired_tick=CHECKPOINT_HELD)], ready=[predecessor], counts=counts),
         'ready release overlaps a holding acquisition'),
        ('premature-release-of-live-hold', 'held', LEDGER,
         dict(holding=held[1:], ready=[still_locked], counts=counts), 'released snow foot overlaps a live planted foot'),
        ('off-triangle-center', 'ready', LEDGER + ('ready', 0, 'center_units'), invalid_center,
         'center is outside the frozen receiver triangle'),
        ('wrong-held-acquisition', 'held', LEDGER + ('holding', 0, 'acquired_tick'), CHECKPOINT_HELD,
         'differs from its live planted foot'),
        ('non-rig-live-identity', 'ready', LEDGER + ('ready',), with_rig(PLAIN), 'differs from its live rig slot'),
        ('invented-retired-rig', 'ready', LEDGER + ('ready',), with_rig('f' * 32), 'no allocated rig identity'),
        ('wrong-snow-version', 'held', SNOW + ('version',), 1, 'snapshot needs version 2'),
        ('consumption-count', 'ready', LEDGER + ('counts', 'saturated'), CHECKPOINT_READY + 1, 'count is out of range'),
        ('acquisition-count', 'ready', LEDGER + ('counts', 'cancelled'), 64 * CHECKPOINT_READY + 1,
         'counts exceed the committed acquisition budget'),
        ('outside-exact-integer-count', 'ready', LEDGER + ('counts', 'overflow'), 2 ** 53, 'Invalid snow foot release overflow count'),
        ('applied-before-compaction', 'ready', LEDGER + ('counts', 'applied'), 1,
         'applied snow footprints exceed the committed compaction generation'),
    )


def qualify(contract, source_path, report):
    source = json.loads(raw_text(source_path))
    assert source['format'] == 'poima.snow-foot-release-contract.v1' and source['passed'] is True
    assert source['executable_sha256'] == report['executable_sha256'], 'Use the binary that produced the successful checkpoints.'
    measured = source['measurements']
    retained_project = source_path.parent / 'Terrain Project'
    shutil.copytree(retained_project, contract.project)
    world = json.loads(contract.world.read_bytes())
    revision = world['revision']
    assert PLAIN in world['entities'] and 'AnimationRig' not in world['entities'][PLAIN]['components']
    assert 'f' * 32 not in world['entities']

    retained_slots = source_path.parent / 'Profile saves'
    snapshots, inventory = {}, {}
    report['sources'] = {}
    for slot, tick in (('held', CHECKPOINT_HELD), ('ready', CHECKPOINT_READY)):
        value, archive, entry = stored(retained_slots, slot)
        payload = value['snapshot']['payload']
        assert payload['tick'] == tick and payload['snow']['version'] == 2
        # Neither fixture performed a structural edit, so no future identity
        # can qualify as an allocated, retired source in these checkpoints.
        assert payload.get('structure') is None
        assert entry['generation'] == 1 and exact(measured['archives'][slot],
            dict(tick=tick, bytes=entry['bytes'], sha256=entry['sha256']))
        inventory[slot] = tree_hashes(archive.parent)
        report['sources'][slot] = dict(tick=tick, sha256=entry['sha256'], bytes=entry['bytes'])
        snapshots[slot] = payload['snow']['foot_release']
    held, queued = snapshots['held']['holding'], snapshots['ready']['ready']
    assert len(held) == len(queued) == 2
    assert all(row['acquired_tick'] < CHECKPOINT_HELD for row in held)
    assert not snapshots['held']['ready'] and not snapshots['ready']['holding']

    storage = contract.root / 'Refusal slots'
    storage.mkdir()
    shutil.copytree(retained_slots / 'slot-held', storage / 'slot-held')
    original = stored(storage, 'held')[1].read_bytes()
    lattice = Lattice()
    with contract.owner('snow-foot-release-refusals', saves=storage) as (client, _):
        fresh = contract.initialize(client, revision, uid(850), storage, None)
        active = uid(851)
        tick = CHECKPOINT_HELD
        assert contract.load(client, revision, fresh, active, 'held', tick, 852) == 0
        baseline_status = client.call('runtime.status')
        baseline_feet = contract.states(client, active, tick)
        baseline = contract.observe(client, active, tick, lattice, snapshots['held'], 'before refusals')
        assert exact(baseline, measured['observations']['held'])
        assert exact(baseline_feet, measured['foot_contacts']['still_held'])
        for index, (slot, source_slot, path, replacement, message) in enumerate(malformed_cases(held, queued)):
            row = dict(case=slot, checkpoint=source_slot, modified_path=path, replacement=replacement,
                       expected_diagnostic=message)
            report['refusals'].append(row)
            sealed = reseal_slot(retained_slots / ('slot-' + source_slot), storage, slot, path, replacement)
            status = client.call('save.inspect', dict(slot=slot))
            assert status['recovered'] is False and status['manifest_recovered'] is False, status
            assert exact(status['selected'], sealed | dict(verified=True)), status
            row['verified_storage'] = status['selected']
            guarded = dict(request_id=uid(860 + index), configuration_generation=1, slot=slot, revision=revision,
                expected_generation=1, expected_session_id=active, expected_tick=tick, expected_gameplay_revision=0,
                expected_structure_revision=0, new_session_id=uid(890 + index))
            refusal = contract.reject(client, 'save.load', guarded, -32070)
            row['diagnostic'] = refusal
            assert message in refusal['message'], refusal
            assert exact(client.call('runtime.status'), baseline_status)
            assert exact(contract.states(client, active, tick), baseline_feet)
            assert exact(contract.observe(client, active, tick, lattice, snapshots['held'], slot), baseline)
        after = contract.archive_release(client, storage, active, tick, 920, 'after-refusals', lattice, snapshots['held'])
        assert after == original
    for slot, expected in inventory.items():
        assert tree_hashes(retained_slots / ('slot-' + slot)) == expected
    assert tree_hashes(storage / 'slot-held') == inventory['held']
    report.update(active_archive_byte_identical=True, original_slots_unchanged=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source-evidence', type=Path, required=True, help='Successful retained snow foot release evidence.json.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    binary = args.binary.resolve(strict=True)
    source = args.source_evidence.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-foot-release-refusals-', dir=args.output.resolve()))
    contract = ReleasedSoleContract(SimpleNamespace(binary=binary, windows_interop=args.windows_interop), root)
    report = dict(format='poima.snow-foot-release-save-refusals.v1', passed=False,
        executable_sha256=file_hash(binary), harness_sha256=file_hash(Path(__file__)),
        source_evidence_sha256=file_hash(source), refusals=[], calls=contract.calls, measurements=contract.measurements)
    try:
        qualify(contract, source, report)
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        path = root / 'evidence.json'
        path.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
