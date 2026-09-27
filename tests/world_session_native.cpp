// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using Json=nlohmann::json;
void check(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
Json call(poima::WorldSession& session,const char* method,Json params=Json::object()) {
    const auto response=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
    if(response.contains("error"))throw std::runtime_error(response.at("error").dump());return response.at("result");
}
std::string read(const fs::path& path) { std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}}; }
int main() {
    const auto directory=fs::current_path()/("world-session-native-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(directory);const auto path=directory/"world.json";const std::string entity(32,'a');
        {
            poima::WorldSession session(path.string());poima::EditorCamera camera;camera.world[14]=5;
            const auto initial_status=session.runtime_status();
            check(!initial_status.active && initial_status.available==poima::Runtime::available(),"Native runtime discovery differs from build.");
            const auto status=call(session,"runtime.status");
            check(status["active"]==false && status["session_id"].is_null(),"Inactive runtime status leaked stale identity.");
            for(const auto* method:{"session.close","host.shutdown","world.capture","runtime.capture","asset.animation.capture","runtime.play"}) {
                const Json request={{"jsonrpc","2.0"},{"id",7},{"method",method},{"params",Json::object()}};
                check(Json::parse(session.request(request.dump(),poima::WorldRequestScope::shared_editor))["error"]["code"]==-32080,"Shared editor accepted an owner-only operation.");
                auto notification=request;notification.erase("id");
                check(session.request(notification.dump(),poima::WorldRequestScope::shared_editor).empty() && !session.closed(),"Rejected shared notification affected owner lifetime.");
            }
            const auto discovery=Json::parse(session.request(R"({"jsonrpc":"2.0","id":8,"method":"world.describe"})",poima::WorldRequestScope::shared_editor))["result"];
            check(discovery["session_scope"]=="shared_editor" && !discovery["methods"].contains("session.close") && !discovery["methods"].contains("world.capture") && discovery["editor_discovery"]=="editor.describe","Shared editor discovery advertised unsupported operations.");
            const auto empty=session.authored_snapshot(camera);check(empty.objects.empty() && !fs::exists(path),"Empty snapshot persisted a world.");
            call(session,"world.transact",{{"request_id",std::string(32,'1')},{"base_revision",0},{"ops",Json::array({
                {{"op","entity.create"},{"id",entity},{"name","Cube"}},
                {{"op","component.set"},{"id",entity},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"albedo",{1,0,0}},{"visible",true}}}}
            })}});
            const auto bytes=read(path);const auto first=session.authored_snapshot(camera);camera.world[12]=2;
            const auto second=session.authored_snapshot(camera);
            check(first.objects.size()==1 && second.objects.size()==1,"Authored snapshots lost mesh.");
            check(first.camera_world[12]==0 && second.camera_world[12]==2,"Editor camera leaked between immutable snapshots.");
            check(first.objects[0].world==second.objects[0].world && read(path)==bytes,"Snapshot mutated authored state.");
            check(first.camera_id=="editor" && first.revision==1,"Snapshot source identity differs.");
            auto bad=camera;bad.world[0]=2;bool rejected=false;
            try { (void)session.authored_snapshot(bad); }catch(const std::exception&) { rejected=true; }check(rejected,"Scaled editor camera accepted.");
            if(poima::Runtime::available()) {
                call(session,"runtime.start",{{"session_id",std::string(32,'b')},{"revision",1}});
                const auto started=session.runtime_status();
                check(started.active && started.tick==0 && started.authored_revision==1 && started.session_id==std::string(32,'b'),"Native active runtime discovery differs.");
                const auto live=session.runtime_snapshot(camera);check(live.objects.size()==1 && live.revision==1,"Runtime external-camera snapshot failed.");
                call(session,"world.undo",{{"request_id",std::string(32,'2')},{"base_revision",1}});
                check(session.authored_snapshot(camera).objects.empty(),"Undo left authored snapshot cache stale.");
                check(session.runtime_snapshot(camera).objects.size()==1,"Authoring undo mutated frozen runtime.");
            }else call(session,"world.undo",{{"request_id",std::string(32,'2')},{"base_revision",1}});
            call(session,"world.redo",{{"request_id",std::string(32,'3')},{"base_revision",2}});
            check(session.authored_snapshot(camera).objects.size()==1 && first.objects.size()==1,"Redo or old snapshot ownership failed.");
            check(Json::parse(session.request("{"))["error"]["code"]==-32700,"Native parse error differs from CLI.");
            check(session.request("{\"jsonrpc\":\"2.0\",\"method\":\"world.inspect\"}").empty(),"Notification unexpectedly returned a response.");
            check(session.request("{\"jsonrpc\":\"2.0\",\"method\":\"session.close\"}").empty() && session.closed(),"Native close notification failed.");
            check(Json::parse(session.request("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"world.inspect\"}"))["error"]["code"]==-32001,"Closed session accepted a request.");
        }
        {
            poima::WorldSession reopened(path.string());const auto history=call(reopened,"world.history");
            check(history["undo_count"]==0 && history["redo_count"]==0 && history["revision"]==3,"History should be session-local while revision persists.");
            const auto replay=call(reopened,"world.redo",{{"request_id",std::string(32,'3')},{"base_revision",2}});
            check(replay["replayed"]==true && replay["revision"]==3,"Undo/redo receipt did not survive restart.");
        }
        fs::remove_all(directory);std::cout<<"Shared native session, external cameras, immutable snapshots, frozen runtime, undo/redo and protocol adapter passed.\n";
    }catch(const std::exception& error) { fs::remove_all(directory);std::cerr<<error.what()<<'\n';return 1; }
}
