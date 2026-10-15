# SPDX-License-Identifier: Apache-2.0
"""Prospective runtime/save successful-result checks; opt-in and transport-free.

This is not a released response, error, behavior, ABI or availability contract.
Unknown methods fail closed. The original result is returned without mutation.
Optional context keys: profile, component_schemas (array or ID-keyed map), and
gameplay_schema (the full native schema).
Context supplies trusted consumer metadata, never an inferred latest boundary.
Without a schema, compact values receive wire-shape checks only. No request,
retry, native call, discovery, envelope validation or schema lookup is implicit.
"""
import json

from .responses import ResponseContractError
from .runtime_response_checks_v1 import ResultChecksV1

MAX_INTEGER = 9007199254740991
DEFAULT_FIELDS = ('world_matrix', 'layout', 'velocity', 'has_body', 'is_character',
                  'ground', 'yaw', 'pitch')
PROFILES = ('source_world', 'source_shared_player', 'readonly_game')


def _ref(name):
    return {'$ref': '#/$defs/' + name}


def _nullable(schema):
    return {'anyOf': [schema, {'type': 'null'}]}


def _object(fields, optional=(), *, closed=False):
    return {'type': 'object', 'properties': fields,
            'required': [key for key in fields if key not in optional],
            'additionalProperties': not closed}


def _array(items, maximum, minimum=0):
    return {'type': 'array', 'items': items, 'minItems': minimum, 'maxItems': maximum}


