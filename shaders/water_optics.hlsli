// SPDX-License-Identifier: Apache-2.0
#include "water_ripple_normal.hlsli"
#include "water_bed.hlsli"
#include "water_swell_variation.hlsli"

StructuredBuffer<WaterColumn> water_columns : register(t7);
TextureCube<float4> water_environment : register(t8);
Texture2D<float2> water_brdf : register(t9);
SamplerState water_sampler : register(s1);

// An axis-aligned segment of the ray. Small directions are parallel at the
// supported view distances. The upper edge belongs to the next interval.
bool clip_water_span(float origin, float direction, float low, float high,
                     inout float entry, inout float exit) {
    if (abs(direction) < 1e-8) return origin >= low && origin < high;
    const float first = (low - origin) / direction;
    const float second = (high - origin) / direction;
    entry = max(entry, min(first, second));
    exit = min(exit, max(first, second));
    return exit > entry;
}

float water_fresnel(float cosine, bool from_air) {
    const float index = 1.333;
    const float ratio = from_air ? 1.0 / index : index;
    const float sin_transmitted_sq = ratio * ratio * (1.0 - cosine * cosine);
    if (sin_transmitted_sq >= 1.0) return 1.0;
    const float transmitted = sqrt(1.0 - sin_transmitted_sq);
    const float s = (ratio * cosine - transmitted) / (ratio * cosine + transmitted);
    const float p = (cosine - ratio * transmitted) / (cosine + ratio * transmitted);
    return 0.5 * (s * s + p * p);
}

float water_interface_footprint(float distance, float ray_y) {
    return 2.0 * distance * max(temporal_right.w / cluster_viewport.z,
        temporal_up.w / cluster_viewport.w) / max(abs(ray_y), 0.03);
}

void projected_water_footprint(float3 ray, float distance, out float2 dx, out float2 dy) {
    // Derivative of the ray's X/Z intersection with the shared datum.
    // Cap the inverse at grazing angles where the linear footprint grows unbounded.
    const float view_depth=distance*dot(ray,temporal_forward.xyz);
    const float inv_y=(ray.y<0 ? -1.0 : 1.0)/max(abs(ray.y),.01);
    const float2 rise=ray.xz*inv_y;
    dx=(temporal_right.xz-rise*temporal_right.y)*(2.0*view_depth*temporal_right.w/cluster_viewport.z);
    dy=(temporal_up.xz-rise*temporal_up.y)*(2.0*view_depth*temporal_up.w/cluster_viewport.w);
}

float2 filtered_offshore_swell(float2 position, float2 dx, float2 dy, out float variance) {
    [branch] if (water_swell_variation.y>0.0) return varied_offshore_swell(position,dx,dy,variance);
    float2 gradient=0;
    variance=0.0;
    [unroll] for (uint i=0;i<4;++i) {
        const float4 wave=water_offshore_swell[i];
        const float footprint=length(float2(dot(wave.xy,dx),dot(wave.xy,dy)));
        const float coverage=1.0-smoothstep(.5,3.14159265,footprint);
        gradient+=wave.xy*(wave.z*coverage*cos(dot(position,wave.xy)-wave.w));
        variance+=.5*dot(wave.xy,wave.xy)*wave.z*wave.z*(1.0-coverage*coverage);
    }
    return gradient;
}

float3 water_interface_normal(WaterConstants water, float2 point_xz, float distance, float3 ray,
                              float ray_y, float2 geometry_gradient, out float variance) {
    const float mode = water.radiance_wave_mode.w;
    const bool shared_swell=water_offshore_swell[0].z>0.0;
    variance=0.0;
    if (mode==0.0 && !shared_swell) return normalize(float3(-geometry_gradient.x,1,-geometry_gradient.y));
    float2 dx,dz;
    float2 swell=geometry_gradient;
    [branch] if (shared_swell) {
        projected_water_footprint(ray,distance,dx,dz);
        // Both sides of the seam use the same spectrum, phase and filter.
        swell=filtered_offshore_swell(point_xz,dx,dz,variance);
    } else {
        const float footprint=water_interface_footprint(distance,ray_y);
        dx=float2(footprint,0);
        dz=float2(0,footprint);
    }
    if (mode==0.0) return normalize(float3(-swell.x,1,-swell.y));
    float ripple_variance;
    if (mode == 1.0) {
        const float3 ripple=water_ripple_normal(water_ripple_shape,water_ripple_motion.x,point_xz,dx,dz,ripple_variance);
        variance+=ripple_variance;
        if (!water_displaced(water)) return ripple;
        const float2 combined=swell-ripple.xz/ripple.y;
        return normalize(float3(-combined.x,1,-combined.y));
    }
    float2 gradient = water_wave_layer_gradient(water_ripple_shape, water_ripple_motion.x, point_xz, dx, dz,ripple_variance);
    variance+=ripple_variance;
    [unroll] for (uint i = 0; i < 3; ++i) {
        if (mode > float(i + 2)) {
            gradient += water_wave_layer_gradient(water_wave_shapes[i], water_ripple_motion[i + 1], point_xz, dx, dz,ripple_variance);
            variance+=ripple_variance;
        }
    }
    gradient+=swell;
    return normalize(float3(-gradient.x, 1.0, -gradient.y));
}

