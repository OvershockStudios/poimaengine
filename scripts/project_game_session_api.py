#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Project complete session19 request schemas; release verification is explicit.

This is request-schema compatibility tooling, not native behavioral qualification.
The historical candidate21 and authoring-core projectors are not modified.
"""
from __future__ import annotations
import argparse
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path

ID='poima.runtime-game-session'
BASELINE_FORMAT='poima.runtime-game-session-requests.v1'
FORMAT='poima.runtime-game-session-request-projection.v1'
METHODS=tuple(sorted(('runtime.component.get','runtime.component.query','runtime.components','runtime.entity',
    'runtime.gameplay.inspect','runtime.hit_test','runtime.inspect','runtime.observe','runtime.raycast',
    'runtime.save.result','runtime.save.status','runtime.status','runtime.step','runtime.ui.activate',
    'runtime.ui.inspect','save.inspect','save.load','save.status','save.write')))
PROFILES={'source_world':dict(mode='authoring',read_only=False,session_scope=None),
    'source_shared_player':dict(mode='authoring',read_only=False,session_scope='shared_headless'),
    'readonly_game':dict(mode='read_only_runtime',read_only=True,session_scope='shared_headless')}
EXPERIMENTAL=[dict(method='runtime.step',parameter=name,when='nonempty-array')
    for name in ('camera_lenses','character_stances','ik','root_motion')]+[
    dict(method='save.load',parameter='upgrade',when='present')]
# Deliberate protocol semantics/capacities; unrelated backend prose, rendering
# limits and authoring/import/deployment policy remain diagnostic metadata.
CONTEXT_RULES=sorted([
    *[(path,'exact')for path in [('protocol_version',),('runtime_available',),
       ('custom_components','type_prefix'),('input_actions','name_pattern'),
       ('gameplay_saves','kinds'),('gameplay_saves','states'),('gameplay_saves','request_rejections')]],
    *[(path,'capacity-at-least')for path in [('limits','request_bytes'),('limits','json_depth'),
       ('custom_components','max_fields'),('custom_components','max_instances'),
       ('custom_components','max_payload_bytes'),('custom_components','max_types'),
       ('input_actions','maximum_actions_per_controller'),('input_actions','maximum_controllers'),
       ('ui','max_elements'),('ui','max_depth'),('ui','max_text_bytes'),('ui','max_total_text_bytes')]]])
DEPENDENCIES={'$ref','$dynamicRef','$recursiveRef','$schema','$id','$anchor','$dynamicAnchor',
              '$recursiveAnchor','$vocabulary','$defs','definitions'}
SCOPE='Complete selected request schemas and explicit normative context only; no behavioral, response, ABI, platform or release-qualification inference.'

def load_checker(path=None):
    path=Path(__file__).with_name('check_authoring_api.py')if path is None else Path(path)
    spec=importlib.util.spec_from_file_location('game_session_conservative_request_checker',path)
    if spec is None or spec.loader is None:raise ValueError('Cannot load conservative request checker.')
    checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker);return checker

def _need(ok,message):
    if not ok:raise ValueError(message)

def reject_output_alias(output,inputs):
    """Protect immutable inputs, including symlinks, hard links and path aliases."""
    output=Path(output)
    resolved=output.resolve()
    for source in inputs:
        if source is None:continue
        source=Path(source)
        _need(resolved!=source.resolve(),'Output/report must not overwrite an input file.')
        if output.exists()and source.exists():
            _need(not output.samefile(source),'Output/report aliases an input file.')

def _at(value,path):
    for name in path:
        _need(isinstance(value,dict)and name in value,'Missing normative context: '+'.'.join(path))
        value=value[name]
    return value

def _dependent(value):
    pending=[value]
    while pending:
        node=pending.pop()
        if isinstance(node,dict):
            if DEPENDENCIES.intersection(node):return True
            pending.extend(node.values())
        elif isinstance(node,list):pending.extend(node)
    return False

def _source(value):
    _need(isinstance(value,dict),'Discovery must be an object.')
    value=value.get('result',value)
    _need(isinstance(value,dict)and value.get('partial')is not True,'Full discovery is required, not a focused view.')
    return value

def _context(discovery,profile,checker):
    _need(profile in PROFILES,'Unknown session profile: '+str(profile))
    expected=PROFILES[profile]
    for name in ('mode','read_only'):
        _need(name in discovery and checker._equal(discovery[name],expected[name]),'Discovery profile context differs: '+name)
    present=expected['session_scope']is not None
    _need(('session_scope'in discovery)==present,'Discovery session_scope presence differs.')
    if present:_need(checker._equal(discovery['session_scope'],expected['session_scope']),'Discovery session_scope value differs.')
    return dict(mode=deepcopy(discovery['mode']),read_only=discovery['read_only'],
        session_scope=dict(present=present,**({'value':deepcopy(discovery['session_scope'])}if present else{})))

def validate_baseline(value,checker):
    _need(isinstance(value,dict)and value.get('format')==BASELINE_FORMAT and value.get('id')==ID
          and type(value.get('version'))is int and value['version']==1,'Unsupported session request baseline identity.')
    methods=value.get('methods')
    _need(isinstance(methods,dict)and set(methods)==set(METHODS)
          and all(isinstance(node,(dict,bool))for node in methods.values()),'Exactly19 whole request schemas are required.')
    _need(checker._equal(value.get('experimental_branches'),EXPERIMENTAL),'Experimental discriminators changed or missing.')
    rules=value.get('normative_context')
    _need(isinstance(rules,list)and len(rules)==len(CONTEXT_RULES),'Explicit normative context is required.')
    for row,(path,comparison)in zip(rules,CONTEXT_RULES):
        _need(isinstance(row,dict)and set(row)=={'path','comparison','value'}and row['path']==list(path)
              and row['comparison']==comparison,'Normative context policy differs.')
        if comparison=='capacity-at-least':_need(type(row['value'])is int and row['value']>0,'Capacity must be a positive integer.')
    profiles=value.get('profiles')
    _need(isinstance(profiles,dict)and set(profiles)==set(PROFILES),'Exactly three admitted context profiles are required.')
    for name,entry in profiles.items():
        expected={key:value for key,value in PROFILES[name].items()if key!='session_scope'or value is not None}
        _need(isinstance(entry,dict)and checker._equal(entry.get('context'),_context(expected,name,checker)),
              'Stored profile context differs: '+name)
        _need('reference_context'in entry,'Explicit reference_context, including null, is required.')
        if entry['reference_context']is not None:
            reproduced=project(entry['reference_context'],value,name,checker,validate=False)
            _need(checker._equal(reproduced['methods'],methods),'Baseline reference source does not reproduce selected schemas.')
    if _dependent(methods):
        _need(all(p['reference_context']is not None for p in profiles.values()),'Reference-bearing baseline lost full sources.')

def project(discovery,baseline,profile,checker=None,*,validate=True):
    checker=load_checker()if checker is None else checker
    if validate:validate_baseline(baseline,checker)
    source=_source(discovery);owner=_context(source,profile,checker)
    schemas=source.get('methods')
    _need(isinstance(schemas,dict),'Discovery methods map is required.')
    _need(set(METHODS)<=schemas.keys(),'Missing selected session method(s).')
    selected={name:deepcopy(schemas[name])for name in METHODS}
    _need(all(isinstance(v,(dict,bool))for v in selected.values()),'Selected methods must contain complete schema nodes.')
    normative=[dict(path=list(path),comparison=comparison,value=deepcopy(_at(source,path)))for path,comparison in CONTEXT_RULES]
    # Scan the COMPLETE source, including root dialect/resource/sibling scopes.
    # Unresolved references never become a certifiable pruned projection.
    return dict(format=FORMAT,id=ID,version=1,scope=SCOPE,profile_id=profile,methods=selected,
        owner_context=owner,normative_context=normative,experimental_branches=deepcopy(EXPERIMENTAL),
        reference_context=deepcopy(source)if _dependent(source)else None,
        diagnostics={key:deepcopy(v)for key,v in source.items()if key not in ('methods','components')})

def _projection(value,baseline,checker):
    _need(isinstance(value,dict)and value.get('format')==FORMAT and value.get('id')==ID
          and type(value.get('version'))is int and value['version']==1 and value.get('scope')==SCOPE,'Unsupported session projection.')
    profile=value.get('profile_id');_need(profile in PROFILES,'Unknown projection profile.')
    _need(checker._equal(value.get('owner_context'),baseline['profiles'][profile]['context']),'Projection owner context differs.')
    _need(checker._equal(value.get('experimental_branches'),EXPERIMENTAL),'Projection extension policy changed.')
    _need(isinstance(value.get('methods'),dict)and set(value['methods'])==set(METHODS)
          and all(isinstance(node,(dict,bool))for node in value['methods'].values()),'Projection method membership/schema changed.')
    _need(isinstance(value.get('normative_context'),list)and len(value['normative_context'])==len(CONTEXT_RULES),'Projection normative context membership changed.')
    for row,(path,comparison)in zip(value['normative_context'],CONTEXT_RULES):
        _need(isinstance(row,dict)and set(row)=={'path','comparison','value'}and row['path']==list(path)
              and row['comparison']==comparison,'Projection context policy changed.')
    _need('reference_context'in value,'Projection lost reference_context marker.')
    reference=value['reference_context']
    if reference is not None:
        reproduced=project(reference,baseline,profile,checker)
        for key in ('methods','owner_context','normative_context','reference_context'):
            _need(checker._equal(value[key],reproduced[key]),'Reference source does not reproduce projection: '+key)
    elif _dependent(value['methods'])or _dependent(value.get('diagnostics',{})):
        raise ValueError('Reference-bearing selected schema/context lost its complete source.')

def compare(baseline,candidate,checker=None):
    checker=load_checker()if checker is None else checker
    validate_baseline(baseline,checker);_projection(candidate,baseline,checker)
    findings=[]
    def add(path,reason):
        if len(findings)<checker.MAX_FINDINGS:findings.append(dict(path=path,reason=reason))
    if candidate['reference_context']is not None or baseline['profiles'][candidate['profile_id']]['reference_context']is not None:
        add('$.reference_context','Reference/resource compatibility is unproven; complete discovery source retained for review.')
    for old,new in zip(baseline['normative_context'],candidate['normative_context']):
        left,right=old['value'],new['value'];path='$.context.'+'.'.join(old['path'])
        if old['comparison']=='capacity-at-least':
            if type(right)is not int or right<left:add(path,'Advertised protocol capacity narrowed or changed type.')
        elif not checker._equal(left,right):add(path,'Normative protocol context changed.')
    for method in METHODS:
        for finding in checker.schema_compare(baseline['methods'][method],candidate['methods'][method],'$.methods['+json.dumps(method)+']'):
            add(finding['path'],finding['reason'])
    return findings

def verify_release(release,baseline_path,discovery,profile,checker=None):
    checker=load_checker()if checker is None else checker
    _need(isinstance(release,dict)and release.get('format')=='poima.runtime-game-session-release.v1'
          and release.get('id')==ID and type(release.get('version'))is int and release['version']==1
          and release.get('status')=='stable'and release.get('scope')=='runtime-game-session','Stable release identity is required.')
    _need(checker._equal(release.get('methods'),list(METHODS)),'Release method membership differs.')
    profiles=release.get('profiles');_need(isinstance(profiles,list)and len(profiles)==3 and set(profiles)==set(PROFILES),'Release profiles differ.')
    expected=release.get('request_baseline',{})
    _need(baseline_path.stat().st_size<=checker.MAX_INPUT_BYTES,'Release request baseline exceeds input bound.')
    _need(expected.get('file')=='runtime-game-session-v1.json'and baseline_path.name==expected['file']
          and expected.get('sha256')==hashlib.sha256(baseline_path.read_bytes()).hexdigest(),'Release request baseline SHA256/file differs.')
    baseline=checker._load(baseline_path);validate_baseline(baseline,checker)
    _need(type(expected.get('source_schema_revision'))is int and expected['source_schema_revision']==baseline['provenance']['schema_revision'],'Release source schema revision differs.')
    source=_source(discovery)
    _context(source,profile,checker)
    header=source.get('runtime_game_session_contract')
    _need(isinstance(header,dict),'Discovery lacks the actual stable session identity header.')
    for key,expected_value in dict(id=ID,version=1,status='stable',scope='runtime-game-session',
        request_baseline='runtime-game-session-v1.json',response_baseline='runtime-game-session-responses-v1.json',methods=list(METHODS)).items():
        _need(key in header and checker._equal(header[key],expected_value),'Native release identity differs: '+key)
    available=header.get('availability',{})
    _need(available.get('implemented')is True and available.get('qualified')is True
          and available.get('profile')==profile and available.get('mode')==PROFILES[profile]['mode'],'Native profile is unimplemented/unqualified or mislabeled.')
    backends=available.get('backends')
    names=('simulation','native_gameplay','coreclr_gameplay')
    _need(isinstance(backends,dict)and all(type(backends.get(name))is bool for name in names),
          'Native backend availability requires three literal Boolean flags.')
    _need(backends['simulation']and source.get('runtime_available')is True
          and (backends['native_gameplay']or backends['coreclr_gameplay'])
          and (profile!='readonly_game'or backends['native_gameplay']),
          'Native qualification contradicts the required runtime/gameplay backends.')
    targets=release.get('qualified_targets')
    def target(value):
        return (isinstance(value,dict)and set(value)=={'os','arch'}and
                all(isinstance(value[key],str)and value[key]for key in ('os','arch')))
    _need(isinstance(targets,list)and targets and all(target(value)for value in targets)
          and target(available.get('target'))and available['target']in targets,'Native target is not release-qualified.')
    return dict(id=ID,version=1,status='stable',request_baseline_sha256=expected['sha256'],
                profile=profile,target=deepcopy(available['target']))

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,default=Path(__file__).resolve().parents[1]/'docs/contracts/runtime-game-session-v1.json')
    parser.add_argument('--profile',required=True,choices=sorted(PROFILES));parser.add_argument('--discovery',type=Path,required=True)
    parser.add_argument('--release',type=Path,help='Verify exact release/baseline hash and actual qualified native identity; omission is schema-only inspection.')
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args(argv)
    try:
        reject_output_alias(args.output,(args.baseline,args.discovery,args.release))
        checker=load_checker();baseline=checker._load(args.baseline);discovery=checker._load(args.discovery)
        verified=None if args.release is None else verify_release(checker._load(args.release),args.baseline,discovery,args.profile,checker)
        result=project(discovery,baseline,args.profile,checker);result['release_verification']=verified
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(result,indent=2,ensure_ascii=True,allow_nan=False)+'\n',encoding='utf-8')
    except (ValueError,OSError,TypeError,KeyError,RecursionError)as error:parser.error(str(error))
    return 0

if __name__=='__main__':raise SystemExit(main())
