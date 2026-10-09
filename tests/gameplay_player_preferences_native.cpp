// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay_player_preferences.hpp"
#include "poima/gameplay_compatibility.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace poima;
using Json=nlohmann::json;
using Binding=GameplayPlayerPreferences;
constexpr PoimaEntityId epoch{7,81};
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
PoimaGamePreferencePatchV1 patch(std::uint64_t revision=0) {
    PoimaGamePreferencePatchV1 value{};value.version=1;value.bytes=sizeof(value);
    value.owner=epoch;value.expected_revision=revision;return value;
}
void rejected(Binding& binding,const PoimaGamePreferencePatchV1& value,std::uint32_t rejection,bool busy=false) {
    const auto before=binding.snapshot();const auto prepared=binding.prepare(value,busy);
    check(!prepared.prepared && prepared.enqueue.rejection==rejection && prepared.enqueue.ticket.sequence==0,
        "Rejected patch retained a candidate/ticket or wrong rejection.");
    const auto after=binding.snapshot();
    check(before.revision==after.revision && before.override_mask==after.override_mask,
        "Rejected preparation mutated configuration.");
}
void unavailable_and_observations() {
    Binding absent({},{});auto out=absent.snapshot();
    check(out.version==1 && out.bytes==216 && !out.available && !out.value_mask && !out.owner.low,
        "Absent owner fabricated configuration.");
    rejected(absent,patch(),PoimaPreferenceRejectionUnavailable);
    auto malformed=patch();malformed.reserved=1;rejected(absent,malformed,PoimaPreferenceRejectionInvalid);
    auto owner=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{});
    Binding binding(owner,epoch);out=binding.snapshot();
    check(out.available && out.value_mask==478 && out.override_mask==0 && out.values.vertical_fov==0 &&
        out.values.ui_scale==0 && out.values.samples==4 && out.next_samples==4 &&
        out.sources[0]==PoimaPreferenceSourceAuthoredCamera && out.sources[5]==PoimaPreferenceSourceWindowDensity,
        "Unresolved inherited values were assigned fake fallback numbers.");
    Binding::Observation observation;observation.mask=8;observation.effective_fov=73;
    binding.observe(observation);out=binding.snapshot();
    check(out.value_mask==479 && out.values.vertical_fov==73 && out.observation_mask==8 && !out.observed_revision,
        "Initial authored camera observation implied a rendered revision/density.");
    observation.mask=63;observation.observed_revision=2;observation.applied_revision=1;
    observation.presented_revision=0;observation.effective_ui_scale=1.25;
    observation.requested_gain=.4;observation.sink_gain=.25;observation.audio_outcome=PoimaPreferenceAudioSinkGainVerified;
    binding.observe(observation);out=binding.snapshot();
    check(out.value_mask==511 && out.observed_revision==2 && out.applied_revision==1 && out.presented_revision==0 &&
        out.values.ui_scale==1.25 && out.sink_gain==.25 && out.requested_gain==.4 && out.values.master_gain==1,
        "Cached observed/application values were confused with authoritative intent.");
}
void rollback_staging_and_receipts() {
    auto owner=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{});Binding binding(owner,epoch);
    auto value=patch();value.set_mask=1u|256u;value.values.vertical_fov=95;value.values.master_gain=.3;
    auto first=binding.prepare(value);check(first.prepared && first.enqueue.ticket.sequence==1,"First staged ticket is wrong.");
    check(owner->snapshot()->revision==0 && binding.snapshot().override_mask==0,
        "Preparation published intent before whole native boundary.");
    check(binding.query(first.enqueue.ticket).state==PoimaPreferenceResultUnknown &&
        binding.query(first.enqueue.ticket,first.prepared.get()).state==PoimaPreferenceResultStaged,
        "Staged query leaked into committed ledger.");
    first.prepared.reset();auto retry=binding.prepare(value);
    check(retry.enqueue.ticket.sequence==1 && binding.can_commit(*retry.prepared),"Rollback consumed a ticket.");
    check(binding.commit(*retry.prepared) && !binding.commit(*retry.prepared),"Commit is not exactly once.");
    auto accepted=binding.query(retry.enqueue.ticket);
    check(accepted.state==PoimaPreferenceResultAccepted && accepted.accepted_revision==1 && !accepted.rejection &&
        !accepted.error_code && owner->snapshot()->values.master_gain==.3 && !binding.snapshot().observation_mask,
        "Accepted intent fabricated application or lacked immutable result.");
    auto wrong=retry.enqueue.ticket;wrong.low=82;
    check(binding.query(wrong).rejection==PoimaPreferenceRejectionStaleOwner,"Wrong owner ticket crossed attachment.");
    auto zero=retry.enqueue.ticket;zero.sequence=0;
    check(binding.query(zero).rejection==PoimaPreferenceRejectionInvalid,"Zero ticket accepted.");
    // Thirty-three subsequent accepted no-ops evict the earliest result while
    // preserving stable monotonic tickets and incrementing real revisions.
    PoimaGamePreferenceTicket last{};
    for(std::uint64_t revision=1;revision<=33;++revision) {
        auto next=binding.prepare(patch(revision));check(next.prepared && next.enqueue.ticket.sequence==revision+1,
            "Committed sequence or owner revision drifted.");
        check(binding.commit(*next.prepared),"Prepared no-op failed commit.");last=next.enqueue.ticket;
    }
    check(binding.query(retry.enqueue.ticket).rejection==PoimaPreferenceRejectionUnknownTicket &&
        binding.query(last).accepted_revision==34,"Bounded receipt eviction/result altered acceptance.");
    binding.deactivate();check(!binding.active() && !binding.snapshot().available,"Detached owner remained readable.");
    check(binding.query(last).state==PoimaPreferenceResultAccepted,"Detach destroyed historical accepted receipt.");
    rejected(binding,patch(34),PoimaPreferenceRejectionUnavailable);
}
void stale_and_lifecycle_policies() {
    auto owner=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{});Binding binding(owner,epoch);
    auto bad=patch();bad.owner.low=1;rejected(binding,bad,PoimaPreferenceRejectionStaleOwner);
    rejected(binding,patch(1),PoimaPreferenceRejectionStaleRevision);
    rejected(binding,patch(),PoimaPreferenceRejectionBusy,true);
    auto capacity_staged=binding.prepare(patch());
    check(bool(capacity_staged.prepared),"Default host mutation capacity blocked preparation.");
    binding.mutation_capacity(false);
    check(binding.snapshot().available && !binding.can_commit(*capacity_staged.prepared) &&
        !binding.commit(*capacity_staged.prepared),"Exhausted host capacity hid reads or committed intent.");
    rejected(binding,patch(),PoimaPreferenceRejectionCapacity);
    binding.mutation_capacity(true);
    auto capacity_retry=binding.prepare(patch());
    check(capacity_retry.prepared && capacity_retry.enqueue.ticket.sequence==1 &&
        binding.can_commit(*capacity_retry.prepared) && owner->snapshot()->revision==0,
        "Host capacity denial consumed ticket/revision or blocked restored admission.");
    Binding replay(owner,epoch,true);check(replay.snapshot().replay,"Replay observation unavailable.");
    rejected(replay,patch(),PoimaPreferenceRejectionReplay);
    auto staged=binding.prepare(patch());const auto candidate=staged.prepared->candidate();
    owner->publish(owner->prepare(R"({"expected_revision":0,"set":{"audio.master_gain":0.5}})"));
    const auto external=owner->snapshot();
    check(!binding.can_commit(*staged.prepared) && !binding.commit(*staged.prepared) && owner->snapshot()==external &&
        candidate->revision==1 && binding.query(staged.enqueue.ticket).state==PoimaPreferenceResultUnknown,
        "Stale external publication was overwritten or created an accepted ticket.");
    // A different binding to the same authority/epoch cannot commit this one's
    // prepared effect, even when baselines and proposed ticket numbers coincide.
    auto next=binding.prepare(patch(1));Binding other(owner,epoch);
    check(!other.can_commit(*next.prepared) && !other.commit(*next.prepared),"Prepared identity escaped its binding.");
    check(binding.commit(*next.prepared) && next.enqueue.ticket.sequence==1,"Rejected stale commit consumed sequence.");
    auto limit=std::make_shared<PlayerPreferences::State>(*owner->snapshot());limit->revision=9007199254740991ULL;
    owner->publish(limit);rejected(binding,patch(limit->revision),PoimaPreferenceRejectionCapacity);
}
void strict_encoding() {
    auto owner=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{});Binding binding(owner,epoch);
    for(unsigned case_id=0;case_id<15;++case_id) {
        auto value=patch();
        switch(case_id) {
        case 0:value.version=2;break;case 1:value.bytes=103;break;case 2:value.reserved=1;break;
        case 3:value.set_mask=512;break;case 4:value.reset_mask=512;break;
        case 5:value.set_mask=value.reset_mask=1;value.values.vertical_fov=60;break;
        case 6:value.values.master_gain=.5;break;case 7:value.set_mask=8;value.values.invert_x=2;break;
        case 8:value.set_mask=1;value.values.vertical_fov=std::numeric_limits<double>::quiet_NaN();break;
        case 9:value.set_mask=2;value.values.sensitivity_x=std::numeric_limits<double>::infinity();break;
        case 10:value.set_mask=32;value.values.ui_scale=.24;break;
        case 11:value.set_mask=64;value.values.samples=2;break;
        case 12:value.set_mask=128;value.values.frames_in_flight=3;break;
        case 13:value.set_mask=256;value.values.master_gain=1.01;break;
        case 14:value.expected_revision=9007199254740992ULL;break;
        }
        rejected(binding,value,PoimaPreferenceRejectionInvalid);
        bool denied=false;try {Binding::validate_patch_encoding(value);}catch(const PlayerPreferences::Error& e) {denied=e.code==-32602;}
        check(denied,"Unattached complete encoding validator accepted malformed values.");
    }
    auto full=patch();full.set_mask=511;full.values={5,0,10,8,0,1,0,1,2};
    auto prepared=binding.prepare(full);check(prepared.prepared && binding.commit(*prepared.prepared),"Valid complete fixed registry encoding rejected.");
    check(binding.snapshot().override_mask==511,"Full patch dropped typed slots.");
}
void reset_sources_and_graphics() {
    PlayerPreferences::Values inherited;inherited.sensitivity_x=.2;inherited.master_gain=.5;
    auto owner=std::make_shared<PlayerPreferences>(inherited,R"({"graphics.samples":1,"graphics.frames_in_flight":1,"audio.master_gain":0.8})",
        R"({"graphics.samples":"profile","graphics.frames_in_flight":"explicit_option","audio.master_gain":"session_override"})");
    Binding binding(owner,epoch);auto out=binding.snapshot();
    check(out.sources[6]==PoimaPreferenceSourceSettingsProfile && out.sources[7]==PoimaPreferenceSourceExplicitOption &&
        out.sources[8]==PoimaPreferenceSourceSessionOverride,"Stored/explicit provenance enum mapping differs.");
    auto reset=patch();reset.reset_mask=64|128|256;auto prepared=binding.prepare(reset);
    check(prepared.prepared && binding.commit(*prepared.prepared),"Validated graphics reset rejected.");out=binding.snapshot();
    check(out.values.samples==1 && out.values.frames_in_flight==1 && out.next_samples==4 && out.next_frames==2 &&
        out.values.master_gain==.5 && out.override_mask==0 && out.sources[8]==PoimaPreferenceSourceEngineDefault,
        "Reset mutated frozen graphics or ignored native inheritance.");
    // Whole-candidate failure: master gain and next samples validate together.
    auto constrained_owner=std::make_shared<PlayerPreferences>(PlayerPreferences::Values{},R"({"graphics.samples":1})");
    Binding constrained(constrained_owner,epoch,false,{true,true,true,false});
    auto change=patch();change.set_mask=64|256;change.values.samples=4;change.values.master_gain=.25;
    rejected(constrained,change,PoimaPreferenceRejectionInvalid);
    check(constrained_owner->snapshot()->values.master_gain==1,"Invalid graphics partially accepted live audio.");
    auto reset_graphic=patch();reset_graphic.reset_mask=64;rejected(constrained,reset_graphic,PoimaPreferenceRejectionInvalid);
    auto valid=patch();valid.set_mask=64|256;valid.values.samples=1;valid.values.master_gain=.25;
    auto accepted=constrained.prepare(valid);check(accepted.prepared && constrained.commit(*accepted.prepared),"Valid combined graphics/live patch rejected.");
    const auto target=owner->snapshot();
    for(const auto flags:{PlayerPreferenceGraphicsConstraints{true,false,true,false},
        PlayerPreferenceGraphicsConstraints{false,true,true,false},PlayerPreferenceGraphicsConstraints{false,false,false,false},
        PlayerPreferenceGraphicsConstraints{false,false,true,true}}) {
        bool denied=false;try {validate_player_preference_graphics(*target,flags);}catch(const PlayerPreferences::Error&) {denied=true;}
        check(denied,"Shared graphics validator allowed invalid desired next renderer.");
    }
}
void independent_feature_policy() {
    using namespace poima::gameplay_abi;
    Contract required;required.services_bytes=256;required.features.push_back(player_preferences_feature);
    const auto available=available_contract();check(available.services_bytes==256 && compatibility_error(required,available).empty(),
        "Default host lacks named preference extension.");
    check(compatibility_error(required,required).empty(),"Preference feature requires unrelated services.");
    for(auto extent:{176u,192u,208u,216u,224u,232u,255u,257u}) {
        auto invalid=required;invalid.services_bytes=extent;
        check(!compatibility_error(invalid,available).empty(),"Preference requirement accepted wrong named prefix.");
    }
    auto opaque=available;std::erase(opaque.features,std::string(player_preferences_feature));
    check(!compatibility_error(required,opaque).empty(),"Opaque extent granted named preferences.");
    auto short_host=required;short_host.services_bytes=232;
    check(!compatibility_error(Contract{},short_host).empty(),"Host advertised preference callbacks without allocation.");
    auto duplicate=required;duplicate.features.push_back(player_preferences_feature);
    check(!compatibility_error(duplicate,available).empty(),"Duplicate preference requirement accepted.");
    for(unsigned mask=0;mask<32;++mask) {
        auto combination=required;
        if(mask&1)combination.features.push_back(animation_feature);
        if(mask&2)combination.features.push_back(animation_layers_feature);
        if(mask&4)combination.features.push_back(character_input_feature);
        if(mask&8)combination.features.push_back(navigation_feature);
        if(mask&16)combination.features.push_back(instances_feature);
        auto all=available;if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        const bool valid=!(mask&2) || bool(mask&1);
        check(compatibility_error(combination,all).empty()==valid,"New prefix bypassed independent feature dependency.");
    }
    PoimaGamePlayerPreferenceServicesV1 services{};auto& baseline=services.instances.navigation.character.animation.animation.baseline;
    baseline.version=7;baseline.bytes=256;const auto legacy=baseline_view(baseline);
    check(legacy.bytes==176 && baseline.bytes==256,"New tail modified legacy bounded-view ABI.");
}
}
int main() {try {
    unavailable_and_observations();rollback_staging_and_receipts();stale_and_lifecycle_policies();strict_encoding();
    reset_sources_and_graphics();independent_feature_policy();
    std::cout<<"Compiled preference binding 6 groups passed.\n";return 0;
}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
