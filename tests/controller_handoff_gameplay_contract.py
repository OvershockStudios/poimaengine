#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual prebuilt CoreCLR controller/physical ownership and callback joins.

Builds nothing and opens no device/window. Run beneath an external process-tree
owner. Raw RPCs, complete typed states, archives, manifests and failures remain
intact. This is a procedural two-part fixture, not humanoid or NativeAOT proof.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import re
import time
import uuid

import world_physical_contract as f
from managed_physical_gameplay_contract import sha, strict_json

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT/'tests/managed_controller_handoff_gameplay'
GAME = 'Poima.Tests.ControllerHandoffGame'
ACTOR, CAMERA, CONTROL, REGION, TARGET = (f'{n:032x}' for n in (1, 2, 20, 30, 600))
FIELDS = {
    **dict.fromkeys(('Actor', 'Camera', 'Rig', 'Control', 'Region', 'Target'), 'entity'),
    **dict.fromkeys(('Mode', 'Ticks', 'Dead', 'CheckGeometry', 'PoseChecks', 'GeometryChecks',
                     'StagedChecks', 'Writes', 'ObservedMode', 'Crouched'), 'int32'),
    **dict.fromkeys(('FailAt', 'ObservedTick', 'RootBody', 'TipBody'), 'int64'),
    **dict.fromkeys(('CommittedYaw', 'CommittedPitch', 'ControlYaw', 'ControlPitch', 'ObservedYaw', 'ObservedPitch',
                     'ActorX', 'ActorY', 'ActorZ', 'ActorFx', 'ActorFy', 'ActorFz', 'Vx', 'Vy', 'Vz',
                     'CameraX', 'CameraY', 'CameraZ', 'CameraFx', 'CameraFy', 'CameraFz',
                     'BaseX', 'BaseY', 'BaseZ', 'BaseFx', 'BaseFy', 'BaseFz',
                     'EffectX', 'EffectY', 'EffectZ', 'RegionDistance', 'GeometryDistance'), 'float64'),
}
need, exact = f.need, f.exact


def fresh():
    return uuid.uuid4().hex


def typed(values):
    need(type(values) is dict and set(values) == set(FIELDS), 'Complete52-field callback state differs')
    for name, kind in FIELDS.items():
        value = values[name]
        valid = (type(value) is str and re.fullmatch('[0-9a-f]{32}', value) is not None) if kind == 'entity' else (
            type(value) is int and -(2**31) <= value < 2**31) if kind == 'int32' else (
            type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None and
            -(2**63) <= int(value) < 2**63) if kind == 'int64' else type(value) is float and math.isfinite(value)
        need(valid, 'Callback state value/type differs: '+name)
    return values


def transform(position):
    return dict(position=list(position), rotation=[0, 0, 0, 1], scale=[1, 1, 1])


def input_for(entity, move=(0, 0), look=(0, 0), jump=False):
    return dict(entity=entity, move=list(move), look=list(look), jump=jump, use=False)


def configure(io, asset):
    def create(identity, name, parent=None):
        return dict(op='entity.create', id=identity, name=name, parent=parent)
    component = f.component
    controller = dict(radius=.3, height=1.8, speed=3, jump_speed=5,
                      camera=CAMERA, locomotion={}, stance=dict(crouched_height=1, crouched_speed=1, camera_drop=.5))
    other = dict(radius=.3, height=1.8, speed=3, jump_speed=5, camera=None, locomotion={})
    profile = f.profile()
    profile['controller_handoff'] = dict(actor=ACTOR, follow='translate_keep_heading')
    ops = [create(ACTOR, 'Linked actor'), component(ACTOR, 'Transform', transform((0, .1, 0))),
           component(ACTOR, 'CharacterController', controller), create(CAMERA, 'Linked eye', ACTOR),
           component(CAMERA, 'Transform', transform((0, 1.6, 0))),
           component(CAMERA, 'Camera', dict(vertical_fov=70, near=.05, far=100)),
           create(CONTROL, 'Independent controller'), component(CONTROL, 'Transform', transform((6, .1, 0))),
           component(CONTROL, 'CharacterController', other),
           create(REGION, 'Moving aim witness', ACTOR), component(REGION, 'Transform', transform((0, 1.6, -4))),
           component(REGION, 'HitRegion', dict(owner=ACTOR, name='aim-witness', shape='sphere', radius=.2)),
           create(TARGET, 'New-look native target'), component(TARGET, 'Transform', transform((-8, 1.6, 0))),
           component(TARGET, 'BoxCollider', dict(half_extents=[.5, .5, .5], motion='static', mass=1, friction=.5, restitution=0)),
           dict(op='entity.reparent', id=f.RIG, parent=ACTOR, mode='keep_local'),
           component(f.RIG, 'Transform', transform((0, .5, 0))),
           component(f.RIG, 'AnimationRig', dict(asset=asset, clip=None, time=0, speed=1, loop=False, playing=False)),
           component(f.RIG, 'PhysicalRig', profile)]
    need(f.transact(io, ops, 1)['revision'] == 2, 'Linked fixture did not commit revision2')


