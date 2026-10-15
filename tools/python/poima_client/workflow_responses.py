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
import copy
import json
import math
import re

from .responses import ResponseContractError

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


def workflow_response_contract():
    """Return a detached prospective manifest, portable without repository files."""
    return copy.deepcopy(_CONTRACT)


def _need(condition, reason):
    if not condition:
        raise ValueError(reason)


def _json(value):
    pending = [(value, 0)]
    nodes = 0
    while pending:
        item, depth = pending.pop()
        nodes += 1
        _need(depth <= 64 and nodes <= 100000, 'JSON exceeds depth/node bound')
        _need(type(item) in (dict, list, str, bool, int, float, type(None)), 'Non-JSON value')
        if type(item) is dict:
            _need(all(type(k) is str for k in item), 'JSON object keys must be strings')
            pending.extend((v, depth + 1) for v in item.values())
        elif type(item) is list:
            pending.extend((v, depth + 1) for v in item)
        elif type(item) is float:
            _need(math.isfinite(item), 'Nonfinite JSON number')
    encoded = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8')
    _need(len(encoded) <= 16 * 1024 * 1024, 'JSON exceeds encoded byte bound')


def _literal(a, b):
    return type(a) is type(b) and a == b


def _check(value, schema, path='$', depth=0):
    _need(depth <= 64, path + ': schema nesting exceeds64')
    if '$ref' in schema:
        return _check(value, _CONTRACT['$defs'][schema['$ref'].split('/')[-1]], path, depth + 1)
    if 'anyOf' in schema:
        accepted = False
        for branch in schema['anyOf']:
            try:
                _check(value, branch, path, depth + 1)
                accepted = True
                break
            except ValueError:
                pass
        _need(accepted, path + ': no accepted variant')
    for key in ('const', 'enum'):
        if key in schema:
            options = [schema[key]] if key == 'const' else schema[key]
            _need(any(_literal(value, x) for x in options), path + ': unexpected literal/enum type')
    kind = schema.get('type')
    kinds = {'object': dict, 'array': list, 'string': str, 'boolean': bool,
             'integer': int, 'null': type(None)}
    if kind:
        _need(type(value) in (int, float) if kind == 'number' else type(value) is kinds[kind], path + ': expected ' + kind)
    if type(value) in (int, float):
        for key, predicate in (('minimum', lambda x, y: x >= y), ('maximum', lambda x, y: x <= y),
                               ('exclusiveMinimum', lambda x, y: x > y)):
            if key in schema:
                _need(predicate(value, schema[key]), path + ': numeric bound')
    if type(value) is str:
        _need(len(value) >= schema.get('minLength', 0) and len(value) <= schema.get('maxLength', len(value)), path + ': text length')
        if 'pattern' in schema:
            _need(re.fullmatch(schema['pattern'], value) is not None, path + ': invalid identifier')
    if type(value) is list:
        _need(schema.get('minItems', 0) <= len(value) <= schema.get('maxItems', len(value)), path + ': array bound')
        if 'items' in schema:
            for i, item in enumerate(value):
                _check(item, schema['items'], path + '[' + str(i) + ']', depth + 1)
    if type(value) is dict:
        _need(set(schema.get('required', [])) <= set(value), path + ': missing required field')
        known = schema.get('properties', {})
        if schema.get('additionalProperties') is False:
            _need(set(value) <= set(known), path + ': unexpected projected field')
        for key, child in known.items():
            if key in value:
                _check(value[key], child, path + '.' + key, depth + 1)


def _integer(value):
    return type(value) is int and 0 <= value <= MAX_INTEGER


def _ids(values, maximum, *, nonzero=False):
    _need(type(values) is list and len(values) <= maximum, 'Identifier array bound')
    for value in values:
        _check(value, _ref('id'))
        _need(not nonzero or value != '0' * 32, 'Zero registered identity')
    _need(values == sorted(set(values)), 'Identifiers must be sorted and unique')


def _typed(value, kind):
    if kind == 'int32':
        _need(type(value) is int and -2147483648 <= value <= 2147483647, 'int32 wire type/range')
    elif kind == 'int64':
        _need(type(value) is str and re.fullmatch(r'-?(0|[1-9][0-9]*)', value) is not None,
              'int64 requires canonical decimal text')
        _need(len(value) <= 20 and -(2**63) <= int(value) < 2**63 and str(int(value)) == value, 'int64 range/canonical form')
    elif kind in ('float32', 'float64'):
        _need(type(value) in (int, float), 'Floating field needs numeric wire value')
        _need(abs(value) <= (3.4028234663852886e38 if kind == 'float32' else 1.7976931348623157e308), 'Floating field overflow')
    else:
        _need(kind == 'entity', 'Unsupported scalar kind')
        _check(value, _ref('id'))


