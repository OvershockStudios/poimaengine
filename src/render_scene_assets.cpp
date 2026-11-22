// SPDX-License-Identifier: Apache-2.0
#include "render_scene_assets.hpp"

#include "poima/assets.hpp"
#include "poima/profiler.hpp"
#include "render_environment_ibl.hpp"

#include <stdexcept>

namespace poima::render_scene_assets {
namespace {

constexpr std::size_t texture_budget = 256 * 1024 * 1024;
constexpr std::size_t upload_payload_limit = 64 * 1024 * 1024;
constexpr std::size_t upload_copy_limit = 256;

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

std::size_t image_bytes(const TextureImage& image) {
    std::size_t bytes = 0;
    for (const auto& mip : image.mips) bytes += mip.rgba.size();
    return bytes;
}

std::array<int, 4> sampler_key(const TextureMap& map) {
    return {map.wrap_s, map.wrap_t, map.min_filter, map.mag_filter};
}

nvrhi::SamplerAddressMode address_mode(int value) {
    if (value == 33071) return nvrhi::SamplerAddressMode::Clamp;
    if (value == 33648) return nvrhi::SamplerAddressMode::Mirror;
    return nvrhi::SamplerAddressMode::Wrap;
}

} // namespace

void Cache::initialize_defaults() {
    auto white = std::make_shared<TextureImage>();
    white->mips.push_back({1, 1, {255, 255, 255, 255}});
    white_image_ = std::move(white);
}

void Cache::release_gpu() noexcept {
    pending_meshes_.clear();
    pending_images_.clear();
    pending_bytes_ = pending_copies_ = 0;
    geometry_.clear();
    materials_.clear();
    textures_.clear();
    samplers_.clear();
    bounds_.clear();
    shadow_ranges_.clear();
    mesh_owners_.clear();
    material_owners_.clear();
    image_owners_.clear();
    white_image_.reset();
    texture_bytes_ = 0;
}

void Cache::invalidate_materials() noexcept {
    materials_.clear();
}

void Cache::make_upload_room(const Upload& upload, std::size_t bytes, std::size_t copies) {
    if (!has_pending_uploads()) return;
    // A larger asset accepted by the existing loader may occupy a batch alone.
    if (pending_bytes_ >= upload_payload_limit || bytes > upload_payload_limit - pending_bytes_ ||
        pending_copies_ >= upload_copy_limit || copies > upload_copy_limit - pending_copies_) {
        flush_uploads(upload);
    }
}

void Cache::flush_uploads(const Upload& upload) {
    if (!has_pending_uploads()) return;
    profiling::Scope profile("renderer.asset_upload");
    const auto parameters = nvrhi::CommandListParameters().setEnableImmediateExecution(false);
    const auto commands = upload.device.createCommandList(parameters);
    require(bool(commands), "Scene asset upload command list creation failed.");
    const auto finish_recording = [&] {
        // Close restores recorded resources to their declared read states.
        commands->close();
        upload.await_submission(upload.device.executeCommandList(commands));
        upload.device.runGarbageCollection();
    };
    commands->open();
    try {
        for (const auto& [mesh, geometry] : pending_meshes_) {
            commands->writeBuffer(geometry.vertices, mesh->vertices.data(), mesh->vertices.size() * sizeof(MeshVertex));
            commands->writeBuffer(geometry.indices, mesh->indices.data(), mesh->indices.size() * sizeof(std::uint32_t));
        }
        for (const auto& [image, texture] : pending_images_) {
            for (std::uint32_t level = 0; level < image->mips.size(); ++level) {
                const auto& mip = image->mips[level];
                commands->writeTexture(texture, 0, level, mip.rgba.data(), std::size_t(mip.width) * 4);
            }
        }
    } catch (...) {
        // NVRHI retains an open list. Retire any recorded copies when possible
        // before the context faults, keeping the original error.
        try { finish_recording(); } catch (...) {}
        throw;
    }
    finish_recording();
    profiling::counter("renderer.asset_upload_batches", 1);
    profiling::counter("renderer.asset_upload_payload_bytes", pending_bytes_);
    profiling::counter("renderer.asset_upload_meshes", pending_meshes_.size());
    profiling::counter("renderer.asset_upload_textures", pending_images_.size());
    pending_meshes_.clear();
    pending_images_.clear();
    pending_bytes_ = pending_copies_ = 0;
}

render_draw::Geometry Cache::geometry(const Upload& upload, const std::shared_ptr<const MeshAsset>& mesh,
                                       const nvrhi::BufferHandle& unit_box) {
    if (!mesh) return {unit_box, nullptr, 36};
    if (const auto found = geometry_.find(mesh.get()); found != geometry_.end()) return found->second;

    render_draw::Geometry result;
    result.count = static_cast<std::uint32_t>(mesh->indices.size());
    nvrhi::BufferDesc vertex_desc;
    vertex_desc.byteSize = mesh->vertices.size() * sizeof(MeshVertex);
    vertex_desc.isVertexBuffer = true;
    vertex_desc.structStride = mesh->influences.empty() ? 0u : std::uint32_t(sizeof(MeshVertex));
    vertex_desc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertex_desc.keepInitialState = true;
    result.vertices = upload.device.createBuffer(vertex_desc);

    auto index_desc = vertex_desc;
    index_desc.structStride = 0;
    index_desc.byteSize = mesh->indices.size() * sizeof(std::uint32_t);
    index_desc.isVertexBuffer = false;
    index_desc.isIndexBuffer = true;
    index_desc.initialState = nvrhi::ResourceStates::IndexBuffer;
    result.indices = upload.device.createBuffer(index_desc);
    require(result.vertices && result.indices, "Imported geometry buffer creation failed.");

    const auto bytes = vertex_desc.byteSize + index_desc.byteSize;
    make_upload_room(upload, bytes, 2);
    pending_meshes_.push_back({mesh, result});
    pending_bytes_ += bytes;
    pending_copies_ += 2;
    geometry_.emplace(mesh.get(), result);
    return result;
}

const render_draw::Geometry& Cache::geometry_for(const MeshAsset* mesh) const {
    return geometry_.at(mesh);
}

Bounds Cache::bounds(const MeshAsset* mesh) {
    auto [entry, inserted] = bounds_.try_emplace(mesh);
    if (inserted) entry->second = mesh_bounds(mesh);
    return entry->second;
}

const render_shadow_index_ranges::Hierarchy* Cache::shadow_ranges(const MeshAsset* mesh) {
    if (!mesh || mesh->indices.size() < render_shadow_index_ranges::minimum_index_count) return nullptr;
    const auto& hierarchy = shadow_ranges_.try_emplace(mesh, *mesh).first->second;
    return hierarchy.empty() ? nullptr : &hierarchy;
}

nvrhi::TextureHandle Cache::upload_texture(const Upload& upload, const std::shared_ptr<const TextureImage>& image) {
    if (const auto found = textures_.find(image.get()); found != textures_.end()) return found->second;
    const auto bytes = image_bytes(*image);
    require(bytes <= texture_budget - texture_bytes_, "GPU texture data exceeds the initial 256 MiB budget.");

    nvrhi::TextureDesc desc;
    desc.width = image->mips.front().width;
    desc.height = image->mips.front().height;
    desc.mipLevels = static_cast<std::uint32_t>(image->mips.size());
    desc.format = image->srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Cooked material texture";
    auto texture = upload.device.createTexture(desc);
    require(bool(texture), "Texture allocation failed.");

    make_upload_room(upload, bytes, image->mips.size());
    pending_images_.push_back({image, texture});
    pending_bytes_ += bytes;
    pending_copies_ += image->mips.size();
    texture_bytes_ += bytes;
    textures_.emplace(image.get(), texture);
    return texture;
}

const nvrhi::TextureHandle& Cache::texture_for(const TextureImage* image) const {
    return textures_.at(image);
}

nvrhi::BindingSetHandle Cache::material(const Upload& upload, const MaterialInputs& inputs, const MeshAsset* mesh,
                                        const MaterialTextures* textures, MaterialBindings* foreground) {
    auto& cache = foreground ? *foreground : materials_;
    const MaterialKey material_key{mesh, textures};
    if (const auto found = cache.find(material_key); found != cache.end()) return found->second;

    // Preserve positional binding slots as well as shader registers. The
    // masked shadow pass takes its base texture and sampler from this set.
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(render_draw::DrawIndices)))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame));
    for (std::uint32_t slot = 0; slot < 5; ++slot) {
        const TextureMap map = textures ? textures->maps[slot] : mesh ? mesh->textures[slot] : TextureMap{};
        const auto image = upload_texture(upload, map.image ? map.image : white_image_);
        auto& sampler = samplers_[sampler_key(map)];
        if (!sampler) {
            nvrhi::SamplerDesc sampler_desc;
            sampler_desc.addressU = address_mode(map.wrap_s);
            sampler_desc.addressV = address_mode(map.wrap_t);
            sampler_desc.minFilter = map.min_filter == 9729 || map.min_filter == 9985 || map.min_filter == 9987;
            sampler_desc.magFilter = map.mag_filter == 9729;
            sampler_desc.mipFilter = map.min_filter == 9986 || map.min_filter == 9987;
            sampler = upload.device.createSampler(sampler_desc);
            require(bool(sampler), "Texture sampler creation failed.");
        }
        auto levels = nvrhi::AllSubresources;
        if (map.min_filter == 9728 || map.min_filter == 9729) levels.setMipLevels(0, 1);
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, image, nvrhi::Format::UNKNOWN, levels))
            .addItem(nvrhi::BindingSetItem::Sampler(slot, sampler));
    }
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.shadow))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, inputs.counts))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, inputs.indices))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, inputs.objects));
    inputs.environment.bind(desc);
    auto result = upload.device.createBindingSet(desc, inputs.layout);
    require(bool(result), "Material texture bindings failed.");
    cache.emplace(material_key, result);
    return result;
}

