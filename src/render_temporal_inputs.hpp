// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <array>

namespace poima::render_schedule { struct Schedule; }

namespace poima::render_temporal_inputs {

struct Inputs {
    nvrhi::IBuffer* frame = nullptr;
    nvrhi::ITexture* normal = nullptr;
    nvrhi::ITexture* motion = nullptr;
    nvrhi::ITexture* correspondence = nullptr;
    nvrhi::ITexture* dense_motion = nullptr;
    nvrhi::ITexture* reactive = nullptr;
    nvrhi::ITexture* raw_hdr = nullptr;
    nvrhi::ITexture* composed_hdr = nullptr;

    friend bool operator==(const Inputs&, const Inputs&) = default;
};

class Pass {
public:
    // The frame owner retires queued work before replacing scene attachments.
    void prepare(nvrhi::IDevice& device, const Inputs& inputs);
    void record(nvrhi::ICommandList& commands) const;
    void verify_bindings(const render_schedule::Schedule& schedule) const;

    void invalidate_bindings() noexcept;
    void clear_targets() noexcept;
    void release_gpu() noexcept;
    nvrhi::ITexture* composition_mask() const noexcept { return composition_mask_.Get(); }

private:
    struct Pipeline {
        nvrhi::ShaderHandle shader;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::ComputePipelineHandle pipeline;
    };

    std::array<Pipeline, 2> pipelines_;
    nvrhi::TextureHandle composition_mask_;
    nvrhi::BindingSetHandle bindings_;
    Inputs inputs_;
};

} // namespace poima::render_temporal_inputs
