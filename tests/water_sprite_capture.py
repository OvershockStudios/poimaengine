#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare water-filtered particles with opaque radiance at matching depths."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from height_fog_capture import environment
from solar_lighting_contract import IDENTITY, solar_reference
from texture_fixture import png
from water_optics_capture import OPTICS
from water_refraction_capture import panel
from water_solar_scattering_capture import NIGHT, STRENGTH
from world_particles_contract import ParticleContract, action, near_vector, transform
from world_terrain_contract import file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, rpc, success, uid


SIZE = (512, 288)
LENS = dict(vertical_fov=75, near=.15, far=12)
AIR, SUBMERGED, SUN = map(uid, (7120, 7121, 5))
CAMERAS = dict(air=(AIR, (11.35, 6.50, -2.13)), water=(SUBMERGED, (11.35, 5.85, -2.13)))
SURFACE = 6.15
BURST = 'suspend'
TICK = 1
REFLECTION_ROUGHNESS = .43
FAR = (11.62, 5.82, -3.59)
NEAR = (11.35+(FAR[0]-11.35)*1.1/1.46, 6.50+(FAR[1]-6.50)*1.1/1.46, -3.23)
# Position, unfiltered linear radiance, alpha, card size.
CARDS = {
    'above': ((11.05,6.30,-3.08),(.92,.70,.28),.50,.13),
    'far': (FAR,(.91,.42,.06),.58,.19),
    'near': (NEAR,(.12,.73,.84),.35,.14),
    'dry': ((10.20,6.41,-3.75),(.54,.32,.87),.66,.17),
    'occluded': ((11.90,6.39,-3.75),(.95,.76,.11),.85,.16),
}
SITES = {
    'air': {'above': ['above'], 'far': ['far','near'], 'dry': ['dry'], 'occluded': []},
    'water': {'above': ['above'], 'far': ['far'], 'near': ['near'], 'occluded': []},
}


def pixel_projection(eye, point):
    w,h=SIZE
    focal=h/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
    x,y,z=(actual-base for actual,base in zip(point,eye,strict=True))
    return w/2-focal*x/z,h/2+focal*y/z


def probes(camera):
    _,eye=CAMERAS[camera]
    return {label:tuple(map(math.floor,pixel_projection(eye,CARDS[label][0]))) for label in SITES[camera]}


def inside(camera, pixel, card):
    _,eye=CAMERAS[camera]
    center,_,_,width=CARDS[card]
    x,y=pixel_projection(eye,center)
    half=SIZE[1]*width/(4*math.tan(math.radians(LENS['vertical_fov'])/2)*(eye[2]-center[2]))
    return abs(pixel[0]+.5-x)<half and abs(pixel[1]+.5-y)<half


