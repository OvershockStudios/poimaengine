// SPDX-License-Identifier: Apache-2.0
// Two parabolas enclose the sheet. Each clipped sightline intersects at
// most two intervals per sheet. Bounds are relative to the primary camera.
struct FallingWaterSheet {
    float4 origin_length;
    float4 heading_width; // Horizontal heading, half width, aeration strength.
    float4 shape; // Curvature, vertical thickness, launch speed, phase in seconds.
};
StructuredBuffer<FallingWaterSheet> water_sheets : register(t11);

// A convex quadratic is below its cutoff between its roots. Work on the
// footprint interval remapped to [0,1] to keep the polynomial well scaled.
float2 sheet_below(float3 coeff, float level) {
    const float c = coeff.z-level;
    if (coeff.x <= 1e-10) {
        if (abs(coeff.y)<=1e-10) return c<=0 ? float2(0,1) : float2(1,0);
        const float root=clamp(-c/coeff.y,0,1);
        return coeff.y>0 ? float2(0,root) : float2(root,1);
    }
    const float disc=coeff.y*coeff.y-4*coeff.x*c;
    if (disc<=0) return float2(1,0);
    const float q=-.5*(coeff.y+(coeff.y>=0 ? 1 : -1)*sqrt(disc));
    const float2 roots=float2(q/coeff.x,c/q);
    return saturate(float2(min(roots.x,roots.y),max(roots.x,roots.y)));
}

float2 sheet_ripples(FallingWaterSheet sheet,float across,float along,float distance) {
    const float2 heading=sheet.heading_width.xy;
    const float2 crosswise=float2(-heading.y,heading.x);
    const float advected=along/sheet.shape.z-sheet.shape.w;
    const float footprint=2*distance*max(temporal_right.w/cluster_viewport.z,temporal_up.w/cluster_viewport.w);
    const float fade=1-smoothstep(.006,.06,footprint);
    const float a=6.2831853*(4*across+3*advected);
    const float b=6.2831853*(11*across-5*advected);
    return fade*(crosswise*(.055*cos(a)+.02*cos(b))+heading*(.025*sin(a-b)));
}

void pick_sheet_surface(FallingWaterSheet sheet,float3 coeff,float interval_edge,bool entering,
                        float start,float duration,float along,float across,float2 motion,inout WaterPath path) {
    if (interval_edge<=0 || interval_edge>=1) return; // Side and end caps are open.
    const float surface=start+interval_edge*duration;
    if (path.interface_m>=0 && surface>=path.interface_m) return;
    const float s=along+interval_edge*motion.x;
    const float u=across+interval_edge*motion.y;
    const float height=(coeff.x*interval_edge+coeff.y)*interval_edge+coeff.z;
    path.interface_m=surface;
    path.interface_gradient=-2*sheet.shape.x*s*sheet.heading_width.xy;
    path.enters=entering;
    path.offshore_interface=false;
    path.reservoir_interface=false;
    path.sheet_side=height>.5*sheet.shape.y ? 1.0 : -1.0;
    path.sheet_ripple=sheet_ripples(sheet,u,s,surface);
}

float added_sheet_length(WaterConstants water,float3 eye,float3 ray,float2 span,bool have_storage_water) {
    const float length_m=span.y-span.x;
    if (length_m<=0) return 0;
    if (!have_storage_water) return length_m;
    const float overlap=trace_storage_water(water,eye+ray*span.x,ray,length_m,false).length_m;
    return max(0,length_m-overlap);
}

float sheet_bubble_extinction(FallingWaterSheet sheet,float along,float across,float distance) {
    const float progress=smoothstep(.02,.72,along/sheet.origin_length.w);
    const float edge=1-smoothstep(.77,1,abs(across)/sheet.heading_width.z);
    const float footprint=2*distance*max(temporal_right.w/cluster_viewport.z,temporal_up.w/cluster_viewport.w);
    const float time=along/sheet.shape.z-sheet.shape.w;
    const float broad=.5+.5*cos(6.2831853*(3.7*across+4*time));
    const float fine=.5+.5*cos(6.2831853*(9.2*across-7*time));
    const float streaks=lerp(broad,.5,smoothstep(.035,.14,footprint))*.65+
                        lerp(fine,.5,smoothstep(.014,.065,footprint))*.35;
    return 32*sheet.heading_width.w*progress*edge*(.5+.5*streaks);
}

