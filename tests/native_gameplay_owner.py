"""Shared native qualification helpers; no game-specific rules or candidate inputs.

This is a test helper, not a supported engine API. Windows execution must be inside the caller's supervising Job. Constructors can
execute trusted compiled constructors; caller admits immutable inputs/output roots.
This module supplies no process supervisor, retry, deadline extension or oracle.
"""
from pathlib import Path
import copy
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'tools/python'))
from workflow_components_game_service import (Checked, FIELDS, artifact, assigned_job,
    contained, exact, manifest, need, pin, read, tree)
from workflow_save_upgrade import stored
from poima_client.game_session_responses import (game_session_response_contract,
    validate_game_session_result)

STABLE_METHODS = frozenset(game_session_response_contract()['methods'])


def bound(owner, result):
    need(result['session_id'] == owner.session and type(result['tick']) is int
         and result['tick'] == owner.tick, 'Read crossed owner session/tick')
    return result  # Preserve original envelope, every field and native JSON type.


def snapshot_members(payload, registry):
    """Join compact native saved component cells to canonical frozen field IDs."""
    ids = [row['id'] for row in payload['entities']]
    need(len(ids) == len(set(ids)), 'Duplicate snapshot entity membership')
    ids = sorted(ids)  # Saved topology order is retained in payload, not assumed lexical.
    schemas = {row['id']: row for row in registry['schemas']}
    need(len(schemas) == len(registry['schemas']), 'Duplicate registry type')
    saved = payload.get('components', {'revision': 0, 'types': []})
    need(saved['revision'] == registry['component_revision'], 'Saved component revision differs')
    types = saved['types']
    need([row['id'] for row in types] == sorted(schemas), 'Saved/registered schema membership differs')
    values = {}
    for row in types:
        schema = schemas[row['id']]
        need(row['fingerprint'] == schema['fingerprint'], 'Saved component fingerprint differs')
        fields = [field['id'] for field in schema['fields']]
        need(fields == sorted(set(fields)), 'Canonical registry fields not sorted/unique')
        members = [item['entity'] for item in row['instances']]
        need(members == sorted(set(members)) and set(members) <= set(ids),
             'Saved component instance membership is not live/sorted/unique')
        values[row['id']] = {}
        for item in row['instances']:
            need(type(item['values']) is list and len(item['values']) == len(fields),
                 'Compact saved component cells differ from canonical schema')
            values[row['id']][item['entity']] = dict(zip(fields, copy.deepcopy(item['values'])))
    return ids, schemas, values


def trusted_descriptor(path):
    path = manifest(Path(path), 'native-gameplay.json')
    metadata, library_row, _ = artifact(path)
    need(metadata['format'] == 'poima.native-gameplay' and type(metadata['version']) is int
         and metadata['version'] == 2, 'Native gameplay descriptor version differs')
    inventory = tree(path.parent)
    expected = {path.name: pin(path)}
    for row in metadata['files']:
        name = row['path']
        need(name.casefold() not in {key.casefold() for key in expected}, 'Artifact member collision')
        expected[name] = dict(bytes=row['size'], sha256=row['sha256'])
        need(pin(contained(path.parent, name)) == expected[name], 'Declared artifact payload differs')
    need(exact(inventory, expected), 'Artifact closure differs from complete declared membership')
    return path, metadata, library_row['sha256']


