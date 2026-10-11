// SPDX-License-Identifier: Apache-2.0
// Real World/Store texture contracts. The included scalar fixture provides
// identical nondefault physics/custom-state and checkpoint guard helpers only.
#define main poima_scalar_material_fixture_unused_main
#include "world_material_native.cpp"
#undef main
#include "poima/assets.hpp"
#include <array>

namespace {
const std::array<const char*,5> slots{"base_color","metallic_roughness","emissive","occlusion","normal"};
const std::string textured="779a901ab8f4448147dfc9b29e217089";
void write_bytes(const fs::path& path,const std::string& value) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);check(bool(out),"Cannot write owned input.");out.write(value.data(),std::streamsize(value.size()));check(bool(out),"Input write failed.");
}
std::string unhex(const char* value) {
    std::string out;for(std::size_t i=0;value[i];i+=2)out.push_back(char(std::stoi(std::string(value+i,2),nullptr,16)));return out;
}
const char* model_hex="676c544602000000a0080000840600004a534f4e7b226173736574223a7b2276657273696f6e223a22322e30227d2c2262756666657273223a5b7b22627974654c656e677468223a3531307d5d2c226275666665725669657773223a5b7b22627566666572223a302c22627974654c656e677468223a3132382c2262797465537472696465223a33327d2c7b22627566666572223a302c22627974654f6666736574223a3132382c22627974654c656e677468223a32347d2c7b22627566666572223a302c22627974654f6666736574223a3135322c22627974654c656e677468223a37307d2c7b22627566666572223a302c22627974654f6666736574223a3232342c22627974654c656e677468223a37307d2c7b22627566666572223a302c22627974654f6666736574223a3239362c22627974654c656e677468223a37307d2c7b22627566666572223a302c22627974654f6666736574223a3336382c22627974654c656e677468223a37307d2c7b22627566666572223a302c22627974654f6666736574223a3434302c22627974654c656e677468223a37307d5d2c226163636573736f7273223a5b7b2262756666657256696577223a302c22636f6d706f6e656e7454797065223a353132362c22636f756e74223a342c2274797065223a2256454333222c226d696e223a5b2d312c2d312c305d2c226d6178223a5b312c312c305d7d2c7b2262756666657256696577223a302c22627974654f6666736574223a31322c22636f6d706f6e656e7454797065223a353132362c22636f756e74223a342c2274797065223a2256454333227d2c7b2262756666657256696577223a302c22627974654f6666736574223a32342c22636f6d706f6e656e7454797065223a353132362c22636f756e74223a342c2274797065223a2256454332227d2c7b2262756666657256696577223a312c22636f6d706f6e656e7454797065223a353132352c22636f756e74223a362c2274797065223a225343414c4152227d5d2c226d6573686573223a5b7b227072696d697469766573223a5b7b2261747472696275746573223a7b22504f534954494f4e223a302c224e4f524d414c223a312c22544558434f4f52445f30223a327d2c22696e6469636573223a332c226d6174657269616c223a307d5d7d5d2c226d6174657269616c73223a5b7b227062724d6574616c6c6963526f7567686e657373223a7b2262617365436f6c6f72466163746f72223a5b302e352c302e352c302e352c312e305d2c226d6574616c6c6963466163746f72223a302e352c22726f7567686e657373466163746f72223a302e352c2262617365436f6c6f7254657874757265223a7b22696e646578223a307d2c226d6574616c6c6963526f7567686e65737354657874757265223a7b22696e646578223a317d7d2c22656d697373697665466163746f72223a5b302e32352c302e32352c302e32355d2c22656d69737369766554657874757265223a7b22696e646578223a327d2c226f63636c7573696f6e54657874757265223a7b22696e646578223a332c22737472656e677468223a302e357d2c226e6f726d616c54657874757265223a7b22696e646578223a342c227363616c65223a302e357d2c22646f75626c655369646564223a747275657d5d2c227465787475726573223a5b7b22736f75726365223a302c2273616d706c6572223a307d2c7b22736f75726365223a312c2273616d706c6572223a307d2c7b22736f75726365223a322c2273616d706c6572223a307d2c7b22736f75726365223a332c2273616d706c6572223a307d2c7b22736f75726365223a342c2273616d706c6572223a307d5d2c22696d61676573223a5b7b2262756666657256696577223a322c226d696d6554797065223a22696d6167652f706e67227d2c7b2262756666657256696577223a332c226d696d6554797065223a22696d6167652f706e67227d2c7b2262756666657256696577223a342c226d696d6554797065223a22696d6167652f706e67227d2c7b2262756666657256696577223a352c226d696d6554797065223a22696d6167652f706e67227d2c7b2262756666657256696577223a362c226d696d6554797065223a22696d6167652f706e67227d5d2c2273616d706c657273223a5b7b226d616746696c746572223a393732382c226d696e46696c746572223a393732387d5d2c226e6f646573223a5b7b226e616d65223a2254657874757265642071756164222c226d657368223a307d5d2c227363656e6573223a5b7b226e6f646573223a5b305d7d5d2c227363656e65223a307d200002000042494e00000080bf000080bf0000000000000000000000000000803f000000000000803f0000803f000080bf0000000000000000000000000000803f0000803f0000803f0000803f0000803f0000000000000000000000000000803f0000803f00000000000080bf0000803f0000000000000000000000000000803f000000000000000000000000010000000200000000000000020000000300000089504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6338e1a0f01f0005240228672030be0000000049454e44ae426082000089504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c63f87fc0e13f0007c002ff177362c40000000049454e44ae426082000089504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c63706850f80f0003c401e0ddf20f750000000049454e44ae426082000089504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6338c0c0f01f00040401c0274c1ee90000000049454e44ae426082000089504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c636868f8ff1f00068202ffb3be51330000000049454e44ae4260820000";
const char* image_hex="89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c637038d0f01f00054402806e63f1e20000000049454e44ae426082";
class TextureFixture : public Fixture {
public:
    fs::path image_source;
    std::string model,colour,linear;
    explicit TextureFixture(const fs::path& directory):Fixture(directory),image_source(directory/"new-image.png") {
        call(world,"runtime.stop",{{"session_id",session}});
        const auto source=directory/"inherited.glb";write_bytes(source,unhex(model_hex));
        model=call(world,"asset.import",{{"source",path_text(source)}}).at("asset");
        transact(Json::array({{{"op","asset.instantiate"},{"id",id(200)},{"asset",model},{"name","Inherited texture quad"}},
            {{"op","component.remove"},{"id",textured},{"type","PbrMaterial"}}}));
        // The inherited PBR path must work without an explicit override. All
        // original geometry and texture slots are owned by the source model.
        session=next();call(world,"runtime.start",{{"session_id",session},{"revision",authored_revision()}});step(7);
        const auto components=call(world,"runtime.components",{{"session_id",session}});
        call(world,"runtime.component.edit",{{"session_id",session},{"request_id",next()},{"expected_tick",tick()},
            {"expected_revision",components.at("component_revision")},{"id",id(1)},{"type",type},{"values",{{field,37}}}});
        write_bytes(image_source,unhex(image_hex));
        colour=call(world,"asset.image.import",{{"source",path_text(image_source)},{"color_space","srgb"}}).at("asset");
        linear=call(world,"asset.image.import",{{"source",path_text(image_source)},{"color_space","linear"}}).at("asset");
    }
    Json texture_inspect() {return call(world,"runtime.texture.inspect",{{"session_id",session},{"tick",tick()},{"ids",{textured}}});}
    Json texture_apply() {auto value=apply();value["ids"]={textured};return value;}
    Json intent() const {return {{"base_color",{{"asset",colour}}},{"metallic_roughness",{{"asset",linear}}},
        {"emissive",{{"asset",colour}}},{"occlusion",{{"asset",linear}}},{"normal",{{"asset",linear}}},
        {"occlusion_strength",.25},{"normal_scale",.75}};}
    void texture_edit(const Json& value) {edit(textured,"PbrTextures",value);}
    void texture_reject(const Json& params,int code) {
        const auto before=texture_inspect();reject("runtime.texture.apply",params,code);
        check(texture_inspect().dump()==before.dump(),"Rejected texture adoption changed exact texture metadata.");
    }
};
poima::MaterialTextures effective(const poima::SceneObject& value) {
    if(value.textures)return *value.textures;
    poima::MaterialTextures out;out.maps=value.mesh->textures;out.normal_scale=value.mesh->normal_scale;
    out.occlusion_strength=value.mesh->occlusion_strength;return out;
}
void same_geometry(const poima::SceneSnapshot& a,const poima::SceneSnapshot& b) {
    check(a.presentation_source_id==b.presentation_source_id && a.world_id==b.world_id && a.objects.size()==b.objects.size(),"Texture adoption replaced runtime identity/membership.");
    for(const auto& old:a.objects) {
        const auto& next=object(b,old.entity_id);
        check(old.mesh==next.mesh && bool(old.skin)==bool(next.skin) && (!old.skin || old.skin->palette==next.skin->palette) && old.incarnation==next.incarnation && old.world==next.world &&
            old.albedo==next.albedo && old.cast_shadows==next.cast_shadows && old.receive_shadows==next.receive_shadows &&
            old.material.has_value()==next.material.has_value() && (!old.material || material(*old.material).dump()==material(*next.material).dump()),
            "Texture update changed old geometry, scalar PBR, pose or incarnation.");
    }
}
void adoption_and_restore(const fs::path& directory) {
    TextureFixture f(directory);const auto old=f.world.runtime_snapshot(poima::EditorCamera{});
    const auto original=f.checkpoint("original");const auto original_bytes=poima::saves::Store(f.storage/"slot-original").read().bytes;
    const auto original_texture=f.texture_inspect();
    check(original_texture.at("textures").at(0).at("authored").is_null(),"Inherited baseline unexpectedly has override.");
    const auto inherited=effective(object(old,textured));
    for(const auto& map:inherited.maps)check(bool(map.image),"Original model does not exercise all five inherited slots.");
    check(inherited.normal_scale==.5f && inherited.occlusion_strength==.5f,"Original strength inheritance missing.");
    f.texture_edit(f.intent());const auto authored=read(f.path);const auto history=call(f.world,"world.history");
    check(f.texture_inspect().at("textures").at(0).at("authored").is_null(),"Authoring implicitly replaced runtime textures.");
    const auto params=f.texture_apply();const auto accepted=call(f.world,"runtime.texture.apply",params);
    check(accepted.at("material_revision")==1 && accepted.at("tick")==7 && !accepted.at("replayed").get<bool>(),"Texture adoption receipt mismatch.");
    const auto changed_scene=f.world.runtime_snapshot(poima::EditorCamera{});same_geometry(old,changed_scene);
    const auto& before=object(old,textured);const auto& after=object(changed_scene,textured);
    check(!before.textures && after.textures && effective(before).maps[0].image==inherited.maps[0].image,
        "Texture adoption mutated old immutable snapshot.");
    check(after.textures->normal_scale==.75f && after.textures->occlusion_strength==.25f,"Texture strengths were not adopted.");
    const auto inspected=f.texture_inspect();check(inspected.at("source_stale")==false && inspected.at("textures").at(0).at("authored").dump()==f.intent().dump(),"Exact authored texture intent not published.");
    for(std::size_t slot=0;slot<slots.size();++slot) {
        const auto& row=inspected.at("textures").at(0).at("effective").at("maps").at(slots[slot]);
        check(row.at("image_sha256")==((slot==0 || slot==2) ? f.colour : f.linear) &&
            row.at("color_space")==((slot==0 || slot==2) ? "srgb" : "linear") && row.at("width")==1 && row.at("height")==1,
            "Effective map identity/colorspace/dimensions differ from actual cooked image.");
    }
    check(read(f.path)==authored && call(f.world,"world.history").dump()==history.dump(),"Adoption wrote authoring/history.");
    const auto changed=f.checkpoint("changed");
    const auto changed_bytes=poima::saves::Store(f.storage/"slot-changed").read().bytes;check(payload_domain(changed.at("snapshot").at("payload")).dump()==payload_domain(original.at("snapshot").at("payload")).dump(),
        "Texture adoption changed full nondefault physics/custom/UI/game/structure snapshot domains.");
    auto sampler=f.intent();sampler["base_color"]["wrap_s"]=33071;sampler["base_color"]["wrap_t"]=33648;
    sampler["base_color"]["min_filter"]=9728;sampler["base_color"]["mag_filter"]=9728;
    f.texture_edit(sampler);call(f.world,"runtime.texture.apply",f.texture_apply());
    const auto sample_scene=f.world.runtime_snapshot(poima::EditorCamera{});same_geometry(changed_scene,sample_scene);
    const auto& sampled=object(sample_scene,textured);check(sampled.textures!=after.textures && sampled.textures->maps[0].image==after.textures->maps[0].image &&
        sampled.textures->maps[0].wrap_s==33071 && sampled.textures->maps[0].wrap_t==33648 && sampled.textures->maps[0].min_filter==9728 && sampled.textures->maps[0].mag_filter==9728,
        "Sampler-only adoption lost immutable image ownership or sampler intent.");
    check(after.textures->maps[0].wrap_s==10497 && after.textures->maps[0].min_filter==9987,"Sampler update mutated old snapshot block.");
    // Hidden original drawables are eligible without affecting visible membership.
    f.edit(id(3),"PbrTextures",{{"base_color",{{"asset",f.colour}}}});
    auto hidden=f.apply();hidden["ids"]={id(3)};call(f.world,"runtime.texture.apply",hidden);
    check(f.world.runtime_snapshot(poima::EditorCamera{}).objects.size()==sample_scene.objects.size(),"Hidden texture selection changed visibility.");
    Json cleared=Json::object();for(const auto* slot:slots)cleared[slot]=nullptr;
    f.texture_edit(cleared);call(f.world,"runtime.texture.apply",f.texture_apply());
    auto live=f.world.runtime_snapshot(poima::EditorCamera{});for(const auto& map:effective(object(live,textured)).maps)check(!map.image,"Explicit null failed to clear original map.");
    f.texture_edit(Json::object());call(f.world,"runtime.texture.apply",f.texture_apply());
    live=f.world.runtime_snapshot(poima::EditorCamera{});const auto inherited_again=effective(object(live,textured));
    for(std::size_t slot=0;slot<slots.size();++slot)check(inherited_again.maps[slot].image==inherited.maps[slot].image,"Omitted slot failed to inherit original immutable model map.");
    check(inherited_again.normal_scale==.5f && inherited_again.occlusion_strength==.5f,"Omitted strengths did not inherit source model.");
    f.transact(Json::array({{{"op","component.remove"},{"id",textured},{"type","PbrTextures"}}}));
    call(f.world,"runtime.texture.apply",f.texture_apply());live=f.world.runtime_snapshot(poima::EditorCamera{});
    check(!object(live,textured).textures && f.texture_inspect().at("textures").at(0).at("authored").is_null(),"Component removal did not restore inherited model owner.");
    f.step();const auto settled=f.state();const auto replay=call(f.world,"runtime.texture.apply",params);
    check(replay.at("replayed")==true && replay.at("material_revision")==1 && replay.at("tick")==7 && f.state().dump()==settled.dump(),"Old receipt retry reapplied maps/current guards.");
    auto conflict=params;conflict["expected_tick"]=8;f.texture_reject(conflict,-32010);
    // All imports and original source files may disappear; cooked saved assets
    // and immutable runtime owners must suffice for exact restore.
    fs::remove(f.image_source);fs::remove(directory/"inherited.glb");
    auto load=f.load_params("original");auto result=call(f.world,"save.load",load);f.session=load.at("new_session_id");
    check(result.at("session_id")==f.session && f.status().at("material_revision")==0 && f.texture_inspect().at("textures").at(0).at("authored").is_null(),"Old baseline restore failed after source deletion.");
    check(f.checkpoint("original-restored").at("snapshot").dump()==original.at("snapshot").dump(),"Old saved snapshot changed after restore.");
    load=f.load_params("changed",false);call(f.world,"save.load",load);f.session=load.at("new_session_id");
    check(f.texture_inspect().at("textures").at(0).at("authored").dump()==f.intent().dump() && f.status().at("material_revision")==0,
        "New baseline did not restore maps with reset session epoch.");
    check(f.checkpoint("changed-restored").at("snapshot").dump()==changed.at("snapshot").dump(),"New saved snapshot changed after restore.");
    check(poima::saves::Store(f.storage/"slot-original").read().bytes==original_bytes,"Texture adoption overwrote old durable slot.");
    // A WorldSession owns its authored path lock until destruction. Give the
    // fresh owner a distinct copied document and cooked-asset directory; shared
    // durable storage is intentionally retained for the exact selected restore.
    const auto fresh_directory=directory/"fresh-owner";check(fs::create_directory(fresh_directory),"Cannot reserve fresh owner directory.");
    const auto fresh_path=fresh_directory/"world.json";check(fs::copy_file(f.path,fresh_path),"Cannot copy fresh authored document.");
    auto source_assets=f.path;source_assets+=".assets";auto fresh_assets=fresh_path;fresh_assets+=".assets";
    fs::copy(source_assets,fresh_assets,fs::copy_options::recursive);
    check(read(fresh_path)==read(f.path),"Fresh copied document changed authored bytes.");
    poima::WorldSession fresh(path_text(fresh_path));call(fresh,"save.configure",{{"request_id",id(9000)},{"expected_generation",0},{"root",path_text(f.storage)}});
    call(fresh,"save.load",{{"request_id",id(9001)},{"configuration_generation",1},{"slot","changed"},{"expected_generation",1},
        {"revision",f.authored_revision()},{"expected_session_id",nullptr},{"expected_tick",nullptr},{"expected_gameplay_revision",nullptr},
        {"expected_material_revision",nullptr},{"new_session_id",id(9002)}});
    const auto fresh_state=call(fresh,"runtime.inspect",{{"session_id",id(9002)}});
    const auto fresh_components=call(fresh,"runtime.components",{{"session_id",id(9002)}});
    Json written={{"request_id",id(9003)},{"configuration_generation",1},{"slot","fresh"},{"expected_generation",0},
        {"session_id",id(9002)},{"expected_tick",fresh_state.at("tick")},{"expected_gameplay_revision",0},
        {"expected_component_revision",fresh_components.at("component_revision")},{"expected_structure_revision",fresh_state.at("structure_revision")},
        {"expected_ui_revision",fresh_state.at("ui_revision")},{"expected_control_sequence",fresh_state.at("control_sequence")},
        {"expected_material_revision",fresh_state.at("material_revision")}};
    call(fresh,"save.write",written);check(Json::parse(poima::saves::Store(f.storage/"slot-fresh").read().bytes).at("snapshot").dump()==changed.at("snapshot").dump(),"Fresh native owner failed exact source-free restore.");
    check(poima::saves::Store(f.storage/"slot-original").read().bytes==original_bytes &&
        poima::saves::Store(f.storage/"slot-changed").read().bytes==changed_bytes,
        "Fresh restore mutated either retained durable baseline slot.");
}
void rejection_and_assets(const fs::path& directory) {
    TextureFixture f(directory);f.texture_edit(f.intent());const auto valid=f.texture_apply();
    for(const auto* key:{"expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision"}) {
        auto wrong=valid;wrong[key]=wrong.at(key).get<std::uint64_t>()+1;f.texture_reject(wrong,-32009);
        wrong=valid;wrong[key]=wrong.at(key).get<double>();f.texture_reject(wrong,-32602);
        wrong=valid;wrong[key]=true;f.texture_reject(wrong,-32602);
    }
    for(const auto& selection:{Json::array(),Json::array({textured,textured}),Json::array({textured,id(1)}),Json::array({7})}) {
        auto wrong=valid;wrong["ids"]=selection;f.texture_reject(wrong,-32602);
    }
    auto wrong=valid;wrong["ids"]={id(999)};f.texture_reject(wrong,-32004);
    wrong=valid;wrong["ids"]={id(4)};f.texture_reject(wrong,-32004);
    wrong=valid;wrong["expected_content_sha256"]=std::string(64,'0');f.texture_reject(wrong,-32009);
    const auto forbidden=[&](const Json& operation) {
        f.transact(Json::array({operation}));f.texture_reject(f.texture_apply(),-32602);
        call(f.world,"world.undo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    };
    forbidden({{"op","component.set"},{"id",textured},{"type","PbrMaterial"},{"value",pbr(1)}});
    forbidden({{"op","component.set"},{"id",id(1)},{"type","Transform"},{"value",transform(12)}});
    forbidden({{"op","entity.rename"},{"id",id(1)},{"name","Unrelated"}});
    forbidden({{"op","component.set"},{"id",id(2)},{"type","PbrTextures"},{"value",{{"base_color",{{"asset",f.colour}}}}}});
    auto wrong_space=f.intent();wrong_space["normal"]["asset"]=f.colour;
    f.texture_edit(wrong_space);f.texture_reject(f.texture_apply(),-32050);
    call(f.world,"world.undo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    const auto cooked=fs::path(path_text(f.path)+".assets")/(f.model+".pmodel");const auto original=read(cooked);
    auto damaged=original;damaged.at(damaged.size()-1)^=1;write_bytes(cooked,damaged);
    const auto scene=f.world.runtime_snapshot(poima::EditorCamera{});const auto state=f.state();
    const auto failed=request(f.world,"runtime.texture.apply",f.texture_apply());check(failed.contains("error"),"Corrupt original cooked asset was accepted.");
    check(f.state().dump()==state.dump(),"Corrupt original asset changed committed state.");unchanged_scene(scene,f.world.runtime_snapshot(poima::EditorCamera{}));
    write_bytes(cooked,original);
    const auto accepted_params=f.texture_apply();call(f.world,"runtime.texture.apply",accepted_params);
    wrong=f.apply();wrong["request_id"]=accepted_params.at("request_id");f.reject("runtime.material.apply",wrong,-32010);
    f.reject("runtime.step",{{"session_id",f.session},{"request_id",accepted_params.at("request_id")},{"expected_tick",f.tick()},{"ticks",1}},-32010);
    f.reject("save.write",f.write_params("missing-material",false),-32602);
    wrong=f.write_params("stale-material");wrong["expected_material_revision"]=0;f.reject("save.write",wrong,-32009);
    f.reject("save.load",f.load_params("missing-slot",false),-32602);
    // Scalar and texture adoption share one monotonically increasing epoch.
    f.edit_materials(1);call(f.world,"runtime.material.apply",f.apply());check(f.status().at("material_revision")==2,"Scalar/texture epoch was split.");
    auto changed=f.intent();changed["normal_scale"]=.5;f.texture_edit(changed);const auto step=f.step_params();call(f.world,"runtime.step",step);
    wrong=f.texture_apply();wrong["request_id"]=step.at("request_id");f.texture_reject(wrong,-32010);
    call(f.world,"runtime.texture.apply",f.texture_apply());check(f.status().at("material_revision")==3,"Texture after scalar adoption reset shared epoch.");
}
std::string provenance_record(TextureFixture& f,const std::string& asset,const char* kind,const char* title) {
    return call(f.world,"asset.provenance.create",{{"record",{{"format","poima.asset-provenance"},{"version",1},
        {"asset",asset},{"kind",kind},{"title",title},{"creator","Poima original fixture"},
        {"license",{{"identifier","Apache-2.0"},{"notice","Original test bytes under repository license."}}}}}}).at("record");
}
void dependency_provenance(const fs::path& directory) {
    TextureFixture f(directory);const auto original=f.checkpoint("source");
    const auto record=provenance_record(f,f.colour,"image","Selected new texture");
    const auto binding=[&](const std::string& asset,const Json& rows){return Json{{"op","asset.provenance.set"},{"asset",asset},{"records",rows}};};
    const Json value={{"base_color",{{"asset",f.colour}}}};
    f.transact(Json::array({{{"op","component.set"},{"id",textured},{"type","PbrTextures"},{"value",value}},binding(f.colour,Json::array({record}))}));
    call(f.world,"runtime.texture.apply",f.texture_apply());const auto adopted=f.checkpoint("introduced");
    check(adopted.at("document").at("asset_provenance").at(f.colour)==Json::array({record}) &&
        payload_domain(adopted.at("snapshot").at("payload")).dump()==payload_domain(original.at("snapshot").at("payload")).dump(),
        "Selected new texture/provenance admission did not preserve complete saved state.");
    // A real texture change does not authorize rewriting retained bindings,
    // removing an active binding, or adding unrelated/inactive declarations.
    auto changed=value;changed["normal_scale"]=.5;f.texture_edit(changed);
    const auto rewritten=provenance_record(f,f.colour,"image","Changed existing declaration");
    const auto unrelated=provenance_record(f,f.model,"model","Existing geometry declaration");
    const auto inactive=provenance_record(f,f.linear,"image","Unselected image declaration");
    for(const auto& operation:{binding(f.colour,Json::array({rewritten})),binding(f.colour,Json::array()),
                               binding(f.model,Json::array({unrelated})),binding(f.linear,Json::array({inactive}))}) {
        f.transact(Json::array({operation}));f.texture_reject(f.texture_apply(),-32602);
        call(f.world,"world.undo",{{"request_id",f.next()},{"base_revision",f.authored_revision()}});
    }
    call(f.world,"runtime.texture.apply",f.texture_apply());
    f.transact(Json::array({{{"op","component.remove"},{"id",textured},{"type","PbrTextures"}},binding(f.colour,Json::array())}));
    call(f.world,"runtime.texture.apply",f.texture_apply());const auto retired=f.checkpoint("retired");
    check(!retired.at("document").contains("asset_provenance") && f.texture_inspect().at("textures").at(0).at("authored").is_null() &&
        payload_domain(retired.at("snapshot").at("payload")).dump()==payload_domain(original.at("snapshot").at("payload")).dump(),
        "Fully retired selected texture/provenance did not restore original geometry/default state.");
}

}
int main() {
    const auto directory=fs::temp_directory_path()/("poima-world-texture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(poima::Runtime::available(),"World texture contracts require simulation.");check(fs::create_directory(directory),"Cannot reserve output.");
        fs::create_directory(directory/"durable");fs::create_directory(directory/"rejections");fs::create_directory(directory/"provenance");
        adoption_and_restore(directory/"durable");rejection_and_assets(directory/"rejections");dependency_provenance(directory/"provenance");fs::remove_all(directory);
        std::cout<<"Texture maps, inherited/null/removed intent, immutable owners, complete CPU/save state, guards and asset corruption passed.\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<"\nFixture retained: "<<directory<<'\n';return 1;}
}
