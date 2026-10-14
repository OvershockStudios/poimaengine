#!/usr/bin/env python3
"""Whole-request candidate projection; not a released API stability contract."""
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import argparse
from copy import deepcopy
import importlib.util
import json
from pathlib import Path

SELECTION_FORMAT = 'poima.alpha-workflow-method-selection.v2'
FORMAT = 'poima.request-profile-projection.v2'
CONTEXT_SCHEMA_NODES = {('spawn_templates', 'components'), ('spawn_templates', 'hierarchy')}
DEPENDENCIES = {'$ref', '$dynamicRef', '$recursiveRef', '$schema', '$id',
                '$anchor', '$dynamicAnchor', '$recursiveAnchor', '$vocabulary'}
GUARD_FIELDS = ('format', 'status', 'methods', 'components', 'context_keys',
                'full_limits', 'profiles', 'context_schema_paths')
SCOPE = ('Candidate whole selected request/component schemas, explicitly named context schema '
         'nodes and otherwise exact selected context only. No results, errors, runtime behavior, availability beyond the '
         'captured scope, performance, binary ABI or release/stability guarantee.')


def _names(values, label):
    if (not isinstance(values, list) or
            any(not isinstance(v, str) or not v for v in values) or
            values != sorted(set(values))):
        raise ValueError(label + ' must be a sorted unique nonempty-name list.')
    return values


def _guard(manifest):
    if not isinstance(manifest, dict) or manifest.get('format') != SELECTION_FORMAT:
        raise ValueError('Unsupported selection format.')
    if manifest.get('status') != 'prospective' or manifest.get('full_limits') is not True:
        raise ValueError('Only prospective whole-limit candidates are supported.')
    for category in ('methods', 'components', 'context_keys'):
        _names(manifest.get(category), category)
    if not manifest['methods'] or 'limits' not in manifest['context_keys']:
        raise ValueError('Candidate needs selected methods and full limits context.')
    paths = manifest.get('context_schema_paths')
    if (not isinstance(paths, list) or any(not isinstance(path, list) or
            not path or any(not isinstance(part, str) or not part for part in path)
            for path in paths)):
        raise ValueError('Explicit context_schema_paths list is required.')
    tuples = [tuple(path) for path in paths]
    if tuples != sorted(set(tuples)):
        raise ValueError('Context schema paths must be sorted and unique.')
    for path in tuples:
        if path[0] not in manifest['context_keys']:
            raise ValueError('Schema path context is not selected.')
        if any(path != other and other[:len(path)] == path for other in tuples):
            raise ValueError('Overlapping context schema paths are forbidden.')
        if path not in CONTEXT_SCHEMA_NODES:
            raise ValueError('Unknown context schema policy path: ' + repr(path))
    profiles = manifest.get('profiles')
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError('Missing profile map.')
    for name, profile in profiles.items():
        if not isinstance(name, str) or not name or not isinstance(profile, dict):
            raise ValueError('Malformed profile.')
        selected = _names(profile.get('selected_methods'), name + '.selected_methods')
        if not selected or not set(selected).issubset(manifest['methods']):
            raise ValueError('Unknown/empty selected method membership in ' + name)
        expected = profile.get('context_expected')
        if (not isinstance(expected, dict) or
                not set(expected).issubset(manifest['context_keys']) or
                type(profile.get('session_scope_presence')) is not bool or
                'session_scope' not in manifest['context_keys']):
            raise ValueError('Malformed expected profile context: ' + name)
    # Retain full profile records, including workflow qualifications. Changing a
    # selection in either side is not evidence that the former selection passed.
    return deepcopy({key: manifest[key] for key in GUARD_FIELDS})


def _dependency(value):
    pending = [value]
    while pending:
        node = pending.pop()
        if isinstance(node, dict):
            if DEPENDENCIES.intersection(node):
                return True
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    return False


def _schema_context_nodes(context, paths):
    nodes = []
    for path in paths:
        entry = context.get(path[0])
        if not isinstance(entry, dict) or entry.get('present') is not True:
            raise ValueError('Selected schema context path is unavailable: ' + repr(path))
        node = entry['value']
        for part in path[1:]:
            if not isinstance(node, dict) or part not in node:
                raise ValueError('Selected schema context path is missing: ' + repr(path))
            node = node[part]
        if not isinstance(node, (dict, bool)):
            raise ValueError('Explicit schema context node must be a schema object or boolean.')
        nodes.append(node)
    return nodes


