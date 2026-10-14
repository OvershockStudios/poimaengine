#!/usr/bin/env python3
"""Portable tests for prospective request projections; optional native-capture integration.

The small unit fixtures are explicit consumer schemas, not native execution evidence.
Opt-in integration reads retained discovery only; this script launches no engine.
"""
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations
import argparse
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT/'tests/fixtures/workflow-api'
FILES = {'source_world':'standalone-authoring.json',
         'source_shared_player':'shared-authoring.json',
         'readonly_game':'shared-readonly-game.json'}
TOOL = CHECKER = UNIT_SELECTION = UNIT_DISCOVERY = PRODUCTION_SELECTION = ARGS = None
INTEGRATION = []


def module(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise ValueError('Cannot load portable tool: '+str(path))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def scope_fixture(scope):
    value=deepcopy(UNIT_DISCOVERY)
    if scope!='source_world':value['session_scope']='shared_headless'
    if scope=='readonly_game':value['mode']='read_only_runtime';value['read_only']=True
    return value


class Projection(unittest.TestCase):
    def setUp(self):
        self.discovery=deepcopy(UNIT_DISCOVERY);self.selection=deepcopy(UNIT_SELECTION)
        self.before=self.project()

    def project(self,discovery=None,selection=None,scope='source_world'):
        return TOOL.project(self.discovery if discovery is None else discovery,
                            self.selection if selection is None else selection,scope,CHECKER)

    def findings(self):return TOOL.compare(self.before,self.project(),CHECKER)

    def lighting(self,value):
        return value['spawn_templates']['hierarchy']['additionalProperties']['properties']['components']['properties']['LightingEnvironment']

    def test_portable_fixture_and_three_scopes_are_explicit(self):
        self.assertEqual(len(UNIT_DISCOVERY['methods']),3)
        self.assertIn('not native discovery evidence',UNIT_DISCOVERY['fixture'])
        for scope in FILES:
            value=scope_fixture(scope);result=self.project(value,scope=scope)
            self.assertEqual(result['methods'],value['methods'])
            self.assertEqual(result['components'],value['components'])
            self.assertEqual(result['context']['session_scope']['present'],scope!='source_world')
            self.assertEqual(TOOL.compare(result,deepcopy(result),CHECKER),[])
            self.assertEqual(result['status'],'candidate')

    def test_prospective_selection_is_exactly_twenty_one_methods_in_three_scopes(self):
        expected={'runtime.start','runtime.status','runtime.inspect','runtime.stop','runtime.step',
            'runtime.entity','runtime.observe','runtime.components','runtime.component.get',
            'runtime.component.query','runtime.gameplay.load_native','runtime.gameplay.inspect',
            'runtime.ui.inspect','runtime.ui.activate','runtime.save.status','runtime.save.result',
            'save.configure','save.status','save.inspect','save.write','save.load'}
        self.assertEqual(PRODUCTION_SELECTION['status'],'prospective')
        self.assertEqual(set(PRODUCTION_SELECTION['methods']),expected)
        self.assertEqual(PRODUCTION_SELECTION['components'],[])
        self.assertEqual(set(PRODUCTION_SELECTION['profiles']),set(FILES))
        for profile in PRODUCTION_SELECTION['profiles'].values():
            self.assertEqual(set(profile['selected_methods']),expected)

    def test_projection_copies_without_mutating_or_aliasing_source(self):
        original=deepcopy(self.discovery);result=self.project()
        result['methods']['runtime.start']['properties']['revision']['minimum']=10
        result['context']['limits']['value']['entities']=0
        result['context']['spawn_templates']['value']['runtime']='modified'
        self.assertEqual(self.discovery,original)

    def test_closed_optional_request_addition_and_bound_widening_pass(self):
        value=self.discovery['methods']['runtime.start']
        value['properties']['optional_flag']={'type':'boolean','default':False}
        value['properties']['revision']['maximum']+=1
        self.assertEqual(self.findings(),[])

    def test_unselected_schema_additions_and_schema_revision_are_not_selected(self):
        self.discovery['methods']['not.selected']={'type':'object'}
        self.discovery['components']['NotSelected']={'type':'boolean'}
        self.discovery['schema_revision']=1000
        self.assertEqual(self.findings(),[])

    def test_added_required_guard_rejects_previously_valid_requests(self):
        value=self.discovery['methods']['runtime.start']
        value['properties']['guard']={'type':'integer'};value['required'].append('guard')
        self.assertTrue(self.findings())

    def test_removed_request_property_and_narrowed_integer_bound_fail(self):
        for change in ('remove','bound'):
            with self.subTest(change=change):
                self.discovery=deepcopy(UNIT_DISCOVERY)
                node=self.discovery['methods']['runtime.step']['properties']
                if change=='remove':del node['expected_tick']
                else:node['ticks']['maximum']=100
                self.assertTrue(self.findings())

    def test_guard_wire_type_and_nullable_load_selection_remain_distinct(self):
        self.discovery['methods']['runtime.start']['properties']['revision']['type']='boolean'
        self.assertTrue(self.findings());self.discovery=deepcopy(UNIT_DISCOVERY)
        self.discovery['methods']['save.load']['properties']['expected_session_id']['anyOf'].pop()
        self.assertTrue(self.findings())

    def test_changed_defaults_omission_and_boolean_integer_literals_fail(self):
        for change in ('value','omit','bool_integer'):
            with self.subTest(change=change):
                self.discovery=deepcopy(UNIT_DISCOVERY)
                node=self.discovery['methods']['save.load']['properties']['allow_recovery']
                if change=='value':node['default']=True
                elif change=='omit':del node['default']
                else:node['default']=0
                self.assertTrue(self.findings())

    def test_schema_annotations_are_cosmetic_but_default_payload_is_not(self):
        self.discovery['methods']['runtime.start']['description']='A new description'
        self.assertEqual(self.findings(),[])
        node=self.discovery['methods']['runtime.start']['properties']
        node['literal']={'default':{'description':'literal','flag':True}}
        self.before=self.project();node['literal']['default']['description']='changed data'
        self.assertTrue(self.findings());node['literal']['default']['description']='literal'
        node['literal']['default']['flag']=1;self.assertTrue(self.findings())

    def test_changed_unknown_pattern_constraint_is_retained_and_fails(self):
        node=self.discovery['methods']['runtime.start']['properties']['session_id'];node['pattern']='^other$'
        result=self.project();self.assertEqual(result['methods']['runtime.start']['properties']['session_id']['pattern'],'^other$')
        self.assertTrue(TOOL.compare(self.before,result,CHECKER))

    def test_optional_field_on_open_request_is_not_assumed_safe(self):
        self.discovery['methods']['runtime.start']['additionalProperties']=True;self.before=self.project()
        self.discovery['methods']['runtime.start']['properties']['new']={'type':'boolean'}
        self.assertTrue(self.findings())

    def test_overlapping_oneof_extension_is_retained_and_fails(self):
        self.discovery['methods']['runtime.start']={'oneOf':[{'type':'string'}]};self.before=self.project()
        self.discovery['methods']['runtime.start']['oneOf'].append({'type':'string','minLength':1})
        result=self.project();self.assertEqual(len(result['methods']['runtime.start']['oneOf']),2)
        self.assertTrue(TOOL.compare(self.before,result,CHECKER))

    def test_component_constraint_tightening_is_checked(self):
        self.discovery['components']['Transform']['properties']['position']['items']['maximum']=10
        self.assertTrue(self.findings())

    def test_selected_method_or_component_omission_fails_projection(self):
        for category,name in [('methods','runtime.start'),('components','Transform')]:
            with self.subTest(category=category):
                value=deepcopy(UNIT_DISCOVERY);del value[category][name]
                with self.assertRaisesRegex(ValueError,'Missing selected'):self.project(value)

    def test_invalid_membership_and_unknown_profile_fail(self):
        with self.assertRaisesRegex(ValueError,'Unknown profile'):self.project(scope='invented')
        for names in [['runtime.start','runtime.start'],['save.load','runtime.start'],['not.selected']]:
            with self.subTest(names=names):
                selected=deepcopy(UNIT_SELECTION);selected['profiles']['source_world']['selected_methods']=names
                with self.assertRaises(ValueError):self.project(selection=selected)

    def test_smaller_or_changed_profile_policy_cannot_certify_original_selection(self):
        selected=deepcopy(self.selection);selected['profiles']['source_world']['selected_methods'].remove('runtime.step')
        self.assertTrue(TOOL.compare(self.before,self.project(selection=selected),CHECKER))
        selected=deepcopy(self.selection);selected['context_schema_paths']=[['spawn_templates','hierarchy']]
        self.assertTrue(TOOL.compare(self.before,self.project(selection=selected),CHECKER))

    def test_tampered_stored_method_and_context_membership_fail(self):
        for change in ('method','context','presence'):
            with self.subTest(change=change):
                result=deepcopy(self.before)
                if change=='method':del result['methods']['runtime.start']
                elif change=='context':del result['context']['limits']
                else:result['context']['session_scope']={'present':False,'value':None}
                with self.assertRaises(ValueError):TOOL.compare(self.before,result,CHECKER)

    def test_exact_non_schema_context_siblings_and_full_limits_are_kept(self):
        for change in ('description_literal','budget','limits','new_limit','presence'):
            with self.subTest(change=change):
                self.discovery=deepcopy(UNIT_DISCOVERY)
                if change=='description_literal':self.discovery['spawn_templates']['runtime']='a changed rule'
                elif change=='budget':self.discovery['spawn_templates']['max_templates']+=1
                elif change=='limits':self.discovery['limits']['entities']+=1
                elif change=='new_limit':self.discovery['limits']['new']=1
                else:self.discovery['session_scope']=None
                if change=='presence':
                    with self.assertRaisesRegex(ValueError,'presence'):self.project()
                else:self.assertTrue(self.findings())

    def test_identically_mislabeled_stored_scopes_still_fail(self):
        result=deepcopy(self.before);result['context']['mode']['value']='read_only_runtime'
        with self.assertRaisesRegex(ValueError,'expected profile context'):TOOL.compare(result,deepcopy(result),CHECKER)
        with self.assertRaisesRegex(ValueError,'profile context'):self.project(scope_fixture('readonly_game'),scope='source_shared_player')

    def test_envelope_full_discovery_partial_and_missing_limits(self):
        self.assertEqual(self.project({'result':self.discovery}),self.before)
        for change in ('partial','limits'):
            with self.subTest(change=change):
                value=deepcopy(self.discovery)
                if change=='partial':value['partial']=True
                else:del value['limits']
                with self.assertRaises(ValueError):self.project(value)

    def test_optional_context_schema_addition_passes_without_losing_siblings(self):
        node=self.lighting(self.discovery);node['properties']['shadow_filter']={'enum':['point','fractional'],'default':'point'}
        result=self.project();self.assertEqual(result['context']['spawn_templates']['value'],self.discovery['spawn_templates'])
        self.assertEqual(self.findings(),[])

    def test_context_schema_default_and_constraint_tightening_fail(self):
        for change in ('bound','default'):
            with self.subTest(change=change):
                self.discovery=deepcopy(UNIT_DISCOVERY);node=self.lighting(self.discovery)
                if change=='bound':node['properties']['ambient']['maximum']=0.1
                else:node['properties']['exposure']['default']=1
                self.assertTrue(self.findings())

    def test_missing_scalar_unknown_overlapping_or_unsorted_schema_policy_fails(self):
        for change in ('missing','scalar','unknown','overlap','unsorted','duplicate'):
            with self.subTest(change=change):
                value=deepcopy(UNIT_DISCOVERY);selected=deepcopy(UNIT_SELECTION)
                if change=='missing':del value['spawn_templates']['hierarchy']
                elif change=='scalar':value['spawn_templates']['hierarchy']='not a schema'
                elif change=='unknown':selected['context_schema_paths']=[['spawn_templates','runtime']]
                elif change=='overlap':selected['context_schema_paths']=[['spawn_templates'],['spawn_templates','hierarchy']]
                elif change=='unsorted':selected['context_schema_paths'].reverse()
                else:selected['context_schema_paths'].append(selected['context_schema_paths'][-1])
                with self.assertRaises(ValueError):self.project(value,selected)

    def test_schema_policy_cannot_hide_other_schema_sibling_change(self):
        selected=deepcopy(self.selection);selected['context_schema_paths']=[['spawn_templates','components']]
        old=self.project(selection=selected);self.lighting(self.discovery)['properties']['extra']={'type':'boolean'}
        self.assertTrue(TOOL.compare(old,self.project(selection=selected),CHECKER))

    def test_selected_reference_and_context_source_are_retained_unproven(self):
        for location in ('method','context'):
            with self.subTest(location=location):
                value=deepcopy(UNIT_DISCOVERY);value['$defs']={'selected':{'type':'object'}}
                if location=='method':value['methods']['runtime.start']={'$ref':'#/$defs/selected'}
                else:value['spawn_templates']['hierarchy']={'$ref':'#/$defs/selected'}
                result=self.project(value);self.assertEqual(result['reference_context'],value)
                self.assertTrue(TOOL.compare(result,deepcopy(result),CHECKER))
                result['reference_context']=None
                with self.assertRaisesRegex(ValueError,'lost its source'):TOOL.compare(result,result,CHECKER)

    def test_reference_source_substitution_cannot_certify_partial_projection(self):
        self.discovery['methods']['runtime.start']={'$ref':'#/$defs/start'};self.discovery['$defs']={'start':True}
        result=self.project();result['reference_context']['methods']['runtime.start']={'type':'boolean'}
        with self.assertRaisesRegex(ValueError,'does not reproduce'):TOOL.compare(result,result,CHECKER)

    def test_missing_reference_context_rejects_and_cli_returns_invalid_input(self):
        result=deepcopy(self.before);del result['reference_context']
        with self.assertRaisesRegex(ValueError,'retain reference_context'):
            TOOL.compare(result,self.before,CHECKER)
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);manifest=directory/'selection.json';source=directory/'discovery.json';baseline=directory/'baseline.json';output=directory/'result.json'
            manifest.write_text(json.dumps(self.selection),encoding='utf-8')
            source.write_text(json.dumps(self.discovery),encoding='utf-8')
            baseline.write_text(json.dumps(result),encoding='utf-8')
            from contextlib import redirect_stderr
            from io import StringIO
            diagnostic=StringIO()
            with redirect_stderr(diagnostic),self.assertRaises(SystemExit) as exit_result:
                TOOL.main(['--checker',str(ARGS.checker),'--manifest',str(manifest),
                           '--profile','source_world','--discovery',str(source),
                           '--baseline',str(baseline),'--output',str(output)])
            self.assertEqual(exit_result.exception.code,2)
            self.assertIn('retain reference_context',diagnostic.getvalue())
            self.assertFalse(output.exists())

    def test_malformed_profile_identity_rejects_and_cli_returns_invalid_input(self):
        for profile in ([],{},None,0,True,''):
            with self.subTest(profile=profile):
                value=deepcopy(self.before);value['profile_id']=profile
                with self.assertRaisesRegex(ValueError,'Unknown projection profile'):
                    TOOL.compare(value,self.before,CHECKER)
                with self.assertRaisesRegex(ValueError,'Unknown profile'):
                    self.project(scope=profile)
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);manifest=directory/'selection.json';source=directory/'discovery.json';baseline=directory/'baseline.json';output=directory/'result.json'
            value=deepcopy(self.before);value['profile_id']=[]
            manifest.write_text(json.dumps(self.selection),encoding='utf-8')
            source.write_text(json.dumps(self.discovery),encoding='utf-8')
            baseline.write_text(json.dumps(value),encoding='utf-8')
            from contextlib import redirect_stderr
            from io import StringIO
            diagnostic=StringIO()
            with redirect_stderr(diagnostic),self.assertRaises(SystemExit) as exit_result:
                TOOL.main(['--checker',str(ARGS.checker),'--manifest',str(manifest),
                           '--profile','source_world','--discovery',str(source),
                           '--baseline',str(baseline),'--output',str(output)])
            self.assertEqual(exit_result.exception.code,2)
            self.assertIn('Unknown projection profile',diagnostic.getvalue())
            self.assertFalse(output.exists())

    def test_dialect_resource_and_dynamic_markers_stay_unproven(self):
        for marker,value in [('$schema','https://json-schema.org/draft/2020-12/schema'),
                             ('$id','https://invalid.example/resource'),('$dynamicRef','#node'),
                             ('$recursiveRef','#'),('$anchor','node'),('$dynamicAnchor','node'),
                             ('$recursiveAnchor',True),('$vocabulary',{'https://invalid.example/vocab':True})]:
            with self.subTest(marker=marker):
                discovery=deepcopy(UNIT_DISCOVERY);discovery['methods']['runtime.start'][marker]=value
                result=self.project(discovery);self.assertEqual(result['reference_context'],discovery)
                self.assertTrue(TOOL.compare(result,result,CHECKER))
        discovery=deepcopy(UNIT_DISCOVERY);discovery['$schema']='https://json-schema.org/draft/2020-12/schema'
        result=self.project(discovery);self.assertEqual(result['reference_context'],discovery)
        self.assertTrue(TOOL.compare(result,result,CHECKER))

    def test_strict_loader_rejects_duplicate_and_nonfinite_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.json'
            for source in ['{"methods":{},"methods":{}}','{"x":NaN}','{"x":1e999}']:
                path.write_text(source,encoding='utf-8')
                with self.assertRaises(ValueError):CHECKER._load(path)

    def test_candidate_projection_and_selection_cannot_be_relabeled_stable(self):
        result=deepcopy(self.before);result['status']='stable'
        with self.assertRaises(ValueError):TOOL.compare(result,result,CHECKER)
        selected=deepcopy(self.selection);selected['status']='stable'
        with self.assertRaises(ValueError):self.project(selection=selected)

    def test_cli_projection_then_negative_comparison_returns_findings(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);manifest=directory/'selection.json';source=directory/'discovery.json';before=directory/'before.json';after=directory/'after.json'
            manifest.write_text(json.dumps(self.selection),encoding='utf-8');source.write_text(json.dumps(self.discovery),encoding='utf-8')
            argv=['--checker',str(ARGS.checker),'--manifest',str(manifest),'--profile','source_world','--discovery',str(source),'--output',str(before)]
            self.assertEqual(TOOL.main(argv),0)
            self.discovery['methods']['runtime.start']['properties']['revision']['maximum']=1
            source.write_text(json.dumps(self.discovery),encoding='utf-8')
            argv[-1]=str(after);argv+=['--baseline',str(before)]
            self.assertEqual(TOOL.main(argv),1)
            result=json.loads(after.read_text());self.assertEqual(result['status'],'candidate');self.assertTrue(result['findings'])


    def test_cli_identical_candidate_comparison_is_still_prospective(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);manifest=directory/'selection.json';source=directory/'discovery.json';before=directory/'before.json';report=directory/'report.json'
            manifest.write_text(json.dumps(self.selection),encoding='utf-8');source.write_text(json.dumps(self.discovery),encoding='utf-8')
            args=['--checker',str(ARGS.checker),'--manifest',str(manifest),'--profile','source_world','--discovery',str(source),'--output',str(before)]
            self.assertEqual(TOOL.main(args),0);args[-1]=str(report);args+=['--baseline',str(before)]
            self.assertEqual(TOOL.main(args),0)
            result=json.loads(report.read_text());self.assertEqual(result['status'],'candidate');self.assertEqual(result['findings'],[])
            self.assertIn('No results',result['scope'])

    def test_cli_invalid_scope_does_not_publish_output(self):
        import contextlib,io
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);manifest=directory/'selection.json';source=directory/'discovery.json';output=directory/'rejected.json'
            manifest.write_text(json.dumps(self.selection),encoding='utf-8');source.write_text(json.dumps(self.discovery),encoding='utf-8')
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as rejected:
                TOOL.main(['--checker',str(ARGS.checker),'--manifest',str(manifest),'--profile','unknown','--discovery',str(source),'--output',str(output)])
            self.assertEqual(rejected.exception.code,2);self.assertFalse(output.exists())


