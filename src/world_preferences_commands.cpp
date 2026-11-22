// SPDX-License-Identifier: Apache-2.0
#include "world_preferences_commands.hpp"

#include "input_profile_store.hpp"
#include "player_settings_store.hpp"
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include "world_capture.hpp"
#include "world_storage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>

namespace poima::world_detail {

fs::path resolve_preference_profile_path(const Json& value,const PreferencePaths& paths,PreferenceKind profile_kind) {
    const bool settings=profile_kind==PreferenceKind::settings;
    const std::string kind=settings ? "Settings" : "Input";
    require(value.is_string(),kind+" profile path must be a string.");
    const auto text=value.get<std::string>();
    require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid "+kind+" profile path.");
    auto raw=fs::path(std::u8string(text.begin(),text.end()));
    if(raw.is_relative())raw=paths.world.parent_path()/raw;
    for(const auto* suffix:{"", ".lock", ".pending", ".previous", ".previous.pending"})
        require(!fs::is_symlink(fs::path(raw).concat(suffix)),kind+" profile paths cannot be symbolic links.");
    if(settings) {
        for(auto parent=raw.parent_path();!parent.empty();parent=parent.parent_path()) {
            require(!fs::is_symlink(parent),"Settings profile parent directories cannot be symbolic links.");
            if(parent==parent.parent_path())break;
        }
    }
    const auto output=fs::weakly_canonical(fs::absolute(raw));
    if(settings && paths.read_only)require(!path_components_contain(output,paths.protected_root),"Player settings must be outside the immutable game bundle.");
    for(const auto* suffix:{"", ".lock", ".pending", ".previous", ".previous.pending"}) {
        const auto candidate=fs::path(output).concat(suffix);
        const auto relative=candidate.lexically_relative(fs::weakly_canonical(paths.assets));
        require(relative.empty() || relative.is_absolute() || *relative.begin()=="..",kind+" profile cannot use the immutable asset store.");
        for(const auto* reserved:{"", ".lock", ".pending", ".previous", ".previous.pending"}) {
            const auto world=fs::path(paths.world).concat(reserved);
            require(!same_path_name(candidate,world),kind+" profile path is reserved by the world service.");
        }
    }
    return output;
}

Json dispatch_input_profile(const std::string& method,const Json& params,const PreferencePaths& paths,
    std::shared_ptr<GamepadHost>& gamepad_host) {
    if(method=="input.describe") { fields(params,{});return input_profiles::describe(); }
    if(method=="input.devices") {
        fields(params,{});
        if(!GamepadHost::available())return {{"available",false},{"devices",Json::array()},{"detail","SDL gamepad device host is not built."}};
        try {
            if(!gamepad_host)gamepad_host=std::make_shared<GamepadHost>();
            auto result=Json::parse(gamepad_host->devices_json());
            result["available"]=true;
            return result;
        }catch(const std::exception& e) { throw Error(-32071,e.what()); }
    }
    if(method=="input.evaluate") {
        fields(params,{"path","events","gamepad_defaults"},{"events"});
        require(!params.contains("gamepad_defaults") || params.at("gamepad_defaults").is_boolean(),"gamepad_defaults must be Boolean.");
        require(!params.contains("path") || !params.contains("gamepad_defaults"),"path and gamepad_defaults are mutually exclusive.");
        const auto& events=params.at("events");require(events.is_array() && events.size()<=256,"Input evaluation takes at most 256 events.");
        auto profile=params.value("gamepad_defaults",false) ? default_gamepad_input_profile() : default_input_profile();
        Json info={{"source","defaults"},{"revision",0},{"content_hash",nullptr}};
        if(params.contains("path"))try {
            const auto file=resolve_preference_profile_path(params.at("path"),paths,PreferenceKind::input);
            auto loaded=paths.read_only ? input_profiles::load_read_only(file) : input_profiles::load(file);
            profile=std::move(loaded.profile);
            info={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash}};
        }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
        const bool has_gamepad=profile.gamepad.has_value();
        info["format"]=input_profiles::profile_format(profile);
        BoundPlayerInput evaluator(std::move(profile));Json frames=Json::array();
        for(std::size_t i=0;i<events.size();++i) {
            try {
            const auto& event=events[i];require(event.is_object(),"Input event must be an object.");
            if(event.contains("gamepad_connect")) {
                fields(event,{"gamepad_connect"},{"gamepad_connect"});require(has_gamepad,"Gamepad events require a v2 profile.");
                const auto& state=event.at("gamepad_connect");fields(state,{"axes","buttons"});std::array<std::int16_t,6> axes{};std::uint32_t buttons=0;
                if(state.contains("axes")) {
                    const auto& values=state.at("axes");require(values.is_array() && values.size()==6,"Gamepad snapshot requires six axes.");
                    for(std::size_t axis=0;axis<6;++axis) { const auto& v=values[axis];require(v.is_number_integer() && v>=(axis<4 ? -32768 : 0) && v<=32767,"Invalid gamepad snapshot axis.");axes[axis]=v.get<std::int16_t>(); }
                }
                if(state.contains("buttons")) { const auto value=revision(state.at("buttons"));require(value<=4294967295ULL,"Gamepad buttons must be a uint32 mask.");buttons=static_cast<std::uint32_t>(value); }
                evaluator.gamepad_connect(axes,buttons);
            }else if(event.contains("gamepad_axis")) {
                fields(event,{"gamepad_axis"},{"gamepad_axis"});require(has_gamepad,"Gamepad events require a v2 profile.");const auto& state=event.at("gamepad_axis");fields(state,{"axis","value"},{"axis","value"});
                const std::array<const char*,6> names{"left_x","left_y","right_x","right_y","left_trigger","right_trigger"};
                const auto it=std::find_if(names.begin(),names.end(),[&](const auto* name) { return state.at("axis")==name; });require(it!=names.end(),"Unknown gamepad axis.");
                const auto axis=static_cast<std::uint16_t>(it-names.begin());const auto& value=state.at("value");require(value.is_number_integer() && value>=(axis<4 ? -32768 : 0) && value<=32767,"Invalid gamepad axis value.");
                evaluator.gamepad_axis(axis,value.get<std::int16_t>());
            }else if(event.contains("gamepad_button")) {
                fields(event,{"gamepad_button"},{"gamepad_button"});require(has_gamepad,"Gamepad events require a v2 profile.");const auto& state=event.at("gamepad_button");fields(state,{"control","down"},{"control","down"});
                require(state.at("control").is_string() && state.at("down").is_boolean(),"Invalid raw gamepad button event.");const auto id=state.at("control").get<std::string>();const auto controls=input_controls();
                const auto it=std::find_if(controls.begin(),controls.end(),[&](const auto& c) { return c.id==id && c.kind==InputControlKind::gamepad_button && c.code<26; });
                require(it!=controls.end(),"Raw gamepad buttons require a physical gamepad control; use axis events for triggers.");evaluator.gamepad_button(it->code,state.at("down").get<bool>());
            }else if(event.contains("gamepad_disconnect")) {
                fields(event,{"gamepad_disconnect"},{"gamepad_disconnect"});require(has_gamepad && event.at("gamepad_disconnect").is_boolean() && event.at("gamepad_disconnect")==true,"gamepad_disconnect requires true and a v2 profile.");evaluator.gamepad_disconnect();
            }else if(event.contains("control")) {
                fields(event,{"control","down"},{"control","down"});require(event.at("control").is_string() && event.at("down").is_boolean(),"Invalid input control event.");
                const auto id=event.at("control").get<std::string>();const auto controls=input_controls();
                const auto found=std::find_if(controls.begin(),controls.end(),[&](const auto& c) { return c.id==id; });
                require(found!=controls.end() && !found->reserved,"Unknown or reserved gameplay control.");
                require(found->kind!=InputControlKind::gamepad_button || has_gamepad,"Gamepad events require a v2 profile.");
                evaluator.control(found->kind,found->code,event.at("down").get<bool>());
            }else if(event.contains("motion")) {
                fields(event,{"motion"},{"motion"});const auto& motion=event.at("motion");require(motion.is_array() && motion.size()==2,"Mouse motion needs two values.");
                for(const auto& v:motion)require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=1e6,"Mouse motion is out of bounds.");
                evaluator.motion(motion[0].get<double>(),motion[1].get<double>());
            }else if(event.contains("consume")) {
                fields(event,{"consume"},{"consume"});require(event.at("consume").is_boolean() && event.at("consume")==Json(true),"consume must be true.");
                const auto frame=evaluator.consume("");Json observed={{"event",i},{"move",frame.move},{"look",frame.look},{"jump",frame.jump},{"use",frame.use},{"connected",evaluator.gamepad_connected()},{"armed",evaluator.gamepad_armed()}};
                if(!frame.actions.empty()) {
                    observed["actions"]=Json::array();for(const auto& action:frame.actions)
                        observed["actions"].push_back({{"name",action.name},{"held",action.held},{"pressed",action.pressed},{"released",action.released}});
                }
                frames.push_back(std::move(observed));
            }else {
                fields(event,{"clear"},{"clear"});require(event.at("clear").is_boolean() && event.at("clear")==Json(true),"clear must be true.");evaluator.clear();
            }
            }catch(const std::invalid_argument& e) { throw Error(-32602,e.what()); }
        }
        return {{"input_profile",info},{"frames",frames},{"gamepad",{{"connected",evaluator.gamepad_connected()},{"armed",evaluator.gamepad_armed()}}}};
    }
    require(method=="input.inspect" || method=="input.transact","Unknown input method.",-32601);
    bool inspect_read_only=paths.read_only;
    if(method=="input.inspect") {
        fields(params,{"path","read_only"},{"path"});
        if(params.contains("read_only")) {
            require(params.at("read_only").is_boolean(),"read_only must be boolean.");
            inspect_read_only=paths.read_only || params.at("read_only").get<bool>();
        }
    }
    else fields(params,{"path","request_id","expected_revision","profile","preview"},{"path","request_id","expected_revision","profile"});
    const auto file=resolve_preference_profile_path(params.at("path"),paths,PreferenceKind::input);
    try {
        if(method=="input.inspect")return inspect_read_only ? input_profiles::inspect_read_only(file) : input_profiles::inspect(file);
        auto operation=params;operation.erase("path");return input_profiles::transact(file,operation);
    }catch(const input_profiles::ProfileError& e) { throw Error(e.code,e.what()); }
}

