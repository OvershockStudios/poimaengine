# SPDX-License-Identifier: Apache-2.0
"""Independent Pump House consumer; explicit plan, outer Windows Job required.
No compilation, source edits, teleportation or authoritative state setters.
"""
from pathlib import Path
import argparse, copy, hashlib, json, math, os, sys, time, traceback, uuid
sys.dont_write_bytecode = True
ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent/'tests/workflow_components_game_service.py').is_file())
sys.path.insert(0, str(ROOT/'tests'))
from workflow_components_game_service import Checked, FIELDS, assigned_job, exact, need, pin, read, tree, write
from workflow_save_upgrade import stored

E = lambda value: f'{value:032x}'
U = lambda value: f'7000000000000000{value:016x}'
PLAYER, CAMERA = E(2), E(3)
ITEMS = [E(0x10),E(0x11),E(0x12)]
BODIES = ITEMS+[E(0x21),E(0x23)]
PROGRESS = ['FuseCount','CogCount','FuseAPresent','FuseBPresent','CogPresent','BreakerOn','BridgeRaised','PumpOn','Objective']
ACTIONS = {'start':0xb3,'pause':0xa6,'resume':0xc2,'save':0xc3,'load':0xc4,'newgame':0xc5}
SLOT = 'checkpoint'


def pos(entity):
    return entity['world_matrix'][12:15]


def wrap(angle):
    return (angle+180)%360-180


def signed_halves(epoch):
    need(type(epoch) is str and len(epoch)==32 and epoch!='0'*32
         and all(c in '0123456789abcdef' for c in epoch),'Actual epoch ID required')
    values=[int(epoch[start:start+16],16) for start in (0,16)]
    return [str(value if value<2**63 else value-2**64) for value in values]


