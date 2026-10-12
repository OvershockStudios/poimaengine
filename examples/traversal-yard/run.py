#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author and launch the original Traversal Yard course through native APIs.

Supply the pinned licensed Kenney intake, or reopen a prepared cooked world.
This launcher does not build, download, teleport characters or simulate physics.
"""
import argparse
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError
from poima_client.transport import JsonRpcProcess

_old_path = ROOT/'examples/locomotion-yard/run.py'
_spec = importlib.util.spec_from_file_location('poima_traversal_intake', _old_path)
if _spec is None or _spec.loader is None:
    raise ImportError('The unchanged Locomotion Yard intake is required.')
intake = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(intake)
TYPE, IDENTITY = 'Poima.Examples.TraversalYardGame', 'poima.examples.traversal-yard'
ACTOR, RIG, PLAYER, CAMERA, OBSERVER = 300, 400, 100, 101, 102
CONFIG, COMPONENT = intake.CONFIG, intake.COMPONENT
SOURCE_SHA, SOURCE_URL = intake.SOURCE_SHA, intake.SOURCE_URL
uid, sha, node_id, normalized = intake.uid, intake.sha, intake.node_id, intake.normalized
ACTIONS = {name: 800+index for index, name in enumerate(('lift', 'pause', 'resume', 'save', 'load', 'status'))}
BEACONS, GATE, LIFT, LOWER, UPPER = (210, 211, 212), 213, 220, 221, 222
START = (-8.0, .1, 6.0)
TARGETS = {210: (-2.7, .6, 6.0), 211: (1.5, 1.4, 6.0), 212: (5.0, 0.0, 6.0), 213: (-7.5, 0.0, 6.0)}
LIFT_POSITIONS = {221: (1.5, .1, 6.0), 222: (1.5, 1.3, 6.0)}
LIFT_TICKS, RAMP_SLOPE = 90, -.3
SAMPLES, SHADOW_FILTER = 4, 'pcf3_fractional'
SUN, ENVIRONMENT = 240, 241


def need(value, message):
    if not value:
        raise AssertionError(message)


def course_ops(ui):
    """An adjoining step/lintel/lift course with a shallow return ramp."""
    ops = []
    def component(n, kind, value):
        ops.append(dict(op='component.set', id=uid(n), type=kind, value=value))
    def create(n, name, position, scale=(1, 1, 1), rotation=(0, 0, 0, 1)):
        ops.append(dict(op='entity.create', id=uid(n), name=name, parent=None))
        component(n, 'Transform', dict(position=list(position), rotation=list(rotation), scale=list(scale)))
    def box(n, name, position, scale, color, motion='static', rotation=(0, 0, 0, 1)):
        create(n, name, position, scale, rotation)
        component(n, 'MeshRenderer', dict(primitive='box', visible=True, albedo=list(color)))
        component(n, 'BoxCollider', dict(half_extents=[.5, .5, .5], motion=motion,
                                       mass=1, friction=.7, restitution=0))
    component(PLAYER, 'Transform', dict(position=list(START), rotation=[0, -math.sqrt(.5), 0, math.sqrt(.5)], scale=[1, 1, 1]))
    component(PLAYER, 'CharacterController', dict(radius=.3, height=1.8, speed=4, jump_speed=5,
        camera=uid(CAMERA), locomotion={}, stance=dict(crouched_height=1, crouched_speed=2, camera_drop=.6)))
    # Native simulation owns both movement roots; visual400 remains a separate
    # authored child, with the unchanged pinned clip/attachment configuration.
    for index in range(3):
        top = (index+1)*.2
        box(230+index, 'Ascending tread '+str(index+1), (-5.5+index, top/2, 6), (1, top, 2), (.34, .42, .5))
    box(233, 'Crouch landing', (-2.5, .3, 7), (1, .6, 4), (.26, .42, .46))
    for index, top in enumerate((.4, .2)):
        box(234+index, 'Descending tread '+str(index+1), (-1.5+index, top/2, 6), (1, top, 2), (.28, .38, .43))
    # The underside is1.9m above the floor: standing at foot.6 cannot pass,
    # while the1m crouched capsule can. Posts stay outside the usable lanes.
    box(236, 'Low passage lintel', (-2.7, 2.05, 7), (1, .3, 4), (.76, .33, .18))
    for n, z in ((237, 4.85), (238, 9.15)):
        box(n, 'Lintel support', (-2.7, .95, z), (.25, 1.9, .25), (.76, .33, .18))
    angle = math.atan(RAMP_SLOPE)
    # Top plane is y=-.3*x, x[-2,0]. Scaled unit-box geometry and its collider
    # share one transform, without applying authored dimensions a second time.
    box(239, 'Return ramp', (-1, .3-.1/math.cos(angle), 8.2), (2/math.cos(angle), .2, 1.5),
        (.24, .47, .32), rotation=(0, 0, math.sin(angle/2), math.cos(angle/2)))
    box(LIFT, 'Beacon lift', LIFT_POSITIONS[LOWER], (2, .2, 2), (.68, .54, .18), motion='kinematic')
    for n, pos in LIFT_POSITIONS.items():
        create(n, 'Lift lower endpoint' if n == LOWER else 'Lift upper endpoint', pos)
    colors = ((.18, .7, .72), (.86, .65, .13), (.38, .75, .28))
    for n, color in zip(BEACONS, colors):
        create(n, 'Beacon '+str(n-209), TARGETS[n], (.3, .4, .3))
        component(n, 'MeshRenderer', dict(primitive='box', visible=True, albedo=list(color)))
    create(GATE, 'Return station', TARGETS[GATE], (1.5, .05, 1.5))
    component(GATE, 'MeshRenderer', dict(primitive='box', visible=True, albedo=[.6, .22, .4]))
    component(OBSERVER, 'Transform', dict(position=[-1, 10, 18], rotation=[-.25881904510252074, 0, 0, .9659258262890683], scale=[1, 1, 1]))
    component(OBSERVER, 'Camera', dict(vertical_fov=60, near=.05, far=100))
    direction = [-.35, -.85, -.4]
    length = math.sqrt(sum(x*x for x in direction));direction = [x/length for x in direction]
    rotation = [direction[1], -direction[0], 0, 1-direction[2]]
    length = math.sqrt(sum(x*x for x in rotation));rotation = [x/length for x in rotation]
    create(SUN, 'Traversal afternoon sun', (0, 0, 0), rotation=rotation)
    component(SUN, 'Light', dict(kind='directional', color=[1, .94, .84], intensity=3, enabled=True,
        shadow=dict(enabled=True, near=.05, distance=60, bias=.0005, normal_bias=.01)))
    create(ENVIRONMENT, 'Traversal sky and ambient fill', (0, 0, 0))
    component(ENVIRONMENT, 'LightingEnvironment', dict(ambient=[.065, .075, .09], exposure=1, shadow_resolution=1024,
        sky=dict(enabled=True, zenith=[.06, .22, .55], horizon=[.55, .7, .85], ground=[.12, .1, .08],
                 horizon_falloff=.35, sun=uid(SUN), sun_size_degrees=.53, sun_intensity=20)))
    # Preserve the six actual native UI elements while giving the new game its
    # own controls. UI800 calls the lift; E is the existing semantic Use key.
    for n, text in ((699, 'Traversal Yard'), (700, 'Collect three beacons, then return to the start.'),
                    (701, 'Courier idle/run follows its committed movement.')):
        element = copy.deepcopy(ui[uid(n)]);element['text'] = text
        ops.append(dict(op='ui.element.set', id=uid(n), element=element))
    for name, n in ACTIONS.items():
        element = copy.deepcopy(ui[uid(n)]);element.update(name=name.title(), text=name.title(), action=name)
        ops.append(dict(op='ui.element.set', id=uid(n), element=element))
    return ops


class OwnedGame(intake.OwnedGame):
    """Explicit native owner shared by the launcher and contract verifier."""
    def __init__(self, args, world, label, record, deadline, native, manifest, binary=None):
        self.args, self.world, self.label, self.record = args, Path(world), label, record
        self.deadline, self.native, self.manifest = deadline, native, manifest
        self.session, self.tick, self.revision, self.closed, self.calls = None, 0, 0, False, 0
        self.raw_bytes = 0
        record.setdefault('transport', [])
        owner = self
        class RecordedTransport(JsonRpcProcess):
            def _accept_response(self, response):
                owner.raw('response', response)
                super()._accept_response(response)
        self.transport = RecordedTransport([str((binary or args.binary).resolve()), 'world', native(self.world)], cwd=ROOT)
        request = self.transport.request
        def logged(method, params=None, timeout=30):
            remaining = owner.deadline-time.monotonic()
            need(remaining > 0, 'Traversal Yard request deadline expired.')
            with self.transport._condition:
                identifier = self.transport._next_id
            owner.raw('request', dict(jsonrpc='2.0', id=identifier, method=method, params=copy.deepcopy(params or {})))
            return request(method, params, timeout=min(timeout, remaining))
        self.transport.request = logged
        self.client = WorldClient(self.transport)

    def raw(self, direction, message):
        row = dict(owner=self.label, direction=direction, message=copy.deepcopy(message))
        self.raw_bytes += len(json.dumps(row, allow_nan=False, separators=(',', ':')).encode())
        need(self.raw_bytes <= 32*1048576 and len(self.record['transport']) < 12000, 'Traversal RPC evidence bound exceeded.')
        self.record['transport'].append(row)

    def rpc(self, method, params=None, error=None, timeout=30):
        with self.transport._condition:
            identifier = self.transport._next_id
        row = dict(owner=self.label, rpc_id=identifier, method=method, params=copy.deepcopy(params or {}))
        self.record['calls'].append(row);self.calls += 1
        need(self.calls <= 4000, 'Traversal owner call budget exceeded.')
        try:
            result = self.client.call(method, params or {}, timeout=min(timeout, max(.001, self.deadline-time.monotonic())))
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message, data=copy.deepcopy(failure.data))
            if error is None or failure.code != error:
                raise
            return row['error']
        row['result'] = copy.deepcopy(result)
        need(error is None, 'Expected native rejection was accepted.')
        return result

    def author(self, source_directory):
        result = super().author(source_directory)
        npc = self.rpc('entity.get', dict(revision=result['revision'], id=uid(ACTOR)))
        controller = copy.deepcopy(npc['value']['components']['CharacterController']);controller['locomotion'] = {}
        original = json.loads(self.world.read_bytes())
        ops = course_ops(original['ui'])+[dict(op='component.set', id=uid(ACTOR), type='CharacterController', value=controller)]
        changed = self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=result['revision'], ops=ops))
        # Navigation carries a complete authored topology guard, including the
        # new course. Its agent remains the courier; a static bake does not
        # promise pathfinding across a moving lift.
        navigation = self.rpc('world.navigation.bake', dict(revision=changed['revision'], profile=dict(**result['capsule'], climb=0)))['asset']
        published = self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=changed['revision'],
            ops=[dict(op='navigation.set', asset=navigation)]))
        result['navigation'] = navigation;result['revision'] = published['revision']
        result['course'] = dict(start=list(START), targets={uid(n): list(p) for n, p in TARGETS.items()}, lift_ticks=LIFT_TICKS)
        return result

    def config(self):
        if self.args.descriptor:
            return dict(descriptor=self.native(self.args.descriptor))
        return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge), assembly=self.native(self.args.assembly), type=TYPE)

    def start(self):
        super().start()
        self.load()

    def step(self, count=1, error=None, inputs=None):
        params = dict(request_id=uuid.uuid4().hex, session_id=self.session, expected_tick=self.tick, ticks=count)
        if inputs is not None:
            params['inputs'] = inputs
        result = self.rpc('runtime.step', params, error)
        if error is None:
            self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def control(self, action):
        state, module = self.inspect(), self.module()
        result = self.rpc('runtime.ui.activate', dict(request_id=uuid.uuid4().hex, session_id=self.session,
            expected_tick=self.tick, expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=module['revision'], expected_structure_revision=state['structure_revision'], id=uid(ACTIONS[action])))
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def input_profile(self, path):
        profile = copy.deepcopy(self.rpc('input.describe')['defaults'])
        profile['actions'] = {'aim': ['mouse.3'], 'crouch': ['key.c']}
        return self.rpc('input.transact', dict(path=self.native(path), request_id=uuid.uuid4().hex, expected_revision=0, profile=profile))


def copy_prepared(source, destination):
    source = Path(source).resolve(strict=True)
    need(source.is_file() and not destination.exists(), 'Prepared world must be a regular input and destination fresh.')
    assets = Path(str(source)+'.assets')
    need(assets.is_dir(), 'Prepared world must retain its cooked asset directory.')
    shutil.copyfile(source, destination)
    shutil.copytree(assets, Path(str(destination)+'.assets'))
    need(sha(source) == sha(destination), 'Prepared world copy differs.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'manifest', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    content = parser.add_mutually_exclusive_group(required=True)
    content.add_argument('--source-directory', type=Path)
    content.add_argument('--prepared-world', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=-1)
    args = parser.parse_args()
    if bool(args.descriptor) == bool(args.assembly) or (not args.descriptor and not (args.hostfxr and args.bridge)):
        parser.error('Supply a native descriptor OR an assembly with hostfxr and bridge.')
    if args.output.exists():
        parser.error('Use a new output directory; existing worlds, artifacts and saves are retained.')
    args.output.mkdir(parents=True)
    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() if args.windows_interop and os.name != 'nt' else value
    world = args.output/'world.json'
    if args.prepared_world:
        copy_prepared(args.prepared_world, world)
    record = dict(calls=[], transport=[], owners=[])
    game = OwnedGame(args, world, 'interactive', record, time.monotonic()+86400, native, json.loads(args.manifest.read_text(encoding='utf-8')))
    try:
        if args.source_directory:
            game.author(args.source_directory)
        game.start();game.saves(args.output/'saves')
        profile = args.output/'traversal.poima-input.json';game.input_profile(profile)
        print('WASD move; C crouch; mouse3 aim; E collect/use lift; Space jump; Tab releases cursor. Collect all three beacons, then return.', flush=True)
        game.rpc('runtime.play', dict(request_id=uuid.uuid4().hex, session_id=game.session, expected_tick=game.tick,
            controller=uid(PLAYER), camera=uid(CAMERA), mode='interactive', input_profile=native(profile), input_revision=1,
            width=1280, height=720, gpu=args.gpu, samples=SAMPLES, shadow_filter=SHADOW_FILTER), timeout=86400)
    finally:
        try:
            game.close()
        finally:
            (args.output/'commands.json').write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
