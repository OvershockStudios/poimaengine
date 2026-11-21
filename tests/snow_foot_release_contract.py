#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify calibrated foot lifts, queued snow compaction and fresh-process saves."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from character_library_intake import LICENSE_SHA, SOURCE_SHA
from input_actions_gameplay_contract import exact
from runtime_ground_contacts import CALIBRATION, CHAINS, RIG
from snow_compaction_contract import (
    EAST, HEIGHT_UNIT, PLAIN, THICKNESS, TILES, Lattice, SnowContract, WEST, down, near, vector_near,
)
from world_particles_contract import component, transform
from world_terrain_contract import error, file_hash, rpc, uid


FOOT_PROFILE = dict(radius_units=1536, depth_units=96, sole_tolerance_m=.002)
ACTOR_POSITION = [1.03, .02, .5]
SLOTS = [slot for slot, *_ in CHAINS]
MANUAL_CENTER, MANUAL_RADIUS, MANUAL_DEPTH = (.15625, .15625), 128, 512
CLEAR = dict(applied=0, saturated=0, cancelled=0, overflow=0)
LIFT = dict(entity=RIG, clip=48, time=0, speed=1, loop=False, playing=False)
CHECKPOINT_HELD, CHECKPOINT_READY = 2, 3
WITNESSES = [('west sole', .817, .573), ('west seam', .987, .529),
             ('east seam', 1.014, .529), ('east sole', 1.296, .526), ('far tile', 1.812, .512)]


def intents(holding=(), ready=(), **counts):
    return dict(holding=list(holding), ready=list(ready), counts=CLEAR | counts)


def released(holding, tick):
    return [row | dict(released_tick=tick) for row in holding]


