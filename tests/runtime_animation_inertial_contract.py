#!/usr/bin/env python3
"""Check agent-accessible inertial transitions against independent pose references."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import unittest
import runtime_animation_blend_contract as legacy


def correction(displacement, velocity, seconds, duration):
    # Solve the six endpoint constraints using elimination, independently of
    # the native evaluator. Initial acceleration and all end derivatives are 0.
    coefficients = [displacement, velocity, 0.0]
    rows = [[duration**3, duration**4, duration**5,
             -displacement - velocity*duration],
            [3*duration**2, 4*duration**3, 5*duration**4, -velocity],
            [6*duration, 12*duration**2, 20*duration**3, 0.0]]
    for column in range(3):
        pivot = rows[column][column]
        rows[column] = [v/pivot for v in rows[column]]
        for other in range(3):
            if other != column:
                factor = rows[other][column]
                rows[other] = [a-factor*b for a, b in zip(rows[other], rows[column])]
    coefficients += [row[3] for row in rows]
    return sum(value*seconds**power for power, value in enumerate(coefficients))


class InertialContract(legacy.BlendContract):
    def test_inertial_discovery_and_analytic_translation(self):
        discovery = self.good('world.describe')
        item = discovery['methods']['runtime.step']['properties']['animations']['items']
        self.assertEqual(item['properties']['transition_mode'],
                         {'enum': ['crossfade', 'inertial'], 'default': 'crossfade'})
        self.prepare()
        self.start()
        self.step(30, [legacy.command(self.rig)])  # x=.5, outgoing dx/dt=1.
        self.step(15, [legacy.command(self.rig, clip=1, time=.25, speed=2,
            blend_ticks=60, transition_mode='inertial')])
        # B(.25+2*.25)=1.25. Offset=-1.25, velocity difference=3.
        self.pose((1.25 + correction(-1.25, 3, .25, 1),
                   3 + correction(-2, 0, .25, 1), 0))
        active = self.animation()['transition']
        self.assertEqual(active['mode'], 'inertial')
        self.assertEqual(active['weight'], .25)
        self.assertTrue(active['source_frozen'])
        for field in ['source_clip', 'source_time', 'source_speed', 'source_loop', 'source_playing']:
            self.assertIsNone(active[field])
        observed = self.good('runtime.observe', session_id=self.session, tick=self.tick,
                             ids=[self.rig], entity_fields=['animation'])
        self.assertEqual(observed['entities'][0]['state']['animation']['transition'], active)
        self.step(45)
        self.pose((1.75, 3, 0))
        self.assertIsNone(self.animation()['transition'])
        self.assertEqual(self.world.read_bytes(), self.before)

    def test_inertial_startup_rest_and_zero_duration(self):
        self.prepare()
        self.start()
        # No previous distinct output tick: outgoing velocity is explicitly 0.
        self.step(15, [legacy.command(self.rig, clip=1, time=.5, playing=False,
            blend_ticks=60, transition_mode='inertial')])
        self.pose((1.5 + correction(-1.5, 0, .25, 1),
                   3 + correction(-2, 0, .25, 1), 0))
        self.step(1, [legacy.command(self.rig, clip=None,
            blend_ticks=0, transition_mode='inertial')])
        self.pose((0, 1, 0))
        self.assertIsNone(self.animation()['transition'])

    def test_inertial_mode_receipts_and_invalid_retries(self):
        self.prepare()
        self.start()
        params = self.params(10, [legacy.command(self.rig, clip=1,
            blend_ticks=120, transition_mode='inertial')])
        self.good('runtime.step', **params)
        before = self.entity()
        self.assertTrue(self.good('runtime.step', **params)['replayed'])
        for mode in [None, 0, True, [], {}, 'Inertial', 'spring']:
            retry = json.loads(json.dumps(params))
            retry['animations'][0]['transition_mode'] = mode
            self.error(self.call('runtime.step', **retry), -32602)
        changed = json.loads(json.dumps(params))
        changed['animations'][0]['transition_mode'] = 'crossfade'
        self.error(self.call('runtime.step', **changed), -32010)
        self.assertEqual(self.entity(), before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    legacy.ARGS = args
    # The legacy test suite runs separately; select only this checkpoint's tests.
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(InertialContract)
             if name.startswith('test_inertial_')]
    if not args.runtime:
        names = []
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite(InertialContract(name) for name in names))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({'passed': result.wasSuccessful(),
            'tests': result.testsRun, 'runtime': bool(args.runtime),
            'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'reports': legacy.REPORTS}, indent=2) + '\n')
    return not result.wasSuccessful()


if __name__ == '__main__':
    raise SystemExit(main())
