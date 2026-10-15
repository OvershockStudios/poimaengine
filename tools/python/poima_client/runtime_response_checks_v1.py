# SPDX-License-Identifier: Apache-2.0
"""Version-one runtime result checks over an explicitly supplied frozen manifest.

Each instance owns its detached schema and scope profiles. Validation performs
no I/O, changes no caller data, and never switches a global validation schema.
The policy is intentionally bounded; it is not a general JSON Schema engine.
An instance does not make its supplied manifest a released/stable contract.
"""
import json
import math
import re
from types import MappingProxyType

from .responses import ResponseContractError

MAX_INTEGER = 9007199254740991
DEFAULT_FIELDS = ('world_matrix', 'layout', 'velocity', 'has_body', 'is_character',
                  'ground', 'yaw', 'pitch')


def _freeze(value):
    if type(value) is dict:
        return MappingProxyType({key: _freeze(child) for key, child in value.items()})
    if type(value) is list:
        return tuple(_freeze(child) for child in value)
    return value


def _thaw(value):
    if isinstance(value, MappingProxyType):
        return {key: _thaw(child) for key, child in value.items()}
    if type(value) is tuple:
        return [_thaw(child) for child in value]
    return value


class ResultChecksV1:
    """Detached schema and immutable profile selection for v1 result checks."""

    __slots__ = ('_contract', '_profiles')

    def __init__(self, contract, profiles=None):
        self._json(contract)
        self._need(type(contract) is dict and type(contract.get('methods')) is dict
                   and type(contract.get('$defs')) is dict, 'Invalid result contract')
        selected = contract.get('profiles', ()) if profiles is None else profiles
        self._need(type(selected) in (list, tuple) and all(type(x) is str for x in selected)
                   and len(set(selected)) == len(selected), 'Invalid result scope profiles')
        self._contract = _freeze(contract)
        self._profiles = tuple(selected)

    def contract(self):
        """Return a detached JSON copy, not the instance's internal schema."""
        return _thaw(self._contract)

    def _ref(self, name):
        return {'$ref': '#/$defs/' + name}

    def _need(self, condition, reason):
        if not condition:
            raise ValueError(reason)

    def _json(self, value):
        pending = [(value, 0)]
        nodes = 0
        while pending:
            item, depth = pending.pop()
            nodes += 1
            self._need(depth <= 64 and nodes <= 100000, 'JSON exceeds depth/node bound')
            self._need(type(item) in (dict, list, str, bool, int, float, type(None)), 'Non-JSON value')
            if type(item) is dict:
                self._need(all(type(k) is str for k in item), 'JSON object keys must be strings')
                pending.extend((v, depth + 1) for v in item.values())
            elif type(item) is list:
                pending.extend((v, depth + 1) for v in item)
            elif type(item) is float:
                self._need(math.isfinite(item), 'Nonfinite JSON number')
        encoded = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8')
        self._need(len(encoded) <= 16 * 1024 * 1024, 'JSON exceeds encoded byte bound')

    def _literal(self, a, b):
        if isinstance(b, MappingProxyType) or type(b) is tuple:
            b = _thaw(b)
        return type(a) is type(b) and a == b

    def _check(self, value, schema, path='$', depth=0):
        self._need(depth <= 64, path + ': schema nesting exceeds64')
        if '$ref' in schema:
            return self._check(value, self._contract['$defs'][schema['$ref'].split('/')[-1]], path, depth + 1)
        if 'anyOf' in schema:
            accepted = False
            for branch in schema['anyOf']:
                try:
                    self._check(value, branch, path, depth + 1)
                    accepted = True
                    break
                except ValueError:
                    pass
            self._need(accepted, path + ': no accepted variant')
        for key in ('const', 'enum'):
            if key in schema:
                options = [schema[key]] if key == 'const' else schema[key]
                self._need(any(self._literal(value, x) for x in options), path + ': unexpected literal/enum type')
        kind = schema.get('type')
        kinds = {'object': dict, 'array': list, 'string': str, 'boolean': bool,
                 'integer': int, 'null': type(None)}
        if kind:
            self._need(type(value) in (int, float) if kind == 'number' else type(value) is kinds[kind], path + ': expected ' + kind)
        if type(value) in (int, float):
            for key, predicate in (('minimum', lambda x, y: x >= y), ('maximum', lambda x, y: x <= y),
                                   ('exclusiveMinimum', lambda x, y: x > y)):
                if key in schema:
                    self._need(predicate(value, schema[key]), path + ': numeric bound')
        if type(value) is str:
            self._need(len(value) >= schema.get('minLength', 0) and len(value) <= schema.get('maxLength', len(value)), path + ': text length')
            if 'pattern' in schema:
                self._need(re.fullmatch(schema['pattern'], value) is not None, path + ': invalid identifier')
        if type(value) is list:
            self._need(schema.get('minItems', 0) <= len(value) <= schema.get('maxItems', len(value)), path + ': array bound')
            if 'items' in schema:
                for i, item in enumerate(value):
                    self._check(item, schema['items'], path + '[' + str(i) + ']', depth + 1)
        if type(value) is dict:
            self._need(set(schema.get('required', [])) <= set(value), path + ': missing required field')
            known = schema.get('properties', {})
            if schema.get('additionalProperties') is False:
                self._need(set(value) <= set(known), path + ': unexpected projected field')
            for key, child in known.items():
                if key in value:
                    self._check(value[key], child, path + '.' + key, depth + 1)

    def _integer(self, value):
        return type(value) is int and 0 <= value <= MAX_INTEGER

    def _ids(self, values, maximum, *, nonzero=False):
        self._need(type(values) is list and len(values) <= maximum, 'Identifier array bound')
        for value in values:
            self._check(value, self._ref('id'))
            self._need(not nonzero or value != '0' * 32, 'Zero registered identity')
        self._need(values == sorted(set(values)), 'Identifiers must be sorted and unique')

    def _typed(self, value, kind):
        if kind == 'int32':
            self._need(type(value) is int and -2147483648 <= value <= 2147483647, 'int32 wire type/range')
        elif kind == 'int64':
            self._need(type(value) is str and re.fullmatch(r'-?(0|[1-9][0-9]*)', value) is not None,
                  'int64 requires canonical decimal text')
            self._need(len(value) <= 20 and -(2**63) <= int(value) < 2**63 and str(int(value)) == value, 'int64 range/canonical form')
        elif kind in ('float32', 'float64'):
            self._need(type(value) in (int, float), 'Floating field needs numeric wire value')
            self._need(abs(value) <= (3.4028234663852886e38 if kind == 'float32' else 1.7976931348623157e308), 'Floating field overflow')
        else:
            self._need(kind == 'entity', 'Unsupported scalar kind')
            self._check(value, self._ref('id'))

    def _schema_same(self, a, b, gameplay=False):
        keys = ('identity', 'bytes') if gameplay else ('id', 'name', 'version', 'fingerprint')
        if not all(self._literal(a.get(k), b.get(k)) for k in keys):
            return False
        field_keys = ('name', 'kind', 'offset', 'bytes') if gameplay else ('id', 'name', 'kind', 'default', 'unit', 'element_kind', 'capacity')
        if len(a['fields']) != len(b['fields']):
            return False
        for x, y in zip(a['fields'], b['fields']):
            for key in field_keys:
                if (key in x) != (key in y) or (key in x and not self._json_same(x[key], y[key])):
                    return False
        if gameplay:
            ac, bc = a.get('components', []), b.get('components', [])
            return len(ac) == len(bc) and all(self._schema_same(x, y) for x, y in zip(ac, bc))
        return True

    def _json_same(self, a, b):
        if type(a) is not type(b):
            return False
        if type(a) is dict:
            return a.keys() == b.keys() and all(self._json_same(a[k], b[k]) for k in a)
        if type(a) is list:
            return len(a) == len(b) and all(self._json_same(x, y) for x, y in zip(a, b))
        return a == b

    def _label(self, value, maximum, empty=False):
        return ((empty or bool(value)) and len(value.encode('utf-8')) <= maximum
                and not any(ord(x) < 32 or ord(x) == 127 for x in value))

    def _component_schema(self, schema, declaration=False):
        self._check(schema, self._ref('component_declaration' if declaration else 'component_schema'))
        self._need(schema['id'] != '0' * 32 and self._label(schema['name'], 64), 'Component identity/name')
        fields = schema['fields']
        identities = [f['id'] for f in fields]
        self._ids(sorted(identities) if declaration else identities, 32, nonzero=True)
        self._need(len({f['name'] for f in fields}) == len(fields), 'Duplicate component field name')
        total, arrays = 0, False
        for f in fields:
            self._need(self._label(f['name'], 64) and self._label(f.get('unit', ''), 24, empty=True), 'Component name/unit bound')
            if f['kind'] == 'array':
                arrays = True
                self._need(schema['version'] == 2 and {'capacity', 'element_kind'} <= f.keys() and f['default'] == [], 'Array schema/default')
                total += (f['capacity'] + 1) * 16
            else:
                self._need(not {'capacity', 'element_kind'} & f.keys(), 'Scalar contains array metadata')
                self._typed(f['default'], f['kind'])
                self._need(f['kind'] != 'entity' or f['default'] == '0' * 32, 'Entity default must be unset')
                total += 16
        self._need(total <= 512 and (schema['version'] == 1 or arrays), 'Component payload/version bound')

    def _component_values(self, values, schema, selected=None):
        if schema is not None and selected is not None:
            self._need(set(selected) <= {f['id'] for f in schema['fields']}, 'Unknown selected component field')
        if values is None:
            return
        self._need(type(values) is dict, 'Component values must be object or null')
        fields = {f['id']: f for f in schema['fields']} if schema is not None else None
        if fields is not None and selected is None:
            selected = set(fields)
        if selected is not None:
            self._need(set(values) == set(selected), 'Component values differ from selected fields')
        for key, value in values.items():
            self._check(key, self._ref('id'))
            if fields is None:
                self._need(type(value) in (int, float, str, list), 'Unsupported unschematized component value')
                if type(value) is list:
                    self._need(len(value) <= 31 and all(type(x) in (int, float, str) for x in value), 'Unschematized array wire shape')
                continue
            self._need(key in fields, 'Unknown component field')
            f = fields[key]
            if f['kind'] == 'array':
                self._need(type(value) is list and len(value) <= f['capacity'], 'Component array capacity/type')
                for item in value:
                    self._typed(item, f['element_kind'])
            else:
                self._typed(value, f['kind'])

    def _gameplay_values(self, module, schema, selected=None):
        values = module['values']
        fields = None
        if schema is not None:
            self._check(schema, self._ref('gameplay_schema'))
            fields = {f['name']: f for f in schema['fields']}
            self._need(len(fields) == len(schema['fields']), 'Duplicate gameplay field')
            spans = set()
            for f in fields.values():
                size = {'int32': 4, 'int64': 8, 'float32': 4, 'float64': 8, 'entity': 16}[f['kind']]
                self._need(f['bytes'] == size and f['offset'] + size <= schema['bytes'], 'Gameplay field layout')
                occupied = set(range(f['offset'], f['offset'] + size))
                self._need(not spans & occupied, 'Overlapping gameplay field layout')
                spans |= occupied
            if 'schema' in module:
                self._need(self._schema_same(module['schema'], schema, gameplay=True), 'Gameplay schema differs from consumer context')
            else:
                self._need(module['identity'] == schema['identity'], 'Gameplay identity differs from consumer schema')
            if selected is None:
                selected = set(fields)
        if selected is not None:
            self._need(set(values) == set(selected), 'Gameplay values differ from requested fields')
        self._need(len(values) <= 128, 'Gameplay values field bound')
        for name, value in values.items():
            self._need(1 <= len(name.encode('utf-8')) <= 64, 'Gameplay field name bound')
            if fields is not None:
                self._need(name in fields, 'Unknown gameplay field')
                self._typed(value, fields[name]['kind'])
            else:
                self._need(type(value) in (int, float, str), 'Unsupported unschematized gameplay value')
        if schema is not None:
            components = schema.get('components', [])
            self._need(len({c['id'] for c in components}) == len(components), 'Duplicate gameplay component declaration')
            for component in components:
                self._component_schema(component, declaration=True)

    def _page(self, ids, next_after, params, maximum, default):
        self._ids(ids, maximum)
        limit = params.get('limit', default)
        self._need(type(limit) is int and 1 <= limit <= maximum and len(ids) <= limit, 'Invalid/exceeded page limit')
        if 'after' in params:
            self._check(params['after'], self._ref('id'))
            self._need(all(x > params['after'] for x in ids), 'Page is not exclusive after cursor')
        if next_after is not None:
            self._check(next_after, self._ref('id'))
            self._need(bool(ids) and next_after == ids[-1] and len(ids) == limit, 'Invalid continuation cursor')

    def _ticket(self, ticket):
        self._need(ticket['epoch'] != '0' * 32, 'Save ticket epoch must be nonzero')
        self._need(ticket['committed_tick'] >= ticket['requested_tick'], 'Save ticket committed before request')
        state, kind = ticket['state'], ticket['kind']
        if state == 0:
            self._need(kind == 0 and all(ticket[k] == 0 for k in ('requested_tick', 'committed_tick', 'generation', 'error_code', 'restored_tick'))
                  and not ticket['recovered'] and ticket['diagnostic'] == '' and ticket['restored_epoch'] is None,
                  'Expired ticket has operation/completion data')
        else:
            self._need(kind in (1, 2), 'Active save ticket lacks operation kind')
        if state == 3:
            self._need(ticket['generation'] > 0 and ticket['error_code'] == 0, 'Successful save completion generation/error')
            self._need(kind != 1 or not ticket['recovered'], 'Successful write cannot be recovered load')
        if state == 4:
            self._need(ticket['error_code'] != 0, 'Failed save completion lacks error code')
        if ticket['restored_epoch'] is not None:
            self._need(ticket['kind'] == 2 and ticket['state'] == 3 and ticket['restored_epoch'] not in ('0' * 32, ticket['epoch']),
                  'Restore epoch needs successful load')
        if ticket['kind'] == 2 and ticket['state'] == 3:
            self._need(ticket['restored_epoch'] is not None, 'Successful load lacks restore epoch')

    def validate(self, method, result, params=None, context=None):
        """Check a selected successful result; return it unchanged, never perform I/O.

        Request joins run when params is supplied. Context is optional trusted schema
        metadata; profile alone cannot prove which native scope produced the reply.
        Native error envelopes and effects require independent executable gates.
        """
        supplied = params is not None
        request = {} if params is None else params
        try:
            self._need(type(method) is str and method in self._contract['methods'], 'Unknown prospective workflow method')
            self._json(request)
            self._need(type(request) is dict, 'Parameters must be an object')
            context = {} if context is None else context
            self._json(context)
            self._need(type(context) is dict and not set(context) - {'profile', 'component_schemas', 'gameplay_schema'}, 'Unknown context fields')
            if 'profile' in context:
                self._need(type(context['profile']) is str and context['profile'] in self._profiles, 'Unknown candidate scope profile')
            schemas = context.get('component_schemas', [])
            if type(schemas) is dict:
                self._need(all(k == v.get('id') for k, v in schemas.items() if type(v) is dict) and all(type(v) is dict for v in schemas.values()), 'Context schema key mismatch')
                schemas = list(schemas.values())
            self._need(type(schemas) is list and len(schemas) <= 64, 'Context component schemas bound')
            schema_map = {}
            for schema in schemas:
                self._component_schema(schema)
                self._need(schema['id'] not in schema_map, 'Duplicate context component schema')
                schema_map[schema['id']] = schema
            if 'gameplay_schema' in context:
                self._check(context['gameplay_schema'], self._ref('gameplay_schema'))
                self._gameplay_values({'schema': context['gameplay_schema'], 'values': {}}, context['gameplay_schema'], set())
            self._json(result)
            self._check(result, self._contract['methods'][method])
            if supplied:
                self._request_joins(method, result, request)
            self._semantics(method, result, request if supplied else None, schema_map, context.get('gameplay_schema'))
        except (ValueError, TypeError, KeyError, OverflowError, RecursionError, UnicodeError) as error:
            try:
                failure = ResponseContractError(str(method), str(error), result, request)
            except (ValueError, TypeError, RecursionError, UnicodeError):
                failure = ResponseContractError(str(method), str(error), result, {})
            raise failure from error
        return result

    def _request_joins(self, method, result, params):
        required = {
            'runtime.start': ('session_id', 'revision'), 'runtime.stop': ('session_id',),
            'runtime.inspect': ('session_id',), 'runtime.components': ('session_id',),
            'runtime.entity': ('session_id', 'id'),
            'runtime.raycast': ('session_id', 'tick', 'origin', 'direction', 'distance'),
            'runtime.hit_test': ('session_id', 'tick', 'origin', 'direction', 'distance'),
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
        self._need(set(required.get(method, ())) <= params.keys(), 'Missing successful-request consumer guard')
        for key in ('session_id', 'id', 'type', 'request_id', 'new_session_id', 'epoch'):
            if key in params:
                self._check(params[key], self._ref('id'))
        numeric = ('tick', 'revision', 'structure_revision', 'component_revision', 'gameplay_revision',
            'ui_revision', 'control_sequence', 'expected_tick', 'expected_revision',
            'expected_structure_revision', 'expected_component_revision', 'expected_gameplay_revision',
            'expected_ui_revision', 'expected_control_sequence', 'expected_material_revision',
            'configuration_generation', 'expected_generation', 'sequence', 'ticks')
        for key in numeric:
            if key in params:
                nullable = method == 'save.load' and key.startswith('expected_') and key != 'expected_generation'
                self._need((nullable and params[key] is None) or self._integer(params[key]), 'Successful request has non-integer ' + key)
        if 'expected_session_id' in params and params['expected_session_id'] is not None:
            self._check(params['expected_session_id'], self._ref('id'))
        direct = method not in ('runtime.step', 'runtime.save.status', 'runtime.save.result', 'save.write', 'save.load')
        if direct and 'session_id' in params:
            self._need(result['session_id'] == params['session_id'], 'Initiating session mismatch')
        for key in ('tick', 'structure_revision', 'component_revision', 'gameplay_revision', 'ui_revision', 'control_sequence'):
            if key in params and key in result:
                self._need(result[key] == params[key], 'Requested read boundary mismatch: ' + key)
        if method in ('runtime.raycast', 'runtime.hit_test'):
            self._query_request(method, result, params)
        if method == 'runtime.start':
            self._need(result['authored_revision'] == params['revision'], 'Start authored revision mismatch')
        if method in ('runtime.entity', 'runtime.component.get'):
            self._need(result['id'] == params['id'], 'Selected entity mismatch')
        if method in ('runtime.component.get', 'runtime.component.query'):
            self._need(result['type'] == params['type'], 'Selected component type mismatch')
        if method == 'runtime.gameplay.load_native':
            self._need(result['tick'] == params['expected_tick'] and result['revision'] == params['expected_revision'] + 1,
                  'Native load historical tick/revision mismatch')
        if method == 'runtime.step':
            self._need(type(params['ticks']) is int and 1 <= params['ticks'] <= 600 and result['stepped'] == params['ticks'], 'Requested step count mismatch')
            before_load = self._prestep_load(result)
            self._need(before_load or (result['session_id'] == params['session_id'] and result['previous_tick'] == params['expected_tick']),
                  'Step initiating boundary mismatch without prior completed load')
        if method == 'runtime.ui.activate':
            self._need(result['tick'] == params['expected_tick'] and result['control_sequence'] == params['expected_control_sequence'] + 1,
                  'UI activation historical acceptance boundary mismatch')
            self._need(result['ui_revision'] >= params['expected_ui_revision'], 'UI revision went backwards during Control')
        if method == 'runtime.save.result':
            self._need(result['epoch'] == params['epoch'] and result['sequence'] == params['sequence'], 'Save ticket identity mismatch')
        if method in ('save.inspect', 'save.write', 'save.load'):
            self._need(result['slot'] == params['slot'], 'Selected save slot mismatch')
        if method == 'save.configure':
            self._need(result['generation'] == params['expected_generation'] + 1 and result['write_mode'] == params.get('write_mode', 'synchronous'), 'Save configuration outcome mismatch')
            self._need((result['root'] is None) == (params['root'] is None), 'Save configuration disabled/null mismatch')
        if method == 'save.write':
            self._need(result['configuration_generation'] == params['configuration_generation'] and result['generation'] == params['expected_generation'] + 1,
                  'Save write routing/committed generation mismatch')
        if method == 'save.load':
            if 'allow_recovery' in params:
                self._need(type(params['allow_recovery']) is bool, 'Recovery consent must be Boolean')
            self._need(not result['recovered'] or params.get('allow_recovery') is True,
                  'Recovered restore requires explicit allow_recovery:true in the original request')
            self._need(result['session_id'] == params['new_session_id'], 'Restored session differs from original requested new session')
            self._need(result['generation'] == params['expected_generation'], 'Save load storage-generation mismatch')
            if not result['replayed']:
                self._need(result['current_authored_revision'] == params['revision'], 'Fresh restore authored boundary mismatch')

    def _prestep_load(self, result):
        operation = result.get('save_operation')
        return (type(operation) is dict and operation.get('kind') == 2 and operation.get('state') == 3
                and result.get('save_serviced') is True and result.get('runtime_replaced') is False
                and result.get('previous_tick') == operation.get('restored_tick'))

    def _semantics(self, method, result, params, schemas, gameplay_schema):
        request = {} if params is None else params
        if method in ('runtime.raycast', 'runtime.hit_test'):
            self._query_hit(method, result)
        if method == 'runtime.status':
            active = result['active']
            self._need(not active or result['available'], 'Unavailable runtime cannot be active')
            for key in ('session_id', 'tick', 'structure_revision', 'material_revision', 'ui_revision', 'control_sequence', 'authored_revision'):
                self._need((result[key] is not None) == active, 'Inactive/active status null boundary mismatch')
        if method == 'runtime.start':
            self._need(result['tick'] == 0, 'Runtime start must describe tick0')
        if method == 'runtime.inspect':
            self._need(result['bodies'] <= result['entities'] and result['characters'] <= result['entities'], 'Runtime counts exceed entities')
        if 'authored_revision' in result and 'current_authored_revision' in result:
            self._need(result['source_stale'] or result['authored_revision'] == result['current_authored_revision'], 'Fresh source has different revisions')
        if method in ('runtime.step', 'runtime.ui.activate'):
            self._need(result['runtime_replaced'] == (result['session_id'] != result['current_session_id']), 'Replacement flag disagrees with source/current identities')
            if 'save_operation' in result:
                self._ticket(result['save_operation'])
            if method == 'runtime.step':
                self._need(result['tick'] == result['previous_tick'] + result['stepped'], 'Step arithmetic mismatch')
                self._need(result['sound_events']['next_voice'] >= result['sound_events']['first_voice'], 'Sound-event range reversed')
                self._need(result['runtime_replaced'] or result['current_tick'] == result['tick'], 'Unreplaced step current tick mismatch')
            elif not result['runtime_replaced'] and not result['replayed']:
                self._need(result['current_tick'] == result['tick'] and result['current_control_sequence'] == result['control_sequence']
                      and result['current_ui_revision'] == result['ui_revision'], 'Fresh unreplaced Control boundary mismatch')
            if result['runtime_replaced']:
                op = result.get('save_operation')
                self._need(type(op) is dict and op['kind'] == 2 and op['state'] == 3 and op['restored_tick'] == result['current_tick'], 'Replacement lacks completed matching load')
        if method in ('runtime.components', 'runtime.component.get'):
            values = result['schemas'] if method == 'runtime.components' else [result['schema']]
            self._ids([s['id'] for s in values], 64, nonzero=True)
            for schema in values:
                self._component_schema(schema)
                if schema['id'] in schemas:
                    self._need(self._schema_same(schema, schemas[schema['id']]), 'Component schema differs from consumer context')
            if method == 'runtime.component.get':
                self._need(result['schema']['id'] == result['type'], 'Returned schema has wrong component identity')
                self._component_values(result['values'], result['schema'])
        if method == 'runtime.component.query':
            self._page(result['entities'], result['next_after'], request if params is not None else {'limit': 256}, 256, 64)
        if method in ('runtime.gameplay.inspect', 'runtime.gameplay.load_native'):
            module = result['module']
            if method == 'runtime.gameplay.inspect' and params is not None:
                include = request.get('include_schema', False)
                self._need(type(include) is bool, 'include_schema must be Boolean')
                if module is not None:
                    self._check(module, self._contract['methods']['runtime.gameplay.inspect']['properties']['module']['anyOf'][0] if include else
                           self._contract['methods']['runtime.gameplay.inspect']['properties']['module']['anyOf'][1])
                if 'fields' in request:
                    selected = request['fields']
                    self._need(module is not None and type(selected) is list and len(selected) <= 128
                          and all(type(x) is str for x in selected) and len(set(selected)) == len(selected), 'Invalid gameplay field selector')
                else:
                    selected = None
            else:
                selected = None
            if module is not None:
                schema = module.get('schema', gameplay_schema)
                if gameplay_schema is not None and 'schema' in module:
                    self._need(self._schema_same(module['schema'], gameplay_schema, gameplay=True), 'Returned gameplay schema differs from consumer context')
                self._gameplay_values(module, schema, selected)
        if method == 'runtime.observe':
            self._observe(result, params, schemas)
        if method == 'runtime.ui.inspect':
            self._page([e['id'] for e in result['elements']], result['next_after'], request if params is not None else {'limit': 256}, 256, 64)
            if 'after' in request:
                self._need('ui_revision' in request, 'UI continuation lacks ui_revision')
            for element in result['elements']:
                button = element['kind'] == 'button'
                self._need((element['action'] is not None) == button, 'UI action/kind mismatch')
                if button:
                    self._need(re.fullmatch('[A-Za-z0-9_.-]{1,128}', element['action']) is not None, 'Invalid button action')
                self._need(button or not element['eligible'], 'Non-button UI marked eligible')
                if element['eligible']:
                    self._need(element['effective_visible'] and element['effective_enabled'], 'Ineligible UI visibility/enabled boundary')
        if method == 'runtime.save.status':
            self._need(result['epoch'] != '0' * 32, 'Active save epoch is zero')
            if result['pending'] is not None:
                self._ticket(result['pending'])
                self._need(result['pending']['epoch'] == result['epoch'] and result['pending']['state'] in (1, 2), 'Pending ticket not owned/unfinished')
            restored = result['last_restore']
            if restored is not None:
                self._need((restored['initiating_epoch'] == '0' * 32) == (restored['initiating_sequence'] == 0), 'Restore initiating ticket zero/sequence mismatch')
        if method == 'runtime.save.result':
            self._ticket(result)
        if method == 'save.inspect':
            if not result['exists']:
                self._need(result['generation'] == 0 and all(result[k] is None for k in ('current', 'previous', 'selected', 'quarantined')), 'Missing slot has committed generations')
            elif result['generation'] == 0:
                self._need(all(result[k] is None for k in ('current', 'previous', 'selected', 'quarantined')), 'Uncommitted slot has generation records')
            else:
                self._need(result['current'] is not None and result['selected'] is not None and result['selected']['verified'], 'Existing slot lacks verified selected generation')
                self._need(result['generation'] == result['current']['generation'], 'Current save generation mismatch')
                keys = ('generation', 'sha256', 'bytes', 'verified')
                self._need(any(record is not None and all(self._literal(result['selected'][k], record[k]) for k in keys)
                          for record in (result['current'], result['previous'])), 'Selected save is not retained current/previous')
        if method == 'save.load':
            self._need(result['selected_generation'] <= result['generation'], 'Selected restore generation is newer than storage')
            if not result['recovered']:
                self._need(result['selected_generation'] == result['generation'], 'Unrecovered restore uses another generation')
            self._need('upgrade' not in result or (params is None or 'upgrade' in request), 'Unrequested upgrade metadata')

    def _observe(self, result, params, schemas):
        rows = result['entities']
        self._ids([row['id'] for row in rows], 96)
        types = result['component_types']
        self._ids([item['type'] for item in types], 4, nonzero=True)
        selections = None
        if params is not None:
            fields = params.get('entity_fields', list(DEFAULT_FIELDS))
            known = set(self._contract['methods']['runtime.entity']['properties']) - {'session_id', 'tick', 'structure_revision', 'id', 'locomotion'}
            self._need(type(fields) is list and len(fields) <= 13 and all(type(f) is str and f in known for f in fields)
                  and len(set(fields)) == len(fields), 'Invalid entity projection selector')
            selected = params.get('components', [])
            self._need(type(selected) is list and len(selected) <= 4, 'Component projection selector bound')
            selections = {}
            for item in selected:
                self._need(type(item) is dict and set(item) <= {'type', 'fields'} and 'type' in item, 'Invalid component selector')
                self._check(item['type'], self._ref('id'))
                self._need(item['type'] not in selections, 'Duplicate component selector')
                if 'fields' in item:
                    f = item['fields']
                    self._need(type(f) is list and len(f) <= 32, 'Invalid selected component fields')
                    self._ids(sorted(f), 32, nonzero=True)
                selections[item['type']] = set(item['fields']) if 'fields' in item else None
            include = params.get('include_schemas', False)
            self._need(type(include) is bool, 'include_schemas must be Boolean')
            self._need([t['type'] for t in types] == sorted(selections), 'Returned component types differ from selectors')
            explicit = params.get('ids', [])
            self._need(type(explicit) is list and len(explicit) <= 32, 'Explicit observation ID bound')
            self._ids(sorted(explicit), 32)
            self._need(bool(explicit) or 'query' in params, 'Observation needs IDs/query')
            for row in rows:
                self._need(set(row['state']) == set(fields), 'Returned entity projection differs from selector')
        for item in types:
            if params is not None:
                self._need(('schema' in item) == include, 'Schema inclusion differs from selector')
            if 'schema' in item:
                schema = item['schema']
                self._component_schema(schema)
                self._need(schema['id'] == item['type'] and schema['fingerprint'] == item['fingerprint'], 'Projection schema identity/fingerprint mismatch')
                if schema['id'] in schemas:
                    self._need(self._schema_same(schema, schemas[schema['id']]), 'Observed schema differs from consumer context')
                schemas = {**schemas, item['type']: schema}
            elif item['type'] in schemas:
                self._need(item['fingerprint'] == schemas[item['type']]['fingerprint'], 'Projection fingerprint differs from consumer context')
        page = result['query']
        if params is not None:
            self._need((page is not None) == ('query' in params), 'Query omission/null variant mismatch')
        if page is not None:
            query = {'limit': 64} if params is None else params['query']
            if params is not None:
                self._need(type(query) is dict and set(query) <= {'type', 'after', 'limit'} and query.get('type') == page['type'], 'Query selector identity mismatch')
                if 'after' in query:
                    self._need({'structure_revision', 'component_revision'} <= params.keys(), 'Observation continuation lacks pins')
            self._page(page['entities'], page['next_after'], query, 64, 32)
        if params is not None:
            union = sorted(set(explicit) | (set(page['entities']) if page is not None else set()))
            self._need([row['id'] for row in rows] == union, 'Observation rows differ from explicit/query union')
        for row in rows:
            self._need(set(row['components']) == {t['type'] for t in types}, 'Component projection keys mismatch')
            for type_id, values in row['components'].items():
                selected = None if selections is None else selections[type_id]
                self._component_values(values, schemas.get(type_id), selected)
                if page is not None and row['id'] in page['entities'] and type_id == page['type']:
                    self._need(values is not None, 'Queried component membership is absent')

    def _query_request(self, method, result, params):
        self._need(not set(params) - {'session_id', 'tick', 'origin', 'direction', 'distance', 'ignore'},
                   'Unknown ray request field')
        for name in ('origin', 'direction'):
            value = params[name]
            self._need(type(value) is list and len(value) == 3 and
                       all(type(x) in (int, float) and abs(x) <= 1e6 for x in value),
                       'Ray ' + name + ' must be a bounded three-vector')
        distance = params['distance']
        self._need(type(distance) in (int, float) and .001 <= distance <= 10000,
                   'Ray distance must be .001..10000')
        length_squared = sum(x * x for x in params['direction'])
        self._need(length_squared >= 1e-24, 'Ray direction is zero or near zero')
        length = math.sqrt(length_squared)
        delta = [x / length * distance for x in params['direction']]
        self._need(all(abs(a + b) <= 1e6 for a, b in zip(params['origin'], delta)),
                   'Ray endpoint exceeds native coordinate bound')
        ignored = params.get('ignore', [])
        self._need(type(ignored) is list and len(ignored) <= 128, 'Ray ignored-ID array bound')
        for identity in ignored:
            self._check(identity, self._ref('id'))
        self._need(len(set(ignored)) == len(ignored), 'Ray ignored IDs must be unique')
        hit = result['hit']
        if hit is not None:
            self._need(hit['entity'] not in ignored and
                       (method != 'runtime.hit_test' or hit['region_entity'] not in ignored),
                       'Ray returned an explicitly ignored identity')
            self._need(math.isclose(hit['distance'], hit['fraction'] * distance, rel_tol=1e-12, abs_tol=1e-9),
                       'Ray fraction/distance mismatch')
            # Native geometry queries cast their segment delta to float32. The
            # point can also use float32 coordinates in single-precision Jolt.
            # This arithmetic allowance does not certify collision precision.
            for actual, origin, segment in zip(hit['position'], params['origin'], delta):
                tolerance = 8 * (2 ** -23) * max(1, abs(origin), abs(segment))
                self._need(abs(actual - (origin + segment * hit['fraction'])) <= tolerance,
                           'Ray position differs from the requested segment')

    def _query_hit(self, method, result):
        hit = result['hit']
        if hit is None:
            return
        self._need(hit['fraction'] != 0 or hit['distance'] == 0, 'Zero-fraction hit has nonzero distance')
        if hit['normal'] is None:
            self._need(hit['fraction'] == 0 and hit['triangle'] is None,
                       'Unavailable normal requires an origin-inside primitive/region hit')
        else:
            self._need(any(x != 0 for x in hit['normal']), 'Hit normal cannot be zero')
        if method == 'runtime.hit_test':
            region = hit['region'] is not None
            self._need(region == (hit['region_entity'] is not None), 'Hit region identity/name pair mismatch')
            self._need(not region or hit['triangle'] is None, 'Hit region cannot have a mesh triangle')
