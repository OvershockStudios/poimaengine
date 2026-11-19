#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify candidate grip geometry, atomic rejection and guarded receipts."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import time

from runtime_surface_measure import geometry, HIDDEN, MANY, probe, REVERSED, SKIN, TARGET
from runtime_ik_attachment import save
from runtime_pose_frames import id_request, transform, uid
import world_physical_contract as native

ITEM, ANCHOR = uid(61), uid(62)


def configure(session):
    revision = geometry(session)
    ops = []
    for identity in (ITEM, ANCHOR, *[uid(i) for i in range(63, 67)]):
        ops.extend((dict(op='entity.create', id=identity, name='Attachment fixture'),
                    native.component(identity, 'Transform', transform())))
    for identity in (TARGET, MANY):
        ops.append(dict(op='entity.reparent', id=identity, parent=ITEM, mode='keep_local'))
    return native.transact(session, ops, revision)['revision']


def verify(session, revision):
    name = id_request()
    session.rpc('runtime.start', dict(session_id=name, revision=revision))
    saves = native.save_root(session)
    tick = structure = 0

    def read(*probes):
        return session.rpc('runtime.surface.measure', dict(session_id=name, tick=tick,
            structure_revision=structure, pairs=list(probes)))['pairs']

    def attach(surface, *others, error=None, request_id=None):
        request = dict(session_id=name, request_id=request_id or id_request(), expected_tick=tick,
            expected_structure_revision=structure, commands=[dict(entity=ITEM, anchor=ANCHOR,
                mode='keep_world', contact=dict(position_limit_m=0, rotation_limit_deg=0, surface=surface)), *others])
        return request, session.rpc('runtime.attachment.apply', request, error=error)

    def snapshot(label):
        return save(session, saves, label, name, tick, structure)

    def bindings():
        return session.rpc('runtime.attachment.inspect', dict(session_id=name,tick=tick,
            structure_revision=structure))['bindings']

    initial = snapshot('before')
    exact = probe(distance_limit_m=.125)
    missed = probe(distance_limit_m=.124999)
    expected = read(exact, missed)
    assert [row['within_distance_limit'] for row in expected] == [True, False]
    req, rejected = attach([exact, missed], error=-32040)
    assert rejected['data'] == dict(entity=ITEM, anchor=ANCHOR, probe_index=1,
        distance_limit_m=.124999, surface=expected[1])

    wrong_target = probe(target=REVERSED, distance_limit_m=1)
    invalid_source = probe(source=TARGET, distance_limit_m=1)
    hidden_source = probe(source=HIDDEN, distance_limit_m=1)
    duplicates = copy.deepcopy(exact); duplicates['target']['triangles'] = [0, 0]
    invalid_vertex = dict(source=dict(entity=SKIN, vertex=100), target=dict(entity=TARGET), distance_limit_m=1)
    collapsing = dict(entity=TARGET, mode='snap', anchor=ANCHOR, offset=transform())
    for bad in ([], [exact]*9, [probe()], [wrong_target], [invalid_source], [hidden_source],
                [duplicates], [invalid_vertex], [probe(target=MANY, distance_limit_m=1)]):
        attach(bad, error=-32602)
    attach([exact], collapsing, error=-32602)
    excessive = [dict(entity=uid(i),anchor=ANCHOR,mode='keep_world',
        contact=dict(position_limit_m=0,rotation_limit_deg=0,surface=[exact]*8)) for i in range(63,67)]
    attach([exact]*8, *excessive, error=-32602)

    # The other command moves the sampled drawable in the candidate. Its
    # committed point is close, but the pending position is a quarter metre off.
    moving = dict(source=dict(entity=REVERSED,vertex=0), target=dict(entity=TARGET), distance_limit_m=0.0)
    former = read(moving)[0]
    assert former['distance_m']==0
    displacement = dict(entity=REVERSED,mode='snap',anchor=ANCHOR,offset=transform((0,0,.25)))
    _, failed = attach([moving], displacement, error=-32040)
    candidate = failed['data']['surface']
    assert candidate['distance_m']==.25 and candidate['source']['point_m'][2]==.25
    assert read(moving)[0]==former and bindings()==[] and snapshot('after-misses')==initial

    fixed_request, accepted = attach([exact, moving], dict(displacement, offset=transform()),
                                     request_id=req['request_id'])
    structure = 1
    assert accepted['structure_revision']==structure and not accepted['replayed']
    assert session.rpc('runtime.attachment.apply',fixed_request)==dict(accepted,replayed=True)
    assert {entry['entity'] for entry in bindings()} == {ITEM, REVERSED}
    assert read(exact)[0] == expected[0]

    tick = session.rpc('runtime.step', dict(session_id=name, request_id=id_request(), expected_tick=0,
        expected_structure_revision=structure, ticks=30))['tick']
    drifting = probe(triangle=3,weights=(.3,.6,.1),distance_limit_m=.125)
    expected_moved = read(drifting)[0]
    assert expected_moved['distance_m']>.3
    before_late = snapshot('animated-before')
    _, moving_miss = attach([drifting],error=-32040)
    assert moving_miss['data']['surface']==expected_moved and snapshot('animated-after')==before_late
    session.rpc('runtime.stop',dict(session_id=name))
    return dict(committed_surface_miss=expected[1], proposed_surface_miss=candidate,
                moved_finger_miss=expected_moved, atomic_saves=2, corrected_request_replayed=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    args=parser.parse_args()
    args.output=args.output.resolve()
    args.output.mkdir()
    native.ARGS,native.DEADLINE=args,time.monotonic()+180
    report=dict(passed=False,engine_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
    session=native.Session(args.output/'session')
    try:
        info=session.rpc('world.describe',dict(view='method',name='runtime.attachment.apply'))
        assert info['schema_revision']>=146
        commands=info['methods']['runtime.attachment.apply']['properties']['commands']['items']['oneOf']
        surface=commands[1]['properties']['contact']['properties']['surface']
        assert surface['maxItems']==8 and 'distance_limit_m' in surface['items']['required']
        revision=configure(session)
        authored=session.world.read_bytes()
        report.update(verify(session,revision))
        assert session.world.read_bytes()==authored
        session.close()
        report['passed']=True
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:
        session.close()
        report['owners']=native.REPORT['owners']
        report['rpc_count']=len(native.REPORT['calls'])
        (args.output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Verified sampled attachment contact in {report["rpc_count"]} native calls.')


if __name__=='__main__':
    main()