class Pump(Checked):
    def __init__(self, plan, directory, deadline, descriptor, ids):
        self.descriptor,self.ids,self.report = descriptor,ids,{}
        super().__init__(Path(plan['engine']),directory,deadline,
            world=None if plan['game'] else Path(plan['world']),
            game=Path(plan['game']) if plan['game'] else None,saves=Path(plan['saves']))
        self.metadata=read(descriptor)
        libraries=[row for row in self.metadata['files'] if row['role']=='library']
        need(len(libraries)==1,'Exactly one native gameplay library required')
        self.library=descriptor.parent/libraries[0]['path']
        need(pin(self.library)==dict(bytes=libraries[0]['size'],sha256=libraries[0]['sha256']),'Trusted library differs')
        if not plan['game']:
            self.session,self.tick=uuid.uuid4().hex,0
            self.call('runtime.start',dict(session_id=self.session,revision=self.call('world.inspect')['revision']))
            self.call('runtime.gameplay.load_native',dict(session_id=self.session,request_id=uuid.uuid4().hex,
                expected_tick=0,expected_revision=0,descriptor=str(descriptor)))
        self.module()

    def call(self, method, params=None, error=None):
        need(method=='host.shutdown' or len(self.calls)<4000,'Bounded owner exceeded 4000 RPCs')
        return super().call(method,params,error)

    def state(self):
        return self.call('runtime.inspect',dict(session_id=self.session))

    def module(self):
        result=self.game();module=result['module']
        need(module is not None and module['backend']=='native_aot'
             and module['assembly_sha256']==pin(self.library)['sha256']
             and exact(module['schema'],self.metadata['schema'])
             and Path(module['native_library']).resolve()==self.library.resolve(),'Actual trusted gameplay image/schema/path differs')
        return result

    def values(self):
        return self.module()['module']['values']

    def entity(self, identity):
        return self.call('runtime.entity',dict(session_id=self.session,tick=self.tick,id=identity))

    def step(self, ticks=1, *, move=None, look=None, use=False):
        need(type(ticks) is int and 1<=ticks<=600,'Bounded integer step required')
        state=self.state()
        params=dict(session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=self.tick,
            expected_structure_revision=state['structure_revision'],ticks=ticks)
        if move is not None or look is not None or use:
            params['inputs']=[dict(entity=PLAYER,move=list(move or (0,0)),look=list(look or (0,0)),use=use)]
        result=self.call('runtime.step',params)
        need(not result['runtime_replaced'] and result['current_session_id']==self.session
             and result['current_tick']==self.tick+ticks,'Unexpected callback replacement or tick count')
        self.tick=result['current_tick']
        return result

    def ui(self):
        result=self.call('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256))
        need(result['next_after'] is None,'Complete UI exceeds one page')
        return result

    def activate(self, action):
        ui=self.ui();state=self.state();module=self.module()
        identity=U(ACTIONS[action])
        if action=='load' and ui['modal']==U(0xb0):identity=U(0xb4)
        candidates=[row for row in ui['elements'] if row['id']==identity]
        need(len(candidates)==1 and candidates[0]['action']==action and candidates[0]['eligible'] is True,
             'Actual logical action is ineligible: '+action)
        result=self.call('runtime.ui.activate',dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,expected_ui_revision=ui['ui_revision'],expected_control_sequence=ui['control_sequence'],
            expected_gameplay_revision=module['revision'],expected_structure_revision=state['structure_revision'],id=identity))
        self.session,self.tick=result['current_session_id'],result['current_tick']
        return result

    def walk(self, x, z):
        history=[]
        for index in range(160):
            before=self.entity(PLAYER);p=pos(before);dx,dz=x-p[0],z-p[2];distance=math.hypot(dx,dz)
            if distance<=.14:
                self.report.setdefault('walks',[]).append(dict(goal=[x,z],final=p,observations=history));return p
            matrix=before['world_matrix'];right=(matrix[0],matrix[2]);forward=(-matrix[8],-matrix[10])
            direction=(dx/distance,dz/distance);ticks=max(1,min(12,int(distance/(4*.8)*60)))
            strength=min(.8,distance/(4*ticks/60))
            move=[strength*sum(a*b for a,b in zip(direction,axis)) for axis in (right,forward)]
            self.step(ticks,move=move);after=pos(self.entity(PLAYER))
            need(math.hypot(after[0]-p[0],after[2]-p[2])<=4*ticks/60+.0003,'Controller exceeded authored speed')
            history.append(dict(tick=self.tick,ticks=ticks,move=move,position=after))
            if index>=12:
                previous=history[-12]['position']
                need(math.hypot(after[0]-previous[0],after[2]-previous[2])>.005,'Native walk stalled against actual collision')
        raise AssertionError('Measured waypoint budget exhausted')

    def aim(self, point):
        for _ in range(6):
            camera=self.entity(CAMERA);player=self.entity(PLAYER);eye=pos(camera)
            dx,dy,dz=[point[i]-eye[i] for i in range(3)]
            yaw=math.degrees(math.atan2(-dx,-dz));pitch=math.degrees(math.atan2(dy,math.hypot(dx,dz)))
            dyaw,dpitch=wrap(yaw-player['yaw']),pitch-player['pitch']
            if abs(dyaw)<.15 and abs(dpitch)<.15:return
            self.step(look=(max(-180,min(180,dyaw)),max(-80,min(80,dpitch))))
        raise AssertionError('Native camera aim did not converge')

    def ray(self):
        camera=self.entity(CAMERA);m=camera['world_matrix']
        return self.call('runtime.raycast',dict(session_id=self.session,tick=self.tick,origin=pos(camera),
            direction=[-m[8],-m[9],-m[10]],distance=3,ignore=[PLAYER]))

    def use(self, identity, feedback=None):
        self.aim(pos(self.entity(identity)));self.step()  # Release prior Use edge.
        ray=self.ray();need(ray['hit'] is not None and ray['hit']['entity']==identity,'Actual camera ray misses target '+identity)
        before=copy.deepcopy(self.values());self.step(use=True);after=copy.deepcopy(self.values())
        need(after['Interactions']==before['Interactions']+1,'Actual compiled spatial interaction did not execute exactly once')
        if feedback is not None:need(after['Feedback']==feedback,'Compiled interaction feedback differs')
        self.report.setdefault('uses',[]).append(dict(target=identity,ray=ray,before=before,after=after,tick=self.tick))
        self.step();return after

    def guards(self):
        state=self.state();module=self.module();registry=self.call('runtime.components',dict(session_id=self.session))
        return dict(expected_session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=module['revision'],
            expected_component_revision=registry['component_revision'],**{'expected_'+key:state[key] for key in
            ('structure_revision','ui_revision','control_sequence','material_revision')})

    def save_probe(self, label):
        guards=self.guards();guards['session_id']=guards.pop('expected_session_id')
        slot='probe-'+uuid.uuid4().hex
        result=self.call('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=self.configuration,
            slot=slot,expected_generation=0,**guards))
        saved,path,row=stored(self.expected_save_root,slot)
        need(result['sha256']==row['sha256'],'Probe reply/disk identity differs')
        self.report.setdefault('snapshots',{})[label]=dict(saved=saved,path=str(path),identity=pin(path),receipt=result)
        return saved

    def selected(self):
        state=self.state();registry=self.call('runtime.components',dict(session_id=self.session))
        need(registry['schemas']==[] and state['entities']==len(self.ids),'Actual no-custom-component/live membership differs')
        observe=self.call('runtime.observe',dict(session_id=self.session,tick=self.tick,ids=self.ids,
            entity_fields=list(FIELDS),components=[],include_schemas=True,structure_revision=registry['structure_revision'],
            component_revision=registry['component_revision']))
        need([row['id'] for row in observe['entities']]==self.ids,'Complete observed membership differs')
        return dict(runtime=state,gameplay=self.module(),components=registry,observation=observe,ui=self.ui(),
                    save_status=self.call('runtime.save.status',dict(session_id=self.session)))

    def capture(self, label, enabled):
        if not enabled:return
        path=self.directory/(label+'.bmp');ui=self.ui()
        result=self.call('runtime.capture',dict(session_id=self.session,tick=self.tick,camera=CAMERA,
            ui_revision=ui['ui_revision'],path=str(path),width=1280,height=720,ui_scale=1))
        need(result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,'Actual native capture failed')
        self.report.setdefault('captures',{})[label]=dict(result=result,path=str(path),identity=pin(path),ui=ui)


