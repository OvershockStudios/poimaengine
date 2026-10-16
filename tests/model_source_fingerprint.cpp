// SPDX-License-Identifier: Apache-2.0
// File-layer guards operate on the complete imported source, not on a selected
// donor or on the final importer-tagged cooked package. Protocol fixtures add
// independently retained source-inspection and complete-world comparisons.
#include "../src/asset_store.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace poima;
namespace fs=std::filesystem;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string bytes(const fs::path& path) {
    const auto size=fs::file_size(path);check(size<=16*1024*1024,"Test fixture exceeds16MiB.");
    std::string out(static_cast<std::size_t>(size),'\0');std::ifstream file(path,std::ios::binary);
    check(bool(file.read(out.data(),static_cast<std::streamsize>(out.size()))),"Cannot read retained test bytes.");return out;
}
std::string hash(const ModelAsset& model) {return content_hash(encode_model(model));}
template<class F> void mismatch(F action,ModelSourceRole role,std::optional<std::size_t> donor,
    const std::string& expected,const std::string& actual) {
    try {action();}
    catch(const ModelSourceFingerprintError& error) {
        const auto& d=error.diagnostic();
        check(d.role==role && d.donor_index==donor && d.expected_model_sha256==expected &&
            d.actual_model_sha256==actual,"Fingerprint error lost complete original role/index/hash facts.");
        check(std::string(error.what())=="Model source changed since inspection; inspect it again before importing.",
            "Actionable path-free mismatch message differs.");return;
    }
    throw std::runtime_error("Changed original source was not rejected by its fingerprint guard.");
}
template<class F> void rejected(F action,const char* expected) {
    try {action();}
    catch(const ModelSourceFingerprintError&) {throw std::runtime_error("Existing non-fingerprint guard became a changed-source error.");}
    catch(const std::exception& error) {check(std::string(error.what())==expected,"Existing/format guard changed.");return;}
    throw std::runtime_error("Invalid source selector/guard was accepted.");
}
void parity(const fs::path& output,const fs::path& source,const std::string& original,
    std::vector<ModelAnimationSource> donors,const std::string& label) {
    const auto legacy=store_model_asset(output/(label+"-legacy"),source,donors);
    for(auto& donor:donors)donor.expected_model_sha256=original;
    const auto guarded=store_model_asset(output/(label+"-guarded"),source,donors,{},original);
    check(legacy.id==guarded.id && legacy.bytes==guarded.bytes &&
        bytes(output/(label+"-legacy")/(legacy.id+".pmodel"))==
        bytes(output/(label+"-guarded")/(guarded.id+".pmodel")),
        "Matching guards changed complete legacy cooked package bytes.");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==2 || (argc==4 && std::string(argv[2])=="--output"),
            "Usage: source-fingerprint-test original-animated-ribbon.glb [--output fresh-directory]");
        const fs::path source=argv[1];fs::path output;
        if(argc==4) {output=argv[3];check(fs::create_directory(output),"Retained test output must be fresh.");}
        else {
            const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
            for(unsigned i=0;i<16;++i) {
                output=fs::temp_directory_path()/("poima-source-fingerprint-"+std::to_string(stamp)+"-"+std::to_string(i));
                if(fs::create_directory(output))break;
                check(i!=15,"Cannot claim a fresh temporary test output.");
            }
        }
        const auto source_bytes=bytes(source);const auto imported=import_model(source);
        check(imported.importer.empty() && imported.model->animations.size()==1 && !imported.model->skins.empty(),
            "Use the unchanged accepted skinned animated-ribbon GLB.");
        const auto original=hash(*imported.model);const std::string stale(64,'0');check(stale!=original,"Bad stale test hash.");
        parity(output,source,original,{},"base");
        mismatch([&]{store_model_asset(output/"base-rejected",source,{}, {},stale);},
            ModelSourceRole::base,{},stale,original);
        check(!fs::exists(output/"base-rejected"),"Stale base guard created publication/staging paths.");

        ModelAnimationSource donor;donor.source=source;donor.clip=0;donor.name="Exact";
        const auto selected=import_animation_source(donor,FbxNormalConvention::opengl);
        const auto selected_hash=hash(*selected.model);check(selected_hash!=original,
            "Selected/renamed donor fixture did not differ from its original source.");
        donor.expected_model_sha256=selected_hash;
        mismatch([&]{import_animation_source(donor,FbxNormalConvention::opengl);},
            ModelSourceRole::animation_donor,{},selected_hash,original);
        donor.expected_model_sha256=stale;donor.clip=255;
        mismatch([&]{import_animation_source(donor,FbxNormalConvention::opengl);},
            ModelSourceRole::animation_donor,{},stale,original);
        donor.expected_model_sha256=original;
        rejected([&]{import_animation_source(donor,FbxNormalConvention::opengl);},
            "Selected animation take index is out of range.");
        donor.clip=0;donor.expected_model_sha256.reset();
        parity(output,source,original,{donor},"exact");
        auto second=donor;second.name="Second";second.expected_model_sha256=stale;
        mismatch([&]{store_model_asset(output/"second-rejected",source,{donor,second});},
            ModelSourceRole::animation_donor,1,stale,original);
        check(!fs::exists(output/"second-rejected"),"Successful donor prefix published before a later stale guard.");

        donor.name="Frames";donor.frame_transfer=AnimationFrameTransfer{};
        parity(output,source,original,{donor},"frame");
        donor.expected_model_sha256=stale;
        mismatch([&]{store_model_asset(output/"frame-rejected",source,{donor});},
            ModelSourceRole::animation_donor,0,stale,original);
        check(!fs::exists(output/"frame-rejected"),"Stale frame donor published before its guard.");
        donor.frame_transfer.reset();donor.name="Rotations";donor.expected_model_sha256.reset();
        donor.rotation_retarget=AnimationRotationRetarget{};
        donor.rotation_retarget->expected_source_model_sha256=original;
        donor.rotation_retarget->expected_target_model_sha256=original;
        parity(output,source,original,{donor},"rotation");
        donor.expected_model_sha256=stale;
        mismatch([&]{store_model_asset(output/"rotation-rejected",source,{donor});},
            ModelSourceRole::animation_donor,0,stale,original);
        check(!fs::exists(output/"rotation-rejected"),"Stale rotation donor published before its guard.");
        donor.expected_model_sha256=original;
        donor.rotation_retarget->expected_source_model_sha256=stale;
        rejected([&]{store_model_asset(output/"inner-rejected",source,{donor});},
            "Reference-rotation original source model SHA256 differs from the inspected model.");
        check(!fs::exists(output/"inner-rejected"),"Outer guard bypassed a conflicting inner rotation guard.");

        for(const auto& invalid:std::vector<std::string>{"",std::string(63,'a'),std::string(65,'a'),std::string(64,'A'),std::string(64,'g')}) {
            const auto missing=output/"missing.glb";
            rejected([&]{store_model_asset(output/"malformed",missing,{}, {},invalid);},
                "Expected model SHA256 must be 64 lowercase hexadecimal characters.");
            ModelAnimationSource bad;bad.source=missing;bad.expected_model_sha256=invalid;
            rejected([&]{import_animation_source(bad,FbxNormalConvention::opengl);},
                "Expected model SHA256 must be 64 lowercase hexadecimal characters.");
            rejected([&]{store_model_asset(output/"malformed-donor",source,{bad});},
                "Expected model SHA256 must be 64 lowercase hexadecimal characters.");
        }
        check(bytes(source)==source_bytes,"Native guards changed caller source bytes.");
        std::cout<<"4 model-source fingerprint guard groups passed (base/donor identity, full-package parity, policy precedence, pre-I/O validation).\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
