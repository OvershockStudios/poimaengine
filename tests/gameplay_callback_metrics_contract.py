#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual, headless CoreCLR callback metrics and complete native rollback.

Native Windows Python, inside an externally owned Windows Job. The caller
supplies already compiled, inventoried engine/bridge/game artifacts and a
separately pinned legacy pack/NativeAOT world. No build, download, forced GC,
rendering, physical input or cleanup of unknown processes is performed here.
Original wire files and complete native saves remain in the fresh output.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import threading
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'scripts'))
from authoring_pack import load_pack
from workflow_components_game_service import Checked,assigned_job,exact,need,read,write
from native_gameplay_owner import NativeOwner,trusted_descriptor
from workflow_save_upgrade import stored

GAME='Poima.Qualification.CallbackProbe'
BAD_INIT='Poima.Qualification.ThrowingInitializeProbe'
IDENTITY='poima.qualification.callback-metrics'
ACTOR,LABEL,PANEL,ALLOCATE,THROW=[f'{n:032x}'for n in(1671,1672,1670,1673,1674)]
KEYS={'animation','authored_revision','cameras','components','content_sha256','control_sequence','entities','gameplay','gameplay_revision','sound','structure','tick','ui','world_id'}
COUNTERS={'calls','returned','threw','samples','invalid_samples','allocated_bytes','max_allocated_bytes','gen0_collections','gen1_collections','gen2_collections'}
CAP=256*1024**2

def fresh():return uuid.uuid4().hex
def pin(path):
    path=Path(path);before=path.lstat()
    need(stat.S_ISREG(before.st_mode)and before.st_nlink==1 and not path.is_symlink()and not getattr(before,'st_file_attributes',0)&0x400,'Ordinary original file required')
    with path.open('rb')as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    after=path.stat();need((before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns)==(after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns),'File changed during hash')
    return dict(bytes=after.st_size,sha256=digest)
def census(root):
    count=total=0
    for path in root.rglob('*'):
        try:value=path.lstat()
        except FileNotFoundError:continue
        need(not path.is_symlink()and not getattr(value,'st_file_attributes',0)&0x400,'Linked output')
        if stat.S_ISDIR(value.st_mode):continue
        need(stat.S_ISREG(value.st_mode),'Nonordinary output');count+=1;total+=value.st_size
        need(count<=4096 and total<=CAP,'Whole callback qualification output cap')
    return dict(files=count,bytes=total)
def inventory(root):
    result={}
    for path in sorted(root.rglob('*')):
        if path.is_dir():
            need(not path.is_symlink()and not getattr(path.lstat(),'st_file_attributes',0)&0x400,'Ordinary artifact directory');continue
        result[path.relative_to(root).as_posix()]=pin(path)
        need(len(result)<=256 and sum(row['bytes']for row in result.values())<=CAP,'Bounded complete selected artifact closure')
    return result
def uint(value,positive=False):
    need(type(value)is str and re.fullmatch(r'0|[1-9][0-9]{0,19}',value)is not None,'Canonical unsigned decimal counter')
    number=int(value);need(number<=2**64-1 and(not positive or number>0),'Unsigned counter range');return number
def validate_metrics(result,owner,*,enabled,backend='coreclr'):
    need(set(result)=={'session_id','tick','revision','structure_revision','module'}and result['session_id']==owner.session and type(result['tick'])is int and result['tick']==owner.tick,'Exact metrics owner envelope')
    module=result['module'];need(type(module)is dict and set(module)=={'format','version','backend','identity','assembly_sha256','supported','enabled','unavailable_reason','data'},'Closed module metrics shape')
    game=owner.game();actual=game['module']
    need(module['format']=='poima.gameplay-callback-metrics.v1'and type(module['version'])is int and module['version']==1 and module['backend']==backend and module['identity']==actual['schema']['identity']and module['assembly_sha256']==actual['assembly_sha256']and module['enabled']is enabled and result['revision']==game['revision']and result['structure_revision']==game['structure_revision'],'Actual module image, schema and revision')
    if not enabled:
        reason='native_aot_not_supported'if backend=='native_aot'else'not_requested'
        need(module['supported']is(False if backend=='native_aot'else None)and module['unavailable_reason']==reason and module['data']is None,'Disabled/unsupported is not zero observations');return None
    need(module['supported']is True and module['unavailable_reason']is None,'Negotiated CoreCLR metrics required')
    data=module['data'];need(type(data)is dict and set(data)=={'format','version','module_handle','owner_thread_id','saturated','last_issue','initialize','tick','control'},'Closed cached data shape')
    need(data['format']=='poima.managed-callback-metrics.v1'and type(data['version'])is int and data['version']==1 and type(data['owner_thread_id'])is int and data['owner_thread_id']>0 and data['saturated']is False and data['last_issue']is None,'Valid actual callback brackets');uint(data['module_handle'],True)
    for name in('initialize','tick','control'):
        bucket=data[name];need(type(bucket)is dict and set(bucket)==COUNTERS,'Closed callback counter bucket')
        numbers={k:uint(v)for k,v in bucket.items()}
        need(numbers['calls']==numbers['returned']+numbers['threw']==numbers['samples']+numbers['invalid_samples']and numbers['invalid_samples']==0 and numbers['max_allocated_bytes']<=numbers['allocated_bytes'],'Actual unsaturated callback accounting')
    return data
