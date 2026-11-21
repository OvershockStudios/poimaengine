#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify that one released print can wait for a planted foot while another compacts."""

import argparse
from copy import deepcopy
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from runtime_ground_contacts import RIG
from snow_compaction_contract import CELLS, EAST, HEIGHT, HEIGHT_UNIT, TILES, WEST, WIDTH, Lattice, down, faces, vector_near
from snow_foot_release_contract import ACTOR_POSITION, FOOT_PROFILE, LIFT, SLOTS, ReleasedSoleContract, intents, released
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, uid


BLOCKER = uid(990)
BLOCKER_POSITION = [ACTOR_POSITION[0] + .54, *ACTOR_POSITION[1:]]


def point(row):
    return tuple(axis*HEIGHT_UNIT for axis in row['center_units'])


def triangle_samples(row):
    cell, selector = divmod(row['triangle'], 2)
    z, x = divmod(cell, CELLS)
    return faces(x + CELLS*TILES.index(row['receiver']), z)[selector]


def adjacent_samples(triangle):
    """Find samples used by any face contributing to the held triangle's vertex normals."""
    return {
        point for x, z in triangle
        for j in range(max(z-1, 0), min(z+1, HEIGHT-1))
        for i in range(max(x-1, 0), min(x+1, WIDTH-1))
        for face in faces(i, j) if (x, z) in face
        for point in face
    }


def affected_samples(lattice, row):
    expected = deepcopy(lattice)
    assert expected.stamp(point(row), FOOT_PROFILE['radius_units'], FOOT_PROFILE['depth_units'])
    return {(x, z) for z in range(HEIGHT) for x in range(WIDTH)
            if lattice.depth[z][x] != expected.depth[z][x]}


