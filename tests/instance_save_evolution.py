#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Evolve an actual retained .78 imported-character v6 save using compiled games.

Run with OS-native Python and engine/config paths. No builds, downloads, source
imports, raw model changes or clock/state patches occur. Caller worlds, closures,
compiled artifacts and save roots stay read-only. All writes use a new output.
An optional actual Vulkan capture compares source restore and target upgrade at
the identical saved tick. It reports actual draw counts and checks capture/output
plumbing; the retained camera does not establish character visibility. It is not
an independent FBX decoder or a rendering-quality benchmark.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

TYPE='eeeeeeeeeeeeeeeeeeeeeeeeeeee7810'
GAME='Poima.Tests.ManagedInstanceGame'
IDENTITY='poima.test.managed-instances'
TEMPLATE,LOCAL_ROOT,LOCAL_RIG=('eeeeeeeeeeeeeeeeeeeeeeeeeeee7801',
    'eeeeeeeeeeeeeeeeeeeeeeeeeeee7802','eeeeeeeeeeeeeeeeeeeeeeeeeeee7803')
FIXTURE=ROOT/'tests/fixtures/instance_save_evolution'
GLOBAL_BASE=0xa7900000000000000000000000000000
ZERO='0'*32
uid=lambda value:f'{value:032x}'

def check(value,message):
    if not value:raise AssertionError(message)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False,allow_nan=False)
def digest(value):return hashlib.sha256(canonical(value).encode('utf-8')).hexdigest()
def raw_text(path,limit=64*1024*1024):
    path=Path(path);check(path.is_file() and not path.is_symlink() and path.stat().st_size<=limit,'Input is missing, linked or too large: '+str(path))
    return path.read_bytes().decode('utf-8')
def read(path,limit=64*1024*1024):
    return json.loads(raw_text(path,limit))
def json_span(text,path):
    """Find a complete member's original JSON token span without reencoding it.

    Stored snapshot checksums cover the native writer's exact payload bytes.
    Python/native shortest-roundtrip float spellings can differ, so decoding is
    used only to walk token boundaries, never to recreate floating-point data.
    Paths contain object keys or nonnegative array indices. Every visited
    container is scanned fully to reject duplicate/ambiguous keys and bad tails.
    """
    check(isinstance(text,str) and len(text.encode('utf-8'))<=64*1024*1024 and len(path)<=32,'Raw JSON/span budget exceeded')
    decoder=json.JSONDecoder()
    def whitespace(at):
        while at<len(text) and text[at] in ' \t\r\n':at+=1
        return at
    start=whitespace(0);_,end=decoder.raw_decode(text,start)
    check(whitespace(end)==len(text),'Raw JSON has trailing data')
    for selector in path:
        at=start;selected=None
        if isinstance(selector,str):
            check(text[at]=='{','Raw member path requires an object');at=whitespace(at+1);seen=set()
            while at<end and text[at]!='}':
                key,next_at=decoder.raw_decode(text,at);check(isinstance(key,str) and key not in seen,'Raw JSON has invalid/duplicate key');seen.add(key)
                at=whitespace(next_at);check(at<end and text[at]==':','Raw JSON object lacks colon');begin=whitespace(at+1)
                _,finish=decoder.raw_decode(text,begin)
                if key==selector:selected=(begin,finish)
                at=whitespace(finish);check(at<end and text[at] in ',}','Raw JSON object lacks delimiter')
                if text[at]=='}':break
                at=whitespace(at+1);check(at<end and text[at]!='}','Raw JSON has trailing comma')
            check(at==end-1 and text[at]=='}','Raw JSON object span is incomplete')
        else:
            check(type(selector) is int and selector>=0 and text[at]=='[','Raw member path requires a nonnegative array index');at=whitespace(at+1);index=0
            while at<end and text[at]!=']':
                begin=at;_,finish=decoder.raw_decode(text,begin)
                if index==selector:selected=(begin,finish)
                index+=1;check(index<=2000000,'Raw JSON array budget exceeded')
                at=whitespace(finish);check(at<end and text[at] in ',]','Raw JSON array lacks delimiter')
                if text[at]==']':break
                at=whitespace(at+1);check(at<end and text[at]!=']','Raw JSON has trailing comma')
            check(at==end-1 and text[at]==']','Raw JSON array span is incomplete')
        check(selected is not None,'Raw JSON path is absent: '+str(selector));start,end=selected
    return start,end
def raw_digest(text,path):
    begin,end=json_span(text,path);return hashlib.sha256(text[begin:end].encode('utf-8')).hexdigest()
def replace_member(text,path,value):
    begin,end=json_span(text,path);return text[:begin]+canonical(value)+text[end:]
