#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows typed Environment Inspector qualification; test-owned projects/windows only."""
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
project = run/'Sky Inspector'
cli(['project','create',project,'--name','Sky Inspector'])
world = project/'world.json'
def ident(n): return f'{n:032x}'
environment, sun, camera, point, alternate = map(ident, (4,5,3,20,21))
def rpc(method, params=None): return {'jsonrpc':'2.0','id':1,'method':method,'params':params or {}}
def transact(revision, ops): return {'base_revision':revision,'request_id':uuid.uuid4().hex,'ops':ops}
def set_env(value): return {'op':'component.set','id':environment,'type':'LightingEnvironment','value':value}
starter = cli(['world',world],json.dumps(rpc('entity.get',{'id':environment}))+'\n')[0]['result']['value']['components']['LightingEnvironment']
initial = json.loads(world.read_text())['revision']
legacy = {key:value for key,value in starter.items() if key != 'sky'}
ops = [set_env(legacy)]
for entity, name, kind in [(point,'Point fixture','point'),(alternate,'Second sun','directional')]:
    ops += [{'op':'entity.create','id':entity,'name':name},
            {'op':'component.set','id':entity,'type':'Transform','value':{'position':[0,0,0],'rotation':[0,0,0,1],'scale':[1,1,1]}},
            {'op':'component.set','id':entity,'type':'Light','value':{'kind':kind,'color':[1,1,1],'intensity':1,'enabled':True}}]
seed = cli(['world',world],json.dumps(rpc('world.transact',transact(initial,ops)))+'\n')[0]
assert 'result' in seed, seed
revision = initial+1
endpoint = 'sky-ui-'+uuid.uuid4().hex
actions=[]; frame=3
def action(op, **values):
    global frame
    actions.append(dict(frame=frame,op=op,**values)); frame+=2
