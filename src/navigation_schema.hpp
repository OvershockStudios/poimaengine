// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/navigation.hpp"
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
    return {{"world.navigation.bake",object({{"revision",revision},{"profile",profile}},{"revision"})},
        {"world.navigation.inspect",object({{"revision",revision},{"asset",asset}},{"revision","asset"})},
        {"world.navigation.path",object({{"revision",revision},{"asset",asset},{"start",vector(-1000000,1000000)},{"end",vector(-1000000,1000000)},{"extents",vector(.01,100)},{"max_polygons",count(1,4096)},{"max_corners",count(2,4096)},{"max_nodes",count(32,4096)}},{"revision","asset","start","end"})}};
}
inline nlohmann::json capability() {
    return {{"available",available()},{"backend","Recast/Detour 1.6.0"},{"source","current authored static BoxCollider and indexed MeshCollider only"},
        {"scope","single immutable static mesh; no runtime, crowd, dynamic obstacles or compiled navigation service"},
        {"publication","content-addressed .pnav; authoring-only synchronous bake"},{"endpoint_lookup","bounded linear scan; no BV tree"},{"determinism","same build/platform; cross-platform byte identity is not promised"},
        {"limits",{{"triangles",max_triangles},{"vertices",max_vertices},{"xz_cells",max_cells},{"height_voxels",1024},{"mesh_polygons",max_mesh_polygons},{"package_bytes",max_package_bytes},{"recast_allocation_bytes",max_bake_memory},{"detour_operation_allocation_bytes",max_detour_memory},{"path_polygons",4096},{"path_corners",4096},{"query_nodes",4096}}}};
}
}
