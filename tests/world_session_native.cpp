// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "poima/audio.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <map>
#include <cmath>

namespace fs=std::filesystem;
using Json=nlohmann::json;
void check(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
Json call(poima::WorldSession& session,const char* method,Json params=Json::object()) {
    const auto response=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
    if(response.contains("error"))throw std::runtime_error(response.at("error").dump());return response.at("result");
}
std::string read(const fs::path& path) { std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}}; }
void write(const fs::path& path,const std::string& bytes) { std::ofstream stream(path,std::ios::binary);stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(stream),"Fixture write failed."); }
std::string hash(const std::string& bytes) { return poima::sha256(std::as_bytes(std::span(bytes.data(),bytes.size()))); }
std::map<std::string,std::string> tree(const fs::path& root) { std::map<std::string,std::string> files;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file())files.emplace(entry.path().lexically_relative(root).generic_string(),read(entry.path()));return files; }
void custom_read_only_regression(const fs::path& directory) {
    const auto path=directory/"custom-read-only.json";const std::string type(32,'1'),field(32,'2'),entity(32,'3'),session_id(32,'4');
    const Json schema={{"id",type},{"name","Health"},{"version",1},{"fields",Json::array({{{"id",field},{"name","Current"},{"kind","int64"},{"default","0"}}})}};
    {
        poima::WorldSession author(path.string());
        call(author,"world.transact",{{"request_id",std::string(32,'5')},{"base_revision",0},{"ops",Json::array({
            {{"op","component.schema.set"},{"schema",schema}},{{"op","entity.create"},{"id",entity},{"name","Readonly component"}},
            {{"op","component.set"},{"id",entity},{"type","game:"+type},{"value",{{field,"9223372036854775807"}}}}
        })}});
    }
    const auto before=tree(directory);
    {
        poima::WorldSession session(path.string(),poima::WorldOpenMode::read_only_runtime);
        check(call(session,"component.schemas").at("schemas").size()==1,"Readonly v2 schema registry missing.");
        const auto discovery=call(session,"world.describe");check(!discovery.at("methods").contains("component.schema.import"),"Readonly discovery advertises schema import.");
        const auto rejected=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method","component.schema.import"},{"params",Json::object()}}.dump()));
        check(rejected.at("error").at("code")==-32081,"Readonly schema import reached validation or mutation.");
        const auto content=session.package_content();const auto document=Json::parse(content.document);
        check(document.at("version")==2 && document.at("component_schemas").contains(type) && document.at("retired_component_schemas").empty(),"Build-only v2 content lost schema registry or retirement sanitation.");
        check(document.at("entities").at(entity).at("components").at("game:"+type).at(field)=="9223372036854775807","Build-only v2 Int64 payload lost precision.");
        if(poima::Runtime::available()) {
            call(session,"runtime.start",{{"session_id",session_id},{"revision",1}});
            const auto status=session.component_status();check(status.active && status.session_id==session_id && status.tick==0 && status.revision==0,"Cheap component status differs from native state.");
            call(session,"runtime.component.edit",{{"session_id",session_id},{"request_id",std::string(32,'6')},{"expected_tick",0},{"expected_revision",0},{"id",entity},{"type",type},{"values",{{field,"-9223372036854775808"}}}});
            check(session.component_status().revision==1,"Cheap component revision did not observe paused write.");
        }
        check(tree(directory)==before,"Readonly custom runtime/build changed bundle files.");
    }
    check(tree(directory)==before,"Readonly custom teardown changed bundle files.");
}
void authored_preview_regression(const fs::path& directory) {
    const auto path=directory/"preview.json",assets=fs::path(path).concat(".assets");fs::create_directory(assets);
    // An analytic one-joint skin proves a bone preview rebuilds palettes, rather
    // than merely translating the SceneObject matrix after snapshot construction.
    auto mesh=std::make_shared<poima::MeshAsset>();mesh->vertices.resize(3);mesh->indices={0,1,2};
    mesh->vertices[1].position={1,0,0};mesh->vertices[2].position={0,1,0};
    // This untextured fixture has no UV0, so the cooked tangent sentinel is w=0.
    for(auto& vertex:mesh->vertices) { vertex.normal={0,0,1};vertex.tangent={1,0,0,0}; }
    mesh->influences.assign(3,poima::SkinWeight{{0,0,0,0},{1,0,0,0}});
    poima::ModelAsset model;model.primitives={mesh};model.nodes.resize(2);model.roots={0,1};
    model.nodes[0].name="Mesh";model.nodes[0].primitives={0};model.nodes[0].skin=0;
    model.nodes[1].name="Joint";model.nodes[1].position={0,1,0};
    auto inverse_bind=poima::identity_matrix();inverse_bind[13]=-1;
    model.skins.push_back({"Joint skin",1,{1},{inverse_bind}});
    const auto encoded=poima::encode_model(model),asset=hash(encoded);write(assets/(asset+".pmodel"),encoded);
    const std::string parent(32,'1'),child(32,'2'),light(32,'3'),rig(32,'4');
    const auto bone=hash("poima.instance.v1/"+rig+"/node/1").substr(0,32);
    auto transform=[](Json position){return Json{{"position",position},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};};
    poima::WorldSession session(path.string());
    call(session,"world.transact",{{"request_id",std::string(32,'5')},{"base_revision",0},{"ops",Json::array({
        {{"op","entity.create"},{"id",parent},{"name","Preview parent"}},
        {{"op","entity.create"},{"id",child},{"name","Child mesh"},{"parent",parent}},
        {{"op","component.set"},{"id",child},{"type","Transform"},{"value",transform({1,2,3})}},
        {{"op","component.set"},{"id",child},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"albedo",{1,0,0}},{"visible",true}}}},
        {{"op","entity.create"},{"id",light},{"name","Child light"},{"parent",parent}},
        {{"op","component.set"},{"id",light},{"type","Transform"},{"value",transform({0,1,0})}},
        {{"op","component.set"},{"id",light},{"type","Light"},{"value",{{"kind","spot"},{"color",{1,1,1}},{"intensity",2},{"enabled",true},{"range",20}}}},
        {{"op","asset.instantiate"},{"id",rig},{"name","Preview skin"},{"asset",asset},{"parent",parent}}
    })}});
    poima::EditorCamera camera;camera.world[14]=8;
    const auto before=session.authored_snapshot(camera);
    const auto inspected=call(session,"world.inspect"),history=call(session,"world.history");const auto files=tree(directory);
    const auto object=[&](const poima::SceneSnapshot& scene,const std::string& id)->const poima::SceneObject& {
        for(const auto& item:scene.objects)if(item.entity_id==id)return item;
        throw std::runtime_error("Preview fixture object missing.");
    };
    const auto skin=[](const poima::SceneSnapshot& scene)->const poima::SkinPose& {
        for(const auto& item:scene.objects)if(item.skin)return *item.skin;
        throw std::runtime_error("Preview fixture skin missing.");
    };
    const auto close=[](double a,double b){check(std::isfinite(a)&&std::abs(a-b)<1e-8,"Preview analytic transform differs.");};
    const auto saved_world=object(before,child).world;const auto saved_palette=skin(before).palette;
    const double q=std::sqrt(.5);
    const auto preview=session.authored_preview(camera,parent,{3,4,5},{0,q,0,q},{2,3,4});
    check(preview.revision==before.revision&&preview.world_id==before.world_id,"Preview changed authored source identity.");
    const auto& moved=object(preview,child).world;close(moved[12],15);close(moved[13],10);close(moved[14],3);
    check(preview.lighting.lights.size()==1,"Preview lost authored light.");
    const auto& lamp=preview.lighting.lights.front();close(lamp.position[0],3);close(lamp.position[1],7);close(lamp.position[2],5);
    close(lamp.direction[0],-1);close(lamp.direction[1],0);close(lamp.direction[2],0);
    const auto bone_preview=session.authored_preview(camera,bone,{0,3,0},{0,0,0,1},{1,1,1});
    check(skin(bone_preview).palette.size()==1,"Preview skin joint count differs.");close(skin(bone_preview).palette[0][13],2);
    check(skin(before).palette==saved_palette&&object(before,child).world==saved_world,"Preview mutated an earlier snapshot.");
    const auto normal=session.authored_snapshot(camera);
    check(object(normal,child).world==saved_world&&skin(normal).palette==saved_palette,"Preview poisoned normal snapshot cache.");
    close(normal.lighting.lights.front().position[1],1);
    bool rejected=false;try {(void)session.authored_preview(camera,parent,{0,0,0},{0,0,0,1},{0,1,1});}catch(const std::exception&){rejected=true;}
    check(rejected,"Invalid preview scale accepted.");
    check(call(session,"world.inspect")==inspected&&call(session,"world.history")==history&&tree(directory)==files,
          "Transient preview changed authoring state, history, receipts or storage.");
}
void game_camera_regression(const fs::path& directory) {
    const auto path=directory/"game-cameras.json";
    const std::string platform(32,'6'),camera(32,'7'),scaled(32,'8'),runtime_id(32,'9');
    const Json lens={{"vertical_fov",72},{"near",.125},{"far",600}};
    auto transform=[](Json position,Json scale=Json::array({1,1,1})) {
        return Json{{"position",position},{"rotation",{0,0,0,1}},{"scale",scale}};
    };
    poima::WorldSession session(path.string());
    const auto rejects=[](auto&& action,const char* message) {
        bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,message);
    };
    const auto near=[](double a,double b){check(std::isfinite(a)&&std::abs(a-b)<1e-5,"Game camera pose/lens differs.");};
    check(session.cameras(false).empty(),"Empty authored world acquired a game camera.");
    rejects([&]{(void)session.cameras(true);},"Inactive runtime camera enumeration succeeded.");
    rejects([&]{(void)session.runtime_camera_snapshot(camera);},"Inactive runtime camera snapshot succeeded.");
    call(session,"world.transact",{{"request_id",std::string(32,'a')},{"base_revision",0},{"ops",Json::array({
        {{"op","entity.create"},{"id",platform},{"name","Moving camera platform"}},
        {{"op","component.set"},{"id",platform},{"type","BoxCollider"},{"value",{{"half_extents",{.5,.5,.5}},{"motion","kinematic"},{"mass",1},{"friction",.5},{"restitution",0}}}},
        {{"op","component.set"},{"id",platform},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"albedo",{.4,.5,.6}},{"visible",true}}}},
        {{"op","entity.create"},{"id",camera},{"name","Game camera"},{"parent",platform}},
        {{"op","component.set"},{"id",camera},{"type","Transform"},{"value",transform({0,1,3})}},
        {{"op","component.set"},{"id",camera},{"type","Camera"},{"value",lens}},
        {{"op","entity.create"},{"id",scaled},{"name","Invalid scaled camera"}},
        {{"op","component.set"},{"id",scaled},{"type","Transform"},{"value",transform({0,0,0},{2,1,1})}},
        {{"op","component.set"},{"id",scaled},{"type","Camera"},{"value",lens}}
    })}});
    const auto authored=session.cameras(false);
    check(authored.size()==2&&authored[0].id==camera&&authored[1].id==scaled,"Camera enumeration is absent or unsorted.");
    near(authored[0].vertical_fov,72);near(authored[0].near_plane,.125);near(authored[0].far_plane,600);
    const auto files=tree(directory);const auto before_history=call(session,"world.history");
    const auto snapshot=session.authored_camera_snapshot(camera);
    check(snapshot.camera_id==camera&&snapshot.revision==1&&snapshot.objects.size()==1,"Authored game snapshot identity/content differs.");
    near(snapshot.camera_world[12],0);near(snapshot.camera_world[13],1);near(snapshot.camera_world[14],3);
    near(snapshot.vertical_fov,72);near(snapshot.near_plane,.125);near(snapshot.far_plane,600);
    rejects([&]{(void)session.authored_camera_snapshot(scaled);},"Scaled authored camera accepted.");
    rejects([&]{(void)session.authored_camera_snapshot(platform);},"Non-camera entity accepted as game camera.");
    rejects([&]{(void)session.authored_camera_snapshot(std::string(32,'f'));},"Missing authored camera accepted.");
    check(tree(directory)==files&&call(session,"world.history")==before_history,"Camera observation or failure changed storage/history.");
    if(!poima::Runtime::available())return;
    call(session,"runtime.start",{{"session_id",runtime_id},{"revision",1}});
    const auto initial=session.runtime_camera_snapshot(camera);
    check(initial.camera_world==snapshot.camera_world&&initial.camera_id==camera,"Runtime camera did not freeze initial authored pose.");
    rejects([&]{(void)session.runtime_camera_snapshot(scaled);},"Scaled runtime camera accepted.");
    rejects([&]{(void)session.runtime_camera_snapshot(platform);},"Runtime non-camera accepted.");
    call(session,"world.transact",{{"request_id",std::string(32,'b')},{"base_revision",1},{"ops",Json::array({
        {{"op","component.set"},{"id",camera},{"type","Camera"},{"value",{{"vertical_fov",30},{"near",.2},{"far",250}}}},
        {{"op","component.set"},{"id",camera},{"type","Transform"},{"value",transform({100,2,4})}}
    })}});
    const auto edited=session.authored_camera_snapshot(camera),frozen=session.runtime_camera_snapshot(camera);
    near(edited.vertical_fov,30);near(edited.camera_world[12],100);
    check(frozen.camera_world==initial.camera_world&&frozen.revision==1,"Authored edit changed frozen runtime camera.");
    near(frozen.vertical_fov,72);near(frozen.near_plane,.125);near(frozen.far_plane,600);
    check(session.cameras(false)[0].vertical_fov==30&&session.cameras(true)[0].vertical_fov==72,"Camera enumeration ignored runtime lens freeze.");
    call(session,"world.transact",{{"request_id",std::string(32,'c')},{"base_revision",2},{"ops",Json::array({
        {{"op","entity.delete"},{"id",camera},{"recursive",false}}
    })}});
    check(session.cameras(false).size()==1&&session.cameras(true).size()==2,"Authored deletion changed frozen camera enumeration.");
    rejects([&]{(void)session.authored_camera_snapshot(camera);},"Deleted authored camera returned a stale snapshot.");
    const auto step_files=tree(directory);const auto history=call(session,"world.history");
    call(session,"runtime.step",{{"session_id",runtime_id},{"request_id",std::string(32,'d')},{"expected_tick",0},{"ticks",60},
        {"motions",Json::array({{{"entity",platform},{"position",{6,0,0}},{"rotation",{0,0,0,1}},{"duration_ticks",60}}})}});
    const auto moved=session.runtime_camera_snapshot(camera);
    near(moved.camera_world[12],6);near(moved.camera_world[13],1);near(moved.camera_world[14],3);
    check(moved.camera_id==camera&&moved.revision==1&&session.runtime_status().tick==60,"Game snapshot advanced ticks or changed frozen source.");
    near(moved.vertical_fov,72);near(initial.camera_world[12],0);
    check(tree(directory)==step_files&&call(session,"world.history")==history,"Runtime camera movement changed authored storage/history.");
    call(session,"runtime.stop",{{"session_id",runtime_id}});
    rejects([&]{(void)session.cameras(true);},"Stopped runtime exposed stale camera metadata.");
    rejects([&]{(void)session.runtime_camera_snapshot(camera);},"Stopped runtime exposed stale camera snapshot.");
}
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
        {
            // Constructor-level read-only enforcement cannot be bypassed by
            // selecting a different RPC transport scope.
            const auto frozen=directory/"readonly.json";write(frozen,read(path));const auto original=read(frozen);
            const auto profile=frozen.parent_path()/"packaged.poima-input.json";
            Json controls={{"bindings",{{"forward",{"key.up"}},{"backward",{"key.s"}},{"left",{"key.a"}},{"right",{"key.d"}},{"jump",{"key.space"}},{"use",{"key.e"}}}},{"sensitivity_x",.1},{"sensitivity_y",.1},{"invert_x",false},{"invert_y",false}};
            // A persisted revision-1 profile must carry its first transaction
            // receipt. Create the fixture through the public authoring API.
            {
                poima::WorldSession author(path.string());
                call(author,"input.transact",{{"path",profile.string()},{"request_id",std::string(32,'b')},{"expected_revision",0},{"profile",controls}});
            }
            fs::remove(fs::path(profile).concat(".lock"));
            const auto before=tree(directory);
            poima::WorldSession session(frozen.string(),poima::WorldOpenMode::read_only_runtime);
            check(call(session,"world.inspect")["read_only"]==true,"Read-only mode is not discoverable.");
            const auto discovery=call(session,"world.describe");
            check(discovery["schema_revision"]==35,"Read-only discovery schema revision differs.");
            check(discovery["methods"].contains("world.dependencies") && !discovery["methods"].contains("world.transact"),"Read-only discovery advertises mutation or hides dependencies.");
            for(const auto* method:{"world.transact","world.undo","world.redo","asset.import","asset.image.import","asset.audio.import","input.transact"})for(const auto scope:{poima::WorldRequestScope::standalone,poima::WorldRequestScope::shared_headless,poima::WorldRequestScope::shared_editor}) {
                const auto response=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",9},{"method",method},{"params",{{"source","missing"},{"path","forbidden.poima-input.json"}}}}.dump(),scope));
                check(response["error"]["code"]==-32081,"Read-only operation reached validation or mutation.");
            }
            const auto inspected=call(session,"input.inspect",{{"path","packaged.poima-input.json"}});check(inspected["revision"]==1 && inspected["profile"]==controls,"Read-only input inspection differs.");
            const auto evaluated=call(session,"input.evaluate",{{"path","packaged.poima-input.json"},{"events",Json::array({{{"control","key.up"},{"down",true}},{{"consume",true}}})}});
            check(evaluated["frames"][0]["move"][1]==1,"Read-only profile did not reach native evaluator.");
            if(poima::Runtime::available()) {
                call(session,"runtime.start",{{"session_id",std::string(32,'c')},{"revision",3}});
                check(call(session,"runtime.step",{{"session_id",std::string(32,'c')},{"request_id",std::string(32,'d')},{"expected_tick",0},{"ticks",2}})["tick"]==2,"Read-only world cannot simulate.");
                call(session,"runtime.stop",{{"session_id",std::string(32,'c')}});
            }
            check(session.package_content().assets.empty(),"Built-in geometry acquired an external dependency.");
            check(read(frozen)==original && tree(directory)==before,"Read-only observation or runtime created files/sidecars.");
            bool failed=false;try { poima::WorldSession missing((directory/"absent.json").string(),poima::WorldOpenMode::read_only_runtime); }catch(const std::exception&) { failed=true; }
            check(failed && tree(directory)==before,"Read-only missing world created storage.");
        }
        {
            const auto package=directory/"package.json";const auto assets=fs::path(package).concat(".assets");fs::create_directory(assets);
            auto embedded=std::make_shared<poima::TextureImage>();embedded->mips.push_back({1,1,{128,128,255,255}});
            poima::TextureImage texture;texture.srgb=true;texture.mips.push_back({1,1,{220,170,100,255}});
            auto mesh=std::make_shared<poima::MeshAsset>();mesh->vertices.resize(3);mesh->indices={0,1,2};mesh->has_uv=true;
            mesh->vertices[1].position={1,0,0};mesh->vertices[2].position={0,1,0};mesh->vertices[1].uv={1,0};mesh->vertices[2].uv={0,1};
            for(auto& vertex:mesh->vertices) { vertex.normal={0,0,1};vertex.tangent={1,0,0,1}; }
            poima::ModelAsset model;model.primitives.push_back(mesh);model.images.push_back(embedded);poima::ModelNode node;node.name="Triangle";node.primitives={0};model.nodes.push_back(node);model.roots={0};
            const auto model_bytes=poima::encode_model(model),image_bytes=poima::encode_image(texture),audio_bytes=poima::encode_audio(poima::AudioClip{{0,.1f,-.1f}});
            const auto model_id=hash(model_bytes),image_id=hash(image_bytes),audio_id=hash(audio_bytes);
            write(assets/(model_id+".pmodel"),model_bytes);write(assets/(image_id+".pimage"),image_bytes);write(assets/(audio_id+".paudio"),audio_bytes);
            auto document=Json::parse(read(path));auto& components=document["entities"][entity]["components"];components.erase("MeshRenderer");
            components["StaticMesh"]={{"asset",model_id},{"primitive",0},{"visible",false}};
            components["PbrTextures"]={{"base_color",{{"asset",image_id}}},{"normal",{{"asset",model_id},{"image",0}}}};
            components["AudioEmitter"]={{"asset",audio_id},{"gain",1},{"loop",false},{"enabled",false}};
            document["retired_ids"]={std::string(32,'e')};document["receipts"].push_back({{"params",{{"request_id",std::string(32,'f')},{"asset",std::string(64,'0')}}},{"result",{{"revision",3}}}});write(package,document.dump());
            const auto before=tree(directory);
            poima::WorldSession session(package.string(),poima::WorldOpenMode::read_only_runtime);
            const auto content=session.package_content();const auto clean=Json::parse(content.document);
            check(content.revision==3 && content.needs_audio && content.assets.size()==3,"Hidden mesh/disabled audio/texture closure incomplete or duplicated.");
            check(clean["entities"]==document["entities"] && clean["world_id"]==document["world_id"] && clean["revision"]==3 && clean["receipts"].empty() && clean["retired_ids"].empty(),"Package sanitation changed live content or retained authoring history.");
            std::string previous;for(const auto& asset:content.assets) { check(previous.empty() || previous<asset.filename,"Package assets are not sorted and unique.");previous=asset.filename;const auto bytes=read(assets/asset.filename);check(asset.sha256==hash(bytes) && asset.bytes==bytes.size(),"Package metadata does not identify exact source bytes."); }
            const auto dependencies=call(session,"world.dependencies");check(dependencies["assets"].size()==3 && !dependencies.contains("document"),"Dependencies exposes document bytes or lost assets.");
            check(tree(directory)==before,"Dependency export mutated source files.");
            // Fresh verification must not trust an earlier model/image cache.
            write(assets/(image_id+".pimage"),"corrupted");bool failed=false;try { (void)session.package_content(); }catch(const std::exception&) { failed=true; }check(failed,"Package closure trusted cached corrupt bytes.");write(assets/(image_id+".pimage"),image_bytes);
            fs::remove(assets/(audio_id+".paudio"));failed=false;try { (void)session.package_content(); }catch(const std::exception&) { failed=true; }check(failed,"Missing disabled audio package was ignored.");write(assets/(audio_id+".paudio"),audio_bytes);
            const auto outside=directory/"linked-image.pimage";write(outside,image_bytes);fs::remove(assets/(image_id+".pimage"));std::error_code link_error;
            fs::create_symlink(outside,assets/(image_id+".pimage"),link_error);
            if(!link_error) { failed=false;try { (void)session.package_content(); }catch(const std::exception&) { failed=true; }check(failed && read(outside)==image_bytes,"Linked package was followed or changed.");fs::remove(assets/(image_id+".pimage")); }
#ifndef _WIN32
            else throw std::runtime_error("Native package symlink fixture failed: "+link_error.message());
#endif
            write(assets/(image_id+".pimage"),image_bytes);
            const auto moved=directory/"actual-assets";fs::rename(assets,moved);link_error.clear();fs::create_directory_symlink(moved,assets,link_error);
            if(!link_error) { failed=false;try { (void)session.package_content(); }catch(const std::exception&) { failed=true; }check(failed,"Linked asset directory was followed.");fs::remove(assets); }
#ifndef _WIN32
            else throw std::runtime_error("Native asset-directory symlink fixture failed: "+link_error.message());
#endif
            fs::rename(moved,assets);
            document["entities"][entity]["components"]["PbrTextures"]["normal"]["image"]=1;write(package,document.dump());
            poima::WorldSession invalid(package.string(),poima::WorldOpenMode::read_only_runtime);failed=false;try { (void)invalid.package_content(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid embedded texture index was bundled.");
        }
        custom_read_only_regression(directory);
        authored_preview_regression(directory);
        game_camera_regression(directory);
        fs::remove_all(directory);std::cout<<"Shared native session, external cameras, immutable snapshots, frozen runtime, undo/redo, transient transform preview and protocol adapter passed.\n";
    }catch(const std::exception& error) { fs::remove_all(directory);std::cerr<<error.what()<<'\n';return 1; }
}
