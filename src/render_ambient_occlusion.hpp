// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

namespace poima { struct AmbientOcclusionOptions; }

namespace poima::ambient_occlusion {
void validate_options(const AmbientOcclusionOptions& options);

enum class Stage { horizon, filter };

struct Inputs {
    nvrhi::IBuffer* frame;
    nvrhi::ITexture* depth;
    nvrhi::ITexture* material_surface;
    nvrhi::ITexture* material_correspondence;
};

class Pass {
public:
    void configure(const AmbientOcclusionOptions& options) noexcept;
    std::uint64_t allocate_targets(nvrhi::IDevice& device, const nvrhi::TextureDesc& scene_hdr);
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);

    // The renderer retires submitted frames before replacing attachments.
    void clear_bindings() noexcept;
    void clear_targets() noexcept;
    void reset() noexcept;

    void upload(nvrhi::ICommandList& commands, nvrhi::IBuffer* destination) const;
    void dispatch(nvrhi::ICommandList& commands, Stage stage) const;
    const nvrhi::BindingSetDesc& binding_desc(Stage stage) const;

    nvrhi::ITexture* raw() const noexcept { return raw_.Get(); }
    nvrhi::ITexture* filtered() const noexcept { return filtered_.Get(); }
    nvrhi::IBuffer* constants() const noexcept { return constants_.Get(); }

private:
    nvrhi::TextureHandle raw_, filtered_;
    nvrhi::BufferHandle constants_;
    std::array<float, 4> settings_{};
    std::array<nvrhi::ShaderHandle, 2> shaders_;
    std::array<nvrhi::BindingLayoutHandle, 2> layouts_;
    std::array<nvrhi::BindingSetHandle, 2> bindings_;
    std::array<nvrhi::ComputePipelineHandle, 2> pipelines_;
};
}
