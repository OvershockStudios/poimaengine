#!/usr/bin/env python3
"""Author and launch Locomotion Yard through the native world service.

The original licensed FBX source directory is supplied by the caller. This launcher does not
download content, edit source files, synthesize motion or implement navigation. Import uses an explicit retarget policy.
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

TYPE = 'Poima.Examples.LocomotionYardGame'
ACTOR, RIG, PLAYER, CAMERA, OBSERVER = 300, 400, 100, 101, 102
CONFIG = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb75'
COMPONENT = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76'
SOURCE_SHA = {
    'Model/characterMedium.fbx': '18835fef534eede635b081ee7fe647d01a885550a591d2e6bf071010906167d8',
    'Animations/run.fbx': 'e635461fc8dace85ec67a7f7941e949a7c3f108b51ae4d2da1557e6e01749df8',
    'Animations/idle.fbx': 'c8a24e0294376ee5a195c56752a13310e1c0b5f8588a4db50e094120e3e4cc74',
    'License.txt': '68280323c6dca1f532c71fb248a6f344abed39a574278a2edfa27801aea3d0cd',
}
SOURCE_URL = 'https://kenney.nl/assets/animated-characters-protagonists'
NOMINAL_RUN_SPEED, SLOWDOWN_DISTANCE, CAPSULE_RADIUS = 3.0, .8, .42
HOME, DELIVERY = (-4, 0), (4, 0)
COVER_HALF = (.5, 1.5, 3)
BLEND_TICKS, IDLE_LEAD_TICKS = 12, 90


def uid(value): return f'{value:032x}'
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def node_id(rig, index):
    return hashlib.sha256(f'poima.instance.v1/{rig}/node/{index}'.encode()).hexdigest()[:32]


COORDINATES, CURSOR = uid(1), uid(2)
ACTIONS = {name: 800+index for index, name in enumerate(
    ('dispatch', 'pause', 'resume', 'save', 'load', 'status'))}


def normalized(value):
    if isinstance(value, dict):
        return {key: normalized(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list): return [normalized(item) for item in value]
    return value


def fixture(asset, idle_clip, run_clip, capsule, attachment):
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
    component(ACTOR, 'CharacterController', dict(radius=capsule['radius'], height=capsule['height'], speed=4, jump_speed=5, camera=None))
    ops.append(dict(op='asset.instantiate', id=uid(RIG), name='Kenney imported visual',
                    asset=asset, parent=uid(ACTOR)))
    component(RIG, 'Transform', attachment)
    component(ACTOR, 'game:'+CONFIG, {
        uid(1): uid(RIG), uid(2): idle_clip, uid(3): run_clip, uid(4): BLEND_TICKS,
        uid(5): IDLE_LEAD_TICKS, uid(6): float(HOME[0]), uid(7): float(HOME[1]),
        uid(8): float(DELIVERY[0]), uid(9): float(DELIVERY[1]), uid(10): NOMINAL_RUN_SPEED,
        uid(11): SLOWDOWN_DISTANCE,
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
        name='Locomotion Yard controls', kind='panel', text='', action=None, visible=True, enabled=True,
        layout=dict(position='absolute', left=dp(0), bottom=dp(0), width=percent(100), height=dp(144),
                    direction='column', align='stretch', gap=4, padding=[12, 16, 12, 16], hit_test='capture'),
        style=dict(background_color='#191519ee', color='#f7e8eb', font_size=14, border_width=0))))
    ops.append(dict(op='ui.element.set', id=uid(699), element=dict(parent=None,
        name='Locomotion Yard title', kind='label', text='Locomotion Yard', action=None, visible=True, enabled=True,
        layout=dict(position='absolute', left=dp(16), top=dp(12), width=dp(240), height=dp(26)),
        style=dict(color='#d46a7e', font_size=20))))
    labels = ((700, 'Courier ready'),
              (701, 'Source idle: advancing'), (702, 'Save a checkpoint or load your last delivery.'))
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
        if remaining <= 0: raise TimeoutError('Locomotion Yard operation budget exhausted')
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

    def author(self, source_directory):
        source_directory = Path(source_directory).resolve(strict=True)
        source_files = {name: source_directory/name for name in SOURCE_SHA}
        if not all(path.is_file() and sha(path)==SOURCE_SHA[name] for name,path in source_files.items()):
            raise ValueError('Supply unchanged Kenney Protagonists 1.1 base, Run, Idle and License.txt files.')
        self.rpc('component.schema.import', dict(request_id=uuid.uuid4().hex, base_revision=0, manifest=self.manifest))
        base_path=source_files['Model/characterMedium.fbx']
        base_info=self.rpc('asset.source.inspect',dict(source=self.native(base_path)))
        base_nodes=self.page('asset.source.inspect',dict(source=self.native(base_path),section='nodes',expected_model_sha256=base_info['model_sha256']))
        selections=[];profiles={}
        for name in ('run','idle'):
            path=source_files['Animations/'+name+'.fbx']
            info=self.rpc('asset.source.inspect',dict(source=self.native(path)))
            guard=dict(source=self.native(path),expected_model_sha256=info['model_sha256'])
            clips=self.page('asset.source.inspect',dict(guard,section='animations'))
            nodes=self.page('asset.source.inspect',dict(guard,section='nodes'))
            references=[c for c in clips if c['name']=='Root|0.Targeting Pose']
            motions=[c for c in clips if c['name']=='Root|'+name.title()]
            selected=[n['index'] for n in nodes if n['name'] in ('HipsCtrl','Hips')]
            if len(references)!=1 or len(motions)!=1 or len(selected)!=2:
                raise ValueError('Original donor take or pelvis ancestry profile differs; inspect before importing.')
            policy=dict(policy='reference-rotation-v1',source_pose=dict(kind='sample',clip=references[0]['index'],time=0),target_pose=dict(kind='rest'),
                positions=dict(kind='reference_delta',nodes=sorted(selected),scale=1),scales='target_reference',
                expected_source_model_sha256=info['model_sha256'],expected_target_model_sha256=base_info['model_sha256'])
            selections.append(dict(source=self.native(path),clip=motions[0]['index'],name=name.title(),retarget=policy))
            profiles[name]=dict(source=str(path),info=info,reference=references[0],motion=motions[0],nodes=nodes,policy=policy)
        baseline=self.rpc('asset.import',dict(source=self.native(base_path)))
        original_points=[]
        for node in base_nodes:
            for primitive in node['primitives']:
                original_points.extend(v['world_position'] for v in self.page('asset.animation.sample',dict(asset=baseline['asset'],section='vertices',node=node['index'],primitive=primitive,time=0)))
        if not original_points:raise ValueError('Original body has no observable vertices.')
        bounds=dict(min=[min(p[k] for p in original_points) for k in range(3)],max=[max(p[k] for p in original_points) for k in range(3)])
        capsule=dict(radius=CAPSULE_RADIUS,height=bounds['max'][1]-bounds['min'][1])
        if not 2*CAPSULE_RADIUS < capsule['height'] <= 4:raise ValueError('Unexpected original standing height.')
        attachment=dict(position=[0,-bounds['min'][1],0],rotation=[0,1,0,0],scale=[1,1,1])
        imported=self.rpc('asset.import',dict(source=self.native(base_path),animations=selections))
        asset=imported['asset'];clips=self.page('asset.inspect',dict(asset=asset,section='animations'))
        by_name={c['name']:c for c in clips}
        if set(by_name)!={'Run','Idle'} or len(clips)!=2 or imported['nodes']!=len(base_nodes):
            raise ValueError('Cooked character/clip identities differ from inspected inputs.')
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,
            ops=fixture(asset,by_name['Idle']['index'],by_name['Run']['index'],capsule,attachment)))
        navigation=self.rpc('world.navigation.bake',dict(revision=2,profile=dict(**capsule,climb=0)))['asset']
        provenance=self.rpc('asset.provenance.create',dict(record=dict(format='poima.asset-provenance',version=1,asset=asset,kind='model',
            title='Animated Characters Protagonists — body, Run and Idle',creator='Kenney',source=SOURCE_URL,
            license=dict(identifier='CC0-1.0',notice='Kenney Animated Characters Protagonists 1.1, CC0-1.0 (https://creativecommons.org/publicdomain/zero/1.0/). Original base and Run/Idle sources cooked with explicit orientation retargeting and HipsCtrl/Hips position deltas; target geometry and binds retained.'))))
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=2,
            ops=[dict(op='navigation.set',asset=navigation),dict(op='asset.provenance.set',asset=asset,records=[provenance['record']])]))
        self.authored=dict(revision=3,asset=asset,baseline=baseline['asset'],rig=uid(RIG),nodes=[node_id(uid(RIG),n['index']) for n in base_nodes],
            imported=imported,navigation=navigation,source_profiles=profiles,base_nodes=base_nodes,base_info=base_info,
            base_bounds=bounds,attachment=attachment,capsule=capsule,clips=by_name,source_files={name:sha(path) for name,path in source_files.items()})
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
            nodes=[self.entity(node_id(uid(RIG), index)) for index in range(61)], route=route,
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
    for name in ('binary', 'source-directory', 'manifest', 'output'):
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
        game.author(args.source_directory); game.start(); game.load(); game.saves(args.output/'saves')
        print('WASD move; mouse look; Tab releases cursor. Dispatch routes the courier. Physics owns travel; original Idle/Run clips drive its retained visual rig.', flush=True)
        game.rpc('runtime.play', dict(request_id=uuid.uuid4().hex, session_id=game.session,
            expected_tick=game.tick, controller=uid(PLAYER), camera=uid(CAMERA), mode='interactive',
            width=1280, height=720, gpu=args.gpu), timeout=86400)
    finally:
        try: game.close()
        finally:
            (args.output/'commands.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__': main()
