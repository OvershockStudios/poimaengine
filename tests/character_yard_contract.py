#!/usr/bin/env python3
"""Qualify the actual compiled imported-character yard sample without game patches.

Uses separately supplied original RiggedFigure.glb, copyright2017 Cesium,
CC-BY-4.0, pinned at the source SHA documented by imported_character_contract.
The source-derived pose oracle reads original arrays rather than engine poses.
No downloads, source preprocessing, engine builds, managed builds, teleports,
manual route/component edits or uncertain-mutation retries are performed.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

from imported_character_contract import SourceOracle, SOURCE_SHA, SOURCE_URL, multiply, point, transform_matrix, maximum_error

ROOT = Path(__file__).resolve().parents[1]
CONFIG = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb70'
ROUTE = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb71'
FIELD = lambda index: f'{index:032x}'
ACTOR = f'{300:032x}'
VISUAL = FIELD(1)
COORDINATES, CURSOR = FIELD(1), FIELD(2)


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


def position(entity):
    return entity['world_matrix'][12:15]


def entity_id(root, node):
    return hashlib.sha256(f'poima.instance.v1/{root}/node/{node}'.encode()).hexdigest()[:32]


def typed(game, component, actor=ACTOR):
    return game.rpc('runtime.component.get', dict(session_id=game.session, tick=game.tick, id=actor, type=component))


def avatar(game, oracle, authored, record):
    """Observe actual actor, rig and every joint; compare to original source data."""
    actor = game.rpc('runtime.entity', dict(session_id=game.session, tick=game.tick, id=ACTOR))
    configuration = typed(game, CONFIG)
    visual = configuration['values'][VISUAL]
    rig = game.rpc('runtime.entity', dict(session_id=game.session, tick=game.tick, id=visual))
    clock = rig['animation']
    need(clock is not None and clock['clip'] == 0, 'Sample must drive the inspected original single clip')
    need(0 <= clock['time'] <= oracle.duration and not clock['loop'], 'Original stationary motion must be bounded and nonlooping')
    source = oracle.evaluate(clock['time'])
    # Attachment is authored separately from the source skeleton. Its original
    # local transform is independently retained, not read back as an expectation.
    attachment = authored['entities'][visual]['components']['Transform']
    matrix = multiply(actor['world_matrix'], transform_matrix(**attachment))
    root_error = maximum_error(matrix, rig['world_matrix'])
    need(root_error < 2e-6, 'Imported visual no longer follows its native character parent')
    joint_error = 0
    nodes = []
    compare_source_pose = clock.get('transition') is None
    for index in range(22):
        actual = game.rpc('runtime.entity', dict(session_id=game.session, tick=game.tick, id=entity_id(visual, index)))
        nodes.append(actual)
        if compare_source_pose:
            joint_error = max(joint_error, maximum_error(actual['world_matrix'], multiply(matrix, source['worlds'][index])))
    if compare_source_pose:
        need(joint_error < 4e-6, 'Runtime imported joint worlds differ from source pose and native attachment')
    feet = [point(matrix, vertex) for vertex in source['vertices']]
    lowest = min(vertex[1] for vertex in feet)
    source_floor_error = abs(lowest-position(actor)[1])
    need(source_floor_error < .06 and abs(lowest) < .06, 'Imported mesh feet do not align with the native capsule root and known flat yard floor')
    toes = []
    for ankle, toe in ((5, 6), (9, 10)):
        a = point(matrix, source['worlds'][ankle][12:15])
        b = point(matrix, source['worlds'][toe][12:15])
        toes.append([b[0]-a[0], b[2]-a[2]])
    forward = [-actor['world_matrix'][8], -actor['world_matrix'][10]]
    alignment = min(sum(a*b for a, b in zip(vector, forward))/(math.hypot(*vector)*math.hypot(*forward)) for vector in toes)
    need(alignment > .98, 'Imported +Z facing does not align with native actor -Z forward')
    values = game.values()
    need(values['AnimatedVisual'] == visual and values['Actor'] == ACTOR and values['AnimationClip'] == 0,
         'Compiled visual state is not bound to the real authored imported rig')
    observation = dict(tick=game.tick, actor_position=position(actor), clip_time=clock['time'],
        maximum_joint_matrix_error=joint_error if compare_source_pose else None,
        source_pose_compared=compare_source_pose, active_transition=clock.get('transition'), attachment_matrix_error=root_error,
        lowest_world_vertex_y=lowest, feet_root_height_error=source_floor_error, forward_alignment=alignment)
    record.setdefault('visual_observations', []).append(observation)
    return dict(actor=actor, rig=rig, nodes=nodes, configuration=configuration, source=source, observation=observation)


def stable_snapshot(game, oracle, authored, record):
    observed = avatar(game, oracle, authored, record)
    inspect = game.inspect()
    module = game.module()
    route = typed(game, ROUTE)
    ui = game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick))
    return normalized(dict(tick=game.tick, actor=observed['actor'], rig=observed['rig'],
        nodes=observed['nodes'], config=observed['configuration'], route=route, values=module['module']['values'],
        gameplay_revision=module['revision'], backend=module['module']['backend'],
        revisions=dict(component_revision=route['component_revision'],
            **{key: inspect[key] for key in ('structure_revision', 'ui_revision', 'control_sequence')}), ui=ui))


def load_launcher():
    path = ROOT/'examples/character-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_character_yard_sample', path)
    need(spec is not None and spec.loader is not None, 'Sample launcher is absent')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def continuation_projection(snapshot):
    # The sample deliberately records the current host save epoch on first Tick
    # after exact restoration. Epoch identity is different for each replacement.
    # Retain every other value, revision, UI element and all native/joint state.
    result = json.loads(json.dumps(snapshot))
    for key in ('ObservedEpochHigh', 'ObservedEpochLow'):
        result['values'].pop(key)
    return result


def label(game, identity):
    ui = game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick))
    return next(element['text'] for element in ui['elements'] if element['id'] == FIELD(identity))



def qualify_controls(game, launcher, record, phase):
    """Virtual native layout plus actual runtime eligibility, not physical input."""
    state = game.rpc('runtime.ui.inspect', dict(session_id=game.session, tick=game.tick))
    runtime = {row['id']: row for row in state['elements']}
    for action, identity in launcher.ACTIONS.items():
        live = runtime[FIELD(identity)]
        need(live['eligible'] and live['effective_visible'] and live['effective_enabled'] and live['action'] == action,
             'Actual runtime action is not eligible: '+action)
    descriptor = game.rpc('world.describe', dict(view='section', name='ui'))['sections']['ui']
    revision = game.rpc('world.inspect')['revision']
    if not descriptor['presentation_available']:
        game.rpc('world.ui.layout', dict(revision=revision, width=960, height=640, scale=1), error=-32003)
        record.setdefault('ui_layout_observations', []).append(dict(phase=phase, tick=game.tick,
            presentation_available=False, eligibility_checked=list(launcher.ACTIONS), viewports=[],
            skip_reason='Actual build discovery reports native UI presentation unavailable; layout rejects -32003. Logical runtime eligibility remains verified.'))
        return False
    observations = []
    for width, height in ((960, 640), (512, 288)):
        layout = game.rpc('world.ui.layout', dict(revision=revision, width=width, height=height, scale=1))
        controls = {row['id']: row for row in layout['controls']}
        buttons = []
        for action, identity in launcher.ACTIONS.items():
            identity = FIELD(identity)
            row, live = controls[identity], runtime[identity]
            need(live['eligible'] and live['effective_visible'] and live['effective_enabled'] and live['action'] == action,
                 'Actual runtime action is not eligible: '+action)
            need(row['kind'] == 'button' and row['visible'] and row['enabled'] and row['hittable'],
                 'Native virtual-viewport button is not hittable: '+action)
            x0, y0, x1, y1 = row['bounds']
            need(0 <= x0 < x1 <= width+.01 and 0 <= y0 < y1 <= height+.01,
                 'Action bounds fall outside native viewport: '+action)
            cx0, cy0, cx1, cy1 = row['clip']
            need(cx0 <= x0+.01 and cy0 <= y0+.01 and x1 <= cx1+.01 and y1 <= cy1+.01,
                 'Action is partially clipped: '+action)
            for other in buttons:
                a0, b0, a1, b1 = other['bounds']
                need(min(x1,a1) <= max(x0,a0)+.01 or min(y1,b1) <= max(y0,b0)+.01,
                     'Action buttons overlap and can occlude one another')
            buttons.append(dict(action=action, **row))
        observations.append(dict(width=width, height=height, scale=1, controls=buttons))
    record.setdefault('ui_layout_observations', []).append(dict(phase=phase, tick=game.tick,
        scope='world.ui.layout virtual viewport and runtime.ui.inspect eligibility; not live focus, scroll or physical input',
        presentation_available=True, viewports=observations))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'source', 'manifest', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    for name in ('assembly', 'bridge', 'hostfxr', 'descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    if bool(args.descriptor) == bool(args.assembly):
        parser.error('Supply exactly one compiled assembly or native descriptor')
    if not args.descriptor and not (args.bridge and args.hostfxr):
        parser.error('CoreCLR requires --assembly, --bridge and --hostfxr')
    if not math.isfinite(args.timeout) or not 30 <= args.timeout <= 1200:
        parser.error('Timeout must be finite and between30 and1200seconds')
    inputs = {name: getattr(args, name) for name in ('binary', 'source', 'manifest', 'assembly', 'bridge', 'hostfxr', 'descriptor') if getattr(args, name)}
    for name, path in inputs.items():
        if not path.is_file():
            parser.error('Missing '+name+' input')
    if args.source.stat().st_size != 50116 or sha(args.source) != SOURCE_SHA:
        parser.error('Supply the pinned unchanged original RiggedFigure.glb')
    if args.descriptor:
        descriptor = json.loads(args.descriptor.read_text(encoding='utf-8'))
        need(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2,
             'Expected published native descriptor with explicit file inventory')
        library_found = False
        for index, item in enumerate(descriptor['files']):
            relative = Path(item['path'])
            need(not relative.is_absolute() and '..' not in relative.parts,
                 'Native artifact member must remain beneath its supplied descriptor')
            path = args.descriptor.parent/relative
            need(path.is_file() and path.stat().st_size == item['size'] and sha(path) == item['sha256'],
                 'Native artifact member does not match its published size/hash')
            inputs['native_artifact_'+str(index)] = path
            library_found |= item['path'] == descriptor['library'] and item['role'] == 'library'
        need(library_found, 'Native descriptor does not inventory its actual compiled library')
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('--output must be new; previous runs are preserved')
    launcher = load_launcher()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    oracle = SourceOracle(args.source.read_bytes())
    need(manifest['format'] == 'poima.components' and {schema['id'] for schema in manifest['schemas']} == {CONFIG, ROUTE},
         'Expected compiled Character Yard component manifest')
    schemas = {schema['id']: schema for schema in manifest['schemas']}
    route_fields = {field['id']: field for field in schemas[ROUTE]['fields']}
    need(route_fields[COORDINATES]['kind'] == 'array' and route_fields[COORDINATES]['element_kind'] == 'float32' and
         route_fields[COORDINATES]['capacity'] == 30 and route_fields[CURSOR]['kind'] == 'int32', 'Actual typed route schema differs')
    source_paths = [Path(__file__), ROOT/'tests/imported_character_contract.py', ROOT/'examples/character-yard/run.py',
                    ROOT/'examples/character-yard/CharacterYardGame.cs', ROOT/'examples/character-yard/Poima.CharacterYardGame.csproj',
                    *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
    record = dict(passed=False, backend='native_aot' if args.descriptor else 'coreclr', calls=[], owners=[], checks=[],
        cleanup_errors=[], hashes={name: sha(path) for name, path in inputs.items()},
        sources={str(path.relative_to(ROOT)): sha(path) for path in source_paths},
        source_url=SOURCE_URL, attribution='Rigged Figure, copyright2017 Cesium, CC-BY-4.0; https://creativecommons.org/licenses/by/4.0/',
        continuation_exclusions=['values.ObservedEpochHigh', 'values.ObservedEpochLow'],
        limits=['One imported19-joint stationary arm-opening source motion; this is not walking, motion matching, retargeting or production character quality.',
                'All movement uses compiled navigation queries and native character inputs; no teleports, route edits, gameplay-value patches or artifact builds.',
                'Immediate exact restores compare all snapshots after removing only session IDs; continued comparisons exclude only explicitly named save-epoch identities.',
                'Original-source poses are compared after active inertial transitions finish; transient joints remain in exact continuation comparisons.',
                'Optional renderer readbacks are correctness evidence, not OS screenshots, physical input, editor usability or performance measurements.'])
    deadline = time.monotonic()+args.timeout
    owners = []
    world = output/'world.json'

    def native(path):
        text = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', text], text=True, timeout=10).strip() if args.windows_interop and os.name != 'nt' else text

    def open_game(label_):
        game = launcher.OwnedGame(args, world, label_, record, deadline, native, manifest)
        owners.append(game)
        return game

    def check(name):
        record['checks'].append(name)

    def snapshot(game):
        return stable_snapshot(game, oracle, authored, record)

    def exact_restore(game):
        game.restore('midroute')
        actual = snapshot(game)
        need(actual == checkpoint, 'Immediate exact checkpoint restore differs from the complete saved snapshot')
        return actual

    def capture(game, name):
        if not args.capture:
            return
        path = output/(name+'.bmp')
        need(not path.exists(), 'Capture path already exists')
        state = game.inspect()
        result = game.rpc('runtime.capture', dict(session_id=game.session, tick=game.tick, ui_revision=state['ui_revision'],
            camera=FIELD(launcher.OBSERVER), path=native(path), width=960, height=640, gpu=args.gpu, samples=4, ui_scale=1, profile=True), timeout=180)
        need(result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, 'Actual yard Vulkan capture failed')
        need(result['render_diagnostics']['last_draws']['skinned_instances'] == 1 and
             result['render_diagnostics']['last_draws']['skinned_vertices'] == 370, 'Yard did not render the actual imported weighted character')
        record.setdefault('captures', []).append(dict(name=name, sha256=sha(path), report=result))

    try:
        game = open_game('yard')
        imported = game.author(args.source)
        record['authored'] = imported
        need((imported['imported']['nodes'], imported['imported']['vertices'], imported['imported']['triangles'],
              imported['imported']['skins'], imported['imported']['animations']) == (22, 370, 256, 1, 1), 'Actual original import profile differs')
        authored_bytes = world.read_bytes()
        authored = json.loads(authored_bytes)
        need(authored['entities'][FIELD(launcher.RIG)]['parent'] == ACTOR, 'Visual must be structurally parented to actual movement root')
        need(authored['entities'][ACTOR]['components']['CharacterController']['camera'] is None and
             authored['entities'][FIELD(launcher.PLAYER)]['components']['CharacterController']['camera'] == FIELD(launcher.CAMERA),
             'NPC and first-person player controller are not independent')
        game.start()
        game.load()
        game.step(16)
        initial = avatar(game, oracle, authored, record)
        need(initial['observation']['source_pose_compared'] and initial['rig']['animation']['time'] == 0 and
             not initial['rig']['animation']['playing'] and game.values()['Phase'] == 0, 'Ready state does not hold source pose after blend settles')
        initial_config = initial['configuration']['values']
        need(initial_config[FIELD(10)] == 0 and initial_config[FIELD(2)] == initial_config[FIELD(3)] == 0,
             'Stationary original motion must not be implicitly looped or mislabeled as another clip')
        if qualify_controls(game, launcher, record, 'ready'):
            check('All seven authored native buttons fit without overlap and are hittable at960x640 and512x288, with actual runtime actions eligible; no physical input claim.')
        else:
            check('All seven logical runtime controls are eligible; native layout/hittability explicitly skipped because this build reports presentation unavailable and rejects layout.')
        capture(game, 'ready')
        game.step(120)
        mid = avatar(game, oracle, authored, record)
        values = game.values()
        route = game.route()['values']
        coordinates = route[COORDINATES]
        need(values['Plans'] == 1 and values['Arrivals'] == 0 and values['LastPathStatus'] == 0 and
             values['LastCornerCount'] >= 4 and len(coordinates) == 3*values['LastCornerCount'], 'Compiled route is absent, incomplete or already terminal')
        need(any(abs(coordinates[index+2]) > 3.25 for index in range(0, len(coordinates), 3)), 'Persisted route ignores solid cover clearance')
        ray = game.rpc('runtime.raycast', dict(session_id=game.session, tick=game.tick,
            origin=[-4, .8, 0], direction=[1, 0, 0], distance=8, ignore=[ACTOR]))
        need(ray['hit']['entity'] == FIELD(2), 'Independent native ray does not confirm direct shortcut is blocked')
        need(math.hypot(position(mid['actor'])[0]+4, position(mid['actor'])[2]) > .5, 'Actual native movement has not left its home position')
        check('Imported source rig follows camera-free native character root, typed complete navigation route and blocked direct ray; no game state was patched.')
        capture(game, 'midroute')
        game.saves(output/'saves')
        checkpoint = snapshot(game)
        written = game.write('midroute')
        payloads = list((output/'saves/slot-midroute').glob('p-*.bin'))
        need(len(payloads) == 1 and sha(payloads[0]) == written['sha256'], 'Verified checkpoint bytes do not match native disk receipt')
        record['checkpoint_payload_sha256'] = written['sha256']
        record['checkpoint'] = checkpoint

        if args.descriptor:
            before = snapshot(game)
            game.load(error=-32060)
            need(snapshot(game) == before, 'Unsupported native module replacement changed the running sample')
            check('NativeAOT replacement rejects atomically without altering compiled/native/typed/UI state.')
        else:
            before = snapshot(game)
            game.load()
            after = snapshot(game)
            need(all(after[key] == before[key] for key in before if key != 'gameplay_revision'),
                 'Compatible CoreCLR reload changes native character, imported joints, route, configuration or gameplay/UI state')
            need(after['gameplay_revision'] > before['gameplay_revision'], 'Compatible reload did not publish a new module revision')
            check('Actual unchanged-assembly CoreCLR reload preserves complete native, imported-joint, typed route/visual and gameplay/UI state.')
        exact_restore(game)
        game.step(42)
        continued = snapshot(game)
        record['grouped_continuation'] = continued
        exact_restore(game)
        partitions = [1, 7, 3, 11, 5, 2, 13]
        for count in partitions:
            game.step(count)
        partitioned = snapshot(game)
        need(continuation_projection(partitioned) == continuation_projection(continued), 'Unequal tick partitions differ from grouped native/gameplay continuation')
        exact_restore(game)
        game.control('dispatch')
        game.step(15)
        redispatched = snapshot(game)
        need(redispatched['values']['Dispatches'] == checkpoint['values']['Dispatches']+1 and
             redispatched['values']['Plans'] == checkpoint['values']['Plans']+1, 'UI re-dispatch did not invoke a new compiled route query')
        record['immediate_redispatch'] = redispatched
        check('Immediate exact disk restore is complete;42-tick grouped and unequal partitions agree except newly allocated host save-epoch identity.')

        exact_restore(game)
        paused = game.control('pause')
        need(paused['intent'] == 2 and game.values()['Paused'] == 1, 'Actual pause UI did not publish host pause intent')
        # A pause intent belongs to the frame host: do not force simulation ticks
        # while paused and then misinterpret deliberate CLI stepping as a freeze.
        saved = game.control('save')
        need(saved['save_serviced'] and saved['save_operation']['state'] == 3 and
             saved['save_operation']['error_code'] == 0, 'Compiled UI save did not complete successfully in native storage')
        raw_ui_checkpoint = snapshot(game)
        game.control('status')
        save_values = game.values()
        need(save_values['SavePending'] == 0 and save_values['LastSaveState'] == 3 and
             save_values['LastSaveError'] == 0 and int(save_values['LastSaveGeneration']) == 1 and
             label(game, 702) == 'Checkpoint saved.', 'Paused Status does not report actual terminal save success')
        operation = saved['save_operation']
        result = game.rpc('runtime.save.result', dict(epoch=operation['epoch'], sequence=operation['sequence']))
        need(result['state'] == 3 and result['error_code'] == 0, 'HUD success is not backed by native save result')
        stored = game.rpc('save.inspect', dict(slot='character-yard'))
        payloads = list((output/'saves/slot-character-yard').glob('p-*.bin'))
        need(stored['selected']['verified'] and len(payloads) == 1 and sha(payloads[0]) == stored['selected']['sha256'], 'UI save lacks verified matching disk payload')
        game.control('resume')
        game.step(25)
        loaded = game.control('load')
        need(loaded['save_serviced'] and loaded['runtime_replaced'] and loaded['save_operation']['state'] == 3,
             'Actual compiled UI load did not replace native runtime from disk')
        need(snapshot(game) == raw_ui_checkpoint, 'Compiled UI load changed the complete immediate saved checkpoint')
        game.control('status')
        load_values = game.values()
        need(load_values['Paused'] == 1 and load_values['SavePending'] == 0 and load_values['LastSaveState'] == 3 and
             load_values['LastSaveError'] == 0 and int(load_values['LastSaveGeneration']) == 1 and
             label(game, 702) == 'Checkpoint loaded.', 'Paused Status does not reconcile actual completed load/restore')
        resumed = game.control('resume')
        need(resumed['intent'] == 1 and game.values()['Paused'] == 0, 'Actual UI resume did not publish host resume intent')
        record['ui_save_operation'] = saved
        record['ui_load_operation'] = loaded
        check('Actual compiled pause/save/status/resume/load controls produce terminal native receipts, matching disk bytes, immediate exact restoration and honest save/load HUD.')
        game.close()

        fresh = open_game('fresh')
        fresh.saves(output/'saves')
        exact_restore(fresh)
        fresh.step(42)
        fresh_continuation = snapshot(fresh)
        need(continuation_projection(fresh_continuation) == continuation_projection(continued), 'Fresh owner grouped continuation differs')
        exact_restore(fresh)
        for _ in range(42):
            fresh.step()
        single_continuation = snapshot(fresh)
        need(continuation_projection(single_continuation) == continuation_projection(continued), 'Fresh owner single ticks differ from grouped continuation')
        exact_restore(fresh)
        fresh.control('dispatch')
        fresh.step(15)
        fresh_redispatch = snapshot(fresh)
        need(continuation_projection(fresh_redispatch) == continuation_projection(redispatched), 'Fresh-owner immediate UI re-dispatch diverged')
        record['fresh_grouped_continuation'] = fresh_continuation
        record['fresh_single_continuation'] = single_continuation
        record['fresh_redispatch'] = fresh_redispatch
        check('Same and fresh owner immediate exact checkpoint,42 grouped/unequally partitioned/single ticks and real UI re-dispatch retain complete stable state; only host save-epoch identities differ after Tick.')

        exact_restore(fresh)
        positions = []
        previous = position(fresh.entity())
        for _ in range(1400):
            actual = position(fresh.entity())
            need(not (abs(actual[0]) < .74 and abs(actual[2]) < 3.24), 'Observed native capsule crossed solid cover')
            displacement = math.dist(actual, previous)
            need(displacement <= .08, 'Native courier moved farther than physical per-tick speed bound; possible teleport')
            positions.append(actual)
            previous = actual
            values = fresh.values()
            if values['Arrivals'] == 1:
                break
            need(values['Phase'] != 3, 'Compiled route became blocked')
            fresh.step()
        need(fresh.values()['Arrivals'] == 1 and math.hypot(positions[-1][0]-4, positions[-1][2]) < .16,
             'Independent native positions did not reach delivery station')
        need(any(abs(actual[2]) > 3.25 for actual in positions), 'Actual movement did not traverse the planned cover detour')
        fresh.step(20)
        delivered = avatar(fresh, oracle, authored, record)
        need(delivered['observation']['source_pose_compared'] and abs(delivered['rig']['animation']['time']-oracle.duration) < 1e-8 and
             not delivered['rig']['animation']['playing'], 'Nonloop source clip did not finish and hold its final pose')
        stationary = position(delivered['actor'])
        route_before = fresh.route()['values']
        fresh.step(15)
        need(math.dist(position(fresh.entity()), stationary) < .001 and fresh.route()['values'] == route_before,
             'Completed native route keeps moving or mutates persisted route')
        capture(fresh, 'delivered')
        record['route_positions'] = positions
        record['delivered'] = snapshot(fresh)
        check('Every independently observed native tick stays outside solid cover, respects movement speed and reaches delivery; imported source motion stops at its nonloop endpoint.')

        fresh.control('gesture')
        fresh.step(24)
        gesture = avatar(fresh, oracle, authored, record)
        need(gesture['observation']['source_pose_compared'] and 0 < gesture['rig']['animation']['time'] < oracle.duration and
             gesture['rig']['animation']['playing'] and not gesture['rig']['animation']['loop'], 'Real Gesture UI did not replay original motion once')
        need(math.dist(position(gesture['actor']), stationary) < .001, 'Source gesture moved the native capsule')
        qualify_controls(fresh, launcher, record, 'gesture')
        capture(fresh, 'gesture')
        fresh.step(90)
        finished = avatar(fresh, oracle, authored, record)
        need(finished['rig']['animation']['time'] == oracle.duration and not finished['rig']['animation']['playing'] and
             fresh.values()['GestureActive'] == 0, 'Explicit gesture did not stop after one source take')
        fresh.control('dispatch')
        fresh.step(15)
        need(fresh.values()['Plans'] == 2 and fresh.values()['Dispatches'] == 2 and fresh.values()['Destination'] == 0,
             'Delivered courier could not use UI to plan return delivery')
        check('Gesture replays the original arm-opening source once without capsule movement; delivered courier can dispatch a compiled return route.')
        need(world.read_bytes() == authored_bytes and sha(args.source) == SOURCE_SHA, 'Runtime or launcher modified source/authored world')
        need(all(sha(path) == record['hashes'][name] for name, path in inputs.items()), 'Supplied executable/module inputs changed during qualification')
        need(all(sha(ROOT/path) == expected for path, expected in record['sources'].items()), 'Test/sample inputs changed during qualification')
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        for game in reversed(owners):
            try:
                game.close()
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        record['passed'] = record['passed'] and not record['cleanup_errors']
        record['rpc_count'] = len(record['calls'])
        record['method_counts'] = dict(sorted(Counter(row['method'] for row in record['calls']).items()))
        with (output/'evidence.json').open('x', encoding='utf-8') as stream:
            json.dump(record, stream, indent=2)
            stream.write('\n')
    print(json.dumps(dict(passed=record['passed'], checks=len(record['checks']), rpc_count=record['rpc_count'], owners=len(record['owners']))))
    if not record['passed']:
        print(record.get('error') or record['cleanup_errors'], file=sys.stderr)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
