// SPDX-License-Identifier: Apache-2.0
#include "poima/frame_performance.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
using namespace poima::frame_performance;
namespace {
std::size_t allocations=0,fail_at=static_cast<std::size_t>(-1);
void check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
template<class Function> void rejects(Function function,const char* message) {
    bool rejected=false;try { function(); }catch(const std::exception&) { rejected=true; }check(rejected,message);
}
void conserved(const Status& value) {
    check(value.gpu_submitted==value.gpu_completed+value.gpu_pending+value.gpu_dropped,"GPU submission conservation failed.");
    check(value.gpu_completed==value.gpu_timed+value.gpu_unavailable+value.gpu_query_failed,"GPU retirement conservation failed.");
}
CpuSample cpu(std::uint64_t sequence) {
    CpuSample sample{};sample.begin_ns=1000+sequence*100;sample.end_ns=sample.begin_ns+90;
    sample.present_return_ns=sample.begin_ns+80;sample.successful_present=true;
    sample.tick_before=12;sample.tick_after=13;sample.width=1280;sample.height=720;
    sample.flags=Flag::focused|Flag::replay|Flag::capture;
    constexpr char session[]="0123456789abcdef0123456789abcdef";
    std::copy_n(session,33,sample.session.begin());
    // Overlapping stages are deliberately not summed/exclusive CPU time.
    sample.work.render_ns=80;sample.work.record_ns=70;sample.work.submit_ns=10;
    sample.work.present_ns=60;sample.work.retire_ns=30;sample.work.gpu_wait_ns=30;return sample;
}
GpuTiming gpu(std::uint64_t total_ns=50) {
    GpuTiming timing{};timing.total_ns=total_ns;timing.opaque_ns=total_ns;
    timing.skinning_ns=total_ns/2;timing.pass_mask=GpuPass::total|GpuPass::opaque|GpuPass::skinning;return timing;
}
}
void* operator new(std::size_t size) {
    ++allocations;if(allocations==fail_at)throw std::bad_alloc();
    if(auto* pointer=std::malloc(size ? size : 1))return pointer;throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { std::free(pointer); }
int main() {
    try {
        static_assert(!std::is_copy_constructible_v<Recorder>);
        static_assert(!std::is_move_constructible_v<Recorder>);
        Recorder capture,other;
        check(capture.rows().empty() && !capture.status().generation && !capture.freeze(),"Fresh recorder was active/freezable.");
        check(!capture.begin_frame(0) && !capture.gpu_submit(FrameToken{}),"Disabled recorder admitted work.");
        rejects([&]{capture.start(0);},"Zero capacity accepted.");
        rejects([&]{capture.start(262145);},"Unbounded capacity accepted.");
        capture.start(4);other.start(1);
        check(capture.status().generation==1 && capture.status().epoch_ns<=Recorder::now_ns(),"Capture epoch/generation invalid.");
        const auto before=allocations;
        const auto first=capture.begin_frame(10),second=capture.begin_frame(12);
        check(first && second && capture.status().sequence_gaps==1 && capture.status().cpu_open==2,"Frame ordering/gaps lost.");
        const auto first_gpu=capture.gpu_submit(first),second_gpu=capture.gpu_submit(second);
        check(first_gpu && second_gpu && !capture.gpu_submit(first),"GPU ticket duplicated or absent.");
        // Retire newer GPU work first, before its CPU record is finished.
        check(capture.gpu_complete(second_gpu,gpu(70)),"GPU retirement before CPU finish rejected.");
        check(capture.finish_frame(first,cpu(10)) && capture.finish_frame(second,cpu(12)),"CPU record failed.");
        check(!capture.finish_frame(first,cpu(10)),"CPU record could be rewritten.");
        check(capture.rows()[1].gpu.total_ns==70 && capture.rows()[0].gpu_state==GpuState::pending,"GPU completion attributed to another frame.");
        capture.stop_admission();check(!capture.begin_frame(13) && !capture.freeze(),"Stop invented GPU completion.");
        check(allocations==before,"Accepted frame/ticket/CPU record paths allocated.");
        rejects([&]{capture.start(1);},"Pending capture discarded on restart.");
        const auto stopped_allocations=allocations;
        check(capture.gpu_complete(first_gpu,gpu(30)),"Stopped admission rejected matching GPU completion.");
        check(!capture.gpu_complete(first_gpu,gpu(1)) && capture.status().gpu_late==1,"Duplicate GPU completion not counted as late.");
        conserved(capture.status());
        check(capture.freeze(),"Drained CPU/GPU capture did not freeze.");
        const auto frozen=capture.status();
        check(!capture.gpu_complete(first_gpu,gpu(1)) && !capture.gpu_drop(first_gpu,GpuDropReason::stopped) &&
              !capture.drop_remaining(GpuDropReason::device_fault) && !capture.gpu_unavailable(first),"Frozen recording accepted mutation.");
        check(capture.status().gpu_late==frozen.gpu_late && capture.rows()[0].gpu.total_ns==30,"Frozen data/counters changed.");
        check(allocations==stopped_allocations,"Accepted retirement/freeze paths allocated.");

        // A stopped CPU frame remains open; finish is permitted, freeze is not.
        const auto other_frame=other.begin_frame(0);other.stop_admission();
        check(!other.freeze(),"Open CPU frame was silently closed.");
        check(other.gpu_unavailable(other_frame) && other.finish_frame(other_frame,cpu(0)) && other.freeze(),"Open frame could not finish after stop.");
        conserved(other.status());
        check(other.status().gpu_not_submitted_unavailable==1 && !other.status().gpu_submitted && !other.status().gpu_unavailable,
              "Non-submitted unavailable row falsified execution counters.");

        // Preallocation failures, including generation allocation, retain capture.
        const auto* old_rows=capture.rows().data();
        for(const std::size_t offset:{1,2}) {
            fail_at=allocations+offset;bool failed=false;
            try { capture.start(2); }catch(const std::bad_alloc&) { failed=true; }
            fail_at=static_cast<std::size_t>(-1);
            check(failed && capture.rows().data()==old_rows && capture.status().generation==1 && capture.status().frozen,
                  "Failed restart replaced a frozen capture.");
        }
        capture.start(2);
        check(!first && !first_gpu && capture.status().generation==2,"Restart retained old recording generation.");
        check(!capture.gpu_complete(first_gpu,gpu()) && !capture.finish_frame(first,cpu(10)) &&
              !capture.gpu_submit(first) && !capture.status().gpu_late,"Old ticket polluted new capture.");
        const auto actual=capture.begin_frame(0);
        check(!other.gpu_submit(actual) && !other.finish_frame(actual,cpu(0)),"Cross-recorder frame accepted.");
        check(!capture.begin_frame(0) && capture.status().frames_rejected==1,"Duplicate frame sequence accepted.");
        const auto next=capture.begin_frame(5);
        check(capture.status().sequence_gaps==4 && !capture.status().full && capture.status().admitting,"Exact capacity silently sealed.");
        check(!capture.begin_frame(6) && capture.status().full && !capture.status().admitting && capture.status().frames_dropped==1,
              "First overflow did not stop admission and count dropped frame.");
        for(unsigned i=0;i<100;++i)check(!capture.begin_frame(7+i),"Full recorder resumed admission.");
        check(capture.status().frames_dropped==1,"Stopped capacity overflow kept inflating dropped count.");
        const auto pending=capture.gpu_submit(actual);
        check(capture.finish_frame(actual,cpu(0)) && capture.finish_frame(next,cpu(5)),"Overflow invalidated admitted CPU work.");
        check(!capture.freeze() && !capture.drop_remaining(GpuDropReason::none),"Never-completed query silently retired.");
        check(capture.drop_remaining(GpuDropReason::device_fault),"Explicit failed-device abandonment failed.");
        check(capture.rows()[0].gpu_state==GpuState::dropped && capture.rows()[0].gpu_drop_reason==GpuDropReason::device_fault &&
              capture.status().gpu_dropped==1 && !capture.status().gpu_completed,"Unknown GPU execution counted completed.");
        check(!capture.gpu_complete(pending,gpu()) && capture.status().gpu_late==1,"Abandoned late query not reported.");
        conserved(capture.status());check(capture.freeze(),"Explicitly dropped capture did not freeze.");

        capture.start(6);
        auto unsupported=capture.begin_frame(0);auto failed=capture.begin_frame(1);auto invalid=capture.begin_frame(2);
        auto unsupported_gpu=capture.gpu_submit(unsupported);auto failed_gpu=capture.gpu_submit(failed);auto invalid_gpu=capture.gpu_submit(invalid);
        check(capture.gpu_drop(unsupported_gpu,GpuDropReason::query_unavailable) &&
              capture.gpu_drop(failed_gpu,GpuDropReason::query_failed),"Known-executed query failures not retired.");
        auto bad_gpu=gpu();bad_gpu.opaque_ns=bad_gpu.total_ns+1;
        check(!capture.gpu_complete(invalid_gpu,bad_gpu),"Invalid query duration accepted.");
        check(capture.status().gpu_completed==3 && !capture.status().gpu_timed && capture.status().gpu_unavailable==1 &&
              capture.status().gpu_query_failed==2 && !capture.status().gpu_dropped,"Query availability confused with execution completion.");
        const auto absent_mask=capture.begin_frame(3);const auto absent_gpu=capture.gpu_submit(absent_mask);
        bad_gpu=gpu();bad_gpu.pass_mask=GpuPass::total;
        check(!capture.gpu_complete(absent_gpu,bad_gpu),"Absent pass mask accepted positive pass duration.");
        const auto zero=capture.begin_frame(4);const auto zero_gpu=capture.gpu_submit(zero);
        check(capture.gpu_complete(zero_gpu,gpu(0)),"Explicit measured zero became unavailable.");
        conserved(capture.status());

        // Invalid CPU fields leave the existing open row recoverable.
        const auto invalid_cpu=capture.begin_frame(5);auto sample=cpu(5);
        sample.end_ns=sample.begin_ns-1;check(!capture.finish_frame(invalid_cpu,sample),"Reversed CPU interval accepted.");
        sample=cpu(5);sample.present_return_ns=sample.end_ns+1;check(!capture.finish_frame(invalid_cpu,sample),"Present stamp outside frame accepted.");
        sample=cpu(5);sample.present_return_ns=0;check(!capture.finish_frame(invalid_cpu,sample),"Successful present without timestamp accepted.");
        sample=cpu(5);sample.flags=1u<<31;check(!capture.finish_frame(invalid_cpu,sample),"Unknown frame flags accepted.");
        sample=cpu(5);sample.work.report_ns=91;check(!capture.finish_frame(invalid_cpu,sample),"Stage longer than frame accepted.");
        sample=cpu(5);sample.session[1]='z';check(!capture.finish_frame(invalid_cpu,sample),"Malformed session identity accepted.");
        sample=cpu(5);sample.session[32]='x';check(!capture.finish_frame(invalid_cpu,sample),"Unterminated session identity accepted.");
        sample=cpu(5);sample.tick_after=1;sample.flags|=Flag::replacement;
        check(capture.finish_frame(invalid_cpu,sample),"Runtime replacement could not retain lower restored tick.");
        for(const auto& frame:{unsupported,failed,invalid,absent_mask,zero})check(capture.finish_frame(frame,cpu(0)),"Admitted CPU finish failed.");

        // Owner enforcement also covers ticket retirement and observation.
        bool rejected=false;
        std::thread thread([&] {
            const auto foreign=capture.begin_frame(99);
            bool start_rejected=false;try { capture.start(1); }catch(const std::logic_error&) { start_rejected=true; }
            rejected=!foreign && !capture.finish_frame(zero,cpu(0)) && !capture.gpu_complete(zero_gpu,gpu()) &&
                !capture.gpu_drop(zero_gpu,GpuDropReason::device_fault) && !capture.gpu_unavailable(zero) &&
                !capture.freeze() && !capture.drop_remaining(GpuDropReason::stopped) &&
                capture.rows().empty() && !capture.status().generation && start_rejected;
            capture.stop_admission();
        });thread.join();
        check(rejected && capture.status().admitting && capture.status().gpu_late==0,"Off-owner access mutated recording.");
        capture.stop_admission();check(capture.freeze(),"Owner could not seal capture after off-owner probes.");

        // Failed measurement closure preserves raw bytes without inventing a
        // valid interval, and must not settle outstanding GPU work implicitly.
        capture.start(1);const auto failed_frame=capture.begin_frame(0);
        const auto failed_frame_gpu=capture.gpu_submit(failed_frame);
        auto failed_sample=cpu(0);failed_sample.end_ns=failed_sample.begin_ns-1;failed_sample.session.fill('x');
        capture.stop_admission();const auto failure_allocations=allocations;
        check(!capture.finish_frame(failed_frame,failed_sample) && capture.fail_frame(failed_frame,failed_sample),
              "Explicit failed CPU closure rejected invalid raw sample.");
        check(!capture.rows()[0].cpu_complete && capture.rows()[0].cpu_failed && capture.status().frames_failed==1 &&
              !capture.status().cpu_open && capture.rows()[0].cpu.end_ns==failed_sample.end_ns &&
              capture.rows()[0].cpu.session==failed_sample.session,"Failed sample was repaired or remained open.");
        check(!capture.freeze() && capture.status().gpu_pending==1,"Failed CPU observation silently retired GPU query.");
        check(!capture.finish_frame(failed_frame,cpu(0)) && !capture.fail_frame(failed_frame,cpu(0)),
              "Failed CPU row could be replaced or settled twice.");
        check(capture.gpu_complete(failed_frame_gpu,gpu()) && capture.freeze(),"Failed CPU row prevented drained freeze.");
        check(!capture.fail_frame(failed_frame,cpu(0)) && capture.status().frames_failed==1 &&
              capture.rows()[0].cpu.end_ns==failed_sample.end_ns,"Frozen failed CPU data mutated.");
        check(allocations==failure_allocations,"Explicit failed-frame recording allocated.");
        conserved(capture.status());

        // Maximum bound is accepted, sequence arithmetic is not signed/truncated.
        other.start(262144);
        const auto low=other.begin_frame(0),high=other.begin_frame(std::numeric_limits<std::uint64_t>::max());
        check(low && high && other.status().sequence_gaps==std::numeric_limits<std::uint64_t>::max()-1,"Large frame gaps overflowed.");
        check(other.finish_frame(low,cpu(0)) && other.finish_frame(high,cpu(0)),"Full-width sequence could not finish.");
        other.stop_admission();check(other.freeze(),"Maximum capacity capture failed.");
        GpuTicket expired;
        { Recorder temporary;temporary.start(1);const auto frame=temporary.begin_frame(0);expired=temporary.gpu_submit(frame); }
        check(!expired && !capture.gpu_complete(expired,gpu()),"Destroyed generation retained a usable ticket.");
        std::cout<<"{\"passed\":true,\"scope\":\"synthetic-owner-frame-accounting\",\"allocations_on_record_paths\":0}\n";
        return 0;
    }catch(const std::exception& exception) { std::cerr<<exception.what()<<'\n';return 1; }
}
