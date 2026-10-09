#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live native preferences around a retained, separately compiled .78 game.

No builds, downloads or imports. Copy the caller's authored world/cooked closure
and compiled game into a new owned output. The original compiled Tick births two
imported hierarchy actors. Actual Windows Vulkan readbacks and v6 saves exercise
live preference isolation and same-window restore. This does not qualify a C#
settings menu, independent FBX pixels, physical input or audible output.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import time
import traceback
import uuid

import instance_save_evolution as retained
import player_service_contract as service
from scene_capture import pixels

ROOT=Path(__file__).resolve().parents[1]
GAME='Poima.Tests.ManagedInstanceGame'
IDENTITY='poima.test.managed-instances'
TEMPLATE,LOCAL_ROOT,LOCAL_RIG=('eeeeeeeeeeeeeeeeeeeeeeeeeeee7801',
    'eeeeeeeeeeeeeeeeeeeeeeeeeeee7802','eeeeeeeeeeeeeeeeeeeeeeeeeeee7803')
LINK='eeeeeeeeeeeeeeeeeeeeeeeeeeee7810'
ZERO='0'*32
WIDTH,HEIGHT=960,540
ARGS=RECORD=DEADLINE=None
check=service.check
uid=service.uid
fresh=lambda:uuid.uuid4().hex

class Client(service.Client):
    def send(self,method,params=None):
        check(time.monotonic()<DEADLINE,'Compiled preference workload deadline expired')
        return super().send(method,params)
    def receive(self):
        remaining=DEADLINE-time.monotonic()
        check(remaining>0,'Compiled preference workload deadline expired')
        previous=ARGS.timeout;ARGS.timeout=min(previous,remaining)
        try:return super().receive()
        finally:ARGS.timeout=previous

def inventory(path):
    return retained.inventory(path)

def config_copy(config,output):
    if 'descriptor' in config:
        original=Path(config['descriptor']);descriptor=retained.read(original,1024*1024)
        check(descriptor['minimum_services_bytes']==232 and descriptor['services_version']==7 and
              {'hierarchical_instances_v1','component_collections_v1','character_input_v1'}<=set(descriptor['required_features']),
              'Supply original hierarchical Native AOT artifact')
        folder=output/'native-gameplay';folder.mkdir()
        for row in descriptor['files']:
            relative=Path(row['path']);source=(original.parent/relative).resolve(strict=True)
            check(not relative.is_absolute() and '..' not in relative.parts and source.is_relative_to(original.parent),
                  'Artifact inventory escapes original root')
            target=folder/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
            check(retained.sha(target)==row['sha256'],'Copied artifact differs')
        copied=folder/'native-gameplay.json';shutil.copyfile(original,copied)
        return {'descriptor':str(copied)}
    result={'hostfxr':config['hostfxr'],'type':GAME}
    for name in ('assembly','bridge'):
        source=Path(config[name]);target=output/('managed-'+name)
        frozen=inventory(source.parent);shutil.copytree(source.parent,target)
        check(inventory(target)==frozen,'Copied managed closure differs')
        result[name]=str(target/source.name)
    return result

def native_config(config):
    return {key:service.native(Path(value)) if key in ('assembly','bridge','hostfxr','descriptor') else value
            for key,value in config.items()}

def runtime(client,session):
    return client.call('runtime.inspect',{'session_id':session})

def module(client,session):
    return client.call('runtime.gameplay.inspect',{'session_id':session})

def components(client,session):
    return client.call('runtime.components',{'session_id':session})

def gameplay(client,session,config):
    state=runtime(client,session);before=module(client,session)
    method='runtime.gameplay.load_native' if 'descriptor' in config else 'runtime.gameplay.load'
    result=client.call(method,{'session_id':session,'request_id':fresh(),'expected_tick':state['tick'],
        'expected_revision':before['revision'],'expected_structure_revision':state['structure_revision'],**native_config(config)})
    actual=result['module']
    check(actual['type']==GAME and actual['schema']['identity']==IDENTITY and
          actual['backend']==('native_aot' if 'descriptor' in config else 'coreclr'), 'Actual original game backend/type differs')
    check(actual['assembly_sha256']==retained.sha(retained.image(config)), 'Loaded module differs from copied original compiled image')
    check(actual['values']['First']==ZERO and actual['values']['Second']==ZERO and actual['values']['Drive']==1,
          'Original Initialize does not match retained .78 consumer')
    RECORD['loaded_module']=actual

