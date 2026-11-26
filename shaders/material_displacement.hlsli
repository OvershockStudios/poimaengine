// SPDX-License-Identifier: Apache-2.0
Texture2D<float4> height_image : register(t0, space2);
[[vk::binding(128, 2)]] SamplerState height_sampler : register(s0, space2);

struct DisplacementVertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
    nointerpolation uint object_index : TEXCOORD7;
};
struct DisplacementFactors {
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

DisplacementVertex displacement_vertex_main(VertexInput input, uint instance : SV_InstanceID) {
    DisplacementVertex vertex;
    vertex.position=input.position;
    vertex.normal=input.normal;
    vertex.uv=input.uv;
    vertex.tangent=input.tangent;
    vertex.object_index=draw_indices.object_index+instance;
    return vertex;
}

// Both camera and shadow patches use the unjittered world camera. A shared
// edge computes its rate from its endpoints, independently of either patch.
float displacement_edge_rate(float3 a, float3 b, ObjectData object) {
    float4 p=float4(a,1), q=float4(b,1);
    float3 world_a=float3(dot(object.model_row0,p),dot(object.model_row1,p),dot(object.model_row2,p));
    float3 world_b=float3(dot(object.model_row0,q),dot(object.model_row1,q),dot(object.model_row2,q));
    float4 clip_a=mul(view_projection,float4(world_a,1));
    float4 clip_b=mul(view_projection,float4(world_b,1));
    if(clip_a.w<=0.001 || clip_b.w<=0.001)
        return max(clip_a.w,clip_b.w)<=0.001 ? 2 : 16;
    float2 ndc_a=clamp(clip_a.xy/clip_a.w,-4,4);
    float2 ndc_b=clamp(clip_b.xy/clip_b.w,-4,4);
    return clamp(length((ndc_a-ndc_b)*cluster_viewport.zw*0.5)/18.0,2,16);
}

DisplacementFactors displacement_rates(InputPatch<DisplacementVertex,3> patch) {
    const ObjectData object=objects[patch[0].object_index];
    DisplacementFactors factors;
    factors.edge[0]=displacement_edge_rate(patch[1].position,patch[2].position,object);
    factors.edge[1]=displacement_edge_rate(patch[2].position,patch[0].position,object);
    factors.edge[2]=displacement_edge_rate(patch[0].position,patch[1].position,object);
    factors.inside=max(factors.edge[0],max(factors.edge[1],factors.edge[2]));
    return factors;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("displacement_rates")]
DisplacementVertex displacement_hull_main(InputPatch<DisplacementVertex,3> patch,
                                           uint corner : SV_OutputControlPointID) {
    return patch[corner];
}

bool ordered_endpoint(float3 a, float3 b) {
    if(a.x!=b.x)return a.x<b.x;
    if(a.y!=b.y)return a.y<b.y;
    return a.z<b.z;
}

float3 edge_position(float3 a, float3 b, float wa, float wb) {
    // Evaluate the same operands in the same order on coincident seams.
    precise float3 p;
    if(ordered_endpoint(a,b))p=a*wa+b*wb;
    else p=b*wb+a*wa;
    return p;
}

float3 patch_position(OutputPatch<DisplacementVertex,3> patch, float3 bary) {
    if(bary.x==1)return patch[0].position;
    if(bary.y==1)return patch[1].position;
    if(bary.z==1)return patch[2].position;
    if(bary.x==0)return edge_position(patch[1].position,patch[2].position,bary.y,bary.z);
    if(bary.y==0)return edge_position(patch[2].position,patch[0].position,bary.z,bary.x);
    if(bary.z==0)return edge_position(patch[0].position,patch[1].position,bary.x,bary.y);
    return patch[0].position*bary.x+patch[1].position*bary.y+patch[2].position*bary.z;
}

float2 height_coordinates(float3 position, uint axis) {
    if(axis==0)return position.yz;
    if(axis==1)return position.xz;
    return position.xy;
}
float3 height_gradient(float2 gradient, uint axis) {
    if(axis==0)return float3(0,gradient);
    if(axis==1)return float3(gradient.x,0,gradient.y);
    return float3(gradient,0);
}
float read_height(float2 uv) {
    return height_image.SampleLevel(height_sampler,uv,0).r;
}

VertexInput displacement_surface(OutputPatch<DisplacementVertex,3> patch, float3 bary, bool surface_frame) {
    VertexInput result;
    const uint packed=draw_indices.reserved.y;
    const uint axis=packed>>16;
    const float frequency=1000.0/(packed&65535u);
    const float amplitude=asfloat(draw_indices.reserved.x);
    result.position=patch_position(patch,bary);
    const float2 uv=height_coordinates(result.position,axis)*frequency;
    result.position[axis]+=amplitude*read_height(uv);
    result.uv=patch[0].uv*bary.x+patch[1].uv*bary.y+patch[2].uv*bary.z;
    result.normal=patch[0].normal*bary.x+patch[1].normal*bary.y+patch[2].normal*bary.z;
    result.tangent=patch[0].tangent*bary.x+patch[1].tangent*bary.y+patch[2].tangent*bary.z;
    if(surface_frame) {
        uint w,h;
        height_image.GetDimensions(w,h);
        const float2 dx=float2(.5/w,0), dy=float2(0,.5/h);
        const float2 derivatives=float2((read_height(uv+dx)-read_height(uv-dx))*w,
                                       (read_height(uv+dy)-read_height(uv-dy))*h);
        const float3 gradient=height_gradient(derivatives*(frequency*amplitude),axis);
        result.normal-=gradient*result.normal[axis];
        result.tangent[axis]+=dot(gradient,result.tangent.xyz);
        result.tangent.w=result.tangent.w<0 ? -1 : result.tangent.w>0 ? 1 : 0;
    }
#if POIMA_SCENE_PRODUCTS
    // Rigid meshes share the same local displaced surface across transforms.
    result.previous_position=result.position;
#endif
    return result;
}

[domain("tri")]
VertexOutput displacement_camera_main(DisplacementFactors factors, const OutputPatch<DisplacementVertex,3> patch,
                                      float3 bary : SV_DomainLocation) {
    return scene_vertex(displacement_surface(patch,bary,true),patch[0].object_index);
}

[domain("tri")]
ShadowVertexOutput displacement_shadow_main(DisplacementFactors factors, const OutputPatch<DisplacementVertex,3> patch,
                                            float3 bary : SV_DomainLocation) {
    return shadow_vertex(displacement_surface(patch,bary,false),patch[0].object_index);
}
