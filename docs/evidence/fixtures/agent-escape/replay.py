#!/usr/bin/env python3
"""Replay the recorded escape game through a fresh native world service."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import traceback
import uuid

FIXTURE = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Engine:
    def __init__(self, binary, output):
        self.calls = []
        self.session = uuid.uuid4().hex
        self.tick = 0
        self.closed = False
        self.lines = queue.Queue()
        self.stderr = (output/'engine.stderr.log').open('a', encoding='utf-8')
        try:
            self.process = subprocess.Popen(
                [str(binary), 'world', str(output/'world.json')],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
                text=True, encoding='utf-8', bufsize=1,
                creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == 'nt' else 0)
        except BaseException:
            self.stderr.close()
            raise

        def read():
            try:
                for line in self.process.stdout:
                    self.lines.put(line)
            except BaseException as error:
                self.lines.put(error)
            finally:
                self.lines.put(None)
        threading.Thread(target=read, daemon=True).start()

    def rpc(self, method, params=None):
        request = dict(jsonrpc='2.0', id=len(self.calls)+1, method=method, params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n')
        self.process.stdin.flush()
        try:
            line = self.lines.get(timeout=60)
        except queue.Empty:
            raise TimeoutError(f'Native service timed out handling {method}.') from None
        require(isinstance(line, str), 'Native service ended or its output reader failed; see engine.stderr.log.')
        response = json.loads(line)
        self.calls.append(dict(request=request, response=response))
        require(isinstance(response, dict) and response.get('jsonrpc') == '2.0'
                and type(response.get('id')) is int and response['id'] == request['id'],
                'Native JSON-RPC response ID/version mismatch.')
        require('error' not in response, f'{method}: {response.get("error")}')
        return response['result']

    def inspect(self):
        return self.rpc('runtime.inspect', dict(session_id=self.session))

    def values(self):
        return self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))['module']['values']

    def entity(self, entity_id):
        return self.rpc('runtime.entity', dict(session_id=self.session, tick=self.tick, id=entity_id))

    def adopt(self, result):
        self.session = result['current_session_id']
        self.tick = result['current_tick']
        return result

    def step(self, segment, player):
        inputs = {key: value for key, value in segment.items() if key in ('move', 'look', 'use', 'jump')}
        if inputs:
            inputs['entity'] = player
        return self.adopt(self.rpc('runtime.step', dict(
            session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick,
            expected_structure_revision=self.inspect()['structure_revision'], ticks=segment['ticks'],
            inputs=[inputs] if inputs else [])))

    def control(self, element_id):
        state = self.inspect()
        game = self.rpc('runtime.gameplay.inspect', dict(session_id=self.session, tick=self.tick))
        return self.adopt(self.rpc('runtime.ui.activate', dict(
            session_id=self.session, request_id=uuid.uuid4().hex, expected_tick=self.tick,
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'], expected_structure_revision=state['structure_revision'], id=element_id)))

    def close(self):
        if self.closed:
            return
        errors = []
        try:
            try:
                self.process.stdin.close()
            except BaseException as error:
                errors.append(f'stdin close: {error}')
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                errors.append('Native service required forced termination after 15 seconds.')
            except BaseException as error:
                errors.append(f'wait: {error}')
        finally:
            if self.process.poll() is None:
                try:
                    self.process.kill()
                    self.process.wait(timeout=10)
                except BaseException as error:
                    errors.append(f'forced cleanup: {error}')
            self.closed = self.process.poll() is not None
            if self.closed:
                for stream in (self.stderr, self.process.stdout):
                    try:
                        stream.close()
                    except BaseException as error:
                        errors.append(f'stream close: {error}')
            if self.process.returncode != 0:
                errors.append(f'Native service exit: {self.process.returncode}')
        require(not errors, '; '.join(errors))


def check_state(engine, manifest, count, escaped):
    values = engine.values()
    require(values[manifest['collected_count_field']] == count and values[manifest['escaped_field']] == escaped,
            f'Unexpected gameplay state: {values}')
    return values


def phase(engine, definition, manifest):
    for segment in definition['segments']:
        engine.step(segment, manifest['player_id'])
    expected = definition['expected']
    require(engine.tick == expected['tick'], f'Unexpected route tick: {engine.tick}')
    state = check_state(engine, manifest, expected['state']['CollectedCount'], expected['state']['Escaped'])
    # Save ticket epochs and active session IDs are deliberately not replay constants.
    for field in (*manifest['key_handle_fields'], 'LockedAttempts'):
        require(state[field] == expected['state'][field], f'Unexpected {field}: {state[field]}')
    return state


def check_hud(engine, text):
    ui = engine.rpc('runtime.ui.inspect', dict(session_id=engine.session, tick=engine.tick))
    require(any(row['id'] == '000000000000000000000000000000c9' and text in row['text']
                for row in ui['elements']), f'Missing HUD text: {text}')


def check_restore(engine, manifest, saved_tick, saved_matrix, one_key):
    require(engine.tick == saved_tick, 'Checkpoint tick was not restored.')
    state = check_state(engine, manifest, 1, 0)
    require(engine.entity(manifest['player_id'])['world_matrix'] == saved_matrix,
            'Checkpoint did not restore the exact player matrix.')
    for field in manifest['key_handle_fields']:
        require(state[field] == one_key[field], f'Checkpoint changed {field}.')
        if state[field] != '0'*32:
            engine.entity(state[field])  # A preserved handle must resolve to a live spawned entity.
    check_hud(engine, 'Keys 1/3')
    check_hud(engine, 'LOCKED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'hostfxr', 'bridge', 'assembly', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    paths = {name: getattr(args, name).resolve() for name in ('engine', 'hostfxr', 'bridge', 'assembly', 'output')}
    for name in ('engine', 'hostfxr', 'bridge', 'assembly'):
        if not paths[name].is_file():
            parser.error(f'--{name} must name an existing file.')
    output = paths['output']
    # Atomic creation prevents accidentally overwriting another game or its durable saves.
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('--output must be a new directory.')
    manifest = json.loads((FIXTURE/'manifest.json').read_text(encoding='utf-8'))
    plan = json.loads((FIXTURE/'plan.json').read_text(encoding='utf-8'))
    evidence = dict(passed=False, scope='Headless controller replay, logical UI and durable checkpoint continuation.',
                    graphical_or_physical_input_qualified=False, cleanup_errors=[], processes=[], checks=[],
                    hashes={name: digest(paths[name]) for name in ('engine', 'hostfxr', 'bridge', 'assembly')})
    evidence['hashes'].update({name: digest(FIXTURE/name) for name in ('Escape.cs', 'world.json', 'plan.json', 'manifest.json', 'replay.py')})
    owned = []
    completed = False
    try:
        shutil.copyfile(FIXTURE/'world.json', output/'world.json')
        (output/'saves').mkdir()

        def start():
            engine = Engine(paths['engine'], output)
            owned.append(engine)
            revision = engine.rpc('world.inspect')['revision']
            engine.rpc('runtime.start', dict(session_id=engine.session, revision=revision))
            loaded = engine.rpc('runtime.gameplay.load', dict(
                session_id=engine.session, request_id=uuid.uuid4().hex, expected_tick=0, expected_revision=0,
                hostfxr=str(paths['hostfxr']), bridge=str(paths['bridge']), assembly=str(paths['assembly']), type=manifest['type']))
            require(loaded['module']['assembly_sha256'] == evidence['hashes']['assembly'],
                    'Native runtime loaded a different gameplay assembly.')
            engine.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=str(output/'saves')))
            check_state(engine, manifest, 0, 0)
            return engine

        engine = start()
        locked = phase(engine, plan['phases'][0], manifest)
        require(locked['LockedAttempts'] >= 1, 'The initial locked exit was not exercised.')
        check_hud(engine, 'LOCKED')
        one_key = phase(engine, plan['phases'][1], manifest)
        saved_tick = engine.tick
        saved_matrix = engine.entity(manifest['player_id'])['world_matrix']
        require(engine.control(manifest['pause_ui_id'])['intent'] == 2, 'Compiled Pause did not request pause.')
        require(engine.control(manifest['resume_ui_id'])['intent'] == 1, 'Compiled Resume did not request resume.')
        saved = engine.control(manifest['save_ui_id'])
        require(saved.get('save_serviced') and saved['save_operation']['state'] == 3, 'Compiled Save failed.')
        slot = engine.rpc('save.inspect', dict(slot=manifest['checkpoint_slot']))
        require(slot['selected']['verified'] and slot['generation'] == 1, 'Durable checkpoint was not verified.')
        phase(engine, plan['phases'][2], manifest)
        check_hud(engine, 'ESCAPED')
        evidence['checks'].append('Recorded controller route rejects the locked exit, collects all three keys and completes the game.')
        previous_session = engine.session
        restored = engine.control(manifest['load_ui_id'])
        require(restored.get('save_serviced') and restored['runtime_replaced']
                and restored['save_operation']['state'] == 3 and restored['save_operation']['kind'] == 2
                and engine.session != previous_session, 'Compiled Load did not successfully replace the runtime.')
        check_restore(engine, manifest, saved_tick, saved_matrix, one_key)
        require(engine.control(manifest['resume_ui_id'])['intent'] == 1, 'Restored Resume failed.')
        phase(engine, plan['restore_and_finish'], manifest)
        check_hud(engine, 'ESCAPED')
        evidence['checks'].append('Compiled Pause/Resume/Save/Load restores tick, key handles and exact player matrix, then completes again.')
        engine.close()

        # Reopen from disk in a separate native process; no in-memory runtime survives.
        engine = start()
        previous_session = engine.session
        restored = engine.control(manifest['load_ui_id'])
        require(restored.get('save_serviced') and restored['runtime_replaced']
                and restored['save_operation']['state'] == 3 and restored['save_operation']['kind'] == 2
                and engine.session != previous_session, 'Fresh-process Load did not successfully replace the runtime.')
        check_restore(engine, manifest, saved_tick, saved_matrix, one_key)
        require(engine.control(manifest['resume_ui_id'])['intent'] == 1, 'Fresh-process Resume failed.')
        final = phase(engine, plan['restore_and_finish'], manifest)
        check_hud(engine, 'ESCAPED')
        evidence['checks'].append('A separate native process loads the durable checkpoint and completes the game again.')
        evidence.update(restored_tick=saved_tick, final_tick=engine.tick, final_values=final,
                        saved_player_matrix=saved_matrix, fresh_process_continuation=True)
        completed = True
    except BaseException:
        evidence['error'] = traceback.format_exc()
    finally:
        for engine in reversed(owned):
            try:
                engine.close()
            except BaseException:
                evidence['cleanup_errors'].append(traceback.format_exc())
        evidence['processes'] = [dict(pid=engine.process.pid, exit_code=engine.process.returncode, calls=engine.calls) for engine in owned]
        evidence['passed'] = completed and 'error' not in evidence and not evidence['cleanup_errors']
        (output/'replay-evidence.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=evidence['passed'], checks=evidence['checks'], evidence=str(output/'replay-evidence.json'))))
    if not evidence['passed']:
        print(evidence.get('error') or evidence['cleanup_errors'], file=sys.stderr)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