def progress(owner, expected):
    values=owner.values()
    need(all(type(values[key]) is int and values[key]==value for key,value in expected.items()),'Gameplay progression differs: '+str(expected))
    return values


def body_y(owner, identity, expected):
    value=pos(owner.entity(identity))[1]
    need(abs(value-expected)<.002,'Native body height differs: '+identity+' '+str(value))
    return value


def inventory_world(owner):
    values=owner.values()
    return dict(values={key:values[key] for key in PROGRESS},bodies={identity:owner.entity(identity) for identity in BODIES})


def first_restore_callback(owner):
    before=copy.deepcopy(owner.values());status=owner.call('runtime.save.status',dict(session_id=owner.session))
    need(status['last_restore'] is not None and status['last_restore']['generation']==1
         and status['last_restore']['recovered'] is False
         and status['last_restore']['restored_tick']==owner.tick,'Native restore metadata missing or differs')
    high,low=signed_halves(status['epoch']);ray=owner.ray()
    codes={E(identity):code for identity,code in [(0x10,1),(0x11,2),(0x12,3),(0x20,4),(0x21,5),(0x22,6),(0x23,7),(0x24,8)]}
    look=0 if ray['hit'] is None else codes.get(ray['hit']['entity'],0)
    expected=copy.deepcopy(before)
    expected.update(SessionEpochHigh=high,SessionEpochLow=low,SaveSequence='0',SaveKindPending=0,
                    SaveStatus=6,Paused=0,ResetPending=0,HudDirty=0,LookCode=look)
    # This is the game's finite TrackSession/PollSave/UI forecast, with a real
    # camera ray. Every other typed global must survive, including ticket epochs.
    need(len(expected)==25 and before['Started']==1 and before['ResetPending']==0
         and [before['SessionEpochHigh'],before['SessionEpochLow']]!=[high,low]
         and [before['SessionEpochHigh'],before['SessionEpochLow']]!=['0','0'],
         'Invalid restore forecast precondition')
    owner.step();after=owner.values()
    need(exact(after,expected),'Full typed globals differ from explicit native-epoch/ray restore forecast')
    owner.report.setdefault('restore_callbacks',[]).append(dict(before=before,expected=expected,
        after=copy.deepcopy(after),actual_status=status,precallback_ray=ray))


