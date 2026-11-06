#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author a fresh Physical Character Yard through Poima's guarded world service.

Requires an existing simulation-enabled native CLI and the original pinned
Knight GLB and license. Imports the complete library; builds and downloads
nothing, starts no simulation and opens no window. Run with native Python on
the same operating system as the selected CLI.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

ACTOR, PLAYER, CAMERA, TARGET_CAMERA, RIG = 1, 2, 3, 4, 100
INPUT_PATH = 'settings/physical-character.poima-input.json'
GAME_TYPE = 'Poima.Examples.PhysicalCharacterGame'
MODULE = 'poima.example.physical-character'
CHAINS = ((1, 25, 24, 23), (2, 29, 28, 27))
# Original rest-geometry sole calibration, shared with the documented
# Ground Contacts / Foot Planting original-Knight workflows. These are authored
# metric offsets/axes, not synthesized motion or modified source animation.
SOLES = (
    dict(sole_offset=[3.518225173502198e-9, .17351426574300935, .02932634479185336],
         sole_up=[6.943410503710568e-8, -.7198726005913565, -.6941062158760954],
         sole_forward=[1.3696781462479196e-7, .6941062158760974, -.719872600591345]),
    dict(sole_offset=[-4.364844294028667e-9, .17351426329625338, .0293263467672322],
         sole_up=[1.021392211036242e-7, -.7198726005913618, -.6941062158760859],
         sole_forward=[1.054333437946812e-7, .6941062158760934, -.7198726005913542]),
)


def uid(value):
    return f'{value:032x}'


