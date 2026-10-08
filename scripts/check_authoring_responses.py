#!/usr/bin/env python3
"""Conservative output evolution gate: producer-new results fit consumer-old.

Selected portable methods/variants only. This does not prove native behavior,
availability, contextual invariants or general JSON Schema subsumption. Context
metadata stays exact; behavioral fixtures must qualify its implementation.
"""
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

FORMAT = 'poima.authoring-core-responses.v1'
MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_FINDINGS = 128
MAX_DEPTH = 64
MAX_NODES = 50000
COSMETIC = {'description', 'title', '$comment'}
TYPES = {'null', 'boolean', 'integer', 'number', 'string', 'object', 'array'}
BOUNDS = {'minLength': True, 'maxLength': False, 'minItems': True,
          'maxItems': False, 'minProperties': True, 'maxProperties': False}
NUMERIC = {'minimum', 'exclusiveMinimum', 'maximum', 'exclusiveMaximum'}
SUPPORTED = {'type', 'properties', 'required', 'additionalProperties', 'items',
             'enum', 'const', 'default', *BOUNDS, *NUMERIC}
SCHEMA_MAPS = {'properties', 'patternProperties', '$defs', 'definitions', 'dependentSchemas'}
SCHEMA_LISTS = {'anyOf', 'oneOf', 'allOf', 'prefixItems'}
SCHEMA_SINGLE = {'items', 'additionalProperties', 'not', 'if', 'then', 'else',
                 'contains', 'propertyNames', 'unevaluatedProperties', 'unevaluatedItems'}
SCOPE_MARKERS = {'$id', '$schema', '$anchor', '$dynamicAnchor', '$recursiveAnchor', '$vocabulary'}
REFERENCES = {'$ref', '$dynamicRef', '$recursiveRef'}
MISSING = object()


class SchemaError(ValueError):
    pass


def _equal(a: Any, b: Any) -> bool:
    # Expanded local definitions can be shared by many branches. Memoization
    # avoids revisiting the same DAG pair, while budgets also bound opaque
    # defaults/context/literal data rather than just schema-node traversal.
    seen: set[tuple[int, int]] = set()
    visited = 0
    def equal(left: Any, right: Any, depth: int) -> bool:
        nonlocal visited
        visited += 1
        if depth > MAX_DEPTH or visited > MAX_NODES:
            raise SchemaError('Equality exceeds bounded depth/node budget.')
        if type(left) is not type(right):
            return False
        if isinstance(left, (dict, list)):
            pair = (id(left), id(right))
            if pair in seen:
                return True
            seen.add(pair)
        if isinstance(left, dict):
            return left.keys() == right.keys() and all(equal(left[k], right[k], depth+1) for k in left)
        if isinstance(left, list):
            return len(left) == len(right) and all(equal(x, y, depth+1) for x, y in zip(left, right))
        return left == right
    return equal(a, b, 0)


def _path(path: str, key: str) -> str:
    return path + '[' + json.dumps(key, ensure_ascii=True) + ']'


class Findings:
    def __init__(self) -> None:
        self.values: list[dict[str, str]] = []
        self.omitted = False
        self.compared: set[tuple[int, int]] = set()
        self.comparison_nodes = 0

    def add(self, path: str, reason: str) -> None:
        if len(self.values) < MAX_FINDINGS:
            self.values.append({'path': path[:1024], 'reason': reason})
        elif not self.omitted:
            self.values.append({'path': '$', 'reason': 'Additional findings omitted by bounded report.'})
            self.omitted = True


def _types(value: Any) -> set[str]:
    values = [value] if isinstance(value, str) else value
    if not isinstance(values, list) or not values or any(not isinstance(v, str) or v not in TYPES for v in values):
        raise SchemaError('Invalid type declaration.')
    if len(values) != len(set(values)):
        raise SchemaError('Duplicate type declaration.')
    return set(values)


def _finite_number(value: Any) -> bool:
    return type(value) is int or (type(value) is float and math.isfinite(value))


def _contains_reference(value: Any) -> bool:
    pending = [value]
    while pending:
        node = pending.pop()
        if isinstance(node, dict):
            if REFERENCES & node.keys():
                return True
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    return False


