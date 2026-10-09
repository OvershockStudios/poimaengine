// SPDX-License-Identifier: Apache-2.0
#include "poima/player_preferences.hpp"
#include "player_settings_store.hpp"
#include <cmath>
#include <set>
#include <string_view>
#include <vector>

namespace poima {
namespace {
using Json=nlohmann::json;
using Values=PlayerPreferences::Values;
using State=PlayerPreferences::State;
constexpr std::uint64_t max_revision=9007199254740991ULL;
constexpr std::size_t max_json_bytes=64*1024;
void require(bool valid,const char* message,int code=-32602) {
    if(!valid)throw PlayerPreferences::Error(code,message);
}
Json parse(const std::string& bytes) {
    require(bytes.size()<=max_json_bytes,"Player preference JSON exceeds 64 KiB.");
    try {
        std::vector<std::set<std::string>> keys;
        return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
            require(depth<=16,"Player preference JSON exceeds its nesting bound.");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key)
                require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate player preference JSON field.");
            else if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
    }catch(const PlayerPreferences::Error&) {throw;}
    catch(const Json::exception&) {throw PlayerPreferences::Error(-32602,"Invalid player preference JSON.");}
}
Json validate(const Json& values) {
    try {return player_settings::validate_values(values);}
    catch(const player_settings::SettingsError& error) {throw PlayerPreferences::Error(error.code,error.what());}
}
std::uint64_t revision(const Json& value) {
    require(value.is_number_integer(),"Preference revision must be a safe nonnegative JSON integer.");
    if(value.is_number_unsigned()) {
        const auto number=value.get<std::uint64_t>();
        require(number<=max_revision,"Preference revision exceeds the safe JSON integer range.");
        return number;
    }
    const auto number=value.get<std::int64_t>();
    require(number>=0 && static_cast<std::uint64_t>(number)<=max_revision,"Preference revision exceeds the safe JSON integer range.");
    return static_cast<std::uint64_t>(number);
}
Json configured(const Values& values) {
    return {{"camera.vertical_fov",values.vertical_fov ? Json(*values.vertical_fov) : Json(nullptr)},
        {"ui.scale",values.ui_scale ? Json(*values.ui_scale) : Json(nullptr)},
        {"input.sensitivity_x",values.sensitivity_x},{"input.sensitivity_y",values.sensitivity_y},
        {"input.invert_x",values.invert_x},{"input.invert_y",values.invert_y},
        {"audio.master_gain",values.master_gain},{"graphics.samples",values.samples},
        {"graphics.frames_in_flight",values.frames_in_flight}};
}
void validate_inherited(const Values& values) {
    auto fields=configured(values);
    if(!values.vertical_fov)fields.erase("camera.vertical_fov");
    if(!values.ui_scale)fields.erase("ui.scale");
    (void)validate(fields);
}
Values apply(Values values,const Json& overrides) {
    if(overrides.contains("camera.vertical_fov"))values.vertical_fov=overrides.at("camera.vertical_fov").get<double>();
    if(overrides.contains("ui.scale"))values.ui_scale=overrides.at("ui.scale").get<float>();
    if(overrides.contains("input.sensitivity_x"))values.sensitivity_x=overrides.at("input.sensitivity_x").get<double>();
    if(overrides.contains("input.sensitivity_y"))values.sensitivity_y=overrides.at("input.sensitivity_y").get<double>();
    if(overrides.contains("input.invert_x"))values.invert_x=overrides.at("input.invert_x").get<bool>();
    if(overrides.contains("input.invert_y"))values.invert_y=overrides.at("input.invert_y").get<bool>();
    if(overrides.contains("audio.master_gain"))values.master_gain=overrides.at("audio.master_gain").get<double>();
    if(overrides.contains("graphics.samples"))values.samples=overrides.at("graphics.samples").get<std::uint32_t>();
    if(overrides.contains("graphics.frames_in_flight"))values.frames_in_flight=overrides.at("graphics.frames_in_flight").get<std::uint32_t>();
    return values;
}
const char* inherited_source(std::string_view id) {
    if(id.starts_with("input."))return "input_profile";
    if(id=="camera.vertical_fov")return "authored_camera";
    if(id=="ui.scale")return "window_density";
    return "engine_default";
}
std::shared_ptr<const State> state(const Values& baseline,const Json& requested,const Json& sources,
    const Json& profile,std::uint64_t current,const Values* frozen_graphics) {
    auto result=std::make_shared<State>();
    result->revision=current;
    result->values=apply(baseline,requested);
    const auto desired=configured(result->values);
    if(frozen_graphics) {
        result->values.samples=frozen_graphics->samples;
        result->values.frames_in_flight=frozen_graphics->frames_in_flight;
    }
    const auto effective=configured(result->values);
    auto rows=Json::object();
    for(const auto& item:effective.items()) {
        const bool graphics=item.key().starts_with("graphics.");
        rows[item.key()]={{"requested",requested.contains(item.key()) ? requested.at(item.key()) : Json(nullptr)},
            {"effective",item.value()},{"source",sources.contains(item.key()) ? sources.at(item.key()) : Json(inherited_source(item.key()))},
            {"application",graphics ? "next_player" : "live"},
            {"requires_next_player",graphics && desired.at(item.key())!=item.value()}};
        if(graphics)rows[item.key()]["next_effective"]=desired.at(item.key());
    }
    result->requested_json=requested.dump();
    result->sources_json=sources.dump();
    result->profile_json=profile.dump();
    result->fields_json=rows.dump();
    return result;
}
}

