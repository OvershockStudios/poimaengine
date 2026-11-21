#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a compiled melee contact consumer through native physics and checkpoints."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import time

import components_gameplay_contract
import input_actions_gameplay_contract
from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import OwnedSession, exact, fresh, need


ROOT = Path(__file__).resolve().parents[1]
GAME, IDENTITY = 'Poima.Tests.MeleeContactGame', 'poima.test.melee-contact'
EMPTY, COVER, TARGET, WITNESS = map(uid, (0, 50, 100, 200))
TARGET_FACE, COVER_FACE = 2.02, .93
PARKED = [1.42, 1.25, -.4]
RADIUS, ENVELOPE, MAX_INTERVALS, ACTIVE_TICKS = .075, .004, 24, 5
WORLD_TOLERANCE = 2e-4  # Jolt single-precision contact geometry.


def scene(case):
    def box(identity, name, position, half_extents, motion='static'):
        return [
            dict(op='entity.create', id=identity, name=name),
            dict(op='component.set', id=identity, type='Transform',
                 value=dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=identity, type='BoxCollider',
                 value=dict(half_extents=half_extents, motion=motion, mass=1, friction=.5, restitution=0)),
        ]

    face = .57 if case == 'overlap' else TARGET_FACE
    target_height = 6 if case == 'miss' else 1.5
    ops = box(TARGET, 'Physical target', [face + .15, target_height, 0], [.15, 1, 1.5])
    ops += box(WITNESS, 'Contact witness', PARKED, [.015, .015, .015], 'kinematic')
    if case == 'cover':
        ops += box(COVER, 'Near environmental cover', [COVER_FACE + .04, 1.5, 0], [.04, 1, 1.5])
    return ops


def vector(values, prefix):
    return [values[prefix + axis] for axis in 'XYZ']


def near(actual, expected, tolerance, reason):
    need(math.dist(actual, expected) <= tolerance, f'{reason}: expected {expected}, got {actual}')


def pose(tick):
    # Verify the exact world trajectory shared by every scene. The separate
    # face oracle below never calls or reproduces the capsule subdivision.
    return ([.35 + tick * (.42 + .018 * tick), 1.05 + .018 * tick, -.35 + .03 * tick],
            [.65 + tick * (.48 + .035 * tick), 1.38 + .027 * tick**2, .4 - .025 * tick])


def check_first_face(observation, face):
    values = observation['values']
    tick = observation['tick']
    _, tip_before = pose(tick - 1)
    _, tip_after = pose(tick)
    travel = tip_after[0] - tip_before[0]
    # The tip has the maximal X over the whole segment, and the broad face
    # covers its swept YZ range. Solve when the actual radius reaches it.
    exact_fraction = (face - RADIUS - tip_before[0]) / travel
    earlier_bound = (2 * ENVELOPE + WORLD_TOLERANCE) / travel
    reported = values['Fraction']
    need(0 < exact_fraction < 1 and exact_fraction - earlier_bound <= reported <= exact_fraction + earlier_bound,
         f'Expected a face contact at fraction {exact_fraction} within {earlier_bound}, got {reported}')
    need(values['InitialOverlap'] == 0 and values['Penetration'] == 0,
         'Separated blade was reported in initial overlap')
    expected = [face, *(a + (b - a) * exact_fraction for a, b in zip(tip_before[1:], tip_after[1:]))]
    obstacle = vector(values, 'Obstacle')
    need(abs(obstacle[0] - face) <= WORLD_TOLERANCE, 'Obstacle contact is off the analytic front face')
    near(obstacle, expected, .015, 'Contact drifts from the predicted blade tip')
    near(vector(values, 'Query'), obstacle, WORLD_TOLERANCE, 'Native query and obstacle points diverge')
    near(vector(values, 'Normal'), [-1, 0, 0], .002, 'Front-face normal differs')
    near(observation['witness']['world_matrix'][12:15], obstacle, WORLD_TOLERANCE,
         'Native witness is not colocated with the reported obstacle point')
    observation['analytic_face'] = dict(x=face, fraction=exact_fraction, fraction_tolerance=earlier_bound,
                                        contact=expected)


