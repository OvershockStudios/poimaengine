// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <filesystem>
#include <memory>
#include <span>

namespace poima {
struct MeshVertex { std::array<float,3> position{}, normal{}; std::array<float,2> uv{}; std::array<float,4> tangent{1,0,0,0}; };
struct TextureMip { std::uint32_t width=0,height=0; std::vector<std::uint8_t> rgba; };
struct TextureImage { bool srgb=false; std::vector<TextureMip> mips; };
struct TextureMap {
    std::shared_ptr<const TextureImage> image;
    int wrap_s=10497,wrap_t=10497,min_filter=9987,mag_filter=9729;
};
// Slot order: base color, metallic/roughness, emissive, occlusion, normal.
struct MaterialTextures {
    std::array<TextureMap,5> maps;
    float occlusion_strength=1,normal_scale=1;
};
struct SkinWeight { std::array<std::uint16_t,4> joints{};std::array<float,4> weights{}; };
struct MeshAsset {
    std::vector<SkinWeight> influences; // Empty for static geometry; one per vertex otherwise.
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    PbrMaterial material;
    std::array<TextureMap,5> textures;
    bool has_uv=false;
    float occlusion_strength=1,normal_scale=1;
};
struct ModelNode {
    std::string name;
    int parent=-1,skin=-1;
    std::array<double,3> position{0,0,0}, scale{1,1,1};
    std::array<double,4> rotation{0,0,0,1};
    std::vector<std::uint32_t> primitives;
};
enum class AnimationPath : std::uint32_t { translation=0,rotation=1,scale=2 };
enum class AnimationInterpolation : std::uint32_t { step=0,linear=1,cubic=2 };
struct AnimationChannel {
    std::uint32_t node=0;AnimationPath path=AnimationPath::translation;
    AnimationInterpolation interpolation=AnimationInterpolation::linear;
    std::vector<float> times;
    // xyz or xyzw; cubic stores in-tangent/value/out-tangent per key.
    std::vector<std::array<float,4>> values;
};
struct AnimationClip { std::string name;double duration=0;std::vector<AnimationChannel> channels; };
struct ModelSkin { std::string name;int skeleton=-1;std::vector<std::uint32_t> joints;std::vector<Matrix4> inverse_bind; };
struct ModelAsset {
    std::vector<ModelSkin> skins;
    std::vector<AnimationClip> animations;
    unsigned package_version=3;
    std::vector<std::shared_ptr<const MeshAsset>> primitives;
    std::vector<std::shared_ptr<const TextureImage>> images;
    std::vector<ModelNode> nodes;
    std::vector<std::uint32_t> roots;
    std::vector<std::string> diagnostics;
};
std::shared_ptr<const TextureImage> decode_texture(std::span<const std::byte> bytes,bool srgb);
std::vector<TextureMip> texture_mips(TextureMip base,bool srgb);
bool valid_texture_sampler(const TextureMap& map);
void generate_tangents(MeshAsset& mesh);
// Explicit import policy: repair only invalid frames on collapsed-UV triangles
// without a normal map. Returns repaired triangle-corner count; strict by default.
std::size_t generate_tangents(MeshAsset& mesh, bool allow_unmapped_collapsed_uv_fallback);
bool valid_tangent(const MeshVertex& vertex);
std::string encode_image(const TextureImage& image);
std::shared_ptr<const TextureImage> decode_image(const std::string& bytes);
std::string sha256(std::span<const std::byte> bytes);
std::shared_ptr<const ModelAsset> import_gltf(const std::filesystem::path& source);
std::string encode_model(const ModelAsset& model);
std::shared_ptr<const ModelAsset> decode_model(const std::string& bytes);
}
