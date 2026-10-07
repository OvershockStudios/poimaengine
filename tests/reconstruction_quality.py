#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent constant-radiance polygon coverage oracle and quality diagnostics.
No temporal reconstruction algorithm is reproduced here. --self-test exercises
synthetic judge controls only; --evaluate requires real renderer frame captures.
"""
import argparse
import hashlib
import json
import math
import sys
from pathlib import Path


def require(ok, why):
    if not ok:
        raise ValueError(why)


def cross(a, b, p):
    return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])


def area(poly):
    return abs(sum(a[0]*b[1]-a[1]*b[0] for a, b in zip(poly, poly[1:]+poly[:1])))*.5


def halfplane(poly, a, b, inside=True):
    if not poly:
        return []
    out=[]
    previous=poly[-1]
    prev=cross(a,b,previous)*(1 if inside else -1)
    for point in poly:
        distance=cross(a,b,point)*(1 if inside else -1)
        if (distance>=0)!=(prev>=0):
            fraction=prev/(prev-distance)
            out.append((previous[0]+fraction*(point[0]-previous[0]), previous[1]+fraction*(point[1]-previous[1])))
        if distance>=0:
            out.append(point)
        previous,prev=point,distance
    return out


def clip(poly, mask):
    for a,b in zip(mask,mask[1:]+mask[:1]):
        poly=halfplane(poly,a,b)
    return poly


def subtract(poly, mask):
    """Disjoint convex pieces outside a convex occluder, not coverage subtraction."""
    pieces=[]
    for a,b in zip(mask,mask[1:]+mask[:1]):
        outside=halfplane(poly,a,b,False)
        if len(outside)>=3 and area(outside)>1e-12:
            pieces.append(outside)
        poly=halfplane(poly,a,b)
        if not poly:
            break
    return pieces


def convex(vertices):
    require(3<=len(vertices)<=16,'Expected3..16 convex polygon vertices')
    require(all(len(p)==2 and all(math.isfinite(v) for v in p) for p in vertices),'Invalid polygon')
    p=[tuple(v) for v in vertices]
    signed=sum(a[0]*b[1]-a[1]*b[0] for a,b in zip(p,p[1:]+p[:1]))
    require(abs(signed)>1e-10,'Degenerate polygon')
    if signed<0:p.reverse()
    require(all(cross(p[i-1],p[i],p[(i+1)%len(p)])>=-1e-9 for i in range(len(p))),'Nonconvex polygon')
    return p


def project(vertices, camera, width, height):
    """Pinhole projection from explicit world vertices and rigid camera columns."""
    m=camera['world'];require(len(m)==16 and all(math.isfinite(x) for x in m),'Invalid camera')
    require(all(abs(m[i]-v)<1e-9 for i,v in ((3,0),(7,0),(11,0),(15,1))),'Camera not affine')
    axes=[m[0:3],m[4:7],m[8:11]]
    require(all(abs(sum(a*b for a,b in zip(axes[i],axes[j]))-(i==j))<1e-7 for i in range(3) for j in range(3)),'Camera not rigid')
    fov=camera['vertical_fov'];require(0<fov<179,'Invalid lens')
    tangent=math.tan(math.radians(fov)/2)
    result=[];depths=[]
    for vertex in vertices:
        require(len(vertex)==3 and all(math.isfinite(x) for x in vertex),'Invalid world vertex')
        delta=[vertex[i]-m[12+i] for i in range(3)]
        x,y,z=[sum(a*b for a,b in zip(axis,delta)) for axis in axes];depth=-z
        require(camera['near']<depth<camera['far'],'Oracle fixture must not intersect near/far clipping planes')
        result.append((width/2+x*height/(2*depth*tangent),height/2-y*height/(2*depth*tangent)));depths.append(depth)
    return convex(result),(min(depths),max(depths))


def display(value, exposure):
    value=max(0,min(65504,value))*exposure
    linear=value/(1+value)
    return linear*12.92 if linear<=.0031308 else 1.055*linear**(1/2.4)-.055


def reference(width,height,layers,background,exposure=1,owners=None):
    """Layers front-to-back, constant radiance; exact projected polygon coverage."""
    require(0<width<=2048 and 0<height<=2048 and len(layers)<=8,'Oracle budget exceeded')
    require(len(background)==3 and all(math.isfinite(x) and x>=0 for x in background),'Invalid background')
    require(math.isfinite(exposure) and exposure>=0,'Invalid exposure')
    polygons=[convex(layer['polygon']) for layer in layers]
    colors=[layer['radiance'] for layer in layers]
    require(all(len(c)==3 and all(math.isfinite(v) and 0<=v<=65504 for v in c) for c in colors),'Invalid radiance')
    image=[];edges=[];coverage=[]
    for y in range(height):
        for x in range(width):
            pixel=[(x,y),(x+1,y),(x+1,y+1),(x,y+1)]
            radiance=[0.,0.,0.];covered=0.;edge=False;owner=-2
            for index,polygon in enumerate(polygons):
                pieces=[clip(polygon,pixel)]
                for occluder in polygons[:index]:
                    pieces=[part for piece in pieces for part in subtract(piece,occluder)]
                weight=sum(area(piece) for piece in pieces)
                require(-1e-8<=weight<=1+1e-8,'Invalid oracle coverage')
                covered+=weight;edge|=1e-8<weight<1-1e-8
                if weight>1-1e-8:owner=index
                for c in range(3):radiance[c]+=weight*colors[index][c]
            require(covered<=1+1e-7,'Overlapping coverage counted twice')
            for c in range(3):radiance[c]+=max(0,1-covered)*background[c]
            if owners is not None:owners.append(-1 if covered<1e-8 else owner)
            image.append(tuple(display(v,exposure) for v in radiance));edges.append(edge);coverage.append(covered)
    return image,edges,coverage


def reveal_mask(previous,current):
    # Stable front-to-back indices: larger means farther. -1 is background;
    # -2 excludes mixed boundary coverage, which has no unique visible owner.
    return [before>=0 and (after==-1 or after>before) for before,after in zip(previous,current)]


def metrics(image,reference_image,mask=None):
    require(len(image)==len(reference_image),'Image extent differs')
    errors=[abs(a-b) for i,(rgb,ref) in enumerate(zip(image,reference_image)) if mask is None or mask[i] for a,b in zip(rgb,ref)]
    require(errors,'Empty metric mask')
    errors.sort();mse=sum(e*e for e in errors)/len(errors)
    return {'mae':sum(errors)/len(errors),'rmse':math.sqrt(mse),'p95':errors[min(len(errors)-1,math.ceil(.95*len(errors))-1)],
            'max':errors[-1],'psnr_db':None if mse==0 else -10*math.log10(mse),'channel_samples':len(errors)}


def blur(image,w,h):
    return [tuple(sum(image[yy*w+xx][c] for yy in range(max(0,y-1),min(h,y+2)) for xx in range(max(0,x-1),min(w,x+2)))/
                  ((min(h,y+2)-max(0,y-1))*(min(w,x+2)-max(0,x-1))) for c in range(3)) for y in range(h) for x in range(w)]


def self_test():
    one,_,_=reference(1,1,[{'polygon':[(0,0),(.5,0),(.5,1),(0,1)],'radiance':[1,1,1]}],[0,0,0])
    require(abs(one[0][0]-display(.5,1))<1e-12,'Half covered pixel oracle failed')
    require(abs(one[0][0]-.5*display(1,1))>.1,'Tone-map order control ineffective')
    layers=[{'polygon':[(0,0),(.5,0),(.5,1),(0,1)],'radiance':[1,0,0]}, {'polygon':[(0,0),(1,0),(1,1),(0,1)],'radiance':[0,1,0]}]
    overlap,_,_=reference(1,1,layers,[0,0,0]);require(max(abs(a-b) for a,b in zip(overlap[0],(display(.5,1),display(.5,1),0)))<1e-12,'Occlusion oracle double counted')
    w,h=24,18;poly=[(3.2,2.1),(19.8,6.6),(17.1,15.2),(2.4,12.1)]
    exact,edge,coverage=reference(w,h,[{'polygon':poly,'radiance':[1,1,1]}],[0,0,0])
    polygon=convex(poly)
    aliased=[tuple([display(1 if all(cross(a,b,(x+.5,y+.5))>=0 for a,b in zip(polygon,polygon[1:]+polygon[:1])) else 0,1)]*3) for y in range(h) for x in range(w)]
    blurred=blur(exact,w,h);shifted,_,_=reference(w,h,[{'polygon':[(x+3,y) for x,y in poly],'radiance':[1,1,1]}],[0,0,0])
    ghost=[tuple(.7*a+.3*b for a,b in zip(current,old)) for current,old in zip(shifted,exact)]
    scores={'perfect':metrics(exact,exact),'aliased':metrics(aliased,exact,edge),'blurred':metrics(blurred,exact,edge),'ghost':metrics(ghost,shifted),
            'blurred_background':metrics(blurred,exact,[v<1e-8 for v in coverage])}
    require(scores['blurred_background']['max']>.05,'Blur spread control ineffective')
    require(scores['perfect']['max']==0 and all(scores[k]['mae']>.005 for k in ('aliased','blurred','ghost')),'Synthetic defects escaped quality judge')
    old_owners=[];new_owners=[]
    rear={'polygon':[(0,0),(3,0),(3,2),(0,2)],'radiance':[0,1,0]}
    near={'polygon':[(0,0),(1,0),(1,2),(0,2)],'radiance':[1,0,0]}
    old,_,old_coverage=reference(3,2,[near,rear],[0,0,0],owners=old_owners)
    moved={**near,'polygon':[(2,0),(3,0),(3,2),(2,2)]}
    new,_,new_coverage=reference(3,2,[moved,rear],[0,0,0],owners=new_owners)
    exposed=reveal_mask(old_owners,new_owners)
    require(sum(exposed)==2 and old_coverage==new_coverage==[1]*6,'Foreground-to-farther reveal control failed')
    stale=[tuple(.6*a+.4*b for a,b in zip(current,prior)) for current,prior in zip(new,old)]
    scores['revealed_far_ghost']=metrics(stale,new,exposed)
    require(scores['revealed_far_ghost']['mae']>.1,'Fully covered farther-surface ghost escaped detection')
    require(reveal_mask([0,0,-2,1],[1,-1,-1,0])==[True,True,False,False],'Reveal ownership direction differs')
    return {'passed':True,'scope':'CPU oracle synthetic controls only; no GPU quality evidence','scores':scores}


def evaluate(manifest_path):
    from scene_capture import pixels
    doc=json.loads(manifest_path.read_text());require(doc['format']=='poima.temporal-quality-input.v1','Unknown input format')
    w,h=doc['width'],doc['height'];require(1<=len(doc['frames'])<=128,'Frame budget exceeded')
    mode_names=set(doc['frames'][0]['captures']);require(mode_names,'No captured modes')
    out={'quality_qualified':False,'status':'diagnostic metrics; no candidate thresholds calibrated','frames':[], 'source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
         'manifest_sha256':hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
         'preregistered_static_edge_target':{'warmup_accepted_frames':16,'minimum_relative_mae_reduction':.20,'baseline':'none','candidate':'fsr3_native',
             'scope':'Settled static diagonal edge-mask diagnostic only; not full temporal quality qualification'}};previous={};prior_owners=None;last_sequence=None;edge_totals={}
    for frame in doc['frames']:
        require(set(frame['captures'])==mode_names,'Each frame must contain the same modes')
        sequence=frame['sequence'];require(type(sequence) is int and (last_sequence is None or sequence==last_sequence+1),'Frames must be consecutive accepted sequence samples')
        last_sequence=sequence
        layers=[];last_depth=0
        for layer in frame['layers']:
            polygon,depth=project(layer['vertices_world'],frame['camera'],w,h)
            require(depth[0]>=last_depth-1e-8,'Oracle only accepts depth-separated front-to-back layers');last_depth=depth[1]
            layers.append({'polygon':polygon,'radiance':layer['radiance']})
        owners=[]
        ref,edge,coverage=reference(w,h,layers,frame['background'],frame['exposure'],owners);row={'sequence':frame['sequence'],'modes':{}}
        for mode,name in frame['captures'].items():
            path=manifest_path.parent/name;rgb=pixels(path);require(len(rgb)==h and all(len(line)==w for line in rgb),'BMP extent differs')
            image=[tuple(v/255 for v in pixel) for line in rgb for pixel in line]
            result={'image_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'display_rgb':metrics(image,ref)}
            if any(edge):
                result['edge_display_rgb']=metrics(image,ref,edge)
                if doc.get('sequence_name')=='static-diagonal' and sequence>16:
                    edge_totals.setdefault(mode,[]).append(result['edge_display_rgb']['mae'])
            foreground=[owner>=0 for owner in owners]
            background=[owner==-1 for owner in owners]
            if any(foreground):result['foreground_interior_display_rgb']=metrics(image,ref,foreground)
            if any(background):result['background_interior_display_rgb']=metrics(image,ref,background)
            if prior_owners is not None:
                stable_background=[old==new==-1 for old,new in zip(prior_owners,owners)]
                stable_foreground=[old==new and new>=0 for old,new in zip(prior_owners,owners)]
                if any(stable_background):result['stable_background_display_rgb']=metrics(image,ref,stable_background)
                if any(stable_foreground):result['stable_foreground_interior_display_rgb']=metrics(image,ref,stable_foreground)
            residual=[tuple(a-b for a,b in zip(p,q)) for p,q in zip(image,ref)]
            if mode in previous:
                delta=[tuple(a-b for a,b in zip(p,q)) for p,q in zip(residual,previous[mode])]
                result['screen_residual_delta_rms']=math.sqrt(sum(x*x for rgb in delta for x in rgb)/(w*h*3))
                revealed=reveal_mask(prior_owners,owners)
                if any(revealed):result['newly_revealed_display_rgb']=metrics(image,ref,revealed)
                for label,mask in (('revealed_far_surface',[flag and owner>=0 for flag,owner in zip(revealed,owners)]),
                                   ('revealed_background',[flag and owner==-1 for flag,owner in zip(revealed,owners)])):
                    if any(mask):result[label+'_display_rgb']={**metrics(image,ref,mask),'pixels':sum(mask)}
            previous[mode]=residual;row['modes'][mode]=result
        prior_owners=owners;out['frames'].append(row)
    if 'none' in edge_totals and 'fsr3_native' in edge_totals:
        baseline=sum(edge_totals['none'])/len(edge_totals['none']);candidate=sum(edge_totals['fsr3_native'])/len(edge_totals['fsr3_native'])
        meaningful=baseline>1e-6
        out['settled_static_edge_diagnostic']={'baseline_mae':baseline,'candidate_mae':candidate,'samples':len(edge_totals['none']),
            'relative_reduction':1-candidate/baseline if meaningful else None,'target_met':candidate<=.8*baseline if meaningful else None,
            'note':'A passing static edge target does not qualify ghosting, detail preservation or overall quality.'}
    return out


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--self-test',action='store_true');p.add_argument('--evaluate',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    require(not sys.flags.optimize,'Shared BMP reader requires assertions')
    require(a.self_test != bool(a.evaluate),'Choose exactly one of --self-test or --evaluate')
    result=self_test() if a.self_test else evaluate(a.evaluate.resolve());text=json.dumps(result,indent=2)+'\n'
    if a.output:a.output.write_text(text)
    print(text,end='')
if __name__=='__main__':main()
