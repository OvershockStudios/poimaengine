#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the joined nearshore and offshore media in both ray directions."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from solar_lighting_contract import VALIDATION_LAYER
from water_optics_capture import AMBIENT, OPTICS, fresnel
from water_refraction_capture import LENS, direction
from water_surface_swell_capture import LOW, LOW_EYE, SwellContract, gradient, polys, weights, wet_length
from water_terrain_fit_capture import PROBES, SURFACE
from world_particles_contract import near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import bed, component, near, rpc, success, uid

SEA=dict(edge='max_x',surface_height_m=SURFACE,depth_m=16.)
OCEAN_VIEW,OCEAN_EAR,SHORE_EAR=map(uid,(8101,8102,8103))
OCEAN_EYE=(17.2,6.1,-3.15)
OFFSHORE_EAR=(14.000001,4.8,-3.37)
NEARSHORE_EAR=(13.999999,4.8,-3.37)
SAMPLES=(NEARSHORE_EAR,OFFSHORE_EAR,(12_000,4.61,93),
         (15.,SURFACE,-3.),(14.,SURFACE-16.01,-3.),(13.4,4.7,-2.7),(-30.,4.5,-3.2))


def clip(half_planes,far):
    lo,hi=0.,far
    for value,slope in half_planes:
        if slope>0:
            lo=max(lo,-value/slope)
        elif slope<0:
            hi=min(hi,-value/slope)
        elif value<0:
            return None
    return (lo,hi) if hi>lo else None


def joined_triangles(tick):
    for vertices,heights,_ in polys(tick,False):
        # The outer vertices share the ocean datum. Interior fan vertices
        # retain the original phase and the conserved column stage.
        lifted=[SURFACE if x==14. else v for (x,_),v in zip(vertices,heights,strict=True)]
        yield vertices,lifted,gradient(lifted,vertices)


def compose(pixel,probe,tick,from_sea):
    eye=OCEAN_EYE if from_sea else LOW_EYE
    rotation=-18 if from_sea else 0
    sx,dy,sz=direction(pixel,rotation)
    dx,dz=(sz,-sx) if from_sea else (-sz,sx)
    ray=(dx,dy,dz)
    x,y,z=eye
    view_cosine=dy*math.sin(math.radians(rotation))-sz*math.cos(math.radians(rotation))
    depth=probe['depth']
    far=LENS['near']/(1-depth+depth*LENS['near']/LENS['far'])/view_cosine
    bounds=[]
    crossings=[]
    sea_planes=[(x-14,dx),(y-SURFACE+SEA['depth_m'],dy),(SURFACE-y,-dy)]
    sea_interval=clip(sea_planes,far)
    if sea_interval:
        bounds.append(sea_interval)
        if dy!=0:
            hit=(SURFACE-y)/dy
            if 0<=hit<far and x+hit*dx>=14:
                crossings.append((hit,dy,'ocean'))
    for vertices,heights,(gx,gz) in joined_triangles(tick):
        from_eye=weights(vertices,x,z)
        one_step=weights(vertices,x+dx,z+dz)
        surface=sum(w*v for w,v in zip(from_eye,heights,strict=True))
        dy_local=dy-dx*gx-dz*gz
        edges=[(initial,advance-initial) for initial,advance in zip(from_eye,one_step,strict=True)]
        ground=(y-bed(x,z),dy+.25*dx-.125*dz)
        prism=clip(edges+[ground,(surface-y,-dy_local)],far)
        if not prism:
            continue
        bounds.append(prism)
        if abs(dy_local)>1e-12:
            hit=(surface-y)/dy_local
            if 0<=hit<far and prism[0]-1e-10<=hit<=prism[1]+1e-10:
                cosine=dy_local/math.sqrt(1+gx*gx+gz*gz)
                crossings.append((hit,cosine,'shore'))
    length=wet_length(bounds,far)
    before=0.
    interface=None
    tint=OPTICS['scattered_radiance']
    extinction=OPTICS['extinction_per_m']
    hdr=[s+(v-s)*math.exp(-k*length) for s,v,k in zip(tint,probe['raw_hdr'],extinction,strict=True)]
    if crossings:
        hit,cosine,interface=min(crossings)
        before=wet_length(bounds,hit)
        f=fresnel(max(-1.,min(1.,cosine)))
        sky=AMBIENT if cosine<0 else tint
        hdr=[(1-f)*v+f*(s+(r-s)*math.exp(-k*before))
             for v,s,r,k in zip(hdr,tint,sky,extinction,strict=True)]
    return dict(pixel=pixel,direction=ray,medium_before_m=before,interface=interface,
        coastal_m=length-(sea_interval[1]-sea_interval[0] if sea_interval else 0),
        offshore_m=sea_interval[1]-sea_interval[0] if sea_interval else 0,
        expected=hdr,actual=probe['resolved_hdr'])


