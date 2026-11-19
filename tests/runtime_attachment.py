#!/usr/bin/env python3
"""Verify same-tick transfers between moving anchors and exact cold continuation."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import time

from animation_fixture import ribbon
from character_library_intake import multiply
from gltf_fixture import glb
from runtime_pose_frames import id_request, qz, uid
from runtime_surface_measure import transform
import world_physical_contract as native

APPLY = 'runtime.attachment.apply'
INSPECT = 'runtime.attachment.inspect'
CAMERA, ITEM, DETAIL, OTHER, BODY, SPAWN = (uid(i) for i in range(31, 37))
PLATFORM=uid(37)
RIG_A, RIG_B = uid(71), uid(72)


def node(rig, index):
    return hashlib.sha256(f'poima.instance.v1/{rig}/node/{index}'.encode()).hexdigest()[:32]


def configure(session):
    doc, blob = ribbon()
    path = session.directory / 'anchors.glb'
    path.write_bytes(glb(doc, blob))
    asset = session.rpc('asset.import', dict(source=native.native(path)))['asset']
    ops = []

    def entity(identity, label, pose, parent=None, **components):
        op = dict(op='entity.create', id=identity, name=label)
        if parent is not None:
            op['parent'] = parent
        ops.extend([op, native.component(identity, 'Transform', pose)])
        ops.extend(native.component(identity, kind, config) for kind, config in components.items())

    lens = dict(vertical_fov=58, near=.01, far=40)
    entity(CAMERA, 'Handoff camera', transform((0, 0, 5)), Camera=dict(lens, viewmodel=lens))
    for rig, x, yaw, scale, speed in ((RIG_A, -1.3, 0, [1, 1, 1], 1),
                                      (RIG_B, 1.3, 55, [1.25, .75, 1], 2)):
        ops.extend([
            dict(op='asset.instantiate', id=rig, name='Moving socket', asset=asset, parent=CAMERA),
            native.component(rig, 'Transform', dict(transform((x, -1.4, -5), rotation=qz(yaw)), scale=scale)),
            native.component(rig, 'AnimationRig', dict(asset=asset, clip=0, time=0, speed=speed,
                                                       playing=True, loop=False)),
        ])
    foreground = dict(camera=CAMERA, mode='viewmodel')
    entity(ITEM, 'Held item', transform((-1, -.4, -5)), parent=CAMERA,
           MeshRenderer=dict(primitive='box', visible=True, albedo=[.8, .07, .015]), CameraDraw=foreground)
    entity(DETAIL, 'Item cap', dict(transform((0, .55, 0)), scale=[.6, .2, .6]), parent=ITEM,
           MeshRenderer=dict(primitive='box', visible=True, albedo=[1, .45, .05]), CameraDraw=foreground)
    entity(OTHER, 'Second item', dict(transform((1.8, -1, -5)), scale=[.3, .3, .3]), parent=CAMERA,
           MeshRenderer=dict(primitive='box', visible=True, albedo=[.5, .2, .8]), CameraDraw=foreground)
    collider=dict(motion='static', half_extents=[.5, .5, .5], mass=1, friction=.5, restitution=0)
    entity(BODY, 'Idle kinematic body', transform((20, 0, 0)), BoxCollider=dict(collider,motion='kinematic'))
    ops.extend([
        dict(op='template.set', id=SPAWN, name='Removable anchor',
             components=dict(Transform=transform((4, 0, 0)))),
        dict(op='template.set', id=PLATFORM, name='Moving anchor',
             components=dict(Transform=transform((22, 0, 0)), BoxCollider=dict(collider,motion='kinematic'))),
    ])
    return native.transact(session, ops, 0)['revision']


def close_matrix(actual, expected):
    assert len(actual) == len(expected) == 16
    assert max(abs(a-b) for a,b in zip(actual,expected)) < 2e-8, (actual, expected)


def compare_pose(session, args, revision, output):
    owner, tick, structure = id_request(), 0, 0
    session.rpc('runtime.start', dict(session_id=owner, revision=revision))
    saves = native.save_root(session)
    first_anchor, second_anchor = node(RIG_A, 1), node(RIG_B, 1)
    authored = session.world.read_bytes()
    captures = {}

    def inspect(error=None, **guards):
        return session.rpc(INSPECT, dict(session_id=owner, **({'tick':tick, 'structure_revision':structure} | guards)), error=error)

    def observed(identity):
        return session.rpc('runtime.entity', dict(session_id=owner, tick=tick, structure_revision=structure, id=identity))

    def world(identity):
        return observed(identity)['world_matrix']

    def apply(*commands, error=None, **guards):
        nonlocal structure
        parameters = dict(session_id=owner, request_id=id_request(), expected_tick=tick,
                          expected_structure_revision=structure, commands=commands)
        parameters.update(guards)
        receipt = session.rpc(APPLY, parameters, error)
        if error is None:
            assert receipt['tick'] == tick and receipt['replayed'] is False and receipt['structure_revision'] == structure+1
            structure += 1
        return parameters, receipt

    def step(count, **commands):
        nonlocal tick
        result = session.rpc('runtime.step', dict(session_id=owner, request_id=id_request(),
            expected_tick=tick, expected_structure_revision=structure, ticks=count, **commands))
        assert result['tick']==tick+count
        tick+=count

    def save(slot):
        result = session.rpc('save.write', dict(request_id=id_request(), configuration_generation=1,
            slot=slot, expected_generation=0, session_id=owner, expected_tick=tick, expected_gameplay_revision=0,
            expected_structure_revision=structure))
        files = list((saves / f'slot-{slot}').glob('p-*.bin'))
        assert len(files)==1
        data=files[0].read_bytes()
        assert len(data)==result['bytes'] and hashlib.sha256(data).hexdigest()==result['sha256']
        return data

    def capture(name):
        if args.gpu is None:
            return None
        path=output/f'{name}.bmp'
        result=session.rpc('runtime.capture',dict(session_id=owner,tick=tick,structure_revision=structure,
            camera=CAMERA,path=native.native(path),width=720,height=405,samples=4,gpu=args.gpu))
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0
        captures[name]=hashlib.sha256(path.read_bytes()).hexdigest()
        return captures[name]

    def structure_change(**operations):
        nonlocal structure
        reply=session.rpc('runtime.structure.transact',dict(request_id=id_request(),session_id=owner,
            expected_tick=tick,expected_structure_revision=structure,**operations))
        structure=reply['structure_revision']
        return reply

    def keep(item, anchor):
        return dict(entity=item, mode='keep_world', anchor=anchor)

    def reset(item):
        return dict(entity=item, mode='reset')

    assert inspect()['bindings']==[]
    initial_item=observed(ITEM)
    initial_detail=world(DETAIL)
    untouched=save('untouched')
    assert 'attachments' not in json.loads(untouched)['snapshot']['payload']
    apply(keep(ITEM, first_anchor))
    close_matrix(world(ITEM),initial_item['world_matrix'])
    step(12)
    old_item,old_detail=world(ITEM),world(DETAIL)
    assert abs(old_item[12]-initial_item['world_matrix'][12]-.2)<2e-7
    before=capture('before-handoff')
    args_retry, receipt=apply(keep(ITEM,second_anchor))
    assert session.rpc(APPLY,args_retry)==dict(receipt,replayed=True)
    session.rpc(APPLY,dict(args_retry,commands=[keep(ITEM,first_anchor)]),error=-32010)
    inspect(error=-32009,structure_revision=structure-1)
    session.rpc('runtime.capture',dict(session_id=owner,tick=tick,structure_revision=structure-1,
        camera=CAMERA,path=native.native(output/'stale.bmp')),error=-32009)
    assert not (output/'stale.bmp').exists()
    close_matrix(world(ITEM),old_item)
    close_matrix(world(DETAIL),old_detail)
    assert before==capture('after-handoff')
    binding=inspect()['bindings'][0]
    assert binding['anchor']==second_anchor
    assert observed(ITEM)['local_transform']==initial_item['local_transform']
    close_matrix(multiply(world(second_anchor),binding['offset_matrix']),old_item)
    checkpoint=save('handoff')
    payload=json.loads(checkpoint)['snapshot']['payload']
    extension=payload['attachments']
    assert extension['version']==1 and len(extension['bindings'])==1
    assert extension['bindings'][0]['offset_matrix']==binding['offset_matrix']
    trackers=payload['kinematic_sleep']['trackers']
    assert [row['entity'] for row in trackers]==[BODY]
    assert trackers[0]['state']['sleep_timer']>.1

    apply(keep(OTHER,ITEM),keep(ITEM,OTHER),error=-32602)
    apply(keep(OTHER,first_anchor),keep(BODY,first_anchor),error=-32602)
    apply(keep(OTHER,first_anchor),keep(OTHER,second_anchor),error=-32602)
    apply(keep(ITEM,DETAIL),error=-32602)
    apply(keep(ITEM,id_request()),error=-32602)
    apply(keep(ITEM,first_anchor),error=-32009,expected_structure_revision=structure-1)
    assert save('after-rejections')==checkpoint

    step(18)
    moved=world(ITEM)
    close_matrix(moved,multiply(world(second_anchor),binding['offset_matrix']))
    assert abs(moved[12]-old_item[12])>.1 and abs(moved[13]-old_item[13])>.2
    following=capture('following')
    if before is not None:
        assert following!=before
    continuation=save('continued')
    settled=json.loads(continuation)['snapshot']['payload']
    assert next(row for row in settled['entities'] if row['id']==BODY)['body']['active'] is False
    expected_tip=world(DETAIL)
    session.rpc('runtime.stop',dict(session_id=owner))
    session.close()

    session=native.Session(output/'cold',world=session.world)
    try:
        owner=id_request()
        session.rpc('save.configure',dict(request_id=id_request(),expected_generation=0,root=native.native(saves)))
        restored=session.rpc('save.load',dict(request_id=id_request(),configuration_generation=1,slot='handoff',
            expected_generation=1,revision=revision,expected_session_id=None,expected_tick=None,
            expected_gameplay_revision=None,expected_structure_revision=None,new_session_id=owner))
        tick=12
        assert restored['tick']==tick
        assert inspect()['bindings']==[binding]
        assert save('restored')==checkpoint
        step(5)
        step(13)
        assert save('restored-continuation')==continuation
        close_matrix(world(DETAIL),expected_tip)
        assert capture('restored-following')==following
        detached=world(ITEM)
        apply(keep(ITEM,None))
        step(5)
        assert inspect()['bindings'][0]['anchor'] is None
        close_matrix(world(ITEM),detached)
        platform=structure_change(spawns=[dict(template_id=PLATFORM)])['spawned'][0]
        apply(keep(ITEM,platform))
        physics_binding=inspect()['bindings'][0]
        step(4, motions=[dict(entity=platform,position=[22.375,0,0],rotation=[0,0,0,1],duration_ticks=4)])
        assert abs(world(platform)[12]-22.375)<2e-6
        moved_by_body=detached.copy()
        moved_by_body[12]+=.375
        close_matrix(world(ITEM),moved_by_body)
        close_matrix(world(ITEM),multiply(world(platform),physics_binding['offset_matrix']))

        # Two writes in one transaction depend on candidate anchor order.
        other_before=world(OTHER)
        apply(keep(OTHER,ITEM),dict(entity=ITEM,mode='snap',anchor=first_anchor,offset=transform((0,.5,0))))
        close_matrix(world(ITEM),multiply(world(first_anchor),inspect()['bindings'][0]['offset_matrix']))
        close_matrix(world(OTHER),other_before)
        step(5)
        state=inspect()['bindings']
        close_matrix(world(OTHER),multiply(world(ITEM),state[1]['offset_matrix']))

        created=structure_change(spawns=[dict(template_id=SPAWN)])['spawned'][0]
        apply(keep(ITEM,created))
        before_removal=save('before-anchor-removal')
        session.rpc('runtime.structure.transact',dict(request_id=id_request(),session_id=owner,
            expected_tick=tick,expected_structure_revision=structure,despawns=[created]),error=-32602)
        assert inspect()['bindings'][0]['anchor']==created
        assert save('after-anchor-removal')==before_removal
        apply(reset(ITEM),reset(OTHER))
        close_matrix(world(ITEM),initial_item['world_matrix'])
        structure_change(despawns=[created,platform])
        assert inspect()['bindings']==[]
        assert 'attachments' not in json.loads(save('reset'))['snapshot']['payload']
        assert session.world.read_bytes()==authored
        session.rpc('runtime.stop',dict(session_id=owner))
        return dict(cold_bytes_equal=True, continuation_bytes_equal=True, failed_batch_bytes_equal=True,
                    follows_kinematic_anchor=True, cold_idle_kinematic_slept=True, denied_anchor_removal_bytes_equal=True,
                    following_position_m=moved[12:15], before_handoff_m=old_item[12:15], captures=captures)
    finally:
        session.close()


def run(args,output):
    output.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+150
    report=dict(passed=False,engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session=native.Session(output/'original')
    try:
        info=session.rpc('world.describe',dict(view='method',name=APPLY))
        assert info['schema_revision']>=141 and info['methods'][APPLY]['properties']['commands']['maxItems']==64
        revision=configure(session)
        report.update(compare_pose(session,args,revision,output))
        report['passed']=True
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        session.close()
        report.update(owners=native.REPORT['owners'],rpc_count=len(native.REPORT['calls']))
        (output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Verified attachment transfers in {report["rpc_count"]} native calls.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    args=parser.parse_args()
    if args.output:
        run(args,args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='poima-attachments-',dir=args.binary.resolve().parent) as directory:
            run(args,Path(directory)/'check')


if __name__=='__main__':
    main()