void Cache::retain_scene(const SceneSnapshot& scene, MaterialBindings* foreground, const RetainHooks& hooks) {
    std::set<const MeshAsset*> meshes;
    std::set<const MaterialTextures*> materials;
    std::set<const TextureImage*> images;
    std::set<std::string> skin_ids;
    std::set<MaterialKey> combinations{{nullptr, nullptr}};
    std::set<std::array<int, 4>> samplers{sampler_key(TextureMap{})};

    const auto retain_image = [&](const std::shared_ptr<const TextureImage>& image) {
        if (image) {
            images.insert(image.get());
            image_owners_[image.get()] = image;
        }
    };
    retain_image(white_image_);
    const auto retain_objects = [&](const std::vector<SceneObject>& objects) {
        for (const auto& object : objects) {
            if (object.skin) skin_ids.insert(object.entity_id);
            const auto* bound_mesh = object.stream_geometry || object.mutable_geometry ? nullptr : object.mesh.get();
            if (bound_mesh) {
                meshes.insert(bound_mesh);
                mesh_owners_[bound_mesh] = object.mesh;
            }
            if (object.lods) for (const auto& level : *object.lods) if (level.mesh) {
                meshes.insert(level.mesh.get());
                mesh_owners_[level.mesh.get()] = level.mesh;
            }
            if (object.textures) {
                materials.insert(object.textures.get());
                material_owners_[object.textures.get()] = object.textures;
            }
            combinations.emplace(bound_mesh, object.textures.get());
            for (std::size_t slot = 0; slot < 5; ++slot) {
                const auto map = object.textures ? object.textures->maps[slot] : bound_mesh ? bound_mesh->textures[slot] : TextureMap{};
                retain_image(map.image);
                samplers.insert(sampler_key(map));
            }
        }
    };
    retain_objects(scene.objects);
    if (scene.viewmodel) retain_objects(scene.viewmodel->objects);
    for (const auto& style : scene.particle_sprites.styles) retain_image(style.texture);

    // Preserve pointer identities until all caches keyed by them are pruned.
    const auto stale_material = [&](const auto& item) { return !combinations.contains(item.first); };
    std::erase_if(materials_, stale_material);
    hooks.shadow_materials(combinations);
    if (foreground) std::erase_if(*foreground, stale_material);
    std::erase_if(geometry_, [&](const auto& item) { return !meshes.contains(item.first); });
    std::erase_if(bounds_, [&](const auto& item) { return item.first && !meshes.contains(item.first); });
    std::erase_if(shadow_ranges_, [&](const auto& item) { return !meshes.contains(item.first); });
    hooks.skins(meshes, skin_ids);
    std::erase_if(textures_, [&](const auto& item) { return !images.contains(item.first); });
    std::erase_if(samplers_, [&](const auto& item) { return !samplers.contains(item.first); });
    std::erase_if(mesh_owners_, [&](const auto& item) { return !meshes.contains(item.first); });
    std::erase_if(material_owners_, [&](const auto& item) { return !materials.contains(item.first); });
    std::erase_if(image_owners_, [&](const auto& item) { return !images.contains(item.first); });
    texture_bytes_ = 0;
    for (const auto& [image, unused] : textures_) {
        (void)unused;
        texture_bytes_ += image_bytes(*image_owners_.at(image));
    }
}

Statistics Cache::statistics() const noexcept {
    return {geometry_.size(), materials_.size(), textures_.size(), texture_bytes_};
}

} // namespace poima::render_scene_assets
