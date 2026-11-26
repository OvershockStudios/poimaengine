#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check cooked cutout mips, Vulkan distance coverage, and standalone assets."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import traceback

from gltf_fixture import glb
from scene_capture import pixels
from terrain_surface_layer_capture import SurfaceLayerFixture
from poima_client import WorldClient
from texture_fixture import png, quad
from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid


WIDTH, HEIGHT, TEXTURE_SIZE = 769, 513, 128
LENS = dict(near=.1,far=120,vertical_fov=57)
FOCAL = HEIGHT / (2*math.tan(math.radians(LENS['vertical_fov'])/2))
GREEN, PADDING = (35,197,54,255), (230,18,224,0)
CAMERA, PLAIN, COVERED, RECEIVER, SUN = [uid(i) for i in (1280,1281,1282,1283,5)]
CENTERS = ((PLAIN,-1.35),(COVERED,1.35))
MATERIAL = dict(base_color=[1]*3,emissive=[0]*3,roughness=.7,metallic=0,
                double_sided=True,alpha_mode='mask',alpha_cutoff=.5)


def read_package(path):
    data = path.read_bytes()
    header,binary = struct.unpack_from('<II',data,8)
    assert len(data)==16+header+binary
    info = json.loads(data[16:16+header])
    offset = 16+header
    levels = []
    for mip in info['mips']:
        stop = offset+mip['bytes']
        levels.append(data[offset:stop])
        offset = stop
    assert offset==len(data)
    return data[:8],info,levels


def green(rgb):
    return rgb[1]>70 and rgb[1]>1.7*max(rgb[0],rgb[2])


def projected_pixels(center_x,center_y,depth,extent=1):
    scale = FOCAL/depth
    half = extent*scale
    cx,cy = WIDTH/2+center_x*scale,HEIGHT/2-center_y*scale
    for y in range(max(0,math.ceil(cy-half-.5)),min(HEIGHT,math.ceil(cy+half-.5))):
        for x in range(max(0,math.ceil(cx-half-.5)),min(WIDTH,math.ceil(cx+half-.5))):
            yield x,y,(x+.5-cx+half)/(2*half),(y+.5-cy+half)/(2*half)


