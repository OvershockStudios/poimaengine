// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <filesystem>
#include <memory>
#include <span>

namespace poima {
struct MeshVertex { std::array<float,3> position{}, normal{}; std::array<float,2> uv{}; };
struct MeshAsset {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    PbrMaterial material;
};
struct ModelNode {
    std::string name;
    int parent=-1;
    std::array<double,3> position{0,0,0}, scale{1,1,1};
    std::array<double,4> rotation{0,0,0,1};
    std::vector<std::uint32_t> primitives;
};
struct ModelAsset {
    std::vector<std::shared_ptr<const MeshAsset>> primitives;
    std::vector<ModelNode> nodes;
    std::vector<std::uint32_t> roots;
    std::vector<std::string> diagnostics;
};
std::string sha256(std::span<const std::byte> bytes);
std::shared_ptr<const ModelAsset> import_gltf(const std::filesystem::path& source);
std::string encode_model(const ModelAsset& model);
std::shared_ptr<const ModelAsset> decode_model(const std::string& bytes);
}
