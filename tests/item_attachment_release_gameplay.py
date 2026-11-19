#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise an input-driven prop release with both compiled gameplay backends."""
import argparse
import json
from pathlib import Path
import shutil
import time

from item_attachment_gameplay import Run, sha
from item_attachment_recovery_gameplay import hold, outcomes
from item_attachment_release import COLLIDER, FLOOR, HOLDER, METHOD, OBSERVER, add_release_world, body_state, capture, payload, release_params
from item_attachment_surface_gameplay import check_previous, configuration, load, parameters
from runtime_attachment import CAMERA, ITEM, close_matrix
from runtime_pose_frames import id_request, transform, uid
import world_physical_contract as native

GAME = 'Poima.Tests.AttachmentReleaseGame'
FEATURE = 'item_attachment_release_v1'
FEATURES = {'baseline_v7', 'item_attachment_v1', 'input_actions_v1', FEATURE}
ACTOR = uid(92)
DROP = [dict(entity=ACTOR, actions=[dict(name='drop', held=False, pressed=True, released=True)])]
INITIAL = dict(Item=ITEM, TemplateLow=str(int(COLLIDER,16)), Actor=ACTOR, BlockedAt=12, SaveBlocked=1, SaveRelease=1,
               OffsetX=.12, BlockedOffsetY=-1.35, LinearX=2.5, LinearY=.75, AngularZ=3)


def release_input(game, ticks=1, error=None):
    result=game.call('runtime.step',error=error,request_id=id_request(),expected_tick=game.tick,
                     expected_structure_revision=game.structure,ticks=ticks,inputs=DROP)
    if error is None:
        game.tick+=ticks
        game.structure+=1
        status=game.owner.rpc('runtime.status')
        assert result['tick']==game.tick and status['structure_revision']==game.structure,(result,status)
    return result


def assert_seen(game, status, tick, anchor, depth=0., obstacle='0'*32):
    values=game.game()['values']
    assert values['LastStatus']==status and values['LastOutcomeTick']==str(tick),values
    assert values['LastAnchor']==anchor and values['LastObstacle']==obstacle and values['LastPenetration']==depth,values


def exercise_rejected_batch(game, prefix, blocked):
    game.edit(DuplicateAt=13)
    duplicate=game.save(prefix+'-before-duplicate')
    reply=release_input(game,error=-32040)
    assert 'Duplicate gameplay item attachment' in reply['message'],reply
    assert game.save(prefix+'-after-duplicate')==duplicate
    game.edit(DuplicateAt=0,InvalidAt=14)
    before=game.save(prefix+'-before-batch')
    reply=release_input(game,2,-32040)
    assert 'currently attached' in reply['message'],reply
    assert game.save(prefix+'-after-batch')==before and outcomes(game)==[blocked]
    assert not (game.saves/'slot-engine-released').exists()
    game.edit(InvalidAt=0)


def from_blocked(game,args,out,prefix,blocked,expected_pose):
    assert game.tick==12 and game.structure==1 and outcomes(game)==[blocked]
    assert game.bindings()[0]['anchor']==HOLDER and game.game()['values']['Ticks']==12
    exercise_rejected_batch(game,prefix,blocked)
    before=capture(game,args,out,prefix+'-held',CAMERA)
    expected_id=payload(game.bytes('engine-blocked'))['structure']['next_entity_id']
    release_input(game)
    receipt=dict(entity=ITEM,anchor=expected_id,status='released')
    assert outcomes(game)==[receipt] and game.bindings()[0]['released'] is True
    assert_seen(game,2,12,HOLDER,blocked['penetration_m'],FLOOR)
    close_matrix(game.matrix(ITEM),expected_pose)
    after=capture(game,args,out,prefix+'-released',OBSERVER)
    if args.gpu is not None:
        assert before['foreground']==3 and after['objects']==4,(before,after)
    released=game.save(prefix+'-released')
    assert released==game.bytes('engine-released')
    state=body_state(released,expected_id)
    assert state['active'] and state['linear_velocity']==[2.5,.75,0] and state['angular_velocity']==[0,0,3]
    return released,expected_id,after


