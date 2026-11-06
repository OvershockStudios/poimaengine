#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unchanged Knight planted feet, corrected-pose takeover and whole cold saves.

Source-only until executed under an external owned process-tree supervisor.
Builds/downloads nothing; opens no window. Idle is paused for explicit contact
calibration. This does not qualify gait, active balance, rendering or performance.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import time
import traceback

import physical_character_controller_handoff as h
import runtime_ground_contacts as contacts

original, f, k, t = h.original, h.f, h.k, h.t
REPO = h.REPO
SELF = Path(__file__).resolve()
ACTOR, CAMERA, OBSERVER, ENV, LIGHT, VISIBLE_FLOOR = (h.ACTOR, h.CAMERA, h.OBSERVER, h.ENV, h.LIGHT, h.VISIBLE_FLOOR)
IDENTITY, CONTROLLER, EYE, OBSERVER_LOCAL = h.IDENTITY, h.CONTROLLER, h.EYE, h.OBSERVER_LOCAL
ACTOR_INITIAL = dict(position=[1., 0., -.5], rotation=copy.deepcopy(original.WRAPPER['rotation']), scale=[1., 1., 1.])
CHAINS = contacts.CHAINS
SECONDS, RPC_LIMIT, TAKEOVER = 360, 3000, 4
Session, uid = h.Session, h.uid


def native_mass(authored):
    # The backend stores a float32 mass override, Jolt stores its float32
    # reciprocal, and runtime.physical_rig returns the double reciprocal of
    # that stored inverse mass. Keep an exact oracle for that public value;
    # the authored double and the literal native reply remain unchanged.
    mass = struct.unpack('<f', struct.pack('<f', authored))[0]
    inverse = struct.unpack('<f', struct.pack('<f', 1. / mass))[0]
    return 1. / inverse


def grounded_chains():
    return [dict(slot=s, root=r, middle=m, end=e, target=[0., 0., 0.],
        pole=[0., 1., 1.], space='rig', metric='near-uniform-v1',
        grounding=dict(copy.deepcopy(contacts.CALIBRATION[i]), reference_height=0.,
            plant_lock=True, plant_height=.03, lift_height=.15, probe_above=.3,
            probe_below=.6, max_correction=.4, max_slope_degrees=55.))
        for i, (s, r, m, e) in enumerate(CHAINS)]


