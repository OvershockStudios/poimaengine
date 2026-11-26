// SPDX-License-Identifier: Apache-2.0
#include "world_asset_commands.hpp"

#include "asset_references.hpp"
#include "asset_store.hpp"
#include "physical_pose_diagnostics.hpp"
#include "poima/animation.hpp"
#include "world_model_assets.hpp"
#include "world_runtime_definition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace poima::world_detail {

Json inspect_asset_references(const Json& params,const Json& document) {
    auto parse_owner=[](const Json& value) {
        fields(value,{"kind","id"},{"kind","id"});
        require(value.at("kind").is_string() && value.at("id").is_string(),"Asset-reference owner fields must be text.");
        asset_references::Owner owner{value.at("kind").get<std::string>(),value.at("id").get<std::string>()};
        require(asset_references::valid_owner(owner),"Invalid asset-reference owner selector.");return owner;
    };
    asset_references::Query query;
    if(params.contains("asset")) {
        require(params.at("asset").is_string(),"Asset-reference asset selector must be text.");query.asset=params.at("asset").get<std::string>();
        require(asset_references::valid_asset(*query.asset),"Invalid asset-reference asset selector.");
    }
    if(params.contains("owner"))query.owner=parse_owner(params.at("owner"));
    require(!(query.asset && query.owner),"Asset and owner selectors are mutually exclusive.");
    if(params.contains("after")) {
        require(params.contains("revision"),"Asset-reference continuation requires revision.");
        const auto& after=params.at("after");fields(after,{"owner","component","path"},{"owner","component","path"});
        require(after.at("component").is_string() && after.at("path").is_string(),"Asset-reference cursor fields must be text.");
        query.after=asset_references::Cursor{parse_owner(after.at("owner")),after.at("component").get<std::string>(),after.at("path").get<std::string>()};
        require(asset_references::valid_cursor(*query.after),"Invalid asset-reference cursor.");
    }
    const auto limit=params.contains("limit") ? revision(params.at("limit")):64;
    require(limit>=1 && limit<=asset_references::max_page,"Asset-reference page limit must be 1..256.");query.limit=static_cast<std::uint32_t>(limit);
    auto owner_json=[](const asset_references::Owner& owner) { return Json{{"kind",owner.kind},{"id",owner.id}}; };
    auto cursor_json=[&](const asset_references::Cursor& cursor) { return Json{{"owner",owner_json(cursor.owner)},{"component",cursor.component},{"path",cursor.path}}; };
    asset_references::Page page;
    try {
        page=asset_references::collect(document.at("entities"),document.contains("templates") ? &document.at("templates"):nullptr,
            query,document.at("world_id").get<std::string>(),document.contains("navigation") ? &document.at("navigation"):nullptr,
            document.contains("ui") ? &document.at("ui"):nullptr,document.contains("water") ? &document.at("water"):nullptr,
            document.contains("surface_mark_profiles") ? &document.at("surface_mark_profiles"):nullptr);
    }
    catch(const asset_references::Error& error) { throw Error(error.code,error.what()); }
    Json selection=Json::object();if(query.asset)selection["asset"]=*query.asset;if(query.owner)selection["owner"]=owner_json(*query.owner);
    Json edges=Json::array();
    for(const auto& edge:page.edges) {
        auto value=cursor_json(edge.source);value["asset"]=edge.asset;value["kind"]=edge.kind;
        value["subresource"]=edge.subresource ? Json{{"kind",edge.subresource->kind},{"index",edge.subresource->index}}:Json(nullptr);
        edges.push_back(std::move(value));
    }
    Json result={{"revision",document.at("revision")},{"scope","authored"},{"selection",std::move(selection)},{"edges",std::move(edges)},
        {"next_after",page.next_after ? cursor_json(*page.next_after):Json(nullptr)}};
    require(result.dump().size()<=asset_references::max_result_bytes,"Asset-reference result exceeds 1 MiB.");return result;
}

Json inspect_entity_material(const Json& components,const Json& entity_id,const Json& current_revision,
    const std::filesystem::path& assets,ModelCache& cache) {
    const auto mesh=resolve_mesh_component(components,assets,cache);
    require(mesh.has_value(),"Entity has no renderable mesh.",-32004);
    MaterialTextures textures;
    if(mesh->textures)textures=*mesh->textures;
    else if(mesh->mesh) { textures.maps=mesh->mesh->textures;textures.normal_scale=mesh->mesh->normal_scale;textures.occlusion_strength=mesh->mesh->occlusion_strength; }
    Json maps=Json::object();const char* names[]={"base_color","metallic_roughness","emissive","occlusion","normal"};
    for(std::size_t slot=0;slot<5;++slot) {
        const auto& map=textures.maps[slot];maps[names[slot]]=nullptr;if(!map.image)continue;
        Json ref;
        for(const auto& [id,image]:cache.images)if(image.image==map.image)ref={{"asset",id}};
        if(ref.is_null())for(const auto& [id,model]:cache.models) {
            const auto found=std::find(model.model->images.begin(),model.model->images.end(),map.image);
            if(found!=model.model->images.end())ref={{"asset",id},{"image",std::size_t(found-model.model->images.begin())}};
        }
        ref["wrap_s"]=map.wrap_s;ref["wrap_t"]=map.wrap_t;ref["min_filter"]=map.min_filter;ref["mag_filter"]=map.mag_filter;
        if(map.image->normal_variance)ref["alpha_channel"]="mean_normal_length";
        maps[names[slot]]=ref;
    }
    return {{"revision",current_revision},{"id",entity_id},{"material",mesh->material ? material_json(*mesh->material) : Json(nullptr)},
        {"legacy_albedo",mesh->albedo},{"textures",maps},{"normal_scale",textures.normal_scale},{"occlusion_strength",textures.occlusion_strength}};
}

