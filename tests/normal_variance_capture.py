#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check cooked normal variance against texel coverage and Vulkan GGX oracles."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import traceback

from cutout_mips_capture import read_package
from geometric_specular_aa_capture import (
    DISTANCE, FLAT, FOCAL, HALF_WIDTH, METAL, PROBES, ROUGHNESS, ROUNDED, WIDTH,
    SpecularFixture, dot, unit,
)
from gltf_fixture import glb
from texture_fixture import png, quad
from world_particles_contract import component
from world_terrain_contract import file_hash, success, tree_hashes, uid


SIZE = 64
POSITIVE, NEGATIVE = (181, 128, 243, 73), (74, 128, 243, 19)
PHASES = (-.3,.3,.65)
FILTERS = dict(min_filter=9984, mag_filter=9728, wrap_s=33071, wrap_t=33071)


def expected_length(source, width, height, mip_width, mip_height, x, y):
    # Exact fractional coverage directly from the source texels, independent
    # of the integral-image cooker.
    areas = [(x*width/mip_width,(x+1)*width/mip_width), (y*height/mip_height,(y+1)*height/mip_height)]
    result = [0.0]*3
    total = 0
    for j in range(height):
        wy = max(0,min(j+1,areas[1][1])-max(j,areas[1][0]))
        for i in range(width):
            wx = max(0,min(i+1,areas[0][1])-max(i,areas[0][0]))
            if wx*wy == 0:
                continue
            origin = 4*(j*width+i)
            n = unit(*(2*v-255 for v in source[origin:origin+3]))
            for k in range(3):
                result[k] += n[k]*wx*wy
            total += wx*wy
    return min(255, math.floor(math.hypot(*result)/total*255+.5))


def ggx_probe(encoded, x, apply_variance):
    n = unit(2*encoded[0]-255,255-2*encoded[1],2*encoded[2]-255)
    v = unit(-(x+.5-WIDTH/2)/FOCAL, 0, 1)
    h = unit(v[0], 0, v[2]+1)
    nh, nv, nl = dot(n,h), dot(n,v), n[2]
    r = encoded[3]/255
    variance = min(.18,(1-r*r)/max(2*r*(3-r*r),1e-5)) if apply_variance else 0
    a2 = min(1,ROUGHNESS**4+variance)
    distribution = a2/(math.pi*(1+nh*nh*(a2-1))**2)
    visibility = .5/(nl*math.sqrt(nv*nv*(1-a2)+a2)+nv*math.sqrt(nl*nl*(1-a2)+a2))
    fresnel = METAL+(1-METAL)*(1-dot(v,h))**5
    return distribution*visibility*fresnel*nl


