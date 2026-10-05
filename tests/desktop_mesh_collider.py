#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Windows typed static MeshCollider Inspector qualification; test-owned projects/windows only."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import subprocess
import struct
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
project = run/'Mesh Collider Inspector'
cli(['project','create',project,'--name','Mesh Collider Inspector'])
world=project/'world.json'
def ident(n): return f'{n:032x}'
mesh, empty, boxed, player, camera = map(ident,(10,11,12,2,3))
def rpc(method,params=None): return {'jsonrpc':'2.0','id':1,'method':method,'params':params or {}}
def transact(revision,ops): return {'base_revision':revision,'request_id':uuid.uuid4().hex,'ops':ops}
def component(entity,kind,value): return {'op':'component.set','id':entity,'type':kind,'value':value}
# Three front-facing rectangles form a real geometric doorway, not an alpha cutout.
vertices=[]; indices=[]
for x0,y0,x1,y1 in [(-2,0,-.75,3),(.75,0,2,3),(-.75,2.2,.75,3)]:
    first=len(vertices)//6
    for x,y in [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]: vertices.extend((x,y,0,0,0,1))
    indices.extend((first,first+1,first+2,first,first+2,first+3))
vertex_bytes=struct.pack('<'+'f'*len(vertices),*vertices)
blob=vertex_bytes+struct.pack('<'+'I'*len(indices),*indices)
doc={'asset':{'version':'2.0','generator':'Poima original doorway fixture'},'buffers':[{'byteLength':len(blob)}],
     'bufferViews':[{'buffer':0,'byteLength':len(vertex_bytes),'byteStride':24},
                    {'buffer':0,'byteOffset':len(vertex_bytes),'byteLength':len(indices)*4}],
     'accessors':[{'bufferView':0,'componentType':5126,'count':12,'type':'VEC3','min':[-2,0,0],'max':[2,3,0]},
                  {'bufferView':0,'byteOffset':12,'componentType':5126,'count':12,'type':'VEC3'},
                  {'bufferView':1,'componentType':5125,'count':18,'type':'SCALAR'}],
     'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2}]}],
     'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
source=run/'doorway.glb';source.write_bytes(glb(doc,blob))
imported=cli(['world',world],json.dumps(rpc('asset.import',{'source':str(source)}))+'\n')[0]
assert 'result' in imported,imported
asset=imported['result']['asset']
static={'asset':asset,'primitive':0,'visible':True}
box={'half_extents':[.5,.5,.5],'motion':'static','mass':10,'friction':.5,'restitution':0}
initial=json.loads(world.read_text())['revision']
ops=[]
for entity,name,x in [(mesh,'Doorway',0),(empty,'Unattached fixture',8),(boxed,'Box conflict fixture',12)]:
    ops += [{'op':'entity.create','id':entity,'name':name},
            component(entity,'Transform',{'position':[x,0,-3],'rotation':[0,0,0,1],'scale':[1,1,1]}),
            component(entity,'StaticMesh',static)]
ops += [component(boxed,'BoxCollider',box),component(player,'StaticMesh',dict(static,visible=False))]
seed=cli(['world',world],json.dumps(rpc('world.transact',transact(initial,ops)))+'\n')[0]
assert 'result' in seed,seed
revision=initial+1;final_revision=revision+9
endpoint='mesh-ui-'+uuid.uuid4().hex
session=uuid.uuid4().hex
actions=[];frame=3
def action(op,**values):
    global frame
    actions.append(dict(frame=frame,op=op,**values));frame+=2
def inspect(tag): action('inspect',tag=tag)
def attach(): action('click_control',control='StaticMesh Add static mesh collision')
def remove(): action('click_control',control='MeshCollider Remove static mesh collision')
def field(label,text): action('set_text',control='MeshCollider '+label,text=text)
action('select',id=mesh);inspect('initial')
action('draft_name',name='Unsaved name');attach();inspect('dirty_attach_blocked');action('reload')
action('set_text',control='Position X',text='NaN');attach();inspect('invalid_attach_blocked');action('reload')
attach();action('wait_text',control='MeshCollider Friction',text='0.5');inspect('attached')
action('undo');inspect('attach_undo');action('redo');inspect('attach_redo')
field('Primitive index','1.5');remove();action('apply',error_contains='invalid Inspector');inspect('invalid_remove_blocked');action('reload')
field('Collision asset','not-a-hash');field('Friction','2.1');field('Restitution','-0.1')
action('apply',error_contains='invalid Inspector');inspect('invalid_fields');action('reload')
field('Friction','0.8');field('Restitution','0.2');action('apply');inspect('materials_saved')
action('draft_name',name='Keep this draft');remove();inspect('dirty_remove_blocked');action('reload')
remove();inspect('removed');action('undo');inspect('remove_undo');action('redo');inspect('remove_redo');action('undo')
field('Friction','0.3')
action('rpc',method='world.transact',params=transact(revision+8,[{'op':'entity.rename','id':mesh,'name':'Doorway external edit'}]))
action('apply',error_contains='Revision conflict');remove();inspect('conflict_preserved');action('reload');inspect('conflict_reloaded')
for entity,tag in [(boxed,'box_blocked'),(player,'character_blocked')]:
    action('select',id=entity)
    action('click_control',control='StaticMesh Add static mesh collision',error_contains='enabled visible control')
    inspect(tag)
action('select',id=mesh)
action('rpc',method='desktop.play.start',params={'revision':final_revision,'session_id':session,'paused':True})
remove();inspect('play_remove_blocked')
action('select',id=empty);attach();inspect('play_attach_blocked');action('select',id=mesh)
for tag,x,z,direction in [('gap',0,0,[0,0,-1]),('solid',1.5,0,[0,0,-1]),('backside',1.5,-5,[0,0,1])]:
    action('rpc',method='runtime.raycast',params={'session_id':session,'tick':0,'origin':[x,1,z],'direction':direction,'distance':5,'ignore':[player]},tag=tag)
action('rpc',method='desktop.play.stop',params={'session_id':session})
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
        deadline=time.monotonic()+65; next_poll=0; captured=False
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
        def collider(tag): return draft(tag)['components'].get('MeshCollider')
        default={'asset':asset,'primitive':0,'friction':.5,'restitution':0}
        saved=dict(default,friction=.8,restitution=.2)
        assert not collider('initial') and draft('initial')['revision']==revision
        for tag in ['dirty_attach_blocked','invalid_attach_blocked']:
            assert collider(tag) is None and draft(tag)['dirty'] and draft(tag)['revision']==revision,(tag,draft(tag))
        assert draft('dirty_attach_blocked')['name']=='Unsaved name'
        assert 'position0' in draft('invalid_attach_blocked')['invalid_fields']
        assert collider('attached')==default and draft('attached')['revision']==revision+1
        assert collider('attach_undo') is None and draft('attach_undo')['revision']==revision+2
        assert collider('attach_redo')==default and draft('attach_redo')['revision']==revision+3
        assert collider('invalid_remove_blocked')==default and draft('invalid_remove_blocked')['revision']==revision+3
        assert 'MeshCollider.input.primitive' in draft('invalid_remove_blocked')['invalid_fields']
        assert set(draft('invalid_fields')['invalid_fields'])=={'MeshCollider.input.asset','MeshCollider.input.friction','MeshCollider.input.restitution'}
        assert collider('materials_saved')==saved and not draft('materials_saved')['dirty']
        assert draft('materials_saved')['revision']==revision+4
        assert collider('dirty_remove_blocked')==saved and draft('dirty_remove_blocked')['name']=='Keep this draft'
        assert draft('dirty_remove_blocked')['dirty'] and draft('dirty_remove_blocked')['revision']==revision+4
        for tag,offset,expected in [('removed',5,None),('remove_undo',6,saved),('remove_redo',7,None)]:
            assert collider(tag)==expected and draft(tag)['revision']==revision+offset,(tag,draft(tag))
        assert draft('conflict_preserved')['conflict'] and collider('conflict_preserved')['friction']==.3
        assert draft('conflict_preserved')['base_revision']==revision+8 and draft('conflict_preserved')['revision']==final_revision
        assert collider('conflict_reloaded')==saved and not draft('conflict_reloaded')['dirty']
        assert draft('conflict_reloaded')['name']=='Doorway external edit'
        for tag in ['box_blocked','character_blocked','play_attach_blocked']:
            assert collider(tag) is None and draft(tag)['revision']==final_revision,(tag,draft(tag))
        assert collider('play_remove_blocked')==saved and draft('play_remove_blocked')['revision']==final_revision
        assert stages['play_remove_blocked']['playback']['state']=='paused'
        assert stages['gap']['result']['hit'] is None,stages['gap']
        for tag in ['solid','backside']:
            hit=stages[tag]['result']['hit']
            assert hit and hit['entity']==mesh and hit['triangle'] is not None,(tag,hit)
            assert abs(hit['distance']-(3 if tag=='solid' else 2))<1e-4,(tag,hit)
        for tag in ['scene_capture','game_capture']:
            state=stages[tag]['native']
            assert state['capture']['state']=='complete' and state['selected']==mesh and stages[tag]['draft']['entity']==mesh,(tag,state)
            assert state['views']['scene']['graphics_error'] is None and state['views']['game']['graphics_error'] is None
        assert stages['game_capture']['native']['binding_mode']=='explicit'
        assert (run/'scene.bmp').stat().st_size>10000 and (run/'game.bmp').stat().st_size>10000
        final=json.loads(world.read_text());assert final['revision']==final_revision
        entity=cli(['world',world],json.dumps(rpc('entity.get',{'id':mesh}))+'\n')[0]['result']['value']
        assert entity['components']['MeshCollider']==saved and entity['components']['StaticMesh']==static
        assert entity['name']=='Doorway external edit'
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
