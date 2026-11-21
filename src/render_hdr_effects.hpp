// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace poima {
struct BloomSettings;
struct PostEffect;
}

namespace poima::hdr_effects {

class ProjectPostPass {
public:
    // The caller owns queued frames and must invalidate downstream users
    // before a target is released. The pipeline can outlive its framebuffer.
    void prepare(nvrhi::IDevice& device, nvrhi::ITexture* source,
                 const std::shared_ptr<const PostEffect>& artifact,
                 const std::function<void(const nvrhi::ITexture*)>& retire_target,
                 const std::function<void()>& drain_frames);
    void record(nvrhi::ICommandList& commands) const;

    void detach_source(const nvrhi::ITexture* source) noexcept;
    void detach_source() noexcept;
    void clear_target() noexcept;

    nvrhi::ITexture* radiance() const noexcept { return target_.Get(); }
    nvrhi::IFramebuffer* framebuffer() const noexcept { return framebuffer_.Get(); }
    nvrhi::IBindingSet* bindings() const noexcept { return bindings_.Get(); }

private:
    std::shared_ptr<const PostEffect> artifact_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    nvrhi::TextureHandle target_;
    nvrhi::FramebufferHandle framebuffer_;
    nvrhi::BindingSetHandle bindings_;
    const nvrhi::ITexture* source_ = nullptr;
};

enum class BloomStage { extract = 0, horizontal = 1, vertical = 2 };

class BloomPass {
public:
    // A changed pair of attachments invalidates the scene output descriptor.
    // The format capability is needed only when creating the cached layout.
    void prepare(nvrhi::IDevice& device, nvrhi::ITexture* source,
                 const std::function<bool()>& supports_linear_filtering,
                 const std::function<void()>& drain_frames,
                 const std::function<void()>& invalidate_output);
    void record(nvrhi::ICommandList& commands, const BloomSettings& settings, BloomStage stage) const;

    void detach_source(const nvrhi::ITexture* source) noexcept;
    void clear_targets() noexcept { targets_.reset(); }

    nvrhi::ITexture* radiance() const noexcept { return targets_->images[0].Get(); }
    nvrhi::ITexture* scratch() const noexcept { return targets_->images[1].Get(); }
    nvrhi::ISampler* sampler() const noexcept { return sampler_.Get(); }
    nvrhi::IFramebuffer* framebuffer(std::size_t index) const noexcept { return targets_->framebuffers[index].Get(); }
    nvrhi::IBindingSet* bindings(std::size_t pass) const noexcept { return targets_->bindings[pass].Get(); }

private:
    struct Targets {
        std::array<nvrhi::TextureHandle, 2> images;
        std::array<nvrhi::FramebufferHandle, 2> framebuffers;
        std::array<nvrhi::BindingSetHandle, 3> bindings;
        const nvrhi::ITexture* source = nullptr;
    };
    nvrhi::GraphicsPipelineHandle pipeline_;
    std::optional<Targets> targets_;
    nvrhi::BindingLayoutHandle layout_;
    nvrhi::ShaderHandle vertex_, pixel_;
    nvrhi::SamplerHandle sampler_;
};
}