def prepare(s,a,oracle,profile):
    imported=s.rpc('asset.import',dict(source=f.native(a.source)));asset=imported['asset']
    k.need(type(asset)is str and len(asset)==64 and all(x in '0123456789abcdef'for x in asset),'Safe complete cooked content identity')
    k.need((imported['nodes'],imported['animations'],imported['skins'],imported['primitives'])==(57,76,1,15),'Unchanged original full library import membership')
    package=Path(str(s.world)+'.assets')/(asset+'.pmodel');raw=package.read_bytes()
    k.need(hashlib.sha256(raw).hexdigest()==asset,'Actual entire cooked content address');k.decode_cooked(raw,oracle)
    def create(identity,name,parent=None):return dict(op='entity.create',id=identity,name=name,parent=parent)
    light_rotation=k.quaternion([0.,math.sin(.2),0.,math.cos(.2)],[math.sin(-.4),0.,0.,math.cos(.4)])
    ops=[create(ACTOR,'Original Knight virtual actor'),f.component(ACTOR,'Transform',copy.deepcopy(ACTOR_INITIAL)),
        f.component(ACTOR,'CharacterController',copy.deepcopy(CONTROLLER)),
        dict(op='asset.instantiate',id=f.RIG,name='Original Knight physical library',asset=asset,parent=ACTOR),
        f.component(f.RIG,'Transform',copy.deepcopy(IDENTITY)),
        f.component(f.RIG,'AnimationRig',dict(asset=asset,clip=36,time=0.,speed=1.,loop=True,playing=False,ik=grounded_chains())),
        f.component(f.RIG,'PhysicalRig',copy.deepcopy(profile)),
        create(CAMERA,'Owned character eye',ACTOR),f.component(CAMERA,'Transform',copy.deepcopy(EYE)),
        f.component(CAMERA,'Camera',dict(vertical_fov=70,near=.05,far=100)),
        create(f.FLOOR,'Static original flat floor'),
        f.component(f.FLOOR,'Transform',dict(position=[0.,-.25,0.],rotation=[0.,0.,0.,1.],scale=[1.,1.,1.])),
        f.component(f.FLOOR,'BoxCollider',dict(half_extents=[12,.25,12],motion='static',mass=1,friction=.65,restitution=0)),
        create(OBSERVER,'Original-character observer',ACTOR),f.component(OBSERVER,'Transform',copy.deepcopy(OBSERVER_LOCAL)),
        f.component(OBSERVER,'Camera',dict(vertical_fov=45,near=.1,far=100)),
        create(ENV,'Original-character environment'),f.component(ENV,'Transform',copy.deepcopy(IDENTITY)),
        f.component(ENV,'LightingEnvironment',dict(ambient=[.12,.12,.12],exposure=1)),
        create(LIGHT,'Original-character directional light'),
        f.component(LIGHT,'Transform',dict(position=[0.,4.,2.],rotation=light_rotation,scale=[1.,1.,1.])),
        f.component(LIGHT,'Light',dict(kind='directional',color=[1,.97,.92],intensity=2.5,enabled=True,
            shadow=dict(enabled=True,near=.05,distance=30,bias=.001,normal_bias=.01))),
        create(VISIBLE_FLOOR,'Visible matching flat floor'),
        f.component(VISIBLE_FLOOR,'Transform',dict(position=[0.,-.25,0.],rotation=[0.,0.,0.,1.],scale=[24.,.5,24.])),
        f.component(VISIBLE_FLOOR,'MeshRenderer',dict(primitive='box',albedo=[.25,.27,.29],visible=True))]
    k.need(f.transact(s,ops,0)['revision']==1,'Complete original actor/library/observer content committed revision1')
    return dict(asset=asset,package=t.identity(package),world=t.identity(s.world),clip=36,terrain='flat',actor=ACTOR,camera=CAMERA,observer=OBSERVER)



def query(s, tick, session, method, **params):
    result = s.rpc(method, dict(session_id=session, tick=tick, **params))
    k.need(set(result) == {'session_id', 'tick', 'source', 'state'} and
           result['session_id'] == session and type(result['tick']) is int and
           result['tick'] == tick and result['source'] == 'runtime', 'Exact native observation envelope')
    return result['state']