def bucket(data,name,*,calls,returned,threw,minimum_bytes):
    b=data[name];need(b['calls']==str(calls)and b['returned']==str(returned)and b['threw']==str(threw)and b['samples']==str(calls)and b['invalid_samples']=='0'and uint(b['allocated_bytes'])>=minimum_bytes,'Actual allocating/throwing callback attempt forecast')

class Probe(NativeOwner):
    def __init__(self,engine,directory,deadline,world,saves,*,bridge=None,assembly=None,hostfxr=None):
        self.stable_validated=0;self.bridge=bridge;self.assembly=assembly;self.hostfxr=hostfxr
        Checked.__init__(self,engine,directory,deadline,world=world,saves=saves)
    def call(self,method,params=None,error=None):
        need(len(self.calls)<1200,'Bounded native protocol request census')
        return Checked.call(self,method,params,error)
    def module(self):return self.game()
    def start(self):
        self.session,self.tick=fresh(),0;revision=self.call('world.inspect')['revision'];self.call('runtime.start',dict(session_id=self.session,revision=revision))
    def load(self,profile=None,*,game=GAME,error=None):
        before=self.game();p=dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_revision=before['revision'],expected_structure_revision=before['structure_revision'],hostfxr=str(self.hostfxr),bridge=str(self.bridge),assembly=str(self.assembly),type=game)
        if profile is not None:p['profile_callbacks']=profile
        result=self.call('runtime.gameplay.load',p,error)
        if error is None:
            need(exact(self.call('runtime.gameplay.load',p),dict(result,replayed=True)),'Exact successful load retry changes receipt')
        return p,result
    def metrics(self,enabled,backend='coreclr'):
        row=self.call('runtime.gameplay.metrics',dict(session_id=self.session,tick=self.tick));return row,validate_metrics(row,self,enabled=enabled,backend=backend)
    def checkpoint(self,label,report):
        point=self.probe('metrics-'+label);doc=point['document'];payload=doc['snapshot']['payload']
        need(doc['format']=='poima.world-save'and doc['snapshot']['version']==7 and set(payload)==KEYS,'Whole unmodified v7 native snapshot')
        retained=self.directory.parent/'checkpoints'/(label+'.bin');retained.parent.mkdir(exist_ok=True)
        shutil.copyfile(point['path'],retained);need(pin(retained)==point['identity'],'Retained complete original save bytes')
        report['checkpoints'][label]=dict(file=str(retained),identity=pin(retained),payload_sha256=doc['snapshot']['sha256'],tick=payload['tick'])
        return point
    def fail_step(self,marker):
        p=dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_structure_revision=self.inspect()['structure_revision'],ticks=1)
        result=self.call('runtime.step',p,error=-32040);need(marker in result['message'],'Expected actual native callback failure');return dict(request=p,error=result)
    def fail_control(self):
        state,game=self.inspect(),self.game();p=dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=THROW)
        result=self.call('runtime.ui.activate',p,error=-32040);need('CallbackProbe actual Control throw.'in result['message'],'Expected actual Control callback exception');return dict(request=p,error=result)

