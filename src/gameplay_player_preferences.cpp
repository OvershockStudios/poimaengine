// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay_player_preferences.hpp"
#include "player_settings_store.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace poima {
namespace {
using Json=nlohmann::json;
using Binding=GameplayPlayerPreferences;
constexpr std::array<const char*,9> keys{{"camera.vertical_fov","input.sensitivity_x",
    "input.sensitivity_y","input.invert_x","input.invert_y","ui.scale",
    "graphics.samples","graphics.frames_in_flight","audio.master_gain"}};
bool same(PoimaEntityId a,PoimaEntityId b) noexcept {return a.high==b.high && a.low==b.low;}
bool same(PoimaGamePreferenceTicket a,PoimaGamePreferenceTicket b) noexcept {
    return a.high==b.high && a.low==b.low && a.sequence==b.sequence;
}
void require(bool valid,const char* message) {
    if(!valid)throw PlayerPreferences::Error(-32602,message);
}
Json patch_set(const PoimaGamePreferencePatchV1& patch) {
    const auto& v=patch.values;Json set=Json::object();
    for(std::size_t i=0;i<keys.size();++i)if(patch.set_mask&(1u<<i)) {
        switch(i) {
        case 0:set[keys[i]]=v.vertical_fov;break;
        case 1:set[keys[i]]=v.sensitivity_x;break;
        case 2:set[keys[i]]=v.sensitivity_y;break;
        case 3:set[keys[i]]=v.invert_x!=0;break;
        case 4:set[keys[i]]=v.invert_y!=0;break;
        case 5:set[keys[i]]=v.ui_scale;break;
        case 6:set[keys[i]]=v.samples;break;
        case 7:set[keys[i]]=v.frames_in_flight;break;
        case 8:set[keys[i]]=v.master_gain;break;
        default:break;
        }
    }
    return set;
}
std::uint32_t source(std::string_view text) {
    if(text=="authored_camera")return PoimaPreferenceSourceAuthoredCamera;
    if(text=="input_profile")return PoimaPreferenceSourceInputProfile;
    if(text=="window_density")return PoimaPreferenceSourceWindowDensity;
    if(text=="engine_default")return PoimaPreferenceSourceEngineDefault;
    if(text=="profile" || text=="settings_profile")return PoimaPreferenceSourceSettingsProfile;
    if(text=="session_override")return PoimaPreferenceSourceSessionOverride;
    if(text=="explicit_option")return PoimaPreferenceSourceExplicitOption;
    if(text=="live_override")return PoimaPreferenceSourceLiveOverride;
    return PoimaPreferenceSourceNone;
}
PoimaGamePreferenceResultV1 result(PoimaGamePreferenceTicket ticket,
    std::uint32_t state,std::uint32_t rejection) noexcept {
    PoimaGamePreferenceResultV1 value{};value.version=1;value.bytes=sizeof(value);
    value.ticket=ticket;value.state=state;value.rejection=rejection;return value;
}
}
void validate_player_preference_graphics(const PlayerPreferences::State& candidate,
    PlayerPreferenceGraphicsConstraints flags) {
    const auto fields=Json::parse(candidate.fields_json);
    const auto samples=fields.at("graphics.samples").at("next_effective").get<std::uint32_t>();
    require(!flags.deferred || samples==1,"Next-player deferred lighting requires samples=1.");
    require(!flags.ambient_occlusion || (flags.deferred && samples==1),"Next-player ambient occlusion requires deferred single-sample lighting.");
    require(flags.color_debug_view || samples==1,"Next-player scene debug views require samples=1.");
    require(!flags.reconstruction || (samples==1 && flags.color_debug_view),"Next-player reconstruction requires single-sample color output.");
}
GameplayPlayerPreferences::GameplayPlayerPreferences(std::shared_ptr<PlayerPreferences> preferences,
    PoimaEntityId owner,bool replay,PlayerPreferenceGraphicsConstraints constraints)
    :preferences_(std::move(preferences)),identity_(std::make_shared<const std::uint8_t>(0)),
     owner_(owner),replay_(replay),active_(bool(preferences_)),constraints_(constraints) {
    require(!active_ || owner.high || owner.low,"Preference attachment requires a nonzero owner epoch.");
}
PoimaGamePreferenceSnapshotV1 GameplayPlayerPreferences::unavailable_snapshot() noexcept {
    PoimaGamePreferenceSnapshotV1 value{};value.version=1;value.bytes=sizeof(value);return value;
}
PoimaGamePreferenceEnqueueV1 GameplayPlayerPreferences::unavailable_enqueue() noexcept {
    PoimaGamePreferenceEnqueueV1 value{};value.version=1;value.bytes=sizeof(value);
    value.rejection=PoimaPreferenceRejectionUnavailable;return value;
}
PoimaGamePreferenceResultV1 GameplayPlayerPreferences::unavailable_result(const PoimaGamePreferenceTicket& ticket) noexcept {
    return result(ticket,PoimaPreferenceResultUnknown,PoimaPreferenceRejectionUnavailable);
}
PoimaGamePreferenceSnapshotV1 GameplayPlayerPreferences::snapshot() const {
    auto out=unavailable_snapshot();if(!active_)return out;
    const auto state=preferences_->snapshot();const auto fields=Json::parse(state->fields_json);
    const auto requested=Json::parse(state->requested_json);const auto& v=state->values;
    out.available=1;out.replay=replay_;out.owner=owner_;out.revision=state->revision;
    out.value_mask=known_mask&~(1u|32u);
    out.values={0,v.sensitivity_x,v.sensitivity_y,0,v.master_gain,
        static_cast<std::uint32_t>(v.invert_x),static_cast<std::uint32_t>(v.invert_y),v.samples,v.frames_in_flight};
    if(v.vertical_fov) {out.values.vertical_fov=*v.vertical_fov;out.value_mask|=1u;}
    else if(observation_.mask&8u) {out.values.vertical_fov=observation_.effective_fov;out.value_mask|=1u;}
    if(v.ui_scale) {out.values.ui_scale=*v.ui_scale;out.value_mask|=32u;}
    else if(observation_.mask&16u) {out.values.ui_scale=observation_.effective_ui_scale;out.value_mask|=32u;}
    for(std::size_t i=0;i<keys.size();++i) {
        if(requested.contains(keys[i]))out.override_mask|=1u<<i;
        out.sources[i]=source(fields.at(keys[i]).at("source").get<std::string>());
    }
    out.next_samples=fields.at(keys[6]).at("next_effective").get<std::uint32_t>();
    out.next_frames=fields.at(keys[7]).at("next_effective").get<std::uint32_t>();
    out.observation_mask=observation_.mask;out.audio_outcome=observation_.audio_outcome;
    out.observed_revision=observation_.observed_revision;out.applied_revision=observation_.applied_revision;
    out.presented_revision=observation_.presented_revision;out.effective_fov=observation_.effective_fov;
    out.effective_ui_scale=observation_.effective_ui_scale;
    out.requested_gain=(observation_.mask&1u) ? observation_.requested_gain : v.master_gain;
    out.sink_gain=observation_.sink_gain;return out;
}
void GameplayPlayerPreferences::validate_patch_encoding(const PoimaGamePreferencePatchV1& patch) {
    require(patch.version==1 && patch.bytes==sizeof(patch) && patch.reserved==0,"Invalid preference patch header/reserved field.");
    require(((patch.set_mask|patch.reset_mask)&~known_mask)==0 && (patch.set_mask&patch.reset_mask)==0,
        "Preference masks must be known and disjoint.");
    const auto& v=patch.values;
    const std::array<double,9> values{{v.vertical_fov,v.sensitivity_x,v.sensitivity_y,
        static_cast<double>(v.invert_x),static_cast<double>(v.invert_y),v.ui_scale,
        static_cast<double>(v.samples),static_cast<double>(v.frames_in_flight),v.master_gain}};
    for(std::size_t i=0;i<keys.size();++i) {
        require(std::isfinite(values[i]),"Preference patch numbers must be finite.");
        require((patch.set_mask&(1u<<i)) || values[i]==0,"Unused preference patch slots must be zero.");
    }
    require(v.invert_x<=1 && v.invert_y<=1,"Preference booleans must be 0 or 1.");
    require(patch.expected_revision<=9007199254740991ULL,"Preference revision exceeds the safe integer limit.");
    try {(void)player_settings::validate_values(patch_set(patch));}
    catch(const player_settings::SettingsError& error) {throw PlayerPreferences::Error(error.code,error.what());}
}
GameplayPlayerPreferences::Preparation GameplayPlayerPreferences::prepare(
    const PoimaGamePreferencePatchV1& patch,bool busy) const {
    Preparation out;out.enqueue=unavailable_enqueue();
    // Malformed ABI values reject even with no attached owner. No candidate or
    // ticket is retained, and a later corrected request can use the same slot.
    try {validate_patch_encoding(patch);}
    catch(const PlayerPreferences::Error&) {out.enqueue.rejection=PoimaPreferenceRejectionInvalid;return out;}
    if(!active_)return out;
    if(!same(patch.owner,owner_)) {out.enqueue.rejection=PoimaPreferenceRejectionStaleOwner;return out;}
    const auto baseline=preferences_->snapshot();
    if(patch.expected_revision!=baseline->revision) {out.enqueue.rejection=PoimaPreferenceRejectionStaleRevision;return out;}
    if(replay_) {out.enqueue.rejection=PoimaPreferenceRejectionReplay;return out;}
    if(busy) {out.enqueue.rejection=PoimaPreferenceRejectionBusy;return out;}
    if(!mutation_capacity_ || sequence_==std::numeric_limits<std::uint64_t>::max() || baseline->revision==9007199254740991ULL) {
        out.enqueue.rejection=PoimaPreferenceRejectionCapacity;return out;
    }
    Json reset=Json::array();for(std::size_t i=0;i<keys.size();++i)if(patch.reset_mask&(1u<<i))reset.push_back(keys[i]);
    std::shared_ptr<const PlayerPreferences::State> candidate;
    try {
        candidate=preferences_->prepare(Json{{"expected_revision",patch.expected_revision},{"set",patch_set(patch)},{"reset",reset}}.dump());
        validate_player_preference_graphics(*candidate,constraints_);
    }catch(const PlayerPreferences::Error& error) {
        out.enqueue.rejection=error.code==-32009 ? PoimaPreferenceRejectionStaleRevision : PoimaPreferenceRejectionInvalid;return out;
    }
    auto staged=std::make_shared<Prepared>();staged->identity_=identity_;
    staged->ticket_={owner_.high,owner_.low,sequence_+1};
    staged->baseline_=baseline;staged->candidate_=std::move(candidate);
    out.enqueue.ticket=staged->ticket_;out.enqueue.rejection=PoimaPreferenceRejectionNone;
    out.prepared=std::move(staged);return out;
}
bool GameplayPlayerPreferences::can_commit(const Prepared& prepared) const noexcept {
    return active_ && mutation_capacity_ && prepared.identity_==identity_ && prepared.baseline_ && prepared.candidate_ &&
        same(PoimaEntityId{prepared.ticket_.high,prepared.ticket_.low},owner_) &&
        sequence_!=std::numeric_limits<std::uint64_t>::max() && prepared.ticket_.sequence==sequence_+1 &&
        preferences_->snapshot()==prepared.baseline_ && prepared.candidate_->revision==prepared.baseline_->revision+1;
}
bool GameplayPlayerPreferences::commit(const Prepared& prepared) noexcept {
    if(!can_commit(prepared))return false;
    auto accepted=result(prepared.ticket_,PoimaPreferenceResultAccepted,PoimaPreferenceRejectionNone);
    accepted.accepted_revision=prepared.candidate_->revision;
    preferences_->publish(prepared.candidate_);
    receipts_[receipt_cursor_]=accepted;receipt_cursor_=(receipt_cursor_+1)%receipts_.size();
    sequence_=prepared.ticket_.sequence;return true;
}
PoimaGamePreferenceResultV1 GameplayPlayerPreferences::query(const PoimaGamePreferenceTicket& ticket,
    const Prepared* staged) const noexcept {
    if(!same(PoimaEntityId{ticket.high,ticket.low},owner_))return result(ticket,PoimaPreferenceResultUnknown,PoimaPreferenceRejectionStaleOwner);
    if(ticket.sequence==0)return result(ticket,PoimaPreferenceResultUnknown,PoimaPreferenceRejectionInvalid);
    for(const auto& receipt:receipts_)if(receipt.state==PoimaPreferenceResultAccepted && same(receipt.ticket,ticket))return receipt;
    if(active_ && staged && staged->identity_==identity_ && same(staged->ticket_,ticket) && can_commit(*staged))
        return result(ticket,PoimaPreferenceResultStaged,PoimaPreferenceRejectionNone);
    return result(ticket,PoimaPreferenceResultUnknown,PoimaPreferenceRejectionUnknownTicket);
}
}
