#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure projected-space PBR filtering on an analytic normal field and a flat control."""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import traceback

from gltf_fixture import glb
from terrain_surface_layer_capture import SurfaceLayerFixture
from texture_fixture import quad
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid
from poima_client import WorldClient


WIDTH, HEIGHT = 321, 193
CAMERA, ROUNDED, FLAT, SUN = map(uid, (370, 371, 372, 5))
FOV, DISTANCE, HALF_WIDTH = 60, 4, .15
METAL, ROUGHNESS, NORMAL_ANGLE = .20, .22, math.radians(50)
FOCAL = HEIGHT / (2*math.tan(math.radians(FOV)/2))
PROBES = [(x, 96) for x in (158, 159, 160, 161, 162, 163, 185)]
STRENGTH = 1


def unit(x, y, z):
    length = math.sqrt(x*x+y*y+z*z)
    return x/length, y/length, z/length


def dot(a, b):
    return sum(x*y for x,y in zip(a,b,strict=True))


def normal(x, phase):
    offset = (x+.5-WIDTH/2+phase) * DISTANCE / FOCAL
    return unit(math.sin(NORMAL_ANGLE)*offset/HALF_WIDTH, 0, math.cos(NORMAL_ANGLE))


def expected_luminance(x, strength, phase):
    # At the center row the directional source is +Z and normals vary only in
    # X. A fine horizontal derivative shares its two lanes across the pair.
    pair = x - x%2
    left, right = normal(pair,phase), normal(pair+1,phase)
    variance = sum((a-b)**2 for a,b in zip(left,right,strict=True)) / math.pi
    a2 = ROUGHNESS**4 + strength*min(variance,.18)
    n = normal(x,phase)
    v = unit(-(x+.5-WIDTH/2)/FOCAL, 0, 1)
    h = unit(v[0], 0, v[2]+1)
    nh, nv, nl = dot(n,h), dot(n,v), n[2]
    ggx = a2 / (math.pi*(1+nh*nh*(a2-1))**2)
    visibility = .5 / (nl*math.sqrt(nv*nv*(1-a2)+a2) + nv*math.sqrt(nl*nl*(1-a2)+a2))
    fresnel = METAL + (1-METAL)*(1-dot(v,h))**5
    return ggx * visibility * fresnel * nl


