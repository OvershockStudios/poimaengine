#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Save played/despawned sound emitters and continue in a relocated fresh owner.

Uses the existing diagnostic WAV, not generated game content. No build, GPU or
device is needed. Every native request and owned output is retained. An explicit
baseline mode observes the pre-fix save rejection; it is not a successful save.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def uid(value):
    return f'{value:032x}'


def inventory(path):
    return {p.relative_to(path).as_posix(): sha(p)
            for p in sorted(path.rglob('*')) if p.is_file()}


def normalized(value):
    result = copy.deepcopy(value)
    result.pop('session_id', None)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--output', type=Path)
    p.add_argument('--expect-save-failure', action='store_true')
    p.add_argument('--windows-interop', action='store_true')
    args = p.parse_args()
    need(not sys.flags.optimize, 'Assertions/validation must remain enabled')
    binary = args.binary.resolve(strict=True)
    args.output = args.output or ROOT/'build'/('sound-retirement-'+uuid.uuid4().hex)
    need(not args.output.exists(), 'Use a new output directory')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True)
    record = dict(passed=False, baseline_defect_mode=args.expect_save_failure,
        binary_sha256=sha(binary), verifier_sha256=sha(__file__),
        diagnostic_source_sha256=sha(ROOT/'examples/assets/acoustic-probe.wav'),
        calls=[], owners=[], groups=[],
        scope='Native logical sound/save/instance lifecycle and relocated fresh-process continuation; no generated game content, graphics, audibility or performance claim.')

    def native(path):
        value = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip() \
            if args.windows_interop and os.name != 'nt' else value

    class Owner:
        def __init__(self, directory, label):
            self.directory, self.session, self.tick, self.structure = directory, None, 0, 0
            self.client = WorldClient.open(str(binary), native(directory/'world.json'), close_timeout=20)
            self.row = dict(label=label, pid=self.client.transport.process_id, exit_code=None)
            record['owners'].append(self.row)

        def rpc(self, method, params=None, error=None):
            row = dict(owner=self.row['label'], method=method, params=params or {})
            record['calls'].append(row)
            try:
                result = self.client.call(method, params or {}, timeout=60)
            except RpcError as e:
                row['error'] = dict(code=e.code, message=e.message, data=e.data)
                need(error is not None and e.code == error, 'Unexpected RPC rejection: '+str(row))
                return row['error']
            row['result'] = result
            need(error is None, 'Expected a rejection: '+method)
            return result

        def close(self):
            self.client.close()
            self.row.update(exit_code=self.client.transport.returncode,
                stderr=self.client.transport.stderr_tail,
                stderr_truncated=self.client.transport.stderr_truncated)
            need(self.row['exit_code'] == 0 and not self.row['stderr'] and
                 not self.row['stderr_truncated'], 'Native owner did not exit cleanly')

        def voices(self):
            return normalized(self.rpc('runtime.audio.voices',
                dict(session_id=self.session, tick=self.tick, limit=256)))

        def change(self, **changes):
            result = self.rpc('runtime.structure.transact', dict(request_id=uuid.uuid4().hex,
                session_id=self.session, expected_tick=self.tick,
                expected_structure_revision=self.structure, **changes))
            self.structure += 1
            return result

        def step(self, sounds, ticks=1, error=None):
            result = self.rpc('runtime.step', dict(request_id=uuid.uuid4().hex,
                session_id=self.session, expected_tick=self.tick,
                expected_structure_revision=self.structure, ticks=ticks, sounds=sounds), error)
            if error is None:
                self.tick += ticks
            return result

        def configure(self):
            (self.directory/'saves').mkdir(exist_ok=True)
            self.rpc('save.configure', dict(request_id=uuid.uuid4().hex,
                expected_generation=0, root=native(self.directory/'saves')))

        def write(self, generation):
            return dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                slot='sound', expected_generation=generation, session_id=self.session,
                expected_tick=self.tick, expected_gameplay_revision=0,
                expected_structure_revision=self.structure)

    try:
        for label, count, removed, loop, ticks in (
            ('sole-active', 1, (0,), True, 1),
            ('newest-finished', 3, (2,), False, 120),
            ('middle-active', 3, (1,), True, 1)):
            original = args.output/(label+'-original')
            original.mkdir()
            owner = Owner(original, label+'-original')
            group = dict(name=label, loop=loop, played_ticks=ticks)
            record['groups'].append(group)
            try:
                source = original/'diagnostic.wav'
                shutil.copyfile(ROOT/'examples/assets/acoustic-probe.wav', source)
                asset = owner.rpc('asset.audio.import', dict(source=native(source)))['asset']
                source.unlink()  # Only the owned fixture copy is removed.
                transform = dict(position=[0,0,0], rotation=[0,0,0,1], scale=[1,1,1])
                emitter = dict(asset=asset, gain=.25, loop=loop, enabled=True)
                template, local = uid(500), uid(501)
                ops = [dict(op='entity.create', id=uid(101), name='Listener'),
                    dict(op='component.set', id=uid(101), type='Camera',
                         value=dict(vertical_fov=60, near=.1, far=100)),
                    dict(op='template.set', id=template, name='Removable audio prop', root=local,
                         entities={local:dict(name='Audio prop', parent=None,
                             components=dict(Transform=transform, AudioEmitter=emitter))})]
                owner.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=ops))
                owner.session = uuid.uuid4().hex
                owner.rpc('runtime.start', dict(session_id=owner.session, revision=1))
                born = owner.change(spawns=[dict(template_id=template) for _ in range(count)])['spawned']
                owner.step([dict(op='play', emitter=identity) for identity in born], ticks)
                initial = owner.voices()
                need([v['voice'] for v in initial['voices']] == list(range(1,count+1)) and
                     initial['next_voice'] == count+1, 'Initial voice allocation differs')
                if loop:
                    need(initial['emitting'] == count, 'Loop fixture did not remain active')
                else:
                    need(initial['emitting'] == 0, 'One-shot fixture has not finished')
                owner.change(despawns=[born[i] for i in removed])
                partial = owner.voices()
                survivors = [i+1 for i in range(count) if i not in removed]
                need([v['voice'] for v in partial['voices']] == survivors and
                     partial['next_voice'] == count+1, 'Retirement reused IDs or lost survivors')
                owner.configure()
                request = owner.write(0)
                if args.expect_save_failure:
                    storage_before = inventory(original/'saves')
                    failure = owner.rpc('save.write', request, -32070)
                    need(failure['message'] in (
                        'Sound allocator does not follow its retained newest voice.',
                        'Unpruned sound history has missing voice IDs.'),
                        'Baseline rejected for another reason')
                    need(owner.voices() == partial and
                         inventory(original/'saves') == storage_before,
                         'Failed baseline save mutated voices or durable storage')
                    group.update(defect_observed=True, failure=failure, partial=partial,
                                 failed_save_isolated=True)
                    continue
                written = owner.rpc('save.write', request)
                before = owner.voices()
                need(owner.rpc('save.write', request) == dict(written, replayed=True) and
                     owner.voices() == before, 'Save receipt retry mutated voices')
                group.update(partial=partial, first_save=written)
            finally:
                owner.close()
            if args.expect_save_failure:
                continue

            frozen = inventory(original)
            relocated = args.output/(label+'-relocated')
            shutil.copytree(original, relocated)
            fresh = Owner(relocated, label+'-fresh')
            try:
                fresh.configure()
                fresh.session, fresh.tick, fresh.structure = uuid.uuid4().hex, ticks, 2
                fresh.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                    slot='sound', expected_generation=1, revision=1, expected_session_id=None,
                    expected_tick=None, expected_gameplay_revision=None, expected_structure_revision=None,
                    new_session_id=fresh.session))
                need(fresh.voices() == partial, 'Fresh restore changed exact voices/allocator/cursors')
                before = fresh.voices()
                # The retired record is unknown. A play preceding the invalid
                # stop must roll back, including the allocator and native tick.
                commands = [] if not survivors else [dict(op='play', emitter=born[survivors[0]-1])]
                commands.append(dict(op='stop', voice=removed[0]+1))
                fresh.step(commands, error=-32040)
                need(fresh.voices() == before, 'Failed sound batch changed restored voices or allocator')
                added = fresh.change(spawns=[dict(template_id=template)])['spawned'][0]
                fresh.step([dict(op='play', emitter=added)])
                continued = fresh.voices()
                need(continued['voices'][-1]['voice'] == count+1 and
                     continued['next_voice'] == count+2, 'Restored continuation reused a retired handle')
                remaining = [born[i] for i in range(count) if i not in removed]
                fresh.change(despawns=remaining+[added])
                empty = fresh.voices()
                need(empty['voices'] == [] and empty['next_voice'] == count+2,
                     'Removing every emitter reset the allocator')
                fresh.rpc('save.write', fresh.write(1))
                group.update(continued=continued, empty=empty)
            finally:
                fresh.close()
            # A third owner proves the empty retained history itself reloads.
            final = Owner(relocated, label+'-empty-fresh')
            try:
                final.configure()
                final.session, final.tick, final.structure = uuid.uuid4().hex, ticks+1, 4
                final.rpc('save.load', dict(request_id=uuid.uuid4().hex, configuration_generation=1,
                    slot='sound', expected_generation=2, revision=1, expected_session_id=None,
                    expected_tick=None, expected_gameplay_revision=None, expected_structure_revision=None,
                    new_session_id=final.session))
                need(final.voices() == empty, 'Empty retired history failed fresh restore')
                last = final.change(spawns=[dict(template_id=template)])['spawned'][0]
                final.step([dict(op='play', emitter=last)])
                tail = final.voices()
                need(tail['voices'][0]['voice'] == count+2 and tail['next_voice'] == count+3,
                     'Empty restored history reused a previously allocated handle')
                group['empty_continuation'] = tail
            finally:
                final.close()
            need(inventory(original) == frozen, 'Relocated continuation modified original closure')
            group['original_unchanged'] = True
        need(sha(binary) == record['binary_sha256'] and sha(__file__) == record['verifier_sha256'] and
             sha(ROOT/'examples/assets/acoustic-probe.wav') == record['diagnostic_source_sha256'],
             'Qualification inputs changed during execution')
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
        raise
    finally:
        record['rpc_count'] = len(record['calls'])
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, groups=len(record['groups']), owners=len(record['owners']),
                         rpc_count=record['rpc_count'], baseline_defect_mode=args.expect_save_failure)))


if __name__ == '__main__':
    main()
