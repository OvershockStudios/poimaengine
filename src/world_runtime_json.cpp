// SPDX-License-Identifier: Apache-2.0
#include "world_runtime_json.hpp"

#include "world_document.hpp"
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace poima::world_detail {

RuntimeInput parse_input(const Json& i) {
    fields(i,{"entity","move","look","jump","use","actions"},{"entity"}); RuntimeInput input; input.entity=identifier(i.at("entity"));
    for(const auto* key:{"move","look"}) if(i.contains(key)) {
        const auto& array=i.at(key); require(array.is_array() && array.size()==2,"Runtime input vector needs two numbers.");
        const double bound=std::string_view(key)=="move" ? 1 : 180;
        for(const auto& v:array) require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=bound,"Runtime input number out of range.");
        if(std::string_view(key)=="move") input.move=array.get<std::array<float,2>>(); else input.look=array.get<std::array<float,2>>();
    }
    if(i.contains("jump")) { require(i.at("jump").is_boolean(),"Jump must be boolean."); input.jump=i.at("jump"); }
    if(i.contains("use")) { require(i.at("use").is_boolean(),"Use must be boolean.");input.use=i.at("use"); }
    if(i.contains("actions")) {
        const auto& actions=i.at("actions");require(actions.is_array() && actions.size()<=input_action_limit,"Input permits at most 32 named actions.");
        std::string previous;
        for(const auto& value:actions) {
            fields(value,{"name","held","pressed","released"},{"name","held","pressed","released"});
            require(value.at("name").is_string(),"Input action name must be a string.");
            const auto name=value.at("name").get<std::string>();
            require(valid_input_action_name(name) && (previous.empty() || previous<name),"Input action names must be valid, sorted and unique.");
            for(const auto* key:{"held","pressed","released"})require(value.at(key).is_boolean(),"Input action states must be Boolean.");
            input.actions.push_back({name,value.at("held").get<bool>(),value.at("pressed").get<bool>(),value.at("released").get<bool>()});previous=name;
        }
    }
    return input;
}
Json motion_json(const KinematicTarget& m) {
    return {{"entity",m.entity},{"position",m.position},{"rotation",m.rotation},{"duration_ticks",m.duration_ticks}};
}
Json animation_json(const RuntimeAnimationState& state) {
    Json transition=nullptr;
    if(state.transition) {
        const auto& t=*state.transition;
        transition={{"start_tick",t.start_tick},{"duration_ticks",t.duration_ticks},{"elapsed_ticks",t.elapsed_ticks},
            {"weight",t.weight},{"source_frozen",t.source_frozen},
            {"source_clip",!t.source_frozen && t.source_clip ? Json(*t.source_clip) : Json(nullptr)},
            {"source_time",t.source_frozen ? Json(nullptr) : Json(t.source_time)},
            {"source_speed",t.source_frozen ? Json(nullptr) : Json(t.source_speed)},
            {"source_loop",t.source_frozen ? Json(nullptr) : Json(t.source_loop)},
            {"source_playing",t.source_frozen ? Json(nullptr) : Json(t.source_playing)}};
        if(t.mode==AnimationTransitionMode::Inertial)transition["mode"]="inertial";
    }
    Json result={{"clip",state.clip ? Json(*state.clip) : Json(nullptr)},{"time",state.time},{"speed",state.speed},
        {"loop",state.loop},{"playing",state.playing},{"duration",state.duration},{"transition",transition}};
    if(!state.layers.empty()) {
        result["layers"]=Json::array();
        for(const auto& layer:state.layers) {
            RuntimeAnimationState playback;playback.clip=layer.clip;playback.time=layer.time;playback.speed=layer.speed;playback.loop=layer.loop;playback.playing=layer.playing;playback.duration=layer.duration;playback.transition=layer.transition;
            Json weight_transition=nullptr;
            if(layer.weight_transition) { const auto& fade=*layer.weight_transition;weight_transition={{"start_tick",fade.start_tick},{"duration_ticks",fade.duration_ticks},{"elapsed_ticks",fade.elapsed_ticks},{"source",fade.source},{"target",fade.target}}; }
            result["layers"].push_back({{"slot",layer.slot},{"mode",layer.mode==AnimationLayerMode::Additive ? "additive" : "override"},
                {"weight",layer.weight},{"target_weight",layer.target_weight},{"weight_transition",weight_transition},{"mask_nodes",layer.mask_nodes},{"playback",animation_json(playback)}});
        }
    }
    return result;
}
std::vector<AnimationCommand> parse_animations(const Json& raw,bool allow_phase_sync) {
    require(raw.is_array() && raw.size()<=64,"Animations must be an array of at most 64 complete playback commands.");
    std::vector<AnimationCommand> result;std::set<std::pair<std::string,std::uint32_t>> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","clip","time","speed","loop","playing","blend_ticks","transition_mode","layer","weight","weight_blend_ticks","phase_sync"},{"entity","clip","time","speed","loop","playing"});
        auto command=animation_value(value);command.entity=identifier(value.at("entity"));
        if(value.contains("phase_sync")) {
            require(allow_phase_sync,"Use animation_phase_matches for a phase-matched ordinary command.");
            const auto& sync=value.at("phase_sync");
            fields(sync,{"source","destination"},{"source","destination"});
            const auto read_markers=[](const Json& markers) {
                require(markers.is_array()&&markers.size()>=2&&markers.size()<=max_animation_phase_markers,
                    "Animation phase matching needs two to eight markers per clip.");
                std::vector<double> times;
                times.reserve(markers.size());
                for(const auto& marker:markers) {
                    require(marker.is_number()&&std::isfinite(marker.get<double>())&&marker>=0,
                        "Animation phase markers must be finite nonnegative seconds.");
                    times.push_back(marker.get<double>());
                }
                return times;
            };
            command.phase_sync=AnimationPhaseSync{read_markers(sync.at("source")),read_markers(sync.at("destination"))};
            require(command.phase_sync->source.size()==command.phase_sync->destination.size()&&
                command.clip&&command.loop&&command.time==0,
                "Animation phase matching needs paired markers and a looping destination clip at time zero.");
        }
        if(value.contains("blend_ticks")) {
            const auto ticks=revision(value.at("blend_ticks"));require(ticks<=3600,"Animation blend_ticks must be within 0..3600.");
            command.blend_ticks=static_cast<std::uint32_t>(ticks);
        }
        if(value.contains("transition_mode")) {
            const auto& mode=value.at("transition_mode");
            require(mode.is_string() && (mode=="crossfade" || mode=="inertial"),"Animation transition_mode must be crossfade or inertial.");
            if(mode=="inertial")command.transition_mode=AnimationTransitionMode::Inertial;
        }
        if(value.contains("layer")) {
            const auto slot=revision(value.at("layer"));require(slot>=1 && slot<=4,"Animation command layer must be1..4.");command.layer=static_cast<std::uint32_t>(slot);
            require(value.contains("weight") && value.at("weight").is_number() && std::isfinite(value.at("weight").get<double>()) && value.at("weight")>=0 && value.at("weight")<=1,"Animation layer command requires weight0..1.");command.weight=value.at("weight").get<double>();
            if(value.contains("weight_blend_ticks")) { const auto ticks=revision(value.at("weight_blend_ticks"));require(ticks<=3600,"Animation weight blend duration must be0..3600.");command.weight_blend_ticks=static_cast<std::uint32_t>(ticks); }
        } else require(!value.contains("weight") && !value.contains("weight_blend_ticks"),"Base animation commands do not accept layer weight fields.");
        require(seen.emplace(command.entity,command.layer.value_or(0)).second,"Duplicate animation command target.");result.push_back(std::move(command));
    }
    return result;
}
void normalize_animation_receipts(Json& animations) {
    if(!animations.is_array())return;
    for(auto& command:animations)if(command.is_object()) {
        if(!command.contains("blend_ticks"))command["blend_ticks"]=0;
        // Validate integer fields before JSON receipt comparison, where 0 and 0.0 compare equal.
        require(revision(command.at("blend_ticks"))<=3600,"Animation blend_ticks must be within 0..3600.");
        if(command.contains("layer")) {
            const auto layer=revision(command.at("layer"));require(layer>=1 && layer<=4,"Animation command layer must be1..4.");
            if(!command.contains("weight_blend_ticks"))command["weight_blend_ticks"]=0;
            require(revision(command.at("weight_blend_ticks"))<=3600,"Animation weight blend duration must be0..3600.");
        }
        if(!command.contains("transition_mode"))command["transition_mode"]="crossfade";
        require(command.at("transition_mode").is_string() && (command.at("transition_mode")=="crossfade" || command.at("transition_mode")=="inertial"),"Animation transition_mode must be crossfade or inertial.");
    }
}
std::vector<AnimationRateCommand> parse_animation_rates(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Animation rates must be an array of at most64 commands.");
    std::vector<AnimationRateCommand> result;std::set<std::pair<std::string,std::uint32_t>> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","speed","layer"},{"entity","speed"});
        AnimationRateCommand command;command.entity=identifier(value.at("entity"));
        const auto& speed=value.at("speed");
        require(speed.is_number() && std::isfinite(speed.get<double>()) && speed>=0 && speed<=8,
            "Animation rate speed must be finite and within0..8.");command.speed=speed.get<double>();
        if(value.contains("layer")) {
            const auto slot=revision(value.at("layer"));require(slot>=1 && slot<=4,"Animation rate layer must be1..4.");
            command.layer=static_cast<std::uint32_t>(slot);
        }
        require(seen.emplace(command.entity,command.layer.value_or(0)).second,"Duplicate animation rate target.");
        result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeIkCommand> parse_ik(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"IK must contain at most64 complete commands.");
    std::vector<RuntimeIkCommand> result;std::set<std::pair<std::string,std::uint32_t>> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","slot","target","pole","enabled","space","weight","end_orientation"},{"entity","slot","target","pole"});
        auto command=ik_control_value(value);command.entity=identifier(value.at("entity"));
        require(seen.emplace(command.entity,command.slot).second,"Duplicate IK command target.");result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeRootMotionCommand> parse_root_motion(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Root motion must contain at most64 complete commands.");
    std::vector<RuntimeRootMotionCommand> result;std::set<std::string> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","enabled"},{"entity"});RuntimeRootMotionCommand command;command.entity=identifier(value.at("entity"));
        require(seen.insert(command.entity).second,"Duplicate root-motion command target.");
        if(value.contains("enabled")){require(value.at("enabled").is_boolean(),"Root-motion enabled must be boolean.");command.enabled=value.at("enabled").get<bool>();}
        result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeGroundingCommand> parse_grounding(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Grounding must contain at most 64 commands.");
    std::vector<RuntimeGroundingCommand> result;std::set<std::pair<std::string,std::uint32_t>> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","slot","enabled","weight"},{"entity","slot","enabled","weight"});
        RuntimeGroundingCommand command;command.entity=identifier(value.at("entity"));
        const auto slot=revision(value.at("slot"));require(slot>=1 && slot<=8,"Grounding slot must be 1..8.");
        command.slot=static_cast<std::uint32_t>(slot);
        require(value.at("enabled").is_boolean(),"Grounding enabled must be boolean.");
        require(value.at("weight").is_number(),"Grounding weight must be numeric.");
        command.enabled=value.at("enabled").get<bool>();command.weight=value.at("weight").get<double>();
        require(std::isfinite(command.weight) && command.weight>=0 && command.weight<=1,"Grounding weight must be finite in 0..1.");
        require(seen.emplace(command.entity,command.slot).second,"Duplicate grounding control target.");
        result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeCharacterStanceCommand> parse_character_stances(const Json& raw) {
    require(raw.is_array() && raw.size()<=32,"Character stances must contain at most 32 commands.");
    std::vector<RuntimeCharacterStanceCommand> result;std::set<std::string> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","crouch"},{"entity","crouch"});
        RuntimeCharacterStanceCommand command;command.entity=identifier(value.at("entity"));
        require(value.at("crouch").is_boolean(),"Character stance crouch must be boolean.");
        require(seen.insert(command.entity).second,"Duplicate character stance target.");
        command.crouch=value.at("crouch").get<bool>();result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeCameraLensCommand> parse_camera_lenses(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Camera lenses must contain at most 64 commands.");
    std::vector<RuntimeCameraLensCommand> result;std::set<std::string> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","world","viewmodel"},{"entity"});
        RuntimeCameraLensCommand command;command.entity=identifier(value.at("entity"));
        require(seen.insert(command.entity).second,"Duplicate camera lens command target.");
        require(value.contains("world") || value.contains("viewmodel"),"Camera lens command needs world or viewmodel.");
        if(value.contains("world"))command.world=scene_lens_value(value.at("world"));
        if(value.contains("viewmodel"))command.viewmodel=scene_lens_value(value.at("viewmodel"));
        result.push_back(std::move(command));
    }
    return result;
}
std::vector<RuntimeCameraPoseCommand> parse_camera_poses(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Camera poses must contain at most64 commands.");
    std::vector<RuntimeCameraPoseCommand> result;std::set<std::string> targets;
    const auto pose=[](const Json& value) {
        fields(value,{"position","rotation"},{"position","rotation"});
        validate_transform({{"position",value.at("position")},{"rotation",value.at("rotation")},{"scale",{1,1,1}}});
        RuntimeCameraPose result;result.position=value.at("position").get<std::array<double,3>>();result.rotation=value.at("rotation").get<std::array<double,4>>();
        for(double v:result.position)require(std::abs(v)<=10,"Camera pose translation exceeds10 metres per axis.");return result;
    };
    for(const auto& value:raw) {
        fields(value,{"entity","world","viewmodel"},{"entity"});RuntimeCameraPoseCommand command;command.entity=identifier(value.at("entity"));
        require(targets.insert(command.entity).second,"Duplicate camera pose target.");
        require(value.contains("world") || value.contains("viewmodel"),"Camera pose command requires world or viewmodel.");
        if(value.contains("world"))command.world=pose(value.at("world"));if(value.contains("viewmodel"))command.viewmodel=pose(value.at("viewmodel"));result.push_back(std::move(command));
    }
    return result;
}
std::array<double,3> query_vector(const Json& v) {
    require(v.is_array() && v.size()==3,"Expected a three-element query vector.");
    for(const auto& x:v)require(x.is_number() && std::isfinite(x.get<double>()) && std::abs(x.get<double>())<=1e6,"Query vector must be finite and within 1e6.");
    return v.get<std::array<double,3>>();
}
std::vector<KinematicTarget> parse_motions(const Json& raw) {
    require(raw.is_array() && raw.size()<=128,"Motions must be an array of at most 128 targets.");
    std::vector<KinematicTarget> result;std::set<std::string> seen;
    for(const auto& value:raw) {
        fields(value,{"entity","position","rotation","duration_ticks"},{"entity","position","rotation","duration_ticks"});
        KinematicTarget m;m.entity=identifier(value.at("entity"));require(seen.insert(m.entity).second,"Duplicate kinematic target entity.");
        m.position=query_vector(value.at("position"));
        validate_transform({{"position",m.position},{"rotation",value.at("rotation")},{"scale",{1,1,1}}});
        m.rotation=value.at("rotation").get<std::array<double,4>>();
        const auto ticks=revision(value.at("duration_ticks"));require(ticks>=1 && ticks<=36000,"Motion duration must be 1..36000 ticks.");m.duration_ticks=static_cast<std::uint32_t>(ticks);
        result.push_back(std::move(m));
    }
    return result;
}
std::vector<SoundCommand> parse_sounds(const Json& raw) {
    require(raw.is_array() && raw.size()<=64,"Sounds must be an array of at most 64 commands.");
    std::vector<SoundCommand> result;
    for(const auto& value:raw) {
        require(value.is_object() && value.contains("op"),"Sound command needs op.");SoundCommand command;
        if(value.at("op")=="play") {
            fields(value,{"op","emitter","gain"},{"op","emitter"});command.emitter=identifier(value.at("emitter"));
            const auto gain=value.value("gain",Json(1));require(gain.is_number() && std::isfinite(gain.get<double>()) && gain.get<double>()>=0 && gain.get<double>()<=4,"Sound gain must be in [0,4].");command.gain=gain.get<float>();
        } else {
            require(value.at("op")=="stop","Sound op must be play or stop.");fields(value,{"op","voice"},{"op","voice"});command.stop=true;command.voice=revision(value.at("voice"));require(command.voice>0,"Sound voice must be positive.");
        }
        result.push_back(std::move(command));
    }
    return result;
}

} // namespace poima::world_detail