Json inspect_animation_physical_limits(const Json& params,const Json& document,
    const std::filesystem::path& assets) {
    const auto id=identifier(params.at("entity"));
    require(document.at("entities").contains(id),"Entity does not exist.",-32004);
    const auto& components=document.at("entities").at(id).at("components");
    require(components.contains("PhysicalRig") && components.contains("AnimationRig"),
        "Select an authored AnimationRig with a PhysicalRig.");
    require(params.at("time").is_number(),"Sample time must be a number.");
    const double time=params.at("time");
    require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
    require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");
    const bool loop=params.value("loop",false);
    std::optional<std::uint32_t> clip;
    if(!params.at("clip").is_null()) {
        const auto index=revision(params.at("clip"));
        require(index<max_model_clips,"Clip index is out of range.");
        clip=static_cast<std::uint32_t>(index);
    } else require(time==0 && !loop,"A rest pose requires time zero and loop false.");
    try {
        const auto loaded=read_model_asset(assets,components.at("AnimationRig").at("asset"));
        if(clip)require(*clip<loaded.model->animations.size(),"Clip index is out of range.");
        auto report=physical_pose_diagnostics(*loaded.model,components.at("PhysicalRig"),clip,time,loop);
        report["asset"]=loaded.id;
        report["entity"]=id;
        report["revision"]=document.at("revision");
        return report;
    }catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
}

