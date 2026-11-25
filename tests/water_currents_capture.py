#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify conserved gate discharge, opposing drifts and cold continuation."""

import json
import math

from particle_collision_capture import main as run_fixture
from retained_capture import CAMERA, Fixture, component, expect_error, new_id, transform, tree_hashes, uid
from scene_capture import pixels

FORWARD, REVERSE, TERRAIN = map(uid, range(970,973))
ACTORS = [FORWARD,REVERSE]
GATE = 'use.sluice'
CHECKPOINT, FINAL = 28, 88
INITIAL = [2.75,2.25,1.75,1.25]
RATE = 4.5
CAP = 1.2
PROFILE = dict(max_speed_m_s=CAP,min_depth_m=.02)
POINTS = [[-.625,.85,-6.5],[.625,.85,-5.5],[-.625,3,-6.5]]
FACES = [dict(cell=[x,z],axis='x') for z in range(2) for x in range(3)]
FACES += [dict(cell=[x,0],axis='z') for x in range(4)]


def near(actual, expected, tolerance=1e-11):
    assert math.isclose(actual,expected,rel_tol=0,abs_tol=tolerance), (actual,expected)


def oracle():
    depth = INITIAL.copy()
    result = [dict(depth=depth,flow=[0]*3)]
    for tick in range(1,FINAL+2):
        test = depth.copy()
        test[0] += RATE/60
        test[3] -= RATE/60
        rate = [12*(test[i]-test[i+1]) for i in range(3)]
        rate[1] *= .75 if tick<=FINAL else 0
        depth = [d + ((rate[i-1] if i else 0)-(rate[i] if i<3 else 0))/60 for i,d in enumerate(test)]
        result.append(dict(depth=depth,flow=rate))
    return result