def _build_contract():
    integer = {'type': 'integer', 'minimum': 0, 'maximum': MAX_INTEGER}
    number, boolean, text = {'type': 'number'}, {'type': 'boolean'}, {'type': 'string'}
    identity = {'type': 'string', 'pattern': '[0-9a-f]{32}'}
    hash_value = {'type': 'string', 'pattern': '[0-9a-f]{64}'}
    vector = _array(number, 3, 3)
    transform = _object({'position': vector, 'rotation': _array(number, 4, 4),
                         'scale': vector})
    field_kinds = ['int32', 'int64', 'float32', 'float64', 'entity']
    component_field = _object({'id': identity, 'name': text,
        'kind': {'type': 'string', 'enum': field_kinds + ['array']},
        'default': {}, 'unit': text, 'element_kind': {'type': 'string', 'enum': field_kinds},
        'capacity': {'type': 'integer', 'minimum': 1, 'maximum': 31}},
        ('element_kind', 'capacity'))
    component_schema = _object({'id': identity, 'name': text,
        'version': {'type': 'integer', 'enum': [1, 2]}, 'fingerprint': hash_value,
        'fields': _array(component_field, 32, 1)})
    declaration_field = _object(component_field['properties'], ('unit', 'element_kind', 'capacity'))
    declaration = _object({**component_schema['properties'], 'fields': _array(declaration_field, 32, 1)}, ('fingerprint',))
    gameplay_field = _object({'name': text,
        'kind': {'type': 'string', 'enum': field_kinds}, 'offset': integer,
        'bytes': {'type': 'integer', 'minimum': 4, 'maximum': 16}})
    gameplay_schema = _object({'identity': {'type': 'string', 'minLength': 1, 'maxLength': 128},
        'bytes': {'type': 'integer', 'minimum': 1, 'maximum': 65536},
        'fields': _array(gameplay_field, 128, 1),
        'components': _array(_ref('component_declaration'), 64), 'persistent': {'type': 'object'}},
        ('components', 'persistent'))
    module = _object({'backend': {'type': 'string', 'enum': ['coreclr', 'native_aot']},
        'native_library': text, 'native_diagnostics': {'type': 'object'},
        'assembly': text, 'assembly_sha256': hash_value, 'type': text,
        'schema': _ref('gameplay_schema'), 'migration': _nullable({'type': 'object'}),
        'values': {'type': 'object'}})
    compact = _object({'identity': {'type': 'string', 'minLength': 1, 'maxLength': 128},
                      'assembly_sha256': hash_value, 'values': {'type': 'object'}}, closed=True)
    transition = _object({'start_tick': integer, 'duration_ticks': integer,
        'elapsed_ticks': integer, 'weight': number, 'source_frozen': boolean,
        'source_clip': _nullable(integer), 'source_time': _nullable(number),
        'source_speed': _nullable(number), 'source_loop': _nullable(boolean),
        'source_playing': _nullable(boolean), 'mode': {'type': 'string', 'enum': ['inertial']}}, ('mode',))
    animation = _object({'clip': _nullable(integer), 'time': number, 'speed': number,
        'loop': boolean, 'playing': boolean, 'duration': number,
        'transition': _nullable(transition), 'layers': _array(_ref('animation_layer'), 4)}, ('layers',))
    weight_transition = _object({'start_tick': integer, 'duration_ticks': integer,
        'elapsed_ticks': integer, 'source': number, 'target': number})
    layer = _object({'slot': {'type': 'integer', 'minimum': 1, 'maximum': 4},
        'mode': {'type': 'string', 'enum': ['override', 'additive']},
        'weight': number, 'target_weight': number,
        'weight_transition': _nullable(weight_transition),
        'mask_nodes': _array(integer, 65536), 'playback': _ref('animation')})
    motion = _object({'entity': identity, 'position': vector,
                     'rotation': _array(number, 4, 4), 'duration_ticks': integer})
    state = {'world_matrix': _array(number, 16, 16), 'layout': {'const': 'column_major'},
        'local_transform': transform, 'animation': _nullable(_ref('animation')),
        'motion': {'type': 'string', 'enum': ['none', 'static', 'dynamic', 'kinematic', 'character']},
        'kinematic_target': _nullable(motion), 'motion_remaining_ticks': integer,
        'velocity': vector, 'has_body': boolean, 'is_character': boolean,
        'ground': {'type': 'string', 'enum': ['', 'on_ground', 'steep_ground', 'not_supported', 'in_air', 'unknown']},
        'yaw': number, 'pitch': number}
    locomotion = _object({'mode': {'const': 'virtual-v1'},
        'profile': _object({'step_height': number, 'ground_snap': number, 'max_slope_degrees': number}),
        'ground_normal': vector, 'ground_velocity': vector})
    ticket = _object({'epoch': identity,
        'sequence': {'type': 'integer', 'minimum': 1, 'maximum': MAX_INTEGER},
        'kind': {'type': 'integer', 'enum': [0, 1, 2]},
        'state': {'type': 'integer', 'enum': [0, 1, 2, 3, 4]},
        'requested_tick': integer, 'committed_tick': integer, 'generation': integer,
        'recovered': boolean, 'error_code': {'type': 'integer', 'minimum': -2147483648, 'maximum': 2147483647},
        'diagnostic': text, 'restored_epoch': _nullable(identity), 'restored_tick': integer})
    generation = _object({'generation': integer, 'sha256': hash_value,
                          'bytes': integer, 'verified': boolean})
    restore = _object({'initiating_epoch': identity, 'initiating_sequence': integer,
        'source_tick': integer, 'restored_tick': integer, 'generation': integer, 'recovered': boolean})
    ui_element = _object({'id': identity, 'parent': _nullable(identity), 'name': text,
        'kind': {'type': 'string', 'enum': ['panel', 'label', 'button']},
        'action': _nullable(text), 'text': text, 'visible': boolean, 'enabled': boolean,
        'effective_visible': boolean, 'effective_enabled': boolean, 'eligible': boolean,
        'layout': {'type': 'object'}, 'style': {'type': 'object'}}, ('layout', 'style'))
    header = {'session_id': identity, 'tick': integer}
    component_header = {**header, 'component_revision': integer, 'structure_revision': integer}
    source = {'authored_revision': integer, 'current_authored_revision': integer, 'source_stale': boolean}
    operation = {'current_session_id': identity, 'current_tick': integer,
        'runtime_replaced': boolean, 'save_serviced': boolean, 'replayed': boolean,
        'save_operation': _ref('save_ticket')}
    methods = {
        'runtime.start': _object({**header, 'authored_revision': integer, 'started': {'const': True}, 'replayed': boolean}),
        'runtime.status': _object({'available': boolean, 'active': boolean,
            **{k: _nullable(v) for k, v in {**header, 'structure_revision': integer, 'material_revision': integer,
                'ui_revision': integer, 'control_sequence': integer, 'authored_revision': integer}.items()}}),
        'runtime.inspect': _object({**header, **source, 'world_id': identity, 'structure_revision': integer,
            'material_revision': integer, 'ui_revision': integer, 'control_sequence': integer,
            'content_sha256': hash_value, 'fixed_dt': {'type': 'number', 'exclusiveMinimum': 0},
            'entities': integer, 'bodies': integer, 'characters': integer, 'scheduler': text, 'physics': text}),
        'runtime.stop': _object({'session_id': identity, 'stopped': {'const': True}, 'replayed': boolean}),
        'runtime.step': _object({**header, **operation, 'previous_tick': integer,
            'stepped': {'type': 'integer', 'minimum': 1, 'maximum': 600},
            'sound_events': _object({'first_voice': integer, 'next_voice': integer})}, ('save_operation',)),
        'runtime.entity': _object({**header, 'structure_revision': integer, 'id': identity,
                                  **state, 'locomotion': locomotion}, ('locomotion',)),
        'runtime.observe': _object({**component_header, **source, 'gameplay_revision': integer,
            'ui_revision': integer, 'control_sequence': integer,
            'entities': _array(_object({'id': identity, 'state': {'type': 'object', 'properties': state},
                                       'components': {'type': 'object'}}), 96),
            'component_types': _array(_object({'type': identity, 'fingerprint': hash_value,
                'schema': _ref('component_schema')}, ('schema',)), 4),
            'query': _nullable(_object({'type': identity, 'entities': _array(identity, 64),
                                       'next_after': _nullable(identity)}))}),
        'runtime.components': _object({**component_header, 'schemas': _array(_ref('component_schema'), 64)}),
        'runtime.component.get': _object({**component_header, 'id': identity, 'type': identity,
            'schema': _ref('component_schema'), 'values': _nullable({'type': 'object'})}),
        'runtime.component.query': _object({**component_header, 'type': identity,
            'entities': _array(identity, 256), 'next_after': _nullable(identity)}),
        'runtime.gameplay.load_native': _object({**header, 'revision': integer, 'structure_revision': integer,
                                               'module': module, 'replayed': boolean}),
        'runtime.gameplay.inspect': _object({**header, 'revision': integer, 'structure_revision': integer,
                        'module': {'anyOf': [module, compact, {'type': 'null'}]}}),
        'runtime.ui.inspect': _object({**header, 'ui_revision': integer, 'control_sequence': integer,
            'modal': _nullable(identity), 'elements': _array(ui_element, 256), 'next_after': _nullable(identity)}),
        'runtime.ui.activate': _object({**header, **operation, 'control_sequence': integer,
            'ui_revision': integer, 'current_control_sequence': integer, 'current_ui_revision': integer,
            'intent': {'type': 'integer', 'enum': [0, 1, 2]}}, ('save_operation',)),
        'runtime.save.status': _object({'epoch': identity, 'enabled': boolean,
            'configuration_generation': integer, 'pending': _nullable(_ref('save_ticket')),
            'last_restore': _nullable(restore)}),
        'runtime.save.result': _ref('save_ticket'),
        'save.status': _object({'generation': integer, 'root': _nullable(text),
                               'write_mode': {'type': 'string', 'enum': ['synchronous', 'background']}}),
        'save.configure': _object({'generation': integer, 'root': _nullable(text),
            'write_mode': {'type': 'string', 'enum': ['synchronous', 'background']}, 'replayed': boolean}),
        'save.inspect': _object({'configuration_generation': integer, 'slot': text, 'exists': boolean,
            'generation': integer, **{k: _nullable(generation) for k in ('current', 'previous', 'selected', 'quarantined')},
            'recovered': boolean, 'manifest_recovered': boolean, 'recovery_reason': text, 'orphan_files': integer}),
        'save.write': _object({'configuration_generation': integer, 'slot': text, 'generation': integer,
            'sha256': hash_value, 'bytes': integer, 'replayed': boolean, 'recovered': boolean, 'cleanup_pending': boolean}),
        'save.load': _object({**header, **source, 'structure_revision': integer, 'ui_revision': integer,
            'control_sequence': integer, 'slot': text, 'generation': integer, 'selected_generation': integer,
            'recovered': boolean, 'recovery_reason': text, 'replayed': boolean, 'upgrade': {'type': 'object'}}, ('upgrade',))}
    return {'$comment': 'SPDX-License-Identifier: Apache-2.0',
        'format': 'poima.runtime-save-responses.v1.candidate', 'status': 'prospective',
        'contract_version': 1, 'source_cohort': '0.0.142; request selection v2, schema revision98',
        'profiles': list(PROFILES), 'context_keys': ['profile', 'component_schemas', 'gameplay_schema'],
        'scope': 'Opt-in selected successful-result shapes and documented consumer predicates only; no stable API, native error, behavior, ABI, physical input or availability promise.',
        'limits': {'depth': 64, 'nodes': 100000, 'encoded_bytes': 16 * 1024 * 1024},
        'unchecked': ['JSON-RPC envelopes/errors', 'native liveness and unchanged-state behavior',
            'cryptographic derivation of component fingerprints', 'semantic completeness of additive diagnostics',
            'full UI layout/style and gameplay persistence/upgrade schemas',
            'compact gameplay/component values without caller-supplied schema'],
        '$defs': {'id': identity, 'integer': integer, 'component_schema': component_schema,
                  'component_declaration': declaration, 'gameplay_schema': gameplay_schema, 'animation': animation,
                  'animation_layer': layer, 'save_ticket': ticket},
        'methods': dict(sorted(methods.items()))}


_CONTRACT = _build_contract()
_CHECKS = ResultChecksV1(_CONTRACT)


def workflow_response_contract():
    """Return a detached prospective manifest, portable without repository files."""
    return _CHECKS.contract()


def validate_workflow_result(method, result, params=None, context=None):
    """Check a selected successful result; return it unchanged, never perform I/O.

    Request joins run when params is supplied. Context is optional trusted schema
    metadata; profile alone cannot prove which native scope produced the reply.
    Native error envelopes and effects require independent executable gates.
    """
    return _CHECKS.validate(method, result, params, context)