def observe(s, tick, session, oracle, profile, label, anchor=None):
    passive = anchor is not None
    rows = original.all_nodes(s, tick, session)
    k.need(len(rows) == 57 and [r['id'] for r in rows] == [f.node(i) for i in range(57)], 'Complete original57-node membership')
    actor = h.entity(s, tick, session, ACTOR)
    wrapper = h.entity(s, tick, session, f.RIG)
    camera = h.entity(s, tick, session, CAMERA)
    observer = h.entity(s, tick, session, OBSERVER)
    support = h.entity(s, tick, session, f.FLOOR)
    state = query(s, tick, session, 'runtime.physical_rig', entity=f.RIG, include_parts=True, include_joints=True)
    ownership = h.ownership(s, tick, session)
    procedural = query(s, tick, session, 'runtime.procedural_pose', entity=f.RIG)
    feet = [query(s, tick, session, 'runtime.ik', entity=f.RIG, slot=slot,
        include_end_orientation=True, include_metric=True, include_grounding=True, include_planting=True)
        for slot, *_ in CHAINS]
    requested = [query(s, tick, session, 'runtime.grounding', entity=f.RIG, slot=slot) for slot, *_ in CHAINS]
    k.need(set(procedural) == {'entity', 'phase', 'diagnostics', 'sampled_tick', 'slots'} and
        procedural['entity'] == f.RIG and f.exact(procedural['slots'], [1, 2]), 'Complete procedural ownership fields')
    k.need(procedural['phase'] == ('physical' if passive else 'animated') and
        procedural['diagnostics'] == ('last_animated_pose' if passive else 'current') and
        procedural['sampled_tick'] == (TAKEOVER if passive else tick), 'Committed procedural phase/age')
    k.need(state['entity'] == f.RIG and state['mode'] == ('passive' if passive else 'animated') and
        state['body_count'] == len(state['parts']) == 15 and state['joint_count'] == len(state['joints']) == 14,
        'Complete physical graph')
    k.need([p['name'] for p in state['parts']] == [p['name'] for p in profile['parts']] and
        [j['name'] for j in state['joints']] == [j['name'] for j in profile['joints']] and
        len({p['body_id'] for p in state['parts']}) == 15 and
        all(set(p) == f.PART_FIELDS for p in state['parts']) and
        all(set(j) == f.JOINT_FIELDS for j in state['joints']), 'Physical parts/joints complete identities')
    k.need(ownership['phase'] == ('physical' if passive else 'locomotion') and
        ownership['capsule_enrolled'] is (not passive), 'Capsule ownership and broadphase enrollment')
    raw = oracle.pose(36, 0.)
    local = [r['local_transform'] for r in rows]
    model, logical = contacts.metric.fk(local, oracle.parents)
    world = [t.multiply(wrapper['world_matrix'], m) for m in model]
    mutable_rotations = {n for _, root, middle, end in CHAINS for n in (root, middle, end)}
    mapped = {p['node'] for p in profile['parts']}
    bodies = []
    for i, (row, source) in enumerate(zip(rows, raw[0])):
        k.need(row['tick'] == tick and row['session_id'] == session, 'Original node clock/owner')
        k.near(row['world_matrix'], world[i], 3e-7, 'Independent whole-affine FK')
        k.need(all(t.ulps(a, b) <= t.SCALE_ULPS for a, b in zip(local[i]['scale'], source['scale'])), 'Unchanged original source scales')
        if not passive or i not in mapped:
            k.near(local[i]['position'], source['position'], t.MATRIX_TOL, 'Original local translation retained')
        if (not passive and i not in mutable_rotations) or (passive and i not in mapped):
            k.need(abs(abs(k.dot(local[i]['rotation'], source['rotation'])) - 1) <= 2e-10, 'Unowned original local quaternion retained')
    wrapper_q = k.normalized(k.quaternion(actor['local_transform']['rotation'], wrapper['local_transform']['rotation']))
    for p, body in zip(profile['parts'], state['parts']):
        n = p['node']
        bone = dict(position=world[n][12:15], rotation=k.normalized(k.quaternion(wrapper_q, logical[n])))
        expected = dict(part=p['name'], **k.compose(bone, p['body_in_bone']))
        bodies.append(expected)
        k.need(body['node'] == n and body['node_entity'] == f.node(n) and
            type(body['mass']) is float and body['mass'] == native_mass(p['mass']) and
            body['enrolled'] is passive, 'Physical part exact source mapping/mass/enrollment')
        if passive:
            k.near(body['position'], expected['position'], 4e-4, 'Solved physical COM versus independent skeletal frame')
            k.need(abs(abs(k.dot(body['rotation'], expected['rotation'])) - 1) <= 2e-5, 'Solved physical logical orientation')
    for current, expected in ((camera, EYE), (observer, OBSERVER_LOCAL)):
        k.need(f.exact(current['local_transform'], expected), 'Separate camera local pose preserved')
        k.near(current['world_matrix'], t.multiply(actor['world_matrix'], t.transform_matrix(**expected)), t.MATRIX_TOL, 'Separate camera inherited frame')
    soles = []
    for i, ((slot, root, middle, end), foot, request) in enumerate(zip(CHAINS, feet, requested)):
        k.need(foot['entity'] == f.RIG and [foot['slot'], foot['root'], foot['middle'], foot['end']] == [slot, root, middle, end], 'Actual original IK chain mapping')
        k.need(set(request) == {'entity', 'slot', 'enabled', 'weight'} and request['entity'] == f.RIG and
            request['slot'] == slot and request['enabled'] is True and type(request['weight']) is float and request['weight'] == 1., 'Complete requested grounding controls')
        g, plant = foot['grounding'], foot['planting']
        if passive:
            k.need(f.exact(foot, anchor['feet'][i]) and f.exact(request, anchor['requested'][i]), 'Complete historical feet/requests unchanged after takeover')
        elif tick == 0:
            k.need(g['decision_tick'] == 0 and g['casts'] == 0 and plant['status'] == 'uninitialized' and plant['anchor'] is None, 'Startup has no fabricated contact')
        else:
            definition = grounded_chains()[i]['grounding']
            actual = t.point(world[end], definition['sole_offset'])
            normal = contacts.normal(world[end], definition)
            k.near(g['actual_sole'], actual, 3e-7, 'Independent original full-affine sole')
            k.near(g['actual_normal'], normal, 3e-6, 'Independent original full-affine sole normal')
            k.need(g['decision_tick'] == tick and g['cast'] is True and g['hit']['entity'] == f.FLOOR and
                plant['status'] == 'locked' and plant['armed'] is False and plant['anchor']['support'] == f.FLOOR,
                'Actual floor contact and support-relative acquired lock')
            target = t.point(support['world_matrix'], plant['anchor']['local_point'])
            k.near(actual, target, 2e-4, 'Actual original sole stays at support anchor')
            k.near([plant['position_error']], [math.dist(actual, target)], 3e-7, 'Independent full-affine planted residual')
            k.need(plant['normal_error'] < 2e-4, 'Actual planted orientation achieved')
            soles.append(dict(slot=slot, actual=actual, normal=normal, target=target))
    if passive:
        k.need(ownership['takeover_tick'] == TAKEOVER and ownership['capsule_body_id'] == anchor['handoff']['capsule_body_id'], 'Captured ownership tick/native capsule ID')
        for field in ('yaw', 'pitch', 'velocity', 'ground', 'locomotion'):
            k.need(f.exact(actor[field], anchor['actor'][field]), 'Suppressed motor field changed: ' + field)
        k.near([actor['world_matrix'][4*c+r] for c in range(3) for r in range(3)],
               [anchor['actor']['world_matrix'][4*c+r] for c in range(3) for r in range(3)], 1e-6, 'Captured actor heading')
        offset = k.sub(anchor['actor']['world_matrix'][12:15], anchor['wrapper']['world_matrix'][12:15])
        k.near(actor['world_matrix'][12:15], k.add(wrapper['world_matrix'][12:15], offset), 1e-7, 'Actor follows solved wrapper translation')
    clock = wrapper['animation']
    t.compare_clock(clock, dict(clip=36, time=0., speed=1., loop=True, playing=False,
        duration=oracle.clips[36]['duration'], transition=None))
    record = dict(label=label, tick=tick, passive=passive, all_nodes=rows, actor=actor, wrapper=wrapper,
        camera=camera, observer=observer, support=support, physical=state, handoff=ownership,
        procedural=procedural, feet=feet, requested=requested, independent_soles=soles, independent_bodies=bodies)
    f.REPORT['observations'].append(record)
    return record


