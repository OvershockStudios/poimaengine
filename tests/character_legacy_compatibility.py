#!/usr/bin/env python3
"""Compare real prior/current capsule input hosts and original save continuation."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import uuid
from components_gameplay_contract import Session, sha, uid
from character_gameplay_contract import fixture, normalized


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'legacy-binary', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    evidence = dict(passed=False, calls=[], checks=[], images=dict(
        old=sha(args.legacy_binary), current=sha(args.binary)), harness_sha256=sha(__file__))
    assert evidence['images']['old'] != evidence['images']['current']

    def native(path):
        path = str(Path(path).resolve())
        return subprocess.check_output(['wslpath', '-w', path], text=True).strip() if args.windows_interop else path

    args.native = native
    hosts = []
    try:
        old_args = argparse.Namespace(**vars(args))
        old_args.binary = args.legacy_binary
        old = Session(old_args, out/'old.json', evidence)
        hosts.append(old)
        old.rpc('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=fixture(actor=False)))
        authored = old.world.read_bytes()
        shutil.copyfile(old.world, out/'current.json')
        current = Session(args, out/'current.json', evidence)
        hosts.append(current)
        saves = out/'saves'
        saves.mkdir()
        for host in hosts:
            host.start(1)
            host.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))

        def snapshot(host):
            return normalized(dict(tick=host.tick, entities=[host.rpc('runtime.entity',
                dict(session_id=host.session, tick=host.tick, id=uid(identity))) for identity in (1, 2, 100, 101)]))

        sequences = [(60, None), (1, ((0, 1), (12, 9), True)),
            (30, ((1, 1), (-5, 3), False)), (120, ((0, 1), (0, 0), False)),
            (1, ((0, 0), (0, 0), True)), (90, None),
            (180, ((-1, 0), (27, -12), False)), (60, None)]
        poses = []
        for ticks, value in sequences:
            for host in hosts:
                inputs = [] if value is None else [dict(entity=uid(100), move=list(value[0]), look=list(value[1]), jump=value[2])]
                host.rpc('runtime.step', dict(session_id=host.session, request_id=uuid.uuid4().hex,
                    expected_tick=host.tick, ticks=ticks, inputs=inputs))
                host.tick += ticks
            states = [snapshot(host) for host in hosts]
            assert states[0] == states[1], dict(tick=old.tick, old=states[0], current=states[1])
            poses.append(dict(tick=old.tick, sha256=hashlib.sha256(json.dumps(states[0], sort_keys=True).encode()).hexdigest()))
        evidence['poses'] = poses
        evidence['checks'].append('Real old/current hosts match every complete entity response over held inputs, first-frame look/jump, diagonal movement, omission, ground/wall contact and eight chunks')
        saved = []
        for label, host in zip(('old', 'current'), hosts):
            saved.append(host.rpc('save.write', dict(request_id=uuid.uuid4().hex,
                configuration_generation=1, slot=label, expected_generation=0,
                session_id=host.session, expected_tick=host.tick, expected_gameplay_revision=0)))
        assert saved[0]['sha256'] == saved[1]['sha256'] and saved[0]['bytes'] == saved[1]['bytes']
        old_payload = next((saves/'slot-old').glob('p-*.bin'))
        assert sha(old_payload) == saved[0]['sha256']
        current.rpc('runtime.stop', dict(session_id=current.session))
        current.close()
        hosts.pop()
        current = Session(args, out/'current.json', evidence)
        hosts.append(current)
        current.rpc('save.configure', dict(request_id=uuid.uuid4().hex, expected_generation=0, root=native(saves)))
        fresh = uuid.uuid4().hex
        restored = current.rpc('save.load', dict(request_id=uuid.uuid4().hex,
            configuration_generation=1, slot='old', expected_generation=1, revision=1,
            expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
            new_session_id=fresh, gameplay=None))
        current.session = fresh
        current.tick = restored['tick']
        assert snapshot(current) == snapshot(old)
        for host in hosts:
            host.rpc('runtime.step', dict(session_id=host.session, request_id=uuid.uuid4().hex,
                expected_tick=host.tick, ticks=60, inputs=[dict(entity=uid(100), move=[0, 1], look=[15, 0], jump=True)]))
            host.tick += 60
        assert snapshot(current) == snapshot(old)
        assert old.world.read_bytes() == authored and current.world.read_bytes() == authored
        evidence['saved_payload_sha256'] = saved[0]['sha256']
        evidence['checks'].append('Complete old/current durable payload bytes match; a fresh current host loads the original old payload and continues identically without altering authored bytes')
        evidence['passed'] = True
    finally:
        for host in hosts:
            host.close()
        evidence['rpc_count'] = len(evidence['calls'])
        (out/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
        print(out/'evidence.json')


if __name__ == '__main__':
    main()
