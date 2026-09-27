// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"

namespace poima {
struct EditorOptions {
    std::string world;
    RenderOptions render;
    std::uint32_t max_frames=0;
    // Optional deterministic UI-action fixture and JSON evidence destination.
    // Uses the same action dispatcher as visible controls, not direct edits.
    std::string script,report;
};
Reply run_editor(const EditorOptions& options);
}
