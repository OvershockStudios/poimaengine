#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows typed rig and paused live animation controls qualification; test-owned projects/windows only."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import subprocess
from animation_fixture import ribbon
from gltf_fixture import glb
import time
import uuid
from PIL import ImageGrab

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--editor', type=Path, required=True)
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gpu', type=int, default=1)
a = p.parse_args()
run = a.output.resolve()/uuid.uuid4().hex
run.mkdir(parents=True)
record = {'passed': False, 'gpu': a.gpu, 'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'editor_sha256': hashlib.sha256((a.editor.parent/'Poima.Editor.dll').read_bytes()).hexdigest(),
          'input': 'Accessible control semantic actions; physical input is not qualified.'}
def cli(args, data=None, timeout=20):
    done = subprocess.run([str(a.binary.resolve()), *map(str,args)], input=data, capture_output=True,
                          text=True, encoding='utf-8', timeout=timeout)
    assert done.returncode == 0, done.stdout+done.stderr
    return [json.loads(line) for line in done.stdout.splitlines()]
project=run/'Animation Controls'
cli(['project','create',project,'--name','Animation Controls'])
world=project/'world.json'
def ident(n): return f'{n:032x}'
rig,camera=map(ident,(100,3))
def rpc(method,params=None): return {'jsonrpc':'2.0','id':1,'method':method,'params':params or {}}
def transact(revision,ops): return {'base_revision':revision,'request_id':uuid.uuid4().hex,'ops':ops}
doc,blob=ribbon();source=run/'ribbon.glb';source.write_bytes(glb(doc,blob))
imported=cli(['world',world],json.dumps(rpc('asset.import',{'source':str(source)}))+'\n')[0]
assert 'result' in imported,imported
asset=imported['result']['asset']
initial=json.loads(world.read_text())['revision']
ops=[{'op':'asset.instantiate','id':rig,'asset':asset,'name':'Animated ribbon'},
     {'op':'component.set','id':rig,'type':'Transform','value':{'position':[0,0,-3],'rotation':[0,0,0,1],'scale':[1,1,1]}}]
seed=cli(['world',world],json.dumps(rpc('world.transact',transact(initial,ops)))+'\n')[0]
assert 'result' in seed,seed
nodes=cli(['world',world],json.dumps(rpc('entity.query',{'component':'RigNode'}))+'\n')[0]['result']['entities']
tip=None
for node in nodes:
    value=cli(['world',world],json.dumps(rpc('entity.get',{'id':node['id']}))+'\n')[0]['result']['value']
    if value['components']['RigNode']['node']==1:tip=node['id']
assert tip is not None
revision=initial+1;final_revision=revision+4
endpoint='animation-ui-'+uuid.uuid4().hex
session=uuid.uuid4().hex;session2=uuid.uuid4().hex
actions=[];frame=3
def action(op,**values):
    global frame
    actions.append(dict(frame=frame,op=op,**values));frame+=2
def inspect(tag): action('inspect',tag=tag)
def authored(label,text): action('set_text',control='AnimationRig '+label,text=text)
def live(label,text): action('set_text',control='AnimationRig Live '+label,text=text)
def press(label): action('click_control',control='AnimationRig '+label)
def load(): press('Load live state')
def pose(tag):
    action('runtime_entity',id=rig,tag=tag)
    action('runtime_entity',id=tip,tag=tag+'_tip')
def step(tick,ticks=1,**extra):
    action('rpc',method='desktop.play.step',params=dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=tick,ticks=ticks,**extra))
