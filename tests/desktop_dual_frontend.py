#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows independent Scene/Game GUI qualification; owned windows only."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid
from PIL import ImageGrab

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--editor',type=Path,required=True);p.add_argument('--binary',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,default=1)
a=p.parse_args();run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
record={'passed':False,'gpu':a.gpu,'source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'editor_sha256':hashlib.sha256((a.editor.parent/'Poima.Editor.dll').read_bytes()).hexdigest(),
        'input':'Semantic messages to two distinct owned HWNDs; physical input not qualified.'}
def cli(args,data=None,timeout=20):
    done=subprocess.run([str(a.binary.resolve()),*map(str,args)],input=data,capture_output=True,text=True,encoding='utf-8',timeout=timeout)
    assert done.returncode==0,done.stdout+done.stderr
    return [json.loads(line) for line in done.stdout.splitlines()]
def rpc(method,params=None):return {'jsonrpc':'2.0','id':1,'method':method,'params':params or {}}
project=run/'Dual View';cli(['project','create',project,'--name','Dual View'])
world=project/'world.json';before=json.loads(world.read_text());revision=before['revision']
player,camera,environment=(f'{n:032x}' for n in (2,3,4))
endpoint='dual-ui-'+uuid.uuid4().hex
actions=[];frame=3
def action(op,advance=2,**values):
    global frame
    actions.append(dict(frame=frame,op=op,**values));frame+=advance
def inspect(tag):action('inspect',tag=tag)
def wait_views():
    action('wait_scene_error',expected=False,view='scene');action('wait_scene_error',expected=False,view='game')
def capture(view,name):
    global frame
    action('rpc',method='desktop.capture',params={'view':view,'revision':revision,'path':str(run/name)})
    frame+=6

def acquire():
    action('game_input',message='left_down');action('game_input',message='left_up')
def key(code,scan,down=True,**extra):action('game_input',message='key_down' if down else 'key_up',key=code,scan=scan,**extra)
action('select',id=environment);inspect('default_layout')
action('reset_layout',split_views=True);wait_views();inspect('both_ready')
action('assert_text',control='LightingEnvironment Ambient R',text='0.12',tag='narrow_ambient_r')
action('assert_text',control='LightingEnvironment Ambient G',text='0.14',tag='narrow_ambient_g')
action('assert_text',control='LightingEnvironment Ambient B',text='0.18',tag='narrow_ambient_b')
action('assert_text',control='Position X',text='0',tag='narrow_position_x')
capture('game','game-before.bmp');inspect('game_before_capture')
action('scene_input',message='right_down',x=.5,y=.5)
action('scene_input',message='move',x=.62,y=.56)
action('scene_input',message='right_up',x=.62,y=.56)
inspect('scene_navigated');capture('game','game-after-scene.bmp');capture('scene','scene-after.bmp')
inspect('independent_captures')
action('runtime_toggle',advance=0);action('runtime_pause')
action('step',ticks=60);action('runtime_entity',id=player,tag='settled')
action('runtime_pause');acquire();inspect('game_acquired')
key(87,0x11);action('game_motion',dx=12,dy=-4);key(32,0x39,tag='jump_press');inspect('jump_queued')
frame+=5;key(32,0x39,False);key(87,0x11,False)
action('runtime_entity',id=player,tag='player_moved');inspect('playing_both')
# The Scene HWND steals only Game focus; it never routes W into the controller.
key(87,0x11);action('scene_input',message='right_down',x=.4,y=.4)
action('scene_input',message='move',x=.48,y=.45);action('scene_input',message='right_up',x=.48,y=.45)
inspect('scene_focus_release')
acquire();key(87,0x11);action('game_input',message='focus_lost');inspect('focus_release')
acquire();key(87,0x11);action('runtime_pause');inspect('paused_release')
action('step',ticks=1);wait_views();inspect('stepped_both')
action('runtime_pause');acquire();key(87,0x11)
action('resize_window',width=1320,height=850);frame+=4;inspect('resize_release')
acquire();key(87,0x11);action('float',panel='Game');frame+=6;inspect('game_floated')
action('runtime_pause');action('float',panel='Scene');frame+=6;inspect('both_floated')
action('reset_layout',split_views=True);wait_views();inspect('reattached')
action('runtime_toggle');inspect('stopped')
# A broken Game selection must not poison or block Scene rendering.
action('rpc',method='world.transact',params={'base_revision':revision,'request_id':uuid.uuid4().hex,
    'ops':[{'op':'component.remove','id':player,'type':'CharacterController'},{'op':'entity.delete','id':camera,'recursive':True}]})