class OverlapContract(ReleasedSoleContract):
    def author_pair(self):
        revision = self.author_knight()
        armature = json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig']
        add = self.world_call('world.transact', request_id=uid(901), base_revision=revision, ops=[
            dict(op='asset.instantiate', id=BLOCKER, asset=armature['asset'], name='Second Knight'),
            component(BLOCKER, 'Transform', transform(BLOCKER_POSITION)),
            component(BLOCKER, 'AnimationRig', armature),
        ])
        assert add['committed'] and add['revision'] == revision+1, add
        return self.enable_foot_release(add['revision'])

    def blocker_states(self, client, session, tick):
        states = []
        for slot in SLOTS:
            row = client.call('runtime.ik', dict(entity=BLOCKER, slot=slot, session_id=session, tick=tick,
                                        include_grounding=True, include_planting=True))
            assert row['session_id'] == session and row['tick'] == tick, row
            assert row['state']['grounding']['decision_tick'] == tick, row
            states.append(row['state'])
        return states

    def blocker_holds(self, states, lattice):
        holds = []
        for slot, state in zip(SLOTS, states, strict=True):
            grounding, plant = state['grounding'], state['planting']
            anchor, hit = plant['anchor'], grounding['hit']
            assert grounding['status'] == 'contact' and grounding['cast'] and plant['status'] == 'locked', state
            assert hit['entity'] == anchor['support'] == EAST and anchor['acquired_tick'] == 1, state
            assert plant['position_error'] <= FOOT_PROFILE['sole_tolerance_m'], state
            vector_near(plant['target_sole'], hit['position'], 2e-5)
            x, _, z = hit['position']
            height, normal, triangle = lattice.surface(x, z, EAST)
            vector_near(hit['position'], [x, height, z], 3e-5)
            vector_near(hit['normal'], normal, .002)
            assert anchor['triangle'] == hit['triangle'] == triangle, state
            holds.append(dict(rig=BLOCKER, slot=slot, acquired_tick=1, receiver=EAST, triangle=triangle,
                              center_units=[math.floor(axis/HEIGHT_UNIT+.5) for axis in (x, z)]))
        return holds

    def protected_rays(self, client, session, tick, lattice, states):
        results = []
        for state in states:
            plant = state['planting']
            anchor = plant['anchor']
            x, y, z = plant['target_sole']
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick) | down(x, z))['hit']
            height, normal, triangle = lattice.surface(x, z, anchor['support'])
            assert hit['entity'] == anchor['support'] and hit['triangle'] == anchor['triangle'] == triangle, hit
            vector_near([x, y, z], [x, height, z], 3e-5)
            vector_near(hit['position'], [x, height, z], 3e-5)
            vector_near(hit['normal'], normal, .002)
            results.append(hit)
        return results

    def finish(self, client, saves, session, label, second, blocked, safe, contacts):
        lattice = Lattice()
        self.automatic_stamp(lattice, safe)
        before_release = intents(holding=second, ready=[blocked], applied=1)
        snapshots = []
        # At tick 4 the blocked row is the only ready candidate. Tick 5 still
        # sees the old lock before its final animation solve clears the anchor.
        for tick in (3, 4):
            self.plain_step(client, session, tick, 920+tick,
                            **(dict(animations=[LIFT | dict(entity=BLOCKER)]) if tick == 4 else {}))
            states = self.blocker_states(client, session, tick+1)
            if tick == 3:
                expected = before_release
                assert exact([row['planting']['anchor'] for row in states],
                             [row['planting']['anchor'] for row in contacts]), states
                self.measurements[f'{label}_wait_rays'] = self.protected_rays(client, session, tick+1, lattice, states)
            else:
                assert all(row['grounding']['status'] == row['planting']['status'] == 'lifted' and
                           row['planting']['anchor'] is None for row in states), states
                expected = intents(ready=[blocked, *released(second, 5)], applied=1)
            snapshots.append(self.observe(client, session, tick+1, lattice, expected, f'{label} {tick+1}'))
        queue = [blocked, *released(second, 5)]
        for tick, event in enumerate(queue, start=5):
            self.plain_step(client, session, tick, 920+tick, retry=True)
            self.automatic_stamp(lattice, event)
            expected = intents(ready=queue[tick-4:], applied=tick-3)
            snapshots.append(self.observe(client, session, tick+1, lattice, expected, f'{label} {tick+1}'))
        assert lattice.generation == 4
        final = self.archive_release(client, saves, session, 8, 928, f'final-{label}', lattice,
                                     intents(applied=4))
        return snapshots, final

    def run(self):
        revision = self.author_pair()
        saves = self.root/'Overlap saves'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('warm overlap') as (client, warm):
            session = self.initialize(client, revision, uid(900), saves, None)
            self.plain_step(client, session, 0, 902)
            first = self.acquire_oracle(self.states(client, session, 1), lattice, 1)
            contacts = self.blocker_states(client, session, 1)
            second = self.blocker_holds(contacts, lattice)
            holding = sorted([*first, *second], key=lambda row: (row['rig'], row['slot']))
            self.observe(client, session, 1, lattice, intents(holding=holding), 'four genuine locks')
            self.measurements['initial_protected_rays'] = self.protected_rays(client, session, 1, lattice, contacts)
            blocked, safe = released(first, 2)
            assert blocked['receiver'] == EAST and safe['receiver'] == WEST
            pinned = set().union(*(adjacent_samples(triangle_samples(row)) for row in second))
            blocked_overlap = affected_samples(lattice, blocked) & pinned
            assert blocked_overlap and not affected_samples(lattice, safe) & pinned
            self.measurements['protected_samples'] = dict(pinned=sorted(pinned), changed_if_first_applied=sorted(blocked_overlap),
                                                           world_anchors=[row['planting']['anchor'] for row in contacts])
            capture_id = uid(931)
            client.call('profiler.start', dict(capture_id=capture_id, expected_capture_id=None, capacity=4096))
            self.plain_step(client, session, 1, 903, animations=[LIFT])
            states = self.states(client, session, 2)
            assert all(row['grounding']['status'] == row['planting']['status'] == 'lifted' for row in states), states
            expected = intents(holding=second, ready=[blocked, safe])
            self.observe(client, session, 2, lattice, expected, 'older conflicting stamp is first')
            self.plain_step(client, session, 2, 904, retry=True)
            self.automatic_stamp(lattice, safe)
            states = self.blocker_states(client, session, 3)
            assert exact([row['planting']['anchor'] for row in states], [row['planting']['anchor'] for row in contacts])
            self.measurements['bypass_protected_rays'] = self.protected_rays(client, session, 3, lattice, states)
            expected = intents(holding=second, ready=[blocked], applied=1)
            bypass = self.observe(client, session, 3, lattice, expected, 'safe stamp bypassed held sole')
            checkpoint = self.archive_release(client, saves, session, 3, 905, 'bypass', lattice, expected)
            warm_results, warm_save = self.finish(client, saves, session, 'warm', second, blocked, safe, contacts)
            sealed = client.call('profiler.stop', dict(capture_id=capture_id))
            assert sealed['dropped'] == 0 and sealed['open'] == 0, sealed
            cooks = []
            for offset in range(0, sealed['count'], 512):
                page = client.call('profiler.events', dict(capture_id=capture_id, offset=offset, limit=512))
                cooks.extend(event['tick'] for event in page['events'] if event['name'] == 'runtime.snow.prepare')
            # These are the pre-physics ticks. The short blocked window must
            # skip mesh preparation, as must requests replayed from receipts.
            assert cooks == [2, 5, 6, 7], cooks
            self.measurements['cooked_at_prephysics_ticks'] = cooks

        with self.owner('cold overlap') as (client, cold):
            fresh = self.initialize(client, revision, uid(906), saves, None)
            session = uid(907)
            assert self.load(client, revision, fresh, session, 'bypass', 3, 908) == 0
            assert exact(self.observe(client, session, 3, lattice, expected, 'cold bypass'), bypass)
            assert self.archive_release(client, saves, session, 3, 909, 'cold-bypass', lattice, expected) == checkpoint
            cold_results, cold_save = self.finish(client, saves, session, 'cold', second, blocked, safe, contacts)
            assert exact(self.measurements['cold_wait_rays'], self.measurements['warm_wait_rays'])
            assert exact(cold_results, warm_results) and cold_save == warm_save
        assert warm['pid'] != cold['pid']
        self.measurements['summary'] = dict(cold_checkpoints_exact=True, safe_bypass_tick=3, wait_tick=4,
                                            blocker_release_tick=5, oldest_ready_applied_tick=6,
                                            collected_final_tick=8, applied=4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--license', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-foot-release-overlap-', dir=args.output.resolve()))
    test = OverlapContract(args, root)
    result = dict(format='poima.snow-foot-release-overlap.v1', passed=False,
                  executable_sha256=file_hash(args.binary), harness_sha256=file_hash(Path(__file__)),
                  calls=test.calls, measurements=test.measurements)
    try:
        test.run()
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
