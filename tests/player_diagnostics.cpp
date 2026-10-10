// SPDX-License-Identifier: Apache-2.0
#include "poima/player_diagnostics.hpp"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace pd=poima::player_diagnostics;
namespace fp=poima::frame_performance;
namespace {
static_assert(std::is_trivially_copyable_v<pd::Sample> && std::is_trivially_copyable_v<pd::Snapshot>);
void check(bool value,const char* detail) {if(!value)throw std::runtime_error(detail);}
pd::Sample poll(std::uint64_t begin,std::uint64_t end,std::uint64_t tick=0) {
    pd::Sample result;result.begin_ns=begin;result.end_ns=end;
    result.tick_before=result.tick_after=tick;return result;
}
void threshold_and_prior() {
    pd::Recorder recorder;
    auto first=poll(0,pd::slow_threshold_ns);
    first.width=1920;first.height=1080;
    check(recorder.record(first),"Owner poll rejected.");
    auto second=poll(first.end_ns+pd::slow_threshold_ns,first.end_ns+pd::slow_threshold_ns+1);
    second.width=1920;second.height=1080;
    recorder.record(second);
    check(recorder.snapshot().count==0,"Exactly 50 ms was classified as strictly slow.");
    auto third=poll(second.end_ns+pd::slow_threshold_ns+1,second.end_ns+pd::slow_threshold_ns+2);
    third.width=1920;third.height=1051;third.flags=fp::Flag::resize;
    third.resize_ns=49'000'000;
    third.audio.device_resume_ns=759'000'000;
    third.clock={true,true,.9,8.0/60,.9-8.0/60,0,8,8};
    recorder.record(third);
    auto result=recorder.snapshot();
    check(result.polls==3 && result.slow_polls==1 && result.count==1 &&
          result.clock_drop_polls==1,"Real clock drop did not retain a fast poll with a slow gap.");
    const auto& row=result.rows[0];
    check(row.valid && row.previous_available && row.gap_available && row.poll_sequence==3 &&
          row.previous.begin_ns==second.begin_ns && row.previous.end_ns==second.end_ns &&
          row.inter_poll_gap_ns==pd::slow_threshold_ns+1 &&
          row.sample.audio.device_resume_ns==759'000'000 && row.sample.clock.planned_ticks==8,
          "Prior poll or external resume evidence lost.");
    check(row.previous.width==1920 && row.previous.height==1080 &&
          row.sample.width==1920 && row.sample.height==1051 &&
          (row.sample.flags&fp::Flag::resize),
          "Actual current/prior native extent or resize context was discarded.");
    auto fourth=poll(third.end_ns,third.end_ns+pd::slow_threshold_ns+1);
    fourth.work.present_ns=40'000'000;fourth.work.render_ns=50'000'001;
    recorder.record(fourth);result=recorder.snapshot();
    check(result.count==2 && result.max_wall_ns==pd::slow_threshold_ns+1 &&
          result.max_gap_ns==pd::slow_threshold_ns+1 &&
          result.rows[1].previous.begin_ns==third.begin_ns &&
          result.rows[1].previous.resize_ns==49'000'000 &&
          result.rows[1].sample.work.present_ns==40'000'000,
          "Overlapping stage durations or previous slow frame were discarded.");
}
void ring_and_lifetime() {
    pd::Recorder recorder;
    for(std::uint64_t i=0;i<pd::capacity+17;++i) {
        auto sample=poll(i*1000,i*1000+1,i);sample.flags=fp::Flag::error;
        sample.audio.enqueue_ns=i;
        recorder.record(sample);
    }
    const auto result=recorder.snapshot();
    check(result.count==pd::capacity && result.polls==pd::capacity+17 &&
          result.slow_polls==pd::capacity+17 && result.overwritten==17 &&
          result.invalid_samples==0,"Ring overflow changed lifetime accounting.");
    for(std::uint32_t i=0;i<result.count;++i)
        check(result.rows[i].poll_sequence==18+i && result.rows[i].sample.audio.enqueue_ns==17+i &&
              result.rows[i].previous_available && result.rows[i].previous.audio.enqueue_ns==16+i,
              "Retained rows are not chronological with their actual predecessor.");
    auto fast=poll((pd::capacity+17)*1000,(pd::capacity+17)*1000+1);
    recorder.record(fast);
    const auto after=recorder.snapshot();
    check(after.polls==result.polls+1 && after.count==result.count &&
          after.overwritten==result.overwritten && after.rows[0].poll_sequence==result.rows[0].poll_sequence,
          "Unretained ordinary poll evicted diagnostic evidence.");
}
void first_gap_and_pause() {
    pd::Recorder recorder;
    auto first=poll(200,200+pd::slow_threshold_ns+1);
    first.clock={true,false,10,0,0,0,0,0};first.flags=fp::Flag::paused;
    recorder.record(first);
    auto result=recorder.snapshot();
    check(result.count==1 && !result.rows[0].previous_available && !result.rows[0].gap_available &&
          result.rows[0].valid && result.rows[0].sample.width==0 && result.rows[0].sample.height==0 &&
          result.max_gap_ns==0 && result.clock_drop_polls==0,
          "First poll invented a gap or paused wall time became discarded clock time.");
    auto replacement=poll(first.end_ns,first.end_ns+1);
    replacement.tick_before=900;replacement.tick_after=7;
    replacement.runtime_replacements_before=12;replacement.runtime_replacements_after=13;
    replacement.flags=fp::Flag::error|fp::Flag::replacement;
    replacement.clock={true,true,8.0/60,8.0/60,0,.0001,8,1};
    recorder.record(replacement);
    result=recorder.snapshot();
    check(result.rows[1].valid && result.rows[1].sample.clock.committed_ticks==1,
          "A restored owner with fewer committed ticks was rejected as backwards simulation.");
    auto saturated_replacement=poll(replacement.end_ns,replacement.end_ns+1);
    saturated_replacement.tick_before=800;saturated_replacement.tick_after=2;
    saturated_replacement.runtime_replacements_before=saturated_replacement.runtime_replacements_after=
        std::numeric_limits<std::uint32_t>::max();
    saturated_replacement.flags=fp::Flag::error|fp::Flag::replacement;
    recorder.record(saturated_replacement);
    check(recorder.snapshot().rows[2].valid,"Saturated replacement count broke legitimate replacement evidence.");
}
void failure_and_owned_query() {
    pd::Recorder recorder;
    auto running=poll(1,11,99);running.tick_after=100;
    recorder.record(running);
    auto failure=poll(13,20,100);failure.flags=fp::Flag::error;
    failure.work.simulation_owner_ns=7;failure.work.report_ns=2;
    failure.clock={true,true,1.0/60,1.0/60,0,0,1,0};
    recorder.record(failure);
    const auto retained=recorder.snapshot();
    check(retained.count==1 && retained.rows[0].valid && retained.rows[0].sample.tick_after==100 &&
          retained.rows[0].sample.clock.planned_ticks==1 && retained.rows[0].sample.clock.committed_ticks==0 &&
          retained.rows[0].previous.tick_after==100 && retained.rows[0].sample.work.simulation_owner_ns==7,
          "A failed simulation attempt disappeared or fabricated a committed tick.");
    auto subsequent=poll(21,22,100);subsequent.flags=fp::Flag::error;
    recorder.record(subsequent);
    const auto current=recorder.snapshot();
    check(current.count==2 && retained.count==1 && retained.rows[1].poll_sequence==0 &&
          current.rows[1].previous.begin_ns==failure.begin_ns,
          "Query copies borrowed mutable recorder storage or lost the failed predecessor.");
}
void malformed_evidence() {
    pd::Recorder recorder;
    auto bad=poll(100,99);bad.clock={true,true,.1,.09,.01,0,2,3};
    recorder.record(bad);
    auto first=recorder.snapshot();
    check(first.count==1 && !first.rows[0].valid &&
          (first.rows[0].issues&pd::Issue::invalid_wall) &&
          (first.rows[0].issues&pd::Issue::invalid_clock) &&
          first.rows[0].sample.end_ns==99 && first.max_wall_ns==0,
          "Malformed timing/clock data was discarded, repaired or unsigned-underflowed.");
    auto nan=poll(110,111);nan.clock.observed=true;
    nan.clock.elapsed_seconds=std::numeric_limits<double>::quiet_NaN();
    recorder.record(nan);auto result=recorder.snapshot();
    check(result.invalid_samples==2 && result.count==2 && !result.rows[1].gap_available &&
          std::isnan(result.rows[1].sample.clock.elapsed_seconds),"NaN or invalid predecessor was hidden.");
    auto backwards=poll(105,108);backwards.tick_before=7;backwards.tick_after=6;
    backwards.runtime_replacements_before=4;backwards.runtime_replacements_after=3;
    recorder.record(backwards);result=recorder.snapshot();
    check(result.invalid_samples==3 && !result.rows[2].gap_available &&
          (result.rows[2].issues&pd::Issue::backwards_poll) &&
          (result.rows[2].issues&pd::Issue::backwards_replacements),
          "Nonmonotonic intervals or replacement counters fabricated gap durations.");
    auto tick=poll(112,113);tick.tick_before=10;tick.tick_after=9;
    recorder.record(tick);result=recorder.snapshot();
    check((result.rows[3].issues&pd::Issue::backwards_tick)!=0,
          "Backwards ticks without replacement were accepted.");
    auto mismatch=poll(114,115);mismatch.clock={true,true,.1,.05,.04,0,1,1};
    recorder.record(mismatch);result=recorder.snapshot();
    check((result.rows[4].issues&pd::Issue::invalid_clock)!=0 && result.clock_drop_polls==0,
          "Invalid accepted/dropped relation polluted clock-drop accounting.");
    auto negative=poll(116,117);negative.clock={true,false,1,0,0,-.1,0,0};
    recorder.record(negative);
    auto infinity=poll(118,119);infinity.clock={true,true,1,0,1,0,0,0};
    infinity.clock.dropped_seconds=std::numeric_limits<double>::infinity();recorder.record(infinity);
    result=recorder.snapshot();
    check(result.invalid_samples==7 && result.count==7 && result.clock_drop_polls==0,
          "Negative or infinite observations disappeared or polluted valid summaries.");
}
void owner_and_saturation() {
    pd::Recorder recorder;recorder.record(poll(1,2));
    std::atomic<bool> record_rejected=false,query_rejected=false;
    std::thread other([&] {
        record_rejected=!recorder.record(poll(2,3));
        try {(void)recorder.snapshot();}catch(const std::logic_error&) {query_rejected=true;}
    });other.join();
    const auto result=recorder.snapshot();
    check(record_rejected && query_rejected && result.polls==1 && result.count==0,
          "Off-owner recording/query touched native state.");
    auto counter=std::numeric_limits<std::uint64_t>::max()-2;bool saturated=false;
    pd::saturating_add(counter,2,saturated);
    check(counter==std::numeric_limits<std::uint64_t>::max() && !saturated,
          "Exactly reaching the counter maximum falsely reported overflow.");
    pd::saturating_add(counter,1,saturated);
    check(counter==std::numeric_limits<std::uint64_t>::max() && saturated,
          "Timing counter overflow wrapped or was hidden.");
    pd::saturating_add(counter,0,saturated);check(saturated,"Saturation was not sticky.");
    pd::AudioWork previous,current;
    previous.queue_wait_ns=20;current.queue_wait_ns=35;
    previous.device_resume_ns=7;current.device_resume_ns=759'000'007;
    previous.drain_wait_ns=current.drain_wait_ns=std::numeric_limits<std::uint64_t>::max();
    const auto delta=pd::audio_delta(current,previous);
    check(delta.queue_wait_ns==15 && delta.device_resume_ns==759'000'000 &&
          delta.drain_wait_ns==0 && !delta.counters_saturated,
          "Cumulative timing differences lost external controls or invented overflow.");
    current.queue_wait_ns=19;current.dsp_ns=50;
    const auto regressed=pd::audio_delta(current,previous);
    check(regressed.queue_wait_ns==0 && regressed.dsp_ns==50 && regressed.counters_saturated,
          "A backwards cumulative timing counter underflowed or hid uncertainty.");
    previous.counters_saturated=true;current.queue_wait_ns=20;
    check(pd::audio_delta(current,previous).counters_saturated,
          "A previously saturated baseline lost the sticky uncertainty marker.");
    auto audio=poll(2,3);audio.audio.counters_saturated=true;recorder.record(audio);
    check(recorder.snapshot().counters_saturated,"Audio cumulative saturation was lost on an ordinary poll.");
}
}
int main() {
    try {
        threshold_and_prior();ring_and_lifetime();first_gap_and_pause();
        failure_and_owned_query();malformed_evidence();owner_and_saturation();
        std::cout<<"Player diagnostics: bounded timing/clock evidence and ownership passed.\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
