#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify world-space terrain material blending in a native Vulkan render."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from snow_compaction_contract import SnowContract, TOP_DOWN, vector_near
from texture_fixture import png
from world_particles_contract import component, transform
from world_terrain_contract import encode, file_hash, success, tree_hashes, uid
from poima_client import WorldClient


WIDTH, HEIGHT = 641, 321
CAMERA, SUN = uid(960), uid(5)
TILES = [uid(i) for i in range(961, 965)]
POSITION = [70.5, 15, 28]
LENS = dict(near=.1, far=50, vertical_fov=70)
FOCAL = HEIGHT / (2*math.tan(math.radians(LENS['vertical_fov'])/2))
RELIEF = .08
BASE = ((200,40,90), (20,130,200))
TOP = ((170,90,40), (30,180,70))
ORM, NORMAL = (179,100,73), (167,133,243)
EMISSIVE = (.05,.02,.03)
SLOPES = [20,50]
STATIONS = [(x,z) for z in (22.2, 34.0, 34.6) for x in (66.8,69.4,71.93,72.0,72.09,74.6,77.2)]


def unit(vector):
    length = math.hypot(*vector)
    return tuple(v/length for v in vector)


def dot(a, b):
    return sum(x*y for x,y in zip(a,b,strict=True))


def blend(a, b, weight):
    return tuple(x+(y-x)*weight for x,y in zip(a,b,strict=True))


def linear(byte):
    value = byte/255
    return value/12.92 if value <= .04045 else ((value+.055)/1.055)**2.4


def color_at(texels, u):
    position = 2*u-.5
    left = math.floor(position)
    return blend(tuple(map(linear,texels[left%2])),tuple(map(linear,texels[(left+1)%2])),position-left)


def pixel(x, z):
    depth = POSITION[1] - RELIEF - max(z - 28, 0)*.75
    return (round(WIDTH/2+(x-POSITION[0])*FOCAL/depth-.5), round(HEIGHT/2+(z-POSITION[2])*FOCAL/depth-.5))


PROBES = list(dict.fromkeys(map(lambda p: pixel(*p), STATIONS)))


def directional(n, ray, color, metallic, roughness):
    view = unit((-ray[0],1,-ray[1]))
    halfway = unit((view[0],view[1]+1,view[2]))
    nl, nv, nh, vh = n[1], dot(n,view), dot(n,halfway), dot(view,halfway)
    a2 = roughness**4
    distribution = a2/(math.pi*(nh*nh*(a2-1)+1)**2)
    visibility=.5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2))
    fresnel = [.04+(base-.04)*metallic for base in color]
    fresnel = [f+(1-f)*(1-vh)**5 for f in fresnel]
    return tuple(((1-f)*(1-metallic)*b/math.pi+distribution*visibility*f)*nl
                 for f,b in zip(fresnel,color,strict=True))


def expectation(px, py, mode):
    ray = ((px+.5-WIDTH/2)/FOCAL,(py+.5-HEIGHT/2)/FOCAL)
    slope = .75 if ray[1] > 0 else 0
    distance = (POSITION[1]-RELIEF)/(1+slope*ray[1])
    x = POSITION[0] + ray[0]*distance
    u = (x-64)/16
    normal = unit((0,1,-slope))
    base = color_at(BASE, x/2 if mode in ('mask','slope','lit') else u)
    coverage = max(0,min(1,2*u-.5))*.8 if mode in ('mask','slope','lit') else 0
    if mode in ('slope','lit'):
        frac = max(0,min(1,(math.degrees(math.atan(slope))-SLOPES[0])/(SLOPES[1]-SLOPES[0])))
        coverage *= frac*frac*(3-2*frac)
    color = blend(base,color_at(TOP,x/4),coverage)
    sampled = [v*2/255-1 for v in NORMAL]
    bitangent = (0,-normal[2],normal[1])
    top_normal = unit((sampled[0], bitangent[1]*sampled[1]+normal[1]*sampled[2],
                       bitangent[2]*sampled[1]+normal[2]*sampled[2]))
    n = unit(blend(normal,top_normal,coverage))
    ao = 1+(ORM[0]/255-1)*coverage
    roughness = .75+(ORM[1]/255-.75)*coverage
    metal = .1+(ORM[2]/255-.1)*coverage
    fill = [b*(1-metal)*ao+e*(1-coverage) for b,e in zip(color,EMISSIVE,strict=True)]
    if mode=='lit':
        fill = [a+b for a,b in zip(fill,directional(n,ray,color,metal,roughness),strict=True)]
    near,far = LENS['near'],LENS['far']
    return fill,n,far/(far-near)*(1-near/distance)


