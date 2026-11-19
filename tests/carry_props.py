#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Play the compiled carry loop and restore a game saved during a blocked drop."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

from item_attachment_gameplay import Run, sha
from item_attachment_release import body_state
from item_attachment_surface_gameplay import configuration, parameters
from item_reach_pickup import (
    ACTOR, CAMERA, FAR, FAR_SUPPORT, FEATURES, FLOOR, HAND, ITEM, PROXY, RIG, SUPPORT, WALL,
    configure, settings,
)
from runtime_pose_frames import id_request, transform, uid
import world_physical_contract as native

GAME = 'Poima.Examples.CarryPropsGame'
REQUIRED = FEATURES | {'item_attachment_aim_v1', 'item_attachment_release_v1'}
CONFIG = 'c913ebece457b13dc5ce02fe05512b62'
PROMPT = uid(81)
INITIAL = dict(settings(), Phase=-1, AimTicks=12, RetryTicks=24)


class CarryRun(Run):
    def state(self):
        return self.game()['values']

    def ui(self):
        view = self.call('runtime.ui.inspect', tick=self.tick, limit=16)
        row, = view['elements']
        assert row['id'] == PROMPT
        return view['ui_revision'], row

    def expect_prompt(self, text):
        _, row = self.ui()
        assert (row['text'], row['visible']) == (text, bool(text)), row

    def save(self, slot):
        revision, _ = self.ui()
        components = self.call('runtime.components')
        assert components['tick'] == self.tick
        result = self.call('save.write', request_id=id_request(), configuration_generation=1,
            slot=slot, expected_generation=0, expected_tick=self.tick,
            expected_gameplay_revision=self.game_revision, expected_structure_revision=self.structure,
            expected_ui_revision=revision, expected_control_sequence=0,
            expected_component_revision=components['component_revision'])
        data = self.bytes(slot)
        assert len(data) == result['bytes']
        assert sha(next((self.saves / f'slot-{slot}').glob('p-*.bin'))) == result['sha256']
        return data

    def run_ticks(self, ticks, *, action=None, look=0, move=0, motions=()):
        controls = [dict(name=action, pressed=True, released=True, held=False)] if action else []
        result = self.call('runtime.step', request_id=id_request(), expected_tick=self.tick,
            expected_structure_revision=self.structure, ticks=ticks, motions=list(motions),
            inputs=[dict(entity=ACTOR, look=[0, look], move=[0, move], actions=controls)])
        self.tick += ticks
        assert result['tick'] == self.tick
        self.structure = self.owner.rpc('runtime.status')['structure_revision']

    def move_wall(self, position):
        self.run_ticks(6, motions=[dict(entity=WALL, position=position, rotation=[0,0,0,1], duration_ticks=6)])

    def visible_item(self):
        camera = self.matrix(CAMERA)
        return self.call('runtime.attachment.aim', tick=self.tick, structure_revision=self.structure,
            origin=camera[12:15], direction=[-x for x in camera[8:11]], distance=2, ignore=[ACTOR])['item']

    def outcome(self):
        row, = self.call('runtime.attachment.outcomes', tick=self.tick, structure_revision=self.structure)['outcomes']
        assert row['entity'] == ITEM
        return row

    def binding(self):
        row, = self.bindings()
        assert row['entity'] == ITEM
        return row


