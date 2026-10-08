# SPDX-License-Identifier: Apache-2.0
"""Validate the candidate core results; this is not a general schema engine."""
import copy
import json
import math
import re
from importlib import resources

_CONTRACT = json.loads(resources.files(__package__).joinpath(
    'core_responses.v1.candidate.json').read_text(encoding='utf-8'))


class ResponseContractError(RuntimeError):
    """A terminal result arrived but its core shape/semantics were unexpected.

    The transport remains usable. No mutation is retried automatically; inspect
    state or deliberately recover its receipt. This differs from OutcomeUnknown.
    """
    def __init__(self, method, reason, result, params):
        super().__init__(method + ': ' + reason)
        self.method = method
        self.result = result
        self.params_json = json.dumps(params, ensure_ascii=False, allow_nan=False,
                                      separators=(',', ':'))
        self.params = json.loads(self.params_json)
        self.request_id = self.params.get('request_id') if isinstance(self.params, dict) else None


def response_contract():
    """Return a detached copy of the portable candidate response manifest."""
    return copy.deepcopy(_CONTRACT)


def _literal_equal(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    return a == b


def _check(value, schema, path='$', depth=0):
    if depth > 64:
        return path + ': result nesting exceeds 64'
    if '$ref' in schema:
        return _check(value, _CONTRACT['$defs'][schema['$ref'].removeprefix('#/$defs/')], path, depth+1)
    if 'anyOf' in schema and all(_check(value, child, path, depth+1)
                                 for child in schema['anyOf']):
        return path + ': no accepted alternative'
    if 'const' in schema and not _literal_equal(value, schema['const']):
        return path + ': unexpected literal'
    if 'enum' in schema and not any(_literal_equal(value, v) for v in schema['enum']):
        return path + ': unexpected enum value'
    kind = schema.get('type')
    numeric = isinstance(value, (int, float)) and not isinstance(value, bool)
    checks = {'object': isinstance(value, dict), 'array': isinstance(value, list),
              'string': isinstance(value, str), 'boolean': isinstance(value, bool),
              'integer': type(value) is int, 'number': numeric, 'null': value is None}
    if kind and not checks[kind]:
        return path + ': expected ' + kind
    if numeric:
        try:
            if isinstance(value, float) and not math.isfinite(value):
                return path + ': nonfinite number'
        except OverflowError:
            return path + ': unrepresentable number'
        for name, comparison in (('minimum', lambda a, b: a < b), ('maximum', lambda a, b: a > b),
                                 ('exclusiveMinimum', lambda a, b: a <= b)):
            if name in schema and comparison(value, schema[name]):
                return path + ': number outside contract bound'
    if isinstance(value, str):
        if 'pattern' in schema and re.fullmatch(schema['pattern'], value) is None:
            return path + ': invalid identifier'
        if len(value) < schema.get('minLength', 0) or len(value) > schema.get('maxLength', len(value)):
            return path + ': string length outside contract bound'
    if isinstance(value, list):
        if len(value) < schema.get('minItems', 0) or len(value) > schema.get('maxItems', len(value)):
            return path + ': array length outside contract bound'
        if 'items' in schema:
            for index, item in enumerate(value):
                reason = _check(item, schema['items'], path+'['+str(index)+']', depth+1)
                if reason:
                    return reason
    if isinstance(value, dict):
        if len(value) < schema.get('minProperties', 0) or len(value) > schema.get('maxProperties', len(value)):
            return path + ': property count outside contract bound'
        for name in schema.get('required', []):
            if name not in value:
                return path + ': missing ' + name
        for name, child in schema.get('properties', {}).items():
            if name in value:
                reason = _check(value[name], child, path+'.'+name, depth+1)
                if reason:
                    return reason
        extra = schema.get('additionalProperties', True)
        if isinstance(extra, dict):
            for name, item in value.items():
                if name not in schema.get('properties', {}):
                    reason = _check(item, extra, path+'.'+name, depth+1)
                    if reason:
                        return reason
    return None


def validate_core_result(method, result, params=None):
    """Validate supported shapes plus guards, without equating retry/current revisions."""
    params = {} if params is None else params
    schema = _CONTRACT['methods'].get(method)
    if schema is None:
        return result
    if not isinstance(params, dict):
        raise ResponseContractError(method, 'successful core result used non-object parameters', result, params)
    if method == 'world.describe':
        view = params.get('view', 'full')
        if not isinstance(view, str) or 'world.describe.'+view not in _CONTRACT['variants']:
            raise ResponseContractError(method, 'successful discovery used an unknown view', result, params)
        schema = _CONTRACT['variants']['world.describe.'+view]
    elif method == 'entity.get':
        suffix = 'full' if 'component' not in params else params['component']
        if not isinstance(suffix, str):
            raise ResponseContractError(method, 'successful component read used an invalid selector', result, params)
        schema = _CONTRACT['variants'].get('entity.get.'+suffix, schema)
    elif method == 'world.transact':
        if type(params.get('preview', False)) is not bool:
            raise ResponseContractError(method, 'successful transaction used an invalid preview flag', result, params)
        schema = _CONTRACT['variants']['world.transact.'+('preview' if params.get('preview', False) else 'commit')]
    reason = _check(result, schema)
    if not reason:
        if method in ('entity.get', 'entity.world_transform') and result['id'] != params.get('id'):
            reason = 'result entity ID differs from the requested ID'
        if 'revision' in params and result.get('revision') != params['revision']:
            reason = 'result revision differs from the requested read guard'
        if method == 'world.history' and (result['undo_count']+result['redo_count'] > result['max_entries']
                                         or result['bytes'] > result['max_bytes']):
            reason = 'history exceeds its advertised bounds'
        if method == 'world.transact' and params.get('preview', False) and 'history_recorded' in result:
            reason = 'preview unexpectedly reports recorded history'
        if method in ('world.describe', 'world.inspect') and result['read_only'] != (result['mode'] == 'read_only_runtime'):
            reason = 'read-only metadata disagrees with mode'
        if method == 'world.describe' and params.get('view') in ('method', 'component', 'section'):
            category = {'method': 'methods', 'component': 'components', 'section': 'sections'}[params['view']]
            if not isinstance(params.get('name'), str) or list(result[category]) != [params['name']]:
                reason = 'focused discovery differs from the requested name'
        if method == 'world.describe' and params.get('view') == 'catalog':
            for category in ('methods', 'components', 'sections'):
                names = result[category]
                if names != sorted(set(names)):
                    reason = 'discovery catalog names are not strictly sorted and unique'
        if method == 'entity.query':
            identifiers = [item['id'] for item in result['entities']]
            limit = params.get('limit', 64)
            after = params.get('after')
            if type(limit) is not int or not 1 <= limit <= 256:
                reason = 'successful query used an invalid page limit'
            elif len(identifiers) > limit:
                reason = 'query exceeds its requested page limit'
            elif identifiers != sorted(set(identifiers)):
                reason = 'query entity IDs are not strictly sorted and unique'
            elif result['next_after'] is not None and (not identifiers or result['next_after'] != identifiers[-1]):
                reason = 'query continuation is not the last returned entity ID'
            elif result['next_after'] is not None and len(identifiers) != limit:
                reason = 'query continuation follows an incomplete page'
            elif 'after' in params and (not isinstance(after, str) or _check(after, _CONTRACT['$defs']['id'])
                                        or 'revision' not in params):
                reason = 'successful query used an invalid continuation guard'
            elif after is not None and identifiers and identifiers[0] <= after:
                reason = 'query returned an entity before its exclusive cursor'
            for row in result['entities']:
                if 'parent' in params and row['parent'] != params['parent']:
                    reason = 'query returned an entity outside the requested parent filter'
                if 'component' in params and params['component'] not in row['components']:
                    reason = 'query returned an entity outside the requested component filter'
                if row['components'] != sorted(set(row['components'])):
                    reason = 'query component names are not strictly sorted and unique'
        if method in ('world.transact', 'world.undo', 'world.redo'):
            base = params.get('base_revision')
            if type(base) is not int or not 0 <= base < 9007199254740991 or result['revision'] != base+1:
                reason = 'mutation revision differs from its original base revision plus one'
            identifiers = result['changed_ids']
            if identifiers != sorted(set(identifiers)):
                reason = 'changed IDs are not strictly sorted and unique'
        if method == 'world.transact' and result['committed'] and not result['replayed'] and 'history_recorded' not in result:
            reason = 'fresh commit lacks its history outcome'
    if reason:
        raise ResponseContractError(method, reason, result, params)
    return result