action('select',id=rig);inspect('untouched')
action('choose_control',control='AnimationRig Initial clip',choice='0 · Bend',tag='named_clip')
authored('Time (s)','0.25');authored('Speed','1');action('apply');inspect('initial_saved')
action('undo');inspect('initial_undo');action('redo');inspect('initial_redo')
authored('Time (s)','NaN');authored('Speed','9');action('apply',error_contains='invalid Inspector');inspect('invalid_initial');action('reload')
authored('Time (s)','1')
action('rpc',method='world.transact',params=transact(revision+3,[{'op':'entity.rename','id':rig,'name':'Ribbon external edit'}]))
action('apply',error_contains='Revision conflict');inspect('conflict');action('reload');inspect('conflict_reloaded')
action('rpc',method='desktop.play.start',params={'revision':final_revision,'session_id':session,'paused':True})
action('wait_text',control='AnimationRig Live Time (s)',text='0.25');pose('started')
live('Time (s)','0.5');press('Live Seek hold');pose('held')
load();live('Clip index (blank = rest)','');live('Blend ticks','4');press('Live Crossfade');pose('to_rest')
action('assert_text',control='AnimationRig Live status',text='Paused · Tick 2 · Clip rest · Time 0 s\nSpeed 1 · held\nBlend 0.25 · 1/4 ticks\nSource: clip 0 at 0.5 s (held)')
step(2);pose('half_rest')
press('Live Crossfade');pose('stale_command')
action('assert_text',control='AnimationRig Live Clip index (blank = rest)',text='')
load();live('Clip index (blank = rest)','0');live('Time (s)','1');live('Blend ticks','6')
action('check_control',control='AnimationRig Live Destination playing',checked=True)
press('Live Crossfade');pose('interrupted')
action('assert_text',control='AnimationRig Live status',text='Paused · Tick 4 · Clip 0 · Time 1.01667 s\nSpeed 1 · playing\nBlend 0.166667 · 1/6 ticks\nSource: frozen pose')
action('draft_name',name='Keep authored draft');press('Live Crossfade');inspect('dirty_blocked');pose('dirty_same_tick');action('reload')
live('Time (s)','-1');press('Live Seek hold');pose('invalid_live')
action('assert_text',control='AnimationRig Live Time (s)',text='-1')
load();live('Time (s)','0.75');live('Speed','0');press('Live Play');pose('zero_speed')
step(5,animations=[{'entity':rig,'clip':None,'time':0,'speed':1,'loop':True,'playing':False}])
press('Live Play');pose('external_tick_guard')
action('assert_text',control='AnimationRig Live Time (s)',text='0.75')
action('rpc',method='desktop.play.resume',params={'session_id':session})
action('click_control',control='AnimationRig Live Crossfade',error_contains='enabled visible control')
action('rpc',method='desktop.play.pause',params={'session_id':session})
action('rpc',method='desktop.play.stop',params={'session_id':session});inspect('stopped')
action('rpc',method='desktop.play.start',params={'revision':final_revision,'session_id':session2,'paused':True})
action('wait_text',control='AnimationRig Live Time (s)',text='0.25');pose('restarted')
live('Time (s)','0');press('Live Play');load()
live('Time (s)','1.5');live('Blend ticks','4');action('check_control',control='AnimationRig Live Destination playing',checked=False)
press('Live Crossfade');pose('advancing_source')
action('wait_scene_error',expected=False)
action('rpc',method='desktop.capture',params={'revision':final_revision,'path':str(run/'scene.bmp'),'view':'scene'})
frame+=8;inspect('scene_capture')
action('game_camera',camera=camera);action('show_panel',panel='Game');action('wait_scene_error',expected=False,view='game')
action('rpc',method='desktop.capture',params={'revision':final_revision,'path':str(run/'game.bmp'),'view':'game'})
frame+=8;inspect('game_capture')
script=run/'actions.json';script.write_text(json.dumps({'actions':actions},indent=2),encoding='utf-8')
report=run/'report.json'

