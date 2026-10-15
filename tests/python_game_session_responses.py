#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable v1 session result mutations; no native execution evidence."""
import copy
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client.game_session_responses import game_session_response_contract, validate_game_session_result as validate
from poima_client.responses import ResponseContractError
from poima_client.runtime_response_checks_v1 import ResultChecksV1
from poima_client.workflow_responses import workflow_response_contract, validate_workflow_result
sys.path.insert(0, str(ROOT / 'scripts'))
from check_game_session_responses import compare_responses
from python_workflow_responses import CURRENT, ENTITY, MODULE, OTHER, SESSION, TYPE, samples

REMOVED = {'runtime.start', 'runtime.stop', 'runtime.gameplay.load_native', 'save.configure'}
QUERIES = {'runtime.raycast', 'runtime.hit_test'}


def ray_request(**changes):
    return dict(session_id=SESSION, tick=9, origin=[1, 2, 3], direction=[0, 0, -2], distance=10, **changes)


def ray_result(method='runtime.raycast'):
    hit = dict(entity=ENTITY, fraction=.5, distance=5, position=[1, 2, -2], normal=[0, 0, 1], triangle=None)
    if method == 'runtime.hit_test':
        hit.update(region_entity=OTHER, region='head')
    return dict(session_id=SESSION, tick=9, hit=hit)