action('wait_scene_error',expected=True,view='game');action('wait_scene_error',expected=False,view='scene')
inspect('game_error_only');frame+=5;inspect('scene_survives')
action('undo');action('wait_scene_error',expected=False,view='scene');action('wait_scene_error',expected=False,view='game')
inspect('camera_repaired')
action('game_camera',camera=None);inspect('no_camera')
action('game_camera',camera=camera);wait_views();inspect('camera_restored')
action('save_layout',tag='saved_layout');action('load_layout');wait_views();inspect('layout_restored')
revision+=2
capture('scene','scene-final.bmp');capture('game','game-final.bmp');inspect('final')
script=run/'actions.json';script.write_text(json.dumps({'actions':actions},indent=2),encoding='utf-8');report=run/'report.json'

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
process=None;raised=None;was_topmost=False
try:
    with (run/'stdout.txt').open('w',encoding='utf-8') as stdout,(run/'stderr.txt').open('w',encoding='utf-8') as stderr:
        process=subprocess.Popen([str(a.editor.resolve()),str(world),'--gpu',str(a.gpu),'--endpoint',endpoint,
            '--script',str(script),'--frames',str(frame+100),'--report',str(report),'--layout',str(run/'layout.json')],stdout=stdout,stderr=stderr)
        deadline=time.monotonic()+65;next_poll=0;captured=False
        while process.poll() is None and time.monotonic()<deadline:
            now=time.monotonic()
            if now>=next_poll and (run/'game-final.bmp').is_file() and not captured:
                next_poll=now+.25
                try:reply=cli(['connect',endpoint],json.dumps(rpc('desktop.inspect'))+'\n',timeout=5)[0]
                except (subprocess.TimeoutExpired,AssertionError):
                    if process.poll() is not None:break
                    raise
                state=reply['result'];latest=state.get('capture') or {}
                if latest.get('state')=='complete' and Path(latest['path']).name=='game-final.bmp':
                    assert state['binding_mode']=='explicit' and state['views']['game']['camera']==camera
                    handles=[]
                    @callback
                    def enum(hwnd,_):
                        pid=w.DWORD();u.GetWindowThreadProcessId(hwnd,c.byref(pid))
                        if pid.value==process.pid and u.IsWindowVisible(hwnd):handles.append(hwnd)
                        return True
                    u.EnumWindows(enum,0)
                    if len(handles)!=1:continue
                    available,reason=input_desktop_available()
                    if available:
                        raised=handles[0];was_topmost=bool(u.GetWindowLongPtrW(raised,-20)&8)
                        assert u.SetWindowPos(raised,w.HWND(-1),60,40,1440,920,0x10)
                        time.sleep(.3);available,reason=input_desktop_available()
                        if available:
                            rectangle=w.RECT();origin=w.POINT()
                            assert u.GetClientRect(raised,c.byref(rectangle)) and u.ClientToScreen(raised,c.byref(origin))
                            image=ImageGrab.grab(bbox=(origin.x,origin.y,origin.x+rectangle.right,origin.y+rectangle.bottom),all_screens=True)
                            available,reason=input_desktop_available()
                            if available:image.save(run/'window.png')
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
        def native(tag):return stages[tag]['native']
        def game(tag):return stages[tag]['game_input']
        def views(tag):return native(tag)['views']
        def presented(tag):
            state=native(tag)
            assert state['binding_mode']=='explicit',state
            for name in ('scene','game'):
                view=state['views'][name]
                assert view['attached'] and view['graphics_error'] is None and view['preparation_error'] is None,(tag,name,view)
                assert view['presented_revision']==state['revision'] and view['frames_presented']>0,(tag,name,view)
        for tag in ('both_ready','scene_navigated','independent_captures','playing_both','stepped_both','reattached','camera_repaired','camera_restored','layout_restored','final'):
            presented(tag)
        for name in ('narrow_ambient_r','narrow_ambient_g','narrow_ambient_b'):assert stages[name]['control_width']>=45,(name,stages[name])
        assert stages['narrow_position_x']['control_width']>=40,stages['narrow_position_x']
        ready=stages['both_ready']
        assert ready['scene_window']['hwnd']!=ready['game_window']['hwnd']
        assert ready['scene_window']['visible'] and ready['game_window']['visible']
        assert native('both_ready')['camera']!=native('scene_navigated')['camera']
        assert views('both_ready')['game']['camera']==views('scene_navigated')['game']['camera']==camera
        assert (run/'game-before.bmp').read_bytes()==(run/'game-after-scene.bmp').read_bytes(),'Scene navigation changed Game pixels'
        assert (run/'scene-after.bmp').read_bytes()!=(run/'game-after-scene.bmp').read_bytes()
        assert game('game_acquired')['captured'] and game('game_acquired')['native']['focused']
        assert game('jump_press')['native']['pending']['jump']
        settled,moved=stages['settled']['result'],stages['player_moved']['result']
        assert moved['world_matrix'][14]<settled['world_matrix'][14]-.1,(settled,moved)
        assert moved['world_matrix'][13]>settled['world_matrix'][13]+.02,(settled,moved)
        assert abs(moved['yaw']-settled['yaw']+1.2)<1e-6 and abs(moved['pitch']-settled['pitch']-.4)<1e-6,moved
        assert native('playing_both')['camera']==native('scene_navigated')['camera'],'Game look moved Scene camera'
        zero={'move':[0,0],'look':[0,0],'jump':False,'use':False}
        for tag in ('scene_focus_release','focus_release','paused_release','resize_release','game_floated','both_floated','reattached','stopped'):
            state=game(tag)
            assert not state['captured'] and not state['native']['focused'] and state['native']['pending']==zero,(tag,state)
        assert native('paused_release')['playback']['state']=='paused'
        assert native('stepped_both')['playback']['tick']==native('paused_release')['playback']['tick']+1
        for view in views('stepped_both').values():assert view['presented_tick']==native('stepped_both')['playback']['tick']
        assert native('scene_focus_release')['camera']!=native('playing_both')['camera']
        assert views('game_floated')['scene']['frames_presented']>views('resize_release')['scene']['frames_presented']
        assert len(stages['both_floated']['windows'])==3
        assert native('stopped')['playback']['state']=='stopped' and native('stopped')['revision']==before['revision']
        assert native('stopped')['camera']==native('reattached')['camera']
        broken=views('game_error_only')
        assert broken['game']['preparation_error'] and broken['game']['graphics_error'] is None
        assert broken['scene']['preparation_error'] is None and broken['scene']['graphics_error'] is None
        assert not stages['game_error_only']['game_window']['visible'],'Invalid Game camera displayed stale child pixels'
        assert views('scene_survives')['scene']['frames_presented']>broken['scene']['frames_presented']
        assert stages['camera_repaired']['game_window']['visible'],'Repaired hidden Game viewport did not recover'
        assert views('no_camera')['game']['camera'] is None and not stages['no_camera']['game_window']['visible']
        assert stages['camera_restored']['game_window']['visible']
        assert stages['saved_layout']['layout']==stages['layout_restored']['layout']
        assert native('final')['capture']['state']=='complete'
        assert (run/'scene-final.bmp').stat().st_size>10000 and (run/'game-final.bmp').stat().st_size>10000
        assert (run/'scene-final.bmp').read_bytes()!=(run/'game-final.bmp').read_bytes()
        after=json.loads(world.read_text());assert after['revision']==before['revision']+2 and after['entities']==before['entities']
        assert captured and record['window_capture']['status'] in ('captured','unavailable')
        record['captures']={name:hashlib.sha256((run/name).read_bytes()).hexdigest() for name in ('scene-final.bmp','game-final.bmp')}
        if record['window_capture']['status']=='captured':record['captures']['window.png']=hashlib.sha256((run/'window.png').read_bytes()).hexdigest()
        else:assert record['window_capture']['reason'] and not (run/'window.png').exists()
        # Restart restores the saved split layout, independently attaches both
        # views, and leaves authored bytes untouched.
        world_bytes=world.read_bytes();restart_script=run/'restart-actions.json';restart_report=run/'restart-report.json'
        restart_script.write_text(json.dumps({'actions':[{'frame':8,'op':'wait_scene_error','expected':False,'view':'scene'},
            {'frame':10,'op':'wait_scene_error','expected':False,'view':'game'},{'frame':12,'op':'inspect'}]}),encoding='utf-8')
        restarted=subprocess.run([str(a.editor.resolve()),str(world),'--gpu',str(a.gpu),'--endpoint','dual-restart-'+uuid.uuid4().hex,
            '--script',str(restart_script),'--frames','50','--report',str(restart_report),'--layout',str(run/'layout.json')],
            capture_output=True,text=True,encoding='utf-8',timeout=30)
        restart=json.loads(restart_report.read_text());record['restart']=restart
        assert restarted.returncode==0 and restart['success'],restart
        restored=restart['actions'][-1]
        assert restored['layout']==stages['saved_layout']['layout']
        assert restored['native']['views']['scene']['attached'] and restored['native']['views']['game']['attached']
        assert restored['scene_window']['visible'] and restored['game_window']['visible']
        assert world.read_bytes()==world_bytes
        record['passed']=True
except Exception as error:
    record['error']=repr(error);raise
finally:
    if raised:u.SetWindowPos(raised,w.HWND(-1 if was_topmost else -2),0,0,0,0,0x13)
    if process is not None and process.poll() is None:process.terminate();process.wait(timeout=10)
    (run/'evidence.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(run/'evidence.json')
