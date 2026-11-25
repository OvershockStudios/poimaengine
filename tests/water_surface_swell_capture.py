#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare a committed water swell with an independent clipped triangle oracle."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from water_listener_audio_contract import filter_error, pcm
from water_optics_capture import AMBIENT, OPTICS, fresnel
from water_refraction_capture import LENS, direction
from water_terrain_fit_capture import AIR, EMITTER, ENTRY, EYE, LISTENER, PITCH, PROBES, PROFILE, SURFACE, TerrainFitContract
from world_particles_contract import near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, bed, cell_center, component, near, rpc, success, uid

LOW=uid(7431)
LOW_EYE=(11.17,4.935,-3.18)
SWELL=[
    dict(wavelength_m=5.,slope=.24,heading_degrees=0,period_ticks=88),
    dict(wavelength_m=4.55,slope=.09,heading_degrees=-43,period_ticks=73),
]
CREST=1
TROUGH=42
POINTS=[ENTRY,(10.9,4.885,-2.5),(10.42,5,-3.11),(11.15,4.96,-3.8),(12.26,4.97,-2.7),
        (13.87,4.91,-3.2),(11.-1e-7,5,-3.17),(11.+1e-7,5,-3.17),(14,5,-3.)]


def sample_wave(x,z,tick):
    value=0
    for wave in SWELL:
        wavelength=wave['wavelength_m']
        angle=math.radians(wave['heading_degrees'])
        arc=math.cos(angle)*(x-10)+math.sin(angle)*(z+4)
        phase=math.tau*(arc/wavelength - (tick%wave['period_ticks'])/wave['period_ticks'])
        value+=wave['slope']*wavelength/math.tau*math.sin(phase)
    return value


def gradient(heights,vertices):
    (ax,az),(bx,bz),(cx,cz)=vertices
    det=(bx-ax)*(cz-az)-(cx-ax)*(bz-az)
    h0,h1,h2=heights
    return (((cz-az)*(h1-h0)-(bz-az)*(h2-h0))/det,
            ((bx-ax)*(h2-h0)-(cx-ax)*(h1-h0))/det)


def weights(vertices,x,z):
    return tuple(value+dx*(x-vertices[0][0])+dz*(z-vertices[0][1])
                 for index,value in enumerate((1.,0.,0.))
                 for dx,dz in (gradient(tuple(float(index==j) for j in range(3)),vertices),))


def polys(tick,opened):
    for ix,iz in CELLS:
        center=cell_center((ix,iz))
        x,z=center
        perimeter=((x-.5,z-.5),(x+.5,z-.5),(x+.5,z+.5),(x-.5,z+.5))
        for side,a in enumerate(perimeter):
            if bed(x,z)>=SURFACE and not (opened and (ix,iz,side)==(0,0,1)):
                continue
            triangle=(center,a,perimeter[(side+1)%4])
            heights=[SURFACE+sample_wave(*point,tick) for point in triangle]
            yield triangle,heights,gradient(heights,triangle)


def wet_length(spans,limit):
    total=end=0.
    for start,stop in sorted(spans):
        stop=min(stop,limit)
        if stop>max(start,end):
            total+=stop-max(start,end)
            end=stop
    return total


