#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify recoverable grips on the committed tick and in a cold process."""
import argparse
import json
from pathlib import Path
import time

from item_attachment_gameplay import Run, sha
from item_attachment_surface import configure, ITEM, ANCHOR
from item_attachment_surface_gameplay import FEATURES, check_previous, configuration, expected_late, load, parameters
from runtime_surface_measure import REVERSED, SKIN, TARGET
from runtime_pose_frames import id_request, uid
import world_physical_contract as native

GAME='Poima.Tests.AttachmentRecoveryGame'
FEATURE='item_attachment_recovery_v1'


def outcomes(game):
    return game.call('runtime.attachment.outcomes',tick=game.tick,structure_revision=game.structure)['outcomes']


def hold(game, item, holder):
    reply=game.call('runtime.attachment.apply',request_id=id_request(), expected_tick=game.tick,
        expected_structure_revision=game.structure,commands=[dict(entity=item,mode='keep_world',anchor=holder)])
    game.structure=reply['structure_revision']


def resume(game, baseline, missed, holder, *, records):
    assert outcomes(game)==[missed] and game.bindings()[0]['anchor']==holder
    game.edit(InvalidAt=31)
    invalid_checkpoint=game.save(records+'-invalid')
    invalid=game.step(error=-32040)
    assert 'Vertex index' in invalid['message'], invalid
    assert game.save(records+'-rejected')==invalid_checkpoint and outcomes(game)==[missed]
    game.edit(InvalidAt=0)
    game.step()
    frame=outcomes(game)
    assert frame==[dict(entity=ITEM,anchor=ANCHOR,status='frame_miss',position_error_m=2.,
                        position_limit_m=0.,rotation_error_deg=0.,rotation_limit_deg=0.)],frame
    first=game.game()['values']
    assert first['Ticks']==31 and first['LastStatus']==3 and first['LastOutcomeTick']=='30'
    assert first['LastDistance']==missed['distance_m'] and game.bindings()[0]['anchor']==holder
    game.step(changes=1)
    assert outcomes(game)==[dict(entity=ITEM,anchor=ANCHOR,status='transferred')]
    second=game.game()['values']
    assert second['LastStatus']==2 and second['LastOutcomeTick']=='31'
    assert game.bindings()[0]['anchor']==ANCHOR
    game.step()
    assert not outcomes(game)
    game.step()
    final=game.game()['values']
    assert final['LastStatus']==0 and final['Attempts']==3 and final['Ticks']==34
    continuation=game.save(records+'-continued')
    assert 'attachment_outcomes' not in json.loads(continuation)['snapshot']['payload']
    assert continuation!=baseline
    return continuation


def run(args):
    out=args.output.resolve()
    out.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+230
    settings=configuration(args,GAME,FEATURES|{FEATURE})
    saves=out/'saves';saves.mkdir()
    report=dict(passed=False,engine_sha256=sha(args.binary),mode='native_aot' if args.descriptor else 'coreclr')
    owner=native.Session(out/'runtime')
    try:
        discovery=owner.rpc('world.describe')
        sdk=discovery['gameplay_item_attachments']
        assert sdk['recovery_feature']==FEATURE and sdk['outcome_bytes']==120
        assert 'runtime.attachment.outcomes' in discovery['methods']
        revision=configure(owner)
        world=owner.world
        authored=world.read_bytes()
        oracle=expected_late(owner,revision)
        game=Run(owner,revision,saves,settings,bool(args.descriptor))
        values=dict(Item=ITEM,Anchor=ANCHOR,Source=SKIN,Target=TARGET,Probe0=0,Count=1,TargetTriangle=0,
                    RotationW=1,DistanceLimit=.125,RetryDistance=1,AttemptAt=30,FrameAt=31,RetryAt=32,SaveFirst=1,SampleTriangle=1)
        load(game,values)
        hold(game,ITEM,REVERSED)
        assert not outcomes(game)
        game.step(30)
        assert game.structure==1 and game.game()['values']['Ticks']==30
        missed=dict(entity=ITEM,anchor=ANCHOR,status='surface_miss',probe_index=0,
            distance_m=oracle['distance_m'],distance_limit_m=.125,target_triangle=oracle['target']['triangle'])
        assert outcomes(game)==[missed] and game.bindings()[0]['anchor']==REVERSED
        saved=game.save('missed')
        assert saved==game.bytes('recovery-auto')
        assert json.loads(saved)['snapshot']['payload']['attachment_outcomes']==dict(version=1,tick=30,outcomes=[missed])
        game.call('runtime.attachment.outcomes',error=-32009,tick=29,structure_revision=1)
        warm=resume(game,saved,missed,REVERSED,records='warm')
        owner.close()

        owner=native.Session(out/'cold',world=world)
        cold=Run(owner,revision,saves,settings,bool(args.descriptor))
        # The miss was saved before the two adversarial staged-command edits.
        cold.restore('missed',30,1,1)
        assert cold.save('reopened')==saved
        assert resume(cold,saved,missed,REVERSED,records='cold')==warm
        cold.call('runtime.stop')
        owner.close()

        # A successful strict command and a recoverable miss can share a tick.
        owner=native.Session(out/'mixed',world=world)
        mixed=Run(owner,revision,saves,settings,bool(args.descriptor))
        other=uid(63)
        load(mixed,dict(values,ConcurrentItem=other,SaveFirst=0))
        hold(mixed,ITEM,REVERSED)
        mixed.step(30,changes=1)
        active={row['entity']:row['anchor'] for row in mixed.bindings()}
        assert active=={ITEM:REVERSED,other:None} and outcomes(mixed)==[missed]
        mixed.call('runtime.stop')
        assert world.read_bytes()==authored
        owner.close()
        if args.previous_binary:
            report['previous_host']=check_previous(args,out,settings,world,revision,values,FEATURE)
        report.update(passed=True,miss=missed,invalid_steps_rolled_back=2,complete_checkpoint_checks=5)
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        owner.close()
        report.update(owners=native.REPORT['owners'],rpc_count=len(native.REPORT['calls']))
        (out/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Recoverable grip verified in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--previous-binary',type=Path)
    run(parser.parse_args())
