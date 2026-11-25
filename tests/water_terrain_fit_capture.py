#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a terrain-fitted optical shore against an affine bed and a guarded gate."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import traceback

from height_fog_capture import environment
from water_listener_audio_contract import filter_error, pcm, write_tones
from water_optics_capture import AMBIENT, OPTICS, fresnel
from water_refraction_capture import LENS, direction
from world_particles_contract import ParticleContract, near_vector, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash
from world_water_contract import CELLS, TILES, WaterContract, bed, cell_center, component, near, rpc, success, uid


AIR, LISTENER, EMITTER = map(uid, (6920,6931,6932))
EYE, PITCH = (11.3,6.97,-2.02), -56
SURFACE = 4.93
ENTRY = (10.9,4.885,-3.5)
PROFILE = dict(cutoff_hz=900,gain=.6,full_depth_m=.09)
POINTS = [ENTRY, (10.9,4.885,-2.5), (10.55,4.885,-3.5), (10.9,4.80,-3.5),
          (11.1,4.885,-3.5), (14,4.885,-3.5)]
PROBES = [(x,y) for y in (48,82,117,154,193,234,280,315)
          for x in (45,133,211,238,254,273,315,442)]


def trace_affine_bed(pixel, depth, raw, opened):
    ray = direction(pixel,PITCH)
    cosine,sine = math.cos(math.radians(PITCH)),math.sin(math.radians(PITCH))
    view_z = LENS['near'] / (1-depth+depth*(LENS['near']/LENS['far']))
    distance = view_z / (ray[1]*sine-ray[2]*cosine)
    vertical_entry = (SURFACE-EYE[1])/ray[1]

    # Clip against the original affine plane and authored outer boundaries,
    # without the GPU shader's water-column walk or corner-based triangles.
    rectangles = [(11,14,-4,-2,False)]
    if opened:
        rectangles += [(10,11,-4,-3,True)]
    length = 0
    crosses_top = False
    for xmin,xmax,zmin,zmax,margin in rectangles:
        x,y,z=EYE
        dx,dy,dz=ray
        planes=[(x-xmin,dx),(xmax-x,-dx),(z-zmin,dz),(zmax-z,-dz),
                (y-bed(x,z),dy+.25*dx-.125*dz),(SURFACE-y,-dy)]
        if margin:
            # Triangular wedge faces the donor at x=11, z=-4..-3.
            planes += [(x+z-7,dx+dz),(x-z-14,dx-dz)]
        start,end = 0,distance
        for value,slope in planes:
            if slope>0:
                start=max(start,-value/slope)
            elif slope<0:
                end=min(end,-value/slope)
            elif value<0:
                end=-1
        if end>start:
            length+=end-start
            crosses_top |= math.isclose(start,vertical_entry,rel_tol=0,abs_tol=1e-11)

    reflected = fresnel(ray[1]) if crosses_top else 0
    filtered = [source+(value-source)*math.exp(-length*k)
                for value,source,k in zip(raw,OPTICS['scattered_radiance'],OPTICS['extinction_per_m'],strict=True)]
    x = EYE[0]+vertical_entry*ray[0]
    z = EYE[2]+vertical_entry*ray[2]
    return dict(length_m=length, interface=crosses_top, entry_xz=[x,z],
        hdr=[sample*(1-reflected)+sky*reflected for sample,sky in zip(filtered,AMBIENT,strict=True)])


