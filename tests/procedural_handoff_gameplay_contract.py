#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual prebuilt CoreCLR manual-IK/controller/physical ownership consumer.

Builds nothing, opens no window/device, and uses three sequential native CLI
owners beneath an external process-tree owner. Retains original literal RPC
streams and complete typed/native saves. This is analytic three-joint content,
not imported-character, Native AOT, contact-quality or performance evidence.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import time

import world_physical_contract as f
import controller_handoff_gameplay_contract as prior
from gltf_fixture import glb
from managed_physical_gameplay_contract import sha, strict_json

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT/'tests/managed_procedural_handoff_gameplay'
GAME, IDENTITY = 'Poima.Tests.ProceduralHandoffGame', 'poima.test.procedural-handoff'
ACTOR, CAMERA, RIG, OTHER, FLOOR = (f'{n:032x}' for n in (1, 2, 100, 200, 10))
FIELDS = {
    **dict.fromkeys(('Actor', 'Camera', 'Rig', 'Root', 'Middle', 'End', 'OtherRig', 'OtherEnd'), 'entity'),
    **dict.fromkeys(('Mode', 'Ticks', 'Dead', 'PoseChecks', 'StagedChecks', 'OwnWrites', 'OtherWrites', 'OtherChecks',
                     'HistoricalReads', 'HistoricalInitialized', 'MalformedChecks', 'ObservedMode', 'HasOrientation',
                     'HistoricalEnabled', 'HistoricalSpace', 'HistoricalClamped', 'HistoricalFallback', 'HistoricalHasOrientation'), 'int32'),
    **dict.fromkeys(('FailAt', 'ObservedTick', 'DeathTick', 'RootBody', 'MiddleBody', 'EndBody'), 'int64'),
    **dict.fromkeys(('CommittedYaw', 'CommittedPitch', 'ObservedYaw', 'ObservedPitch',
                     'ActorX', 'ActorY', 'ActorZ', 'CameraX', 'CameraY', 'CameraZ',
                     'TargetX', 'TargetY', 'TargetZ', 'EndX', 'EndY', 'EndZ', 'TargetError', 'OtherEndX', 'OtherEndY', 'OtherEndZ',
                     'HistoricalWeight', 'HistoricalTargetX', 'HistoricalTargetY', 'HistoricalTargetZ',
                     'HistoricalPoleX', 'HistoricalPoleY', 'HistoricalPoleZ', 'HistoricalEndX', 'HistoricalEndY', 'HistoricalEndZ', 'HistoricalError',
                     'HistoricalGoalX', 'HistoricalGoalY', 'HistoricalGoalZ', 'HistoricalGoalW', 'HistoricalOrientationWeight',
                     'HistoricalActualX', 'HistoricalActualY', 'HistoricalActualZ', 'HistoricalActualW', 'HistoricalAngularError',
                     'OrientationGoalX', 'OrientationGoalY', 'OrientationGoalZ', 'OrientationGoalW', 'OrientationWeight', 'AngularError', 'CallbackBoneGap'), 'float64'),
}
need, exact, fresh = f.need, f.exact, prior.fresh


def typed(values):
    need(type(values) is dict and set(values) == set(FIELDS), 'Complete managed procedural state membership differs')
    for name, kind in FIELDS.items():
        value = values[name]
        valid = (type(value) is str and re.fullmatch('[0-9a-f]{32}', value) is not None) if kind == 'entity' else (
            type(value) is int and -(2**31) <= value < 2**31) if kind == 'int32' else (
            type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)', value) is not None and
            -(2**63) <= int(value) < 2**63) if kind == 'int64' else type(value) is float and math.isfinite(value)
        need(valid, 'Managed procedural field type/value differs: '+name)
    return values


def near(a, b, tolerance=2e-6):
    need(type(a) in (float, int) and type(b) in (float, int) and math.isfinite(a) and math.isfinite(b) and abs(a-b) <= tolerance,
         'Independent procedural scalar differs: '+repr((a, b, tolerance)))


def vector(value, count):
    need(type(value) is list and len(value) == count and all(type(x) is float and math.isfinite(x) for x in value),
         'Native vector type/finite membership differs')


