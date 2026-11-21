#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify directional sole compaction against an independent integer lattice."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from character_library_intake import LibraryOracle
from imported_character_contract import point
from input_actions_gameplay_contract import exact
from runtime_ground_contacts import CALIBRATION, CHAINS, RIG
from snow_compaction_contract import HEIGHT, HEIGHT_UNIT, SPACING, THICKNESS, TILES, WIDTH, Lattice, down
from snow_compaction_contract import near, unit, vector_near
from snow_foot_release_contract import CHECKPOINT_HELD, CHECKPOINT_READY, LIFT, ReleasedSoleContract, intents, released
from snow_foot_release_save_refusals import LEDGER, SNOW, reseal_slot
from world_particles_contract import component, transform
from world_terrain_contract import error, file_hash, rpc, tree_hashes, uid


HALF_LENGTH, HALF_WIDTH, PRESSURE = 576, 256, 96
SOLE_TOLERANCE = .002
PROFILE = dict(sole=dict(half_length_units=HALF_LENGTH, half_width_units=HALF_WIDTH),
               depth_units=PRESSURE, sole_tolerance_m=SOLE_TOLERANCE)
AXIS_SCALE, WEIGHT_SCALE, STEP_UNITS = 1 << 14, 1 << 12, round(SPACING/HEIGHT_UNIT)
HEADINGS = [(0, [.76, .02, .50]), (45, [.815, .02, .55]),
            (90, [.955, .02, .55]), (180, [1.28, .02, .50])]


def round_away(value):
    return math.floor(value+.5) if value >= 0 else -math.floor(.5-value)


def quotient(numerator, denominator):
    """Round a signed rational to its nearest integer, with halves away."""
    magnitude, remainder = divmod(abs(numerator), denominator)
    result = magnitude+int(2*remainder >= denominator)
    return -result if numerator < 0 else result


DIRECTIONS = tuple((round_away(math.sin(math.tau*i/256)*AXIS_SCALE),
                    round_away(math.cos(math.tau*i/256)*AXIS_SCALE)) for i in range(256))


def heading(forward):
    x, _, z = forward
    length = math.hypot(x, z)
    assert length > 1e-12, forward
    x, z = (round_away(value/length*(1 << 20)) for value in (x, z))
    # max retains the earliest entry with equal dot product.
    return max(range(256), key=lambda index: x*DIRECTIONS[index][0]+z*DIRECTIONS[index][1])


def yaw_vector(vector, degrees):
    x, y, z = vector
    theta = math.radians(degrees)
    return (math.cos(theta)*x+math.sin(theta)*z, y, math.cos(theta)*z-math.sin(theta)*x)


def offset_weight(longitudinal, transverse):
    center = quotient(38*HALF_LENGTH, 100)
    extent = quotient(62*HALF_LENGTH, 100)
    heel_width = quotient(74*HALF_WIDTH, 100)
    candidates = []
    for shift, width in ((-center, heel_width), (center, HALF_WIDTH)):
        longitudinal_cost = quotient(WEIGHT_SCALE*(longitudinal-shift)**2, extent**2)
        transverse_cost = quotient(WEIGHT_SCALE*transverse**2, width**2)
        candidates.append(WEIGHT_SCALE-longitudinal_cost-transverse_cost)
    return max(0, *candidates)


class SoleLattice(Lattice):
    def shaped_stamp(self, event):
        cx, cz = event['center_units']
        fx, fz = DIRECTIONS[event['heading_bin']]
        changes = 0
        # Visit the entire region so the reference has no production envelope
        # optimization or dirty-tile logic in common with the native stamp.
        for z, row in enumerate(self.depth):
            for x, prior in enumerate(row):
                dx, dz = x*STEP_UNITS-cx, z*STEP_UNITS-cz
                longitudinal = quotient(dx*fx+dz*fz, AXIS_SCALE)
                transverse = quotient(dx*fz-dz*fx, AXIS_SCALE)
                increment = quotient(PRESSURE*offset_weight(longitudinal, transverse), WEIGHT_SCALE)
                row[x] = min(THICKNESS, prior+increment)
                changes += row[x] != prior
        self.generation += int(changes != 0)
        return changes


