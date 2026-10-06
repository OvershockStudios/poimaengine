#!/usr/bin/env python3
"""Headless Windows desktop owner handling of real compiled UI Control intents."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback
import uuid
from desktop_gameplay_save_requests import Desktop, check
from gameplay_save_requests import sha


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bridge','binary','hostfxr','gameplay-bridge','assembly','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--headless',action='store_true')
    args=parser.parse_args()
    if os.name!='nt' or not args.headless:parser.error('Use native Windows Python with --headless; no window is created.')
    run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    record=dict(success=False,checks=[],calls=[],polls=[],hashes={name:sha(getattr(args,name)) for name in
        ('bridge','binary','hostfxr','gameplay_bridge','assembly')},test_sha256=sha(__file__),
        limitations=['Real CoreCLR and desktop C ABI; no HWND, GPU, physical input or capture expiry qualification.'])
    desktop=None
    try:
        project=run/'Project'
        created=subprocess.run([str(args.binary.resolve()),'project','create',str(project),'--name','UI desktop controls'],
            capture_output=True,text=True,encoding='utf-8',timeout=30)
        check(created.returncode==0,created.stdout+created.stderr)
        manifest=json.loads((project/'project.json').read_text());world=project/manifest['entry']['world']
        desktop=Desktop(args,world,record)
        uid=lambda n:f'{n:032x}'
        definitions=[(1,'panel','',''),(2,'label','Original',''),(3,'button','Edit','edit'),
                     (4,'button','Save','save'),(5,'button','Resume','resume'),(6,'button','Pause','pause'),(13,'button','Load','load')]
        ops=[dict(op='ui.element.set',id=uid(n),element=dict(parent=None if n==1 else uid(1),name=f'UI {n}',
             kind=kind,text=text,action=action or None,visible=True,enabled=True)) for n,kind,text,action in definitions]
        revision=desktop.rpc('world.inspect')['revision']
        desktop.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops))
        revision=desktop.rpc('world.inspect')['revision'];authored=sha(world)
        storage=run/'Saves';storage.mkdir()
        desktop.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(storage)))
        profile=dict(hostfxr=str(args.hostfxr.resolve()),bridge=str(args.gameplay_bridge.resolve()),
                     assembly=str(args.assembly.resolve()),type='Poima.Tests.ManagedUiGame')
        desktop.rpc('desktop.gameplay.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,profile=profile))
        sid=uuid.uuid4().hex
        desktop.rpc('desktop.play.start',dict(session_id=sid,revision=revision,expected_gameplay_generation=1,paused=True))
        def runtime():return desktop.rpc('runtime.inspect',dict(session_id=sid))
        def playback():return desktop.rpc('desktop.play.inspect')
        def params(number):
            state=runtime();game=desktop.gameplay(sid)
            return dict(session_id=sid,request_id=uuid.uuid4().hex,expected_tick=state['tick'],
                expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],
                expected_gameplay_revision=game['revision'],id=uid(number))
        def activate(number):
            request=params(number);return request,desktop.rpc('runtime.ui.activate',request)
        request=params(5)
        notification=dict(jsonrpc='2.0',method='runtime.ui.activate',params=request)
        pointer=desktop.lib.poima_desktop_call(desktop.host,json.dumps(notification).encode())
        check(pointer,desktop.text(desktop.lib.poima_desktop_error(desktop.host)))
        check(desktop.text(pointer)=='','Notification leaked its synthetic response.')
        record['calls'].append(dict(transport='C ABI notification',request=notification,response=''))
        check(playback()['state']=='playing' and runtime()['tick']==0,'Notification resume did not apply without ticking.')
        check(desktop.gameplay(sid)['module']['values']['ControlCalls']==1,'Notification handler count differs.')
        desktop.rpc('desktop.play.pause',dict(session_id=sid))
        retry=desktop.rpc('runtime.ui.activate',request)
        check(retry['replayed'] and playback()['state']=='paused','Retry revived a manually paused owner.')
        check(desktop.gameplay(sid)['module']['values']['ControlCalls']==1,'Retry reexecuted handler.')
        record['checks'].append('Notification resumes once without advancing Tick; lost-response retry after manual pause does not resume or reexecute.')
        resume_request,resume=activate(5)
        check(resume['intent']==1 and playback()['state']=='playing','Fresh resume failed.')
        _,pause=activate(6)
        check(pause['intent']==2 and playback()['state']=='paused' and runtime()['tick']==0,'Pause intent failed or advanced Tick.')
        desktop.rpc('runtime.ui.activate',resume_request)
        check(playback()['state']=='paused','Older resume receipt overrode later pause.')
        record['checks'].append('Fresh resume and pause apply at fixed Tick; an older resume receipt cannot override a later intent.')
        _,saved=activate(4)
        check(saved['save_serviced'] and not saved['runtime_replaced'],'Save control was not serviced.')
        old=sid
        activate(5)
        load_request,loaded=activate(13)
        check(loaded['runtime_replaced'] and loaded['save_serviced'],'Load did not replace runtime.')
        sid=loaded['current_session_id']
        check(sid!=old and playback()['state']=='paused' and playback()['session_id']==sid,'Load did not leave new owner paused.')
        restored=runtime();globals_before=desktop.gameplay(sid)
        desktop.rpc('runtime.ui.activate',resume_request)
        desktop.rpc('runtime.ui.activate',load_request)
        check(playback()['state']=='paused' and runtime()==restored and desktop.gameplay(sid)==globals_before,
              'Old source receipt changed restored owner/state.')
        check(sha(world)==authored,'Control mutated authored world.')
        record['checks'].append('Load replaces the session and pauses; old resume and Load receipts neither revive nor mutate restored state.')
        record['success']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        if desktop:desktop.close()
        record['rpc_count']=len(record['calls']);evidence=run/'evidence.json'
        evidence.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(dict(success=record['success'],checks=len(record['checks']),calls=record['rpc_count'],evidence=str(evidence))))
    if not record['success']:print(record.get('error','Failed'),file=sys.stderr)
    return 0 if record['success'] else 1


if __name__=='__main__':raise SystemExit(main())
