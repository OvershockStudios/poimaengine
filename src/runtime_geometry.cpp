// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
}
void validate_runtime_mesh_colliders(const RuntimeDefinition& definition) {
    check(definition.entities.size()<=10000,"Runtime entity limit exceeded.");
    std::size_t collision_triangles=0,collision_vertices=0;
    std::map<std::string,const RuntimeEntityDefinition*> entities;
    for(const auto& entity:definition.entities) {
        check(entities.emplace(entity.id,&entity).second,"Duplicate runtime entity ID.");
        if(entity.mesh_collider) {
            const auto& collider=*entity.mesh_collider;
            check(!entity.collider && !entity.character,"MeshCollider cannot combine with BoxCollider or CharacterController.");
            check(bool(collider.mesh),"MeshCollider requires an explicit mesh asset.");
            check(std::isfinite(collider.friction) && collider.friction>=0 && collider.friction<=2 &&
                std::isfinite(collider.restitution) && collider.restitution>=0 && collider.restitution<=1,"Invalid MeshCollider material.");
            const auto& mesh=*collider.mesh;
            check(mesh.influences.empty(),"MeshCollider cannot use weighted geometry.");
            check(mesh.vertices.size()>=3 && mesh.vertices.size()<=300000,"MeshCollider vertex limit is 3..300000.");
            check(!mesh.indices.empty() && mesh.indices.size()%3==0 && mesh.indices.size()/3<=100000,"MeshCollider requires 1..100000 indexed triangles.");
            collision_triangles+=mesh.indices.size()/3;collision_vertices+=mesh.vertices.size();
            check(collision_triangles<=250000 && collision_vertices<=750000,"World MeshCollider budget is 250000 triangles and 750000 vertices.");
            for(const auto& vertex:mesh.vertices)for(float value:vertex.position)
                check(std::isfinite(value) && std::abs(value)<=1e6f,"MeshCollider vertices must be finite and within +/-1000000 source units.");
            std::set<std::array<std::array<float,3>,3>> triangles;
            for(std::size_t i=0;i<mesh.indices.size();i+=3) {
                std::array<std::array<float,3>,3> points;
                for(std::size_t k=0;k<3;++k) {
                    check(mesh.indices[i+k]<mesh.vertices.size(),"MeshCollider triangle index is outside its vertex array.");
                    points[k]=mesh.vertices[mesh.indices[i+k]].position;
                }
                std::array<double,3> u,v,cross;
                for(std::size_t k=0;k<3;++k) { u[k]=double(points[1][k])-points[0][k];v[k]=double(points[2][k])-points[0][k]; }
                cross={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                check(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]>0,"MeshCollider contains a degenerate triangle.");
                std::sort(points.begin(),points.end());
                check(triangles.insert(points).second,"MeshCollider contains duplicate triangles (including reversed winding).");
            }
        }
    }
    // Follow each mesh's complete ancestry, including ordinary intermediate
    // entities. This helper is usable by authoring/export without Jolt.
    for(const auto& entity:definition.entities)if(entity.mesh_collider) {
        std::vector<const RuntimeEntityDefinition*> chain;std::set<std::string> seen;
        auto current=&entity;
        while(current) {
            check(seen.insert(current->id).second,"MeshCollider hierarchy contains a cycle.");
            check(!current->character && (!current->collider || current->collider->motion==BodyMotion::Static),"MeshCollider cannot inherit a moving body/controller.");
            chain.push_back(current);
            if(current->parent.empty())break;
            const auto parent=entities.find(current->parent);check(parent!=entities.end(),"MeshCollider hierarchy parent is absent.");current=parent->second;
        }
        auto world=identity_matrix();
        for(auto it=chain.rbegin();it!=chain.rend();++it) {
            const auto& t=(*it)->transform;
            for(double value:t.scale)check(std::isfinite(value) && value>0,"MeshCollider hierarchy requires positive finite scales.");
            world=multiply(world,local_matrix(t.position,t.rotation,t.scale));
            for(double v:world)check(std::isfinite(v) && std::abs(v)<=1e12,"MeshCollider hierarchy matrix exceeds the supported range.");
        }
        auto rotation=world;std::array<double,3> scale{};
        for(std::size_t col=0;col<3;++col) {
            for(std::size_t row=0;row<3;++row)scale[col]+=world[col*4+row]*world[col*4+row];
            scale[col]=std::sqrt(scale[col]);check(std::isfinite(scale[col]) && scale[col]>0,"MeshCollider transform has invalid scale.");
            for(std::size_t row=0;row<3;++row)rotation[col*4+row]/=scale[col];
        }
        check(rigid_transform(rotation),"MeshCollider cannot represent a sheared hierarchy.");
        for(std::size_t k=12;k<15;++k)check(std::abs(world[k])<=1e6,"MeshCollider position exceeds the current 1000 km bound.");
        const auto& mesh=*entity.mesh_collider->mesh;
        std::vector<std::array<float,3>> scaled_vertices;scaled_vertices.reserve(mesh.vertices.size());
        for(const auto& vertex:mesh.vertices) {
            std::array<float,3> point;
            for(std::size_t k=0;k<3;++k) {
                const double value=vertex.position[k]*scale[k];check(std::isfinite(value) && std::abs(value)<=10000,"Scaled MeshCollider vertices must be within +/-10000 meters.");
                point[k]=static_cast<float>(value);
            }
            scaled_vertices.push_back(point);
        }
        // Check the actual float coordinates passed to Jolt, not just source
        // geometry. Underflow/rounding can collapse valid triangles or merge
        // distinct faces. BVH quantization is still checked by backend cooking.
        std::set<std::array<std::array<float,3>,3>> scaled_triangles;
        for(std::size_t i=0;i<mesh.indices.size();i+=3) {
            std::array<std::array<float,3>,3> points{scaled_vertices[mesh.indices[i]],scaled_vertices[mesh.indices[i+1]],scaled_vertices[mesh.indices[i+2]]};
            std::array<float,3> u,v,cross;
            for(std::size_t k=0;k<3;++k) { u[k]=points[1][k]-points[0][k];v[k]=points[2][k]-points[0][k]; }
            cross={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            const float area_squared=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
            check(std::isfinite(area_squared) && area_squared>1e-12f,"MeshCollider contains a triangle degenerate at scaled float precision.");
            std::sort(points.begin(),points.end());
            check(scaled_triangles.insert(points).second,"MeshCollider triangles become duplicates at scaled float precision.");
        }
    }
}
}