class WaterSpriteCapture(WaterContract):
    owner=ParticleContract.owner

    def apply(self, revision, request, *ops):
        return success(self.world_calls(self.transaction(revision,request,*ops))[0])['revision']

    def author(self):
        recipe,_,revision=self.arm_terrain()
        texture=self.project/'white.png'
        texture.write_bytes(png(1,1,[255,255,255,255]))
        image=self.world_call('asset.image.import',source=texture.name,color_space='srgb')['asset']
        lighting=environment()|dict(ambient=[.35,.47,.56])
        if self.args.rough_reflection:
            lighting['ibl']=dict(enabled=True,diffuse_intensity=0.,specular_intensity=1.)
        ops=[dict(op='component.remove',id=uid(1),type='MeshRenderer'),
             dict(op='component.remove',id=uid(1),type='BoxCollider')]
        if self.args.sun:
            lighting['sky']['sun']=SUN
            ops += [component(SUN,'Transform',transform([0,0,0],[-math.sqrt(.5),0,0,math.sqrt(.5)])),
                    component(SUN,'Light',dict(kind='directional',color=[1,.86,.7],intensity=1.4,enabled=True,
                        shadow=dict(enabled=True,distance=10,bias=.00008,normal_bias=0)))]
        else:
            ops.append(dict(op='component.remove',id=SUN,type='Light'))
        ops.append(component(uid(4),'LightingEnvironment',lighting))
        for name,(entity,eye) in CAMERAS.items():
            ops += [dict(op='entity.create',id=entity,name=name+' camera'),
                    component(entity,'Transform',transform(eye)),component(entity,'Camera',LENS)]
        ops += panel(uid(7122),'Matte receiver',(11.3,5.8,-7),(18,9,.06),[.12,.18,.24])
        target=CARDS['occluded'][0]
        eye=CAMERAS['air'][1]
        at=[origin+(destination-origin)*.59 for origin,destination in zip(eye,target,strict=True)]
        # Stretch the occluder vertically across both viewpoints.
        ops += panel(uid(7123),'Opaque blocker',(at[0],6.20,at[2]),(.19,1.3,.02),[.21,.15,.13])
        self.references={}
        for index,(label,(center,color,alpha,size)) in enumerate(CARDS.items()):
            entity,ref=uid(7130+index),uid(7140+index)
            sprite=dict(spawn_space='world',simulation_space='world',lifetime_ticks=480,size_m=size,linear_rgb=color,
                burst=dict(action=BURST,count=1),sprite=dict(texture=image,opacity=alpha))
            ops += [dict(op='entity.create',id=entity,name=label+' particle'),component(entity,'Transform',transform(center)),
                    component(entity,'ParticleEmitter',sprite)]
            x,y,z=center
            marker=panel(ref,label+' radiance control',(x,y,z-.006),(size,size,.012),color)
            marker[-2]['value']['visible']=False
            ops += marker
            self.references[label]=marker[-2]
        revision=self.apply(revision,7151,*ops)
        grid=dict(cells_x=4,cells_z=2,cell_size_m=1,origin_x_m=10,origin_z_m=-4,relaxation_per_second=0)
        params=dict(revision=revision,terrain=dict(source=recipe.name),tile_entities=TILES,grid=grid,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            initial_depths_m=[SURFACE-bed(*cell_center(cell)) if cell[0]>=1 else 0 for cell in CELLS],
            optics=OPTICS|dict(shore_foam=dict(depth_m=2.4,opacity=.95),follow_terrain=True))
        if self.args.rough_reflection:
            params['optics']['reflection_roughness']=REFLECTION_ROUGHNESS
        if self.args.solar_native_descriptor:
            params['optics']['solar_scattering']=STRENGTH
        planned=self.world_call('world.water.plan',**params)
        pin=planned['plan_sha256']
        assert self.world_call('world.water.cook',**params,expected_plan_sha256=pin)==planned
        return self.apply(revision,7152,dict(op='water.set',asset=pin))

    def render(self, client, session, tick, camera, tag, particles, fsr=False):
        path=self.root/(tag+'-'+camera+'.bmp')
        sites=probes(camera)
        response=client.call('runtime.capture',dict(session_id=session,tick=tick,camera=CAMERAS[camera][0],
            path=self.native(path),width=SIZE[0],height=SIZE[1],gpu=self.args.gpu,samples=1,
            capture_frames=2,profile=True,lighting_path=self.args.lighting_path,
            reconstruction='fsr3_native' if fsr else 'none',
            scene_product_probes=[dict(x=x,y=y) for x,y in sites.values()]))
        assert response['hardware'] and response['capture_written'] and response['frames_presented']==2
        assert response['nvrhi_errors']==0 and response['tick']==tick
        diag=response['render_diagnostics']
        optics=diag['water_optics']
        assert optics['active'] and optics['follow_terrain'] and optics['shore_foam']['active']
        if self.args.solar_native_descriptor:
            gain=solar_reference(NIGHT)['sky_gain']
            assert response['lighting']['solar_lighting']['sky_gain']==gain
            assert optics['solar_scattering']==dict(strength=STRENGTH,sky_gain=gain,radiance_scale=1-STRENGTH*(1-gain))
            assert optics['shore_foam']['sun_lighting'] is False
        else:
            assert optics['shore_foam']['sun_lighting'] is self.args.sun
        if self.args.rough_reflection:
            assert optics['rough_reflection']==dict(active=True,perceptual_roughness=REFLECTION_ROUGHNESS)
        assert optics.get('sprites',False) is particles and diag['reconstruction']['active'] is fsr,diag
        assert (optics['camera_submersion_m']>.1)==(camera=='water')
        self.measurements[tag+'-'+camera]=dict(report=response,sha256=file_hash(path))
        return dict(zip(sites,diag['scene_products']['probes'],strict=True))

    def marker(self, name, visible):
        binding=self.references[name]
        return binding | dict(value=binding['value']|dict(visible=visible))

    def start_capture(self,client,session,revision,request,tick=0):
        client.call('runtime.start',self.start(session,revision)['params'])
        if self.args.solar_native_descriptor:
            descriptor=self.args.solar_native_descriptor
            loaded=client.call('runtime.gameplay.load_native',dict(session_id=session,request_id=uid(7240),
                expected_tick=0,expected_revision=0,descriptor=self.native(descriptor),
                expected_descriptor_sha256=file_hash(descriptor)))
            assert loaded['revision']==1 and loaded['module']['schema']['identity']==IDENTITY
        if tick:
            assert client.call('runtime.step',self.step(session,0,tick,request)['params'])['current_tick']==tick

    def opaque_references(self, revision):
        captures={}
        tick=NIGHT if self.args.solar_native_descriptor else 0
        for i,name in enumerate(('above','near','far','dry')):
            revision=self.apply(revision,7170+i,self.marker(name,True))
            with self.owner('opaque '+name) as (client,_):
                session=uid(7180+i)
                self.start_capture(client,session,revision,7241,tick)
                captures[name]={camera:self.render(client,session,tick,camera,'solid-'+name,False) for camera in CAMERAS}
            revision=self.apply(revision,7190+i,self.marker(name,False))
        return revision,captures

    def expected(self, baseline, sources, live):
        results={}
        for camera,sites in SITES.items():
            for label,order in sites.items():
                point=probes(camera)[label]
                assert all(inside(camera,point,name) for name in order)
                assert all(not inside(camera,point,name) for name in CARDS if name not in order and name!='occluded')
                base=baseline[camera][label]
                actual=live[camera][label]
                assert actual['raw_hdr']==base['raw_hdr'] and actual['depth']==base['depth'], (camera,label,actual,base)
                rgb=base['resolved_hdr']
                reference_depths={}
                for name in order:
                    _,color,alpha,_=CARDS[name]
                    surface=sources[name][camera][label]
                    assert surface['depth']<base['depth'],(camera,label,name,surface,base)
                    near_vector(surface['raw_hdr'],color,tolerance=.002)
                    reference_depths[name]=surface['depth']
                    rgb=[(1-alpha)*back+alpha*front for back,front in zip(rgb,surface['resolved_hdr'],strict=True)]
                near_vector(actual['resolved_hdr'],rgb,tolerance=.002)
                if not order:
                    assert actual['resolved_hdr']==base['resolved_hdr'],(camera,label)
                results[camera+'-'+label]=dict(pixel=point,expected=rgb,actual=actual['resolved_hdr'],
                    baseline=base['resolved_hdr'],sources=reference_depths)
        # The same above-water sprite stays clear on the air camera and is
        # visibly attenuated along the submerged camera's path to the surface.
        a=[sources['above'][camera]['above'] for camera in CAMERAS]
        assert a[0]['raw_hdr']==a[0]['resolved_hdr']
        assert math.dist(a[1]['raw_hdr'],a[1]['resolved_hdr'])>.05
        # A ray that goes through the surface sees the filtered red sprite.
        far=sources['far']['air']['far']
        assert far['raw_hdr'][0]-far['resolved_hdr'][0]>.15
        return results

    def relocated(self, revision, saves):
        before,moved=self.root/'first export',self.root/'relocated export'
        success(json.loads(self.cli('project','build',self.native(self.manifest),'--output',self.native(before),
                                    '--runtime',self.native(self.args.runtime_root))))
        before.rename(moved)
        source=self.root/'parked authoring'
        self.project.rename(source)
        try:
            with self.owner('relocated save',game=moved/'game.json',saves=saves) as (client,_):
                active=client.call('runtime.status')['session_id']
                session=uid(7220)
                client.call('save.load',self.load(session,revision,7221,active=active,active_tick=0)['params'])
                for camera in CAMERAS:
                    self.render(client,session,TICK,camera,'relocated',True)
                    assert self.measurements['relocated-'+camera]['sha256']==self.measurements['live-'+camera]['sha256']
        finally:
            source.rename(self.project)

    def run(self):
        revision=self.author()
        revision,controls=self.opaque_references(revision)
        solar=self.args.solar_native_descriptor is not None
        tick=NIGHT if solar else TICK
        if solar:
            with self.owner('night without particles') as (client,_):
                session=uid(7242)
                self.start_capture(client,session,revision,7243,tick)
                baseline={camera:self.render(client,session,tick,camera,'empty',False) for camera in CAMERAS}
        saves=self.root/'external saves'
        saves.mkdir()
        with self.owner('live particles') as (client,_):
            session=uid(7210)
            self.start_capture(client,session,revision,7244,tick-1)
            if not solar:
                baseline={camera:self.render(client,session,0,camera,'empty',False) for camera in CAMERAS}
            step=self.step(session,tick-1,1,7211)['params']
            step['inputs']=[dict(entity=uid(2),actions=[action(BURST,pressed=True)])]
            assert client.call('runtime.step',step)['current_tick']==tick
            live={camera:self.render(client,session,tick,camera,'live',True) for camera in CAMERAS}
            client.call('save.configure',dict(request_id=uid(7212),expected_generation=0,root=self.native(saves)))
            args=self.write(session,tick,'checkpoint',7213)['params']
            client.call('save.write',args | dict(expected_gameplay_revision=1 if solar else 0))
            if self.args.fsr:
                for camera in CAMERAS:
                    fsr=self.render(client,session,tick,camera,'fsr',True,fsr=True)
                    for label,probe in fsr.items():
                        near_vector(probe['resolved_hdr'],live[camera][label]['resolved_hdr'],tolerance=.002)
        self.measurements['blend_oracle']=self.expected(baseline,controls,live)
        if self.args.runtime_root:
            self.relocated(revision,saves)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--fsr',action='store_true')
    parser.add_argument('--sun',action='store_true')
    parser.add_argument('--rough-reflection',action='store_true')
    parser.add_argument('--solar-native-descriptor',type=Path,help='Check sprite compositing during the compiled dusk phase.')
    args=parser.parse_args()
    if args.solar_native_descriptor and (not args.sun or args.runtime_root):
        parser.error('The solar variant needs --sun. Use water_solar_scattering_capture.py to check relocated export.')
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='water-sprites-',dir=args.output))
    case=WaterSpriteCapture(args,root)
    report=dict(passed=False,calls=case.calls,measurements=case.measurements)
    try:
        case.run()
        logs=[call['wire']['stderr.log'] for call in case.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in
            ('VUID-','SYNC-HAZARD','Validation Error','Validation Warning')) for log in logs)
        report['passed']=True
    except BaseException:
        report['failure']=traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Water sprite evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