def source_asset():
    """Reviewable three-node chain with one triangle and original scalar curves."""
    blob, views, accessors = bytearray(), [], []
    def accessor(rows, kind, fmt='f'):
        while len(blob) % 4: blob.append(0)
        start = len(blob); flat = [x for row in rows for x in row]
        blob.extend(struct.pack('<'+fmt*len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=start, byteLength=len(blob)-start))
        accessors.append(dict(bufferView=len(views)-1, componentType=5126 if fmt == 'f' else 5123,
                              count=len(rows), type=kind))
        return len(accessors)-1
    positions = accessor([(0, 0, 0), (.0625, 0, 0), (0, .0625, 0)], 'VEC3')
    accessors[positions].update(min=[0, 0, 0], max=[.0625, .0625, 0])
    normals = accessor([(0, 0, 1)]*3, 'VEC3'); indices = accessor([(0,), (1,), (2,)], 'SCALAR', 'H')
    times = accessor([(0,), (2,)], 'SCALAR'); accessors[times].update(min=[0], max=[2])
    translation = accessor([(0, 0, 0), (.03, 0, 0)], 'VEC3')
    scales = accessor([(1, 1, 1), (1.000004,)*3], 'VEC3')
    document = dict(asset=dict(version='2.0', generator='Poima original analytic three-joint procedural-handoff fixture'),
        buffers=[dict(byteLength=len(blob))], bufferViews=views, accessors=accessors,
        meshes=[dict(primitives=[dict(attributes=dict(POSITION=positions, NORMAL=normals), indices=indices)])],
        nodes=[dict(name='Root', mesh=0, children=[1]), dict(name='Middle', translation=[0, .5, 0], children=[2]),
               dict(name='End', translation=[0, .5, 0])], scenes=[dict(nodes=[0])], scene=0,
        animations=[dict(name='Original root translation and bounded scale',
            samplers=[dict(input=times, output=translation, interpolation='LINEAR'), dict(input=times, output=scales, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(node=0, path='translation')), dict(sampler=1, target=dict(node=0, path='scale'))])])
    return glb(document, bytes(blob))


def node(rig, index):
    return hashlib.sha256(f'poima.instance.v1/{rig}/node/{index}'.encode()).hexdigest()[:32]


def transform(position):
    return dict(position=list(position), rotation=[0, 0, 0, 1], scale=[1, 1, 1])


def profile():
    frame = lambda p: dict(position=list(p), rotation=[0, 0, 0, 1])
    parts = [dict(name=name, node=index, body_in_bone=frame((0, 0, 0)), shape=dict(kind='sphere', radius=.08),
        mass=mass, friction=.5, restitution=0, linear_damping=0, angular_damping=0)
        for index, (name, mass) in enumerate((('root', 3), ('middle', 2), ('end', 1)))]
    joints = [dict(name='joint'+str(index), parent=('root', 'middle')[index-1], child=('middle', 'end')[index-1],
        parent_frame=frame((0, .5, 0)), child_frame=frame((0, 0, 0)),
        limits=dict(kind='swing_twist', swing_y=3, swing_z=3, twist_min=-3, twist_max=3),
        neutral_rotation=[0, 0, 0, 1], rest_stiffness=0, rest_damping=0, soft_limit_stiffness=0,
        soft_limit_zone=.1, max_torque=1000, friction_torque=0) for index in (1, 2)]
    return dict(root='root', initial_mode='animated', parts=parts, joints=joints,
        pose_frame='skeletal_bounded_scale', max_scale_deviation=1e-5,
        controller_handoff=dict(actor=ACTOR, follow='translate_keep_heading'), procedural_pose='animated_only')


