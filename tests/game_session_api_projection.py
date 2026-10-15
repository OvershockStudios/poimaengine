#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure adversarial session19 request tests; retained native capture reads are opt-in.

Reconstructed unit discoveries below are NOT new native execution evidence.
"""
from __future__ import annotations
import argparse
from copy import deepcopy
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BASELINE_PATH=ROOT/'docs/contracts/runtime-game-session-v1.json'
TOOL=CHECK=SCHEMA=BASELINE=ARGS=None

def module(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise ValueError('Cannot load test tool.')
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def fixture(profile='source_world'):
    result=dict(methods=deepcopy(BASELINE['methods']),schema_revision=98,
                fixture='Reconstructed portable fixture, not native discovery evidence.')
    result.update({key:value for key,value in TOOL.PROFILES[profile].items()if key!='session_scope'or value is not None})
    for row in BASELINE['normative_context']:
        cursor=result
        for part in row['path'][:-1]:cursor=cursor.setdefault(part,{})
        cursor[row['path'][-1]]=deepcopy(row['value'])
    return result

def header(profile='source_world'):
    return dict(id=TOOL.ID,version=1,status='stable',scope='runtime-game-session',methods=list(TOOL.METHODS),
        request_baseline='runtime-game-session-v1.json',response_baseline='runtime-game-session-responses-v1.json',
        availability=dict(implemented=True,qualified=True,mode=TOOL.PROFILES[profile]['mode'],profile=profile,
                          target=dict(os='Windows',arch='x86_64'),
                          backends=dict(simulation=True,native_gameplay=True,coreclr_gameplay=False)))

def release(path):
    return dict(format='poima.runtime-game-session-release.v1',id=TOOL.ID,version=1,status='stable',
        introduced_in='0.0.149',scope='runtime-game-session',methods=list(TOOL.METHODS),profiles=sorted(TOOL.PROFILES),
        request_baseline=dict(file=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),source_schema_revision=98),
        response_baseline=dict(file='runtime-game-session-responses-v1.json',sha256='f'*64),
        qualified_targets=[dict(os='Windows',arch='x86_64')])

class SessionProjection(unittest.TestCase):
    def setUp(self):self.source=fixture();self.baseline=deepcopy(BASELINE)
    def project(self,source=None,profile='source_world'):
        return TOOL.project(self.source if source is None else source,self.baseline,profile,SCHEMA)
    def findings(self):return TOOL.compare(self.baseline,self.project(),SCHEMA)

    def test_exact19_names_and_three_scope_memberships(self):
        self.assertEqual(len(TOOL.METHODS),19)
        for forbidden in ('runtime.start','runtime.stop','runtime.gameplay.load_native','save.configure'):
            self.assertNotIn(forbidden,BASELINE['methods'])
        for profile in TOOL.PROFILES:
            source=fixture(profile);value=self.project(source,profile)
            self.assertEqual(value['methods'],source['methods']);self.assertEqual(TOOL.compare(self.baseline,value,SCHEMA),[])
            self.assertEqual(value['owner_context']['session_scope']['present'],profile!='source_world')

    def test_provenance_truthfully_distinguishes_capture_and_behavior(self):
        p=BASELINE['provenance'];self.assertEqual(p['discovery_native_version'],'0.0.142')
        self.assertEqual(p['schema_revision'],98);self.assertIn('0.0.146',p['behavioral_cohorts'])
        self.assertEqual(set(p['captures']),set(TOOL.PROFILES))

    def test_complete_experimental_nodes_are_retained_not_pruned(self):
        projected=self.project()
        for row in TOOL.EXPERIMENTAL:
            node=self.source['methods'][row['method']]['properties'][row['parameter']]
            self.assertEqual(projected['methods'][row['method']]['properties'][row['parameter']],node)
        self.assertEqual(projected['experimental_branches'],TOOL.EXPERIMENTAL)
        self.assertEqual({r['parameter']for r in TOOL.EXPERIMENTAL},{'ik','root_motion','camera_lenses','character_stances','upgrade'})

    def test_no_aliases_or_source_mutation(self):
        original=deepcopy(self.source);p=self.project();p['methods']['runtime.entity']['properties']['tick']['minimum']=900
        p['normative_context'][0]['value']='changed';self.assertEqual(self.source,original)

    def test_full_native_reply_wrapper_accepted_partial_rejected(self):
        self.assertEqual(self.project({'jsonrpc':'2.0','id':7,'result':self.source})['methods'],self.source['methods'])
        self.source['partial']=True
        with self.assertRaisesRegex(ValueError,'Full discovery'):self.project()

    def test_added_required_guard_fails(self):
        node=self.source['methods']['runtime.entity'];node['required'].append('tick');self.assertTrue(self.findings())

    def test_changed_guard_type_and_nullable_alternative_fail(self):
        for change in ('boolean','nullable'):
            with self.subTest(change=change):
                self.source=fixture()
                if change=='boolean':self.source['methods']['runtime.entity']['properties']['tick']['type']='boolean'
                else:self.source['methods']['save.load']['properties']['expected_session_id']['anyOf'].pop()
                self.assertTrue(self.findings())

    def test_default_value_type_and_omission_fail(self):
        for value in (True,0,None):
            with self.subTest(value=value):
                self.source=fixture();node=self.source['methods']['save.load']['properties']['allow_recovery']
                if value is None:del node['default']
                else:node['default']=value
                self.assertTrue(self.findings())

    def test_narrowed_bounds_and_removed_property_fail(self):
        self.source['methods']['runtime.step']['properties']['ticks']['maximum']=599;self.assertTrue(self.findings())
        self.source=fixture();del self.source['methods']['runtime.raycast']['properties']['ignore'];self.assertTrue(self.findings())

    def test_optional_closed_field_and_wider_capacity_are_compatible(self):
        self.source['methods']['runtime.entity']['properties']['new_optional']={'type':'boolean','default':False}
        self.source['methods']['runtime.step']['properties']['ticks']['maximum']=601
        self.source['limits']['request_bytes']+=1;self.assertEqual(self.findings(),[])

    def test_unknown_constraint_and_overlapping_union_fail_closed(self):
        node=self.source['methods']['runtime.entity']['properties']['id'];node['pattern']='^different$'
        self.assertTrue(self.findings());self.source=fixture()
        old={'oneOf':[{'type':'string'}]}
        self.baseline['methods']['runtime.entity']['properties']['id']=deepcopy(old)
        self.source['methods']['runtime.entity']['properties']['id']=deepcopy(old)
        self.source['methods']['runtime.entity']['properties']['id']['oneOf'].append({'type':'string','minLength':1})
        self.assertTrue(self.findings())

    def test_annotation_ignored_but_annotation_named_literal_is_data(self):
        self.source['methods']['runtime.entity']['description']='Changed documentation';self.assertEqual(self.findings(),[])
        node=self.baseline['methods']['runtime.entity']['properties']['id'];node['default']={'description':'original','flag':True}
        self.source['methods']['runtime.entity']['properties']['id']['default']={'description':'changed','flag':1}
        self.assertTrue(self.findings())

    def test_unrelated_limits_backend_prose_and_schema_revision_are_diagnostic(self):
        self.source['limits']['unrelated_render_budget']=17
        self.source['navigation']={'backend':'changed backend prose'};self.source['schema_revision']=999
        self.source['methods']['unselected.new']={'type':'object'}
        result=self.project();self.assertEqual(self.findings(),[])
        self.assertEqual(result['diagnostics']['navigation'],self.source['navigation'])

    def test_normative_capacity_narrowing_or_boolean_fails(self):
        for value in (1,True):
            with self.subTest(value=value):
                self.source=fixture();self.source['custom_components']['max_types']=value;self.assertTrue(self.findings())

    def test_normative_enum_and_missing_context_fail(self):
        self.source['gameplay_saves']['states']['failed']=True;self.assertTrue(self.findings())
        del self.source['input_actions']['name_pattern']
        with self.assertRaisesRegex(ValueError,'Missing normative'):self.project()

    def test_scope_presence_and_bool_integer_are_exact(self):
        for change in ('extra-null','wrong-mode','boolean-as-integer'):
            with self.subTest(change=change):
                self.source=fixture()
                if change=='extra-null':self.source['session_scope']=None
                elif change=='wrong-mode':self.source['mode']='read_only_runtime'
                else:self.source['read_only']=0
                with self.assertRaises(ValueError):self.project()

    def test_baseline_smaller_membership_or_changed_policy_cannot_certify(self):
        for change in ('method','context','extension','profile'):
            with self.subTest(change=change):
                self.baseline=deepcopy(BASELINE)
                if change=='method':del self.baseline['methods']['runtime.hit_test']
                elif change=='context':self.baseline['normative_context'].pop()
                elif change=='extension':self.baseline['experimental_branches'].pop()
                else:del self.baseline['profiles']['readonly_game']
                with self.assertRaises(ValueError):self.project()

    def test_projection_method_context_and_extension_tampering_fail(self):
        for change in ('method','context','extension','owner'):
            with self.subTest(change=change):
                p=self.project()
                if change=='method':del p['methods']['runtime.hit_test']
                elif change=='context':p['normative_context'].pop()
                elif change=='extension':p['experimental_branches'].pop()
                else:p['owner_context']['session_scope']={'present':True,'value':None}
                with self.assertRaises(ValueError):TOOL.compare(self.baseline,p,SCHEMA)

    def test_references_and_root_or_sibling_scope_keep_complete_source(self):
        for change in ('reference','root-dialect','sibling-anchor','defs'):
            with self.subTest(change=change):
                source=fixture()
                if change=='reference':source['methods']['runtime.entity']['$ref']='#/$defs/runtime'
                elif change=='root-dialect':source['$schema']='https://json-schema.org/draft/2020-12/schema'
                elif change=='sibling-anchor':source['methods']['unselected']={'$anchor':'anchor','type':'object'}
                else:source['$defs']={'runtime':{'type':'object'}}
                p=self.project(source);self.assertEqual(p['reference_context'],source)
                self.assertTrue(TOOL.compare(self.baseline,p,SCHEMA))

    def test_reference_context_tamper_and_lost_marker_fail(self):
        self.source['methods']['runtime.entity']['$ref']='#/methods/runtime.inspect';p=self.project()
        p['reference_context']['methods']['runtime.entity']['$ref']='#/other'
        with self.assertRaises(ValueError):TOOL.compare(self.baseline,p,SCHEMA)
        p=self.project();p['reference_context']=None
        with self.assertRaises(ValueError):TOOL.compare(self.baseline,p,SCHEMA)

    def test_root_scope_marker_cannot_drop_source_and_experimental_constraints_remain(self):
        self.source['$id']='https://invalid.example/resource';p=self.project();p['reference_context']=None
        with self.assertRaises(ValueError):TOOL.compare(self.baseline,p,SCHEMA)
        self.source=fixture();self.source['methods']['runtime.step']['properties']['ik']['maxItems']=0
        self.assertTrue(self.findings())

    def test_projection_invalid_schema_value_rejects(self):
        p=self.project();p['methods']['runtime.raycast']=1
        with self.assertRaises(ValueError):TOOL.compare(self.baseline,p,SCHEMA)

    def test_historical_candidate_guard_not_relabeled(self):
        old=module(ROOT/'scripts/project_workflow_api.py','unchanged_candidate_projector')
        selection=SCHEMA._load(ROOT/'docs/contracts/runtime-save-workflow-v1.candidate.json')
        selection['status']='stable'
        with self.assertRaises(ValueError):old._guard(selection)
        self.assertEqual(len(selection['methods']),21)

    def test_strict_loader_duplicate_nonfinite_and_overflow_rejected(self):
        with tempfile.TemporaryDirectory()as directory:
            path=Path(directory)/'bad.json'
            for raw in ('{"x":1,"x":2}','{"x":NaN}','{"x":1e999}'):
                path.write_text(raw)
                with self.assertRaises(ValueError):SCHEMA._load(path)

    def test_release_hash_identity_and_real_native_header_required(self):
        with tempfile.TemporaryDirectory()as directory:
            path=Path(directory)/BASELINE_PATH.name;path.write_bytes(BASELINE_PATH.read_bytes());manifest=release(path)
            with self.assertRaisesRegex(ValueError,'actual stable'):TOOL.verify_release(manifest,path,self.source,'source_world',SCHEMA)
            self.source['runtime_game_session_contract']=header()
            self.assertEqual(TOOL.verify_release(manifest,path,self.source,'source_world',SCHEMA)['status'],'stable')
            manifest['request_baseline']['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'SHA256'):TOOL.verify_release(manifest,path,self.source,'source_world',SCHEMA)

    def test_release_version_bool_scope_target_and_qualification_reject(self):
        with tempfile.TemporaryDirectory()as directory:
            path=Path(directory)/BASELINE_PATH.name;path.write_bytes(BASELINE_PATH.read_bytes())
            for change in ('bool-version','missing-method','unqualified','wrong-target','wrong-profile'):
                with self.subTest(change=change):
                    source=fixture();source['runtime_game_session_contract']=header();manifest=release(path)
                    if change=='bool-version':manifest['version']=True
                    elif change=='missing-method':manifest['methods'].pop()
                    elif change=='unqualified':source['runtime_game_session_contract']['availability']['qualified']=False
                    elif change=='wrong-target':source['runtime_game_session_contract']['availability']['target']['os']='Linux'
                    else:source['runtime_game_session_contract']['availability']['profile']='readonly_game'
                    with self.assertRaises(ValueError):TOOL.verify_release(manifest,path,source,'source_world',SCHEMA)

    def test_cli_schema_only_projection_then_check_and_negative_exit(self):
        with tempfile.TemporaryDirectory()as directory:
            folder=Path(directory);source=folder/'discovery.json';output=folder/'projection.json';report=folder/'report.json'
            source.write_text(json.dumps(self.source))
            self.assertEqual(TOOL.main(['--baseline',str(BASELINE_PATH),'--profile','source_world','--discovery',str(source),'--output',str(output)]),0)
            self.assertIsNone(json.loads(output.read_text())['release_verification'])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(CHECK.main(['--baseline',str(BASELINE_PATH),'--candidate',str(output),'--report',str(report)]),0)
            self.source['methods']['runtime.step']['properties']['ticks']['maximum']=2;source.write_text(json.dumps(self.source))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(CHECK.main(['--candidate',str(source),'--profile','source_world','--report',str(report)]),1)
            self.assertTrue(json.loads(report.read_text())['findings'])

    def test_cli_release_failure_no_output_and_bad_inputs_exit2(self):
        with tempfile.TemporaryDirectory()as directory:
            folder=Path(directory);source=folder/'source.json';manifest=folder/'release.json';output=folder/'never.json'
            source.write_text(json.dumps(self.source));manifest.write_text(json.dumps(release(BASELINE_PATH)))
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit)as rejected:
                TOOL.main(['--profile','source_world','--discovery',str(source),'--release',str(manifest),'--output',str(output)])
            self.assertEqual(rejected.exception.code,2);self.assertFalse(output.exists())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(CHECK.main(['--candidate',str(source)]),2)

    def test_release_backend_flags_are_literal_and_coherent(self):
        with tempfile.TemporaryDirectory()as directory:
            path=Path(directory)/BASELINE_PATH.name;path.write_bytes(BASELINE_PATH.read_bytes());manifest=release(path)
            cases=[None,{},dict(simulation=True,native_gameplay=False,coreclr_gameplay=False),
                   dict(simulation=False,native_gameplay=True,coreclr_gameplay=False)]
            for name in ('simulation','native_gameplay','coreclr_gameplay'):
                for bad in (0,1,None,'true'):
                    flags=dict(simulation=True,native_gameplay=True,coreclr_gameplay=False);flags[name]=bad;cases.append(flags)
            for flags in cases:
                with self.subTest(flags=flags):
                    source=fixture();source['runtime_game_session_contract']=header()
                    source['runtime_game_session_contract']['availability']['backends']=flags
                    with self.assertRaises(ValueError):TOOL.verify_release(manifest,path,source,'source_world',SCHEMA)
            source=fixture();source['runtime_game_session_contract']=header()
            source['runtime_game_session_contract']['availability']['backends']=dict(simulation=True,native_gameplay=False,coreclr_gameplay=True)
            self.assertEqual(TOOL.verify_release(manifest,path,source,'source_world',SCHEMA)['profile'],'source_world')
            source=fixture('readonly_game');source['runtime_game_session_contract']=header('readonly_game')
            source['runtime_game_session_contract']['availability']['backends']=dict(simulation=True,native_gameplay=False,coreclr_gameplay=True)
            with self.assertRaisesRegex(ValueError,'contradicts'):TOOL.verify_release(manifest,path,source,'readonly_game',SCHEMA)
            source['runtime_game_session_contract']['availability']['backends']['native_gameplay']=True
            self.assertEqual(TOOL.verify_release(manifest,path,source,'readonly_game',SCHEMA)['profile'],'readonly_game')

    def test_release_header_cannot_override_actual_owner_or_runtime_availability(self):
        with tempfile.TemporaryDirectory()as directory:
            path=Path(directory)/BASELINE_PATH.name;path.write_bytes(BASELINE_PATH.read_bytes());manifest=release(path)
            source=fixture();source['runtime_game_session_contract']=header();source['runtime_available']=False
            with self.assertRaisesRegex(ValueError,'contradicts'):TOOL.verify_release(manifest,path,source,'source_world',SCHEMA)
            source=fixture('readonly_game');source['runtime_game_session_contract']=header()
            with self.assertRaisesRegex(ValueError,'profile context'):TOOL.verify_release(manifest,path,source,'source_world',SCHEMA)

    def test_projector_and_checker_preserve_all_input_path_aliases(self):
        with tempfile.TemporaryDirectory()as directory:
            folder=Path(directory);base=folder/BASELINE_PATH.name;source=folder/'discovery.json';manifest=folder/'release.json'
            base.write_bytes(BASELINE_PATH.read_bytes());source.write_text(json.dumps(self.source));manifest.write_text(json.dumps(release(base)))
            inputs=(base,source,manifest);original={path:path.read_bytes()for path in inputs}
            nested=folder/'nested';nested.mkdir()
            for path in inputs:
                alias=nested/'..'/path.name
                with self.subTest(path=path),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit)as failed:
                    TOOL.main(['--baseline',str(base),'--profile','source_world','--discovery',str(source),
                               '--release',str(manifest),'--output',str(alias)])
                self.assertEqual(failed.exception.code,2)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(CHECK.main(['--baseline',str(base),'--candidate',str(source),'--profile','source_world',
                                                '--release',str(manifest),'--report',str(alias)]),2)
                self.assertEqual({path:path.read_bytes()for path in inputs},original)
            # Invalid JSON must not let the error report overwrite its own input.
            source.write_text('{invalid');damaged=source.read_bytes()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(CHECK.main(['--baseline',str(base),'--candidate',str(source),'--report',str(source)]),2)
            self.assertEqual(source.read_bytes(),damaged)

    def test_existing_hardlink_output_is_rejected_by_file_identity(self):
        with tempfile.TemporaryDirectory()as directory:
            folder=Path(directory);base=folder/BASELINE_PATH.name;source=folder/'source.json';alias=folder/'hardlink.json'
            base.write_bytes(BASELINE_PATH.read_bytes());source.write_text(json.dumps(self.source));original=source.read_bytes()
            try:os.link(source,alias)
            except OSError as error:self.skipTest('Filesystem hard links unavailable: '+str(error))
            self.assertNotEqual(alias.resolve(),source.resolve())
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit)as failed:
                TOOL.main(['--baseline',str(base),'--profile','source_world','--discovery',str(source),'--output',str(alias)])
            self.assertEqual(failed.exception.code,2)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(CHECK.main(['--baseline',str(base),'--candidate',str(source),'--profile','source_world','--report',str(alias)]),2)
            self.assertEqual(source.read_bytes(),original);self.assertEqual(alias.read_bytes(),original)

    def test_opt_in_original_qualified_discoveries(self):
        if ARGS.discovery_dir is None:self.skipTest('Retained native metadata integration is opt-in; no native execution.')
        files={'source_world':'standalone-authoring.json','source_shared_player':'shared-authoring.json','readonly_game':'shared-readonly-game.json'}
        for profile,name in files.items():
            path=ARGS.discovery_dir/name;source=SCHEMA._load(path)
            expected=BASELINE['provenance']['captures'][profile]
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),expected['sha256'])
            self.assertEqual(TOOL.compare(BASELINE,TOOL.project(source,BASELINE,profile,SCHEMA),SCHEMA),[])

def main(argv=None):
    global TOOL,CHECK,SCHEMA,BASELINE,ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--discovery-dir',type=Path);parser.add_argument('--evidence',type=Path)
    ARGS,remaining=parser.parse_known_args(argv)
    TOOL=module(ROOT/'scripts/project_game_session_api.py','session_projector_test')
    CHECK=module(ROOT/'scripts/check_game_session_api.py','session_checker_test');SCHEMA=TOOL.load_checker();BASELINE=SCHEMA._load(BASELINE_PATH)
    suite=unittest.main(argv=[sys.argv[0],*remaining],exit=False);passed=suite.result.wasSuccessful()
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True,exist_ok=True)
        ARGS.evidence.write_text(json.dumps(dict(format='poima.game-session-request-tests.v1',passed=passed,tests_run=suite.result.testsRun,
            skipped=len(suite.result.skipped),native_execution=False,retained_metadata_only=ARGS.discovery_dir is not None),indent=2)+'\n')
    return 0 if passed else 1

if __name__=='__main__':raise SystemExit(main())