def selected_payload(selected):
    # Only these explicit transport session envelopes differ across owners.
    # The raw selections, save-owner lifecycle metadata and trusted module
    # diagnostics remain intact in the report, outside this state comparison.
    result={key:{name:copy.deepcopy(value) for name,value in selected[key].items() if name!='session_id'}
            for key in ('observation','components','ui')}
    result['gameplay']={key:copy.deepcopy(selected['gameplay']['module'][key]) for key in ('schema','values')}
    return result


def exact_restored(owner, expected, expected_selected, label):
    actual=owner.save_probe(label)
    need(exact(actual,expected),'Full native save/document differs before any restored callback: '+label)
    selected=owner.selected()
    need(exact(selected_payload(selected),selected_payload(expected_selected)),
         'Complete selected entity/UI/component/global state differs before callback: '+label)
    owner.report.setdefault('precallback_restores',{})[label]=dict(full_exact=True,selected_exact=True,selected=selected)


def finish(owner):
    progress(owner,dict(FuseCount=0,CogCount=1,BreakerOn=1,BridgeRaised=0,PumpOn=0,Objective=2))
    owner.walk(-7,1);owner.walk(-7,-6);owner.walk(-5,-7)
    need(pos(owner.entity(PLAYER))[1]>1.5,'Actual ramp did not reach catwalk')
    owner.use(E(0x22),9);progress(owner,dict(CogCount=0,BridgeRaised=1,Objective=3))
    owner.step(95);body_y(owner,E(0x23),1.7)
    before=inventory_world(owner);owner.use(E(0x22),10);after=inventory_world(owner)
    need(exact(before['values'],after['values']) and all(exact(before['bodies'][k]['world_matrix'],after['bodies'][k]['world_matrix']) for k in BODIES),'Repeated winch consumed inventory or moved settled bodies')
    owner.walk(-3.6,-6.4);owner.walk(4.6,-6.4)
    p=pos(owner.entity(PLAYER));need(p[0]>3.5 and p[1]>1.5,'Actual controller did not cross the raised bridge')
    owner.use(E(0x24),12);progress(owner,dict(PumpOn=1,Objective=4,CogCount=0,FuseCount=0))
    before=inventory_world(owner);owner.use(E(0x24),13);after=inventory_world(owner)
    need(exact(before['values'],after['values']),'Repeated console changed progression')


def new_game_and_old_load(owner, saved, saved_selected, saves):
    progress(owner,dict(PumpOn=1,Objective=4,BridgeRaised=1))
    before_files=tree(saves);owner.activate('pause');owner.activate('newgame');owner.step(60)
    progress(owner,dict(FuseCount=0,CogCount=0,FuseAPresent=1,FuseBPresent=1,CogPresent=1,
        BreakerOn=0,BridgeRaised=0,PumpOn=0,Objective=0,ResetPending=0,Started=1,Paused=0))
    for identity,home in [(E(0x10),[-6,.95,5]),(E(0x11),[5,1.05,6.5]),(E(0x12),[7.5,1.1,0]),
                          (E(0x21),[6.95,1.2,0]),(E(0x23),[0,-.05,-6.5])]:
        need(all(abs(actual-expected)<.002 for actual,expected in zip(pos(owner.entity(identity)),home)),
             'New Game did not reset complete native body position: '+identity)
    need(tree(saves)==before_files,'New Game changed any external checkpoint/probe-store bytes')
    owner.walk(5.5,-3.5);owner.walk(-4.9,5);owner.use(E(0x10),1)
    progress(owner,dict(FuseCount=1,FuseAPresent=0,BreakerOn=0,Objective=0))
    before_load=tree(saves);owner.activate('pause');reply=owner.activate('load')
    need(reply['runtime_replaced'] and reply['save_operation']['state']==3,'Compiled old-checkpoint load did not replace runtime successfully')
    need(tree(saves)==before_load,'Loading old checkpoint after reset changed external files')
    exact_restored(owner,saved,saved_selected,'old-checkpoint-after-newgame')
    owner.report['new_game_from_complete']=dict(all_bodies_reset=True,all_storage_unchanged=True,
        reset_replay_pickup=True,original_checkpoint_loaded=True)