WaterPath trace_water_columns(WaterConstants water,float3 eye,float3 ray,float distance,bool find_interface) {
    WaterPath path=trace_storage_water(water,eye,ray,distance,find_interface);
    const uint count=min(8u,water_sheet_count(water));
    [branch] if (count==0) return path;
    float3 spans[16]; // Entry, exit and midpoint bubble extinction.
    uint span_count=0;
    const float3 camera_relative=eye-water.camera_cell_size.xyz;
    [loop] for (uint i=0;i<count;++i) {
        const FallingWaterSheet sheet=water_sheets[i];
        const float2 hd=sheet.heading_width.xy;
        const float2 crosswise=float2(-hd.y,hd.x);
        const float3 local=camera_relative-sheet.origin_length.xyz;
        const float2 axes=float2(dot(local.xz,hd),dot(local.xz,crosswise));
        const float2 motion=float2(dot(ray.xz,hd),dot(ray.xz,crosswise));
        float entry=0,exit=distance;
        if (!clip_water_span(axes.x,motion.x,0,sheet.origin_length.w,entry,exit) ||
            !clip_water_span(axes.y,motion.y,-sheet.heading_width.z,sheet.heading_width.z,entry,exit)) continue;
        const float duration=exit-entry;
        const float2 start=axes+motion*entry;
        const float2 step=motion*duration;
        const float k=sheet.shape.x;
        const float3 coeff=float3(k*step.x*step.x,ray.y*duration+2*k*start.x*step.x,
            local.y+ray.y*entry+k*start.x*start.x);
        const float2 outer=sheet_below(coeff,sheet.shape.y);
        if (outer.y<=outer.x) continue;
        const float2 cut=sheet_below(coeff,0);
        float2 intervals[2];
        intervals[0]=float2(outer.x,cut.y>cut.x ? min(outer.y,cut.x) : outer.y);
        intervals[1]=float2(max(outer.x,cut.y),cut.y>cut.x ? outer.y : 0);
        [unroll] for (uint j=0;j<2;++j) {
            const float2 band=intervals[j];
            if (band.y<=band.x) continue;
            if (find_interface) {
                pick_sheet_surface(sheet,coeff,band.x,true,entry,duration,start.x,start.y,step,path);
                pick_sheet_surface(sheet,coeff,band.y,false,entry,duration,start.x,start.y,step,path);
            }
            const float2 interval=entry+duration*band;
            float bubble_extinction=0;
            [branch] if (sheet.heading_width.w>0) {
                const float2 uv=start+step*((band.x+band.y)*.5);
                bubble_extinction=sheet_bubble_extinction(sheet,uv.x,uv.y,(interval.x+interval.y)*.5);
            }
            const float3 span=float3(interval,bubble_extinction);
            uint where=span_count++;
            [loop] while (where>0) {
                const float3 prior=spans[where-1];
                if (prior.x<=span.x) break;
                spans[where]=prior;
                --where;
            }
            spans[where]=span;
        }
    }
    if (span_count==0) return path;
    const bool storage=path.length_m>0;
    if (path.sheet_side!=0 && storage)
        path.water_before_interface_m=trace_storage_water(water,eye,ray,path.interface_m,false).length_m;
    [loop] for (uint j=0;j<span_count;++j) {
        const float3 span=spans[j];
        [branch] if (span.z<=0) continue;
        const float depth=span.z*added_sheet_length(water,eye,ray,span.xy,storage);
        path.bubble_depth+=depth;
        path.bubble_distance_weighted+=depth*((span.x+span.y)*.5);
        if (path.interface_m>=span.y) path.bubble_before_interface+=depth;
        else if (path.interface_m>span.x)
            path.bubble_before_interface+=span.z*added_sheet_length(water,eye,ray,float2(span.x,path.interface_m),storage);
    }
    float2 merged=spans[0].xy;
    [loop] for (uint i=1;i<=span_count;++i) {
        if (i<span_count) {
            const float2 next=spans[i].xy;
            if (next.x<=merged.y) { merged.y=max(merged.y,next.y); continue; }
        }
        path.length_m+=added_sheet_length(water,eye,ray,merged,storage);
        if (path.interface_m>merged.x)
            path.water_before_interface_m+=added_sheet_length(water,eye,ray,float2(merged.x,min(merged.y,path.interface_m)),storage);
        if (i<span_count) merged=spans[i];
    }
    return path;
}
