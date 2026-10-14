#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable consumer mutations; these fixtures do not qualify native execution.

Run: python3 -B tests/python_workflow_responses.py
"""
import copy
import json
import math
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client.responses import ResponseContractError
from poima_client.workflow_responses import validate_workflow_result as validate
from poima_client.workflow_responses import workflow_response_contract


def uid(n):
    return f'{n:032x}'


SESSION, CURRENT, ENTITY, OTHER, TYPE, F1, F2, F3, F4, EPOCH = [uid(n) for n in range(1, 11)]
ZERO, HASH = uid(0), 'a' * 64
SCHEMA = dict(id=TYPE, name='Equipment', version=2, fingerprint=HASH, fields=[
    dict(id=F1, name='Count', kind='int32', default=0, unit=''),
    dict(id=F2, name='Exact', kind='int64', default='0', unit=''),
    dict(id=F3, name='Target', kind='entity', default=ZERO, unit=''),
    dict(id=F4, name='Parts', kind='array', element_kind='int32', capacity=3, default=[], unit='')])
VALUES = {F1: 3, F2: '9223372036854775807', F3: OTHER, F4: [1, 2]}
GAME_SCHEMA = dict(identity='example.portable', bytes=32, fields=[
    dict(name='Count', kind='int32', offset=0, bytes=4),
    dict(name='Exact', kind='int64', offset=8, bytes=8),
    dict(name='Target', kind='entity', offset=16, bytes=16)], components=[SCHEMA])
GAME_VALUES = dict(Count=2, Exact='-9223372036854775808', Target=OTHER)
MODULE = dict(backend='native_aot', native_library='game.dll', native_diagnostics={},
              assembly='game.dll', assembly_sha256=HASH, type='Example.Game',
              schema=GAME_SCHEMA, migration=None, values=GAME_VALUES)
HEADER = dict(session_id=SESSION, tick=9, component_revision=2, structure_revision=1)
SOURCE = dict(authored_revision=3, current_authored_revision=4, source_stale=True)
STATE = dict(world_matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
             layout='column_major', local_transform=dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1]),
             animation=None, motion='none', kinematic_target=None, motion_remaining_ticks=0,
             velocity=[0, 0, 0], has_body=False, is_character=False, ground='', yaw=0, pitch=0)
DEFAULT_STATE = {k: STATE[k] for k in ('world_matrix', 'layout', 'velocity', 'has_body', 'is_character', 'ground', 'yaw', 'pitch')}
TICKET = dict(epoch=EPOCH, sequence=1, kind=1, state=3, requested_tick=9, committed_tick=9,
              generation=1, recovered=False, error_code=0, diagnostic='', restored_epoch=None, restored_tick=0)
CONTROL = dict(session_id=SESSION, tick=9, current_session_id=SESSION, current_tick=9,
               runtime_replaced=False, save_serviced=False, replayed=False,
               control_sequence=2, ui_revision=4, current_control_sequence=2, current_ui_revision=4, intent=0)
STEP = dict(session_id=SESSION, tick=10, current_session_id=SESSION, current_tick=10,
            runtime_replaced=False, save_serviced=False, replayed=False,
            previous_tick=9, stepped=1, sound_events=dict(first_voice=1, next_voice=1))
OBSERVE = dict(**HEADER, **SOURCE, gameplay_revision=2, ui_revision=3, control_sequence=1,
               entities=[dict(id=ENTITY, state=DEFAULT_STATE, components={TYPE: VALUES})],
               component_types=[dict(type=TYPE, fingerprint=HASH)], query=None)
SLOT = dict(configuration_generation=1, slot='slot', exists=False, generation=0,
            current=None, previous=None, selected=None, quarantined=None,
            recovered=False, manifest_recovered=False, recovery_reason='', orphan_files=0)


def samples():
    # Deliberately explicit consumer records, not generated from the validator.
    return {
        'runtime.start': dict(session_id=SESSION, tick=0, authored_revision=3, started=True, replayed=False),
        'runtime.stop': dict(session_id=SESSION, stopped=True, replayed=False),
        'runtime.status': dict(available=True, active=False, **{k: None for k in ('session_id', 'tick', 'structure_revision', 'material_revision', 'ui_revision', 'control_sequence', 'authored_revision')}),
        'runtime.inspect': dict(session_id=SESSION, tick=9, **SOURCE, world_id=OTHER, structure_revision=1,
            material_revision=1, ui_revision=3, control_sequence=1, content_sha256=HASH, fixed_dt=1/60,
            entities=2, bodies=1, characters=0, scheduler='serial', physics='jolt'),
        'runtime.step': STEP,
        'runtime.entity': dict(session_id=SESSION, tick=9, structure_revision=1, id=ENTITY, **STATE),
        'runtime.observe': OBSERVE,
        'runtime.components': dict(**HEADER, schemas=[SCHEMA]),
        'runtime.component.get': dict(**HEADER, id=ENTITY, type=TYPE, schema=SCHEMA, values=VALUES),
        'runtime.component.query': dict(**HEADER, type=TYPE, entities=[ENTITY], next_after=None),
        'runtime.gameplay.load_native': dict(session_id=SESSION, tick=9, revision=2, structure_revision=1, module=MODULE, replayed=False),
        'runtime.gameplay.inspect': dict(session_id=SESSION, tick=9, revision=2, structure_revision=1, module=MODULE),
        'runtime.ui.inspect': dict(session_id=SESSION, tick=9, ui_revision=3, control_sequence=1, modal=None, elements=[], next_after=None),
        'runtime.ui.activate': CONTROL,
        'runtime.save.status': dict(epoch=EPOCH, enabled=False, configuration_generation=0, pending=None, last_restore=None),
        'runtime.save.result': TICKET,
        'save.status': dict(generation=0, root=None, write_mode='synchronous'),
        'save.configure': dict(generation=1, root=None, write_mode='synchronous', replayed=False),
        'save.inspect': SLOT,
        'save.write': dict(configuration_generation=1, slot='slot', generation=1, sha256=HASH, bytes=100, replayed=False, recovered=False, cleanup_pending=False),
        'save.load': dict(session_id=CURRENT, tick=9, **SOURCE, structure_revision=1, ui_revision=3, control_sequence=1,
                         slot='slot', generation=1, selected_generation=1, recovered=False, recovery_reason='', replayed=False),
    }


class WorkflowResponses(unittest.TestCase):
    def reject(self, method, value, params=None, context=None):
        before = repr(value)
        with self.assertRaises(ResponseContractError) as failure:
            validate(method, value, params, context)
        self.assertIs(failure.exception.result, value)
        self.assertEqual(repr(value), before)

    def test_manifest_portable_detached_and_matches_public_copy(self):
        manifest = workflow_response_contract()
        self.assertEqual(manifest, json.loads((ROOT / 'docs/contracts/runtime-save-responses-v1.candidate.json').read_text()))
        self.assertEqual(set(samples()), set(manifest['methods']))
        self.assertEqual(manifest['status'], 'prospective')
        manifest['methods'].clear()
        self.assertEqual(len(workflow_response_contract()['methods']), 21)

    def test_all_selected_shapes_and_additive_diagnostics(self):
        for method, original in samples().items():
            with self.subTest(method=method):
                value = copy.deepcopy(original)
                value['future_diagnostic'] = {'enabled': True, 'samples': [1, 2.5, None]}
                before = copy.deepcopy(value)
                self.assertIs(validate(method, value), value)
                self.assertEqual(value, before)

    def test_unknown_method_and_missing_header_fail_closed(self):
        self.reject('world.list', {})
        result = copy.deepcopy(samples()['runtime.component.get'])
        del result['component_revision']
        self.reject('runtime.component.get', result)

    def test_integer_headers_reject_boolean_and_float(self):
        for bad in (True, 9.0, -1, 9007199254740992):
            result = copy.deepcopy(samples()['runtime.component.get'])
            result['tick'] = bad
            self.reject('runtime.component.get', result)

    def test_nonjson_nonfinite_and_bounded_nesting(self):
        for bad in (float('nan'), float('inf'), object(), {1: 'invalid'}, (1, 2)):
            self.reject('save.status', dict(generation=0, root=None, write_mode='synchronous', debug=bad))
        nested = None
        for _ in range(65):
            nested = [nested]
        self.reject('save.status', dict(generation=0, root=None, write_mode='synchronous', debug=nested))

    def test_encoded_byte_bound_is_utf8(self):
        result = samples()['save.status']
        result['debug'] = 'é' * (8 * 1024 * 1024)
        self.reject('save.status', result)

    def test_caller_parameters_and_context_types(self):
        for params in ([], False, 'session'):
            self.reject('runtime.status', samples()['runtime.status'], params)
        for context in ([], {'profile': False}, {'profile': 'latest'}, {'tick': 9}, {'component_schemas': None}, {'gameplay_schema': None}):
            self.reject('runtime.status', samples()['runtime.status'], context=context)
        result = samples()['runtime.entity']
        self.reject('runtime.entity', result, dict(session_id=SESSION, id=ENTITY, tick=9.0))
        self.reject('runtime.entity', result, dict(session_id=SESSION, id=True))

    def test_read_guards_optional_and_supplied_guards_exact(self):
        result = samples()['runtime.component.get']
        params = dict(session_id=SESSION, tick=9, id=ENTITY, type=TYPE)
        self.assertIs(validate('runtime.component.get', result, params), result)
        self.reject('runtime.component.get', result, dict(params, structure_revision=0))
        self.reject('runtime.component.get', result, dict(params, component_revision=True))
        self.reject('runtime.component.get', result, dict(params, session_id=OTHER))

    def test_component_null_differs_from_empty_object(self):
        result = copy.deepcopy(samples()['runtime.component.get'])
        result['values'] = None
        validate('runtime.component.get', result)
        result['values'] = {}
        self.reject('runtime.component.get', result)

    def test_component_lossless_decimal_and_array_types(self):
        for field, bad in ((F1, True), (F1, 3.0), (F2, 9223372036854775807), (F2, '+2'), (F2, '-0'), (F2, '9223372036854775808'), (F3, 'A' * 32), (F4, [1, True]), (F4, [1, 2, 3, 4])):
            result = copy.deepcopy(samples()['runtime.component.get'])
            result['values'][field] = bad
            self.reject('runtime.component.get', result)

    def test_schema_version_fingerprint_capacity_and_defaults(self):
        for mutator in (lambda s: s.update(version=True), lambda s: s.update(fingerprint='a'*63),
                        lambda s: s.update(version=1), lambda s: s['fields'][3].update(capacity=True),
                        lambda s: s['fields'][3].update(default=[0]), lambda s: s['fields'][2].update(default=ENTITY),
                        lambda s: s['fields'][0].update(name='bad\nname')):
            result = copy.deepcopy(samples()['runtime.components'])
            mutator(result['schemas'][0])
            self.reject('runtime.components', result)

    def test_schema_context_preserves_values_and_allows_new_diagnostics(self):
        result = copy.deepcopy(samples()['runtime.component.get'])
        result['schema']['future'] = {'hint': True}
        context = {'profile': 'source_world', 'component_schemas': {TYPE: copy.deepcopy(SCHEMA)}}
        before = copy.deepcopy(context)
        validate('runtime.component.get', result, context=context)
        self.assertEqual(context, before)
        context['component_schemas'][TYPE]['fields'][0]['unit'] = 'rounds'
        self.reject('runtime.component.get', result, context=context)
        self.reject('runtime.component.get', result, context={'component_schemas': {OTHER: SCHEMA}})

    def test_module_declarations_allow_native_optional_metadata_and_order(self):
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        declaration = result['module']['schema']['components'][0]
        del declaration['fingerprint']
        for field in declaration['fields']:
            del field['unit']
        declaration['fields'].reverse()
        declaration['future_diagnostic'] = {'revision': 2}
        validate('runtime.gameplay.inspect', result, dict(session_id=SESSION, include_schema=True),
                 {'gameplay_schema': copy.deepcopy(result['module']['schema'])})
        canonical = dict(**HEADER, schemas=[declaration])
        self.reject('runtime.components', canonical)
        declaration['fields'][0]['capacity'] = True
        self.reject('runtime.gameplay.inspect', result)
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        result['module']['schema']['components'][0]['fields'][2]['default'] = ENTITY
        self.reject('runtime.gameplay.inspect', result)

    def test_canonical_schema_metadata_order_and_embedded_uniqueness(self):
        for mutator in (lambda s: s.pop('fingerprint'), lambda s: s['fields'][0].pop('unit'),
                        lambda s: s['fields'].reverse()):
            result = copy.deepcopy(samples()['runtime.components'])
            mutator(result['schemas'][0])
            self.reject('runtime.components', result)
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        declaration = result['module']['schema']['components'][0]
        declaration['fields'][1]['id'] = declaration['fields'][0]['id']
        self.reject('runtime.gameplay.inspect', result)
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        result['module']['schema']['components'].append(copy.deepcopy(SCHEMA))
        self.reject('runtime.gameplay.inspect', result)

    def test_component_query_exclusive_sorted_cursor(self):
        result = copy.deepcopy(samples()['runtime.component.query'])
        result['entities'] = [ENTITY, OTHER]
        result['next_after'] = OTHER
        params = dict(session_id=SESSION, tick=9, type=TYPE, limit=2)
        validate('runtime.component.query', result, params)
        self.reject('runtime.component.query', result, dict(params, limit=True))
        self.reject('runtime.component.query', result, dict(params, after=ENTITY))
        result['entities'] = [OTHER, ENTITY]
        self.reject('runtime.component.query', result, params)

    def test_compact_full_and_empty_selector_are_distinct(self):
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        params = dict(session_id=SESSION, include_schema=True)
        validate('runtime.gameplay.inspect', result, params)
        self.reject('runtime.gameplay.inspect', result, dict(session_id=SESSION))
        result['module'] = dict(identity=GAME_SCHEMA['identity'], assembly_sha256=HASH, values=copy.deepcopy(GAME_VALUES))
        validate('runtime.gameplay.inspect', result, dict(session_id=SESSION), {'gameplay_schema': GAME_SCHEMA})
        self.reject('runtime.gameplay.inspect', result, params)
        result['module']['schema'] = GAME_SCHEMA
        self.reject('runtime.gameplay.inspect', result)
        del result['module']['schema']
        result['module']['values'] = {}
        validate('runtime.gameplay.inspect', result, dict(session_id=SESSION, fields=[]), {'gameplay_schema': GAME_SCHEMA})

    def test_gameplay_selector_type_duplicate_unknown_and_module_absence(self):
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        result['module'] = dict(identity=GAME_SCHEMA['identity'], assembly_sha256=HASH, values={'Count': 2})
        for fields in ('Count', [False], ['Count', 'Count'], ['Unknown']):
            self.reject('runtime.gameplay.inspect', result, dict(session_id=SESSION, fields=fields), {'gameplay_schema': GAME_SCHEMA})
        result['module'] = None
        validate('runtime.gameplay.inspect', result, dict(session_id=SESSION))
        self.reject('runtime.gameplay.inspect', result, dict(session_id=SESSION, fields=[]))

    def test_gameplay_layout_and_context_identity(self):
        result = copy.deepcopy(samples()['runtime.gameplay.inspect'])
        bad = copy.deepcopy(GAME_SCHEMA)
        bad['fields'][1]['offset'] = 0
        self.reject('runtime.gameplay.inspect', result, context={'gameplay_schema': bad})
        bad = copy.deepcopy(GAME_SCHEMA)
        bad['identity'] = 'different'
        self.reject('runtime.gameplay.inspect', result, context={'gameplay_schema': bad})
        result['module']['values']['Exact'] = -9223372036854775808
        self.reject('runtime.gameplay.inspect', result)

    def test_native_load_retry_still_needs_integer_guards(self):
        result = samples()['runtime.gameplay.load_native']
        params = dict(session_id=SESSION, request_id=OTHER, expected_tick=9, expected_revision=1, descriptor='native-gameplay.json')
        validate('runtime.gameplay.load_native', result, params)
        for key in ('expected_tick', 'expected_revision'):
            self.reject('runtime.gameplay.load_native', result, dict(params, **{key: float(params[key])}))

    def test_observation_projection_and_union(self):
        result = copy.deepcopy(OBSERVE)
        params = dict(session_id=SESSION, tick=9, ids=[ENTITY], components=[dict(type=TYPE)])
        context = {'component_schemas': [SCHEMA]}
        before = copy.deepcopy((result, params, context))
        validate('runtime.observe', result, params, context)
        self.assertEqual((result, params, context), before)
        result['entities'][0]['state']['animation'] = None
        self.reject('runtime.observe', result, params, context)
        del result['entities'][0]['state']['animation']
        result['entities'][0]['components'][TYPE][F2] = 2
        self.reject('runtime.observe', result, params, context)

    def test_observation_bad_selectors_and_fingerprint(self):
        params = dict(session_id=SESSION, tick=9, ids=[ENTITY], components=[dict(type=TYPE)])
        for change in ({'ids': [ENTITY, ENTITY]}, {'entity_fields': ['yaw', 'yaw']}, {'entity_fields': ['not_a_field']},
                       {'components': [dict(type=TYPE, fields=[True])]}, {'include_schemas': 1}):
            self.reject('runtime.observe', copy.deepcopy(OBSERVE), dict(params, **change))
        result = copy.deepcopy(OBSERVE)
        result['component_types'][0]['fingerprint'] = 'b'*64
        self.reject('runtime.observe', result, params, {'component_schemas': [SCHEMA]})

    def test_observation_schema_inclusion_and_absent_unknown_field(self):
        result = copy.deepcopy(OBSERVE)
        result['component_types'][0]['schema'] = copy.deepcopy(SCHEMA)
        params = dict(session_id=SESSION, tick=9, ids=[ENTITY], components=[dict(type=TYPE)], include_schemas=True)
        validate('runtime.observe', result, params)
        self.reject('runtime.observe', result, dict(params, include_schemas=False))
        result['entities'][0]['components'][TYPE] = None
        params['components'][0]['fields'] = [uid(999)]
        self.reject('runtime.observe', result, params)

    def test_observation_query_continuation_requires_both_pins(self):
        result = copy.deepcopy(OBSERVE)
        result['query'] = dict(type=TYPE, entities=[ENTITY], next_after=ENTITY)
        params = dict(session_id=SESSION, tick=9, query=dict(type=TYPE, after=ZERO, limit=1),
                      components=[dict(type=TYPE)], structure_revision=1, component_revision=2)
        validate('runtime.observe', result, params, {'component_schemas': [SCHEMA]})
        del params['component_revision']
        self.reject('runtime.observe', result, params)

    def test_step_request_arithmetic_and_save_operation_omission(self):
        params = dict(session_id=SESSION, request_id=OTHER, expected_tick=9, ticks=1)
        validate('runtime.step', STEP, params)
        result = copy.deepcopy(STEP)
        result['tick'] = 11
        self.reject('runtime.step', result, params)
        result = dict(STEP, save_operation=None)
        self.reject('runtime.step', result, params)

    def test_control_replacement_and_historical_retry_boundaries(self):
        result = copy.deepcopy(CONTROL)
        result.update(current_session_id=CURRENT, current_tick=3, runtime_replaced=True, save_serviced=True,
                      current_ui_revision=1, current_control_sequence=0,
                      save_operation=dict(TICKET, kind=2, restored_epoch=CURRENT, restored_tick=3))
        params = dict(session_id=SESSION, request_id=OTHER, expected_tick=9, expected_ui_revision=3,
                      expected_control_sequence=1, expected_gameplay_revision=2, id=ENTITY)
        validate('runtime.ui.activate', result, params)
        result['save_operation']['restored_tick'] = 4
        self.reject('runtime.ui.activate', result, params)
        result = dict(CONTROL, replayed=True, current_tick=20, current_ui_revision=15, current_control_sequence=10)
        validate('runtime.ui.activate', result, params)

    def test_step_prior_load_branch_is_bounded(self):
        result = dict(STEP, session_id=CURRENT, current_session_id=CURRENT, previous_tick=3, tick=4, current_tick=4,
                      save_serviced=True, save_operation=dict(TICKET, kind=2, restored_epoch=CURRENT, restored_tick=3))
        params = dict(session_id=SESSION, request_id=OTHER, expected_tick=9, ticks=1)
        validate('runtime.step', result, params)
        result['save_serviced'] = False
        self.reject('runtime.step', result, params)

    def test_ticket_enums_expiry_and_after_stop_are_distinct(self):
        unknown = dict(TICKET, kind=0, state=0, requested_tick=0, committed_tick=0, generation=0)
        validate('runtime.save.result', unknown, dict(epoch=EPOCH, sequence=1))
        for change in ({'epoch': ZERO}, {'sequence': True}, {'kind': 5}, {'state': 8}, {'kind': 2, 'restored_epoch': None}, {'committed_tick': 8}):
            self.reject('runtime.save.result', dict(TICKET, **change))
        self.reject('runtime.save.result', TICKET, dict(epoch=OTHER, sequence=1))

    def test_ticket_success_failure_and_expiry_completion_data(self):
        for change in ({'generation': 0}, {'error_code': -1}, {'recovered': True},
                       {'state': 4, 'error_code': 0}, {'state': 0},
                       {'kind': 2, 'restored_epoch': EPOCH}):
            self.reject('runtime.save.result', dict(TICKET, **change))
        validate('runtime.save.result', dict(TICKET, state=4, generation=0, error_code=-32070))
        validate('runtime.save.result', dict(TICKET, state=1, generation=0))
        validate('runtime.save.result', dict(TICKET, state=2, generation=0, error_code=-32070))

    def test_ui_eligibility_and_continuation(self):
        result = copy.deepcopy(samples()['runtime.ui.inspect'])
        label = dict(id=ENTITY, parent=None, name='Status', kind='label', action=None, text='Saved', visible=True,
                     enabled=True, effective_visible=True, effective_enabled=True, eligible=False)
        result['elements'] = [label]
        validate('runtime.ui.inspect', result, dict(session_id=SESSION, tick=9))
        result['elements'][0]['eligible'] = True
        self.reject('runtime.ui.inspect', result)
        result['elements'][0].update(kind='button', action='save', effective_visible=False)
        self.reject('runtime.ui.inspect', result)
        result['elements'][0].update(eligible=False)
        result['next_after'] = ENTITY
        self.reject('runtime.ui.inspect', result, dict(session_id=SESSION, tick=9, after=ZERO, limit=1))

    def test_uncommitted_existing_slot_and_retained_record_diagnostics(self):
        result = dict(SLOT, exists=True)
        validate('save.inspect', result, dict(slot='slot'))
        current = dict(generation=1, sha256=HASH, bytes=100, verified=True, writer='future diagnostic')
        result = dict(SLOT, exists=True, generation=1, current=current,
                      selected={k: current[k] for k in ('generation', 'sha256', 'bytes', 'verified')})
        validate('save.inspect', result, dict(slot='slot'))
        result['selected']['sha256'] = 'b'*64
        self.reject('save.inspect', result, dict(slot='slot'))

    def test_durable_save_retry_and_restore_revision_direction(self):
        result = dict(samples()['save.write'], replayed=True)
        params = dict(request_id=OTHER, configuration_generation=1, slot='slot', expected_generation=0,
                      session_id=SESSION, expected_tick=9, expected_gameplay_revision=2)
        validate('save.write', result, params)
        self.reject('save.write', result, dict(params, configuration_generation=0))
        result = dict(samples()['save.load'], authored_revision=10, current_authored_revision=3)
        params = dict(request_id=OTHER, configuration_generation=1, slot='slot', expected_generation=1,
                      revision=3, expected_session_id=None, expected_tick=None, expected_gameplay_revision=None, new_session_id=CURRENT)
        validate('save.load', result, params)
        self.reject('save.load', result, dict(params, new_session_id=SESSION))
        result['source_stale'] = False
        self.reject('save.load', result, params)


if __name__ == '__main__':
    unittest.main()