class NativeOwner(Checked):
    """Paused/headless source or bundled game owner with retained original wires."""
    def __init__(self, directory, deadline, *, saves, descriptor=None,
                 binary=None, world=None, game=None, initial_values=None,
                 qualified_runtime=None, admitted_execution_pins=None):
        need((world is None) != (game is None), 'Choose exactly source world or bundled game')
        if world is not None:
            need(type(initial_values) is dict, 'Explicit project-bound source initial_values dict required')
            need(type(admitted_execution_pins) is dict, 'Independent source execution pins required')
        assigned_job()
        self.stable_validated = 0
        if game is not None:
            need(type(qualified_runtime) is dict and len(qualified_runtime) == 49,
                 'Explicit qualified49-file runtime-cohort mapping required')
            need(descriptor is None and binary is None and initial_values is None,
                 'Bundle executable/descriptor/values must come from actual manifest')
            game = manifest(Path(game), 'game.json')
            document = read(game)
            tree(game.parent)  # Bounded complete ordinary-file closure; rejects linked members.
            declared = [row['path'] for row in document['files']]
            need(len(declared) == len({name.casefold() for name in declared}), 'Duplicate bundle file')
            for row in document['files']:
                need(pin(contained(game.parent, row['path'])) == dict(bytes=row['size'], sha256=row['sha256']),
                     'Declared bundle file differs')
            runtime_rows = {}
            for row in document['files']:
                if row['role'] == 'runtime':
                    need(row['path'].startswith('runtime/'), 'Runtime role escaped runtime subtree')
                    runtime_rows[row['path'][len('runtime/'):]] = dict(bytes=row['size'], sha256=row['sha256'])
            need(exact(runtime_rows, qualified_runtime)
                 and exact(tree(game.parent/'runtime'), qualified_runtime),
                 'Bundle runtime differs from independent qualified installed runtime')
            selected = document['engine']['executable']
            rows = [row for row in document['files'] if row['path'] == selected]
            need(len(rows) == 1 and rows[0]['role'] == 'runtime', 'Bundled executable not unique runtime')
            need(selected.startswith('runtime/'), 'Bundled engine must belong to admitted runtime subtree')
            binary = contained(game.parent, selected)
            descriptor = contained(game.parent, document['gameplay']['descriptor'])
        else:
            need(descriptor is not None and binary is not None, 'Explicit source binary and trusted descriptor required')
            binary, world, descriptor = Path(binary).resolve(), Path(world).resolve(), Path(descriptor).resolve()
            required_execution = (binary, binary.parent/'phonon.dll')
            for path in required_execution:
                need(str(path) in admitted_execution_pins, 'Native binary/phonon missing independent execution pin')
            for name, expected in admitted_execution_pins.items():
                path = Path(name)
                need(path.is_absolute() and str(path.resolve()) == name
                     and pin(path) == expected, 'Independent source execution identity differs')
            pin(world)
        self.descriptor, self.descriptor_metadata, self.image = trusted_descriptor(Path(descriptor))
        self.descriptor_pin = pin(self.descriptor)
        super().__init__(binary, Path(directory), deadline, world=world, game=game, saves=Path(saves))
        if world is not None:
            revision = self.call('world.inspect')['revision']
            self.session, self.tick = uuid.uuid4().hex, 0
            self.call('runtime.start', dict(session_id=self.session, revision=revision))
            self.call('runtime.gameplay.load_native', dict(session_id=self.session,
                request_id=uuid.uuid4().hex, expected_tick=0, expected_revision=0,
                descriptor=str(self.descriptor), expected_descriptor_sha256=self.descriptor_pin['sha256'],
                values=initial_values))
        self.module()

    def call(self, method, params=None, error=None):
        result = super().call(method, params, error)
        if error is None and method in STABLE_METHODS:
            validate_game_session_result(method, result, {} if params is None else params, self.context)
            self.stable_validated += 1
        return result

    def inspect(self):
        return bound(self, self.call('runtime.inspect', dict(session_id=self.session)))

    def module(self):
        result = bound(self, self.game())
        module = result['module']
        _, library_row, library = artifact(self.descriptor)
        need(pin(self.descriptor) == self.descriptor_pin and module is not None
             and module['backend'] == 'native_aot' and Path(module['native_library']).resolve() == library
             and module['assembly_sha256'] == library_row['sha256'] == self.image
             and module['type'] == self.descriptor_metadata['type']
             and exact(module['schema'], self.descriptor_metadata['schema']),
             'Observed module path/image/type/schema differs from trusted descriptor')
        return result

    def idle(self):
        status = self.call('runtime.save.status', dict(session_id=self.session))
        configuration = self.call('save.status')
        need(status['enabled'] is True and status['pending'] is None
             and status['configuration_generation'] == self.configuration == configuration['generation']
             and configuration['root'] is not None
             and Path(configuration['root']).resolve() == self.expected_save_root,
             'Disabled, pending/resolving or changed save configuration')
        return status

    def guards(self):
        state, game = self.inspect(), self.module()
        registry = bound(self, self.call('runtime.components', dict(session_id=self.session)))
        need(registry['structure_revision'] == state['structure_revision'] == game['structure_revision'],
             'Mutation guards crossed structure revision')
        return dict(expected_session_id=self.session, expected_tick=self.tick,
            expected_gameplay_revision=game['revision'], expected_component_revision=registry['component_revision'],
            **{'expected_'+key:state[key] for key in
               ('structure_revision','ui_revision','control_sequence','material_revision')})

    def step(self, ticks=1, *, allow_replacement=False):
        need(type(ticks) is int and 1 <= ticks <= 600, 'Bounded positive native step required')
        state = self.inspect()
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_structure_revision=state['structure_revision'], ticks=ticks)
        result = self.call('runtime.step', params)
        if result['runtime_replaced']:
            need(allow_replacement and result['current_session_id'] != self.session, 'Unexpected step runtime replacement')
        else:
            need(result['current_session_id'] == self.session and result['current_tick'] == self.tick+ticks,
                 'Step changed owner or tick count')
        self.session, self.tick = result['current_session_id'], result['current_tick']
        if result['runtime_replaced']: self.module()
        return params, result

    def logical_ui(self):
        state = self.inspect()
        rows, pages, cursor = [], [], None
        for _ in range(64):
            params = dict(session_id=self.session, tick=self.tick, ui_revision=state['ui_revision'], limit=64)
            if cursor is not None: params['after'] = cursor
            page = bound(self, self.call('runtime.ui.inspect', params))
            need(page['ui_revision'] == state['ui_revision'] and page['control_sequence'] == state['control_sequence'],
                 'UI page crossed logical revision')
            ids = [row['id'] for row in page['elements']]
            need(ids == sorted(set(ids)) and (not ids or cursor is None or ids[0] > cursor), 'UI page not sorted/exclusive')
            rows.extend(page['elements']); pages.append(page)
            following = page['next_after']
            if following is None: return dict(pages=pages, elements=rows)
            need(ids and following == ids[-1], 'UI continuation cursor differs')
            cursor = following
        raise AssertionError('UI pagination exceeds bounded native selection')

    def control(self, identity, action, *, allow_replacement=False):
        state, game = self.inspect(), self.module()
        selected = [row for row in self.logical_ui()['elements'] if row['id'] == identity]
        need(len(selected) == 1 and selected[0]['kind'] == 'button'
             and selected[0]['action'] == action and selected[0]['eligible'] is True,
             'Caller-selected logical control/action is not eligible')
        params = dict(request_id=uuid.uuid4().hex, session_id=self.session,
            expected_tick=self.tick, expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'], expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'], id=identity)
        result = self.call('runtime.ui.activate', params)
        if result['runtime_replaced']:
            need(allow_replacement and result['current_session_id'] != self.session, 'Unexpected control runtime replacement')
        else:
            need(result['current_session_id'] == self.session and result['current_tick'] == self.tick,
                 'Unticked compiled control advanced runtime')
        self.session, self.tick = result['current_session_id'], result['current_tick']
        if result['runtime_replaced']: self.module()
        return params, result

    def probe(self, slot):
        """Write a fresh explicit caller-owned diagnostic slot; no Tick/callback."""
        self.idle()
        guards = self.guards(); guards['session_id'] = guards.pop('expected_session_id')
        params = dict(request_id=uuid.uuid4().hex, configuration_generation=self.configuration,
            slot=slot, expected_generation=0, **guards)
        receipt = self.call('save.write', params)
        document, path, row = stored(self.expected_save_root, slot)
        need(receipt['generation'] == 1 and not receipt['replayed']
             and receipt['sha256'] == row['sha256'] and receipt['bytes'] == row['bytes']
             and document['snapshot']['payload']['tick'] == self.tick, 'Native probe write/disk boundary differs')
        self.idle()
        return dict(document=document, path=str(path), identity=pin(path), receipt=receipt, params=params)

    def capture(self, probe, *, max_entities=256, max_component_cells=4096):
        """Complete selected RPC view joined to a full unmodified saved payload.

        Complete native snapshot is retained independently; RPC entity fields are
        the public selected fields, not a claim of exposing every internal field.
        """
        payload = probe['document']['snapshot']['payload']
        state, game = self.inspect(), self.module()
        registry = bound(self, self.call('runtime.components', dict(session_id=self.session)))
        need(payload['tick'] == self.tick and payload['gameplay_revision'] == game['revision']
             and payload['control_sequence'] == state['control_sequence']
             and registry['structure_revision'] == state['structure_revision'] == game['structure_revision'],
             'Probe/runtime boundary changed before capture')
        need(exact(payload['gameplay'], {key:game['module'][key] for key in
             ('backend','assembly_sha256','type','schema','values')}), 'Saved/observed complete gameplay payload differs')
        ids, schemas, values = snapshot_members(payload, registry)
        need(len(ids) == state['entities'] and len(ids) <= max_entities
             and len(ids)*len(schemas) <= max_component_cells, 'Complete selection exceeds explicit bounds')
        queries, direct, entities, pages = {}, [], [], []
        for type_id in sorted(schemas):
            found, cursor = [], None
            for _ in range(513):
                params = dict(session_id=self.session, tick=self.tick, type=type_id,
                    structure_revision=registry['structure_revision'], limit=64)
                if cursor is not None: params['after'] = cursor
                page = bound(self, self.call('runtime.component.query', params))
                need(page['component_revision'] == registry['component_revision']
                     and page['structure_revision'] == registry['structure_revision'], 'Query component revision changed')
                batch = page['entities']; need(batch == sorted(set(batch))
                    and (not batch or cursor is None or batch[0] > cursor), 'Query not sorted/exclusive')
                found.extend(batch)
                following = page['next_after']
                if following is None: break
                need(batch and following == batch[-1], 'Query cursor differs'); cursor = following
            else: raise AssertionError('Component pagination did not terminate')
            need(found == sorted(values[type_id]), 'Query lost/added saved instance membership')
            queries[type_id] = found
        for identity in ids:
            entity = bound(self, self.call('runtime.entity', dict(session_id=self.session, tick=self.tick, id=identity)))
            entities.append(entity)
            for type_id in sorted(schemas):
                component = bound(self, self.call('runtime.component.get', dict(session_id=self.session,
                    tick=self.tick, structure_revision=registry['structure_revision'], id=identity, type=type_id)))
                need(component['component_revision'] == registry['component_revision']
                     and component['structure_revision'] == registry['structure_revision']
                     and exact(component['schema'], schemas[type_id])
                     and exact(component['values'], values[type_id].get(identity)), 'Direct component differs from native payload')
                direct.append(component)
        type_ids = sorted(schemas)
        selections = [type_ids[i:i+2] for i in range(0, len(type_ids), 2)] or [[]]
        for start in range(0, len(ids), 32):
            batch = ids[start:start+32]
            for selected in selections:
                observed = bound(self, self.call('runtime.observe', dict(session_id=self.session, tick=self.tick,
                    ids=batch, structure_revision=registry['structure_revision'], component_revision=registry['component_revision'],
                    gameplay_revision=game['revision'], ui_revision=state['ui_revision'], control_sequence=state['control_sequence'],
                    entity_fields=list(FIELDS), components=[dict(type=t) for t in selected], include_schemas=True)))
                need([row['id'] for row in observed['entities']] == batch, 'Joined observation membership differs')
                need(observed['query'] is None and observed['component_revision'] == registry['component_revision']
                     and observed['structure_revision'] == registry['structure_revision']
                     and observed['gameplay_revision'] == game['revision']
                     and observed['ui_revision'] == state['ui_revision']
                     and observed['control_sequence'] == state['control_sequence'], 'Joined observation revision differs')
                for row in observed['entities']:
                    entity = next(item for item in entities if item['id'] == row['id'])
                    need(exact(row['state'], {key:entity[key] for key in FIELDS})
                         and exact(row['components'], {t:values[t].get(row['id']) for t in selected}),
                         'Joined/direct/native compact values differ')
                pages.append(observed)
        ui = self.logical_ui()
        need(exact(self.inspect(), state) and exact(self.module(), game)
             and exact(bound(self, self.call('runtime.components', dict(session_id=self.session))), registry),
             'Capture changed state while reading its complete selected pages')
        return dict(runtime=state, gameplay=game, registry=registry, entities=entities,
            components=direct, queries=queries, observation_pages=pages, ui=ui,
            save_status=self.idle(), probe=copy.deepcopy(probe))

    def close_recorded(self):
        """Local observed exit metadata, to be joined to actual enclosing Job."""
        self.idle()  # Never label shutdown-abandonment of pending work a completed owner.
        self.close()
        need(self.transport.returncode == 0 and (self.host is None or self.host.returncode == 0),
             'Native owner did not report normal exit')
        return dict(calls=len(self.calls), candidate_validated=self.validated,
            stable19_selected_validated=self.stable_validated, returncode=self.transport.returncode,
            host_returncode=None if self.host is None else self.host.returncode,
            transcript=str(self.directory/'transcript.json'),
            scope='Observed local exits only; supervising Job receipt required for natural descendant closure')
