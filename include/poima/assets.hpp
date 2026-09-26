// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <filesystem>
#include <memory>
#include <span>

namespace poima {
struct MeshVertex { std::array<float,3> position{}, normal{}; std::array<float,2> uv{}; };
struct TextureMip { std::uint32_t width=0,height=0; std::vector<std::uint8_t> rgba; };
struct TextureImage { bool srgb=false; std::vector<TextureMip> mips; };
struct TextureMap {
    std::shared_ptr<const TextureImage> image;
    int wrap_s=10497,wrap_t=10497,min_filter=9987,mag_filter=9729;
};
// Slot order is base color, metallic/roughness, emissive, occlusion.
struct MeshAsset {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    PbrMaterial material;
    std::array<TextureMap,4> textures;
    float occlusion_strength=1;
};
struct ModelNode {
    std::string name;
    int parent=-1;
    std::array<double,3> position{0,0,0}, scale{1,1,1};
    std::array<double,4> rotation{0,0,0,1};
    std::vector<std::uint32_t> primitives;
};
struct ModelAsset {
    unsigned package_version=2;
    std::vector<std::shared_ptr<const MeshAsset>> primitives;
    std::vector<std::shared_ptr<const TextureImage>> images;
    std::vector<ModelNode> nodes;
    std::vector<std::uint32_t> roots;
    std::vector<std::string> diagnostics;
};
std::shared_ptr<const TextureImage> decode_texture(std::span<const std::byte> bytes,bool srgb);
std::vector<TextureMip> texture_mips(TextureMip base,bool srgb);
bool valid_texture_sampler(const TextureMap& map);
std::string sha256(std::span<const std::byte> bytes);
std::shared_ptr<const ModelAsset> import_gltf(const std::filesystem::path& source);
std::string encode_model(const ModelAsset& model);
std::shared_ptr<const ModelAsset> decode_model(const std::string& bytes);
}
