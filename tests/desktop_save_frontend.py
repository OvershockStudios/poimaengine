#!/usr/bin/env python3
"""Qualify Save-window controls and durable restoration in the packaged editor.

Uses routed button events and actual TextBoxes in visible script-owned windows.
No physical input, desktop screenshots, or source builds are performed.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree(path):
    return {str(item.relative_to(path)): digest(item)
            for item in path.rglob('*') if item.is_file()}


class Actions:
    def __init__(self):
        self.items = []
        self.frame = 3

    def add(self, op, **fields):
        self.items.append(dict(frame=self.frame, op=op, **fields))
        self.frame += 3

    def text(self, name, value, prefix='Save '):
        self.add('set_text', control=prefix+name, text=str(value))

    def press(self, name, prefix='Save '):
        self.add('click_control', control=prefix+name)

    def check(self, name, value):
        self.add('check_control', control='Save '+name, checked=value)

    def inspect(self, tag):
        self.add('inspect_saves', tag=tag)

    def rpc(self, method, params, **fields):
        self.add('rpc', method=method, params=params, **fields)

    def gameplay(self, assembly):
        self.add('show_gameplay')
        self.text('Assembly', assembly, 'Gameplay ')
        self.text('Game type', 'Poima.Tests.RuntimeSaveProbe', 'Gameplay ')
        self.text('Initial values JSON', json.dumps({'Target': f'{2:032x}'}), 'Gameplay ')
        self.press('Configure launch', 'Gameplay ')

    def module(self, tag):
        self.press('Revert / refresh values', 'Gameplay ')
        self.add('inspect', tag=tag)


def launch(args, run, world, name, actions, environment, record):
    directory = run/name
    directory.mkdir()
    script = directory/'actions.json'
    report = directory/'report.json'
    script.write_text(json.dumps({'actions': actions.items}, indent=2)+'\n')
    command = [str(args.editor), str(world), '--gpu', str(args.gpu), '--endpoint', 'saves-ui-'+uuid.uuid4().hex,
               '--layout', str(directory/'layout.json'), '--script', str(script),
               '--frames', str(actions.frame+15), '--report', str(report)]
    with (directory/'stdout.txt').open('w', encoding='utf-8') as out, (directory/'stderr.txt').open('w', encoding='utf-8') as err:
        process = subprocess.Popen(command, stdout=out, stderr=err, env=environment)
        try:
            process.wait(timeout=120)
        except BaseException:
            process.kill()
            process.wait(timeout=10)
            raise
    data = json.loads(report.read_text()) if report.is_file() else None
    record['phases'].append(dict(name=name, exit_code=process.returncode, report=data))
    assert process.returncode == 0 and data is not None and data['success'], (data, (directory/'stderr.txt').read_text())
    assert len(data['actions']) == len(actions.items)
    stdout = [json.loads(line) for line in (directory/'stdout.txt').read_text().splitlines() if line.startswith('{')]
    assert stdout == [data], 'Managed loading changed the report stdout channel.'
    return {spec['tag']: result for spec, result in zip(actions.items, data['actions']) if 'tag' in spec}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('editor', 'binary', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    assert os.name == 'nt', 'Run with Windows Python; this qualifies the packaged native editor.'
    for name in ('editor', 'binary', 'assembly', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    root = Path(__file__).resolve().parents[1]
    run = args.output/uuid.uuid4().hex
    run.mkdir(parents=True)
    project = run/'Project'; project.mkdir()
    world = project/'world.json'
    saves = run/'Save files'; saves.mkdir()
    marker = run/'initialize-must-not-run'
    environment = dict(os.environ, POIMA_SAVE_FIXTURE_INITIALIZE_MARKER=str(marker))
    record = dict(passed=False, gpu=args.gpu, test_sha256=digest(__file__),
                  binary_sha256=digest(args.binary),
                  frontend_sha256=digest(args.editor.parent/'Poima.Editor.dll'),
                  bridge_sha256=digest(args.editor.parent/'poima_desktop.dll'),
                  assembly_sha256=digest(args.assembly), phases=[],
                  limitations=['Routed control actions exercise actual visible Avalonia controls; no physical keyboard, IME or screen-reader qualification.',
                               'Window content renders are Avalonia visual-tree renders, not desktop screenshots.',
                               'Storage crash durability and broad malformed-file matrices have separate native suites.'])
    try:
        fixture = json.loads((root/'examples/interaction-room.jsonl').read_text())
        authored = subprocess.run([str(args.binary), 'world', str(world)], input=json.dumps(fixture)+'\n',
                                  capture_output=True, text=True, encoding='utf-8', timeout=20)
        assert authored.returncode == 0 and 'result' in json.loads(authored.stdout), authored.stdout+authored.stderr
        world_hash = digest(world)
        sid = uuid.uuid4().hex
        a = Actions()
        a.add('open_saves'); a.inspect('initial')
        a.text('Root', saves)
        a.add('close_saves'); a.inspect('dirty_tool_close')
        a.text('Root', saves)
        a.add('close_guard'); a.inspect('dirty_owner_close')
        a.press('Refresh configuration'); a.text('Root', saves)
        a.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=None))
        a.press('Configure root'); a.inspect('config_conflict')
        a.add('assert_text', control='Save Root', text=str(saves))
        a.press('Dismiss pending'); a.press('Refresh configuration')
        a.text('Root', saves); a.press('Configure root'); a.inspect('configured')
        a.text('Slot', '../outside'); a.press('Inspect slot'); a.inspect('invalid_slot')
        a.add('assert_text', control='Save Slot', text='../outside')
        a.text('Slot', 'quick'); a.press('Inspect slot')
        a.gameplay(args.assembly)
        a.rpc('desktop.play.start', dict(session_id=sid, revision=1, paused=True, expected_gameplay_generation=1))
        a.rpc('desktop.play.step', dict(request_id=uuid.uuid4().hex, session_id=sid, expected_tick=0, ticks=17,
              motions=[dict(entity=f'{2:032x}', position=[3, 1.5, -3], rotation=[0, 0, 0, 1], duration_ticks=120)]))
        a.press('Inspect slot'); a.add('select', id=f'{2:032x}'); a.add('draft_name', name='Unapplied authoring draft')
        a.press('Write checkpoint'); a.inspect('dirty_inspector_write'); a.add('inspect', tag='authoring_draft')
        a.add('reload'); a.press('Write checkpoint'); a.inspect('written_17')
        a.module('module_17'); a.add('runtime_entity', id=f'{2:032x}', tag='door_17')
        a.press('Inspect slot')
        a.rpc('desktop.play.step', dict(request_id=uuid.uuid4().hex, session_id=sid, expected_tick=17, ticks=23))
        a.press('Write checkpoint'); a.inspect('stale_tick')
        a.press('Inspect slot'); a.inspect('pending_after_inspect')
        a.press('Retry pending'); a.inspect('same_retry')
        a.press('Dismiss pending'); a.press('Write checkpoint'); a.inspect('written_40')
        a.module('module_40'); a.add('runtime_entity', id=f'{2:032x}', tag='door_40')
        a.press('Inspect slot'); a.add('step', ticks=5)
        a.press('Load checkpoint'); a.inspect('stale_load')
        a.press('Dismiss pending'); a.press('Inspect slot')
        a.add('draft_name', name='Preserve me on failed Load'); a.press('Load checkpoint')
        a.inspect('dirty_inspector_load'); a.add('inspect', tag='load_authoring_draft')
        a.add('reload'); a.press('Load checkpoint'); a.inspect('loaded_40')
        a.module('restored_module_40'); a.add('runtime_entity', id=f'{2:032x}', tag='restored_door_40')
        a.add('scroll_saves', position='top')
        a.add('render_saves', path=str(run/'save-window-top.png'), tag='visual_top')
        a.add('scroll_saves', position='bottom')
        a.add('render_saves', path=str(run/'save-window-bottom.png'), tag='visual_bottom')
        first = launch(args, run, world, 'write-and-guards', a, environment, record)
        check_first(first, saves, record)
        assert digest(world) == world_hash, 'Save-window actions changed authored world bytes.'
        initial_files = tree(saves)
        marker.write_text('A fresh restore must not call Initialize.\n')

        b = Actions(); b.add('open_saves'); b.text('Root', saves); b.press('Configure root')
        b.gameplay(args.assembly); b.text('Slot', 'quick'); b.press('Inspect slot')
        b.check('Use configured gameplay', True); b.press('Load checkpoint'); b.inspect('fresh_loaded')
        b.module('fresh_module'); b.add('runtime_entity', id=f'{2:032x}', tag='fresh_door')
        b.add('step', ticks=80); b.module('continued_module')
        b.add('runtime_entity', id=f'{2:032x}', tag='continued_door')
        second = launch(args, run, world, 'fresh-process', b, environment, record)
        check_fresh(first, second)
        assert tree(saves) == initial_files and digest(world) == world_hash

        slot = saves/'slot-quick'
        manifest = json.loads((slot/'current.json').read_text())
        assert manifest['payload']['generation'] == 2
        current = manifest['payload']['current']
        payload = slot/current['file']
        assert payload.parent.resolve() == slot.resolve() and payload.is_file()
        corrupt = b'intentional fixture payload corruption\n'
        payload.write_bytes(corrupt)
        damaged_files = tree(saves)
        c = Actions(); c.add('open_saves'); c.text('Root', saves); c.press('Configure root')
        c.gameplay(args.assembly); c.text('Slot', 'quick'); c.press('Inspect slot'); c.inspect('recovery_observed')
        c.check('Use configured gameplay', True); c.press('Load checkpoint'); c.inspect('implicit_recovery_refused')
        c.press('Dismiss pending'); c.check('Allow recovered load', True); c.press('Load checkpoint')
        c.inspect('recovered_17'); c.module('recovered_module'); c.add('runtime_entity', id=f'{2:032x}', tag='recovered_door')
        c.add('render_saves', path=str(run/'save-recovery.png'), tag='recovery_visual')
        c.press('Inspect slot'); c.press('Write checkpoint'); c.inspect('implicit_ack_refused')
        c.press('Dismiss pending'); c.check('Acknowledge payload recovery', True); c.press('Write checkpoint')
        c.inspect('acknowledged_write'); c.press('Inspect slot'); c.inspect('healthy_after_recovery')
        third = launch(args, run, world, 'explicit-recovery', c, environment, record)
        check_recovery(first, third)
        verify_visuals(third, ('recovery_visual',), record)
        assert payload.read_bytes() == corrupt, 'Acknowledged recovery removed or overwrote quarantined evidence.'
        assert digest(world) == world_hash
        final = json.loads((slot/'current.json').read_text())['payload']
        assert final['generation'] == 3
        assert final['quarantine']['file'] == payload.name
        record.update(passed=True, actions=sum(len(phase['report']['actions']) for phase in record['phases']),
                      world_sha256=world_hash, original_store=initial_files, damaged_store=damaged_files,
                      final_store=tree(saves), initialize_guard='armed before both fresh-process restores',
                      checks=['Actual Save-window controls preserve dirty and stale observations.',
                              'Exact pending mutation retry survives explicit observation refresh.',
                              'Saved typed C# state and mid-motion door restore in the same and fresh editor processes.',
                              'Implicit recovery is refused; explicit load and acknowledged write preserve quarantined payload.',
                              'Authored world bytes remain unchanged throughout all phases.'])
    except BaseException as error:
        record['error'] = repr(error)
        raise
    finally:
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(run/'evidence.json')


def module(stage):
    return stage['gameplay_draft']['observation']['module']


def without_session(value):
    if isinstance(value, dict):
        return {key: without_session(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [without_session(item) for item in value]
    return value


def check_first(stages, saves, record):
    state = lambda tag: stages[tag]['saves']
    assert state('initial')['generation'] == 0 and state('initial')['config_observation']['root'] is None
    for tag in ('dirty_tool_close', 'dirty_owner_close'):
        closed = state(tag)
        assert closed['root_dirty'] and closed['dirty'] and closed['root_draft'] == str(saves)
    assert state('dirty_tool_close')['error'] and 'before closing' in state('dirty_tool_close')['error']
    conflict = state('config_conflict')
    assert conflict['root_dirty'] and conflict['config_conflict'] and '(-32009)' in conflict['error']
    assert conflict['root_draft'] == str(saves) and conflict['pending']['method'] == 'save.configure'
    assert state('configured')['generation'] == 2 and not state('configured')['root_dirty']
    assert state('configured')['pending'] is None
    assert state('invalid_slot')['error'] and state('invalid_slot')['slot_draft'] == '../outside'
    for tag, draft_tag, name in [('dirty_inspector_write', 'authoring_draft', 'Unapplied authoring draft'),
                                 ('dirty_inspector_load', 'load_authoring_draft', 'Preserve me on failed Load')]:
        assert state(tag)['error'] and state(tag)['pending'] is None
        assert stages[draft_tag]['draft']['dirty'] and stages[draft_tag]['draft']['name'] == name
    written = state('written_17')
    assert written['last_result']['generation'] == 1 and not written['last_result']['replayed']
    assert written['runtime_observation']['tick'] == 17 and written['observation_stale']
    pending = state('stale_tick')['pending']
    assert pending['method'] == 'save.write' and pending['params']['expected_tick'] == 17
    assert '(-32009)' in state('stale_tick')['error']
    assert state('pending_after_inspect')['pending'] == pending, 'Refresh replaced an unresolved request.'
    assert state('pending_after_inspect')['runtime_observation']['tick'] == 40
    assert state('same_retry')['pending'] == pending and '(-32009)' in state('same_retry')['error']
    assert state('written_40')['last_result']['generation'] == 2 and state('written_40')['pending'] is None
    assert '(-32009)' in state('stale_load')['error'] and state('stale_load')['pending']['method'] == 'save.load'
    assert state('loaded_40')['last_result']['tick'] == 40 and state('loaded_40')['pending'] is None
    before = module(stages['module_40']); restored = module(stages['restored_module_40'])
    assert restored == before, 'UI Load changed typed values, schema, backend or image binding.'
    assert before['values']['Ticks'] == 40 and before['values']['Accumulator'] == '780'
    assert module(stages['module_17'])['values']['Ticks'] == 17
    assert module(stages['module_17'])['values']['Accumulator'] == '136'
    assert without_session(stages['door_40']['result']) == without_session(stages['restored_door_40']['result'])
    assert abs(stages['door_17']['result']['world_matrix'][12]-.425) < 1e-5
    assert abs(stages['door_40']['result']['world_matrix'][12]-1) < 1e-5
    assert stages['door_40']['result']['motion_remaining_ticks'] == 80
    verify_visuals(stages, ('visual_top', 'visual_bottom'), record)


def check_fresh(first, stages):
    loaded = stages['fresh_loaded']['saves']['last_result']
    assert loaded['tick'] == 40 and not loaded['recovered']
    assert module(stages['fresh_module']) == module(first['module_40'])
    assert without_session(stages['fresh_door']['result']) == without_session(first['door_40']['result'])
    values = module(stages['continued_module'])['values']
    assert values['Ticks'] == 120 and values['Accumulator'] == '7140' and values['Minimum'] == str(-(2**63))
    assert values['Fraction'] == 30.25 and values['Precise'] == 15.125
    door = stages['continued_door']['result']
    assert abs(door['world_matrix'][12]-3) < 1e-5 and door['motion_remaining_ticks'] == 0


def check_recovery(first, stages):
    state = lambda tag: stages[tag]['saves']
    observed = state('recovery_observed')['slot_observation']
    assert observed['recovered'] and observed['generation'] == 2
    assert observed['selected']['generation'] == 1 and observed['selected']['verified']
    assert not observed['current']['verified']
    assert '(-32070)' in state('implicit_recovery_refused')['error']
    assert state('implicit_recovery_refused')['pending']['params']['allow_recovery'] is False
    recovered = state('recovered_17')['last_result']
    assert recovered['recovered'] and recovered['tick'] == 17
    assert module(stages['recovered_module']) == module(first['module_17'])
    assert without_session(stages['recovered_door']['result']) == without_session(first['door_17']['result'])
    assert '(-32070)' in state('implicit_ack_refused')['error']
    assert state('implicit_ack_refused')['pending']['params']['acknowledge_recovery'] is False
    assert state('acknowledged_write')['last_result']['generation'] == 3
    healthy = state('healthy_after_recovery')['slot_observation']
    assert not healthy['recovered'] and healthy['current']['verified'] and healthy['selected']['generation'] == 3
    assert healthy['quarantined']['generation'] == 2 and not healthy['quarantined']['verified']


def verify_visuals(stages, names, record):
    from PIL import Image
    for name in names:
        visual = stages[name]['visual']; path = Path(visual['path'])
        assert visual['kind'] == 'avalonia_visual_content' and path.is_file()
        with Image.open(path) as image:
            assert image.size == (visual['width'], visual['height'])
            assert 32 <= image.width <= 4096 and 32 <= image.height <= 4096
            assert any(low != high for low, high in image.convert('RGB').getextrema()), 'Tool-window content is blank.'
        record.setdefault('visuals', []).append(dict(visual, sha256=digest(path)))


if __name__ == '__main__':
    main()
