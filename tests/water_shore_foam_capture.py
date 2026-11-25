#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify stock shoreline foam with independent geometry and HDR coverage checks."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from height_fog_capture import environment
from water_optics_capture import AMBIENT, OPTICS
from water_refraction_capture import LENS, direction, panel
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, rpc, success, uid


AIR, SUBMERGED, SUN, SHADE = map(uid, (6870, 6871, 5, 6872))
EYE, PITCH = (11.7, 7.3, -1.95), -55
SURFACE = 5.27
FOAM = dict(depth_m=.7, opacity=.92)
SUN_COLOR = (1, .89, .72)
SUN_INTENSITY = 2.1
LATER = 64
PROBES = [(x, y) for y in (54, 79, 108, 138, 172, 201, 234, 267, 305)
          for x in (77, 153, 230, 307, 384, 461, 538)]


def ease(low, high, value):
    t = max(0, min(1, (value-low)/(high-low)))
    return t*t*(3-2*t)


def sample_noise(x, z):
    mask = 0xffffffff

    def lattice(i, j):
        a = (i*0x9e3779b9 ^ j*0x85ebca6b) & mask
        b = ((a ^ (a >> 16)) * 0x7feb352d) & mask
        c = ((b ^ (b >> 15)) * 0x846ca68b) & mask
        return ((c ^ (c >> 16)) & 65535) / 65535

    u, v = math.floor(x), math.floor(z)
    a, b = ease(0, 1, x-u), ease(0, 1, z-v)
    return sum(lattice(u+i, v+j)*wx*wz
               for i, wx in enumerate((1-a, a)) for j, wz in enumerate((1-b, b)))


def reference(pixel, tick, old, shade=False):
    ray = direction(pixel, PITCH)
    distance = (SURFACE-EYE[1])/ray[1]
    x, _, z = (e+r*distance for e, r in zip(EYE, ray, strict=True))
    if not (10 < x < 14 and -4 < z < -2):
        return dict(coverage=0, expected=old, outside=True)

    # The authored bed is an affine plane. Filtering its sample grid should
    # reproduce its value at the surface projection, clamped to cell centers.
    ground = bed(max(10.5, min(x, 13.5)), max(-3.5, min(z, -2.5)))
    depth = (SURFACE-ground)/FOAM['depth_m']
    if depth >= 1:
        return dict(coverage=0, expected=old, outside=False)
    span = 2*distance*math.tan(math.radians(LENS['vertical_fov'])/2)
    footprint = span/(CAPTURE_SIZE[1]*abs(ray[1]))
    phase = math.tau*(tick % 240)/240
    dx, dz = .22*math.cos(phase), .22*math.sin(phase)
    coarse = sample_noise(1.7*(x-10)+dx, 1.7*(z+4)+dz)
    coarse = .5 + (coarse-.5)*(1-ease(.16,.60,footprint))
    fine = sample_noise(6.5*(z+4)-2*dx, 6.5*(x-10)-2*dz)
    fine = .5 + (fine-.5)*(1-ease(.036,.15,footprint))
    fragments = ease(.26,.78,.7*coarse+.3*(1-abs(2*fine-1)))
    crest = .5 + .5*math.cos(14*depth + phase + 1.8*(coarse-.5))
    pulse = .24 + .76*ease(.36,.92,crest)
    coverage = FOAM['opacity']*(1-ease(.10,1,depth))*fragments*pulse
    radiance = [.82*(ambient+(0 if shade else SUN_INTENSITY*channel/math.pi))
                for ambient, channel in zip(AMBIENT,SUN_COLOR,strict=True)]
    return dict(coverage=coverage, outside=False, position=[x,SURFACE,z], depth_fraction=depth,
        expected=[ambient+coverage*(foam-ambient) for ambient, foam in zip(old,radiance,strict=True)])


