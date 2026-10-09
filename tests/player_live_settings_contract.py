#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Guarded live player preferences, with optional real Vulkan presentation.

Network-free, builds nothing, and starts only owned processes. The default
provider/device-free path checks discovery and absent-player admission. Supply
--capture and a new --output to test native FOV projection, absolute UI scale,
profile isolation, same-window save replacement and semantic replay. This is a
native service test, not a compiled C# menu or physical-input/audio audition.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time
import traceback
import uuid

import player_service_contract as service
from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 960, 540
ARGS = None
RECORD = None
DEADLINE = None
uid, check = service.uid, service.check
LIVE = ('camera.vertical_fov', 'input.sensitivity_x', 'input.sensitivity_y',
        'input.invert_x', 'input.invert_y', 'ui.scale', 'audio.master_gain')
GRAPHICS = ('graphics.samples', 'graphics.frames_in_flight')


class Client(service.Client):
    """Retain the shared helper's protocol while bounding the entire workload."""
    def send(self, method, params=None):
        check(time.monotonic() < DEADLINE, 'Overall live-settings deadline expired')
        return super().send(method, params)

    def receive(self):
        remaining = DEADLINE-time.monotonic()
        check(remaining > 0, 'Overall live-settings deadline expired')
        previous = ARGS.timeout
        ARGS.timeout = min(previous, remaining)
        try:
            return super().receive()
        finally:
            ARGS.timeout = previous


def request_id():
    return uuid.uuid4().hex


def prefs(client):
    result = client.call('player.settings.inspect')
    settings = result['settings']
    check(settings['format'] == 'poima.player-preferences.v1', result)
    check(type(settings['revision']) is int and settings['revision'] >= 0, result)
    check(set(settings['fields']) == set(LIVE+GRAPHICS), result)
    for name in LIVE:
        check(settings['fields'][name]['application'] == 'live' and
              not settings['fields'][name]['requires_next_player'], result)
    for name in GRAPHICS:
        check(settings['fields'][name]['application'] == 'next_player', result)
    return result


def patch(observed, **changes):
    return {'player_id': observed['player_id'], 'request_id': request_id(),
            'expected_control_revision': observed['control_revision'],
            'expected_settings_revision': observed['settings']['revision'], **changes}


def presented(client, revision, label):
    observed = service.await_state(client, lambda state:
        state['ready'] and state['report'].get('live_settings', {}).get('application', {}).get(
            'presented_revision') == revision, label)
    current = prefs(client)
    application = current['settings']['application']
    check(application['observed_revision'] == revision and
          application['applied_revision'] == revision and
          application['presented_revision'] == revision, current)
    check(observed['report']['hardware'] and observed['report']['nvrhi_errors'] == 0, observed)
    return current


def commit(client, parameters, label):
    result = client.call('player.settings.transact', parameters)
    check(not result['preview'] and not result['replayed'] and
          result['previous_settings_revision'] == parameters['expected_settings_revision'] and
          result['settings']['revision'] == parameters['expected_settings_revision']+1 and
          result['control_revision'] == parameters['expected_control_revision']+1, result)
    return result, presented(client, result['settings']['revision'], label)


def capture(client, name, observed=None):
    observed = client.call('player.inspect') if observed is None else observed
    path = ARGS.output/(name+'.bmp')
    result = client.call('player.capture', {
        'player_id': observed['player_id'], 'request_id': request_id(),
        'expected_control_revision': observed['control_revision'],
        'session_id': observed['session_id'], 'tick': observed['tick'],
        'expected_structure_revision': observed['structure_revision'],
        'expected_ui_revision': observed['ui_revision'], 'path': service.native(path)})
    value = result['capture']
    raw = path.read_bytes()
    check(len(raw) >= 54 and raw[:2] == b'BM', 'Missing actual native BMP')
    width, height = struct.unpack_from('<ii', raw, 18)
    check((width, abs(height)) == (WIDTH, HEIGHT) and value['capture_written'] and
          value['hardware'] and value['samples'] == 1 and value['nvrhi_errors'] == 0, result)
    after = client.call('player.inspect')
    check(after['tick'] == observed['tick'] and after['session_id'] == observed['session_id'],
          'Capture advanced or replaced simulation')
    record = {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw),
              'metadata': result, 'width': width, 'height': abs(height)}
    RECORD.setdefault('captures', {})[name] = record
    return pixels(path)


