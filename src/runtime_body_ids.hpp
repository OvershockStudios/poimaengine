// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace poima {
// Runtime-owned Jolt identity allocation, independent of public entity IDs.
// Copy this value before an atomic batch and assign it back on rollback only
// after restoring physical body membership. Speculative IDs must not escape
// that batch. Generation exhaustion retires a slot instead of wrapping.
//
// Inventory every startup body (including Character-owned bodies), then seal.
// All subsequent creation must use these IDs with CreateBodyWithID; mixing in
// Jolt's automatic allocator is unsafe because its counters are not restored.
class RuntimeBodyIds {
    struct Slot {
        std::uint8_t generation=0;
        bool used=false,occupied=false;
        bool operator==(const Slot&) const = default;
    };
public:
    static constexpr std::uint32_t capacity=4096;
    // Every row is a used slot, including unoccupied retired generations and
    // reserved generation zero. An omitted slot is exactly never-used, not a
    // free slot whose generation may be discarded. Wider generation storage
    // admits strict validation before narrowing to Jolt's uint8 sequence.
    struct SnapshotRow {
        std::uint32_t index=0,generation=0;
        bool occupied=false;
        bool operator==(const SnapshotRow&) const = default;
    };
    struct Snapshot {
        bool sealed=false;
        std::vector<SnapshotRow> rows; // Strict ascending index, at most capacity.
        bool operator==(const Snapshot&) const = default;
    };
private:
    std::array<Slot,capacity> slots_{};
    std::uint32_t live_=0,exhausted_=0;
    bool sealed_=false;

    static bool valid(JPH::BodyID id) noexcept {
        const auto raw=id.GetIndexAndSequenceNumber();
        return raw!=JPH::BodyID::cInvalidBodyID &&
            (raw&JPH::BodyID::cBroadPhaseBit)==0 && id.GetIndex()<capacity;
    }
public:
    // A reservation is for an existing startup body, never for re-importing a
    // retired ID. The caller must finish inventory before releasing any body.
    void reserve(JPH::BodyID id) {
        if(sealed_)throw std::logic_error("Runtime body ID startup inventory is sealed.");
        if(!valid(id))throw std::invalid_argument("Invalid startup runtime body ID.");
        auto& slot=slots_[id.GetIndex()];
        if(slot.used)throw std::invalid_argument("Runtime body ID slot was already reserved or used.");
        slot={id.GetSequenceNumber(),true,true};
        ++live_;
    }
    void seal() noexcept { sealed_=true; }
    JPH::BodyID allocate() {
        for(std::uint32_t index=0;index<capacity;++index) {
            auto& slot=slots_[index];
            if(slot.occupied || slot.generation==JPH::BodyID::cMaxSequenceNumber)continue;
            const auto generation=static_cast<std::uint8_t>(slot.generation+1);
            slot={generation,true,true};
            ++live_;sealed_=true;
            return JPH::BodyID(index,generation);
        }
        throw std::overflow_error("Runtime body ID capacity or generations exhausted.");
    }
    // Call only AFTER this exact body's allocation has been destroyed in Jolt.
    // Removing it from broadphase alone is not sufficient to release its slot.
    void release(JPH::BodyID id) {
        if(!owns(id))throw std::invalid_argument("Cannot release an unowned or stale runtime body ID.");
        auto& slot=slots_[id.GetIndex()];
        slot.occupied=false;
        --live_;
        if(slot.generation==JPH::BodyID::cMaxSequenceNumber)++exhausted_;
        sealed_=true;
    }
    bool owns(JPH::BodyID id) const noexcept {
        return valid(id) && slots_[id.GetIndex()].occupied &&
            slots_[id.GetIndex()].generation==id.GetSequenceNumber();
    }
    std::uint32_t live() const noexcept { return live_; }
    std::uint32_t available() const noexcept { return capacity-live_-exhausted_; }
    std::uint32_t exhausted() const noexcept { return exhausted_; }
    Snapshot snapshot() const {
        Snapshot result;result.sealed=sealed_;
        for(std::uint32_t index=0;index<capacity;++index)if(slots_[index].used)
            result.rows.push_back({index,slots_[index].generation,slots_[index].occupied});
        return result;
    }
    // Only reconstructs the owned ledger value. The integration caller must
    // separately join every live row to the actual Jolt body/part inventory;
    // this is not allocation, solver-state restoration or a contact-ID remap.
    static RuntimeBodyIds from_snapshot(const Snapshot& value) {
        if(value.rows.size()>capacity)throw std::invalid_argument("Runtime body ID snapshot exceeds slot capacity.");
        bool first=true;std::uint32_t previous=0;
        for(const auto& row:value.rows) {
            if(row.index>=capacity || (!first && row.index<=previous))
                throw std::invalid_argument("Runtime body ID snapshot rows must be unique, in range and strictly sorted.");
            if(row.generation>JPH::BodyID::cMaxSequenceNumber)
                throw std::invalid_argument("Runtime body ID snapshot generation exceeds the native sequence domain.");
            // reserve() alone may leave startup unsealed, including gen0 or
            // gen255, but any release/allocation necessarily seals the ledger.
            if(!value.sealed && !row.occupied)
                throw std::invalid_argument("Unsealed runtime body ID snapshot cannot contain retired slots.");
            first=false;previous=row.index;
        }
        // Full typed admission precedes installing any row. Counters are derived,
        // never trusted redundant inputs or copied estimates of broadphase state.
        RuntimeBodyIds result;result.sealed_=value.sealed;
        for(const auto& row:value.rows) {
            result.slots_[row.index]={static_cast<std::uint8_t>(row.generation),true,row.occupied};
            if(row.occupied)++result.live_;
            else if(row.generation==JPH::BodyID::cMaxSequenceNumber)++result.exhausted_;
        }
        return result;
    }
    bool operator==(const RuntimeBodyIds&) const = default;
};
}
