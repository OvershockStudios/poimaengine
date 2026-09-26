// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"

namespace poima {
GraphicsProbe inspect_vulkan() {
    return {"not_built", "", "Vulkan inspection is disabled in this build.", {}};
}
} // namespace poima
