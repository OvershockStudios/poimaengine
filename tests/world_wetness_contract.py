#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check native rain exposure, retained film, guarded steps, and exact saves."""
import argparse
from dataclasses import dataclass
from fractions import Fraction
import json
import math
from pathlib import Path
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from snow_foot_release_save_refusals import reseal_slot
from surface_marks_capture import SurfaceMarksCapture
from world_particles_contract import component, near_vector, transform
from world_terrain_contract import error, file_hash, rpc, success, tree_hashes, uid


OPEN, COVERED, PROP, ROOF, MOVING_ROOF, DYNAMIC_ROOF, CAMERA = map(uid, range(510, 517))
RECEIVERS = (OPEN, COVERED, PROP)
OPEN_X, COVERED_X = -1.55, 1.55
PROP_OPEN = [OPEN_X, .2, 1.55]
PROP_COVERED = [COVERED_X, .2, 1.55]
ROOF_POSITION, ROOF_SCALE = [COVERED_X, 2.14, .78], [1.28, .16, 2.8]
MOVING_ROOF_AWAY = [-3.25, 1.95, 0]
MOVING_ROOF_NEAR = [OPEN_X, 1.95, 0]
MOVING_ROOF_SCALE = [.72, .2, 1.0]
DYNAMIC_ROOF_POSITION = [OPEN_X, 8, 0]
POSITIONS = {
    OPEN: [OPEN_X, .15, 0], COVERED: [COVERED_X, .15, 0], PROP: PROP_OPEN,
    ROOF: ROOF_POSITION, MOVING_ROOF: MOVING_ROOF_AWAY, DYNAMIC_ROOF: DYNAMIC_ROOF_POSITION,
}
CAMERA_POSITION = [0, 3, 6]
CAMERA_ROTATION = [-math.sin(math.radians(11)), 0, 0, math.cos(math.radians(11))]
CAMERA_LENS = dict(vertical_fov=52, near=.1, far=30)
LIGHT_ROTATION = [-math.sqrt(.5), 0, 0, math.sqrt(.5)]
AMBIENT, INTENSITY = .18, 1.3
BASE = [.36, .42, .5]
MATERIAL = dict(base_color=BASE, emissive=[0, 0, 0], metallic=0, roughness=.78, double_sided=False)
DRAWABLE = dict(primitive='box', albedo=BASE, visible=True, cast_shadows=False, receive_shadows=False)
COLLIDER = dict(motion='static', half_extents=[.5]*3, mass=1, friction=.5, restitution=0)
PROFILE = dict(capacity_nm=6000, retention_q16=32768, drying_nm_per_second=1000,
               # This origin is inside the receiver's own collider.
               probe_local_m=[0, .29, 0], darkening=.55, roughness_factor=.36)
FIRST_QUANTUM, Q16 = 800, 65536
LAST_QUANTUM = 315_359_999_998
WETNESS = ('snapshot', 'payload', 'wetness')


@dataclass(frozen=True)
class Phase:
    name: str
    rain: tuple[int, ...]
    ticks: int = 1
    approach_ticks: int = 0
    targets: tuple[tuple[str, list[float]], ...] = ()


PHASES = (
    Phase('rain', (5000, 4500)),
    Phase('under-roof', (6000,), approach_ticks=3, targets=((PROP, PROP_COVERED),)),
    Phase('drying', (1, 0), ticks=2),
    Phase('sliding-roof', (1_000_000,), approach_ticks=3,
          targets=((PROP, PROP_OPEN), (MOVING_ROOF, MOVING_ROOF_NEAR))),
    Phase('saturation', (1_000_000,), approach_ticks=1, targets=((MOVING_ROOF, MOVING_ROOF_AWAY),)),
    Phase('clear', (0, 0), ticks=2),
)


def motion(entity, position, duration):
    return dict(entity=entity, position=position, rotation=[0, 0, 0, 1], duration_ticks=duration)


