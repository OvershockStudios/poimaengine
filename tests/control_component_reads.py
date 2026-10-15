"""Focused compiled Control component reads; root-owned Windows Job, no compiler/GPU."""
from pathlib import Path
import argparse, copy, importlib.util, json, os, sys, time, traceback, uuid
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'))
from workflow_components_game_service import Checked, assigned_job, exact, need, pin, read, tree, write
from native_gameplay_owner import NativeOwner, trusted_descriptor
QUEUE='cc150000000000000000000000000001';UNIT='cc150000000000000000000000000002'
LABEL=f'{100:032x}'
ACTIONS={name:f'{number:032x}' for number,name in enumerate(('read','refuse','missing','fail-unknown','fail-missing','fail-empty-query'),101)}
def uid(i):return f'{i:032x}'
def prepare(author,schemas):
    need({s['id'] for s in schemas}=={QUEUE,UNIT},'Expected generated queue/unit schemas')
    queue=next(s for s in schemas if s['id']==QUEUE);unit=next(s for s in schemas if s['id']==UNIT)
    need(len(queue['fields'])==1 and queue['fields'][0]['kind']=='array'
         and queue['fields'][0]['element_kind']=='entity' and queue['fields'][0]['capacity']==4,'Queue schema differs')
    values=dict(Count=7,Exact='9007199254740993',Single=.5,Precise=1e-200,Owner=uid(1),
        Ints=[-2147483648,2147483647],Longs=['-9223372036854775808','9223372036854775807'],
        Floats=[.5,-65536.0],Doubles=[1e-200,-1e200],Entities=[uid(1),uid(0)])
    need({f['name'] for f in unit['fields']}==values.keys(),'Complete generated unit field membership differs')
    wire={f['id']:values[f['name']] for f in unit['fields']}
    authored=author.call('world.inspect')
    need(authored['entity_count']==0,'Fixture requires an actual empty source world, not a starter project')
    ops=[dict(op='component.schema.set',schema=s) for s in schemas]
    ops.extend(dict(op='entity.create',id=uid(i),name='Control read '+str(i)) for i in (1,2,4,6,8,10))
    ops.append(dict(op='component.set',id=uid(1),type='game:'+QUEUE,value={queue['fields'][0]['id']:[uid(i) for i in (2,4,6,8)]}))
    ops.extend(dict(op='component.set',id=uid(i),type='game:'+UNIT,value=wire) for i in (2,4,6,8))
    def element(identity,kind,text,action=None,parent=uid(99)):
        return dict(op='ui.element.set',id=identity,element=dict(parent=parent,name='Control read '+str(identity),
            kind=kind,text=text,action=action,visible=True,enabled=True))
    ops.extend([element(uid(99),'panel','',parent=None),element(LABEL,'label','Original')])
    ops.extend(element(identity,'button',action,action) for action,identity in ACTIONS.items())
    author.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=authored['revision'],ops=ops))
    return unit

def expected_control(before,updates,text):
    expected=copy.deepcopy(before)
    expected['gameplay_revision']+=1;expected['control_sequence']+=1
    expected['gameplay']['values']['Controls']+=1
    expected['gameplay']['values'].update(updates)
    expected['ui']['revision']+=1;expected['ui']['elements'][LABEL]['text']=text
    return expected

def point(owner,label,records):
    probe=owner.probe('control-probe-'+label)
    capture=owner.capture(probe,max_entities=8,max_component_cells=16)
    records[label]=capture
    return capture

def payload(capture):return capture['probe']['document']['snapshot']['payload']

