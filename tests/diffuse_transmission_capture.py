#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify thin dielectric lighting, shadow occlusion, and retained cutout coverage."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from gltf_fixture import glb
from snow_compaction_contract import TOP_DOWN, vector_near
from terrain_surface_layer_capture import SurfaceLayerFixture, directional, unit
from poima_client import WorldClient
from texture_fixture import png, quad
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


WIDTH, HEIGHT = 513, 321
CAMERA_Z = 5
LENS = dict(near=.1, far=20, vertical_fov=57)
FOCAL = HEIGHT / (2*math.tan(math.radians(LENS['vertical_fov'])/2))
LIT, CONTROL, CAMERA, OCCLUDER, SUN = [uid(i) for i in (1210,1211,1212,1213,5)]
LEAVES, LIGHT_COLOR = [(-1.65,LIT),(1.65,CONTROL)], (1,.84,.63)
BASE_COLOR, STRENGTH = (.17,.63,.28), .625
MATERIAL = dict(base_color=BASE_COLOR, emissive=[0]*3, metallic=0, roughness=.62,
                double_sided=True, alpha_mode='mask', alpha_cutoff=.5)
ENVIRONMENT = dict(ambient=[0]*3,exposure=1,shadow_resolution=1024)
SKY = dict(enabled=True,zenith=[.06,.23,.58],horizon=[.24,.32,.41],ground=[.42,.21,.07],
           horizon_falloff=1,sun=None,sun_size_degrees=.53,sun_intensity=0)


def probe_points():
    result = {}
    for center, entity in LEAVES:
        for coverage, x in (('leaf',center-.57), ('hole',center+.59)):
            for row, y in enumerate((-.51,0,.39)):
                px = round(WIDTH/2 + FOCAL*x/CAMERA_Z - .5)
                py = round(HEIGHT/2 - FOCAL*y/CAMERA_Z - .5)
                result[px,py] = (entity,coverage,row)
    assert len(result)==12
    return result


PROBES = probe_points()


def expected_color(mode, px, py, amount):
    if mode in ('back','unlit'):
        weight = math.sqrt(.75)*(.96/math.pi)*amount if mode=='back' else 0
        return [base*light*weight for base,light in zip(BASE_COLOR,LIGHT_COLOR,strict=True)]
    # Reuse the scalar GGX oracle with a coordinate frame where the light
    # points toward +Y. The normal and per-pixel view receive the same rotation.
    sx = (px+.5-WIDTH/2)/FOCAL
    sy = (py+.5-HEIGHT/2)/FOCAL
    if mode=='sky':
        # A linear hemispherical sky integrates to one-third horizon
        # and two-thirds zenith or ground under a cosine-weighted lobe.
        nv = 1/math.sqrt(1+sx*sx+sy*sy)
        fresnel = .04+(1-MATERIAL['roughness']-.04)*(1-nv)**5
        return [base*(1-fresnel)*(h/3+2*(z*(1-amount)+g*amount)/3)
                for base,h,z,g in zip(BASE_COLOR,SKY['horizon'],SKY['zenith'],SKY['ground'],strict=True)]
    cosine, sine = math.sqrt(.75), .5
    view = unit((-sx,sy,1))
    toward_light = view[1]*sine + view[2]*cosine
    across_light = view[1]*cosine - view[2]*sine
    normal = (0,cosine,-sine)
    ray = (-view[0]/toward_light,-across_light/toward_light)
    reflected = [base*(1-amount) for base in BASE_COLOR]
    response = directional(normal,ray,reflected,0,MATERIAL['roughness'])
    return [value*light for value,light in zip(response,LIGHT_COLOR,strict=True)]


