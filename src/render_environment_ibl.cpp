// SPDX-License-Identifier: Apache-2.0
#include "render_environment_ibl.hpp"

#include "poima/environment_ibl.hpp"
#include "poima/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::render_environment_ibl {
namespace {
constexpr std::uint64_t baked_payload_bytes = 581'600;

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
}

Bindings Resources::bound() const noexcept {
    const auto& textures = active_.diffuse ? active_ : dummy_;
    return {textures.diffuse.Get(), textures.specular.Get(), textures.brdf.Get(), sampler_.Get()};
}

void Resources::bind(nvrhi::BindingSetDesc& desc) const {
    const auto value = bound();
    require(value.diffuse && value.specular && value.brdf && value.sampler, "Environment bindings are unavailable.");
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(10, value.diffuse))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(11, value.specular))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(12, value.brdf))
        .addItem(nvrhi::BindingSetItem::Sampler(5, value.sampler));
}

Resources::Textures Resources::allocate(nvrhi::IDevice& device) {
    const auto cube = [&](std::uint32_t size, std::uint32_t mips, const char* name) {
        nvrhi::TextureDesc desc;
        desc.width = desc.height = size;
        desc.arraySize = 6;
        desc.mipLevels = mips;
        desc.dimension = nvrhi::TextureDimension::TextureCube;
        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = name;
        auto image = device.createTexture(desc);
        require(bool(image), "Environment cube allocation failed.");
        return image;
    };
    Textures textures;
    textures.diffuse = cube(ibl_diffuse_size, 1, "Diffuse environment E over pi");
    textures.specular = cube(ibl_specular_size, ibl_specular_levels, "Roughness-prefiltered environment");
    nvrhi::TextureDesc desc;
    desc.width = desc.height = ibl_brdf_size;
    desc.format = nvrhi::Format::RG32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Environment split-sum BRDF";
    textures.brdf = device.createTexture(desc);
    require(bool(textures.brdf), "Environment BRDF allocation failed.");
    textures.bytes = std::uint64_t(ibl_diffuse_size) * ibl_diffuse_size * 6 * 16 +
        std::uint64_t(ibl_brdf_size) * ibl_brdf_size * 8;
    for (std::uint32_t level = 0, size = ibl_specular_size; level < ibl_specular_levels;
         ++level, size = std::max(1u, size / 2)) {
        textures.bytes += std::uint64_t(size) * size * 6 * 16;
    }
    require(textures.bytes <= 1024 * 1024, "Environment texture payload exceeds one MiB.");
    return textures;
}

void Resources::prepare_dummy(nvrhi::IDevice& device, const SubmitUpload& upload) {
    if (dummy_.diffuse) return;
    // Normalized black textures satisfy the same SRV types on hardware
    // whose float textures cannot be linearly sampled.
    Textures candidate;
    nvrhi::TextureDesc desc;
    desc.width = desc.height = 1;
    desc.arraySize = 6;
    desc.dimension = nvrhi::TextureDimension::TextureCube;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Disabled diffuse environment";
    candidate.diffuse = device.createTexture(desc);
    desc.debugName = "Disabled specular environment";
    candidate.specular = device.createTexture(desc);
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.debugName = "Disabled environment BRDF";
    candidate.brdf = device.createTexture(desc);
    require(candidate.diffuse && candidate.specular && candidate.brdf, "Disabled environment binding allocation failed.");
    candidate.bytes = 52;
    nvrhi::SamplerDesc sampler_desc;
    sampler_desc.addressU = sampler_desc.addressV = sampler_desc.addressW = nvrhi::SamplerAddressMode::Clamp;
    sampler_desc.minFilter = sampler_desc.magFilter = sampler_desc.mipFilter = true;
    auto sampler = device.createSampler(sampler_desc);
    require(bool(sampler), "Environment sampler allocation failed.");
    const std::array<std::uint8_t, 4> black{};
    upload([&](nvrhi::ICommandList& commands) {
        for (unsigned face = 0; face < 6; ++face) {
            commands.writeTexture(candidate.diffuse, face, 0, black.data(), sizeof(black));
            commands.writeTexture(candidate.specular, face, 0, black.data(), sizeof(black));
        }
        commands.writeTexture(candidate.brdf, 0, 0, black.data(), sizeof(black));
    });
    dummy_ = std::move(candidate);
    sampler_ = std::move(sampler);
}

