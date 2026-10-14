#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""File-only regressions for Switchback evidence admission and public content.

No engine, SDK, provider, Windows process or GPU is started. Synthetic native
receipts below test contradictory observations, not native import or rendering.
"""
import copy
import hashlib
import json
import queue
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
import switchback_oracle as oracle
from switchback_game_service import require_terminal_eof

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'docs/evidence/fixtures/agent-switchback'
SOURCE_SHA = '3b89178dac23d04316cf9735b047746ac354910cacfe231679185179d183cbcc'

class TerminalWireContract(unittest.TestCase):
    def test_final_unsolicited_or_missing_frames_are_rejected(self):
        frames = queue.Queue()
        frames.put(None)
        require_terminal_eof(frames)
        for values in ([], [{'jsonrpc': '2.0', 'id': 99, 'result': {}}],
                       [None, None], [None, {'id': 99}], [{'id': 99}, None]):
            frames = queue.Queue()
            for value in values:
                frames.put(value)
            with self.subTest(values=values), self.assertRaises(AssertionError):
                require_terminal_eof(frames)


def load(path):
    return oracle.strict_json(Path(path).read_bytes())


class MetadataContract(unittest.TestCase):
    def setUp(self):
        self.metadata = load(FIXTURE / 'exercise.json')
        self.directory = tempfile.TemporaryDirectory(prefix='poima-switchback-contract-')
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'exercise.json'

    def read(self, value):
        self.path.write_text(json.dumps(value, allow_nan=False), encoding='utf-8')
        return oracle.read_handoff(self.path)

    def test_public_metadata_is_admitted(self):
        self.assertEqual(self.read(self.metadata), self.metadata)

    def test_missing_extra_and_conflicting_roles_are_rejected(self):
        mutations = []
        value = copy.deepcopy(self.metadata); del value['ui']['retry']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['extra'] = True; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['roles']['camera'] = value['roles']['player']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['roles']['region_b'] = value['roles']['target_b']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['roles']['region_a'] = value['roles']['player']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['roles']['completion_emitter'] = value['roles']['pickup_hit_emitter']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['ui']['save'] = value['ui']['load']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['fields']['Ammo'] = value['fields']['Health']; mutations.append(value)
        value = copy.deepcopy(self.metadata); value['presentation']['save_status_field'] = value['fields']['Phase']; mutations.append(value)
        for index, value in enumerate(mutations):
            with self.subTest(index=index), self.assertRaises(AssertionError):
                self.read(value)

    def test_imported_index_bool_float_string_and_bounds_are_rejected(self):
        for key in ('node', 'skin', 'idle_clip', 'run_clip'):
            for bad in (True, False, 0.0, '0', -1, 2**32 - 1):
                value = copy.deepcopy(self.metadata); value['b_binding'][key] = bad
                with self.subTest(key=key, bad=bad), self.assertRaises(AssertionError):
                    self.read(value)
        for bad in (True, 3.0, '3'):
            value = copy.deepcopy(self.metadata); value['presentation']['save_status_complete'] = bad
            with self.subTest(presentation=bad), self.assertRaises(AssertionError):
                self.read(value)

    def test_imported_path_and_clip_contradictions_are_rejected(self):
        for changes in (
                {'asset': 'wrong'}, {'node_name': ''}, {'node_path': []},
                {'node_path': [0, 29, 29]}, {'node_path': [0, 24]},
                {'node_path': [0, True, 29]}, {'node_path': [0, 1.0, 29]},
                {'rig_node': '1'*32}, {'run_clip': self.metadata['b_binding']['idle_clip']}):
            value = copy.deepcopy(self.metadata); value['b_binding'].update(changes)
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                self.read(value)

    def test_duplicate_nonfinite_and_excessive_depth_json_is_rejected(self):
        for data in (b'{"phase":0,"phase":1}', b'{"x":NaN}', b'{"x":Infinity}',
                     b'{"x":1e9999}', b'['*66+b'0'+b']'*66):
            with self.subTest(data=data[:40]), self.assertRaises(AssertionError):
                oracle.strict_json(data)

    def test_native_numbers_and_golden_semantics_do_not_accept_bools(self):
        for bad in (True, False, '1', None, float('inf'), float('nan')):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                oracle.number(bad)
        golden = oracle.Golden()
        fields = self.metadata['fields']
        values = {fields[name]: getattr(golden, name) for name in oracle.SEMANTICS}
        world, foreground = golden.lenses()
        lenses = {'world': {'vertical_fov': world, 'near': .1, 'far': 80},
                  'viewmodel': {'vertical_fov': foreground}}
        golden.assert_observed(values, lenses, fields)
        for bad in (False, 0.0):
            changed = dict(values); changed[fields['Ammo']] = bad
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                golden.assert_observed(changed, lenses, fields)
        for bad in (True, 1.0, -1, 13):
            with self.subTest(elapsed=bad), self.assertRaises(AssertionError):
                oracle.perspective_curve(70, 35, bad)


class CheckpointContract(unittest.TestCase):
    @staticmethod
    def checkpoint():
        return {'snapshot': {'version': 9, 'gameplay': {'Ammo': 1, 'Alive': True,
                 'Weight': 1.0, 'Epoch': '9223372036854775807'},
                 'controllers': [{'position': [0.0, .1, 5.0], 'velocity': [0, 0, 0]}]},
                'document': {'world_id': 'a'*32, 'revision': 5,
                 'entities': {'b'*32: {'components': {'Transform': {'position': [0, 0, 0]}}}}}}

    def test_object_key_order_is_immaterial_but_numeric_types_are_not(self):
        a = self.checkpoint(); b = copy.deepcopy(a)
        b['snapshot']['gameplay'] = dict(reversed(list(b['snapshot']['gameplay'].items())))
        self.assertTrue(oracle.exact_checkpoint(a, b))
        for field, bad in (('Ammo', True), ('Ammo', 1.0), ('Alive', 1), ('Weight', 1),
                           ('Epoch', 9223372036854775807)):
            changed = copy.deepcopy(a); changed['snapshot']['gameplay'][field] = bad
            with self.subTest(field=field, bad=bad):
                self.assertFalse(oracle.exact_checkpoint(a, changed))

    def test_complete_document_and_all_snapshot_members_are_compared(self):
        a = self.checkpoint()
        for section in ('snapshot', 'document'):
            changed = copy.deepcopy(a); changed[section]['unexpected'] = 0
            with self.subTest(section=section):
                self.assertFalse(oracle.exact_checkpoint(a, changed))
        changed = copy.deepcopy(a); changed['snapshot']['controllers'][0]['velocity'][2] = 1
        self.assertFalse(oracle.exact_checkpoint(a, changed))
        changed = copy.deepcopy(a)
        changed['document']['entities']['b'*32]['components']['Transform']['position'][1] = 1
        self.assertFalse(oracle.exact_checkpoint(a, changed))
        changed = copy.deepcopy(a); del changed['snapshot']['gameplay']['Epoch']
        self.assertFalse(oracle.exact_checkpoint(a, changed))

    def test_all_nine_checkpoints_and_each_declared_pair_are_required(self):
        rows = {label: self.checkpoint() for label in oracle.CHECKPOINT_LABELS}
        oracle.assert_nine_checkpoints(rows)
        self.assertEqual(len(oracle.CHECKPOINT_LABELS), 9)
        self.assertEqual(len(oracle.EXACT_PAIRS), 5)
        missing = copy.deepcopy(rows); del missing[oracle.CHECKPOINT_LABELS[-1]]
        with self.assertRaises(AssertionError): oracle.assert_nine_checkpoints(missing)
        for left, right in oracle.EXACT_PAIRS:
            changed = copy.deepcopy(rows)
            changed[oracle.CHECKPOINT_LABELS[right]]['document']['revision'] += 1
            with self.subTest(pair=(left, right)), self.assertRaises(AssertionError):
                oracle.assert_nine_checkpoints(changed)


class AuthoredReceiptStub:
    """Synthetic native-shaped observations, independent of mutated declarations.

    The public cooked asset identity is checked separately. These selected rows
    are a small receipt fixture, not an implementation of the model decoder.
    """
    def __init__(self):
        self.handoff = load(FIXTURE / 'exercise.json')
        self.world = load(FIXTURE / 'world.json')
        self.asset = self.handoff['b_binding']['asset']
        nodes = [(0, 'FBX Scene', -1), (1, 'Root', 0), (5, 'HipsCtrl', 1),
                 (12, 'Hips', 5), (19, 'Spine', 12), (24, 'Chest', 19), (29, 'UpperChest', 24)]
        self.tables = {'nodes': {i: {'index': i, 'name': name, 'parent': parent} for i, name, parent in nodes},
                       'skins': {0: {'index': 0, 'joints': 45}},
                       'animations': {0: {'index': 0, 'name': 'Idle', 'duration': 1.0666, 'channels': 46},
                                      1: {'index': 1, 'name': 'Run', 'duration': .6666, 'channels': 46}},
                       'primitives': {0: {'index': 0, 'skinned': True, 'vertices': 1029, 'triangles': 1604}}}
        self.tables['nodes'][2] = {'index': 2, 'name': 'characterMedium', 'parent': 0, 'skin': 0, 'primitives': [0]}
        self.meshes = [key for key, value in self.world['entities'].items() if 'SkinnedMesh' in value['components']]
        self.foreign_asset = False

    def rpc(self, method, **params):
        revision = self.world['revision']
        if method == 'world.inspect': return {'revision': revision}
        if method == 'entity.get':
            return {'revision': revision, 'id': params['id'], 'value': copy.deepcopy(self.world['entities'][params['id']])}
        if method == 'asset.inspect':
            return {'asset': '0'*64 if self.foreign_asset else self.asset,
                    'items': [copy.deepcopy(self.tables[params['section']][params['offset']])]}
        if method == 'entity.query':
            return {'revision': revision, 'entities': [{'id': key} for key in self.meshes], 'next_after': None}
        raise AssertionError('Unexpected receipt-stub operation: ' + method)


class ImportedBindingContract(unittest.TestCase):
    def test_consistent_public_authored_and_imported_receipts_join(self):
        result = oracle.inspect_import_binding(AuthoredReceiptStub())
        self.assertEqual(result['ancestry'][-1]['name'], 'UpperChest')
        self.assertEqual(len(result['meshes']), 1)
        self.assertFalse(result['weight_topology_verified'])
        self.assertFalse(result['rendered_animation_verified'])

    def test_contradictory_authored_or_imported_receipts_are_rejected(self):
        def rig_parent(o): o.world['entities'][o.handoff['roles']['rig_b']]['parent'] = o.handoff['roles']['player']
        def rig_asset(o): o.world['entities'][o.handoff['roles']['rig_b']]['components']['AnimationRig']['asset'] = '0'*64
        def rig_node(o): o.world['entities'][o.handoff['roles']['region_b']]['components']['RigNode']['node'] = 24
        def region_owner(o): o.world['entities'][o.handoff['roles']['region_b']]['components']['HitRegion']['owner'] = o.handoff['roles']['target_a']
        def hidden_mesh(o): o.world['entities'][o.meshes[0]]['components']['SkinnedMesh']['visible'] = False
        def ancestry(o): o.tables['nodes'][24]['parent'] = 0
        def skin(o): o.tables['nodes'][2]['skin'] = 1
        def no_joints(o): o.tables['skins'][0]['joints'] = 0
        def clip(o): o.tables['animations'][1]['name'] = 'Other'
        def unskinned(o): o.tables['primitives'][0]['skinned'] = False
        def foreign_asset(o): o.foreign_asset = True
        def no_meshes(o): o.meshes = []
        for mutate in (rig_parent, rig_asset, rig_node, region_owner, hidden_mesh, ancestry,
                       skin, no_joints, clip, unskinned, foreign_asset, no_meshes):
            owner = AuthoredReceiptStub(); mutate(owner)
            with self.subTest(mutation=mutate.__name__), self.assertRaises(AssertionError):
                oracle.inspect_import_binding(owner)


class PublicFixtureContract(unittest.TestCase):
    def test_game_source_is_the_preserved_original_and_project_is_portable(self):
        self.assertEqual(hashlib.sha256((FIXTURE/'src/Range.cs').read_bytes()).hexdigest(), SOURCE_SHA)
        project = load(FIXTURE/'project.json'); world = load(FIXTURE/project['entry']['world'])
        metadata = oracle.read_handoff(FIXTURE/'exercise.json')
        self.assertEqual(project['version'], 1)
        self.assertNotIn('gameplay', project)
        self.assertEqual(project['entry']['controller'], metadata['roles']['player'])
        self.assertEqual(project['entry']['camera'], metadata['roles']['camera'])
        self.assertEqual(world['revision'], 5)
        self.assertEqual(len(world['entities']), 82)
        self.assertEqual(len(world['ui']), 11)
        self.assertEqual(world['receipts'], [])
        self.assertTrue(all(value in world['entities'] for value in metadata['roles'].values()))
        self.assertTrue(all(value in world['ui'] for value in metadata['ui'].values()))
        source_project = ET.parse(FIXTURE/'src/Switchback.csproj').getroot()
        self.assertEqual(source_project.find('./ItemGroup/Compile').attrib['Include'], 'Range.cs')
        reference = source_project.find('./ItemGroup/Reference')
        self.assertEqual(reference.attrib['Include'], 'Poima.Gameplay')
        self.assertEqual(reference.findtext('HintPath'), '../reference/Poima.Gameplay.dll')
        self.assertEqual(reference.findtext('Private'), 'true')

    def test_input_profile_retains_required_revision_one_receipt(self):
        profile = load(FIXTURE/'settings/range.poima-input.json')
        self.assertEqual(profile['format'], 'poima.input.v3')
        self.assertEqual(profile['revision'], 1)
        self.assertEqual(len(profile['receipts']), 1, 'Clearing receipts without resetting revision is invalid')
        receipt = profile['receipts'][0]
        self.assertEqual(receipt['params']['expected_revision'], 0)
        self.assertEqual(receipt['result']['previous_revision'], 0)
        self.assertEqual(receipt['result']['revision'], 1)
        self.assertEqual(receipt['params']['profile'], profile['profile'])
        encoded = json.dumps(profile['profile'], sort_keys=True, separators=(',', ':')).encode('utf-8')
        self.assertEqual(hashlib.sha256(encoded).hexdigest(), receipt['result']['content_hash'])
        self.assertEqual(profile['profile']['actions'], {'aim': ['mouse.3'], 'crouch': ['key.c'],
                                                       'fire': ['mouse.1'], 'reload': ['key.r']})

    def test_active_cooked_assets_have_exact_identity_and_original_notices(self):
        world = load(FIXTURE/'world.json'); selected = set()
        for entity in world['entities'].values():
            for name in ('SkinnedMesh', 'AnimationRig', 'AudioEmitter'):
                component = entity['components'].get(name)
                if component: selected.add(component['asset'])
        provenance = load(FIXTURE/'provenance.json')
        self.assertEqual(selected, {item['asset'] for item in provenance['content']})
        self.assertEqual(len(selected), 3)
        store = FIXTURE/'world.json.assets'
        self.assertEqual(len(list(store.iterdir())), 6)
        for path in store.iterdir():
            with self.subTest(file=path.name):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), path.stem)
        for item in provenance['content']:
            with self.subTest(content=item['title']):
                record = load(store/(item['provenance']+'.pprov'))
                self.assertEqual(record['asset'], item['asset'])
                self.assertEqual(record['license']['identifier'], item['license'])
                self.assertEqual(item['license'], 'CC0-1.0')
                self.assertEqual(record['license']['notice'], (FIXTURE/item['notice']).read_text(encoding='utf-8'))
                self.assertEqual(record['creator'], item['creator'])
                self.assertEqual(record['source'], item['source'])


if __name__ == '__main__':
    if sys.version_info < (3, 11):
        raise SystemExit('Switchback evidence contracts require Python 3.11 or newer')
    unittest.main()
