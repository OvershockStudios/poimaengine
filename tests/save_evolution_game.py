"""Real Collection Room save-upgrade qualification. Never edits the supplied baseline."""
import argparse, copy, hashlib, json, os, queue, shutil, subprocess, sys, threading, traceback, uuid
from pathlib import Path

HERE=Path(__file__).resolve().parent
FIXTURE=HERE/'fixtures/save_evolution_game'
uid=lambda n:f'{n:032x}'
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def check(ok,message):
    if not ok:raise RuntimeError(message)
def tree(path):return {p.relative_to(path).as_posix():sha(p) for p in sorted(path.rglob('*')) if p.is_file() and p.name!='.lock'}
def native(path):
    path=Path(path).resolve()
    return subprocess.check_output(['wslpath','-w',str(path)],text=True).strip() if ARGS.windows_interop else str(path)
def baseline():
    manifest=json.loads((BASE/'manifest.json').read_text())
    for row in manifest['files']:
        p=BASE/row['path'];check(p.stat().st_size==row['bytes'] and sha(p)==row['sha256'],'Baseline changed: '+row['path'])
    return manifest

class Host:
    def __init__(self,world,label,trap=True):
        self.calls=[];self.label=label;self.session=None;self.stderr=(OUT/(label+'.stderr.log')).open('w',encoding='utf-8');self.lines=queue.Queue();self.closed=False
        env=dict(os.environ);env['POIMA_SAVE_UPGRADE_FORBID_INITIALIZE']='1' if trap else '0'
        if ARGS.windows_interop:env['WSLENV']=':'.join(filter(None,[env.get('WSLENV',''),'POIMA_SAVE_UPGRADE_FORBID_INITIALIZE']))
        try:self.process=subprocess.Popen([str(ARGS.binary.resolve()),'world',native(world)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.stderr,text=True,encoding='utf-8',env=env)
        except BaseException:self.stderr.close();raise
        def reader():
            try:
                for line in self.process.stdout:self.lines.put(line)
            except BaseException as error:self.lines.put(error)
            finally:self.lines.put(None)
        threading.Thread(target=reader,daemon=True).start();OWNED.append(self)
    def rpc(self,method,params=None,error=None):
        request=dict(jsonrpc='2.0',id=len(self.calls)+1,method=method,params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n');self.process.stdin.flush()
        try:line=self.lines.get(timeout=90)
        except queue.Empty:raise TimeoutError('RPC timed out: '+method) from None
        if line is None or isinstance(line,BaseException):raise RuntimeError('Native output ended: '+method)
        reply=json.loads(line);self.calls.append(dict(request=request,response=reply))
        check(reply.get('jsonrpc')=='2.0' and reply.get('id')==request['id'],'Wrong RPC response identity')
        if error is not None:
            check('error' in reply and (error is True or reply['error']['code']==error),'Expected rejection: '+str(reply));return reply['error']
        check('result' in reply,'RPC failed: '+str(reply));return reply['result']
    def close(self):
        if self.closed:return
        self.closed=True;errors=[]
        try:
            try:self.process.stdin.close()
            except BaseException as e:errors.append(str(e))
            try:self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill();self.process.wait(timeout=10);errors.append('Host shutdown timed out')
            if self.process.returncode:errors.append('Host exit '+str(self.process.returncode))
        finally:self.stderr.close()
        check(not errors,'; '.join(errors))
    def configure(self,storage):
        self.storage=storage
        self.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(storage)))
    def start(self):
        self.session=uuid.uuid4().hex;self.rpc('runtime.start',dict(session_id=self.session,revision=self.rpc('world.inspect')['revision']))
    def state(self):return self.rpc('runtime.inspect',dict(session_id=self.session))
    def game(self):return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,include_schema=True))
    def guards(self):
        if self.session is None:return dict(expected_session_id=None,expected_tick=None,expected_gameplay_revision=None)
        state=self.state();game=self.game();state['component_revision']=self.rpc('runtime.components',dict(session_id=self.session))['component_revision']
        return dict(expected_session_id=self.session,expected_tick=state['tick'],expected_gameplay_revision=game['revision'],**{
            'expected_'+key:state[key] for key in ['component_revision','structure_revision','ui_revision','control_sequence']})
    def load_request(self,slot='collection-room',generation=1,upgrade=None):
        result=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=generation,
            revision=self.rpc('world.inspect')['revision'],new_session_id=uuid.uuid4().hex,gameplay=CONFIG,**self.guards())
        if upgrade is not None:result['upgrade']=upgrade
        return result
    def step(self,ticks=1,move=None,use=False):
        state=self.state();r=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=state['tick'],
            expected_structure_revision=state['structure_revision'],ticks=ticks,inputs=[] if move is None and not use else [dict(entity=uid(100),move=move or [0,0],use=use)]))
        self.session=r['current_session_id'];return r
    def control(self,number):
        state=self.state();r=self.rpc('runtime.ui.activate',dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=state['tick'],
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],expected_gameplay_revision=self.game()['revision'],expected_structure_revision=state['structure_revision'],id=uid(number)))
        self.session=r['current_session_id'];return r
    def write(self,slot,generation=0):
        guards=self.guards();guards['session_id']=guards.pop('expected_session_id')
        return self.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot=slot,expected_generation=generation,**guards))
    def snapshot(self):
        slot='check-'+uuid.uuid4().hex;self.write(slot)
        root=self.storage/('slot-'+slot);manifest=json.loads((root/'current.json').read_text())
        row=manifest['payload']['current'];payload=root/row['file'];check(sha(payload)==row['sha256'],'Snapshot observation storage checksum differs')
        return json.loads(payload.read_text())['snapshot']
    def fingerprint(self):return dict(state=self.state(),game=self.game(),saves=self.rpc('runtime.save.status',dict(session_id=self.session)),components=self.rpc('runtime.components',dict(session_id=self.session)))