def author(owner, manifest, wall_scale=(.32,.28,.05), rig_degrees=None):
    revision = configure(owner, aimed=True)
    added = owner.rpc('component.schema.import', dict(request_id=id_request(), base_revision=revision, manifest=manifest))
    assert {schema['id'] for schema in manifest['schemas']} == {CONFIG}
    q = math.sqrt(.5)
    rig_rotation = (-q,0,0,q)
    if rig_degrees is not None:
        half_angle = math.radians(rig_degrees)/2
        rig_rotation = (math.sin(half_angle),0,0,math.cos(half_angle))
    prompt = dict(name='Prop interaction', kind='label', parent=None, action=None, text='', visible=False, enabled=True,
        layout=dict(position='absolute', bottom=dict(unit='dp', value=20), left=dict(unit='percent', value=29),
                    width=dict(unit='percent', value=42), padding=[7,14,7,14]),
        style=dict(color='#fff2de',background_color='#221a19e8',border_color='#94745e',
                   border_width=1,border_radius=6,font_size=20,text_align='center'))
    ops = [
        native.component(RIG,'Transform',transform((0,0,0),rotation=rig_rotation)),
        native.component(WALL,'Transform',transform((1.5,1.05,-.36), scale=wall_scale)),
        dict(op='component.set', id=ACTOR, type='game:'+CONFIG,
            value={uid(1):'0',uid(2):str(int(PROXY,16)),uid(3):'0',uid(4):str(int(PROMPT,16)),
                   uid(5):7,uid(6):9,uid(7):CAMERA,uid(8):SUPPORT}),
        dict(op='ui.element.set', id=PROMPT, element=prompt)
    ]
    # Display the exact authored supports. Visual trim adds spatial context
    # without changing the colliders or calibration used by the observations.
    for number, name, parent, at, size, color in (
        (90,'Workbench top',SUPPORT,(0,0,0),(.4,.1,.18),(.24,.13,.075)),
        (91,'Workbench leg',SUPPORT,(0,-.49,0),(.29,.92,.14),(.15,.085,.052)),
        (92,'Rear shelf top',FAR_SUPPORT,(0,0,0),(.4,.1,.18),(.24,.13,.075)),
        (93,'Floor surface',FLOOR,(0,0,0),(20,.8,20),(.17,.22,.22)),
        (94,'Back wall',None,(0,1.3,-2.4),(5,2.6,.12),(.13,.20,.21)),
        (95,'Wall trim',None,(0,.8,-2.32),(5,.10,.024),(.32,.23,.14)),
    ):
        entity = uid(number)
        ops.extend([dict(op='entity.create',id=entity,name=name,parent=parent),
            native.component(entity,'Transform',transform(at,scale=size)),
            native.component(entity,'MeshRenderer',dict(primitive='box',albedo=color,visible=True))])
    for item in (ITEM,FAR):
        ops.append(native.component(item,'PbrMaterial',dict(base_color=[.33,.65,.54],
            metallic=.3,roughness=.42,emissive=[0,0,0],double_sided=True)))
    for number, item in ((96,ITEM),(97,FAR)):
        edge = uid(number)
        ops.extend([dict(op='entity.create',id=edge,name='Raised handle',parent=item),
            native.component(edge,'Transform',transform((0,0,.012),scale=(.18,.028,.060))),
            native.component(edge,'MeshRenderer',dict(primitive='box',albedo=[.22,.45,.36],visible=True)),
            native.component(edge,'CameraDraw',dict(camera=CAMERA,mode='viewmodel'))])
    return native.transact(owner,ops,added['revision'])['revision']


def pickup(game):
    game.run_ticks(1, action='pickup')
    state = game.state()
    assert state['Phase'] == 1 and state['Item'] == ITEM, state
    game.expect_prompt('Reaching')
    for _ in range(53):
        game.run_ticks(1, move=.77 if _ < 19 else 0)
        state = game.state()
        if state['Phase'] == 3:
            break
    assert state['Phase'] == 3 and state['Item'] == ITEM and game.binding()['anchor'] == HAND, state
    game.expect_prompt('Drop')
    return int(state['PickupTick'])


def capture(game, args, name, camera=CAMERA):
    if args.gpu is None:
        return None
    path = args.output / (name + '.bmp')
    revision, _ = game.ui()
    frame = game.call('runtime.capture', tick=game.tick, structure_revision=game.structure,
        ui_revision=revision, camera=camera, path=native.native(path), width=640, height=360,
        samples=1, gpu=args.gpu)
    assert frame['capture_written'] and frame['hardware'] and frame['nvrhi_errors'] == 0
    return dict(tick=frame['tick'], gpu=frame['gpu'], sha256=sha(path))


def finish_drop(game, wall_clear, args=None):
    game.run_ticks(1)
    game.expect_prompt('No room to drop')
    image_blocked = capture(game,args,'blocked_drop') if args else None
    game.move_wall(wall_clear)
    game.run_ticks(3)
    game.expect_prompt('Drop')
    assert game.state()['Opening'] == 0

    game.run_ticks(1, action='drop')
    game.run_ticks(6)
    release_tick = game.tick
    release = game.outcome()
    assert release['status'] == 'released', release
    proxy = release['anchor']
    assert game.binding()['released'] and game.binding()['anchor'] == proxy
    image_released = capture(game,args,'released') if args else None
    game.run_ticks(140)
    position = game.matrix(proxy)[12:15]
    camera = game.matrix(CAMERA)
    actual_pitch = math.degrees(math.atan2(-camera[9], camera[10]))
    target_pitch = math.degrees(math.atan2(position[1]-camera[13],camera[14]-position[2]))
    game.run_ticks(2,look=target_pitch-actual_pitch)
    assert game.visible_item() == ITEM
    game.expect_prompt('Pick up')
    second_tick = pickup(game)
    game.call('runtime.entity', error=-32004, tick=game.tick, structure_revision=game.structure, id=proxy)
    return dict(release_tick=release_tick, second_pickup=second_tick, proxy=proxy,
                captures=dict(blocked=image_blocked,released=image_released))