def exercise(owner,unit,records):
    initial=point(owner,'initial',records)
    need(owner.tick==0,'Controls must begin without a synthetic Tick')
    controls=[]
    def success(action,label,updates,text):
        nonlocal initial
        before=payload(initial)
        params,result=owner.control(ACTIONS[action],action)
        after=point(owner,label,records)
        need(exact(payload(after),expected_control(before,updates,text)),
             'Compiled Control full native payload differs from exact bounded predicted delta: '+action)
        need(owner.tick==0,'Compiled read/refusal advanced simulation time')
        controls.append(dict(action=action,params=params,result=result));initial=after
        return params
    first=success('read','read',dict(ReadChecks=1,CopyChecks=1,QueryPages=3,QueryMembers=4,ObservedFirstCount=7,Sum=28),
        'Read 1: 4 units, first 7, sum 28')
    success('refuse','refused',dict(Refusals=1),'Full queue refused without a tick.')
    success('missing','missing',dict(MissingFalse=1,DeadFalse=1),'Missing component and unknown entity distinguished.')
    # A real native component edit between callbacks proves reads use committed authority, not a cached mirror.
    registry=owner.call('runtime.components',dict(session_id=owner.session))
    direct=owner.call('runtime.component.get',dict(session_id=owner.session,tick=owner.tick,id=uid(2),type=UNIT,
        structure_revision=registry['structure_revision']))
    changed=copy.deepcopy(direct['values']);count_id=next(f['id'] for f in unit['fields'] if f['name']=='Count');changed[count_id]=8
    owner.call('runtime.component.edit',dict(session_id=owner.session,request_id=uuid.uuid4().hex,
        expected_tick=owner.tick,expected_revision=registry['component_revision'],expected_structure_revision=registry['structure_revision'],
        id=uid(2),type=UNIT,values=changed))
    initial=point(owner,'external-edit',records)
    success('read','read-current',dict(ReadChecks=2,CopyChecks=2,QueryPages=3,QueryMembers=4,ObservedFirstCount=8,Sum=29),
        'Read 2: 4 units, first 8, sum 29')
    failures=[]
    markers={'fail-unknown':'Runtime entity does not exist.','fail-missing':'Entity does not have the requested component.',
             'fail-empty-query':'Query pages hold 1..256 IDs.'}
    for index,(action,marker) in enumerate(markers.items()):
        before=initial;state=owner.inspect();game=owner.module()
        queue_before=owner.idle();files_before=tree(owner.expected_save_root)
        params=dict(session_id=owner.session,request_id=uuid.uuid4().hex,expected_tick=owner.tick,
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=ACTIONS[action])
        error=owner.call('runtime.ui.activate',params,error=-32040)
        need(marker in error['message'],'Did not reach the armed compiled Control read error: '+action)
        need(exact(owner.idle(),queue_before) and tree(owner.expected_save_root)==files_before,
             'Failed Control published save work or changed storage')
        after=point(owner,'failure-'+str(index),records)
        need(exact(after['probe']['document'],before['probe']['document']),
             'Failed Control changed complete saved state, not only selected components')
        failures.append(dict(action=action,params=params,error=error));initial=after
    # Exact historical retry must not reenter the old read after the intervening native edit.
    before=initial;queue_before=owner.idle();files_before=tree(owner.expected_save_root)
    replay=owner.call('runtime.ui.activate',first)
    need(replay['replayed'] is True,'Historical Control receipt was not recovered')
    need(exact(owner.idle(),queue_before) and tree(owner.expected_save_root)==files_before,'Retry changed save state')
    after=point(owner,'historical-retry',records)
    need(exact(after['probe']['document'],before['probe']['document']),'Historical retry reexecuted compiled reads')
    success('read','after-errors',dict(ReadChecks=3,CopyChecks=3,QueryPages=3,QueryMembers=4,ObservedFirstCount=8,Sum=29),
        'Read 3: 4 units, first 8, sum 29')
    return dict(successful_controls=controls,armed_failures=failures,historical_retry=replay,
        final_tick=owner.tick,scope='Compiled read-only source-owner callbacks and complete native snapshot deltas; no GPU or original-management repair')