int2 water_grid_dimensions(WaterConstants water) {
    return int2(water.dimensions & 0xffu, (water.dimensions >> 8u) & 0xffu);
}

struct WaterPath {
    float length_m;
    float interface_m;
    float water_before_interface_m;
    float2 interface_gradient;
    bool enters;
    bool offshore_interface;
    bool reservoir_interface;
    float sheet_side;
    float2 sheet_ripple;
};

#include "water_shore_trace.hlsli"

WaterPath trace_water_grid(WaterConstants water,float3 eye,float3 ray,float max_distance,bool find_interface) {
    WaterPath path={0,-1,0,float2(0,0),false,false,false,0,float2(0,0)};
    const float size = water.camera_cell_size.w;
    const int2 dimensions = water_grid_dimensions(water);
    float entry = 0, exit = max_distance;
    if (!clip_water_span(eye.x, ray.x, 0, size * dimensions.x, entry, exit) ||
        !clip_water_span(eye.z, ray.z, 0, size * dimensions.y, entry, exit)) return path;

    const float2 direction = ray.xz;
    const int2 step = int2(direction.x < -1e-8 ? -1 : direction.x > 1e-8 ? 1 : 0,
                           direction.y < -1e-8 ? -1 : direction.y > 1e-8 ? 1 : 0);
    const float2 start = (eye.xz + direction * entry) / size;
    // Exact boundary hits enter the cell on the forward side of the ray.
    const int2 first = int2(step.x < 0 ? ceil(start.x) - 1.0 : floor(start.x),
                            step.y < 0 ? ceil(start.y) - 1.0 : floor(start.y));
    int2 cell = clamp(first, 0, dimensions - 1);
    const float2 edge = size * float2(cell + int2(step.x > 0 ? 1 : 0, step.y > 0 ? 1 : 0));
    const float2 stride = float2(step.x != 0 ? size / abs(direction.x) : 0,
                                 step.y != 0 ? size / abs(direction.y) : 0);
    float2 cross_at = float2(step.x != 0 ? (edge.x - eye.x) / direction.x : exit,
                             step.y != 0 ? (edge.y - eye.z) / direction.y : exit);
    // Each step moves at least one grid axis. A 32 by 32 grid needs at most 63 cells.
    [loop] for (uint i = 0; i < 63; ++i) {
        const float crossing = min(cross_at.x, cross_at.y);
        const float next = max(entry, min(exit, crossing));
        const WaterColumn surface = water_columns[cell.y * dimensions.x + cell.x];
        const float2 column=float2(surface.floor_m,surface.stage_m);
        bool coarse=!has_water_bed_fan(surface);
        bool fit=false;
        [branch] if (!coarse && next > entry) {
            const float2 y=eye.y+ray.y*float2(entry,next);
            const float low=min(y.x,y.y),high=max(y.x,y.y);
            const float4 corners=surface.corners;
            const float highest=max(column.x,max(max(corners.x,corners.y),max(corners.z,corners.w)));
            // Over a wet center all four wedges share one stage. A ray
            // already above the entire bed needs only the flat surface test.
            coarse=!water_displaced(water) && column.y>column.x && low>=highest;
            if (!coarse) {
                const float lowest=min(column.x,min(min(corners.x,corners.y),min(corners.z,corners.w)));
                const float4 stages=surface.stages;
                const float4 swell=surface.swell_corners;
                const float lift=max(surface.swell_center_m,max(max(swell.x,swell.y),max(swell.z,swell.w)));
                const float top=max(max(stages.x,stages.y),max(stages.z,stages.w))+lift;
                fit=top>lowest && low<=top && high>=lowest;
            }
        }
        [branch] if (fit) {
            integrate_fitted_water(surface,eye,ray,eye.xz/size-float2(cell),direction/size,
                size,entry,next,max_distance,find_interface,path);
        } else if (coarse && column.y > column.x && next > entry) {
            float first_water = entry, last_water = next;
            if (clip_water_span(eye.y, ray.y, column.x, column.y, first_water, last_water))
                path.length_m += last_water - first_water;
            if (find_interface && path.interface_m < 0.0 && abs(ray.y) >= 1e-8) {
                const float top = (column.y - eye.y) / ray.y;
                if (top >= entry && top <= next && top < max_distance) {
                    path.interface_m = top;
                    path.enters=ray.y<0.0;
                    path.water_before_interface_m = path.length_m - max(0.0, last_water - max(top, first_water));
                }
            }
        }
        if (next >= exit) break;
        const bool crosses_x = cross_at.x == crossing;
        const bool crosses_z = cross_at.y == crossing;
        if (crosses_x) { cell.x += step.x; cross_at.x += stride.x; }
        if (crosses_z) { cell.y += step.y; cross_at.y += stride.y; }
        if (any(cell < 0) || any(cell >= dimensions)) break;
        entry = next;
    }

    return path;
}