class Consumer:
    def __init__(self, io, report, config):
        self.io, self.report, self.config = io, report, config
        self.session, self.tick = fresh(), 0

    def game(self):
        result = self.io.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))
        module = result['module']
        need(module['backend'] == 'coreclr' and module['assembly_sha256'] == self.report['assembly_sha256'], 'Wrong compiled CoreCLR image/backend')
        need(module['schema']['identity'] == 'poima.test.controller-handoff' and
             {row['name']: row['kind'] for row in module['schema']['fields']} == FIELDS and
             len(module['schema']['fields']) == len(FIELDS), 'Generated complete callback schema differs')
        typed(module['values'])
        return result

    def values(self):
        return self.game()['module']['values']

    def inspect(self):
        result = self.io.rpc('runtime.inspect', dict(session_id=self.session))
        need(type(result['tick']) is int and result['tick'] == self.tick, 'Native owner clock differs')
        return result

    def handoff(self):
        result = self.io.rpc('runtime.controller_handoff', dict(session_id=self.session, tick=self.tick, entity=f.RIG))
        need(set(result) == {'session_id', 'tick', 'source', 'state'} and result['session_id'] == self.session and
             type(result['tick']) is int and result['tick'] == self.tick and result['source'] == 'runtime', 'Handoff observation envelope differs')
        state = result['state']
        need(type(state) is dict and set(state) == {'entity', 'actor', 'phase', 'capsule_body_id', 'capsule_enrolled', 'takeover_tick', 'support'} and
             state['entity'] == f.RIG and state['actor'] == ACTOR and state['phase'] in ('locomotion', 'physical') and
             type(state['capsule_body_id']) is int and 0 <= state['capsule_body_id'] <= 0xffffffff and type(state['capsule_enrolled']) is bool,
             'Complete handoff observation/state types differ')
        if state['phase'] == 'locomotion':
            need(state['capsule_enrolled'] is True and state['takeover_tick'] is None and state['support'] is None, 'Locomotion ownership is inconsistent')
        else:
            need(state['capsule_enrolled'] is False and type(state['takeover_tick']) is int and
                 type(state['support']) is dict and state['support']['semantics'] == 'cached_at_takeover', 'Physical ownership/cached support differs')
            support = state['support']
            need(set(support) == {'semantics', 'ground', 'body_id', 'normal', 'velocity'} and
                 support['ground'] in ('on_ground', 'steep_ground', 'not_supported', 'in_air') and
                 type(support['body_id']) is int and 0 <= support['body_id'] <= 0xffffffff,
                 'Cached support membership/identity differs')
            for key in ('normal', 'velocity'):
                need(type(support[key]) is list and len(support[key]) == 3 and
                     all(type(x) is float and math.isfinite(x) for x in support[key]), 'Cached support numeric types differ')
        return state

    def physical(self):
        return f.query(self.io, self.tick, session=self.session, include_parts=True, include_joints=True)

    def entity(self, entity):
        return self.io.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=entity))

    def edit(self, **changes):
        need(set(changes) <= {'Mode', 'CheckGeometry', 'FailAt'}, 'Verifier edited non-control callback fields')
        before = self.game()
        self.io.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
                    expected_revision=before['revision'], values=changes))
        after = self.values()
        for key, value in changes.items():
            need(type(after[key]) is type(value) and exact(after[key], value), 'Typed control edit was coerced')

    def step(self, ticks=1, inputs=(), error=None, **extra):
        result = self.io.rpc('runtime.step', dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
                    ticks=ticks, inputs=list(inputs), **extra), error)
        if error is None:
            need(type(result['tick']) is int and result['tick'] == self.tick+ticks and result['replayed'] is False and
                 result['current_session_id'] == self.session and result['current_tick'] == result['tick'], 'Actual accepted step receipt differs')
            self.tick = result['tick']
        return result

    def snapshot(self, saves, label):
        state, game = self.inspect(), self.game()
        slot = 'handoff-'+str(len(self.report['archives'])+1)
        receipt = self.io.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot,
            expected_generation=0, session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], expected_material_revision=state['material_revision'],
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence']))
        manifest_path = saves/('slot-'+slot)/'current.json'
        need(manifest_path.stat().st_size <= 1024*1024, 'Save manifest exceeds1MiB')
        manifest = strict_json(manifest_path.read_bytes()); member = manifest['payload']['current']
        need(type(member['file']) is str and re.fullmatch('p-[0-9a-f]{32}\\.bin', member['file']), 'Unsafe payload member')
        path = manifest_path.parent/member['file']; raw = path.read_bytes()
        need(len(raw) <= 32*1024*1024 and type(receipt['bytes']) is int and type(member['bytes']) is int and
             len(raw) == receipt['bytes'] == member['bytes'] and sha(path) == receipt['sha256'] == member['sha256'], 'Complete archive/manifest/receipt identity differs')
        archive = strict_json(raw); payload = archive['snapshot']['payload']
        need(archive['format'] == 'poima.world-save' and archive['version'] == 1 and payload['tick'] == self.tick and
             payload['gameplay']['backend'] == 'coreclr' and payload['gameplay']['assembly_sha256'] == self.report['assembly_sha256'] and
             payload['gameplay']['type'] == GAME and exact(typed(payload['gameplay']['values']), game['module']['values']),
             'Complete saved gameplay state/backend/image differs from current source')
        record = dict(label=label, slot=slot, tick=self.tick, path=str(path), bytes=len(raw), sha256=sha(path),
                      archive=archive, receipt=receipt, manifest=manifest, manifest_path=str(manifest_path), handoff=self.handoff())
        self.report['archives'].append(copy.deepcopy(record))
        return record

    def restore(self, row, existing=True):
        state, game = (self.inspect(), self.game()) if existing else (None, None)
        sid = fresh()
        result = self.io.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot=row['slot'], expected_generation=1,
            revision=2, expected_session_id=self.session if existing else None, expected_tick=self.tick if existing else None,
            expected_gameplay_revision=game['revision'] if existing else None, expected_structure_revision=state['structure_revision'] if existing else None,
            expected_material_revision=state['material_revision'] if existing else None, expected_ui_revision=state['ui_revision'] if existing else None,
            expected_control_sequence=state['control_sequence'] if existing else None, new_session_id=sid, gameplay=self.config))
        need(type(result['tick']) is int and result['tick'] == row['tick'], 'Restore receipt clock differs')
        self.session, self.tick = sid, result['tick']
        need(self.inspect()['session_id'] == sid and exact(self.values(), row['archive']['snapshot']['payload']['gameplay']['values']), 'Original complete typed restore differs')


