// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay_save.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
using namespace poima;
namespace {
std::size_t allocations=0;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
GameplaySaveCompletion success(std::uint64_t generation) {
    GameplaySaveCompletion result;result.succeeded=true;result.generation=generation;return result;
}
}
void* operator new(std::size_t size) { ++allocations;if(auto p=std::malloc(size ? size : 1))return p;throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
int main() {
    try {
        constexpr GameplaySaveEpoch epoch{0x0123456789abcdefULL,0xfedcba9876543210ULL},replacement{7,9};
        GameplaySaveQueue disabled;
        check(!disabled.configure(true,0),"Invalid epoch enabled capability.");
        check(disabled.enqueue(GameplaySaveKind::save,"quick",{},false,0).rejection==GameplaySaveRejection::disabled,"Unhosted queue accepted work.");
        GameplaySaveQueue queue(epoch);GameplaySaveLedger ledger;
        check(queue.configure(true,8),"Valid configuration rejected.");
        const auto baseline=queue;
        for(const auto slot:{"","../quick","a/b","UPPER","with space","a\\b","a.b"})
            check(queue.enqueue(GameplaySaveKind::save,slot,{},false,10).rejection==GameplaySaveRejection::invalid,"Invalid slot accepted.");
        check(queue.enqueue(GameplaySaveKind::save,std::string(65,'a'),{},false,10).rejection==GameplaySaveRejection::invalid,"Oversized slot accepted.");
        check(queue.enqueue(GameplaySaveKind::save,std::string_view("a\0b",3),{},false,10).rejection==GameplaySaveRejection::invalid,"Embedded NUL accepted.");
        check(queue.enqueue(GameplaySaveKind::save,"quick",{},true,10).rejection==GameplaySaveRejection::invalid,"Save silently acknowledged recovery.");
        check(queue.enqueue(GameplaySaveKind::none,"quick",{},false,10).rejection==GameplaySaveRejection::invalid,"Unknown kind accepted.");
        check(queue.enqueue(GameplaySaveKind::load,"quick",gameplay_save_max_revision+1,false,10).rejection==GameplaySaveRejection::invalid,"Oversized generation accepted.");
        check(queue.enqueue(GameplaySaveKind::load,"quick",{},false,gameplay_save_max_revision+1).rejection==GameplaySaveRejection::invalid,"Oversized tick accepted.");
        const auto request=queue.enqueue(GameplaySaveKind::save,"quick",0,false,10);
        check(request.rejection==GameplaySaveRejection::none && request.ticket.sequence==1,"Failed enqueues consumed a ticket.");
        check(queue.pending()->configuration_generation==8 && queue.pending()->has_expected_generation && queue.pending()->expected_generation==0,"Request guards were not copied.");
        check(queue.query(request.ticket,&ledger).state==GameplaySaveState::queued,"Same-callback query did not report queued.");
        check(!queue.clear(request.ticket) && !queue.commit(9) && !queue.mark_resolving(-1,"early"),"Uncommitted work escaped its batch.");
        check(queue.enqueue(GameplaySaveKind::load,"other",{},false,11).rejection==GameplaySaveRejection::busy,"Outstanding work was replaced.");
        check(!queue.configure(false,8) && !queue.configure(true,9) && queue.configure(true,8),"Pending routing changed.");
        queue=baseline; // Later callback failure rolls back request + sequence.
        check(!queue.pending() && queue.query(request.ticket,&ledger).state==GameplaySaveState::expired,"Rollback retained tentative work.");
        auto retry=queue.enqueue(GameplaySaveKind::save,"quick",0,false,10);
        check(retry.ticket==request.ticket,"Atomic retry did not restore ticket sequence.");
        check(queue.commit(15) && queue.commit(15) && !queue.commit(16),"Commit boundary was not immutable/idempotent.");
        check(queue.query(retry.ticket).request.requested_tick==10 && queue.query(retry.ticket).request.committed_tick==15,"Requested and post-batch ticks conflated.");
        check(queue.mark_resolving(-32070,"Publication outcome unknown"),"Committed uncertainty rejected.");
        const auto unresolved=queue;
        auto resolving=queue.query(retry.ticket,&ledger);
        check(resolving.state==GameplaySaveState::resolving && resolving.completion.error_code==-32070 && ledger.size()==0,"Uncertainty became a terminal result.");
        check(queue.enqueue(GameplaySaveKind::save,"again",{},false,16).rejection==GameplaySaveRejection::busy,"Uncertain work did not block new requests.");
        const auto terminal=success(1);const auto before=allocations;
        check(ledger.complete(*queue.pending(),terminal)==GameplaySavePublication::recorded,"Terminal publication failed.");
        check(ledger.complete(*queue.pending(),terminal)==GameplaySavePublication::replayed && ledger.size()==1,"Exact completion repeated publication.");
        auto changed=terminal;changed.generation=2;
        check(ledger.complete(*queue.pending(),changed)==GameplaySavePublication::conflict,"Changed completion reused identity.");
        check(queue.query(retry.ticket,&ledger).state==GameplaySaveState::succeeded,"Published completion hidden behind pending work.");
        check(queue.clear(retry.ticket) && !queue.clear(retry.ticket),"Matching request was not cleared once.");
        check(allocations==before,"Completion/query path allocated memory.");
        queue=unresolved; // Owner ledger is deliberately outside checkpoints.
        check(queue.query(retry.ticket,&ledger).state==GameplaySaveState::succeeded,"Queue checkpoint incorrectly rolled back the owner ledger.");
        check(queue.clear(retry.ticket),"Checkpoint fixture clear failed.");

        auto load=queue.enqueue(GameplaySaveKind::load,"chapter-1",1,true,20);
        check(queue.commit(22),"Load commit failed.");
        auto restored=success(1);restored.recovered=true;restored.restored_epoch=replacement;restored.restored_tick=4;
        check(ledger.complete(*queue.pending(),restored)==GameplaySavePublication::recorded,"Load completion failed.");
        auto restore=ledger.last_restore(replacement);
        check(restore && restore->initiating_ticket==load.ticket && restore->committed_source_tick==22 && restore->restored_tick==4 && restore->recovered,"Replacement metadata lost source/destination meaning.");
        GameplaySaveQueue next(replacement);check(next.configure(true,8),"Replacement configuration failed.");
        check(next.query(load.ticket,&ledger).state==GameplaySaveState::succeeded,"Replacement lost owner result ledger.");
        check(!ledger.last_restore(epoch),"Restore metadata leaked into another runtime epoch.");
        GameplaySaveLedger restarted;
        check(next.query(load.ticket,&restarted).state==GameplaySaveState::expired,"Restart resurrected a saved pending ticket.");
        check(next.query({epoch,999},&ledger).state==GameplaySaveState::expired,"Unknown ticket stayed pending.");
        check(restarted.set_restore({{},replacement,0,4,1,false}) && restarted.last_restore(replacement)->initiating_ticket==GameplaySaveTicket{},"External restore metadata rejected.");
        check(!restarted.set_restore({{epoch,0},replacement,0,4,1,false}),"Malformed initiating ticket accepted.");

        GameplaySaveQueue failures({3,4});check(failures.configure(true,0),"Failure fixture configuration failed.");
        auto failureTicket=failures.enqueue(GameplaySaveKind::save,"slot",{},false,0).ticket;check(failures.commit(1),"Failure commit failed.");
        GameplaySaveCompletion failure;failure.error_code=-32009;gameplay_save_diagnostic(failure.diagnostic,"Generation conflict");
        check(ledger.complete(*failures.pending(),failure)==GameplaySavePublication::recorded,"Definite failure not published.");
        check(failures.query(failureTicket,&ledger).state==GameplaySaveState::failed && failures.query(failureTicket,&ledger).request.committed_tick==1,"Storage failure implied simulation rollback.");
        auto invalid=success(0);const auto count=ledger.size();
        check(ledger.complete(*failures.pending(),invalid)==GameplaySavePublication::invalid && ledger.size()==count,"Invalid terminal result mutated ledger.");
        invalid=success(1);invalid.diagnostic.fill('x');
        check(ledger.complete(*failures.pending(),invalid)==GameplaySavePublication::invalid,"Unterminated diagnostic accepted.");
        auto badRequest=*failures.pending();badRequest.slot.fill('x');
        check(ledger.complete(badRequest,failure)==GameplaySavePublication::invalid,"Unterminated slot accepted on publication.");

        GameplaySaveLedger ring;GameplaySaveQueue many({5,6});check(many.configure(true,0),"Ring fixture configuration failed.");
        GameplaySaveTicket oldest,newest;
        for(std::uint64_t i=0;i<65;++i) {
            const auto ticket=many.enqueue(GameplaySaveKind::save,"slot",{},false,i).ticket;
            if(i==0)oldest=ticket;
            newest=ticket;
            check(many.commit(i) && ring.complete(*many.pending(),success(i+1))==GameplaySavePublication::recorded && many.clear(ticket),"Bounded ring publication failed.");
        }
        check(ring.size()==64 && many.query(oldest,&ring).state==GameplaySaveState::expired && many.query(newest,&ring).state==GameplaySaveState::succeeded,"Terminal eviction policy incorrect.");
        std::array<char,256> diagnostic;
        check(!gameplay_save_diagnostic(diagnostic,std::string(254,'a')+"\xe2\x82\xac") && diagnostic[254]==0 && diagnostic.back()==0,"UTF-8 diagnostic truncated inside a character.");
        check(gameplay_save_diagnostic(diagnostic,"short") && diagnostic[5]==0 && diagnostic[254]==0,"Diagnostic retained stale bytes.");
        std::cout<<"Gameplay save queue: validation, atomic checkpoint rollback, bounded tickets/results, uncertain publication, replacement metadata and allocation-free completion passed.\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
