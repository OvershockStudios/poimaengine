// SPDX-License-Identifier: Apache-2.0
// Preserve the raster vertex ABI using aligned storage vectors. A structured
// float3 normal at byte 12 would require optional Vulkan scalarBlockLayout.
struct PackedVertex { float4 position_normal_x;float4 normal_yz_uv;float4 tangent; };
struct Vertex { float3 position;float3 normal;float2 uv;float4 tangent; };
Vertex unpack_vertex(PackedVertex packed) {
    Vertex vertex;
    vertex.position=packed.position_normal_x.xyz;
    vertex.normal=float3(packed.position_normal_x.w,packed.normal_yz_uv.xy);
    vertex.uv=packed.normal_yz_uv.zw;vertex.tangent=packed.tangent;
    return vertex;
}
PackedVertex pack_vertex(Vertex vertex) {
    PackedVertex packed;
    packed.position_normal_x=float4(vertex.position,vertex.normal.x);
    packed.normal_yz_uv=float4(vertex.normal.yz,vertex.uv);packed.tangent=vertex.tangent;
    return packed;
}
struct Influence { uint4 joints;float4 weights; };
struct Joint { float4 row0;float4 row1;float4 row2; };
struct Parameters { uint vertices;uint joints;uint object;uint reserved; };
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;
StructuredBuffer<PackedVertex> source_vertices : register(t0);
StructuredBuffer<Influence> influences : register(t1);
StructuredBuffer<Joint> palette : register(t2);
RWStructuredBuffer<PackedVertex> destination : register(u0);
RWByteAddressBuffer errors : register(u1);
void fail(uint index,Vertex source) {
    uint previous;errors.InterlockedCompareExchange(0,0,1,previous);
    if(previous==0) { errors.Store(4,parameters.object);errors.Store(8,index); }
    destination[index]=pack_vertex(source); // Finite diagnostic frame; host rejects publication.
}
[numthreads(64,1,1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    const uint index=id.x;if(index>=parameters.vertices)return;
    Vertex source=unpack_vertex(source_vertices[index]);Influence influence=influences[index];
    if(any(influence.joints>=parameters.joints)) { fail(index,source);return; }
    float4 a=0,b=0,c=0;
    [unroll] for(uint k=0;k<4;++k) {
        Joint joint=palette[influence.joints[k]];float w=influence.weights[k];
        a+=w*joint.row0;b+=w*joint.row1;c+=w*joint.row2;
    }
    float3 cof0=cross(b.xyz,c.xyz),cof1=cross(c.xyz,a.xyz),cof2=cross(a.xyz,b.xyz);
    float determinant=dot(a.xyz,cof0);
    if(!isfinite(determinant) || determinant==0) { fail(index,source);return; }
    Vertex result=source;float4 position=float4(source.position,1);
    result.position=float3(dot(a,position),dot(b,position),dot(c,position));
    float3 normal=float3(dot(cof0,source.normal),dot(cof1,source.normal),dot(cof2,source.normal))/determinant;
    float normal_length=dot(normal,normal);
    if(any(!isfinite(result.position)) || any(abs(result.position)>1e9) || !isfinite(normal_length) || normal_length<=1e-24) { fail(index,source);return; }
    result.normal=normal*rsqrt(normal_length);
    if(source.tangent.w!=0) {
        float3 tangent=float3(dot(a.xyz,source.tangent.xyz),dot(b.xyz,source.tangent.xyz),dot(c.xyz,source.tangent.xyz));
        tangent-=result.normal*dot(tangent,result.normal);float length=dot(tangent,tangent);
        if(!isfinite(length) || length<=1e-24) { fail(index,source);return; }
        result.tangent=float4(tangent*rsqrt(length),source.tangent.w*(determinant<0 ? -1 : 1));
    }
    destination[index]=pack_vertex(result);
}