def validate_reference():
    assert all(heading(direction) == expected for direction, expected in (
        ((0, 0, 1), 0), ((1, 0, 1), 32), ((1, 0, 0), 64),
        ((0, 0, -1), 128), ((-1, 0, 0), 192)))
    assert heading((35, 0, 401)) == 3
    assert quotient(-3, 2) == -2 and quotient(3, 2) == 2
    center = [32*STEP_UNITS+64, 16*STEP_UNITS]
    north, south = SoleLattice(), SoleLattice()
    assert north.shaped_stamp(dict(center_units=center, heading_bin=0))
    assert south.shaped_stamp(dict(center_units=center, heading_bin=128))
    # At equal axial distances, this transverse witness is inside the toe
    # lobe and outside the narrower heel. Turn the boot and the witness swaps.
    assert north.depth[18][31] > 0 and north.depth[14][31] == 0
    assert south.depth[18][31] == 0 and south.depth[14][31] > 0


class SoleShapeContract(ReleasedSoleContract):
    def bind_shape(self, revision, yaw, position, first=False):
        definition = dict(tiles=TILES, thickness_units=THICKNESS)
        before = self.world.read_bytes()
        if first:
            invalid = {
                'two shapes': PROFILE | dict(radius_units=512),
                'missing width': PROFILE | dict(sole=dict(half_length_units=HALF_LENGTH)),
            }
            for name, length, width in (
                    ('boolean length', True, HALF_WIDTH), ('below minimum length', 255, HALF_WIDTH),
                    ('above maximum length', 1025, HALF_WIDTH), ('below minimum width', HALF_LENGTH, 127),
                    ('above maximum width', HALF_LENGTH, 513), ('outside total envelope', 1024, 512)):
                invalid[name] = PROFILE | dict(sole=dict(half_length_units=length, half_width_units=width))
            negatives = self.measurements['rejected_profiles'] = {}
            for index, (name, profile) in enumerate(invalid.items()):
                op = dict(op='snow.set', definition=definition | dict(foot_release=profile))
                reply, = self.world_calls(rpc('world.transact', request_id=uid(850+index),
                                              base_revision=revision, ops=[op]))
                negatives[name] = error(reply, -32602)
                assert self.world.read_bytes() == before
            self.measurements['accepted_dimension_bounds'] = []
            for index, (length, width) in enumerate(((256, 128), (1024, 510), (1022, 512))):
                profile = PROFILE | dict(sole=dict(half_length_units=length, half_width_units=width))
                op = dict(op='snow.set', definition=definition | dict(foot_release=profile))
                accepted = self.world_call('world.transact', request_id=uid(910+index), base_revision=revision, ops=[op])
                assert accepted['committed'] and accepted['revision'] == revision+1, accepted
                revision = accepted['revision']
                assert exact(json.loads(self.world.read_bytes())['snow']['foot_release'], profile)
                self.measurements['accepted_dimension_bounds'].append(profile)
        radians = math.radians(yaw)/2
        rotation = [0, math.sin(radians), 0, math.cos(radians)]
        ops = [dict(op='snow.set', definition=definition | dict(foot_release=PROFILE)),
               component(RIG, 'Transform', transform(position, rotation))]
        reply = self.world_call('world.transact', request_id=uid(860+yaw), base_revision=revision, ops=ops)
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def verify_anchors(self, states, base, yaw, position, source_pose):
        expected_holding = []
        for (slot, _, _, end), calibration, state in zip(CHAINS, CALIBRATION, states, strict=True):
            matrix = source_pose[end]
            authored_sole = point(matrix, calibration['sole_offset'])
            expected_input = [a+b for a, b in zip(position, yaw_vector(authored_sole, yaw), strict=True)]
            forward = [sum(matrix[4*j+i]*calibration['sole_forward'][j] for j in range(3)) for i in range(3)]
            forward = yaw_vector(forward, yaw)
            g, p = state['grounding'], state['planting']
            assert g['status'] == 'contact' and g['cast'] and p['status'] == 'locked', state
            vector_near(g['input_sole'], expected_input, 3e-6)
            anchor = p['anchor']
            assert anchor['acquired_tick'] == 1 and p['position_error'] <= SOLE_TOLERANCE, state
            x, y, z = g['hit']['position']
            tile = TILES[int(expected_input[0] >= 1)]
            assert anchor['support'] == g['hit']['entity'] == tile, state
            height, normal, triangle = base.surface(x, z, tile)
            near(y, height, 3e-5)
            assert g['hit']['triangle'] == anchor['triangle'] == triangle, state
            vector_near(anchor['local_point'], [x, y, z], 3e-5)
            vector_near(anchor['local_normal'], normal, .002)
            perpendicular = sum(a*b for a, b in zip(forward, normal, strict=True))
            projected = unit(tuple(a-perpendicular*b for a, b in zip(forward, normal, strict=True)))
            vector_near(anchor['local_forward'], projected, .002)
            # Reference heading comes from original GLB tracks and the
            # independent triangle plane as well as the public contact frame.
            expected_bin = heading(projected)
            assert heading(anchor['local_forward']) == expected_bin, (anchor, projected, expected_bin)
            expected_holding.append(dict(rig=RIG, slot=slot, acquired_tick=1, receiver=tile, triangle=triangle,
                center_units=[round_away(x/HEIGHT_UNIT), round_away(z/HEIGHT_UNIT)], heading_bin=expected_bin))
        return expected_holding

    def selected_witnesses(self, holding):
        witnesses = []
        for row in holding:
            matrix = SoleLattice()
            assert matrix.shaped_stamp(row) > 0
            for delta in (64, 128):
                rotated = SoleLattice()
                assert rotated.shaped_stamp(row | dict(heading_bin=(row['heading_bin']+delta)%256)) > 0
                assert matrix.depth != rotated.depth, (row, delta)
            cx, cz = row['center_units']
            x = cx*HEIGHT_UNIT+.0041
            z = cz*HEIGHT_UNIT+.0058
            witnesses.append((f"foot {row['slot']}", x, z))
        seam = SoleLattice()
        for row in holding:
            assert seam.shaped_stamp(row) > 0
        z_index = max(range(2, HEIGHT-2), key=lambda z: seam.depth[z][32])
        assert seam.depth[z_index][32] > 0, holding
        z = (z_index+.18)*SPACING
        witnesses.extend([('west edge', 1-.31*SPACING, z), ('east edge', 1+.33*SPACING, z),
                          ('untouched', 1.867, .873)])
        return witnesses

    def inspect_shape(self, client, session, tick, lattice, expected, label, witnesses=()):
        snow = self.snow(client, session, tick, lattice)
        assert exact(snow['foot_release'], expected), (snow, expected)
        collision = {}
        for name, x, z in witnesses:
            observed = client.call('runtime.raycast', dict(session_id=session, tick=tick) | down(x, z))['hit']
            tile = TILES[int(x >= 1)]
            height, normal, triangle = lattice.surface(x, z, tile)
            assert observed['entity'] == tile and observed['triangle'] == triangle, (name, observed, triangle)
            vector_near(observed['position'], [x, height, z], 3e-5)
            vector_near(observed['normal'], normal, .002)
            collision[name] = dict(hit=observed, expected_height_m=height, expected_normal=normal)
        observation = dict(snow={k: v for k, v in snow.items() if k != 'session_id'}, collision=collision)
        self.measurements.setdefault('observations', {})[label] = observation
        return observation

    def archive_shape(self, client, saves, session, tick, request, slot, lattice, expected):
        raw = self.archive(client, saves, session, tick, request, slot, has_marks=False)
        snow = json.loads(raw)['snapshot']['payload']['snow']
        expected_payload = lattice.payload() | dict(version=3, foot_release=expected)
        assert exact(snow, expected_payload), (slot, snow, expected_payload)
        return raw

    def refuse_archives(self, client, saves, revision, session, lattice, holding, witness, baseline, original_raw):
        """A failed load must preserve the live sole anchors and exact save bytes."""
        held_slot, ready_slot = 'yaw-0-held', 'yaw-0-ready'
        originals = {slot: tree_hashes(saves/('slot-'+slot)) for slot in (held_slot, ready_slot)}
        before_status = client.call('runtime.status')
        before_feet = self.states(client, session, CHECKPOINT_HELD)
        cases = [
            ('shaped-with-radial-version', held_slot, SNOW+('version',), 2, 'snapshot needs version 3'),
            ('missing-heading', held_slot, LEDGER+('holding',),
             [{k: v for k, v in holding[0].items() if k != 'heading_bin'}, *holding[1:]], 'invalid fields'),
            ('out-of-range-heading', ready_slot, LEDGER+('ready', 0, 'heading_bin'), 256, 'heading_bin must be'),
            ('noninteger-heading', ready_slot, LEDGER+('ready', 0, 'heading_bin'), True, 'heading_bin must be'),
            ('mismatched-held-heading', held_slot, LEDGER+('holding', 0, 'heading_bin'),
             (holding[0]['heading_bin']+64)%256, 'differs from its live planted foot'),
        ]
        results = self.measurements['rejected_archives'] = {}
        expected = intents(holding=holding)
        for index, (case, source, path, value, diagnostic) in enumerate(cases):
            sealed = reseal_slot(saves/('slot-'+source), saves, case, path, value)
            checked = client.call('save.inspect', dict(slot=case))
            assert checked['recovered'] is False and checked['manifest_recovered'] is False, checked
            assert exact(checked['selected'], sealed | dict(verified=True)), checked
            guarded = dict(request_id=uid(920+index), configuration_generation=1, slot=case, revision=revision,
                expected_generation=1, expected_session_id=session, expected_tick=CHECKPOINT_HELD,
                expected_gameplay_revision=0, expected_structure_revision=0, new_session_id=uid(930+index))
            refusal = self.reject(client, 'save.load', guarded, -32070)
            assert diagnostic in refusal['message'], refusal
            assert exact(client.call('runtime.status'), before_status)
            assert exact(self.states(client, session, CHECKPOINT_HELD), before_feet)
            after = self.inspect_shape(client, session, CHECKPOINT_HELD, lattice, expected, case, witness)
            assert exact(after, baseline)
            results[case] = dict(storage=checked['selected'], edit=list(path), replacement=value, error=refusal)
        assert original_raw == self.archive_shape(client, saves, session, CHECKPOINT_HELD, 935,
                                                  'after-refusals', lattice, expected)
        assert all(tree_hashes(saves/('slot-'+slot)) == original for slot, original in originals.items())

    def release_pose(self, client, session):
        self.plain_step(client, session, CHECKPOINT_HELD, 873, animations=[LIFT], retry=True)
        states = self.states(client, session, CHECKPOINT_READY)
        for row in states:
            assert row['grounding']['status'] == row['planting']['status'] == 'lifted', row
            assert row['planting']['anchor'] is None, row
        return states

    def apply_queue(self, client, session, queue, witnesses, label, lattice):
        results = []
        for index, event in enumerate(queue):
            expected_tick = CHECKPOINT_READY+index
            self.plain_step(client, session, expected_tick, 874+index, retry=True)
            assert lattice.shaped_stamp(event) > 0
            result = self.inspect_shape(client, session, expected_tick+1, lattice,
                intents(ready=queue[index+1:], applied=index+1), f'{label} print {index+1}', witnesses)
            results.append(result)
        return results

    def exercise(self, revision, yaw, position, source_pose):
        saves = self.root/f'Saves yaw {yaw}'
        saves.mkdir()
        prefix = f'yaw {yaw}'
        lattice = SoleLattice()
        with self.owner(prefix+' warm') as (client, warm):
            session = self.initialize(client, revision, uid(870), saves, None)
            self.inspect_shape(client, session, 0, lattice, intents(), prefix+' pristine')
            self.plain_step(client, session, 0, 871)
            acquired = self.states(client, session, 1)
            holding = self.verify_anchors(acquired, lattice, yaw, position, source_pose)
            witnesses = self.selected_witnesses(holding)
            original = self.inspect_shape(client, session, 1, lattice, intents(holding=holding),
                                          prefix+' acquired', witnesses)
            self.plain_step(client, session, 1, 872)
            held_states = self.states(client, session, CHECKPOINT_HELD)
            assert exact([r['planting']['anchor'] for r in acquired], [r['planting']['anchor'] for r in held_states])
            held = self.inspect_shape(client, session, CHECKPOINT_HELD, lattice, intents(holding=holding),
                                      prefix+' held', witnesses)
            assert exact(held['collision'], original['collision'])
            held_raw = self.archive_shape(client, saves, session, CHECKPOINT_HELD, 876, f'yaw-{yaw}-held', lattice,
                                          intents(holding=holding))
            lifted = self.release_pose(client, session)
            queue = released(holding, CHECKPOINT_READY)
            ready = self.inspect_shape(client, session, CHECKPOINT_READY, lattice, intents(ready=queue),
                                       prefix+' ready', witnesses)
            ready_raw = self.archive_shape(client, saves, session, CHECKPOINT_READY, 877, f'yaw-{yaw}-ready', lattice,
                                           intents(ready=queue))
            assert exact(ready['collision'], original['collision'])
            outcomes = self.apply_queue(client, session, queue, witnesses, prefix, lattice)
            for index, name in enumerate(('foot 1', 'foot 2')):
                before = ready if index == 0 else outcomes[0]
                assert outcomes[index]['collision'][name]['hit']['position'][1] < before['collision'][name]['hit']['position'][1]
            assert any(outcomes[-1]['collision'][name]['hit']['position'][1] <
                       original['collision'][name]['hit']['position'][1] for name in ('west edge', 'east edge'))
            final_tick = CHECKPOINT_READY+len(queue)
            final_raw = self.archive_shape(client, saves, session, final_tick, 878, f'yaw-{yaw}-final', lattice,
                                           intents(applied=2))

        owners = [warm['pid']]
        for offset, slot in enumerate(('held', 'ready')):
            lattice = SoleLattice()
            label = prefix+' restored '+slot
            with self.owner(label) as (client, cold):
                owners.append(cold['pid'])
                fresh = self.initialize(client, revision, uid(880+offset), saves, None)
                session = uid(882+offset)
                tick = CHECKPOINT_HELD if slot == 'held' else CHECKPOINT_READY
                assert self.load(client, revision, fresh, session, f'yaw-{yaw}-{slot}', tick, 884+offset) == 0
                if slot == 'held':
                    assert exact(held_states, self.states(client, session, tick))
                    assert exact(held, self.inspect_shape(client, session, tick, lattice, intents(holding=holding),
                                                         label, witnesses))
                    assert held_raw == self.archive_shape(client, saves, session, tick, 886, f'yaw-{yaw}-cold-held', lattice,
                                                          intents(holding=holding))
                    if yaw == 0:
                        self.refuse_archives(client, saves, revision, session, lattice, holding, witnesses,
                                             held, held_raw)
                    assert exact(lifted, self.release_pose(client, session))
                else:
                    assert exact(lifted, self.states(client, session, tick))
                    assert ready_raw == self.archive_shape(client, saves, session, tick, 887, f'yaw-{yaw}-cold-ready', lattice,
                                                           intents(ready=queue))
                check = self.inspect_shape(client, session, CHECKPOINT_READY, lattice, intents(ready=queue),
                                           label+' ready', witnesses)
                assert exact(ready, check)
                assert exact(outcomes, self.apply_queue(client, session, queue, witnesses, label, lattice))
                assert final_raw == self.archive_shape(client, saves, session, final_tick, 888,
                                                       f'yaw-{yaw}-final-{slot}', lattice, intents(applied=2))
        assert len(set(owners)) == len(owners)
        return dict(yaw_degrees=yaw, root_position_m=position, anchors=holding, witnesses=witnesses,
                    final_lost_volume_units=lattice.volume_units())

    def run(self):
        validate_reference()
        revision = self.author_knight()
        oracle = LibraryOracle(self.args.source.read_bytes())
        source = oracle.pose(36, 0)[2]
        cohort = self.measurements['headings'] = []
        for index, (yaw, position) in enumerate(HEADINGS):
            revision = self.bind_shape(revision, yaw, position, first=index == 0)
            result = self.exercise(revision, yaw, position, source)
            if cohort:
                origin = cohort[0]['anchors']
                for original, actual in zip(origin, result['anchors'], strict=True):
                    expected_bin = (original['heading_bin']+round(yaw*256/360))%256
                    assert actual['heading_bin'] == expected_bin, (yaw, original, actual, expected_bin)
            cohort.append(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source', type=Path, required=True, help='Original Knight.glb.')
    parser.add_argument('--license', type=Path, required=True, help='Exact CC0 license accompanying the Knight.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-sole-shape-', dir=args.output.resolve()))
    contract = SoleShapeContract(args, root)
    result = dict(format='poima.snow-sole-shape-contract.v1', passed=False,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
