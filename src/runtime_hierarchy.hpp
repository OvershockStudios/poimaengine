// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace poima::detail {
// Derived acceleration data, paired with one Node's retained world matrix.
// Neither is serialized. Invalidate if the output is independently replaced
// or the parent binding changes. Runtime parents are fixed for a Node lifetime.
class RuntimeHierarchyCache {
    RuntimeTransform evaluated_;
    bool valid_=false;
    static bool same(const RuntimeTransform& a,const RuntimeTransform& b) noexcept {
        return std::memcmp(a.position.data(),b.position.data(),a.position.size()*sizeof(double))==0 &&
            std::memcmp(a.rotation.data(),b.rotation.data(),a.rotation.size()*sizeof(double))==0 &&
            std::memcmp(a.scale.data(),b.scale.data(),a.scale.size()*sizeof(double))==0;
    }
public:
    void invalidate() noexcept { valid_=false; }
    // Parent propagation is conservative: recomputation can yield equal bits.
    bool update(const RuntimeTransform& local,const Matrix4* parent,
        bool parent_recomputed,Matrix4& world) {
        if(valid_ && !parent_recomputed && same(local,evaluated_))return false;
        const auto relative=local_matrix(local.position,local.rotation,local.scale);
        const auto candidate=parent ? multiply(*parent,relative) : relative;
        for(double value:candidate)
            if(!std::isfinite(value) || std::abs(value)>1e12)
                throw std::runtime_error("Runtime hierarchy matrix exceeds the supported range.");
        // Publish the matrix and key together only after all validation succeeds.
        world=candidate;evaluated_=local;valid_=true;return true;
    }
};
}
