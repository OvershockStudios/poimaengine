#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check compiled fixed-tick heading steering against native kinematic poses."""

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
GAME, IDENTITY = 'Poima.Tests.HeadingGame', 'poima.test.heading'
ACTOR, ACROSS, REVERSE, NEAR, FINAL = map(uid, (100, 201, 202, 203, 204))
POSITION = [0, 1, 0]
INITIAL_YAW = math.radians(176)
AZIMUTHS = {ACROSS: math.radians(-145), REVERSE: math.radians(127), FINAL: math.radians(96)}
MAX_SPEED, SPEED_DELTA, DEAD_ZONE = 3, .6, .002
BRAKING_TICKS = math.ceil(MAX_SPEED / SPEED_DELTA)
POSE_TOLERANCE = 2e-6  # Float32 yaw quaternion and world transform roundoff.
COSINE_ROUNDING = 4 * 2**-24


def yaw_quaternion(yaw):
    return [0, math.sin(yaw / 2), 0, math.cos(yaw / 2)]


def angle_delta(actual, expected):
    return math.remainder(actual - expected, math.tau)


def scene():
    def entity(identity, name, position, rotation):
        return [
            dict(op='entity.create', id=identity, name=name),
            dict(op='component.set', id=identity, type='Transform',
                 value=dict(position=position, rotation=rotation, scale=[1, 1, 1])),
        ]

    ops = entity(ACTOR, 'Kinematic facing root', POSITION, yaw_quaternion(INITIAL_YAW))
    ops.append(dict(op='component.set', id=ACTOR, type='BoxCollider', value=dict(
        half_extents=[.25, .45, .2], motion='kinematic', mass=1, friction=.5, restitution=0)))
    for target, yaw in AZIMUTHS.items():
        ops += entity(target, 'Fixed facing target', [-4 * math.sin(yaw), 2, -4 * math.cos(yaw)], [0, 0, 0, 1])
    # The target is three metres higher, with just .5 micrometres of XZ separation.
    ops += entity(NEAR, 'Nearly coincident in XZ', [4e-7, 4, -3e-7], [0, 0, 0, 1])
    return ops


def braking_oracle(previous, error):
    # Numerically invert the finite distance travelled at each candidate
    # speed and subsequent maximum-braking speeds. The package uses a
    # closed form; this oracle enumerates the actual fixed-tick distances.
    if abs(error) <= DEAD_ZONE or previous * error < 0:
        return math.copysign(max(0, abs(previous) - SPEED_DELTA), previous)
    lower, upper = 0., MAX_SPEED
    for _ in range(48):
        trial = (lower + upper) / 2
        travel = sum(max(0, trial - index * SPEED_DELTA) for index in range(BRAKING_TICKS)) / 60
        if travel <= abs(error):
            lower = trial
        else:
            upper = trial
    desired = math.copysign(lower, error)
    return max(previous - SPEED_DELTA, min(previous + SPEED_DELTA, desired))


def native_step_bounds(applied):
    # Jolt forms a binary32 relative quaternion and takes 2*acos(abs(w)).
    # Allow four binary32 intervals below 1 for normalization, products, and
    # accumulation in w. Invert the resulting cosine interval because near
    # 1 its angular width is much larger than float32 quaternion roundoff.
    if abs(applied) <= 1e-12:
        return -POSE_TOLERANCE, POSE_TOLERANCE
    cosine = math.cos(applied / 2)
    low = 2 * math.acos(min(1, cosine + COSINE_ROUNDING)) - POSE_TOLERANCE
    high = 2 * math.acos(max(0, cosine - COSINE_ROUNDING)) + POSE_TOLERANCE
    return (low, high) if applied > 0 else (-high, -low)


