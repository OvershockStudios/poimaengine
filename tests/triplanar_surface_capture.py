#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify world-projected material channels on flat, steep, and compound slopes."""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import traceback

from gltf_fixture import glb
from snow_compaction_contract import TOP_DOWN, vector_near
from terrain_surface_layer_capture import SurfaceLayerFixture, blend, directional, dot, linear, unit
from poima_client import WorldClient
from texture_fixture import png
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


WIDTH, HEIGHT, ALTITUDE = 641, 361, 13
LENS = dict(vertical_fov=60, near=.1, far=45)
FOCAL = HEIGHT / (2 * math.tan(math.radians(LENS['vertical_fov']) / 2))
PLANES = (
    (-7, -3, 0, 0), (0, -3, 3, 0), (7, -3, 0, 3),
    (-7, 3, 1.15, -1.3), (0, 3, -3, 0), (7, 3, 0, -3),
)
BASE_REPEAT, LAYER_REPEAT, WEIGHT = 1/4, 1/3, .65
BASE_MATERIAL = dict(base_color=[.9,.85,1], metallic=.61, roughness=.83, emissive=[.04,.03,.02], double_sided=False)
MAPS = {
    'base': ((70,160,30), (180,110,45), (50,120,95), (90,145,190)),
    'layer': ((190,55,30), (52,104,170), (65,86,31), (200,135,62)),
    'base_channels': ((210,80,30), (120,180,70), (175,200,60), (200,110,45)),
    'layer_channels': ((175,110,55), (225,165,12), (200,150,36), (125,38,70)),
    'base_normal': ((119,111,244), (139,126,248), (143,159,244), (129,132,249)),
    'layer_normal': ((162,133,243), (123,146,247), (149,113,244), (152,152,241)),
}
SRGB = ('base', 'layer')
CAMERA, SUN = uid(1050), uid(5)


def normal(plane):
    return unit((-plane[2], 1, -plane[3]))