def drive(game, args=None):
    game.run_ticks(138)
    game.run_ticks(2,look=-9)
    assert game.binding()['released']
    initial_proxy = game.binding()['anchor']
    assert not body_state(game.save('settled'),initial_proxy)['active']
    game.expect_prompt('Pick up')
    ready = capture(game,args,'ready') if args else None
    first_tick = pickup(game)
    carried = capture(game,args,'carried') if args else None
    game.call('runtime.entity', error=-32004, tick=game.tick, structure_revision=game.structure, id=initial_proxy)

    held = game.matrix(ITEM)[12:15]
    wall_clear = [1.5,held[1],held[2]]
    game.move_wall(held)
    game.run_ticks(1,action='drop')
    game.run_ticks(2)
    state = game.state()
    assert state['Phase'] == 4 and 0 < state['Opening'] < .8, state
    game.expect_prompt('Releasing')
    game.run_ticks(4)
    blocked = game.outcome()
    assert blocked['status'] == 'release_blocked' and blocked['obstacle'] == WALL, blocked
    assert game.binding()['anchor'] == HAND and game.structure == 3
    boundary = (game.tick,game.structure,game.game_revision)
    checkpoint = game.save('blocked')
    result = finish_drop(game,wall_clear,args)
    result['captures'].update(ready=ready,carried=carried)
    assert result['proxy'] != initial_proxy and game.structure == 5
    final = game.save('final')
    compact = dict(first_pickup=first_tick, initial_proxy=initial_proxy, blocked=blocked, **result)
    return compact, (boundary, checkpoint, wall_clear, final)


def verify_export(args, world, revision, checkpoint, final):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/python'))
    from poima_client import RpcError, WorldClient

    class GameOwner:
        def __init__(self, client):
            self.client = client
            self.calls = 0

        def rpc(self, method, params=None, error=None):
            self.calls += 1
            try:
                result = self.client.call(method,params or {},timeout=45)
            except RpcError as exc:
                assert error == exc.code, exc
                return
            assert error is None
            return result

    install = args.runtime.resolve()
    executable = 'poima.exe'
    engine = install/'bin'/executable
    output = args.output/'delivery'
    source = output/'source'
    source.mkdir(parents=True)
    authored = source/'world.json'
    shutil.copyfile(world,authored)
    shutil.copytree(world.with_suffix('.json.assets'),authored.with_suffix('.json.assets'))
    profile_path = source/'controls.poima-input.json'
    with WorldClient.open(str(engine),native.native(authored),trace_directory=output/'profile') as client:
        profile = client.call('input.describe')['defaults']
        profile['bindings']['use'] = []
        profile['actions'] = dict(pickup=['key.e'],drop=['key.g'],cancel_pickup=['key.q'])
        client.call('input.transact',dict(path=native.native(profile_path),request_id=id_request(),
            expected_revision=0,profile=profile))
    project = source/'project.json'
    project.write_text(json.dumps(dict(format='poima.project',version=1,project_id=id_request(),
        name='Carry Props',entry=dict(world=authored.name,controller=ACTOR,camera=CAMERA),
        input_profile=profile_path.name,audio=False),indent=2)+'\n')
    request = output/'adopt.json'
    request.write_text(json.dumps(dict(request_id=id_request(),expected_project_sha256=sha(project),
        expected_artifact_sha256=sha(args.descriptor),values=INITIAL))+'\n')

    def cli(*parts):
        argv = [str(engine), *(native.native(part) if isinstance(part,Path) else part for part in parts)]
        run = subprocess.run(argv,capture_output=True,encoding='utf-8',timeout=120)
        with (output/'cli.jsonl').open('a') as stream:
            stream.write(json.dumps(dict(argv=argv,exit=run.returncode,stdout=run.stdout,stderr=run.stderr))+'\n')
        response = json.loads(run.stdout)
        assert run.returncode == 0 and response['status'] == 'ok', response
        return response['result']

    cli('project','adopt-gameplay',project,'--artifact',args.descriptor,'--request',request)
    staged = output/'game'
    cli('project','build',project,'--runtime',install,'--output',staged)
    relocated = output/'Relocated Carry Props'
    staged.rename(relocated)
    source.rename(output/'source-offline')
    manifest = relocated/'game.json'
    inspected = cli('game','inspect',manifest)
    assert inspected['revision'] == revision and inspected['input_profile']
    binary = relocated/'runtime/bin'/executable
    saves = output/'saves'
    saves.mkdir()

    def inventory():
        return {path.relative_to(relocated).as_posix():sha(path) for path in relocated.rglob('*') if path.is_file()}

    frozen = inventory()
    with WorldClient.open_game(str(binary),native.native(manifest),save_root=native.native(saves),
                              trace_directory=output/'warm') as client:
        owner = GameOwner(client)
        assert owner.rpc('world.describe')['read_only']
        game = CarryRun(owner,revision,saves,{},True,configure_saves=False)
        game.id = owner.rpc('runtime.status')['session_id']
        base = game.call('runtime.gameplay.inspect',tick=0)
        game.game_revision = base['revision']
        _, (boundary, bundled_blocked, wall_clear, bundled_final) = drive(game)
        assert bundled_blocked == checkpoint and bundled_final == final
    assert client.transport.returncode == 0
    warm_calls = owner.calls

    with WorldClient.open_game(str(binary),native.native(manifest),save_root=native.native(saves),
                              trace_directory=output/'cold') as client:
        owner = GameOwner(client)
        cold = CarryRun(owner,revision,saves,{},True,configure_saves=False)
        state = owner.rpc('runtime.status')
        assert state['tick'] == 0
        loaded = owner.rpc('save.load',dict(request_id=id_request(),configuration_generation=1,slot='blocked',
            expected_generation=1,revision=revision,expected_session_id=state['session_id'],expected_tick=0,
            expected_gameplay_revision=1,expected_structure_revision=0,expected_component_revision=0,
            expected_ui_revision=0,expected_control_sequence=0,new_session_id=cold.id))
        assert loaded['tick'] == boundary[0] and loaded['structure_revision'] == boundary[1]
        cold.tick, cold.structure, cold.game_revision = boundary
        assert cold.save('cold-blocked') == checkpoint
        finish_drop(cold,wall_clear)
        assert cold.save('cold-final') == final
    assert client.transport.returncode == 0 and not source.exists() and inventory() == frozen
    return dict(calls=warm_calls+owner.calls, files=len(frozen), exact_archive_pairs=4,
                engine_sha256=sha(binary), manifest_sha256=sha(manifest))


