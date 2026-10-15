#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check session19 request compatibility, optionally verifying stable release identity.

Exit0 means no conservative request findings; it is not behavioral qualification.
Exit1 reports compatibility findings; exit2 reports malformed/unadmitted inputs.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path

def load_projector():
    path=Path(__file__).with_name('project_game_session_api.py')
    spec=importlib.util.spec_from_file_location('session_request_projector',path)
    if spec is None or spec.loader is None:raise ValueError('Cannot load session projector.')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,default=Path(__file__).resolve().parents[1]/'docs/contracts/runtime-game-session-v1.json')
    parser.add_argument('--candidate',type=Path,required=True,help='Full native discovery or stored session request projection.')
    parser.add_argument('--profile',choices=['source_world','source_shared_player','readonly_game'])
    parser.add_argument('--release',type=Path,help='Require exact stable manifest identity/hash and native qualification header; no silent absence PASS.')
    parser.add_argument('--report',type=Path);args=parser.parse_args(argv)
    report_admitted=False
    try:
        tool=load_projector()
        if args.report:tool.reject_output_alias(args.report,(args.baseline,args.candidate,args.release))
        report_admitted=True
        checker=tool.load_checker();baseline=checker._load(args.baseline);candidate=checker._load(args.candidate)
        projected=candidate.get('format')==tool.FORMAT
        profile=candidate.get('profile_id')if args.profile is None and projected else args.profile
        if profile is None:raise ValueError('--profile is required for full discovery.')
        if projected and profile!=candidate.get('profile_id'):raise ValueError('Explicit profile differs from stored projection.')
        # A detached projection cannot prove an actual identity header. Supply
        # the full native discovery to --release instead of trusting a label.
        if args.release is not None and projected:raise ValueError('--release requires the original full discovery, not a detached projection.')
        verified=None if args.release is None else tool.verify_release(checker._load(args.release),args.baseline,candidate,profile,checker)
        projection=candidate if projected else tool.project(candidate,baseline,profile,checker)
        findings=tool.compare(baseline,projection,checker)
        report=dict(format='poima.runtime-game-session-request-check.v1',passed=not findings,
            scope=tool.SCOPE,release_verification=verified,profile_id=profile,findings=findings)
        code=0 if not findings else 1
    except (OSError,ValueError,TypeError,KeyError,RecursionError)as error:
        report=dict(format='poima.runtime-game-session-request-check.v1',passed=False,error=str(error)[:4096],release_verification=None);code=2
    text=json.dumps(report,indent=2,ensure_ascii=True,allow_nan=False)+'\n';print(text,end='')
    if args.report and report_admitted:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(text,encoding='utf-8')
    return code

if __name__=='__main__':raise SystemExit(main())