def project(discovery, manifest, profile_id, checker):
    """Copy complete selected nodes; never normalize context or rewrite schema."""
    guard = _guard(manifest)
    if not isinstance(profile_id, str) or not profile_id or profile_id not in guard['profiles']:
        raise ValueError('Unknown profile: ' + str(profile_id))
    if not isinstance(discovery, dict):
        raise ValueError('Discovery must be an object.')
    if 'result' in discovery:
        discovery = discovery['result']
    if not isinstance(discovery, dict) or discovery.get('partial') is True:
        raise ValueError('A full discovery snapshot is required.')
    profile = guard['profiles'][profile_id]
    for key, expected in profile['context_expected'].items():
        if key not in discovery or not checker._equal(discovery[key], expected):
            raise ValueError('Discovery does not match profile context: ' + key)
    if ('session_scope' in discovery) != profile['session_scope_presence']:
        raise ValueError('Discovery session_scope presence differs from profile.')
    if not isinstance(discovery.get('limits'), dict):
        raise ValueError('Full discovery limits map is required.')
    selected = {}
    for category, names in (('methods', profile['selected_methods']),
                            ('components', guard['components'])):
        source = discovery.get(category)
        if not isinstance(source, dict):
            raise ValueError('Missing discovery schema map: ' + category)
        missing = set(names) - set(source)
        if missing:
            raise ValueError('Missing selected ' + category + ': ' + ', '.join(sorted(missing)))
        if any(not isinstance(source[name], (dict, bool)) for name in names):
            raise ValueError('Selected schemas must be objects or booleans.')
        selected[category] = {name: deepcopy(source[name]) for name in names}
    context = {key: ({'present': True, 'value': deepcopy(discovery[key])}
                     if key in discovery else {'present': False})
               for key in guard['context_keys']}
    # A partial document cannot establish JSON-pointer/resource scope resolution.
    # Retain the entire source and refuse compatibility certification if a selected
    # node or root has scope/reference markers. Nothing is stripped to force PASS.
    schema_context = _schema_context_nodes(context, guard['context_schema_paths'])
    dependency = _dependency(selected) or _dependency(schema_context) or bool(DEPENDENCIES.intersection(discovery))
    if '$defs' in discovery or 'definitions' in discovery:
        dependency = True
    return {'format': FORMAT, 'status': 'candidate', 'scope': SCOPE,
            'profile_id': profile_id, 'selection': guard, **selected,
            'context': context, 'reference_context': deepcopy(discovery) if dependency else None,
            'provenance': {'schema_revision': discovery.get('schema_revision')}}


def _validate_projection(value, checker):
    if (not isinstance(value, dict) or value.get('format') != FORMAT or
            value.get('status') != 'candidate' or value.get('scope') != SCOPE):
        raise ValueError('Unsupported or relabeled candidate projection.')
    if 'reference_context' not in value:
        raise ValueError('Projection must retain reference_context, including explicit null.')
    guard = _guard(value.get('selection'))
    profile_id = value.get('profile_id')
    if not isinstance(profile_id, str) or not profile_id or profile_id not in guard['profiles']:
        raise ValueError('Unknown projection profile.')
    expected = {'methods': guard['profiles'][profile_id]['selected_methods'],
                'components': guard['components']}
    for category, names in expected.items():
        nodes = value.get(category)
        if (not isinstance(nodes, dict) or set(nodes) != set(names) or
                any(not isinstance(node, (dict, bool)) for node in nodes.values())):
            raise ValueError('Projection membership differs from selection: ' + category)
    context = value.get('context')
    if not isinstance(context, dict) or set(context) != set(guard['context_keys']):
        raise ValueError('Projection context membership differs from selection.')
    for entry in context.values():
        if (not isinstance(entry, dict) or type(entry.get('present')) is not bool or
                set(entry) != ({'present', 'value'} if entry['present'] else {'present'})):
            raise ValueError('Malformed context presence record.')
    profile = guard['profiles'][profile_id]
    for key, expected_value in profile['context_expected'].items():
        entry = context[key]
        if not entry['present'] or not checker._equal(entry['value'], expected_value):
            raise ValueError('Projection does not match expected profile context: ' + key)
    if context['session_scope']['present'] != profile['session_scope_presence']:
        raise ValueError('Projection session_scope presence differs from profile.')
    if not context['limits']['present'] or not isinstance(context['limits']['value'], dict):
        raise ValueError('Projection lacks full limits map.')
    schema_context = _schema_context_nodes(context, guard['context_schema_paths'])
    # Re-project reference-bearing sources to prevent removing their dependency
    # marker while leaving an apparently certifiable partial referenced schema.
    reference = value['reference_context']
    if reference is not None:
        reproduced = project(reference, guard, profile_id, checker)
        for key in ('methods', 'components', 'context', 'reference_context'):
            if not checker._equal(reproduced[key], value.get(key)):
                raise ValueError('Reference context does not reproduce selected schema/context.')
    elif _dependency({'methods': value['methods'], 'components': value['components']}) or _dependency(schema_context):
        raise ValueError('Reference-dependent projection lost its source context.')