PlayerPreferences::PlayerPreferences(Values inherited,const std::string& initial_values_json,
    const std::string& initial_sources_json,const std::string& profile_json):inherited_(inherited) {
    validate_inherited(inherited_);
    const auto requested=validate(parse(initial_values_json));
    auto sources=parse(initial_sources_json);
    require(sources.is_object() && sources.size()<=9,"Preference sources must be a bounded object.");
    for(const auto& item:sources.items()) {
        require(requested.contains(item.key()),"Preference source metadata must name an initial override.");
        require(item.value().is_string(),"Preference source metadata must contain strings.");
        const auto& name=item.value().get_ref<const std::string&>();
        require(!name.empty() && name.size()<=64 && name.find('\0')==std::string::npos,"Preference source metadata is outside its bounds.");
    }
    for(const auto& item:requested.items())if(!sources.contains(item.key()))sources[item.key()]="session_override";
    const auto profile=parse(profile_json);
    require(profile.is_object() && profile.size()<=16,"Preference profile metadata must be a bounded object.");
    state_=state(inherited_,requested,sources,profile,0,nullptr);
}

std::shared_ptr<const PlayerPreferences::State> PlayerPreferences::prepare(const std::string& patch_json) const {
    const auto patch=parse(patch_json);
    require(patch.is_object() && patch.size()<=3 && patch.contains("expected_revision"),"Preference patch requires expected_revision and optional set/reset.");
    for(const auto& item:patch.items())require(item.key()=="expected_revision" || item.key()=="set" || item.key()=="reset","Unknown player preference patch field.");
    const auto expected=revision(patch.at("expected_revision"));
    require(expected==state_->revision,"Live preference revision conflict; inspect and retry.",-32009);
    require(expected<max_revision,"Live preference revision limit reached.");
    const auto set=patch.contains("set") ? validate(patch.at("set")) : Json::object();
    auto requested=parse(state_->requested_json);
    auto sources=parse(state_->sources_json);
    if(patch.contains("reset")) {
        const auto& reset=patch.at("reset");
        require(reset.is_array() && reset.size()<=9,"Preference reset exceeds the registry ID count.");
        std::set<std::string> seen;
        const auto keys=player_settings::values_schema().at("properties");
        for(const auto& id:reset) {
            require(id.is_string(),"Preference reset IDs must be strings.");
            const auto& name=id.get_ref<const std::string&>();
            require(keys.contains(name),"Unknown player preference reset ID.");
            require(seen.insert(name).second,"Duplicate player preference reset ID.");
            require(!set.contains(name),"A preference cannot be set and reset in one patch.");
            requested.erase(name);sources.erase(name);
        }
    }
    for(const auto& item:set.items()) {requested[item.key()]=item.value();sources[item.key()]="live_override";}
    return state(inherited_,requested,sources,parse(state_->profile_json),expected+1,&state_->values);
}

std::string PlayerPreferences::inspect() const {
    return Json({{"format","poima.player-preferences.v1"},{"revision",state_->revision},
        {"values",parse(state_->requested_json)},{"sources",parse(state_->sources_json)},
        {"profile",parse(state_->profile_json)},{"fields",parse(state_->fields_json)}}).dump();
}
}
