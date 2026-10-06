// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace poima {
// Session-local identity lineage, separate from reusable native/physics handles.
// Authored IDs stay excluded even after deletion. Allocated IDs are never freed.
// Copy before a batch and restore ONLY with the entire failed batch: tentative
// IDs must not escape publication. Copying shares immutable exclusions.
class RuntimeEntityIds {
public:
    static constexpr std::size_t max_authored=10000;
    struct Cursor {
        PoimaEntityId next{0,1};
        bool exhausted=false;
    };
private:
    std::shared_ptr<const std::vector<PoimaEntityId>> authored_;
    Cursor cursor_;
    static bool zero(PoimaEntityId id) noexcept { return !id.high && !id.low; }
    static bool less(PoimaEntityId a,PoimaEntityId b) noexcept {
        return a.high<b.high || (a.high==b.high && a.low<b.low);
    }
    static bool equal(PoimaEntityId a,PoimaEntityId b) noexcept { return a.high==b.high && a.low==b.low; }
    static bool advance(PoimaEntityId& id) noexcept {
        if(++id.low==0 && ++id.high==0)return false;
        return true;
    }
public:
    explicit RuntimeEntityIds(std::span<const PoimaEntityId> authored):RuntimeEntityIds(authored,Cursor{}) {}
    // The owner restores a trusted saved frontier, after checking live spawned
    // IDs/provenance against it. This constructor alone does not validate a save.
    RuntimeEntityIds(std::span<const PoimaEntityId> authored,Cursor cursor):cursor_(cursor) {
        if(authored.size()>max_authored)throw std::invalid_argument("Runtime authored identity budget exceeded.");
        if(cursor.exhausted!=zero(cursor.next))throw std::invalid_argument("Invalid runtime entity identity frontier.");
        auto ids=std::make_shared<std::vector<PoimaEntityId>>(authored.begin(),authored.end());
        std::sort(ids->begin(),ids->end(),less);
        if((!ids->empty() && zero(ids->front())) || std::adjacent_find(ids->begin(),ids->end(),equal)!=ids->end())
            throw std::invalid_argument("Runtime authored identities must be nonzero and unique.");
        authored_=std::move(ids);
    }
    Cursor cursor() const noexcept { return cursor_; }
    bool authored(PoimaEntityId id) const noexcept { return std::binary_search(authored_->begin(),authored_->end(),id,less); }
    // Allocated in this lineage, not necessarily still alive. Liveness belongs
    // to the runtime's published membership, never to the allocation frontier.
    bool allocated(PoimaEntityId id) const noexcept {
        return !zero(id) && !authored(id) && (cursor_.exhausted || less(id,cursor_.next));
    }
    PoimaEntityId allocate() {
        if(cursor_.exhausted)throw std::overflow_error("Runtime entity identity space exhausted.");
        auto candidate=cursor_.next;
        auto reserved=std::lower_bound(authored_->begin(),authored_->end(),candidate,less);
        while(reserved!=authored_->end() && equal(*reserved,candidate)) {
            if(!advance(candidate))throw std::overflow_error("Runtime entity identity space exhausted by authored exclusions.");
            ++reserved;
        }
        const auto result=candidate;
        const auto remains=advance(candidate);
        cursor_={candidate,!remains};
        return result;
    }
};
}
