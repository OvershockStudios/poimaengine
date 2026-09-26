#!/usr/bin/env python3
"""Windows-owned window lifecycle test; messages target only the child player.

Uses native window messages, not physical hardware input; no global SendInput.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--input-profile',action='store_true')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
run=args.output/uuid.uuid4().hex;run.mkdir()
u=c.WinDLL('user32',use_last_error=True)
callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
u.EnumWindows.argtypes=[callback,w.LPARAM];u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
u.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,c.c_int];u.GetWindowTextW.restype=c.c_int
u.PostMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.PostMessageW.restype=w.BOOL
u.SetWindowPos.argtypes=[w.HWND,w.HWND,c.c_int,c.c_int,c.c_int,c.c_int,w.UINT];u.SetWindowPos.restype=w.BOOL
u.ShowWindow.argtypes=[w.HWND,c.c_int];u.SetForegroundWindow.argtypes=[w.HWND]
u.GetForegroundWindow.restype=w.HWND
u.GetClientRect.argtypes=[w.HWND,c.POINTER(w.RECT)]
# SDL renders in physical pixels. Match that coordinate space when Python
# queries/resizes the child; otherwise Windows virtualizes an unaware harness
# at e.g. 125% desktop scaling and the capture-size assertion compares DIP/px.
u.SetThreadDpiAwarenessContext.argtypes=[c.c_void_p]
u.SetThreadDpiAwarenessContext.restype=c.c_void_p
assert u.SetThreadDpiAwarenessContext(c.c_void_p(-4)),c.get_last_error() # PER_MONITOR_AWARE_V2

def uid(n):return f'{n:032x}'
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'requests':[],'responses':[],'input_source':'Targeted Win32 window messages; not physical keyboard/mouse qualification.'}
p=subprocess.Popen([str(args.binary.resolve()),'world',str((run/'world.json').resolve())],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
lines=queue.Queue()
def reader():
    for line in p.stdout:lines.put(json.loads(line))
threading.Thread(target=reader,daemon=True).start()
def send(method,params):
    r={'jsonrpc':'2.0','id':len(record['requests'])+1,'method':method,'params':params};record['requests'].append(r)
    p.stdin.write(json.dumps(r)+'\n');p.stdin.flush()
def receive():
    r=lines.get(timeout=30);record['responses'].append(r);assert 'result' in r,r;return r['result']
def post(hwnd,message,key=0,value=0):assert u.PostMessageW(hwnd,message,key,value),c.get_last_error()
def find_window():
    found=[]
    @callback
    def visit(hwnd,_):
        pid=w.DWORD();u.GetWindowThreadProcessId(hwnd,c.byref(pid))
        if pid.value==p.pid:
            title=c.create_unicode_buffer(512);u.GetWindowTextW(hwnd,title,512)
            if title.value.startswith('Poima player'):found.append(hwnd)
        return True
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        u.EnumWindows(visit,0)
        if found:return found[0]
        assert p.poll() is None,'Child exited before opening its window.'
        time.sleep(.02)
    raise AssertionError('Player window did not appear.')
try:
    fixture=json.loads((Path(__file__).resolve().parents[1]/'examples/physics-room.jsonl').read_text())
    send('world.transact',fixture['params']);receive()
    send('runtime.start',{'session_id':uid(900),'revision':1});receive()
    send('runtime.step',{'session_id':uid(900),'request_id':uid(1000),'expected_tick':0,'ticks':120});receive()
    play={'session_id':uid(900),'request_id':uid(1001),'expected_tick':120,'controller':uid(100),'camera':uid(101),'mode':'interactive',
          'path':str((run/'window.bmp').resolve())}
    profile_receipt=None
    if args.input_profile:
        profile={'bindings':{'forward':['key.up'],'backward':['key.s'],'left':['key.a'],'right':['key.d'],'jump':['key.space'],'use':['key.e']},
                 'sensitivity_x':.1,'sensitivity_y':.1,'invert_x':False,'invert_y':False}
        profile_path=str((run/'rebound.poima-input.json').resolve())
        send('input.transact',{'path':profile_path,'request_id':uid(990),'expected_revision':0,'profile':profile});profile_receipt=receive()
        play.update(input_profile=profile_path,input_revision=1)
    send('runtime.play',play)
    hwnd=find_window();start=time.monotonic()
    focus_requested=bool(u.SetForegroundWindow(hwnd))
    time.sleep(.15)
    initial_foreground=u.GetForegroundWindow()==hwnd
    record['foreground_request_succeeded']=focus_requested
    record['initial_foreground']=initial_foreground
    # Explicit click also handles a window created without initial focus.
    post(hwnd,0x0201,1,100|(100<<16));post(hwnd,0x0202,0,100|(100<<16))
    time.sleep(.1)
    # UP uses the extended-key flag; W is a nonextended physical scancode.
    post(hwnd,0x0100,0x26 if args.input_profile else ord('W'),
         1|(0x48<<16)|(1<<24) if args.input_profile else 1|(0x11<<16));time.sleep(.35)
    # Minimize while a movement key is held. Restoration must clear that key.
    u.ShowWindow(hwnd,6);time.sleep(.75)
    u.ShowWindow(hwnd,9);u.SetForegroundWindow(hwnd);time.sleep(.15)
    post(hwnd,0x0201,1,100|(100<<16));post(hwnd,0x0202,0,100|(100<<16))
    assert u.SetWindowPos(hwnd,None,0,0,820,520,0x0002|0x0004),c.get_last_error()
    time.sleep(.35)
    rect=w.RECT();assert u.GetClientRect(hwnd,c.byref(rect));expected_size=[rect.right,rect.bottom]
    post(hwnd,0x0010)
    played=receive();elapsed=time.monotonic()-start
    assert played['success'] and played['stop_reason']=='window_closed',played
    assert played['capture_written'] and played['nvrhi_errors']==0 and played['swapchain_rebuilds']>=2,played
    if args.input_profile:
        metadata=played['input_profile'];assert metadata['source']=='profile' and metadata['applied'] and metadata['revision']==1,metadata
        assert metadata['content_hash']==profile_receipt['content_hash'];record['input_profile']=metadata
    assert [played['width'],played['height']]==expected_size,(played,expected_size)
    send('runtime.entity',{'session_id':uid(900),'id':uid(100)});state=receive()
    # Loose wall-time bounds avoid interpreting scheduling jitter as game speed.
    # At least half the minimized duration must be excluded from simulation.
    assert 0<=played['tick']-120<(elapsed-.35)*60,(played,elapsed)
    z=state['world_matrix'][14]
    if initial_foreground:
        assert played['tick']>125 and -.5<z<1.8,(state,played)
        record['input_qualification']='Targeted synthetic key message moved the focused player; physical devices remain unqualified.'
    else:
        record['input_qualification']='Foreground unavailable: movement and physical input remain unqualified by this desktop run.'
    assert state['velocity'][2]==0,'Held movement survived focus loss.'
    send('runtime.inspect',{'session_id':uid(900)});assert receive()['tick']==played['tick']
    send('session.close',{});receive();p.stdin.close();assert p.wait(timeout=10)==0
    record.update(passed=True,elapsed_seconds=elapsed,expected_size=expected_size,checks={'targeted_keyboard_moves_character':initial_foreground,'focus_loss_clears_held_input':initial_foreground,
        'minimize_restore_survives':True,'paused_time_bound':True,'resize_rebuilds_swapchain_and_capture':True,'window_close_preserves_runtime':True},image={'path':str((run/'window.bmp').resolve()),'sha256':hashlib.sha256((run/'window.bmp').read_bytes()).hexdigest()})
    if args.input_profile:record['checks']['saved_remapping_moves_character_with_synthetic_up_key']=initial_foreground
finally:
    while not lines.empty():record['responses'].append(lines.get_nowait())
    if p.poll() is None:p.kill();p.wait(timeout=10)
    record['stderr']=p.stderr.read();record['exit_code']=p.returncode
    (args.output/'window.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':True,'checks':record['checks'],'tick':played['tick'],'size':expected_size}))
