// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/navigation.hpp"
#include "poima/core.hpp"
#include <nlohmann/json.hpp>
namespace poima::navigation {
inline nlohmann::json schemas() {
    using J=nlohmann::json;
    auto object=[](J properties,J required=J::array()){return J{{"type","object"},{"additionalProperties",false},{"properties",std::move(properties)},{"required",std::move(required)}};};
    auto numeric=[](double lo,double hi){return J{{"type","number"},{"minimum",lo},{"maximum",hi}};};
    auto count=[](unsigned lo,unsigned hi){return J{{"type","integer"},{"minimum",lo},{"maximum",hi}};};
    const J revision={{"type","integer"},{"minimum",0},{"maximum",9007199254740991ULL}},asset={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    auto vector=[&](double lo,double hi){return J{{"type","array"},{"minItems",3},{"maxItems",3},{"items",numeric(lo,hi)}};};
    const J profile=object({{"radius",numeric(.05,2)},{"height",numeric(.1,4)},{"climb",numeric(0,2)},{"slope",numeric(0,60)},{"cell_size",numeric(.025,1)},{"cell_height",numeric(.025,.5)}});
    J methods={{"world.navigation.bake",object({{"revision",revision},{"profile",profile}},{"revision"})},
        {"world.navigation.inspect",object({{"revision",revision},{"asset",asset}},{"revision","asset"})},
        {"world.navigation.path",object({{"revision",revision},{"asset",asset},{"start",vector(-1000000,1000000)},{"end",vector(-1000000,1000000)},{"extents",vector(.01,100)},{"max_polygons",count(1,4096)},{"max_corners",count(2,4096)},{"max_nodes",count(32,4096)}},{"revision","asset","start","end"})}};
    if(poima::runtime_navigation_available()) {
        const J id={{"type","string"},{"pattern","^[0-9a-f]{32}$"}};
        methods["runtime.navigation.path"]=object({{"session_id",id},{"tick",revision},{"structure_revision",revision},{"entity",id},
            {"goal",vector(-1000000,1000000)},{"extents",vector(.01,100)},
            {"max_polygons",count(1,256)},{"max_corners",count(2,256)},{"max_nodes",count(32,4096)}},
            {"session_id","tick","structure_revision","entity","goal"});
        methods["runtime.navigation.path"]["description"]="Read the frozen route from a live CharacterController using current static or kinematic navigation obstacle poses. Guarded by session ID, tick and structure revision. Returns the native corners, sampled obstacle bounds and whether a complete route became blocked.";
    }
    return methods;
}
inline nlohmann::json capability() {
    return {{"available",available()},{"runtime_available",poima::runtime_navigation_available()},{"backend","Recast/Detour 1.6.0"},{"source","current authored static BoxCollider and indexed MeshCollider only"},
        {"scope","single immutable static mesh; optional frozen runtime binding with runtime obstacle filtering"},
        {"runtime_obstacles",{{"available",poima::runtime_navigation_available()},{"component","BoxCollider.navigation_obstacle"},{"max_obstacles",max_obstacles},{"motion",{"static","kinematic"}}}},
        {"publication","content-addressed .pnav; authoring-only synchronous bake"},{"endpoint_lookup","bounded linear scan; no BV tree"},{"determinism","same build/platform; cross-platform byte identity is not promised"},
        {"limits",{{"triangles",max_triangles},{"vertices",max_vertices},{"xz_cells",max_cells},{"height_voxels",1024},{"mesh_polygons",max_mesh_polygons},{"package_bytes",max_package_bytes},{"recast_allocation_bytes",max_bake_memory},{"detour_operation_allocation_bytes",max_detour_memory},{"path_polygons",4096},{"path_corners",4096},{"query_nodes",4096}}}};
}
}
