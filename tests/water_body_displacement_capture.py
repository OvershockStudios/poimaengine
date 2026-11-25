#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Move water by adding and removing a buoyant box, then replay the export."""

import json
import math

from particle_collision_capture import main as run_fixture
from retained_capture import CAMERA, Fixture, component, expect_error, new_id, transform, tree_hashes, uid

TERRAIN, TEMPLATE = uid(980), uid(981)
GATE = 'use.sluice'
PROFILE = dict(max_rise_m=.1)
FACE = dict(cell=[0,0],axis='x')
EYE = [.5,.51,-5]


def near(measured, expected, tolerance=2e-10):
    assert math.isclose(measured,expected,rel_tol=0,abs_tol=tolerance),(measured,expected)


class DisplacementFixture(Fixture):
    def transact(self, client, revision, *ops):
        result = client.call('world.transact',dict(request_id=new_id(),base_revision=revision,ops=ops))
        assert result['committed']
        return result['revision']

    def author(self, client):
        revision = client.call('world.inspect')['revision']
        raw = json.dumps(dict(format='poima.terrain.heightfield',version=1,samples_x=3,samples_z=3,tile_cells=2,
            origin_m=[-2,0,-7],spacing_m=[2,2],heights_m=[0]*9))
        terrain = client.call('world.terrain.inspect',dict(recipe=raw))
        assert client.call('world.terrain.cook',dict(recipe=raw,expected_recipe_sha256=terrain['recipe_sha256']))==terrain
        tile, = terrain['tiles']
        mesh = dict(asset=tile['asset'],primitive=tile['primitive'])
        recipe = dict(op='template.set',id=TEMPLATE,name='Buoyant block',components=dict(
            Transform=transform([-.5,.35,-5]) | dict(scale=[.5]*3),
            BoxCollider=dict(motion='dynamic',half_extents=[.5]*3,mass=100,friction=.4,restitution=0,buoyancy={}),
            MeshRenderer=dict(primitive='box',albedo=[.86,.15,.015],visible=True)))
        revision = self.transact(client,revision,
            dict(op='component.remove',id=uid(1),type='BoxCollider'),
            dict(op='component.remove',id=uid(1),type='MeshRenderer'),
            dict(op='component.remove',id=uid(5),type='Light'),
            component(uid(4),'LightingEnvironment',dict(ambient=[.7]*3,exposure=1)),
            dict(op='physics_checkpoint.set',definition=dict(version=1)),
            dict(op='entity.create',id=TERRAIN,name='Canal floor'),component(TERRAIN,'Transform',transform([0,0,0])),
            component(TERRAIN,'StaticMesh',mesh | dict(visible=True)),
            component(TERRAIN,'MeshCollider',mesh | dict(friction=.7,restitution=0)),
            dict(op='entity.create',id=CAMERA,name='Downstream waterline'),
            component(CAMERA,'Transform',transform(EYE)),
            component(CAMERA,'Camera',dict(vertical_fov=50,near=.03,far=50)),recipe)
        params = dict(revision=revision,terrain=dict(recipe=raw),expected_terrain_recipe_sha256=terrain['recipe_sha256'],
            tile_entities=[TERRAIN],grid=dict(cells_x=2,cells_z=1,cell_size_m=1,origin_x_m=-1,origin_z_m=-5.5,
            relaxation_per_second=12),initial_depths_m=[.5,.5],faces=[FACE | dict(openness=0,action=GATE)],
            currents=dict(max_speed_m_s=4,min_depth_m=.02),
            optics=dict(follow_terrain=True,refraction=True,extinction_per_m=[.12,.4,.75],scattered_radiance=[.05,.1,.15]))
        reference = client.call('world.water.plan',params)
        for value in (True,{},dict(max_rise_m=.001),dict(max_rise_m=3)):
            expect_error(-32602,'world.water.plan',client,params | dict(body_displacement=value))
        args = params | dict(body_displacement=PROFILE)
        plan = client.call('world.water.plan',args)
        assert plan['plan_sha256']!=reference['plan_sha256'] and plan['body_displacement']==PROFILE
        assert client.call('world.water.cook',args | dict(expected_plan_sha256=plan['plan_sha256']))==plan
        self.data['plan_sha256'] = plan['plan_sha256']
        revision = self.transact(client,revision,dict(op='water.set',asset=plan['plan_sha256']))
        assert client.call('world.dependencies')['needs_water_body_displacement']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_water_body_displacement']
        return revision

    def read(self, client, session, tick):
        result = client.call('runtime.water.inspect',dict(session_id=session,tick=tick,cells=[[0,0],[1,0]],
            current_faces=[FACE],points=[EYE],segments=[dict(from_world=[.5,1,-5],to_world=[.5,.1,-5])]))
        result.pop('session_id')
        assert result['body_displacement']==PROFILE and result['volume_m3']==1
        assert result['last_step'] is None or result['last_step']['volume_after_m3']==1
        return result

    def step(self, client, session, tick, structure, toggle=False):
        params = dict(session_id=session,request_id=new_id(),expected_tick=tick,expected_structure_revision=structure,ticks=1)
        if toggle:
            params['inputs'] = [dict(entity=uid(2),actions=[dict(name=GATE,held=True,pressed=True,released=False)])]
        assert client.call('runtime.step',params)['current_tick']==tick+1

    def check(self, result, expected_left, expected_right, rise, discharge):
        left,right = result['cells']
        near(left['depth_m'],expected_left)
        near(right['depth_m'],expected_right)
        near(left['body_occupied_depth_m'],rise)
        near(left['body_surface_rise_m'],rise)
        assert right['body_occupied_depth_m']==right['body_surface_rise_m']==0
        near(result['current_faces'][0]['discharge_m3_s'],discharge)
        near(result['segments'][0]['position_m'][1],expected_right)
        near(result['points'][0]['submersion_m'],max(0,expected_right-EYE[1]))

    def picture(self, client, session, tick, label, depth):
        if self.args.gpu is None:
            return
        capture = self.capture(client,session,tick,label,width=240,height=160,reuse=True)
        diagnostics = capture['report']['render_diagnostics']['water_optics']
        assert diagnostics['active'] and diagnostics['refraction']
        near(diagnostics['camera_submersion_m'],depth,2e-6)
        self.data['pixel_counts'][label] = dict(image=capture['image'],submersion_m=diagnostics['camera_submersion_m'])

    def continue_after_remove(self, client, session, label, body):
        at_save = self.read(client,session,2)
        self.check(at_save,.48,.52,.1,1.2)
        self.picture(client,session,2,label+'-flooded',.01)
        gone = client.call('runtime.structure.transact',dict(session_id=session,request_id=new_id(),
            expected_tick=2,expected_structure_revision=1,despawns=[body]))
        assert gone['structure_revision']==2
        self.step(client,session,2,2)
        reversed_flow = self.read(client,session,3)
        self.check(reversed_flow,.488,.512,0,-.48)
        self.data['continuations'][label] = [at_save,reversed_flow]
        return self.data['continuations'][label]

    def begin(self, client, session, label):
        before = self.read(client,session,0)
        self.check(before,.5,.5,0,0)
        self.picture(client,session,0,label+'-dry',0)
        release = client.call('runtime.structure.transact',dict(session_id=session,request_id=new_id(),expected_tick=0,
            expected_structure_revision=0,spawns=[dict(template_id=TEMPLATE)]))
        body, = release['spawned']
        self.step(client,session,0,1)
        blocked = self.read(client,session,1)
        self.check(blocked,.5,.5,.1,0)
        self.step(client,session,1,1,toggle=True)
        saved = client.call('save.write',dict(request_id=new_id(),configuration_generation=1,
            expected_generation=0,slot='pressure',session_id=session,expected_tick=2,
            expected_gameplay_revision=0,expected_structure_revision=1))
        self.data['initial'].append([before,blocked])
        self.data['save_sha256'][label] = saved['sha256']
        return body,self.continue_after_remove(client,session,label,body)

    def restore(self, client, old, tick, structure, body, label):
        session = new_id()
        reply = client.call('save.load',dict(request_id=new_id(),configuration_generation=1,slot='pressure',
            expected_generation=1,revision=self.revision,expected_session_id=old,expected_tick=tick,
            expected_gameplay_revision=0,expected_structure_revision=structure,new_session_id=session))
        assert reply['tick']==2 and not reply['source_stale']
        return self.continue_after_remove(client,session,label,body)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Displacement Sluice')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author(client)
            client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
            session = self.mount(client)
            body,expected = self.begin(client,session,'source')
            assert self.restore(client,session,3,2,body,'restored')==expected
        if self.args.runtime_root:
            stage,bundle,hidden = (self.root/name for name in ('Built','Relocated','Hidden source'))
            self.cli('project','build',self.native(self.manifest),'--output',self.native(stage),
                '--runtime',self.native(self.args.runtime_root))
            stage.rename(bundle)
            self.project.rename(hidden)
            game = bundle/'game.json'
            assert self.cli('game','inspect',self.native(game))['needs_water_body_displacement']
            if self.args.windows_interop:
                exe = bundle/json.loads(game.read_bytes())['engine']['executable']
                exe.chmod(exe.stat().st_mode | 0o100)
            before = tree_hashes(bundle)
            saves = self.root/'Export saves'
            saves.mkdir()
            with self.owner('relocated',game=game,saves=saves) as client:
                state = client.call('runtime.status')
                exported,actual = self.begin(client,state['session_id'],'relocated')
                assert actual==expected
            with self.owner('fresh-save',game=game,saves=saves) as client:
                state = client.call('runtime.status')
                assert self.restore(client,state['session_id'],0,0,exported,'fresh-save')==expected
            assert tree_hashes(bundle)==before and not self.project.exists()
            self.data['immutable_files'] = len(before)
        if self.args.gpu is not None:
            captures = self.data['pixel_counts']
            assert captures['source-dry']['image']!=captures['source-flooded']['image']
            assert len({record['image'] for name,record in captures.items() if name.endswith('-flooded')})==1


if __name__=='__main__':
    run_fixture(DisplacementFixture)
