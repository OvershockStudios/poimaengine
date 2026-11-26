#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Observe hardware-tessellated depth, shared edges and cast shadows on Vulkan."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import RpcError, WorldClient, new_id

from gltf_fixture import glb
from scene_capture import pixels
from shadow_index_culling_capture import component, create, transform
from texture_fixture import png, quad

CAMERA, ENV, PLATE, RECEIVER, LIGHT, PLAYER = (f'{i:032x}' for i in range(1,7))
WIDTH, HEIGHT = 601, 401
NEAR, FAR = .1, 30
FOV = 45
DEPTH = 4
PROBES = [(WIDTH//2+x, HEIGHT//2+y) for x,y in [(0,0),(-49,49),(49,-49),(-65,0),(65,0),(0,65)]]
MATERIAL = {'doubleSided': False,
            'pbrMetallicRoughness': {'baseColorFactor': [.6, .6, .6, 1], 'metallicFactor': 0., 'roughnessFactor': .8}}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def duplicated_edge_quad():
    doc, _ = quad([], MATERIAL)
    # Reversed winding along the common edge and different UVs at the seam.
    verts = ((-1,-1,0,0), (1,-1,1,0), (1,1,1,1), (-1,-1,.2,.3), (1,1,.8,.7), (-1,1,.2,.7))
    blob = b''.join(struct.pack('<8f',x,y,0,0,0,1,u,v) for x,y,u,v in verts)
    indices = struct.pack('<6I',*range(6))
    doc['buffers'][0]['byteLength'] = len(blob)+len(indices)
    doc['bufferViews'][0]['byteLength'] = len(blob)
    doc['bufferViews'][1]['byteOffset'] = len(blob)
    for accessor in doc['accessors'][:3]:
        accessor['count'] = 6
    return glb(doc, blob+indices)


def linear_height(x):
    # Two repeating texel centers at x = 0.5 m and x = 1.5 m.
    texel=x-.5
    lower=math.floor(texel)
    weight=texel-lower
    return 1-weight if lower%2 else weight


def expected_depth(px, amplitude, constant, camera_depth=DEPTH):
    ray = ((px+.5-WIDTH*.5)*2/HEIGHT)*math.tan(math.radians(FOV/2))
    world_x = 0.
    for _ in range(32):
        height = constant if constant is not None else linear_height(world_x)
        world_x = ray*(camera_depth-amplitude*height)
    z = camera_depth-amplitude*height
    return (FAR-NEAR*FAR/z)/(FAR-NEAR)


class Fixture:
    def __init__(self,args,report):
        self.args=args
        self.report=report
        self.world=args.output/'world.json'
        self.height = {}
        self.revision = 0

    def native(self,path):
        if self.args.windows_interop:
            return subprocess.check_output(['wslpath','-aw',str(path)],text=True).strip()
        return str(path)

    def edit(self,*ops):
        accepted=self.client.call('world.transact',dict(
            request_id=new_id(),base_revision=self.revision,ops=list(ops)))
        assert accepted['committed'] and accepted['revision']==self.revision+1
        self.revision=accepted['revision']

    def plate(self, amplitude=None, texture='ridge', axis='z', cast=False):
        desc=dict(asset=self.asset,primitive=0,visible=True,cast_shadows=cast)
        if amplitude is not None:
            desc['displacement']=dict(texture=self.height[texture],height_m=amplitude,tile_mm=2000,axis=axis)
        return component(PLATE,'StaticMesh',desc)

    def setup(self):
        source=self.args.output/'split.glb'
        source.write_bytes(duplicated_edge_quad())
        self.asset=self.client.call('asset.import',dict(source=self.native(source)))['asset']
        for name,values in [('ridge',(0,255)),('white',(255,255)),('black',(0,0))]:
            path=self.args.output/(name+'.png')
            path.write_bytes(png(2,1,[v for value in values for v in (value,value,value,255)]))
            self.height[name]=self.client.call('asset.image.import',dict(
                source=self.native(path),color_space='linear'))['asset']
        self.edit(
            create(PLAYER,'Export controller'), transform(PLAYER,(0,0,DEPTH)),
            component(PLAYER,'CharacterController',dict(radius=.2,height=1.5,speed=4,
                jump_speed=4,camera=CAMERA)),
            dict(op='entity.create',id=CAMERA,name='Orthogonal depth witness',parent=PLAYER),
            transform(CAMERA),component(CAMERA,'Camera',dict(vertical_fov=FOV,near=NEAR,far=FAR)),
            create(ENV,'Exposure reference'),
            component(ENV,'LightingEnvironment',dict(ambient=[1]*3,exposure=1,shadow_resolution=1024)),
            create(PLATE,'Split diagonal plate'),transform(PLATE),self.plate())

    def capture(self,label, *, expected, patches, probes=True, **options):
        path=self.args.output/(label+'.bmp')
        request=dict(revision=self.revision,camera=CAMERA,width=WIDTH,height=HEIGHT,samples=1,
                     path=self.native(path),capture_frames=2,lighting_path=self.args.lighting_path,gpu=self.args.gpu)
        if probes:
            request['scene_product_probes']=[dict(x=x,y=y) for x,y in PROBES]
        frame=self.client.call('world.capture',request|options,timeout=60)
        d=frame['render_diagnostics']
        assert frame['hardware'] and frame['nvrhi_errors']==0 and frame['capture_written'],frame
        assert d['material_displacement_supported'] and frame['frames_presented']==2
        draws=d['last_draws']
        assert draws['displacement_requested']==expected and draws['displacement_patches']==patches,draws
        self.report['captures'][label]=dict(sha256=sha(path),result=frame)
        return path,d['scene_products']['probes']

    def depth_witness(self,probe_rows,amplitude,constant, tolerance=2e-6, camera_depth=DEPTH):
        assert len(probe_rows)==len(PROBES)
        errors=[]
        for probe,(x,y) in zip(probe_rows,PROBES,strict=True):
            assert (probe['x'],probe['y'])==(x,y) and probe['surface_valid'],probe
            errors.append(abs(probe['depth']-expected_depth(x,amplitude,constant,camera_depth)))
        assert max(errors)<=tolerance,errors
        return max(errors)

    def shadows(self):
        self.edit(create(RECEIVER,'Backplate'),transform(RECEIVER,(0,0,-2.4),(0,0,0,1),(4,4,1)),
                  component(RECEIVER,'StaticMesh',dict(asset=self.asset,primitive=0,visible=True,cast_shadows=False)),
                  create(LIGHT,'Off-axis spotlight'),transform(LIGHT,(1.3,0,2.8)),
                  component(LIGHT,'Light',dict(kind='spot',color=[1,1,1],intensity=180,range=12,
                    enabled=True,inner_angle=32,outer_angle=49,shadow=dict(enabled=True,distance=10,bias=.0001,normal_bias=.005))),
                  component(ENV,'LightingEnvironment',dict(ambient=[.02]*3,exposure=1,shadow_resolution=1024)),
                  self.plate(None,cast=True))
        flat,_=self.capture('flat-shadow',expected=0,patches=0,probes=False)
        self.edit(self.plate(.65,'white',cast=True))
        shifted,_=self.capture('displaced-shadow',expected=1,patches=4,probes=False)
        self.edit(self.plate(.65,'white',cast=False))
        uncast,_=self.capture('displaced-no-cast',expected=1,patches=2,probes=False)
        a,b,c=(pixels(path) for path in (flat,shifted,uncast))
        # The left-hand region is beyond both plate silhouettes. Separate
        # cast and receive controls identify changes from the shadow pass.
        affected=sum(int(b[y][x][0])+12<int(c[y][x][0]) for y in range(105,294) for x in range(60,155))
        changed=sum(a[y][x]!=b[y][x] for y in range(105,294) for x in range(60,155))
        assert affected>100 and changed>100,(affected,changed)
        self.report['shadow_witness']={'occluded_pixels':affected,'shifted_pixels':changed}

    def motion(self):
        session=new_id()
        assert self.client.call('runtime.start',dict(session_id=session,revision=self.revision))['started']

        def frame(label,tick):
            target=self.args.output/(label+'.bmp')
            result=self.client.call('runtime.capture',dict(session_id=session,tick=tick,camera=CAMERA,
                path=self.native(target),width=WIDTH,height=HEIGHT,capture_frames=1,samples=1,
                gpu=self.args.gpu,lighting_path=self.args.lighting_path,reuse_renderer=True,
                scene_product_probes=[dict(x=x,y=y) for x,y in PROBES]),timeout=60)
            assert result['hardware'] and result['capture_written'] and result['nvrhi_errors']==0,result
            self.report['captures'][label]=dict(sha256=sha(target),result=result)
            return result

        previous=frame('motion-first',0)
        step=self.client.call('runtime.step',dict(session_id=session,request_id=new_id(),
            expected_tick=0,ticks=1,camera_poses=[dict(entity=CAMERA,
                world=dict(position=[.28,0,0],rotation=[0,0,0,1]))]))
        assert step['current_tick']==1
        current=frame('motion-shifted',1)
        assert not previous['renderer_reused'] and current['renderer_reused']
        dx,dy,dz=(b-a for a,b in zip(previous['camera_world'][12:15],current['camera_world'][12:15],strict=True))
        assert abs(dx-.28)<1e-5 and abs(dz)<1e-6,(dx,dy,dz)
        probes=current['render_diagnostics']['scene_products']['probes']
        errors=[]
        for probe in probes:
            assert probe['surface_valid'] and probe['motion_valid'],probe
            distance=NEAR*FAR/(FAR-(FAR-NEAR)*probe['depth'])
            scale=1/(2*math.tan(math.radians(FOV/2))*distance)
            expected=dx*scale*HEIGHT/WIDTH,-dy*scale
            errors.append(max(abs(wanted-actual) for wanted,actual in zip(expected,probe['motion'],strict=True)))
        assert len(errors)==len(PROBES) and max(errors)<4e-6,errors
        assert self.client.call('runtime.capture.release',dict(session_id=session))['released']
        self.client.call('runtime.stop',dict(session_id=session))
        self.report['motion_error_uv']=max(errors)

    def export(self):
        runtime=self.args.runtime.resolve(strict=True)
        root=self.args.output
        project=root/'Source project'
        project.mkdir()
        shutil.copy2(self.world,project/self.world.name)
        assets=self.world.with_name(self.world.name+'.assets')
        shutil.copytree(assets,project/assets.name)
        manifest=project/'project.poimaproject'
        manifest.write_text(json.dumps(dict(format='poima.project',version=1,project_id=new_id(),
            name='Displaced plane fixture',entry=dict(world=self.world.name,camera=CAMERA,controller=PLAYER),
            audio=False,startup_splash=False,startup_render=dict(lighting_path=self.args.lighting_path)))+'\n')

        def cli(binary,*args,okay=True):
            process=subprocess.run([str(binary),*args],capture_output=True,text=True,cwd=root,timeout=90)
            result=json.loads(process.stdout)
            assert (process.returncode==0)==okay,(process.returncode,result,process.stderr)
            return result['result'] if okay else result

        build=('project','build',self.native(manifest))
        old=root/'Runtime without displacement'
        old.mkdir()
        descriptor=json.loads((runtime/'runtime.json').read_bytes())
        assert descriptor['features'].pop('material_displacement')
        (old/'runtime.json').write_text(json.dumps(descriptor))
        denied=root/'Denied'
        rejection=cli(self.args.binary,*build,'--runtime',self.native(old),'--output',self.native(denied),okay=False)
        assert not denied.exists() and any('height displacement' in error['message']
                                           for error in rejection['diagnostics']),rejection
        initial=root/'Initial export'
        cli(self.args.binary,*build,'--runtime',self.native(runtime),'--output',self.native(initial))
        relocated=root/'Relocated export'
        initial.rename(relocated)
        game=relocated/'game.json'
        bundle=json.loads(game.read_bytes())
        assert (relocated/'assets'/(self.height['white']+'.pimage')).is_file() or any(
            relocated.rglob(self.height['white']+'.pimage'))
        replay=root/'Replay.json'
        replay.write_text('[{"ticks":1}]\n')
        detached=root/'Hidden project'
        project.rename(detached)
        try:
            frame=root/'Relocated.bmp'
            played=cli(relocated/bundle['engine']['executable'],'game','run',self.native(game),
                '--replay',self.native(replay),'--capture',self.native(frame),'--width',str(WIDTH),
                '--height',str(HEIGHT),'--samples','1','--gpu',str(self.args.gpu))['play']
            assert played['tick']==1 and played['capture_written'] and played['stop_reason']=='replay_complete',played
            assert played['render_diagnostics']['last_draws']['displacement_patches']==2,played
            assert not project.exists()
            self.report['export']=dict(image_sha256=sha(frame),height_sha256=self.height['white'],
                                       replay_ticks=1,missing_feature_refused=True)
        finally:
            detached.rename(project)

    def run(self):
        with WorldClient.open(self.args.binary,self.native(self.world),cwd=self.args.output,
                              trace_directory=self.args.output/'wire',trace_limit_bytes=8*1024*1024) as client:
            self.client=client
            self.setup()
            base,probes=self.capture('flat',expected=0,patches=0)
            self.report['flat_depth_error']=self.depth_witness(probes,0,0)
            for amplitude in (-.01,4.001,'0.5',False):
                with_bad=self.plate(amplitude)
                try:
                    self.edit(with_bad)
                except RpcError as error:
                    assert error.code==-32602,error
                else:
                    raise AssertionError('Invalid height accepted')
            self.edit(self.plate(0))
            zero,_=self.capture('zero-height',expected=0,patches=0)
            assert sha(zero)==sha(base)
            self.edit(self.plate(.65,'white'))
            white,probes=self.capture('white-height',expected=1,patches=2)
            self.report['white_depth_error']=self.depth_witness(probes,.65,1)
            assert sha(white)!=sha(base)
            self.edit(self.plate(.65,'black'))
            black,probes=self.capture('black-height',expected=1,patches=2)
            self.report['black_depth_error']=self.depth_witness(probes,0,0)
            assert pixels(black)[HEIGHT//2][WIDTH//2]==pixels(base)[HEIGHT//2][WIDTH//2]
            self.edit(self.plate(.65))
            ridge,probes=self.capture('ridge-height',expected=1,patches=2)
            # Coverage through the duplicated diagonal remains lit on both sides.
            image=pixels(ridge)
            seam=[image[HEIGHT//2-offset][WIDTH//2+offset+delta][0]
                  for offset in range(-72,73) for delta in (-1,0,1)]
            assert min(seam)>70,min(seam)
            self.report['ridge_depth_error']=self.depth_witness(probes,.65,None,5e-4)
            self.report['seam_pixels']=len(seam)
            refs=self.client.call('world.asset.references',dict(asset=self.height['ridge']))['edges']
            assert any(ref['component']=='StaticMesh' and ref['path']=='/displacement/texture' for ref in refs),refs
            assert self.client.call('world.dependencies')['needs_material_displacement']
            self.motion()
            self.shadows()
        if self.args.runtime:
            self.export()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--gpu',type=int,required=True)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--runtime',type=Path)
    args=parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.binary=args.binary.resolve(strict=True)
    args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,exe_sha256=sha(args.binary),fixture_sha256=sha(__file__),
                gpu=args.gpu,lighting_path=args.lighting_path,captures={})
    try:
        Fixture(args,report).run()
        report['passed']=True
    except BaseException:
        report['failure']=traceback.format_exc()
        raise
    finally:
        print(args.output/'report.json')
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':
    main()
