#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify frame capture with actual native Windows Vulkan player lifetimes.

Uses targeted window resize messages, not physical input. This checks telemetry
integrity on a small fixture; it is not a representative game benchmark.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import sys
import time
import traceback
import uuid
import player_service_contract as shared
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/performance'))
import windows_process_memory as memory

def verify_capture(client,capture_id):
    summary=client.call('performance.summary',dict(capture_id=capture_id));rows=[];offset=0
    while True:
        page=client.call('performance.frames',dict(capture_id=capture_id,offset=offset,limit=17));rows+=page['frames']
        if page['next_offset'] is None:break
        offset=page['next_offset']
    assert rows and len(rows)==summary['count']
    assert summary['frozen'] and summary['cpu_open']==0 and summary['gpu_pending']==0
    assert not summary['drain_failed'] and summary['frames_failed']==0
    assert summary['gpu_submitted']==summary['gpu_completed']+summary['gpu_dropped']
    assert summary['gpu_completed']==summary['gpu_timed']+summary['gpu_unavailable']+summary['gpu_query_failed']
    assert summary['gpu_dropped']==0 and summary['gpu_query_failed']==0
    assert sum(row['gpu_submitted'] for row in rows)==summary['gpu_submitted']
    assert sum(row['gpu_state']=='completed' for row in rows)==summary['gpu_timed']
    assert sum(row['gpu_submitted'] and row['gpu_state']=='unavailable' and row['gpu_drop_reason']=='query_unavailable' for row in rows)==summary['gpu_unavailable']
    assert sum(row['gpu_submitted'] and row['gpu_state']=='unavailable' and row['gpu_drop_reason'] in ('query_failed','invalid_timing') for row in rows)==summary['gpu_query_failed']
    assert sum(row['gpu_state']=='dropped' for row in rows)==summary['gpu_dropped']
    assert sum(not row['gpu_submitted'] and row['gpu_state']=='unavailable' for row in rows)==summary['gpu_not_submitted_unavailable']
    assert sum(row['cpu_failed'] for row in rows)==summary['frames_failed']
    for name,bit in summary['flag_bits'].items():assert summary['flagged_frames'][name]==sum(bool(row['flags']&bit) for row in rows)
    assert summary['gpu_timed']>0 and summary['gpu_completed']==summary['gpu_submitted']
    assert all(row['cpu_complete'] and not row['cpu_failed'] for row in rows)
    assert all(rows[i]['sequence']>rows[i-1]['sequence'] for i in range(1,len(rows)))
    for row in rows:
        assert row['begin_ns']>=0 and row['end_ns']>=row['begin_ns']
        assert row['session'] and len(row['session'])==32
        if row['successful_present']:assert row['begin_ns']<=row['present_return_ns']<=row['end_ns']
        assert all(0<=value<=row['poll_wall_ns'] for value in row['cpu_wall_ns'].values())
        if row['gpu_state']=='completed':
            assert row['gpu_drop_reason']=='none' and row['gpu_ns']['pass_mask']&1
            assert all(0<=value<=row['gpu_ns']['total'] for key,value in row['gpu_ns'].items() if key not in ('total','pass_mask'))
    returns=[row['present_return_ns'] for row in rows if row['successful_present']]
    assert len(returns)>1 and summary['present_return_cadence_all']['samples']==len(returns)-1
    assert summary['present_return_cadence_all']['max']==max(b-a for a,b in zip(returns,returns[1:]))
    assert summary['poll_wall']['samples']==len(rows)
    assert client.call('performance.summary',dict(capture_id=capture_id))==summary
    return dict(summary=summary,rows=rows)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('binary',type=Path);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--gpu',type=int,default=1);args=ap.parse_args()
    if os.name!='nt':ap.error('Run with native Windows Python for actual HWND/PID identity.')
    run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    args.output=run;args.windows_interop=False;args.timeout=30
    record=dict(passed=False,scope='Native Windows telemetry integrity; small fixture, not game performance',calls=[],owners=[],gpu=args.gpu,binary_sha256=shared.sha(args.binary),test_sha256=shared.sha(Path(__file__)),captures=[])
    shared.ARGS=args;shared.RECORD=record;endpoint='frame-'+uuid.uuid4().hex;world=run/'world.json';host=None;client=None;window=None;pool=None;sampling=None
    u=ctypes.WinDLL('user32',use_last_error=True)
    callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    u.EnumWindows.argtypes=[callback,w.LPARAM];u.GetWindowThreadProcessId.argtypes=[w.HWND,ctypes.POINTER(w.DWORD)]
    u.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,ctypes.c_int];u.GetWindowTextW.restype=ctypes.c_int
    u.SetWindowPos.argtypes=[w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT];u.SetWindowPos.restype=w.BOOL
    u.SetThreadDpiAwarenessContext.argtypes=[ctypes.c_void_p];u.SetThreadDpiAwarenessContext.restype=ctypes.c_void_p
    assert u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    def find_window():
        found=[]
        @callback
        def visit(hwnd,_):
            pid=w.DWORD();u.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
            if pid.value==host.process.pid:
                title=ctypes.create_unicode_buffer(256);u.GetWindowTextW(hwnd,title,256)
                if title.value.startswith('Poima player'):found.append(hwnd)
            return True
        u.EnumWindows(visit,0);assert len(found)==1,found;return found[0]
    try:
        host=shared.ProcessOwner([str(args.binary.resolve()),'serve',str(world),'--endpoint',endpoint],'native-owner',endpoint,'serve')
        client=shared.Client(endpoint,'native-client');discovery=client.call('world.describe');assert discovery['schema_revision']>=72
        for method in ('performance.start','performance.stop','performance.status','performance.frames','performance.summary'):assert method in discovery['methods']
        assert client.call('performance.status')['state']=='empty'
        client.reject('performance.start',dict(capture_id=uuid.uuid4().hex,expected_capture_id=None,player_id=uuid.uuid4().hex),-32003)
        transaction=json.loads((ROOT/'examples/physics-room.jsonl').read_text())['params'];client.call('world.transact',transaction)
        baseline=world.read_bytes();sid=uuid.uuid4().hex;client.call('runtime.start',dict(session_id=sid,revision=1))
        pool=ThreadPoolExecutor(max_workers=1)
        probe=memory.WindowsProvider(memory.Options(host.process.pid));identity=probe.creation_filetime;probe.close()
        sampling=pool.submit(memory.collect,memory.Options(host.process.pid,duration=8,interval=.2,system_ram=True,expected_creation_filetime=identity))
        previous=None;generation=0
        for slots in (2,1):
            params=shared.start_parameters(sid,generation=generation,gpu=args.gpu,frames_in_flight=slots)
            client.call('player.start',params);state=shared.await_state(client,lambda row:row['ready'],'ready-'+str(slots));generation=state['generation'];window=find_window()
            # Remain paused deliberately. Focus is observed, not required or faked.
            tick=client.call('runtime.inspect',dict(session_id=sid))['tick'];cap=uuid.uuid4().hex
            start=dict(capture_id=cap,expected_capture_id=previous,player_id=state['player_id'],capacity=4096)
            client.call('performance.start',start);assert client.call('performance.start',start)['replayed']
            time.sleep(.4);status=client.call('performance.status');assert status['count']>=2
            client.reject('performance.frames',dict(capture_id=cap),-32009)
            client.call('profiler.start',dict(capture_id=uuid.uuid4().hex,expected_capture_id=client.call('profiler.status')['capture_id'],capacity=64))
            profiler=client.call('profiler.status')['capture_id'];time.sleep(.15);client.call('profiler.stop',dict(capture_id=profiler));frozen_profiler=client.call('profiler.status')
            # An explicit screenshot is retained separately; following cadence
            # must carry the capture flag, even though capture is not a poll row.
            state=client.call('player.inspect');path=run/('capture-'+str(slots)+'.bmp')
            client.call('player.capture',dict(player_id=state['player_id'],request_id=uuid.uuid4().hex,expected_control_revision=state['control_revision'],session_id=sid,tick=state['tick'],path=str(path)))
            time.sleep(.2)
            # Resize this exact owned native window; no global mouse/keyboard use.
            assert u.SetWindowPos(window,None,0,0,850,600,0x0002|0x0004|0x0010),ctypes.get_last_error()
            time.sleep(.4)
            stopped=client.call('performance.stop',dict(capture_id=cap));assert stopped['gpu_pending']==0 and stopped['frozen']
            assert client.call('profiler.status')==frozen_profiler,'Performance drain changed existing frozen profiler'
            result=verify_capture(client,cap);result['slots']=slots;result['capture_sha256']=shared.sha(path)
            assert any(row['flags']&16 for row in result['rows']),'Screenshot intervention was lost'
            assert any(row['flags']&8 for row in result['rows']),'Resize intervention was lost'
            assert all(row['tick_before']==tick and row['tick_after']==tick for row in result['rows']),'Paused measurement advanced simulation'
            assert any(row['width']!=640 or row['height']!=480 for row in result['rows']),'Actual resized extent not observed'
            record['captures'].append(result);previous=cap
            # Overflow retains the prefix and ends admission without stopping play.
            cap=uuid.uuid4().hex;client.call('performance.start',dict(capture_id=cap,expected_capture_id=previous,player_id=state['player_id'],capacity=3))
            time.sleep(.3);full=client.call('performance.status');assert full['full'] and not full['admitting'] and full['count']==3 and full['frames_dropped']==1
            client.call('performance.stop',dict(capture_id=cap));result=verify_capture(client,cap);result['slots']=slots;result['overflow']=True;record['captures'].append(result);previous=cap
            state=client.call('player.inspect');client.call('player.control',shared.control_parameters(state,'stop'))
            final=client.call('player.inspect');assert final['state']=='finished'
            assert final['report']['success'] and final['report']['hardware'] and final['report']['nvrhi_errors']==0
            record.setdefault('players',[]).append(dict(slots=slots,gpu=final['report']['gpu'],frames_presented=final['report']['frames_presented'],swapchain_rebuilds=final['report']['swapchain_rebuilds'],nvrhi_errors=final['report']['nvrhi_errors']))
        mem=sampling.result(timeout=15);assert mem['completed'] and len(mem['samples'])>=2 and 'close_error' not in mem
        assert mem['process']['creation_filetime_100ns']==str(identity) and not mem['gpu']['qualified']
        (run/'memory.json').write_text(json.dumps(mem,indent=2));record['memory_sha256']=shared.sha(run/'memory.json');record['memory_samples']=len(mem['samples'])
        assert world.read_bytes()==baseline,'Diagnostics modified authored world'
        client.call('runtime.stop',dict(session_id=sid))
        client.call('host.shutdown');client.close();client=None;host.close();host=None;record['passed']=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        if sampling and not (run/'memory.json').exists():
            try:
                retained=sampling.result(timeout=15);(run/'memory.json').write_text(json.dumps(retained,indent=2));record['memory_sha256']=shared.sha(run/'memory.json');record['memory_samples']=len(retained['samples'])
            except BaseException:record['memory_error']=traceback.format_exc();record['passed']=False
        if pool:pool.shutdown(wait=True)
        if client:
            try:
                state=client.call('player.inspect')
                if state['active']:client.call('player.control',shared.control_parameters(state,'stop'))
                client.call('host.shutdown')
            except BaseException:record['cleanup_error']=traceback.format_exc()
            try:client.close(expected=False)
            except BaseException:record['client_close_error']=traceback.format_exc();record['passed']=False
        if host:
            try:host.close(expected=record['passed'])
            except BaseException:record['host_close_error']=traceback.format_exc();record['passed']=False
        (run/'evidence.json').write_text(json.dumps(record,indent=2));print(json.dumps(dict(passed=record['passed'],output=str(run),error=record.get('error'))),flush=True)
    return 0 if record['passed'] else 1
if __name__=='__main__':sys.exit(main())