class OffshoreContract(SwellContract):
    def author(self):
        revision=super().author()
        p=math.radians(-18)*.5
        # +90 degree yaw rotates forward to -X, keeping the -18 degree pitch.
        t=math.sqrt(.5)
        rotation=[math.sin(p)*t,math.cos(p)*t,-math.sin(p)*t,math.cos(p)*t]
        revision=self.apply(revision,8100,
            dict(op='entity.create',id=OCEAN_VIEW,name='Ocean toward coast'),
            component(OCEAN_VIEW,'Transform',transform(OCEAN_EYE,rotation)),
            component(OCEAN_VIEW,'Camera',LENS),
            dict(op='entity.create',id=OCEAN_EAR,name='Offshore listener'),
            component(OCEAN_EAR,'Transform',transform(OFFSHORE_EAR)),
            dict(op='entity.create',id=SHORE_EAR,name='Nearshore listener'),
            component(SHORE_EAR,'Transform',transform(NEARSHORE_EAR)))
        self.definition=dict(self.definition,revision=revision,optics=self.definition['optics']|dict(offshore=SEA))
        return revision

    def verify_seam(self,client,session,tick):
        args=dict(session_id=session,tick=tick)
        state=client.call('runtime.water.inspect',args|dict(points=SAMPLES))
        row=state['points']
        near(row[0]['column']['surface_height_m'],SURFACE,tolerance=1e-6)
        for i in (1,2,3,4):
            column=row[i]['column']
            assert column==dict(cell=None,offshore=True,floor_height_m=SURFACE-16,
                                surface_height_m=SURFACE,depth_m=16.,surface_normal=[0,1,0]),column
        near(row[1]['submersion_m'],SURFACE-OFFSHORE_EAR[1])
        near(row[2]['submersion_m'],SURFACE-4.61)
        assert row[3]['submersion_m']==row[4]['submersion_m']==0 and row[6]['column'] is None
        near(state['volume_m3'],sum(self.definition['initial_depths_m']))
        listeners=[client.call('runtime.audio.inspect',args|dict(listener=listener))['listener_water']
                   for listener in (SHORE_EAR,OCEAN_EAR)]
        for i,result in enumerate(listeners):
            near(result['submersion_m'],row[i]['submersion_m'])
            assert result['target_blend']==1
        return dict(state=state,listeners=listeners)

    def image(self,client,session,tick,label,from_sea):
        path=self.root/(label+'.bmp')
        result=client.call('runtime.capture',dict(session_id=session,tick=tick,camera=OCEAN_VIEW if from_sea else LOW,
            path=self.native(path),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=1,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            scene_product_probes=[dict(x=x,y=y) for x,y in PROBES]))
        assert result['hardware'] and result['capture_written'] and result['nvrhi_errors']==0 and result['frames_presented']==2
        diag=result['render_diagnostics']
        assert diag['water_optics']['offshore']
        rows=[compose(pixel,probe,tick,from_sea)
              for pixel,probe in zip(PROBES,diag['scene_products']['probes'],strict=True)]
        for i in rows:
            near_vector(i['actual'],i['expected'],tolerance=.003)
        self.measurements[label]=dict(sha256=file_hash(path),adapter=result['gpu'],samples=rows)

    def configure(self,client):
        client.call('save.configure',dict(request_id=uid(8118),expected_generation=0,root=self.native(self.root/'saves')))

    def run(self):
        revision=self.author()
        amended=self.definition['optics']
        self.cases(*[(label,rpc('world.water.plan',**(self.definition|dict(optics=value))),-32602)
            for label,value in (
                ('bad_side',amended|dict(offshore=SEA|dict(edge='up'))),
                ('bad_depth',amended|dict(offshore=SEA|dict(depth_m=True))),
                ('bad_top',amended|dict(offshore=SEA|dict(surface_height_m=2e6))),
                ('needs_fit',amended|dict(follow_terrain=False))
            )])
        plan=self.world_call('world.water.plan',**self.definition)
        pin=plan['plan_sha256']
        assert self.world_call('world.water.cook',**self.definition,expected_plan_sha256=pin)==plan
        source=self.assets/(pin+'.pwater')
        assert file_hash(source)==pin and json.loads(source.read_bytes())['version']==12
        revision=self.apply(revision,8110,dict(op='water.set',asset=pin))
        assert self.world_call('world.dependencies')['needs_water_offshore']
        (self.root/'saves').mkdir()
        session=uid(8111)
        with self.owner('water seam') as (client,_):
            client.call('runtime.start',self.start(session,revision)['params'])
            initial=self.verify_seam(client,session,0)
            client.call('runtime.step',self.step(session,0,45,8112)['params'])
            at_tick=self.verify_seam(client,session,45)
            assert initial['state']['points'][5]['column']['surface_height_m']!=at_tick['state']['points'][5]['column']['surface_height_m']
            if self.args.gpu is not None:
                self.image(client,session,45,'from-shore',False)
                self.image(client,session,45,'from-ocean',True)
                hits=[r for name in ('from-shore','from-ocean') for r in self.measurements[name]['samples']]
                assert sum(r['coastal_m']>.01 and r['offshore_m']>.01 for r in hits)>=8
                assert any(r['interface']=='ocean' for r in hits)
            self.configure(client)
            client.call('save.write',self.write(session,45,'checkpoint',8113)['params'])
        self.measurements['point_checks']=at_tick
        if self.args.runtime_root:
            self.relocated(revision,pin,at_tick)
        else:
            with self.owner('fresh ocean phase') as (client,_):
                self.configure(client)
                fresh=uid(8114)
                client.call('save.load',self.load(fresh,revision,8115)['params'])
                self.compare_restore(client,fresh,at_tick)

    def compare_restore(self,client,session,before):
        after=self.verify_seam(client,session,45)
        assert after==dict(before,state=dict(before['state'],session_id=session))
        if self.args.gpu is not None:
            self.image(client,session,45,'cold-ocean',True)
            assert self.measurements['cold-ocean']['sha256']==self.measurements['from-ocean']['sha256']

    def relocated(self,revision,pin,expected):
        first=self.root/'initial export'
        older=self.root/'runtime missing offshore'
        older.mkdir()
        manifest=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop('water_offshore')
        (older/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        args=['project','build',self.native(self.manifest),'--output',self.native(first),'--runtime']
        rej=subprocess.run([str(self.args.binary.resolve()),*args,self.native(older)],capture_output=True,text=True,timeout=60)
        assert rej.returncode==4 and not first.exists(),rej
        assert json.loads(rej.stdout)['diagnostics'][0]['message']=='Offshore water requires a compatible Vulkan runtime.'
        success(json.loads(self.cli(*args,self.native(self.args.runtime_root))))
        target=self.root/'relocated export'
        first.rename(target)
        parked=self.root/'parked project'
        self.project.rename(parked)
        try:
            game=target/'game.json'
            binary=target/json.loads(game.read_bytes())['engine']['executable']
            assert success(json.loads(self.cli('game','inspect',self.native(game),executable=binary)))['needs_water_offshore']
            assets=list(target.rglob('*.pwater'))
            assert len(assets)==1 and file_hash(assets[0])==pin
            with self.owner('relocated ocean phase',game=game,saves=self.root/'saves') as (client,_):
                active=client.call('runtime.status')['session_id']
                session=uid(8116)
                client.call('save.load',self.load(session,revision,8117,active=active,active_tick=0)['params'])
                self.compare_restore(client,session,expected)
        finally:
            parked.rename(self.project)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--require-validation',action='store_true')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='water-offshore-',dir=args.output))
    fixture=OffshoreContract(args,root)
    evidence=dict(passed=False,calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        logs=[call['wire']['stderr.log'] for call in fixture.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in
            ('VUID-','SYNC-HAZARD','Validation Error','Validation Warning')) for log in logs)
        if args.require_validation:
            assert any(VALIDATION_LAYER in log for log in logs), 'Khronos validation did not load.'
        evidence['passed']=True
    except BaseException:
        evidence['failure']=traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Offshore evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