def verify(args):
    args.output = args.output.resolve()
    args.output.mkdir()
    native.ARGS = args
    native.MAX_CALLS = 850
    native.DEADLINE = time.monotonic()+240
    config = configuration(args,GAME,REQUIRED)
    manifest = args.descriptor.with_name('game.poima-components.json') if args.descriptor else args.manifest
    owner = native.Session(args.output/'warm')
    saves = args.output/'saves'
    saves.mkdir()
    world = owner.world
    report = dict(passed=False)
    try:
        revision = author(owner,json.loads(manifest.read_text(encoding='utf-8-sig')))
        game = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        game.call('runtime.start',revision=revision)
        game.game_revision = game.call('runtime.gameplay.load_native' if args.descriptor else 'runtime.gameplay.load',
            request_id=id_request(), expected_tick=0, expected_structure_revision=0, expected_revision=0,
            values=INITIAL, **config)['revision']
        result, (boundary, checkpoint, wall_clear, final) = drive(game,args)
        owner.close()
        owner = native.Session(args.output/'cold',world=world)
        cold = CarryRun(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('blocked',*boundary)
        assert cold.save('cold-blocked') == checkpoint
        replay = finish_drop(cold,wall_clear)
        assert all(replay[key]==result[key] for key in ('release_tick','second_pickup','proxy'))
        assert cold.save('cold-final') == final
        owner.close()
        report.update(source_calls=len(native.REPORT['calls']),exact_archive_pairs=2,**result)
        if args.runtime:
            report['relocated_windows_game'] = verify_export(args,world,revision,checkpoint,final)
        report['passed'] = True
    finally:
        owner.close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parameters(parser)
    parser.add_argument('--manifest',type=Path,help='Component manifest extracted from the CoreCLR gameplay assembly.')
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--runtime',type=Path,help='Installed Windows runtime for an optional relocated export check.')
    options = parser.parse_args()
    if not options.descriptor and not options.manifest:
        parser.error('Supply --manifest with the CoreCLR gameplay paths.')
    if options.runtime and not options.descriptor:
        parser.error('The relocated Windows game requires the Native AOT descriptor.')
    verify(options)
