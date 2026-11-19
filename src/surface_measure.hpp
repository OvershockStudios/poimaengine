// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/animation.hpp"
#include "poima/runtime_surface.hpp"
#include "pose_frames.hpp"
#include <limits>
#include <optional>
#include <stdexcept>

namespace poima::surface_measure {
using pose_frames::Vector;
using pose_frames::dot;

inline Vector subtract(const Vector& a,const Vector& b) {
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
inline Vector cross(const Vector& a,const Vector& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
inline Vector world_vertex(const SceneObject& drawable,std::uint32_t index) {
    const auto local=posed_mesh_vertex(drawable,index);
    Vector result{};
    for(std::size_t i=0;i<3;++i) {
        result[i]=drawable.world[12+i]+drawable.world[i]*local[0]+drawable.world[4+i]*local[1]+drawable.world[8+i]*local[2];
        if(!std::isfinite(result[i]) || std::abs(result[i])>1e12)
            throw std::runtime_error("Surface vertex exceeds the supported world coordinates.");
    }
    return result;
}
struct Triangle {
    std::array<Vector,3> vertex;
    std::optional<Vector> normal;
};
inline Triangle triangle(const SceneObject& drawable,std::size_t index) {
    const auto& indices=drawable.mesh->indices;
    Triangle result{{world_vertex(drawable,indices.at(3*index)),world_vertex(drawable,indices.at(3*index+1)),
        world_vertex(drawable,indices.at(3*index+2))},std::nullopt};
    const auto ab=subtract(result.vertex[1],result.vertex[0]),ac=subtract(result.vertex[2],result.vertex[0]);
    const auto bc=subtract(result.vertex[2],result.vertex[1]);
    auto n=cross(ab,ac);
    const auto length=std::hypot(n[0],n[1],n[2]);
    const auto scale=std::max({dot(ab,ab),dot(ac,ac),dot(bc,bc)});
    if(length>32*std::numeric_limits<double>::epsilon()*scale) {
        for(auto& component:n)component/=length;
        result.normal=n;
    }
    return result;
}
inline Vector point(const Triangle& triangle,const Vector& weights) {
    Vector result{};
    for(std::size_t i=0;i<3;++i)
        for(std::size_t axis=0;axis<3;++axis)
            result[axis]+=triangle.vertex[i][axis]*weights[i];
    return result;
}
struct Sample {
    Vector position{},weights{};
    double distance_squared=std::numeric_limits<double>::infinity();
};
inline Sample nearest(const Triangle& triangle,const Vector& query) {
    const auto& [a,b,c]=triangle.vertex;
    auto sample=[&](const Vector& weights) {
        const auto position=point(triangle,weights);
        const auto delta=subtract(query,position);
        return Sample{position,weights,dot(delta,delta)};
    };
    if(triangle.normal) {
        // Oriented areas with the unit face normal also project out the
        // normal component of the query without a second world-space point.
        const auto ab=subtract(b,a),ac=subtract(c,a),aq=subtract(query,a);
        const auto& normal=*triangle.normal;
        const auto area=dot(normal,cross(ab,ac));
        const auto v=dot(normal,cross(aq,ac))/area;
        const auto w=dot(normal,cross(ab,aq))/area;
        const auto u=1-v-w;
        if(u>=0 && v>=0 && w>=0)return sample({u,v,w});
    }
    Sample closest;
    for(std::size_t start=0;start<3;++start) {
        const auto end=(start+1)%3;
        const auto edge=subtract(triangle.vertex[end],triangle.vertex[start]);
        const auto length=dot(edge,edge);
        const auto t=length>0?std::clamp(dot(subtract(query,triangle.vertex[start]),edge)/length,0.,1.):0.;
        Vector weights{};weights[start]=1-t;weights[end]=t;
        const auto candidate=sample(weights);
        if(candidate.distance_squared<closest.distance_squared)closest=candidate;
    }
    return closest;
}
inline RuntimeSurfaceMeasurement measure(const RuntimeSurfaceProbe& probe,const SceneObject& source,
    const SceneObject& target,std::size_t& triangle_checks) {
    const auto target_count=target.mesh->indices.size()/3;
    auto selected=probe.target_triangles;
    if(selected.size()>max_surface_selected_triangles)
        throw std::invalid_argument("Select between 1 and 4096 target triangles.");
    std::sort(selected.begin(),selected.end());
    if(!selected.empty() && selected.back()>=target_count)
        throw std::invalid_argument("Triangle index is outside the cooked drawable.");
    if(std::adjacent_find(selected.begin(),selected.end())!=selected.end())
        throw std::invalid_argument("Target triangles must be unique.");
    const auto count=selected.empty()?target_count:selected.size();
    if(count==0 || count>max_surface_triangle_checks-triangle_checks)
        throw std::invalid_argument("Surface measurement exceeds the 32768 target triangle budget.");
    triangle_checks+=count;

    RuntimeSurfaceMeasurement result;
    if(probe.source_weights) {
        if(probe.source_index>=source.mesh->indices.size()/3)
            throw std::invalid_argument("Triangle index is outside the cooked drawable.");
        auto weights=*probe.source_weights;
        double sum=0;
        for(const double weight:weights) {
            if(!std::isfinite(weight) || weight<0 || weight>1)
                throw std::invalid_argument("Barycentric weights must be within 0..1.");
            sum+=weight;
        }
        if(std::abs(sum-1)>1e-8)
            throw std::invalid_argument("Barycentric weights must sum to one within 1e-8.");
        for(auto& weight:weights)weight/=sum;
        const auto face=triangle(source,probe.source_index);
        result.source_point=point(face,weights);
        result.source_normal=face.normal;
    } else {
        if(probe.source_index>=source.mesh->vertices.size())
            throw std::invalid_argument("Vertex index is outside the cooked drawable.");
        result.source_point=world_vertex(source,static_cast<std::uint32_t>(probe.source_index));
    }

    Sample closest;
    for(std::size_t i=0;i<count;++i) {
        const auto index=selected.empty()?i:selected[i];
        const auto face=triangle(target,index);
        const auto candidate=nearest(face,result.source_point);
        // Full and explicit searches visit ascending ordinals.
        if(candidate.distance_squared<closest.distance_squared) {
            closest=candidate;
            result.target_triangle=index;
            result.target_normal=face.normal;
        }
    }
    result.target_point=closest.position;
    result.target_weights=closest.weights;
    result.distance_m=std::sqrt(closest.distance_squared);
    return result;
}
}
