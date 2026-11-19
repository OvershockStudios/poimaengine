#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Move an obstacle after the last clear reach sample and inspect pickup ownership."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import time

from carry_props import CarryRun, REQUIRED, author, capture
from ik_clearance import GAME as PREVIEW_GAME, finish, load, probe
from instance_save_evolution import raw_digest, replace_member
from item_attachment_release import payload
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import ACTOR, CAMERA, HAND, ITEM, RIG, WALL
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME = 'Poima.Examples.FinalClearanceCarryPropsGame'
FEATURE = 'item_attachment_pickup_clearance_v1'
PREVIEW_FEATURES = REQUIRED | {'animation_ik_clearance_v1','capsule_cast_v1'}


def last_reach_sample(game):
    game.call('runtime.start',revision=game.revision)
    load(game)
    game.run_ticks(138)
    game.run_ticks(2,look=-9)
    game.run_ticks(17,move=.77)
    camera = game.matrix(CAMERA)
    target = game.matrix(ITEM)[12:15]
    current_pitch = math.degrees(math.atan2(-camera[9],camera[10]))
    target_pitch = math.degrees(math.atan2(target[1]-camera[13],camera[14]-target[2]))
    game.run_ticks(2,look=target_pitch-current_pitch)
    assert game.visible_item() == ITEM
    body = game.binding()['anchor']
    game.run_ticks(1,action='pickup')
    game.run_ticks(10)
    state = game.state()
    assert state['Phase'] == 1 and state['TicksInPhase'] == 11 and state['Attempts'] == 0, state
    rig = game.matrix(RIG)
    pole = [sum(rig[axis*4+k]*weight for axis,weight in enumerate((.4,.15,.02,1)))
            for k in range(3)]
    goal = dict(entity=RIG,slot=1,target=game.matrix(ITEM)[12:15],pole=pole)
    preflight = probe(game,goal,[ACTOR,body])
    assert preflight['blocker'] is None,preflight
    elbow = preflight['candidate'][1]
    assert elbow[0]>.06,elbow
    return body, elbow


def close_in_one_tick(game, elbow):
    game.run_ticks(1,motions=[dict(entity=WALL,position=elbow,rotation=[0,0,0,1],duration_ticks=1)])
    assert math.dist(game.matrix(WALL)[12:15],elbow)<1e-6


def recover(game, expected):
    assert game.outcome() == expected
    game.run_ticks(1)
    state = game.state()
    assert state['Phase'] == 8 and state['Cancellations'] == 1 and state['PendingTick'] == '0',state
    game.expect_prompt('Reach blocked')


def reject_invalid_witness(game, final):
    original = game.saves/'slot-blocked'
    invalid = game.saves/'slot-invalid-witness'
    shutil.copytree(original,invalid)
    manifest = invalid/'current.json'
    document = json.loads(manifest.read_text())
    path = invalid/document['payload']['current']['file']
    raw = path.read_text()
    raw = replace_member(raw,('snapshot','payload','attachment_outcomes','outcomes',0,'interval'),5)
    raw = replace_member(raw,('snapshot','sha256'),raw_digest(raw,('snapshot','payload')))
    changed = raw.encode('utf-8')
    path.write_bytes(changed)
    digest = hashlib.sha256(changed).hexdigest()
    updated = manifest.read_text()
    for field,value in (('sha256',digest),('bytes',len(changed))):
        updated = replace_member(updated,('payload','current',field),value)
        updated = replace_member(updated,('payload','receipts',0,field),value)
    updated = replace_member(updated,('sha256',),raw_digest(updated,('payload',)))
    manifest.write_bytes(updated.encode('utf-8'))

    ui_revision,_ = game.ui()
    component_revision = game.call('runtime.components')['component_revision']
    rejected = game.owner.rpc('save.load',dict(request_id=id_request(),configuration_generation=1,
        slot='invalid-witness',expected_generation=1,revision=game.revision,new_session_id=id_request(),
        expected_session_id=game.id,expected_tick=game.tick,expected_gameplay_revision=game.game_revision,
        expected_structure_revision=game.structure,expected_component_revision=component_revision,
        expected_ui_revision=ui_revision,expected_control_sequence=0,gameplay=game.config),error=-32070)
    assert 'snapshot integer' in rejected['message'],rejected
    assert game.save('after-invalid-load') == final
    return rejected


