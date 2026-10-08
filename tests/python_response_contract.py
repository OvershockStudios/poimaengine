#!/usr/bin/env python3
"""Independent authoring-core response fixtures; no native processes or provider calls."""
# SPDX-License-Identifier: Apache-2.0
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import ResponseContractError
from poima_client.responses import response_contract, validate_core_result


def uid(number):
    return f'{number:032x}'


IDENTITY = {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}
MATRIX = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
META = dict(protocol_version=1, schema_revision=47, mode='authoring',
            read_only=False, runtime_available=True)
CORE_NAMES = {'world.describe', 'world.inspect', 'world.history', 'world.transact',
              'world.undo', 'world.redo', 'entity.get', 'entity.query', 'entity.world_transform'}


def row(number, parent=None, components=None):
    return dict(id=uid(number), name=f'Entity {number}', parent=parent,
                components=['Transform'] if components is None else components)


def mutation_params():
    return dict(base_revision=5, request_id=uid(99),
                ops=[dict(op='entity.rename', id=uid(1), name='Renamed')], preview=False)


def commit():
    return dict(revision=6, committed=True, replayed=False,
                changed_ids=[uid(1)], history_recorded=True)


def mutation_discovery(operation='entity.rename', component=None, branches=None):
    selection = dict(operation=operation)
    properties = dict(op={'const': operation})
    if component is not None:
        selection['type'] = component
        properties['type'] = {'const': component}
    if branches is None:
        branches = [dict(type='object', properties=properties)]
    envelope = dict(type='object', required=['request_id', 'base_revision', 'ops'],
                    properties=dict(request_id={'type': 'string'}, base_revision={'type': 'integer'},
                                    ops={'type': 'array', 'items': {'oneOf': branches}}))
    return dict(META, schema_revision=48, partial=True, view='mutation', selection=selection,
                methods={'world.transact': envelope})


def fixtures():
    """Literal public wire records, deliberately independent of schema generation."""
    whole = dict(name='Origin', parent=None, components=dict(Transform=copy.deepcopy(IDENTITY)))
    entries = [
        ('world.describe', {}, dict(META, methods={'world.inspect': {'type': 'object'}},
                                    components={'Transform': {'type': 'object'}})),
        ('world.inspect', {}, dict(world_id=uid(20), revision=5, entity_count=1,
                                   persisted=True, read_only=False, mode='authoring',
                                   coordinate_system='right-handed Y-up; meters; local XYZW quaternion transforms')),
        ('world.history', {}, dict(revision=5, undo_count=2, redo_count=1, bytes=128,
                                   max_entries=32, max_bytes=16777216, session_local=True,
                                   skipped_large_edits=0)),
        ('entity.get', dict(id=uid(1), revision=5), dict(id=uid(1), revision=5, value=whole)),
        ('entity.query', dict(revision=5, limit=2),
         dict(revision=5, entities=[row(1), row(2)], next_after=uid(2))),
        ('entity.world_transform', dict(id=uid(1), revision=5),
         dict(id=uid(1), revision=5, matrix=MATRIX.copy(), layout='column_major')),
        ('world.transact', mutation_params(), commit()),
    ]
    for method, action in (('world.undo', 'undo'), ('world.redo', 'redo')):
        entries.append((method, dict(base_revision=5, request_id=uid(99)),
                        dict(commit(), history_action=action)))
    return entries


