#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded interactive player for an already-built CoreCLR projectile module.

Requires an admitted native Windows graphics route; no build/export occurs.
Unattended execution additionally requires an external process-tree owner.
"""
import argparse
import json
from pathlib import Path
import sys
import traceback
import uuid
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','project','assembly','bridge','hostfxr','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--gpu',type=int,required=True);p.add_argument('--frames',type=int,default=600)
    a=p.parse_args()
    if not 1<=a.frames<=3600 or a.gpu<0:p.error('Explicit hardware GPU and1..3600 frames required.')
    if a.output.exists()or not a.output.parent.is_dir():p.error('Fresh output/existing parent required.')
    for name in ('binary','project','assembly','bridge','hostfxr'):setattr(a,name,getattr(a,name).resolve(strict=True))
    spec=json.loads(a.project.read_bytes());world=a.project.parent/spec['entry']['world'];a.output.mkdir()
    r=dict(passed=False,calls=[],errors=[],scope='Interactive CoreCLR sample invocation; no export/AOT/build or qualification claim.');client=None
    try:
        client=WorldClient.open(str(a.binary),str(world),trace_directory=str((a.output/'stdio').absolute()),close_timeout=5)
        def call(method,params=None,timeout=30):
            row=dict(method=method,params=params or {});r['calls'].append(row)
            row['result']=client.call(method,row['params'],timeout=timeout);return row['result']
        session=uuid.uuid4().hex;revision=call('world.inspect')['revision'];call('runtime.start',dict(session_id=session,revision=revision))
        current=call('runtime.gameplay.inspect',dict(session_id=session,tick=0))
        call('runtime.gameplay.load',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=current['revision'],assembly=str(a.assembly),bridge=str(a.bridge),hostfxr=str(a.hostfxr),type='Poima.Examples.ProjectileRangeGame'))
        print('Choose a launch button. WASD/mouse moves the observer; Tab releases the cursor. Fault is an intentional exception test, not normal play.',flush=True)
        call('runtime.play',dict(request_id=uuid.uuid4().hex,session_id=session,expected_tick=0,controller=spec['entry']['controller'],camera=spec['entry']['camera'],mode='interactive',max_frames=a.frames,width=1280,height=720,gpu=a.gpu),timeout=120)
        r['passed']=True
    except BaseException:r['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            try:client.close()
            except BaseException:r['passed']=False;r['errors'].append(traceback.format_exc())
            r['close']=dict(closed=client.closed,pid=client.transport.process_id,exit_code=client.transport.returncode,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error))
            if not client.closed or client.transport.returncode!=0 or client.cleanup_error is not None:r['passed']=False
        (a.output/'calls.json').write_text(json.dumps(r,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=r['passed'],record=str(a.output/'calls.json'))));return 0 if r['passed']else 1


if __name__=='__main__':raise SystemExit(main())