class Resolver:
    """Bare static root $defs pointers only; no dynamic/dialect/resource solver."""
    def __init__(self, definitions: Any = None) -> None:
        self.definitions = {} if definitions is None else definitions
        if not isinstance(self.definitions, dict):
            raise SchemaError('$defs must be a map.')
        self.cache: dict[str, Any] = {}
        self.active: set[str] = set()
        self.nodes = 0

    def expand(self, schema: Any, depth: int = 0) -> Any:
        self.nodes += 1
        if depth > MAX_DEPTH or self.nodes > MAX_NODES:
            raise SchemaError('Schema expansion exceeds bounded depth/node budget.')
        if type(schema) is bool:
            return schema
        if not isinstance(schema, dict):
            raise SchemaError('Schema must be an object or boolean.')
        if SCOPE_MARKERS & schema.keys() or {'$dynamicRef', '$recursiveRef'} & schema.keys():
            raise SchemaError('Unsupported reference/resource/dialect scope.')
        if '$ref' in schema:
            if set(schema) - COSMETIC - {'$ref'}:
                raise SchemaError('Reference siblings require an unsupported dialect proof.')
            reference = schema['$ref']
            if not isinstance(reference, str) or not reference.startswith('#/$defs/'):
                raise SchemaError('Only bare static root #/$defs/name references are supported.')
            encoded = reference[len('#/$defs/'):]
            if not encoded or '/' in encoded:
                raise SchemaError('Nested/ancestor/array reference pointers are unsupported.')
            # Decode exactly one JSON Pointer token. Invalid escapes are not
            # silently accepted, and URI-fragment percent decoding is unsupported.
            token = ''; index = 0
            while index < len(encoded):
                if encoded[index] == '~':
                    if index+1 >= len(encoded) or encoded[index+1] not in '01':
                        raise SchemaError('Invalid JSON Pointer token escape.')
                    token += '~' if encoded[index+1] == '0' else '/'; index += 2
                else:
                    token += encoded[index]; index += 1
            if '%' in encoded or token not in self.definitions:
                raise SchemaError('Reference target is missing or unsupported.')
            if token in self.active:
                raise SchemaError('Cyclic local reference is unsupported.')
            if token not in self.cache:
                self.active.add(token)
                try:
                    self.cache[token] = self.expand(self.definitions[token], depth+1)
                finally:
                    self.active.remove(token)
            return self.cache[token]
        out = {}
        for key, value in schema.items():
            if key in COSMETIC:
                continue
            if key in SCHEMA_MAPS:
                if not isinstance(value, dict) or any(not isinstance(k, str) for k in value):
                    raise SchemaError('Invalid schema map: '+key)
                out[key] = {name: self.expand(child, depth+1) for name, child in value.items()}
            elif key in SCHEMA_LISTS:
                if not isinstance(value, list) or not value:
                    raise SchemaError('Invalid schema alternatives: '+key)
                out[key] = [self.expand(child, depth+1) for child in value]
            elif key in SCHEMA_SINGLE:
                out[key] = self.expand(value, depth+1)
            else:
                if key not in {'const', 'enum', 'default'} and _contains_reference(value):
                    raise SchemaError('Reference hidden in unsupported keyword: '+key)
                out[key] = value
        self.validate(out)
        return out

    @staticmethod
    def validate(schema: dict) -> None:
        if 'type' in schema:
            _types(schema['type'])
        for key in BOUNDS:
            if key in schema and (type(schema[key]) is not int or schema[key] < 0):
                raise SchemaError('Invalid integral bound: '+key)
        for low, high in (('minLength', 'maxLength'), ('minItems', 'maxItems'), ('minProperties', 'maxProperties')):
            if low in schema and high in schema and schema[low] > schema[high]:
                raise SchemaError('Contradictory size bounds.')
        for key in NUMERIC:
            if key in schema and not _finite_number(schema[key]):
                raise SchemaError('Invalid finite numeric bound: '+key)
        if 'required' in schema:
            required = schema['required']
            if not isinstance(required, list) or any(not isinstance(k, str) for k in required) or len(required) != len(set(required)):
                raise SchemaError('Invalid required-field list.')
        if 'enum' in schema and (not isinstance(schema['enum'], list) or not schema['enum']):
            raise SchemaError('Invalid enum list.')


def _bound(schema: dict, lower: bool) -> tuple[Any, bool] | None:
    inclusive, exclusive = ('minimum', 'exclusiveMinimum') if lower else ('maximum', 'exclusiveMaximum')
    bounds = [(schema[k], k == exclusive) for k in (inclusive, exclusive) if k in schema]
    if not bounds:
        return None
    value = max(v for v, _ in bounds) if lower else min(v for v, _ in bounds)
    return value, any(strict for v, strict in bounds if v == value)