def image_oracle(image, fov, scale, label):
    # Unit box front face is 4.5m from the fixed camera. Perspective silhouette
    # height is independent of the engine's camera matrix implementation.
    expected_height = HEIGHT*.5/(4.5*math.tan(math.radians(fov)/2))
    points = [(x,y) for y,row in enumerate(image) for x in range(WIDTH//2-24, WIDTH//2+24)
              if row[x][0] > 70 and row[x][0] > 2*row[x][1] and row[x][0] > 2*row[x][2]]
    check(points, 'No visible projection subject')
    actual_height = max(y for x,y in points)-min(y for x,y in points)+1
    check(abs(actual_height-expected_height) <= 3, (label, actual_height, expected_height))
    button = [(x,y) for y,row in enumerate(image) for x,value in enumerate(row)
              if all(abs(a-b)<=1 for a,b in zip(value, (34,68,102)))]
    check(button, 'No visible opaque native UI rectangle')
    bounds = [min(x for x,y in button), min(y for x,y in button),
              max(x for x,y in button)+1, max(y for x,y in button)+1]
    expected_bounds = [24*scale,52*scale,168*scale,88*scale]
    check(all(abs(a-b)<=1 for a,b in zip(bounds, expected_bounds)),
          (label, bounds, expected_bounds))
    RECORD.setdefault('image_oracles', {})[label] = {
        'vertical_fov': fov, 'ui_scale': scale,
        'expected_box_height_pixels': expected_height, 'observed_box_height_pixels': actual_height,
        'expected_button_bounds_pixels': expected_bounds, 'observed_button_bounds_pixels': bounds,
        'projection_error_bound_pixels': 3, 'ui_error_bound_pixels': 1}


def author(client):
    def entity(n, position, parent=None, **components):
        ops = [{'op': 'entity.create', 'id': uid(n), 'name': str(n), 'parent': parent},
               {'op': 'component.set', 'id': uid(n), 'type': 'Transform', 'value': {
                   'position': position, 'rotation': [0,0,0,1], 'scale': [1,1,1]}}]
        return ops+[{'op': 'component.set', 'id': uid(n), 'type': kind, 'value': value}
                    for kind,value in components.items()]
    ops = entity(10, [0,0,0], Camera={'vertical_fov':60,'near':.1,'far':100})
    ops += entity(20, [0,0,-5], MeshRenderer={'primitive':'box','visible':True,'albedo':[.9,.01,.01]})
    ops += entity(100, [100,10,0], CharacterController={'radius':.3,'height':1.8,'speed':4,
                    'jump_speed':5,'camera':uid(101)})
    ops += entity(101, [0,1.6,0], uid(100), Camera={'vertical_fov':70,'near':.1,'far':100})
    def dp(value):
        return {'unit':'dp','value':value}
    def ui(n,parent,kind,text='',action=None,**properties):
        return {'op':'ui.element.set','id':uid(n),'element':{
            'parent':parent,'name':f'Live settings proof {n}','kind':kind,'text':text,
            'action':action,'visible':True,'enabled':True,**properties}}
    ops += [ui(500,None,'panel',layout={'position':'absolute','left':dp(16),'top':dp(16),
            'width':dp(160),'height':dp(88),'direction':'column','align':'stretch','justify':'start',
            'padding':[8,8,8,8],'gap':8,'hit_test':'pass_through'},
            style={'background_color':'#14202a','border_width':0,'border_radius':0}),
        ui(501,uid(500),'label','Live settings',layout={'width':dp(144),'height':dp(20)},
           style={'font_size':14,'color':'#f7e8eb'}),
        ui(502,uid(500),'button','Settings','settings-proof',
           layout={'width':dp(144),'height':dp(36)},
           style={'background_color':'#224466','color':'#f7e8eb','font_size':14,
                  'border_width':0,'border_radius':0,
                  'hover':{'background_color':'#224466'},'focus':{'background_color':'#224466'},
                  'pressed':{'background_color':'#224466'}})]
    client.call('world.transact', {'base_revision':0,'request_id':request_id(),'ops':ops})
    return ops


def controls(client, action):
    current = client.call('player.inspect')
    return client.call('player.control', service.control_parameters(current, action))


def stop(client, label):
    controls(client, 'stop')
    return service.await_state(client, lambda state:not state['active'], label)


def stored_payload(root, slot):
    # The native store checks its own exact raw-byte checksum. Parsed-value
    # equality below deliberately avoids Python float reserialization hashes.
    folder = root/('slot-'+slot)
    manifest = folder/'current.json'
    check(manifest.is_file() and manifest.stat().st_size <= 1024*1024, manifest)
    current = json.loads(manifest.read_text(encoding='utf-8'))['payload']['current']
    name = current['file']
    check(type(name) is str and len(name)==38 and name.startswith('p-') and
          name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]), name)
    path = folder/name
    check(path.is_file() and path.stat().st_size <= 64*1024*1024, path)
    raw = path.read_bytes()
    check(len(raw)==current['bytes'] and hashlib.sha256(raw).hexdigest()==current['sha256'],
          'Native stored bytes differ from the manifest identity')
    document = json.loads(raw)
    return document['snapshot']['payload']


