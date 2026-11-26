// SPDX-License-Identifier: Apache-2.0
#include "asset_references.hpp"
#include "surface_mark_profiles.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <string_view>
#include <tuple>

namespace poima::asset_references {
namespace {
using Json=nlohmann::json;
void need(bool condition,const char* message,int code=-32602) { if(!condition)throw Error(message,code); }
bool hex(const std::string& value,std::size_t count) noexcept {
    if(value.size()!=count)return false;
    for(const char c:value)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
    return true;
}
constexpr std::array<std::string_view,5> texture_slots{"base_color","emissive","metallic_roughness","normal","occlusion"};
constexpr std::array<std::string_view,7> component_order{"AnimationRig","AudioEmitter","MeshCollider","ParticleEmitter","PbrTextures","SkinnedMesh","StaticMesh"};
std::uint32_t index(const Json& value,std::uint32_t maximum) {
    need(value.is_number_integer(),"Asset-reference subresource index must be an integer.");
    if(value.is_number_unsigned()) {
        const auto number=value.get<std::uint64_t>();need(number<=maximum,"Asset-reference subresource index is out of range.");return static_cast<std::uint32_t>(number);
    }
    const auto number=value.get<std::int64_t>();need(number>=0 && number<=maximum,"Asset-reference subresource index is out of range.");return static_cast<std::uint32_t>(number);
}
std::string asset_id(const Json& value) {
    need(value.is_string(),"Asset-reference identity must be text.");const auto result=value.get<std::string>();
    need(valid_asset(result),"Asset-reference identity must contain 64 lowercase hexadecimal digits.");return result;
}
}
bool valid_owner(const Owner& value) noexcept { return (value.kind=="entity" || value.kind=="template" || value.kind=="world") && hex(value.id,32); }
bool valid_asset(const std::string& value) noexcept { return hex(value,64); }
bool valid_cursor(const Cursor& value) noexcept {
    if(!valid_owner(value.owner))return false;
    if(value.owner.kind=="world") {
        if(value.component=="navigation" || value.component=="water")return value.path=="/asset";
        if(value.component=="surface_mark_profiles") {
            const std::string_view path=value.path;
            return path.size()==38 && path.front()=='/' && path.ends_with("/mask") &&
                surface_mark_profiles::valid_id(path.substr(1,32));
        }
        constexpr std::string_view suffix="/style/image/asset";
        if(value.component!="ui" || value.path.size()!=4+32+suffix.size() ||
            !value.path.starts_with("/ui/") || !value.path.ends_with(suffix))return false;
        const auto local=std::string_view(value.path).substr(4,32);
        return local.find_first_not_of("0123456789abcdef")==std::string_view::npos &&
            local.find_first_not_of('0')!=std::string_view::npos;
    }
    std::string_view path=value.path;
    bool member=false;
    if(path.starts_with("/entities/")) {
        if(value.owner.kind!="template" || path.size()<10+32+12+value.component.size())return false;
        const auto local=path.substr(10,32);
        for(const char c:local)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
        if(local.find_first_not_of('0')==std::string_view::npos)return false;
        path.remove_prefix(42);
        if(!path.starts_with("/components/"))return false;
        path.remove_prefix(12);
        if(!path.starts_with(value.component))return false;
        path.remove_prefix(value.component.size());member=true;
    }
    if(value.owner.kind=="template" && !member && value.component!="StaticMesh" && value.component!="PbrTextures" && value.component!="ParticleEmitter")return false;
    if(value.component=="ParticleEmitter")return path=="/sprite/texture";
    if(value.component=="StaticMesh" && path=="/displacement/texture")return true;
    if(value.component=="StaticMesh" && path.starts_with("/lods/")) {
        return path.size()==13 && path[6]>='0' && path[6]-'0'<static_cast<int>(max_mesh_lod_levels) &&
            path.substr(7)=="/asset";
    }
    if(value.component=="PbrTextures") {
        for(const auto slot:texture_slots)
            if(path.size()==slot.size()+7 && path.front()=='/' && path.substr(1,slot.size())==slot && path.substr(slot.size()+1)=="/asset")return true;
        return false;
    }
    for(const auto component:component_order)if(value.component==component)return path=="/asset";
    return false;
}
bool before(const Cursor& a,const Cursor& b) noexcept {
    return std::tie(a.owner.kind,a.owner.id,a.component,a.path)<std::tie(b.owner.kind,b.owner.id,b.component,b.path);
}
Page collect(const Json& entities,const Json* templates,const Query& query,const std::string& world_id,const Json* navigation,const Json* ui,
    const Json* water,const Json* mark_profiles) {
    need(world_id.empty() || hex(world_id,32),"Malformed authored world identity.");
    need(!navigation || (!world_id.empty() && navigation->is_object() && navigation->size()==1 && navigation->contains("asset")),"Malformed authored navigation binding.");
    need(!water || (!world_id.empty() && water->is_object() && water->size()==1 && water->contains("asset")),"Malformed authored water binding.");
    need(entities.is_object() && entities.size()<=max_entities,"Asset-reference entity map exceeds bounds.");
    need(!templates || (templates->is_object() && templates->size()<=max_templates),"Asset-reference template map exceeds bounds.");
    need(!ui || (!world_id.empty() && ui->is_object() && ui->size()<=max_ui_elements),"Asset-reference UI map exceeds bounds.");
    if(mark_profiles) {
        need(!world_id.empty(),"Authored surface mark profiles need a world identity.");
        try { surface_mark_profiles::validate_catalog(*mark_profiles); }
        catch(const std::invalid_argument& error) { throw Error(error.what()); }
    }
    need(query.limit>=1 && query.limit<=max_page,"Asset-reference page limit must be 1..256.");
    need(!(query.asset && query.owner),"Asset and owner selectors are mutually exclusive.");
    need(!query.asset || valid_asset(*query.asset),"Invalid asset-reference asset selector.");
    need(!query.owner || valid_owner(*query.owner),"Invalid asset-reference owner selector.");
    need(!query.after || valid_cursor(*query.after),"Invalid asset-reference cursor.");
    if(query.owner) {
        if(query.owner->kind=="world")need(!world_id.empty() && query.owner->id==world_id,"Asset-reference world owner does not exist.",-32004);
        else {const auto* selected=query.owner->kind=="entity" ? &entities : templates;
            need(selected && selected->contains(query.owner->id),"Asset-reference owner does not exist.",-32004);}
    }
    Page result;result.edges.reserve(query.limit+1);std::size_t examined=0;
    auto emit=[&](const Owner& owner,std::string_view component,std::string path,const Json& reference,const char* kind,std::optional<Subresource> subresource) {
        need(++examined<=max_edges,"Authored asset-reference scan exceeds bounds.");
        Edge edge{{owner,std::string(component),std::move(path)},asset_id(reference),kind,std::move(subresource)};
        if(query.asset && edge.asset!=*query.asset)return;
        if(query.after && !before(*query.after,edge.source))return;
        result.edges.push_back(std::move(edge));
    };
    auto visit=[&](const Owner& owner,const Json& value,const std::string& prefix,std::string_view only) {
        need(valid_owner(owner),"Malformed authored asset-reference owner identity.");
        need(value.is_object() && value.contains("components") && value.at("components").is_object(),"Malformed authored asset-reference component map.");
        const auto& components=value.at("components");
        for(const auto type:component_order) {
            if(!only.empty() && type!=only)continue;
            const auto found=components.find(std::string(type));if(found==components.end())continue;
            need(owner.kind!="template" || !prefix.empty() || type=="StaticMesh" || type=="PbrTextures" || type=="ParticleEmitter","Unsupported asset-reference template component.");
            const auto& component=*found;need(component.is_object(),"Asset-reference component must be an object.");
            if(type=="ParticleEmitter") {
                const auto sprite=component.find("sprite");
                if(sprite==component.end())continue;
                need(sprite->is_object() && sprite->contains("texture"),"Malformed authored particle sprite texture reference.");
                emit(owner,type,prefix+(!prefix.empty() ? std::string(type):std::string{})+"/sprite/texture",sprite->at("texture"),"image",std::nullopt);
            } else if(type=="PbrTextures") {
                for(const auto slot:texture_slots) {
                    const auto map=component.find(std::string(slot));if(map==component.end() || map->is_null())continue;
                    need(map->is_object() && map->contains("asset"),"Malformed authored texture reference.");
                    std::optional<Subresource> subresource;
                    if(map->contains("image"))subresource=Subresource{"image",index(map->at("image"),255)};
                    emit(owner,type,prefix+(!prefix.empty() ? std::string(type):std::string{})+"/"+std::string(slot)+"/asset",map->at("asset"),subresource ? "model":"image",subresource);
                    if(result.edges.size()>query.limit)return;
                }
            } else {
                need(component.contains("asset"),"Authored asset reference is absent.");
                std::optional<Subresource> subresource;
                if(type=="StaticMesh" || type=="SkinnedMesh" || type=="MeshCollider") {
                    need(component.contains("primitive"),"Authored mesh primitive reference is absent.");
                    subresource=Subresource{"primitive",index(component.at("primitive"),9999)};
                }
                const auto base_path=prefix+(!prefix.empty() ? std::string(type):std::string{});
                emit(owner,type,base_path+"/asset",component.at("asset"),type=="AudioEmitter" ? "audio":"model",subresource);
                if(type=="StaticMesh" && component.contains("displacement") && result.edges.size()<=query.limit)
                    emit(owner,type,base_path+"/displacement/texture",component.at("displacement").at("texture"),"image",std::nullopt);
                if(type=="StaticMesh" && component.contains("lods")) {
                    const auto& lods=component.at("lods");
                    need(lods.is_array() && !lods.empty() && lods.size()<=max_mesh_lod_levels,"Malformed mesh LOD asset references.");
                    for(std::size_t level=0;level<lods.size();++level) {
                        if(result.edges.size()>query.limit)return;
                        const auto& lod=lods.at(level);
                        need(lod.is_object() && lod.contains("asset") && lod.contains("primitive"),"Malformed mesh LOD asset reference.");
                        emit(owner,type,base_path+"/lods/"+std::to_string(level)+"/asset",lod.at("asset"),"model",
                            Subresource{"primitive",index(lod.at("primitive"),9999)});
                    }
                }
            }
            if(result.edges.size()>query.limit)return;
        }
    };
    auto visit_owner=[&](const Owner& owner,const Json& value) {
        if(owner.kind!="template" || !value.contains("entities")) {visit(owner,value,"","");return;}
        const auto& members=value.at("entities");
        need(members.is_object() && !members.empty() && members.size()<=1024,"Asset-reference template member map exceeds bounds.");
        // Cursor order is owner, component, path; visit members within each
        // component to preserve it across bounded continuation pages.
        for(const auto type:component_order)for(const auto& [local,member]:members.items()) {
            need(hex(local,32) && local.find_first_not_of('0')!=std::string::npos,"Malformed template local identity.");
            visit(owner,member,"/entities/"+local+"/components/",type);
            if(result.edges.size()>query.limit)return;
        }
    };
    auto visit_world=[&](const Owner& owner) {
        if(navigation)emit(owner,"navigation","/asset",navigation->at("asset"),"navigation",std::nullopt);
        if(mark_profiles)for(const auto& [id,profile]:mark_profiles->items()) {
            if(result.edges.size()>query.limit)return;
            emit(owner,"surface_mark_profiles","/"+id+"/mask",profile.at("mask"),"image",std::nullopt);
        }
        if(ui)for(const auto& [id,element]:ui->items()) {
            if(result.edges.size()>query.limit)return;
            need(hex(id,32) && id.find_first_not_of('0')!=std::string::npos && element.is_object(),
                "Malformed authored UI element.");
            const auto style=element.find("style");
            if(style==element.end())continue;
            need(style->is_object(),"Malformed authored UI style.");
            const auto image=style->find("image");
            if(image==style->end())continue;
            need(image->is_object() && image->contains("asset"),"Malformed authored UI image reference.");
            emit(owner,"ui","/ui/"+id+"/style/image/asset",image->at("asset"),"image",std::nullopt);
        }
        if(water && result.edges.size()<=query.limit)emit(owner,"water","/asset",water->at("asset"),"water",std::nullopt);
    };
    if(query.owner) {
        if(query.owner->kind=="world")visit_world(*query.owner);
        else {const auto& owners=query.owner->kind=="entity" ? entities : *templates;visit_owner(*query.owner,owners.at(query.owner->id));}
    } else {
        // nlohmann::json object storage is a sorted std::map, not insertion order.
        for(const auto& [id,value]:entities.items()) { visit_owner({"entity",id},value);if(result.edges.size()>query.limit)break; }
        if(result.edges.size()<=query.limit && templates)
            for(const auto& [id,value]:templates->items()) { visit_owner({"template",id},value);if(result.edges.size()>query.limit)break; }
        if(result.edges.size()<=query.limit && (navigation || ui || water || mark_profiles))visit_world({"world",world_id});
    }
    if(result.edges.size()>query.limit) { result.edges.pop_back();result.next_after=result.edges.back().source; }
    return result;
}
}