class SurfaceLayerFixture(SnowContract):
    def transact(self, client, ops, request):
        change = client.call('world.transact',dict(request_id=uid(request),base_revision=self.revision,ops=ops))
        assert change['committed'] and change['revision']==self.revision+1, change
        self.revision=change['revision']

    def setup(self, client):
        recipe=encode(dict(format='poima.terrain.heightfield',version=2,samples_x=17,samples_z=17,
            tile_cells=8,origin_m=[64,0,20],spacing_m=[1,1],
            heights_m=[.75*max(z-8,0) for z in range(17) for _ in range(17)],
            lods=[dict(stride=4,min_distance_m=13.3)]))
        plan=client.call('world.terrain.inspect',dict(recipe=recipe))
        assert client.call('world.terrain.cook',dict(recipe=recipe,expected_recipe_sha256=plan['recipe_sha256']))==plan
        self.measurements['terrain']=plan

        def texture(name, colors, srgb):
            path=self.root/(name+'.png')
            path.write_bytes(png(len(colors),1,[v for rgb in colors for v in (*rgb,255)]))
            result=client.call('asset.image.import',dict(source=self.native(path),color_space='srgb' if srgb else 'linear'))
            path.unlink()
            return result['asset']

        height=texture('White height',[(255,255,255)],False)
        self.base=texture('First layer',BASE,True)
        self.layer=dict(base_color=texture('Second layer',TOP,True), orm=texture('ORM',[ORM],False),
                        mask=texture('Painted weight',[(0,0,0),(255,255,255)],False),
                        normal=texture('Projected normal',[NORMAL],False),tile_mm=4000,weight=.8)
        self.areas = [dict(tile['static_mesh'],receive_shadows=False,
                           displacement=dict(texture=height,tile_mm=4000,axis='y',height_m=RELIEF)) for tile in plan['tiles']]
        ops=[dict(op='component.remove',id=uid(1),type=name) for name in ('MeshRenderer','BoxCollider')]
        ops += [dict(op='entity.create',id=CAMERA,name='Overhead terrain view'),
                component(CAMERA,'Transform',transform(POSITION,TOP_DOWN)),component(CAMERA,'Camera',LENS),
                component(uid(4),'LightingEnvironment',dict(ambient=[1]*3,exposure=1,shadow_resolution=1024)),
                component(SUN,'Transform',transform([0,0,0],TOP_DOWN)), self.light(False)]
        for entity,primitive in zip(TILES,self.areas,strict=True):
            ops += [dict(op='entity.create',id=entity,name='Painted terrain'),component(entity,'Transform',transform([0,0,0])),
                    component(entity,'StaticMesh',primitive),component(entity,'PbrTextures',dict(base_color=dict(asset=self.base))),
                    component(entity,'PbrMaterial',dict(base_color=[1]*3,emissive=EMISSIVE,metallic=.1,roughness=.75,double_sided=False))]
        self.transact(client,ops,970)

    def light(self, enabled):
        return component(SUN,'Light',dict(kind='directional',color=[1]*3,intensity=1,enabled=enabled,
            shadow=dict(enabled=enabled,distance=30,bias=.0001,normal_bias=.001)))

    def edit_layer(self, client, layer, index, *extra):
        ops=[component(entity,'StaticMesh',dict(mesh,surface_layer=layer)) for entity,mesh in zip(TILES,self.areas,strict=True)]
        self.transact(client,ops+list(extra),index)

    def capture(self, client, label, mode, *, session=None, material_revision=0):
        params=dict(camera=CAMERA,path=self.native(self.root/(label+'.bmp')),width=WIDTH,height=HEIGHT,samples=1,
            gpu=self.args.gpu,lighting_path=self.args.lighting_path,reconstruction='none',capture_frames=1,
            scene_product_probes=[dict(x=x,y=y) for x,y in PROBES])
        if session:
            params.update(tick=0,session_id=session,material_revision=material_revision)
        else:
            params['revision']=self.revision
        result=client.call('runtime.capture' if session else 'world.capture',params,timeout=90)
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==1 and result['nvrhi_errors']==0,result
        d=result['render_diagnostics']
        assert d['lighting_path']==self.args.lighting_path and d['last_draws']['mesh_lod']['tier_counts'][:2]==[1,3],d['last_draws']
        assert d['last_draws']['displacement_draws']>0
        if mode=='lit':
            assert d['last_draws']['shadow_draws']>0
            ambient = self.measurements['captures']['slope']['receipt']['render_diagnostics']['scene_products']['probes']
            ambient = {(probe['x'], probe['y']): probe for probe in ambient}
        maximum=[0.,0.,0.]
        combined_hdr_error = 0.
        for actual in d['scene_products']['probes']:
            assert actual['surface_valid'],actual
            x,y = actual['x'],actual['y']
            color,normal,depth=expectation(x,y,mode)
            observed_color = actual['raw_hdr']
            if mode=='lit':
                # Separate the independently verified ambient pass from the direct-light increment.
                observed_fill = ambient[x,y]
                assert actual['shading_normal'] == observed_fill['shading_normal']
                assert actual['depth'] == observed_fill['depth']
                combined_hdr_error = max(combined_hdr_error, *(abs(a-b) for a,b in zip(observed_color,color,strict=True)))
                fill,_,_ = expectation(x,y,'slope')
                observed_color = [a-b for a,b in zip(observed_color,observed_fill['raw_hdr'],strict=True)]
                color = [a-b for a,b in zip(color,fill,strict=True)]
            vector_near(observed_color,color,.0035)
            vector_near(actual['shading_normal'],normal,.004)
            vector_near([actual['depth']],[depth],2e-6)
            for i,(a,b) in enumerate(((observed_color,color),(actual['shading_normal'],normal),([actual['depth']],[depth]))):
                maximum[i]=max(maximum[i],max(abs(x-y) for x,y in zip(a,b,strict=True)))
        record=dict(sha256=file_hash(self.root/(label+'.bmp')),receipt=result,max_error=maximum)
        if mode=='lit':
            record['combined_hdr_error'] = combined_hdr_error
        self.measurements.setdefault('captures',{})[label]=record
        return record['sha256']

    def live_edit(self, client):
        session=uid(992)
        assert client.call('runtime.start',dict(session_id=session,revision=self.revision))['started']
        for epoch,value in enumerate((None,dict(base_color=dict(asset=self.base))),1):
            book=client.call('runtime.inspect',dict(session_id=session))
            removal=[dict(op='component.remove',id=entity,type='PbrTextures') if value is None else
                     component(entity,'PbrTextures',value) for entity in TILES]
            self.transact(client,removal,993+epoch)
            result=client.call('runtime.texture.apply',dict(session_id=session,request_id=uid(996+epoch),expected_tick=0,
                expected_structure_revision=0,expected_material_revision=epoch-1,revision=self.revision,
                expected_authored_revision=book['authored_revision'],expected_content_sha256=book['content_sha256'],ids=TILES))
            assert result['material_revision']==epoch and result['tick']==0,result
        self.measurements['live_edit_result']=result
        if not self.args.headless:
            assert self.capture(client,'live-layer','lit',session=session,material_revision=2)==self.measurements['captures']['lit']['sha256']
        client.call('runtime.stop',dict(session_id=session))

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Surface Layers')))
        self.revision=self.world_call('world.inspect')['revision']
        with self.owner('source') as (client,_):
            self.setup(client)
            if not self.args.headless:
                base=self.capture(client,'base','base')
        if self.args.reference:
            with WorldClient.open(self.args.reference.resolve(strict=True),project=self.native(self.manifest)) as old:
                assert self.capture(old,'old-base','base')==base
                assert old.call('session.close')['closed']
            self.measurements['reference_sha256']=file_hash(self.args.reference)
        with self.owner('layer edits') as (client,_):
            self.edit_layer(client,dict(self.layer,weight=0),971)
            before=self.world.read_bytes()
            for request,invalid,code in ((973,dict(self.layer,orm=self.layer['base_color']),-32050),
                    (974,dict(self.layer,slope_degrees=[40,20]),-32602),(975,dict(self.layer,tile_mm=True),-32602)):
                self.reject(client,'world.transact',dict(base_revision=self.revision,request_id=uid(request),
                    ops=[component(TILES[0],'StaticMesh',dict(self.areas[0],surface_layer=invalid))]),code)
                assert self.world.read_bytes()==before
            edges=client.call('world.asset.references',dict(owner=dict(kind='entity',id=TILES[0])))['edges']
            assert [row['path'] for row in edges if row['path'].startswith('/surface_layer/')]==[
                '/surface_layer/'+name for name in ('base_color','mask','normal','orm')]
            assert client.call('world.dependencies')['needs_surface_layer']
            if not self.args.headless:
                assert self.capture(client,'zero','base')==base
            for index,(mode,additions) in enumerate((('mask',{}),('slope',dict(slope_degrees=SLOPES)),('lit',dict(slope_degrees=SLOPES))),980):
                self.edit_layer(client,dict(self.layer,base_tile_mm=2000,**additions),index,self.light(mode=='lit'))
                if not self.args.headless:
                    assert self.capture(client,mode,mode)!=base
            self.live_edit(client)
        if self.args.headless:
            return
        out=self.root/'Packaged terrain'
        build=('project','build',self.native(self.manifest),'--output',self.native(out))
        old=self.root/'Runtime without layer support'
        old.mkdir()
        descriptor=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('surface_layer')
        descriptor['features'].pop('surface_layer_triplanar',None)
        (old/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        refusal=subprocess.run([str(self.args.binary.resolve()),*build,'--runtime',self.native(old)],
                               capture_output=True,text=True,encoding='utf-8',timeout=90)
        assert refusal.returncode!=0 and not out.exists()
        assert any('surface layers require' in d['message'] for d in json.loads(refusal.stdout)['diagnostics']),refusal.stdout
        success(json.loads(self.cli(*build,'--runtime',self.native(self.args.runtime_root))))
        relocated=self.root/'Relocated terrain'
        out.rename(relocated)
        self.project.rename(self.root/'Detached project')
        assert not out.exists() and not self.project.exists()
        frozen=tree_hashes(relocated)
        assert all(self.layer[key] in frozen.values() for key in ('base_color','orm','mask','normal'))
        saves=self.root/'Game saves'
        saves.mkdir()
        with self.owner('package',game=relocated/'game.json',saves=saves) as (client,_):
            session=self.initialize(client,self.revision,uid(999),saves,relocated/'game.json')
            assert self.capture(client,'relocated','lit',session=session)==self.measurements['captures']['lit']['sha256']
        assert tree_hashes(relocated)==frozen


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path)
    mode=p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless',action='store_true')
    mode.add_argument('--runtime-root',type=Path)
    p.add_argument('--reference',type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--windows-interop',action='store_true')
    p.add_argument('--gpu',type=int,default=0)
    p.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    args=p.parse_args()
    if not __debug__ or not 0<=args.gpu<=4095 or (args.reference and args.headless):
        p.error('Use assertions and an in-range GPU index. The retained image reference requires Vulkan.')
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='terrain-surface-layer-',dir=args.output.resolve()))
    fixture=SurfaceLayerFixture(args,root)
    result=dict(format='poima.terrain-surface-layer-witness.v1',passed=False,headless=args.headless,
                binary_sha256=file_hash(args.binary),harness_sha256=file_hash(Path(__file__)),
                calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        result['passed']=True
    except BaseException:
        result['failure']=traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Evidence file {root / "evidence.json"}')


if __name__=='__main__':
    main()
