// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay_save.hpp"
#include <algorithm>
#include <type_traits>

namespace poima {
namespace {
bool bounded(std::uint64_t value) noexcept { return value<=gameplay_save_max_revision; }
bool valid_slot(std::string_view slot) noexcept {
    if(slot.empty() || slot.size()>64)return false;
    for(const unsigned char c:slot)
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))return false;
    return true;
}
bool valid_request(const GameplaySaveRequest& r) noexcept {
    const auto end=std::find(r.slot.begin(),r.slot.end(),'\0');
    return r.ticket.valid() && (r.kind==GameplaySaveKind::save || r.kind==GameplaySaveKind::load) &&
        end!=r.slot.end() && valid_slot({r.slot.data(),static_cast<std::size_t>(end-r.slot.begin())}) &&
        std::all_of(end,r.slot.end(),[](char c){return c==0;}) &&
        bounded(r.configuration_generation) && bounded(r.requested_tick) && bounded(r.committed_tick) &&
        r.committed && r.committed_tick>=r.requested_tick && bounded(r.expected_generation) &&
        (r.has_expected_generation || r.expected_generation==0) && (r.kind==GameplaySaveKind::load || !r.allow_recovery);
}
bool valid_completion(const GameplaySaveRequest& r,const GameplaySaveCompletion& c) noexcept {
    if(!bounded(c.generation) || !bounded(c.restored_tick) || c.diagnostic.back()!=0)return false;
    if(!c.succeeded)return c.error_code!=0 && !c.restored_epoch.valid() && c.restored_tick==0;
    if(c.error_code!=0 || c.generation==0)return false;
    if(r.kind==GameplaySaveKind::save)return !c.restored_epoch.valid() && c.restored_tick==0 && !c.recovered;
    return c.restored_epoch.valid() && c.restored_epoch!=r.ticket.epoch;
}
}
static_assert(std::is_trivially_copyable_v<GameplaySaveQueue>);
static_assert(std::is_nothrow_copy_assignable_v<GameplaySaveQueue>);
static_assert(sizeof(GameplaySaveQueue)<=512);
static_assert(std::is_trivially_copyable_v<GameplaySaveLedger>);