def write_slot(client, configured, session, tick, slot):
    boundary=client.call('player.inspect')
    check(boundary['session_id']==session and boundary['tick']==tick and boundary['paused'],
          'Save must guard the actual paused UI boundary')
    return client.call('save.write', {'request_id':request_id(),
        'configuration_generation':configured['generation'],'slot':slot,'expected_generation':0,
        'session_id':session,'expected_tick':tick,'expected_gameplay_revision':0,
        'expected_ui_revision':boundary['ui_revision'],
        'expected_control_sequence':boundary['control_sequence']})


def admission_checks(client, discovery, world):
    for name in ('player.settings.inspect','player.settings.transact'):
        check(name in discovery['methods'], 'Missing public method '+name)
    initial = client.call('player.inspect')
    client.reject('player.settings.inspect', {}, -32004)
    client.reject('player.settings.transact', {
        'player_id':uid(900),'request_id':request_id(),
        'expected_control_revision':0,'expected_settings_revision':0,'set':{'ui.scale':1}}, -32004)
    check(client.call('player.inspect') == initial, 'Absent preferences operation published player state')
    check(not world.exists(), 'Absent operation created authored world')
    if not initial['available']:
        parameters = service.start_parameters(request_id())
        client.reject('player.start', parameters, -32003)
        check(client.call('player.inspect') == initial, 'Disabled start changed player generation')
    RECORD['admission'] = {'methods_advertised':True,'absent_rejected_without_publication':True,
                            'driver_available':initial['available'],'graphics_requested':ARGS.capture}


