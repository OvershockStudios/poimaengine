// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/core.hpp"

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace poima { struct LightingEnvironment; }

namespace poima::render_environment_ibl {

using RecordUpload = std::function<void(nvrhi::ICommandList&)>;
// Record, submit and retire these writes synchronously before returning.
using SubmitUpload = std::function<void(const RecordUpload&)>;

struct Replacement {
    std::function<void()> retire_frames;
    std::function<void()> require_float_sampling;
    SubmitUpload upload;
    std::function<void()> invalidate_bindings;
};

struct Bindings {
    nvrhi::ITexture* diffuse;
    nvrhi::ITexture* specular;
    nvrhi::ITexture* brdf;
    nvrhi::ISampler* sampler;
};

class Resources {
public:
    void prepare_dummy(nvrhi::IDevice& device, const SubmitUpload& upload);
    std::optional<EnvironmentIblDiagnostics> prepare(nvrhi::IDevice& device, const LightingEnvironment& lighting,
                                                    const Replacement& replacement);
    Bindings bound() const noexcept;
    void bind(nvrhi::BindingSetDesc& desc) const;
    std::uint64_t payload_bytes() const noexcept { return active_.bytes; }

private:
    struct Textures {
        nvrhi::TextureHandle diffuse, specular, brdf;
        std::array<float, 10> key{};
        std::uint64_t bytes = 0;
        double prepare_cpu_ms = 0;
    };
    static Textures allocate(nvrhi::IDevice& device);
    Textures active_, dummy_;
    nvrhi::SamplerHandle sampler_;
    std::uint64_t generation_ = 0;
};
}