def pair(report, label, a, b):
    need(Path(a['path']).read_bytes() == Path(b['path']).read_bytes() and exact(a['archive'], b['archive']), 'Complete archive comparison differs: '+label)
    report['whole_pairs'].append(dict(label=label, left=a['path'], right=b['path'], bytes_equal=True, complete_archive_equal=True, exclusions=[]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'hostfxr', 'bridge', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    f.ARGS, f.DEADLINE, f.MAX_CALLS = args, time.monotonic()+240, 1200
    report = dict(format='poima.controller-handoff-coreclr.v1', passed=False, backend='coreclr', calls=[], owners=[], archives=[],
                  whole_pairs=[], checks=[], source_pins={}, assembly_sha256=sha(args.assembly),
                  limits=dict(seconds=240, rpc=1200, output_bytes=64*1024*1024),
                  limitations=['Procedural two-part rig, actual prebuilt CoreCLR consumer and headless native engine. No humanoid, NativeAOT, renderer, physical input or performance claim.',
                               'External process-tree owner required. This fixture records naturally closed CLI children and preserves all original failures.'])
    f.REPORT = report
    config = dict(hostfxr=f.native(args.hostfxr), bridge=f.native(args.bridge), assembly=f.native(args.assembly), type=GAME)
    files = [Path(__file__).resolve(), Path(f.__file__).resolve(), ROOT/'tests/managed_physical_gameplay_contract.py',
             ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py', SAMPLE/'ManagedControllerHandoffGame.cs',
             SAMPLE/'Poima.ManagedControllerHandoffGame.csproj', ROOT/'managed/Directory.Build.props',
             args.binary.resolve(), args.hostfxr.resolve(), args.bridge.resolve(), args.assembly.resolve()]
    for image in (args.bridge.resolve(), args.assembly.resolve()):
        files.append(image.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json', '.runtimeconfig.json'):
            member = image.with_suffix(suffix)
            if member.is_file(): files.append(member)
    io = c = None
    try:
        for path in set(files):
            need(path.is_file(), 'Pinned source or compiled member absent: '+str(path)); report['source_pins'][str(path)] = sha(path)
        need(sha(args.bridge.parent/'Poima.Gameplay.dll') == sha(args.assembly.parent/'Poima.Gameplay.dll'), 'Consumer and bridge SDK bytes differ')
        io = f.Session(out/'warm'); asset = f.prepare(io); configure(io, asset)
        saves = f.save_root(io); c = Consumer(io, report, config)
        io.rpc('runtime.start', dict(session_id=c.session, revision=2)); c.step(120)
        io.rpc('runtime.gameplay.load', dict(session_id=c.session, request_id=fresh(), expected_tick=c.tick, expected_revision=0, **config))
        c.game()
        effects = [dict(entity=CAMERA, world=dict(position=[.125, .0625, -.25], rotation=[0, 0, 0, 1]))]
        c.step(camera_poses=effects)
        alive = c.snapshot(saves, 'alive-reference')
        need(alive['handoff']['phase'] == 'locomotion' and c.entity(ACTOR)['ground'] == 'on_ground', 'Native grounded locomotion precondition absent')
        capsule_id = alive['handoff']['capsule_body_id']
        ray = dict(session_id=c.session, tick=c.tick, origin=[2, 1.75, 0], direction=[-1, 0, 0], distance=4)
        before_ray = io.rpc('runtime.raycast', ray)
        need(before_ray['hit'] is not None and before_ray['hit']['entity'] == ACTOR, 'Enrolled capsule witness is absent')
        held = [input_for(ACTOR, (1, 1), (90, 0), True), input_for(CONTROL, (0, 0), (11, 0))]
        for mode in (3, 6):
            c.edit(Mode=mode); before = c.snapshot(saves, 'rejected-mode-'+str(mode)+'-before')
            c.step(inputs=held, error=-32040)
            pair(report, 'whole callback rollback '+str(mode), before, c.snapshot(saves, 'rejected-mode-'+str(mode)+'-after'))
        c.restore(alive)
        c.edit(Mode=1, CheckGeometry=1, FailAt=str(c.tick+1))
        before = c.snapshot(saves, 'late-failure-before'); c.step(2, held, error=-32040)
        pair(report, 'whole later-Tick ownership/capsule/game rollback', before, c.snapshot(saves, 'late-failure-after'))
        need(c.handoff()['phase'] == 'locomotion' and c.handoff()['capsule_body_id'] == capsule_id, 'Later rejection retained suppressed capsule')
        c.restore(alive); c.edit(Mode=1, CheckGeometry=1)
        takeover_tick = c.tick; c.step(inputs=held)
        after = c.handoff(); values = c.values(); physical = c.physical()
        need(after['phase'] == 'physical' and after['capsule_body_id'] == capsule_id and after['takeover_tick'] == takeover_tick and
             values['ObservedYaw'] == 90.0 and values['ObservedPitch'] == 0.0 and values['Dead'] == 1 and values['GeometryChecks'] == 1 and
             abs(values['RegionDistance']-3.8) < 2e-5 and abs(values['GeometryDistance']-7.5) < 2e-5 and values['Vy'] == 5.0,
             'Compiled new-look geometry/jump or takeover witness differs')
        need(after['support']['ground'] == 'on_ground' and
             all(abs(x-y) < 1e-6 for x, y in zip(after['support']['normal'], (0, 1, 0))) and
             all(abs(x) < 1e-6 for x in after['support']['velocity']), 'Static-floor support was not captured before held jump/look')
        need([values['EffectX'], values['EffectY'], values['EffectZ']] == [.125, .0625, -.25], 'Authored effects vanished from callback')
        actor = c.entity(ACTOR)
        need(actor['yaw'] == 0.0 and actor['pitch'] == 0.0 and values['ControlYaw'] == 11.0 and physical['mode'] == 'passive' and
             all(part['enrolled'] is True for part in physical['parts']), 'Activation committed held look, lost control input or failed rig enrollment')
        ray.update(session_id=c.session, tick=c.tick)
        need(io.rpc('runtime.raycast', ray)['hit'] is None, 'Suppressed capsule remains a high physical obstruction')
        center = physical['parts'][0]['position']
        hit = io.rpc('runtime.raycast', dict(session_id=c.session, tick=c.tick, origin=[center[0]+2, center[1], center[2]], direction=[-1, 0, 0], distance=4))['hit']
        need(hit is not None and hit['entity'] == f.RIG, 'Enrolled physical part lacks actual ray geometry')
        passive = c.snapshot(saves, 'passive-reference')

        def future():
            c.step(inputs=[input_for(ACTOR, (-1, 1), (37, -17), True), input_for(CONTROL, (0, 0), (-7, 4))],
                   character_stances=[dict(entity=ACTOR, crouch=True)])
            c.step(3, [input_for(ACTOR, (1, -1), (-17, 9), True)])
            state, observed = c.handoff(), c.values()
            need(state['phase'] == 'physical' and state['capsule_body_id'] == capsule_id and observed['CommittedYaw'] == 0.0 and
                 observed['CommittedPitch'] == 0.0 and observed['Crouched'] == 0, 'Suppressed controls modified captured ownership/view/posture')

        future(); continued = c.snapshot(saves, 'warm-future')
        c.restore(passive); pair(report, 'warm passive complete roundtrip', passive, c.snapshot(saves, 'warm-passive-roundtrip'))
        future(); pair(report, 'warm restored complete future', continued, c.snapshot(saves, 'warm-restored-future'))
        world_path = io.world; io.close(); io = None
        io = f.Session(out/'cold', world=world_path); c = Consumer(io, report, config)
        io.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=f.native(saves)))
        c.restore(passive, existing=False); pair(report, 'cold passive complete roundtrip', passive, c.snapshot(saves, 'cold-passive-roundtrip'))
        future(); pair(report, 'cold restored complete future', continued, c.snapshot(saves, 'cold-restored-future'))
        c.restore(alive); c.edit(Mode=2); c.step(inputs=[input_for(CONTROL, look=(-19, 0))])
        compiled = c.values(); state = c.handoff()
        need(state['phase'] == 'physical' and state['capsule_body_id'] == capsule_id and compiled['ObservedYaw'] == 0.0 and
             compiled['CommittedYaw'] == 0.0 and compiled['CommittedPitch'] == 0.0 and compiled['Writes'] == 3 and compiled['Dead'] == 1 and
             c.entity(ACTOR)['yaw'] == 0.0, 'Compiled held intents were applied after activation or leaked into staged reads')
        c.snapshot(saves, 'compiled-held-takeover')
        io.close(); io = None
        report['recordings'] = []
        for owner in report['owners']:
            need(owner['natural_exit'] is True and owner['forced_cleanup'] is False and
                 owner['exit_code'] == 0 and owner['reader_failure'] is None, 'Native owner did not close naturally before transcript verification')
            directory = Path(owner['directory'])
            calls = [row for row in report['calls'] if Path(row['owner']) == directory]
            for name, field in (('requests', 'request'), ('responses', 'response')):
                path = directory/(name+'.jsonl')
                rows = [strict_json(line) for line in path.read_bytes().splitlines() if line.strip()]
                need(exact(rows, [row[field] for row in calls]), 'Literal '+name+' differ from retained call history')
                report['recordings'].append(dict(path=str(path), bytes=path.stat().st_size, sha256=sha(path)))
        report['checks'] = ['Independent analytic same-Tick actor/camera/BaseWorld/effect and attached-region/native-ray joins through actual compiled CoreCLR callbacks',
                            'Caller-held and compiled-held movement/look/jump/crouch superseded by one-way transfer; unrelated controller retains its input',
                            'Actual capsule ray disappears, physical-part ray appears, raw capsule identity retained and support labeled historical',
                            'Whole callback/later-Tick rollback and complete52-field/native archive byte equality through warm/cold continuation']
        need(all(sha(Path(path)) == digest for path, digest in report['source_pins'].items()), 'Pinned input changed during workflow')
        report['passed'] = True
    except BaseException as error:
        report['error'] = repr(error); raise
    finally:
        if io is not None:
            try: io.close()
            except BaseException as error:
                report['passed'] = False; report['close_error'] = repr(error)
        report['rpc_count'] = len(report['calls'])
        report['natural_cli_owners'] = sum(row.get('natural_exit') is True and row.get('exit_code') == 0 and not row['forced_cleanup'] for row in report['owners'])
        raw = (json.dumps(report, indent=2, allow_nan=False)+'\n').encode()
        need(len(raw) <= 32*1024*1024 and sum(p.stat().st_size for p in out.rglob('*') if p.is_file())+len(raw) <= 64*1024*1024, 'Bounded workflow output exceeded')
        (out/'evidence.json').write_bytes(raw); print(out/'evidence.json', flush=True)
        if not report['passed'] and 'error' not in report:
            raise RuntimeError(report.get('close_error', 'Workflow did not complete'))


if __name__ == '__main__':
    main()
