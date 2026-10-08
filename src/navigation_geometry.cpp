// SPDX-License-Identifier: Apache-2.0
#include "navigation_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace poima::navigation {
namespace {
using Json=nlohmann::json;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::string digest(const std::string& value){return sha256(std::as_bytes(std::span(value.data(),value.size())));}
void rigid(const Matrix4& matrix) {
    auto rotation=matrix;
    for(std::size_t column=0;column<3;++column) {
        double squared=0;for(std::size_t row=0;row<3;++row)squared+=matrix[column*4+row]*matrix[column*4+row];
        const auto scale=std::sqrt(squared);check(std::isfinite(scale) && scale>0,"Navigation collider scale is invalid.");
        for(std::size_t row=0;row<3;++row)rotation[column*4+row]/=scale;
    }
    check(rigid_transform(rotation),"Navigation cannot bake a sheared collider.");
}
Point transform(const Matrix4& matrix,Point p) {
    Point result;
    for(std::size_t row=0;row<3;++row) {double value=matrix[12+row];for(std::size_t col=0;col<3;++col)value+=matrix[col*4+row]*p[col];check(std::isfinite(value) && std::abs(value)<=1000000,"Navigation collider world coordinate exceeds bounds.");result[row]=static_cast<float>(value);}return result;
}
}
Source collision_source(const Json& entities,const std::map<std::string,Matrix4>& matrices) {
    check(entities.is_object() && entities.size()<=10000,"Navigation entity limit exceeded.");Source result;
    for(const auto& [id,entity]:entities.items()) {
        const auto& components=entity.at("components");
        if(components.contains("BoxCollider") && components.at("BoxCollider").at("motion")!="static") {++result.ignored_moving_colliders;continue;}
        const auto kind=components.contains("MeshCollider") ? "MeshCollider" : components.contains("BoxCollider") ? "BoxCollider" : nullptr;
        if(!kind)continue;
        check(!(components.contains("MeshCollider") && components.contains("BoxCollider")) && !components.contains("CharacterController"),"Navigation collider has incompatible collision components.");
        auto ancestor=id;std::set<std::string> seen;
        while(true) {
            check(seen.insert(ancestor).second && entities.contains(ancestor),"Invalid navigation collider hierarchy.");
            const auto& node=entities.at(ancestor);const auto& c=node.at("components");
            check(!c.contains("CharacterController") && (!c.contains("BoxCollider") || c.at("BoxCollider").at("motion")=="static"),"Static navigation collider has a moving ancestor.");
            check(!c.contains("AnimationRig") && !c.contains("RigNode") && !c.contains("SkinnedMesh"),"Static navigation collider has animation-owned ancestry.");
            if(node.at("parent").is_null())break;
            ancestor=node.at("parent").get<std::string>();
        }
        const auto matrix=matrices.at(id);rigid(matrix);
        Json shape=components.at(kind);
        // Friction, restitution, mass and names cannot alter baked topology.
        for(auto key:{"friction","restitution","mass","motion"})shape.erase(key);
        result.colliders.push_back({{"id",id},{"kind",kind},{"matrix",matrix},{"shape",std::move(shape)}});
    }
    return result;
}
std::string source_fingerprint(const Source& source) {
    return digest(Json{{"format","poima.navigation-source"},{"version",1},{"colliders",source.colliders}}.dump());
}
Geometry collision_geometry(const Source& source,const MeshResolver& resolver) {
    Geometry result;
    constexpr int box_faces[]{0,1,5,0,5,4, 2,6,7,2,7,3, 0,4,6,0,6,2, 1,3,7,1,7,5, 0,2,3,0,3,1, 4,5,7,4,7,6};
    for(const auto& collider:source.colliders) {
        const auto matrix=collider.at("matrix").get<Matrix4>();const auto& shape=collider.at("shape");
        const auto base=result.vertices.size();
        if(collider.at("kind")=="BoxCollider") {
            const auto extents=shape.at("half_extents").get<Point>();
            for(int corner=0;corner<8;++corner)result.vertices.push_back(transform(matrix,{(corner&1 ? 1:-1)*extents[0],(corner&2 ? 1:-1)*extents[1],(corner&4 ? 1:-1)*extents[2]}));
            for(int index:box_faces)result.triangles.push_back(static_cast<int>(base)+index);
        } else {
            const auto mesh=resolver(shape.at("asset"),shape.at("primitive"));
            check(bool(mesh) && mesh->influences.empty() && mesh->vertices.size()>=3 && mesh->indices.size()%3==0 && !mesh->indices.empty(),"Navigation requires indexed unweighted MeshCollider geometry.");
            check(mesh->vertices.size()<=300000 && mesh->indices.size()/3<=100000,"Navigation MeshCollider per-body geometry exceeds physics limits.");
            check(base+mesh->vertices.size()<=max_vertices && result.triangles.size()/3+mesh->indices.size()/3<=max_triangles,"Navigation world geometry budget exceeded.");
            for(const auto& v:mesh->vertices)result.vertices.push_back(transform(matrix,v.position));
            for(auto index:mesh->indices) {check(index<mesh->vertices.size(),"Navigation mesh triangle index is invalid.");result.triangles.push_back(static_cast<int>(base+index));}
        }
        check(result.vertices.size()<=max_vertices && result.triangles.size()/3<=max_triangles,"Navigation world geometry budget exceeded.");
    }
    return result;
}
}