def inventory(path):
    path=Path(path);rows={};total=0
    check(path.is_dir() and not path.is_symlink(),'Inventory must be an ordinary directory')
    for item in sorted(path.rglob('*')):
        check(not item.is_symlink(),'Linked input is not supported: '+str(item))
        if item.is_file():
            total+=item.stat().st_size;check(total<=256*1024*1024,'Input inventory exceeds256MiB')
            rows[item.relative_to(path).as_posix()]={'bytes':item.stat().st_size,'sha256':sha(item)}
    return rows
def configuration(path):
    path=path.resolve(strict=True);value=read(path,1024*1024)
    for key in ('hostfxr','bridge','assembly','descriptor'):
        if key in value:
            selected=Path(value[key]);value[key]=str((selected if selected.is_absolute() else path.parent/selected).resolve(strict=True))
    check(value.get('type',GAME)==GAME,'Config must select retained game type')
    check(('descriptor' in value)!=('assembly' in value),'Config requires descriptor OR assembly')
    if 'assembly' in value:
        check('bridge' in value and 'hostfxr' in value,'CoreCLR needs bridge/hostfxr');value.setdefault('type',GAME)
    else:value.pop('type',None)
    return value
def compiled(config):
    rows={}
    if 'descriptor' in config:
        descriptor=Path(config['descriptor']);value=read(descriptor,1024*1024);rows[str(descriptor)]=sha(descriptor)
        check(value['type']==GAME and value['identity']==IDENTITY,'Artifact selects another game')
        for item in value['files']:
            relative=Path(item['path']);check(not relative.is_absolute() and '..' not in relative.parts,'Artifact file escapes closure')
            path=(descriptor.parent/relative).resolve(strict=True)
            check(path.is_relative_to(descriptor.parent) and path.stat().st_size==item['size'] and sha(path)==item['sha256'],'Artifact closure is not intact')
            rows[str(path)]=sha(path)
    else:
        for key in ('hostfxr','bridge','assembly'):
            path=Path(config[key]);rows[str(path)]=sha(path)
        for key in ('bridge','assembly'):
            path=Path(config[key]).parent/'Poima.Gameplay.dll';check(path.is_file(),'Matching SDK closure is missing');rows[str(path)]=sha(path)
    return rows
def image(config):
    return Path(config['descriptor']).parent/read(config['descriptor'])['library'] if 'descriptor' in config else Path(config['assembly'])
def stored(root,slot):
    folder=root/('slot-'+slot);manifest=read(folder/'current.json',1024*1024)
    check(manifest['format']=='poima.save-slot' and manifest['version']==1,'Unknown save slot envelope')
    entry=manifest['payload']['current'];name=Path(entry['file'])
    check(name.name==str(name) and name.suffix=='.bin','Unsafe slot payload name')
    path=folder/name;check(path.stat().st_size==entry['bytes'] and sha(path)==entry['sha256'],'Retained slot payload differs')
    raw=raw_text(path);value=json.loads(raw);check(value['format']=='poima.world-save' and value['version']==1,'Unknown world save')
    snapshot=value['snapshot'];check(snapshot['sha256']==raw_digest(raw,('snapshot','payload')),'Snapshot raw-payload checksum differs')
    return value,path,entry
def identity(save):
    payload=save['snapshot']['payload'];game=payload['gameplay']
    return dict(world_id=payload['world_id'],content_sha256=payload['content_sha256'],backend=game['backend'],
        module_identity=game['schema']['identity'],type=game['type'],image_sha256=game['assembly_sha256'],schema_sha256=digest(game['schema']))
def schema(manifest,capacity):
    check(manifest['format']=='poima.components' and manifest['version']==1 and len(manifest['schemas'])==1,'Target manifest must contain one component')
    value=manifest['schemas'][0];fields={item['id']:item for item in value['fields']}
    check(value['id']==TYPE and value['version']==2 and set(fields)=={uid(n) for n in (1,3,4,5,6)},'Target component field IDs differ')
    check(fields[uid(1)]['kind']=='int32' and fields[uid(1)]['default']==999 and fields[uid(3)]['kind']=='entity','Retained target scalar declarations differ')
    check(fields[uid(4)]['kind']=='array' and fields[uid(4)]['element_kind']=='entity' and fields[uid(4)]['capacity']==capacity,'Target entity array extent differs')
    check(fields[uid(5)]['kind']=='int32' and fields[uid(5)]['default']==37 and fields[uid(6)]['kind']=='array' and fields[uid(6)]['element_kind']=='int32' and fields[uid(6)]['capacity']==2 and fields[uid(6)]['default']==[],'Added scalar/empty array differ')
    return value