def oracle(pixel,probe,tick,opened,low):
    if low:
        x,y,z=LOW_EYE
        sx,dy,sz=direction(pixel,0)
        dx,dz=-sz,sx
        view_cosine=dx
    else:
        x,y,z=EYE
        dx,dy,dz=direction(pixel,PITCH)
        view_cosine=dy*math.sin(math.radians(PITCH))-dz*math.cos(math.radians(PITCH))
    depth=probe['depth']
    far=LENS['near']/(1-depth+depth*(LENS['near']/LENS['far']))/view_cosine
    spans=[]
    interfaces=[]
    for vertices,heights,(gx,gz) in polys(tick,opened):
        at_eye=weights(vertices,x,z)
        one_metre=weights(vertices,x+dx,z+dz)
        top=sum(w*h for w,h in zip(at_eye,heights,strict=True))
        vertical=dy-dx*gx-dz*gz
        inequalities=list(zip(at_eye,(after-before for before,after in zip(at_eye,one_metre,strict=True)),strict=True))
        inequalities.extend(((y-bed(x,z),dy+.25*dx-.125*dz),(top-y,-vertical)))
        start,end=0.,far
        for value,slope in inequalities:
            if slope>0:
                start=max(start,-value/slope)
            elif slope<0:
                end=min(end,-value/slope)
            elif value<0:
                end=-1.
        if start>=end:
            continue
        spans.append((start,end))
        if abs(vertical)>1e-9:
            root=(top-y)/vertical
            if 0<=root<far and start-1e-10<=root<=end+1e-10:
                interfaces.append((root,vertical/math.sqrt(1+gx*gx+gz*gz)))
    length=wet_length(spans,far)
    scatter=OPTICS['scattered_radiance']
    extinction=OPTICS['extinction_per_m']
    mixed=[s+(v-s)*math.exp(-e*length) for v,s,e in zip(probe['raw_hdr'],scatter,extinction,strict=True)]
    crossing=None
    if interfaces:
        root,cosine=min(interfaces)
        before=wet_length(spans,root)
        reflection=AMBIENT if cosine<0 else scatter
        f=fresnel(max(-1.,min(1.,cosine)))
        mixed=[(1-f)*m+f*(s+(r-s)*math.exp(-e*before))
               for m,r,s,e in zip(mixed,reflection,scatter,extinction,strict=True)]
        crossing=dict(distance_m=root,cosine=cosine,ray_y=dy)
    return dict(length_m=length,crossing=crossing,hdr=mixed)