Json dispatch_saved_player_settings(const std::string& method,const Json& params,const PreferencePaths& paths) {
    if(method=="settings.describe") { fields(params,{});return player_settings::describe(); }
    require(method=="settings.inspect" || method=="settings.transact","Unknown settings method.",-32601);
    if(method=="settings.inspect")fields(params,{"path"},{"path"});
    else fields(params,{"path","request_id","expected_revision","set","reset","preview"},{"path","request_id","expected_revision"});
    const auto file=resolve_preference_profile_path(params.at("path"),paths,PreferenceKind::settings);
    try {
        if(method=="settings.inspect")return paths.read_only ? player_settings::inspect_read_only(file) : player_settings::inspect(file);
        auto operation=params;operation.erase("path");return player_settings::transact(file,operation);
    }catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
}

Json apply_player_settings(const Json& params,PlayerOptions& options,const PreferencePaths& paths,
    const Runtime& runtime) {
    const bool requested=params.contains("settings_profile") || params.contains("settings_overrides") || params.contains("settings_revision");
    require(!params.contains("settings_revision") || params.contains("settings_profile"),"settings_revision requires settings_profile.");
    PlayerPreferences::Values inherited;
    inherited.sensitivity_x=options.input_profile->sensitivity_x;
    inherited.sensitivity_y=options.input_profile->sensitivity_y;
    inherited.invert_x=options.input_profile->invert_x;
    inherited.invert_y=options.input_profile->invert_y;
    // Explicit launch graphics options are the baseline for a reset. The
    // final combined profile/override renderer is validated below.
    inherited.samples=params.value("samples",4u);
    inherited.frames_in_flight=params.value("frames_in_flight",2u);
    Json values=Json::object(),sources=Json::object();
    Json profile={{"source","none"},{"revision",0},{"content_hash",nullptr}};
    try {
        if(params.contains("settings_profile")) {
            const auto path=resolve_preference_profile_path(params.at("settings_profile"),paths,PreferenceKind::settings);
            auto loaded=paths.read_only ? player_settings::load_read_only(path) : player_settings::load(path);
            if(params.contains("settings_revision"))require(revision(params.at("settings_revision"))==loaded.revision,"Settings profile revision conflict.",-32009);
            values=std::move(loaded.values);
            profile={{"source","profile"},{"revision",loaded.revision},{"content_hash",loaded.content_hash}};
            for(auto it=values.begin();it!=values.end();++it)sources[it.key()]="profile";
        }
        if(params.contains("settings_overrides")) {
            const auto overrides=player_settings::validate_values(params.at("settings_overrides"));
            for(auto it=overrides.begin();it!=overrides.end();++it) { values[it.key()]=it.value();sources[it.key()]="session_override"; }
        }
    }catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
    // Explicit existing launch options remain last in the precedence chain.
    for(const auto* key:{"samples","frames_in_flight"})if(params.contains(key)) {
        const std::string setting=std::string("graphics.")+key;
        auto single=Json::object();single[setting]=params.at(key);
        try { values[setting]=player_settings::validate_values(single).at(setting); }
        catch(const player_settings::SettingsError& error) { throw Error(error.code,error.what()); }
        sources[setting]="explicit_option";
    }
    auto rendering=params;
    for(const auto* key:{"samples","frames_in_flight"}) {
        const std::string setting=std::string("graphics.")+key;
        if(values.contains(setting))rendering[key]=values.at(setting);
    }
    options.render=decode_render_options(rendering,paths.world,paths.assets);
    InputProfile input=*options.input_profile;
    if(values.contains("input.sensitivity_x"))input.sensitivity_x=values.at("input.sensitivity_x").get<double>();
    if(values.contains("input.sensitivity_y"))input.sensitivity_y=values.at("input.sensitivity_y").get<double>();
    if(values.contains("input.invert_x"))input.invert_x=values.at("input.invert_x").get<bool>();
    if(values.contains("input.invert_y"))input.invert_y=values.at("input.invert_y").get<bool>();
    validate_input_profile(input);
    options.input_profile=std::make_shared<const InputProfile>(std::move(input));
    if(values.contains("camera.vertical_fov"))options.vertical_fov=values.at("camera.vertical_fov").get<double>();
    if(values.contains("ui.scale"))options.ui_scale=values.at("ui.scale").get<float>();
    try { options.preferences=std::make_shared<PlayerPreferences>(inherited,values.dump(),sources.dump(),profile.dump()); }
    catch(const PlayerPreferences::Error& error) { throw Error(error.code,error.what()); }
    if(!requested)return nullptr;
    const auto camera=runtime.snapshot(options.camera);
    Json effective={{"camera.vertical_fov",player_effective_vertical_fov(camera.vertical_fov,camera.authored_vertical_fov,options.vertical_fov)},
        {"input.sensitivity_x",options.input_profile->sensitivity_x},{"input.sensitivity_y",options.input_profile->sensitivity_y},
        {"input.invert_x",options.input_profile->invert_x},{"input.invert_y",options.input_profile->invert_y},
        {"ui.scale",options.ui_scale ? Json(*options.ui_scale) : Json(nullptr)},
        {"graphics.samples",options.render.samples},{"graphics.frames_in_flight",options.render.frames_in_flight},
        {"audio.master_gain",options.preferences->snapshot()->values.master_gain}};
    Json rows=Json::object();
    for(auto it=effective.begin();it!=effective.end();++it) {
        const auto& key=it.key();
        const std::string inherited_source=key.starts_with("input.") ? "input_profile" : key=="camera.vertical_fov" ? "authored_camera" : key=="ui.scale" ? "window_density" : "engine_default";
        rows[key]={{"requested",values.contains(key) ? values.at(key) : Json(nullptr)},
            {"effective",it.value()},{"source",sources.contains(key) ? sources.at(key) : Json(inherited_source)},{"outcome","not_presented"}};
    }
    return {{"format","poima.settings.application.v1"},{"application","next_player"},{"profile",profile},{"fields",rows}};
}

