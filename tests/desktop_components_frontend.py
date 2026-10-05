#!/usr/bin/env python3
"""Actual Inspector controls over generated schemas, C# ticks and guarded native saves."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

p = argparse.ArgumentParser(description=__doc__)
for name in ('editor', 'binary', 'manifest', 'assembly', 'output'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--gameplay-bridge', type=Path)
p.add_argument('--gpu', type=int, default=1)
a = p.parse_args()
assert os.name == 'nt', 'Run using native Windows Python.'
for key in ('editor','binary','manifest','assembly','output'):
    setattr(a, key, getattr(a, key).resolve())
run = a.output/uuid.uuid4().hex
run.mkdir(parents=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
record = dict(passed=False, test_sha256=sha(Path(__file__)), gpu=a.gpu,
              binary_sha256=sha(a.binary), bridge_sha256=sha(a.editor.parent/'poima_desktop.dll'),
              editor_sha256=sha(a.editor.parent/'Poima.Editor.dll'), manifest_sha256=sha(a.manifest), assembly_sha256=sha(a.assembly),
              limitations=['Actual attached accessible controls; no physical input, IME or screen capture claim.',
                           'Inspector PNG is Avalonia client visual, not an OS screenshot or native viewport image.'])
manifest = json.loads(a.manifest.read_text())
health = next(s for s in manifest['schemas'] if s['name'] == 'Health')
interaction = next(s for s in manifest['schemas'] if s['name'] == 'Interaction')
h, it = health['id'], interaction['id']
hf = {f['name']: f['id'] for f in health['fields']}
inf = {f['name']: f['id'] for f in interaction['fields']}
uid = lambda n: f'{n:032x}'
entity_a, entity_b, target = map(uid, (501,502,503))

def cli(arguments, request=None):
    data = None if request is None else json.dumps(request)+'\n'
    done = subprocess.run([str(a.binary), *map(str, arguments)], input=data, capture_output=True, text=True, encoding='utf-8', timeout=30)
    assert done.returncode == 0, done.stdout+done.stderr
    rows = [json.loads(line) for line in done.stdout.splitlines()]
    return rows

def request(method, params=None): return dict(jsonrpc='2.0', id=1, method=method, params=params or {})
project = run/'Project'
cli(['project','create',project,'--name','Component Inspector'])
world = project/'world.json'
revision = json.loads(world.read_text())['revision']
ops = []
for entity, name in ((entity_a,'Health A'),(entity_b,'Health B'),(target,'Target')):
    ops += [dict(op='entity.create',id=entity,name=name),dict(op='component.set',id=entity,type='Transform',value=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1]))]
reply = cli(['world',world],request('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops)))[0]
assert 'result' in reply, reply
revision += 1
save_root = run/'Saves'; save_root.mkdir()
duplicate = run/'duplicate.poima-components.json'
duplicate.write_text('{"format":"poima.components","format":"poima.components","version":1,"schemas":[]}')
actions = []; frame = 3

def action(op, **values):
    global frame
    actions.append(dict(frame=frame,op=op,**values)); frame += 2

def inspect(tag): action('inspect',tag=tag)
def text(control,value): action('set_text',control=control,text=str(value))
def click(control): action('click_control',control=control)
def choose(control,choice): action('choose_control',control=control,choice=choice)
def rpc(method,params=None,**extra): action('rpc',method=method,params=params or {},**extra)
def field(component,name,value): text('Component '+component+' '+name,value)
def apply(): click('Inspector Apply')
def add(name): choose('Components Available schemas',name); click('Components Add component')
def read_live(tag): inspect(tag)

action('select',id=entity_a)
action('expand_control',control='Components Tools',expanded=True)
text('Components Manifest path',duplicate); click('Components Import schemas'); inspect('duplicate_refused')
text('Components Manifest path',a.manifest); click('Components Import schemas'); revision += 1; inspect('imported')
add('Health'); field('Health','Current','NaN'); action('apply',error_contains='invalid Inspector'); inspect('invalid_float')
action('select',id=entity_b,error_contains='Apply or reload')
action('reload'); inspect('invalid_reverted')
add('Health'); field('Health','Current','88.5'); field('Health','Maximum','150'); field('Health','Score',2**63-1)
apply(); revision += 1; inspect('typed_authored')
click('Component Health Remove'); inspect('remove_draft'); apply(); revision += 1; inspect('removed')
action('undo'); revision += 1; inspect('undo_remove')
action('redo'); revision += 1; inspect('redo_remove')
action('undo'); revision += 1
field('Health','Score',-2); apply(); revision += 1
add('Interaction'); choose('Component Interaction Target Entity','Target · 00000000'); field('Interaction','Weight','1.25'); apply(); revision += 1; inspect('reference_authored')
action('select',id=entity_b); add('Health'); field('Health','Current',77); field('Health','Score',-4); apply(); revision += 1
inspect('second_entity')
action('select',id=entity_a); field('Health','Current',66)
renamed = copy.deepcopy(manifest)
next(s for s in renamed['schemas'] if s['id']==h)['name'] = 'Vital Health'
rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=revision,manifest=renamed)); revision += 1
inspect('schema_conflict'); action('assert_text',control='Component Health Current',text='66')
action('apply',error_contains='(-32009)'); action('reload'); inspect('schema_reloaded')
field('Interaction','Target',uid(99999)); action('apply',error_contains='(-32602)'); inspect('unknown_reference'); action('reload')
action('scroll_inspector',position='bottom'); action('render_inspector',path=str(run/'authored-inspector.png'))
# Configure the real generated fixture through the existing human launch controls.
action('show_gameplay')
text('Gameplay Assembly',a.assembly); text('Gameplay Game type','Poima.Tests.ComponentGame')
if a.gameplay_bridge: text('Gameplay Managed bridge',a.gameplay_bridge.resolve())
text('Gameplay Initial values JSON',json.dumps(dict(Mode=1,Selected=entity_a)))
click('Gameplay Configure launch')
session = uuid.uuid4().hex
rpc('desktop.play.start',dict(revision=revision,session_id=session,paused=True,expected_gameplay_generation=1))
click('Component Vital Health Load live'); read_live('initial_live')
text('Live component Current',50); text('Live component Score',-2)
action('step',error_contains='live component'); action('select',id=entity_b,error_contains='live component'); action('close_guard')
click('Live component Apply'); read_live('live_edited')
click('Step simulation'); click('Live component Reload'); read_live('after_tick')
rpc('runtime.gameplay.inspect',dict(session_id=session,tick=1,include_schema=True),tag='csharp')
rpc('runtime.component.get',dict(session_id=session,tick=1,id=entity_b,type=h),tag='other_unchanged')
text('Live component Current',40)
external = {hf['Current']:46,hf['Maximum']:150,hf['Score']:'-1'}
rpc('runtime.component.edit',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=1,expected_revision=2,id=entity_a,type=h,values=external))
read_live('external_conflict'); click('Live component Apply'); read_live('failed_apply'); action('assert_text',control='Live component Current',text='40')
click('Live component Reload'); read_live('live_reloaded')
# Save UI must observe component revisions independently from unchanged tick/global-state revision.
action('open_saves'); text('Save Root',save_root); click('Save Configure root'); click('Save Inspect slot')
text('Live component Current',45); click('Save Write checkpoint'); inspect('dirty_save_refused')
click('Live component Apply'); click('Save Write checkpoint'); inspect('stale_save_refused')
click('Save Dismiss pending'); click('Save Inspect slot'); click('Save Write checkpoint'); inspect('saved')
action('scroll_inspector',position='top'); action('render_inspector',path=str(run/'live-inspector.png'))
# Current authored membership changes; restored frozen runtime must still expose its own schema and values.
action('runtime_toggle'); click('Live component Discard'); click('Component Vital Health Remove'); apply(); revision += 1
inspect('authored_removed_after_save'); click('Save Inspect slot'); click('Save Load checkpoint'); inspect('restored')
click('Live component Refresh types'); choose('Live component Types','Vital Health'); click('Live component Load selected'); inspect('restored_frozen_component')
# Explicit batches retain all-or-nothing semantics; automatic Play retains only its committed prefix.
click('Step simulation'); click('Live component Reload'); inspect('prefix_baseline')
click('Gameplay Revert / refresh values'); text('Gameplay Value Mode',4); click('Gameplay Apply live values')
action('step',ticks=2,error_contains='(-32040)'); click('Live component Reload'); inspect('explicit_component_rollback')
action('runtime_pause'); action('wait_playback',state='paused',tick=3)
click('Live component Reload'); inspect('automatic_component_prefix')
click('Live component Discard'); action('runtime_toggle'); inspect('final')
assert len(actions) <= 256
script, report = run/'actions.json', run/'report.json'
script.write_text(json.dumps(dict(actions=actions),indent=2))
process = None
try:
    with (run/'stdout.txt').open('w',encoding='utf-8') as stdout, (run/'stderr.txt').open('w',encoding='utf-8') as stderr:
        process = subprocess.Popen([str(a.editor),str(world),'--gpu',str(a.gpu),'--endpoint','components-'+uuid.uuid4().hex,
            '--layout',str(run/'layout.json'),'--script',str(script),'--frames',str(frame+20),'--report',str(report)],stdout=stdout,stderr=stderr)
        process.wait(timeout=120)
    record['exit_code'] = process.returncode
    assert report.exists(),(run/'stderr.txt').read_text()
    result = json.loads(report.read_text()); record['desktop'] = result
    assert process.returncode == 0 and result['success'],result
    assert result['ui_backend_actual'] == 'Avalonia.Vulkan.VulkanPlatformGraphics'
    scene = result['state']['views']['scene']
    assert scene['attached'] and scene['frames_presented'] > 0
    assert scene['graphics_error'] is None and scene['preparation_error'] is None
    assert scene['render']['hardware'] and scene['render']['success'] and scene['render']['nvrhi_errors']==0
    assert len(result['actions']) == len(actions)
    stages = {spec['tag']:row for spec,row in zip(actions,result['actions']) if 'tag' in spec}
    d = lambda tag: stages[tag]['draft']
    live = lambda tag: stages[tag]['component_draft']
    hv = lambda tag: d(tag)['components']['game:'+h]
    assert d('duplicate_refused')['revision']+1 == d('imported')['revision']
    assert d('invalid_float')['dirty'] and d('invalid_float')['invalid_fields']
    assert 'game:'+h not in d('invalid_reverted')['components'] and not d('invalid_reverted')['dirty']
    assert hv('typed_authored') == {hf['Current']:88.5,hf['Maximum']:150,hf['Score']:str(2**63-1)}
    assert d('remove_draft')['dirty'] and 'game:'+h not in d('remove_draft')['components']
    assert hv('undo_remove') == hv('typed_authored')
    for tag in ('removed','redo_remove'): assert 'game:'+h not in d(tag)['components']
    assert d('reference_authored')['components']['game:'+it] == {inf['Target']:target,inf['Weight']:1.25}
    assert d('schema_conflict')['dirty'] and d('schema_conflict')['conflict'] and hv('schema_conflict')[hf['Current']]==66
    assert hv('schema_reloaded')[hf['Current']]==88.5 and not d('schema_reloaded')['dirty']
    assert d('unknown_reference')['dirty']
    assert live('initial_live')['observation']['values'] == hv('schema_reloaded')
    assert live('live_edited')['observation']['component_revision']==1
    assert live('after_tick')['observation']['values'] == {hf['Current']:49,hf['Maximum']:150,hf['Score']:'-1'}
    assert live('after_tick')['observation']['component_revision']==2
    module = stages['csharp']['result']['module']['values']
    assert module['LastHealth']==50 and module['LastScore']=='-2' and module['ReadBeforeWrite']==1 and module['Queried']==2,module
    assert stages['other_unchanged']['result']['values'] == hv('second_entity')
    assert live('external_conflict')['dirty'] and live('external_conflict')['conflict']
    assert '(-32009)' in live('failed_apply')['error'] and live('failed_apply')['values'][hf['Current']]=='40'
    assert live('live_reloaded')['observation']['values']==external and not live('live_reloaded')['dirty']
    assert stages['dirty_save_refused']['save_draft']['pending'] is None and 'live component' in stages['dirty_save_refused']['save_draft']['error']
    pending = stages['stale_save_refused']['save_draft']['pending']
    assert pending['params']['expected_component_revision']==3
    assert '(-32009)' in stages['stale_save_refused']['save_draft']['error']
    assert stages['saved']['save_draft']['last_result']['generation']==1
    assert stages['saved']['save_draft']['runtime_observation']['component_revision']==4
    assert 'game:'+h not in d('authored_removed_after_save')['components']
    restored = live('restored_frozen_component')['observation']
    assert restored['session_id'] != session and restored['tick']==1 and restored['component_revision']==4
    assert restored['values'] == {hf['Current']:45,hf['Maximum']:150,hf['Score']:'-1'}
    assert 'game:'+h not in d('restored_frozen_component')['components']
    baseline = live('prefix_baseline')['observation']
    assert baseline['tick']==2 and baseline['component_revision']==5
    assert baseline['values']=={hf['Current']:44,hf['Maximum']:150,hf['Score']:'0'}
    assert live('explicit_component_rollback')['observation']==baseline
    prefix = live('automatic_component_prefix')['observation']
    assert prefix['tick']==3 and prefix['component_revision']==6
    assert prefix['values']=={hf['Current']:43,hf['Maximum']:150,hf['Score']:'1'}
    assert stages['automatic_component_prefix']['playback']['state']=='paused'
    assert 'Later component fixture rollback' in stages['automatic_component_prefix']['playback']['last_error']
    assert d('automatic_component_prefix')['revision']==revision
    assert d('final')['revision']==revision and not d('final')['dirty']
    record['actions'] = len(actions)
    record['images'] = {name:sha(run/name) for name in ('authored-inspector.png','live-inspector.png')}
    record['passed'] = True
except Exception as error:
    record['error'] = repr(error)
    raise
finally:
    if process is not None and process.poll() is None: process.terminate(); process.wait(timeout=10)
    (run/'evidence.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(run/'evidence.json')