class GameSessionResponses(unittest.TestCase):
    def reject(self, method, result, params=None, context=None):
        before = copy.deepcopy((result, params, context))
        with self.assertRaises(ResponseContractError) as failure:
            validate(method, result, params, context)
        self.assertIs(failure.exception.result, result)
        self.assertEqual((result, params, context), before)

    def test_frozen_resources_and_transitive_candidate_definitions(self):
        public = ROOT / 'docs/contracts/runtime-game-session-responses-v1.json'
        packaged = ROOT / 'tools/python/poima_client/runtime_game_session_responses.v1.json'
        self.assertEqual(public.read_bytes(), packaged.read_bytes())
        stable = game_session_response_contract()
        candidate = workflow_response_contract()
        self.assertEqual(stable, json.loads(public.read_text(encoding='utf-8')))
        self.assertEqual(stable['$defs'], candidate['$defs'])
        self.assertEqual(set(stable['methods']), (set(candidate['methods']) - REMOVED) | QUERIES)
        self.assertEqual(len(stable['methods']), 19)
        for method in set(stable['methods']) - QUERIES:
            self.assertEqual(stable['methods'][method], candidate['methods'][method])
            result = samples()[method]
            self.assertIs(validate(method, result), result)
        for method in REMOVED:
            self.reject(method, samples()[method])

    def test_detached_manifests_cannot_repoint_stable_checks(self):
        detached = game_session_response_contract()
        detached['methods'].clear()
        detached['$defs'].clear()
        future = workflow_response_contract()
        future['methods']['save.status']['properties']['generation']['minimum'] = 10
        future_checks = ResultChecksV1(future)
        value = samples()['save.status']
        with self.assertRaises(ResponseContractError):
            future_checks.validate('save.status', value)
        self.assertIs(validate('save.status', value), value)
        self.assertIs(validate_workflow_result('save.status', value), value)
        self.assertEqual(len(game_session_response_contract()['methods']), 19)

    def test_full_compact_and_dynamic_context_remain_supported(self):
        full = samples()['runtime.gameplay.inspect']
        params = dict(session_id=SESSION, tick=9, include_schema=True)
        self.assertIs(validate('runtime.gameplay.inspect', full, params,
                      {'profile': 'readonly_game', 'gameplay_schema': MODULE['schema']}), full)
        component = samples()['runtime.component.get']
        context = {'profile': 'readonly_game', 'component_schemas': [component['schema']]}
        self.assertIs(validate('runtime.component.get', component,
                      dict(session_id=SESSION, tick=9, id=ENTITY, type=TYPE), context), component)
        wrong = copy.deepcopy(context)
        wrong['component_schemas'][0]['fields'][0]['default'] = 2
        self.reject('runtime.component.get', component, None, wrong)
        for bad in ({'profile': 'shared_editor'}, {'latest_tick': 9}, {'component_schemas': [True]}):
            self.reject('runtime.status', samples()['runtime.status'], None, bad)

    def test_query_hit_miss_geometry_region_and_inside_variants(self):
        for method in QUERIES:
            request = ray_request()
            result = ray_result(method)
            self.assertIs(validate(method, result, request), result)
            missed = dict(result, hit=None)
            self.assertIs(validate(method, missed, request), missed)
            inside = copy.deepcopy(result)
            inside['hit'].update(fraction=0, distance=0, position=request['origin'], normal=None)
            self.assertIs(validate(method, inside, request), inside)
            if method == 'runtime.hit_test':
                result['hit'].update(region_entity=None, region=None, triangle=0)
                self.assertIs(validate(method, result, request), result)
            else:
                result['hit']['triangle'] = 0
                self.assertIs(validate(method, result, request), result)

    def test_query_original_guards_and_numeric_types(self):
        for method in QUERIES:
            result = ray_result(method)
            for changed in ({'session_id': CURRENT}, {'tick': 10}, {'tick': 9.0}, {'tick': True},
                            {'origin': [True, 2, 3]}, {'direction': [0, 0, 0]},
                            {'direction': [0, 0, 1e-13]}, {'distance': False}, {'distance': .0001},
                            {'distance': 10001}, {'origin': [1000001, 2, 3]},
                            {'direction': [0, 0, -1000001]}, {'ignore': [ENTITY, ENTITY]},
                            {'ignore': ['invalid']}, {'ignore': None}):
                with self.subTest(method=method, changed=changed):
                    request = ray_request()
                    request.update(changed)
                    self.reject(method, result, request)
            request = ray_request()
            del request['tick']
            self.reject(method, result, request)
            request = ray_request()
            request.update(origin=[1000000, 0, 0], direction=[1, 0, 0])
            self.reject(method, dict(result, hit=None), request)
            self.reject(method, result, dict(ray_request(), unexpected=True))

    def test_query_distance_position_and_ignored_identity_joins(self):
        for method in QUERIES:
            for key, value in (('distance', 6), ('fraction', .6), ('position', [2, 2, -2])):
                wrong = ray_result(method)
                wrong['hit'][key] = value
                self.reject(method, wrong, ray_request())
            self.reject(method, ray_result(method), ray_request(ignore=[ENTITY]))
        self.reject('runtime.hit_test', ray_result('runtime.hit_test'), ray_request(ignore=[OTHER]))
        # Native ignore order is unconstrained; it is not the sorted query cursor contract.
        request = ray_request(ignore=[CURRENT, SESSION])
        result = ray_result()
        self.assertIs(validate('runtime.raycast', result, request), result)

    def test_query_region_normal_triangle_and_scalar_shapes(self):
        for changed in ({'region': None}, {'region_entity': None}, {'triangle': 1}, {'region': 'BadName'}):
            result = ray_result('runtime.hit_test')
            result['hit'].update(changed)
            self.reject('runtime.hit_test', result)
        for method in QUERIES:
            for changed in ({'fraction': True}, {'fraction': 1.1}, {'distance': -1},
                            {'normal': None}, {'normal': [0, 0, 0]}, {'normal': [0, 0, True]},
                            {'triangle': True}, {'triangle': 1.0}, {'triangle': 4294967296},
                            {'position': [1, 2]}, {'entity': 'not-an-id'}):
                result = ray_result(method)
                result['hit'].update(changed)
                self.reject(method, result)
            result = ray_result(method)
            result['hit'].update(fraction=0, distance=1)
            self.reject(method, result)

    def test_query_tolerance_retains_realistic_float32_and_small_rays(self):
        request = dict(session_id=SESSION, tick=9, origin=[999990, 0, 0], direction=[1, 0, 0], distance=10)
        result = ray_result()
        result['hit'].update(position=[999995.03125, 0, 0])
        self.assertIs(validate('runtime.raycast', result, request), result)
        result['hit']['position'][0] = 999997
        self.reject('runtime.raycast', result, request)
        request = dict(session_id=SESSION, tick=9, origin=[0, 0, 0], direction=[0, 0, -1e-12], distance=.001)
        result = ray_result()
        result['hit'].update(distance=.0005, position=[0, 0, -.0005])
        self.assertIs(validate('runtime.raycast', result, request), result)

    def test_query_diagnostics_and_response_only_boundaries(self):
        result = ray_result('runtime.hit_test')
        result.update(diagnostic={'future': [True, 2, None]})
        result['hit']['extra'] = 'diagnostic'
        params = ray_request()
        context = {'profile': 'readonly_game'}
        before = copy.deepcopy((result, params, context))
        self.assertIs(validate('runtime.hit_test', result, params, context), result)
        self.assertEqual((result, params, context), before)
        # No original request means no invented latest boundary or input ray.
        self.assertIs(validate('runtime.hit_test', result), result)
        result['tick'] = 10
        self.assertIs(validate('runtime.hit_test', result), result)
        self.reject('runtime.hit_test', result, params, context)


