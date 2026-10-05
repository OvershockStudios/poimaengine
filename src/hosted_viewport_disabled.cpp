// SPDX-License-Identifier: Apache-2.0
#include "poima/hosted_viewport.hpp"
#include <stdexcept>
namespace poima {
struct HostedViewport::Impl {};
HostedViewport::HostedViewport(const RenderOptions&,const SceneSnapshot&,void*) {
    throw std::runtime_error("This build has no hosted Vulkan viewport.");
}
HostedViewport::~HostedViewport()=default;
std::array<std::uint32_t,2> HostedViewport::extent() const { return {}; }
void HostedViewport::resize() {}
bool HostedViewport::draw(const SceneSnapshot&,bool) { return false; }
bool HostedViewport::draw_capture(const SceneSnapshot&,const std::string&) { return false; }
bool HostedViewport::draw_frame(const SceneSnapshot&,const std::string*) { return false; }
RenderReport HostedViewport::report() const { RenderReport result;result.available=false;result.detail="This build has no hosted Vulkan viewport.";return result; }
}
