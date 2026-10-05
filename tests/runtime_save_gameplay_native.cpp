// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string content(64,'c'),target="00000000000000000000000000000001",camera="00000000000000000000000000000002";
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void marker_environment(const std::filesystem::path& path) {
#ifdef _WIN32
    check(_wputenv_s(L"POIMA_SAVE_FIXTURE_INITIALIZE_MARKER",path.c_str())==0,"Cannot set fixture Initialize marker.");
#else
    check((path.empty()?unsetenv("POIMA_SAVE_FIXTURE_INITIALIZE_MARKER"):setenv("POIMA_SAVE_FIXTURE_INITIALIZE_MARKER",path.c_str(),1))==0,
          "Cannot set fixture Initialize marker.");
#endif
}
struct InitializeMarker {
    std::filesystem::path directory,file;
    InitializeMarker() {
        std::random_device random;const auto base=std::filesystem::temp_directory_path();
        for(unsigned attempt=0;attempt<32;++attempt) {
            const auto candidate=base/("poima-save-init-"+std::to_string(random())+"-"+std::to_string(random()));
            if(std::filesystem::create_directory(candidate)) { directory=candidate;break; }
        }
        check(!directory.empty(),"Cannot create unique fixture marker directory.");
        file=directory/"reject-initialize";
        try { marker_environment(file); }catch(...) { std::filesystem::remove(directory);throw; }
    }
    void arm() {
        check(!std::filesystem::exists(file),"Fixture marker already exists.");
        std::ofstream output(file,std::ios::binary);output<<"test-owned Initialize guard\n";output.close();
        check(output.good() && std::filesystem::is_regular_file(file),"Cannot create fixture Initialize marker.");
    }
    ~InitializeMarker() {
        try { marker_environment({}); }catch(...) {}
        // Remove only our marker and our now-empty directory, never recursively.
        std::error_code ignored;std::filesystem::remove(file,ignored);std::filesystem::remove(directory,ignored);
    }
};
RuntimeDefinition definition() {
    RuntimeDefinition result;result.world_id="00000000000000000000000000000010";result.authored_revision=7;
    RuntimeEntityDefinition entity;entity.id=target;entity.transform.position={3,2,-1};result.entities.push_back(entity);
    entity={};entity.id=camera;entity.camera=RuntimeCamera{};entity.transform.position={0,1,5};result.entities.push_back(entity);return result;
}
Json values(const Runtime& runtime) { return Json::parse(runtime.gameplay_inspect()).at("values"); }
std::string reseal(Json document) {
    const auto bytes=document.at("payload").dump();document["sha256"]=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));return document.dump();
}
void same_game(const Runtime& source,const Runtime& restored) {
    check(source.inspect().tick==restored.inspect().tick,"Restored simulation tick differs.");
    check(source.gameplay_revision()==restored.gameplay_revision(),"Restored gameplay revision differs.");
    check(values(source)==values(restored),"Restored complete typed gameplay values differ.");
    const auto a=Json::parse(source.gameplay_inspect()),b=Json::parse(restored.gameplay_inspect());
    for(const auto* key:{"backend","assembly_sha256","type","schema"})check(a.at(key)==b.at(key),"Restored gameplay identity differs.");
}
void collected_modules(bool native,std::size_t expected) {
    const auto collected=Json::parse(Gameplay::collect());
    check(collected.at("active_modules")==expected && collected.at("retired_alive")==0,
          "Restore/failure left unexpected active or retired managed modules alive.");
    if(native) {
        const auto& details=collected.at("native");
        check(details.at("active_modules")==expected && details.at("retired_alive")==0 && details.at("unload_supported")==false,
              "NativeAOT module instance lifecycle diagnostics differ.");
        check(details.at("diagnostics").at("dynamic_code_supported")==false && details.at("diagnostics").at("dynamic_code_compiled")==false,
              "Native fixture did not report actual NativeAOT execution.");
    }
}
}
int main(int argc,char** argv) {
    try {
        const bool native=argc==3 && std::string(argv[1])=="--native";
        check(argc==5 || native,"Usage: runtime-save-gameplay-test HOSTFXR BRIDGE ASSEMBLY TYPE | --native DESCRIPTOR");
        // Set the immutable environment path before CoreCLR starts. The managed
        // environment may cache native environment values after initialization.
        InitializeMarker initialize_marker;
        GameplayConfig config;
        if(native) {
            const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;
            config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;
            config.native_schema=artifact.schema;config.type=artifact.type;
        }else { config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type=argv[4]; }
        const auto def=definition();
        {
            Runtime source(def);source.gameplay_load(config);source.step(17,{});
            auto typed=values(source);
            check(typed.at("Minimum")=="-9223372036854775808","Int64.MinValue was not represented losslessly.");
            check(typed.at("Target")==target && typed.at("OptionalTarget")==std::string(32,'0'),"Entity/null fixture differs.");
            check(typed.at("Ticks")==17 && typed.at("Accumulator")=="136" && typed.at("Fraction")==4.5 &&
                  typed.at("Precise")==2.25 && typed.at("LastX")==3,"Actual managed Tick results differ.");
            source.gameplay_edit("{\"Precise\":123.125,\"OptionalTarget\":\"00000000000000000000000000000002\"}");
            const auto saved=source.save_snapshot(content);const auto envelope=Json::parse(saved);
            check(!envelope.at("payload").at("gameplay").is_null(),"Snapshot omitted loaded gameplay.");
            initialize_marker.arm();
            bool initialization_blocked=false;
            try {
                // Native replacement is independently forbidden, so use a fresh
                // Runtime to prove Initialize itself rejects the armed marker.
                if(native) { Runtime fresh(def);fresh.gameplay_load(config); }
                else source.gameplay_load(config);
            }catch(const std::exception& error) {
                initialization_blocked=std::string(error.what()).find("Initialize must not execute during snapshot restore")!=std::string::npos;
            }
            check(initialization_blocked,"Fixture Initialize rejection was not armed.");
            check(source.save_snapshot(content)==saved,"Failed ordinary reload changed the live game.");
            auto restored=Runtime::from_snapshot(def,content,saved,config);same_game(source,*restored);
            source.step(23,{});restored->step(23,{});same_game(source,*restored);
            typed=values(*restored);
            check(typed.at("Ticks")==40 && typed.at("Accumulator")=="780" && typed.at("Minimum")=="-9223372036854775808",
                  "Restored gameplay did not continue on its saved fixed-tick clock.");
            check(typed.at("Precise")==126 && typed.at("LastX")==3,"Saved typed values/entity references did not reach real Tick.");
            restored.reset();
            const auto live_before=source.save_snapshot(content);std::size_t failures=0;
            auto reject=[&](const Json& candidate,const std::optional<GameplayConfig>& trusted) {
                bool failed=false;try { (void)Runtime::from_snapshot(def,content,reseal(candidate),trusted); }catch(const std::exception&) { failed=true; }
                check(failed,"Malformed or incompatible gameplay snapshot was accepted.");
                check(source.save_snapshot(content)==live_before,"Rejected restore mutated the old game's fields/clock/revision.");++failures;
            };
            auto mutate=[&](const auto& change) { auto candidate=envelope;change(candidate["payload"]["gameplay"]);reject(candidate,config); };
            reject(envelope,std::nullopt);
            auto wrong_type=config;wrong_type.type="Poima.Tests.AlternateSaveProbe";reject(envelope,wrong_type);
            mutate([](Json& g){g["assembly_sha256"]=std::string(64,'0');});
            mutate([](Json& g){g["type"]="Poima.Tests.AlternateSaveProbe";});
            mutate([](Json& g){g["schema"]["identity"]="another-module";});
            mutate([](Json& g){g["values"].erase("Minimum");});
            mutate([](Json& g){g["values"]["Extra"]=1;});
            mutate([](Json& g){g["values"]["Minimum"]=-1;});
            mutate([](Json& g){g["values"]["Minimum"]="9223372036854775808";});
            mutate([](Json& g){g["values"]["Ticks"]=true;});
            mutate([](Json& g){g["values"]["Fraction"]=nullptr;});
            mutate([](Json& g){g["values"]["Target"]="000000000000000000000000000000ff";});
            mutate([](Json& g){g["values"]["OptionalTarget"]="invalid";});
            mutate([&](Json& g){g["backend"]=native?"coreclr":"native_aot";});
            mutate([](Json& g){g["extra"]=1;});
            // Valid no-module snapshots must not accidentally execute supplied code.
            Runtime no_game(def);auto no_game_save=Json::parse(no_game.save_snapshot(content));reject(no_game_save,config);
            auto nullable=envelope;nullable["payload"]["gameplay"]["values"]["OptionalTarget"]=std::string(32,'0');
            auto with_null=Runtime::from_snapshot(def,content,reseal(nullable),config);with_null->step(1,{});
            check(values(*with_null).at("LastX")==3,"Null entity reference did not survive restore and Tick.");with_null.reset();
            for(unsigned repeat=0;repeat<40;++repeat) {
                auto candidate=Runtime::from_snapshot(def,content,saved,config);candidate->step(1,{});
                check(values(*candidate).at("Ticks")==18,"Repeated restore failed to resume managed state.");
            }
            collected_modules(native,1);
            check(source.save_snapshot(content)==live_before,"Independent repeated restores changed the original runtime.");
            source.step(1,{});check(values(source).at("Ticks")==41,"Original game could not continue after rejected restores.");
            std::cout<<(native?"NativeAOT":"CoreCLR")<<" runtime snapshot passed: complete typed values, Initialize bypass, exact tick continuation, trusted binding, "
                     <<failures<<" resealed rejection cases and 40 "<<(native?"released native instances":"collectible restores")<<".\n";
        }
        collected_modules(native,0);
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
