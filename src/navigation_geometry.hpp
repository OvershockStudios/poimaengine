// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/navigation.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <map>
namespace poima::navigation {
struct Source {
    nlohmann::json colliders=nlohmann::json::array();
    std::size_t ignored_moving_colliders=0;
};
using MeshResolver=std::function<std::shared_ptr<const MeshAsset>(const std::string&,std::uint32_t)>;
Source collision_source(const nlohmann::json& entities,const std::map<std::string,Matrix4>& matrices);
std::string source_fingerprint(const Source&);
Geometry collision_geometry(const Source&,const MeshResolver&);
}
