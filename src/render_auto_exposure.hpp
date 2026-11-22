// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

namespace poima { struct AutoExposureSettings; }

namespace poima::render_auto_exposure {

inline constexpr std::uint32_t push_constant_bytes = 48;

class Pass {
public:
    void prepare(nvrhi::IDevice& device, nvrhi::ITexture* source);
    // The rectangle is x, y, width, height in source pixels. The frame owner
    // resets the first accepted dispatch and after its history is invalidated.
    void record(nvrhi::ICommandList& commands, const AutoExposureSettings& settings,
                std::array<std::uint32_t, 4> rect, float seconds, bool reset) const;

    nvrhi::ITexture* source() const noexcept { return source_; }
    nvrhi::IBuffer* state() const noexcept { return state_.Get(); }
    nvrhi::IBindingSet* bindings() const noexcept { return bindings_.Get(); }

    // The frame owner retires consumers before releasing source attachments.
    void detach_source() noexcept;
    void release_gpu() noexcept;

private:
    nvrhi::ITexture* source_ = nullptr;
    nvrhi::BufferHandle state_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ComputePipelineHandle pipeline_;
    nvrhi::BindingSetHandle bindings_;
};

} // namespace poima::render_auto_exposure