def artifact(args):
    paths = [Path(__file__), Path(components_gameplay_contract.__file__),
             Path(input_actions_gameplay_contract.__file__), ROOT/'tests/character_gameplay_contract.py',
             args.binary, ROOT/'managed/Directory.Build.props',
             *(ROOT/'tests/fixtures/melee-contact').glob('*.cs*'), *(ROOT/'packages/fps').glob('*.cs*')]
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve(strict=True)
        need(descriptor.stat().st_size <= 1024 * 1024, 'Native descriptor exceeds 1 MiB')
        spec = json.loads(descriptor.read_bytes())
        need((spec['type'], spec['identity']) == (GAME, IDENTITY) and spec['services_version'] == 7 and
             spec['minimum_services_bytes'] == 400 and 'capsule_cast_v1' in spec['required_features'],
             'Native descriptor does not declare the compiled capsule consumer')
        closure = set()
        for item in spec['files']:
            relative = Path(item['path'])
            path = (descriptor.parent/relative).resolve(strict=True)
            need(not relative.is_absolute() and '..' not in relative.parts and path.is_relative_to(descriptor.parent) and
                 path.stat().st_size == item['size'] and sha(path) == item['sha256'], 'Native artifact closure differs')
            closure.add(path)
        image = (descriptor.parent/spec['library']).resolve(strict=True)
        need(image in closure, 'Native fixture library is absent from the descriptor closure')
        return image, dict(descriptor=args.native(descriptor), expected_descriptor_sha256=sha(descriptor)), [
            *paths, descriptor, *sorted(closure)]

    image = args.assembly.resolve(strict=True)
    paths.extend([image, args.bridge, args.hostfxr, image.parent/'Poima.Fps.dll'])
    for assembly in (image, args.bridge):
        paths.append(assembly.parent/'Poima.Gameplay.dll')
        paths.extend(path for suffix in ('.deps.json', '.runtimeconfig.json')
                     if (path := assembly.with_suffix(suffix)).is_file())
    return image, dict(hostfxr=args.native(args.hostfxr), bridge=args.native(args.bridge),
                       assembly=args.native(image), type=GAME), paths