def comparison(args, world, revision):
    previous = copy.copy(args)
    previous.binary = args.previous_binary
    previous.descriptor = args.comparison_descriptor
    if args.comparison_assembly:
        previous.assembly = args.comparison_assembly
        previous.bridge = args.comparison_bridge
    native.ARGS = previous
    owner = native.Session(args.output/'comparison',world=world)
    saves = args.output/'comparison-saves'
    saves.mkdir()
    try:
        config = configuration(previous,PREVIEW_GAME,PREVIEW_FEATURES)
        game = CarryRun(owner,revision,saves,config,bool(previous.descriptor))
        body, elbow = last_reach_sample(game)
        close_in_one_tick(game,elbow)
        verdict = game.outcome()
        assert verdict['status'] == 'transferred' and game.structure == 3,verdict
        assert game.binding()['anchor'] == HAND
        game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=3,id=body)
        return dict(tick=game.tick,outcome=verdict,elbow=elbow)
    finally:
        owner.close()
        native.ARGS = args


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 850
    native.DEADLINE = time.monotonic()+260
    native.REPORT['owners'].clear()
    native.REPORT['calls'].clear()
    owner = native.Session(args.output/'warm')
    world = owner.world
    saves = args.output/'saves'
    saves.mkdir()
    report = dict(passed=False)
    try:
        discovery = owner.rpc('world.describe')['gameplay_item_attachments']
        assert discovery['pickup_clearance_feature'] == FEATURE and discovery['pickup_clearance_command_bytes'] == 312
        config = configuration(args,GAME,PREVIEW_FEATURES | {FEATURE})
        manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
        revision = author(owner,json.loads(manifest.read_text(encoding='utf-8-sig')),
            wall_scale=(.04,.06,.05),rig_degrees=-65)
        game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        body, elbow = last_reach_sample(game)
        close_in_one_tick(game,elbow)
        rejected = game.outcome()
        assert rejected['status']=='reach_blocked' and rejected['obstacle']==WALL,rejected
        assert rejected['anchor']==HAND and game.structure==2
        assert game.binding()['released'] and game.binding()['anchor']==body
        assert game.visible_item() == ITEM
        state = game.state()
        assert state['PendingTick']==str(game.tick) and state['Attempts']==1,state
        boundary = (game.tick,game.structure,game.game_revision)
        blocked = game.save('blocked')
        witness = payload(blocked)['attachment_outcomes']
        assert witness == dict(version=2,tick=game.tick,outcomes=[rejected]),witness
        recover(game,rejected)
        picture = capture(game,args,'reach-blocked')
        cleared = [1.5,elbow[1],elbow[2]]
        successful_tick = finish(game,cleared)
        assert game.structure == 3
        game.call('runtime.entity',error=-32004,tick=game.tick,structure_revision=3,id=body)
        final = game.save('final')
        finished = capture(game,args,'recovered')
        invalid = reject_invalid_witness(game,final)
        owner.close()

        owner = native.Session(args.output/'cold',world=world)
        cold = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('blocked',*boundary)
        assert cold.save('cold-blocked') == blocked
        recover(cold,rejected)
        assert finish(cold,cleared) == successful_tick
        assert cold.save('cold-final') == final
        owner.close()
        old_proof = None
        old_error = None
        if args.previous_binary:
            old_proof = comparison(args,world,revision)
            assert old_proof['tick']==boundary[0] and math.dist(old_proof.pop('elbow'),elbow)<1e-8
            native.ARGS = copy.copy(args)
            native.ARGS.binary = args.previous_binary
            owner = native.Session(args.output/'previous',world=world)
            previous_saves = args.output/'previous-saves'
            previous_saves.mkdir()
            old = CarryRun(owner,revision,previous_saves,config,bool(args.descriptor))
            old.call('runtime.start',revision=revision)
            old_error = load(old,error=-32060)
            assert FEATURE in old_error['message'],old_error
            owner.close()
        report.update(passed=True,calls=len(native.REPORT['calls']),exact_archive_pairs=3,
            blocked=rejected,blocked_tick=boundary[0],successful_tick=successful_tick,
            invalid_saved_witness_rejection=invalid,
            previous_game=old_proof,previous_host_rejection=old_error,
            captures=dict(blocked=picture,recovered=finished),owners=native.REPORT['owners'])
    finally:
        owner.close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--previous-binary',type=Path)
    parser.add_argument('--comparison-descriptor',type=Path,help='Published preview-only module for the old Windows host.')
    parser.add_argument('--comparison-assembly',type=Path,help='Retained CoreCLR gameplay assembly.')
    parser.add_argument('--comparison-bridge',type=Path,help='Matching bridge for the retained assembly.')
    parser.add_argument('--gpu',type=int)
    options = parser.parse_args()
    if not options.descriptor and not options.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    if options.descriptor and options.previous_binary and not options.comparison_descriptor:
        parser.error('Supply an existing preview-only Native AOT descriptor for the previous host comparison.')
    if bool(options.comparison_assembly)!=bool(options.comparison_bridge):
        parser.error('Supply both paths to qualify the retained CoreCLR assembly and bridge.')
    verify(options)
