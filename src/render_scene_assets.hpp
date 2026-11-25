// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render_scene_draw.hpp"
#include "render_shadow_index_ranges.hpp"

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace poima::render_environment_ibl { class Resources; }

namespace poima::render_scene_assets {

using MaterialKey = std::pair<const MeshAsset*, const MaterialTextures*>;
using MaterialBindings = std::map<MaterialKey, nvrhi::BindingSetHandle>;

struct Upload {
    nvrhi::IDevice& device;
    std::function<void(std::uint64_t)> await_submission;
};

struct MaterialInputs {
    nvrhi::IBindingLayout* layout;
    nvrhi::IBuffer* frame;
    nvrhi::IBuffer* lights;
    nvrhi::IBuffer* counts;
    nvrhi::IBuffer* indices;
    nvrhi::IBuffer* objects;
    nvrhi::ITexture* shadow;
    const render_environment_ibl::Resources& environment;
    nvrhi::ITexture* cloud_shadow = nullptr;
    nvrhi::ISampler* cloud_shadow_sampler = nullptr;
    nvrhi::IBuffer* water_columns = nullptr;
};

struct RetainHooks {
    std::function<void(const std::set<MaterialKey>&)> shadow_materials;
    std::function<void(const std::set<const MeshAsset*>&, const std::set<std::string>&)> skins;
};

struct Statistics {
    std::size_t geometries, materials, images, texture_bytes;
};

class Cache {
public:
    void initialize_defaults();
    void release_gpu() noexcept;
    void invalidate_materials() noexcept;
    bool has_pending_uploads() const noexcept { return pending_copies_ != 0; }
    void flush_uploads(const Upload& upload);

    render_draw::Geometry geometry(const Upload& upload, const std::shared_ptr<const MeshAsset>& mesh,
                                   const nvrhi::BufferHandle& unit_box);
    const render_draw::Geometry& geometry_for(const MeshAsset* mesh) const;
    Bounds bounds(const MeshAsset* mesh);
    const render_shadow_index_ranges::Hierarchy* shadow_ranges(const MeshAsset* mesh);

    nvrhi::TextureHandle upload_texture(const Upload& upload, const std::shared_ptr<const TextureImage>& image);
    const nvrhi::TextureHandle& texture_for(const TextureImage* image) const;
    nvrhi::BindingSetHandle material(const Upload& upload, const MaterialInputs& inputs, const MeshAsset* mesh,
                                     const MaterialTextures* textures, MaterialBindings* foreground = nullptr);

    void retain_scene(const SceneSnapshot& scene, MaterialBindings* foreground, const RetainHooks& hooks);
    Statistics statistics() const noexcept;

private:
    struct MeshUpload {
        std::shared_ptr<const MeshAsset> mesh;
        render_draw::Geometry geometry;
    };
    struct ImageUpload {
        std::shared_ptr<const TextureImage> image;
        nvrhi::TextureHandle texture;
    };
    void make_upload_room(const Upload& upload, std::size_t bytes, std::size_t copies);

    std::map<const MeshAsset*, render_draw::Geometry> geometry_;
    std::map<const MeshAsset*, Bounds> bounds_;
    std::map<const MeshAsset*, render_shadow_index_ranges::Hierarchy> shadow_ranges_;
    std::map<const MeshAsset*, std::shared_ptr<const MeshAsset>> mesh_owners_;
    std::map<const MaterialTextures*, std::shared_ptr<const MaterialTextures>> material_owners_;
    std::map<const TextureImage*, std::shared_ptr<const TextureImage>> image_owners_;
    MaterialBindings materials_;
    std::map<const TextureImage*, nvrhi::TextureHandle> textures_;
    std::map<std::array<int, 4>, nvrhi::SamplerHandle> samplers_;
    std::shared_ptr<const TextureImage> white_image_;
    std::size_t texture_bytes_ = 0;
    // Staging owns both ends of every copy until the batch has completed.
    std::vector<MeshUpload> pending_meshes_;
    std::vector<ImageUpload> pending_images_;
    std::size_t pending_bytes_ = 0, pending_copies_ = 0;
};

} // namespace poima::render_scene_assets
