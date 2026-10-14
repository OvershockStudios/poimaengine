#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent native Switchback gameplay gate; no compilation or candidate edits.

Windows Python is required. The supervisor assigns a kill-on-close Job before
releasing its worker; all native lifetimes descend from that worker. Failed
reports and their raw RPC/input records are retained. No state/pose writes are
used to satisfy gameplay rules.
"""
from pathlib import Path
import argparse
import os
import sys
import json
import hashlib
import time
import uuid
import copy
import traceback
import ctypes

sys.dont_write_bytecode=True
os.environ['PYTHONDONTWRITEBYTECODE']='1'

def identity(path):
    path=Path(path)
    assert path.is_file() and not path.is_symlink() and not getattr(path,'is_junction',lambda:False)(), 'Regular input required'
    with path.open('rb') as stream:
        return {'bytes':path.stat().st_size,'sha256':hashlib.file_digest(stream,'sha256').hexdigest()}

def inventory(directory):
    directory=Path(directory)
    assert directory.is_dir() and not directory.is_symlink() and not getattr(directory,'is_junction',lambda:False)()
    result={}; folded=set()
    for path in sorted(directory.rglob('*')):
        assert not path.is_symlink() and not getattr(path,'is_junction',lambda:False)(), 'Input link/junction forbidden'
        if path.is_dir(): continue
        name=path.relative_to(directory).as_posix()
        assert name.casefold() not in folded and len(result)<10000, 'Input membership exceeds bound/collides'
        folded.add(name.casefold()); result[name]=identity(path)
    return result

def worker(plan_path):
    sys.dont_write_bytecode=True
    plan=json.loads(plan_path.read_text(encoding="utf-8")); out=Path(plan['output']); deadline=time.monotonic()+plan['seconds']
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetCurrentProcess.restype=ctypes.c_void_p
    kernel.IsProcessInJob.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)]
    inside=ctypes.c_int()
    assert kernel.IsProcessInJob(kernel.GetCurrentProcess(),None,ctypes.byref(inside)) and inside.value
    def admit():
        assert time.monotonic()<deadline
        for path,expected in plan['pins'].items(): assert identity(path)==expected,path
        for path,expected in plan['inventories'].items(): assert inventory(Path(path))==expected,path
    # An actual inherited kill-on-close Job must exist before SDK/native imports.
    from switchback_ownership import Job
    class Extended(ctypes.Structure):
        _fields_ = [('basic', Job.Basic), ('io', Job.IO), ('process_limit', ctypes.c_size_t), ('job_limit', ctypes.c_size_t), ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
    limits=Extended()
    kernel.QueryInformationJobObject.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p]
    assert kernel.QueryInformationJobObject(None,9,ctypes.byref(limits),ctypes.sizeof(limits),None)
    assert limits.basic.flags & 0x2000
    admit()
    import switchback_oracle as oracle
    handoff=oracle.read_handoff(plan['handoff'])
    sys.path.insert(0,plan['sdk'])
    report={'limits': ['No physical-input, image quality, rendered observation, source-absence or fresh-provider workflow claim', 'Gameplay transport is recorded explicitly; --bundle alone is inventory only, --game executes the bundled readonly service'], 'format':'poima.switchback-independent-campaign.v1','passed':False,'scope':'Explicit native inputs, compiled callbacks, lenses and selected full snapshot restoration; no rendered/physical-input/fresh-workflow claim','checks':[],'owners':[],'checkpoints':{},'plan':identity(plan_path)}
    owners=[]; drivers=[]
    storage=out/'saves'; storage.mkdir()
    def start(label,save_root=None):
        selected_storage=storage if save_root is None else save_root
        if plan.get('game'):
            from switchback_game_service import GameOwner
            endpoint='switchback-'+uuid.uuid4().hex
            expected_host=(plan['engine'],'game','serve',plan['game'],'--endpoint',endpoint,'--save-root',str(selected_storage))
            expected_connect=(plan['engine'],'connect',endpoint,'--timeout-ms','12000')
            def command_admission(argv): return argv in (expected_host,expected_connect)
            o=GameOwner(plan['engine'],plan['game'],handoff,admit,deadline,out/('service-'+label),selected_storage,
                        endpoint=endpoint,command_admission=command_admission)
        else:
            o=oracle.Owner(plan['engine'],plan['world'],handoff,admit,deadline,
                command_admission=lambda argv: argv==(plan['engine'],'world',plan['world']))
        owners.append((label,o)); report['owners'].append({'label':label,'argv':list(o.argv),'pid':o.transport._process.pid,'transport':'game.serve' if plan.get('game') else 'world'})
        o.rpc('world.describe',view='method',name='save.load')
        if plan.get('game'):
            world=o.rpc('world.inspect')
            assert world['read_only'] is True and world['mode']=='read_only_runtime'
            status=o.rpc('runtime.status')
            assert status['active'] is True and status['tick']==0
            o.session=oracle.identifier(status['session_id'])
            save_status=o.rpc('save.status')
            assert save_status['generation']==1 and Path(save_status['root']).resolve()==selected_storage.resolve()
            assert save_status['write_mode']=='synchronous'
            assert o.rpc('player.inspect')['active'] is False
        else:
            o.rpc('save.configure',request_id=uuid.uuid4().hex,expected_generation=0,root=str(selected_storage))
            rev=o.rpc('world.inspect')['revision']; o.start(rev)
            o.rpc('runtime.gameplay.load_native',session_id=o.session,request_id=uuid.uuid4().hex,
                expected_tick=0,expected_revision=0,descriptor=plan['descriptor'],expected_descriptor_sha256=plan['descriptor_sha256'],values={})
        obs=o.observe(); assert obs['module']['backend']=='native_aot'
        assert obs['module']['assembly_sha256']==plan['assembly_sha256']
        assert Path(obs['module']['native_library']).resolve()==Path(plan['native_library']).resolve()
        assert obs['module']['schema']['identity']==plan['game_identity']
        schema={field['name']:field for field in obs['module']['schema']['fields']}
        assert all(schema[name]['kind']=='int32' for name in handoff['fields'].values())
        if handoff['format']=='poima.switchback-exercise.v1':
            assert schema[handoff['presentation']['save_status_field']]['kind']=='int32'
        return o
    def load(o,slot):
        state=o.inspect(); game=o.rpc('runtime.gameplay.inspect',session_id=o.session,tick=state['tick'])
        components=o.rpc('runtime.components',session_id=o.session)
        r=o.rpc('save.load',request_id=uuid.uuid4().hex,configuration_generation=1,
            slot=slot,expected_generation=1,revision=o.rpc('world.inspect')['revision'],new_session_id=uuid.uuid4().hex,
            gameplay={'descriptor':plan['descriptor'],'expected_descriptor_sha256':plan['descriptor_sha256']},
            expected_session_id=o.session,expected_tick=state['tick'],expected_gameplay_revision=game['revision'],
            expected_component_revision=components['component_revision'],expected_structure_revision=state['structure_revision'],
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'])
        o.session=r['session_id']; return r
    def checked_control(d,role):
        receipt=d.owner.control(role); d.golden.control(role)
        obs=d.owner.observe(query_hit=False)
        d.golden.assert_observed(obs['module']['values'],obs['lenses'],handoff['fields'])
        return receipt
    try:
        o=start('positive'); report['import_binding']=oracle.inspect_import_binding(o)
        d=oracle.CheckedSession(o); drivers.append(('positive',d))
        obs=o.observe(); d.golden.assert_observed(obs['module']['values'],obs['lenses'],handoff['fields'])
        for i in range(12): d.tick(aim=True,crouch=True,fire=True,reload=True,use=(i%2==0))
        assert abs(o.observe()['foot'][1])<=.05
        assert abs(o.observe()['target_b']['world_matrix'][12]-3)<1e-8
        report['checks'].append('Ready-gates-twelve-explicit-input-ticks')
        d.tick(aim=False,crouch=False,fire=False,reload=False)
        checked_control(d,'begin')
        d.tick(fire=True); d.tick(fire=False)
        for i in range(13): d.tick(aim=True)
        for i in range(7): d.tick(aim=False)
        for i in range(13): d.tick(aim=True)
        report['checks'].append('empty-fire-rejection-and-independent-ADS-endpoints/reversal')
        checked_control(d,'menu'); frozen=o.observe()
        for i in range(8): d.tick(aim=False,crouch=True,fire=(i%2==0),reload=(i%2==0),use=(i%2==0))
        now=o.observe()
        before_position=frozen['target_b']['world_matrix'][12:15]
        after_position=now['target_b']['world_matrix'][12:15]
        # The task freezes horizontal patrol during Menu. Explicit runtime
        # steps still run physics; float contact settling is not patrol.
        assert after_position[0]==before_position[0] and after_position[2]==before_position[2]
        assert abs(after_position[1]-before_position[1])<=1e-6 and abs(after_position[1])<=.05
        report['menu_patrol_gate']={'before':before_position,'after':after_position,
            'horizontal_exact':True,'vertical_contact_settling_limit_metres':1e-6}
        checked_control(d,'resume'); d.tick(aim=False,crouch=False,fire=False,reload=False)
        report['checks'].append('compiled-menu/resume-and-explicit-step-gating')
        d.aim_at([-3,1.3,-6]); hit=o.observe()['hit']
        assert hit is not None and o.observe()['hit_bit']==0 and hit['distance']<11
        report['checks'].append('authored-cover-occludes-start-ray')
        d.tick(crouch=True)
        for x,z in [(-4.7,2),(-4.7,0),(-3,0)]: d.drive_to(x,z,max_ticks=300,crouch=True,fire=False,reload=False,aim=False)
        d.tick(use=True,reload=True,crouch=True)
        assert d.golden.Supply==1 and d.golden.Ammo==4 and d.golden.ReloadRemaining==0
        d.tick(use=False,reload=False,crouch=False)
        under=o.observe(); assert under['stance']['crouched'] and under['stance']['stand_blocked']
        report['checks'].append('committed-crouch-pickup/reload-order-and-blocked-stand')
        d.drive_to(-4.7,0,max_ticks=200,crouch=False)
        assert not o.observe()['stance']['crouched']
        d.aim_at([-3,1.3,-6],fire=False)
        assert o.observe()['hit_bit']==1
        d.tick(fire=True)
        for i in range(4): d.tick(fire=True)
        assert d.golden.Shots==1 and d.golden.ClearedMask==1
        d.tick(fire=False)
        d.drive_to(-4.7,-3,max_ticks=200,fire=False)
        state=o.inspect(); region=o.rpc('runtime.entity',session_id=o.session,tick=state['tick'],structure_revision=state['structure_revision'],id=handoff['roles']['region_b'])
        d.aim_at(region['world_matrix'][12:15],fire=False)
        assert o.observe()['hit_bit']==2
        d.tick(fire=True); d.tick(fire=False)
        assert d.golden.Shots==2 and d.golden.ClearedMask==3
        report['checks'].append('real-camera-hit-tests-A/B-and-held-fire-single-edge')
        before=o.observe(); records=d.frames.next(fire=True,aim=False,reload=False,crouch=False)
        batch_golden=copy.deepcopy(d.golden)
        for i in range(6):
            frame=copy.deepcopy(records)
            if i:
                for action in frame.values(): action['pressed']=False; action['released']=False
            batch_golden.tick(before,frame)
        step=o.step(records,ticks=6)
        after=o.observe(query_hit=False)
        batch_golden.assert_observed(after['module']['values'],after['lenses'],handoff['fields'])
        d.golden=batch_golden
        report['batch_edge']={'before':before,'records':records,'step':step,'after':after,'golden':oracle.asdict(batch_golden)}
        assert d.golden.Shots==3 and d.golden.Ammo==1 and d.golden.ClearedMask==3
        d.tick(fire=False)
        report['checks'].append('six-tick-native-batch/single-initial-fire-edge')
        d.tick(reload=True,aim=True)
        for i in range(3): d.tick(reload=False,aim=True)
        assert d.golden.ReloadRemaining==9 and d.golden.ads_elapsed==3
        first=o.checkpoint(1,storage,'mid-reload-and-ADS')
        report['checkpoints']['mid-reload-and-ADS']=first
        retained=copy.deepcopy(d.golden); held=copy.deepcopy(d.frames.held)
        d.tick(aim=False); load(o,'oracle-1')
        same=o.checkpoint(1,storage,'same-owner-restored')
        report['checkpoints']['same-owner-restored']=same
        assert oracle.exact_checkpoint(first,same)
        d=oracle.CheckedSession(o,retained); d.frames.held=copy.deepcopy(held); drivers.append(('same-owner-restored',d))
        d.tick(aim=True,reload=False,fire=False,crouch=False)
        report['checks'].append('full-v9-same-owner-mid-reload/ADS-restore-and-next-tick')
        o.close()
        continuation=[{'aim':True},{'aim':False},{'aim':False},{'aim':True},{'aim':True}]
        for label,restored_label,continued_label in (
            ('cold-reference','cold-reference-restored','cold-reference-continued'),
            ('cold-fresh','cold-fresh-restored','cold-fresh-continued')):
            o=start(label); load(o,'oracle-1')
            restored=o.checkpoint(1,storage,restored_label)
            report['checkpoints'][restored_label]=restored
            assert oracle.exact_checkpoint(first,restored)
            d=oracle.CheckedSession(o,retained); d.frames.held=copy.deepcopy(held); drivers.append((label,d))
            for frame in continuation: d.tick(fire=False,reload=False,crouch=False,**frame)
            report['checkpoints'][continued_label]=o.checkpoint(1,storage,continued_label)
            if label=='cold-reference': o.close()
        a=report['checkpoints']['cold-reference-continued']; b=report['checkpoints']['cold-fresh-continued']
        assert oracle.exact_checkpoint(a,b)
        report['checks'].append('two-sequential-fresh-native-owners/full-v9-restores-and-identical-continuation')
        checked_control(d,'menu'); menu=o.checkpoint(1,storage,'menu-checkpoint')
        report['checkpoints']['menu-checkpoint']=menu; menu_held=copy.deepcopy(d.frames.held)
        d.tick(aim=False,fire=True,reload=True,use=True)
        load(o,'oracle-7'); restored_menu=o.checkpoint(1,storage,'menu-restored')
        report['checkpoints']['menu-restored']=restored_menu
        assert oracle.exact_checkpoint(menu,restored_menu)
        d.frames.held=menu_held
        save_control=checked_control(d,'save')
        save_presentation={}
        if handoff['format']=='poima.switchback-exercise.v2':
            save_operation=save_control['save_operation']
            assert save_control['save_serviced'] and save_operation['kind']==1 and save_operation['state']==3 and save_operation['error_code']==0
            save_presentation['queued']=oracle.inspect_save_presentation(o,'queued')
        game_slot=o.rpc('save.inspect',slot='switchback-checkpoint')['selected']
        assert game_slot['verified'] and game_slot['generation']==2
        if handoff['format']=='poima.switchback-exercise.v2':
            assert save_operation['generation']==game_slot['generation']
        d.tick(aim=False,fire=False,reload=False)
        if handoff['format']=='poima.switchback-exercise.v1':
            assert o.observe()['module']['values'][handoff['presentation']['save_status_field']]==handoff['presentation']['save_status_complete']
        else:
            save_presentation['succeeded']=oracle.inspect_save_presentation(o,'succeeded')
        source_save_status=o.rpc('runtime.save.status',session_id=o.session)
        restored_by_game=o.control('load')
        operation=restored_by_game['save_operation']
        assert restored_by_game['runtime_replaced'] and restored_by_game['save_serviced']
        assert operation['kind']==2 and operation['state']==3 and operation['error_code']==0
        assert operation['generation']==game_slot['generation']
        assert operation['restored_tick']==restored_by_game['current_tick']
        assert operation['epoch']==source_save_status['epoch']
        assert operation['restored_epoch']!=operation['epoch']
        restore_status=o.rpc('runtime.save.status',session_id=o.session)
        assert restore_status['epoch']==operation['restored_epoch']
        last_restore=restore_status['last_restore']
        assert last_restore is not None
        assert last_restore['initiating_epoch']==operation['epoch']
        assert last_restore['initiating_sequence']==operation['sequence']
        assert last_restore['source_tick']==operation['committed_tick']
        assert last_restore['restored_tick']==operation['restored_tick']
        assert last_restore['generation']==operation['generation']
        assert last_restore['recovered']==operation['recovered']
        if handoff['format']=='poima.switchback-exercise.v1':
            report['compiled_save_load']={'selected':game_slot,'restore':restored_by_game,'restore_status':restore_status,'source_save_status':source_save_status,
                'presentation_field':handoff['presentation']['save_status_field'],
                'presentation_complete':handoff['presentation']['save_status_complete']}
        else:
            report['compiled_save_load']={'selected':game_slot,'restore':restored_by_game,'restore_status':restore_status,'source_save_status':source_save_status,
                'save_control':save_control,'presentation':save_presentation}
        observed=o.observe(); d.golden.assert_observed(observed['module']['values'],observed['lenses'],handoff['fields'])
        d.tick(aim=False,fire=False,reload=False)
        if handoff['format']=='poima.switchback-exercise.v1':
            assert o.observe()['module']['values'][handoff['presentation']['save_status_field']]==handoff['presentation']['save_status_complete']
        else:
            save_presentation['restored']=oracle.inspect_save_presentation(o,'restored')
        report['checks'].append('full-v9-menu-pair-and-compiled-game-save/load-ticket-reconciliation')
        checked_control(d,'resume')
        for x,z in [(-4.7,2),(0,2),(0,5)]: d.drive_to(x,z,max_ticks=300,aim=False,fire=False,reload=False,crouch=False)
        ammo=d.golden.Ammo; shots=d.golden.Shots
        d.tick(use=True,fire=True)
        assert d.golden.Phase==3 and d.golden.Ammo==ammo and d.golden.Shots==shots
        for i in range(8): d.tick(use=(i%2==0),fire=(i%2==0),reload=(i%2==0),aim=(i%2==0))
        completed=o.checkpoint(1,storage,'completed'); report['checkpoints']['completed']=completed
        sound=completed['snapshot']['payload']['sound']
        assert sound['next_voice_id']==5 and len(sound['voices'])==4
        assert [voice['emitter'] for voice in sound['voices']]==oracle.expected_cue_emitters(handoff)
        oracle.assert_nine_checkpoints(report['checkpoints'])
        report['checks'].append('completed-native-course/winning-Use-before-Fire-and-terminal-gates')
        report['checks'].append('nine-full-v9-checkpoints/five-exact-pairs/no-field-exclusions-and-four-native-cues')
        o.close()
        negative_storage=out/'negative-saves'; negative_storage.mkdir()
        o=start('negative',negative_storage); d=oracle.CheckedSession(o); drivers.append(('negative',d))
        for i in range(12): d.tick()
        checked_control(d,'begin'); initial_golden=copy.deepcopy(d.golden)
        initial_frames=copy.deepcopy(d.frames.held)
        selected=o.rpc('save.inspect',slot='switchback-checkpoint')['selected']
        assert selected['verified'] and selected['generation']==1
        initial=oracle.read_checkpoint(negative_storage,'switchback-checkpoint',selected)
        d.tick(use=True); d.tick(use=False)
        assert d.golden.Rejected==1
        d.drive_to(0,-3.4,max_ticks=250)
        for expected_health in (2,1):
            d.drive_to(0,-4,max_ticks=80)
            for i in range(4): d.tick()
            assert d.golden.Health==expected_health
            d.drive_to(0,-3.4,max_ticks=80)
        d.drive_to(0,-3.56,max_ticks=80,tolerance=.02)
        d.tick(move=(0,1),fire=False)
        assert d.golden.Health==1 and abs(o.observe()['foot'][2]+4)<.4
        rejected=d.golden.Rejected; timer=d.golden.Timer
        d.tick(fire=True,use=True)
        assert d.golden.Phase==2 and d.golden.Health==0 and d.golden.Rejected==rejected and d.golden.Timer==timer+1
        for i in range(6): d.tick(fire=(i%2==0),reload=(i%2==0),aim=(i%2==0),use=(i%2==0))
        assert o.control('retry')['runtime_replaced']
        restored=o.checkpoint(1,negative_storage,'same-owner-restored')
        assert oracle.exact_checkpoint(restored,initial)
        d=oracle.CheckedSession(o,initial_golden); d.frames.held=initial_frames; drivers.append(('after-real-retry',d))
        d.tick()
        report['negative']={'initial':initial,'after_retry':restored,'checks':['premature-finish-rejected','one-damage-per-outside/inside-entry','lethal-entry-before-simultaneous-use/fire','Dead-input-gates','compiled-Retry-exact-full-native-initial-checkpoint','next-native-tick-after-retry']}
        report['checks'].append('native-hazard/death-gates-and-compiled-full-checkpoint-Retry')
        report['passed']=True
    except BaseException: report['error']=traceback.format_exc()
    finally:
        for label,o in owners:
            try: o.close()
            except BaseException: report.setdefault('close_errors',[]).append(traceback.format_exc()); report['passed']=False
            payload={'service':o.evidence() if hasattr(o,'evidence') else None,'argv':list(o.argv),'pid':o.transport._process.pid,'returncode':o.transport.returncode,'forced':o.forced,'calls':o.calls,'raw':o.raw}
            data=json.dumps(payload,allow_nan=False).encode(); assert len(data)<=128*1048576
            (out/(label+'-rpc.json')).write_bytes(data)
            report['owners'][[r['label'] for r in report['owners']].index(label)].update(identity=identity(out/(label+'-rpc.json')),forced=o.forced,returncode=o.transport.returncode)
        for i,(label,d) in enumerate(drivers):
            data=json.dumps({'label':label,'records':d.records,'golden':oracle.asdict(d.golden)},allow_nan=False).encode()
            assert len(data)<=128*1048576; (out/('inputs-'+str(i)+'.json')).write_bytes(data)
        try: admit()
        except BaseException: report['input_error']=traceback.format_exc(); report['passed']=False
        (out/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'passed':report['passed'],'checks':report['checks'],'error':report.get('error'),'owners':report['owners']}))
    return 0 if report['passed'] else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,help='Required for --project/--world; --game uses its exact bundled engine')
    selection=parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--world',type=Path)
    selection.add_argument('--project',type=Path)
    selection.add_argument('--game',type=Path,help='Execute actual readonly game.serve from this game.json with the bundled engine')
    parser.add_argument('--native-descriptor',type=Path,help='Required with --world; defaults to project gameplay descriptor with --project')
    parser.add_argument('--metadata',type=Path,required=True,help='Public exercise.json identity/presentation map')
    parser.add_argument('--bundle',type=Path,help='Optional game.json: retain its unchanged file inventory; NOT a bundle runtime/relocation check')
    parser.add_argument('--sdk',type=Path,help='Directory containing public poima_client package; defaults to checkout tools/python')
    parser.add_argument('--output',type=Path,required=True,help='Fresh output directory; existing paths are rejected')
    parser.add_argument('--timeout',type=int,default=360)
    args=parser.parse_args()
    assert sys.version_info >= (3,11), 'Verifier requires Python 3.11 or newer'
    assert os.name=='nt', 'Use native Windows Python; Linux/WSL Python cannot supervise the Windows Job'
    assert not sys.flags.optimize, 'Assertions must remain enabled'
    assert 60<=args.timeout<=1800, 'Campaign budget must be 60..1800 seconds'
    import switchback_oracle as oracle
    binary=args.binary.resolve(strict=True) if args.binary else None; metadata=args.metadata.resolve(strict=True)
    oracle.read_handoff(metadata)
    world=args.world.resolve(strict=True) if args.world else None
    descriptor=args.native_descriptor.resolve(strict=True) if args.native_descriptor else None
    project=args.project.resolve(strict=True) if args.project else None
    game=args.game.resolve(strict=True) if args.game else None
    if game:
        data=oracle.strict_json(game.read_bytes())
        assert data['format']=='poima.game' and data['version']==2
        def member(name):
            assert type(name) is str and name and not Path(name).is_absolute() and '\\' not in name
            assert all(part not in ('..','.') for part in name.split('/'))
            target=(game.parent/name).resolve(strict=True)
            assert target.is_relative_to(game.parent)
            return target
        selected_binary=member(data['engine']['executable'])
        assert binary is None or binary==selected_binary, '--game must use its exact bundled executable'
        binary=selected_binary
        world=member(data['entry']['world'])
        selected_descriptor=member(data['gameplay']['descriptor'])
        assert descriptor is None or descriptor==selected_descriptor, '--game must use its actual bundled descriptor'
        descriptor=selected_descriptor
        assert data['gameplay']['values']=={}, 'Campaign requires frozen initial gameplay defaults'
        assert data['entry']['controller']==oracle.read_handoff(metadata)['roles']['player']
        assert data['entry']['camera']==oracle.read_handoff(metadata)['roles']['camera']
    if project:
        data=oracle.strict_json(project.read_bytes())
        assert data['format']=='poima.project' and data['version'] in (1,2)
        def project_member(name):
            assert type(name) is str and name and not Path(name).is_absolute() and '\\' not in name
            assert all(part not in ('..','.') for part in name.split('/'))
            target=(project.parent/name).resolve(strict=True)
            assert target.is_relative_to(project.parent)
            return target
        world=project_member(data['entry']['world'])
        if descriptor is None: descriptor=project_member(data['gameplay']['descriptor'])
        assert data.get('gameplay',{}).get('values',{})=={}, 'Campaign requires frozen initial gameplay defaults'
    assert binary is not None, '--project/--world requires --binary'
    assert descriptor is not None, '--world requires --native-descriptor'
    native=oracle.strict_json(descriptor.read_bytes())
    native_library=descriptor.parent/native['library']
    assert native_library.resolve().is_relative_to(descriptor.parent)
    assert native['files'] and any(row['path']==native['library'] and row['sha256']==identity(native_library)['sha256'] for row in native['files'])
    sdk=args.sdk.resolve(strict=True) if args.sdk else None
    if sdk is None:
        for ancestor in Path(__file__).resolve().parents:
            if (ancestor/'tools/python/poima_client').is_dir(): sdk=ancestor/'tools/python'; break
    assert sdk is not None and (sdk/'poima_client/core_responses.v1.json').is_file(), 'Public Python SDK required'
    sdk_files=inventory(sdk/'poima_client')
    assert not any('__pycache__' in name or name.endswith('.pyc') for name in sdk_files), 'Use a clean SDK copy; verifier does not remove user files'
    output=args.output.resolve()
    assert not output.exists(), 'Output must be fresh; failed runs are retained'
    closure=[world,descriptor,metadata,binary,Path(__file__).resolve(),Path(__file__).with_name('switchback_oracle.py'),Path(__file__).with_name('switchback_ownership.py'),Path(__file__).with_name('switchback_game_service.py'),Path(sys.executable).resolve()]
    if project: closure.append(project)
    if game: closure.append(game)
    closure.extend(path for path in binary.parent.iterdir() if path.is_file() and path.suffix.lower()=='.dll')
    inputs={str(path):identity(path) for path in closure}
    directories={str(descriptor.parent):inventory(descriptor.parent),str(sdk/'poima_client'):sdk_files}
    assets=world.with_name(world.name+'.assets')
    if assets.exists(): directories[str(assets)]=inventory(assets)
    if game: directories[str(game.parent)]=inventory(game.parent)
    bundle=None
    if args.bundle:
        bundle=args.bundle.resolve(strict=True)
        assert bundle.is_file() and bundle.name=='game.json'
        inputs[str(bundle)]=identity(bundle)
        directories[str(bundle.parent)]=inventory(bundle.parent)
    assert all(not output.is_relative_to(Path(path)) for path in directories), 'Output cannot be inside an input closure'
    assert all(not Path(path).is_relative_to(output) for path in inputs), 'Output cannot contain inputs'
    output.mkdir(parents=True,exist_ok=False)
    plan={'format':'poima.switchback-campaign-plan.v1','seconds':args.timeout,'output':str(output),'engine':str(binary),'world':str(world),
        'game':str(game) if game else None,'assembly_sha256':identity(native_library)['sha256'],'native_library':str(native_library.resolve()),'game_identity':native['identity'],'descriptor':str(descriptor),'descriptor_sha256':inputs[str(descriptor)]['sha256'],'handoff':str(metadata),'sdk':str(sdk),
        'pins':inputs,'inventories':directories,'bundle_inventory_only':str(bundle) if bundle else None}
    plan_path=output/'plan.json'; plan_path.write_text(json.dumps(plan,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    from switchback_ownership import gated
    result=gated([sys.executable,str(Path(__file__).resolve()),'--worker',str(plan_path)],output/'job',args.timeout+30)
    passed=result['passed']
    report=None
    if (output/'report.json').exists():
        report=oracle.strict_json((output/'report.json').read_bytes()); passed=bool(passed and report['passed'])
    else: passed=False
    summary={'format':'poima.switchback-verification.v1','passed':passed,'seconds':result['seconds'],
        'checks':report['checks'] if report else [],'owner':identity(output/'job/owner.json'),
        'campaign':identity(output/'report.json') if report else None,
        'limits':report['limits'] if report else ['Worker did not publish a campaign report']}
    (output/'verification.json').write_text(json.dumps(summary,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps(summary,allow_nan=False))
    return 0 if passed else 1

if __name__=='__main__':
    assert not sys.flags.optimize, 'Assertions must remain enabled'
    if len(sys.argv)==3 and sys.argv[1]=='--worker':
        assert os.name=='nt'
        raise SystemExit(worker(Path(sys.argv[2])))
    raise SystemExit(main())
