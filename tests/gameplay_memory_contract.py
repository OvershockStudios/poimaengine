#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless Windows CLR-memory RPC checks inside an external supervising Job.

Inputs are already compiled engine/bridge/fixture closures, a retained older
bridge and a complete Native AOT world. No compiler, renderer, explicit GC or
unknown-process cleanup runs here. Keep original wires and complete save bytes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests'))
from gameplay_callback_metrics_contract import (Probe, census, inventory, pin,
    fresh, same, reload_state, unchanged_failure)
from workflow_components_game_service import assigned_job, exact, need, write

GAME = 'Poima.Qualification.MemoryProbe'
BAD_INIT = 'Poima.Qualification.ThrowingInitializeMemoryProbe'


def decimal(value, maximum=2**63-1):
    need(type(value) is str and re.fullmatch(r'0|[1-9][0-9]{0,18}', value),
         'Canonical raw unsigned decimal string required')
    number = int(value)
    need(number <= maximum, 'Raw source counter exceeds its range')
    return number


class MemoryProbe(Probe):
    def load(self, profile=None, *, game=GAME, error=None):
        return super().load(profile, game=game, error=error)

    def checkpoint(self, label, report):
        point = self.probe('memory-' + label)
        document = point['document']
        need(document['format'] == 'poima.world-save' and
             document['snapshot']['payload']['tick'] == self.tick,
             'Complete actual saved snapshot at the current tick')
        target = self.directory.parent / 'checkpoints' / (label + '.bin')
        target.parent.mkdir(exist_ok=True)
        shutil.copyfile(point['path'], target)
        need(pin(target) == point['identity'], 'Whole original checkpoint copy')
        report['checkpoints'][label] = dict(file=str(target), identity=pin(target),
            snapshot_version=document['snapshot']['version'],
            payload_sha256=document['snapshot']['sha256'])
        return point

    def memory(self, supported, reason=None):
        row = self.call('runtime.gameplay.memory',
                        dict(session_id=self.session, tick=self.tick))
        game = self.game()
        need(set(row) == {'session_id', 'tick', 'revision', 'structure_revision', 'module'}
             and row['session_id'] == self.session and type(row['tick']) is int
             and row['tick'] == self.tick and row['revision'] == game['revision']
             and row['structure_revision'] == game['structure_revision'],
             'Actual session, tick and revision association')
        module = row['module']
        need(type(module) is dict and set(module) == {'format', 'version', 'backend',
             'identity', 'assembly_sha256', 'supported', 'unavailable_reason', 'data'},
             'Closed memory wrapper')
        need(module['format'] == 'poima.gameplay-memory-snapshot.v1' and
             type(module['version']) is int and module['version'] == 1 and
             module['backend'] == game['module']['backend'] and
             module['identity'] == game['module']['schema']['identity'] and
             module['assembly_sha256'] == game['module']['assembly_sha256'] and
             module['supported'] is supported and module['unavailable_reason'] == reason,
             'Actual loaded module and supported/unavailable state')
        if supported is not True:
            need(module['data'] is None, 'Unavailable observations must remain null')
            return row
        data = module['data']
        need(type(data) is dict and set(data) == {'format', 'version', 'process_id',
             'timestamp_start', 'timestamp_end', 'timestamp_frequency', 'active_modules',
             'retired_contexts_tracked', 'retired_contexts_alive', 'gc', 'last_gc'},
             'Closed original CLR snapshot')
        need(data['format'] == 'poima.managed-memory-snapshot.v1' and
             type(data['version']) is int and data['version'] == 1 and
             type(data['process_id']) is int and data['process_id'] > 0 and
             decimal(data['timestamp_end']) >= decimal(data['timestamp_start']) and
             decimal(data['timestamp_frequency']) > 0, 'CLR identity and sample bracket')
        need(data['process_id'] == self.transport.process.pid,
             'Snapshot belongs to the actual owned native process')
        for key, maximum in [('active_modules', 32), ('retired_contexts_tracked', 1024),
                             ('retired_contexts_alive', 1024)]:
            need(type(data[key]) is int and 0 <= data[key] <= maximum, 'Context bounds')
        need(data['active_modules'] == 1 and
             data['retired_contexts_alive'] <= data['retired_contexts_tracked'],
             'One live fixture module and bounded weak observations')
        need(set(data['gc']) == {'total_memory_bytes', 'total_allocated_bytes',
             'gen0_collections', 'gen1_collections', 'gen2_collections'}, 'Closed GC counters')
        for key, value in data['gc'].items():
            decimal(value, 2**31-1 if key.endswith('_collections') else 2**63-1)
        latest = data['last_gc']
        need(set(latest) == {'index', 'generation', 'concurrent', 'compacted',
             'heap_size_bytes', 'fragmented_bytes', 'total_committed_bytes',
             'memory_load_bytes', 'high_memory_load_threshold_bytes',
             'total_available_memory_bytes'}, 'Closed historical GC record')
        need(type(latest['generation']) is int and 0 <= latest['generation'] <= 2 and
             type(latest['concurrent']) is bool and type(latest['compacted']) is bool,
             'Historical GC scalar types')
        for key, value in latest.items():
            if key not in ('generation', 'concurrent', 'compacted'):
                decimal(value)
        return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'bridge', 'assembly', 'hostfxr', 'old-bridge',
                 'aot-world', 'aot-descriptor', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Actual nonoptimized Windows Python')
    assigned_job()
    output = args.output.absolute()
    need(not output.exists(), 'Fresh owned output required')
    inputs = {}
    for role in ('engine', 'bridge', 'assembly', 'hostfxr', 'old_bridge', 'aot_world', 'aot_descriptor'):
        path = getattr(args, role).resolve(strict=True)
        setattr(args, role, path)
        need(not output.is_relative_to(path.parent) and not path.parent.is_relative_to(output),
             'Output must not overlap an immutable input closure')
        inputs[role] = dict(path=str(path), identity=pin(path))
    closures = {role: inventory(getattr(args, role).parent)
                for role in ('bridge', 'assembly', 'old_bridge', 'aot_world')}
    need(args.aot_descriptor.is_relative_to(args.aot_world.parent), 'Descriptor in owned AOT closure')
    output.mkdir(parents=True)
    (output / 'saves').mkdir()
    report = dict(format='poima.managed-memory.native-contract.v1', passed=False,
                  inputs=inputs, input_closures=closures, checkpoints={},
                  whole_state_pairs=[], observations={}, owners=[], errors=[], cleanup_errors=[])
    owner = None
    deadline = time.monotonic() + 240
    started = time.monotonic()
    try:
        owner = MemoryProbe(args.engine, output / 'current', deadline, output / 'world.json',
                            output / 'saves', bridge=args.bridge,
                            assembly=args.assembly, hostfxr=args.hostfxr)
        owner.start()
        need(owner.call('runtime.gameplay.memory', dict(session_id=owner.session))['module'] is None,
             'Unloaded runtime has no memory module')
        owner.load()
        initial = owner.memory(True)
        report['observations']['default_load'] = initial
        before = owner.checkpoint('reads-before', report)
        for _ in range(8):
            observed = owner.memory(True)
            need(observed['module']['data']['retired_contexts_tracked'] ==
                 initial['module']['data']['retired_contexts_tracked'], 'Passive read pruned/added context entries')
        after = owner.checkpoint('reads-after', report)
        same(before, after, report, 'eight passive reads preserve every save field and byte')
        owner.step(3)
        need(owner.game()['module']['values']['Ticks'] == 3, 'Real compiled Tick ran')
        before = owner.checkpoint('reload-before', report)
        prior = owner.memory(True)
        owner.load()
        after = owner.checkpoint('reload-after', report)
        reload_state(before, after)
        reloaded = owner.memory(True)
        need(reloaded['module']['data']['retired_contexts_tracked'] ==
             prior['module']['data']['retired_contexts_tracked'] + 1,
             'Compatible reload retains one additional retired weak context')
        report['observations']['after_reload'] = reloaded
        prior = reloaded
        unchanged_failure(owner, report, 'initialize-failure',
                          lambda: owner.load(game=BAD_INIT, error=-32060))
        rejected = owner.memory(True)
        need(rejected['module']['assembly_sha256'] == prior['module']['assembly_sha256'] and
             rejected['module']['data']['retired_contexts_tracked'] ==
             prior['module']['data']['retired_contexts_tracked'] + 1,
             'Rejected candidate retired separately without replacing selected gameplay')
        report['observations']['after_rejected_candidate'] = rejected
        owner.edit(Mode=1)
        unchanged_failure(owner, report, 'throwing-tick',
                          lambda: owner.fail_step('MemoryProbe intentional Tick failure.'))
        report['observations']['after_rollback'] = owner.memory(True)
        owner.edit(Mode=0)
        before = owner.checkpoint('rejections-before', report)
        owner.call('runtime.gameplay.memory', dict(session_id=owner.session, tick=owner.tick+1), error=-32009)
        for invalid in (True, float(owner.tick), str(owner.tick), None):
            owner.call('runtime.gameplay.memory', dict(session_id=owner.session, tick=invalid), error=-32602)
        owner.call('runtime.gameplay.memory', dict(session_id=fresh()), error=-32030)
        owner.call('runtime.gameplay.memory', dict(session_id=owner.session, extra=True), error=-32602)
        after = owner.checkpoint('rejections-after', report)
        same(before, after, report, 'rejected reads preserve complete native saves')
        report['owners'].append(dict(label='current', **owner.close_recorded()))
        owner = None

        owner = MemoryProbe(args.engine, output / 'older', deadline, output / 'older.json',
                            output / 'saves', bridge=args.old_bridge,
                            assembly=args.assembly, hostfxr=args.hostfxr)
        owner.start()
        owner.load()
        owner.step()
        before = owner.checkpoint('old-before', report)
        report['observations']['older_bridge'] = owner.memory(None, 'bridge_not_negotiated')
        after = owner.checkpoint('old-after', report)
        same(before, after, report, 'older bridge unavailable reads preserve complete state')
        report['owners'].append(dict(label='older-bridge', **owner.close_recorded()))
        owner = None

        stage = output / 'aot-source'
        shutil.copytree(args.aot_world.parent, stage)
        need(exact(inventory(stage), closures['aot_world']), 'Independent complete AOT source copy')
        descriptor = stage / args.aot_descriptor.relative_to(args.aot_world.parent)
        owner = MemoryProbe(args.engine, output / 'aot', deadline,
                            stage / args.aot_world.name, output / 'saves')
        owner.start()
        state = owner.game()
        owner.call('runtime.gameplay.load_native', dict(session_id=owner.session,
            request_id=fresh(), expected_tick=owner.tick, expected_revision=state['revision'],
            expected_structure_revision=state['structure_revision'], descriptor=str(descriptor),
            expected_descriptor_sha256=pin(descriptor)['sha256']))
        before = owner.checkpoint('aot-before', report)
        report['observations']['native_aot'] = owner.memory(False, 'native_aot_not_supported')
        after = owner.checkpoint('aot-after', report)
        same(before, after, report, 'AOT unavailable reads preserve complete state')
        report['owners'].append(dict(label='native-aot', **owner.close_recorded()))
        owner = None
        for role, row in inputs.items():
            need(pin(Path(row['path'])) == row['identity'], 'Selected input changed: ' + role)
        for role, expected in closures.items():
            need(exact(inventory(getattr(args, role).parent), expected), 'Input closure changed: ' + role)
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:
                report['owners'].append(dict(label='failure', **owner.close_recorded()))
            except BaseException:
                report['cleanup_errors'].append(traceback.format_exc())
        report['seconds'] = time.monotonic() - started
        report['passed'] = bool(report['passed'] and not report['errors'] and not report['cleanup_errors'])
        report['terminal_census_before_report'] = census(output)
        write(output / 'report.json', report)
        census(output)
    print(json.dumps({key: report[key] for key in ('passed', 'seconds', 'errors', 'cleanup_errors')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
