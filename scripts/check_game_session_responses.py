#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Conservative v1 result evolution: producer-new must fit consumer-old.

Uses the authoring response comparator without changing its policy. Identical
admitted recursive closures are equality proofs; changed recursive schemas can
remain unproven. No native behavior, availability or error-policy proof follows.
"""
import argparse
import hashlib
import json
from pathlib import Path

from check_authoring_responses import (
    COSMETIC, MAX_DEPTH, MAX_NODES, REFERENCES, SCHEMA_LISTS, SCHEMA_MAPS,
    SCHEMA_SINGLE, SCOPE_MARKERS, Findings, Resolver, _contains_reference,
    _equal, _load, response_schema_compare)

FORMAT = 'poima.runtime-game-session-responses.v1'
IDENTITY = 'poima.runtime-game-session'
METHODS = (
    'runtime.component.get', 'runtime.component.query', 'runtime.components',
    'runtime.entity', 'runtime.gameplay.inspect', 'runtime.hit_test',
    'runtime.inspect', 'runtime.observe', 'runtime.raycast', 'runtime.save.result',
    'runtime.save.status', 'runtime.status', 'runtime.step', 'runtime.ui.activate',
    'runtime.ui.inspect', 'save.inspect', 'save.load', 'save.status', 'save.write')
PROFILES = ['source_world', 'source_shared_player', 'readonly_game']


def _closure(schema, definitions):
    """Admit bounded bare local refs, including unchanged recursive closures."""
    visited, nodes = set(), 0

    def visit(node, depth=0):
        nonlocal nodes
        nodes += 1
        if depth > MAX_DEPTH or nodes > MAX_NODES:
            raise ValueError('Response closure exceeds bounded depth/node budget.')
        if type(node) is bool:
            return
        if type(node) is not dict:
            raise ValueError('Response schema must be an object or Boolean.')
        if SCOPE_MARKERS & node.keys() or (REFERENCES - {'$ref'}) & node.keys():
            raise ValueError('Unsupported resource/dialect/dynamic reference context.')
        if '$ref' in node:
            if set(node) - {'$ref'} - COSMETIC:
                raise ValueError('Reference siblings require unsupported dialect proof.')
            ref = node['$ref']
            if type(ref) is not str or not ref.startswith('#/$defs/'):
                raise ValueError('Only bare root definition references are supported.')
            encoded = ref[len('#/$defs/'):]
            token, index = '', 0
            while index < len(encoded):
                if encoded[index] == '~':
                    if index + 1 >= len(encoded) or encoded[index + 1] not in '01':
                        raise ValueError('Invalid JSON Pointer token escape.')
                    token += '~' if encoded[index + 1] == '0' else '/'
                    index += 2
                else:
                    token += encoded[index]
                    index += 1
            if not encoded or '/' in encoded or '%' in encoded or token not in definitions:
                raise ValueError('Missing or unsupported root definition reference.')
            if token not in visited:
                visited.add(token)
                visit(definitions[token], depth + 1)
            return
        Resolver.validate(node)
        for key, value in node.items():
            if key in SCHEMA_MAPS:
                if type(value) is not dict:
                    raise ValueError('Invalid response schema map.')
                for child in value.values():
                    visit(child, depth + 1)
            elif key in SCHEMA_LISTS:
                if type(value) is not list or not value:
                    raise ValueError('Invalid response schema alternatives.')
                for child in value:
                    visit(child, depth + 1)
            elif key in SCHEMA_SINGLE:
                visit(value, depth + 1)
            elif key not in {'const', 'enum', 'default'} and _contains_reference(value):
                raise ValueError('Reference hidden in an unsupported schema keyword.')

    visit(schema)
    return visited


def _admit(document):
    if type(document) is not dict:
        raise ValueError('Response manifest must be an object.')
    required = {'format', 'id', 'status', 'contract_version', 'source_cohort', 'profiles',
                'context_keys', 'scope', 'limits', 'unchecked', 'query_checks', '$defs', 'methods'}
    if not required <= document.keys():
        raise ValueError('Missing frozen response metadata fields.')
    for key, expected in (('format', FORMAT), ('id', IDENTITY), ('status', 'stable'),
                          ('contract_version', 1), ('profiles', PROFILES),
                          ('context_keys', ['profile', 'component_schemas', 'gameplay_schema']),
                          ('limits', {'depth': 64, 'nodes': 100000, 'encoded_bytes': 16777216})):
        if not _equal(document.get(key), expected):
            raise ValueError('Invalid frozen response identity/profile: ' + key)
    if SCOPE_MARKERS & document.keys() or REFERENCES & document.keys():
        raise ValueError('Manifest resource/reference context is unsupported.')
    methods, definitions = document.get('methods'), document.get('$defs')
    if type(methods) is not dict or sorted(methods) != list(METHODS):
        raise ValueError('Expected exactly the nineteen selected session methods.')
    if type(definitions) is not dict or not definitions:
        raise ValueError('Expected the transitive definition map.')
    closures = {name: _closure(schema, definitions) for name, schema in methods.items()}
    reachable = set().union(*closures.values())
    if reachable != set(definitions):
        raise ValueError('Definitions must be exactly the selected transitive closure.')
    return closures


def compare_responses(baseline, candidate):
    old_closures, new_closures = _admit(baseline), _admit(candidate)
    found = Findings()
    # Status is intentionally included; unlike the historical candidate gate,
    # publication identity and all noncosmetic context must remain exact.
    metadata = (baseline.keys() | candidate.keys()) - {'methods', '$defs'} - COSMETIC
    for key in sorted(metadata):
        if (key in baseline) != (key in candidate) or not _equal(baseline.get(key), candidate.get(key)):
            found.add('$[' + json.dumps(key) + ']', 'Frozen response metadata/context changed.')
    if set(baseline['$defs']) != set(candidate['$defs']):
        found.add('$["$defs"]', 'Frozen definition membership changed.')
    for name in METHODS:
        path = '$["methods"][' + json.dumps(name) + ']'
        old, new = baseline['methods'][name], candidate['methods'][name]
        if new is False and old is not False:
            found.add(path, 'Selected result became impossible; availability needs separate qualification.')
            continue
        closure = old_closures[name] | new_closures[name]
        same_closure = old_closures[name] == new_closures[name] and all(
            _equal(baseline['$defs'][key], candidate['$defs'][key]) for key in closure)
        if _equal(old, new) and same_closure:
            continue  # Exact admitted recursive closure needs no unfolding.
        for finding in response_schema_compare(old, new, baseline['$defs'], candidate['$defs'], path):
            found.add(finding['path'], finding['reason'])
    return found.values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.report:
        for source in (args.baseline, args.candidate):
            if args.report.resolve() == source.resolve() or (
                    args.report.exists() and source.exists() and args.report.samefile(source)):
                parser.error('Report must not overwrite or alias either input manifest.')
    try:
        findings = compare_responses(_load(args.baseline), _load(args.candidate))
        result = {'passed': not findings, 'findings': findings,
                  'scope': 'Selected output containment and frozen metadata only; no native behavior, errors or availability proof.',
                  'inputs': {key: {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                   'bytes': path.stat().st_size}
                             for key, path in (('baseline', args.baseline), ('candidate', args.candidate))}}
        code = 1 if findings else 0
    except (OSError, ValueError, TypeError, RecursionError) as error:
        result = {'passed': False, 'error': str(error)[:4096]}
        code = 2
    text = json.dumps(result, indent=2, ensure_ascii=True) + '\n'
    print(text, end='')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding='utf-8')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
