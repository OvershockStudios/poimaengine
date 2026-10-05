#!/usr/bin/env python3
"""Actual Game Input controls and native configuration; no physical gamepad claim."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

p = argparse.ArgumentParser(description=__doc__)
for name in ('editor', 'binary', 'output'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--gpu', type=int, default=1)
a = p.parse_args()
assert os.name == 'nt', 'Run using native Windows Python.'
for name in ('editor','binary','output'):
    setattr(a, name, getattr(a, name).resolve())
run = a.output/uuid.uuid4().hex
run.mkdir(parents=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
record = dict(passed=False, test_sha256=sha(Path(__file__)), gpu=a.gpu,
              binary_sha256=sha(a.binary), bridge_sha256=sha(a.editor.parent/'poima_desktop.dll'),
              editor_sha256=sha(a.editor.parent/'Poima.Editor.dll'),
              limitations=['Actual attached accessible controls and owned-HWND synthetic keyboard/mouse routing.',
                           'No physical controller, focus acquisition, IME or OS screenshot qualification.',
                           'Device list reflects actual connected devices; this test does not create SDL virtual devices.'])

def cli(args, method=None, params=None):
    data = None if method is None else json.dumps(dict(jsonrpc='2.0',id=1,method=method,params=params or {}))+'\n'
    done = subprocess.run([str(a.binary), *map(str,args)],input=data,capture_output=True,text=True,encoding='utf-8',timeout=30)
    assert done.returncode == 0, done.stdout+done.stderr
    rows = [json.loads(line) for line in done.stdout.splitlines()]
    if method:
        assert 'result' in rows[0], rows
        return rows[0]['result']
    return rows

project = run/'Project'
cli(['project','create',project,'--name','Gamepad Controls'])
world = project/'world.json'; before = world.read_bytes(); revision = json.loads(before)['revision']
controller, camera = f'{2:032x}', f'{3:032x}'
profile_path = run/'keyboard.poima-input.json'
v1 = cli(['world',world],'input.describe')['defaults']
cli(['world',world],'input.transact',dict(path=str(profile_path),expected_revision=0,request_id=uuid.uuid4().hex,profile=v1))
profile_before = profile_path.read_bytes()
actions = []; frame = 3

def action(op, **values):
    global frame
    actions.append(dict(frame=frame,op=op,**values)); frame += 2

def choose(control,choice): action('choose_control',control='Game Input '+control,choice=choice)
def click(control): action('click_control',control='Game Input '+control)
def inspect(tag): action('inspect_game_input',tag=tag)
def rpc(method,params=None,**extra): action('rpc',method=method,params=params or {},**extra)
def capture():
    action('game_input',message='left_down'); action('game_input',message='left_up')

action('reset_layout',split_views=True)
action('open_game_input'); inspect('initial')
choose('Bindings','Keyboard, mouse and gamepad'); choose('Assignment','Only connected gamepad'); click('Apply'); inspect('staged_v2')
choose('Bindings','Profile file'); action('set_text',control='Game Input Profile path',text=str(profile_path)); click('Apply'); inspect('v1_refused')
click('Reload selection'); inspect('reloaded_v2')
click('Refresh devices'); inspect('discovery')
session = uuid.uuid4().hex
rpc('desktop.play.start',dict(revision=revision,session_id=session,paused=True))
action('game_camera',camera=camera); action('show_panel',panel='Game')
click('Apply'); inspect('configured_v2')
# A real persisted v1 must remain v1, with an explicit disabled assignment.
choose('Bindings','Profile file'); action('set_text',control='Game Input Profile path',text=str(profile_path))
choose('Assignment','Disabled'); click('Apply'); inspect('configured_v1')
# Remote policy is observed without silently replacing the tool's editable choices.
rpc('desktop.input.configure',dict(session_id=session,controller=controller,defaults='keyboard_mouse_gamepad',gamepad=dict(mode='only_connected')))
click('Reload selection'); inspect('remote_v2')
# Restore a deterministic disabled configuration before synthetic gameplay input.
choose('Assignment','Disabled'); click('Apply')
action('close_game_input'); action('runtime_pause'); capture()
action('game_input',message='key_down',key=87,scan=0x11)
action('inspect',tag='captured_before_failure')
# Native failed acquire must not unfocus or clear held input. UINT32_MAX cannot
# be a currently assigned SDL instance in this bounded test process.
rpc('desktop.input.configure',dict(session_id=session,controller=controller,defaults='keyboard_mouse_gamepad',gamepad=dict(mode='explicit',id=0xffffffff)),error_contains='Explicit session device ID is absent')
action('inspect',tag='captured_after_failure')
# Invalid profile selection uses the same public GUI action as the old shortcut.
action('game_profile',path=str(run/'missing.poima-input.json'),error_contains='existing input profile')
action('inspect',tag='profile_failure_preserved')
action('game_input',message='key_up',key=87,scan=0x11)
action('game_input',message='key_down',key=27,scan=1)
action('inspect',tag='released')
action('runtime_pause'); action('open_game_input'); click('Reload selection')
# Actual Apply failure while runtime exists leaves the good v2 configuration.
choose('Bindings','Profile file'); action('set_text',control='Game Input Profile path',text=str(profile_path))
choose('Assignment','Only connected gamepad'); click('Apply'); inspect('active_v1_refused')
click('Reload selection'); action('render_game_input',path=str(run/'game-input.png'))
action('runtime_toggle'); inspect('stopped')
script, report = run/'actions.json',run/'report.json'
script.write_text(json.dumps(dict(actions=actions),indent=2))
process = None
try:
    with (run/'stdout.txt').open('w',encoding='utf-8') as stdout,(run/'stderr.txt').open('w',encoding='utf-8') as stderr:
        process = subprocess.Popen([str(a.editor),str(world),'--gpu',str(a.gpu),'--endpoint','gamepad-ui-'+uuid.uuid4().hex,
            '--layout',str(run/'layout.json'),'--script',str(script),'--frames',str(frame+15),'--report',str(report)],stdout=stdout,stderr=stderr)
        process.wait(timeout=120)
    record['exit_code'] = process.returncode
    assert report.exists(), (run/'stderr.txt').read_text()
    result = json.loads(report.read_text()); record['desktop'] = result
    assert process.returncode == 0 and result['success'],result
    assert result['ui_backend_actual']=='Avalonia.Vulkan.VulkanPlatformGraphics'
    assert len(result['actions'])==len(actions)
    stages = {spec['tag']:row for spec,row in zip(actions,result['actions']) if 'tag' in spec}
    settings = lambda tag: stages[tag]['game_input_settings']
    native = lambda tag: settings(tag)['input']['native']
    for tag in ('staged_v2','v1_refused','reloaded_v2'):
        state = settings(tag)['input']
        assert state['configuration_pending'] and state['defaults']=='keyboard_mouse_gamepad'
        assert state['gamepad_mode']=='only_connected' and not state['native']['configured']
    assert 'v2 profile' in settings('v1_refused')['message']
    assert settings('reloaded_v2')['bindings']==1
    assert native('configured_v2')['profile']['format']=='poima.input.v2'
    assert native('configured_v2')['gamepad']['policy']=='only_connected'
    assert not native('configured_v2')['focused']
    assert native('configured_v1')['profile']['format']=='poima.input.v1'
    assert native('configured_v1')['gamepad']['policy']=='disabled'
    assert settings('configured_v1')['device_status']=='Gamepad disabled.'
    assert settings('remote_v2')['bindings']==1 and settings('remote_v2')['assignment']==1
    for tag in ('captured_before_failure','captured_after_failure','profile_failure_preserved'):
        state = stages[tag]['game_input']
        assert state['captured'] and state['native']['focused'],(tag,state)
        assert state['native']['pending']['move']==[0,1],(tag,state)
        assert state['native']['gamepad']['policy']=='disabled'
    assert not stages['released']['game_input']['captured']
    assert not stages['released']['game_input']['native']['focused']
    assert 'v2 profile' in settings('active_v1_refused')['message']
    assert native('active_v1_refused')['profile']['format']=='poima.input.v2'
    assert native('active_v1_refused')['gamepad']['policy']=='disabled'
    assert not native('stopped')['configured']
    assert world.read_bytes()==before and profile_path.read_bytes()==profile_before
    for view in ('scene','game'):
        state = result['state']['views'][view]
        assert state['attached'] and state['frames_presented']>0 and state['graphics_error'] is None and state['preparation_error'] is None
        assert state['render']['hardware'] and state['render']['success'] and state['render']['nvrhi_errors']==0
    record['checks'] = ['Explicit v2 staging and v1 refusal preserve source files','Actual Apply configures persisted v1 only with disabled gamepad',
                        'Remote configuration observed with explicit Reload','Failed device/profile selection preserves capture and held input',
                        'Escape releases native and managed capture','Both native Vulkan views render without validation errors']
    record['actions'] = len(actions); record['passed'] = True
finally:
    if process is not None and process.poll() is None:
        process.terminate(); process.wait(timeout=10)
    (run/'evidence.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(dict(passed=record['passed'],evidence=str(run/'evidence.json'))))