def target_document(original,target_schema):
    result=copy.deepcopy(original);check(set(result['component_schemas'])=={TYPE},'Source component membership differs')
    result['component_schemas'][TYPE]=target_schema;result['revision']+=1
    def cells(entity):
        bag=entity['components'];key='game:'+TYPE
        if key in bag:
            old=bag[key];check(set(old)=={uid(n) for n in (1,2,3,4)},'Source custom fields differ')
            bag[key]={uid(1):old[uid(1)],uid(3):old[uid(3)],uid(4):old[uid(4)],uid(5):37,uid(6):[]}
    for entity in result['entities'].values():cells(entity)
    for recipe in result['templates'].values():
        if 'entities' in recipe:
            for entity in recipe['entities'].values():cells(entity)
        else:cells(recipe)
    return result
def projection(snapshot):
    result=copy.deepcopy(snapshot['payload'])
    for name in ('ObservedEpochHigh','ObservedEpochLow'):result['gameplay']['values'].pop(name,None)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','source-world','source-saves','source-config','target-config','target-manifest','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--slot',default='hierarchy');parser.add_argument('--timeout',type=float,default=1200)
    parser.add_argument('--overflow-target-config',type=Path);parser.add_argument('--overflow-target-manifest',type=Path)
    parser.add_argument('--gpu',type=int,choices=(0,1));parser.add_argument('--capture',action='store_true')
    args=parser.parse_args();check(not sys.flags.optimize,'Do not run optimized Python')
    check(math.isfinite(args.timeout) and 60<=args.timeout<=1800,'Timeout must be finite60..1800seconds')
    check(bool(args.overflow_target_config)==bool(args.overflow_target_manifest),'Supply both overflow config and manifest')
    check(not args.capture or args.gpu is not None,'Capture requires explicit hardware GPU')
    check(not args.output.exists(),'Output must be new');output=args.output.resolve()
    # Reject before creating anything: a new child of a caller-owned save/asset
    # root is still a write to that input closure.
    for protected_root in (args.source_saves.resolve(strict=True),Path(str(args.source_world.resolve(strict=True))+'.assets').resolve(strict=True)):
        check(output!=protected_root and not output.is_relative_to(protected_root),'Output must be disjoint from caller save/asset closures')
    output.mkdir(parents=True)
    record=dict(passed=False,calls=[],owners=[],checks=[],cleanup_errors=[],runner_sha256=sha(__file__),
        limitations=['Explicit one-edge compiled hierarchy/collection evolution; no automatic migration or backend conversion.',
            'Exact saved native/skin hierarchy preservation and source-free cooked closure; not an independent raw FBX decoder.',
            'Optional source/target Vulkan pixels check capture/output plumbing at one tick; original character visibility is not qualified.',
            'No graphics-quality, physical-input or independent original-FBX image comparison claim.'],
        continuation_exclusions=['gameplay.values.ObservedEpochHigh','gameplay.values.ObservedEpochLow'])
    owners=[];protected={};deadline=time.monotonic()+args.timeout;finished=False
    def remain():
        value=deadline-time.monotonic();check(value>0,'Overall verifier deadline expired');return min(90,value)
    class Owner:
        def __init__(self,label,world,saves,config,trap=False):
            self.label=label;self.world=world;self.saves=saves;self.config=config;self.session=None;self.tick=0;self.closed=False
            old=os.environ.get('POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE');os.environ['POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE']='1' if trap else '0'
            try:self.client=WorldClient.open(str(args.binary.resolve(strict=True)),str(world),close_timeout=20)
            finally:
                if old is None:os.environ.pop('POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE',None)
                else:os.environ['POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE']=old
            self.row={'label':label,'process_id':self.client.transport.process_id,'exit_code':None};record['owners'].append(self.row);owners.append(self)
            self.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(saves)))
        def rpc(self,method,params=None,error=None):
            row=dict(owner=self.label,method=method,params=params or {});record['calls'].append(row)
            try:result=self.client.call(method,params or {},timeout=remain());row['result']=result
            except RpcError as failure:
                row['error']=dict(code=failure.code,message=failure.message,data=failure.data)
                check(error is not None and (error is True or failure.code==error),'Unexpected RPC rejection: '+str(row));return row['error']
            check(error is None,'Expected RPC rejection: '+method);return result
        def runtime(self):return self.rpc('runtime.inspect',dict(session_id=self.session))
        def game(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
        def guards(self):
            if not self.session:return dict(expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,
                expected_component_revision=None,expected_structure_revision=None,expected_ui_revision=None,expected_control_sequence=None)
            state=self.runtime();game=self.game();components=self.rpc('runtime.components',dict(session_id=self.session))
            return dict(expected_session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],
                expected_component_revision=components['component_revision'],expected_structure_revision=state['structure_revision'],
                expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'])
        def load_params(self,slot,generation=1,upgrade=None):
            value=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=generation,
                revision=self.rpc('world.inspect')['revision'],new_session_id=uuid.uuid4().hex,gameplay=self.config,**self.guards())
            if upgrade:value['upgrade']=dict(path=str(upgrade),expected_sha256=sha(upgrade))
            return value
        def load(self,params,error=None):
            value=self.rpc('save.load',params,error)
            if error is None:self.session=params['new_session_id'];self.tick=value['tick']
            return value
        def start(self):
            self.session=uuid.uuid4().hex;self.tick=0;revision=self.rpc('world.inspect')['revision']
            self.rpc('runtime.start',dict(session_id=self.session,revision=revision))
            return self.rpc('runtime.gameplay.load_native' if 'descriptor' in self.config else 'runtime.gameplay.load',
                dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,
                    expected_structure_revision=0,**self.config))
        def write(self,slot,generation=0):
            guards=self.guards();guards['session_id']=guards.pop('expected_session_id')
            value=self.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=generation,**guards))
            saved,path,entry=stored(self.saves,slot);check(value['sha256']==entry['sha256'],'Saved payload response differs');return saved
        def snapshot(self,label):
            value=self.write('probe-'+uuid.uuid4().hex);record.setdefault('snapshots',{})[self.label+'-'+label]=value['snapshot'];return value['snapshot']
        def step(self,count,error=None):
            state=self.runtime();value=self.rpc('runtime.step',dict(request_id=uuid.uuid4().hex,session_id=self.session,
                expected_tick=self.tick,expected_structure_revision=state['structure_revision'],ticks=count),error)
            if error is None:self.session=value['current_session_id'];self.tick=value['current_tick']
            return value
        def control(self,name):
            state=self.runtime();game=self.game()
            # The explicit fixture mode is an input to the compiled Tick. This
            # works on headless builds with no native UI and never patches poses,
            # clocks, entity membership or resolved handles.
            return self.rpc('runtime.gameplay.edit',dict(request_id=uuid.uuid4().hex,session_id=self.session,expected_tick=self.tick,
                expected_revision=game['revision'],expected_structure_revision=state['structure_revision'],values={'Mode':{'repair':5,'birth':1}[name]}))
        def instance(self,root):
            return self.rpc('runtime.instance',dict(session_id=self.session,tick=self.tick,id=root,expected_structure_revision=self.runtime()['structure_revision']))
        def capture(self,label):
            state=self.runtime();path=output/(label+'.bmp');camera=next(key for key,value in read(self.world)['entities'].items() if 'Camera' in value['components'])
            value=self.rpc('runtime.capture',dict(session_id=self.session,tick=self.tick,ui_revision=state['ui_revision'],camera=camera,
                path=str(path),width=960,height=540,samples=4,gpu=args.gpu,ui_scale=1,profile=True))
            check(value['hardware'] and value['capture_written'] and value['nvrhi_errors']==0 and self.runtime()==state,'Capture failed or advanced simulation')
            draws=value['render_diagnostics']['last_draws']
            observed={key:draws[key] for key in ('camera_draws','camera_triangles','skinned_instances','skinned_vertices')}
            check(all(type(count) is int and count>=0 for count in observed.values()),'Capture draw counts are malformed')
            record.setdefault('captures',{})[label]=dict(response=value,sha256=sha(path),observed_geometry=observed,
                actor_visibility_qualified=False)
            return path
        def close(self):
            if self.closed:return
            self.closed=True
            try:self.client.close()
            finally:
                transport=self.client.transport;self.row.update(exit_code=transport.returncode,stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated)
            check(self.row['exit_code']==0 and not self.row['stderr'] and not self.row['stderr_truncated'],'Native owner did not exit cleanly: '+self.label)
    try:
        for name in ('binary','source_world','source_config','target_config','target_manifest'):
            path=getattr(args,name).resolve(strict=True);setattr(args,name,path);protected[name]=(path,sha(path))
        args.source_saves=args.source_saves.resolve(strict=True)
        assets=Path(str(args.source_world)+'.assets');protected['source_saves']=(args.source_saves,inventory(args.source_saves));protected['source_assets']=(assets,inventory(assets))
        src_config=configuration(args.source_config);dst_config=configuration(args.target_config)
        check(('descriptor' in src_config)==('descriptor' in dst_config),'Upgrade may not change gameplay backend')
        record['backend']='native_aot' if 'descriptor' in dst_config else 'coreclr';record['compiled_inputs']={'source':compiled(src_config),'target':compiled(dst_config)}
        original,original_path,original_entry=stored(args.source_saves,args.slot)
        check(original['snapshot']['version']==6 and len(original['snapshot']['payload']['structure']['spawned'])==2,'Supply actual retained two-instance v6 save')
        source_identity=identity(original);check(source_identity['type']==GAME and source_identity['module_identity']==IDENTITY and source_identity['image_sha256']==sha(image(src_config)),'Source compiled image/identity differs from retained save')
        source_doc=original['document'];target_schema=schema(read(args.target_manifest),4);target_doc=target_document(source_doc,target_schema)
        fixture_sources={str(path.relative_to(ROOT)):sha(path) for path in sorted(FIXTURE.rglob('*'))
            if path.is_file() and path.suffix in ('.cs','.csproj','.md') and not any(part in ('bin','obj') for part in path.parts)}
        record['fixture_sources']=fixture_sources
        worlds=[output/'source.json',output/'target.json']
        for path,document in zip(worlds,(source_doc,target_doc)):
            path.write_text(json.dumps(document,indent=2,ensure_ascii=False)+'\n',encoding='utf-8');shutil.copytree(assets,Path(str(path)+'.assets'))
        saves=output/'saves';shutil.copytree(args.source_saves,saves)
        record['retained_source']=dict(world_sha256=sha(args.source_world),payload_sha256=sha(original_path),entry=original_entry,identity=source_identity,
            snapshot_version=6,tick=original['snapshot']['payload']['tick'],closure=inventory(assets),saved_document_sha256=digest(source_doc))
        source=Owner('retained-source',worlds[0],saves,src_config)
        status=source.rpc('save.inspect',dict(slot=args.slot));check(status['selected']['sha256']==original_entry['sha256'],'Save owner did not select retained payload')
        source.load(source.load_params(args.slot,original_entry['generation']));source_snapshot=source.snapshot('exact-retained')
        check(source_snapshot==original['snapshot'],'Retained source exact restoration changed logical snapshot')
        if args.capture:source_pixels=source.capture('retained-source')
        source.close()
        probe=Owner('target-probe',worlds[1],saves,dst_config);probe.start();target_probe=probe.write('target-metadata');probe.close()
        target_identity=identity(target_probe);check(target_identity['image_sha256']==sha(image(dst_config)),'Actual target module image differs')
        source_fields=original['snapshot']['payload']['gameplay']['schema']['fields']
        legacy=[dict(name=field['name'],id=uid(GLOBAL_BASE+i)) for i,field in enumerate(source_fields,1)]
        globals_preserved=[item['id'] for item in legacy];global_defaults=[uid(GLOBAL_BASE+i) for i in (128,129)]
        persistent={field['id']:field for field in target_probe['snapshot']['payload']['gameplay']['schema']['persistent']['fields']}
        check(set(persistent)==set(globals_preserved+global_defaults),'Actual target persistent global IDs differ')
        plan=dict(format='poima.save-upgrade',version=2,id=uuid.uuid4().hex,source=source_identity,target=target_identity,
            **{'global':dict(preserve=globals_preserved,retire=[],default=global_defaults),'legacy_global_ids':legacy,
               'components':[dict(id=TYPE,source_fingerprint=source_doc['component_schemas'][TYPE]['fingerprint'],target_fingerprint=target_schema['fingerprint'],
                   preserve=[uid(n) for n in (1,3,4)],retire=[uid(2)],default=[uid(5),uid(6)],
                   array_capacity=[dict(id=uid(4),source_capacity=2,target_capacity=4,overflow='reject')])]})
        plan_path=output/'approved-upgrade.json';plan_path.write_text(json.dumps(plan,indent=2)+'\n',encoding='utf-8')
        record.update(source_identity=source_identity,target_identity=target_identity,plan=plan,target_document_sha256=digest(target_doc))
        target=Owner('target-upgrade',worlds[1],saves,dst_config,trap=True)
        target.session=uuid.uuid4().hex;target.rpc('runtime.start',dict(session_id=target.session,revision=target_doc['revision']))
        trap=target.rpc('runtime.gameplay.load_native' if 'descriptor' in dst_config else 'runtime.gameplay.load',
            dict(session_id=target.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,expected_structure_revision=0,**dst_config),True)
        check('Initialize forbidden during instance save upgrade.' in trap['message'] and target.game()['module'] is None,'Initialize trap was not positively established')
        record['initialize_guard']=dict(positive_control=True,error=trap)
        before=target.snapshot('before-rejections')
        target.load(target.load_params(args.slot,original_entry['generation']),True)
        check(target.snapshot('after-exact-rejection')==before,'Exact-schema rejection changed runtime')
        for name,change in (('wrong-image',lambda value:value['target'].__setitem__('image_sha256','f'*64)),
                            ('wrong-capacity-declaration',lambda value:value['components'][0]['array_capacity'][0].__setitem__('source_capacity',3))):
            altered=copy.deepcopy(plan);altered['id']=uuid.uuid4().hex;change(altered);path=output/(name+'.json');path.write_text(json.dumps(altered,indent=2)+'\n')
            failure=target.load(target.load_params(args.slot,original_entry['generation'],path),-32070)
            check(target.snapshot('after-'+name)==before,'Failed explicit upgrade changed active runtime');record.setdefault('rejections',{})[name]=failure
        request=target.load_params(args.slot,original_entry['generation'],plan_path);receipt=target.load(request)
        upgraded=target.snapshot('immediate-upgrade');payload=upgraded['payload'];old=original['snapshot']['payload']
        unchanged=[key for key in old if key not in ('content_sha256','authored_revision','gameplay','components')]
        check(all(payload[key]==old[key] for key in unchanged),'Upgrade changed non-mapped native snapshot fields')
        expected_globals=copy.deepcopy(old['gameplay']['values']);expected_globals['SimulationTicks']=expected_globals.pop('Ticks');expected_globals.update(EvolutionTicks=0,ReferencesChecked=0)
        check(payload['gameplay']['values']==expected_globals and payload['gameplay']['schema']==target_probe['snapshot']['payload']['gameplay']['schema'],'Immediate global values/schema differ from independent mapping')
        expected_components=copy.deepcopy(old['components']);row=expected_components['types'][0];row['fingerprint']=target_schema['fingerprint']
        for instance in row['instances']:
            previous=instance['values'];check(len(previous)==4 and len(previous[3])==2,'Source compact array differs');instance['values']=[previous[0],previous[2],previous[3],37,[]]
        check(payload['components']==expected_components,'Immediate component values differ from independent field mapping')
        check(payload['content_sha256']==target_identity['content_sha256'] and payload['authored_revision']==target_doc['revision'] and
              payload['gameplay']['assembly_sha256']==target_identity['image_sha256'],'Target identity publication differs')
        record['immediate_allowlist']=dict(unchanged_sections=unchanged,mapped_globals=expected_globals,mapped_components=expected_components)
        if args.capture:
            from scene_capture import pixels
            upgraded_pixels=target.capture('target-upgrade');a,b=pixels(source_pixels),pixels(upgraded_pixels)
            source_draws=record['captures']['retained-source']['observed_geometry'];target_draws=record['captures']['target-upgrade']['observed_geometry']
            check(a==b and source_draws==target_draws,'Source/target retained-camera output or observed draw counts changed')
            record['same_tick_pixel_comparison']=dict(exact=True,source_sha256=sha(source_pixels),target_sha256=sha(upgraded_pixels),
                source_draws=source_draws,target_draws=target_draws,actor_visual_preservation_qualified=False,
                scope='Same-tick Vulkan capture/context/output preservation; no independent character visibility oracle.',
                observation='Both captures draw zero meshes and zero skinned instances.' if source_draws['camera_draws']==0 and source_draws['skinned_instances']==0
                    else 'Draw counts are reported; visibility of each original character has not been independently established.')
        plan_path.unlink();replayed=target.rpc('save.load',request)
        check(replayed==dict(receipt,replayed=True) and target.snapshot('after-removed-plan-retry')==upgraded,'Retained receipt re-applied upgrade or reread missing plan')
        changed=copy.deepcopy(request);changed['new_session_id']=uuid.uuid4().hex;target.rpc('save.load',changed,-32010)
        target.step(1);first=target.snapshot('compiled-first-tick');values=first['payload']['gameplay']['values']
        check(values['EvolutionTicks']==1 and values['ReferencesChecked']==2 and values['SimulationTicks']==expected_globals['SimulationTicks']+1,'Actual target callback did not validate/read evolved state')
        for row in first['payload']['components']['types'][0]['instances']:
            value=row['values'];check(value[0] in (11,22) and len(value[2])==4 and value[2][:2]==value[2][2:] and value[3]==38 and value[4]==[10,20],'Compiled target did not mutate grown/defaulted collections')
        # Rejection isolation with a fully loaded, already advanced target game.
        live_bad=copy.deepcopy(plan);live_bad['id']=uuid.uuid4().hex;live_bad['target']['image_sha256']='e'*64
        live_bad_path=output/'live-wrong-edge.json';live_bad_path.write_text(json.dumps(live_bad,indent=2)+'\n')
        failure=target.load(target.load_params(args.slot,original_entry['generation'],live_bad_path),-32070)
        check(target.snapshot('live-after-wrong-edge')==first,'Wrong edge changed the active compiled target')
        stale=target.load_params(args.slot,original_entry['generation'],live_bad_path);stale['expected_tick']-=1
        target.load(stale,-32009);check(target.snapshot('live-after-stale-guard')==first,'Stale load guard changed active compiled state')
        check(sha(stored(saves,args.slot)[1])==original_entry['sha256'],'Rejected live replacement modified the retained slot')
        record['active_target_rejections']=dict(wrong_edge=failure,stale_guard_unchanged=True)
        checkpoint_save=target.write('evolved');checkpoint=checkpoint_save['snapshot'];target.step(17);expected=target.snapshot('grouped-17')
        target.load(target.load_params('evolved'));check(projection(target.snapshot('same-owner-restored'))==projection(checkpoint),'Same-owner exact target restore changed state')
        for count in (3,1,7,6):target.step(count)
        check(projection(target.snapshot('unequal-17'))==projection(expected),'Unequal tick groups changed target continuation')
        target.load(target.load_params('evolved'));target.control('repair');target.step(1);retired=target.snapshot('retired-first');old_first=checkpoint['payload']['gameplay']['values']['First']
        check(retired['payload']['gameplay']['values']['First']==ZERO and len(retired['payload']['structure']['spawned'])==1,'Whole instance retirement failed')
        target.rpc('runtime.instance',dict(session_id=target.session,tick=target.tick,id=old_first,expected_structure_revision=target.runtime()['structure_revision']),-32004)
        target.control('birth');target.step(1);reborn=target.snapshot('reborn-first');new_first=reborn['payload']['gameplay']['values']['First']
        check(new_first!=old_first and int(new_first,16)>=int(checkpoint['payload']['structure']['next_entity_id'],16) and len(reborn['payload']['structure']['spawned'])==2,'Rebirth reused retired identities or lost frontier')
        target.step(2);reborn_continued=target.snapshot('reborn-continued');target.close()
        fresh=Owner('fresh-target',worlds[1],saves,dst_config,trap=True);fresh.load(fresh.load_params('evolved'))
        check(projection(fresh.snapshot('fresh-restored'))==projection(checkpoint),'Fresh-owner target exact restore differs')
        for _ in range(17):fresh.step(1)
        check(projection(fresh.snapshot('single-17'))==projection(expected),'Fresh single-tick continuation differs')
        fresh.load(fresh.load_params('evolved'));fresh.control('repair');fresh.step(1);fresh.control('birth');fresh.step(1);fresh.step(2)
        check(projection(fresh.snapshot('fresh-reborn'))==projection(reborn_continued),'Fresh retirement/rebirth changed allocator/native continuation')
        fresh.close()
        # An authorized retirement must not conceal invalid SOURCE references.
        # Mutate only an owned negative slot, with complete storage checksums,
        # so ordinary source validation (not a checksum failure) rejects it.
        invalid_saves=output/'invalid-source-saves';shutil.copytree(args.source_saves,invalid_saves)
        invalid,invalid_payload,invalid_entry=stored(invalid_saves,args.slot)
        invalid_raw=replace_member(raw_text(invalid_payload),('snapshot','payload','components','types',0,'instances',0,'values',1),'f'*32)
        invalid_raw=replace_member(invalid_raw,('snapshot','sha256'),raw_digest(invalid_raw,('snapshot','payload')))
        invalid_payload.write_bytes(invalid_raw.encode('utf-8'))
        manifest_path=invalid_payload.parent/'current.json';manifest_raw=raw_text(manifest_path,1024*1024);invalid_manifest=json.loads(manifest_raw);slot_payload=invalid_manifest['payload']
        manifest_raw=replace_member(manifest_raw,('payload','current','sha256'),sha(invalid_payload))
        manifest_raw=replace_member(manifest_raw,('payload','current','bytes'),invalid_payload.stat().st_size)
        for index,prior in enumerate(slot_payload['receipts']):
            if prior['generation']==invalid_entry['generation']:
                manifest_raw=replace_member(manifest_raw,('payload','receipts',index,'sha256'),sha(invalid_payload))
                manifest_raw=replace_member(manifest_raw,('payload','receipts',index,'bytes'),invalid_payload.stat().st_size)
        manifest_raw=replace_member(manifest_raw,('sha256',),raw_digest(manifest_raw,('payload',)))
        manifest_path.write_bytes(manifest_raw.encode('utf-8'))
        restored_plan=output/'source-validation-plan.json';restored_plan.write_text(json.dumps(plan,indent=2)+'\n')
        negative=Owner('invalid-retired-source-reference',worlds[1],invalid_saves,dst_config,trap=True)
        negative.session=uuid.uuid4().hex;negative.rpc('runtime.start',dict(session_id=negative.session,revision=target_doc['revision']))
        before_invalid=negative.snapshot('before-invalid-source')
        selected=negative.rpc('save.inspect',dict(slot=args.slot));check(selected['selected']['sha256']==sha(invalid_payload),'Negative slot checksum did not reach source validation')
        failure=negative.load(negative.load_params(args.slot,original_entry['generation'],restored_plan),-32070)
        check('reference' in failure['message'].lower() and negative.snapshot('after-invalid-source')==before_invalid,
              'Source dangling reference was concealed by retirement or changed active state')
        record['invalid_retired_source_reference']=dict(error=failure,negative_payload_sha256=sha(invalid_payload),
            field='retired InstanceLink.Target',phase='source validation before mapping')
        negative.close()
        if args.overflow_target_config:
            overflow_config=configuration(args.overflow_target_config);check(('descriptor' in overflow_config)==('descriptor' in dst_config),'Overflow target backend differs')
            overflow_schema=schema(read(args.overflow_target_manifest),1);overflow_world=output/'overflow.json';overflow_doc=target_document(source_doc,overflow_schema)
            # The source recipe already has TWO local references. A valid cap-one
            # target needs a different initializer, which is intentionally not an
            # approved migration. Reject source authored-array overflow before
            # snapshot mapping, with a genuine matching cap-one compiled image.
            for recipe in overflow_doc['templates'].values():
                for member in recipe.get('entities',{}).values():
                    bag=member['components']
                    if 'game:'+TYPE in bag:bag['game:'+TYPE][uid(4)]=[]
            overflow_world.write_text(json.dumps(overflow_doc,indent=2)+'\n');shutil.copytree(assets,Path(str(overflow_world)+'.assets'))
            overflow_inputs=compiled(overflow_config);protected['overflow_config']=(args.overflow_target_config.resolve(),sha(args.overflow_target_config));protected['overflow_manifest']=(args.overflow_target_manifest.resolve(),sha(args.overflow_target_manifest))
            overflow_probe=Owner('overflow-metadata',overflow_world,saves,overflow_config);overflow_probe.start();overflow_save=overflow_probe.write('overflow-metadata');overflow_probe.close()
            overflow_plan=copy.deepcopy(plan);overflow_plan['id']=uuid.uuid4().hex;overflow_plan['target']=identity(overflow_save)
            check(overflow_plan['target']['image_sha256']==sha(image(overflow_config)),'Overflow target selected wrong compiled image')
            overflow_plan['components'][0]['target_fingerprint']=overflow_schema['fingerprint'];overflow_plan['components'][0]['array_capacity'][0]['target_capacity']=1
            overflow_path=output/'overflow-upgrade.json';overflow_path.write_text(json.dumps(overflow_plan,indent=2)+'\n')
            overflow=Owner('overflow-rejection',overflow_world,saves,overflow_config,trap=True)
            overflow.session=uuid.uuid4().hex;overflow.rpc('runtime.start',dict(session_id=overflow.session,revision=overflow_doc['revision']))
            before_overflow=overflow.snapshot('before-overflow');failure=overflow.load(overflow.load_params(args.slot,original_entry['generation'],overflow_path),-32070)
            check('Preserved array exceeds target capacity; truncation is forbidden.' in failure['message'],'Shrink rejected before the actual source array overflow check')
            check(overflow.snapshot('after-overflow')==before_overflow,'Authored collection overflow changed active runtime')
            record['overflow_profile']=dict(compiled_inputs=overflow_inputs,manifest_sha256=sha(args.overflow_target_manifest),error=failure,
                rejection_stage='source authored recipe Members length2 cannot fit target capacity1; snapshot mapping not reached')
            overflow.close();check(compiled(overflow_config)==overflow_inputs,'Overflow compiled inputs changed')
        check(sha(original_path)==record['retained_source']['payload_sha256'],'Original retained save changed')
        record['checks']=['Actual retained v6 source restore','Explicit approved graph/global/collection mapping','Positive Initialize guard',
            'Exact and malformed edge rejection without publication','Immediate native snapshot preservation allowlist','Removed-plan retry recovery',
            'Compiled grown collection mutation and controller/rig continuation','Same/fresh exact target saves and arbitrary tick partitions',
            'Whole-root retirement/rebirth preserves frontier','Retirement cannot hide an invalid original entity reference']
        finished=True
    except BaseException:record['error']=traceback.format_exc()
    finally:
        for owner in owners:
            try:owner.close()
            except BaseException:record['cleanup_errors'].append(traceback.format_exc())
        try:
            for name,(path,pin) in protected.items():check((inventory(path) if isinstance(pin,dict) else sha(path))==pin,'Caller input changed: '+name)
            if 'compiled_inputs' in record:check(record['compiled_inputs']=={'source':compiled(src_config),'target':compiled(dst_config)},'Compiled source/target inputs changed')
            if 'fixture_sources' in record:check(record['fixture_sources']=={str(path.relative_to(ROOT)):sha(path) for path in sorted(FIXTURE.rglob('*'))
                if path.is_file() and path.suffix in ('.cs','.csproj','.md') and not any(part in ('bin','obj') for part in path.parts)},'Fixture sources changed')
            check(sha(__file__)==record['runner_sha256'],'Verifier changed during execution')
        except BaseException:record['cleanup_errors'].append(traceback.format_exc())
        record['passed']=finished and not record['cleanup_errors'];record['rpc_count']=len(record['calls'])
        (output/'evidence.json').write_text(json.dumps(record,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')
    if not record['passed']:raise SystemExit('Qualification failed; see '+str(output/'evidence.json'))

if __name__=='__main__':main()
