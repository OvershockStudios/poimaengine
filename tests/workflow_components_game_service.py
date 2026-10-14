#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a compiled ComponentGame source and immutable readonly Windows game.

Run inside a supervising Windows Job, for example switchback_ownership.gated.
No publishing, compilation, rendering, physical input or ABI release is implied.
--source/--project selects an adopted project; --game selects its exported game.
The original inputs remain untouched; only an owned source copy is opened.
"""
import argparse
import copy
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client.workflow_responses import validate_workflow_result, workflow_response_contract
from workflow_response_native import Recorded
from poima_client import WorldClient
from switchback_game_service import Budget, RetainedTransport, retain_host_stream, wait_host

HEALTH, INTERACTION = '1' * 32, '2' * 32
FIELDS = ('world_matrix', 'layout', 'local_transform', 'animation', 'motion',
          'kinematic_target', 'motion_remaining_ticks', 'velocity', 'has_body',
          'is_character', 'ground', 'yaw', 'pitch')
METHODS = set(workflow_response_contract()['methods'])
FILE_CAP, TREE_CAP = 256 * 1048576, 512 * 1048576


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def exact(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return a.keys() == b.keys() and all(exact(a[k], b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(exact(x, y) for x, y in zip(a, b))
    return a == b


def pin(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= FILE_CAP,
         'Missing/linked/oversized input: ' + str(path))
    with path.open('rb') as stream:
        return dict(bytes=path.stat().st_size, sha256=hashlib.file_digest(stream, 'sha256').hexdigest())


def tree(path):
    rows = {}
    for file in sorted(path.rglob('*')):
        need(not file.is_symlink() and not getattr(file, 'is_junction', lambda: False)(), 'Linked input')
        if file.is_file():
            rows[file.relative_to(path).as_posix()] = pin(file)
    need(sum(row['bytes'] for row in rows.values()) <= TREE_CAP, 'Input tree exceeds 512 MiB')
    return rows


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def read(path):
    need(path.stat().st_size <= 16 * 1048576, 'Metadata exceeds 16 MiB')
    def pairs(rows):
        result = {}
        for key, value in rows:
            need(key not in result, 'Duplicate metadata key')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs,
                      parse_constant=lambda value: need(False, 'Nonfinite metadata'))


def contained(root, relative):
    need(type(relative) is str and relative and not Path(relative).is_absolute(), 'Expected relative declared path')
    path = (root / relative).resolve()
    need(path.is_relative_to(root.resolve()), 'Declared path escaped root')
    pin(path)
    return path


def manifest(path, name):
    path = path.resolve()
    result = path / name if path.is_dir() else path
    need(result.name == name, 'Expected ' + name)
    pin(result)
    return result


def assigned_job():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel.IsProcessInJob.restype = wintypes.BOOL
    assigned = wintypes.BOOL()
    need(kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(assigned)) and assigned.value,
         'A supervising Windows Job is required; this script never force-cleans unknown native outcomes')


class Checked(Recorded):
    """Reuse the retained public transport; schema context precedes later reads."""
    def __init__(self, binary, directory, deadline, *, world=None, game=None, saves=None):
        self.context = {'profile': 'readonly_game' if game else 'source_world'}
        self.validated = 0
        self.expected_save_root = saves.resolve()
        # Recorded deliberately supplies natural shutdown only. The caller's Job
        # owns partial startup, blocked pipes, exceptions and unknown outcomes.
        directory.mkdir()
        self.directory, self.calls, self.raw = directory, [], []
        self.budget = Budget(deadline, [])
        self.host, self.host_threads, self.client = None, [], None
        if game:
            endpoint = 'workflow-components-' + uuid.uuid4().hex
            self.host = subprocess.Popen([str(binary), 'game', 'serve', str(game),
                '--endpoint', endpoint, '--save-root', str(self.expected_save_root)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, bufsize=0, shell=False)
            outputs = [directory/'host-stdout.txt', directory/'host-stderr.txt']
            for role, path in zip(('stdout', 'stderr'), outputs):
                path.touch()
                self.budget.paths.append(path)
                thread = threading.Thread(target=retain_host_stream,
                    args=(self.host, role, path, self.budget), daemon=True)
                thread.start()
                self.host_threads.append(thread)
            wait_host(self.host, endpoint, *outputs, self.budget)
            argv = [str(binary), 'connect', endpoint, '--timeout-ms', '10000']
        else:
            argv = [str(binary), 'world', str(world)]
        self.transport = RetainedTransport(argv, directory, self.budget, self.raw.append)
        self.client = WorldClient(self.transport)
        self.session, self.tick = None, None
        status = self.call('runtime.status')
        if game:
            need(status['active'] and status['tick'] == 0, 'Readonly game must auto-start at tick zero')
            self.session, self.tick = status['session_id'], status['tick']
        else:
            need(not status['active'], 'Source must begin inactive')
        configuration = self.call('save.status')
        if not game:
            self.call('save.configure', dict(request_id=uuid.uuid4().hex,
                expected_generation=configuration['generation'], root=str(self.expected_save_root)))
            configuration = self.call('save.status')
        need(configuration['root'] is not None and Path(configuration['root']).resolve() == self.expected_save_root,
             'Native save store is not the explicitly owned external root')
        self.configuration = configuration['generation']

    def call(self, method, params=None, error=None):
        result = super().call(method, params, error)
        if error is None and method in METHODS:
            validate_workflow_result(method, result, {} if params is None else params, self.context)
            self.validated += 1
            if method == 'runtime.components':
                self.context = {**self.context, 'component_schemas': copy.deepcopy(result['schemas'])}
            if method in ('runtime.gameplay.load_native', 'runtime.gameplay.inspect'):
                module = result['module']
                if module is not None and 'schema' in module:
                    self.context = {**self.context, 'gameplay_schema': copy.deepcopy(module['schema'])}
        return result

    def game(self):
        return self.call('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick, include_schema=True))

    def edit(self, **values):
        current = self.game()
        return self.call('runtime.gameplay.edit', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, expected_revision=current['revision'], values=values))

    def step(self, ticks=1):
        params = dict(session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick, ticks=ticks)
        result = self.call('runtime.step', params)
        self.session, self.tick = result['current_session_id'], result['current_tick']
        return params, result


def boundary(owner, result):
    need(result['session_id'] == owner.session and type(result['tick']) is int and result['tick'] == owner.tick,
         'Checkpoint crossed its actual session/tick boundary')
    return {key: copy.deepcopy(value) for key, value in result.items() if key != 'session_id'}


def artifact(descriptor):
    metadata = read(descriptor)
    libraries = [row for row in metadata['files'] if row['role'] == 'library']
    need(len(libraries) == 1 and libraries[0]['path'] == metadata['library'], 'Expected one declared native library')
    library = contained(descriptor.parent, metadata['library'])
    row = libraries[0]
    need(pin(library) == {'bytes': row['size'], 'sha256': row['sha256']}, 'Declared native library bytes differ')
    return metadata, row, library


def module_payload(module, descriptor):
    need(module['backend'] == 'native_aot', 'Expected actual NativeAOT gameplay')
    expected, library_row, library = artifact(descriptor)
    need(Path(module['native_library']).resolve() == library,
         'Module path does not match explicitly trusted descriptor')
    need(module['assembly_sha256'] == library_row['sha256'] and exact(module['schema'], expected['schema']),
         'Loaded image/schema differs from trusted published artifact')
    expected_migration = dict(added=sorted(f['name'] for f in module['schema']['fields']), removed=[], preserved=[])
    need(exact(module['migration'], expected_migration), 'Fresh/restore module did not report exact all-added constructor metadata')
    # Exactly this path differs between source-owned and bundled copies. Every
    # other returned module key is retained and compared with strict JSON types.
    return {key: copy.deepcopy(value) for key, value in module.items() if key != 'native_library'}


def checkpoint(owner, identities, descriptor):
    runtime = owner.call('runtime.inspect', dict(session_id=owner.session))
    registry = owner.call('runtime.components', dict(session_id=owner.session))
    game = owner.game()
    need({s['id'] for s in registry['schemas']} == {HEALTH, INTERACTION}, 'Both nonempty fixture types must be registered')
    need(registry['tick'] == runtime['tick'] == game['tick'] == owner.tick, 'Checkpoint header drift')
    rows, component_rows = [], []
    for identity in identities:
        rows.append(boundary(owner, owner.call('runtime.entity', dict(session_id=owner.session, tick=owner.tick, id=identity))))
        for type_id in (HEALTH, INTERACTION):
            component_rows.append(boundary(owner, owner.call('runtime.component.get', dict(session_id=owner.session,
                tick=owner.tick, id=identity, type=type_id, structure_revision=registry['structure_revision']))))
    observed = owner.call('runtime.observe', dict(session_id=owner.session, tick=owner.tick, ids=identities,
        structure_revision=registry['structure_revision'], component_revision=registry['component_revision'],
        entity_fields=list(FIELDS), components=[dict(type=HEALTH), dict(type=INTERACTION)], include_schemas=True))
    need([row['id'] for row in observed['entities']] == identities, 'Full selected observation lost membership')
    for row in observed['entities']:
        for type_id in (HEALTH, INTERACTION):
            direct = next(x for x in component_rows if x['id'] == row['id'] and x['type'] == type_id)
            need(exact(row['components'][type_id], direct['values']), 'Observation differs from complete component read')
        entity = next(x for x in rows if x['id'] == row['id'])
        need(exact(row['state'], {key: entity[key] for key in FIELDS}), 'Observation differs from complete selected entity state')
    ui = owner.call('runtime.ui.inspect', dict(session_id=owner.session, tick=owner.tick, limit=256))
    need(ui['next_after'] is None, 'Fixture UI exceeds the explicitly bounded complete page')
    gameplay = boundary(owner, game)
    gameplay['module'] = module_payload(game['module'], descriptor)
    result = dict(runtime=boundary(owner, runtime), registry=boundary(owner, registry),
                  gameplay=gameplay, entities=rows, components=component_rows,
                  observation=boundary(owner, observed), ui=boundary(owner, ui))
    # Save queue and epoch have an independent lifecycle and are never stripped
    # recursively from gameplay values or asserted equal across native owners.
    status = owner.call('runtime.save.status', dict(session_id=owner.session))
    need(status['enabled'] and status['configuration_generation'] == owner.configuration and status['pending'] is None,
         'Fixture save routing/pending state differs')
    return result


def payload(checkpoint_value, entity, type_id):
    return next(row['values'] for row in checkpoint_value['components'] if row['id'] == entity and row['type'] == type_id)


def membership(checkpoint_value, type_id):
    return [row['id'] for row in checkpoint_value['components'] if row['type'] == type_id and row['values'] is not None]


def pages(owner, expected):
    registry = owner.call('runtime.components', dict(session_id=owner.session))
    for type_id in (HEALTH, INTERACTION):
        collected, cursor = [], None
        for _ in range(32):
            params = dict(session_id=owner.session, tick=owner.tick, type=type_id, limit=1,
                          structure_revision=registry['structure_revision'])
            if cursor is not None:
                params['after'] = cursor
            page = owner.call('runtime.component.query', params)
            collected.extend(page['entities'])
            cursor = page['next_after']
            if cursor is None:
                break
        else:
            raise AssertionError('Bounded component pagination did not end')
        need(collected == membership(expected, type_id), 'Sorted exclusive query lost/duplicated membership')
    params = dict(session_id=owner.session, tick=owner.tick, query=dict(type=HEALTH, limit=1),
        ids=[membership(expected, INTERACTION)[0]], entity_fields=list(FIELDS),
        components=[dict(type=HEALTH), dict(type=INTERACTION)], include_schemas=True)
    collected, cursor = [], None
    for _ in range(32):
        if cursor is not None:
            params['query']['after'] = cursor
            params.update(structure_revision=registry['structure_revision'], component_revision=registry['component_revision'])
        observed = owner.call('runtime.observe', params)
        for row in observed['entities']:
            reference = next(x for x in expected['observation']['entities'] if x['id'] == row['id'])
            need(exact(row, reference), 'Paged observation changed full selected typed payload')
        collected.extend(observed['query']['entities'])
        cursor = observed['query']['next_after']
        if cursor is None:
            break
    else:
        raise AssertionError('Bounded observation pagination did not end')
    need(collected == membership(expected, HEALTH), 'Joined query continuation changed Health membership')


def rejected_reads(owner, identities, descriptor):
    before = checkpoint(owner, identities, descriptor)
    params = dict(session_id=owner.session, tick=owner.tick, type=HEALTH, id=identities[0])
    owner.call('runtime.component.get', dict(params, tick=float(owner.tick)), error=-32602)
    owner.call('runtime.component.get', dict(params, tick=owner.tick+1), error=-32009)
    owner.call('runtime.observe', dict(session_id=owner.session, tick=owner.tick, ids=[identities[0], identities[0]]), error=-32602)
    owner.call('runtime.observe', dict(session_id=owner.session, tick=owner.tick,
        query=dict(type=HEALTH, limit=1, after=identities[0])), error=-32602)
    need(exact(checkpoint(owner, identities, descriptor), before), 'Rejected reads changed the full selected checkpoint')


def callback_rollbacks(owner, identities, descriptor):
    prior_mode = owner.game()['module']['values']['Mode']
    need(owner.tick % 2 == 0, 'Later-tick rollback must begin at an observed even Tick')
    cases = []
    for mode, ticks, label, marker in ((2, 1, 'duplicate_staged_write', 'Duplicate gameplay component target in one tick.'),
                                      (4, 2, 'later_tick_exception', 'Later component fixture rollback.')):
        owner.edit(Mode=mode)
        def capture():
            return dict(checkpoint=checkpoint(owner, identities, descriptor),
                        save_routing=owner.call('save.status'),
                        save_queue=owner.call('runtime.save.status', dict(session_id=owner.session)),
                        save_files=tree(owner.expected_save_root))
        before = capture()
        params = dict(session_id=owner.session, request_id=uuid.uuid4().hex,
                      expected_tick=owner.tick, ticks=ticks)
        rejected = owner.call('runtime.step', params, error=-32040)
        need(marker in rejected['message'], 'Native rejection did not identify the armed compiled failure: ' + label)
        after = capture()
        need(exact(after, before), 'Compiled callback rejection changed selected state or external saves: ' + label)
        cases.append(dict(case=label, mode=mode, ticks=ticks, request=params,
                          native_error=rejected, before=before, after=after, unchanged=True))
    owner.edit(Mode=prior_mode)
    return cases


def ticket(values):
    mask = (1 << 64)-1
    return dict(epoch=f'{int(values["SaveHigh"]) & mask:016x}{int(values["SaveLow"]) & mask:016x}',
                sequence=int(values['SaveSequence']))


def exercise(owner, identities, descriptor):
    initial = checkpoint(owner, identities, descriptor)
    need(initial['gameplay']['module']['values']['Mode'] == 0, 'Declare Mode0 for the no-op baseline')
    health = membership(initial, HEALTH)
    interaction = membership(initial, INTERACTION)
    need(len(health) >= 3 and bool(interaction) and bool(set(health)-set(interaction)),
         'Need three Health members and both present/absent Interaction memberships')
    selected = health[0]
    owner.step()
    noop = checkpoint(owner, identities, descriptor)
    need(exact(noop['components'], [{**x, 'tick': 1} for x in initial['components']]), 'Default Tick mutated component state')
    values = noop['gameplay']['module']['values']
    need(values['Ticks'] == 1 and values['Queried'] == len(health) and values['Selected'] == selected
         and values['Alive'] == 1, 'Compiled no-op query/read/liveness behavior differs')
    owner.edit(Mode=1)
    owner.step(3)
    written = checkpoint(owner, identities, descriptor)
    current = '0'*31+'1'
    score = '0'*31+'3'
    need(payload(written, selected, HEALTH)[current] == payload(noop, selected, HEALTH)[current]-3
         and payload(written, selected, HEALTH)[score] == str(int(payload(noop, selected, HEALTH)[score])+3),
         'Compiled Tick did not change Health and lossless score as declared')
    need(written['registry']['component_revision'] == noop['registry']['component_revision']+3
         and written['gameplay']['module']['values']['ReadBeforeWrite'] == 1, 'Staged native component writes/revisions differ')
    pages(owner, written)
    rejected_reads(owner, identities, descriptor)
    rollbacks = callback_rollbacks(owner, identities, descriptor)
    owner.edit(Mode=8, SaveHigh='0', SaveLow='0', SaveSequence='0', SaveState=0)
    _, saved_step = owner.step()
    need(saved_step['save_serviced'] and not saved_step['runtime_replaced'], 'Compiled Tick save was not serviced')
    saved = checkpoint(owner, identities, descriptor)
    saved_values = saved['gameplay']['module']['values']
    identity = ticket(saved_values)
    operation = owner.call('runtime.save.result', identity)
    need(operation['kind'] == 1 and operation['state'] == 3 and operation['generation'] == 1
         and saved_values['SaveState'] == 1, 'Actual save completion versus saved callback state differs')
    need(exact(saved_step['save_operation'], operation), 'Step/save ticket completion differs')
    inspected = owner.call('save.inspect', dict(slot='components-game'))
    need(inspected['generation'] == 1 and inspected['selected']['verified'], 'Native saved bytes are not verified')
    owner.step(2)
    need(owner.game()['module']['values']['SaveState'] == 3, 'Same-owner Tick did not see completed ticket')
    owner.edit(Mode=9, SaveHigh='0', SaveLow='0', SaveSequence='0', SaveState=0)
    request, loaded_step = owner.step()
    need(loaded_step['runtime_replaced'] and loaded_step['save_serviced']
         and loaded_step['session_id'] == request['session_id'] and owner.session != request['session_id']
         and owner.tick == saved['runtime']['tick'], 'Compiled Tick load boundary was not replaced/restored')
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Compiled load failed full selected typed checkpoint equality')
    before = checkpoint(owner, identities, descriptor)
    replay = owner.call('runtime.step', request)
    need(exact(replay, dict(loaded_step, replayed=True)) and exact(checkpoint(owner, identities, descriptor), before),
         'Historical step retry reran the compiled load or changed state')
    need(owner.call('runtime.save.result', identity)['state'] == 3, 'Same owner lost successful pre-restore ticket')
    owner.step()
    continuation = checkpoint(owner, identities, descriptor)
    need(continuation['gameplay']['module']['values']['SaveState'] == 3
         and payload(continuation, selected, HEALTH)[current] == payload(saved, selected, HEALTH)[current]-1,
         'Restored module failed next Tick continuation')
    return dict(initial=initial, noop=noop, written=written, saved=saved,
                restored=restored, continuation=continuation, callback_rollbacks=rollbacks), identity


def cold(owner, identities, descriptor, saved, old_ticket, same_owner_continuation):
    initial = owner.game()
    runtime = owner.call('runtime.inspect', dict(session_id=owner.session))
    registry = owner.call('runtime.components', dict(session_id=owner.session))
    ui = owner.call('runtime.ui.inspect', dict(session_id=owner.session, tick=owner.tick))
    for observed in (initial, runtime, registry, ui):
        boundary(owner, observed)
    need(registry['structure_revision'] == runtime['structure_revision']
         and ui['ui_revision'] == runtime['ui_revision']
         and ui['control_sequence'] == runtime['control_sequence'], 'Cold active runtime read guards disagree')
    params = dict(request_id=uuid.uuid4().hex, configuration_generation=owner.configuration,
        slot='components-game', expected_generation=1, revision=runtime['current_authored_revision'],
        expected_session_id=owner.session, expected_tick=owner.tick, expected_gameplay_revision=initial['revision'],
        expected_component_revision=registry['component_revision'],
        expected_structure_revision=registry['structure_revision'], expected_ui_revision=ui['ui_revision'],
        expected_control_sequence=ui['control_sequence'], expected_material_revision=runtime['material_revision'],
        new_session_id=uuid.uuid4().hex)
    result = owner.call('save.load', params)
    need(result['session_id'] == params['new_session_id'] and result['session_id'] != owner.session,
         'Cold readonly restore did not bind fresh requested runtime identity')
    owner.session, owner.tick = result['session_id'], result['tick']
    restored = checkpoint(owner, identities, descriptor)
    need(exact(restored, saved), 'Fresh readonly owner lost full selected saved state')
    need(owner.call('runtime.save.result', old_ticket)['state'] == 0, 'Fresh owner fabricated previous owner ledger receipt')
    owner.step()
    continued = checkpoint(owner, identities, descriptor)
    expected_continuation = copy.deepcopy(same_owner_continuation)
    expected_continuation['gameplay']['module']['values']['SaveState'] = 0
    need(exact(continued, expected_continuation), 'Fresh readonly Tick continuation differs beyond the declared expired-ticket SaveState result')
    return dict(restored=restored, continuation=continued)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--source', '--project', dest='source', required=True, type=Path)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Windows Python without optimization required')
    need(30 <= args.timeout <= 600, 'Timeout must be 30..600 seconds')
    assigned_job()
    args.binary, args.output = args.binary.resolve(), args.output.resolve()
    source, game = manifest(args.source, 'project.json'), manifest(args.game, 'game.json')
    need(not args.output.exists() and not args.output.is_relative_to(source.parent)
         and not args.output.is_relative_to(game.parent), 'Fresh independent output outside source/game required')
    initial_inventory = dict(source=tree(source.parent), game=tree(game.parent))
    args.output.mkdir(parents=True)
    started = time.monotonic()
    deadline = started + args.timeout
    report = dict(passed=False, binary=pin(args.binary), harness=pin(Path(__file__)),
        original_inputs=initial_inventory, owners=[], checkpoints={}, errors=[],
        limits=['Selected full entity/component/gameplay/UI observations; no claim of complete internal version9 snapshot comparison.',
                'Actual Job closure must be joined to the supervising ownership receipt; no inferred global process census.',
                'No rendering, physical input, audio listening, performance or complete21method/ABI/stability qualification.',
                'This fixture has scalar Health/Interaction fields; readonly bounded arrays remain a separate consumer gate.',
                'Source, compiler and generic checkout/toolchain paths may remain available; no source-absence or clean-machine claim.'])
    owner = None
    try:
        source_spec, game_spec = read(source), read(game)
        game_binary = contained(game.parent, game_spec['engine']['executable'])
        executable_rows = [row for row in game_spec['files'] if row['path'] == game_spec['engine']['executable']]
        need(len(executable_rows) == 1 and executable_rows[0]['role'] == 'runtime', 'Bundled executable is not uniquely declared')
        executable_row = executable_rows[0]
        need(pin(game_binary) == {'bytes': executable_row['size'], 'sha256': executable_row['sha256']}, 'Bundled executable inventory hash/size mismatch')
        report['bundled_binary'] = dict(path=game_spec['engine']['executable'], **pin(game_binary))
        report['binary_selection'] = 'Source standalone uses explicit --binary; readonly shared/cold hosts and clients use exact declared bundled executable.'
        need(exact(source_spec['gameplay'].get('values', {}), game_spec['gameplay']['values']), 'Declared initial gameplay values differ')
        copied = args.output/'source-project'
        shutil.copytree(source.parent, copied)
        copied_before = tree(copied)
        source_descriptor = contained(copied, source_spec['gameplay']['descriptor'])
        game_descriptor = contained(game.parent, game_spec['gameplay']['descriptor'])
        (sd, sl, _), (gd, gl, _) = artifact(source_descriptor), artifact(game_descriptor)
        need(sd['identity'] == gd['identity'] == 'poima.test.components' and exact(sd['schema'], gd['schema'])
             and sl['sha256'] == gl['sha256'] and sl['size'] == gl['size'], 'Source/bundle trusted module cohort differs')
        world = contained(copied, source_spec['entry']['world'])
        identities = sorted(read(world)['entities'])
        need(1 <= len(identities) <= 32, 'Bounded fixture requires1..32 complete entity rows')
        all_results = {}
        for name, readonly in [('source-standalone', False), ('readonly-shared', True)]:
            saves = args.output/(name+'-saves')
            saves.mkdir()
            owner = Checked(game_binary if readonly else args.binary, args.output/name, deadline, world=world if not readonly else None,
                            game=game if readonly else None, saves=saves)
            if not readonly:
                revision = owner.call('world.inspect')['revision']
                owner.session, owner.tick = uuid.uuid4().hex, 0
                owner.call('runtime.start', dict(session_id=owner.session, revision=revision))
                owner.call('runtime.gameplay.load_native', dict(session_id=owner.session, request_id=uuid.uuid4().hex,
                    expected_tick=0, expected_revision=0, descriptor=str(source_descriptor),
                    values=source_spec['gameplay'].get('values', {})))
            results, old_ticket = exercise(owner, identities, game_descriptor if readonly else source_descriptor)
            all_results[name] = results
            report['checkpoints'][name] = results
            if readonly:
                readonly_ticket, readonly_saves = old_ticket, saves
            owner.close()
            report['owners'].append(dict(name=name, calls=len(owner.calls), validated_successes=owner.validated,
                returncode=owner.transport.returncode, host_returncode=None if owner.host is None else owner.host.returncode))
            owner = None
            need(dict(source=tree(source.parent), game=tree(game.parent)) == initial_inventory, 'Original input bytes changed')
        for name in ('initial', 'noop', 'written'):
            need(exact(all_results['source-standalone'][name], all_results['readonly-shared'][name]),
                 'Source/readonly complete selected typed checkpoint differs: ' + name)
        report['source_readonly_typed_pairs'] = 3
        report['same_owner_save_restore_typed_pairs'] = 2
        report['compiled_callback_rollback_checks'] = dict(total=4, scopes=['source-standalone', 'readonly-shared'],
            cases=['duplicate_staged_write', 'later_tick_exception'],
            assertion='Strict typed equality of complete selected entity/component/gameplay/UI checkpoint, actual owner save routing/queue and external save-file inventory before/after native -32040. No failed-command receipt or complete internal snapshot claim.')
        report['comparison_scope'] = 'Three pre-ticket cross-owner pairs; full post-ticket save/restore pairs within each owner and cold readonly owner. Random ticket globals remain present and exact in restore pairs.'
        owner = Checked(game_binary, args.output/'readonly-cold', deadline, game=game, saves=readonly_saves)
        report['checkpoints']['readonly-cold'] = cold(owner, identities, game_descriptor,
            all_results['readonly-shared']['saved'], readonly_ticket, all_results['readonly-shared']['continuation'])
        owner.close()
        report['owners'].append(dict(name='readonly-cold', calls=len(owner.calls), validated_successes=owner.validated,
            returncode=owner.transport.returncode, host_returncode=owner.host.returncode))
        owner = None
        report['cold_readonly_save_restore_typed_pairs'] = 1
        after = tree(copied)
        for name, row in copied_before.items():
            need(after.get(name) == row, 'Owned source input changed: ' + name)
        extras = set(after)-set(copied_before)
        permitted = {world.relative_to(copied).as_posix()+'.lock'}
        need(extras <= permitted and all(after[name]['bytes'] == 0 for name in extras), 'Unexpected owned source sidecar')
        report['copied_source_sidecars'] = {name: after[name] for name in extras}
        need(dict(source=tree(source.parent), game=tree(game.parent)) == initial_inventory, 'Original source/game changed')
        report['original_inputs_unchanged'] = True
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:
                owner.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
                report['passed'] = False
        report['seconds'] = time.monotonic()-started
        write(args.output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], seconds=report['seconds'], errors=len(report['errors']))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