class CutoutFixture(SurfaceLayerFixture):
    def import_assets(self, client):
        pattern = bytes(channel for y in range(TEXTURE_SIZE) for x in range(TEXTURE_SIZE)
            for channel in (GREEN if x%2==0 and y%2==0 else PADDING))
        image = png(TEXTURE_SIZE,TEXTURE_SIZE,pattern)
        source = self.root/'Original cutout.png'
        source.write_bytes(image)
        imported = client.call('asset.image.import',dict(source=self.native(source),color_space='srgb'))
        source.unlink()
        self.plain = imported['asset']
        assert imported['format']=='poima.image.v1'
        model_path = self.root/'Original cutout.glb'
        doc,blob = quad([image])
        model_path.write_bytes(glb(doc,blob))
        self.mesh = client.call('asset.import',dict(source=self.native(model_path)))['asset']
        model_path.unlink()
        self.measurements['source_images'] = dict(image=self.plain,model=self.mesh)
        signature,_,self.plain_mips = read_package(self.assets/(self.plain+'.pimage'))
        assert signature==b'POIMAI01' and self.plain_mips[0]==pattern
        assert all(max(level[3::4])<128 for level in self.plain_mips[1:])
        return imported

    def try_odd_image(self, client):
        source = self.root/'Odd alpha texture.png'
        # Four colored samples exercise partial texel areas and zero support
        # when the 7x5 image is reduced to 3x2.
        source.write_bytes(png(7,5,[v for y in range(5) for x in range(7)
            for v in (GREEN if (x,y) in ((0,0),(3,0),(0,4),(6,4)) else PADDING)]))
        image = client.call('asset.image.import',dict(source=self.native(source),color_space='linear'))['asset']
        source.unlink()
        cutoff = 128
        result = client.call('asset.image.cutout',dict(asset=image,alpha_threshold=cutoff))
        _,_,old = read_package(self.assets/(image+'.pimage'))
        _,meta,baked = read_package(self.assets/(result['asset']+'.pimage'))
        assert old[0]==baked[0]
        for old_mip, new_mip, level in zip(old[1:],baked[1:],meta['mips'][1:],strict=True):
            count = level['width']*level['height']
            expected = min((4*count*2+35)//70,sum(a>0 for a in old_mip[3::4]))
            assert sum(a>=cutoff for a in new_mip[3::4])==expected
            assert all(a!=0 or b==0 for a,b in zip(old_mip[3::4],new_mip[3::4],strict=True))
        self.measurements['odd_image'] = result

    def cook(self, client):
        before = self.world.read_bytes()
        initial = {p.name for p in self.assets.iterdir()}
        for invalid in (0,256,True,128.0):
            self.reject(client,'asset.image.cutout',dict(asset=self.plain,alpha_threshold=invalid),-32602)
        self.reject(client,'asset.image.cutout',dict(asset=self.mesh,image=1,alpha_threshold=128),-32050)
        assert {p.name for p in self.assets.iterdir()}==initial
        args = dict(asset=self.plain,alpha_threshold=128)
        cooked = client.call('asset.image.cutout',args)
        self.covered = cooked['asset']
        assert cooked==client.call('asset.image.cutout',dict(asset=self.mesh,image=0,alpha_threshold=128))
        assert cooked==client.call('asset.image.cutout',dict(asset=self.covered,alpha_threshold=128))
        assert cooked==client.call('asset.image.cutout',args)
        assert cooked==client.call('asset.image.inspect',dict(asset=self.covered))
        assert cooked['format']=='poima.image.v2' and cooked['alpha_threshold']==128 and self.covered!=self.plain
        path = self.assets/(self.covered+'.pimage')
        signature,meta,self.covered_mips = read_package(path)
        assert signature==b'POIMAI02' and meta['alpha_coverage_threshold']==128 and file_hash(path)==self.covered
        assert self.covered_mips[0]==self.plain_mips[0]
        for n,(level,new) in enumerate(zip(cooked['mips'],self.covered_mips,strict=True)):
            count = level['width']*level['height']
            target = (count+2)//4
            assert level['covered_texels']==target==sum(a>=128 for a in new[3::4])
            if n:
                assert all(new[i:i+3]==bytes(GREEN[:3]) for i in range(0,len(new),4))
        self.try_odd_image(client)
        # Typed v2 metadata rejects a well-hashed package with an invalid cutoff.
        payload = b''.join(self.covered_mips)
        header = json.dumps(meta | dict(alpha_coverage_threshold=True),separators=(',',':')).encode()
        malformed = b'POIMAI02'+struct.pack('<II',len(header),len(payload))+header+payload
        ident = hashlib.sha256(malformed).hexdigest()
        (self.assets/(ident+'.pimage')).write_bytes(malformed)
        self.reject(client,'asset.image.inspect',dict(asset=ident),-32050)
        assert self.world.read_bytes()==before
        self.measurements['cooked_image'] = cooked

    def map(self, entity, asset, *, filtered=False):
        return component(entity,'PbrTextures',dict(base_color=dict(asset=asset,wrap_s=33071,wrap_t=33071,
            min_filter=9987 if filtered else 9984,mag_filter=9729 if filtered else 9728)))

    @staticmethod
    def camera(depth):
        return component(CAMERA,'Transform',transform([0,0,depth]))

    @staticmethod
    def sun(enabled, casts=False):
        return component(SUN,'Light',dict(kind='directional',color=[1]*3,intensity=1,enabled=enabled,
            shadow=dict(enabled=casts,distance=80,bias=.0005,normal_bias=.02)))

    def setup(self, client):
        ops = [dict(op='component.remove',id=uid(1),type=name) for name in ('MeshRenderer','BoxCollider')]
        ops += [dict(op='entity.create',id=CAMERA,name='Cutout distance camera'),self.camera(4),
                component(CAMERA,'Camera',LENS),
                component(uid(4),'LightingEnvironment',dict(ambient=[1]*3,exposure=1,shadow_resolution=256)),self.sun(False)]
        for entity, center in CENTERS:
            ops += [dict(op='entity.create',id=entity,name='Thin repeated cutout'),
                    component(entity,'Transform',transform([center,0,0])),
                    component(entity,'StaticMesh',dict(asset=self.mesh,primitive=0,visible=True)),
                    component(entity,'PbrMaterial',MATERIAL),self.map(entity,self.plain)]
        self.transact(client,ops,1290)

    def capture(self, client, label, *, session=None):
        path = self.root/(label+'.bmp')
        params = dict(camera=CAMERA,path=self.native(path),width=WIDTH,height=HEIGHT,samples=1,
            gpu=self.args.gpu,lighting_path=self.args.lighting_path,reconstruction='none',capture_frames=1)
        if session:
            params.update(tick=0,session_id=session)
        else:
            params['revision'] = self.revision
        result = client.call('runtime.capture' if session else 'world.capture',params,timeout=90)
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==1,result
        diagnostics = result['render_diagnostics']
        assert result['nvrhi_errors']==0 and diagnostics['lighting_path']==self.args.lighting_path
        self.measurements.setdefault('captures',{})[label] = dict(sha256=file_hash(path),receipt=result)
        return pixels(path)

    def compare_nearest(self, image, depth, covered, label):
        wavelength = TEXTURE_SIZE*depth/(2*FOCAL)
        level = max(0,math.floor(math.log2(wavelength)+.5))
        dimension = TEXTURE_SIZE >> level
        statistics = {}
        for index,(_,center) in enumerate(CENTERS):
            mip = self.covered_mips[level] if index==1 and covered else self.plain_mips[level]
            tested = seen = 0
            for x,y,u,v in projected_pixels(center,0,depth):
                a,b = dimension*u,dimension*v
                # Exclude points exactly on a texel transition or the raster edge.
                if min(abs(a-round(a)),abs(b-round(b)))<2e-4:
                    continue
                expected = mip[(int(b)*dimension+int(a))*4+3]>=128
                observed = green(image[y][x])
                assert observed==expected, (label,level,index,x,y,u,v,expected,image[y][x])
                tested += 1
                seen += observed
            assert tested>256
            statistics['adjusted' if index else 'ordinary'] = dict(tested=tested,surviving=seen)
        self.measurements.setdefault('nearest',{})[label] = dict(mip=level,**statistics)

    def compare_trilinear(self, image):
        results = []
        for _,center in CENTERS:
            mask = [green(image[y][x]) for x,y,_,_ in projected_pixels(center,0,24,.96)]
            results.append(dict(covered=sum(mask),pixels=len(mask)))
        assert results[0]['covered']==0
        assert results[1]['covered']/results[1]['pixels']>=.08,results
        self.measurements['trilinear'] = results

    def shadow_trial(self, client):
        half = math.radians(-22.5)
        ops = [component(uid(4),'LightingEnvironment',dict(ambient=[.04]*3,exposure=1,shadow_resolution=256)),
               component(SUN,'Transform',transform([0,0,0],[math.sin(half),0,0,math.cos(half)])),
               self.sun(True),
               dict(op='entity.create',id=RECEIVER,name='Shadow receiver'),
               component(RECEIVER,'Transform',dict(transform([0,-.7,-4.05]),scale=[6,2.9,.1])),
               component(RECEIVER,'MeshRenderer',dict(primitive='box',visible=True,albedo=[.8]*3,cast_shadows=False))]
        for entity,center in CENTERS:
            ops += [component(entity,'Transform',transform([center,3.3,0])),
                    self.map(entity,self.covered if entity==COVERED else self.plain)]
        self.transact(client,ops,1296)
        if not self.args.headless:
            unshadowed = self.capture(client,'shadows-off')
        self.transact(client,[self.sun(True,True)],1297)
        if not self.args.headless:
            shadowed = self.capture(client,'shadows-on')
            stats = []
            for _,center in CENTERS:
                points = list(projected_pixels(center,-.7,28,.77))
                darker = sum(unshadowed[y][x][0]-shadowed[y][x][0]>12 for x,y,_,_ in points)
                stats.append(dict(shadowed=darker,pixels=len(points)))
            assert stats[1]['shadowed']>stats[0]['shadowed']+20,stats
            self.measurements['shadows'] = stats
            assert self.measurements['captures']['shadows-on']['receipt']['render_diagnostics']['last_draws']['shadow_draws']>0

    def standalone(self):
        build = self.root/'Portable cutouts'
        args = ('project','build',self.native(self.manifest),'--output',self.native(build))
        stripped = self.root/'Runtime without the image format'
        stripped.mkdir()
        descriptor = json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert descriptor['features'].pop('alpha_coverage_images')
        (stripped/'runtime.json').write_text(json.dumps(descriptor),encoding='utf-8')
        result = subprocess.run([str(self.args.binary.resolve()),*args,'--runtime',self.native(stripped)],
                                capture_output=True,text=True,encoding='utf-8',timeout=90)
        assert result.returncode!=0 and not build.exists(), result
        assert any('coverage-adjusted images require' in row['message']
                   for row in json.loads(result.stdout)['diagnostics']),result.stdout
        success(json.loads(self.cli(*args,'--runtime',self.native(self.args.runtime_root))))
        relocated = self.root/'Relocated cutouts'
        build.rename(relocated)
        self.project.rename(self.root/'Source project detached')
        assert not build.exists() and not self.project.exists()
        files = tree_hashes(relocated)
        assert self.covered in files.values()
        saves = self.root/'Cutout saves'
        saves.mkdir()
        with self.owner('package',game=relocated/'game.json',saves=saves) as (client,_):
            session = self.initialize(client,self.revision,uid(1300),saves,relocated/'game.json')
            assert client.call('asset.image.inspect',dict(asset=self.covered))==self.measurements['cooked_image']
            self.reject(client,'asset.image.cutout',dict(asset=self.plain,alpha_threshold=128),-32081)
            self.capture(client,'relocated',session=session)
        assert self.measurements['captures']['relocated']['sha256']==self.measurements['captures']['shadows-on']['sha256']
        assert tree_hashes(relocated)==files

    def run(self):
        success(json.loads(self.cli('project','create',self.native(self.project),'--name','Cutout mip witness')))
        self.revision = self.world_call('world.inspect')['revision']
        with self.owner('author') as (client,_):
            imported = self.import_assets(client)
            self.setup(client)
            if not self.args.headless:
                self.compare_nearest(self.capture(client,'original'),4,False,'original')
        if self.args.reference:
            with WorldClient.open(self.args.reference.resolve(strict=True),project=self.native(self.manifest)) as old:
                assert old.call('asset.image.inspect',dict(asset=self.plain))==imported
                self.capture(old,'prior-original')
                assert old.call('session.close')['closed']
            assert self.measurements['captures']['prior-original']['sha256']==self.measurements['captures']['original']['sha256']
            self.measurements['reference_sha256'] = file_hash(self.args.reference)
        with self.owner('derive and use') as (client,_):
            self.cook(client)
            assert not client.call('world.dependencies').get('needs_alpha_coverage_images')
            self.transact(client,[self.map(COVERED,self.covered)],1291)
            assert client.call('world.dependencies')['needs_alpha_coverage_images']
            if not self.args.headless:
                self.compare_nearest(self.capture(client,'near'),4,True,'near')
                assert self.measurements['captures']['near']['sha256']==self.measurements['captures']['original']['sha256']
            for index,depth in enumerate((24,48),1292):
                self.transact(client,[self.camera(depth)],index)
                if not self.args.headless:
                    label = f'nearest-{depth}'
                    self.compare_nearest(self.capture(client,label),depth,True,label)
            self.transact(client,[self.camera(24),self.map(PLAIN,self.plain,filtered=True),
                                 self.map(COVERED,self.covered,filtered=True)],1294)
            if not self.args.headless:
                self.compare_trilinear(self.capture(client,'trilinear'))
            self.shadow_trial(client)
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
        parser.error('Use assertions and an in-range GPU index. A reference capture requires Vulkan.')
    args.output.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='cutout-mips-',dir=args.output.resolve()))
    fixture = CutoutFixture(args,root)
    report = dict(format='poima.cutout-mips-witness.v1',passed=False,headless=args.headless,
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
