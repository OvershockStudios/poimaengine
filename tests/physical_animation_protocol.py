#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native CLI physical-animation targets: closed protocol, rollback and cold saves.

Uses the existing analytic three-part fixture, adding deterministic rotation
tracks. Builds nothing and opens no renderer or audio device. An external
process-tree owner must supervise execution. Literal wires, full native saves
and original failures are retained; this is not a character-quality benchmark.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid

sys.dont_write_bytecode = True
import physical_drive_gameplay as d
import world_physical_contract as f
from gltf_fixture import glb
from managed_physical_gameplay_contract import sha, strict_json

ROOT = Path(__file__).resolve().parents[1]
RIG, FLOOR = d.RIG, d.FLOOR
SECONDS, CALLS, MIB = 180, 500, 1024**2
need, exact = f.need, f.exact


def uid():
    return uuid.uuid4().hex


def model():
    """Preserve the existing geometry/bind recipe and append three rotation keys."""
    original = d.analytic_model()
    size, kind = struct.unpack_from('<II', original, 12)
    need(kind == 0x4e4f534a, 'Original analytic GLB JSON chunk absent')
    document = strict_json(original[20:20+size])
    binary_size, binary_kind = struct.unpack_from('<II', original, 20+size)
    need(binary_kind == 0x004e4942, 'Original analytic GLB binary chunk absent')
    binary = bytearray(original[28+size:28+size+binary_size])
    def accessor(rows, kind):
        while len(binary) % 4: binary.append(0)
        start = len(binary)
        binary.extend(struct.pack('<'+'f'*sum(len(row) for row in rows), *(v for row in rows for v in row)))
        document['bufferViews'].append(dict(buffer=0, byteOffset=start, byteLength=len(binary)-start))
        document['accessors'].append(dict(bufferView=len(document['bufferViews'])-1, componentType=5126, count=len(rows), type=kind))
        return len(document['accessors'])-1
    times = accessor([(0.,), (1.,), (2.,)], 'SCALAR')
    document['accessors'][times].update(min=[0.], max=[2.])
    legal = accessor([(math.sin(a/2), 0., 0., math.cos(a/2)) for a in (0., .4, .1)], 'VEC4')
    # Y(.5) * X(1.6): deliberately outside the elbow's hinge and upper limit,
    # while remaining in the declared nonsingular fitting chart.
    x, y = .8, .25
    over = (math.cos(y)*math.sin(x), math.sin(y)*math.cos(x), -math.sin(y)*math.sin(x), math.cos(y)*math.cos(x))
    fitted = accessor([over]*3, 'VEC4')
    for name, output, node in [('bounded shoulder rotation', legal, 1), ('explicit elbow fit', fitted, 2)]:
        document['animations'].append(dict(name=name, samplers=[dict(input=times, output=output, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(node=node, path='rotation'))]))
    document['buffers'][0]['byteLength'] = len(binary)
    document['asset']['generator'] = 'Poima deterministic physical animation protocol recipe'
    return glb(document, bytes(binary))


def prepare(io, velocity_tracking=False):
    source = io.directory/'original-analytic-targets.glb'; source.write_bytes(model())
    imported = io.rpc('asset.import', dict(source=f.native(source)))
    need((imported['nodes'], imported['animations'], imported['skins'], imported['primitives']) == (4, 3, 1, 1), 'Complete analytic target import membership')
    asset = imported['asset']; need(type(asset) is str and len(asset) == 64, 'Cooked target content address')
    profile = d.profile(); profile['animation_drive'] = dict(limit_policy='fit')
    if velocity_tracking: profile['animation_drive']['velocity_tracking'] = True
    layer = dict(slot=1, mode='override', clip=0, time=0., speed=1., loop=False, playing=False,
                 weight=0., mask=[dict(node=2, weight=1.)])
    operations = [dict(op='asset.instantiate', id=RIG, name='Physical animation protocol', asset=asset),
        f.component(RIG, 'Transform', dict(**f.pose((0, 200, 0)), scale=[1, 1, 1])),
        f.component(RIG, 'AnimationRig', dict(asset=asset, clip=0, time=0., speed=1., loop=False, playing=True, layers=[layer])),
        dict(op='entity.create', id=FLOOR, name='Unowned static floor'),
        f.component(FLOOR, 'Transform', dict(**f.pose((0, -.5, 0)), scale=[1, 1, 1])),
        f.component(FLOOR, 'BoxCollider', dict(half_extents=[20, .5, 20], motion='static', mass=1, friction=.5, restitution=0))]
    need(f.transact(io, operations, 0)['revision'] == 1, 'Complete model authoring')
    need(f.transact(io, [f.component(RIG, 'PhysicalRig', profile)], 1)['revision'] == 2, 'Opted-in target profile authoring')
    package = Path(str(io.world)+'.assets')/(asset+'.pmodel')
    need(package.is_file() and sha(package) == asset, 'Complete cooked model identity')
    return dict(source=str(source), source_sha256=sha(source), package=str(package), package_sha256=asset, profile=profile)