def ramp(plane, *, split=False):
    cx, cz, sx, sz = plane
    corners = ((-1.15,-1.15), (1.15,-1.15), (1.15,1.15), (-1.15,1.15))
    vertices = [(cx+x, sx*x+sz*z, cz+z) for x,z in corners]
    indices = (0,2,1,0,3,2)
    if split:
        vertices.append((cx,0,cz))
        indices = (0,4,1,1,4,2,2,4,3,3,4,0)
    attributes = tuple(v for p in vertices for v in (*p,*normal(plane)))
    vertex_bytes, index_bytes = len(attributes)*4, len(indices)*4
    blob = struct.pack(f'<{len(attributes)}f{len(indices)}I', *attributes, *indices)
    return glb({
        'asset': {'version': '2.0'},
        'buffers': [{'byteLength': len(blob)}],
        'bufferViews': [{'buffer': 0, 'byteLength': vertex_bytes, 'byteStride': 24},
                        {'buffer': 0, 'byteOffset': vertex_bytes, 'byteLength': index_bytes}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC3',
             'min': list(map(min, zip(*vertices))), 'max': list(map(max, zip(*vertices)))},
            {'bufferView': 0, 'byteOffset': 12, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC3'},
            {'bufferView': 1, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'},
        ],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0,
    }, blob)


def ray_at(px, py):
    return ((px+.5-WIDTH/2)/FOCAL, (py+.5-HEIGHT/2)/FOCAL)


def pixel_samples():
    result = {}
    for plane in PLANES:
        cx,cz,sx,sz = plane
        for dx,dz in ((-.49,-.35), (-.43,.4), (0,0), (.38,-.44), (.42,.35)):
            depth = ALTITUDE - sx*dx - sz*dz
            px = round(WIDTH/2 + FOCAL*(cx+dx)/depth - .5)
            py = round(HEIGHT/2 + FOCAL*(cz+dz)/depth - .5)
            assert (px,py) not in result
            result[px,py] = plane
    return result


PROBES = pixel_samples()


def bilinear(key, uv):
    texels = MAPS[key]
    ix, iy = (2*u-.5 for u in uv)
    x0, y0 = math.floor(ix), math.floor(iy)
    def texel(x, y):
        return tuple(linear(value) if key in SRGB else value/255 for value in texels[(y%2)*2+x%2])
    top = blend(texel(x0,y0), texel(x0+1,y0), ix-x0)
    bottom = blend(texel(x0,y0+1), texel(x0+1,y0+1), ix-x0)
    return blend(top, bottom, iy-y0)


AXES = ((2,1), (0,2), (0,1))


def material_plane(key, point, face, repeat, *, normal_map=False):
    weights = [value**4 for value in face]
    total = sum(weights)
    accum = [0.,0.,0.]
    for weight,(u,v) in zip(weights,AXES,strict=True):
        value = bilinear(key,(point[u]*repeat,point[v]*repeat))
        if normal_map:
            deltas = [2*channel-1 for channel in value]
            for axis, delta in zip((u,v),deltas[:2],strict=True):
                accum[axis] += weight/total * delta/max(deltas[2],.125)
        else:
            for axis, channel in enumerate(value):
                accum[axis] += weight/total * channel
    if normal_map:
        perpendicular = dot(accum,face)
        return unit([s+t-s*perpendicular for s,t in zip(face,accum,strict=True)])
    return accum


def expectation(px, py):
    plane = PROBES[px,py]
    cx, cz, sx, sz = plane
    rx, rz = ray_at(px,py)
    distance = (ALTITUDE + sx*cx + sz*cz) / (1 + sx*rx + sz*rz)
    point = (rx*distance, ALTITUDE-distance, rz*distance)
    assert abs(point[0]-cx)<1.15 and abs(point[2]-cz)<1.15, (point,plane)
    face = normal(plane)
    base = material_plane('base',point,face,BASE_REPEAT)
    top = material_plane('layer',point,face,LAYER_REPEAT)
    base = [a*b for a,b in zip(BASE_MATERIAL['base_color'],base,strict=True)]
    color = blend(base,top,WEIGHT)
    borm = material_plane('base_channels',point,face,BASE_REPEAT)
    torm = material_plane('layer_channels',point,face,LAYER_REPEAT)
    ao, roughness, metallic = blend((borm[0], borm[1]*BASE_MATERIAL['roughness'],
                                     borm[2]*BASE_MATERIAL['metallic']),torm,WEIGHT)
    bn = material_plane('base_normal',point,face,BASE_REPEAT,normal_map=True)
    tn = material_plane('layer_normal',point,face,LAYER_REPEAT,normal_map=True)
    shaded = unit(blend(bn,tn,WEIGHT))
    emission = material_plane('base',point,face,BASE_REPEAT)
    fill = [v*(1-metallic)*ao+e*factor*(1-WEIGHT) for v,e,factor in
            zip(color,emission,BASE_MATERIAL['emissive'],strict=True)]
    # Only the sun's upper hemisphere contributes, including near-grazing ramps.
    direct = directional(shaded,(rx,rz),color,metallic,roughness) if shaded[1]>0 and dot(shaded,(-rx,1,-rz))>0 else (0,0,0)
    near, far = LENS['near'], LENS['far']
    return fill, direct, shaded, far/(far-near)*(1-near/distance)


class TriplanarFixture(SurfaceLayerFixture):
    def setup(self, client):
        self.textures = {}
        for name, texels in MAPS.items():
            path = self.root/(name+'.png')
            path.write_bytes(png(2,2,[value for rgb in texels for value in (*rgb,255)]))
            self.textures[name] = client.call('asset.image.import',
                dict(source=self.native(path),color_space='srgb' if name in SRGB else 'linear'))['asset']
            path.unlink()
        self.layer = dict(base_color=self.textures['layer'], orm=self.textures['layer_channels'],
                          normal=self.textures['layer_normal'], tile_mm=3000, weight=WEIGHT)
        self.material_maps = {slot: dict(asset=self.textures[key]) for slot,key in
            (('base_color','base'), ('metallic_roughness','base_channels'), ('occlusion','base_channels'),
             ('normal','base_normal'), ('emissive','base'))}
        self.draws = {}
        ops = [dict(op='component.remove',id=uid(1),type=name) for name in ('MeshRenderer','BoxCollider')]
        ops += [dict(op='entity.create',id=CAMERA,name='Overhead cliff view'),
                component(CAMERA,'Transform',transform([0,ALTITUDE,0],TOP_DOWN)),
                component(CAMERA,'Camera',LENS),component(uid(4),'LightingEnvironment',dict(ambient=[1]*3,exposure=1)),
                component(SUN,'Transform',transform([0,0,0],TOP_DOWN)),self.light(False)]
        def import_ramp(plane, *, split=False):
            path = self.root/'Original ramp.glb'
            path.write_bytes(ramp(plane,split=split))
            asset = client.call('asset.import',dict(source=self.native(path)))['asset']
            path.unlink()
            return asset

        for index, plane in enumerate(PLANES,1100):
            use_lod = index==1100
            asset = import_ramp(plane,split=use_lod)
            entity = uid(index)
            mesh = dict(asset=asset, primitive=0, visible=True, receive_shadows=False)
            if use_lod:
                mesh['lods'] = [dict(asset=import_ramp(plane),primitive=0,min_distance_m=11)]
            self.draws[entity] = mesh
            ops += [dict(op='entity.create',id=entity,name='Planar cliff'),
                    component(entity,'Transform',transform([0,0,0])),
                    component(entity,'StaticMesh',dict(mesh,surface_layer=self.layer)),
                    component(entity,'PbrMaterial',BASE_MATERIAL)]
        self.transact(client,ops,1106)
        self.measurements['planes'] = self.draws

    @staticmethod
    def light(enabled):
        return component(SUN,'Light',dict(kind='directional',color=[1]*3,intensity=1,enabled=enabled))

    def capture(self, client, label, mode, *, session=None, material_revision=0):
        params = dict(camera=CAMERA,path=self.native(self.root/(label+'.bmp')),width=WIDTH,height=HEIGHT,
            samples=1,gpu=self.args.gpu,lighting_path=self.args.lighting_path,reconstruction='none',
            capture_frames=1,scene_product_probes=[dict(x=x,y=y) for x,y in PROBES])
        if session:
            params.update(tick=0,session_id=session,material_revision=material_revision)
        else:
            params['revision'] = self.revision
        receipt = client.call('runtime.capture' if session else 'world.capture',params,timeout=90)
        assert receipt['hardware'] and receipt['capture_written'] and receipt['frames_presented']==1, receipt
        assert receipt['nvrhi_errors']==0 and receipt['render_diagnostics']['lighting_path']==self.args.lighting_path
        lod = receipt['render_diagnostics']['last_draws']['mesh_lod']
        assert lod['tier_counts'][:2]==[0,1] and lod['sample']==[dict(id=uid(1100),tier=1)], lod
        probes = receipt['render_diagnostics']['scene_products']['probes']
        assert len(probes)==len(PROBES)
        observations = self.measurements.setdefault('captures',{})
        errors = [0.,0.,0.]
        if mode=='direct':
            previous = observations['ambient']['receipt']['render_diagnostics']['scene_products']['probes']
            ambient = {(p['x'],p['y']): p for p in previous}
        for probe in probes:
            assert probe['surface_valid'], probe
            if mode=='xz':
                continue
            pixel = (probe['x'],probe['y'])
            fill,direct,shaded,depth = expectation(*pixel)
            observed = probe['raw_hdr']
            if mode=='direct':
                before = ambient[pixel]
                assert probe['depth']==before['depth'] and probe['shading_normal']==before['shading_normal']
                observed = [a-b for a,b in zip(observed,before['raw_hdr'],strict=True)]
            comparison = ((observed,direct if mode=='direct' else fill,.004),
                          (probe['shading_normal'],shaded,.004),([probe['depth']],[depth],2e-6))
            for index,(actual,wanted,tolerance) in enumerate(comparison):
                vector_near(actual,wanted,tolerance)
                errors[index] = max(errors[index], *(abs(a-b) for a,b in zip(actual,wanted,strict=True)))
        image = file_hash(self.root/(label+'.bmp'))
        observations[label] = dict(sha256=image, receipt=receipt, max_error=errors)
        return image

    def edit_layer_and_test_guards(self, client):
        assert not client.call('world.dependencies').get('needs_surface_layer_triplanar')
        entity, mesh = next(iter(self.draws.items()))
        projected = dict(self.layer, projection='triplanar', base_tile_mm=4000)
        before = self.world.read_bytes()
        for index,(invalid,extra,code) in enumerate((
            (dict(projected,projection=True),[], -32602),
            (dict(projected,projection='cylindrical'),[], -32602),
            (dict(projected,mask=self.textures['layer_channels']),[], -32050),
            (dict(self.layer,projection='triplanar'),[component(entity,'PbrTextures',self.material_maps)], -32050),
        ),1120):
            self.reject(client,'world.transact',dict(request_id=uid(index),base_revision=self.revision,
                ops=[component(entity,'StaticMesh',dict(mesh,surface_layer=invalid)),*extra]),code)
            assert self.world.read_bytes()==before
        ops = []
        for entity,mesh in self.draws.items():
            ops += [component(entity,'StaticMesh',dict(mesh,surface_layer=projected)),
                    component(entity,'PbrTextures',self.material_maps)]
        self.transact(client,ops,1124)
        assert client.call('world.dependencies')['needs_surface_layer_triplanar']
        self.measurements['projected_draws'] = list(self.draws)

    def edit_live_base(self, client):
        session = uid(1130)
        assert client.call('runtime.start',dict(session_id=session,revision=self.revision))['started']
        entity = next(iter(self.draws))
        for epoch in (1,2):
            maps = dict(self.material_maps)
            maps['base_color'] = dict(asset=self.textures['layer' if epoch==1 else 'base'])
            book = client.call('runtime.inspect',dict(session_id=session))
            self.transact(client,[component(entity,'PbrTextures',maps)],1130+epoch)
            result = client.call('runtime.texture.apply',dict(session_id=session,request_id=uid(1140+epoch),
                expected_tick=0,expected_structure_revision=0,expected_material_revision=epoch-1,
                revision=self.revision,expected_authored_revision=book['authored_revision'],
                expected_content_sha256=book['content_sha256'],ids=[entity]))
            assert result['material_revision']==epoch and result['tick']==0, result
        self.measurements['live_edit_result'] = result
        if not self.args.headless:
            assert self.capture(client,'restored-base','direct',session=session,material_revision=2)==self.direct_image
        client.call('runtime.stop',dict(session_id=session))

    def standalone(self):
        out = self.root/'Cliff build'
        build = ('project','build',self.native(self.manifest),'--output',self.native(out))
        old = self.root/'Without triplanar'
        old.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('surface_layer_triplanar') and descriptor['features']['surface_layer']
        (old/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        result = subprocess.run([str(self.args.binary.resolve()),*build,'--runtime',self.native(old)],
                                 capture_output=True,text=True,encoding='utf-8',timeout=90)
        assert result.returncode!=0 and not out.exists(), result
        assert any('triplanar surface layers require' in row['message']
                   for row in json.loads(result.stdout)['diagnostics']), result.stdout
        success(json.loads(self.cli(*build,'--runtime',self.native(self.args.runtime_root))))
        relocated = self.root/'Relocated cliffs'
        out.rename(relocated)
        self.project.rename(self.root/'Detached project')
        assert not out.exists() and not self.project.exists()
        files = tree_hashes(relocated)
        assert set(self.textures.values()).issubset(files.values())
        saves = self.root/'Cliff saves'
        saves.mkdir()
        with self.owner('package',game=relocated/'game.json',saves=saves) as (client,_):
            session = self.initialize(client,self.revision,uid(1150),saves,relocated/'game.json')
            assert self.capture(client,'relocated','direct',session=session)==self.direct_image
        assert tree_hashes(relocated)==files

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Cliff materials')))
        self.revision = self.world_call('world.inspect')['revision']
        with self.owner('author') as (client,_):
            self.setup(client)
            if not self.args.headless:
                original = self.capture(client,'xz','xz')
        if self.args.reference:
            with WorldClient.open(self.args.reference.resolve(strict=True),project=self.native(self.manifest)) as reference:
                assert self.capture(reference,'previous-xz','xz')==original
                assert reference.call('session.close')['closed']
            self.measurements['reference_sha256'] = file_hash(self.args.reference)
        with self.owner('projected edits') as (client,_):
            self.edit_layer_and_test_guards(client)
            if not self.args.headless:
                assert self.capture(client,'ambient','ambient')!=original
            self.transact(client,[self.light(True)],1125)
            if not self.args.headless:
                self.direct_image = self.capture(client,'direct','direct')
                assert self.direct_image!=self.measurements['captures']['ambient']['sha256']
            self.edit_live_base(client)
        if not self.args.headless:
            self.standalone()


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
        parser.error('Use assertions and an in-range GPU index. The retained image reference requires Vulkan.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='triplanar-surface-',dir=args.output.resolve()))
    fixture = TriplanarFixture(args,root)
    report = dict(format='poima.triplanar-surface-witness.v1',passed=False,headless=args.headless,
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
