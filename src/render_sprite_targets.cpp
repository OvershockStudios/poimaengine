// SPDX-License-Identifier: Apache-2.0
#include "render_sprite_targets.hpp"

#include <stdexcept>
#include <utility>

namespace poima::render_sprite_targets {
namespace {

bool is_world_hdr(const nvrhi::TextureDesc& description) {
    return description.dimension == nvrhi::TextureDimension::Texture2D &&
        description.format == nvrhi::Format::RGBA16_FLOAT && description.width && description.height &&
        description.arraySize == 1 && description.mipLevels == 1 && description.sampleCount == 1;
}

bool same_extent(const nvrhi::TextureDesc& left, const nvrhi::TextureDesc& right) {
    return left.width == right.width && left.height == right.height;
}

} // namespace

void Targets::prepare(nvrhi::IDevice& device, nvrhi::ITexture* world) {
    if (!world || !is_world_hdr(world->getDesc()) || world == hdr_.Get())
        throw std::runtime_error("Temporal particle sprites require a distinct single-sample world HDR input.");
    const auto& source = world->getDesc();
    if (hdr_) {
        if (!same_extent(hdr_->getDesc(), source))
            throw std::runtime_error("Temporal particle targets require the current render extent.");
        return;
    }

    nvrhi::TextureDesc description;
    description.width = source.width;
    description.height = source.height;
    description.format = nvrhi::Format::RGBA16_FLOAT;
    description.isRenderTarget = true;
    description.isShaderResource = true;
    description.initialState = nvrhi::ResourceStates::ShaderResource;
    description.keepInitialState = true;
    description.debugName = "Temporal particle sprite radiance";
    auto hdr = device.createTexture(description);
    if (!hdr) throw std::runtime_error("Temporal particle HDR allocation failed.");

    description.format = nvrhi::Format::R8_UNORM;
    description.debugName = "Temporal particle sprite coverage";
    auto coverage = device.createTexture(description);
    if (!coverage) throw std::runtime_error("Temporal particle coverage allocation failed.");

    hdr_ = std::move(hdr);
    coverage_ = std::move(coverage);
}

void Targets::record_inputs(nvrhi::ICommandList& commands, nvrhi::ITexture* world) const {
    if (!hdr_ || !coverage_ || !world || world == hdr_.Get() || !is_world_hdr(world->getDesc()) ||
        !same_extent(hdr_->getDesc(), world->getDesc()))
        throw std::runtime_error("Temporal particle inputs require prepared render-resolution targets.");

    commands.copyTexture(hdr_, {}, world, {});
    commands.clearTextureFloat(coverage_, nvrhi::AllSubresources, nvrhi::Color(0.f));
}

void Targets::clear() noexcept {
    hdr_ = nullptr;
    coverage_ = nullptr;
}

std::uint64_t Targets::bytes() const noexcept {
    if (!hdr_) return 0;
    const auto& description = hdr_->getDesc();
    return std::uint64_t(description.width) * description.height * bytes_per_pixel;
}

} // namespace poima::render_sprite_targets
