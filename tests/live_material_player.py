#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual one-window material adoption; run only inside a root-owned Windows Job.

Native Windows Python: live_material_player.py --binary ABSOLUTE_POIMA_EXE
--output NEW_DIRECTORY --lighting-path forward --gpu 0. Whole bound 300 seconds.
Two immutable authored target BMPs are captured before player.start because an
attached player exclusively owns graphics. No focus/input injection, build,
managed gameplay, latency target, scanout or GPU-completion claim is made.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback
import uuid

ROOT = next(p for p in Path(__file__).resolve().parents if (p/'tools/python/poima_client').is_dir())
sys.path.insert(0, str(ROOT/'tools/python'))
sys.path.insert(0, str(ROOT/'tests'))
from poima_client import WorldClient, RpcError
from scene_capture import pixels

CAP = 64*1048576
WIDTH, HEIGHT = 320, 240
BOX, CAMERA, LIGHT, ENV = [f'{n:032x}' for n in (1,2,3,4)]
BASE = dict(base_color=[.625,.0625,.03125],emissive=[.015625,0.,0.],metallic=.125,roughness=.5,double_sided=False)
PAUSED = dict(base_color=[.03125,.625,.0625],emissive=[0.,.03125,0.],metallic=.375,roughness=.625,double_sided=False)
RUNNING = dict(base_color=[.0625,.03125,.625],emissive=[0.,0.,.0625],metallic=.0625,roughness=.25,double_sided=False)


def need(value, message):
    if not value:
        raise AssertionError(message)


def uid():
    return uuid.uuid4().hex