class SwellContract(TerrainFitContract):
    def author(self):
        revision=super().author()
        revision=self.apply(revision,7430,
            dict(op='entity.create',id=LOW,name='Near-horizontal swell camera'),
            component(LOW,'Transform',transform(LOW_EYE,[0,-math.sqrt(.5),0,math.sqrt(.5)])),
            component(LOW,'Camera',LENS))
        self.definition.update(revision=revision,optics=OPTICS|dict(follow_terrain=True),surface_swell=SWELL)
        return revision

    def inspect(self,client,session,tick,opened):
        args=dict(session_id=session,tick=tick)
        result=client.call('runtime.water.inspect',args|dict(cells=CELLS,points=POINTS+[LOW_EYE]))
        assert result['surface_swell']==SWELL
        near(result['volume_m3'],sum(self.definition['initial_depths_m']))
        for row,(x,y,z) in zip(result['points'],POINTS+[LOW_EYE],strict=True):
            candidates=[]
            for triangle,heights,(dx,dz) in polys(tick,opened):
                barycentric=weights(triangle,x,z)
                if min(barycentric)>=-1e-12:
                    sample=sum(w*h for w,h in zip(barycentric,heights,strict=True))
                    length=math.sqrt(dx*dx+1+dz*dz)
                    candidates.append((sample,(-dx/length,1/length,-dz/length)))
            column=row['column']
            if column is None:
                assert x==14
                continue
            near(column['floor_height_m'],bed(x,z))
            top=bed(x,z)
            if candidates:
                # Samples with ambiguous derivatives are kept off fan edges.
                top=max(top,candidates[0][0])
                if top>bed(x,z):
                    assert any(math.dist(column['surface_normal'],n)<2e-11 for _,n in candidates)
            if top<=bed(x,z):
                assert 'surface_normal' not in column
            near(column['surface_height_m'],top)
            near(row['submersion_m'],max(0.,top-y) if y>=bed(x,z) else 0.)
        assert all(row['depth_m']==depth for row,depth in zip(result['cells'],self.definition['initial_depths_m'],strict=True))
        audio=client.call('runtime.audio.inspect',args|dict(listener=LISTENER))['listener_water']
        depth=result['points'][0]['submersion_m']
        blend=min(1,depth/PROFILE['full_depth_m'])
        near(audio['submersion_m'],depth)
        near(audio['target_blend'],blend*blend*(3-2*blend))
        return dict(water=result,audio=audio)

    def capture(self,client,session,tick,label,*,low=False,fsr=False):
        output=self.root/(label+'.bmp')
        result=client.call('runtime.capture',dict(session_id=session,tick=tick,camera=LOW if low else AIR,
            path=self.native(output),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=1,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            reconstruction='fsr3_native' if fsr else 'none',
            scene_product_probes=[dict(x=x,y=y) for x,y in PROBES]))
        assert result['hardware'] and result['capture_written'] and result['frames_presented']==2
        assert result['nvrhi_errors']==0
        diag=result['render_diagnostics']
        assert diag['water_optics']['active'] and diag['water_optics']['surface_swell']
        assert diag['reconstruction']['active'] is fsr
        self.measurements[label]=dict(report=result,sha256=file_hash(output))
        return diag['scene_products']['probes']

    def verify_capture(self,probes,tick,opened,low):
        observations=[]
        for probe,pixel in zip(probes,PROBES,strict=True):
            expect=oracle(pixel,probe,tick,opened,low)
            near_vector(probe['resolved_hdr'],expect['hdr'],tolerance=.003)
            observations.append(dict(pixel=pixel,**expect,actual=probe['resolved_hdr']))
        return observations

    def sound(self,client,session,tick,label):
        path=self.root/(label+'.wav')
        result=client.call('runtime.audio.capture',dict(session_id=session,tick=tick,listener=LISTENER,
            path=self.native(path),frames=4800))
        assert result['capture']['sha256']==file_hash(path)
        samples=pcm(path)
        assert max(map(abs,samples))>.01
        self.measurements[label]=dict(report=result,sha256=file_hash(path))
        return samples

    def run(self):
        revision=self.author()
        plan=rpc('world.water.plan',**self.definition)
        self.cases(*[(name,plan|dict(params=self.definition|extra),-32602) for name,extra in (
            ('short',dict(surface_swell=[SWELL[0]|dict(wavelength_m=3.99)])),
            ('steep',dict(surface_swell=[SWELL[0],SWELL[0]])),
            ('tall',dict(surface_swell=[SWELL[0]|dict(wavelength_m=128)])),
            ('shape',dict(surface_swell=[])),
            ('fit',dict(optics=OPTICS)))])
        expected=self.world_call('world.water.plan',**self.definition)
        pin=expected['plan_sha256']
        assert self.world_call('world.water.cook',**self.definition,expected_plan_sha256=pin)==expected
        artifact=self.assets/(pin+'.pwater')
        assert json.loads(artifact.read_bytes())['version']==11 and file_hash(artifact)==pin
        revision=self.apply(revision,7432,dict(op='water.set',asset=pin))
        assert self.world_call('world.dependencies')['needs_water_surface_swell']
        saves=self.root/'saves'
        saves.mkdir()
        session=uid(7433)
        sampled={}
        with self.owner('committed swell') as (client,_):
            client.call('runtime.start',self.start(session,revision)['params'])
            sampled[0]=self.inspect(client,session,0,False)
            before=self.sound(client,session,0,'dry-pcm')
            client.call('runtime.step',self.step(session,0,1,7434,pressed=True)['params']|
                dict(sounds=[dict(op='play',emitter=EMITTER)]))
            sampled[CREST]=self.inspect(client,session,CREST,True)
            wet=self.sound(client,session,CREST,'crest-pcm')
            blend=sampled[CREST]['audio']['target_blend']
            assert sampled[0]['audio']['target_blend']==0 and blend>.65
            self.measurements['audio_oracle_error']=filter_error(before,wet,lambda _:blend,blend)
            if self.args.gpu is not None:
                self.measurements['crest_oracle']=self.verify_capture(self.capture(client,session,CREST,'crest'),CREST,True,False)
                self.measurements['grazing_oracle']=self.verify_capture(self.capture(client,session,CREST,'crest-low',low=True),CREST,True,True)
                if self.args.fsr:
                    self.capture(client,session,CREST,'crest-fsr',fsr=True)
            client.call('save.configure',dict(request_id=uid(7435),expected_generation=0,root=self.native(saves)))
            client.call('save.write',self.write(session,CREST,'checkpoint',7436)['params'])
            client.call('runtime.step',self.step(session,CREST,TROUGH-CREST,7437)['params'])
            sampled[TROUGH]=self.inspect(client,session,TROUGH,True)
            assert sampled[TROUGH]['audio']['target_blend']==0
            if self.args.gpu is not None:
                self.measurements['trough_oracle']=self.verify_capture(self.capture(client,session,TROUGH,'trough'),TROUGH,True,False)
                assert self.measurements['trough']['sha256']!=self.measurements['crest']['sha256']
        self.measurements['states']=sampled

        if self.args.runtime_root:
            self.relocate_swell(revision,pin,saves,sampled[CREST])
        else:
            with self.owner('fresh swell restore') as (client,_):
                client.call('save.configure',dict(request_id=uid(7438),expected_generation=0,root=self.native(saves)))
                client.call('save.load',self.load(uid(7439),revision,7440)['params'])
                self.confirm_restore(client,uid(7439),sampled[CREST])

    def confirm_restore(self,client,session,expected):
        restored=self.inspect(client,session,CREST,True)
        assert restored==dict(expected,water=dict(expected['water'],session_id=session))
        if self.args.gpu is not None:
            self.capture(client,session,CREST,'cold')
            assert self.measurements['cold']['sha256']==self.measurements['crest']['sha256']
        self.sound(client,session,CREST,'cold-pcm')
        assert self.measurements['cold-pcm']['sha256']==self.measurements['crest-pcm']['sha256']

    def relocate_swell(self,revision,pin,saves,expected):
        from subprocess import run

        output=self.root/'initial export'
        no_swell=self.root/'older runtime'
        no_swell.mkdir()
        manifest=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop('water_surface_swell')
        (no_swell/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        args=['project','build',self.native(self.manifest),'--output',self.native(output),'--runtime']
        rejected=run([str(self.args.binary.resolve()),*args,self.native(no_swell)],capture_output=True,text=True,timeout=60)
        assert rejected.returncode==4 and not output.exists(),rejected
        message=json.loads(rejected.stdout)['diagnostics'][0]['message']
        assert message=='Water surface swell requires a compatible Vulkan runtime.',message
        self.measurements['export_admission']=message
        success(json.loads(self.cli(*args,self.native(self.args.runtime_root))))
        moved=self.root/'relocated export'
        output.rename(moved)
        parked=self.root/'parked project'
        self.project.rename(parked)
        try:
            game=moved/'game.json'
            exe=moved/json.loads(game.read_bytes())['engine']['executable']
            assert success(json.loads(self.cli('game','inspect',self.native(game),executable=exe)))['needs_water_surface_swell']
            asset,=moved.rglob('*.pwater')
            assert file_hash(asset)==pin
            session=uid(7442)
            with self.owner('relocated swell restore',game=game,saves=saves) as (client,_):
                active=client.call('runtime.status')['session_id']
                client.call('save.load',self.load(session,revision,7443,active=active,active_tick=0)['params'])
                self.confirm_restore(client,session,expected)
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
    parser.add_argument('--fsr',action='store_true')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='water-swell-',dir=args.output))
    fixture=SwellContract(args,root)
    evidence=dict(passed=False,calls=fixture.calls,measurements=fixture.measurements)
    try:
        fixture.run()
        logs=[call['wire']['stderr.log'] for call in fixture.calls if 'wire' in call]
        assert logs and all(not any(error in log for error in
            ('VUID-','SYNC-HAZARD','Validation Error','Validation Warning')) for log in logs)
        evidence['passed']=True
    except BaseException:
        evidence['failure']=traceback.format_exc()
        raise
    finally:
        (root/'evidence.json').write_text(json.dumps(evidence,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(f'Swell evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