class ThinSurfaceFixture(SurfaceLayerFixture):
    def setup(self, client):
        path = self.root/'Cutout leaf.glb'
        # Left half of each sheet survives the alpha test. The same geometry
        # and texture are bound to the comparison material.
        image = png(2,1,[255,255,255,255,255,255,255,0])
        doc, blob = quad([image],sampler=dict(magFilter=9728,minFilter=9728))
        path.write_bytes(glb(doc,blob))
        mesh = client.call('asset.import',dict(source=self.native(path)))['asset']
        path.unlink()
        self.measurements['model'] = mesh
        ops = [dict(op='component.remove',id=uid(1),type=name) for name in ('MeshRenderer','BoxCollider')]
        ops += [dict(op='entity.create',id=CAMERA,name='Leaf test camera'),
                component(CAMERA,'Transform',transform([0,0,CAMERA_Z])),component(CAMERA,'Camera',LENS),
                component(uid(4),'LightingEnvironment',ENVIRONMENT),
                *self.sun(back=True)]
        for center, entity in LEAVES:
            ops += [dict(op='entity.create',id=entity,name='Cutout leaf'),
                    component(entity,'Transform',transform([center,0,0])),
                    component(entity,'StaticMesh',dict(asset=mesh,primitive=0,visible=True)),
                    component(entity,'PbrMaterial',MATERIAL)]
        ops += [dict(op='entity.create',id=OCCLUDER,name='Light-side blocker'),
                component(OCCLUDER,'Transform',self.blocker(False)),
                component(OCCLUDER,'MeshRenderer',dict(primitive='box',albedo=[.2,.2,.2],visible=True))]
        self.transact(client,ops,1214)

    @staticmethod
    def sun(*, back):
        half_angle = math.radians(-75 if back else -15)
        return [component(SUN,'Transform',transform([0,0,0],[math.sin(half_angle),0,0,math.cos(half_angle)])),
                component(SUN,'Light',dict(kind='directional',color=LIGHT_COLOR,intensity=1,enabled=True,
                    shadow=dict(enabled=True,distance=9,bias=.0005,normal_bias=.02)))]

    @staticmethod
    def blocker(active):
        position = [-2.22,.968,-1] if active else [40,0,0]
        return dict(transform(position),scale=[.65,.42,.18])

    def use_transmission(self, client):
        assert not client.call('world.dependencies').get('needs_diffuse_transmission')
        changed = dict(MATERIAL,diffuse_transmission=STRENGTH)
        before = self.world.read_bytes()
        for request, invalid in enumerate((dict(changed,diffuse_transmission=True),
                dict(changed,diffuse_transmission=-.01),dict(changed,diffuse_transmission=1.01),
                dict(changed,metallic=.125),dict(changed,double_sided=False)),1220):
            self.reject(client,'world.transact',dict(request_id=uid(request),base_revision=self.revision,
                ops=[component(LIT,'PbrMaterial',invalid)]),-32602)
            assert self.world.read_bytes()==before
        self.transact(client,[component(LIT,'PbrMaterial',changed)],1225)
        assert client.call('world.dependencies')['needs_diffuse_transmission']

    def use_sky(self, client):
        self.transact(client,[component(uid(4),'LightingEnvironment',
                dict(ENVIRONMENT,sky=SKY,ibl=dict(enabled=True,diffuse_intensity=1,specular_intensity=0))),
            component(CAMERA,'Transform',transform([0,CAMERA_Z,0],TOP_DOWN)),
            component(SUN,'Light',dict(kind='directional',color=LIGHT_COLOR,intensity=1,enabled=False)),
            component(OCCLUDER,'Transform',self.blocker(False)),
            *[component(entity,'Transform',transform([center,0,0],TOP_DOWN)) for center,entity in LEAVES]],1228)
        if not self.args.headless:
            self.capture(client,'sky','sky')

    def use_front_light(self, client):
        self.transact(client,[*self.sun(back=False),component(OCCLUDER,'Transform',self.blocker(False)),
            component(uid(4),'LightingEnvironment',ENVIRONMENT),
            component(CAMERA,'Transform',transform([0,0,CAMERA_Z])),
            *[component(entity,'Transform',transform([center,0,0])) for center,entity in LEAVES]],1227)

    def capture(self, client, label, lighting, *, strength=STRENGTH, blocked=False, session=None, material_revision=0):
        params = dict(camera=CAMERA,path=self.native(self.root/(label+'.bmp')),width=WIDTH,height=HEIGHT,
            samples=1,gpu=self.args.gpu,lighting_path=self.args.lighting_path,reconstruction='none',
            capture_frames=1,scene_product_probes=[dict(x=x,y=y) for x,y in PROBES])
        if session:
            params.update(tick=0,session_id=session,material_revision=material_revision)
        else:
            params['revision'] = self.revision
        result = client.call('runtime.capture' if session else 'world.capture',params,timeout=90)
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==1, result
        diagnostics = result['render_diagnostics']
        assert result['nvrhi_errors']==0 and diagnostics['lighting_path']==self.args.lighting_path
        assert (diagnostics['last_draws']['shadow_draws']>0)==(lighting!='sky')
        probes = diagnostics['scene_products']['probes']
        assert len(probes)==len(PROBES)
        maximum = 0
        for probe in probes:
            px,py = probe['x'],probe['y']
            entity, coverage, row = PROBES[px,py]
            if coverage=='hole':
                assert not probe['surface_valid'] and probe['depth']==1, probe
                continue
            assert probe['surface_valid'],probe
            amount = strength if entity==LIT else 0
            mode = 'unlit' if blocked and entity==LIT and row==2 else lighting
            expected = expected_color(mode,px,py,amount)
            vector_near(probe['raw_hdr'],expected,.0025)
            vector_near(probe['shading_normal'],(0,1,0) if lighting=='sky' else (0,0,1),.002)
            near,far = LENS['near'],LENS['far']
            vector_near([probe['depth']],[far/(far-near)*(1-near/CAMERA_Z)],2e-6)
            maximum = max(maximum,*(abs(a-b) for a,b in zip(probe['raw_hdr'],expected,strict=True)))
        image = file_hash(self.root/(label+'.bmp'))
        self.measurements.setdefault('captures',{})[label] = dict(sha256=image,receipt=result,max_hdr_error=maximum)
        return image

    def live_edit(self, client):
        session = uid(1230)
        assert client.call('runtime.start',dict(session_id=session,revision=self.revision))['started']
        for epoch, fraction in enumerate((0,STRENGTH),1):
            book = client.call('runtime.inspect',dict(session_id=session))
            self.transact(client,[component(LIT,'PbrMaterial',dict(MATERIAL,diffuse_transmission=fraction))],1230+epoch)
            request = dict(session_id=session,request_id=uid(1235+epoch),expected_tick=0,
                expected_structure_revision=0,expected_material_revision=epoch-1,revision=self.revision,
                expected_authored_revision=book['authored_revision'],expected_content_sha256=book['content_sha256'],ids=[LIT])
            receipt = client.call('runtime.material.apply',request)
            assert receipt['material_revision']==epoch and receipt['tick']==0
            assert client.call('runtime.material.apply',request)['replayed']
            value = client.call('runtime.material.inspect',dict(session_id=session,tick=0,material_revision=epoch,ids=[LIT]))
            assert value['materials'][0]['material'].get('diffuse_transmission',0)==fraction
            self.measurements['live_edit'] = value
            if not self.args.headless:
                image = self.capture(client,'live-'+str(epoch),'front',strength=fraction,session=session,material_revision=epoch)
                assert (image==self.measurements['captures']['front']['sha256'])==(epoch==2)
        client.call('runtime.stop',dict(session_id=session))

    def relocated_game(self):
        build = self.root/'Leaf sample'
        args = ('project','build',self.native(self.manifest),'--output',self.native(build))
        unsupported = self.root/'No transmission support'
        unsupported.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('diffuse_transmission')
        (unsupported/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        refusal = subprocess.run([str(self.args.binary.resolve()),*args,'--runtime',self.native(unsupported)],
                                 capture_output=True,text=True,encoding='utf-8',timeout=90)
        assert refusal.returncode!=0 and not build.exists(), refusal
        assert any('diffuse transmission requires' in row['message']
                   for row in json.loads(refusal.stdout)['diagnostics']), refusal.stdout
        success(json.loads(self.cli(*args,'--runtime',self.native(self.args.runtime_root))))
        relocated = self.root/'Relocated leaf sample'
        build.rename(relocated)
        self.project.rename(self.root/'Original project hidden')
        assert not build.exists() and not self.project.exists()
        files = tree_hashes(relocated)
        assert self.measurements['model'] in files.values()
        saves = self.root/'Test saves'
        saves.mkdir()
        with self.owner('package',game=relocated/'game.json',saves=saves) as (client,_):
            session = self.initialize(client,self.revision,uid(1250),saves,relocated/'game.json')
            assert self.capture(client,'relocated','front',session=session)==self.measurements['captures']['front']['sha256']
        assert tree_hashes(relocated)==files

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Backlit foliage fixture')))
        self.revision = self.world_call('world.inspect')['revision']
        with self.owner('author') as (client,_):
            self.setup(client)
            if not self.args.headless:
                original = self.capture(client,'original','back',strength=0)
        if self.args.reference:
            with WorldClient.open(self.args.reference.resolve(strict=True),project=self.native(self.manifest)) as old:
                assert self.capture(old,'prior-original','back',strength=0)==original
                assert old.call('session.close')['closed']
            self.measurements['reference_sha256'] = file_hash(self.args.reference)
        with self.owner('light changes') as (client,_):
            self.use_transmission(client)
            if not self.args.headless:
                backlit = self.capture(client,'backlit','back')
                assert backlit!=original
            self.transact(client,[component(OCCLUDER,'Transform',self.blocker(True))],1226)
            if not self.args.headless:
                assert self.capture(client,'blocked','back',blocked=True)!=backlit
            self.use_sky(client)
            self.use_front_light(client)
            if not self.args.headless:
                self.capture(client,'front','front')
            self.live_edit(client)
        if not self.args.headless:
            self.relocated_game()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless',action='store_true')
    mode.add_argument('--runtime-root',type=Path)
    parser.add_argument('--reference',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    args = parser.parse_args()
    if not __debug__ or not 0<=args.gpu<=4095 or (args.reference and args.headless):
        parser.error('Use assertions and an in-range GPU index. The reference image requires Vulkan.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='diffuse-transmission-',dir=args.output.resolve()))
    fixture = ThinSurfaceFixture(args,root)
    report = dict(format='poima.diffuse-transmission-witness.v1',passed=False,headless=args.headless,
                  binary_sha256=file_hash(args.binary),harness_sha256=file_hash(Path(__file__)),
                  calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        report['passed'] = True
    except BaseException:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Evidence file {path}')


if __name__=='__main__':
    main()