class NormalVarianceFixture(SpecularFixture):
    def model(self, client, name, curve):
        return super().model(client, name, False)

    def image(self, client, name, width, height, source, color_space='linear'):
        path = self.root/(name+'.png')
        path.write_bytes(png(width,height,source))
        receipt = client.call('asset.image.import',dict(source=self.native(path),color_space=color_space))
        path.unlink()
        return receipt['asset']

    def cook(self, client):
        # At mip two, the center separates a balanced tile from a tilted one.
        source = bytes(v for _ in range(SIZE) for x in range(SIZE)
                       for v in (POSITIVE if x%4 < (2 if x//4%2 else 3) else NEGATIVE))
        self.ordinary = self.image(client,'Original fine normals',SIZE,SIZE,source)
        cooked = client.call('asset.image.normal_variance',dict(asset=self.ordinary))
        self.variance = cooked['asset']
        assert cooked['format']=='poima.image.v3' and cooked['alpha_channel']=='mean_normal_length'
        assert cooked==client.call('asset.image.inspect',dict(asset=self.variance))
        assert cooked==client.call('asset.image.normal_variance',dict(asset=self.variance))
        source_path = self.root/'Normal source.glb'
        source_path.write_bytes(glb(*quad([png(SIZE,SIZE,source)],material=dict(normalTexture=dict(index=0)))))
        embedded = client.call('asset.import',dict(source=self.native(source_path)))['asset']
        source_path.unlink()
        assert cooked==client.call('asset.image.normal_variance',dict(asset=embedded,image=0))
        _,_,ordinary = read_package(self.assets/(self.ordinary+'.pimage'))
        path = self.assets/(self.variance+'.pimage')
        magic,meta,self.mips = read_package(path)
        assert magic==b'POIMAI03' and meta['version']==3 and meta['srgb'] is False
        assert self.mips[0][3::4]==bytes((255,))*SIZE**2
        for old,new in zip(ordinary,self.mips,strict=True):
            assert all(old[c::4]==new[c::4] for c in range(3))
        odd_source = bytes(v for y in range(5) for x in range(7) for v in
                           (POSITIVE if (x+3*y)%5>1 else NEGATIVE))
        odd_raw = self.image(client,'Odd dimensions',7,5,odd_source)
        odd = client.call('asset.image.normal_variance',dict(asset=odd_raw))
        _,header,levels = read_package(self.assets/(odd['asset']+'.pimage'))
        counters = []
        for info,level in zip(header['mips'],levels,strict=True):
            w,h = info['width'],info['height']
            expected = [expected_length(odd_source,7,5,w,h,x,y) for y in range(h) for x in range(w)]
            assert list(level[3::4])==expected,(info,list(level[3::4]),expected)
            counters.append(dict(width=w,height=h,length=expected))
        self.measurements['odd_mip_oracle'] = counters
        self.measurements['cooked'] = cooked

        color = self.image(client,'Wrong color space',7,5,odd_source,'srgb')
        self.reject(client,'asset.image.normal_variance',dict(asset=color),-32050)
        self.reject(client,'asset.image.normal_variance',dict(asset=self.ordinary,image=True),-32602)
        self.reject(client,'asset.image.cutout',dict(asset=self.variance,alpha_threshold=128),-32050)
        forged = bytearray(path.read_bytes())
        alpha0 = 16+struct.unpack_from('<I',forged,8)[0]+3
        forged[alpha0] = 254
        digest = hashlib.sha256(forged).hexdigest()
        (self.assets/(digest+'.pimage')).write_bytes(forged)
        self.reject(client,'asset.image.inspect',dict(asset=digest),-32050)

    @staticmethod
    def normal(entity, asset, *, scale=1, min_filter=9984):
        values = FILTERS | dict(asset=asset,min_filter=min_filter)
        return component(entity,'PbrTextures',dict(normal=values,normal_scale=scale))

    def compare(self, client):
        # Nearest-mip selection isolates the mip contents from interpolation.
        screen_width = 2*HALF_WIDTH*FOCAL/DISTANCE
        level = math.floor(math.log2(SIZE/screen_width)+.5)
        assert level==2
        dimension = SIZE>>level
        peaks = [[],[]]
        for i,phase in enumerate(PHASES):
            observations = []
            for j,asset in enumerate((self.ordinary,self.variance)):
                self.transact(client,[self.camera(phase),self.normal(ROUNDED,asset)],701+2*i+j)
                _,rows = self.capture(client,f'motion-{i}-{j}')
                for x,_ in PROBES[:-1]:
                    u = .5+(x+.5-WIDTH/2+phase)/screen_width
                    start = 4*((dimension//2)*dimension+int(dimension*u))
                    expected = ggx_probe(self.mips[level][start:start+4],x,j==1)
                    for value in rows[x]['raw_hdr']:
                        assert abs(value-expected)<max(.001,.03*expected),(x,i,j,value,expected)
                observations.append(rows)
                peaks[j].append(rows[160]['raw_hdr'][0])
            for x,_ in PROBES:
                for field in ('depth','shading_normal'):
                    assert observations[0][x][field]==observations[1][x][field]
            assert observations[0][185]['raw_hdr']==observations[1][185]['raw_hdr']
        span = [max(values)-min(values) for values in peaks]
        assert span[0]>1 and span[1]<span[0]/3,(peaks,span)
        self.measurements['motion'] = dict(phases=PHASES,peaks=peaks,spans=span,level=level)

        for i,(label,selection) in enumerate((('base-mip',dict(min_filter=9728)),('zero-strength',dict(scale=0)))):
            hashes = []
            for j,asset in enumerate((self.ordinary,self.variance)):
                self.transact(client,[self.normal(ROUNDED,asset,**selection)],730+2*i+j)
                frame,_ = self.capture(client,f'{label}-{j}')
                hashes.append(frame['image'])
            assert hashes[0]==hashes[1]
        self.transact(client,[self.normal(ROUNDED,self.variance)],735)
        frame,_ = self.capture(client,'native-aa',reconstruction='fsr3_native')
        assert frame['result']['render_diagnostics']['reconstruction']['active']

    def compare_projection(self, client):
        base = client.call('entity.get',dict(id=ROUNDED,component='StaticMesh'))['value']
        albedo = self.image(client,'Layer color',1,1,bytes((124,124,124,255)),'srgb')
        orm = self.image(client,'Layer roughness',1,1,bytes((255,56,255,255)))
        layer = dict(base_color=albedo,orm=orm,projection='triplanar',tile_mm=300)
        self.transact(client,[self.camera(.08),self.normal(ROUNDED,self.ordinary)],750)
        results = {}
        for index,slot in enumerate(('base','layer')):
            readings = []
            for filtered,asset in enumerate((self.ordinary,self.variance)):
                if slot=='base':
                    surface = layer | dict(base_tile_mm=300,weight=0)
                    base_path = self.normal(ROUNDED,asset)
                else:
                    surface = layer | dict(normal=asset)
                    base_path = self.normal(ROUNDED,self.ordinary)
                self.transact(client,[component(ROUNDED,'StaticMesh',base|dict(surface_layer=surface)),base_path],
                              751+2*index+filtered)
                _,raw = self.capture(client,slot+'-projection-'+str(filtered))
                readings.append(raw)
            original,changed = readings
            for x,_ in PROBES:
                for key in ('depth','shading_normal'):
                    assert original[x][key]==changed[x][key]
            assert max(abs(original[x]['raw_hdr'][0]-changed[x]['raw_hdr'][0])
                       for x,_ in PROBES[:-1])>.015
            assert original[185]['raw_hdr']==changed[185]['raw_hdr']
            results[slot] = [reading[160]['raw_hdr'][0] for reading in readings]
        self.measurements['projection'] = results
        self.transact(client,[self.camera(PHASES[-1]),component(ROUNDED,'StaticMesh',base),
            self.normal(ROUNDED,self.variance)],756)

    def standalone(self):
        output=self.root/'Packaged normals'
        args=('project','build',self.native(self.manifest),'--output',self.native(output))
        old=self.root/'Old decoder'
        old.mkdir()
        descriptor=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('normal_variance_images')
        (old/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        refusal=subprocess.run([str(self.args.binary.resolve()),*args,'--runtime',self.native(old)],
            capture_output=True,text=True,encoding='utf-8',timeout=90)
        assert refusal.returncode!=0 and not output.exists()
        assert any('normal variance images require' in row['message'] for row in json.loads(refusal.stdout)['diagnostics'])
        success(json.loads(self.cli(*args,'--runtime',self.native(self.args.runtime_root))))
        relocated=self.root/'Relocated normals'
        output.rename(relocated)
        self.project.rename(self.root/'Detached project')
        files=tree_hashes(relocated)
        assert self.variance in files.values()
        saves=self.root/'Saves'
        saves.mkdir()
        with self.owner('standalone',game=relocated/'game.json',saves=saves) as (client,_):
            session=self.initialize(client,self.revision,uid(740),saves,relocated/'game.json')
            self.reject(client,'asset.image.normal_variance',dict(asset=self.variance),-32081)
            frame,_=self.capture(client,'relocated',session=session)
            assert frame['image']==self.measurements['frames']['motion-2-1']['image']
        assert tree_hashes(relocated)==files

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Normal variance')))
        self.revision=self.world_call('world.inspect')['revision']
        with self.owner('cook') as (client,_):
            schema=client.call('world.describe',dict(view='method',name='asset.image.normal_variance'))
            assert schema['methods']['asset.image.normal_variance']['required']==['asset']
            self.author(client)
            base=self.world.read_bytes()
            self.cook(client)
            assert self.world.read_bytes()==base
            assert not client.call('world.dependencies').get('needs_normal_variance_images')
            self.transact(client,[component(ROUNDED,'PbrTextures',dict(occlusion=dict(asset=self.variance)))],690)
            self.reject(client,'entity.material',dict(id=ROUNDED,revision=self.revision),-32050)
            self.reject(client,'world.dependencies',{},-32050)
            self.transact(client,[dict(op='component.remove',id=ROUNDED,type='PbrTextures')],691)
            if self.args.headless:
                self.transact(client,[self.normal(ROUNDED,self.variance)],700)
            else:
                self.compare(client)
                self.compare_projection(client)
            assert client.call('world.dependencies')['needs_normal_variance_images']
            result=client.call('entity.material',dict(id=ROUNDED,revision=self.revision))
            assert result['textures']['normal']['alpha_channel']=='mean_normal_length'
        if not self.args.headless:
            self.standalone()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--headless',action='store_true')
    mode.add_argument('--runtime-root',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    args=parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='normal-variance-',dir=args.output.resolve()))
    fixture=NormalVarianceFixture(args,root)
    report=dict(format='poima.normal-variance-witness.v1',passed=False,headless=args.headless,
        executable_sha256=file_hash(args.binary),harness_sha256=file_hash(Path(__file__)),
        calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        report['passed']=True
    except BaseException:
        report['failure']=traceback.format_exc()
        raise
    finally:
        path=root/'evidence.json'
        path.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Evidence file {path}')


if __name__=='__main__':
    main()
