#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real compiled player preference callbacks and optional native presentation.

Accepts an already compiled CoreCLR or Native AOT fixture config. Builds nothing,
uses no network, creates a new owned output, and cleans up only its own processes.
Headless checks qualify absent-owner callbacks only. --capture additionally runs
the same compiled Control/Tick callbacks against a real paused native player.
This source candidate has not been executed or qualified merely by being added.
"""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import uuid

import player_live_settings_contract as live
import player_service_contract as service

ROOT = Path(__file__).resolve().parents[1]
GAME = 'Poima.Tests.ManagedPlayerPreferencesGame'
IDENTITY = 'poima.test.player-preferences'
SLOT = 'player-preferences'
ARGS = RECORD = DEADLINE = None
check, uid, sha = service.check, service.uid, service.sha
ACTIONS = ('pref.refresh', 'pref.stage.throw', 'pref.stage.invalid', 'pref.stage.save',
           'pref.stage.load', 'pref.remember', 'pref.stale', 'pref.query',
           'pref.reset', 'pref.reset.all', 'pref.graphics.low', 'pref.graphics.high')
IDS = {name: uid(601+i) for i, name in enumerate(ACTIONS)}
MENU_ACTIONS = ('pref.fov.minus', 'pref.fov.plus', 'pref.sensitivity.minus',
                'pref.sensitivity.plus', 'pref.invert', 'pref.ui.minus', 'pref.ui.plus',
                'pref.gain.minus', 'pref.gain.plus', 'pref.reset', 'pref.reset.all',
                'pref.graphics.low', 'pref.graphics.high', 'pref.refresh', 'pref.close')
MENU_IDS = {name:uid(710+i) for i,name in enumerate(MENU_ACTIONS)}
MENU_IDS['pref.open'] = uid(699)


def fresh():
    return uuid.uuid4().hex


class Client(service.Client):
    def send(self, method, params=None):
        check(time.monotonic() < DEADLINE, 'Compiled preference workload deadline expired')
        return super().send(method, params)

    def receive(self):
        remaining = DEADLINE-time.monotonic()
        check(remaining > 0, 'Compiled preference workload deadline expired')
        previous = ARGS.timeout
        ARGS.timeout = min(previous, remaining)
        try:
            return super().receive()
        finally:
            ARGS.timeout = previous


def configuration(path):
    check(path.stat().st_size <= 1024*1024, 'Config exceeds 1MiB')
    config = json.loads(path.read_text(encoding='utf-8'))
    check(type(config) is dict, 'Config must be an object')
    native = 'descriptor' in config
    allowed = {'descriptor', 'expected_descriptor_sha256'} if native else {'hostfxr', 'bridge', 'assembly', 'type'}
    check(set(config) <= allowed, 'Unexpected gameplay config keys')
    check(native or {'hostfxr', 'bridge', 'assembly'} <= set(config), 'Supply CoreCLR paths or Native AOT descriptor')
    paths, pins = {}, {str(path): sha(path)}
    for key in ('hostfxr', 'bridge', 'assembly', 'descriptor'):
        if key in config:
            selected = Path(config[key])
            selected = (selected if selected.is_absolute() else path.parent/selected).resolve(strict=True)
            check(selected.is_file(), 'Compiled input is not a file')
            paths[key] = selected
            config[key] = service.native(selected)
            pins[str(selected)] = sha(selected)
    if native:
        descriptor = paths['descriptor']
        check(descriptor.stat().st_size <= 1024*1024, 'Descriptor exceeds 1MiB')
        document = json.loads(descriptor.read_text(encoding='utf-8'))
        check(document['type'] == GAME and document['identity'] == IDENTITY and
              document['services_version'] == 7 and document['minimum_services_bytes'] == 256 and
              'player_preferences_v1' in document['required_features'], 'Supply the compiled .81 preference fixture')
        for row in document['files']:
            relative = Path(row['path'])
            selected = (descriptor.parent/relative).resolve(strict=True)
            check(not relative.is_absolute() and '..' not in relative.parts and
                  selected.is_relative_to(descriptor.parent) and selected.is_file() and
                  selected.stat().st_size == row['size'] and sha(selected) == row['sha256'],
                  'Native AOT artifact closure differs')
            pins[str(selected)] = sha(selected)
        config['expected_descriptor_sha256'] = sha(descriptor)
        image = (descriptor.parent/document['library']).resolve(strict=True)
        check(str(image) in pins, 'Native library is absent from verified inventory')
    else:
        check(config.get('type', GAME) == GAME, 'Config selects another managed consumer')
        config['type'] = GAME
        image = paths['assembly']
        for key in ('bridge', 'assembly'):
            sdk = paths[key].parent/'Poima.Gameplay.dll'
            check(sdk.is_file(), 'Compiled consumer/bridge SDK closure missing')
            pins[str(sdk)] = sha(sdk)
    return config, pins, sha(image)


def author(client):
    original = live.author(client)
    button = copy.deepcopy(next(op for op in original if op.get('id') == uid(502)))
    button['element']['action'] = 'live'
    # Offscreen, eligible actions preserve the independent .80 proof rectangle.
    ops = [button, {'op':'ui.element.set', 'id':uid(600), 'element':{
        'parent':None, 'name':'Compiled preference contract actions', 'kind':'panel',
        'text':'', 'action':None, 'visible':True, 'enabled':True,
        'layout':{'position':'absolute', 'left':{'unit':'dp','value':2000},
                  'top':{'unit':'dp','value':0}, 'width':{'unit':'dp','value':200},
                  'height':{'unit':'dp','value':800}, 'direction':'column'}}}]
    for action, identity in IDS.items():
        ops.append({'op':'ui.element.set', 'id':identity, 'element':{
            'parent':uid(600), 'name':action, 'kind':'button', 'text':action,
            'action':action, 'visible':True, 'enabled':True}})
    def dp(value):
        return {'unit':'dp', 'value':value}
    def percent(value):
        return {'unit':'percent', 'value':value}
    ops += [{'op':'ui.element.set', 'id':uid(699), 'element':{
        'parent':None, 'name':'Open compiled settings menu', 'kind':'button', 'text':'Settings menu',
        'action':'pref.open', 'visible':True, 'enabled':True,
        'layout':{'position':'absolute', 'left':dp(200), 'top':dp(16), 'width':dp(140), 'height':dp(36)},
        'style':{'background_color':'#353039', 'color':'#f7e8eb', 'font_size':14}}},
        {'op':'ui.element.set', 'id':uid(700), 'element':{
        'parent':None, 'name':'Compiled player settings', 'kind':'panel', 'text':'', 'action':None,
        'visible':False, 'enabled':True,
        'layout':{'position':'absolute', 'left':dp(0), 'top':dp(0), 'width':percent(100),
                  'height':percent(100), 'direction':'column', 'align':'stretch', 'justify':'start',
                  'padding':[8,8,8,8], 'gap':2},
        'style':{'background_color':'#1e1b20', 'border_radius':4, 'border_width':0}}},
        {'op':'ui.element.set', 'id':uid(701), 'element':{
        'parent':uid(700), 'name':'Current native preference status', 'kind':'label',
        'text':'Open settings to read the current native player.', 'action':None, 'visible':True, 'enabled':True,
        'layout':{'position':'absolute', 'left':percent(2), 'top':percent(3),
                  'width':percent(96), 'height':percent(20)},
        'style':{'font_size':10, 'color':'#f7e8eb'}}}]
    labels = ('FOV -5', 'FOV +5', 'Sensitivity -0.05', 'Sensitivity +0.05', 'Toggle invert Y',
              'UI scale -0.25', 'UI scale +0.25', 'Master gain -0.1', 'Master gain +0.1',
              'Reset FOV/UI/gain', 'Reset all overrides', 'Next player: 1 sample / 1 frame',
              'Next player: 4 samples / 2 frames', 'Refresh accepted/native status', 'Close and resume')
    # Explicit percentage rows avoid inherited legacy panel/button margins
    # squeezing the native controls at nondefault UI densities.
    for row in range(8):
        row_id = uid(730+row)
        ops.append({'op':'ui.element.set', 'id':row_id, 'element':{
            'parent':uid(700), 'name':'Settings row '+str(row), 'kind':'panel', 'text':'',
            'action':None, 'visible':True, 'enabled':True,
            'layout':{'position':'absolute', 'left':percent(2), 'top':percent(25+row*9),
                      'width':percent(96), 'height':percent(8), 'padding':[0,0,0,0]},
            'style':{'background_color':'#00000000', 'border_width':0}}})
        for index in range(row*2, min(row*2+2, len(MENU_ACTIONS))):
            action = MENU_ACTIONS[index]
            ops.append({'op':'ui.element.set', 'id':MENU_IDS[action], 'element':{
                'parent':row_id, 'name':labels[index], 'kind':'button', 'text':labels[index],
                'action':action, 'visible':True, 'enabled':True,
                'layout':{'position':'absolute', 'left':percent(51*(index%2)), 'top':dp(0),
                          'width':percent(49), 'height':percent(100), 'padding':[4,6,4,6]},
                'style':{'font_size':12, 'color':'#f7e8eb', 'border_width':0,
                         'background_color':'#722f37' if action=='pref.close' else '#353039',
                         'hover':{'background_color':'#4f1a23'},
                         'focus':{'background_color':'#4f1a23'},
                         'pressed':{'background_color':'#2b0d13'}}}})
    ops += [{'op':'entity.create', 'id':uid(200), 'name':'Earlier physics rollback witness'},
            {'op':'component.set', 'id':uid(200), 'type':'Transform', 'value':{
                'position':[100,10,0], 'rotation':[0,0,0,1], 'scale':[1,1,1]}},
            {'op':'component.set', 'id':uid(200), 'type':'BoxCollider', 'value':{
                'motion':'dynamic', 'half_extents':[.5,.5,.5], 'mass':1,
                'friction':.5, 'restitution':0}}]
    client.call('world.transact', {'base_revision':1, 'request_id':fresh(), 'ops':ops})


class Probe:
    def __init__(self, client, config, image_sha):
        self.client, self.config, self.image_sha = client, config, image_sha
        self.session = None

    def runtime(self):
        return self.client.call('runtime.inspect', {'session_id':self.session})

    def game(self):
        return self.client.call('runtime.gameplay.inspect', {'session_id':self.session})

    def values(self):
        return self.game()['module']['values']

    def ui(self):
        return self.client.call('runtime.ui.inspect', {'session_id':self.session, 'tick':self.runtime()['tick']})

    def start(self):
        self.session = fresh()
        revision = self.client.call('world.inspect')['revision']
        self.client.call('runtime.start', {'session_id':self.session, 'revision':revision})
        self.load()

    def load(self):
        state, game = self.runtime(), self.game()
        method = 'runtime.gameplay.load_native' if 'descriptor' in self.config else 'runtime.gameplay.load'
        result = self.client.call(method, {'session_id':self.session, 'request_id':fresh(),
            'expected_tick':state['tick'], 'expected_revision':game['revision'],
            'expected_structure_revision':state['structure_revision'], **self.config})
        module = result['module']
        check(module['type'] == GAME and module['schema']['identity'] == IDENTITY and
              module['assembly_sha256'] == self.image_sha and module['backend'] ==
              ('native_aot' if 'descriptor' in self.config else 'coreclr'), 'Wrong actual compiled consumer')
        RECORD.setdefault('modules', []).append(module)

    def edit(self, **values):
        state, game = self.runtime(), self.game()
        return self.client.call('runtime.gameplay.edit', {'session_id':self.session, 'request_id':fresh(),
            'expected_tick':state['tick'], 'expected_revision':game['revision'],
            'expected_structure_revision':state['structure_revision'], 'values':values})

    def control_parameters(self, action, element=None):
        state, game = self.runtime(), self.game()
        return {'session_id':self.session, 'request_id':fresh(), 'expected_tick':state['tick'],
            'expected_ui_revision':state['ui_revision'], 'expected_control_sequence':state['control_sequence'],
            'expected_gameplay_revision':game['revision'], 'expected_structure_revision':state['structure_revision'],
            'id':element if element else uid(502) if action == 'live' else IDS[action]}

    def activate(self, action, element=None):
        request = self.control_parameters(action, element)
        result = self.client.call('runtime.ui.activate', request)
        self.session = result['current_session_id']
        return request, result

    def step(self, count, error=None):
        state = self.runtime()
        request = {'session_id':self.session, 'request_id':fresh(), 'expected_tick':state['tick'],
                   'expected_structure_revision':state['structure_revision'], 'ticks':count}
        return self.client.reject('runtime.step', request, error) if error else self.client.call('runtime.step', request)

    def state(self, preferences=False):
        value = {'runtime':self.runtime(), 'game':self.game(), 'ui':self.ui(),
                 'save':self.client.call('runtime.save.status', {'session_id':self.session}),
                 'body':self.client.call('runtime.entity', {'session_id':self.session, 'id':uid(200)})}
        if preferences:
            observed = live.prefs(self.client)
            value['preferences'] = {key:observed[key] for key in ('player_id', 'session_id', 'control_revision')}
            value['preferences']['settings'] = {key:observed['settings'][key]
                for key in ('format', 'revision', 'values', 'fields')}
        return value

    def save(self, configured, slot):
        state, game = self.runtime(), self.game()
        return self.client.call('save.write', {'request_id':fresh(),
            'configuration_generation':configured['generation'], 'slot':slot, 'expected_generation':0,
            'session_id':self.session, 'expected_tick':state['tick'], 'expected_gameplay_revision':game['revision'],
            'expected_structure_revision':state['structure_revision'], 'expected_ui_revision':state['ui_revision'],
            'expected_control_sequence':state['control_sequence']})


def integer(values, name):
    value = values[name]
    check(type(value) in (int, str), ('Expected typed integer', name, value))
    return int(value)


def owner(values):
    return integer(values, 'OwnerHigh'), integer(values, 'OwnerLow')


def headless(probe):
    probe.start()
    probe.step(1)
    value = probe.values()
    check(integer(value, 'Available') == 0 and integer(value, 'Revision') == -1,
          'Compiled unattached snapshot invents an owner/revision')
    probe.activate('live')
    check(integer(probe.values(), 'StageRejection') == 1, 'Compiled unattached patch did not return unavailable')
    probe.edit(Mode=4)
    before = probe.state()
    probe.step(1, -32040)
    check(probe.state() == before, 'Failed unattached Tick escaped native rollback')
    probe.client.call('runtime.stop', {'session_id':probe.session})
    probe.session = None
    RECORD['checks'].append('Real compiled unattached Tick/Control snapshot and patch rejection; failed callback restores native state.')


def graphics(probe, observer, world):
    client = probe.client
    probe.start()
    parameters = service.start_parameters(probe.session, camera=uid(10), width=live.WIDTH,
        height=live.HEIGHT, gpu=ARGS.gpu, audio=ARGS.audio, settings_overrides={'ui.scale':1})
    parameters.pop('controller')
    ack = client.call('player.start', parameters)
    initial = live.presented(observer, 0, 'compiled-initial')
    probe.activate('pref.refresh')
    value = probe.values()
    initial_owner = owner(value)
    check(integer(value, 'Available') == 1 and integer(value, 'Revision') == 0 and
          initial_owner != (0,0), 'Compiled attached snapshot missing native owner')
    live.image_oracle(live.capture(client, 'compiled-initial'), 60, 1, 'compiled-initial')
    authored = world.read_bytes()
    stale_external = live.patch(initial, set={'ui.scale':2})
    request, receipt = probe.activate('live')
    value = probe.values()
    check(integer(value, 'StageRejection') == 0 and integer(value, 'ResultState') == 1 and
          integer(value, 'Revision') == 0 and integer(value, 'ReadAfterRevision') == 0 and
          integer(value, 'TicketSequence') == 1, 'Staged patch published inside Control or consumed wrong ticket')
    changed = live.presented(observer, 1, 'compiled-live')
    check(changed['player_id'] == ack['player_id'] and changed['settings']['values']['camera.vertical_fov'] == 65 and
          changed['settings']['values']['ui.scale'] == 1.25 and
          math.isclose(changed['settings']['values']['audio.master_gain'], .9), 'Compiled callback missed shared authority')
    live.image_oracle(live.capture(client, 'compiled-live'), 65, 1.25, 'compiled-live')
    client.reject('player.settings.transact', stale_external, -32009)
    before = probe.state(True)
    check(client.call('runtime.ui.activate', request) == {**receipt, 'replayed':True} and
          probe.state(True) == before, 'Exact Control retry invoked compiled handler or republished preferences')
    probe.activate('pref.refresh')
    value = probe.values()
    check(integer(value, 'ResultState') == 2 and integer(value, 'AcceptedRevision') == 1,
          'Committed ticket did not retain immutable accepted revision')
    for action in ('pref.stage.throw', 'pref.stage.invalid'):
        before = probe.state(True)
        client.reject('runtime.ui.activate', probe.control_parameters(action), -32040)
        check(probe.state(True) == before, 'Failed Control leaked game/UI/physics/preferences/ticket '+action)
    probe.activate('live')
    live.presented(observer, 2, 'compiled-after-rollback')
    check(integer(probe.values(), 'TicketSequence') == 2, 'Failed Control consumed preference sequence')
    # An earlier successful Tick really steps the falling body before the later
    # callback fails. Equality proves rollback of that completed native physics.
    for mode, count in ((2,2), (3,1), (4,1)):
        probe.edit(Mode=mode)
        before = probe.state(True)
        probe.step(count, -32040)
        check(probe.state(True) == before, ('Whole native batch rollback failed', mode))
    probe.edit(Mode=1)
    baseline_tick = probe.runtime()['tick']
    baseline_body = probe.state()['body']
    probe.step(2)
    live.presented(observer, 3, 'compiled-batch')
    value = probe.values()
    check(integer(value, 'StageRejection') == 5 and integer(value, 'ReadAfterRevision') == 2 and
          integer(value, 'TicketSequence') == 3 and probe.runtime()['tick'] == baseline_tick+2 and
          probe.state()['body']['local_transform']['position'] != baseline_body['local_transform']['position'],
          'Batch allowed multiple patches, observed staged revision, or skipped actual physics')
    probe.edit(Mode=0)
    probe.activate('pref.refresh')
    check(integer(probe.values(), 'AcceptedRevision') == 3, 'Batch ticket lost accepted result')
    RECORD['checks'].append('Actual failed Control/entity-reference and later multi-tick callback rollback; completed falling-body physics restored; one batch patch and busy rejection.')

    probe.activate('pref.graphics.high')
    current = live.presented(observer, 4, 'compiled-next-graphics')
    for key, expected in (('graphics.samples',4), ('graphics.frames_in_flight',2)):
        row = current['settings']['fields'][key]
        check(row['requested'] == expected and row['effective'] == 1 and row['requires_next_player'],
              'Compiled graphics changed frozen launch configuration')
    probe.activate('pref.remember')
    external = live.patch(live.prefs(client), set={'input.sensitivity_x':.7})
    accepted = client.call('player.settings.transact', external)
    live.presented(observer, 5, 'external-after-remember')
    probe.activate('pref.stale')
    check(integer(probe.values(), 'StageRejection') == 3 and live.prefs(client)['settings']['revision'] == 5,
          'Compiled stale guard silently overwrote external intent')
    check(client.call('player.settings.transact', external) == {**accepted, 'replayed':True}, 'External receipt changed')
    # CoreCLR permits compatible code reload. Native images retain their
    # documented process-lifetime replacement restriction.
    before_preferences = live.prefs(client)['settings']['values']
    if 'descriptor' not in probe.config:
        probe.load()
    else:
        state, game = probe.runtime(), probe.game()
        before = probe.state(True)
        client.reject('runtime.gameplay.load_native', {'session_id':probe.session, 'request_id':fresh(),
            'expected_tick':state['tick'], 'expected_revision':game['revision'],
            'expected_structure_revision':state['structure_revision'], **probe.config}, -32060)
        check(probe.state(True) == before, 'Unsupported native image replacement mutated owner/runtime')
    probe.activate('pref.refresh')
    check(owner(probe.values()) == initial_owner and live.prefs(client)['settings']['values'] == before_preferences,
          'Compatible compiled reload replaced preference authority')

    saves = ARGS.output/'saves'
    saves.mkdir()
    configured = client.call('save.configure', {'request_id':fresh(), 'expected_generation':0,
                                              'root':service.native(saves)})
    probe.activate('pref.stage.save')
    current_revision = live.prefs(client)['settings']['revision']
    live.presented(observer, current_revision, 'compiled-save')
    inspect = client.call('save.inspect', {'slot':SLOT})
    check(inspect['generation'] == 1 and inspect['selected']['verified'], 'Compiled callback save not durably serviced')
    probe.activate('pref.refresh')
    saved_ticket = integer(probe.values(), 'TicketSequence')
    check(integer(probe.values(), 'AcceptedRevision') == current_revision, 'Preference flush lost callback save ticket')
    probe.save(configured, 'isolation-before')
    before_payload = live.stored_payload(saves, 'isolation-before')
    external = live.patch(live.prefs(client), set={'input.sensitivity_y':.8})
    client.call('player.settings.transact', external)
    live.presented(observer, current_revision+1, 'compiled-save-isolation')
    probe.save(configured, 'isolation-after')
    check(live.stored_payload(saves, 'isolation-after') == before_payload,
          'Live preference owner/receipt ledger leaked into native gameplay save')
    old_session = probe.session
    old_player = client.call('player.inspect')['player_id']
    _, loaded = probe.activate('pref.stage.load')
    check(loaded['runtime_replaced'] and loaded['save_serviced'] and probe.session != old_session,
          'Compiled preference/load Control did not replace original Runtime')
    after_load_revision = live.prefs(client)['settings']['revision']
    check(after_load_revision == current_revision+2, 'Preference stage disappeared before queued Runtime load')
    check(live.prefs(client)['settings']['values']['input.sensitivity_y'] == .8,
          'Game save imported old preferences over current owner')
    live.presented(observer, after_load_revision, 'compiled-load')
    probe.activate('pref.refresh')
    check(owner(probe.values()) == initial_owner and client.call('player.inspect')['player_id'] == old_player and
          integer(probe.values(), 'TicketSequence') == saved_ticket and
          integer(probe.values(), 'AcceptedRevision') == current_revision,
          'Save restored preference authority instead of querying same-owner retained ticket')
    client.reject('runtime.ui.activate', {'session_id':old_session, 'request_id':fresh(), **{
        key:value for key,value in request.items() if key not in ('session_id','request_id')}}, -32030)
    before = live.prefs(client)
    probe.activate('pref.reset.all')
    reset = live.presented(observer, before['settings']['revision']+1, 'compiled-reset')
    check(reset['settings']['values'] == {} and
          reset['settings']['fields']['camera.vertical_fov']['source'] == 'authored_camera' and
          reset['settings']['fields']['ui.scale']['source'] == 'window_density', 'Compiled reset did not restore sparse inheritance')
    live.image_oracle(live.capture(client, 'compiled-reset'), 60,
                      reset['settings']['application']['effective_ui_scale'], 'compiled-reset')
    if ARGS.audio:
        application = reset['settings']['application']
        check(application['audio_outcome'] == 'sink_gain_verified' and
              math.isclose(application['sink_gain'], application['requested_master_gain'], abs_tol=1e-6),
              'Actual SDL sink gain was not numerically verified')
    before_menu = live.prefs(client)
    probe.activate('pref.open', MENU_IDS['pref.open'])
    menu = probe.ui()
    check(menu['modal'] == uid(700) and next(row for row in menu['elements'] if row['id'] == uid(700))['visible'],
          'Compiled menu opener did not show the actual native modal panel')
    probe.activate('pref.ui.plus', MENU_IDS['pref.ui.plus'])
    menu_preferences = live.presented(observer, before_menu['settings']['revision']+1, 'compiled-menu-ui')
    check(math.isclose(menu_preferences['settings']['values']['ui.scale'],
                       reset['settings']['application']['effective_ui_scale']+.25),
          'Reachable native menu tuning button did not stage the compiled patch')
    probe.activate('pref.refresh', MENU_IDS['pref.refresh'])
    live.capture(client, 'compiled-settings-menu')
    _, closed = probe.activate('pref.close', MENU_IDS['pref.close'])
    check(closed['intent'] == 1, 'Compiled menu close did not request resume')
    client.call('player.control', service.control_parameters(client.call('player.inspect'), 'pause'))
    service.await_state(observer, lambda state:state['active'] and state['paused'], 'compiled-menu-paused')
    check(world.read_bytes() == authored, 'Compiled preferences silently wrote authored world')
    RECORD['checks'].append('Authored reachable native settings menu opens modal, tunes through compiled Control, refreshes visible status and closes with native resume intent.')
    RECORD['checks'].append('Native same-window independent FOV/UI pixel oracles, guarded external conflicts, sparse reset and frozen next-launch graphics; CoreCLR reload or unchanged native replacement rejection.')
    RECORD['checks'].append('Compiled callback stages preferences plus durable Save/Load; flush survives original Runtime replacement; owner/receipt state stays outside gameplay saves.')

    # Keep the saved old-owner ticket in TState through a fresh native window.
    state = client.call('player.inspect')
    client.call('player.control', service.control_parameters(state, 'stop'))
    terminal = service.await_state(observer, lambda state:not state['active'], 'compiled-stopped')
    probe.activate('pref.refresh')
    check(integer(probe.values(), 'Available') == 0, 'Detached Runtime still accessed stopped owner')
    parameters = service.start_parameters(probe.session, generation=terminal['generation'],
        expected_tick=probe.runtime()['tick'], camera=uid(10), width=live.WIDTH, height=live.HEIGHT,
        gpu=ARGS.gpu, audio=ARGS.audio)
    parameters.pop('controller')
    new_ack = client.call('player.start', parameters)
    live.presented(observer, 0, 'compiled-fresh-owner')
    probe.activate('pref.query')
    value = probe.values()
    check(new_ack['player_id'] != old_player and owner(value) != initial_owner and
          integer(value, 'ResultState') == 0 and integer(value, 'ResultRejection') == 2,
          'Fresh player inherited prior-owner receipt/configuration')
    client.call('player.control', service.control_parameters(client.call('player.inspect'), 'stop'))
    terminal = service.await_state(observer, lambda state:not state['active'], 'compiled-fresh-stopped')
    RECORD['checks'].append('Stop detaches callback availability; fresh player owner rejects a saved old ticket without importing previous preferences.')

    probe.edit(Mode=1)
    replay_tick = probe.runtime()['tick']
    character = client.call('runtime.entity', {'session_id':probe.session, 'id':uid(100)})
    parameters = service.start_parameters(probe.session, generation=terminal['generation'],
        expected_tick=probe.runtime()['tick'], camera=uid(10), width=live.WIDTH, height=live.HEIGHT,
        gpu=ARGS.gpu, mode='replay', sequence=[{'ticks':1, 'look':[7,-3]}])
    client.call('player.start', parameters)
    live.presented(observer, 0, 'compiled-replay')
    client.reject('runtime.ui.activate', probe.control_parameters('live'), -32080)
    client.call('player.control', service.control_parameters(client.call('player.inspect'), 'resume'))
    finished = service.await_state(observer, lambda state:not state['active'], 'compiled-replay-finished')
    check(integer(probe.values(), 'Replay') == 1 and integer(probe.values(), 'StageRejection') == 4 and
          live.prefs(client)['settings']['revision'] == 0, 'Compiled callback mutated replay preferences')
    after = client.call('runtime.entity', {'session_id':probe.session, 'id':uid(100)})
    check(finished['tick'] == replay_tick+1 and finished['report']['success'] and
          math.isclose(after['yaw']-character['yaw'], 7, abs_tol=1e-9) and
          math.isclose(after['pitch']-character['pitch'], -3, abs_tol=1e-9),
          'Actual recorded native Tick did not preserve semantic replay degrees')
    RECORD['checks'].append('Real replay-driven compiled Tick reads attached preferences and receives typed replay mutation rejection; semantic replay degrees stay unchanged.')


def fresh_restore(probe, observer, world):
    client = probe.client
    saves = ARGS.output/'saves'
    configured = client.call('save.configure', {'request_id':fresh(), 'expected_generation':0,
                                              'root':service.native(saves)})
    inspect = client.call('save.inspect', {'slot':SLOT})
    before_payload = live.stored_payload(saves, SLOT)
    probe.session = fresh()
    loaded = client.call('save.load', {'request_id':fresh(), 'configuration_generation':configured['generation'],
        'slot':SLOT, 'expected_generation':inspect['generation'],
        'revision':client.call('world.inspect')['revision'], 'expected_session_id':None,
        'expected_tick':None, 'expected_gameplay_revision':None, 'expected_ui_revision':None,
        'expected_control_sequence':None, 'new_session_id':probe.session, 'gameplay':probe.config})
    check(loaded['session_id'] == probe.session and
          probe.game()['module']['identity'] == IDENTITY, 'Fresh process did not restore actual compiled state')
    saved_ticket = (integer(probe.values(), 'TicketHigh'), integer(probe.values(), 'TicketLow'))
    parameters = service.start_parameters(probe.session, expected_tick=probe.runtime()['tick'],
        camera=uid(10), width=live.WIDTH, height=live.HEIGHT, gpu=ARGS.gpu,
        settings_overrides={'ui.scale':1})
    parameters.pop('controller')
    client.call('player.start', parameters)
    current = live.presented(observer, 0, 'compiled-fresh-process')
    probe.activate('pref.query')
    value = probe.values()
    check(owner(value) != saved_ticket and integer(value, 'ResultState') == 0 and
          integer(value, 'ResultRejection') == 2 and current['settings']['values'] == {
              'ui.scale':1, 'graphics.samples':1, 'graphics.frames_in_flight':1},
          'Fresh-process save imported process-local preference owner/ledger/overrides')
    live.image_oracle(live.capture(client, 'compiled-fresh-process'), 60, 1, 'compiled-fresh-process')
    check(live.stored_payload(saves, SLOT) == before_payload, 'Fresh-process restore rewrote durable slot')
    client.call('player.control', service.control_parameters(client.call('player.inspect'), 'stop'))
    service.await_state(observer, lambda state:not state['active'], 'compiled-fresh-process-stopped')
    RECORD['checks'].append('Fresh process restores real compiled save state with explicit trusted code, starts independent native preferences and rejects the saved old-owner ticket.')


def main():
    global ARGS, RECORD, DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'config', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--audio', action='store_true', help='Numerically verify SDL gain; does not test physical audibility')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--total-timeout', type=int, default=900)
    ARGS = parser.parse_args()
    check(not sys.flags.optimize, 'Do not disable independent image decoder assertions')
    check(ARGS.binary.is_file() and ARGS.config.is_file() and 30 <= ARGS.timeout <= 90 and
          ARGS.timeout <= ARGS.total_timeout <= 1800 and 0 <= ARGS.gpu <= 4095, 'Invalid binary/config/timeouts/GPU')
    if ARGS.audio and not ARGS.capture:
        parser.error('--audio requires --capture')
    if ARGS.capture:
        check(os.name == 'nt' or ARGS.windows_interop, 'Native Windows presentation needs Windows Python or --windows-interop')
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.config = ARGS.config.resolve(strict=True)
    ARGS.output = ARGS.output.resolve()
    check(not ARGS.output.exists(), '--output must be a new directory')
    service.ARGS = ARGS
    config, compiled_pins, image_sha = configuration(ARGS.config)
    for path in compiled_pins:
        check(not Path(path).is_relative_to(ARGS.output) and ARGS.output != Path(path),
              'Output contains a protected compiled input')
    source_paths = [Path(__file__), Path(service.__file__), Path(live.__file__), ROOT/'tests/scene_capture.py',
                    ROOT/'tests/managed_player_preferences_gameplay/PlayerPreferencesMenuGame.cs']
    pins = {str(path):sha(path) for path in source_paths+[ARGS.binary]}
    ARGS.output.mkdir(parents=True)
    RECORD = {'passed':False, 'backend':'native_aot' if 'descriptor' in config else 'coreclr',
        'compiled_pins':compiled_pins, 'source_pins':pins, 'calls':[], 'owners':[], 'checks':[],
        'limitations':['Uses caller-supplied real compiled artifacts; performs no builds or downloads.',
            'Headless mode qualifies absent-owner callbacks only; attached acceptance requires --capture.',
            'No physical input, audibility, event-batch ordering, Jolt error injection, source-free export or performance claim.',
            'Earlier completed physics is checked through rollback after a later compiled callback failure.',
            'Greybox independent pixels do not qualify imported models or a source-free exported game.']}
    service.RECORD = live.RECORD = RECORD
    live.ARGS = ARGS
    DEADLINE = live.DEADLINE = time.monotonic()+ARGS.total_timeout
    world = ARGS.output/'world.json'
    endpoint = 'gameplay-preferences-'+fresh()
    host = client = observer = None
    try:
        host = service.ProcessOwner([str(ARGS.binary), 'serve', service.native(world), '--endpoint', endpoint],
                                    'compiled-preferences-host', endpoint, 'serve')
        host.record['expected_stderr'] = 'Poima shared world ready: '+endpoint+'\n'
        client = Client(endpoint, 'compiled-preferences-client')
        observer = Client(endpoint, 'compiled-preferences-observer')
        service.common_checks(client, observer)
        author(client)
        probe = Probe(client, config, image_sha)
        headless(probe)
        if ARGS.capture:
            graphics(probe, observer, world)
            observer.close(); observer = None
            client.call('host.shutdown')
            client.close(); client = None
            host.close(); host = None
            endpoint = 'gameplay-preferences-fresh-'+fresh()
            host = service.ProcessOwner([str(ARGS.binary), 'serve', service.native(world), '--endpoint', endpoint],
                                        'compiled-preferences-fresh-host', endpoint, 'serve')
            host.record['expected_stderr'] = 'Poima shared world ready: '+endpoint+'\n'
            client = Client(endpoint, 'compiled-preferences-fresh-client')
            observer = Client(endpoint, 'compiled-preferences-fresh-observer')
            fresh_restore(Probe(client, config, image_sha), observer, world)
        for path, expected in {**compiled_pins, **pins}.items():
            check(sha(Path(path)) == expected, 'Qualification input changed '+path)
        observer.close(); observer = None
        check(host.process.poll() is None, 'Observer disconnect killed host')
        client.call('host.shutdown')
        client.close(); client = None
        host.close(); host = None
        for record in RECORD['owners']:
            expected = record.get('expected_stderr', '')
            check(record['exit_code'] == 0 and not record['stderr_truncated'] and record['stderr'] == expected, record)
        RECORD['passed'] = True
    except BaseException:
        RECORD['failure'] = traceback.format_exc()
        raise
    finally:
        failure = sys.exc_info()[0] is not None
        errors = []
        for process in (observer, client, host):
            if process is not None:
                try:
                    process.close(expected=False)
                except BaseException as error:
                    errors.append(repr(error))
        if errors:
            RECORD['passed'] = False
            RECORD['cleanup_errors'] = errors
        RECORD['rpc_count'] = len(RECORD['calls'])
        (ARGS.output/'evidence.json').write_text(json.dumps(RECORD, indent=2)+'\n', encoding='utf-8')
        if errors and not failure:
            raise AssertionError(errors)
    print(json.dumps({'passed':True, 'checks':len(RECORD['checks']), 'rpc_count':RECORD['rpc_count'],
                      'evidence':str(ARGS.output/'evidence.json')}))


if __name__ == '__main__':
    main()
