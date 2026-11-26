// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/render_submission.hpp"
#include <nlohmann/json.hpp>

namespace poima {
inline nlohmann::json render_submission_json(const RenderSubmission& value) {
    return {{"renderer_id",value.renderer_id},{"submission_id",value.submission_id}};
}
}