def inspect(tag): action('inspect',tag=tag)
def env_field(name,text): action('set_text',control='LightingEnvironment '+name,text=text)
def choose_sun(name): action('choose_control',control='LightingEnvironment Sun',choice=name)
action('select',id=environment)
action('wait_text',control='LightingEnvironment Ambient R',text='0.12')
inspect('legacy_untouched')
env_field('Ambient R','1000000'); action('apply'); inspect('hdr_ambient')
action('undo'); inspect('ambient_undo')
action('click_control',control='LightingEnvironment Configure sky'); inspect('configured_defaults')
action('reload'); inspect('configuration_reverted')
action('click_control',control='LightingEnvironment Configure sky')
action('check_control',control='LightingEnvironment Sky enabled',checked=True)
choose_sun('Second sun · 00000000'); choose_sun('None'); choose_sun('Second sun · 00000000')
env_field('Zenith R','1.1'); action('apply',error_contains='invalid Inspector'); inspect('invalid_sky')
action('reload'); inspect('invalid_reverted')
action('click_control',control='LightingEnvironment Configure sky')
action('check_control',control='LightingEnvironment Sky enabled',checked=True)
choose_sun('Second sun · 00000000'); env_field('Horizon falloff','1.25')
action('apply'); inspect('saved_sky')
action('undo'); inspect('sky_undo'); action('redo'); inspect('sky_redo')
# Restore before testing atomic creation; creation should select the singleton.
action('rpc',method='world.transact',params=transact(revision+5,[set_env(starter)]))
action('create',kind='Environment',tag='select_existing'); inspect('existing_environment')
action('rpc',method='world.transact',params=transact(revision+6,[{'op':'entity.delete','id':environment,'recursive':True},{'op':'entity.delete','id':sun,'recursive':True}]))
action('create',kind='Environment',tag='create_environment'); inspect('created_environment')
action('create',kind='Environment',tag='select_new_existing'); inspect('no_duplicate')
action('undo'); inspect('creation_undo'); action('redo')
action('create',kind='Environment'); inspect('creation_redo')
action('assert_text',control='LightingEnvironment Sun diameter (°)',text='0.53')
action('assert_text',control='LightingEnvironment Horizon falloff',text='0.35')
action('assert_draft',expected={'dirty':False})
action('rpc',method='world.transact',params=transact(revision+10,[{'op':'entity.delete','id':point,'recursive':True},{'op':'entity.delete','id':alternate,'recursive':True}]))
final_revision=revision+11
# Freshly created default Environment + Sun, starter floor/camera: no artistic fixture overrides.
action('wait_scene_error',expected=False)
action('rpc',method='desktop.capture',params={'revision':final_revision,'path':str(run/'scene.bmp'),'view':'scene'})
frame+=8; inspect('scene_capture')
action('float',panel='Scene'); frame+=6; action('reset_layout'); frame+=6
action('wait_scene_error',expected=False)
action('game_camera',camera=camera)
action('show_panel',panel='Game')
action('wait_scene_error',expected=False,view='game')
action('rpc',method='desktop.capture',params={'revision':final_revision,'path':str(run/'game.bmp'),'view':'game'})
frame+=8; inspect('game_capture')
script=run/'actions.json'; script.write_text(json.dumps({'actions':actions},indent=2),encoding='utf-8')
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
        deadline=time.monotonic()+55; next_poll=0; captured=False
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
        def env(tag): return draft(tag)['components']['LightingEnvironment']
        for tag in ['legacy_untouched','ambient_undo','configuration_reverted','invalid_reverted','sky_undo']:
            assert env(tag)==legacy and not draft(tag)['dirty'],(tag,draft(tag))
        assert env('hdr_ambient')['ambient'][0]==1000000 and not draft('hdr_ambient')['dirty']
        defaults=cli(['world',world],json.dumps(rpc('world.describe'))+'\n')[0]['result']['components']['LightingEnvironment']['properties']['sky']['default']
        assert env('configured_defaults')['sky']==defaults and draft('configured_defaults')['dirty']
        assert draft('configured_defaults')['revision']==revision+2
        assert draft('invalid_sky')['dirty'] and 'LightingEnvironment.input.sky.zenith0' in draft('invalid_sky')['invalid_fields']
        saved=env('saved_sky')['sky']
        assert saved['enabled'] and saved['sun']==alternate and saved['horizon_falloff']==1.25
        assert env('sky_redo')['sky']==saved and not draft('sky_redo')['dirty']
        choices=[result for result in evidence['actions'] if result['op']=='choose_control']
        assert choices and all(item['options']==['None','Sun · 00000000','Second sun · 00000000'] for item in choices),choices
        assert stages['select_existing']['id']==environment and draft('existing_environment')['revision']==revision+6
        created=stages['create_environment']['id']
        assert stages['select_new_existing']['id']==created and draft('no_duplicate')['revision']==revision+8
        created_sky=env('created_environment')['sky']
        assert created_sky['enabled'] and created_sky['sun']!=sun and created_sky['sun']!=alternate
        assert draft('creation_undo')['entity'] is None and draft('creation_undo')['revision']==revision+9
        assert draft('creation_redo')['entity']==created and env('creation_redo')==env('created_environment')
        assert draft('creation_redo')['revision']==revision+10
        for tag in ('scene_capture','game_capture'):
            assert stages[tag]['draft']['entity']==created and stages[tag]['native']['selected']==created, (tag,stages[tag]['draft'])
        assert stages['scene_capture']['native']['capture']['state']=='complete'
        assert stages['game_capture']['native']['capture']['state']=='complete'
        assert stages['game_capture']['native']['binding_mode']=='explicit' and stages['game_capture']['native']['views']['game']['camera']==camera
        assert (run/'scene.bmp').stat().st_size>10000 and (run/'game.bmp').stat().st_size>10000
        final=json.loads(world.read_text());assert final['revision']==final_revision
        # Authoritative inspection proves creation was one transaction with one linked sun.
        reply=cli(['world',world],json.dumps(rpc('entity.get',{'id':created_sky['sun']}))+'\n')[0]
        light=reply['result']['value']['components']['Light']
        assert light=={'kind':'directional','color':[1,.95,.85],'intensity':3.5,'enabled':True,'shadow':{'enabled':True}},light
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