def tiny_world(owner):
    def ui(identity,kind,text,action=None,parent=PANEL):return dict(op='ui.element.set',id=identity,element=dict(parent=parent,name='Callback probe '+kind,kind=kind,text=text,action=action,visible=True,enabled=True))
    camera=f'{1680:032x}'
    ops=[dict(op='entity.create',id=ACTOR,name='Callback staged kinematic actor'),dict(op='component.set',id=ACTOR,type='Transform',value=dict(position=[0,2,0],rotation=[0,0,0,1],scale=[1,1,1])),dict(op='component.set',id=ACTOR,type='BoxCollider',value=dict(motion='kinematic',half_extents=[.5,.5,.5],mass=1,friction=.5,restitution=0)),dict(op='entity.create',id=camera,name='Saved camera, no renderer'),dict(op='component.set',id=camera,type='Camera',value=dict(vertical_fov=50,near=.05,far=100)),ui(PANEL,'panel','',parent=None),ui(LABEL,'label','Initial'),ui(ALLOCATE,'button','Allocate','allocate'),ui(THROW,'button','Throw','throw')]
    owner.call('world.transact',dict(request_id=fresh(),base_revision=owner.call('world.inspect')['revision'],ops=ops));owner.start()
def same(left,right,report,label):
    need(exact(left['document'],right['document'])and Path(left['path']).read_bytes()==Path(right['path']).read_bytes(),'Complete type-and-byte save equality failed: '+label)
    report['whole_state_pairs'].append(dict(label=label,left=left['identity'],right=right['identity'],exclusions=[]))
def storage(owner):return dict(queue=owner.call('runtime.save.status',dict(session_id=owner.session)),configuration=owner.call('save.status'),files={p.relative_to(owner.expected_save_root).as_posix():pin(p)for p in owner.expected_save_root.rglob('*')if p.is_file()})
def unchanged_failure(owner,report,label,action):
    before=owner.checkpoint(label+'-before',report);external=storage(owner);result=action();need(exact(storage(owner),external),'Failed operation changed original save queue/configuration/files')
    after=owner.checkpoint(label+'-after',report);same(before,after,report,label);return result
def reload_state(before,after):
    expected=copy.deepcopy(before['document']);expected['snapshot']['payload']['gameplay_revision']+=1
    expected['snapshot']['sha256']=after['document']['snapshot']['sha256'] # stored() independently checked original lexical checksum.
    need(exact(expected,after['document']),'Same-image compatible reload changed complete state beyond revision and derived checksum')
