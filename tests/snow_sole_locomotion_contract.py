#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify directional snow impressions from an uninterrupted original walk.

The original CC0 Walking_B clip runs at 1× on a prescribed native kinematic
path with a quarter turn and a crossfade to Idle. This headless fixture checks
contact and continuation, without qualifying a player controller or gait look.
"""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from character_library_intake import LibraryOracle
from imported_character_contract import multiply, point
from input_actions_gameplay_contract import exact
from runtime_ground_contacts import CALIBRATION, CHAINS, RIG
from snow_compaction_contract import HEIGHT_UNIT, TILES, THICKNESS, down, near, vector_near
from snow_foot_release_contract import intents
from snow_sole_shape_contract import PROFILE, SoleLattice, SoleShapeContract, heading, round_away
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, uid


ACTOR = uid(930)
WALK, IDLE, HZ = 73, 36, 60
PLANT, LIFT = .020, .076
FADE, SETTLE = 16, 8
# The imported in-place walk needs a fitted root pace around the turn. A fixed
# mount offset supplies knee bend on this grade without a live pelvis solver.
MOUNT_Y = -.077
START = ([.49, .009, .15], 0)
LEGS = (
    (30, [.49, .020, .57], 0),
    (48, [.59, .022, .66], 90),
    (67, [.685, .027, .66], 90),
    (104, [1.08, .032, .66], 90),
)
STOP = LEGS[-1][0]
FINAL = STOP + FADE + SETTLE


def yaw_rotation(degrees):
    half_yaw = math.radians(degrees)/2
    return [0, math.sin(half_yaw), 0, math.cos(half_yaw)]


def pose_on_path(tick):
    origin, incoming = START
    start = 0
    for end, target, heading_degrees in LEGS:
        if tick <= end:
            weight = (tick-start)/(end-start)
            return ([a+(b-a)*weight for a, b in zip(origin, target, strict=True)],
                    incoming+(heading_degrees-incoming)*weight)
        origin, incoming, start = target, heading_degrees, end
    return origin, incoming


def commands_at(tick):
    start = 0
    for end, target, angle in LEGS:
        if tick == start+1:
            return dict(motions=[dict(entity=ACTOR, position=target,
                                      rotation=yaw_rotation(angle), duration_ticks=end-start)])
        start = end
    if tick == STOP+1:
        return dict(animations=[dict(entity=RIG, clip=IDLE, time=0, speed=1, loop=True,
                                     playing=True, blend_ticks=FADE)])
    return {}


def angular_distance(a, b):
    return min((a-b) % 256, (b-a) % 256)


class LocomotionContract(SoleShapeContract):
    def author(self):
        revision = self.author_knight()
        rig = json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig']
        rig.update(clip=WALK, time=0, speed=1, loop=True, playing=True)
        for chain in rig['ik']:
            chain['grounding'].update(plant_height=PLANT, lift_height=LIFT)
        operations = [
            dict(op='entity.create', id=ACTOR, name='Kinematic walk path'),
            component(ACTOR, 'Transform', transform(START[0])),
            component(ACTOR, 'BoxCollider', dict(motion='kinematic', half_extents=[.025]*3,
                                                mass=1, friction=.5, restitution=0)),
            component(RIG, 'Transform', transform([0, MOUNT_Y, 0])),
            dict(op='entity.reparent', id=RIG, parent=ACTOR, mode='keep_local'),
            component(RIG, 'AnimationRig', rig),
            dict(op='snow.set', definition=dict(tiles=TILES, thickness_units=THICKNESS, foot_release=PROFILE)),
        ]
        result = self.world_call('world.transact', request_id=uid(940), base_revision=revision, ops=operations)
        assert result['committed'] and result['revision'] == revision+1, result
        return result['revision']

    def rig_pose(self, client, session, tick):
        rig = client.call('runtime.entity', dict(session_id=session, tick=tick, id=RIG))
        world = rig['world_matrix']
        expected_position, angle = pose_on_path(tick)
        expected_position = [expected_position[0], expected_position[1]+MOUNT_Y, expected_position[2]]
        theta = math.radians(angle)
        vector_near(world[12:15], expected_position, 3e-6)
        # Jolt interpolates single-precision orientations with approximated trig.
        vector_near(world[8:11], [math.sin(theta), 0, math.cos(theta)], 2e-5)
        animation = rig['animation']
        assert animation['speed'] == 1 and animation['playing'] is True and animation['loop'] is True, animation
        clip = WALK if tick <= STOP else IDLE
        assert animation['clip'] == clip, animation
        clock = (tick if clip == WALK else tick-STOP)/HZ % self.oracle.clips[clip]['duration']
        near(animation['time'], clock, 1e-7)
        progress = animation['transition']
        if STOP < tick < STOP+FADE:
            assert progress is not None and progress['elapsed_ticks'] == tick-STOP, animation
        else:
            assert progress is None, animation
        # During the stop blend there is no single source pose to compare.
        source = None if progress is not None else self.oracle.pose(clip, clock)[2]
        return world, animation, source

    def foot(self, state, end, calibration, world, source):
        grounding, plant = state['grounding'], state['planting']
        forward = None
        if source is not None:
            sole = point(source[end], calibration['sole_offset'])
            vector_near(grounding['input_sole'], point(world, sole), 4e-6)
            assert (grounding['status'] == 'lifted') == (sole[1] >= LIFT), (grounding, sole)
            matrix = multiply(world, source[end])
            forward = [sum(matrix[4*j+i]*calibration['sole_forward'][j] for j in range(3)) for i in range(3)]
        if plant['status'] == 'locked':
            actual, anchor = grounding['actual_sole'], plant['anchor']
            near(math.dist(actual, plant['target_sole']), plant['position_error'], 4e-7)
            assert anchor is not None and grounding['cast'] and grounding['hit']['entity'] == anchor['support'], state
        return forward

    def anchor(self, row, state, source_forward, lattice):
        plant, grounding = state['planting'], state['grounding']
        anchor, hit = plant['anchor'], grounding['hit']
        assert plant['status'] == 'locked' and plant['position_error'] <= PROFILE['sole_tolerance_m'], state
        assert anchor['support'] == row['receiver'] == hit['entity']
        assert anchor['triangle'] == row['triangle'] == hit['triangle']
        x, y, z = hit['position']
        surface, normal, face = lattice.surface(x, z, row['receiver'])
        assert face == row['triangle'], (row, hit, face)
        near(y, surface, 3e-5)
        vector_near(anchor['local_point'], hit['position'], 3e-5)
        vector_near(anchor['local_normal'], normal, .002)
        assert exact(row['center_units'], [round_away(x/HEIGHT_UNIT), round_away(z/HEIGHT_UNIT)]), (row, hit)
        assert row['heading_bin'] == heading(anchor['local_forward']), (row, anchor)
        if source_forward is not None:
            perpendicular = sum(a*b for a, b in zip(source_forward, normal, strict=True))
            projected = [a-perpendicular*b for a, b in zip(source_forward, normal, strict=True)]
            assert row['heading_bin'] == heading(projected), (row, projected)

    def timeline(self, client, session, start, end, lattice, previous, record):
        for tick in range(start+1, end+1):
            self.plain_step(client, session, tick-1, 1000+tick, **commands_at(tick))
            world, animation, source = self.rig_pose(client, session, tick)
            states = self.states(client, session, tick)
            forwards = [self.foot(state, chain[-1], calibration, world, source)
                        for state, chain, calibration in zip(states, CHAINS, CALIBRATION, strict=True)]
            guard = dict(session_id=session, tick=tick, structure_revision=0)
            current = client.call('runtime.snow.inspect', guard | dict(samples=[]))['foot_release']
            current_held = [row for row in current['holding'] if row['acquired_tick'] == tick]
            by_slot = {chain[0]: index for index, chain in enumerate(CHAINS)}
            new_ready = []
            cancelled = 0
            for row in previous['holding']:
                state = states[by_slot[row['slot']]]
                plant = state['planting']
                if plant['status'] == 'locked':
                    assert row in current['holding'], (tick, row, current)
                    assert heading(plant['anchor']['local_forward']) == row['heading_bin'], (tick, row, plant)
                elif plant['status'] == state['grounding']['status'] == 'lifted':
                    new_ready.append(row | dict(released_tick=tick))
                else:
                    cancelled += 1
            expected_counts = previous['counts'] | dict(cancelled=previous['counts']['cancelled']+cancelled)
            new_ready.sort(key=lambda row: (row['acquired_tick'], row['rig'], row['slot']))
            ran = []
            if current['counts']['applied'] > expected_counts['applied']:
                expected_counts['applied'] += 1
                ran = [row for row in previous['ready'] if row not in current['ready']]
                assert len(ran) == 1 and lattice.shaped_stamp(ran[0]) > 0, (tick, previous, current)
            elif current['counts']['saturated'] > expected_counts['saturated']:
                expected_counts['saturated'] += 1
                ran = [row for row in previous['ready'] if row not in current['ready']]
                assert len(ran) == 1 and lattice.shaped_stamp(ran[0]) == 0, (tick, previous, current)
            assert exact(current['counts'], expected_counts), (tick, previous, current)
            assert exact(current['ready'], [row for row in previous['ready'] if row not in ran]+new_ready)
            for row in current_held:
                index = by_slot[row['slot']]
                self.anchor(row, states[index], forwards[index], lattice)
            assert exact(self.snow(client, session, tick, lattice)['foot_release'], current)
            for row in ran:
                # The integer center can coincide with an internal triangle
                # edge. Observe just inside the receiver carrying the mark.
                x, z = (value*HEIGHT_UNIT for value in row['center_units'])
                x += .00013 if x != 1 or row['receiver'] == TILES[1] else -.00013
                z += .00017
                hit = client.call('runtime.raycast', dict(session_id=session, tick=tick) | down(x, z))['hit']
                y, normal, face = lattice.surface(x, z, hit['entity'])
                assert hit['entity'] == row['receiver'] and hit['triangle'] == face, (row, hit)
                vector_near(hit['position'], [x, y, z], 3e-5)
                vector_near(hit['normal'], normal, .002)
            record.append(dict(tick=tick, actor_matrix=world, animation=animation, feet=states,
                               acquired=current_held, released=new_ready, applied=ran, snow=current))
            previous = current
            yield tick, current

    def run(self):
        self.oracle = LibraryOracle(self.args.source.read_bytes())
        assert self.oracle.clips[WALK]['name'] == 'Walking_B' and self.oracle.clips[IDLE]['name'] == 'Idle'
        self.measurements['contract'] = dict(clip='Walking_B', source_time_seconds=self.oracle.clips[WALK]['duration'],
            fixed_hz=HZ, path=LEGS, rig_mount_y_m=MOUNT_Y, stop_tick=STOP, final_tick=FINAL, fade_ticks=FADE,
            contact_thresholds_m=dict(plant=PLANT, lift=LIFT))
        revision = self.author()
        saves = self.root/'walk saves'
        saves.mkdir()
        warm = self.measurements['warm'] = []
        lattice = SoleLattice()
        checkpoint = None
        with self.owner('warm walk') as (client, warm_owner):
            session = self.initialize(client, revision, uid(949), saves, None)
            for tick, snow in self.timeline(client, session, 0, FINAL, lattice, intents(), warm):
                if checkpoint is None and snow['ready']:
                    saved = self.archive_shape(client, saves, session, tick, 1950, 'walking', lattice, snow)
                    checkpoint = (tick, snow, saved, json.loads(json.dumps(lattice.depth)), lattice.generation)
            final = self.archive_shape(client, saves, session, FINAL, 1951, 'completed', lattice, snow)

        max_yaw_error = max(abs(math.degrees(math.atan2(row['actor_matrix'][8], row['actor_matrix'][10]))
                                - pose_on_path(row['tick'])[1]) for row in warm)
        self.measurements['max_path_yaw_error_degrees'] = max_yaw_error
        assert max_yaw_error < .0012, max_yaw_error
        assert snow['counts']['cancelled'] == snow['counts']['saturated'] == 0, snow
        applied = [(row['tick'], print_) for row in warm for print_ in row['applied']]
        assert len(applied) >= 3 and {item['slot'] for _, item in applied} == {1, 2}, applied
        before = [item for _, item in applied if item['acquired_tick'] < LEGS[0][0]]
        after = [item for _, item in applied if item['acquired_tick'] > LEGS[1][0]]
        assert before and after and any(angular_distance(a['heading_bin'], b['heading_bin']) >= 48
                                       for a in before for b in after), applied
        assert all(tick > item['released_tick'] > item['acquired_tick'] for tick, item in applied)
        assert all(not row['applied'] and not row['released'] for row in warm[-SETTLE:])
        assert checkpoint is not None
        tick, snow, saved, depth, generation = checkpoint
        self.measurements['impressions'] = applied

        cold = self.measurements['cold'] = []
        lattice = SoleLattice()
        lattice.depth, lattice.generation = depth, generation
        with self.owner('cold walk') as (client, cold_owner):
            active = self.initialize(client, revision, uid(1952), saves, None)
            session = uid(1953)
            assert self.load(client, revision, active, session, 'walking', tick, 1954) == 0
            assert saved == self.archive_shape(client, saves, session, tick, 1955, 'restored-walking', lattice, snow)
            for _, snow in self.timeline(client, session, tick, FINAL, lattice, snow, cold):
                pass
            assert exact(warm[tick:], cold)
            assert final == self.archive_shape(client, saves, session, FINAL, 1956, 'restored-completed', lattice, snow)
        assert warm_owner['pid'] != cold_owner['pid']
        self.measurements['restored_tick'] = tick


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--license', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.windows_interop = False
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-sole-walk-', dir=args.output.resolve()))
    contract = LocomotionContract(args, root)
    report = dict(format='poima.snow-sole-locomotion.v1', passed=False, executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)), calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        evidence = root/'evidence.json'
        evidence.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {evidence}')


if __name__ == '__main__':
    main()
