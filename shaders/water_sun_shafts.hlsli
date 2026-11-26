// SPDX-License-Identifier: Apache-2.0
// All medium queries are relative to the water origin. Shadow receivers are
// converted back to world space only at the air-water interface.
bool water_sun_column(WaterConstants water,float2 xz,out float2 levels) {
    if (offshore_contains(xz)) {
        levels=water_offshore.zy;
        return true;
    }
    const float2 local=xz/water.camera_cell_size.w;
    const int2 dimensions=water_grid_dimensions(water);
    if (any(local<0) || any(local>=float2(dimensions))) {
        levels=0;
        return false;
    }
    const int2 cell=int2(local);
    levels=water_column_levels(water_columns[cell.y*dimensions.x+cell.x],frac(local));
    return levels.y>levels.x;
}

// Find a surface entry along the refracted mean direction. A second query
// updates the height for sloped or moving water before checking the residual.
float water_sun_distance(WaterConstants water,float3 position,float top,float3 toward_water_sun,
                         out float3 surface) {
    surface=position;
    float2 levels;
    [unroll] for (uint iteration=0;iteration<2;++iteration) {
        surface=position+toward_water_sun*((top-position.y)/toward_water_sun.y);
        if (!water_sun_column(water,surface.xz,levels) || levels.y<=position.y) return 0;
        top=levels.y;
    }
    const float distance=(top-position.y)/toward_water_sun.y;
    surface=position+distance*toward_water_sun;
    if (distance<=0 || !water_sun_column(water,surface.xz,levels) || abs(levels.y-surface.y)>.03) return 0;
    const float3 halfway=(position+surface)*.5;
    if (!water_sun_column(water,halfway.xz,levels) || halfway.y<levels.x || halfway.y>=levels.y) return 0;
    return distance;
}

float3 integrate_water_sun(WaterConstants water,float3 eye,float3 ray,float2 span) {
    const uint ordinal=uint(water_sun_shafts.y);
    if (span.y<=span.x || ordinal==0 || ordinal>light_count.x) return 0;
    SceneLight sun=lights[ordinal-1];
    const float3 toward_sun=-sun.direction_range.xyz;
    const float3 up=float3(0,1,0);
    // Preserve the mean Snell direction for a stable volume. The separate
    // caustics pass retains fine focusing at opaque receivers.
    const float3 toward_water_sun=-refract(-toward_sun,up,1.0/1.333);
    const float cosine=dot(toward_water_sun,ray);
    const float g=.7;
    const float inverse_root=rsqrt(1+g*g-2*g*clamp(cosine,-1,1));
    const float phase=(1-g*g)*(.25/3.141592653589793)*inverse_root*inverse_root*inverse_root;
    const float horizon=smoothstep(.12,.24,toward_sun.y);
    const float transmission=1-water_fresnel(toward_sun.y,true);
    // Use the surface comparison footprint regardless of the emitter's
    // contact-hardening mode. This bounds the work of marching the volume.
    sun.cone.z=0.0;
    const float3 irradiance=sun.color_intensity.rgb*(sun.color_intensity.w*
        water_sun_shafts.x*phase*horizon*transmission);
    static const uint steps=12u;
    const float step=min(span.y-span.x,12.0)/float(steps);
    const float3 segment_transmission=exp(-water.extinction_per_m*step);
    float3 eye_transmission=1;
    float3 scattering=0;
    [loop] for (uint i=0;i<steps;++i) {
        const float3 sample_pos=eye+(span.x+(float(i)+.5)*step)*ray;
        float2 levels;
        if (!water_sun_column(water,sample_pos.xz,levels) || sample_pos.y<levels.x || sample_pos.y>=levels.y) continue;
        float3 surface;
        const float light_distance=water_sun_distance(water,sample_pos,levels.y,toward_water_sun,surface);
        [branch] if (light_distance>0) {
            const float3 world_surface=camera.xyz+(surface-water.camera_cell_size.xyz);
            const float visibility=light_visibility(sun,world_surface,up,toward_sun);
            scattering+=eye_transmission*(1-segment_transmission)*visibility*
                exp(-water.extinction_per_m*light_distance);
        }
        eye_transmission*=segment_transmission;
    }
    return scattering*irradiance;
}