std::optional<EnvironmentIblDiagnostics> Resources::prepare(nvrhi::IDevice& device, const LightingEnvironment& lighting,
                                                           const Replacement& replacement) {
    if (!lighting.ibl || !lighting.ibl->enabled) {
        if (active_.diffuse) {
            replacement.retire_frames();
            replacement.invalidate_bindings();
            active_ = {};
            device.runGarbageCollection();
        }
        if (!lighting.ibl) return std::nullopt;
        EnvironmentIblDiagnostics disabled;
        disabled.generation = generation_;
        return disabled;
    }
    require(lighting.sky.enabled, "Enabled environment IBL requires the procedural sky.");
    const auto& sky = lighting.sky;
    const std::array<float, 10> key = {sky.zenith[0], sky.zenith[1], sky.zenith[2], sky.horizon[0], sky.horizon[1],
        sky.horizon[2], sky.ground[0], sky.ground[1], sky.ground[2], sky.horizon_falloff};
    if (!active_.diffuse || active_.key != key) {
        require(generation_ < std::numeric_limits<std::uint64_t>::max(), "Environment generation exhausted.");
        const auto started = std::chrono::steady_clock::now();
        const auto baked = bake_environment_ibl(sky);
        const auto prepare_cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        require(baked.diffuse.size == ibl_diffuse_size &&
            baked.diffuse.texels.size() == std::size_t(ibl_diffuse_size) * ibl_diffuse_size * 6 &&
            baked.brdf.size() == std::size_t(ibl_brdf_size) * ibl_brdf_size,
            "Environment bake dimensions differ from the fixed profile.");
        // Bake while the last old frame may still complete. Retire every old
        // consumer before staging the replacement textures.
        replacement.retire_frames();
        replacement.require_float_sampling();
        auto candidate = allocate(device);
        require(candidate.bytes == baked_payload_bytes && candidate.bytes + active_.bytes <= 2 * baked_payload_bytes,
            "Environment staged payload exceeds the two-generation bound.");
        replacement.upload([&](nvrhi::ICommandList& commands) {
            const auto write_cube = [&](nvrhi::ITexture* image, const EnvironmentIblLevel& level, unsigned mip) {
                const auto face_pixels = std::size_t(level.size) * level.size;
                require(level.texels.size() == face_pixels * 6, "Malformed environment face payload.");
                for (unsigned face = 0; face < 6; ++face) {
                    commands.writeTexture(image, face, mip, level.texels.data() + std::size_t(face) * face_pixels,
                        std::size_t(level.size) * 16);
                }
            };
            write_cube(candidate.diffuse.Get(), baked.diffuse, 0);
            for (unsigned level = 0; level < ibl_specular_levels; ++level) {
                require(baked.specular[level].size == std::max(1u, ibl_specular_size >> level), "Malformed environment mip size.");
                write_cube(candidate.specular.Get(), baked.specular[level], level);
            }
            commands.writeTexture(candidate.brdf, 0, 0, baked.brdf.data(), std::size_t(ibl_brdf_size) * 8);
        });
        candidate.key = key;
        candidate.prepare_cpu_ms = prepare_cpu_ms;
        replacement.invalidate_bindings();
        active_ = std::move(candidate);
        ++generation_;
        device.runGarbageCollection();
    }
    return EnvironmentIblDiagnostics{true, ibl_diffuse_size, ibl_specular_size, ibl_specular_levels, ibl_brdf_size,
        active_.bytes, generation_, active_.prepare_cpu_ms};
}
}
