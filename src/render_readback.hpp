// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/core.hpp"
#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace poima::fsr3 { struct DiagnosticResources; }

namespace poima::render_readback {

// The renderer retains these handles and records their copy commands. The
// read functions consume buffers only after the owning submission retires.
nvrhi::StagingTextureHandle allocate_capture(nvrhi::IDevice& device,
    const nvrhi::TextureDesc& source, std::uint32_t width, std::uint32_t height);

void publish_capture(nvrhi::IDevice& device, nvrhi::IStagingTexture* staging,
    std::uint32_t width, std::uint32_t height, bool bgra, bool exclusive,
    const std::string& path);

void allocate_product_probes(nvrhi::IDevice& device, std::uint32_t count,
    bool ambient_occlusion, std::array<nvrhi::StagingTextureHandle, 8>& staging);

void read_product_probes(nvrhi::IDevice& device, std::span<SceneProductSample> samples,
    std::span<const nvrhi::StagingTextureHandle> staging, nvrhi::IBuffer* fsr_history,
    const fsr3::DiagnosticResources& fsr_resources);

} // namespace poima::render_readback
