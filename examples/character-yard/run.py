#!/usr/bin/env python3
"""Author and launch Character Yard through the native world service.

The original licensed source is supplied by the caller. This launcher does not
download content, change skeletons, synthesize poses or implement navigation.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

TYPE = 'Poima.Examples.CharacterYardGame'
ACTOR, RIG, PLAYER, CAMERA, OBSERVER = 300, 400, 100, 101, 102
CONFIG = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb70'
COMPONENT = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb71'
SOURCE_SHA = 'd6be85417d3e256861ee733eea6916093a7af7c79c16366181fd8abcaeb38cf5'
SOURCE_URL = ('https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/'
              'edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/RiggedFigure/glTF-Binary/RiggedFigure.glb')
HOME, DELIVERY = (-4, 0), (4, 0)
COVER_HALF = (.5, 1.5, 3)
ATTACHMENT = dict(position=[0, 0, 0], rotation=[0, 1, 0, 0], scale=[1, 1, 1])
BLEND_TICKS, IDLE_LEAD_TICKS = 12, 30


def uid(value): return f'{value:032x}'
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def node_id(rig, index):
    return hashlib.sha256(f'poima.instance.v1/{rig}/node/{index}'.encode()).hexdigest()[:32]


COORDINATES, CURSOR = uid(1), uid(2)
ACTIONS = {name: 800+index for index, name in enumerate(
    ('dispatch', 'gesture', 'pause', 'resume', 'save', 'load', 'status'))}


def normalized(value):
    if isinstance(value, dict):
        return {key: normalized(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list): return [normalized(item) for item in value]
    return value


def fixture(asset, clip=0):
    """One authoring transaction, independent capsule and attached visual rig."""
    ops = []

    def create(identity, name, position, scale=(1, 1, 1), parent=None, rotation=(0, 0, 0, 1)):
        ops.extend([dict(op='entity.create', id=uid(identity), name=name,
                         parent=uid(parent) if parent is not None else None),
                    dict(op='component.set', id=uid(identity), type='Transform',
                         value=dict(position=list(position), rotation=list(rotation), scale=list(scale)))])

    def component(identity, kind, value):
        ops.append(dict(op='component.set', id=uid(identity), type=kind, value=value))

    # Unit-box half extents are scaled once, equally for visible and query geometry.
    for identity, name, pos, scale, color in (
        (1, 'Yard floor', (0, -.5, 0), (20, 1, 20), (.24, .22, .19)),
        (2, 'Route-blocking cover', (0, 1.5, 0), (1, 3, 6), (.28, .08, .11)),
    ):
        create(identity, name, pos, scale)
        component(identity, 'MeshRenderer', dict(primitive='box', visible=True, albedo=list(color)))
        component(identity, 'BoxCollider', dict(half_extents=[.5, .5, .5], motion='static',
                                               mass=1, friction=.5, restitution=0))
    for identity, name, position, color in (
        (3, 'Home station', (-4, .015, 0), (.46, .16, .22)),
        (4, 'Delivery station', (4, .015, 0), (.21, .39, .29)),
    ):
        create(identity, name, position, (1.2, .03, 1.2))
        component(identity, 'MeshRenderer', dict(primitive='box', visible=True, albedo=list(color)))
    create(ACTOR, 'Courier movement root', (-4, 0, 0))
    component(ACTOR, 'CharacterController', dict(radius=.3, height=1.8, speed=3, jump_speed=5, camera=None))
    ops.append(dict(op='asset.instantiate', id=uid(RIG), name='Cesium Rigged Figure visual',
                    asset=asset, parent=uid(ACTOR)))
    component(RIG, 'Transform', ATTACHMENT)
    component(ACTOR, 'game:'+CONFIG, {
        uid(1): uid(RIG), uid(2): clip, uid(3): clip, uid(4): BLEND_TICKS,
        uid(5): IDLE_LEAD_TICKS, uid(6): float(HOME[0]), uid(7): float(HOME[1]),
        uid(8): float(DELIVERY[0]), uid(9): float(DELIVERY[1]), uid(10): 0,
    })
    component(ACTOR, 'game:'+COMPONENT, {COORDINATES: [], CURSOR: 0})
    create(PLAYER, 'First-person observer', (-4, 0, 7))
    component(PLAYER, 'CharacterController', dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=uid(CAMERA)))
    create(CAMERA, 'Player camera', (0, 1.6, 0), parent=PLAYER)
    component(CAMERA, 'Camera', dict(vertical_fov=70, near=.05, far=100))
    # A separate elevated camera observes the route without patching runtime poses.
    create(OBSERVER, 'Yard overview', (0, 9, 13), rotation=(-.2588190451, 0, 0, .9659258263))
    component(OBSERVER, 'Camera', dict(vertical_fov=50, near=.05, far=100))
    dp = lambda value: dict(unit='dp', value=value)
    percent = lambda value: dict(unit='percent', value=value)
    # Keep the scene visible. Explicit native layout replaces the compatibility
    # menu rather than relying on its large scrollable default container.
    ops.append(dict(op='ui.element.set', id=uid(690), element=dict(parent=None,
        name='Character Yard controls', kind='panel', text='', action=None, visible=True, enabled=True,
        layout=dict(position='absolute', left=dp(0), bottom=dp(0), width=percent(100), height=dp(144),
                    direction='column', align='stretch', gap=4, padding=[12, 16, 12, 16], hit_test='capture'),
        style=dict(background_color='#191519ee', color='#f7e8eb', font_size=14, border_width=0))))
    ops.append(dict(op='ui.element.set', id=uid(699), element=dict(parent=None,
        name='Character Yard title', kind='label', text='Character Yard', action=None, visible=True, enabled=True,
        layout=dict(position='absolute', left=dp(16), top=dp(12), width=dp(240), height=dp(26)),
        style=dict(color='#d46a7e', font_size=20))))
    labels = ((700, 'Courier ready'),
              (701, 'Source motion: holding pose'), (702, 'Save a checkpoint or load your last delivery.'))
    for order, (identity, text) in enumerate(labels):
        ops.append(dict(op='ui.element.set', id=uid(identity), element=dict(parent=uid(690),
            name=text, kind='label', text=text, action=None, visible=True, enabled=True,
            layout=dict(width=percent(100), height=dp(20), shrink=0, order=order),
            style=dict(color='#f7e8eb', font_size=14))))
    ops.append(dict(op='ui.element.set', id=uid(691), element=dict(parent=uid(690),
        name='Delivery actions', kind='panel', text='', action=None, visible=True, enabled=True,
        layout=dict(width=percent(100), height=dp(36), order=3, direction='row', align='stretch',
                    gap=6, padding=[0, 0, 0, 0], hit_test='pass_through', shrink=0),
        style=dict(background_color='#00000000', border_width=0))))
    for order, (action, identity) in enumerate(ACTIONS.items()):
        ops.append(dict(op='ui.element.set', id=uid(identity), element=dict(parent=uid(691),
            name=action.title(), kind='button', text=action.title(), action=action, visible=True, enabled=True,
            layout=dict(width=dp(50), min_width=dp(48), height=dp(36), grow=1, shrink=1, order=order,
                        padding=[0, 4, 0, 4]),
            style=dict(color='#f7e8eb', background_color='#722f37' if action=='dispatch' else '#30252a',
                       border_color='#4f1a23', border_width=1, border_radius=4, font_size=12, text_align='center',
                       hover=dict(background_color='#4f1a23'), focus=dict(border_color='#d46a7e')))))
    return ops


class OwnedGame:
    """Owner used by the launcher and independent contract verifier.

    Commands are issued once with observed guards. A timeout is preserved rather
    than converted into an automatic mutation retry.
    """
    def __init__(self, args, world, label, record, deadline, native, manifest, binary=None):
        self.args, self.world, self.label, self.record = args, Path(world), label, record
        self.deadline, self.native, self.manifest = deadline, native, manifest
        self.session, self.tick, self.revision, self.closed, self.calls = None, 0, 0, False, 0
        self.client = WorldClient.open(str((binary or args.binary).resolve()), native(self.world))

    def rpc(self, method, params=None, error=None, timeout=30):
        remaining = self.deadline-time.monotonic()
        if remaining <= 0: raise TimeoutError('Character Yard operation budget exhausted')
        row = dict(owner=self.label, method=method, params=params or {})
        self.record['calls'].append(row); self.calls += 1
        try: result = self.client.call(method, params or {}, timeout=min(timeout, remaining))
        except RpcError as failure:
            row['error'] = dict(code=failure.code, message=failure.message)
            if error is None or failure.code != error: raise
            return row['error']
        row['result'] = result
        if error is not None: raise AssertionError((method, 'Expected rejection', error, result))
        return result

    def page(self, method, params):
        rows, offset = [], 0
        for _ in range(1024):
            result = self.rpc(method, dict(params, offset=offset, limit=64))
            rows.extend(result['items'])
            next_offset = result['next_offset']
            if next_offset is None: return rows
            if not isinstance(next_offset, int) or next_offset <= offset:
                raise RuntimeError('Asset pagination did not advance.')
            offset = next_offset
        raise RuntimeError('Asset pagination exceeded its traversal budget.')

    def author(self, source):
        if sha(source) != SOURCE_SHA: raise ValueError('Supply the pinned, unmodified RiggedFigure.glb source.')
        self.rpc('component.schema.import', dict(request_id=uuid.uuid4().hex, base_revision=0, manifest=self.manifest))
        imported = self.rpc('asset.import', dict(source=self.native(source)))
        asset = imported['asset']
        clips = self.page('asset.inspect', dict(asset=asset, section='animations'))
        if len(clips) != 1 or clips[0]['duration'] != 1.25: raise ValueError('Unexpected source animation profile.')
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=1, ops=fixture(asset, clips[0]['index'])))
        navigation = self.rpc('world.navigation.bake', dict(revision=2, profile=dict(radius=.3, height=1.8, climb=0)))['asset']
        provenance = self.rpc('asset.provenance.create', dict(record=dict(
            format='poima.asset-provenance', version=1, asset=asset, kind='model',
            title='Rigged Figure', creator='Cesium', source=SOURCE_URL,
            license=dict(identifier='CC-BY-4.0', notice=(
                'Rigged Figure, Copyright 2017 Cesium. Licensed under Creative Commons Attribution 4.0 International '
                '(https://creativecommons.org/licenses/by/4.0/). Original GLB imported without source edits; '
                'Poima cooks it and places its retained rig beneath a rotated movement attachment.')))))
        self.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=2, ops=[
            dict(op='navigation.set', asset=navigation),
            dict(op='asset.provenance.set', asset=asset, records=[provenance['record']])]))
        self.authored = dict(revision=3, asset=asset, rig=uid(RIG),
            nodes=[node_id(uid(RIG), index) for index in range(22)], imported=imported, navigation=navigation)
        return self.authored

    def start(self):
        if self.session: self.rpc('runtime.stop', dict(session_id=self.session))
        self.session, self.tick, self.revision = uuid.uuid4().hex, 0, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=self.rpc('world.inspect')['revision']))
        self.step(60)

    def config(self):
        if self.args.descriptor: return dict(descriptor=self.native(self.args.descriptor))
        return dict(hostfxr=self.native(self.args.hostfxr), bridge=self.native(self.args.bridge),
                    assembly=self.native(self.args.assembly), type=TYPE)

    def load(self, error=None):
        method = 'runtime.gameplay.load_native' if self.args.descriptor else 'runtime.gameplay.load'
        result = self.rpc(method, dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=self.module()['revision'], **self.config()), error)
        if error is None:
            self.revision = result['revision']
            expected = 'native_aot' if self.args.descriptor else 'coreclr'
            if result['module']['backend'] != expected: raise AssertionError(result)
        return result

    def inspect(self): return self.rpc('runtime.inspect', dict(session_id=self.session))
    def module(self): return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))
    def values(self): return self.module()['module']['values']
    def entity(self, identity=ACTOR): return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=uid(identity) if isinstance(identity, int) else identity))
    def route(self): return self.rpc('runtime.component.get', dict(session_id=self.session, tick=self.tick, id=uid(ACTOR), type=COMPONENT))
    def visual_config(self): return self.rpc('runtime.component.get', dict(session_id=self.session, tick=self.tick, id=uid(ACTOR), type=CONFIG))

    def step(self, count=1, error=None):
        result = self.rpc('runtime.step', dict(request_id=uuid.uuid4().hex, session_id=self.session,
            expected_tick=self.tick, ticks=count), error)
        if error is None: self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def control(self, action):
        state, module = self.inspect(), self.module()
        result = self.rpc('runtime.ui.activate', dict(request_id=uuid.uuid4().hex, session_id=self.session,
            expected_tick=self.tick, expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=module['revision'], expected_structure_revision=state['structure_revision'], id=uid(ACTIONS[action])))
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def snapshot(self):
        state, module, route = self.inspect(), self.module(), self.route()
        return normalized(dict(tick=self.tick, entity=self.entity(), rig=self.entity(RIG),
            nodes=[self.entity(node_id(uid(RIG), index)) for index in range(22)], route=route,
            config=self.visual_config(), values=module['module']['values'], backend=module['module']['backend'],
            gameplay_revision=module['revision'], revisions=dict(component_revision=route['component_revision'],
                **{key:state[key] for key in ('structure_revision', 'ui_revision', 'control_sequence')}),
            ui=self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick))))

    def saves(self, path):
        path = Path(path); path.mkdir(exist_ok=True)
        self.configuration = self.rpc('save.configure', dict(request_id=uuid.uuid4().hex,
            expected_generation=0, root=self.native(path)))['generation']

    def write(self, slot):
        state, game = self.inspect(), self.module()
        return self.rpc('save.write', dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
            slot=slot, expected_generation=0, session_id=self.session, expected_tick=self.tick,
            expected_gameplay_revision=game['revision'], expected_component_revision=self.route()['component_revision'],
            expected_structure_revision=state['structure_revision'], expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence']))

    def restore(self, slot):
        state, game = (self.inspect(), self.module()) if self.session else (None, None)
        params = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
            slot=slot, expected_generation=1, revision=self.rpc('world.inspect')['revision'],
            expected_session_id=self.session, expected_tick=self.tick if self.session else None,
            expected_gameplay_revision=game['revision'] if game else None,
            expected_component_revision=self.route()['component_revision'] if state else None,
            expected_structure_revision=state['structure_revision'] if state else None,
            expected_ui_revision=state['ui_revision'] if state else None,
            expected_control_sequence=state['control_sequence'] if state else None,
            new_session_id=uuid.uuid4().hex, gameplay=self.config())
        result = self.rpc('save.load', params)
        self.session, self.tick = params['new_session_id'], result['tick']
        self.revision = self.module()['revision']
        return result

    def close(self):
        if self.closed: return
        self.closed = True
        owner = dict(label=self.label, calls=self.calls)
        try: self.client.close()
        except BaseException as error:
            owner['cleanup_error'] = repr(error)
            raise
        finally:
            owner['exit_code'] = self.client.transport.returncode
            self.record['owners'].append(owner)
        code = owner['exit_code']
        if code != 0: raise RuntimeError(('Native owner cleanup failed', self.label, code))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source', 'manifest', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    for name in ('hostfxr', 'bridge', 'assembly', 'descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, default=-1)
    args = parser.parse_args()
    if bool(args.descriptor) == bool(args.assembly): parser.error('Supply assembly or descriptor, exclusively.')
    if not args.descriptor and not (args.hostfxr and args.bridge): parser.error('CoreCLR requires hostfxr and bridge.')
    if args.output.exists(): parser.error('Use a new output directory to preserve existing worlds and saves.')
    args.output.mkdir(parents=True)

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() if args.windows_interop and os.name != 'nt' else value

    record = dict(calls=[], owners=[])
    game = OwnedGame(args, args.output/'world.json', 'interactive', record, time.monotonic()+86400,
                     native, json.loads(args.manifest.read_text(encoding='utf-8')))
    try:
        game.author(args.source); game.start(); game.load(); game.saves(args.output/'saves')
        print('WASD move; mouse look; Tab releases cursor. Dispatch routes the courier. Gesture replays the original arm motion (not a walking clip).', flush=True)
        game.rpc('runtime.play', dict(request_id=uuid.uuid4().hex, session_id=game.session,
            expected_tick=game.tick, controller=uid(PLAYER), camera=uid(CAMERA), mode='interactive',
            width=1280, height=720, gpu=args.gpu), timeout=86400)
    finally:
        try: game.close()
        finally:
            (args.output/'commands.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__': main()
