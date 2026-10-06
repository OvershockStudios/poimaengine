// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <optional>
#include <string>
namespace poima {
enum class UiInputKind {pointer_move,pointer_down,pointer_up,pointer_leave,focus_next,focus_previous,accept_down,accept_up,cancel,pointer_wheel};
struct UiInput {UiInputKind kind=UiInputKind::cancel;float x=0,y=0,delta=0;};
struct UiInputResult {
    bool consumed=false;
    std::optional<std::string> activated,focused;
    // Visual generation, including hover/focus; not the authoritative UI revision.
    std::uint64_t presentation_revision=0;
};
}