def _literals(schema: dict) -> list[Any] | None:
    if 'const' in schema:
        values = [schema['const']]
        if 'enum' in schema:
            values = [v for v in values if any(_equal(v, x) for x in schema['enum'])]
        return values
    return schema.get('enum')


def _default_preserved(old: Any, new: Any, path: str, found: Findings) -> None:
    before = old.get('default', MISSING) if isinstance(old, dict) else MISSING
    after = new.get('default', MISSING) if isinstance(new, dict) else MISSING
    if not _equal(before, after):
        found.add(_path(path, 'default'), 'Default metadata changed, was added, or was removed.')


def _has_default(schema: Any) -> bool:
    pending = [schema]; seen: set[int] = set()
    while pending:
        node = pending.pop()
        if not isinstance(node, dict) or id(node) in seen:
            continue
        seen.add(id(node))
        if len(seen) > MAX_NODES:
            raise SchemaError('Default metadata scan exceeds bounded node budget.')
        if 'default' in node:
            return True
        for key in SCHEMA_MAPS:
            if key in node:
                pending.extend(node[key].values())
        for key in SCHEMA_LISTS:
            pending.extend(node.get(key, []))
        for key in SCHEMA_SINGLE:
            if key in node:
                pending.append(node[key])
    return False


def _subset(old: Any, new: Any, path: str, found: Findings, depth: int = 0) -> None:
    found.comparison_nodes += 1
    if depth > MAX_DEPTH or found.comparison_nodes > MAX_NODES:
        found.add(path, 'Schema comparison exceeds bounded depth/node budget.'); return
    # Definition expansion shares immutable subtrees. Comparing the same pair
    # again adds no proof and could otherwise multiply work exponentially. Any
    # rejection from its first visit already remains in the shared report.
    pair = (id(old), id(new))
    if pair in found.compared:
        return
    found.compared.add(pair)
    _default_preserved(old, new, path, found)
    if _equal(old, new):
        return
    if new is False:
        if _has_default(old):
            found.add(path, 'Impossible/removed schema loses nested old default metadata.')
        return
    if old is True:
        return
    if old is False:
        found.add(path, 'Producer can emit a value forbidden by the old consumer.'); return
    if new is True:
        new = {}
    assert isinstance(old, dict) and isinstance(new, dict)
    unknown = (old.keys() | new.keys()) - SUPPORTED
    if unknown:
        # Even unchanged unknown constraints can interact with changed known
        # ones. For example patternProperties can admit an old named property
        # after its declaration is removed from a newly closed object. Only an
        # identical complete node can bypass this unsupported-context guard.
        for key in sorted(unknown):
            found.add(_path(path, key), 'Changed node contains unsupported keyword/union; output containment is unproven.')
        return
    old_types = _types(old['type']) if 'type' in old else None
    new_types = _types(new['type']) if 'type' in new else None
    if old_types is not None:
        if new_types is None or any(kind not in old_types and not (kind == 'integer' and 'number' in old_types) for kind in new_types):
            found.add(_path(path, 'type'), 'Producer widened or removed an old output type constraint.')
    old_literals, new_literals = _literals(old), _literals(new)
    if old_literals is not None and (new_literals is None or any(not any(_equal(v, o) for o in old_literals) for v in new_literals)):
        found.add(path, 'Producer widened or removed an old output literal constraint.')
    for key, lower in BOUNDS.items():
        if key in old and (key not in new or (new[key] < old[key] if lower else new[key] > old[key])):
            found.add(_path(path, key), 'Producer relaxed or removed an old output size bound.')
    for lower in (True, False):
        before, after = _bound(old, lower), _bound(new, lower)
        if before is not None and (after is None or (after[0] < before[0] if lower else after[0] > before[0]) or
                                   (after[0] == before[0] and before[1] and not after[1])):
            found.add(path, 'Producer relaxed or removed an old numeric output bound.')
    old_required, new_required = set(old.get('required', [])), set(new.get('required', []))
    if old_required - new_required:
        found.add(_path(path, 'required'), 'Producer no longer guarantees every old required field.')
    old_props, new_props = old.get('properties', {}), new.get('properties', {})
    old_extra, new_extra = old.get('additionalProperties', True), new.get('additionalProperties', True)
    for name, child in old_props.items():
        child_path = _path(_path(path, 'properties'), name)
        if name in new_props:
            _subset(child, new_props[name], child_path, found, depth+1)
        elif new_extra is False:
            # Optional old names cannot be emitted, so their value constraints
            # are vacuous. Preserve any existing default metadata separately.
            if _has_default(child):
                found.add(child_path, 'Removed optional field loses old default metadata.')
        else:
            _subset(child, new_extra, child_path, found, depth+1)
    for name, child in new_props.items():
        if name not in old_props:
            if old_extra is False:
                found.add(_path(_path(path, 'properties'), name), 'Producer introduced a field forbidden by the old closed object.')
            elif old_extra is not True:
                _subset(old_extra, child, _path(_path(path, 'properties'), name), found, depth+1)
    _subset_extra(old_extra, new_extra, _path(path, 'additionalProperties'), found, depth)
    if 'items' in old:
        _subset(old['items'], new.get('items', True), _path(path, 'items'), found, depth+1)


