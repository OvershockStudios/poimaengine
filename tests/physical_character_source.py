#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure original-source/profile algebra checks; never a native qualification."""
import argparse,hashlib,json,math,sys
from pathlib import Path
STAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(STAGE/'examples/physical-character-yard'))
import knight_profile as k
import physical_character_native as native
from imported_character_contract import point,transform_matrix,multiply

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True,type=Path);p.add_argument('--license',required=True,type=Path);a=p.parse_args()
    originals=[x.read_bytes()for x in(a.source,a.license)]
    oracle,profile,report=k.inspect_inputs(a.source,a.license)
    k.need(len(profile['parts'])==15 and len(profile['joints'])==14 and profile['pose_frame']=='skeletal_fixed_scale','Explicit native profile population and policy')
    k.need(report['calibration']['total_mass']==72.,'Explicit authored total mass')
    byname={p['name']:p for p in profile['parts']};rest=oracle.pose();frames=k.logical_frames(oracle,rest)
    for joint in profile['joints']:
        parent,child=byname[joint['parent']],byname[joint['child']]
        current=child['node'];ancestry=[]
        while current in oracle.parents:current=oracle.parents[current];ancestry.append(current)
        k.need(parent['node']in ancestry,'Physical parent is an actual original ancestor through unmapped nodes')
        lhs=k.compose(k.compose(frames[parent['node']],parent['body_in_bone']),joint['parent_frame'])
        rhs=k.compose(k.compose(frames[child['node']],child['body_in_bone']),joint['child_frame'])
        k.near(lhs['position'],rhs['position'],1e-12,'Independent rest COM/joint anchor closure')
        k.need(abs(abs(k.dot(lhs['rotation'],rhs['rotation']))-1)<1e-12,'Independent rest joint frame closure')
    # Original source anisotropy is an actual retained input, not an inflated
    # admission epsilon. Initialized scales differ between these exact takes.
    idle,run=[oracle.pose(i,0)for i in (36,48)]
    changed=[i for i in range(57)if idle[0][i]['scale']!=run[0][i]['scale']]
    k.need(len(changed)==28,'Original per-take scale difference remains visible')
    for index in (36,48):
        scales=[c for c in oracle.clips[index]['channels']if c['path']=='scale']
        k.need(all(all(v==c['values'][0]for v in c['values'])for c in scales),'Every original selected scale key stays constant')
        # Endpoint and non-key samples must retain the exact initialized scale;
        # no averaging or removal of the original channels is permitted.
        for t in (0.,.173,oracle.clips[index]['duration']*.53,oracle.clips[index]['duration']):
            pose=oracle.pose(index,t)
            k.need(all(a['scale']==b['scale']for a,b in zip(pose[0],oracle.pose(index,0)[0])),'Independent curve interpolation preserves exact selected fixed scales')
    # Explicit nonuniform/sheared example: use complete parent affine inverse
    # for translations. A rigid logical frame differs from affine column axes.
    parent=transform_matrix([2,-3,4],[0,0,math.sin(.31),math.cos(.31)],[2,3,4])
    child=transform_matrix([.2,.3,.4],[math.sin(.23),0,0,math.cos(.23)],[1,1.5,.8])
    world=multiply(parent,child);origin=point(world,[0,0,0]);logical=k.quaternion([0,0,math.sin(.31),math.cos(.31)],[math.sin(.23),0,0,math.cos(.23)])
    expected_origin=k.add([2,-3,4],k.rotate([0,0,math.sin(.31),math.cos(.31)],[.4,.9,1.6]))
    k.near(origin,expected_origin,1e-12,'Full affine origin independent analytic expectation')
    body=k.compose(dict(position=origin,rotation=logical),dict(position=[.12,-.07,.03],rotation=[0,0,0,1]))
    k.near(body['position'],k.add(expected_origin,k.rotate(logical,[.12,-.07,.03])),1e-12,'Metric COM offset independent of axis scales')
    # Pure independent quaternion-log controls, including q/-q and world axes.
    angle=.031;before=[0,0,0,1];after=[0,math.sin(angle/2),0,math.cos(angle/2)]
    k.near(native.angular_velocity(before,after),[0,angle*60,0],1e-10,'Analytic world-Y quaternion increment')
    k.near(native.angular_velocity(before,[-v for v in after]),[0,angle*60,0],1e-10,'Shortest arc independent of quaternion representation sign')
    offset=[math.sin(.4),0,0,math.cos(.4)]
    old=k.quaternion(offset,before);current=k.quaternion(offset,after)
    k.near(native.angular_velocity(old,current),k.rotate(offset,[0,angle*60,0]),1e-10,'World angular axis includes original/external quaternion ancestry')
    for clip in (36,48):
        previous=native.source_bodies(oracle,profile,clip,5);current=native.source_bodies(oracle,profile,clip,6)
        linear=[k.mul(k.sub(a['position'],b['position']),60)for a,b in zip(current,previous)]
        angular=[native.angular_velocity(b['rotation'],a['rotation'])for a,b in zip(current,previous)]
        k.need(any(k.norm(v)>.001 for v in linear+angular),'Selected played interval provides actual original source momentum')
        k.need(all(math.isfinite(x)for row in linear+angular for x in row),'Finite original source-derived played momentum')
    k.need(all(x.read_bytes()==b for x,b in zip((a.source,a.license),originals)),'Original source/license untouched')
    print(json.dumps(dict(source_only=True,passed=True,parts=15,joints=14,takes=76,changed_scale_nodes=changed,
        limitations=['Independent source and algebra checks only. No native binding, takeover, contacts, saves, rendering or humanoid quality result.'])))
if __name__=='__main__':main()
