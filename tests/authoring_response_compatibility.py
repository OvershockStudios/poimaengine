#!/usr/bin/env python3
"""Directional output-contract regressions, independent of native execution."""
# SPDX-License-Identifier: Apache-2.0
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from check_authoring_responses import (FORMAT, MAX_FINDINGS, MAX_INPUT_BYTES,
                                       compare_responses, response_schema_compare)

CANDIDATE = json.loads((ROOT/'tools/python/poima_client/core_responses.v1.candidate.json').read_text(encoding='utf-8'))


def object_schema(properties, required=(), extra=True):
    return dict(type='object', properties=properties, required=list(required), additionalProperties=extra)


def manifest(schema):
    return dict(format=FORMAT, status='candidate', contract_version=1,
                methods={'world.inspect': schema}, variants={})


class OutputCompatibility(unittest.TestCase):
    def accepts(self, old, new, **kwargs):
        self.assertEqual(response_schema_compare(old, new, **kwargs), [])

    def rejects(self, old, new, reason=None, **kwargs):
        findings = response_schema_compare(old, new, **kwargs)
        self.assertTrue(findings, (old, new))
        self.assertTrue(all(f['path'].startswith('$') for f in findings))
        if reason:
            self.assertTrue(any(reason in f['reason'] for f in findings), findings)

    def test_current_nine_core_contract_and_publication_status(self):
        self.assertEqual(len(CANDIDATE['methods']), 9)
        self.assertEqual(compare_responses(CANDIDATE, CANDIDATE), [])
        stable = copy.deepcopy(CANDIDATE); stable['status'] = 'stable'
        self.assertEqual(compare_responses(CANDIDATE, stable), [])
        stable['description'] = 'Publication description'
        self.assertEqual(compare_responses(CANDIDATE, stable), [])
        for field, value in (('contract_version', 2), ('context_version', 1),
                             ('context_rules', ['query.default_limit=256'])):
            with self.subTest(field=field):
                changed = copy.deepcopy(CANDIDATE); changed[field] = value
                self.assertTrue(compare_responses(CANDIDATE, changed))
        changed = copy.deepcopy(CANDIDATE); changed['format'] = 'other'
        with self.assertRaises(ValueError): compare_responses(CANDIDATE, changed)

    def test_additive_native_metadata_fits_old_open_consumer(self):
        new = copy.deepcopy(CANDIDATE)
        for name, schema in new['methods'].items():
            schema.setdefault('properties', {})['future_metadata'] = {'type': 'object'}
            schema['required'].append('future_metadata')
        self.assertEqual(compare_responses(CANDIDATE, new), [])
        new['methods']['future.method'] = {'type': 'string'}
        new['variants']['future.variant'] = {'type': 'boolean'}
        self.assertEqual(compare_responses(CANDIDATE, new), [])
        # The same emitted field fails when the old consumer is closed.
        old = object_schema({'revision': {'type': 'integer'}}, ['revision'], extra=False)
        added = copy.deepcopy(old); added['properties']['extra'] = {'type': 'string'}
        self.rejects(old, added, 'forbidden')

    def test_required_fields_output_types_and_enum_direction(self):
        old = object_schema({'revision': {'type': 'integer'}}, ['revision'])
        new = copy.deepcopy(old); new['required'] = []
        self.rejects(old, new, 'required')
        self.accepts(dict(type='number'), dict(type='integer'))
        self.rejects(dict(type='integer'), dict(type='number'), 'type')
        self.rejects(dict(type='string'), {})
        self.accepts({}, dict(type='string'))
        self.rejects(dict(type=['string', 'null']), dict(type=['string', 'boolean']))
        self.accepts(dict(enum=['a', 'b']), dict(enum=['a']))
        self.rejects(dict(enum=['a']), dict(enum=['a', 'b']), 'literal')
        self.rejects(dict(enum=[True]), dict(enum=[1]), 'literal')
        self.accepts(dict(const=3), dict(enum=[3]))
        self.rejects(dict(const=3), {})

    def test_size_bounds_are_producer_restrictions_not_input_acceptance(self):
        for key, old, tight, wide in (('minLength', 2, 3, 1), ('maxLength', 8, 7, 9),
                                    ('minItems', 2, 3, 1), ('maxItems', 8, 7, 9),
                                    ('minProperties', 2, 3, 1), ('maxProperties', 8, 7, 9)):
            with self.subTest(key=key):
                self.accepts({key: old}, {key: tight})
                self.rejects({key: old}, {key: wide}, 'size')
                self.rejects({key: old}, {}, 'size')
                self.accepts({}, {key: tight})
        self.rejects({'minItems': 1}, {'minItems': True}, 'Unproven')
        self.rejects({}, {'minItems': 4, 'maxItems': 3}, 'Unproven')

    def test_numeric_exclusive_bounds_cross_keyword_direction(self):
        self.accepts(dict(minimum=2), dict(exclusiveMinimum=2))
        self.rejects(dict(exclusiveMinimum=2), dict(minimum=2), 'numeric')
        self.accepts(dict(maximum=8), dict(exclusiveMaximum=8))
        self.rejects(dict(exclusiveMaximum=8), dict(maximum=8), 'numeric')
        self.accepts(dict(minimum=0, exclusiveMinimum=-1), dict(minimum=-1, exclusiveMinimum=0))
        self.rejects(dict(minimum=-1, exclusiveMinimum=0), dict(minimum=0, exclusiveMinimum=-1))
        self.rejects(dict(maximum=8), {})
        self.accepts({}, dict(minimum=0))
        self.rejects({}, dict(maximum=float('inf')), 'Unproven')

    def test_removed_optional_property_cannot_hide_wrong_typed_emission(self):
        old = object_schema({'optional': {'type': 'integer'}})
        # With open extra properties, removing its declaration permits
        # {optional: "wrong"}, which the old consumer rejects.
        self.rejects(old, object_schema({}), 'type')
        self.accepts(old, object_schema({}, extra=False))
        self.accepts(old, object_schema({}, extra={'type': 'integer'}))
        self.rejects(old, object_schema({}, extra={'type': 'number'}))
        old['required'] = ['optional']
        self.rejects(old, object_schema({}, extra=False), 'required')

    def test_extra_field_schema_and_namespace_crossover(self):
        old = object_schema({}, extra={'type': 'integer'})
        self.accepts(old, object_schema({'new': {'type': 'integer'}}, extra=False))
        self.rejects(old, object_schema({'new': {'type': 'string'}}, extra=False), 'type')
        self.rejects(old, object_schema({}, extra=True), 'type')
        self.accepts(old, object_schema({}, extra=False))
        self.rejects(object_schema({}, extra=False), object_schema({}), 'extra')
        self.accepts(object_schema({}), object_schema({}, extra=False))

    def test_array_items_and_exact_matrix_dimensions(self):
        old = dict(type='array', minItems=16, maxItems=16, items={'type': 'number'})
        self.accepts(old, dict(old, items={'type': 'integer'}))
        self.rejects(old, dict(old, maxItems=17), 'size')
        self.rejects(old, dict(old, items={'type': 'string'}), 'type')
        removed = copy.deepcopy(old); del removed['items']
        self.rejects(old, removed, 'type')
        self.accepts(dict(type='array'), old)

    def test_defaults_and_context_are_not_cosmetic_schema_data(self):
        for old, new in ((dict(default=3), dict(default=4)), (dict(default=3), {}),
                         ({}, dict(default=3)), (dict(default=True), dict(default=1)),
                         (dict(default={'description': 'old'}), dict(default={'description': 'new'}))):
            self.rejects(old, new, 'Default')
        old = object_schema({'optional': {'type': 'integer', 'default': 3}})
        self.rejects(old, object_schema({}, extra=False), 'default')
        nested = object_schema({'optional': object_schema({'nested': {'type': 'integer', 'default': 3}})})
        self.rejects(nested, object_schema({}, extra=False), 'default')
        removed = copy.deepcopy(nested); removed['properties']['optional'] = False
        self.rejects(nested, removed, 'default')
        old_extra = object_schema({}, extra={'type': 'integer', 'default': 3})
        self.rejects(old_extra, object_schema({}, extra=False), 'Default')
        base = manifest(object_schema({})); base['context_rules'] = {'query_default': 64, 'receipt_revision': 'original_base+1'}
        changed = copy.deepcopy(base); changed['context_rules']['query_default'] = 256
        self.assertTrue(compare_responses(base, changed))
        changed = copy.deepcopy(base); del changed['context_rules']
        self.assertTrue(compare_responses(base, changed))
        cosmetic = copy.deepcopy(old); cosmetic['description'] = 'Docs'; cosmetic['properties']['optional']['title'] = 'Title'
        self.accepts(old, cosmetic)

    def test_removed_method_variant_and_impossible_selected_result(self):
        for category in ('methods', 'variants'):
            changed = copy.deepcopy(CANDIDATE); del changed[category][next(iter(changed[category]))]
            self.assertTrue(compare_responses(CANDIDATE, changed))
        changed = copy.deepcopy(CANDIDATE); changed['methods']['world.inspect'] = False
        self.assertTrue(compare_responses(CANDIDATE, changed))

    def test_local_static_reference_definition_changes_are_checked(self):
        reference = {'$ref': '#/$defs/revision'}
        self.accepts(reference, reference, old_defs={'revision': {'type': 'integer', 'minimum': 0}},
                     new_defs={'revision': {'type': 'integer', 'minimum': 1}})
        self.rejects(reference, reference, old_defs={'revision': {'type': 'integer', 'minimum': 0}},
                     new_defs={'revision': {'type': 'number', 'minimum': 0}}, reason='type')
        self.accepts({'$ref': '#/$defs/a~1b~0c'}, {'type': 'string'}, old_defs={'a/b~c': {'type': 'string'}})
        self.accepts(reference, dict(reference, description='Updated docs'),
                     old_defs={'revision': {'type': 'integer'}}, new_defs={'revision': {'type': 'integer'}})
        # Used definitions change acceptance; unused additions do not.
        changed = copy.deepcopy(CANDIDATE); changed['$defs']['revision']['maximum'] += 1
        self.assertTrue(compare_responses(CANDIDATE, changed))
        changed = copy.deepcopy(CANDIDATE); changed['$defs']['unused'] = {'type': 'string'}
        self.assertEqual(compare_responses(CANDIDATE, changed), [])

    def test_unresolved_references_scopes_cycles_and_siblings_fail_closed(self):
        for schema in ({'$ref': 'https://example.test/schema'}, {'$ref': '#/properties/x'},
                       {'$ref': '#/$defs/array/1'}, {'$dynamicRef': '#/$defs/x'},
                       {'$recursiveRef': '#/$defs/x'}, {'$ref': '#/$defs/x', 'type': 'integer'},
                       {'$ref': '#/$defs/missing'}, {'$ref': '#/$defs/a~2b'},
                       {'$ref': '#/$defs/a%20b'}, {'type': 'integer', '$id': 'urn:example'}):
            with self.subTest(schema=schema): self.rejects(schema, schema, 'Unproven')
        cycle = {'x': {'$ref': '#/$defs/y'}, 'y': {'$ref': '#/$defs/x'}}
        self.rejects({'$ref': '#/$defs/x'}, {'$ref': '#/$defs/x'}, old_defs=cycle, new_defs=cycle, reason='Unproven')
        changed = copy.deepcopy(CANDIDATE); changed['$schema'] = 'https://json-schema.org/draft/2020-12/schema'
        with self.assertRaises(ValueError): compare_responses(CANDIDATE, changed)

    def test_unknown_union_and_negated_definition_context_changes_fail_closed(self):
        old = {'anyOf': [{'type': 'integer'}, {'type': 'null'}]}
        self.accepts(old, old)
        self.rejects(old, {'anyOf': [{'type': 'number'}, {'type': 'null'}]}, 'unsupported')
        self.rejects({'oneOf': [{'type': 'string'}, {'type': 'integer'}]},
                     {'oneOf': [{'type': 'string'}, {'type': 'number'}]}, 'unsupported')
        self.rejects({'pattern': '^a$'}, {'pattern': '.*'}, 'unsupported')
        self.rejects({}, {'exclusiveMaximum': 3, 'unknown_constraint': True}, 'unsupported')
        # Resolving references under an unsupported negative constraint prevents
        # an unchanged ref spelling from hiding changes to the pointed-at body.
        old = {'not': {'$ref': '#/$defs/blocked'}}
        self.rejects(old, old, old_defs={'blocked': {'type': 'integer'}},
                     new_defs={'blocked': {'type': 'number'}}, reason='unsupported')
        self.rejects({'future': {'$ref': '#/$defs/x'}}, {'future': {'$ref': '#/$defs/x'}},
                     old_defs={'x': {}}, new_defs={'x': {}}, reason='Unproven')

    def test_unchanged_unknown_constraints_can_interact_with_known_changes(self):
        # Old x must satisfy both integer and the string pattern constraint;
        # candidate removes its integer declaration but patternProperties still
        # admits {x: "accepted"}, despite additionalProperties:false.
        old = object_schema({'x': {'type': 'integer'}})
        old['patternProperties'] = {'^x$': {'type': 'string'}}
        new = object_schema({}, extra=False)
        new['patternProperties'] = copy.deepcopy(old['patternProperties'])
        self.rejects(old, new, 'unsupported')
        self.accepts(old, old)
        # An unchanged conditional/extension is likewise not assumed to have
        # no interaction with neighboring type/property/cardinality changes.
        for keyword, constraint in (('not', {'const': {}}),
                                    ('if', {'required': ['x']}),
                                    ('future_constraint', {'dependent_name': 'x'})):
            before = dict(old, **{keyword: constraint})
            after = copy.deepcopy(before); after['properties']['x']['type'] = 'number'
            self.rejects(before, after, 'unsupported')
        # An unchanged supported parent anyOf cannot justify a sibling rewrite.
        before = {'anyOf': [{'type': 'integer'}, {'type': 'null'}]}
        self.rejects(before, dict(before, type='integer'), 'unsupported')

    def test_bounded_expansion_findings_and_input(self):
        old = object_schema({'field'+str(i): {'type': 'integer'} for i in range(300)})
        findings = response_schema_compare(old, object_schema({}))
        self.assertEqual(len(findings), MAX_FINDINGS+1)
        self.assertIn('omitted', findings[-1]['reason'])
        deep = {'type': 'string'}
        for _ in range(70): deep = {'type': 'array', 'items': deep}
        self.rejects(deep, deep, 'bounded')

    def test_cli_direction_and_strict_json_exit_codes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); old_path = root/'old.json'; new_path = root/'new.json'; report = root/'report.json'
            old = manifest(object_schema({'revision': {'type': 'integer'}}, ['revision']))
            old_path.write_text(json.dumps(old), encoding='utf-8')
            command = [sys.executable, str(ROOT/'scripts/check_authoring_responses.py'), '--baseline', str(old_path),
                       '--candidate', str(new_path), '--report', str(report)]
            new = copy.deepcopy(old); new['methods']['world.inspect']['properties']['extra'] = {'type': 'string'}
            new_path.write_text(json.dumps(new), encoding='utf-8')
            result = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertTrue(json.loads(report.read_text())['passed'])
            new['methods']['world.inspect']['required'] = []
            new_path.write_text(json.dumps(new), encoding='utf-8')
            self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 1)
            for bad in ('{"format":"x","format":"y"}', '{"number":NaN}', '{"number":1e999}'):
                new_path.write_text(bad, encoding='utf-8')
                self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 2)
            new_path.write_bytes(b' '*(MAX_INPUT_BYTES+1))
            oversized = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(oversized.returncode, 2)
            self.assertIn('8 MiB', json.loads(oversized.stdout)['error'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