Json inspect_animation_asset(const std::string& method,const Json& params,
    const std::filesystem::path& assets) {
    if(method=="asset.animation.bounds") {
        fields(params,{"asset","clip","time","loop"},{"asset","time"});
        require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
        require(params.at("time").is_number(),"Sample time must be a number.");
        const double time=params.at("time");
        require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
        require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");
        std::optional<std::uint64_t> selected;
        if(params.contains("clip")) {
            selected=revision(params.at("clip"));
            require(*selected<max_model_clips,"Clip index is out of range.");
        }
        try {
            const auto loaded=read_model_asset(assets,params.at("asset"));
            if(selected)require(*selected<loaded.model->animations.size(),"Clip index is out of range.");
            const auto clip=selected ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*selected)) : std::nullopt;
            const auto sampled=sample_model_bounds(*loaded.model,clip,time,params.value("loop",false));
            Json bounds=nullptr;
            if(sampled.bounds)bounds={{"min",sampled.bounds->minimum},{"max",sampled.bounds->maximum}};
            return {{"asset",loaded.id},{"clip",clip ? Json(*clip) : Json(nullptr)},
                {"requested_time",time},{"sample_time",sampled.effective_time},{"space","model"},
                {"empty",!sampled.bounds},{"bounds",std::move(bounds)},
                {"primitive_instances",sampled.primitive_instances},{"vertex_instances",sampled.vertex_instances}};
        }catch(const Error&) { throw; }catch(const std::exception& error) { throw Error(-32050,error.what()); }
    }
    if(method=="asset.animation.channel")fields(params,{"asset","clip","channel","offset","limit"},{"asset","clip","channel"});
    else if(method=="asset.animation.skin")fields(params,{"asset","skin","offset","limit"},{"asset","skin"});
    else if(method=="asset.animation.sample")fields(params,{"asset","clip","time","loop","section","node","primitive","offset","limit"},{"asset","time"});
    else throw Error(-32601,"Unknown animation method.");
    require(params.at("asset").is_string() && valid_asset_id(params.at("asset").get<std::string>()),"Invalid asset ID.");
    const auto offset=params.contains("offset") ? revision(params.at("offset")) : 0;
    const auto limit=params.contains("limit") ? revision(params.at("limit")) : 64;
    require(limit>=1 && limit<=64,"Animation page limit must be 1..64.");
    try {
        const auto loaded=read_model_asset(assets,params.at("asset"));const auto& model=*loaded.model;
        Json out={{"asset",loaded.id},{"items",Json::array()}};std::size_t total=0;
        if(method=="asset.animation.channel") {
            const auto clip=revision(params.at("clip")),channel=revision(params.at("channel"));
            require(clip<model.animations.size() && channel<model.animations[clip].channels.size(),"Clip/channel index is out of range.");
            const auto& c=model.animations[clip].channels[channel];total=c.times.size();
            const bool cubic=c.interpolation==AnimationInterpolation::cubic;
            const char* paths[]={"translation","rotation","scale"};const char* modes[]={"STEP","LINEAR","CUBICSPLINE"};
            out["clip"]=clip;out["channel"]=channel;out["node"]=c.node;out["path"]=paths[std::size_t(c.path)];out["interpolation"]=modes[std::size_t(c.interpolation)];
            for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i) {
                Json item={{"index",i},{"time",c.times[i]},{"value",c.values[i*(cubic ? 3 : 1)+(cubic ? 1 : 0)]}};
                if(cubic) { item["in_tangent"]=c.values[i*3];item["out_tangent"]=c.values[i*3+2]; }
                out["items"].push_back(std::move(item));
            }
        } else if(method=="asset.animation.skin") {
            const auto index=revision(params.at("skin"));require(index<model.skins.size(),"Skin index is out of range.");
            const auto& skin=model.skins[index];total=skin.joints.size();out["skin"]=index;out["name"]=skin.name;out["skeleton"]=skin.skeleton;
            for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i)out["items"].push_back({{"index",i},{"node",skin.joints[i]},{"inverse_bind",skin.inverse_bind[i]}});
        } else {
            require(params.at("time").is_number(),"Sample time must be a number.");const double time=params.at("time");
            require(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be within 0..1e9 seconds.");
            require(!params.contains("loop") || params.at("loop").is_boolean(),"Loop must be boolean.");
            std::optional<std::uint32_t> clip;
            if(params.contains("clip")) { const auto index=revision(params.at("clip"));require(index<model.animations.size(),"Clip index is out of range.");clip=static_cast<std::uint32_t>(index); }
            const auto section=params.value("section",std::string("nodes"));require(section=="nodes" || section=="vertices","Invalid pose section.");
            require(section=="vertices" || (!params.contains("node") && !params.contains("primitive")),"Node/primitive selectors require the vertices section.");
            const auto pose=sample_model(model,clip,time,params.value("loop",false));out["clip"]=clip ? Json(*clip) : Json(nullptr);out["requested_time"]=time;out["sample_time"]=pose.time;out["section"]=section;
            if(section=="nodes") {
                total=model.nodes.size();
                for(auto i=offset;i<std::min<std::uint64_t>(total,offset+limit);++i)out["items"].push_back({{"index",i},{"parent",model.nodes[i].parent},{"position",pose.local[i].position},{"rotation",pose.local[i].rotation},{"scale",pose.local[i].scale},{"world",pose.world[i]}});
            } else {
                require(params.contains("node") && params.contains("primitive"),"Vertex sampling requires node and primitive indices.");
                const auto node=revision(params.at("node")),primitive=revision(params.at("primitive"));require(node<model.nodes.size(),"Node index is out of range.");
                const auto& n=model.nodes[node];require(std::find(n.primitives.begin(),n.primitives.end(),primitive)!=n.primitives.end(),"Primitive is not bound to this node.");
                const auto& mesh=*model.primitives[primitive];total=mesh.vertices.size();out["node"]=node;out["primitive"]=primitive;out["mesh_world"]=pose.world[node];
                const auto end=std::min<std::uint64_t>(total,offset+limit);MeshAsset page;
                for(auto i=offset;i<end;++i) { page.vertices.push_back(mesh.vertices[i]);if(n.skin>=0)page.influences.push_back(mesh.influences[i]); }
                std::shared_ptr<const MeshAsset> deformed;
                if(n.skin>=0 && !page.vertices.empty())deformed=deform_mesh(page,skin_palette(model,pose,static_cast<std::uint32_t>(node)));
                const auto& vertices=deformed ? deformed->vertices : page.vertices;
                for(auto i=offset;i<end;++i) {
                    const auto& v=vertices[i-offset];std::array<double,3> world{};
                    for(std::size_t row=0;row<3;++row) { world[row]=pose.world[node][12+row];for(std::size_t k=0;k<3;++k)world[row]+=pose.world[node][k*4+row]*v.position[k]; }
                    Json item={{"index",i},{"position",v.position},{"world_position",world},{"normal",v.normal},{"tangent",v.tangent},{"uv",v.uv}};
                    if(n.skin>=0) { item["joints"]=mesh.influences[i].joints;item["weights"]=mesh.influences[i].weights; }
                    out["items"].push_back(std::move(item));
                }
            }
        }
        out["total"]=total;out["next_offset"]=offset+limit<total ? Json(offset+limit) : Json(nullptr);return out;
    }catch(const Error&) { throw; }catch(const std::exception& e) { throw Error(-32050,e.what()); }
}

} // namespace poima::world_detail