class TerrainFitContract(WaterContract):
    owner = ParticleContract.owner

    def apply(self, revision, request, *ops):
        return success(self.world_calls(self.transaction(revision,request,*ops))[0])['revision']

    def author(self):
        recipe,_,revision = self.arm_terrain()
        sound = self.root/'tone.wav'
        write_tones(sound)
        clip = self.world_call('asset.audio.import',source=self.native(sound))['asset']
        half = math.radians(PITCH)/2
        revision = self.apply(revision,6950,
            dict(op='component.remove',id=uid(1),type='MeshRenderer'),
            dict(op='component.remove',id=uid(1),type='BoxCollider'),
            dict(op='component.remove',id=uid(5),type='Light'),
            component(uid(4),'LightingEnvironment',environment()|dict(ambient=AMBIENT)),
            dict(op='entity.create',id=AIR,name='Terrain fit camera'),
            component(AIR,'Transform',transform(EYE,[math.sin(half),0,0,math.cos(half)])),
            component(AIR,'Camera',LENS),
            dict(op='entity.create',id=LISTENER,name='Listener in the wet wedge'),
            component(LISTENER,'Transform',transform(ENTRY)),
            dict(op='entity.create',id=EMITTER,name='Test tone source'),
            component(EMITTER,'Transform',transform([11.7,5.35,-3.1])),
            component(EMITTER,'AudioEmitter',dict(asset=clip,gain=1,loop=True,enabled=True)))
        self.definition = dict(revision=revision,terrain=dict(source=recipe.name),tile_entities=TILES,
            expected_terrain_recipe_sha256=self.terrain['recipe_sha256'],
            grid=dict(cells_x=4,cells_z=2,cell_size_m=1,origin_x_m=10,origin_z_m=-4,relaxation_per_second=0),
            initial_depths_m=[max(0,SURFACE-bed(*cell_center(cell))) for cell in CELLS],
            faces=[dict(cell=[0,0],axis='x',openness=0,sill_height_m=4.85,action='sluice',action_openness=1),
                   dict(cell=[0,1],axis='x',openness=1,sill_height_m=5.1)],
            listener_audio=PROFILE)
        return revision

    def cook(self, enabled):
        optics = OPTICS | ({'follow_terrain':enabled} if enabled is not None else {})
        definition = self.definition | dict(optics=optics)
        plan = self.world_call('world.water.plan',**definition)
        pin = plan['plan_sha256']
        assert self.world_call('world.water.cook',**definition,expected_plan_sha256=pin)==plan
        asset = self.assets/(pin+'.pwater')
        recipe = json.loads(asset.read_bytes())
        assert recipe['version']==(10 if enabled else 6) and file_hash(asset)==pin
        if enabled:
            assert recipe['shore_corner_heights_m']==[bed(x,z) for z in range(-4,-1) for x in range(10,15)]
        else:
            assert 'shore_corner_heights_m' not in recipe and recipe['optics']==OPTICS
        return pin

    def observe(self, client, session, tick, fitted, opened):
        args = dict(session_id=session,tick=tick)
        state = client.call('runtime.water.inspect',args|dict(cells=[[0,0],[0,1]],points=POINTS))
        wanted = (.045 if fitted and opened else 0,0,0,0,.045,0)
        for row,expected in zip(state['points'],wanted,strict=True):
            near(row['submersion_m'],expected)
        if fitted:
            near(state['points'][0]['column']['floor_height_m'],bed(ENTRY[0],ENTRY[2]))
        assert not any(cell['depth_m'] or cell['volume_m3'] for cell in state['cells'])
        near(state['volume_m3'],sum(self.definition['initial_depths_m']))
        audio = client.call('runtime.audio.inspect',args|dict(listener=LISTENER))['listener_water']
        near(audio['submersion_m'],wanted[0])
        near(audio['target_blend'],.5 if fitted and opened else 0)
        return dict(water=state,audio=audio)

    def image(self, client, session, tick, label, fitted, *, samples=1, fsr=False):
        path = self.root/(label+'.bmp')
        response = client.call('runtime.capture',dict(session_id=session,tick=tick,camera=AIR,
            path=self.native(path),width=CAPTURE_SIZE[0],height=CAPTURE_SIZE[1],gpu=self.args.gpu,samples=samples,
            lighting_path=self.args.lighting_path,profile=True,capture_frames=2,
            reconstruction='fsr3_native' if fsr else 'none',
            scene_product_probes=[dict(x=x,y=y) for x,y in PROBES] if samples==1 else []))
        assert response['hardware'] and response['capture_written'] and response['frames_presented']==2
        assert response['nvrhi_errors']==0 and response['tick']==tick
        diag=response['render_diagnostics']
        assert diag['water_optics']['active'] is (samples==1)
        assert diag['water_optics'].get('follow_terrain',False) is fitted and diag['reconstruction']['active'] is fsr
        self.measurements[label]=dict(report=response,sha256=file_hash(path))
        return diag['scene_products']['probes']

    def mix(self, client, session, label):
        path = self.root/(label+'.wav')
        result = client.call('runtime.audio.capture',dict(session_id=session,tick=1,
            listener=LISTENER,path=self.native(path),frames=4800))
        assert result['capture']['sha256']==file_hash(path)
        data=pcm(path)
        assert max(abs(value) for value in data)>.01
        self.measurements[label]=dict(result=result,sha256=file_hash(path))
        return data

    def sample_run(self, revision, pin, label, fit, saves=None):
        revision = self.apply(revision,6960+revision,dict(op='water.set',asset=pin))
        session = uid(6970+revision)
        with self.owner(label) as (client,_):
            client.call('runtime.start',self.start(session,revision)['params'])
            before = self.observe(client,session,0,fit,False)
            closed = self.image(client,session,0,label+'-closed',fit)
            step = self.step(session,0,1,7000+revision,pressed=True)['params']
            client.call('runtime.step',step|dict(sounds=[dict(op='play',emitter=EMITTER)]))
            after = self.observe(client,session,1,fit,True)
            assert after['water']['faces'][0]['openness']==1
            opened = self.image(client,session,1,label+'-opened',fit)
            wave = self.mix(client,session,label+'-audio')
            if fit and self.args.fsr:
                self.image(client,session,1,'fsr-opened',fit,fsr=True)
            if fit and self.args.lighting_path=='forward':
                self.image(client,session,1,'msaa-opened',fit,samples=4)
            if saves:
                client.call('save.configure',dict(request_id=uid(7012),expected_generation=0,root=self.native(saves)))
                client.call('save.write',self.write(session,1,'checkpoint',7013)['params'])
        self.measurements[label+'-observations']=dict(before=before,after=after)
        return revision,closed,opened,wave

    def compare(self, closed, opened):
        results=[]
        for pixel, before, after in zip(PROBES,closed,opened,strict=True):
            assert before['depth']==after['depth'] and before['raw_hdr']==after['raw_hdr'],pixel
            pair=[]
            for visible, probe in ((False,before),(True,after)):
                expect = trace_affine_bed(pixel,probe['depth'],probe['raw_hdr'],visible)
                near_vector(probe['resolved_hdr'],expect['hdr'],tolerance=.003)
                pair.append(expect|dict(observed=probe['resolved_hdr']))
            gain = pair[1]['length_m']-pair[0]['length_m']
            results.append(dict(pixel=pixel,added_water_m=gain,closed=pair[0],opened=pair[1]))
        assert sum(row['added_water_m']>.02 for row in results)>=3,results
        assert any(row['opened']['interface'] and not row['closed']['interface'] for row in results)
        assert any(row['opened']['length_m']==0 for row in results)
        return results

    def relocated(self, revision, pin, saves):
        first,moved=self.root/'initial export',self.root/'moved export'
        no_feature=self.root/'runtime missing terrain fit'
        no_feature.mkdir()
        manifest=json.loads((self.args.runtime_root/'runtime.json').read_bytes())
        assert manifest['features'].pop('water_terrain_fit')
        (no_feature/'runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
        argv=['project','build',self.native(self.manifest),'--output',self.native(first),'--runtime']
        rejected=subprocess.run([str(self.args.binary.resolve()),*argv,self.native(no_feature)],
                                capture_output=True,text=True,timeout=60)
        assert rejected.returncode==4 and not first.exists(),rejected
        message=json.loads(rejected.stdout)['diagnostics'][0]['message']
        assert message=='Terrain-fit water requires a compatible Vulkan runtime.',message
        self.measurements['admission_error']=message
        success(json.loads(self.cli(*argv,self.native(self.args.runtime_root))))
        first.rename(moved)
        unavailable=self.root/'parked project'
        self.project.rename(unavailable)
        try:
            game=moved/'game.json'
            binary=moved/json.loads(game.read_bytes())['engine']['executable']
            assert success(json.loads(self.cli('game','inspect',self.native(game),executable=binary)))['needs_water_terrain_fit']
            asset,=moved.rglob('*.pwater')
            assert file_hash(asset)==pin
            with self.owner('relocated save',game=game,saves=saves) as (client,_):
                active=client.call('runtime.status')['session_id']
                session=uid(7020)
                client.call('save.load',self.load(session,revision,7021,active=active,active_tick=0)['params'])
                self.observe(client,session,1,True,True)
                self.image(client,session,1,'relocated',True)
                self.mix(client,session,'relocated-audio')
            for name,other in (('relocated','fitted-opened'),('relocated-audio','fitted-audio')):
                assert self.measurements[name]['sha256']==self.measurements[other]['sha256']
        finally:
            unavailable.rename(self.project)

    def run(self):
        revision=self.author()
        self.cases(('non_boolean',rpc('world.water.plan',**(self.definition|dict(optics=OPTICS|dict(follow_terrain=1)))),-32602),
            ('source_corner_data',rpc('world.water.plan',**(self.definition|dict(shore_corner_heights_m=[]))),-32602))
        baseline=self.cook(None)
        assert baseline==self.cook(False)
        pin=self.cook(True)
        saves=self.root/'saves'
        saves.mkdir()
        revision,base_closed,base_open,dry=self.sample_run(revision,baseline,'baseline',False)
        assert base_closed==base_open
        revision,closed,opened,wet=self.sample_run(revision,pin,'fitted',True,saves)
        assert self.world_call('world.dependencies')['needs_water_terrain_fit']
        self.measurements['samples']=self.compare(closed,opened)
        self.measurements['stereo_pcm_oracle_max_error']=filter_error(dry,wet,lambda _:.5,.5)
        if self.args.runtime_root:
            self.relocated(revision,pin,saves)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--fsr',action='store_true')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='water-terrain-fit-',dir=args.output))
    fixture=TerrainFitContract(args,root)
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
        print(f'Terrain-fit water evidence {root / "evidence.json"}')


if __name__=='__main__':
    main()
