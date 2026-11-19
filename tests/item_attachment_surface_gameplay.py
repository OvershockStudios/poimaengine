#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify sampled grip commands from real compiled Tick callbacks."""
import argparse
import copy
import json
from pathlib import Path
import time

from item_attachment_gameplay import Run, sha
from item_attachment_surface import configure, ITEM, ANCHOR
from runtime_surface_measure import SKIN, TARGET, probe
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME = 'Poima.Tests.AttachmentSurfaceGame'
FEATURES = {'baseline_v7', 'item_attachment_v1', 'item_attachment_contact_v1', 'item_attachment_surface_v1'}


def parameters(parser):
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--hostfxr',type=Path)
    parser.add_argument('--bridge',type=Path)
    parser.add_argument('--assembly',type=Path)
    parser.add_argument('--descriptor',type=Path)


def configuration(args, game_type=GAME, features=FEATURES):
    if args.descriptor:
        declaration=json.loads(args.descriptor.read_text(encoding='utf-8'))
        assert set(declaration['required_features'])==features and declaration['minimum_services_bytes']==528
        return dict(descriptor=native.native(args.descriptor),expected_descriptor_sha256=sha(args.descriptor))
    assert args.hostfxr and args.bridge and args.assembly
    return dict(hostfxr=native.native(args.hostfxr),bridge=native.native(args.bridge),
                assembly=native.native(args.assembly),type=game_type)


def load(run, values, *, error=None):
    run.call('runtime.start',revision=run.revision)
    method='runtime.gameplay.load_native' if run.native_aot else 'runtime.gameplay.load'
    result=run.call(method,error,request_id=id_request(),expected_tick=0,expected_structure_revision=0,
                    expected_revision=0,values=values,**run.config)
    if error is None:
        run.game_revision=result['revision']
    return result


def expected_late(owner, revision):
    session=id_request()
    owner.rpc('runtime.start',dict(session_id=session,revision=revision))
    owner.rpc('runtime.step',dict(session_id=session,request_id=id_request(),expected_tick=0,ticks=30))
    pair=probe(triangle=3,weights=(.3,.6,.1),distance_limit_m=.125)
    pair['target']['triangles']=[0]
    row=owner.rpc('runtime.surface.measure',dict(session_id=session,tick=30,structure_revision=0,pairs=[pair]))['pairs'][0]
    assert row['distance_m']>.3
    owner.rpc('runtime.stop',dict(session_id=session))
    return row


def check_previous(args, output, settings, world, revision, values, feature='item_attachment_surface_v1'):
    old=copy.copy(args)
    old.binary=args.previous_binary.resolve()
    native.ARGS=old
    previous=None
    try:
        previous=native.Session(output/'previous-host',world=world)
        saves=output/'previous-saves';saves.mkdir()
        game=Run(previous,revision,saves,settings,bool(args.descriptor))
        rejected=load(game,values,error=-32060)
        assert feature in rejected['message'], rejected
        assert game.bindings()==[]
        previous.close()
        return dict(sha256=sha(old.binary),rejection=rejected)
    finally:
        if previous is not None:
            previous.close()
        native.ARGS=args


def run(args):
    output=args.output.resolve()
    output.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+230
    settings=configuration(args)
    saves=output/'saves';saves.mkdir()
    has_aot=bool(args.descriptor)
    report=dict(passed=False,engine_sha256=sha(args.binary),mode='native_aot' if has_aot else 'coreclr')
    owner=native.Session(output/'runtime')
    try:
        sdk=owner.rpc('world.describe')['gameplay_item_attachments']
        assert sdk['surface_feature']=='item_attachment_surface_v1' and sdk['services_bytes']==528
        assert sdk['surface_command_bytes']==272 and sdk['surface_probe_bytes']==88
        revision=configure(owner)
        world=owner.world
        authored=world.read_bytes()

        pose=expected_late(owner,revision)
        game=Run(owner,revision,saves,settings,has_aot)
        values=dict(Item=ITEM,Anchor=ANCHOR,Source=SKIN,Target=TARGET,Count=1,Probe0=0,
                    TargetTriangle=0,RotationW=1,DistanceLimit=.125,AtTick=1,RepeatAtTick=30,SaveFirst=1)
        load(game,values)
        baseline=game.save('before')
        rejected=game.step(30,error=-32040)
        assert rejected['data']==dict(entity=ITEM,anchor=ANCHOR,probe_index=0,distance_limit_m=.125,surface=pose)
        assert game.bindings()==[] and game.game()['values']['Ticks']==0
        assert game.save('after-miss')==baseline and not (saves/'slot-surface-auto').exists()

        game.edit(RepeatAtTick=0)
        game.step(changes=1)
        binding=game.bindings()
        assert len(binding)==1 and binding[0]['entity']==ITEM and binding[0]['anchor']==ANCHOR
        assert game.game()['values']['Attempts']==1
        committed=game.save('committed')
        assert committed==game.bytes('surface-auto')
        game.step(29)
        expected=game.save('continued')
        owner.close()

        owner=native.Session(output/'cold',world=world)
        cold=Run(owner,revision,saves,settings,has_aot)
        cold.restore('committed',1,1,game.game_revision)
        assert cold.bindings()==binding and cold.save('restored')==committed
        cold.step(29)
        assert cold.game()['values']['Ticks']==30 and cold.save('cold-continued')==expected
        assert world.read_bytes()==authored
        owner.close()
        if args.previous_binary:
            report['previous_host']=check_previous(args,output,settings,world,revision,values)
        report.update(passed=True,late_miss=pose,complete_checkpoint_checks=4)
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        owner.close()
        report.update(rpc_count=len(native.REPORT['calls']),owners=native.REPORT['owners'])
        (output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Sampled compiled grip verified in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--previous-binary',type=Path,help='An older frame-only host to check feature negotiation.')
    run(parser.parse_args())