def step(client,session,count):
    state=runtime(client,session)
    return client.call('runtime.step',{'session_id':session,'request_id':fresh(),'expected_tick':state['tick'],
        'expected_structure_revision':state['structure_revision'],'ticks':count})

def actors(client,session):
    state=runtime(client,session);game=module(client,session);values=game['module']['values']
    check(values['Births']==2 and values['Mode']==0 and values['First']!=values['Second'] and
          all(values[name]!=ZERO for name in ('First','Second','FirstRig','SecondRig')),'Compiled Tick did not birth distinct actors')
    rows={}
    for name in ('First','Second'):
        root,rig=values[name],values[name+'Rig']
        mapping=client.call('runtime.instance',{'session_id':session,'tick':state['tick'],'id':root,
            'expected_structure_revision':state['structure_revision']})
        check(mapping['template_id']==TEMPLATE and mapping['nodes'][LOCAL_ROOT]==root and mapping['nodes'][LOCAL_RIG]==rig,
              'Complete instance roots/visual maps differ')
        entity=client.call('runtime.entity',{'session_id':session,'tick':state['tick'],'id':root})
        visual=client.call('runtime.entity',{'session_id':session,'tick':state['tick'],'id':rig})
        link=client.call('runtime.component.get',{'session_id':session,'tick':state['tick'],'id':root,'type':LINK})
        check(link['values'][uid(3)]==rig and link['values'][uid(4)]==[root,rig], 'Resolved scalar/array member references differ')
        check(entity['is_character'] and visual['animation']['playing'] and visual['animation']['loop'] and
              visual['animation']['speed']==(1 if name=='First' else 2),'Independent native character/rig playback missing')
        rows[name]={'instance':mapping,'actor':entity,'visual':visual,'link':link}
    return {'tick':state['tick'],'values':values,'actors':rows,'module_revision':game['revision']}

def save(client,session,configuration,root,slot):
    state=runtime(client,session);game=module(client,session);component_state=components(client,session)
    result=client.call('save.write',{'request_id':fresh(),'configuration_generation':configuration['generation'],
        'slot':slot,'expected_generation':0,'session_id':session,'expected_tick':state['tick'],
        'expected_gameplay_revision':game['revision'],'expected_component_revision':component_state['component_revision'],
        'expected_structure_revision':state['structure_revision'],'expected_ui_revision':state['ui_revision'],
        'expected_control_sequence':state['control_sequence']})
    document,path,entry=retained.stored(root,slot)
    check(document['snapshot']['version']==6 and len(document['snapshot']['payload']['structure']['spawned'])==2,
          'Actual saved snapshot is not complete two-instance v6')
    RECORD.setdefault('saves',{})[slot]={'sha256':retained.sha(path),'bytes':path.stat().st_size,
        'snapshot_sha256':document['snapshot']['sha256'],'generation':entry['generation']}
    return result,document['snapshot']

def settings(client):return client.call('player.settings.inspect')

def await_presented(client,revision,label):
    value=service.await_state(client,lambda state:state['ready'] and state['report']['live_settings']['application']['presented_revision']==revision,label)
    check(value['paused'] and value['report']['hardware'] and value['report']['nvrhi_errors']==0,'Native paused presentation unavailable')
    return value