def need(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    with Path(path).open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(bytes=Path(path).stat().st_size, sha256=digest)


def original(path, expected):
    info = path.lstat()
    need(stat.S_ISREG(info.st_mode) and not path.is_symlink() and
         not getattr(info, 'st_file_attributes', 0) & 0x400,
         'An ordinary original file is required: '+str(path))
    need(info.st_size == expected['bytes'], 'Original file length differs: '+str(path))
    pin = identity(path)
    need(pin['sha256'] == expected['sha256'], 'Original file hash differs: '+str(path))
    return pin


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def transform(position=(0., 0., 0.), rotation=(0., 0., 0., 1.), scale=(1., 1., 1.)):
    return dict(position=list(position), rotation=list(rotation), scale=list(scale))


def component(entity, kind, value):
    return dict(op='component.set', id=uid(entity) if isinstance(entity, int) else entity,
                type=kind, value=value)


def grounded_chains():
    return [dict(slot=slot, root=root, middle=middle, end=end, target=[0., 0., 0.],
        pole=[0., 1., 1.], space='rig', metric='near-uniform-v1',
        grounding=dict(copy.deepcopy(SOLES[i]), reference_height=0., plant_lock=True,
            plant_height=.03, lift_height=.15, probe_above=.3, probe_below=.6,
            max_correction=.4, max_slope_degrees=55.))
        for i, (slot, root, middle, end) in enumerate(CHAINS)]


def drive_profile(base, library):
    """Admit complete authored controls; never clamp or replace source geometry."""
    need(type(library) is dict and set(library) == {'format', 'version', 'joints', 'presets', 'provenance'} and
         library['format'] == 'poima.physical-character-drive-poses' and
         type(library['version']) is int and library['version'] == 1 and
         type(library['provenance']) is dict, 'Unsupported complete drive pose library.')
    names = [joint['name'] for joint in base['joints']]
    need(type(library['joints']) is list and len(library['joints']) == len(names) == 14,
         'Drive library must name all fourteen original joints.')
    def finite(value, low, high):
        return type(value) in (int, float) and math.isfinite(value) and low <= value <= high
    result = copy.deepcopy(base)
    for index, row in enumerate(library['joints']):
        need(type(row) is dict and set(row) == {'name', 'drive', 'default_strength'} and row['name'] == names[index],
             'Drive joint order differs from the original physical profile.')
        gain = row['drive']
        need(type(gain) is dict and set(gain) == {'stiffness', 'damping', 'max_torque'} and
             finite(gain['stiffness'], 0, 1e6) and gain['stiffness'] > 0 and
             finite(gain['damping'], 0, 1e4) and finite(gain['max_torque'], 0, 1e5) and gain['max_torque'] > 0,
             'Invalid bounded authored joint-drive gains.')
        need(finite(row['default_strength'], 0, 1) and row['default_strength'] == .65,
             'Default strength differs from the matching compiled sample.')
        need('drive' not in result['joints'][index], 'Original profile must remain drive-free.')
        result['joints'][index]['drive'] = copy.deepcopy(gain)
    need(type(library['presets']) is list and len(library['presets']) == 2, 'Exactly Relaxed and Guard presets are required.')
    for preset, name in zip(library['presets'], ('Relaxed', 'Guard')):
        need(type(preset) is dict and set(preset) == {'id', 'name', 'targets'} and
             type(preset['id']) is int and preset['id'] == (0 if name == 'Relaxed' else 1) and preset['name'] == name and
             type(preset['targets']) is list and len(preset['targets']) == 14, 'Complete named pose membership differs.')
        for index, target in enumerate(preset['targets']):
            need(type(target) is dict and set(target) == {'joint', 'target_rotation'} and target['joint'] == names[index],
                 'Every pose must retain complete original joint order.')
            q = target['target_rotation']
            need(type(q) is list and len(q) == 4 and all(finite(x, -1.000001, 1.000001) for x in q) and
                 abs(math.hypot(*q)-1) <= 1e-6 and q[3] >= 0, 'Authored targets must be canonical finite unit XYZW quaternions.')
            # Independent admission in the authored joint chart. Keep the exact
            # original doubles: no target normalization, projection or clamping.
            chart = math.hypot(q[0], q[3]); need(chart > 1e-4, 'Unsupported singular target chart.')
            twist = 2*math.atan2(q[0], q[3]); limits = base['joints'][index]['limits']
            if limits['kind'] == 'hinge':
                need(abs(q[1]) <= 1e-8 and abs(q[2]) <= 1e-8 and
                     limits['minimum']-1e-10 <= twist <= limits['maximum']+1e-10, 'Authored hinge target exceeds its exact axis/limits.')
            else:
                y = (q[3]*q[1]-q[0]*q[2])/chart/math.sin(limits['swing_y']/2)
                z = (q[3]*q[2]+q[0]*q[1])/chart/math.sin(limits['swing_z']/2)
                need(y*y+z*z <= 1+1e-10 and limits['twist_min']-1e-10 <= twist <= limits['twist_max']+1e-10,
                     'Authored target exceeds the original swing/twist limits.')
    need('animation_drive' not in result, 'Original profile must remain target-player-free.')
    result['animation_drive'] = dict(limit_policy='fit')
    return result


def ui_operations():
    dp = lambda value: dict(unit='dp', value=value)
    percent = lambda value: dict(unit='percent', value=value)
    result = []
    # The bottom panel leaves the aim line and target visible. Its children are
    # ordinary native UI controls, equally inspectable by agents and the player.
    def add(number, kind, text, layout, *, parent=500, action=None, visible=True):
        style = dict(color='#f7e8eb', font_size=14, background_color='#00000000')
        if kind == 'button':
            style.update(background_color='#722f37', border_color='#4f1a23',
                border_width=1, border_radius=4, text_align='center',
                hover=dict(background_color='#4f1a23'), focus=dict(border_color='#d46a7e'))
        if number == 500:
            style.update(background_color='#20191dee', border_width=0)
        if number == 501:
            style.update(color='#d46a7e', font_size=20)
        result.append(dict(op='ui.element.set', id=uid(number), element=dict(
            parent=None if parent is None else uid(parent), name=text or 'Yard controls',
            kind=kind, text=text, action=action, visible=visible, enabled=True,
            layout=layout, style=style)))
    add(500, 'panel', '', dict(position='absolute', left=dp(0), bottom=dp(0),
        width=percent(100), height=dp(308), hit_test='capture'), parent=None)
    add(501, 'label', 'PHYSICAL CHARACTER / BEGIN', dict(position='absolute',
        left=dp(16), top=dp(10), width=dp(550), height=dp(28)))
    add(502, 'label', 'Original Knight / ready', dict(position='absolute',
        left=dp(16), top=dp(42), width=dp(1000), height=dp(42)))
    # Begin/Menu share a slot. Resume has its own slot because a restored
    # live game is paused by the player and needs an explicit resume action.
    buttons = ((503, 'Begin', 'begin', 16, True), (504, 'Menu', 'menu', 16, False),
        (507, 'Resume', 'resume', 116, False), (505, 'Save', 'save', 216, False),
        (506, 'Load', 'load', 316, False), (508, 'Retry', 'retry', 416, False),
        (510, 'Target motion', 'target-motion', 516, False))
    for number, text, action, left, visible in buttons:
        add(number, 'button', text, dict(position='absolute', left=dp(left), top=dp(84),
            width=dp(150 if number == 510 else 92), height=dp(32)),
            action=action, visible=visible)
    pose_buttons = ((514, 'Hold pose', 'pose-hold', 16, 92),
        (515, 'Release', 'pose-release', 116, 92),
        (516, 'Pose: Relaxed', 'pose-preset', 216, 144),
        (517, 'Joint: shoulder_l', 'pose-joint', 368, 192),
        (518, 'Request: 65%', 'pose-strength', 568, 138))
    for number, text, action, left, width in pose_buttons:
        add(number, 'button', text, dict(position='absolute', left=dp(left), top=dp(120),
            width=dp(width), height=dp(30)), action=action, visible=False)
    target_buttons = ((520, 'Speed: 1x', 'target-speed', 16, 160),
        (521, 'Restart clip', 'target-restart', 184, 140),
        (522, 'Next: Inertial', 'target-transition', 332, 205),
        (523, 'Next blend: 12 ticks', 'target-blend', 545, 180))
    for number, text, action, left, width in target_buttons:
        add(number, 'button', text, dict(position='absolute', left=dp(left), top=dp(154),
            width=dp(width), height=dp(30)), action=action, visible=False)
    add(519, 'label', 'Pose controls hold relative joint angles. Gravity still applies.',
        dict(position='absolute', left=dp(16), top=dp(188), width=dp(1190), height=dp(22)), visible=False)
    add(524, 'label', 'Live clips supply joint targets; they do not balance the character.',
        dict(position='absolute', left=dp(16), top=dp(210), width=dp(1190), height=dp(42)), visible=False)
    add(509, 'label', 'Begin creates the retry checkpoint in this launch\u2019s save folder.',
        dict(position='absolute', left=dp(16), top=dp(254), width=dp(1150), height=dp(22)))
    add(513, 'label', 'WASD / mouse / click fire / Space jump / Tab cursor. Strength: 0/25/50/75/100%. Loaded saves pause; Resume continues.',
        dict(position='absolute', left=dp(16), top=dp(278), width=dp(1190), height=dp(22)))
    # Native text, no image asset. This marker is screen center, not target aim.
    add(512, 'panel', '', dict(position='absolute', left=dp(0), top=dp(0),
        width=percent(100), height=percent(100), align='center', justify='center',
        direction='column', hit_test='pass_through'), parent=None)
    add(511, 'label', '+', dict(width=dp(16), height=dp(24)), parent=512)
    return result


def scene_operations(asset, profile, idle):
    ops = []
    def create(number, name, local=None, parent=None):
        ops.append(dict(op='entity.create', id=uid(number), name=name,
                        parent=None if parent is None else uid(parent)))
        ops.append(component(number, 'Transform', local or transform()))
    def controller(number, camera, speed):
        ops.append(component(number, 'CharacterController', dict(radius=.3, height=1.8,
            speed=speed, jump_speed=5., camera=uid(camera),
            stance=dict(crouched_height=1., crouched_speed=1., camera_drop=.5),
            locomotion=dict(step_height=.4, ground_snap=.5, max_slope_degrees=45.))))
    # Original Knight faces +Z; controller forward is -Z. The fixed wrapper
    # yaw maps source facing to controller travel (as in Locomotion Yard).
    # Starting the actor at the same half-turn leaves the visible character
    # facing the player at +Z without modifying imported source transforms.
    create(ACTOR, 'Knight movement owner', transform(rotation=(0., 1., 0., 0.)))
    controller(ACTOR, TARGET_CAMERA, 2.)
    create(TARGET_CAMERA, 'Knight controller camera', transform((.05, 1.6, .02)), ACTOR)
    ops.append(component(TARGET_CAMERA, 'Camera', dict(vertical_fov=70, near=.05, far=100)))
    ops.append(dict(op='asset.instantiate', id=uid(RIG), name='Original Knight library',
                    asset=asset, parent=uid(ACTOR)))
    ops.append(component(RIG, 'Transform', transform(rotation=(0., 1., 0., 0.))))
    ops.append(component(RIG, 'AnimationRig', dict(asset=asset, clip=idle, time=0.,
        speed=1., loop=True, playing=False, ik=grounded_chains())))
    ops.append(component(RIG, 'PhysicalRig', profile))
    create(PLAYER, 'Player', transform((0., 0., 6.)))
    controller(PLAYER, CAMERA, 4.)
    create(CAMERA, 'Player camera', transform((0., 1.6, 0.)), PLAYER)
    ops.append(component(CAMERA, 'Camera', dict(vertical_fov=70, near=.05, far=100)))
    create(10, 'Solid floor', transform((0., -.25, 0.)))
    ops.append(component(10, 'BoxCollider', dict(half_extents=[12., .25, 12.],
        motion='static', mass=1., friction=.65, restitution=0.)))
    create(750, 'Visible floor', transform((0., -.25, 0.), scale=(24., .5, 24.)))
    ops.append(component(750, 'MeshRenderer', dict(primitive='box', visible=True,
        albedo=[.25, .27, .29])))
    create(701, 'Yard environment')
    ops.append(component(701, 'LightingEnvironment', dict(ambient=[.12, .12, .12],
        exposure=1., shadow_resolution=1024, shadow_filter='pcf3_fractional')))
    # Quaternion product yaw(.4) * pitch(-.8), as in the original specimen.
    rotation = [math.cos(.2)*math.sin(-.4), math.sin(.2)*math.cos(.4),
                -math.sin(.2)*math.sin(-.4), math.cos(.2)*math.cos(.4)]
    create(702, 'Yard sun', transform((0., 4., 2.), rotation))
    ops.append(component(702, 'Light', dict(kind='directional', color=[1., .97, .92],
        intensity=2.5, enabled=True, shadow=dict(enabled=True, near=.05, distance=30.,
            bias=.0001, normal_bias=.005))))
    return ops + ui_operations()


def prepare(args):
    args.output = args.output.absolute()
    need(not args.output.exists() and not args.output.is_symlink() and
         args.output.parent.is_dir(), 'Output must be new with an existing parent.')
    args.binary = args.binary.resolve(strict=True)
    need(args.binary.is_file(), 'Select an existing native Poima executable.')
    need(os.name == 'nt' or args.binary.suffix.lower() != '.exe',
         'Run Windows Python with Windows poima.exe; cross-OS path guessing is unsupported.')
    manifest = json.loads((HERE/'source-manifest.json').read_bytes())
    source_pin = original(args.source, manifest['model'])
    license_pin = original(args.license, manifest['license_file'])
    base = json.loads((HERE/'profiles/knight.json').read_bytes())
    need(len(base['parts']) == 15 and len(base['joints']) == 14,
         'Expected the original fifteen-part Knight profile.')
    pose_path = HERE/'drive-poses.json'
    need(pose_path.stat().st_size <= 64*1024, 'Bounded authored drive library required.')
    pose_pin = identity(pose_path)
    poses = json.loads(pose_path.read_bytes())
    profile = drive_profile(base, poses)
    if getattr(args, 'velocity_tracking', False):
        profile['animation_drive']['velocity_tracking'] = True
    need(poses['provenance']['original_profile'] == identity(HERE/'profiles/knight.json') and
         poses['provenance']['original_model'] == source_pin, 'Pose reference identities differ from the original profile/model.')
    profile.update(pose_frame='skeletal_bounded_scale', max_scale_deviation=.00001,
        procedural_pose='grounded_animated_only',
        controller_handoff=dict(actor=uid(ACTOR), follow='translate_keep_heading'))
    args.output.mkdir()
    for name in ('content', 'settings', 'authoring'):
        (args.output/name).mkdir()
    source = args.output/'content/Knight.glb'
    notice = args.output/'content/LICENSE.txt'
    shutil.copyfile(args.source, source)
    shutil.copyfile(args.license, notice)
    original(source, manifest['model']); original(notice, manifest['license_file'])
    write_json(args.output/'content/source-manifest.json', manifest)
    write_json(args.output/'content/physical-profile.json', profile)
    shutil.copyfile(pose_path, args.output/'content/drive-poses.json')
    need(identity(args.output/'content/drive-poses.json') == pose_pin, 'Copied authored pose library changed.')
    (args.output/'content/NOTICE.txt').write_text(
        'KayKit Adventurers 1.0 Knight by Kay Lousberg, CC0-1.0.\n'
        +manifest['repository']+'/tree/'+manifest['revision']+'\n'
        'Original model, all 76 animation takes and embedded artwork retained unchanged.\n'
        'Original license: LICENSE.txt. Physical proxy/sole calibration: Poima Physical Character Yard.\n',
        encoding='utf-8')
    world = args.output/'world.json'
    report = dict(format='poima.physical-character-preparation.v1', passed=False,
        calls=[], errors=[], inputs=dict(source=source_pin, license=license_pin,
            binary=identity(args.binary), recipe=identity(Path(__file__)),
            source_manifest=identity(HERE/'source-manifest.json'),
            profile=identity(HERE/'profiles/knight.json'), drive_poses=pose_pin), profile=profile, drive_pose_library=poses,
        scope='Native authoring only; no simulation, compiled gameplay or rendered qualification.')
    client = None
    deadline = time.monotonic()+180
    try:
        client = WorldClient.open(str(args.binary), str(world),
            trace_directory=str(args.output/'authoring/stdio'), close_timeout=5)
        def call(method, params=None):
            remaining = deadline-time.monotonic()
            need(remaining > 0, 'Authoring exceeded its 180-second budget; inspect retained calls.')
            row = dict(method=method, params=params or {})
            report['calls'].append(row)
            try:
                row['result'] = client.call(method, row['params'], timeout=min(45, remaining))
            except RpcError as error:
                row['error'] = dict(code=error.code, message=error.message, data=error.data)
                raise
            return row['result']
        def page(method, params):
            rows, offset = [], 0
            for _ in range(8):
                result = call(method, dict(params, offset=offset, limit=64))
                rows.extend(result['items'])
                next_offset = result['next_offset']
                if next_offset is None:
                    return rows
                need(type(next_offset) is int and next_offset > offset,
                     'Native asset traversal did not advance.')
                offset = next_offset
            raise ValueError('Asset traversal exceeded the complete-library bound.')
        def transact(ops):
            before = call('world.inspect')
            result = call('world.transact', dict(request_id=uuid.uuid4().hex,
                base_revision=before['revision'], ops=ops))
            need(result['committed'] is True, 'Native authoring transaction was not committed.')
            return result['revision']
        before = call('world.inspect')
        need(before['revision'] == 0 and before['entity_count'] == 0, 'Expected a fresh empty world.')
        info = call('asset.source.inspect', dict(source=str(source)))
        guard = dict(source=str(source), expected_model_sha256=info['model_sha256'])
        nodes = page('asset.source.inspect', dict(guard, section='nodes'))
        clips = page('asset.source.inspect', dict(guard, section='animations'))
        need(len(nodes) == 57 and len(clips) == 76 and sum(c['channels'] for c in clips) == 8712,
             'The pinned complete Knight library did not survive source inspection.')
        selected = {}
        for name, index in (('Idle', 36), ('Running_A', 48)):
            found = [clip for clip in clips if clip['name'] == name]
            need(len(found) == 1 and found[0]['index'] == index,
                 'Original take identity differs: '+name)
            selected[name] = found[0]
        imported = call('asset.import', guard)
        asset = imported['asset']
        need(asset == info['model_sha256'] and
             (imported['nodes'], imported['animations'], imported['skins'], imported['primitives']) == (57, 76, 1, 15),
             'Import must retain the entire pinned character, skin and take library.')
        cooked_clips = page('asset.inspect', dict(asset=asset, section='animations'))
        need(cooked_clips == clips, 'Cooked clips differ from the inspected original library.')
        revision = transact(scene_operations(asset, profile, selected['Idle']['index']))
        # Resolve identities from real native instantiation. No hash-derived node
        # IDs, guessed bone transforms or rewriting imported nodes is necessary.
        query = call('entity.query', dict(revision=revision, component='RigNode', limit=64))
        need(query['next_after'] is None and len(query['entities']) == 57,
             'Expected the complete instantiated 57-node hierarchy.')
        mapping = {}
        for row in query['entities']:
            observed = call('entity.get', dict(id=row['id'], revision=revision))
            value = observed['value']; binding = value['components']['RigNode']
            need(binding['rig'] == uid(RIG) and type(binding['node']) is int and
                 binding['node'] not in mapping, 'Unexpected or duplicated source-node binding.')
            mapping[binding['node']] = dict(node=binding['node'], entity=row['id'], value=value)
        need(sorted(mapping) == list(range(57)), 'Original source-node membership changed.')
        hit_ops, hit_regions = [], []
        for index, part in enumerate(profile['parts']):
            number = 2000+index; parent = mapping[part['node']]['entity']
            local = dict(copy.deepcopy(part['body_in_bone']), scale=[1., 1., 1.])
            shape = copy.deepcopy(part['shape']); shape['shape'] = shape.pop('kind')
            region = dict(owner=uid(ACTOR), name=part['name'], enabled=True,
                          center=[0., 0., 0.], **shape)
            hit_ops.extend([dict(op='entity.create', id=uid(number), name=part['name']+' hit region', parent=parent),
                component(number, 'Transform', local), component(number, 'HitRegion', region)])
            hit_regions.append(dict(entity=uid(number), parent=parent, part=part['name'],
                node=part['node'], transform=local, region=region))
        provenance = call('asset.provenance.create', dict(record=dict(
            format='poima.asset-provenance', version=1, asset=asset, kind='model',
            title='KayKit Adventurers 1.0 Knight — original complete library', creator=manifest['creator'],
            source=manifest['repository']+'/tree/'+manifest['revision'],
            license=dict(identifier=manifest['license'], notice=(args.output/'content/NOTICE.txt').read_text(encoding='utf-8')))))
        hit_ops.append(dict(op='asset.provenance.set', asset=asset, records=[provenance['record']]))
        revision = transact(hit_ops)
        defaults = call('input.describe')
        inputs = copy.deepcopy(defaults['gamepad_defaults'])
        inputs['actions'] = dict(aim=['mouse.3', 'gamepad.left_trigger'],
            fire=['mouse.1', 'gamepad.right_trigger'], crouch=['key.c'], reload=['key.r'])
        input_receipt = call('input.transact', dict(path=str(args.output/INPUT_PATH),
            request_id=uuid.uuid4().hex, expected_revision=0, profile=inputs))
        need(input_receipt['persisted'] is True, 'Input profile was not persisted.')
        project = dict(format='poima.project', version=1, project_id=uuid.uuid4().hex,
            name='Physical Character Yard', entry=dict(world='world.json',
                controller=uid(PLAYER), camera=uid(CAMERA)), input_profile=INPUT_PATH, audio=False)
        write_json(args.output/'project.json', project)
        report.update(revision=revision, asset=asset, source_info=info, source_nodes=nodes,
            imported=imported, clips=cooked_clips, selected_clips=selected,
            node_map=[mapping[index] for index in sorted(mapping)], hit_regions=hit_regions,
            input_profile=dict(path=INPUT_PATH, revision=input_receipt['revision'], profile=inputs),
            expected_game=dict(module=MODULE, type=GAME_TYPE),
            project=project, source_profile='kaykit-knight-original-grounded-bounded-drives-v1')
        package = Path(str(world)+'.assets')/(asset+'.pmodel')
        need(identity(package)['sha256'] == asset, 'Cooked package content address differs.')
        report['files'] = {path.relative_to(args.output).as_posix(): identity(path) for path in
            (world, package, args.output/'project.json', args.output/INPUT_PATH, source, notice,
             args.output/'content/NOTICE.txt', args.output/'content/source-manifest.json',
             args.output/'content/physical-profile.json', args.output/'content/drive-poses.json')}
        original(args.source, manifest['model']); original(args.license, manifest['license_file'])
        need(identity(HERE/'profiles/knight.json') == report['inputs']['profile'],
             'Original physical profile changed while authoring.')
        need(identity(pose_path) == report['inputs']['drive_poses'], 'Authored pose library changed while preparing.')
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            try:
                client.close()
            except BaseException:
                report['passed'] = False
                report['errors'].append(traceback.format_exc())
            report['close'] = dict(closed=client.closed, pid=client.transport.process_id,
                exit_code=client.transport.returncode,
                cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error))
            if not client.closed or client.transport.returncode != 0 or client.cleanup_error is not None:
                report['passed'] = False
        write_json(args.output/'preparation.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source', 'license', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--velocity-tracking', action='store_true',
        help='Track fitted animation target velocity as well as orientation during physical playback.')
    args = parser.parse_args()
    try:
        report = prepare(args)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, str(error)+'\nSupply the original files named in source-manifest.json and a fresh output path.\n')
    print(json.dumps(dict(passed=report['passed'], project=str(args.output/'project.json'),
        report=str(args.output/'preparation.json'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