Json player_live_settings(const PlayerOptions& options,const PlayerReport& report,
    const std::optional<RuntimeCameraState>& final_camera) {
    if(!options.preferences)return nullptr;
    auto result=Json::parse(options.preferences->inspect());
    const auto state=options.preferences->snapshot();
    const auto& observed=report.preferences;
    auto optional=[](const auto& value) -> Json { return value ? Json(*value) : Json(nullptr); };
    result["fields"]["camera.vertical_fov"]["effective"]=final_camera ?
        Json(player_effective_vertical_fov(final_camera->vertical_fov,final_camera->authored_vertical_fov,state->values.vertical_fov)) :
        state->values.vertical_fov ? Json(*state->values.vertical_fov) : Json(nullptr);
    result["fields"]["ui.scale"]["effective"]=state->values.ui_scale ? Json(*state->values.ui_scale) :
        observed.applied_revision ? Json(report.effective_ui_scale) : Json(nullptr);
    result["application"]={{"observed_revision",optional(observed.observed_revision)},
        {"applied_revision",optional(observed.applied_revision)},{"presented_revision",optional(observed.presented_revision)},
        {"effective_vertical_fov",optional(observed.effective_vertical_fov)},
        {"effective_ui_scale",observed.applied_revision ? Json(report.effective_ui_scale) : Json(nullptr)},
        {"sensitivity_x",observed.sensitivity_x},{"sensitivity_y",observed.sensitivity_y},
        {"invert_x",observed.invert_x},{"invert_y",observed.invert_y},
        {"requested_master_gain",observed.requested_master_gain},{"sink_gain",optional(observed.sink_gain)},
        {"audio_outcome",observed.audio_outcome}};
    const bool presented=observed.presented_revision && *observed.presented_revision==state->revision;
    for(auto& field:result["fields"].items()) {
        auto& row=field.value();const auto& key=field.key();
        row["outcome"]=key.starts_with("graphics.") ? (row.at("requires_next_player").get<bool>() ? "requires_next_player" : "unchanged") :
            key=="audio.master_gain" ? observed.audio_outcome :
            (options.replay || options.background) && key.starts_with("input.") ? "validated_only" : presented ? "applied" : "awaiting_presentation";
    }
    return result;
}