float2 offshore_outward() {
    const uint side=uint(water_offshore.x);
    if (side==1u) return float2(0,-1);
    if (side==2u) return float2(1,0);
    if (side==3u) return float2(0,1);
    return float2(-1,0);
}

bool offshore_contains(float2 position) {
    if (water_offshore.x==0.0) return false;
    const float distance=dot(position,offshore_outward())-water_offshore.w;
    return water_offshore.x==1.0 || water_offshore.x==4.0 ? distance>0.0 : distance>=0.0;
}

WaterPath trace_terrain_water(WaterConstants water,float3 eye,float3 ray,float max_distance,bool find_interface) {
    WaterPath local=trace_water_grid(water,eye,ray,max_distance,find_interface);
    [branch] if (water_offshore.x==0.0) return local;

    const float2 outward=offshore_outward();
    const float origin=dot(outward,eye.xz)-water_offshore.w;
    const float motion=dot(outward,ray.xz);
    float entry=0.0,exit=max_distance;
    const bool owns_edge=water_offshore.x==2.0 || water_offshore.x==3.0;
    if (!clip_above_water_plane(origin,motion,owns_edge,entry,exit)) return local;
    float underwater_entry=entry,underwater_exit=exit;
    const bool wet=clip_water_span(eye.y,ray.y,water_offshore.z,water_offshore.y,
        underwater_entry,underwater_exit);
    const float sea_length=wet ? underwater_exit-underwater_entry : 0.0;
    if (find_interface && abs(ray.y)>=1e-8) {
        const float surface=(water_offshore.y-eye.y)/ray.y;
        if (surface>=entry && surface<=exit && surface<max_distance &&
            (local.interface_m<0.0 || surface<local.interface_m)) {
            const float sea_before=wet ? max(0.0,min(surface,underwater_exit)-underwater_entry) : 0.0;
            local.water_before_interface_m=sea_before+(motion>0.0 ? local.length_m : 0.0);
            local.interface_m=surface;
            local.interface_gradient=0.0;
            local.enters=ray.y<0.0;
            local.offshore_interface=true;
        }
    }
    if (local.interface_m>=0.0 && !local.offshore_interface && motion<0.0)
        local.water_before_interface_m+=sea_length;
    local.length_m+=sea_length;
    return local;
}

#include "water_reservoirs.hlsli"
#include "water_sheets.hlsli"
#ifndef POIMA_WATER_STRAIGHT_SURFACE
#include "water_refraction.hlsli"
#endif
#include "water_shore_foam.hlsli"

