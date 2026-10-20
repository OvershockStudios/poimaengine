#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled FPS spring relaxation through an owned background window.

Native Windows Python in an externally admitted Job is required. Consumes an
already built unchanged FPS Feedback CoreCLR game, bridge and runtime. No build,
physical input, shot, benchmark, display scan-out or new Native AOT claim.
"""
import argparse
import copy
import ctypes
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'tests'))
from poima_client import WorldClient, RpcError
from switchback_oracle import read_checkpoint

CAM,FORE=(f'{n:032x}'for n in(3,305))
CAP,FILE_CAP,RPC_CAP,SECONDS=64*1048576,512,149,180
SEED=dict(Phase=1,MenuOpen=0,AimX=.18,WeaponX=.24,WeaponBack=.06,ShakeEnvelope=.8)


def need(value,message):
    if not value:raise AssertionError(message)


def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()
def same(a,b):return encoded(a)==encoded(b)
def uid():return uuid.uuid4().hex


def pin(path):
    p=Path(path);need(p.is_file()and not p.is_symlink(),'Ordinary input/artifact required: '+str(p))
    with p.open('rb')as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    return dict(bytes=p.stat().st_size,sha256=digest)


def vector(value,n):
    need(type(value)is list and len(value)==n and all(type(v)is float and math.isfinite(v)for v in value),'Original finite double-vector shape differs')
    return value


def close(a,b,message,tolerance=2e-11):
    need(len(a)==len(b)and all(math.isfinite(x)and math.isfinite(y)and abs(x-y)<=tolerance for x,y in zip(a,b)),message)


def multiply(a,b):return [sum(a[k*4+r]*b[c*4+k]for k in range(4))for c in range(4)for r in range(4)]


def local(pose):
    x,y,z,w=pose['rotation'];norm=math.sqrt(x*x+y*y+z*z+w*w);x,y,z,w=x/norm,y/norm,z/norm,w/norm
    sx,sy,sz=pose.get('scale',[1,1,1])
    return [(1-2*(y*y+z*z))*sx,2*(x*y+z*w)*sx,2*(x*z-y*w)*sx,0.,
        2*(x*y-z*w)*sy,(1-2*(x*x+z*z))*sy,2*(y*z+x*w)*sy,0.,
        2*(x*z+y*w)*sz,2*(y*z-x*w)*sz,(1-2*(x*x+y*y))*sz,0.,*pose['position'],1.]


def slerp_oracle(a,b,alpha):
    # Independent great-circle tangent basis, unlike native relative-quaternion
    # multiplication/atan2. Work only on copies; original response values survive.
    if alpha==0:return a[:]
    if alpha==1:return b[:]
    lengths=[math.sqrt(sum(v*v for v in q))for q in(a,b)]
    u=[v/lengths[0]for v in a];v=[x/lengths[1]for x in b]
    dot=sum(x*y for x,y in zip(u,v))
    if dot<0:v=[-x for x in v];dot=-dot
    tangent=[y-dot*x for x,y in zip(u,v)];length=math.sqrt(sum(x*x for x in tangent))
    if length<1e-14:return u
    theta=math.atan2(length,max(0.,min(1.,dot)))
    q=[x*math.cos(alpha*theta)+y/length*math.sin(alpha*theta)for x,y in zip(u,tangent)]
    norm=math.sqrt(sum(x*x for x in q));return [x/norm for x in q]


def check_pose(value):
    need(type(value)is dict and set(value)=={'position','rotation'},'Complete original pose channel required')
    p=vector(value['position'],3);q=vector(value['rotation'],4)
    need(all(abs(x)<=10 for x in p)and all(abs(x)<=1.00001 for x in q)and abs(sum(x*x for x in q)-1)<=1e-5,'Original pose bounds differ')


def verify_presentation(capture,foreground_local,*,running,native=None):
    p=capture['pose_presentation'];lens=capture['lens_presentation'];diagnostics=capture['render_diagnostics']
    need(p['enabled']is True and lens['enabled']is True and type(p['interpolating'])is bool,'Both explicit interpolation options required')
    alpha=p['alpha'];need(type(alpha)is float and math.isfinite(alpha)and 0<=alpha<=1 and alpha==lens['alpha'],'One frozen finite shared clock fraction required')
    need(type(p['previous_tick'])is int and type(p['target_tick'])is int and p['target_tick']==lens['target_tick'],'Original integer Tick metadata differs')
    if running:
        need(p['previous_tick']+1==p['target_tick']and 0<alpha<1 and p['interpolating']and p['reset_reason']=='','No genuine changing fractional compiled pair')
        need(not same(p['previous_world'],p['target_world'])and not same(p['previous_viewmodel'],p['target_viewmodel']),'Both compiled channels must actually change')
    else:
        need(alpha==1 and not p['interpolating']and not lens['interpolating']and p['previous_tick']==p['target_tick'],'Paused histories did not collapse to current values')
        need(p['reset_reason']=='paused'and lens['reset_reason']=='paused','Paused history reset reason differs')
    for channel in('world','viewmodel'):
        before,target,shown=(p[key+'_'+channel]for key in('previous','target','rendered'))
        for pose in(before,target,shown):check_pose(pose)
        close(shown['position'],[a+(b-a)*alpha for a,b in zip(before['position'],target['position'])],'Local position differs from independent line segment')
        expected=slerp_oracle(before['rotation'],target['rotation'],alpha)
        # Quaternion signs encode the same rotation. Matrix comparison preserves
        # the untouched raw metadata while testing physical orientation.
        close(local(dict(position=[0.,0.,0.],rotation=shown['rotation'])),local(dict(position=[0.,0.,0.],rotation=expected)),'Rotation differs from independent shortest great-circle arc')
        if running:need(abs(sum(x*x for x in shown['rotation'])-1)<3e-13,'Interior rotation is not unit')
        if not running:need(same(shown,target),'Paused offset is not exact committed endpoint')
    base=vector(p['target_base_world'],16);camera=multiply(base,local(p['rendered_world']))
    close(vector(capture['camera_world'],16),camera,'Rendered camera does not use current base times presented local offset')
    receipts=diagnostics['scene_receipts'];scene=receipts['presented']
    need(same(scene,receipts['submitted'])and scene['frame_sequence']>0 and scene['submission_id']>0,'Capture submitted/presented ownership differs')
    need(scene['camera_id']==CAM and scene['camera_draw']['viewmodels']==[dict(entity_id=FORE,incarnation=1)],'Actual foreground membership differs')
    rendered=scene['camera_pose'];close(vector(rendered['camera_world'],16),camera,'Renderer receipt camera matrix differs')
    need(len(rendered['viewmodels'])==1 and rendered['viewmodels'][0]['entity']==FORE and rendered['viewmodels'][0]['incarnation']==1,'Actual foreground matrix identity differs')
    expected_fore=multiply(multiply(camera,local(p['rendered_viewmodel'])),local(foreground_local))
    close(vector(rendered['viewmodels'][0]['world'],16),expected_fore,'Foreground matrix does not independently compose camera/VM/logical weapon local')
    for channel in('world','viewmodel'):
        a,b,r=(lens[k+'_'+channel]for k in('previous','target','rendered'))
        need(a['near']==b['near']==r['near']and a['far']==b['far']==r['far'],'Clipping was interpolated')
        fov=math.degrees(2*math.atan(math.tan(math.radians(a['vertical_fov'])/2)**(1-alpha)*math.tan(math.radians(b['vertical_fov'])/2)**alpha))
        if channel=='world'and lens['preference_fov']is not None:
            pref=lens['preference_fov'];authored=lens['authored_fov']
            fov=pref if authored is None else max(5,min(150,math.degrees(2*math.atan(math.tan(math.radians(pref)/2)*math.tan(math.radians(fov)/2)/math.tan(math.radians(authored)/2)))))
        close([r['vertical_fov']],[fov],'Lens path does not match independent perspective-scale interpolation')
    need(same(scene['camera_lens'],lens['rendered_world'])and same(scene['camera_draw']['viewmodel_lens'],lens['rendered_viewmodel']),'Renderer lens receipt differs')
    if native is not None:
        need(same(p['target_world'],native['world'])and same(p['target_viewmodel'],native['viewmodel'])and same(p['target_base_world'],native['base_world_matrix']),'Paused raw native pose/current BaseWorld differs from frozen target')
    return dict(alpha=alpha,previous_tick=p['previous_tick'],target_tick=p['target_tick'],running=running)


def require_job():
    from ctypes import wintypes as w
    k=ctypes.WinDLL('kernel32',use_last_error=True);k.GetCurrentProcess.restype=w.HANDLE
    k.IsProcessInJob.argtypes=[w.HANDLE,w.HANDLE,ctypes.POINTER(w.BOOL)];k.IsProcessInJob.restype=w.BOOL
    inside=w.BOOL();need(k.IsProcessInJob(k.GetCurrentProcess(),None,ctypes.byref(inside))and inside.value,'External Windows Job admission required before any native process')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in('binary','hostfxr','bridge','assembly','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,required=True);args=parser.parse_args()
    need(os.name=='nt'and not sys.flags.optimize,'Native Windows Python required');require_job()
    for name in('binary','hostfxr','bridge','assembly'):setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.absolute();need(args.gpu>=0 and not args.output.exists()and args.output.parent.is_dir(),'Explicit GPU/fresh owned output/existing parent')
    sample=ROOT/'examples/fps-feedback';spec=importlib.util.spec_from_file_location('fps_public_prepare',sample/'prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    sources=[Path(__file__),ROOT/'tests/switchback_oracle.py',args.binary,args.hostfxr,args.bridge,args.assembly]
    sources += [p for p in sample.iterdir()if p.is_file()and p.suffix in('.cs','.csproj','.py')]
    sources += [ROOT/'packages/fps/Feedback.cs',ROOT/'packages/fps/Recoil.cs']
    sources += [p for p in(ROOT/'tools/python/poima_client').iterdir()if p.is_file()and p.suffix in('.py','.json')]
    for image in(args.bridge,args.assembly):
        sources.append(image.parent/'Poima.Gameplay.dll')
        for suffix in('.deps.json','.runtimeconfig.json'):
            p=image.with_suffix(suffix)
            if p.is_file():sources.append(p)
    if(args.binary.parent/'phonon.dll').is_file():sources.append(args.binary.parent/'phonon.dll')
    pins={str(p):pin(p)for p in set(sources)};basis=prepare.inputs();args.output.mkdir();(args.output/'captures').mkdir();saves=args.output/'saves';saves.mkdir()
    started=time.monotonic();deadline=started+SECONDS;client=None;host=None;player=None;session=uid();cleanup=[];fields=None
    record=dict(format='poima.fps-pose-player.v1',passed=False,errors=[],calls=[],captures=[],checkpoints=[],whole_pairs=[],source_pins=pins,
        seed=SEED,recipe=dict(mode='background',width=640,height=360,samples=4,viewmodel_samples=4,max_frames=60,captures=4,gpu=args.gpu,pose_interpolation=True,lens_interpolation=True),
        limits=dict(seconds=SECONDS,rpc=RPC_CAP,bytes=CAP,files=FILE_CAP),scope='Actual unchanged CoreCLR FPS spring relaxation and native background-player adapter, owned frozen GPU captures and complete paused native archives. No physical input/shots/audio/FPS/latency/scan-out/AOT/fresh-agent claim.')
    def cap():
        files=[p for p in args.output.rglob('*')if p.is_file()]
        need(len(files)<=FILE_CAP and sum(p.stat().st_size for p in files)+len(encoded(record))<=CAP,'Observed64MiB/512file output cap')
        need(all(not(args.output/name).is_file()or(args.output/name).stat().st_size<=8*1048576 for name in('host.stdout.bin','host.stderr.bin')),'Observed original host raw stream8MiB bound')
    def rpc(method,params=None,error=None):
        need(len(record['calls'])<RPC_CAP and time.monotonic()<deadline,'149RPC/180s original bound')
        row=dict(method=method,params=copy.deepcopy(params or {}));record['calls'].append(row)
        try:value=client.call(method,params or {},timeout=min(30,max(.001,deadline-time.monotonic())))
        except RpcError as e:
            row['error']=dict(code=e.code,message=e.message,data=e.data)
            need(time.monotonic()<deadline,'Original rejected RPC exceeded whole deadline');cap()
            need(error is not None and e.code==error,'Unexpected native rejection: '+str(row['error']));return row['error']
        except BaseException as e:row['transport_error']=repr(e);raise
        need(error is None,'Expected guarded native rejection');row['result']=value
        need(time.monotonic()<deadline,'Original RPC exceeded whole deadline');cap();return value
    def inspect_player():
        p=rpc('player.inspect');need(p['active']and p['mode']=='background','Original window owner finished unexpectedly')
        if p['report']is not None:need(p['report']['frames_presented']<=60,'Native frame cap exceeded')
        return p
    def control(action):
        nonlocal player
        info=inspect_player();value=rpc('player.control',dict(player_id=info['player_id'],request_id=uid(),expected_control_revision=info['control_revision'],action=action));player=value;return value
    def validate_values(values):
        need(type(values)is dict and set(values)==set(fields),'All59 original saved fields required')
        for name,kind in fields.items():
            v=values[name]
            if kind=='float64':need(type(v)is float and math.isfinite(v),'Original float64 field differs: '+name)
            elif kind=='int32':need(type(v)is int and -(2**31)<=v<2**31,'Original int32 field differs: '+name)
            else:need(kind=='int64'and type(v)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',v)and -(2**63)<=int(v)<2**63,'Original canonical int64 field differs: '+name)
        return values
    def checkpoint(label):
        state=rpc('runtime.inspect',dict(session_id=session));game=rpc('runtime.gameplay.inspect',dict(session_id=session));validate_values(game['module']['values'])
        receipt=rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,session_id=session,expected_tick=state['tick'],expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        selected=read_checkpoint(saves,label,receipt);current=json.loads((saves/('slot-'+label)/'current.json').read_bytes())['payload']['current'];path=saves/('slot-'+label)/current['file'];raw=path.read_bytes()
        record['checkpoints'].append(dict(label=label,path=str(path.relative_to(args.output)),identity=pin(path),complete=selected));return raw
    def capture(label,running,native=None):
        info=inspect_player();need(info['ready']and info['paused']is(not running),'Window readiness/pause phase differs')
        path=args.output/'captures'/(label+'.bmp');request=dict(player_id=info['player_id'],request_id=uid(),expected_control_revision=info['control_revision'],session_id=session,expected_material_revision=0,path=str(path))
        result=rpc('player.capture_current',request);entry=dict(label=label,request=request,result=result);record['captures'].append(entry)
        image=result['capture'];need(image['build_version']=='0.0.207'and image['hardware']is True and image['capture_written']is True and image['nvrhi_errors']==0,'Actual207 hardware capture required')
        need(image['width']==640 and image['height']==360 and image['samples']==4 and image['frames_presented']<=60,'Exact small capture recipe/frame cap')
        d=image['render_diagnostics'];need(d['lighting_path']=='forward'and d['viewmodel']['rendered']is True and d['viewmodel']['samples']==4,'Actual MSAA4 world/foreground rendering required')
        need(d['scene_products']['available']is False,'MSAA4 RGB capture must not claim single-sample scene products')
        entry['verification']=verify_presentation(image,foreground_local,running=running,native=native)
        need(result['captured_tick']==image['pose_presentation']['target_tick'],'Captured Tick differs from owned bracket')
        raw=path.read_bytes();need(len(raw)>54 and raw[:2]==b'BM'and struct.unpack_from('<i',raw,18)[0]==640 and abs(struct.unpack_from('<i',raw,22)[0])==360,'Actual640x360 BMP artifact required')
        entry['image']=dict(path=str(path.relative_to(args.output)),identity=pin(path));return entry
    try:
        record['preparation']=prepare.prepare(args.output/'project');world=args.output/'project/world.json'
        authored=json.loads(world.read_bytes());fore=authored['entities'][FORE];need(fore['parent']==CAM and fore['components']['CameraDraw']==dict(camera=CAM,mode='viewmodel'),'Unchanged source foreground binding required')
        foreground_local=fore['components']['Transform']
        # The shared headless host pumps native player work between requests.
        # World stdio deliberately does not own this running-window service.
        endpoint='fps-pose-'+uid();command=[str(args.binary),'serve',str(world),'--endpoint',endpoint]
        record['host']=dict(command=command,endpoint=endpoint,closed=False,exit_code=None,cleanup_actions=[],scope='Directly launched shared headless native owner; full tree belongs to external Windows Job.')
        with(args.output/'host.stdout.bin').open('xb')as stdout,(args.output/'host.stderr.bin').open('xb')as stderr:
            host=subprocess.Popen(command,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,shell=False)
        record['host']['pid']=host.pid
        client=WorldClient.connect(str(args.binary),endpoint,cwd=ROOT,timeout_ms=20000,
            trace_directory=str(args.output/'stdio'),trace_limit_bytes=8*1048576,close_timeout=5)
        process=client.transport._process
        for action in('terminate','kill'):
            original=getattr(process,action)
            def observed(*a,_action=action,_original=original,**kw):cleanup.append(dict(action=_action,monotonic=time.monotonic()));return _original(*a,**kw)
            setattr(process,action,observed)
        revision=rpc('world.inspect')['revision']
        element=dict(parent=f'{500:032x}',name='Feedback profile',kind='button',text='Feedback: Hybrid',action='feedback-mode',visible=True,enabled=True,
            layout=dict(position='absolute',left=dict(unit='dp',value=320),top=dict(unit='dp',value=145),width=dict(unit='dp',value=280),height=dict(unit='dp',value=42)),
            style=dict(background_color='#722f37',color='#f7e8eb',font_size=18,border_color='#722f37',border_width=1,focus=dict(border_color='#d46a7e')))
        revision=rpc('world.transact',dict(request_id=uid(),base_revision=revision,ops=[dict(op='ui.element.set',id=f'{510:032x}',element=element)]))['revision'];original_world=pin(world)
        rpc('runtime.start',dict(session_id=session,revision=revision))
        loaded=rpc('runtime.gameplay.load',dict(session_id=session,request_id=uid(),expected_tick=0,expected_revision=0,assembly=str(args.assembly),bridge=str(args.bridge),hostfxr=str(args.hostfxr),type='Poima.Examples.FpsFeedbackGame'))
        module=loaded['module'];need(module['backend']=='coreclr'and module['assembly_sha256']==pin(args.assembly)['sha256']and module['schema']['identity']=='poima.example.fps-feedback','Exact selected compiled sample identity/image required')
        fields={r['name']:r['kind']for r in module['schema']['fields']};need(len(fields)==len(module['schema']['fields'])==59,'All59 distinct primitive fields required');validate_values(module['values'])
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)))
        rpc('runtime.gameplay.edit',dict(session_id=session,request_id=uid(),expected_tick=0,expected_revision=loaded['revision'],expected_structure_revision=0,values=SEED))
        stepped=rpc('runtime.step',dict(session_id=session,request_id=uid(),expected_tick=0,ticks=1));tick=stepped['current_tick']
        prior=rpc('player.inspect');player=rpc('player.start',dict(session_id=session,request_id=uid(),expected_tick=tick,expected_generation=prior['generation'],camera=CAM,controller=f'{2:032x}',mode='background',initially_paused=True,
            width=640,height=360,gpu=args.gpu,samples=4,viewmodel_samples=4,lighting_path='forward',frames_in_flight=2,max_frames=60,audio=False,gamepad=dict(mode='disabled'),pose_interpolation=True,lens_interpolation=True))
        for attempt in range(12):
            info=inspect_player()
            if info['ready']:break
            time.sleep(.005)
        need(info['ready']and info['paused'],'Paused background window never became ready')
        control('resume')
        for attempt in range(16):
            info=inspect_player();shown=info['report']['materials']['presented']if info['report']is not None else None
            p=shown.get('pose_presentation')if shown else None
            if p and p['interpolating']and 0<p['alpha']<1 and p['previous_tick']+1==p['target_tick']and not same(p['previous_world'],p['target_world'])and not same(p['previous_viewmodel'],p['target_viewmodel']):break
            time.sleep(.005)
        need(p and p['interpolating']and 0<p['alpha']<1,'No changing fractional compiled observation in16polls')
        first=capture('running-a',True)
        for attempt in range(12):
            info=inspect_player()
            if info['tick']>first['result']['captured_tick']:break
            time.sleep(.005)
        need(info['tick']>first['result']['captured_tick'],'Native clock did not advance after first frozen capture')
        replay=rpc('player.capture_current',first['request']);need(same(replay,{**first['result'],'replayed':True}),'Retry after later Tick changed original complete capture receipt');first['retry_after_advance']=dict(observed_tick=info['tick'],result=replay)
        capture('running-b',True)
        control('pause');info=inspect_player();tick=info['tick'];need(info['paused'],'Explicit pause did not commit')
        native=rpc('runtime.camera_pose',dict(session_id=session,tick=tick,entity=CAM))['state'];camera=rpc('runtime.entity',dict(session_id=session,tick=tick,id=CAM));fore_state=rpc('runtime.entity',dict(session_id=session,tick=tick,id=FORE))
        close(native['base_world_matrix'],multiply(rpc('runtime.entity',dict(session_id=session,tick=tick,id=f'{2:032x}'))['world_matrix'],local(camera['local_transform'])),'Current BaseWorld differs from native actor/look/stance frame')
        close(camera['world_matrix'],multiply(native['base_world_matrix'],local(native['world'])),'Current native camera is not committed base plus world offset')
        close(fore_state['world_matrix'],multiply(camera['world_matrix'],local(foreground_local)),'VM effect changed logical foreground transform')
        game=rpc('runtime.gameplay.inspect',dict(session_id=session));values=validate_values(game['module']['values']);n=int(values['FeedbackTicks']);need(n>=2 and values['Phase']==1 and values['MenuOpen']==0,'Actual active compiled relaxation did not run')
        for name,hz,initial in(('AimX',6,.18),('WeaponX',8,.24),('WeaponBack',8,.06),('ShakeEnvelope',3,.8)):
            t=n/60;omega=2*math.pi*hz;expected=initial*(1+omega*t)*math.exp(-omega*t);close([values[name]],[expected],'Actual saved compiled spring differs from analytic relaxation: '+name)
        record['paused_native']=dict(tick=tick,pose=native,camera=camera,foreground=fore_state,game=game)
        before=checkpoint('pose-before')
        rejected=args.output/'captures/rejected-stale.bmp';guard=dict(player_id=info['player_id'],request_id=uid(),expected_control_revision=info['control_revision'],session_id=session,tick=tick+1,path=str(rejected))
        rpc('player.capture',guard,error=-32009);need(not rejected.exists(),'Stale Tick guard created an image')
        forbidden=args.output/'captures/rejected-tick-current.bmp';rpc('player.capture_current',{**guard,'request_id':uid(),'tick':tick,'path':str(forbidden)},error=-32602);need(not forbidden.exists(),'Forbidden atomic-current Tick created an image')
        capture('paused-a',False,native);after=checkpoint('pose-after-a');need(before==after,'Paused capture/guards changed complete original native save bytes');record['whole_pairs'].append(dict(before='pose-before',after='pose-after-a',bytes_equal=True))
        capture('paused-b',False,native);last=checkpoint('pose-after-b');need(after==last,'Second paused capture changed complete original native save bytes');record['whole_pairs'].append(dict(before='pose-after-a',after='pose-after-b',bytes_equal=True))
        stopped=control('stop');record['player_stop']=stopped;player=None
        final=rpc('player.inspect');record['final_player']=final;need(not final['active']and final['report']['frames_presented']<=60,'Original native window did not stop within60presented frames')
        need(pin(world)==original_world and prepare.inputs()==basis,'Authored copy or preserved source basis changed during presentation')
        need(len(record['captures'])==4,'Exactly four actual captures required');record['passed']=True
    except BaseException:record['passed']=False;record['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            if player is not None and not client.closed:
                try:record['cleanup_player_stop']=control('stop')
                except BaseException:record['passed']=False;record['errors'].append('Player cleanup failed:\n'+traceback.format_exc())
            if not client.closed:
                try:
                    rpc('runtime.stop',dict(session_id=session))
                except BaseException:record['passed']=False;record['errors'].append('Shared runtime stop failed:\n'+traceback.format_exc())
                try:record['host']['shutdown']=rpc('host.shutdown')
                except BaseException:record['passed']=False;record['errors'].append('Shared host shutdown failed:\n'+traceback.format_exc())
            try:client.close()
            except BaseException:record['passed']=False;record['errors'].append('SDK close failed:\n'+traceback.format_exc())
            try:
                record['native_owner']=dict(pid=client.transport.process_id,closed=client.closed,exit_code=client.transport.returncode,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),cleanup_actions=cleanup,stderr=client.transport.stderr_tail,stderr_truncated=client.transport.stderr_truncated,role='shared_connector',scope='Direct SDK connector lifetime only; shared host is recorded separately and external Job proves complete process-tree closure.')
                need(client.closed and client.transport.returncode==0 and client.cleanup_error is None and not cleanup and not client.transport.stderr_truncated,'Original native owner did not close naturally/without truncation')
            except BaseException:record['passed']=False;record['errors'].append('Terminal owner metadata failed:\n'+traceback.format_exc())
        if host is not None:
            try:
                host.wait(timeout=min(10,max(.001,deadline-time.monotonic())))
                record['host'].update(closed=True,exit_code=host.returncode)
                need(host.returncode==0,'Shared native host did not close with exit0')
            except BaseException:
                record['passed']=False;record['host'].update(closed=host.poll()is not None,exit_code=host.returncode,known_live_pid=host.pid if host.poll()is None else None)
                record['errors'].append('Shared host natural closure failed; external Job retains ownership:\n'+traceback.format_exc())
            try:
                record['host']['streams']={name:pin(args.output/name)for name in('host.stdout.bin','host.stderr.bin')}
                need(all(row['bytes']<=8*1048576 for row in record['host']['streams'].values()),'Original host raw stream8MiB bound')
            except BaseException:record['passed']=False;record['errors'].append('Shared host raw-stream metadata failed:\n'+traceback.format_exc())
        record['seconds']=time.monotonic()-started
        try:
            record['changed_sources']=[p for p,v in pins.items()if not Path(p).is_file()or pin(p)!=v]
            need(not record['changed_sources']and record['seconds']<=SECONDS,'Original source/whole deadline fence failed');cap()
        except BaseException:record['passed']=False;record['errors'].append('Terminal input/output fence failed:\n'+traceback.format_exc())
        try:(args.output/'evidence.json').write_bytes(encoded(record)+b'\n')
        except BaseException:
            print(json.dumps(dict(original_errors=record['errors'],evidence_publication_error=traceback.format_exc())),file=sys.stderr);raise
    print(json.dumps(dict(passed=record['passed'],rpc=len(record['calls']),captures=len(record['captures']),whole_pairs=len(record['whole_pairs']),seconds=record['seconds'],evidence=str(args.output/'evidence.json'))))
    return 0 if record['passed']else 1


if __name__=='__main__':raise SystemExit(main())