class SpecularFixture(SurfaceLayerFixture):
    def environment(self, strength=None):
        value = dict(ambient=[0]*3, exposure=1)
        if strength is not None:
            value['geometric_specular_aa'] = strength
        return component(uid(4),'LightingEnvironment',value)

    def camera(self, phase):
        return component(CAMERA,'Transform',transform([phase*DISTANCE/FOCAL,0,DISTANCE]))

    def model(self, client, name, curve):
        source = self.root / (name+'.glb')
        doc, blob = quad([],material=dict(doubleSided=True))
        blob = bytearray(blob)
        doc['accessors'][0]['min'][0] = -HALF_WIDTH
        doc['accessors'][0]['max'][0] = HALF_WIDTH
        for i, (side, height) in enumerate(((-1,-1),(1,-1),(1,1),(-1,1))):
            n = (side*math.sin(NORMAL_ANGLE),0,math.cos(NORMAL_ANGLE)) if curve else (0,0,1)
            struct.pack_into('<6f',blob,32*i,side*HALF_WIDTH,height,0,*n)
        source.write_bytes(glb(doc,bytes(blob)))
        asset = client.call('asset.import',dict(source=self.native(source)))['asset']
        source.unlink()
        return asset

    def author(self, client):
        ops = [dict(op='component.remove',id=uid(1),type=name) for name in ('MeshRenderer','BoxCollider')]
        ops += [dict(op='entity.create',id=CAMERA,name='Specular probe'),self.camera(0),
                component(CAMERA,'Camera',dict(vertical_fov=FOV,near=.1,far=30)),
                self.environment(),component(SUN,'Transform',transform([0,0,0])),
                component(SUN,'Light',dict(kind='directional',color=[1]*3,intensity=1,enabled=True))]
        for entity,x,curve,label in ((ROUNDED,0,True,'Curved normals'),(FLAT,.6,False,'Flat normals')):
            mesh = self.model(client,label,curve)
            ops += [dict(op='entity.create',id=entity,name=label),component(entity,'Transform',transform([x,0,0])),
                    component(entity,'StaticMesh',dict(asset=mesh,primitive=0,visible=True)),
                    component(entity,'PbrMaterial',dict(base_color=[METAL]*3,metallic=1,roughness=ROUGHNESS,
                                                        emissive=[0]*3,double_sided=True))]
        self.transact(client,ops,400)

    def capture(self, client, label, *, session=None, reconstruction='none'):
        filename = self.root/(label+'.bmp')
        args = dict(camera=CAMERA, path=self.native(filename), width=WIDTH, height=HEIGHT,
            samples=1, capture_frames=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
            reconstruction=reconstruction, scene_product_probes=[dict(x=x,y=y) for x,y in PROBES])
        args.update(dict(session_id=session,tick=0) if session else dict(revision=self.revision))
        result = client.call('runtime.capture' if session else 'world.capture',args,timeout=90)
        assert result['capture_written'] and result['hardware'] and result['frames_presented']==1
        assert result['nvrhi_errors']==0 and result['render_diagnostics']['lighting_path']==self.args.lighting_path
        probes = result['render_diagnostics']['scene_products']['probes']
        assert len(probes)==len(PROBES) and all(row['surface_valid'] for row in probes), probes
        record = dict(image=file_hash(filename),result=result)
        self.measurements.setdefault('frames',{})[label] = record
        return record, {row['x']:row for row in probes}

    def visualize(self, client):
        phases, peaks = (0,.25,.5,.75), [[],[]]
        for i,phase in enumerate(phases):
            images = []
            for j,strength in enumerate((0,STRENGTH)):
                label = f'phase-{i}-{j}'
                self.transact(client,[self.camera(phase),self.environment(strength)],420+2*i+j)
                report, probes = self.capture(client,label)
                assert report['result']['lighting'].get('geometric_specular_aa',0)==strength
                for x, _ in PROBES[:-1]:
                    target = expected_luminance(x,strength,phase)
                    for value in probes[x]['raw_hdr']:
                        assert abs(value-target) < max(.001,.023*target),(label,x,value,target)
                peaks[j].append(probes[160]['raw_hdr'][0])
                images.append(probes)
            for x,_ in PROBES:
                for key in ('depth','shading_normal'):
                    assert images[0][x][key]==images[1][x][key],(x,key)
            assert images[0][185]['raw_hdr']==images[1][185]['raw_hdr']
        spans = [max(row)-min(row) for row in peaks]
        assert spans[0]>1 and spans[1]<.5*spans[0],(peaks,spans)
        self.measurements['phase_luminance'] = dict(offsets=phases,disabled=peaks[0],enabled=peaks[1],spans=spans)

    def standalone(self):
        target = self.root/'Packaged metal'
        args = ('project','build',self.native(self.manifest),'--output',self.native(target))
        old = self.root/'Runtime missing specular filtering'
        old.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('geometric_specular_aa')
        (old/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        refusal = subprocess.run([str(self.args.binary.resolve()),*args,'--runtime',self.native(old)],
                                 capture_output=True,text=True,encoding='utf-8',timeout=90)
        self.measurements['legacy_refusal'] = json.loads(refusal.stdout)
        assert refusal.returncode!=0 and not target.exists()
        assert any('geometric specular antialiasing requires' in row['message']
                   for row in self.measurements['legacy_refusal']['diagnostics'])
        success(json.loads(self.cli(*args,'--runtime',self.native(self.args.runtime_root))))
        relocated = self.root/'Relocated metal'
        target.rename(relocated)
        self.project.rename(self.root/'Detached project')
        frozen = tree_hashes(relocated)
        saves = self.root/'Saves'
        saves.mkdir()
        with self.owner('standalone',game=relocated/'game.json',saves=saves) as (client,_):
            session = self.initialize(client,self.revision,uid(500),saves,relocated/'game.json')
            frame,_ = self.capture(client,'relocated',session=session)
        assert frame['image']==self.measurements['frames']['phase-3-1']['image']
        assert tree_hashes(relocated)==frozen

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Specular filtering')))
        self.revision = self.world_call('world.inspect')['revision']
        with self.owner('author') as (client,_):
            self.author(client)
            assert not client.call('world.dependencies').get('needs_geometric_specular_aa')
            schema = client.call('world.describe',dict(view='component',name='LightingEnvironment'))
            field = schema['components']['LightingEnvironment']['properties']['geometric_specular_aa']
            assert all(field[k]==v for k,v in dict(type='number',minimum=0,maximum=1,default=0).items())
            before = self.world.read_bytes()
            for index,invalid in enumerate((-1,1.01,True,'1'),401):
                self.reject(client,'world.transact',dict(base_revision=self.revision,request_id=uid(index),
                    ops=[self.environment(invalid)]),-32602)
            assert self.world.read_bytes()==before
            if not self.args.headless:
                baseline,_ = self.capture(client,'default')
        if not self.args.headless:
            with WorldClient.open(self.args.reference.resolve(strict=True),project=self.native(self.manifest),
                                  trace_directory=self.root/'prior-transport') as old:
                previous,_ = self.capture(old,'prior-default')
                assert old.call('session.close')['closed']
            assert baseline['image']==previous['image']
        with self.owner('measure') as (client,_):
            if not self.args.headless:
                self.visualize(client)
            else:
                self.transact(client,[self.environment(STRENGTH)],430)
            assert client.call('world.dependencies')['needs_geometric_specular_aa']
            if not self.args.headless:
                self.capture(client,'native-aa',reconstruction='fsr3_native')
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
    if not __debug__ or (not args.headless and not args.reference):
        parser.error('Use assertions and supply the reference executable for Vulkan.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='specular-aa-',dir=args.output.resolve()))
    fixture = SpecularFixture(args,root)
    report = dict(format='poima.geometric-specular-aa.v1',passed=False,headless=args.headless,
                  executable_sha256=file_hash(args.binary),harness_sha256=file_hash(Path(__file__)),
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