float3 compose_water_path(WaterConstants water,float3 input_radiance,float3 ray,WaterPath path,uint2 extent) {
    const float3 eye = water.camera_cell_size.xyz;
    const float interface_at = path.interface_m;
    if (path.length_m == 0.0 && interface_at < 0.0) return input_radiance;
    const float3 scattering = water.radiance_wave_mode.xyz;
    const float3 transmittance = exp(-water.extinction_per_m * path.length_m);
    float3 color = lerp(scattering, input_radiance, transmittance);
    if (interface_at >= 0.0) {
        const bool from_air = path.enters;
        const float3 geometry_normal=normalize(float3(-path.interface_gradient.x,1,-path.interface_gradient.y));
        const float relative_y=water_displaced(water) ? dot(geometry_normal,ray) : ray.y;
        float slope_variance=0;
        float3 normal=geometry_normal;
        if (path.sheet_side!=0)
            normal=normalize(float3(-(path.interface_gradient+path.sheet_ripple).x,1,
                -(path.interface_gradient+path.sheet_ripple).y))*path.sheet_side;
        else if (!path.reservoir_interface)
            normal=water_interface_normal(water,eye.xz+interface_at*ray.xz,interface_at,ray,relative_y,path.interface_gradient,slope_variance);
        if (!from_air) normal = -normal;
        const float nv=saturate(-dot(normal,ray));
        float fresnel=water_fresnel(nv,from_air);
        float reflection_roughness=.08;
        [branch] if (from_air && water_reflection.y>0.0) {
            // Add the slope energy removed by the footprint to the authored
            // GGX width. The existing split-sum table uses perceptual roughness.
            const float alpha=water_reflection.x*water_reflection.x;
            reflection_roughness=sqrt(sqrt(saturate(alpha*alpha+slope_variance)));
            const float f0=(1.333-1.0)/(1.333+1.0);
            const float2 response=water_brdf.SampleLevel(water_sampler,float2(nv,reflection_roughness),0);
            fresnel=saturate(f0*f0*response.x+response.y);
        }
        const float before_m = max(path.water_before_interface_m, 0.0);
#ifndef POIMA_WATER_STRAIGHT_SURFACE
        [branch] if (path.sheet_side==0 && (water.dimensions & (1u << 16u)) != 0u && fresnel < 0.999) {
            const float3 transmitted = refract(ray, normal, from_air ? 1.0 / 1.333 : 1.333);
            // A shading normal may tilt the bent ray beyond the actual flat
            // boundary. Such a ray cannot enter the selected outgoing medium.
            const float transmitted_side=water_displaced(water) ? dot(geometry_normal,transmitted) : transmitted.y;
            if (from_air ? transmitted_side < -1e-5 : transmitted_side > 1e-5) {
                WaterRefractionHit hit;
                const float3 offset = interface_at * ray;
                if (water_refraction_hit(water, offset, transmitted, from_air, extent, hit)) {
                    const WaterPath bent = trace_water_columns(water, eye + offset, transmitted, hit.distance_m, false);
                    const float tolerance = clamp(water.camera_cell_size.w * .03, .015, .06);
                    const float wrong_medium_m = from_air ? hit.distance_m - bent.length_m : bent.length_m;
                    if (wrong_medium_m <= tolerance) {
                        const float3 bent_transmittance = exp(-water.extinction_per_m * (before_m + bent.length_m));
                        const float3 bent_radiance = surface_radiance.Load(int3(hit.pixel, 0)).rgb;
                        color = lerp(color, lerp(scattering, bent_radiance, bent_transmittance), hit.confidence);
                    }
                }
            }
        }
#endif
        float3 reflection = scattering;
        if (from_air) {
            reflection = ambient_exposure.rgb;
            if (ibl_parameters.x > 0.0)
                reflection = water_environment.SampleLevel(water_sampler, reflect(ray, normal), reflection_roughness * ibl_parameters.w).rgb *
                    ibl_parameters.z;
        }
        const float3 before = exp(-water.extinction_per_m * before_m);
        color = lerp(color, lerp(scattering, reflection, before), fresnel);
        [branch] if (from_air && !path.offshore_interface && !path.reservoir_interface && path.sheet_side==0 && water_shore_foam.y > 0.0) {
            const float3 offset = interface_at * ray;
            const float3 entry = eye + offset;
            const float coverage = shore_foam_coverage(water, entry, water_interface_footprint(interface_at, relative_y));
            float seam_fade=1.0;
            if (water_offshore.x!=0.0) {
                const float inward=water_offshore.w-dot(entry.xz,offshore_outward());
                seam_fade=smoothstep(0.0,water.camera_cell_size.w,inward);
            }
            [branch] if (coverage > 0.0)
                color = lerp(color,lerp(scattering,shore_foam_radiance(camera.xyz+offset),before),coverage*seam_fade);
        }
    }
    return clamp(color, -65504.0, 65504.0);
}
