// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <array>

namespace poima::render_environment_ibl { class Resources; }
namespace poima::render_schedule { struct Schedule; }

namespace poima::render_deferred_lighting {

struct Inputs {
    nvrhi::IBuffer* frame;
    nvrhi::ITexture* material_base;
    nvrhi::ITexture* material_surface;
    nvrhi::ITexture* material_correspondence;
    nvrhi::ITexture* depth;
    nvrhi::ITexture* shadow;
    nvrhi::IBuffer* lights;
    nvrhi::IBuffer* counts;
    nvrhi::IBuffer* indices;
    nvrhi::ITexture* hdr;
    nvrhi::ITexture* normal;
    nvrhi::ITexture* motion;
    nvrhi::ITexture* motion_valid;
    nvrhi::ITexture* ao;
    nvrhi::ITexture* reflection_roughness;
    nvrhi::ITexture* cloud_shadow = nullptr;
    nvrhi::ISampler* cloud_shadow_sampler = nullptr;
};

class Pass {
public:
    void prepare(nvrhi::IDevice& device, const Inputs& inputs, const render_environment_ibl::Resources& environment,
                 bool products, bool ambient_occlusion);
    void verify_bindings(const render_schedule::Schedule& plan) const;
    void dispatch(nvrhi::ICommandList& commands, const nvrhi::ITexture& hdr) const;

    // The frame owner retires old work before replacing scene attachments.
    void invalidate_bindings() noexcept;
    void release_gpu() noexcept;

private:
    nvrhi::ShaderHandle shader_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    nvrhi::ComputePipelineHandle pipeline_;
    std::array<const void*, 21> binding_key_{};
    bool reflection_variant_ = false;
};

} // namespace poima::render_deferred_lighting
