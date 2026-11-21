#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify compiled gameplay rain, exact rollback and fresh-process continuation."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import traceback

from input_actions_gameplay_contract import exact
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid
from world_wetness_contract import PROP, Phase, Reference, WetnessContract, motion


TYPE = 'Poima.Tests.ManagedWetnessGame'
IDENTITY = 'poima.test.managed-surface-wetness'
FEATURES = {'baseline_v7', 'surface_wetness_v1'}
FIRST = 58
CHECKPOINT = 2
# Seed 6, Storm-only 60-second slots, no transition. These are the independently
# integrated and rounded depths at seconds 58..62 across the slot boundary.
RAIN_NM = (5932, 5932, 7474, 7474, 7474)
PROP_OPEN = [.65, .2, 1.55]
PROP_COVERED = [1.1, .2, 1.55]
PROP_TARGETS = {2: PROP_COVERED, 3: PROP_OPEN, 4: PROP_COVERED, 5: PROP_OPEN}


def initial_reference():
    result = Reference()
    result.positions[PROP] = PROP_OPEN
    return result


class ManagedWetnessContract(WetnessContract):
    def configure_gameplay(self):
        if not self.args.native_descriptor:
            host, bridge, assembly = self.args.hostfxr, self.args.bridge, self.args.assembly
            sdk = bridge.with_name('Poima.Gameplay.dll')
            assert file_hash(sdk) == file_hash(assembly.with_name(sdk.name))
            self.image_hash = file_hash(assembly)
            self.measurements['gameplay_artifact'] = dict(
                hostfxr_sha256=file_hash(host), bridge_sha256=file_hash(bridge),
                sdk_sha256=file_hash(sdk), assembly_sha256=self.image_hash,
                weather_dependency_sha256=file_hash(assembly.with_name('Poima.Weather.dll')))
            return dict(hostfxr=self.native(host), bridge=self.native(bridge),
                        assembly=self.native(assembly), type=TYPE)

        descriptor = self.args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert (metadata['type'], metadata['identity'], metadata['call_version'], metadata['call_bytes'],
                metadata['services_version'], metadata['minimum_services_bytes']) == (TYPE, IDENTITY, 1, 80, 7, 576)
        assert set(metadata['required_features']) == FEATURES and len(metadata['required_features']) == len(FEATURES)
        digest = file_hash(descriptor)
        self.measurements['gameplay_artifact'] = dict(descriptor_sha256=digest, files=metadata['files'])
        frozen = self.project/'gameplay'
        for record in metadata['files']:
            path = (descriptor.parent/record['path']).resolve(strict=True)
            assert path.is_relative_to(descriptor.parent) and file_hash(path) == record['sha256'], record
            assert path.stat().st_size == record['size'], record
            if self.args.runtime_root:
                target = frozen/record['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        self.image_hash = file_hash(descriptor.parent/metadata['library'])
        assert self.image_hash in {record['sha256'] for record in metadata['files']}
        if self.args.runtime_root:
            shutil.copyfile(descriptor, frozen/'native-gameplay.json')
            assert file_hash(frozen/'native-gameplay.json') == digest
            manifest = json.loads(self.manifest.read_bytes())
            manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
            self.manifest.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        return dict(descriptor=self.native(descriptor), expected_descriptor_sha256=digest)

    def start_game(self, client, revision, saves, game, start):
        session = self.initialize(client, revision, uid(start), saves, game)
        if game is None:
            method = 'runtime.gameplay.load_native' if self.args.native_descriptor else 'runtime.gameplay.load'
            loaded = client.call(method, dict(session_id=session, request_id=uid(start+1), expected_tick=0,
                                              expected_revision=0, **self.config))
            assert loaded['revision'] == 1 and loaded['replayed'] is False, loaded
        return session

    def state(self, client, session, model, label=None):
        tick = model.tick
        info = client.call('runtime.gameplay.inspect', dict(session_id=session, tick=tick, include_schema=True))
        assert exact([info['session_id'], info['tick'], info['structure_revision']], [session, tick, 0]), info
        module = info['module']
        backend = 'native_aot' if self.args.native_descriptor else 'coreclr'
        assert (module['backend'], module['type'], module['schema']['identity']) == (backend, TYPE, IDENTITY), module
        assert module['assembly_sha256'] == self.image_hash, module
        assert exact(module['values'], dict(NextQuantum=str(FIRST+tick))), module
        if self.args.native_descriptor:
            assert exact(module['native_diagnostics'],
                         dict(dynamic_code_supported=False, dynamic_code_compiled=False)), module
        wet = self.verify_reading(client, session, model)
        expected_cursor = FIRST+tick if tick else None
        assert exact(wet['next_quantum'], expected_cursor), wet
        observation = dict(revision=info['revision'], tick=tick, gameplay=module['values'], wet=wet)
        if label:
            self.measurements.setdefault('observations', {})[label] = observation
        return observation

    def step_game(self, client, session, before, request, *, retry=False):
        tick = before.tick+1
        params = self.guard(session, before.tick, request)
        target = PROP_TARGETS.get(tick)
        targets = ((PROP, target),) if target is not None else ()
        if target is not None:
            params['motions'] = [motion(PROP, target, 1)]
        result = client.call('runtime.step', params)
        assert exact([result['current_session_id'], result['current_tick'], result['stepped']],
                     [session, tick, 1]), result
        assert result['runtime_replaced'] is False and result['replayed'] is False, result
        if retry:
            assert exact(client.call('runtime.step', params), result | dict(replayed=True))

        after = before.copy()
        if after.next_quantum is None:
            after.next_quantum = FIRST
        after.finish(Phase(f'compiled rain at tick {tick}', (RAIN_NM[tick-1],), targets=targets))
        return after

    def archive_game(self, client, saves, session, model, state, slot, request):
        receipt = client.call('save.write', dict(request_id=uid(request), configuration_generation=1,
            slot=slot, expected_generation=0, session_id=session, expected_tick=model.tick,
            expected_gameplay_revision=state['revision'], expected_structure_revision=0))
        path, = (saves/('slot-'+slot)).glob('p-*.bin')
        raw = path.read_bytes()
        digest = file_hash(path)
        assert receipt['sha256'] == digest and receipt['bytes'] == len(raw), receipt
        payload = json.loads(raw)['snapshot']['payload']
        assert exact(payload['wetness'], model.saved()), payload['wetness']
        assert exact(payload['gameplay']['values'], state['gameplay']), payload['gameplay']
        assert exact([payload['tick'], payload['gameplay_revision']], [model.tick, state['revision']]), payload
        self.measurements.setdefault('archives', {})[slot] = dict(tick=model.tick, sha256=digest, bytes=len(raw))
        return raw

    def mix_refusal(self, client, saves, session, model, state, checkpoint_bytes):
        params = self.guard(session, model.tick, 2310) | dict(
            motions=[motion(PROP, PROP_OPEN, 1)],
            wetness_dose=dict(first_quantum=model.next_quantum, rain_nm=[1000]))
        refusal = self.reject(client, 'runtime.wetness.step', params, -32040)
        assert 'both caller and gameplay doses' in refusal['message'], refusal
        assert exact(self.state(client, session, model), state)
        assert self.archive_game(client, saves, session, model, state, 'after-mixed-dose', 2311) == checkpoint_bytes
        self.measurements['mixed_dose_refusal'] = refusal

    def verify_gameplay(self, revision, game=None):
        saves = self.root/'External Compiled Wetness Saves'
        saves.mkdir()
        with self.owner('warm compiled wetness', game=game, saves=saves) as (client, warm):
            session = self.start_game(client, revision, saves, game, 2300)
            model = initial_reference()
            self.state(client, session, model, 'warm initial')
            warm_states = {}
            for index in range(len(RAIN_NM)):
                model = self.step_game(client, session, model, 2320+index, retry=index == 0)
                state = self.state(client, session, model, f'warm tick {model.tick}')
                warm_states[model.tick] = state
                if model.tick == CHECKPOINT:
                    checkpoint_model = model.copy()
                    checkpoint_bytes = self.archive_game(client, saves, session, model, state, 'checkpoint', 2340)
                    self.mix_refusal(client, saves, session, model, state, checkpoint_bytes)
            final = self.archive_game(client, saves, session, model, state, 'warm-final', 2341)

        with self.owner('cold compiled wetness', game=game, saves=saves) as (client, cold):
            previous = self.start_game(client, revision, saves, game, 2350)
            fresh = self.state(client, previous, initial_reference())
            inspection = client.call('save.inspect', dict(slot='checkpoint'))
            selected = inspection['selected']
            archive = self.measurements['archives']['checkpoint']
            assert inspection['recovered'] is False and inspection['manifest_recovered'] is False, inspection
            assert exact(selected, dict(verified=True, generation=1, sha256=archive['sha256'], bytes=archive['bytes'])), inspection
            session = uid(2352)
            loaded = client.call('save.load', dict(request_id=uid(2353), configuration_generation=1,
                slot='checkpoint', expected_generation=1, revision=revision, expected_session_id=previous,
                expected_tick=0, expected_gameplay_revision=fresh['revision'], expected_structure_revision=0,
                new_session_id=session))
            assert exact([loaded['session_id'], loaded['tick'], loaded['structure_revision'], loaded['source_stale']],
                         [session, CHECKPOINT, 0, False]), loaded
            state = self.state(client, session, checkpoint_model, 'restored checkpoint')
            assert exact(state, warm_states[CHECKPOINT]), state
            restored = self.archive_game(client, saves, session, checkpoint_model, state, 'restored-checkpoint', 2354)
            assert restored == checkpoint_bytes
            self.measurements['verified_restore'] = dict(selected=selected, loaded=loaded)
            model = checkpoint_model
            for index in range(CHECKPOINT, len(RAIN_NM)):
                model = self.step_game(client, session, model, 2360+index)
                state = self.state(client, session, model, f'cold tick {model.tick}')
                assert exact(state, warm_states[model.tick]), state
            assert self.archive_game(client, saves, session, model, state, 'cold-final', 2370) == final
        assert warm['pid'] != cold['pid'], (warm, cold)

    def run(self):
        revision = self.author()
        positioned = self.world_call('world.transact', request_id=uid(2290), base_revision=revision,
                                     ops=[component(PROP, 'Transform', transform(PROP_OPEN))])
        assert positioned['committed'] and positioned['revision'] == revision+1, positioned
        revision = positioned['revision']
        self.config = self.configure_gameplay()
        self.measurements['dose_oracle'] = dict(first_quantum=FIRST, rain_nm=RAIN_NM)
        if not self.args.runtime_root:
            self.verify_gameplay(revision)
            return

        output = self.root/'Initial export'
        info = success(json.loads(self.cli('project', 'inspect', self.native(self.manifest))))
        assert info['gameplay']['requirements']['minimum_services_bytes'] == 576, info
        success(json.loads(self.cli('project', 'build', self.native(self.manifest), '--output', self.native(output),
                                   '--runtime', self.native(self.args.runtime_root))))
        bundle = self.root/'Relocated Compiled Wetness Game'
        detached = self.root/'Unavailable source'
        output.rename(bundle)
        self.project.rename(detached)
        assert not self.project.exists() and not output.exists()
        frozen, authored = tree_hashes(bundle), tree_hashes(detached)
        game = bundle/'game.json'
        assert self.image_hash in frozen.values()
        self.measurements['bundle'] = dict(manifest_sha256=file_hash(game), inventory=frozen, detached_source=authored)
        self.verify_gameplay(revision, game)
        assert tree_hashes(bundle) == frozen and tree_hashes(detached) == authored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor', 'runtime-root'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    managed = (args.hostfxr, args.bridge, args.assembly)
    if (args.native_descriptor and any(managed)) or (not args.native_descriptor and not all(managed)):
        parser.error('Supply a Native AOT descriptor or the CoreCLR hostfxr, bridge and assembly.')
    if args.runtime_root and not args.native_descriptor:
        parser.error('Standalone export requires --native-descriptor.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='managed-wetness-', dir=args.output.resolve()))
    test = ManagedWetnessContract(args, root)
    result = dict(format='poima.managed-wetness-contract.v1', passed=False, standalone=bool(args.runtime_root),
                  backend='native_aot' if args.native_descriptor else 'coreclr', calls=test.calls,
                  measurements=test.measurements, harness_sha256=file_hash(Path(__file__)),
                  executable_sha256=file_hash(args.binary))
    try:
        test.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
