#!/usr/bin/env python3
"""Core discovery projection must not hide incompatible oneOf branches."""
# SPDX-License-Identifier: Apache-2.0
import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from check_authoring_api import compare_discovery
from project_authoring_api import project

BASELINE = json.loads((ROOT / 'docs/contracts/authoring-core-v1.candidate.json').read_text(encoding='utf-8'))


def source():
    result = copy.deepcopy(BASELINE)
    result['schema_revision'] = result.pop('source_schema_revision')
    for key in ('format', 'status', 'contract_version'):
        result.pop(key)
    # The native selector accepts built-ins and registered custom components.
    for name in ('entity.get', 'entity.query'):
        result['methods'][name]['properties']['component'] = {
            'anyOf': [{'enum': ['Transform', 'Camera', 'MeshRenderer']},
                      {'type': 'string', 'pattern': '^game:[0-9a-f]{32}$'}]}
    return result


def mutations(document):
    return document['methods']['world.transact']['properties']['ops']['items']['oneOf']


def mutation(document, name, component=None):
    return next(branch for branch in mutations(document)
                if branch['properties']['op'].get('const') == name
                and (component is None or branch['properties']['type'].get('const') == component))


class Projection(unittest.TestCase):
    def test_checked_in_baseline_reproduces_from_supported_native_selectors(self):
        original = source()
        before = copy.deepcopy(original)
        self.assertEqual(project(original, baseline=True), BASELINE)
        self.assertEqual(project({'jsonrpc': '2.0', 'id': 1, 'result': original}, baseline=True), BASELINE)
        self.assertEqual(compare_discovery(BASELINE, project(original)), [])
        self.assertEqual(original, before, 'Projection changed authoritative discovery input.')

    def test_provably_outside_scope_additions_do_not_pin_other_components(self):
        original = source()
        optional = copy.deepcopy(mutation(original, 'component.set', 'Transform'))
        optional['properties']['type'] = {'const': 'Camera'}
        custom = copy.deepcopy(optional)
        custom['properties']['type'] = {'type': 'string', 'pattern': '^game:[0-9a-f]{32}$'}
        unrelated = copy.deepcopy(mutation(original, 'entity.create'))
        unrelated['properties']['op'] = {'const': 'template.set'}
        mutations(original).extend((optional, custom, unrelated))
        original['methods']['renderer.experimental'] = {'type': 'object', 'required': ['new_field']}
        original['components']['ExperimentalRenderer'] = {'type': 'boolean'}
        projected = project(original)
        self.assertEqual(len(mutations(projected)), 5)
        self.assertEqual(compare_discovery(BASELINE, projected), [])
        self.assertEqual(project(original, baseline=True), BASELINE)

    def test_broad_operation_overlap_survives_projection_and_fails_gate(self):
        for discriminator in ({'enum': ['entity.create', 'future.op']}, {'type': 'string'}, {}):
            with self.subTest(discriminator=discriminator):
                candidate = source()
                overlap = copy.deepcopy(mutation(candidate, 'entity.create'))
                overlap['properties']['op'] = discriminator
                mutations(candidate).append(overlap)
                projected = project(candidate)
                self.assertEqual(len(mutations(projected)), 6, 'Potential native oneOf overlap was hidden.')
                self.assertTrue(compare_discovery(BASELINE, projected), 'Unprovable overlap was certified compatible.')

    def test_broad_transform_overlap_survives_projection_and_fails_gate(self):
        for discriminator in ({'enum': ['Transform', 'Camera']}, {'type': 'string'},
                              {'type': 'string', 'pattern': '^(Transform|Camera)$'}):
            with self.subTest(discriminator=discriminator):
                candidate = source()
                overlap = copy.deepcopy(mutation(candidate, 'component.set', 'Transform'))
                overlap['properties']['type'] = discriminator
                mutations(candidate).append(overlap)
                projected = project(candidate)
                self.assertEqual(len(mutations(projected)), 6)
                self.assertTrue(compare_discovery(BASELINE, projected))

    def test_references_cannot_prove_discriminator_branches_outside_scope(self):
        for reference in ('$ref', '$dynamicRef', '$recursiveRef'):
            for placement in ('outside_operation', 'custom_component_type'):
                with self.subTest(reference=reference, placement=placement):
                    candidate = source()
                    if placement == 'outside_operation':
                        branch = copy.deepcopy(mutation(candidate, 'entity.create'))
                        branch['properties']['op'] = {'const': 'template.set'}
                        # Drafts that ignore reference siblings need not enforce
                        # the apparently disjoint operation constraint.
                        branch[reference] = '#/$defs/possible_core_overlap'
                    else:
                        branch = copy.deepcopy(mutation(candidate, 'component.set', 'Transform'))
                        branch['properties']['type'] = {
                            'type': 'string', 'pattern': '^game:[0-9a-f]{32}$',
                            reference: '#/$defs/possible_core_overlap'}
                    mutations(candidate).append(branch)
                    projected = project(candidate)
                    self.assertEqual(len(mutations(projected)), 6,
                                     'Reference-bearing branch was incorrectly discarded.')
                    self.assertEqual(mutations(projected)[-1], branch,
                                     'Projection changed the unresolved native branch.')
                    self.assertTrue(compare_discovery(BASELINE, projected),
                                    'Unresolved reference overlap was certified compatible.')

    def test_removed_scope_entries_remain_missing_and_fail_gate(self):
        for category, name in (('methods', 'world.transact'), ('methods', 'entity.get'),
                               ('components', 'Transform')):
            with self.subTest(category=category, name=name):
                candidate = source()
                del candidate[category][name]
                projected = project(candidate)
                self.assertNotIn(name, projected[category])
                self.assertTrue(compare_discovery(BASELINE, projected))
        candidate = source()
        mutations(candidate).remove(mutation(candidate, 'entity.rename'))
        projected = project(candidate)
        self.assertEqual(len(mutations(projected)), 4)
        self.assertTrue(compare_discovery(BASELINE, projected))

    def test_candidate_transform_selector_is_not_rewritten_into_acceptance(self):
        candidate = source()
        candidate['methods']['entity.get']['properties']['component'] = {'enum': ['Camera']}
        projected = project(candidate)
        self.assertEqual(projected['methods']['entity.get']['properties']['component'], {'enum': ['Camera']})
        self.assertTrue(compare_discovery(BASELINE, projected))

    def test_baseline_rejects_missing_transform_or_unproven_selectors(self):
        selectors = ({'enum': ['Camera']}, {'type': 'string', 'pattern': '^game:[0-9a-f]{32}$'},
                     {'anyOf': [{'enum': ['Camera']}]}, {'enum': 'Transform'},
                     {'enum': ['Transform'], 'not': {'const': 'Transform'}})
        for name in ('entity.get', 'entity.query'):
            for selector in selectors:
                with self.subTest(name=name, selector=selector):
                    candidate = source()
                    candidate['methods'][name]['properties']['component'] = selector
                    with self.assertRaises((ValueError, KeyError, TypeError)):
                        project(candidate, baseline=True)
            candidate = source()
            del candidate['methods'][name]['properties']['component']
            with self.assertRaises((ValueError, KeyError, TypeError)):
                project(candidate, baseline=True)

    def test_five_branches_cannot_hide_a_missing_core_discriminator(self):
        candidate = source()
        mutations(candidate).remove(mutation(candidate, 'entity.rename'))
        mutations(candidate).append(copy.deepcopy(mutation(candidate, 'entity.create')))
        self.assertEqual(len(mutations(candidate)), 5)
        with self.assertRaises(ValueError):
            project(candidate, baseline=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
