#!/usr/bin/env python3
"""Guarded Audio window controls, zero-volume real output and asynchronous editor close."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

p=argparse.ArgumentParser(description=__doc__)
for name in ('editor','binary','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--gpu',type=int,default=1)
a=p.parse_args()
assert os.name=='nt','Use native Windows Python.'
for name in ('editor','binary','output'):setattr(a,name,getattr(a,name).resolve())
run=a.output/uuid.uuid4().hex;run.mkdir(parents=True)
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
record=dict(passed=False,gpu=a.gpu,test_sha256=sha(Path(__file__)),binary_sha256=sha(a.binary),
            bridge_sha256=sha(a.editor.parent/'poima_desktop.dll'),editor_sha256=sha(a.editor.parent/'Poima.Editor.dll'),
            limitations=['Actual accessible controls and SDL output-device submission at zero device volume.',
                         'No subjective listening, microphone/loopback or physical input qualification.',
                         'Attached Audio window PNG is not an OS screenshot.'])
def cli(args,method=None,params=None):
    data=None if method is None else json.dumps(dict(jsonrpc='2.0',id=1,method=method,params=params or {}))+'\n'
    done=subprocess.run([str(a.binary),*map(str,args)],input=data,capture_output=True,text=True,encoding='utf-8',timeout=30)
    assert done.returncode==0,done.stdout+done.stderr
    rows=[json.loads(line) for line in done.stdout.splitlines()]
    if method:
        assert 'result' in rows[0],rows
        return rows[0]['result']
    return rows
project=run/'Project';cli(['project','create',project,'--name','Hosted Audio'])
world=project/'world.json';revision=json.loads(world.read_text())['revision']
source=Path(__file__).resolve().parents[1]/'examples/assets/acoustic-probe.wav'
asset=cli(['world',world],'asset.audio.import',dict(source=str(source)))['asset']
emitter,camera=f'{20:032x}',f'{3:032x}'
cli(['world',world],'world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=[
    dict(op='entity.create',id=emitter,name='Audio fixture'),
    dict(op='component.set',id=emitter,type='Transform',value=dict(position=[2,2,-2],rotation=[0,0,0,1],scale=[1,1,1])),
    dict(op='component.set',id=emitter,type='AudioEmitter',value=dict(asset=asset,gain=.1,loop=True,enabled=True))]))
revision+=1;before=world.read_bytes()
actions=[];frame=3
def action(op,**values):
    global frame
    actions.append(dict(frame=frame,op=op,**values));frame+=2
def text(value):action('set_text',control='Audio Volume',text=str(value))
def check(name,value):action('check_control',control='Audio '+name,checked=value)
def click(name):action('click_control',control='Audio '+name)
def inspect(tag):action('inspect_audio',tag=tag)
def rpc(method,params=None,**extra):action('rpc',method=method,params=params or {},**extra)
def config(generation,volume):return dict(request_id=uuid.uuid4().hex,expected_generation=generation,enabled=True,muted=True,volume=volume)
def start(session):
    rpc('desktop.play.start',dict(revision=revision,session_id=session,paused=True))
    rpc('runtime.step',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,ticks=1,sounds=[dict(op='play',emitter=emitter)]))
    action('runtime_pause')

action('reset_layout',split_views=True);action('open_audio');inspect('initial')
text('NaN');click('Apply');inspect('invalid')
action('close_audio');inspect('close_blocked');action('close_guard')
click('Reload');check('Enable',True);check('Mute',True);text('.25');click('Apply');inspect('stopped_enabled')
text('.7');rpc('desktop.audio.configure',config(1,.4));inspect('remote_conflict')
click('Apply');inspect('stale_apply');click('Retry request');inspect('stale_retry')
click('Dismiss request');click('Reload');inspect('reloaded')
text('.3');action('click_control',control='Game audio mute');inspect('toolbar_dirty_guard');click('Reload')
action('click_control',control='Game audio mute');inspect('toolbar_unmuted_stopped')
action('click_control',control='Game audio mute');inspect('toolbar_muted_stopped')
text(0);check('Mute',False);click('Apply');action('render_audio',path=str(run/'audio.png'))
session=uuid.uuid4().hex;action('game_camera',camera=camera);start(session)
action('wait_audio',device_open=True,submitted_frames=512,tag='device_running')
action('click_control',control='Game audio mute');action('wait_audio',state='muted',tag='muted_running')
frame+=8;inspect('muted_later')
action('click_control',control='Game audio mute');action('wait_audio',state='active',device_open=True)
action('runtime_pause');action('wait_audio',active=False,tag='paused')
action('inspect',tag='paused_scene');action('float',panel='Scene');inspect('scene_detached')
action('reset_layout',split_views=True);inspect('scene_reattached')
action('runtime_pause');action('wait_audio',active=True,device_open=True)
action('game_camera',camera=None);action('wait_audio',active=False,tag='no_listener')
action('game_camera',camera=camera);action('wait_audio',active=True,device_open=True)
action('runtime_toggle');action('wait_audio',active=False,tag='stopped')
second=uuid.uuid4().hex;start(second);action('wait_audio',active=True,device_open=True,tag='replacement')
# This must be the last action: shutdown polling may not run more scripts,
# navigation or new playback. Both views and a real audio worker are alive.
action('close_editor')
script,report=run/'actions.json',run/'report.json';script.write_text(json.dumps(dict(actions=actions),indent=2))
process=None
try:
    with (run/'stdout.txt').open('w',encoding='utf-8') as stdout,(run/'stderr.txt').open('w',encoding='utf-8') as stderr:
        process=subprocess.Popen([str(a.editor),str(world),'--gpu',str(a.gpu),'--endpoint','audio-ui-'+uuid.uuid4().hex,
            '--layout',str(run/'layout.json'),'--script',str(script),'--frames',str(frame+250),'--report',str(report)],stdout=stdout,stderr=stderr)
        process.wait(timeout=120)
    record['exit_code']=process.returncode
    assert report.exists(),(run/'stderr.txt').read_text()
    result=json.loads(report.read_text());record['desktop']=result
    assert process.returncode==0 and result['success'],result
    assert result['ui_backend_actual']=='Avalonia.Vulkan.VulkanPlatformGraphics'
    assert len(result['actions'])==len(actions)
    rows={spec['tag']:row for spec,row in zip(actions,result['actions']) if 'tag' in spec}
    audio=lambda tag:rows[tag]['audio']
    status=lambda tag:audio(tag)['status']
    assert audio('initial')['generation']==0 and not audio('initial')['dirty']
    assert audio('invalid')['generation']==0 and audio('invalid')['pending'] is None and '0 to 1' in audio('invalid')['error']
    assert audio('close_blocked')['dirty'] and 'before closing' in audio('close_blocked')['error']
    assert audio('stopped_enabled')['generation']==1 and status('stopped_enabled')['config']==dict(enabled=True,muted=True,volume=.25)
    assert not status('stopped_enabled')['device_open']
    assert audio('remote_conflict')['conflict'] and audio('remote_conflict')['volume']=='.7'
    assert audio('stale_apply')['pending']==audio('stale_retry')['pending']
    assert '(-32009)' in audio('stale_apply')['error'] and audio('stale_apply')['pending']['expected_generation']==1
    assert audio('reloaded')['generation']==2 and not audio('reloaded')['dirty']
    assert audio('toolbar_dirty_guard')['generation']==2 and audio('toolbar_dirty_guard')['volume']=='.3'
    assert not status('toolbar_unmuted_stopped')['config']['muted'] and not status('toolbar_unmuted_stopped')['device_open']
    assert status('toolbar_muted_stopped')['config']['muted'] and not status('toolbar_muted_stopped')['device_open']
    running=status('device_running')
    assert running['device_open'] and running['submitted_frames']>=512 and running['driver'] not in (None,'dummy','disk','unknown'),running
    assert running['listener']==camera and running['session']==session and not running['config']['muted'] and running['config']['volume']==0
    assert status('muted_later')['submitted_frames']==status('muted_running')['submitted_frames']
    assert status('muted_later')['device_queued_frames']==0 and status('muted_later')['queued_frames']==0
    assert status('muted_later')['last_submitted_tick']>status('muted_running')['last_submitted_tick']
    for tag in ('paused','no_listener','stopped'):assert not status(tag)['active'],(tag,status(tag))
    assert status('scene_detached')['epoch']==status('paused')['epoch']==status('scene_reattached')['epoch']
    assert status('replacement')['session']==second and status('replacement')['epoch']>running['epoch']
    assert result['shutdown_pumps']>=1 and result['state']['closing'] and result['state']['audio']['closed']
    assert not result['state']['audio']['active'] and not result['state']['audio']['device_open']
    assert result['state']['audio']['error'] is None
    assert world.read_bytes()==before
    record['checks']=['Actual Audio controls validate without mutation','Generation conflict preserves raw draft and exact pending request',
        'Toolbar mute respects dirty draft and changes native preference','Stopped enable and stopped mute never open device',
        'Real SDL output submitted at zero volume; mute clears queues and prevents further submissions while ticks continue','Pause/camera loss/Stop suspend; Scene docking preserves audio epoch',
        'Fresh runtime replaces audio session','Asynchronous owner close pumps to terminal closed and emits final report']
    record['actions']=len(actions);record['passed']=True
finally:
    if process is not None and process.poll() is None:process.terminate();process.wait(timeout=10)
    (run/'evidence.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(dict(passed=record['passed'],evidence=str(run/'evidence.json'))))