def archive(s, saves, tick, session, label, observation):
    value, raw, record = original.archived(s, saves, tick, label, session)
    payload = value['snapshot']['payload']
    by_id = {row['id']: row for row in payload['entities']}
    k.need(len(by_id) == len(payload['entities']), 'Complete saved entity uniqueness')
    for row in observation['all_nodes'] + [observation[n] for n in ('actor', 'wrapper', 'camera', 'observer', 'support')]:
        k.need(f.exact(by_id[row['id']]['local'], row['local_transform']), 'Whole saved/native local transform')
    physical = payload['physical']
    k.need(physical['version'] == 4 and len(physical['rigs']) == 1, 'Grounded physical extension4')
    rig = physical['rigs'][0]
    k.need(rig['entity'] == f.RIG and rig['handoff']['actor'] == ACTOR and rig['procedural_capture'] is not None, 'Complete saved grounded owner')
    capture = rig['procedural_capture']
    k.need(capture['version'] == 2 and capture['grounding']['prepared'] is True and
        capture['captured_tick'] == observation['procedural']['sampled_tick'] and
        [row['slot'] for row in capture['states']] == [1, 2], 'Complete prepared two-slot witness')
    for cached, current in zip(capture['states'], observation['feet']):
        k.need(set(cached) == {'entity', 'slot', 'root', 'middle', 'end', 'enabled', 'space', 'target',
            'pole', 'weight', 'end_position', 'target_error', 'clamped', 'pole_fallback',
            'end_orientation', 'metric', 'grounding'}, 'Complete saved cached state fields')
        for key in set(cached)-{'grounding', 'end_orientation', 'metric'}:
            k.need(f.exact(cached[key], current[key]), 'Saved/public point field: '+key)
        k.need(f.exact(cached['grounding']['state'], current['grounding']) and
            f.exact(cached['grounding']['planting'], current['planting']), 'Complete saved/public contact and anchor')
        saved_orientation, observed_orientation = cached['end_orientation'], current['end_orientation']
        k.need((saved_orientation is None) == (observed_orientation is None), 'Saved/public orientation presence')
        if saved_orientation is not None:
            k.need(f.exact(saved_orientation['goal'], dict(rotation=observed_orientation['rotation'], weight=observed_orientation['weight'])) and
                f.exact(saved_orientation['actual_rotation'], observed_orientation['actual_rotation']) and
                f.exact(saved_orientation['angular_error'], observed_orientation['angular_error']), 'Complete saved/public orientation')
        for key, metric_value in cached['metric'].items():
            k.need(f.exact(metric_value, current['metric'][key]), 'Saved/public metric: '+key)
    if not observation['passive']:
        k.need(rig['procedural_terminal'] is None, 'Animated owner has no physical terminal')
    if observation['passive']:
        k.need(f.exact(rig['procedural_capture'], rig['procedural_terminal']) and
            rig['procedural_terminal']['version'] == 2 and rig['handoff']['passive']['takeover_tick'] == TAKEOVER,
            'Exact complete historical grounded witness')
        k.need(not any(b['id'] == observation['handoff']['capsule_body_id'] for b in physical['physics']['bodies']), 'Suppressed capsule absent from enrolled native state')
    call = f.REPORT['calls'][-1]
    manifest_path = saves/('slot-' + label)/'current.json'
    record.update(observation=copy.deepcopy(observation), receipt=copy.deepcopy(call['response']['result']),
        manifest_path=str(manifest_path), manifest=h.strict_json(manifest_path.read_bytes()),
        manifest_identity=t.identity(manifest_path))
    t.output_bound(f.ARGS.output)
    return value, raw, record


