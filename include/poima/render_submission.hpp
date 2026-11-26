// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace poima {
// The queue serial is scoped to a renderer lifetime within this process.
struct RenderSubmission {
    std::uint64_t renderer_id=0;
    std::uint64_t submission_id=0;
    explicit operator bool() const noexcept { return renderer_id!=0 && submission_id!=0; }
    friend bool operator==(const RenderSubmission&,const RenderSubmission&)=default;
};
}
