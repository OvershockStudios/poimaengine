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
from functools import lru_cache

from height_fog_capture import environment
from solar_lighting_contract import VALIDATION_LAYER
from water_optics_capture import OPTICS, fresnel
from water_refraction_capture import LENS, direction
from water_surface_swell_capture import LOW, LOW_EYE, SWELL, SwellContract, gradient, polys, weights, wet_length
from water_terrain_fit_capture import PROBES, SURFACE
from world_particles_contract import near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import bed, component, near, rpc, success, uid

SEA=dict(edge='max_x',surface_height_m=SURFACE,depth_m=16.)
OCEAN_VIEW,OCEAN_EAR,SHORE_EAR=map(uid,(8101,8102,8103))
OCEAN_EYE=(17.2,6.1,-3.15)
GRAZING_VIEW=uid(8104)
PATCH_VIEW=uid(8105)
CAMERAS={
    'shore': (LOW,LOW_EYE,0,False),
    'ocean': (OCEAN_VIEW,OCEAN_EYE,-18,True),
    'grazing': (GRAZING_VIEW,(13.0,5.42,-3.2),-.8,False),
    'patches': (PATCH_VIEW,(65.2,6.85,-55.12),-14,True),
}
VARIATION=dict(scale_m=54.,strength=.72,seed=0x6a09e667)
REFLECTION_ROUGHNESS=.32
GRAZING_PROBES=[(x,y) for x in (279,320,365) for y in (174,175,176,177,178,180,183,189,200,232,270,310)]
AMBIENT=(.68,.52,.88)
FOG=dict(density_per_m=.026,base_height_m=4.93,height_falloff_per_m=0,start_distance_m=.1,max_distance_m=38,
         ambient_radiance=[.32,.24,.18],anisotropy=0)
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