def discovery(io):
    full = io.rpc('world.describe', dict(view='full'))
    need(full['schema_revision'] == 130, 'Physical animation schema revision')
    for method in ('runtime.physical_animation', 'runtime.step'):
        focused = io.rpc('world.describe', dict(view='method', name=method))
        need(focused['partial'] is True and focused['view'] == 'method' and focused['schema_revision'] == 130 and
             exact(focused['methods'], {method: full['methods'][method]}), 'Focused method differs from complete discovery')
    query = full['methods']['runtime.physical_animation']
    need(query['additionalProperties'] is False and set(query['properties']) == {'session_id', 'tick', 'entity'} and
         set(query['required']) == {'session_id', 'tick', 'entity'}, 'Closed target read schema')
    step = full['methods']['runtime.step']['properties']; item = step['physical_rigs']['items']
    need(step['physical_rigs']['maxItems'] == 64 and item['additionalProperties'] is False and
         set(item['properties']) == {'entity', 'activate', 'impulse', 'drive', 'drive_source', 'animation', 'animation_rate'}, 'Closed physical command membership/budget')
    expected = copy.deepcopy(step['animations']['items'])
    for row in [expected, *expected['oneOf']]:
        del row['properties']['entity']; row['required'].remove('entity')
    need(exact(expected, item['properties']['animation']), 'Target playback schema diverges from ordinary animation rules')
    expected = copy.deepcopy(step['animation_rates']['items']); del expected['properties']['entity']; expected['required'].remove('entity')
    need(exact(expected, item['properties']['animation_rate']), 'Target rate schema diverges from ordinary animation rules')
    source = item['properties']['drive_source']
    need(source['additionalProperties'] is False and set(source['required']) == {'joint', 'source', 'strength'} and
         set(source['properties']) == {'joint', 'source', 'strength'} and source['properties']['source']['enum'] == ['manual', 'animation'], 'Closed source-selection schema')
    component = io.rpc('world.describe', dict(view='component', name='PhysicalRig'))
    need(exact(component['components'], {'PhysicalRig': full['components']['PhysicalRig']}) and
         'animation_drive' in component['components']['PhysicalRig']['properties'], 'Focused opt-in component discovery')
    authoring = component['components']['PhysicalRig']['properties']['animation_drive']
    need(authoring['additionalProperties'] is False and set(authoring['required']) == {'limit_policy'} and
         set(authoring['properties']) == {'limit_policy', 'velocity_tracking'} and
         authoring['properties']['velocity_tracking']['type'] == 'boolean', 'Explicit optional Boolean velocity permission')


def playback(clip=1, **changes):
    return dict(dict(clip=clip, time=0., speed=1., loop=False, playing=True), **changes)


def source(joint, selected='animation', strength=.6):
    return dict(entity=RIG, drive_source=dict(joint=joint, source=selected, strength=strength))