def encoded(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode('utf-8')


def same(a,b):
    return encoded(a)==encoded(b)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def component(entity, kind, value):
    return dict(op='component.set',id=entity,type=kind,value=value)


def transform(entity, position):
    return component(entity,'Transform',dict(position=position,rotation=[0,0,0,1],scale=[1,1,1]))


def compare(left,right):
    need(len(left)==len(right)==HEIGHT and all(len(row)==WIDTH for row in left+right),'Capture extent differs.')
    deltas=[max(abs(x-y) for x,y in zip(a,b)) for ar,br in zip(left,right) for a,b in zip(ar,br)]
    return dict(max_rgb_delta=max(deltas),strong_changed_pixels=sum(d>=2 for d in deltas))


class Owner:
    def __init__(self,args,record,deadline):
        self.args,self.record,self.deadline=args,record,deadline
        self.client=None;self.host=None;self.session=None;self.player=None;self.revision=0
        self.lock=threading.Lock();self.raw_bytes=0
        self.call_bytes=0;self.settled_calls=0
        self.endpoint='material-'+uid()
        self.host_stderr=args.output/'host.stderr.txt'
        self.host_stdout=args.output/'host.stdout.txt'
        self.stdout_truncated=False;self.stdout_observed_bytes=0;self.stdout_error=None
        self.stdout_thread=None
        with self.host_stderr.open('xb') as stderr:
            self.host=subprocess.Popen([str(args.binary),'serve',str(args.output/'world.json'),'--endpoint',self.endpoint],
                cwd=ROOT,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=stderr)
        record['host']=dict(process_id=self.host.pid,exit_code=None,forced_cleanup=False)
        # Drain the exact owned host's stdout while retaining at most16KiB.
        # CLI startup errors are JSON on stdout; they must survive connection failure.
        def stdout():
            try:
                kept=0
                with self.host_stdout.open('xb') as stream:
                    while True:
                        data=self.host.stdout.read(4096)
                        if not data:break
                        self.stdout_observed_bytes+=len(data)
                        retain=data[:max(0,16384-kept)]
                        if retain:stream.write(retain);kept+=len(retain)
                        self.stdout_truncated |= len(retain)!=len(data)
            except BaseException as error:self.stdout_error=repr(error)
        self.stdout_thread=threading.Thread(target=stdout,name='material-owned-host-stdout',daemon=True)
        self.stdout_thread.start()
        self.client=WorldClient.connect(args.binary,self.endpoint,cwd=ROOT,timeout_ms=30000,close_timeout=5)
        transport=self.client.transport
        original_accept=transport._accept_response
        def accept(response):
            self.raw(dict(direction='response',message=copy.deepcopy(response)))
            original_accept(response)
        transport._accept_response=accept
        request=transport.request
        def logged(method,params=None,timeout=30.0):
            self.time_left()
            with transport._condition:identifier=transport._next_id
            self.raw(dict(direction='request',message=dict(jsonrpc='2.0',id=identifier,method=method,params=copy.deepcopy(params or {}))))
            return request(method,params,timeout=min(timeout,self.time_left()))
        transport.request=logged

    def time_left(self):
        value=self.deadline-time.monotonic()
        need(value>0,'Whole fixture deadline exceeded.')
        return value

    def raw(self,row):
        size=len(encoded(row))
        with self.lock:
            need(size<=2*1048576 and self.raw_bytes+size<=24*1048576 and len(self.record['transport'])<2000,
                 'Lossless transport evidence bound exceeded.')
            self.raw_bytes+=size;self.record['transport'].append(row)

    def rpc(self,method,params=None):
        params={} if params is None else params
        row=dict(method=method,params=copy.deepcopy(params));self.record['calls'].append(row)
        try:
            result=self.client.call(method,params,timeout=min(30,self.time_left()))
            row['result']=copy.deepcopy(result);self.time_left()
            return result
        except BaseException as error:
            row['error']=dict(type=type(error).__name__,message=str(error))
            if isinstance(error,RpcError):row['error'].update(code=error.code,data=copy.deepcopy(error.data))
            raise
        finally:
            # Account only this detached settled row. Never serialize the growing
            # record or scan output files between an observed tick and its apply.
            size=len(encoded(row))
            need(size<=2*1048576 and self.call_bytes+size<=24*1048576 and self.settled_calls<1000,
                 'Settled interpreted RPC evidence bound exceeded.')
            self.call_bytes+=size;self.settled_calls+=1

    def accounting(self):
        # A terminal-only recomputation catches any accidental late row mutation.
        need(self.settled_calls==len(self.record['calls']) and
             self.call_bytes==sum(len(encoded(row)) for row in self.record['calls']) and
             self.raw_bytes==sum(len(encoded(row)) for row in self.record['transport']),
             'Recorded RPC rows changed after incremental accounting.')
        self.record['bookkeeping']=dict(settled_calls=self.settled_calls,call_row_bytes=self.call_bytes,
            transport_rows=len(self.record['transport']),transport_row_bytes=self.raw_bytes,
            per_row_cap_bytes=2*1048576,interpreted_cap_bytes=24*1048576,transport_cap_bytes=24*1048576,
            scope='Compact JSON value reserialization sizes; not wire bytes. Whole actual final output remains bounded64MiB.')

    def bound(self):
        need(sum(p.stat().st_size for p in self.args.output.rglob('*') if p.is_file())+len(encoded(self.record))<=CAP,
             'Whole output/evidence exceeds64MiB.')

    def edit(self,value):
        self.revision=self.rpc('world.transact',dict(base_revision=self.revision,request_id=uid(),
            ops=[component(BOX,'PbrMaterial',copy.deepcopy(value))]))['revision']
        self.bound() # completed authored-edit boundary, before any fresh tick guard

    def inspect(self):
        value=self.rpc('player.inspect')
        if self.player is not None:
            need(value['player_id']==self.player['player_id'] and value['generation']==self.player['generation'] and
                 value['session_id']==value['current_session_id']==self.session and value['active'],
                 'Player identity/lifetime changed.')
            need(value['report'] is None or value['report']['runtime_replacements']==0,'Runtime owner was replaced.')
        return value

    def await_presented(self,epoch,tick=None,after_sequence=None):
        limit=min(self.deadline,time.monotonic()+30)
        while time.monotonic()<limit:
            value=self.inspect();self.time_left()
            if value['ready']:
                report=value['report'];need(report['hardware'] and report['nvrhi_errors']==0,'Native window render failed.')
                need(report['width']==WIDTH and report['height']==HEIGHT and report['samples']==1,'Native window recipe changed.')
                materials=report['materials'];presented=materials['presented'];submitted=materials['submitted']
                if presented and presented['scene']['material_revision']==epoch:
                    self.receipt(presented,epoch,tick)
                    need(same(presented,submitted),'Presented receipt does not equal its actual submission.')
                    if after_sequence is not None:need(presented['scene']['frame_sequence']>after_sequence,'New material reused an old frame receipt.')
                    return value
            time.sleep(min(.01,max(0,limit-time.monotonic())))
        raise AssertionError('No exact new material presentation within30seconds.')

    def receipt(self,receipt,epoch,tick=None):
        scene=receipt['scene']
        need(receipt['session_id']==self.session and receipt['player_generation']==self.player['generation'] and
             type(receipt['tick']) is int and (tick is None or receipt['tick']==tick),'Receipt owner/tick differs.')
        need(scene['material_revision']==scene['presentation_generation']==epoch and scene['camera_id']==CAMERA and
             scene['authored_revision']==self.revision and
             type(scene['frame_sequence']) is int and scene['frame_sequence']>0 and
             type(scene['submission_id']) is int and scene['submission_id']>0,'Invalid exact submitted frame receipt.')
        if 'presentation_source_id' in self.record:
            need(scene['presentation_source_id']==self.record['presentation_source_id'] and
                 scene['world_id']==self.record['world_id'],'Runtime presentation source changed.')

    def control(self,action):
        current=self.inspect()
        return self.rpc('player.control',dict(player_id=current['player_id'],request_id=uid(),
            expected_control_revision=current['control_revision'],action=action))

    def close(self):
        errors=[]
        if self.client is not None:
            try:
                current=self.rpc('player.inspect')
                if current['active']:
                    stopped=self.rpc('player.control',dict(player_id=current['player_id'],request_id=uid(),
                        expected_control_revision=current['control_revision'],action='stop'))
                    need(stopped['active'] is False,'Player did not stop naturally.')
                terminal=self.rpc('player.inspect');self.record['terminal_player']=terminal
                if self.player is not None:
                    need(terminal['player_id']==self.player['player_id'] and terminal['generation']==self.player['generation'] and
                         not terminal['active'] and terminal['report']['success'] and terminal['report']['nvrhi_errors']==0,
                         'Terminal player report is not clean/exact.')
                if self.session is not None:self.rpc('runtime.stop',dict(session_id=self.session))
                self.rpc('host.shutdown')
            except BaseException as error:errors.append(repr(error))
            finally:
                try:self.client.close()
                except BaseException as error:errors.append(repr(error))
                transport=self.client.transport
                self.record['client']=dict(process_id=transport.process_id,exit_code=transport.returncode,
                    stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated)
        if self.host is not None:
            try:self.host.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.record['host']['forced_cleanup']=True;self.host.kill();self.host.wait(timeout=5)
                errors.append('Exact owned host required forced cleanup.')
            self.record['host']['exit_code']=self.host.returncode
            if self.stdout_thread is not None:
                self.stdout_thread.join(timeout=3)
                if self.stdout_thread.is_alive():errors.append('Owned host stdout reader did not finish.')
                else:self.host.stdout.close()
            if self.stdout_error is not None:errors.append(self.stdout_error)
            stdout=self.host_stdout.read_bytes() if self.host_stdout.is_file() else b''
            need(len(stdout)<=16384,'Bounded host stdout recorder exceeded16KiB.')
            self.record['host'].update(stdout=stdout.decode('utf-8',errors='strict'),stdout_bytes=len(stdout),
                stdout_observed_bytes=self.stdout_observed_bytes,stdout_truncated=self.stdout_truncated,
                stdout_sha256=hashlib.sha256(stdout).hexdigest())
            stderr=self.host_stderr.read_bytes()
            need(len(stderr)<=16384,'Host stderr exceeded bound.')
            self.record['host']['stderr']=stderr.decode('utf-8',errors='strict')
        self.record['cleanup_errors']=errors
        need(not errors and self.record['host']['exit_code']==0 and not self.record['host']['forced_cleanup'] and
             self.record['host']['stdout']=='' and self.record['host']['stdout_truncated'] is False and
             self.record['host']['stderr'] in ('Poima shared world ready: '+self.endpoint+'\n',
                 'Poima shared world ready: '+self.endpoint+'\r\n') and
             self.record['client']['exit_code']==0 and self.record['client']['stderr']=='' and
             self.record['client']['stderr_truncated'] is False,'Native owners did not close naturally.')
        self.accounting();self.bound()


def capture_recipe(args):
    return dict(camera=CAMERA,width=WIDTH,height=HEIGHT,gpu=args.gpu,samples=1,frames_in_flight=2,
        capture_frames=2,lighting_path=args.lighting_path,ambient_occlusion=dict(mode='none'),reconstruction='none',
        shadow_filter='pcf3_point')


def image_record(owner,label,result,path):
    need(result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0 and
         result['width']==WIDTH and result['height']==HEIGHT and result['build_version']=='0.0.113','Actual capture failed.')
    need(path.is_file(),'Native capture file missing.')
    owner.record['captures'].append(dict(label=label,path=path.name,sha256=sha(path),bytes=path.stat().st_size,result=copy.deepcopy(result)))
    owner.bound() # completed capture boundary; no following mutation uses an old tick
    return pixels(path)


def player_capture(owner,label,epoch):
    info=owner.inspect();need(info['paused'],'Exact player capture requires paused boundary.')
    path=owner.args.output/(label+'.bmp')
    request=dict(player_id=info['player_id'],request_id=uid(),expected_control_revision=info['control_revision'],
        session_id=owner.session,tick=info['tick'],expected_material_revision=epoch,path=str(path))
    result=owner.rpc('player.capture',request);capture=result['capture']
    need(capture['material_revision']==epoch,'Capture acknowledgement differs from exact material guard.')
    scene=capture['render_diagnostics']['scene_receipts']['presented']
    need(same(scene,capture['render_diagnostics']['scene_receipts']['submitted']) and
         scene['material_revision']==epoch and scene['presentation_source_id']==owner.record['presentation_source_id'] and
         scene['world_id']==owner.record['world_id'] and scene['camera_id']==CAMERA and
         scene['authored_revision']==owner.revision and scene['presentation_generation']==epoch and
         type(scene['frame_sequence']) is int and scene['frame_sequence']>0 and
         type(scene['submission_id']) is int and scene['submission_id']>0,'Capture exact immutable scene receipt differs.')
    after=owner.inspect();need(after['tick']==info['tick'] and after['paused'],'Paused capture advanced time.')
    return image_record(owner,label,capture,path),request


def checkpoint(owner,label,epoch):
    info=owner.rpc('runtime.inspect',dict(session_id=owner.session))
    receipt=owner.rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=label,expected_generation=0,
        session_id=owner.session,expected_tick=info['tick'],expected_gameplay_revision=0,expected_material_revision=epoch))
    selected=owner.rpc('save.inspect',dict(slot=label))['selected']
    directory=owner.args.output/'saves'/('slot-'+label);files=list(directory.glob('*.bin'))
    need(len(files)==1 and selected['verified'] and sha(files[0])==selected['sha256'] and receipt['generation']==1,
         'Actual durable snapshot does not match native receipt.')
    data=json.loads(files[0].read_text(encoding='utf-8'));owner.record['checkpoints'].append(
        dict(slot=label,receipt=receipt,selected=selected,sha256=sha(files[0]),snapshot=copy.deepcopy(data['snapshot'])))
    owner.bound() # completed paused checkpoint boundary
    return data['snapshot']['payload']