void finish_player_settings(Json& application,const PlayerOptions& options,const PlayerReport& report,const std::optional<RuntimeCameraState>& final_camera) {
    if(options.preferences) {
        const auto current=player_live_settings(options,report,final_camera);
        application["fields"]=current.at("fields");
        if(options.preferences->snapshot()->revision) application["application"]="live_player";
        else {
            // Preserve the existing launch application report. The live
            // channel has separate next-player/application observations;
            // adding it must not change older successful-launch outcomes.
            for(auto& field:application["fields"].items())if(field.key()!="audio.master_gain")
                field.value()["outcome"]=(options.replay || options.background) && field.key().starts_with("input.") ? "validated_only" :
                    report.render.frames_presented>0 ? "applied" : "not_presented";
        }
        return;
    }
    // Report final configured owner state; an error may have prevented its
    // last presentation. Never retain a replaced camera's initial lens.
    application["fields"]["camera.vertical_fov"]["effective"]=final_camera ?
        Json(player_effective_vertical_fov(final_camera->vertical_fov,final_camera->authored_vertical_fov,options.vertical_fov)) :
        options.vertical_fov ? Json(*options.vertical_fov) : Json(nullptr);
    application["fields"]["ui.scale"]["effective"]=report.effective_ui_scale;
    for(auto it=application["fields"].begin();it!=application["fields"].end();++it)
        it.value()["outcome"]=((options.replay || options.background) && it.key().starts_with("input.")) ? "validated_only" : report.render.frames_presented>0 ? "applied" : "not_presented";
}

} // namespace poima::world_detail