def run():
    original=baseline();key='native' if ARGS.backend=='native_aot' else 'coreclr';source=original['identities'][key]
    check(source['tick']==3 and source['values']['Collected']==1,'Unexpected retained source scenario')
    world=OUT/'world.json';shutil.copy2(BASE/key/'world.json',world);world_hash=sha(world)
    storage=OUT/'saves';shutil.copytree(BASE/key/'saves',storage)
    old_slot=storage/'slot-collection-room';old_bytes=tree(old_slot)
    # Discover actual target metadata AND frozen content hash through a normal
    # host-generated throwaway save. Initialize is allowed only in this probe.
    probe_world=OUT/'probe-world.json';shutil.copy2(world,probe_world)
    probe_store=OUT/'probe-saves';probe_store.mkdir();probe=Host(probe_world,'probe',trap=False);probe.configure(probe_store);probe.start()
    probe.rpc('runtime.gameplay.load_native' if ARGS.backend=='native_aot' else 'runtime.gameplay.load',dict(session_id=probe.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**CONFIG))
    metadata=probe.game()['module'];meta_path=OUT/'actual-target-metadata.json';meta_path.write_text(json.dumps(metadata,indent=2)+'\n')
    probe.write('target-probe');probe.close()
    manifest=json.loads((probe_store/'slot-target-probe/current.json').read_text());payload=probe_store/'slot-target-probe'/manifest['payload']['current']['file']
    check(sha(payload)==manifest['payload']['current']['sha256'],'Probe save payload hash mismatch')
    snapshot=json.loads(payload.read_text())['snapshot'];content=snapshot['payload']['content_sha256']
    check(snapshot['payload']['gameplay']['schema']==metadata['schema'],'Actual host schema/save mismatch')
    plan=OUT/'upgrade-plan.json';helper=subprocess.run([sys.executable,str(FIXTURE/'derive_plan.py'),'--baseline',str(BASE),'--mapping',str(FIXTURE/'mapping.json'),'--backend',ARGS.backend,'--target-metadata',str(meta_path),'--target-content-sha256',content,'--output',str(plan)],capture_output=True,text=True,timeout=30)
    check(helper.returncode==0,helper.stdout+helper.stderr);RECORD['plan_preparation']=json.loads(helper.stdout)
    selected=dict(path=native(plan),expected_sha256=sha(plan));p=json.loads(plan.read_text())
    trap_world=OUT/'trap-world.json';shutil.copy2(world,trap_world)
    trap=Host(trap_world,'trap');trap.start()
    failure=trap.rpc('runtime.gameplay.load_native' if ARGS.backend=='native_aot' else 'runtime.gameplay.load',dict(session_id=trap.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=0,**CONFIG),error=True)
    check('Initialize forbidden during old-save upgrade' in str(failure),'Compiled Initialize trap positive control did not fire');trap.close()
    host=Host(world,'upgrade');host.configure(storage);host.start()
    def reject(request,error=True):
        before=host.fingerprint();checkpoint=host.snapshot();host.rpc('save.load',request,error=error)
        check(host.fingerprint()==before,'Rejected load mutated live runtime');check(tree(old_slot)==old_bytes,'Rejected load mutated original slot')
        check(host.snapshot()==checkpoint,'Rejected load changed complete runtime snapshot')
    reject(host.load_request()) # Changed compiled schema/image cannot exact-load.
    wrong_digest=host.load_request(upgrade=dict(selected,expected_sha256='0'*64));reject(wrong_digest)
    reject(wrong_digest) # Failed retry must remain rejection with no mutation.
    wrong=copy.deepcopy(p);wrong['target']['image_sha256']='0'*64;wrong_path=OUT/'wrong-plan.json';wrong_path.write_text(json.dumps(wrong)+'\n')
    reject(host.load_request(upgrade=dict(path=native(wrong_path),expected_sha256=sha(wrong_path))))
    # These failures happen after trusted CoreCLR target registration. They
    # must release the detached module and preserve live save epoch/ledger.
    for case,edit in [
        ('target-schema',lambda value:value['target'].__setitem__('schema_sha256','0'*64)),
        ('missing-legacy',lambda value:value['legacy_global_ids'].pop()),
        ('missing-preserve',lambda value:value['global']['preserve'].pop()),
        ('wrong-source',lambda value:value['source'].__setitem__('schema_sha256','0'*64)),
    ]:
        bad=copy.deepcopy(p);edit(bad);bad_path=OUT/(case+'-plan.json');bad_path.write_text(json.dumps(bad)+'\n')
        reject(host.load_request(upgrade=dict(path=native(bad_path),expected_sha256=sha(bad_path))))
        collected=host.rpc('runtime.gameplay.collect',dict(session_id=host.session))
        check(collected['active_modules']==0 and collected['retired_alive']==0,'Rejected target leaked a registered module')
    request=host.load_request(upgrade=selected);loaded=host.rpc('save.load',request);host.session=loaded['session_id']
    check(loaded['tick']==3 and loaded['upgrade']['plan_sha256']==selected['expected_sha256'],'Upgrade receipt differs')
    expected=copy.deepcopy(source['values']);expected['RecoveredCount']=expected.pop('Collected');expected['UpgradeSteps']=731
    check(host.game()['module']['values']==expected,'Restored fields differ before first Tick/Control')
    check(host.game()['module']['schema']==metadata['schema'],'Restored target schema differs')
    original_manifest=json.loads((old_slot/'current.json').read_text());original_file=old_slot/original_manifest['payload']['current']['file']
    original_snapshot=json.loads(original_file.read_text())['snapshot'];new_snapshot=host.snapshot()
    original_payload=copy.deepcopy(original_snapshot['payload']);new_payload=copy.deepcopy(new_snapshot['payload'])
    del original_payload['gameplay'];del new_payload['gameplay']
    check(original_snapshot['version']==new_snapshot['version'] and original_payload==new_payload,'Global-only upgrade altered native physics/UI/allocator or other snapshot state')

    plan_bytes=plan.read_bytes();plan.unlink()
    before=host.fingerprint();retry=host.rpc('save.load',request)
    check(retry['replayed'] and host.fingerprint()==before,'Exact retry reactivated/mutated runtime or reread deleted plan')
    plan.write_bytes(plan_bytes)
    changed=dict(request,new_session_id=uuid.uuid4().hex);reject(changed,-32010)
    stale=host.load_request(upgrade=selected);stale['expected_tick']=2;reject(stale,-32009)
    check(tree(old_slot)==old_bytes,'Successful upgrade/retry changed source slot')
    host.control(11);check(host.state()['tick']==3 and host.game()['module']['values']['UpgradeSteps']==732,'Actual Control did not run at unchanged tick')
    check(host.game()['module']['values']['SavePending']==0 and host.game()['module']['values']['Status']==3,'Restore epoch failed to resolve old pending ticket')
    host.step();check(host.game()['module']['values']['UpgradeSteps']==733,'Actual Tick failed after upgrade')
    # Exercise real remaining collection gameplay, not just a synthetic counter.
    for expected_count in (2,3):
        host.step(30,move=[1,0]);host.step(60,move=[0,1]);host.step(30,move=[-1,0]);host.step(use=True)
        check(host.game()['module']['values']['RecoveredCount']==expected_count,'Remaining collectible failed')
    ui=host.rpc('runtime.ui.inspect',dict(session_id=host.session,tick=host.state()['tick']))
    check(next(row['text'] for row in ui['elements'] if row['id']==uid(2))=='All 3 cells collected. Room complete!','Completion HUD differs')
    final=host.fingerprint();written=host.write('upgraded');check(written['generation']==1,'Explicit new save generation differs')
    check(tree(old_slot)==old_bytes,'New save changed source slot')
    # Create two genuine generations: retained old source, then an actual new
    # target save. Corrupt only the new test-owned generation for recovery.
    recovery_slot=storage/'slot-recovery-upgrade';shutil.copytree(old_slot,recovery_slot)
    check(host.write('recovery-upgrade',generation=1)['generation']==2,'Recovery fixture generation failed')
    recovery_manifest=json.loads((recovery_slot/'current.json').read_text())
    damaged=recovery_slot/recovery_manifest['payload']['current']['file'];damaged.write_bytes(b'intentionally damaged test checkpoint')
    damaged_tree=tree(recovery_slot);host.close()
    recovery=Host(world,'recovery');recovery.configure(storage);recovery.start()
    guarded=recovery.load_request(slot='recovery-upgrade',generation=2,upgrade=selected);before=recovery.fingerprint()
    recovery.rpc('save.load',guarded,error=-32070)
    check(recovery.fingerprint()==before and tree(recovery_slot)==damaged_tree,'Recovery rejection changed live state/storage')
    guarded['allow_recovery']=True;recovered=recovery.rpc('save.load',guarded);recovery.session=recovered['session_id']
    check(recovered['generation']==2 and recovered['selected_generation']==1 and recovered['recovered'] and recovered['tick']==3,'Recovery selected/report generation differs')
    check(recovery.game()['module']['values']==expected,'Recovered old generation did not migrate correctly')
    status=recovery.rpc('runtime.save.status',dict(session_id=recovery.session))
    check(status['last_restore']['generation']==1 and status['last_restore']['recovered'],'Recovery ledger uses manifest generation instead of selected generation')
    check(tree(recovery_slot)==damaged_tree and tree(old_slot)==old_bytes,'Recovery upgrade rewrote stored generations');recovery.close()

    fresh=Host(world,'fresh');fresh.configure(storage);exact=fresh.load_request(slot='upgraded');result=fresh.rpc('save.load',exact);fresh.session=result['session_id']
    check('upgrade' not in result and fresh.state()['tick']==final['state']['tick'],'Fresh process did not exact-restore new save')
    check(fresh.game()['module']['values']==final['game']['module']['values'] and fresh.game()['module']['schema']==metadata['schema'],'Fresh process lost target values/schema')
    previous=fresh.game()['module']['values']['UpgradeSteps'];fresh.step();check(fresh.game()['module']['values']['UpgradeSteps']==previous+1,'Fresh exact restore did not resume compiled Tick')
    check(tree(old_slot)==old_bytes,'Fresh exact restore changed original slot');check(sha(world)==world_hash,'Upgrade changed authored world');fresh.close();baseline()
    RECORD.update(source_identity=source,target_metadata=metadata,plan_sha256=sha(plan),source_slot_unchanged=True,final_tick=final['state']['tick'],final_values=final['game']['module']['values'],new_save=written)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--backend',choices=['coreclr','native_aot'],required=True)
    parser.add_argument('--hostfxr',type=Path);parser.add_argument('--bridge',type=Path);parser.add_argument('--assembly',type=Path);parser.add_argument('--native-descriptor',type=Path)
    parser.add_argument('--baseline',type=Path,required=True,help='Preserved source saves/artifacts with manifest.json; never regenerated')
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');ARGS=parser.parse_args()
    if sys.flags.optimize:parser.error('Run without Python optimization; qualification must retain all checks.')
    if ARGS.backend=='coreclr' and not all([ARGS.hostfxr,ARGS.bridge,ARGS.assembly]):parser.error('CoreCLR requires hostfxr/bridge/assembly')
    if ARGS.backend=='native_aot' and not ARGS.native_descriptor:parser.error('NativeAOT requires descriptor')
    BASE=ARGS.baseline.resolve()
    OUT=ARGS.output.resolve()
    if OUT==BASE or BASE in OUT.parents:parser.error('--output must not be inside the preserved baseline')
    OUT.mkdir(parents=True,exist_ok=False);OWNED=[];RECORD=dict(passed=False,backend=ARGS.backend,calls=[],limitations=['Actual Collection Room qualification; no physical input/rendering or general arbitrary migration claim.'])
    try:
        CONFIG=dict(descriptor=native(ARGS.native_descriptor)) if ARGS.backend=='native_aot' else dict(hostfxr=native(ARGS.hostfxr),bridge=native(ARGS.bridge),assembly=native(ARGS.assembly),type='Poima.Examples.CollectionGame')
        RECORD['input_hashes']={str(p):sha(p) for p in [ARGS.binary,Path(__file__),BASE/'manifest.json',FIXTURE/'derive_plan.py',FIXTURE/'mapping.json',FIXTURE/'CollectionGame.cs',FIXTURE/'Poima.CollectionGame.csproj',*( [ARGS.native_descriptor] if ARGS.backend=='native_aot' else [ARGS.bridge,ARGS.assembly])]}
        run();RECORD['passed']=True
    except BaseException:RECORD['error']=traceback.format_exc()
    finally:
        for host in OWNED:
            try:host.close()
            except BaseException:RECORD.setdefault('cleanup_errors',[]).append(traceback.format_exc());RECORD['passed']=False
            RECORD['calls'].append(dict(host=host.label,calls=host.calls))
        try:baseline()
        except BaseException:RECORD['baseline_error']=traceback.format_exc();RECORD['passed']=False
        (OUT/'evidence.json').write_text(json.dumps(RECORD,indent=2)+'\n')
    print(json.dumps(dict(passed=RECORD['passed'],evidence=str(OUT/'evidence.json'))))
    if not RECORD['passed']:print(RECORD.get('error','Qualification/cleanup failed'),file=sys.stderr)
    raise SystemExit(0 if RECORD['passed'] else 1)