def capture(client,name):
    state=client.call('player.inspect');path=ARGS.output/(name+'.bmp')
    result=client.call('player.capture',{'player_id':state['player_id'],'request_id':fresh(),
        'expected_control_revision':state['control_revision'],'session_id':state['session_id'],'tick':state['tick'],
        'expected_structure_revision':state['structure_revision'],'expected_ui_revision':state['ui_revision'],
        'path':service.native(path)})
    captured=result['capture'];draws=captured['render_diagnostics']['last_draws'];raw=path.read_bytes()
    check(len(raw)>=54 and raw[:2]==b'BM' and struct.unpack_from('<ii',raw,18)==(WIDTH,HEIGHT), 'Actual BMP dimensions differ')
    check(captured['capture_written'] and captured['hardware'] and captured['nvrhi_errors']==0 and
          draws['skinned_instances']==2 and draws['skinned_vertices']>0 and draws['camera_draws']>=2,
          'Both actual imported skin instances were not submitted in the camera view')
    after=client.call('player.inspect')
    check(after['tick']==state['tick'] and after['session_id']==state['session_id'],'Readback advanced native simulation')
    RECORD.setdefault('captures',{})[name]={'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),'metadata':result}
    return pixels(path)

def main():
    global ARGS,RECORD,DEADLINE
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','output'):parser.add_argument('--'+name,type=Path,required=True)
    choice=parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--artifact',type=Path,help='Original .78 Native AOT descriptor')
    choice.add_argument('--managed',type=Path,help='Original .78 CoreCLR config with assembly/bridge/hostfxr/type')
    parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--timeout',type=int,default=90);parser.add_argument('--deadline-seconds',type=float,default=900)
    ARGS=parser.parse_args()
    check(ARGS.binary.is_file() and 0<=ARGS.gpu<=4095 and 30<=ARGS.timeout<=90 and
          math.isfinite(ARGS.deadline_seconds) and 120<=ARGS.deadline_seconds<=1800,'Invalid binary/gpu/deadline')
    check(os.name=='nt' or ARGS.windows_interop,'Actual Windows Vulkan requires native Windows or --windows-interop')
    check(not sys.flags.optimize,'Do not disable independent pixel decoding assertions')
    ARGS.binary=ARGS.binary.resolve(strict=True);ARGS.world=ARGS.world.resolve(strict=True);ARGS.output=ARGS.output.resolve()
    check(not ARGS.output.exists(),'--output must be new')
    assets=Path(str(ARGS.world)+'.assets');source=retained.read(ARGS.world,16*1024*1024)
    check(set(source['entities'])=={uid(1),uid(100)} and set(source['templates'])=={TEMPLATE},
          'Supply original authored .78 two-instance fixture, not an exported observer variant')
    check(source['component_schemas'][LINK]['version']==2,'Retained world has no original component schema')
    declarations={field['id']:field for field in source['component_schemas'][LINK]['fields']}
    check(set(declarations)=={uid(n) for n in (1,2,3,4)} and declarations[uid(4)]['capacity']==2,
          'Supply original .78 cap-two component world, not evolved .79 target')
    recipe=source['templates'][TEMPLATE]
    check(recipe['root']==LOCAL_ROOT and LOCAL_RIG in recipe['entities'] and uid(100) in source['entities'],
          'Retained original template/observer absent')
    check('Camera' in source['entities'][uid(100)]['components'],'Retained observer is not a camera')
    config={'descriptor':str(ARGS.artifact.resolve(strict=True))} if ARGS.artifact else retained.configuration(ARGS.managed)
    input_compiled=retained.compiled(config)
    asset_pins=inventory(assets);world_pin=retained.sha(ARGS.world)
    protected=[ARGS.world,assets,*[Path(v) for k,v in config.items() if k in ('descriptor','assembly','bridge','hostfxr')]]
    roots=[assets]+[Path(config[k]).parent for k in ('descriptor','assembly','bridge') if k in config]
    check(all(not ARGS.output.is_relative_to(path) and not path.is_relative_to(ARGS.output) for path in roots+protected),
          'New output overlaps protected source world, assets or compiled closure')
    config_pin=retained.sha(ARGS.managed) if ARGS.managed else None
    ARGS.output.mkdir(parents=True);world=ARGS.output/'world.json';shutil.copyfile(ARGS.world,world)
    owned_assets=Path(str(world)+'.assets');shutil.copytree(assets,owned_assets)
    check(inventory(owned_assets)==asset_pins,'Copied cooked closure differs')
    copied=config_copy(config,ARGS.output);copied_pins=retained.compiled(copied)
    sources=[Path(__file__),ROOT/'tests/player_service_contract.py',ROOT/'tests/instance_save_evolution.py',
        ROOT/'tests/scene_capture.py',ROOT/'tests/managed_instance_gameplay/ManagedInstanceGame.cs']
    RECORD={'passed':False,'calls':[],'owners':[],'cleanup_errors':[],
        'backend':'native_aot' if ARGS.artifact else 'coreclr','binary_sha256':retained.sha(ARGS.binary),
        'source_world_sha256':world_pin,'source_assets':asset_pins,'input_compiled':input_compiled,'copied_compiled':copied_pins,
        'source_pins':{p.relative_to(ROOT).as_posix():retained.sha(p) for p in sources},
        'limitations':['No builds, source model imports or content downloads.',
            'Original separately compiled .78 Tick births actors; preferences are external native service commands, not a C# menu.',
            'Draw statistics and same-owner readbacks are not independent original FBX or visibility/pose pixel oracles.',
            'Exact native save payload, live maps and compiled continuation checks; no migration/export/performance/physical-input/audibility claim.',
            'Caller originals stay read-only; copied CoreCLR hostfxr remains a trusted external runtime dependency.']}
    service.ARGS,service.RECORD=ARGS,RECORD
    DEADLINE=time.monotonic()+ARGS.deadline_seconds
    endpoint='compiled-preferences-'+fresh();host=client=None
    try:
        host=service.ProcessOwner([str(ARGS.binary),'serve',service.native(world),'--endpoint',endpoint],
            'compiled-preferences-host',endpoint,'serve')
        client=Client(endpoint,'compiled-preferences-client')
        discovery=client.call('world.describe');check(discovery['session_scope']=='shared_headless','Owner is not pumped')
        # This sole authored change affects only the owned observer; originals,
        # imported templates, bind data and cooked bytes remain untouched.
        client.call('world.transact',{'base_revision':source['revision'],'request_id':fresh(),'ops':[
            {'op':'component.set','id':uid(100),'type':'Transform','value':{
                'position':[0,2,14],'rotation':[0,0,0,1],'scale':[1,1,1]}}]})
        authored=world.read_bytes();revision=client.call('world.inspect')['revision']
        session=fresh();client.call('runtime.start',{'session_id':session,'revision':revision})
        gameplay(client,session,copied);before=module(client,session);state=runtime(client,session)
        client.call('runtime.gameplay.edit',{'session_id':session,'request_id':fresh(),'expected_tick':state['tick'],
            'expected_revision':before['revision'],'expected_structure_revision':state['structure_revision'],'values':{'Mode':1}})
        step(client,session,1);born=actors(client,session);RECORD['compiled_birth']=born
        saves=ARGS.output/'saves';saves.mkdir();configured=client.call('save.configure',{
            'request_id':fresh(),'expected_generation':0,'root':service.native(saves)})
        written,baseline=save(client,session,configured,saves,'before-preferences')
        parameters=service.start_parameters(session,camera=uid(100),width=WIDTH,height=HEIGHT,gpu=ARGS.gpu,
            expected_tick=born['tick'],
            expected_structure_revision=runtime(client,session)['structure_revision'],
            settings_overrides={'ui.scale':1})
        parameters.pop('controller') # Camera-only spectator never adds a synthetic player/controller.
        ack=client.call('player.start',parameters);initial=await_presented(client,0,'compiled-initial-presented')
        image=capture(client,'compiled-initial');current=settings(client)
        check(current['settings']['values']=={'ui.scale':1,'graphics.samples':1,'graphics.frames_in_flight':1},
              'Initial explicit graphics/UI launch intent differs')
        desired={'camera.vertical_fov':90,'ui.scale':1.5,'input.sensitivity_x':.3,
                 'input.sensitivity_y':.4,'input.invert_x':True,'audio.master_gain':.25}
        operation={'player_id':current['player_id'],'request_id':fresh(),
            'expected_control_revision':current['control_revision'],'expected_settings_revision':0,'set':desired}
        expected_preferences={**current['settings']['values'],**desired}
        receipt=client.call('player.settings.transact',operation);applied=await_presented(client,1,'compiled-live-presented')
        check(applied['player_id']==ack['player_id'] and settings(client)['settings']['revision']==1,
              'Live preferences replaced the native window')
        live=capture(client,'compiled-live');check(live!=image,'Live FOV/UI preference produced no readback change')
        _,isolated=save(client,session,configured,saves,'after-preferences')
        check(isolated==baseline and actors(client,session)==born,'Preferences altered saved native/compiled state')
        step(client,session,17);continued=actors(client,session)
        check(continued['values']['Ticks']==born['values']['Ticks']+17 and
              continued['actors']['First']['actor']['local_transform']['position']!=born['actors']['First']['actor']['local_transform']['position'],
              'Actual compiled/native character continuation did not move')
        _,advance_snapshot=save(client,session,configured,saves,'continued')
        check(advance_snapshot!=baseline,'Actual continuation did not change native checkpoint')
        state=runtime(client,session);game=module(client,session);component_state=components(client,session);replacement=fresh()
        loaded=client.call('save.load',{'request_id':fresh(),'configuration_generation':configured['generation'],
            'slot':'before-preferences','expected_generation':written['generation'],'revision':revision,
            'expected_session_id':session,'expected_tick':state['tick'],'expected_gameplay_revision':game['revision'],
            'expected_component_revision':component_state['component_revision'],'expected_structure_revision':state['structure_revision'],
            'expected_ui_revision':state['ui_revision'],'expected_control_sequence':state['control_sequence'],
            'new_session_id':replacement,'gameplay':native_config(copied)})
        restored=await_presented(client,1,'compiled-restored-presented');session=replacement
        check(restored['player_id']==ack['player_id'] and restored['session_id']==replacement and
              settings(client)['settings']['values']==expected_preferences,'Same-window replacement reset current preferences')
        _,restored_snapshot=save(client,session,configured,saves,'restored')
        check(restored_snapshot==baseline,'Exact v6 replacement failed native/compiled state preservation')
        check(capture(client,'compiled-restored')==live,'Same-tick restored readback changed current preferences/actors')
        check(client.call('player.settings.transact',operation)==dict(receipt,replayed=True),
              'Historical preference receipt changed after Runtime replacement')
        check(settings(client)['settings']['revision']==1,'Historical receipt reapplied state')
        step(client,session,1);after=actors(client,session)
        check(after['values']['Ticks']==born['values']['Ticks']+1 and after['values']['First']==born['values']['First'] and
              after['values']['FirstRig']==born['values']['FirstRig'],'Compiled continuation lost handles after replacement')
        RECORD['restored_module']=module(client,session);RECORD['restore_result']=loaded
        state=client.call('player.inspect');client.call('player.control',service.control_parameters(state,'stop'))
        terminal=service.await_state(client,lambda value:not value['active'],'compiled-stopped')
        check(terminal['report']['success'],'Compiled native window terminal failed')
        client.call('runtime.stop',{'session_id':session});client.call('host.shutdown');client.close();client=None
        host.process.wait(timeout=20);host.close();host=None
        check(world.read_bytes()==authored and inventory(owned_assets)==asset_pins,'Live owner changed authored/cooked content')
        check(retained.sha(ARGS.world)==world_pin and inventory(assets)==asset_pins and
              all(retained.sha(path)==pin for path,pin in input_compiled.items()),'Caller input changed')
        check(not ARGS.managed or retained.sha(ARGS.managed)==config_pin,'Caller managed config changed')
        check(retained.compiled(copied)==copied_pins,'Copied compiled artifacts changed')
        check(retained.sha(ARGS.binary)==RECORD['binary_sha256'] and all(retained.sha(ROOT/path)==pin for path,pin in RECORD['source_pins'].items()),
              'Verifier/engine source changed during qualification')
        for owner in RECORD['owners']:
            expected='Poima shared world ready: '+endpoint+'\n' if owner['label']=='compiled-preferences-host' else ''
            check(owner['exit_code']==0 and not owner['stderr_truncated'] and owner['stderr']==expected,'Owned process did not exit cleanly')
        RECORD['checks']={key:True for key in ('actual_compiled_births','complete_maps_and_native_character_motion',
            'camera_skin_draw_submission','live_settings_native_state_isolation','actual_compiled_17_tick_continuation',
            'same_window_v6_restore_preserves_preferences','exact_restored_payload','same_tick_restored_pixels',
            'historical_retry_not_reapplied','post_restore_compiled_tick','read_only_caller_inputs')}
        RECORD['rpc_count']=len(RECORD['calls']);RECORD['passed']=True
    except BaseException as error:
        RECORD['failure']=repr(error);RECORD['traceback']=traceback.format_exc();raise
    finally:
        active=sys.exc_info()[0] is not None
        for owner in (client,host):
            if owner is not None:
                try:owner.close(expected=False)
                except BaseException as error:RECORD['cleanup_errors'].append(repr(error))
        if RECORD['cleanup_errors']:RECORD['passed']=False
        (ARGS.output/'evidence.json').write_text(json.dumps(RECORD,indent=2)+'\n',encoding='utf-8')
        if RECORD['cleanup_errors'] and not active:raise AssertionError(RECORD['cleanup_errors'])
    print(json.dumps({'passed':True,'backend':RECORD['backend'],'rpc_count':RECORD['rpc_count']}))

if __name__=='__main__':main()
