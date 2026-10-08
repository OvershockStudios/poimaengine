#!/usr/bin/env python3
"""Project native discovery to the candidate authoring-core request contract."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import json
from pathlib import Path

METHODS = (
    'world.describe', 'world.inspect', 'world.history', 'world.transact',
    'world.undo', 'world.redo', 'entity.get', 'entity.query', 'entity.world_transform',
)
ENTITY_OPS = {'entity.create', 'entity.rename', 'entity.reparent', 'entity.delete'}


def contains_reference(value):
    if isinstance(value, dict):
        return (bool({'$ref', '$dynamicRef', '$recursiveRef'} & value.keys())
                or any(contains_reference(child) for child in value.values()))
    return isinstance(value, list) and any(contains_reference(child) for child in value)


def contains_schema_dependency(value):
    """References and resource/dialect scopes make structural pruning unsafe.

    A pointer can observe a sibling index or an ancestor outside the branch
    being filtered. Scan the complete discovery conservatively rather than
    resolve references or assume a particular JSON Schema dialect.
    """
    markers = {'$ref', '$dynamicRef', '$recursiveRef', '$schema', '$id',
               '$anchor', '$dynamicAnchor', '$recursiveAnchor', '$vocabulary'}
    pending = [value]
    while pending:
        node = pending.pop()
        if isinstance(node, dict):
            if markers & node.keys():
                return True
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    return False


def outside_core(branch):
    """Discard only branches whose constraints exclude every core request.

    Unrecognized/broad branches must survive: oneOf overlap can invalidate old
    requests even when the original selected branch itself is unchanged.
    """
    if not isinstance(branch, dict) or contains_reference(branch):
        return False
    props = branch.get('properties', {})
    op = props.get('op', {}).get('const')
    if isinstance(op, str) and op not in ENTITY_OPS | {'component.set'}:
        return True
    if op == 'component.set':
        component = props.get('type', {})
        value = component.get('const')
        return ((isinstance(value, str) and value != 'Transform')
                or component.get('pattern') == '^game:[0-9a-f]{32}$')
    return False


def accepts_transform_selector(schema):
    # Validate the known source shape before recording the narrower baseline.
    # Unknown source shapes require review rather than inventing acceptance.
    if not isinstance(schema, dict):
        return False
    if set(schema) <= {'enum', 'description', 'title', '$comment'} and 'enum' in schema:
        return isinstance(schema['enum'], list) and 'Transform' in schema['enum']
    if set(schema) <= {'anyOf', 'description', 'title', '$comment'} and 'anyOf' in schema:
        return (isinstance(schema['anyOf'], list)
                and any(accepts_transform_selector(branch) for branch in schema['anyOf']))
    return False


def project(discovery, baseline=False):
    """Leave missing entries missing so the compatibility gate can reject them."""
    discovery = discovery.get('result', discovery)
    dependent = contains_schema_dependency(discovery)
    if baseline and dependent:
        raise ValueError('Cannot create a baseline from unresolved schema references or resource/dialect scopes.')
    methods = {name: copy.deepcopy(discovery['methods'][name])
               for name in METHODS if name in discovery.get('methods', {})}
    transact = methods.get('world.transact')
    if transact:
        union = transact['properties']['ops']['items']
        if not dependent:
            union['oneOf'] = [branch for branch in union['oneOf'] if not outside_core(branch)]
        # Keep the entire array, including outside branches, when references or
        # scopes are present: retaining just a reference-bearing branch can
        # silently retarget its sibling JSON Pointer after earlier removals.
    # Baseline promises only Transform selections, not every currently listed component.
    # Candidate selectors stay intact: narrowing or removing Transform must be detected.
    if baseline:
        if len(methods) != len(METHODS):
            raise ValueError('Discovery lacks the complete baseline method scope.')
        for name in ('entity.get', 'entity.query'):
            if not accepts_transform_selector(methods[name].get('properties', {}).get('component')):
                raise ValueError('Cannot prove source selector accepts Transform: '+name)
            methods[name]['properties']['component'] = {'enum': ['Transform']}
    result = {'methods': methods, 'components': {
        'Transform': copy.deepcopy(discovery['components']['Transform'])
    } if 'Transform' in discovery.get('components', {}) else {}}
    if baseline:
        identities = [(branch.get('properties', {}).get('op', {}).get('const'),
                       branch.get('properties', {}).get('type', {}).get('const'))
                      for branch in union['oneOf'] if isinstance(branch, dict)]
        expected = {(op, None) for op in ENTITY_OPS} | {('component.set', 'Transform')}
        if len(union['oneOf']) != 5 or len(identities) != 5 or set(identities) != expected or not result['components']:
            raise ValueError('Discovery lacks the complete baseline scope.')
        result.update(format='poima.authoring-api-baseline.v1', status='candidate',
                      contract_version=1, source_schema_revision=discovery['schema_revision'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--discovery', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--baseline', action='store_true', help='Create a candidate baseline from qualified discovery.')
    args = parser.parse_args()
    result = project(json.loads(args.discovery.read_text(encoding='utf-8')), args.baseline)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
