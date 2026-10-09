#!/usr/bin/env python3
"""Qualify compiled controller-driven idle/run playback from original separate FBX.

No downloads, builds, source edits, teleports, gameplay patches or animation-clock
overrides. Original normalized source poses are inspected by the native importer;
quaternion-chain/FK and weighted skin expectations are calculated separately.
This is not independent FBX decoding, stride calibration, IK or foot locking.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback

from animation_rotation_retarget import (conjugate, from_columns, inverse,
    matrix, multiply, paths, point, product, qchains, rotated, unit, worlds)
from animation_rotation_retarget_capture import (ORIGINAL_SHA256, LICENSE_SHA256,
    package_geometry, weighted)

ROOT = Path(__file__).resolve().parents[1]
CONFIG, ROUTE = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb75', 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb76'
FIELD = lambda value: f'{value:032x}'
ACTOR, RIG = FIELD(300), FIELD(400)
DT = 1/60


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def normalized(value):
    if isinstance(value, dict):
        return {key: normalized(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [normalized(item) for item in value]
    return value


def projection(snapshot):
    result = copy.deepcopy(snapshot)
    # Exact immediate restoration keeps these too. The first subsequent Tick
    # deliberately observes the newly allocated host save epoch.
    for key in ('ObservedEpochHigh', 'ObservedEpochLow'):
        result['values'].pop(key)
    return result


def node_id(index):
    return hashlib.sha256(f'poima.instance.v1/{RIG}/node/{index}'.encode()).hexdigest()[:32]


def maximum_matrix_error(a, b):
    return max(abs(x-y) for ar, br in zip(a, b) for x, y in zip(ar, br))


def horizontal(a, b):
    return math.hypot(a[0]-b[0], a[2]-b[2])


def position(entity):
    return entity['world_matrix'][12:15]


class OriginalOracle:
    """Uses unmodified ORIGINAL imports, never converted sampled poses."""
    def __init__(self, game, source_directory, world, imported, record, native):
        self.directory, self.native = source_directory, native
        self.base_source = source_directory/'Model/characterMedium.fbx'
        self.base_info = game.rpc('asset.source.inspect', dict(source=native(self.base_source)))
        self.base = self.nodes(game, self.base_source, self.base_info['model_sha256'])
        need(len(self.base) == 61, 'Original target must retain all61 nodes')
        self.base_paths = paths(self.base)
        self.base_chain = qchains(self.base)
        self.base_worlds = worlds(self.base)
        self.baseline = game.rpc('asset.import', dict(source=native(self.base_source)))['asset']
        self.package = package_geometry(Path(str(world)+'.assets')/(self.baseline+'.pmodel'), self.baseline)
        cooked = package_geometry(Path(str(world)+'.assets')/(imported['asset']+'.pmodel'), imported['asset'])
        need(cooked['geometry_bytes'] == self.package['geometry_bytes'], 'Retarget changed original geometry/influences/indices')
        for key in ('nodes', 'skins', 'roots', 'primitives'):
            need(cooked['metadata'][key] == self.package['metadata'][key], 'Retarget changed original target '+key)
        self.skins = {}
        for skin in game.page('asset.inspect', dict(asset=self.baseline, section='skins')):
            self.skins[skin['index']] = game.page('asset.animation.skin', dict(asset=self.baseline, skin=skin['index']))
        self.profiles = {}
        for name, index in (('run', 0), ('idle', 1)):
            source = source_directory/('Animations/'+name+'.fbx')
            info = game.rpc('asset.source.inspect', dict(source=native(source)))
            clips = game.page('asset.source.inspect', dict(source=native(source), section='animations', expected_model_sha256=info['model_sha256']))
            need(len(clips) == 2 and 'Targeting' in clips[0]['name'] and name.lower() in clips[1]['name'].lower(), 'Original take selection differs')
            reference = self.nodes(game, source, info['model_sha256'], dict(kind='sample', clip=0, time=0))
            source_paths = paths(reference)
            by_path = {value: key for key, value in source_paths.items()}
            mapping = {i: by_path[path] for i, path in self.base_paths.items() if path in by_path}
            selected = {row['index'] for row in reference if row['name'] in ('HipsCtrl', 'Hips')}
            need(len(selected) == 2 and selected <= set(mapping.values()), 'Explicit original pelvis nodes are not uniquely mapped')
            source_chain = qchains(reference)
            correction = {i: unit(product(conjugate(source_chain[s]), self.base_chain[i])) for i, s in mapping.items()}
            self.profiles[index] = dict(source=source, hash=info['model_sha256'], reference={r['index']: r for r in reference},
                selected=selected, mapping=mapping, correction=correction, duration=clips[1]['duration'], original_clip=1, name=name)
        clips = game.page('asset.inspect', dict(asset=imported['asset'], section='animations'))
        need(len(clips) == 2 and all(clips[i]['duration'] == self.profiles[i]['duration'] for i in range(2)), 'Cooked clips changed original durations')
        record['original_oracle'] = dict(base_model_sha256=self.base_info['model_sha256'], baseline_asset=self.baseline,
            geometry_sha256=self.package['geometry_sha256'], profiles={str(i): dict(name=p['name'], original_model_sha256=p['hash'],
                original_motion_clip=p['original_clip'], reference_clip=0, duration=p['duration'], translation_nodes=sorted(p['selected'])) for i,p in self.profiles.items()},
            scope='Native normalized ORIGINAL reference/motion poses; separate quaternion-chain/FK/weighted skin math with immutable original target geometry/IBMs. Not an independent FBX parser.')
        self.cache = {}

    def nodes(self, game, source, pin, pose=None):
        params = dict(source=self.native(source), section='nodes', expected_model_sha256=pin)
        if pose is not None:
            params['pose'] = pose
        return game.page('asset.source.inspect', params)

    def evaluate(self, game, clip, clock):
        key = (clip, clock)
        if key in self.cache:
            return self.cache[key]
        profile = self.profiles[clip]
        source = self.nodes(game, profile['source'], profile['hash'], dict(kind='sample', clip=profile['original_clip'], time=clock))
        indexed, chains = {r['index']: r for r in source}, qchains(source)
        target = copy.deepcopy(self.base)
        desired_chain = {i: unit(product(chains[s], profile['correction'][i])) for i,s in profile['mapping'].items()}
        for row in target:
            i, parent = row['index'], row['parent']
            if i not in profile['mapping']:
                continue
            source_id = profile['mapping'][i]
            row['rotation'] = desired_chain[i] if parent < 0 else unit(product(conjugate(desired_chain[parent]), desired_chain[i]))
            if source_id in profile['selected']:
                delta = [indexed[source_id]['position'][k]-profile['reference'][source_id]['position'][k] for k in range(3)]
                left = conjugate(profile['correction'][parent]) if parent >= 0 else [0,0,0,1]
                row['position'] = [row['position'][k]+v for k,v in enumerate(rotated(left, delta))]
        result = worlds(target)
        self.cache[key] = result
        return result

    def skin_points(self, joint_worlds):
        result = []
        for mesh in self.base:
            if not mesh['primitives']:
                continue
            mesh_world = joint_worlds[mesh['index']]
            palette = [multiply(multiply(inverse(mesh_world), joint_worlds[j['node']]), from_columns(j['inverse_bind'])) for j in self.skins[mesh['skin']]]
            for primitive in mesh['primitives']:
                data = self.package['primitives'][primitive]
                for vertex, influence in zip(data['vertices'], data['influences']):
                    blend = weighted(palette, influence[:4], influence[4:])
                    # Sampling applies mesh world once, after weighted xyz;
                    # float weights need not sum exactly1. No divide by w.
                    result.append(point(mesh_world, point(blend, vertex[:3])))
        need(len(result) == 1029, 'Original weighted vertex count differs')
        return result


def observe(game, oracle, authored, record):
    actor, rig = game.entity(), game.entity(RIG)
    clock = rig['animation']
    need(clock is not None and clock['clip'] in (0,1) and clock['loop'] and clock['playing'], 'Real idle/run loop is not playing')
    need(0 <= clock['time'] < oracle.profiles[clock['clip']]['duration'], 'Real loop clock is outside original clip duration')
    attachment = authored['entities'][RIG]['components']['Transform']
    wrapper = multiply(from_columns(actor['world_matrix']), matrix(**attachment))
    attachment_error = maximum_matrix_error(wrapper, from_columns(rig['world_matrix']))
    need(attachment_error < 1e-5, 'Imported rig lost authored controller attachment')
    rows = [game.entity(node_id(i)) for i in range(61)]
    actual = {i: from_columns(row['world_matrix']) for i,row in enumerate(rows)}
    compared = clock.get('transition') is None
    joint_error = skin_error = None
    if compared:
        local = oracle.evaluate(game, clock['clip'], clock['time'])
        expected = {i: multiply(wrapper, value) for i,value in local.items()}
        joint_error = max(maximum_matrix_error(expected[i], actual[i]) for i in range(61))
        need(joint_error < 8e-5, 'Actual runtime node worlds differ from original-source rotation-policy oracle')
        wanted, measured = oracle.skin_points(expected), oracle.skin_points(actual)
        skin_error = max(math.dist(a,b) for a,b in zip(wanted, measured))
        need(skin_error < 1.5e-4, 'Actual runtime joints deform original weighted geometry differently from original-source oracle')
    values = game.values()
    need(values['Actor'] == ACTOR and values['AnimatedVisual'] == RIG and values['AnimationClip'] == clock['clip'] and
         abs(values['AnimationRate']-clock['speed']) < 1e-10, 'Compiled animation state disagrees with actual native clock')
    forward = unit([-actor['world_matrix'][8],0,-actor['world_matrix'][10]])
    # The explicit180-degree wrapper maps source+Z to controller-Z. Animated
    # toes may roll, so a gait pose is not used as a guessed facing reference.
    visual_forward = unit([wrapper[0][2],0,wrapper[2][2]])
    alignment = sum(a*b for a,b in zip(forward,visual_forward))
    need(alignment > .999999, 'Imported source-facing attachment opposes native movement forward')
    record.setdefault('visual_observations', []).append(dict(tick=game.tick, clip=clock['clip'], clip_time=clock['time'],
        speed=clock['speed'], transition=clock.get('transition'), source_pose_compared=compared,
        maximum_joint_matrix_error=joint_error, maximum_weighted_vertex_error=skin_error,
        attachment_matrix_error=attachment_error, forward_alignment=alignment, actor_position=position(actor)))
    state, module, route = game.inspect(), game.module(), game.route()
    return normalized(dict(tick=game.tick, actor=actor, rig=rig, nodes=rows, config=game.visual_config(), route=route,
        values=module['module']['values'], backend=module['module']['backend'], gameplay_revision=module['revision'],
        revisions=dict(component_revision=route['component_revision'], **{k:state[k] for k in ('structure_revision','ui_revision','control_sequence')}),
        ui=game.rpc('runtime.ui.inspect', dict(session_id=game.session,tick=game.tick))))


def load_launcher():
    path = ROOT/'examples/locomotion-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_locomotion_yard_sample', path)
    need(spec is not None and spec.loader is not None, 'Locomotion launcher is absent')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','manifest','source-directory','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    for name in ('assembly','bridge','hostfxr','descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=float, default=1200)
    args = parser.parse_args()
    if bool(args.descriptor) == bool(args.assembly) or (not args.descriptor and not (args.bridge and args.hostfxr)):
        parser.error('Supply --descriptor OR --assembly with --bridge and --hostfxr')
    if args.gpu < 0 or not math.isfinite(args.timeout) or not 60 <= args.timeout <= 1800:
        parser.error('GPU must be nonnegative and timeout finite within60..1800seconds')
    inputs = {name:getattr(args,name).resolve() for name in ('binary','manifest','assembly','bridge','hostfxr','descriptor') if getattr(args,name)}
    for name,path in inputs.items():
        if not path.is_file():
            parser.error('Missing '+name)
        setattr(args,name,path)
    args.source_directory = args.source_directory.resolve(strict=True)
    originals = {name:args.source_directory/relative for name,relative in dict(base='Model/characterMedium.fbx',run='Animations/run.fbx',idle='Animations/idle.fbx').items()}
    originals['license'] = args.source_directory/'License.txt'
    pins = dict(ORIGINAL_SHA256, license=LICENSE_SHA256)
    if not all(path.is_file() and sha(path) == pins[name] for name,path in originals.items()):
        parser.error('Supply the untouched pinned Kenney Protagonists1.1 base,run,idle and CC0 License.txt')
    if args.descriptor:
        descriptor = json.loads(args.descriptor.read_text(encoding='utf-8'))
        need(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2, 'Native descriptor must inventory actual published artifacts')
        library_found = False
        for i,item in enumerate(descriptor['files']):
            relative = Path(item['path'])
            need(not relative.is_absolute() and '..' not in relative.parts, 'Unsafe descriptor member')
            path = args.descriptor.parent/relative
            need(path.is_file() and path.stat().st_size == item['size'] and sha(path) == item['sha256'], 'Native artifact inventory mismatch')
            inputs['artifact_'+str(i)] = path
            library_found |= item['path'] == descriptor['library'] and item['role'] == 'library'
        need(library_found, 'Descriptor does not inventory its native library')
    output = args.output.resolve()
    if output.exists():
        parser.error('--output must be new')
    output.mkdir(parents=True)
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    need(manifest['format'] == 'poima.components' and {s['id'] for s in manifest['schemas']} == {CONFIG,ROUTE}, 'Actual compiled Locomotion Yard schemas differ')
    schema = next(s for s in manifest['schemas'] if s['id'] == ROUTE)
    fields = {f['id']:f for f in schema['fields']}
    need(fields[FIELD(1)]['kind'] == 'array' and fields[FIELD(1)]['element_kind'] == 'float32' and fields[FIELD(1)]['capacity'] == 30 and fields[FIELD(2)]['kind'] == 'int32', 'Compiled persisted route schema differs')
    launcher = load_launcher()
    source_paths = [Path(__file__), ROOT/'examples/locomotion-yard/run.py', ROOT/'examples/locomotion-yard/LocomotionYardGame.cs',
        ROOT/'examples/locomotion-yard/Poima.LocomotionYardGame.csproj', *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
        *[ROOT/'tests'/name for name in ('animation_rotation_retarget.py','animation_rotation_retarget_capture.py','rotation_retarget_fixture.py','frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')]]
    record = dict(passed=False,backend='native_aot' if args.descriptor else 'coreclr',calls=[],owners=[],checks=[],cleanup_errors=[],
        hashes={name:sha(path) for name,path in inputs.items()},sources={p.relative_to(ROOT).as_posix():sha(p) for p in source_paths},
        original_source_hashes=pins,continuation_exclusions=['values.ObservedEpochHigh','values.ObservedEpochLow'],
        limits=['Original source decoding/sampling uses the native importer; quaternion chains/FK/weighted skin expectations are independent of the converted asset.',
            'Controller-driven in-place locomotion with explicitly authored3m/s playback reference; no stride fitting, IK, foot locking or loop repair. Original Run is C0, not C1, at wrap.',
            'Fresh restoration removes only verifier-owned exact source copies. Caller originals remain available read-only to the source-pose oracle.',
            'Optional Vulkan readbacks do not qualify physical input, editor usability, OS screenshots or frame-rate performance.'])
    deadline = time.monotonic()+args.timeout
    world = output/'world.json'
    owned_sources = output/'owned-sources'
    for name,path in originals.items():
        target = owned_sources/path.relative_to(args.source_directory)
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,target)
        need(sha(target) == pins[name], 'Verifier-owned exact source copy differs')
    owners = []

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if args.windows_interop and os.name != 'nt' else value

    def open_game(label):
        game = launcher.OwnedGame(args,world,label,record,deadline,native,manifest)
        owners.append(game)
        return game

    def close_game(game):
        game.close()
        transport = game.client.transport
        row = next(r for r in record['owners'] if r['label'] == game.label)
        row.update(process_id=transport.process_id,stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated)
        need(row['exit_code'] == 0 and not row['stderr'] and not row['stderr_truncated'], 'Native owner has a nonclean terminal result')

    def snapshot(game):
        return observe(game,oracle,authored,record)

    def restore(game):
        game.restore('midfade')
        need(snapshot(game) == checkpoint, 'Immediate exact save restore changed complete native/compiled/typed/UI state')

    def capture(game, name):
        if not args.capture:
            return
        state = game.inspect()
        path = output/(name+'.bmp')
        result = game.rpc('runtime.capture',dict(session_id=game.session,tick=game.tick,ui_revision=state['ui_revision'],
            camera=FIELD(launcher.OBSERVER),path=native(path),width=960,height=640,gpu=args.gpu,samples=4,ui_scale=1,profile=True),timeout=180)
        need(result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, 'Actual Vulkan readback failed')
        draws = result['render_diagnostics']['last_draws']
        need(draws['skinned_instances'] == 1 and draws['skinned_vertices'] == 1029, 'Actual original weighted character was not rendered')
        record.setdefault('captures',[]).append(dict(name=name,sha256=sha(path),report=result))

    def one_tick(game):
        before_actor, before_rig, before_values = game.entity(),game.entity(RIG),game.values()
        game.step()
        actor,rig,values = game.entity(),game.entity(RIG),game.values()
        old,new = before_rig['animation'],rig['animation']
        need(new['clip'] == (0 if values['Moving'] else 1) and new['loop'] and new['playing'], 'Compiled movement mode did not select an advancing original idle/run loop')
        prior = [before_values['PreviousX'],0,before_values['PreviousZ']]
        displacement = horizontal(position(before_actor),prior) if before_values['PreviousPositionValid'] else 0
        need(abs(values['LastDisplacement']-displacement) < 2e-7, 'Playback sampled current uncommitted movement instead of previous committed position')
        need(abs(values['LastHorizontalSpeed']-displacement/DT) < 2e-5 and abs(values['RequestedRunRate']-min(8,displacement/DT/nominal_run_speed)) < 8e-6, 'Compiled playback rate is not derived from committed horizontal meters per second')
        need(abs(values['PreviousX']-position(before_actor)[0]) < 2e-7 and abs(values['PreviousZ']-position(before_actor)[2]) < 2e-7, 'Persisted movement history is not prior committed position')
        switched = old['clip'] != new['clip']
        expected_time = math.fmod((0 if switched else old['time'])+new['speed']*DT,new['duration'])
        # Gameplay observes the committed pre-step tick and publishes commands
        # there, before physics and ++tick. Clip changes anchor at0; same-clip
        # rate corrections retain that observed time and advance at the new
        # rate for the next committed tick. Derive this from actual clocks.
        need(abs(new['time']-expected_time) < 3e-8, 'Native clock restarted, advanced with the wrong rate, or wrapped incorrectly')
        transitions = values['AnimationTransitions']-before_values['AnimationTransitions']
        corrections = values['RateChanges']-before_values['RateChanges']
        need(transitions == int(switched) and 0 <= corrections <= 1, 'Clip/rate counters do not reflect actual playback commands')
        requested = values['RequestedRunRate'] if values['Moving'] else 1
        if switched or corrections:
            need(abs(new['speed']-requested) < 1e-10, 'Published clip/rate change ignores the actual committed movement request')
        if not switched and old.get('transition') is not None:
            need(new['speed'] == old['speed'] and corrections == 0, 'Rate-only correction replaced an active finite fade')
            deferred = int(abs(old['speed']-requested) >= .01)
            need(values['DeferredRateUpdates']-before_values['DeferredRateUpdates'] == deferred, 'Deferred-rate counter does not describe actual finite-fade rate difference')
        distance = horizontal(position(actor),position(before_actor))
        need(distance <= 4*DT+2e-5, 'Movement exceeded authored capsule speed; possible teleport')
        if distance > .001:
            forward = [-actor['world_matrix'][8],-actor['world_matrix'][10]]
            moved = [position(actor)[0]-position(before_actor)[0],position(actor)[2]-position(before_actor)[2]]
            need(sum(a*b for a,b in zip(forward,moved))/(math.hypot(*forward)*distance) > .98, 'Visual/controller facing differs from actual forward movement')
        row = dict(tick=game.tick,position=position(actor),previous_committed_position=position(before_actor),clip=new['clip'],time=new['time'],speed=new['speed'],
            sampled_displacement=values['LastDisplacement'],requested_rate=values['RequestedRunRate'],transition=new.get('transition'),
            wrapped=not switched and new['time'] < old['time'],rate_changes=corrections,animation_transitions=transitions,deferred=values['DeferredRateUpdates'])
        record.setdefault('tick_observations',[]).append(row)
        return row

    try:
        game = open_game('locomotion')
        imported = game.author(owned_sources)
        record['authored'] = imported
        authored_bytes = world.read_bytes()
        authored = json.loads(authored_bytes)
        need(authored['entities'][RIG]['parent'] == ACTOR, 'Imported rig is not attached to native capsule')
        controller = authored['entities'][ACTOR]['components']['CharacterController']
        need(controller['camera'] is None and controller['speed'] == 4 and controller['radius'] == .42, 'Independent courier controller profile differs')
        cover = authored['entities'][FIELD(2)]['components']
        cover_transform, cover_collider = cover['Transform'], cover['BoxCollider']
        need(cover_transform['rotation'] == [0,0,0,1], 'Closest-point cover oracle requires the authored unrotated box')
        cover_center = [cover_transform['position'][i] for i in (0,2)]
        cover_half = [abs(cover_transform['scale'][i])*cover_collider['half_extents'][i] for i in (0,2)]
        record['cover_clearance'] = dict(center_xz=cover_center,half_extents_xz=cover_half,capsule_radius=controller['radius'],
            numerical_epsilon_meters=2e-5,minimum_signed_gap_meters=None,method='Horizontal Euclidean closest-point distance to original cover rectangle, minus capsule radius; not an axis-expanded square.')
        config = authored['entities'][ACTOR]['components']['game:'+CONFIG]
        nominal_run_speed = config[FIELD(10)]
        need(nominal_run_speed == 3 and config[FIELD(11)] == .8, 'Explicit playback reference/physical corner slowdown profile differs')
        oracle = OriginalOracle(game,args.source_directory,world,imported,record,native)
        minimum = min(p[1] for p in oracle.skin_points(oracle.base_worlds))
        maximum = max(p[1] for p in oracle.skin_points(oracle.base_worlds))
        need(abs(controller['height']-(maximum-minimum)) < 2e-5 and abs(authored['entities'][RIG]['components']['Transform']['position'][1]+minimum) < 2e-5, 'Capsule height/visual offset did not come from original standing geometry')
        game.start(); game.load(); game.step(16)
        initial = snapshot(game)
        need(initial['rig']['animation']['clip'] == 1 and initial['rig']['animation']['time'] > 0 and initial['rig']['animation'].get('transition') is None and initial['values']['Phase'] == 0, 'Initial stationary state must advance actual settled idle, not hold a fake pose')
        ui = game.rpc('runtime.ui.inspect',dict(session_id=game.session,tick=game.tick))
        controls = {r['id']:r for r in ui['elements']}
        for action,identity in launcher.ACTIONS.items():
            row = controls[FIELD(identity)]
            need(row['eligible'] and row['effective_visible'] and row['effective_enabled'] and row['action'] == action, 'Compiled control is not logically eligible: '+action)
        need(len(launcher.ACTIONS) == 6, 'Expected six real locomotion controls')
        capture(game,'idle')
        run_rows = []
        first_run = None
        for _ in range(180):
            row = one_tick(game)
            if row['clip'] == 0:
                first_run = first_run or row['tick']
                run_rows.append(row)
                if game.entity(RIG)['animation'].get('transition') is not None and len(run_rows) == 4:
                    break
        need(first_run is not None and any(r['wrapped'] and r['clip'] == 1 for r in record['tick_observations']), 'Initial idle did not naturally wrap before compiled dispatch')
        checkpoint = snapshot(game)
        need(checkpoint['rig']['animation'].get('transition') is not None and checkpoint['values']['Phase'] == 1, 'Save fixture did not capture a real moving idle-to-run inertial fade')
        coordinates = checkpoint['route']['values'][FIELD(1)]
        need(checkpoint['values']['LastPathStatus'] == 0 and checkpoint['values']['LastCornerCount'] >= 4 and len(coordinates) == 3*checkpoint['values']['LastCornerCount'], 'Compiled complete navigation route is absent')
        need(any(abs(coordinates[i+2]) > 3.4 for i in range(0,len(coordinates),3)), 'Persisted path ignores cover/capsule clearance')
        ray = game.rpc('runtime.raycast',dict(session_id=game.session,tick=game.tick,origin=[-4,.8,0],direction=[1,0,0],distance=8,ignore=[ACTOR]))
        need(ray['hit']['entity'] == FIELD(2), 'Native ray does not confirm blocked direct shortcut')
        game.saves(output/'saves')
        receipt = game.write('midfade')
        payloads = list((output/'saves/slot-midfade').glob('p-*.bin'))
        need(len(payloads) == 1 and sha(payloads[0]) == receipt['sha256'], 'Exact native save receipt lacks matching disk bytes')
        record['checkpoint'],record['checkpoint_payload_sha256'] = checkpoint,receipt['sha256']
        before = snapshot(game)
        if args.descriptor:
            game.load(error=-32060)
            need(snapshot(game) == before, 'Unsupported AOT replacement mutated sample')
        else:
            game.load()
            after = snapshot(game)
            need(all(after[k] == before[k] for k in before if k != 'gameplay_revision') and after['gameplay_revision'] > before['gameplay_revision'], 'Compatible CoreCLR reload lost live movement/clock/fade/typed/UI state')
        restore(game)
        game.step(57)
        continued = snapshot(game)
        need(continued['rig']['animation']['clip'] == 0 and continued['rig']['animation'].get('transition') is None, 'Continuation must exercise settled genuine Run source-pose comparison')
        capture(game,'run')
        restore(game)
        for count in (1,7,3,11,5,2,28):
            game.step(count)
        partitioned = snapshot(game)
        need(projection(partitioned) == projection(continued), 'Unequal fixed-tick partitions changed exact native/compiled continuation')
        restore(game)
        game.control('dispatch'); game.step(15)
        redispatched = snapshot(game)
        need(redispatched['values']['Dispatches'] == checkpoint['values']['Dispatches']+1 and redispatched['values']['Plans'] == checkpoint['values']['Plans']+1, 'Real redispatch did not issue compiled route query')
        record.update(grouped_continuation=continued,unequal_continuation=partitioned,immediate_redispatch=redispatched)
        restore(game)
        paused = game.control('pause')
        need(paused['intent'] == 2 and game.values()['Paused'] == 1, 'Real pause control did not publish host pause intent')
        saved = game.control('save')
        need(saved['save_serviced'] and saved['save_operation']['state'] == 3 and saved['save_operation']['error_code'] == 0, 'Real compiled UI save failed')
        ui_checkpoint = snapshot(game)
        game.control('status'); game.control('resume'); game.step(25)
        loaded = game.control('load')
        need(loaded['save_serviced'] and loaded['runtime_replaced'] and loaded['save_operation']['state'] == 3 and snapshot(game) == ui_checkpoint, 'Compiled UI load did not restore complete immediate state')
        game.control('status')
        status = game.values()
        # int64 gameplay fields use exact decimal strings on the wire.
        need(status['Paused'] == 1 and status['SavePending'] == 0 and status['LastSaveState'] == 3 and status['LastSaveError'] == 0 and
             int(status['LastSaveGeneration']) == loaded['save_operation']['generation'], 'Compiled status did not reconcile restored save receipt')
        need(game.control('resume')['intent'] == 1 and game.values()['Paused'] == 0, 'Real resume did not publish host resume intent')
        record.update(ui_save_operation=saved,ui_load_operation=loaded)
        close_game(game)
        shutil.rmtree(owned_sources)
        need(not owned_sources.exists(), 'Owned authoring source removal failed')
        fresh = open_game('fresh-without-owned-authoring-sources')
        fresh.saves(output/'saves'); restore(fresh)
        fresh.step(57)
        fresh_grouped = snapshot(fresh)
        need(projection(fresh_grouped) == projection(continued), 'Fresh source-independent grouped continuation differs')
        restore(fresh)
        for _ in range(57):
            fresh.step()
        fresh_single = snapshot(fresh)
        need(projection(fresh_single) == projection(continued), 'Fresh source-independent single ticks differ')
        restore(fresh)
        fresh.control('dispatch'); fresh.step(15)
        fresh_redispatch = snapshot(fresh)
        need(projection(fresh_redispatch) == projection(redispatched), 'Fresh redispatch diverged from saved route/history')
        record.update(fresh_grouped_continuation=fresh_grouped,fresh_single_continuation=fresh_single,fresh_redispatch=fresh_redispatch)
        restore(fresh)
        positions = []
        local_run = []
        for _ in range(1500):
            row = one_tick(fresh)
            positions.append(row['position'])
            gap = math.hypot(*[max(abs(row['position'][i]-center)-extent,0) for i,center,extent in zip((0,2),cover_center,cover_half)])-controller['radius']
            row['cover_signed_gap_meters'] = gap
            previous_gap = record['cover_clearance']['minimum_signed_gap_meters']
            record['cover_clearance']['minimum_signed_gap_meters'] = gap if previous_gap is None else min(gap,previous_gap)
            need(gap >= -2e-5, 'Native capsule overlaps solid cover beyond20micrometers numerical allowance')
            if row['clip'] == 0:
                local_run.append(row)
            values = fresh.values()
            need(values['Phase'] != 3, 'Compiled complete route became blocked')
            if values['Arrivals'] == 1:
                break
        need(fresh.values()['Arrivals'] == 1 and horizontal(positions[-1],[4,0,0]) < .16 and any(abs(p[2]) > 3.4 for p in positions), 'Actual movement did not follow detour and arrive')
        need(any(r['wrapped'] for r in local_run), 'Actual controller-driven Run never crossed original float-duration wrap')
        for offset in (40,41):
            boundary = next((r for r in local_run if r['tick'] == first_run+offset),None)
            need(boundary is not None, 'Run40/41 tick boundary was not observed')
            record.setdefault('run_boundary_observations',[]).append(dict(ticks_since_first_run=offset,**boundary))
        need(fresh.values()['RateChanges'] > 0 and fresh.values()['DeferredRateUpdates'] > 0, 'Actual motion did not exercise deferred finite-fade rate correction')
        fresh.step(20)
        delivered = snapshot(fresh)
        need(delivered['rig']['animation']['clip'] == 1 and delivered['rig']['animation']['transition'] is None, 'Arrival did not settle back to live idle')
        stationary = position(delivered['actor'])
        old_time = delivered['rig']['animation']['time']
        fresh.step(15)
        settled = snapshot(fresh)
        need(horizontal(position(settled['actor']),stationary) < .001 and abs(settled['rig']['animation']['time']-math.fmod(old_time+15*DT,oracle.profiles[1]['duration'])) < 3e-8, 'Arrival moved capsule or froze/restarted idle breathing')
        capture(fresh,'delivered-idle')
        fresh.control('dispatch'); fresh.step(15)
        need(fresh.values()['Plans'] == 2 and fresh.values()['Dispatches'] == 2 and fresh.values()['Destination'] == 0, 'Delivered courier cannot dispatch return route')
        record.update(route_positions=positions,delivered=delivered,settled=settled,source_removal=dict(owned_sources_removed=True,caller_originals_preserved=True))
        record['checks'] = ['Original target geometry/IBMs preserved;61 joint/node worlds and1029 weighted vertices compared against original-source rotation-policy math after fades.',
            'Idle advances and wraps; real Run clocks preserve cumulative committed playback timing through40/41 fixed ticks and original float-duration wrap.',
            'Committed native displacement controls playback rate one tick later; active finite fades defer speed-only updates without clock restart.',
            'Native capsule detours around ray-confirmed cover and arrives within authored speed; visual facing/attachment follow movement.',
            'Exact midfade disk saves preserve all native/compiled/typed/UI state; same/fresh owners grouped, unequal and single-tick continuations agree.',
            'Compatible CLR reload or unsupported AOT replacement is verified; actual compiled pause/save/load/resume and redispatch controls run.']
        need(world.read_bytes() == authored_bytes, 'Runtime/oracle changed authored world bytes')
        need(all(sha(path) == pins[name] for name,path in originals.items()), 'Caller original source changed')
        need(all(sha(path) == record['hashes'][name] for name,path in inputs.items()), 'Supplied binary/artifact changed')
        need(all(sha(ROOT/path) == pin for path,pin in record['sources'].items()), 'Frozen sample/verifier inputs changed')
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        for game in reversed(owners):
            try:
                close_game(game)
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        record['passed'] = record['passed'] and not record['cleanup_errors']
        record['rpc_count'] = len(record['calls'])
        record['method_counts'] = dict(sorted(Counter(r['method'] for r in record['calls']).items()))
        with (output/'evidence.json').open('x',encoding='utf-8') as stream:
            json.dump(record,stream,indent=2);stream.write('\n')
    print(json.dumps(dict(passed=record['passed'],rpc_count=record['rpc_count'],owners=len(record['owners']))))
    if not record['passed']:
        print(record.get('error') or record['cleanup_errors'],file=sys.stderr)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