class GameSessionOutputEvolution(unittest.TestCase):
    def test_fixed_resource_and_recursive_definition_identity(self):
        manifest = game_session_response_contract()
        before = copy.deepcopy(manifest)
        self.assertEqual(compare_responses(manifest, copy.deepcopy(manifest)), [])
        self.assertEqual(manifest, before)
        changed = copy.deepcopy(manifest)
        changed['$defs']['animation']['properties']['speed']['maximum'] = 4
        findings = compare_responses(manifest, changed)
        self.assertTrue(findings)
        self.assertTrue(any('Unproven' in row['reason'] for row in findings))

    def test_output_containment_direction_and_required_fields(self):
        baseline = game_session_response_contract()
        narrowed = copy.deepcopy(baseline)
        narrowed['methods']['save.status']['properties']['generation']['maximum'] = 100
        self.assertEqual(compare_responses(baseline, narrowed), [])
        self.assertTrue(compare_responses(narrowed, baseline))
        removed = copy.deepcopy(baseline)
        removed['methods']['save.status']['required'].remove('generation')
        self.assertTrue(compare_responses(baseline, removed))
        changed = copy.deepcopy(baseline)
        changed['methods']['save.status']['properties']['write_mode']['enum'].append('future')
        self.assertTrue(compare_responses(baseline, changed))
        # Old open diagnostics permit an additive field on a nonrecursive result.
        changed = copy.deepcopy(baseline)
        changed['methods']['runtime.inspect']['properties']['diagnostic'] = {'type': 'string'}
        self.assertEqual(compare_responses(baseline, changed), [])

    def test_metadata_and_selected_method_membership_fail_closed(self):
        baseline = game_session_response_contract()
        for key, value in (('status', 'prospective'), ('id', 'other'), ('contract_version', True),
                           ('profiles', ['readonly_game']), ('context_keys', [])):
            changed = copy.deepcopy(baseline)
            changed[key] = value
            with self.assertRaises(ValueError):
                compare_responses(baseline, changed)
        changed = copy.deepcopy(baseline)
        changed['query_checks']['input'] = 'different semantics'
        self.assertTrue(compare_responses(baseline, changed))
        changed = copy.deepcopy(baseline)
        del changed['methods']['runtime.raycast']
        with self.assertRaises(ValueError):
            compare_responses(baseline, changed)
        changed = copy.deepcopy(baseline)
        del changed['context_keys']
        with self.assertRaises(ValueError):
            compare_responses(changed, changed)

    def test_unknown_references_and_unrelated_definitions_never_pass_identity(self):
        for schema in ({'$ref': '#/$defs/missing'}, {'$ref': 'https://example.test/schema'},
                       {'$dynamicRef': '#/$defs/id'}, {'$ref': '#/$defs/id', 'type': 'string'},
                       {'$schema': 'https://json-schema.org/draft/2020-12/schema'},
                       {'customKeyword': {'$ref': '#/$defs/id'}}):
            manifest = game_session_response_contract()
            manifest['methods']['runtime.raycast'] = schema
            with self.assertRaises(ValueError):
                compare_responses(manifest, copy.deepcopy(manifest))
        manifest = game_session_response_contract()
        manifest['$defs']['unselected'] = {'type': 'string'}
        with self.assertRaises(ValueError):
            compare_responses(manifest, manifest)

    def test_cli_hardlink_report_cannot_destroy_either_input(self):
        tool = ROOT / 'scripts/check_game_session_responses.py'
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            baseline, candidate = folder / 'old.json', folder / 'new.json'
            data = json.dumps(game_session_response_contract()).encode('utf-8')
            baseline.write_bytes(data)
            candidate.write_bytes(data)
            for index, source in enumerate((baseline, candidate)):
                alias = folder / ('report-' + str(index) + '.json')
                try:
                    os.link(source, alias)
                except OSError as error:
                    self.skipTest('Filesystem hard links unavailable: ' + str(error))
                self.assertNotEqual(source.resolve(), alias.resolve())
                run = subprocess.run([sys.executable, '-B', str(tool), '--baseline', str(baseline),
                                      '--candidate', str(candidate), '--report', str(alias)],
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, 2)
                self.assertEqual(baseline.read_bytes(), data)
                self.assertEqual(candidate.read_bytes(), data)
                self.assertEqual(alias.read_bytes(), data)

    def test_cli_reports_distinguish_compatible_changed_and_invalid(self):
        tool = ROOT / 'scripts/check_game_session_responses.py'
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary) / 'old.json'
            candidate = Path(temporary) / 'new.json'
            report = Path(temporary) / 'report.json'
            manifest = game_session_response_contract()
            base.write_text(json.dumps(manifest), encoding='utf-8')
            for expected, change in ((0, 'same'), (1, 'required'), (2, 'identity')):
                changed = copy.deepcopy(manifest)
                if change == 'required':
                    changed['methods']['save.status']['required'].remove('generation')
                elif change == 'identity':
                    changed['status'] = 'prospective'
                candidate.write_text(json.dumps(changed), encoding='utf-8')
                run = subprocess.run([sys.executable, '-B', str(tool), '--baseline', str(base),
                                      '--candidate', str(candidate), '--report', str(report)],
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, expected, run.stderr + run.stdout)
                self.assertEqual(json.loads(report.read_text())['passed'], expected == 0)
            original = base.read_bytes()
            run = subprocess.run([sys.executable, '-B', str(tool), '--baseline', str(base),
                                  '--candidate', str(candidate), '--report', str(base)],
                                 capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 2)
            self.assertEqual(base.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
