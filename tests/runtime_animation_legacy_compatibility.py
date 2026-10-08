#!/usr/bin/env python3
"""Compare real old/new unlayered animation hosts and restore actual old saves.

This provider-free compatibility check uses original ribbon curves without a
second implementation of playback math. It builds no engine/game, patches no
live poses and normalizes only runtime session IDs in complete entity responses.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import WorldClient
from runtime_animation_blend_contract import blend_fixture, command, component, transform, uid
from gltf_fixture import glb


def require(value, message):
    if not value:
        raise AssertionError(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def normalized(value):
    if isinstance(value, dict):
        return {key: normalized(item) for key, item in value.items() if key != 'session_id'}
    if isinstance(value, list):
        return [normalized(item) for item in value]
    return value


class Host:
    def __init__(self, client, label, world, save_root, identities, record, deadline, native):
        self.client, self.label, self.world, self.save_root = client, label, world, save_root
        self.identities, self.record, self.deadline, self.native = identities, record, deadline, native
        self.session, self.tick, self.configuration = None, 0, None

    def rpc(self, method, params=None):
        remaining = self.deadline-time.monotonic()
        require(remaining > 0, 'Compatibility deadline exceeded')
        row = dict(host=self.label, method=method, params=params or {})
        self.record['calls'].append(row)
        try:
            result = self.client.call(method, params or {}, timeout=min(30, remaining))
        except BaseException as failure:
            row['error'] = dict(type=type(failure).__name__, message=str(failure))
            raise
        row['result'] = result
        return result

    def configure(self):
        self.save_root.mkdir(exist_ok=True)
        self.configuration = self.rpc('save.configure', dict(request_id=uuid.uuid4().hex,
            expected_generation=0, root=self.native(self.save_root)))['generation']
        require(self.configuration == 1, 'Unexpected fresh save configuration generation')

    def start(self):
        if self.session is not None:
            self.rpc('runtime.stop', dict(session_id=self.session))
        self.session, self.tick = uuid.uuid4().hex, 0
        self.rpc('runtime.start', dict(session_id=self.session, revision=1))

    def step(self, ticks, animations=()):
        result = self.rpc('runtime.step', dict(session_id=self.session, request_id=uuid.uuid4().hex,
            expected_tick=self.tick, ticks=ticks, animations=list(animations)))
        self.tick += ticks
        require(result['tick'] == self.tick, 'Step did not reach the exact requested tick')

    def snapshot(self):
        # All current fixture entities, with every runtime.entity field retained.
        return normalized(dict(tick=self.tick, entities=[self.rpc('runtime.entity',
            dict(session_id=self.session, tick=self.tick, id=identity)) for identity in self.identities]))

    def write(self, slot, animation_version):
        result = self.rpc('save.write', dict(request_id=uuid.uuid4().hex,
            configuration_generation=self.configuration, slot=slot, expected_generation=0,
            session_id=self.session, expected_tick=self.tick, expected_gameplay_revision=0))
        require(result['generation'] == 1 and not result['recovered'] and not result['cleanup_pending'],
                'Fresh snapshot save required recovery or incomplete cleanup')
        files = list((self.save_root/('slot-'+slot)).glob('p-*.bin'))
        require(len(files) == 1, 'Fresh slot does not contain exactly one durable payload')
        payload = files[0]
        require(payload.stat().st_size == result['bytes'] and sha256(payload) == result['sha256'],
                'Returned save hash/byte count differs from actual durable payload')
        state = json.loads(payload.read_bytes())
        animation = state['snapshot']['payload']['animation']
        require(animation['version'] == animation_version and
                all('layers' not in rig for rig in animation['rigs']),
                'Unlayered snapshot changed its legacy nested animation version/shape')
        return dict(sha256=result['sha256'], bytes=result['bytes'], animation_version=animation_version)

    def restore(self, slot):
        session = uuid.uuid4().hex
        result = self.rpc('save.load', dict(request_id=uuid.uuid4().hex,
            configuration_generation=self.configuration, slot=slot, expected_generation=1, revision=1,
            expected_session_id=self.session, expected_tick=self.tick if self.session is not None else None,
            expected_gameplay_revision=0 if self.session is not None else None,
            new_session_id=session, gameplay=None))
        require(not result['recovered'] and result['selected_generation'] == 1,
                'Restore did not select the original intact snapshot')
        self.session, self.tick = session, result['tick']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'legacy-binary', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    require(60 <= args.timeout <= 1200, 'Timeout must be 60..1200 seconds')
    binary, legacy = args.binary.resolve(), args.legacy_binary.resolve()
    require(binary.is_file() and legacy.is_file(), 'Both real binary inputs must exist')
    output = args.output.resolve()
    require(not output.exists(), 'Output must be new; preserve prior evidence')
    output.mkdir(parents=True)
    deadline = time.monotonic()+args.timeout
    sources = [Path(__file__).resolve(), ROOT/'tests/runtime_animation_blend_contract.py',
        ROOT/'tests/animation_fixture.py', ROOT/'tests/gltf_fixture.py']
    source_hashes = {str(path.relative_to(ROOT)): sha256(path) for path in sources}
    record = dict(passed=False, calls=[], processes=[], scenarios={}, source_sha256=source_hashes,
        binary_sha256=sha256(binary), legacy_binary_sha256=sha256(legacy),
        normalization='Remove only keys named session_id recursively; retain all entity fields, ticks, poses, velocities, playback and transitions.',
        limits=['Compatibility fixture, not an independent animation-math reference or performance benchmark.',
            'No GPU, installed desktop, physical input, C# service or console qualification.',
            'Owned process cancellation/cleanup may exceed the request deadline.'])
    require(record['binary_sha256'] != record['legacy_binary_sha256'],
            'Supply distinct real old/new binary images, not the same host twice')
    hosts = []

    def native(path):
        if args.windows_interop and os.name != 'nt':
            return subprocess.check_output(['wslpath', '-w', str(Path(path).resolve())],
                                           text=True, timeout=10).strip()
        return str(Path(path).resolve())

    def open_host(label, executable, world, saves, identities):
        require(deadline > time.monotonic(), 'Deadline exceeded before opening host')
        client = WorldClient.open(str(executable), native(world), close_timeout=10)
        host = Host(client, label, world, saves, identities, record, deadline, native)
        hosts.append(host)
        return host

    def copy_world(source, target):
        shutil.copyfile(source, target)
        shutil.copytree(Path(str(source)+'.assets'), Path(str(target)+'.assets'))

    def compare_state(selected, scenario, label):
        snapshots = [host.snapshot() for host in selected]
        require(all(snapshot == snapshots[0] for snapshot in snapshots[1:]),
                scenario+' complete entities differ at '+label)
        record['scenarios'][scenario]['poses'].append(dict(label=label, hosts=[host.label for host in selected],
            tick=selected[0].tick, snapshot=snapshots[0]))
        return snapshots[0]

    def compare_saves(selected, scenario, suffix, version):
        # Fresh owners read the original old save root. Give each new write a
        # distinct slot so identical payloads do not race on slot generation.
        results = [host.write(scenario+'-'+suffix + ('' if suffix == 'checkpoint' else '-'+host.label), version)
                   for host in selected]
        require(all(result == results[0] for result in results[1:]),
                scenario+' complete durable payload bytes differ at '+suffix)
        record['scenarios'][scenario]['saves'].append(dict(label=suffix, hosts=[host.label for host in selected],
            tick=selected[0].tick, **results[0]))
        return results[0]

    try:
        old_world, new_world = output/'old-world.json', output/'new-world.json'
        old = open_host('old', legacy, old_world, output/'old-saves', [])
        document, blob = blend_fixture()
        asset_source = output/'ribbon.glb';asset_source.write_bytes(glb(document, blob))
        record['asset_source_sha256'] = sha256(asset_source)
        asset = old.rpc('asset.import', dict(source=native(asset_source)))['asset']
        asset_source.unlink()
        rig = uid(100)
        ops = [dict(op='asset.instantiate', id=rig, name='Compatibility ribbon', asset=asset),
               dict(op='asset.instantiate', id=uid(200), name='Independent ribbon', asset=asset)]
        for identity, position, motion, half in [(1,(0,-.5,0),'static',(20,.5,20)),
                (2,(2,6,0),'dynamic',(.25,.25,.25))]:
            ops += [dict(op='entity.create', id=uid(identity), name='Body '+str(identity)),
                component(uid(identity),'Transform',transform(position)),
                component(uid(identity),'BoxCollider',dict(half_extents=list(half),motion=motion,mass=1,
                    friction=.5,restitution=0))]
        old.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=ops))
        authored = old_world.read_bytes();authored_document = json.loads(authored)
        identities = sorted(authored_document['entities']);old.identities = identities
        require(len(identities) >= 10, 'Both complete ribbon instances/body fixtures must exist')
        record['authored_sha256'] = hashlib.sha256(authored).hexdigest()
        record['authored_world_id'] = authored_document['world_id']
        record['entity_ids'] = identities
        copy_world(old_world,new_world)
        new = open_host('new',binary,new_world,output/'new-saves',identities)
        require(new_world.read_bytes() == authored, 'New host does not use byte-identical authored content')
        old.configure();new.configure()
        for scenario,version in [('crossfade',1),('inertial',2)]:
            record['scenarios'][scenario] = dict(poses=[],saves=[],checks={})
            for host in (old,new):
                host.start()
                host.step(30,[command(rig,time=.25,speed=1.25,loop=False)])
            compare_state([old,new],scenario,'source-playback')
            extra = dict(blend_ticks=120)
            if scenario == 'inertial':
                extra['transition_mode'] = 'inertial'
            for host in (old,new):
                host.step(15,[command(rig,clip=1,time=.25,speed=2,loop=False,**extra)])
            compare_state([old,new],scenario,'active-transition-15')
            for host in (old,new):host.step(15)
            checkpoint = compare_state([old,new],scenario,'active-checkpoint')
            saved = compare_saves([old,new],scenario,'checkpoint',version)
            continued_world = output/(scenario+'-continued-world.json')
            copy_world(old_world,continued_world)
            continued = open_host(scenario+'-fresh-continuation',binary,continued_world,old.save_root,identities)
            continued.configure();continued.restore(scenario+'-checkpoint')
            require(continued.snapshot() == checkpoint, 'Old payload fresh restore differs before continuation')
            require(continued.write(scenario+'-fresh-restored',version) == saved,
                    'Fresh restore changed the complete old payload bytes')
            selected = [old,new,continued]
            for index,ticks in enumerate((1,7,13)):
                for host in selected:host.step(ticks)
                compare_state(selected,scenario,'continuation-'+str(index))
                compare_saves(selected,scenario,'continued-'+str(index),version)
            record['scenarios'][scenario]['checks']['old_payload_fresh_exact_continuation'] = True
            # Return the still-running owners to the ORIGINAL checkpoint, then
            # restore another fresh owner and interrupt before any new tick can
            # synthesize history missing from that old durable payload.
            old.restore(scenario+'-checkpoint');new.restore(scenario+'-checkpoint')
            interrupted_world = output/(scenario+'-interrupted-world.json')
            copy_world(old_world,interrupted_world)
            interrupted = open_host(scenario+'-fresh-immediate-interruption',binary,interrupted_world,old.save_root,identities)
            interrupted.configure();interrupted.restore(scenario+'-checkpoint')
            selected = [old,new,interrupted]
            require(all(host.snapshot() == checkpoint for host in selected), 'Second restore differs from original checkpoint')
            replacement = dict(blend_ticks=60)
            if scenario == 'inertial':replacement['transition_mode'] = 'inertial'
            for index,ticks in enumerate((1,7,13,29,10,7)):
                animations = [command(rig,clip=0,time=1.25,playing=False,loop=False,**replacement)] if index == 0 else []
                for host in selected:host.step(ticks,animations)
                compare_state(selected,scenario,'immediate-interruption-'+str(index))
                compare_saves(selected,scenario,'interrupted-'+str(index),version)
            record['scenarios'][scenario]['checks']['old_payload_fresh_immediate_reinterruption'] = True
        require(all(host.world.read_bytes() == authored for host in hosts), 'Compatibility replay changed authored content')
        require(sha256(binary) == record['binary_sha256'] and sha256(legacy) == record['legacy_binary_sha256'],
                'A supplied binary changed during qualification')
        require(all(sha256(ROOT/name) == expected for name,expected in source_hashes.items()),
                'A preserved fixture/runner source changed during qualification')
        record['passed'] = True
    except BaseException as failure:
        record['failure'] = dict(type=type(failure).__name__,message=str(failure))
        raise
    finally:
        for host in reversed(hosts):
            error = None
            try:host.client.close()
            except BaseException as failure:error = str(failure)
            transport = host.client.transport
            row = dict(host=host.label,pid=transport.process_id,exit_code=transport.returncode,
                closed=host.client.closed,cleanup_error=error,stderr=transport.stderr_tail)
            record['processes'].append(row)
            if error is not None or transport.returncode != 0 or not host.client.closed:record['passed'] = False
        (output/'evidence.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print(output/'evidence.json')
    require(record['passed'], 'Owned process cleanup failed')
    print(json.dumps(dict(passed=True,scenarios=list(record['scenarios']),rpc_calls=len(record['calls']),
        clean_owned_processes=len(record['processes']))))


if __name__ == '__main__':
    main()
