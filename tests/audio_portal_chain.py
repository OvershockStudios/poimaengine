#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check four successive apertures in a switchback corridor and a relocated player."""

import math

from audio_portal import MATERIAL, PROFILE, SOURCE, PortalFixture, energy, main
from retained_capture import component, new_id, transform, uid
from water_listener_audio_contract import RATE

OPEN_Y, CLOSED_Y, PARTIAL_Y = 4.6, 2.6, 3.45
COORDINATES = ((-2.15,-6), (2.15,-4), (-2.15,-2), (2.15,0))
PORTALS = [uid(1000+6*i) for i in range(4)]
GATES = [uid(1001+6*i) for i in range(4)]


def pose(index, height):
    x,z = COORDINATES[index]
    return [x,height,z]


def move(index, height):
    return dict(ticks=12,motions=[dict(entity=GATES[index],position=pose(index,height),
                                      rotation=[0,0,0,1],duration_ticks=12)])


SEQUENCE = [dict(ticks=12,sounds=[dict(op='play',emitter=SOURCE)])]
for index,height in ((0,CLOSED_Y),(0,OPEN_Y),(3,CLOSED_Y),(3,OPEN_Y),(1,CLOSED_Y)):
    SEQUENCE.extend([move(index,height),dict(ticks=8)])


class CorridorFixture(PortalFixture):
    portal_count = 4
    project_name = 'Switchback acoustic corridor'
    sequence = SEQUENCE
    total_ticks = sum(part['ticks'] for part in SEQUENCE)

    def author(self, client):
        self.revision = client.call('world.inspect')['revision']
        self.author_source(client,[0,2.6,-8])

        def wall(entity, name, position, extents, mode, color):
            panel = uid(int(entity,16)+1)
            return [
                dict(op='entity.create',id=entity,name=name),
                component(entity,'Transform',transform(position)),
                component(entity,'BoxCollider',dict(half_extents=extents,motion=mode,mass=1,
                                                      friction=.5,restitution=0)),
                component(entity,'AcousticMaterial',MATERIAL),
                dict(op='entity.create',id=panel,parent=entity,name=name+' surface'),
                component(panel,'Transform',transform([0,0,0]) | dict(scale=[2*n for n in extents])),
                component(panel,'MeshRenderer',dict(primitive='box',visible=True,albedo=color)),
            ]

        for index,(x,z) in enumerate(COORDINATES):
            portal,gate = PORTALS[index],GATES[index]
            self.edit(client,
                dict(op='entity.create',id=portal,name=f'Aperture {index+1}'),
                component(portal,'Transform',transform(pose(index,CLOSED_Y))),
                component(portal,'AudioPortal',PROFILE),
                *wall(gate,f'Panel {index+1}',pose(index,OPEN_Y),[.6,.75,.08],'kinematic',[.53,.22,.07]))
            # Each wall is a finite pair of solids. The aperture stays in
            # their actual gap throughout the motion of its separate panel.
            for side,(lo,hi) in enumerate(((-8,x-.65),(x+.65,8))):
                slab = uid(2000+index*4+side*2)
                self.edit(client,*wall(slab,'Wall',[(lo+hi)/2,2.7,z],
                                      [(hi-lo)/2,4,.08],'static',[.16,.19,.26]))
        assert client.call('world.dependencies')['needs_audio_portals']

    def aperture(self, source, visible):
        path = source['portal_path']
        if not visible:
            assert path is None, path
            return
        assert path is not None, source
        route = path['apertures']
        assert [crossing['entity'] for crossing in route] == PORTALS
        assert path['entity'] == PORTALS[-1] and path['point_world'] == route[-1]['point_world']
        counts = visible if isinstance(visible,list) else [visible]*len(route)
        assert [crossing['visible_samples'] for crossing in route] == counts, route
        for row,(x,z) in zip(route,COORDINATES,strict=True):
            sample = row['sample']
            assert 0<=sample<9
            expected = [x+(sample%3-1)*.3,CLOSED_Y+(sample//3-1)*.4,z]
            assert math.dist(row['point_world'],expected)<1e-12
        points = [source['source_position'],*(row['point_world'] for row in route),source['listener_position']]
        distance = sum(math.dist(a,b) for a,b in zip(points,points[1:]))
        assert abs(path['distance_m']-distance)<1e-9
        assert abs(path['distance_gain']-1/distance)<1e-7
        assert path['propagation_delay_samples']==round(distance/343*RATE)
        last = math.dist(points[-2],points[-1])
        toward = [(p-o)/last for p,o in zip(points[-2],points[-1],strict=True)]
        assert math.dist(path['listener_local_direction'],toward)<1e-8
        gain = math.prod(PROFILE['gain']*math.sqrt(count/9) for count in counts)
        for measured,frequency in zip(path['bands'],(400,2500,10000),strict=True):
            expected = gain/math.sqrt(1+4*frequency*(distance-source['distance_m'])/343)
            assert abs(measured-expected)<1e-7
        return path

    def static_controls(self, client):
        self.data['paths']['all_open'] = self.aperture(self.observe(client),9)
        for index,gate in enumerate(GATES):
            self.edit(client,component(gate,'Transform',transform(pose(index,CLOSED_Y))))
            self.aperture(self.observe(client),0)
            self.edit(client,component(gate,'Transform',transform(pose(index,OPEN_Y))))
        self.edit(client,component(GATES[1],'Transform',transform(pose(1,PARTIAL_Y))))
        self.data['paths']['second_partial'] = self.aperture(self.observe(client),[9,6,9,9])
        self.edit(client,component(GATES[1],'Transform',transform(pose(1,OPEN_Y))))

    def saved_portal(self, client, saves):
        client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
        session = self.mount(client)
        result = client.call('runtime.step',dict(session_id=session,request_id=new_id(),
            expected_tick=0,**self.sequence[0]))
        assert result['current_tick']==12
        open_route = self.aperture(self.observe(client,(session,12)),9)
        self.save(client,session,12,'door-open')
        closed = client.call('runtime.step',dict(session_id=session,request_id=new_id(),
            expected_tick=12,**move(2,CLOSED_Y)))
        assert closed['current_tick']==24
        self.aperture(self.observe(client,(session,24)),0)
        assert self.restore_portal(client,session,24)==open_route
        return open_route

    def review_capture(self, samples):
        windows = dict(initially_open=(6,12),first_sealed=(26,32),first_reopened=(46,52),
                       last_sealed=(66,72),last_reopened=(86,92),middle_sealed=(106,112))
        measured = {label:energy(samples,*interval) for label,interval in windows.items()}
        for label,measured_energy in measured.items():
            if label.endswith('sealed'):
                assert measured_energy==0, measured
            else:
                assert measured_energy>.001, measured
        left,right = (energy(samples,*windows['initially_open'],ear) for ear in (0,1))
        assert right>1.5*left
        self.data['energy'] = measured | dict(left=left,right=right)
        self.data['energy_windows'] = windows


if __name__=='__main__':
    main(CorridorFixture, __doc__)
