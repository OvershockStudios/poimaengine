#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify focused underwater sunlight against numerical wave derivatives and native ghosts."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from height_fog_capture import environment
from water_optics_capture import OPTICS
from water_refraction_capture import direction, panel
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, rpc, success, uid


CAMERA, SUN, FLOOR, BLOCKER = map(uid, (6810, 5, 6811, 6812))
EYE, PITCH = (12.05, 8.1, -1.25), -53
LENS = dict(vertical_fov=50, near=.05, far=35)
SURFACE, RECEIVER = 6.2, 5.48
WAVES = [
    dict(wavelength_m=.74, slope=.17, period_ticks=43, heading_degrees=34),
    dict(wavelength_m=1.03, slope=.16, period_ticks=29, heading_degrees=-57),
]
PROBES = [(x, y) for y in (78, 108, 140, 172, 201, 228, 258, 288, 309)
          for x in (83, 150, 223, 300, 387, 456, 523)]
STRENGTH = .92
TIMES = (0, 9)


def smoothstep(low, high, value):
    t = min(max((value - low) / (high - low), 0), 1)
    return t * t * (3 - 2 * t)


def illuminate(entry, tick, footprint):
    slope = [0.0, 0.0]
    for wave in WAVES:
        angle = math.radians(wave['heading_degrees'])
        d = (math.cos(angle), math.sin(angle))
        k = math.tau / wave['wavelength_m']
        phase = k * (entry[0]*d[0] + entry[1]*d[1]) - math.tau * (tick % wave['period_ticks']) / wave['period_ticks']
        magnitude = wave['slope'] * math.cos(phase) * (1 - smoothstep(.5, 2.5, k * footprint))
        for axis in range(2):
            slope[axis] += magnitude*d[axis]
    length = math.hypot(*slope, 1)
    normal = (-slope[0]/length, 1/length, -slope[1]/length)
    eta = 1/1.333
    tangent = (eta*normal[0]*normal[1], eta*(normal[1]**2 - 1), eta*normal[1]*normal[2])
    perpendicular = math.sqrt(1 - sum(t*t for t in tangent))
    ray = tuple(t - perpendicular*n for t, n in zip(tangent, normal, strict=True))
    distance = (SURFACE-RECEIVER)/(-ray[1])
    return (entry[0] + distance*ray[0], entry[1] + distance*ray[2]), distance


def derivative(entry, tick, footprint):
    h = .00001
    columns = []
    for axis in range(2):
        front, back = list(entry), list(entry)
        front[axis] += h
        back[axis] -= h
        a, _ = illuminate(front, tick, footprint)
        b, _ = illuminate(back, tick, footprint)
        columns.append([(v - w) / (2*h) for v, w in zip(a, b, strict=True)])
    x, z = columns
    return x, z, x[0]*z[1] - x[1]*z[0]


def forecast(pixel, tick):
    ray = direction(pixel, PITCH)
    distance = (RECEIVER-EYE[1])/ray[1]
    position = tuple(e + w*distance for e, w in zip(EYE, ray, strict=True))
    # Leave a margin around the board and the wet/dry grid boundaries.
    if not (10.15 < position[0] < 13.85 and -3.87 < position[2] < -2.13):
        return None
    if position[0] > 13.12:
        return dict(position=position, modulation=[1]*3, wet=False)
    if position[0] > 12.9:
        return None
    target = (position[0]-10, position[2]+4)
    angle = math.radians(PITCH)
    view_z = distance * (ray[1]*math.sin(angle) - ray[2]*math.cos(angle))
    footprint = 2*view_z*math.tan(math.radians(LENS['vertical_fov'])/2)/CAPTURE_SIZE[1]
    entry = target
    for _ in range(2):
        actual, _ = illuminate(entry, tick, footprint)
        jx, jz, determinant = derivative(entry, tick, footprint)
        if abs(determinant) < .035:
            break
        ex, ez = (a - b for a, b in zip(actual, target, strict=True))
        step = ((jz[1]*ex-jz[0]*ez)/determinant, (jx[0]*ez-jx[1]*ex)/determinant)
        scale = min(1, .35/max(math.hypot(*step), .0001))
        entry = tuple(v-s*scale for v, s in zip(entry, step, strict=True))
    actual, path_m = illuminate(entry, tick, footprint)
    residual = math.dist(actual,target)
    _, _, determinant = derivative(entry, tick, footprint)
    if residual >= .045 or not (0 <= entry[0] < 3 and 0 <= entry[1] < 2):
        return dict(position=position, wet=True, modulation=[1]*3, residual=residual)
    focus = max(.25, min(4, math.sqrt(1 + .12**2)/math.hypot(determinant,.12)))
    gain = (focus-1) * STRENGTH * (1 - smoothstep(.002,.045,residual))
    modulation = [1 + gain*math.exp(-coefficient*path_m) for coefficient in OPTICS['extinction_per_m']]
    return dict(position=position, wet=True, determinant=determinant, residual=residual,
        entry=(entry[0]+10,SURFACE,entry[1]-4), modulation=modulation)


