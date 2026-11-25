#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise force-driven wakes in the shared water surface, optics and saves."""

from particle_collision_capture import main as run_fixture
from retained_capture import component, transform, uid
from water_currents_capture import CHECKPOINT, FINAL, CurrentFixture, near

WAKE = dict(speed_m_s=5,damping_per_second=4,max_amplitude_m=.1,body_gain=1)
OPTICS = dict(follow_terrain=True,refraction=True,extinction_per_m=[.12,.3,.65],scattered_radiance=[.04,.07,.1])
PROBE = uid(973)
CENTERS = [[x-1.5,0,z-6.5] for z in range(2) for x in range(4)]
PIXELS = [dict(x=x,y=y) for y in (48,75,100,133) for x in (41,95,145,198)]


class OpticalCanalFixture(CurrentFixture):
    def surface_options(self):
        return dict(optics=OPTICS)

    def author(self, client):
        revision = super().author(client)
        # Look just above the undisplaced cell stage at the first crest.
        baseline = self.expected[2]['depth'][0]-1
        return self.transact(client,revision,dict(op='entity.create',id=PROBE,name='Crest optical probe'),
            component(PROBE,'Transform',transform([-1.5,baseline+.001,-6.5])),
            component(PROBE,'Camera',dict(vertical_fov=52,near=.03,far=40)))

    def capture_probe(self, client, session, tick, label):
        captured = self.capture(client,session,tick,label,width=240,height=160,camera=PROBE,
            reuse=True,profile=True,scene_product_probes=PIXELS)
        diag = captured['report']['render_diagnostics']
        assert diag['water_optics']['active'] and diag['water_optics']['refraction']
        probes = diag['scene_products']['probes']
        assert len(probes)==len(PIXELS)
        return dict(optics=diag['water_optics'],products=probes,image=captured['image'])


class WakeFixture(OpticalCanalFixture):
    def surface_options(self):
        return super().surface_options() | dict(surface_wakes=WAKE)

    def author(self, client):
        revision = super().author(client)
        assert client.call('world.dependencies')['needs_water_surface_wakes']
        assert self.cli('project','inspect',self.native(self.manifest))['needs_water_surface_wakes']
        return revision

    def observe(self, client, session, tick):
        base = super().observe(client,session,tick)
        guard = dict(session_id=session,tick=tick)
        receipt = client.call('runtime.water.inspect',guard | dict(
            cells=[[x,z] for z in range(2) for x in range(4)],points=CENTERS,
            segments=[dict(from_world=[x,4,z],to_world=[x,-.75,z]) for x,_,z in CENTERS]))
        assert receipt['surface_wakes']==WAKE
        heights = [cell['wake_height_m'] for cell in receipt['cells']]
        steps = [cell['wake_vertical_step_m'] for cell in receipt['cells']]
        if tick<=1:
            assert heights==steps==[0]*8
        assert max(map(abs,heights))<=WAKE['max_amplitude_m']+1e-12
        for cell,point,segment in zip(receipt['cells'],receipt['points'],receipt['segments'],strict=True):
            actual = cell['surface_height_m']+cell['wake_height_m']
            near(point['column']['surface_height_m'],actual)
            near(segment['position_m'][1],actual)
            assert segment['cell']==cell['cell']
            for axis,expected in zip(segment['normal'],point['column']['surface_normal'],strict=True):
                near(axis,expected)
        summary = dict(heights_m=heights,steps_m=steps,crossings=receipt['segments'])
        if tick==2:
            # The nominally dry camera must be submerged by the displacement.
            submersion = heights[0]-.001
            assert submersion>.0005 and heights[1]<0 and heights[2]==heights[3]==0
            near(sum(heights),0)
            records = self.data.setdefault('optical_probe',[])
            if self.args.gpu is not None:
                shot = self.capture_probe(client,session,tick,f'wake-probe-{len(records)}')
                assert shot['optics']['surface_wakes']
                near(shot['optics']['camera_submersion_m'],submersion,2e-6)
                records.append(dict(expected_m=submersion,computed_m=shot['optics']['camera_submersion_m'],
                    image=shot['image'],products=shot['products']))
        if tick in (CHECKPOINT,CHECKPOINT+1,FINAL):
            assert max(map(abs,heights))>.002
        return base | dict(surface=summary)

    def compare_optical_control(self):
        root = self.root/'No wake control'
        root.mkdir()
        control = OpticalCanalFixture(self.args,root,dict(owners=[],cli=[],captures={},plan_sha256=None))
        control.cli('project','create',control.native(control.project),'--name','Undisplaced canal')
        with control.owner('same tick without wakes') as client:
            control.revision = control.author(client)
            session = control.mount(client)
            control.step(client,session,0,toggle=True)
            control.step(client,session,1)
            shot = control.capture_probe(client,session,2,'dry')
        assert shot['optics']['camera_submersion_m']==0 and 'surface_wakes' not in shot['optics']
        wet = self.data['optical_probe'][0]
        errors = []
        for straight,displaced in zip(shot['products'],wet['products'],strict=True):
            assert straight['raw_hdr']==displaced['raw_hdr'] and straight['depth']==displaced['depth']
            errors.append(max(abs(x-y) for x,y in zip(straight['resolved_hdr'],displaced['resolved_hdr'],strict=True)))
        assert sum(error>1e-5 for error in errors)>=4, errors
        self.data['control_comparison'] = dict(dry=shot,changed_probes=sum(error>1e-5 for error in errors),
            max_hdr_difference=max(errors))

    def run(self):
        super().run()
        if self.args.gpu is not None:
            probes = self.data['optical_probe']
            if self.args.runtime_root:
                assert len(probes)==2 and probes[0]==probes[1]
            self.compare_optical_control()
        if self.args.runtime_root:
            game = self.root/'Relocated'/'game.json'
            assert self.cli('game','inspect',self.native(game))['needs_water_surface_wakes']


if __name__=='__main__':
    run_fixture(WakeFixture)
