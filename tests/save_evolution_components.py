#!/usr/bin/env python3
"""Qualify compiled component save evolution through the headless engine.

Run with Linux Python/Linux binaries or native Windows Python/Windows binaries.
Supply built source/target gameplay configs and their generated component manifests.
Config paths resolve relative to their JSON file. Output must be a new directory.
The fixtures exercise stable field IDs across rename, layout reorder, retirement
and an explicit added default, including authored and template-spawned instances.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse,copy,hashlib,json,os,queue,subprocess,sys,threading,traceback,uuid
from pathlib import Path
TYPE='1'*32
GAME='Poima.Tests.SaveEvolutionGame'
FIXTURES=Path(__file__).resolve().parent/'fixtures/save_evolution_components'
def uid(n):return f'{n:032x}'
def check(ok,message):
    if not ok:raise RuntimeError(message)
def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False)
def digest(value):return hashlib.sha256(canonical(value).encode()).hexdigest()
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

def verify_image(config,identity):
    if 'descriptor' in config:
        path=Path(config['descriptor']);spec=json.loads(path.read_text());image=path.parent/spec['library']
    else:image=Path(config['assembly'])
    check(identity['image_sha256']==sha(image),'Host selected a different compiled image')

def validate_fields(schema,target):
    wanted={uid(2):'RecoveredCount' if target else 'Count',uid(3):'Total',uid(4):'Weight',uid(5):'Distance',uid(6):'Link'}
    wanted[uid(7) if target else uid(1)]='Bonus' if target else 'Retired'
    check({field['id']:field['name'] for field in schema['fields']}==wanted,'Fixture stable IDs or named fields differ')

def world(manifest,target):
    check(len(manifest['schemas'])==1 and manifest['schemas'][0]['id']==TYPE,'Fixture manifest differs')
    validate_fields(manifest['schemas'][0],target)
    cell={uid(2):10,uid(3):str(-(2**63)),uid(4):1.25,uid(5):4.5,uid(6):uid(2)}
    cell[uid(7) if target else uid(1)]=37 if target else 55
    recipe=copy.deepcopy(cell);recipe[uid(2)]=100;recipe[uid(6)]=uid(0)
    transform=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])
    return dict(format='poima.authored-world',version=3,world_id='f'*32,revision=2 if target else 1,
        entities={uid(1):dict(name='Authored cell',parent=None,components={'Transform':transform,'game:'+TYPE:cell}),
                  uid(2):dict(name='Reference anchor',parent=None,components={'Transform':transform})},
        component_schemas={TYPE:manifest['schemas'][0]},templates={uid(200):dict(name='Cell recipe',components={'Transform':transform,'game:'+TYPE:recipe})},
        receipts=[],retired_ids=[],retired_component_schemas=[],retired_template_ids=[])
class Session:
    def __init__(self,binary,path,evidence,trap=False):
        self.evidence=evidence;self.session=None;self.tick=0;self.process=None;self.err=None
        self.path=path;self.err=path.with_suffix('.stderr').open('a',encoding='utf-8')
        env=dict(os.environ);env['POIMA_COMPONENT_UPGRADE_FORBID_INITIALIZE']='1' if trap else '0'
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
    def step(self,n):
        observed=self.rpc('runtime.inspect',dict(session_id=self.session))
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,expected_structure_revision=observed['structure_revision'],ticks=n));self.tick=result['current_tick'];self.session=result['current_session_id']
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
def identity(save):
    payload=save['snapshot']['payload'];game=payload['gameplay']
    return dict(world_id=payload['world_id'],content_sha256=payload['content_sha256'],backend=game['backend'],module_identity=game['schema']['identity'],
        type=game['type'],image_sha256=game['assembly_sha256'],schema_sha256=digest(game['schema']))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for flag in ('binary','source-config','target-config','source-manifest','target-manifest','output'):p.add_argument('--'+flag,type=Path,required=True)
    a=p.parse_args();check(not sys.flags.optimize,'Run without Python optimization');out=a.output.resolve();check(not out.exists(),'Output must be new');out.mkdir(parents=True)
    evidence=dict(passed=False,calls=[],cleanup_errors=[],claims=['Actual compiled component fixture through headless save owner APIs'],limitations=['One explicit source/target component evolution; no arbitrary schema migration, rendering or physical input qualification.'],runner_sha256=sha(__file__))
    owned=[];completed=False
    try:
        src_config=configuration(a.source_config.resolve());dst_config=configuration(a.target_config.resolve())
        check(('descriptor' in src_config)==('descriptor' in dst_config),'Source and target must use the same backend')
        evidence['backend']='native_aot' if 'descriptor' in src_config else 'coreclr'
        evidence['compiled_artifacts']={'source':compiled_hashes(src_config),'target':compiled_hashes(dst_config)}
        evidence['fixture_hashes']={str(path.relative_to(FIXTURES)):sha(path) for path in sorted(FIXTURES.rglob('*')) if path.is_file() and path.suffix in ('.cs','.csproj') and not any(part in ('bin','obj') for part in path.relative_to(FIXTURES).parts)}
        manifests=[json.loads(a.source_manifest.read_text()),json.loads(a.target_manifest.read_text())]
        evidence['input_hashes']={k:sha(getattr(a,k)) for k in ('binary','source_config','target_config','source_manifest','target_manifest')}
        worlds=[out/'source.json',out/'target.json']
        for i in range(2):worlds[i].write_text(json.dumps(world(manifests[i],bool(i)),indent=2)+'\n')
        saves=out/'saves';saves.mkdir()
        def session(path,trap=False):
            obj=Session(a.binary.resolve(),path,evidence,trap);owned.append(obj);return obj
        source=session(worlds[0]);source.configure(saves);source.start(1,src_config);source.step(3)
        original_values=source.game()['module']['values'];spawned=original_values['Spawned']
        check(original_values['Ticks']==3 and original_values['Checked']==2,'Source real Tick differs')
        check(source.get(uid(1))[uid(2)]==13 and source.get(spawned)[uid(2)]==103,'Source values did not diverge from defaults')
        source.write('evolution',0);source_save,source_payload=stored(saves,'evolution');original_hash=sha(source_payload);source.close()
        # Separate actual target probe provides host-frozen content and real compiled
        # metadata. This is a fresh-game probe, not a restore; Initialize allowed.
        probe=session(worlds[1]);probe.configure(saves);probe.start(2,dst_config);probe.write('target-probe',0)
        target_probe,_=stored(saves,'target-probe');probe.close()
        source_id,target_id=identity(source_save),identity(target_probe)
        verify_image(src_config,source_id);verify_image(dst_config,target_id)
        old_schema=source_save['snapshot']['payload']['gameplay']['schema'];new_schema=target_probe['snapshot']['payload']['gameplay']['schema']
        global_ids=sorted(f['id'] for f in old_schema['persistent']['fields']);check(global_ids==sorted(f['id'] for f in new_schema['persistent']['fields']),'Global IDs changed')
        plan=dict(format='poima.save-upgrade',version=1,id=uuid.uuid4().hex,source=source_id,target=target_id,
            **{'global':dict(preserve=global_ids,retire=[],default=[]),'components':[dict(id=TYPE,
            source_fingerprint=source_save['document']['component_schemas'][TYPE]['fingerprint'],
            target_fingerprint=target_probe['document']['component_schemas'][TYPE]['fingerprint'],
            preserve=[uid(i) for i in range(2,7)],retire=[uid(1)],default=[uid(7)])]})
        plan_path=out/'upgrade.json';plan_path.write_text(json.dumps(plan,indent=2)+'\n')
        target=session(worlds[1],True);target.configure(saves)
        # A positive control proves the inherited environment actually arms the
        # fixture before asserting that restore bypasses Initialize.
        target.session=uuid.uuid4().hex
        target.rpc('runtime.start',dict(session_id=target.session,revision=2))
        initialization=target.rpc('runtime.gameplay.load_native' if 'descriptor' in dst_config else 'runtime.gameplay.load',
            dict(session_id=target.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**dst_config),error=True)
        check('Initialize forbidden during component save upgrade.' in initialization.get('message',''),'Initialize trap positive control did not execute')
        check(target.game()['module'] is None,'Failed Initialize published a gameplay module')
        collected=target.rpc('runtime.gameplay.collect',dict(session_id=target.session))
        check(collected['active_modules']==0 and collected['retired_alive']==0,'Failed Initialize leaked a module')
        target.rpc('runtime.stop',dict(session_id=target.session));target.session=None
        evidence['initialize_guard']=dict(positive_control=True,module_cleanup=collected)
        def load_params(generation=1):return dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='evolution',expected_generation=generation,revision=2,
            expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=uuid.uuid4().hex,gameplay=dst_config)
        target.rpc('save.load',load_params(),error=True);check(sha(source_payload)==original_hash,'Exact rejection changed original save')
        request=load_params();request['upgrade']=dict(path=str(plan_path),expected_sha256=sha(plan_path))
        restored=target.rpc('save.load',request);target.session=request['new_session_id'];target.tick=restored['tick']
        check(target.tick==3 and target.game()['module']['values']==original_values,'Upgrade changed global state/clock')
        check(target.get(uid(1))[uid(2)]==13 and target.get(spawned)[uid(2)]==103,'Upgrade reset runtime component values')
        for entity in (uid(1),spawned):
            values=target.get(entity);check(uid(1) not in values and values[uid(7)]==37 and values[uid(6)]==uid(2),'Retirement/default/reference mapping differs')
        check(target.rpc('save.load',request)==dict(restored,replayed=True),'Upgrade receipt replay differs')
        target.step(1);check(target.game()['module']['values']['BonusObserved']==37,'Actual target Tick did not read default')
        check(target.get(uid(1))[uid(2)]==14 and target.get(spawned)[uid(7)]==38,'Actual target Tick did not write generated fields')
        target.write('evolution',1);target.close();check(sha(source_payload)==original_hash,'Upgrade/new generation changed original payload')
        reopened=session(worlds[1],True);reopened.configure(saves);request=load_params(2)
        restored=reopened.rpc('save.load',request);reopened.session=request['new_session_id'];reopened.tick=restored['tick'];check(reopened.tick==4,'Fresh exact restore clock differs')
        reopened.step(1);check(reopened.get(uid(1))[uid(2)]==15 and reopened.get(spawned)[uid(7)]==39,'Fresh exact continuation failed')
        evidence.update(final_values=reopened.game()['module']['values'],source_identity=source_id,target_identity=target_id,upgrade=plan,original_payload_sha256=original_hash)
        check(evidence['compiled_artifacts']=={'source':compiled_hashes(src_config),'target':compiled_hashes(dst_config)},'Compiled inputs changed during qualification')
        check(evidence['fixture_hashes']=={str(path.relative_to(FIXTURES)):sha(path) for path in sorted(FIXTURES.rglob('*')) if path.is_file() and path.suffix in ('.cs','.csproj') and not any(part in ('bin','obj') for part in path.relative_to(FIXTURES).parts)},'Fixture sources changed during qualification')
        completed=True
    except BaseException:evidence['error']=traceback.format_exc()
    finally:
        for obj in owned:
            try:obj.close()
            except BaseException:evidence['cleanup_errors'].append(traceback.format_exc())
        evidence['passed']=completed and not evidence['cleanup_errors']
        (out/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    if not evidence['passed']:raise SystemExit('Qualification failed; see '+str(out/'evidence.json'))
if __name__=='__main__':main()