def _subset_extra(old: Any, new: Any, path: str, found: Findings, depth: int) -> None:
    if old is True and new is not False:
        _default_preserved(old, new, path, found)
        return
    if old is False and new is not False:
        found.add(path, 'Producer now permits arbitrary extra object fields.'); return
    _subset(old, new, path, found, depth+1)


def response_schema_compare(consumer_old: Any, producer_new: Any, old_defs=None, new_defs=None, path='$') -> list[dict[str, str]]:
    found = Findings()
    try:
        old = Resolver(old_defs).expand(consumer_old)
        new = Resolver(new_defs).expand(producer_new)
        _subset(old, new, path, found)
    except (SchemaError, RecursionError) as error:
        found.add(path, 'Unproven schema/reference context: '+str(error))
    return found.values


def compare_responses(baseline: dict, candidate: dict) -> list[dict[str, str]]:
    if not isinstance(baseline, dict) or not isinstance(candidate, dict):
        raise ValueError('Response manifests must be objects.')
    if baseline.get('format') != FORMAT or candidate.get('format') != FORMAT:
        raise ValueError('Expected portable response format '+FORMAT)
    for document in (baseline, candidate):
        if not isinstance(document.get('methods'), dict) or not document['methods'] or not isinstance(document.get('variants'), dict):
            raise ValueError('Response manifest needs selected methods and variants maps.')
        if SCOPE_MARKERS & document.keys() or REFERENCES & document.keys():
            raise ValueError('Manifest resource/dialect/reference scope is unsupported.')
    found = Findings()
    ignored = {'methods', 'variants', '$defs', 'status'} | COSMETIC
    for key in (baseline.keys() | candidate.keys()) - ignored:
        if not _equal(baseline.get(key, MISSING), candidate.get(key, MISSING)):
            found.add(_path('$', key), 'Manifest metadata/context changed; behavioral preservation is unproven.')
    for category in ('methods', 'variants'):
        for name, old in baseline[category].items():
            path = _path(_path('$', category), name)
            if name not in candidate[category]:
                found.add(path, 'Selected response method/variant was removed.')
                continue
            if candidate[category][name] is False and old is not False:
                found.add(path, 'Selected response was made impossible; availability requires separate qualification.')
                continue
            for finding in response_schema_compare(old, candidate[category][name], baseline.get('$defs'), candidate.get('$defs'), path):
                found.add(finding['path'], finding['reason'])
    return found.values


def _load(path: Path) -> Any:
    with path.open('rb') as source:
        data = source.read(MAX_INPUT_BYTES+1)
    if len(data) > MAX_INPUT_BYTES:
        raise ValueError('Input exceeds 8 MiB.')
    def floating(value):
        number = float(value)
        if not math.isfinite(number): raise ValueError('Non-finite JSON number.')
        return number
    def invalid(value):
        raise ValueError('Non-finite JSON number.')
    def pairs(entries):
        out = {}
        for key, value in entries:
            if key in out: raise ValueError('Duplicate JSON key: '+key)
            out[key] = value
        return out
    return json.loads(data.decode('utf-8'), parse_float=floating, parse_constant=invalid, object_pairs_hook=pairs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    try:
        findings = compare_responses(_load(args.baseline), _load(args.candidate))
        result = {'passed': not findings, 'findings': findings,
                  'scope': 'Selected result acceptance only; no native behavior, context implementation, availability or general schema-subsumption guarantee.'}
        code = 1 if findings else 0
    except (OSError, ValueError, TypeError, RecursionError) as error:
        result = {'passed': False, 'error': str(error)[:4096]}; code = 2
    text = json.dumps(result, indent=2, ensure_ascii=True)+'\n'
    print(text, end='')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding='utf-8')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
