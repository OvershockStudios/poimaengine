#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare committed solar water scattering with independent optical path integration."""
import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import traceback

from height_fog_capture import environment
from solar_lighting_contract import IDENTITY, VALIDATION_FAILURES, VALIDATION_LAYER, solar_reference
from water_offshore_capture import AMBIENT, CAMERAS, FOG, GRAZING_PROBES, OffshoreContract, PROBES, compose
from water_refraction_capture import LENS
from world_particles_contract import near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, read_inventory, tree_hashes
from world_water_contract import CELLS, component, rpc, success, uid


STRENGTH = .75
NIGHT = 5
SUBMERGED = (uid(8130), (17.2, 4.4, -3.15), 12, True)
VIEWS = dict(shore=CAMERAS['shore'], ocean=CAMERAS['ocean'], submerged=SUBMERGED)
FEATURE = 'water_solar_scattering'
DEPENDENCY = 'needs_' + FEATURE


class SolarScatteringContract(OffshoreContract):
    def author(self):
        revision = super().author()
        light = environment(FOG) | dict(ambient=AMBIENT, shadow_resolution=256)
        light['sky']['sun'] = uid(5)
        camera, eye, pitch, _ = SUBMERGED
        half = math.radians(pitch) / 2
        t = math.sqrt(.5)
        revision = self.apply(revision, 8131,
            component(uid(4), 'LightingEnvironment', light),
            component(uid(5), 'Light', dict(kind='directional', color=[1, 1, 1], intensity=0, enabled=True)),
            dict(op='entity.create', id=camera, name='Submerged solar scattering camera'),
            component(camera, 'Transform', transform(eye, [math.sin(half)*t, t*math.cos(half),
                                                          -math.sin(half)*t, t*math.cos(half)])),
            component(camera, 'Camera', LENS))
        self.definition['revision'] = revision
        return revision

    def cook(self, definition, version):
        planned = self.world_call('world.water.plan', **definition)
        pin = planned['plan_sha256']
        assert self.world_call('world.water.cook', **definition, expected_plan_sha256=pin) == planned
        asset = self.assets / (pin + '.pwater')
        assert file_hash(asset) == pin
        assert json.loads(asset.read_bytes())['version'] == version
        return planned

    def prepare(self):
        schema = self.world_call('world.describe', view='method', name='world.water.plan')
        assert '"solar_scattering"' in json.dumps(schema)
        definition = self.definition
        optics = definition['optics']
        before, assets = self.world.read_bytes(), read_inventory(self.assets)
        for bad in (True, None, '0.5', -.01, 1.01):
            invalid = definition | dict(optics=optics | dict(solar_scattering=bad))
            planned = rpc('world.water.plan', **invalid)
            cooked = rpc('world.water.cook', **invalid, expected_plan_sha256='0'*64)
            self.cases(('invalid_plan', planned, -32602), ('invalid_cook', cooked, -32602))
        assert self.world.read_bytes() == before and read_inventory(self.assets) == assets
        legacy = self.cook(definition, 12)
        assets = read_inventory(self.assets)
        zero = definition | dict(optics=optics | dict(solar_scattering=0))
        assert self.cook(zero, 12) == legacy and read_inventory(self.assets) == assets
        active = self.cook(definition | dict(optics=optics | dict(solar_scattering=STRENGTH)), 15)
        assert active['plan_sha256'] != legacy['plan_sha256']
        assert active['optics']['solar_scattering'] == STRENGTH
        assert self.world.read_bytes() == before
        self.measurements['cooked'] = dict(legacy=legacy,active=active)
        return legacy, active

    def artifact(self):
        descriptor = self.args.native_descriptor.resolve(strict=True)
        metadata = json.loads(descriptor.read_bytes())
        assert metadata['identity'] == IDENTITY
        assert metadata['minimum_services_bytes'] == 584
        assert set(metadata['required_features']) == {'baseline_v7', 'solar_lighting_v1'}
        for row in metadata['files']:
            source = (descriptor.parent / row['path']).resolve(strict=True)
            assert source.is_relative_to(descriptor.parent)
            assert source.stat().st_size == row['size'] and file_hash(source) == row['sha256']
            if self.args.runtime_root:
                target = self.project / 'gameplay' / row['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
        pin = file_hash(descriptor)
        library = file_hash(descriptor.parent / metadata['library'])
        self.measurements['gameplay'] = dict(descriptor_sha256=pin, library_sha256=library)
        if self.args.runtime_root:
            shutil.copyfile(descriptor, self.project / 'gameplay/native-gameplay.json')
            manifest = json.loads(self.manifest.read_bytes())
            manifest.update(version=2, gameplay=dict(descriptor='gameplay/native-gameplay.json', values={}))
            self.manifest.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        return dict(descriptor=self.native(descriptor), expected_descriptor_sha256=pin)

    def render(self, client, session, tick, tag, name, active, state):
        path = self.root / f'{tag}-{tick}-{name}.bmp'
        probes = GRAZING_PROBES if name == 'shore' else PROBES
        response = client.call('runtime.capture', dict(session_id=session, tick=tick, camera=VIEWS[name][0],
            path=self.native(path), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1], gpu=self.args.gpu, samples=1,
            lighting_path=self.args.lighting_path, profile=True, capture_frames=2,
            scene_product_probes=[dict(x=x,y=y) for x,y in probes]))
        assert response['hardware'] and response['capture_written'] and response['frames_presented'] == 2
        assert not response['nvrhi_errors'] and response['tick'] == tick
        diag = response['render_diagnostics']
        optics = diag['water_optics']
        assert optics['active'] and optics['offshore'] and diag['height_fog']['active']
        expected_lighting = solar_reference(tick)
        gain = 1 if expected_lighting is None else expected_lighting['sky_gain']
        scale = 1 - STRENGTH*(1 - gain) if active else 1
        expected = dict(strength=STRENGTH, sky_gain=gain, radiance_scale=scale) if active else None
        assert optics.get('solar_scattering') == expected, optics
        if name == 'submerged':
            assert optics['camera_submersion_m'] > .1
        elif name == 'ocean':
            assert optics['camera_submersion_m'] == 0
        rows = [compose(pixel, probe, tick, VIEWS[name], scatter_scale=scale, ambient=state['ambient'])
                | dict(raw_hdr=probe['raw_hdr'], depth=probe['depth'])
                for pixel,probe in zip(probes,diag['scene_products']['probes'],strict=True)]
        for row in rows:
            near_vector(row['actual'], row['expected'], tolerance=.003)
        captured = dict(sha256=file_hash(path), samples=rows, solar_scattering=expected, adapter=response['gpu'])
        self.measurements[f'{tag}-{tick}-{name}'] = captured
        return captured

    def observation(self, client, session, tick, active):
        lighting = client.call('runtime.lighting', dict(session_id=session, tick=tick))
        expected = solar_reference(tick)
        actual = lighting.get('solar_lighting')
        if expected is None:
            assert actual is None
        else:
            near_vector(actual['toward_sun'], expected['toward_sun'], tolerance=1e-6)
            assert actual['world_milliseconds'] == expected['world_milliseconds']
            assert actual['sky_gain'] == expected['sky_gain']
            assert lighting['ambient'] == actual['ambient'] == expected['ambient']
        water = client.call('runtime.water.inspect', dict(session_id=session,tick=tick,cells=CELLS))
        assert water['optics'].get('solar_scattering') == (STRENGTH if active else None)
        assert [row['depth_m'] for row in water['cells']] == self.definition['initial_depths_m']
        return dict(ambient=lighting['ambient'], solar=actual, cells=water['cells'], volume_m3=water['volume_m3'])

    def exercise(self, revision, plan, tag, active, binding):
        pin = plan['plan_sha256']
        revision = self.apply(revision, 8132 + active, dict(op='water.set',asset=pin))
        dependencies = self.world_call('world.dependencies')
        project = success(json.loads(self.cli('project','inspect',self.native(self.manifest))))
        assert dependencies.get(DEPENDENCY,False) is active and project.get(DEPENDENCY,False) is active
        session = uid(8134 + active)
        result = {}
        with self.owner(tag) as (client,_):
            client.call('runtime.start',self.start(session,revision)['params'])
            if binding:
                loaded = client.call('runtime.gameplay.load_native',dict(session_id=session, request_id=uid(8136),
                    expected_tick=0,expected_revision=0,**binding))
                assert loaded['revision'] == 1 and loaded['module']['schema']['identity'] == IDENTITY
            previous = 0
            for tick in (0,2,NIGHT) if binding else (0,):
                if tick:
                    client.call('runtime.step',self.step(session,previous,tick-previous,8137+tick)['params'])
                state = self.observation(client,session,tick,active)
                result[tick] = dict(state=state)
                if self.args.gpu is not None:
                    names = VIEWS if tick == NIGHT else ('ocean',)
                    result[tick]['frames'] = {name:self.render(client,session,tick,tag,name,active,state) for name in names}
                previous = tick
            if active:
                self.configure(client)
                client.call('save.write',dict(request_id=uid(8143), configuration_generation=1, slot='checkpoint',
                    expected_generation=0,session_id=session,expected_tick=previous,expected_gameplay_revision=1 if binding else 0))
        return revision,result

    def restore(self, revision, expected, binding, game=None):
        with self.owner('relocated cold' if game else 'cold',
                        game=game,saves=self.root/'saves' if game else None) as (client,_):
            if not game:
                self.configure(client)
                active = None
            else:
                active = client.call('runtime.status')['session_id']
            session = uid(8144)
            args = dict(request_id=uid(8145),configuration_generation=1,slot='checkpoint',expected_generation=1,
                revision=revision,expected_session_id=active,expected_tick=0 if active else None,
                expected_gameplay_revision=1 if active else None,new_session_id=session)
            # For a stopped authoring session, bind the same gameplay at tick zero
            # before loading a save that contains its state.
            if binding and not game:
                active = uid(8146)
                client.call('runtime.start',self.start(active,revision)['params'])
                client.call('runtime.gameplay.load_native',dict(session_id=active,request_id=uid(8147),
                    expected_tick=0,expected_revision=0,**binding))
                args.update(expected_session_id=active,expected_tick=0,expected_gameplay_revision=1)
            restored = client.call('save.load',args)
            tick = NIGHT if binding else 0
            assert restored['tick'] == tick and restored['session_id'] == session
            state = self.observation(client,session,tick,True)
            assert state == expected['state']
            if self.args.gpu is not None:
                capture = self.render(client,session,tick,'restored','ocean',True,state)
                assert capture == expected['frames']['ocean']

    def relocated_export(self, revision, pin, expected, binding):
        first = self.root/'initial export'
        older = self.root/'runtime missing solar scattering'
        older.mkdir()
        manifest = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop(FEATURE)
        (older/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        flags = ['project','build',self.native(self.manifest),'--output',self.native(first),'--runtime']
        rejected = subprocess.run([str(self.args.binary.resolve()), *flags, self.native(older)],
            capture_output=True,text=True,timeout=60)
        assert rejected.returncode == 4 and not first.exists(), rejected
        reason = json.loads(rejected.stdout)['diagnostics'][0]['message']
        assert 'solar' in reason.lower() and 'scattering' in reason.lower(), reason
        self.measurements['older_export_rejection'] = reason
        success(json.loads(self.cli(*flags, self.native(self.args.runtime_root))))
        target = self.root/'relocated export'
        parked = self.root/'unavailable source'
        first.rename(target)
        self.project.rename(parked)
        try:
            game = target/'game.json'
            binary = target/json.loads(game.read_bytes())['engine']['executable']
            inspected = success(json.loads(self.cli('game','inspect',self.native(game),executable=binary)))
            assert inspected[DEPENDENCY]
            asset, = target.rglob('*.pwater')
            assert file_hash(asset) == pin
            inventory = tree_hashes(target)
            self.restore(revision,expected,binding,game=game)
            assert tree_hashes(target) == inventory
            self.measurements['relocated_bundle'] = inventory
        finally:
            parked.rename(self.project)

    def run(self):
        revision = self.author()
        legacy, active = self.prepare()
        binding = self.artifact() if self.args.native_descriptor else None
        (self.root/'saves').mkdir()
        revision, baseline = self.exercise(revision,legacy,'legacy',False,binding)
        revision, proposed = self.exercise(revision,active,'solar',True,binding)
        assert baseline.keys() == proposed.keys()
        for tick,value in baseline.items():
            assert proposed[tick]['state'] == value['state']
        self.measurements['state_by_tick'] = {tick:value['state'] for tick,value in proposed.items()}
        if self.args.gpu is not None:
            for tick in (0,2):
                assert baseline[tick]['frames']['ocean']['sha256'] == proposed[tick]['frames']['ocean']['sha256']
            for name in VIEWS:
                old = baseline[NIGHT]['frames'][name]
                new = proposed[NIGHT]['frames'][name]
                assert old['sha256'] != new['sha256']
                largest = 0
                for a,b in zip(old['samples'],new['samples'],strict=True):
                    assert a['pixel'] == b['pixel'] and a['depth'] == b['depth'] and a['raw_hdr'] == b['raw_hdr']
                    largest = max(largest, max(x-y for x,y in zip(a['actual'],b['actual'],strict=True)))
                assert largest > .01, (name,largest)
        expected = proposed[NIGHT if binding else 0]
        if self.args.runtime_root:
            self.relocated_export(revision,active['plan_sha256'],expected,binding)
        else:
            self.restore(revision,expected,binding)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--native-descriptor',type=Path,help='Prebuilt ManagedSolarLightingGame Native AOT bundle.')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--require-validation',action='store_true')
    args = parser.parse_args()
    if (args.gpu is not None or args.runtime_root) and args.native_descriptor is None:
        parser.error('GPU capture and relocated export require the solar lighting fixture Native AOT bundle.')
    if args.require_validation and args.gpu is None:
        parser.error('Vulkan validation requires a GPU capture.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-solar-scattering-',dir=args.output))
    # The fixture deliberately isolates solar scattering from the IBL roughness
    # and the spatial swell variant covered by water_offshore_capture.py.
    args.rough_reflection = args.swell_variation = False
    fixture = SolarScatteringContract(args,root)
    evidence = dict(passed=False,calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        logs = [call['wire']['stderr.log'] for call in fixture.calls if 'wire' in call]
        assert logs and all(not any(marker in log for marker in VALIDATION_FAILURES) for log in logs)
        if args.require_validation:
            assert any(VALIDATION_LAYER in log for log in logs)
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Water solar scattering evidence {root / "evidence.json"}')


if __name__ == '__main__':
    main()
