// SPDX-License-Identifier: Apache-2.0
#include <FidelityFX/host/backends/vk/ffx_vk.h>
// The SDK Vulkan interface references this even in SR-only builds. This target
// deliberately has no frame-generation swapchain implementation.
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) {
    return FFX_ERROR_INVALID_ARGUMENT;
}