def verify_history(saved, observations, profile):
    history = saved['snapshot']['payload']['physical']['rigs'][0]['pose']
    k.need(history['current']['tick'] == TAKEOVER and history['previous']['tick'] == TAKEOVER-1,
           'Exact last two completed corrected history ticks')
    result = []
    for frame in (history['previous'], history['current']):
        now = observations[frame['tick']]
        old = observations[frame['tick']-1]
        k.need([p['part'] for p in frame['bodies']] == [p['name'] for p in profile['parts']], 'Complete ordered15body pose history')
        for actual, current, previous in zip(frame['bodies'], now['independent_bodies'], old['independent_bodies']):
            linear = k.mul(k.sub(current['position'], previous['position']), 60.)
            angular = original.angular_velocity(previous['rotation'], current['rotation'])
            k.near(actual['position'], current['position'], t.COM_TOL, 'Independent corrected COM history')
            k.need(abs(abs(k.dot(actual['rotation'], current['rotation']))-1) <= 2e-10, 'Independent corrected logical rotation history')
            k.near(actual['linear_velocity'], linear, t.VELOCITY_TOL, 'Corrected COM finite-difference handoff velocity')
            k.near(actual['angular_velocity'], angular, t.ANGULAR_TOL, 'Corrected quaternion-log handoff velocity')
            result.append(dict(tick=frame['tick'], part=current['part'], position=current['position'],
                rotation=current['rotation'], linear_velocity=linear, angular_velocity=angular))
    return result


def step(s, tick, session, ticks=1, activate=False, moving=False, error=None, **extra):
    inputs = [dict(entity=ACTOR, move=[.08, 0.] if moving else [0., 0.], look=[0., 0.], jump=False, use=False)]
    args = dict(session_id=session, request_id=uid(), expected_tick=tick, ticks=ticks, inputs=inputs)
    if activate:
        args['physical_rigs'] = [dict(entity=f.RIG, activate=True)]
    args.update(extra)
    result = s.rpc('runtime.step', args, error)
    if error is None:
        k.need(type(result['tick']) is int and result['tick'] == tick+ticks, 'Completed exact native tick')
    return result


