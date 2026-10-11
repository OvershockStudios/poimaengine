#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a retained CoreCLR Relay Yard save plus one explicit new type.

Caller must gate this module before imports in an owned Windows Job. The caller
first compiles only the supplied target recipe through development.compile and
supplies its exact assembly/manifest. This verifier never invokes dotnet, edits
the retained source checkpoint, teleports actors or launches a renderer.
"""
import argparse,copy,hashlib,importlib.util,json,math,os,shutil,sys,time,traceback,uuid
from pathlib import Path
from types import SimpleNamespace

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'examples/relay-yard/run.py').is_file())
sys.path.insert(0,str(ROOT/'tests'))
import relay_yard_contract as contract
import instance_save_evolution as raw_json
launcher=contract.load_launcher()
need,uid=contract.need,contract.uid
TYPE='c1120000000000000000000000000001'
ENTITIES=[uid(100),uid(300)]
SOURCE_SLOT='iteration-a'
SOURCE_PAYLOAD_SHA='36fa97572d79ed42abc66b05ee63703e98f8ee3771132921f0e1dcbbd02e723d'
SOURCE_IMAGE_SHA='00f148dc9abb4fd43d16eee4acf612aff1c4d20535f42082e19276cba3b34f68'
SOURCE_FIELDS={uid(n):0 for n in (1,2,3)}
fresh=lambda:uuid.uuid4().hex

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda:stream.read(1048576),b''):h.update(part)
    return h.hexdigest()

def read(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size<=64*1048576,'Bounded regular JSON required')
    return json.loads(path.read_text(encoding='utf-8'))

def exact(a,b):
    # Preserve JSON bool/int and floating/integer distinctions; no loose equality.
    return json.dumps(a,sort_keys=True,separators=(',',':'),allow_nan=False)==json.dumps(b,sort_keys=True,separators=(',',':'),allow_nan=False)

def inventory(root):
    result={};total=0
    for p in sorted(root.rglob('*')):
        need(not p.is_symlink() and not getattr(p,'is_junction',lambda:False)(),'Linked closure forbidden')
        if p.is_file():
            total+=p.stat().st_size;need(len(result)<10000 and total<=128*1048576,'Closure cap exceeded')
            name=p.relative_to(root).as_posix();need(name.casefold() not in {n.casefold() for n in result},'Closure case collision')
            result[name]=dict(bytes=p.stat().st_size,sha256=sha(p))
    return result

def stored(storage,slot):
    folder=storage/('slot-'+slot);manifest=read(folder/'current.json');entry=manifest['payload']['current']
    name=entry['file'];need(type(name) is str and len(name)==38 and name.startswith('p-') and name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),'Unsafe payload name')
    p=folder/name;need(p.stat().st_size==entry['bytes'] and sha(p)==entry['sha256'],'Durable manifest/payload mismatch')
    save=read(p);need(save['format']=='poima.world-save' and save['version']==1 and save['snapshot']['format']=='poima.runtime-snapshot','Unknown save format')
    need(raw_json.raw_digest(p.read_text(encoding='utf-8'),('snapshot','payload'))==save['snapshot']['sha256'],'Exact raw native snapshot digest mismatch')
    return save,p,entry

def identity(save,path):
    p=save['snapshot']['payload'];g=p['gameplay']
    return dict(world_id=p['world_id'],content_sha256=p['content_sha256'],backend=g['backend'],module_identity=g['schema']['identity'],type=g['type'],image_sha256=g['assembly_sha256'],schema_sha256=raw_json.raw_digest(path.read_text(encoding='utf-8'),('snapshot','payload','gameplay','schema')))

def same_json(a,b):
    if type(a) is not type(b):return False
    if type(a) is dict:return a.keys()==b.keys() and all(same_json(a[k],b[k]) for k in a)
    if type(a) is list:return len(a)==len(b) and all(same_json(x,y) for x,y in zip(a,b))
    if type(a) is float:return a.hex()==b.hex()
    return a==b

def strict_load(encoded):
    def pairs(items):
        result={}
        for key,value in items:
            need(key not in result,'Duplicate retained row member');result[key]=value
        return result
    return json.loads(encoded,object_pairs_hook=pairs,parse_constant=lambda _:need(False,'Nonfinite retained row'))

def failure_receipt(record,directory,stage,error,context=None):
    # This bounded independent receipt survives a failed full-record encoder.
    # The first exception is immutable; later cleanup/recording faults are extra.
    text=''.join(traceback.format_exception(type(error),error,error.__traceback__))[-24576:]
    item=dict(stage=str(stage)[:128],exception_type=type(error).__name__[:128],traceback=text,
        call_count=len(record.get('calls',[])),context=context or {})
    if 'first_failure' not in record:record['first_failure']=item
    else:record.setdefault('secondary_failures',[]).append(item)
    record['passed']=False
    value=dict(passed=False,first_failure=record['first_failure'],
        secondary_failures=record.get('secondary_failures',[])[:8])
    encoded=(json.dumps(value,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
    need(len(encoded)<=256*1024,'Bounded first-failure receipt exceeded')
    temporary=directory/'failure.json.tmp'
    with temporary.open('wb') as stream:
        stream.write(encoded);stream.flush();os.fsync(stream.fileno())
    os.replace(temporary,directory/'failure.json')

class SettledCalls:
    """Lossless original OwnedRelay rows, not transport JSON-RPC envelopes."""
    def __init__(self,path,cap=64*1048576,row_cap=1048576):
        need(type(cap) is int and cap>0 and type(row_cap) is int and 0<row_cap<=cap,'Strict recorder caps required')
        self.path=Path(path);self.cap=cap;self.row_cap=row_cap;self.bytes=0;self.count=0
        self.hash=hashlib.sha256();self.failed=False;self.closed=False;self.stream=self.path.open('xb')

    def append(self,raw):
        need(not self.closed and not self.failed,'Recorder is closed/failed')
        try:
            encoded=(json.dumps(raw,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
            need(len(encoded)<=self.row_cap,'Individual settled row exceeds1MiB')
            need(self.bytes+len(encoded)<=self.cap,'Whole settled RPC ledger exceeds64MiB')
            decoded=strict_load(encoded);need(same_json(decoded,raw),'Settled row JSON changes a Python type/value')
            meta=dict(owner=raw['owner'],method=raw['method'],params=copy.deepcopy(raw['params']),
                raw=dict(ordinal=self.count,offset=self.bytes,bytes=len(encoded),sha256=hashlib.sha256(encoded).hexdigest()))
            # A short/failed append cannot publish a successful row receipt.
            need(self.stream.write(encoded)==len(encoded),'Short settled row write');self.stream.flush()
            self.hash.update(encoded);self.bytes+=len(encoded);self.count+=1
            return meta
        except BaseException:
            self.failed=True;raise

    def close(self):
        if self.closed:return
        try:self.stream.flush();os.fsync(self.stream.fileno())
        finally:self.stream.close();self.closed=True

    def verify(self,metadata):
        need(self.closed and not self.failed and self.path.stat().st_size==self.bytes and sha(self.path)==self.hash.hexdigest(),
            'Terminal settled ledger hash/size/close mismatch')
        need(len(metadata)==self.count,'Settled metadata count differs')
        offset=0
        with self.path.open('rb') as stream:
            for ordinal,meta in enumerate(metadata):
                encoded=stream.readline(self.row_cap+1)
                need(encoded.endswith(b'\n') and len(encoded)<=self.row_cap,'Bounded complete settled row required')
                need(meta['raw']==dict(ordinal=ordinal,offset=offset,bytes=len(encoded),sha256=hashlib.sha256(encoded).hexdigest()),
                    'Settled ordinal/offset/hash/index differs')
                raw=strict_load(encoded)
                need(set(raw)<= {'owner','method','params','result','error'} and not ('result' in raw and 'error' in raw),
                    'Unknown original row shape')
                need(same_json({k:raw[k] for k in ('owner','method','params')},{k:meta[k] for k in ('owner','method','params')}),
                    'Original/light method/params correlation differs')
                offset+=len(encoded)
            need(not stream.read(1) and offset==self.bytes,'Unexpected raw tail or missing row')
        return dict(format='poima.settled-relay-rpc-rows',version=1,path=self.path.name,bytes=self.bytes,
            sha256=self.hash.hexdigest(),rows=self.count,row_cap_bytes=self.row_cap,total_cap_bytes=self.cap,
            exact_index_and_light_projection=True,scope='Original owner/method/params/native-result-or-error rows; no wire envelope claim')

class Owner(launcher.OwnedRelay):
    def __init__(self,args,assembly,world,saves,label,record,deadline):
        config=SimpleNamespace(binary=args.binary,descriptor=None,hostfxr=args.hostfxr,bridge=args.bridge,assembly=assembly)
        super().__init__(config,world,label,record,deadline,lambda p:str(Path(p).resolve()),None)
        self.ledger=args.ledger;self.output=args.output
        self.storage=saves;self.saves(saves)

    def rpc(self,method,params=None,error=None,timeout=30):
        need(self.calls<20000 and len(self.record['calls'])<50000,'RPC count bound exceeded')
        index=len(self.record['calls'])
        try:result=super().rpc(method,copy.deepcopy(params),error,min(timeout,30))
        except BaseException as first:
            failure_receipt(self.record,self.output,'native_call',first,dict(owner=self.label,method=method))
            try:self.settle(index)
            except BaseException as secondary:
                failure_receipt(self.record,self.output,'recording_failed_native_call',secondary,dict(owner=self.label,method=method))
            raise
        try:self.settle(index)
        except BaseException as failure:
            failure_receipt(self.record,self.output,'settled_row_recording',failure,dict(owner=self.label,method=method));raise
        return result

    def settle(self,index):
        need(index==len(self.record['calls'])-1,'A native call did not append exactly one original row')
        self.record['calls'][index]=self.ledger.append(self.record['calls'][index])
        # Preserve RelayDriver.control's method/params slices with bounded metadata.
        if len(self.record['calls'])%64==0:bounded_record(self.record)

    def guards(self):
        keys=('session_id','tick','gameplay_revision','component_revision','structure_revision','ui_revision','control_sequence')
        if not self.session:return {'expected_'+k:None for k in keys}
        state,module,route=self.inspect(),self.module(),self.route()
        return dict(expected_session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=module['revision'],expected_component_revision=route['component_revision'],**{'expected_'+k:state[k] for k in ('structure_revision','ui_revision','control_sequence')})

    def load_parameters(self,slot=SOURCE_SLOT,generation=1,plan=None):
        p=dict(request_id=fresh(),configuration_generation=self.configuration,slot=slot,expected_generation=generation,revision=self.rpc('world.inspect')['revision'],new_session_id=fresh(),gameplay=self.config(),**self.guards())
        if plan is not None:p['upgrade']=dict(path=str(plan.resolve()),expected_sha256=sha(plan))
        return p

    def restore_request(self,p,error=None):
        result=self.rpc('save.load',p,error)
        if error is None:self.session,self.tick=p['new_session_id'],result['tick']
        return result

    def write(self,slot):
        g=self.guards();g['session_id']=g.pop('expected_session_id')
        result=self.rpc('save.write',dict(request_id=fresh(),configuration_generation=self.configuration,slot=slot,expected_generation=0,**g))
        save,path,entry=stored(self.storage,slot);need(result['sha256']==entry['sha256'],'Save reply differs from published bytes')
        return save,path,result

    def snapshot(self,label):
        save,path,result=self.write('probe-'+fresh());self.record.setdefault('snapshots',{})[self.label+':'+label]=dict(snapshot=save['snapshot'],payload_sha256=sha(path),receipt=result)
        bounded_record(self.record)
        return save['snapshot']

    def fingerprint(self):
        return dict(world=self.rpc('world.inspect'),history=self.rpc('world.history'),runtime=self.inspect(),module=self.module(),route=self.route(),ui=self.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256)),saves=self.rpc('runtime.save.status',dict(session_id=self.session)))

    def progress(self):
        result=[self.rpc('runtime.component.get',dict(session_id=self.session,tick=self.tick,id=entity,type=TYPE)) for entity in ENTITIES]
        return [r['values'] for r in result]

def write_json(path,value):path.write_text(json.dumps(value,indent=2,allow_nan=False)+'\n',encoding='utf-8')

def bounded_record(record):
    need(len(json.dumps(record,separators=(',',':'),allow_nan=False).encode('utf-8'))<=16*1048576-256*1024,'Incremental complete metadata byte budget exceeded (failure headroom reserved)')

def qualify(args,record,owners,deadline):
    baseline=args.baseline;run=baseline/'run';historical=read(baseline/'owner.json')
    need(historical['passed'] is True and historical['child_passed'] is True and historical['forced_cleanup'] is False and historical['exit_code']==0 and historical['job_accounting_at_child_exit']['active_processes']==historical['job_accounting_at_child_exit']['terminated_processes']==0,'Retained source owner is not qualified/closed')
    original,original_path,entry=stored(run/'saves',SOURCE_SLOT);old=original['snapshot']['payload'];source_image=run/'compiled/a/managed/Poima.RelayYardGame.dll'
    need(entry['generation']==1 and original_path.stat().st_size==120965 and sha(original_path)==SOURCE_PAYLOAD_SHA and sha(source_image)==SOURCE_IMAGE_SHA,'Exact retained source/image differ')
    need(original['snapshot']['version']==5 and old['tick']==140 and len(old['entities'])==83 and len(old['gameplay']['values'])==50 and len(old['components']['types'])==3 and old['gameplay']['backend']=='coreclr' and old['gameplay']['values']['Collected']==1 and old['gameplay']['values']['Started']==1 and old['gameplay']['values']['Won']==0,'Not the retained partial Relay Yard game')
    need(old['gameplay']['assembly_sha256']==SOURCE_IMAGE_SHA and old['gameplay']['values']['CellOne']==uid(0),'Saved image/removed pickup identity differs')
    manifest=read(args.target_manifest);schemas={s['id']:s for s in manifest['schemas']};need(set(schemas)==set(original['document']['component_schemas'])|{TYPE},'Target declares an unexplained component type')
    progress_schema=schemas[TYPE];fields={f['id']:f for f in progress_schema['fields']}
    need(set(fields)==set(SOURCE_FIELDS) and all(f['kind']=='int32' and type(f['default']) is int and f['default']==0 for f in fields.values()),'Target Progress is not exactly three typed zero defaults')
    target_doc=copy.deepcopy(original['document']);target_doc['revision']+=1;target_doc['component_schemas'][TYPE]=progress_schema
    for entity in ENTITIES:
        need(entity in original['document']['entities'] and any(e['id']==entity for e in old['entities']),'Addition target is not original authored and saved-live')
        target_doc['entities'][entity]['components']['game:'+TYPE]=copy.deepcopy(SOURCE_FIELDS)
    storage=args.output/'saves';shutil.copytree(run/'saves'/('slot-'+SOURCE_SLOT),storage/('slot-'+SOURCE_SLOT))
    source_slot=inventory(storage/('slot-'+SOURCE_SLOT));source_assets=inventory(run/'world.json.assets');source_managed=inventory(source_image.parent)
    record.update(retained_owner_sha256=sha(baseline/'owner.json'),retained_payload_sha256=SOURCE_PAYLOAD_SHA,retained_source_image_sha256=SOURCE_IMAGE_SHA,retained_source_assets=source_assets,retained_source_managed=source_managed,original_snapshot=original['snapshot'])

    def owner(label,document,assembly):
        world=args.output/(label+'.json');write_json(world,document);shutil.copytree(run/'world.json.assets',Path(str(world)+'.assets'))
        need(inventory(Path(str(world)+'.assets'))==source_assets,'Cooked source closure copy differs')
        game=Owner(args,assembly,world,storage,label,record,deadline);owners.append(game);return game

    source=owner('source-exact',original['document'],source_image)
    discovery=source.rpc('world.describe',dict(view='section',name='saves'))
    need(discovery['schema_revision']==81 and discovery['sections']['saves']['upgrades']['plan_versions']==[1,2,3]
        and discovery['sections']['saves']['upgrades']['add_components']['plan_version']==3,'Actual native host does not expose the reviewed v3 contract')
    record['discovery']=discovery
    source.restore_request(source.load_parameters())
    need(exact(source.snapshot('retained-exact'),original['snapshot']),'Current source exact restore changed original full snapshot')
    source.close()
    target=owner('target',target_doc,args.target_assembly);target.start();target.load();probe,probe_path,_=target.write('target-metadata')
    source_id,target_id=identity(original,original_path),identity(probe,probe_path);target_schema=probe['snapshot']['payload']['gameplay']['schema']
    source_schema=copy.deepcopy(old['gameplay']['schema']);new_schema=copy.deepcopy(target_schema)
    old_components=source_schema.pop('components');new_components=new_schema.pop('components')
    need(exact(source_schema,new_schema) and len(new_components)==len(old_components)+1,'Global schema changed or target-only type missing')
    component={s['id']:s for s in new_components};need(TYPE in component and all(exact(component[s['id']],s) for s in old_components),'Existing compiled component schema changed')
    # The native canonical authored schema carries the observed fingerprint.
    canonical_reply=target.rpc('component.schemas',dict(id=TYPE))
    need(len(canonical_reply['schemas'])==1,'Native Progress schema selection differs')
    canonical=canonical_reply['schemas'][0]
    need('fingerprint' in canonical,'Target native schema fingerprint unavailable; stop rather than guess')
    preserve=sorted(f['id'] for f in old['gameplay']['schema']['persistent']['fields'])
    plan=dict(format='poima.save-upgrade',version=3,id=fresh(),source=source_id,target=target_id,components=[],add_components=[dict(id=TYPE,target_fingerprint=canonical['fingerprint'],entities=ENTITIES)],**{'global':dict(preserve=preserve,retire=[],default=[])})
    plan_path=args.output/'approved-plan.json';write_json(plan_path,plan);record['plan']=plan;record['plan_sha256']=sha(plan_path)
    before=target.snapshot('before-rejections');fingerprint=target.fingerprint();world_pin=sha(target.world)

    def reject(label,chosen=plan,game=target):
        path=args.output/(label+'-plan.json');write_json(path,chosen)
        prior=game.snapshot(label+'-before');f=game.fingerprint();w=sha(game.world)
        failure=game.restore_request(game.load_parameters(plan=path),-32070)
        need(exact(game.snapshot(label+'-after'),prior) and exact(game.fingerprint(),f) and sha(game.world)==w and inventory(storage/('slot-'+SOURCE_SLOT))==source_slot,'Rejected addition mutated current complete state/world/source slot')
        record.setdefault('rejections',{})[label]=failure

    target.restore_request(target.load_parameters(),-32070)
    need(exact(target.snapshot('ordinary-image-rejection'),before) and exact(target.fingerprint(),fingerprint) and sha(target.world)==world_pin,'Ordinary changed-target selection was not inert')
    wrong=copy.deepcopy(plan);wrong['add_components'][0]['target_fingerprint']='0'*64;reject('wrong-fingerprint',wrong)
    unlisted=copy.deepcopy(plan);unlisted['add_components']=[];reject('unlisted-type',unlisted)
    missing=copy.deepcopy(plan);missing['add_components'][0]['entities']=ENTITIES[:1];reject('missing-caller-id',missing)
    for label in ('nondefault-target','missing-authored-id'):
        doc=copy.deepcopy(target_doc)
        if label=='nondefault-target':doc['entities'][ENTITIES[0]]['components']['game:'+TYPE][uid(1)]=1
        else:del doc['entities'][ENTITIES[0]]['components']['game:'+TYPE]
        alternate=owner(label,doc,args.target_assembly);alternate.start();alternate.load();saved,path,_=alternate.write(label+'-metadata')
        p=copy.deepcopy(plan);p['target']=identity(saved,path);reject(label,p,alternate);alternate.close()
    request=target.load_parameters(plan=plan_path);receipt=target.restore_request(request);upgraded=target.snapshot('immediate-no-tick');p=upgraded['payload']
    need(target.tick==140 and exact(target.values(),old['gameplay']['values']) and exact(target.progress(),[SOURCE_FIELDS,SOURCE_FIELDS]),'Upgrade ran a Tick or lost old globals/new defaults')
    need(receipt['upgrade']['added_components']==[dict(id=TYPE,instances=2,defaulted_fields=3)] and receipt['upgrade']['authored_added_components']==[dict(id=TYPE,authored_instances=2)],'Added-component reports differ')
    a,b=copy.deepcopy(old),copy.deepcopy(p)
    for key in ('content_sha256','authored_revision'):a.pop(key);b.pop(key)
    for key in ('assembly_sha256','schema'):a['gameplay'].pop(key);b['gameplay'].pop(key)
    rows=b['components']['types'];added=[r for r in rows if r['id']==TYPE];need(len(added)==1 and added[0]['fingerprint']==canonical['fingerprint'] and exact(added[0]['instances'],[dict(entity=e,values=[0,0,0]) for e in ENTITIES]),'New saved component row not exact typed defaults')
    b['components']['types']=[r for r in rows if r['id']!=TYPE];need(exact(a,b),'Upgrade changed an existing full native payload domain')
    need(p['content_sha256']==target_id['content_sha256'] and p['authored_revision']==target_doc['revision'] and p['gameplay']['assembly_sha256']==sha(args.target_assembly) and exact(p['gameplay']['schema'],target_schema),'Target identity binding differs')
    record['immediate_upgrade']=dict(receipt=receipt,snapshot=upgraded)
    plan_bytes=plan_path.read_bytes();plan_path.unlink()
    try:need(target.rpc('save.load',request)==dict(receipt,replayed=True) and exact(target.snapshot('exact-retry'),upgraded),'Retry reread missing plan or repeated activation')
    finally:plan_path.write_bytes(plan_bytes)
    target.restore_request(dict(request,new_session_id=fresh()),-32010)
    stale=target.load_parameters(plan=plan_path);stale['expected_tick']=target.tick-1;target.restore_request(stale,-32009)
    need(exact(target.snapshot('stale-and-collision'),upgraded),'Stale/collision request changed full state')
    driver=contract.RelayDriver(target,record);driver.step();need(exact(target.progress(),[{uid(1):1,uid(2):1,uid(3):0}]*2),'Actual typed Query/Get/Set did not update new component after first Tick')
    for index,field in enumerate(('CellTwo','CellThree'),1):
        driver.drive_to(*contract.CELL_APPROACHES[index],'remaining-'+str(index));values=driver.use_target(old['gameplay']['values'][field]);need(values['Collected']==index+1,'Real remaining pickup did not collect')
    driver.wait_for_delivery();driver.follow_waypoints(((5.2,5.2),(5.2,1.4)),'terminal');final=driver.use_target(contract.TERMINAL)
    need(final['Won']==1 and final['Arrivals']==1 and final['Collected']==3,'Real partial game cannot finish')
    progress=target.progress();need(exact(progress,[{uid(1):target.tick-140,uid(2):3,uid(3):1}]*2),'Progress does not reflect real callback ticks/objective')
    completed,_,completed_receipt=target.write('component-upgraded-complete');record['completed']=dict(snapshot=completed['snapshot'],receipt=completed_receipt,values=final,progress=progress);target.close()
    reopened=owner('fresh-process',target_doc,args.target_assembly);reopened.restore_request(reopened.load_parameters('component-upgraded-complete'))
    need(exact(reopened.snapshot('fresh-exact'),completed['snapshot']),'Fresh native process did not restore complete target checkpoint exactly')
    follow=contract.RelayDriver(reopened,record);follow.control('menu');follow.control('refresh');follow.control('close');follow.step(5)
    need(reopened.values()['Won']==1 and reopened.values()['Collected']==3 and exact(reopened.progress(),[{uid(1):progress[0][uid(1)]+5,uid(2):3,uid(3):1}]*2),'Fresh callbacks did not continue new persistent component state')
    record['fresh_continuation']=reopened.snapshot('after-real-callbacks');reopened.close()
    need(inventory(storage/('slot-'+SOURCE_SLOT))==source_slot and inventory(run/'world.json.assets')==source_assets and inventory(source_image.parent)==source_managed,'Retained checkpoint/cooked/image closure changed')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','baseline','target-assembly','target-manifest','hostfxr','bridge','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();need(os.name=='nt' and not sys.flags.optimize,'Use native Windows Python inside caller owned/gated Job')
    for key in ('binary','baseline','target_assembly','target_manifest','hostfxr','bridge'):setattr(args,key,getattr(args,key).resolve(strict=True))
    args.output=args.output.resolve();need(not args.output.exists() and not args.output.is_relative_to(args.baseline) and not args.output.is_relative_to(args.target_assembly.parent),'Fresh output outside immutable inputs required')
    args.output.mkdir(parents=True);started=time.monotonic();deadline=started+900;owners=[]
    inputs=[Path(__file__),Path(contract.__file__),Path(raw_json.__file__),ROOT/'examples/relay-yard/run.py',ROOT/'examples/locomotion-yard/run.py',args.binary,args.target_assembly,args.target_manifest,args.hostfxr,args.bridge]
    inputs.extend(p for p in (ROOT/'tools/python/poima_client').glob('*') if p.is_file() and p.suffix in ('.py','.json'))
    for directory in {args.target_assembly.parent,args.bridge.parent,args.baseline/'run/compiled/a/managed'}:
        inputs.extend(p for p in directory.iterdir() if p.is_file() and (p.suffix=='.dll' or p.name.endswith(('.deps.json','.runtimeconfig.json'))))
    inputs.extend(p for p in (args.baseline/'run/source/a').iterdir() if p.is_file())
    inputs.extend([args.baseline/'owner.json',args.baseline/'run/saves/slot-iteration-a/current.json',args.baseline/'run/saves/slot-iteration-a/p-ed3a592935868be5108a38e327f32276.bin'])
    pins={str(p.resolve()):sha(p) for p in inputs};record=dict(passed=False,calls=[],owners=[],source_pins=pins,scope='Retained .98 CoreCLR partial checkpoint; explicit v3 target-only type defaults on two permanent authored entities; no graphics/physical input/latency/NativeAOT claim',cleanup_errors=[])
    args.ledger=SettledCalls(args.output/'calls.jsonl')
    try:qualify(args,record,owners,deadline);record['passed']=True
    except BaseException as error:
        record['error']=traceback.format_exc()
        failure_receipt(record,args.output,'qualification',error)
    finally:
        for owner in reversed(owners):
            try:owner.close()
            except BaseException as error:
                record['cleanup_errors'].append(traceback.format_exc());failure_receipt(record,args.output,'native_owner_cleanup',error)
        try:
            args.ledger.close()
            record['rpc_ledger']=args.ledger.verify(record['calls'])
            record['changed_inputs']=[p for p,pin in pins.items() if not Path(p).is_file() or sha(p)!=pin]
            record['seconds']=time.monotonic()-started
            record['retained_bytes_before_evidence']=sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())
            need(not record['changed_inputs'] and not record['cleanup_errors'] and all(o['exit_code']==0 for o in record['owners'])
                and record['seconds']<=900 and record['retained_bytes_before_evidence']<=128*1048576-16*1048576,
                'Final source/owner/time/output closure failed')
            encoded=(json.dumps(record,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
            need(len(encoded)<=16*1048576,'Summary evidence cap exceeded')
            (args.output/'evidence.json').write_bytes(encoded)
            need(time.monotonic()-started<=900,'Whole deadline expired during evidence publication')
        except BaseException as error:
            # Never replace the first qualification/native-call error with a
            # final encoder/budget failure. This small receipt is unconditional.
            failure_receipt(record,args.output,'final_retention',error)
            summary=dict(passed=False,first_failure=record['first_failure'],
                secondary_failures=record.get('secondary_failures',[])[:8],owners=record['owners'],
                cleanup_errors=record['cleanup_errors'][:8],seconds=time.monotonic()-started,
                rpc_partial=dict(rows=args.ledger.count,bytes=args.ledger.bytes,
                    sha256=args.ledger.hash.hexdigest(),failed=args.ledger.failed,closed=args.ledger.closed))
            encoded=(json.dumps(summary,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
            need(len(encoded)<=256*1024,'Small failed evidence cap exceeded')
            (args.output/'evidence.json').write_bytes(encoded)
    print(json.dumps(dict(passed=record['passed'],error=record.get('error'))));return 0 if record['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
