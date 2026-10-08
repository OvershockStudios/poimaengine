// SPDX-License-Identifier: Apache-2.0
#include "poima/mcp.hpp"
#include "poima/local_session.hpp"
#include "poima/build_metadata.hpp"
#include "poima/capture_image.hpp"
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
Json tools(){
    return Json::array({
        {{"name","poima_discover"},{"description","Discover the current native world API. Defaults to its compact catalog. Read invariants before editing; request individual method, component or section schemas, or mutation schemas with operation and optional component type."},
         {"inputSchema",schema({{"view",{{"type","string"},{"enum",{"full","catalog","method","component","section","mutation"}},{"default","catalog"}}},{"name",{{"type","string"},{"minLength",1}}},{"operation",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"type",{{"type","string"},{"minLength",1},{"maxLength",128}}}})},
         {"annotations",{{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}}}},
        {{"name","poima_call"},{"description","Call one native world operation using discovered parameters. Preserve native request_id and base_revision for edits. Calls are synchronous and never retried; transport failure may leave the outcome unknown."},
         {"inputSchema",schema({{"method",{{"type","string"},{"minLength",1}}},{"params",{{"type","object"}}},{"image",schema({{"max_edge",{{"type","integer"},{"minimum",128},{"maximum",2048},{"default",1280}}}})}},{"method"})},
         {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",true}}}}
    });
}
}
struct McpSession::Impl {
    std::function<std::string(std::string_view)> backend;
    bool initialized=false,ready=false;
    explicit Impl(std::function<std::string(std::string_view)> value):backend(std::move(value)) {
        if(!backend)throw std::invalid_argument("MCP backend is absent.");
    }
    Json call(const std::string& name,const Json& arguments) {
        // Tool argument failures are tool errors, not malformed MCP envelopes.
        if(!arguments.is_object())return tool_result({{"error",error(-32602,"Tool arguments must be an object.")}},true);
        std::string method;Json params;bool image_requested=false;std::uint32_t max_edge=1280;
        if(name=="poima_discover") {
            method="world.describe";params=arguments;
            if(!params.contains("view"))params["view"]="catalog";
        } else {
            for(const auto& [key,value]:arguments.items()) {
                (void)value;
                if(key!="method" && key!="params" && key!="image")return tool_result({{"error",error(-32602,"Unknown call argument.")}},true);
            }
            if(!arguments.contains("method") || !arguments["method"].is_string() || arguments["method"].get_ref<const std::string&>().empty())
                return tool_result({{"error",error(-32602,"A nonempty native method is required.")}},true);
            method=arguments["method"].get<std::string>();params=arguments.value("params",Json::object());
            if(!params.is_object())return tool_result({{"error",error(-32602,"Native params must be an object.")}},true);
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
        // Fixed correlation is safe because this adapter permits one synchronous
        // call at a time. It is unrelated to durable engine mutation request_id.
        const Json id="poima-mcp-native";
        const auto request=Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}.dump();
        if(request.size()>message_limit)return tool_result({{"error",error(-32602,"Native request exceeds 1 MiB.")}},true);
        try {
            const auto reply=parse(backend(request),local_session_response_limit);
            require(reply.is_object() && !reply.contains("method") && reply.value("jsonrpc",Json{})=="2.0" && reply.contains("id") && reply["id"]==id && (reply.contains("result")!=reply.contains("error")),-32603,"Invalid backend response envelope.");
            if(reply.contains("error")) {
                const auto& problem=reply["error"];
                require(problem.is_object() && problem.contains("code") && problem["code"].is_number_integer() && problem.contains("message") && problem["message"].is_string(),-32603,"Invalid backend error envelope.");
                return tool_result({{"error",problem}},true);
            }
            return image_requested ? observation(method,reply["result"],max_edge) : tool_result({{"result",reply["result"]}},false);
        } catch(const std::exception& failure) {
            std::cerr<<"MCP backend response failed: "<<failure.what()<<'\n';
        } catch(...) {std::cerr<<"MCP backend response failed with an unknown exception.\n";}
        return tool_result({{"error",error(-32603,"Backend response unavailable or invalid; do not retry blindly.")},{"outcome_unknown",true}},true);
    }
};
McpSession::McpSession(std::function<std::string(std::string_view)> backend):impl_(std::make_unique<Impl>(std::move(backend))){}
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
            impl_->initialized=true;
        } else if(method=="ping")result=Json::object();
        else {
            require(method=="tools/list" || method=="tools/call",-32601,"Unknown MCP method.");
            require(impl_->ready,-32002,"Complete MCP initialization before using tools.");
            if(method=="tools/list") {
                require(!params.contains("cursor"),-32602,"Tool catalog is not paginated.");
                result={{"tools",tools()}};
            } else if(method=="tools/call") {
                require(!params.contains("task"),-32602,"Task execution is unsupported.");
                require(params.contains("name") && params["name"].is_string(),-32602,"Tool name is required.");
                const auto name=params["name"].get<std::string>();
                require(name=="poima_discover" || name=="poima_call",-32602,"Unknown tool.");
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
