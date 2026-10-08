#!/usr/bin/env python3
"""Independently replay the recorded Workshop Relay fixture.

Uses compiled gameplay, generated native buffers and newly computed controller
routes. Owns two standalone processes and edits only copies of the input worlds.
No model/provider calls, source repairs, runtime field edits or teleports.
Run Python on the engine's native operating system. Local evidence includes
paths and RPCs; public-summary.json contains only allowlisted results.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient, RpcError

MAX_CALLS = 8000
ALLOWED = frozenset(('runtime.status', 'runtime.stop', 'world.inspect', 'world.ui.layout',
    'runtime.start', 'runtime.gameplay.load', 'runtime.gameplay.inspect', 'runtime.inspect',
    'runtime.entity', 'runtime.components', 'runtime.component.get', 'runtime.component.query',
    'runtime.step', 'runtime.ui.inspect', 'runtime.ui.activate', 'runtime.raycast',
    'save.status', 'save.configure', 'save.inspect', 'save.load'))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    with Path(path).open('rb') as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(65536), b''):
            result.update(chunk)
    return result.hexdigest()


def position(entity):
    return entity['world_matrix'][12:15]


def distance(a, b):
    return math.sqrt(sum((x-y)**2 for x, y in zip(a, b)))


def raw_type(value):
    require(isinstance(value, str) and value.startswith('game:'), 'Expected native custom component type')
    result = value[5:]
    require(re.fullmatch('[0-9a-f]{32}', result), 'Malformed native component type')
    return result


class Game:
    def __init__(self, client, settings, manifest, evidence, name, deadline):
        self.client, self.s, self.m, self.e, self.name = client, settings, manifest, evidence, name
        self.deadline = deadline
        self.session, self.tick = None, 0
        self.validated_session = None

    def rpc(self, method, params=None):
        require(method in ALLOWED, 'Forbidden verifier operation: '+method)
        require(len(self.e['calls']) < MAX_CALLS, 'Independent RPC budget exceeded')
        remaining = self.deadline-time.monotonic()
        require(remaining > 0, 'Independent overall deadline exceeded')
        record = dict(process=self.name, method=method, params=copy.deepcopy(params or {}))
        self.e['calls'].append(record)
        try:
            result = self.client.call(method, params, timeout=min(30, remaining))
            record['result'] = copy.deepcopy(result)
            return result
        except BaseException as error:
            record['error'] = dict(kind=type(error).__name__, message=str(error), code=getattr(error, 'code', None))
            raise

    def inspect(self):
        result = self.rpc('runtime.inspect', dict(session_id=self.session))
        require(result['tick'] == self.tick, 'Runtime advanced outside explicit verifier steps')
        return result

    def gameplay(self):
        params = dict(session_id=self.session, tick=self.tick)
        if self.validated_session != self.session:
            params['include_schema'] = True
        result = self.rpc('runtime.gameplay.inspect', params)
        module = result['module']
        if params.get('include_schema'):
            require(module['backend'] == 'coreclr', 'Workshop must execute compiled CoreCLR gameplay')
            self.validated_session = self.session
        require(module['assembly_sha256'] == self.e['hashes']['assembly'], 'Runtime artifact hash differs')
        return result

    def values(self):
        return self.gameplay()['module']['values']

    def n(self, values, field):
        value = values[self.m['fields'][field]]
        require(type(value) is int or (isinstance(value, str) and value.lstrip('-').isdigit()), 'Invalid state integer '+field)
        return int(value)

    def entity(self, identity):
        state = self.inspect()
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick,
            structure_revision=state['structure_revision'], id=identity))

    def component(self, identity, type):
        state = self.inspect()
        return self.rpc('runtime.component.get', dict(session_id=self.session, tick=self.tick,
            structure_revision=state['structure_revision'], id=identity, type=raw_type(type)))

    def inventory(self):
        result = self.component(self.m['player'], self.m['inventory']['type'])
        schema = result['schema']
        require(schema['version'] == 2, 'Inventory must use generated version-2 collection schema')
        field = next((f for f in schema['fields'] if f['id'] == self.m['inventory']['field']), None)
        require(field and field['kind'] == 'array' and field['element_kind'] == 'int32' and field['capacity'] == 3,
                'Inventory is not a native capacity-three int32 buffer')
        require(result['values'] is not None, 'Player lacks generated inventory membership')
        items = result['values'][self.m['inventory']['field']]
        require(isinstance(items, list) and len(items) <= 3 and all(type(x) is int and x in (1, 2) for x in items),
                'Invalid native inventory list')
        return items

    def pickups(self):
        state = self.inspect()
        query = self.rpc('runtime.component.query', dict(session_id=self.session, tick=self.tick,
            structure_revision=state['structure_revision'], type=raw_type(self.m['pickups']['type']), limit=256))
        require(query['next_after'] is None and len(query['entities']) <= 7, 'Pickup query exceeded bounded native set')
        result = {}
        for identity in query['entities']:
            component = self.component(identity, self.m['pickups']['type'])
            field = next((f for f in component['schema']['fields'] if f['id'] == self.m['pickups']['kind_field']), None)
            require(field and field['kind'] == 'int32', 'Pickup kind is not native scalar int32')
            require(component['values'] is not None, 'Query returned absent pickup membership')
            kind = component['values'][self.m['pickups']['kind_field']]
            require(type(kind) is int and kind in (1, 2), 'Native pickup kind is invalid')
            body = self.entity(identity)
            require(body['has_body'], 'Pickup has no native collision body')
            result[identity] = dict(kind=kind, entity=body)
        return result

    def ui_state(self):
        result = self.rpc('runtime.ui.inspect', dict(session_id=self.session, tick=self.tick))
        require(result['next_after'] is None, 'Logical UI exceeds bounded qualification page')
        return result

    def hud(self):
        rows = {row['id']: row for row in self.ui_state()['elements']}
        for key in ('inventory', 'progress', 'message'):
            row = rows.get(self.m['ui'][key])
            require(row and row['kind'] == 'label' and row['effective_visible'] and isinstance(row['text'], str),
                    'Missing visible logical HUD label '+key)
        text = rows[self.m['ui']['inventory']]['text']
        match = re.fullmatch(r'PACK \[\s*([AB ]*)\s*\]\s*([0-3])/3', text)
        require(match is not None, 'Observed Workshop inventory HUD format changed: '+text)
        expected = ['A' if n == 1 else 'B' for n in self.inventory()]
        require(match.group(1).split() == expected and int(match.group(2)) == len(expected),
                'HUD inventory order or capacity differs from native buffer')
        values = self.values()
        progress = rows[self.m['ui']['progress']]['text']
        require(('2/2' if self.n(values, 'won') else str(self.n(values, 'delivered'))+'/2') in progress,
                'HUD progression differs from actual deliveries')
        return {key: rows[self.m['ui'][key]]['text'] for key in ('inventory', 'progress', 'message')}

    def step(self, ticks=1, **input):
        require(1 <= ticks <= 120, 'Unsafe verifier tick batch')
        state = self.inspect()
        if input:
            input['entity'] = self.m['player']
        previous = self.tick
        result = self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_structure_revision=state['structure_revision'], ticks=ticks,
            inputs=[input] if input else []))
        require(not result.get('runtime_replaced'), 'Ordinary input unexpectedly replaced runtime')
        self.session, self.tick = result['current_session_id'], result['current_tick']
        require(self.tick == previous+ticks, 'Native step did not advance exact tick count')
        return result

    def ui(self, name):
        current = {r['id']: r for r in self.ui_state()['elements']}
        require(current[self.m['ui'][name]]['eligible'], 'Compiled UI control not eligible: '+name)
        state, game = self.inspect(), self.gameplay()
        result = self.rpc('runtime.ui.activate', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'], expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], id=self.m['ui'][name]))
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def pause(self):
        if self.n(self.values(), 'paused'):
            return
        tick = self.tick
        result = self.ui('pause')
        require(result['intent'] == 2 and self.tick == tick and self.n(self.values(), 'paused') == 1,
                'Compiled Pause does not preserve tick and request owner pause')
        require(self.ui_state()['modal'] == self.m['ui']['menu'], 'Paused menu is not modal')

    def resume(self):
        if not self.n(self.values(), 'paused'):
            return
        tick = self.tick
        result = self.ui('resume')
        require(result['intent'] == 1 and self.tick == tick and self.n(self.values(), 'paused') == 0,
                'Compiled Resume does not preserve tick and request owner resume')
        require(self.ui_state()['modal'] is None, 'Resume left modal menu active')

    def ray(self, maximum=None):
        camera = self.entity(self.m['camera'])
        matrix = camera['world_matrix']
        return self.rpc('runtime.raycast', dict(session_id=self.session, tick=self.tick,
            origin=position(camera), direction=[-matrix[8], -matrix[9], -matrix[10]],
            distance=self.m['interaction_range'] if maximum is None else maximum, ignore=[self.m['player'], self.m['camera']]))['hit']

    def look(self, yaw, pitch):
        player = self.entity(self.m['player'])
        self.step(1, look=[(yaw-player['yaw']+180) % 360-180,
                           max(-180, min(180, pitch-player['pitch']))])

    def aim(self, identity):
        for _ in range(2):
            camera, target = self.entity(self.m['camera']), self.entity(identity)
            dx, dy, dz = [b-a for a, b in zip(position(camera), position(target))]
            self.look(math.degrees(math.atan2(-dx, -dz)), math.degrees(math.atan2(dy, math.hypot(dx, dz))))
        return self.ray()

    def reach(self, identity):
        # Routes derive only from actual current native poses; no author plan.
        stalled = 0
        for index in range(48):
            player, target = self.entity(self.m['player']), self.entity(identity)
            a, b = position(player), position(target)
            dx, dz = b[0]-a[0], b[2]-a[2]
            horizontal = math.hypot(dx, dz)
            if horizontal <= 2.05:
                hit = self.aim(identity)
                if hit is not None and hit['entity'] == identity:
                    return hit
                self.step(6, move=[1 if index % 2 == 0 else -1, 0])
                continue
            self.look(math.degrees(math.atan2(-dx, -dz)), 0)
            ticks = max(2, min(12, int((horizontal-1.6)*10)))
            before = position(self.entity(self.m['player']))
            self.step(ticks, move=[0, 1])
            after = position(self.entity(self.m['player']))
            stalled = stalled+1 if distance(before, after) < .05 else 0
            if stalled:
                self.step(8, move=[1 if stalled % 2 else -1, 0])
        raise AssertionError('Native controller could not reach/raycast target within bounded route: '+identity)

    def conservation(self):
        items, pickups, values = self.inventory(), self.pickups(), self.values()
        delivered = self.n(values, 'delivered')
        require(delivered in (0, 1, 2), 'Invalid delivery progress')
        consumed = {1: 0, 2: 0}
        for recipe in self.m['recipes'][:delivered]:
            for kind in recipe:
                consumed[kind] += 1
        for kind in (1, 2):
            require(items.count(kind)+sum(p['kind'] == kind for p in pickups.values())+consumed[kind] == 3,
                    'Resource conservation violated for native pickup kind '+str(kind))
        self.hud()
        return dict(inventory=items, pickups=pickups, values=values)

    def resource_state(self):
        state = self.conservation()
        return dict(inventory=state['inventory'], pickups={i:dict(kind=p['kind'], matrix=p['entity']['world_matrix'])
                    for i,p in state['pickups'].items()}, delivered=self.n(state['values'], 'delivered'),
                    collected=self.n(state['values'], 'collected'), dropped=self.n(state['values'], 'dropped'),
                    won=self.n(state['values'], 'won'))

    def collect(self, kind):
        candidates = self.pickups()
        candidates = {i:p for i,p in candidates.items() if p['kind'] == kind}
        require(candidates, 'No remaining native pickup of requested kind')
        player = position(self.entity(self.m['player']))
        identity = min(candidates, key=lambda i: distance(position(candidates[i]['entity']), player))
        self.reach(identity)
        before = self.resource_state()
        self.step(1, use=True)
        after = self.resource_state()
        require(after['inventory'] == before['inventory']+[kind], 'Collect did not append ordered native kind')
        require(set(after['pickups']) == set(before['pickups'])-{identity}, 'Collect did not despawn exact ray hit')
        require(after['collected'] == before['collected']+1, 'Collected counter did not increment once')
        require(after['delivered'] == before['delivered'] and after['dropped'] == before['dropped'], 'Collect changed unrelated counters')
        self.e['route'].append(dict(process=self.name, event='collect', id=identity, kind=kind, tick=self.tick))
        return identity

    def reject_target(self, identity, reason):
        self.reach(identity)
        before, rejected = self.resource_state(), self.n(self.values(), 'rejected')
        self.step(1, use=True)
        require(self.resource_state() == before, 'Denied '+reason+' changed inventory, native props or counters')
        require(self.n(self.values(), 'rejected') == rejected+1, 'Eligible denied '+reason+' did not increment Rejected once')
        self.e['checks'].append(reason+' preserved exact inventory/native pickup set and poses.')

    def miss_and_range(self):
        before, rejected = self.resource_state(), self.n(self.values(), 'rejected')
        original_yaw = self.entity(self.m['player'])['yaw']
        for offset in (0, 90, 180, -90):
            self.look(original_yaw+offset, 80)
            if self.ray() is None:
                break
        else:
            raise AssertionError('Bounded genuine native sky-ray miss was not found')
        self.step(1, use=True)
        require(self.resource_state() == before and self.n(self.values(), 'rejected') == rejected,
                'Genuine ray miss changed resources or eligible-rejection counter')
        self.e['checks'].append('Genuine native camera-ray miss Use preserves resources/counters.')
        for identity in self.pickups():
            camera, target = self.entity(self.m['camera']), self.entity(identity)
            if distance(position(camera), position(target)) <= 3.6:
                continue
            short = self.aim(identity)
            far = self.ray(20)
            if short is None and far is not None and far['entity'] == identity and far['distance'] > 3:
                before, rejected = self.resource_state(), self.n(self.values(), 'rejected')
                self.step(1, use=True)
                require(self.resource_state() == before and self.n(self.values(), 'rejected') == rejected,
                        'Out-of-range aimed native pickup Use changed resources/counters')
                self.e['out_of_range'] = dict(id=identity, native_hit=far, tick=self.tick)
                self.e['checks'].append('Actual aimed native pickup beyond three meters cannot be collected.')
                return
        raise AssertionError('Bounded native out-of-range target probe found no unobstructed distant pickup')

    def open_drop_direction(self):
        # Fixed-placement game logic does not validate walls/overlap. Choose a
        # genuine open-space controller view for this main-route drop test.
        player = self.entity(self.m['player'])
        initial_yaw = player['yaw']
        pickups = self.pickups()
        require(pickups, 'Open-space placement probe requires actual native pickup height')
        height = position(next(iter(pickups.values()))['entity'])[1]
        for offset in (180, 90, -90, 0):
            self.look(initial_yaw+offset, 0)
            player = self.entity(self.m['player'])
            yaw = math.radians(player['yaw'])
            origin = position(player)
            origin[1] = height
            hit = self.rpc('runtime.raycast', dict(session_id=self.session,tick=self.tick,origin=origin,
                direction=[-math.sin(yaw),0,-math.cos(yaw)],distance=1.6,
                ignore=[self.m['player'],self.m['camera']]))['hit']
            if hit is None:
                self.e['drop_open_space_probe'] = dict(tick=self.tick,origin=origin,yaw=player['yaw'],distance=1.6)
                return
        raise AssertionError('Bounded native open-space drop direction search failed')

    def drop(self):
        self.open_drop_direction()
        before, tick = self.resource_state(), self.tick
        require(before['inventory'], 'Drop requires nonempty inventory')
        dropped_kind = before['inventory'][-1]
        result = self.ui('drop')
        require(self.tick == tick and self.resource_state() == before and self.n(self.values(), 'drop_pending') == 1,
                'Compiled Control changed native inventory/props before next tick')
        self.step(1)
        after = self.resource_state()
        require(after['inventory'] == before['inventory'][:-1], 'Deferred drop did not remove only LAST item')
        added = set(after['pickups'])-set(before['pickups'])
        require(len(added) == 1 and set(before['pickups']).issubset(after['pickups']), 'Drop native spawn count differs')
        identity = next(iter(added))
        require(after['pickups'][identity]['kind'] == dropped_kind, 'Dropped native pickup kind differs')
        require(after['dropped'] == before['dropped']+1 and after['collected'] == before['collected'] and
                self.n(self.values(), 'drop_pending') == 0, 'Deferred drop counters/intent differ')
        # Genuine closest collider hit from a separately computed camera look.
        hit = self.aim(identity)
        require(hit is not None and hit['entity'] == identity, 'Dropped item is not immediately reachable within three meters')
        self.e['deferred_drop'] = dict(control=result, before=before, after=after, spawned=identity)
        return identity

    def deliver(self, station):
        self.reach(self.m['stations'][station])
        before = self.resource_state()
        target = 0 if station == 'first' else 1
        require(sorted(before['inventory']) == sorted(self.m['recipes'][target]), 'Verifier is not carrying actual expected recipe')
        self.step(1, use=True)
        after = self.resource_state()
        require(after['inventory'] == [] and after['pickups'] == before['pickups'] and
                after['delivered'] == before['delivered']+1, 'Correct delivery did not consume only native inventory')
        require(after['collected'] == before['collected'] and after['dropped'] == before['dropped'], 'Delivery altered unrelated counters')
        require(after['won'] == int(station == 'second'), 'Victory does not occur exactly on second delivery')
        self.e['route'].append(dict(process=self.name, event='deliver', station=station, tick=self.tick))

    def snapshot(self):
        state, game = self.inspect(), self.gameplay()
        pickups = self.pickups()
        ids = [self.m['player'], self.m['camera'], self.m['stations']['first'], self.m['stations']['second'], *pickups]
        return dict(tick=self.tick, state=state, values=game['module']['values'], gameplay_revision=game['revision'],
            component_revision=self.component(self.m['player'],self.m['inventory']['type'])['component_revision'],
            inventory=self.inventory(), pickups=pickups, entities={i:self.entity(i) for i in ids}, ui=self.ui_state())

    def restore_equals(self, saved):
        observed = self.snapshot()
        for key in ('tick', 'values', 'inventory', 'gameplay_revision', 'component_revision'):
            require(observed[key] == saved[key], 'Checkpoint '+key+' differs before any resumed callback')
        require(set(observed['pickups']) == set(saved['pickups']), 'Checkpoint native pickup identities differ')
        require(set(observed['entities']) == set(saved['entities']), 'Checkpoint native entity set differs')
        for identity, entity in saved['entities'].items():
            for key in ('world_matrix', 'motion', 'kinematic_target', 'motion_remaining_ticks', 'yaw', 'pitch', 'velocity'):
                if key in entity:
                    require(observed['entities'][identity].get(key) == entity[key], 'Checkpoint native '+key+' differs: '+identity)
        for identity in saved['pickups']:
            require(observed['pickups'][identity]['kind'] == saved['pickups'][identity]['kind'], 'Checkpoint pickup kind differs')
        for key in ('ui_revision', 'control_sequence', 'structure_revision', 'component_revision'):
            if key in saved['state']:
                require(observed['state'].get(key) == saved['state'][key], 'Checkpoint counter differs: '+key)
        for key in ('elements', 'modal'):
            require(observed['ui'][key] == saved['ui'][key], 'Checkpoint complete logical UI differs: '+key)
        return observed

    def finish(self):
        self.resume()
        require(self.inventory() == [1, 2] and self.n(self.values(), 'delivered') == 1,
                'Continuation does not start from real nonempty [A,B] checkpoint')
        self.collect(2)
        require(self.inventory() == [1, 2, 2], 'Second recipe is not native ordered ABB')
        self.deliver('second')
        terminal = self.resource_state()
        values = self.values()
        require(terminal['won'] == 1 and terminal['delivered'] == 2 and terminal['collected'] == 7 and
                terminal['dropped'] == 1 and terminal['inventory'] == [] and not terminal['pickups'],
                'Terminal win accounting does not equal six initial resources, seven collects, one drop')
        self.step(24)
        require(self.resource_state() == terminal, 'Terminal neutral ticks changed resources/progression')
        self.reach(self.m['stations']['second'])
        self.step(1, use=True)
        require(self.resource_state() == terminal, 'Terminal Use changed resources/progression')
        rows = {r['id']:r for r in self.ui_state()['elements']}
        if rows[self.m['ui']['drop']]['eligible']:
            self.ui('drop')
            self.step(1)
            require(self.resource_state() == terminal and self.n(self.values(), 'drop_pending') == 0,
                    'Terminal compiled Drop changed resources or queued intent')
        require(self.n(self.values(), 'rejected') == self.n(values, 'rejected'), 'Terminal inputs incremented rejection counter')
        return dict(tick=self.tick, values=self.values(), inventory=self.inventory(), pickups=self.pickups(), hud=self.hud())

    def fresh(self):
        current = self.rpc('runtime.status')
        if current['active']:
            self.rpc('runtime.stop', dict(session_id=current['session_id']))
        revision = self.rpc('world.inspect')['revision']
        self.session = uuid.uuid4().hex
        self.rpc('runtime.start', dict(session_id=self.session, revision=revision))
        self.rpc('runtime.gameplay.load', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=0, expected_revision=0, hostfxr=self.s['hostfxr'], bridge=self.s['bridge'],
            assembly=self.s['assembly'], type=self.m['type']))
        self.step(1)
        state = self.conservation()
        require(state['inventory'] == [] and len(state['pickups']) == 6 and
                all(self.n(state['values'], k) == 0 for k in self.m['fields']), 'Fresh initial compiled state differs')
        return state


def manifest_check(m):
    require(m['format'] == 'poima.agent-workshop' and m['version'] == 1, 'Workshop manifest format/version differs')
    require(m['inventory']['capacity'] == 3 and m['inventory']['element_kind'] == 'int32', 'Manifest buffer contract differs')
    require(m['recipes'] == [[1,1,2],[1,2,2]] and m['interaction_range'] == 3, 'Manifest recipe/range differs')
    require(m['pickups']['initial_counts'] == {'1':3,'2':3}, 'Manifest initial resource counts differ')
    require(set(m['fields']) == {'delivered','won','paused','drop_pending','collected','dropped','rejected'}, 'Manifest scalar field mapping differs')
    require(set(m['ui']) == {'inventory','progress','message','drop','pause','resume','save','load','menu'}, 'Manifest UI mapping differs')
    identities = [m['player'], m['camera'], *m['stations'].values(), *m['ui'].values(),
                  *m['pickups']['templates'].values(), m['inventory']['field'], m['pickups']['kind_field']]
    require(all(isinstance(x,str) and re.fullmatch('[0-9a-f]{32}',x) for x in identities), 'Manifest identity is not native 32hex')
    raw_type(m['inventory']['type']); raw_type(m['pickups']['type'])
    require(len(set(m['ui'].values())) == len(m['ui']), 'UI role identities duplicated')


def layout_check(game):
    revision = game.rpc('world.inspect')['revision']
    result = []
    for width,height in ((960,540),(1280,720)):
        layout = game.rpc('world.ui.layout', dict(revision=revision,width=width,height=height,scale=1))
        rows = {r['id']:r for r in layout['controls']}
        visible = []
        for key in ('inventory','progress','message','drop','pause'):
            row = rows[game.m['ui'][key]]
            require(row['visible'], 'Authored main HUD unexpectedly hidden: '+key)
            x0,y0,x1,y1 = row['bounds']
            require(0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height, 'Authored HUD bounds empty/offscreen: '+key)
            visible.append((key,row['bounds']))
        for index,(left,a) in enumerate(visible):
            for right,b in visible[index+1:]:
                overlap = max(0,min(a[2],b[2])-max(a[0],b[0]))*max(0,min(a[3],b[3])-max(a[1],b[1]))
                require(overlap < 1, 'Meaningful authored HUD overlap: '+left+'/'+right)
        result.append(dict(width=width,height=height,controls={key:rows[game.m['ui'][key]] for key in game.m['ui'] if game.m['ui'][key] in rows}))
    return result


def copy_world(source, target):
    shutil.copyfile(source, target)
    assets = Path(str(source)+'.assets')
    if assets.exists():
        shutil.copytree(assets, Path(str(target)+'.assets'))


def asset_inventory(source):
    assets = Path(str(source)+'.assets')
    if not assets.exists():
        return {}
    require(assets.is_dir(), 'World assets path must be a directory')
    return {str(path.relative_to(assets)):digest(path) for path in sorted(assets.rglob('*')) if path.is_file()}


def summary(evidence, manifest):
    outcomes = {}
    for name in ('win','win_after_compiled_load','fresh_process_win'):
        if name in evidence:
            outcome = evidence[name]
            outcomes[name] = dict(tick=outcome['tick'],inventory=outcome['inventory'],
                pickup_count=len(outcome['pickups']),
                counters={key:int(outcome['values'][field]) for key,field in manifest['fields'].items()})
    return dict(format='poima.agent-workshop-replay',version=1,passed=evidence['passed'],
        calls=len(evidence['calls']),elapsed_seconds=evidence['elapsed_seconds'],
        hashes=evidence.get('hashes',{}),checks=evidence['checks'],limitations=evidence['limitations'],
        checkpoint_inventory=evidence.get('checkpoint',{}).get('inventory'),outcomes=outcomes,
        owned_processes=len(evidence['processes']),
        cleanup_ok=not evidence['cleanup_errors'] and all(p['closed'] and p['returncode'] == 0 for p in evidence['processes']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine','world','manifest','assembly','hostfxr','bridge','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--source',type=Path,action='append',default=[],
                        help='Optional retained source/project file to hash and check unchanged; repeatable')
    parser.add_argument('--timeout',type=float,default=180)
    args = parser.parse_args()
    require(not sys.flags.optimize and 30 <= args.timeout <= 300,
            'Run without optimization and within bounded timeout')
    inputs = {name:getattr(args,name).resolve() for name in ('engine','world','manifest','assembly','hostfxr','bridge')}
    require(all(path.is_file() for path in inputs.values()), 'All input paths must be actual files')
    sources = [path.resolve() for path in args.source]
    require(all(path.is_file() for path in sources), 'Optional source paths must be actual files')
    manifest = json.loads(inputs['manifest'].read_text(encoding='utf-8'))
    manifest_check(manifest)
    directory = args.output.resolve()
    require(not directory.exists(), 'Output must be a new directory')
    asset_root = Path(str(inputs['world'])+'.assets').resolve()
    require(asset_root not in (directory,*directory.parents), 'Output must be outside source world assets')
    directory.mkdir(parents=True)
    saves = directory/'saves'
    saves.mkdir()
    settings = {key:str(inputs[key]) for key in ('world','assembly','hostfxr','bridge')}
    settings['binary'] = str(inputs['engine'])
    evidence = dict(passed=False,calls=[],checks=[],route=[],cleanup_errors=[],processes=[],
        platform=sys.platform,runner_sha256=digest(__file__),started_unix=time.time(),deadline_seconds=args.timeout,
        input_paths={name:str(path) for name,path in inputs.items()},
        limitations=['Synthetic native controller and logical UI; no physical input or GPU/render qualification.',
          'world.ui.layout observes authored virtual geometry, not paused live presentation.',
          'Fresh owned process restoration on the tested machine, not an independently provisioned clean machine.',
          'The retained game uses fixed forward Drop placement without wall/overlap validation; this verifies one ray-probed open-space placement only.',
          'The retained Save label reports accepted queuing before durable completion; this test checks the actual completed save operation and stored generation.',
          'True interaction occlusion is not independently qualified by this main route; closest-hit/range probes are recorded.',
          'CoreCLR gameplay requires the supplied hostfxr/bridge; this does not qualify NativeAOT or a relocated shipping bundle.'])
    clients = []
    deadline = time.monotonic()+args.timeout
    try:
        evidence['hashes'] = {name:digest(path) for name,path in inputs.items()}
        evidence['hashes']['runner'] = evidence['runner_sha256']
        evidence['source_hashes'] = {str(path):digest(path) for path in sources}
        evidence['asset_hashes'] = asset_inventory(inputs['world'])
        original = json.loads(inputs['world'].read_text(encoding='utf-8'))
        # Native schemas/templates must support genuine compiled Spawn, not a
        # pre-authored substitute for runtime pickup membership.
        inv = original['component_schemas'][raw_type(manifest['inventory']['type'])]
        require(inv['version'] == 2, 'Authored generated inventory schema version differs')
        for kind in ('1','2'):
            template = original['templates'][manifest['pickups']['templates'][kind]]
            require(template['components'][manifest['pickups']['type']][manifest['pickups']['kind_field']] == int(kind),
                    'Actual native pickup template kind differs')
        require(all(manifest['pickups']['type'] not in entity['components'] for entity in original['entities'].values()),
                'Pickups must originate from compiled native Spawn, not authored substitutes')
        first_world = directory/'first-world.json'
        copy_world(inputs['world'],first_world)
        require(digest(first_world) == evidence['hashes']['world'], 'First owned world copy differs')
        client = WorldClient.open(settings['binary'],first_world,close_timeout=10)
        clients.append(client)
        game = Game(client,settings,manifest,evidence,'first-owned-native-process',deadline)
        evidence['authored_layout'] = layout_check(game)
        evidence['fresh'] = game.fresh()
        game.miss_and_range()
        game.collect(1);game.collect(1);game.collect(1)
        require(game.inventory() == [1,1,1], 'Actual native buffer did not hold AAA')
        b = next(i for i,p in game.pickups().items() if p['kind'] == 2)
        game.reject_target(b,'Full-capacity pickup rejection')
        game.reject_target(manifest['stations']['first'],'Wrong AAA recipe rejection')
        game.reject_target(manifest['stations']['second'],'Locked second station rejection')
        game.drop()
        game.collect(2)
        require(game.inventory() == [1,1,2], 'First native recipe is not AAB after deferred LAST drop')
        game.deliver('first')
        game.collect(1);game.collect(2)
        require(game.inventory() == [1,2] and game.n(game.values(),'delivered') == 1,
                'Checkpoint is not delivered-one/nonempty AB')
        generation = game.rpc('save.status')['generation']
        game.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=generation,root=str(saves)))
        game.pause()
        saved = game.ui('save')
        require(saved.get('save_serviced') and saved['save_operation']['state'] == 3 and saved['save_operation']['kind'] == 1,
                'Actual compiled Save operation did not complete')
        checkpoint = game.snapshot()
        require(checkpoint['inventory'] == [1,2] and game.n(checkpoint['values'],'paused') == 1,
                'Saved checkpoint lacks real ordered AB and paused modal state')
        slot = game.rpc('save.inspect',dict(slot=manifest['save_slot']))
        require(slot['generation'] == 1 and slot['selected'] is not None, 'Independent saved generation not durable')
        evidence['checkpoint'],evidence['save'] = checkpoint,saved
        evidence['win'] = game.finish()
        game.pause()
        old_session = game.session
        loaded = game.ui('load')
        require(loaded.get('runtime_replaced') and loaded['save_operation']['state'] == 3 and
                loaded['save_operation']['kind'] == 2 and game.session != old_session,
                'Actual compiled Load did not replace runtime from durable checkpoint')
        evidence['compiled_restore'] = game.restore_equals(checkpoint)
        evidence['win_after_compiled_load'] = game.finish()

        fresh_world = directory/'fresh-world.json'
        copy_world(inputs['world'],fresh_world)
        require(digest(fresh_world) == evidence['hashes']['world'], 'Second owned world copy differs')
        fresh_client = WorldClient.open(settings['binary'],fresh_world,close_timeout=10)
        clients.append(fresh_client)
        fresh = Game(fresh_client,settings,manifest,evidence,'fresh-owned-native-process',deadline)
        require(not fresh.rpc('runtime.status')['active'], 'Fresh process unexpectedly initialized runtime')
        configuration = fresh.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(saves)))
        revision = fresh.rpc('world.inspect')['revision']
        selection = {key:settings[key] for key in ('hostfxr','bridge','assembly')}
        selection['type'] = manifest['type']
        restored = fresh.rpc('save.load',dict(request_id=uuid.uuid4().hex,
            configuration_generation=configuration['generation'],slot=manifest['save_slot'],expected_generation=1,
            revision=revision,expected_session_id=None,expected_tick=None,expected_gameplay_revision=None,
            new_session_id=uuid.uuid4().hex,gameplay=selection))
        fresh.session,fresh.tick = restored['session_id'],restored['tick']
        evidence['fresh_process_restore'] = fresh.restore_equals(checkpoint)
        evidence['fresh_process_win'] = fresh.finish()
        require(all(digest(path) == evidence['hashes'][name] for name,path in inputs.items()),
                'Replay changed original fixture/tool/artifact bytes')
        require(all(digest(path) == sha for path,sha in evidence['source_hashes'].items()),
                'Retained source/project changed during replay')
        require(asset_inventory(inputs['world']) == evidence['asset_hashes'], 'Original fixture assets changed during replay')
        require(all(digest(path) == evidence['hashes']['world'] for path in (first_world,fresh_world)),
                'Replay changed copied authored world bytes')
        evidence['checks'].extend(['Generated native capacity-three int32 buffer and six compiled-spawned kind-tagged collision props.',
            'AAA capacity rejection, wrong recipe and locked station preserve exact resources.',
            'Compiled Drop Control only queues; next Tick removes LAST and spawns a reachable matching-kind native prop in the tested open-space placement.',
            'Per-kind conservation through two exact deliveries, seven successful collects and one completed drop.',
            'Pause/Resume owner intents at unchanged ticks and durable compiled Save/Load.',
            'Full reflected state, ordered buffer, native identities/poses, runtime revisions and logical modal UI restore before callback.',
            'Fresh native process loads without Initialize and independently completes second recipe.',
            'Terminal neutral/Use/Drop inputs preserve victory/resources.'])
        evidence['work_complete'] = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        for client in reversed(clients):
            transport = client.transport
            try:
                client.close()
            except BaseException:
                evidence['cleanup_errors'].append(traceback.format_exc())
            evidence['processes'].append(dict(pid=transport.process_id,closed=transport.closed,returncode=transport.returncode))
        evidence['finished_unix'] = time.time()
        evidence['elapsed_seconds'] = evidence['finished_unix']-evidence['started_unix']
        evidence['passed'] = bool(evidence.get('work_complete') and not evidence.get('error') and not evidence['cleanup_errors']
            and len(evidence['processes']) == 2 and all(p['closed'] and p['returncode'] == 0 for p in evidence['processes']))
        (directory/'local-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
        public = summary(evidence,manifest)
        (directory/'public-summary.json').write_text(json.dumps(public,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(dict(passed=evidence['passed'],calls=len(evidence['calls']),elapsed_seconds=evidence['elapsed_seconds'],
            evidence=str(directory/'local-evidence.json'),error=evidence.get('error'))))
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