u = c.WinDLL('user32', use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.IsWindowVisible.argtypes = [w.HWND]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.GetWindowLongPtrW.argtypes = [w.HWND, c.c_int]; u.GetWindowLongPtrW.restype = c.c_ssize_t
u.SetThreadDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetThreadDpiAwarenessContext(c.c_void_p(-4))
u.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]; u.OpenInputDesktop.restype = w.HANDLE
u.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
u.GetUserObjectInformationW.restype = w.BOOL
u.CloseDesktop.argtypes = [w.HANDLE]; u.CloseDesktop.restype = w.BOOL
wts = c.WinDLL('wtsapi32', use_last_error=True)
wts.WTSQuerySessionInformationW.argtypes = [w.HANDLE, w.DWORD, c.c_int, c.POINTER(c.c_void_p), c.POINTER(w.DWORD)]
wts.WTSQuerySessionInformationW.restype = w.BOOL
wts.WTSFreeMemory.argtypes = [c.c_void_p]
class WtsSessionLevel1(c.Structure):
    _fields_ = [('SessionId', w.DWORD), ('SessionState', c.c_int), ('SessionFlags', w.LONG),
                ('WinStationName', w.WCHAR * 33), ('UserName', w.WCHAR * 21), ('DomainName', w.WCHAR * 18),
                ('LogonTime', c.c_longlong), ('ConnectTime', c.c_longlong), ('DisconnectTime', c.c_longlong),
                ('LastInputTime', c.c_longlong), ('CurrentTime', c.c_longlong),
                ('IncomingBytes', w.DWORD), ('OutgoingBytes', w.DWORD),
                ('IncomingFrames', w.DWORD), ('OutgoingFrames', w.DWORD),
                ('IncomingCompressedBytes', w.DWORD), ('OutgoingCompressedBytes', w.DWORD)]
class WtsSessionInfo(c.Structure):
    _fields_ = [('Level', w.DWORD), ('Data', WtsSessionLevel1)]
def input_desktop_available():
    # Read-only check: never unlock, switch desktops or inject global input.
    # Modern lock surfaces may coexist with an accessible Default desktop.
    # Require an explicitly unlocked current session as well (Windows 10+).
    # https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w
    buffer = c.c_void_p(); size = w.DWORD()
    if not wts.WTSQuerySessionInformationW(None, 0xFFFFFFFF, 25, c.byref(buffer), c.byref(size)):
        return False, f'Cannot verify current session lock state (error {c.get_last_error()}).'
    try:
        if not buffer or size.value < c.sizeof(WtsSessionInfo):
            return False, 'Current session lock-state response is incomplete.'
        session = c.cast(buffer, c.POINTER(WtsSessionInfo)).contents
        if session.Level != 1 or session.Data.SessionFlags != 1 or session.Data.SessionState != 0:
            return False, f'Current session is locked, disconnected or unknown (level {session.Level}, state {session.Data.SessionState}, flags {session.Data.SessionFlags}).'
    finally:
        wts.WTSFreeMemory(buffer)
    desktop = u.OpenInputDesktop(0, False, 0x0001)  # DESKTOP_READOBJECTS
    if not desktop:
        return False, f'Input desktop unavailable (OpenInputDesktop error {c.get_last_error()}); it may be locked.'
    try:
        name = c.create_unicode_buffer(256); needed = w.DWORD()
        if not u.GetUserObjectInformationW(desktop, 2, name, c.sizeof(name), c.byref(needed)):  # UOI_NAME
            return False, f'Cannot identify input desktop (error {c.get_last_error()}).'
        if name.value.lower() != 'default':
            return False, f'Input desktop is {name.value!r}, not Default; no window screenshot taken.'
        return True, 'Default input desktop is available.'
    finally:
        u.CloseDesktop(desktop)