class NativeCaptureIntegration(unittest.TestCase):
    def test_opt_in_current_and_previous_discovery_in_three_scopes(self):
        if ARGS.discovery_dir is None:self.skipTest('Native discovery integration is opt-in')
        for scope,filename in FILES.items():
            with self.subTest(scope=scope):
                source=ARGS.discovery_dir/filename;discovery=CHECKER._load(source)
                projected=TOOL.project(discovery,PRODUCTION_SELECTION,scope,CHECKER)
                self.assertEqual(set(projected['methods']),set(PRODUCTION_SELECTION['methods']))
                for name,node in projected['methods'].items():self.assertTrue(CHECKER._equal(node,discovery['methods'][name]))
                self.assertEqual(TOOL.compare(projected,deepcopy(projected),CHECKER),[])
                row={'profile':scope,'selected_methods':len(projected['methods']),'current_discovery_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'metadata_only':True}
                if ARGS.previous_discovery_dir is not None:
                    previous=ARGS.previous_discovery_dir/filename
                    baseline=TOOL.project(CHECKER._load(previous),PRODUCTION_SELECTION,scope,CHECKER)
                    findings=TOOL.compare(baseline,projected,CHECKER)
                    row['previous_discovery_sha256']=hashlib.sha256(previous.read_bytes()).hexdigest();row['findings']=findings
                    self.assertEqual(findings,[])
                INTEGRATION.append(row)


def main(argv=None):
    global TOOL,CHECKER,UNIT_SELECTION,UNIT_DISCOVERY,PRODUCTION_SELECTION,ARGS
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker',type=Path,default=ROOT/'scripts/check_authoring_api.py')
    parser.add_argument('--discovery-dir',type=Path)
    parser.add_argument('--previous-discovery-dir',type=Path)
    parser.add_argument('--evidence',type=Path)
    ARGS,remaining=parser.parse_known_args(argv)
    if ARGS.previous_discovery_dir is not None and ARGS.discovery_dir is None:parser.error('--previous-discovery-dir requires --discovery-dir')
    TOOL=module(ROOT/'scripts/project_workflow_api.py','workflow_api_projection_tool');CHECKER=TOOL.load_checker(ARGS.checker)
    UNIT_SELECTION=CHECKER._load(FIXTURES/'consumer-selection.json');UNIT_DISCOVERY=CHECKER._load(FIXTURES/'consumer-discovery.json')
    PRODUCTION_SELECTION=CHECKER._load(ROOT/'docs/contracts/runtime-save-workflow-v1.candidate.json')
    suite=unittest.main(argv=[sys.argv[0],*remaining],exit=False)
    passed=suite.result.wasSuccessful()
    if ARGS.evidence:
        ARGS.evidence.write_text(json.dumps({'format':'poima.workflow-projection-tests.v1','passed':passed,'tests_run':suite.result.testsRun,'skipped':len(suite.result.skipped),'native_execution':False,'status':'prospective','integration':INTEGRATION},indent=2)+'\n',encoding='utf-8')
    return 0 if passed else 1


if __name__=='__main__':raise SystemExit(main())