def graphics_checks(client, second, world):
    check(client.call('player.inspect')['available'], 'Frame-driven rendering unavailable')
    author(client)
    authored = world.read_bytes()
    saves = ARGS.output/'saves'; saves.mkdir()
    configured = client.call('save.configure', {'request_id':request_id(),'expected_generation':0,
                                              'root':service.native(saves)})
    profile = ARGS.output/'preferences.poima-settings.json'
    # A sparse empty profile leaves the authored camera/density/profile defaults
    # as genuine inherited sources, rather than resetting to synthetic constants.
    created = client.call('settings.transact', {'path':service.native(profile),'request_id':request_id(),
                              'expected_revision':0,'set':{}})
    profile_before = profile.read_bytes()
    session = request_id()
    client.call('runtime.start', {'session_id':session,'revision':1})
    parameters = service.start_parameters(session, camera=uid(10), gpu=ARGS.gpu,
        width=WIDTH,height=HEIGHT,audio=ARGS.audio,settings_profile=service.native(profile),
        settings_revision=created['revision'])
    ack = client.call('player.start', parameters)
    check(ack['active'] and not ack['ready'], ack)
    base = presented(second, 0, 'initial-presented')
    initial_values={'graphics.samples':1,'graphics.frames_in_flight':1}
    check(base['settings']['revision'] == 0 and base['settings']['values'] == initial_values, base)
    for name in GRAPHICS:
        check(base['settings']['fields'][name]['source']=='explicit_option' and
              base['settings']['fields'][name]['requested']==1, base)
    baseline_state = client.call('runtime.entity', {'session_id':session,'id':uid(100)})
    baseline_camera = client.call('runtime.entity', {'session_id':session,'id':uid(10)})
    baseline_image = capture(client, 'initial')
    density = base['settings']['application']['effective_ui_scale']
    check(density > 0, base)
    image_oracle(baseline_image, 60, density, 'initial')
    before_write = write_slot(client, configured, session, 0, 'before')
    before_payload = stored_payload(saves, 'before')

    values = {'camera.vertical_fov':90,'ui.scale':1.5,'input.sensitivity_x':.3,
              'input.sensitivity_y':.4,'input.invert_x':True,'input.invert_y':True,
              'audio.master_gain':.4,'graphics.samples':4,'graphics.frames_in_flight':2}
    client.reject('player.settings.inspect', {'player_id':uid(900)}, -32004)
    client.reject('player.settings.transact', {**patch(base,set={'ui.scale':2}),
                   'player_id':uid(900)}, -32004)
    proposed = patch(base, set=values, preview=True)
    preview = client.call('player.settings.transact', proposed)
    check(client.call('player.settings.transact', proposed) == preview,
          'Preview retained a receipt or changed an otherwise identical candidate')
    check(preview['preview'] and not preview['replayed'] and
          preview['settings']['revision'] == 1 and preview['control_revision'] == base['control_revision'], preview)
    unchanged = prefs(second)
    check(unchanged['settings']['revision'] == 0 and unchanged['settings']['values'] == initial_values, unchanged)
    check(unchanged['control_revision'] == base['control_revision'], 'Preview changed control revision')
    check(capture(client, 'preview') == baseline_image, 'Preview changed presented pixels')

    for values_bad in ({'camera.vertical_fov':90,'audio.master_gain':-1},
                       {'ui.scale':True},{'input.invert_x':1},{'graphics.samples':2},
                       {'unknown':1}):
        client.reject('player.settings.transact', patch(base,set=values_bad), -32602)
        after = prefs(second)
        check(after['settings']['values'] == initial_values and after['settings']['revision'] == 0 and
              after['control_revision'] == base['control_revision'], 'Invalid patch partially published')
    client.reject('player.settings.transact', patch(base,set={'ui.scale':1},reset=['ui.scale']), -32602)
    client.reject('player.settings.transact', patch(base,reset=['ui.scale','ui.scale']), -32602)
    check(profile.read_bytes() == profile_before and world.read_bytes() == authored,
          'Preview/rejected patch wrote profile or authored world')

    accepted_parameters = patch(base,set=values)
    accepted, applied = commit(client, accepted_parameters, 'live-presented')
    check(applied['settings']['values'] == values and applied['session_id'] == session, applied)
    application = applied['settings']['application']
    for key,expected in [('effective_vertical_fov',90),('effective_ui_scale',1.5),
            ('sensitivity_x',.3),('sensitivity_y',.4),('invert_x',True),('invert_y',True),
            ('requested_master_gain',.4)]:
        check(application[key] == expected, (key, application))
    if ARGS.audio:
        check(application['audio_outcome'] == 'sink_gain_verified' and
              application['sink_gain'] is not None and abs(application['sink_gain']-.4)<=1e-6, application)
    else:
        check(application['audio_outcome'] == 'disabled' and application['sink_gain'] is None, application)
    for name in GRAPHICS:
        row = applied['settings']['fields'][name]
        check(row['requested'] == values[name] and row['effective'] == 1 and
              row['requires_next_player'], row)
    live_image = capture(client, 'live')
    image_oracle(live_image, 90, 1.5, 'live')
    check(live_image != baseline_image, 'Live presentation unchanged')
    check(client.call('runtime.entity', {'session_id':session,'id':uid(100)}) == baseline_state and
          client.call('runtime.entity', {'session_id':session,'id':uid(10)}) == baseline_camera,
          'Presentation/input preferences changed native gameplay data')
    write_slot(client, configured, session, 0, 'after')
    check(stored_payload(saves,'after') == before_payload, 'Preferences leaked into gameplay snapshot')
    check(profile.read_bytes() == profile_before and world.read_bytes() == authored,
          'Live patch silently persisted preferences or authored camera/UI')

    # Exact retries recover their historical response before stale guards and
    # cannot reapply presentation state. Changed payloads collide explicitly.
    replayed = client.call('player.settings.transact', accepted_parameters)
    check(replayed == {**accepted,'replayed':True}, 'Live receipt changed on exact retry')
    client.reject('player.settings.transact', {**accepted_parameters,'set':{'ui.scale':2}}, -32010)
    client.reject('player.settings.transact', {**patch(applied,set={'ui.scale':2}),
                   'expected_settings_revision':0}, -32009)
    client.reject('player.settings.transact', {**patch(applied,set={'ui.scale':2}),
                   'expected_control_revision':base['control_revision']}, -32009)
    after_reject = prefs(second)
    check(after_reject['settings']['values'] == values and
          after_reject['settings']['revision'] == applied['settings']['revision'] and
          after_reject['control_revision'] == applied['control_revision'], 'Retry/rejection reapplied state')
    noop, nooped = commit(client, patch(after_reject), 'noop-presented')
    check(not noop['changed'] and nooped['settings']['values'] == values, noop)

    # Restore a gameplay save older than the live preference commit, preserving
    # the same window and its independent current preferences.
    player_before_restore = client.call('player.inspect')
    fresh = request_id()
    client.call('save.load', {'request_id':request_id(),'configuration_generation':configured['generation'],
        'slot':'before','expected_generation':before_write['generation'],'revision':1,
        'expected_session_id':session,'expected_tick':0,'expected_gameplay_revision':0,
        'expected_ui_revision':player_before_restore['ui_revision'],
        'expected_control_sequence':player_before_restore['control_sequence'],'new_session_id':fresh})
    restored = presented(second, nooped['settings']['revision'], 'restore-presented')
    check(restored['session_id'] == fresh and restored['player_id'] == applied['player_id'] and
          restored['settings']['values'] == values and
          restored['control_revision'] > player_before_restore['control_revision'], restored)
    check(capture(client, 'restored') == live_image, 'Gameplay restore reset live camera/UI preferences')
    check(client.call('player.settings.transact', accepted_parameters) == {**accepted,'replayed':True},
          'Historical settings receipt did not survive runtime replacement')
    check(prefs(second)['settings']['revision'] == restored['settings']['revision'],
          'Historical receipt reapplied an older settings revision')
    write_slot(client, configured, fresh, 0, 'restored')
    check(stored_payload(saves,'restored') == before_payload, 'Restore preferences changed saved gameplay')

    reset_parameters = patch(prefs(client),reset=list(LIVE+GRAPHICS))
    reset, inherited = commit(client, reset_parameters, 'reset-presented')
    check(inherited['settings']['values'] == {}, inherited)
    for name in ('sensitivity_x','sensitivity_y','invert_x','invert_y','requested_master_gain'):
        check(inherited['settings']['application'][name] == base['settings']['application'][name],
              'Reset failed to restore inherited '+name)
    for name in GRAPHICS:
        row=inherited['settings']['fields'][name]
        check(row['requested'] is None and row['effective']==1 and row['next_effective']==1 and
              not row['requires_next_player'], 'Reset retained pending graphics intent')
    if ARGS.audio:
        check(abs(inherited['settings']['application']['sink_gain']-
                  base['settings']['application']['sink_gain'])<=1e-6,
              'Reset did not restore actual inherited sink gain')
    check(inherited['settings']['fields']['camera.vertical_fov']['source'] == 'authored_camera' and
          inherited['settings']['fields']['ui.scale']['source'] == 'window_density', inherited)
    reset_image = capture(client, 'reset')
    image_oracle(reset_image, 60, density, 'reset')
    check(reset_image == baseline_image, 'Reset did not restore true inherited presentation')
    # A profile update is persistent intent only. A subsequent player explicitly
    # loading the profile consumes it; the current native window does not.
    profile_change = client.call('settings.transact', {
        'path':service.native(profile),'request_id':request_id(),'expected_revision':created['revision'],
        'set':{'camera.vertical_fov':75,'ui.scale':1.25,'audio.master_gain':.7}})
    profile_changed = profile.read_bytes()
    after_profile = prefs(second)
    check(after_profile['settings']['revision'] == inherited['settings']['revision'] and
          after_profile['settings']['values'] == {}, 'Profile write auto-applied live preferences')
    check(capture(client, 'profile-not-live') == baseline_image, 'Profile write changed existing pixels')
    terminal = stop(client, 'first-stopped')
    parameters.update(request_id=request_id(),expected_generation=terminal['generation'],
                      session_id=fresh,settings_revision=profile_change['revision'])
    client.call('player.start', parameters)
    relaunched = presented(client,0,'profile-relaunch-presented')
    check(relaunched['settings']['values'] == {**initial_values,'camera.vertical_fov':75,
          'ui.scale':1.25,'audio.master_gain':.7}, relaunched)
    check(relaunched['settings']['application']['requested_master_gain']==.7, relaunched)
    if ARGS.audio:
        check(abs(relaunched['settings']['application']['sink_gain']-.7)<=1e-6, relaunched)
    image_oracle(capture(client,'relaunched'),75,1.25,'relaunched')
    # A resume request does not imply that Windows grants foreground focus.
    # Submit one live patch after resume and record actual owner state; only a
    # genuinely unpaused response qualifies the running-application branch.
    controls(client,'resume')
    before_running = client.call('player.inspect')
    running_parameters = patch(prefs(client),set={'audio.master_gain':.6})
    running_reply = client.reply('player.settings.transact',running_parameters)
    if 'error' in running_reply:
        check(running_reply['error']['code']==-32009, running_reply)
        # A focus transition legitimately invalidates the optimistic guard.
        controls(client,'pause')
        running_parameters=patch(prefs(client),set={'audio.master_gain':.6})
        running_reply=client.reply('player.settings.transact',running_parameters)
    check('result' in running_reply, running_reply)
    running_result=running_reply['result']
    presented(client,running_result['settings']['revision'],'resumed-live-presented')
    after_running=client.call('player.inspect')
    RECORD['running_application']={'before':before_running,'after':after_running,
        'accepted':True,'unpaused_application_qualified':not before_running['paused'] and not after_running['paused'],
        'scope':'Real focus observed; accepting resume alone does not qualify physical input.'}
    controls(client,'pause')
    stopped = stop(client,'relaunched-stopped')

    # Semantic replay degrees stay semantic, irrespective of pointer tuning.
    # Replay permits cancellation but rejects live preferences even when paused.
    controller_before = client.call('runtime.entity', {'session_id':fresh,'id':uid(100)})
    replay_start_tick=stopped['tick']
    replay = service.start_parameters(fresh, generation=stopped['generation'],expected_tick=replay_start_tick,
        camera=uid(10),gpu=ARGS.gpu,
        width=WIDTH,height=HEIGHT,mode='replay',sequence=[{'ticks':1,'look':[7,-3]}],
        settings_overrides={'input.sensitivity_x':10,'input.sensitivity_y':10,
                           'input.invert_x':True,'input.invert_y':True})
    client.call('player.start', replay)
    replay_ready = presented(client,0,'replay-presented')
    client.reject('player.settings.transact',patch(replay_ready,set={'ui.scale':2}),-32080)
    check(prefs(second)['settings']['revision'] == 0, 'Replay rejected patch changed preference revision')
    controls(client,'resume')
    replay_terminal = service.await_state(client,lambda state:not state['active'],'replay-finished')
    check(replay_terminal['tick'] == replay_start_tick+1 and replay_terminal['report']['success'],replay_terminal)
    controller_after = client.call('runtime.entity', {'session_id':fresh,'id':uid(100)})
    check(abs(controller_after['yaw']-controller_before['yaw']-7)<=1e-9 and
          abs(controller_after['pitch']-controller_before['pitch']+3)<=1e-9,
          'Pointer preferences reinterpreted semantic replay look')
    # Exercise the actual future-renderer dependency validator on a presented
    # deferred player. A valid live UI change in the same invalid batch must
    # not publish or change existing pixels when four-sample intent is rejected.
    deferred_parameters=service.start_parameters(fresh,generation=replay_terminal['generation'],
        expected_tick=replay_terminal['tick'],camera=uid(10),gpu=ARGS.gpu,
        width=WIDTH,height=HEIGHT,lighting_path='deferred',settings_overrides={'ui.scale':1})
    client.call('player.start',deferred_parameters)
    deferred=presented(client,0,'dependency-deferred-presented')
    native_before=client.call('player.inspect')
    check(native_before['report']['samples']==1 and
          native_before['report']['render_diagnostics']['lighting_path']=='deferred',native_before)
    deferred_image=capture(client,'dependency-before')
    dependency_parameters=patch(deferred,set={'graphics.samples':4,'ui.scale':2})
    rejected=client.reject('player.settings.transact',dependency_parameters,-32602)
    preserved=prefs(second)
    native_after=client.call('player.inspect')
    for key in ('revision','values','sources','fields','application'):
        check(preserved['settings'][key]==deferred['settings'][key],
              'Invalid future-renderer batch partially changed '+key)
    check(preserved['control_revision']==deferred['control_revision'] and
          preserved['session_id']==deferred['session_id'] and native_after['tick']==native_before['tick'] and
          native_after['report']['samples']==1 and
          native_after['report']['render_diagnostics']['lighting_path']=='deferred',
          'Invalid future-renderer batch changed owner or actual launch graphics')
    check(capture(client,'dependency-after')==deferred_image,
          'Invalid future-renderer batch applied its live UI subset')
    RECORD['graphics_dependency_rejection']={'before':deferred,'after':preserved,
        'rejected':rejected,'exact_context_pixels_preserved':True,
        'actual_lighting_path':'deferred','actual_samples':1}
    stop(client,'dependency-deferred-stopped')
    client.call('runtime.stop',{'session_id':fresh})
    check(world.read_bytes() == authored and profile.read_bytes() == profile_changed,
          'Player lifecycle wrote authoring state or silently saved live overrides')
    RECORD['graphics'] = {'passed':True,'gpu_index':ARGS.gpu,
        'hardware_name':replay_terminal['report']['gpu'],'actual_audio_sink_requested':ARGS.audio,
        'live_application':application,'replay_before':controller_before,'replay_after':controller_after}
    RECORD['checks'] = {name:True for name in (
        'independent_fov_projection','independent_absolute_ui_pixel_bounds','preview_no_publication',
        'invalid_patch_atomic','guarded_live_commit','pending_graphics_not_recreated',
        'snapshot_authoring_profile_isolation','historical_retry_not_reapplied',
        'changed_request_id_collision','stale_revisions_rejected','accepted_noop_revision',
        'save_replacement_retains_live_preferences','reset_restores_inheritance',
        'profile_write_not_live','explicit_profile_relaunch','semantic_replay_not_pointer_input',
        'replay_live_mutation_rejected','future_graphics_dependency_atomic')}