def velocity_projection(state):
    need(set(state) == {'entity','mode','body_count','joint_count','parts','joints'} and state['entity'] == RIG and
         state['mode'] in ('animated','passive','driven') and type(state['body_count']) is int and state['body_count'] == 3 and
         type(state['joint_count']) is int and state['joint_count'] == 2 and len(state['parts']) == 3 and len(state['joints']) == 2,
         'Complete physical projection mode/membership')
    for index,(p,name,mass) in enumerate(zip(state['parts'],('pelvis','arm','hand'),(4.,2.,1.))):
        need(set(p) == f.PART_FIELDS and p['name'] == name and type(p['node']) is int and p['node'] == index and
             p['node_entity'] == d.node(index) and type(p['body_id']) is int and 0 <= p['body_id'] <= 0x7fffffff,
             'Actual part/source-node/native-body join')
        for field,count in [('position',3),('rotation',4),('linear_velocity',3),('angular_velocity',3),('inertia',9)]:
            need(type(p[field]) is list and len(p[field]) == count and all(type(x) is float and math.isfinite(x) for x in p[field]), 'Part typed vector: '+field)
        for field in ('mass','max_linear_velocity','max_angular_velocity','linear_damping','angular_damping','friction','restitution'):
            need(type(p[field]) is float and math.isfinite(p[field]), 'Part typed number: '+field)
        need(p['mass'] == mass, 'Exact analytic powers-of-two mass')
        for field in ('enrolled','active','sleeping','at_linear_velocity_limit','at_angular_velocity_limit'):
            need(type(p[field]) is bool, 'Part flag coerced: '+field)
        need(p['enrolled'] == (state['mode'] != 'animated') and p['sleeping'] == (p['enrolled'] and not p['active']), 'Physical enrollment/sleep semantics')
    need(len({p['body_id'] for p in state['parts']}) == 3,'Physical parts alias native identities')
    for index,j in enumerate(state['joints']):
        need(set(j) == f.JOINT_FIELDS|{'drive'} and (j['name'],j['parent'],j['child'],j['hinge']) ==
             (('shoulder','pelvis','arm',False) if index == 0 else ('elbow','arm','hand',True)), 'Complete authored joint membership')
        need(type(j['hinge']) is bool,'Joint hinge flag coerced')
        vectors={'relative_rotation':4,'relative_angular_velocity':3,'evaluated_passive_child_torque':3}
        for key,count in vectors.items():
            need(type(j[key]) is list and len(j[key])==count and all(type(x) is float and math.isfinite(x) for x in j[key]),'Complete joint typed vector: '+key)
        for key in f.JOINT_FIELDS-{'name','parent','child','hinge'}-set(vectors):
            need(type(j[key]) is float and math.isfinite(j[key]),'Complete joint typed number: '+key)
        drive = j['drive']
        expected = {'target_rotation','strength','stiffness','damping','max_torque','evaluated_child_torque','angular_error','saturation_scale'}
        if 'target_angular_velocity' in drive:
            expected.add('target_angular_velocity')
            need(type(drive['target_angular_velocity']) is list and len(drive['target_angular_velocity']) == 3 and
                 all(type(x) is float and math.isfinite(x) for x in drive['target_angular_velocity']), 'Complete typed native target velocity')
        need(set(drive) == expected, 'Complete drive diagnostics with the sole optional velocity extension')
        for key,count in [('target_rotation',4),('evaluated_child_torque',3)]:
            need(type(drive[key]) is list and len(drive[key]) == count and all(type(x) is float and math.isfinite(x) for x in drive[key]), 'Drive typed vector')
        for key in ('strength','stiffness','damping','max_torque','angular_error','saturation_scale'):
            need(type(drive[key]) is float and math.isfinite(drive[key]), 'Drive typed number')
        need((drive['stiffness'],drive['damping'],drive['max_torque']) == (60.,6.,10.) and 0 <= drive['strength'] <= 1 and
             0 <= drive['saturation_scale'] <= 1 and math.hypot(*drive['evaluated_child_torque']) <= drive['strength']*10+1e-10, 'Authored gain/strength/torque bounds')
        if drive['strength'] == 0: need(exact(drive['evaluated_child_torque'],[0.0]*3), 'Zero drive produced active torque')
    return state


def motion_contract(motion, endpoint):
    """Independent principal-rotation secant, without changing retained values."""
    need(type(motion) is dict and set(motion) == {'start_rotation', 'angular_velocity'}, 'Closed fitted-interval motion observation')
    start, velocity = motion['start_rotation'], motion['angular_velocity']
    need(type(start) is list and len(start) == 4 and all(type(x) is float and math.isfinite(x) for x in start) and
         abs(sum(x*x for x in start)-1) <= 2e-12, 'Typed unit interval start')
    need(type(velocity) is list and len(velocity) == 3 and all(type(x) is float and math.isfinite(x) for x in velocity), 'Typed finite interval velocity')
    # end * inverse(start), in the parent joint frame; one simulation tick.
    x,y,z,w = endpoint; a,b,c,s = (-start[0], -start[1], -start[2], start[3])
    delta = [w*a+x*s+y*c-z*b, w*b-x*c+y*s+z*a, w*c+x*b-y*a+z*s, w*s-x*a-y*b-z*c]
    if delta[3] < 0: delta = [-v for v in delta]
    sine = math.hypot(*delta[:3]); angle = 2*math.atan2(sine, delta[3])
    expected = [v*angle*60/sine for v in delta[:3]] if sine else [0.0]*3
    need(max(abs(a-b) for a,b in zip(expected, velocity)) <= 1e-9, 'Reported angular velocity differs from fitted interval endpoints')


