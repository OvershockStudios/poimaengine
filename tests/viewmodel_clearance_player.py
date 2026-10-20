#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Owned Windows Vulkan/CoreCLR weapon-envelope clearance; no build or input.

Consumes the unchanged FPS Feedback game and an explicitly selected208 runtime.
The external Windows Job owns the shared headless host and SDK connector. Four
small captures cover changing interpolation, a front wall, a volume-only edge
obstacle and blocked-anchor suppression. Original calls and full saves survive.
"""
import argparse
import copy
import importlib.util
import itertools
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

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'))
from fps_pose_player import need,encoded,same,uid,pin,vector,close,multiply,local,slerp_oracle,check_pose,require_job
from poima_client import WorldClient, RpcError
from switchback_oracle import read_checkpoint

CAM,FORE,ACTOR=(f'{n:032x}'for n in(3,305,2))
SLABS=[f'{n:032x}'for n in(910,911,912)]
PROFILE=dict(position=[.18,-.17,-.4],rotation=[math.sqrt(.5),0.,0.,math.sqrt(.5)],radius=.075,half_height=.175,max_retraction=.65,skin=.005)
SEED=dict(Phase=1,MenuOpen=0,AimX=.18,WeaponX=.24,WeaponBack=.06,ShakeEnvelope=.8)
CAP,FILE_CAP,RPC_CAP,SECONDS=64*1048576,1024,219,240
EXTENTS=[(.30,.25,.012),(.012,.25,.012),(.20,.25,.015)]
PARKING=[(10.+i,8.,10.)for i in range(3)]


def dot(a,b):return sum(x*y for x,y in zip(a,b))
def add(a,b):return [x+y for x,y in zip(a,b)]
def scale(v,s):return [x*s for x in v]
def point(m,p):return [sum(m[k*4+r]*p[k]for k in range(3))+m[12+r]for r in range(3)]


def quaternion(m):
    # Matrix-to-quaternion conversion for independently authored obstacle/query
    # orientation, not a call into native capsule or clearance implementation.
    tr=m[0]+m[5]+m[10]
    if tr>0:
        s=2*math.sqrt(tr+1);q=[(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,.25*s]
    elif m[0]>m[5]and m[0]>m[10]:
        s=2*math.sqrt(1+m[0]-m[5]-m[10]);q=[.25*s,(m[4]+m[1])/s,(m[8]+m[2])/s,(m[6]-m[9])/s]
    elif m[5]>m[10]:
        s=2*math.sqrt(1+m[5]-m[0]-m[10]);q=[(m[4]+m[1])/s,.25*s,(m[9]+m[6])/s,(m[8]-m[2])/s]
    else:
        s=2*math.sqrt(1+m[10]-m[0]-m[5]);q=[(m[8]+m[2])/s,(m[9]+m[6])/s,.25*s,(m[1]-m[4])/s]
    n=math.sqrt(dot(q,q));return [v/n for v in q]


def geometry(base,world,vm):
    camera=multiply(base,local(world));weapon=multiply(camera,local(vm));capsule=multiply(weapon,local(PROFILE))
    return dict(camera=camera,desired=capsule[12:15],axis=capsule[4:7],rotation=quaternion(capsule),
        back=camera[8:11],right=camera[:3],anchor=add(capsule[12:15],scale(camera[8:11],PROFILE['max_retraction'])))


def valid_hit(hit,distance):
    if hit is None:return
    need(type(hit)is dict and set(hit)=={'entity','fraction','distance','point_on_query','point_on_obstacle','normal','penetration_depth','triangle','initial_overlap'},'Complete capsule hit shape required')
    need(type(hit['entity'])is str and re.fullmatch('[0-9a-f]{32}',hit['entity']),'Native hit identity differs')
    for key in('fraction','distance','penetration_depth'):need(type(hit[key])is float and math.isfinite(hit[key])and hit[key]>=0,'Original finite hit double required')
    need(hit['fraction']<=1 and hit['distance']<=distance+1e-8 and type(hit['initial_overlap'])is bool,'Capsule fraction/distance/initial-overlap differs')
    for name in('point_on_query','point_on_obstacle'):vector(hit[name],3)
    if hit['normal']is not None:
        n=vector(hit['normal'],3);need(abs(dot(n,n)-1)<1e-4,'Native contact normal is not unit')
    need(hit['triangle']is None or type(hit['triangle'])is int and hit['triangle']>=0,'Original triangle type differs')


def verify(image,fore_local,expected,*,running,native=None,slab=None,half_thickness=None):
    diagnostics=image['render_diagnostics'];receipts=diagnostics['scene_receipts'];scene=receipts['presented']
    need(same(scene,receipts['submitted'])and scene['frame_sequence']>0 and scene['submission_id']>0,'Actual scene submission/presentation join differs')
    p=image['pose_presentation'];need(p['enabled']is True and type(p['alpha'])is float and 0<=p['alpha']<=1,'Explicit pose interpolation metadata required')
    if running:
        need(p['interpolating']is True and 0<p['alpha']<1 and p['previous_tick']+1==p['target_tick'],'No actual changing fractional pose')
        need(not same(p['previous_world'],p['target_world'])and not same(p['previous_viewmodel'],p['target_viewmodel']),'Both original compiled feedback channels must actually change')
    else:need(p['alpha']==1 and not p['interpolating']and p['previous_tick']==p['target_tick'],'Paused presentation did not reset')
    for channel in('world','viewmodel'):
        a,b,r=(p[k+'_'+channel]for k in('previous','target','rendered'))
        for value in(a,b,r):check_pose(value)
        close(r['position'],[x+(y-x)*p['alpha']for x,y in zip(a['position'],b['position'])],'Presented local translation differs')
        expected_q=slerp_oracle(a['rotation'],b['rotation'],p['alpha'])
        close(local(dict(position=[0.,0.,0.],rotation=r['rotation'])),local(dict(position=[0.,0.,0.],rotation=expected_q)),'Presented rotation differs from independent arc')
    g=geometry(vector(p['target_base_world'],16),p['rendered_world'],p['rendered_viewmodel'])
    close(vector(image['camera_world'],16),g['camera'],'Clearance changed rendered world camera')
    close(vector(scene['camera_pose']['camera_world'],16),g['camera'],'Renderer world camera differs')
    v=scene['viewmodel_clearance'];need(v['status']==expected and same(v['profile'],PROFILE),'Expected native clearance profile/status differs')
    need(type(v['resolved'])is bool and type(v['suppressed'])is bool and type(v['retraction'])is float and 0<=v['retraction']<=.65,'Original resolution/retraction types differ')
    need(v['ignored_entities']==[ACTOR]and v['cast']['ignore']==[ACTOR],'Exact camera physical ancestor exclusion differs')
    c=v['cast'];close(vector(c['position'],3),g['anchor'],'Cast anchor is not full retraction from actual presented candidate')
    close(vector(c['direction'],3),scale(g['back'],-1),'Cast direction differs from current rendered camera -Z')
    # The native physics-pose adapter records the actual float32 Jolt quaternion
    # as doubles. Compare its rotation at that explicit precision, without
    # rewriting the original cast receipt or relaxing camera/draw matrix checks.
    close(local(dict(position=[0.,0.,0.],rotation=c['rotation'])),local(dict(position=[0.,0.,0.],rotation=g['rotation'])),'Capsule fixed orientation differs',tolerance=5e-7)
    need(c['radius']==.075 and c['half_height']==.175 and c['distance']==.65,'Exact fixed capsule dimensions/travel differ')
    for name in('anchor_hit','sweep_hit','final_hit'):valid_hit(v[name],.65)
    nominal=multiply(multiply(g['camera'],local(p['rendered_viewmodel'])),local(fore_local))
    if expected in('clear','retracted'):
        need(v['resolved']is True and v['suppressed']is False and v['anchor_hit']is None and v['final_hit']is None and v['suppressed_entities']==[],'Resolved foreground must have clear anchor/final checks')
        need(scene['camera_draw']['viewmodels']==[dict(entity_id=FORE,incarnation=1)]and len(scene['camera_pose']['viewmodels'])==1,'Resolved foreground membership differs')
        draw=scene['camera_pose']['viewmodels'][0];need(draw['entity']==FORE and draw['incarnation']==1,'Resolved drawable identity differs')
        target=nominal[:]
        for i in range(3):target[12+i]+=g['back'][i]*v['retraction']
        close(vector(draw['world'],16),target,'Resolved foreground matrix is not whole-draw camera-local +Z retraction')
        if expected=='clear':need(v['retraction']==0.0 and v['sweep_hit']is None,'Clear case unexpectedly retracted')
        else:
            need(v['retraction']>0 and v['sweep_hit']is not None,'Obstruction did not retract')
            if slab is not None:
                # Source-derived convex plane support for this actual moved box,
                # independent of native capsule casts and reported contact points.
                n=slab[8:11];plane=add(slab[12:15],scale(n,half_thickness));support=.075+.175*abs(dot(g['axis'],n))
                gap=dot(n,[a-b for a,b in zip(g['desired'],plane)])-support
                needed=max(0,(.005-gap)/dot(n,g['back']))
                need(abs(v['retraction']-needed)<=5e-5,'Actual front-wall retraction differs from independent capsule-plane support')
    else:
        need(expected=='blocked_anchor'and v['resolved']is False and v['suppressed']is True and v['anchor_hit']is not None and v['anchor_hit']['initial_overlap']is True,'Anchor obstruction must suppress unresolved foreground')
        need(v['suppressed_entities']==[FORE]and scene['camera_draw']['viewmodels']==[]and scene['camera_pose']['viewmodels']==[],'Suppressed foreground reappeared in render membership')
        need(diagnostics['viewmodel']['draws']['camera_draws']==0,'Suppressed foreground still submitted a camera draw')
    if native is not None:
        need(same(p['target_world'],native['world'])and same(p['target_viewmodel'],native['viewmodel'])and same(p['target_base_world'],native['base_world_matrix']),'Safety changed committed camera/feedback/BaseWorld')
    need(same(scene['camera_lens'],image['lens'])and same(scene['camera_draw']['viewmodel_lens'],image['lens_presentation']['rendered_viewmodel']),'Clearance changed independent lens selection')
    return dict(geometry=g,receipt=v,nominal_foreground=nominal)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in('binary','hostfxr','bridge','assembly','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,required=True);args=parser.parse_args()
    need(os.name=='nt'and not sys.flags.optimize,'Native Windows Python required');require_job()
    for name in('binary','hostfxr','bridge','assembly'):setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.absolute();need(args.gpu>=0 and not args.output.exists()and args.output.parent.is_dir(),'Fresh owned output and explicit GPU required')
    sample=ROOT/'examples/fps-feedback';spec=importlib.util.spec_from_file_location('clearance_prepare',sample/'prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    sources=[Path(__file__),ROOT/'tests/fps_pose_player.py',ROOT/'tests/switchback_oracle.py',args.binary,args.hostfxr,args.bridge,args.assembly]
    sources += [p for p in sample.iterdir()if p.is_file()and p.suffix in('.cs','.csproj','.py')]
    sources += [p for p in(ROOT/'tools/python/poima_client').iterdir()if p.is_file()and p.suffix in('.py','.json')]
    for image in(args.bridge,args.assembly):
        sources.append(image.parent/'Poima.Gameplay.dll')
        for suffix in('.deps.json','.runtimeconfig.json'):
            if image.with_suffix(suffix).is_file():sources.append(image.with_suffix(suffix))
    if(args.binary.parent/'phonon.dll').is_file():sources.append(args.binary.parent/'phonon.dll')
    pins={str(p):pin(p)for p in set(sources)};basis=prepare.inputs();args.output.mkdir();(args.output/'captures').mkdir();saves=args.output/'saves';saves.mkdir()
    deadline=time.monotonic()+SECONDS;started=time.monotonic();client=host=None;player=None;session=uid();tick=0;fields=None;cleanup=[]
    record=dict(format='poima.viewmodel-clearance-player.v1',passed=False,errors=[],calls=[],captures=[],checkpoints=[],whole_pairs=[],source_pins=pins,
        profile=PROFILE,seed=SEED,limits=dict(seconds=240,rpc=219,frames=120,captures=4,bytes=CAP,files=FILE_CAP),scope='Selected rigid greybox envelope with unchanged CoreCLR FPS game and native Vulkan post-interpolation safety. No physical input/shots/imported-weapon/art/AOT/FPS/latency claim.')
    def cap():
        files=[p for p in args.output.rglob('*')if p.is_file()]
        need(len(files)<=FILE_CAP and sum(p.stat().st_size for p in files)+len(encoded(record))<=CAP,'Observed64MiB/1024file bound')
        need(all(not(args.output/n).is_file()or(args.output/n).stat().st_size<=8*1048576 for n in('host.stdout.bin','host.stderr.bin')),'Observed original host log8MiB bound')
    def rpc(method,params=None,error=None):
        need(len(record['calls'])<RPC_CAP and time.monotonic()<deadline,'Original219RPC/240s bound')
        row=dict(method=method,params=copy.deepcopy(params or {}));record['calls'].append(row)
        try:r=client.call(method,params or {},timeout=min(30,max(.001,deadline-time.monotonic())))
        except RpcError as e:
            row['error']=dict(code=e.code,message=e.message,data=e.data);need(error is not None and e.code==error,'Unexpected native rejection: '+str(row['error']));cap();return row['error']
        except BaseException as e:row['transport_error']=repr(e);raise
        row['result']=r;need(error is None and time.monotonic()<deadline,'Missing rejection/late reply');cap();return r
    def pinfo():
        r=rpc('player.inspect');need(r['active']and r['mode']=='background','Original background window ended unexpectedly')
        if r['report']is not None:need(r['report']['frames_presented']<=120,'Native120frame cap exceeded')
        return r
    def control(action):
        nonlocal player
        p=pinfo();player=rpc('player.control',dict(player_id=p['player_id'],request_id=uid(),expected_control_revision=p['control_revision'],action=action));return player
    def step(ticks=1,motions=None):
        nonlocal tick
        req=dict(session_id=session,request_id=uid(),expected_tick=tick,ticks=ticks)
        if motions is not None:req['motions']=motions
        r=rpc('runtime.step',req);tick=r['current_tick'];return r
    def native_pose():return rpc('runtime.camera_pose',dict(session_id=session,tick=tick,entity=CAM))['state']
    def entity(id):return rpc('runtime.entity',dict(session_id=session,tick=tick,id=id))
    def values(v):
        need(type(v)is dict and set(v)==set(fields),'All59 original fields required')
        for name,kind in fields.items():
            x=v[name]
            if kind=='float64':need(type(x)is float and math.isfinite(x),'Original float64 field differs')
            elif kind=='int32':need(type(x)is int and -(2**31)<=x<2**31,'Original int32 field differs')
            else:need(kind=='int64'and type(x)is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',x)and -(2**63)<=int(x)<2**63,'Original int64 field differs')
    def checkpoint(label):
        state=rpc('runtime.inspect',dict(session_id=session));game=rpc('runtime.gameplay.inspect',dict(session_id=session));values(game['module']['values'])
        need(state['tick']==tick,'Paused save boundary advanced')
        receipt=rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,session_id=session,expected_tick=tick,expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        saved=read_checkpoint(saves,label,receipt);values(saved['snapshot']['payload']['gameplay']['values'])
        current=json.loads((saves/('slot-'+label)/'current.json').read_bytes())['payload']['current'];path=saves/('slot-'+label)/current['file']
        record['checkpoints'].append(dict(label=label,path=str(path.relative_to(args.output)),identity=pin(path),complete=saved));return path.read_bytes()
    def query(cast):
        r=rpc('runtime.capsule_cast',dict(session_id=session,tick=tick,**cast));need(r['source']=='runtime'and r['tick']==tick and r['session_id']==session,'Public capsule query boundary differs');valid_hit(r['hit'],cast['distance']);return r['hit']
    def capture(label,status,running=False,native=None,slab=None):
        p=pinfo();need(p['ready']and p['paused']is(not running),'Actual window phase differs')
        path=args.output/'captures'/(label+'.bmp');result=rpc('player.capture_current',dict(player_id=p['player_id'],request_id=uid(),expected_control_revision=p['control_revision'],session_id=session,expected_material_revision=0,path=str(path)))
        image=result['capture'];entry=dict(label=label,result=result);record['captures'].append(entry)
        need(image['build_version']=='0.0.208'and image['hardware']is True and image['capture_written']is True and image['nvrhi_errors']==0,'Actual208 hardware capture required')
        need(image['width']==640 and image['height']==360 and image['samples']==4 and image['frames_presented']<=120,'Exact small MSAA4 capture/frame recipe differs')
        need(image['render_diagnostics']['lighting_path']=='forward'and image['render_diagnostics']['viewmodel']['samples']==4,'Foreground MSAA/forward policy differs')
        entry['verification']=verify(image,fore_local,status,running=running,native=native,slab=slab,half_thickness=.012 if slab is not None else None)
        raw=path.read_bytes();need(raw[:2]==b'BM'and struct.unpack_from('<i',raw,18)[0]==640 and abs(struct.unpack_from('<i',raw,22)[0])==360,'Actual640x360 BMP missing')
        entry['image']=dict(path=str(path.relative_to(args.output)),identity=pin(path));return entry
    try:
        record['preparation']=prepare.prepare(args.output/'project');world=args.output/'project/world.json';doc=json.loads(world.read_bytes());fore_local=doc['entities'][FORE]['components']['Transform']
        need(doc['entities'][FORE]['parent']==CAM,'Original direct weapon-camera binding required')
        corners=[]
        for v in itertools.product((-.045,.045),(-.055,.055),(-.175,.175)):
            d=math.sqrt(v[0]*v[0]+v[1]*v[1]+max(0,abs(v[2])-.175)**2);need(d<=.075,'Original box outside configured proxy');corners.append(dict(local=list(v),distance_to_capsule_axis_segment=d))
        record['envelope_corners']=corners
        endpoint='vm-clear-'+uid();command=[str(args.binary),'serve',str(world),'--endpoint',endpoint]
        with(args.output/'host.stdout.bin').open('xb')as out,(args.output/'host.stderr.bin').open('xb')as err:host=subprocess.Popen(command,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=out,stderr=err,shell=False)
        record['host']=dict(pid=host.pid,command=command,closed=False,exit_code=None,cleanup_actions=[])
        client=WorldClient.connect(str(args.binary),endpoint,cwd=ROOT,timeout_ms=20000,close_timeout=5,trace_directory=str(args.output/'stdio'),trace_limit_bytes=8*1048576)
        for action in('terminate','kill'):
            original=getattr(client.transport._process,action)
            def observed(*a,_action=action,_original=original,**kw):cleanup.append(dict(action=_action,monotonic=time.monotonic()));return _original(*a,**kw)
            setattr(client.transport._process,action,observed)
        revision=rpc('world.inspect')['revision'];camera=copy.deepcopy(doc['entities'][CAM]['components']['Camera']);camera['viewmodel_clearance']=PROFILE
        # Same native authoring semantics as configure.py, with three separately
        # owned real collider slabs parked away from the preserved range.
        element=dict(parent=f'{500:032x}',name='Feedback profile',kind='button',text='Feedback: Hybrid',action='feedback-mode',visible=True,enabled=True,
            layout=dict(position='absolute',left=dict(unit='dp',value=320),top=dict(unit='dp',value=145),width=dict(unit='dp',value=280),height=dict(unit='dp',value=42)),style=dict(background_color='#722f37',color='#f7e8eb',font_size=18,border_color='#722f37',border_width=1,focus=dict(border_color='#d46a7e')))
        ops=[dict(op='component.set',id=CAM,type='Camera',value=camera),dict(op='ui.element.set',id=f'{510:032x}',element=element)]
        for id,pos,half in zip(SLABS,PARKING,EXTENTS):
            visual=f'{int(id,16)+1000:032x}'
            ops += [dict(op='entity.create',id=id,name='Clearance probe '+id,parent=None),dict(op='component.set',id=id,type='Transform',value=dict(position=list(pos),rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])),
                dict(op='component.set',id=id,type='BoxCollider',value=dict(half_extents=list(half),motion='kinematic',mass=1.,friction=.5,restitution=0.)),
                dict(op='entity.create',id=visual,name='Matching collider visual '+id,parent=id),
                dict(op='component.set',id=visual,type='Transform',value=dict(position=[0.,0.,0.],rotation=[0.,0.,0.,1.],scale=[2*x for x in half])),
                dict(op='component.set',id=visual,type='MeshRenderer',value=dict(primitive='box',visible=True,albedo=[.45,.12,.16]))]
        revision=rpc('world.transact',dict(request_id=uid(),base_revision=revision,ops=ops))['revision'];original_world=pin(world)
        rpc('runtime.start',dict(session_id=session,revision=revision));loaded=rpc('runtime.gameplay.load',dict(session_id=session,request_id=uid(),expected_tick=0,expected_revision=0,hostfxr=str(args.hostfxr),bridge=str(args.bridge),assembly=str(args.assembly),type='Poima.Examples.FpsFeedbackGame'))
        module=loaded['module'];need(module['backend']=='coreclr'and module['assembly_sha256']==pin(args.assembly)['sha256']and module['schema']['identity']=='poima.example.fps-feedback','Exact original compiled FPS image required')
        fields={x['name']:x['kind']for x in module['schema']['fields']};need(len(fields)==len(module['schema']['fields'])==59,'All59 primitive fields required');values(module['values'])
        rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)));step(60)
        game=rpc('runtime.gameplay.inspect',dict(session_id=session));rpc('runtime.gameplay.edit',dict(session_id=session,request_id=uid(),expected_tick=tick,expected_revision=game['revision'],expected_structure_revision=0,values=SEED));step()
        prior=rpc('player.inspect');player=rpc('player.start',dict(session_id=session,request_id=uid(),expected_tick=tick,expected_generation=prior['generation'],camera=CAM,controller=ACTOR,mode='background',initially_paused=True,width=640,height=360,gpu=args.gpu,samples=4,viewmodel_samples=4,lighting_path='forward',frames_in_flight=2,max_frames=120,audio=False,gamepad=dict(mode='disabled'),pose_interpolation=True,lens_interpolation=True))
        for i in range(12):
            p=pinfo()
            if p['ready']:break
            time.sleep(.005)
        need(p['ready'],'Actual background window never ready');control('resume')
        for i in range(16):
            p=pinfo();shown=p['report']['materials']['presented']if p['report']is not None else None;interp=shown.get('pose_presentation')if shown else None
            if interp and interp['interpolating']and 0<interp['alpha']<1:break
            time.sleep(.005)
        need(interp and interp['interpolating']and 0<interp['alpha']<1,'No changing compiled fractional pair');capture('clear-fractional','clear',True)
        control('pause');tick=pinfo()['tick'];game=rpc('runtime.gameplay.inspect',dict(session_id=session));rpc('runtime.gameplay.edit',dict(session_id=session,request_id=uid(),expected_tick=tick,expected_revision=game['revision'],expected_structure_revision=0,values=dict(Phase=0,MenuOpen=1)));step()
        # Freeze sample feedback only through its public Phase/Menu fields. Every
        # original operand remains saved; no source, archive or response is fixed.
        pose=native_pose();g=geometry(pose['base_world_matrix'],pose['world'],pose['viewmodel']);D=g['back'];R=g['right'];axis=g['axis'];sign=1 if dot(axis,D)>=0 else -1
        support=.075+.175*abs(dot(axis,D));front=add(g['desired'],scale(D,-(support-.04)))
        endpoint_front=add(g['desired'],scale(axis,-sign*.175));edge=add(add(endpoint_front,scale(R,.06)),scale(D,-.03))
        endpoint_back=add(g['anchor'],scale(axis,sign*.175));anchor=add(endpoint_back,scale(D,.045))
        placements=[front,edge,anchor];rotation=quaternion(g['camera']);record['placement_recipe']=dict(observed_pose=pose,geometry=g,front=front,edge=edge,anchor=anchor,slab_rotation=rotation)
        for index,label in enumerate(('front-wall','volume-edge','blocked-anchor')):
            motions=[dict(entity=id,position=placements[j]if j==index else list(PARKING[j]),rotation=rotation if j==index else[0.,0.,0.,1.],duration_ticks=30)for j,id in enumerate(SLABS)]
            step(30,motions);p=pinfo();need(p['paused']and p['tick']==tick,'Kinematic case did not retain paused native boundary')
            native=native_pose();fore_before=entity(FORE);before=checkpoint(label+'-before')
            ng=geometry(native['base_world_matrix'],native['world'],native['viewmodel'])
            cast=dict(position=ng['desired'],rotation=ng['rotation'],radius=.075,half_height=.175,direction=scale(ng['back'],-1),distance=0.0,ignore=[ACTOR])
            if index<2:
                hit=query(cast);need(hit is not None and hit['entity']==SLABS[index]and hit['initial_overlap']is True,'Real nominal capsule did not overlap selected obstacle')
            else:
                hit=query({**cast,'position':ng['anchor']});need(hit is not None and hit['entity']==SLABS[index]and hit['initial_overlap']is True,'Real anchor capsule did not overlap selected blocker')
            if index==1:
                ray=rpc('runtime.raycast',dict(session_id=session,tick=tick,origin=ng['camera'][12:15],direction=scale(ng['back'],-1),distance=1.2,ignore=[ACTOR]));need(ray['hit']is None,'Volume-edge witness unexpectedly hit centre ray');record['centre_ray_miss']=dict(ray=ray,capsule_hit=hit)
            slab=entity(SLABS[index])['world_matrix']if index==0 else None
            entry=capture(label,'blocked_anchor'if index==2 else'retracted',native=native,slab=slab)
            receipt=entry['verification']['receipt'];anchor_query=query({**receipt['cast'],'distance':0.0});need(same(anchor_query,receipt['anchor_hit']),'Independent public initial-overlap query differs from rendered safety receipt')
            if receipt['resolved']:
                need(same(query(receipt['cast']),receipt['sweep_hit']),'Independent public sweep differs from safety receipt')
                final_position=add(entry['verification']['geometry']['desired'],scale(entry['verification']['geometry']['back'],receipt['retraction']))
                need(query({**receipt['cast'],'position':final_position,'distance':0.0})is None,'Resolved final capsule remains obstructed')
            need(same(native_pose(),native)and same(entity(FORE),fore_before),'Clearance query/render changed committed camera/weapon state')
            after=checkpoint(label+'-after');need(before==after,'Query/capture changed complete original native save bytes');record['whole_pairs'].append(dict(before=label+'-before',after=label+'-after',bytes_equal=True,exclusions=[]))
        record['player_stop']=control('stop');player=None;record['final_player']=rpc('player.inspect');need(not record['final_player']['active']and record['final_player']['report']['frames_presented']<=120,'Original native window failed to stop within120frames')
        need(pin(world)==original_world and prepare.inputs()==basis,'Authored world or original source basis changed');need(len(record['captures'])==4,'Four actual captures required');record['passed']=True
    except BaseException:record['passed']=False;record['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            if player is not None and not client.closed:
                try:record['cleanup_player_stop']=control('stop')
                except BaseException:record['passed']=False;record['errors'].append('Player stop failed:\n'+traceback.format_exc())
            if not client.closed:
                for method,params in(('runtime.stop',dict(session_id=session)),('host.shutdown',{})):
                    try:r=rpc(method,params);record['host']['shutdown'if method=='host.shutdown'else'runtime_stop']=r
                    except BaseException:record['passed']=False;record['errors'].append('Shutdown failed:\n'+traceback.format_exc())
            try:client.close()
            except BaseException:record['passed']=False;record['errors'].append('Connector close failed:\n'+traceback.format_exc())
            try:
                record['native_owner']=dict(pid=client.transport.process_id,closed=client.closed,exit_code=client.transport.returncode,cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error),cleanup_actions=cleanup,stderr=client.transport.stderr_tail,stderr_truncated=client.transport.stderr_truncated,role='shared_connector')
                need(client.closed and client.transport.returncode==0 and client.cleanup_error is None and not cleanup and not client.transport.stderr_truncated,'Connector did not close naturally/losslessly')
            except BaseException:record['passed']=False;record['errors'].append('Connector metadata failed:\n'+traceback.format_exc())
        if host is not None:
            try:host.wait(timeout=min(10,max(.001,deadline-time.monotonic())));record['host'].update(closed=True,exit_code=host.returncode);need(host.returncode==0,'Host nonzero exit')
            except BaseException:record['passed']=False;record['host'].update(closed=host.poll()is not None,exit_code=host.returncode,known_live_pid=host.pid if host.poll()is None else None);record['errors'].append('Host closure failed; external Job retains ownership:\n'+traceback.format_exc())
            try:record['host']['streams']={n:pin(args.output/n)for n in('host.stdout.bin','host.stderr.bin')}
            except BaseException:record['passed']=False;record['errors'].append(traceback.format_exc())
        record['seconds']=time.monotonic()-started
        try:record['changed_sources']=[p for p,v in pins.items()if not Path(p).is_file()or pin(p)!=v];need(not record['changed_sources']and record['seconds']<=SECONDS,'Source/whole deadline fence');cap()
        except BaseException:record['passed']=False;record['errors'].append('Terminal input/output fence failed:\n'+traceback.format_exc())
        try:(args.output/'evidence.json').write_bytes(encoded(record)+b'\n')
        except BaseException:print(json.dumps(dict(original_errors=record['errors'],publication_error=traceback.format_exc())),file=sys.stderr);raise
    print(json.dumps(dict(passed=record['passed'],rpc=len(record['calls']),captures=len(record['captures']),whole_pairs=len(record['whole_pairs']),seconds=record['seconds'])));return 0 if record['passed']else 1


if __name__=='__main__':raise SystemExit(main())