record['window_capture']={'status':'not_attempted'}
process=None; raised=None; was_topmost=False
try:
    with (run/'stdout.txt').open('w',encoding='utf-8') as stdout, (run/'stderr.txt').open('w',encoding='utf-8') as stderr:
        process=subprocess.Popen([str(a.editor.resolve()),str(world),'--gpu',str(a.gpu),'--endpoint',endpoint,
            '--script',str(script),'--frames',str(frame+100),'--report',str(report),'--layout',str(run/'layout.json')],stdout=stdout,stderr=stderr)
        deadline=time.monotonic()+75; next_poll=0; captured=False
        while process.poll() is None and time.monotonic()<deadline:
            now=time.monotonic()
            if now>=next_poll and (run/'game.bmp').is_file() and not captured:
                next_poll=now+.25
                reply=cli(['connect',endpoint],json.dumps(rpc('desktop.inspect'))+'\n',timeout=5)[0]
                state=reply['result']; capture=state.get('capture') or {}
                if capture.get('state')=='complete' and Path(capture['path']).name=='game.bmp':
                    assert state['binding_mode']=='explicit' and state['views']['game']['camera']==camera and state['revision']==final_revision, state
                    handles=[]
                    @callback
                    def enum(hwnd,_):
                        pid=w.DWORD();u.GetWindowThreadProcessId(hwnd,c.byref(pid))
                        if pid.value==process.pid and u.IsWindowVisible(hwnd): handles.append(hwnd)
                        return True
                    u.EnumWindows(enum,0)
                    if len(handles)!=1: continue
                    available,reason=input_desktop_available()
                    if available:
                        raised=handles[0];was_topmost=bool(u.GetWindowLongPtrW(raised,-20)&8)
                        assert u.SetWindowPos(raised,w.HWND(-1),60,40,1440,920,0x10)
                        time.sleep(.3)
                        available,reason=input_desktop_available()
                        if available:
                            rectangle=w.RECT();origin=w.POINT()
                            assert u.GetClientRect(raised,c.byref(rectangle)) and u.ClientToScreen(raised,c.byref(origin))
                            image=ImageGrab.grab(bbox=(origin.x,origin.y,origin.x+rectangle.right,origin.y+rectangle.bottom),all_screens=True)
                            available,reason=input_desktop_available()
                            if available: image.save(run/'window.png')
                            image.close()
                        u.SetWindowPos(raised,w.HWND(-1 if was_topmost else -2),0,0,0,0,0x13);raised=None
                    record['window_capture']=({'status':'captured','path':'window.png'} if available else {'status':'unavailable','reason':reason})
                    captured=True
            time.sleep(.04)
        assert process.poll() is not None,'Editor exceeded bounded lifetime'
        record['exit_code']=process.returncode
        assert report.is_file(),(run/'stderr.txt').read_text()
        evidence=json.loads(report.read_text());record['desktop']=evidence
        assert process.returncode==0 and evidence['success'],evidence
        assert evidence['ui_backend_actual']=='Avalonia.Vulkan.VulkanPlatformGraphics'
        assert len(evidence['actions'])==len(actions)
        stages={spec['tag']:result for spec,result in zip(actions,evidence['actions']) if 'tag' in spec}
        def draft(tag): return stages[tag]['draft']
        def animation(tag): return stages[tag]['result']['animation']
        def tick(tag): return stages[tag]['result']['tick']
        def x(tag): return stages[tag+'_tip']['result']['local_transform']['position'][0]
        def near(actual,expected): assert abs(actual-expected)<2e-6,(actual,expected)
        rest={'asset':asset,'clip':None,'time':0,'speed':1,'loop':True,'playing':False}
        initial_state=dict(rest,clip=0,time=.25)
        assert draft('untouched')['components']['AnimationRig']==rest and not draft('untouched')['dirty']
        assert stages['named_clip']['options']==['Rest pose','0 · Bend']
        for tag in ['initial_saved','initial_redo','conflict_reloaded','stopped']:
            assert draft(tag)['components']['AnimationRig']==initial_state and not draft(tag)['dirty'],(tag,draft(tag))
        assert draft('initial_saved')['revision']==revision+1 and draft('initial_undo')['components']['AnimationRig']==rest
        assert draft('initial_redo')['revision']==revision+3
        assert set(draft('invalid_initial')['invalid_fields'])=={'AnimationRig.input.time','AnimationRig.input.speed'}
        assert draft('invalid_initial')['revision']==revision+3
        assert draft('conflict')['conflict'] and draft('conflict')['components']['AnimationRig']['time']==1
        assert draft('conflict')['base_revision']==revision+3 and draft('conflict')['revision']==final_revision
        assert draft('conflict_reloaded')['name']=='Ribbon external edit'
        assert tick('started')==0;near(animation('started')['time'],.25);near(x('started'),.25)
        assert tick('held')==1 and not animation('held')['playing'];near(x('held'),.5)
        transition=animation('to_rest')['transition']
        assert tick('to_rest')==2 and animation('to_rest')['clip'] is None
        assert transition['duration_ticks']==4 and transition['elapsed_ticks']==1 and not transition['source_frozen'] and transition['source_clip']==0
        near(transition['weight'],.25);near(x('to_rest'),.375)
        assert tick('half_rest')==3;near(animation('half_rest')['transition']['weight'],.5);near(x('half_rest'),.25)
        assert tick('stale_command')==3 and animation('stale_command')==animation('half_rest')
        transition=animation('interrupted')['transition']
        assert tick('interrupted')==4 and transition['source_frozen'] and transition['source_clip'] is None and transition['source_time'] is None
        near(transition['weight'],1/6);near(animation('interrupted')['time'],1+1/60);near(x('interrupted'),.25*5/6+(1+1/60)/6)
        assert draft('dirty_blocked')['dirty'] and draft('dirty_blocked')['name']=='Keep authored draft'
        for tag in ['dirty_same_tick','invalid_live']:
            assert tick(tag)==4 and animation(tag)==animation('interrupted'),(tag,stages[tag])
        assert tick('zero_speed')==5 and animation('zero_speed')['playing'] and animation('zero_speed')['speed']==0 and animation('zero_speed')['transition'] is None
        near(x('zero_speed'),.75)
        assert tick('external_tick_guard')==6 and animation('external_tick_guard')['clip'] is None
        assert stages['stopped']['playback']['state']=='stopped'
        assert tick('restarted')==0;near(animation('restarted')['time'],.25);near(x('restarted'),.25)
        transition=animation('advancing_source')['transition']
        assert tick('advancing_source')==2 and not transition['source_frozen'] and transition['source_playing'] and transition['source_clip']==0
        near(transition['source_time'],2/60);near(transition['weight'],.25);near(x('advancing_source'),(2/60)*.75+1.5*.25)
        for tag in ['scene_capture','game_capture']:
            state=stages[tag]['native']
            assert state['capture']['state']=='complete' and state['selected']==rig and stages[tag]['draft']['entity']==rig,(tag,state)
            assert state['runtime']['tick']==2 and stages[tag]['playback']['state']=='paused'
            assert state['views']['scene']['graphics_error'] is None and state['views']['game']['graphics_error'] is None
            assert draft(tag)['components']['AnimationRig']==initial_state and not draft(tag)['dirty']
        assert (run/'scene.bmp').stat().st_size>10000 and (run/'game.bmp').stat().st_size>10000
        final=json.loads(world.read_text());assert final['revision']==final_revision
        entity=cli(['world',world],json.dumps(rpc('entity.get',{'id':rig}))+'\n')[0]['result']['value']
        assert entity['components']['AnimationRig']==initial_state and entity['name']=='Ribbon external edit'
        assert captured and record['window_capture']['status'] in ('captured','unavailable')
        record['captures']={name:hashlib.sha256((run/name).read_bytes()).hexdigest() for name in ('scene.bmp','game.bmp')}
        if record['window_capture']['status']=='captured': record['captures']['window.png']=hashlib.sha256((run/'window.png').read_bytes()).hexdigest()
        else: assert record['window_capture']['reason'] and not (run/'window.png').exists()
        record['passed']=True
except Exception as error:
    record['error']=repr(error)
    raise
finally:
    if raised: u.SetWindowPos(raised,w.HWND(-1 if was_topmost else -2),0,0,0,0,0x13)
    if process is not None and process.poll() is None: process.terminate();process.wait(timeout=10)
    (run/'evidence.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(run/'evidence.json')
