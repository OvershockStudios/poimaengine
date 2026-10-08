// SPDX-License-Identifier: Apache-2.0
#include "material_service.hpp"
#include "poima/material_recipe.hpp"
#include "poima/build_metadata.hpp"
#include "asset_store.hpp"
#include <future>
#include <map>
#include <chrono>
#include <set>

namespace poima::materials {
namespace {
using Json=nlohmann::json;namespace fs=std::filesystem;
void check(bool value,const char* text,int code=-32602){if(!value)throw Error(code,text);}
void fields(const Json& value,std::initializer_list<const char*> names){check(value.is_object(),"Material request must be an object.");for(const auto& [key,unused]:value.items()){(void)unused;check(std::find(names.begin(),names.end(),key)!=names.end(),"Unknown material request field.");}check(value.size()==names.size(),"Material request fields are required.");}
std::string id(const Json& value){check(value.is_string() && valid_asset_id(value.get<std::string>()),"Material recipe/job ID must be64 lowercase hexadecimal digits.");return value.get<std::string>();}
std::string safe_diagnostic(const char* message){
    // Filesystem errors are byte strings and can end inside a UTF-8 sequence.
    // Mirror development-service replacement, then bound valid output bytes.
    auto text=Json::parse(Json(std::string(message).substr(0,1024)).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
    auto length=std::min<std::size_t>(text.size(),1024);
    while(length<text.size() && length>0 && (static_cast<unsigned char>(text[length])&0xc0u)==0x80u)--length;
    text.resize(length);return text;
}
Json canonical_facts(){return {{"height","intermediate only; no displacement or collision change"},{"normal","linear tangent-space XYZ; +Y follows increasing V; periodic physical central differences"},
    {"packed_channels","R=0 G=perceptual roughness B=0 metallic A=255"},{"color","sRGB inputs blended in linear space; opaque sRGB output"},
    {"determinism","same recipe/evaluator and qualified numerical build profile; cross-platform equality requires qualification"},{"bundle_policy","authoring recipe manifest excluded; referenced baked images follow ordinary runtime closure"},
    {"uv_scale","UV 0..1 must cover tile_width_m by tile_height_m; no automatic UV transform"}};}
struct Encoded {std::array<std::string,3> images;Json manifest;};
Encoded encode_bake(const Recipe& recipe,const std::shared_ptr<std::atomic_bool>& cancel){
    auto baked=bake(recipe,cancel.get());Encoded out;Json maps=Json::object(),textures=Json::object();
    const std::array<const char*,3> names{"base_color","normal","metallic_roughness"};
    for(std::size_t i=0;i<3;++i){if(cancel->load())throw Cancelled();out.images[i]=encode_image(baked.images[i]);(void)decode_image(out.images[i]);const auto hash=content_hash(out.images[i]);
        maps[names[i]]={{"asset",hash},{"color_space",baked.images[i].srgb?"srgb":"linear"},{"width",recipe.width},{"height",recipe.height},{"mips",baked.images[i].mips.size()},{"bytes",out.images[i].size()}};
        textures[names[i]]={{"asset",hash},{"wrap_s",10497},{"wrap_t",10497},{"min_filter",9987},{"mag_filter",9729}};
    }
    textures["emissive"]=nullptr;textures["occlusion"]=nullptr;textures["normal_scale"]=1;textures["occlusion_strength"]=1;
    const auto& build=build_metadata();
    out.manifest={{"numerical_profile",{{"compiler",build.compiler},{"compiler_version",build.compiler_version},{"target_os",build.target_os},{"target_arch",build.target_arch}}},{"format","poima.material.v1"},{"recipe_id",recipe_id(recipe)},{"evaluator",evaluator},{"recipe",canonical_recipe(recipe)},{"maps",maps},
        {"PbrTextures",textures},{"PbrMaterial",{{"base_color",{1,1,1}},{"emissive",{0,0,0}},{"metallic",1},{"roughness",1},{"double_sided",false}}},
        {"facts",canonical_facts()}};
    if(cancel->load())throw Cancelled();
    return out;
}
Json read_manifest(const fs::path& directory,const std::string& identity){
    try {
    const auto path=asset_package_path(directory,identity,".pmaterial");const auto length=fs::file_size(path);check(length<=32768,"Material manifest exceeds32KiB.",-32050);
    std::string text(static_cast<std::size_t>(length),'\0');std::ifstream file(path,std::ios::binary);check(bool(file.read(text.data(),static_cast<std::streamsize>(length))),"Material manifest read failed.",-32050);
    std::vector<std::set<std::string>> keys;
    auto value=Json::parse(text,[&](int depth,Json::parse_event_t event,Json& item){
        check(depth<=16,"Material manifest nesting exceeds bounds.",-32050);
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(!keys.empty() && keys.back().insert(item.get<std::string>()).second,"Duplicate material manifest field.",-32050);
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    check(value.is_object() && value.size()==9 && value.value("format","")=="poima.material.v1" && value.value("evaluator","")==evaluator && value.value("recipe_id","")==identity,"Material manifest identity/profile mismatch.",-32050);
    const auto& profile=value.at("numerical_profile");check(profile.is_object() && profile.size()==4,"Material numerical profile malformed.",-32050);
    for(const auto* key:{"compiler","compiler_version","target_os","target_arch"})check(profile.contains(key) && profile.at(key).is_string() && !profile.at(key).get_ref<const std::string&>().empty() && profile.at(key).get_ref<const std::string&>().size()<=128,"Material numerical profile malformed.",-32050);
    const auto recipe=parse_recipe(value.at("recipe"));check(recipe_id(recipe)==identity && canonical_recipe(recipe)==value.at("recipe"),"Material canonical recipe mismatch.",-32050);
    // Rebuild the cheap metadata independently from verified image packages.
    // No opaque descriptor may redirect runtime material fields to other IDs.
    check(value.at("maps").is_object() && value.at("maps").size()==3,"Material map metadata malformed.",-32050);
    Json expected_textures=Json::object();const std::array<const char*,3> names{"base_color","normal","metallic_roughness"};
    for(std::size_t i=0;i<3;++i){const auto& map=value.at("maps").at(names[i]);const auto loaded=read_image_asset(directory,id(map.at("asset")));const auto& image=*loaded.image;
        check(image.srgb==(i==0) && image.mips.front().width==recipe.width && image.mips.front().height==recipe.height,"Material map dimension/color-space mismatch.",-32050);
        check(map==Json{{"asset",loaded.id},{"color_space",image.srgb?"srgb":"linear"},{"width",recipe.width},{"height",recipe.height},{"mips",image.mips.size()},{"bytes",loaded.bytes}},"Material map metadata does not match cooked image.",-32050);
        expected_textures[names[i]]={{"asset",loaded.id},{"wrap_s",10497},{"wrap_t",10497},{"min_filter",9987},{"mag_filter",9729}};
    }
    expected_textures["emissive"]=nullptr;expected_textures["occlusion"]=nullptr;expected_textures["normal_scale"]=1;expected_textures["occlusion_strength"]=1;
    check(value.at("PbrTextures")==expected_textures && value.at("PbrMaterial")==Json{{"base_color",{1,1,1}},{"emissive",{0,0,0}},{"metallic",1},{"roughness",1},{"double_sided",false}},"Material suggestion differs from verified maps.",-32050);
    check(value.at("facts")==canonical_facts(),"Material facts differ from this evaluator contract.",-32050);
    value["manifest_sha256"]=content_hash(text);return value;
    }catch(const std::exception& error){throw Error(-32050,safe_diagnostic(error.what()));}
}
Json publish(const fs::path& directory,Encoded&& value){
    for(const auto& bytes:value.images)(void)store_cooked_image(directory,bytes);
    const auto identity=value.manifest.at("recipe_id").get<std::string>();const auto path=directory/(identity+".pmaterial");
    if(fs::exists(fs::symlink_status(path))){
        auto existing=read_manifest(directory,identity);
        if(existing.at("numerical_profile")==value.manifest.at("numerical_profile"))
            check(existing.at("maps")==value.manifest.at("maps") && existing.at("PbrTextures")==value.manifest.at("PbrTextures") && existing.at("PbrMaterial")==value.manifest.at("PbrMaterial"),"Existing same-profile material maps differ from regenerated recipe output.",-32050);
        return existing;
    }
    const auto text=value.manifest.dump();check(text.size()<=32768,"Material manifest exceeds32KiB.",-32050);const auto pending=directory/(identity+".material-pending");
    check(!fs::exists(fs::symlink_status(pending)),"Material staging path already exists.",-32050);
    write_asset_pending_exclusive(pending,text);
    try {world_detail::replace_file(pending,path);}catch(...) {std::error_code unused;fs::remove(pending,unused);throw;}
    return read_manifest(directory,identity);
}
Json object(Json properties,Json required){return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};}
}
struct Service::Impl {
    struct Job {std::string id,state="baking",diagnostic;Recipe recipe;std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);std::future<Encoded> future;Json result=nullptr;};
    std::map<std::string,Job> jobs;
    ~Impl(){for(auto& [identity,job]:jobs){(void)identity;job.cancel->store(true);}for(auto& [identity,job]:jobs){(void)identity;if(job.future.valid())job.future.wait();}}
    Json summary(const Job& job)const{return {{"id",job.id},{"state",job.state},{"recipe_id",job.id},{"result",job.result},{"diagnostic",job.diagnostic.empty()?Json(nullptr):Json(job.diagnostic)}};}
    void poll(Job& job,const fs::path& directory){
        if(job.state!="baking" || !job.future.valid() || job.future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try {auto encoded=job.future.get();if(job.cancel->load()){job.state="cancelled";return;}job.result=publish(directory,std::move(encoded));job.state="succeeded";}
        catch(const Cancelled&){job.state="cancelled";}
        catch(const std::exception& error){job.state="failed";job.diagnostic=safe_diagnostic(error.what());}
    }
};
Service::Service():impl_(std::make_unique<Impl>()){}Service::~Service()=default;
Json Service::dispatch(const std::string& method,const Json& params,const fs::path& directory){
    try {
        constexpr std::array names{"asset.material.generate","asset.material.job","asset.material.jobs","asset.material.cancel","asset.material.forget","asset.material.inspect"};
        check(std::find(names.begin(),names.end(),method)!=names.end(),"Unknown material method.",-32601);
        if(method=="asset.material.inspect"){fields(params,{"recipe"});return read_manifest(directory,id(params.at("recipe")));}
        if(method=="asset.material.generate"){
            fields(params,{"recipe"});const auto recipe=parse_recipe(params.at("recipe"));const auto identity=recipe_id(recipe);
            if(const auto found=impl_->jobs.find(identity);found!=impl_->jobs.end())return impl_->summary(found->second);
            check(impl_->jobs.size()<8,"Material jobs retain at most8 records; forget a terminal job.",-32080);
            for(const auto& [key,job]:impl_->jobs){(void)key;check(!job.future.valid() || job.future.wait_for(std::chrono::seconds(0))==std::future_status::ready,"One material bake may run at a time.",-32080);}
            Impl::Job job;job.id=identity;job.recipe=recipe;
            // Allocate the record before launching; no process/worker is accepted
            // if map insertion fails. Roll back if thread creation fails.
            auto inserted=impl_->jobs.emplace(identity,std::move(job));
            try {auto flag=inserted.first->second.cancel;inserted.first->second.future=std::async(std::launch::async,[recipe,flag]{return encode_bake(recipe,flag);});}
            catch(...){impl_->jobs.erase(inserted.first);throw;}
            return impl_->summary(inserted.first->second);
        }
        if(method=="asset.material.jobs"){fields(params,{});Json values=Json::array();for(const auto& [identity,job]:impl_->jobs){(void)identity;values.push_back(impl_->summary(job));}return {{"jobs",values}};}
        fields(params,{"id"});const auto identity=id(params.at("id"));auto found=impl_->jobs.find(identity);check(found!=impl_->jobs.end(),"Material job not retained in this session.",-32004);auto& job=found->second;
        if(method=="asset.material.cancel"){
            // No poll/publication before cancellation: even a completed CPU bake
            // can be cancelled until its owner publishes a successful result.
            if(job.state=="baking"){job.cancel->store(true);if(job.future.wait_for(std::chrono::seconds(0))==std::future_status::ready)impl_->poll(job,directory);}
            return impl_->summary(job);
        }
        if(method=="asset.material.job"){impl_->poll(job,directory);return impl_->summary(job);}
        if(method=="asset.material.forget"){check(job.state!="baking","Only terminal material jobs can be forgotten.",-32080);impl_->jobs.erase(found);return {{"id",identity},{"forgotten",true}};}
        throw Error(-32601,"Unknown material method.");
    }catch(const Error&){throw;}catch(const std::exception& error){throw Error(-32050,safe_diagnostic(error.what()));}
}
Json Service::schemas(){
    const Json hash={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    const auto number=[](double low,double high){return Json{{"type","number"},{"minimum",low},{"maximum",high}};};
    const auto count=[](unsigned low,unsigned high){return Json{{"type","integer"},{"minimum",low},{"maximum",high}};};
    Json common={{"format",{{"const","poima.material.recipe.v1"}}},{"kind",{{"enum",{"brick","plaster"}}}},{"seed",count(0,4294967295u)},
        {"width",count(64,512)},{"height",count(64,512)},{"tile_width_m",number(.1,100)},{"tile_height_m",number(.1,100)},
        {"color",{{"type","string"},{"pattern","^#[0-9a-f]{6}$"}}},{"roughness",number(.045,1)},{"roughness_variation",number(0,.5)},{"color_variation",number(0,.5)},{"grain_m",number(0,.01)}};
    auto plaster=common;plaster["kind"]={{"const","plaster"}};auto brick=common;brick["kind"]={{"const","brick"}};
    brick.update({{"rows",count(2,64)},{"columns",count(1,64)},{"joint_width_m",number(0,.2)},{"joint_depth_m",number(0,.05)},{"bevel_m",number(.000001,.2)},{"joint_color",{{"type","string"},{"pattern","^#[0-9a-f]{6}$"}}}});
    brick["rows"]["multipleOf"]=2;
    Json recipe={{"oneOf",{object(brick,{"format","kind"}),object(plaster,{"format","kind"})}}};
    recipe["description"]="Even brick rows required; joint_width_m+2*bevel_m must fit both cell dimensions. Defaults canonicalized; see PROCEDURAL_MATERIALS.md.";
    return {{"asset.material.generate",object({{"recipe",recipe}},{"recipe"})},{"asset.material.job",object({{"id",hash}},{"id"})},
        {"asset.material.cancel",object({{"id",hash}},{"id"})},{"asset.material.forget",object({{"id",hash}},{"id"})},
        {"asset.material.jobs",object(Json::object(),Json::array())},{"asset.material.inspect",object({{"recipe",hash}},{"recipe"})}};
}
}