def main():
    global ARGS, RECORD, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--capture',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--audio',action='store_true',help='Check numeric actual sink gain, not physical audibility')
    parser.add_argument('--timeout',type=int,default=90)
    parser.add_argument('--total-timeout',type=int,default=900)
    ARGS = parser.parse_args()
    check(ARGS.binary.is_file() and 30<=ARGS.timeout<=90,'Binary and RPC timeout30..90 required')
    check(ARGS.timeout<=ARGS.total_timeout<=1800,'Overall timeout must cover the RPC bound and be at most1800s')
    DEADLINE=time.monotonic()+ARGS.total_timeout
    if (ARGS.capture or ARGS.audio) and ARGS.output is None:
        parser.error('Hardware checks require a new explicit --output')
    if ARGS.audio and not ARGS.capture:
        parser.error('--audio requires --capture')
    temporary = ARGS.output is None
    if temporary:
        directory=ROOT/'build'/'player-live-settings-contract';directory.mkdir(parents=True,exist_ok=True)
        ARGS.output=Path(tempfile.mkdtemp(prefix='admission-',dir=directory))
    else:
        ARGS.output=ARGS.output.resolve();ARGS.output.mkdir(parents=True,exist_ok=False)
    RECORD={'passed':False,'binary_sha256':service.sha(ARGS.binary),
        'verifier_sha256':service.sha(Path(__file__)),
        'helper_sha256':service.sha(Path(service.__file__)),
        'pixel_decoder_sha256':service.sha(ROOT/'tests'/'scene_capture.py'),
        'owners':[],'calls':[],'rpc_timeout_seconds':ARGS.timeout,
        'overall_timeout_seconds':ARGS.total_timeout,
        'limitations':['No physical pointer/controller input or latency/performance claim',
                       'No physical audio audibility or generated media',
                       'Native service preferences; no compiled C# menu qualification',
                       'Greybox FOV/UI oracle; no imported-character visual claim',
                       'Client queue order does not establish one host poll batch']}
    service.ARGS,service.RECORD=ARGS,RECORD
    world=ARGS.output/'world.json';endpoint='live-settings-'+request_id()
    host=client=second=None
    try:
        host=service.ProcessOwner([str(ARGS.binary.resolve()),'serve',service.native(world),
                   '--endpoint',endpoint],'live-settings-host',endpoint,'serve')
        client=Client(endpoint,'primary-client');second=Client(endpoint,'observer-client')
        discovery=service.common_checks(client,second)
        admission_checks(client,discovery,world)
        if ARGS.capture:
            graphics_checks(client,second,world)
        second.close();second=None
        check(host.process.poll() is None,'Disconnecting observer killed owner')
        client.call('host.shutdown');client.close();client=None
        host.process.wait(timeout=20);host.close();host=None
        for owner in RECORD['owners']:
            expected='Poima shared world ready: '+endpoint+'\n' if owner['label']=='live-settings-host' else ''
            check(owner['exit_code']==0 and not owner['stderr_truncated'] and owner['stderr']==expected,owner)
        for file,key in ((ARGS.binary,'binary_sha256'),(Path(__file__),'verifier_sha256'),
                         (Path(service.__file__),'helper_sha256'),(ROOT/'tests'/'scene_capture.py','pixel_decoder_sha256')):
            check(service.sha(file)==RECORD[key], 'Qualification input changed '+str(file))
        RECORD['rpc_count']=len(RECORD['calls']);RECORD['passed']=True
    except BaseException as exc:
        RECORD['failure']=repr(exc);RECORD['traceback']=traceback.format_exc()
        raise
    finally:
        active_failure=sys.exc_info()[0] is not None
        cleanup=[]
        for owner in (second,client,host):
            if owner is not None:
                try: owner.close(expected=False)
                except BaseException as exc: cleanup.append(repr(exc))
        if cleanup:
            RECORD['passed']=False;RECORD['cleanup_errors']=cleanup
        (ARGS.output/'evidence.json').write_text(json.dumps(RECORD,indent=2)+'\n',encoding='utf-8')
        if not RECORD['passed']:
            print('Live-settings evidence retained: '+str(ARGS.output),file=sys.stderr)
        if cleanup and not active_failure:
            raise AssertionError(cleanup)
    if temporary:
        shutil.rmtree(ARGS.output)
    print(json.dumps({key:RECORD[key] for key in ('passed','rpc_count')}))


if __name__=='__main__':
    main()