def apply_parameters(owner,info):
    return dict(session_id=owner.session,request_id=uid(),expected_tick=info['tick'],
        expected_structure_revision=info['structure_revision'],expected_material_revision=info['material_revision'],
        revision=owner.revision,expected_authored_revision=info['authored_revision'],expected_content_sha256=info['content_sha256'],ids=[BOX])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),required=True);parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args();args.binary=args.binary.resolve();args.output=args.output.resolve()
    need(os.name=='nt' and not sys.flags.optimize,'Native Windows Python under an external owned Job is required.')
    need(not args.output.exists(),'Output must be fresh.');args.output.mkdir(parents=True);(args.output/'saves').mkdir()
    started=time.monotonic();deadline=started+300
    sources=[Path(__file__),ROOT/'tests/scene_capture.py']+sorted(p for p in (ROOT/'tools/python/poima_client').iterdir()
        if p.is_file() and p.suffix in ('.py','.json'))
    pins={str(p):sha(p) for p in sources};binary=sha(args.binary);audio=sha(args.binary.parent/'phonon.dll')
    record=dict(passed=False,scope='One actual shared player; paused/running material snapshot acknowledgements and captures, no latency/scanout claim.',
        source_pins=pins,binary_sha256=binary,phonon_sha256=audio,recipe=dict(width=WIDTH,height=HEIGHT,gpu=args.gpu,
        lighting_path=args.lighting_path,frames_in_flight=2,samples=1,maximum_rgb_error=1),calls=[],transport=[],captures=[],checkpoints=[],comparisons=[])
    owner=None;failure=None
    try:
        owner=Owner.__new__(Owner);owner.__init__(args,record,deadline)
        discovery=owner.rpc('world.describe');record['discovery']=discovery
        need(all(name in discovery['methods'] for name in ('runtime.material.apply','runtime.material.inspect','player.start','player.capture')),
             'Material/player methods unavailable.')
        ops=[]
        for entity in (BOX,CAMERA,LIGHT,ENV):ops.append(dict(op='entity.create',id=entity,name='Material '+entity))
        ops.extend([transform(BOX,[0,0,0]),component(BOX,'MeshRenderer',dict(primitive='box',albedo=[.625,.0625,.03125],visible=True)),
            component(BOX,'PbrMaterial',BASE),transform(CAMERA,[0,0,6]),component(CAMERA,'Camera',dict(vertical_fov=40,near=.1,far=100)),
            transform(LIGHT,[0,0,3]),component(LIGHT,'Light',dict(kind='point',color=[1,1,1],intensity=12,enabled=True,
                shadow=dict(enabled=False,distance=20,bias=.0001,normal_bias=.005))),
            component(ENV,'LightingEnvironment',dict(ambient=[.2,.2,.2],exposure=1))])
        owner.revision=owner.rpc('world.transact',dict(base_revision=0,request_id=uid(),ops=ops))['revision']
        targets=[]
        for label,value in (('paused-target',PAUSED),('running-target',RUNNING)):
            owner.edit(value);path=args.output/(label+'.bmp')
            result=owner.rpc('world.capture',dict(**capture_recipe(args),revision=owner.revision,path=str(path)))
            targets.append(image_record(owner,label,result,path))
        owner.edit(BASE);owner.session=uid()
        owner.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(args.output/'saves')))
        owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision))
        owner.player=owner.rpc('player.start',dict(session_id=owner.session,request_id=uid(),expected_tick=0,
            expected_generation=0,camera=CAMERA,mode='interactive',initially_paused=True,audio=False,
            gamepad=dict(mode='disabled'),width=WIDTH,height=HEIGHT,gpu=args.gpu,samples=1,frames_in_flight=2,
            lighting_path=args.lighting_path,ambient_occlusion=dict(mode='none'),reconstruction='none',shadow_filter='pcf3_point'))
        ready=owner.await_presented(0,0);need(ready['paused'] and ready['tick']==0,'Initial player was not actually paused at tick0.')
        initial=ready['report']['materials']['presented']
        record['presentation_source_id']=initial['scene']['presentation_source_id'];record['world_id']=initial['scene']['world_id']
        before=checkpoint(owner,'original',0);initial_image,_=player_capture(owner,'initial',0)
        owner.edit(PAUSED);info=owner.rpc('runtime.inspect',dict(session_id=owner.session));params=apply_parameters(owner,info)
        applied=owner.rpc('runtime.material.apply',params);need(applied['material_revision']==1 and applied['tick']==0,'Paused apply changed native time/epoch.')
        accepted=owner.rpc('runtime.material.inspect',dict(session_id=owner.session,tick=0,material_revision=1,ids=[BOX]))
        need(accepted['material_revision']==1 and same(accepted['materials'][0]['material'],PAUSED),'Native commit is not directly inspectable.')
        record['immediate_player_inspect']=owner.inspect() # cached acknowledgement may legitimately precede this commit
        paused_presentation=owner.await_presented(1,0,initial['scene']['frame_sequence'])
        adopted_image,old_capture=player_capture(owner,'paused-adopted',1)
        parity=compare(targets[0],adopted_image);effect=compare(initial_image,adopted_image)
        need(parity['max_rgb_delta']<=1 and effect['strong_changed_pixels']>=100,'Paused adoption lacks authored parity/visible change.')
        record['comparisons'].append(dict(stage='paused',parity=parity,effect=effect))
        after=checkpoint(owner,'adopted',1)
        for payload in (before,after):payload.pop('authored_revision');payload.pop('content_sha256')
        need(same(before,after),'Paused adoption changed unrelated complete runtime/save state.')
        replay=owner.rpc('runtime.material.apply',params);need(same(replay,{**applied,'replayed':True}),'Exact material retry did not resolve its original receipt.')
        need(owner.rpc('runtime.material.inspect',dict(session_id=owner.session,tick=0,ids=[BOX]))['material_revision']==1,'Retry advanced epoch.')
        stale=copy.deepcopy(old_capture);stale.update(request_id=uid(),expected_material_revision=0,path=str(args.output/'stale.bmp'))
        try:owner.rpc('player.capture',stale)
        except RpcError as error:need(error.code==-32009 and 'material' in str(error).lower(),'Stale capture rejected for another reason.')
        else:raise AssertionError('Stale material capture guard was accepted.')
        need(not (args.output/'stale.bmp').exists(),'Rejected capture wrote pixels.')
        owner.control('resume');run_start=owner.inspect()['tick']
        progress_deadline=min(deadline,time.monotonic()+15)
        while owner.inspect()['tick']<=run_start:
            need(time.monotonic()<progress_deadline,'Running owner did not actually advance ticks without physical setup.');time.sleep(.01)
        owner.edit(RUNNING);running_deadline=min(deadline,time.monotonic()+15);record['running_tick_conflicts']=0
        for attempt in range(32):
            need(time.monotonic()<running_deadline,'Running apply guard retry bound expired.')
            info=owner.rpc('runtime.inspect',dict(session_id=owner.session));params2=apply_parameters(owner,info)
            try:applied2=owner.rpc('runtime.material.apply',params2);break
            except RpcError as error:
                need(error.code==-32009 and str(error)=='Runtime tick conflict.','Running apply failed beyond its observed tick guard.')
                record['running_tick_conflicts']+=1
        else:raise AssertionError('Running apply exhausted32 explicit fresh tick/request attempts.')
        need(applied2['material_revision']==2 and applied2['tick']>run_start,'Running apply did not commit during actual progress.')
        presented=owner.await_presented(2,after_sequence=paused_presentation['report']['materials']['presented']['scene']['frame_sequence'])
        need(presented['paused'] is False,'Running presentation required a pause/restart.')
        paused=owner.control('pause');need(paused['paused'],'Owner failed to pause.')
        stable=owner.inspect();need(stable['tick']>=applied2['tick'],'Pause lost accepted runtime time.')
        accepted2=owner.rpc('runtime.material.inspect',dict(session_id=owner.session,tick=stable['tick'],material_revision=2,ids=[BOX]))
        need(same(accepted2['materials'][0]['material'],RUNNING),'Running commit material differs.')
        owner.await_presented(2,stable['tick']);running_image,_=player_capture(owner,'running-adopted',2)
        parity2=compare(targets[1],running_image);effect2=compare(adopted_image,running_image)
        need(parity2['max_rgb_delta']<=1 and effect2['strong_changed_pixels']>=100,'Running adoption lacks authored parity/visible change.')
        record['comparisons'].append(dict(stage='running',parity=parity2,effect=effect2))
        replay2=owner.rpc('runtime.material.apply',params2);need(same(replay2,{**applied2,'replayed':True}),'Running receipt retry changed outcome.')
        need(owner.rpc('runtime.material.inspect',dict(session_id=owner.session,tick=stable['tick'],ids=[BOX]))['material_revision']==2,'Running retry advanced epoch.')
        record['accepted_running_tick']=applied2['tick'];record['final_tick']=stable['tick']
        need(len(record['captures'])==5 and len(record['comparisons'])==2,'Incomplete one-window capture matrix.')
    except BaseException as error:
        failure=repr(error);record['error']=failure;record['traceback']=traceback.format_exc()
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException as error:record['cleanup_error']=repr(error);failure=failure or repr(error)
        record['seconds']=time.monotonic()-started
        try:
            need(record['seconds']<=300 and all(sha(Path(p))==h for p,h in pins.items()) and sha(args.binary)==binary and
                 sha(args.binary.parent/'phonon.dll')==audio,'Deadline/source/native closure failed.')
            record['passed']=failure is None
            data=encoded(record)+b'\n';need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data)<=CAP,'Final64MiB output cap exceeded.')
            pending=args.output/'evidence.pending.json'
            with pending.open('xb') as stream:stream.write(data)
            need(time.monotonic()<=deadline,'Publication exceeded whole300seconds.')
            pending.rename(args.output/'evidence.json')
            need(time.monotonic()<=deadline,'Publication rename exceeded whole300seconds.')
        except BaseException as error:
            failure=failure or repr(error);record['passed']=False;record['publication_error']=repr(error)
            if (args.output/'evidence.json').exists():(args.output/'evidence.json').rename(args.output/'late-evidence.json')
            with (args.output/'failure.json').open('xb') as stream:
                stream.write(encoded(dict(passed=False,error=failure,seconds=time.monotonic()-started))+b'\n')
    print(json.dumps(dict(passed=failure is None,captures=len(record['captures']),seconds=record['seconds'])))
    return 0 if failure is None else 1

if __name__=='__main__':
    raise SystemExit(main())