def patch_values(x,z,profile):
    u,v=x/profile['scale_m'],z/profile['scale_m']
    ix,iz=math.floor(u),math.floor(v)
    corners=[]
    for dx,dz in ((0,0),(1,0),(0,1),(1,1)):
        bits=((ix+dx)%1048576)*0x8da6b343 ^ ((iz+dz)%1048576)*0xd8163841 ^ profile['seed']
        bits &= 0xffffffff
        for shift,multiplier in ((16,0x7feb352d),(15,0x846ca68b)):
            bits=((bits^(bits>>shift))*multiplier)%2**32
        bits^=bits>>16
        corners.append([(bits%65536)/32767.5-1,(bits//65536)/32767.5-1])
    a,b=(t**2*(3-2*t) for t in (u-ix,v-iz))
    basis=((1-a)*(1-b),a*(1-b),(1-a)*b,a*b)
    return [sum(weight*corner[channel] for weight,corner in zip(basis,corners,strict=True))
            for channel in (0,1)]


def band_sample(wave,x,z,tick,profile):
    heading=math.radians(wave['heading_degrees'])
    e=(math.cos(heading),math.sin(heading))
    k=math.tau/wave['wavelength_m']
    phase=k*(x*e[0]+z*e[1])-math.tau*(tick%wave['period_ticks'])/wave['period_ticks']
    amplitude=wave['slope']/k
    if profile:
        dx,dz=patch_values(x,z,profile)
        phase+=2.5*profile['strength']*(dx*e[0]+dz*e[1])/math.sqrt(2)
        amplitude*=1-.55*profile['strength']*(dz+1)/2
    return amplitude*math.sin(phase),phase


def joined_triangles(tick,profile=None):
    for vertices,heights,_ in polys(tick,False):
        # The outer vertices share the ocean datum. Interior fan vertices
        # retain the original phase and the conserved column stage.
        if profile:
            heights=[SURFACE+sum(band_sample(wave,x-10,z+4,tick,profile)[0] for wave in SWELL)
                     for x,z in vertices]
        lifted=[SURFACE if x==14. else v for (x,_),v in zip(vertices,heights,strict=True)]
        yield vertices,lifted,gradient(lifted,vertices)


def sightline(pixel,rotation,from_sea):
    sx,dy,sz=direction(pixel,rotation)
    dx,dz=(sz,-sx) if from_sea else (-sz,sx)
    view_cosine=dy*math.sin(math.radians(rotation))-sz*math.cos(math.radians(rotation))
    return (dx,dy,dz),view_cosine


def wave_slopes(pixel,rotation,from_sea,ray,view_cosine,hit,eye,tick,profile=None):
    # Sample nearby rays at the same view depth, then project their derivative
    # onto the horizontal datum. Limit the grazing projection as in the contract.
    def on_view_plane(i,j):
        direction,cosine=sightline((i,j),rotation,from_sea)
        return [v*hit*view_cosine/cosine for v in direction]

    footprint=[]
    for di,dj in ((.001,0),(0,.001)):
        before=on_view_plane(pixel[0]-di,pixel[1]-dj)
        after=on_view_plane(pixel[0]+di,pixel[1]+dj)
        px,py,pz=((b-a)/.002 for a,b in zip(before,after,strict=True))
        dy=math.copysign(max(.01,abs(ray[1])),ray[1])
        footprint.append((px-ray[0]*py/dy,pz-ray[2]*py/dy))
    x=eye[0]+ray[0]*hit-10
    z=eye[2]+ray[2]*hit+4
    filtered=[0.,0.]
    unfiltered=[0.,0.]
    variance=0.
    for wave in SWELL:
        heading=math.radians(wave['heading_degrees'])
        unit=(math.cos(heading),math.sin(heading))
        k=math.tau/wave['wavelength_m']
        phase=k*(x*unit[0]+z*unit[1])-math.tau*(tick%wave['period_ticks'])/wave['period_ticks']
        slope=[math.cos(phase)*wave['slope']*axis for axis in unit]
        frequency=[k*axis for axis in unit]
        second_moment=.5*wave['slope']**2
        if profile:
            # Differentiate the scalar height and phase by symmetric samples.
            # The shader uses the analytic derivatives and no extra samples.
            quarter=[]
            for i,(dx,dz) in enumerate(((1e-4,0),(0,1e-4))):
                a=band_sample(wave,x-dx,z-dz,tick,profile)
                b=band_sample(wave,x+dx,z+dz,tick,profile)
                slope[i]=(b[0]-a[0])/.0002
                frequency[i]=(b[1]-a[1])/.0002
                quarter_tick=tick+wave['period_ticks']/4
                a=band_sample(wave,x-dx,z-dz,quarter_tick,profile)
                b=band_sample(wave,x+dx,z+dz,quarter_tick,profile)
                quarter.append((b[0]-a[0])/.0002)
            second_moment=.5*sum(value**2 for value in slope+quarter)
        support=math.hypot(*(a*frequency[0]+b*frequency[1] for a,b in footprint))
        weight=max(0.,min(1.,(support-.5)/(math.pi-.5)))
        coverage=1-weight*weight*(3-2*weight)
        variance+=second_moment*(1-coverage**2)
        for i,value in enumerate(slope):
            filtered[i]+=value*coverage
            unfiltered[i]+=value
    return filtered,unfiltered,variance


@lru_cache(maxsize=1)
def brdf_samples():
    return [((j+.5)/512,math.cos(phi),math.sin(phi))
            for j in range(512) for phi in (math.tau*int(f'{j:09b}'[::-1],2)/512,)]


@lru_cache(maxsize=4096)
def reflection_texel(x,y):
    nv=(x+.5)/64
    alpha=((y+.5)/64)**2
    a2=alpha**2
    vx=math.sqrt(1-nv*nv)
    length=math.hypot(alpha*vx,nv)
    tx,tz=alpha*vx/length,nv/length
    view_length=math.sqrt(nv*nv+(1-nv*nv)*a2)
    visible=2*nv/(nv+view_length)
    f0=((1.333-1)/(1.333+1))**2
    total=0.
    # Integrate reflected energy for a uniform unit-radiance hemisphere,
    # with visible GGX normals and height-correlated Smith masking.
    for u,cos_phi,sin_phi in brdf_samples():
        hz=1-u*(1+tz)
        r=math.sqrt(max(0.,1-hz*hz))
        hx,hy,hz=alpha*(tx+r*cos_phi),alpha*r*sin_phi,tz+hz
        scale=1/math.hypot(hx,hy,hz)
        vh=min(1.,(vx*hx+nv*hz)*scale)
        nl=2*vh*hz*scale-nv
        if min(vh,nl)>0:
            divisor=nl*view_length+nv*math.sqrt(nl*nl*(1-a2)+a2)
            energy=f0+(1-f0)*(1-vh)**5
            total+=energy*2*nv*nl/(max(divisor,1e-6)*visible)
    return total/512


def rough_response(cosine,roughness):
    u,v=[max(0.,min(63.,a*64-.5)) for a in (cosine,roughness)]
    x,y=int(u),int(v)
    sx,sy=u-x,v-y
    return min(1.,sum(a*b*reflection_texel(i,j)
        for i,a in ((x,1-sx),(min(x+1,63),sx)) for j,b in ((y,1-sy),(min(y+1,63),sy))))


def haze(color,distance):
    length=max(0.,min(FOG['max_distance_m'],distance)-FOG['start_distance_m'])
    weight=math.exp(-length*FOG['density_per_m'])
    return [base+(value-base)*weight for base,value in zip(FOG['ambient_radiance'],color,strict=True)]


def channel_delta(a,b):
    return max(abs(x-y) for x,y in zip(a,b,strict=True))


def compose(pixel,probe,tick,camera,profile=None,roughness=None):
    _,eye,rotation,from_sea=CAMERAS[camera]
    ray,view_cosine=sightline(pixel,rotation,from_sea)
    dx,dy,dz=ray
    x,y,z=eye
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
    for vertices,heights,(gx,gz) in joined_triangles(tick,profile):
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
    solid_distance=far if depth<1 else FOG['max_distance_m']
    distance=solid_distance
    no_filter=faceted=regular=base_rough=hdr
    effective_roughness=roughness
    if crossings:
        hit,cosine,interface=min(crossings)
        before=wet_length(bounds,hit)
        from_air=cosine<0
        sky=AMBIENT if from_air else tint

        def reflected(incidence,width=None):
            f=fresnel(max(-1.,min(1.,incidence)))
            if from_air and width is not None:
                f=rough_response(min(abs(incidence),1.),width)
            return [(1-f)*v+f*(s+(r-s)*math.exp(-k*before))
                    for v,s,r,k in zip(hdr,tint,sky,extinction,strict=True)]

        def slope_incidence(slope):
            gx,gz=slope
            signed=(dy-gx*dx-gz*dz)/math.hypot(gx,1,gz)
            return min(-1e-15,signed) if from_air else max(0.,signed)

        filtered,unfiltered,variance=wave_slopes(pixel,rotation,from_sea,ray,view_cosine,hit,eye,tick,profile)
        if roughness is not None:
            effective_roughness=min(1.,roughness**4+variance)**.25
        if from_air and before<=.0001:
            distance=hit
        no_filter=reflected(slope_incidence(unfiltered),roughness)
        faceted=reflected(cosine,effective_roughness)
        if profile:
            periodic,_,_=wave_slopes(pixel,rotation,from_sea,ray,view_cosine,hit,eye,tick)
            regular=reflected(slope_incidence(periodic),effective_roughness)
        incidence=slope_incidence(filtered)
        base_rough=reflected(incidence,roughness)
        hdr=reflected(incidence,effective_roughness)
        if not profile:
            regular=hdr
    expected=haze(hdr,distance)
    return dict(pixel=pixel,direction=ray,medium_before_m=before,interface=interface,
        coastal_m=length-(sea_interval[1]-sea_interval[0] if sea_interval else 0),
        offshore_m=sea_interval[1]-sea_interval[0] if sea_interval else 0,
        expected=expected,actual=probe['resolved_hdr'],solid_hit=depth<1,
        fog_surface_m=distance,filter_delta=channel_delta(expected,haze(no_filter,distance)),
        facet_delta=channel_delta(expected,haze(faceted,distance)),
        variation_delta=channel_delta(expected,haze(regular,distance)),
        roughness=effective_roughness,variance_response_delta=channel_delta(expected,haze(base_rough,distance)),
        opaque_fog_delta=channel_delta(expected,haze(hdr,solid_distance)))


class OffshoreContract(SwellContract):
    def author(self):
        revision=super().author()
        lighting=environment(FOG)|dict(ambient=AMBIENT)
        if self.args.rough_reflection:
            lighting['sky'].update(zenith=AMBIENT,horizon=AMBIENT,ground=AMBIENT)
            lighting['ibl']=dict(enabled=True,diffuse_intensity=0.,specular_intensity=1.)
        p=math.radians(-18)*.5
        g=math.radians(CAMERAS['grazing'][2])*.5
        # +90 degree yaw rotates forward to -X, keeping the -18 degree pitch.
        t=math.sqrt(.5)
        rotation=[math.sin(p)*t,math.cos(p)*t,-math.sin(p)*t,math.cos(p)*t]
        revision=self.apply(revision,8100,
            dict(op='entity.create',id=OCEAN_VIEW,name='Ocean toward coast'),
            component(OCEAN_VIEW,'Transform',transform(OCEAN_EYE,rotation)),
            component(OCEAN_VIEW,'Camera',LENS),
            dict(op='entity.create',id=GRAZING_VIEW,name='Offshore grazing camera'),
            component(GRAZING_VIEW,'Transform',transform(CAMERAS['grazing'][1],
                [math.sin(g)*t,-math.cos(g)*t,math.sin(g)*t,math.cos(g)*t])),
            component(GRAZING_VIEW,'Camera',LENS),
            component(uid(4),'LightingEnvironment',lighting),
            dict(op='entity.create',id=OCEAN_EAR,name='Offshore listener'),
            component(OCEAN_EAR,'Transform',transform(OFFSHORE_EAR)),
            dict(op='entity.create',id=SHORE_EAR,name='Nearshore listener'),
            component(SHORE_EAR,'Transform',transform(NEARSHORE_EAR)))
        self.definition=dict(self.definition,revision=revision,optics=self.definition['optics']|dict(offshore=SEA))
        if self.args.rough_reflection:
            self.definition['optics']['reflection_roughness']=REFLECTION_ROUGHNESS
        # The leading positive band is subnormal in float presentation. It
        # must not suppress the two visible bands or invalidate the scene.
        self.definition['surface_swell']=[dict(SWELL[0],slope=1e-40),*SWELL]
        if self.args.swell_variation:
            p=math.radians(CAMERAS['patches'][2])*.5
            revision=self.apply(revision,8106,
                dict(op='entity.create',id=PATCH_VIEW,name='Across offshore swell patches'),
                component(PATCH_VIEW,'Transform',transform(CAMERAS['patches'][1],
                    [math.sin(p)*t,math.cos(p)*t,-math.sin(p)*t,math.cos(p)*t])),
                component(PATCH_VIEW,'Camera',LENS))
            self.definition.update(revision=revision,swell_variation=VARIATION)
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
        if self.args.rough_reflection:
            assert state['optics']['reflection_roughness']==REFLECTION_ROUGHNESS
        if self.args.swell_variation:
            assert state['swell_variation']==VARIATION
            for i in (0,5):
                x,_,z=SAMPLES[i]
                candidates=[]
                for vertices,heights,(gx,gz) in joined_triangles(tick,VARIATION):
                    coords=weights(vertices,x,z)
                    if min(coords)>=-1e-12:
                        candidates.append((sum(a*b for a,b in zip(coords,heights,strict=True)),
                                           (-gx/math.hypot(gx,1,gz),1/math.hypot(gx,1,gz),-gz/math.hypot(gx,1,gz))))
                column=row[i]['column']
                assert any(abs(height-column['surface_height_m'])<2e-11 and
                    math.dist(normal,column['surface_normal'])<2e-11 for height,normal in candidates),column
        listeners=[client.call('runtime.audio.inspect',args|dict(listener=listener))['listener_water']
                   for listener in (SHORE_EAR,OCEAN_EAR)]
        for i,result in enumerate(listeners):
            near(result['submersion_m'],row[i]['submersion_m'])
            assert result['target_blend']==1
        return dict(state=state,listeners=listeners)

    def image(self,client,session,tick,label,camera):
        path=self.root/(label+'.bmp')
        pixels=GRAZING_PROBES if camera=='grazing' else PROBES
        result=client.call('runtime.capture',dict(session_id=session,tick=tick,camera=CAMERAS[camera][0],
            path=self.native(path),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=1,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            scene_product_probes=[dict(x=x,y=y) for x,y in pixels]))
        assert result['hardware'] and result['capture_written'] and result['nvrhi_errors']==0 and result['frames_presented']==2
        diag=result['render_diagnostics']
        assert diag['water_optics']['offshore']
        assert diag['water_optics']['offshore_swell_layers']==len(SWELL) and diag['height_fog']['active']
        profile=VARIATION if self.args.swell_variation else None
        roughness=REFLECTION_ROUGHNESS if self.args.rough_reflection else None
        assert diag['water_optics'].get('swell_variation')==profile
        if roughness is not None:
            assert diag['water_optics']['rough_reflection']==dict(active=True,perceptual_roughness=roughness)
            assert diag['environment_ibl']['enabled']
        rows=[compose(pixel,probe,tick,camera,profile,roughness)
              for pixel,probe in zip(pixels,diag['scene_products']['probes'],strict=True)]
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
        if self.args.swell_variation:
            description=self.world_call('world.describe',view='method',name='world.water.plan')
            assert '"swell_variation"' in json.dumps(description)
            self.cases(*[(label,rpc('world.water.plan',**(self.definition|{key:value})),-32602)
                for label,key,value in (
                    ('missing_swell','surface_swell',[]),
                    ('zero_variation','swell_variation',VARIATION|dict(strength=0)),
                    ('sampled_too_coarsely','swell_variation',VARIATION|dict(scale_m=8)),
                    ('invalid_seed','swell_variation',VARIATION|dict(seed=2**32)),
                    ('bool_scale','swell_variation',VARIATION|dict(scale_m=True)),
                )])
        if self.args.rough_reflection:
            self.cases(*[(label,rpc('world.water.plan',**(self.definition|dict(
                optics=amended|dict(reflection_roughness=value)))),-32602)
                for label,value in (('roughness_high',1.001),('roughness_low',-.001),('roughness_boolean',True))])
        plan=self.world_call('world.water.plan',**self.definition)
        if self.args.rough_reflection:
            assert plan['optics']['reflection_roughness']==REFLECTION_ROUGHNESS
        if self.args.swell_variation:
            assert plan['swell_variation']==VARIATION
            plain={k:v for k,v in self.definition.items() if k!='swell_variation'}
            assert self.world_call('world.water.plan',**plain)['plan_sha256']!=plan['plan_sha256']
        pin=plan['plan_sha256']
        assert self.world_call('world.water.cook',**self.definition,expected_plan_sha256=pin)==plan
        source=self.assets/(pin+'.pwater')
        artifact_version=14 if self.args.rough_reflection else 13 if self.args.swell_variation else 12
        assert file_hash(source)==pin and json.loads(source.read_bytes())['version']==artifact_version
        revision=self.apply(revision,8110,dict(op='water.set',asset=pin))
        dependencies=self.world_call('world.dependencies')
        assert dependencies['needs_water_offshore']
        assert dependencies.get('needs_water_swell_variation',False) is self.args.swell_variation
        assert dependencies.get('needs_water_rough_reflection',False) is self.args.rough_reflection
        (self.root/'saves').mkdir()
        session=uid(8111)
        with self.owner('water seam') as (client,_):
            client.call('runtime.start',self.start(session,revision)['params'])
            initial=self.verify_seam(client,session,0)
            client.call('runtime.step',self.step(session,0,45,8112)['params'])
            at_tick=self.verify_seam(client,session,45)
            assert initial['state']['points'][5]['column']['surface_height_m']!=at_tick['state']['points'][5]['column']['surface_height_m']
            if self.args.gpu is not None:
                self.image(client,session,45,'from-shore','shore')
                self.image(client,session,45,'from-ocean','ocean')
                self.image(client,session,45,'toward-horizon','grazing')
                hits=[r for name in ('from-shore','from-ocean') for r in self.measurements[name]['samples']]
                assert sum(r['coastal_m']>.01 and r['offshore_m']>.01 for r in hits)>=8
                assert any(r['interface']=='ocean' for r in hits)
                assert all(any(r['interface'] and r['solid_hit'] is solid and r['opaque_fog_delta']>.004
                              for r in hits) for solid in (True,False))
                grazing=self.measurements['toward-horizon']['samples']
                if self.args.rough_reflection:
                    assert any(r['interface']=='ocean' and r['roughness']>REFLECTION_ROUGHNESS+.06 for r in grazing)
                    assert sum(r['variance_response_delta']>.004 for r in grazing)>=4
                    assert any(r['interface']=='shore' and r['roughness']==REFLECTION_ROUGHNESS for r in hits)
                else:
                    assert all(any(r['interface']==side and r['facet_delta']>.006 for r in hits)
                               for side in ('shore','ocean'))
                    assert sum(r['interface']=='ocean' and r['filter_delta']>.006 for r in grazing)>=4
                if self.args.swell_variation:
                    self.image(client,session,45,'across-patches','patches')
                    across=self.measurements['across-patches']['samples']
                    if not self.args.rough_reflection:
                        assert sum(r['interface']=='ocean' and r['variation_delta']>.015 for r in across)>=8
                        assert any(r['interface']=='shore' and r['variation_delta']>.015 for r in hits)
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
            self.image(client,session,45,'cold-ocean','ocean')
            assert self.measurements['cold-ocean']['sha256']==self.measurements['from-ocean']['sha256']

    def relocated(self,revision,pin,expected):
        first=self.root/'initial export'
        older=self.root/'runtime missing offshore'
        older.mkdir()
        manifest=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        feature='water_swell_variation' if self.args.swell_variation else 'water_offshore'
        label='Spatially varied swell' if self.args.swell_variation else 'Offshore water'
        if self.args.rough_reflection:
            feature,label='water_rough_reflection','Rough water reflection'
        assert manifest['features'].pop(feature)
        (older/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        args=['project','build',self.native(self.manifest),'--output',self.native(first),'--runtime']
        rej=subprocess.run([str(self.args.binary.resolve()),*args,self.native(older)],capture_output=True,text=True,timeout=60)
        assert rej.returncode==4 and not first.exists(),rej
        assert json.loads(rej.stdout)['diagnostics'][0]['message']==f'{label} requires a compatible Vulkan runtime.'
        success(json.loads(self.cli(*args,self.native(self.args.runtime_root))))
        target=self.root/'relocated export'
        first.rename(target)
        parked=self.root/'parked project'
        self.project.rename(parked)
        try:
            game=target/'game.json'
            binary=target/json.loads(game.read_bytes())['engine']['executable']
            inspected=success(json.loads(self.cli('game','inspect',self.native(game),executable=binary)))
            assert inspected['needs_water_offshore'] and inspected['needs_'+feature]
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
    parser.add_argument('--swell-variation',action='store_true')
    parser.add_argument('--rough-reflection',action='store_true')
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
