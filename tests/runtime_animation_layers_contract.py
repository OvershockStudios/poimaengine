#!/usr/bin/env python3
"""Frozen animation layers checked against independent poses and durable state."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import unittest
import uuid
import runtime_animation_blend_contract as legacy
from runtime_animation_inertial_contract import correction
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/python'))
from poima_client import WorldClient


def authored(slot=1, mode='override', clip=1, weight=.5, mask=None, **extra):
    return dict(slot=slot, mode=mode, clip=clip, time=0, speed=1, loop=False,
                playing=False, weight=weight,
                mask=[dict(node=1, weight=1)] if mask is None else mask, **extra)


def layer_command(rig, slot=1, clip=1, weight=1, **extra):
    options = dict(playing=False, loop=False, layer=slot, weight=weight)
    options.update(extra)
    return legacy.command(rig, clip=clip, **options)


class LayersContract(legacy.BlendContract):
    def prepare_layers(self, layers=None, two=False):
        asset = self.prepare(two=two)
        self.layers = [authored()] if layers is None else layers
        self.rig_definition = dict(asset=asset, clip=0, time=0, speed=1, loop=False,
                                   playing=True, layers=self.layers)
        self.good('world.transact', request_id=uuid.uuid4().hex, base_revision=1,
                  ops=[legacy.component(self.rig, 'AnimationRig', self.rig_definition)])
        self.before = self.world.read_bytes()
        return asset

    def start(self, session=None):
        if session:
            self.session = session
        self.tick = 0
        return self.good('runtime.start', session_id=self.session, revision=2)

    def layers_state(self):
        return self.animation()['layers']

    def capture(self):
        return dict(tick=self.tick, rig=self.animation(), node=self.entity(),
                    observe=self.good('runtime.observe', session_id=self.session,
                        tick=self.tick, ids=[self.rig, self.tip], entity_fields=['animation', 'local_transform', 'world_matrix']))

    def test_layers_sparse_override_and_observation(self):
        self.prepare_layers([authored(mask=[dict(node=1, weight=.5)])])
        self.start(); self.step(30)
        # Base A(.5)=(.5,1), override B(0)=(2,3), alpha=.5*.5=.25.
        self.pose((.875, 1.5, 0))
        root = self.entity(legacy.node(self.rig, 2))
        self.assertEqual(root['local_transform'], legacy.transform())
        state = self.layers_state()[0]
        self.assertEqual((state['slot'], state['mode'], state['mask_nodes']), (1, 'override', 1))
        self.assertEqual((state['weight'], state['target_weight']), (.5, .5))
        self.assertIsNone(state['weight_transition'])
        observed = self.good('runtime.observe', session_id=self.session, tick=self.tick,
                             ids=[self.rig], entity_fields=['animation'])
        self.assertEqual(observed['entities'][0]['state']['animation'], self.animation())
        self.assertEqual(self.world.read_bytes(), self.before)

    def test_layers_additive_static_reference_and_sorted_slots(self):
        self.prepare_layers([authored(mode='additive', clip=1, weight=.5,
                                     reference_clip=0, reference_time=.5)])
        self.start(); self.step(30)
        # additive B(0)-A(.5)=(1.5,2), independent of advancing base A(t).
        self.pose((1.25, 2, 0)); self.step(30); self.pose((1.75, 2, 0))
        self.good('runtime.stop', session_id=self.session)
        layers = [authored(slot=4, clip=0, weight=.5), authored(slot=1, clip=1, weight=.5)]
        self.rig_definition['layers'] = layers
        self.good('world.transact', request_id=uuid.uuid4().hex, base_revision=2,
                  ops=[legacy.component(self.rig, 'AnimationRig', self.rig_definition)])
        self.session = uuid.uuid4().hex
        self.tick = 0
        self.good('runtime.start', session_id=self.session, revision=3)
        self.step(30); self.pose((.625, 1.5, 0))
        self.assertEqual([x['slot'] for x in self.layers_state()], [1, 4])

    def test_layers_weight_interruption_and_receipt(self):
        self.prepare_layers([authored(weight=0)])
        self.start()
        params = self.params(30, [layer_command(self.rig, weight=1, weight_blend_ticks=60)])
        result = self.good('runtime.step', **params); self.tick = 30
        self.pose((1.25, 2, 0))
        self.assertEqual(self.good('runtime.step', **params), dict(result, replayed=True))
        row = self.layers_state()[0]
        self.assertEqual(row['weight_transition'], dict(start_tick=0, duration_ticks=60,
                         elapsed_ticks=30, source=0, target=1))
        self.step(15, [layer_command(self.rig, weight=0, weight_blend_ticks=30)])
        self.pose((1.0625, 1.5, 0)); self.assertEqual(self.layers_state()[0]['weight'], .25)
        self.step(15); self.pose((1, 1, 0))
        self.assertIsNone(self.layers_state()[0]['weight_transition'])
        # Same base entity and layer slot may be commanded together.
        self.step(1, [legacy.command(self.rig, time=.5, playing=False), layer_command(self.rig, weight=1)])
        self.pose((2, 3, 0))

    def test_layers_base_history_and_layer_history_are_independent(self):
        self.prepare_layers([authored(weight=1)])
        self.start(); self.step(30)
        self.step(15, [legacy.command(self.rig, clip=1, playing=False, loop=False,
            blend_ticks=60, transition_mode='inertial'), layer_command(self.rig, weight=0)])
        # Base source .5 and dx/dt1; upper override must not become base source2.
        self.pose((2 + correction(-1.5, 1, .25, 1), 3 + correction(-2, 0, .25, 1), 0))
        self.step(1, [layer_command(self.rig, clip=0, weight=1, blend_ticks=60,
                                  transition_mode='inertial')])
        # Layer has remained stationary B(0)=(2,3), including while weight0.
        t = 1/60
        self.pose((correction(2, 0, t, 1), 1+correction(2, 0, t, 1), 0))

    def test_layers_rejection_atomicity_and_frozen_authoring(self):
        self.prepare_layers(); self.start(); self.step(30)
        before = self.capture()
        for animation in [layer_command(self.rig, slot=0), layer_command(self.rig, slot=2),
                layer_command(self.rig, slot=5), layer_command(self.rig, weight=-.1),
                layer_command(self.rig, weight=1.1), layer_command(self.rig, weight_blend_ticks=3601)]:
            reply = self.call('runtime.step', **self.params(1, [animation]))
            self.assertIn(reply.get('error', {}).get('code'), (-32602, -32040))
            self.assertEqual(self.capture(), before)
        duplicate = [layer_command(self.rig), layer_command(self.rig, clip=0)]
        self.error(self.call('runtime.step', **self.params(1, duplicate)), -32602)
        missing = layer_command(self.rig); del missing['weight']
        self.error(self.call('runtime.step', **self.params(1, [missing])), -32602)
        # Endpoints are valid but interior scale becomes negative after earlier
        # ticks: reject whole batch, including histories and weight transitions.
        self.error(self.call('runtime.step', **self.params(60, [layer_command(self.rig,
            clip=2, playing=True, weight=.75, weight_blend_ticks=30)])), -32040)
        self.assertEqual(self.capture(), before)
        changed = copy.deepcopy(self.rig_definition); changed['layers'][0]['weight'] = 0
        self.good('world.transact', request_id=uuid.uuid4().hex, base_revision=2,
                  ops=[legacy.component(self.rig, 'AnimationRig', changed)])
        self.assertEqual(self.layers_state()[0]['weight'], .5)
        self.good('runtime.stop', session_id=self.session)
        self.session = uuid.uuid4().hex; self.tick = 0
        self.good('runtime.start', session_id=self.session, revision=3)
        self.assertEqual(self.layers_state()[0]['weight'], 0)

    def test_layers_authored_validation_and_legacy_omission(self):
        self.prepare_layers()
        before = self.world.read_bytes()
        for layers in [[authored(), authored()], [authored(slot=0)], [authored(slot=5)],
                [authored(mask=[dict(node=1, weight=1), dict(node=1, weight=.5)])],
                [authored(mask=[dict(node=999, weight=1)])],
                [authored(mask=[dict(node=1, weight=-1)])],
                [authored(reference_clip=0)], [authored(weight=1.1)]]:
            bad = dict(self.rig_definition, layers=layers)
            self.error(self.call('world.transact', request_id=uuid.uuid4().hex, base_revision=2,
                ops=[legacy.component(self.rig, 'AnimationRig', bad)]), -32602)
            self.assertEqual(self.world.read_bytes(), before)
        if legacy.ARGS.runtime:
            self.start(); self.step(1)
            self.assertEqual(len(self.layers_state()), 1)

    def test_layers_discovery_and_runtime_gate(self):
        discovery = self.good('world.describe')
        self.assertEqual(discovery['runtime_available'], bool(legacy.ARGS.runtime))
        layer_schema = discovery['components']['AnimationRig']['properties']['layers']
        self.assertEqual((layer_schema['type'], layer_schema['maxItems']), ('array', 4))
        slot = layer_schema['items']['properties']['slot']
        self.assertEqual((slot['minimum'],slot['maximum']), (1,4))
        self.assertEqual(layer_schema['items']['properties']['mode']['enum'], ['override','additive'])
        self.assertIn('mask', layer_schema['items']['required'])
        commands = discovery['methods']['runtime.step']['properties']['animations']['items']
        common = {'entity','clip','time','speed','loop','playing'}
        self.assertEqual(set(commands['required']), common)
        self.assertTrue(common <= commands['properties'].keys())
        branches = commands['oneOf']
        self.assertEqual(len(branches), 2)
        base_branch = next(branch for branch in branches if 'layer' not in branch['properties'])
        layer_branch = next(branch for branch in branches if 'layer' in branch['properties'])
        self.assertEqual(set(base_branch['required']), common)
        self.assertEqual(set(layer_branch['required']), common | {'layer','weight'})
        for branch in branches:
            self.assertEqual(branch['type'], 'object')
            self.assertIs(branch['additionalProperties'], False)
            for name in common | {'blend_ticks','transition_mode'}:
                self.assertEqual(branch['properties'][name], commands['properties'][name])
        for name in ['layer','weight','weight_blend_ticks']:
            self.assertNotIn(name, base_branch['properties'])
        self.assertEqual(layer_branch['properties']['layer'], {'type':'integer','minimum':1,'maximum':4})
        self.assertEqual(layer_branch['properties']['weight'], {'type':'number','minimum':0,'maximum':1})
        self.assertEqual(layer_branch['properties']['weight_blend_ticks'],
                         {'type':'integer','minimum':0,'maximum':3600,'default':0})
        self.prepare_layers()
        if not legacy.ARGS.runtime:
            self.error(self.call('runtime.start', session_id=self.session, revision=2), -32003)
            self.assertEqual(self.world.read_bytes(), self.before)
            # No fallback to a partially active layered simulation.
            self.error(self.call('runtime.entity', session_id=self.session, id=self.rig), -32030)
        else:
            self.start()
            self.assertEqual([x['slot'] for x in self.layers_state()], [1])

    def test_layers_fresh_save_and_immediate_interruption(self):
        self.prepare_layers([authored(weight=.8)])
        self.start(); self.step(30)
        self.step(17, [legacy.command(self.rig, clip=1, playing=False, loop=False,
            blend_ticks=120, transition_mode='inertial'),
            layer_command(self.rig, clip=0, weight=.3, weight_blend_ticks=90,
                          blend_ticks=120, transition_mode='inertial')])
        saves = self.root/'saves'; saves.mkdir()
        generation = self.good('save.configure', request_id=uuid.uuid4().hex,
                               expected_generation=0, root=self.native(saves))['generation']
        saved = self.good('save.write', request_id=uuid.uuid4().hex,
            configuration_generation=generation, slot='layers', expected_generation=0,
            session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=0)
        freshworld = self.root/'fresh-world.json'; shutil.copyfile(self.world, freshworld)
        shutil.copytree(Path(str(self.world)+'.assets'), Path(str(freshworld)+'.assets'))
        fresh_session = uuid.uuid4().hex
        with WorldClient.open(str(legacy.ARGS.binary.resolve()), self.native(freshworld), close_timeout=10) as fresh:
            calls = []
            def call(method, **params):
                value = fresh.call(method, params, timeout=30)
                calls.append(dict(method=method, params=params, result=value))
                return value
            configured = call('save.configure', request_id=uuid.uuid4().hex,
                              expected_generation=0, root=self.native(saves))['generation']
            loaded = call('save.load', request_id=uuid.uuid4().hex,
                configuration_generation=configured, slot='layers', expected_generation=1,
                revision=2, expected_session_id=None, expected_tick=None,
                expected_gameplay_revision=None, new_session_id=fresh_session)
            self.assertEqual(loaded['tick'], self.tick)
            def detach(value):
                if isinstance(value, dict):
                    return {k:detach(v) for k,v in value.items() if k!='session_id'}
                if isinstance(value, list): return [detach(v) for v in value]
                return value
            def same():
                for identity in [self.rig, self.tip, legacy.node(self.rig, 2)]:
                    self.assertEqual(detach(self.entity(identity)), detach(call('runtime.entity',
                        session_id=fresh_session, id=identity)))
            same()
            repeated = call('save.write', request_id=uuid.uuid4().hex,
                configuration_generation=configured, slot='fresh', expected_generation=0,
                session_id=fresh_session, expected_tick=self.tick, expected_gameplay_revision=0)
            self.assertEqual((saved['sha256'],saved['bytes']), (repeated['sha256'],repeated['bytes']))
            commands = [layer_command(self.rig, clip=1, weight=.7, weight_blend_ticks=60,
                          blend_ticks=60, transition_mode='inertial')]
            tick = self.tick; self.step(1, commands)
            call('runtime.step', session_id=fresh_session, request_id=uuid.uuid4().hex,
                 expected_tick=tick, ticks=1, animations=commands)
            same(); tick = self.tick; self.step(25)
            call('runtime.step', session_id=fresh_session, request_id=uuid.uuid4().hex,
                 expected_tick=tick, ticks=7)
            call('runtime.step', session_id=fresh_session, request_id=uuid.uuid4().hex,
                 expected_tick=tick+7, ticks=18)
            same()
        self.assertTrue(fresh.closed)
        self.assertEqual(fresh.transport.returncode, 0)
        legacy.REPORTS.append(dict(test=self.id()+'.fresh', calls=calls,
                                  exit_code=fresh.transport.returncode))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--runtime', type=int, choices=[0, 1], default=1)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args(); legacy.ARGS = args
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(LayersContract)
             if name.startswith('test_layers_')]
    if not args.runtime:
        names = ['test_layers_authored_validation_and_legacy_omission',
                 'test_layers_discovery_and_runtime_gate']
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(LayersContract(name) for name in names))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(dict(passed=result.wasSuccessful(), tests=result.testsRun,
            runtime=bool(args.runtime), binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), reports=legacy.REPORTS), indent=2)+'\n')
    return not result.wasSuccessful()


if __name__ == '__main__':
    raise SystemExit(main())