def from_launch(game,body,prefix):
    assert outcomes(game)==[dict(entity=ITEM,anchor=body,status='released')]
    game.step()
    assert_seen(game,1,13,body)
    assert not outcomes(game)
    game.step(131)
    state=game.save(prefix+'-landed')
    pose=body_state(state,body)
    assert not pose['active'] and -1.02<pose['position'][1]<-.78,pose
    return state


def verify(args):
    out=args.output.resolve()
    out.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+230
    config=configuration(args,GAME,FEATURES)
    warm_saves=out/'warm-saves';warm_saves.mkdir()
    report=dict(passed=False,engine_sha256=sha(args.binary),mode='native_aot' if args.descriptor else 'coreclr')
    owner=native.Session(out/'authored')
    try:
        inspect=owner.rpc('world.describe')['gameplay_item_attachments']
        assert (inspect['release_feature'],inspect['release_command_bytes'],inspect['release_outcome_bytes'])==(FEATURE,152,80)
        revision=add_release_world(owner)
        actor=[dict(op='entity.create',id=ACTOR,name='Drop input actor'),
            native.component(ACTOR,'Transform',transform((15,0,0))),
            native.component(ACTOR,'CharacterController',dict(radius=.25,height=1.7,speed=4,jump_speed=5,camera=None))]
        revision=native.transact(owner,actor,revision)['revision']
        world=owner.world
        authored=world.read_bytes()
        owner.close()

        owner=native.Session(out/'oracle',world=world)
        (out/'oracle-saves').mkdir()
        reference=Run(owner,revision,out/'oracle-saves',{})
        reference.call('runtime.start',revision=revision)
        hold(reference,ITEM,HOLDER)
        reference.step(12)
        pose_before_input=reference.matrix(ITEM)
        expected_block=owner.rpc(METHOD,release_params(reference,offset=dict(position=[.12,-1.35,0],rotation=[0,0,0,1])),error=-32602)
        assert expected_block['data']['obstacle']==FLOOR and expected_block['data']['penetration_m']>.001
        reference.step()
        expected_pose=reference.matrix(ITEM)
        assert max(abs(a-b) for a,b in zip(pose_before_input,expected_pose))>1e-4
        owner.close()

        owner=native.Session(out/'warm',world=world)
        warm=Run(owner,revision,warm_saves,config,bool(args.descriptor))
        load(warm,INITIAL)
        hold(warm,ITEM,HOLDER)
        warm.step(12)
        blocked=dict(entity=ITEM,anchor=HOLDER,status='release_blocked',**expected_block['data'])
        assert outcomes(warm)==[blocked] and warm.save('blocked-inspection')==warm.bytes('engine-blocked')
        saved_blocked=warm.bytes('engine-blocked')
        assert payload(saved_blocked)['attachment_outcomes']==dict(version=1,tick=12,outcomes=[blocked])
        assert 'physical' not in payload(saved_blocked)
        warm_launch,body,world_frame=from_blocked(warm,args,out,'warm',blocked,expected_pose)
        warm_end=from_launch(warm,body,'warm')
        owner.close()

        cold_saves=out/'cold-saves';cold_saves.mkdir()
        shutil.copytree(warm_saves/'slot-engine-blocked',cold_saves/'slot-engine-blocked')
        owner=native.Session(out/'cold-blocked',world=world)
        cold=Run(owner,revision,cold_saves,config,bool(args.descriptor))
        cold.restore('engine-blocked',12,1,1)
        assert cold.save('restored-blocked')==saved_blocked
        assert from_blocked(cold,args,out,'cold',blocked,expected_pose)==(warm_launch,body,world_frame)
        assert from_launch(cold,body,'cold')==warm_end
        owner.close()

        owner=native.Session(out/'cold-released',world=world)
        cold=Run(owner,revision,cold_saves,config,bool(args.descriptor))
        cold.restore('engine-released',13,2,4)
        assert cold.save('restored-released')==warm_launch
        assert from_launch(cold,body,'restored')==warm_end
        owner.close()
        assert world.read_bytes()==authored
        if args.previous_binary:
            report['previous_host']=check_previous(args,out,config,world,revision,INITIAL,FEATURE)
        report.update(passed=True,blocked=blocked,settled=body_state(warm_end,body),
                      windows_capture=world_frame,complete_checkpoint_comparisons=12)
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        owner.close()
        report.update(rpc_count=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (out/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Compiled prop release verified in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--gpu',type=int)
    verify(parser.parse_args())