def warm(owner, saves, capture):
    need(not owner.call('save.inspect',dict(slot=SLOT))['exists'],'Warm verification requires fresh user storage')
    owner.step(2);need(owner.ui()['modal']==U(0xb0),'Initial Start modal absent')
    owner.activate('start');owner.step(35)
    progress(owner,dict(FuseCount=0,CogCount=0,BreakerOn=0,BridgeRaised=0,PumpOn=0,Objective=0,Started=1))
    owner.activate('pause');progress(owner,dict(Paused=1));owner.activate('resume');progress(owner,dict(Paused=0))
    owner.capture('initial',capture)
    owner.walk(4.5,3.5);owner.walk(6,0)
    owner.aim(pos(owner.entity(E(0x12))));owner.step();ray=owner.ray()
    need(ray['hit'] is not None and ray['hit']['entity']==E(0x21),'Closed shutter did not actually occlude the cog ray')
    owner.use(E(0x21),6);progress(owner,dict(CogCount=0,CogPresent=1,BreakerOn=0));body_y(owner,E(0x21),1.2)
    owner.report['closed_cog_ray']=ray
    owner.walk(4.5,3.5);owner.walk(-4.9,5);owner.use(E(0x10),1);owner.step(6)
    progress(owner,dict(FuseCount=1,FuseAPresent=0));body_y(owner,E(0x10),-3)
    owner.walk(-6.3,1);before=inventory_world(owner);owner.use(E(0x20),3);after=inventory_world(owner)
    need(exact(before['values'],after['values']) and all(exact(before['bodies'][key]['world_matrix'],after['bodies'][key]['world_matrix']) for key in BODIES),
         'Missing-prerequisite rejection consumed inventory or changed world bodies')
    owner.report['one_fuse_rejection']=dict(before=before,after=after)
    owner.walk(-4.5,3.9);owner.walk(5,5.3);owner.use(E(0x11),1);owner.step(6)
    progress(owner,dict(FuseCount=2,FuseBPresent=0));body_y(owner,E(0x11),-3)
    owner.walk(-4.5,3.9);owner.walk(-6.3,1);owner.use(E(0x20),4)
    progress(owner,dict(FuseCount=0,BreakerOn=1));owner.step(65);body_y(owner,E(0x21),3)
    before=inventory_world(owner);owner.use(E(0x20),5);after=inventory_world(owner)
    need(exact(before['values'],after['values']) and all(exact(before['bodies'][key]['world_matrix'],after['bodies'][key]['world_matrix']) for key in BODIES),
         'Repeated breaker consumed inventory or moved settled bodies')
    owner.walk(6,0);owner.use(E(0x12),7);owner.step(6)
    progress(owner,dict(CogCount=1,CogPresent=0,BreakerOn=1,BridgeRaised=0,PumpOn=0,Objective=2));body_y(owner,E(0x12),-3)
    owner.activate('pause');result=owner.activate('save');operation=result['save_operation']
    need(not result['runtime_replaced'] and operation['kind']==1 and operation['state']==3
         and operation['error_code']==0,'Compiled save did not complete successfully')
    ticket=dict(epoch=operation['epoch'],sequence=operation['sequence'])
    need(exact(owner.call('runtime.save.result',ticket),operation),'Saved step/control ticket differs from native ledger')
    saved,path,row=stored(saves,SLOT);status=owner.call('save.inspect',dict(slot=SLOT))
    need(row['generation']==1 and status['selected']['verified'] and not status['recovered'],'Partial save not healthy generation1')
    need(saved['snapshot']['payload']['gameplay']['values']['SaveStatus']==1,'Expected actual pending-save durable boundary differs')
    saved_selected=owner.selected()
    owner.report['partial_checkpoint']=dict(saved=saved,path=str(path),identity=pin(path),inspection=status,
        operation=operation,selected_at_durable_boundary=saved_selected)
    owner.step(3);progress(owner,dict(SaveStatus=2,CogCount=1,Objective=2))
    need(owner.values()['SavedTick']==str(operation['committed_tick']),'Successful-save HUD tick differs from actual completion')
    owner.capture('partial',capture)
    finish(owner);owner.capture('completed',capture)
    owner.activate('pause');load=owner.activate('load')
    need(load['runtime_replaced'] and load['save_operation']['state']==3,'Compiled Load did not complete')
    exact_restored(owner,saved,saved_selected,'same-owner-before-callback');first_restore_callback(owner)
    finish(owner);new_game_and_old_load(owner,saved,saved_selected,saves)
    return saved,saved_selected