def compare(before, after, checker):
    """Return conservative findings. Empty findings are request-only evidence."""
    _validate_projection(before, checker)
    _validate_projection(after, checker)
    findings = []
    def add(path, reason):
        if len(findings) < checker.MAX_FINDINGS:
            findings.append({'path': path, 'reason': reason})
    for key in ('profile_id', 'selection'):
        if not checker._equal(before[key], after[key]):
            add('$.' + key, 'Selected profile membership or exact discovery context changed.')
    if before['reference_context'] is not None or after['reference_context'] is not None:
        add('$.reference_context', 'Reference/resource scope compatibility is unproven; complete source retained for review.')
    # Stop on membership changes; comparing a smaller selection cannot certify it.
    if not checker._equal(before['selection'], after['selection']) or before['profile_id'] != after['profile_id']:
        return findings
    schema_paths = {tuple(path) for path in before['selection']['context_schema_paths']}
    def context_compare(left, right, path):
        label = '$.context' + ''.join('[' + json.dumps(part) + ']' for part in path)
        if path in schema_paths:
            for finding in checker.schema_compare(left, right, label):
                add(finding['path'], finding['reason'])
        elif any(selected[:len(path)] == path for selected in schema_paths):
            if not isinstance(left, dict) or not isinstance(right, dict) or left.keys() != right.keys():
                add(label, 'Non-schema context membership changed.')
                return
            for key in left:
                context_compare(left[key], right[key], path + (key,))
        elif not checker._equal(left, right):
            add(label, 'Non-schema context value changed.')
    for key in before['context']:
        left, right = before['context'][key], after['context'][key]
        if left['present'] != right['present']:
            add('$.context[' + json.dumps(key) + ']', 'Context presence changed.')
        elif left['present']:
            context_compare(left['value'], right['value'], (key,))
    for category in ('methods', 'components'):
        for name in before[category]:
            path = '$.' + category + '[' + json.dumps(name) + ']'
            for finding in checker.schema_compare(before[category][name], after[category][name], path):
                add(finding['path'], finding['reason'])
    return findings


def load_checker(path):
    spec = importlib.util.spec_from_file_location('request_profile_compatibility', path)
    if spec is None or spec.loader is None:
        raise ValueError('Cannot load existing request compatibility checker.')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', type=Path, default=Path(__file__).with_name('check_authoring_api.py'),
                        help='Existing conservative request checker (default: sibling script).')
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).resolve().parents[1]/'docs/contracts/runtime-save-workflow-v1.candidate.json',
                        help='Prospective selection manifest, never a stability release.')
    parser.add_argument('--profile', required=True)
    parser.add_argument('--discovery', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        checker = load_checker(args.checker)
        candidate = project(checker._load(args.discovery), checker._load(args.manifest), args.profile, checker)
        result = candidate if args.baseline is None else {
            'format': 'poima.request-profile-comparison.v2', 'status': 'candidate', 'scope': SCOPE,
            'findings': compare(checker._load(args.baseline), candidate, checker)}
        args.output.write_text(json.dumps(result, indent=2, ensure_ascii=True, allow_nan=False) + '\n', encoding='utf-8')
    except (ValueError, OSError) as error:
        parser.error(str(error))
    return 0 if args.baseline is None or not result['findings'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