class CausticsCapture(WaterContract):
    owner = ParticleContract.owner

    def apply(self, revision, request, *ops):
        return success(self.world_calls(self.transaction(revision, request, *ops))[0])['revision']

    def sun(self, intensity=3, shadow=False):
        return component(SUN, 'Light', dict(kind='directional', color=[1, .92, .83], intensity=intensity,
            enabled=True, shadow=dict(enabled=shadow, distance=10, bias=.00008, normal_bias=0)))

    def author(self):
        recipe, _, revision = self.arm_terrain()
        lighting = environment() | dict(shadow_resolution=2048)
        lighting['sky']['sun'] = SUN
        half = math.radians(PITCH) / 2
        ops = [
            dict(op='component.remove', id=uid(1), type='MeshRenderer'),
            dict(op='component.remove', id=uid(1), type='BoxCollider'),
            component(uid(4), 'LightingEnvironment', lighting),
            component(SUN, 'Transform', transform([0,0,0], [-math.sqrt(.5),0,0,math.sqrt(.5)])),
            self.sun(),
            dict(op='entity.create', id=CAMERA, name='Caustic receiver camera'),
            component(CAMERA, 'Transform', transform(EYE,[math.sin(half),0,0,math.cos(half)])),
            component(CAMERA, 'Camera', LENS),
        ]
        ops.extend(panel(FLOOR, 'Diffuse submerged witness', (12, RECEIVER-.03, -3), (4,.06,2), [0,0,0]))
        ops[-2]['value']['receive_shadows']=True
        ops[-1]['value'].update(base_color=[.28,.43,.52], metallic=0)
        new_revision=self.apply(revision, 6815, *ops)
        self.parameters = dict(terrain=dict(source=recipe.name), tile_entities=TILES,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            grid=dict(cells_x=4,cells_z=2,cell_size_m=1,origin_x_m=10,origin_z_m=-4,relaxation_per_second=0),
            initial_depths_m=[SURFACE-bed(*cell_center(cell)) if cell[0]<3 else 0 for cell in CELLS])
        return new_revision

    def cook(self, revision, strength, waves=WAVES):
        optics=OPTICS | ({'refraction':True} if self.args.refraction else {})
        if strength is not None:
            optics['caustics_strength']=strength
        params=self.parameters | dict(revision=revision,optics=optics)
        if waves:
            params['surface_wave_layers']=waves
        result=self.world_call('world.water.plan',**params)
        pin=result['plan_sha256']
        assert self.world_call('world.water.cook',**params,expected_plan_sha256=pin)==result
        asset=self.assets/(pin+'.pwater')
        assert file_hash(asset)==pin
        assert json.loads(asset.read_bytes())['version']==(8 if strength else 7 if self.args.refraction else 6)
        return pin

    def capture(self, client, session, label, tick, expect, probes=PROBES, fsr=False):
        path=self.root/(label+'.bmp')
        result=client.call('runtime.capture',dict(session_id=session,tick=tick,camera=CAMERA,
            path=self.native(path),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=1,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            reconstruction='fsr3_native' if fsr else 'none',
            scene_product_probes=[dict(x=x,y=y) for x,y in probes]))
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==2 and result['nvrhi_errors']==0
        report=result['render_diagnostics']
        assert report['water_optics'].get('refraction',False) is self.args.refraction
        info=report['water_optics'].get('caustics')
        if expect is None:
            assert info is None, info
        else:
            assert info==dict(active=expect=='active',reason=expect,strength=STRENGTH), info
        assert report['reconstruction']['active'] is fsr
        self.measurements[label]=dict(report=result,sha256=file_hash(path))
        return report['scene_products']['probes']

    def observe(self, revision, label, prefix, expect, *, probes=PROBES):
        with self.owner(label) as (client, _):
            session=uid(prefix)
            client.call('runtime.start',self.start(session,revision)['params'])
            return self.capture(client,session,label,0,expect,probes)

    def compare(self, baseline, visible, tick):
        observed=[]
        for pixel, a, b in zip(PROBES,baseline,visible,strict=True):
            assert a['depth']==b['depth'], (pixel,a,b)
            reference=forecast(pixel,tick)
            if reference is None:
                continue
            assert a['surface_valid'] and min(a['raw_hdr'])>.01, (pixel,a)
            predicted=[channel*gain for channel,gain in zip(a['raw_hdr'],reference['modulation'],strict=True)]
            near_vector(b['raw_hdr'],predicted,tolerance=.0035)
            observed.append(reference | dict(pixel=pixel,expected=predicted,actual=b['raw_hdr']))
        assert sum(row['wet'] for row in observed)>=20 and sum(not row['wet'] for row in observed)>=5
        assert max(row['modulation'][2] for row in observed)>1.10
        assert min(row['modulation'][2] for row in observed)<.90
        return observed

    def shadowed_entry(self, revision, base_pin, lit_pin, observations):
        pair=next(row for row in observations
                  if 'entry' in row and row['modulation'][2]>1.20
                  and math.hypot(row['entry'][0]-row['position'][0],row['entry'][2]-row['position'][2])>.027)
        x, _, z=pair['entry']
        caster=panel(BLOCKER,'Small shade at the predicted sunlight entry',(x,6.75,z),(.030,.08,.030),[0,0,0])
        caster[-2]['value']['cast_shadows']=True
        revision=self.apply(revision,6840,self.sun(shadow=True),*caster,dict(op='water.set',asset=base_pin))
        site=[pair['pixel']]
        shadow_baseline=self.observe(revision,'entry-shadow-baseline',6841,None,probes=site)[0]
        revision=self.apply(revision,6842,dict(op='water.set',asset=lit_pin))
        shadow_pattern=self.observe(revision,'entry-shadow-caustic',6843,'active',probes=site)[0]
        unshadowed=self.measurements['baseline']['report']['render_diagnostics']['scene_products']['probes'][PROBES.index(site[0])]
        near_vector(shadow_baseline['raw_hdr'],unshadowed['raw_hdr'],tolerance=.0025)
        assert shadow_pattern['depth']==shadow_baseline['depth']==unshadowed['depth']
        near_vector(shadow_pattern['raw_hdr'],shadow_baseline['raw_hdr'],tolerance=.006)
        assert math.dist(pair['actual'],shadow_pattern['raw_hdr'])>.015
        self.measurements['entry_shadow']=dict(pixel=site[0],entry=pair['entry'],
            unblocked=pair['actual'],blocked=shadow_pattern['raw_hdr'],base=shadow_baseline['raw_hdr'])

    def relocated(self, revision, pin, saves):
        original=self.root/'original export'
        moved=self.root/'relocated export'
        without_feature=self.root/'unsupported runtime'
        without_feature.mkdir()
        manifest=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop('water_caustics')
        (without_feature/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        args=['project','build',self.native(self.manifest),'--output',self.native(original),'--runtime']
        rejected=subprocess.run([str(self.args.binary.resolve()),*args,self.native(without_feature)],
                                text=True,capture_output=True,timeout=60)
        assert rejected.returncode==4 and not original.exists(),rejected
        message=json.loads(rejected.stdout)['diagnostics'][0]['message']
        assert message=='Water caustics require a compatible Vulkan runtime.',message
        self.measurements['admission_rejection']=message
        success(json.loads(self.cli('project','build',self.native(self.manifest), '--output',self.native(original),
                                   '--runtime',self.native(self.args.runtime_root))))
        original.rename(moved)
        source=self.root/'parked original'
        self.project.rename(source)
        try:
            game=moved/'game.json'
            executable=moved/json.loads(game.read_bytes())['engine']['executable']
            inspected=success(json.loads(self.cli('game','inspect',self.native(game),executable=executable)))
            assert inspected['needs_water_caustics']
            package,=moved.rglob('*.pwater')
            assert file_hash(package)==pin
            with self.owner('relocated save',game=game,saves=saves) as (client,_):
                active=client.call('runtime.status')['session_id']
                restored=uid(6852)
                client.call('save.load',self.load(restored,revision,6853,active=active,active_tick=0)['params'])
                self.capture(client,restored,'relocated',TIMES[1],'active')
            assert self.measurements['relocated']['sha256']==self.measurements['moved']['sha256']
        finally:
            source.rename(self.project)

    def run(self):
        revision=self.author()
        self.cases(*[(str(bad),rpc('world.water.plan',**(self.parameters|dict(revision=revision,
            optics=OPTICS|dict(caustics_strength=bad)))), -32602) for bad in (True,-.01,1.01)])
        base=self.cook(revision,None)
        assert self.cook(revision,0)==base
        enabled=self.cook(revision,STRENGTH)
        still=self.cook(revision,STRENGTH,waves=[])
        revision=self.apply(revision,6820,dict(op='water.set',asset=base))
        baseline=self.observe(revision,'baseline',6821,None)
        revision=self.apply(revision,6822,dict(op='water.set',asset=still))
        no_wave=self.observe(revision,'still',6823,'no_waves')
        assert [row['raw_hdr'] for row in no_wave]==[row['raw_hdr'] for row in baseline]
        revision=self.apply(revision,6824,dict(op='water.set',asset=enabled),self.sun(intensity=0))
        no_light=self.observe(revision,'unlit',6825,'sun_unlit')
        assert all(max(row['raw_hdr'])==0 for row in no_light if row['surface_valid'])
        revision=self.apply(revision,6826,self.sun())
        assert self.world_call('world.dependencies')['needs_water_caustics']
        saves=self.root/'external saves'
        saves.mkdir()
        with self.owner('moving caustics') as (client,_):
            session=uid(6830)
            client.call('runtime.start',self.start(session,revision)['params'])
            lit=self.capture(client,session,'lit',0,'active')
            client.call('save.configure',dict(request_id=uid(6831),expected_generation=0,root=self.native(saves)))
            client.call('runtime.step',self.step(session,0,TIMES[1],6832)['params'])
            client.call('save.write',self.write(session,TIMES[1],'checkpoint',6833)['params'])
            moved=self.capture(client,session,'moved',TIMES[1],'active')
            if self.args.fsr:
                self.capture(client,session,'fsr',TIMES[1],'active',fsr=True)
        first=self.compare(baseline,lit,0)
        second=self.compare(baseline,moved,TIMES[1])
        assert self.measurements['lit']['sha256']!=self.measurements['moved']['sha256']
        self.measurements['comparisons']={'initial':first,'later':second}
        if self.args.runtime_root:
            self.relocated(revision,enabled,saves)
        self.shadowed_entry(revision,base,enabled,first)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--fsr',action='store_true')
    parser.add_argument('--refraction',action='store_true')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='water-caustics-',dir=args.output))
    f=CausticsCapture(args,root)
    report=dict(passed=False,calls=f.calls,measurements=f.measurements)
    try:
        f.run()
        logs=[call['wire']['stderr.log'] for call in f.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in ('VUID-','SYNC-HAZARD','Validation Error','Validation Warning'))
                            for log in logs)
        report['passed']=True
    except BaseException:
        report['failure']=traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Water caustics evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
