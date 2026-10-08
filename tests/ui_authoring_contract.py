#!/usr/bin/env python3
"""Typed authored UI through real transactions, inspection and optional native layout.

No graphics, provider, compiled gameplay or physical input is exercised here.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient

FIXTURE = ROOT / 'examples/ui/authored-hud.json'


def uid(number):
    return f'{number:032x}'


def definition():
    return json.loads(FIXTURE.read_text(encoding='utf-8'))


def operations(value=None):
    value = definition() if value is None else value
    # Reverse dependency order deliberately exercises final-tree validation.
    return [dict(op='ui.element.set', id=identity, element=element)
            for identity, element in reversed(list(value.items()))]


def compact(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')


def frozen_hash(document):
    value = copy.deepcopy(document)
    for key in ('receipts', 'retired_ids', 'retired_component_schemas', 'retired_template_ids', 'retired_ui_ids'):
        value.pop(key, None)
    return hashlib.sha256(compact(dict(format='poima.runtime-content', version=1, document=value, assets=[]))).hexdigest()


def expected_controls(width, height, scale=1):
    menu_x, menu_y = (width - 320 * scale) / 2, (height - 200 * scale) / 2
    x, y = menu_x + 16 * scale, menu_y + 16 * scale
    hud_y = height - 24 * scale - 48 * scale
    return {
        uid(111): [x, y, x + 288 * scale, y + 32 * scale],
        uid(113): [x, y + 40 * scale, x + 288 * scale, y + 80 * scale],
        uid(112): [x, y + 88 * scale, x + 288 * scale, y + 128 * scale],
        uid(201): [36 * scale, hud_y + 12 * scale, 132 * scale, hud_y + 36 * scale],
        uid(202): [156 * scale, hud_y + 12 * scale, 252 * scale, hud_y + 36 * scale],
    }


class AuthoringContract(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build/ui-authoring-contract'
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix='ui-', dir=scratch)
        self.addCleanup(self.directory.cleanup)
        self.world = Path(self.directory.name) / 'world.json'
        self.clients = []
        self.addCleanup(self.close_clients)
        self.open()

    def native(self, path):
        value = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=5).strip() if ARGS.windows_interop else value

    def open(self):
        self.client = WorldClient.open(str(ARGS.engine.resolve()), self.native(self.world), close_timeout=3)
        self.clients.append(self.client)

    def close_clients(self):
        for client in self.clients:
            client.close()
            self.assertEqual(client.transport.returncode, 0, client.transport.stderr_tail)

    def call(self, method, **params):
        return self.client.call(method, params, timeout=12)

    def reject(self, method, code=-32602, **params):
        with self.assertRaises(RpcError) as caught:
            self.call(method, **params)
        self.assertEqual(caught.exception.code, code)

    def tx(self, revision, ops=None, **extra):
        return self.client.transact(operations() if ops is None else ops, revision, timeout=12, **extra)

    def authored(self):
        return {row['id']: row['element'] for row in self.call('world.ui.list', limit=256)['elements']}

    def test_preview_retry_history_and_reopen_retain_exact_typed_definition(self):
        initial, history = self.client.inspect(), self.client.history()
        preview = self.tx(0, preview=True)
        self.assertFalse(preview['committed'])
        self.assertEqual(self.client.inspect(), initial)
        self.assertEqual(self.client.history(), history)
        self.assertFalse(self.world.exists())
        request = uuid.uuid4().hex
        accepted = self.tx(0, request_id=request)
        self.assertEqual(self.authored(), definition())
        before = self.world.read_bytes()
        self.assertEqual(self.tx(0, request_id=request), dict(accepted, replayed=True))
        self.assertEqual(self.world.read_bytes(), before)
        self.client.undo(1, timeout=12)
        self.assertEqual(self.authored(), {})
        self.client.redo(2, timeout=12)
        self.assertEqual(self.authored(), definition())
        self.client.close()
        self.assertEqual(self.client.transport.returncode, 0)
        self.open()
        self.assertEqual(self.authored(), definition())
        self.assertEqual(self.tx(0, request_id=request), dict(accepted, replayed=True))
        self.assertEqual(json.loads(self.world.read_bytes())['ui'], definition())

    def test_focused_discovery_exposes_typed_authoring_without_losing_transaction_guards(self):
        state, history = self.client.inspect(), self.client.history()
        full = self.call('world.describe', view='method', name='world.transact')['methods']['world.transact']
        focused = self.client.discover_mutation('ui.element.set', timeout=12)
        self.assertEqual(focused['selection'], {'operation': 'ui.element.set'})
        self.assertEqual(list(focused['methods']), ['world.transact'])
        branches = full['properties']['ops']['items']['oneOf']
        selected = [branch for branch in branches if branch.get('properties', {}).get('op', {}).get('const') == 'ui.element.set']
        self.assertEqual(len(selected), 1)
        expected = copy.deepcopy(full)
        expected['properties']['ops']['items']['oneOf'] = selected
        self.assertEqual(focused['methods']['world.transact'], expected)
        declared = self.call('world.describe', view='section', name='ui')['sections']['ui']['element']
        self.assertEqual(selected[0]['properties']['element'], declared)
        variants = {branch['properties']['kind']['const']: branch for branch in declared['oneOf']}
        self.assertEqual(set(variants), {'panel', 'label', 'button'})
        required = {'parent', 'name', 'kind', 'text', 'action', 'visible', 'enabled'}
        for kind, branch in variants.items():
            self.assertEqual(set(branch['required']), required)
            self.assertIs(branch['additionalProperties'], False)
            self.assertIn('style', branch['properties'])
            layout = branch['properties']['layout']
            self.assertIs(layout['additionalProperties'], False)
            self.assertEqual(layout['minProperties'], 1)
            for key in ('width', 'height', 'left', 'top', 'order', 'padding'):
                self.assertIn(key, layout['properties'])
            self.assertEqual('direction' in layout['properties'], kind == 'panel')
            self.assertEqual('hit_test' in layout['properties'], kind == 'panel')
        capture = self.call('world.describe', view='method', name='runtime.capture')['methods']['runtime.capture']
        density = capture['properties']['ui_scale']
        self.assertEqual({key: density[key] for key in ('type', 'minimum', 'maximum')},
                         {'type': 'number', 'minimum': .25, 'maximum': 8})
        self.assertNotIn('ui_scale', capture.get('required', []))
        authored_capture = self.call('world.describe', view='method', name='world.capture')['methods']['world.capture']
        self.assertNotIn('ui_scale', authored_capture['properties'])
        self.assertEqual(self.client.inspect(), state)
        self.assertEqual(self.client.history(), history)
        self.assertFalse(self.world.exists())

    def test_invalid_typed_properties_reject_late_batch_atomically(self):
        self.tx(0)
        saved, history, state = self.world.read_bytes(), self.client.history(), self.client.inspect()
        base = definition()[uid(113)]
        invalid = []
        for key, value in (
            ('layout', {}), ('style', {}), ('layout', {'width': {'unit': 'px', 'value': 1}}),
            ('layout', {'width': {'unit': 'dp', 'value': -1}}),
            ('layout', {'width': {'unit': 'percent', 'value': 101}}),
            ('layout', {'width': {'unit': 'dp', 'value': True}}),
            ('layout', {'width': {'unit': 'dp', 'value': 8193}}),
            ('layout', {'left': {'unit': 'dp', 'value': 1}}),
            ('layout', {'order': 1.5}), ('layout', {'order': 1025}),
            ('layout', {'direction': 'row'}), ('layout', {'padding': [0, 0, 0]}),
            ('layout', {'grow': 17}), ('layout', {'unknown': 1}),
            ('style', {'background_color': '#ABCDEF'}), ('style', {'color': 'red'}),
            ('style', {'font_size': 7}), ('style', {'font_size': 129}),
            ('style', {'border_width': 33}), ('style', {'hover': {}}),
            ('style', {'hover': {'font_size': 20}}), ('style', {'unknown': 1}),
        ):
            element = copy.deepcopy(base)
            element[key] = value
            invalid.append(element)
        panel = copy.deepcopy(definition()[uid(110)])
        panel['layout']['overflow'] = 'hidden'
        panel['style']['border_radius'] = 8
        invalid.append(panel)
        panel = copy.deepcopy(definition()[uid(110)])
        panel['layout'].update(min_width={'unit': 'dp', 'value': 200}, max_width={'unit': 'dp', 'value': 100})
        invalid.append(panel)
        first = copy.deepcopy(definition()[uid(201)])
        first['text'] = 'Must not publish'
        for element in invalid:
            with self.subTest(element=element):
                identity = uid(110) if element['kind'] == 'panel' else uid(113)
                self.reject('world.transact', request_id=uuid.uuid4().hex, base_revision=1,
                            ops=[dict(op='ui.element.set', id=uid(201), element=first),
                                 dict(op='ui.element.set', id=identity, element=element)])
                self.assertEqual(self.world.read_bytes(), saved)
                self.assertEqual(self.client.history(), history)
                self.assertEqual(self.client.inspect(), state)

    def test_legacy_seven_field_elements_keep_absent_metadata_and_canonical_bytes(self):
        legacy = dict(parent=None, name='Legacy', kind='label', text='Unstyled', action=None, visible=True, enabled=True)
        self.tx(0, [dict(op='ui.element.set', id=uid(1), element=legacy)])
        self.assertEqual(self.call('world.ui.get', id=uid(1))['element'], legacy)
        source = self.world.read_bytes()
        document = json.loads(source)
        self.assertEqual(document['ui'], {uid(1): legacy})
        digest = frozen_hash(document)
        self.client.close()
        self.open()
        self.assertEqual(self.call('world.ui.get', id=uid(1))['element'], legacy)
        self.assertEqual(self.world.read_bytes(), source)
        self.assertEqual(frozen_hash(json.loads(self.world.read_bytes())), digest)

    def test_layout_availability_and_invalid_queries_are_observational(self):
        self.tx(0)
        descriptor = self.call('world.describe', view='section', name='ui')['sections']['ui']
        self.assertEqual(descriptor['presentation_available'], bool(ARGS.layout_available))
        saved, state, history = self.world.read_bytes(), self.client.inspect(), self.client.history()
        if not ARGS.layout_available:
            self.reject('world.ui.layout', -32003, revision=1, width=960, height=540, scale=1)
        else:
            for extra in ({'width': 0}, {'height': 8193}, {'width': True}, {'height': 1.5},
                          {'scale': 0}, {'scale': 8.01}, {'scale': True}, {'unknown': 1}):
                params = dict(revision=1, width=960, height=540, scale=1)
                params.update(extra)
                self.reject('world.ui.layout', **params)
            self.reject('world.ui.layout', revision=1, width=960)
            self.reject('world.ui.layout', -32009, revision=0, width=960, height=540)
        self.assertEqual(self.world.read_bytes(), saved)
        self.assertEqual(self.client.inspect(), state)
        self.assertEqual(self.client.history(), history)

    def test_responsive_layout_has_authored_order_bounds_and_density(self):
        if not ARGS.layout_available:
            self.skipTest('Native layout backend disabled')
        self.tx(0)
        saved, history = self.world.read_bytes(), self.client.history()
        for width, height, scale in ((960, 540, 1), (1280, 720, 1), (1024, 768, 1), (1280, 720, 2)):
            with self.subTest(width=width, height=height, scale=scale):
                result = self.call('world.ui.layout', revision=1, width=width, height=height, scale=scale)
                self.assertEqual({key: result[key] for key in ('revision', 'width', 'height', 'scale')},
                                 dict(revision=1, width=width, height=height, scale=scale))
                rows = {row['id']: row for row in result['controls']}
                self.assertEqual(set(rows), set(expected_controls(width, height, scale)))
                for identity, expected in expected_controls(width, height, scale).items():
                    row = rows[identity]
                    self.assertEqual(set(row), {'id', 'kind', 'bounds', 'clip', 'hittable', 'visible', 'enabled', 'focused'})
                    self.assertEqual(len(row['bounds']), 4)
                    for actual, target in zip(row['bounds'], expected):
                        self.assertAlmostEqual(actual, target, delta=.05)
                    self.assertEqual(row['clip'], [0, 0, width, height])
                    self.assertIs(row['visible'], True)
                    self.assertIs(row['enabled'], True)
                    self.assertIs(row['focused'], False)
                    self.assertEqual(row['kind'], 'button' if identity in (uid(112), uid(113)) else 'label')
                    self.assertEqual(row['hittable'], identity in (uid(112), uid(113)))
                self.assertLess(rows[uid(113)]['bounds'][1], rows[uid(112)]['bounds'][1], 'Authored order must override stable ID order.')
        self.assertEqual(self.world.read_bytes(), saved)
        self.assertEqual(self.client.history(), history)

    def save_checkpoint(self, definition_value):
        self.tx(0, operations(definition_value))
        self.session = uuid.uuid4().hex
        self.call('runtime.start', session_id=self.session, revision=1)
        frozen = self.call('runtime.ui.inspect', session_id=self.session, tick=0)
        self.assertEqual({row['id'] for row in frozen['elements']}, set(definition_value))
        for row in frozen['elements']:
            for key in ('layout', 'style'):
                if key in definition_value[row['id']]:
                    self.assertEqual(row[key], definition_value[row['id']][key])
                else:
                    self.assertNotIn(key, row)
        self.saves = Path(self.directory.name) / 'saves'
        self.saves.mkdir()
        self.configuration = self.call('save.configure', request_id=uuid.uuid4().hex, expected_generation=0, root=self.native(self.saves))['generation']
        self.call('save.write', request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
                  slot='ui', expected_generation=0, session_id=self.session, expected_tick=0,
                  expected_gameplay_revision=0, expected_ui_revision=0, expected_control_sequence=0)
        self.slot = self.saves / 'slot-ui'
        manifest = json.loads((self.slot / 'current.json').read_bytes())
        payload_path = self.slot / manifest['payload']['current']['file']
        checkpoint = json.loads(payload_path.read_bytes())
        self.assertEqual(checkpoint['document']['ui'], definition_value)
        self.assertEqual(checkpoint['snapshot']['payload']['content_sha256'], frozen_hash(checkpoint['document']))
        return manifest, payload_path, checkpoint

    def load_params(self):
        return dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration, slot='ui',
                    expected_generation=1, revision=1, expected_session_id=self.session, expected_tick=0,
                    expected_gameplay_revision=0, expected_ui_revision=0, expected_control_sequence=0,
                    new_session_id=uuid.uuid4().hex)

    def test_legacy_save_restore_uses_unchanged_frozen_definition_hash(self):
        if not ARGS.runtime_available:
            self.skipTest('Simulation unavailable')
        legacy = {uid(1): dict(parent=None, name='Legacy', kind='label', text='Unstyled', action=None, visible=True, enabled=True)}
        _, _, checkpoint = self.save_checkpoint(legacy)
        source = self.world.read_bytes()
        params = self.load_params()
        self.call('save.load', **params)
        self.session = params['new_session_id']
        inspected = self.call('runtime.ui.inspect', session_id=self.session, tick=0)
        self.assertEqual(inspected['elements'][0]['text'], 'Unstyled')
        self.assertNotIn('layout', inspected['elements'][0])
        self.assertNotIn('style', inspected['elements'][0])
        self.assertEqual(self.world.read_bytes(), source)
        self.assertEqual(checkpoint['document']['ui'], legacy)

    def test_rechecksummed_save_with_changed_style_rejects_content_mismatch_atomically(self):
        if not ARGS.runtime_available:
            self.skipTest('Simulation unavailable')
        manifest, payload_path, checkpoint = self.save_checkpoint(definition())
        original = self.call('runtime.ui.inspect', session_id=self.session, tick=0)
        source = self.world.read_bytes()
        checkpoint['document']['ui'][uid(113)]['style']['background_color'] = '#112233'
        # Change only the frozen authored style. Keep the old snapshot identity;
        # repair outer storage checksums so native content binding is exercised.
        encoded = compact(checkpoint)
        payload_path.write_bytes(encoded)
        sha = hashlib.sha256(encoded).hexdigest()
        manifest['payload']['current'].update(bytes=len(encoded), sha256=sha)
        manifest['payload']['receipts'][-1].update(bytes=len(encoded), sha256=sha)
        manifest['sha256'] = hashlib.sha256(compact(manifest['payload'])).hexdigest()
        (self.slot / 'current.json').write_bytes(compact(manifest))
        status = self.call('save.inspect', slot='ui')
        self.assertTrue(status['current']['verified'])
        self.assertFalse(status['recovered'])
        self.reject('save.load', -32070, **self.load_params())
        self.assertEqual(self.call('runtime.ui.inspect', session_id=self.session, tick=0), original)
        self.assertEqual(self.world.read_bytes(), source)

    def test_runtime_metadata_is_frozen_and_not_a_live_style_setter(self):
        if not ARGS.runtime_available:
            self.skipTest('Simulation unavailable')
        self.tx(0)
        session = uuid.uuid4().hex
        self.call('runtime.start', session_id=session, revision=1)
        original = self.call('runtime.ui.inspect', session_id=session, tick=0)
        rows = {row['id']: row for row in original['elements']}
        for identity, element in definition().items():
            self.assertEqual(rows[identity]['layout'], element['layout'])
            self.assertEqual(rows[identity]['style'], element['style'])
        changed = copy.deepcopy(definition()[uid(113)])
        changed['layout']['width']['value'] = 75
        changed['style']['background_color'] = '#112233'
        self.tx(1, [dict(op='ui.element.set', id=uid(113), element=changed)])
        self.assertEqual(self.call('world.ui.get', id=uid(113))['element'], changed)
        self.assertEqual(self.call('runtime.ui.inspect', session_id=session, tick=0), original)
        self.assertTrue(self.call('runtime.inspect', session_id=session)['source_stale'])
        for key in ('layout', 'style'):
            self.reject('runtime.ui.edit', session_id=session, request_id=uuid.uuid4().hex,
                        expected_tick=0, expected_ui_revision=0,
                        edits=[dict(id=uid(113), **{key: changed[key]})])
            self.assertEqual(self.call('runtime.ui.inspect', session_id=session, tick=0), original)
        self.call('runtime.stop', session_id=session)
        session = uuid.uuid4().hex
        self.call('runtime.start', session_id=session, revision=2)
        current = {row['id']: row for row in self.call('runtime.ui.inspect', session_id=session, tick=0)['elements']}
        self.assertEqual(current[uid(113)]['layout'], changed['layout'])
        self.assertEqual(current[uid(113)]['style'], changed['style'])


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--layout-available', '--layoutavailable', type=int, choices=(0, 1), default=0)
    parser.add_argument('--runtime-available', type=int, choices=(0, 1), default=0)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + remaining)


if __name__ == '__main__':
    main()
