#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check precipitation over a terrain-bound water region through the native CLI."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import traceback

from managed_wetness_contract import FIRST as GAME_FIRST, RAIN_NM as GAME_RAIN
from snow_foot_release_save_refusals import reseal_slot
from world_particles_contract import transform
from world_precipitation_contract import emitter
from world_terrain_contract import encode, file_hash, read_inventory, rpc, success, tree_hashes, uid
from world_water_contract import TILES, WaterContract, component, near
from world_wetness_contract import COLLIDER, DRAWABLE, MATERIAL, PROFILE


CELLS = [[0, 0], [1, 0], [2, 0]]
CATCHMENT = [CELLS[0], CELLS[2]]
CELL_SIZE = Fraction(5, 4)
AREA = CELL_SIZE ** 2
FIRST = 170
RAIN = (1_000_000, 15, 0, 73000, *range(60))
CONTINUED = (17, 100_001)
PLATE, RAIN_SOURCE = map(uid, (7010, 7011))
WATER = ('snapshot', 'payload', 'water')


def rain_volume(depth_nm):
    return Fraction(depth_nm, 10**9) * len(CATCHMENT) * AREA


def payload(saves, slot):
    path, = (saves / ('slot-'+slot)).glob('p-*.bin')
    return path.read_bytes()


def without_session(report):
    return {key: value for key, value in report.items() if key != 'session_id'}


