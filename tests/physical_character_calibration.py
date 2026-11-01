#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent analytic controls for source-only convex proxy intersection math."""
import json,math,sys
from pathlib import Path
STAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(STAGE/'examples/physical-character-yard'))
import knight_profile as c

def main():
    def close(actual,wanted):c.need(abs(actual-wanted)<1e-11,'Analytic distance expected '+str(wanted)+', actual '+str(actual))
    I=dict(position=[0,0,0],rotation=[0,0,0,1]);box=dict(kind='box',half_extents=[1,1,1]);sphere=dict(kind='sphere',radius=.25)
    close(c.line_box_distance_squared([-2,0,0],[2,0,0],[1,1,1]),0)
    close(c.line_box_distance_squared([-2,0,3],[2,0,3],[1,1,1]),4)
    close(c.line_box_distance_squared([-2,2,0],[2,2,0],[1,1,1]),1)
    close(c.line_box_distance_squared([2,2,2],[3,3,3],[1,1,1]),3)
    close(c.line_line_distance_squared([-1,0,0],[1,0,0],[0,-1,2],[0,1,2]),4)
    close(c.line_line_distance_squared([0,0,0],[0,0,0],[1,-1,0],[1,1,0]),1)
    close(c.line_line_distance_squared([0,0,0],[0,0,0],[1,0,0],[1,0,0]),1)
    close(c.primitive_clearance(box,I,box,dict(position=[3,0,0],rotation=I['rotation'])),1)
    close(c.primitive_clearance(box,I,box,dict(position=[1.5,0,0],rotation=I['rotation'])),-.5)
    close(c.primitive_clearance(sphere,dict(position=[1.5,0,0],rotation=I['rotation']),box,I),.25)
    close(c.primitive_clearance(sphere,I,box,I),-.25) # guaranteed intersection; not minimum rigid separation distance.
    cap=dict(kind='capsule',radius=.2,half_height=1)
    quarter=[0,0,math.sin(math.pi/4),math.cos(math.pi/4)]
    close(c.primitive_clearance(cap,I,cap,dict(position=[0,0,0],rotation=quarter)),-.4)
    close(c.primitive_clearance(cap,dict(position=[0,1.4,0],rotation=quarter),box,I),.2)
    # OBB SAT includes face and edge axes: a quarter-turn swaps extents exactly.
    narrow=dict(kind='box',half_extents=[2,.5,.25]);other=dict(position=[0,2.6,0],rotation=quarter)
    close(c.primitive_clearance(narrow,I,narrow,other),.1)
    for a,pa,b,pb in [(box,I,sphere,dict(position=[1.5,0,0],rotation=I['rotation'])),(cap,I,box,dict(position=[2,0,0],rotation=quarter))]:
        close(c.primitive_clearance(a,pa,b,pb),c.primitive_clearance(b,pb,a,pa))
    print(json.dumps(dict(source_math_only=True,passed=True,analytic_checks=16,
        scope='Primitive geometric audit algebra only; no native engine, proxy admission, contact solver, rendering or performance result.')))
if __name__=='__main__':main()
