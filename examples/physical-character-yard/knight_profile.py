#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original Knight metric physical-profile recipe and source-only geometric audit.

Requires the unchanged caller-supplied KayKit Knight GLB and CC0 notice.
Uses independent original GLB/accessor/FK/LBS qualification algorithms from the
source checkout. Does not create artwork, invoke an engine, or alter the model.
"""
import argparse,copy,json,math,sys
from pathlib import Path
REPO=next(p for p in Path(__file__).resolve().parents if (p/'tests/character_library_intake.py').is_file())
sys.path.insert(0,str(REPO/'tests'))
from character_library_intake import LibraryOracle,SOURCE_SHA,LICENSE_SHA,decode_cooked,near,need,identity
from imported_character_contract import point
PARTS = [
 ('pelvis',30,[30,21],12.,'box',21), ('chest',20,[20],20.,'box',18),
 ('head',18,[18],5.,'sphere',None),
 ('upper_arm_l',9,[9],2.,'capsule',8), ('forearm_l',8,[8,7],1.5,'capsule',6),
 ('hand_l',6,[6,5],.5,'box',None),
 ('upper_arm_r',16,[16],2.,'capsule',15), ('forearm_r',15,[15,14],1.5,'capsule',13),
 ('hand_r',13,[13,12],.5,'box',None),
 ('thigh_l',25,[25],8.,'capsule',24), ('shin_l',24,[24],4.,'capsule',23),
 ('foot_l',23,[23,22],1.5,'box',None),
 ('thigh_r',29,[29],8.,'capsule',28), ('shin_r',28,[28],4.,'capsule',27),
 ('foot_r',27,[27,26],1.5,'box',None)]

JOINTS = [
 ('spine','pelvis','chest','swing_twist',(.55,.4,-.45,.45)),
 ('neck','chest','head','swing_twist',(.6,.6,-.7,.7)),
 ('shoulder_l','chest','upper_arm_l','swing_twist',(1.45,1.45,-1.2,1.2)),
 ('elbow_l','upper_arm_l','forearm_l','hinge',(-.12,2.65)),
 ('wrist_l','forearm_l','hand_l','swing_twist',(.55,.55,-.65,.65)),
 ('shoulder_r','chest','upper_arm_r','swing_twist',(1.45,1.45,-1.2,1.2)),
 ('elbow_r','upper_arm_r','forearm_r','hinge',(-.12,2.65)),
 ('wrist_r','forearm_r','hand_r','swing_twist',(.55,.55,-.65,.65)),
 ('hip_l','pelvis','thigh_l','swing_twist',(1.05,.7,-.65,.65)),
 ('knee_l','thigh_l','shin_l','hinge',(-.10,2.45)),
 ('ankle_l','shin_l','foot_l','swing_twist',(.45,.3,-.25,.25)),
 ('hip_r','pelvis','thigh_r','swing_twist',(1.05,.7,-.65,.65)),
 ('knee_r','thigh_r','shin_r','hinge',(-.10,2.45)),
 ('ankle_r','shin_r','foot_r','swing_twist',(.45,.3,-.25,.25))]

SELECTED = [(36,'Idle'),(48,'Running_A')]

def add(a,b): return [x+y for x,y in zip(a,b)]

def sub(a,b): return [x-y for x,y in zip(a,b)]

def mul(a,s): return [x*s for x in a]

def dot(a,b): return sum(x*y for x,y in zip(a,b))

def norm(a): return math.sqrt(dot(a,a))

def unit(a):
    n=norm(a); need(n>1e-12,'Nonzero vector'); return mul(a,1/n)

def conjugate(q): return [-q[0],-q[1],-q[2],q[3]]

def quaternion(a,b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]

def normalized(q): return unit(q)

def rotate(q,p): return quaternion(quaternion(q,[*p,0]),conjugate(q))[:3]

def qfrom_to(a,b):
    a,b=unit(a),unit(b); d=dot(a,b)
    if d < -.999999999: return [1,0,0,0] if abs(a[0])<.9 else [0,1,0,0]
    cross=[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
    return normalized([*cross,1+d])

def logical_frames(oracle, pose):
    rotations={}
    def visit(i):
        if i not in rotations:
            local=normalized(pose[0][i]['rotation']); parent=oracle.parents.get(i)
            rotations[i]=normalized(quaternion(visit(parent),local)) if parent is not None else local
        return rotations[i]
    return [{'position':point(pose[2][i],[0,0,0]),'rotation':visit(i)} for i in range(len(oracle.nodes))]

def compose(a,b): return {'position':add(a['position'],rotate(a['rotation'],b['position'])), 'rotation':normalized(quaternion(a['rotation'],b['rotation']))}

def relative(parent,child):
    q=conjugate(parent['rotation'])
    return {'position':rotate(q,sub(child['position'],parent['position'])),'rotation':normalized(quaternion(q,child['rotation']))}

def percentile(values,p):
    rows=sorted(values); need(rows,'Nonempty retained geometry'); k=(len(rows)-1)*p; i=int(k)
    return rows[i]+(rows[min(i+1,len(rows)-1)]-rows[i])*(k-i)

def local_points(oracle,pose,frames,index,members):
    rows=[]; chosen=set(members); inverse=conjugate(frames[index]['rotation'])
    palette=oracle.skin['joints']
    for ni,node in enumerate(oracle.nodes):
        if 'mesh' not in node or 'skin' not in node: continue
        mesh=oracle.meshes[node['mesh']]
        for i,(joints,weights) in enumerate(zip(mesh['joints'],mesh['weights'])):
            weight=sum(w for j,w in zip(joints,weights) if palette[j] in chosen)
            if weight < .5: continue
            p=oracle.vertex(pose,ni,i)[0]
            rows.append(rotate(inverse,sub(p,frames[index]['position'])))
    need(len(rows)>=4, 'Insufficient original weighted geometry for '+str(index))
    return rows

def cross(a,c):return [a[1]*c[2]-a[2]*c[1],a[2]*c[0]-a[0]*c[2],a[0]*c[1]-a[1]*c[0]]

def clamp(x,lo,hi):return max(lo,min(hi,x))

def line_box_distance_squared(a,c,extent):
    """Exact finite piecewise-quadratic segment/AABB distance, no search fit."""
    direction=sub(c,a);cuts={0.,1.}
    for origin,delta,e in zip(a,direction,extent):
        if delta:
            for face in (-e,e):
                t=(face-origin)/delta
                if 0<t<1:cuts.add(t)
    cuts=sorted(cuts);candidates=list(cuts)
    for lo,hi in zip(cuts,cuts[1:]):
        mid=(lo+hi)/2;aa=bb=0.
        for origin,delta,e in zip(a,direction,extent):
            coordinate=origin+mid*delta
            if -e<=coordinate<=e:continue
            face=-e if coordinate < -e else e
            aa+=delta*delta;bb+=delta*(origin-face)
        if aa:candidates.append(clamp(-bb/aa,lo,hi))
    return min(sum(max(abs(origin+t*delta)-e,0)**2 for origin,delta,e in zip(a,direction,extent))for t in candidates)

def line_line_distance_squared(p,q,a,c):
    # Finite segment closest-point equations, including all endpoint regions.
    u,v,w=sub(q,p),sub(c,a),sub(p,a);aa=dot(u,u);bb=dot(u,v);cc=dot(v,v);dd=dot(u,w);ee=dot(v,w)
    if aa<1e-24:
        t=clamp(ee/cc,0,1)if cc>1e-24 else 0.;result=sub(w,mul(v,t));return dot(result,result)
    if cc<1e-24:t=clamp(-dd/aa,0,1);return dot(add(w,mul(u,t)),add(w,mul(u,t)))
    denom=aa*cc-bb*bb;s=clamp((bb*ee-cc*dd)/denom,0,1)if denom>1e-24 else 0
    t=(bb*s+ee)/cc
    if t<0:t=0;s=clamp(-dd/aa,0,1)
    elif t>1:t=1;s=clamp((bb-dd)/aa,0,1)
    result=sub(add(w,mul(u,s)),mul(v,t));return dot(result,result)

def cap_segment(pose,shape):
    delta=rotate(pose['rotation'],[0,shape.get('half_height',0),0])
    return sub(pose['position'],delta),add(pose['position'],delta)

def box_axes(pose):return [rotate(pose['rotation'],[float(i==j)for i in range(3)])for j in range(3)]

def primitive_clearance(a,pa,c,pc):
    """Positive separated / negative intersecting; zero tangency.

    Rounded shape distances are exact. Box/box uses all15 normalized SAT axes;
    positive SAT gap is a separation witness, not general Euclidean distance.
    Negative minimum SAT overlap is a penetration witness, not solver response.
    """
    ka,kc=a['kind'],c['kind']
    if ka=='box'and kc=='box':
        ax,cx=box_axes(pa),box_axes(pc);delta=sub(pc['position'],pa['position']);axes=ax+cx
        axes += [unit(v)for x in ax for y in cx if norm(v:=cross(x,y))>1e-10]
        gaps=[abs(dot(delta,n))-sum(e*abs(dot(x,n))for e,x in zip(a['half_extents'],ax))-sum(e*abs(dot(x,n))for e,x in zip(c['half_extents'],cx))for n in axes]
        return max(gaps)
    if ka=='box':return primitive_clearance(c,pc,a,pa)
    if kc=='box':
        start,end=cap_segment(pa,a);inverse=conjugate(pc['rotation'])
        start=rotate(inverse,sub(start,pc['position']));end=rotate(inverse,sub(end,pc['position']))
        return math.sqrt(line_box_distance_squared(start,end,c['half_extents']))-a['radius']
    p,q=cap_segment(pa,a);r,s=cap_segment(pc,c)
    if norm(sub(p,q))<1e-12:
        direction=sub(s,r);dd=dot(direction,direction);t=clamp(dot(sub(p,r),direction)/dd,0,1)if dd else 0
        distance=norm(sub(p,add(r,mul(direction,t))))
    elif norm(sub(r,s))<1e-12:
        direction=sub(q,p);t=clamp(dot(sub(r,p),direction)/dot(direction,direction),0,1);distance=norm(sub(r,add(p,mul(direction,t))))
    else:distance=math.sqrt(max(0,line_line_distance_squared(p,q,r,s)))
    return distance-a['radius']-c['radius']

def source_world_bodies(oracle,profile,clip=None,time=0):
    frames=logical_frames(oracle,oracle.pose(clip,time))
    return {p['name']:compose(frames[p['node']],p['body_in_bone'])for p in profile['parts']}

def penetration_audit(oracle,profile):
    adjacent={frozenset((j['parent'],j['child']))for j in profile['joints']};rows=[]
    for clip,t,label in [(None,0,'source-rest'),(36,0,'Idle-initial'),(36,6/60,'Idle-played-six'),(48,0,'Running_A-initial'),(48,6/60,'Running_A-played-six')]:
        frames=source_world_bodies(oracle,profile,clip,t);pairs=[]
        for i,a in enumerate(profile['parts']):
            for c in profile['parts'][i+1:]:
                if frozenset((a['name'],c['name']))in adjacent:continue
                clearance=primitive_clearance(a['shape'],frames[a['name']],c['shape'],frames[c['name']])
                pairs.append(dict(a=a['name'],b=c['name'],signed_clearance_m=clearance,intersecting=clearance < -1e-9))
        rows.append(dict(label=label,clip=clip,time=t,checked_nonadjacent_pairs=len(pairs),intersections=[p for p in pairs if p['intersecting']],minimum_clearance_m=min(p['signed_clearance_m']for p in pairs),pairs=pairs))
    return dict(scope='Independent original-source static convex proxy intersection audit. No Jolt contacts, rendering, native tolerances or quality claim.',adjacent_excluded_pairs=len(adjacent),poses=rows)

def make_profile(oracle):
    rest=oracle.pose();frames=logical_frames(oracle,rest);parts=[];axis_rows=[]
    for name,node,members,mass,kind,endpoint in PARTS:
        points=local_points(oracle,rest,frames,node,members)
        low=[percentile([p[k]for p in points],.02)for k in range(3)]
        high=[percentile([p[k]for p in points],.98)for k in range(3)]
        center=mul(add(low,high),.5);q=[0,0,0,1]
        if kind=='box':shape=dict(kind='box',half_extents=[max(.012,.5*(b-a)*.90)for a,b in zip(low,high)])
        elif kind=='sphere':shape=dict(kind='sphere',radius=max(.03,percentile([norm(sub(p,center))for p in points],.90)*.90))
        else:
            # Align the physical capsule's Y to the actual original segment.
            # The authored joint twist-X axis is calibrated separately below.
            direction=rotate(conjugate(frames[node]['rotation']),sub(frames[endpoint]['position'],frames[node]['position']))
            direction=unit(direction);q=qfrom_to([0,1,0],direction);inverse=conjugate(q)
            oriented=[rotate(inverse,p)for p in points]
            low=[percentile([p[i]for p in oriented],.02)for i in range(3)];high=[percentile([p[i]for p in oriented],.98)for i in range(3)]
            local_center=mul(add(low,high),.5);radial=[math.hypot(p[0]-local_center[0],p[2]-local_center[2])for p in oriented]
            radius=max(.025,percentile(radial,.90)*.98);half=max(0,(high[1]-low[1])*.48-radius)
            center=rotate(q,local_center);shape=dict(kind='capsule',radius=radius,half_height=half)
            axis_rows.append(dict(name=name,node=node,endpoint_node=endpoint,bone_local_endpoint_direction=direction,
                boneY_endpoint_angle_rad=math.acos(clamp(direction[1],-1,1)),proxy_local_rotation=q,source_endpoint_world_direction=unit(sub(frames[endpoint]['position'],frames[node]['position']))))
        parts.append(dict(name=name,node=node,body_in_bone=dict(position=center,rotation=q),shape=shape,mass=mass,
            friction=.65,restitution=0.,linear_damping=.10,angular_damping=.15))
    byname={p['name']:p for p in parts}
    # Meaningful core proxies use explicit skeletal landmark spans instead of
    # assigning every broadly influenced armor vertex to a shoulder-high pelvis.
    for name,start,end,members,width_fraction,depth_fraction,span_fraction in [
        ('pelvis',30,21,[30],.70,.65,1.20),('chest',21,18,[21,20],.85,.80,.82)]:
        part=byname[name];node=part['node'];origin=frames[node]['position'];rotation=frames[node]['rotation']
        start_local=rotate(conjugate(rotation),sub(frames[start]['position'],origin));end_local=rotate(conjugate(rotation),sub(frames[end]['position'],origin))
        q=qfrom_to([0,1,0],sub(end_local,start_local));inverse=conjugate(q)
        points=[rotate(inverse,p)for p in local_points(oracle,rest,frames,node,members)]
        lows=[percentile([p[i]for p in points],.02)for i in range(3)];highs=[percentile([p[i]for p in points],.98)for i in range(3)]
        center=mul(add(start_local,end_local),.5)
        shape=dict(kind='box',half_extents=[max(.03,(highs[0]-lows[0])*.5*width_fraction),norm(sub(end_local,start_local))*.5*span_fraction,max(.03,(highs[2]-lows[2])*.5*depth_fraction)])
        part['body_in_bone']=dict(position=center,rotation=q);part['shape']=shape
    bodies={p['name']:compose(frames[p['node']],p['body_in_bone'])for p in parts};joint_axes=[];joints=[]
    endpoints={name:(node,end)for name,node,_,_,_,end in PARTS if end is not None}
    endpoints.update(hand_l=(6,5),hand_r=(13,12),foot_l=(23,22),foot_r=(27,26))
    def longitudinal(name):
        start,end=endpoints[name];return unit(sub(frames[end]['position'],frames[start]['position']))
    for name,parent,child,kind,limits in JOINTS:
        source='source longitudinal'
        if name.startswith('elbow')or name.startswith('knee'):
            axis=cross(longitudinal(parent),longitudinal(child));need(norm(axis)>.001,'Original source hinge plane must be nondegenerate');axis=unit(axis);source='cross(original parent and child segment directions)'
        elif name.startswith('wrist'):axis=longitudinal(parent)
        elif child in endpoints:axis=longitudinal(child)
        else:axis=unit(sub(frames[byname[child]['node']]['position'],frames[byname[parent]['node']]['position']))
        frame=dict(position=frames[byname[child]['node']]['position'],rotation=qfrom_to([1,0,0],axis))
        pf=relative(bodies[parent],frame);cf=relative(bodies[child],frame)
        bound=dict(kind=kind,minimum=limits[0],maximum=limits[1])if kind=='hinge'else dict(kind=kind,swing_y=limits[0],swing_z=limits[1],twist_min=limits[2],twist_max=limits[3])
        joints.append(dict(name=name,parent=parent,child=child,parent_frame=pf,child_frame=cf,limits=bound,neutral_rotation=[0,0,0,1],
            rest_stiffness=.5,rest_damping=.08,soft_limit_stiffness=5.,soft_limit_zone=.04,max_torque=60.,friction_torque=.10))
        joint_axes.append(dict(name=name,world_constraintX=axis,derivation=source))
    profile=dict(root='pelvis',initial_mode='animated',pose_frame='skeletal_fixed_scale',parts=parts,joints=joints)
    coverage=[]
    for name,node,members,_,_,_ in PARTS:
        part=byname[name];local=part['body_in_bone'];inverse=conjugate(local['rotation']);shape=part['shape'];points=local_points(oracle,rest,frames,node,members);inside=0
        for point in points:
            p=rotate(inverse,sub(point,local['position']))
            if shape['kind']=='box':included=all(abs(x)<=e for x,e in zip(p,shape['half_extents']))
            elif shape['kind']=='sphere':included=norm(p)<=shape['radius']
            else:included=norm(sub(p,[0,clamp(p[1],-shape['half_height'],shape['half_height']),0]))<=shape['radius']
            inside+=included
        coverage.append(dict(name=name,weighted_vertex_count=len(points),inside_fraction=inside/len(points)))
    return profile,dict(total_mass=sum(p['mass']for p in parts),capsule_source_axes=axis_rows,joint_source_axes=joint_axes,coverage=coverage,
        tuning='Provisional source-landmark proxy tuning; geometry coverage is diagnostic, not anatomical accuracy or native contact acceptance.')

def inspect_inputs(source,license):
    source=Path(source);license=Path(license);source_bytes=source.read_bytes();license_bytes=license.read_bytes()
    need(identity(source_bytes)==dict(bytes=3659532,sha256=SOURCE_SHA),'Exact unchanged original Knight GLB required')
    need(identity(license_bytes)==dict(bytes=891,sha256=LICENSE_SHA),'Exact original CC0 license required')
    oracle=LibraryOracle(source_bytes);profile,calibration=make_profile(oracle)
    selected_path=Path(__file__).parent/'profiles/knight.json';selected_bytes=selected_path.read_bytes()
    generated=(json.dumps(profile,indent=2,allow_nan=False)+'\n').encode()
    need(generated==selected_bytes,'Source recipe must reproduce every selected profile byte')
    selected=[]
    for index,name in SELECTED:
        clip=oracle.clips[index];need(clip['name']==name,'Original selected take identity')
        scales=[c for c in clip['channels']if c['path']=='scale']
        need(all(all(v==c['values'][0]for v in c['values'])for c in scales),'Every selected source scale channel is exactly constant')
        initial=oracle.pose(index,0);poses=source_world_bodies(oracle,profile,index,0)
        selected.append(dict(index=index,name=name,duration=clip['duration'],initialized_time=0.,frozen_scales=[row['scale']for row in initial[0]],
            initial_bodies=[dict(name=p['name'],**poses[p['name']])for p in profile['parts']],scope='Independent initialized world; no clip switching, crossfade or variable-scale take admission.'))
    need(source.read_bytes()==source_bytes and license.read_bytes()==license_bytes,'Original source/license changed during inspection')
    report=dict(format='poima.physical-character-calibration.v1',source=identity(source_bytes),license=identity(license_bytes),selected_profile=identity(selected_bytes),
        retained_library=dict(nodes=57,skin_joints=41,takes=76,vertices=7024,triangles=6952),selected=selected,calibration=calibration,
        nonadjacent_audit=penetration_audit(oracle,profile))
    return oracle,profile,report

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    need(not a.output.exists(),'Fresh output required');oracle,profile,report=inspect_inputs(a.source,a.license);a.output.mkdir(parents=True)
    for name,value in [('physical-profile.json',profile),('calibration.json',report),('animation-library.json',oracle.summaries())]:(a.output/name).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
    print(json.dumps(dict(source_only=True,parts=15,joints=14,total_mass=72.,source=report['source'],selected_profile=report['selected_profile'],
        intersections={p['label']:len(p['intersections'])for p in report['nonadjacent_audit']['poses']})))
if __name__=='__main__':main()