def copy_closure(source,dest,inventory):
    for name,identity in inventory.items():
        relative=Path(name);need(not relative.is_absolute()and'..'not in relative.parts,'Safe explicit legacy closure')
        original=source/relative;need(pin(original)==identity,'Pinned original legacy source differs');target=dest/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target);need(pin(target)==identity,'Independent legacy bytes differ')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in('engine','assembly','bridge','hostfxr','old-pack','aot-world','legacy-authority','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--seconds',type=int,default=300);a=p.parse_args()
    need(os.name=='nt'and not sys.flags.optimize,'Native Windows nonoptimized Python required');assigned_job();need(60<=a.seconds<=600,'Bounded fixture60..600s')
    for key in('engine','assembly','bridge','hostfxr','old_pack','aot_world','legacy_authority'):setattr(a,key,getattr(a,key).resolve(strict=True))
    output=a.output.absolute();need(not output.exists(),'Fresh output only')
    for immutable in(a.engine.parent,a.assembly.parent,a.bridge.parent,a.hostfxr.parent,a.old_pack,a.aot_world.parent,ROOT):need(not output.is_relative_to(immutable)and not immutable.is_relative_to(output),'Output overlaps original input/helper closure')
    authority=read(a.legacy_authority);need(authority['format']=='poima.callback-metrics.legacy-authority.v1','Selected independent legacy authority')
    old=load_pack(a.old_pack);old.verify();need(old.engine_version=='0.0.164'and pin(old.root/'authoring.json')==authority['old_pack_manifest'],'Original qualified premetrics installed pack identity')
    need(pin(a.aot_world)==authority['native_aot_source_files']['world.json'],'Original166 final authored-world identity')
    old_descriptor=a.aot_world.parent/'gameplay/native-gameplay.json';trusted_descriptor(old_descriptor)
    need(pin(old_descriptor)==authority['native_aot_source_files']['gameplay/native-gameplay.json'],'Original166 NativeAOT descriptor')
    sdk=pin(old.resource('sdk/Poima.Gameplay.dll'))
    need(pin(a.assembly.parent/'Poima.Gameplay.dll')==pin(a.bridge.parent/'Poima.Gameplay.dll')==sdk,'Unchanged selected binary SDK across game/new bridge/old pack')
    selected={str(path):pin(path)for path in(a.engine,a.assembly,a.bridge,a.hostfxr,a.legacy_authority,Path(__file__),ROOT/'tests/managed_callback_metrics_game/CallbackProbe.cs',ROOT/'tests/managed_callback_metrics_game/Poima.CallbackMetricsGame.csproj')}
    artifact_closures=dict(game=inventory(a.assembly.parent),bridge=inventory(a.bridge.parent))
    need({'Poima.ManagedBridge.dll','Poima.ManagedBridge.deps.json','Poima.ManagedBridge.runtimeconfig.json','Poima.Gameplay.dll'}<=set(artifact_closures['bridge']),'Complete new bridge runtime prerequisites')
    output.mkdir(parents=True);(output/'saves').mkdir();deadline=time.monotonic()+a.seconds;report=dict(format='poima.gameplay-callback-metrics.native-contract.v1',passed=False,errors=[],cleanup_errors=[],inputs=selected,artifact_closures=artifact_closures,legacy_authority=authority,owners=[],checkpoints={},whole_state_pairs=[],observations={})
    owner=None;done=threading.Event()
    def watch():
        while not done.wait(.5):
            try:need(time.monotonic()<deadline,'Owned fixture deadline');census(output)
            except BaseException:
                with(output/'watchdog-failure.txt').open('x')as f:f.write(traceback.format_exc())
                os._exit(91)
    watcher=threading.Thread(target=watch,daemon=True);watcher.start()
    try:
        owner=Probe(a.engine,output/'current',deadline,output/'world.json',output/'saves',bridge=a.bridge,assembly=a.assembly,hostfxr=a.hostfxr);tiny_world(owner)
        need(owner.call('runtime.gameplay.metrics',dict(session_id=owner.session))['module']is None,'No module must report null')
        default_request,_=owner.load();owner.metrics(False)
        owner.call('runtime.gameplay.load',dict(default_request,profile_callbacks=False),error=-32010)
        owner.step();owner.control(ALLOCATE,'allocate');owner.metrics(False)
        before=owner.checkpoint('reads-before',report);owner.metrics(False);owner.metrics(False);after=owner.checkpoint('reads-after',report);same(before,after,report,'default reads do not change native state')
        owner.load(False);owner.metrics(False)
        for invalid in(0,1,'true',[],{}):owner.load(invalid,error=-32602)
        null_request,_=owner.load(False)
        owner.call('runtime.gameplay.load',dict(null_request,request_id=fresh(),profile_callbacks=None),error=-32602)
        before=owner.checkpoint('enable-before',report);owner.load(True);after=owner.checkpoint('enable-after',report);reload_state(before,after)
        m,d=owner.metrics(True);bucket(d,'initialize',calls=1,returned=1,threw=0,minimum_bytes=32768);bucket(d,'tick',calls=0,returned=0,threw=0,minimum_bytes=0);bucket(d,'control',calls=0,returned=0,threw=0,minimum_bytes=0)
        first_handle=d['module_handle'];owner.step(2);owner.control(ALLOCATE,'allocate');m,d=owner.metrics(True);bucket(d,'tick',calls=2,returned=2,threw=0,minimum_bytes=8192);bucket(d,'control',calls=1,returned=1,threw=0,minimum_bytes=8192)
        before=owner.checkpoint('enabled-reads-before',report);prior=owner.metrics(True)[0];need(exact(owner.metrics(True)[0],prior),'Read mutated cached counters');after=owner.checkpoint('enabled-reads-after',report);same(before,after,report,'enabled reads do not change full native state')
        owner.edit(Mode=1);report['observations']['throwing_tick']=unchanged_failure(owner,report,'throwing-tick',lambda:owner.fail_step('CallbackProbe actual Tick throw.'))
        d=owner.metrics(True)[1];bucket(d,'tick',calls=3,returned=2,threw=1,minimum_bytes=12288)
        owner.edit(Mode=0);report['observations']['throwing_control']=unchanged_failure(owner,report,'throwing-control',owner.fail_control)
        d=owner.metrics(True)[1];bucket(d,'control',calls=2,returned=1,threw=1,minimum_bytes=16384)
        owner.edit(Mode=2)
        report['observations']['returned_native_rejection']=unchanged_failure(owner,report,'returned-native-rejection',lambda:owner.fail_step('reference'))
        d=owner.metrics(True)[1];bucket(d,'tick',calls=4,returned=3,threw=1,minimum_bytes=16384)
        owner.edit(Mode=0)
        prior=owner.metrics(True)[0];report['observations']['throwing_initialize']=unchanged_failure(owner,report,'throwing-initialize',lambda:owner.load(True,game=BAD_INIT,error=-32060));need(exact(owner.metrics(True)[0],prior),'Failed candidate replaced old metrics')
        before=owner.checkpoint('reset-before',report);owner.load(True);after=owner.checkpoint('reset-after',report);reload_state(before,after);m,d=owner.metrics(True)
        need(d['module_handle']!=first_handle,'Reload reused diagnostic handle');bucket(d,'initialize',calls=1,returned=1,threw=0,minimum_bytes=32768);bucket(d,'tick',calls=0,returned=0,threw=0,minimum_bytes=0);bucket(d,'control',calls=0,returned=0,threw=0,minimum_bytes=0)
        report['observations']['current_reset']=m
        owner.call('runtime.gameplay.metrics',dict(session_id=owner.session,tick=owner.tick+1),error=-32009)
        owner.call('runtime.gameplay.metrics',dict(session_id=owner.session,tick=float(owner.tick)),error=-32602)
        owner.call('runtime.gameplay.metrics',dict(session_id=owner.session,unexpected=True),error=-32602)
        report['owners'].append(dict(label='current',**owner.close_recorded()));owner=None

        owner=Probe(a.engine,output/'old-bridge',deadline,output/'old-world.json',output/'saves',bridge=old.resource('bridge/Poima.ManagedBridge.dll'),assembly=a.assembly,hostfxr=a.hostfxr);tiny_world(owner);owner.load();owner.step();owner.metrics(False)
        failure=unchanged_failure(owner,report,'old-bridge-optin',lambda:owner.load(True,error=-32060));need('does not support requested callback metrics'in failure[1]['message']and'already hosts a different'not in failure[1]['message'],'Old bridge rejection must prove negotiation, not path-switch guard')
        owner.metrics(False);owner.step();report['observations']['old_bridge_optin']=failure
        report['owners'].append(dict(label='old-bridge',**owner.close_recorded()));owner=None

        stage=output/'aot-stage';copy_closure(a.aot_world.parent,stage,authority['native_aot_source_files'])
        owner=Probe(a.engine,output/'aot',deadline,stage/'world.json',output/'saves');owner.start();before=owner.game();descriptor=stage/'gameplay/native-gameplay.json'
        req=dict(session_id=owner.session,request_id=fresh(),expected_tick=0,expected_revision=before['revision'],expected_structure_revision=before['structure_revision'],descriptor=str(descriptor),expected_descriptor_sha256=pin(descriptor)['sha256'])
        owner.call('runtime.gameplay.load_native',req);before=owner.checkpoint('aot-before',report);report['observations']['native_aot']=owner.metrics(False,'native_aot')[0];owner.metrics(False,'native_aot');after=owner.checkpoint('aot-after',report);same(before,after,report,'unsupported AOT reads preserve complete native state')
        owner.call('runtime.gameplay.load_native',dict(req,request_id=fresh(),profile_callbacks=True),error=-32602)
        report['owners'].append(dict(label='aot',**owner.close_recorded()));owner=None
        for name,identity in selected.items():need(pin(Path(name))==identity,'Selected immutable input changed')
        need(exact(inventory(a.assembly.parent),artifact_closures['game'])and exact(inventory(a.bridge.parent),artifact_closures['bridge']),'Complete game/newbridge artifact membership changed')
        for name,identity in authority['native_aot_source_files'].items():need(pin(a.aot_world.parent/name)==identity,'Original166 source changed')
        old.verify();report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:report['owners'].append(dict(label='failure-owner',**owner.close_recorded()))
            except BaseException:report['cleanup_errors'].append(traceback.format_exc())
        report['passed']=bool(report['passed']and not report['errors']and not report['cleanup_errors']);report['seconds']=a.seconds-(deadline-time.monotonic())
        try:
            report['terminal_output_before_publication']=census(output);data=(json.dumps(report,indent=2,allow_nan=False)+'\n').encode();need(len(data)<=32*1024**2 and census(output)['bytes']+len(data)<=CAP,'Reserved final complete evidence publication');write(output/'report.json',report);census(output)
        finally:done.set();watcher.join(2);need(not watcher.is_alive(),'Output watchdog retirement')
    print(json.dumps(dict(passed=report['passed'],seconds=report['seconds'],owners=len(report['owners']),pairs=len(report['whole_state_pairs']),errors=report['errors'],cleanup_errors=report['cleanup_errors'])))
    return 0 if report['passed']else 1
if __name__=='__main__':raise SystemExit(main())
