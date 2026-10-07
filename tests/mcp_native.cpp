// SPDX-License-Identifier: Apache-2.0
#include "poima/mcp.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>
#include <stdexcept>
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Json rpc(poima::McpSession& session,const char* method,Json params=Json::object(),Json id="client-id") {
    const auto reply=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}.dump()));
    check(reply.at("jsonrpc")=="2.0"&&reply.at("id")==id,"MCP correlation differs");return reply;
}
void error(const Json& reply,int code){check(reply.contains("error")&&reply.at("error").at("code")==code,"MCP error code differs");}
Json tool(poima::McpSession& session,Json arguments,const char* name="poima_call") {
    auto reply=rpc(session,"tools/call",{{"name",name},{"arguments",arguments}}).at("result");
    check(Json::parse(reply.at("content").at(0).at("text").get<std::string>())==reply.at("structuredContent"),"MCP text and structured results diverge");return reply;
}
void image_checks() {
    namespace fs=std::filesystem;
    const auto directory=fs::temp_directory_path()/("poima-mcp-image-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{directory};
    const auto path=directory/"fixture.bmp";
    std::vector<unsigned char> bmp(70,0);
    const auto word=[&](std::size_t at,unsigned value,unsigned count) {for(unsigned i=0;i<count;++i)bmp[at+i]=static_cast<unsigned char>(value>>(i*8));};
    bmp[0]='B';bmp[1]='M';word(2,70,4);word(10,54,4);word(14,40,4);word(18,2,4);word(22,2,4);word(26,1,2);word(28,24,2);word(34,16,4);
    // Bottom-up BGR: blue/white on bottom, red/green on top, 2 padding bytes/row.
    bmp[54]=255;bmp[57]=255;bmp[58]=255;bmp[59]=255;bmp[64]=255;bmp[66]=255;
    {std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(bmp.data()),static_cast<std::streamsize>(bmp.size()));}
    const auto utf8=path.u8string();const std::string filename(utf8.begin(),utf8.end());
    const Json good={{"capture_written",true},{"path",filename},{"width",2},{"height",2},{"revision",17}};
    Json receipt=good,last;unsigned calls=0;
    poima::McpSession session([&](std::string_view text){++calls;last=Json::parse(text);return Json{{"jsonrpc","2.0"},{"id",last.at("id")},{"result",receipt}}.dump();});
    rpc(session,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",Json::object()},{"clientInfo",{{"name","image-test"},{"version","1"}}}});
    session.request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    const Json params={{"revision",17},{"camera",std::string(32,'1')},{"path","caller-path-is-not-trusted.bmp"}};
    auto plain=tool(session,{{"method","world.capture"},{"params",params}});
    check(plain.at("content").size()==1&&!plain.at("structuredContent").contains("observation"),"Image unexpectedly returned without opt-in");
    auto observed=tool(session,{{"method","world.capture"},{"params",params},{"image",Json::object()}});
    check(last.at("params")==params&&!last.at("params").contains("image"),"Image option modified native capture params");
    check(observed.at("isError")==false&&observed.at("structuredContent").at("result")==good,"Image conversion changed native receipt");
    const auto& metadata=observed.at("structuredContent").at("observation");
    check(metadata.at("state")=="ready"&&metadata.at("source_width")==2&&metadata.at("source_height")==2&&metadata.at("width")==2&&metadata.at("height")==2&&metadata.at("format")=="PNG"&&metadata.at("source_sha256").get<std::string>().size()==64,"Image metadata invalid");
    check(observed.at("content").size()==2&&observed.at("content").at(1).at("mimeType")=="image/png","Missing PNG image content");
    const auto encoded=observed.at("content").at(1).at("data").get<std::string>();
    std::vector<unsigned char> png;unsigned accumulator=0,bits=0;
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(char c:encoded) {if(c=='=')break;const auto value=alphabet.find(c);check(value!=std::string::npos,"Invalid base64 image");accumulator=(accumulator<<6)|static_cast<unsigned>(value);bits+=6;if(bits>=8){bits-=8;png.push_back(static_cast<unsigned char>(accumulator>>bits));}}
    const auto decoded=poima::decode_texture(std::as_bytes(std::span(png)),true);
    check(decoded&&!decoded->mips.empty()&&decoded->mips[0].width==2&&decoded->mips[0].height==2,"PNG image is not decodable");
    const std::vector<std::uint8_t> expected={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    check(decoded->mips[0].rgba==expected,"PNG color or orientation differs");
    check(observed.at("content").at(0).at("text").get<std::string>().find(encoded)==std::string::npos,"Image base64 leaked into text");
    const auto bad_observation=[&](const char* method) {
        const auto before=calls;const auto out=tool(session,{{"method",method},{"image",Json::object()}});
        check(calls==before+1&&out.at("isError")==true&&out.at("structuredContent").at("result")==receipt&&out.at("structuredContent").at("observation").at("state")=="error"&&!out.at("structuredContent").contains("outcome_unknown")&&out.at("content").size()==1,"Image failure lost known native outcome or retried");
    };
    receipt=good;receipt["width"]=3;bad_observation("world.capture");
    receipt=good;receipt["path"]=filename+".missing";bad_observation("world.capture");
    receipt=good;receipt["path"]=false;bad_observation("world.capture");
    receipt=good;receipt["path"]=std::string("bad\0path",8);bad_observation("world.capture");
    receipt=good;receipt["capture_written"]=false;bad_observation("world.capture");
    receipt={{"state","queued"},{"path",false},{"capture_id",4}};
    observed=tool(session,{{"method","desktop.capture.status"},{"image",Json::object()}});
    check(observed.at("isError")==false&&observed.at("structuredContent").at("result")==receipt&&observed.at("structuredContent").at("observation").at("state")=="pending"&&observed.at("content").size()==1,"Queued capture read image or lost receipt");
    receipt={{"state","error"},{"error",{{"code",-32009},{"message","Capture stale"}}}};bad_observation("desktop.capture.status");
    receipt={{"state","complete"},{"capture_id",4},{"result",good}};
    observed=tool(session,{{"method","desktop.capture.status"},{"image",Json::object()}});
    check(observed.at("isError")==false&&observed.at("structuredContent").at("result")==receipt&&observed.at("content").size()==2,"Desktop image lost outer completion receipt");
    receipt=good;receipt.erase("capture_written");
    observed=tool(session,{{"method","editor.capture"},{"image",{{"max_edge",128}}}});check(observed.at("isError")==false,"Legacy editor receipt rejected");
    const auto before=calls;
    for(const Json image:{Json(nullptr),Json(true),Json::array(),Json{{"unknown",1}},Json{{"max_edge",true}},Json{{"max_edge",128.0}},Json{{"max_edge",127}},Json{{"max_edge",2049}}}) {
        observed=tool(session,{{"method","world.capture"},{"image",image}});check(observed.at("isError")==true&&observed.at("structuredContent").at("error").at("code")==-32602,"Invalid image option not rejected");
    }
    observed=tool(session,{{"method","world.inspect"},{"image",Json::object()}});check(observed.at("isError")==true,"Unsupported image method accepted");
    check(calls==before,"Invalid image request invoked backend");
}

}
int main(){
try {
    unsigned calls=0;Json seen;std::string behavior="success";
    const Json receipt={{"revision",12},{"replayed",true},{"request_id",std::string(32,'1')}};
    const Json native_error={{"code",-32009},{"message","Stale revision"},{"data",{{"actual",12}}}};
    poima::McpSession session([&](std::string_view input){
        ++calls;seen=Json::parse(input);
        if(behavior=="throw")throw std::runtime_error("transport disconnected after write");
        if(behavior=="json")return std::string("{");
        if(behavior=="large")return std::string(32*1024*1024+1,' ');
        if(behavior=="duplicate")return std::string(R"({"jsonrpc":"2.0","id":"poima-mcp-native","result":1,"result":2})");
        Json response={{"jsonrpc","2.0"},{"id",seen.at("id")}};
        if(behavior=="error")response["error"]=native_error;
        else response["result"]=receipt;
        if(behavior=="id")response["id"]="wrong";
        if(behavior=="hybrid")response["method"]="world.transact";
        if(behavior=="both")response["error"]=native_error;
        if(behavior=="hybrid")response["method"]="ping";
        if(behavior=="bad-error") {response.erase("result");response["error"]={{"code","bad"},{"message",false}};}
        return response.dump();
    });
    const auto ignore_responses=[&] {
        const auto before=calls;
        for(const Json response:{
            Json{{"jsonrpc","2.0"},{"id","unexpected"},{"result",Json::object()}},
            Json{{"jsonrpc","2.0"},{"id",1},{"error",{{"code",-32601},{"message","Unknown method"}}}},
            Json{{"jsonrpc","2.0"},{"id",nullptr},{"error",{{"code",-32700},{"message","Parse error"}}}}
        })check(session.request(response.dump()).empty(),"MCP replied to an incoming response and could cause a response loop");
        check(calls==before,"Incoming response invoked backend");
    };
    ignore_responses();
    error(rpc(session,"tools/list"),-32002);
    error(rpc(session,"tools/call",{{"name","poima_call"},{"arguments",{{"method","world.transact"}}}}),-32002);
    check(session.request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),"Premature notification replied");
    error(rpc(session,"tools/list"),-32002);check(calls==0,"Backend called before initialization");
    error(rpc(session,"initialize",{{"protocolVersion",1}}),-32602);
    const auto initialized=rpc(session,"initialize",{{"protocolVersion","older-client"},{"capabilities",Json::object()},{"clientInfo",{{"name","test"},{"version","1"}}}});
    check(initialized.at("result").at("protocolVersion")=="2025-11-25","Server did not advertise supported protocol");
    error(rpc(session,"tools/list"),-32002);
    check(session.request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),"Initialization notification replied");
    error(rpc(session,"initialize"),-32600);
    ignore_responses();
    const auto listed=rpc(session,"tools/list").at("result").at("tools");
    check(listed.size()==2,"Unexpected tool count");
    check(calls==0,"Lifecycle or listing invoked backend");
    check(session.request(R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"poima_call","arguments":{"method":"world.transact"}}})").empty(),"Tool notification replied");
    check(calls==0,"Tool notification executed backend");
    const auto discovery=tool(session,Json::object(),"poima_discover");
    check(!discovery.at("isError").get<bool>()&&seen.at("method")=="world.describe"&&seen.at("params")==Json{{"view","catalog"}},"Default discovery not catalog");
    const Json parameters={{"request_id",std::string(32,'1')},{"base_revision",11}};
    auto result=tool(session,{{"method","world.transact"},{"params",parameters}});
    check(seen.at("params")==parameters&&result.at("structuredContent").at("result")==receipt,"Native parameters or receipt changed");
    const auto before_invalid=calls;
    for(const Json arguments:{Json{{"method",1}},Json{{"method",""}},Json{{"method","world.inspect"},{"params",false}},Json{{"method","world.inspect"},{"extra",1}}}) {
        result=tool(session,arguments);check(result.at("isError")==true&&result.at("structuredContent").at("error").at("code")==-32602,"Known tool argument error not tool result");
    }
    for(const Json arguments:{Json(nullptr),Json::array(),Json(true),Json(1),Json("bad")})
        error(rpc(session,"tools/call",{{"name","poima_call"},{"arguments",arguments}}),-32602);
    error(rpc(session,"tools/call",{{"name","unknown"}}),-32602);
    error(rpc(session,"tools/call",Json::array()),-32602);
    error(rpc(session,"not/a/method"),-32601);
    check(calls==before_invalid,"Malformed tool invoked backend");
    behavior="error";result=tool(session,{{"method","world.transact"}});
    check(result.at("isError")==true&&result.at("structuredContent")==Json{{"error",native_error}},"Native error details not preserved");
    for(const char* failure:{"throw","json","large","duplicate","id","both","hybrid","bad-error","hybrid"}) {
        behavior=failure;const auto before=calls;
        result=tool(session,{{"method","world.transact"},{"params",parameters}});
        check(calls==before+1,"Unknown outcome retried backend");
        check(result.at("isError")==true&&result.at("structuredContent").at("outcome_unknown")==true,"Ambiguous outcome not marked unknown");
        check(result.at("structuredContent").at("error").at("code")==-32603,"Ambiguous transport error code differs");
    }
    behavior="success";
    for(const Json id:{Json("same-id"),Json("same-id"),Json(0),Json(-1)})rpc(session,"ping",Json::object(),id);
    for(const Json id:{Json(nullptr),Json(true),Json(1.25),Json::array(),Json::object()}) {
        const auto reply=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",id},{"method","ping"}}.dump()));error(reply,-32600);
    }
    const auto before_parse=calls;
    for(const auto& input:{std::string("{"),std::string(R"({"jsonrpc":"2.0","id":1,"id":2,"method":"ping"})"),std::string(65,'[')+"0"+std::string(65,']')})
        error(Json::parse(session.request(input)),-32700);
    check(Json::parse(session.request(std::string(1024*1024+1,' '))).contains("error"),"Oversized request accepted");
    check(calls==before_parse,"Malformed message called backend");
    image_checks();
    std::cout<<Json{{"passed",true},{"backend_calls",calls},{"scope","In-process MCP envelope/lifecycle and exact-once adapter behavior; not socket transport qualification"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