class ShoreFoamCapture(WaterContract):
    owner = ParticleContract.owner

    def apply(self, revision, request, *ops):
        return success(self.world_calls(self.transaction(revision, request, *ops))[0])['revision']

    def sun(self, shadow):
        return component(SUN,'Light',dict(kind='directional',color=SUN_COLOR,intensity=SUN_INTENSITY,
            enabled=True,shadow=dict(enabled=shadow,distance=10,bias=.00008,normal_bias=0)))

    def author(self):
        recipe, _, revision = self.arm_terrain()
        lighting = environment() | dict(ambient=AMBIENT)
        lighting['sky']['sun'] = SUN
        ops = [dict(op='component.remove',id=uid(1),type='MeshRenderer'),
               dict(op='component.remove',id=uid(1),type='BoxCollider'),
               component(uid(4),'LightingEnvironment',lighting),
               component(SUN,'Transform',transform([0,0,0],[-math.sqrt(.5),0,0,math.sqrt(.5)])),
               self.sun(False)]
        for entity, eye, pitch, name in ((AIR,EYE,PITCH,'Shore'),
                                        (SUBMERGED,(11.3,5.1,-2.65),61,'Below the surface')):
            half = math.radians(pitch)*.5
            ops += [dict(op='entity.create',id=entity,name=name),
                    component(entity,'Transform',transform(eye,[math.sin(half),0,0,math.cos(half)])),
                    component(entity,'Camera',LENS)]
        revision = self.apply(revision, 6873, *ops)
        self.params = dict(terrain=dict(source=recipe.name),tile_entities=TILES,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            grid=dict(cells_x=4,cells_z=2,cell_size_m=1,origin_x_m=10,origin_z_m=-4,relaxation_per_second=0),
            initial_depths_m=[SURFACE-bed(*cell_center(cell)) for cell in CELLS])
        return revision

    def cook(self, revision, foam):
        optics = OPTICS | (dict(refraction=True) if self.args.refraction else {})
        if foam is not None:
            optics['shore_foam'] = foam
        params = self.params | dict(revision=revision,optics=optics)
        info = self.world_call('world.water.plan',**params)
        digest = info['plan_sha256']
        assert self.world_call('world.water.cook',**params,expected_plan_sha256=digest)==info
        asset = self.assets/(digest+'.pwater')
        assert file_hash(asset)==digest and info['optics']==optics
        data = json.loads(asset.read_bytes())
        assert data['optics']==optics and data['version']==(9 if foam else 7 if self.args.refraction else 6)
        return digest

    def capture(self, client, session, label, tick, foam, *, camera=AIR, pixels=PROBES, fsr=False, samples=1):
        path = self.root/(label+'.bmp')
        result = client.call('runtime.capture',dict(session_id=session,tick=tick,camera=camera,
            path=self.native(path),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=samples,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            reconstruction='fsr3_native' if fsr else 'none', scene_product_probes=[dict(x=x,y=y) for x,y in pixels]))
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==2
        assert result['nvrhi_errors']==0 and result['tick']==tick
        report = result['render_diagnostics']
        info = report['water_optics']
        active = foam is not None and foam['opacity']>0 and samples==1
        if foam is None:
            assert 'shore_foam' not in info
        else:
            assert info['shore_foam']==(foam | dict(active=active,sun_lighting=active)),info
        assert info['active'] is (samples==1) and report['reconstruction']['active'] is fsr
        if camera==SUBMERGED:
            assert info['camera_submersion_m']>.1
        self.measurements[label] = dict(report=result,sha256=file_hash(path))
        return report['scene_products']['probes']

    def observe(self, revision, label, prefix, foam, **options):
        with self.owner(label) as (client, _):
            session = uid(prefix)
            client.call('runtime.start',self.start(session,revision)['params'])
            assert client.call('runtime.water.inspect',dict(session_id=session,tick=0))['optics'].get('shore_foam')==foam
            return self.capture(client,session,label,0,foam,**options)

    def compare(self, control, treatment, tick):
        observed = []
        for pixel,before,after in zip(PROBES,control,treatment,strict=True):
            assert before['depth']==after['depth'] and before['raw_hdr']==after['raw_hdr'],pixel
            model = reference(pixel,tick,before['resolved_hdr'])
            if model['coverage']==0:
                assert before['resolved_hdr']==after['resolved_hdr'],(pixel, before, after, model)
            else:
                near_vector(after['resolved_hdr'],model['expected'],tolerance=.0025)
            observed.append(model | dict(pixel=pixel,baseline=before['resolved_hdr'],actual=after['resolved_hdr']))
        assert sum(row['coverage']>.025 for row in observed)>=15
        assert sum(row['coverage']==0 and not row['outside'] for row in observed)>=10
        assert sum(row['outside'] for row in observed)>=10
        return observed

    def shade(self, revision, baseline_pin, foam_pin, comparisons):
        witness = max(comparisons,key=lambda row:row['coverage'])
        x,y,z = witness['position']
        shade = panel(SHADE,'Shade above the water surface',(x,y+.3,z),(.12,.06,.12),[0,0,0])
        shade[-2]['value']['cast_shadows'] = True
        pixels = [witness['pixel']]
        revision = self.apply(revision,6890,self.sun(True),*shade,dict(op='water.set',asset=baseline_pin))
        before, = self.observe(revision,'shadow-control',6891,None,pixels=pixels)
        revision = self.apply(revision,6892,dict(op='water.set',asset=foam_pin))
        actual, = self.observe(revision,'shadow-foam',6893,FOAM,pixels=pixels)
        assert before['depth']==actual['depth']
        forecast = reference(pixels[0],0,before['resolved_hdr'],shade=True)
        near_vector(actual['resolved_hdr'],forecast['expected'],tolerance=.0025)
        assert math.dist(witness['actual'],actual['resolved_hdr'])>.05
        self.measurements['shadow_check'] = forecast | dict(actual=actual['resolved_hdr'],pixel=pixels[0])

    def relocated(self, revision, pin, saves):
        original, moved = self.root/'original export', self.root/'relocated export'
        missing = self.root/'runtime missing foam'
        missing.mkdir()
        manifest = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop('water_shore_foam')
        (missing/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        argv = ['project','build',self.native(self.manifest),'--output',self.native(original),'--runtime']
        rejected = subprocess.run([str(self.args.binary.resolve()),*argv,self.native(missing)],
                                  text=True,capture_output=True,timeout=60)
        assert rejected.returncode==4 and not original.exists(),rejected
        message = json.loads(rejected.stdout)['diagnostics'][0]['message']
        assert message=='Water shore foam requires a compatible Vulkan runtime.',message
        self.measurements['admission_rejection'] = message
        success(json.loads(self.cli(*argv,self.native(self.args.runtime_root))))
        original.rename(moved)
        unavailable = self.root/'parked original'
        self.project.rename(unavailable)
        try:
            game = moved/'game.json'
            executable = moved/json.loads(game.read_bytes())['engine']['executable']
            assert success(json.loads(self.cli('game','inspect',self.native(game),executable=executable)))['needs_water_shore_foam']
            package, = moved.rglob('*.pwater')
            assert file_hash(package)==pin
            with self.owner('relocated save',game=game,saves=saves) as (client,_):
                active = client.call('runtime.status')['session_id']
                restored = uid(6894)
                client.call('save.load',self.load(restored,revision,6895,active=active,active_tick=0)['params'])
                self.capture(client,restored,'relocated',LATER,FOAM)
            assert self.measurements['relocated']['sha256']==self.measurements['later']['sha256']
        finally:
            unavailable.rename(self.project)

    def run(self):
        revision = self.author()
        malformed = [dict(depth_m=.019,opacity=1),dict(depth_m=4,opacity=1),dict(FOAM,opacity=True),
                     dict(FOAM,opacity=-.01),dict(FOAM,opacity=1.01),dict(FOAM,unexpected=1)]
        self.cases(*[(str(bad),rpc('world.water.plan',**(self.params|dict(revision=revision,
            optics=OPTICS|dict(shore_foam=bad)))),-32602) for bad in malformed])
        base = self.cook(revision,None)
        enabled = self.cook(revision,FOAM)
        disabled = dict(FOAM,opacity=0)
        zero = self.cook(revision,disabled)
        revision = self.apply(revision,6874,dict(op='water.set',asset=base))
        control = self.observe(revision,'control',6875,None)
        self.observe(revision,'control-underwater',6876,None,camera=SUBMERGED)
        revision = self.apply(revision,6877,dict(op='water.set',asset=zero))
        off = self.observe(revision,'zero-opacity',6878,disabled)
        assert off==control and self.measurements['zero-opacity']['sha256']==self.measurements['control']['sha256']
        revision = self.apply(revision,6879,dict(op='water.set',asset=enabled))
        assert self.world_call('world.dependencies')['needs_water_shore_foam']
        saves = self.root/'external saves'
        saves.mkdir()
        with self.owner('committed phases') as (client,_):
            session = uid(6880)
            client.call('runtime.start',self.start(session,revision)['params'])
            first = self.capture(client,session,'enabled',0,FOAM)
            self.capture(client,session,'enabled-underwater',0,FOAM,camera=SUBMERGED)
            assert self.measurements['enabled-underwater']['sha256']==self.measurements['control-underwater']['sha256']
            client.call('save.configure',dict(request_id=uid(6881),expected_generation=0,root=self.native(saves)))
            client.call('runtime.step',self.step(session,0,LATER,6882)['params'])
            client.call('save.write',self.write(session,LATER,'checkpoint',6883)['params'])
            later = self.capture(client,session,'later',LATER,FOAM)
            if self.args.fsr:
                self.capture(client,session,'fsr',LATER,FOAM,fsr=True)
        before = self.compare(control,first,0)
        after = self.compare(control,later,LATER)
        assert self.measurements['enabled']['sha256']!=self.measurements['later']['sha256']
        self.measurements['comparisons'] = dict(initial=before,later=after)
        if self.args.runtime_root:
            self.relocated(revision,enabled,saves)
        self.shade(revision,base,enabled,before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--fsr',action='store_true')
    parser.add_argument('--refraction',action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='water-shore-foam-',dir=args.output))
    fixture = ShoreFoamCapture(args,root)
    report = dict(passed=False,calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        logs = [call['wire']['stderr.log'] for call in fixture.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in ('VUID-','SYNC-HAZARD','Validation Error','Validation Warning'))
                            for log in logs)
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Water shore foam evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