def shelter(position, positions):
    """Independent vertical slab intersection against the two authored covers."""
    x, y, z = (a+b for a, b in zip(position, PROFILE['probe_local_m'], strict=True))
    hits = []
    for entity, scale in ((ROOF, ROOF_SCALE), (MOVING_ROOF, MOVING_ROOF_SCALE)):
        bx, by, bz = positions[entity]
        hx, hy, hz = (value/2 for value in scale)
        if abs(x-bx) < hx and abs(z-bz) < hz and 0 <= by-hy-y <= 100:
            hits.append((by-hy-y, entity))
    return min(hits)[1] if hits else None


class Reference:
    def __init__(self):
        self.tick = 0
        self.next_quantum = None
        self.positions = POSITIONS.copy()
        self.film = dict.fromkeys(RECEIVERS, Fraction(0))
        self.blockers = dict.fromkeys(RECEIVERS)

    def copy(self):
        result = Reference()
        result.tick = self.tick
        result.next_quantum = self.next_quantum
        result.positions = self.positions.copy()
        result.film = self.film.copy()
        result.blockers = self.blockers.copy()
        return result

    def finish(self, phase):
        self.tick += phase.approach_ticks + phase.ticks
        self.positions.update(phase.targets)
        first = self.next_quantum if self.next_quantum is not None else FIRST_QUANTUM
        self.next_quantum = first + len(phase.rain)
        for entity in RECEIVERS:
            blocker = shelter(self.positions[entity], self.positions)
            for dose in phase.rain:
                water = self.film[entity] + (Fraction(dose, 2) if blocker is None else 0)
                self.film[entity] = max(Fraction(0), min(Fraction(6000), water - 1000))
            self.blockers[entity] = blocker

    def rows(self):
        rows = []
        for entity in RECEIVERS:
            amount = self.film[entity] * Q16
            assert amount.denominator == 1, amount
            blocker = self.blockers[entity]
            rows.append(dict(entity=entity, film_nm_q16=amount.numerator,
                             capacity_nm=PROFILE['capacity_nm'], blocker=blocker,
                             exposed=None if self.next_quantum is None else blocker is None))
        return rows

    def saved(self):
        return dict(version=1, next_quantum=self.next_quantum,
                    receivers=[{key: row[key] for key in ('entity', 'film_nm_q16', 'blocker')}
                               for row in self.rows()])