def check_pose(observation, previous):
    tick, native, values = observation['tick'], observation['native'], observation['values']
    yaw = observation['yaw']
    need(native['motion'] == 'kinematic' and native['motion_remaining_ticks'] == 0 and
         native['local_transform']['position'] == POSITION and native['world_matrix'][12:15] == POSITION,
         f'Tick {tick} changed the physical root position or left a pending motion')
    quaternion = native['local_transform']['rotation']
    expected = yaw_quaternion(yaw)
    if math.fsum(a * b for a, b in zip(quaternion, expected)) < 0:
        quaternion = [-value for value in quaternion]
    need(math.dist(quaternion, expected) <= POSE_TOLERANCE,
         f'Tick {tick} local quaternion and world yaw differ: {yaw}, {quaternion}')
    need(values['Ticks'] == tick, 'Compiled update count differs from the native clock')
    if not tick:
        need(abs(angle_delta(yaw, INITIAL_YAW)) <= POSE_TOLERANCE, 'Native root has the wrong authored initial yaw')
        return

    target = ACROSS if tick <= 12 else REVERSE if tick <= 20 else NEAR if tick <= 28 else FINAL
    direction = target != NEAR
    error = angle_delta(AZIMUTHS[target], values['CommittedYaw']) if direction else 0
    speed = values['AngularVelocity']
    applied = values['AppliedRadians']
    need(values['Target'] == target and values['HasDirection'] == int(direction) and
         abs(values['ErrorRadians'] - error) <= 1e-10, f'Tick {tick} projected target bearing differs')
    need(abs(speed) <= MAX_SPEED + 1e-10 and abs(applied - speed / 60) <= 1e-10 and
         abs(angle_delta(values['CommandedYaw'], values['CommittedYaw'] + applied)) <= 1e-10,
         f'Tick {tick} commanded yaw differs from its bounded fixed-tick step')
    native_step = angle_delta(yaw, values['CommittedYaw'])
    low, high = native_step_bounds(applied)
    observation['native_step'] = dict(radians=native_step, lower_bound=low, upper_bound=high,
                                      error_radians=native_step - applied)
    need(low <= native_step <= high,
         f'Tick {tick} native step {native_step} escaped its cosine-derived bounds [{low}, {high}]')
    if previous['tick'] + 1 == tick:
        old_speed = previous['values']['AngularVelocity']
        expected_speed = braking_oracle(old_speed, error)
        need(abs(angle_delta(values['CommittedYaw'], previous['yaw'])) <= 1e-10,
             f'Tick {tick} used a heading other than the last committed native pose')
        need(abs(speed - old_speed) <= SPEED_DELTA + 1e-10 and abs(speed - expected_speed) <= 1e-9,
             f'Tick {tick} exceeded acceleration or differs from the discrete braking oracle {expected_speed}: {speed}')
        observation['expected_speed'] = expected_speed


def artifact(args):
    paths = [Path(__file__), Path(components_gameplay_contract.__file__),
             Path(input_actions_gameplay_contract.__file__), ROOT/'tests/character_gameplay_contract.py',
             args.binary, ROOT/'managed/Directory.Build.props',
             *(ROOT/'tests/fixtures/heading').glob('*.cs*'), *(ROOT/'packages/ai').glob('*.cs*')]
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve(strict=True)
        need(descriptor.stat().st_size <= 1024 * 1024, 'Native descriptor exceeds 1 MiB')
        spec = json.loads(descriptor.read_bytes())
        need((spec['type'], spec['identity']) == (GAME, IDENTITY) and spec['services_version'] == 7 and
             spec['minimum_services_bytes'] == 176 and spec['required_features'] == ['baseline_v7'],
             'Native descriptor differs from the compiled baseline heading consumer')
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
    paths.extend([image, args.bridge, args.hostfxr, image.parent/'Poima.Ai.dll'])
    for assembly in (image, args.bridge):
        paths.append(assembly.parent/'Poima.Gameplay.dll')
        paths.extend(path for suffix in ('.deps.json', '.runtimeconfig.json')
                     if (path := assembly.with_suffix(suffix)).is_file())
    return image, dict(hostfxr=args.native(args.hostfxr), bridge=args.native(args.bridge),
                       assembly=args.native(image), type=GAME), paths


