#!/usr/bin/env python3
"""Independent native-input patrol encounter, LOS, outcome and durable decision replay."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import traceback
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('patrol_sample',ROOT/'examples/managed/PatrolGame/run.py')
sample=importlib.util.module_from_spec(spec);spec.loader.exec_module(sample)
Encounter,uid,sha=sample.Encounter,sample.uid,sample.sha


def normalized(value):
    if isinstance(value,dict):return {k:normalized(v) for k,v in value.items() if k!='session_id'}
    if isinstance(value,list):return [normalized(v) for v in value]
    return value


def position(entity):return entity['world_matrix'][12:15]


def go(game,x,z,max_steps=160,allow_terminal=False):
    """Route from observed native poses; no teleports or gameplay field edits."""
    for _ in range(max_steps):
        player=game.entity(100);p=position(player);dx,dz=x-p[0],z-p[2]
        distance=math.hypot(dx,dz)
        if distance<.23:return
        desired=math.degrees(math.atan2(-dx,-dz))
        delta=(desired-player['yaw']+180)%360-180
        # Keep the turn in a one-tick request, then continue with translation only.
        game.step(1,move=[0,min(1,distance/.3)],look=[delta,0])
        game.step(min(5,max(1,int(distance/.09))),move=[0,min(1,distance/.3)])
        if not allow_terminal:assert game.values()['Outcome']==0,game.values()
    raise AssertionError('Native movement did not reach route waypoint '+str((x,z,position(game.entity(100)))))


def expose(game):
    # Peek only until the first real visual acquisition. Continuing all the way
    # to the guard would consume the player's escape lead and test another route.
    go(game,7,8)
    player=game.entity(100)
    game.step(1,look=[(-player['yaw']+180)%360-180,0])
    for _ in range(160):
        if game.values()['Alerts']>0:return
        game.step(1,move=[0,1])
    raise AssertionError('Approaching the open side did not alert the guard')


def snapshot(game):
    return normalized(dict(tick=game.tick,values=game.values(),guard=game.entity(300),
        player=game.entity(100),camera=game.entity(101),ui=game.rpc('runtime.ui.inspect',dict(session_id=game.session,tick=game.tick))))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','hostfxr','bridge','assembly','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--timeout',type=float,default=180)
    parser.add_argument('--capture',action='store_true')
    parser.add_argument('--gpu',type=int,default=-1)
    args=parser.parse_args()
    if sys.flags.optimize:parser.error('Assertions must be enabled')
    if not math.isfinite(args.timeout) or args.timeout<=0:parser.error('Positive finite timeout required')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    config=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type='Poima.Examples.PatrolGame')
    evidence=dict(passed=False,checks=[],owners=[],captures=[],hashes={name:sha(getattr(args,name)) for name in ('binary','hostfxr','bridge','assembly')},
        source_sha256=sha(ROOT/'examples/managed/PatrolGame/PatrolGame.cs'),world_sha256=sha(ROOT/'examples/patrol-room.jsonl'),runner_sha256=sha(__file__),
        limits=['Small authored four-waypoint room; straight steering, not navmesh or general obstacle avoidance.',
            'Scripted native input, not physical devices or interactive GUI qualification.',
            'Optional capture is a renderer readback, not an OS screenshot; no performance or shipping-platform claim.'],cleanup_errors=[])
    owned=[];game=None;deadline=time.monotonic()+args.timeout
    def owner(world):
        instance=Encounter(args.binary,world,config,native,args.timeout);instance.deadline=deadline;owned.append(instance);return instance
    def close(instance):
        if instance.closed:return
        try:instance.close()
        except BaseException:evidence['cleanup_errors'].append(traceback.format_exc());raise
    def capture(name):
        if not args.capture:return
        before=snapshot(game);state=game.inspect();path=output/(name+'.bmp')
        report=game.rpc('runtime.capture',dict(session_id=game.session,tick=game.tick,ui_revision=state['ui_revision'],
            camera=uid(101),path=native(path),width=1280,height=720,gpu=args.gpu,samples=1))
        assert report['capture_written'] and report['nvrhi_errors']==0,report
        assert snapshot(game)==before,'Capture changed authoritative gameplay'
        evidence['captures'].append(dict(name=name,sha256=sha(path),report=report,observation=before))
    try:
        world=output/'world.json';game=owner(world);game.author();game.start();authored=world.read_bytes()
        # Availability is independently exercised by normal player Inputs below;
        # camera-free guard remains a runtime CharacterController, never caller Inputs.
        assert game.entity(300)['is_character'] and game.entity(100)['is_character']
        initial=position(game.entity(300));game.step(480)
        values=game.values();assert values['Patrolled']>=1 and values['Alerts']==values['Seen']==values['Outcome']==0,values
        assert math.dist(initial,position(game.entity(300)))>1
        p,g=position(game.entity(100)),position(game.entity(300))
        delta=[p[0]-g[0],p[1]-g[1],p[2]-g[2]];length=math.sqrt(sum(v*v for v in delta))
        ray=game.rpc('runtime.raycast',dict(session_id=game.session,tick=game.tick,origin=[g[0],g[1]+.65,g[2]],
            direction=[v/length for v in delta],distance=length+.05,ignore=[uid(300)]))
        assert ray['hit'] and ray['hit']['entity']==uid(2),ray
        capture('patrol')
        evidence['checks'].append('Camera-free guard physically patrols; a native ray hits authored cover and compiled vision does not acquire the stationary player.')

        # Noise is an explicit player action, not a fabricated guard control. It
        # gives an independently reachable nonterminal decision checkpoint.
        game.step(use=True);values=game.values()
        assert values['NoiseCount']==1 and values['Mode']==1 and values['Seen']==0,values
        game.step(20)
        # Test occlusion when range and FOV would otherwise admit this player,
        # rather than inferring it from an out-of-range or rear-facing guard.
        g,p=position(game.entity(300)),position(game.entity(100))
        guard_matrix=game.entity(300)['world_matrix'];forward=[-guard_matrix[8],-guard_matrix[10]]
        dx,dz=p[0]-g[0],p[2]-g[2];distance=math.hypot(dx,dz)
        facing=(forward[0]*dx+forward[1]*dz)/(math.hypot(*forward)*distance)
        assert distance<=13 and facing>=math.cos(math.radians(75)),(distance,facing)
        ray=game.rpc('runtime.raycast',dict(session_id=game.session,tick=game.tick,
            origin=[g[0],g[1]+.65,g[2]],direction=[dx/distance,0,dz/distance],distance=distance+.05,ignore=[uid(300)]))
        assert ray['hit'] and ray['hit']['entity']==uid(2) and game.values()['Seen']==game.values()['Alerts']==0,ray
        paused_tick=game.tick
        assert game.control(10)['intent']==2 and game.values()['Paused']==1 and game.tick==paused_tick
        assert game.control(11)['intent']==1 and game.values()['Paused']==0 and game.tick==paused_tick
        before=snapshot(game);reloaded=game.load()
        assert game.values()==before['values'] and game.entity(300)==dict(before['guard'],session_id=game.session)
        assert reloaded['module']['migration']['added']==reloaded['module']['migration']['removed']==[]
        saves=output/'saves';saves.mkdir()
        game.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))
        saved=snapshot(game);save_guards=game.inspect()
        written=game.rpc('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='patrol-room',expected_generation=0,
            session_id=game.session,expected_tick=game.tick,expected_gameplay_revision=game.game()['revision'],
            expected_control_sequence=save_guards['control_sequence'],expected_ui_revision=save_guards['ui_revision']))
        payloads=list((saves/'slot-patrol-room').glob('p-*.bin'));assert len(payloads)==1 and sha(payloads[0])==written['sha256']
        game.step(30);continuation=snapshot(game);close(game)
        game=owner(world)
        game.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))
        fresh=uuid.uuid4().hex
        restored=game.rpc('save.load',dict(request_id=uuid.uuid4().hex,configuration_generation=1,slot='patrol-room',expected_generation=1,
            revision=1,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,new_session_id=fresh,gameplay=config))
        game.session,game.tick=fresh,restored['tick'];assert snapshot(game)==saved
        game.step(30);assert snapshot(game)==continuation
        assert world.read_bytes()==authored
        evidence['checks'].append('Player noise enters investigation; compatible compiled reload preserves decisions; a fresh native owner restores exact remembered target/timers/poses/UI and reproduces 30 subsequent ticks.')

        # Start a new match through normal lifecycle (not pose or state edits).
        game.start();expose(game)
        assert game.values()['Alerts']>0 and game.values()['Mode']==2,game.values()
        capture('chase')
        peek=position(game.entity(100));assert peek[2]>1.6,peek
        evidence['chase_positions']=dict(player=peek,guard=position(game.entity(300)))
        go(game,0,peek[2])
        assert game.values()['Outcome']==0
        saw_investigation=False
        for _ in range(100):
            game.step()
            values=game.values();assert values['Outcome']==0,values
            if values['Mode']==1 and values['Seen']==0:saw_investigation=True;break
        assert saw_investigation,'Guard never lost actual sight behind cover'
        go(game,0,9);go(game,-10,9,allow_terminal=True)
        assert game.values()['Outcome']==1 and game.values()['Alerts']>0,game.values()
        terminal=game.values();guard=position(game.entity(300));game.step(20)
        assert game.values()==terminal and math.dist(guard,position(game.entity(300)))<.001
        capture('evaded')
        evidence['checks'].append('Actual player movement exposes the player, causes visual chase, breaks sight behind solid cover, enters last-position investigation and reaches earned extraction; terminal guard intent is neutral.')

        game.start();expose(game)
        assert game.values()['Alerts']>0
        for _ in range(300):
            values=game.values()
            if values['Outcome']==2:break
            # Chase has a clear, open right-side path to a stationary player.
            game.step()
        assert game.values()['Outcome']==2,game.values()
        terminal=game.values();game.step(20);assert game.values()==terminal
        assert world.read_bytes()==authored
        capture('caught')
        evidence['checks'].append('A stationary exposed player is physically reached and caught; outcome remains terminal without resetting or teleporting either capsule.')
        evidence['passed']=True
    except BaseException:
        evidence['error']=traceback.format_exc()
        if game and game.session:
            try:evidence['failure_observation']=dict(values=game.values(),player=position(game.entity(100)),guard=position(game.entity(300)))
            except BaseException:pass
    finally:
        for instance in reversed(owned):
            try:close(instance)
            except BaseException:pass
        evidence['owners']=[dict(exit_code=i.client.transport.returncode,calls=i.calls) for i in owned]
        evidence['rpc_count']=sum(i.calls for i in owned)
        evidence['passed']=evidence['passed'] and not evidence['cleanup_errors']
        (output/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps(dict(passed=evidence['passed'],checks=len(evidence['checks']),rpc_count=evidence['rpc_count'],evidence=str(output/'evidence.json'))))
    if not evidence['passed']:print(evidence.get('error') or evidence['cleanup_errors'],file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
