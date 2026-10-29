#!/usr/bin/env python3
"""Directional contact-shadow authoring/save contracts through the real native CLI.

Capture option checks deliberately fail later common validation before any
window/device initialization. Rendered visibility and diagnostics need their
separate native GPU qualification.
"""
# SPDX-License-Identifier: Apache-2.0
import json
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

from world_contract import BINARY, SCRATCH, Client, create, native, uid


MODES = ['pcf3_point', 'pcf3_fractional', 'pcss_directional']
FILTER_ERROR = 'shadow_filter must be pcf3_point, pcf3_fractional or pcss_directional.'


def component(n, kind, value):
    return dict(op='component.set', id=uid(n), type=kind, value=value)


def light(n, kind='directional', **changes):
    return component(n, 'Light', {'kind': kind, 'color': [1, 1, 1], 'intensity': 1,
                                'enabled': True, **changes})


def environment(n, mode='pcss_directional'):
    return component(n, 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1,
                                                 shadow_filter=mode))


class ContactShadowContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.features = json.loads(subprocess.check_output(
            [BINARY, 'capabilities'], text=True))['result']['features']

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='contact-shadows-', dir=SCRATCH)
        self.path = Path(self.directory.name) / 'world.json'
        self.clients = []

    def tearDown(self):
        try:
            for client in self.clients:
                client.close()
                self.assertEqual(client.process.returncode, 0, client.errors)
        finally:
            self.directory.cleanup()

    def open(self):
        client = Client(self.path)
        self.clients.append(client)
        return client

    def test_schema_and_focused_discovery_keep_complete_branches(self):
        c = self.open()
        d = c.rpc('world.describe')
        self.assertEqual(d['schema_revision'], 120)
        variants = d['components']['Light']['oneOf']
        self.assertEqual(len(variants), 3)
        for branch in variants:
            kind = branch['properties']['kind']['const']
            shadow = branch['properties']['shadow']
            self.assertFalse(shadow['additionalProperties'])
            if kind == 'directional':
                angular = shadow['properties']['angular_size_degrees']
                self.assertEqual((angular['type'], angular['minimum'], angular['maximum']),
                                 ('number', 0, 20))
                self.assertNotIn('angular_size_degrees', shadow['required'])
                self.assertNotIn('default', angular)  # Renderer default is not stored data.
                self.assertIn('0.53', angular['description'])
            else:
                self.assertNotIn('angular_size_degrees', shadow['properties'])
        self.assertEqual(d['components']['LightingEnvironment']['properties']['shadow_filter']['enum'], MODES)
        self.assertEqual(d['components']['LightingEnvironment']['properties']['shadow_filter']['default'],
                         'pcf3_point')
        for method in ('world.capture', 'runtime.capture', 'runtime.play', 'asset.animation.capture'):
            self.assertEqual(d['methods'][method]['properties']['shadow_filter']['enum'], MODES)
        focused = c.rpc('world.describe', dict(view='component', name='Light'))
        self.assertEqual(focused['components']['Light'], d['components']['Light'])
        projected = c.rpc('world.describe', dict(view='mutation', operation='component.set', type='Light'))
        envelope = projected['methods']['world.transact']
        full = d['methods']['world.transact']
        for key in ('request_id', 'base_revision', 'preview'):
            self.assertEqual(envelope['properties'][key], full['properties'][key])
        self.assertEqual(envelope['required'], full['required'])
        branches = envelope['properties']['ops']['items']['oneOf']
        self.assertEqual(len(branches), 1)
        self.assertEqual(branches[0]['properties']['type'], {'const': 'Light'})
        self.assertEqual(branches[0]['properties']['value'], d['components']['Light'])
        self.assertFalse(self.path.exists())

    def test_legacy_omitted_fields_keep_stored_and_lighting_shapes(self):
        c = self.open()
        c.txn(0, [create(1), light(1, shadow={'enabled': True}), create(2),
                  light(2, 'point', shadow={'enabled': False}),
                  component(1, 'LightingEnvironment', dict(ambient=[0, 0, 0], exposure=1))])
        stored = c.rpc('entity.get', dict(id=uid(1)))['value']['components']
        self.assertEqual(stored['Light']['shadow'], {'enabled': True})
        self.assertNotIn('shadow_filter', stored['LightingEnvironment'])
        state = c.rpc('world.lighting')
        self.assertEqual(state['shadow_filter'], 'pcf3_point')
        self.assertNotIn('contact_shadows', state)
        for resolved in state['lights']:
            self.assertEqual(set(resolved), {'id', 'kind', 'position', 'direction', 'color',
                                            'intensity', 'range', 'inner_angle', 'outer_angle',
                                            'intensity_unit', 'shadow'})
            self.assertEqual(set(resolved['shadow']), {'enabled', 'near', 'distance',
                                                      'bias', 'normal_bias'})
        c.close()
        c = self.open()
        self.assertEqual(c.rpc('world.lighting'), state)
        c.txn(1, [environment(1)])
        current = c.rpc('world.lighting')
        self.assertEqual(current['shadow_filter'], 'pcss_directional')
        self.assertTrue(all('angular_size_degrees' not in x['shadow'] for x in current['lights']))
        self.assertEqual(c.rpc('entity.get', dict(id=uid(1)))['value']['components']['Light'],
                         stored['Light'])

    def test_strict_angular_validation_is_atomic_even_when_disabled(self):
        c = self.open()
        c.txn(0, [create(1), light(1, shadow={'enabled': True}), environment(1)])
        original = self.path.read_bytes()
        state, history = c.rpc('world.inspect'), c.rpc('world.history')
        resolved = c.rpc('world.lighting')
        invalid = [light(1, shadow={'enabled': False, 'angular_size_degrees': value})
                   for value in (None, True, False, '0.53', '', [], {}, -.000000001, 20.000000001)]
        invalid += [light(1, kind, enabled=False,
                          shadow={'enabled': False, 'angular_size_degrees': 0})
                    for kind in ('point', 'spot')]
        invalid += [light(1, angular_size_degrees=.53)]
        transform = component(1, 'Transform', dict(position=[99, 0, 0],
                              rotation=[0, 0, 0, 1], scale=[1, 1, 1]))
        for op in invalid:
            with self.subTest(value=op['value']):
                c.txn(1, [transform, op], error=-32602)
                self.assertEqual(self.path.read_bytes(), original)
                self.assertEqual(c.rpc('world.inspect'), state)
                self.assertEqual(c.rpc('world.history'), history)
                self.assertEqual(c.rpc('world.lighting'), resolved)
        for bad in (float('nan'), float('inf'), -float('inf')):
            request = dict(jsonrpc='2.0', id='nonfinite', method='world.transact', params=dict(
                request_id=uuid.uuid4().hex, base_revision=1,
                ops=[light(1, shadow={'enabled': True, 'angular_size_degrees': bad})]))
            self.assertEqual(c.raw(json.dumps(request))['error']['code'], -32700)
            self.assertEqual(self.path.read_bytes(), original)
        # These bounds are checked on the input double before native float conversion.
        for revision, angular in enumerate((0, .53, 20), 1):
            c.txn(revision, [light(1, shadow={'enabled': False, 'angular_size_degrees': angular})])
            stored = c.rpc('entity.get', dict(id=uid(1)))['value']['components']['Light']['shadow']
            self.assertEqual(stored['angular_size_degrees'], angular)
            self.assertAlmostEqual(c.rpc('world.lighting')['lights'][0]['shadow']['angular_size_degrees'],
                                   angular, places=6)

    def test_preview_replay_revision_and_omission_are_preserved(self):
        c = self.open()
        c.txn(0, [create(1), light(1, shadow={'enabled': True})])
        before, history = self.path.read_bytes(), c.rpc('world.history')
        request = dict(base_revision=1, request_id=uuid.uuid4().hex,
                       ops=[light(1, shadow={'enabled': True, 'angular_size_degrees': 0}), environment(1)])
        c.rpc('world.transact', dict(request, preview=True))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(c.rpc('world.history'), history)
        c.rpc('world.transact', request)
        committed = self.path.read_bytes()
        self.assertTrue(c.rpc('world.transact', request)['replayed'])
        self.assertEqual(self.path.read_bytes(), committed)
        c.txn(1, [environment(1)], error=-32009)
        self.assertEqual(self.path.read_bytes(), committed)
        self.assertEqual(c.rpc('world.lighting')['lights'][0]['shadow']['angular_size_degrees'], 0)
        c.txn(2, [light(1, shadow={'enabled': True})])
        self.assertNotIn('angular_size_degrees', c.rpc('world.lighting')['lights'][0]['shadow'])
        c.rpc('world.undo', dict(base_revision=3, request_id=uuid.uuid4().hex))
        self.assertEqual(c.rpc('world.lighting')['lights'][0]['shadow']['angular_size_degrees'], 0)

    def test_capture_and_play_filter_options_reject_before_graphics(self):
        c = self.open()
        c.txn(0, [create(1), component(1, 'Camera', dict(vertical_fov=60, near=.1, far=100))])
        before, history = self.path.read_bytes(), c.rpc('world.history')
        player = c.rpc('player.inspect')
        requests = [('world.capture', dict(revision=1, camera=uid(1))),
                    ('asset.animation.capture', dict(revision=1, camera=uid(1), asset='0'*64, time=0))]
        session, runtime = uid(900), None
        if self.features['simulation']:
            c.rpc('runtime.start', dict(session_id=session, revision=1))
            runtime = c.rpc('runtime.inspect', dict(session_id=session))
            requests += [('runtime.capture', dict(session_id=session, tick=0, camera=uid(1))),
                         ('runtime.play', dict(session_id=session, request_id=uuid.uuid4().hex,
                                              expected_tick=0, camera=uid(1), mode='interactive', max_frames=0))]
        for method, base in requests:
            output = Path(self.directory.name) / (method+'.bmp')
            params = dict(base, path=native(output), samples=1, scene_debug_view='ambient_occlusion')
            for mode in MODES:
                denied = c.rpc(method, dict(params, shadow_filter=mode), error=-32602)
                self.assertEqual(denied['message'], 'ambient_occlusion debug view requires enabled ambient occlusion.')
            for mode in ('unknown', 'PCSS_DIRECTIONAL', True, 2, 2.0, None, [], {}):
                denied = c.rpc(method, dict(params, shadow_filter=mode), error=-32602)
                self.assertEqual(denied['message'], FILTER_ERROR)
            self.assertFalse(output.exists())
            self.assertEqual(self.path.read_bytes(), before)
            self.assertEqual(c.rpc('world.history'), history)
            self.assertEqual(c.rpc('player.inspect'), player)
            if runtime is not None:
                self.assertEqual(c.rpc('runtime.inspect', dict(session_id=session)), runtime)

    def test_frozen_lighting_and_complete_cold_save_roundtrip(self):
        if not self.features['simulation']:
            self.skipTest('Simulation disabled in this build')
        c = self.open()
        c.txn(0, [create(1), light(1, shadow={'enabled': True, 'angular_size_degrees': 2}),
                  create(2), light(2, shadow={'enabled': True}), create(3),
                  component(3, 'Camera', dict(vertical_fov=60, near=.1, far=100)), environment(3)])
        session = uid(910)
        c.rpc('runtime.start', dict(session_id=session, revision=1))
        frozen = c.rpc('runtime.lighting', dict(session_id=session))
        c.txn(1, [light(1, shadow={'enabled': True, 'angular_size_degrees': 5}), environment(3, 'pcf3_point')])
        self.assertEqual(c.rpc('runtime.lighting', dict(session_id=session)), frozen)
        self.assertEqual(c.rpc('world.lighting')['shadow_filter'], 'pcf3_point')
        saves = Path(self.directory.name) / 'saves'
        saves.mkdir()
        c.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        c.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='original',
              expected_generation=0, session_id=session, expected_tick=0, expected_gameplay_revision=0))
        original = next((saves/'slot-original').glob('*.bin')).read_bytes()
        saved = json.loads(original)
        self.assertEqual(saved['document']['entities'][uid(1)]['components']['Light']['shadow']['angular_size_degrees'], 2)
        self.assertNotIn('angular_size_degrees', saved['document']['entities'][uid(2)]['components']['Light']['shadow'])
        self.assertEqual(saved['document']['entities'][uid(3)]['components']['LightingEnvironment']['shadow_filter'],
                         'pcss_directional')
        authored = self.path.read_bytes()
        c.close()
        c = self.open()
        c.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        restored_session = uid(911)
        c.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='original',
              expected_generation=1, revision=2, expected_session_id=None, expected_tick=None,
              expected_gameplay_revision=None, new_session_id=restored_session))
        restored = c.rpc('runtime.lighting', dict(session_id=restored_session))
        self.assertEqual(restored, dict(frozen, session_id=restored_session))
        c.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='probe',
              expected_generation=0, session_id=restored_session, expected_tick=0, expected_gameplay_revision=0))
        probe = next((saves/'slot-probe').glob('*.bin')).read_bytes()
        self.assertEqual(probe, original)
        self.assertEqual(json.loads(probe), saved)  # Complete documents; no field exclusions.
        self.assertEqual(self.path.read_bytes(), authored)


if __name__ == '__main__':
    unittest.main(argv=['contact_shadow_contract'], verbosity=2)
