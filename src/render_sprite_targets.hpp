// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace poima::render_sprite_targets {

class Targets {
public:
    static constexpr std::uint64_t bytes_per_pixel = 9;

    // The frame owner releases these attachments after retiring queued work.
    void prepare(nvrhi::IDevice& device, nvrhi::ITexture* world);
    void record_inputs(nvrhi::ICommandList& commands, nvrhi::ITexture* world) const;
    void clear() noexcept;

    nvrhi::ITexture* hdr() const noexcept { return hdr_.Get(); }
    nvrhi::ITexture* coverage() const noexcept { return coverage_.Get(); }
    std::uint64_t bytes() const noexcept;

private:
    nvrhi::TextureHandle hdr_;
    nvrhi::TextureHandle coverage_;
};

} // namespace poima::render_sprite_targets
