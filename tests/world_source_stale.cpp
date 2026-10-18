// SPDX-License-Identifier: Apache-2.0
// Real CPU WorldSession/Store regressions; no renderer or gameplay compiler.
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/save_store.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
std::size_t checks=0,pairs=0;
Json observations=Json::array();
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
std::string text(const fs::path& path) {const auto value=path.u8string();return {value.begin(),value.end()};}
std::string id(std::uint64_t value) {std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(32)<<value;return out.str();}
std::string bytes(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);check(bool(input),"Cannot read retained fixture.");
    return {std::istreambuf_iterator<char>(input),{}};
}
void write_fixture(const fs::path& path,const std::string& value) {
    check(!fs::exists(path),"Source variants must be fresh.");
    std::ofstream output(path,std::ios::binary);output.write(value.data(),static_cast<std::streamsize>(value.size()));
    output.close();check(bool(output),"Cannot write closed-owner authored variant.");
}
Json request(poima::WorldSession& owner,const char* method,const Json& params=Json::object()) {
    return Json::parse(owner.request(Json{{"jsonrpc","2.0"},{"id",17},{"method",method},{"params",params}}.dump()));
}
Json call(poima::WorldSession& owner,const char* method,const Json& params=Json::object()) {
    auto result=request(owner,method,params);
    if(result.contains("error"))throw std::runtime_error(std::string(method)+": "+result.at("error").dump());
    return result.at("result");
}
std::map<std::string,std::string> files(const fs::path& root) {
    // Only the external save namespace: no live, exclusively held world lock.
    std::map<std::string,std::string> result;
    for(const auto& item:fs::recursive_directory_iterator(root)) {
        check(!item.is_symlink(),"Unexpected save link.");
        if(item.is_directory())result[text(item.path().lexically_relative(root))+"/"]="";
        else {check(item.is_regular_file(),"Unexpected save entry.");result[text(item.path().lexically_relative(root))]=bytes(item.path());}
    }
    return result;
}
class Fixture {
public:
    fs::path path,storage;
    poima::WorldSession world;
    std::string session;
    std::uint64_t sequence=1000;
    Fixture(const fs::path& source,const fs::path& saves):path(source),storage(saves),world(text(path)) {
        fs::create_directories(storage);
        call(world,"save.configure",{{"request_id",next()},{"expected_generation",0},{"root",text(storage)}});
    }
    std::string next() {return id(sequence++);}
    Json revision() {return call(world,"world.inspect").at("revision");}
    Json status() {return call(world,"runtime.inspect",{{"session_id",session}});}
    void start() {session=next();call(world,"runtime.start",{{"session_id",session},{"revision",revision()}});}
    void stop() {call(world,"runtime.stop",{{"session_id",session}});session.clear();}
    Json views(bool expected) {
        const auto state=status();
        const Json observed=call(world,"runtime.observe",{{"session_id",session},{"tick",state.at("tick")},{"ids",{id(1)}}});
        const Json selection={{"session_id",session},{"tick",state.at("tick")},{"ids",{id(1)}}};
        const auto material=call(world,"runtime.material.inspect",selection),texture=call(world,"runtime.texture.inspect",selection);
        for(const auto& view:{state,observed,material,texture})
            check(view.at("source_stale").is_boolean() && view.at("source_stale")==expected,"Current read surfaces disagree on source staleness.");
        return state;
    }
    Json guards() {
        const auto state=status(),components=call(world,"runtime.components",{{"session_id",session}});
        return {{"expected_tick",state.at("tick")},{"expected_gameplay_revision",0},
            {"expected_component_revision",components.at("component_revision")},{"expected_structure_revision",state.at("structure_revision")},
            {"expected_ui_revision",state.at("ui_revision")},{"expected_control_sequence",state.at("control_sequence")},
            {"expected_material_revision",state.at("material_revision")}};
    }
    std::string save(const std::string& slot) {
        auto params=guards();params.update({{"request_id",next()},{"configuration_generation",1},{"slot",slot},
            {"expected_generation",0},{"session_id",session}});
        const auto receipt=call(world,"save.write",params);
        const auto saved=poima::saves::Store(storage/("slot-"+slot)).read();
        check(saved.status.selected.has_value(),"Native save has no selected generation.");
        check(receipt.at("generation")==1 && receipt.at("replayed")==false &&
            receipt.at("bytes")==saved.bytes.size() && receipt.at("sha256")==saved.status.selected->sha256,
            "Native save receipt/operand differs.");
        const auto doc=Json::parse(saved.bytes);
        // No Camera/UI/custom schemas/structural spawn/motor: this fixture
        // deliberately exercises the original nine-member v1 snapshot.
        check(doc.at("format")=="poima.world-save" && doc.at("version")==1 &&
            doc.at("snapshot").at("format")=="poima.runtime-snapshot" && doc.at("snapshot").at("version")==1,
            "Unexpected complete native save format.");
        const auto& payload=doc.at("snapshot").at("payload");
        check(payload.size()==9,"Unexpected original snapshot payload membership.");
        for(const auto* field:{"content_sha256","world_id","authored_revision","tick","gameplay_revision","entities","animation","sound","gameplay"})
            check(payload.contains(field),"Missing original snapshot payload member.");
        observations.push_back({{"slot",slot},{"receipt",receipt},{"bytes",saved.bytes.size()},{"snapshot",doc.at("snapshot").at("sha256")}});
        return saved.bytes;
    }
    void read_pair(const std::string& label,bool stale) {
        const auto before=save(label+"-before"),source=bytes(path);
        const auto authored=call(world,"world.inspect"),history=call(world,"world.history");
        const auto external=files(storage);const auto baseline=views(stale);
        for(unsigned i=0;i<4;++i)check(views(stale)==baseline,"Repeated reads changed the complete runtime summary.");
        check(bytes(path)==source && call(world,"world.inspect")==authored && call(world,"world.history")==history,
            "Cache reads changed authored bytes, revision, receipts or history.");
        check(files(storage)==external,"Cache reads changed original save namespace bytes/membership.");
        const auto after=save(label+"-after");
        check(before==after,"Cache reads changed complete native saved bytes.");++pairs;
    }
    Json load_params(const std::string& slot) {
        auto params=session.empty() ? Json{{"expected_tick",nullptr},{"expected_gameplay_revision",nullptr},
            {"expected_component_revision",nullptr},{"expected_structure_revision",nullptr},{"expected_ui_revision",nullptr},
            {"expected_control_sequence",nullptr},{"expected_material_revision",nullptr}} : guards();
        params.update({{"request_id",next()},{"configuration_generation",1},{"slot",slot},{"expected_generation",1},
            {"revision",revision()},{"expected_session_id",session.empty() ? Json(nullptr) : Json(session)},{"new_session_id",next()}});
        return params;
    }
    Json load(const Json& params,bool expected) {
        const auto result=call(world,"save.load",params);session=params.at("new_session_id").get<std::string>();
        check(result.at("source_stale")==expected && result.at("authored_revision")==1 && result.at("current_authored_revision")==1,
            "Prepared restore reply did not compare the newly selected frozen document.");
        views(expected);return result;
    }
};
void author(const fs::path& path) {
    poima::WorldSession owner(text(path));
    const Json material={{"base_color",{.25,.5,.75}},{"emissive",{0.,0.,0.}},{"metallic",.25},{"roughness",.75},{"double_sided",false}};
    call(owner,"world.transact",{{"request_id",id(20)},{"base_revision",0},{"ops",Json::array({
        {{"op","entity.create"},{"id",id(1)},{"name","Baseline A"}},
        {{"op","component.set"},{"id",id(1)},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"visible",true},{"albedo",{.25,.5,.75}}}}},
        {{"op","component.set"},{"id",id(1)},{"type","PbrMaterial"},{"value",material}}
    })}});
}
Json rename(const std::string& value) {return Json::array({{{"op","entity.rename"},{"id",id(1)},{"name",value}}});}
void authoring(const fs::path& base) {
    const auto path=base/"history.json";author(path);Fixture f(path,base/"history-saves");f.start();f.read_pair("false",false);
    const auto source=bytes(path);const auto history=call(f.world,"world.history");
    auto preview=Json{{"request_id",f.next()},{"base_revision",1},{"ops",rename("Preview")},{"preview",true}};
    check(call(f.world,"world.transact",preview).at("committed")==false,"Preview unexpectedly committed.");
    check(bytes(path)==source && call(f.world,"world.history")==history,"Preview altered committed source/history.");f.views(false);
    const auto rejected_id=f.next();auto broken=Json{{"request_id",rejected_id},{"base_revision",1},{"ops",rename("Speculative")}};
    broken["ops"].push_back({{"op","entity.rename"},{"id",id(999)},{"name","Missing"}});
    const auto reject=request(f.world,"world.transact",broken);
    check(reject.contains("error") && reject.at("error").at("code")==-32004,"Invalid late transaction did not reject.");
    check(bytes(path)==source && call(f.world,"world.history")==history,"Rejected transaction published staged source/history.");f.views(false);
    auto corrected=Json{{"request_id",rejected_id},{"base_revision",1},{"ops",rename("Committed")}};
    const auto edited=call(f.world,"world.transact",corrected);check(edited.at("committed")==true && edited.at("revision")==2,"Unused rejected ID could not commit corrected source.");
    f.read_pair("true",true);
    const auto changed_source=bytes(path);const auto changed_history=call(f.world,"world.history");
    preview["request_id"]=f.next();preview["base_revision"]=2;
    check(call(f.world,"world.transact",preview).at("committed")==false,"Stale-source preview unexpectedly committed.");
    broken["request_id"]=f.next();broken["base_revision"]=2;
    const auto again=request(f.world,"world.transact",broken);
    check(again.contains("error") && again.at("error").at("code")==-32004,"Stale-source invalid batch did not reject.");
    check(bytes(path)==changed_source && call(f.world,"world.history")==changed_history,"Stale-source preview/rejection published speculative state.");f.views(true);
    check(call(f.world,"world.transact",corrected).at("replayed")==true,"Successful authored receipt did not replay.");f.views(true);
    const auto undo=Json{{"request_id",f.next()},{"base_revision",2}};call(f.world,"world.undo",undo);
    check(call(f.world,"entity.get",{{"id",id(1)}}).at("value").at("name")=="Baseline A","Undo did not restore baseline content.");
    check(f.revision()==3,"Undo did not advance authored revision.");f.views(true);
    check(call(f.world,"world.undo",undo).at("replayed")==true,"Undo replay changed receipt behavior.");f.views(true);
    call(f.world,"world.redo",{{"request_id",f.next()},{"base_revision",3}});check(f.revision()==4,"Redo revision differs.");f.views(true);
    f.stop();f.start();f.read_pair("restart",false);
}
void replacements(const fs::path& base) {
    const auto path=base/"source-a.json",variant=base/"source-b.json",storage=base/"replacement-saves";
    author(path);
    // Both owners are closed here. This is a fresh authored fixture variant,
    // not an observed save/receipt edit or an external edit to a live owner.
    const auto original=bytes(path);auto alternate=Json::parse(original);
    alternate["entities"][id(1)]["name"]="Same-revision B";
    write_fixture(variant,alternate.dump(2)+"\n");
    check(alternate.at("revision")==1 && alternate.at("world_id")==Json::parse(original).at("world_id"),"Variant lost same revision/world identity.");
    std::string saved_a,saved_b;
    {Fixture b(variant,storage);b.start();b.views(false);saved_b=b.save("baseline-b");}
    check(bytes(path)==original,"Variant preparation altered original source.");
    {
        Fixture f(path,storage);f.start();saved_a=f.save("baseline-a");
        check(Json::parse(saved_a).at("document").at("revision")==Json::parse(saved_b).at("document").at("revision") &&
            Json::parse(saved_a).at("document").at("entities")!=Json::parse(saved_b).at("document").at("entities"),"Native baselines do not exercise same-revision different frozen content.");
        f.read_pair("initial-a",false);
        const auto to_b=f.load_params("baseline-b");const auto result=f.load(to_b,true);f.read_pair("loaded-b",true);
        const auto before=files(storage);const auto state=f.status();
        auto replay=result;replay["replayed"]=true;check(call(f.world,"save.load",to_b)==replay,"Warm restore receipt replay changed the original reply.");
        check(f.status()==state && files(storage)==before,"Restore replay changed runtime/storage.");
        auto rejected=f.load_params("baseline-a");rejected["expected_generation"]=2;
        const auto failure=request(f.world,"save.load",rejected);check(failure.contains("error") && failure.at("error").at("code")==-32009,"Wrong save generation did not reject.");
        check(f.status()==state && files(storage)==before,"Failed restore changed selected runtime/storage.");f.views(true);
        f.load(f.load_params("baseline-a"),false);f.read_pair("loaded-a",false);
        f.stop();f.load(f.load_params("baseline-b"),true);f.read_pair("stopped-b",true);
        f.stop();f.start();f.views(false);
    }
    {Fixture fresh(path,storage);fresh.load(fresh.load_params("baseline-b"),true);fresh.read_pair("cold-b",true);
        fresh.load(fresh.load_params("baseline-a"),false);fresh.read_pair("cold-a",false);}
    check(poima::saves::Store(storage/"slot-baseline-a").read().bytes==saved_a &&
        poima::saves::Store(storage/"slot-baseline-b").read().bytes==saved_b,"Original native saved operands were changed.");
}
}
int main(int argc,char** argv) {
    fs::path directory;bool retained=false;
    try {
        if(argc==3 && std::string(argv[1])=="--output") {directory=fs::absolute(argv[2]);retained=true;}
        else {check(argc==1,"Use --output FRESH_DIRECTORY to retain original operands.");directory=fs::temp_directory_path()/("poima-world-source-stale-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));}
        if(!poima::Runtime::available()) {std::cout<<"World source staleness checks skipped: simulation unavailable.\n";return 0;}
        check(!fs::exists(directory) && fs::is_directory(directory.parent_path()) && fs::create_directory(directory),"Fresh fixture directory required.");
        authoring(directory);replacements(directory);
        if(retained)write_fixture(directory/"observations.json",Json{{"checks",checks},{"whole_save_pairs",pairs},{"native_save_records",observations}}.dump(2)+"\n");
        else fs::remove_all(directory);
        std::cout<<"World source staleness passed: "<<checks<<" checks, "<<pairs<<" complete saved-byte pairs; retained="<<(retained?"yes":"no")<<".\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<"\nFixture retained: "<<directory<<'\n';return 1;}
}
