// SPDX-License-Identifier: Apache-2.0
#include "poima/material_recipe.hpp"
#include "material_service.hpp"
#include "poima/world.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>

namespace {
using Json=nlohmann::json;namespace pm=poima::materials;namespace fs=std::filesystem;
void check(bool v,const char* text){if(!v)throw std::runtime_error(text);}
void near(double a,double b,double epsilon=1e-11){check(std::abs(a-b)<=epsilon,"Independent numeric oracle differs.");}
template<class F>void reject(F&& f,int code=-32602){try{f();}catch(const pm::Error& e){check(e.code==code,"Wrong recipe/service rejection code.");return;}throw std::runtime_error("Malformed input accepted.");}
Json recipe(const char* kind="brick"){return {{"format","poima.material.recipe.v1"},{"kind",kind},{"width",64},{"height",64},{"seed",19}};}
bool equal(const pm::Baked& a,const pm::Baked& b){for(std::size_t i=0;i<3;++i){if(a.images[i].srgb!=b.images[i].srgb || a.images[i].mips.size()!=b.images[i].mips.size())return false;for(std::size_t j=0;j<a.images[i].mips.size();++j)if(a.images[i].mips[j].rgba!=b.images[i].mips[j].rgba)return false;}return true;}
Json terminal(pm::Service& service,const std::string& id,const fs::path& path){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);for(;;){auto value=service.dispatch("asset.material.job",{{"id",id}},path);if(value.at("state")!="baking")return value;check(std::chrono::steady_clock::now()<deadline,"Material job deadline expired.");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
}
int main(){
    fs::path scratch;
    try {
        std::vector<std::string> checks;
        auto flat=recipe("plaster");flat.update({{"color","#204080"},{"grain_m",0},{"color_variation",0},{"roughness_variation",0},{"roughness",.5}});
        const auto r=pm::parse_recipe(flat);const auto baked=pm::bake(r);
        for(std::size_t level=0;level<baked.images[0].mips.size();++level){const auto& color=baked.images[0].mips[level].rgba;const auto& normal=baked.images[1].mips[level].rgba;const auto& packed=baked.images[2].mips[level].rgba;
            for(std::size_t i=0;i<color.size();i+=4){check(color[i]==32 && color[i+1]==64 && color[i+2]==128 && color[i+3]==255,"Uniform exact sRGB oracle failed.");
                check(normal[i]==128 && normal[i+1]==128 && normal[i+2]==255 && normal[i+3]==255,"Uniform flat normal oracle failed.");
                check(packed[i]==0 && packed[i+1]==128 && packed[i+2]==0 && packed[i+3]==255,"Independent packed roughness/nonmetal oracle failed.");}}
        check(baked.images[0].srgb && !baked.images[1].srgb && !baked.images[2].srgb,"Color/data channels mixed.");checks.push_back("exact uniform color, neutral normal and packed channels across complete mips");
        const auto n=pm::height_normal(3,4);near(n[0],-3/std::sqrt(26.));near(n[1],-4/std::sqrt(26.));near(n[2],1/std::sqrt(26.));
        const auto flipped=pm::height_normal(-3,4);near(flipped[0],-n[0]);near(flipped[1],n[1]);reject([]{(void)pm::height_normal(INFINITY,0);});checks.push_back("independent physical slope normalization and tangent U/V sign");
        const auto brick=pm::parse_recipe(recipe());const auto a=pm::bake(brick),b=pm::bake(brick);check(equal(a,b),"Same seed/recipe not deterministic.");
        auto changed=brick;changed.seed++;check(!equal(a,pm::bake(changed)),"Seed did not change correlated maps.");
        for(double u:{-.5,-.001,0.,.017,.25,.999,1.2})for(double v:{-.003,0.,.001,.125,.71,1.4}){
            const auto p=pm::sample_surface(brick,u,v),q=pm::sample_surface(brick,u+2,v-3);near(p.height,q.height);near(p.roughness,q.roughness);for(unsigned c=0;c<3;++c)near(p.color[c],q.color[c]);}
        const auto l=pm::sample_surface(brick,.35,-1e-8),right=pm::sample_surface(brick,.35,1e-8);near(l.height,right.height,1e-8);near(l.roughness,right.roughness,1e-6);
        checks.push_back("same-seed equality, changed seed sensitivity and continuous wrapped surface evaluation");
        auto structure=recipe();structure.update({{"seed",0},{"grain_m",0},{"color_variation",0},{"roughness_variation",0},{"color","#ffffff"},{"joint_color","#000000"}});
        const auto structured=pm::parse_recipe(structure);const auto mortar=pm::sample_surface(structured,0,0),center=pm::sample_surface(structured,.5/structured.columns,.5/structured.rows);
        near(mortar.height,0);near(center.height,structured.joint_depth);near(mortar.color[0],0);near(center.color[0],1);checks.push_back("independent brick cell/mortar shared color-height correlation");
        auto odd=recipe("plaster");odd["width"]=65;odd["height"]=67;const auto odd_image=pm::bake(pm::parse_recipe(odd));check(odd_image.images[0].mips[1].width==32 && odd_image.images[0].mips[1].height==33 && odd_image.images[0].mips.back().width==1,"Odd mip dimensions incorrect.");
        for(const auto& image:a.images){const auto encoded=poima::encode_image(image);const auto decoded=poima::decode_image(encoded);check(decoded->mips[0].rgba==image.mips[0].rgba,"Image package roundtrip changed pixels.");
            if(!image.srgb && &image==&a.images[1])for(std::size_t i=0;i<image.mips[0].rgba.size();i+=4){double length=0;for(unsigned c=0;c<3;++c){const auto x=image.mips[0].rgba[i+c]/255.*2-1;length+=x*x;}near(length,1,.018);}}
        checks.push_back("existing image codec/mip cooking, odd sizes and quantized unit base normals");
        for(const auto& [field,value]:std::vector<std::pair<std::string,Json>>{{"seed",true},{"seed",-1},{"seed",4294967296ULL},{"width",63},{"width",513},{"height",64.5},{"rows",3},{"rows",0},{"columns",65},{"grain_m",.02},{"roughness",.01},{"roughness_variation",-.1},{"color","#FF0000"},{"tile_width_m",0},{"joint_width_m",.2},{"bevel_m",0},{"unknown",1}}){auto bad=recipe();bad[field]=value;reject([&]{(void)pm::parse_recipe(bad);});}
        auto inappropriate=recipe("plaster");inappropriate["rows"]=8;reject([&]{(void)pm::parse_recipe(inappropriate);});
        check(pm::canonical_recipe(pm::parse_recipe(pm::canonical_recipe(brick)))==pm::canonical_recipe(brick),"Canonical recipe not reusable.");
        std::atomic_bool cancelled{true};bool stopped=false;try{(void)pm::bake(brick,&cancelled);}catch(const pm::Cancelled&){stopped=true;}check(stopped,"Precancelled bake allocated/evaluated.");checks.push_back("closed versioned admission, canonical roundtrip and cancellation");
        const auto candidate=fs::temp_directory_path()/("poima-material-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));check(fs::create_directory(candidate),"Cannot create owned material test directory.");scratch=candidate;
        pm::Service service;auto submitted=service.dispatch("asset.material.generate",{{"recipe",flat}},scratch);const auto identity=submitted.at("id").get<std::string>();
        check(service.dispatch("asset.material.generate",{{"recipe",flat}},scratch).at("id")==identity,"Duplicate canonical submission lost job identity.");
        auto unpublished=flat;unpublished["seed"]=2;Json next_job;const auto worker_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for(;;){
            try{next_job=service.dispatch("asset.material.generate",{{"recipe",unpublished}},scratch);break;}
            catch(const pm::Error& error){check(error.code==-32080,"Unexpected worker admission error.");}
            check(std::chrono::steady_clock::now()<worker_deadline,"CPU-ready worker deadline expired.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(fs::is_empty(scratch),"A completed native worker performed package I/O before owner publication.");
        const auto unpublished_id=next_job.at("id").get<std::string>();
        (void)service.dispatch("asset.material.cancel",{{"id",unpublished_id}},scratch);
        check(terminal(service,unpublished_id,scratch).at("state")=="cancelled" && fs::is_empty(scratch),"Cancelled unpublished worker performed package I/O.");
        (void)service.dispatch("asset.material.forget",{{"id",unpublished_id}},scratch);
        checks.push_back("completed native worker and cancelled successor perform no package I/O before owner publication");
        auto done=terminal(service,identity,scratch);check(done.at("state")=="succeeded","Generated material failed publication.");
        check(service.dispatch("asset.material.inspect",{{"recipe",identity}},scratch)==done.at("result"),"Persisted manifest differs from terminal result.");
        const auto manifest_path=scratch/(identity+".pmaterial");const auto before=fs::last_write_time(manifest_path);(void)service.dispatch("asset.material.forget",{{"id",identity}},scratch);
        auto again=service.dispatch("asset.material.generate",{{"recipe",flat}},scratch);check(terminal(service,again.at("id"),scratch).at("state")=="succeeded" && fs::last_write_time(manifest_path)==before,"Rebuild overwrote immutable manifest.");
        auto cancellation_recipe=recipe();cancellation_recipe["width"]=512;cancellation_recipe["height"]=512;auto cancel_job=service.dispatch("asset.material.generate",{{"recipe",cancellation_recipe}},scratch);const auto cancel_id=cancel_job.at("id").get<std::string>();(void)service.dispatch("asset.material.cancel",{{"id",cancel_id}},scratch);
        check(terminal(service,cancel_id,scratch).at("state")=="cancelled" && !fs::exists(scratch/(cancel_id+".pmaterial")),"Cancelled bake published a successful descriptor.");
        reject([&]{(void)service.dispatch("asset.material.job",{{"id",std::string(64,'f')}},scratch);},-32004);
        checks.push_back("bounded native worker dedup, owner publication, persisted reload, immutable reuse and cancelled-no-success");
        auto failing_recipe=recipe();failing_recipe["seed"]=45;const auto failing_id=pm::recipe_id(pm::parse_recipe(failing_recipe));
        const auto occupied=scratch/(failing_id+".material-pending");{std::ofstream sentinel(occupied,std::ios::binary);sentinel<<"keep";}
        const auto failing=service.dispatch("asset.material.generate",{{"recipe",failing_recipe}},scratch);
        const auto failed=terminal(service,failing.at("id"),scratch);check(failed.at("state")=="failed" && failed.at("result").is_null() && !fs::exists(scratch/(failing_id+".pmaterial")),"Failed publication reported a complete material.");
        {std::ifstream sentinel(occupied,std::ios::binary);std::string retained{std::istreambuf_iterator<char>(sentinel),{}};check(retained=="keep","Publication truncated a pre-existing staging entry.");}
        auto abandoned_recipe=recipe();abandoned_recipe["seed"]=46;abandoned_recipe["width"]=512;abandoned_recipe["height"]=512;
        const auto abandoned_id=pm::recipe_id(pm::parse_recipe(abandoned_recipe));
        {pm::Service abandoned;(void)abandoned.dispatch("asset.material.generate",{{"recipe",abandoned_recipe}},scratch);}
        check(!fs::exists(scratch/(abandoned_id+".pmaterial")),"Destruction published unfinished bake.");
        checks.push_back("occupied staging fails without successful descriptor or truncation; destruction cancels and joins unpublished work");
#ifdef _WIN32
        constexpr unsigned diagnostic_variants=1; // Native Windows paths are UTF-16, not malformed POSIX bytes.
#else
        constexpr unsigned diagnostic_variants=2;
#endif
        for(unsigned variant=0;variant<diagnostic_variants;++variant){
            std::string component;
            if(variant==0){for(unsigned k=0;k<400;++k)component+="\xe2\x98\x83";}
            else{component=std::string(300,'x');component[80]=static_cast<char>(0xff);}
            const auto invalid_directory=scratch/(variant==0 ? fs::path(std::u8string(component.begin(),component.end())) : fs::path(component));pm::Service diagnostic_service;
            auto diagnostic_recipe=flat;diagnostic_recipe["seed"]=100+variant;
            const auto diagnostic_job=diagnostic_service.dispatch("asset.material.generate",{{"recipe",diagnostic_recipe}},invalid_directory);
            const auto failed_diagnostic=terminal(diagnostic_service,diagnostic_job.at("id"),invalid_directory);
            check(failed_diagnostic.at("state")=="failed","Invalid filesystem output unexpectedly succeeded.");
            const auto diagnostic=failed_diagnostic.at("diagnostic").get<std::string>();check(diagnostic.size()<=1024,"Failure diagnostic exceeds byte bound.");
            check(Json::parse(failed_diagnostic.dump())==failed_diagnostic,"Failure diagnostic JSON roundtrip changed.");
            bool inspection_rejected=false;try{(void)diagnostic_service.dispatch("asset.material.inspect",{{"recipe",identity}},invalid_directory);}
            catch(const pm::Error& error){inspection_rejected=true;check(error.code==-32050,"Stored resource failure has wrong code.");check(std::string(error.what()).size()<=1024,"Inspection diagnostic exceeds byte bound.");(void)Json(error.what()).dump();}
            check(inspection_rejected,"Invalid output inspection unexpectedly succeeded.");
        }
        checks.push_back("native-platform oversized Unicode and POSIX malformed-byte filesystem failures remain bounded valid UTF-8 job and inspection diagnostics");
        const auto world_path=scratch/"world.json";
        auto utf8=[](const fs::path& path){const auto text=path.u8string();return std::string(text.begin(),text.end());};
        auto request=[](poima::WorldSession& session,const char* method,Json params,poima::WorldRequestScope scope){
            return Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump(),scope));
        };
        const std::string entity(32,'1');
        {poima::WorldSession world(utf8(world_path));auto response=request(world,"world.transact",{{"request_id",std::string(32,'2')},{"base_revision",0},
            {"ops",Json::array({Json{{"op","entity.create"},{"id",entity},{"name","Recipe surface"},{"parent",nullptr}},
                Json{{"op","component.set"},{"id",entity},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"albedo",{1,1,1}},{"visible",true}}}}})}},poima::WorldRequestScope::standalone);
            check(response.contains("result"),"Native authored fixture failed.");}
        const auto assets=fs::path(world_path).concat(".assets");fs::create_directory(assets);
        for(const auto& entry:fs::directory_iterator(scratch))if(entry.path().extension()==".pimage" || entry.path().extension()==".pmaterial")fs::copy_file(entry.path(),assets/entry.path().filename());
        {poima::WorldSession world(utf8(world_path));const auto response=request(world,"world.transact",{{"request_id",std::string(32,'3')},{"base_revision",1},
            {"ops",Json::array({Json{{"op","component.set"},{"id",entity},{"type","PbrMaterial"},{"value",done.at("result").at("PbrMaterial")}},
                Json{{"op","component.set"},{"id",entity},{"type","PbrTextures"},{"value",done.at("result").at("PbrTextures")}}})}},poima::WorldRequestScope::standalone);
            check(response.contains("result"),"Actual generated PBR material failed authored application.");
            const auto content=world.package_content();check(content.assets.size()==3,"Material bundle closure did not contain exactly3 generated images.");
            for(const auto& asset:content.assets)check(asset.filename.ends_with(".pimage"),"Authoring recipe descriptor leaked into runtime closure.");}
        {poima::WorldSession world(utf8(world_path),poima::WorldOpenMode::read_only_runtime);
            for(const auto scope:{poima::WorldRequestScope::standalone,poima::WorldRequestScope::shared_editor,poima::WorldRequestScope::shared_headless}){
                const auto description=request(world,"world.describe",Json::object(),scope).at("result");
                check(description.at("methods").contains("asset.material.inspect"),"Read-only recipe inspection not discoverable.");
                for(const auto* method:{"asset.material.generate","asset.material.job","asset.material.jobs","asset.material.cancel","asset.material.forget"}){
                    check(!description.at("methods").contains(method),"Read-only recipe mutation discovered.");
                    const auto denied=request(world,method,Json::object(),scope);check(denied.at("error").at("code")==-32081,"Read-only recipe mutation accepted.");
                }
                check(request(world,"asset.material.inspect",{{"recipe",identity}},scope).at("result")==done.at("result"),"Read-only scope recipe inspection differs.");
            }}
        checks.push_back("native read-only/all shared scopes hide and reject generation jobs, allow verified recipe reads; runtime closure contains images only");
        const auto image_id=done.at("result").at("maps").at("base_color").at("asset").get<std::string>();{std::ofstream corrupt(scratch/(image_id+".pimage"),std::ios::binary|std::ios::app);corrupt<<'!';}
        reject([&]{(void)service.dispatch("asset.material.inspect",{{"recipe",identity}},scratch);},-32050);checks.push_back("manifest inspection verifies actual immutable image package hashes");
        fs::remove_all(scratch);std::cout<<Json{{"passed",true},{"checks",checks}}.dump()<<'\n';return 0;
    }catch(const std::exception& e){if(!scratch.empty()){std::error_code ignored;fs::remove_all(scratch,ignored);}std::cerr<<e.what()<<'\n';return 1;}
}
