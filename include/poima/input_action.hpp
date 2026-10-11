// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace poima {
inline constexpr std::size_t input_action_limit=32;
inline bool valid_input_action_name(std::string_view name) noexcept {
    if(name.empty() || name.size()>64 || name.front()<'a' || name.front()>'z')return false;
    for(const char c:name)if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-'))return false;
    return true;
}
// Complete per-tick semantic state. Both edges can be true for a quick tap.
// Names are sorted/unique at the native boundary; held repeats through a batch,
// while pressed/released are delivered only on its first fixed tick.
struct RuntimeAction { std::string name;bool held=false,pressed=false,released=false; };
}