def analytic_shoulder(c, state, speed):
    """The fixture's first segment rotates around X by0.4 radians per second."""
    p = state['playback']; joint = state['joints'][0]; end = p['time']; start = end-speed/60
    # The sole authored layer masks node2 only; shoulder node1 is unaffected.
    need(p['clip'] == 1 and p['speed'] == speed and p['transition'] is None and
         0 < start < end < 1, 'Independent constant-rate curve interval')
    for observed, t in ((joint['motion']['start_rotation'], start), (joint['sample']['target_rotation'], end)):
        expected = [math.sin(.4*t/2), 0.0, 0.0, math.cos(.4*t/2)]
        # Source keys are explicitly float32, so compare to the authored
        # analytic curve with its quantization error rather than rewriting them.
        need(max(abs(a-b) for a,b in zip(observed, expected)) <= 1e-7, 'Fitted target does not follow the independently authored X curve')
    expected = [.4*speed, 0.0, 0.0]
    need(max(abs(a-b) for a,b in zip(joint['motion']['angular_velocity'], expected)) <= 2e-6,
         'Actual interval velocity does not match independent curve/tick/rate')
    c.report['velocity_checks'].append(dict(tick=c.tick, label='analytic shoulder curve', start_time=start, end_time=end,
        expected_radians_per_second=expected, actual=copy.deepcopy(joint['motion'])))