def configure(io, report):
    source = io.directory/'original-three-joint.glb'; source.write_bytes(source_asset())
    imported = io.rpc('asset.import', dict(source=f.native(source))); asset = imported['asset']
    need(type(asset) is str and re.fullmatch('[0-9a-f]{64}', asset), 'Cooked analytic model identity absent')
    operations = [dict(op='asset.instantiate', id=RIG, name='Linked analytic three-joint rig', asset=asset),
                  dict(op='asset.instantiate', id=OTHER, name='Independent manual IK rig', asset=asset)]
    need(f.transact(io, operations, 0)['revision'] == 1, 'Analytic asset instances did not commit revision1')
    document = strict_json(io.world.read_bytes()); c = f.component
    create = lambda identity, name, parent=None: dict(op='entity.create', id=identity, name=name, parent=parent)
    operations = [create(ACTOR, 'Virtual procedural actor'), c(ACTOR, 'Transform', transform((0, 5, 0))),
        c(ACTOR, 'CharacterController', dict(radius=.3, height=1.8, speed=2, jump_speed=5, camera=CAMERA,
            locomotion={}, stance=dict(crouched_height=1, crouched_speed=1, camera_drop=.5))),
        create(CAMERA, 'Separate owned eye', ACTOR), c(CAMERA, 'Transform', transform((.05, 1.6, .02))),
        c(CAMERA, 'Camera', dict(vertical_fov=70, near=.05, far=100)), create(FLOOR, 'Actual floor'),
        c(FLOOR, 'Transform', transform((0, -.5, 0))), c(FLOOR, 'BoxCollider', dict(half_extents=[12, .5, 12], motion='static', mass=1, friction=.5, restitution=0)),
        dict(op='entity.reparent', id=RIG, parent=ACTOR, mode='keep_local'), c(RIG, 'Transform', transform((.8, 2.5, 0))),
        c(OTHER, 'Transform', transform((-4, 4, 0)))]
    for identity in (RIG, OTHER):
        animation = copy.deepcopy(document['entities'][identity]['components']['AnimationRig'])
        chain = dict(slot=1, root=0, middle=1, end=2, enabled=True, space='world' if identity == RIG else 'rig', weight=1,
                     target=[1.35, 8.05, .2] if identity == RIG else [.4, .6, .1], pole=[.8, 7.5, 1] if identity == RIG else [0, 0, 1])
        if identity == RIG: chain['end_orientation'] = dict(rotation=[0, math.sin(.1), 0, math.cos(.1)], weight=.35)
        animation.update(clip=0, time=0, speed=1, loop=True, playing=True, ik=[chain])
        operations.append(c(identity, 'AnimationRig', animation))
    operations.append(c(RIG, 'PhysicalRig', profile()))
    need(f.transact(io, operations, 1)['revision'] == 2, 'Linked manual IK authoring did not commit revision2')
    document = strict_json(io.world.read_bytes()); mapping = {}
    for identity in (RIG, OTHER):
        actual = {row['components']['RigNode']['node']: key for key, row in document['entities'].items()
                  if row['components'].get('RigNode', {}).get('rig') == identity}
        need(actual == {index: node(identity, index) for index in range(3)}, 'Complete actual cooked RigNode binding differs')
        mapping[identity] = actual
    package = Path(str(io.world)+'.assets')/(asset+'.pmodel')
    need(package.is_file() and package.stat().st_size <= 1024*1024, 'Cooked analytic package missing or unexpectedly large')
    report['fixture'] = dict(asset=asset, source=str(source), source_sha256=sha(source), package=str(package), package_sha256=sha(package),
        authored_world=str(io.world), authored_sha256=sha(io.world), revision=2, nodes=mapping, entities=sorted(document['entities']))
    need(len(report['fixture']['entities']) == 13, 'Complete authored analytic fixture inventory differs')
    return mapping


