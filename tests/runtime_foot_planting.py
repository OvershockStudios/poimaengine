#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unchanged licensed Knight: planted soles, moving supports and complete saves.

Builds/downloads nothing. Requires an external process-tree owner. Native surface
queries and full affine readback establish geometric locking, not rendered gait,
physical input, full-body balance or representative performance.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import time
import traceback

import runtime_ground_contacts as contacts
from runtime_ground_contacts import metric, base, need, identity, typed_equal, CHAINS, RIG, fk, close, distance
from character_library_intake import LibraryOracle, decode_cooked, LICENSE_SHA
from imported_character_contract import point, multiply
from components_gameplay_contract import uid

ROOT = Path(__file__).resolve().parents[1]
ACTOR, FLOOR = uid(920), uid(910)


def controls(enabled=True, weight=1.):
    return [dict(entity=RIG, slot=s, enabled=enabled, weight=weight) for s, *_ in CHAINS]


def inverse_rigid_point(matrix, world):
    delta = [world[i]-matrix[12+i] for i in range(3)]
    return [sum(matrix[4*c+r]*delta[r] for r in range(3)) for c in range(3)]


class Workflow(metric.Workflow):
    def prepare(self):
        raw = self.args.source.read_bytes()
        self.oracle = LibraryOracle(raw)
        license_bytes = self.args.license.read_bytes()
        need(hashlib.sha256(license_bytes).hexdigest() == LICENSE_SHA, 'Exact original CC0 license')
        self.source = self.args.output/'original-Knight.glb'
        self.source.write_bytes(raw)
        (self.args.output/'original-LICENSE.txt').write_bytes(license_bytes)
        info = self.p.rpc('asset.source.inspect', dict(source=self.args.native(self.source)))
        asset = self.p.rpc('asset.import', dict(source=self.args.native(self.source), expected_model_sha256=info['model_sha256']))['asset']
        self.package = Path(str(self.world)+'.assets')/(asset+'.pmodel')
        meta, meshes, mips = decode_cooked(self.package.read_bytes(), self.oracle)
        self.package_pin = identity(self.package)
        self.poses = metric.SourcePoses(self.oracle, meta)
        self.e['original_asset'] = dict(source=identity(self.source), license=identity(self.args.license),
            asset=asset, cooked=self.package_pin, nodes=57, takes=76, channels=8712, keys=90748,
            triangles=6952, vertices=7024, skin_joints=41)
        self.tx([dict(op='asset.instantiate', id=RIG, asset=asset, name='Original Knight planted feet')])
        self.rig = copy.deepcopy(json.loads(self.world.read_bytes())['entities'][RIG]['components']['AnimationRig'])
        self.rig.update(clip=36, time=0, speed=1, loop=True, playing=False,
            ik=[dict(slot=s, root=r, middle=m, end=e, target=[0, 0, 0], pole=[0, 1, 1], space='rig',
                metric='near-uniform-v1', grounding=dict(contacts.CALIBRATION[i], reference_height=0,
                    plant_lock=True, max_correction=.4)) for i, (s, r, m, e) in enumerate(CHAINS)])
        original = self.world.read_bytes()
        for bad in (0, 1, None, 'true', {}, []):
            malformed = copy.deepcopy(self.rig)
            malformed['ik'][0]['grounding']['plant_lock'] = bad
            self.tx([dict(op='component.set', id=RIG, type='AnimationRig', value=malformed)], -32602)
            need(self.world.read_bytes() == original, 'Invalid plant profile changed authored bytes')
        ops = [dict(op='entity.create', id=ACTOR, name='Kinematic original Knight wrapper'),
            dict(op='component.set', id=ACTOR, type='Transform', value=dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=ACTOR, type='BoxCollider', value=dict(half_extents=[.05]*3, motion='kinematic', mass=1, friction=.5, restitution=0)),
            dict(op='entity.reparent', id=RIG, parent=ACTOR, mode='keep_local'),
            dict(op='component.set', id=RIG, type='AnimationRig', value=self.rig),
            dict(op='entity.create', id=FLOOR, name='Moving and rotating physical support'),
            dict(op='component.set', id=FLOOR, type='Transform', value=dict(position=[0, -.15, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])),
            dict(op='component.set', id=FLOOR, type='BoxCollider', value=dict(half_extents=[5, .2, 5], motion='kinematic', mass=1, friction=.5, restitution=0))]
        # Visual-only child keeps the support's physical frame unscaled.
        # The original imported character remains byte-identical.
        ops += [dict(op='entity.create', id=uid(911), name='Support visualization', parent=FLOOR),
            dict(op='component.set', id=uid(911), type='Transform', value=dict(position=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[10, .4, 10])),
            dict(op='component.set', id=uid(911), type='MeshRenderer', value=dict(primitive='box', visible=True, albedo=[.26, .27, .29])),
            dict(op='entity.create', id=uid(3), name='Planting observation camera'),
            dict(op='component.set', id=uid(3), type='Transform', value=dict(position=[1.6, 1.3, 3],
                rotation=[math.cos(.245)*math.sin(-.044), math.sin(.245)*math.cos(-.044),
                          -math.sin(.245)*math.sin(-.044), math.cos(.245)*math.cos(-.044)], scale=[1, 1, 1])),
            dict(op='component.set', id=uid(3), type='Camera', value=dict(vertical_fov=55, near=.05, far=40)),
            dict(op='entity.create', id=uid(30), name='Observation sun'),
            dict(op='component.set', id=uid(30), type='Transform', value=dict(position=[0, 0, 0], rotation=[-.3826834323650898, 0, 0, .9238795325112867], scale=[1, 1, 1])),
            dict(op='component.set', id=uid(30), type='Light', value=dict(kind='directional', color=[1, .96, .9], intensity=3, enabled=True, shadow=dict(enabled=True, distance=30))),
            dict(op='entity.create', id=uid(31), name='Observation ambient'),
            dict(op='component.set', id=uid(31), type='LightingEnvironment', value=dict(ambient=[.08, .09, .11], exposure=1, shadow_resolution=512))]
        self.tx(ops)
        self.document = json.loads(self.world.read_bytes())
        self.document_contract = base.frozen_document_contract(self.document)
        self.retain_authored('authored-before-start')
        self.p.start(self.revision)
        self.p.rpc('save.configure', dict(request_id=base.fresh(), expected_generation=0, root=self.args.native(self.saves)))
        method = self.p.rpc('world.describe', dict(view='method', name='runtime.ik'))
        self.e['discovery'] = method

    def states(self):
        return [self.p.rpc('runtime.ik', dict(session_id=self.p.session, tick=self.p.tick,
            entity=RIG, slot=s, include_grounding=True, include_planting=True, include_metric=True,
            include_end_orientation=True))['state'] for s, *_ in CHAINS]

    def geometry(self, label, locked=False):
        local, observed, envelope = self.observe()
        computed, _ = fk(local, self.poses.parents)
        wrapper = self.p.rpc('runtime.entity', dict(session_id=self.p.session, tick=self.p.tick, id=RIG))['world_matrix']
        support = self.p.rpc('runtime.entity', dict(session_id=self.p.session, tick=self.p.tick, id=FLOOR))['world_matrix']
        states = self.states()
        for i in range(57):
            close(observed[i], multiply(wrapper, computed[i]), 3e-7, 'Independent whole imported FK')
            for key in ('position', 'scale'):
                close(local[i][key], self.poses.pose(36, 0)[i][key], 2e-8, 'Unmodified original '+key)
        feet = []
        for i, ((slot, root, middle, end), state) in enumerate(zip(CHAINS, states)):
            g, plant = state['grounding'], state['planting']
            profile = self.rig['ik'][i]['grounding']
            world = multiply(wrapper, computed[end])
            actual = point(world, profile['sole_offset'])
            actual_normal = contacts.normal(world, profile)
            close(g['actual_sole'], actual, 3e-7, 'Actual original full-affine sole')
            close(g['actual_normal'], actual_normal, 3e-6, 'Actual original full-affine normal')
            need(g['decision_tick'] == self.p.tick, 'Current committed decision')
            if locked:
                need(plant['status'] == 'locked' and plant['armed'] is False and plant['anchor']['support'] == FLOOR,
                     'Expected actual imported support-relative lock')
                anchor = plant['anchor']
                wanted = point(support, anchor['local_point'])
                close(plant['target_sole'], wanted, 3e-7, 'Independent rigid support transport')
                close(actual, wanted, 2e-4, 'Original sole must remain at its transported anchor')
                close(inverse_rigid_point(support, actual), anchor['local_point'], 2e-4, 'Actual support-relative sole drift')
                close([plant['position_error']], [distance(actual, wanted)], 3e-7, 'Independent planted residual')
                need(plant['normal_error'] < 2e-4 and g['cast'] is True and g['hit']['entity'] == FLOOR,
                     'Fresh eligible surface validation and achieved normal')
                if hasattr(self, 'anchors'):
                    need(typed_equal(anchor, self.anchors[i]), 'Planted anchor identity or local fields drifted')
            feet.append(dict(slot=slot, state=state, independent_sole=actual, independent_normal=actual_normal,
                support_local_sole=inverse_rigid_point(support, actual)))
        self.e['samples'].append(dict(label=label, tick=self.p.tick, pose_envelope=envelope,
            support=support, wrapper=wrapper, feet=feet))
        return states

    def cases(self):
        initial = self.states()
        need(all(s['planting']['status'] == 'uninitialized' and s['planting']['armed'] is True and
                 s['planting']['anchor'] is None and s['grounding']['casts'] == 0 for s in initial), 'No initial contact queries')
        baseline = self.capture('initial-planting-v10')
        need(baseline['value']['snapshot']['payload']['animation']['version'] == 10, 'Explicit planted-state version')
        for bad in (None, 0, 1, 'true', [], {}):
            self.p.rpc('runtime.ik', dict(session_id=self.p.session, tick=self.p.tick, entity=RIG, slot=1, include_planting=bad), -32602)
        default = self.p.rpc('runtime.ik', dict(session_id=self.p.session, tick=self.p.tick, entity=RIG, slot=1))['state']
        old = self.p.rpc('runtime.ik', dict(session_id=self.p.session, tick=self.p.tick, entity=RIG, slot=1, include_grounding=True))['state']
        need('planting' not in default and 'planting' not in old and 'grounding' not in default, 'Earlier query shapes changed')
        self.pair('read-and-rejection-complete-state', baseline, self.capture('after-query-admission'))
        self.step()
        states = self.geometry('idle-lock-acquired', True)
        self.anchors = [copy.deepcopy(s['planting']['anchor']) for s in states]
        original_soles = [s['grounding']['actual_sole'] for s in states]
        self.step(4, motions=[dict(entity=ACTOR, position=[.04, 0, .02], rotation=[0, 0, 0, 1], duration_ticks=4)])
        moved = self.geometry('wrapper-moves-without-sole-slide', True)
        need(all(distance(s['grounding']['actual_sole'], original_soles[i]) < 2e-4 for i, s in enumerate(moved)), 'World static-support anti-slide')
        q = [0, math.sin(.08), 0, math.cos(.08)]
        command = dict(entity=FLOOR, position=[.06, -.13, .03], rotation=q, duration_ticks=6)
        for i in range(3):
            self.step(motions=[command] if i == 0 else [])
            self.geometry('translated-yawed-support-'+str(i), True)
        source = self.capture('active-moving-support')
        self.restore(source)
        self.pair('same-owner-locked-complete', source, self.capture('same-owner-restored'))
        self.step(3)
        self.geometry('completed-platform-motion', True)
        continued = self.capture('expected-platform-continuation')
        self.cold_source, self.cold_future = source, continued
        self.cold_anchors = copy.deepcopy(self.anchors)
        # Requested disable releases/rearms and never casts. Reenable permits an
        # explicit new stance even when the stationary Idle clip never lifts.
        self.step(grounding=controls(False))
        released = self.geometry('disabled-plant-rearms')
        need(all(s['planting']['armed'] is True and s['planting']['anchor'] is None and
                 s['planting']['status'] == 'disabled' and not s['grounding']['cast'] for s in released), 'Disable policy')
        del self.anchors
        self.step(grounding=controls(True))
        reacquired = self.geometry('explicit-new-stance', True)
        self.anchors = [copy.deepcopy(s['planting']['anchor']) for s in reacquired]
        need(all(s['planting']['anchor']['acquired_tick'] == self.p.tick for s in reacquired), 'Explicit rearm did not acquire at actual tick')
        # A large wrapper move exceeds the configured correction and must not
        # ratchet the still-planted Idle foot onto a new anchor every other tick.
        self.step(motions=[dict(entity=ACTOR, position=[2, 0, 0], rotation=[0, 0, 0, 1], duration_ticks=4)])
        self.step(3)
        failed = self.states()
        need(all(s['planting']['anchor'] is None and s['planting']['armed'] is False for s in failed), 'Constraint release must disarm')
        self.step(3)
        need(all(s['planting']['anchor'] is None and s['planting']['armed'] is False for s in self.states()), 'Foot silently reacquired without a release')
        self.e['checks'].extend([
            'Original Knight geometry, all76takes/8712channels/90748keys retained;57node full-affine observations',
            'Actual Idle anti-slide under wrapper movement and off-centre translating/yawing physical support',
            'Guarded independent cached planting read, strict Boolean admission and earlier response shapes',
            'Complete locked warm/cold native archives and continued moving-platform state',
            'Disable/rearm and constrained release without repeated stance reacquisition',
        ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source', 'license', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    args.native = lambda p: str(Path(p).resolve())
    args.output = args.output.resolve()
    need(not args.output.exists() and args.output.parent.is_dir(), 'Fresh output with existing parent')
    args.output.mkdir()
    args.deadline = time.monotonic()+180
    evidence = dict(format='poima.runtime-foot-planting.v1', passed=False, calls=[], owners=[], checkpoints=[],
        whole_pairs=[], checks=[], samples=[], limitations=['CPU geometry and complete native state; no rendered gait, hardware input, production crowd, full-body balance or performance claim.',
        'Original asset/project remain available; no relocation/export or fresh-agent claim.'])
    workflow = None
    try:
        paths = [args.binary.resolve(), args.source.resolve(), args.license.resolve(), Path(__file__).resolve(),
                 Path(contacts.__file__).resolve(), Path(metric.__file__).resolve(), Path(base.__file__).resolve()]
        evidence['input_pins'] = {str(p): identity(p) for p in paths}
        workflow = Workflow(args, evidence)
        workflow.prepare()
        workflow.cases()
        workflow.p.rpc('runtime.stop', dict(session_id=workflow.p.session))
        workflow.p.close()
        workflow.p = None
        workflow.p = base.RawSession(args, workflow.world, evidence)
        workflow.p.rpc('save.configure', dict(request_id=base.fresh(), expected_generation=0, root=args.native(workflow.saves)))
        workflow.restore(workflow.cold_source, cold=True)
        workflow.pair('fresh-owner-locked-complete', workflow.cold_source, workflow.capture('fresh-owner-restored'))
        workflow.step(3)
        workflow.pair('fresh-owner-moving-platform-continuation', workflow.cold_future, workflow.capture('fresh-owner-platform-continuation'))
        workflow.anchors = workflow.cold_anchors
        workflow.geometry('fresh-owner-original-locked-feet', True)
        workflow.p.rpc('runtime.stop', dict(session_id=workflow.p.session))
        workflow.p.close()
        workflow.p = None
        workflow.guard_authored()
        need(identity(workflow.package) == workflow.package_pin, 'Original cooked library changed')
        need(all(identity(p) == value for p, value in evidence['input_pins'].items()), 'Selected source inputs changed')
        evidence['passed'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
        raise
    finally:
        if workflow and workflow.p:
            try:
                workflow.p.close()
            except BaseException:
                evidence['closure_error'] = traceback.format_exc()
                evidence['passed'] = False
        evidence['seconds'] = time.monotonic()-(args.deadline-180)
        base.census(args.output)
        (args.output/'report.json').write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n')
        print(args.output/'report.json', flush=True)


if __name__ == '__main__':
    main()