class Consumer:
    def __init__(self, io, report, saves):
        self.io, self.report, self.saves, self.session, self.tick = io, report, saves, uid(), 0
        self.velocity_tracking = report.get('velocity_tracking', False)

    def query(self, method='runtime.physical_animation', entity=RIG, **flags):
        value = self.io.rpc(method, dict(session_id=self.session, tick=self.tick, entity=entity, **flags))
        need(set(value) == {'session_id', 'tick', 'source', 'state'} and value['session_id'] == self.session and
             type(value['tick']) is int and value['tick'] == self.tick and value['source'] == 'runtime', 'Exact committed observation receipt')
        return value['state']

    def physical(self):
        state = self.query('runtime.physical_rig', include_parts=True, include_joints=True)
        return velocity_projection(state) if self.velocity_tracking else d.projection(state)

    def observe(self, label):
        state = self.query(); physical = self.physical()
        fields = {'entity', 'sampled_tick', 'limit_policy', 'playback', 'joints'} | ({'velocity_tracking'} if self.velocity_tracking else set())
        need(set(state) == fields and state['entity'] == RIG and
             type(state['sampled_tick']) is int and state['sampled_tick'] == self.tick and state['limit_policy'] == 'fit', 'Complete physical-animation observation')
        if self.velocity_tracking: need(state['velocity_tracking'] is True, 'Explicit observed velocity permission')
        p = state['playback']
        need(set(p) == {'clip', 'time', 'speed', 'loop', 'playing', 'duration', 'transition', 'layers'} and
             len(p['layers']) == 1 and p['layers'][0]['slot'] == 1, 'Complete base/layer observation shape')
        need([j['name'] for j in state['joints']] == ['shoulder', 'elbow'], 'Complete configured drive order')
        for joint, native in zip(state['joints'], physical['joints']):
            need(set(joint) == {'name', 'source', 'sample'} | ({'motion'} if self.velocity_tracking else set()) and
                 joint['source'] in ('manual', 'animation'), 'Exact source observation fields')
            sample = joint['sample']
            if joint['source'] == 'manual':
                need(sample is None, 'Manual source retained misleading current animation attribution')
                if self.velocity_tracking: need(joint['motion'] is None and 'target_angular_velocity' not in native['drive'], 'Manual hold retained automatic velocity ownership')
                continue
            need(type(sample) is dict and set(sample) == {'requested_rotation', 'target_rotation', 'correction_radians', 'chart_condition',
                 'hinge_projected', 'twist_clamped', 'swing_clamped'}, 'Complete fit diagnostics')
            for key in ('requested_rotation', 'target_rotation'):
                need(type(sample[key]) is list and len(sample[key]) == 4 and all(type(x) is float and math.isfinite(x) for x in sample[key]) and
                     abs(sum(x*x for x in sample[key])-1) <= 2e-12, 'Typed canonical sampled quaternion')
            for key in ('correction_radians', 'chart_condition'):
                need(type(sample[key]) is float and math.isfinite(sample[key]) and sample[key] >= 0, 'Typed finite fit metric')
            for key in ('hinge_projected', 'twist_clamped', 'swing_clamped'): need(type(sample[key]) is bool, 'Fit flag coerced')
            # The backend may perform its existing admission canonicalization;
            # compare orientations using its documented unit-input tolerance.
            dot = abs(sum(x*y for x, y in zip(sample['target_rotation'], native['drive']['target_rotation'])))
            need(abs(dot-1) <= 2e-12, 'Committed fitted target differs from effective native orientation')
            if self.velocity_tracking:
                motion_contract(joint['motion'], sample['target_rotation'])
                need(exact(joint['motion']['angular_velocity'], native['drive']['target_angular_velocity']), 'Every committed target velocity joins the actual backend exactly')
        self.report['observations'].append(dict(label=label, tick=self.tick, target=copy.deepcopy(state), physical=copy.deepcopy(physical)))
        return state

    def step(self, commands=(), ticks=1, error=None, request=None, expected_tick=None):
        result = self.io.rpc('runtime.step', dict(session_id=self.session, request_id=request or uid(),
            expected_tick=self.tick if expected_tick is None else expected_tick, ticks=ticks, physical_rigs=list(commands)), error)
        if error is None and not result['replayed']:
            need(type(result['tick']) is int and result['tick'] == self.tick+ticks, 'Native step clock'); self.tick = result['tick']
        return result

    def archive(self, label):
        slot = 'target-'+str(len(self.report['archives'])+1)
        value = f.archive(self.io, self.saves, self.tick, slot, self.session); row = self.report['archives'][-1]
        raw = Path(row['path']).read_bytes(); strict_json(raw)
        payload = d.original_member(d.original_member(raw, 'snapshot'), 'payload')
        need(hashlib.sha256(payload.encode()).hexdigest() == value['snapshot']['sha256'], 'Original unreformatted snapshot checksum')
        physical = value['snapshot']['payload']['physical']
        need(physical['version'] == (7 if self.velocity_tracking else 6) and len(physical['rigs']) == 1 and physical['rigs'][0]['entity'] == RIG, 'Complete authored physical extension version')
        target = physical['rigs'][0]['animation']; player = target['player']
        need(target['version'] == (2 if self.velocity_tracking else 1), 'Exact authored target save version')
        need(target['tick'] == self.tick and player['format'] == 'poima.animation-target-state' and player['version'] == 1 and
             player['tick'] == self.tick and player['entity'] == RIG and player['history']['current']['tick'] == self.tick and
             len(player['layer_history']) == 1 and player['layer_history'][0]['current']['tick'] == self.tick, 'Complete independently sampled target histories')
        controls = physical['rigs'][0]['membership']['drives']
        need([j['joint'] for j in target['joints']] == [j['joint'] for j in controls] == ['shoulder', 'elbow'], 'Complete saved target and backend membership')
        for joint, control in zip(target['joints'], controls):
            need(set(joint) == {'joint', 'source', 'sample'} | ({'motion'} if self.velocity_tracking else set()), 'Exact saved target fields')
            selected = self.velocity_tracking and joint['source'] == 'animation'
            need(set(control) == {'joint', 'target_rotation', 'strength'} | ({'target_angular_velocity'} if selected else set()), 'Exact saved native velocity ownership')
            if selected:
                motion_contract(joint['motion'], joint['sample']['target'])
                need(exact(joint['motion']['angular_velocity'], control['target_angular_velocity']), 'Complete saved backend and interval vector join')
            elif self.velocity_tracking: need(joint['motion'] is None, 'Saved manual joint retained a motion interval')
        row.update(label=label, archive=value, manifest=str(self.saves/('slot-'+slot)/'current.json'))
        return copy.deepcopy(row)

    def load(self, row, existing=True):
        requested = uid()
        result = self.io.rpc('save.load', dict(request_id=uid(), configuration_generation=1, slot=row['slot'], expected_generation=1,
            revision=2, expected_session_id=self.session if existing else None, expected_tick=self.tick if existing else None,
            expected_gameplay_revision=0 if existing else None, new_session_id=requested))
        need(result.get('session_id', result.get('new_session_id')) == requested and type(result['tick']) is int and result['tick'] == row['tick'], 'Cold/warm restored owner and tick')
        self.session, self.tick = requested, result['tick']


