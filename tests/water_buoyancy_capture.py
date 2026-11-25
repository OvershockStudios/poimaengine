#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify flotation, sinking, shoreline torque and a relocated physics checkpoint."""

import json
import math

from particle_collision_capture import main as run_fixture
from retained_capture import CAMERA, Fixture, component, expect_error, new_id, transform, tree_hashes, uid
from scene_capture import pixels

FLOAT, STONE, EDGE, DRY, CONTROL, TERRAIN, RECIPE = map(uid, range(960,967))
BODIES = [FLOAT,STONE,EDGE,DRY]
MASS = .75**3*250
VOLUME = .75**3
CHECKPOINT, FINAL = 28, 88


def box(mass):
    return dict(motion='dynamic',mass=mass,friction=.6,restitution=0,half_extents=[.5]*3)


def near(actual, expected, tolerance=1e-7):
    assert math.isclose(actual,expected,rel_tol=0,abs_tol=tolerance), (actual,expected)


class BuoyancyFixture(Fixture):
    def transact(self, client, revision, ops, expected=None):
        params = dict(base_revision=revision,request_id=new_id(),ops=ops)
        if expected is not None:
            return expect_error(expected,'world.transact',client,params)
        result = client.call('world.transact',params)
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
        ops = [
            dict(op='component.remove',id=uid(1),type='BoxCollider'),
            dict(op='component.remove',id=uid(1),type='MeshRenderer'),
            dict(op='component.remove',id=uid(5),type='Light'),
            component(uid(4),'LightingEnvironment',dict(ambient=[.7]*3,exposure=1)),
            dict(op='physics_checkpoint.set',definition=dict(version=1)),
        ]
        cases = (
            (TERRAIN,'Terrain bed',[0,0,0],dict(StaticMesh=mesh | dict(visible=True),
                MeshCollider=mesh | dict(friction=.7,restitution=0))),
            (CAMERA,'Waterline',[0,1.8,0],dict(Camera=dict(vertical_fov=50,near=.1,far=60))),
            (FLOAT,'Floating cedar',[-1,1,-5],dict(BoxCollider=box(MASS) | dict(buoyancy={}))),
            (STONE,'Sinking stone',[1,1,-5],dict(BoxCollider=box(VOLUME*2000) | dict(buoyancy={}))),
            (EDGE,'Shoreline torque',[2,1,-7.25],dict(BoxCollider=box(MASS) | dict(buoyancy={}))),
            (DRY,'Dry cedar',[3,1,-6],dict(BoxCollider=box(MASS) | dict(buoyancy={}))),
            (CONTROL,'No fluid response',[-1.35,1,-7.25],dict(BoxCollider=box(MASS))),
        )
        colors = {FLOAT:[.94,.01,.005], STONE:[.005,.01,.94], EDGE:[.02,.75,.05],
                  DRY:[.83,.6,.01], CONTROL:[.7,.03,.72]}
        for entity,name,position,components in cases:
            pose = transform(position)
            ops.append(dict(op='entity.create',id=entity,name=name))
            if entity == CAMERA:
                pitch = math.radians(-4)
                pose['rotation'] = [math.sin(pitch),0,0,math.cos(pitch)]
            if entity in colors:
                pose['scale'] = [.75]*3
                ops.append(component(entity,'MeshRenderer',dict(primitive='box',albedo=colors[entity],visible=True)))
            ops.append(component(entity,'Transform',pose))
            ops.extend(component(entity,kind,value) for kind,value in components.items())
        ops.extend([dict(op='template.set',id=RECIPE,name='Buoyant test box',
            components=dict(Transform=transform([.25,1,-6.15]),BoxCollider=box(250) | dict(buoyancy={})))])
        revision = self.transact(client,revision,ops)
        for override in (dict(motion='static'),dict(mass=.0005),dict(buoyancy=True),
                         dict(buoyancy=dict(linear_drag=2.1)),dict(buoyancy=dict(angularDrag=.2))):
            self.transact(client,revision,[component(FLOAT,'BoxCollider',box(MASS) | dict(buoyancy={}) | override)],-32602)
            assert client.call('world.inspect')['revision']==revision

        args = dict(revision=revision,terrain=dict(recipe=raw),expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            tile_entities=[TERRAIN],grid=dict(cells_x=2,cells_z=2,cell_size_m=2,origin_x_m=-2,origin_z_m=-8,
            relaxation_per_second=0),initial_depths_m=[2]*4)
        plan = client.call('world.water.plan',args)
        assert client.call('world.water.cook',args | dict(expected_plan_sha256=plan['plan_sha256']))==plan
        revision = self.transact(client,revision,[dict(op='water.set',asset=plan['plan_sha256'])])
        assert client.call('world.dependencies')['needs_water_buoyancy']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_water_buoyancy']
        return revision

    def observe(self, client, session, tick):
        guard = dict(session_id=session,tick=tick)
        water = client.call('runtime.water.inspect',guard | dict(bodies=BODIES))
        assert [row['entity'] for row in water['bodies']]==BODIES and water['volume_m3']==32
        rows = {row['entity']:row for row in water['bodies']}
        motion = {}
        for entity in [*BODIES,CONTROL]:
            live = client.call('runtime.entity',guard | dict(id=entity))
            motion[entity] = [live['world_matrix'],live['velocity']]
        for body in BODIES:
            near(rows[body]['body_volume_m3'],VOLUME)
        assert rows[DRY]['center_of_buoyancy'] is None and rows[DRY]['submerged_volume_m3']==rows[DRY]['lift_n']==0
        return dict(water=rows,motion=motion)

    def step(self, client, session, tick, count=1):
        result = client.call('runtime.step',dict(session_id=session,expected_tick=tick,request_id=new_id(),ticks=count))
        assert result['current_tick']==tick+count

    def picture(self, client, session, tick, label):
        if self.args.gpu is None:
            return
        captured = self.capture(client,session,tick,label,width=240,height=160,reuse=True)
        image = pixels(self.root/(label+'.bmp'))
        red = [y for y,scan in enumerate(image) for r,g,b in scan if r>105 and r>3*max(g,b)]
        blue = [y for y,scan in enumerate(image) for r,g,b in scan if b>105 and b>3*max(r,g)]
        assert min(len(red),len(blue))>20,(len(red),len(blue),label)
        self.data['pixel_counts'][label] = dict(red=len(red),blue=len(blue),
            red_center_y=sum(red)/len(red),blue_center_y=sum(blue)/len(blue),image=captured['image'])

    def begin(self, client, session, label):
        initial = self.observe(client,session,0)
        for body,fraction in ((FLOAT,.5),(STONE,.5),(EDGE,.25)):
            near(initial['water'][body]['submerged_volume_m3'],VOLUME*fraction)
        assert initial['water'][EDGE]['center_of_buoyancy']==[2-.1875,1-.1875,-7.25]
        expect_error(-32602,'runtime.water.inspect',client,dict(session_id=session,tick=0,bodies=[CONTROL]))
        self.picture(client,session,0,label+'-entry')
        self.step(client,session,0)
        first = self.observe(client,session,1)
        for body,mass in ((FLOAT,MASS),(STONE,VOLUME*2000),(EDGE,MASS),(DRY,MASS)):
            acceleration = initial['water'][body]['lift_n']/mass-9.81
            near(first['motion'][body][1][1], acceleration/60, .002)
        assert first['motion'][EDGE][0][1]<-1e-4 and first['motion'][FLOAT][0][1]==0
        assert first['motion'][DRY][1]==first['motion'][CONTROL][1]
        self.data['initial'].append(dict(initial=initial,first=first))
        self.step(client,session,1,CHECKPOINT-1)
        saved = self.save(client,session,CHECKPOINT,'floating')
        self.data['save_sha256'][label] = saved['sha256']
        return self.continue_motion(client,session,label)

    def continue_motion(self, client, session, label):
        before = self.observe(client,session,CHECKPOINT)
        self.step(client,session,CHECKPOINT,FINAL-CHECKPOINT)
        final = self.observe(client,session,FINAL)
        assert final['motion'][FLOAT][0][13]>1.05
        assert final['motion'][STONE][0][13]<-.4 and final['motion'][DRY][0][13]<-.4
        self.picture(client,session,FINAL,label+'-rest')
        result = [before,final]
        self.data['continuations'][label] = result
        return result

    def load(self, client, previous, tick, label):
        session = new_id()
        result = client.call('save.load',dict(request_id=new_id(),configuration_generation=1,slot='floating',
            expected_generation=1,revision=self.revision,expected_session_id=previous,expected_tick=tick,
            expected_gameplay_revision=0,expected_structure_revision=0,new_session_id=session))
        assert result['tick']==CHECKPOINT and not result['source_stale']
        return session,self.continue_motion(client,session,label)

    def check_template(self, client, session):
        command = dict(session_id=session,request_id=new_id(),expected_tick=FINAL,expected_structure_revision=0,
                       spawns=[dict(template_id=RECIPE)])
        result = client.call('runtime.structure.transact',command)
        spawned, = result['spawned']
        query = dict(session_id=session,tick=FINAL,bodies=[spawned])
        expect_error(-32009,'runtime.water.inspect',client,query)
        measured, = client.call('runtime.water.inspect',query | dict(structure_revision=1))['bodies']
        near(measured['submerged_volume_m3'],.5)
        self.data['spawned_template'] = measured

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Buoyancy Yard')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            session = self.mount(client)
            initial = self.begin(client,session,'source')
            restored,actual = self.load(client,session,FINAL,'restored')
            assert initial==actual
            self.check_template(client,restored)
        if self.args.runtime_root:
            stage,bundle,hidden = (self.root/name for name in ('Built','Relocated','Hidden source'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),
                '--runtime',self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(hidden)
            manifest = bundle/'game.json'
            dependency = self.cli('game','inspect',self.native(manifest))
            assert dependency['needs_water_buoyancy']
            if self.args.windows_interop:
                binary = bundle/json.loads(manifest.read_bytes())['engine']['executable']
                binary.chmod(binary.stat().st_mode | 0o100)
            before = tree_hashes(bundle)
            save_root = self.root/'Export saves'
            save_root.mkdir()
            with self.owner('relocated',game=manifest,saves=save_root) as client:
                state = client.call('runtime.status')
                assert state['tick']==0 and self.begin(client,state['session_id'],'relocated')==initial
            with self.owner('fresh-save',game=manifest,saves=save_root) as client:
                state = client.call('runtime.status')
                assert state['tick']==0 and self.load(client,state['session_id'],0,'fresh-save')[1]==initial
            assert tree_hashes(bundle)==before and not self.project.exists()
            self.data['immutable_files'] = len(before)
        if self.args.gpu is not None:
            for stage_name in ('entry','rest'):
                assert len({row['image'] for name,row in self.data['pixel_counts'].items() if
                    name.endswith('-'+stage_name)})==1
            before = self.data['pixel_counts']['source-entry']
            after = self.data['pixel_counts']['source-rest']
            assert before['red_center_y']-after['red_center_y']>6
            assert after['blue_center_y']-before['blue_center_y']>30


if __name__=='__main__':
    run_fixture(BuoyancyFixture)
