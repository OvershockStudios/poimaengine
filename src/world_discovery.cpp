// SPDX-License-Identifier: Apache-2.0
#include "world_discovery.hpp"

#include "discovery_selection.hpp"
#include "mutation_discovery.hpp"
#include "poima/build_metadata.hpp"
#include "poima/gameplay.hpp"
#include "poima/runtime.hpp"
#include "poima/world.hpp"
#include <optional>
#include <set>
#include <utility>

namespace poima::world_detail {

std::string discovery_view(const Json& params) {
    fields(params,{"view","name","operation","type"});
    const auto view=params.value("view",std::string("full"));
    require(view=="full" || view=="catalog" || view=="method" || view=="component" || view=="section" || view=="mutation","Unknown discovery view.");
    const bool named=view=="method" || view=="component" || view=="section";
    require(params.contains("name")==named,"Discovery name is required only for method, component or section views.");
    if(named)require(params.at("name").is_string() && !params.at("name").get_ref<const std::string&>().empty(),"Discovery name must be a nonempty string.");
    require(params.contains("operation")== (view=="mutation"),"Discovery operation is required only for the mutation view.");
    require(view=="mutation" || !params.contains("type"),"Discovery type applies only to the mutation view.");
    if(view=="mutation") {
        for(const auto* key:{"operation","type"})if(params.contains(key))
            require(params.at(key).is_string() && !params.at(key).get_ref<const std::string&>().empty() &&
                    params.at(key).get_ref<const std::string&>().size()<=128,"Mutation selectors must contain 1..128 UTF-8 bytes.");
        if(params.contains("type"))require(params.at("operation")=="component.set" || params.at("operation")=="component.remove","Discovery type applies only to component.set/remove.");
    }
    return view;
}
void validate_discovery_selection(const Json& params) {
    fields(params,{"methods","components","sections","mutations"});
    std::size_t count=0;
    for(const auto& [field,items]:params.items()) {
        require(items.is_array() && !items.empty() && items.size()<=discovery_detail::selection_limit-count,
            "Discovery selection requires 1..12 total entries in nonempty arrays.");
        count+=items.size();
        std::set<Json> seen;
        for(const auto& item:items) {
            if(field=="mutations") {
                fields(item,{"operation","type"},{"operation"});
                auto selector=item;
                selector["view"]="mutation";
                (void)discovery_view(selector);
            } else {
                require(item.is_string() && !item.get_ref<const std::string&>().empty() &&
                    item.get_ref<const std::string&>().size()<=discovery_detail::selection_name_bytes,
                    "Discovery names must contain 1..128 UTF-8 bytes.");
            }
            require(seen.insert(item).second,"Duplicate discovery selector in "+field+".");
        }
    }
    require(count>0,"Discovery selection requires at least one entry.");
}
Json authoring_contract_identity(bool read_only) {
    // This identifies the selected source-46 authoring inputs and response
    // contract. It does not freeze the complete evolving method catalog.
    return {{"id","poima.authoring-core"},{"version",1},{"status","stable"},
        {"scope","authored-world"},{"request_baseline","authoring-core-v1.json"},
        {"response_baseline","authoring-core-responses-v1.json"},
        {"methods",{"entity.get","entity.query","entity.world_transform","world.describe",
                    "world.history","world.inspect","world.redo","world.transact","world.undo"}},
        {"components",{"Transform"}},
        {"mutations",{"component.set","entity.create","entity.delete","entity.rename","entity.reparent"}},
        {"availability",{{"mode",read_only ? "read_only_runtime" : "authoring"},{"mutations",!read_only}}}};
}
Json runtime_game_session_contract_identity(bool read_only,WorldRequestScope scope) {
    const auto& build=build_metadata();
    const bool simulation=Runtime::available();
    const bool native_gameplay=Gameplay::native_available(),managed_gameplay=Gameplay::available();
    const bool implemented=simulation && (native_gameplay || managed_gameplay);
    const bool target=std::string_view(build.target_os)=="Windows" && std::string_view(build.target_arch)=="x86_64";
    Json profile=nullptr;
    if(scope==WorldRequestScope::standalone && !read_only)profile="source_world";
    else if(scope==WorldRequestScope::shared_headless)profile=read_only ? "readonly_game" : "source_shared_player";
    // The exported-game profile admits Native AOT images. A CoreCLR-only
    // source host must not imply qualification of that deployment context.
    const bool profile_backend=!read_only || native_gameplay;
    const bool qualified=implemented && target && !profile.is_null() && profile_backend;
    const char* reason=!implemented ? "required_backend_not_built" : !target ? "unqualified_target" : profile.is_null() ? "unqualified_context" : !profile_backend ? "required_profile_backend_not_built" : "qualified_profile";
    return {{"id","poima.runtime-game-session"},{"version",1},{"status","stable"},
        {"scope","runtime-game-session"},{"request_baseline","runtime-game-session-v1.json"},
        {"response_baseline","runtime-game-session-responses-v1.json"},
        {"methods",{"runtime.component.get","runtime.component.query","runtime.components","runtime.entity",
            "runtime.gameplay.inspect","runtime.hit_test","runtime.inspect","runtime.observe","runtime.raycast",
            "runtime.save.result","runtime.save.status","runtime.status","runtime.step","runtime.ui.activate",
            "runtime.ui.inspect","save.inspect","save.load","save.status","save.write"}},
        {"availability",{{"implemented",implemented},{"qualified",qualified},{"reason",reason},
            {"mode",read_only ? "read_only_runtime" : "authoring"},{"profile",std::move(profile)},
            {"target",{{"os",build.target_os},{"arch",build.target_arch}}},
            {"backends",{{"simulation",simulation},{"native_gameplay",native_gameplay},{"coreclr_gameplay",managed_gameplay}}},
            {"prerequisites",{"active_runtime_for_runtime_reads","trusted_gameplay_for_compiled_controls",
                "registered_schema_for_component_reads","configured_external_save_storage",
                "paused_or_no_player_for_mutations"}}}}};
}
Json project_discovery(Json description,const Json& params,std::string_view view,
                       const std::function<void(const std::string&)>& registered_type) {
    if(view=="full")return description;
    Json result={{"partial",true},{"view",view}};
    for(const auto* key:{"protocol_version","schema_revision","mode","read_only","runtime_available","session_scope","editor_discovery","unavailable_methods","unavailable_mutations","authoring_contract","runtime_game_session_contract"})
        if(description.contains(key)) { result[key]=std::move(description[key]);description.erase(key); }
    auto methods=std::move(description.at("methods"));description.erase("methods");
    auto components=std::move(description.at("components"));description.erase("components");
    const auto select_mutation=[&](const Json& selector) {
        require(methods.contains("world.transact"),"Mutation discovery is unavailable in this session.");
        const auto operation=selector.at("operation").get<std::string>();
        std::optional<std::string> type;
        if(selector.contains("type")) {
            type=selector.at("type").get<std::string>();
            if(type->starts_with("game:"))registered_type(*type);
        }
        try {
            return discovery_detail::mutation_schema(methods.at("world.transact"),operation,
                type ? std::optional<std::string_view>(*type) : std::nullopt);
        } catch(const std::invalid_argument& error) { throw Error(-32602,error.what()); }
    };
    const auto select_named=[](const Json& values,const std::string& name) -> const Json& {
        require(values.contains(name),"Unknown or unavailable discovery name: "+name);
        return values.at(name);
    };
    if(view=="catalog") {
        for(const auto* key:{"methods","components","sections"})result[key]=Json::array();
        for(const auto& [name,value]:methods.items())result["methods"].push_back(name);
        for(const auto& [name,value]:components.items())result["components"].push_back(name);
        for(const auto& [name,value]:description.items())result["sections"].push_back(name);
    } else if(view=="mutation") {
        result["methods"]={{"world.transact",select_mutation(params)}};
        result["selection"]={{"operation",params.at("operation")}};
        if(params.contains("type"))result["selection"]["type"]=params.at("type");
    } else if(view=="selection") {
        for(const auto* field:discovery_detail::named_selection_fields) {
            if(!params.contains(field))continue;
            const auto key=std::string_view(field);
            const auto& values=key=="methods" ? methods : key=="components" ? components : description;
            for(const auto& item:params.at(field)) {
                const auto& name=item.get_ref<const std::string&>();
                result[field][name]=select_named(values,name);
            }
        }
        if(params.contains("mutations")) {
            result["mutations"]=Json::array();
            for(const auto& selector:params.at("mutations"))
                result["mutations"].push_back({{"selection",selector},{"schema",select_mutation(selector)}});
        }
        require(result.dump().size()<=discovery_detail::selection_result_bytes,
            "Discovery selection exceeds 24 KiB. Request fewer entries or narrow large transaction schemas with a mutation selector.");
    } else {
        const auto name=params.at("name").get<std::string>();
        const auto& values=view=="method" ? methods : view=="component" ? components : description;
        result[view=="method" ? "methods" : view=="component" ? "components" : "sections"]={{name,select_named(values,name)}};
    }
    return result;
}

} // namespace poima::world_detail