def pair(report, label, left, right):
    equal = Path(left['path']).read_bytes() == Path(right['path']).read_bytes() and exact(left['archive'], right['archive'])
    report['whole_pairs'].append(dict(label=label, left=left['path'], right=right['path'], complete_bytes_and_typed_equal=equal, excluded_fields=[]))
    need(equal, 'Complete original archives differ: '+label)


def rejection(c, label, commands, code):
    before = c.archive(label+' before'); c.step(commands, error=code)
    pair(c.report, label, before, c.archive(label+' after'))


def future(c, label):
    c.step([dict(entity=RIG, animation_rate=dict(speed=1.25))], ticks=3)
    c.step([dict(entity=RIG, animation=playback(time=.75, blend_ticks=5, transition_mode='inertial'))], ticks=2)
    c.step(ticks=4); c.observe(label)
    return c.archive(label+' complete')


def released_future(c, label):
    c.step([dict(entity=RIG, animation=playback(0, playing=False))], ticks=3)
    need(c.physical()['mode'] == 'passive', 'Released targets reactivated physical drives')
    c.observe(label); return c.archive(label+' complete')


def velocity_cases(c):
    origin = c.archive('velocity cases complete origin')
    c.step([source('shoulder', strength=0.), source('elbow', 'manual', 0.),
            dict(entity=RIG, animation=playback(0, time=0., playing=False))])
    still = c.observe('instant clip replacement has no inherited velocity spike')
    need(exact(still['joints'][0]['motion']['angular_velocity'], [0.0]*3) and
         exact(still['joints'][0]['motion']['start_rotation'], still['joints'][0]['sample']['target_rotation']),
         'Stationary replacement differentiated the unrelated previously committed clip')
    c.step([dict(entity=RIG, animation=playback(time=.25, speed=.5))])
    zero = c.observe('zero strength still samples moving target velocity'); analytic_shoulder(c, zero, .5)
    native = c.physical()
    need(native['mode'] == 'passive' and all(j['drive']['strength'] == 0. and exact(j['drive']['evaluated_child_torque'], [0.0]*3)
         for j in native['joints']), 'Zero-strength moving targets produced active drive torque')
    saved = c.archive('zero-strength automatic velocity checkpoint')
    c.step(ticks=2); expected = c.archive('zero-strength automatic future')
    c.load(saved); pair(c.report, 'whole zero-strength automatic restore', saved, c.archive('zero-strength restored'))
    c.step(ticks=2); pair(c.report, 'whole zero-strength automatic continuation', expected, c.archive('zero-strength repeated future'))
    c.step([dict(entity=RIG, animation_rate=dict(speed=0.))]); stopped = c.observe('zero rate keeps automatic ownership with zero velocity')
    need(exact(stopped['joints'][0]['motion']['angular_velocity'], [0.0]*3), 'Stopped playback retained target angular velocity')
    held = c.physical()['joints'][0]['drive']['target_rotation']
    c.step([source('shoulder', 'manual', 0.)]); c.observe('manual source clears only automatic velocity ownership')
    need(exact(c.physical()['joints'][0]['drive']['target_rotation'], held), 'Manual source changed held orientation')
    c.step([source('shoulder', strength=.6), dict(entity=RIG, animation=playback(time=.25, speed=2.))])
    resumed = c.observe('explicit source and rate restart use post-command interval'); analytic_shoulder(c, resumed, 2.)
    c.load(origin); pair(c.report, 'velocity branch restores complete original continuation state', origin, c.archive('velocity cases origin restored'))
    c.report['velocity_checks'].append(dict(label='replacement, zero strength, zero rate and manual source', tick=c.tick,
        zero_strength_and_zero_rate_verified=True, original_complete_state_restored_after_branch=True))