bool gameplay_save_diagnostic(std::array<char,256>& destination,std::string_view text) noexcept {
    auto count=std::min(text.size(),destination.size()-1);
    const bool fits=count==text.size();
    if(!fits) {
        // If the first omitted byte is a continuation, omit its whole sequence.
        while(count>0 && (static_cast<unsigned char>(text[count])&0xc0)==0x80)--count;
    }
    destination.fill(0);
    std::copy_n(text.begin(),count,destination.begin());
    return fits;
}
std::optional<GameplaySaveResult> GameplaySaveLedger::find(GameplaySaveTicket ticket) const noexcept {
    if(!ticket.valid())return std::nullopt;
    for(std::size_t i=0;i<size_;++i)if(results_[i].request.ticket==ticket)return results_[i];
    return std::nullopt;
}
GameplaySavePublication GameplaySaveLedger::complete(const GameplaySaveRequest& request,const GameplaySaveCompletion& completion) noexcept {
    if(!valid_request(request) || !valid_completion(request,completion))return GameplaySavePublication::invalid;
    const GameplaySaveResult candidate{completion.succeeded ? GameplaySaveState::succeeded : GameplaySaveState::failed,request,completion};
    if(const auto previous=find(request.ticket))return *previous==candidate ? GameplaySavePublication::replayed : GameplaySavePublication::conflict;
    results_[next_]=candidate;next_=(next_+1)%capacity;size_=std::min(size_+1,capacity);
    if(completion.succeeded && request.kind==GameplaySaveKind::load)
        restore_=GameplaySaveRestore{request.ticket,completion.restored_epoch,request.committed_tick,completion.restored_tick,completion.generation,completion.recovered};
    return GameplaySavePublication::recorded;
}
bool GameplaySaveLedger::set_restore(const GameplaySaveRestore& restore) noexcept {
    const bool external=restore.initiating_ticket==GameplaySaveTicket{};
    if(!restore.destination_epoch.valid() || (!external && !restore.initiating_ticket.valid()) ||
        (!external && restore.destination_epoch==restore.initiating_ticket.epoch) ||
        !bounded(restore.committed_source_tick) || !bounded(restore.restored_tick) ||
        restore.generation==0 || !bounded(restore.generation))return false;
    restore_=restore;return true;
}
std::optional<GameplaySaveRestore> GameplaySaveLedger::last_restore(GameplaySaveEpoch destination) const noexcept {
    if(restore_ && destination.valid() && restore_->destination_epoch==destination)return restore_;
    return std::nullopt;
}
bool GameplaySaveQueue::configure(bool enabled,std::uint64_t generation) noexcept {
    if(!bounded(generation) || (enabled && !epoch_.valid()))return false;
    if(pending_ && (enabled!=enabled_ || generation!=configuration_generation_))return false;
    enabled_=enabled;configuration_generation_=generation;return true;
}
GameplaySaveEnqueue GameplaySaveQueue::enqueue(GameplaySaveKind kind,std::string_view slot,
    std::optional<std::uint64_t> expected,bool allow_recovery,std::uint64_t tick) noexcept {
    if((kind!=GameplaySaveKind::save && kind!=GameplaySaveKind::load) || !valid_slot(slot) ||
        !bounded(tick) || (expected && !bounded(*expected)) || (kind!=GameplaySaveKind::load && allow_recovery))
        return {GameplaySaveRejection::invalid,{}};
    if(!enabled_ || !epoch_.valid())return {GameplaySaveRejection::disabled,{}};
    if(pending_)return {GameplaySaveRejection::busy,{}};
    if(next_sequence_>gameplay_save_max_revision)return {GameplaySaveRejection::exhausted,{}};
    GameplaySaveRequest request;
    request.ticket={epoch_,next_sequence_};request.kind=kind;
    std::copy(slot.begin(),slot.end(),request.slot.begin());
    request.configuration_generation=configuration_generation_;request.requested_tick=tick;
    request.has_expected_generation=expected.has_value();request.expected_generation=expected.value_or(0);request.allow_recovery=allow_recovery;
    pending_=request;++next_sequence_;resolving_=false;resolving_code_=0;resolving_diagnostic_.fill(0);
    return {GameplaySaveRejection::none,request.ticket};
}
bool GameplaySaveQueue::commit(std::uint64_t tick) noexcept {
    if(!pending_ || !bounded(tick) || tick<pending_->requested_tick)return false;
    if(pending_->committed)return tick==pending_->committed_tick;
    pending_->committed=true;pending_->committed_tick=tick;return true;
}
bool GameplaySaveQueue::mark_resolving(std::int32_t code,std::string_view diagnostic) noexcept {
    if(!pending_ || !pending_->committed || code==0)return false;
    resolving_=true;resolving_code_=code;gameplay_save_diagnostic(resolving_diagnostic_,diagnostic);return true;
}
bool GameplaySaveQueue::clear(GameplaySaveTicket ticket) noexcept {
    if(!pending_ || !pending_->committed || pending_->ticket!=ticket)return false;
    pending_.reset();resolving_=false;resolving_code_=0;resolving_diagnostic_.fill(0);return true;
}
GameplaySaveResult GameplaySaveQueue::query(GameplaySaveTicket ticket,const GameplaySaveLedger* ledger) const noexcept {
    if(ledger)if(auto terminal=ledger->find(ticket))return *terminal;
    if(pending_ && pending_->ticket==ticket) {
        GameplaySaveResult result;result.state=resolving_ ? GameplaySaveState::resolving : GameplaySaveState::queued;result.request=*pending_;
        if(resolving_) { result.completion.error_code=resolving_code_;result.completion.diagnostic=resolving_diagnostic_; }
        return result;
    }
    GameplaySaveResult expired;expired.request.ticket=ticket;return expired;
}
}