def run(a, oracle, profile):
    s = Session(a.output/'primary')
    try:
        f.REPORT['prepared'] = prepare(s, a, oracle, profile)
        package = Path(str(s.world)+'.assets')/(f.REPORT['prepared']['asset']+'.pmodel')
        f.REPORT['authored_identity'] = dict(world=t.identity(s.world), package=t.identity(package))
        session = uid()
        s.rpc('runtime.start', dict(session_id=session, revision=1))
        saves = f.save_root(s)
        f.REPORT['artifacts'] = dict(world=str(s.world), asset_package=str(package), saves_root=str(saves),
            authored_revision=1, save_configuration_generation=1, render_observer=OBSERVER)
        observations = {}
        for tick in range(TAKEOVER+1):
            if tick:
                step(s, tick-1, session, moving=tick >= 3)
            observations[tick] = observe(s, tick, session, oracle, profile, 'animated-'+str(tick))
            if tick == 2:
                archive(s, saves, tick, session, 'planted-stationary', observations[tick])
        anchor = observations[TAKEOVER]
        raw_model = oracle.pose(36, 0.)[2]
        for i, (_, _, _, end) in enumerate(CHAINS):
            uncorrected = t.point(t.multiply(anchor['wrapper']['world_matrix'], raw_model[end]),
                grounded_chains()[i]['grounding']['sole_offset'])
            k.need(math.dist(uncorrected, anchor['feet'][i]['grounding']['actual_sole']) > .0001,
                'Selected imported feet must actually differ from uncorrected source pose')
        delta = k.sub(anchor['actor']['world_matrix'][12:15], observations[2]['actor']['world_matrix'][12:15])
        k.need(math.hypot(delta[0], delta[2]) > .0001, 'Actual resolved motor displacement before planted takeover')
        for before, after in zip(observations[2]['feet'], anchor['feet']):
            k.need(f.exact(before['planting']['anchor'], after['planting']['anchor']), 'Real planted anchors remain fixed during small motor displacement')
        base = archive(s, saves, TAKEOVER, session, 'planted-active', anchor)
        f.REPORT['corrected_history'] = verify_history(base[0], observations, profile)
        # A command conflict must leave the complete native snapshot untouched.
        step(s, TAKEOVER, session, activate=True, error=-32040,
             grounding=[dict(entity=f.RIG, slot=1, enabled=True, weight=1.)])
        rejected = observe(s, TAKEOVER, session, oracle, profile, 'rejected-activation-origin')
        t.whole(base, archive(s, saves, TAKEOVER, session, 'rejected-activation', rejected), 'whole-conflicting-activation')
        step(s, TAKEOVER, session, activate=True)
        first_observation = observe(s, TAKEOVER+1, session, oracle, profile, 'first-physical', anchor)
        first = archive(s, saves, TAKEOVER+1, session, 'takeover-first', first_observation)
        step(s, TAKEOVER+1, session, ticks=4)
        future_observation = observe(s, TAKEOVER+5, session, oracle, profile, 'physical-future', anchor)
        future = archive(s, saves, TAKEOVER+5, session, 'takeover-future', future_observation)
        warm = uid()
        k.need(original.restore(s, saves, base[2]['slot'], warm, session, TAKEOVER+5) == TAKEOVER, 'Warm active restore tick')
        restored = observe(s, TAKEOVER, warm, oracle, profile, 'active-warm-origin')
        t.whole(base, archive(s, saves, TAKEOVER, warm, 'active-warm-origin', restored), 'whole-active-warm-origin')
        step(s, TAKEOVER, warm, activate=True)
        replay = observe(s, TAKEOVER+1, warm, oracle, profile, 'active-warm-takeover', anchor)
        t.whole(first, archive(s, saves, TAKEOVER+1, warm, 'active-warm-takeover', replay), 'whole-active-warm-takeover')
        step(s, TAKEOVER+1, warm, ticks=4)
        replay = observe(s, TAKEOVER+5, warm, oracle, profile, 'active-warm-future', anchor)
        t.whole(future, archive(s, saves, TAKEOVER+5, warm, 'active-warm-future', replay), 'whole-active-warm-future')
        t.world_witness(s, 'primary-before-close')
        s.close()
        t.world_witness(s, 'primary-after-close')
        s = Session(a.output/'cold-active', s.world)
        session = uid()
        k.need(original.restore(s, saves, base[2]['slot'], session) == TAKEOVER, 'Fresh owner active checkpoint tick')
        restored = observe(s, TAKEOVER, session, oracle, profile, 'active-cold-origin')
        t.whole(base, archive(s, saves, TAKEOVER, session, 'active-cold-origin', restored), 'whole-active-cold-origin')
        step(s, TAKEOVER, session, activate=True)
        replay = observe(s, TAKEOVER+1, session, oracle, profile, 'active-cold-takeover', anchor)
        t.whole(first, archive(s, saves, TAKEOVER+1, session, 'active-cold-takeover', replay), 'whole-active-cold-takeover')
        step(s, TAKEOVER+1, session, ticks=4)
        replay = observe(s, TAKEOVER+5, session, oracle, profile, 'active-cold-future', anchor)
        passive_base = archive(s, saves, TAKEOVER+5, session, 'active-cold-future', replay)
        t.whole(future, passive_base, 'whole-active-cold-future')
        step(s, TAKEOVER+5, session, ticks=6)
        replay = observe(s, TAKEOVER+11, session, oracle, profile, 'passive-next', anchor)
        passive_future = archive(s, saves, TAKEOVER+11, session, 'passive-next', replay)
        warm = uid()
        k.need(original.restore(s, saves, passive_base[2]['slot'], warm, session, TAKEOVER+11) == TAKEOVER+5, 'Warm passive checkpoint tick')
        restored = observe(s, TAKEOVER+5, warm, oracle, profile, 'passive-warm-origin', anchor)
        t.whole(passive_base, archive(s, saves, TAKEOVER+5, warm, 'passive-warm-origin', restored), 'whole-passive-warm-origin')
        step(s, TAKEOVER+5, warm, ticks=6)
        replay = observe(s, TAKEOVER+11, warm, oracle, profile, 'passive-warm-next', anchor)
        t.whole(passive_future, archive(s, saves, TAKEOVER+11, warm, 'passive-warm-next', replay), 'whole-passive-warm-next')
        t.world_witness(s, 'cold-active-before-close')
        s.close()
        t.world_witness(s, 'cold-active-after-close')
        s = Session(a.output/'cold-passive', s.world)
        session = uid()
        k.need(original.restore(s, saves, passive_base[2]['slot'], session) == TAKEOVER+5, 'Fresh owner passive checkpoint tick')
        restored = observe(s, TAKEOVER+5, session, oracle, profile, 'passive-cold-origin', anchor)
        t.whole(passive_base, archive(s, saves, TAKEOVER+5, session, 'passive-cold-origin', restored), 'whole-passive-cold-origin')
        step(s, TAKEOVER+5, session, ticks=6)
        replay = observe(s, TAKEOVER+11, session, oracle, profile, 'passive-cold-next', anchor)
        t.whole(passive_future, archive(s, saves, TAKEOVER+11, session, 'passive-cold-next', replay), 'whole-passive-cold-next')
        t.world_witness(s, 'cold-passive-before-close')
        f.REPORT['final_tick'] = TAKEOVER+11
    finally:
        s.close()