class Consumer(prior.Consumer):
    def game(self):
        result = self.io.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True)); module = result['module']
        need(module['backend'] == 'coreclr' and module['assembly_sha256'] == self.report['assembly_sha256'] and module['schema']['identity'] == IDENTITY,
             'Actual CoreCLR consumer/backend/image differs')
        fields = module['schema']['fields']; need(len(fields) == len(FIELDS) and {row['name']: row['kind'] for row in fields} == FIELDS, 'Complete generated procedural schema differs')
        offset, layout = 0, {}
        for name, kind in FIELDS.items():
            size = 16 if kind == 'entity' else 4 if kind == 'int32' else 8; align = min(size, 8); offset = (offset+align-1)//align*align
            layout[name] = dict(name=name, kind=kind, offset=offset, bytes=size); offset += size
        need(type(module['schema']['bytes']) is int and module['schema']['bytes'] == (offset+7)//8*8, 'Complete generated state byte size differs')
        for row in fields: need(exact(row, layout[row['name']]), 'Generated scalar field layout differs: '+row['name'])
        typed(module['values']); return result

    def edit(self, **changes):
        need(set(changes) <= {'Mode', 'FailAt', 'Root', 'Middle', 'End', 'OtherEnd'}, 'Verifier edited non-control/nonbinding managed fields')
        before = self.game(); self.io.rpc('runtime.gameplay.edit', dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
            expected_revision=before['revision'], values=changes)); after = self.values()
        for key, value in changes.items(): need(type(after[key]) is type(value) and exact(after[key], value), 'Typed edit was coerced: '+key)

    def ik(self, identity=RIG):
        result = self.io.rpc('runtime.ik', dict(session_id=self.session, tick=self.tick, entity=identity, slot=1, include_end_orientation=True))
        state = result['state']; keys = {'entity', 'slot', 'root', 'middle', 'end', 'enabled', 'space', 'target', 'pole', 'weight', 'end_position', 'target_error', 'clamped', 'pole_fallback', 'end_orientation'}
        need(set(result) == {'session_id', 'tick', 'source', 'state'} and result['session_id'] == self.session and type(result['tick']) is int and result['tick'] == self.tick and result['source'] == 'runtime', 'IK envelope differs')
        need(type(state) is dict and set(state) == keys and state['entity'] == identity and [state['slot'], state['root'], state['middle'], state['end']] == [1, 0, 1, 2] and
             all(type(state[k]) is int for k in ('slot', 'root', 'middle', 'end')), 'Complete IK state identity/indices/types differ')
        for key in ('enabled', 'clamped', 'pole_fallback'): need(type(state[key]) is bool, 'IK Boolean was coerced')
        need(state['space'] == ('world' if identity == RIG else 'rig'), 'IK target space differs')
        for key in ('target', 'pole', 'end_position'): vector(state[key], 3)
        for key in ('weight', 'target_error'): need(type(state[key]) is float and math.isfinite(state[key]), 'IK scalar type differs')
        orientation = state['end_orientation']
        if orientation is not None:
            need(type(orientation) is dict and set(orientation) == {'rotation', 'weight', 'actual_rotation', 'angular_error'}, 'Complete orientation state differs')
            vector(orientation['rotation'], 4); vector(orientation['actual_rotation'], 4)
            for key in ('weight', 'angular_error'): need(type(orientation[key]) is float and math.isfinite(orientation[key]), 'Orientation scalar type differs')
        return state

    def procedural(self):
        result = self.io.rpc('runtime.procedural_pose', dict(session_id=self.session, tick=self.tick, entity=RIG)); state = result['state']
        need(set(result) == {'session_id', 'tick', 'source', 'state'} and result['session_id'] == self.session and type(result['tick']) is int and result['tick'] == self.tick and result['source'] == 'runtime', 'Procedural envelope differs')
        need(type(state) is dict and set(state) == {'entity', 'phase', 'diagnostics', 'sampled_tick', 'slots'} and state['entity'] == RIG and
             state['phase'] in ('animated', 'physical') and type(state['sampled_tick']) is int and type(state['slots']) is list and
             len(state['slots']) == 1 and type(state['slots'][0]) is int and state['slots'][0] == 1, 'Complete procedural status differs')
        if state['phase'] == 'animated': need((state['diagnostics'], state['sampled_tick']) == ('current', self.tick), 'Animated procedural diagnostic is not current')
        else: need(state['diagnostics'] == 'last_animated_pose' and state['sampled_tick'] < self.tick, 'Historical procedural diagnostic age/ownership was relabeled')
        return state

    def observe(self, label):
        physical = self.physical(); need(set(physical) == {'entity', 'mode', 'body_count', 'joint_count', 'parts', 'joints'} and
            physical['entity'] == RIG and type(physical['body_count']) is int and physical['body_count'] == 3 and type(physical['joint_count']) is int and physical['joint_count'] == 2 and
            len(physical['parts']) == 3 and len(physical['joints']) == 2, 'Complete three-part native rig differs')
        for index, part in enumerate(physical['parts']):
            need(set(part) == f.PART_FIELDS and part['name'] == ('root', 'middle', 'end')[index] and type(part['node']) is int and part['node'] == index and
                 part['node_entity'] == node(RIG, index) and type(part['body_id']) is int and 0 <= part['body_id'] <= 0xffffffff and
                 type(part['enrolled']) is bool and part['enrolled'] is (physical['mode'] == 'passive'), 'Physical part identity/enrollment/type differs')
            for key, size in (('position', 3), ('rotation', 4), ('linear_velocity', 3), ('angular_velocity', 3), ('inertia', 9)): vector(part[key], size)
            for key in ('mass', 'max_linear_velocity', 'max_angular_velocity', 'linear_damping', 'angular_damping', 'friction', 'restitution'):
                need(type(part[key]) is float and math.isfinite(part[key]), 'Physical part scalar type differs: '+key)
            for key in ('enrolled', 'active', 'sleeping', 'at_linear_velocity_limit', 'at_angular_velocity_limit'):
                need(type(part[key]) is bool, 'Physical part Boolean was coerced: '+key)
        for index, joint in enumerate(physical['joints']):
            need(set(joint) == f.JOINT_FIELDS and (joint['name'], joint['parent'], joint['child']) ==
                 ('joint'+str(index+1), ('root', 'middle')[index], ('middle', 'end')[index]) and type(joint['hinge']) is bool and joint['hinge'] is False,
                 'Complete physical joint DTO membership/frozen profile differs')
            for key, size in (('relative_rotation', 4), ('relative_angular_velocity', 3), ('evaluated_passive_child_torque', 3)): vector(joint[key], size)
            for key in f.JOINT_FIELDS-{'name', 'parent', 'child', 'hinge', 'relative_rotation', 'relative_angular_velocity', 'evaluated_passive_child_torque'}:
                need(type(joint[key]) is float and math.isfinite(joint[key]), 'Physical joint scalar type differs: '+key)
        entities = {identity: self.entity(identity) for identity in self.report['fixture']['entities']}
        state = dict(label=label, session_id=self.session, tick=self.tick, game=self.game(), ik=self.ik(), other_ik=self.ik(OTHER),
                     procedural=self.procedural(), handoff=self.handoff(), physical=physical, entities=entities)
        if physical['mode'] == 'passive':
            for part in physical['parts']:
                for a, b in zip(part['position'], entities[part['node_entity']]['world_matrix'][12:15]): near(a, b, 2e-6)
        root_scale = entities[node(RIG, 0)]['local_transform']['scale']; vector(root_scale, 3)
        scalar = struct.unpack('<f', struct.pack('<f', 1.000004))[0]
        for value in root_scale: near(value, 1+(scalar-1)*self.tick/120, 3e-15)
        self.report['observations'].append(copy.deepcopy(state)); return state

    def snapshot(self, saves, label):
        state, game = self.inspect(), self.game(); slot = 'procedural-'+str(len(self.report['archives'])+1)
        receipt = self.io.rpc('save.write', dict(request_id=fresh(), configuration_generation=1, slot=slot, expected_generation=0,
            session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=game['revision'], expected_structure_revision=state['structure_revision'],
            expected_material_revision=state['material_revision'], expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence']))
        manifest_path = saves/('slot-'+slot)/'current.json'; need(manifest_path.stat().st_size <= 1024*1024, 'Manifest exceeds1MiB')
        manifest = strict_json(manifest_path.read_bytes()); member = manifest['payload']['current']
        need(type(member['file']) is str and re.fullmatch(r'p-[0-9a-f]{32}\.bin', member['file']), 'Unsafe complete payload filename')
        path = manifest_path.parent/member['file']; raw = path.read_bytes()
        need(len(raw) <= 32*1024*1024 and type(receipt['bytes']) is int and type(member['bytes']) is int and len(raw) == receipt['bytes'] == member['bytes'] and
             sha(path) == receipt['sha256'] == member['sha256'], 'Complete save/manifest/receipt identities differ')
        document = strict_json(raw); payload = document['snapshot']['payload']
        need(document['format'] == 'poima.world-save' and type(document['version']) is int and document['version'] == 1 and payload['tick'] == self.tick and
             payload['gameplay']['backend'] == 'coreclr' and payload['gameplay']['assembly_sha256'] == self.report['assembly_sha256'] and payload['gameplay']['type'] == GAME and
             exact(typed(payload['gameplay']['values']), game['module']['values']), 'Complete typed save state/image/backend differs')
        need(payload['physical']['version'] == 3 and len(payload['physical']['rigs']) == 1, 'Composed extension3 ownership membership differs')
        observation = self.observe(label)
        row = dict(label=label, slot=slot, tick=self.tick, path=str(path), bytes=len(raw), sha256=sha(path), archive=document, receipt=receipt,
                   manifest_path=str(manifest_path), manifest=manifest, observation=observation)
        self.report['archives'].append(copy.deepcopy(row)); return row


def pair(report, label, a, b):
    need(Path(a['path']).read_bytes() == Path(b['path']).read_bytes() and exact(a['archive'], b['archive']), 'Complete native/gameplay archive comparison differs: '+label)
    report['whole_pairs'].append(dict(label=label, left=a['path'], right=b['path'], bytes=a['bytes'], sha256=a['sha256'], exclusions=[]))


def historical(values, expected):
    need(values['HistoricalInitialized'] == 1 and values['HistoricalReads'] > 0, 'Actual C# did not read/capture passive historical IK')
    for key, value in (('Enabled', int(expected['enabled'])), ('Space', 0), ('Clamped', int(expected['clamped'])), ('Fallback', int(expected['pole_fallback']))):
        need(type(values['Historical'+key]) is int and values['Historical'+key] == value, 'C# historical enum/flag differs')
    for prefix, member in (('Target', 'target'), ('Pole', 'pole'), ('End', 'end_position')):
        need(exact([values['Historical'+prefix+axis] for axis in 'XYZ'], expected[member]), 'C# frozen vector differs: '+prefix)
    need(exact(values['HistoricalWeight'], expected['weight']) and exact(values['HistoricalError'], expected['target_error']), 'C# historical point scalar differs')
    goal = expected['end_orientation']; need(goal is not None and values['HistoricalHasOrientation'] == 1, 'Expected oriented historical state absent')
    need(exact([values['HistoricalGoal'+axis] for axis in 'XYZW'], goal['rotation']) and
         exact([values['HistoricalActual'+axis] for axis in 'XYZW'], goal['actual_rotation']) and
         exact(values['HistoricalOrientationWeight'], goal['weight']) and exact(values['HistoricalAngularError'], goal['angular_error']),
         'C# complete historical orientation differs')


def verify_closed(report):
    need(len(report['owners']) == 3, 'Workflow must have exactly three sequential actual CLI owners')
    report['recordings'] = []
    for owner in report['owners']:
        need(owner['natural_exit'] is True and owner['forced_cleanup'] is False and owner['exit_code'] == 0 and owner['reader_failure'] is None,
             'Actual owner did not close naturally before literal verification')
        directory = Path(owner['directory']); calls = [row for row in report['calls'] if Path(row['owner']) == directory]
        for name, field in (('requests', 'request'), ('responses', 'response')):
            path = directory/(name+'.jsonl'); rows = [strict_json(line) for line in path.read_bytes().splitlines() if line.strip()]
            need(exact(rows, [row[field] for row in calls]), 'Closed literal RPC stream differs: '+name)
            report['recordings'].append(dict(path=str(path), bytes=path.stat().st_size, sha256=sha(path)))
    for row in report['archives']:
        path, manifest = Path(row['path']), Path(row['manifest_path'])
        need(path.stat().st_size == row['bytes'] and sha(path) == row['sha256'] and exact(strict_json(path.read_bytes()), row['archive']) and
             exact(strict_json(manifest.read_bytes()), row['manifest']), 'Closed complete save/manifest no longer matches its literal producer')
        observation = row['observation']; need(exact(typed(row['archive']['snapshot']['payload']['gameplay']['values']), observation['game']['module']['values']), 'Closed saved typed fields differ from original observation')
        need(any(call['request']['method'] == 'save.write' and call['request']['params'].get('slot') == row['slot'] and
            call['request']['params'].get('session_id') == observation['session_id'] and call['request']['params'].get('expected_tick') == row['tick'] and
            'result' in call['response'] and exact(call['response']['result'], row['receipt']) for call in report['calls']),
            'Closed archive receipt does not rejoin its literal save.write response')
    for row in report['whole_pairs']:
        need(Path(row['left']).read_bytes() == Path(row['right']).read_bytes() and sha(Path(row['left'])) == row['sha256'] and row['exclusions'] == [], 'Closed whole-byte pair differs')
    for observation in report['observations']:
        for method, key, entity, nested in (('runtime.gameplay.inspect', 'game', None, False), ('runtime.ik', 'ik', RIG, True),
            ('runtime.ik', 'other_ik', OTHER, True), ('runtime.procedural_pose', 'procedural', RIG, True),
            ('runtime.controller_handoff', 'handoff', RIG, True), ('runtime.physical_rig', 'physical', RIG, True)):
            candidates = [call['response']['result'] for call in report['calls'] if call['request']['method'] == method and
                call['request']['params'].get('session_id') == observation['session_id'] and call['request']['params'].get('tick') == observation['tick'] and
                (entity is None or call['request']['params'].get('entity') == entity) and 'result' in call['response']]
            need(any(exact(result['state'] if nested else result, observation[key]) for result in candidates), 'Observation does not rejoin a literal native reply: '+key)
        for identity, state in observation['entities'].items():
            need(any(call['request']['method'] == 'runtime.entity' and call['request']['params'] == dict(session_id=observation['session_id'], tick=observation['tick'], id=identity) and
                'result' in call['response'] and exact(call['response']['result'], state) for call in report['calls']), 'Complete entity observation lacks a literal reply')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'hostfxr', 'bridge', 'assembly', 'output'): parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true'); args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    f.ARGS, f.DEADLINE, f.MAX_CALLS = args, time.monotonic()+300, 1600
    report = dict(format='poima.procedural-handoff-coreclr.v1', passed=False, backend='coreclr', calls=[], owners=[], archives=[], observations=[], whole_pairs=[],
        source_pins={}, assembly_sha256=sha(args.assembly), limits=dict(seconds=300, rpc=1600, output_bytes=96*1024*1024, archive_bytes=32*1024*1024),
        limitations=['Actual supplied CoreCLR assembly and headless native engine; original analytic three-joint/three-proxy fixture.',
            'No imported-character, Native AOT, graphics, physical input, contact-quality or performance qualification.',
            'Three sequential naturally closed CLI owners; external process-tree owner required. Failures remain failures.'])
    f.REPORT = report; config = dict(hostfxr=f.native(args.hostfxr), bridge=f.native(args.bridge), assembly=f.native(args.assembly), type=GAME)
    files = [Path(__file__).resolve(), Path(f.__file__).resolve(), Path(prior.__file__).resolve(), ROOT/'tests/managed_physical_gameplay_contract.py',
        ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py', SAMPLE/'Game.cs', SAMPLE/'Poima.ManagedProceduralHandoffGame.csproj',
        ROOT/'managed/Directory.Build.props', args.binary.resolve(), args.hostfxr.resolve(), args.bridge.resolve(), args.assembly.resolve()]
    for image in (args.bridge.resolve(), args.assembly.resolve()):
        files.append(image.parent/'Poima.Gameplay.dll')
        for suffix in ('.deps.json', '.runtimeconfig.json'):
            if image.with_suffix(suffix).is_file(): files.append(image.with_suffix(suffix))
    io = c = None
    try:
        for path in set(files): need(path.is_file(), 'Pinned input absent: '+str(path)); report['source_pins'][str(path)] = sha(path)
        need(sha(args.bridge.parent/'Poima.Gameplay.dll') == sha(args.assembly.parent/'Poima.Gameplay.dll'), 'Bridge and consumer SDK images differ')
        io = f.Session(out/'warm'); mapping = configure(io, report); world = io.world
        saves = out/'saves'; saves.mkdir(); io.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=f.native(saves)))
        c = Consumer(io, report, config); io.rpc('runtime.start', dict(session_id=c.session, revision=2))
        io.rpc('runtime.gameplay.load', dict(session_id=c.session, request_id=fresh(), expected_tick=0, expected_revision=0, **config))
        c.edit(Root=mapping[RIG][0], Middle=mapping[RIG][1], End=mapping[RIG][2], OtherEnd=mapping[OTHER][2])
        c.step(inputs=[prior.input_for(ACTOR, (.35, .65), (2, -.25))]); c.snapshot(saves, 'alive-initial')
        c.edit(Mode=1); c.step(inputs=[prior.input_for(ACTOR, (.35, .65), (3, -.25))])
        need(c.ik()['end_orientation'] is None and c.values()['OwnWrites'] == 1, 'Actual basic C# setter did not clear orientation/publish controls')
        c.snapshot(saves, 'basic-setter-complete')
        c.edit(Mode=2); c.step(inputs=[prior.input_for(ACTOR, (.35, .65), (2, -.25))]); oriented = c.ik()['end_orientation']
        need(oriented is not None and oriented['weight'] == .65 and c.values()['OwnWrites'] == 2,
             'Actual oriented C# setter did not publish its explicit goal')
        # Independent Python/.NET trig and canonical unitization may differ at
        # the last bit. Literal native state/save comparisons remain exact.
        for actual, expected in zip(oriented['rotation'], (0.0, math.sin(.125), 0.0, math.cos(.125))): near(actual, expected, 2e-14)
        c.snapshot(saves, 'oriented-setter-complete'); c.step(); alive = c.snapshot(saves, 'alive-reference'); frozen_ik = copy.deepcopy(c.ik())
        need(alive['observation']['procedural']['phase'] == 'animated' and alive['observation']['entities'][ACTOR]['ground'] == 'in_air' and c.values()['CommittedYaw'] == 7.0,
             'Nonzero moving/turning airborne precondition absent')
        held = [prior.input_for(ACTOR, (-.75, .5), (5, 1), True)]
        for mode in (4, 5, 6):
            c.edit(Mode=mode); before = c.snapshot(saves, 'rejected-mode-'+str(mode)+'-before'); c.step(inputs=held, error=-32040)
            pair(report, 'whole callback conflict/rollback '+str(mode), before, c.snapshot(saves, 'rejected-mode-'+str(mode)+'-after')); c.restore(alive)
        c.edit(Mode=7); c.step(); need(c.values()['MalformedChecks'] == 1, 'Malformed actual managed setter was not rejected/caught')
        c.snapshot(saves, 'malformed-managed-setter-retained'); c.restore(alive)
        c.edit(Mode=3, FailAt=str(c.tick+1)); before = c.snapshot(saves, 'later-tick-death-before')
        c.step(2, held, error=-32040); pair(report, 'whole late-Tick IK/ownership/gameplay rollback', before, c.snapshot(saves, 'later-tick-death-after')); c.restore(alive)
        actor_before = c.entity(ACTOR); origin = actor_before['world_matrix'][12:15]
        capsule_ray = dict(origin=[origin[0]+2, origin[1]+1.1, origin[2]], direction=[-1, 0, 0], distance=4)
        hit = io.rpc('runtime.raycast', dict(session_id=c.session, tick=c.tick, **capsule_ray))['hit']; need(hit is not None and hit['entity'] == ACTOR, 'Actual alive capsule ray witness absent')

        def takeover():
            c.edit(Mode=3); tick = c.tick; c.step(inputs=held, character_stances=[dict(entity=ACTOR, crouch=True)])
            phase, handoff, values, ik = c.procedural(), c.handoff(), c.values(), c.ik()
            need(phase['phase'] == 'physical' and phase['sampled_tick'] == tick and handoff['phase'] == 'physical' and handoff['takeover_tick'] == tick and
                 values['ObservedMode'] == 0 and values['ObservedYaw'] == 12.0 and values['CommittedYaw'] == 7.0 and values['Dead'] == 1 and values['DeathTick'] == str(tick),
                 'Actual compiled activation/planned preview/frozen ownership differs')
            need(exact(ik, frozen_ik), 'Activation substituted a callback-preview solve for last committed complete IK')
            near(c.entity(ACTOR)['yaw'], 7.0, 0); near(c.entity(ACTOR)['pitch'], -.75, 0)
            return tick

        takeover_tick = takeover(); need(io.rpc('runtime.raycast', dict(session_id=c.session, tick=c.tick, **capsule_ray))['hit'] is None, 'Suppressed capsule remains a ghost collider')
        center = c.physical()['parts'][0]['position']; hit = io.rpc('runtime.raycast', dict(session_id=c.session, tick=c.tick, origin=[center[0]+2, center[1], center[2]], direction=[-1, 0, 0], distance=4))['hit']
        need(hit is not None and hit['entity'] == RIG, 'Enrolled physical part ray witness absent')
        passive = c.snapshot(saves, 'passive-reference')
        c.edit(Mode=8); before = c.snapshot(saves, 'passive-write-before'); c.step(error=-32040)
        pair(report, 'whole passive IK write rejection', before, c.snapshot(saves, 'passive-write-after')); c.restore(passive)

        def continuation(ticks):
            c.step(ticks, [prior.input_for(ACTOR, (1, -1), (37, -17), True)], character_stances=[dict(entity=ACTOR, crouch=True)])
            need(exact(c.ik(), frozen_ik) and c.procedural()['sampled_tick'] == takeover_tick and c.values()['OtherWrites'] == c.values()['Ticks'],
                 'Passive override stopped unrelated IK or changed frozen diagnostics')
            historical(c.values(), frozen_ik); near(c.entity(ACTOR)['yaw'], 7.0, 0); near(c.entity(ACTOR)['pitch'], -.75, 0)

        continuation(4); future = c.snapshot(saves, 'warm-future'); continuation(2); late = c.snapshot(saves, 'warm-late')
        c.restore(passive); pair(report, 'warm passive whole roundtrip', passive, c.snapshot(saves, 'warm-passive-roundtrip'))
        continuation(4); pair(report, 'warm restored whole future', future, c.snapshot(saves, 'warm-restored-future'))
        continuation(2); pair(report, 'warm restored whole late', late, c.snapshot(saves, 'warm-restored-late'))
        io.close(); io = None
        io = f.Session(out/'cold', world=world); c = Consumer(io, report, config)
        io.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=f.native(saves)))
        c.restore(passive, existing=False); pair(report, 'cold passive whole roundtrip', passive, c.snapshot(saves, 'cold-passive-roundtrip'))
        continuation(4); pair(report, 'cold passive whole future', future, c.snapshot(saves, 'cold-future'))
        continuation(2); pair(report, 'cold passive whole late', late, c.snapshot(saves, 'cold-late'))
        c.restore(alive); pair(report, 'cold alive whole roundtrip', alive, c.snapshot(saves, 'cold-alive-roundtrip'))
        takeover(); pair(report, 'cold alive whole takeover', passive, c.snapshot(saves, 'cold-alive-takeover'))
        continuation(4); pair(report, 'cold alive whole future', future, c.snapshot(saves, 'cold-alive-future'))
        continuation(2); pair(report, 'cold alive whole late', late, c.snapshot(saves, 'cold-alive-late'))
        io.close(); io = None
        io = f.Session(out/'cold-historical', world=world); c = Consumer(io, report, config)
        io.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=f.native(saves)))
        c.restore(future, existing=False); pair(report, 'cold initialized-history whole roundtrip', future, c.snapshot(saves, 'cold-historical-roundtrip'))
        continuation(2); pair(report, 'cold initialized-history whole late', late, c.snapshot(saves, 'cold-historical-late'))
        io.close(); io = None; verify_closed(report)
        need(all(sha(Path(path)) == digest for path, digest in report['source_pins'].items()) and sha(world) == report['fixture']['authored_sha256'] and
             sha(Path(report['fixture']['package'])) == report['fixture']['package_sha256'] and sha(Path(report['fixture']['source'])) == report['fixture']['source_sha256'],
             'Pinned source/image/authored/cooked content changed')
        report['checks'] = ['Actual CoreCLR point/oriented getters and setters; complete generated typed field layout/state',
            'Independent planned heading/pitch/camera and three-node FK/IK/orientation joins, staged write invisibility',
            'Atomic callback, same-Tick activation+IK, duplicate, late-Tick and passive write rejection with complete rollback',
            'Historical manual IK survives physical takeover while independent C# IK keeps updating',
            'Whole native+typed-gameplay saves, warm/cold alive/passive/historical continuation; literal postclosure transcript/archive joins']
        report['passed'] = True
    except BaseException as error:
        report['error'] = repr(error); raise
    finally:
        if io is not None:
            try: io.close()
            except BaseException as error: report['passed'] = False; report['close_error'] = repr(error)
        report['rpc_count'] = len(report['calls']); report['natural_cli_owners'] = sum(row.get('natural_exit') is True and row.get('exit_code') == 0 and not row['forced_cleanup'] for row in report['owners'])
        raw = (json.dumps(report, indent=2, allow_nan=False)+'\n').encode()
        need(len(raw) <= 32*1024*1024 and sum(path.stat().st_size for path in out.rglob('*') if path.is_file())+len(raw) <= 96*1024*1024, 'Bounded consumer output exceeded')
        (out/'evidence.json').write_bytes(raw); print(out/'evidence.json', flush=True)
        if not report['passed'] and 'error' not in report: raise RuntimeError(report.get('close_error', 'Consumer did not complete'))


if __name__ == '__main__':
    main()