class RainContract(WaterContract):
    def dose(self, session, tick, request, first, rain, ticks=1):
        return rpc('runtime.wetness.step', session_id=session, request_id=uid(request),
                   expected_tick=tick, expected_structure_revision=0, ticks=ticks,
                   wetness_dose=dict(first_quantum=first, rain_nm=list(rain)))

    def reading(self, session, tick):
        return self.state(session, tick, cells=CELLS)

    def climate(self, session, tick):
        return rpc('runtime.wetness.inspect', session_id=session, tick=tick, structure_revision=0)

    def verify(self, state, cumulative, tick_depth):
        assert state['active'] and state['plan_sha256'] == self.pin, state
        near(state['volume_m3'], float(rain_volume(cumulative)))
        actual = state['rain']
        assert actual['cells'] == CATCHMENT and actual['cumulative_depth_nm'] == cumulative, actual
        near(actual['cumulative_m3'], float(rain_volume(cumulative)))
        for cell, observed in zip(CELLS, state['cells'], strict=True):
            assert observed['cell'] == cell, observed
            depth = Fraction(cumulative if cell in CATCHMENT else 0, 10**9)
            near(observed['depth_m'], float(depth))
            near(observed['volume_m3'], float(depth * AREA))
        receipt = state['last_step']
        if state['tick'] == 0:
            assert receipt is None
        else:
            assert receipt['tick'] == state['tick']
            near(receipt['rain_m3'], float(rain_volume(tick_depth)))
            near(receipt['volume_after_m3'], state['volume_m3'])
            near(receipt['volume_before_m3'] + receipt['supplied_m3'] + receipt['rain_m3'] -
                 receipt['drained_m3'], receipt['volume_after_m3'], tolerance=2e-9)

    def cook(self, params):
        plan = self.world_call('world.water.plan', **params)
        assert self.world_call('world.water.cook', **params, expected_plan_sha256=plan['plan_sha256']) == plan
        return plan['plan_sha256']

    def bind(self, revision, pin, request, *additional):
        reply = success(self.world_calls(
            self.transaction(revision, request, dict(op='water.set', asset=pin), *additional))[0])
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def check_authoring(self, dry, wet):
        plan = self.world_call('world.water.plan', **wet)
        assert self.world_call('world.water.plan', **(wet | dict(rain_cells=CATCHMENT))) == plan
        assert self.world_call('world.water.plan', **(dry | dict(rain_cells=[]))) == self.world_call('world.water.plan', **dry)
        assert self.world_call('world.water.plan', **dry)['plan_sha256'] != plan['plan_sha256']
        inventory, document = read_inventory(self.assets), self.world.read_bytes()
        for cells in ([CELLS[0]]*2, [[3, 0]], [[0, True]], [[0.0, 0]], [CELLS[0]]*4, None):
            self.cases(('bad_cells', rpc('world.water.plan', **(wet | dict(rain_cells=cells))), -32602))
        self.cases(('stale_pin', rpc('world.water.cook', **wet, expected_plan_sha256='a'*64), -32009))
        assert read_inventory(self.assets) == inventory and self.world.read_bytes() == document
        self.pin = self.cook(wet)
        path = self.assets/(self.pin+'.pwater')
        assert file_hash(path) == self.pin
        decoded = json.loads(path.read_bytes())
        assert decoded['version'] == 2 and decoded['rain_cells'] == CATCHMENT, decoded
        for index, bad in enumerate((decoded | dict(version=1),
            decoded | dict(rain_cells=CATCHMENT[::-1]),
            decoded | dict(rain_cells=[CELLS[0]]*2),
            decoded | dict(rain_cells=[]))):
            raw = encode(bad).encode()
            digest = hashlib.sha256(raw).hexdigest()
            path = self.assets/(digest+'.pwater')
            path.write_bytes(raw)
            self.cases(('bad_package', self.transaction(wet['revision'], 7300+index,
                dict(op='water.set', asset=digest)), -32050))
            assert self.world.read_bytes() == document
            path.unlink()

    def configure(self, saves, request):
        return rpc('save.configure', request_id=uid(request), expected_generation=0, root=self.native(saves))

    def compare_cadence(self, revision):
        batch, serial, fresh, probe = map(uid, range(7100, 7104))
        saves = self.root/'Rain checkpoints'
        saves.mkdir()
        request = self.dose(batch, 0, 7200, FIRST, RAIN)
        result = self.cases(('configuration', self.configure(saves, 7100)),
            ('start', self.start(batch, revision)), ('initial', self.reading(batch, 0)),
            ('initial_clock', self.climate(batch, 0)), ('dose', request),
            ('catchment', self.reading(batch, 1)), ('clock', self.climate(batch, 1)),
            ('retry', request), ('after_retry', self.reading(batch, 1)),
            ('checkpoint', self.write(batch, 1, 'checkpoint', 7201)),
            ('continue', self.dose(batch, 1, 7202, FIRST+len(RAIN), CONTINUED)),
            ('continued', self.reading(batch, 2)), ('save', self.write(batch, 2, 'original', 7203)),
            ('close', rpc('session.close')))
        self.verify(result['initial'], 0, 0)
        assert result['initial_clock']['receivers'] == [] and result['initial_clock']['next_quantum'] is None
        self.verify(result['catchment'], sum(RAIN), sum(RAIN))
        assert result['clock']['receivers'] == [] and result['clock']['next_quantum'] == FIRST+len(RAIN)
        assert result['retry'] == dict(result['dose'], replayed=True)
        assert result['after_retry'] == result['catchment']
        self.verify(result['continued'], sum(RAIN+CONTINUED), sum(CONTINUED))
        data = json.loads(payload(saves, 'checkpoint'))['snapshot']['payload']
        assert data['wetness'] == dict(version=1, next_quantum=FIRST+len(RAIN), receivers=[])
        assert data['water']['version'] == 2 and data['water']['rain'] == dict(
            first_quantum=FIRST, cumulative_depth_nm=sum(RAIN), last_step_depth_nm=sum(RAIN))
        self.measurements['batch'] = result

        streaming = [(f'tick-{i}', self.dose(serial, i, 7400+i, FIRST+i, [dose]))
                     for i, dose in enumerate(RAIN)]
        steps = self.cases(('start', self.start(serial, revision)), *streaming,
                           ('water', self.reading(serial, len(RAIN))), ('clock', self.climate(serial, len(RAIN))))
        self.verify(steps['water'], sum(RAIN), RAIN[-1])
        assert steps['water']['rain'] == result['catchment']['rain']
        assert steps['clock']['next_quantum'] == result['clock']['next_quantum']
        self.measurements['serial'] = steps

        # Restore the checkpoints from a fresh owner, then reject resealed
        # archives where the water ledger is incompatible with the shared dose clock.
        original = tree_hashes(saves/'slot-checkpoint')
        bad_entries = (
            ('water-time', WATER+('rain', 'first_quantum'), FIRST+len(RAIN)),
            ('negative-rain', WATER+('rain', 'last_step_depth_nm'), -1),
            ('cumulative-rain', WATER+('rain', 'cumulative_depth_nm'), 64_000_001),
            ('forged-volume', WATER+('last_report', 'rain_m3'), 0),
            ('wrong-clock', ('snapshot', 'payload', 'wetness', 'next_quantum'), None),
            ('bad-plan', WATER+('plan_sha256',), 'a'*64),
        )
        failures = []
        for index, (slot, path, replacement) in enumerate(bad_entries):
            reseal_slot(saves/'slot-checkpoint', saves, slot, path, replacement)
            request = self.load(uid(7580+index), revision, 7550+index, active=fresh, active_tick=1)
            request['params']['slot'] = slot
            failures.extend([(slot, request, -32070), (slot+'-unchanged', self.reading(fresh, 1))])
        restored = self.cases(('configure', self.configure(saves, 7530)), ('load', self.load(fresh, revision, 7531)),
            ('water', self.reading(fresh, 1)), ('climate', self.climate(fresh, 1)),
            ('clock-conflict', self.dose(fresh, 1, 7532, FIRST+len(RAIN)+1, [10]), -32040),
            ('wrong-owner', self.dose(probe, 1, 7533, FIRST+len(RAIN), [10]), -32030),
            *failures, ('continue', self.dose(fresh, 1, 7534, FIRST+len(RAIN), CONTINUED)),
            ('continued', self.reading(fresh, 2)), ('save', self.write(fresh, 2, 'restored', 7535)),
            ('close', rpc('session.close')))
        assert restored['load']['tick'] == 1
        assert without_session(restored['water']) == without_session(result['catchment'])
        assert without_session(restored['climate']) == without_session(result['clock'])
        assert all(restored[slot+'-unchanged'] == restored['water'] for slot, *_ in bad_entries)
        assert without_session(restored['continued']) == without_session(result['continued'])
        assert tree_hashes(saves/'slot-checkpoint') == original
        assert payload(saves, 'original') == payload(saves, 'restored')
        self.measurements['restored'] = restored

    def flow_and_drain(self, wet, revision):
        active = wet | dict(revision=revision, grid=wet['grid'] | dict(relaxation_per_second=6),
            exchanges=[dict(cell=CELLS[0], source_m3_per_second=0, drain_m3_per_second=.012)])
        self.pin = self.cook(active)
        revision = self.bind(revision, self.pin, 7589)
        session = uid(7129)
        result = self.cases(('start', self.start(session, revision)),
            ('dose', self.dose(session, 0, 7588, FIRST, [1_000_000])),
            ('water', self.reading(session, 1)))
        drained = Fraction(12, 1000*60)
        wet_depth = Fraction(1, 1000)
        after_drain = wet_depth - drained / AREA
        flow = after_drain * Fraction(1, 10)
        depths = (after_drain-flow, flow, wet_depth)
        for sample, expected in zip(result['water']['cells'], depths, strict=True):
            near(sample['depth_m'], float(expected))
        receipt = result['water']['last_step']
        near(receipt['drained_m3'], float(drained))
        near(receipt['volume_after_m3'], float(rain_volume(1_000_000) - drained))
        near(receipt['rain_m3'], float(rain_volume(1_000_000)))
        assert receipt['supplied_m3'] == 0
        self.measurements['flow_and_drain'] = result
        return revision

    def rollback(self, wet, revision):
        overflow = wet | dict(revision=revision, initial_depths_m=[985000, 0, 0],
            exchanges=[dict(cell=CELLS[0], source_m3_per_second=1_000_000, drain_m3_per_second=0)])
        self.pin = self.cook(overflow)
        revision = self.bind(revision, self.pin, 7590)
        session = uid(7130)
        result = self.cases(('start', self.start(session, revision)), ('before', self.reading(session, 0)),
            ('overflow', self.dose(session, 0, 7591, FIRST, [1_000_000], ticks=2), -32040),
            ('after', self.reading(session, 0)), ('clock', self.climate(session, 0)),
            ('apply', self.dose(session, 0, 7592, FIRST, [1_000_000])), ('one', self.reading(session, 1)))
        assert 'exceeds the supported depth' in result['overflow']['message'], result
        assert result['before'] == result['after'] and result['clock']['next_quantum'] is None
        assert result['one']['rain']['cumulative_depth_nm'] == 1_000_000
        near(result['one']['last_step']['rain_m3'], float(rain_volume(1_000_000)))
        near(result['one']['volume_m3'], float(985000*AREA+Fraction(1_000_000, 60)+rain_volume(1_000_000)), 2e-9)
        self.measurements['rollback'] = result
        return revision

    def compiled_weather(self, revision):
        descriptor = self.args.native_descriptor
        if not descriptor:
            return
        session = uid(7131)
        mixed = self.dose(session, 1, 7650, GAME_FIRST+1, [1000])
        steps = []
        for i in range(len(GAME_RAIN)):
            steps.extend([(f'step-{i+1}', self.step(session, i, 1, 7660+i)),
                          (f'water-{i+1}', self.reading(session, i+1)),
                          (f'clock-{i+1}', self.climate(session, i+1))])
            if i == 0:
                steps.extend([('mixed-dose', mixed, -32040), ('after-mixed', self.reading(session, 1)),
                              ('after-mixed-clock', self.climate(session, 1))])
        result = self.cases(('start', self.start(session, revision)),
            ('load', rpc('runtime.gameplay.load_native', session_id=session, request_id=uid(7651),
                expected_tick=0, expected_revision=0, descriptor=self.native(descriptor),
                expected_descriptor_sha256=file_hash(descriptor))), *steps)
        assert result['load']['revision'] == 1
        assert 'both caller and gameplay doses' in result['mixed-dose']['message'], result
        assert result['after-mixed'] == result['water-1'] and result['after-mixed-clock'] == result['clock-1']
        for tick, dose in enumerate(GAME_RAIN, 1):
            self.verify(result[f'water-{tick}'], sum(GAME_RAIN[:tick]), dose)
            assert result[f'clock-{tick}']['next_quantum'] == GAME_FIRST+tick
        self.measurements['compiled'] = result

    def joined_rollback(self, revision):
        revision = self.bind(revision, self.pin, 7630,
            component(PLATE, 'Transform', transform([8, 2, 3])),
            component(PLATE, 'BoxCollider', COLLIDER),
            component(PLATE, 'SurfaceWetness', PROFILE | dict(capacity_nm=2_000_000,
                retention_q16=65_536, drying_nm_per_second=0)),
            dict(op='entity.create', id=RAIN_SOURCE, name='Exposed weather particles'),
            component(RAIN_SOURCE, 'Transform', transform([8, 2, -3])),
            component(RAIN_SOURCE, 'ParticleEmitter', emitter([.6, .7, 1])))
        saves = self.root/'Joined weather saves'
        saves.mkdir()
        warm, fresh = uid(7170), uid(7171)
        first, later = (1_000_000, 180), (240,)

        def inspect(label, session, tick):
            return [(label+'-water', self.reading(session, tick)),
                    (label+'-film', self.climate(session, tick)),
                    (label+'-particles', rpc('runtime.particles.inspect', session_id=session, tick=tick))]

        # The mark is well-formed, but its ray only misses once the engine
        # reaches the surface mark phase after all three weather owners advanced.
        miss = self.dose(warm, 1, 7633, FIRST+len(first), later)
        miss['params']['surface_marks'] = [dict(receiver=PLATE,
            ray=dict(origin=[40, 4, 40], direction=[0, -1, 0], distance=6),
            half_extent_m=[.12, .12], half_depth_m=.05, minimum_normal_dot=.8,
            edge_fraction=.25, linear_rgb=[.18, .1, .12], opacity=1, roughness=.6)]
        states = self.cases(('configure', self.configure(saves, 7631)),
            ('start', self.start(warm, revision)), ('apply', self.dose(warm, 0, 7632, FIRST, first)),
            *inspect('before', warm, 1), ('checkpoint', self.write(warm, 1, 'checkpoint', 7634)),
            ('late-miss', miss, -32040), *inspect('after', warm, 1),
            ('rejected-save', self.write(warm, 1, 'rejected', 7635)),
            ('continue', self.dose(warm, 1, 7636, FIRST+len(first), later)),
            *inspect('final', warm, 2), ('final-save', self.write(warm, 2, 'joined-final', 7637)),
            ('close', rpc('session.close')))
        assert 'must first hit the expected receiver' in states['late-miss']['message'], states['late-miss']
        owners = ('water', 'film', 'particles')
        for name in owners:
            assert states['after-'+name] == states['before-'+name]
        assert payload(saves, 'rejected') == payload(saves, 'checkpoint')

        for label, sequence, latest, births, phase, scheduled in (
                ('before', first, first, 1, 60, 1), ('final', first+later, later, 2, 60, 2)):
            self.verify(states[label+'-water'], sum(sequence), sum(latest))
            film = states[label+'-film']
            assert film['next_quantum'] == FIRST+len(sequence) and film['receivers'] == [
                dict(entity=PLATE, capacity_nm=2_000_000, film_nm_q16=sum(sequence)*65_536,
                     blocker=None, exposed=True)], film
            particles = states[label+'-particles']
            assert particles['rain'] == dict(rain_nm=sequence[-1], ticks_remaining=59,
                                             sample_tick=states[label+'-water']['tick']), particles
            source, = particles['sources']
            assert source['entity'] == RAIN_SOURCE and source['rain'] == dict(
                phase=phase, scheduled_births=scheduled, blocker=None), source
            assert particles['counts']['live'] == births, particles

        restored = self.cases(('configure', self.configure(saves, 7638)),
            ('load', self.load(fresh, revision, 7639)), *inspect('before', fresh, 1),
            ('restored-save', self.write(fresh, 1, 'joined-restored', 7640)),
            ('continue', self.dose(fresh, 1, 7641, FIRST+len(first), later)),
            *inspect('final', fresh, 2), ('final-save', self.write(fresh, 2, 'joined-fresh-final', 7642)),
            ('close', rpc('session.close')))
        for label in ('before', 'final'):
            for name in owners:
                key = label+'-'+name
                assert without_session(restored[key]) == without_session(states[key])
        assert payload(saves, 'joined-restored') == payload(saves, 'checkpoint')
        assert payload(saves, 'joined-fresh-final') == payload(saves, 'joined-final')
        self.measurements['joined_rollback'] = dict(warm=states, fresh=restored)
        return revision

    def export(self, revision):
        runtime = self.args.runtime_root
        if not runtime:
            return
        unsupported = self.root/'Missing rain capability'
        unsupported.mkdir()
        spec = json.loads((runtime/'runtime.json').read_bytes())
        assert spec['features'].pop('water_rain') is True, spec
        (unsupported/'runtime.json').write_text(encode(spec), encoding='utf-8')
        output = self.root/'Rain bundle'
        command = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                   '--output', self.native(output), '--runtime', self.native(unsupported)]
        child = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.calls.append(dict(argv=command, exit=child.returncode, stdout=child.stdout, stderr=child.stderr))
        assert child.returncode == 4 and not output.exists(), child
        diagnostic = json.loads(child.stdout)['diagnostics']
        assert diagnostic == [dict(code='project.failed',
            message='Rain-enabled water regions require a compatible simulation runtime.')], diagnostic
        success(json.loads(self.cli('project', 'build', self.native(self.manifest),
            '--output', self.native(output), '--runtime', self.native(runtime))))
        bundle, detached = self.root/'Relocated rain bundle', self.root/'Detached source'
        output.rename(bundle)
        self.project.rename(detached)
        assert not output.exists() and not self.project.exists()
        files = list(bundle.rglob('*.pwater'))
        assert len(files) == 1 and files[0].stem == file_hash(files[0]) == self.pin
        manifest = bundle/'game.json'
        executable = bundle/json.loads(manifest.read_bytes())['engine']['executable']
        info = success(json.loads(self.cli('game', 'inspect', self.native(manifest), executable=executable)))
        assert info['revision'] == revision
        self.measurements['bundle_sha256'] = file_hash(manifest)

    def run(self):
        recipe, _, revision = self.arm_terrain()
        dry = dict(revision=revision, terrain={'source': recipe.name},
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'], tile_entities=TILES,
            grid=dict(cells_x=3, cells_z=1, cell_size_m=float(CELL_SIZE),
                      origin_x_m=10, origin_z_m=-4, relaxation_per_second=0))
        wet = dry | dict(rain_cells=CATCHMENT[::-1])
        self.check_authoring(dry, wet)
        rain_pin = self.pin
        revision = self.bind(revision, rain_pin, 7601)
        assert self.world_call('world.dependencies')['needs_water_rain'] is True
        self.compiled_weather(revision)
        self.compare_cadence(revision)
        revision = self.flow_and_drain(wet, revision)
        revision = self.rollback(wet, revision)

        # One forecast feeds both physical water and a film that retains zero.
        plate = [dict(op='entity.create', id=PLATE, name='Zero retention plate'),
                 component(PLATE, 'MeshRenderer', DRAWABLE), component(PLATE, 'PbrMaterial', MATERIAL),
                 component(PLATE, 'SurfaceWetness', PROFILE | dict(retention_q16=0, drying_nm_per_second=0))]
        self.pin = rain_pin
        revision = self.bind(revision, self.pin, 7610, *plate)
        session = uid(7140)
        together = self.cases(('start', self.start(session, revision)),
            ('rain', self.dose(session, 0, 7611, FIRST, RAIN)), ('water', self.reading(session, 1)),
            ('climate', self.climate(session, 1)))
        self.verify(together['water'], sum(RAIN), sum(RAIN))
        receiver, = together['climate']['receivers']
        assert receiver['entity'] == PLATE and receiver['film_nm_q16'] == 0, receiver
        self.measurements['film_and_volume'] = together

        dry_pin = self.cook(dry | dict(revision=revision))
        dry_file = self.assets/(dry_pin+'.pwater')
        decoded = json.loads(dry_file.read_bytes())
        assert decoded['version'] == 1 and 'rain_cells' not in decoded, decoded
        revision = self.bind(revision, dry_pin, 7620)
        session = uid(7141)
        control = self.cases(('start', self.start(session, revision)),
            ('rain', self.dose(session, 0, 7621, FIRST, RAIN)), ('water', self.reading(session, 1)),
            ('climate', self.climate(session, 1)))
        assert 'rain' not in control['water'] and 'rain_m3' not in control['water']['last_step']
        assert control['water']['volume_m3'] == 0
        assert control['climate']['next_quantum'] == together['climate']['next_quantum']
        assert 'needs_water_rain' not in self.world_call('world.dependencies')
        self.measurements['dry_control'] = control
        revision = self.joined_rollback(revision)
        self.export(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--runtime-root', type=Path, help='Matching installed runtime for export admission and relocation.')
    parser.add_argument('--native-descriptor', type=Path, help='Built Native AOT managed wetness fixture descriptor.')
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    scratch = Path(__file__).resolve().parents[1]/'build'/'world-water-rain-contract'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='rain-', dir=scratch) as directory:
        test = RainContract(args, Path(directory))
        passed = False
        failure = None
        try:
            test.run()
            passed = True
        except BaseException:
            failure = traceback.format_exc()
            raise
        finally:
            if args.evidence:
                args.evidence.write_text(json.dumps(dict(passed=passed, calls=test.calls,
                    measurements=test.measurements, failure=failure), indent=2)+'\n', encoding='utf-8')
    print('Native terrain precipitation contract passed')


if __name__ == '__main__':
    main()