def audit_archives():
    for row in f.REPORT['archives']:
        path = Path(row['path'])
        k.need(t.identity(path) == dict(bytes=row['bytes'], sha256=row['sha256']), 'Whole archive changed after owner closure')
        value = h.strict_json(path.read_bytes())
        by_id = {x['id']: x for x in value['snapshot']['payload']['entities']}
        for node in row['observation']['all_nodes']:
            k.need(f.exact(by_id[node['id']]['local'], node['local_transform']), 'Postclosure whole57-node local witnesses')
        k.need(f.exact(h.strict_json(Path(row['manifest_path']).read_bytes()), row['manifest']) and
            t.identity(row['manifest_path']) == row['manifest_identity'], 'Complete original manifest changed')
    for row in f.REPORT['whole_pairs']:
        left, right = Path(row['left']).read_bytes(), Path(row['right']).read_bytes()
        k.need(left == right and len(left) == row['bytes'] and hashlib.sha256(left).hexdigest() == row['sha256'], 'Postclosure complete archive pair mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source', 'license', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    for key in ('binary', 'source', 'license', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    k.need(not args.output.exists(), 'Fresh output directory required')
    args.output.mkdir(parents=True)
    f.ARGS, f.DEADLINE, f.MAX_CALLS, f.MAX_RECORDED = args, time.monotonic()+SECONDS, RPC_LIMIT, 48*1024*1024
    f.REPORT = dict(format='poima.physical-character-grounded-handoff.v1', passed=False,
        calls=[], owners=[], archives=[], whole_pairs=[], observations=[], world_identity_joins=[], errors=[],
        bounds=dict(seconds=SECONDS, rpc=RPC_LIMIT, observed_output_bytes=t.OUTPUT_LIMIT,
            transcript_per_owner=f.MAX_RECORDED, source_scale_ulps=t.SCALE_ULPS),
        scope='One original Knight, paused Idle calibration, actual planted contacts and small resolved actor movement; corrected two-frame COM/rotation history, full-body passive transfer and complete three-process save continuation. No source-only execution, freefall momentum, rendered motion, active balance, NativeAOT or performance claim.')
    pins = {}
    try:
        # Imported repository helpers form the actual Python input closure.
        import sys
        helpers = {Path(module.__file__).resolve() for module in tuple(sys.modules.values())
                   if getattr(module, '__file__', None) and Path(module.__file__).resolve().is_relative_to(REPO)
                   and Path(module.__file__).suffix == '.py'}
        inputs = {SELF, args.binary, args.source, args.license, *helpers,
            REPO/'examples/physical-character-yard/profiles/knight.json',
            REPO/'examples/physical-character-yard/source-manifest.json'}
        pins = {str(p): t.identity(p) for p in sorted(inputs)}
        f.REPORT['inputs'] = pins
        oracle, initial_profile, calibration = k.inspect_inputs(args.source, args.license)
        profile = copy.deepcopy(initial_profile)
        profile.update(pose_frame='skeletal_bounded_scale', max_scale_deviation=t.BOUND,
            controller_handoff=dict(actor=ACTOR, follow='translate_keep_heading'), procedural_pose='grounded_animated_only')
        before = copy.deepcopy(profile)
        for key in ('max_scale_deviation', 'controller_handoff', 'procedural_pose'):
            before.pop(key)
        before['pose_frame'] = 'skeletal_fixed_scale'
        k.need(f.exact(before, initial_profile), 'Original body/joint/material source profile changed')
        f.REPORT.update(calibration=calibration, authored_profile=profile, authored_ik=grounded_chains(),
            actor_profile=copy.deepcopy(CONTROLLER), actor_initial_transform=copy.deepcopy(ACTOR_INITIAL))
        run(args, oracle, profile)
    except BaseException:
        f.REPORT['errors'].append(traceback.format_exc())
    try:
        h.audit_recordings()
        k.need(len(f.REPORT['owners']) == 3, 'Exactly three sequential naturally closed native owners')
        k.need(len(f.REPORT['observations']) == 19 and len(f.REPORT['archives']) == 16 and
            len(f.REPORT['whole_pairs']) == 11, 'Complete frozen observation/archive/pair cohort')
        audit_archives()
        for path, pin in pins.items():
            k.need(t.identity(path) == pin, 'Source/native/helper input changed during workflow')
    except BaseException:
        f.REPORT['errors'].append(traceback.format_exc())
    f.REPORT['passed'] = not f.REPORT['errors']
    f.REPORT['rpc_count'] = len(f.REPORT['calls'])
    f.REPORT['output_census'] = t.output_bound(args.output)
    raw = (json.dumps(f.REPORT, indent=2, allow_nan=False)+'\n').encode()
    k.need(len(raw) <= t.EVIDENCE_RESERVE and f.REPORT['output_census']['bytes']+len(raw) <= t.OUTPUT_LIMIT, 'Complete final evidence exceeds bounded output')
    (args.output/'evidence.json').write_bytes(raw)
    print(args.output/'evidence.json', flush=True)
    return 0 if f.REPORT['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