class ResponseContract(unittest.TestCase):
    def accepts(self, method, result, params=None):
        self.assertIs(validate_core_result(method, result, params), result)

    def rejects(self, method, result, params=None):
        with self.assertRaises(ResponseContractError) as raised:
            validate_core_result(method, result, params)
        self.assertEqual(raised.exception.method, method)
        return raised.exception

    def test_all_nine_methods_and_additive_result_metadata(self):
        self.assertEqual({method for method, _, _ in fixtures()}, CORE_NAMES)
        for method, params, result in fixtures():
            with self.subTest(method=method):
                self.accepts(method, result, params)
                self.accepts(method, dict(result, future_metadata={'version': 2}), params)

    def test_required_top_level_fields_and_wrong_wire_types(self):
        for method, params, result in fixtures():
            for field, value in result.items():
                with self.subTest(method=method, field=field, kind='missing'):
                    changed = copy.deepcopy(result)
                    del changed[field]
                    self.rejects(method, changed, params)
                wrong = 0 if isinstance(value, bool) else 'wrong' if isinstance(value, (int, list, dict)) else 0
                with self.subTest(method=method, field=field, kind='type'):
                    self.rejects(method, dict(result, **{field: wrong}), params)

    def test_integer_counts_and_revisions_do_not_accept_bool_or_float(self):
        for method, params, result in fixtures():
            for field in ('revision', 'schema_revision', 'entity_count', 'undo_count', 'redo_count',
                          'bytes', 'max_entries', 'max_bytes', 'skipped_large_edits'):
                if field in result:
                    for bad in (True, False, float(result[field]), -1):
                        with self.subTest(method=method, field=field, value=bad):
                            self.rejects(method, dict(result, **{field: bad}), params)

    def test_read_only_shared_and_build_specific_discovery_metadata(self):
        for scope in (None, 'shared_editor', 'shared_headless'):
            for available in (False, True):
                result = dict(META, mode='read_only_runtime', read_only=True,
                              runtime_available=available, methods={'world.inspect': {'type': 'object'}},
                              components={'Transform': {'type': 'object'}},
                              unavailable_mutations=['world.transact', 'world.undo', 'world.redo'])
                if scope is not None:
                    result['session_scope'] = scope
                if scope == 'shared_editor':
                    result.update(editor_discovery='editor.describe', unavailable_methods=['world.capture'])
                with self.subTest(scope=scope, runtime_available=available):
                    self.accepts('world.describe', result)
                    self.rejects('world.describe', dict(result, read_only=False))
        inspect = fixtures()[1][2]
        self.accepts('world.inspect', dict(inspect, read_only=True, mode='read_only_runtime'))
        self.rejects('world.inspect', dict(inspect, read_only=True))

    def test_every_discovery_view_has_a_context_specific_shape(self):
        full = fixtures()[0][2]
        self.accepts('world.describe', full, dict(view='full'))
        catalog = dict(META, partial=True, view='catalog', methods=['entity.get', 'world.inspect'],
                       components=['Camera', 'Transform'], sections=['invariants', 'limits'])
        self.accepts('world.describe', catalog, dict(view='catalog'))
        for view, category, name, value in (
                ('method', 'methods', 'world.inspect', {'type': 'object'}),
                ('component', 'components', 'Transform', {'type': 'object'}),
                ('section', 'sections', 'invariants', ['Meters; right-handed Y-up'])):
            params = dict(view=view, name=name)
            result = dict(META, partial=True, view=view, **{category: {name: value}})
            with self.subTest(view=view):
                self.accepts('world.describe', result, params)
                self.rejects('world.describe', dict(result, view='catalog'), params)
                self.rejects('world.describe', dict(result, partial=False), params)
                self.rejects('world.describe', dict(result, **{category: {'other': value}}), params)
                self.rejects('world.describe', dict(result, **{category: {name: value, 'extra': value}}), params)
                if view != 'section':
                    self.rejects('world.describe', dict(result, **{category: {name: None}}), params)
        for category in ('methods', 'components'):
            self.rejects('world.describe', dict(full, **{category: {'broken': None}}))
        for category in ('methods', 'components', 'sections'):
            for bad in (['duplicate', 'duplicate'], ['z', 'a']):
                self.rejects('world.describe', dict(catalog, **{category: bad}), dict(view='catalog'))
        self.rejects('world.describe', catalog, dict(view='full'))

    def test_entity_components_and_guarded_identity(self):
        selected = dict(id=uid(1), revision=5, value=copy.deepcopy(IDENTITY))
        params = dict(id=uid(1), revision=5, component='Transform')
        self.accepts('entity.get', selected, params)
        for field in ('position', 'rotation', 'scale'):
            missing = copy.deepcopy(selected)
            del missing['value'][field]
            self.rejects('entity.get', missing, params)
            invalid = copy.deepcopy(selected)
            invalid['value'][field] = [False] * len(IDENTITY[field])
            self.rejects('entity.get', invalid, params)
        whole = copy.deepcopy(fixtures()[3][2])
        del whole['value']['components']['Transform']
        self.rejects('entity.get', whole, dict(id=uid(1)))
        camera = dict(id=uid(1), revision=5, value={'vertical_fov': 60, 'near': .05, 'far': 1000})
        self.accepts('entity.get', camera, dict(id=uid(1), component='Camera'))
        self.rejects('entity.get', dict(camera, value=None), dict(id=uid(1), component='Camera'))
        for method, params, result in (fixtures()[3], fixtures()[5]):
            self.rejects(method, dict(result, id=uid(2)), params)
            self.rejects(method, dict(result, revision=4), params)
            self.rejects(method, dict(result, id='ABC'), params)

    def test_mutation_discovery_preserves_enum_and_conservative_union_branches(self):
        params = dict(view='mutation', operation='component.remove', type='Camera')
        branches = [dict(type='object', properties=dict(op={'const': 'component.remove'},
                                                       type={'enum': ['Camera', 'Light']})),
                    {}, {'$ref': '#/future', 'properties': {'op': {'const': 'future.operation'}}}]
        result = mutation_discovery('component.remove', 'Camera', branches)
        self.accepts('world.describe', result, params)
        self.assertEqual(result['methods']['world.transact']['properties']['ops']['items']['oneOf'], branches)
        custom = 'game:' + uid(70)
        branch = dict(type='object', properties=dict(op={'enum': ['component.set', 'component.remove']},
                                                    type={'pattern': '^game:[0-9a-f]{32}$'}))
        self.accepts('world.describe', mutation_discovery('component.set', custom, [branch]),
                     dict(view='mutation', operation='component.set', type=custom))
        self.accepts('world.describe', mutation_discovery('component.set', branches=[branch]),
                     dict(view='mutation', operation='component.set'))

    def test_mutation_discovery_rejects_wrong_selection_and_incomplete_envelopes(self):
        params = dict(view='mutation', operation='entity.rename')
        result = mutation_discovery()
        malformed = [dict(result, partial=False), dict(result, view='method'),
                     dict(result, selection={'operation': 'entity.create'}),
                     dict(result, selection={'operation': 'entity.rename', 'type': 'Camera'}),
                     dict(result, methods={}), dict(result, methods={'other': {'type': 'object'}}),
                     dict(result, methods=dict(result['methods'], extra={'type': 'object'})),
                     dict(result, methods={'world.transact': {}})]
        for changed in malformed:
            self.rejects('world.describe', changed, params)
        for path, replacement in ((('required',), ['ops', 'other', 'third']),
                                  (('properties', 'ops', 'items', 'oneOf'), []),
                                  (('properties', 'ops', 'items', 'oneOf'), [False]),
                                  (('properties', 'ops', 'items'), {}),
                                  (('properties', 'request_id'), {})):
            changed = copy.deepcopy(result)
            target = changed['methods']['world.transact']
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = replacement
            self.rejects('world.describe', changed, params)
        unsupported = mutation_discovery(branches=[dict(properties=dict(op={'const': 'entity.create'}))])
        self.rejects('world.describe', unsupported, params)
        typed = mutation_discovery('component.set', 'Transform')
        self.rejects('world.describe', typed, dict(view='mutation', operation='component.set', type='Camera'))
        typed['methods']['world.transact']['properties']['ops']['items']['oneOf'][0]['properties']['type'] = {'const': 'Camera'}
        self.rejects('world.describe', typed, dict(view='mutation', operation='component.set', type='Transform'))

    def test_mutation_discovery_scope_and_selector_context_are_consistent(self):
        result = mutation_discovery()
        params = dict(view='mutation', operation='entity.rename')
        for scope in ('shared_editor', 'shared_headless'):
            self.accepts('world.describe', dict(result, session_scope=scope, runtime_available=False), params)
        self.rejects('world.describe', dict(result, mode='read_only_runtime', read_only=True), params)
        self.rejects('world.describe', dict(result, read_only=True), params)
        for unavailable in ('unavailable_methods', 'unavailable_mutations'):
            self.rejects('world.describe', dict(result, **{unavailable: ['world.transact']}), params)
        for invalid in ({'view': 'mutation'}, dict(params, name='world.transact'),
                        dict(params, operation=True), dict(params, operation=''),
                        dict(params, operation='é'*65), dict(params, type='Camera'),
                        dict(params, type=None)):
            self.rejects('world.describe', result, invalid)
        for view in ('full', 'catalog', 'method', 'component', 'section'):
            invalid = dict(view=view, operation='entity.rename')
            if view in ('method', 'component', 'section'):
                invalid['name'] = 'world.transact'
            self.rejects('world.describe', result, invalid)

    def test_matrix_and_transform_numbers_are_finite_and_correctly_sized(self):
        _, params, matrix = fixtures()[5]
        for bad in (MATRIX[:-1], MATRIX + [0], [float('nan')] + MATRIX[1:],
                    [float('inf')] + MATRIX[1:], [True] + MATRIX[1:]):
            self.rejects('entity.world_transform', dict(matrix, matrix=bad), params)
        selected = dict(id=uid(1), revision=5, value=copy.deepcopy(IDENTITY))
        params = dict(id=uid(1), component='Transform')
        for field, value in (('position', [float('nan'), 0, 0]), ('rotation', [0, 0, 1]),
                             ('scale', [1, 0, 1]), ('scale', [1, -1, 1])):
            self.rejects('entity.get', dict(selected, value=dict(IDENTITY, **{field: value})), params)

    def test_history_budget_and_locality(self):
        result = fixtures()[2][2]
        self.rejects('world.history', dict(result, undo_count=32, redo_count=1))
        self.rejects('world.history', dict(result, bytes=result['max_bytes']+1))
        self.rejects('world.history', dict(result, session_local=False))

    def test_query_limit_parent_component_and_snapshot_guards(self):
        page = dict(revision=5, entities=[row(1), row(2)], next_after=None)
        self.accepts('entity.query', page, dict(revision=5, limit=2, parent=None, component='Transform'))
        self.rejects('entity.query', page, dict(limit=1))
        self.rejects('entity.query', page, dict(revision=4, limit=2))
        self.rejects('entity.query', page, dict(limit=2, parent=uid(50)))
        self.rejects('entity.query', page, dict(limit=2, component='Camera'))
        child = dict(revision=5, entities=[row(1, parent=uid(50), components=['Camera', 'Transform'])],
                     next_after=None)
        self.accepts('entity.query', child, dict(parent=uid(50), component='Camera'))
        self.rejects('entity.query', child, dict(parent=None))
        # The omitted limit is the native default (64), not the maximum (256).
        long_page = dict(revision=5, entities=[row(n) for n in range(1, 66)], next_after=None)
        self.rejects('entity.query', long_page)

    def test_query_cursor_and_record_integrity(self):
        page = dict(revision=5, entities=[row(2), row(3)], next_after=uid(3))
        self.accepts('entity.query', page, dict(revision=5, after=uid(1), limit=2))
        self.rejects('entity.query', page, dict(revision=5, after=uid(2), limit=2))
        for rows, cursor in (([], uid(3)), ([row(2), row(3)], uid(2)),
                             ([row(3), row(2)], None), ([row(2), row(2)], None)):
            self.rejects('entity.query', dict(page, entities=rows, next_after=cursor), dict(limit=2))
        self.rejects('entity.query', dict(page, entities=[row(2)], next_after=uid(2)), dict(limit=2))
        for field in ('id', 'name', 'parent', 'components'):
            missing = row(2)
            del missing[field]
            self.rejects('entity.query', dict(page, entities=[missing], next_after=None))
        self.rejects('entity.query', dict(page, entities=[row(2, components=[False])], next_after=None))

    def test_preview_fresh_commit_and_legacy_replay_receipt(self):
        params = mutation_params()
        preview = dict(revision=6, committed=False, replayed=False, changed_ids=[uid(1)])
        self.accepts('world.transact', preview, dict(params, preview=True))
        self.rejects('world.transact', dict(preview, history_recorded=False), dict(params, preview=True))
        self.rejects('world.transact', dict(preview, replayed=True), dict(params, preview=True))
        fresh = commit()
        self.accepts('world.transact', fresh, params)
        fresh.pop('history_recorded')
        self.rejects('world.transact', fresh, params)
        self.accepts('world.transact', dict(fresh, replayed=True), params)
        self.accepts('world.transact', dict(commit(), replayed=True), params)
        self.accepts('world.transact', dict(commit(), history_recorded=False,
                                           history_reason='Edit exceeds history budget.'), params)

    def test_mutation_revision_uses_original_guard_including_historical_retry(self):
        for method, params, result in fixtures()[-3:]:
            for replayed in (False, True):
                with self.subTest(method=method, replayed=replayed):
                    value = dict(result, replayed=replayed)
                    self.accepts(method, value, params)
                    # No current-world revision is available here. The receipt
                    # still reports the original operation's base+1, even if
                    # the live world has advanced many times since that edit.
                    self.rejects(method, dict(value, revision=params['base_revision']), params)
                    self.rejects(method, dict(value, revision=params['base_revision']+2), params)
        preview = dict(revision=6, committed=False, replayed=False, changed_ids=[])
        self.rejects('world.transact', dict(preview, revision=0), dict(mutation_params(), preview=True))

    def test_undo_redo_literals_and_changed_id_order(self):
        for method, params, result in fixtures()[-3:]:
            self.rejects(method, dict(result, changed_ids=[uid(2), uid(1)]), params)
            self.rejects(method, dict(result, changed_ids=[uid(1), uid(1)]), params)
            self.rejects(method, dict(result, changed_ids=['invalid']), params)
            self.accepts(method, dict(result, changed_template_ids=[uid(5)], changed_ui_ids=[uid(8)]), params)
            if method in ('world.undo', 'world.redo'):
                self.rejects(method, dict(result, history_recorded=False), params)
                self.rejects(method, dict(result, history_action='other'), params)

    def test_validation_errors_preserve_detached_recovery_context(self):
        params = mutation_params()
        error = self.rejects('world.transact', dict(commit(), committed=False), params)
        self.assertEqual(error.request_id, params['request_id'])
        self.assertEqual(error.params, params)
        params['ops'][0]['name'] = 'Edited after failure'
        self.assertEqual(error.params['ops'][0]['name'], 'Renamed')
        self.assertIn('Renamed', error.params_json)
        self.assertNotIn('Edited after failure', error.params_json)

    def test_manifest_copy_and_experimental_passthrough(self):
        contract = response_contract()
        self.assertEqual(set(contract['methods']), CORE_NAMES)
        contract['methods'].clear()
        self.assertEqual(set(response_contract()['methods']), CORE_NAMES)
        result = {'experimental': ['retained', None]}
        self.accepts('renderer.future_feature', result, {'new_argument': True})


if __name__ == '__main__':
    unittest.main()