def exercise(c):
    started = c.io.rpc('runtime.start', dict(session_id=c.session, revision=2))
    need(started['started'] is True and started['tick'] == 0, 'Initial native target session')
    initial = c.observe('initial independent target'); need(all(j['source'] == 'manual' for j in initial['joints']), 'Opt-in profile implicitly selected active sources')
    before = c.archive('before read guards'); base = dict(session_id=c.session, tick=0, entity=RIG)
    for update, code in [(dict(tick=1), -32009), (dict(tick=True), -32602), (dict(session_id=uid()), -32030),
                         (dict(entity=uid()), -32004), (dict(extra=True), -32602), (dict(include_joints=True), -32602)]:
        c.io.rpc('runtime.physical_animation', dict(base, **update), code)
    need(c.query(entity=FLOOR) is None, 'Unconfigured physical-animation query must return null')
    pair(c.report, 'read guards and null projection preserve whole state', before, c.archive('after read guards'))
    rejection(c, 'target control before physical ownership', [dict(entity=RIG, animation=playback())], -32040)
    commands = [dict(source('shoulder'), activate=True, animation=playback(time=.25)), source('elbow')]
    request, old_tick = uid(), c.tick; accepted = c.step(commands, request=request)
    state = c.observe('activated and streamed legal targets')
    if c.velocity_tracking: analytic_shoulder(c, state, 1.)
    before = c.archive('before retry')
    need(exact(c.step(commands, request=request, expected_tick=old_tick), dict(accepted, replayed=True)), 'Exact physical target retry receipt')
    c.step([dict(source('shoulder'), activate=True)], request=request, expected_tick=old_tick, error=-32010)
    pair(c.report, 'target retry and mismatched retry preserve whole state', before, c.archive('after retry'))
    c.step(ticks=5); state = c.observe('advancing shoulder target')
    if c.velocity_tracking: analytic_shoulder(c, state, 1.)
    need(state['playback']['clip'] == 1 and state['joints'][0]['sample']['requested_rotation'][0] > .01, 'Target rotation did not follow original curve')
    need(any(math.hypot(*part['angular_velocity']) > 1e-5 for part in c.physical()['parts']), 'Target controls produced no native angular response')
    ordinary = c.io.rpc('runtime.entity', dict(session_id=c.session, tick=c.tick, id=RIG))
    need(ordinary['animation']['clip'] == 0, 'Physical target playback replaced displayed/source animation clock')
    layer = playback(2, playing=False, layer=1, weight=1., weight_blend_ticks=6)
    c.step([dict(entity=RIG, animation_rate=dict(speed=.5)), dict(entity=RIG, animation=layer)], ticks=6)
    fitted = c.observe('explicit layer and limit fitting'); fit = fitted['joints'][1]['sample']
    need(fitted['playback']['speed'] == .5 and fitted['playback']['layers'][0]['weight'] == 1. and fit['hinge_projected'] is True and
         fit['twist_clamped'] is True and fit['correction_radians'] > .1, 'Explicit fitting/rate/layer path was not exercised')
    invalid = [
        ('nested entity', dict(entity=RIG, animation=playback(entity=FLOOR)), -32602),
        ('Boolean rate', dict(entity=RIG, animation_rate=dict(speed=True)), -32602),
        ('fractional layer', dict(entity=RIG, animation_rate=dict(speed=1., layer=1.0)), -32602),
        ('base weight', dict(entity=RIG, animation=playback(weight=.5)), -32602),
        ('layer without weight', dict(entity=RIG, animation=playback(layer=1)), -32602),
        ('Boolean source strength', source('shoulder', strength=True), -32602),
        ('unknown source', source('shoulder', 'automatic'), -32602),
        ('extra source key', dict(entity=RIG, drive_source=dict(joint='shoulder', source='animation', strength=.6, extra=0)), -32602),
        ('unknown source joint', source('missing'), -32040),
        ('unavailable clip', dict(entity=RIG, animation=playback(255)), -32040),
    ]
    for label, command, code in invalid: rejection(c, label, [command], code)
    rejection(c, 'duplicate playback and rate', [dict(entity=RIG, animation=playback()), dict(entity=RIG, animation_rate=dict(speed=1.))], -32040)
    rejection(c, 'duplicate source and manual drive', [source('shoulder'), d.drive('shoulder', .2, .5)], -32040)
    previous = c.physical()['joints'][1]['drive']['target_rotation']
    c.step([source('elbow', 'manual', .2)]); manual = c.observe('manual keeps effective target')
    need(manual['joints'][1]['sample'] is None and exact(previous, c.physical()['joints'][1]['drive']['target_rotation']), 'Manual selection changed effective target or kept attribution')
    c.step([d.drive('elbow', .2, 0.)]); driven = c.archive('driven continuation origin')
    reference = future(c, 'driven future reference'); c.load(driven)
    pair(c.report, 'warm driven complete restore', driven, c.archive('warm driven restored'))
    pair(c.report, 'warm driven complete future', reference, future(c, 'warm driven future'))
    if c.velocity_tracking: velocity_cases(c)
    return driven, reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path); parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--velocity-tracking', action='store_true', help='Opt into fitted-interval velocity; retain the complete legacy sequence and add independent velocity checks.')
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    report = dict(format='poima.physical-animation-protocol.v1', passed=False, calls=[], owners=[], archives=[], whole_pairs=[], observations=[],
        velocity_tracking=args.velocity_tracking, velocity_checks=[],
        limits=dict(seconds=SECONDS, rpc=CALLS, recorded_bytes=32*MIB, evidence_bytes=64*MIB),
        limitations=['Tiny deterministic analytic fixture; no original-character, rendering, motion-quality, performance, managed gameplay or NativeAOT claim.',
                    'Full save and continuation comparisons are same-platform/image, with no excluded fields. External owner must establish process-tree closure.'])
    f.ARGS, f.REPORT, f.DEADLINE, f.MAX_CALLS, f.MAX_RECORDED = args, report, time.monotonic()+SECONDS, CALLS, 32*MIB
    io = None; pins = {}
    try:
        files = [Path(__file__).resolve(), Path(f.__file__).resolve(), Path(d.__file__).resolve(),
                 ROOT/'tests/managed_physical_gameplay_contract.py', ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py', args.binary.resolve()]
        for path in files: need(path.is_file(), 'Required protocol input absent: '+str(path)); pins[str(path)] = sha(path)
        report['input_pins'] = copy.deepcopy(pins)
        io = f.Session(output/'original-owner'); discovery(io); report['authoring'] = prepare(io, args.velocity_tracking)
        for name in ('source', 'package'):
            path = Path(report['authoring'][name]); pins[str(path)] = sha(path)
        report['input_pins'] = copy.deepcopy(pins)
        world, original = io.world, io.world.read_bytes(); saves = f.save_root(io); c = Consumer(io, report, saves)
        driven, reference = exercise(c); io.close(); io = None
        io = f.Session(output/'cold-driven-owner', world)
        need(io.rpc('save.configure', dict(request_id=uid(), expected_generation=0, root=f.native(saves)))['generation'] == 1, 'Fresh save host configuration')
        c = Consumer(io, report, saves); c.load(driven, existing=False)
        pair(report, 'cold driven complete restore', driven, c.archive('cold driven restored'))
        pair(report, 'cold driven complete future', reference, future(c, 'cold driven future'))
        c.step([source('shoulder', 'manual', 0.)]); released = c.archive('released continuation origin')
        release_future = released_future(c, 'released future reference'); io.close(); io = None
        io = f.Session(output/'cold-released-owner', world)
        need(io.rpc('save.configure', dict(request_id=uid(), expected_generation=0, root=f.native(saves)))['generation'] == 1, 'Released save host configuration')
        c = Consumer(io, report, saves); c.load(released, existing=False)
        pair(report, 'cold released complete restore', released, c.archive('cold released restored'))
        pair(report, 'cold released complete future', release_future, released_future(c, 'cold released future'))
        need(world.read_bytes() == original, 'Protocol workflow changed original authoring bytes'); io.close(); io = None
        need(len(report['owners']) == 3 and all(o['natural_exit'] and not o['forced_cleanup'] and o['exit_code'] == 0 for o in report['owners']), 'Three CLI owners must close naturally')
        need(all(sha(path) == digest for path, digest in pins.items()), 'Original input changed during protocol qualification')
        report['passed'] = True
    except Exception as error:
        report['failure'] = dict(type=type(error).__name__, message=str(error), traceback=traceback.format_exc())
    finally:
        if io is not None:
            try: io.close()
            except Exception as error: report['closure_failure'] = dict(type=type(error).__name__, message=str(error)); report['passed'] = False
        report['elapsed_seconds'] = time.monotonic()-(f.DEADLINE-SECONDS); report['recordings'] = []
        for owner in report['owners']:
            for name in ('requests.jsonl', 'responses.jsonl', 'stderr.log'):
                path = Path(owner['directory'])/name
                if path.is_file(): report['recordings'].append(dict(path=str(path), bytes=path.stat().st_size, sha256=sha(path)))
        encoded = json.dumps(report, indent=2, allow_nan=False)+'\n'
        if len(encoded.encode()) > 64*MIB:
            report['passed'] = False; report['failure'] = dict(type='EvidenceBound', message='Evidence exceeds64MiB; no fields removed')
            encoded = json.dumps(report, indent=2, allow_nan=False)+'\n'
        (output/'evidence.json').write_text(encoded, encoding='utf-8')
        print(json.dumps(dict(passed=report['passed'], output=str(output), **{key: len(report[key]) for key in ('calls', 'owners', 'archives', 'whole_pairs', 'observations')})))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
