// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"

namespace poima {
RenderReport run_render_smoke(const RenderOptions&) {
    RenderReport report;
    report.available = false;
    report.detail = "This build has no rendering experiment. Configure POIMA_BUILD_RENDER_SMOKE=ON.";
    return report;
}
} // namespace poima
