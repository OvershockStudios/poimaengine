// SPDX-License-Identifier: Apache-2.0
#include "runtime_body_ids.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using poima::RuntimeBodyIds;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects_unchanged(RuntimeBodyIds& ids,F action) {
    const auto before=ids;
    bool rejected=false;
    try { action(); }catch(const std::exception&) { rejected=true; }
    check(rejected,"Invalid body ID operation succeeded.");
    check(ids==before,"Failed body ID operation mutated allocator state.");
}
void startup_inventory() {
    RuntimeBodyIds ids;
    const JPH::BodyID zero(0,0),middle(7,42),last(RuntimeBodyIds::capacity-1,255);
    ids.reserve(middle);ids.reserve(zero);ids.reserve(last);
    check(ids.live()==3 && ids.available()==RuntimeBodyIds::capacity-3 && ids.exhausted()==0,
        "Startup counts differ.");
    check(ids.owns(zero) && ids.owns(middle) && ids.owns(last),"Startup IDs were not retained exactly.");
    rejects_unchanged(ids,[&] { ids.reserve(middle); });
    rejects_unchanged(ids,[&] { ids.reserve(JPH::BodyID(7,43)); });
    rejects_unchanged(ids,[&] { ids.reserve(JPH::BodyID()); });
    rejects_unchanged(ids,[&] { ids.reserve(JPH::BodyID(RuntimeBodyIds::capacity,1)); });
    // Construct raw corruption without invoking BodyID's debug assertion.
    static_assert(std::is_trivially_copyable_v<JPH::BodyID> && sizeof(JPH::BodyID)==sizeof(std::uint32_t));
    const auto corrupt=std::bit_cast<JPH::BodyID>(std::uint32_t(JPH::BodyID::cBroadPhaseBit|5));
    check(!ids.owns(corrupt),"Broadphase-bit corruption was owned.");
    rejects_unchanged(ids,[&] { ids.reserve(corrupt); });
    rejects_unchanged(ids,[&] { ids.release(corrupt); });
    ids.seal();const auto sealed=ids;ids.seal();check(ids==sealed,"Seal was not idempotent.");
    rejects_unchanged(ids,[&] { ids.reserve(JPH::BodyID(4,1)); });
    check(ids.allocate()==JPH::BodyID(1,1),"Startup inventory did not leave lowest free slot.");
    ids.release(zero);check(ids.allocate()==JPH::BodyID(0,1),"Reserved generation zero did not advance.");
    ids.release(last);check(ids.exhausted()==1,"Reserved generation255 did not exhaust after release.");
}
void deterministic_reuse() {
    RuntimeBodyIds ids;
    ids.reserve(JPH::BodyID(1,7));ids.reserve(JPH::BodyID(3,1));
    const auto a=ids.allocate(),b=ids.allocate(),c=ids.allocate();
    check(a==JPH::BodyID(0,1) && b==JPH::BodyID(2,1) && c==JPH::BodyID(4,1),"Lowest-free allocation order differs.");
    ids.release(c);ids.release(b);
    check(ids.allocate()==JPH::BodyID(2,2) && ids.allocate()==JPH::BodyID(4,2),"Release order changed allocation order.");
    rejects_unchanged(ids,[&] { ids.release(b); });
    check(!ids.owns(b),"Stale generation remains owned.");
    ids.release(a);rejects_unchanged(ids,[&] { ids.release(a); });
    rejects_unchanged(ids,[&] { ids.reserve(JPH::BodyID(0,1)); });
    rejects_unchanged(ids,[&] { ids.release(JPH::BodyID()); });
}
void checkpoint_rollback() {
    RuntimeBodyIds ids;ids.reserve(JPH::BodyID(0,12));ids.seal();
    const auto original=ids.allocate();const auto checkpoint=ids;
    ids.release(original);const auto replacement=ids.allocate();const auto speculative=ids.allocate();
    check(!ids.owns(original) && ids.owns(replacement) && ids.owns(speculative),"Speculative membership differs.");
    // The owning runtime restores actual physical topology before this copy.
    ids=checkpoint;
    check(ids.owns(original) && !ids.owns(replacement) && !ids.owns(speculative),"Value restore did not restore ownership.");
    ids.release(original);
    check(ids.allocate()==replacement && ids.allocate()==speculative,"Retry consumed new generations after rollback.");
    auto control=checkpoint;control.release(original);(void)control.allocate();(void)control.allocate();
    check(ids==control,"Replayed successful allocation differs from control.");
}
void generation_exhaustion() {
    RuntimeBodyIds ids;
    for(unsigned generation=1;generation<=255;++generation) {
        const auto id=ids.allocate();check(id==JPH::BodyID(0,static_cast<JPH::uint8>(generation)),"Generation sequence differs.");
        ids.release(id);
    }
    check(ids.live()==0 && ids.exhausted()==1 && ids.available()==RuntimeBodyIds::capacity-1,"Exhaustion counts differ.");
    check(ids.allocate()==JPH::BodyID(1,1),"Generation wrapped instead of exhausting slot.");
    rejects_unchanged(ids,[&] { ids.release(JPH::BodyID(0,255)); });
    RuntimeBodyIds exhausted;
    for(std::uint32_t i=0;i<RuntimeBodyIds::capacity;++i)exhausted.reserve(JPH::BodyID(i,255));
    exhausted.seal();
    for(std::uint32_t i=0;i<RuntimeBodyIds::capacity;++i)exhausted.release(JPH::BodyID(i,255));
    check(exhausted.exhausted()==RuntimeBodyIds::capacity && exhausted.available()==0,"All-slot exhaustion differs.");
    rejects_unchanged(exhausted,[&] { (void)exhausted.allocate(); });
}
void capacity_and_failure() {
    RuntimeBodyIds ids;
    for(std::uint32_t i=0;i<RuntimeBodyIds::capacity;++i)ids.reserve(JPH::BodyID(i,1));
    check(ids.available()==0,"Full capacity reports available slots.");
    rejects_unchanged(ids,[&] { (void)ids.allocate(); });
    ids.release(JPH::BodyID(211,1));
    check(ids.allocate()==JPH::BodyID(211,2),"Released full-capacity slot was not reusable.");
    rejects_unchanged(ids,[&] { ids.release(JPH::BodyID(211,1)); });
}
}
int main() {
    static_assert(std::is_trivially_copyable_v<RuntimeBodyIds>);
    static_assert(std::is_nothrow_copy_constructible_v<RuntimeBodyIds> && std::is_nothrow_copy_assignable_v<RuntimeBodyIds>);
    try {
        startup_inventory();deterministic_reuse();checkpoint_rollback();generation_exhaustion();capacity_and_failure();
        std::cout<<"Runtime body IDs: 5 groups passed (startup guards, deterministic reuse, checkpoint replay, nonwrapping generations, capacity/atomic failures).\n";
        return 0;
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
