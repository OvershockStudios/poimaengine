#!/usr/bin/env python3
"""Optional procedural-sky IBL authoring and complete-save native contracts.

These checks use the native CLI without creating a graphics device. Renderer
convolution, material response, resources and costs have separate GPU checks.
"""
# SPDX-License-Identifier: Apache-2.0
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

from world_contract import BINARY, SCRATCH, Client, create, native, uid


def sky(enabled=True):
    return dict(enabled=enabled, zenith=[.06, .22, .55], horizon=[.55, .70, .85],
                ground=[.12, .10, .08], horizon_falloff=.35, sun=None,
                sun_size_degrees=.53, sun_intensity=20)


def ibl(enabled=True, diffuse=1, specular=1):
    return dict(enabled=enabled, diffuse_intensity=diffuse, specular_intensity=specular)


def environment(**fields):
    return dict(ambient=[0, 0, 0], exposure=1, **fields)


def component(value):
    return dict(op='component.set', id=uid(1), type='LightingEnvironment', value=value)


class EnvironmentIblContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.features = json.loads(subprocess.check_output(
            [BINARY, 'capabilities'], text=True))['result']['features']

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='environment-ibl-', dir=SCRATCH)
        self.path = Path(self.directory.name) / 'world.json'
        self.clients = []

    def tearDown(self):
        try:
            for client in self.clients:
                client.close()
                self.assertEqual(client.process.returncode, 0, client.errors)
        finally:
            self.directory.cleanup()

    def open(self, path=None):
        client = Client(path or self.path)
        self.clients.append(client)
        return client

    def test_discovery_preserves_complete_environment_and_mutation_envelope(self):
        c = self.open()
        d = c.rpc('world.describe')
        self.assertEqual(d['schema_revision'], 125)
        full = d['components']['LightingEnvironment']
        self.assertNotIn('ibl', full['required'])
        branch = full['properties']['ibl']
        self.assertFalse(branch['additionalProperties'])
        self.assertEqual(set(branch['required']),
                         {'enabled', 'diffuse_intensity', 'specular_intensity'})
        self.assertEqual(branch['properties']['enabled'], {'type': 'boolean'})
        self.assertNotIn('default', branch)
        for field in ('diffuse_intensity', 'specular_intensity'):
            self.assertEqual(branch['properties'][field],
                             {'type': 'number', 'minimum': 0, 'maximum': 64})
        self.assertEqual(set(full['properties']['sky']['required']), set(sky()))
        dependency, = full['allOf']
        self.assertEqual(dependency['if']['required'], ['ibl'])
        self.assertEqual(dependency['if']['properties']['ibl']['properties']['enabled'], {'const': True})
        self.assertEqual(dependency['then']['required'], ['sky'])
        self.assertEqual(dependency['then']['properties']['sky']['properties']['enabled'], {'const': True})
        self.assertEqual(c.rpc('world.describe', dict(view='component', name='LightingEnvironment'))
                         ['components']['LightingEnvironment'], full)
        projected = c.rpc('world.describe', dict(view='mutation', operation='component.set',
                                               type='LightingEnvironment'))['methods']['world.transact']
        envelope = d['methods']['world.transact']
        for field in ('request_id', 'base_revision', 'preview'):
            self.assertEqual(projected['properties'][field], envelope['properties'][field])
        self.assertEqual(projected['required'], envelope['required'])
        selected, = projected['properties']['ops']['items']['oneOf']
        self.assertEqual(selected['properties']['type'], {'const': 'LightingEnvironment'})
        self.assertEqual(selected['properties']['value'], full)
        self.assertFalse(self.path.exists())

    def test_omission_and_explicit_disabled_presence_survive_reopen(self):
        c = self.open()
        self.assertNotIn('ibl', c.rpc('world.lighting'))
        c.txn(0, [create(1), component(environment(sky=sky()))])
        original = c.rpc('entity.get', dict(id=uid(1)))['value']['components']['LightingEnvironment']
        resolved = c.rpc('world.lighting')
        self.assertNotIn('ibl', original)
        self.assertNotIn('ibl', resolved)
        c.close()
        c = self.open()
        self.assertEqual(c.rpc('world.lighting'), resolved)
        self.assertEqual(c.rpc('entity.get', dict(id=uid(1)))['value']['components']['LightingEnvironment'], original)
        disabled = ibl(False, 0, 64)
        c.txn(1, [component(environment(ibl=disabled))])
        self.assertEqual(c.rpc('world.lighting')['ibl'], disabled)
        self.assertEqual(c.rpc('entity.get', dict(id=uid(1)))['value']['components']['LightingEnvironment'],
                         environment(ibl=disabled))
        c.close()
        c = self.open()
        self.assertEqual(c.rpc('world.lighting')['ibl'], disabled)
        c.txn(2, [component(environment())])
        self.assertNotIn('ibl', c.rpc('world.lighting'))
        self.assertNotIn('ibl', c.rpc('entity.get', dict(id=uid(1)))['value']['components']['LightingEnvironment'])

    def test_strict_ibl_fields_reject_atomically_even_when_disabled(self):
        c = self.open()
        c.txn(0, [create(1), component(environment())])
        original = self.path.read_bytes()
        state, history, lighting = c.rpc('world.inspect'), c.rpc('world.history'), c.rpc('world.lighting')
        invalid = [None, True, False, 1, 'ibl', [], {},
                   {'enabled': False}, {'enabled': False, 'diffuse_intensity': 1},
                   dict(ibl(False), unexpected=0)]
        invalid += [ibl(value) for value in (0, 1, None, 'false', [], {})]
        for field in ('diffuse_intensity', 'specular_intensity'):
            for value in (None, True, False, '1', [], {}, -.000000001, 64.000000001):
                invalid.append(dict(ibl(False), **{field: value}))
        transform = dict(op='component.set', id=uid(1), type='Transform',
                         value=dict(position=[99, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1]))
        for value in invalid:
            with self.subTest(ibl=value):
                c.txn(1, [transform, component(environment(sky=sky(), ibl=value))], error=-32602)
                self.assertEqual(self.path.read_bytes(), original)
                self.assertEqual(c.rpc('world.inspect'), state)
                self.assertEqual(c.rpc('world.history'), history)
                self.assertEqual(c.rpc('world.lighting'), lighting)
        for field in ('diffuse_intensity', 'specular_intensity'):
            for value in (float('nan'), float('inf'), -float('inf')):
                request = dict(jsonrpc='2.0', id='nonfinite', method='world.transact', params=dict(
                    request_id=uuid.uuid4().hex, base_revision=1,
                    ops=[component(environment(ibl=dict(ibl(False), **{field: value})))]))
                self.assertEqual(c.raw(json.dumps(request))['error']['code'], -32700)
                self.assertEqual(self.path.read_bytes(), original)
        for revision, gains in enumerate(((0, 64), (64, 0), (.5, 2)), 1):
            chosen = ibl(False, *gains)
            c.txn(revision, [component(environment(ibl=chosen))])
            self.assertEqual(c.rpc('world.lighting')['ibl'], chosen)

    def test_enabled_ibl_requires_enabled_sky_and_rejects_partial_changes(self):
        c = self.open()
        c.txn(0, [create(1), component(environment())])
        original = self.path.read_bytes()
        for value in (environment(ibl=ibl()), environment(sky=sky(False), ibl=ibl())):
            denied = c.txn(1, [component(value)], error=-32602)
            self.assertEqual(denied['message'], 'Enabled IBL requires an enabled procedural sky.')
            self.assertEqual(self.path.read_bytes(), original)
        full = environment(sky=sky(), ibl=ibl(True, 0, 0))
        c.txn(1, [component(full)])
        self.assertEqual(c.rpc('world.lighting')['ibl'], full['ibl'])
        before = self.path.read_bytes()
        disabled_sky = copy.deepcopy(full)
        disabled_sky['sky']['enabled'] = False
        c.txn(2, [component(disabled_sky)], error=-32602)
        self.assertEqual(self.path.read_bytes(), before)
        disabled_sky['ibl']['enabled'] = False
        c.txn(2, [component(disabled_sky)])
        resolved = c.rpc('world.lighting')
        self.assertFalse(resolved['sky']['enabled'])
        self.assertEqual(resolved['ibl'], ibl(False, 0, 0))

    def test_preview_retry_revision_undo_and_redo_preserve_presence(self):
        c = self.open()
        c.txn(0, [create(1), component(environment())])
        before, history = self.path.read_bytes(), c.rpc('world.history')
        value = environment(sky=sky(), ibl=ibl(True, 2, .5))
        request = dict(base_revision=1, request_id=uuid.uuid4().hex, ops=[component(value)])
        c.rpc('world.transact', dict(request, preview=True))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(c.rpc('world.history'), history)
        c.rpc('world.transact', request)
        committed = self.path.read_bytes()
        self.assertTrue(c.rpc('world.transact', request)['replayed'])
        self.assertEqual(self.path.read_bytes(), committed)
        c.txn(1, [component(environment())], error=-32009)
        self.assertEqual(self.path.read_bytes(), committed)
        c.rpc('world.undo', dict(base_revision=2, request_id=uuid.uuid4().hex))
        self.assertNotIn('ibl', c.rpc('world.lighting'))
        c.rpc('world.redo', dict(base_revision=3, request_id=uuid.uuid4().hex))
        self.assertEqual(c.rpc('world.lighting')['ibl'], value['ibl'])
        self.assertEqual(c.rpc('entity.get', dict(id=uid(1)))['value']['components']['LightingEnvironment'], value)

    def test_frozen_enabled_disabled_and_omitted_complete_cold_saves(self):
        if not self.features['simulation']:
            self.skipTest('Simulation disabled in this build')
        cases = [environment(), environment(ibl=ibl(False, 0, 64)),
                 environment(sky=sky(), ibl=ibl(True, 2, .5))]
        for number, value in enumerate(cases):
            with self.subTest(environment=value):
                path = Path(self.directory.name) / ('saved-'+str(number)+'.json')
                c = self.open(path)
                c.txn(0, [create(1), component(value)])
                session = uid(900+number)
                c.rpc('runtime.start', dict(session_id=session, revision=1))
                frozen = c.rpc('runtime.lighting', dict(session_id=session))
                if 'ibl' in value:
                    self.assertEqual(frozen['ibl'], value['ibl'])
                else:
                    self.assertNotIn('ibl', frozen)
                changed = environment(sky=sky(), ibl=ibl(False, 7, 3))
                c.txn(1, [component(changed)])
                self.assertEqual(c.rpc('runtime.lighting', dict(session_id=session)), frozen)
                self.assertEqual(c.rpc('world.lighting')['ibl'], changed['ibl'])
                saves = Path(self.directory.name) / ('saves-'+str(number))
                saves.mkdir()
                c.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
                c.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='original',
                      expected_generation=0, session_id=session, expected_tick=0, expected_gameplay_revision=0))
                original = next((saves/'slot-original').glob('*.bin')).read_bytes()
                full = json.loads(original)
                self.assertEqual(full['document']['entities'][uid(1)]['components']['LightingEnvironment'], value)
                authored = path.read_bytes()
                c.close()
                c = self.open(path)
                c.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
                restored = uid(910+number)
                c.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='original',
                      expected_generation=1, revision=2, expected_session_id=None, expected_tick=None,
                      expected_gameplay_revision=None, new_session_id=restored))
                self.assertEqual(c.rpc('runtime.lighting', dict(session_id=restored)), dict(frozen, session_id=restored))
                c.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=1, slot='probe',
                      expected_generation=0, session_id=restored, expected_tick=0, expected_gameplay_revision=0))
                probe = next((saves/'slot-probe').glob('*.bin')).read_bytes()
                self.assertEqual(probe, original)
                self.assertEqual(json.loads(probe), full)  # Full archive, with no field exclusions.
                self.assertEqual(path.read_bytes(), authored)
                c.close()


if __name__ == '__main__':
    unittest.main(argv=['environment_ibl_contract'], verbosity=2)
