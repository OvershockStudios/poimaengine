// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace poima {
enum class SceneDebugView : std::uint32_t;
struct OutputGrade;
}

namespace poima::render_scene_output {

struct Inputs {
    nvrhi::ITexture* radiance;
    nvrhi::ITexture* depth;
    nvrhi::ITexture* normal;
    nvrhi::ITexture* motion;
    nvrhi::ITexture* motion_valid;
    nvrhi::ITexture* occlusion;
    nvrhi::ITexture* bloom;
    nvrhi::ISampler* bloom_sampler;
    SceneDebugView view;
    bool reconstructed;
    bool grade;
    nvrhi::IBuffer* exposure_state = nullptr;
};

struct Draw {
    nvrhi::Viewport target;
    nvrhi::Viewport display;
    float exposure;
    float near_plane, far_plane;
    float raster_width, raster_height;
    float bloom_intensity;
    SceneDebugView view;
    bool srgb;
};

class Pass {
public:
    // The frame owner chooses the source and destination and retires frames
    // before removing images used by the pass.
    void prepare(nvrhi::IDevice& device, const Inputs& inputs, const nvrhi::IFramebuffer& destination);
    void record(nvrhi::ICommandList& commands, nvrhi::IFramebuffer& destination,
                const Draw& draw, const OutputGrade* grade, nvrhi::IBuffer* grade_buffer) const;

    void invalidate_bindings() noexcept { bindings_ = nullptr; }
    void detach_source() noexcept;
    void detach_source(const nvrhi::ITexture* source) noexcept;
    void detach_framebuffer() noexcept;
    void release_gpu() noexcept;

    bool uses_bloom() const noexcept { return bloom_bound_; }
    nvrhi::IBuffer* grade_buffer() const noexcept { return grade_bound_ ? grade_buffer_.Get() : nullptr; }
    nvrhi::IBindingSet* bindings() const noexcept { return bindings_.Get(); }

private:
    nvrhi::ShaderHandle vertex_, pixel_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::BindingSetHandle bindings_;
    bool reconstruction_bound_ = false;
    bool bloom_bound_ = false;
    bool grade_bound_ = false;
    nvrhi::BufferHandle grade_buffer_;
    const nvrhi::ITexture* source_ = nullptr;
    const nvrhi::IBuffer* exposure_state_ = nullptr;
    nvrhi::GraphicsPipelineHandle pipeline_;
};

}