class Fixture:
    def __init__(self, args, output, evidence):
        self.args, self.output, self.evidence = args, output, evidence
        self.saves = output/'saves'
        self.saves.mkdir()
        self.owner = None

    def open(self, case, *, saves=False):
        self.owner = OwnedSession(self.args, self.output/(case + '.json'), self.evidence)
        if saves:
            self.owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0,
                                                  root=self.args.native(self.saves)))

    def start(self, case, image, config, *, saves=False):
        self.open(case, saves=saves)
        revision = self.owner.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene(case)))['revision']
        self.owner.start(revision)
        self.check_module(self.owner.load(config)['module'], image)
        return revision

    def check_module(self, module, image):
        need(module['schema']['identity'] == IDENTITY and module['backend'] == self.evidence['backend'] and
             module['assembly_sha256'] == sha(image), 'Native owner loaded a different compiled melee game')
        if self.evidence['backend'] == 'native_aot':
            need(module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
                 'Native melee module unexpectedly reports dynamic code')

    def close(self):
        if self.owner is not None:
            self.owner.close()
            self.owner = None

    def stop(self):
        self.owner.rpc('runtime.stop', dict(session_id=self.owner.session))
        self.close()

    def read(self):
        game = self.owner.game()
        sample = self.owner.rpc('runtime.observe', dict(session_id=self.owner.session, tick=self.owner.tick,
            gameplay_revision=game['revision'], ids=[WITNESS],
            entity_fields=['world_matrix', 'velocity', 'motion', 'motion_remaining_ticks']))
        need(sample['tick'] == self.owner.tick and sample['session_id'] == self.owner.session and
             sample['gameplay_revision'] == game['revision'], 'Gameplay and native witness observation drifted')
        row, = sample['entities']
        need(row['id'] == WITNESS and row['state']['motion'] == 'kinematic', 'Missing physical witness')
        return dict(tick=self.owner.tick, values=game['module']['values'], witness=row['state'])

    def observe(self, label, *, tick, candidate=EMPTY, first=EMPTY, hits=0, repeats=0, blocked=0):
        observation = self.read()
        self.evidence['observations'][label] = observation
        values = observation['values']
        active = min(tick, ACTIVE_TICKS)
        expected = dict(ActiveTicks=active, Candidate=candidate, FirstContact=first, Hits=hits,
                        HitTarget=TARGET if hits else EMPTY, RepeatedContacts=repeats, BlockedContacts=blocked)
        need(observation['tick'] == tick and exact({key: values[key] for key in expected}, expected),
             f'{label}: expected {expected} at tick {tick}, got {observation}')
        grip, tip = pose(active)
        near(vector(values, 'Grip'), grip, 1e-12, 'Saved grip pose differs')
        near(vector(values, 'Tip'), tip, 1e-12, 'Saved tip pose differs')
        if active:
            need(1 <= values['Queries'] <= MAX_INTERVALS and 0 < values['EnvelopeError'] <= ENVELOPE,
                 'Capsule query budget or conservative error differs')
        if candidate == EMPTY:
            need(values['Fraction'] == 1 and values['InitialOverlap'] == 0 and values['Penetration'] == 0 and
                 all(vector(values, name) == [0, 0, 0] for name in ('Query', 'Obstacle', 'Normal')),
                 'A swept miss returned contact data')
        return observation

    def checkpoint(self, slot):
        runtime = self.owner.rpc('runtime.inspect', dict(session_id=self.owner.session))
        receipt = self.owner.rpc('save.write', dict(session_id=self.owner.session, request_id=fresh(),
            configuration_generation=1, slot=slot, expected_generation=0, expected_tick=self.owner.tick,
            expected_gameplay_revision=self.owner.game()['revision'],
            expected_structure_revision=runtime['structure_revision'], expected_ui_revision=runtime['ui_revision'],
            expected_control_sequence=runtime['control_sequence'], expected_material_revision=runtime['material_revision']))
        payload, = (self.saves/('slot-' + slot)).glob('p-*.bin')
        need(payload.stat().st_size <= 16 * 1024 * 1024 and sha(payload) == receipt['sha256'],
             'Durable save receipt differs')
        snapshot = json.loads(payload.read_bytes())['snapshot']
        need(snapshot['payload']['tick'] == self.owner.tick, 'Complete snapshot tick differs')
        self.evidence['snapshots'][slot] = dict(path=str(payload.relative_to(self.output)), receipt=receipt, snapshot=snapshot)
        return payload

    def continuation(self, prefix, witness):
        observations = []
        for tick, repeats in ((4, 1), (5, 2), (6, 2)):
            self.owner.step()
            observation = self.observe(f'{prefix}-{tick}', tick=tick, candidate=TARGET, first=TARGET, hits=1,
                                       repeats=repeats)
            need(exact(observation['witness'], witness), 'Repeat contact moved the first-contact witness')
            need(observation['values']['InitialOverlap'] == 1 and observation['values']['Fraction'] == 0,
                 'The repeated blade path was expected to already intersect the target')
            observations.append(observation)
        return observations, self.checkpoint(prefix + '-terminal').read_bytes()

    def run_target(self, image, config):
        revision = self.start('target', image, config, saves=True)
        world = self.owner.world
        authored = world.read_bytes()
        self.owner.step(2)
        before_contact = self.observe('target-before-contact', tick=2)
        near(before_contact['witness']['world_matrix'][12:15], PARKED, WORLD_TOLERANCE, 'Witness should stay parked')

        self.owner.edit(FailAt='3')
        before_failed_step = self.read()
        before = self.checkpoint('rollback-before').read_bytes()
        error = self.owner.step(2, error=True)
        need('Melee fixture later Tick fault after 1 accepted hit, 1 repeat and a native marker move.' in error['message'],
             'Failure did not follow real contact, native movement and a second callback')
        need(exact(self.read(), before_failed_step) and self.checkpoint('rollback-after').read_bytes() == before,
             'A later failure changed observed state or the complete durable archive')

        self.owner.edit(FailAt='-1')
        self.owner.step()
        accepted = self.observe('target-first-contact', tick=3, candidate=TARGET, first=TARGET, hits=1)
        check_first_face(accepted, TARGET_FACE)
        source = self.checkpoint('cold-source')
        saved = source.read_bytes()
        snapshot = self.evidence['snapshots']['cold-source']['snapshot']['payload']
        witness, = (entry for entry in snapshot['entities'] if entry['id'] == WITNESS)
        near(witness['body']['position'], vector(accepted['values'], 'Obstacle'), WORLD_TOLERANCE,
             'Saved physics witness differs from the accepted contact')
        warm = self.continuation('warm', accepted['witness'])
        self.stop()

        self.open('target', saves=True)
        session = fresh()
        loaded = self.owner.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot='cold-source',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        self.owner.session, self.owner.tick = session, loaded['tick']
        self.check_module(self.owner.game()['module'], image)
        observed = self.observe('cold-source', tick=3, candidate=TARGET, first=TARGET, hits=1)
        check_first_face(observed, TARGET_FACE)
        need(exact(observed, accepted) and self.checkpoint('cold-restored').read_bytes() == saved,
             'Fresh process failed to restore the complete mid-swing checkpoint')
        cold = self.continuation('cold', accepted['witness'])
        need(exact(warm[0], cold[0]) and warm[1] == cold[1], 'Warm and cold continuation differ')
        need(source.read_bytes() == saved and world.read_bytes() == authored, 'Original source save or authored world changed')
        self.stop()

    def run_controls(self, image, config):
        self.start('miss', image, config)
        for tick in range(1, ACTIVE_TICKS + 1):
            self.owner.step()
            observation = self.observe(f'miss-{tick}', tick=tick)
            near(observation['witness']['world_matrix'][12:15], PARKED, WORLD_TOLERANCE, 'Clear miss moved the witness')
        self.stop()

        self.start('cover', image, config)
        for tick in range(1, ACTIVE_TICKS + 1):
            self.owner.step()
            observation = self.observe(f'cover-{tick}', tick=tick, candidate=COVER if tick < 3 else TARGET,
                                       first=COVER, blocked=tick)
            if tick == 1:
                check_first_face(observation, COVER_FACE)
                cover_witness = observation['witness']
            else:
                need(exact(observation['witness'], cover_witness), 'A later candidate replaced the cover witness')
        self.stop()

        self.start('overlap', image, config)
        self.owner.step()
        overlap = self.observe('initial-overlap', tick=1, candidate=TARGET, first=TARGET, hits=1)
        values = overlap['values']
        need(values['InitialOverlap'] == 1 and values['Fraction'] == 0 and values['Penetration'] > .01,
             'Expected an immediate native overlap with positive depth')
        near(overlap['witness']['world_matrix'][12:15], vector(values, 'Obstacle'), WORLD_TOLERANCE,
             'Initial overlap did not place the native contact witness')
        self.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    development = (args.hostfxr, args.bridge, args.assembly)
    if (args.native_descriptor and any(development)) or (not args.native_descriptor and not all(development)):
        parser.error('Supply --native-descriptor or all three CoreCLR paths')

    def native(path):
        value = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=5).strip() if args.windows_interop else value

    args.native, args.deadline = native, time.monotonic() + 180
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    evidence = dict(format='poima.melee-contact-contract.v1', passed=False, owners=[], calls=[], observations={}, snapshots={},
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    scope='Compiled fixed-tick blade contact, native collision and durable checkpoints in authored fixtures.')
    fixture = Fixture(args, output, evidence)
    try:
        image, config, paths = artifact(args)
        pins = {str(path.resolve()): sha(path) for path in paths}
        evidence['input_hashes'] = pins
        fixture.run_target(image, config)
        fixture.run_controls(image, config)
        need(all(sha(path) == digest for path, digest in pins.items()), 'A tested input changed')
        evidence['passed'] = True
    except BaseException as failure:
        evidence['error'] = repr(failure)
        raise
    finally:
        try:
            fixture.close()
        except BaseException as failure:
            evidence['close_error'] = repr(failure)
            if 'error' not in evidence:
                raise
        finally:
            path = output/'evidence.json'
            path.write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
            print(path)


if __name__ == '__main__':
    main()