class Fixture:
    def __init__(self, args, output, evidence):
        self.args, self.output, self.evidence = args, output, evidence
        self.world, self.saves = output/'world.json', output/'saves'
        self.saves.mkdir()
        self.owner = None

    def open(self):
        self.owner = OwnedSession(self.args, self.world, self.evidence)
        self.owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=self.args.native(self.saves)))

    def close(self):
        if self.owner is not None:
            self.owner.close()
            self.owner = None

    def stop(self):
        self.owner.rpc('runtime.stop', dict(session_id=self.owner.session))
        self.close()

    def check_module(self, module, image):
        need(module['schema']['identity'] == IDENTITY and module['backend'] == self.evidence['backend'] and
             module['assembly_sha256'] == sha(image), 'Native owner loaded a different compiled heading game')
        if self.evidence['backend'] == 'native_aot':
            need(module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
                 'Native heading module unexpectedly reports dynamic code')

    def read(self):
        guard = dict(session_id=self.owner.session, tick=self.owner.tick)
        game = self.owner.rpc('runtime.gameplay.inspect', guard)
        sample = self.owner.rpc('runtime.observe', dict(guard, gameplay_revision=game['revision'], ids=[ACTOR],
            entity_fields=['world_matrix', 'local_transform', 'motion', 'motion_remaining_ticks']))
        row, = sample['entities']
        need(row['id'] == ACTOR and sample['tick'] == self.owner.tick and sample['session_id'] == self.owner.session and
             sample['gameplay_revision'] == game['revision'], 'Gameplay and native pose observation drifted')
        matrix = row['state']['world_matrix']
        return dict(tick=self.owner.tick, values=game['module']['values'], native=row['state'],
                    yaw=math.atan2(matrix[8], matrix[10]))

    def checkpoint(self, slot):
        runtime = self.owner.rpc('runtime.inspect', dict(session_id=self.owner.session))
        receipt = self.owner.rpc('save.write', dict(session_id=self.owner.session, request_id=fresh(),
            configuration_generation=1, slot=slot, expected_generation=0, expected_tick=self.owner.tick,
            expected_gameplay_revision=self.owner.game()['revision'],
            expected_structure_revision=runtime['structure_revision'], expected_ui_revision=runtime['ui_revision'],
            expected_control_sequence=runtime['control_sequence'], expected_material_revision=runtime['material_revision']))
        payload, = (self.saves/('slot-' + slot)).glob('p-*.bin')
        need(payload.stat().st_size <= 16 * 1024 * 1024 and sha(payload) == receipt['sha256'], 'Durable save receipt differs')
        snapshot = json.loads(payload.read_bytes())['snapshot']
        need(snapshot['payload']['tick'] == self.owner.tick, 'Complete snapshot tick differs')
        self.evidence['snapshots'][slot] = dict(path=str(payload.relative_to(self.output)), receipt=receipt, snapshot=snapshot)
        return payload

    def advance(self, ticks, trace, previous):
        for tick in ticks:
            self.owner.step(tick - self.owner.tick)
            observation = self.read()
            trace[tick] = observation
            check_pose(observation, previous)
            previous = observation

    def run(self, image, config):
        self.open()
        revision = self.owner.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene()))['revision']
        authored = self.world.read_bytes()
        self.owner.start(revision)
        self.check_module(self.owner.load(config)['module'], image)
        initial = self.read()
        check_pose(initial, None)
        warm = self.evidence['observations']['warm']
        warm[0] = initial
        self.advance(range(1, 4), warm, initial)

        self.owner.edit(FailAt='4')
        before_state = self.read()
        before_save = self.checkpoint('rollback-before').read_bytes()
        failure = self.owner.step(2, error=True)
        need('Heading fixture later Tick fault after the native seam crossing.' in failure['message'],
             'Fault did not follow a committed native turn inside the rejected batch')
        need(exact(self.read(), before_state) and self.checkpoint('rollback-after').read_bytes() == before_save,
             'Later callback rejection changed the complete pre-step state')
        self.owner.edit(FailAt='-1')
        self.advance(range(4, 16), warm, warm[3])
        need(warm[3]['yaw'] > 3 and warm[4]['yaw'] < -3 and angle_delta(warm[4]['yaw'], warm[3]['yaw']) > 0,
             'Native root failed to turn through the short arc across the seam')
        need(warm[15]['values']['AngularVelocity'] > 0 and warm[15]['values']['ErrorRadians'] < 0,
             'Mid-turn checkpoint does not contain momentum opposing the new target')
        source = self.checkpoint('cold-source')
        saved = source.read_bytes()
        continuation = [*range(16, 41), *range(63, 77), 84]
        self.advance(continuation, warm, warm[15])
        terminal = self.checkpoint('warm-terminal').read_bytes()
        self.stop()

        self.open()
        session = fresh()
        loaded = self.owner.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot='cold-source',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        self.owner.session, self.owner.tick = session, loaded['tick']
        self.check_module(self.owner.game()['module'], image)
        cold = self.evidence['observations']['cold']
        cold[15] = self.read()
        check_pose(cold[15], warm[14])
        need(exact(cold[15], warm[15]) and self.checkpoint('cold-restored').read_bytes() == saved,
             'Fresh process failed to restore the complete heading checkpoint')
        self.advance(continuation, cold, cold[15])
        need(all(exact(cold[tick], warm[tick]) for tick in continuation) and
             self.checkpoint('cold-terminal').read_bytes() == terminal, 'Fresh-process heading continuation differs')

        speeds = {tick: row['values']['AngularVelocity'] for tick, row in warm.items()}
        need(speeds[12] == MAX_SPEED and abs(speeds[17]) <= 1e-10 and speeds[18] < 0,
             'The root failed to brake through zero before reversing')
        need(speeds[20] < speeds[21] < speeds[22] < 0 and abs(speeds[23]) <= 1e-10 and
             all(warm[tick]['values']['HasDirection'] == 0 for tick in range(21, 29)) and
             abs(angle_delta(warm[28]['yaw'], warm[23]['yaw'])) <= POSE_TOLERANCE,
             'The near-coincident target failed to brake and hold the actual yaw')
        need(warm[38]['yaw'] < -3 and warm[39]['yaw'] > 3 and angle_delta(warm[39]['yaw'], warm[38]['yaw']) < 0,
             'Native root failed to turn through the seam in the reverse direction')
        need(abs(speeds[84]) <= 1e-10 and abs(angle_delta(warm[84]['yaw'], AZIMUTHS[FINAL])) <= DEAD_ZONE + POSE_TOLERANCE,
             'Final target failed to reach a settled heading')
        step_errors = {tick: abs(row['native_step']['error_radians']) for tick, row in warm.items() if tick}
        peak_tick = max(step_errors, key=step_errors.get)
        self.evidence['native_metrics'] = dict(selected_ticks=len(step_errors), peak_step_error_tick=peak_tick,
            peak_step_error_radians=step_errors[peak_tick],
            peak_observed_speed_radians_per_second=60 * max(abs(row['native_step']['radians'])
                                                           for tick, row in warm.items() if tick),
            terminal_error_radians=abs(angle_delta(warm[84]['yaw'], AZIMUTHS[FINAL])))
        need(source.read_bytes() == saved and self.world.read_bytes() == authored, 'Original source save or authored world changed')
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
    evidence = dict(format='poima.heading-gameplay-contract.v1', passed=False, owners=[], calls=[], snapshots={},
                    observations=dict(warm={}, cold={}), backend='native_aot' if args.native_descriptor else 'coreclr',
                    scope='Compiled 60 Hz heading helper, native kinematic pose and durable checkpoints.')
    fixture = Fixture(args, output, evidence)
    try:
        image, config, paths = artifact(args)
        pins = {str(path.resolve()): sha(path) for path in paths}
        evidence['input_hashes'] = pins
        fixture.run(image, config)
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