class CurrentFixture(Fixture):
    expected = oracle()

    def surface_options(self):
        return {}

    def transact(self, client, revision, *ops):
        result = client.call('world.transact',dict(request_id=new_id(),base_revision=revision,ops=ops))
        assert result['committed']
        return result['revision']

    def author(self, client):
        revision = client.call('world.inspect')['revision']
        raw = json.dumps(dict(format='poima.terrain.heightfield',version=1,samples_x=3,samples_z=3,tile_cells=2,
            origin_m=[-4,-1,-10],spacing_m=[4,4],heights_m=[0]*9))
        terrain = client.call('world.terrain.inspect',dict(recipe=raw))
        assert client.call('world.terrain.cook',dict(recipe=raw,expected_recipe_sha256=terrain['recipe_sha256']))==terrain
        tile, = terrain['tiles']
        mesh = dict(asset=tile['asset'],primitive=tile['primitive'])
        ops = [dict(op='component.remove',id=uid(1),type=kind) for kind in ('BoxCollider','MeshRenderer')]
        ops += [dict(op='component.remove',id=uid(5),type='Light'),
                component(uid(4),'LightingEnvironment',dict(ambient=[.7]*3,exposure=1)),
                dict(op='physics_checkpoint.set',definition=dict(version=1))]
        for entity,name,position in ((TERRAIN,'Canal bed',[0,0,0]),(CAMERA,'Canal overview',[0,1.8,0]),
            (FORWARD,'Red float',[-.55,1.375,-6.5]),(REVERSE,'Blue float',[.55,1.375,-5.5])):
            ops.append(dict(op='entity.create',id=entity,name=name))
            pose = transform(position)
            if entity in ACTORS:
                pose['scale'] = [.5]*3
            if entity==CAMERA:
                pitch = math.radians(-4)
                pose['rotation'] = [math.sin(pitch),0,0,math.cos(pitch)]
            ops.append(component(entity,'Transform',pose))
        ops += [component(TERRAIN,'StaticMesh',mesh | dict(visible=True)),
                component(TERRAIN,'MeshCollider',mesh | dict(friction=.7,restitution=0)),
                component(CAMERA,'Camera',dict(vertical_fov=50,near=.1,far=60))]
        for entity,color in ((FORWARD,[.94,.01,.005]),(REVERSE,[.005,.01,.94])):
            ops.extend([component(entity,'MeshRenderer',dict(primitive='box',albedo=color,visible=True)),
                        component(entity,'BoxCollider',dict(half_extents=[.5]*3,motion='dynamic',mass=31.25,
                            friction=.4,restitution=0,buoyancy=dict(linear_drag=1.6)))])
        revision = self.transact(client,revision,*ops)
        faces = [dict(face,openness=0) for face in FACES if face['axis']=='z']
        faces += [dict(cell=[1,z],axis='x',openness=0,action=GATE,action_openness=.75) for z in range(2)]
        exchanges = [dict(cell=[x,z],source_m3_per_second=s,drain_m3_per_second=d)
            for x,z,s,d in ((0,0,RATE,0),(3,0,0,RATE),(0,1,0,RATE),(3,1,RATE,0))]
        options = dict(revision=revision,terrain=dict(recipe=raw),expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            tile_entities=[TERRAIN],grid=dict(cells_x=4,cells_z=2,cell_size_m=1,origin_x_m=-2,origin_z_m=-7,
            relaxation_per_second=12),initial_depths_m=INITIAL+INITIAL[::-1],faces=faces,exchanges=exchanges) | self.surface_options()
        for invalid in ({},PROFILE | dict(max_speed_m_s=31),PROFILE | dict(min_depth_m=0),False):
            expect_error(-32602,'world.water.plan',client,options | dict(currents=invalid))
        ordinary = client.call('world.water.plan',options)
        options['currents'] = PROFILE
        plan = client.call('world.water.plan',options)
        assert plan['plan_sha256']!=ordinary['plan_sha256'] and plan['currents']==PROFILE
        assert client.call('world.water.cook',options | dict(expected_plan_sha256=plan['plan_sha256']))==plan
        self.data['plan_sha256'] = plan['plan_sha256']
        revision = self.transact(client,revision,dict(op='water.set',asset=plan['plan_sha256']))
        assert client.call('world.dependencies')['needs_water_currents']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_water_currents']
        return revision

    def observe(self, client, session, tick):
        guard = dict(session_id=session,tick=tick)
        state = client.call('runtime.water.inspect',guard | dict(current_faces=FACES,bodies=ACTORS,points=POINTS,
            cells=[[x,z] for z in range(2) for x in range(4)]))
        reference = self.expected[tick]
        near(state['volume_m3'],16)
        assert state['currents']==PROFILE
        for row,expected in zip(state['cells'],reference['depth']+reference['depth'][::-1],strict=True):
            near(row['depth_m'],expected)
        flux = reference['flow']
        for row,pin,expected in zip(state['current_faces'],FACES,flux+[-u for u in flux[::-1]]+[0]*4,strict=True):
            assert row['cell']==pin['cell'] and row['axis']==pin['axis']
            near(row['discharge_m3_s'],expected)
        # The wet point is 0.375 m into the second cell. Mirror X/Z for the second stream.
        point = (.625*flux[0]+.375*flux[1])/max(reference['depth'][1],PROFILE['min_depth_m'],
            max(abs(flux[0]),abs(flux[1]))/CAP)
        for row,expected in zip(state['points'],[point,-point,0],strict=True):
            actual = row['water_velocity_m_s']
            near(actual[0],expected)
            assert actual[1:]==[0,0]
        rows = {}
        for actor,row in zip(ACTORS,state['bodies'],strict=True):
            assert row['entity']==actor
            live = client.call('runtime.entity',guard | dict(id=actor))
            rows[actor] = dict(center=live['world_matrix'][12:15],velocity=live['velocity'],
                current=row['water_velocity_m_s'],displaced=row['submerged_volume_m3'])
        return dict(bodies=rows,discharge=[f['discharge_m3_s'] for f in state['current_faces']],points=state['points'])

    def step(self, client, session, start, count=1, toggle=False):
        params = dict(request_id=new_id(),session_id=session,expected_tick=start,ticks=count)
        if toggle:
            params['inputs'] = [dict(entity=uid(2),actions=[dict(name=GATE,held=True,pressed=True,released=False)])]
        stepped = client.call('runtime.step',params)
        assert stepped['current_tick']==start+count

    def picture(self, client, session, tick, label):
        if self.args.gpu is None:
            return
        captured = self.capture(client,session,tick,label,width=240,height=160,reuse=True)
        image = pixels(self.root/(label+'.bmp'))
        chosen = [[x for row in image for x,rgb in enumerate(row) if
                   rgb[i]>105 and rgb[i]>3*max(rgb[j] for j in range(3) if j!=i)] for i in (0,2)]
        assert min(map(len,chosen))>16
        self.data['pixel_counts'][label] = dict(sha256=captured['image'],centroids_x=[sum(x)/len(x) for x in chosen])

    def continue_motion(self, client, session, label):
        at_save = self.observe(client,session,CHECKPOINT)
        self.step(client,session,CHECKPOINT)
        resumed = self.observe(client,session,CHECKPOINT+1)
        self.step(client,session,CHECKPOINT+1,FINAL-CHECKPOINT-1)
        final = self.observe(client,session,FINAL)
        self.picture(client,session,FINAL,label+'-final')
        rows = [at_save,resumed,final]
        self.data['continuations'][label] = rows
        return rows

    def begin(self, client, session, label):
        born = self.observe(client,session,0)
        expect_error(-32602,'runtime.water.inspect',client,dict(session_id=session,tick=0,
            current_faces=[dict(cell=[3,0],axis='x')]))
        expect_error(-32009,'runtime.water.inspect',client,dict(session_id=session,tick=1,current_faces=FACES))
        self.picture(client,session,0,label+'-initial')
        self.step(client,session,0,toggle=True)
        first = self.observe(client,session,1)
        assert first['bodies'][FORWARD]['velocity'][0]==first['bodies'][REVERSE]['velocity'][0]==0
        self.step(client,session,1)
        second = self.observe(client,session,2)
        for actor,sign in ((FORWARD,1),(REVERSE,-1)):
            assert first['bodies'][actor]['current'][0]*sign>0
            assert second['bodies'][actor]['velocity'][0]*sign>.005
        self.data['initial'].append([born,first,second])
        self.step(client,session,2,CHECKPOINT-2)
        saved = self.save(client,session,CHECKPOINT,'flow')
        self.data['save_sha256'][label] = saved['sha256']
        return self.continue_motion(client,session,label)

    def load(self, client, session, tick, label):
        restored = new_id()
        result = client.call('save.load',dict(request_id=new_id(),configuration_generation=1,slot='flow',
            expected_generation=1,revision=self.revision,expected_session_id=session,expected_tick=tick,
            expected_gameplay_revision=0,expected_structure_revision=0,new_session_id=restored))
        assert result['tick']==CHECKPOINT and not result['source_stale']
        return restored,self.continue_motion(client,restored,label)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Counterflow Yard')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            session = self.mount(client)
            expected = self.begin(client,session,'source')
            restored,actual = self.load(client,session,FINAL,'restored')
            assert expected==actual
            self.step(client,restored,FINAL,toggle=True)
            closed = self.observe(client,restored,FINAL+1)
            assert closed['discharge'][1]==closed['discharge'][4]==0
            self.data['closed_flux'] = closed['discharge']
        if self.args.runtime_root:
            stage,bundle,hidden = (self.root/name for name in ('Built','Relocated','Hidden source'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),
                '--runtime',self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(hidden)
            manifest = bundle/'game.json'
            assert self.cli('game','inspect',self.native(manifest))['needs_water_currents']
            if self.args.windows_interop:
                binary = bundle/json.loads(manifest.read_bytes())['engine']['executable']
                binary.chmod(binary.stat().st_mode | 0o100)
            immutable = tree_hashes(bundle)
            saves = self.root/'Export saves'
            saves.mkdir()
            with self.owner('relocated',game=manifest,saves=saves) as client:
                state = client.call('runtime.status')
                assert state['tick']==0 and self.begin(client,state['session_id'],'relocated')==expected
            with self.owner('fresh-save',game=manifest,saves=saves) as client:
                state = client.call('runtime.status')
                assert state['tick']==0 and self.load(client,state['session_id'],0,'fresh-save')[1]==expected
            assert tree_hashes(bundle)==immutable and not self.project.exists()
            self.data['immutable_files'] = len(immutable)
        if self.args.gpu is not None:
            captures = self.data['pixel_counts']
            for phase in ('initial','final'):
                assert len({row['sha256'] for name,row in captures.items() if name.endswith('-'+phase)})==1
            prior = captures['source-initial']['centroids_x']
            after = captures['source-final']['centroids_x']
            assert after[0]-prior[0]>7 and prior[1]-after[1]>7,(prior,after)


if __name__=='__main__':
    run_fixture(CurrentFixture)
