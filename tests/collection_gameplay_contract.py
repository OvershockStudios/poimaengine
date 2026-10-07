#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual compiled bounded collections: callbacks, atomic rollback and durable restore.
Native-host Python and matching engine/config paths; output must be new.
"""
import argparse,copy,hashlib,json,os,queue,subprocess,sys,threading,traceback,uuid
from pathlib import Path
TYPE='6'*32
GAME='Poima.Tests.CollectionGameplay'
def uid(n):return f'{n:032x}'

def check(ok,message):
    if not ok:raise RuntimeError(message)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def configuration(path):
    config=json.loads(path.read_text())
    for key in ('hostfxr','bridge','assembly','descriptor'):
        if key in config:
            selected=Path(config[key]);config[key]=str((selected if selected.is_absolute() else path.parent/selected).resolve(strict=True))
    check(config.get('type',GAME)==GAME,'Gameplay config selects another fixture type')
    return config

def compiled_hashes(config):
    result={}
    if 'descriptor' in config:
        descriptor=Path(config['descriptor']);spec=json.loads(descriptor.read_text());result[str(descriptor)]=sha(descriptor)
        check(spec['type']==GAME,'Native artifact selects another fixture type')
        for row in spec['files']:
            path=(descriptor.parent/row['path']).resolve(strict=True)
            check(path.is_relative_to(descriptor.parent.resolve()),'Artifact payload escapes its directory')
            check(path.stat().st_size==row['size'] and sha(path)==row['sha256'],'Native artifact payload differs: '+row['path'])
            result[str(path)]=sha(path)
    else:
        for key in ('hostfxr','bridge','assembly'):
            path=Path(config[key]);result[str(path)]=sha(path)
        for key in ('bridge','assembly'):
            sdk=Path(config[key]).parent/'Poima.Gameplay.dll'
            check(sdk.is_file(),'Compiled fixture/bridge SDK absent: '+str(sdk));result[str(sdk)]=sha(sdk)
    return result

class Session:
    def __init__(self,binary,path,evidence,trap=False):
        self.evidence=evidence;self.session=None;self.tick=0;self.process=None;self.err=None
        self.path=path;self.err=path.with_suffix('.stderr').open('a',encoding='utf-8')
        env=dict(os.environ);env['POIMA_COLLECTION_FORBID_INITIALIZE']='1' if trap else '0'
        try:self.process=subprocess.Popen([str(binary),'world',str(path)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.err,text=True,encoding='utf-8',env=env)
        except BaseException:self.err.close();raise
        self.output=queue.Queue()
        def read():
            try:
                for line in self.process.stdout:self.output.put(line)
            finally:self.output.put(None)
        threading.Thread(target=read,daemon=True).start()
    def rpc(self,method,params=None,error=False):
        req=dict(jsonrpc='2.0',id=uuid.uuid4().hex,method=method,params=params or {})
        self.process.stdin.write(json.dumps(req)+'\n');self.process.stdin.flush()
        line=self.output.get(timeout=90);check(line is not None,'Engine exited before response')
        reply=json.loads(line);self.evidence['calls'].append(dict(request=req,response=reply))
        check(reply.get('jsonrpc')=='2.0' and reply.get('id')==req['id'],'RPC identity differs')
        check(('error' in reply) if error else ('result' in reply),'Unexpected RPC outcome: '+str(reply))
        return reply['error' if error else 'result']
    def start(self,revision,config):
        self.session=uuid.uuid4().hex;self.rpc('runtime.start',dict(session_id=self.session,revision=revision))
        self.rpc('runtime.gameplay.load_native' if 'descriptor' in config else 'runtime.gameplay.load',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**config))
    def configure(self,root):self.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(root)))
    def game(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
    def inspect(self):return self.rpc('runtime.inspect',dict(session_id=self.session))
    def step(self,n,error=False):
        observed=self.rpc('runtime.inspect',dict(session_id=self.session))
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_structure_revision=observed['structure_revision'],ticks=n),error)
        if not error:self.tick=result['current_tick'];self.session=result['current_session_id']
        return result
    def edit(self,**values):
        return self.rpc('runtime.gameplay.edit',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_revision=self.game()['revision'],expected_structure_revision=self.inspect()['structure_revision'],values=values))
    def get(self,entity):return self.rpc('runtime.component.get',dict(session_id=self.session,tick=self.tick,type=TYPE,id=entity))['values']
    def write(self,slot,generation):
        state=self.inspect();game=self.game();components=self.rpc('runtime.components',dict(session_id=self.session))
        return self.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=generation,session_id=self.session,
            expected_tick=self.tick,expected_gameplay_revision=game['revision'],expected_component_revision=components['component_revision'],expected_structure_revision=state['structure_revision']))
    def close(self):
        if self.process is None:return
        process=self.process;self.process=None
        try:
            if process.stdin:process.stdin.close()
            process.wait(timeout=20);check(process.returncode==0,'Engine nonzero exit')
        finally:
            if process.poll() is None:process.kill();process.wait(timeout=10)
            self.err.close()

def stored(root,slot):
    path=root/('slot-'+slot);manifest=json.loads((path/'current.json').read_text());entry=manifest['payload']['current'];raw=(path/entry['file']).read_bytes()
    check(hashlib.sha256(raw).hexdigest()==entry['sha256'],'Stored payload hash differs');return json.loads(raw),path/entry['file']
def world(manifest):
    check(len(manifest['schemas'])==1 and manifest['schemas'][0]['id']==TYPE and manifest['schemas'][0]['version']==2,'Expected actual collection fixture schema')
    schema=manifest['schemas'][0];fields={f['id']:f for f in schema['fields']}
    check(fields[uid(1)]['element_kind']=='entity' and fields[uid(2)]['element_kind']=='int32' and fields[uid(1)]['capacity']==fields[uid(2)]['capacity']==4,'Fixture capacities/types differ')
    transform=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])
    entities={uid(i):dict(name='Owner' if i==1 else 'Item '+str(i),parent=None,components={'Transform':transform}) for i in range(1,2)}
    entities[uid(1)]['components']['game:'+TYPE]={uid(1):[],uid(2):[],uid(3):0}
    return dict(format='poima.authored-world',version=3,world_id='e'*32,revision=1,entities=entities,
        component_schemas={TYPE:schema},templates={uid(200):dict(name='Bag',components={'Transform':transform,'game:'+TYPE:{uid(1):[],uid(2):[2,5],uid(3):0}}),uid(201):dict(name='Item',components={'Transform':transform})},
        receipts=[],retired_ids=[],retired_component_schemas=[],retired_template_ids=[])
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','config','manifest','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();check(not sys.flags.optimize,'Run without Python optimization');out=a.output.resolve();check(not out.exists(),'Output must be new');out.mkdir(parents=True)
    evidence=dict(passed=False,calls=[],cleanup_errors=[],checks=[],hashes={},runner_sha256=sha(__file__))
    owned=[];complete=False
    try:
        config=configuration(a.config.resolve());evidence['hashes']=compiled_hashes(config)
        evidence['hashes'].update({str(a.binary.resolve()):sha(a.binary),str(a.manifest.resolve()):sha(a.manifest)})
        fixture=Path(__file__).with_name('collection_gameplay')
        evidence['sources']={path.name:sha(path) for path in fixture.glob('*') if path.is_file()}
        path=out/'world.json';path.write_text(json.dumps(world(json.loads(a.manifest.read_text())),indent=2)+'\n');authored_hash=sha(path)
        storage=out/'saves';storage.mkdir()
        def open_session(trap=False):
            instance=Session(a.binary.resolve(),path,evidence,trap);owned.append(instance);instance.configure(storage);return instance
        process=open_session();process.start(1,config);process.step(1)
        spawned_items=process.game()['module']['values'];items=[spawned_items[key] for key in ('FirstItem','SecondItem','ThirdItem','LastItem')]
        check(len(set(items))==4 and all(item!=uid(0) and item!=uid(1) for item in items),'Actual item spawn identities invalid')
        evidence['spawned_items']=items
        process.edit(Mode=1);process.step(4)
        check(process.get(uid(1))[uid(1)]==items and process.get(uid(1))[uid(2)]==[10,11,12,13],'Actual generated Get/Set append order differs')
        full=process.get(uid(1));process.step(1);values=process.game()['module']['values'];after_full=process.get(uid(1))
        check(after_full[uid(1)]==full[uid(1)] and after_full[uid(2)]==full[uid(2)],'Full append changed collection contents')
        check(values['FullRejected']==1 and values['PublishedRead']==1 and process.get(uid(1))[uid(1)]==items,'Capacity/staged read behavior differs')
        evidence['checks'].append('Generated Get/Set fills capacity4, rejects overflow without mutation, preserves published reads')
        def frozen(slot):
            process.write(slot,0);save,payload=stored(storage,slot);return save,payload.read_bytes()
        for mode in (2,3,4,7,10):
            process.edit(Mode=mode);before,raw=frozen('before-'+str(mode));error=process.step(1,error=True);after,other=frozen('after-'+str(mode))
            if mode in (2,7,10):check('Component entity reference does not resolve' in error['message'],'Expected collection reference rejection, not unrelated structural failure')
            if mode==3:check('Intentional collection rollback' in error['message'],'Expected explicit post-write fixture exception')
            check(raw==other,'Failed collection Tick changed full saved world/runtime snapshot for mode '+str(mode))
        process.edit(Mode=0)
        if process.tick%2:process.step(1)
        process.edit(Mode=9);before,raw=frozen('before-batch');error=process.step(2,error=True);after,other=frozen('after-batch')
        check('Later tick collection rollback' in error['message'],'Expected intentional later-tick fixture exception')
        check(raw==other,'Later-tick failure changed full snapshot')
        evidence['checks'].append('Dangling first/last references, index failure, queued-write exception and later-tick batch failure preserve complete world-save bytes')
        process.edit(Mode=5);process.step(1);process.step(1);spawned=process.game()['module']['values']['Spawned']
        check(process.get(spawned)[uid(1)]==[items[0],items[3]] and process.get(spawned)[uid(2)]==[2,5],'Actual template spawn collection values differ')
        before_globals=process.game()['module']['values'];before_owner=process.get(uid(1));before_spawned=process.get(spawned)
        process.write('inventory',0);save,original_payload=stored(storage,'inventory');original_hash=sha(original_payload);saved_tick=process.tick;process.close()
        process=open_session(True)
        # Explicit control proves the marker really prevents normal Initialize.
        process.session=uuid.uuid4().hex;process.rpc('runtime.start',dict(session_id=process.session,revision=1))
        error=process.rpc('runtime.gameplay.load_native' if 'descriptor' in config else 'runtime.gameplay.load',dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**config),error=True)
        check('Initialize forbidden during collection exact restore' in error['message'],'Initialize trap not armed')
        process.rpc('runtime.stop',dict(session_id=process.session));process.session=None
        def load(generation):
            request=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='inventory',expected_generation=generation,revision=1,
                expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=uuid.uuid4().hex,gameplay=config)
            result=process.rpc('save.load',request);process.session=request['new_session_id'];process.tick=result['tick'];return result
        load(1);check(process.tick==saved_tick and process.game()['module']['values']==before_globals and process.get(uid(1))==before_owner and process.get(spawned)==before_spawned,'Fresh exact restore changed nonempty collections')
        restored_ticks=process.game()['module']['values']['Ticks'];prior_tick=process.tick;process.step(1)
        continued=process.game()['module']['values'];check(continued['Inventories']==2 and continued['Ticks']==restored_ticks+1 and process.tick==prior_tick+1,'Actual restored Tick cannot query/advance inventories')
        process.edit(Mode=6);process.step(1)
        process.rpc('runtime.entity',dict(session_id=process.session,id=items[0]),error=True);process.rpc('runtime.entity',dict(session_id=process.session,id=items[3]),error=True)
        check(process.get(uid(1))[uid(1)]==items[1:3] and process.get(uid(1))[uid(2)]==[11,12] and process.get(spawned)[uid(1)]==[] and process.get(spawned)[uid(2)]==[],'First/last repair lost ordering or left references')
        check(sha(path)==authored_hash,'Runtime collection operations mutated authored template/world')
        process.write('inventory',1);second_tick=process.tick;process.close();check(sha(original_payload)==original_hash,'New generation changed old payload bytes')
        process=open_session(True);load(2);check(process.tick==second_tick,'Second-generation clock differs');process.step(1)
        check(process.get(uid(1))[uid(1)]==items[1:3] and process.get(spawned)[uid(1)]==[],'Second fresh continuation lost repaired collection values')
        evidence['checks'].append('Actual template spawn, Init-bypassed fresh exact restore, first/last reference repair+despawn, generation2 and fresh continuation passed')
        evidence.update(final_values=process.game()['module']['values'],initial_saved_tick=saved_tick,second_saved_tick=second_tick,original_payload_sha256=original_hash)
        complete=True
    except BaseException:evidence['error']=traceback.format_exc()
    finally:
        for instance in owned:
            try:instance.close()
            except BaseException:evidence['cleanup_errors'].append(traceback.format_exc())
        evidence['passed']=complete and not evidence['cleanup_errors'];(out/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    if not evidence['passed']:raise SystemExit('Qualification failed; see '+str(out/'evidence.json'))
if __name__=='__main__':main()