def _schema_same(a, b, gameplay=False):
    keys = ('identity', 'bytes') if gameplay else ('id', 'name', 'version', 'fingerprint')
    if not all(_literal(a.get(k), b.get(k)) for k in keys):
        return False
    field_keys = ('name', 'kind', 'offset', 'bytes') if gameplay else ('id', 'name', 'kind', 'default', 'unit', 'element_kind', 'capacity')
    if len(a['fields']) != len(b['fields']):
        return False
    for x, y in zip(a['fields'], b['fields']):
        for key in field_keys:
            if (key in x) != (key in y) or (key in x and not _json_same(x[key], y[key])):
                return False
    if gameplay:
        ac, bc = a.get('components', []), b.get('components', [])
        return len(ac) == len(bc) and all(_schema_same(x, y) for x, y in zip(ac, bc))
    return True


def _json_same(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return a.keys() == b.keys() and all(_json_same(a[k], b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(_json_same(x, y) for x, y in zip(a, b))
    return a == b


def _label(value, maximum, empty=False):
    return ((empty or bool(value)) and len(value.encode('utf-8')) <= maximum
            and not any(ord(x) < 32 or ord(x) == 127 for x in value))


def _component_schema(schema, declaration=False):
    _check(schema, _ref('component_declaration' if declaration else 'component_schema'))
    _need(schema['id'] != '0' * 32 and _label(schema['name'], 64), 'Component identity/name')
    fields = schema['fields']
    identities = [f['id'] for f in fields]
    _ids(sorted(identities) if declaration else identities, 32, nonzero=True)
    _need(len({f['name'] for f in fields}) == len(fields), 'Duplicate component field name')
    total, arrays = 0, False
    for f in fields:
        _need(_label(f['name'], 64) and _label(f.get('unit', ''), 24, empty=True), 'Component name/unit bound')
        if f['kind'] == 'array':
            arrays = True
            _need(schema['version'] == 2 and {'capacity', 'element_kind'} <= f.keys() and f['default'] == [], 'Array schema/default')
            total += (f['capacity'] + 1) * 16
        else:
            _need(not {'capacity', 'element_kind'} & f.keys(), 'Scalar contains array metadata')
            _typed(f['default'], f['kind'])
            _need(f['kind'] != 'entity' or f['default'] == '0' * 32, 'Entity default must be unset')
            total += 16
    _need(total <= 512 and (schema['version'] == 1 or arrays), 'Component payload/version bound')


def _component_values(values, schema, selected=None):
    if schema is not None and selected is not None:
        _need(set(selected) <= {f['id'] for f in schema['fields']}, 'Unknown selected component field')
    if values is None:
        return
    _need(type(values) is dict, 'Component values must be object or null')
    fields = {f['id']: f for f in schema['fields']} if schema is not None else None
    if fields is not None and selected is None:
        selected = set(fields)
    if selected is not None:
        _need(set(values) == set(selected), 'Component values differ from selected fields')
    for key, value in values.items():
        _check(key, _ref('id'))
        if fields is None:
            _need(type(value) in (int, float, str, list), 'Unsupported unschematized component value')
            if type(value) is list:
                _need(len(value) <= 31 and all(type(x) in (int, float, str) for x in value), 'Unschematized array wire shape')
            continue
        _need(key in fields, 'Unknown component field')
        f = fields[key]
        if f['kind'] == 'array':
            _need(type(value) is list and len(value) <= f['capacity'], 'Component array capacity/type')
            for item in value:
                _typed(item, f['element_kind'])
        else:
            _typed(value, f['kind'])


def _gameplay_values(module, schema, selected=None):
    values = module['values']
    fields = None
    if schema is not None:
        _check(schema, _ref('gameplay_schema'))
        fields = {f['name']: f for f in schema['fields']}
        _need(len(fields) == len(schema['fields']), 'Duplicate gameplay field')
        spans = set()
        for f in fields.values():
            size = {'int32': 4, 'int64': 8, 'float32': 4, 'float64': 8, 'entity': 16}[f['kind']]
            _need(f['bytes'] == size and f['offset'] + size <= schema['bytes'], 'Gameplay field layout')
            occupied = set(range(f['offset'], f['offset'] + size))
            _need(not spans & occupied, 'Overlapping gameplay field layout')
            spans |= occupied
        if 'schema' in module:
            _need(_schema_same(module['schema'], schema, gameplay=True), 'Gameplay schema differs from consumer context')
        else:
            _need(module['identity'] == schema['identity'], 'Gameplay identity differs from consumer schema')
        if selected is None:
            selected = set(fields)
    if selected is not None:
        _need(set(values) == set(selected), 'Gameplay values differ from requested fields')
    _need(len(values) <= 128, 'Gameplay values field bound')
    for name, value in values.items():
        _need(1 <= len(name.encode('utf-8')) <= 64, 'Gameplay field name bound')
        if fields is not None:
            _need(name in fields, 'Unknown gameplay field')
            _typed(value, fields[name]['kind'])
        else:
            _need(type(value) in (int, float, str), 'Unsupported unschematized gameplay value')
    if schema is not None:
        components = schema.get('components', [])
        _need(len({c['id'] for c in components}) == len(components), 'Duplicate gameplay component declaration')
        for component in components:
            _component_schema(component, declaration=True)


def _page(ids, next_after, params, maximum, default):
    _ids(ids, maximum)
    limit = params.get('limit', default)
    _need(type(limit) is int and 1 <= limit <= maximum and len(ids) <= limit, 'Invalid/exceeded page limit')
    if 'after' in params:
        _check(params['after'], _ref('id'))
        _need(all(x > params['after'] for x in ids), 'Page is not exclusive after cursor')
    if next_after is not None:
        _check(next_after, _ref('id'))
        _need(bool(ids) and next_after == ids[-1] and len(ids) == limit, 'Invalid continuation cursor')


def _ticket(ticket):
    _need(ticket['epoch'] != '0' * 32, 'Save ticket epoch must be nonzero')
    _need(ticket['committed_tick'] >= ticket['requested_tick'], 'Save ticket committed before request')
    state, kind = ticket['state'], ticket['kind']
    if state == 0:
        _need(kind == 0 and all(ticket[k] == 0 for k in ('requested_tick', 'committed_tick', 'generation', 'error_code', 'restored_tick'))
              and not ticket['recovered'] and ticket['diagnostic'] == '' and ticket['restored_epoch'] is None,
              'Expired ticket has operation/completion data')
    else:
        _need(kind in (1, 2), 'Active save ticket lacks operation kind')
    if state == 3:
        _need(ticket['generation'] > 0 and ticket['error_code'] == 0, 'Successful save completion generation/error')
        _need(kind != 1 or not ticket['recovered'], 'Successful write cannot be recovered load')
    if state == 4:
        _need(ticket['error_code'] != 0, 'Failed save completion lacks error code')
    if ticket['restored_epoch'] is not None:
        _need(ticket['kind'] == 2 and ticket['state'] == 3 and ticket['restored_epoch'] not in ('0' * 32, ticket['epoch']),
              'Restore epoch needs successful load')
    if ticket['kind'] == 2 and ticket['state'] == 3:
        _need(ticket['restored_epoch'] is not None, 'Successful load lacks restore epoch')


def validate_workflow_result(method, result, params=None, context=None):
    """Check a selected successful result; return it unchanged, never perform I/O.

    Request joins run when params is supplied. Context is optional trusted schema
    metadata; profile alone cannot prove which native scope produced the reply.
    Native error envelopes and effects require independent executable gates.
    """
    supplied = params is not None
    request = {} if params is None else params
    try:
        _need(type(method) is str and method in _CONTRACT['methods'], 'Unknown prospective workflow method')
        _json(request)
        _need(type(request) is dict, 'Parameters must be an object')
        context = {} if context is None else context
        _json(context)
        _need(type(context) is dict and not set(context) - {'profile', 'component_schemas', 'gameplay_schema'}, 'Unknown context fields')
        if 'profile' in context:
            _need(type(context['profile']) is str and context['profile'] in PROFILES, 'Unknown candidate scope profile')
        schemas = context.get('component_schemas', [])
        if type(schemas) is dict:
            _need(all(k == v.get('id') for k, v in schemas.items() if type(v) is dict) and all(type(v) is dict for v in schemas.values()), 'Context schema key mismatch')
            schemas = list(schemas.values())
        _need(type(schemas) is list and len(schemas) <= 64, 'Context component schemas bound')
        schema_map = {}
        for schema in schemas:
            _component_schema(schema)
            _need(schema['id'] not in schema_map, 'Duplicate context component schema')
            schema_map[schema['id']] = schema
        if 'gameplay_schema' in context:
            _check(context['gameplay_schema'], _ref('gameplay_schema'))
            _gameplay_values({'schema': context['gameplay_schema'], 'values': {}}, context['gameplay_schema'], set())
        _json(result)
        _check(result, _CONTRACT['methods'][method])
        if supplied:
            _request_joins(method, result, request)
        _semantics(method, result, request if supplied else None, schema_map, context.get('gameplay_schema'))
    except (ValueError, TypeError, KeyError, OverflowError, RecursionError, UnicodeError) as error:
        try:
            failure = ResponseContractError(str(method), str(error), result, request)
        except (ValueError, TypeError, RecursionError, UnicodeError):
            failure = ResponseContractError(str(method), str(error), result, {})
        raise failure from error
    return result


def _request_joins(method, result, params):
    required = {
        'runtime.start': ('session_id', 'revision'), 'runtime.stop': ('session_id',),
        'runtime.inspect': ('session_id',), 'runtime.components': ('session_id',),
        'runtime.entity': ('session_id', 'id'),
        'runtime.observe': ('session_id', 'tick'),
        'runtime.component.get': ('session_id', 'tick', 'id', 'type'),
        'runtime.component.query': ('session_id', 'tick', 'type'),
        'runtime.gameplay.inspect': ('session_id',),
        'runtime.gameplay.load_native': ('session_id', 'request_id', 'expected_tick', 'expected_revision', 'descriptor'),
        'runtime.step': ('session_id', 'request_id', 'expected_tick', 'ticks'),
        'runtime.ui.inspect': ('session_id', 'tick'),
        'runtime.ui.activate': ('session_id', 'request_id', 'expected_tick', 'expected_ui_revision', 'expected_control_sequence', 'expected_gameplay_revision', 'id'),
        'runtime.save.status': ('session_id',), 'runtime.save.result': ('epoch', 'sequence'),
        'save.configure': ('request_id', 'expected_generation', 'root'),
        'save.inspect': ('slot',),
        'save.write': ('request_id', 'configuration_generation', 'slot', 'expected_generation', 'session_id', 'expected_tick', 'expected_gameplay_revision'),
        'save.load': ('request_id', 'configuration_generation', 'slot', 'expected_generation', 'revision', 'expected_session_id', 'expected_tick', 'expected_gameplay_revision', 'new_session_id')}
    _need(set(required.get(method, ())) <= params.keys(), 'Missing successful-request consumer guard')
    for key in ('session_id', 'id', 'type', 'request_id', 'new_session_id', 'epoch'):
        if key in params:
            _check(params[key], _ref('id'))
    numeric = ('tick', 'revision', 'structure_revision', 'component_revision', 'gameplay_revision',
        'ui_revision', 'control_sequence', 'expected_tick', 'expected_revision',
        'expected_structure_revision', 'expected_component_revision', 'expected_gameplay_revision',
        'expected_ui_revision', 'expected_control_sequence', 'expected_material_revision',
        'configuration_generation', 'expected_generation', 'sequence', 'ticks')
    for key in numeric:
        if key in params:
            nullable = method == 'save.load' and key.startswith('expected_') and key != 'expected_generation'
            _need((nullable and params[key] is None) or _integer(params[key]), 'Successful request has non-integer ' + key)
    if 'expected_session_id' in params and params['expected_session_id'] is not None:
        _check(params['expected_session_id'], _ref('id'))
    direct = method not in ('runtime.step', 'runtime.save.status', 'runtime.save.result', 'save.write', 'save.load')
    if direct and 'session_id' in params:
        _need(result['session_id'] == params['session_id'], 'Initiating session mismatch')
    for key in ('tick', 'structure_revision', 'component_revision', 'gameplay_revision', 'ui_revision', 'control_sequence'):
        if key in params and key in result:
            _need(result[key] == params[key], 'Requested read boundary mismatch: ' + key)
    if method == 'runtime.start':
        _need(result['authored_revision'] == params['revision'], 'Start authored revision mismatch')
    if method in ('runtime.entity', 'runtime.component.get'):
        _need(result['id'] == params['id'], 'Selected entity mismatch')
    if method in ('runtime.component.get', 'runtime.component.query'):
        _need(result['type'] == params['type'], 'Selected component type mismatch')
    if method == 'runtime.gameplay.load_native':
        _need(result['tick'] == params['expected_tick'] and result['revision'] == params['expected_revision'] + 1,
              'Native load historical tick/revision mismatch')
    if method == 'runtime.step':
        _need(type(params['ticks']) is int and 1 <= params['ticks'] <= 600 and result['stepped'] == params['ticks'], 'Requested step count mismatch')
        before_load = _prestep_load(result)
        _need(before_load or (result['session_id'] == params['session_id'] and result['previous_tick'] == params['expected_tick']),
              'Step initiating boundary mismatch without prior completed load')
    if method == 'runtime.ui.activate':
        _need(result['tick'] == params['expected_tick'] and result['control_sequence'] == params['expected_control_sequence'] + 1,
              'UI activation historical acceptance boundary mismatch')
        _need(result['ui_revision'] >= params['expected_ui_revision'], 'UI revision went backwards during Control')
    if method == 'runtime.save.result':
        _need(result['epoch'] == params['epoch'] and result['sequence'] == params['sequence'], 'Save ticket identity mismatch')
    if method in ('save.inspect', 'save.write', 'save.load'):
        _need(result['slot'] == params['slot'], 'Selected save slot mismatch')
    if method == 'save.configure':
        _need(result['generation'] == params['expected_generation'] + 1 and result['write_mode'] == params.get('write_mode', 'synchronous'), 'Save configuration outcome mismatch')
        _need((result['root'] is None) == (params['root'] is None), 'Save configuration disabled/null mismatch')
    if method == 'save.write':
        _need(result['configuration_generation'] == params['configuration_generation'] and result['generation'] == params['expected_generation'] + 1,
              'Save write routing/committed generation mismatch')
    if method == 'save.load':
        if 'allow_recovery' in params:
            _need(type(params['allow_recovery']) is bool, 'Recovery consent must be Boolean')
        _need(not result['recovered'] or params.get('allow_recovery') is True,
              'Recovered restore requires explicit allow_recovery:true in the original request')
        _need(result['session_id'] == params['new_session_id'], 'Restored session differs from original requested new session')
        _need(result['generation'] == params['expected_generation'], 'Save load storage-generation mismatch')
        if not result['replayed']:
            _need(result['current_authored_revision'] == params['revision'], 'Fresh restore authored boundary mismatch')


def _prestep_load(result):
    operation = result.get('save_operation')
    return (type(operation) is dict and operation.get('kind') == 2 and operation.get('state') == 3
            and result.get('save_serviced') is True and result.get('runtime_replaced') is False
            and result.get('previous_tick') == operation.get('restored_tick'))


def _semantics(method, result, params, schemas, gameplay_schema):
    request = {} if params is None else params
    if method == 'runtime.status':
        active = result['active']
        _need(not active or result['available'], 'Unavailable runtime cannot be active')
        for key in ('session_id', 'tick', 'structure_revision', 'material_revision', 'ui_revision', 'control_sequence', 'authored_revision'):
            _need((result[key] is not None) == active, 'Inactive/active status null boundary mismatch')
    if method == 'runtime.start':
        _need(result['tick'] == 0, 'Runtime start must describe tick0')
    if method == 'runtime.inspect':
        _need(result['bodies'] <= result['entities'] and result['characters'] <= result['entities'], 'Runtime counts exceed entities')
    if 'authored_revision' in result and 'current_authored_revision' in result:
        _need(result['source_stale'] or result['authored_revision'] == result['current_authored_revision'], 'Fresh source has different revisions')
    if method in ('runtime.step', 'runtime.ui.activate'):
        _need(result['runtime_replaced'] == (result['session_id'] != result['current_session_id']), 'Replacement flag disagrees with source/current identities')
        if 'save_operation' in result:
            _ticket(result['save_operation'])
        if method == 'runtime.step':
            _need(result['tick'] == result['previous_tick'] + result['stepped'], 'Step arithmetic mismatch')
            _need(result['sound_events']['next_voice'] >= result['sound_events']['first_voice'], 'Sound-event range reversed')
            _need(result['runtime_replaced'] or result['current_tick'] == result['tick'], 'Unreplaced step current tick mismatch')
        elif not result['runtime_replaced'] and not result['replayed']:
            _need(result['current_tick'] == result['tick'] and result['current_control_sequence'] == result['control_sequence']
                  and result['current_ui_revision'] == result['ui_revision'], 'Fresh unreplaced Control boundary mismatch')
        if result['runtime_replaced']:
            op = result.get('save_operation')
            _need(type(op) is dict and op['kind'] == 2 and op['state'] == 3 and op['restored_tick'] == result['current_tick'], 'Replacement lacks completed matching load')
    if method in ('runtime.components', 'runtime.component.get'):
        values = result['schemas'] if method == 'runtime.components' else [result['schema']]
        _ids([s['id'] for s in values], 64, nonzero=True)
        for schema in values:
            _component_schema(schema)
            if schema['id'] in schemas:
                _need(_schema_same(schema, schemas[schema['id']]), 'Component schema differs from consumer context')
        if method == 'runtime.component.get':
            _need(result['schema']['id'] == result['type'], 'Returned schema has wrong component identity')
            _component_values(result['values'], result['schema'])
    if method == 'runtime.component.query':
        _page(result['entities'], result['next_after'], request if params is not None else {'limit': 256}, 256, 64)
    if method in ('runtime.gameplay.inspect', 'runtime.gameplay.load_native'):
        module = result['module']
        if method == 'runtime.gameplay.inspect' and params is not None:
            include = request.get('include_schema', False)
            _need(type(include) is bool, 'include_schema must be Boolean')
            if module is not None:
                _check(module, _CONTRACT['methods']['runtime.gameplay.load_native']['properties']['module'] if include else
                       _CONTRACT['methods']['runtime.gameplay.inspect']['properties']['module']['anyOf'][1])
            if 'fields' in request:
                selected = request['fields']
                _need(module is not None and type(selected) is list and len(selected) <= 128
                      and all(type(x) is str for x in selected) and len(set(selected)) == len(selected), 'Invalid gameplay field selector')
            else:
                selected = None
        else:
            selected = None
        if module is not None:
            schema = module.get('schema', gameplay_schema)
            if gameplay_schema is not None and 'schema' in module:
                _need(_schema_same(module['schema'], gameplay_schema, gameplay=True), 'Returned gameplay schema differs from consumer context')
            _gameplay_values(module, schema, selected)
    if method == 'runtime.observe':
        _observe(result, params, schemas)
    if method == 'runtime.ui.inspect':
        _page([e['id'] for e in result['elements']], result['next_after'], request if params is not None else {'limit': 256}, 256, 64)
        if 'after' in request:
            _need('ui_revision' in request, 'UI continuation lacks ui_revision')
        for element in result['elements']:
            button = element['kind'] == 'button'
            _need((element['action'] is not None) == button, 'UI action/kind mismatch')
            if button:
                _need(re.fullmatch('[A-Za-z0-9_.-]{1,128}', element['action']) is not None, 'Invalid button action')
            _need(button or not element['eligible'], 'Non-button UI marked eligible')
            if element['eligible']:
                _need(element['effective_visible'] and element['effective_enabled'], 'Ineligible UI visibility/enabled boundary')
    if method == 'runtime.save.status':
        _need(result['epoch'] != '0' * 32, 'Active save epoch is zero')
        if result['pending'] is not None:
            _ticket(result['pending'])
            _need(result['pending']['epoch'] == result['epoch'] and result['pending']['state'] in (1, 2), 'Pending ticket not owned/unfinished')
        restored = result['last_restore']
        if restored is not None:
            _need((restored['initiating_epoch'] == '0' * 32) == (restored['initiating_sequence'] == 0), 'Restore initiating ticket zero/sequence mismatch')
    if method == 'runtime.save.result':
        _ticket(result)
    if method == 'save.inspect':
        if not result['exists']:
            _need(result['generation'] == 0 and all(result[k] is None for k in ('current', 'previous', 'selected', 'quarantined')), 'Missing slot has committed generations')
        elif result['generation'] == 0:
            _need(all(result[k] is None for k in ('current', 'previous', 'selected', 'quarantined')), 'Uncommitted slot has generation records')
        else:
            _need(result['current'] is not None and result['selected'] is not None and result['selected']['verified'], 'Existing slot lacks verified selected generation')
            _need(result['generation'] == result['current']['generation'], 'Current save generation mismatch')
            keys = ('generation', 'sha256', 'bytes', 'verified')
            _need(any(record is not None and all(_literal(result['selected'][k], record[k]) for k in keys)
                      for record in (result['current'], result['previous'])), 'Selected save is not retained current/previous')
    if method == 'save.load':
        _need(result['selected_generation'] <= result['generation'], 'Selected restore generation is newer than storage')
        if not result['recovered']:
            _need(result['selected_generation'] == result['generation'], 'Unrecovered restore uses another generation')
        _need('upgrade' not in result or (params is None or 'upgrade' in request), 'Unrequested upgrade metadata')


def _observe(result, params, schemas):
    rows = result['entities']
    _ids([row['id'] for row in rows], 96)
    types = result['component_types']
    _ids([item['type'] for item in types], 4, nonzero=True)
    selections = None
    if params is not None:
        fields = params.get('entity_fields', list(DEFAULT_FIELDS))
        known = set(_CONTRACT['methods']['runtime.entity']['properties']) - {'session_id', 'tick', 'structure_revision', 'id', 'locomotion'}
        _need(type(fields) is list and len(fields) <= 13 and all(type(f) is str and f in known for f in fields)
              and len(set(fields)) == len(fields), 'Invalid entity projection selector')
        selected = params.get('components', [])
        _need(type(selected) is list and len(selected) <= 4, 'Component projection selector bound')
        selections = {}
        for item in selected:
            _need(type(item) is dict and set(item) <= {'type', 'fields'} and 'type' in item, 'Invalid component selector')
            _check(item['type'], _ref('id'))
            _need(item['type'] not in selections, 'Duplicate component selector')
            if 'fields' in item:
                f = item['fields']
                _need(type(f) is list and len(f) <= 32, 'Invalid selected component fields')
                _ids(sorted(f), 32, nonzero=True)
            selections[item['type']] = set(item['fields']) if 'fields' in item else None
        include = params.get('include_schemas', False)
        _need(type(include) is bool, 'include_schemas must be Boolean')
        _need([t['type'] for t in types] == sorted(selections), 'Returned component types differ from selectors')
        explicit = params.get('ids', [])
        _need(type(explicit) is list and len(explicit) <= 32, 'Explicit observation ID bound')
        _ids(sorted(explicit), 32)
        _need(bool(explicit) or 'query' in params, 'Observation needs IDs/query')
        for row in rows:
            _need(set(row['state']) == set(fields), 'Returned entity projection differs from selector')
    for item in types:
        if params is not None:
            _need(('schema' in item) == include, 'Schema inclusion differs from selector')
        if 'schema' in item:
            schema = item['schema']
            _component_schema(schema)
            _need(schema['id'] == item['type'] and schema['fingerprint'] == item['fingerprint'], 'Projection schema identity/fingerprint mismatch')
            if schema['id'] in schemas:
                _need(_schema_same(schema, schemas[schema['id']]), 'Observed schema differs from consumer context')
            schemas = {**schemas, item['type']: schema}
        elif item['type'] in schemas:
            _need(item['fingerprint'] == schemas[item['type']]['fingerprint'], 'Projection fingerprint differs from consumer context')
    page = result['query']
    if params is not None:
        _need((page is not None) == ('query' in params), 'Query omission/null variant mismatch')
    if page is not None:
        query = {'limit': 64} if params is None else params['query']
        if params is not None:
            _need(type(query) is dict and set(query) <= {'type', 'after', 'limit'} and query.get('type') == page['type'], 'Query selector identity mismatch')
            if 'after' in query:
                _need({'structure_revision', 'component_revision'} <= params.keys(), 'Observation continuation lacks pins')
        _page(page['entities'], page['next_after'], query, 64, 32)
    if params is not None:
        union = sorted(set(explicit) | (set(page['entities']) if page is not None else set()))
        _need([row['id'] for row in rows] == union, 'Observation rows differ from explicit/query union')
    for row in rows:
        _need(set(row['components']) == {t['type'] for t in types}, 'Component projection keys mismatch')
        for type_id, values in row['components'].items():
            selected = None if selections is None else selections[type_id]
            _component_values(values, schemas.get(type_id), selected)
            if page is not None and row['id'] in page['entities'] and type_id == page['type']:
                _need(values is not None, 'Queried component membership is absent')
