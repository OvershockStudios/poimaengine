#!/usr/bin/env python3
"""Real C# births through the agent service, guarded editing, reload and durable restore."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import subprocess
import uuid
from components_gameplay_contract import Session, sha, uid


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    for name in ('manifest','hostfxr','bridge','assembly','native-descriptor'):
        parser.add_argument('--'+name,type=Path,required=name=='manifest')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    if not args.native_descriptor and not all((args.hostfxr,args.bridge,args.assembly)):
        parser.error('CoreCLR paths or native descriptor required')
    def native(path):
        path=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',path],text=True).strip() if args.windows_interop else path
    args.native=native
    run=args.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True)
    evidence=dict(passed=False,backend='native_aot' if args.native_descriptor else 'coreclr',checks=[],calls=[],
                  hashes={name:sha(getattr(args,name)) for name in ('binary','manifest','hostfxr','bridge','assembly','native_descriptor') if getattr(args,name)},
                  harness_sha256=sha(__file__))
    process=None
    try:
        process=Session(args,run/'world.json',evidence)
        schema=json.loads(args.manifest.read_text())['schemas'][0]
        schema=dict(schema);schema.pop('fingerprint',None)
        type_id=schema['id'];initial={f['id']:f['default'] for f in schema['fields']}
        transform=dict(position=[0,2,0],rotation=[0,0,0,1],scale=[1,1,1])
        collider=dict(motion='kinematic',half_extents=[.5,.5,.5],mass=1,friction=.5,restitution=0)
        ops=[dict(op='component.schema.set',schema=schema),dict(op='entity.create',id='8'*32,name='Survivor'),
             dict(op='component.set',id='8'*32,type='game:'+type_id,value=initial)]
        for template,linked in [('3'*32,False),('4'*32,True)]:
            parts=dict(Transform=transform,BoxCollider=collider)
            if linked:parts['game:'+type_id]=initial
            ops.append(dict(op='template.set',id=template,name='Linked' if linked else 'Plain',components=parts))
        authored=process.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=ops))
        revision=authored['revision'];before=(run/'world.json').read_bytes();process.start(revision)
        config=dict(descriptor=native(args.native_descriptor)) if args.native_descriptor else dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type='Poima.Tests.ManagedLifecycleGame')
        process.load(dict(config,values=dict(Mode=1)))
        request=dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=0,expected_structure_revision=0,ticks=1)
        result=process.rpc('runtime.step',request);process.tick=1
        game=process.game();assert game['structure_revision']==1
        values=game['module']['values'];first,second=values['First'],values['Second']
        def component(entity):
            return process.rpc('runtime.component.get',dict(session_id=process.session,tick=process.tick,structure_revision=1,type=type_id,id=entity))
        saved_component=component(first)
        assert saved_component['values'][uid(1)]==11 and saved_component['values'][uid(2)]==second
        retry=process.rpc('runtime.step',request);assert retry==dict(result,replayed=True)
        rejected=dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=1,ticks=1)
        process.rpc('runtime.step',rejected,error=-32602)
        process.rpc('runtime.step',dict(rejected,expected_structure_revision=0),error=-32009)
        evidence['checks'].append('Actual C# births increment structural revision; original step receipt remains recoverable and unguarded/stale next steps reject')
        if not args.native_descriptor:
            reload=dict(config,session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=1,expected_revision=game['revision'],expected_structure_revision=1)
            process.rpc('runtime.gameplay.load',reload)
            assert process.game()['module']['values']==values and component(first)['values']==saved_component['values']
            evidence['checks'].append('Compatible CoreCLR reload retains spawned membership and initialized custom/global values')
        root=run/'saves';root.mkdir()
        process.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(root)))
        write=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='births',expected_generation=0,session_id=process.session,
                   expected_tick=1,expected_gameplay_revision=process.game()['revision'],expected_component_revision=process.components()['component_revision'],expected_structure_revision=1)
        saved=process.rpc('save.write',write);assert saved['generation']==1
        edit=dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=1,expected_revision=process.game()['revision'],expected_structure_revision=1,values=dict(Mode=4))
        process.rpc('runtime.gameplay.edit',edit)
        process.rpc('runtime.step',dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=1,expected_structure_revision=1,ticks=1));process.tick=2
        assert process.game()['structure_revision']==2
        process.rpc('runtime.entity',dict(session_id=process.session,id=first),error=True)
        new_session=uuid.uuid4().hex
        load=dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='births',expected_generation=1,revision=revision,
                  expected_session_id=process.session,expected_tick=2,expected_gameplay_revision=process.game()['revision'],
                  expected_component_revision=process.components()['component_revision'],expected_structure_revision=2,new_session_id=new_session)
        process.rpc('save.load',dict(load,expected_structure_revision=1),error=-32009)
        restored=process.rpc('save.load',load);process.session=new_session;process.tick=restored['tick'];assert process.tick==1
        assert process.game()['module']['values']==values and component(first)['values']==saved_component['values']
        process.rpc('runtime.step',dict(session_id=process.session,request_id=uuid.uuid4().hex,expected_tick=1,expected_structure_revision=1,ticks=1));process.tick=2
        assert process.game()['module']['values']['SeenFirst']==11
        def observed():
            return dict(runtime=process.rpc('runtime.inspect',dict(session_id=process.session)),game=process.game(),component=component(first))
        before_retry=observed()
        assert process.rpc('save.write',write)==dict(saved,replayed=True)
        assert observed()==before_retry
        assert (run/'world.json').read_bytes()==before
        evidence['checks'].append('C# reference repair/removal, guarded durable v3 restore and continued gameplay preserve authored bytes; old save receipt does not mutate the restored runtime')
        evidence['passed']=True
    finally:
        if process:process.close()
        evidence['rpc_count']=len(evidence['calls']);(run/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
        print(run/'evidence.json')

if __name__=='__main__':main()
