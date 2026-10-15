#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable isolation checks; no native execution or release qualification."""
import copy
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client.responses import ResponseContractError
from poima_client.runtime_response_checks_v1 import ResultChecksV1
from poima_client.workflow_responses import validate_workflow_result, workflow_response_contract
from python_workflow_responses import CURRENT, MODULE, SESSION, samples


class ImmutableRuntimeResponseContract(unittest.TestCase):
    def test_candidate_manifest_matches_existing_artifact(self):
        expected = json.loads((ROOT / 'docs/contracts/runtime-save-responses-v1.candidate.json').read_text(encoding='utf-8'))
        actual = workflow_response_contract()
        self.assertEqual(json.dumps(actual, sort_keys=True), json.dumps(expected, sort_keys=True))
        self.assertEqual(len(actual['methods']), 21)
        for method, result in samples().items():
            with self.subTest(method=method):
                self.assertIs(validate_workflow_result(method, result), result)

    def test_input_manifest_and_returned_copy_cannot_change_checks(self):
        original = workflow_response_contract()
        checks = ResultChecksV1(original)
        original['methods'].clear()
        original['$defs']['id']['pattern'] = 'wrong'
        returned = checks.contract()
        returned['methods']['save.status']['properties']['generation']['type'] = 'string'
        returned['profiles'].clear()
        for method, result in samples().items():
            with self.subTest(method=method):
                self.assertIs(checks.validate(method, result), result)
        invalid = dict(samples()['save.status'], generation='0')
        with self.assertRaises(ResponseContractError):
            checks.validate('save.status', invalid)
        self.assertEqual(checks.contract(), workflow_response_contract())

    def test_full_gameplay_inspect_does_not_depend_on_bootstrap_method(self):
        manifest = workflow_response_contract()
        methods = manifest['methods']
        self.assertEqual(methods['runtime.gameplay.load_native']['properties']['module'],
                         methods['runtime.gameplay.inspect']['properties']['module']['anyOf'][0])
        for method in ('runtime.start', 'runtime.stop', 'runtime.gameplay.load_native', 'save.configure'):
            del methods[method]
        checks = ResultChecksV1(manifest)
        full = samples()['runtime.gameplay.inspect']
        params = dict(session_id=SESSION, tick=full['tick'], include_schema=True)
        self.assertIs(checks.validate('runtime.gameplay.inspect', full, params), full)
        compact = copy.deepcopy(full)
        compact['module'] = dict(identity=MODULE['schema']['identity'],
                                 assembly_sha256=MODULE['assembly_sha256'], values=MODULE['values'])
        self.assertIs(checks.validate('runtime.gameplay.inspect', compact,
                      dict(session_id=SESSION, tick=compact['tick'])), compact)
        invalid = copy.deepcopy(full)
        invalid['module'].pop('schema')
        with self.assertRaises(ResponseContractError):
            checks.validate('runtime.gameplay.inspect', invalid, params)
        with self.assertRaises(ResponseContractError):
            checks.validate('runtime.gameplay.load_native', samples()['runtime.gameplay.load_native'])

    def test_instances_and_profiles_remain_independent_under_concurrent_use(self):
        original = workflow_response_contract()
        strict = copy.deepcopy(original)
        strict['methods']['save.status']['properties']['generation']['minimum'] = 1
        ordinary = ResultChecksV1(original, profiles=['source_world'])
        different = ResultChecksV1(strict, profiles=['readonly_game'])
        value = samples()['save.status']
        before = copy.deepcopy(value)

        def run(index):
            if index % 2 == 0:
                self.assertIs(ordinary.validate('save.status', value, {}, {'profile': 'source_world'}), value)
            else:
                with self.assertRaises(ResponseContractError):
                    different.validate('save.status', value, {}, {'profile': 'readonly_game'})
            with self.assertRaises(ResponseContractError):
                ordinary.validate('save.status', value, {}, {'profile': 'readonly_game'})
            return True

        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertTrue(all(pool.map(run, range(80))))
        self.assertEqual(value, before)
        self.assertIs(validate_workflow_result('save.status', value), value)

    def test_request_context_and_failure_identity_are_preserved(self):
        checks = ResultChecksV1(workflow_response_contract())
        result = copy.deepcopy(samples()['save.load'])
        request = dict(request_id=CURRENT, configuration_generation=1, slot='slot', expected_generation=1,
                       revision=4, expected_session_id=SESSION, expected_tick=9,
                       expected_gameplay_revision=2, new_session_id=CURRENT)
        context = {'profile': 'readonly_game'}
        before = copy.deepcopy((result, request, context))
        self.assertIs(checks.validate('save.load', result, request, context), result)
        self.assertEqual((result, request, context), before)
        request['expected_tick'] = 9.0
        failure_input = copy.deepcopy((result, request, context))
        with self.assertRaises(ResponseContractError) as failure:
            checks.validate('save.load', result, request, context)
        self.assertIs(failure.exception.result, result)
        self.assertEqual(type(failure.exception.params['expected_tick']), float)
        self.assertEqual((result, request, context), failure_input)
        self.assertEqual(failure.exception.request_id, CURRENT)

    def test_frozen_literal_values_keep_json_array_and_object_types(self):
        manifest = workflow_response_contract()
        manifest['methods']['save.status']['properties']['diagnostic'] = {
            'const': {'items': [True, 1, 'one']}}
        checks = ResultChecksV1(manifest)
        value = dict(samples()['save.status'], diagnostic={'items': [True, 1, 'one']})
        self.assertIs(checks.validate('save.status', value), value)
        value['diagnostic']['items'][-1] = 'different'
        with self.assertRaises(ResponseContractError):
            checks.validate('save.status', value)


if __name__ == '__main__':
    unittest.main()
