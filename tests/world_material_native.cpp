// SPDX-License-Identifier: Apache-2.0
// Source proposal: real CPU World/Store contracts; no renderer or gameplay ABI.
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/save_store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string id(std::uint64_t value) {std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(32)<<value;return out.str();}
std::string path_text(const fs::path& path) {const auto value=path.u8string();return {value.begin(),value.end()};}
std::string read(const fs::path& path) {std::ifstream in(path,std::ios::binary);check(bool(in),"Cannot read fixture file.");return {std::istreambuf_iterator<char>(in),{}};}
Json request(poima::WorldSession& world,const char* method,const Json& params=Json::object()) {
    return Json::parse(world.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
}
Json call(poima::WorldSession& world,const char* method,const Json& params=Json::object()) {
    auto response=request(world,method,params);
    if(response.contains("error"))throw std::runtime_error(std::string(method)+": "+response.at("error").dump());
    return response.at("result");
}
Json pbr(unsigned variant=0) {
    // Exactly representable factors distinguish all four editable domains.
    return variant==0 ? Json{{"base_color",{.25,.5,.75}},{"emissive",{0.,.125,.25}},
        {"metallic",.25},{"roughness",.75},{"double_sided",false}} :
        Json{{"base_color",{.75,.25,.5}},{"emissive",{.25,.5,.125}},
        {"metallic",.75},{"roughness",.25},{"double_sided",false}};
}
Json transform(double height=0) {return {{"position",{0.,height,0.}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};}
Json material(const poima::PbrMaterial& value) {
    return {{"base_color",value.base_color},{"emissive",value.emissive},{"metallic",value.metallic},
        {"roughness",value.roughness},{"double_sided",value.double_sided}};
}
const poima::SceneObject& object(const poima::SceneSnapshot& scene,const std::string& identity) {
    const auto found=std::find_if(scene.objects.begin(),scene.objects.end(),[&](const auto& row){return row.entity_id==identity;});
    check(found!=scene.objects.end(),"Drawable absent from immutable scene.");return *found;
}
void unchanged_scene(const poima::SceneSnapshot& before,const poima::SceneSnapshot& after) {
    check(before.world_id==after.world_id && before.revision==after.revision &&
        before.presentation_source_id==after.presentation_source_id && before.presentation_generation==after.presentation_generation &&
        before.material_revision==after.material_revision && before.objects.size()==after.objects.size(),"Rejected operation changed scene identity.");
    for(const auto& a:before.objects) {
        const auto& b=object(after,a.entity_id);
        check(a.world==b.world && a.mesh==b.mesh && a.textures==b.textures && a.skin==b.skin && a.incarnation==b.incarnation &&
            a.albedo==b.albedo && a.cast_shadows==b.cast_shadows && a.receive_shadows==b.receive_shadows &&
            a.material.has_value()==b.material.has_value() && (!a.material || material(*a.material)==material(*b.material)),
            "Rejected operation changed a drawable or its immutable owners.");
    }
}

class Fixture {
public:
    fs::path path,storage;
    poima::WorldSession world;
    std::string session=id(100);
    std::uint64_t sequence=1000;
    Json inspected_ids=Json::array({id(1),id(2),id(3)});
    const std::string type=id(20),field=id(21);
    explicit Fixture(const fs::path& directory):path(directory/"world.json"),storage(directory/"saves"),world(path_text(path)) {
        check(fs::create_directory(storage),"Cannot reserve save root.");
        Json ops=Json::array();
        ops.push_back({{"op","component.schema.set"},{"schema",{{"id",type},{"name","Retained values"},{"version",1},
            {"fields",Json::array({{{"id",field},{"name","Counter"},{"kind","int32"},{"default",0}}})}}}});
        for(unsigned value=1;value<=5;++value) {
            ops.push_back({{"op","entity.create"},{"id",id(value)},{"name","Material fixture"}});
            ops.push_back({{"op","component.set"},{"id",id(value)},{"type","Transform"},{"value",transform(value==1 ? 10 : 0)}});
            ops.push_back({{"op","component.set"},{"id",id(value)},{"type","MeshRenderer"},
                {"value",{{"primitive","box"},{"visible",value!=3},{"albedo",{.25,.5,.75}}}}});
            if(value!=4)ops.push_back({{"op","component.set"},{"id",id(value)},{"type","PbrMaterial"},{"value",pbr()}});
        }
        ops.push_back({{"op","component.set"},{"id",id(1)},{"type","BoxCollider"},
            {"value",{{"half_extents",{.5,.5,.5}},{"motion","dynamic"},{"mass",1},{"friction",.5},{"restitution",0}}}});
        ops.push_back({{"op","component.set"},{"id",id(1)},{"type","game:"+type},{"value",{{field,23}}}});
        ops.push_back({{"op","template.set"},{"id",id(30)},{"name","Spawnable"},
            {"components",{{"Transform",transform()},{"MeshRenderer",{{"primitive","box"},{"visible",true},{"albedo",{.25,.5,.75}}}},
                {"PbrMaterial",pbr()}}}});
        transact(ops);
        transact(Json::array({{{"op","entity.delete"},{"id",id(5)},{"recursive",false}}}));
        check(Json::parse(read(path)).at("retired_ids").size()==1,"Positive fixture lacks preexisting retired bookkeeping.");
        call(world,"save.configure",{{"request_id",next()},{"expected_generation",0},{"root",path_text(storage)}});
        call(world,"runtime.start",{{"session_id",session},{"revision",authored_revision()}});
        step(7);
        const auto values=call(world,"runtime.components",{{"session_id",session}});
        call(world,"runtime.component.edit",{{"session_id",session},{"request_id",next()},{"expected_tick",tick()},
            {"expected_revision",values.at("component_revision")},{"id",id(1)},{"type",type},{"values",{{field,37}}}});
        check(call(world,"runtime.entity",{{"session_id",session},{"id",id(1)}}).at("world_matrix").at(13)<10,
            "Physics fixture did not move before material adoption.");
    }
    std::string next() {return id(sequence++);}
    Json status() {return call(world,"runtime.inspect",{{"session_id",session}});}
    Json authored_revision() {return call(world,"world.inspect").at("revision");}
    Json tick() {return status().at("tick");}
    void transact(const Json& operations) {call(world,"world.transact",{{"request_id",next()},{"base_revision",authored_revision()},{"ops",operations}});}
    void edit(const std::string& identity,const std::string& type_name,const Json& value) {
        transact(Json::array({{{"op","component.set"},{"id",identity},{"type",type_name},{"value",value}}}));
    }
    void edit_materials(unsigned variant) {
        Json ops=Json::array();for(unsigned value=1;value<=3;++value)
            ops.push_back({{"op","component.set"},{"id",id(value)},{"type","PbrMaterial"},{"value",pbr(variant)}});
        transact(ops);
    }
    Json inspect() {return call(world,"runtime.material.inspect",{{"session_id",session},{"tick",tick()},{"ids",inspected_ids}});}
    Json apply() {
        const auto state=status();return {{"request_id",next()},{"session_id",session},{"expected_tick",state.at("tick")},
            {"expected_structure_revision",state.at("structure_revision")},{"expected_material_revision",state.at("material_revision")},
            {"revision",authored_revision()},{"expected_authored_revision",state.at("authored_revision")},
            {"expected_content_sha256",state.at("content_sha256")},{"ids",{id(1),id(2),id(3)}}};
    }
    Json step_params(unsigned count=1) {const auto state=status();return {{"session_id",session},{"request_id",next()},
        {"expected_tick",state.at("tick")},{"expected_structure_revision",state.at("structure_revision")},{"ticks",count}};}
    Json step(unsigned count=1) {return call(world,"runtime.step",step_params(count));}
    Json write_params(const std::string& slot,bool material_guard=true) {
        const auto state=status(),components=call(world,"runtime.components",{{"session_id",session}});
        Json result={{"request_id",next()},{"configuration_generation",1},{"slot",slot},{"expected_generation",0},
            {"session_id",session},{"expected_tick",state.at("tick")},{"expected_gameplay_revision",0},
            {"expected_component_revision",components.at("component_revision")},{"expected_structure_revision",state.at("structure_revision")},
            {"expected_ui_revision",state.at("ui_revision")},{"expected_control_sequence",state.at("control_sequence")}};
        if(material_guard)result["expected_material_revision"]=state.at("material_revision");return result;
    }
    Json checkpoint(const std::string& slot) {
        const auto written=call(world,"save.write",write_params(slot));check(written.at("generation")==1,"Checkpoint did not publish generation one.");
        return Json::parse(poima::saves::Store(storage/("slot-"+slot)).read().bytes);
    }
    Json load_params(const std::string& slot,bool guard=true) {
        const auto state=status(),components=call(world,"runtime.components",{{"session_id",session}});
        Json result={{"request_id",next()},{"configuration_generation",1},{"slot",slot},{"expected_generation",1},
            {"revision",authored_revision()},{"expected_session_id",session},{"expected_tick",state.at("tick")},
            {"expected_gameplay_revision",0},{"expected_component_revision",components.at("component_revision")},
            {"expected_structure_revision",state.at("structure_revision")},{"expected_ui_revision",state.at("ui_revision")},
            {"expected_control_sequence",state.at("control_sequence")},{"new_session_id",next()}};
        if(guard)result["expected_material_revision"]=state.at("material_revision");return result;
    }
    Json state() {
        return {{"runtime",status()},{"materials",inspect()},{"body",call(world,"runtime.entity",{{"session_id",session},{"id",id(1)}})},
            {"components",call(world,"runtime.component.get",{{"session_id",session},{"tick",tick()},{"id",id(1)},{"type",type}})},
            {"world",call(world,"world.inspect")},{"history",call(world,"world.history")},{"authored_bytes",read(path)}};
    }
    void reject(const char* method,const Json& params,int code) {
        const auto before=state();const auto scene=world.runtime_snapshot(poima::EditorCamera{});
        const auto response=request(world,method,params);
        check(response.contains("error") && response.at("error").at("code")==code,"Unexpected material contract rejection.");
        check(state()==before,"Rejected operation changed committed runtime, authoring, history or files.");
        unchanged_scene(scene,world.runtime_snapshot(poima::EditorCamera{}));
    }
};
Json payload_domain(Json value) {value.erase("content_sha256");value.erase("authored_revision");return value;}
void valid_and_durable(const fs::path& directory) {
    Fixture f(directory);const auto old_scene=f.world.runtime_snapshot(poima::EditorCamera{});
    const auto original_write=f.write_params("original",false);
    call(f.world,"save.write",original_write); // Epoch zero preserves the old omitted-guard contract.
    const auto old_bytes=poima::saves::Store(f.storage/"slot-original").read().bytes;
    const auto original=Json::parse(old_bytes);
    check(call(f.world,"runtime.component.get",{{"session_id",f.session},{"tick",f.tick()},{"id",id(1)},{"type",f.type}}).at("values").at(f.field)==37 &&
        original.at("snapshot").at("payload").at("components").at("revision")==1,
        "Checkpoint fixture lacks modified custom state.");
    const auto old_status=f.status();f.edit_materials(1);
    check(f.status().at("source_stale")==true && f.inspect().at("materials").at(0).at("material")==pbr(),
        "Ordinary authored material edit changed the frozen runtime before apply.");
    const auto authored_bytes=read(f.path);
    const auto history=call(f.world,"world.history"),apply=f.apply();
    const auto accepted=call(f.world,"runtime.material.apply",apply);
    check(!accepted.at("replayed").get<bool>() && accepted.at("material_revision")==1 && accepted.at("tick")==7 &&
        accepted.at("authored_revision")==f.authored_revision() && accepted.at("ids")==apply.at("ids"),"Apply lost its guarded baseline receipt.");
    const auto live=f.inspect();
    const auto fresh_scene=f.world.runtime_snapshot(poima::EditorCamera{});
    check(live.at("material_revision")==1 && live.at("source_stale")==false && fresh_scene.material_revision==1 &&
        fresh_scene.presentation_generation==1 && fresh_scene.presentation_source_id==old_scene.presentation_source_id &&
        f.world.runtime_status().material_revision==1,"Material epoch did not reach exact native observations.");
    check(fresh_scene.objects.size()==old_scene.objects.size() && fresh_scene.objects.size()==3,"Hidden material adoption changed visibility.");
    for(unsigned i=1;i<=3;++i)check(live.at("materials").at(i-1).at("material")==pbr(1),"Selected scalar material was not adopted.");
    for(unsigned i:{1u,2u,4u}) {
        const auto& old=object(old_scene,id(i));const auto& next=object(fresh_scene,id(i));
        check(old.world==next.world && old.mesh==next.mesh && old.textures==next.textures && old.incarnation==next.incarnation &&
            old.albedo==next.albedo && old.cast_shadows==next.cast_shadows && old.receive_shadows==next.receive_shadows,
            "Apply replaced geometry, texture ownership, pose or incarnation.");
        if(i!=4)check(old.material && next.material && material(*old.material)==pbr() && material(*next.material)==pbr(1),
            "Apply mutated an older immutable snapshot or failed to publish new factors.");
    }
    check(read(f.path)==authored_bytes && call(f.world,"world.history")==history,"Apply wrote authoring or consumed undo history.");
    const auto changed=f.checkpoint("changed");
    check(payload_domain(changed.at("snapshot").at("payload")).dump()==payload_domain(original.at("snapshot").at("payload")).dump(),
        "Material adoption changed unrelated full checkpoint state.");
    check(changed.at("snapshot").at("payload").at("content_sha256")!=original.at("snapshot").at("payload").at("content_sha256") &&
        changed.at("document").at("entities").at(id(1)).at("components").at("PbrMaterial")==pbr(1),"Saved baseline failed to adopt material/content identity.");
    f.reject("save.write",f.write_params("missing-material",false),-32602);
    auto wrong=f.write_params("stale-material");wrong["expected_material_revision"]=0;f.reject("save.write",wrong,-32009);
    f.reject("save.load",f.load_params("original",false),-32602);
    wrong=f.load_params("original");wrong["expected_material_revision"]=0;f.reject("save.load",wrong,-32009);
    // Exact prior durable receipt remains recoverable after adoption.
    const auto before_storage_retry=f.state();
    check(call(f.world,"save.write",original_write).at("replayed")==true && f.state()==before_storage_retry &&
        poima::saves::Store(f.storage/"slot-original").read().bytes==old_bytes,"Prior durable receipt replay required a new material epoch or wrote again.");
    f.step();const auto before_retry=f.state();auto replay=call(f.world,"runtime.material.apply",apply);
    check(replay.at("replayed")==true && replay.at("material_revision")==1 && replay.at("tick")==7 && f.state()==before_retry,
        "Exact replay reapplied materials or required current clocks.");
    auto conflict=apply;conflict["expected_tick"]=8;f.reject("runtime.material.apply",conflict,-32010);
    for(const auto* key:{"expected_tick","expected_material_revision","expected_structure_revision","revision","expected_authored_revision"}) {
        auto alias=apply;alias[key]=alias.at(key).get<double>();f.reject("runtime.material.apply",alias,-32602);
        alias=apply;alias[key]=true;f.reject("runtime.material.apply",alias,-32602);
    }
    auto load=f.load_params("original");const auto loaded=call(f.world,"save.load",load);f.session=load.at("new_session_id");
    check(loaded.at("source_stale")==true && f.status().at("material_revision")==0 && f.inspect().at("materials").at(0).at("material")==pbr(),
        "Old checkpoint failed to restore its original frozen material baseline.");
    const auto old_restored=f.checkpoint("old-restored");check(old_restored.at("snapshot").dump()==original.at("snapshot").dump(),"Old restore changed full checkpoint state.");
    load=f.load_params("changed",false);call(f.world,"save.load",load);f.session=load.at("new_session_id");
    check(f.status().at("source_stale")==false && f.status().at("material_revision")==0 && f.inspect().at("materials").at(0).at("material")==pbr(1),
        "New checkpoint did not restore adopted baseline with a fresh session-local epoch.");
    check(f.checkpoint("new-restored").at("snapshot").dump()==changed.at("snapshot").dump(),"New restore changed full checkpoint state.");
    check(poima::saves::Store(f.storage/"slot-original").read().bytes==old_bytes,"Material adoption overwrote an older save slot.");
    // Existing history still requires explicit adoption, in both directions.
    call(f.world,"world.undo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    check(f.status().at("source_stale")==true && f.inspect().at("materials").at(0).at("material")==pbr(1),"Undo implicitly changed runtime materials.");
    call(f.world,"runtime.material.apply",f.apply());check(f.inspect().at("materials").at(0).at("material")==pbr(),"Explicit undo adoption failed.");
    call(f.world,"world.redo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    call(f.world,"runtime.material.apply",f.apply());check(f.status().at("material_revision")==2 && f.inspect().at("materials").at(0).at("material")==pbr(1),"Explicit redo adoption failed.");
    call(f.world,"runtime.stop",{{"session_id",f.session}});
    load={{"request_id",f.next()},{"configuration_generation",1},{"slot","changed"},{"expected_generation",1},
        {"revision",f.authored_revision()},{"expected_session_id",nullptr},{"expected_tick",nullptr},{"expected_gameplay_revision",nullptr},
        {"expected_material_revision",nullptr},{"new_session_id",f.next()}};
    call(f.world,"save.load",load);f.session=load.at("new_session_id");
    check(f.inspect().at("materials").at(0).at("material")==pbr(1),"Stopped nullable material guard prevented exact restore.");
}

void rejection_and_ownership(const fs::path& directory) {
    Fixture f(directory);f.edit_materials(1);const auto source=f.checkpoint("before-rejections");
    const auto valid=f.apply();
    for(const auto* key:{"expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision"}) {
        auto value=valid;value[key]=value.at(key).get<std::uint64_t>()+1;f.reject("runtime.material.apply",value,-32009);
    }
    auto value=valid;value["expected_content_sha256"]=std::string(64,'0');f.reject("runtime.material.apply",value,-32009);
    value=valid;value["expected_content_sha256"]=std::string(64,'A');f.reject("runtime.material.apply",value,-32602);
    for(const Json ids:{Json::array(),Json::array({id(2),id(1)}),Json::array({id(1),id(1)}),Json(nullptr),Json::array({7})}) {
        value=valid;value["ids"]=ids;f.reject("runtime.material.apply",value,-32602);
    }
    value=valid;value["ids"]=Json::array();for(unsigned i=1;i<=33;++i)value["ids"].push_back(id(i));f.reject("runtime.material.apply",value,-32602);
    value=valid;value["ids"]={id(1),id(999)};f.reject("runtime.material.apply",value,-32004);
    value=valid;value["ids"]={id(1),id(4)};f.reject("runtime.material.apply",value,-32004);
    value=valid;value["ids"]={id(1),id(2)};f.reject("runtime.material.apply",value,-32602); // Unselected edited hidden material.
    value=valid;value["preview"]=true;f.reject("runtime.material.apply",value,-32602);
    f.reject("runtime.material.inspect",{{"session_id",f.session},{"tick",f.tick()},{"material_revision",1},{"ids",{id(1)}}},-32009);
    f.reject("runtime.material.inspect",{{"session_id",f.session},{"tick",f.tick()},{"ids",{id(4)}}},-32004);
    // Each forbidden authored change is committed; failed adoption must leave
    // that edit and its undo history intact while retaining the old runtime.
    const auto forbidden=[&](const Json& operation) {
        f.transact(Json::array({operation}));f.reject("runtime.material.apply",f.apply(),-32602);
        call(f.world,"world.undo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    };
    forbidden({{"op","component.set"},{"id",id(1)},{"type","Transform"},{"value",transform(12)}});
    auto double_sided=pbr(1);double_sided["double_sided"]=true;
    forbidden({{"op","component.set"},{"id",id(1)},{"type","PbrMaterial"},{"value",double_sided}});
    forbidden({{"op","component.set"},{"id",id(1)},{"type","game:"+f.type},{"value",{{f.field,99}}}});
    forbidden({{"op","entity.rename"},{"id",id(1)},{"name","Unrelated name"}});
    forbidden({{"op","entity.create"},{"id",id(6)},{"name","Unrelated content"}});
    forbidden({{"op","component.remove"},{"id",id(1)},{"type","game:"+f.type}});
    forbidden({{"op","component.set"},{"id",id(2)},{"type","MeshRenderer"},
        {"value",{{"primitive","box"},{"visible",false},{"albedo",{.25,.5,.75}}}}});
    // Failed attempts do not reserve their IDs: the original guarded ID may
    // succeed after effective unrelated content is undone.
    auto accepted=f.apply();accepted["request_id"]=valid.at("request_id");call(f.world,"runtime.material.apply",accepted);
    const auto adopted=f.checkpoint("after-rejections");check(payload_domain(adopted.at("snapshot").at("payload")).dump()==payload_domain(source.at("snapshot").at("payload")).dump(),
        "Rejected authoring interleavings changed full runtime state.");
    f.reject("runtime.step",{{"session_id",f.session},{"request_id",accepted.at("request_id")},{"expected_tick",f.tick()},{"ticks",1}},-32010);
    f.reject("runtime.structure.transact",{{"session_id",f.session},{"request_id",accepted.at("request_id")},{"expected_tick",f.tick()},
        {"expected_structure_revision",0},{"spawns",Json::array({{{"template_id",id(30)}}})}},-32010);
    const auto step=f.step_params();call(f.world,"runtime.step",step);f.edit_materials(0);
    value=f.apply();value["request_id"]=step.at("request_id");f.reject("runtime.material.apply",value,-32010);
    const Json spawn={{"session_id",f.session},{"request_id",f.next()},{"expected_tick",f.tick()},
        {"expected_structure_revision",0},{"spawns",Json::array({{{"template_id",id(30)}}})}};
    const auto spawned=call(f.world,"runtime.structure.transact",spawn);
    value=f.apply();value["request_id"]=spawn.at("request_id");f.reject("runtime.material.apply",value,-32010);
    value=f.apply();value["ids"]={spawned.at("spawned").at(0)};f.reject("runtime.material.apply",value,-32004);
    // Only spawned instance roots can be removed by the current native policy.
    // Retired/non-authored IDs remain ineligible; ordinary authored material
    // adoption still preserves the complete post-structure checkpoint state.
    call(f.world,"runtime.structure.transact",{{"session_id",f.session},{"request_id",f.next()},{"expected_tick",f.tick()},
        {"expected_structure_revision",1},{"despawns",{spawned.at("spawned").at(0)}}});
    value=f.apply();value["ids"]={spawned.at("spawned").at(0)};f.reject("runtime.material.apply",value,-32004);
    const auto before=f.checkpoint("structure-before-material");call(f.world,"runtime.material.apply",f.apply());
    const auto after=f.checkpoint("structure-after-material");
    check(payload_domain(before.at("snapshot").at("payload")).dump()==payload_domain(after.at("snapshot").at("payload")).dump() &&
        f.status().at("structure_revision")==2 && f.status().at("material_revision")==2,
        "Material adoption lost the live structural state or reset its revision.");
}
}
int main() {
    const auto directory=fs::temp_directory_path()/("poima-world-material-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(poima::Runtime::available(),"Material World qualification requires simulation.");
        check(fs::create_directory(directory),"Cannot reserve test directory.");
        fs::create_directory(directory/"durable");fs::create_directory(directory/"rejections");
        valid_and_durable(directory/"durable");rejection_and_ownership(directory/"rejections");fs::remove_all(directory);
        std::cout<<"Guarded material scalar adoption, complete CPU state, immutable scenes, durable baselines and atomic rejections passed.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<"\nFixture retained: "<<directory<<'\n';return 1;}
}