def cold(owner, saved, saved_selected, saves):
    owner.step(2);need(owner.ui()['modal']==U(0xb0),'Cold owner initial Start modal absent')
    result=owner.activate('load')
    need(result['runtime_replaced'] and result['save_operation']['kind']==2
         and result['save_operation']['state']==3 and result['save_operation']['error_code']==0,
         'Cold Start-modal compiled Load did not complete')
    exact_restored(owner,saved,saved_selected,'cold-before-callback');first_restore_callback(owner)
    finish(owner);new_game_and_old_load(owner,saved,saved_selected,saves)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--plan',type=Path,required=True)
    args=parser.parse_args();need(os.name=='nt' and not sys.flags.optimize,'Nonoptimized Windows Python required');assigned_job()
    plan=read(args.plan);need((plan['world'] is None)!=(plan['game'] is None),'Select source world OR actual bundled game')
    need(plan['seconds']==300 and type(plan['capture']) is bool,'Bounded300-second/capture plan required')
    output,saves,descriptor=Path(plan['output']),Path(plan['saves']),Path(plan['descriptor'])
    need(not output.exists(),'Fresh output required');output.mkdir(parents=True)
    if not saves.exists():saves.mkdir(parents=True)
    need(saves.is_dir() and not tree(saves),'Fresh empty external storage required')
    for name,identity in plan['pins'].items():need(pin(Path(name))==identity,'Plan pin changed: '+name)
    for name,expected in plan['inventories'].items():need(tree(Path(name))==expected,'Initial immutable input closure differs')
    world=Path(plan['world']) if plan['world'] else Path(plan['game']).parent/read(Path(plan['game']))['entry']['world']
    original_world=pin(world);ids=sorted(read(world)['entities']);need(len(ids)==29,'Bounded authored membership differs')
    started=time.monotonic();deadline=started+300;owner=None
    report=dict(format='poima.puzzle148.independent-game.v1',passed=False,plan=pin(args.plan),harness=pin(Path(__file__)),
        owners=[],errors=[],scope='Independent real input/UI/callback and complete same-image before-callback save checks; no gameplay/source repair.')
    try:
        for label in ('warm','cold'):
            owner=Pump(plan,output/label,deadline,descriptor,ids)
            if label=='warm':saved,saved_selected=warm(owner,saves,plan['capture'])
            else:cold(owner,saved,saved_selected,saves)
            owner.close();report['owners'].append(dict(name=label,calls=len(owner.calls),validated_successes=owner.validated,
                returncode=owner.transport.returncode,host_returncode=None if owner.host is None else owner.host.returncode,evidence=owner.report));owner=None
        need(pin(world)==original_world,'Authored world changed')
        for name,identity in plan['pins'].items():need(pin(Path(name))==identity,'Original plan input changed')
        for name,expected in plan['inventories'].items():
            actual=tree(Path(name));need(all(actual.get(key)==value for key,value in expected.items()),'Original immutable bytes changed')
            extras=set(actual)-set(expected);allowed=world.relative_to(Path(name)).as_posix()+'.lock' if world.is_relative_to(Path(name)) else None
            need(extras<=({allowed} if allowed else set()) and all(actual[key]['bytes']==0 for key in extras),'Unexpected input sidecars')
        report['passed']=True
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException:report['errors'].append(traceback.format_exc())
            report['owners'].append(dict(name=owner.directory.name,calls=len(owner.calls),validated_successes=owner.validated,
                returncode=owner.transport.returncode,host_returncode=None if owner.host is None else owner.host.returncode,evidence=owner.report,interrupted=True))
        report['seconds']=time.monotonic()-started
        report['native_calls']=sum(row['calls'] for row in report['owners']);report['validated_successes']=sum(row['validated_successes'] for row in report['owners'])
        write(output/'report.json',report)
    print(json.dumps(dict(passed=report['passed'],seconds=report['seconds'],calls=report['native_calls'],errors=len(report['errors']))))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
