// SPDX-License-Identifier: Apache-2.0
#include "poima/mcp.hpp"
#include "poima/local_session.hpp"
#include "poima/build_metadata.hpp"
#include "poima/capture_image.hpp"
#include "poima/project.hpp"
#include "authoring_reference.hpp"
#include "discovery_selection.hpp"
#include "project_draft_paths.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <utility>
namespace poima {
namespace {
using Json=nlohmann::json;
constexpr std::size_t message_limit=local_session_request_limit;
struct ProtocolError:std::runtime_error {
    int code;
    ProtocolError(int value,const char* message):std::runtime_error(message),code(value){}
};
void require(bool value,int code,const char* message){if(!value)throw ProtocolError(code,message);}
Json parse(std::string_view input,std::size_t limit=message_limit) {
    require(input.size()<=limit,-32700,"Message exceeds its size limit.");
    std::vector<std::unordered_set<std::string>> keys;
    return Json::parse(input,[&](int depth,Json::parse_event_t event,Json& value){
        require(depth<=64,-32700,"JSON nesting exceeds 64.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(value.get<std::string>()).second,-32700,"Duplicate JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
bool valid_id(const Json& id){return id.is_string() || id.is_number_integer();}
Json error(int code,const std::string& message){return {{"code",code},{"message",message}};}
Json tool_result(Json content,bool failed){return {{"content",Json::array({{{"type","text"},{"text",content.dump()}}})},{"structuredContent",std::move(content)},{"isError",failed}};}
std::string base64(std::span<const std::byte> bytes) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;encoded.reserve((bytes.size()+2)/3*4);
    for(std::size_t i=0;i<bytes.size();i+=3) {
        const auto a=std::to_integer<unsigned>(bytes[i]),b=i+1<bytes.size()?std::to_integer<unsigned>(bytes[i+1]):0,c=i+2<bytes.size()?std::to_integer<unsigned>(bytes[i+2]):0;
        encoded+=alphabet[a>>2];encoded+=alphabet[((a&3)<<4)|(b>>4)];encoded+=i+1<bytes.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';encoded+=i+2<bytes.size()?alphabet[c&63]:'=';
    }
    return encoded;
}
Json observation(const std::string& method,const Json& receipt,std::uint32_t max_edge) {
    try {
        require(receipt.is_object(),-32603,"Capture receipt must be an object.");
        const Json* capture=&receipt;
        if(method=="desktop.capture.status") {
            require(receipt.contains("state")&&receipt["state"].is_string(),-32603,"Capture status lacks state.");
            if(receipt["state"]=="queued")return tool_result({{"result",receipt},{"observation",{{"state","pending"}}}},false);
            require(receipt["state"]=="complete",-32603,"Desktop capture did not complete successfully.");
            require(receipt.contains("result")&&receipt["result"].is_object(),-32603,"Completed capture lacks result.");capture=&receipt["result"];
        } else if(method!="editor.capture")require(receipt.value("capture_written",Json{})==true,-32603,"Capture was not written.");
        require(capture->contains("path")&&(*capture)["path"].is_string(),-32603,"Capture receipt lacks a path.");
        const auto text=(*capture)["path"].get<std::string>();
        require(!text.empty()&&text.find('\0')==std::string::npos,-32603,"Capture path must be nonempty and NUL-free.");
        // JSON parsing validates UTF-8; filesystem conversion preserves it on Windows.
        const std::filesystem::path path(std::u8string(text.begin(),text.end()));
        require(std::filesystem::symlink_status(path).type()==std::filesystem::file_type::regular,-32603,"Capture path must be a regular file.");
        const auto size=std::filesystem::file_size(path);
        require(size>0&&size<=128ull*1024*1024,-32603,"Capture file exceeds the bounded input size.");
        std::ifstream stream(path,std::ios::binary);require(bool(stream),-32603,"Cannot open capture file.");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size));
        require(stream.gcount()==static_cast<std::streamsize>(size)&&stream.peek()==std::char_traits<char>::eof(),-32603,"Capture changed or could not be read completely.");
        const auto image=capture_image(bytes,max_edge);
        for(const auto* key:{"width","height"})require(capture->contains(key)&&(*capture)[key].is_number_integer()&&(*capture)[key]>0&&(*capture)[key]<=std::numeric_limits<std::uint32_t>::max(),-32603,"Capture receipt dimensions are invalid.");
        require(image.source_width==(*capture)["width"].get<std::uint32_t>()&&image.source_height==(*capture)["height"].get<std::uint32_t>(),-32603,"Capture dimensions differ from the native receipt.");
        require(!image.png.empty()&&image.png.size()<=16ull*1024*1024,-32603,"PNG observation exceeds output limit.");
        auto reply=tool_result({{"result",receipt},{"observation",{{"state","ready"},{"source_width",image.source_width},{"source_height",image.source_height},{"width",image.width},{"height",image.height},{"source_sha256",image.source_sha256},{"format","PNG"}}}},false);
        reply["content"].push_back({{"type","image"},{"mimeType","image/png"},{"data",base64(image.png)}});return reply;
    }catch(const std::exception& failure) {
        return tool_result({{"result",receipt},{"observation",{{"state","error"},{"message",failure.what()}}}},true);
    }
}
Json schema(Json properties,Json required=Json::array()) {return {{"type","object"},{"properties",std::move(properties)},{"required",std::move(required)},{"additionalProperties",false}};}
constexpr std::array review_methods{
    std::string_view("world.inspect"), std::string_view("world.lighting"),
    std::string_view("entity.query"), std::string_view("entity.get"), std::string_view("entity.world_transform"),
    std::string_view("world.asset.references"), std::string_view("world.ui.list"), std::string_view("world.ui.get")
};
constexpr std::array<std::string_view, 35> draft_methods{
    "asset.animation.author", "asset.animation.bounds", "asset.animation.channel", "asset.animation.sample",
    "asset.animation.skin", "asset.audio.import", "asset.audio.inspect", "asset.catalog",
    "asset.image.import", "asset.image.inspect", "asset.import", "asset.inspect", "asset.label",
    "asset.mesh.extract_rigid", "asset.mesh.triangles", "asset.provenance.create", "asset.provenance.inspect",
    "asset.source.inspect", "component.schema.import", "component.schemas",
    "entity.get", "entity.material", "entity.query", "entity.world_transform",
    "world.asset.provenance", "world.asset.references", "world.dependencies", "world.history",
    "world.inspect", "world.lighting", "world.redo", "world.transact", "world.ui.get", "world.ui.list", "world.undo"
};
bool source_method(std::string_view name) {
    return name=="project.source.read" || name=="project.source.write";
}
bool draft_method(std::string_view name,bool source) {
    return (source && source_method(name)) || std::find(draft_methods.begin(),draft_methods.end(),name)!=draft_methods.end();
}
void check_draft_request(std::string_view method,const Json& params,const std::string& world,bool gameplay_source) {
    constexpr auto unavailable="Native method is unavailable to a draft connection.";
    if(method=="world.describe" || method=="world.describe.select") {
        if(params.value("view",std::string{})=="method" && params.contains("name"))
            require(params.at("name").is_string() && draft_method(params.at("name").get_ref<const std::string&>(),gameplay_source),-32602,unavailable);
        if(params.contains("methods") && params.at("methods").is_array()) {
            for(const auto& selected:params.at("methods"))
                require(selected.is_string() && draft_method(selected.get_ref<const std::string&>(),gameplay_source),-32602,unavailable);
        }
        return;
    }
    if(method=="world.reference")return;
    require(draft_method(method,gameplay_source),-32602,unavailable);
    if(method!="asset.import" && method!="asset.source.inspect" && method!="asset.audio.import" && method!="asset.image.import")
        return;
    const auto check_source=[&](const Json& source) {
        require(source.is_string(),-32602,"Draft import source must be a relative file path.");
        project_detail::validate_draft_asset_source(world,source.get_ref<const std::string&>());
    };
    require(params.contains("source"),-32602,"Draft import source is required.");
    check_source(params.at("source"));
    if(method=="asset.import" && params.contains("animations")) {
        const auto& donors=params.at("animations");
        require(donors.is_array() && !donors.empty() && donors.size()<=32,-32602,"Animations must contain 1..32 source paths.");
        for(const auto& donor:donors) {
            require(!donor.is_object() || donor.contains("source"),-32602,"Draft animation source is required.");
            check_source(donor.is_object() ? donor.at("source") : donor);
        }
    }
}
void scope_draft_discovery(Json& result,bool source) {
    if(result.contains("methods")) {
        auto& methods=result.at("methods");
        for(auto it=methods.begin();it!=methods.end();) {
            const auto& name=methods.is_array() ? it->get_ref<const std::string&>() : it.key();
            if(!draft_method(name,source)) {
                it=methods.erase(it);
                continue;
            }
            if(methods.is_object() && source_method(name)) {
                auto& properties=it->at("properties");
                properties.erase("profile");
                properties.erase("project");
                properties["path"]["description"]="A portable .cs path under this draft's gameplay-src directory.";
                (*it)["required"]=name=="project.source.read" ? Json::array({"path"}) :
                    Json::array({"path","expected_sha256","text"});
            }
            ++it;
        }
    }
    result["mcp_scope"]="project_draft";
    if(source)result["draft_gameplay_source"]=true;
}
bool available_tool(std::string_view name,bool review,bool draft,bool source) {
    if(name=="poima_discover" || name=="poima_reference")return true;
    if(review)return name=="poima_inspect";
    if(draft)return name=="poima_call" || (source && name=="poima_source");
    return name=="poima_call" || name=="poima_build" || name=="poima_source" || name=="poima_checks";
}
Json source_tool(const char* description) {
    return {{"name","poima_source"},{"description",description},
        {"inputSchema",schema({{"method",{{"enum",{"project.source.write"}}}},{"params",{{"type","object"}}}},{"method","params"})},
        {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",true},{"openWorldHint",false}}}};
}
Json tools(bool review,bool draft,bool source){
    auto catalog=Json::array({
        {{"name","poima_discover"},{"description","Discover the current native world API. Defaults to its compact catalog. Use selection alone to retrieve up to 12 exact method, component, section or scoped mutation schemas in one reply. Read invariants before editing. Select specific operations for transactions."},
         {"inputSchema",schema({{"view",{{"type","string"},{"enum",{"full","catalog","method","component","section","mutation"}},{"default","catalog"}}},{"name",{{"type","string"},{"minLength",1}}},{"operation",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"type",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"selection",discovery_detail::selection_parameters_schema()}})},
         {"annotations",{{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}}}},
        {{"name","poima_reference"},{"description","Read gameplay and verification guides bundled in the world owner's engine. Pass guide and an exact section heading for focused instructions. A guide alone lists its headings. Empty arguments list guides."},
         {"inputSchema",authoring_reference::schema()},
         {"annotations",{{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}}}},
    });
    if(review) {
        catalog.push_back({{"name","poima_inspect"},{"description","Inspect authored scene data at this connection's assigned revision. Use poima_discover for exact native parameter schemas. The tool adds the revision guard. A later revision requires a new review assignment."},
            {"inputSchema",schema({{"method",{{"type","string"},{"enum",review_methods}}},{"params",{{"type","object"}}}},{"method"})},
            {"annotations",{{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}}}});
        return catalog;
    }
    if(draft) {
        Json methods=draft_methods;
        if(source)methods.push_back("project.source.read");
        catalog.push_back({{"name","poima_call"},{"description","Call one scoped native operation on this draft. Preserve the revision and durable request ID when editing. Asset sources and animation donors use portable paths relative to the draft entry world's directory. Discover exact parameters with poima_discover."},
            {"inputSchema",schema({{"method",{{"type","string"},{"enum",std::move(methods)}}},{"params",{{"type","object"}}}},{"method"})},
            {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",false}}}});
        if(source)catalog.push_back(source_tool("Write one C# file in this draft. Read project.source.read through poima_call first. Pass only path, expected_sha256 and text. The lead builds and verifies the integrated candidate."));
        return catalog;
    }
    for(auto& tool:Json::array({
        {{"name","poima_call"},{"description","Call one native world operation using discovered parameters. Preserve native request_id and base_revision for edits. Calls are synchronous and never retried; transport failure may leave the outcome unknown."},
         {"inputSchema",schema({{"method",{{"type","string"},{"minLength",1}}},{"params",{{"type","object"}}},{"image",schema({{"max_edge",{{"type","integer"},{"minimum",128},{"maximum",2048},{"default",1280}}}})}},{"method"})},
         {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",true}}}},
        source_tool("Write one C# source file in the open host-configured project. First read project.source.read through poima_call and use its SHA-256 as the write guard. This operation publishes text without compiling it."),
        {{"name","poima_checks"},{"description","Write the open project's gameplay verification suite. Read project.verification.read through poima_call and use its SHA-256 as the write guard. A successful write validates the suite format; the game has to run the checks separately."},
         {"inputSchema",schema({{"method",{{"enum",{"project.verification.write"}}}},{"params",{{"type","object"}}}},{"method","params"})},
         {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",true},{"openWorldHint",false}}}},
        {{"name","poima_build"},{"description","Submit trusted compile, native publish, export, verification or candidate jobs, or synchronously retain a passing candidate or adopt native gameplay. Publishing/export/adoption use host-configured profiles; legacy compilation uses an explicit SDK executable. This grants build/process/filesystem authority separately from world edits. Inspect development.profiles first. Compile, publish, export and verification return jobs polled through development.inspect; project.gameplay.adopt returns a durable project receipt and launches no compiler. No automatic gameplay activation or undo of emitted files."},
         {"inputSchema",schema({{"method",{{"enum",{"development.compile","development.publish","development.export","development.verify","development.candidate","project.candidate.retain","project.gameplay.adopt"}}}},{"params",{{"type","object"}}}},{"method","params"})},
         {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",true}}}}
    }))catalog.push_back(std::move(tool));
    return catalog;
}
}
struct McpSession::Impl {
    std::function<std::string(std::string_view)> backend;
    const std::optional<std::uint64_t> review_revision;
    const std::string draft_world;
    const std::string draft_source_manifest;
    bool initialized=false,ready=false;
    Impl(std::function<std::string(std::string_view)> value,std::optional<std::uint64_t> revision,std::string world,std::string source_manifest)
        :backend(std::move(value)),review_revision(revision),draft_world(std::move(world)),draft_source_manifest(std::move(source_manifest)) {
        if(!backend)throw std::invalid_argument("MCP backend is absent.");
        if(review_revision && *review_revision>mcp_max_review_revision)
            throw std::invalid_argument("MCP review revision exceeds the safe JSON integer range.");
        if(review_revision && !draft_world.empty())
            throw std::invalid_argument("Draft and review MCP scopes are mutually exclusive.");
        if(!draft_source_manifest.empty() && draft_world.empty())
            throw std::invalid_argument("Gameplay source scope requires a draft world.");
    }
    bool draft_source() const {return !draft_source_manifest.empty();}
    Json call(const std::string& name,const Json& arguments) {
        // Tool argument failures are tool errors, not malformed MCP envelopes.
        if(!arguments.is_object())return tool_result({{"error",error(-32602,"Tool arguments must be an object.")}},true);
        std::string method;Json params;bool image_requested=false;std::uint32_t max_edge=1280;
        if(name=="poima_discover") {
            if(arguments.contains("selection")) {
                if(arguments.size()!=1)return tool_result({{"error",error(-32602,"Discovery selection cannot be combined with single-view arguments.")}},true);
                method="world.describe.select";params=arguments.at("selection");
            } else {
                method="world.describe";params=arguments;
                if(!params.contains("view"))params["view"]="catalog";
            }
        } else if(name=="poima_reference") {
            method="world.reference";params=arguments;
        } else {
            for(const auto& [key,value]:arguments.items()) {
                (void)value;
                if(key!="method" && key!="params" && (key!="image" || review_revision || !draft_world.empty()))return tool_result({{"error",error(-32602,"Unknown call argument.")}},true);
            }
            if(!arguments.contains("method") || !arguments["method"].is_string() || arguments["method"].get_ref<const std::string&>().empty())
                return tool_result({{"error",error(-32602,"A nonempty native method is required.")}},true);
            method=arguments["method"].get<std::string>();params=arguments.value("params",Json::object());
            if(!params.is_object())return tool_result({{"error",error(-32602,"Native params must be an object.")}},true);
            if(!draft_world.empty() && !draft_method(method,draft_source()))
                return tool_result({{"error",error(-32602,"Native method is unavailable to a draft connection.")}},true);
            if(review_revision) {
                if(std::find(review_methods.begin(),review_methods.end(),method)==review_methods.end())
                    return tool_result({{"error",error(-32602,"Native method is unavailable to a review connection.")}},true);
                if(params.contains("revision") && (!params["revision"].is_number_integer() || params["revision"]!=*review_revision))
                    return tool_result({{"error",error(-32602,"Requested revision differs from the assigned review revision.")}},true);
                // world.inspect is itself a single atomic read of identity and
                // revision. Its result is checked below before forwarding it.
                if(method=="world.inspect")params.erase("revision");
                else params["revision"]=*review_revision;
            }
            const bool build=method=="development.compile" || method=="development.publish" || method=="development.export" || method=="development.verify" || method=="development.candidate" || method=="project.candidate.retain" || method=="project.gameplay.adopt";
            const bool source=method=="project.source.write";
            const bool checks=method=="project.verification.write";
            if(name=="poima_build") {
                if(!build || !arguments.contains("params") || arguments.contains("image"))return tool_result({{"error",error(-32602,"poima_build requires compile, publish, export, verify or adopt parameters and accepts no image option.")}},true);
            } else if(name=="poima_source") {
                if(!source || !arguments.contains("params") || arguments.contains("image"))return tool_result({{"error",error(-32602,"poima_source expects project.source.write parameters.")}},true);
            } else if(name=="poima_checks") {
                if(!checks || !arguments.contains("params") || arguments.contains("image"))return tool_result({{"error",error(-32602,"poima_checks expects project.verification.write parameters.")}},true);
            } else if(build) return tool_result({{"error",error(-32602,"Use the separately authorized poima_build tool for compilation, native publishing, export, verification and project gameplay adoption.")}},true);
            else if(source) return tool_result({{"error",error(-32602,"Use the separately authorized poima_source tool for source writes.")}},true);
            else if(checks) return tool_result({{"error",error(-32602,"Use the separately authorized poima_checks tool for verification suite writes.")}},true);
            if(arguments.contains("image")) {
                const auto& image=arguments["image"];
                if(!image.is_object())return tool_result({{"error",error(-32602,"image must be an object.")}},true);
                for(const auto& [key,value]:image.items()) {
                    if(key!="max_edge" || !value.is_number_integer() || value<128 || value>2048)
                        return tool_result({{"error",error(-32602,"image accepts only integer max_edge in [128,2048].")}},true);
                }
                if(method!="world.capture"&&method!="runtime.capture"&&method!="asset.animation.capture"&&method!="editor.capture"&&method!="desktop.capture.status")
                    return tool_result({{"error",error(-32602,"Image observation is unsupported for this method.")}},true);
                max_edge=image.value("max_edge",1280u);image_requested=true;
            }
        }
        if(!draft_world.empty()) {
            try {check_draft_request(method,params,draft_world,draft_source());}
            catch(const std::exception& failure) {return tool_result({{"error",error(-32602,failure.what())}},true);}
        }
        // Fixed correlation is safe because this adapter permits one synchronous
        // call at a time. It is unrelated to durable engine mutation request_id.
        const Json id="poima-mcp-native";
        const auto request=Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}.dump();
        if(request.size()>message_limit)return tool_result({{"error",error(-32602,"Native request exceeds 1 MiB.")}},true);
        try {
            if(draft_source() && source_method(method)) {
                if(params.contains("text")!=(method=="project.source.write"))
                    return tool_result({{"error",error(-32602,"Source reads accept a path. Source writes also require a guard and replacement text.")}},true);
                const auto reply=project_gameplay_source(draft_source_manifest,draft_world,params.dump());
                const auto receipt=parse(reply.json,local_session_response_limit);
                if(reply.exit_code!=0) {
                    const auto& diagnostic=receipt.at("diagnostics").at(0);
                    return tool_result({{"error",error(diagnostic.at("error_code").get<int>(),diagnostic.at("message").get<std::string>())}},true);
                }
                return tool_result({{"result",receipt.at("result")}},false);
            }
            auto reply=parse(backend(request),local_session_response_limit);
            require(reply.is_object() && !reply.contains("method") && reply.value("jsonrpc",Json{})=="2.0" && reply.contains("id") && reply["id"]==id && (reply.contains("result")!=reply.contains("error")),-32603,"Invalid backend response envelope.");
            if(reply.contains("error")) {
                const auto& problem=reply["error"];
                require(problem.is_object() && problem.contains("code") && problem["code"].is_number_integer() && problem.contains("message") && problem["message"].is_string(),-32603,"Invalid backend error envelope.");
                return tool_result({{"error",problem}},true);
            }
            auto& result=reply["result"];
            if(!draft_world.empty() && name=="poima_discover")scope_draft_discovery(result,draft_source());
            if(name=="poima_inspect") {
                require(result.is_object() && result.contains("revision") && result["revision"].is_number_integer(),
                        -32603,"Review result lacks a native revision.");
                if(result["revision"]!=*review_revision)
                    return tool_result({{"error",error(-32009,"Review revision is stale. Request a new review assignment.")}},true);
            }
            return image_requested ? observation(method,result,max_edge) : tool_result({{"result",result}},false);
        } catch(const std::exception& failure) {
            std::cerr<<"MCP backend response failed: "<<failure.what()<<'\n';
        } catch(...) {std::cerr<<"MCP backend response failed with an unknown exception.\n";}
        return tool_result({{"error",error(-32603,"Backend response unavailable or invalid; do not retry blindly.")},{"outcome_unknown",true}},true);
    }
};
McpSession::McpSession(std::function<std::string(std::string_view)> backend,std::optional<std::uint64_t> review_revision,
                       std::string draft_world,std::string draft_source_manifest)
    :impl_(std::make_unique<Impl>(std::move(backend),review_revision,std::move(draft_world),std::move(draft_source_manifest))){}
McpSession::~McpSession()=default;
std::string McpSession::request(std::string_view message) {
    Json id=nullptr;bool notification=false;
    try {
        Json input;
        try {input=parse(message);}catch(const Json::exception&){throw ProtocolError(-32700,"Invalid JSON.");}
        require(input.is_object(),-32600,"Expected one JSON-RPC request object.");
        // This server issues no JSON-RPC requests. A valid response (including
        // a peer parse-error response with null ID) has nothing to correlate;
        // discard it rather than responding to a response indefinitely.
        const bool response_fields=input.contains("result") || input.contains("error");
        if(!input.contains("method") && response_fields) {
            require(input.value("jsonrpc",Json{})=="2.0" && input.contains("id") &&
                (input["id"].is_null() || valid_id(input["id"])) &&
                (input.contains("result")!=input.contains("error")),-32600,"Invalid JSON-RPC response.");
            if(input.contains("error")) {
                const auto& problem=input["error"];
                require(problem.is_object() && problem.contains("code") && problem["code"].is_number_integer() &&
                    problem.contains("message") && problem["message"].is_string(),-32600,"Invalid JSON-RPC error response.");
            }
            return {};
        }
        require(!response_fields,-32600,"Request cannot contain response fields.");
        if(input.contains("id")) {require(valid_id(input["id"]),-32600,"Invalid request ID.");id=input["id"];}
        require(input.value("jsonrpc",Json{})=="2.0" && input.contains("method") && input["method"].is_string(),-32600,"Invalid JSON-RPC request.");
        notification=!input.contains("id");
        const auto method=input["method"].get<std::string>();
        if(notification) {
            if(method=="notifications/initialized" && impl_->initialized && (!input.contains("params") || input["params"].is_object()))impl_->ready=true;
            return {}; // Includes tools/call: notifications can never invoke a tool.
        }
        const auto params=input.value("params",Json::object());
        require(params.is_object(),-32602,"MCP params must be an object.");
        Json result;
        if(method=="initialize") {
            require(!impl_->initialized,-32600,"Session is already initialized.");
            require(params.contains("protocolVersion") && params["protocolVersion"].is_string() && params.contains("capabilities") && params["capabilities"].is_object() && params.contains("clientInfo") && params["clientInfo"].is_object(),-32602,"Invalid initialization parameters.");
            const auto& info=params["clientInfo"];
            require(info.contains("name") && info["name"].is_string() && info.contains("version") && info["version"].is_string(),-32602,"Invalid clientInfo.");
            result={{"protocolVersion","2025-11-25"},{"capabilities",{{"tools",Json::object()}}},{"serverInfo",{{"name","Poima"},{"version",build_metadata().version}}},{"instructions","Use poima_discover for current native schemas. Preserve engine revision guards and durable request IDs. Calls are synchronous; cancellation and automatic retries are not supported."}};
            if(impl_->review_revision)
                result["instructions"]="This connection can inspect authored revision "+std::to_string(*impl_->review_revision)
                    +". Use poima_inspect for scene reads and poima_discover for their schemas. A stale read requires a new assignment from the lead.";
            else if(!impl_->draft_world.empty()) {
                result["instructions"]="This connection edits one project draft. Discover its scene and asset operations with poima_discover. "
                    "Use poima_call with the current revision and a durable request ID. Draft edits require separate lead integration.";
                if(impl_->draft_source())result["instructions"]=result["instructions"].get<std::string>()+
                    " Read draft C# files through project.source.read and submit guarded writes through poima_source.";
            }
            impl_->initialized=true;
        } else if(method=="ping")result=Json::object();
        else {
            require(method=="tools/list" || method=="tools/call",-32601,"Unknown MCP method.");
            require(impl_->ready,-32002,"Complete MCP initialization before using tools.");
            if(method=="tools/list") {
                require(!params.contains("cursor"),-32602,"Tool catalog is not paginated.");
                result={{"tools",tools(impl_->review_revision.has_value(),!impl_->draft_world.empty(),impl_->draft_source())}};
            } else if(method=="tools/call") {
                require(!params.contains("task"),-32602,"Task execution is unsupported.");
                require(params.contains("name") && params["name"].is_string(),-32602,"Tool name is required.");
                const auto name=params["name"].get<std::string>();
                require(available_tool(name,impl_->review_revision.has_value(),!impl_->draft_world.empty(),impl_->draft_source()),-32602,"Unknown tool.");
                require(!params.contains("arguments") || params["arguments"].is_object(),-32602,"Tool arguments must be an object.");
                result=impl_->call(name,params.value("arguments",Json::object()));
            } else throw ProtocolError(-32601,"Unknown MCP method.");
        }
        return Json{{"jsonrpc","2.0"},{"id",id},{"result",result}}.dump();
    } catch(const ProtocolError& failure) {
        if(notification)return {};
        return Json{{"jsonrpc","2.0"},{"id",id},{"error",error(failure.code,failure.what())}}.dump();
    } catch(const Json::exception&) {
        if(notification)return {};
        return Json{{"jsonrpc","2.0"},{"id",id},{"error",error(-32602,"Invalid operation parameters.")}}.dump();
    } catch(const std::exception& failure) {
        std::cerr<<"MCP request failed: "<<failure.what()<<'\n';
        if(notification)return {};
        return Json{{"jsonrpc","2.0"},{"id",id},{"error",error(-32603,"Internal adapter failure.")}}.dump();
    }
}
}
