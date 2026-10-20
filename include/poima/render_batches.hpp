// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace poima::render_batches {
struct Key {
    std::uintptr_t vertices=0,indices=0,previous_vertices=0,bindings=0;
    std::uint32_t count=0,pipeline=0;
    friend bool operator==(const Key&,const Key&)=default;
};
struct Candidate {std::uint32_t object_index=0;bool visible=true;Key key;};
struct Batch {
    // Original candidate index, never a compacted visible index or object ID.
    std::uint32_t first=0,count=0;
    friend bool operator==(const Batch&,const Batch&)=default;
};

// Identities are opaque, already validated by the caller. Ordering, culled gaps
// and exact resource/pipeline distinctions are retained even when disabled.
inline std::vector<Batch> plan(std::span<const Candidate> input,bool enabled=true) {
    if(input.size()>std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Render batch candidate count exceeds UINT32_MAX.");
    std::vector<Batch> result;
    for(std::size_t i=0;i<input.size();++i) {
        const auto& current=input[i];
        if(!current.visible)continue;
        if(enabled && i!=0 && input[i-1].visible && input[i-1].key==current.key &&
            std::uint64_t{input[i-1].object_index}+1==current.object_index) {
            // A preceding visible entry always has an existing batch. The
            // input-size guard also bounds this count without uint32 wrap.
            ++result.back().count;
        } else result.push_back({static_cast<std::uint32_t>(i),1});
    }
    return result;
}
} // namespace poima::render_batches