class ReleasedSoleContract(SnowContract):
    def author_knight(self):
        revision = super().author()
        source, notice = self.args.source, self.args.license
        assert file_hash(source) == SOURCE_SHA and file_hash(notice) == LICENSE_SHA
        inspected = self.world_call('asset.source.inspect', source=self.native(source))
        asset = self.world_call('asset.import', source=self.native(source),
                                expected_model_sha256=inspected['model_sha256'])['asset']
        assert file_hash(self.assets/(asset+'.pmodel')) == asset
        added = self.world_call('world.transact', request_id=uid(700), base_revision=revision,
                                 ops=[dict(op='asset.instantiate', id=RIG, asset=asset, name='Original Knight')])
        assert added['committed'] and added['revision'] == revision+1, added
        revision = added['revision']
        rig = json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig']
        rig.update(clip=36, time=0, speed=1, loop=False, playing=False,
                   ik=[dict(slot=slot, root=root, middle=middle, end=end, target=[0, 0, 0], pole=[0, 1, 1],
                            space='rig', metric='near-uniform-v1',
                            grounding=dict(calibration, reference_height=0, plant_lock=True, max_correction=.4))
                       for (slot, root, middle, end), calibration in zip(CHAINS, CALIBRATION, strict=True)])
        calibrated = self.world_call('world.transact', request_id=uid(701), base_revision=revision,
            ops=[component(RIG, 'Transform', transform(ACTOR_POSITION)), component(RIG, 'AnimationRig', rig)])
        assert calibrated['committed'] and calibrated['revision'] == revision+1, calibrated
        self.measurements['knight'] = dict(asset=asset, source_sha256=SOURCE_SHA, license_sha256=LICENSE_SHA)
        return calibrated['revision']

    def enable_foot_release(self, revision):
        before = self.world.read_bytes()
        refused = self.measurements['profile_refusals'] = []
        definition = dict(tiles=TILES, thickness_units=THICKNESS)
        for idx, patch in enumerate((dict(radius_units=127), dict(depth_units=True), dict(sole_tolerance_m='bad'))):
            op = dict(op='snow.set', definition=definition | dict(foot_release=FOOT_PROFILE | patch))
            reply, = self.world_calls(rpc('world.transact', request_id=uid(710+idx), base_revision=revision, ops=[op]))
            refused.append(error(reply, -32602))
            assert self.world.read_bytes() == before
        op = dict(op='snow.set', definition=definition | dict(foot_release=FOOT_PROFILE))
        reply = self.world_call('world.transact', request_id=uid(713), base_revision=revision, ops=[op])
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def states(self, client, session, tick):
        result = []
        for slot in SLOTS:
            row = client.call('runtime.ik', dict(session_id=session, tick=tick, entity=RIG, slot=slot,
                                                include_grounding=True, include_planting=True))
            assert row['session_id'] == session and row['tick'] == tick, row
            state = row['state']
            assert state['grounding']['decision_tick'] == tick, state
            result.append(state)
        return result

    def acquire_oracle(self, states, lattice, tick):
        holding = []
        for slot, expected_support, state in zip(SLOTS, [EAST, WEST], states, strict=True):
            g, p = state['grounding'], state['planting']
            assert g['status'] == 'contact' and g['cast'] and p['status'] == 'locked', state
            anchor = p['anchor']
            hit = g['hit']
            assert anchor['acquired_tick'] == tick and anchor['support'] == hit['entity'] == expected_support, state
            assert p['position_error'] <= FOOT_PROFILE['sole_tolerance_m'], state
            vector_near(p['target_sole'], hit['position'], 2e-5)
            x, y, z = hit['position']
            height, normal, face = lattice.surface(x, z, expected_support)
            near(y, height, 3e-5)
            vector_near(hit['normal'], normal, .002)
            assert hit['triangle'] == face, state
            holding.append(dict(rig=RIG, slot=slot, acquired_tick=tick, receiver=expected_support, triangle=face,
                                center_units=[math.floor(x/HEIGHT_UNIT+.5), math.floor(z/HEIGHT_UNIT+.5)]))
        return holding

    def plain_step(self, client, session, tick, request, *, retry=False, **commands):
        request = self.guard(session, tick, request) | commands
        result = client.call('runtime.step', request)
        assert result['current_session_id'] == session and exact(result['current_tick'], tick+1), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        if retry:
            replay = client.call('runtime.step', request)
            assert exact(replay, result | dict(replayed=True)), (result, replay)
        return result

    def automatic_stamp(self, lattice, row):
        x, z = row['center_units']
        assert lattice.stamp((x*HEIGHT_UNIT, z*HEIGHT_UNIT), FOOT_PROFILE['radius_units'], FOOT_PROFILE['depth_units'])

    def observe(self, client, session, tick, lattice, expected, label):
        snow = self.snow(client, session, tick, lattice)
        rays = {}
        for name, x, z in WITNESSES:
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick) | down(x, z))['hit']
            owner = TILES[int(x > 1)]
            y, normal, face = lattice.surface(x, z, owner)
            assert hit['entity'] == owner and hit['triangle'] == face, (hit, owner, face)
            vector_near(hit['position'], [x, y, z], 3e-5)
            vector_near(hit['normal'], normal, .002)
            rays[name] = dict(hit=hit, expected_height_m=y)
        if expected is None:
            assert 'foot_release' not in snow, snow
        else:
            assert exact(snow['foot_release'], expected), (snow, expected)
        result = dict(snow={k: v for k, v in snow.items() if k != 'session_id'}, rays=rays)
        self.measurements.setdefault('observations', {})[label] = result
        return result

    def archive_release(self, client, saves, session, tick, request, slot, lattice, expected):
        raw = self.archive(client, saves, session, tick, request, slot, has_marks=False)
        actual = json.loads(raw)['snapshot']['payload']['snow']
        payload = lattice.payload()
        if expected is not None:
            payload.update(version=2, foot_release=expected)
        assert exact(actual, payload), (slot, actual, payload)
        return raw

    def legacy(self, revision):
        saves = self.root/'Legacy saves'
        saves.mkdir()
        with self.owner('profile absent') as (client, _):
            session = self.initialize(client, revision, uid(720), saves, None)
            lattice = Lattice()
            self.plain_step(client, session, 0, 721)
            assert len(self.acquire_oracle(self.states(client, session, 1), lattice, 1)) == 2
            self.plain_step(client, session, 1, 722, animations=[LIFT])
            states = self.states(client, session, 2)
            assert all(row['planting']['status'] == 'lifted' for row in states), states
            self.plain_step(client, session, 2, 723)
            self.observe(client, session, 3, lattice, None, 'legacy')
            self.archive_release(client, saves, session, 3, 724, 'legacy-v1', lattice, None)

    def lift(self, client, session, label, holding, lattice):
        self.plain_step(client, session, CHECKPOINT_HELD, 732, animations=[LIFT], retry=True)
        states = self.states(client, session, CHECKPOINT_READY)
        assert all(row['grounding']['status'] == row['planting']['status'] == 'lifted' and
                   row['planting']['anchor'] is None for row in states), states
        queued = released(holding, CHECKPOINT_READY)
        expected = intents(ready=queued)
        observed = self.observe(client, session, CHECKPOINT_READY, lattice, expected, label+' released')
        return states, queued, observed

    def late_rollback(self, client, session, saves, lattice, queued, checkpoint, observation):
        mark = dict(receiver=PLAIN, ray=down(9, 9), half_extent_m=[.1, .1], half_depth_m=.03,
                    minimum_normal_dot=.8, edge_fraction=.25, linear_rgb=[.1, .2, .3], opacity=1, roughness=.8)
        # The valid ready event attempts a geometry change before the frame
        # that fails its end-of-tick surface mark. Both are transactional.
        result = self.reject(client, 'runtime.step', self.guard(session, CHECKPOINT_READY, 733) |
                             dict(surface_marks=[mark]), -32040)
        assert 'must first hit the expected receiver' in result['message'], result
        expected = intents(ready=queued)
        assert exact(self.observe(client, session, CHECKPOINT_READY, lattice, expected, 'after automatic rollback'),
                     observation)
        assert checkpoint == self.archive_release(client, saves, session, CHECKPOINT_READY, 739, 'after-rollback',
                                                  lattice, expected)
        self.measurements['late_rollback'] = result

    def consume(self, client, session, label, queued, lattice):
        observations = {}
        for tick, request, changed in ((3, 735, True), (4, 736, False)):
            manual = self.stamp(client, session, tick, lattice.generation, MANUAL_RADIUS, MANUAL_DEPTH,
                                center=MANUAL_CENTER)
            self.update(client, session, tick, request, manual, retry=True)
            assert lattice.stamp(MANUAL_CENTER, MANUAL_RADIUS, MANUAL_DEPTH) is changed
            observations[tick+1] = self.observe(client, session, tick+1, lattice, intents(ready=queued),
                                                 f'{label} explicit at tick {tick+1}')
        for index, event in enumerate(queued):
            tick = 5+index
            self.plain_step(client, session, tick, 740+index, retry=True)
            self.automatic_stamp(lattice, event)
            expected = intents(ready=queued[index+1:], applied=index+1)
            observations[tick+1] = self.observe(client, session, tick+1, lattice, expected,
                                                 f'{label} automatic at tick {tick+1}')
        assert lattice.generation == 3 and len(queued) == 2
        # Both feet reach the common edge while each center remains owned by
        # its original tile. The lattice oracle and live rays require both changes.
        assert lattice.depth[17][32] > 0
        self.plain_step(client, session, 7, 742)
        observations[8] = self.observe(client, session, 8, lattice, intents(applied=2), label+' consumed')
        return observations

    def cancel(self, revision, saves, holding):
        with self.owner('restored for cancellation') as (client, _):
            fresh = self.initialize(client, revision, uid(760), saves, None)
            session = uid(761)
            assert self.load(client, revision, fresh, session, 'held', CHECKPOINT_HELD, 762) == 0
            lattice = Lattice()
            self.plain_step(client, session, CHECKPOINT_HELD, 763, animations=[LIFT],
                            grounding=[dict(entity=RIG, slot=SLOTS[1], enabled=False, weight=0)])
            states = self.states(client, session, CHECKPOINT_READY)
            left, right = states
            assert left['grounding']['status'] == left['planting']['status'] == 'lifted', states
            assert right['grounding']['status'] == 'disabled' and right['planting']['anchor'] is None, states
            queued = released(holding[:1], CHECKPOINT_READY)
            self.observe(client, session, CHECKPOINT_READY, lattice, intents(ready=queued, cancelled=1),
                         'explicitly disabled sole')
            self.plain_step(client, session, CHECKPOINT_READY, 764)
            self.automatic_stamp(lattice, queued[0])
            self.observe(client, session, 4, lattice, intents(applied=1, cancelled=1), 'one authorized print')
            self.plain_step(client, session, 4, 765)
            self.observe(client, session, 5, lattice, intents(applied=1, cancelled=1), 'no canceled print')
            self.archive_release(client, saves, session, 5, 766, 'cancel-result', lattice,
                                 intents(applied=1, cancelled=1))
            self.measurements['cancellation'] = states

    def run(self):
        legacy_revision = self.author_knight()
        self.legacy(legacy_revision)
        revision = self.enable_foot_release(legacy_revision)
        saves = self.root/'Profile saves'
        saves.mkdir()
        owners = []
        lattice = Lattice()
        with self.owner('warm release workflow') as (client, warm):
            owners.append(warm)
            session = self.initialize(client, revision, uid(730), saves, None)
            self.observe(client, session, 0, lattice, intents(), 'before acquisition')
            self.plain_step(client, session, 0, 731)
            planted = self.states(client, session, 1)
            holding = self.acquire_oracle(planted, lattice, 1)
            self.observe(client, session, 1, lattice, intents(holding=holding), 'planted')
            self.plain_step(client, session, 1, 734)
            held_states = self.states(client, session, CHECKPOINT_HELD)
            for acquired, current in zip(planted, held_states, strict=True):
                assert current['planting']['status'] == 'locked', current
                assert exact(acquired['planting']['anchor'], current['planting']['anchor']), (acquired, current)
            held_observation = self.observe(client, session, CHECKPOINT_HELD, lattice, intents(holding=holding), 'held')
            held = self.archive_release(client, saves, session, CHECKPOINT_HELD, 737, 'held', lattice,
                                        intents(holding=holding))

            lifted, queued, ready_observation = self.lift(client, session, 'warm', holding, lattice)
            ready = self.archive_release(client, saves, session, CHECKPOINT_READY, 738, 'ready', lattice,
                                         intents(ready=queued))
            self.late_rollback(client, session, saves, lattice, queued, ready, ready_observation)
            outcomes = self.consume(client, session, 'warm', queued, lattice)
            final = self.archive_release(client, saves, session, 8, 743, 'final', lattice, intents(applied=2))

        for index, restore_slot in enumerate(('held', 'ready')):
            lattice = Lattice()
            with self.owner('cold from '+restore_slot) as (client, cold):
                owners.append(cold)
                fresh = self.initialize(client, revision, uid(750+index), saves, None)
                session = uid(752+index)
                tick = CHECKPOINT_HELD if restore_slot == 'held' else CHECKPOINT_READY
                assert self.load(client, revision, fresh, session, restore_slot, tick, 754+index) == 0
                if restore_slot == 'held':
                    expected = intents(holding=holding)
                    assert exact(held_states, self.states(client, session, tick))
                    assert exact(held_observation, self.observe(client, session, tick, lattice, expected, 'cold held'))
                    assert held == self.archive_release(client, saves, session, tick, 747, 'cold-held', lattice, expected)
                    replayed_lift, replayed_queue, replayed_observation = self.lift(client, session, 'cold held',
                                                                                 holding, lattice)
                    assert exact(lifted, replayed_lift) and exact(queued, replayed_queue)
                    assert exact(ready_observation, replayed_observation)
                else:
                    assert exact(lifted, self.states(client, session, tick))
                    expected = intents(ready=queued)
                    assert exact(ready_observation, self.observe(client, session, tick, lattice, expected, 'cold ready'))
                    assert ready == self.archive_release(client, saves, session, tick, 748, 'cold-ready', lattice, expected)
                result = self.consume(client, session, 'cold '+restore_slot, queued, lattice)
                assert exact(result, outcomes)
                assert final == self.archive_release(client, saves, session, 8, 749, 'final-'+restore_slot,
                                                     lattice, intents(applied=2))
        assert len({row['pid'] for row in owners}) == len(owners)
        self.cancel(revision, saves, holding)
        self.measurements['foot_contacts'] = dict(acquired=planted, still_held=held_states, lifted=lifted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source', type=Path, required=True, help='Original Knight.glb.')
    parser.add_argument('--license', type=Path, required=True, help='Exact CC0 notice accompanying the Knight.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-foot-release-', dir=args.output.resolve()))
    contract = ReleasedSoleContract(args, root)
    result = dict(format='poima.snow-foot-release-contract.v1', passed=False,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  snow_fixture_sha256=file_hash(Path(__file__).with_name('snow_compaction_contract.py')),
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