class WetnessContract(SurfaceMarksCapture):
    def author(self):
        success(json.loads(self.cli('project', 'create', self.native(self.project), '--name', 'Sheltered Surface Wetness')))
        revision = self.world_call('world.inspect')['revision']
        self.discovery()
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            component(uid(4), 'LightingEnvironment', dict(ambient=[AMBIENT]*3, exposure=1)),
            component(uid(5), 'Transform', transform([0, 0, 0], LIGHT_ROTATION)),
            component(uid(5), 'Light', dict(kind='directional', color=[1, 1, 1], intensity=INTENSITY, enabled=True)),
            dict(op='entity.create', id=CAMERA, name='Wetness verification camera'),
            component(CAMERA, 'Transform', transform(CAMERA_POSITION, CAMERA_ROTATION)),
            component(CAMERA, 'Camera', CAMERA_LENS),
        ]
        for entity, name, motion_type in ((OPEN, 'Exposed plate', 'static'), (COVERED, 'Roofed plate', 'static'),
                                           (PROP, 'Movable wet crate', 'kinematic')):
            ops += [dict(op='entity.create', id=entity, name=name),
                    component(entity, 'Transform', transform(POSITIONS[entity])),
                    component(entity, 'MeshRenderer', DRAWABLE),
                    component(entity, 'PbrMaterial', MATERIAL),
                    component(entity, 'BoxCollider', COLLIDER | dict(motion=motion_type)),
                    component(entity, 'SurfaceWetness', PROFILE)]
        for entity, name, motion_type, scale in (
                (ROOF, 'Fixed shelter', 'static', ROOF_SCALE),
                (MOVING_ROOF, 'Sliding shelter', 'kinematic', MOVING_ROOF_SCALE),
                (DYNAMIC_ROOF, 'Dynamic overhead control', 'dynamic', [1, 1, 1])):
            ops += [dict(op='entity.create', id=entity, name=name),
                    component(entity, 'Transform', transform(POSITIONS[entity]) | dict(scale=scale)),
                    component(entity, 'BoxCollider', COLLIDER | dict(motion=motion_type))]
            if entity != DYNAMIC_ROOF:
                albedo = [.18, .12, .09]
                ops += [component(entity, 'MeshRenderer', DRAWABLE | dict(albedo=albedo)),
                        component(entity, 'PbrMaterial', MATERIAL | dict(base_color=albedo))]
        authored = self.world_call('world.transact', request_id=uid(1800), base_revision=revision, ops=ops)
        assert authored['committed'] and authored['revision'] == revision+1, authored
        return authored['revision']

    def discovery(self):
        # The full step schema is large. Use separate bounded selections.
        inspection = self.world_call('world.describe.select', methods=['runtime.wetness.inspect'],
                                     components=['SurfaceWetness'], sections=['wetness'])
        assert set(PROFILE) == set(inspection['components']['SurfaceWetness']['required']), inspection
        assert set(inspection['methods']['runtime.wetness.inspect']['required']) == {
            'session_id', 'tick', 'structure_revision'}, inspection
        step = self.world_call('world.describe.select', methods=['runtime.wetness.step'])
        spec = step['methods']['runtime.wetness.step']
        assert 'wetness_dose' in spec['required'] and 'snow_stamp' not in spec['properties'], spec
        rain = spec['properties']['wetness_dose']['properties']['rain_nm']
        assert rain['minItems'] == 1 and rain['maxItems'] == 64 and rain['items']['type'] == 'integer', rain
        plain = self.world_call('world.describe.select', methods=['runtime.step'])
        assert 'wetness_dose' not in plain['methods']['runtime.step']['properties'], plain

    def author_refusals(self, revision):
        original = self.world.read_bytes()
        cases = {
            'bool-capacity': [component(OPEN, 'SurfaceWetness', PROFILE | dict(capacity_nm=True))],
            'too-fast-drying': [component(OPEN, 'SurfaceWetness', PROFILE | dict(drying_nm_per_second=6001))],
            'probe-too-far': [component(OPEN, 'SurfaceWetness', PROFILE | dict(probe_local_m=[0, 16.01, 0]))],
            'missing-profile-field': [component(OPEN, 'SurfaceWetness',
                                                {key: v for key, v in PROFILE.items() if key != 'darkening'})],
            'missing-pbr': [dict(op='component.remove', id=OPEN, type='PbrMaterial')],
            'catalog-receiver': [dict(op='template.set', id=uid(520), name='Unbounded template receiver',
                                      components={'Transform': transform([0, 1, 0]), 'MeshRenderer': DRAWABLE,
                                                  'PbrMaterial': MATERIAL, 'SurfaceWetness': PROFILE})],
        }
        failures = {}
        for index, (name, ops) in enumerate(cases.items()):
            reply, = self.world_calls(rpc('world.transact', request_id=uid(1810+index),
                                         base_revision=revision, ops=ops))
            failures[name] = error(reply, -32602)
            assert self.world.read_bytes() == original
        self.measurements['authoring_refusals'] = failures

    @staticmethod
    def guard(session, tick, request, ticks=1):
        return dict(session_id=session, request_id=uid(request), expected_tick=tick,
                    expected_structure_revision=0, ticks=ticks)

    def inspect(self, client, session, tick, **scope):
        query = dict(session_id=session, tick=tick, structure_revision=0)
        result = client.call('runtime.wetness.inspect', query | scope)
        assert exact([result['session_id'], result['tick'], result['structure_revision']], [session, tick, 0]), result
        return dict(next_quantum=result['next_quantum'], rows=result['receivers'])

    def verify_reading(self, client, session, reference):
        report = self.inspect(client, session, reference.tick)
        expected = dict(next_quantum=reference.next_quantum, rows=reference.rows())
        assert exact(report, expected), (reference.tick, report, expected)
        filtered = self.inspect(client, session, reference.tick, entity=PROP)
        assert exact(filtered, expected | dict(rows=[expected['rows'][-1]])), filtered
        for entity in (PROP, MOVING_ROOF):
            live = client.call('runtime.entity', dict(session_id=session, tick=reference.tick, id=entity))
            near_vector(live['world_matrix'][12:15], reference.positions[entity])
        return report

    def observe_phase(self, client, session, reference, label):
        """Override to add render evidence in a native Windows capture fixture."""
        reading = self.verify_reading(client, session, reference)
        self.measurements.setdefault('phases', {})[label] = dict(tick=reference.tick, **reading)
        return reading

    def advance(self, client, session, before, phase, request, *, retry=False):
        if phase.approach_ticks:
            # End the motion on the wetness tick. Observe the pre-dose pose
            # before the body or roof crosses the shelter boundary.
            positions_before = before.positions.copy()
            targets = [motion(entity, position, phase.approach_ticks+1) for entity, position in phase.targets]
            approach = self.guard(session, before.tick, request, phase.approach_ticks) | dict(motions=targets)
            stepped = client.call('runtime.step', approach)
            assert stepped['current_tick'] == before.tick+phase.approach_ticks, stepped
            unchanged = self.inspect(client, session, stepped['current_tick'])
            assert exact(unchanged, dict(next_quantum=before.next_quantum, rows=before.rows())), unchanged
            fraction = phase.approach_ticks/(phase.approach_ticks+1)
            for entity, target in phase.targets:
                expected = [a+(b-a)*fraction for a, b in zip(positions_before[entity], target, strict=True)]
                live = client.call('runtime.entity', dict(session_id=session, tick=stepped['current_tick'], id=entity))
                near_vector(live['world_matrix'][12:15], expected)
        start = before.tick+phase.approach_ticks
        first = before.next_quantum if before.next_quantum is not None else FIRST_QUANTUM
        request_dose = self.guard(session, start, request+1, phase.ticks) | dict(
            wetness_dose=dict(first_quantum=first, rain_nm=list(phase.rain)))
        result = client.call('runtime.wetness.step', request_dose)
        assert exact([result['current_session_id'], result['current_tick'], result['stepped']],
                     [session, start+phase.ticks, phase.ticks]), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        reference = before.copy()
        reference.finish(phase)
        if retry:
            repeated = client.call('runtime.wetness.step', request_dose)
            assert exact(repeated, result | dict(replayed=True)), repeated
            conflict = request_dose | dict(wetness_dose=dict(first_quantum=first, rain_nm=[1, *phase.rain[1:]]))
            refusal = self.reject(client, 'runtime.wetness.step', conflict, -32010)
            self.measurements['changed_retry'] = refusal
        return reference

    def archive_wetness(self, client, saves, session, reference, request, slot):
        raw = super().archive(client, saves, session, reference.tick, request, slot, has_marks=False)
        payload = json.loads(raw)['snapshot']['payload']
        assert exact(payload['wetness'], reference.saved()), (payload['wetness'], reference.saved())
        return raw

    def check_overhead_control(self, client, session):
        result = client.call('runtime.raycast', dict(session_id=session, tick=0,
            origin=[OPEN_X, .15+PROFILE['probe_local_m'][1], 0],
            direction=[0, 1, 0], distance=100, ignore=[OPEN]))
        assert result['hit']['entity'] == DYNAMIC_ROOF, result
        self.measurements['dynamic_overhead_ray'] = result['hit']

    def step_refusals(self, client, saves, session, state, original):
        tick, first = state.tick, state.next_quantum
        bad_doses = {
            'empty': dict(first_quantum=first, rain_nm=[]),
            'too-long': dict(first_quantum=first, rain_nm=[0]*65),
            'boolean-amount': dict(first_quantum=first, rain_nm=[True]),
            'floating-amount': dict(first_quantum=first, rain_nm=[1.0]),
            'amount-range': dict(first_quantum=first, rain_nm=[1_000_001]),
            'boolean-index': dict(first_quantum=False, rain_nm=[0]),
            'terminal-overflow': dict(first_quantum=315_359_999_998, rain_nm=[0, 0]),
        }
        failures = {name: self.reject(client, 'runtime.wetness.step', self.guard(session, tick, 1900+index) |
                                     dict(wetness_dose=value), -32602)
                    for index, (name, value) in enumerate(bad_doses.items())}
        legal = dict(first_quantum=first, rain_nm=[6000])
        failures['wrong-method'] = self.reject(client, 'runtime.step',
            self.guard(session, tick, 1920) | dict(wetness_dose=legal), -32602)
        failures['missing-dose'] = self.reject(client, 'runtime.wetness.step',
            self.guard(session, tick, 1921), -32602)
        failures['skip-quantum'] = self.reject(client, 'runtime.wetness.step',
            self.guard(session, tick, 1922) | dict(wetness_dose=legal | dict(first_quantum=first+1)), -32040)
        failures['reapply-quantum'] = self.reject(client, 'runtime.wetness.step',
            self.guard(session, tick, 1923) | dict(wetness_dose=legal | dict(first_quantum=first-1)), -32040)
        failures['stale-tick'] = self.reject(client, 'runtime.wetness.inspect',
            dict(session_id=session, tick=tick-1, structure_revision=0), -32009)
        failures['stale-structure'] = self.reject(client, 'runtime.wetness.inspect',
            dict(session_id=session, tick=tick, structure_revision=1), -32009)
        # A valid mark request whose ray misses after simulation has advanced.
        mark = dict(receiver=PROP, ray=dict(origin=[40, 4, 40], direction=[0, -1, 0], distance=6),
                    half_extent_m=[.12, .12], half_depth_m=.05, minimum_normal_dot=.8,
                    edge_fraction=.25, linear_rgb=[.18, .1, .12], opacity=1, roughness=.6)
        failed = self.guard(session, tick, 1924) | dict(
            wetness_dose=legal, motions=[motion(PROP, [OPEN_X+.5, .2, 1.55], 1)], surface_marks=[mark])
        failures['late-ray-miss'] = self.reject(client, 'runtime.wetness.step', failed, -32040)
        assert 'must first hit the expected receiver' in failures['late-ray-miss']['message'], failures['late-ray-miss']
        self.verify_reading(client, session, state)
        assert original == self.archive_wetness(client, saves, session, state, 1925, 'after-step-refusals')
        self.measurements['step_refusals'] = failures

    def save_refusals(self, client, saves, revision, session, state, original):
        stored_slot = 'checkpoint'
        source = saves/('slot-'+stored_slot)
        unchanged = tree_hashes(source)
        failures = {}
        cases = (
            ('wrong-version', ('version',), True),
            ('film-over-capacity', ('receivers', 0, 'film_nm_q16'), 6000*Q16+1),
            ('float-film', ('receivers', 0, 'film_nm_q16'), 1.0),
            ('wrong-entity', ('receivers', 0, 'entity'), DYNAMIC_ROOF),
            ('invalid-blocker', ('receivers', 0, 'blocker'), False),
            ('invalid-cursor', ('next_quantum',), 315_360_000_000),
            ('unstarted-nonzero-film', ('next_quantum',), None),
        )
        for index, (name, path, value) in enumerate(cases):
            identity = reseal_slot(source, saves, name, WETNESS+path, value)
            inspected = client.call('save.inspect', dict(slot=name))
            assert inspected['recovered'] is False and inspected['manifest_recovered'] is False, inspected
            assert exact(inspected['selected'], identity | dict(verified=True)), inspected
            request = dict(request_id=uid(1930+index), configuration_generation=1, slot=name,
                           expected_generation=1, revision=revision, expected_session_id=session,
                           expected_tick=state.tick, expected_gameplay_revision=0, expected_structure_revision=0,
                           new_session_id=uid(1940+index))
            refusal = self.reject(client, 'save.load', request, -32070)
            reading = self.verify_reading(client, session, state)
            failures[name] = dict(identity=identity, rejection=refusal, reading=reading)
        assert original == self.archive_wetness(client, saves, session, state, 1950, 'after-save-refusals')
        assert tree_hashes(source) == unchanged
        self.measurements['save_refusals'] = failures

    def exercise(self, revision, game=None):
        saves = self.root/'External Wetness Saves'
        saves.mkdir()
        with self.owner('warm wetness', game=game, saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(1970), saves, game)
            state = Reference()
            self.check_overhead_control(client, session)
            self.observe_phase(client, session, state, 'warm initial')
            for index, phase in enumerate(PHASES):
                state = self.advance(client, session, state, phase, 2000+10*index, retry=index == 0)
                self.observe_phase(client, session, state, 'warm '+phase.name)
                if index == 0:
                    first = self.archive_wetness(client, saves, session, state, 2060, 'first-rain')
                    if game is None:
                        self.step_refusals(client, saves, session, state, first)
                elif phase.name == 'under-roof':
                    checkpoint = state.copy()
                    checkpoint_bytes = self.archive_wetness(client, saves, session, state, 2061, 'checkpoint')
                    if game is None:
                        self.save_refusals(client, saves, revision, session, state, checkpoint_bytes)
                elif phase.name == 'drying':
                    grouped_bytes = self.archive_wetness(client, saves, session, state, 2062, 'grouped-drying')
            final_bytes = self.archive_wetness(client, saves, session, state, 2063, 'warm-final')
            assert [state.film[e] for e in RECEIVERS] == [Fraction(4000), Fraction(0), Fraction(4000)]

        with self.owner('cold wetness', game=game, saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(2070), saves, game)
            session = uid(2071)
            assert self.load(client, revision, previous, session, 'checkpoint', checkpoint.tick, 2072) == 0
            self.observe_phase(client, session, checkpoint, 'cold under-roof')
            assert checkpoint_bytes == self.archive_wetness(client, saves, session, checkpoint, 2073, 'restored-checkpoint')
            resumed = checkpoint
            for index, phase in enumerate(PHASES[2:]):
                resumed = self.advance(client, session, resumed, phase, 2100+10*index)
                self.observe_phase(client, session, resumed, 'cold '+phase.name)
            assert final_bytes == self.archive_wetness(client, saves, session, resumed, 2140, 'cold-final')
        assert warm['pid'] != cold['pid'], (warm, cold)
        if game is None:
            with self.owner('cold partition', saves=saves) as (client, partition):
                previous = self.initialize(client, revision, uid(2150), saves, None)
                session = uid(2151)
                assert self.load(client, revision, previous, session, 'checkpoint', checkpoint.tick, 2152) == 0
                first_half = self.advance(client, session, checkpoint, Phase('one-second', (1,)), 2160)
                second_half = self.advance(client, session, first_half, Phase('another-second', (0,)), 2170)
                self.verify_reading(client, session, second_half)
                assert grouped_bytes == self.archive_wetness(client, saves, session, second_half, 2180, 'split-drying')
            assert len({row['pid'] for row in (warm, cold, partition)}) == 3

    def run(self):
        revision = self.author()
        self.author_refusals(revision)
        self.exercise(revision)
        self.clock_limit(revision)

    def clock_limit(self, revision):
        saves = self.root/'Clock Limit Saves'
        saves.mkdir()
        with self.owner('last wetness batch', saves=saves) as (client, warm):
            session = self.initialize(client, revision, uid(2200), saves, None)
            state = Reference()
            state.next_quantum = LAST_QUANTUM-63
            state = self.advance(client, session, state, Phase('last complete seconds', (5000,)*64), 2210)
            assert state.next_quantum == LAST_QUANTUM+1
            self.observe_phase(client, session, state, 'last complete seconds')
            checkpoint = self.archive_wetness(client, saves, session, state, 2220, 'clock-limit')
            refusal = self.reject(client, 'runtime.wetness.step', self.guard(session, state.tick, 2221) |
                dict(wetness_dose=dict(first_quantum=LAST_QUANTUM+1, rain_nm=[0])), -32602)
            self.measurements['clock_limit_refusal'] = refusal
            assert checkpoint == self.archive_wetness(client, saves, session, state, 2222, 'clock-limit-unchanged')

        with self.owner('restored wetness clock limit', saves=saves) as (client, cold):
            previous = self.initialize(client, revision, uid(2230), saves, None)
            session = uid(2231)
            assert self.load(client, revision, previous, session, 'clock-limit', state.tick, 2232) == 0
            self.verify_reading(client, session, state)
            assert checkpoint == self.archive_wetness(client, saves, session, state, 2233, 'clock-limit-restored')
        assert warm['pid'] != cold['pid']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='wetness-', dir=args.output.resolve()))
    test = WetnessContract(args, root)
    result = dict(format='poima.world-wetness-contract.v1', passed=False, calls=test.calls, measurements=test.measurements,
                  harness_sha256=file_hash(Path(__file__)), binary_sha256=file_hash(args.binary))
    try:
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {root/"evidence.json"}')


if __name__ == '__main__':
    main()
