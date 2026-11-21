#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check snow contact ownership when an imported Knight instance is removed."""

import argparse
import json
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from runtime_ground_contacts import RIG
from snow_compaction_contract import HEIGHT_UNIT, Lattice, down, vector_near
from snow_foot_release_contract import LIFT, SLOTS, ReleasedSoleContract, intents, released
from world_terrain_contract import file_hash, uid


TEMPLATE = uid(950)


class StructureContract(ReleasedSoleContract):
    structure = 0

    def author_template(self):
        revision = self.author_knight()
        entities = json.loads(self.world.read_bytes())['entities']
        members = {}
        for key, value in entities.items():
            ancestor = key
            while ancestor is not None and ancestor != RIG:
                ancestor = entities[ancestor]['parent']
            if ancestor == RIG:
                members[key] = value
        assert members[RIG]['parent'] is None and len(members) > 57
        tx = self.world_call('world.transact', request_id=uid(951), base_revision=revision, ops=[
            dict(op='entity.delete', id=RIG, recursive=True),
            dict(op='template.set', id=TEMPLATE, name='Snow-walking Knight', root=RIG, entities=members),
        ])
        assert tx['committed'] and tx['revision'] == revision+1, tx
        return self.enable_foot_release(tx['revision']), set(members)

    def guard(self, session, tick, request):
        return super().guard(session, tick, request) | dict(expected_structure_revision=self.structure)

    def change(self, client, session, tick, request, **change):
        params = dict(session_id=session, expected_tick=tick, request_id=uid(request),
                      expected_structure_revision=self.structure) | change
        result = client.call('runtime.structure.transact', params)
        assert result['structure_revision'] == self.structure+1 and not result['replayed'], result
        assert exact(client.call('runtime.structure.transact', params), result | dict(replayed=True))
        self.structure = result['structure_revision']
        return result

    def spawn(self, client, session, tick, request, members):
        root, = self.change(client, session, tick, request, spawns=[dict(template_id=TEMPLATE)])['spawned']
        instance = client.call('runtime.instance', dict(session_id=session, tick=tick, id=root,
                                                        expected_structure_revision=self.structure))
        nodes = instance['nodes']
        assert set(nodes) == members and nodes[RIG] == root and len(set(nodes.values())) == len(members), instance
        assert not members & set(nodes.values()), instance
        return nodes

    def read_states(self, client, session, tick, rig):
        states = [client.call('runtime.ik', dict(session_id=session, tick=tick, entity=rig, slot=slot,
                                  include_grounding=True, include_planting=True))['state'] for slot in SLOTS]
        assert all(row['entity'] == rig and row['grounding']['decision_tick'] == tick for row in states), states
        return states

    def read_snow(self, client, session, tick, lattice, expected, label):
        result = client.call('runtime.snow.inspect', dict(session_id=session, tick=tick, structure_revision=self.structure))
        assert (result['session_id'], result['tick'], result['structure_revision']) == (session, tick, self.structure), result
        assert result['active'] is True and exact(result['generation'], lattice.generation), result
        assert exact(result['compacted_samples'], len(lattice.payload()['samples'])), result
        assert exact(result['lost_volume_units'], lattice.volume_units()) and exact(result['foot_release'], expected), result
        observed = {key: value for key, value in result.items() if key != 'session_id'}
        self.measurements.setdefault('observations', {})[label] = observed
        return observed

    def save_snow(self, client, saves, session, tick, request, slot, lattice, expected):
        raw = self.archive(client, saves, session, tick, request, slot, has_marks=False, structure=self.structure)
        payload = json.loads(raw)['snapshot']['payload']
        assert exact(payload['snow'], lattice.payload() | dict(version=2, foot_release=expected))
        assert payload['structure']['revision'] == self.structure
        return raw

    def consume(self, client, saves, session, queued, label):
        lattice = Lattice()
        observations = []
        for index, event in enumerate(queued):
            tick = 3+index
            self.plain_step(client, session, tick, 970+index, retry=True)
            self.automatic_stamp(lattice, event)
            expected = intents(ready=queued[index+1:], applied=index+1, cancelled=2)
            observed = self.read_snow(client, session, tick+1, lattice, expected, f'{label} applied {index+1}')
            x, z = (axis*HEIGHT_UNIT for axis in event['center_units'])
            y, normal, triangle = lattice.surface(x, z, event['receiver'])
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick+1) | down(x, z))['hit']
            assert hit['entity'] == event['receiver'] and hit['triangle'] == triangle, hit
            vector_near(hit['position'], [x, y, z], 3e-5)
            vector_near(hit['normal'], normal, .002)
            observations.append(dict(snow=observed, collision=hit))
        final = self.save_snow(client, saves, session, 5, 973, f'final-{label}', lattice, intents(applied=2, cancelled=2))
        self.measurements[f'{label}_consumption'] = observations
        return observations, final

    def run(self):
        revision, members = self.author_template()
        saves = self.root/'Structure saves'
        saves.mkdir()
        lattice = Lattice()
        with self.owner('warm structure') as (client, warm):
            session = self.initialize(client, revision, uid(952), saves, None)
            first = self.spawn(client, session, 0, 953, members)
            self.plain_step(client, session, 0, 954)
            first_states = self.read_states(client, session, 1, first[RIG])
            holding = [row | dict(rig=first[RIG]) for row in self.acquire_oracle(first_states, lattice, 1)]
            expected = intents(holding=holding)
            self.read_snow(client, session, 1, lattice, expected, 'first rig held')
            held = self.save_snow(client, saves, session, 1, 955, 'held', lattice, expected)
            child = next(identity for local, identity in first.items() if local != RIG)
            refusal = self.reject(client, 'runtime.structure.transact', dict(session_id=session, expected_tick=1,
                request_id=uid(956), expected_structure_revision=self.structure, despawns=[child]), -32602)
            assert exact(self.read_states(client, session, 1, first[RIG]), first_states)
            assert self.save_snow(client, saves, session, 1, 957, 'held-after-refusal', lattice, expected) == held
            self.change(client, session, 1, 958, despawns=[first[RIG]])
            self.read_snow(client, session, 1, lattice, intents(cancelled=2), 'immediate cancellation')
            self.reject(client, 'runtime.instance', dict(session_id=session, tick=1, id=first[RIG],
                        expected_structure_revision=self.structure), -32004)

            second = self.spawn(client, session, 1, 959, members)
            assert not set(first.values()) & set(second.values())
            self.read_snow(client, session, 1, lattice, intents(cancelled=2), 'replacement has no prior locks')
            self.plain_step(client, session, 1, 960)
            second_states = self.read_states(client, session, 2, second[RIG])
            holding = [row | dict(rig=second[RIG]) for row in self.acquire_oracle(second_states, lattice, 2)]
            self.read_snow(client, session, 2, lattice, intents(holding=holding, cancelled=2), 'replacement fresh locks')
            self.plain_step(client, session, 2, 961, animations=[LIFT | dict(entity=second[RIG])])
            lifted = self.read_states(client, session, 3, second[RIG])
            assert all(row['grounding']['status'] == row['planting']['status'] == 'lifted' and
                       row['planting']['anchor'] is None for row in lifted), lifted
            queued = released(holding, 3)
            expected = intents(ready=queued, cancelled=2)
            self.read_snow(client, session, 3, lattice, expected, 'replacement released')
            self.change(client, session, 3, 962, despawns=[second[RIG]])
            self.reject(client, 'runtime.instance', dict(session_id=session, tick=3, id=second[RIG],
                        expected_structure_revision=self.structure), -32004)
            retired = self.read_snow(client, session, 3, lattice, expected, 'ready prints survive retirement')
            ready = self.save_snow(client, saves, session, 3, 963, 'after-retirement', lattice, expected)
            cursor = json.loads(ready)['snapshot']['payload']['structure']
            assert cursor['spawned'] == [] and cursor['revision'] == 4
            assert int(cursor['next_entity_id'], 16) > int(max(second.values()), 16)
            warm_results, warm_save = self.consume(client, saves, session, queued, 'warm')

        with self.owner('cold retired source') as (client, cold):
            fresh = self.initialize(client, revision, uid(964), saves, None)
            session = uid(965)
            self.structure = self.load(client, revision, fresh, session, 'after-retirement', 3, 966)
            assert self.structure == 4
            self.reject(client, 'runtime.instance', dict(session_id=session, tick=3, id=second[RIG],
                        expected_structure_revision=self.structure), -32004)
            assert exact(self.read_snow(client, session, 3, lattice, expected, 'cold restored retired source'), retired)
            assert self.save_snow(client, saves, session, 3, 967, 'cold-retired', lattice, expected) == ready
            cold_results, cold_save = self.consume(client, saves, session, queued, 'cold')
            assert exact(cold_results, warm_results) and cold_save == warm_save
        assert warm['pid'] != cold['pid']
        self.measurements['structure_summary'] = dict(template_members=len(members), cancelled_holds=2,
            first_root=first[RIG], retired_ready_source=second[RIG], kept_ready_rows=len(queued),
            next_entity_id=cursor['next_entity_id'], structure_revision=4, refused_member_removal=refusal,
            immediate_and_final_cold_save_exact=True)


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
    root = Path(tempfile.mkdtemp(prefix='snow-foot-release-structure-', dir=args.output.resolve()))
    test = StructureContract(args, root)
    result = dict(format='poima.snow-foot-release-structure.v1', passed=False,
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