def dependencies():
    names=['tests/control_component_reads.py','tests/native_gameplay_owner.py','tests/switchback_ownership.py',
        'tests/workflow_components_game_service.py','tests/workflow_response_native.py','tests/workflow_save_upgrade.py',
        'tests/switchback_game_service.py','tests/switchback_oracle.py','tests/relay_yard_upgrade.py',
        'tests/relay_yard_contract.py','tests/instance_save_evolution.py','examples/relay-yard/run.py',
        'examples/locomotion-yard/run.py']
    return [ROOT/name for name in names]+[p for p in sorted((ROOT/'tools/python/poima_client').rglob('*'))
        if p.is_file() and p.suffix in ('.py','.json')]


def input_pins(binary,artifact):
    # Caller explicitly selects the trusted engine; hashes record that choice,
    # not an invented producer or independent vendor qualification.
    files=dependencies()+[Path(sys.executable),binary,binary.parent/'phonon.dll']
    return {str(p.resolve()):pin(p) for p in files}


def unchanged(admission):
    need(all(pin(Path(name))==row for name,row in admission['files'].items()),'Input/source bytes changed')
    need(exact(tree(Path(admission['artifact_root'])),admission['artifact_files']),'Artifact membership or bytes changed')


def worker(args,admission):
    assigned_job();unchanged(admission)
    consumer=args.output/'consumer';need(not consumer.exists(),'Fresh consumer output required');consumer.mkdir()
    deadline=time.monotonic()+args.seconds
    binary,artifact=args.engine,args.artifact
    metadata=read(artifact);need(metadata['type']=='Poima.Tests.ControlReadGame'
        and metadata['identity']=='poima.test.control-component-reads','Wrong compiled fixture')
    original_artifact=admission['artifact_files']
    original_image=admission['files'][str(binary)]
    original_phonon=admission['files'][str(binary.parent/'phonon.dll')]
    owner=None;author=None;report=dict(passed=False,errors=[],owners=[],checkpoints={},
        native_image=original_image,artifact=pin(artifact),
        limits=['Actual caller-selected NativeAOT source-owner Control reads; no renderer or engine/compiler build.',
                'Complete saved payloads plus public selected observations; no physical input or all-capacity/performance claim.',
                'No exported/readonly/CoreCLR or genuinely retired-handle qualification from this fixture.',
                'The caller-selected executable is explicitly trusted input, not inferred from candidate metadata or an invented producer.'])
    try:
        source=consumer/'source';source.mkdir();world=source/'world.json';saves=consumer/'saves';saves.mkdir()
        author=Checked(binary,consumer/'authoring',deadline,world=world,saves=saves)
        unit=prepare(author,metadata['schema']['components']);author.close()
        need(author.transport.returncode==0,'Authoring owner failed normal EOF closure')
        report['owners'].append(dict(role='authoring',calls=len(author.calls),returncode=author.transport.returncode));author=None
        source_before=tree(source)
        owner=NativeOwner(consumer/'runtime',deadline,saves=saves,binary=binary,world=world,
            descriptor=artifact,initial_values={},admitted_execution_pins={str(binary):original_image,
                str(binary.parent/'phonon.dll'):original_phonon})
        report['exercise']=exercise(owner,unit,report['checkpoints'])
        report['owners'].append(owner.close_recorded());owner=None
        need(tree(source)==source_before and tree(artifact.parent)==original_artifact
             and pin(binary)==original_image and pin(binary.parent/'phonon.dll')==original_phonon,'Immutable input changed')
        unchanged(admission);report.update(passed=True,original_inputs_unchanged=True)
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        # Known owners get normal close attempts; unknown partial startup stays
        # in the enclosing Job. Never kill by name, retry or relabel a failure.
        if owner is not None:
            try:report['owners'].append(owner.close_recorded())
            except BaseException:report['errors'].append(traceback.format_exc())
        if author is not None:
            try:
                author.close();report['owners'].append(dict(role='failed-authoring-close',calls=len(author.calls),returncode=author.transport.returncode))
            except BaseException:report['errors'].append(traceback.format_exc())
        write(consumer/'report.json',report)
    return 0 if report['passed'] else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine',type=Path,required=True,help='Explicitly trusted matching Windows CLI; sibling phonon.dll required')
    parser.add_argument('--artifact',type=Path,required=True,help='Already compiled ControlReadGame artifact directory or descriptor')
    parser.add_argument('--output',type=Path,required=True,help='Fresh output outside the immutable artifact and engine directories')
    parser.add_argument('--seconds',type=int,default=180,help='Worker deadline,30..600 seconds (default180)')
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--admission-sha256',help=argparse.SUPPRESS)
    args=parser.parse_args()
    need(os.name=='nt' and not sys.flags.optimize,'Native Windows nonoptimized Python required')
    need(30<=args.seconds<=600,'Worker deadline must be30..600 seconds')
    sys.dont_write_bytecode=True;os.environ['PYTHONDONTWRITEBYTECODE']='1'
    args.engine=args.engine.resolve(strict=True)
    args.artifact=(args.artifact/'native-gameplay.json' if args.artifact.is_dir() else args.artifact).resolve(strict=True)
    args.output=args.output.resolve()
    need(args.artifact.name=='native-gameplay.json','Expected actual native-gameplay.json descriptor')
    if args.worker:
        need(args.admission_sha256==pin(args.output/'admission.json')['sha256'],'Worker admission bytes differ')
        admission=read(args.output/'admission.json')
        need(admission['engine']==str(args.engine) and admission['artifact']==str(args.artifact)
             and admission['seconds']==args.seconds,'Worker selection differs from parent admission')
        return worker(args,admission)
    need(not args.output.exists() and not args.output.is_relative_to(args.artifact.parent)
         and not args.output.is_relative_to(args.engine.parent),'Fresh independent output outside immutable inputs required')
    _,metadata,_=trusted_descriptor(args.artifact)
    need(metadata['type']=='Poima.Tests.ControlReadGame' and metadata['identity']=='poima.test.control-component-reads','Wrong compiled fixture')
    artifact_files=tree(args.artifact.parent)
    admission=dict(format='poima.control-component-reads.admission.v1',engine=str(args.engine),artifact=str(args.artifact),
        artifact_root=str(args.artifact.parent),artifact_files=artifact_files,files=input_pins(args.engine,args.artifact),
        seconds=args.seconds,scope='Caller-selected trusted executable and complete published fixture closure; no inferred producer authority')
    args.output.mkdir(parents=True);write(args.output/'admission.json',admission)
    spec=importlib.util.spec_from_file_location('control_component_reads_job',ROOT/'tests/switchback_ownership.py')
    gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
    command=[sys.executable,str(Path(__file__).resolve()),'--engine',str(args.engine),'--artifact',str(args.artifact),
        '--output',str(args.output),'--seconds',str(args.seconds),'--worker',
        '--admission-sha256',pin(args.output/'admission.json')['sha256']]
    job=gate.gated(command,args.output/'job',args.seconds+40)
    result=dict(passed=False,job=job,admission=pin(args.output/'admission.json'),errors=[])
    try:
        unchanged(admission)
        consumer=read(args.output/'consumer/report.json');result['consumer_report']=pin(args.output/'consumer/report.json')
        result['passed']=job['passed'] is True and consumer['passed'] is True and job['forced_cleanup'] is False and job['cleanup_errors']==[]
        for name in ('job_accounting_at_child_exit','job_accounting_after_cleanup'):
            accounting=job[name];need(accounting['active_processes']==accounting['terminated_processes']==0,'Job did not close naturally')
    except BaseException:result['errors'].append(traceback.format_exc());result['passed']=False
    write(args.output/'qualification.json',result)
    print(json.dumps(dict(passed=result['passed'],seconds=job['seconds'],forced_cleanup=job['forced_cleanup'],errors=result['errors'])))
    return 0 if result['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
